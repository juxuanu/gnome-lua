/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Handles: the Lua view of GObjects (shell-lua-handle.h).
 */

#include "test-utils.h"

#include "shell-lua-handle.h"

/* ------------------------------------------------------------------------ */
/* Test objects                                                              */
/* ------------------------------------------------------------------------ */

typedef enum {
  TEST_MODE_SLOW,
  TEST_MODE_FAST,
} TestMode;

typedef enum {
  TEST_FEATURE_NONE = 0,
  TEST_FEATURE_A = 1 << 0,
  TEST_FEATURE_B = 1 << 1,
} TestFeatures;

GType test_mode_get_type (void) G_GNUC_CONST;
GType test_features_get_type (void) G_GNUC_CONST;
#define TEST_TYPE_MODE (test_mode_get_type ())
#define TEST_TYPE_FEATURES (test_features_get_type ())

G_DEFINE_ENUM_TYPE (TestMode, test_mode,
                    G_DEFINE_ENUM_VALUE (TEST_MODE_SLOW, "slow"),
                    G_DEFINE_ENUM_VALUE (TEST_MODE_FAST, "fast"))
G_DEFINE_FLAGS_TYPE (TestFeatures, test_features,
                     G_DEFINE_ENUM_VALUE (TEST_FEATURE_A, "a"),
                     G_DEFINE_ENUM_VALUE (TEST_FEATURE_B, "b"))

typedef struct _TestThing TestThing;
typedef struct { GObjectClass parent_class; } TestThingClass;

struct _TestThing {
  GObject       parent_instance;
  int           count;
  char         *label;
  TestMode      mode;
  char        **tags;
  TestFeatures  features;
  double        ratio;
  TestThing    *friend;
  char         *secret;
  char         *token;
  gboolean      flag;
};

GType test_thing_get_type (void) G_GNUC_CONST;
#define TEST_TYPE_THING (test_thing_get_type ())
G_DEFINE_TYPE (TestThing, test_thing, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_COUNT,
  PROP_LABEL,
  PROP_MODE,
  PROP_TAGS,
  PROP_FEATURES,
  PROP_RATIO,
  PROP_SERIAL,
  PROP_FRIEND,
  PROP_SECRET,
  PROP_TOKEN,
  PROP_FLAG,
  N_PROPS
};

static void
test_thing_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  TestThing *self = (TestThing *) object;

  switch (id)
    {
    case PROP_COUNT:
      self->count = g_value_get_int (value);
      break;
    case PROP_LABEL:
      g_free (self->label);
      self->label = g_value_dup_string (value);
      break;
    case PROP_MODE:
      self->mode = g_value_get_enum (value);
      break;
    case PROP_TAGS:
      g_strfreev (self->tags);
      self->tags = g_value_dup_boxed (value);
      break;
    case PROP_FEATURES:
      self->features = g_value_get_flags (value);
      break;
    case PROP_RATIO:
      self->ratio = g_value_get_double (value);
      break;
    case PROP_FRIEND:
      g_clear_object (&self->friend);
      self->friend = g_value_dup_object (value);
      break;
    case PROP_SECRET:
      g_free (self->secret);
      self->secret = g_value_dup_string (value);
      break;
    case PROP_TOKEN:
      g_free (self->token);
      self->token = g_value_dup_string (value);
      break;
    case PROP_FLAG:
      self->flag = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
test_thing_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  TestThing *self = (TestThing *) object;

  switch (id)
    {
    case PROP_COUNT:
      g_value_set_int (value, self->count);
      break;
    case PROP_LABEL:
      g_value_set_string (value, self->label);
      break;
    case PROP_MODE:
      g_value_set_enum (value, self->mode);
      break;
    case PROP_TAGS:
      g_value_set_boxed (value, self->tags);
      break;
    case PROP_FEATURES:
      g_value_set_flags (value, self->features);
      break;
    case PROP_RATIO:
      g_value_set_double (value, self->ratio);
      break;
    case PROP_SERIAL:
      g_value_set_uint (value, 42);
      break;
    case PROP_FRIEND:
      g_value_set_object (value, self->friend);
      break;
    case PROP_SECRET:
      g_value_set_string (value, self->secret);
      break;
    case PROP_TOKEN:
      g_value_set_string (value, self->token);
      break;
    case PROP_FLAG:
      g_value_set_boolean (value, self->flag);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
test_thing_finalize (GObject *object)
{
  TestThing *self = (TestThing *) object;

  g_free (self->label);
  g_strfreev (self->tags);
  g_clear_object (&self->friend);
  g_free (self->secret);
  g_free (self->token);
  G_OBJECT_CLASS (test_thing_parent_class)->finalize (object);
}

static void
test_thing_class_init (TestThingClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamFlags rw = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS;

  object_class->set_property = test_thing_set_property;
  object_class->get_property = test_thing_get_property;
  object_class->finalize = test_thing_finalize;

  g_object_class_install_property (object_class, PROP_COUNT,
    g_param_spec_int ("count", NULL, NULL, 0, 100, 0, rw));
  g_object_class_install_property (object_class, PROP_LABEL,
    g_param_spec_string ("label", NULL, NULL, NULL, rw));
  g_object_class_install_property (object_class, PROP_MODE,
    g_param_spec_enum ("mode", NULL, NULL, TEST_TYPE_MODE, TEST_MODE_SLOW, rw));
  g_object_class_install_property (object_class, PROP_TAGS,
    g_param_spec_boxed ("tags", NULL, NULL, G_TYPE_STRV, rw));
  g_object_class_install_property (object_class, PROP_FEATURES,
    g_param_spec_flags ("features", NULL, NULL, TEST_TYPE_FEATURES, TEST_FEATURE_NONE, rw));
  g_object_class_install_property (object_class, PROP_RATIO,
    g_param_spec_double ("ratio", NULL, NULL, -1000, 1000, 0, rw));
  g_object_class_install_property (object_class, PROP_SERIAL,
    g_param_spec_uint ("serial", NULL, NULL, 0, G_MAXUINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_FRIEND,
    g_param_spec_object ("friend", NULL, NULL, TEST_TYPE_THING, rw));
  g_object_class_install_property (object_class, PROP_SECRET,
    g_param_spec_string ("secret", NULL, NULL, "hidden", rw));
  g_object_class_install_property (object_class, PROP_TOKEN,
    g_param_spec_string ("token", NULL, NULL, "tok", G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_FLAG,
    g_param_spec_boolean ("flag", NULL, NULL, FALSE, rw));
}

static void
test_thing_init (TestThing *self)
{
}

/* subtype with one more property, and an unregistered grandchild */
typedef struct {
  TestThing parent_instance;
  int       depth;
} TestSubThing;
typedef struct { TestThingClass parent_class; } TestSubThingClass;
GType test_sub_thing_get_type (void) G_GNUC_CONST;
#define TEST_TYPE_SUB_THING (test_sub_thing_get_type ())
G_DEFINE_TYPE (TestSubThing, test_sub_thing, TEST_TYPE_THING)

static void
test_sub_thing_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  ((TestSubThing *) object)->depth = g_value_get_int (value);
}

static void
test_sub_thing_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  g_value_set_int (value, ((TestSubThing *) object)->depth);
}

static void
test_sub_thing_class_init (TestSubThingClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = test_sub_thing_set_property;
  object_class->get_property = test_sub_thing_get_property;
  g_object_class_install_property (object_class, 1,
    g_param_spec_int ("depth", NULL, NULL, 0, 10, 3, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
test_sub_thing_init (TestSubThing *self)
{
  self->depth = 3;
}

typedef struct { TestSubThing parent_instance; } TestSubSubThing;
typedef struct { TestSubThingClass parent_class; } TestSubSubThingClass;
GType test_sub_sub_thing_get_type (void) G_GNUC_CONST;
#define TEST_TYPE_SUB_SUB_THING (test_sub_sub_thing_get_type ())
G_DEFINE_TYPE (TestSubSubThing, test_sub_sub_thing, TEST_TYPE_SUB_THING)
static void test_sub_sub_thing_class_init (TestSubSubThingClass *klass) { }
static void test_sub_sub_thing_init (TestSubSubThing *self) { }

typedef struct { GObject parent_instance; } TestOther;
typedef struct { GObjectClass parent_class; } TestOtherClass;
GType test_other_get_type (void) G_GNUC_CONST;
#define TEST_TYPE_OTHER (test_other_get_type ())
G_DEFINE_TYPE (TestOther, test_other, G_TYPE_OBJECT)
static void test_other_class_init (TestOtherClass *klass) { }
static void test_other_init (TestOther *self) { }

/* ------------------------------------------------------------------------ */
/* Handle types                                                              */
/* ------------------------------------------------------------------------ */

static int
l_thing_add (lua_State *L)
{
  TestThing *thing = (TestThing *) shell_lua_handle_check (L, 1, "test.Thing");
  lua_Integer n = luaL_checkinteger (L, 2);

  thing->count += (int) n;
  lua_pushinteger (L, thing->count);
  return 1;
}

static int
l_thing_describe (lua_State *L)
{
  TestThing *thing = (TestThing *) shell_lua_handle_check (L, 1, "test.Thing");

  lua_pushfstring (L, "Thing(%s=%d)", thing->label != NULL ? thing->label : "?", thing->count);
  return 1;
}

static int
l_sub_extra (lua_State *L)
{
  TestSubThing *sub = (TestSubThing *) shell_lua_handle_check (L, 1, "test.SubThing");

  lua_pushfstring (L, "extra:%d", sub->depth);
  return 1;
}

static int
l_other_ping (lua_State *L)
{
  shell_lua_handle_check (L, 1, "test.Other");
  lua_pushliteral (L, "pong");
  return 1;
}

static const luaL_Reg thing_methods[] = {
  { "add", l_thing_add },
  { "describe", l_thing_describe },
  { NULL, NULL }
};
static const luaL_Reg sub_methods[] = { { "extra", l_sub_extra }, { NULL, NULL } };
static const luaL_Reg other_methods[] = { { "ping", l_other_ping }, { NULL, NULL } };

static const ShellLuaHandleProperty thing_properties[] = {
  { "count",    "count",   TRUE, TRUE },
  { "label",    NULL,      TRUE, TRUE },
  { "mode",     NULL,      TRUE, TRUE },
  { "tags",     NULL,      TRUE, TRUE },
  { "features", NULL,      TRUE, TRUE },
  { "ratio",    NULL,      TRUE, TRUE },
  { "id",       "serial",  TRUE, FALSE },
  { "friend",   NULL,      TRUE, TRUE },
  { "token",    NULL,      TRUE, TRUE },
  { "enabled",  "flag",    TRUE, TRUE },
  { "ghost",    "no-such", TRUE, TRUE },
  { NULL }
};
static const ShellLuaHandleProperty sub_properties[] = {
  { "depth", NULL, TRUE, TRUE },
  { NULL }
};
static const ShellLuaHandleProperty no_properties[] = { { NULL } };

static ShellLuaHandleType thing_type = {
  .name = "test.Thing", .methods = thing_methods, .properties = thing_properties,
};
static ShellLuaHandleType sub_type = {
  .name = "test.SubThing", .methods = sub_methods, .properties = sub_properties,
  .parent = "test.Thing",
};
static ShellLuaHandleType other_type = {
  .name = "test.Other", .methods = other_methods, .properties = no_properties,
};

/* Lua-callable probes of the C helpers */
static int
l_thing_count (lua_State *L)
{
  TestThing *thing = (TestThing *) shell_lua_handle_check (L, 1, "test.Thing");

  lua_pushinteger (L, thing->count);
  return 1;
}

static int
l_peek_is_thing (lua_State *L)
{
  lua_pushboolean (L, shell_lua_handle_peek (L, 1, "test.Thing") != NULL);
  return 1;
}

static int
l_peek_is_any (lua_State *L)
{
  lua_pushboolean (L, shell_lua_handle_peek (L, 1, NULL) != NULL);
  return 1;
}

static int
l_check_any (lua_State *L)
{
  lua_pushboolean (L, shell_lua_handle_check (L, 1, NULL) != NULL);
  return 1;
}

static int
l_is_alive (lua_State *L)
{
  lua_pushboolean (L, shell_lua_handle_is_alive (L, 1));
  return 1;
}

static ShellLuaState *
new_state (ShellLuaStateKind kind)
{
  ShellLuaState *s = tu_state_new (kind == SHELL_LUA_STATE_SANDBOXED ? "sbx" : "priv", kind);
  lua_State *L = shell_lua_state_get_L (s);

  thing_type.gtype = TEST_TYPE_THING;
  sub_type.gtype = TEST_TYPE_SUB_THING;
  other_type.gtype = TEST_TYPE_OTHER;
  shell_lua_handle_register_type (L, &thing_type);
  shell_lua_handle_register_type (L, &sub_type);
  shell_lua_handle_register_type (L, &other_type);
  /* registering twice is harmless */
  shell_lua_handle_register_type (L, &thing_type);

  tu_set_cfunction (s, "thing_count", l_thing_count);
  tu_set_cfunction (s, "peek_is_thing", l_peek_is_thing);
  tu_set_cfunction (s, "peek_is_any", l_peek_is_any);
  tu_set_cfunction (s, "check_any", l_check_any);
  tu_set_cfunction (s, "is_alive", l_is_alive);
  return s;
}

typedef struct {
  const char *global;
  const char *type_name;
  GObject    *object;
} PushCtx;

static int
cf_push (lua_State *L)
{
  PushCtx *c = lua_touserdata (L, 1);

  shell_lua_handle_push (L, c->type_name, c->object);
  lua_setglobal (L, c->global);
  return 0;
}

/* Returns the error message if pushing failed. */
static char *
try_push (ShellLuaState *s, const char *global, const char *type_name, gpointer object)
{
  lua_State *L = shell_lua_state_get_L (s);
  PushCtx c = { global, type_name, object };
  char *message = NULL;

  lua_pushcfunction (L, cf_push);
  lua_pushlightuserdata (L, &c);
  if (lua_pcall (L, 1, 0, 0) != LUA_OK)
    {
      message = g_strdup (lua_tostring (L, -1));
      lua_pop (L, 1);
    }
  return message;
}

static void
push (ShellLuaState *s, const char *global, const char *type_name, gpointer object)
{
  g_autofree char *err = try_push (s, global, type_name, object);

  if (err != NULL)
    g_error ("pushing %s failed: %s", global, err);
}

/* ------------------------------------------------------------------------ */
/* Tests                                                                     */
/* ------------------------------------------------------------------------ */

static void
test_identity (void)
{
  ShellLuaState *s = new_state (SHELL_LUA_STATE_PRIVILEGED);
  ShellLuaState *s2 = new_state (SHELL_LUA_STATE_PRIVILEGED);
  GObject *obj = g_object_new (TEST_TYPE_THING, NULL);
  GObject *other = g_object_new (TEST_TYPE_THING, NULL);

  push (s, "a", NULL, obj);
  push (s, "b", "test.Thing", obj);
  push (s, "c", NULL, other);
  push (s2, "a", NULL, obj);

  /* the same object is the same userdata, however it was pushed */
  tu_assert_eval_true (s, "rawequal(a, b) and a == b");
  tu_assert_eval_true (s, "not rawequal(a, c) and a ~= c");
  tu_assert_eval_true (s, "(function() local t = {} t[a] = 1 return t[b] == 1 and t[c] == nil end)()");
  tu_assert_eval_true (s, "type(a) == 'userdata'");
  /* pushing again later still finds the cached handle */
  push (s, "d", NULL, obj);
  tu_assert_eval_true (s, "rawequal(a, d)");
  /* each state has its own handle for the same object */
  tu_assert_eval_true (s2, "type(a) == 'userdata' and a.count == 0");

  g_object_unref (s);
  g_object_unref (s2);
  g_object_unref (obj);
  g_object_unref (other);
}

static void
test_methods (void)
{
  ShellLuaState *s = new_state (SHELL_LUA_STATE_PRIVILEGED);
  GObject *thing = g_object_new (TEST_TYPE_THING, "label", "L", NULL);
  GObject *other = g_object_new (TEST_TYPE_OTHER, NULL);

  push (s, "t", NULL, thing);
  push (s, "o", NULL, other);

  tu_assert_eval_str (s, "t:add(5)", "5");
  tu_assert_eval_str (s, "t:add(2)", "7");
  tu_assert_eval_str (s, "t.count", "7");
  tu_assert_eval_str (s, "t:describe()", "Thing(L=7)");
  tu_assert_eval_str (s, "o:ping()", "pong");
  tu_assert_eval_str (s, "type(t.add)", "function");

  /* methods check their receiver */
  tu_assert_fails (s, "t.add(5)", SHELL_LUA_ERROR_RUNTIME, "test.Thing expected, got number");
  tu_assert_fails (s, "t.add(o, 1)", SHELL_LUA_ERROR_RUNTIME, "test.Thing expected, got test.Other");
  tu_assert_fails (s, "o.ping(t)", SHELL_LUA_ERROR_RUNTIME, "test.Other expected, got test.Thing");
  tu_assert_fails (s, "t:add('x')", SHELL_LUA_ERROR_RUNTIME, "number expected, got string");
  /* the Lua side cannot swap methods in */
  tu_assert_fails (s, "t.add = print", SHELL_LUA_ERROR_RUNTIME, "no property 'add' on test.Thing");

  g_object_unref (s);
  g_object_unref (thing);
  g_object_unref (other);
}

static void
test_properties_get_set (void)
{
  ShellLuaState *s = new_state (SHELL_LUA_STATE_PRIVILEGED);
  TestThing *thing = g_object_new (TEST_TYPE_THING, "label", "first", NULL);

  push (s, "t", NULL, thing);

  tu_assert_eval_str (s, "t.count", "0");
  tu_do (s, "t.count = 7");
  g_assert_cmpint (thing->count, ==, 7);
  tu_assert_eval_str (s, "t.count", "7");
  tu_do (s, "t.count = 8.0");     /* integral floats are fine */
  g_assert_cmpint (thing->count, ==, 8);

  tu_assert_eval_str (s, "t.label", "first");
  tu_do (s, "t.label = 'second'");
  g_assert_cmpstr (thing->label, ==, "second");
  tu_do (s, "t.label = nil");
  g_assert_null (thing->label);
  tu_assert_eval_str (s, "t.label", "nil");

  tu_do (s, "t.ratio = 2.5");
  g_assert_cmpfloat (thing->ratio, ==, 2.5);
  tu_do (s, "t.ratio = 3");
  g_assert_cmpfloat (thing->ratio, ==, 3.0);
  tu_assert_eval_str (s, "t.ratio", "3.0");

  tu_do (s, "t.enabled = true");
  g_assert_true (thing->flag);
  tu_assert_eval_str (s, "t.enabled", "true");

  /* read-only and construct-only properties */
  tu_assert_eval_str (s, "t.id", "42");
  tu_assert_fails (s, "t.id = 1", SHELL_LUA_ERROR_RUNTIME, "property 'id' on test.Thing is read-only");
  tu_assert_eval_str (s, "t.token", "tok");
  tu_assert_fails (s, "t.token = 'x'", SHELL_LUA_ERROR_RUNTIME, "property 'token' on test.Thing is read-only");
  g_assert_cmpstr (thing->token, ==, "tok");

  /* a property of the table with no GObject property behind it */
  tu_assert_fails (s, "return t.ghost", SHELL_LUA_ERROR_RUNTIME, "property 'ghost' of test.Thing is not readable");
  tu_assert_fails (s, "t.ghost = 1", SHELL_LUA_ERROR_RUNTIME, "property 'ghost' on test.Thing is read-only");

  g_object_unref (s);
  g_object_unref (thing);
}

static void
test_property_errors (void)
{
  ShellLuaState *s = new_state (SHELL_LUA_STATE_PRIVILEGED);
  TestThing *thing = g_object_new (TEST_TYPE_THING, NULL);

  push (s, "t", NULL, thing);

  /* a wrong-type value names the property, the type and the problem */
  tu_assert_fails (s, "t.count = 'x'", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'count' on test.Thing: integer expected, got string");
  tu_assert_fails (s, "t.count = 1.5", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'count' on test.Thing: number has no integer representation");
  tu_assert_fails (s, "t.count = 2^40", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'count' on test.Thing: integer 1099511627776 out of range");
  tu_assert_fails (s, "t.count = true", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'count' on test.Thing: integer expected, got boolean");
  tu_assert_fails (s, "t.count = nil", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'count' on test.Thing: integer expected, got nil");
  tu_assert_fails (s, "t.count = {}", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'count' on test.Thing: integer expected, got table");
  /* in the type's range but rejected by the GParamSpec */
  tu_assert_fails (s, "t.count = 1000", SHELL_LUA_ERROR_RUNTIME,
                   "value out of range for property 'count' on test.Thing");
  tu_assert_fails (s, "t.count = -1", SHELL_LUA_ERROR_RUNTIME,
                   "value out of range for property 'count' on test.Thing");
  tu_assert_fails (s, "t.label = 5", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'label' on test.Thing: string expected, got number");
  tu_assert_fails (s, "t.ratio = 'x'", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'ratio' on test.Thing: number expected, got string");
  tu_assert_fails (s, "t.enabled = 1", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'enabled' on test.Thing: boolean expected, got number");
  g_assert_cmpint (thing->count, ==, 0);

  /* unknown names: reads are nil (feature detection), writes raise */
  tu_assert_eval_str (s, "t.nonexistent", "nil");
  tu_assert_eval_str (s, "t.secret", "nil");     /* a GObject property that is not exposed */
  tu_assert_eval_str (s, "t[1]", "nil");
  tu_assert_fails (s, "t.nonexistent = 1", SHELL_LUA_ERROR_RUNTIME, "no property 'nonexistent' on test.Thing");
  tu_assert_fails (s, "t.secret = 'x'", SHELL_LUA_ERROR_RUNTIME, "no property 'secret' on test.Thing");
  g_assert_cmpstr (thing->secret, ==, NULL);
  tu_assert_fails (s, "t[1] = 'x'", SHELL_LUA_ERROR_RUNTIME, "no property of type number on test.Thing");

  g_object_unref (s);
  g_object_unref (thing);
}

static void
test_enum_flags_strv (void)
{
  ShellLuaState *s = new_state (SHELL_LUA_STATE_PRIVILEGED);
  TestThing *thing = g_object_new (TEST_TYPE_THING, NULL);

  push (s, "t", NULL, thing);

  /* enums are nick strings */
  tu_assert_eval_str (s, "t.mode", "slow");
  tu_do (s, "t.mode = 'fast'");
  g_assert_cmpint (thing->mode, ==, TEST_MODE_FAST);
  tu_assert_eval_str (s, "t.mode", "fast");
  tu_do (s, "t.mode = 0");
  g_assert_cmpint (thing->mode, ==, TEST_MODE_SLOW);
  tu_assert_fails (s, "t.mode = 'bogus'", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'mode' on test.Thing: invalid value 'bogus' for TestMode (valid: slow, fast)");
  tu_assert_fails (s, "t.mode = 7", SHELL_LUA_ERROR_RUNTIME, "invalid value 7 for TestMode");
  tu_assert_fails (s, "t.mode = true", SHELL_LUA_ERROR_RUNTIME, "enum nick (string) expected, got boolean");
  g_assert_cmpint (thing->mode, ==, TEST_MODE_SLOW);

  /* flags are sets */
  tu_assert_eval_true (s, "next(t.features) == nil");
  tu_do (s, "t.features = {a = true, b = true}");
  g_assert_cmpint (thing->features, ==, TEST_FEATURE_A | TEST_FEATURE_B);
  tu_assert_eval_true (s, "t.features.a == true and t.features.b == true");
  tu_do (s, "t.features = {a = true, b = false}");
  g_assert_cmpint (thing->features, ==, TEST_FEATURE_A);
  tu_do (s, "t.features = {'b'}");
  g_assert_cmpint (thing->features, ==, TEST_FEATURE_B);
  tu_do (s, "t.features = 'a'");
  g_assert_cmpint (thing->features, ==, TEST_FEATURE_A);
  tu_do (s, "t.features = {}");
  g_assert_cmpint (thing->features, ==, TEST_FEATURE_NONE);
  tu_assert_fails (s, "t.features = {bogus = true}", SHELL_LUA_ERROR_RUNTIME, "invalid flag 'bogus' for TestFeatures");
  tu_assert_fails (s, "t.features = 5.5", SHELL_LUA_ERROR_RUNTIME, "flags table {nick=true} expected, got number");

  /* GStrv is a sequence of strings; NULL reads as an empty table */
  tu_assert_eval_true (s, "type(t.tags) == 'table' and #t.tags == 0");
  tu_do (s, "t.tags = {'x', 'y', 'z'}");
  g_assert_cmpuint (g_strv_length (thing->tags), ==, 3);
  g_assert_cmpstr (thing->tags[1], ==, "y");
  tu_assert_eval_true (s, "#t.tags == 3 and t.tags[1] == 'x' and t.tags[3] == 'z'");
  tu_do (s, "t.tags = {}");
  g_assert_cmpuint (g_strv_length (thing->tags), ==, 0);
  tu_assert_fails (s, "t.tags = {'a', 2}", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'tags' on test.Thing: element 2 is a number, string expected");
  tu_assert_fails (s, "t.tags = 'x'", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'tags' on test.Thing: table of strings expected, got string");

  g_object_unref (s);
  g_object_unref (thing);
}

static void
test_object_property (void)
{
  ShellLuaState *s = new_state (SHELL_LUA_STATE_PRIVILEGED);
  TestThing *thing = g_object_new (TEST_TYPE_THING, NULL);
  TestThing *friend = g_object_new (TEST_TYPE_SUB_THING, NULL);
  GObject *other = g_object_new (TEST_TYPE_OTHER, NULL);

  push (s, "t", NULL, thing);
  push (s, "f", NULL, friend);
  push (s, "o", NULL, other);

  tu_assert_eval_str (s, "t.friend", "nil");
  tu_do (s, "t.friend = f");
  g_assert_true (thing->friend == friend);
  /* reading gives back the same handle, as its most specific type */
  tu_assert_eval_true (s, "rawequal(t.friend, f) and t.friend.depth == 3");
  tu_do (s, "t.friend = nil");
  g_assert_null (thing->friend);

  tu_assert_fails (s, "t.friend = o", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'friend' on test.Thing: test.Other handle given where TestThing is expected");
  tu_assert_fails (s, "t.friend = 5", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'friend' on test.Thing: handle expected, got number");
  tu_assert_fails (s, "t.friend = {}", SHELL_LUA_ERROR_RUNTIME, "handle expected, got table");
  g_assert_null (thing->friend);

  /* a dead handle cannot be stored */
  tu_do (s, "t.friend = f");
  g_assert_nonnull (thing->friend);
  shell_lua_handle_invalidate (NULL, G_OBJECT (friend));
  tu_assert_fails (s, "t.friend = f", SHELL_LUA_ERROR_RUNTIME, "handle to test.SubThing is no longer valid");

  g_object_unref (s);
  g_object_unref (thing);
  g_object_unref (friend);
  g_object_unref (other);
}

static void
test_tostring_and_metatable (void)
{
  ShellLuaState *priv = new_state (SHELL_LUA_STATE_PRIVILEGED);
  ShellLuaState *sbx = new_state (SHELL_LUA_STATE_SANDBOXED);
  GObject *thing = g_object_new (TEST_TYPE_THING, NULL);

  push (priv, "t", NULL, thing);
  push (sbx, "t", NULL, thing);

  tu_assert_eval_true (priv, "tostring(t):find('^<test%.Thing 0x%x+>$') ~= nil");
  tu_assert_eval_true (sbx, "tostring(t):find('^<test%.Thing 0x%x+>$') ~= nil");
  /* the metatable is locked: false for the stock getmetatable, nil in the sandbox */
  tu_assert_eval_str (priv, "getmetatable(t)", "false");
  tu_assert_eval_str (sbx, "getmetatable(t)", "nil");
  tu_assert_fails (priv, "setmetatable(t, {})", SHELL_LUA_ERROR_RUNTIME, "table expected, got test.Thing");
  /* an error raised by a handle in a sandboxed state is just another error */
  tu_assert_fails (sbx, "t.nonexistent = 1", SHELL_LUA_ERROR_RUNTIME, "no property 'nonexistent' on test.Thing");
  tu_assert_eval_str (sbx, "t:add(2)", "2");

  g_object_unref (priv);
  g_object_unref (sbx);
  g_object_unref (thing);
}

static void
test_object_finalized (void)
{
  ShellLuaState *s = new_state (SHELL_LUA_STATE_PRIVILEGED);
  GObject *obj = g_object_new (TEST_TYPE_THING, NULL);
  GObject *other;

  push (s, "t", NULL, obj);
  tu_assert_eval_true (s, "is_alive(t)");
  tu_assert_eval_str (s, "t:add(3)", "3");

  g_object_unref (obj);     /* last reference: the handle dies with it */

  tu_assert_eval_true (s, "not is_alive(t)");
  tu_assert_eval_str (s, "tostring(t)", "<test.Thing (dead)>");
  tu_assert_fails (s, "return t.count", SHELL_LUA_ERROR_RUNTIME, "handle to test.Thing is no longer valid");
  tu_assert_fails (s, "t.count = 1", SHELL_LUA_ERROR_RUNTIME, "handle to test.Thing is no longer valid");
  tu_assert_fails (s, "t:add(1)", SHELL_LUA_ERROR_RUNTIME, "handle to test.Thing is no longer valid");
  tu_assert_fails (s, "return t.nonexistent", SHELL_LUA_ERROR_RUNTIME, "handle to test.Thing is no longer valid");
  tu_assert_fails (s, "return thing_count(t)", SHELL_LUA_ERROR_RUNTIME, "handle to test.Thing is no longer valid");
  tu_assert_fails (s, "return peek_is_thing(t)", SHELL_LUA_ERROR_RUNTIME, "handle to test.Thing is no longer valid");
  /* ... and the error is catchable */
  tu_assert_eval_true (s, "(function() local ok, e = pcall(function() return t.count end) "
                          "return not ok and e:find('no longer valid', 1, true) ~= nil end)()");

  /* whatever gets allocated next (possibly at the same address) is a fresh handle */
  other = g_object_new (TEST_TYPE_THING, NULL);
  push (s, "t2", NULL, other);
  tu_assert_eval_true (s, "is_alive(t2) and t2.count == 0 and t ~= t2 and not rawequal(t, t2)");
  tu_assert_eval_true (s, "tostring(t2) ~= tostring(t)");

  g_object_unref (s);
  g_object_unref (other);
}

static void
test_invalidate (void)
{
  ShellLuaState *s1 = new_state (SHELL_LUA_STATE_PRIVILEGED);
  ShellLuaState *s2 = new_state (SHELL_LUA_STATE_PRIVILEGED);
  ShellLuaState *s3 = new_state (SHELL_LUA_STATE_SANDBOXED);
  TestThing *obj = g_object_new (TEST_TYPE_THING, NULL);
  TestThing *obj2 = g_object_new (TEST_TYPE_THING, NULL);
  TestThing *unwrapped = g_object_new (TEST_TYPE_THING, NULL);

  push (s1, "t", NULL, obj);
  push (s2, "t", NULL, obj);
  push (s3, "t", NULL, obj);
  push (s1, "u", NULL, obj2);
  push (s2, "u", NULL, obj2);

  /* a single state */
  shell_lua_handle_invalidate (shell_lua_state_get_L (s1), G_OBJECT (obj2));
  tu_assert_eval_true (s1, "not is_alive(u) and is_alive(t)");
  tu_assert_eval_true (s2, "is_alive(u) and is_alive(t)");

  /* every state at once */
  shell_lua_handle_invalidate (NULL, G_OBJECT (obj));
  tu_assert_eval_true (s1, "not is_alive(t)");
  tu_assert_eval_true (s2, "not is_alive(t)");
  tu_assert_eval_true (s3, "not is_alive(t)");
  tu_assert_eval_true (s2, "is_alive(u)");
  tu_assert_fails (s1, "return t.count", SHELL_LUA_ERROR_RUNTIME, "handle to test.Thing is no longer valid");
  tu_assert_fails (s3, "return t.count", SHELL_LUA_ERROR_RUNTIME, "handle to test.Thing is no longer valid");
  /* the object itself is untouched */
  g_assert_true (G_IS_OBJECT (obj));
  obj->count = 9;

  /* wrapping it again gives a new, live handle; the old one stays dead */
  push (s1, "t_new", NULL, obj);
  tu_assert_eval_true (s1, "is_alive(t_new) and t_new.count == 9 and t_new ~= t and not is_alive(t)");

  /* harmless no-ops */
  shell_lua_handle_invalidate (NULL, NULL);
  shell_lua_handle_invalidate (NULL, G_OBJECT (unwrapped));
  shell_lua_handle_invalidate (shell_lua_state_get_L (s1), G_OBJECT (unwrapped));
  shell_lua_handle_invalidate (NULL, G_OBJECT (obj));
  shell_lua_handle_invalidate (NULL, G_OBJECT (obj));
  tu_assert_eval_true (s1, "not is_alive(t_new)");

  g_object_unref (s1);
  g_object_unref (s2);
  g_object_unref (s3);
  g_object_unref (obj);
  g_object_unref (obj2);
  g_object_unref (unwrapped);
}

static void
test_subtype_resolution (void)
{
  ShellLuaState *s = new_state (SHELL_LUA_STATE_PRIVILEGED);
  GObject *thing = g_object_new (TEST_TYPE_THING, NULL);
  GObject *sub = g_object_new (TEST_TYPE_SUB_THING, "count", 4, NULL);
  GObject *subsub = g_object_new (TEST_TYPE_SUB_SUB_THING, NULL);
  GObject *plain = g_object_new (G_TYPE_OBJECT, NULL);
  g_autofree char *err = NULL;

  /* the most specific registered type wins; unregistered subclasses fall back */
  push (s, "thing", NULL, thing);
  push (s, "sub", NULL, sub);
  push (s, "subsub", NULL, subsub);
  tu_assert_eval_true (s, "tostring(thing):find('^<test%.Thing ') ~= nil");
  tu_assert_eval_true (s, "tostring(sub):find('^<test%.SubThing ') ~= nil");
  tu_assert_eval_true (s, "tostring(subsub):find('^<test%.SubThing ') ~= nil");
  tu_assert_eval_str (s, "sub:extra()", "extra:3");
  tu_assert_eval_str (s, "subsub:extra()", "extra:3");
  tu_assert_eval_str (s, "thing.extra", "nil");

  /* inheritance: methods and properties of the parent type */
  tu_assert_eval_str (s, "sub.count", "4");
  tu_assert_eval_str (s, "sub:add(1)", "5");
  tu_do (s, "sub.depth = 7; sub.label = 'inherited'");
  tu_assert_eval_str (s, "sub:describe()", "Thing(inherited=5)");
  tu_assert_eval_str (s, "sub.depth", "7");
  tu_assert_fails (s, "thing.depth = 1", SHELL_LUA_ERROR_RUNTIME, "no property 'depth' on test.Thing");
  tu_assert_fails (s, "sub.depth = 11", SHELL_LUA_ERROR_RUNTIME, "value out of range for property 'depth'");
  /* check/peek accept subtypes of the requested type, and nothing else */
  tu_assert_eval_str (s, "thing_count(sub)", "5");
  tu_assert_eval_true (s, "peek_is_thing(thing) and peek_is_thing(sub) and peek_is_thing(subsub)");
  tu_assert_eval_true (s, "not peek_is_thing(5) and not peek_is_thing(nil) and not peek_is_thing({}) and "
                          "not peek_is_thing('x')");
  tu_assert_eval_true (s, "peek_is_any(sub) and not peek_is_any(5) and check_any(thing)");
  tu_assert_fails (s, "check_any(5)", SHELL_LUA_ERROR_RUNTIME, "handle expected, got number");
  tu_assert_fails (s, "thing_count(5)", SHELL_LUA_ERROR_RUNTIME, "test.Thing expected, got number");

  /* not wrapped at all */
  err = try_push (s, "plain", NULL, plain);
  g_assert_nonnull (err);
  g_assert_nonnull (strstr (err, "no handle type registered for GObject"));
  g_clear_pointer (&err, g_free);

  /* an explicit type must fit the object */
  err = try_push (s, "x", "test.Other", sub);
  g_assert_nonnull (err);
  g_assert_nonnull (strstr (err, "object is a TestSubThing, not a test.Other"));
  g_clear_pointer (&err, g_free);
  err = try_push (s, "x", "test.NoSuchType", sub);
  g_assert_nonnull (err);
  g_assert_nonnull (strstr (err, "unknown handle type 'test.NoSuchType'"));
  g_clear_pointer (&err, g_free);
  err = try_push (s, "x", "test.SubThing", thing);
  g_assert_nonnull (err);
  g_assert_nonnull (strstr (err, "object is a TestThing, not a test.SubThing"));
  g_clear_pointer (&err, g_free);
  err = try_push (s, "x", NULL, NULL);
  g_assert_nonnull (err);
  g_assert_nonnull (strstr (err, "not a GObject"));
  g_clear_pointer (&err, g_free);
  /* none of the failures left anything behind */
  tu_assert_eval_str (s, "x", "nil");
  tu_assert_eval_str (s, "plain", "nil");

  g_object_unref (s);
  g_object_unref (thing);
  g_object_unref (sub);
  g_object_unref (subsub);
  g_object_unref (plain);
}

static void
test_subtype_upgrade_keeps_identity (void)
{
  ShellLuaState *s = new_state (SHELL_LUA_STATE_PRIVILEGED);
  GObject *sub = g_object_new (TEST_TYPE_SUB_THING, NULL);

  /* first seen as its parent type... */
  push (s, "a", "test.Thing", sub);
  tu_assert_eval_true (s, "a.extra == nil and tostring(a):find('^<test%.Thing ') ~= nil");

  /* ...then as the most specific one: same userdata, now with the subtype's API */
  push (s, "b", NULL, sub);
  tu_assert_eval_true (s, "rawequal(a, b)");
  tu_assert_eval_true (s, "a.extra ~= nil and a:extra() == 'extra:3' and a.depth == 3");
  tu_assert_eval_true (s, "tostring(a):find('^<test%.SubThing ') ~= nil");

  /* asking for the parent type afterwards does not downgrade it */
  push (s, "c", "test.Thing", sub);
  tu_assert_eval_true (s, "rawequal(a, c) and c.extra ~= nil");

  g_object_unref (s);
  g_object_unref (sub);
}

static void
test_gc_and_lifetimes (void)
{
  ShellLuaState *s = new_state (SHELL_LUA_STATE_PRIVILEGED);
  ShellLuaState *s2 = new_state (SHELL_LUA_STATE_PRIVILEGED);
  ShellLuaState *s3 = new_state (SHELL_LUA_STATE_PRIVILEGED);
  GObject *obj = g_object_new (TEST_TYPE_THING, NULL);
  GObject *obj2 = g_object_new (TEST_TYPE_THING, NULL);

  /* a handle collected by Lua leaves nothing behind on the object */
  push (s, "t", NULL, obj);
  tu_do (s, "t = nil; collectgarbage(); collectgarbage()");
  push (s, "t", NULL, obj);       /* a new handle for the same object */
  tu_assert_eval_true (s, "is_alive(t)");
  tu_do (s, "t = nil; collectgarbage(); collectgarbage()");
  g_object_unref (obj);           /* finalizing must not touch freed handles */

  /* the state going away first: the object outlives it */
  push (s2, "t", NULL, obj2);
  g_object_unref (s2);
  /* ... and the other order */
  push (s3, "t", NULL, obj2);
  g_object_unref (obj2);
  tu_assert_eval_true (s3, "not is_alive(t)");
  g_object_unref (s3);

  g_object_unref (s);
}

static void
test_sandboxed_state (void)
{
  ShellLuaState *s = new_state (SHELL_LUA_STATE_SANDBOXED);
  GObject *thing = g_object_new (TEST_TYPE_THING, "label", "sb", NULL);

  push (s, "t", NULL, thing);
  tu_assert_eval_str (s, "t:add(4)", "4");
  tu_do (s, "t.mode = 'fast'");
  tu_assert_eval_str (s, "t.mode", "fast");
  tu_assert_fails (s, "t.count = 'x'", SHELL_LUA_ERROR_RUNTIME,
                   "bad value for property 'count' on test.Thing");
  g_object_unref (thing);
  tu_assert_fails (s, "return t.count", SHELL_LUA_ERROR_RUNTIME, "no longer valid");

  g_object_unref (s);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/shell-lua/handle/identity", test_identity);
  g_test_add_func ("/shell-lua/handle/methods", test_methods);
  g_test_add_func ("/shell-lua/handle/properties/get-set", test_properties_get_set);
  g_test_add_func ("/shell-lua/handle/properties/errors", test_property_errors);
  g_test_add_func ("/shell-lua/handle/properties/enum-flags-strv", test_enum_flags_strv);
  g_test_add_func ("/shell-lua/handle/properties/object", test_object_property);
  g_test_add_func ("/shell-lua/handle/tostring-and-metatable", test_tostring_and_metatable);
  g_test_add_func ("/shell-lua/handle/finalized", test_object_finalized);
  g_test_add_func ("/shell-lua/handle/invalidate", test_invalidate);
  g_test_add_func ("/shell-lua/handle/types/resolution", test_subtype_resolution);
  g_test_add_func ("/shell-lua/handle/types/upgrade-keeps-identity", test_subtype_upgrade_keeps_identity);
  g_test_add_func ("/shell-lua/handle/gc-and-lifetimes", test_gc_and_lifetimes);
  g_test_add_func ("/shell-lua/handle/sandboxed", test_sandboxed_state);

  return g_test_run ();
}
