/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The runtime rules of docs/api/README.md that API modules rely on:
 * `alive` and idempotent destroy() on dead handles, virtual properties,
 * assignments that call back (rule 7), the owned-object registry (reverse destruction,
 * the 4096 cap, slots freed by destroy(), children dying with parents),
 * callbacks (first argument, never after disable begins), deferral of
 * destroying the emitter (rule 8), capabilities.
 *
 * A test-only module, shell.testobj, wraps TestObj (a GObject with a
 * "destroy" signal, a "poked" signal, children and a parent).
 */

#include "test-utils.h"

#include "shell-hooks.h"
#include "shell-lua-extension.h"
#include "shell-lua-extension-private.h"
#include "shell-lua-handle.h"

/* ------------------------------------------------------------------------ */
/* TestObj                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct _TestObj TestObj;
typedef struct { GObjectClass parent_class; } TestObjClass;

struct _TestObj {
  GObject  parent_instance;
  char    *name;
  int      level;
  gboolean destroyed;
  TestObj *parent;       /* unowned */
  GPtrArray *children;   /* strong */
};

GType test_obj_get_type (void) G_GNUC_CONST;
#define TEST_TYPE_OBJ (test_obj_get_type ())
G_DEFINE_TYPE (TestObj, test_obj, G_TYPE_OBJECT)

enum { SIG_DESTROY, SIG_POKED, SIG_GONE, N_SIGS };
static guint obj_signals[N_SIGS];
enum { PROP_0, PROP_LEVEL, PROP_NAME, N_PROPS };

static GString *destroy_log;          /* names in destruction order */
static int finalized_objs;
static int hits;                      /* callbacks that ran (shell.testobj.hit) */

static void
test_obj_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  TestObj *self = (TestObj *) object;

  switch (id)
    {
    case PROP_LEVEL:
      self->level = g_value_get_int (value);
      /* a setter that provokes the "poked" signal: its callbacks run (rule 7) */
      g_signal_emit (self, obj_signals[SIG_POKED], 0, self->level);
      break;
    case PROP_NAME:
      g_free (self->name);
      self->name = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
test_obj_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  TestObj *self = (TestObj *) object;

  switch (id)
    {
    case PROP_LEVEL:
      g_value_set_int (value, self->level);
      break;
    case PROP_NAME:
      g_value_set_string (value, self->name);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
test_obj_finalize (GObject *object)
{
  TestObj *self = (TestObj *) object;

  finalized_objs++;
  g_free (self->name);
  g_ptr_array_unref (self->children);
  G_OBJECT_CLASS (test_obj_parent_class)->finalize (object);
}

static void
test_obj_class_init (TestObjClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);

  oc->set_property = test_obj_set_property;
  oc->get_property = test_obj_get_property;
  oc->finalize = test_obj_finalize;
  g_object_class_install_property (oc, PROP_LEVEL,
    g_param_spec_int ("level", NULL, NULL, 0, 1000, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (oc, PROP_NAME,
    g_param_spec_string ("name", NULL, NULL, NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  obj_signals[SIG_DESTROY] = g_signal_new ("destroy", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                           0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  /* a destroyed signal with arguments, like ShellNotification::destroy */
  obj_signals[SIG_GONE] = g_signal_new ("gone", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);
  obj_signals[SIG_POKED] = g_signal_new ("poked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
test_obj_init (TestObj *self)
{
  self->children = g_ptr_array_new_with_free_func (g_object_unref);
}

/* The "really destroy" function: children first, then the destroy signal. */
static void
test_obj_destroy (GObject *object)
{
  TestObj *self = (TestObj *) object;

  if (self->destroyed)
    return;
  self->destroyed = TRUE;

  g_object_ref (self);
  while (self->children->len > 0)
    {
      TestObj *child = g_object_ref (g_ptr_array_index (self->children, self->children->len - 1));

      g_ptr_array_remove_index (self->children, self->children->len - 1);
      child->parent = NULL;
      test_obj_destroy (G_OBJECT (child));
      g_object_unref (child);
    }
  if (destroy_log != NULL)
    g_string_append_printf (destroy_log, "%s,", self->name != NULL ? self->name : "?");
  if (self->parent != NULL)
    g_ptr_array_remove (self->parent->children, self);
  g_signal_emit (self, obj_signals[SIG_DESTROY], 0);
  g_signal_emit (self, obj_signals[SIG_GONE], 0, "bye", 7);
  g_object_unref (self);
}

static gboolean
test_obj_contains (GObject *ancestor, GObject *descendant)
{
  TestObj *d = (TestObj *) descendant;

  for (; d != NULL; d = d->parent)
    if ((GObject *) d == ancestor)
      return TRUE;
  return FALSE;
}

static const ShellLuaOwnOps obj_ops = { test_obj_destroy, "destroy" };
static const ShellLuaOwnOps gone_ops = { test_obj_destroy, "gone" };

/* ------------------------------------------------------------------------ */
/* shell.testobj                                                             */
/* ------------------------------------------------------------------------ */

static void
push_int_arg (lua_State *L, gpointer data)
{
  lua_pushinteger (L, GPOINTER_TO_INT (data));
}

static void
poked_cb_n (TestObj *obj, int n, ShellLuaCallback *cb)
{
  shell_lua_callback_emit (cb, 1, push_int_arg, GINT_TO_POINTER (n));
}

static const char * const new_fields[] = { "name", "parent", "on_poked", "level", "via_gone", NULL };

static int
l_new (lua_State *L)
{
  TestObj *obj, *parent = NULL;
  int ref;

  shell_lua_check_fields (L, 1, new_fields);
  shell_lua_check_owned_capacity (L);
  if (lua_getfield (L, 1, "parent") != LUA_TNIL)
    parent = (TestObj *) shell_lua_handle_check (L, -1, "test.Obj");
  lua_pop (L, 1);
  ref = shell_lua_opt_callback (L, 1, "on_poked", FALSE);

  obj = g_object_new (TEST_TYPE_OBJ, "name", shell_lua_opt_string (L, 1, "name", "anon"), NULL);
  if (parent != NULL)
    {
      obj->parent = parent;
      g_ptr_array_add (parent->children, g_object_ref (obj));
    }
  shell_lua_own_object (L, G_OBJECT (obj),
                        shell_lua_opt_boolean (L, 1, "via_gone", FALSE) ? &gone_ops : &obj_ops);
  g_object_unref (obj);   /* ours; the context holds one (and the parent) */
  if (ref != LUA_NOREF)
    {
      ShellLuaCallback *cb = shell_lua_callback_new (L, G_OBJECT (obj), ref);

      shell_lua_callback_connect (cb, obj, "poked", G_CALLBACK (poked_cb_n));
    }
  shell_lua_handle_push (L, "test.Obj", G_OBJECT (obj));
  return 1;
}

static int
l_obj_poke (lua_State *L)
{
  GObject *obj = shell_lua_handle_check (L, 1, "test.Obj");

  g_signal_emit (obj, obj_signals[SIG_POKED], 0, (int) luaL_optinteger (L, 2, 0));
  return 0;
}

static int
l_obj_on_poked (lua_State *L)
{
  GObject *obj = shell_lua_handle_check (L, 1, "test.Obj");
  ShellLuaCallback *cb;
  gulong id;

  luaL_checktype (L, 2, LUA_TFUNCTION);
  lua_pushvalue (L, 2);
  cb = shell_lua_callback_new (L, obj, luaL_ref (L, LUA_REGISTRYINDEX));
  id = shell_lua_callback_connect (cb, obj, "poked", G_CALLBACK (poked_cb_n));
  return shell_lua_callback_push_connection (L, cb, obj, id);
}

/* destroyed by someone else than the context (a parent, the shell) */
static int
l_obj_external_destroy (lua_State *L)
{
  test_obj_destroy (shell_lua_handle_check (L, 1, "test.Obj"));
  return 0;
}

static int
l_obj_plain (lua_State *L)
{
  shell_lua_handle_check (L, 1, "test.Obj");
  lua_pushliteral (L, "plain");
  return 1;
}

static int
l_obj_level_of (lua_State *L)
{
  TestObj *obj = (TestObj *) shell_lua_handle_check (L, 1, "test.Obj");

  lua_pushinteger (L, obj->level);
  return 1;
}

/* deferral probe: o:reparent_probe() returns whether the action was deferred */
typedef struct { int *flag; } LaterData;

static void later_fn (gpointer data) { *((LaterData *) data)->flag = 1; }

static int later_flag;

static int
l_obj_defer(lua_State *L)
{
  GObject *obj = shell_lua_handle_check (L, 1, "test.Obj");
  LaterData *d = g_new0 (LaterData, 1);

  d->flag = &later_flag;
  if (shell_lua_defer_if_emitting (L, obj, later_fn, d, g_free))
    {
      lua_pushboolean (L, 1);
      return 1;
    }
  g_free (d);
  lua_pushboolean (L, 0);
  return 1;
}

static int
l_later_flag (lua_State *L)
{
  lua_pushinteger (L, later_flag);
  return 1;
}

static void
virt_get_double (lua_State *L, GObject *object)
{
  lua_pushinteger (L, ((TestObj *) object)->level * 2);
}

static void
virt_set_double (lua_State *L, GObject *object, int idx)
{
  lua_Integer v = luaL_checkinteger (L, idx);

  if (v < 0 || v % 2)
    luaL_error (L, "double must be a non-negative even integer");
  g_object_set (object, "level", (int) (v / 2), NULL);
}

static void
virt_get_ro (lua_State *L, GObject *object)
{
  lua_pushstring (L, ((TestObj *) object)->name);
}

static void
virt_get_children (lua_State *L, GObject *object)
{
  lua_pushinteger (L, ((TestObj *) object)->children->len);
}

static const luaL_Reg obj_methods[] = {
  { "poke", l_obj_poke },
  { "on_poked", l_obj_on_poked },
  { "plain", l_obj_plain },
  { "external_destroy", l_obj_external_destroy },
  { "defer", l_obj_defer },
  { "level_of", l_obj_level_of },
  { NULL, NULL }
};

static const ShellLuaHandleMethod obj_methods_ex[] = {
  { "destroy", shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty obj_properties[] = {
  { "level", NULL, TRUE, TRUE },
  { "name", NULL, TRUE, FALSE },
  { NULL }
};

static const ShellLuaHandleVirtual obj_virtuals[] = {
  { "double", virt_get_double, virt_set_double },
  { "label", virt_get_ro, NULL },
  { "n_children", virt_get_children, NULL },
  { NULL }
};

static ShellLuaHandleType obj_type = {
  .name = "test.Obj",
  .methods = obj_methods,
  .methods_ex = obj_methods_ex,
  .properties = obj_properties,
  .virtuals = obj_virtuals,
};

/* A type sharing the GType of test.Obj: what a callback's first argument is */
static int
l_plus_only (lua_State *L)
{
  shell_lua_handle_check (L, 1, "test.ObjPlus");
  lua_pushliteral (L, "plus");
  return 1;
}

static const luaL_Reg plus_methods[] = { { "plus_only", l_plus_only }, { NULL, NULL } };
static ShellLuaHandleType plus_type = {
  .name = "test.ObjPlus", .parent = "test.Obj", .methods = plus_methods,
};

static int
l_new_plus (lua_State *L)
{
  TestObj *obj = g_object_new (TEST_TYPE_OBJ, "name", "plus", NULL);

  shell_lua_own_object (L, G_OBJECT (obj), &obj_ops);
  g_object_unref (obj);
  shell_lua_handle_push (L, "test.ObjPlus", G_OBJECT (obj));
  return 1;
}

static int
l_resolve (lua_State *L)
{
  shell_lua_handle_push (L, NULL, shell_lua_handle_check (L, 1, NULL));
  return 1;
}

static int
l_count (lua_State *L)
{
  lua_pushinteger (L, shell_lua_n_owned_objects (L));
  return 1;
}

static int
l_log (lua_State *L)
{
  lua_pushstring (L, destroy_log->str);
  g_string_truncate (destroy_log, 0);
  return 1;
}

static int
l_hit (lua_State *L)
{
  hits++;
  return 0;
}

static int
l_hits (lua_State *L)
{
  lua_pushinteger (L, hits);
  return 1;
}

static int
l_cap (lua_State *L)
{
  shell_lua_require_capability (L, "search", "shell.testobj.cap()");
  return 0;
}

static void
testobj_register (ShellLuaState *state, lua_State *L)
{
  static const luaL_Reg funcs[] = {
    { "new", l_new }, { "count", l_count }, { "log", l_log }, { "hit", l_hit },
    { "hits", l_hits }, { "cap", l_cap }, { "later_flag", l_later_flag },
    { "new_plus", l_new_plus }, { "resolve", l_resolve },
    { "new_plus", l_new_plus }, { "resolve", l_resolve },
    { NULL, NULL }
  };

  obj_type.gtype = TEST_TYPE_OBJ;
  plus_type.gtype = TEST_TYPE_OBJ;
  shell_lua_handle_register_type (L, &obj_type);
  shell_lua_handle_register_type (L, &plus_type);

  luaL_newlib (L, funcs);
  lua_setfield (L, -2, "testobj");
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static ShellLuaState *
new_priv (void)
{
  return tu_state_new ("objects", SHELL_LUA_STATE_PRIVILEGED);
}

static void
reset (void)
{
  hits = 0;
  later_flag = 0;
  finalized_objs = 0;
  g_string_truncate (destroy_log, 0);
}

#define PRELUDE "local t = require('shell').testobj\n"

/* ------------------------------------------------------------------------ */
/* Handles: alive, destroy on dead handles, virtuals, assignment guard      */
/* ------------------------------------------------------------------------ */

static void
test_alive_and_destroy (void)
{
  ShellLuaState *s = new_priv ();

  reset ();
  tu_do (s, PRELUDE
         "o = t.new{name='a'}\n"
         "assert(o.alive == true)\n"
         "assert(o:plain() == 'plain')\n"
         "o:destroy()\n"
         "assert(o.alive == false)\n"
         /* idempotent and allowed on a dead handle */
         "o:destroy()\n"
         "o:destroy()\n"
         "assert(tostring(o):find('dead'))\n"
         "assert(o == o)\n"
         "assert(t.count() == 0)\n");
  /* any other use raises */
  tu_assert_fails (s, PRELUDE "o = t.new{}; o:destroy(); o:plain()", SHELL_LUA_ERROR_RUNTIME,
                   "handle to test.Obj is no longer valid");
  tu_assert_fails (s, "return o.level", SHELL_LUA_ERROR_RUNTIME, "no longer valid");
  tu_assert_fails (s, "o.level = 3", SHELL_LUA_ERROR_RUNTIME, "no longer valid");
  tu_assert_fails (s, "return o.nothing", SHELL_LUA_ERROR_RUNTIME, "no longer valid");
  /* `alive` is read-only, on live and dead handles */
  tu_assert_fails (s, PRELUDE "t.new{}.alive = true", SHELL_LUA_ERROR_RUNTIME, "'alive' on test.Obj is read-only");
  tu_assert_fails (s, "o.alive = true", SHELL_LUA_ERROR_RUNTIME, "no longer valid");
  tu_do (s, PRELUDE "local p = t.new{}; p:destroy(); p:destroy(); assert(not p.alive)");
  g_object_unref (s);
}

static void
test_owned_object_is_released (void)
{
  ShellLuaState *s = new_priv ();

  reset ();
  /* dropping the last Lua reference never destroys an owned object */
  tu_do (s, PRELUDE "t.new{name='keep'}; collectgarbage(); collectgarbage()");
  g_assert_cmpint (finalized_objs, ==, 0);
  tu_assert_eval_str (s, "require('shell').testobj.count()", "1");
  /* destroy frees the slot and the object */
  tu_do (s, PRELUDE "local o = t.new{name='x'}; assert(t.count() == 2); o:destroy(); assert(t.count() == 1)");
  g_assert_cmpint (finalized_objs, ==, 1);
  g_object_unref (s);
  /* the state teardown destroys what is left */
  g_assert_cmpint (finalized_objs, ==, 2);
}

static void
test_virtual_properties (void)
{
  ShellLuaState *s = new_priv ();

  reset ();
  tu_do (s, PRELUDE
         "local o = t.new{name='v', level=0}\n"
         "o.level = 5\n"
         "assert(o.double == 10)\n"
         "o.double = 8\n"
         "assert(o.level == 4)\n"
         "assert(o.label == 'v')\n"
         "assert(o.n_children == 0)\n"
         "local c = t.new{parent=o}\n"
         "assert(o.n_children == 1)\n"
         /* unknown names read as nil */
         "assert(o.nonsense == nil)\n");
  tu_assert_fails (s, PRELUDE "local o = t.new{}; o.label = 'x'", SHELL_LUA_ERROR_RUNTIME,
                   "'label' on test.Obj is read-only");
  tu_assert_fails (s, PRELUDE "local o = t.new{}; o.double = 3", SHELL_LUA_ERROR_RUNTIME,
                   "double must be a non-negative even integer");
  tu_assert_fails (s, PRELUDE "local o = t.new{}; o.double = 'x'", SHELL_LUA_ERROR_RUNTIME,
                   "number expected");
  tu_assert_fails (s, PRELUDE "local o = t.new{}; o.nonsense = 1", SHELL_LUA_ERROR_RUNTIME,
                   "no property 'nonsense' on test.Obj");
  tu_assert_fails (s, PRELUDE "local o = t.new{}; o.name = 'x'", SHELL_LUA_ERROR_RUNTIME,
                   "'name' on test.Obj is read-only");
  /* a virtual setter that raised did not leave the guard on */
  tu_do (s, PRELUDE "local hit = 0; local o = t.new{on_poked = function() hit = hit + 1 end}\n"
         "o:poke(1); assert(hit == 1)\n");
  g_object_unref (s);
}

typedef struct { GString *names; } Names;

static void
collect_member (const char *name, gboolean is_method, gpointer data)
{
  g_string_append_printf (((Names *) data)->names, "%s%s,", name, is_method ? "()" : "");
}

static void
test_foreach_member (void)
{
  ShellLuaState *s = new_priv ();
  lua_State *L = shell_lua_state_get_L (s);
  Names n = { g_string_new (NULL) };

  reset ();
  tu_do (s, PRELUDE "o = t.new{}");
  lua_getglobal (L, "o");
  shell_lua_handle_foreach_member (L, -1, collect_member, &n);
  lua_pop (L, 1);
  g_assert_nonnull (strstr (n.names->str, "alive,"));
  g_assert_nonnull (strstr (n.names->str, "double,"));
  g_assert_nonnull (strstr (n.names->str, "level,"));
  g_assert_nonnull (strstr (n.names->str, "destroy()"));
  g_assert_nonnull (strstr (n.names->str, "poke()"));
  g_string_free (n.names, TRUE);
  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* Owned objects                                                             */
/* ------------------------------------------------------------------------ */

static void
test_reverse_destruction (void)
{
  ShellLuaState *s = new_priv ();

  reset ();
  tu_do (s, PRELUDE "a = t.new{name='a'}; b = t.new{name='b'}; c = t.new{name='c'}");
  shell_lua_objects_destroy_all (s);
  g_assert_cmpstr (destroy_log->str, ==, "c,b,a,");
  tu_do (s, "assert(not a.alive and not b.alive and not c.alive)");
  g_object_unref (s);
}

static void
test_parent_destroys_children (void)
{
  ShellLuaState *s = new_priv ();

  reset ();
  tu_do (s, PRELUDE
         "p = t.new{name='p'}; k1 = t.new{name='k1', parent=p}; k2 = t.new{name='k2', parent=p}\n"
         "g = t.new{name='g', parent=k1}\n"
         "assert(t.count() == 4)\n"
         "k2:destroy()\n"                      /* a child frees its slot immediately */
         "assert(t.count() == 3 and not k2.alive and p.alive and p.n_children == 1)\n"
         "p:destroy()\n"
         "assert(t.count() == 0)\n"
         "assert(not p.alive and not k1.alive and not g.alive)\n");
  /* children first, then the parent */
  g_assert_cmpstr (destroy_log->str, ==, "k2,g,k1,p,");
  g_assert_cmpint (finalized_objs, ==, 4);
  g_object_unref (s);
}

static void
test_destroyed_from_elsewhere (void)
{
  ShellLuaState *s = new_priv ();

  reset ();
  /* the slot is freed and the handle dies when the object is destroyed by
   * someone else, whatever arguments the destroyed signal carries */
  tu_do (s, PRELUDE
         "a = t.new{name='a'}; b = t.new{name='b', via_gone=true}\n"
         "assert(t.count() == 2)\n"
         "a:external_destroy()\n"
         "assert(not a.alive and t.count() == 1)\n"
         "b:external_destroy()\n"
         "assert(not b.alive and t.count() == 0)\n"
         "a:destroy(); b:destroy()\n");
  g_assert_cmpint (finalized_objs, ==, 2);
  /* and the explicit destroy of such an object works */
  tu_do (s, PRELUDE "c = t.new{via_gone=true}; c:destroy(); assert(not c.alive and t.count() == 0)");
  g_object_unref (s);
}

static void
test_type_hint (void)
{
  ShellLuaState *s = new_priv ();

  reset ();
  /* a handle pushed without a type name gets the type the module chose, even
   * once the first handle was collected */
  tu_do (s, PRELUDE
         "local o = t.new_plus()\n"
         "assert(o:plus_only() == 'plus')\n"
         "o = nil; collectgarbage(); collectgarbage()\n"
         "local again = t.resolve(t.new_plus())\n"
         "assert(again:plus_only() == 'plus')\n");
  g_object_unref (s);
}

static void
test_cap (void)
{
  ShellLuaState *s = tu_state_new ("cap", SHELL_LUA_STATE_SANDBOXED);

  reset ();
  tu_do (s, PRELUDE
         "local made = {}\n"
         "for i = 1, 4096 do made[i] = t.new{} end\n"
         "assert(t.count() == 4096)\n"
         "local ok, err = pcall(t.new, {})\n"
         "assert(not ok and tostring(err):find('too many objects'), err)\n"
         "assert(t.count() == 4096)\n"
         /* destroy() frees the slot at once */
         "made[100]:destroy()\n"
         "assert(t.count() == 4095)\n"
         "made[100] = t.new{}\n"
         "assert(t.count() == 4096)\n"
         /* a parent destroyed by something else frees its children's slots */
         "made[1]:destroy(); made[2]:destroy()\n"
         "assert(t.count() == 4094)\n");
  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* Callbacks                                                                 */
/* ------------------------------------------------------------------------ */

static void
test_callbacks (void)
{
  ShellLuaState *s = new_priv ();

  reset ();
  tu_do (s, PRELUDE
         "local log = {}\n"
         "o = t.new{name='cb', on_poked = function(self, n) log[#log+1] = {self, n} end}\n"
         "o:poke(7)\n"
         "assert(#log == 1 and log[1][1] == o and log[1][2] == 7)\n"
         /* h:on_<event>(fn) returns a Connection */
         "local seen\n"
         "local conn = o:on_poked(function(self, n) seen = n end)\n"
         "o:poke(9)\n"
         "assert(seen == 9 and #log == 2)\n"
         "conn:disconnect()\n"
         "o:poke(11)\n"
         "assert(seen == 9 and #log == 3)\n");
  g_object_unref (s);
}

static void
test_assignment_calls_back (void)
{
  ShellLuaState *s = new_priv ();

  reset ();
  tu_do (s, PRELUDE
         "local n = 0\n"
         "o = t.new{on_poked = function() n = n + 1 end}\n"
         /* rule 7: assigning a GObject property or a virtual calls back like
          * any other change */
         "o.level = 3\n"
         "assert(n == 1, n)\n"
         "o.double = 8\n"
         "assert(n == 2, n)\n"
         "o:poke(1)\n"
         "assert(n == 3)\n"
         /* a virtual setter that raises: the error reaches the assignment */
         "assert(not pcall(function() o.double = 3 end))\n"
         "assert(n == 3)\n"
         /* a callback may assign, and that assignment calls back in turn */
         "local levels = {}\n"
         "local conn = o:on_poked(function(self, level)\n"
         "  levels[#levels + 1] = level\n"
         "  if level < 7 then self.level = level + 1 end\n"
         "end)\n"
         "o:poke(5)\n"
         "assert(table.concat(levels, ',') == '5,6,7', table.concat(levels, ','))\n"
         "assert(n == 6, n)\n"
         "conn:disconnect()\n");
  g_object_unref (s);
}

/* A callback that assigns what calls it back, forever: the extension's bug.
 * Lua's limit of nested C calls cuts the loop with an error of the innermost
 * callback; the outer ones return and the state works afterwards. */
static void
test_assignment_loop_is_cut (void)
{
  ShellLuaState *s = new_priv ();

  reset ();
  tu_do (s, PRELUDE
         "local depth = 0\n"
         "o = t.new{}\n"
         "local conn = o:on_poked(function(self, level)\n"
         "  depth = depth + 1\n"
         "  self.level = level + 1\n"
         "end)\n"
         "o:poke(0)\n"
         "assert(depth > 10 and depth < 200, depth)\n"
         /* the innermost callback failed before or at its assignment */
         "assert(o.level == depth or o.level == depth - 1, o.level)\n"
         "conn:disconnect()\n"
         "local seen\n"
         "o:on_poked(function(_, level) seen = level end)\n"
         "o:poke(1)\n"
         "assert(seen == 1)\n");
  g_object_unref (s);
}

static void
test_callbacks_die_with_object (void)
{
  ShellLuaState *s = new_priv ();

  reset ();
  tu_do (s, PRELUDE
         "local n = 0\n"
         "o = t.new{on_poked = function() n = n + 1 end}\n"
         "o:poke(1); assert(n == 1)\n"
         "local conn = o:on_poked(function() n = n + 10 end)\n"
         "o:poke(2); assert(n == 12)\n"
         "o:destroy()\n"
         "conn:disconnect()\n"            /* harmless on a dead object */
         "assert(n == 12)\n");
  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* Rule 8                                                                    */
/* ------------------------------------------------------------------------ */

static void
test_destroy_emitter_is_deferred (void)
{
  ShellLuaState *s = new_priv ();

  reset ();
  shell_lua_set_contains_func (test_obj_contains);
  tu_do (s, PRELUDE
         "local state = {}\n"
         "o = t.new{name='o', on_poked = function(self)\n"
         "  self:destroy()\n"
         "  state.alive_inside = self.alive\n"
         "  state.level_inside = self.level\n"       /* still readable */
         "  self:destroy()\n"                         /* idempotent while deferred */
         "  state.count_inside = t.count()\n"
         "end}\n"
         "o:poke(1)\n"
         "assert(state.alive_inside == true and state.level_inside == 0)\n"
         "assert(state.count_inside == 1)\n"
         "assert(o.alive == false and t.count() == 0)\n");

  /* destroying an ancestor of the emitter is deferred as well */
  tu_do (s, PRELUDE
         "local inside\n"
         "p = t.new{name='p'}\n"
         "k = t.new{name='k', parent=p, on_poked = function(self)\n"
         "  p:destroy()\n"
         "  inside = {p.alive, k.alive}\n"
         "end}\n"
         "k:poke(1)\n"
         "assert(inside[1] == true and inside[2] == true)\n"
         "assert(not p.alive and not k.alive)\n");

  /* unrelated objects are destroyed at once */
  tu_do (s, PRELUDE
         "local inside\n"
         "other = t.new{name='other'}\n"
         "e = t.new{name='e', on_poked = function() other:destroy(); inside = other.alive end}\n"
         "e:poke(1)\n"
         "assert(inside == false)\n");

  /* the generic deferral helper (reparenting uses it) */
  tu_do (s, PRELUDE
         "assert(t.later_flag() == 0)\n"
         "local d = t.new{name='d'}\n"
         "assert(d:defer() == false)\n"          /* not emitting: caller acts */
         "local inside_flag, deferred\n"
         "e2 = t.new{on_poked = function(self) deferred = self:defer(); inside_flag = t.later_flag() end}\n"
         "e2:poke(1)\n"
         "assert(deferred == true and inside_flag == 0 and t.later_flag() == 1)\n");
  shell_lua_set_contains_func (NULL);
  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* Capabilities and extension integration                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
  char                     *tmp;
  char                     *user;
  ShellLuaExtensionManager *manager;
} Env;

static void
env_init (Env *e)
{
  g_autoptr (GFile) user = NULL;
  GFile *dirs[1];

  e->tmp = tu_tmpdir_new ();
  e->user = g_build_filename (e->tmp, "user", NULL);
  g_assert_cmpint (g_mkdir_with_parents (e->user, 0700), ==, 0);
  user = g_file_new_for_path (e->user);
  dirs[0] = user;
  e->manager = g_object_new (SHELL_TYPE_LUA_EXTENSION_MANAGER, NULL);
  shell_lua_extension_manager_set_search_dirs (e->manager, dirs, 1);
}

static void
env_clear (Env *e)
{
  g_object_unref (e->manager);
  tu_rm_rf (e->tmp);
  g_free (e->tmp);
  g_free (e->user);
}

static ShellLuaExtension *
install (Env *e, const char *uuid, const char *caps, const char *lua)
{
  g_autofree char *dir = g_build_filename (e->user, uuid, NULL);
  g_autofree char *metadata = g_strdup_printf (
    "{\"uuid\": \"%s\", \"name\": \"T\", \"shell-api\": \"1\"%s%s}", uuid,
    caps != NULL ? ", \"capabilities\": " : "", caps != NULL ? caps : "");

  tu_write_file (dir, "metadata.json", metadata);
  tu_write_file (dir, "extension.lua", lua);
  shell_lua_extension_manager_scan (e->manager);
  return shell_lua_extension_manager_lookup (e->manager, uuid);
}

static void
test_capability_helper (void)
{
  Env e;
  g_autoptr (GError) error = NULL;
  ShellLuaExtension *without, *with;

  env_init (&e);
  without = install (&e, "nocap@test", NULL,
                     "return { enable = function() local ok, err = pcall(require('shell').testobj.cap)\n"
                     "  assert(not ok and tostring(err):find('shell.testobj.cap() requires the \"search\" capability', 1, true), err)\n"
                     "end }");
  with = install (&e, "cap@test", "[\"search\"]",
                  "return { enable = function() require('shell').testobj.cap() end }");

  g_assert_true (shell_lua_extension_manager_enable (e.manager, "nocap@test", &error));
  g_assert_no_error (error);
  g_assert_cmpint (shell_lua_extension_get_state (without), ==, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  g_assert_true (shell_lua_extension_manager_enable (e.manager, "cap@test", &error));
  g_assert_cmpint (shell_lua_extension_get_state (with), ==, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  env_clear (&e);
}

static void
test_search_hook_gate (void)
{
  Env e;
  g_autoptr (GError) error = NULL;
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  ShellLuaExtension *denied, *allowed;

  shell_hook_registry_declare (registry, "search.query", "query:s terms:as", FALSE, "A search started");
  env_init (&e);
  denied = install (&e, "nosearch@test", NULL,
                    "local hooks = require('shell').hooks\n"
                    "return { enable = function()\n"
                    "  for _, connect in ipairs{ hooks.on } do\n"
                    "    local ok, err = pcall(connect, 'search.query', function() end)\n"
                    "    assert(not ok and tostring(err):find('hook \\'search.query\\' requires the \"search\" capability', 1, true), err)\n"
                    "  end\n"
                    "  local found\n"
                    "  for _, h in ipairs(hooks.list()) do if h.name == 'search.query' then found = h end end\n"
                    "  assert(found and found.capability == 'search')\n"
                    "end }");
  allowed = install (&e, "search@test", "[\"search\"]",
                     "return { enable = function()\n"
                     "  local conn = require('shell').hooks.on('search.query', function() end)\n"
                     "  assert(conn:is_connected())\n"
                     "end }");

  g_assert_true (shell_lua_extension_manager_enable (e.manager, "nosearch@test", &error));
  g_assert_no_error (error);
  g_assert_cmpint (shell_lua_extension_get_state (denied), ==, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  g_assert_true (shell_lua_extension_manager_enable (e.manager, "search@test", &error));
  g_assert_cmpint (shell_lua_extension_get_state (allowed), ==, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  env_clear (&e);
}

static void
test_extension_disable (void)
{
  Env e;
  g_autoptr (GError) error = NULL;
  ShellLuaExtension *ext;
  int hits_before;

  reset ();
  env_init (&e);
  ext = install (&e, "owner@test", NULL,
                 "local t = require('shell').testobj\n"
                 "local ext = {}\n"
                 "local objs = {}\n"
                 "function ext.enable()\n"
                 "  local hit = t.hit\n"
                 "  objs.a = t.new{name='a', on_poked = hit}\n"
                 "  objs.b = t.new{name='b', parent = objs.a, on_poked = hit}\n"
                 "  objs.c = t.new{name='c', on_poked = hit}\n"
                 "  objs.c:on_poked(hit)\n"
                 "end\n"
                 "function ext.disable()\n"
                 "  objs.c:poke(1)\n"        /* callbacks never run once disable begins */
                 "  objs.a:poke(1)\n"
                 "end\n"
                 "return ext");
  g_assert_true (shell_lua_extension_manager_enable (e.manager, "owner@test", &error));
  g_assert_no_error (error);
  g_assert_cmpint (shell_lua_extension_get_state (ext), ==, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  g_assert_cmpint (shell_lua_n_owned_objects (shell_lua_state_get_L (shell_lua_extension_get_lua_state (ext))), ==, 3);

  /* a poke from outside the extension's own code still reaches Lua before disable */
  tu_do (shell_lua_extension_get_lua_state (ext), "require('shell').testobj.hit()");
  hits_before = hits;

  g_assert_true (shell_lua_extension_manager_disable (e.manager, "owner@test", &error));
  g_assert_no_error (error);
  /* reverse creation order: c, then a's child b, then a. b is a's child, so
   * a destroys b first */
  g_assert_cmpstr (destroy_log->str, ==, "c,b,a,");
  g_assert_cmpint (hits, ==, hits_before);   /* nothing ran during or after disable() */
  g_assert_cmpint (finalized_objs, ==, 3);
  g_assert_null (shell_lua_extension_get_lua_state (ext));

  /* enabling again starts clean */
  reset ();
  g_assert_true (shell_lua_extension_manager_enable (e.manager, "owner@test", &error));
  g_assert_cmpint (shell_lua_n_owned_objects (shell_lua_state_get_L (shell_lua_extension_get_lua_state (ext))), ==, 3);
  g_assert_true (shell_lua_extension_manager_disable (e.manager, "owner@test", &error));
  env_clear (&e);
}

int
main (int argc, char *argv[])
{
  int ret;

  g_test_init (&argc, &argv, NULL);
  destroy_log = g_string_new (NULL);
  shell_lua_api_register ("testobj", testobj_register);

  g_test_add_func ("/shell-lua/objects/alive-and-destroy", test_alive_and_destroy);
  g_test_add_func ("/shell-lua/objects/release", test_owned_object_is_released);
  g_test_add_func ("/shell-lua/objects/virtual-properties", test_virtual_properties);
  g_test_add_func ("/shell-lua/objects/foreach-member", test_foreach_member);
  g_test_add_func ("/shell-lua/objects/reverse-destruction", test_reverse_destruction);
  g_test_add_func ("/shell-lua/objects/parent-destroys-children", test_parent_destroys_children);
  g_test_add_func ("/shell-lua/objects/destroyed-from-elsewhere", test_destroyed_from_elsewhere);
  g_test_add_func ("/shell-lua/objects/type-hint", test_type_hint);
  g_test_add_func ("/shell-lua/objects/cap", test_cap);
  g_test_add_func ("/shell-lua/objects/callbacks", test_callbacks);
  g_test_add_func ("/shell-lua/objects/assignment-calls-back", test_assignment_calls_back);
  g_test_add_func ("/shell-lua/objects/assignment-loop-is-cut", test_assignment_loop_is_cut);
  g_test_add_func ("/shell-lua/objects/callbacks-die-with-object", test_callbacks_die_with_object);
  g_test_add_func ("/shell-lua/objects/destroy-emitter-is-deferred", test_destroy_emitter_is_deferred);
  g_test_add_func ("/shell-lua/objects/capability-helper", test_capability_helper);
  g_test_add_func ("/shell-lua/objects/search-hook-gate", test_search_hook_gate);
  g_test_add_func ("/shell-lua/objects/extension-disable", test_extension_disable);

  ret = g_test_run ();
  g_string_free (destroy_log, TRUE);
  return ret;
}
