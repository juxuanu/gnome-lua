/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Fuzzing the Lua API boundary (docs/PLAN.md M8): malformed handles,
 * re-entrancy from hooks, disable during a callback, resource limits and
 * random sequences of API calls. The contract being checked is docs/api/
 * README.md (handles, ownership and teardown, deferred destroy, the
 * watchdog, the error policy); the runtime must never crash, never log a
 * critical, never leak a state or an object, and keep its registries
 * consistent whatever the Lua code does.
 *
 * Everything is deterministic. The pseudo random choices come from one
 * GRand seeded with SHELL_LUA_FUZZ_SEED (default: a constant; the seed is
 * printed with g_test_message()), SHELL_LUA_FUZZ_ITERATIONS scales the random
 * sequence test and SHELL_LUA_FUZZ_VERBOSE=1 traces every random call to
 * stderr so that a crash can be reproduced from the last line.
 *
 * Fixture: a test-only module, shell.fz, wraps FzObj (a GObject with one
 * property of every kind the handle layer marshals, a "destroy" signal, a
 * "poked" signal, children and a parent) and offers the helpers the
 * scripts need (hook emission, a synchronous disable of the calling
 * extension, counters). Warnings that sandboxed states log for failed
 * callbacks are captured instead of being fatal; criticals never are.
 */

#include "test-utils.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "shell-hooks.h"
#include "shell-lua-extension.h"
#include "shell-lua-extension-private.h"
#include "shell-lua-handle.h"

#define TEST_SCHEMA "org.gnome-lua.test"

/* ------------------------------------------------------------------------ */
/* Determinism                                                               */
/* ------------------------------------------------------------------------ */

static GRand *rng;
static guint32 fuzz_seed = 0x5eed1e55;
static int fuzz_iterations = 1500;
static gboolean fuzz_verbose;

static void
fuzz_init_from_environment (void)
{
  const char *seed = g_getenv ("SHELL_LUA_FUZZ_SEED");
  const char *iterations = g_getenv ("SHELL_LUA_FUZZ_ITERATIONS");

  if (seed != NULL && seed[0] != '\0')
    fuzz_seed = (guint32) g_ascii_strtoull (seed, NULL, 0);
  if (iterations != NULL && iterations[0] != '\0')
    fuzz_iterations = (int) g_ascii_strtoll (iterations, NULL, 0);
  fuzz_verbose = g_getenv ("SHELL_LUA_FUZZ_VERBOSE") != NULL;
  g_test_message ("SHELL_LUA_FUZZ_SEED=%u SHELL_LUA_FUZZ_ITERATIONS=%d", fuzz_seed, fuzz_iterations);
}

static void
rng_reset (guint32 salt)
{
  g_clear_pointer (&rng, g_rand_free);
  rng = g_rand_new_with_seed (fuzz_seed ^ salt);
}

/* ------------------------------------------------------------------------ */
/* Log capture: warnings are recorded, criticals abort                       */
/* ------------------------------------------------------------------------ */

static GPtrArray *captured;            /* char *, the warnings seen */
static GLogFunc previous_log_func;
static gpointer previous_log_data;
static GLogLevelFlags previous_fatal_mask;
static gboolean capturing;

static void
capture_handler (const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer data)
{
  if (level & (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR))
    {
      fprintf (stderr, "unexpected %s%s%s: %s\n", domain != NULL ? domain : "", domain != NULL ? " " : "",
               (level & G_LOG_LEVEL_ERROR) ? "error" : "critical", message);
      abort ();
    }
  if (level & G_LOG_LEVEL_WARNING)
    g_ptr_array_add (captured, g_strdup (message));
  /* messages and debug output are dropped */
}

static void
capture_begin (void)
{
  g_assert_false (capturing);
  capturing = TRUE;
  captured = g_ptr_array_new_with_free_func (g_free);
  previous_fatal_mask = g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  previous_log_func = g_log_set_default_handler (capture_handler, NULL);
  previous_log_data = NULL;
}

static void
capture_end (void)
{
  g_assert_true (capturing);
  g_log_set_default_handler (previous_log_func, previous_log_data);
  g_log_set_always_fatal (previous_fatal_mask);
  g_clear_pointer (&captured, g_ptr_array_unref);
  capturing = FALSE;
}

/* How many captured warnings contain @substr (NULL: all of them). */
static guint
captured_count (const char *substr)
{
  guint i, n = 0;

  for (i = 0; captured != NULL && i < captured->len; i++)
    if (substr == NULL || strstr (g_ptr_array_index (captured, i), substr) != NULL)
      n++;
  return n;
}

static void
captured_clear (void)
{
  g_ptr_array_set_size (captured, 0);
}

/* ------------------------------------------------------------------------ */
/* FzObj                                                                     */
/* ------------------------------------------------------------------------ */

typedef enum {
  FZ_MODE_SLOW,
  FZ_MODE_FAST,
} FzMode;

typedef enum {
  FZ_FEATURE_NONE = 0,
  FZ_FEATURE_A = 1 << 0,
  FZ_FEATURE_B = 1 << 1,
} FzFeatures;

GType fz_mode_get_type (void) G_GNUC_CONST;
GType fz_features_get_type (void) G_GNUC_CONST;
#define FZ_TYPE_MODE (fz_mode_get_type ())
#define FZ_TYPE_FEATURES (fz_features_get_type ())

G_DEFINE_ENUM_TYPE (FzMode, fz_mode,
                    G_DEFINE_ENUM_VALUE (FZ_MODE_SLOW, "slow"),
                    G_DEFINE_ENUM_VALUE (FZ_MODE_FAST, "fast"))
G_DEFINE_FLAGS_TYPE (FzFeatures, fz_features,
                     G_DEFINE_ENUM_VALUE (FZ_FEATURE_A, "a"),
                     G_DEFINE_ENUM_VALUE (FZ_FEATURE_B, "b"))

typedef struct _FzObj FzObj;
typedef struct { GObjectClass parent_class; } FzObjClass;

struct _FzObj {
  GObject     parent_instance;
  char       *name;
  int         i;
  guint       u;
  gint64      l;
  double      d;
  float       f;
  gboolean    b;
  char       *s;
  FzMode      mode;
  FzFeatures  flags;
  char      **tags;
  FzObj      *friend;      /* weak */
  GVariant   *any;
  GVariant   *vs;
  int         co;
  int         wo;
  gpointer    ptr;
  gint8       ch;
  guint8      uc;
  gulong      ul;
  glong       lg;
  guint64     u64;
  gboolean    destroyed;
  FzObj      *parent;      /* unowned */
  GPtrArray  *children;    /* strong */
};

GType fz_obj_get_type (void) G_GNUC_CONST;
#define FZ_TYPE_OBJ (fz_obj_get_type ())
G_DEFINE_TYPE (FzObj, fz_obj, G_TYPE_OBJECT)

enum {
  PROP_0, PROP_NAME, PROP_I, PROP_U, PROP_L, PROP_D, PROP_F, PROP_B, PROP_S, PROP_MODE, PROP_FLAGS,
  PROP_TAGS, PROP_FRIEND, PROP_ANY, PROP_VS, PROP_RO, PROP_CO, PROP_WO, PROP_PTR, PROP_CH, PROP_UC,
  PROP_UL, PROP_LG, PROP_U64, PROP_ACT, N_PROPS
};
enum { SIG_DESTROY, SIG_POKED, N_SIGS };
static guint obj_signals[N_SIGS];

static GString *destroy_log;       /* names in destruction order */
static GString *marks;             /* fz.mark() */
static int live_objs;              /* instances not finalized yet */
static GHashTable *live_set;       /* ... which ones (diagnostics) */
static int finalized_objs;
static int destroy_runs;           /* really-destroy functions that did work */
static int double_destroys;        /* ... and that were asked twice */
static int hits;
static int poked_count;

static void
fz_obj_set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  FzObj *self = (FzObj *) object;

  switch (id)
    {
    case PROP_NAME:
      g_free (self->name);
      self->name = g_value_dup_string (value);
      break;
    case PROP_I:
      self->i = g_value_get_int (value);
      /* a setter that provokes a signal: its callbacks run (rule 7) */
      g_signal_emit (self, obj_signals[SIG_POKED], 0, self->i);
      break;
    case PROP_U:
      self->u = g_value_get_uint (value);
      break;
    case PROP_L:
      self->l = g_value_get_int64 (value);
      break;
    case PROP_D:
      self->d = g_value_get_double (value);
      break;
    case PROP_F:
      self->f = g_value_get_float (value);
      break;
    case PROP_B:
      self->b = g_value_get_boolean (value);
      break;
    case PROP_S:
      g_free (self->s);
      self->s = g_value_dup_string (value);
      break;
    case PROP_MODE:
      self->mode = g_value_get_enum (value);
      break;
    case PROP_FLAGS:
      self->flags = g_value_get_flags (value);
      break;
    case PROP_TAGS:
      g_strfreev (self->tags);
      self->tags = g_value_dup_boxed (value);
      break;
    case PROP_FRIEND:
      g_set_weak_pointer (&self->friend, g_value_get_object (value));
      break;
    case PROP_ANY:
      g_clear_pointer (&self->any, g_variant_unref);
      self->any = g_value_dup_variant (value);
      break;
    case PROP_VS:
      g_clear_pointer (&self->vs, g_variant_unref);
      self->vs = g_value_dup_variant (value);
      break;
    case PROP_CO:
      self->co = g_value_get_int (value);
      break;
    case PROP_WO:
      self->wo = g_value_get_int (value);
      break;
    case PROP_PTR:
      self->ptr = g_value_get_pointer (value);
      break;
    case PROP_CH:
      self->ch = g_value_get_schar (value);
      break;
    case PROP_UC:
      self->uc = g_value_get_uchar (value);
      break;
    case PROP_UL:
      self->ul = g_value_get_ulong (value);
      break;
    case PROP_LG:
      self->lg = g_value_get_long (value);
      break;
    case PROP_U64:
      self->u64 = g_value_get_uint64 (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
fz_obj_get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  FzObj *self = (FzObj *) object;

  switch (id)
    {
    case PROP_NAME: g_value_set_string (value, self->name); break;
    case PROP_I: g_value_set_int (value, self->i); break;
    case PROP_U: g_value_set_uint (value, self->u); break;
    case PROP_L: g_value_set_int64 (value, self->l); break;
    case PROP_D: g_value_set_double (value, self->d); break;
    case PROP_F: g_value_set_float (value, self->f); break;
    case PROP_B: g_value_set_boolean (value, self->b); break;
    case PROP_S: g_value_set_string (value, self->s); break;
    case PROP_MODE: g_value_set_enum (value, self->mode); break;
    case PROP_FLAGS: g_value_set_flags (value, self->flags); break;
    case PROP_TAGS: g_value_set_boxed (value, self->tags); break;
    case PROP_FRIEND: g_value_set_object (value, self->friend); break;
    case PROP_ANY: g_value_set_variant (value, self->any); break;
    case PROP_VS: g_value_set_variant (value, self->vs); break;
    case PROP_RO: g_value_set_string (value, "read-only"); break;
    case PROP_CO: g_value_set_int (value, self->co); break;
    case PROP_PTR: g_value_set_pointer (value, self->ptr); break;
    case PROP_CH: g_value_set_schar (value, self->ch); break;
    case PROP_UC: g_value_set_uchar (value, self->uc); break;
    case PROP_UL: g_value_set_ulong (value, self->ul); break;
    case PROP_LG: g_value_set_long (value, self->lg); break;
    case PROP_U64: g_value_set_uint64 (value, self->u64); break;
    case PROP_ACT: g_value_set_object (value, NULL); break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
    }
}

static void
fz_obj_finalize (GObject *object)
{
  FzObj *self = (FzObj *) object;

  finalized_objs++;
  live_objs--;
  g_hash_table_remove (live_set, self);
  g_free (self->name);
  g_free (self->s);
  g_strfreev (self->tags);
  g_clear_weak_pointer (&self->friend);
  g_clear_pointer (&self->any, g_variant_unref);
  g_clear_pointer (&self->vs, g_variant_unref);
  g_ptr_array_unref (self->children);
  G_OBJECT_CLASS (fz_obj_parent_class)->finalize (object);
}

static void
fz_obj_class_init (FzObjClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  GParamFlags rw = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS;

  oc->set_property = fz_obj_set_property;
  oc->get_property = fz_obj_get_property;
  oc->finalize = fz_obj_finalize;

  g_object_class_install_property (oc, PROP_NAME, g_param_spec_string ("name", NULL, NULL, NULL, rw));
  g_object_class_install_property (oc, PROP_I, g_param_spec_int ("i", NULL, NULL, 0, 1000, 0, rw));
  g_object_class_install_property (oc, PROP_U, g_param_spec_uint ("u", NULL, NULL, 0, G_MAXUINT, 0, rw));
  g_object_class_install_property (oc, PROP_L, g_param_spec_int64 ("l", NULL, NULL, G_MININT64, G_MAXINT64, 0, rw));
  g_object_class_install_property (oc, PROP_D, g_param_spec_double ("d", NULL, NULL, -1000, 1000, 0, rw));
  g_object_class_install_property (oc, PROP_F, g_param_spec_float ("f", NULL, NULL, -1000, 1000, 0, rw));
  g_object_class_install_property (oc, PROP_B, g_param_spec_boolean ("b", NULL, NULL, FALSE, rw));
  g_object_class_install_property (oc, PROP_S, g_param_spec_string ("s", NULL, NULL, NULL, rw));
  g_object_class_install_property (oc, PROP_MODE, g_param_spec_enum ("mode", NULL, NULL, FZ_TYPE_MODE, FZ_MODE_SLOW, rw));
  g_object_class_install_property (oc, PROP_FLAGS, g_param_spec_flags ("flags", NULL, NULL, FZ_TYPE_FEATURES, FZ_FEATURE_NONE, rw));
  g_object_class_install_property (oc, PROP_TAGS, g_param_spec_boxed ("tags", NULL, NULL, G_TYPE_STRV, rw));
  g_object_class_install_property (oc, PROP_FRIEND, g_param_spec_object ("friend", NULL, NULL, FZ_TYPE_OBJ, rw));
  g_object_class_install_property (oc, PROP_ANY, g_param_spec_variant ("any", NULL, NULL, G_VARIANT_TYPE_ANY, NULL, rw));
  g_object_class_install_property (oc, PROP_VS, g_param_spec_variant ("vs", NULL, NULL, G_VARIANT_TYPE_STRING, NULL, rw));
  g_object_class_install_property (oc, PROP_RO, g_param_spec_string ("ro", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (oc, PROP_CO, g_param_spec_int ("co", NULL, NULL, 0, 100, 7, rw | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (oc, PROP_WO, g_param_spec_int ("wo", NULL, NULL, 0, 100, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (oc, PROP_PTR, g_param_spec_pointer ("ptr", NULL, NULL, rw));
  g_object_class_install_property (oc, PROP_CH, g_param_spec_char ("ch", NULL, NULL, -100, 100, 0, rw));
  g_object_class_install_property (oc, PROP_UC, g_param_spec_uchar ("uc", NULL, NULL, 0, 200, 0, rw));
  g_object_class_install_property (oc, PROP_UL, g_param_spec_ulong ("ul", NULL, NULL, 0, G_MAXULONG, 0, rw));
  g_object_class_install_property (oc, PROP_LG, g_param_spec_long ("lg", NULL, NULL, G_MINLONG, G_MAXLONG, 0, rw));
  g_object_class_install_property (oc, PROP_U64, g_param_spec_uint64 ("u64", NULL, NULL, 0, G_MAXUINT64, 0, rw));
  g_object_class_install_property (oc, PROP_ACT, g_param_spec_object ("act", NULL, NULL, G_TYPE_ACTION, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  obj_signals[SIG_DESTROY] = g_signal_new ("destroy", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                           0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  obj_signals[SIG_POKED] = g_signal_new ("poked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
fz_obj_init (FzObj *self)
{
  self->children = g_ptr_array_new_with_free_func (g_object_unref);
  live_objs++;
  g_hash_table_add (live_set, self);
}

/* The "really destroy" function: children first, then the destroy signal. */
static void
fz_obj_destroy (GObject *object)
{
  FzObj *self = (FzObj *) object;

  if (self->destroyed)
    {
      double_destroys++;
      return;
    }
  self->destroyed = TRUE;
  destroy_runs++;

  g_object_ref (self);
  while (self->children->len > 0)
    {
      FzObj *child = g_object_ref (g_ptr_array_index (self->children, self->children->len - 1));

      g_ptr_array_remove_index (self->children, self->children->len - 1);
      child->parent = NULL;
      fz_obj_destroy (G_OBJECT (child));
      g_object_unref (child);
    }
  g_string_append_printf (destroy_log, "%s,", self->name != NULL ? self->name : "?");
  if (self->parent != NULL)
    g_ptr_array_remove (self->parent->children, self);
  g_signal_emit (self, obj_signals[SIG_DESTROY], 0);
  g_object_unref (self);
}

static gboolean
fz_obj_contains (GObject *ancestor, GObject *descendant)
{
  FzObj *d = (FzObj *) descendant;

  for (; d != NULL; d = d->parent)
    if ((GObject *) d == ancestor)
      return TRUE;
  return FALSE;
}

static const ShellLuaOwnOps obj_ops = { fz_obj_destroy, "destroy" };

/* A second, unrelated type */
typedef struct { GObject parent_instance; } FzOther;
typedef struct { GObjectClass parent_class; } FzOtherClass;
GType fz_other_get_type (void) G_GNUC_CONST;
#define FZ_TYPE_OTHER (fz_other_get_type ())
G_DEFINE_TYPE (FzOther, fz_other, G_TYPE_OBJECT)
static void fz_other_class_init (FzOtherClass *klass) { }
static void fz_other_init (FzOther *self) { }

/* ------------------------------------------------------------------------ */
/* shell.fz                                                                  */
/* ------------------------------------------------------------------------ */

static GPtrArray *others;          /* unowned FzOther / FzObj kept alive for the scripts */
static GObject *shared;            /* weak, fz.share() / fz.take() */
static int later_flag;

static void
push_int_arg (lua_State *L, gpointer data)
{
  lua_pushinteger (L, GPOINTER_TO_INT (data));
}

static void
poked_cb (FzObj *obj, int n, ShellLuaCallback *cb)
{
  poked_count++;
  shell_lua_callback_emit (cb, 1, push_int_arg, GINT_TO_POINTER (n));
}

static const char * const new_fields[] = { "name", "parent", "on_poked", "co", NULL };

static int
l_new (lua_State *L)
{
  FzObj *obj, *parent = NULL;
  int ref;

  shell_lua_check_fields (L, 1, new_fields);
  shell_lua_check_owned_capacity (L);
  if (lua_getfield (L, 1, "parent") != LUA_TNIL)
    parent = (FzObj *) shell_lua_handle_check (L, -1, "fz.Obj");
  lua_pop (L, 1);
  ref = shell_lua_opt_callback (L, 1, "on_poked", FALSE);

  obj = g_object_new (FZ_TYPE_OBJ, "name", shell_lua_opt_string (L, 1, "name", "anon"),
                      "co", (int) shell_lua_opt_integer (L, 1, "co", 7), NULL);
  if (parent != NULL)
    {
      obj->parent = parent;
      g_ptr_array_add (parent->children, g_object_ref (obj));
    }
  shell_lua_own_object (L, G_OBJECT (obj), &obj_ops);
  g_object_unref (obj);   /* ours; the context holds one (and the parent) */
  if (ref != LUA_NOREF)
    {
      ShellLuaCallback *cb = shell_lua_callback_new (L, G_OBJECT (obj), ref);

      shell_lua_callback_connect (cb, obj, "poked", G_CALLBACK (poked_cb));
    }
  shell_lua_handle_push (L, "fz.Obj", G_OBJECT (obj));
  return 1;
}

/* An unowned object of the other type, kept alive until the next reset. */
static int
l_new_other (lua_State *L)
{
  GObject *obj = g_object_new (FZ_TYPE_OTHER, NULL);

  g_ptr_array_add (others, obj);
  shell_lua_handle_push (L, "fz.Other", obj);
  return 1;
}

/* An unowned FzObj kept alive until fz.drop(): its handle dies when dropped. */
static int
l_hold (lua_State *L)
{
  GObject *obj = g_object_new (FZ_TYPE_OBJ, "name", "held", NULL);

  g_ptr_array_add (others, obj);
  shell_lua_handle_push (L, "fz.Obj", obj);
  return 1;
}

static int
l_drop (lua_State *L)
{
  g_ptr_array_set_size (others, 0);
  return 0;
}

/* A handle that is dead from birth: the only reference to the object is
 * dropped right after the handle was made. */
static int
l_ephemeral (lua_State *L)
{
  GObject *obj = g_object_new (FZ_TYPE_OBJ, "name", "ephemeral", NULL);

  /* @others holds it should the push raise (out of memory) */
  g_ptr_array_add (others, obj);
  shell_lua_handle_push (L, "fz.Obj", obj);
  g_ptr_array_remove_index (others, others->len - 1);
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
l_mark (lua_State *L)
{
  g_string_append_printf (marks, "%s,", luaL_checkstring (L, 1));
  return 0;
}

static int
l_marks (lua_State *L)
{
  lua_pushstring (L, marks->str);
  g_string_truncate (marks, 0);
  return 1;
}

static int
l_stats (lua_State *L)
{
  lua_createtable (L, 0, 6);
  lua_pushinteger (L, finalized_objs);
  lua_setfield (L, -2, "finalized");
  lua_pushinteger (L, live_objs);
  lua_setfield (L, -2, "live");
  lua_pushinteger (L, destroy_runs);
  lua_setfield (L, -2, "destroyed");
  lua_pushinteger (L, double_destroys);
  lua_setfield (L, -2, "double");
  lua_pushinteger (L, poked_count);
  lua_setfield (L, -2, "poked");
  lua_pushinteger (L, hits);
  lua_setfield (L, -2, "hits");
  return 1;
}

static int
l_mem (lua_State *L)
{
  lua_pushinteger (L, (lua_Integer) shell_lua_state_get_memory_usage (shell_lua_state_from_L (L)));
  return 1;
}

static int
l_gc (lua_State *L)
{
  lua_gc (L, LUA_GCCOLLECT);
  return 0;
}

static int
l_error_count (lua_State *L)
{
  lua_pushinteger (L, shell_lua_state_get_error_count (shell_lua_state_from_L (L)));
  return 1;
}

static int
l_rand (lua_State *L)
{
  lua_Integer n = luaL_checkinteger (L, 1);

  luaL_argcheck (L, n >= 1 && n < G_MAXINT32, 1, "out of range");
  lua_pushinteger (L, g_rand_int_range (rng, 1, (gint32) n + 1));
  return 1;
}

static int
l_frand (lua_State *L)
{
  lua_pushnumber (L, g_rand_double (rng));
  return 1;
}

static int
l_trace (lua_State *L)
{
  if (fuzz_verbose)
    fprintf (stderr, "fuzz: %s\n", luaL_checkstring (L, 1));
  return 0;
}

static int
l_lud (lua_State *L)
{
  lua_pushlightuserdata (L, L);
  return 1;
}

/* fz.members(h): how many members foreach_member reports; never raises on
 * a dead handle. */
static void
count_member (const char *name, gboolean is_method, gpointer data)
{
  (*(int *) data)++;
}

static int
l_members (lua_State *L)
{
  int n = 0;

  shell_lua_handle_foreach_member (L, 1, count_member, &n);
  lua_pushinteger (L, n);
  return 1;
}

static void
list_member (const char *name, gboolean is_method, gpointer data)
{
  lua_State *L = data;

  lua_pushboolean (L, is_method);
  lua_setfield (L, -2, name);
}

/* fz.member_names(h): { name = is_method } for every member of the handle. */
static int
l_member_names (lua_State *L)
{
  lua_newtable (L);
  shell_lua_handle_foreach_member (L, 1, list_member, L);
  return 1;
}

static int
l_share (lua_State *L)
{
  g_set_weak_pointer (&shared, shell_lua_handle_check (L, 1, NULL));
  return 0;
}

static int
l_take (lua_State *L)
{
  if (shared == NULL)
    return luaL_error (L, "nothing is shared");
  shell_lua_handle_push (L, NULL, shared);
  return 1;
}

static int
l_invalidate (lua_State *L)
{
  GObject *obj = shell_lua_handle_check (L, 1, NULL);

  shell_lua_handle_invalidate (lua_toboolean (L, 2) ? NULL : L, obj);
  return 0;
}

/* fz.disable(): disable the extension that runs this code, right now, from
 * inside whatever callback is running. Returns ok, message. */
static int
l_disable (lua_State *L)
{
  ShellLuaExtension *ext = shell_lua_extension_from_L (L);
  g_autoptr (GError) error = NULL;
  gboolean ok;

  if (ext == NULL)
    {
      lua_pushboolean (L, 0);
      lua_pushliteral (L, "no extension");
      return 2;
    }
  ok = shell_lua_extension_disable (ext, &error);
  lua_pushboolean (L, ok);
  if (error != NULL)
    lua_pushstring (L, error->message);
  else
    lua_pushnil (L);
  return 2;
}

/* fz.disable_ext(uuid): disable another extension (of the test manager) from here. */
static ShellLuaExtensionManager *test_manager;

static int
l_disable_ext (lua_State *L)
{
  g_autoptr (GError) error = NULL;
  gboolean ok;

  if (test_manager == NULL)
    return luaL_error (L, "no manager");
  ok = shell_lua_extension_manager_disable (test_manager, luaL_checkstring (L, 1), &error);
  lua_pushboolean (L, ok);
  return 1;
}

/* fz.kill_state(): destroy the state that runs this code, from inside it. */
static int
l_kill_state (lua_State *L)
{
  shell_lua_state_destroy (shell_lua_state_from_L (L));
  return 0;
}

/* ---- hooks ---- */

static int
l_declare_raw (lua_State *L)
{
  shell_hook_registry_declare (shell_hook_registry_get_default (), luaL_checkstring (L, 1),
                               luaL_checkstring (L, 2), lua_toboolean (L, 3), "fuzz hook");
  return 0;
}

static int
l_declare (lua_State *L)
{
  const char *name = luaL_checkstring (L, 1);

  /* a name the registry would refuse is a Lua error here, not a critical */
  if (!g_regex_match_simple ("^[a-z][a-z0-9-]*(\\.[a-z][a-z0-9-]*)+$", name, 0, 0))
    return luaL_error (L, "invalid hook name '%s'", name);
  shell_hook_registry_declare (shell_hook_registry_get_default (), name, "k:s",
                               lua_toboolean (L, 2), "fuzz hook");
  return 0;
}

static int
l_is_declared (lua_State *L)
{
  lua_pushboolean (L, shell_hook_registry_is_declared (shell_hook_registry_get_default (),
                                                       luaL_checkstring (L, 1)));
  return 1;
}

static GVariant *
payload_arg (lua_State *L)
{
  if (lua_isnoneornil (L, 2))
    return NULL;
  luaL_checktype (L, 2, LUA_TTABLE);
  return shell_lua_to_variant (L, 2, G_VARIANT_TYPE_VARDICT);
}

static int
l_emit_after (lua_State *L)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  const char *name = luaL_checkstring (L, 1);
  GVariant *payload;

  if (!shell_hook_registry_is_declared (registry, name))
    return luaL_error (L, "unknown hook '%s'", name);
  payload = payload_arg (L);
  shell_hook_registry_emit_after (registry, name, payload);
  if (payload != NULL)
    g_variant_unref (payload);
  return 0;
}

static int
l_emit_before (lua_State *L)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  const char *name = luaL_checkstring (L, 1);
  GVariant *payload, *amendments = NULL;
  ShellHookVerdict verdict;

  if (!shell_hook_registry_is_declared (registry, name))
    return luaL_error (L, "unknown hook '%s'", name);
  if (!shell_hook_registry_is_cancellable (registry, name))
    return luaL_error (L, "hook '%s' is not cancellable", name);
  payload = payload_arg (L);
  verdict = shell_hook_registry_emit_before (registry, name, payload, &amendments);
  if (payload != NULL)
    g_variant_unref (payload);
  lua_pushboolean (L, verdict == SHELL_HOOK_VERDICT_CANCEL);
  if (amendments != NULL)
    {
      shell_lua_push_variant (L, amendments);
      g_variant_unref (amendments);
    }
  else
    {
      lua_pushnil (L);
    }
  return 2;
}

static int
l_listeners (lua_State *L)
{
  const char *name = luaL_checkstring (L, 1);
  int phase = luaL_checkoption (L, 2, "after", (const char *[]) { "before", "after", NULL });

  lua_pushinteger (L, shell_hook_registry_count_listeners (shell_hook_registry_get_default (), name,
                                                           phase == 0 ? SHELL_HOOK_PHASE_BEFORE
                                                                      : SHELL_HOOK_PHASE_AFTER));
  return 1;
}

/* ---- handle type fz.Obj ---- */

static int
l_obj_poke (lua_State *L)
{
  GObject *obj = shell_lua_handle_check (L, 1, "fz.Obj");

  g_signal_emit (obj, obj_signals[SIG_POKED], 0, (int) luaL_optinteger (L, 2, 0));
  return 0;
}

static int
l_obj_on_poked (lua_State *L)
{
  GObject *obj = shell_lua_handle_check (L, 1, "fz.Obj");
  ShellLuaCallback *cb;
  gulong id;

  luaL_checktype (L, 2, LUA_TFUNCTION);
  lua_pushvalue (L, 2);
  cb = shell_lua_callback_new (L, obj, luaL_ref (L, LUA_REGISTRYINDEX));
  id = shell_lua_callback_connect (cb, obj, "poked", G_CALLBACK (poked_cb));
  return shell_lua_callback_push_connection (L, cb, obj, id);
}

static int
l_obj_external_destroy (lua_State *L)
{
  fz_obj_destroy (shell_lua_handle_check (L, 1, "fz.Obj"));
  return 0;
}

static int
l_obj_plain (lua_State *L)
{
  shell_lua_handle_check (L, 1, "fz.Obj");
  lua_pushliteral (L, "plain");
  return 1;
}

static int
l_obj_deadok (lua_State *L)
{
  GObject *obj = shell_lua_handle_get (L, 1, "fz.Obj");

  lua_pushstring (L, obj != NULL ? "alive" : "dead");
  return 1;
}

static int
l_obj_sub_only (lua_State *L)
{
  shell_lua_handle_check (L, 1, "fz.Sub");
  lua_pushliteral (L, "sub");
  return 1;
}

static void later_fn (gpointer data) { later_flag++; }

static int
l_obj_defer (lua_State *L)
{
  GObject *obj = shell_lua_handle_check (L, 1, "fz.Obj");

  lua_pushboolean (L, shell_lua_defer_if_emitting (L, obj, later_fn, NULL, NULL));
  return 1;
}

static int
l_later_flag (lua_State *L)
{
  lua_pushinteger (L, later_flag);
  return 1;
}

static int
l_other_ping (lua_State *L)
{
  shell_lua_handle_check (L, 1, "fz.Other");
  lua_pushliteral (L, "pong");
  return 1;
}

static void
virt_get_double (lua_State *L, GObject *object)
{
  lua_pushinteger (L, ((FzObj *) object)->i * 2);
}

static void
virt_set_double (lua_State *L, GObject *object, int idx)
{
  lua_Integer v = luaL_checkinteger (L, idx);

  if (v < 0 || v % 2 || v > 2000)
    luaL_error (L, "double must be an even integer in 0..2000");
  g_object_set (object, "i", (int) (v / 2), NULL);
}

static void
virt_get_label (lua_State *L, GObject *object)
{
  lua_pushstring (L, ((FzObj *) object)->name);
}

static void
virt_get_boom (lua_State *L, GObject *object)
{
  luaL_error (L, "boom-get");
}

static void
virt_set_boom (lua_State *L, GObject *object, int idx)
{
  /* provoke a callback first, then raise: the callback has run */
  g_object_set (object, "i", 3, NULL);
  luaL_error (L, "boom-set");
}

static const luaL_Reg obj_methods[] = {
  { "poke", l_obj_poke },
  { "on_poked", l_obj_on_poked },
  { "plain", l_obj_plain },
  { "external_destroy", l_obj_external_destroy },
  { "defer", l_obj_defer },
  { NULL, NULL }
};

static const ShellLuaHandleMethod obj_methods_ex[] = {
  { "destroy", shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { "deadok", l_obj_deadok, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty obj_properties[] = {
  { "name", NULL, TRUE, TRUE },
  { "i", NULL, TRUE, TRUE },
  { "u", NULL, TRUE, TRUE },
  { "l", NULL, TRUE, TRUE },
  { "d", NULL, TRUE, TRUE },
  { "f", NULL, TRUE, TRUE },
  { "b", NULL, TRUE, TRUE },
  { "s", NULL, TRUE, TRUE },
  { "mode", NULL, TRUE, TRUE },
  { "flags", NULL, TRUE, TRUE },
  { "tags", NULL, TRUE, TRUE },
  { "friend", NULL, TRUE, TRUE },
  { "any", NULL, TRUE, TRUE },
  { "vs", NULL, TRUE, TRUE },
  { "ro", NULL, TRUE, TRUE },            /* the table says rw, the GObject property is read-only */
  { "co", NULL, TRUE, TRUE },            /* ... construct-only */
  { "wo", NULL, TRUE, TRUE },            /* ... write-only */
  { "ptr", NULL, TRUE, TRUE },           /* a type the marshalling does not support */
  { "ch", NULL, TRUE, TRUE },
  { "uc", NULL, TRUE, TRUE },
  { "ul", NULL, TRUE, TRUE },
  { "lg", NULL, TRUE, TRUE },
  { "u64", NULL, TRUE, TRUE },
  { "act", NULL, TRUE, TRUE },           /* an interface typed object, never writable in practice */
  { "ghost", NULL, TRUE, TRUE },         /* no such GObject property */
  { "alias", "i", TRUE, TRUE },
  { "hidden", "i", FALSE, FALSE },       /* neither readable nor writable */
  { NULL }
};

static const ShellLuaHandleVirtual obj_virtuals[] = {
  { "double", virt_get_double, virt_set_double },
  { "label", virt_get_label, NULL },
  { "boom", virt_get_boom, virt_set_boom },
  { NULL }
};

static ShellLuaHandleType obj_type = {
  .name = "fz.Obj",
  .methods = obj_methods,
  .methods_ex = obj_methods_ex,
  .properties = obj_properties,
  .virtuals = obj_virtuals,
};

static const luaL_Reg sub_methods[] = { { "sub_only", l_obj_sub_only }, { NULL, NULL } };
static ShellLuaHandleType sub_type = { .name = "fz.Sub", .parent = "fz.Obj", .methods = sub_methods };

static const luaL_Reg other_methods[] = { { "ping", l_other_ping }, { NULL, NULL } };
static ShellLuaHandleType other_type = { .name = "fz.Other", .methods = other_methods };

static int
l_new_sub (lua_State *L)
{
  FzObj *obj;

  shell_lua_check_owned_capacity (L);
  obj = g_object_new (FZ_TYPE_OBJ, "name", "sub", NULL);
  shell_lua_own_object (L, G_OBJECT (obj), &obj_ops);
  g_object_unref (obj);
  shell_lua_handle_push (L, "fz.Sub", G_OBJECT (obj));
  return 1;
}

static void
fz_register (ShellLuaState *state, lua_State *L)
{
  static const luaL_Reg funcs[] = {
    { "new", l_new }, { "new_sub", l_new_sub }, { "new_other", l_new_other }, { "hold", l_hold },
    { "drop", l_drop }, { "ephemeral", l_ephemeral }, { "count", l_count }, { "log", l_log },
    { "hit", l_hit }, { "hits", l_hits }, { "mark", l_mark }, { "marks", l_marks },
    { "stats", l_stats }, { "mem", l_mem }, { "gc", l_gc }, { "error_count", l_error_count }, { "rand", l_rand },
    { "frand", l_frand }, { "trace", l_trace }, { "lud", l_lud }, { "members", l_members }, { "member_names", l_member_names },
    { "share", l_share }, { "take", l_take }, { "invalidate", l_invalidate }, { "disable", l_disable }, { "kill_state", l_kill_state }, { "disable_ext", l_disable_ext },
    { "declare", l_declare }, { "declare_raw", l_declare_raw }, { "is_declared", l_is_declared },
    { "emit_after", l_emit_after }, { "emit_before", l_emit_before }, { "listeners", l_listeners },
    { "later_flag", l_later_flag },
    { NULL, NULL }
  };

  obj_type.gtype = FZ_TYPE_OBJ;
  sub_type.gtype = FZ_TYPE_OBJ;
  other_type.gtype = FZ_TYPE_OTHER;
  shell_lua_handle_register_type (L, &obj_type);
  shell_lua_handle_register_type (L, &sub_type);
  shell_lua_handle_register_type (L, &other_type);

  luaL_newlib (L, funcs);
  lua_setfield (L, -2, "fz");
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

#define PRELUDE "local shell = require('shell'); local fz = shell.fz\n"

static guint base_states;
static gsize base_memory;

static void
fixture_begin (guint32 salt)
{
  rng_reset (salt);
  g_string_truncate (destroy_log, 0);
  g_string_truncate (marks, 0);
  hits = 0;
  later_flag = 0;
  poked_count = 0;
  finalized_objs = 0;
  destroy_runs = 0;
  double_destroys = 0;
  g_assert_cmpint (live_objs, ==, 0);
  base_states = shell_lua_get_state_count ();
  base_memory = shell_lua_get_total_memory ();
  shell_lua_set_contains_func (fz_obj_contains);
}

/* Every state is gone, every object finalized, every byte given back. */
static void
fixture_end (void)
{
  shell_lua_set_contains_func (NULL);
  g_ptr_array_set_size (others, 0);
  g_clear_weak_pointer (&shared);
  if (live_objs != 0)
    {
      GHashTableIter iter;
      gpointer key;

      g_hash_table_iter_init (&iter, live_set);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        g_printerr ("leaked FzObj %p '%s' ref_count %u destroyed %d\n", key, ((FzObj *) key)->name,
                    ((GObject *) key)->ref_count, ((FzObj *) key)->destroyed);
    }
  g_assert_cmpint (live_objs, ==, 0);
  g_assert_cmpint (double_destroys, ==, 0);
  g_assert_cmpuint (shell_lua_get_state_count (), ==, base_states);
  g_assert_cmpuint (shell_lua_get_total_memory (), ==, base_memory);
}

static ShellLuaState *
new_priv (void)
{
  return tu_state_new ("fuzz", SHELL_LUA_STATE_PRIVILEGED);
}

/* Run @code; it must succeed. */
#define DO(state, code) tu_do ((state), PRELUDE code)

/* The state must be usable: a callback fires for an assignment and for a
 * poke. */
static void
assert_state_usable (ShellLuaState *s)
{
  tu_do (s, PRELUDE
         "local n = 0\n"
         "local o = fz.new{name='probe', on_poked = function() n = n + 1 end}\n"
         "o.i = 4\n"
         "assert(n == 1, 'no callback for an assignment')\n"
         "o:poke(1)\n"
         "assert(n == 2, 'no callback for a poke')\n"
         "o:destroy()\n"
         "assert(not o.alive)\n"
         "assert(fz.count() == fz.count())\n");
}

/* ---- extensions ---- */

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
  test_manager = e->manager;
}

static void
env_clear (Env *e)
{
  test_manager = NULL;
  g_object_unref (e->manager);
  tu_rm_rf (e->tmp);
  g_free (e->tmp);
  g_free (e->user);
}

/* Writes and scans <uuid>; @extra is spliced into metadata.json ("" or ', "capabilities": [...]'). */
static ShellLuaExtension *
install (Env *e, const char *uuid, const char *extra, const char *lua)
{
  g_autofree char *dir = g_build_filename (e->user, uuid, NULL);
  g_autofree char *metadata = g_strdup_printf (
    "{\"uuid\": \"%s\", \"name\": \"Fuzz\", \"shell-api\": \"1\"%s}", uuid, extra != NULL ? extra : "");

  tu_write_file (dir, "metadata.json", metadata);
  tu_write_file (dir, "extension.lua", lua);
  shell_lua_extension_manager_scan (e->manager);
  return g_object_ref (shell_lua_extension_manager_lookup (e->manager, uuid));
}

static void
enable_ok (Env *e, const char *uuid)
{
  g_autoptr (GError) error = NULL;

  if (!shell_lua_extension_manager_enable (e->manager, uuid, &error))
    g_error ("enabling %s failed: %s", uuid, error->message);
}

static void
set_budget (Env *e, gint64 budget_us, gsize memory)
{
  ShellLuaLimits limits = { .callback_budget_us = budget_us, .memory_limit = memory };

  shell_lua_extension_manager_set_limits (e->manager, &limits);
}

typedef struct {
  ShellLuaExtension      *ext;
  ShellLuaExtensionState  state;
} StateWait;

static gboolean
pred_state (gpointer data)
{
  StateWait *w = data;

  return shell_lua_extension_get_state (w->ext) == w->state;
}

static void
wait_for_state (ShellLuaExtension *ext, ShellLuaExtensionState state)
{
  StateWait w = { ext, state };

  if (!tu_wait_for (pred_state, &w, 5000))
    g_error ("%s: timed out waiting for state %s (is %s)", shell_lua_extension_get_uuid (ext),
             shell_lua_extension_state_to_string (state),
             shell_lua_extension_state_to_string (shell_lua_extension_get_state (ext)));
}

static void
assert_state (ShellLuaExtension *ext, ShellLuaExtensionState state)
{
  if (shell_lua_extension_get_state (ext) != state)
    g_error ("%s: state is %s, expected %s", shell_lua_extension_get_uuid (ext),
             shell_lua_extension_state_to_string (shell_lua_extension_get_state (ext)),
             shell_lua_extension_state_to_string (state));
}

static void
declare_hooks (void)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();

  shell_hook_registry_declare (registry, "fz.alpha", "k:s", FALSE, "fuzz");
  shell_hook_registry_declare (registry, "fz.beta", "k:s", TRUE, "fuzz");
  shell_hook_registry_declare (registry, "fz.gamma", "k:s", FALSE, "fuzz");
}

static guint
listeners (const char *name, ShellHookPhase phase)
{
  return shell_hook_registry_count_listeners (shell_hook_registry_get_default (), name, phase);
}

static void
assert_no_listeners (void)
{
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 0);
  g_assert_cmpuint (listeners ("fz.beta", SHELL_HOOK_PHASE_AFTER), ==, 0);
  g_assert_cmpuint (listeners ("fz.beta", SHELL_HOOK_PHASE_BEFORE), ==, 0);
  g_assert_cmpuint (listeners ("fz.gamma", SHELL_HOOK_PHASE_AFTER), ==, 0);
}

/* ======================================================================== */
/* 1. Malformed handles                                                      */
/* ======================================================================== */

/* Methods and metamethods called with the wrong self. The metamethods are
 * reached through debug.getmetatable(), which only the privileged state has. */
static void
test_handles_wrong_self (void)
{
  ShellLuaState *s;

  fixture_begin (1);
  s = new_priv ();
  DO (s,
     "local o = fz.new{name='live'}\n"
     "local sub = fz.new_sub()\n"
     "local other = fz.new_other()\n"
     "local dead = fz.new{name='dead'}; dead:destroy()\n"
     "local mt = debug.getmetatable(o)\n"
     "assert(type(mt) == 'table', 'the privileged state can see the metatable')\n"
     "assert(getmetatable(o) == false, 'but getmetatable() honours __metatable')\n"
     "assert(not pcall(setmetatable, o, {}), 'a handle cannot get a new metatable')\n"
     "local fake = setmetatable({}, mt)\n"        /* a table wearing a handle's metatable */
     "local selves = { true, false, 0, -1, 1.5, 2^63, 'str', '', {}, fake, print, coroutine.create(print),\n"
     "                 other, dead, fz.lud(), shell.hooks.CANCEL, io.stdout, shell, shell.api_version }\n"
     "assert(#selves == 19)\n"
     "local function classify(err)\n"
     "  err = tostring(err)\n"
     "  if err:find('no longer valid', 1, true) then return 'dead' end\n"
     "  if err:find('expected, got', 1, true) then return 'type' end\n"
     "  if err:find('not a handle', 1, true) then return 'not-a-handle' end\n"
     "  if err:find('cannot be destroyed', 1, true) then return 'cannot-destroy' end\n"
     "  return 'other: ' .. err\n"
     "end\n"
     /* ordinary methods: wrong self is a type error, a dead self a dead-handle error */
     "for _, name in ipairs{'poke', 'on_poked', 'plain', 'external_destroy', 'defer'} do\n"
     "  local f = o[name]\n"
     "  assert(type(f) == 'function', name)\n"
     "  for i, self in ipairs(selves) do\n"
     "    local ok, err = pcall(f, self, function() end)\n"
     "    assert(not ok, name .. ' accepted self #' .. i)\n"
     "    local kind = classify(err)\n"
     "    assert(kind == (self == dead and 'dead' or 'type'), name .. ' self #' .. i .. ': ' .. kind)\n"
     "  end\n"
     "  local ok, err = pcall(f)\n"
     "  assert(not ok and classify(err) == 'type', name .. '(): ' .. tostring(err))\n"
     "  ok, err = pcall(f, nil)\n"
     "  assert(not ok and classify(err) == 'type', name .. '(nil): ' .. tostring(err))\n"
     "end\n"
     /* the DEAD_OK methods: destroy() takes any handle (it raises for objects the context does not own),
      * deadok() wants an fz.Obj; neither raises for a dead one */
     "for i, self in ipairs(selves) do\n"
     "  local ok, err = pcall(o.deadok, self)\n"
     "  if self == dead then assert(ok and err == 'dead')\n"
     "  else assert(not ok and classify(err) == 'type', 'deadok self #' .. i .. ': ' .. tostring(err)) end\n"
     "  ok, err = pcall(o.destroy, self)\n"
     "  if self == dead then assert(ok, tostring(err))\n"
     "  elseif self == other then assert(not ok and classify(err) == 'cannot-destroy', tostring(err))\n"
     "  else assert(not ok and classify(err) == 'type', 'destroy self #' .. i .. ': ' .. tostring(err)) end\n"
     "end\n"
     "assert(not pcall(sub.sub_only, o) and select(2, pcall(sub.sub_only, o)):find('fz.Sub expected', 1, true))\n"
     "assert(sub:sub_only() == 'sub' and o:plain() == 'plain' and fz.count() == 2)\n"
     "assert(o.plain(sub) == 'plain' and pcall(o.poke, sub, 1) and sub:deadok() == 'alive', 'a subtype is a fine self')\n"
     /* the metamethods */
     "local keys = { 'i', 'alive', 'plain', 'nope', '', 1, 1.5, true, false, {}, print, o, dead }\n"
     "for i, self in ipairs(selves) do\n"
     "  for _, key in ipairs(keys) do\n"
     "    local ok, err = pcall(mt.__index, self, key)\n"
     "    if self == dead and key == 'alive' then assert(ok and err == false)\n"
     "    elseif self == other and key == 'alive' then assert(ok and err == true)\n"
     "    elseif self == other and type(key) ~= 'string' then assert(ok and err == nil)\n"
     "    elseif self == other then assert(ok or classify(err) ~= 'other')\n"
     "    elseif self == dead then assert(not ok and classify(err) == 'dead')\n"
     "    else assert(not ok and classify(err) == 'not-a-handle', 'index self #' .. i .. ' key ' .. tostring(key)) end\n"
     "    ok, err = pcall(mt.__newindex, self, key, 1)\n"
     "    assert(not ok, 'newindex self #' .. i .. ' key ' .. tostring(key))\n"
     "    local want = self == dead and 'dead' or (self == other and 'other' or 'not-a-handle')\n"
     "    local kind = classify(err)\n"
     "    assert(kind:sub(1, #want) == want, 'newindex self #' .. i .. ': ' .. kind)\n"
     "  end\n"
     "  assert(select(2, pcall(mt.__tostring, self)) == (self == dead and '<fz.Obj (dead)>' or\n"
     "         self == other and tostring(other) or '<invalid handle>'), 'tostring self #' .. i)\n"
     "  for _, b in ipairs(selves) do\n"
     "    local ok, eq = pcall(mt.__eq, self, b)\n"
     "    assert(ok and type(eq) == 'boolean', '__eq')\n"
     "    assert(not eq or (self == b and (self == dead or self == other)), '__eq of two different things is true')\n"
     "  end\n"
     "  assert(pcall(mt.__eq, self))\n"
     "  assert(pcall(mt.__eq))\n"
     "end\n"
     "assert(not pcall(mt.__index) and not pcall(mt.__newindex) and select(2, pcall(mt.__index)):find('not a handle'))\n"
     "assert(pcall(mt.__tostring))\n"
     "assert(pcall(mt.__gc) and pcall(mt.__gc, nil) and pcall(mt.__gc, {}) and pcall(mt.__gc, 'x') and pcall(mt.__gc, fake))\n"
     "assert(pcall(mt.__gc, io.stdout) == true and io.stdout:write('') ~= nil, 'a foreign userdata is not a handle')\n"
     /* the live handles are untouched by all of the above */
     "assert(o.alive and sub.alive and other.alive and not dead.alive)\n"
     "assert(o == o and o ~= sub and o ~= other and o ~= 1 and o ~= 'x' and dead == dead and dead ~= o)\n"
     "assert(fz.count() == 2)\n"
     /* __gc on a live handle only kills the handle, never the object */
     "local g = fz.new{name='gc'}\n"
     "mt.__gc(g)\n"
     "assert(not g.alive and fz.count() == 3 and fz.take == fz.take)\n"
     "assert(not pcall(function() return g.i end))\n"
     "mt.__gc(g); mt.__gc(g)\n"
     "assert(fz.count() == 3)\n");
  g_object_unref (s);
  fixture_end ();
}

/* Reading and writing garbage keys; handles as keys and values. */
static void
test_handles_garbage_keys (void)
{
  ShellLuaState *s;

  fixture_begin (2);
  s = new_priv ();
  DO (s,
     "local o = fz.new{name='k'}\n"
     "local other = fz.new_other()\n"
     "local big = string.rep('x', 1 << 20)\n"
     "local keys = { '', ' ', '\\0', 'alive\\0junk', 'i\\0junk', '__index', '__newindex', '__gc', '__eq', '__name',\n"
     "  '__metatable', '__call', '__close', '__len', '__tostring', 'destroy ', ' destroy', 'DESTROY', 'Alive',\n"
     "  'caf\\u{e9}', '\\xff\\xfe', big, 0, 1, -1, 2^53, 2^63, -2^63, 1.5, 0.0, -0.0, 1/0, -1/0,\n"
     "  true, false, {}, {1}, print, coroutine.create(print), o, other, fz.lud(), io.stdout, string.rep('a', 4096) }\n"
     "local n_keys = #keys\n"
     "for i, k in ipairs(keys) do\n"
     "  local ok, v = pcall(function() return o[k] end)\n"
     "  if k == 'alive' then assert(ok and v == true) end\n"
     "  assert(ok, 'reading key #' .. i .. ' raised: ' .. tostring(v))\n"
     "  if type(k) == 'string' and not k:find('\\0', 1, true) and k ~= 'alive' then\n"
     "    assert(v == nil or o[k] == v, 'key #' .. i)\n"
     "  end\n"
     "  local ok2, err = pcall(function() o[k] = 1 end)\n"
     "  assert(not ok2, 'writing key #' .. i .. ' did not raise')\n"
     "  assert(tostring(err):find('no property', 1, true) or tostring(err):find('read%-only')\n"
     "         or tostring(err):find('read-only', 1, true) or tostring(err):find('bad value', 1, true), tostring(err))\n"
     "end\n"
     /* unknown names read as nil (feature detection), writes raise naming the property */
     "assert(o.nope == nil and o['no.such'] == nil and o.i == 0)\n"
     "local ok, err = pcall(function() o.nope = 1 end)\n"
     "assert(not ok and err:find(\"no property 'nope' on fz.Obj\", 1, true), err)\n"
     "ok, err = pcall(function() o[true] = 1 end)\n"
     "assert(not ok and err:find('no property of type boolean on fz.Obj', 1, true), err)\n"
     /* the same for the option tables of constructors: "ms\0x" is not the field ms */
     "local cb = function() end\n"
     "for _, bad in ipairs{ 'ms\\0x', 'callback\\0', 'name\\0junk' } do\n"
     "  local ok, err = pcall(shell.timer.after, { ms = 1, callback = cb, [bad] = 2 })\n"
     "  assert(not ok and err:find('unknown field', 1, true), 'timer.after accepted the field ' .. ('%q'):format(bad))\n"
     "  ok, err = pcall(fz.new, { [bad] = 2 })\n"
     "  assert(not ok and err:find('unknown field', 1, true), 'fz.new accepted the field ' .. ('%q'):format(bad))\n"
     "end\n"
     /* rawset/rawget cannot reach userdata */
     "assert(not pcall(rawset, o, 'i', 3) and not pcall(rawget, o, 'i'))\n"
     "assert(rawequal(o, o) and not rawequal(o, other) and rawlen ~= nil and not pcall(rawlen, o))\n"
     /* a handle as a table key is fine and keeps identity */
     "local t = { [o] = 1, [other] = 2 }\n"
     "assert(t[o] == 1 and t[fz.take and o] == 1 and t[other] == 2)\n"
     /* pairs / ipairs / # / calls / arithmetic / concatenation / comparison on a handle */
     "assert(not pcall(pairs, o) or true)\n"
     "assert(not pcall(function() return #o end) and not pcall(function() return o() end)\n"
     "       and not pcall(function() return o + 1 end) and not pcall(function() return o .. 'x' end)\n"
     "       and not pcall(function() return o < o end) and not pcall(function() return -o end)\n"
     "       and not pcall(function() for _ in pairs(o) do end end) and not pcall(function() return o.plain() end))\n"
     "assert(o == o and tostring(o):find('^<fz%.Obj 0x') ~= nil)\n"
     "assert(o.alive and fz.count() == 1)\n");
  g_object_unref (s);
  fixture_end ();
}

/* Every kind of Lua value assigned to every kind of property. */
static void
test_handles_assignment_matrix (void)
{
  ShellLuaState *s;

  fixture_begin (3);
  s = new_priv ();
  DO (s,
     "local hits_in_callback = 0\n"
     "local o = fz.new{name='m', on_poked = function() hits_in_callback = hits_in_callback + 1 end}\n"
     "local friend = fz.new{name='friend'}\n"
     "local other = fz.new_other()\n"
     "local dead = fz.new{name='dead'}; dead:destroy()\n"
     "local co = coroutine.create(print)\n"
     "local values = {\n"
     "  nil, true, false, 0, 1, -1, 7, 100, 101, 255, 1000, 1001, 2^31, 2^31 - 1, -2^31, 2^32, 2^53, 2^63,\n"
     "  math.maxinteger, math.mininteger, 1.5, -1.5, 0.0, -0.0, 1e300, 1/0, -1/0, 0/0, '', 'x', 'slow', 'fast',\n"
     "  'a', 'b', 'a,b', string.rep('s', 70000), 'caf\\u{e9}',\n"
     "  {}, {1, 2}, {'a'}, {'a', 'b'}, {a = true}, {a = true, b = true}, {a = false}, {nope = true}, {1, {}},\n"
     "  { 'a', 5 }, setmetatable({}, { __index = function() error('index') end }), { [1.5] = 'a' },\n"
     "  print, function() end, co, o, friend, other, dead, fz.lud(), shell.hooks.CANCEL, io.stdout }\n"
     "local n_values = 62\n"
     "local props = { 'name', 'i', 'u', 'l', 'd', 'f', 'b', 's', 'mode', 'flags', 'tags', 'friend', 'any', 'vs',\n"
     "  'ro', 'co', 'wo', 'ptr', 'ch', 'uc', 'ul', 'lg', 'u64', 'act', 'ghost', 'alias', 'hidden', 'alive',\n"
     "  'double', 'label', 'boom', 'nonexistent' }\n"
     "local documented = { 'bad value for property', 'value out of range', 'is read%-only', 'no property',\n"
     "  'not readable', 'no longer valid', 'double must be', 'boom%-set', 'not supported', 'handle given where',\n"
     "  'expected, got', 'field value', 'nested too deeply', 'cannot infer', 'cannot convert', 'bad argument', 'boom%-get' }\n"
     "local function check_message(err, prop, value)\n"
     "  err = tostring(err)\n"
     "  for _, pat in ipairs(documented) do if err:find(pat) then return end end\n"
     "  error('undocumented error for ' .. prop .. ' = ' .. tostring(value) .. ': ' .. err)\n"
     "end\n"
     "local accepted, rejected = 0, 0\n"
     "for _, prop in ipairs(props) do\n"
     "  for vi = 1, n_values do\n"
     "    local value = values[vi]\n"
     "    local ok, err = pcall(function() o[prop] = value end)\n"
     "    if ok then accepted = accepted + 1 else rejected = rejected + 1; check_message(err, prop, value) end\n"
     "    assert(o.alive and friend.alive and fz.count() == 2, 'objects changed by ' .. prop .. ' = ' .. tostring(value))\n"
     "    if ok and prop == 'ghost' or ok and prop == 'hidden' or ok and prop == 'nonexistent' or ok and prop == 'alive'\n"
     "       or ok and prop == 'ro' or ok and prop == 'co' or ok and prop == 'label' then\n"
     "      error(prop .. ' accepted ' .. tostring(value))\n"
     "    end\n"
     "  end\n"
     /* reading back never crashes; write-only and unsupported types raise */
     "  local ok, err = pcall(function() return o[prop] end)\n"
     "  if not ok then check_message(err, prop, '(read)') end\n"
     "end\n"
     "assert(accepted > 100 and rejected > 500, accepted .. '/' .. rejected)\n"
     /* rule 7: the assignments that set i, directly or through a virtual,
      * called the poked callback back */
     "assert(hits_in_callback > 0, 'no assignment called back')\n"
     "local hits = hits_in_callback\n"
     "o:poke(1)\n"
     "assert(hits_in_callback == hits + 1)\n"
     /* specific outcomes */
     "o.i = 5; assert(o.i == 5 and o.alias == 5 and o.double == 10)\n"
     "o.alias = 6; assert(o.i == 6)\n"
     "assert(not pcall(function() o.i = 1001 end) and o.i == 6)\n"
     "assert(not pcall(function() o.i = 1.5 end) and not pcall(function() o.i = '7' end))\n"
     "o.mode = 'fast'; assert(o.mode == 'fast')\n"
     "o.mode = 0; assert(o.mode == 'slow')\n"
     "assert(not pcall(function() o.mode = 'nope' end) and not pcall(function() o.mode = 9 end))\n"
     "o.flags = {a = true, b = true}; assert(o.flags.a and o.flags.b)\n"
     "o.flags = {'a'}; assert(o.flags.a and not o.flags.b)\n"
     "o.flags = ''; assert(next(o.flags) == nil)\n"
     "o.tags = {'x', 'y'}; assert(#o.tags == 2 and o.tags[2] == 'y')\n"
     "o.tags = {}; assert(#o.tags == 0)\n"
     "o.friend = friend; assert(o.friend == friend)\n"
     "o.friend = o; assert(o.friend == o)\n"
     "o.friend = nil; assert(o.friend == nil)\n"
     "assert(select(2, pcall(function() o.friend = other end)):find('FzObj', 1, true))\n"
     "assert(select(2, pcall(function() o.friend = dead end)):find('no longer valid', 1, true))\n"
     "assert(select(2, pcall(function() o.ptr = 1 end)):find(\"properties of type 'gpointer' are not supported\", 1, true))\n"
     "assert(select(2, pcall(function() return o.ptr end)):find(\"cannot convert property 'ptr'\", 1, true))\n"
     "o.wo = 5\n"
     "assert(select(2, pcall(function() return o.wo end)):find(\"property 'wo' of fz.Obj is not readable\", 1, true))\n"
     "assert(select(2, pcall(function() o.ro = 'x' end)):find(\"property 'ro' on fz.Obj is read-only\", 1, true))\n"
     "assert(select(2, pcall(function() o.co = 1 end)):find(\"property 'co' on fz.Obj is read-only\", 1, true))\n"
     "assert(select(2, pcall(function() o.hidden = 1 end)):find(\"property 'hidden' on fz.Obj is read-only\", 1, true))\n"
     "assert(o.hidden == nil)\n"
     "assert(select(2, pcall(function() return o.ghost end)):find(\"property 'ghost' of fz.Obj is not readable\", 1, true))\n"
     "hits = hits_in_callback\n"
     "assert(select(2, pcall(function() o.boom = 1 end)):find('boom-set', 1, true))\n"
     "assert(select(2, pcall(function() return o.boom end)):find('boom-get', 1, true))\n"
     "assert(hits_in_callback == hits + 1, 'the change a raising virtual setter made did not call back')\n"
     "o:poke(2); assert(hits_in_callback == hits + 2)\n"
     "o.any = 5; assert(o.any == 5)\n"
     "o.any = {1, 2, 3}; assert(#o.any == 3)\n"
     "assert(not pcall(function() o.any = nil end), 'a variant cannot be inferred from nil')\n"
     "o.vs = 'text'; assert(o.vs == 'text')\n"
     "assert(not pcall(function() o.vs = 5 end) and o.vs == 'text')\n");
  assert_state_usable (s);
  g_object_unref (s);
  fixture_end ();
}

/* Every method of every handle type called on every kind of handle and
 * non-handle, with arguments of every kind: only Lua errors may come out. */
static void
test_handles_cross_type_calls (void)
{
  ShellLuaState *s;
  g_autoptr (GSettings) settings = g_settings_new (TEST_SCHEMA);

  fixture_begin (6);
  declare_hooks ();
  s = new_priv ();
  DO (s,
     "local dead = fz.new{name='dead'}; dead:destroy()\n"
     "local handles = {\n"
     "  shell.timer.after{ms = 100000, callback = function() end},\n"
     "  shell.timer.every{ms = 100000, callback = function() end},\n"
     "  shell.settings.open('org.gnome-lua.test'),\n"
     "  shell.hooks.on('fz.alpha', function() end),\n"
     "  shell.hooks.before('fz.beta', function() end),\n"
     "  fz.new{name = 'a'}, fz.new_sub(), fz.new_other(), dead,\n"
     "}\n"
     "handles[#handles + 1] = handles[3]:on_changed('count', function() end)\n"
     "handles[#handles + 1] = handles[6]:on_poked(function() end)\n"
     "local others = { true, 0, 1.5, 'count', {}, tostring, coroutine.create(tostring), fz.lud(), io.stdout }\n"
     "for _, h in ipairs(handles) do others[#others + 1] = h end\n"
     "local args = { {}, {'count'}, {1}, {'x', 2}, {tostring}, {{}}, {handles[6]}, {'count', 7}, {nil, nil}, {'flag', true}, {'name', 'n'}, {'list', {'a'}} }\n"
     "local calls, errors = 0, 0\n"
     "for _, a in ipairs(handles) do\n"
     "  local names = fz.member_names(a)\n"
     "  assert(next(names) ~= nil and names.alive == false)\n"
     "  for name, is_method in pairs(names) do\n"
     "    if is_method then\n"
     "      local f = a.alive and a[name] or (a:deadok() and nil)\n"
     "      if f == nil and not a.alive then f = (select(2, pcall(function() return a[name] end))) end\n"
     "      if type(f) == 'function' then\n"
     "        for _, b in ipairs(others) do\n"
     "          for _, argv in ipairs(args) do\n"
     "            calls = calls + 1\n"
     "            local ok, err = pcall(f, b, table.unpack(argv, 1, 2))\n"
     "            if not ok then errors = errors + 1 end\n"
     "          end\n"
     "        end\n"
     "      end\n"
     "    end\n"
     "  end\n"
     "end\n"
     "assert(calls > 1000 and errors > 500, calls .. '/' .. errors)\n");
  assert_state_usable (s);
  g_object_unref (s);
  assert_no_listeners ();
  g_settings_reset (settings, "count");
  g_settings_reset (settings, "flag");
  g_settings_reset (settings, "name");
  g_settings_reset (settings, "list");
  fixture_end ();
}

/* Every way a handle can die, then every operation on it. */
static void
test_handles_dead (void)
{
  ShellLuaState *s;

  fixture_begin (4);
  s = new_priv ();
  DO (s,
     "local deaths = {}\n"
     "do\n"
     "  local a = fz.new{name='a'}; a:destroy(); deaths.destroy = a\n"
     "  local b = fz.new{name='b'}; b:external_destroy(); deaths.external = b\n"
     "  local c = fz.new{name='c'}; fz.invalidate(c); deaths.invalidated = c\n"
     "  local d = fz.new{name='d'}; fz.invalidate(d, true); deaths.invalidated_all = d\n"
     "  deaths.ephemeral = fz.ephemeral()\n"
     "  local h = fz.hold(); fz.drop(); deaths.finalized = h\n"
     "  local p = fz.new{name='p'}; local k = fz.new{name='k', parent=p}; p:destroy(); deaths.child = k; deaths.parent = p\n"
     "  local g = fz.new{name='g'}; debug.getmetatable(g).__gc(g); deaths.gc = g\n"
     "end\n"
     "local live = fz.new{name='live'}\n"
     "local other = fz.new_other()\n"
     "local base = fz.count()\n"
     "local n = 0\n"
     "for how, h in pairs(deaths) do\n"
     "  n = n + 1\n"
     "  assert(h.alive == false, how)\n"
     "  assert(tostring(h):find('(dead)', 1, true), how)\n"
     "  assert(h == h and h ~= live and h ~= other, how)\n"
     "  assert(h:deadok() == 'dead', how)\n"
     "  assert(pcall(h.destroy, h) and pcall(h.destroy, h), how)\n"
     "  assert(fz.members(h) > 20, how)\n"
     "  for _, key in ipairs{'i', 'name', 'friend', 'ghost', 'nope', 'plain', 'poke', 'double', 'label', 'boom', 'ro', 'wo', 'ptr', 'alive_', 1, true} do\n"
     "    local ok, err = pcall(function() return h[key] end)\n"
     "    assert(not ok and err:find('handle to fz.Obj is no longer valid', 1, true), how .. ' read ' .. tostring(key) .. ': ' .. tostring(err))\n"
     "    ok, err = pcall(function() h[key] = 1 end)\n"
     "    assert(not ok and err:find('handle to fz.Obj is no longer valid', 1, true), how .. ' write ' .. tostring(key))\n"
     "  end\n"
     "  for _, name in ipairs{'poke', 'on_poked', 'plain', 'external_destroy', 'defer'} do\n"
     "    local ok, err = pcall(function() return h[name] end)\n"
     "    assert(not ok and err:find('no longer valid', 1, true), how .. ' ' .. name)\n"
     "    ok, err = pcall(live[name], h, function() end)\n"
     "    assert(not ok and err:find('no longer valid', 1, true), how .. ' ' .. name .. ' with the dead handle as self')\n"
     "  end\n"
     /* as an argument */
     "  assert(not pcall(fz.new, {parent = h}) and not pcall(fz.share, h) and not pcall(fz.invalidate, h))\n"
     "  assert(not pcall(function() live.friend = h end))\n"
     "  assert(pcall(fz.members, h) and fz.count() == base, how)\n"
     "end\n"
     "assert(n == 9, n)\n"
     /* the object of an invalidated handle lives on: a new handle is a different, live userdata */
     "local x = fz.new{name='x'}\n"
     "fz.share(x); fz.invalidate(x)\n"
     "assert(not x.alive)\n"
     "local x2 = fz.take()\n"
     "assert(x2.alive and x2 ~= x and x2.name == 'x' and fz.count() == base + 1)\n"
     "x2:destroy()\n"
     "assert(not x2.alive and fz.count() == base)\n");
  assert_state_usable (s);
  g_object_unref (s);
  fixture_end ();
}

/* The same objects in two states: handles never leak from one to the other,
 * and each state's teardown leaves the other alone. */
static void
test_handles_two_states (void)
{
  ShellLuaState *a, *b, *c;
  GObject *obj;
  int order;

  for (order = 0; order < 3; order++)
    {
      fixture_begin (5 + order);
      a = new_priv ();
      b = tu_state_new ("fuzz-b", SHELL_LUA_STATE_SANDBOXED);
      c = new_priv ();

      DO (a, "O = fz.new{name='shared'}; fz.share(O); P = fz.new{name='a-only'}; assert(fz.count() == 2)");
      DO (b, "H = fz.take(); assert(H.alive and H.name == 'shared' and fz.count() == 0)\n"
             "assert(H.i == 0 and H:plain() == 'plain')\n"
             /* not owned by this state: destroy() is refused, nothing else is hurt */
             "local ok, err = pcall(H.destroy, H)\n"
             "assert(not ok and err:find('cannot be destroyed', 1, true), err)\n"
             "H.i = 9");
      DO (c, "Q = fz.take(); assert(Q.alive and Q.i == 9)");
      /* the same object is a different userdata per state; equality is per state */
      DO (a, "assert(O.i == 9 and O == fz.take() and P ~= O)");

      /* invalidating in one state leaves the others' handles alone ... */
      DO (a, "fz.invalidate(O); assert(not O.alive)");
      DO (b, "assert(H.alive and H.i == 9)");
      DO (c, "assert(Q.alive)");
      /* ... and in all states kills them all */
      DO (c, "fz.invalidate(Q, true); assert(not Q.alive)");
      DO (b, "assert(not H.alive)");
      DO (a, "O = fz.take(); assert(O.alive)");
      DO (b, "H = fz.take(); assert(H.alive)");

      /* a state going away first, last or in the middle */
      obj = shared;
      g_object_ref (obj);
      switch (order)
        {
        case 0: g_object_unref (a); g_object_unref (b); g_object_unref (c); break;
        case 1: g_object_unref (c); g_object_unref (a); g_object_unref (b); break;
        default: g_object_unref (b); g_object_unref (c); g_object_unref (a); break;
        }
      /* a is gone: its owned object was destroyed; the object itself is finalized once nothing refers to it */
      g_assert_true (((FzObj *) obj)->destroyed);
      shell_lua_handle_invalidate (NULL, obj);   /* must not touch the closed states */
      g_object_unref (obj);
      fixture_end ();
    }
}

/* ======================================================================== */
/* 2. Re-entrancy from hooks                                                 */
/* ======================================================================== */

/* Listeners that connect, disconnect (each other, themselves, all of them)
 * and declare while the hook is being emitted. */
static void
test_hooks_registry_reentrancy (void)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  ShellLuaState *s;

  fixture_begin (10);
  declare_hooks ();
  s = new_priv ();

  /* order, self-disconnects, disconnects of later and earlier listeners, connects during emission */
  DO (s,
     "local hooks = shell.hooks\n"
     "log, conns, six = {}, {}, false\n"
     "local function on(name, f)\n"
     "  conns[name] = hooks.on('fz.alpha', function(p) log[#log + 1] = name; if f then f(p) end end)\n"
     "end\n"
     "on('1', function() conns['4']:disconnect() end)\n"
     "on('2', function() conns['2']:disconnect(); conns['2']:disconnect(); assert(not conns['2']:is_connected()) end)\n"
     "on('3', function() if not six then six = true; on('6') end end)\n"
     "on('4')\n"
     "on('5', function() fz.emit_after('fz.gamma') end)\n"
     "assert(fz.listeners('fz.alpha') == 5)\n");
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  /* 4 was disconnected before its turn, 6 was connected after the snapshot */
  tu_assert_eval_str (s, "table.concat(log, ',')", "1,2,3,5");
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 4);   /* 1, 3, 5, 6 */
  DO (s, "log = {}");
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  tu_assert_eval_str (s, "table.concat(log, ',')", "1,3,5,6");
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 4);

  /* a listener that disconnects every connection, itself included */
  DO (s,
     "log = {}\n"
     "conns['1']:disconnect()\n"
     "conns['0'] = shell.hooks.on('fz.alpha', function()\n"
     "  log[#log + 1] = 'killer'\n"
     "  for k, c in pairs(conns) do c:disconnect() end\n"
     "end)\n");
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 4);   /* 3, 5, 6, killer */
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  /* 3, 5, 6 ran before the killer (registration order), nothing after it */
  tu_assert_eval_str (s, "table.concat(log, ',')", "3,5,6,killer");
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 0);
  DO (s, "log = {}");
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  tu_assert_eval_str (s, "table.concat(log, ',')", "");

  /* the killer first: the rest of the snapshot is skipped */
  DO (s,
     "log, conns = {}, {}\n"
     "conns.k = shell.hooks.on('fz.alpha', function() log[#log + 1] = 'k'; for _, c in pairs(conns) do c:disconnect() end end)\n"
     "for i = 1, 5 do conns[i] = shell.hooks.on('fz.alpha', function() log[#log + 1] = i end) end\n");
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  tu_assert_eval_str (s, "table.concat(log, ',')", "k");
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 0);

  /* the before phase: the first CANCEL stops it, disconnecting the later ones changes nothing for the verdict */
  DO (s,
     "log, conns = {}, {}\n"
     "conns.a = shell.hooks.before('fz.beta', function() log[#log + 1] = 'a'; return {first = 1} end)\n"
     "conns.b = shell.hooks.before('fz.beta', function() log[#log + 1] = 'b'; conns.d:disconnect(); return {second = 2} end)\n"
     "conns.c = shell.hooks.before('fz.beta', function() log[#log + 1] = 'c'; return shell.hooks.CANCEL end)\n"
     "conns.d = shell.hooks.before('fz.beta', function() log[#log + 1] = 'd' end)\n"
     "conns.e = shell.hooks.on('fz.beta', function() log[#log + 1] = 'after' end)\n");
  DO (s,
     "local cancelled, amend = fz.emit_before('fz.beta')\n"
     "assert(cancelled == true and amend.first == 1 and amend.second == 2, tostring(cancelled))\n"
     "assert(table.concat(log, ',') == 'a,b,c')\n"
     "assert(fz.listeners('fz.beta', 'before') == 3)\n"
     "conns.c:disconnect()\n"
     "log = {}\n"
     "cancelled, amend = fz.emit_before('fz.beta')\n"
     "assert(cancelled == false and table.concat(log, ',') == 'a,b', table.concat(log, ','))\n"
     "fz.emit_after('fz.beta')\n"
     "assert(table.concat(log, ',') == 'a,b,after')\n"
     "for _, c in pairs(conns) do c:disconnect() end\n");

  /* declaring hooks (and connecting to them, and emitting them) while emitting */
  DO (s,
     "declared = 0\n"
     "dyn = shell.hooks.on('fz.alpha', function()\n"
     "  for i = 1, 400 do\n"
     "    fz.declare('fz.dyn-' .. i .. '.ev', i % 2 == 0)\n"
     "    declared = declared + 1\n"
     "  end\n"
     "  fz.declare('fz.dyn-1.ev', false)\n"                   /* idempotent */
     "  local got = 0\n"
     "  local c = shell.hooks.on('fz.dyn-1.ev', function() got = got + 1 end)\n"
     "  fz.emit_after('fz.dyn-1.ev')\n"
     "  local b = shell.hooks.before('fz.dyn-2.ev', function() got = got + 10; return shell.hooks.CANCEL end)\n"
     "  assert(fz.emit_before('fz.dyn-2.ev') == true)\n"
     "  assert(got == 11, got)\n"
     "  c:disconnect(); b:disconnect()\n"
     "end)\n");
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  tu_assert_eval_str (s, "declared", "400");
  g_assert_true (shell_hook_registry_is_declared (registry, "fz.dyn-400.ev"));
  g_assert_true (shell_hook_registry_is_cancellable (registry, "fz.dyn-400.ev"));
  g_assert_false (shell_hook_registry_is_cancellable (registry, "fz.dyn-399.ev"));
  DO (s, "dyn:disconnect()\n"
        "local n = 0 for _, h in ipairs(shell.hooks.list()) do if h.name:find('^fz%.dyn%-') then n = n + 1 end end\n"
        "assert(n == 400, n)\n"
        "assert(not pcall(fz.declare, 'Bad Name') and not pcall(fz.declare, 'nodots') and not pcall(fz.declare, ''))\n"
        "assert(not pcall(shell.hooks.on, 'fz.never-declared', print))\n"
        "assert(not pcall(shell.hooks.before, 'fz.alpha', print))\n");

  /* the programmer errors are criticals, once each, and change nothing */
  g_test_expect_message (NULL, G_LOG_LEVEL_CRITICAL, "Hook 'fz.alpha' redeclared with a different cancellable flag");
  DO (s, "fz.declare_raw('fz.alpha', 'k:s', true)");
  g_test_expect_message (NULL, G_LOG_LEVEL_CRITICAL, "Hook 'fz.alpha' redeclared with a different schema");
  DO (s, "fz.declare_raw('fz.alpha', 'other:s', false)");
  g_test_expect_message (NULL, G_LOG_LEVEL_CRITICAL, "Invalid hook name 'Bad Name'*");
  DO (s, "fz.declare_raw('Bad Name', '', false)");
  g_test_assert_expected_messages ();
  g_assert_false (shell_hook_registry_is_cancellable (registry, "fz.alpha"));

  assert_no_listeners ();
  assert_state_usable (s);
  g_object_unref (s);
  fixture_end ();
}

/* A listener that emits its own hook again and again, and listeners that
 * make the state unusable halfway (the state is still usable afterwards). */
static void
test_hooks_recursion (void)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  ShellLuaState *s;
  guint errors;

  fixture_begin (11);
  declare_hooks ();
  s = new_priv ();

  DO (s,
     "depth, maxdepth, calls = 0, 0, 0\n"
     "rec = shell.hooks.on('fz.gamma', function()\n"
     "  calls = calls + 1\n"
     "  depth = depth + 1\n"
     "  if depth > maxdepth then maxdepth = depth end\n"
     "  fz.emit_after('fz.gamma')\n"            /* never stops by itself: the nesting limit does */
     "  depth = depth - 1\n"
     "end)\n");
  errors = shell_lua_state_get_error_count (s);
  shell_hook_registry_emit_after (registry, "fz.gamma", NULL);
  tu_assert_eval_true (s, "maxdepth > 10 and maxdepth <= 50 and depth == 0 and calls == maxdepth");
  g_assert_cmpuint (shell_lua_state_get_error_count (s), >, errors);
  DO (s, "rec:disconnect()");
  g_assert_cmpuint (listeners ("fz.gamma", SHELL_HOOK_PHASE_AFTER), ==, 0);

  /* two hooks that emit each other */
  DO (s,
     "ping, pong = 0, 0\n"
     "c1 = shell.hooks.on('fz.alpha', function() ping = ping + 1; fz.emit_after('fz.gamma') end)\n"
     "c2 = shell.hooks.on('fz.gamma', function() pong = pong + 1; fz.emit_after('fz.alpha') end)\n");
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  tu_assert_eval_true (s, "ping > 5 and pong > 5 and ping + pong <= 60");
  DO (s, "c1:disconnect(); c2:disconnect()");

  /* the same inside the before phase, with CANCEL on the way */
  DO (s,
     "n = 0\n"
     "b1 = shell.hooks.before('fz.beta', function() n = n + 1; local cancelled = fz.emit_before('fz.beta'); return shell.hooks.CANCEL end)\n");
  DO (s, "assert(fz.emit_before('fz.beta') == true and n > 5)\n");
  DO (s, "b1:disconnect()");

  /* a listener that connects a new listener to the hook it is running for, forever */
  DO (s,
     "grow = 0\n"
     "local function again()\n"
     "  grow = grow + 1\n"
     "  if grow < 200 then shell.hooks.on('fz.alpha', again) end\n"
     "end\n"
     "shell.hooks.on('fz.alpha', again)\n");
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  tu_assert_eval_str (s, "grow", "1");                 /* connected during emission: next time */
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 2);
  for (int i = 0; i < 10; i++)
    shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  /* every run connects one more until 200 have run */
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 200);
  tu_assert_eval_true (s, "grow >= 200");

  /* the state going away while listeners are connected: they are gone with it */
  g_object_unref (s);
  assert_no_listeners ();
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  fixture_end ();
}

/* Rule 8 through a hook: the listener destroys the object whose callback emitted it. */
static void
test_hooks_destroy_emitter (void)
{
  ShellLuaState *s;
  GObject *obj;

  fixture_begin (12);
  declare_hooks ();
  s = new_priv ();

  /* 1. the emitting object destroyed (twice) by the listener: deferred until the callback returns */
  DO (s,
     "events = {}\n"
     "o = fz.new{name='o', on_poked = function(self, n)\n"
     "  fz.emit_after('fz.alpha', {k = 'x'})\n"
     "  events[#events + 1] = 'cb:' .. tostring(self.alive) .. ':' .. fz.count()\n"
     "end}\n"
     "inside = {}\n"
     "c = shell.hooks.on('fz.alpha', function()\n"
     "  o:destroy()\n"
     "  inside[#inside + 1] = o.alive\n"
     "  o:destroy()\n"
     "  inside[#inside + 1] = o.i\n"
     "end)\n"
     "o:poke(1)\n"
     "assert(inside[1] == true and inside[2] == 0, 'the handle stays valid until the callback returns')\n"
     "assert(events[1] == 'cb:true:1', events[1])\n"
     "assert(o.alive == false and fz.count() == 0)\n");
  g_assert_cmpstr (destroy_log->str, ==, "o,");

  /* 2. an ancestor destroyed by the listener: the emitter dies with it, afterwards */
  g_string_truncate (destroy_log, 0);
  DO (s,
     "p = fz.new{name='p'}\n"
     "k = fz.new{name='k', parent=p, on_poked = function() fz.emit_after('fz.alpha') end}\n"
     "g = fz.new{name='g', parent=k}\n"
     "state = {}\n"
     "c:disconnect()\n"
     "c = shell.hooks.on('fz.alpha', function()\n"
     "  p:destroy()\n"
     "  state[#state + 1] = p.alive and k.alive and g.alive\n"
     "  k:destroy()\n"
     "  fz.new{name='late', parent=p}\n"                      /* a new child of the doomed parent */
     "end)\n"
     "k:poke(1)\n"
     "assert(state[1] == true, 'ancestor and emitter stay valid inside')\n"
     "assert(not p.alive and not k.alive and not g.alive and fz.count() == 0, fz.count())\n");
  g_assert_cmpstr (destroy_log->str, ==, "late,g,k,p,");

  /* 3. nested emitting frames, everything destroyed from the innermost listener */
  g_string_truncate (destroy_log, 0);
  DO (s,
     "c:disconnect()\n"
     "o2 = fz.new{name='o2', on_poked = function() fz.emit_after('fz.alpha') end}\n"
     "o1 = fz.new{name='o1', on_poked = function() o2:poke(2) end}\n"
     "c = shell.hooks.on('fz.alpha', function() o1:destroy(); o2:destroy(); o1:destroy(); mark = fz.count() end)\n"
     "o1:poke(1)\n"
     "assert(mark == 2 and fz.count() == 0 and not o1.alive and not o2.alive)\n"
     "c:disconnect()\n");
  g_assert_cmpstr (destroy_log->str, ==, "o2,o1,");

  /* 4. a failing callback or listener does not lose the deferred destroy */
  g_string_truncate (destroy_log, 0);
  DO (s,
     "e = fz.new{name='e', on_poked = function(self) self:destroy(); error('callback failed') end}\n"
     "local errors = fz.error_count()\n"
     "e:poke(1)\n"
     "assert(not e.alive and fz.error_count() == errors + 1 and fz.count() == 0)\n"
     "f = fz.new{name='f', on_poked = function(self) fz.emit_after('fz.alpha') end}\n"
     "c = shell.hooks.on('fz.alpha', function() f:destroy(); error('listener failed') end)\n"
     "f:poke(1)\n"
     "assert(not f.alive and fz.count() == 0)\n"
     "c:disconnect()\n");
  g_assert_cmpstr (destroy_log->str, ==, "e,f,");

  /* 5. the emission starts in C, outside any Lua call: the object is destroyed from the listener */
  g_string_truncate (destroy_log, 0);
  DO (s,
     "q = fz.new{name='q', on_poked = function(self) fz.emit_after('fz.alpha') end}\n"
     "fz.share(q)\n"
     "c = shell.hooks.on('fz.alpha', function() q:destroy(); gone_inside = q.alive end)\n");
  obj = g_object_ref (shared);
  g_signal_emit (obj, obj_signals[SIG_POKED], 0, 1);
  DO (s, "assert(gone_inside == true and not q.alive and fz.count() == 0)");
  g_assert_true (((FzObj *) obj)->destroyed);
  g_object_unref (obj);
  g_assert_cmpstr (destroy_log->str, ==, "q,");

  DO (s, "c:disconnect()");
  assert_no_listeners ();
  assert_state_usable (s);
  g_object_unref (s);
  fixture_end ();
}

/* Two states: a listener of one emits a hook the other listens to, and the
 * other state destroys itself, spins or raises from inside it. */
static void
test_hooks_cross_state (void)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  ShellLuaLimits a_limits = { .callback_budget_us = 10 * G_USEC_PER_SEC };
  ShellLuaLimits b_limits = { .callback_budget_us = 30 * 1000 };
  ShellLuaState *a, *b;

  fixture_begin (13);
  declare_hooks ();
  capture_begin ();
  a = shell_lua_state_new ("state-a", SHELL_LUA_STATE_SANDBOXED, &a_limits);
  b = shell_lua_state_new ("state-b", SHELL_LUA_STATE_SANDBOXED, &b_limits);

  DO (a, "shell.hooks.on('fz.alpha', function() fz.mark('A1'); fz.emit_after('fz.gamma'); fz.mark('A2') end)\n"
        "shell.hooks.on('fz.alpha', function() fz.mark('A3') end)\n");
  DO (b, "shell.hooks.on('fz.gamma', function() fz.mark('B1'); error('B1 failed') end)\n"
        "shell.hooks.on('fz.gamma', function() fz.mark('B2'); while true do end end)\n"
        "shell.hooks.on('fz.gamma', function() fz.mark('B3'); fz.kill_state(); fz.mark('B3-after') end)\n"
        "shell.hooks.on('fz.gamma', function() fz.mark('B4') end)\n");

  /* B fails, is cut by its own watchdog, then destroys itself in the middle of A's call */
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  g_assert_cmpstr (marks->str, ==, "A1,B1,B2,B3,B3-after,A2,A3,");
  g_string_truncate (marks, 0);
  g_assert_true (shell_lua_state_is_alive (a));
  g_assert_false (shell_lua_state_is_alive (b));
  g_assert_cmpuint (listeners ("fz.gamma", SHELL_HOOK_PHASE_AFTER), ==, 0);
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 2);
  g_assert_cmpuint (shell_lua_state_get_error_count (b), ==, 2);
  g_assert_cmpuint (shell_lua_state_get_error_count (a), ==, 0);

  /* nothing left of B: the next emission only reaches A */
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  g_assert_cmpstr (marks->str, ==, "A1,A2,A3,");
  g_string_truncate (marks, 0);
  g_object_unref (b);

  /* the other way round: A dies while it emits to B, B's listener runs to its end */
  b = shell_lua_state_new ("state-b", SHELL_LUA_STATE_SANDBOXED, &b_limits);
  DO (b, "shell.hooks.on('fz.gamma', function() fz.mark('B'); shell.hooks.on('fz.gamma', function() fz.mark('B-new') end) end)\n");
  DO (a, "shell.hooks.on('fz.alpha', function() fz.kill_state() end)\n");
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  g_assert_false (shell_lua_state_is_alive (a));
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 0);
  g_string_truncate (marks, 0);
  shell_hook_registry_emit_after (registry, "fz.gamma", NULL);
  g_assert_cmpstr (marks->str, ==, "B,B-new,");   /* the listener B made during the first emission (A1 emitted) */
  g_object_unref (a);
  g_object_unref (b);
  assert_no_listeners ();
  capture_end ();
  fixture_end ();
}

/* The API called from inside coroutines: arguments and results live on the
 * coroutine's stack, not on the state's main one. */
static void
test_api_from_coroutines (void)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  ShellLuaState *states[2];
  guint i;

  fixture_begin (14);
  declare_hooks ();
  capture_begin ();
  states[0] = new_priv ();
  states[1] = tu_state_new ("fuzz-sandboxed", SHELL_LUA_STATE_SANDBOXED);

  for (i = 0; i < G_N_ELEMENTS (states); i++)
    {
      ShellLuaState *s = states[i];

      DO (s,
         "calls = {}\n"
         "local conns = {}\n"
         /* connected from a coroutine of each kind: the listener is the function that was passed */
         "local co = coroutine.wrap(function()\n"
         "  conns[1] = shell.hooks.on('fz.alpha', function() calls[#calls + 1] = 'wrap-listener' end)\n"
         "  coroutine.yield()\n"
         "  conns[2] = shell.hooks.before('fz.beta', function() calls[#calls + 1] = 'before-listener' end)\n"
         "end)\n"
         "co(); co()\n"
         "local co2 = coroutine.create(function(a, b)\n"
         "  conns[3] = shell.hooks.on('fz.alpha', function() calls[#calls + 1] = 'create-listener' end)\n"
         "  local o = fz.new{name = 'in-co', on_poked = function(self, n) calls[#calls + 1] = 'poked:' .. n end}\n"
         "  o:on_poked(function(self, n) calls[#calls + 1] = 'conn-poked:' .. n end)\n"
         "  shell.timer.after{ms = 100000, callback = function() end}\n"
         "  shell.timer.idle(function() calls[#calls + 1] = 'idle' end)\n"
         "  coroutine.yield(o)\n"
         "  return 'done'\n"
         "end)\n"
         "local ok, o = coroutine.resume(co2)\n"
         "assert(ok and o.alive and o.name == 'in-co', tostring(o))\n"
         "assert(coroutine.resume(co2))\n"
         "assert(conns[1]:is_connected() and conns[2]:is_connected() and conns[3]:is_connected())\n"
         "fz.emit_after('fz.alpha')\n"
         "o:poke(5)\n"
         "assert(table.concat(calls, ',') == 'wrap-listener,create-listener,poked:5,conn-poked:5', table.concat(calls, ','))\n"
         "assert(fz.emit_before('fz.beta') == false and calls[#calls] == 'before-listener')\n"
         /* a hook listener emitting from inside a coroutine, a callback that tries to yield */
         "calls = {}\n"
         "local yielder = shell.hooks.on('fz.gamma', function() calls[#calls + 1] = select(2, pcall(coroutine.yield)) end)\n"
         "local co3 = coroutine.wrap(function() fz.emit_after('fz.gamma'); return 'finished' end)\n"
         "assert(co3() == 'finished' and #calls == 1 and tostring(calls[1]):find('yield', 1, true), tostring(calls[1]))\n"
         "yielder:disconnect()\n"
         /* disconnecting and destroying from inside coroutines */
         "coroutine.wrap(function() for _, c in ipairs(conns) do c:disconnect() end; o:destroy() end)()\n"
         "assert(not o.alive and not conns[1]:is_connected())\n"
         "assert(fz.listeners('fz.alpha') == 0 and fz.count() == 0)\n");
      tu_spin (5);   /* the idle */
      DO (s, "assert(calls[#calls] == 'idle' or #calls == 0, tostring(calls[#calls]))");
    }

  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  g_assert_cmpuint (captured_count (NULL), ==, 0);
  assert_no_listeners ();
  g_object_unref (states[0]);
  g_object_unref (states[1]);
  capture_end ();
  fixture_end ();
}

/* ======================================================================== */
/* 3. Disable during a callback                                              */
/* ======================================================================== */

typedef void (*Trigger) (ShellLuaExtension *ext);

typedef struct {
  const char *name;
  const char *metadata;        /* extra metadata.json members, or NULL */
  const char *lua;             /* extension.lua */
  Trigger     trigger;         /* what makes the callback run (NULL: a timer in the script does) */
  const char *expect_log;      /* destroy order, "" for none */
  const char *expect_marks;    /* what the script recorded with fz.mark() */
  gboolean    enable_may_fail; /* the disable happens during enable() */
} DisableCase;

/* Runs the extension of @c, waits for it to disable itself, and checks that
 * everything it owned was destroyed exactly once, that nothing runs any more
 * and that the Lua state and its memory are gone. */
static void
run_disable_case (const DisableCase *c, guint32 salt)
{
  Env e;
  g_autoptr (ShellLuaExtension) ext = NULL;
  ShellLuaState *state = NULL;     /* weak: the extension's reference must be the only one */
  g_autoptr (GError) error = NULL;
  int hits_after;
  ShellLuaExtensionState final;

  g_test_message ("case %s", c->name);
  fixture_begin (salt);
  declare_hooks ();
  capture_begin ();
  env_init (&e);
  ext = install (&e, "fuzz-disable@test", c->metadata, c->lua);
  g_assert_nonnull (ext);

  if (c->enable_may_fail)
    {
      g_assert_false (shell_lua_extension_manager_enable (e.manager, "fuzz-disable@test", &error));
    }
  else
    {
      enable_ok (&e, "fuzz-disable@test");
      state = shell_lua_extension_get_lua_state (ext);
      g_object_add_weak_pointer (G_OBJECT (state), (gpointer *) &state);
    }
  if (c->trigger != NULL)
    c->trigger (ext);

  if (!c->enable_may_fail)
    wait_for_state (ext, SHELL_LUA_EXTENSION_STATE_INACTIVE);
  tu_spin (150);   /* nothing may run after the disable: timers, signals, settings */
  final = shell_lua_extension_get_state (ext);
  if (final != SHELL_LUA_EXTENSION_STATE_INACTIVE && final != SHELL_LUA_EXTENSION_STATE_ERROR)
    g_error ("%s: unexpected final state %s", c->name, shell_lua_extension_state_to_string (final));
  hits_after = hits;

  g_assert_null (shell_lua_extension_get_lua_state (ext));
  if (g_strcmp0 (destroy_log->str, c->expect_log) != 0)
    g_error ("%s: objects were destroyed in the order '%s', expected '%s'", c->name, destroy_log->str, c->expect_log);
  if (g_strcmp0 (marks->str, c->expect_marks) != 0)
    g_error ("%s: the script recorded '%s', expected '%s'", c->name, marks->str, c->expect_marks);
  g_assert_cmpint (double_destroys, ==, 0);
  g_assert_cmpint (live_objs, ==, 0);          /* every object finalized: nothing leaked, nothing held */
  assert_no_listeners ();
  if (!c->enable_may_fail)
    g_assert_null (state);            /* finalized: nothing else kept it, nothing leaked */
  if (captured_count (NULL) != 0)
    g_error ("%s: unexpected warning: %s", c->name, (char *) g_ptr_array_index (captured, 0));

  /* and still nothing, later */
  tu_spin (60);
  g_assert_cmpint (hits, ==, hits_after);

  /* the extension can be enabled again afterwards (from ERROR too) and torn down from the outside as usual */
  g_string_truncate (destroy_log, 0);
  g_string_truncate (marks, 0);
  if (c->enable_may_fail)
    {
      g_autoptr (GError) again = NULL;

      g_assert_false (shell_lua_extension_manager_enable (e.manager, "fuzz-disable@test", &again));
      g_assert_nonnull (again);
    }
  else
    {
      enable_ok (&e, "fuzz-disable@test");
      assert_state (ext, SHELL_LUA_EXTENSION_STATE_ACTIVE);
      g_assert_cmpint (live_objs, >, 0);
      g_assert_true (shell_lua_extension_manager_disable (e.manager, "fuzz-disable@test", NULL));
      assert_state (ext, SHELL_LUA_EXTENSION_STATE_INACTIVE);
    }
  g_assert_cmpint (live_objs, ==, 0);
  g_assert_cmpint (double_destroys, ==, 0);
  assert_no_listeners ();

  env_clear (&e);
  capture_end ();
  g_clear_object (&ext);
  fixture_end ();
}

/* ---- the scripts ---- */

/* An extension with three owned objects (b is a child of a), a listener, a
 * long timer that must never fire, and a body that runs inside the callback. */
#define DISABLE_PROLOGUE \
  "local shell = require('shell'); local fz = shell.fz\n" \
  "local ext = {}\n" \
  "local a, b, c, conn\n" \
  "local function after_disable()\n" \
  "  fz.mark('disabled')\n" \
  /* the context is gone: its objects are dead, any use raises, nothing crashes */ \
  "  for _, h in ipairs{a, b, c} do\n" \
  "    local ok, err = pcall(function() return h.name end)\n" \
  "    fz.mark(tostring(h.alive) .. (ok and '?' or (tostring(err):find('no longer valid', 1, true) and 'D' or '!')))\n" \
  "  end\n" \
  "  fz.mark('count:' .. fz.count())\n" \
  /* create more things after the disable began: they die with the state, once, and never run */ \
  "  local x = fz.new{name='x', on_poked = function() fz.hit() end}\n" \
  "  fz.mark('x:' .. tostring(x.alive))\n" \
  "  shell.timer.after{ms=1, callback=function() fz.hit() end}\n" \
  "  shell.timer.every{ms=1, callback=function() fz.hit() end}\n" \
  "  shell.hooks.on('fz.alpha', function() fz.hit() end)\n" \
  "  shell.hooks.before('fz.beta', function() fz.hit() end)\n" \
  "  x:on_poked(function() fz.hit() end)\n" \
  "  fz.mark('listeners:' .. fz.listeners('fz.alpha'))\n" \
  "end\n" \
  "function ext.enable()\n" \
  "  a = fz.new{name='a'}\n" \
  "  b = fz.new{name='b', parent=a, on_poked = function() fz.hit() end}\n" \
  "  c = fz.new{name='c'}\n" \
  "  b:on_poked(function() fz.hit() end)\n" \
  "  conn = shell.hooks.on('fz.alpha', function() fz.hit() end)\n" \
  "  shell.timer.after{ms=150, callback=function() fz.hit() end}\n" \
  "  shell.timer.every{ms=40, callback=function() fz.hit() end}\n" \
  "  shell.timer.idle{callback=function() end}\n" \
  "  fz.share(b)\n"

#define DISABLE_EPILOGUE \
  "end\n" \
  "function ext.disable()\n" \
  "  local ok, err = fz.disable()\n"                          /* re-entrant: refused, not recursed */ \
  "  fz.mark('redisable:' .. tostring(ok))\n" \
  "end\n" \
  "return ext\n"

#define EXPECTED_MARKS "redisable:false,disabled,falseD,falseD,falseD,count:0,x:true,listeners:1,"
#define EXPECTED_LOG   "c,b,a,x,"

static const char disable_in_timer[] =
  DISABLE_PROLOGUE
  "  shell.timer.after{ms=3, callback=function()\n"
  "    local ok, err = fz.disable()\n"
  "    fz.mark('ok:' .. tostring(ok))\n"
  "    after_disable()\n"
  "  end}\n"
  DISABLE_EPILOGUE;

static const char disable_in_every_timer[] =
  DISABLE_PROLOGUE
  "  local n = 0\n"
  "  shell.timer.every{ms=2, callback=function()\n"
  "    n = n + 1\n"
  "    if n < 3 then return end\n"
  "    fz.disable()\n"
  "    after_disable()\n"
  "    return true\n"                                  /* asks to repeat: it must not */
  "  end}\n"
  DISABLE_EPILOGUE;

static const char disable_in_idle[] =
  DISABLE_PROLOGUE
  "  shell.timer.idle{callback=function()\n"
  "    fz.disable()\n"
  "    after_disable()\n"
  "  end}\n"
  DISABLE_EPILOGUE;

static const char disable_in_hook[] =
  DISABLE_PROLOGUE
  "  shell.hooks.on('fz.alpha', function()\n"
  "    fz.disable()\n"
  "    after_disable()\n"
  "  end)\n"
  DISABLE_EPILOGUE;

static const char disable_in_before_hook[] =
  DISABLE_PROLOGUE
  "  shell.hooks.before('fz.beta', function()\n"
  "    fz.disable()\n"
  "    after_disable()\n"
  "    return shell.hooks.CANCEL\n"
  "  end)\n"
  DISABLE_EPILOGUE;

static const char disable_in_settings[] =
  DISABLE_PROLOGUE
  "  local st = shell.settings.extension()\n"
  "  st:on_changed('count', function(key, value)\n"
  "    fz.disable()\n"
  "    after_disable()\n"
  "  end)\n"
  DISABLE_EPILOGUE;

static const char disable_in_object_callback[] =
  DISABLE_PROLOGUE
  "  c:on_poked(function(self)\n"
  "    fz.disable()\n"
  "    after_disable()\n"
  "    fz.mark(tostring(self.alive))\n"
  "  end)\n"
  "  fz.share(c)\n"
  DISABLE_EPILOGUE;

/* the callback first destroys its own object (deferred, rule 8), then the extension goes away under it */
static const char disable_after_deferred_destroy[] =
  DISABLE_PROLOGUE
  "  c:on_poked(function(self)\n"
  "    self:destroy()\n"
  "    fz.mark('deferred:' .. tostring(self.alive))\n"
  "    fz.disable()\n"
  "    after_disable()\n"
  "  end)\n"
  "  fz.share(c)\n"
  DISABLE_EPILOGUE;

static void
trigger_hook (ShellLuaExtension *ext)
{
  shell_hook_registry_emit_after (shell_hook_registry_get_default (), "fz.alpha", NULL);
}

static void
trigger_before_hook (ShellLuaExtension *ext)
{
  /* the verdict of a listener that disabled its own extension is dropped */
  g_assert_cmpint (shell_hook_registry_emit_before (shell_hook_registry_get_default (), "fz.beta", NULL, NULL),
                   ==, SHELL_HOOK_VERDICT_CONTINUE);
}

static void
trigger_settings (ShellLuaExtension *ext)
{
  g_autoptr (GSettings) other = g_settings_new (TEST_SCHEMA);

  g_settings_set_int (other, "count", 42);
  g_settings_sync ();
}

/* The signal is emitted from C, outside any Lua call: the callback's pcall is the outermost one. */
static void
trigger_object_signal (ShellLuaExtension *ext)
{
  g_autoptr (GObject) obj = g_object_ref (shared);

  g_signal_emit (obj, obj_signals[SIG_POKED], 0, 1);
}

static void
test_disable_during_callbacks (void)
{
  static const DisableCase cases[] = {
    { "timer", NULL, disable_in_timer, NULL, EXPECTED_LOG, "redisable:false,ok:true,disabled,falseD,falseD,falseD,count:0,x:true,listeners:1,", FALSE },
    { "every-timer", NULL, disable_in_every_timer, NULL, EXPECTED_LOG, EXPECTED_MARKS, FALSE },
    { "idle", NULL, disable_in_idle, NULL, EXPECTED_LOG, EXPECTED_MARKS, FALSE },
    { "hook-after", NULL, disable_in_hook, trigger_hook, EXPECTED_LOG, EXPECTED_MARKS, FALSE },
    { "hook-before", NULL, disable_in_before_hook, trigger_before_hook, EXPECTED_LOG, EXPECTED_MARKS, FALSE },
    { "settings", ", \"settings-schema\": \"" TEST_SCHEMA "\"", disable_in_settings, trigger_settings,
      EXPECTED_LOG, EXPECTED_MARKS, FALSE },
    { "deferred-destroy-then-disable", NULL, disable_after_deferred_destroy, trigger_object_signal, EXPECTED_LOG,
      "deferred:true,redisable:false,disabled,falseD,falseD,falseD,count:0,x:true,listeners:1,", FALSE },
    { "object-callback", NULL, disable_in_object_callback, trigger_object_signal, EXPECTED_LOG,
      "redisable:false,disabled,falseD,falseD,falseD,count:0,x:true,listeners:1,false,", FALSE },
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    run_disable_case (&cases[i], 20 + i);
}

/* disable() called while enable() or the main chunk still runs, and from disable() itself */
static void
test_disable_during_enable (void)
{
  static const DisableCase cases[] = {
    { "in-enable", NULL,
      "local shell = require('shell'); local fz = shell.fz\n"
      "local ext = {}\n"
      "function ext.enable()\n"
      "  local a = fz.new{name='a'}\n"
      "  local b = fz.new{name='b', parent=a}\n"
      "  shell.timer.after{ms=1, callback=function() fz.hit() end}\n"
      "  shell.hooks.on('fz.alpha', function() fz.hit() end)\n"
      "  local ok = fz.disable()\n"
      "  fz.mark('disable:' .. tostring(ok) .. ':' .. tostring(a.alive))\n"
      "  fz.new{name='after'}\n"
      "end\n"
      "return ext\n",
      NULL, "b,a,after,", "disable:true:false,", TRUE },
    { "in-main-chunk", NULL,
      "local shell = require('shell'); local fz = shell.fz\n"
      "local a = fz.new{name='a'}\n"
      "shell.timer.every{ms=1, callback=function() fz.hit() end}\n"
      "fz.disable()\n"
      "fz.mark('chunk:' .. tostring(a.alive))\n"
      "return {}\n",
      NULL, "a,", "chunk:false,", TRUE },
    { "in-disable", NULL,
      "local shell = require('shell'); local fz = shell.fz\n"
      "local ext = {}\n"
      "local a, t\n"
      "function ext.enable()\n"
      "  a = fz.new{name='a'}\n"
      "  t = shell.timer.after{ms=2, callback=function() fz.disable() end}\n"
      "end\n"
      "function ext.disable()\n"
      "  fz.mark('disable():' .. tostring(a.alive))\n"
      "  local ok, err = fz.disable()\n"
      "  fz.mark(tostring(ok))\n"
      "  fz.new{name='created-in-disable'}\n"
      "end\n"
      "return ext\n",
      NULL, "created-in-disable,a,", "disable():true,false,", FALSE },
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    run_disable_case (&cases[i], 40 + i);
}

/* Two extensions listen to one hook; the first disables itself from its
 * listener. The registry must skip what the dying extension had left in the
 * snapshot, still run the other extension's listeners in order, and keep the
 * counts right. */
static void
test_hooks_disable_self_in_emission (void)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  Env e;
  g_autoptr (ShellLuaExtension) ea = NULL, eb = NULL;
  g_autoptr (ShellLuaState) sa = NULL;

  fixture_begin (30);
  declare_hooks ();
  capture_begin ();
  env_init (&e);
  ea = install (&e, "a@test", NULL,
                "local shell = require('shell'); local fz = shell.fz\n"
                "return { enable = function()\n"
                "  shell.hooks.on('fz.alpha', function() fz.mark('a1') end)\n"
                "  shell.hooks.on('fz.alpha', function() fz.mark('a2'); fz.disable(); fz.mark('a2-after') end)\n"
                "  shell.hooks.on('fz.alpha', function() fz.mark('a3') end)\n"       /* never */
                "  shell.hooks.before('fz.beta', function() fz.mark('ab1') end)\n"
                "  shell.hooks.before('fz.beta', function() fz.mark('ab2'); fz.disable(); return shell.hooks.CANCEL end)\n"
                "  shell.hooks.before('fz.beta', function() fz.mark('ab3') end)\n"
                "end }\n");
  eb = install (&e, "b@test", NULL,
                "local shell = require('shell'); local fz = shell.fz\n"
                "return { enable = function()\n"
                "  shell.hooks.on('fz.alpha', function() fz.mark('b1') end)\n"
                "  shell.hooks.before('fz.beta', function() fz.mark('bb1') end)\n"
                "end }\n");
  enable_ok (&e, "a@test");
  enable_ok (&e, "b@test");
  sa = g_object_ref (shell_lua_extension_get_lua_state (ea));
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 4);
  g_assert_cmpuint (listeners ("fz.beta", SHELL_HOOK_PHASE_BEFORE), ==, 4);

  /* the answer of a listener that disabled its own extension is dropped (the call is reported as failed
   * because its state was destroyed during it), so the emission goes on with the other extension's */
  g_assert_cmpint (shell_hook_registry_emit_before (registry, "fz.beta", NULL, NULL), ==, SHELL_HOOK_VERDICT_CONTINUE);
  g_assert_cmpstr (marks->str, ==, "ab1,ab2,bb1,");
  g_string_truncate (marks, 0);
  assert_state (ea, SHELL_LUA_EXTENSION_STATE_INACTIVE);
  assert_state (eb, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  g_assert_false (shell_lua_state_is_alive (sa));
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 1);
  g_assert_cmpuint (listeners ("fz.beta", SHELL_HOOK_PHASE_BEFORE), ==, 1);

  /* the second extension is unaffected, and the dead one's listeners are gone */
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  g_assert_cmpint (shell_hook_registry_emit_before (registry, "fz.beta", NULL, NULL), ==, SHELL_HOOK_VERDICT_CONTINUE);
  g_assert_cmpstr (marks->str, ==, "b1,bb1,");
  g_string_truncate (marks, 0);

  /* re-enable: its listeners come after b's now; the disabler is the second of three, the third never runs */
  enable_ok (&e, "a@test");
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 4);
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  g_assert_cmpstr (marks->str, ==, "b1,a1,a2,a2-after,");   /* the listener itself runs on to its end */
  g_string_truncate (marks, 0);
  assert_state (ea, SHELL_LUA_EXTENSION_STATE_INACTIVE);
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 1);
  g_assert_cmpuint (captured_count (NULL), ==, 0);

  /* disabling the survivor from the outside while it is not running anything */
  g_assert_true (shell_lua_extension_manager_disable (e.manager, "b@test", NULL));
  assert_no_listeners ();
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  g_assert_cmpstr (marks->str, ==, "");
  env_clear (&e);
  capture_end ();
  g_clear_object (&sa);
  g_clear_object (&ea);
  g_clear_object (&eb);
  fixture_end ();
}

/* Extension A's listener, called from inside extension B's listener (B emitted
 * the hook A listens to), disables B: B's call is still running. */
static void
test_hooks_disable_other_extension (void)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  Env e;
  g_autoptr (ShellLuaExtension) ea = NULL, eb = NULL;
  ShellLuaState *sb = NULL;

  fixture_begin (32);
  declare_hooks ();
  capture_begin ();
  env_init (&e);
  ea = install (&e, "a@test", NULL,
                "local shell = require('shell'); local fz = shell.fz\n"
                "return { enable = function()\n"
                "  shell.hooks.on('fz.gamma', function() fz.mark('A'); fz.mark('disable:' .. tostring(fz.disable_ext('b@test'))) end)\n"
                "end }\n");
  eb = install (&e, "b@test", NULL,
                "local shell = require('shell'); local fz = shell.fz\n"
                "local o1, o2\n"
                "return { enable = function()\n"
                "  o1 = fz.new{name = 'b1'}; o2 = fz.new{name = 'b2', parent = o1, on_poked = function() fz.mark('poked') end}\n"
                "  shell.timer.after{ms = 1, callback = function() fz.hit() end}\n"
                "  shell.hooks.on('fz.alpha', function()\n"
                "    fz.mark('B1')\n"
                "    fz.emit_after('fz.gamma')\n"                          /* A disables us in here */
                "    fz.mark('B2:' .. tostring(o1.alive) .. tostring(pcall(function() return o2.name end)))\n"
                "    fz.mark('B3:' .. tostring(fz.new{name = 'late'}.alive))\n"
                "  end)\n"
                "  shell.hooks.on('fz.alpha', function() fz.mark('B-second') end)\n"
                "end }\n");
  enable_ok (&e, "a@test");
  enable_ok (&e, "b@test");
  sb = shell_lua_extension_get_lua_state (eb);
  g_object_add_weak_pointer (G_OBJECT (sb), (gpointer *) &sb);

  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  g_assert_cmpstr (marks->str, ==, "B1,A,disable:true,B2:falsefalse,B3:true,");
  assert_state (eb, SHELL_LUA_EXTENSION_STATE_INACTIVE);
  assert_state (ea, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  g_assert_null (sb);
  g_assert_cmpstr (destroy_log->str, ==, "b2,b1,late,");
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 0);
  g_assert_cmpuint (listeners ("fz.gamma", SHELL_HOOK_PHASE_AFTER), ==, 1);
  g_assert_cmpint (live_objs, ==, 0);
  g_assert_cmpuint (captured_count (NULL), ==, 0);
  tu_spin (20);
  g_assert_cmpint (hits, ==, 0);

  g_assert_true (shell_lua_extension_manager_disable (e.manager, "a@test", NULL));
  assert_no_listeners ();
  env_clear (&e);
  capture_end ();
  g_clear_object (&ea);
  g_clear_object (&eb);
  fixture_end ();
}

/* Errors in listeners: the emission carries on, the count rises, the error
 * policy (more than five) disables the extension from an idle, never under
 * the emission; a spinning listener is cut by the watchdog and the next one
 * still runs. */
static void
test_hooks_errors_and_watchdog (void)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  Env e;
  g_autoptr (ShellLuaExtension) ext = NULL, good = NULL;
  g_autoptr (ShellLuaState) state = NULL;
  int i;

  fixture_begin (31);
  declare_hooks ();
  capture_begin ();
  env_init (&e);
  set_budget (&e, 40 * 1000, 0);

  ext = install (&e, "bad@test", NULL,
                 "local shell = require('shell'); local fz = shell.fz\n"
                 "local fails = 0\n"
                 "return { enable = function()\n"
                 "  for i = 1, 4 do\n"
                 "    shell.hooks.on('fz.alpha', function() fz.mark('e' .. i); error('listener ' .. i .. ' failed') end)\n"
                 "  end\n"
                 "  shell.hooks.on('fz.alpha', function() fz.mark('spin'); while true do end end)\n"
                 "  shell.hooks.on('fz.alpha', function() fz.mark('catch'); while true do pcall(function() while true do end end) end end)\n"
                 "  shell.hooks.on('fz.alpha', function() fz.mark('obj'); error(setmetatable({}, {__tostring = function() error('again') end})) end)\n"
                 "  shell.hooks.on('fz.alpha', function() fz.mark('last') end)\n"
                 "end }\n");
  good = install (&e, "good@test", NULL,
                  "local shell = require('shell'); local fz = shell.fz\n"
                  "return { enable = function()\n"
                  "  shell.hooks.on('fz.alpha', function() fz.mark('good') end)\n"
                  "end }\n");
  enable_ok (&e, "bad@test");
  enable_ok (&e, "good@test");
  state = g_object_ref (shell_lua_extension_get_lua_state (ext));

  /* one emission: seven failing listeners, every one of them ran, so did the good ones */
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  g_assert_cmpstr (marks->str, ==, "e1,e2,e3,e4,spin,catch,obj,last,good,");
  g_string_truncate (marks, 0);
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 7);
  g_assert_cmpuint (captured_count ("watchdog"), ==, 2);
  g_assert_cmpuint (captured_count (NULL), ==, 7);
  /* the policy fires later, from an idle; until then the extension is as it was */
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 9);
  captured_clear ();
  wait_for_state (ext, SHELL_LUA_EXTENSION_STATE_ERROR);
  g_assert_cmpuint (captured_count ("raised more than 5 errors"), ==, 1);
  g_assert_false (shell_lua_state_is_alive (state));
  g_assert_cmpuint (shell_lua_state_get_memory_usage (state), ==, 0);
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 1);
  assert_state (good, SHELL_LUA_EXTENSION_STATE_ACTIVE);
  shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
  g_assert_cmpstr (marks->str, ==, "good,");
  g_string_truncate (marks, 0);

  /* a watchdog trip in the before phase neither cancels nor amends, and the extension keeps working */
  captured_clear ();
  g_object_unref (state);
  state = NULL;
  g_clear_object (&ext);
  ext = install (&e, "spinner@test", NULL,
                 "local shell = require('shell'); local fz = shell.fz\n"
                 "return { enable = function()\n"
                 "  shell.hooks.before('fz.beta', function() fz.mark('spin'); while true do end end)\n"
                 "  shell.hooks.before('fz.beta', function() fz.mark('amend'); return {k = 'v'} end)\n"
                 "end }\n");
  enable_ok (&e, "spinner@test");
  for (i = 0; i < 3; i++)
    {
      GVariant *amendments = NULL;

      g_assert_cmpint (shell_hook_registry_emit_before (registry, "fz.beta", NULL, &amendments),
                       ==, SHELL_HOOK_VERDICT_CONTINUE);
      g_assert_nonnull (amendments);
      g_variant_unref (amendments);
      g_assert_cmpstr (marks->str, ==, "spin,amend,");
      g_string_truncate (marks, 0);
    }
  g_assert_cmpuint (captured_count ("watchdog"), ==, 3);
  g_assert_cmpuint (shell_lua_extension_get_error_count (ext), ==, 3);
  assert_state (ext, SHELL_LUA_EXTENSION_STATE_ACTIVE);

  g_assert_true (shell_lua_extension_manager_disable (e.manager, "spinner@test", NULL));
  g_assert_true (shell_lua_extension_manager_disable (e.manager, "good@test", NULL));
  assert_no_listeners ();
  env_clear (&e);
  capture_end ();
  g_clear_object (&state);
  g_clear_object (&ext);
  g_clear_object (&good);
  fixture_end ();
}

/* ======================================================================== */
/* 4. Limits                                                                 */
/* ======================================================================== */

static ShellLuaState *
new_limited (gint64 budget_us, gsize memory)
{
  ShellLuaLimits limits = { .callback_budget_us = budget_us, .memory_limit = memory };
  ShellLuaState *s = shell_lua_state_new ("limits", SHELL_LUA_STATE_SANDBOXED, &limits);

  g_assert_true (shell_lua_state_is_alive (s));
  return s;
}

/* Run @code, which must fail; returns the error code. A failed callback of a
 * sandboxed state logs a warning: captured. */
static ShellLuaError
run_expect_error (ShellLuaState *s, const char *code, const char *substr)
{
  g_autoptr (GError) error = NULL;
  ShellLuaError what;

  if (shell_lua_state_do_string (s, code, "=(fuzz)", &error))
    g_error ("chunk unexpectedly succeeded:\n%s", code);
  if (substr != NULL && strstr (error->message, substr) == NULL)
    g_error ("error message lacks '%s': %s\n--- chunk:\n%s", substr, error->message, code);
  what = (ShellLuaError) error->code;
  return what;
}

static void
assert_error_code (ShellLuaState *s, const char *code, ShellLuaError expected, const char *substr)
{
  ShellLuaError got = run_expect_error (s, code, substr);

  if (got != expected)
    g_error ("wrong error kind %d (wanted %d) for:\n%s", (int) got, (int) expected, code);
}

/* Lua stack overflow, the memory limit or the watchdog, whichever comes first */
static void
assert_resource_error (ShellLuaState *s, const char *code, const char *substr)
{
  ShellLuaError got = run_expect_error (s, code, NULL);

  if (got != SHELL_LUA_ERROR_RUNTIME && got != SHELL_LUA_ERROR_MEMORY && got != SHELL_LUA_ERROR_WATCHDOG)
    g_error ("wrong error kind %d for:\n%s", (int) got, code);
  (void) substr;
}

static void
test_limits_memory (void)
{
  const gsize limit = 4 * 1024 * 1024;
  ShellLuaState *s;
  guint errors;

  fixture_begin (50);
  capture_begin ();
  s = new_limited (2 * G_USEC_PER_SEC, limit);

  assert_error_code (s, "local t = {} while true do t[#t + 1] = {} end", SHELL_LUA_ERROR_MEMORY, "not enough memory");
  assert_error_code (s, "local s = 'x' while true do s = s .. s end", SHELL_LUA_ERROR_MEMORY, "not enough memory");
  assert_error_code (s, "return string.rep('x', 1 << 30)", SHELL_LUA_ERROR_MEMORY, "not enough memory");
  assert_error_code (s, "local t = {} while true do t[#t + 1] = coroutine.create(function() end) end",
                     SHELL_LUA_ERROR_MEMORY, "not enough memory");
  assert_error_code (s, "local t = {} for i = 1, 1e7 do t[i] = i end", SHELL_LUA_ERROR_MEMORY, "not enough memory");
  assert_error_code (s, "local t = {} while true do local f = function() return t end t[#t + 1] = f end",
                     SHELL_LUA_ERROR_MEMORY, "not enough memory");
  assert_error_code (s, "local t = {} for i = 1, 1e7 do t['k' .. i] = ('v'):rep(10) .. i end",
                     SHELL_LUA_ERROR_MEMORY, "not enough memory");
  assert_error_code (s, "local t = {} for i = 1, 100 do t[i] = ('x'):rep(100000) end return table.concat(t, ',', 1, 100):rep(100)",
                     SHELL_LUA_ERROR_MEMORY, "not enough memory");
  assert_error_code (s, "return string.rep('xx', 1 << 62)", SHELL_LUA_ERROR_RUNTIME, "resulting string too large");
  assert_error_code (s, "return table.unpack({}, 1, 1e8)", SHELL_LUA_ERROR_RUNTIME, "too many results");
  assert_error_code (s, "local t = {} t[1 << 40] = 1; return table.concat(t, '', 1, 1 << 40)", SHELL_LUA_ERROR_RUNTIME, NULL);
  assert_error_code (s, "return ('x'):rep(1e9, ',')", SHELL_LUA_ERROR_MEMORY, NULL);
  g_assert_cmpuint (shell_lua_state_get_memory_usage (s), <=, limit);

  /* the owner of a memory error can catch it and carry on, any number of times */
  DO (s,
     "local caught = 0\n"
     "for round = 1, 12 do\n"
     "  local ok, err = pcall(function() local t = {} while true do t[#t + 1] = ('x'):rep(500) .. #t end end)\n"
     "  assert(not ok and tostring(err):find('not enough memory', 1, true), tostring(err))\n"
     "  caught = caught + 1\n"
     "  local small = {} for i = 1, 200 do small[i] = {i} end\n"
     "  assert(#small == 200)\n"
     "end\n"
     "assert(caught == 12)\n");

  /* the objects the API makes are bounded too (4096 slots), long before the memory runs out */
  DO (s,
     "local made = {}\n"
     "local ok, err = pcall(function() while true do made[#made + 1] = fz.new{name='n'} end end)\n"
     "assert(not ok and tostring(err):find('too many objects (limit 4096)', 1, true), tostring(err))\n"
     "assert(#made == 4096 and fz.count() == 4096)\n"
     "for i = 1, #made, 2 do made[i]:destroy() end\n"
     "assert(fz.count() == 2048)\n"
     "for i = 1, 2048 do made[#made + 1] = fz.new{} end\n"            /* freed slots are reusable */
     "assert(fz.count() == 4096)\n"
     "for _, h in ipairs(made) do h:destroy() end\n"
     "assert(fz.count() == 0)\n");

  errors = shell_lua_state_get_error_count (s);
  g_assert_cmpuint (errors, >=, 12);
  shell_lua_state_gc (s);
  assert_state_usable (s);
  g_object_unref (s);
  capture_end ();
  fixture_end ();
}

/* The allocator refuses at arbitrary points inside the API: every failure is
 * a Lua error, nothing is half done, nothing leaks. */
static void
test_limits_memory_injection (void)
{
  const gsize limit = 256 * 1024;
  ShellLuaState *s;
  char *code;

  fixture_begin (51);
  declare_hooks ();
  capture_begin ();
  s = new_limited (30 * G_USEC_PER_SEC, limit);

  code = g_strdup_printf (
    PRELUDE
    "local LIMIT = %" G_GSIZE_FORMAT "\n"
    "local R = fz.rand\n"
    "local made, fails, successes = {}, 0, 0\n"
    "local actions = {\n"
    "  function() made[#made + 1] = fz.new{name = 'm'} end,\n"
    "  function() made[#made + 1] = fz.new{name = 'm', on_poked = function() end} end,\n"
    "  function() shell.timer.after{ms = 100000, callback = function() end} end,\n"
    "  function() shell.timer.every{ms = 100000, callback = function() end} end,\n"
    "  function() shell.timer.idle{callback = function() end}:cancel() end,\n"
    "  function() shell.hooks.on('fz.alpha', function() end) end,\n"
    "  function() shell.hooks.before('fz.beta', function() end):disconnect() end,\n"
    "  function() local o = fz.new{name = 'c'}; made[#made + 1] = o; o:on_poked(function() end) end,\n"
    "  function() local o = fz.new_sub(); made[#made + 1] = o; fz.share(o); assert(fz.take() == o) end,\n"
    "  function() fz.ephemeral() end,\n"
    "  function() local o = fz.new{}; o.name = 'x'; o.tags = {'a', 'b'}; o.flags = {a = true}; o.any = {1, 2}; o:destroy() end,\n"
    "  function() local t = {} for i = 1, 50 do t[i] = {i} end end,\n"
    "  function() fz.emit_after('fz.alpha', {k = 'v'}) end,\n"
    "  function() fz.emit_before('fz.beta', {k = ('v'):rep(100)}) end,\n"
    "  function() local c = coroutine.wrap(function() fz.new{} end) c() end,\n"
    "}\n"
    "for round = 1, 300 do\n"
    "  local ballast = nil\n"
    "  fz.gc()\n"                                  /* so that the ballast below is all that is live */
    "  local headroom = R(R(2) == 1 and 150 or 1200)\n"
    "  pcall(function()\n"
    "    local filler = ('c'):rep(24)\n"
    "    while fz.mem() + headroom < LIMIT - 200 do\n"
    "      ballast = { ballast, filler }\n"      /* a linked list that makes no garbage and needs no resizing */
    "    end\n"
    "  end)\n"
    "  for k = 1, 3 do\n"
    "    local which = R(#actions)\n"
    "    local ok, err = pcall(actions[which])\n"
    "    if ok then successes = successes + 1 else fails = fails + 1 end\n"
    "  end\n"
    "  ballast = nil\n"
    "end\n"
    "assert(fails > 20 and successes > 20, fails .. '/' .. successes)\n"
    "result = fails\n",
    limit);
  tu_do (s, code);
  g_free (code);
  /* the listeners that failed for want of memory were reported, nothing else was */
  g_assert_cmpuint (captured_count ("not enough memory") + captured_count ("error in error handling"), ==,
                    captured_count (NULL));

  /* the leftovers are coherent: what the registry owns is what the state reports, then all of it goes */
  shell_lua_state_gc (s);
  g_assert_cmpuint (shell_lua_n_owned_objects (shell_lua_state_get_L (s)), ==, (guint) live_objs - (guint) others->len);
  shell_lua_objects_destroy_all (s);
  g_assert_cmpuint (shell_lua_n_owned_objects (shell_lua_state_get_L (s)), ==, 0);
  assert_state_usable (s);
  g_object_unref (s);
  assert_no_listeners ();
  capture_end ();
  fixture_end ();
}

static void
test_limits_recursion (void)
{
  ShellLuaState *s;
  g_autofree char *deep_parens = NULL, *deep_tables = NULL, *deep_concat = NULL, *many_locals = NULL;
  GString *locals;
  int i;

  fixture_begin (52);
  declare_hooks ();
  capture_begin ();
  s = new_limited (2 * G_USEC_PER_SEC, 16 * 1024 * 1024);

  assert_resource_error (s, "local function f() return 1 + f() end return f()", NULL);
  assert_resource_error (s, "local mt = {} mt.__index = function(t, k) return t[k] end return setmetatable({}, mt).x", NULL);
  assert_resource_error (s, "local mt = {} mt.__newindex = function(t, k, v) t[k] = v end setmetatable({}, mt).x = 1", NULL);
  assert_resource_error (s, "local mt = {} mt.__call = function(t) return t() + 1 end return setmetatable({}, mt)()", NULL);
  assert_resource_error (s, "local mt = {} mt.__tostring = function(t) return tostring(t) end return tostring(setmetatable({}, mt))", NULL);
  assert_resource_error (s, "local mt = {} mt.__concat = function(a, b) return a .. b end return setmetatable({}, mt) .. 'x'", NULL);
  assert_resource_error (s, "local mt = {} mt.__eq = function(a, b) return a == b end return setmetatable({}, mt) == setmetatable({}, mt)", NULL);
  assert_resource_error (s, "local function f() return coroutine.wrap(f)() end return f()", NULL);
  assert_resource_error (s, "local function f(s) return (s:gsub('.', f)) end return f('x')", NULL);
  assert_resource_error (s, "local function f() table.sort({3, 2, 1}, function(a, b) f() return a < b end) end f()", NULL);
  assert_resource_error (s, "local function f() return select('#', pcall(f)) + f() end return f()", NULL);
  assert_resource_error (s, "local function f(n) if n == 0 then error('deep') end return 1 + f(n - 1) end return f(1e6)", NULL);
  assert_error_code (s, "local function f(n) if n == 0 then error('deep') end return 1 + f(n - 1) end return f(15000)",
                     SHELL_LUA_ERROR_RUNTIME, "deep");
  assert_error_code (s, "error(string.rep('x', 1 << 20))", SHELL_LUA_ERROR_RUNTIME, "xxxxxxxxxxxxxxxx");
  assert_error_code (s, "error(setmetatable({}, {__tostring = function() return string.rep('y', 1 << 20) end}))",
                     SHELL_LUA_ERROR_RUNTIME, "yyyyyyyyyyyyyyyy");
  /* pcall catches the overflow at the deepest level and everything unwinds normally */
  DO (s, "local function f() return pcall(f) end f()\n"
        "local depth = 0\n"
        "local function g() depth = depth + 1; local ok = pcall(g); return ok end\n"
        "g()\n"
        "assert(depth > 100)\n");

  /* parser limits are syntax errors, not crashes */
  {
    g_autofree char *open_paren = g_strnfill (5000, '('), *close_paren = g_strnfill (5000, ')');
    g_autofree char *open_brace = g_strnfill (5000, '{'), *close_brace = g_strnfill (5000, '}');

    deep_parens = g_strdup_printf ("return %s1%s", open_paren, close_paren);
    deep_tables = g_strdup_printf ("return %s%s", open_brace, close_brace);
  }
  {
    GString *c = g_string_new ("return 1");

    for (i = 0; i < 100000; i++)
      g_string_append (c, " .. 1");
    deep_concat = g_string_free (c, FALSE);
  }
  locals = g_string_new ("local a0");
  for (i = 1; i < 400; i++)
    g_string_append_printf (locals, ", a%d", i);
  g_string_append (locals, " = 1");
  many_locals = g_string_free (locals, FALSE);
  assert_error_code (s, deep_parens, SHELL_LUA_ERROR_SYNTAX, NULL);
  assert_error_code (s, deep_tables, SHELL_LUA_ERROR_SYNTAX, NULL);
  assert_error_code (s, deep_concat, SHELL_LUA_ERROR_SYNTAX, NULL);
  assert_error_code (s, many_locals, SHELL_LUA_ERROR_SYNTAX, "too many");
  assert_error_code (s, "return \"unfinished", SHELL_LUA_ERROR_SYNTAX, NULL);
  assert_error_code (s, "\x1bLua", SHELL_LUA_ERROR_SYNTAX, NULL);
  assert_error_code (s, "goto nowhere", SHELL_LUA_ERROR_SYNTAX, NULL);

  /* callbacks that call themselves: the nesting limit of protected calls ends it */
  DO (s,
     "o = fz.new{name='rec'}\n"
     "calls = 0\n"
     "o:on_poked(function(self) calls = calls + 1; self:poke(1) end)\n"
     "local errors = fz.error_count()\n"
     "o:poke(1)\n"
     "assert(calls > 10 and calls <= 51, calls)\n"
     "assert(fz.error_count() > errors)\n"
     "o:destroy()\n"
     "assert(not o.alive, 'the emitting frames were not unwound')\n"
     "assert(fz.count() == 0)\n");

  /* data too deep or cyclic for the variant conversion of a property */
  DO (s,
     "local o = fz.new{}\n"
     "local deep = {} local t = deep for i = 1, 300 do t[1] = {} t = t[1] end\n"
     "local ok, err = pcall(function() o.any = deep end)\n"
     "assert(not ok and tostring(err):find('nested too deeply', 1, true), tostring(err))\n"
     "local cyclic = {} cyclic[1] = cyclic\n"
     "ok, err = pcall(function() o.any = cyclic end)\n"
     "assert(not ok, 'a cyclic table was accepted')\n"
     "ok, err = pcall(function() fz.emit_after('fz.alpha', {k = cyclic}) end)\n"
     "assert(not ok)\n"
     "ok = pcall(function() o.tags = cyclic end)\n"
     "assert(not ok)\n"
     "o:destroy()\n");

  assert_state_usable (s);
  g_object_unref (s);
  capture_end ();
  fixture_end ();
}

static void
test_limits_watchdog (void)
{
  ShellLuaState *s;
  guint errors;
  GObject *obj;

  fixture_begin (53);
  declare_hooks ();
  capture_begin ();
  s = new_limited (30 * 1000, 0);

  assert_error_code (s, "while true do end", SHELL_LUA_ERROR_WATCHDOG, "watchdog");
  assert_error_code (s, "for i = 1, math.huge do end", SHELL_LUA_ERROR_WATCHDOG, "watchdog");
  assert_error_code (s, "repeat until false", SHELL_LUA_ERROR_WATCHDOG, "watchdog");
  assert_error_code (s, "local t = {} while true do t = {t} end", SHELL_LUA_ERROR_WATCHDOG, "watchdog");
  assert_error_code (s, "local s = '' while true do s = ('x'):rep(100) .. #s end", SHELL_LUA_ERROR_WATCHDOG, "watchdog");
  assert_error_code (s, "::top:: goto top", SHELL_LUA_ERROR_WATCHDOG, "watchdog");
  /* catching the error with pcall does not buy any time */
  assert_error_code (s, "local ok = pcall(function() while true do end end); local n = 0; while true do n = n + 1 end",
                     SHELL_LUA_ERROR_WATCHDOG, "watchdog");
  assert_error_code (s, "while true do pcall(function() while true do end end) end", SHELL_LUA_ERROR_WATCHDOG, "watchdog");
  assert_error_code (s, "local co = coroutine.wrap(function() while true do end end); while true do pcall(co) end",
                     SHELL_LUA_ERROR_WATCHDOG, "watchdog");
  assert_error_code (s, "local co = coroutine.create(function() while true do coroutine.yield() end end)\n"
                        "while true do coroutine.resume(co) end", SHELL_LUA_ERROR_WATCHDOG, "watchdog");
  /* spinning inside an API callback, called from inside the script */
  assert_error_code (s, PRELUDE
                        "local o = fz.new{on_poked = function() while true do end end}\n"
                        "o:poke(1)\n"
                        "while true do end", SHELL_LUA_ERROR_WATCHDOG, "watchdog");
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, 12);   /* ten scripts, the outer one and the callback that was cut */

  /* the same from C, outside any script: the callback is the outermost call */
  DO (s,
     "q = fz.new{name='q', on_poked = function() while true do pcall(function() while true do end end) end end}\n"
     "fz.share(q)\n");
  obj = g_object_ref (shared);
  errors = shell_lua_state_get_error_count (s);
  g_signal_emit (obj, obj_signals[SIG_POKED], 0, 1);
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, errors + 1);
  g_signal_emit (obj, obj_signals[SIG_POKED], 0, 2);        /* a fresh budget each time */
  g_assert_cmpuint (shell_lua_state_get_error_count (s), ==, errors + 2);
  g_object_unref (obj);
  g_assert_cmpuint (captured_count ("watchdog"), >=, 14);
  captured_clear ();

  /* a hook listener that spins halfway through an emission: the ones after it still run */
  DO (s,
     "seen = {}\n"
     "shell.hooks.on('fz.alpha', function() seen[#seen + 1] = 1 end)\n"
     "shell.hooks.on('fz.alpha', function() seen[#seen + 1] = 2; while true do end end)\n"
     "shell.hooks.on('fz.alpha', function() seen[#seen + 1] = 3 end)\n");
  shell_hook_registry_emit_after (shell_hook_registry_get_default (), "fz.alpha", NULL);
  shell_hook_registry_emit_after (shell_hook_registry_get_default (), "fz.alpha", NULL);
  tu_assert_eval_str (s, "table.concat(seen, ',')", "1,2,3,1,2,3");
  g_assert_cmpuint (captured_count ("watchdog"), ==, 2);

  assert_state_usable (s);
  g_object_unref (s);
  assert_no_listeners ();
  capture_end ();
  fixture_end ();
}

/* A state destroyed by the code it runs: the rest of that code still runs,
 * but anything it creates dies with the state, and the state ends cleanly dead. */
static void
test_limits_state_destroyed_from_inside (void)
{
  ShellLuaState *s;
  g_autoptr (GError) error = NULL;
  int hits_before;

  fixture_begin (54);
  declare_hooks ();
  s = new_priv ();
  DO (s,
     "o = fz.new{name='o', on_poked = function() fz.hit() end}\n"
     "t = shell.timer.every{ms=1, callback=function() fz.hit() end}\n"
     "c = shell.hooks.on('fz.alpha', function() fz.hit() end)\n"
     "fz.mark('before')\n");
  g_assert_cmpuint (listeners ("fz.alpha", SHELL_HOOK_PHASE_AFTER), ==, 1);

  tu_assert_fails (s,
                   PRELUDE
                   "fz.kill_state()\n"
                   "fz.mark('still-running:' .. tostring(o.alive))\n"
                   "o:poke(1)\n"
                   "fz.new{name='late'}\n"
                   "shell.timer.after{ms=1, callback=function() fz.hit() end}\n"
                   "shell.timer.idle{callback=function() fz.hit() end}\n"
                   "shell.hooks.on('fz.alpha', function() fz.hit() end)\n"
                   "c:disconnect(); t:cancel()\n",
                   SHELL_LUA_ERROR_DEAD, "destroyed during the call");
  g_assert_cmpstr (marks->str, ==, "before,still-running:true,");
  g_assert_false (shell_lua_state_is_alive (s));
  g_assert_null (shell_lua_state_get_L (s));
  g_assert_cmpuint (shell_lua_state_get_memory_usage (s), ==, 0);
  g_assert_cmpstr (destroy_log->str, ==, "late,o,");
  assert_no_listeners ();

  /* a dead state refuses everything politely */
  tu_assert_fails (s, "return 1", SHELL_LUA_ERROR_DEAD, "dead");
  g_assert_false (shell_lua_state_load_string (s, "return 1", "=x", &error));
  g_assert_error (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_DEAD);
  g_clear_error (&error);
  shell_lua_state_gc (s);
  shell_lua_state_destroy (s);
  hits_before = hits;
  tu_spin (40);
  g_assert_cmpint (hits, ==, hits_before);
  g_object_unref (s);
  fixture_end ();
}

/* ======================================================================== */
/* 5. Random sequences                                                       */
/* ======================================================================== */

/* The fuzzer proper. Every operation runs under pcall; errors are expected
 * (that is the point) but must never be internal ones. Between batches the
 * script's own bookkeeping is compared with the runtime's registries:
 *   - the owned-object registry holds exactly the objects whose handle is alive;
 *   - the hook registry holds exactly the connections that report connected;
 *   - nothing was destroyed twice.
 * Callbacks run more random operations (to a bounded depth), so listeners
 * connect, disconnect and destroy things from inside emissions. All random
 * choices come from fz.rand(), i.e. from the seeded GRand. */
static const char fuzz_program[] =
  "local shell = require('shell')\n"
  "local fz = shell.fz\n"
  "local R, frand, trace = fz.rand, fz.frand, fz.trace\n"
  "local F = {}\n"
  "local objs, hconns, oconns, timers = {}, {}, {}, {}\n"
  "local stats = { ops = 0, errors = 0, calls = 0, internal = 0 }\n"
  "local depth, dyn, timers_made, st = 0, 0, 0, nil\n"
  "local hook_names = { 'fz.alpha', 'fz.beta', 'fz.gamma', 'fz.alpha', 'fz.beta', 'no.such-hook', 'Bad Name', '' }\n"
  "local props = { 'name', 'i', 'u', 'l', 'd', 'f', 'b', 's', 'mode', 'flags', 'tags', 'friend', 'any', 'vs', 'ro', 'co',\n"
  "  'wo', 'ptr', 'ch', 'uc', 'ul', 'lg', 'u64', 'act', 'ghost', 'alias', 'hidden', 'alive', 'double', 'label', 'boom',\n"
  "  'nonexistent', '', 'destroy' }\n"
  "local methods = { 'poke', 'on_poked', 'plain', 'external_destroy', 'defer', 'destroy', 'deadok', 'sub_only', 'nope' }\n"
  "local keys = { 'flag', 'count', 'ratio', 'name', 'list', 'nope', '' }\n"
  "local step\n"
  "local function coin(p) return frand() < (p or 0.5) end\n"
  "local function pick(t) if #t == 0 then return nil end return t[R(#t)] end\n"
  "local function hook() return pick(hook_names) end\n"
  "local function junk()\n"
  "  local k = R(17)\n"
  "  if k == 1 then return nil elseif k == 2 then return true elseif k == 3 then return false\n"
  "  elseif k == 4 then return R(100) - 50 elseif k == 5 then return frand() * 2000 - 1000\n"
  "  elseif k == 6 then return math.maxinteger elseif k == 7 then return 0 / 0\n"
  "  elseif k == 8 then return 'str' .. R(50) elseif k == 9 then return ''\n"
  "  elseif k == 10 then return {} elseif k == 11 then return { R(9), 'x', true }\n"
  "  elseif k == 12 then return tostring elseif k == 13 then return pick(objs)\n"
  "  elseif k == 14 then local h = pick(hconns); return h and h.c\n"
  "  elseif k == 15 then return fz.lud() elseif k == 16 then return { a = { b = { c = junk } } }\n"
  "  else return coroutine.create(print) end\n"
  "end\n"
  /* a callback that does more random things, and fails or answers in random ways */
  "local function make_cb()\n"
  "  return function(...)\n"
  "    stats.calls = stats.calls + 1\n"
  "    fz.hit()\n"
  "    local r = R(12)\n"
  "    if depth < 4 then\n"
  "      depth = depth + 1\n"
  "      for i = 1, R(3) do pcall(step) end\n"
  "      depth = depth - 1\n"
  "    end\n"
  "    if r == 1 then error('callback failed') elseif r == 2 then error({}) elseif r == 3 then\n"
  "      local x = 0 for i = 1, 3000 do x = x + i end\n"
  "    elseif r == 4 then return shell.hooks.CANCEL elseif r == 5 then return { k = 'v', n = R(9) }\n"
  "    elseif r == 6 then return junk() elseif r == 7 then return false end\n"
  "  end\n"
  "end\n"
  /* callbacks that must not use the random stream (their timing is real time) */
  "local function plain_cb()\n"
  "  return function()\n"
  "    stats.calls = stats.calls + 1\n"
  "    fz.hit()\n"
  "    if stats.calls % 11 == 0 then error('plain callback failed') end\n"
  "    return stats.calls % 3 ~= 0\n"
  "  end\n"
  "end\n"
  /* callbacks that call back into the objects they listen to fan out without end (n callbacks, n^50 calls):
   * bound the nesting of those the operations below create */
  "local function guarded(f)\n"
  "  return function(...)\n"
  "    if depth >= 4 then return end\n"
  "    depth = depth + 1\n"
  "    local ok, err = pcall(f, ...)\n"
  "    depth = depth - 1\n"
  "    if not ok then error(err, 0) end\n"
  "  end\n"
  "end\n"
  "local ops = {}\n"
  "local function op(name, f) ops[#ops + 1] = { name = name, f = f } end\n"
  "local function an_object() local o = pick(objs); return o end\n"
  "op('new', function()\n"
  "  local t = { name = 'o' .. R(1000) }\n"
  "  if coin(0.3) then t.parent = pick(objs) end\n"
  "  if coin(0.5) then t.on_poked = make_cb() end\n"
  "  if coin(0.05) then t.bogus = 1 end\n"
  "  objs[#objs + 1] = fz.new(t)\n"
  "end)\n"
  "op('new-sub', function() objs[#objs + 1] = fz.new_sub() end)\n"
  "op('new-junk', function() objs[#objs + 1] = fz.new(junk()) end)\n"
  "op('new-fields', function() objs[#objs + 1] = fz.new{ name = junk(), parent = junk(), on_poked = junk(), co = junk() } end)\n"
  "op('temp', function() local t = fz.new{ name = 'tmp' } if coin() then t:destroy() else objs[#objs + 1] = t end end)\n"
  "op('ephemeral', function() local e = fz.ephemeral(); assert(not e.alive); e:destroy() end)\n"
  "op('destroy', function() local o = an_object(); if o then o:destroy() end end)\n"
  "op('destroy-twice', function() local o = an_object(); if o then o:destroy(); o:destroy() end end)\n"
  "op('external-destroy', function() local o = an_object(); if o then o:external_destroy() end end)\n"
  "op('poke', function() local o = an_object(); if o then o:poke(junk()) end end)\n"
  "op('defer', function() local o = an_object(); if o then o:defer() end end)\n"
  "op('set', function() local o = an_object(); if o then o[pick(props)] = junk() end end)\n"
  "op('get', function() local o = an_object(); if o then local v = o[pick(props)] end end)\n"
  "op('set-friend', function() local o = an_object(); if o then o.friend = coin() and pick(objs) or nil end end)\n"
  "op('index-junk', function() local o = an_object(); if o then local v = o[junk()] end end)\n"
  "op('call-junk', function() local o = an_object(); if o then o[pick(methods)](o, junk()) end end)\n"
  "op('method-wrong-self', function() local o = an_object(); if o then o[pick(methods)](junk(), junk()) end end)\n"
  "op('take', function() local o = an_object(); if o and o.alive then fz.share(o); assert(fz.take() == o, 'identity') end end)\n"
  "op('invalidate', function()\n"
  "  local i = R(math.max(#objs, 1)); local o = objs[i]\n"
  "  if not o then return end\n"
  "  if not o.alive then fz.invalidate(o); return end\n"
  /* the handle dies, the object lives on: a fresh handle replaces it, and is a different value */
  "  fz.share(o); fz.invalidate(o, coin())\n"
  "  assert(not o.alive)\n"
  "  local n = fz.take()\n"
  "  assert(n.alive and n ~= o)\n"
  "  objs[i] = n\n"
  "end)\n"
  "op('members', function() local o = an_object(); if o then assert(fz.members(o) > 20) end end)\n"
  "op('tostring', function() local o = an_object(); if o then assert(type(tostring(o)) == 'string') end end)\n"
  "op('compare', function() local o, p = an_object(), an_object(); if o and p then local e = (o == p) end end)\n"
  "op('on-poked', function() local o = an_object(); if o then oconns[#oconns + 1] = o:on_poked(make_cb()) end end)\n"
  "op('on-poked-junk', function() local o = an_object(); if o then o:on_poked(junk()) end end)\n"
  "op('oconn', function() local c = pick(oconns); if c then if coin() then c:disconnect() else local x = c:is_connected() end end end)\n"
  "op('hook-on', function() local n = hook(); hconns[#hconns + 1] = { c = shell.hooks.on(n, make_cb()), name = n, phase = 'after' } end)\n"
  "op('hook-before', function() local n = hook(); hconns[#hconns + 1] = { c = shell.hooks.before(n, make_cb()), name = n, phase = 'before' } end)\n"
  "op('hook-junk', function() shell.hooks.on(junk(), junk()) end)\n"
  "op('hook-before-junk', function() shell.hooks.before(junk(), junk()) end)\n"
  "op('hook-disconnect', function() local h = pick(hconns); if h then h.c:disconnect() end end)\n"
  "op('hook-disconnect-all', function() if coin(0.2) then for _, h in ipairs(hconns) do h.c:disconnect() end end end)\n"
  "op('hook-emit-after', function() fz.emit_after(hook(), coin() and { k = junk() } or nil) end)\n"
  "op('hook-emit-before', function() fz.emit_before(hook(), coin() and { k = junk() } or nil) end)\n"
  "op('hook-list', function() local l = shell.hooks.list(); assert(#l >= 3) end)\n"
  "op('hook-declare', function() dyn = dyn + 1; local n = 'fz.fuzz-' .. dyn .. '.ev'; fz.declare(n, dyn % 2 == 0); hook_names[#hook_names + 1] = n end)\n"
  "op('timer-after', function()\n"
  "  if timers_made > 40 then return end\n"
  "  timers_made = timers_made + 1\n"
  "  local ms = R(4) - 1\n"
  "  timers[#timers + 1] = shell.timer.after{ ms = ms, callback = ms == 0 and make_cb() or plain_cb() }\n"
  "end)\n"
  "op('timer-after-positional', function()\n"
  "  if timers_made > 40 then return end\n"
  "  timers_made = timers_made + 1\n"
  "  timers[#timers + 1] = shell.timer.after(R(3), plain_cb())\n"
  "end)\n"
  "op('timer-every', function()\n"
  "  if timers_made > 40 then return end\n"
  "  timers_made = timers_made + 1\n"
  "  timers[#timers + 1] = shell.timer.every{ ms = R(3), callback = plain_cb() }\n"
  "end)\n"
  "op('timer-idle', function()\n"
  "  if timers_made > 40 then return end\n"
  "  timers_made = timers_made + 1\n"
  "  timers[#timers + 1] = shell.timer.idle{ callback = make_cb() }\n"
  "end)\n"
  "op('timer-junk', function() shell.timer.after(junk(), junk()); shell.timer.every{ ms = junk(), callback = junk() } end)\n"
  "op('timer-cancel', function() local t = pick(timers); if t then t:cancel(); assert(t.active == false) end end)\n"
  "op('timer-active', function() local t = pick(timers); if t then local a = t.active end end)\n"
  "op('gettext', function() shell.gettext(junk()); shell.ngettext(junk(), junk(), junk()); shell.pgettext(junk(), junk()) end)\n"
  "op('log', function()\n"
  "  local f = pick{ shell.log.debug, shell.log.info, shell.log.warning }\n"
  "  f(junk(), junk())\n"
  "end)\n"
  "op('shell-junk', function() local x = shell[junk()]; shell.nonexistent = nil end)\n"
  "op('settings', function()\n"
  "  if not PRIVILEGED then return end\n"
  "  st = st or shell.settings.open('org.gnome-lua.test')\n"
  "  local k = pick(keys)\n"
  "  local r = R(8)\n"
  "  if r == 1 then st:get(k) elseif r == 2 then st:set(k, junk()) elseif r == 3 then st:get_int(k)\n"
  "  elseif r == 4 then st:set_int(k, junk()) elseif r == 5 then st:set_string(k, junk())\n"
  "  elseif r == 6 then st:reset(k) elseif r == 7 then st:keys()\n"
  "  else oconns[#oconns + 1] = st:on_changed(coin() and k or nil, plain_cb()) end\n"
  "end)\n"
  "op('settings-junk', function() if PRIVILEGED then shell.settings.open(junk()) else shell.settings.open('x') end end)\n"
  "op('gc', function() if PRIVILEGED then collectgarbage() else collectgarbage('count') end end)\n"
  "op('emit-poked-from-callback-chain', function()\n"
  "  local a, b = an_object(), an_object()\n"
  "  if a and b then a:on_poked(guarded(function() b:poke(1); b:destroy() end)); a:poke(1) end\n"
  "end)\n"
  "op('destroy-in-callback', function()\n"
  "  local o = an_object(); local victim = an_object()\n"
  "  if o and victim then o:on_poked(guarded(function() victim:destroy(); o:destroy() end)); o:poke(1) end\n"
  "end)\n"
  "op('disconnect-in-callback', function()\n"
  "  local n = hook(); local self_conn\n"
  "  self_conn = shell.hooks.on(n, guarded(function() self_conn:disconnect(); for _, h in ipairs(hconns) do if coin(0.3) then h.c:disconnect() end end end))\n"
  "  hconns[#hconns + 1] = { c = self_conn, name = n, phase = 'after' }\n"
  "end)\n"
  "op('coroutine-op', function()\n"
  "  local inner = pick(ops)\n"
  "  local co = coroutine.wrap(inner.f)\n"
  "  co()\n"
  "end)\n"
  "op('coroutine-yield-op', function()\n"
  "  local inner, inner2 = pick(ops), pick(ops)\n"
  "  local co = coroutine.create(function() pcall(inner.f); coroutine.yield(1); inner2.f() end)\n"
  "  coroutine.resume(co); coroutine.resume(co); coroutine.resume(co)\n"
  "end)\n"
  "op('coroutine-callback', function()\n"
  "  local co = coroutine.wrap(function() coroutine.yield(); local o = fz.new{ name = 'co', on_poked = make_cb() }; objs[#objs + 1] = o; o:poke(1) end)\n"
  "  co(); co()\n"
  "end)\n"
  "op('api-emit', function()\n"
  "  local f = coin() and shell.hooks.emit_after or shell.hooks.emit_before\n"
  "  f(hook(), coin() and { k = junk() } or nil)\n"
  "end)\n"
  "op('connection-junk', function()\n"
  "  local c = pick(oconns) or (pick(hconns) or {}).c\n"
  "  if not c then return end\n"
  "  local m = pick{ 'disconnect', 'is_connected' }\n"
  "  c[m](junk()); c[m](c); local x = c.alive; local y = tostring(c)\n"
  "end)\n"
  "op('churn-handles', function() local o = an_object(); if o and o.alive then fz.share(o); for i = 1, 30 do assert(fz.take() == o) end end end)\n"
  "op('require', function() local m = require(pick{ 'shell', 'nonexistent', '', '..', 'a.b' }) end)\n"
  "op('pcall-nest', function() pcall(pcall, pcall, step) end)\n"
  "op('xpcall', function() xpcall(step, function(e) pcall(step); return e end) end)\n"
  "step = function()\n"
  "  local o = pick(ops)\n"
  "  stats.ops = stats.ops + 1\n"
  "  trace(o.name)\n"
  "  local ok, err = pcall(o.f)\n"
  "  if not ok then\n"
  "    stats.errors = stats.errors + 1\n"
  "    local msg = type(err) == 'string' and err or ''\n"
  "    for _, bad in ipairs{ 'internal error', 'cannot store the callback', 'not a handle', 'stack traceback', 'invalid hook name' } do\n"
  "      if msg:find(bad, 1, true) then stats.internal = stats.internal + 1; error('unexpected error from ' .. o.name .. ': ' .. msg) end\n"
  "    end\n"
  "  end\n"
  "end\n"
  "function F.run(n) for i = 1, n do step() end end\n"
  "function F.check()\n"
  "  timers_made = 0\n"
  "  local alive = 0\n"
  "  for _, o in ipairs(objs) do\n"
  "    if o.alive then alive = alive + 1 end\n"
  "    assert(o:deadok() == (o.alive and 'alive' or 'dead'))\n"
  "  end\n"
  "  assert(fz.count() == alive, 'owned objects: registry has ' .. fz.count() .. ', script has ' .. alive .. ' alive')\n"
  "  local want = {}\n"
  "  for _, h in ipairs(hconns) do\n"
  "    if h.c:is_connected() then local key = h.name .. '/' .. h.phase; want[key] = (want[key] or 0) + 1 end\n"
  "  end\n"
  "  local seen = {}\n"
  "  for _, name in ipairs(hook_names) do\n"
  "    if fz.is_declared(name) and not seen[name] then\n"
  "      seen[name] = true\n"
  "      for _, phase in ipairs{ 'before', 'after' } do\n"
  "        local have = fz.listeners(name, phase)\n"
  "        assert(have == (want[name .. '/' .. phase] or 0), 'listeners of ' .. name .. '/' .. phase .. ': registry ' .. have .. ', script ' .. (want[name .. '/' .. phase] or 0))\n"
  "      end\n"
  "    end\n"
  "  end\n"
  "  assert(fz.stats().double == 0, 'an object was destroyed twice')\n"
  "  assert(stats.internal == 0)\n"
  "end\n"
  "function F.shutdown()\n"
  "  for _, h in ipairs(hconns) do h.c:disconnect() end\n"
  "  for _, c in ipairs(oconns) do c:disconnect() end\n"
  "  for _, t in ipairs(timers) do t:cancel() end\n"
  "  for _, o in ipairs(objs) do o:destroy() end\n"
  "  assert(fz.count() == 0, 'objects left after shutdown: ' .. fz.count())\n"
  "  for _, name in ipairs(hook_names) do\n"
  "    if fz.is_declared(name) then assert(fz.listeners(name, 'after') == 0 and fz.listeners(name, 'before') == 0, name) end\n"
  "  end\n"
  "  return stats.ops, stats.errors, stats.calls\n"
  "end\n"
  "fuzz = F\n";

/* Run everything that is ready, without waiting; bounded. */
static void
drain_main_context (void)
{
  int guard = 0;

  while (g_main_context_iteration (NULL, FALSE) && guard++ < 20000)
    ;
}

/* A little chaos from C between the batches: emissions and signals that do
 * not come from Lua. */
static void
chaos_from_c (void)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  static const char * const names[] = { "fz.alpha", "fz.gamma" };

  switch (g_rand_int_range (rng, 0, 5))
    {
    case 0:
      shell_hook_registry_emit_after (registry, names[g_rand_int_range (rng, 0, 2)], NULL);
      break;
    case 1:
      shell_hook_registry_emit_before (registry, "fz.beta", NULL, NULL);
      break;
    case 2:
      if (shared != NULL)
        {
          g_autoptr (GObject) obj = g_object_ref (shared);

          g_signal_emit (obj, obj_signals[SIG_POKED], 0, 7);
        }
      break;
    case 3:
      shell_hook_registry_emit_after (registry, "fz.alpha", NULL);
      break;
    default:
      break;
    }
}

static void
run_fuzz (ShellLuaState *s, gboolean privileged, int iterations)
{
  g_autofree char *program = g_strconcat ("PRIVILEGED = ", privileged ? "true" : "false", "\n", fuzz_program, NULL);
  int done = 0;

  tu_do (s, program);

  while (done < iterations)
    {
      g_autofree char *run = NULL;
      int n = MIN (40, iterations - done);

      run = g_strdup_printf ("fuzz.run(%d)", n);
      tu_do (s, run);
      done += n;
      drain_main_context ();
      chaos_from_c ();
      drain_main_context ();
      tu_do (s, "fuzz.check()");
    }
}

static void
fuzz_shutdown (ShellLuaState *s)
{
  tu_spin (30);                    /* the timers with a delay */
  tu_do (s, "fuzz.check()");
  tu_do (s, "ops, errors, calls = fuzz.shutdown()");
  g_test_message ("fuzz: %" G_GINT64_FORMAT " operations, %" G_GINT64_FORMAT " raised, %" G_GINT64_FORMAT " callbacks",
                  tu_eval_int (s, "ops"), tu_eval_int (s, "errors"), tu_eval_int (s, "calls"));
  g_assert_cmpint (tu_eval_int (s, "ops"), >, 100);
  g_assert_cmpint (tu_eval_int (s, "errors"), >, 10);
  g_assert_cmpint (tu_eval_int (s, "calls"), >, 10);
}

static void
test_random_privileged (void)
{
  ShellLuaState *s;

  fixture_begin (60);
  declare_hooks ();
  capture_begin ();
  s = new_priv ();
  run_fuzz (s, TRUE, fuzz_iterations);
  fuzz_shutdown (s);
  assert_no_listeners ();
  g_assert_cmpint (double_destroys, ==, 0);
  assert_state_usable (s);
  g_object_unref (s);
  capture_end ();
  g_autoptr (GSettings) st = g_settings_new (TEST_SCHEMA);
  g_settings_reset (st, "count");
  g_settings_reset (st, "flag");
  g_settings_reset (st, "ratio");
  g_settings_reset (st, "name");
  g_settings_reset (st, "list");
  fixture_end ();
}

static void
test_random_sandboxed (void)
{
  ShellLuaState *s;
  ShellLuaLimits limits = { .callback_budget_us = 5 * G_USEC_PER_SEC };

  fixture_begin (61);
  declare_hooks ();
  capture_begin ();
  s = shell_lua_state_new ("fuzz-sandboxed", SHELL_LUA_STATE_SANDBOXED, &limits);
  run_fuzz (s, FALSE, fuzz_iterations);
  fuzz_shutdown (s);
  assert_no_listeners ();
  assert_state_usable (s);
  g_object_unref (s);
  capture_end ();
  fixture_end ();
}

/* The same program, but the state is destroyed with timers, idles and
 * listeners pending: nothing may run afterwards. */
static void
test_random_destroyed_midway (void)
{
  ShellLuaState *s;
  int hits_before;

  fixture_begin (62);
  declare_hooks ();
  capture_begin ();
  s = new_priv ();
  run_fuzz (s, TRUE, fuzz_iterations / 3);
  g_assert_cmpuint (shell_lua_get_state_count (), ==, base_states + 1);
  g_object_unref (s);
  assert_no_listeners ();
  g_assert_cmpuint (shell_lua_get_state_count (), ==, base_states);
  hits_before = hits;
  drain_main_context ();
  tu_spin (30);
  g_assert_cmpint (hits, ==, hits_before);
  chaos_from_c ();
  shell_hook_registry_emit_after (shell_hook_registry_get_default (), "fz.alpha", NULL);
  capture_end ();
  fixture_end ();
}

/* The sequences again, but through extensions that are disabled at random
 * moments while their timers, hooks and objects are busy. */
static void
test_random_extension_churn (void)
{
  Env e;
  g_autoptr (ShellLuaExtension) ext = NULL;
  int round;

  fixture_begin (63);
  declare_hooks ();
  capture_begin ();
  env_init (&e);
  set_budget (&e, 2 * G_USEC_PER_SEC, 0);
  ext = install (&e, "churn@test", NULL,
                 "local shell = require('shell'); local fz = shell.fz\n"
                 "local ext = {}\n"
                 "local objs, n = {}, 0\n"
                 "function ext.enable()\n"
                 "  for i = 1, 5 do\n"
                 "    local o = fz.new{ name = 'e' .. i, parent = i > 2 and objs[i - 2] or nil, on_poked = function(self)\n"
                 "      fz.hit()\n"
                 "      local r = fz.rand(8)\n"
                 "      if r == 1 then fz.disable() elseif r == 2 then fz.new{ name = 'x' } elseif r == 3 then self:destroy()\n"
                 "      elseif r == 4 then fz.emit_after('fz.alpha') elseif r == 5 then error('failed') end\n"
                 "    end }\n"
                 "    objs[i] = o\n"
                 "  end\n"
                 "  for i = 1, 3 do\n"
                 "    shell.hooks.on('fz.alpha', function()\n"
                 "      fz.hit()\n"
                 "      local r = fz.rand(10)\n"
                 "      if r == 1 then fz.disable() elseif r == 2 then objs[fz.rand(5)]:poke(1) elseif r == 3 then fz.new{ name = 'y' }\n"
                 "      elseif r == 4 then shell.timer.after{ ms = 1, callback = function() fz.hit(); if fz.rand(3) == 1 then fz.disable() end end } end\n"
                 "    end)\n"
                 "  end\n"
                 "  shell.timer.every{ ms = 1, callback = function() fz.hit(); if fz.rand(30) == 1 then fz.disable() end; return fz.rand(50) ~= 1 end }\n"
                 "end\n"
                 "function ext.disable() fz.mark('d') end\n"
                 "return ext\n");

  for (round = 0; round < 40; round++)
    {
      g_autoptr (ShellLuaState) state = NULL;
      g_autoptr (GError) error = NULL;
      int k;

      g_assert_true (shell_lua_extension_manager_enable (e.manager, "churn@test", &error));
      g_assert_no_error (error);
      state = g_object_ref (shell_lua_extension_get_lua_state (ext));
      for (k = 0; k < 6 && shell_lua_extension_get_state (ext) == SHELL_LUA_EXTENSION_STATE_ACTIVE; k++)
        {
          shell_hook_registry_emit_after (shell_hook_registry_get_default (), "fz.alpha", NULL);
          if (shared != NULL && shell_lua_extension_get_state (ext) == SHELL_LUA_EXTENSION_STATE_ACTIVE)
            {
              g_autoptr (GObject) obj = g_object_ref (shared);

              g_signal_emit (obj, obj_signals[SIG_POKED], 0, 1);
            }
          tu_spin (g_rand_int_range (rng, 0, 4));
        }
      /* disable from the outside if the script did not */
      if (shell_lua_extension_get_state (ext) == SHELL_LUA_EXTENSION_STATE_ACTIVE)
        g_assert_true (shell_lua_extension_manager_disable (e.manager, "churn@test", &error));
      g_assert_no_error (error);
      g_assert_cmpint (shell_lua_extension_get_state (ext), !=, SHELL_LUA_EXTENSION_STATE_ACTIVE);
      g_assert_false (shell_lua_state_is_alive (state));
      g_assert_cmpuint (shell_lua_state_get_memory_usage (state), ==, 0);
      assert_no_listeners ();
      g_assert_cmpint (live_objs, ==, 0);
      g_assert_cmpint (double_destroys, ==, 0);
      /* the error policy may have put it in ERROR: enable() starts again from there */
      g_string_truncate (marks, 0);
      g_string_truncate (destroy_log, 0);
    }

  env_clear (&e);
  capture_end ();
  g_clear_object (&ext);
  fixture_end ();
}

/* ======================================================================== */

int
main (int argc, char *argv[])
{
  int ret;

  g_test_init (&argc, &argv, NULL);
  fuzz_init_from_environment ();
  destroy_log = g_string_new (NULL);
  marks = g_string_new (NULL);
  others = g_ptr_array_new_with_free_func (g_object_unref);
  live_set = g_hash_table_new (g_direct_hash, g_direct_equal);
  shell_lua_api_register ("fz", fz_register);

  g_test_add_func ("/shell-lua/fuzz/handles/wrong-self", test_handles_wrong_self);
  g_test_add_func ("/shell-lua/fuzz/handles/garbage-keys", test_handles_garbage_keys);
  g_test_add_func ("/shell-lua/fuzz/handles/assignment-matrix", test_handles_assignment_matrix);
  g_test_add_func ("/shell-lua/fuzz/handles/cross-type-calls", test_handles_cross_type_calls);
  g_test_add_func ("/shell-lua/fuzz/handles/dead", test_handles_dead);
  g_test_add_func ("/shell-lua/fuzz/handles/two-states", test_handles_two_states);
  g_test_add_func ("/shell-lua/fuzz/hooks/registry-reentrancy", test_hooks_registry_reentrancy);
  g_test_add_func ("/shell-lua/fuzz/hooks/recursion", test_hooks_recursion);
  g_test_add_func ("/shell-lua/fuzz/hooks/destroy-emitter", test_hooks_destroy_emitter);
  g_test_add_func ("/shell-lua/fuzz/hooks/disable-self-in-emission", test_hooks_disable_self_in_emission);
  g_test_add_func ("/shell-lua/fuzz/hooks/disable-other-extension", test_hooks_disable_other_extension);
  g_test_add_func ("/shell-lua/fuzz/hooks/errors-and-watchdog", test_hooks_errors_and_watchdog);
  g_test_add_func ("/shell-lua/fuzz/hooks/cross-state", test_hooks_cross_state);
  g_test_add_func ("/shell-lua/fuzz/api/from-coroutines", test_api_from_coroutines);
  g_test_add_func ("/shell-lua/fuzz/disable/during-callbacks", test_disable_during_callbacks);
  g_test_add_func ("/shell-lua/fuzz/disable/during-enable", test_disable_during_enable);
  g_test_add_func ("/shell-lua/fuzz/limits/memory", test_limits_memory);
  g_test_add_func ("/shell-lua/fuzz/limits/memory-injection", test_limits_memory_injection);
  g_test_add_func ("/shell-lua/fuzz/limits/recursion", test_limits_recursion);
  g_test_add_func ("/shell-lua/fuzz/limits/watchdog", test_limits_watchdog);
  g_test_add_func ("/shell-lua/fuzz/limits/state-destroyed-from-inside", test_limits_state_destroyed_from_inside);
  g_test_add_func ("/shell-lua/fuzz/random/privileged", test_random_privileged);
  g_test_add_func ("/shell-lua/fuzz/random/sandboxed", test_random_sandboxed);
  g_test_add_func ("/shell-lua/fuzz/random/destroyed-midway", test_random_destroyed_midway);
  g_test_add_func ("/shell-lua/fuzz/random/extension-churn", test_random_extension_churn);

  ret = g_test_run ();
  g_string_free (destroy_log, TRUE);
  g_string_free (marks, TRUE);
  g_ptr_array_unref (others);
  g_clear_pointer (&rng, g_rand_free);
  return ret;
}
