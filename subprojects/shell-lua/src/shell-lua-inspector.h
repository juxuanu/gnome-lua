/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaInspector: the privileged state behind Looking Glass's Lua tab and
 * the test tool. Introspectable (ShellLua-1.gir).
 */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SHELL_TYPE_LUA_INSPECTOR (shell_lua_inspector_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaInspector, shell_lua_inspector, SHELL, LUA_INSPECTOR, GObject)

ShellLuaInspector *shell_lua_inspector_get_default (void);

/**
 * shell_lua_inspector_eval:
 * @code: a Lua chunk; if it is an expression, "return " is prepended
 * @error: set with the traceback on failure
 *
 * Returns: (transfer full) (nullable): the results rendered one per line
 *   (tables rendered shallowly, handles as "<shell.Type 0x...>"), or NULL
 */
char *shell_lua_inspector_eval (ShellLuaInspector  *self,
                                const char         *code,
                                GError            **error);

/* Reset the privileged state (new lua_State). */
void  shell_lua_inspector_reset (ShellLuaInspector *self);

/* Read-only snapshot for the Looking Glass "Extensions"/"Memory" views:
 * a(sstuu) of (uuid, state-name, lua-bytes, error-count) per live state
 * plus the inspector itself. (transfer full) */
GVariant *shell_lua_inspector_get_states (ShellLuaInspector *self);

G_END_DECLS
