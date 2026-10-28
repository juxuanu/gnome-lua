/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * extension-system.c: The extension manager of the shell. Port of
 * js/ui/extensionSystem.js, rewritten for Lua extensions.
 *
 * #ShellLuaExtensionManager (subprojects/shell-lua) discovers the extensions,
 * runs them in their sandboxed states and applies the error policy. This
 * object decides which of them run:
 *
 *  - the wanted extensions are the ones the session mode enables on its own
 *    plus the `enabled-extensions` setting unless `disable-user-extensions`
 *    is set, without the ones in `disabled-extensions`, which takes
 *    precedence; they are enabled in that order (extensions that declare more
 *    session modes first), and an extension only runs in the session modes
 *    its metadata lists in `session-modes` (default `["user"]`, which covers
 *    the modes that inherit from `user`);
 *  - the stylesheet of an extension is in the theme while it runs;
 *  - `extension-state-changed` is emitted for every change an extension goes
 *    through, for org.gnome.Shell.Extensions to report it.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - Extensions are Lua extensions: `extension.lua` and `prefs.lua`,
 *    `shell-api` instead of `shell-version`. The JS extensions are gone, and
 *    with them `ExtensionUtils`, the cache of imported modules ("a different
 *    version was loaded previously, log out") and the "rebase" that disabled
 *    and enabled again the extensions enabled after a disabled one: the state
 *    of an extension is its own and everything it creates is owned by it.
 *  - The `disable-extension-version-validation` setting has no effect on the
 *    loading of extensions: `shell-api` is checked by the Lua extension
 *    manager (the major version must be the supported one). It is still sent
 *    to the update service.
 *  - The check for updates when a new major version of the shell is run for
 *    the first time (`update-check-<major>`) is gone: extensions do not
 *    depend on the version of the shell.
 *  - An extension whose stylesheet cannot be loaded is put in the error state
 *    before it is enabled, as before; when something else enables it the error
 *    is logged and the extension runs without it.
 *  - ReloadExtension () works: the extension is read from its directory again
 *    and enabled if it is wanted.
 *  - With `allow-extension-installation` off the per-user extensions are
 *    neither listed nor enabled (the JS did not look in the directory).
 *  - The system and the per-user directories of the extensions, and the
 *    directories of updates, are those of #ShellLuaExtensionManager (which
 *    also reads SHELL_LUA_EXTENSION_PATH) and `global.userdatadir`.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "shell-app-system.h"
#include "shell-global.h"
#include "shell-lua-extension-private.h"
#include "st.h"
#include "ui/extension-downloader.h"
#include "ui/extension-system.h"
#include "ui/extension-system-private.h"

#define SHELL_SETTINGS_SCHEMA "org.gnome.shell"
#define ENABLED_EXTENSIONS_KEY "enabled-extensions"
#define DISABLED_EXTENSIONS_KEY "disabled-extensions"
#define DISABLE_USER_EXTENSIONS_KEY "disable-user-extensions"
#define ALLOW_EXTENSION_INSTALLATION_KEY "allow-extension-installation"

#define EXTENSIONS_BUS_NAME "org.gnome.Shell.Extensions"
#define EXTENSIONS_OBJECT_PATH "/org/gnome/Shell/Extensions"
#define EXTENSIONS_INTERFACE "org.gnome.Shell.Extensions"

#define UPDATE_CHECK_TIMEOUT (24 * 60 * 60) /* 1 day in seconds */
#define DISABLE_FILE_TIMEOUT 60 /* s */
#define DISABLE_FILE_NAME "gnome-shell-disable-extensions"

enum
{
  PROP_0,
  PROP_SESSION_MODE,
  PROP_MESSAGE_TRAY,
  PROP_SETTINGS,
  PROP_LUA_MANAGER,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  EXTENSION_STATE_CHANGED,
  OVERVIEW_HIDE_REQUESTED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellExtensionManager
{
  GObject parent;

  ShellSessionMode *session_mode;           /* strong */
  ShellMessageTray *message_tray;           /* weak, nullable */
  GSettings *settings;                      /* strong */
  ShellLuaExtensionManager *lua_manager;    /* strong */
  ShellExtensionDownloader *downloader;
  GCancellable *cancellable;

  GPtrArray *order;                         /* char *: the uuids in the order they were enabled */
  GPtrArray *enabled_uuids;                 /* char *: the wanted extensions at the last sync */
  GHashTable *errors;                       /* uuid -> GPtrArray of char * */
  GHashTable *updates;                      /* set of uuids with an update ready */
  GHashTable *stylesheets;                  /* uuid -> GFile of the loaded stylesheet */

  guint sync_idle_id;
  guint update_timeout_id;
  guint disable_file_timeout_id;
  GFile *disable_file;

  gboolean started;
  gboolean shutting_down;
  gboolean syncing;
  gboolean sync_again;
  gboolean update_notified;
  gboolean follows_color_scheme;
};

G_DEFINE_FINAL_TYPE (ShellExtensionManager, shell_extension_manager, G_TYPE_OBJECT)

/* ------------------------------------------------------------------------ */
/* Errors                                                                    */
/* ------------------------------------------------------------------------ */

static const GDBusErrorEntry extension_error_entries[] = {
  { SHELL_EXTENSION_ERROR_INFO_DOWNLOAD_FAILED, "org.gnome.Shell.Extensions.Error.InfoDownloadFailed" },
  { SHELL_EXTENSION_ERROR_DOWNLOAD_FAILED, "org.gnome.Shell.Extensions.Error.DownloadFailed" },
  { SHELL_EXTENSION_ERROR_EXTRACT_FAILED, "org.gnome.Shell.Extensions.Error.ExtractFailed" },
  { SHELL_EXTENSION_ERROR_ENABLE_FAILED, "org.gnome.Shell.Extensions.Error.EnableFailed" },
  { SHELL_EXTENSION_ERROR_NOT_ALLOWED, "org.gnome.Shell.Extensions.Error.NotAllowed" },
};

/**
 * shell_extension_error_quark:
 *
 * The error domain of #ShellExtensionError, registered with the D-Bus names
 * of the errors.
 *
 * Returns: the quark of the domain
 */
GQuark
shell_extension_error_quark (void)
{
  static gsize quark = 0;

  g_dbus_error_register_error_domain ("shell-extensions-error", &quark,
                                      extension_error_entries,
                                      G_N_ELEMENTS (extension_error_entries));
  return (GQuark) quark;
}

/* ------------------------------------------------------------------------ */
/* Files                                                                     */
/* ------------------------------------------------------------------------ */

/* js/misc/fileUtils.js recursivelyDeleteDir () */
gboolean
shell_extension_recursively_delete_dir (GFile    *dir,
                                        gboolean  delete_parent,
                                        GError  **error)
{
  g_autoptr (GFileEnumerator) children = NULL;

  children = g_file_enumerate_children (dir,
                                        G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                        G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, error);
  if (children == NULL)
    return FALSE;

  while (TRUE)
    {
      g_autoptr (GError) local_error = NULL;
      g_autoptr (GFileInfo) info = g_file_enumerator_next_file (children, NULL, &local_error);
      g_autoptr (GFile) child = NULL;
      GFileType type;

      if (info == NULL)
        {
          if (local_error != NULL)
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
          break;
        }

      type = g_file_info_get_file_type (info);
      child = g_file_get_child (dir, g_file_info_get_name (info));

      if (type == G_FILE_TYPE_REGULAR || type == G_FILE_TYPE_SYMBOLIC_LINK)
        {
          if (!g_file_delete (child, NULL, error))
            return FALSE;
        }
      else if (type == G_FILE_TYPE_DIRECTORY)
        {
          if (!shell_extension_recursively_delete_dir (child, TRUE, error))
            return FALSE;
        }
    }

  if (delete_parent)
    return g_file_delete (dir, NULL, error);

  return TRUE;
}

/* js/misc/fileUtils.js recursivelyMoveDir () */
gboolean
shell_extension_recursively_move_dir (GFile    *source,
                                      GFile    *destination,
                                      GError  **error)
{
  g_autoptr (GFileEnumerator) children = NULL;

  children = g_file_enumerate_children (source,
                                        G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                        G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        NULL, error);
  if (children == NULL)
    return FALSE;

  if (!g_file_query_exists (destination, NULL) &&
      !g_file_make_directory_with_parents (destination, NULL, error))
    return FALSE;

  while (TRUE)
    {
      g_autoptr (GFileInfo) info = g_file_enumerator_next_file (children, NULL, error);
      g_autoptr (GFile) source_child = NULL;
      g_autoptr (GFile) destination_child = NULL;
      GFileType type;

      if (info == NULL)
        {
          if (*error != NULL)
            return FALSE;
          break;
        }

      type = g_file_info_get_file_type (info);
      source_child = g_file_get_child (source, g_file_info_get_name (info));
      destination_child = g_file_get_child (destination, g_file_info_get_name (info));

      if (type == G_FILE_TYPE_REGULAR || type == G_FILE_TYPE_SYMBOLIC_LINK)
        {
          if (!g_file_move (source_child, destination_child, G_FILE_COPY_NONE,
                            NULL, NULL, NULL, error))
            return FALSE;
        }
      else if (type == G_FILE_TYPE_DIRECTORY)
        {
          if (!shell_extension_recursively_move_dir (source_child, destination_child, error))
            return FALSE;
        }
    }

  return TRUE;
}

/* The directories of the data of the shell: the user's (when
 * @include_user_dir) and then the system's, `gnome-shell/<subdir>` in each */
static GPtrArray *
collect_datadirs (const char *subdir,
                  gboolean    include_user_dir)
{
  GPtrArray *dirs = g_ptr_array_new_with_free_func (g_object_unref);
  const char * const *system_dirs = g_get_system_data_dirs ();
  guint i;

  if (include_user_dir)
    {
      g_autofree char *path = g_build_filename (g_get_user_data_dir (), "gnome-shell", subdir, NULL);

      g_ptr_array_add (dirs, g_file_new_for_path (path));
    }

  for (i = 0; system_dirs[i] != NULL; i++)
    {
      g_autofree char *path = g_build_filename (system_dirs[i], "gnome-shell", subdir, NULL);

      g_ptr_array_add (dirs, g_file_new_for_path (path));
    }

  return dirs;
}

/* ------------------------------------------------------------------------ */
/* Accessors                                                                 */
/* ------------------------------------------------------------------------ */

/* The shell, or NULL when the manager is used without one (unit tests) */
static ShellGlobal *
get_global (void)
{
  return g_type_class_peek (SHELL_TYPE_GLOBAL) != NULL ? shell_global_get () : NULL;
}


GSettings *
shell_extension_manager_get_settings (ShellExtensionManager *self)
{
  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), NULL);

  return self->settings;
}

char *
shell_extension_manager_dup_userdatadir (ShellExtensionManager *self)
{
  ShellGlobal *global = get_global ();

  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), NULL);

  if (global != NULL)
    return g_strdup (shell_global_get_userdatadir (global));

  return g_build_filename (g_get_user_data_dir (), "gnome-shell", NULL);
}

static gboolean
installation_allowed (ShellExtensionManager *self)
{
  return g_settings_get_boolean (self->settings, ALLOW_EXTENSION_INSTALLATION_KEY);
}

/* The extensions the manager deals with: with installation not allowed the
 * per-user ones do not exist */
static gboolean
extension_is_visible (ShellExtensionManager *self,
                      ShellLuaExtension     *extension)
{
  return shell_lua_extension_get_extension_type (extension) != SHELL_LUA_EXTENSION_TYPE_PER_USER ||
         installation_allowed (self);
}

/**
 * shell_extension_manager_lookup:
 * @self: a #ShellExtensionManager
 * @uuid: the UUID of an extension
 *
 * Returns: (transfer none) (nullable): the extension, or %NULL if it is not
 *   installed
 */
ShellLuaExtension *
shell_extension_manager_lookup (ShellExtensionManager *self,
                                const char            *uuid)
{
  ShellLuaExtension *extension;

  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), NULL);
  g_return_val_if_fail (uuid != NULL, NULL);

  extension = shell_lua_extension_manager_lookup (self->lua_manager, uuid);
  if (extension == NULL || !extension_is_visible (self, extension))
    return NULL;

  return extension;
}

/**
 * shell_extension_manager_get_uuids:
 * @self: a #ShellExtensionManager
 *
 * Returns: (transfer full) (array zero-terminated=1) (element-type utf8): the
 *   UUIDs of the installed extensions, sorted
 */
char **
shell_extension_manager_get_uuids (ShellExtensionManager *self)
{
  g_autoptr (GList) extensions = NULL;
  GPtrArray *uuids = g_ptr_array_new ();
  GList *l;

  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), NULL);

  extensions = shell_lua_extension_manager_list (self->lua_manager);
  for (l = extensions; l != NULL; l = l->next)
    {
      if (extension_is_visible (self, l->data))
        g_ptr_array_add (uuids, g_strdup (shell_lua_extension_get_uuid (l->data)));
    }
  g_ptr_array_add (uuids, NULL);

  return (char **) g_ptr_array_free (uuids, FALSE);
}

gboolean
shell_extension_manager_has_update (ShellExtensionManager *self,
                                    const char            *uuid)
{
  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), FALSE);

  return g_hash_table_contains (self->updates, uuid);
}

/**
 * shell_extension_manager_get_updates_supported:
 * @self: a #ShellExtensionManager
 *
 * Whether updates of the extensions are downloaded: installing extensions is
 * allowed and an application that installs the updates exists (the Extensions
 * application or Extension Manager).
 *
 * Returns: %TRUE if updates are supported
 */
gboolean
shell_extension_manager_get_updates_supported (ShellExtensionManager *self)
{
  ShellGlobal *global = get_global ();
  ShellAppSystem *app_system;
  gboolean has_updates_app;

  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), FALSE);

  if (global == NULL || !installation_allowed (self))
    return FALSE;

  app_system = shell_global_get_app_system (global);
  has_updates_app =
    shell_app_system_lookup_app (app_system, "org.gnome.Extensions.desktop") != NULL ||
    shell_app_system_lookup_app (app_system, "com.mattjakeman.ExtensionManager.desktop") != NULL;

  return has_updates_app;
}

/* ------------------------------------------------------------------------ */
/* Session modes and the list of enabled extensions                          */
/* ------------------------------------------------------------------------ */

/* The session modes an extension runs in (`session-modes`, "user" by default) */
static gboolean
extension_supports_session_mode (ShellExtensionManager *self,
                                 ShellLuaExtension     *extension)
{
  JsonObject *metadata = shell_lua_extension_get_metadata (extension);
  JsonNode *node = json_object_get_member (metadata, "session-modes");
  const char *current = shell_session_mode_get_current_mode (self->session_mode);
  const char *parent = shell_session_mode_get_parent_mode (self->session_mode);
  gboolean listed = FALSE;

  if (node != NULL && JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *modes = json_node_get_array (node);
      guint i, n = json_array_get_length (modes);

      for (i = 0; i < n; i++)
        {
          JsonNode *element = json_array_get_element (modes, i);
          const char *mode;

          if (!JSON_NODE_HOLDS_VALUE (element) || json_node_get_value_type (element) != G_TYPE_STRING)
            continue;

          listed = TRUE;
          mode = json_node_get_string (element);
          if (g_strcmp0 (mode, current) == 0 || g_strcmp0 (mode, parent) == 0)
            return TRUE;
        }
    }

  if (listed)
    return FALSE;

  /* The default */
  return g_strcmp0 (current, "user") == 0 || g_strcmp0 (parent, "user") == 0;
}

static guint
extension_n_session_modes (ShellLuaExtension *extension)
{
  JsonObject *metadata = shell_lua_extension_get_metadata (extension);
  JsonNode *node = json_object_get_member (metadata, "session-modes");

  if (node != NULL && JSON_NODE_HOLDS_ARRAY (node) &&
      json_array_get_length (json_node_get_array (node)) > 0)
    return json_array_get_length (json_node_get_array (node));

  return 1;
}

static char **
get_mode_extensions (ShellExtensionManager *self)
{
  char **extensions = NULL;

  g_object_get (self->session_mode, "enabled-extensions", &extensions, NULL);
  return extensions;
}

static gboolean
strv_array_contains (GPtrArray  *array,
                     const char *value)
{
  guint i;

  for (i = 0; i < array->len; i++)
    {
      if (strcmp (g_ptr_array_index (array, i), value) == 0)
        return TRUE;
    }

  return FALSE;
}

static void
remove_from_array (GPtrArray  *array,
                   const char *value)
{
  guint i;

  for (i = 0; i < array->len; i++)
    {
      if (strcmp (g_ptr_array_index (array, i), value) == 0)
        {
          g_ptr_array_remove_index (array, i);
          return;
        }
    }
}

/* Extensions with more session modes first (the extensions of the session
 * mode itself, such as those of the login screen, before the others); the
 * order is otherwise the one of the list. */
static guint
extension_sort_key (ShellExtensionManager *self,
                    const char            *uuid)
{
  ShellLuaExtension *extension = shell_lua_extension_manager_lookup (self->lua_manager, uuid);

  return extension != NULL ? extension_n_session_modes (extension) : 0;
}

/* `_getEnabledExtensions ()`: (uuid) */
static GPtrArray *
get_wanted_extensions (ShellExtensionManager *self)
{
  g_autoptr (GPtrArray) list = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *wanted = g_ptr_array_new_with_free_func (g_free);
  g_auto (GStrv) mode_extensions = get_mode_extensions (self);
  g_auto (GStrv) disabled = g_settings_get_strv (self->settings, DISABLED_EXTENSIONS_KEY);
  guint i;

  for (i = 0; mode_extensions != NULL && mode_extensions[i] != NULL; i++)
    {
      if (!strv_array_contains (list, mode_extensions[i]))
        g_ptr_array_add (list, g_strdup (mode_extensions[i]));
    }

  if (!g_settings_get_boolean (self->settings, DISABLE_USER_EXTENSIONS_KEY))
    {
      g_auto (GStrv) enabled = g_settings_get_strv (self->settings, ENABLED_EXTENSIONS_KEY);

      for (i = 0; enabled[i] != NULL; i++)
        {
          if (!strv_array_contains (list, enabled[i]))
            g_ptr_array_add (list, g_strdup (enabled[i]));
        }
    }

  /* A stable insertion sort by the number of session modes, descending */
  for (i = 0; i < list->len; i++)
    {
      const char *uuid = g_ptr_array_index (list, i);
      guint key = extension_sort_key (self, uuid);
      guint position = wanted->len;

      if (g_strv_contains ((const char * const *) disabled, uuid))
        continue;

      while (position > 0 &&
             extension_sort_key (self, g_ptr_array_index (wanted, position - 1)) < key)
        position--;
      g_ptr_array_insert (wanted, position, g_strdup (uuid));
    }

  return wanted;
}

/* `_updateCanChange ()` */
static gboolean
compute_can_change (ShellExtensionManager *self,
                    const char            *uuid)
{
  g_auto (GStrv) mode_extensions = get_mode_extensions (self);
  gboolean is_mode = mode_extensions != NULL &&
                     g_strv_contains ((const char * const *) mode_extensions, uuid);
  gboolean mode_only = g_settings_get_boolean (self->settings, DISABLE_USER_EXTENSIONS_KEY);
  const char *change_key = is_mode ? DISABLE_USER_EXTENSIONS_KEY : ENABLED_EXTENSIONS_KEY;

  return g_settings_is_writable (self->settings, change_key) && (is_mode || !mode_only);
}

/* ------------------------------------------------------------------------ */
/* Stylesheets                                                               */
/* ------------------------------------------------------------------------ */

static StTheme *
get_theme (void)
{
  ShellGlobal *global = get_global ();
  StThemeContext *context;

  if (global == NULL || shell_global_get_stage (global) == NULL)
    return NULL;

  context = st_theme_context_get_for_stage (shell_global_get_stage (global));
  return context != NULL ? st_theme_context_get_theme (context) : NULL;
}

/* `Main.getStyleVariant ()` */
static const char *
get_style_variant (ShellExtensionManager *self)
{
  g_autofree char *scheme = NULL;
  StSystemColorScheme system_scheme = st_settings_get_color_scheme (st_settings_get ());

  g_object_get (self->session_mode, "color-scheme", &scheme, NULL);

  if (g_strcmp0 (scheme, "force-dark") == 0)
    return "dark";
  if (g_strcmp0 (scheme, "force-light") == 0)
    return "light";
  if (g_strcmp0 (scheme, "prefer-dark") == 0)
    return system_scheme == ST_SYSTEM_COLOR_SCHEME_PREFER_LIGHT ? "light" : "dark";
  if (g_strcmp0 (scheme, "prefer-light") == 0)
    return system_scheme == ST_SYSTEM_COLOR_SCHEME_PREFER_DARK ? "dark" : "light";

  return "";
}

static void
unload_stylesheet (ShellExtensionManager *self,
                   const char            *uuid)
{
  GFile *stylesheet = g_hash_table_lookup (self->stylesheets, uuid);
  StTheme *theme;

  if (stylesheet == NULL)
    return;

  theme = get_theme ();
  if (theme != NULL)
    st_theme_unload_stylesheet (theme, stylesheet);

  g_hash_table_remove (self->stylesheets, uuid);
}

/* `_loadExtensionStylesheet ()`: the first of `<mode>-<variant>.css`,
 * `stylesheet-<variant>.css`, `<mode>.css` and `stylesheet.css` that exists */
static gboolean
load_stylesheet (ShellExtensionManager  *self,
                 ShellLuaExtension      *extension,
                 GError                **error)
{
  const char *uuid = shell_lua_extension_get_uuid (extension);
  ShellGlobal *global = get_global ();
  StTheme *theme = get_theme ();
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  const char *variant;
  guint i;

  if (theme == NULL || g_hash_table_contains (self->stylesheets, uuid))
    return TRUE;

  variant = get_style_variant (self);

  if (*variant != '\0')
    g_ptr_array_add (names, g_strdup_printf ("%s-%s.css", shell_global_get_session_mode (global), variant));
  if (*variant != '\0')
    g_ptr_array_add (names, g_strdup_printf ("stylesheet-%s.css", variant));
  g_ptr_array_add (names, g_strdup_printf ("%s.css", shell_global_get_session_mode (global)));
  g_ptr_array_add (names, g_strdup ("stylesheet.css"));

  for (i = 0; i < names->len; i++)
    {
      g_autoptr (GFile) file = g_file_get_child (shell_lua_extension_get_dir (extension),
                                                 g_ptr_array_index (names, i));

      if (!g_file_query_exists (file, NULL))
        continue; /* not an error */

      if (!st_theme_load_stylesheet (theme, file, error))
        return FALSE;

      g_hash_table_replace (self->stylesheets, g_strdup (uuid), g_steal_pointer (&file));
      break;
    }

  return TRUE;
}

/* The stylesheets that have light and dark variants follow the color scheme */
static void
on_color_scheme_changed (StSettings             *settings,
                         GParamSpec             *pspec,
                         ShellExtensionManager  *self)
{
  GHashTableIter iter;
  gpointer key, value;
  g_autoptr (GPtrArray) uuids = g_ptr_array_new_with_free_func (g_free);
  guint i;

  g_hash_table_iter_init (&iter, self->stylesheets);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      g_autofree char *path = g_file_get_path (value);

      if (path != NULL && (g_str_has_suffix (path, "-dark.css") || g_str_has_suffix (path, "-light.css")))
        g_ptr_array_add (uuids, g_strdup (key));
    }

  for (i = 0; i < uuids->len; i++)
    {
      const char *uuid = g_ptr_array_index (uuids, i);
      ShellLuaExtension *extension = shell_lua_extension_manager_lookup (self->lua_manager, uuid);
      g_autoptr (GError) error = NULL;

      if (extension == NULL)
        continue;

      unload_stylesheet (self, uuid);
      if (!load_stylesheet (self, extension, &error))
        {
          shell_extension_manager_log_extension_error (self, uuid, error->message);
          g_message ("Extension %s: %s", uuid, error->message);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* State of the extensions                                                   */
/* ------------------------------------------------------------------------ */

static void
add_error (ShellExtensionManager *self,
           const char            *uuid,
           const char            *message)
{
  GPtrArray *errors = g_hash_table_lookup (self->errors, uuid);

  if (errors == NULL)
    {
      errors = g_ptr_array_new_with_free_func (g_free);
      g_hash_table_insert (self->errors, g_strdup (uuid), errors);
    }

  if (errors->len > 0 && strcmp (g_ptr_array_index (errors, errors->len - 1), message) == 0)
    return;

  g_ptr_array_add (errors, g_strdup (message));
}

static void
queue_sync (ShellExtensionManager *self);

static void
on_lua_extension_state_changed (ShellLuaExtensionManager *lua_manager,
                                ShellLuaExtension        *extension,
                                ShellExtensionManager    *self)
{
  const char *uuid = shell_lua_extension_get_uuid (extension);

  switch (shell_lua_extension_get_state (extension))
    {
    case SHELL_LUA_EXTENSION_STATE_ACTIVATING:
      {
        g_autoptr (GError) error = NULL;

        if (!load_stylesheet (self, extension, &error))
          g_message ("Could not load the stylesheet of extension %s: %s", uuid, error->message);
        break;
      }

    case SHELL_LUA_EXTENSION_STATE_ACTIVE:
      g_hash_table_remove (self->errors, uuid);
      if (!strv_array_contains (self->order, uuid))
        g_ptr_array_add (self->order, g_strdup (uuid));
      break;

    case SHELL_LUA_EXTENSION_STATE_ERROR:
      if (shell_lua_extension_get_error (extension) != NULL &&
          *shell_lua_extension_get_error (extension) != '\0')
        add_error (self, uuid, shell_lua_extension_get_error (extension));
      remove_from_array (self->order, uuid);
      unload_stylesheet (self, uuid);
      break;

    case SHELL_LUA_EXTENSION_STATE_UNINSTALLED:
      remove_from_array (self->order, uuid);
      unload_stylesheet (self, uuid);
      g_hash_table_remove (self->errors, uuid);
      g_hash_table_remove (self->updates, uuid);
      break;

    case SHELL_LUA_EXTENSION_STATE_INACTIVE:
    case SHELL_LUA_EXTENSION_STATE_INITIALIZED:
    case SHELL_LUA_EXTENSION_STATE_OUT_OF_DATE:
      remove_from_array (self->order, uuid);
      unload_stylesheet (self, uuid);
      break;

    default:
      break;
    }

  if (extension_is_visible (self, extension))
    g_signal_emit (self, signals[EXTENSION_STATE_CHANGED], 0, extension);
}

/**
 * shell_extension_manager_log_extension_error:
 * @self: a #ShellExtensionManager
 * @uuid: the UUID of an extension
 * @message: what went wrong
 *
 * Puts the extension in the error state with @message (`logExtensionError ()`).
 * Nothing happens if the extension is not installed.
 */
void
shell_extension_manager_log_extension_error (ShellExtensionManager *self,
                                             const char            *uuid,
                                             const char            *message)
{
  ShellLuaExtension *extension;

  g_return_if_fail (SHELL_IS_EXTENSION_MANAGER (self));
  g_return_if_fail (uuid != NULL);
  g_return_if_fail (message != NULL);

  extension = shell_extension_manager_lookup (self, uuid);
  if (extension == NULL)
    return;

  g_message ("Extension %s: %s", uuid, message);
  shell_lua_extension_fail (extension, message);
}

/* ------------------------------------------------------------------------ */
/* Enabling and disabling                                                    */
/* ------------------------------------------------------------------------ */

static void
enable_one (ShellExtensionManager *self,
            ShellLuaExtension     *extension)
{
  const char *uuid = shell_lua_extension_get_uuid (extension);
  g_autoptr (GError) error = NULL;

  /* A stylesheet that cannot be loaded keeps the extension from running */
  if (!load_stylesheet (self, extension, &error))
    {
      shell_extension_manager_log_extension_error (self, uuid, error->message);
      return;
    }

  if (!shell_lua_extension_manager_enable (self->lua_manager, uuid, &error))
    {
      /* The reason is the error of the extension, and has been logged */
      g_debug ("Failed to enable extension %s: %s", uuid, error->message);
      return;
    }

  if (!strv_array_contains (self->order, uuid))
    g_ptr_array_add (self->order, g_strdup (uuid));
}

static void
disable_one (ShellExtensionManager *self,
             const char            *uuid)
{
  g_autoptr (GError) error = NULL;

  if (!shell_lua_extension_manager_disable (self->lua_manager, uuid, &error))
    g_message ("Failed to disable extension %s: %s", uuid, error->message);

  remove_from_array (self->order, uuid);
}

/* `_onEnabledExtensionsChanged ()` and `_onSettingsWritableChanged ()` */
static void
sync_once (ShellExtensionManager *self)
{
  g_autoptr (GPtrArray) wanted = get_wanted_extensions (self);
  g_autoptr (GPtrArray) to_disable = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GList) extensions = shell_lua_extension_manager_list (self->lua_manager);
  GList *l;
  guint i;

  /* An extension that was copied to the extensions directory since the scan
   * is picked up when it is enabled */
  for (i = 0; i < wanted->len; i++)
    {
      if (shell_lua_extension_manager_lookup (self->lua_manager,
                                              g_ptr_array_index (wanted, i)) == NULL)
        {
          shell_lua_extension_manager_scan (self->lua_manager);
          g_list_free (extensions);
          extensions = shell_lua_extension_manager_list (self->lua_manager);
          break;
        }
    }

  for (l = extensions; l != NULL; l = l->next)
    {
      ShellLuaExtension *extension = l->data;
      const char *uuid = shell_lua_extension_get_uuid (extension);

      shell_lua_extension_set_enabled (extension, strv_array_contains (wanted, uuid));
      shell_lua_extension_set_can_change (extension, compute_can_change (self, uuid));
    }

  /* Disable what is no longer wanted, the last enabled first */
  for (i = self->order->len; i > 0; i--)
    {
      const char *uuid = g_ptr_array_index (self->order, i - 1);
      ShellLuaExtension *extension = shell_extension_manager_lookup (self, uuid);

      if (extension == NULL ||
          !strv_array_contains (wanted, uuid) ||
          !extension_supports_session_mode (self, extension))
        g_ptr_array_add (to_disable, g_strdup (uuid));
    }

  for (i = 0; i < to_disable->len; i++)
    disable_one (self, g_ptr_array_index (to_disable, i));

  /* Enable what is wanted: extensions that were never started or were
   * stopped, and the ones in the error state that were asked for again */
  for (i = 0; i < wanted->len && !self->shutting_down; i++)
    {
      const char *uuid = g_ptr_array_index (wanted, i);
      ShellLuaExtension *extension = shell_extension_manager_lookup (self, uuid);

      if (extension == NULL || !extension_supports_session_mode (self, extension))
        continue;

      switch (shell_lua_extension_get_state (extension))
        {
        case SHELL_LUA_EXTENSION_STATE_INITIALIZED:
        case SHELL_LUA_EXTENSION_STATE_INACTIVE:
          enable_one (self, extension);
          break;

        case SHELL_LUA_EXTENSION_STATE_ERROR:
          if (!strv_array_contains (self->enabled_uuids, uuid))
            enable_one (self, extension);
          break;

        default:
          break;
        }
    }

  g_ptr_array_unref (self->enabled_uuids);
  self->enabled_uuids = g_steal_pointer (&wanted);
}

static void
sync_extensions (ShellExtensionManager *self)
{
  if (!self->started || self->shutting_down)
    return;

  /* Enabling an extension can change the settings: go around again */
  if (self->syncing)
    {
      self->sync_again = TRUE;
      return;
    }

  self->syncing = TRUE;
  do
    {
      self->sync_again = FALSE;
      sync_once (self);
    }
  while (self->sync_again && !self->shutting_down);
  self->syncing = FALSE;
}

static gboolean
sync_idle_cb (gpointer user_data)
{
  ShellExtensionManager *self = user_data;

  self->sync_idle_id = 0;
  sync_extensions (self);

  return G_SOURCE_REMOVE;
}

static void
queue_sync (ShellExtensionManager *self)
{
  if (self->sync_idle_id != 0 || self->shutting_down)
    return;

  self->sync_idle_id = g_idle_add (sync_idle_cb, self);
  g_source_set_name_by_id (self->sync_idle_id, "[gnome-shell] sync_idle_cb");
}

static void
on_enabled_extensions_changed (GSettings             *settings,
                               const char            *key,
                               ShellExtensionManager *self)
{
  queue_sync (self);
}

static void
on_user_extensions_changed (GSettings             *settings,
                            const char            *key,
                            ShellExtensionManager *self)
{
  /* `_onUserExtensionsEnabledChanged ()`: the flags too */
  sync_extensions (self);
}

static void
on_session_mode_updated (ShellSessionMode       *session_mode,
                         ShellExtensionManager  *self)
{
  sync_extensions (self);
}

/**
 * shell_extension_manager_enable_extension:
 * @self: a #ShellExtensionManager
 * @uuid: the UUID of an extension
 *
 * Adds @uuid to `enabled-extensions` and removes it from
 * `disabled-extensions`; the extension starts when the settings change.
 *
 * Returns: %FALSE if the extension is not installed
 */
gboolean
shell_extension_manager_enable_extension (ShellExtensionManager *self,
                                          const char            *uuid)
{
  g_auto (GStrv) enabled = NULL;
  g_auto (GStrv) disabled = NULL;

  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), FALSE);
  g_return_val_if_fail (uuid != NULL, FALSE);

  if (shell_extension_manager_lookup (self, uuid) == NULL)
    return FALSE;

  enabled = g_settings_get_strv (self->settings, ENABLED_EXTENSIONS_KEY);
  disabled = g_settings_get_strv (self->settings, DISABLED_EXTENSIONS_KEY);

  if (g_strv_contains ((const char * const *) disabled, uuid))
    {
      g_autoptr (GPtrArray) list = g_ptr_array_new ();
      guint i;

      for (i = 0; disabled[i] != NULL; i++)
        {
          if (strcmp (disabled[i], uuid) != 0)
            g_ptr_array_add (list, disabled[i]);
        }
      g_ptr_array_add (list, NULL);
      g_settings_set_strv (self->settings, DISABLED_EXTENSIONS_KEY,
                           (const char * const *) list->pdata);
    }

  if (!g_strv_contains ((const char * const *) enabled, uuid))
    {
      g_autoptr (GPtrArray) list = g_ptr_array_new ();
      guint i;

      for (i = 0; enabled[i] != NULL; i++)
        g_ptr_array_add (list, enabled[i]);
      g_ptr_array_add (list, (gpointer) uuid);
      g_ptr_array_add (list, NULL);
      g_settings_set_strv (self->settings, ENABLED_EXTENSIONS_KEY,
                           (const char * const *) list->pdata);
    }

  return TRUE;
}

/**
 * shell_extension_manager_disable_extension:
 * @self: a #ShellExtensionManager
 * @uuid: the UUID of an extension
 *
 * Removes @uuid from `enabled-extensions` and adds it to
 * `disabled-extensions`; the extension stops when the settings change.
 *
 * Returns: %FALSE if the extension is not installed
 */
gboolean
shell_extension_manager_disable_extension (ShellExtensionManager *self,
                                           const char            *uuid)
{
  g_auto (GStrv) enabled = NULL;
  g_auto (GStrv) disabled = NULL;

  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), FALSE);
  g_return_val_if_fail (uuid != NULL, FALSE);

  if (shell_extension_manager_lookup (self, uuid) == NULL)
    return FALSE;

  enabled = g_settings_get_strv (self->settings, ENABLED_EXTENSIONS_KEY);
  disabled = g_settings_get_strv (self->settings, DISABLED_EXTENSIONS_KEY);

  if (g_strv_contains ((const char * const *) enabled, uuid))
    {
      g_autoptr (GPtrArray) list = g_ptr_array_new ();
      guint i;

      for (i = 0; enabled[i] != NULL; i++)
        {
          if (strcmp (enabled[i], uuid) != 0)
            g_ptr_array_add (list, enabled[i]);
        }
      g_ptr_array_add (list, NULL);
      g_settings_set_strv (self->settings, ENABLED_EXTENSIONS_KEY,
                           (const char * const *) list->pdata);
    }

  if (!g_strv_contains ((const char * const *) disabled, uuid))
    {
      g_autoptr (GPtrArray) list = g_ptr_array_new ();
      guint i;

      for (i = 0; disabled[i] != NULL; i++)
        g_ptr_array_add (list, disabled[i]);
      g_ptr_array_add (list, (gpointer) uuid);
      g_ptr_array_add (list, NULL);
      g_settings_set_strv (self->settings, DISABLED_EXTENSIONS_KEY,
                           (const char * const *) list->pdata);
    }

  return TRUE;
}

/**
 * shell_extension_manager_reload_extension:
 * @self: a #ShellExtensionManager
 * @uuid: the UUID of an extension
 * @error: return location for a #GError
 *
 * Reads the extension from its directory again, so that the code, the
 * metadata and the stylesheet are the ones on disk now, and starts it if it
 * is enabled. A running extension is stopped first.
 *
 * Returns: %TRUE on success
 */
gboolean
shell_extension_manager_reload_extension (ShellExtensionManager  *self,
                                          const char             *uuid,
                                          GError                **error)
{
  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), FALSE);
  g_return_val_if_fail (uuid != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (shell_extension_manager_lookup (self, uuid) == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Extension %s is not installed", uuid);
      return FALSE;
    }

  if (!shell_lua_extension_manager_reload (self->lua_manager, uuid, error))
    return FALSE;

  g_hash_table_remove (self->errors, uuid);
  sync_extensions (self);
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Installing, updating, removing                                            */
/* ------------------------------------------------------------------------ */

gboolean
shell_extension_manager_add_installed (ShellExtensionManager  *self,
                                       const char             *uuid,
                                       GError                **error)
{
  g_autoptr (GError) load_error = NULL;

  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), FALSE);

  if (shell_lua_extension_manager_lookup (self->lua_manager, uuid) != NULL)
    {
      if (!shell_lua_extension_manager_reload (self->lua_manager, uuid, &load_error))
        g_message ("Could not load the new version of extension %s: %s", uuid, load_error->message);
    }
  else
    {
      shell_lua_extension_manager_scan (self->lua_manager);
    }

  if (shell_extension_manager_lookup (self, uuid) == NULL)
    {
      g_set_error (error, SHELL_EXTENSION_ERROR, SHELL_EXTENSION_ERROR_ENABLE_FAILED,
                   "%s was installed, but is not a Lua extension that can be loaded "
                   "(it needs a metadata.json with \"shell-api\" and an extension.lua)",
                   uuid);
      return FALSE;
    }

  if (!shell_extension_manager_enable_extension (self, uuid))
    {
      g_set_error (error, SHELL_EXTENSION_ERROR, SHELL_EXTENSION_ERROR_ENABLE_FAILED,
                   "Cannot enable %s", uuid);
      return FALSE;
    }

  queue_sync (self);
  return TRUE;
}

gboolean
shell_extension_manager_remove_installed (ShellExtensionManager *self,
                                          const char            *uuid)
{
  ShellLuaExtension *extension;
  g_autoptr (GFile) dir = NULL;
  g_autoptr (GFile) updates_dir = NULL;
  g_autofree char *userdatadir = NULL;
  g_autoptr (GError) error = NULL;
  GFileType type;

  extension = shell_extension_manager_lookup (self, uuid);
  if (extension == NULL)
    return FALSE;

  /* Don't try to uninstall system extensions */
  if (shell_lua_extension_get_extension_type (extension) != SHELL_LUA_EXTENSION_TYPE_PER_USER)
    return FALSE;

  dir = g_object_ref (shell_lua_extension_get_dir (extension));

  /* Try to stop it, it will be gone from the list in any case */
  disable_one (self, uuid);

  /* A link to a checkout of the extension is removed, not its target */
  type = g_file_query_file_type (dir, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);
  if (type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      if (!g_file_delete (dir, NULL, &error))
        {
          g_warning ("Could not remove extension %s: %s", uuid, error->message);
          return FALSE;
        }
    }
  else if (!shell_extension_recursively_delete_dir (dir, TRUE, &error))
    {
      g_warning ("Could not remove extension %s: %s", uuid, error->message);
      /* The extension that was running is gone from memory too */
      shell_lua_extension_manager_scan (self->lua_manager);
      return FALSE;
    }

  userdatadir = shell_extension_manager_dup_userdatadir (self);
  {
    g_autofree char *path = g_build_filename (userdatadir, "extension-updates", uuid, NULL);

    updates_dir = g_file_new_for_path (path);
  }
  if (g_file_query_exists (updates_dir, NULL))
    {
      g_autoptr (GError) updates_error = NULL;

      /* not an error */
      if (!shell_extension_recursively_delete_dir (updates_dir, TRUE, &updates_error))
        g_debug ("Could not remove the update of extension %s: %s", uuid, updates_error->message);
    }

  /* The directory is gone: the Lua manager forgets the extension */
  shell_lua_extension_manager_scan (self->lua_manager);
  return TRUE;
}

/**
 * shell_extension_manager_uninstall_extension:
 * @self: a #ShellExtensionManager
 * @uuid: the UUID of an extension
 *
 * Stops the extension and deletes its files. Only per-user extensions can be
 * removed.
 *
 * Returns: %TRUE if the extension was uninstalled
 */
gboolean
shell_extension_manager_uninstall_extension (ShellExtensionManager *self,
                                             const char            *uuid)
{
  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), FALSE);
  g_return_val_if_fail (uuid != NULL, FALSE);

  return shell_extension_manager_remove_installed (self, uuid);
}

/**
 * shell_extension_manager_install_extension_async:
 * @self: a #ShellExtensionManager
 * @uuid: the UUID of the extension on extensions.gnome.org
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (closure user_data): called when the installation is over
 * @user_data: data for @callback
 *
 * Downloads and installs an extension after the user agreed in a dialog
 * (InstallRemoteExtension ()).
 */
void
shell_extension_manager_install_extension_async (ShellExtensionManager *self,
                                                 const char            *uuid,
                                                 GCancellable          *cancellable,
                                                 GAsyncReadyCallback    callback,
                                                 gpointer               user_data)
{
  g_return_if_fail (SHELL_IS_EXTENSION_MANAGER (self));
  g_return_if_fail (uuid != NULL);

  shell_extension_downloader_install_async (self->downloader, uuid, cancellable,
                                            callback, user_data);
}

/**
 * shell_extension_manager_install_extension_finish:
 * @self: a #ShellExtensionManager
 * @result: the result passed to the callback
 * @error: return location for a #GError
 *
 * Returns: (transfer full): `successful` or `cancelled`; %NULL with a
 *   #ShellExtensionError in @error if the installation failed
 */
char *
shell_extension_manager_install_extension_finish (ShellExtensionManager  *self,
                                                  GAsyncResult           *result,
                                                  GError                **error)
{
  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), NULL);

  return shell_extension_downloader_install_finish (self->downloader, result, error);
}

/**
 * shell_extension_manager_check_for_updates_async:
 * @self: a #ShellExtensionManager
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (closure user_data): called when the check is over
 * @user_data: data for @callback
 *
 * Asks extensions.gnome.org for newer versions of the per-user extensions and
 * downloads them (CheckForUpdates ()).
 */
void
shell_extension_manager_check_for_updates_async (ShellExtensionManager *self,
                                                 GCancellable          *cancellable,
                                                 GAsyncReadyCallback    callback,
                                                 gpointer               user_data)
{
  g_return_if_fail (SHELL_IS_EXTENSION_MANAGER (self));

  shell_extension_downloader_check_for_updates_async (self->downloader, cancellable,
                                                      callback, user_data);
}

/**
 * shell_extension_manager_check_for_updates_finish:
 * @self: a #ShellExtensionManager
 * @result: the result passed to the callback
 * @error: return location for a #GError
 *
 * Returns: %TRUE if the check went through
 */
gboolean
shell_extension_manager_check_for_updates_finish (ShellExtensionManager  *self,
                                                  GAsyncResult           *result,
                                                  GError                **error)
{
  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), FALSE);

  return shell_extension_downloader_check_for_updates_finish (self->downloader, result, error);
}

static void
check_for_updates_cb (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  ShellExtensionManager *self = SHELL_EXTENSION_MANAGER (source);
  g_autoptr (GError) error = NULL;

  if (!shell_extension_manager_check_for_updates_finish (self, result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("Update check failed: %s", error->message);
}

static void
check_for_updates (ShellExtensionManager *self)
{
  if (self->shutting_down)
    return;

  shell_extension_manager_check_for_updates_async (self, self->cancellable,
                                                   check_for_updates_cb, NULL);
}

static gboolean
update_timeout_cb (gpointer user_data)
{
  check_for_updates (user_data);

  return G_SOURCE_CONTINUE;
}

/* `_installExtensionUpdates ()`: the updates that were downloaded last time
 * replace the extensions now, before they are loaded */
static void
install_extension_updates (ShellExtensionManager *self)
{
  g_autoptr (GPtrArray) dirs = NULL;
  g_autofree char *userdatadir = NULL;
  guint i;

  if (!shell_extension_manager_get_updates_supported (self))
    return;

  userdatadir = shell_extension_manager_dup_userdatadir (self);
  dirs = collect_datadirs ("extension-updates", TRUE);

  for (i = 0; i < dirs->len; i++)
    {
      GFile *updates_dir = g_ptr_array_index (dirs, i);
      g_autoptr (GFileEnumerator) enumerator = NULL;

      enumerator = g_file_enumerate_children (updates_dir,
                                              G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                              G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                              G_FILE_QUERY_INFO_NONE, NULL, NULL);
      if (enumerator == NULL)
        continue;

      while (TRUE)
        {
          g_autoptr (GFileInfo) info = g_file_enumerator_next_file (enumerator, NULL, NULL);
          g_autoptr (GFile) dir = NULL;
          g_autoptr (GFile) extension_dir = NULL;
          g_autoptr (GError) error = NULL;
          g_autofree char *extension_path = NULL;
          const char *uuid;

          if (info == NULL)
            break;

          if (g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
            continue;

          uuid = g_file_info_get_name (info);
          dir = g_file_enumerator_get_child (enumerator, info);
          extension_path = g_build_filename (userdatadir, "extensions", uuid, NULL);
          extension_dir = g_file_new_for_path (extension_path);

          if (!shell_extension_recursively_delete_dir (extension_dir, FALSE, &error) ||
              !shell_extension_recursively_move_dir (dir, extension_dir, &error))
            g_message ("Failed to install extension updates for %s", uuid);
          g_clear_error (&error);

          if (!shell_extension_recursively_delete_dir (dir, TRUE, &error))
            g_message ("Failed to delete extension update: %s", error->message);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* The notification that updates are ready                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_EXTENSION_UPDATE_SOURCE (shell_extension_update_source_get_type ())
G_DECLARE_FINAL_TYPE (ShellExtensionUpdateSource, shell_extension_update_source,
                      SHELL, EXTENSION_UPDATE_SOURCE, ShellSource)

struct _ShellExtensionUpdateSource
{
  ShellSource parent;

  ShellApp *app;                      /* strong, nullable */
  ShellExtensionManager *manager;     /* weak */
};

G_DEFINE_FINAL_TYPE (ShellExtensionUpdateSource, shell_extension_update_source, SHELL_TYPE_SOURCE)

static void
shell_extension_update_source_open (ShellSource *source)
{
  ShellExtensionUpdateSource *self = SHELL_EXTENSION_UPDATE_SOURCE (source);

  if (self->app != NULL)
    shell_app_activate (self->app);

  if (self->manager != NULL)
    g_signal_emit (self->manager, signals[OVERVIEW_HIDE_REQUESTED], 0);
}

static void
shell_extension_update_source_dispose (GObject *object)
{
  ShellExtensionUpdateSource *self = SHELL_EXTENSION_UPDATE_SOURCE (object);

  g_clear_weak_pointer (&self->manager);
  g_clear_object (&self->app);

  G_OBJECT_CLASS (shell_extension_update_source_parent_class)->dispose (object);
}

static void
shell_extension_update_source_class_init (ShellExtensionUpdateSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellSourceClass *source_class = SHELL_SOURCE_CLASS (klass);

  object_class->dispose = shell_extension_update_source_dispose;
  source_class->open = shell_extension_update_source_open;
}

static void
shell_extension_update_source_init (ShellExtensionUpdateSource *self)
{
}

static void
on_update_notification_activated (ShellNotification *notification,
                                  ShellSource       *source)
{
  shell_source_open (source);
}

static void
show_update_notification (ShellExtensionManager *self)
{
  ShellApp *app = NULL;
  g_autoptr (ShellNotificationPolicy) policy = NULL;
  ShellExtensionUpdateSource *source;
  ShellNotification *notification;
  ShellGlobal *global = get_global ();

  if (self->message_tray == NULL || global == NULL)
    return;

  app = shell_app_system_lookup_app (shell_global_get_app_system (global), "org.gnome.Extensions.desktop");
  if (app == NULL)
    app = shell_app_system_lookup_app (shell_global_get_app_system (global),
                                       "com.mattjakeman.ExtensionManager.desktop");
  if (app == NULL)
    return;

  policy = shell_notification_policy_new_for_app (app);
  source = g_object_new (SHELL_TYPE_EXTENSION_UPDATE_SOURCE,
                         "title", shell_app_get_name (app),
                         "icon", shell_app_get_icon (app),
                         "policy", policy,
                         NULL);
  source->app = g_object_ref (app);
  g_set_weak_pointer (&source->manager, self);
  shell_message_tray_add (self->message_tray, SHELL_SOURCE (source));

  notification = shell_notification_new (SHELL_SOURCE (source),
                                         _("Extension Updates Available"),
                                         _("Extension updates are ready to be installed"));
  g_signal_connect_object (notification, "activated",
                           G_CALLBACK (on_update_notification_activated), source, 0);
  shell_source_add_notification (SHELL_SOURCE (source), notification);

  /* The source holds the notification and the tray the source */
  g_object_unref (notification);
  g_object_unref (source);
}

void
shell_extension_manager_notify_extension_update (ShellExtensionManager *self,
                                                 const char            *uuid)
{
  ShellLuaExtension *extension;

  g_return_if_fail (SHELL_IS_EXTENSION_MANAGER (self));
  g_return_if_fail (uuid != NULL);

  extension = shell_extension_manager_lookup (self, uuid);
  if (extension == NULL)
    return;

  g_hash_table_add (self->updates, g_strdup (uuid));
  g_signal_emit (self, signals[EXTENSION_STATE_CHANGED], 0, extension);

  if (!self->update_notified)
    {
      self->update_notified = TRUE;
      show_update_notification (self);
    }
}

/* ------------------------------------------------------------------------ */
/* D-Bus answers                                                             */
/* ------------------------------------------------------------------------ */

static GVariant *
serialize_extension (ShellExtensionManager *self,
                     ShellLuaExtension     *extension)
{
  g_autoptr (GVariant) info = shell_lua_extension_serialize (extension);
  g_autoptr (GVariantDict) dict = g_variant_dict_new (info);

  g_variant_dict_insert (dict, "hasUpdate", "b",
                         g_hash_table_contains (self->updates,
                                                shell_lua_extension_get_uuid (extension)));

  return g_variant_ref_sink (g_variant_dict_end (dict));
}

/**
 * shell_extension_manager_serialize_extension:
 * @self: a #ShellExtensionManager
 * @extension: an extension, also one that was just removed
 *
 * The description of @extension, as shell_extension_manager_get_extension_info ()
 * returns it, for the ExtensionStateChanged signal.
 *
 * Returns: (transfer full): a variant of type `a{sv}`
 */
GVariant *
shell_extension_manager_serialize_extension (ShellExtensionManager *self,
                                             ShellLuaExtension     *extension)
{
  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), NULL);
  g_return_val_if_fail (SHELL_IS_LUA_EXTENSION (extension), NULL);

  return serialize_extension (self, extension);
}

/**
 * shell_extension_manager_list_extensions:
 * @self: a #ShellExtensionManager
 *
 * The extensions, as ListExtensions () returns them.
 *
 * Returns: (transfer full): a variant of type `a{sa{sv}}`
 */
GVariant *
shell_extension_manager_list_extensions (ShellExtensionManager *self)
{
  g_autoptr (GList) extensions = NULL;
  GVariantBuilder builder;
  GList *l;

  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), NULL);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sa{sv}}"));

  extensions = shell_lua_extension_manager_list (self->lua_manager);
  for (l = extensions; l != NULL; l = l->next)
    {
      g_autoptr (GVariant) info = NULL;

      if (!extension_is_visible (self, l->data))
        continue;

      info = serialize_extension (self, l->data);
      g_variant_builder_add (&builder, "{s@a{sv}}",
                             shell_lua_extension_get_uuid (l->data), info);
    }

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

/**
 * shell_extension_manager_get_extension_info:
 * @self: a #ShellExtensionManager
 * @uuid: the UUID of an extension
 *
 * The description of an extension, as GetExtensionInfo () returns it: the
 * metadata of the extension and its `type`, `state`, `enabled`, `path`,
 * `error`, `hasPrefs`, `hasUpdate`, `canChange` and `sessionModes`.
 *
 * Returns: (transfer full): a variant of type `a{sv}`, empty if the extension
 *   is not installed
 */
GVariant *
shell_extension_manager_get_extension_info (ShellExtensionManager *self,
                                            const char            *uuid)
{
  ShellLuaExtension *extension;

  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), NULL);
  g_return_val_if_fail (uuid != NULL, NULL);

  extension = shell_extension_manager_lookup (self, uuid);
  if (extension == NULL)
    return g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0));

  return serialize_extension (self, extension);
}

/**
 * shell_extension_manager_get_extension_errors:
 * @self: a #ShellExtensionManager
 * @uuid: the UUID of an extension
 *
 * The errors that put the extension in the error state, as
 * GetExtensionErrors () returns them.
 *
 * Returns: (transfer full): a variant of type `as`
 */
GVariant *
shell_extension_manager_get_extension_errors (ShellExtensionManager *self,
                                              const char            *uuid)
{
  GPtrArray *errors;

  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), NULL);
  g_return_val_if_fail (uuid != NULL, NULL);

  errors = g_hash_table_lookup (self->errors, uuid);
  if (errors == NULL || shell_extension_manager_lookup (self, uuid) == NULL)
    return g_variant_ref_sink (g_variant_new_strv (NULL, 0));

  return g_variant_ref_sink (g_variant_new_strv ((const char * const *) errors->pdata, errors->len));
}

static void
open_prefs_cb (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  if (reply == NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("Failed to open the preferences of an extension: %s", error->message);
}

/**
 * shell_extension_manager_open_extension_prefs:
 * @self: a #ShellExtensionManager
 * @uuid: the UUID of an extension
 * @parent_window: the identifier of the window of the client, or an empty string
 * @options: (nullable): a variant of type `a{sv}` with the options
 *   (`modal`: a boolean)
 *
 * Asks the extensions service (org.gnome.Shell.Extensions) to show the
 * preferences of the extension; the service runs in a process of its own.
 *
 * Returns: %FALSE if the extension is not installed or has no `prefs.lua`
 */
gboolean
shell_extension_manager_open_extension_prefs (ShellExtensionManager *self,
                                              const char            *uuid,
                                              const char            *parent_window,
                                              GVariant              *options)
{
  ShellLuaExtension *extension;
  g_autoptr (GFile) prefs = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GVariant) empty_options = NULL;

  g_return_val_if_fail (SHELL_IS_EXTENSION_MANAGER (self), FALSE);
  g_return_val_if_fail (uuid != NULL, FALSE);

  extension = shell_extension_manager_lookup (self, uuid);
  if (extension == NULL)
    return FALSE;

  prefs = g_file_get_child (shell_lua_extension_get_dir (extension), "prefs.lua");
  if (!g_file_query_exists (prefs, NULL))
    return FALSE;

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (connection == NULL)
    {
      g_warning ("Failed to open the preferences of extension %s: %s", uuid, error->message);
      return FALSE;
    }

  if (options == NULL || !g_variant_is_of_type (options, G_VARIANT_TYPE_VARDICT))
    {
      empty_options = g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0));
      options = empty_options;
    }

  g_dbus_connection_call (connection,
                          EXTENSIONS_BUS_NAME, EXTENSIONS_OBJECT_PATH, EXTENSIONS_INTERFACE,
                          "OpenExtensionPrefs",
                          g_variant_new ("(ss@a{sv})", uuid,
                                         parent_window != NULL ? parent_window : "", options),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, self->cancellable,
                          open_prefs_cb, NULL);
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Start and shutdown                                                        */
/* ------------------------------------------------------------------------ */

static void
delete_disable_file (ShellExtensionManager *self)
{
  g_autoptr (GError) error = NULL;

  if (self->disable_file == NULL)
    return;

  g_clear_handle_id (&self->disable_file_timeout_id, g_source_remove);

  if (!g_file_delete (self->disable_file, NULL, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    g_message ("Failed to delete the file %s: %s",
               g_file_peek_path (self->disable_file), error->message);

  g_clear_object (&self->disable_file);
}

static void
disable_file_timeout_cb (gpointer user_data)
{
  ShellExtensionManager *self = user_data;

  self->disable_file_timeout_id = 0;
  delete_disable_file (self);
}

/* The file exists for a period of time when extensions are enabled after
 * start. If it exists, then the systemd unit will disable extensions should
 * gnome-shell crash. Should the file already exist from a previous login,
 * then this is OK. */
static void
create_disable_file (ShellExtensionManager *self)
{
  g_autofree char *path = g_build_filename (g_get_user_runtime_dir (), DISABLE_FILE_NAME, NULL);
  g_autoptr (GFileOutputStream) stream = NULL;
  g_autoptr (GError) error = NULL;

  self->disable_file = g_file_new_for_path (path);
  stream = g_file_create (self->disable_file, G_FILE_CREATE_REPLACE_DESTINATION, NULL, &error);
  if (stream == NULL)
    {
      g_message ("Failed to create file %s: %s", path, error->message);
      g_clear_object (&self->disable_file);
      return;
    }
  g_output_stream_close (G_OUTPUT_STREAM (stream), NULL, NULL);

  self->disable_file_timeout_id =
    g_timeout_add_seconds_once (DISABLE_FILE_TIMEOUT, disable_file_timeout_cb, self);
}

static void
on_global_shutdown (ShellGlobal           *global,
                    ShellExtensionManager *self)
{
  self->shutting_down = TRUE;

  g_cancellable_cancel (self->cancellable);
  g_clear_handle_id (&self->sync_idle_id, g_source_remove);
  g_clear_handle_id (&self->update_timeout_id, g_source_remove);
  delete_disable_file (self);
}

/**
 * shell_extension_manager_start:
 * @self: a #ShellExtensionManager
 *
 * Loads the extensions and enables the ones that are wanted (`init ()` of the
 * JS manager). Call it once, when the shell is ready to run extensions.
 */
void
shell_extension_manager_start (ShellExtensionManager *self)
{
  g_return_if_fail (SHELL_IS_EXTENSION_MANAGER (self));

  if (self->started || self->shutting_down)
    return;

  /* Without the settings or the session mode there is nothing to follow */
  g_return_if_fail (self->settings != NULL && self->session_mode != NULL);

  self->started = TRUE;

  if (get_global () != NULL)
    create_disable_file (self);

  install_extension_updates (self);

  g_signal_connect_object (self->settings, "changed::" ENABLED_EXTENSIONS_KEY,
                           G_CALLBACK (on_enabled_extensions_changed), self, 0);
  g_signal_connect_object (self->settings, "changed::" DISABLED_EXTENSIONS_KEY,
                           G_CALLBACK (on_enabled_extensions_changed), self, 0);
  g_signal_connect_object (self->settings, "changed::" DISABLE_USER_EXTENSIONS_KEY,
                           G_CALLBACK (on_user_extensions_changed), self, 0);
  g_signal_connect_object (self->settings, "changed::" ALLOW_EXTENSION_INSTALLATION_KEY,
                           G_CALLBACK (on_user_extensions_changed), self, 0);
  g_signal_connect_object (self->settings, "writable-changed::" ENABLED_EXTENSIONS_KEY,
                           G_CALLBACK (on_user_extensions_changed), self, 0);
  g_signal_connect_object (self->settings, "writable-changed::" DISABLED_EXTENSIONS_KEY,
                           G_CALLBACK (on_user_extensions_changed), self, 0);

  shell_lua_extension_manager_scan (self->lua_manager);
  sync_extensions (self);

  check_for_updates (self);
  self->update_timeout_id = g_timeout_add_seconds (UPDATE_CHECK_TIMEOUT, update_timeout_cb, self);
  g_source_set_name_by_id (self->update_timeout_id, "[gnome-shell] update_timeout_cb");
}

/* ------------------------------------------------------------------------ */
/* Object                                                                    */
/* ------------------------------------------------------------------------ */

static void
shell_extension_manager_constructed (GObject *object)
{
  ShellExtensionManager *self = SHELL_EXTENSION_MANAGER (object);
  ShellGlobal *global = get_global ();

  G_OBJECT_CLASS (shell_extension_manager_parent_class)->constructed (object);

  if (self->settings == NULL)
    {
      GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
      g_autoptr (GSettingsSchema) schema = NULL;

      if (global != NULL)
        self->settings = g_object_ref (shell_global_get_settings (global));
      else if (source != NULL &&
               (schema = g_settings_schema_source_lookup (source, SHELL_SETTINGS_SCHEMA, TRUE)) != NULL)
        self->settings = g_settings_new_full (schema, NULL, NULL);
    }

  if (self->lua_manager == NULL)
    {
      if (global != NULL)
        self->lua_manager = g_object_ref (shell_global_get_lua_extension_manager (global));
      else
        self->lua_manager = g_object_ref (shell_lua_extension_manager_get_default ());
    }

  self->downloader = shell_extension_downloader_new (self);

  g_signal_connect_object (self->lua_manager, "extension-state-changed",
                           G_CALLBACK (on_lua_extension_state_changed), self, 0);
  if (self->session_mode != NULL)
    g_signal_connect_object (self->session_mode, "updated",
                             G_CALLBACK (on_session_mode_updated), self, 0);
  if (global != NULL)
    {
      g_signal_connect_object (st_settings_get (), "notify::color-scheme",
                               G_CALLBACK (on_color_scheme_changed), self, 0);
      self->follows_color_scheme = TRUE;
      g_signal_connect_object (global, "shutdown", G_CALLBACK (on_global_shutdown), self, 0);
    }
}

static void
shell_extension_manager_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  ShellExtensionManager *self = SHELL_EXTENSION_MANAGER (object);

  switch (prop_id)
    {
    case PROP_SESSION_MODE:
      g_set_object (&self->session_mode, g_value_get_object (value));
      break;

    case PROP_MESSAGE_TRAY:
      g_set_weak_pointer (&self->message_tray, g_value_get_object (value));
      break;

    case PROP_SETTINGS:
      g_set_object (&self->settings, g_value_get_object (value));
      break;

    case PROP_LUA_MANAGER:
      g_set_object (&self->lua_manager, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_extension_manager_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  ShellExtensionManager *self = SHELL_EXTENSION_MANAGER (object);

  switch (prop_id)
    {
    case PROP_SESSION_MODE:
      g_value_set_object (value, self->session_mode);
      break;

    case PROP_MESSAGE_TRAY:
      g_value_set_object (value, self->message_tray);
      break;

    case PROP_SETTINGS:
      g_value_set_object (value, self->settings);
      break;

    case PROP_LUA_MANAGER:
      g_value_set_object (value, self->lua_manager);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_extension_manager_dispose (GObject *object)
{
  ShellExtensionManager *self = SHELL_EXTENSION_MANAGER (object);

  self->shutting_down = TRUE;

  /* Dropping the last reference to the Lua manager disables the extensions
   * that still run, which must not reach this object any more */
  if (self->lua_manager != NULL)
    g_signal_handlers_disconnect_by_data (self->lua_manager, self);
  if (self->session_mode != NULL)
    g_signal_handlers_disconnect_by_data (self->session_mode, self);
  if (self->settings != NULL)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  if (self->follows_color_scheme)
    g_signal_handlers_disconnect_by_data (st_settings_get (), self);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  g_clear_handle_id (&self->sync_idle_id, g_source_remove);
  g_clear_handle_id (&self->update_timeout_id, g_source_remove);
  g_clear_handle_id (&self->disable_file_timeout_id, g_source_remove);
  g_clear_object (&self->disable_file);

  g_clear_object (&self->downloader);
  g_clear_object (&self->cancellable);
  g_clear_weak_pointer (&self->message_tray);
  g_clear_object (&self->session_mode);
  g_clear_object (&self->settings);
  g_clear_object (&self->lua_manager);

  G_OBJECT_CLASS (shell_extension_manager_parent_class)->dispose (object);
}

static void
shell_extension_manager_finalize (GObject *object)
{
  ShellExtensionManager *self = SHELL_EXTENSION_MANAGER (object);

  g_clear_pointer (&self->order, g_ptr_array_unref);
  g_clear_pointer (&self->enabled_uuids, g_ptr_array_unref);
  g_clear_pointer (&self->errors, g_hash_table_unref);
  g_clear_pointer (&self->updates, g_hash_table_unref);
  g_clear_pointer (&self->stylesheets, g_hash_table_unref);

  G_OBJECT_CLASS (shell_extension_manager_parent_class)->finalize (object);
}

static void
shell_extension_manager_class_init (ShellExtensionManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_extension_manager_constructed;
  object_class->set_property = shell_extension_manager_set_property;
  object_class->get_property = shell_extension_manager_get_property;
  object_class->dispose = shell_extension_manager_dispose;
  object_class->finalize = shell_extension_manager_finalize;

  /**
   * ShellExtensionManager:session-mode:
   *
   * The session mode: which extensions run depends on it.
   */
  props[PROP_SESSION_MODE] =
    g_param_spec_object ("session-mode", NULL, NULL,
                         SHELL_TYPE_SESSION_MODE,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellExtensionManager:message-tray:
   *
   * The message tray that shows the notification that updates are available.
   */
  props[PROP_MESSAGE_TRAY] =
    g_param_spec_object ("message-tray", NULL, NULL,
                         SHELL_TYPE_MESSAGE_TRAY,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellExtensionManager:settings:
   *
   * The settings with the lists of extensions, `org.gnome.shell` of the shell
   * by default.
   */
  props[PROP_SETTINGS] =
    g_param_spec_object ("settings", NULL, NULL,
                         G_TYPE_SETTINGS,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellExtensionManager:lua-manager:
   *
   * The manager of the Lua extensions, which loads and runs them. The
   * manager of the shell by default.
   */
  props[PROP_LUA_MANAGER] =
    g_param_spec_object ("lua-manager", NULL, NULL,
                         SHELL_TYPE_LUA_EXTENSION_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellExtensionManager::extension-state-changed:
   * @self: the #ShellExtensionManager
   * @extension: the extension
   *
   * The state of an extension changed, or whether it is enabled, whether it
   * can be changed, its error or whether an update is ready. Also emitted when
   * an extension is installed or removed.
   */
  signals[EXTENSION_STATE_CHANGED] =
    g_signal_new ("extension-state-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_LUA_EXTENSION);

  /**
   * ShellExtensionManager::overview-hide-requested:
   * @self: the #ShellExtensionManager
   *
   * The user opened the application that installs the updates from the
   * notification: the overview and the calendar should be closed.
   */
  signals[OVERVIEW_HIDE_REQUESTED] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_extension_manager_init (ShellExtensionManager *self)
{
  self->cancellable = g_cancellable_new ();
  self->order = g_ptr_array_new_with_free_func (g_free);
  self->enabled_uuids = g_ptr_array_new_with_free_func (g_free);
  self->errors = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify) g_ptr_array_unref);
  self->updates = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->stylesheets = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
}

/**
 * shell_extension_manager_new:
 * @session_mode: the session mode
 * @message_tray: (nullable): the message tray
 *
 * Creates the extension manager. src/ui/main.c creates the only instance;
 * shell_extension_manager_start () loads and starts the extensions.
 *
 * Returns: (transfer full): a new #ShellExtensionManager
 */
ShellExtensionManager *
shell_extension_manager_new (ShellSessionMode *session_mode,
                             ShellMessageTray *message_tray)
{
  g_return_val_if_fail (SHELL_IS_SESSION_MODE (session_mode), NULL);

  return g_object_new (SHELL_TYPE_EXTENSION_MANAGER,
                       "session-mode", session_mode,
                       "message-tray", message_tray,
                       NULL);
}
