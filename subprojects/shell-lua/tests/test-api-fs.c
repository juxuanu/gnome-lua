/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.fs and shell.extension.data_dir (docs/api/fs.md).
 *
 * Everything happens in a temporary directory under g_get_tmp_dir (which is
 * also XDG_DATA_HOME for the program); the extensions are installed there
 * and enabled through the extension manager, because the capabilities and the
 * directories of an extension are what the module checks.
 */

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test-utils.h"
#include "shell-lua-extension-private.h"

static char *base_dir = NULL;   /* the temporary directory of the whole program */

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static int bumps = 0;

static int
cf_bump (lua_State *L)
{
  bumps++;
  return 0;
}

G_GNUC_PRINTF (2, 3)
static void
do_f (ShellLuaState *s, const char *format, ...)
{
  va_list args;
  char *code;

  va_start (args, format);
  code = g_strdup_vprintf (format, args);
  va_end (args);
  tu_do (s, code);
  g_free (code);
}

G_GNUC_PRINTF (3, 4)
static void
fails_f (ShellLuaState *s, const char *substr, const char *format, ...)
{
  va_list args;
  char *code;

  va_start (args, format);
  code = g_strdup_vprintf (format, args);
  va_end (args);
  tu_assert_fails (s, code, SHELL_LUA_ERROR_RUNTIME, substr);
  g_free (code);
}

typedef struct {
  ShellLuaState *state;
  const char    *expr;
} WaitLua;

static gboolean
pred_lua (gpointer data)
{
  WaitLua *wait = data;

  return tu_eval_bool (wait->state, wait->expr);
}

/* Iterate the main loop until the Lua expression is true (5 s at most). */
static void
wait_lua (ShellLuaState *s, const char *expr)
{
  WaitLua wait = { s, expr };

  if (!tu_wait_for (pred_lua, &wait, 5000))
    g_error ("timed out waiting for: %s", expr);
}

typedef struct {
  char                     *root;
  char                     *user;      /* extension search directory */
  char                     *scratch;   /* files of the test */
  char                     *data_home; /* XDG_DATA_HOME */
  ShellLuaExtensionManager *manager;
} Env;

static Env *
env_new (void)
{
  Env *env = g_new0 (Env, 1);
  g_autoptr (GFile) dir = NULL;
  GFile *dirs[1];

  env->root = g_dir_make_tmp ("t-XXXXXX", NULL);
  g_assert_nonnull (env->root);
  /* g_dir_make_tmp is under g_get_tmp_dir */
  env->user = g_build_filename (env->root, "extensions", NULL);
  env->scratch = g_build_filename (env->root, "scratch", NULL);
  env->data_home = g_strdup (g_get_user_data_dir ());
  g_assert_cmpint (g_mkdir_with_parents (env->user, 0700), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (env->scratch, 0700), ==, 0);

  env->manager = g_object_new (SHELL_TYPE_LUA_EXTENSION_MANAGER, NULL);
  /* the tests make errors on purpose, many of them */
  shell_lua_extension_manager_set_error_limit (env->manager, G_MAXUINT);
  dir = g_file_new_for_path (env->user);
  dirs[0] = dir;
  shell_lua_extension_manager_set_search_dirs (env->manager, dirs, 1);
  return env;
}

static void
env_free (Env *env)
{
  g_object_unref (env->manager);
  tu_rm_rf (env->root);
  g_free (env->root);
  g_free (env->user);
  g_free (env->scratch);
  g_free (env->data_home);
  g_free (env);
}

/* Installs and enables an extension with the given capabilities (a JSON
 * array or NULL); returns its state. */
static ShellLuaState *
env_enable (Env *env, const char *uuid, const char *caps)
{
  g_autofree char *dir = g_build_filename (env->user, uuid, NULL);
  g_autofree char *metadata = g_strdup_printf ("{\"uuid\": \"%s\", \"name\": \"Test\", \"shell-api\": \"1.1\"%s%s}",
                                               uuid, caps != NULL ? ", \"capabilities\": " : "",
                                               caps != NULL ? caps : "");
  g_autoptr (GError) error = NULL;
  ShellLuaExtension *extension;
  ShellLuaState *state;

  tu_write_file (dir, "metadata.json", metadata);
  tu_write_file (dir, "extension.lua", "return { enable = function() end }");
  shell_lua_extension_manager_scan (env->manager);
  g_assert_true (shell_lua_extension_manager_enable (env->manager, uuid, &error));
  g_assert_no_error (error);
  extension = shell_lua_extension_manager_lookup (env->manager, uuid);
  g_assert_nonnull (extension);
  state = shell_lua_extension_get_lua_state (extension);
  g_assert_nonnull (state);
  tu_set_cfunction (state, "bump", cf_bump);
  bumps = 0;
  return state;
}

static char *
scratch_path (Env *env, const char *name)
{
  return g_build_filename (env->scratch, name, NULL);
}

static void
put (Env *env, const char *name, const char *contents, gssize length)
{
  g_autofree char *path = scratch_path (env, name);
  g_autoptr (GError) error = NULL;
  g_autofree char *parent = g_path_get_dirname (path);

  g_assert_cmpint (g_mkdir_with_parents (parent, 0700), ==, 0);
  g_file_set_contents (path, contents, length, &error);
  g_assert_no_error (error);
}

static char *
slurp (const char *path, gsize *length)
{
  g_autoptr (GError) error = NULL;
  char *contents = NULL;

  g_file_get_contents (path, &contents, length, &error);
  g_assert_no_error (error);
  return contents;
}

/* Sets the Lua global P to the scratch path of @name. */
static void
set_p (ShellLuaState *s, Env *env, const char *name)
{
  g_autofree char *path = scratch_path (env, name);

  do_f (s, "P = '%s'", path);
}

/* --- Lua prelude: result slots ------------------------------------------- */

/* got / err / n_done / n_err are written by the callbacks of the snippets */
#define RESET "got = nil; err = nil; n_done = 0; n_err = 0\n"
#define CBS "on_done = function(c) got = c; n_done = n_done + 1 end, " \
            "on_error = function(e) err = e; n_err = n_err + 1 end"

/* ------------------------------------------------------------------------ */
/* data_dir                                                                  */
/* ------------------------------------------------------------------------ */

static void
test_data_dir (void)
{
  Env *env = env_new ();
  g_autofree char *expected = g_build_filename (env->data_home, "gnome-lua", "fsdata@test", NULL);
  g_autofree char *code = NULL;
  ShellLuaState *s;
  GStatBuf st;

  g_assert_false (g_file_test (expected, G_FILE_TEST_EXISTS));
  s = env_enable (env, "fsdata@test", NULL);

  /* enabling, and reading the other fields, does not create it */
  tu_assert_eval_str (s, "shell.extension.uuid", "fsdata@test");
  g_assert_false (g_file_test (expected, G_FILE_TEST_EXISTS));

  tu_assert_eval_str (s, "shell.extension.data_dir", expected);
  g_assert_true (g_file_test (expected, G_FILE_TEST_IS_DIR));
  g_assert_cmpint (g_stat (expected, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0700);

  /* stable, read-only, a string */
  tu_assert_eval_true (s, "shell.extension.data_dir == shell.extension.data_dir");
  tu_assert_eval_str (s, "type(shell.extension.data_dir)", "string");
  tu_assert_fails (s, "shell.extension.data_dir = '/tmp'", SHELL_LUA_ERROR_RUNTIME, "read-only");
  tu_assert_eval_str (s, "shell.extension.data_dir", expected);

  /* another extension has its own */
  {
    g_autofree char *other = g_build_filename (env->data_home, "gnome-lua", "fsdata2@test", NULL);
    ShellLuaState *s2 = env_enable (env, "fsdata2@test", NULL);

    tu_assert_eval_str (s2, "shell.extension.data_dir", other);
  }

  /* an unknown field is still nil */
  tu_assert_eval_true (s, "shell.extension.nothing == nil");

  /* the directory is under the data home, which is a path of the tests */
  code = g_strdup_printf ("shell.extension.data_dir:sub(1, %d) == '%s'",
                          (int) strlen (env->data_home), env->data_home);
  tu_assert_eval_true (s, code);

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* Capability gate                                                           */
/* ------------------------------------------------------------------------ */

static void
test_capability_gate_no_extension (void)
{
  ShellLuaState *sbx = tu_state_new ("sandbox", SHELL_LUA_STATE_SANDBOXED);
  ShellLuaState *priv = tu_state_new ("priv", SHELL_LUA_STATE_PRIVILEGED);

  /* a sandboxed state with no extension has no capabilities at all, and no
   * directories of its own */
  tu_assert_fails (sbx, "shell.fs.exists('/tmp')", SHELL_LUA_ERROR_RUNTIME,
                   "shell.fs.exists() requires the \"fs\" capability");
  tu_assert_fails (sbx, "shell.fs.info('/tmp')", SHELL_LUA_ERROR_RUNTIME,
                   "shell.fs.info() requires the \"fs\" capability");
  tu_assert_fails (sbx, "shell.fs.read{ path = '/tmp/x', on_done = function() end }", SHELL_LUA_ERROR_RUNTIME,
                   "shell.fs.read() requires the \"fs\" capability");
  tu_assert_fails (sbx, "shell.fs.write{ path = '/tmp/x', contents = '' }", SHELL_LUA_ERROR_RUNTIME,
                   "shell.fs.write() requires the \"fs\" capability");
  tu_assert_fails (sbx, "shell.fs.list{ path = '/tmp', on_done = function() end }", SHELL_LUA_ERROR_RUNTIME,
                   "shell.fs.list() requires the \"fs\" capability");
  tu_assert_fails (sbx, "shell.fs.remove{ path = '/tmp/x' }", SHELL_LUA_ERROR_RUNTIME,
                   "shell.fs.remove() requires the \"fs\" capability");
  tu_assert_fails (sbx, "shell.fs.watch{ path = '/tmp', on_changed = function() end }", SHELL_LUA_ERROR_RUNTIME,
                   "shell.fs.watch() requires the \"fs\" capability");

  /* a privileged state passes the capability, whatever the path */
  tu_assert_eval_true (priv, "shell.fs.exists('/tmp')");
  tu_assert_eval_str (priv, "shell.fs.info('/tmp').type", "directory");
  tu_assert_eval_true (priv, "shell.fs.exists('/this/does/not/exist') == false");

  g_object_unref (sbx);
  g_object_unref (priv);
}

static void
test_paths_without_capability (void)
{
  Env *env = env_new ();
  ShellLuaState *s = env_enable (env, "fsplain@test", NULL);
  g_autofree char *data_dir = g_build_filename (env->data_home, "gnome-lua", "fsplain@test", NULL);

  tu_do (s, "ext_dir = shell.extension.dir; data_dir = shell.extension.data_dir");

  /* the two directories, read */
  tu_assert_eval_true (s, "shell.fs.exists(ext_dir)");
  tu_assert_eval_true (s, "shell.fs.exists(ext_dir .. '/extension.lua')");
  tu_assert_eval_str (s, "shell.fs.info(ext_dir .. '/metadata.json').type", "file");
  tu_assert_eval_str (s, "shell.fs.info(ext_dir).type", "directory");
  tu_assert_eval_true (s, "shell.fs.exists(data_dir)");
  tu_assert_eval_true (s, "shell.fs.exists(data_dir .. '/nothing') == false");
  /* `..` that stays inside is fine */
  tu_assert_eval_true (s, "shell.fs.exists(ext_dir .. '/../' .. shell.extension.uuid .. '/extension.lua')");

  /* everything else raises with the capability text, before any I/O */
#define DENIED(call, fn) \
  tu_assert_fails (s, call, SHELL_LUA_ERROR_RUNTIME, "shell.fs." fn "() requires the \"fs\" capability")
  DENIED ("shell.fs.exists('/etc/passwd')", "exists");
  DENIED ("shell.fs.exists('/')", "exists");
  DENIED ("shell.fs.info('/tmp')", "info");
  DENIED ("shell.fs.exists(ext_dir .. '/..')", "exists");
  DENIED ("shell.fs.exists(ext_dir .. '/../../../../etc/passwd')", "exists");
  DENIED ("shell.fs.exists(data_dir .. '/../x')", "exists");
  DENIED ("shell.fs.exists(data_dir .. '/../fsplain@test2')", "exists");
  DENIED ("shell.fs.exists(data_dir .. '2')", "exists");
  DENIED ("shell.fs.read{ path = '/etc/passwd', on_done = function() end }", "read");
  DENIED ("shell.fs.list{ path = '/', on_done = function() end }", "list");
  DENIED ("shell.fs.watch{ path = '/tmp', on_changed = function() end }", "watch");
  /* the extension's own directory is read only */
  DENIED ("shell.fs.write{ path = ext_dir .. '/x.txt', contents = 'x' }", "write");
  DENIED ("shell.fs.write{ path = ext_dir .. '/extension.lua', contents = '' }", "write");
  DENIED ("shell.fs.remove{ path = ext_dir .. '/extension.lua' }", "remove");
  /* nothing outside the data directory is writable */
  DENIED ("shell.fs.write{ path = '/tmp/gnome-lua-test-x', contents = 'x' }", "write");
  DENIED ("shell.fs.remove{ path = data_dir .. '/../x' }", "remove");
#undef DENIED

  /* misuse: raises also when the path would be allowed */
  fails_f (s, "must be an absolute path", "shell.fs.exists('relative/path')");
  fails_f (s, "must be an absolute path", "shell.fs.exists('')");
  fails_f (s, "must be an absolute path", "shell.fs.exists('~/x')");
  fails_f (s, "must be a string, got no value", "shell.fs.exists()");
  fails_f (s, "must be a string, got number", "shell.fs.info(5)");
  fails_f (s, "must not contain a NUL byte", "shell.fs.exists(data_dir .. '\\0x')");
  fails_f (s, "field 'path' must be an absolute path", "shell.fs.read{ path = 'x', on_done = function() end }");
  fails_f (s, "field 'path' must be a string, got number",
           "shell.fs.write{ path = 3, contents = '' }");
  fails_f (s, "missing required field 'path'", "shell.fs.remove{ on_done = function() end }");

  /* symbolic links are followed before the check */
  {
    g_autofree char *link_out = g_build_filename (data_dir, "link-out", NULL);
    g_autofree char *link_in = g_build_filename (data_dir, "link-in", NULL);
    g_autofree char *link_dangling = g_build_filename (data_dir, "link-dangling", NULL);
    g_autofree char *real = g_build_filename (data_dir, "real", NULL);

    g_assert_cmpint (g_mkdir_with_parents (real, 0700), ==, 0);
    g_assert_cmpint (symlink ("/etc", link_out), ==, 0);
    g_assert_cmpint (symlink (real, link_in), ==, 0);
    g_assert_cmpint (symlink ("/nonexistent-gnome-lua-target/x", link_dangling), ==, 0);
    tu_write_file (real, "f.txt", "inside");

    tu_assert_eval_true (s, "shell.fs.exists(data_dir .. '/link-in/f.txt')");
    tu_assert_fails (s, "shell.fs.exists(data_dir .. '/link-out/passwd')", SHELL_LUA_ERROR_RUNTIME,
                     "requires the \"fs\" capability");
    tu_assert_fails (s, "shell.fs.exists(data_dir .. '/link-out')", SHELL_LUA_ERROR_RUNTIME,
                     "requires the \"fs\" capability");
    tu_assert_fails (s, "shell.fs.write{ path = data_dir .. '/link-out/new.txt', contents = 'x' }",
                     SHELL_LUA_ERROR_RUNTIME, "requires the \"fs\" capability");
    /* a link that leads nowhere is never written through */
    tu_assert_fails (s, "shell.fs.write{ path = data_dir .. '/link-dangling', contents = 'x' }",
                     SHELL_LUA_ERROR_RUNTIME, "requires the \"fs\" capability");
    g_assert_false (g_file_test ("/nonexistent-gnome-lua-target", G_FILE_TEST_EXISTS));
  }

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* exists, info (with the capability)                                        */
/* ------------------------------------------------------------------------ */

static void
test_exists_info (void)
{
  Env *env = env_new ();
  ShellLuaState *s = env_enable (env, "fsinfo@test", "[\"fs\"]");
  g_autofree char *file = scratch_path (env, "file.txt");
  g_autofree char *dir = scratch_path (env, "dir");
  g_autofree char *link = scratch_path (env, "link");
  g_autofree char *dangling = scratch_path (env, "dangling");
  gint64 now = g_get_real_time ();

  put (env, "file.txt", "twelve bytes", -1);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_assert_cmpint (symlink (file, link), ==, 0);
  g_assert_cmpint (symlink ("/nonexistent-gnome-lua-target", dangling), ==, 0);

  do_f (s, "F = '%s'; D = '%s'; L = '%s'; X = '%s'", file, dir, link, dangling);

  tu_assert_eval_true (s, "shell.fs.exists(F) == true and shell.fs.exists(D) == true");
  tu_assert_eval_true (s, "shell.fs.exists(L) == true");                 /* follows links */
  tu_assert_eval_true (s, "shell.fs.exists(X) == false");                /* a broken one */
  tu_assert_eval_true (s, "shell.fs.exists(D .. '/nope') == false");
  tu_assert_eval_true (s, "shell.fs.exists(F .. '/x') == false");        /* a file is not a directory */

  tu_assert_eval_str (s, "shell.fs.info(F).type", "file");
  tu_assert_eval_str (s, "shell.fs.info(F).size", "12");
  tu_assert_eval_str (s, "math.type(shell.fs.info(F).size)", "integer");
  tu_assert_eval_str (s, "math.type(shell.fs.info(F).modified)", "float");
  tu_assert_eval_str (s, "shell.fs.info(D).type", "directory");
  tu_assert_eval_str (s, "shell.fs.info(L).type", "symlink");            /* the link itself */
  tu_assert_eval_str (s, "shell.fs.info(X).type", "symlink");
  tu_assert_eval_str (s, "shell.fs.info('/dev/null').type", "special");

  /* modified is seconds since the epoch, with a fraction */
  {
    g_autofree char *expr = g_strdup_printf ("math.abs(shell.fs.info(F).modified - %f) < 30",
                                             (double) now / 1e6);

    tu_assert_eval_true (s, expr);
  }

  /* nil when it does not exist: one value; unknown fields are nil */
  tu_assert_eval_true (s, "shell.fs.info(D .. '/nope') == nil");
  tu_assert_eval_str (s, "select('#', shell.fs.info(D .. '/nope'))", "1");
  tu_assert_eval_true (s, "shell.fs.info(F).nothing == nil");
  /* not a directory: an environmental failure, nil and a message */
  tu_assert_eval_str (s, "select('#', shell.fs.info(F .. '/x'))", "2");
  tu_assert_eval_true (s, "type(select(2, shell.fs.info(F .. '/x'))) == 'string'");

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* read                                                                      */
/* ------------------------------------------------------------------------ */

static void
test_read (void)
{
  Env *env = env_new ();
  ShellLuaState *s = env_enable (env, "fsread@test", "[\"fs\"]");
  g_autofree char *big = NULL;
  gsize i;

  put (env, "small.txt", "hello\n", -1);
  put (env, "binary.dat", "a\0b\xff\xfe", 5);
  put (env, "empty", "", 0);
  put (env, "six", "123456", -1);
  big = g_malloc (700000);
  for (i = 0; i < 700000; i++)
    big[i] = (char) ('a' + i % 26);
  put (env, "big", big, 700000);
  put (env, "dir/x", "x", -1);

  /* success, and never synchronous */
  set_p (s, env, "small.txt");
  do_f (s, RESET "t = shell.fs.read{ path = P, " CBS " }");
  tu_assert_eval_true (s, "got == nil and err == nil");
  tu_assert_eval_true (s, "t.alive == true");
  tu_assert_eval_true (s, "tostring(t):find('^<shell%.Task ') ~= nil");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "got", "hello\n");
  tu_assert_eval_str (s, "n_done .. n_err", "10");
  tu_assert_eval_true (s, "t.alive == false");
  tu_do (s, "t:cancel()");                          /* harmless on a finished task */
  tu_spin (30);
  tu_assert_eval_str (s, "n_done .. n_err", "10");

  /* raw bytes */
  set_p (s, env, "binary.dat");
  do_f (s, RESET "shell.fs.read{ path = P, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "#got", "5");
  tu_assert_eval_true (s, "got == 'a\\0b\\xff\\xfe'");

  /* empty */
  set_p (s, env, "empty");
  do_f (s, RESET "shell.fs.read{ path = P, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_true (s, "got == ''");

  /* more than one chunk */
  set_p (s, env, "big");
  do_f (s, RESET "shell.fs.read{ path = P, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "#got", "700000");
  tu_assert_eval_true (s, "got == ('abcdefghijklmnopqrstuvwxyz'):rep(26924):sub(1, 700000)");

  /* max_bytes: exactly the size passes, one less is too-large */
  set_p (s, env, "six");
  do_f (s, RESET "shell.fs.read{ path = P, max_bytes = 6, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "got", "123456");
  do_f (s, RESET "shell.fs.read{ path = P, max_bytes = 5, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_true (s, "got == nil and err.code == 'too-large' and type(err.message) == 'string'");
  tu_assert_eval_str (s, "n_done .. n_err", "01");
  set_p (s, env, "big");
  do_f (s, RESET "shell.fs.read{ path = P, max_bytes = 1000, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "err.code", "too-large");

  /* not found, a directory */
  set_p (s, env, "nothing");
  do_f (s, RESET "shell.fs.read{ path = P, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_true (s, "got == nil and err.code == 'not-found' and err.message:find('nothing', 1, true) ~= nil");
  tu_assert_eval_str (s, "n_done .. n_err", "01");
  set_p (s, env, "dir");
  do_f (s, RESET "shell.fs.read{ path = P, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "err.code", "other");

  /* permission denied (root reads everything) */
  if (geteuid () != 0)
    {
      g_autofree char *path = scratch_path (env, "secret");

      put (env, "secret", "x", -1);
      g_assert_cmpint (g_chmod (path, 0), ==, 0);
      set_p (s, env, "secret");
      do_f (s, RESET "shell.fs.read{ path = P, " CBS " }");
      wait_lua (s, "n_done + n_err > 0");
      tu_assert_eval_str (s, "err.code", "permission-denied");
    }

  /* without on_error a failure is logged at message level and is not an
   * error of the extension */
  set_p (s, env, "nothing");
  g_test_expect_message ("shell-lua/fsread@test", G_LOG_LEVEL_MESSAGE, "*shell.fs.read*nothing*failed*");
  do_f (s, RESET "shell.fs.read{ path = P, on_done = function(c) got = c end }");
  tu_spin (150);
  g_test_assert_expected_messages ();
  tu_assert_eval_true (s, "got == nil");
  g_assert_cmpuint (shell_lua_extension_get_error_count (shell_lua_extension_manager_lookup (env->manager, "fsread@test")), ==, 0);

  /* the callback may start the next read */
  set_p (s, env, "small.txt");
  do_f (s, RESET
        "shell.fs.read{ path = P, on_done = function(a)\n"
        "  shell.fs.read{ path = P, " CBS " }\n"
        "end }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "got", "hello\n");

  /* option errors */
  fails_f (s, "missing required field 'on_done'", "shell.fs.read{ path = P }");
  fails_f (s, "missing required field 'path'", "shell.fs.read{ on_done = function() end }");
  fails_f (s, "unknown field 'bogus'", "shell.fs.read{ path = P, on_done = function() end, bogus = 1 }");
  fails_f (s, "field 'on_done' must be a function, got number", "shell.fs.read{ path = P, on_done = 5 }");
  fails_f (s, "field 'on_error' must be a function, got string",
           "shell.fs.read{ path = P, on_done = function() end, on_error = 'x' }");
  fails_f (s, "field 'max_bytes' must be between 1 and 67108864",
           "shell.fs.read{ path = P, on_done = function() end, max_bytes = 0 }");
  fails_f (s, "field 'max_bytes' must be between 1 and 67108864",
           "shell.fs.read{ path = P, on_done = function() end, max_bytes = 70000000 }");
  fails_f (s, "field 'max_bytes' must be an integer",
           "shell.fs.read{ path = P, on_done = function() end, max_bytes = 'x' }");
  fails_f (s, "options table expected", "shell.fs.read('x')");
  fails_f (s, "options table expected", "shell.fs.read()");

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* write                                                                     */
/* ------------------------------------------------------------------------ */

static guint
count_entries (const char *dir)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  guint n = 0;

  g_assert_nonnull (d);
  while (g_dir_read_name (d) != NULL)
    n++;
  g_dir_close (d);
  return n;
}

static void
test_write (void)
{
  Env *env = env_new ();
  ShellLuaState *s = env_enable (env, "fswrite@test", "[\"fs\"]");
  g_autofree char *file = scratch_path (env, "w/out.txt");
  g_autofree char *dir = scratch_path (env, "w");
  g_autofree char *contents = NULL;
  gsize length;

  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  do_f (s, "F = '%s'; D = '%s'", file, dir);

  /* a new file; no callback needed, with one it comes later */
  do_f (s, RESET "t = shell.fs.write{ path = F, contents = 'one', " CBS " }");
  tu_assert_eval_true (s, "t.alive and n_done == 0");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_true (s, "got == nil and n_done == 1 and n_err == 0 and not t.alive");
  contents = slurp (file, &length);
  g_assert_cmpstr (contents, ==, "one");
  g_clear_pointer (&contents, g_free);

  /* replaced atomically: the content is the new one, and nothing else is left */
  do_f (s, RESET "shell.fs.write{ path = F, contents = 'a much longer second version\\0\\1', " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  contents = slurp (file, &length);
  g_assert_cmpuint (length, ==, 30);
  g_assert_cmpint (memcmp (contents, "a much longer second version\0\1", 30), ==, 0);
  g_clear_pointer (&contents, g_free);
  g_assert_cmpuint (count_entries (dir), ==, 1);

  do_f (s, RESET "shell.fs.write{ path = F, contents = '', " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  contents = slurp (file, &length);
  g_assert_cmpuint (length, ==, 0);
  g_clear_pointer (&contents, g_free);

  /* append: adds, and creates */
  do_f (s, RESET "shell.fs.write{ path = F, contents = 'abc', " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  do_f (s, RESET "shell.fs.write{ path = F, contents = 'def', append = true, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "n_done .. n_err", "10");
  contents = slurp (file, &length);
  g_assert_cmpstr (contents, ==, "abcdef");
  g_clear_pointer (&contents, g_free);
  {
    g_autofree char *fresh = scratch_path (env, "w/fresh.txt");

    do_f (s, RESET "shell.fs.write{ path = '%s', contents = 'new', append = true, " CBS " }", fresh);
    wait_lua (s, "n_done + n_err > 0");
    contents = slurp (fresh, &length);
    g_assert_cmpstr (contents, ==, "new");
    g_clear_pointer (&contents, g_free);
  }

  /* a missing parent is an error outside the data directory, even with fs */
  {
    g_autofree char *missing = scratch_path (env, "w/missing/deeper/x.txt");

    do_f (s, RESET "shell.fs.write{ path = '%s', contents = 'x', " CBS " }", missing);
    wait_lua (s, "n_done + n_err > 0");
    tu_assert_eval_str (s, "err.code", "not-found");
    g_assert_false (g_file_test (missing, G_FILE_TEST_EXISTS));
    do_f (s, RESET "shell.fs.write{ path = '%s', contents = 'x', append = true, " CBS " }", missing);
    wait_lua (s, "n_done + n_err > 0");
    tu_assert_eval_str (s, "err.code", "not-found");
  }

  /* a directory cannot be written */
  do_f (s, RESET "shell.fs.write{ path = D, contents = 'x', " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_true (s, "err ~= nil and (err.code == 'other' or err.code == 'permission-denied')");

  /* the data directory gets its parents, with or without the capability */
  {
    g_autofree char *nested = g_build_filename (env->data_home, "gnome-lua", "fswrite@test", "a", "b", "c.txt", NULL);

    do_f (s, RESET "shell.fs.write{ path = shell.extension.data_dir .. '/a/b/c.txt', contents = 'deep', " CBS " }");
    wait_lua (s, "n_done + n_err > 0");
    tu_assert_eval_str (s, "n_done .. n_err", "10");
    contents = slurp (nested, &length);
    g_assert_cmpstr (contents, ==, "deep");
    g_clear_pointer (&contents, g_free);
  }

  /* a big one, over several writes of the stream */
  {
    g_autofree char *bigpath = scratch_path (env, "w/big");

    do_f (s, RESET "shell.fs.write{ path = '%s', contents = ('x'):rep(3000000), append = true, " CBS " }", bigpath);
    wait_lua (s, "n_done + n_err > 0");
    tu_assert_eval_str (s, "n_done .. n_err", "10");
    contents = slurp (bigpath, &length);
    g_assert_cmpuint (length, ==, 3000000);
    g_clear_pointer (&contents, g_free);
  }

  /* fire and forget: no callbacks at all */
  {
    g_autofree char *quiet = scratch_path (env, "w/quiet.txt");

    do_f (s, "shell.fs.write{ path = '%s', contents = 'q' }", quiet);
    tu_spin (100);
    contents = slurp (quiet, &length);
    g_assert_cmpstr (contents, ==, "q");
    g_clear_pointer (&contents, g_free);
  }

  /* options */
  fails_f (s, "missing required field 'contents'", "shell.fs.write{ path = F }");
  fails_f (s, "field 'contents' must be a string, got number", "shell.fs.write{ path = F, contents = 5 }");
  fails_f (s, "field 'append' must be a boolean, got string",
           "shell.fs.write{ path = F, contents = '', append = 'yes' }");
  fails_f (s, "unknown field 'mode'", "shell.fs.write{ path = F, contents = '', mode = 0 }");
  fails_f (s, "field 'on_done' must be a function, got boolean",
           "shell.fs.write{ path = F, contents = '', on_done = true }");

  env_free (env);
}

/* without the capability: the whole life cycle inside the data directory */
static void
test_data_dir_round_trip (void)
{
  Env *env = env_new ();
  ShellLuaState *s = env_enable (env, "fsrt@test", NULL);

  tu_do (s, "D = shell.extension.data_dir");

  do_f (s, RESET "shell.fs.write{ path = D .. '/sub/one.txt', contents = '1', " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "n_done .. n_err", "10");
  do_f (s, RESET "shell.fs.write{ path = D .. '/two.txt', contents = '22', " CBS " }");
  wait_lua (s, "n_done + n_err > 0");

  do_f (s, RESET "shell.fs.read{ path = D .. '/sub/one.txt', " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "got", "1");

  do_f (s, RESET "shell.fs.list{ path = D, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "#got", "2");
  tu_assert_eval_str (s, "got[1].name .. ':' .. got[1].type .. ' ' .. got[2].name .. ':' .. got[2].type",
                      "sub:directory two.txt:file");

  tu_assert_eval_str (s, "shell.fs.info(D .. '/two.txt').size", "2");

  /* own directory: readable, listable */
  do_f (s, RESET "shell.fs.list{ path = shell.extension.dir, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "got[1].name .. ' ' .. got[2].name", "extension.lua metadata.json");
  do_f (s, RESET "shell.fs.read{ path = shell.extension.dir .. '/metadata.json', " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_true (s, "got:find('fsrt@test', 1, true) ~= nil");

  /* remove */
  do_f (s, RESET "shell.fs.remove{ path = D .. '/two.txt', " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_true (s, "shell.fs.exists(D .. '/two.txt') == false");

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* list, remove                                                              */
/* ------------------------------------------------------------------------ */

static void
test_list (void)
{
  Env *env = env_new ();
  ShellLuaState *s = env_enable (env, "fslist@test", "[\"fs\"]");
  g_autofree char *link = scratch_path (env, "d/c-link");
  g_autofree char *target = scratch_path (env, "d/a-file");

  put (env, "d/b-file", "b", -1);
  put (env, "d/a-file", "a", -1);
  put (env, "d/sub/inner", "i", -1);
  put (env, "d/Z-upper", "z", -1);
  put (env, "e/.hidden", "h", -1);
  g_assert_cmpint (symlink (target, link), ==, 0);

  set_p (s, env, "d");
  do_f (s, RESET "t = shell.fs.list{ path = P, " CBS " }");
  tu_assert_eval_true (s, "got == nil and t.alive");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "n_done .. n_err", "10");
  tu_assert_eval_str (s, "#got", "5");
  /* bytewise order; the link is a link */
  tu_assert_eval_str (s,
                      "(function() local r = {} for _, e in ipairs(got) do r[#r + 1] = e.name .. '=' .. e.type end "
                      "return table.concat(r, ' ') end)()",
                      "Z-upper=file a-file=file b-file=file c-link=symlink sub=directory");

  /* hidden files are listed, . and .. are not */
  set_p (s, env, "e");
  do_f (s, RESET "shell.fs.list{ path = P, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "#got .. got[1].name", "1.hidden");

  /* an empty directory */
  {
    g_autofree char *empty = scratch_path (env, "empty");

    g_assert_cmpint (g_mkdir_with_parents (empty, 0700), ==, 0);
    set_p (s, env, "empty");
    do_f (s, RESET "shell.fs.list{ path = P, " CBS " }");
    wait_lua (s, "n_done + n_err > 0");
    tu_assert_eval_str (s, "type(got) .. #got", "table0");
  }

  /* more than one batch */
  {
    guint i;

    for (i = 0; i < 600; i++)
      {
        g_autofree char *name = g_strdup_printf ("many/f%04u", i);

        put (env, name, "", 0);
      }
  }
  set_p (s, env, "many");
  do_f (s, RESET "shell.fs.list{ path = P, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "#got", "600");
  tu_assert_eval_str (s, "got[1].name .. got[600].name", "f0000f0599");

  /* errors */
  set_p (s, env, "nothing");
  do_f (s, RESET "shell.fs.list{ path = P, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "err.code", "not-found");
  set_p (s, env, "d/a-file");
  do_f (s, RESET "shell.fs.list{ path = P, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "err.code", "other");

  fails_f (s, "missing required field 'on_done'", "shell.fs.list{ path = P }");

  env_free (env);
}

static void
test_remove (void)
{
  Env *env = env_new ();
  ShellLuaState *s = env_enable (env, "fsremove@test", "[\"fs\"]");
  g_autofree char *target = scratch_path (env, "keep");
  g_autofree char *link = scratch_path (env, "link");
  g_autofree char *emptydir = scratch_path (env, "emptydir");

  put (env, "file", "x", -1);
  put (env, "full/x", "x", -1);
  put (env, "keep", "kept", -1);
  g_assert_cmpint (g_mkdir_with_parents (emptydir, 0700), ==, 0);
  g_assert_cmpint (symlink (target, link), ==, 0);

  set_p (s, env, "file");
  do_f (s, RESET "t = shell.fs.remove{ path = P, " CBS " }");
  tu_assert_eval_true (s, "t.alive and n_done == 0");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "n_done .. n_err", "10");
  tu_assert_eval_true (s, "shell.fs.exists(P) == false and not t.alive");

  /* gone: not found */
  do_f (s, RESET "shell.fs.remove{ path = P, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "err.code", "not-found");

  /* an empty directory goes, one with content stays */
  set_p (s, env, "emptydir");
  do_f (s, RESET "shell.fs.remove{ path = P, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "n_done .. n_err", "10");
  tu_assert_eval_true (s, "shell.fs.exists(P) == false");
  set_p (s, env, "full");
  do_f (s, RESET "shell.fs.remove{ path = P, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "err.code", "other");
  tu_assert_eval_true (s, "shell.fs.exists(P .. '/x')");

  /* a link goes, not its target */
  set_p (s, env, "link");
  do_f (s, RESET "shell.fs.remove{ path = P, " CBS " }");
  wait_lua (s, "n_done + n_err > 0");
  tu_assert_eval_str (s, "n_done .. n_err", "10");
  g_assert_false (g_file_test (link, G_FILE_TEST_IS_SYMLINK));
  g_assert_true (g_file_test (target, G_FILE_TEST_EXISTS));

  /* fire and forget */
  {
    g_autofree char *quiet = scratch_path (env, "quiet");

    put (env, "quiet", "q", -1);
    do_f (s, "shell.fs.remove{ path = '%s' }", quiet);
    tu_spin (100);
    g_assert_false (g_file_test (quiet, G_FILE_TEST_EXISTS));
  }

  fails_f (s, "missing required field 'path'", "shell.fs.remove{}");
  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* cancel                                                                    */
/* ------------------------------------------------------------------------ */

static void
test_cancel (void)
{
  Env *env = env_new ();
  ShellLuaState *s = env_enable (env, "fscancel@test", "[\"fs\"]");
  g_autofree char *zeros = g_malloc0 (8 * 1024 * 1024);

  put (env, "big", zeros, 8 * 1024 * 1024);
  set_p (s, env, "big");

  do_f (s, RESET "t = shell.fs.read{ path = P, " CBS " }\n"
        "t:cancel()");
  /* the handle is dead at once, the callback comes later and says why */
  tu_assert_eval_true (s, "t.alive == false and n_done == 0 and n_err == 0");
  wait_lua (s, "n_err > 0");
  tu_assert_eval_true (s, "got == nil and err.code == 'cancelled' and type(err.message) == 'string'");
  tu_do (s, "t:cancel(); t:cancel()");
  tu_spin (100);
  tu_assert_eval_str (s, "n_done .. n_err", "01");

  /* a task cancelled without on_error just ends */
  do_f (s, RESET "t = shell.fs.read{ path = P, on_done = function(c) got = c end }\n"
        "t:cancel()");
  tu_spin (100);
  tu_assert_eval_true (s, "got == nil and not t.alive");

  /* cancelling frees the slot at once */
  do_f (s, "for i = 1, 20 do local x = shell.fs.read{ path = P, on_done = function() end }; x:cancel() end");

  /* cancel from the callback of another operation */
  do_f (s, RESET "a = shell.fs.read{ path = P, on_done = function() end, on_error = function() n_err = n_err + 1 end }\n"
        "b = shell.fs.list{ path = shell.extension.dir, on_done = function() a:cancel(); n_done = n_done + 1 end }");
  wait_lua (s, "n_done > 0");
  tu_spin (100);
  tu_assert_eval_true (s, "n_done == 1 and n_err <= 1");

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* watch                                                                     */
/* ------------------------------------------------------------------------ */

static void
touch (const char *path, const char *contents, int flags)
{
  int fd = g_open (path, O_WRONLY | O_CREAT | flags, 0600);

  g_assert_cmpint (fd, >=, 0);
  g_assert_cmpint (write (fd, contents, strlen (contents)), ==, (gssize) strlen (contents));
  close (fd);
}

#define EV_PRELUDE \
  "ev = {}\n" \
  "function has(s) for _, e in ipairs(ev) do if e == s then return true end end return false end\n"

static void
test_watch (void)
{
  Env *env = env_new ();
  ShellLuaState *s = env_enable (env, "fswatch@test", "[\"fs\"]");
  g_autofree char *dir = scratch_path (env, "w");
  g_autofree char *file = scratch_path (env, "w/a.txt");
  g_autofree char *renamed = scratch_path (env, "w/b.txt");
  g_autofree char *single = scratch_path (env, "single.txt");
  g_autofree char *later = scratch_path (env, "later.txt");
  int before;

  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  put (env, "single.txt", "start", -1);

  do_f (s, EV_PRELUDE
        "W = '%s'\n"
        "w = shell.fs.watch{ path = W, on_changed = function(e, p, o) ev[#ev + 1] = e .. ' ' .. p .. (o and (' -> ' .. o) or '') end }",
        dir);
  tu_assert_eval_true (s, "w.alive and tostring(w):find('^<shell%.FileWatch ') ~= nil");
  tu_assert_eval_str (s, "w.path", dir);

  /* a directory: entries are reported by path */
  touch (file, "x", 0);
  do_f (s, "created = 'created %s'", file);
  wait_lua (s, "has(created)");

  g_assert_cmpint (g_rename (file, renamed), ==, 0);
  do_f (s, "renamed = 'renamed %s -> %s'; gone_old = 'deleted %s'", file, renamed, file);
  wait_lua (s, "has(renamed) or has(gone_old)");

  g_assert_cmpint (g_unlink (renamed), ==, 0);
  do_f (s, "gone = 'deleted %s'", renamed);
  wait_lua (s, "has(gone)");

  /* the callback gets data only: no handle */
  tu_do (s, "seen = nil; w2 = shell.fs.watch{ path = W, on_changed = function(...) seen = select('#', ...) end }");
  touch (file, "again", 0);
  wait_lua (s, "seen ~= nil");
  tu_assert_eval_true (s, "seen == 2 or seen == 3");

  /* disconnect: alive is false, nothing arrives */
  tu_do (s, "w2:disconnect(); w2:disconnect(); w2:destroy()");
  tu_assert_eval_true (s, "w2.alive == false");
  tu_do (s, "w:destroy()");
  tu_assert_eval_true (s, "w.alive == false");
  tu_assert_eval_str (s, "tostring(select(2, pcall(function() return w.path end)))"
                         ":find('no longer valid', 1, true) ~= nil and 'ok' or 'bad'", "ok");
  tu_do (s, "n_before = #ev; seen = nil");
  touch (file, "silent", O_APPEND);
  g_unlink (file);
  tu_spin (300);
  tu_assert_eval_true (s, "#ev == n_before and seen == nil");

  /* a single file: its content changes */
  tu_do (s, "ev = {}");
  do_f (s, "S = '%s'; ws = shell.fs.watch{ path = S, on_changed = function(e, p) ev[#ev + 1] = e .. ' ' .. p end }", single);
  touch (single, "more", O_APPEND);
  do_f (s, "changed = 'changed %s'", single);
  wait_lua (s, "has(changed)");
  tu_do (s, "ws:disconnect()");

  /* a path that does not exist yet can be watched */
  tu_do (s, "ev = {}");
  do_f (s, "wl = shell.fs.watch{ path = '%s', on_changed = function(e, p) ev[#ev + 1] = e .. ' ' .. p end }", later);
  tu_assert_eval_true (s, "wl ~= nil and wl.alive");
  touch (later, "now", 0);
  do_f (s, "appeared = 'created %s'", later);
  wait_lua (s, "has(appeared)");

  /* watches are owned: they count, and are gone when the extension is */
  tu_do (s, "ev = {}");
  before = bumps;
  tu_do (s, "wb = shell.fs.watch{ path = W, on_changed = function() bump() end }");
  g_assert_true (shell_lua_extension_manager_disable (env->manager, "fswatch@test", NULL));
  touch (file, "after disable", 0);
  tu_spin (300);
  g_assert_cmpint (bumps, ==, before);

  /* misuse */
  {
    ShellLuaState *s2 = env_enable (env, "fswatch2@test", "[\"fs\"]");

    fails_f (s2, "missing required field 'on_changed'", "shell.fs.watch{ path = '/tmp' }");
    fails_f (s2, "unknown field 'recursive'", "shell.fs.watch{ path = '/tmp', on_changed = function() end, recursive = true }");
    fails_f (s2, "field 'on_changed' must be a function, got string",
             "shell.fs.watch{ path = '/tmp', on_changed = 'x' }");
    fails_f (s2, "must be an absolute path", "shell.fs.watch{ path = 'tmp', on_changed = function() end }");
  }

  env_free (env);
}

/* ------------------------------------------------------------------------ */
/* Teardown                                                                  */
/* ------------------------------------------------------------------------ */

static void
test_teardown (void)
{
  Env *env = env_new ();
  ShellLuaState *s = env_enable (env, "fsteardown@test", "[\"fs\"]");
  g_autofree char *zeros = g_malloc0 (4 * 1024 * 1024);
  g_autofree char *watched = scratch_path (env, "watched");

  put (env, "big", zeros, 4 * 1024 * 1024);
  g_assert_cmpint (g_mkdir_with_parents (watched, 0700), ==, 0);
  set_p (s, env, "big");
  do_f (s, "W = '%s'", watched);

  /* pending work of every kind, and a watch, when the extension is disabled */
  tu_do (s,
         "shell.fs.read{ path = P, on_done = function() bump() end, on_error = function() bump() end }\n"
         "shell.fs.list{ path = W, on_done = function() bump() end, on_error = function() bump() end }\n"
         "shell.fs.write{ path = shell.extension.data_dir .. '/x', contents = 'x', on_done = function() bump() end,\n"
         "                on_error = function() bump() end }\n"
         "shell.fs.remove{ path = P .. '.nope', on_done = function() bump() end, on_error = function() bump() end }\n"
         "c = shell.fs.read{ path = P, on_done = function() bump() end, on_error = function() bump() end }\n"
         "c:cancel()\n"
         "shell.fs.watch{ path = W, on_changed = function() bump() end }");
  g_assert_cmpint (bumps, ==, 0);
  g_assert_true (shell_lua_extension_manager_disable (env->manager, "fsteardown@test", NULL));
  {
    g_autofree char *late = g_build_filename (watched, "late", NULL);

    touch (late, "x", 0);
  }
  tu_spin (400);
  g_assert_cmpint (bumps, ==, 0);

  /* enabling again starts clean */
  s = env_enable (env, "fsteardown@test", "[\"fs\"]");
  tu_assert_eval_true (s, "shell.fs.exists(shell.extension.data_dir)");

  env_free (env);
}

/* the 4096 cap counts pending operations, not finished ones */
static void
test_object_cap (void)
{
  Env *env = env_new ();
  ShellLuaState *s = env_enable (env, "fscap@test", "[\"fs\"]");

  set_p (s, env, "nothing");
  tu_do (s, "ts = {}; cnt = 0\n"
            "for i = 1, 4096 do ts[i] = shell.fs.read{ path = P, on_done = function() end, on_error = function() cnt = cnt + 1 end } end");
  tu_assert_fails (s, "shell.fs.read{ path = P, on_done = function() end }", SHELL_LUA_ERROR_RUNTIME,
                   "too many objects");
  /* they all fail (not found) and free their slots */
  wait_lua (s, "cnt == 4096");
  tu_do (s, "shell.fs.read{ path = P, on_done = function() end, on_error = function() end }");

  env_free (env);
}


/* ------------------------------------------------------------------------ */
/* The fixtures of the conformance test (tests/api/fixtures/fs@test, ...)    */
/* ------------------------------------------------------------------------ */

/* A stand-in for the shell.test API of src/shell-test-runner.c */
typedef struct {
  char     *name;
  gboolean  ok;
  char     *message;
} Report;

static GPtrArray  *reports = NULL;
static GHashTable *done_labels = NULL;

static void
report_free (gpointer data)
{
  Report *r = data;

  g_free (r->name);
  g_free (r->message);
  g_free (r);
}

static int
cf_report (lua_State *L)
{
  Report *r = g_new0 (Report, 1);

  r->name = g_strdup (luaL_checkstring (L, 1));
  r->ok = lua_toboolean (L, 2);
  r->message = g_strdup (luaL_optstring (L, 3, NULL));
  g_ptr_array_add (reports, r);
  return 0;
}

static int
cf_done (lua_State *L)
{
  g_hash_table_add (done_labels, g_strdup (luaL_optstring (L, 1, "main")));
  return 0;
}

static void
test_api_register (ShellLuaState *state, lua_State *L)
{
  lua_newtable (L);
  lua_pushcfunction (L, cf_report);
  lua_setfield (L, -2, "report");
  lua_pushcfunction (L, cf_done);
  lua_setfield (L, -2, "done");
  lua_setfield (L, -2, "test");
}

static gboolean
pred_done (gpointer data)
{
  return g_hash_table_contains (done_labels, data);
}

static void
check_reports (const char *what,
               guint       minimum)
{
  GString *failures = g_string_new (NULL);
  guint i;

  for (i = 0; i < reports->len; i++)
    {
      Report *r = g_ptr_array_index (reports, i);

      if (!r->ok)
        g_string_append_printf (failures, "\n  %s: %s", r->name, r->message != NULL ? r->message : "failed");
    }
  if (failures->len > 0)
    g_error ("%s: checks failed (of %u):%s", what, reports->len, failures->str);
  g_assert_cmpuint (reports->len, >=, minimum);
  g_string_free (failures, TRUE);
}

static const char *
reported_message (const char *name)
{
  guint i;

  for (i = 0; i < reports->len; i++)
    {
      Report *r = g_ptr_array_index (reports, i);

      if (strcmp (r->name, name) == 0)
        return r->message;
    }
  return NULL;
}

/* Installs the fixtures of tests/api/fixtures in @user. */
static void
install_fixtures (const char *user, const char * const *uuids)
{
  g_autofree char *src = g_build_filename (g_getenv ("G_TEST_SRCDIR"), "..", "..", "..", "tests", "api",
                                           "fixtures", NULL);
  g_autoptr (GError) error = NULL;
  guint i;

  for (i = 0; uuids[i] != NULL; i++)
    {
      g_autofree char *metadata_path = g_build_filename (src, uuids[i], "metadata.json", NULL);
      g_autofree char *lua_path = g_build_filename (src, uuids[i], "extension.lua", NULL);
      g_autofree char *metadata = NULL;
      g_autofree char *lua = NULL;
      g_autofree char *dest = g_build_filename (user, uuids[i], NULL);

      g_assert_true (g_file_get_contents (metadata_path, &metadata, NULL, &error));
      g_assert_no_error (error);
      g_assert_true (g_file_get_contents (lua_path, &lua, NULL, &error));
      g_assert_no_error (error);
      tu_write_file (dest, "metadata.json", metadata);
      tu_write_file (dest, "extension.lua", lua);
    }
}

static void
test_fixtures (void)
{
  Env *env = env_new ();
  g_autoptr (GError) error = NULL;
  const char *uuids[] = { "fs@test", "fs-nocap@test", NULL };
  guint cycle;

  install_fixtures (env->user, uuids);
  shell_lua_extension_manager_scan (env->manager);
  g_assert_nonnull (shell_lua_extension_manager_lookup (env->manager, "fs@test"));

  reports = g_ptr_array_new_with_free_func (report_free);
  done_labels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* without the capability: the extension's own directories only */
  for (cycle = 0; cycle < 2; cycle++)
    {
      g_assert_true (shell_lua_extension_manager_enable (env->manager, "fs-nocap@test", &error));
      g_assert_no_error (error);
      g_assert_true (tu_wait_for (pred_done, (gpointer) "main", 10000));
      check_reports ("fs-nocap@test", 9);
      g_assert_cmpuint (shell_lua_extension_get_error_count (shell_lua_extension_manager_lookup (env->manager, "fs-nocap@test")), ==, 0);
      g_assert_true (shell_lua_extension_manager_disable (env->manager, "fs-nocap@test", &error));
      g_assert_no_error (error);
      g_ptr_array_set_size (reports, 0);
      g_hash_table_remove_all (done_labels);
    }

  /* the conformance fixture, twice */
  for (cycle = 0; cycle < 2; cycle++)
    {
      ShellLuaExtension *ext = shell_lua_extension_manager_lookup (env->manager, "fs@test");
      g_autofree char *persist = NULL;
      g_autofree char *poke = NULL;
      g_autofree char *contents = NULL;
      guint n_before;

      g_assert_true (shell_lua_extension_manager_enable (env->manager, "fs@test", &error));
      g_assert_no_error (error);
      g_assert_true (tu_wait_for (pred_done, (gpointer) "main", 30000));
      check_reports ("fs@test", 12);
      g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 0);
      g_assert_nonnull (reported_message ("data-dir"));
      persist = g_build_filename (reported_message ("data-dir"), "persist.txt", NULL);
      poke = g_build_filename (reported_message ("data-dir"), "keep", "poke", NULL);

      g_assert_true (shell_lua_extension_manager_disable (env->manager, "fs@test", &error));
      g_assert_no_error (error);

      /* the data directory stays; nothing runs any more, the watch is gone */
      contents = slurp (persist, NULL);
      g_assert_cmpstr (contents, ==, "kept");
      n_before = reports->len;
      touch (poke, "x", O_APPEND);
      tu_spin (400);
      g_assert_cmpuint (reports->len, ==, n_before);

      g_ptr_array_set_size (reports, 0);
      g_hash_table_remove_all (done_labels);
    }

  g_clear_pointer (&reports, g_ptr_array_unref);
  g_clear_pointer (&done_labels, g_hash_table_unref);
  env_free (env);
}

int
main (int argc, char **argv)
{
  int result;

  /* XDG_DATA_HOME before anything asks GLib for it */
  base_dir = g_dir_make_tmp ("shell-lua-fs-XXXXXX", NULL);
  g_assert_nonnull (base_dir);
  {
    g_autofree char *data_home = g_build_filename (base_dir, "data", NULL);

    g_setenv ("XDG_DATA_HOME", data_home, TRUE);
  }

  g_test_init (&argc, &argv, NULL);

  shell_lua_api_register ("zz-test", test_api_register);

  g_test_add_func ("/shell-lua/fs/data-dir", test_data_dir);
  g_test_add_func ("/shell-lua/fs/capability/no-extension", test_capability_gate_no_extension);
  g_test_add_func ("/shell-lua/fs/capability/paths", test_paths_without_capability);
  g_test_add_func ("/shell-lua/fs/exists-info", test_exists_info);
  g_test_add_func ("/shell-lua/fs/read", test_read);
  g_test_add_func ("/shell-lua/fs/write", test_write);
  g_test_add_func ("/shell-lua/fs/data-dir-round-trip", test_data_dir_round_trip);
  g_test_add_func ("/shell-lua/fs/list", test_list);
  g_test_add_func ("/shell-lua/fs/remove", test_remove);
  g_test_add_func ("/shell-lua/fs/cancel", test_cancel);
  g_test_add_func ("/shell-lua/fs/watch", test_watch);
  g_test_add_func ("/shell-lua/fs/teardown", test_teardown);
  g_test_add_func ("/shell-lua/fs/object-cap", test_object_cap);
  g_test_add_func ("/shell-lua/fs/fixtures", test_fixtures);

  result = g_test_run ();

  tu_rm_rf (base_dir);
  g_free (base_dir);
  return result;
}
