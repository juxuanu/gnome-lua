/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * GVariant <-> Lua conversion (docs/api/variant.md).
 */

#include "test-utils.h"

#include <lualib.h>

#include "shell-lua-variant.h"

typedef struct {
  lua_State *L;
} Fixture;

static void
fixture_setup (Fixture *f, gconstpointer data)
{
  f->L = luaL_newstate ();
  luaL_openlibs (f->L);
}

static void
fixture_teardown (Fixture *f, gconstpointer data)
{
  lua_close (f->L);
}

/* ------------------------------------------------------------------------ */
/* Helpers: all conversions run under lua_pcall, as a Lua error raised       */
/* outside a protected call would abort the process.                         */
/* ------------------------------------------------------------------------ */

typedef struct {
  char               *code;
  const GVariantType *type;
  GVariant           *result;
} EvalCtx;

static int
cf_eval (lua_State *L)
{
  EvalCtx *c = lua_touserdata (L, 1);

  if (luaL_loadstring (L, c->code) != LUA_OK)
    return lua_error (L);
  lua_call (L, 0, 1);
  c->result = shell_lua_to_variant (L, -1, c->type);
  return 0;
}

/* Evaluate the Lua expression @expr and convert it. NULL + @err on failure. */
static GVariant *
eval_variant (lua_State *L, const char *expr, const char *type, char **err)
{
  EvalCtx c = { g_strconcat ("return ", expr, NULL), NULL, NULL };

  if (type != NULL)
    c.type = G_VARIANT_TYPE (type);
  *err = NULL;
  lua_pushcfunction (L, cf_eval);
  lua_pushlightuserdata (L, &c);
  if (lua_pcall (L, 1, 0, 0) != LUA_OK)
    {
      *err = g_strdup (lua_tostring (L, -1));
      lua_pop (L, 1);
    }
  g_free (c.code);
  g_assert_cmpint (lua_gettop (L), ==, 0);
  return c.result;
}

/* Table-driven checks report every mismatch instead of stopping at the
 * first one (run with -k to see them all when a test fails). */
G_GNUC_PRINTF (1, 2)
static void
fail_printf (const char *format, ...)
{
  va_list args;
  char *message;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);
  g_test_message ("%s", message);
  g_test_fail ();
  g_free (message);
}

static void
assert_converts (lua_State *L, const char *expr, const char *type,
                 const char *expected_type, const char *expected_text)
{
  g_autofree char *err = NULL;
  GVariant *v = eval_variant (L, expr, type, &err);
  g_autofree char *text = NULL;

  if (v == NULL)
    {
      fail_printf ("converting %s (%s) failed: %s", expr, type ? type : "inferred", err);
      return;
    }
  text = g_variant_print (v, TRUE);
  if (g_strcmp0 (g_variant_get_type_string (v), expected_type) != 0 ||
      g_strcmp0 (text, expected_text) != 0)
    fail_printf ("converting %s (%s): got %s '%s', expected %s '%s'", expr,
                        type ? type : "inferred", g_variant_get_type_string (v), text,
                        expected_type, expected_text);
  g_variant_unref (v);
}

/* @message is matched exactly when @exact, else as a substring. */
static void
assert_fails (lua_State *L, const char *expr, const char *type, const char *message,
              gboolean exact)
{
  g_autofree char *err = NULL;
  GVariant *v = eval_variant (L, expr, type, &err);

  if (v != NULL)
    {
      g_autofree char *text = g_variant_print (v, TRUE);

      fail_printf ("converting %s (%s) unexpectedly gave %s", expr,
                          type ? type : "inferred", text);
      g_variant_unref (v);
      return;
    }
  if (exact ? g_strcmp0 (err, message) != 0 : strstr (err, message) == NULL)
    fail_printf ("converting %s (%s) failed with\n  '%s'\nexpected%s\n  '%s'", expr,
                        type ? type : "inferred", err, exact ? "" : " (substring)", message);
}

#define ASSERT_CONVERTS(expr, type, etype, etext) \
  assert_converts (f->L, expr, type, etype, etext)
#define ASSERT_ERROR(expr, type, msg) assert_fails (f->L, expr, type, msg, TRUE)
#define ASSERT_ERROR_SUB(expr, type, msg) assert_fails (f->L, expr, type, msg, FALSE)

typedef struct {
  GVariant *variant;
} PushCtx;

static int
cf_push_global (lua_State *L)
{
  PushCtx *c = lua_touserdata (L, 1);

  shell_lua_push_variant (L, c->variant);
  lua_setglobal (L, "v");
  return 0;
}

/* Parse @text, push it as global `v` (returns the pcall error or NULL). */
static char *
push_text (lua_State *L, const char *text)
{
  g_autoptr (GError) error = NULL;
  PushCtx c;
  char *message = NULL;

  c.variant = g_variant_parse (NULL, text, NULL, NULL, &error);
  if (c.variant == NULL)
    g_error ("cannot parse '%s': %s", text, error->message);

  lua_pushcfunction (L, cf_push_global);
  lua_pushlightuserdata (L, &c);
  if (lua_pcall (L, 1, 0, 0) != LUA_OK)
    {
      message = g_strdup (lua_tostring (L, -1));
      lua_pop (L, 1);
    }
  g_variant_unref (c.variant);
  return message;
}

static void
run_lua (lua_State *L, const char *code)
{
  if (luaL_loadstring (L, code) != LUA_OK || lua_pcall (L, 0, 0, 0) != LUA_OK)
    g_error ("Lua failed: %s\n--- chunk:\n%s", lua_tostring (L, -1), code);
}

/* Push @text as `v`, then require the Lua condition @check to hold. */
static void
assert_pushes (lua_State *L, const char *text, const char *check)
{
  g_autofree char *err = push_text (L, text);
  g_autofree char *code = g_strdup_printf ("if not (%s) then error('check failed', 0) end", check);

  if (err != NULL)
    g_error ("pushing %s failed: %s", text, err);
  if (luaL_loadstring (L, code) != LUA_OK || lua_pcall (L, 0, 0, 0) != LUA_OK)
    g_error ("pushing %s: check '%s' failed: %s", text, check, lua_tostring (L, -1));
}

/* ------------------------------------------------------------------------ */
/* GVariant -> Lua                                                           */
/* ------------------------------------------------------------------------ */

static void
test_push_basic (Fixture *f, gconstpointer data)
{
  lua_State *L = f->L;

  assert_pushes (L, "true", "v == true");
  assert_pushes (L, "false", "v == false");
  assert_pushes (L, "byte 200", "v == 200 and math.type(v) == 'integer'");
  assert_pushes (L, "int16 -5", "v == -5 and math.type(v) == 'integer'");
  assert_pushes (L, "uint16 65535", "v == 65535 and math.type(v) == 'integer'");
  assert_pushes (L, "int32 -7", "v == -7 and math.type(v) == 'integer'");
  assert_pushes (L, "uint32 4294967295", "v == 4294967295 and math.type(v) == 'integer'");
  assert_pushes (L, "int64 -9223372036854775807", "v == -9223372036854775807 and math.type(v) == 'integer'");
  assert_pushes (L, "uint64 9223372036854775807", "v == math.maxinteger and math.type(v) == 'integer'");
  assert_pushes (L, "handle 3", "v == 3 and math.type(v) == 'integer'");
  assert_pushes (L, "1.5", "v == 1.5 and math.type(v) == 'float'");
  assert_pushes (L, "2.0", "v == 2 and math.type(v) == 'float'");
  assert_pushes (L, "'hello'", "v == 'hello'");
  assert_pushes (L, "''", "v == ''");
  assert_pushes (L, "objectpath '/org/gnome/Shell'", "v == '/org/gnome/Shell'");
  assert_pushes (L, "signature 'a{sv}'", "v == 'a{sv}'");

  /* uint64 beyond int64 keeps its magnitude as a float */
  assert_pushes (L, "uint64 9223372036854775808", "math.type(v) == 'float' and v == 2^63");
}

static void
test_push_variants_and_maybe (Fixture *f, gconstpointer data)
{
  lua_State *L = f->L;

  /* v is unwrapped, however deep */
  assert_pushes (L, "<'x'>", "v == 'x'");
  assert_pushes (L, "<<<int32 4>>>", "v == 4");
  /* m* is nil or the inner value */
  assert_pushes (L, "@mi nothing", "v == nil");
  assert_pushes (L, "@mi 3", "v == 3");
  assert_pushes (L, "@mas nothing", "v == nil");
  assert_pushes (L, "@mas ['a']", "v[1] == 'a' and #v == 1");
  assert_pushes (L, "@mmi 3", "v == 3");
  /* a maybe inside a sequence leaves a hole */
  assert_pushes (L, "@ams [nothing, 'a']", "v[1] == nil and v[2] == 'a'");
}

static void
test_push_containers (Fixture *f, gconstpointer data)
{
  lua_State *L = f->L;

  /* a{s*}, a{o*}: tables keyed by string */
  assert_pushes (L, "@a{sv} {'a': <1>, 'b': <'x'>, 'c': <[<1>, <'two'>]>}",
                 "v.a == 1 and v.b == 'x' and v.c[1] == 1 and v.c[2] == 'two' and #v.c == 2");
  assert_pushes (L, "@a{sv} {}", "next(v) == nil");
  assert_pushes (L, "@a{ss} {'k': 'v', 'k2': 'v2'}", "v.k == 'v' and v.k2 == 'v2'");
  assert_pushes (L, "@a{oi} {'/a': 1, '/b': 2}", "v['/a'] == 1 and v['/b'] == 2");
  assert_pushes (L, "@a{sa{sb}} {'o': {'i': true}}", "v.o.i == true");
  /* other dictionaries are sequences of {key, value} */
  assert_pushes (L, "@a{ii} {1: 2, 3: 4}",
                 "#v == 2 and v[1][1] == 1 and v[1][2] == 2 and v[2][1] == 3 and v[2][2] == 4");
  /* arrays and tuples are sequences */
  assert_pushes (L, "@ai [10, 20, 30]", "#v == 3 and v[1] == 10 and v[3] == 30");
  assert_pushes (L, "@ai []", "#v == 0 and next(v) == nil");
  assert_pushes (L, "@as ['a', 'b']", "#v == 2 and v[2] == 'b'");
  assert_pushes (L, "(1, 'two', 3.5)", "#v == 3 and v[1] == 1 and v[2] == 'two' and v[3] == 3.5");
  assert_pushes (L, "()", "#v == 0 and next(v) == nil");
  assert_pushes (L, "@a(si) [('a', 1), ('b', 2)]",
                 "#v == 2 and v[1][1] == 'a' and v[1][2] == 1 and v[2][1] == 'b' and v[2][2] == 2");
  assert_pushes (L, "((1, 2), 'x')", "v[1][1] == 1 and v[1][2] == 2 and v[2] == 'x'");
  /* ay is a raw byte string */
  assert_pushes (L, "@ay [0x61, 0x00, 0x62, 0xff]", "v == 'a\\0b\\xff' and #v == 4");
  assert_pushes (L, "@ay []", "v == ''");
  assert_pushes (L, "@aay [[0x61], [0x62, 0x63]]", "#v == 2 and v[1] == 'a' and v[2] == 'bc'");
}

typedef struct {
  GVariant *variant;
} DeepCtx;

static int
cf_push_deep (lua_State *L)
{
  DeepCtx *c = lua_touserdata (L, 1);

  shell_lua_push_variant (L, c->variant);
  return 1;
}

static GVariant *
nested_tuple (guint levels)
{
  GVariant *v = g_variant_new_int32 (1);
  guint i;

  for (i = 0; i < levels; i++)
    v = g_variant_new_tuple (&v, 1);
  return g_variant_ref_sink (v);
}

static void
test_push_nesting_cap (Fixture *f, gconstpointer data)
{
  lua_State *L = f->L;
  DeepCtx c;

  c.variant = nested_tuple (60);
  lua_pushcfunction (L, cf_push_deep);
  lua_pushlightuserdata (L, &c);
  g_assert_cmpint (lua_pcall (L, 1, 1, 0), ==, LUA_OK);
  g_assert_true (lua_istable (L, -1));
  lua_pop (L, 1);
  g_variant_unref (c.variant);

  c.variant = nested_tuple (70);
  lua_pushcfunction (L, cf_push_deep);
  lua_pushlightuserdata (L, &c);
  g_assert_cmpint (lua_pcall (L, 1, 1, 0), ==, LUA_ERRRUN);
  g_assert_cmpstr (lua_tostring (L, -1), ==, "variant is nested too deeply (limit 64)");
  lua_pop (L, 1);
  g_variant_unref (c.variant);

  /* nested v behaves the same */
  {
    GVariant *v = g_variant_new_int32 (1);
    guint i;

    for (i = 0; i < 70; i++)
      v = g_variant_new_variant (v);
    c.variant = g_variant_ref_sink (v);
    lua_pushcfunction (L, cf_push_deep);
    lua_pushlightuserdata (L, &c);
    g_assert_cmpint (lua_pcall (L, 1, 1, 0), ==, LUA_ERRRUN);
    g_assert_nonnull (strstr (lua_tostring (L, -1), "nested too deeply"));
    lua_pop (L, 1);
    g_variant_unref (c.variant);
  }
  g_assert_cmpint (lua_gettop (L), ==, 0);
}

/* ------------------------------------------------------------------------ */
/* Lua -> GVariant, inference                                                */
/* ------------------------------------------------------------------------ */

static void
test_infer_scalars (Fixture *f, gconstpointer data)
{
  ASSERT_CONVERTS ("true", NULL, "b", "true");
  ASSERT_CONVERTS ("false", NULL, "b", "false");
  ASSERT_CONVERTS ("5", NULL, "x", "int64 5");
  ASSERT_CONVERTS ("-5", NULL, "x", "int64 -5");
  ASSERT_CONVERTS ("math.maxinteger", NULL, "x", "int64 9223372036854775807");
  ASSERT_CONVERTS ("1.5", NULL, "d", "1.5");
  ASSERT_CONVERTS ("2.0", NULL, "d", "2.0");     /* a float stays a double */
  ASSERT_CONVERTS ("'hi'", NULL, "s", "'hi'");
  ASSERT_CONVERTS ("''", NULL, "s", "''");
  /* the same through the explicit "any" type */
  ASSERT_CONVERTS ("5", "*", "x", "int64 5");
}

static void
test_infer_tables (Fixture *f, gconstpointer data)
{
  /* empty table -> a{sv} */
  ASSERT_CONVERTS ("{}", NULL, "a{sv}", "@a{sv} {}");
  /* sequence -> av */
  ASSERT_CONVERTS ("{1, 2, 3}", NULL, "av", "[<int64 1>, <int64 2>, <int64 3>]");
  ASSERT_CONVERTS ("{1, 'a', true, 2.5}", NULL, "av", "[<int64 1>, <'a'>, <true>, <2.5>]");
  ASSERT_CONVERTS ("{{1, 2}, {3}}", NULL, "av",
                   "[<[<int64 1>, <int64 2>]>, <[<int64 3>]>]");
  /* string keys -> a{sv}, sorted by key */
  ASSERT_CONVERTS ("{b = 'x', a = 1}", NULL, "a{sv}", "{'a': <int64 1>, 'b': <'x'>}");
  ASSERT_CONVERTS ("{a = {b = {c = true}}}", NULL, "a{sv}", "{'a': <{'b': <{'c': <true>}>}>}");
  ASSERT_CONVERTS ("{list = {1, 2}, empty = {}}", NULL, "a{sv}",
                   "{'empty': <@a{sv} {}>, 'list': <[<int64 1>, <int64 2>]>}");
  /* a table with string keys and integer keys is a dictionary: the
   * integer keys are not representable and are ignored */
  ASSERT_CONVERTS ("{1, 2, x = 3}", NULL, "a{sv}", "{'x': <int64 3>}");
  /* nil values are absent keys */
  ASSERT_CONVERTS ("{a = 1, b = nil}", NULL, "a{sv}", "{'a': <int64 1>}");
  /* values may be given as a variant explicitly */
  ASSERT_CONVERTS ("{5, 'x'}", "av", "av", "[<int64 5>, <'x'>]");
}

static void
test_infer_errors (Fixture *f, gconstpointer data)
{
  /* nil is ambiguous */
  ASSERT_ERROR ("nil", NULL,
                "cannot infer a variant type from nil for field value "
                "(ambiguous; give a type or omit the value)");
  ASSERT_ERROR_SUB ("{1, nil, 3}", NULL, "sparse array");
  ASSERT_ERROR ("{[1] = 1, [3] = 3}", NULL,
                "field value is a sparse array (holes in 1..3); cannot convert");
  ASSERT_ERROR ("{[2] = 'x'}", NULL,
                "field value is a sparse array (holes in 1..2); cannot convert");
  ASSERT_ERROR_SUB ("{true, a = {1, nil, 3}}", NULL, "field value.a is a sparse array");
  /* a hole inside a nested sequence names the path */
  ASSERT_ERROR ("{{1, {[2] = 1}}}", NULL,
                "field value[1][2] is a sparse array (holes in 1..2); cannot convert");
  ASSERT_ERROR_SUB ("{[true] = 1}", NULL, "unsupported table key of type boolean");
  ASSERT_ERROR_SUB ("{[1.5] = 1}", NULL, "unsupported table key of type number");
  ASSERT_ERROR ("print", NULL, "cannot convert Lua function for field value to a variant");
  ASSERT_ERROR ("{f = print}", NULL, "cannot convert Lua function for field value.f to a variant");
  ASSERT_ERROR ("io.stdout", NULL, "cannot convert Lua userdata for field value to a variant");
  ASSERT_ERROR ("coroutine.create(print)", NULL, "cannot convert Lua thread for field value to a variant");
  ASSERT_ERROR_SUB ("'\\xff\\xfe'", NULL, "not valid UTF-8");
  ASSERT_ERROR_SUB ("'a\\0b'", NULL, "embedded NUL");
  ASSERT_ERROR_SUB ("{['a\\0b'] = 1}", NULL, "embedded NUL");
}

static void
assert_ok (lua_State *L, const char *expr, const char *type)
{
  g_autofree char *err = NULL;
  GVariant *v = eval_variant (L, expr, type, &err);

  if (v == NULL)
    g_error ("converting %s failed: %s", expr, err);
  g_variant_unref (v);
}

static void
test_nesting_cap (Fixture *f, gconstpointer data)
{
  /* self-referencing tables end in an error instead of a crash */
  ASSERT_ERROR_SUB ("(function() local t = {} t.a = t return t end)()", NULL,
                    "nested too deeply (limit 64)");
  ASSERT_ERROR_SUB ("(function() local t = {} t[1] = t return t end)()", NULL,
                    "nested too deeply (limit 64)");

  /* the cap is 64 levels, for sequences, dictionaries and typed tuples */
  assert_ok (f->L, "(function() local t = {} for i = 1, 60 do t = {t} end return t end)()", NULL);
  assert_ok (f->L, "(function() local t = {} for i = 1, 60 do t = {k = t} end return t end)()", NULL);
  ASSERT_ERROR_SUB ("(function() local t = {} for i = 1, 70 do t = {t} end return t end)()", NULL,
                    "nested too deeply (limit 64)");
  ASSERT_ERROR_SUB ("(function() local t = {} for i = 1, 70 do t = {k = t} end return t end)()", NULL,
                    "nested too deeply (limit 64)");
  ASSERT_ERROR_SUB ("(function() local t = {} for i = 1, 70 do t = {t} end return t end)()", "av",
                    "nested too deeply (limit 64)");
  ASSERT_ERROR_SUB ("(function() local t = {} for i = 1, 70 do t = {t} end return t end)()", "v",
                    "nested too deeply (limit 64)");
}

/* ------------------------------------------------------------------------ */
/* Lua -> GVariant, typed                                                    */
/* ------------------------------------------------------------------------ */

static void
test_typed_integers (Fixture *f, gconstpointer data)
{
  static const struct {
    const char *type;
    const char *min;
    const char *max;
    const char *below;   /* NULL: nothing below the minimum is an integer */
    const char *above;   /* NULL: likewise above the maximum */
  } widths[] = {
    { "y", "0",                      "255",                  "-1",                   "256" },
    { "n", "-32768",                 "32767",                "-32769",               "32768" },
    { "q", "0",                      "65535",                "-1",                   "65536" },
    { "i", "-2147483648",            "2147483647",           "-2147483649",          "2147483648" },
    { "h", "-2147483648",            "2147483647",           "-2147483649",          "2147483648" },
    { "u", "0",                      "4294967295",           "-1",                   "4294967296" },
    { "x", "math.mininteger",        "math.maxinteger",      NULL,                   NULL },
    { "t", "0",                      "math.maxinteger",      "-1",                   NULL },
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (widths); i++)
    {
      g_autofree char *err = NULL;
      GVariant *lo = eval_variant (f->L, widths[i].min, widths[i].type, &err);
      GVariant *hi = eval_variant (f->L, widths[i].max, widths[i].type, &err);

      /* the limits themselves convert */
      if (lo == NULL || hi == NULL)
        g_error ("type %s limits failed: %s", widths[i].type, err);
      g_assert_cmpstr (g_variant_get_type_string (lo), ==, widths[i].type);
      g_assert_cmpstr (g_variant_get_type_string (hi), ==, widths[i].type);
      g_variant_unref (lo);
      g_variant_unref (hi);

      /* one beyond them does not */
      if (widths[i].below != NULL)
        {
          g_autofree char *message = g_strdup_printf ("integer %s out of range for field value (type '%s')",
                                                      widths[i].below, widths[i].type);

          ASSERT_ERROR (widths[i].below, widths[i].type, message);
        }
      if (widths[i].above != NULL)
        {
          g_autofree char *message = g_strdup_printf ("integer %s out of range for field value (type '%s')",
                                                      widths[i].above, widths[i].type);

          ASSERT_ERROR (widths[i].above, widths[i].type, message);
        }
    }

  /* integral floats are accepted, others are not */
  ASSERT_CONVERTS ("3.0", "i", "i", "3");
  ASSERT_CONVERTS ("-4.0", "n", "n", "int16 -4");
  ASSERT_ERROR ("1.5", "i", "expected integer for field value, got non-integral number 1.500000");
  ASSERT_ERROR_SUB ("0/0", "i", "non-integral number");
  ASSERT_ERROR_SUB ("math.huge", "i", "non-integral number");

  /* above int64 */
  ASSERT_ERROR ("2^63", "x", "integer 9223372036854775808 out of range for field value (type 'x')");
  ASSERT_ERROR ("2^63", "i", "integer 9223372036854775808 out of range for field value (type 'i')");
  ASSERT_CONVERTS ("2^63", "t", "t", "uint64 9223372036854775808");
  ASSERT_CONVERTS ("2^64 - 2048", "t", "t", "uint64 18446744073709549568");
  ASSERT_ERROR_SUB ("2^64", "t", "for field value");
  ASSERT_ERROR ("-1", "t", "integer -1 out of range for field value (type 't')");

  /* wrong kinds of value name the field and the expected type */
  ASSERT_ERROR ("'5'", "i", "expected integer for field value, got string (variant type 'i')");
  ASSERT_ERROR ("true", "y", "expected integer for field value, got boolean (variant type 'y')");
  ASSERT_ERROR ("nil", "u", "expected integer for field value, got nil (variant type 'u')");
  ASSERT_ERROR ("{}", "x", "expected integer for field value, got table (variant type 'x')");
}

static void
test_typed_scalars (Fixture *f, gconstpointer data)
{
  ASSERT_CONVERTS ("true", "b", "b", "true");
  ASSERT_ERROR ("1", "b", "expected boolean for field value, got number (variant type 'b')");
  ASSERT_ERROR ("nil", "b", "expected boolean for field value, got nil (variant type 'b')");

  ASSERT_CONVERTS ("3", "d", "d", "3.0");
  ASSERT_CONVERTS ("-0.25", "d", "d", "-0.25");
  ASSERT_ERROR ("'1'", "d", "expected number for field value, got string (variant type 'd')");

  ASSERT_CONVERTS ("'x'", "s", "s", "'x'");
  ASSERT_ERROR ("5", "s", "expected string for field value, got number (variant type 's')");
  ASSERT_ERROR ("{}", "s", "expected string for field value, got table (variant type 's')");

  /* object paths and signatures */
  ASSERT_CONVERTS ("'/org/gnome/Shell'", "o", "o", "objectpath '/org/gnome/Shell'");
  ASSERT_CONVERTS ("'/'", "o", "o", "objectpath '/'");
  ASSERT_ERROR ("'foo'", "o", "string 'foo' for field value is not a valid D-Bus object path");
  ASSERT_ERROR ("'/a/'", "o", "string '/a/' for field value is not a valid D-Bus object path");
  ASSERT_ERROR ("''", "o", "string '' for field value is not a valid D-Bus object path");
  ASSERT_CONVERTS ("'a{sv}'", "g", "g", "signature 'a{sv}'");
  ASSERT_CONVERTS ("''", "g", "g", "signature ''");
  ASSERT_ERROR ("'a{'", "g", "string 'a{' for field value is not a valid D-Bus signature");
  ASSERT_ERROR ("5", "o", "expected string for field value, got number (variant type 'o')");

  /* variants and maybe */
  ASSERT_CONVERTS ("5", "v", "v", "<int64 5>");
  ASSERT_CONVERTS ("{a = 1}", "v", "v", "<{'a': <int64 1>}>");
  ASSERT_ERROR_SUB ("nil", "v", "cannot infer a variant type from nil");
  ASSERT_CONVERTS ("nil", "mi", "mi", "@mi nothing");
  ASSERT_CONVERTS ("5", "mi", "mi", "@mi 5");
  ASSERT_CONVERTS ("nil", "ms", "ms", "@ms nothing");
  ASSERT_CONVERTS ("'x'", "ms", "ms", "@ms 'x'");
  ASSERT_CONVERTS ("nil", "mas", "mas", "@mas nothing");
  ASSERT_CONVERTS ("{'a'}", "mas", "mas", "@mas ['a']");
  ASSERT_CONVERTS ("nil", "mmi", "mmi", "@mmi nothing");
  ASSERT_ERROR ("'x'", "mi", "expected integer for field value, got string (variant type 'i')");
  ASSERT_ERROR ("300", "my", "integer 300 out of range for field value (type 'y')");
}

static void
test_typed_strings_and_bytes (Fixture *f, gconstpointer data)
{
  g_autofree char *err = NULL;
  GVariant *v;
  gsize n = 0;
  gconstpointer bytes;

  /* ay from a Lua string keeps embedded zeros and arbitrary bytes */
  v = eval_variant (f->L, "'a\\0b\\xff'", "ay", &err);
  g_assert_nonnull (v);
  g_assert_cmpstr (g_variant_get_type_string (v), ==, "ay");
  bytes = g_variant_get_fixed_array (v, &n, 1);
  g_assert_cmpuint (n, ==, 4);
  g_assert_cmpint (memcmp (bytes, "a\0b\xff", 4), ==, 0);
  g_variant_unref (v);

  ASSERT_CONVERTS ("''", "ay", "ay", "@ay []");
  /* ay from a table of integers works too */
  ASSERT_CONVERTS ("{97, 98}", "ay", "ay", "[byte 0x61, 0x62]");
  ASSERT_ERROR_SUB ("{97, 256}", "ay", "out of range for field value[2] (type 'y')");
  /* a string with no restrictions on bytes is still a string for 's' */
  ASSERT_ERROR_SUB ("'a\\xff'", "s", "not valid UTF-8");
  ASSERT_ERROR_SUB ("'a\\0'", "s", "embedded NUL");
}

static void
test_typed_containers (Fixture *f, gconstpointer data)
{
  /* arrays */
  ASSERT_CONVERTS ("{'a', 'b'}", "as", "as", "['a', 'b']");
  ASSERT_CONVERTS ("{}", "as", "as", "@as []");
  ASSERT_CONVERTS ("{1, 2.0, 3}", "ai", "ai", "[1, 2, 3]");
  ASSERT_CONVERTS ("{{1, 2}, {}}", "aai", "aai", "[[1, 2], []]");
  ASSERT_ERROR ("{1}", "as", "expected string for field value[1], got number (variant type 's')");
  ASSERT_ERROR ("{'a', 2}", "as", "expected string for field value[2], got number (variant type 's')");
  ASSERT_ERROR_SUB ("{a = 1}", "ai", "table without elements 1..n");
  ASSERT_ERROR ("'x'", "ai", "expected table for field value, got string (variant type 'ai')");
  ASSERT_ERROR ("5", "as", "expected table for field value, got number (variant type 'as')");
  ASSERT_CONVERTS ("{'/a', '/b/c'}", "ao", "ao", "[objectpath '/a', '/b/c']");
  ASSERT_ERROR ("{'/a', 'b'}", "ao",
                "string 'b' for field value[2] is not a valid D-Bus object path");

  /* tuples */
  ASSERT_CONVERTS ("{'a', 1}", "(si)", "(si)", "('a', 1)");
  ASSERT_CONVERTS ("{{'a', 1}, {'b', 2}}", "a(si)", "a(si)", "[('a', 1), ('b', 2)]");
  ASSERT_CONVERTS ("{}", "()", "()", "()");
  ASSERT_CONVERTS ("{{1, 2}, 'x'}", "((ii)s)", "((ii)s)", "((1, 2), 'x')");
  ASSERT_ERROR ("{'a', 1, 2}", "(si)", "expected 2 elements for field value, got 3");
  ASSERT_ERROR ("{'a'}", "(si)", "expected integer for field value[2], got nil (variant type 'i')");
  ASSERT_ERROR ("{'a', 'b'}", "(si)", "expected integer for field value[2], got string (variant type 'i')");
  ASSERT_ERROR ("'ab'", "(ss)", "expected table for field value, got string (variant type '(ss)')");
  ASSERT_ERROR ("{{'a', 1}, {'b', 'c'}}", "a(si)",
                "expected integer for field value[2][2], got string (variant type 'i')");
  /* nil is a legal member of maybe-typed tuples and arrays (including holes) */
  ASSERT_CONVERTS ("{1}", "(imims)", "(imims)", "(1, @mi nothing, @ms nothing)");
  ASSERT_ERROR ("{1}", "(iii)", "expected integer for field value[2], got nil (variant type 'i')");
}

static void
test_typed_dicts (Fixture *f, gconstpointer data)
{
  ASSERT_CONVERTS ("{a = 1, b = 2}", "a{sx}", "a{sx}", "{'a': int64 1, 'b': 2}");
  ASSERT_CONVERTS ("{}", "a{ss}", "a{ss}", "@a{ss} {}");
  ASSERT_CONVERTS ("{k = 'v'}", "a{ss}", "a{ss}", "{'k': 'v'}");
  ASSERT_CONVERTS ("{x = 1, y = 's', z = {1, 2}}", "a{sv}", "a{sv}",
                   "{'x': <int64 1>, 'y': <'s'>, 'z': <[<int64 1>, <int64 2>]>}");
  ASSERT_CONVERTS ("{['/a'] = 1}", "a{oi}", "a{oi}", "{objectpath '/a': 1}");
  ASSERT_CONVERTS ("{{1, 2}, {3, 4}}", "a{ii}", "a{ii}", "{1: 2, 3: 4}");
  ASSERT_CONVERTS ("{a = {b = 1}}", "a{sa{si}}", "a{sa{si}}", "{'a': {'b': 1}}");

  ASSERT_ERROR ("{a = 1}", "a{ss}",
                "expected string for field value.a, got number (variant type 's')");
  ASSERT_ERROR ("{a = {b = 'x'}}", "a{sa{si}}",
                "expected integer for field value.a.b, got string (variant type 'i')");
  ASSERT_ERROR ("{a = 300}", "a{sy}", "integer 300 out of range for field value.a (type 'y')");
  ASSERT_ERROR ("{foo = 1}", "a{oi}",
                "string 'foo' for field value.foo is not a valid D-Bus object path");
  ASSERT_ERROR_SUB ("{x = {1, nil, 3}}", "a{sv}", "field value.x is a sparse array");
  ASSERT_ERROR ("5", "a{ss}", "expected table for field value, got number (variant type 'a{ss}')");
}

static void
test_typed_maybe_holes (Fixture *f, gconstpointer data)
{
  ASSERT_CONVERTS ("{nil, 'a'}", "ams", "ams", "[@ms nothing, 'a']");
  ASSERT_CONVERTS ("{'a', nil, 'b'}", "ams", "ams", "[@ms 'a', nothing, 'b']");
  ASSERT_CONVERTS ("{}", "ams", "ams", "@ams []");
  ASSERT_CONVERTS ("{}", "(mi)", "(mi)", "(@mi nothing,)");
}

/* ------------------------------------------------------------------------ */
/* Round trips                                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
  GVariant *in;
  GVariant *out;
} RoundTrip;

static int
cf_roundtrip (lua_State *L)
{
  RoundTrip *rt = lua_touserdata (L, 1);

  shell_lua_push_variant (L, rt->in);
  rt->out = shell_lua_to_variant (L, -1, g_variant_get_type (rt->in));
  return 0;
}

static void
assert_roundtrip (lua_State *L, const char *text)
{
  g_autoptr (GError) error = NULL;
  RoundTrip rt = { NULL, NULL };

  rt.in = g_variant_parse (NULL, text, NULL, NULL, &error);
  if (rt.in == NULL)
    g_error ("cannot parse '%s': %s", text, error->message);

  lua_pushcfunction (L, cf_roundtrip);
  lua_pushlightuserdata (L, &rt);
  if (lua_pcall (L, 1, 0, 0) != LUA_OK)
    g_error ("round trip of %s failed: %s", text, lua_tostring (L, -1));
  g_assert_cmpint (lua_gettop (L), ==, 0);

  if (!g_variant_equal (rt.in, rt.out))
    {
      g_autofree char *a = g_variant_print (rt.in, TRUE);
      g_autofree char *b = g_variant_print (rt.out, TRUE);

      g_error ("round trip changed %s into %s", a, b);
    }
  g_variant_unref (rt.in);
  g_variant_unref (rt.out);
}

static void
test_roundtrip (Fixture *f, gconstpointer data)
{
  static const char *const values[] = {
    /* scalars */
    "true", "false", "byte 0", "byte 255", "int16 -32768", "int16 32767",
    "uint16 0", "uint16 65535", "int32 -2147483648", "int32 2147483647",
    "uint32 0", "uint32 4294967295", "int64 -9223372036854775808",
    "int64 9223372036854775807", "uint64 0", "uint64 9223372036854775807",
    "uint64 9223372036854775808", "handle 0", "handle 2147483647", "handle -1",
    "0.0", "1.5", "-1e300", "1e-300",
    "''", "'plain'", "'café 世界'", "objectpath '/'", "objectpath '/org/gnome/Shell'",
    "signature ''", "signature 'a{sv}'", "signature '(ii)'",
    /* maybe */
    "@mi nothing", "@mi 3", "@ms nothing", "@ms 'x'", "@mas nothing", "@mas ['a', 'b']",
    "@ami [nothing, 1, nothing, 2]", "@ams [nothing, 'a']", "(@mi nothing,)", "(@mi 1, @ms nothing)",
    /* arrays */
    "@ai []", "@ai [1]", "@ai [1, 2, 3]", "@as []", "@as ['a', 'b']", "@ab [true, false]",
    "@ad [0.5, 1.5]", "@aax [[1, 2], []]", "@ao ['/a', '/b/c']", "@ag ['i', 'a{sv}']",
    "@an [-1, 1]", "@aq [1, 65535]", "@au [4294967295]", "@ax [-9223372036854775808]",
    "@at [0, 9223372036854775807]", "@ah [0, 5]", "@ay [0x00, 0x01, 0xff, 0x00]", "@ay []",
    "@aay [[0x61], [], [0x62, 0x00]]",
    /* tuples */
    "()", "(1, 'x', true)", "((1, 2), 'x')", "((1, (2, 3)), (@as ['a'], 4.5))",
    "@a(si) [('a', 1), ('b', 2)]", "@a(si) []", "@a(sas) [('a', ['x', 'y'])]",
    /* dictionaries */
    "@a{sv} {}",
    "@a{sv} {'a': <int64 1>, 'b': <'x'>, 'c': <[<int64 1>, <'s'>]>, 'd': <{'e': <true>}>, 'f': <2.5>}",
    "@a{ss} {'k': 'v', 'k2': 'v2'}", "@a{sx} {'a': 1, 'b': -2}", "@a{oi} {'/a': 1, '/b': 2}",
    "@a{sa{ss}} {'o': {'i': 'j'}, 'p': @a{ss} {}}", "@a{ii} {1: 2, 3: 4}", "@a{sas} {'k': ['a', 'b']}",
    "@a{sai} {'k': []}", "@a{s(ii)} {'k': (1, 2)}",
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (values); i++)
    assert_roundtrip (f->L, values[i]);
}

/* ------------------------------------------------------------------------ */
/* Errors are catchable Lua errors                                           */
/* ------------------------------------------------------------------------ */

static int
cf_to_variant (lua_State *L)
{
  const GVariantType *type = lua_isnone (L, 2) ? NULL : G_VARIANT_TYPE (luaL_checkstring (L, 2));
  GVariant *v = shell_lua_to_variant (L, 1, type);
  char *text = g_variant_print (v, FALSE);

  lua_pushstring (L, g_variant_get_type_string (v));
  lua_pushstring (L, text);
  g_free (text);
  g_variant_unref (v);
  return 2;
}

static void
test_errors_reach_lua (Fixture *f, gconstpointer data)
{
  lua_State *L = f->L;

  lua_pushcfunction (L, cf_to_variant);
  lua_setglobal (L, "to_variant");

  /* the documented wording, delivered as an ordinary error that pcall catches */
  run_lua (L,
           "local function err(...) local ok, e = pcall(to_variant, ...) assert(not ok, 'no error') return e end\n"
           "assert(err(300, 'y') == \"integer 300 out of range for field value (type 'y')\", err(300, 'y'))\n"
           "assert(err(1.5, 'i') == 'expected integer for field value, got non-integral number 1.500000')\n"
           "assert(err('foo', 'o') == \"string 'foo' for field value is not a valid D-Bus object path\")\n"
           "assert(err(nil) == 'cannot infer a variant type from nil for field value (ambiguous; give a type or omit the value)')\n"
           "assert(err({1, nil, 3}):find('sparse array', 1, true))\n"
           "assert(err('x', 'b') == \"expected boolean for field value, got string (variant type 'b')\")\n"
           "assert(err({x = 'a'}, 'a{sx}') == \"expected integer for field value.x, got string (variant type 'x')\")\n"
           "local t = {} t.self = t\n"
           "assert(err(t):find('nested too deeply', 1, true))\n"
           /* a successful call right after the failures is unaffected */
           "local ty, text = to_variant({a = 1})\n"
           "assert(ty == 'a{sv}' and text == \"{'a': <int64 1>}\", text)\n");
  g_assert_cmpint (lua_gettop (L), ==, 0);

  /* uncaught: the error object is the plain message string */
  lua_pushcfunction (L, cf_to_variant);
  lua_pushinteger (L, 256);
  lua_pushstring (L, "y");
  g_assert_cmpint (lua_pcall (L, 2, 2, 0), ==, LUA_ERRRUN);
  g_assert_cmpstr (lua_tostring (L, -1), ==, "integer 256 out of range for field value (type 'y')");
  lua_pop (L, 1);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

#define ADD(path, func) \
  g_test_add (path, Fixture, NULL, fixture_setup, func, fixture_teardown)

  ADD ("/shell-lua/variant/push/basic", test_push_basic);
  ADD ("/shell-lua/variant/push/variants-and-maybe", test_push_variants_and_maybe);
  ADD ("/shell-lua/variant/push/containers", test_push_containers);
  ADD ("/shell-lua/variant/push/nesting-cap", test_push_nesting_cap);
  ADD ("/shell-lua/variant/infer/scalars", test_infer_scalars);
  ADD ("/shell-lua/variant/infer/tables", test_infer_tables);
  ADD ("/shell-lua/variant/infer/errors", test_infer_errors);
  ADD ("/shell-lua/variant/infer/nesting-cap", test_nesting_cap);
  ADD ("/shell-lua/variant/typed/integers", test_typed_integers);
  ADD ("/shell-lua/variant/typed/scalars", test_typed_scalars);
  ADD ("/shell-lua/variant/typed/strings-and-bytes", test_typed_strings_and_bytes);
  ADD ("/shell-lua/variant/typed/containers", test_typed_containers);
  ADD ("/shell-lua/variant/typed/dicts", test_typed_dicts);
  ADD ("/shell-lua/variant/typed/maybe-holes", test_typed_maybe_holes);
  ADD ("/shell-lua/variant/roundtrip", test_roundtrip);
  ADD ("/shell-lua/variant/errors-reach-lua", test_errors_reach_lua);

  return g_test_run ();
}
