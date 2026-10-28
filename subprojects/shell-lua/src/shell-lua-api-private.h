/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Internal glue between the state core and the API modules. Not installed.
 */
#pragma once

#include "shell-lua.h"
#include "shell-lua-handle.h"
#include "shell-lua-variant.h"

G_BEGIN_DECLS

/* Register an API module. Modules call this from a constructor function
 * (G_DEFINE_CONSTRUCTOR or __attribute__((constructor))) so that linking the
 * library is enough. @name is informational ("core", "hooks", ...). Each
 * registered function is invoked for every new state (both kinds) with the
 * `shell` table on top of the stack. */
void shell_lua_api_register (const char              *name,
                             ShellLuaApiRegisterFunc  func);

/* Called by shell_lua_state_new() after the sandbox is built. (skip) */
void shell_lua_api_populate  (ShellLuaState *state,
                              lua_State     *L);

/* Helpers shared by api-*.c */

/* Push the ShellLuaState that owns @L (from the registry). */
ShellLuaState *shell_lua_state_from_L (lua_State *L);

/* Argument helpers for the "single options table" calling convention used by
 * every shell.* constructor: shell.timer.every{interval=..., callback=...}.
 * All of them raise a Lua error naming the field on type mismatch. */
const char *shell_lua_opt_string  (lua_State *L, int tbl, const char *field, const char *def);
lua_Integer shell_lua_opt_integer (lua_State *L, int tbl, const char *field, lua_Integer def);
lua_Number  shell_lua_opt_number  (lua_State *L, int tbl, const char *field, lua_Number def);
gboolean    shell_lua_opt_boolean (lua_State *L, int tbl, const char *field, gboolean def);
/* Returns a registry ref to the function at tbl.field, or LUA_NOREF if absent
 * (raises if present but not callable; raises if @required and absent). */
int         shell_lua_opt_callback (lua_State *L, int tbl, const char *field, gboolean required);

/* Run a stored callback: pushes ref, pushes nargs args via @push_args, pcalls
 * with the owning state (so watchdog/error policy apply). Results discarded.
 * Returns FALSE if the call failed (already logged and signalled). */
typedef void (*ShellLuaPushArgsFunc) (lua_State *L, gpointer user_data);
gboolean    shell_lua_call_ref (ShellLuaState        *state,
                                int                   ref,
                                int                   nargs,
                                ShellLuaPushArgsFunc  push_args,
                                gpointer              user_data);

/* Module entry points (one per api-*.c), called through the registry; listed
 * here only so tests can register them explicitly in a bare state. */
void shell_lua_api_core_register  (ShellLuaState *state, lua_State *L);
void shell_lua_api_hooks_register (ShellLuaState *state, lua_State *L);

G_END_DECLS
