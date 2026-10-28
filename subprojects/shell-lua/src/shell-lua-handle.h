/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Handles: the only way Lua code touches a C object.
 *
 * A handle is a full userdata holding a weak reference to a GObject plus a
 * type descriptor. The *owner* of the object (an extension context, see
 * shell-lua-extension.h) controls its lifetime; handles never keep objects
 * alive. When the object is finalized, or shell_lua_handle_invalidate() is
 * called (e.g. on ClutterActor::destroy), the handle turns dead and every
 * method on it raises "handle to <type> is no longer valid".
 *
 * Types are curated: a handle exposes exactly the methods and the properties
 * listed in its ShellLuaHandleType. There is no generic GObject access.
 */
#pragma once

#include <glib-object.h>
#include <lua.h>
#include <lauxlib.h>

G_BEGIN_DECLS

typedef struct {
  const char *name;       /* Lua-visible property name */
  const char *gproperty;  /* GObject property name, NULL = same as name */
  gboolean    readable;
  gboolean    writable;
} ShellLuaHandleProperty;

/* Flags of a method listed in ShellLuaHandleType.methods_ex. */
typedef enum {
  /* The method may be called on a dead handle (its C function must then use
   * shell_lua_handle_get(), which returns NULL instead of raising). Every
   * other method raises "handle to <type> is no longer valid". */
  SHELL_LUA_METHOD_DEAD_OK = 1 << 0,
} ShellLuaMethodFlags;

typedef struct {
  const char   *name;
  lua_CFunction func;
  guint         flags;   /* ShellLuaMethodFlags */
} ShellLuaHandleMethod;

/* A property that is not one GObject property (a computed value, a
 * property of a child object, a per-type setting). @get pushes exactly one
 * value; @set receives the absolute stack index of the new value and raises
 * on a bad one. A NULL @set makes it read-only. Both run with the object
 * referenced; @set runs silenced (docs/api/README.md rule 7: callbacks
 * connected with shell_lua_callback_* are not invoked, see
 * shell-lua-api-private.h). */
typedef void (* ShellLuaVirtualGetter) (lua_State *L,
                                        GObject   *object);
typedef void (* ShellLuaVirtualSetter) (lua_State *L,
                                        GObject   *object,
                                        int        idx);

typedef struct {
  const char            *name;
  ShellLuaVirtualGetter  get;
  ShellLuaVirtualSetter  set;
} ShellLuaHandleVirtual;

typedef struct {
  const char                   *name;        /* metatable name, e.g. "shell.Timer" */
  GType                         gtype;       /* required type of wrapped objects */
  const luaL_Reg               *methods;     /* NULL-terminated; found through __index */
  const ShellLuaHandleProperty *properties;  /* terminated by {NULL}; get/set through __index/__newindex,
                                              * marshalled with shell_lua_push_gvalue / shell_lua_to_gvalue */
  const char                   *parent;      /* metatable name of the parent type, or NULL;
                                              * methods/properties are inherited */
  const ShellLuaHandleMethod   *methods_ex;  /* optional, terminated by {NULL}: methods with flags */
  const ShellLuaHandleVirtual  *virtuals;    /* optional, terminated by {NULL}: virtual properties.
                                              * They win over a GObject property of the same name. */
} ShellLuaHandleType;

/* Every handle has a read-only boolean property `alive` (it never raises,
 * not even on a dead handle) and shows up as such in
 * shell_lua_handle_foreach_member(). Assigning a read-only or unknown
 * property raises; reading an unknown one gives nil. */

/* Register a type in @L. Idempotent per name. Must be called before any
 * handle of that type is pushed; API modules do it in their register func. */
void      shell_lua_handle_register_type (lua_State                *L,
                                          const ShellLuaHandleType *type);

/* Make @type (registered already) the one handles of its GType get when
 * pushed without a type name, instead of the first type registered for the
 * GType. The privileged inspector state uses it so that the generic
 * GObject handles win over the extension API's types. */
void      shell_lua_handle_prefer_type   (lua_State                *L,
                                          const ShellLuaHandleType *type);

/* Push the handle for @object. The same object yields the same userdata
 * within one lua_State while it is alive (weak cache keyed by pointer).
 * @type_name may be NULL to use the most specific registered type for
 * G_OBJECT_TYPE (object), walking up the GType hierarchy. Raises a Lua error
 * if no registered type matches. */
void      shell_lua_handle_push          (lua_State  *L,
                                          const char *type_name,
                                          GObject    *object);

/* Get the object behind the handle at @idx, checking it is a (sub)type of
 * @type_name (NULL = any handle). Raises on wrong type or dead handle. */
GObject  *shell_lua_handle_check         (lua_State  *L,
                                          int         idx,
                                          const char *type_name);

/* Like check, but returns NULL instead of raising on a non-handle or wrong
 * type. Still raises on a dead handle. */
GObject  *shell_lua_handle_peek          (lua_State  *L,
                                          int         idx,
                                          const char *type_name);

gboolean  shell_lua_handle_is_alive      (lua_State  *L,
                                          int         idx);

/* Like check, but for methods flagged SHELL_LUA_METHOD_DEAD_OK: returns NULL
 * (without raising) when the handle is dead. Still raises on a non-handle or
 * on the wrong type. */
GObject  *shell_lua_handle_get           (lua_State  *L,
                                          int         idx,
                                          const char *type_name);

/* Called for each member visible on a handle: @name is the Lua-visible name,
 * @is_method is TRUE for methods and FALSE for properties. Only the readable
 * properties are reported. */
typedef void (* ShellLuaHandleMemberFunc) (const char *name,
                                           gboolean    is_method,
                                           gpointer    user_data);

/* Report the properties and methods of the handle at @idx, most specific type
 * first (inherited ones included). Does nothing if @idx is not a handle. Does
 * not raise, also for dead handles (their type is still known). */
void      shell_lua_handle_foreach_member (lua_State                *L,
                                           int                       idx,
                                           ShellLuaHandleMemberFunc  func,
                                           gpointer                  user_data);

/* A resolver is consulted by shell_lua_handle_push() (and so by every property
 * that returns an object) when no type name was given, before the registered
 * types are searched for @gtype or one of its parents. It may register handle
 * types for @gtype in @L. Per state: only the privileged inspector installs
 * one, to give generic access to the objects it meets. */
typedef void (* ShellLuaHandleResolver) (lua_State *L,
                                         GType      gtype);

void      shell_lua_handle_set_resolver  (lua_State              *L,
                                          ShellLuaHandleResolver  resolver);

/* Kill every handle to @object in @L (and in every state, if @L is NULL). */
void      shell_lua_handle_invalidate    (lua_State  *L,
                                          GObject    *object);

/* GValue <-> Lua for the property path. Supports bool, all ints, float/double,
 * string, enums (as their nick string), flags (as a set table {nick=true}),
 * GStrv (array), and GObject (as handle, pushing nil when no type matches).
 * to_gvalue raises a Lua error on mismatch. */
void      shell_lua_push_gvalue          (lua_State    *L,
                                          const GValue *value);
void      shell_lua_to_gvalue            (lua_State    *L,
                                          int           idx,
                                          GValue       *value);  /* value pre-initialised with the target type */

G_END_DECLS
