/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Internal glue between ShellLuaExtensionManager and ShellLuaExtension.
 * Not installed.
 *
 * Extra signals on ShellLuaExtension (not part of the public header):
 *   "state-changed" ()            -- state, error message, enabled or
 *                                    can-change flag changed
 *   "lua-error"     (GError *)    -- an error was raised by any pcall in the
 *                                    extension's state (re-emitted from the
 *                                    ShellLuaState "error" signal for the
 *                                    whole lifetime of the extension,
 *                                    including during enable())
 */
#pragma once

#include "shell-lua-extension.h"

G_BEGIN_DECLS

/* Parses and validates <dir>/metadata.json. Returns NULL with @error set when
 * the directory cannot be used at all (missing/invalid metadata, uuid not
 * matching the directory name, unknown capability, ...). A "shell-api" whose
 * major is not 1 is not an error: the object is returned in state OUT_OF_DATE
 * with the error message "shell-api %s not supported". Otherwise the object
 * is returned in state INITIALIZED. (transfer full) */
ShellLuaExtension *shell_lua_extension_new_from_dir (GFile                  *dir,
                                                     ShellLuaExtensionType   type,
                                                     GError                **error);

/* Limits for the ShellLuaState created at enable (NULL = defaults). */
void        shell_lua_extension_set_limits      (ShellLuaExtension    *self,
                                                 const ShellLuaLimits *limits);

/* The "enabled" and "canChange" fields of org.gnome.Shell.Extensions. They
 * mirror the org.gnome.shell settings; they do not start or stop anything. */
void        shell_lua_extension_set_enabled     (ShellLuaExtension *self,
                                                 gboolean           enabled);
gboolean    shell_lua_extension_get_enabled     (ShellLuaExtension *self);
void        shell_lua_extension_set_can_change  (ShellLuaExtension *self,
                                                 gboolean           can_change);

/* Number of errors raised by the current (or last) state since enable(). */
guint       shell_lua_extension_get_error_count (ShellLuaExtension *self);

/* Tear the runtime down (calling Lua disable() if the state is live) and put
 * the extension in state ERROR with @message. Used by the error policy. */
void        shell_lua_extension_fail            (ShellLuaExtension *self,
                                                 const char        *message);

/* Disables a live extension, then marks it UNINSTALLED. */
void        shell_lua_extension_mark_uninstalled (ShellLuaExtension *self);

/* "ACTIVE", "INACTIVE", ... (same names as the JS ExtensionState keys) */
const char *shell_lua_extension_state_to_string (ShellLuaExtensionState state);

G_END_DECLS
