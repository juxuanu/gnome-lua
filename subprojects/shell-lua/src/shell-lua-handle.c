/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Handles: GObject <-> userdata registry. See shell-lua-handle.h for the
 * contract.
 *
 * Layout, per lua_State (all keys are addresses of static variables, stored
 * in the registry):
 *
 *   scratch_key   a thread that is never run. Everything that has to touch the
 *                 Lua state from a GObject callback (weak notify, invalidate)
 *                 does so on this thread's own (empty) stack, so we never
 *                 push onto a stack that is in use by running code.
 *   cache_key     weak-valued table: lightuserdata(GObject*) -> handle
 *   types_key     table: type name -> lightuserdata(ShellLuaHandleType*)
 *   gtypes_key    table: integer(GType) -> lightuserdata(ShellLuaHandleType*)
 *   closer_key    userdata whose __gc (run by lua_close) removes the scratch
 *                 thread from the process-global list of live states
 *
 * Contracts that callers must honour:
 *  - ShellLuaHandleType descriptors (and the method/property arrays they
 *    point to) must stay valid for the lifetime of the state: use static
 *    data.
 *  - Single-threaded: objects must be finalized in the thread that runs the
 *    Lua state.
 *  - Lua errors raised mid-way through a property read/write can leak a
 *    GValue's contents (only out-of-memory can raise at that point; the
 *    sandbox has a memory limit, so this is possible but harmless).
 */

#include "shell-lua-handle.h"
#include "shell-lua-api-private.h"
#include "shell-lua-variant.h"

#include <string.h>

typedef struct {
  GObject                  *object;   /* NULL once dead */
  const ShellLuaHandleType *type;
  lua_State                *scratch;
  gboolean                  dead;
} Handle;

/* qdata on an object: the interned name of the handle type it was last pushed
 * as. Pushing without a type name (a callback's first argument, a property
 * that returns an object) then gives the type the module chose, not the
 * first one registered for the GType. */
#define TYPE_HINT_QUARK "shell-lua-handle-type"

static char scratch_key, cache_key, types_key, gtypes_key, closer_key, marker_key, resolver_key;

#define MAX_TYPE_DEPTH 16

/* Process-global list of live states (their scratch threads). */
G_LOCK_DEFINE_STATIC (states);
static GPtrArray *states = NULL;

/* ------------------------------------------------------------------------ */
/* Lookups                                                                   */
/* ------------------------------------------------------------------------ */

static Handle *
to_handle (lua_State *L,
           int        idx)
{
  Handle *h;

  idx = lua_absindex (L, idx);
  if (lua_type (L, idx) != LUA_TUSERDATA)
    return NULL;
  if (!lua_getmetatable (L, idx))
    return NULL;
  lua_rawgetp (L, -1, &marker_key);
  h = lua_toboolean (L, -1) ? lua_touserdata (L, idx) : NULL;
  lua_pop (L, 2);
  return h;
}

static lua_State *
get_scratch (lua_State *L)
{
  lua_State *S = NULL;

  if (lua_rawgetp (L, LUA_REGISTRYINDEX, &scratch_key) == LUA_TTHREAD)
    S = lua_tothread (L, -1);
  lua_pop (L, 1);
  return S;
}

static const ShellLuaHandleType *
find_type (lua_State  *L,
           const char *name)
{
  const ShellLuaHandleType *t = NULL;

  if (name == NULL)
    return NULL;
  if (lua_rawgetp (L, LUA_REGISTRYINDEX, &types_key) == LUA_TTABLE)
    {
      lua_pushstring (L, name);
      if (lua_rawget (L, -2) == LUA_TLIGHTUSERDATA)
        t = lua_touserdata (L, -1);
      lua_pop (L, 1);
    }
  lua_pop (L, 1);
  return t;
}

static const ShellLuaHandleType *
parent_of (lua_State                *L,
           const ShellLuaHandleType *t)
{
  return t->parent != NULL ? find_type (L, t->parent) : NULL;
}

/* Is @t the type called @name, or a descendant of it? */
static gboolean
type_is_a (lua_State                *L,
           const ShellLuaHandleType *t,
           const char               *name)
{
  int depth;

  for (depth = 0; t != NULL && depth < MAX_TYPE_DEPTH; depth++)
    {
      if (strcmp (t->name, name) == 0)
        return TRUE;
      t = parent_of (L, t);
    }
  return FALSE;
}

static const luaL_Reg *
find_method (lua_State                *L,
             const ShellLuaHandleType *t,
             const char               *key)
{
  int depth;

  for (depth = 0; t != NULL && depth < MAX_TYPE_DEPTH; depth++)
    {
      const luaL_Reg *r;

      for (r = t->methods; r != NULL && r->name != NULL; r++)
        if (r->func != NULL && strcmp (r->name, key) == 0)
          return r;
      t = parent_of (L, t);
    }
  return NULL;
}

static const ShellLuaHandleMethod *
find_method_ex (lua_State                *L,
                const ShellLuaHandleType *t,
                const char               *key)
{
  int depth;

  for (depth = 0; t != NULL && depth < MAX_TYPE_DEPTH; depth++)
    {
      const ShellLuaHandleMethod *r;

      for (r = t->methods_ex; r != NULL && r->name != NULL; r++)
        if (r->func != NULL && strcmp (r->name, key) == 0)
          return r;
      t = parent_of (L, t);
    }
  return NULL;
}

static const ShellLuaHandleVirtual *
find_virtual (lua_State                *L,
              const ShellLuaHandleType *t,
              const char               *key)
{
  int depth;

  for (depth = 0; t != NULL && depth < MAX_TYPE_DEPTH; depth++)
    {
      const ShellLuaHandleVirtual *v;

      for (v = t->virtuals; v != NULL && v->name != NULL; v++)
        if (v->get != NULL && strcmp (v->name, key) == 0)
          return v;
      t = parent_of (L, t);
    }
  return NULL;
}

static const ShellLuaHandleProperty *
find_property (lua_State                *L,
               const ShellLuaHandleType *t,
               const char               *key)
{
  int depth;

  for (depth = 0; t != NULL && depth < MAX_TYPE_DEPTH; depth++)
    {
      const ShellLuaHandleProperty *p;

      for (p = t->properties; p != NULL && p->name != NULL; p++)
        if (strcmp (p->name, key) == 0)
          return p;
      t = parent_of (L, t);
    }
  return NULL;
}

/* ------------------------------------------------------------------------ */
/* Lifetime                                                                  */
/* ------------------------------------------------------------------------ */

/* Drop the cache entry for @key if it still points at @h. Runs on the scratch
 * thread; only uses non-allocating API calls, so it cannot raise. */
static void
uncache (lua_State *S,
         gpointer   key,
         Handle    *h)
{
  if (lua_rawgetp (S, LUA_REGISTRYINDEX, &cache_key) == LUA_TTABLE)
    {
      if (lua_rawgetp (S, -1, key) == LUA_TUSERDATA && lua_touserdata (S, -1) == (void *) h)
        {
          lua_pop (S, 1);
          lua_pushnil (S);
          lua_rawsetp (S, -2, key);
        }
      else
        {
          lua_pop (S, 1);
        }
    }
  lua_pop (S, 1);
}

static void
weak_notify (gpointer data,
             GObject *where)
{
  Handle *h = data;

  h->dead = TRUE;
  h->object = NULL;
  uncache (h->scratch, where, h);
}

static void
handle_kill (Handle *h)
{
  GObject *object = h->object;

  if (h->dead || object == NULL)
    return;

  h->dead = TRUE;
  h->object = NULL;
  uncache (h->scratch, object, h);
  g_object_weak_unref (object, weak_notify, h);
}

static void
invalidate_in (lua_State *S,
               GObject   *object)
{
  if (lua_rawgetp (S, LUA_REGISTRYINDEX, &cache_key) == LUA_TTABLE)
    {
      if (lua_rawgetp (S, -1, object) == LUA_TUSERDATA)
        {
          Handle *h = to_handle (S, -1);

          if (h != NULL && h->object == object)
            handle_kill (h);
        }
      lua_pop (S, 1);
    }
  lua_pop (S, 1);
}

void
shell_lua_handle_invalidate (lua_State *L,
                             GObject   *object)
{
  if (object == NULL)
    return;

  if (L != NULL)
    {
      lua_State *S = get_scratch (L);

      if (S != NULL)
        invalidate_in (S, object);
      return;
    }

  G_LOCK (states);
  if (states != NULL)
    {
      guint i;

      for (i = 0; i < states->len; i++)
        invalidate_in (g_ptr_array_index (states, i), object);
    }
  G_UNLOCK (states);
}

/* ------------------------------------------------------------------------ */
/* GValue marshalling                                                        */
/* ------------------------------------------------------------------------ */

static gboolean handle_push_try (lua_State *L, const char *type_name, GObject *object);

/* Returns FALSE (pushing nothing) if the value cannot be represented. */
static gboolean
push_gvalue_try (lua_State    *L,
                 const GValue *v)
{
  GType type = G_VALUE_TYPE (v);

  if (!lua_checkstack (L, 6))
    return FALSE;

  if (G_VALUE_HOLDS (v, G_TYPE_STRV))
    {
      gchar **strv = g_value_get_boxed (v);
      int i;

      lua_newtable (L);
      for (i = 0; strv != NULL && strv[i] != NULL; i++)
        {
          lua_pushstring (L, strv[i]);
          lua_rawseti (L, -2, i + 1);
        }
      return TRUE;
    }

  switch (G_TYPE_FUNDAMENTAL (type))
    {
    case G_TYPE_BOOLEAN:
      lua_pushboolean (L, g_value_get_boolean (v));
      return TRUE;
    case G_TYPE_CHAR:
      lua_pushinteger (L, g_value_get_schar (v));
      return TRUE;
    case G_TYPE_UCHAR:
      lua_pushinteger (L, g_value_get_uchar (v));
      return TRUE;
    case G_TYPE_INT:
      lua_pushinteger (L, g_value_get_int (v));
      return TRUE;
    case G_TYPE_UINT:
      lua_pushinteger (L, g_value_get_uint (v));
      return TRUE;
    case G_TYPE_LONG:
      lua_pushinteger (L, g_value_get_long (v));
      return TRUE;
    case G_TYPE_ULONG:
      lua_pushinteger (L, (lua_Integer) g_value_get_ulong (v));
      return TRUE;
    case G_TYPE_INT64:
      lua_pushinteger (L, g_value_get_int64 (v));
      return TRUE;
    case G_TYPE_UINT64:
      lua_pushinteger (L, (lua_Integer) g_value_get_uint64 (v));
      return TRUE;
    case G_TYPE_FLOAT:
      lua_pushnumber (L, g_value_get_float (v));
      return TRUE;
    case G_TYPE_DOUBLE:
      lua_pushnumber (L, g_value_get_double (v));
      return TRUE;
    case G_TYPE_STRING:
      {
        const char *s = g_value_get_string (v);

        if (s != NULL)
          lua_pushstring (L, s);
        else
          lua_pushnil (L);
        return TRUE;
      }
    case G_TYPE_ENUM:
      {
        GEnumClass *klass = g_type_class_ref (type);
        GEnumValue *ev = g_enum_get_value (klass, g_value_get_enum (v));

        if (ev != NULL)
          lua_pushstring (L, ev->value_nick);
        else
          lua_pushinteger (L, g_value_get_enum (v));
        g_type_class_unref (klass);
        return TRUE;
      }
    case G_TYPE_FLAGS:
      {
        GFlagsClass *klass;
        guint flags = g_value_get_flags (v);
        guint i;

        lua_newtable (L);
        klass = g_type_class_ref (type);
        for (i = 0; i < klass->n_values; i++)
          {
            const GFlagsValue *fv = &klass->values[i];

            if (fv->value != 0 && (flags & fv->value) == fv->value)
              {
                lua_pushboolean (L, 1);
                lua_setfield (L, -2, fv->value_nick);
              }
          }
        g_type_class_unref (klass);
        return TRUE;
      }
    case G_TYPE_OBJECT:
    case G_TYPE_INTERFACE:
      {
        GObject *o = g_value_get_object (v);

        if (o == NULL || !handle_push_try (L, NULL, o))
          lua_pushnil (L);
        return TRUE;
      }
    case G_TYPE_VARIANT:
      {
        GVariant *var = g_value_get_variant (v);

        if (var == NULL)
          lua_pushnil (L);
        else
          shell_lua_push_variant (L, var);
        return TRUE;
      }
    default:
      return FALSE;
    }
}

void
shell_lua_push_gvalue (lua_State    *L,
                       const GValue *value)
{
  if (!push_gvalue_try (L, value))
    luaL_error (L, "cannot convert a value of type '%s' to Lua",
                g_type_name (G_VALUE_TYPE (value)));
}

static char *
type_mismatch (lua_State  *L,
               int         idx,
               const char *expected)
{
  return g_strdup_printf ("%s expected, got %s", expected, luaL_typename (L, idx));
}

static gboolean
to_integer (lua_State  *L,
            int         idx,
            lua_Integer min,
            lua_Integer max,
            lua_Integer *out,
            char      **err)
{
  int isnum = 0;
  lua_Integer i;

  if (lua_type (L, idx) != LUA_TNUMBER)
    {
      *err = type_mismatch (L, idx, "integer");
      return FALSE;
    }
  i = lua_tointegerx (L, idx, &isnum);
  if (!isnum)
    {
      *err = g_strdup ("number has no integer representation");
      return FALSE;
    }
  if (i < min || i > max)
    {
      *err = g_strdup_printf ("integer %" G_GINT64_FORMAT " out of range", (gint64) i);
      return FALSE;
    }
  *out = i;
  return TRUE;
}

static char *
valid_nicks (GType type)
{
  GString *s = g_string_new (NULL);
  guint i;

  if (G_TYPE_IS_ENUM (type))
    {
      GEnumClass *klass = g_type_class_ref (type);

      for (i = 0; i < klass->n_values; i++)
        g_string_append_printf (s, "%s%s", i ? ", " : "", klass->values[i].value_nick);
      g_type_class_unref (klass);
    }
  else
    {
      GFlagsClass *klass = g_type_class_ref (type);

      for (i = 0; i < klass->n_values; i++)
        g_string_append_printf (s, "%s%s", i ? ", " : "", klass->values[i].value_nick);
      g_type_class_unref (klass);
    }
  return g_string_free (s, FALSE);
}

static gboolean
flags_add_nick (GFlagsClass *klass,
                const char  *nick,
                guint       *flags)
{
  GFlagsValue *fv = g_flags_get_value_by_nick (klass, nick);

  if (fv == NULL)
    fv = g_flags_get_value_by_name (klass, nick);
  if (fv == NULL)
    return FALSE;
  *flags |= fv->value;
  return TRUE;
}

/* Never raises (except through shell_lua_to_variant for G_TYPE_VARIANT, which
 * allocates nothing before it). On failure returns FALSE and sets *err. */
static gboolean
gvalue_from_lua (lua_State *L,
                 int        idx,
                 GValue    *v,
                 char     **err)
{
  GType type = G_VALUE_TYPE (v);
  lua_Integer i;

  idx = lua_absindex (L, idx);

  if (G_VALUE_HOLDS (v, G_TYPE_STRV))
    {
      GPtrArray *arr;
      lua_Unsigned n, k;

      if (lua_type (L, idx) != LUA_TTABLE)
        {
          *err = type_mismatch (L, idx, "table of strings");
          return FALSE;
        }
      n = lua_rawlen (L, idx);
      arr = g_ptr_array_new_with_free_func (g_free);
      for (k = 1; k <= n; k++)
        {
          lua_rawgeti (L, idx, (lua_Integer) k);
          if (lua_type (L, -1) != LUA_TSTRING)
            {
              *err = g_strdup_printf ("element %" G_GUINT64_FORMAT " is a %s, string expected",
                                      (guint64) k, luaL_typename (L, -1));
              lua_pop (L, 1);
              g_ptr_array_free (arr, TRUE);
              return FALSE;
            }
          g_ptr_array_add (arr, g_strdup (lua_tostring (L, -1)));
          lua_pop (L, 1);
        }
      g_ptr_array_add (arr, NULL);
      g_value_take_boxed (v, g_ptr_array_free (arr, FALSE));
      return TRUE;
    }

  switch (G_TYPE_FUNDAMENTAL (type))
    {
    case G_TYPE_BOOLEAN:
      if (lua_type (L, idx) != LUA_TBOOLEAN)
        {
          *err = type_mismatch (L, idx, "boolean");
          return FALSE;
        }
      g_value_set_boolean (v, lua_toboolean (L, idx));
      return TRUE;
    case G_TYPE_CHAR:
      if (!to_integer (L, idx, G_MININT8, G_MAXINT8, &i, err))
        return FALSE;
      g_value_set_schar (v, (gint8) i);
      return TRUE;
    case G_TYPE_UCHAR:
      if (!to_integer (L, idx, 0, G_MAXUINT8, &i, err))
        return FALSE;
      g_value_set_uchar (v, (guchar) i);
      return TRUE;
    case G_TYPE_INT:
      if (!to_integer (L, idx, G_MININT, G_MAXINT, &i, err))
        return FALSE;
      g_value_set_int (v, (gint) i);
      return TRUE;
    case G_TYPE_UINT:
      if (!to_integer (L, idx, 0, G_MAXUINT, &i, err))
        return FALSE;
      g_value_set_uint (v, (guint) i);
      return TRUE;
    case G_TYPE_LONG:
      if (!to_integer (L, idx, G_MINLONG, G_MAXLONG, &i, err))
        return FALSE;
      g_value_set_long (v, (glong) i);
      return TRUE;
    case G_TYPE_ULONG:
      if (!to_integer (L, idx, 0, LUA_MAXINTEGER, &i, err))
        return FALSE;
      g_value_set_ulong (v, (gulong) i);
      return TRUE;
    case G_TYPE_INT64:
      if (!to_integer (L, idx, LUA_MININTEGER, LUA_MAXINTEGER, &i, err))
        return FALSE;
      g_value_set_int64 (v, (gint64) i);
      return TRUE;
    case G_TYPE_UINT64:
      if (!to_integer (L, idx, 0, LUA_MAXINTEGER, &i, err))
        return FALSE;
      g_value_set_uint64 (v, (guint64) i);
      return TRUE;
    case G_TYPE_FLOAT:
      if (lua_type (L, idx) != LUA_TNUMBER)
        {
          *err = type_mismatch (L, idx, "number");
          return FALSE;
        }
      g_value_set_float (v, (gfloat) lua_tonumber (L, idx));
      return TRUE;
    case G_TYPE_DOUBLE:
      if (lua_type (L, idx) != LUA_TNUMBER)
        {
          *err = type_mismatch (L, idx, "number");
          return FALSE;
        }
      g_value_set_double (v, lua_tonumber (L, idx));
      return TRUE;
    case G_TYPE_STRING:
      if (lua_type (L, idx) == LUA_TNIL)
        {
          g_value_set_string (v, NULL);
          return TRUE;
        }
      if (lua_type (L, idx) != LUA_TSTRING)
        {
          *err = type_mismatch (L, idx, "string");
          return FALSE;
        }
      g_value_set_string (v, lua_tostring (L, idx));
      return TRUE;
    case G_TYPE_ENUM:
      {
        GEnumClass *klass;
        GEnumValue *ev = NULL;
        gboolean ok = FALSE;

        if (lua_type (L, idx) != LUA_TSTRING && lua_type (L, idx) != LUA_TNUMBER)
          {
            *err = type_mismatch (L, idx, "enum nick (string)");
            return FALSE;
          }
        klass = g_type_class_ref (type);
        if (lua_type (L, idx) == LUA_TSTRING)
          {
            const char *nick = lua_tostring (L, idx);

            ev = g_enum_get_value_by_nick (klass, nick);
            if (ev == NULL)
              ev = g_enum_get_value_by_name (klass, nick);
            if (ev == NULL)
              {
                char *valid = valid_nicks (type);

                *err = g_strdup_printf ("invalid value '%s' for %s (valid: %s)",
                                        nick, g_type_name (type), valid);
                g_free (valid);
              }
          }
        else if (lua_isinteger (L, idx))
          {
            ev = g_enum_get_value (klass, (gint) lua_tointeger (L, idx));
            if (ev == NULL)
              *err = g_strdup_printf ("invalid value %" G_GINT64_FORMAT " for %s",
                                      (gint64) lua_tointeger (L, idx), g_type_name (type));
          }
        else
          {
            *err = type_mismatch (L, idx, "enum nick (string)");
          }
        if (ev != NULL)
          {
            g_value_set_enum (v, ev->value);
            ok = TRUE;
          }
        g_type_class_unref (klass);
        return ok;
      }
    case G_TYPE_FLAGS:
      {
        GFlagsClass *klass;
        guint flags = 0;
        gboolean ok = TRUE;
        int t = lua_type (L, idx);

        if (t == LUA_TNUMBER && lua_isinteger (L, idx))
          {
            g_value_set_flags (v, (guint) lua_tointeger (L, idx));
            return TRUE;
          }
        if (t != LUA_TSTRING && t != LUA_TTABLE)
          {
            *err = type_mismatch (L, idx, "flags table {nick=true}");
            return FALSE;
          }
        klass = g_type_class_ref (type);
        if (t == LUA_TSTRING)
          {
            const char *nick = lua_tostring (L, idx);

            if (nick[0] != '\0' && !flags_add_nick (klass, nick, &flags))
              ok = FALSE, *err = g_strdup_printf ("invalid flag '%s' for %s", nick, g_type_name (type));
          }
        else
          {
            lua_pushnil (L);
            while (ok && lua_next (L, idx) != 0)
              {
                /* set form {nick=true} or sequence form {"nick", ...}; only
                 * strings are ever read with lua_tostring (no conversion of
                 * a key in place, which would confuse lua_next) */
                if (lua_type (L, -2) == LUA_TSTRING && lua_toboolean (L, -1))
                  {
                    if (!flags_add_nick (klass, lua_tostring (L, -2), &flags))
                      ok = FALSE, *err = g_strdup_printf ("invalid flag '%s' for %s",
                                                           lua_tostring (L, -2), g_type_name (type));
                  }
                else if (lua_type (L, -2) == LUA_TNUMBER && lua_type (L, -1) == LUA_TSTRING)
                  {
                    if (!flags_add_nick (klass, lua_tostring (L, -1), &flags))
                      ok = FALSE, *err = g_strdup_printf ("invalid flag '%s' for %s",
                                                           lua_tostring (L, -1), g_type_name (type));
                  }
                if (!ok)
                  lua_pop (L, 2);   /* key and value: leave the iteration */
                else
                  lua_pop (L, 1);
              }
          }
        g_type_class_unref (klass);
        if (ok)
          g_value_set_flags (v, flags);
        return ok;
      }
    case G_TYPE_OBJECT:
    case G_TYPE_INTERFACE:
      {
        Handle *h;

        if (lua_type (L, idx) == LUA_TNIL)
          {
            g_value_set_object (v, NULL);
            return TRUE;
          }
        h = to_handle (L, idx);
        if (h == NULL)
          {
            *err = type_mismatch (L, idx, "handle");
            return FALSE;
          }
        if (h->dead)
          {
            *err = g_strdup_printf ("handle to %s is no longer valid", h->type->name);
            return FALSE;
          }
        if (!g_type_is_a (G_OBJECT_TYPE (h->object), type))
          {
            *err = g_strdup_printf ("%s handle given where %s is expected",
                                    h->type->name, g_type_name (type));
            return FALSE;
          }
        g_value_set_object (v, h->object);
        return TRUE;
      }
    case G_TYPE_VARIANT:
      {
        GVariant *var = shell_lua_to_variant (L, idx, NULL);

        g_value_take_variant (v, var);
        return TRUE;
      }
    default:
      *err = g_strdup_printf ("properties of type '%s' are not supported", g_type_name (type));
      return FALSE;
    }
}

void
shell_lua_to_gvalue (lua_State *L,
                     int        idx,
                     GValue    *value)
{
  char *err = NULL;

  if (!gvalue_from_lua (L, idx, value, &err))
    {
      lua_pushstring (L, err != NULL ? err : "invalid value");
      g_free (err);
      lua_error (L);
    }
}

/* ------------------------------------------------------------------------ */
/* Metamethods                                                               */
/* ------------------------------------------------------------------------ */

static int
dead_error (lua_State *L,
            Handle    *h)
{
  return luaL_error (L, "handle to %s is no longer valid", h->type->name);
}

static GParamSpec *
lookup_pspec (Handle                       *h,
              const ShellLuaHandleProperty *p)
{
  return g_object_class_find_property (G_OBJECT_GET_CLASS (h->object),
                                       p->gproperty != NULL ? p->gproperty : p->name);
}

static int
handle_index (lua_State *L)
{
  Handle *h = to_handle (L, 1);
  const char *key;
  const luaL_Reg *method;
  const ShellLuaHandleMethod *method_ex;
  const ShellLuaHandleVirtual *virt;
  const ShellLuaHandleProperty *prop;

  if (h == NULL)
    return luaL_error (L, "not a handle");

  if (lua_type (L, 2) != LUA_TSTRING)
    {
      if (h->dead)
        return dead_error (L, h);
      lua_pushnil (L);
      return 1;
    }
  key = lua_tostring (L, 2);

  /* `alive` exists on every handle and never raises. */
  if (strcmp (key, "alive") == 0)
    {
      lua_pushboolean (L, !h->dead);
      return 1;
    }

  if (h->dead)
    {
      /* Only the methods flagged DEAD_OK survive the object. */
      method_ex = find_method_ex (L, h->type, key);
      if (method_ex != NULL && (method_ex->flags & SHELL_LUA_METHOD_DEAD_OK))
        {
          lua_pushcfunction (L, method_ex->func);
          return 1;
        }
      return dead_error (L, h);
    }

  method = find_method (L, h->type, key);
  if (method != NULL)
    {
      lua_pushcfunction (L, method->func);
      return 1;
    }
  method_ex = find_method_ex (L, h->type, key);
  if (method_ex != NULL)
    {
      lua_pushcfunction (L, method_ex->func);
      return 1;
    }

  virt = find_virtual (L, h->type, key);
  if (virt != NULL)
    {
      GObject *object = g_object_ref (h->object);

      /* The getter may raise: the reference is dropped by the caller's
       * object ownership at worst (the object is owned by an extension), so
       * a leak here would need an out-of-memory error. */
      virt->get (L, object);
      g_object_unref (object);
      return 1;
    }

  prop = find_property (L, h->type, key);
  if (prop != NULL && prop->readable)
    {
      GParamSpec *pspec = lookup_pspec (h, prop);
      GValue value = G_VALUE_INIT;
      gboolean ok;
      GObject *object;

      if (pspec == NULL || !(pspec->flags & G_PARAM_READABLE))
        return luaL_error (L, "property '%s' of %s is not readable", key, h->type->name);

      object = g_object_ref (h->object);
      g_value_init (&value, G_PARAM_SPEC_VALUE_TYPE (pspec));
      g_object_get_property (object, pspec->name, &value);
      g_object_unref (object);
      ok = push_gvalue_try (L, &value);
      g_value_unset (&value);
      if (!ok)
        return luaL_error (L, "cannot convert property '%s' of %s to Lua", key, h->type->name);
      return 1;
    }

  /* Unknown names read as nil (feature detection); writes raise. */
  lua_pushnil (L);
  return 1;
}

/* TRUE if @pspec accepts @value as it is (g_param_value_validate() would not
 * have to modify it). */
static gboolean
value_passes_validation (GParamSpec  *pspec,
                         const GValue *value)
{
  GValue copy = G_VALUE_INIT;
  gboolean modified;

  g_value_init (&copy, G_VALUE_TYPE (value));
  g_value_copy (value, &copy);
  modified = g_param_value_validate (pspec, &copy);
  g_value_unset (&copy);
  return !modified;
}

typedef struct {
  ShellLuaVirtualSetter set;
  GObject              *object;
  int                   idx;
} VirtualSet;

static void
virtual_set_trampoline (lua_State *L,
                        gpointer   data)
{
  VirtualSet *vs = data;

  vs->set (L, vs->object, vs->idx);
}

static int
handle_newindex (lua_State *L)
{
  Handle *h = to_handle (L, 1);
  const char *key;
  const ShellLuaHandleProperty *prop;
  const ShellLuaHandleVirtual *virt;
  GParamSpec *pspec;
  GValue value = G_VALUE_INIT;
  char *err = NULL;
  GObject *object;

  if (h == NULL)
    return luaL_error (L, "not a handle");
  if (h->dead)
    return dead_error (L, h);

  if (lua_type (L, 2) != LUA_TSTRING)
    return luaL_error (L, "no property of type %s on %s", luaL_typename (L, 2), h->type->name);
  key = lua_tostring (L, 2);

  if (strcmp (key, "alive") == 0)
    return luaL_error (L, "property 'alive' on %s is read-only", h->type->name);

  virt = find_virtual (L, h->type, key);
  if (virt != NULL)
    {
      VirtualSet vs;
      gboolean ok;

      if (virt->set == NULL)
        return luaL_error (L, "property '%s' on %s is read-only", key, h->type->name);

      vs.set = virt->set;
      vs.object = g_object_ref (h->object);
      vs.idx = 1;
      /* Rule 7: nothing the setter provokes calls back into Lua. The value
       * is the only argument of the silenced frame. */
      lua_pushvalue (L, 3);
      ok = shell_lua_try_silenced_args (L, 1, virtual_set_trampoline, &vs);
      g_object_unref (vs.object);
      if (!ok)
        return lua_error (L);   /* the error object is on top */
      return 0;
    }

  prop = find_property (L, h->type, key);
  if (prop == NULL)
    return luaL_error (L, "no property '%s' on %s", key, h->type->name);
  if (!prop->writable)
    return luaL_error (L, "property '%s' on %s is read-only", key, h->type->name);

  pspec = lookup_pspec (h, prop);
  if (pspec == NULL || !(pspec->flags & G_PARAM_WRITABLE) || (pspec->flags & G_PARAM_CONSTRUCT_ONLY))
    return luaL_error (L, "property '%s' on %s is read-only", key, h->type->name);

  g_value_init (&value, G_PARAM_SPEC_VALUE_TYPE (pspec));
  if (!gvalue_from_lua (L, 3, &value, &err))
    {
      g_value_unset (&value);
      lua_pushfstring (L, "bad value for property '%s' on %s: %s", key, h->type->name, err);
      g_free (err);
      luaL_where (L, 1);
      lua_insert (L, -2);
      lua_concat (L, 2);
      return lua_error (L);
    }
  /* g_param_value_is_valid() is not usable here: for object properties it
   * reports NULL as invalid, so `handle.prop = nil` could never clear one.
   * Validate a copy instead, the same check g_object_set_property() makes. */
  if (!value_passes_validation (pspec, &value))
    {
      g_value_unset (&value);
      return luaL_error (L, "value out of range for property '%s' on %s", key, h->type->name);
    }

  /* Setting may run arbitrary signal handlers, which can invalidate @h. */
  object = g_object_ref (h->object);
  shell_lua_silence_enter (L);   /* rule 7; nothing below can raise */
  g_object_set_property (object, pspec->name, &value);
  shell_lua_silence_leave (L);
  g_object_unref (object);
  g_value_unset (&value);
  return 0;
}

static int
handle_tostring (lua_State *L)
{
  Handle *h = to_handle (L, 1);

  if (h == NULL)
    lua_pushstring (L, "<invalid handle>");
  else if (h->dead)
    lua_pushfstring (L, "<%s (dead)>", h->type->name);
  else
    lua_pushfstring (L, "<%s %p>", h->type->name, (void *) h->object);
  return 1;
}

static int
handle_eq (lua_State *L)
{
  Handle *a = to_handle (L, 1);
  Handle *b = to_handle (L, 2);

  lua_pushboolean (L, a != NULL && b != NULL &&
                      (a == b || (!a->dead && !b->dead && a->object == b->object)));
  return 1;
}

static int
handle_gc (lua_State *L)
{
  Handle *h = to_handle (L, 1);

  if (h != NULL && h->object != NULL)
    {
      g_object_weak_unref (h->object, weak_notify, h);
      h->object = NULL;
      h->dead = TRUE;
    }
  return 0;
}

static int
closer_gc (lua_State *L)
{
  lua_State **p = lua_touserdata (L, 1);

  if (p != NULL && *p != NULL)
    {
      G_LOCK (states);
      if (states != NULL)
        g_ptr_array_remove_fast (states, *p);
      G_UNLOCK (states);
      *p = NULL;
    }
  return 0;
}

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

static void
ensure_state (lua_State *L)
{
  lua_State *S, **closer;

  if (lua_rawgetp (L, LUA_REGISTRYINDEX, &closer_key) != LUA_TNIL)
    {
      lua_pop (L, 1);
      return;
    }
  lua_pop (L, 1);

  luaL_checkstack (L, 6, "shell_lua_handle_register_type");

  S = lua_newthread (L);
  lua_rawsetp (L, LUA_REGISTRYINDEX, &scratch_key);

  /* cache: weak values */
  lua_newtable (L);
  lua_newtable (L);
  lua_pushstring (L, "v");
  lua_setfield (L, -2, "__mode");
  lua_setmetatable (L, -2);
  lua_rawsetp (L, LUA_REGISTRYINDEX, &cache_key);

  lua_newtable (L);
  lua_rawsetp (L, LUA_REGISTRYINDEX, &types_key);
  lua_newtable (L);
  lua_rawsetp (L, LUA_REGISTRYINDEX, &gtypes_key);

  /* The closer comes last: its presence means "initialised". It is added to
   * the global list before being anchored, and its __gc removes the entry, so
   * a failure in between leaves no dangling pointer. */
  closer = lua_newuserdatauv (L, sizeof (lua_State *), 0);
  *closer = NULL;
  lua_newtable (L);
  lua_pushcfunction (L, closer_gc);
  lua_setfield (L, -2, "__gc");
  lua_setmetatable (L, -2);

  G_LOCK (states);
  if (states == NULL)
    states = g_ptr_array_new ();
  g_ptr_array_add (states, S);
  G_UNLOCK (states);
  *closer = S;

  lua_rawsetp (L, LUA_REGISTRYINDEX, &closer_key);
}

void
shell_lua_handle_register_type (lua_State                *L,
                                const ShellLuaHandleType *type)
{
  static const luaL_Reg metamethods[] = {
    { "__index",    handle_index },
    { "__newindex", handle_newindex },
    { "__tostring", handle_tostring },
    { "__eq",       handle_eq },
    { "__gc",       handle_gc },
    { NULL, NULL }
  };

  g_return_if_fail (type != NULL && type->name != NULL);

  ensure_state (L);

  if (find_type (L, type->name) != NULL)
    return;

  luaL_checkstack (L, 6, "shell_lua_handle_register_type");

  if (!luaL_newmetatable (L, type->name))
    {
      /* The name is taken by something that is not one of our types. */
      lua_pop (L, 1);
      luaL_error (L, "cannot register handle type '%s': registry name in use", type->name);
      return;
    }
  luaL_setfuncs (L, metamethods, 0);
  lua_pushboolean (L, 0);
  lua_setfield (L, -2, "__metatable");
  lua_pushboolean (L, 1);
  lua_rawsetp (L, -2, &marker_key);
  lua_pop (L, 1);

  lua_rawgetp (L, LUA_REGISTRYINDEX, &types_key);
  lua_pushstring (L, type->name);
  lua_pushlightuserdata (L, (void *) type);
  lua_rawset (L, -3);
  lua_pop (L, 1);

  /* First registration for a GType wins the GType -> type mapping. */
  lua_rawgetp (L, LUA_REGISTRYINDEX, &gtypes_key);
  lua_pushinteger (L, (lua_Integer) type->gtype);
  if (lua_rawget (L, -2) == LUA_TNIL)
    {
      lua_pop (L, 1);
      lua_pushinteger (L, (lua_Integer) type->gtype);
      lua_pushlightuserdata (L, (void *) type);
      lua_rawset (L, -3);
    }
  else
    {
      lua_pop (L, 1);
    }
  lua_pop (L, 1);
}

/* ------------------------------------------------------------------------ */
/* Push / check                                                              */
/* ------------------------------------------------------------------------ */

/* Remember @t as the type of @object unless a more specific one is known. */
static void
set_type_hint (lua_State                *L,
               GObject                  *object,
               const ShellLuaHandleType *t)
{
  const char *old = g_object_get_data (object, TYPE_HINT_QUARK);
  const ShellLuaHandleType *old_t = old != NULL ? find_type (L, old) : NULL;

  if (old_t == NULL || type_is_a (L, t, old_t->name))
    g_object_set_data (object, TYPE_HINT_QUARK, (gpointer) g_intern_string (t->name));
}

typedef enum {
  PUSH_OK,
  PUSH_NOT_OBJECT,
  PUSH_NO_STATE,
  PUSH_UNKNOWN_NAME,
  PUSH_MISMATCH,
  PUSH_NO_TYPE,
  PUSH_CONFLICT,
} PushResult;

static const ShellLuaHandleType *
find_type_for_gtype (lua_State *L,
                     GType      gtype)
{
  const ShellLuaHandleType *t = NULL;

  if (lua_rawgetp (L, LUA_REGISTRYINDEX, &gtypes_key) == LUA_TTABLE)
    {
      for (; gtype != 0 && t == NULL; gtype = g_type_parent (gtype))
        {
          lua_pushinteger (L, (lua_Integer) gtype);
          if (lua_rawget (L, -2) == LUA_TLIGHTUSERDATA)
            t = lua_touserdata (L, -1);
          lua_pop (L, 1);
        }
    }
  lua_pop (L, 1);
  return t;
}

/* Pushes the handle on success; pushes nothing otherwise. Only raises on
 * memory errors. */
static PushResult
handle_push_impl (lua_State  *L,
                  const char *type_name,
                  GObject    *object,
                  const char **detail)
{
  const ShellLuaHandleType *t;
  lua_State *S;
  Handle *h;

  if (!G_IS_OBJECT (object))
    return PUSH_NOT_OBJECT;

  S = get_scratch (L);
  if (S == NULL)
    return PUSH_NO_STATE;

  if (type_name != NULL)
    {
      t = find_type (L, type_name);
      if (t == NULL)
        {
          *detail = type_name;
          return PUSH_UNKNOWN_NAME;
        }
      if (!g_type_is_a (G_OBJECT_TYPE (object), t->gtype))
        {
          *detail = type_name;
          return PUSH_MISMATCH;
        }
    }
  else
    {
      const ShellLuaHandleType *hinted;
      const char *hint = g_object_get_data (object, TYPE_HINT_QUARK);

      if (lua_rawgetp (L, LUA_REGISTRYINDEX, &resolver_key) == LUA_TLIGHTUSERDATA)
        {
          ShellLuaHandleResolver resolver;

          /* function pointers are stored as a lightuserdata of their address */
          resolver = (ShellLuaHandleResolver) (gpointer) lua_touserdata (L, -1);
          lua_pop (L, 1);
          resolver (L, G_OBJECT_TYPE (object));
        }
      else
        {
          lua_pop (L, 1);
        }

      t = find_type_for_gtype (L, G_OBJECT_TYPE (object));
      if (t == NULL)
        {
          *detail = G_OBJECT_TYPE_NAME (object);
          return PUSH_NO_TYPE;
        }

      /* The type a module pushed the object as wins when it refines the one
       * of the GType (several types may share a GType). */
      hinted = hint != NULL ? find_type (L, hint) : NULL;
      if (hinted != NULL && hinted != t && type_is_a (L, hinted, t->name) &&
          g_type_is_a (G_OBJECT_TYPE (object), hinted->gtype))
        t = hinted;
    }

  luaL_checkstack (L, 4, "shell_lua_handle_push");

  lua_rawgetp (L, LUA_REGISTRYINDEX, &cache_key);   /* cache */
  if (lua_rawgetp (L, -1, object) == LUA_TUSERDATA)
    {
      Handle *c = to_handle (L, -1);

      if (c != NULL && !c->dead && c->object == object)
        {
          if (type_is_a (L, c->type, t->name))
            {
              /* already wrapped as @t or something more specific */
              lua_remove (L, -2);
              return PUSH_OK;
            }
          if (type_is_a (L, t, c->type->name))
            {
              /* upgrade in place: identity is preserved */
              c->type = t;
              luaL_setmetatable (L, t->name);
              if (type_name != NULL)
                set_type_hint (L, object, t);
              lua_remove (L, -2);
              return PUSH_OK;
            }
          *detail = c->type->name;
          lua_pop (L, 2);
          return PUSH_CONFLICT;
        }
    }
  lua_pop (L, 1);                                    /* cache */

  h = lua_newuserdatauv (L, sizeof (Handle), 0);     /* cache, ud */
  h->object = NULL;
  h->type = t;
  h->scratch = S;
  h->dead = FALSE;
  luaL_setmetatable (L, t->name);

  h->object = object;
  g_object_weak_ref (object, weak_notify, h);
  if (type_name != NULL)
    set_type_hint (L, object, t);

  lua_pushvalue (L, -1);
  lua_rawsetp (L, -3, object);                       /* may raise: __gc cleans up */
  lua_remove (L, -2);
  return PUSH_OK;
}

static gboolean
handle_push_try (lua_State  *L,
                 const char *type_name,
                 GObject    *object)
{
  const char *detail = NULL;

  return handle_push_impl (L, type_name, object, &detail) == PUSH_OK;
}

void
shell_lua_handle_push (lua_State  *L,
                       const char *type_name,
                       GObject    *object)
{
  const char *detail = NULL;

  switch (handle_push_impl (L, type_name, object, &detail))
    {
    case PUSH_OK:
      return;
    case PUSH_NOT_OBJECT:
      luaL_error (L, "cannot create a handle: not a GObject");
      break;
    case PUSH_NO_STATE:
      luaL_error (L, "cannot create a handle: no handle type is registered in this state");
      break;
    case PUSH_UNKNOWN_NAME:
      luaL_error (L, "cannot create a handle: unknown handle type '%s'", detail);
      break;
    case PUSH_MISMATCH:
      luaL_error (L, "cannot create a handle: object is a %s, not a %s",
                  G_OBJECT_TYPE_NAME (object), detail);
      break;
    case PUSH_NO_TYPE:
      luaL_error (L, "cannot create a handle: no handle type registered for %s", detail);
      break;
    case PUSH_CONFLICT:
      luaL_error (L, "cannot create a handle as %s: the object is already wrapped as %s",
                  type_name != NULL ? type_name : "(most specific type)", detail);
      break;
    }
}

GObject *
shell_lua_handle_check (lua_State  *L,
                        int         idx,
                        const char *type_name)
{
  Handle *h = to_handle (L, idx);

  if (h == NULL)
    {
      luaL_typeerror (L, idx, type_name != NULL ? type_name : "handle");
      return NULL;
    }
  if (type_name != NULL && !type_is_a (L, h->type, type_name))
    {
      luaL_typeerror (L, idx, type_name);
      return NULL;
    }
  if (h->dead)
    {
      dead_error (L, h);
      return NULL;
    }
  return h->object;
}

GObject *
shell_lua_handle_get (lua_State  *L,
                      int         idx,
                      const char *type_name)
{
  Handle *h = to_handle (L, idx);

  if (h == NULL)
    {
      luaL_typeerror (L, idx, type_name != NULL ? type_name : "handle");
      return NULL;
    }
  if (type_name != NULL && !type_is_a (L, h->type, type_name))
    {
      luaL_typeerror (L, idx, type_name);
      return NULL;
    }
  return h->dead ? NULL : h->object;
}

GObject *
shell_lua_handle_peek (lua_State  *L,
                       int         idx,
                       const char *type_name)
{
  Handle *h = to_handle (L, idx);

  if (h == NULL)
    return NULL;
  if (type_name != NULL && !type_is_a (L, h->type, type_name))
    return NULL;
  if (h->dead)
    {
      dead_error (L, h);
      return NULL;
    }
  return h->object;
}

void
shell_lua_handle_set_resolver (lua_State              *L,
                               ShellLuaHandleResolver  resolver)
{
  g_return_if_fail (L != NULL);

  if (resolver != NULL)
    lua_pushlightuserdata (L, (gpointer) resolver);
  else
    lua_pushnil (L);
  lua_rawsetp (L, LUA_REGISTRYINDEX, &resolver_key);
}

void
shell_lua_handle_foreach_member (lua_State                *L,
                                 int                       idx,
                                 ShellLuaHandleMemberFunc  func,
                                 gpointer                  user_data)
{
  Handle *h = to_handle (L, idx);
  const ShellLuaHandleType *t;
  int depth;

  if (h == NULL)
    return;

  func ("alive", FALSE, user_data);

  for (t = h->type, depth = 0; t != NULL && depth < MAX_TYPE_DEPTH; depth++)
    {
      const ShellLuaHandleProperty *p;
      const ShellLuaHandleVirtual *v;
      const ShellLuaHandleMethod *m;
      const luaL_Reg *r;

      for (p = t->properties; p != NULL && p->name != NULL; p++)
        if (p->readable)
          func (p->name, FALSE, user_data);
      for (v = t->virtuals; v != NULL && v->name != NULL; v++)
        if (v->get != NULL)
          func (v->name, FALSE, user_data);
      for (r = t->methods; r != NULL && r->name != NULL; r++)
        if (r->func != NULL)
          func (r->name, TRUE, user_data);
      for (m = t->methods_ex; m != NULL && m->name != NULL; m++)
        if (m->func != NULL)
          func (m->name, TRUE, user_data);
      t = parent_of (L, t);
    }
}

gboolean
shell_lua_handle_is_alive (lua_State *L,
                           int        idx)
{
  Handle *h = to_handle (L, idx);

  return h != NULL && !h->dead;
}
