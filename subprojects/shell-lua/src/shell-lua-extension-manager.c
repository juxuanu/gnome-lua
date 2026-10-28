/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaExtensionManager: discovery, enable/disable, error policy.
 * Counterpart of ExtensionManager in js/ui/extensionSystem.js.
 */

#include <string.h>

#include "shell-lua-extension.h"
#include "shell-lua-extension-private.h"

#define SHELL_SETTINGS_SCHEMA "org.gnome.shell"
#define ENABLED_EXTENSIONS_KEY "enabled-extensions"
#define DISABLED_EXTENSIONS_KEY "disabled-extensions"
#define DISABLE_USER_EXTENSIONS_KEY "disable-user-extensions"

#define DEFAULT_ERROR_LIMIT 5
#define EXTENSION_PATH_ENV "SHELL_LUA_EXTENSION_PATH"
#define LIMIT_PENDING_KEY "shell-lua-error-limit-pending"
#define LAST_ERROR_KEY "shell-lua-last-error"

enum {
  SIGNAL_EXTENSION_STATE_CHANGED,
  SIGNAL_EXTENSIONS_LOADED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

typedef struct
{
  GFile *file;
  ShellLuaExtensionType type;
} SearchDir;

struct _ShellLuaExtensionManager
{
  GObject parent_instance;

  GPtrArray *dirs;             /* SearchDir*, highest priority first */
  GHashTable *extensions;      /* uuid -> ShellLuaExtension (owned) */
  GPtrArray *order;            /* uuids (owned), in enable order */

  guint error_limit;
  ShellLuaLimits limits;
  gboolean has_limits;

  GSettings *shell_settings;
  gboolean settings_checked;
  guint sync_idle_id;
  gboolean syncing;
  gboolean sync_again;
};

G_DEFINE_FINAL_TYPE (ShellLuaExtensionManager, shell_lua_extension_manager,
                     G_TYPE_OBJECT)

static void on_extension_state_changed (ShellLuaExtension        *extension,
                                        ShellLuaExtensionManager *self);
static void on_extension_lua_error     (ShellLuaExtension        *extension,
                                        GError                   *error,
                                        ShellLuaExtensionManager *self);

/* ------------------------------------------------------------------ */
/* Search directories                                                 */
/* ------------------------------------------------------------------ */

static void
search_dir_free (SearchDir *dir)
{
  g_object_unref (dir->file);
  g_free (dir);
}

static void
add_search_dir (ShellLuaExtensionManager *self,
                GFile                    *file,
                ShellLuaExtensionType     type)
{
  SearchDir *dir = g_new0 (SearchDir, 1);

  dir->file = g_object_ref (file);
  dir->type = type;
  g_ptr_array_add (self->dirs, dir);
}

static void
set_default_search_dirs (ShellLuaExtensionManager *self)
{
  const char *override = g_getenv (EXTENSION_PATH_ENV);
  const char * const *system_dirs;
  g_autofree char *user_path = NULL;
  g_autoptr (GFile) user_dir = NULL;
  int i;

  g_ptr_array_set_size (self->dirs, 0);

  /* Development override: every entry behaves like a per-user directory,
   * earlier entries win. */
  if (override != NULL && *override != '\0')
    {
      g_auto (GStrv) parts = g_strsplit (override, G_SEARCHPATH_SEPARATOR_S, -1);

      for (i = 0; parts[i] != NULL; i++)
        {
          g_autoptr (GFile) file = NULL;

          if (*parts[i] == '\0')
            continue;

          file = g_file_new_for_path (parts[i]);
          add_search_dir (self, file, SHELL_LUA_EXTENSION_TYPE_PER_USER);
        }
      return;
    }

  user_path = g_build_filename (g_get_user_data_dir (), "gnome-shell",
                                "extensions", NULL);
  user_dir = g_file_new_for_path (user_path);
  add_search_dir (self, user_dir, SHELL_LUA_EXTENSION_TYPE_PER_USER);

  system_dirs = g_get_system_data_dirs ();
  for (i = 0; system_dirs[i] != NULL; i++)
    {
      g_autofree char *path = g_build_filename (system_dirs[i], "gnome-shell",
                                                "extensions", NULL);
      g_autoptr (GFile) file = g_file_new_for_path (path);

      add_search_dir (self, file, SHELL_LUA_EXTENSION_TYPE_SYSTEM);
    }
}

/* The first directory is the per-user one, all others are system
 * directories (see the default list). Earlier directories shadow later ones
 * for the same uuid. */
void
shell_lua_extension_manager_set_search_dirs (ShellLuaExtensionManager *self,
                                             GFile * const            *dirs,
                                             gsize                     n_dirs)
{
  gsize i;

  g_return_if_fail (SHELL_IS_LUA_EXTENSION_MANAGER (self));

  g_ptr_array_set_size (self->dirs, 0);
  for (i = 0; i < n_dirs; i++)
    add_search_dir (self, dirs[i],
                    i == 0 ? SHELL_LUA_EXTENSION_TYPE_PER_USER
                           : SHELL_LUA_EXTENSION_TYPE_SYSTEM);
}

/* ------------------------------------------------------------------ */
/* Object                                                             */
/* ------------------------------------------------------------------ */

static void
shell_lua_extension_manager_dispose (GObject *object)
{
  ShellLuaExtensionManager *self = SHELL_LUA_EXTENSION_MANAGER (object);
  GList *extensions, *l;

  g_clear_handle_id (&self->sync_idle_id, g_source_remove);

  if (self->shell_settings != NULL)
    g_signal_handlers_disconnect_by_data (self->shell_settings, self);
  g_clear_object (&self->shell_settings);

  /* Disable in reverse order, like the JS manager on session teardown */
  if (self->order != NULL)
    {
      while (self->order->len > 0)
        {
          char *uuid = g_ptr_array_steal_index (self->order,
                                                self->order->len - 1);
          ShellLuaExtension *extension =
            g_hash_table_lookup (self->extensions, uuid);

          if (extension != NULL)
            shell_lua_extension_disable (extension, NULL);
          g_free (uuid);
        }
    }

  if (self->extensions != NULL)
    {
      extensions = g_hash_table_get_values (self->extensions);
      for (l = extensions; l != NULL; l = l->next)
        g_signal_handlers_disconnect_by_data (l->data, self);
      g_list_free (extensions);
      g_hash_table_remove_all (self->extensions);
    }

  G_OBJECT_CLASS (shell_lua_extension_manager_parent_class)->dispose (object);
}

static void
shell_lua_extension_manager_finalize (GObject *object)
{
  ShellLuaExtensionManager *self = SHELL_LUA_EXTENSION_MANAGER (object);

  g_clear_pointer (&self->dirs, g_ptr_array_unref);
  g_clear_pointer (&self->extensions, g_hash_table_unref);
  g_clear_pointer (&self->order, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_lua_extension_manager_parent_class)->finalize (object);
}

static void
shell_lua_extension_manager_class_init (ShellLuaExtensionManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_lua_extension_manager_dispose;
  object_class->finalize = shell_lua_extension_manager_finalize;

  signals[SIGNAL_EXTENSION_STATE_CHANGED] =
    g_signal_new ("extension-state-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_LUA_EXTENSION);

  signals[SIGNAL_EXTENSIONS_LOADED] =
    g_signal_new ("extensions-loaded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_lua_extension_manager_init (ShellLuaExtensionManager *self)
{
  self->dirs = g_ptr_array_new_with_free_func ((GDestroyNotify) search_dir_free);
  self->extensions = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                            g_object_unref);
  self->order = g_ptr_array_new_with_free_func (g_free);
  self->error_limit = DEFAULT_ERROR_LIMIT;

  set_default_search_dirs (self);
}

ShellLuaExtensionManager *
shell_lua_extension_manager_get_default (void)
{
  static ShellLuaExtensionManager *default_manager = NULL;

  if (default_manager == NULL)
    {
      default_manager = g_object_new (SHELL_TYPE_LUA_EXTENSION_MANAGER, NULL);
      g_object_add_weak_pointer (G_OBJECT (default_manager),
                                 (gpointer *) &default_manager);
    }

  return default_manager;
}

/* ------------------------------------------------------------------ */
/* Lookup                                                             */
/* ------------------------------------------------------------------ */

ShellLuaExtension *
shell_lua_extension_manager_lookup (ShellLuaExtensionManager *self,
                                    const char               *uuid)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION_MANAGER (self), NULL);
  g_return_val_if_fail (uuid != NULL, NULL);

  return g_hash_table_lookup (self->extensions, uuid);
}

static gint
compare_by_uuid (gconstpointer a,
                 gconstpointer b)
{
  return strcmp (shell_lua_extension_get_uuid ((ShellLuaExtension *) a),
                 shell_lua_extension_get_uuid ((ShellLuaExtension *) b));
}

static gint
compare_ptr_by_uuid (gconstpointer a,
                     gconstpointer b)
{
  return compare_by_uuid (*(gconstpointer *) a, *(gconstpointer *) b);
}

GList *
shell_lua_extension_manager_list (ShellLuaExtensionManager *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION_MANAGER (self), NULL);

  return g_list_sort (g_hash_table_get_values (self->extensions),
                      compare_by_uuid);
}

void
shell_lua_extension_manager_set_error_limit (ShellLuaExtensionManager *self,
                                             guint                     limit)
{
  g_return_if_fail (SHELL_IS_LUA_EXTENSION_MANAGER (self));
  self->error_limit = limit;
}

void
shell_lua_extension_manager_set_limits (ShellLuaExtensionManager *self,
                                        const ShellLuaLimits     *limits)
{
  GHashTableIter iter;
  gpointer value;

  g_return_if_fail (SHELL_IS_LUA_EXTENSION_MANAGER (self));

  if (limits != NULL)
    {
      self->limits = *limits;
      self->has_limits = TRUE;
    }
  else
    {
      memset (&self->limits, 0, sizeof (self->limits));
      self->has_limits = FALSE;
    }

  /* Applies at the next enable() of every extension */
  g_hash_table_iter_init (&iter, self->extensions);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    shell_lua_extension_set_limits (value, limits);
}

/* ------------------------------------------------------------------ */
/* Scanning                                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
  GFile *dir;
  ShellLuaExtensionType type;
} FoundExtension;

static void
found_extension_free (FoundExtension *found)
{
  g_object_unref (found->dir);
  g_free (found);
}

static void
scan_directory (ShellLuaExtensionManager *self,
                SearchDir                *search_dir,
                GHashTable               *found)
{
  g_autoptr (GFileEnumerator) enumerator = NULL;
  g_autoptr (GError) error = NULL;

  enumerator = g_file_enumerate_children (search_dir->file,
                                          G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                          G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                          G_FILE_QUERY_INFO_NONE,
                                          NULL, &error);
  if (enumerator == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY))
        g_warning ("Could not read extension directory: %s", error->message);
      return;
    }

  while (TRUE)
    {
      g_autoptr (GFileInfo) info = NULL;
      g_autoptr (GFile) dir = NULL;
      g_autoptr (GFile) metadata = NULL;
      g_autoptr (GFile) script = NULL;
      FoundExtension *entry;
      const char *uuid;

      info = g_file_enumerator_next_file (enumerator, NULL, &error);
      if (info == NULL)
        {
          if (error != NULL)
            g_warning ("Error while reading extension directory: %s",
                       error->message);
          break;
        }

      if (g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
        {
          /* symlinks to directories report SYMBOLIC_LINK here */
          if (g_file_info_get_file_type (info) != G_FILE_TYPE_SYMBOLIC_LINK)
            continue;
        }

      uuid = g_file_info_get_name (info);
      dir = g_file_get_child (search_dir->file, uuid);
      metadata = g_file_get_child (dir, "metadata.json");
      script = g_file_get_child (dir, "extension.lua");

      if (!g_file_query_exists (metadata, NULL) ||
          !g_file_query_exists (script, NULL))
        continue;

      /* Earlier directories shadow later ones */
      if (g_hash_table_contains (found, uuid))
        {
          g_debug ("Lua extension %s in %s is shadowed",
                   uuid, g_file_peek_path (dir));
          continue;
        }

      entry = g_new0 (FoundExtension, 1);
      entry->dir = g_steal_pointer (&dir);
      entry->type = search_dir->type;
      g_hash_table_insert (found, g_strdup (uuid), entry);
    }
}

static void
forget_extension (ShellLuaExtensionManager *self,
                  const char               *uuid)
{
  ShellLuaExtension *extension = g_hash_table_lookup (self->extensions, uuid);
  guint i;

  if (extension == NULL)
    return;

  g_object_ref (extension);

  for (i = 0; i < self->order->len; i++)
    {
      if (strcmp (g_ptr_array_index (self->order, i), uuid) == 0)
        {
          g_ptr_array_remove_index (self->order, i);
          break;
        }
    }

  /* Disables a running extension, then reports UNINSTALLED through the
   * state-changed forwarding. */
  shell_lua_extension_mark_uninstalled (extension);

  g_signal_handlers_disconnect_by_data (extension, self);
  g_hash_table_remove (self->extensions, uuid);
  g_object_unref (extension);
}

/* Creates the extension that lives in @dir and registers it; the manager
 * owns it. Returns %NULL with @error set if the directory cannot be used. */
static ShellLuaExtension *
add_extension (ShellLuaExtensionManager  *self,
               GFile                     *dir,
               ShellLuaExtensionType      type,
               GError                   **error)
{
  ShellLuaExtension *extension;

  extension = shell_lua_extension_new_from_dir (dir, type, error);
  if (extension == NULL)
    return NULL;

  if (self->has_limits)
    shell_lua_extension_set_limits (extension, &self->limits);

  g_hash_table_insert (self->extensions,
                       g_strdup (shell_lua_extension_get_uuid (extension)),
                       extension);
  g_signal_connect (extension, "state-changed",
                    G_CALLBACK (on_extension_state_changed), self);
  g_signal_connect (extension, "lua-error",
                    G_CALLBACK (on_extension_lua_error), self);

  return extension;
}

void
shell_lua_extension_manager_scan (ShellLuaExtensionManager *self)
{
  g_autoptr (GHashTable) found = NULL;
  g_autoptr (GPtrArray) gone = NULL;
  g_autoptr (GPtrArray) added = NULL;
  GHashTableIter iter;
  gpointer key, value;
  guint i;

  g_return_if_fail (SHELL_IS_LUA_EXTENSION_MANAGER (self));

  found = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                 (GDestroyNotify) found_extension_free);
  for (i = 0; i < self->dirs->len; i++)
    scan_directory (self, g_ptr_array_index (self->dirs, i), found);

  /* Extensions that disappeared, moved or changed type */
  gone = g_ptr_array_new_with_free_func (g_free);
  g_hash_table_iter_init (&iter, self->extensions);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      ShellLuaExtension *extension = value;
      FoundExtension *entry = g_hash_table_lookup (found, key);

      if (entry == NULL ||
          entry->type != shell_lua_extension_get_extension_type (extension) ||
          !g_file_equal (entry->dir, shell_lua_extension_get_dir (extension)))
        g_ptr_array_add (gone, g_strdup (key));
    }

  for (i = 0; i < gone->len; i++)
    forget_extension (self, g_ptr_array_index (gone, i));

  /* New ones */
  added = g_ptr_array_new_with_free_func (g_object_unref);
  g_hash_table_iter_init (&iter, found);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      FoundExtension *entry = value;
      g_autoptr (GError) error = NULL;
      ShellLuaExtension *extension;

      if (g_hash_table_contains (self->extensions, key))
        continue;

      extension = add_extension (self, entry->dir, entry->type, &error);
      if (extension == NULL)
        {
          g_warning ("Could not load Lua extension %s: %s",
                     (const char *) key, error->message);
          continue;
        }

      g_ptr_array_add (added, g_object_ref (extension));
    }

  g_ptr_array_sort (added, compare_ptr_by_uuid);
  for (i = 0; i < added->len; i++)
    g_signal_emit (self, signals[SIGNAL_EXTENSION_STATE_CHANGED], 0,
                   g_ptr_array_index (added, i));

  g_signal_emit (self, signals[SIGNAL_EXTENSIONS_LOADED], 0);
}

/**
 * shell_lua_extension_manager_reload:
 * @self: a #ShellLuaExtensionManager
 * @uuid: the uuid of an installed extension
 * @error: return location for a #GError
 *
 * Forgets the extension (a running one is disabled and reports UNINSTALLED)
 * and reads its directory again, so that new code, metadata and files are
 * seen. The new extension is not enabled; it is reported through
 * "extension-state-changed" like a newly scanned one.
 *
 * Returns: %TRUE if the extension was loaded again. If its directory cannot
 *   be used any more, or is gone, the extension stays forgotten and @error is
 *   set
 */
gboolean
shell_lua_extension_manager_reload (ShellLuaExtensionManager  *self,
                                    const char                *uuid,
                                    GError                   **error)
{
  g_autofree char *uuid_copy = g_strdup (uuid);
  g_autoptr (GFile) dir = NULL;
  ShellLuaExtension *extension;
  ShellLuaExtension *reloaded;
  ShellLuaExtensionType type;

  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION_MANAGER (self), FALSE);
  g_return_val_if_fail (uuid != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  extension = g_hash_table_lookup (self->extensions, uuid_copy);
  if (extension == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No Lua extension with uuid %s", uuid_copy);
      return FALSE;
    }

  dir = g_object_ref (shell_lua_extension_get_dir (extension));
  type = shell_lua_extension_get_extension_type (extension);

  forget_extension (self, uuid_copy);

  reloaded = add_extension (self, dir, type, error);
  if (reloaded == NULL)
    return FALSE;

  g_signal_emit (self, signals[SIGNAL_EXTENSION_STATE_CHANGED], 0, reloaded);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* State forwarding and error policy                                  */
/* ------------------------------------------------------------------ */

static void
on_extension_state_changed (ShellLuaExtension        *extension,
                            ShellLuaExtensionManager *self)
{
  g_signal_emit (self, signals[SIGNAL_EXTENSION_STATE_CHANGED], 0, extension);
}

typedef struct
{
  ShellLuaExtensionManager *manager;
  ShellLuaExtension *extension;
} LimitData;

static void
remove_from_order (ShellLuaExtensionManager *self,
                   const char               *uuid)
{
  guint i;

  for (i = 0; i < self->order->len; i++)
    {
      if (strcmp (g_ptr_array_index (self->order, i), uuid) == 0)
        {
          g_ptr_array_remove_index (self->order, i);
          return;
        }
    }
}

static gboolean
disable_over_limit_cb (gpointer user_data)
{
  LimitData *data = user_data;
  ShellLuaExtension *extension = data->extension;
  g_autofree char *message = NULL;
  const char *last;

  g_object_set_data (G_OBJECT (extension), LIMIT_PENDING_KEY, NULL);

  if (shell_lua_extension_get_state (extension) ==
      SHELL_LUA_EXTENSION_STATE_ACTIVE)
    {
      last = g_object_get_data (G_OBJECT (extension), LAST_ERROR_KEY);
      message = g_strdup (last != NULL ? last : "too many errors");

      g_warning ("Lua extension %s raised more than %u errors, disabling it",
                 shell_lua_extension_get_uuid (extension),
                 data->manager->error_limit);

      remove_from_order (data->manager,
                         shell_lua_extension_get_uuid (extension));
      shell_lua_extension_fail (extension, message);
    }

  g_object_unref (data->extension);
  g_object_unref (data->manager);
  g_free (data);

  return G_SOURCE_REMOVE;
}

/* The ShellLuaState "error" signal is forwarded by the extension. Counting
 * happens there; here we only apply the limit. The actual disable is deferred
 * to an idle: the error was raised inside a pcall of that very state, which
 * must not be destroyed under its feet. */
static void
on_extension_lua_error (ShellLuaExtension        *extension,
                        GError                   *error,
                        ShellLuaExtensionManager *self)
{
  LimitData *data;

  /* Keep the last message for the ERROR state and for D-Bus clients */
  if (error != NULL && error->message != NULL)
    g_object_set_data_full (G_OBJECT (extension), LAST_ERROR_KEY,
                            g_strdup (error->message), g_free);

  if (shell_lua_extension_get_state (extension) !=
      SHELL_LUA_EXTENSION_STATE_ACTIVE)
    return;

  if (shell_lua_extension_get_error_count (extension) <= self->error_limit)
    return;

  if (g_object_get_data (G_OBJECT (extension), LIMIT_PENDING_KEY) != NULL)
    return;

  g_object_set_data (G_OBJECT (extension), LIMIT_PENDING_KEY,
                     GUINT_TO_POINTER (1));

  data = g_new0 (LimitData, 1);
  data->manager = g_object_ref (self);
  data->extension = g_object_ref (extension);
  g_idle_add (disable_over_limit_cb, data);
}

/* ------------------------------------------------------------------ */
/* Enable / disable                                                   */
/* ------------------------------------------------------------------ */

gboolean
shell_lua_extension_manager_enable (ShellLuaExtensionManager  *self,
                                    const char                *uuid,
                                    GError                   **error)
{
  ShellLuaExtension *extension;

  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION_MANAGER (self), FALSE);
  g_return_val_if_fail (uuid != NULL, FALSE);

  extension = g_hash_table_lookup (self->extensions, uuid);
  if (extension == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No Lua extension with uuid %s", uuid);
      return FALSE;
    }

  if (shell_lua_extension_get_state (extension) ==
      SHELL_LUA_EXTENSION_STATE_ACTIVE)
    return TRUE;

  /* A failure (including one in the error policy) is reported through the
   * ERROR state; a manual enable gives the extension a fresh start. */
  g_object_set_data (G_OBJECT (extension), LAST_ERROR_KEY, NULL);

  if (!shell_lua_extension_enable (extension, error))
    return FALSE;

  remove_from_order (self, uuid);
  g_ptr_array_add (self->order, g_strdup (uuid));
  return TRUE;
}

gboolean
shell_lua_extension_manager_disable (ShellLuaExtensionManager  *self,
                                     const char                *uuid,
                                     GError                   **error)
{
  ShellLuaExtension *extension;

  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION_MANAGER (self), FALSE);
  g_return_val_if_fail (uuid != NULL, FALSE);

  extension = g_hash_table_lookup (self->extensions, uuid);
  if (extension == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No Lua extension with uuid %s", uuid);
      return FALSE;
    }

  remove_from_order (self, uuid);
  return shell_lua_extension_disable (extension, error);
}

/* ------------------------------------------------------------------ */
/* Sync with org.gnome.shell                                          */
/* ------------------------------------------------------------------ */

static GSettings *
get_shell_settings (ShellLuaExtensionManager *self)
{
  GSettingsSchemaSource *source;
  g_autoptr (GSettingsSchema) schema = NULL;

  if (self->shell_settings != NULL || self->settings_checked)
    return self->shell_settings;

  self->settings_checked = TRUE;

  source = g_settings_schema_source_get_default ();
  if (source != NULL)
    schema = g_settings_schema_source_lookup (source, SHELL_SETTINGS_SCHEMA,
                                              TRUE);
  if (schema == NULL)
    {
      g_debug ("Schema %s is not installed, not syncing Lua extensions",
               SHELL_SETTINGS_SCHEMA);
      return NULL;
    }

  self->shell_settings = g_settings_new_full (schema, NULL, NULL);
  return self->shell_settings;
}

static gboolean
sync_idle_cb (gpointer user_data)
{
  ShellLuaExtensionManager *self = user_data;

  self->sync_idle_id = 0;
  shell_lua_extension_manager_sync (self);

  return G_SOURCE_REMOVE;
}

static void
queue_sync (ShellLuaExtensionManager *self)
{
  if (self->sync_idle_id != 0)
    return;

  self->sync_idle_id = g_idle_add (sync_idle_cb, self);
}

static void
on_settings_changed (GSettings                *settings,
                     const char               *key,
                     ShellLuaExtensionManager *self)
{
  queue_sync (self);
}

/* _getEnabledExtensions(): enabled-extensions unless disable-user-extensions,
 * minus disabled-extensions, which takes precedence. There are no session
 * mode extensions for Lua. */
static GPtrArray *
get_enabled_extensions (GSettings *settings)
{
  g_autoptr (GPtrArray) result = g_ptr_array_new_with_free_func (g_free);
  g_auto (GStrv) enabled = NULL;
  g_auto (GStrv) disabled = NULL;
  int i;

  if (g_settings_get_boolean (settings, DISABLE_USER_EXTENSIONS_KEY))
    return g_steal_pointer (&result);

  enabled = g_settings_get_strv (settings, ENABLED_EXTENSIONS_KEY);
  disabled = g_settings_get_strv (settings, DISABLED_EXTENSIONS_KEY);

  for (i = 0; enabled[i] != NULL; i++)
    {
      if (g_strv_contains ((const char * const *) disabled, enabled[i]))
        continue;
      g_ptr_array_add (result, g_strdup (enabled[i]));
    }

  return g_steal_pointer (&result);
}

static gboolean
uuid_in_array (GPtrArray  *array,
               const char *uuid)
{
  guint i;

  for (i = 0; i < array->len; i++)
    {
      if (strcmp (g_ptr_array_index (array, i), uuid) == 0)
        return TRUE;
    }

  return FALSE;
}

static void
sync_once (ShellLuaExtensionManager *self,
           GSettings                *settings)
{
  g_autoptr (GPtrArray) wanted = get_enabled_extensions (settings);
  g_autoptr (GPtrArray) to_disable = g_ptr_array_new_with_free_func (g_free);
  GList *extensions, *l;
  gboolean mode_only;
  guint i;

  /* enabled / canChange flags (_onEnabledExtensionsChanged,
   * _updateCanChange) */
  mode_only = g_settings_get_boolean (settings, DISABLE_USER_EXTENSIONS_KEY);
  extensions = shell_lua_extension_manager_list (self);
  for (l = extensions; l != NULL; l = l->next)
    {
      ShellLuaExtension *extension = l->data;
      const char *uuid = shell_lua_extension_get_uuid (extension);

      shell_lua_extension_set_enabled (extension, uuid_in_array (wanted, uuid));
      shell_lua_extension_set_can_change (extension,
        g_settings_is_writable (settings, ENABLED_EXTENSIONS_KEY) &&
        !mode_only);
    }

  /* Disable what is running but no longer wanted, newest first */
  for (i = self->order->len; i > 0; i--)
    {
      const char *uuid = g_ptr_array_index (self->order, i - 1);

      if (!uuid_in_array (wanted, uuid))
        g_ptr_array_add (to_disable, g_strdup (uuid));
    }
  for (i = 0; i < to_disable->len; i++)
    {
      const char *uuid = g_ptr_array_index (to_disable, i);
      g_autoptr (GError) error = NULL;

      if (!shell_lua_extension_manager_disable (self, uuid, &error))
        g_warning ("Could not disable Lua extension %s: %s", uuid,
                   error->message);
    }

  /* Enable what is wanted, installed and not yet running */
  for (i = 0; i < wanted->len; i++)
    {
      const char *uuid = g_ptr_array_index (wanted, i);
      ShellLuaExtension *extension = shell_lua_extension_manager_lookup (self,
                                                                         uuid);
      g_autoptr (GError) error = NULL;

      if (extension == NULL)
        continue;

      switch (shell_lua_extension_get_state (extension))
        {
        case SHELL_LUA_EXTENSION_STATE_INITIALIZED:
        case SHELL_LUA_EXTENSION_STATE_INACTIVE:
          if (!shell_lua_extension_manager_enable (self, uuid, &error))
            g_warning ("Could not enable Lua extension %s: %s", uuid,
                       error->message);
          break;

        default:
          break;
        }
    }

  g_list_free (extensions);
}

void
shell_lua_extension_manager_sync (ShellLuaExtensionManager *self)
{
  GSettings *settings;

  g_return_if_fail (SHELL_IS_LUA_EXTENSION_MANAGER (self));

  settings = get_shell_settings (self);
  if (settings == NULL)
    return;

  /* First sync: subscribe to the relevant keys */
  if (g_signal_handler_find (settings, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL,
                             self) == 0)
    {
      g_signal_connect (settings, "changed::" ENABLED_EXTENSIONS_KEY,
                        G_CALLBACK (on_settings_changed), self);
      g_signal_connect (settings, "changed::" DISABLED_EXTENSIONS_KEY,
                        G_CALLBACK (on_settings_changed), self);
      g_signal_connect (settings, "changed::" DISABLE_USER_EXTENSIONS_KEY,
                        G_CALLBACK (on_settings_changed), self);
    }

  /* A settings change may arrive while we are enabling: run again after */
  if (self->syncing)
    {
      self->sync_again = TRUE;
      return;
    }

  self->syncing = TRUE;
  do
    {
      self->sync_again = FALSE;
      sync_once (self, settings);
    }
  while (self->sync_again);
  self->syncing = FALSE;
}
