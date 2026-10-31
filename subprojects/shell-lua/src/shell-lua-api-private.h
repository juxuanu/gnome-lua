/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Internal glue between the state core and the API modules. Not installed.
 */
#pragma once

#include "shell-lua.h"
#include "shell-lua-extension.h"
#include "shell-lua-handle.h"
#include "shell-lua-variant.h"

G_BEGIN_DECLS

/* Register an API module. Modules call this from a constructor function
 * (G_DEFINE_CONSTRUCTOR or __attribute__((constructor))) so that linking the
 * library is enough. @name is informational ("core", "hooks", ...). Each
 * registered function is invoked for every new SANDBOXED or PRIVILEGED state
 * with the `shell` table on top of the stack. A module that also serves
 * WORKER states (which are populated from a worker thread, so the function
 * must touch nothing shared) registers with shell_lua_api_register_for_kinds
 * and SHELL_LUA_API_KINDS_ALL. */
void shell_lua_api_register (const char              *name,
                             ShellLuaApiRegisterFunc  func);

#define SHELL_LUA_API_KIND(kind) (1u << (kind))
#define SHELL_LUA_API_KINDS_MAIN (SHELL_LUA_API_KIND (SHELL_LUA_STATE_SANDBOXED) | \
                                  SHELL_LUA_API_KIND (SHELL_LUA_STATE_PRIVILEGED))
#define SHELL_LUA_API_KINDS_ALL  (SHELL_LUA_API_KINDS_MAIN | SHELL_LUA_API_KIND (SHELL_LUA_STATE_WORKER))

void shell_lua_api_register_for_kinds (const char              *name,
                                       ShellLuaApiRegisterFunc  func,
                                       guint                    kinds);

/* Called by shell_lua_state_new() after the sandbox is built. (skip) */
void shell_lua_api_populate  (ShellLuaState *state,
                              lua_State     *L);

/* Helpers shared by api-*.c */

/* Push the ShellLuaState that owns @L (from the registry). */
ShellLuaState *shell_lua_state_from_L (lua_State *L);

/* The rule of require () for module names (SANDBOX.md): letters, digits,
 * `_` and `.` with no empty component, at most 255 bytes. */
gboolean       shell_lua_module_name_is_valid (const char *name);

/* The require () roots of a state, in order (GFile, transfer full): a worker
 * gets the roots of the state that starts it. */
GPtrArray     *shell_lua_state_dup_search_roots (ShellLuaState *state);

/* Where print () and shell.log of a state write when a sink is set: worker
 * states relay their messages to the main thread this way. @level is the
 * GLib level (G_LOG_LEVEL_MESSAGE for print). The sink may raise a Lua
 * error in @L (a stopped worker). shell_lua_state_sink_message () writes
 * through the sink and returns FALSE when the state has none. */
typedef void (* ShellLuaMessageSink) (lua_State      *L,
                                      GLogLevelFlags  level,
                                      const char     *message,
                                      gpointer        data);
void           shell_lua_state_set_message_sink (ShellLuaState       *state,
                                                 ShellLuaMessageSink  sink,
                                                 gpointer             data);
gboolean       shell_lua_state_sink_message     (ShellLuaState  *state,
                                                 lua_State      *L,
                                                 GLogLevelFlags  level,
                                                 const char     *message);

/* shell.log's writer (shell-lua-api-core.c): one message of the extension
 * @id to the GLib log, domain "shell-lua/<id>", never fatal. Main thread
 * only (it lowers the process-wide fatal mask around the call). */
void           shell_lua_write_extension_log (const char     *id,
                                              GLogLevelFlags  level,
                                              const char     *message);

/* Building the read-only tables of shell.extension (shell-lua-api-core.c):
 * push a JsonNode as Lua values (objects and arrays read-only), and replace
 * the table on top of the stack by a read-only proxy. */
void           shell_lua_push_json_node (lua_State *L,
                                         JsonNode  *node);
void           shell_lua_make_readonly  (lua_State *L);

/* A trampoline that runs an extension's function from a protected call
 * calls this right before lua_call () with the stack index of the function:
 * the time accounting reports slow calls by the function they ran
 * (ShellLuaStats.longest_where). Does nothing for a nested call. */
void           shell_lua_state_note_callee (lua_State *L,
                                            int        idx);

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

/* Raise "unknown field 'x'" for any key of the table at @tbl that is not in
 * the NULL-terminated @allowed list (README calling convention 1). */
void        shell_lua_check_fields (lua_State         *L,
                                    int                tbl,
                                    const char *const *allowed);

/* Module entry points (one per api-*.c), called through the registry; listed
 * here only so tests can register them explicitly in a bare state. */
void shell_lua_api_core_register  (ShellLuaState *state, lua_State *L);
void shell_lua_api_hooks_register (ShellLuaState *state, lua_State *L);
void shell_lua_api_dbus_register  (ShellLuaState *state, lua_State *L);
void shell_lua_api_fs_register    (ShellLuaState *state, lua_State *L);
void shell_lua_api_spawn_register (ShellLuaState *state, lua_State *L);

/* shell.extension.data_dir (shell-lua-api-fs.c): lazily created on first read.
 * Called with the table behind shell.extension on top of the stack. */
void shell_lua_fs_install_data_dir (lua_State *L);
void shell_lua_api_time_register  (ShellLuaState *state, lua_State *L);
void shell_lua_api_worker_register (ShellLuaState *state, lua_State *L);

/* shell.worker (shell-lua-api-worker.c): what the workers of a state cost
 * (ShellLuaWorkerStats, shell-lua-extension.h). @state is the extension's
 * state (NULL allowed). Zeroes when it never started one. */
void shell_lua_worker_get_stats (ShellLuaState       *state,
                                 ShellLuaWorkerStats *stats);

/* Tests: the shell-wide limit of live workers (default max (8, 2 * CPUs)),
 * and the number of worker threads that have not exited yet. */
void  shell_lua_worker_set_limit      (guint limit);
guint shell_lua_worker_get_limit      (void);
guint shell_lua_worker_get_live_count (void);
void  shell_lua_worker_set_max_message_values (gsize max_values);   /* 0: the default */


/* ======================================================================== *
 * Runtime API for the modules of libshell (src/lua-api/, M7)
 *
 * The rest of this file is what a module that wraps shell objects needs:
 * the extension of a state, capabilities, owned objects with a destroy rule
 * and a slot cap, callbacks bound to an owned object (never run after
 * disable begins), the "emitting" deferral of
 * README rule 8, and the handle type registration (shell-lua-handle.h: a
 * parent type, methods with flags, properties and virtual properties).
 *
 * Typical constructor of a widget type:
 *
 *   static int l_label (lua_State *L)
 *   {
 *     StWidget *w;
 *     shell_lua_check_fields (L, 1, label_fields);
 *     shell_lua_check_owned_capacity (L);              // raises at the cap
 *     w = g_object_new (ST_TYPE_LABEL, ...);           // floating
 *     shell_lua_own_object (L, G_OBJECT (w), &widget_ops);  // sinks, owns
 *     shell_lua_handle_push (L, "shell.Label", G_OBJECT (w));
 *     return 1;
 *   }
 *
 * and the destroy method of every owning handle type is
 *   { "destroy", shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK }
 * in the type's methods_ex.
 * ======================================================================== */

/* The extension that owns the state of @L, or NULL (inspector, test states). */
ShellLuaExtension *shell_lua_extension_from_L (lua_State *L);

/* Raise `<function> requires the "<capability>" capability` unless the
 * extension declares it. States without an extension pass when PRIVILEGED. */
void        shell_lua_require_capability (lua_State  *L,
                                          const char *capability,
                                          const char *function);

/* ---- Owned objects ------------------------------------------------------ */

/* At most this many live owned objects per state; the next shell_lua_own_object
 * raises `too many objects (limit 4096)`. */
#define SHELL_LUA_MAX_OWNED_OBJECTS 4096

/* How an owned object is destroyed. */
typedef void (* ShellLuaDestroyFunc) (GObject *object);

typedef struct {
  /* Really destroys the object, so that it stops being visible and its
   * children die with it (clutter_actor_destroy, ...). NULL means
   * g_object_run_dispose(). */
  ShellLuaDestroyFunc  destroy;
  /* Name of a signal the object emits when it is destroyed from elsewhere (its
   * parent was destroyed, the shell removed it): "destroy" for a
   * ClutterActor. The slot is then freed and the handles die. NULL: none.
   * The emitter must keep the instance alive during the emission. */
  const char          *destroyed_signal;
} ShellLuaOwnOps;

/* Raise unless one more object can be owned (call before creating one). */
void        shell_lua_check_owned_capacity (lua_State *L);

/* Make the extension context own @object: g_object_ref_sink() semantics. A
 * floating object (a new actor) has its floating reference sunk into the
 * context's: the caller must NOT unref it. Any other object gets one more
 * reference, and the caller keeps (and releases) its own. Objects are
 * destroyed in reverse creation order when the extension is disabled. Beyond
 * the cap the object is destroyed again and a Lua error is raised: use
 * shell_lua_check_owned_capacity() before creating anything. Owning an
 * object twice is a no-op. */
void        shell_lua_own_object (lua_State            *L,
                                  GObject              *object,
                                  const ShellLuaOwnOps *ops);

gboolean    shell_lua_object_is_owned (lua_State *L,
                                       GObject   *object);
guint       shell_lua_n_owned_objects (lua_State *L);

/* Destroy an owned object now: kills its callbacks, runs ops->destroy, frees
 * its slot immediately (so it stops counting against the cap), turns every
 * handle to it dead. Idempotent: FALSE when @object is not owned (any more).
 * README rule 8: if @object, or an ancestor of it (see
 * shell_lua_set_contains_func), is emitting a callback right now, the
 * destruction is deferred until that callback returns and the handle stays
 * valid until then (still returns TRUE). */
gboolean    shell_lua_destroy_object (lua_State *L,
                                      GObject   *object);

/* The Lua C function for `h:destroy()`: list it in methods_ex with
 * SHELL_LUA_METHOD_DEAD_OK. Does nothing on a dead handle. */
int         shell_lua_handle_destroy_method (lua_State *L);

/* Tell the runtime how containment works so that destroying or reparenting
 * an *ancestor* of the emitting object is deferred too (libshell sets
 * clutter_actor_contains()). @ancestor itself counts as containing itself. */
typedef gboolean (* ShellLuaContainsFunc) (GObject *ancestor,
                                           GObject *descendant);
void        shell_lua_set_contains_func (ShellLuaContainsFunc func);

/* ---- Callbacks bound to an object ---------------------------------------- */

/* A Lua function stored for an event of an owned object. Reference counted:
 * the owner's slot holds one reference, and every shell_lua_callback_connect()
 * one more (dropped by the signal's closure notify). When the owner is
 * destroyed the callback is killed: its Lua reference is released and every
 * emit returns FALSE, so a handler still connected to a dying instance is
 * harmless. */
typedef struct _ShellLuaCallback ShellLuaCallback;

/* Takes over the registry @ref (from shell_lua_opt_callback or luaL_ref). The
 * returned pointer is (transfer none): it lives as long as the owner or a
 * connection holds it; take shell_lua_callback_ref() to keep it longer.
 * @owner must already be owned (shell_lua_own_object); it is passed to
 * callbacks as their first argument (README rule 6). */
ShellLuaCallback *shell_lua_callback_new   (lua_State *L,
                                            GObject   *owner,
                                            int        ref);
ShellLuaCallback *shell_lua_callback_ref   (ShellLuaCallback *cb);
void              shell_lua_callback_unref (ShellLuaCallback *cb);
gboolean          shell_lua_callback_is_live (ShellLuaCallback *cb);
/* Release the Lua function now (disconnect()); emits become no-ops. */
void              shell_lua_callback_kill  (ShellLuaCallback *cb);

/* g_signal_connect_data() with @cb as user data (the C handler gets it as its
 * last argument); the connection holds a reference on @cb. Returns the handler
 * id. */
gulong            shell_lua_callback_connect (ShellLuaCallback *cb,
                                              gpointer          instance,
                                              const char       *detailed_signal,
                                              GCallback         c_handler);

/* Call the function as `fn(owner_handle, args...)` (shapes: "handles that are
 * something on screen pass the emitting handle first"). @nargs counts the
 * arguments pushed by @push_args, not the handle. Returns FALSE without
 * calling when the callback is dead or the extension is being disabled; also
 * FALSE if the call failed (already logged, counted and signalled). The
 * callback runs whatever caused the event, an assignment made by Lua code
 * included. While it runs the owner counts as emitting (rule 8). */
gboolean          shell_lua_callback_emit  (ShellLuaCallback    *cb,
                                            int                  nargs,
                                            ShellLuaPushArgsFunc push_args,
                                            gpointer             user_data);

/* Same without the handle: `fn(args...)` (one-shot completions, data
 * sources). The disable rule applies. */
gboolean          shell_lua_callback_emit_data (ShellLuaCallback    *cb,
                                                int                  nargs,
                                                ShellLuaPushArgsFunc push_args,
                                                gpointer             user_data);

/* Pushes the shell.Connection (owned by the extension) of an `h:on_<event>`
 * call: disconnect() disconnects @handler_id on @instance and kills @cb.
 * @instance is held weakly. Returns 1 (for `return shell_lua_callback_push_connection (...)`). */
int               shell_lua_callback_push_connection (lua_State        *L,
                                                      ShellLuaCallback *cb,
                                                      gpointer          instance,
                                                      gulong            handler_id);

/* Run an event callback whose emitter is not a callback's owner, with the
 * same rules (blocked, emitting frame). The handle of @emitter is
 * pushed first when @emitter is not NULL. */
gboolean          shell_lua_emit_ref (ShellLuaState        *state,
                                      GObject              *emitter,
                                      int                   ref,
                                      int                   nargs,
                                      ShellLuaPushArgsFunc  push_args,
                                      gpointer              user_data);

/* ---- Rule 8 (destroy / reparent the emitter) ---------------------------- */

typedef void (* ShellLuaDeferredFunc) (gpointer data);

/* If @object (or an ancestor of it) is emitting a callback right now, queue
 * @func(@data) to run when that callback returns, free @data with @notify
 * afterwards, and return TRUE: the caller must then return without acting.
 * Otherwise return FALSE and do nothing (the caller acts now; @data stays its
 * own). Never defers while the extension is being disabled. */
gboolean          shell_lua_defer_if_emitting (lua_State           *L,
                                               GObject             *object,
                                               ShellLuaDeferredFunc func,
                                               gpointer             data,
                                               GDestroyNotify       notify);

/* ---- Teardown (used by ShellLuaExtension; also by tests) ---------------- */

/* From now on no callback runs for this state (call_ref, emit and
 * call_protected return FALSE). Called when disable begins. */
void              shell_lua_objects_block      (ShellLuaState *state);
gboolean          shell_lua_objects_is_blocked (ShellLuaState *state);
/* Destroy every owned object, newest first. Also runs at state destruction. */
void              shell_lua_objects_destroy_all (ShellLuaState *state);

G_END_DECLS
