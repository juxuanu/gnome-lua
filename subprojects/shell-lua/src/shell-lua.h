/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaState: one embedded Lua 5.5 state.
 *
 * Every extension gets its own state (SANDBOXED); Looking Glass and the test
 * runner get a PRIVILEGED one. The state owns the lua_State, the sandbox
 * environment, the require() searcher, the watchdog and the memory accounting.
 * Everything that runs Lua code goes through shell_lua_state_pcall(), which is
 * the only place that arms the watchdog and converts Lua errors into GError
 * with a traceback attached.
 *
 * This library depends on GLib/GIO/json-glib and Lua only. Nothing in here may
 * include Clutter, St, Meta or any other shell header.
 */
#pragma once

#include <gio/gio.h>
#include <glib-object.h>
#include <lua.h>
#include <lauxlib.h>

G_BEGIN_DECLS

#define SHELL_LUA_ERROR (shell_lua_error_quark ())
GQuark shell_lua_error_quark (void);

typedef enum {
  SHELL_LUA_ERROR_SYNTAX,    /* chunk failed to compile */
  SHELL_LUA_ERROR_RUNTIME,   /* error() / runtime fault; message carries the traceback */
  SHELL_LUA_ERROR_WATCHDOG,  /* instruction or wall-clock budget exceeded */
  SHELL_LUA_ERROR_MEMORY,    /* allocator refused: memory_limit exceeded */
  SHELL_LUA_ERROR_SANDBOX,   /* attempted use of a forbidden facility (e.g. require outside roots) */
  SHELL_LUA_ERROR_DEAD,      /* state already destroyed */
} ShellLuaError;

typedef enum {
  SHELL_LUA_STATE_SANDBOXED,   /* extensions: restricted stdlib, see docs/SANDBOX.md */
  SHELL_LUA_STATE_PRIVILEGED,  /* Looking Glass / tests: full stdlib + shell.* */
} ShellLuaStateKind;

/* A zero field means "use the default for the state kind" (see shell-lua.c:
 * SANDBOXED = 10000 instructions, 500 ms, 64 MiB; PRIVILEGED = 10000
 * instructions, unlimited, unlimited). */
typedef struct {
  guint  hook_interval;        /* instructions between watchdog checks (count hook) */
  gint64 callback_budget_us;   /* wall-clock budget for one pcall(), microseconds; G_MAXINT64 = unlimited */
  gsize  memory_limit;         /* bytes the allocator may hand out; G_MAXSIZE = unlimited */
} ShellLuaLimits;

#define SHELL_TYPE_LUA_STATE (shell_lua_state_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaState, shell_lua_state, SHELL, LUA_STATE, GObject)

/* Registration hook for API modules (api-*.c). Called once per new state with
 * the `shell` table on top of the stack; the function must leave the stack
 * balanced. Modules register themselves at library init through
 * shell_lua_api_register() (see shell-lua-api-private.h). */
typedef void (*ShellLuaApiRegisterFunc) (ShellLuaState *state,
                                         lua_State     *L);

ShellLuaState     *shell_lua_state_new            (const char           *id,
                                                   ShellLuaStateKind     kind,
                                                   const ShellLuaLimits *limits);
const char        *shell_lua_state_get_id         (ShellLuaState *self);
ShellLuaStateKind  shell_lua_state_get_kind       (ShellLuaState *self);
gboolean           shell_lua_state_is_alive       (ShellLuaState *self);

/* (skip) raw access; only api-*.c and tests should need it. */
lua_State         *shell_lua_state_get_L          (ShellLuaState *self);

/* Opaque owner pointer, used by the API modules to find the extension that
 * owns a state (NULL for inspector/test states). Not referenced. */
void               shell_lua_state_set_owner      (ShellLuaState *self,
                                                   gpointer       owner);
gpointer           shell_lua_state_get_owner      (ShellLuaState *self);

/* require() resolution. Roots are searched in order; a module name "a.b" maps
 * to "<root>/a/b.lua" then "<root>/a/b/init.lua". A GFile with a resource://
 * URI is a valid root. Nothing outside the roots is loadable; the C loader and
 * package.cpath are absent. "shell" is always resolvable and returns the API
 * table. */
void               shell_lua_state_add_search_root (ShellLuaState *self,
                                                    GFile         *root);

/* Compile a chunk and push the resulting function. Returns FALSE on failure
 * with SHELL_LUA_ERROR_SYNTAX. The chunk runs in the sandbox _ENV. */
gboolean           shell_lua_state_load_file      (ShellLuaState  *self,
                                                   GFile          *file,
                                                   GError        **error);
gboolean           shell_lua_state_load_string    (ShellLuaState  *self,
                                                   const char     *code,
                                                   const char     *chunkname,
                                                   GError        **error);

/* Protected call with traceback handler and watchdog. The function and nargs
 * arguments must be on the stack. On success nresults values are left
 * (LUA_MULTRET allowed). On failure the error object is popped, converted to
 * a GError and the "error" signal is emitted; the error counter increments.
 * Re-entrant: a C function called from Lua may call back into pcall. */
gboolean           shell_lua_state_pcall          (ShellLuaState  *self,
                                                   int             nargs,
                                                   int             nresults,
                                                   GError        **error);

/* Convenience: load + pcall with no arguments, discarding results. */
gboolean           shell_lua_state_do_string      (ShellLuaState  *self,
                                                   const char     *code,
                                                   const char     *chunkname,
                                                   GError        **error);
gboolean           shell_lua_state_do_file        (ShellLuaState  *self,
                                                   GFile          *file,
                                                   GError        **error);

/* Registry references for Lua values held by C (callbacks). ref pops the
 * value on top of the stack. */
int                shell_lua_state_ref            (ShellLuaState *self);
void               shell_lua_state_unref          (ShellLuaState *self,
                                                   int            ref);
void               shell_lua_state_push_ref       (ShellLuaState *self,
                                                   int            ref);

/* Teardown callbacks run (LIFO) when the state is destroyed, before the
 * lua_State is closed. Used by API modules to release C-side resources that
 * Lua code created. Returns an id for early removal. */
guint              shell_lua_state_add_teardown   (ShellLuaState  *self,
                                                   GDestroyNotify  func,
                                                   gpointer        data);
void               shell_lua_state_remove_teardown (ShellLuaState *self,
                                                    guint          id);

/* Explicit destruction: runs teardowns, closes the lua_State, emits
 * "destroyed". Idempotent. Also happens on dispose. */
void               shell_lua_state_destroy        (ShellLuaState *self);

/* Statistics, consumed by Looking Glass. */
gsize              shell_lua_state_get_memory_usage (ShellLuaState *self);
guint              shell_lua_state_get_error_count  (ShellLuaState *self);
void               shell_lua_state_reset_error_count (ShellLuaState *self);
void               shell_lua_state_gc               (ShellLuaState *self);

/* The state whose code is executing right now (innermost pcall), or NULL.
 * Used by API modules and by the fatal-signal handler. */
ShellLuaState     *shell_lua_state_get_current    (void);

/* Async-signal-safe-ish dump of the current state's Lua traceback to stderr.
 * Called from the fatal signal handler in main.c; must not allocate. */
void               shell_lua_dump_stack           (void);

/* Process-wide: number of live states and total Lua heap, for ShellMemoryInfo. */
guint              shell_lua_get_state_count      (void);
gsize              shell_lua_get_total_memory     (void);

/* Signals on ShellLuaState:
 *   "error"     (GError *error)  -- every failed pcall
 *   "destroyed" ()               -- after the lua_State is closed
 */

G_END_DECLS
