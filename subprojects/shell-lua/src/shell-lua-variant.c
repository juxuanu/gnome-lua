/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * GVariant <-> Lua conversion. See shell-lua-variant.h and docs/api/variant.md.
 *
 * The Lua -> GVariant direction never raises a Lua error while it holds
 * GVariants or other C allocations: conversion reports failures through a
 * plain C error string, everything is freed, and only then is the Lua error
 * raised (the longjmp would otherwise leak). Only raw table accesses are used
 * (no metamethods).
 */

#include "shell-lua-variant.h"

#include <lauxlib.h>
#include <math.h>
#include <string.h>

#define MAX_DEPTH 64
#define ROOT_FIELD "value"

/* ===== GVariant -> Lua ================================================== */

/* push_variant() may raise a Lua error (nesting cap, stack or memory) at any
 * point, and the longjmp would skip the g_variant_unref() of every child
 * reference held by the C frames in between. Child references are therefore
 * kept in @refs, a stack that outlives the raise: shell_lua_push_variant()
 * runs the conversion under lua_pcall(), frees whatever is left in @refs and
 * re-raises. */
typedef struct
{
  GVariant  *variant;
  GPtrArray *refs;     /* GVariant *, freed with g_variant_unref */
} PushJob;

static void push_variant (lua_State *L, GVariant *v, int depth, GPtrArray *refs);

/* Takes a reference that is released by g_ptr_array_set_size (refs, mark). */
static GVariant *
hold (GPtrArray *refs,
      GVariant  *ref)
{
  g_ptr_array_add (refs, ref);
  return ref;
}

static void
push_string_variant (lua_State *L,
                     GVariant  *v)
{
  gsize len = 0;
  const char *s = g_variant_get_string (v, &len);

  lua_pushlstring (L, s, len);
}

static void
push_sequence (lua_State *L,
               GVariant  *v,
               int        depth,
               GPtrArray *refs)
{
  gsize n = g_variant_n_children (v);
  guint mark = refs->len;

  lua_createtable (L, (int) MIN (n, (gsize) G_MAXINT), 0);
  for (gsize i = 0; i < n; i++)
    {
      GVariant *child = hold (refs, g_variant_get_child_value (v, i));

      push_variant (L, child, depth + 1, refs);
      g_ptr_array_set_size (refs, mark);
      lua_rawseti (L, -2, (lua_Integer) (i + 1));
    }
}

static void
push_table (lua_State *L,
            GVariant  *v,
            int        depth,
            GPtrArray *refs)
{
  gsize n = g_variant_n_children (v);
  guint mark = refs->len;

  lua_createtable (L, 0, (int) MIN (n, (gsize) G_MAXINT));
  for (gsize i = 0; i < n; i++)
    {
      GVariant *entry = hold (refs, g_variant_get_child_value (v, i));
      GVariant *key = hold (refs, g_variant_get_child_value (entry, 0));
      GVariant *value = hold (refs, g_variant_get_child_value (entry, 1));

      push_string_variant (L, key);
      push_variant (L, value, depth + 1, refs);
      g_ptr_array_set_size (refs, mark);
      lua_rawset (L, -3);
    }
}

static void
push_variant (lua_State *L,
              GVariant  *v,
              int        depth,
              GPtrArray *refs)
{
  guint mark = refs->len;

  if (depth > MAX_DEPTH)
    luaL_error (L, "variant is nested too deeply (limit %d)", MAX_DEPTH);
  if (!lua_checkstack (L, 8))
    luaL_error (L, "stack overflow while converting variant");

  switch (g_variant_classify (v))
    {
    case G_VARIANT_CLASS_BOOLEAN:
      lua_pushboolean (L, g_variant_get_boolean (v));
      break;
    case G_VARIANT_CLASS_BYTE:
      lua_pushinteger (L, g_variant_get_byte (v));
      break;
    case G_VARIANT_CLASS_INT16:
      lua_pushinteger (L, g_variant_get_int16 (v));
      break;
    case G_VARIANT_CLASS_UINT16:
      lua_pushinteger (L, g_variant_get_uint16 (v));
      break;
    case G_VARIANT_CLASS_INT32:
      lua_pushinteger (L, g_variant_get_int32 (v));
      break;
    case G_VARIANT_CLASS_UINT32:
      lua_pushinteger (L, g_variant_get_uint32 (v));
      break;
    case G_VARIANT_CLASS_HANDLE:
      lua_pushinteger (L, g_variant_get_handle (v));
      break;
    case G_VARIANT_CLASS_INT64:
      lua_pushinteger (L, (lua_Integer) g_variant_get_int64 (v));
      break;
    case G_VARIANT_CLASS_UINT64:
      {
        guint64 u = g_variant_get_uint64 (v);

        /* Wrapping into a negative integer would be wrong; a float loses low
         * bits but keeps the magnitude. */
        if (u > (guint64) G_MAXINT64)
          lua_pushnumber (L, (lua_Number) u);
        else
          lua_pushinteger (L, (lua_Integer) u);
        break;
      }
    case G_VARIANT_CLASS_DOUBLE:
      lua_pushnumber (L, g_variant_get_double (v));
      break;
    case G_VARIANT_CLASS_STRING:
    case G_VARIANT_CLASS_OBJECT_PATH:
    case G_VARIANT_CLASS_SIGNATURE:
      push_string_variant (L, v);
      break;
    case G_VARIANT_CLASS_VARIANT:
      {
        GVariant *inner = hold (refs, g_variant_get_variant (v));

        push_variant (L, inner, depth + 1, refs);
        g_ptr_array_set_size (refs, mark);
        break;
      }
    case G_VARIANT_CLASS_MAYBE:
      {
        GVariant *inner = g_variant_get_maybe (v);

        if (inner == NULL)
          lua_pushnil (L);
        else
          {
            hold (refs, inner);
            push_variant (L, inner, depth + 1, refs);
            g_ptr_array_set_size (refs, mark);
          }
        break;
      }
    case G_VARIANT_CLASS_ARRAY:
      {
        const GVariantType *elem = g_variant_type_element (g_variant_get_type (v));

        if (g_variant_type_equal (elem, G_VARIANT_TYPE_BYTE))
          {
            gsize n = 0;
            gconstpointer data = g_variant_get_fixed_array (v, &n, 1);

            lua_pushlstring (L, data ? data : "", n);
          }
        else if (g_variant_type_is_dict_entry (elem))
          {
            const GVariantType *key = g_variant_type_key (elem);

            if (g_variant_type_equal (key, G_VARIANT_TYPE_STRING) ||
                g_variant_type_equal (key, G_VARIANT_TYPE_OBJECT_PATH))
              push_table (L, v, depth, refs);
            else
              push_sequence (L, v, depth, refs);
          }
        else
          push_sequence (L, v, depth, refs);
        break;
      }
    case G_VARIANT_CLASS_TUPLE:
    case G_VARIANT_CLASS_DICT_ENTRY:
      push_sequence (L, v, depth, refs);
      break;
    default:
      g_assert_not_reached ();
    }
}

static int
push_job (lua_State *L)
{
  PushJob *job = lua_touserdata (L, 1);

  lua_settop (L, 0);
  push_variant (L, job->variant, 0, job->refs);
  return 1;
}

void
shell_lua_push_variant (lua_State *L,
                        GVariant  *variant)
{
  PushJob job;
  int status;

  g_return_if_fail (variant != NULL);

  luaL_checkstack (L, 3, "pushing a variant");
  job.variant = variant;
  job.refs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);

  lua_pushcfunction (L, push_job);
  lua_pushlightuserdata (L, &job);
  status = lua_pcall (L, 1, 1, 0);

  /* whatever the failure left behind */
  g_ptr_array_unref (job.refs);

  if (status != LUA_OK)
    lua_error (L);
}

/* ===== Typed values (boxes) ============================================ */

#define BOX_MT "shell.lua.variant-box"

typedef struct
{
  GVariant *variant;
} Box;

static int
box_gc (lua_State *L)
{
  Box *box = lua_touserdata (L, 1);

  g_clear_pointer (&box->variant, g_variant_unref);
  return 0;
}

static int
box_tostring (lua_State *L)
{
  Box *box = lua_touserdata (L, 1);

  if (box->variant == NULL)
    lua_pushliteral (L, "<variant>");
  else
    lua_pushfstring (L, "<variant '%s'>", g_variant_get_type_string (box->variant));
  return 1;
}

void
shell_lua_push_variant_box (lua_State *L,
                            GVariant  *variant)
{
  Box *box;

  g_return_if_fail (variant != NULL);

  luaL_checkstack (L, 3, "pushing a typed value");
  box = lua_newuserdatauv (L, sizeof (Box), 0);
  box->variant = NULL;
  if (luaL_newmetatable (L, BOX_MT))
    {
      lua_pushcfunction (L, box_gc);
      lua_setfield (L, -2, "__gc");
      lua_pushcfunction (L, box_tostring);
      lua_setfield (L, -2, "__tostring");
      lua_pushliteral (L, "shell.dbus.variant");
      lua_setfield (L, -2, "__metatable");
    }
  lua_setmetatable (L, -2);
  box->variant = g_variant_ref_sink (variant);
}

GVariant *
shell_lua_variant_box_peek (lua_State *L,
                            int        idx)
{
  Box *box;

  if (lua_type (L, idx) != LUA_TUSERDATA)
    return NULL;
  box = luaL_testudata (L, idx, BOX_MT);
  return box != NULL ? box->variant : NULL;
}

/* ===== Lua -> GVariant ================================================== */

typedef struct
{
  lua_State *L;
  GString   *path;      /* current field path, for messages */
  char      *error;     /* set on failure (g_malloc'd) */
  guint      flags;       /* ShellLuaVariantFlags */
  gsize      max_values;  /* 0: unlimited */
  gsize      n_values;    /* every value converted so far, containers included */
} Ctx;

static GVariant *convert (Ctx *ctx, int idx, const GVariantType *type, int depth);

G_GNUC_PRINTF (2, 3)
static GVariant *
fail (Ctx        *ctx,
      const char *format,
      ...)
{
  va_list args;

  g_clear_pointer (&ctx->error, g_free);
  va_start (args, format);
  ctx->error = g_strdup_vprintf (format, args);
  va_end (args);

  return NULL;
}

static void
free_children (GPtrArray *children)
{
  for (guint i = 0; i < children->len; i++)
    g_variant_unref (g_variant_ref_sink (g_ptr_array_index (children, i)));
  g_ptr_array_free (children, TRUE);
}

static const char *
expected_name (const GVariantType *type)
{
  if (g_variant_type_is_maybe (type))
    return expected_name (g_variant_type_element (type));
  if (g_variant_type_equal (type, G_VARIANT_TYPE_BOOLEAN))
    return "boolean";
  if (g_variant_type_equal (type, G_VARIANT_TYPE_DOUBLE))
    return "number";
  if (g_variant_type_is_basic (type) &&
      (g_variant_type_equal (type, G_VARIANT_TYPE_STRING) ||
       g_variant_type_equal (type, G_VARIANT_TYPE_OBJECT_PATH) ||
       g_variant_type_equal (type, G_VARIANT_TYPE_SIGNATURE)))
    return "string";
  if (g_variant_type_is_basic (type) && !g_variant_type_equal (type, G_VARIANT_TYPE ("?")))
    return "integer";
  if (g_variant_type_is_array (type) || g_variant_type_is_tuple (type) ||
      g_variant_type_is_dict_entry (type))
    return "table";
  return "value";
}

static GVariant *
mismatch (Ctx                *ctx,
          int                 idx,
          const GVariantType *type)
{
  char *tstr = g_variant_type_dup_string (type);

  fail (ctx, "expected %s for field %s, got %s (variant type '%s')",
        expected_name (type), ctx->path->str, luaL_typename (ctx->L, idx), tstr);
  g_free (tstr);
  return NULL;
}

/* Classifies the Lua number at @idx. Returns 1 and sets @s when it fits in a
 * gint64, 2 and sets @u when it fits only in a guint64, 0 when it is not an
 * integral value (or not a number at all, see ctx caller). */
static int
number_as_integer (lua_State *L,
                   int        idx,
                   gint64    *s,
                   guint64   *u)
{
  if (lua_isinteger (L, idx))
    {
      *s = (gint64) lua_tointeger (L, idx);
      return 1;
    }
  else
    {
      lua_Number n = lua_tonumber (L, idx);

      if (!(n == floor (n)))  /* also rejects NaN and, below, +-inf */
        return 0;
      if (n >= -9223372036854775808.0 && n < 9223372036854775808.0)
        {
          *s = (gint64) n;
          return 1;
        }
      if (n >= 9223372036854775808.0 && n < 18446744073709551616.0)
        {
          *u = (guint64) n;
          return 2;
        }
      return 0;
    }
}

static gboolean
check_string (Ctx        *ctx,
              const char *s,
              gsize       len,
              char        kind)
{
  if (memchr (s, '\0', len) != NULL)
    {
      fail (ctx, "string for field %s contains an embedded NUL (use type 'ay' for bytes)",
            ctx->path->str);
      return FALSE;
    }
  if (!g_utf8_validate (s, (gssize) len, NULL))
    {
      fail (ctx, "string for field %s is not valid UTF-8 (use type 'ay' for bytes)",
            ctx->path->str);
      return FALSE;
    }
  if (kind == 'o' && !g_variant_is_object_path (s))
    {
      fail (ctx, "string '%s' for field %s is not a valid D-Bus object path",
            s, ctx->path->str);
      return FALSE;
    }
  if (kind == 'g' && !g_variant_is_signature (s))
    {
      fail (ctx, "string '%s' for field %s is not a valid D-Bus signature",
            s, ctx->path->str);
      return FALSE;
    }
  return TRUE;
}

static GVariant *
convert_string (Ctx  *ctx,
                int   idx,
                char  kind,
                const GVariantType *type)
{
  size_t len;
  const char *s;

  if (lua_type (ctx->L, idx) != LUA_TSTRING)
    return mismatch (ctx, idx, type);

  s = lua_tolstring (ctx->L, idx, &len);
  if (!check_string (ctx, s, len, kind))
    return NULL;

  switch (kind)
    {
    case 'o':
      return g_variant_new_object_path (s);
    case 'g':
      return g_variant_new_signature (s);
    default:
      return g_variant_new_string (s);
    }
}

static GVariant *
convert_basic_number (Ctx                *ctx,
                      int                 idx,
                      const GVariantType *type)
{
  const char kind = g_variant_type_peek_string (type)[0];
  gint64 s = 0;
  guint64 u = 0;
  int r;
  gint64 lo, hi;

  if (kind == 'd')
    {
      if (lua_type (ctx->L, idx) != LUA_TNUMBER)
        return mismatch (ctx, idx, type);
      return g_variant_new_double (lua_tonumber (ctx->L, idx));
    }

  if (lua_type (ctx->L, idx) != LUA_TNUMBER)
    return mismatch (ctx, idx, type);

  r = number_as_integer (ctx->L, idx, &s, &u);
  if (r == 0)
    return fail (ctx, "expected integer for field %s, got non-integral number %f",
                 ctx->path->str, (double) lua_tonumber (ctx->L, idx));

  if (kind == 't')
    {
      if (r == 2)
        return g_variant_new_uint64 (u);
      if (s < 0)
        return fail (ctx, "integer %" G_GINT64_FORMAT " out of range for field %s (type 't')",
                     s, ctx->path->str);
      return g_variant_new_uint64 ((guint64) s);
    }

  if (r == 2)
    return fail (ctx, "integer %" G_GUINT64_FORMAT " out of range for field %s (type '%c')",
                 u, ctx->path->str, kind);

  switch (kind)
    {
    case 'y': lo = 0;          hi = G_MAXUINT8;  break;
    case 'n': lo = G_MININT16; hi = G_MAXINT16;  break;
    case 'q': lo = 0;          hi = G_MAXUINT16; break;
    case 'i':
    case 'h': lo = G_MININT32; hi = G_MAXINT32;  break;
    case 'u': lo = 0;          hi = G_MAXUINT32; break;
    case 'x': lo = G_MININT64; hi = G_MAXINT64;  break;
    default:
      return mismatch (ctx, idx, type);
    }

  if (s < lo || s > hi)
    return fail (ctx, "integer %" G_GINT64_FORMAT " out of range for field %s (type '%c')",
                 s, ctx->path->str, kind);

  switch (kind)
    {
    case 'y': return g_variant_new_byte ((guint8) s);
    case 'n': return g_variant_new_int16 ((gint16) s);
    case 'q': return g_variant_new_uint16 ((guint16) s);
    case 'i': return g_variant_new_int32 ((gint32) s);
    case 'h': return g_variant_new_handle ((gint32) s);
    case 'u': return g_variant_new_uint32 ((guint32) s);
    default:  return g_variant_new_int64 (s);
    }
}

/* Largest positive integer key of the table at @idx (0 if none). Unlike #t
 * this is well defined for tables with holes, which matter for arrays and
 * tuples of maybe types (nil == nothing). */
static gsize
max_sequence_key (lua_State *L,
                  int         idx)
{
  gsize max = 0;

  lua_pushnil (L);
  while (lua_next (L, idx))
    {
      lua_pop (L, 1);
      if (lua_isinteger (L, -1) && lua_tointeger (L, -1) >= 1)
        max = MAX (max, (gsize) lua_tointeger (L, -1));
    }
  return max;
}

static int
compare_keys (gconstpointer a,
              gconstpointer b)
{
  return strcmp (*(const char * const *) a, *(const char * const *) b);
}

/* Table with string keys -> a{s*}/a{o*}/a{g*}. Non-string keys are ignored. */
static GVariant *
convert_dict (Ctx                *ctx,
              int                 idx,
              const GVariantType *type,
              int                 depth)
{
  lua_State *L = ctx->L;
  const GVariantType *entry_type = g_variant_type_element (type);
  const GVariantType *key_type = g_variant_type_key (entry_type);
  const GVariantType *value_type = g_variant_type_value (entry_type);
  const char key_kind = g_variant_type_peek_string (key_type)[0];
  GPtrArray *keys = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *children = g_ptr_array_new ();
  GVariant *result = NULL;
  gsize base_len = ctx->path->len;

  lua_pushnil (L);
  while (lua_next (L, idx))
    {
      lua_pop (L, 1);
      if (lua_type (L, -1) == LUA_TSTRING)
        {
          size_t len;
          const char *k = lua_tolstring (L, -1, &len);

          g_ptr_array_add (keys, g_strndup (k, len));
          if (strlen (g_ptr_array_index (keys, keys->len - 1)) != len)
            {
              g_string_append_printf (ctx->path, ".%s", (const char *) g_ptr_array_index (keys, keys->len - 1));
              lua_pop (L, 1);
              fail (ctx, "key of field %s contains an embedded NUL", ctx->path->str);
              goto out;
            }
        }
    }

  g_ptr_array_sort (keys, compare_keys);

  for (guint i = 0; i < keys->len; i++)
    {
      const char *k = g_ptr_array_index (keys, i);
      GVariant *key_v, *val_v;

      g_string_append_c (ctx->path, '.');
      g_string_append (ctx->path, k);

      if (!check_string (ctx, k, strlen (k), key_kind))
        goto out;

      lua_pushstring (L, k);
      lua_rawget (L, idx);
      val_v = convert (ctx, lua_absindex (L, -1), value_type, depth + 1);
      lua_pop (L, 1);
      if (val_v == NULL)
        goto out;

      switch (key_kind)
        {
        case 'o': key_v = g_variant_new_object_path (k); break;
        case 'g': key_v = g_variant_new_signature (k); break;
        default:  key_v = g_variant_new_string (k); break;
        }
      g_ptr_array_add (children, g_variant_new_dict_entry (key_v, val_v));
      g_string_truncate (ctx->path, base_len);
    }

  if (children->len == 0 && !g_variant_type_is_definite (entry_type))
    {
      fail (ctx, "cannot build empty array of indefinite type for field %s", ctx->path->str);
      goto out;
    }

  result = g_variant_new_array (children->len == 0 ? entry_type : NULL,
                                (GVariant * const *) children->pdata, children->len);
  g_ptr_array_free (children, TRUE);
  children = NULL;

out:
  if (children != NULL)
    free_children (children);
  g_ptr_array_unref (keys);
  return result;
}

/* Sequence of 1..#t elements -> array / tuple / dict-entry children. */
static gboolean
convert_elements (Ctx                *ctx,
                  int                 idx,
                  const GVariantType *first,   /* NULL: all elements use @elem */
                  const GVariantType *elem,
                  gsize               count,
                  GPtrArray          *children,
                  int                 depth)
{
  lua_State *L = ctx->L;
  gsize base_len = ctx->path->len;
  const GVariantType *t = first;

  for (gsize i = 0; i < count; i++)
    {
      GVariant *child;

      g_string_append_printf (ctx->path, "[%" G_GSIZE_FORMAT "]", i + 1);
      lua_rawgeti (L, idx, (lua_Integer) (i + 1));
      child = convert (ctx, lua_absindex (L, -1), t ? t : elem, depth + 1);
      lua_pop (L, 1);
      if (child == NULL)
        return FALSE;
      g_ptr_array_add (children, child);
      g_string_truncate (ctx->path, base_len);

      if (first)
        t = g_variant_type_next (t);
    }
  return TRUE;
}

static GVariant *
convert_array_or_tuple (Ctx                *ctx,
                        int                 idx,
                        const GVariantType *type,
                        int                 depth)
{
  lua_State *L = ctx->L;
  GPtrArray *children;
  GVariant *result = NULL;
  gsize n = lua_rawlen (L, idx);

  if (g_variant_type_is_array (type))
    {
      const GVariantType *elem = g_variant_type_element (type);

      if (g_variant_type_is_maybe (elem))
        n = max_sequence_key (L, idx);

      if (n == 0)
        {
          lua_pushnil (L);
          if (lua_next (L, idx))
            {
              lua_pop (L, 2);
              /* a{s*} handled by the caller; anything else with keys but no
               * sequence part is a mismatch */
              return fail (ctx, "expected sequence for field %s, got table without elements 1..n",
                           ctx->path->str);
            }
          if (!g_variant_type_is_definite (elem))
            return fail (ctx, "cannot build empty array of indefinite type for field %s",
                         ctx->path->str);
          return g_variant_new_array (elem, NULL, 0);
        }

      children = g_ptr_array_new ();
      if (convert_elements (ctx, idx, NULL, elem, n, children, depth))
        {
          GVariant *first = g_ptr_array_index (children, 0);
          gboolean same = TRUE;

          /* indefinite element types: all children must agree */
          for (guint i = 1; i < children->len && same; i++)
            same = g_variant_is_of_type (g_ptr_array_index (children, i),
                                         g_variant_get_type (first));
          if (same)
            {
              result = g_variant_new_array (NULL, (GVariant * const *) children->pdata,
                                            children->len);
              g_ptr_array_free (children, TRUE);
              return result;
            }
          fail (ctx, "elements of field %s do not share one type", ctx->path->str);
        }
      free_children (children);
      return NULL;
    }
  else
    {
      /* tuple or dict entry */
      gsize want;
      const GVariantType *first;

      if (g_variant_type_equal (type, G_VARIANT_TYPE_TUPLE))
        return fail (ctx, "cannot convert to indefinite tuple type for field %s", ctx->path->str);

      want = g_variant_type_is_tuple (type) ? g_variant_type_n_items (type) : 2;
      first = g_variant_type_is_tuple (type) ? g_variant_type_first (type)
                                             : g_variant_type_key (type);

      /* Trailing/inner nils are legal for maybe-typed members; they are
       * converted (and rejected for other types) as nil values. */
      n = max_sequence_key (L, idx);
      if (n > want)
        return fail (ctx, "expected %" G_GSIZE_FORMAT " elements for field %s, got %" G_GSIZE_FORMAT,
                     want, ctx->path->str, n);
      n = want;

      children = g_ptr_array_new ();
      if (g_variant_type_is_dict_entry (type))
        {
          /* key and value types are not siblings in the type API, so convert
           * the two members explicitly */
          GVariant *k, *v;
          gsize base_len = ctx->path->len;

          g_string_append (ctx->path, "[1]");
          lua_rawgeti (L, idx, 1);
          k = convert (ctx, lua_absindex (L, -1), first, depth + 1);
          lua_pop (L, 1);
          g_string_truncate (ctx->path, base_len);
          if (k == NULL)
            goto fail_dict;
          g_string_append (ctx->path, "[2]");
          lua_rawgeti (L, idx, 2);
          v = convert (ctx, lua_absindex (L, -1), g_variant_type_value (type), depth + 1);
          lua_pop (L, 1);
          if (v == NULL)
            {
              g_variant_unref (g_variant_ref_sink (k));
              goto fail_dict;
            }
          g_string_truncate (ctx->path, base_len);
          g_ptr_array_unref (children);
          return g_variant_new_dict_entry (k, v);
        fail_dict:
          g_ptr_array_unref (children);
          return NULL;
        }

      if (convert_elements (ctx, idx, first, NULL, n, children, depth))
        {
          result = g_variant_new_tuple ((GVariant * const *) children->pdata, children->len);
          g_ptr_array_free (children, TRUE);
          return result;
        }
      free_children (children);
      return NULL;
    }
}

/* ---- inference (no target type) ---------------------------------------- */

static GVariant *
convert_inferred (Ctx *ctx,
                  int  idx,
                  int  depth)
{
  lua_State *L = ctx->L;

  switch (lua_type (L, idx))
    {
    case LUA_TBOOLEAN:
      return g_variant_new_boolean (lua_toboolean (L, idx));
    case LUA_TNUMBER:
      if (lua_isinteger (L, idx))
        return g_variant_new_int64 ((gint64) lua_tointeger (L, idx));
      return g_variant_new_double (lua_tonumber (L, idx));
    case LUA_TSTRING:
      /* worker messages: bytes that are not a string cross as `ay` (they
       * arrive as a Lua string again, variant.md) */
      if (ctx->flags & SHELL_LUA_VARIANT_BYTES_AS_AY)
        {
          size_t len;
          const char *str = lua_tolstring (L, idx, &len);

          if (memchr (str, '\0', len) != NULL || !g_utf8_validate (str, (gssize) len, NULL))
            return g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, str, len, 1);
        }
      return convert_string (ctx, idx, 's', G_VARIANT_TYPE_STRING);
    case LUA_TNIL:
      return fail (ctx, "cannot infer a variant type from nil for field %s "
                   "(ambiguous; give a type or omit the value)", ctx->path->str);
    case LUA_TTABLE:
      {
        gsize n_int = 0, n_str = 0, max_int = 0;

        lua_pushnil (L);
        while (lua_next (L, idx))
          {
            lua_pop (L, 1); /* keep key for next */
            if (lua_isinteger (L, -1) && lua_tointeger (L, -1) >= 1)
              {
                n_int++;
                max_int = MAX (max_int, (gsize) lua_tointeger (L, -1));
              }
            else if (lua_type (L, -1) == LUA_TSTRING)
              n_str++;
            else
              {
                const char *kt = luaL_typename (L, -1);

                lua_pop (L, 1);
                return fail (ctx, "unsupported table key of type %s in field %s",
                             kt, ctx->path->str);
              }
          }

        if (n_str > 0 && n_int > 0 && (ctx->flags & SHELL_LUA_VARIANT_STRICT_KEYS))
          return fail (ctx, "field %s mixes integer and string keys; it is neither a sequence "
                       "nor a dictionary", ctx->path->str);

        if (n_str == 0 && n_int > 0)
          {
            if (n_int != max_int)
              return fail (ctx, "field %s is a sparse array (holes in 1..%" G_GSIZE_FORMAT
                           "); cannot convert", ctx->path->str, max_int);
            return convert (ctx, idx, G_VARIANT_TYPE ("av"), depth);
          }
        return convert (ctx, idx, G_VARIANT_TYPE ("a{sv}"), depth);
      }
    default:
      return fail (ctx, "cannot convert Lua %s for field %s to a variant",
                   luaL_typename (L, idx), ctx->path->str);
    }
}

/* ---- main dispatcher ----------------------------------------------------- */

static GVariant *
convert (Ctx                *ctx,
         int                 idx,
         const GVariantType *type,
         int                 depth)
{
  lua_State *L = ctx->L;
  GVariant *result;

  idx = lua_absindex (L, idx);
  ctx->n_values++;
  if (ctx->max_values != 0 && ctx->n_values > ctx->max_values)
    return fail (ctx, "too many values (limit %" G_GSIZE_FORMAT ")", ctx->max_values);

  if (depth > MAX_DEPTH)
    return fail (ctx, "value for field %s is nested too deeply (limit %d)",
                 ctx->path->str, MAX_DEPTH);
  if (!lua_checkstack (L, 8))
    return fail (ctx, "stack overflow while converting field %s", ctx->path->str);

  if (shell_lua_variant_box_peek (L, idx) != NULL)
    {
      /* A typed value goes where a `v` is expected and nowhere else */
      if (type != NULL && g_variant_type_equal (type, G_VARIANT_TYPE_VARIANT))
        return g_variant_new_variant (shell_lua_variant_box_peek (L, idx));
      return fail (ctx, "typed value (shell.dbus.variant) for field %s is only valid "
                   "where the type is 'v'", ctx->path->str);
    }

  if (type == NULL || g_variant_type_equal (type, G_VARIANT_TYPE_ANY))
    return convert_inferred (ctx, idx, depth);

  if (g_variant_type_equal (type, G_VARIANT_TYPE_VARIANT))
    {
      result = convert_inferred (ctx, idx, depth);
      return result ? g_variant_new_variant (result) : NULL;
    }

  if (g_variant_type_equal (type, G_VARIANT_TYPE_BASIC))
    {
      result = convert_inferred (ctx, idx, depth);
      if (result && !g_variant_type_is_basic (g_variant_get_type (result)))
        {
          g_variant_unref (g_variant_ref_sink (result));
          return mismatch (ctx, idx, type);
        }
      return result;
    }

  if (g_variant_type_is_maybe (type))
    {
      const GVariantType *elem = g_variant_type_element (type);

      if (lua_isnil (L, idx))
        {
          if (!g_variant_type_is_definite (elem))
            return fail (ctx, "cannot build 'nothing' of indefinite type for field %s",
                         ctx->path->str);
          return g_variant_new_maybe (elem, NULL);
        }
      result = convert (ctx, idx, elem, depth + 1);
      return result ? g_variant_new_maybe (NULL, result) : NULL;
    }

  if (g_variant_type_is_array (type))
    {
      const GVariantType *elem = g_variant_type_element (type);

      if (g_variant_type_equal (elem, G_VARIANT_TYPE_BYTE) && lua_type (L, idx) == LUA_TSTRING)
        {
          size_t len;
          const char *s = lua_tolstring (L, idx, &len);
          gpointer copy = g_memdup2 (s, len);

          if (len == 0)
            return g_variant_new_array (G_VARIANT_TYPE_BYTE, NULL, 0);
          return g_variant_new_from_data (type, copy, len, TRUE, g_free, copy);
        }

      if (lua_type (L, idx) != LUA_TTABLE)
        return mismatch (ctx, idx, type);

      if (g_variant_type_is_dict_entry (elem))
        {
          const char kind = g_variant_type_peek_string (g_variant_type_key (elem))[0];

          if (kind == 's' || kind == 'o' || kind == 'g')
            return convert_dict (ctx, idx, type, depth);
        }
      return convert_array_or_tuple (ctx, idx, type, depth);
    }

  if (g_variant_type_is_tuple (type) || g_variant_type_is_dict_entry (type))
    {
      if (lua_type (L, idx) != LUA_TTABLE)
        return mismatch (ctx, idx, type);
      return convert_array_or_tuple (ctx, idx, type, depth);
    }

  /* basic types */
  switch (g_variant_type_peek_string (type)[0])
    {
    case 'b':
      if (!lua_isboolean (L, idx))
        return mismatch (ctx, idx, type);
      return g_variant_new_boolean (lua_toboolean (L, idx));
    case 's':
      return convert_string (ctx, idx, 's', type);
    case 'o':
      return convert_string (ctx, idx, 'o', type);
    case 'g':
      return convert_string (ctx, idx, 'g', type);
    case 'y': case 'n': case 'q': case 'i': case 'u': case 'x': case 't':
    case 'h': case 'd':
      return convert_basic_number (ctx, idx, type);
    default:
      return fail (ctx, "unsupported variant type for field %s", ctx->path->str);
    }
}

GVariant *
shell_lua_to_variant_full (lua_State          *L,
                           int                 idx,
                           const GVariantType *type,
                           guint               flags,
                           gsize               max_values)
{
  Ctx ctx = { L, g_string_new (ROOT_FIELD), NULL, flags, max_values, 0 };
  GVariant *result;

  idx = lua_absindex (L, idx);
  result = convert (&ctx, idx, type, 0);

  if (result == NULL)
    {
      lua_pushstring (L, ctx.error ? ctx.error : "variant conversion failed");
      g_free (ctx.error);
      g_string_free (ctx.path, TRUE);
      luaL_error (L, "%s", lua_tostring (L, -1));
      return NULL; /* not reached */
    }

  g_string_free (ctx.path, TRUE);
  g_free (ctx.error);
  return g_variant_ref_sink (result);
}

GVariant *
shell_lua_to_variant (lua_State          *L,
                      int                 idx,
                      const GVariantType *type)
{
  return shell_lua_to_variant_full (L, idx, type, SHELL_LUA_VARIANT_NONE, 0);
}
