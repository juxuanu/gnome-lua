/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaInspector: the privileged state behind Looking Glass's Evaluator
 * and the test tool. Introspectable (ShellLua-1.gir).
 *
 * The inspector state is not sandboxed: it has the full standard library and
 * `shell.*`, plus these globals:
 *
 *   r (n)    the n-th result of the Evaluator (1-based), an error if it is
 *            unknown; `it` is the last result
 *   print    captured: what it prints is returned by
 *            shell_lua_inspector_evaluate ()
 *   plus whatever the host registered with
 *   shell_lua_inspector_set_global_object () and
 *   shell_lua_inspector_add_function ().
 *
 * Objects (GObject instances the host hands in, and the objects the
 * properties of those return) are seen from Lua as generic handles: reading
 * `obj.some_property` (dashes in the name of the GObject property are
 * underscores) gets the property, `obj.some_property = v` sets it, and
 * `obj:gtype ()`, `obj:properties ()` plus the methods the host added with
 * shell_lua_inspector_add_object_methods () are available. There is no other
 * access to the C side: Looking Glass is a debugging tool, extensions never
 * see this.
 */
#pragma once

#include <glib-object.h>
#include <lua.h>
#include <lauxlib.h>

G_BEGIN_DECLS

#define SHELL_TYPE_LUA_INSPECTOR (shell_lua_inspector_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaInspector, shell_lua_inspector, SHELL, LUA_INSPECTOR, GObject)

/**
 * shell_lua_inspector_get_default:
 *
 * Returns: (transfer none): the singleton inspector
 */
ShellLuaInspector *shell_lua_inspector_get_default (void);

/**
 * shell_lua_inspector_eval:
 * @code: a Lua chunk; if it is an expression, "return " is prepended
 * @error: set with the traceback on failure
 *
 * Evaluates @code and renders its results, one per line (tables rendered
 * shallowly, handles as "<gobject.Type 0x...>"). The results are not
 * recorded, see shell_lua_inspector_evaluate () for that.
 *
 * Returns: (transfer full) (nullable): the results rendered one per line, or NULL
 */
char *shell_lua_inspector_eval (ShellLuaInspector  *self,
                                const char         *code,
                                GError            **error);

/* Reset the privileged state (new lua_State). */
void  shell_lua_inspector_reset (ShellLuaInspector *self);

/* Read-only snapshot for the Looking Glass "Extensions"/"Memory" views:
 * a(sstuu) of (uuid, state-name, lua-bytes, error-count, state-number) per live state
 * plus the inspector itself. (transfer full) */
/**
 * shell_lua_inspector_get_states:
 *
 * Returns: (transfer full): a(sstuu) of (uuid, state-name, lua-bytes, error-count, state-number)
 */
GVariant *shell_lua_inspector_get_states (ShellLuaInspector *self);

/* ------------------------------------------------------------------------ */
/* The Evaluator                                                            */
/* ------------------------------------------------------------------------ */

/**
 * shell_lua_inspector_evaluate:
 * @code: a Lua chunk; if it is an expression, "return " is prepended
 * @output: (out) (optional) (transfer full): what the chunk printed with
 *   `print ()`, also when it failed
 * @error: set with the message and the traceback on failure
 *
 * Evaluates @code in the inspector state and records every value it returns
 * as a result: it gets the next result number n, becomes `r (n)`, and the
 * last one becomes `it`. Results stay until they are released with
 * shell_lua_inspector_release ().
 *
 * Returns: (transfer full) (nullable): an array of (result number, type name,
 *   one line summary), type `a(tss)`, one entry per value; %NULL on error
 */
GVariant *shell_lua_inspector_evaluate (ShellLuaInspector  *self,
                                        const char         *code,
                                        char              **output,
                                        GError            **error);

/**
 * shell_lua_inspector_inspect:
 * @id: a result number, or the id of a member returned by an earlier call
 * @error: set when @id is unknown (it was released or the state was reset)
 *
 * Describes a recorded value: its type, a one line summary and its members.
 * The members of a table are its entries, the ones of an object its
 * properties and methods. The value of a member that has structure of its own
 * (a table, an object, a function) gets an id that can be inspected in turn
 * or inserted; it stays valid until it is released.
 *
 * Returns: (transfer full) (nullable): `(ssa(sst))` of (type name, summary,
 *   members), where a member is (name, one line summary, id or 0); %NULL on
 *   error
 */
GVariant *shell_lua_inspector_inspect (ShellLuaInspector  *self,
                                       guint64             id,
                                       GError            **error);

/**
 * shell_lua_inspector_insert:
 * @id: a result number or the id of a member of an inspected value
 * @error: set when @id is unknown
 *
 * Records the value as a new result (the Insert button of the object
 * inspector).
 *
 * Returns: (transfer full) (nullable): `(tss)` as an entry of
 *   shell_lua_inspector_evaluate (); %NULL on error
 */
GVariant *shell_lua_inspector_insert (ShellLuaInspector  *self,
                                      guint64             id,
                                      GError            **error);

/**
 * shell_lua_inspector_add_object_result:
 * @object: any #GObject
 * @error: set when the object cannot be pushed to Lua
 *
 * Records @object as a new result (the picker of Looking Glass).
 *
 * Returns: (transfer full) (nullable): `(tss)` as an entry of
 *   shell_lua_inspector_evaluate (); %NULL on error
 */
GVariant *shell_lua_inspector_add_object_result (ShellLuaInspector  *self,
                                                 GObject            *object,
                                                 GError            **error);

/**
 * shell_lua_inspector_pin_object:
 * @object: any #GObject
 * @error: set when the object cannot be pushed to Lua
 *
 * Gives @object an id that can be inspected with
 * shell_lua_inspector_inspect () and inserted, without making it a result.
 * Release the id with shell_lua_inspector_release ().
 *
 * Returns: the id, or 0 on error
 */
guint64   shell_lua_inspector_pin_object (ShellLuaInspector  *self,
                                          GObject            *object,
                                          GError            **error);

/**
 * shell_lua_inspector_get_object:
 * @id: a result number or the id of a member
 *
 * Returns: (transfer full) (nullable): the object behind the value, or %NULL
 *   if it is not an object, the object is gone or @id is unknown
 */
GObject *shell_lua_inspector_get_object (ShellLuaInspector *self,
                                         guint64            id);

/**
 * shell_lua_inspector_release:
 * @id: a result number or the id of a member
 *
 * Forgets the value. A released result number is an error for `r ()`.
 */
void     shell_lua_inspector_release (ShellLuaInspector *self,
                                      guint64            id);

/**
 * shell_lua_inspector_complete:
 * @text: the text before the cursor
 * @head: (out) (optional) (transfer full): the part of the last word that is
 *   already typed
 *
 * Completes the last word of @text: the global variables and the keywords of
 * the inspector state, or the fields of the table or the properties and
 * methods of the object that precede a `.` or `:`. Only plain paths
 * (`a.b.c`) are resolved. The values are read, which can run `__index`
 * metamethods and the getters of properties, but nothing is called.
 *
 * Returns: (transfer full) (array zero-terminated=1): the sorted completions,
 *   complete words that start with @head
 */
char   **shell_lua_inspector_complete (ShellLuaInspector  *self,
                                       const char         *text,
                                       char              **head);

/**
 * shell_lua_inspector_gc:
 *
 * Runs a full garbage collection in the inspector state and in every live
 * state of an extension (the trash button of Looking Glass).
 */
void     shell_lua_inspector_gc (ShellLuaInspector *self);

/* ------------------------------------------------------------------------ */
/* Host integration                                                         */
/* ------------------------------------------------------------------------ */

/**
 * shell_lua_inspector_set_global_object:
 * @name: the name of the global variable
 * @object: (nullable): the object, held weakly; %NULL removes the global
 *
 * Makes @object available to the Evaluator as a global (`stage`,
 * `global`, ...), also after the state is reset. Looks like a generic
 * handle, see above.
 */
void shell_lua_inspector_set_global_object (ShellLuaInspector *self,
                                            const char        *name,
                                            GObject           *object);

/**
 * shell_lua_inspector_add_function: (skip)
 * @name: the name of the global variable
 * @func: a function for Lua
 *
 * Defines a global function, also after the state is reset. @func runs under
 * the usual protected call of the inspector.
 */
void shell_lua_inspector_add_function (ShellLuaInspector *self,
                                       const char        *name,
                                       lua_CFunction      func);

/**
 * shell_lua_inspector_add_object_methods: (skip)
 * @gtype: a #GObject type
 * @methods: methods, terminated by a `{ NULL, NULL }` entry, which must
 *   stay valid
 *
 * Adds methods to the generic handles of @gtype and its descendants, for
 * objects that are pushed after the call (call it before the first
 * evaluation). In the method, argument 1 is the handle: get the object with
 * shell_lua_handle_check ().
 */
void shell_lua_inspector_add_object_methods (GType           gtype,
                                             const luaL_Reg *methods);

/**
 * shell_lua_inspector_push_object: (skip)
 * @L: a state of the inspector
 * @object: the object
 *
 * Pushes the generic handle of @object. Raises a Lua error on failure.
 */
void shell_lua_inspector_push_object (lua_State *L,
                                      GObject   *object);

G_END_DECLS
