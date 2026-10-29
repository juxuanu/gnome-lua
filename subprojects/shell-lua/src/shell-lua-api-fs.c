/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.fs and shell.extension.data_dir (docs/api/fs.md): files, without
 * ever blocking the shell.
 *
 * Notes for maintainers:
 *
 *  - Every path is checked before anything touches the file system
 *    (check_path ()): absolute, canonicalised textually, then, for an
 *    extension without the "fs" capability, resolved through the file system
 *    (longest existing prefix) and compared with the two directories the
 *    extension may use. The *resolved* path is the one that is used.
 *  - The pending operations (read, write, list, remove) are owned objects of
 *    the runtime (shell_lua_own_object), like the D-Bus bus names: a
 *    shell.Task handle is alive while the work is pending, the slot is freed
 *    (and the handle dies) before the callback runs, and the destroy rule
 *    cancels the GIO operation and drops the callbacks, so that nothing runs
 *    after disable. cancel() additionally schedules on_error ("cancelled") for
 *    a later main loop iteration.
 *  - Watches are owned objects too; their callback is a ShellLuaCallback
 *    bound to the watch, so the runtime kills it with the object.
 *  - luaL_error () longjmps: nothing allocated may be live when it is called.
 *    The functions that raise either check everything first or free what they
 *    hold explicitly before raising.
 *  - Nothing here logs above message level on a path Lua can reach.
 */

#ifndef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ShellLua"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "shell-lua-api-private.h"

#define DEFAULT_MAX_BYTES (16 * 1024 * 1024)
#define MAX_MAX_BYTES (64 * 1024 * 1024)
#define MAX_WRITE_BYTES (64 * 1024 * 1024)
#define MAX_LIST_ENTRIES 100000
#define READ_CHUNK (256 * 1024)
#define LIST_BATCH 256

static void __attribute__ ((constructor))
shell_lua_api_fs_init (void)
{
  shell_lua_api_register ("fs", shell_lua_api_fs_register);
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

G_GNUC_PRINTF (3, 4)
static void
ext_log (ShellLuaState  *state,
         GLogLevelFlags  level,
         const char     *format,
         ...)
{
  g_autofree char *domain = NULL;
  g_autofree char *message = NULL;
  va_list args;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  domain = g_strdup_printf ("shell-lua/%s",
                            state != NULL ? shell_lua_state_get_id (state) : "fs");
  g_log (domain, level, "%s", message);
}

static void
release_ref (ShellLuaState *state,
             int           *ref)
{
  int r = *ref;

  *ref = LUA_NOREF;
  if (r != LUA_NOREF && r != LUA_REFNIL && state != NULL && shell_lua_state_is_alive (state))
    shell_lua_state_unref (state, r);
}

/* Reads the callback field once and leaves it on the stack; returns its
 * index. Raises unless it is absent (and not @required) or callable. The
 * reference is taken later, by ref_of (), from this copy: the options table
 * may have an __index that answers differently the second time. */
static int
push_callback_field (lua_State  *L,
                     int         tbl,
                     const char *field,
                     gboolean    required)
{
  int type = lua_getfield (L, tbl, field);

  if (type == LUA_TNIL)
    {
      if (required)
        luaL_error (L, "missing required field '%s' (a function)", field);
    }
  else if (type != LUA_TFUNCTION)
    {
      if (luaL_getmetafield (L, -1, "__call") == LUA_TNIL)
        luaL_error (L, "field '%s' must be a function, got %s", field, luaL_typename (L, -1));
      lua_pop (L, 1);
    }
  return lua_gettop (L);
}

static int
ref_of (lua_State *L,
        int        idx)
{
  if (lua_isnil (L, idx))
    return LUA_NOREF;
  lua_pushvalue (L, idx);
  return luaL_ref (L, LUA_REGISTRYINDEX);
}

/* ------------------------------------------------------------------------ */
/* Paths                                                                     */
/* ------------------------------------------------------------------------ */

static char *
data_dir_path (ShellLuaExtension *extension)
{
  g_autofree char *dir = g_build_filename (g_get_user_data_dir (), "gnome-lua",
                                           shell_lua_extension_get_uuid (extension), NULL);

  return g_canonicalize_filename (dir, NULL);
}

static gboolean
path_within (const char *path,
             const char *root)
{
  gsize n = strlen (root);

  if (strcmp (root, "/") == 0)
    return TRUE;
  return strncmp (path, root, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* @canon is absolute and canonical. Resolves the symbolic links of the longest
 * prefix that exists and appends the rest. NULL when it cannot be resolved
 * (permissions, a loop, a dangling link). */
static char *
resolve_prefix (const char *canon)
{
  char *head = g_strdup (canon);
  GString *tail = g_string_new (NULL);
  char *result = NULL;

  for (;;)
    {
      char *real = realpath (head, NULL);
      char *slash;
      struct stat st;

      if (real != NULL)
        {
          if (strcmp (real, "/") == 0)
            result = g_strdup (tail->len > 0 ? tail->str : "/");
          else
            result = g_strconcat (real, tail->str, NULL);
          free (real);
          break;
        }

      /* a link that exists but leads nowhere must not be written through */
      if ((errno != ENOENT && errno != ENOTDIR) || g_lstat (head, &st) == 0)
        break;
      if (strcmp (head, "/") == 0)
        break;

      slash = strrchr (head, '/');
      g_string_prepend (tail, slash);
      if (slash == head)
        head[1] = '\0';
      else
        *slash = '\0';
    }

  g_free (head);
  g_string_free (tail, TRUE);
  return result;
}

static gboolean
state_has_fs (lua_State         *L,
              ShellLuaExtension *extension)
{
  ShellLuaState *state = shell_lua_state_from_L (L);

  if (extension != NULL)
    return shell_lua_extension_has_capability (extension, "fs");
  return state != NULL && shell_lua_state_get_kind (state) == SHELL_LUA_STATE_PRIVILEGED;
}

/* The path to use for @path (absolute, checked by the caller), or NULL when
 * the extension may not go there. With @in_data_dir set to whether the path
 * is inside the data directory. Allocates nothing that outlives the call and
 * never raises. */
static char *
resolve_access (lua_State  *L,
                const char *path,
                gboolean    write,
                gboolean   *in_data_dir)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);
  char *canon = g_canonicalize_filename (path, NULL);
  char *data_dir = extension != NULL ? data_dir_path (extension) : NULL;
  char *result = NULL;

  *in_data_dir = data_dir != NULL && path_within (canon, data_dir);

  if (state_has_fs (L, extension))
    {
      result = g_steal_pointer (&canon);
    }
  else if (extension != NULL)
    {
      g_autofree char *resolved = resolve_prefix (canon);

      if (resolved != NULL)
        {
          g_autofree char *data_root = resolve_prefix (data_dir);
          g_autofree char *own_dir = g_file_get_path (shell_lua_extension_get_dir (extension));
          g_autofree char *own_canon = own_dir != NULL ? g_canonicalize_filename (own_dir, NULL) : NULL;
          g_autofree char *own_root = own_canon != NULL ? resolve_prefix (own_canon) : NULL;
          gboolean in_data = data_root != NULL && path_within (resolved, data_root);
          gboolean in_own = own_root != NULL && path_within (resolved, own_root);

          if (in_data || (in_own && !write))
            {
              *in_data_dir = in_data;
              result = g_steal_pointer (&resolved);
            }
        }
    }

  g_free (canon);
  g_free (data_dir);
  return result;
}

/* Validates the shape of a path argument; raises on misuse. @what names the
 * argument ("path" or "field 'path'"). The string stays valid while it is on
 * the stack. */
static const char *
check_path_string (lua_State  *L,
                   int         idx,
                   const char *what)
{
  size_t len;
  const char *path;

  if (lua_type (L, idx) != LUA_TSTRING)
    luaL_error (L, "%s must be a string, got %s", what, luaL_typename (L, idx));
  path = lua_tolstring (L, idx, &len);
  if (strlen (path) != len)
    luaL_error (L, "%s must not contain a NUL byte", what);
  if (path[0] != '/')
    luaL_error (L, "%s must be an absolute path (it starts with '/'), got '%s'", what, path);
  return path;
}

/* Pushes and checks the required field 'path' of the options table at @tbl. */
static const char *
check_path_field (lua_State *L,
                  int        tbl)
{
  if (lua_getfield (L, tbl, "path") == LUA_TNIL)
    luaL_error (L, "missing required field 'path' (a string)");
  return check_path_string (L, -1, "field 'path'");
}

/* The checked path for @function ("shell.fs.read()"), allocated. Raises the
 * capability error when the extension may not go there. */
static char *
check_path (lua_State  *L,
            const char *function,
            const char *path,
            gboolean    write,
            gboolean   *in_data_dir)
{
  gboolean in_data = FALSE;
  char *result = resolve_access (L, path, write, &in_data);

  if (in_data_dir != NULL)
    *in_data_dir = in_data;
  if (result == NULL)
    {
      shell_lua_require_capability (L, "fs", function);
      luaL_error (L, "%s requires the \"fs\" capability", function);
    }
  return result;
}

/* ------------------------------------------------------------------------ */
/* shell.extension.data_dir                                                  */
/* ------------------------------------------------------------------------ */

/* __index of the table behind shell.extension: only the first read of
 * "data_dir" gets here (the value is stored raw afterwards). */
static int
data_dir_index (lua_State *L)
{
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);
  ShellLuaState *state = shell_lua_state_from_L (L);

  if (extension != NULL && lua_type (L, 2) == LUA_TSTRING &&
      strcmp (lua_tostring (L, 2), "data_dir") == 0)
    {
      char *dir = g_build_filename (g_get_user_data_dir (), "gnome-lua",
                                    shell_lua_extension_get_uuid (extension), NULL);
      gboolean created = g_mkdir_with_parents (dir, 0700) == 0;

      if (!created)
        ext_log (state, G_LOG_LEVEL_MESSAGE, "cannot create the data directory '%s': %s",
                 dir, g_strerror (errno));

      lua_pushstring (L, dir);
      g_free (dir);
      if (created)
        {
          lua_pushvalue (L, 2);
          lua_pushvalue (L, -2);
          lua_rawset (L, 1);
        }
      return 1;
    }

  lua_pushnil (L);
  return 1;
}

/* Called by shell-lua-api-core.c on the table of shell.extension (on top of
 * the stack) before it is made read-only. */
void
shell_lua_fs_install_data_dir (lua_State *L)
{
  luaL_checkstack (L, 3, "data_dir");
  lua_newtable (L);
  lua_pushcfunction (L, data_dir_index);
  lua_setfield (L, -2, "__index");
  lua_setmetatable (L, -2);
}

/* ------------------------------------------------------------------------ */
/* Errors                                                                    */
/* ------------------------------------------------------------------------ */

static const char *
error_code (const GError *error)
{
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    return "not-found";
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_READ_ONLY))
    return "permission-denied";
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return "cancelled";
  return "other";
}

typedef struct {
  const char *code;
  const char *message;
} ErrorInfo;

static void
push_error_table (lua_State *L,
                  gpointer   data)
{
  ErrorInfo *info = data;

  lua_createtable (L, 0, 2);
  lua_pushstring (L, info->message);
  lua_setfield (L, -2, "message");
  lua_pushstring (L, info->code);
  lua_setfield (L, -2, "code");
}

/* ------------------------------------------------------------------------ */
/* ShellLuaFsTask                                                            */
/* ------------------------------------------------------------------------ */

typedef enum {
  FS_READ,
  FS_WRITE,
  FS_LIST,
  FS_REMOVE
} FsOp;

typedef struct {
  char       *name;
  const char *type;
} ListEntry;

static void
list_entry_free (gpointer data)
{
  ListEntry *entry = data;

  g_free (entry->name);
  g_free (entry);
}

#define SHELL_TYPE_LUA_FS_TASK (shell_lua_fs_task_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaFsTask, shell_lua_fs_task, SHELL, LUA_FS_TASK, GObject)

struct _ShellLuaFsTask
{
  GObject parent_instance;

  ShellLuaState  *state;       /* unowned; NULL once destroyed */
  FsOp            op;
  char           *path;
  GFile          *file;
  GCancellable   *cancellable;
  int             done_ref;
  int             error_ref;
  gboolean        finished;

  /* read */
  gsize           max_bytes;
  GByteArray     *data;
  GInputStream   *in;

  /* write */
  GBytes         *contents;
  GOutputStream  *out;

  /* list */
  GFileEnumerator *enumerator;
  GPtrArray       *entries;      /* ListEntry */
};

G_DEFINE_FINAL_TYPE (ShellLuaFsTask, shell_lua_fs_task, G_TYPE_OBJECT)

static void
shell_lua_fs_task_finalize (GObject *object)
{
  ShellLuaFsTask *task = SHELL_LUA_FS_TASK (object);

  g_free (task->path);
  g_clear_object (&task->file);
  g_clear_object (&task->cancellable);
  g_clear_pointer (&task->data, g_byte_array_unref);
  g_clear_object (&task->in);
  g_clear_pointer (&task->contents, g_bytes_unref);
  g_clear_object (&task->out);
  g_clear_object (&task->enumerator);
  g_clear_pointer (&task->entries, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_lua_fs_task_parent_class)->finalize (object);
}

static void
shell_lua_fs_task_class_init (ShellLuaFsTaskClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = shell_lua_fs_task_finalize;
}

static void
shell_lua_fs_task_init (ShellLuaFsTask *task)
{
  task->done_ref = LUA_NOREF;
  task->error_ref = LUA_NOREF;
  task->cancellable = g_cancellable_new ();
}

/* The ShellLuaOwnOps destroy rule: the operation is over, whatever way. */
static void
task_destroy_op (GObject *object)
{
  ShellLuaFsTask *task = SHELL_LUA_FS_TASK (object);
  ShellLuaState *state = task->state;

  if (!task->finished)
    g_cancellable_cancel (task->cancellable);
  task->finished = TRUE;
  task->state = NULL;
  release_ref (state, &task->done_ref);
  release_ref (state, &task->error_ref);
}

static const ShellLuaOwnOps task_ops = { .destroy = task_destroy_op };

/* Ends @task: hands its slot back (the handle dies), then runs on_done (when
 * @code is NULL) or on_error / the log. Does nothing for a task that is over
 * already. */
static void
task_finish (ShellLuaFsTask       *task,
             const char           *code,
             const char           *message,
             int                   nargs,
             ShellLuaPushArgsFunc  push_args,
             gpointer              push_data,
             const char           *what)
{
  g_autoptr(ShellLuaState) state = NULL;
  int done_ref, error_ref;

  if (task->finished || task->state == NULL)
    return;

  state = g_object_ref (task->state);
  g_object_ref (task);
  done_ref = task->done_ref;
  error_ref = task->error_ref;
  task->done_ref = LUA_NOREF;
  task->error_ref = LUA_NOREF;
  task->finished = TRUE;

  if (shell_lua_state_is_alive (state))
    shell_lua_destroy_object (shell_lua_state_get_L (state), G_OBJECT (task));

  if (code == NULL)
    {
      if (done_ref != LUA_NOREF && shell_lua_state_is_alive (state))
        shell_lua_call_ref (state, done_ref, nargs, push_args, push_data);
    }
  else if (error_ref != LUA_NOREF && shell_lua_state_is_alive (state))
    {
      ErrorInfo info = { code, message };

      shell_lua_call_ref (state, error_ref, 1, push_error_table, &info);
    }
  else if (error_ref == LUA_NOREF)
    {
      ext_log (state, G_LOG_LEVEL_MESSAGE, "%s failed: %s", what, message);
    }

  release_ref (state, &done_ref);
  release_ref (state, &error_ref);
  g_object_unref (task);
}

static void
task_succeed (ShellLuaFsTask       *task,
              int                   nargs,
              ShellLuaPushArgsFunc  push_args,
              gpointer              push_data)
{
  task_finish (task, NULL, NULL, nargs, push_args, push_data, NULL);
}

static const char *
op_name (FsOp op)
{
  switch (op)
    {
    case FS_READ:
      return "shell.fs.read";
    case FS_WRITE:
      return "shell.fs.write";
    case FS_LIST:
      return "shell.fs.list";
    case FS_REMOVE:
    default:
      return "shell.fs.remove";
    }
}

static void
task_fail (ShellLuaFsTask *task,
           const char     *code,
           const char     *message)
{
  g_autofree char *what = g_strdup_printf ("%s('%s')", op_name (task->op), task->path);

  task_finish (task, code, message, 0, NULL, NULL, what);
}

static void
task_fail_error (ShellLuaFsTask *task,
                 const GError   *error)
{
  /* Only a cancellation by the task itself gives this, and that ends the task
   * first. */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  task_fail (task, error_code (error), error->message);
}

static void
ignore_close (GObject      *source,
              GAsyncResult *result,
              gpointer      user_data)
{
  if (G_IS_INPUT_STREAM (source))
    g_input_stream_close_finish (G_INPUT_STREAM (source), result, NULL);
  else if (G_IS_FILE_ENUMERATOR (source))
    g_file_enumerator_close_finish (G_FILE_ENUMERATOR (source), result, NULL);
}

/* --- read ----------------------------------------------------------------- */

static void
push_contents (lua_State *L,
               gpointer   data)
{
  GByteArray *bytes = data;

  lua_pushlstring (L, (const char *) bytes->data, bytes->len);
}

static void read_next (ShellLuaFsTask *task);

static void
read_chunk_cb (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  ShellLuaFsTask *task = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), result, &error);

  if (task->finished)
    {
      g_object_unref (task);
      return;
    }

  if (bytes == NULL)
    {
      task_fail_error (task, error);
    }
  else if (g_bytes_get_size (bytes) == 0)
    {
      g_autoptr(GByteArray) contents = g_steal_pointer (&task->data);

      g_input_stream_close_async (task->in, G_PRIORITY_DEFAULT, NULL, ignore_close, NULL);
      task_succeed (task, 1, push_contents, contents);
    }
  else
    {
      gsize size;
      gconstpointer data = g_bytes_get_data (bytes, &size);

      g_byte_array_append (task->data, data, size);
      if (task->data->len > task->max_bytes)
        {
          g_autofree char *message = g_strdup_printf ("'%s' is larger than %" G_GSIZE_FORMAT " bytes",
                                                      task->path, task->max_bytes);

          g_input_stream_close_async (task->in, G_PRIORITY_DEFAULT, NULL, ignore_close, NULL);
          task_fail (task, "too-large", message);
        }
      else
        {
          read_next (task);
        }
    }
  g_object_unref (task);
}

static void
read_next (ShellLuaFsTask *task)
{
  g_input_stream_read_bytes_async (task->in, READ_CHUNK, G_PRIORITY_DEFAULT, task->cancellable,
                                   read_chunk_cb, g_object_ref (task));
}

static void
read_opened_cb (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  ShellLuaFsTask *task = user_data;
  g_autoptr(GError) error = NULL;
  GFileInputStream *stream = g_file_read_finish (G_FILE (source), result, &error);

  if (task->finished)
    {
      g_clear_object (&stream);
    }
  else if (stream == NULL)
    {
      task_fail_error (task, error);
    }
  else
    {
      task->in = G_INPUT_STREAM (stream);
      task->data = g_byte_array_new ();
      read_next (task);
    }
  g_object_unref (task);
}

/* --- write ---------------------------------------------------------------- */

/* Replacing: g_file_replace_contents_async () writes a temporary file and
 * renames it over the target at the end, and throws it away when the
 * operation is cancelled, so the old content stays if anything fails.
 * Appending: open, write, close. GIO refuses a NULL buffer, so an empty write
 * never calls the write functions. */

static void
replace_done_cb (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  ShellLuaFsTask *task = user_data;
  g_autoptr(GError) error = NULL;

  if (!g_file_replace_contents_finish (G_FILE (source), result, NULL, &error))
    {
      if (!task->finished)
        task_fail_error (task, error);
    }
  else
    {
      task_succeed (task, 0, NULL, NULL);
    }
  g_object_unref (task);
}

static void
append_closed_cb (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  ShellLuaFsTask *task = user_data;
  g_autoptr(GError) error = NULL;

  if (!g_output_stream_close_finish (G_OUTPUT_STREAM (source), result, &error))
    task_fail_error (task, error);
  else
    task_succeed (task, 0, NULL, NULL);
  g_object_unref (task);
}

static void
append_close (ShellLuaFsTask *task)
{
  g_output_stream_close_async (task->out, G_PRIORITY_DEFAULT, task->cancellable,
                               append_closed_cb, g_object_ref (task));
}

static void
append_written_cb (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  ShellLuaFsTask *task = user_data;
  g_autoptr(GError) error = NULL;
  gsize written = 0;

  if (task->finished)
    {
      g_output_stream_write_all_finish (G_OUTPUT_STREAM (source), result, &written, NULL);
    }
  else if (!g_output_stream_write_all_finish (G_OUTPUT_STREAM (source), result, &written, &error))
    {
      g_output_stream_close_async (task->out, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
      task_fail_error (task, error);
    }
  else
    {
      append_close (task);
    }
  g_object_unref (task);
}

static void
append_opened_cb (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  ShellLuaFsTask *task = user_data;
  g_autoptr(GError) error = NULL;
  GFileOutputStream *stream = g_file_append_to_finish (G_FILE (source), result, &error);

  if (task->finished)
    {
      g_clear_object (&stream);
    }
  else if (stream == NULL)
    {
      task_fail_error (task, error);
    }
  else
    {
      gsize size;
      gconstpointer data = g_bytes_get_data (task->contents, &size);

      task->out = G_OUTPUT_STREAM (stream);
      if (size == 0)
        append_close (task);
      else
        g_output_stream_write_all_async (task->out, data, size, G_PRIORITY_DEFAULT,
                                         task->cancellable, append_written_cb, g_object_ref (task));
    }
  g_object_unref (task);
}

/* --- list ----------------------------------------------------------------- */

static const char *
file_type_name (GFileType type)
{
  switch (type)
    {
    case G_FILE_TYPE_REGULAR:
      return "file";
    case G_FILE_TYPE_DIRECTORY:
      return "directory";
    case G_FILE_TYPE_SYMBOLIC_LINK:
      return "symlink";
    case G_FILE_TYPE_UNKNOWN:
    case G_FILE_TYPE_SPECIAL:
    case G_FILE_TYPE_SHORTCUT:
    case G_FILE_TYPE_MOUNTABLE:
    default:
      return "special";
    }
}

static int
compare_entries (gconstpointer a,
                 gconstpointer b)
{
  const ListEntry * const *ea = a;
  const ListEntry * const *eb = b;

  return strcmp ((*ea)->name, (*eb)->name);
}

static void
push_entries (lua_State *L,
              gpointer   data)
{
  GPtrArray *entries = data;
  guint i;

  luaL_checkstack (L, 6, "shell.fs.list");
  lua_createtable (L, entries->len, 0);
  for (i = 0; i < entries->len; i++)
    {
      ListEntry *entry = g_ptr_array_index (entries, i);

      lua_createtable (L, 0, 2);
      lua_pushstring (L, entry->name);
      lua_setfield (L, -2, "name");
      lua_pushstring (L, entry->type);
      lua_setfield (L, -2, "type");
      lua_rawseti (L, -2, i + 1);
    }
}

static void list_next (ShellLuaFsTask *task);

static void
list_batch_cb (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  ShellLuaFsTask *task = user_data;
  g_autoptr(GError) error = NULL;
  GList *infos = g_file_enumerator_next_files_finish (G_FILE_ENUMERATOR (source), result, &error);
  GList *l;

  if (task->finished)
    {
      g_list_free_full (infos, g_object_unref);
      g_object_unref (task);
      return;
    }

  if (error != NULL)
    {
      task_fail_error (task, error);
    }
  else if (infos == NULL)
    {
      g_autoptr(GPtrArray) entries = g_steal_pointer (&task->entries);

      g_ptr_array_sort (entries, compare_entries);
      g_file_enumerator_close_async (task->enumerator, G_PRIORITY_DEFAULT, NULL, ignore_close, NULL);
      task_succeed (task, 1, push_entries, entries);
    }
  else
    {
      for (l = infos; l != NULL; l = l->next)
        {
          GFileInfo *info = l->data;
          ListEntry *entry = g_new0 (ListEntry, 1);

          entry->name = g_strdup (g_file_info_get_name (info));
          entry->type = file_type_name (g_file_info_get_file_type (info));
          g_ptr_array_add (task->entries, entry);
        }

      if (task->entries->len > MAX_LIST_ENTRIES)
        {
          g_autofree char *message = g_strdup_printf ("'%s' has more than %d entries",
                                                      task->path, MAX_LIST_ENTRIES);

          g_file_enumerator_close_async (task->enumerator, G_PRIORITY_DEFAULT, NULL, ignore_close, NULL);
          task_fail (task, "too-large", message);
        }
      else
        {
          list_next (task);
        }
    }

  g_list_free_full (infos, g_object_unref);
  g_object_unref (task);
}

static void
list_next (ShellLuaFsTask *task)
{
  g_file_enumerator_next_files_async (task->enumerator, LIST_BATCH, G_PRIORITY_DEFAULT,
                                      task->cancellable, list_batch_cb, g_object_ref (task));
}

static void
list_opened_cb (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  ShellLuaFsTask *task = user_data;
  g_autoptr(GError) error = NULL;
  GFileEnumerator *enumerator = g_file_enumerate_children_finish (G_FILE (source), result, &error);

  if (task->finished)
    {
      g_clear_object (&enumerator);
    }
  else if (enumerator == NULL)
    {
      task_fail_error (task, error);
    }
  else
    {
      task->enumerator = enumerator;
      task->entries = g_ptr_array_new_with_free_func (list_entry_free);
      list_next (task);
    }
  g_object_unref (task);
}

/* --- remove --------------------------------------------------------------- */

static void
remove_done_cb (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  ShellLuaFsTask *task = user_data;
  g_autoptr(GError) error = NULL;

  if (!g_file_delete_finish (G_FILE (source), result, &error))
    {
      if (!task->finished)
        task_fail_error (task, error);
    }
  else
    {
      task_succeed (task, 0, NULL, NULL);
    }
  g_object_unref (task);
}

/* --- Lua side ------------------------------------------------------------- */

typedef struct {
  GWeakRef state;
  int      ref;
} Cancelled;

static gboolean
cancelled_idle (gpointer data)
{
  Cancelled *cancelled = data;
  ErrorInfo info = { "cancelled", "the operation was cancelled" };
  g_autoptr(ShellLuaState) state = g_weak_ref_get (&cancelled->state);

  if (state != NULL && shell_lua_state_is_alive (state))
    {
      shell_lua_call_ref (state, cancelled->ref, 1, push_error_table, &info);
      shell_lua_state_unref (state, cancelled->ref);
    }
  g_weak_ref_clear (&cancelled->state);
  g_free (cancelled);
  return G_SOURCE_REMOVE;
}

static int
l_task_cancel (lua_State *L)
{
  GObject *object = shell_lua_handle_get (L, 1, "shell.Task");
  ShellLuaFsTask *task;
  ShellLuaState *state;
  int error_ref;

  if (object == NULL)
    return 0;

  task = SHELL_LUA_FS_TASK (object);
  state = task->state;
  error_ref = task->error_ref;
  task->error_ref = LUA_NOREF;

  /* cancels the GIO operation, drops on_done, frees the slot; the handle dies */
  shell_lua_destroy_object (L, object);

  if (error_ref != LUA_NOREF)
    {
      /* on_error runs later, never inside cancel () */
      Cancelled *cancelled = g_new0 (Cancelled, 1);

      g_weak_ref_init (&cancelled->state, state);
      cancelled->ref = error_ref;
      g_idle_add_full (G_PRIORITY_DEFAULT, cancelled_idle, cancelled, NULL);
    }
  return 0;
}

static const luaL_Reg task_methods[] = {
  { NULL, NULL }
};

static const ShellLuaHandleMethod task_methods_ex[] = {
  { "cancel", l_task_cancel, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty task_properties[] = {
  { NULL }
};

static ShellLuaHandleType task_type = {
  .name = "shell.Task",
  .methods = task_methods,
  .methods_ex = task_methods_ex,
  .properties = task_properties,
};

/* Creates and owns a task for the checked @path; takes over the references.
 * Nothing may raise between the capacity check and here. */
static ShellLuaFsTask *
task_new (lua_State  *L,
          FsOp        op,
          const char *path,
          int         done_ref,
          int         error_ref)
{
  ShellLuaFsTask *task = g_object_new (SHELL_TYPE_LUA_FS_TASK, NULL);

  task->state = shell_lua_state_from_L (L);
  task->op = op;
  task->path = g_strdup (path);
  task->file = g_file_new_for_path (path);
  task->done_ref = done_ref;
  task->error_ref = error_ref;

  shell_lua_own_object (L, G_OBJECT (task), &task_ops);
  g_object_unref (task);   /* the owner holds it now */
  return task;
}

static int
push_task (lua_State      *L,
           ShellLuaFsTask *task)
{
  shell_lua_handle_push (L, "shell.Task", G_OBJECT (task));
  return 1;
}

static const char * const read_fields[] = { "path", "max_bytes", "on_done", "on_error", NULL };
static const char * const write_fields[] = { "path", "contents", "append", "on_done", "on_error", NULL };
static const char * const plain_fields[] = { "path", "on_done", "on_error", NULL };

static int
l_fs_read (lua_State *L)
{
  const char *path;
  lua_Integer max_bytes;
  char *checked;
  int done_ref, error_ref, done_idx, error_idx;
  ShellLuaFsTask *task;

  shell_lua_check_fields (L, 1, read_fields);
  max_bytes = shell_lua_opt_integer (L, 1, "max_bytes", DEFAULT_MAX_BYTES);
  if (max_bytes < 1 || max_bytes > MAX_MAX_BYTES)
    return luaL_error (L, "field 'max_bytes' must be between 1 and %d", MAX_MAX_BYTES);
  done_idx = push_callback_field (L, 1, "on_done", TRUE);
  error_idx = push_callback_field (L, 1, "on_error", FALSE);
  path = check_path_field (L, 1);
  shell_lua_check_owned_capacity (L);

  checked = check_path (L, "shell.fs.read()", path, FALSE, NULL);
  done_ref = ref_of (L, done_idx);
  error_ref = ref_of (L, error_idx);

  task = task_new (L, FS_READ, checked, done_ref, error_ref);
  task->max_bytes = (gsize) max_bytes;
  g_free (checked);
  g_file_read_async (task->file, G_PRIORITY_DEFAULT, task->cancellable,
                     read_opened_cb, g_object_ref (task));
  return push_task (L, task);
}

static int
l_fs_write (lua_State *L)
{
  const char *path;
  const char *contents;
  size_t length;
  gboolean append;
  gboolean in_data_dir = FALSE;
  char *checked;
  int done_ref, error_ref, done_idx, error_idx;
  ShellLuaFsTask *task;

  shell_lua_check_fields (L, 1, write_fields);
  if (lua_getfield (L, 1, "contents") != LUA_TSTRING)
    {
      if (lua_isnil (L, -1))
        return luaL_error (L, "missing required field 'contents' (a string)");
      return luaL_error (L, "field 'contents' must be a string, got %s", luaL_typename (L, -1));
    }
  contents = lua_tolstring (L, -1, &length);
  if (length > MAX_WRITE_BYTES)
    return luaL_error (L, "field 'contents' is too large (at most %d bytes)", MAX_WRITE_BYTES);
  append = shell_lua_opt_boolean (L, 1, "append", FALSE);
  done_idx = push_callback_field (L, 1, "on_done", FALSE);
  error_idx = push_callback_field (L, 1, "on_error", FALSE);
  path = check_path_field (L, 1);
  shell_lua_check_owned_capacity (L);

  checked = check_path (L, "shell.fs.write()", path, TRUE, &in_data_dir);
  done_ref = ref_of (L, done_idx);
  error_ref = ref_of (L, error_idx);

  task = task_new (L, FS_WRITE, checked, done_ref, error_ref);
  /* never NULL data, not even for an empty write */
  task->contents = length > 0 ? g_bytes_new (contents, length) : g_bytes_new_static ("", 0);

  /* a data directory that does not exist yet (or a subdirectory of it) is
   * made on the way; anywhere else a missing parent is an error */
  if (in_data_dir)
    {
      g_autofree char *parent = g_path_get_dirname (checked);

      g_mkdir_with_parents (parent, 0700);
    }
  g_free (checked);

  if (append)
    {
      g_file_append_to_async (task->file, G_FILE_CREATE_NONE, G_PRIORITY_DEFAULT,
                              task->cancellable, append_opened_cb, g_object_ref (task));
    }
  else
    {
      g_file_replace_contents_bytes_async (task->file, task->contents, NULL, FALSE,
                                           G_FILE_CREATE_NONE, task->cancellable,
                                           replace_done_cb, g_object_ref (task));
    }
  return push_task (L, task);
}

static int
l_fs_list (lua_State *L)
{
  const char *path;
  char *checked;
  int done_ref, error_ref, done_idx, error_idx;
  ShellLuaFsTask *task;

  shell_lua_check_fields (L, 1, plain_fields);
  done_idx = push_callback_field (L, 1, "on_done", TRUE);
  error_idx = push_callback_field (L, 1, "on_error", FALSE);
  path = check_path_field (L, 1);
  shell_lua_check_owned_capacity (L);

  checked = check_path (L, "shell.fs.list()", path, FALSE, NULL);
  done_ref = ref_of (L, done_idx);
  error_ref = ref_of (L, error_idx);

  task = task_new (L, FS_LIST, checked, done_ref, error_ref);
  g_free (checked);
  g_file_enumerate_children_async (task->file, "standard::name,standard::type",
                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT,
                                   task->cancellable, list_opened_cb, g_object_ref (task));
  return push_task (L, task);
}

static int
l_fs_remove (lua_State *L)
{
  const char *path;
  char *checked;
  int done_ref, error_ref, done_idx, error_idx;
  ShellLuaFsTask *task;

  shell_lua_check_fields (L, 1, plain_fields);
  done_idx = push_callback_field (L, 1, "on_done", FALSE);
  error_idx = push_callback_field (L, 1, "on_error", FALSE);
  path = check_path_field (L, 1);
  shell_lua_check_owned_capacity (L);

  checked = check_path (L, "shell.fs.remove()", path, TRUE, NULL);
  done_ref = ref_of (L, done_idx);
  error_ref = ref_of (L, error_idx);

  task = task_new (L, FS_REMOVE, checked, done_ref, error_ref);
  g_free (checked);
  g_file_delete_async (task->file, G_PRIORITY_DEFAULT, task->cancellable,
                       remove_done_cb, g_object_ref (task));
  return push_task (L, task);
}

/* ------------------------------------------------------------------------ */
/* exists, info                                                              */
/* ------------------------------------------------------------------------ */

static int
l_fs_exists (lua_State *L)
{
  const char *path = check_path_string (L, 1, "argument #1 (path)");
  char *checked = check_path (L, "shell.fs.exists()", path, FALSE, NULL);
  gboolean exists = g_file_test (checked, G_FILE_TEST_EXISTS);

  g_free (checked);
  lua_pushboolean (L, exists);
  return 1;
}

static int
l_fs_info (lua_State *L)
{
  const char *path = check_path_string (L, 1, "argument #1 (path)");
  char *checked = check_path (L, "shell.fs.info()", path, FALSE, NULL);
  GFile *file = g_file_new_for_path (checked);
  GError *error = NULL;
  GFileInfo *info;
  lua_Number modified = 0;
  lua_Integer size = 0;
  const char *type = NULL;
  gboolean not_found = FALSE;
  char *message = NULL;

  g_free (checked);
  info = g_file_query_info (file, "standard::type,standard::size,time::modified,time::modified-usec",
                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
  g_object_unref (file);

  if (info != NULL)
    {
      type = file_type_name (g_file_info_get_file_type (info));
      size = (lua_Integer) g_file_info_get_size (info);
      modified = (lua_Number) g_file_info_get_attribute_uint64 (info, "time::modified") +
                 (lua_Number) g_file_info_get_attribute_uint32 (info, "time::modified-usec") / 1e6;
      g_object_unref (info);
    }
  else
    {
      not_found = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      message = g_strdup (error->message);
      g_error_free (error);
    }

  if (type == NULL)
    {
      lua_pushnil (L);
      if (not_found)
        {
          g_free (message);
          return 1;
        }
      lua_pushstring (L, message);
      g_free (message);
      return 2;
    }

  lua_createtable (L, 0, 3);
  lua_pushstring (L, type);
  lua_setfield (L, -2, "type");
  lua_pushinteger (L, size);
  lua_setfield (L, -2, "size");
  lua_pushnumber (L, modified);
  lua_setfield (L, -2, "modified");
  return 1;
}

/* ------------------------------------------------------------------------ */
/* ShellLuaFsWatch                                                           */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LUA_FS_WATCH (shell_lua_fs_watch_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaFsWatch, shell_lua_fs_watch, SHELL, LUA_FS_WATCH, GObject)

struct _ShellLuaFsWatch
{
  GObject parent_instance;

  char         *path;
  GFileMonitor *monitor;
};

G_DEFINE_FINAL_TYPE (ShellLuaFsWatch, shell_lua_fs_watch, G_TYPE_OBJECT)

enum { WATCH_PROP_0, WATCH_PROP_PATH, WATCH_N_PROPS };
static GParamSpec *watch_props[WATCH_N_PROPS];

static void
shell_lua_fs_watch_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellLuaFsWatch *watch = SHELL_LUA_FS_WATCH (object);

  switch (prop_id)
    {
    case WATCH_PROP_PATH:
      g_value_set_string (value, watch->path);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_lua_fs_watch_finalize (GObject *object)
{
  ShellLuaFsWatch *watch = SHELL_LUA_FS_WATCH (object);

  g_free (watch->path);
  g_clear_object (&watch->monitor);

  G_OBJECT_CLASS (shell_lua_fs_watch_parent_class)->finalize (object);
}

static void
shell_lua_fs_watch_class_init (ShellLuaFsWatchClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_lua_fs_watch_get_property;
  object_class->finalize = shell_lua_fs_watch_finalize;

  watch_props[WATCH_PROP_PATH] =
    g_param_spec_string ("path", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, WATCH_N_PROPS, watch_props);
}

static void
shell_lua_fs_watch_init (ShellLuaFsWatch *watch)
{
}

/* The callbacks of a watch are killed by the runtime before this runs, so
 * the monitor can only stop. */
static void
watch_destroy_op (GObject *object)
{
  ShellLuaFsWatch *watch = SHELL_LUA_FS_WATCH (object);

  if (watch->monitor != NULL)
    g_file_monitor_cancel (watch->monitor);
}

static const ShellLuaOwnOps watch_ops = { .destroy = watch_destroy_op };

typedef struct {
  const char *event;
  char       *path;
  char       *other_path;
} WatchEvent;

static void
push_watch_event (lua_State *L,
                  gpointer   data)
{
  WatchEvent *event = data;

  lua_pushstring (L, event->event);
  lua_pushstring (L, event->path);
  if (event->other_path != NULL)
    lua_pushstring (L, event->other_path);
}

static void
watch_changed_cb (GFileMonitor      *monitor,
                  GFile             *file,
                  GFile             *other_file,
                  GFileMonitorEvent  event_type,
                  gpointer           user_data)
{
  ShellLuaCallback *callback = user_data;
  WatchEvent event = { NULL, NULL, NULL };

  switch (event_type)
    {
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
      event.event = "created";
      break;
    case G_FILE_MONITOR_EVENT_CHANGED:
      event.event = "changed";
      break;
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
      event.event = "deleted";
      break;
    case G_FILE_MONITOR_EVENT_RENAMED:
      event.event = "renamed";
      break;
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
    case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
    case G_FILE_MONITOR_EVENT_UNMOUNTED:
    default:
      return;
    }

  event.path = g_file_get_path (file);
  if (event.path == NULL)
    return;
  if (event_type == G_FILE_MONITOR_EVENT_RENAMED && other_file != NULL)
    event.other_path = g_file_get_path (other_file);

  shell_lua_callback_emit_data (callback, event.other_path != NULL ? 3 : 2,
                                push_watch_event, &event);
  g_free (event.path);
  g_free (event.other_path);
}

static int
l_watch_disconnect (lua_State *L)
{
  GObject *object = shell_lua_handle_get (L, 1, "shell.FileWatch");

  if (object != NULL)
    shell_lua_destroy_object (L, object);
  return 0;
}

static const luaL_Reg watch_methods[] = {
  { NULL, NULL }
};

static const ShellLuaHandleMethod watch_methods_ex[] = {
  { "disconnect", l_watch_disconnect,           SHELL_LUA_METHOD_DEAD_OK },
  { "destroy",    shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty watch_properties[] = {
  { "path", "path", TRUE, FALSE },
  { NULL }
};

static ShellLuaHandleType watch_type = {
  .name = "shell.FileWatch",
  .methods = watch_methods,
  .methods_ex = watch_methods_ex,
  .properties = watch_properties,
};

static const char * const watch_fields[] = { "path", "on_changed", NULL };

static int
l_fs_watch (lua_State *L)
{
  const char *path;
  char *checked;
  GFile *file;
  GFileMonitor *monitor;
  GError *error = NULL;
  ShellLuaFsWatch *watch;
  ShellLuaCallback *callback;
  int ref, changed_idx;

  shell_lua_check_fields (L, 1, watch_fields);
  changed_idx = push_callback_field (L, 1, "on_changed", TRUE);
  path = check_path_field (L, 1);
  shell_lua_check_owned_capacity (L);

  checked = check_path (L, "shell.fs.watch()", path, FALSE, NULL);
  ref = ref_of (L, changed_idx);

  file = g_file_new_for_path (checked);
  monitor = g_file_monitor (file, G_FILE_MONITOR_WATCH_MOVES, NULL, &error);
  g_object_unref (file);
  if (monitor == NULL)
    {
      luaL_unref (L, LUA_REGISTRYINDEX, ref);
      g_free (checked);
      lua_pushnil (L);
      lua_pushstring (L, error->message);
      g_error_free (error);
      return 2;
    }

  watch = g_object_new (SHELL_TYPE_LUA_FS_WATCH, NULL);
  watch->path = checked;
  watch->monitor = monitor;
  shell_lua_own_object (L, G_OBJECT (watch), &watch_ops);
  g_object_unref (watch);   /* the owner holds it now */

  callback = shell_lua_callback_new (L, G_OBJECT (watch), ref);
  shell_lua_callback_connect (callback, monitor, "changed", G_CALLBACK (watch_changed_cb));

  shell_lua_handle_push (L, "shell.FileWatch", G_OBJECT (watch));
  return 1;
}

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

static const luaL_Reg fs_funcs[] = {
  { "exists", l_fs_exists },
  { "info",   l_fs_info },
  { "read",   l_fs_read },
  { "write",  l_fs_write },
  { "list",   l_fs_list },
  { "remove", l_fs_remove },
  { "watch",  l_fs_watch },
  { NULL, NULL }
};

void
shell_lua_api_fs_register (ShellLuaState *state,
                           lua_State     *L)
{
  int shell = lua_absindex (L, -1);

  luaL_checkstack (L, 6, "shell fs api");

  task_type.gtype = SHELL_TYPE_LUA_FS_TASK;
  shell_lua_handle_register_type (L, &task_type);
  watch_type.gtype = SHELL_TYPE_LUA_FS_WATCH;
  shell_lua_handle_register_type (L, &watch_type);

  lua_newtable (L);
  luaL_setfuncs (L, fs_funcs, 0);
  lua_setfield (L, shell, "fs");
}
