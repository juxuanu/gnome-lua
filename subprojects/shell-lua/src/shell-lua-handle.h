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

typedef struct {
  const char                   *name;        /* metatable name, e.g. "shell.Timer" */
  GType                         gtype;       /* required type of wrapped objects */
  const luaL_Reg               *methods;     /* NULL-terminated; found through __index */
  const ShellLuaHandleProperty *properties;  /* terminated by {NULL}; get/set through __index/__newindex,
                                              * marshalled with shell_lua_push_gvalue / shell_lua_to_gvalue */
  const char                   *parent;      /* metatable name of the parent type, or NULL;
                                              * methods/properties are inherited */
} ShellLuaHandleType;

/* Register a type in @L. Idempotent per name. Must be called before any
 * handle of that type is pushed; API modules do it in their register func. */
void      shell_lua_handle_register_type (lua_State                *L,
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
