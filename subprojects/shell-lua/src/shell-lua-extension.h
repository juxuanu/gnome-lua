/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaExtension: one installed Lua extension and its runtime context.
 * ShellLuaExtensionManager: discovery, enable/disable, error policy.
 *
 * Layout of an extension directory (same as today's JS extensions):
 *   metadata.json   {"uuid", "name", "description", "shell-api": "1",
 *                    "capabilities": ["dbus", ...] (optional),
 *                    "settings-schema" (optional)}
 *   extension.lua   returns a table with optional enable()/disable()
 *   prefs.lua       (optional) returns a prefs schema table
 *   stylesheet.css, schemas/, locale/   (optional)
 *
 * The context owns everything the extension creates through shell.*:
 * shell_lua_extension_own() registers an object to be disposed on disable.
 */
#pragma once

#include <gio/gio.h>
#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "shell-lua.h"

G_BEGIN_DECLS

/* Mirrors the JS ExtensionState values so org.gnome.Shell.Extensions keeps
 * reporting the same numbers. */
typedef enum {
  SHELL_LUA_EXTENSION_STATE_ACTIVE        = 1,
  SHELL_LUA_EXTENSION_STATE_INACTIVE      = 2,
  SHELL_LUA_EXTENSION_STATE_ERROR         = 3,
  SHELL_LUA_EXTENSION_STATE_OUT_OF_DATE   = 4,
  SHELL_LUA_EXTENSION_STATE_DOWNLOADING   = 5,
  SHELL_LUA_EXTENSION_STATE_INITIALIZED   = 6,
  SHELL_LUA_EXTENSION_STATE_DEACTIVATING  = 7,
  SHELL_LUA_EXTENSION_STATE_ACTIVATING    = 8,
  SHELL_LUA_EXTENSION_STATE_UNINSTALLED   = 99,
} ShellLuaExtensionState;

typedef enum {
  SHELL_LUA_EXTENSION_TYPE_SYSTEM = 1,
  SHELL_LUA_EXTENSION_TYPE_PER_USER = 2,
} ShellLuaExtensionType;

#define SHELL_TYPE_LUA_EXTENSION (shell_lua_extension_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaExtension, shell_lua_extension, SHELL, LUA_EXTENSION, GObject)

const char             *shell_lua_extension_get_uuid        (ShellLuaExtension *self);
/**
 * shell_lua_extension_get_dir:
 *
 * Returns: (transfer none): the extension directory
 */
GFile                  *shell_lua_extension_get_dir         (ShellLuaExtension *self);
ShellLuaExtensionType   shell_lua_extension_get_extension_type (ShellLuaExtension *self);
ShellLuaExtensionState  shell_lua_extension_get_state       (ShellLuaExtension *self);
/* (nullable) last error message, cleared on successful enable */
const char             *shell_lua_extension_get_error       (ShellLuaExtension *self);
/* (transfer none) parsed metadata.json */
/**
 * shell_lua_extension_get_metadata:
 *
 * Returns: (transfer none): the parsed metadata.json
 */
JsonObject             *shell_lua_extension_get_metadata    (ShellLuaExtension *self);
/* (transfer none) (nullable) alive only between enable and disable */
/**
 * shell_lua_extension_get_lua_state:
 *
 * Returns: (transfer none) (nullable): the state, alive only while enabled
 */
ShellLuaState          *shell_lua_extension_get_lua_state   (ShellLuaExtension *self);

/* What the workers of an extension cost (docs/api/worker.md "Time
 * accounting"), next to the ShellLuaStats of its state, never added to them:
 * the worker threads run off the main thread. */
typedef struct {
  guint64 live;          /* workers whose thread has not exited */
  guint64 messages;      /* messages its workers handled */
  gint64  total_us;      /* CPU time of the worker threads in them, microseconds */
  gint64  longest_us;    /* the longest handler */
  gint64  memory;        /* bytes the Lua states of its live workers use now */
} ShellLuaWorkerStats;

gboolean                shell_lua_extension_get_worker_stats (ShellLuaExtension   *self,
                                                              ShellLuaWorkerStats *stats);
/* (transfer none) */
/**
 * shell_lua_extension_get_capabilities:
 *
 * Returns: (transfer none) (array zero-terminated=1): declared capabilities
 */
const char * const     *shell_lua_extension_get_capabilities (ShellLuaExtension *self);
gboolean                shell_lua_extension_has_capability  (ShellLuaExtension *self,
                                                             const char        *capability);
/* (transfer none) (nullable) GSettings for "settings-schema" in metadata, with
 * the extension's schemas/ dir compiled in; created lazily. */
/**
 * shell_lua_extension_get_settings:
 *
 * Returns: (transfer none) (nullable): the extension's settings
 */
GSettings              *shell_lua_extension_get_settings    (ShellLuaExtension *self);

/* Lifecycle. enable() creates the state, runs extension.lua, calls enable().
 * disable() calls disable() if present, disposes owned objects (LIFO), then
 * destroys the state. Both return FALSE and set state ERROR on failure. */
gboolean                shell_lua_extension_enable          (ShellLuaExtension  *self,
                                                             GError            **error);
gboolean                shell_lua_extension_disable         (ShellLuaExtension  *self,
                                                             GError            **error);

/* Ownership of resources created on behalf of this extension. @object is
 * ref'd; on disable it is run through @dispose (default: g_object_run_dispose)
 * then unref'd. Returns an id usable to release early. (skip) */
/**
 * shell_lua_extension_own: (skip)
 */
guint                   shell_lua_extension_own             (ShellLuaExtension *self,
                                                             GObject           *object,
                                                             GDestroyNotify     dispose);
/**
 * shell_lua_extension_own_full: (skip)
 */
guint                   shell_lua_extension_own_full        (ShellLuaExtension *self,
                                                             gpointer           data,
                                                             GDestroyNotify     destroy);
/**
 * shell_lua_extension_release: (skip)
 */
void                    shell_lua_extension_release         (ShellLuaExtension *self,
                                                             guint              id);

/* (transfer full) the a{sv} dictionary org.gnome.Shell.Extensions reports */
/**
 * shell_lua_extension_serialize:
 *
 * Returns: (transfer full): the a{sv} dictionary org.gnome.Shell.Extensions reports
 */
GVariant               *shell_lua_extension_serialize       (ShellLuaExtension *self);

#define SHELL_TYPE_LUA_EXTENSION_MANAGER (shell_lua_extension_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaExtensionManager, shell_lua_extension_manager, SHELL, LUA_EXTENSION_MANAGER, GObject)

/**
 * shell_lua_extension_manager_get_default:
 *
 * Returns: (transfer none): the singleton manager
 */
ShellLuaExtensionManager *shell_lua_extension_manager_get_default (void);

/* Search directories: $XDG_DATA_HOME/gnome-shell/extensions (PER_USER) then
 * each $XDG_DATA_DIRS/gnome-shell/extensions (SYSTEM). Tests override with
 * set_search_dirs. Only directories containing metadata.json + extension.lua
 * are considered. */
/**
 * shell_lua_extension_manager_set_search_dirs:
 * @dirs: (array length=n_dirs): directories to scan, first is per-user
 * @n_dirs: length of @dirs
 */
void       shell_lua_extension_manager_set_search_dirs (ShellLuaExtensionManager *self,
                                                        GFile * const            *dirs,
                                                        gsize                     n_dirs);
void       shell_lua_extension_manager_scan            (ShellLuaExtensionManager *self);
/* Forget an extension and read its directory again (new code, new metadata);
 * the result is not enabled. */
gboolean   shell_lua_extension_manager_reload          (ShellLuaExtensionManager  *self,
                                                        const char                *uuid,
                                                        GError                   **error);
/* (transfer none) (nullable) */
/**
 * shell_lua_extension_manager_lookup:
 *
 * Returns: (transfer none) (nullable): the extension with @uuid
 */
ShellLuaExtension *shell_lua_extension_manager_lookup  (ShellLuaExtensionManager *self,
                                                        const char               *uuid);
/* (transfer container) (element-type ShellLuaExtension) */
/**
 * shell_lua_extension_manager_list:
 *
 * Returns: (transfer container) (element-type ShellLuaExtension): installed extensions
 */
GList     *shell_lua_extension_manager_list            (ShellLuaExtensionManager *self);
gboolean   shell_lua_extension_manager_enable          (ShellLuaExtensionManager  *self,
                                                        const char                *uuid,
                                                        GError                   **error);
gboolean   shell_lua_extension_manager_disable         (ShellLuaExtensionManager  *self,
                                                        const char                *uuid,
                                                        GError                   **error);
/* Enable everything listed in org.gnome.shell enabled-extensions that is
 * installed and not in disabled-extensions; disable the rest. Also follows
 * the "disable-user-extensions" key. Safe to call repeatedly. Called by the
 * shell once the session mode allows extensions. */
void       shell_lua_extension_manager_sync            (ShellLuaExtensionManager *self);
/* Runtime errors above this many in one extension disable it (default 5). */
void       shell_lua_extension_manager_set_error_limit (ShellLuaExtensionManager *self,
                                                        guint                     limit);
/* Per-extension-state limits applied at enable (NULL = defaults). */
/**
 * shell_lua_extension_manager_set_limits:
 * @limits: (nullable): limits applied at enable, %NULL for defaults
 */
void       shell_lua_extension_manager_set_limits      (ShellLuaExtensionManager *self,
                                                        const ShellLuaLimits     *limits);

/* Signals on ShellLuaExtensionManager:
 *   "extension-state-changed" (ShellLuaExtension *extension)
 *   "extensions-loaded" ()
 */

G_END_DECLS
