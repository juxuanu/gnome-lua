/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaConnection and the small helpers shared by the api-*.c modules
 * that create C objects on behalf of Lua code (timers, connections,
 * settings). Not installed.
 */
#pragma once

#include <glib-object.h>
#include <lua.h>
#include <lauxlib.h>

#include "shell-lua.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellLuaConnection: what shell.Connection handles wrap                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LUA_CONNECTION (shell_lua_connection_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaConnection, shell_lua_connection, SHELL, LUA_CONNECTION, GObject)

/* @callback_ref is the registry ref of the Lua callback (or LUA_NOREF); the
 * connection takes it over and releases it on disconnect. @disconnect_fn is
 * called at most once with @data when the connection is disconnected (by
 * Lua, by dispose, or because the state went away); it must undo whatever
 * @data represents. @state is not referenced: it must outlive the connection
 * unless the owner (extension / state teardown) disposes it first, which is
 * what shell_lua_connection_own() arranges. */
ShellLuaConnection *shell_lua_connection_new          (ShellLuaState *state,
                                                       int            callback_ref,
                                                       GDestroyNotify disconnect_fn,
                                                       gpointer       data);
void                shell_lua_connection_disconnect   (ShellLuaConnection *self);
gboolean            shell_lua_connection_is_connected (ShellLuaConnection *self);
/* LUA_NOREF once disconnected */
int                 shell_lua_connection_get_callback_ref (ShellLuaConnection *self);

/* Register @self with the owner of its state (extension or state teardown).
 * Disconnecting later releases the ownership again. */
void                shell_lua_connection_own          (ShellLuaConnection *self);

/* Idempotent; the "shell.Connection" handle type of @L. */
void                shell_lua_connection_register_handle_type (lua_State *L);
/* Pushes a shell.Connection handle for @self (registers the type if needed
 * and keeps @self alive as long as the handle is reachable from Lua). */
void                shell_lua_connection_push         (ShellLuaConnection *self,
                                                       lua_State          *L);
/* shell_lua_connection_push() for a C function that returns the handle
 * (`return shell_lua_connection_push_take (connection, L);`): it takes over
 * the caller's reference and drops it whether or not the push raised (an
 * out-of-memory error must not leak the connection). Returns 1. */
int                 shell_lua_connection_push_take    (ShellLuaConnection *self,
                                                       lua_State          *L);

/* ------------------------------------------------------------------------ */
/* Ownership                                                                 */
/* ------------------------------------------------------------------------ */

/* Token for one ownership registration. Zero-initialise before use. */
typedef struct {
  ShellLuaState *state;
  guint          id;        /* 0 = not owned */
  gboolean       extension; /* TRUE: shell_lua_extension_own(), FALSE: state teardown */
  GObject       *object;    /* teardown path: the reference held for the state */
} ShellLuaOwnToken;

/* Keep @object alive and dispose it when the owner goes away: the owning
 * ShellLuaExtension (shell_lua_extension_own) if the state has one, else the
 * state's teardown list. Takes its own reference. */
void     shell_lua_api_own    (ShellLuaState    *state,
                               GObject          *object,
                               ShellLuaOwnToken *token);
/* Early release of an object that is done (timer fired, connection
 * disconnected). Must not be called while the object is being disposed by
 * its owner. Safe on a zeroed or already released token. */
void     shell_lua_api_disown (ShellLuaOwnToken *token);

/* Keep @object alive for as long as the Lua value at @idx (a handle) is
 * reachable. Lets a handle to a finished timer/connection stay valid, so that
 * calling cancel()/disconnect() on it again is harmless instead of raising. */
void     shell_lua_api_anchor (lua_State *L,
                               int        idx,
                               GObject   *object);

/* ------------------------------------------------------------------------ */
/* Calling into Lua from C callbacks                                         */
/* ------------------------------------------------------------------------ */

/* @fn runs inside a Lua C function, itself called through
 * shell_lua_state_pcall(), so it may raise freely (argument conversion,
 * out-of-memory) and runs under the watchdog and the error policy. It must
 * leave the values it wants to return on the stack and return their count.
 * On success @nresults values (LUA_MULTRET allowed) are left on the state's
 * main stack for the caller, which must pop them. Returns FALSE if the state
 * is dead or the call failed (the error has already been logged and
 * signalled). */
typedef int (*ShellLuaProtectedFunc) (lua_State *L,
                                      gpointer   data);

gboolean shell_lua_api_call_protected (ShellLuaState        *state,
                                       int                   nresults,
                                       ShellLuaProtectedFunc fn,
                                       gpointer              data);

G_END_DECLS
