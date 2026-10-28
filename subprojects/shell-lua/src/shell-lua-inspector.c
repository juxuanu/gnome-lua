/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaInspector: the privileged state behind Looking Glass's Evaluator.
 *
 * Values that the user sees in Looking Glass (results, members of inspected
 * values) live in two tables of the Lua registry, boxed in one element tables
 * so that nil is a value too:
 *
 *   results_key  result number -> {value}   (numbers 1, 2, ..., `r (n)`)
 *   pins_key     id -> {value}              (ids from PIN_BASE up: members
 *                                            handed out by inspect ())
 *
 * Everything that can raise a Lua error (summaries call __tostring, reading
 * a property can fail) runs in a lua_CFunction under shell_lua_state_pcall ()
 * and keeps its C-side memory in a context owned by the caller.
 */

#include <string.h>

#include "shell-lua.h"
#include "shell-lua-extension.h"
#include "shell-lua-extension-private.h"
#include "shell-lua-handle.h"
#include "shell-lua-inspector.h"

#define INSPECTOR_ID "inspector"
#define INSPECTOR_ROOT_URI "resource:///org/gnome/shell/lua"

/* "Unlimited" wall-clock budget: far enough away (about 31 years) not to
 * overflow when added to a monotonic timestamp. */
#define INSPECTOR_BUDGET_US (G_GINT64_CONSTANT (1000000000000000))

#define MAX_TABLE_ENTRIES 32
#define MAX_RESULTS 200
#define MAX_MEMBERS 500
#define MAX_SUMMARY_CHARS 300
#define MAX_OUTPUT_BYTES (1024 * 1024)
#define MAX_METATABLE_DEPTH 8

/* Ids of the pins start here, so that they never clash with result numbers */
#define PIN_BASE (((guint64) 1) << 32)

static char results_key, pins_key;

typedef struct
{
  char        *name;
  lua_CFunction func;
} GlobalFunction;

typedef struct
{
  char     *name;
  GWeakRef  object;
} GlobalObject;

struct _ShellLuaInspector
{
  GObject parent_instance;

  ShellLuaState *state;

  GString *output;       /* what print () wrote since the last evaluation */
  guint64  next_result;
  guint64  next_pin;

  GPtrArray *functions;  /* GlobalFunction */
  GPtrArray *objects;    /* GlobalObject */
};

G_DEFINE_FINAL_TYPE (ShellLuaInspector, shell_lua_inspector, G_TYPE_OBJECT)

static void push_stored_or_error (lua_State *L, guint64 id);

/* ------------------------------------------------------------------ */
/* Generic handles                                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
  GType gtype;
  const luaL_Reg *methods;
} HostMethods;

static GPtrArray *host_methods = NULL;

/* One handle type per GType, in the image of the GType tree. They are never
 * freed: the handle layer wants descriptors that outlive every state. */
typedef struct
{
  ShellLuaHandleType type;
  char *name;
  char *parent_name;
  ShellLuaHandleProperty *properties;
  luaL_Reg *methods;
} GenericType;

static GHashTable *generic_types = NULL;

static int
l_gtype (lua_State *L)
{
  GObject *object = shell_lua_handle_check (L, 1, NULL);

  lua_pushstring (L, G_OBJECT_TYPE_NAME (object));
  return 1;
}

typedef struct
{
  GPtrArray *names;
} NameCollector;

static void
collect_property_name (const char *name,
                       gboolean    is_method,
                       gpointer    user_data)
{
  NameCollector *collector = user_data;

  if (!is_method)
    g_ptr_array_add (collector->names, g_strdup (name));
}

static int
compare_strings (gconstpointer a,
                 gconstpointer b)
{
  return strcmp (*(const char * const *) a, *(const char * const *) b);
}

static int
l_properties (lua_State *L)
{
  NameCollector collector;
  guint i;

  shell_lua_handle_check (L, 1, NULL);
  luaL_checkstack (L, 4, "properties");

  collector.names = g_ptr_array_new_with_free_func (g_free);
  shell_lua_handle_foreach_member (L, 1, collect_property_name, &collector);
  g_ptr_array_sort (collector.names, compare_strings);

  lua_createtable (L, collector.names->len, 0);
  for (i = 0; i < collector.names->len; i++)
    {
      lua_pushstring (L, g_ptr_array_index (collector.names, i));
      lua_rawseti (L, -2, i + 1);
    }
  g_ptr_array_unref (collector.names);
  return 1;
}

static const luaL_Reg base_methods[] = {
  { "gtype", l_gtype },
  { "properties", l_properties },
  { NULL, NULL }
};

/* (Re)builds the methods of a type: the base ones, the ones the host added.
 * The handle layer reads the array through the descriptor each time, so
 * this also reaches states that registered the type earlier. */
static void
build_methods (GenericType *gt)
{
  GPtrArray *methods = g_ptr_array_new ();
  guint i;

  if (gt->type.gtype == G_TYPE_OBJECT)
    {
      const luaL_Reg *r;

      for (r = base_methods; r->name != NULL; r++)
        g_ptr_array_add (methods, (gpointer) r);
    }
  if (host_methods != NULL)
    {
      for (i = 0; i < host_methods->len; i++)
        {
          HostMethods *host = g_ptr_array_index (host_methods, i);
          const luaL_Reg *r;

          if (host->gtype != gt->type.gtype)
            continue;
          for (r = host->methods; r->name != NULL; r++)
            g_ptr_array_add (methods, (gpointer) r);
        }
    }

  /* only called between Lua calls, nothing reads the old array now */
  g_free (gt->methods);
  gt->methods = g_new0 (luaL_Reg, methods->len + 1);
  for (i = 0; i < methods->len; i++)
    gt->methods[i] = *(luaL_Reg *) g_ptr_array_index (methods, i);
  g_ptr_array_free (methods, TRUE);
  gt->type.methods = gt->methods;
}

static GenericType *
get_generic_type (GType gtype)
{
  GenericType *gt;
  GType parent;
  GObjectClass *klass;
  GParamSpec **pspecs;
  GPtrArray *props;
  guint n_pspecs, i;

  if (generic_types == NULL)
    generic_types = g_hash_table_new (NULL, NULL);

  gt = g_hash_table_lookup (generic_types, GSIZE_TO_POINTER (gtype));
  if (gt != NULL)
    return gt;

  gt = g_new0 (GenericType, 1);
  gt->name = g_strconcat ("gobject.", g_type_name (gtype), NULL);
  gt->type.name = gt->name;
  gt->type.gtype = gtype;

  parent = g_type_parent (gtype);
  if (parent != 0 && g_type_is_a (parent, G_TYPE_OBJECT))
    {
      GenericType *parent_type = get_generic_type (parent);

      gt->parent_name = g_strdup (parent_type->name);
      gt->type.parent = gt->parent_name;
    }
  else
    {
      parent = 0;
    }

  /* The properties this class adds: the ones it declares, and the ones of
   * an interface which the parent does not have yet. */
  props = g_ptr_array_new ();
  klass = g_type_class_ref (gtype);
  pspecs = g_object_class_list_properties (klass, &n_pspecs);
  for (i = 0; i < n_pspecs; i++)
    {
      GParamSpec *pspec = pspecs[i];
      ShellLuaHandleProperty *p;

      if (!(pspec->flags & (G_PARAM_READABLE | G_PARAM_WRITABLE)))
        continue;

      if (pspec->owner_type != gtype &&
          (!G_TYPE_IS_INTERFACE (pspec->owner_type) ||
           (parent != 0 && g_type_is_a (parent, pspec->owner_type))))
        continue;

      p = g_new0 (ShellLuaHandleProperty, 1);
      p->name = g_strdelimit (g_strdup (pspec->name), "-", '_');
      p->gproperty = pspec->name;
      p->readable = (pspec->flags & G_PARAM_READABLE) != 0;
      p->writable = (pspec->flags & G_PARAM_WRITABLE) != 0 &&
                    !(pspec->flags & G_PARAM_CONSTRUCT_ONLY);
      g_ptr_array_add (props, p);
    }
  g_free (pspecs);
  g_type_class_unref (klass);

  gt->properties = g_new0 (ShellLuaHandleProperty, props->len + 1);
  for (i = 0; i < props->len; i++)
    {
      gt->properties[i] = *(ShellLuaHandleProperty *) g_ptr_array_index (props, i);
      g_free (g_ptr_array_index (props, i));
    }
  g_ptr_array_free (props, TRUE);

  build_methods (gt);
  gt->type.properties = gt->properties;

  g_hash_table_insert (generic_types, GSIZE_TO_POINTER (gtype), gt);
  return gt;
}

static void
register_generic_chain (lua_State *L,
                        GType      gtype)
{
  GenericType *gt = get_generic_type (gtype);
  GType parent = g_type_parent (gtype);

  if (parent != 0 && g_type_is_a (parent, G_TYPE_OBJECT))
    register_generic_chain (L, parent);

  shell_lua_handle_register_type (L, &gt->type);
}

static void
resolve_generic_type (lua_State *L,
                      GType      gtype)
{
  if (g_type_is_a (gtype, G_TYPE_OBJECT))
    register_generic_chain (L, gtype);
}

void
shell_lua_inspector_push_object (lua_State *L,
                                 GObject   *object)
{
  g_return_if_fail (G_IS_OBJECT (object));

  resolve_generic_type (L, G_OBJECT_TYPE (object));
  shell_lua_handle_push (L, NULL, object);
}

void
shell_lua_inspector_add_object_methods (GType           gtype,
                                        const luaL_Reg *methods)
{
  HostMethods *host;

  g_return_if_fail (g_type_is_a (gtype, G_TYPE_OBJECT));
  g_return_if_fail (methods != NULL);

  if (host_methods == NULL)
    host_methods = g_ptr_array_new ();

  host = g_new0 (HostMethods, 1);
  host->gtype = gtype;
  host->methods = methods;
  g_ptr_array_add (host_methods, host);

  /* a type that exists already picks the methods up */
  if (generic_types != NULL)
    {
      GenericType *gt = g_hash_table_lookup (generic_types, GSIZE_TO_POINTER (gtype));

      if (gt != NULL)
        build_methods (gt);
    }
}

/* ------------------------------------------------------------------ */
/* The state                                                          */
/* ------------------------------------------------------------------ */

static int
l_print (lua_State *L)
{
  ShellLuaInspector *self = lua_touserdata (L, lua_upvalueindex (1));
  int n = lua_gettop (L);
  int i;

  luaL_checkstack (L, 2, "print");

  for (i = 1; i <= n; i++)
    {
      size_t len;
      const char *s = luaL_tolstring (L, i, &len);

      if (self->output->len < MAX_OUTPUT_BYTES)
        {
          if (i > 1)
            g_string_append_c (self->output, '\t');
          g_string_append_len (self->output, s, len);
        }
      lua_pop (L, 1);
    }
  if (self->output->len < MAX_OUTPUT_BYTES)
    g_string_append_c (self->output, '\n');

  return 0;
}

static int
l_r (lua_State *L)
{
  lua_Integer n = luaL_checkinteger (L, 1);

  if (n < 1 || (guint64) n >= PIN_BASE)
    return luaL_error (L, "Unknown result at index %I", n);

  luaL_checkstack (L, 4, "r");
  push_stored_or_error (L, (guint64) n);
  return 1;
}

/* arg 1: lightuserdata ShellLuaInspector; arg 2: name; arg 3: GObject */
static int
set_global_object_cb (lua_State *L)
{
  const char *name = lua_tostring (L, 1);
  GObject *object = lua_touserdata (L, 2);

  luaL_checkstack (L, 4, "set_global_object");
  lua_pushglobaltable (L);
  if (object != NULL)
    shell_lua_inspector_push_object (L, object);
  else
    lua_pushnil (L);
  lua_setfield (L, -2, name);
  return 0;
}

static int
setup_inspector_state (lua_State *L)
{
  ShellLuaInspector *self = lua_touserdata (L, 1);
  guint i;

  luaL_checkstack (L, 8, "setup");

  lua_newtable (L);
  lua_rawsetp (L, LUA_REGISTRYINDEX, &results_key);
  lua_newtable (L);
  lua_rawsetp (L, LUA_REGISTRYINDEX, &pins_key);

  shell_lua_handle_set_resolver (L, resolve_generic_type);

  lua_pushglobaltable (L);

  lua_pushlightuserdata (L, self);
  lua_pushcclosure (L, l_print, 1);
  lua_setfield (L, -2, "print");

  lua_pushcfunction (L, l_r);
  lua_setfield (L, -2, "r");

  for (i = 0; i < self->functions->len; i++)
    {
      GlobalFunction *function = g_ptr_array_index (self->functions, i);

      lua_pushcfunction (L, function->func);
      lua_setfield (L, -2, function->name);
    }

  for (i = 0; i < self->objects->len; i++)
    {
      GlobalObject *global_object = g_ptr_array_index (self->objects, i);
      GObject *object = g_weak_ref_get (&global_object->object);

      if (object == NULL)
        continue;

      shell_lua_inspector_push_object (L, object);
      lua_setfield (L, -2, global_object->name);
      g_object_unref (object);
    }

  lua_pop (L, 1);
  return 0;
}

static ShellLuaState *
create_state (ShellLuaInspector *self)
{
  g_autoptr (GFile) root = NULL;
  g_autoptr (GError) error = NULL;
  ShellLuaLimits limits = { 0, };
  ShellLuaState *state;
  lua_State *L;

  limits.callback_budget_us = INSPECTOR_BUDGET_US;
  limits.memory_limit = 0;

  state = shell_lua_state_new (INSPECTOR_ID, SHELL_LUA_STATE_PRIVILEGED,
                               &limits);

  root = g_file_new_for_uri (INSPECTOR_ROOT_URI);
  shell_lua_state_add_search_root (state, root);

  L = shell_lua_state_get_L (state);
  lua_pushcfunction (L, setup_inspector_state);
  lua_pushlightuserdata (L, self);
  if (!shell_lua_state_pcall (state, 1, 0, &error))
    g_debug ("Setting up the inspector state failed: %s", error->message);

  return state;
}

static ShellLuaState *
ensure_state (ShellLuaInspector *self)
{
  if (self->state != NULL && !shell_lua_state_is_alive (self->state))
    g_clear_object (&self->state);

  if (self->state == NULL)
    self->state = create_state (self);

  return self->state;
}

static void
global_function_free (GlobalFunction *function)
{
  g_free (function->name);
  g_free (function);
}

static void
global_object_free (GlobalObject *global_object)
{
  g_weak_ref_clear (&global_object->object);
  g_free (global_object->name);
  g_free (global_object);
}

static void
shell_lua_inspector_dispose (GObject *object)
{
  ShellLuaInspector *self = SHELL_LUA_INSPECTOR (object);

  if (self->state != NULL)
    {
      shell_lua_state_destroy (self->state);
      g_clear_object (&self->state);
    }

  G_OBJECT_CLASS (shell_lua_inspector_parent_class)->dispose (object);
}

static void
shell_lua_inspector_finalize (GObject *object)
{
  ShellLuaInspector *self = SHELL_LUA_INSPECTOR (object);

  g_string_free (self->output, TRUE);
  g_ptr_array_unref (self->functions);
  g_ptr_array_unref (self->objects);

  G_OBJECT_CLASS (shell_lua_inspector_parent_class)->finalize (object);
}

static void
shell_lua_inspector_class_init (ShellLuaInspectorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_lua_inspector_dispose;
  object_class->finalize = shell_lua_inspector_finalize;
}

static void
shell_lua_inspector_init (ShellLuaInspector *self)
{
  self->output = g_string_new (NULL);
  self->next_result = 1;
  self->next_pin = PIN_BASE;
  self->functions = g_ptr_array_new_with_free_func ((GDestroyNotify) global_function_free);
  self->objects = g_ptr_array_new_with_free_func ((GDestroyNotify) global_object_free);
}

ShellLuaInspector *
shell_lua_inspector_get_default (void)
{
  static ShellLuaInspector *default_inspector = NULL;

  if (default_inspector == NULL)
    {
      default_inspector = g_object_new (SHELL_TYPE_LUA_INSPECTOR, NULL);
      g_object_add_weak_pointer (G_OBJECT (default_inspector),
                                 (gpointer *) &default_inspector);
    }

  return default_inspector;
}

void
shell_lua_inspector_add_function (ShellLuaInspector *self,
                                  const char        *name,
                                  lua_CFunction      func)
{
  GlobalFunction *function;
  guint i;

  g_return_if_fail (SHELL_IS_LUA_INSPECTOR (self));
  g_return_if_fail (name != NULL && func != NULL);

  for (i = 0; i < self->functions->len; i++)
    {
      function = g_ptr_array_index (self->functions, i);
      if (strcmp (function->name, name) == 0)
        {
          function->func = func;
          goto apply;
        }
    }

  function = g_new0 (GlobalFunction, 1);
  function->name = g_strdup (name);
  function->func = func;
  g_ptr_array_add (self->functions, function);

apply:
  if (self->state != NULL && shell_lua_state_is_alive (self->state))
    {
      lua_State *L = shell_lua_state_get_L (self->state);

      if (lua_checkstack (L, 4))
        {
          int top = lua_gettop (L);

          lua_pushglobaltable (L);
          lua_pushcfunction (L, func);
          lua_setfield (L, -2, name);
          lua_settop (L, top);
        }
    }
}

void
shell_lua_inspector_set_global_object (ShellLuaInspector *self,
                                       const char        *name,
                                       GObject           *object)
{
  GlobalObject *global_object = NULL;
  guint i;

  g_return_if_fail (SHELL_IS_LUA_INSPECTOR (self));
  g_return_if_fail (name != NULL);
  g_return_if_fail (object == NULL || G_IS_OBJECT (object));

  for (i = 0; i < self->objects->len; i++)
    {
      GlobalObject *candidate = g_ptr_array_index (self->objects, i);

      if (strcmp (candidate->name, name) == 0)
        {
          global_object = candidate;
          break;
        }
    }

  if (object == NULL)
    {
      if (global_object != NULL)
        g_ptr_array_remove (self->objects, global_object);
    }
  else if (global_object != NULL)
    {
      g_weak_ref_set (&global_object->object, object);
    }
  else
    {
      global_object = g_new0 (GlobalObject, 1);
      global_object->name = g_strdup (name);
      g_weak_ref_init (&global_object->object, object);
      g_ptr_array_add (self->objects, global_object);
    }

  if (self->state != NULL && shell_lua_state_is_alive (self->state))
    {
      g_autoptr (GError) error = NULL;
      lua_State *L = shell_lua_state_get_L (self->state);

      if (lua_checkstack (L, 4))
        {
          lua_pushcfunction (L, set_global_object_cb);
          lua_pushstring (L, name);
          lua_pushlightuserdata (L, object);
          if (!shell_lua_state_pcall (self->state, 2, 0, &error))
            g_debug ("Setting the global '%s' failed: %s", name, error->message);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                          */
/* ------------------------------------------------------------------ */

/* Appends luaL_tolstring() of the value at @idx (honours __tostring and
 * __name). May raise a Lua error: only call it under a protected call. */
static void
append_tostring (lua_State *L,
                 int        idx,
                 GString   *out)
{
  size_t len;
  const char *s = luaL_tolstring (L, idx, &len);

  g_string_append_len (out, s, len);
  lua_pop (L, 1);
}

static gboolean
has_tostring (lua_State *L,
              int        idx)
{
  if (luaL_getmetafield (L, idx, "__tostring") != LUA_TNIL)
    {
      lua_pop (L, 1);
      return TRUE;
    }

  return FALSE;
}

/* Value inside a table: strings are quoted, nested tables are elided. */
static void
append_member (lua_State *L,
               int        idx,
               GString   *out)
{
  idx = lua_absindex (L, idx);

  if (lua_type (L, idx) == LUA_TSTRING)
    {
      size_t len;
      const char *s = lua_tolstring (L, idx, &len);

      g_string_append_c (out, '"');
      g_string_append_len (out, s, len);
      g_string_append_c (out, '"');
    }
  else if (lua_type (L, idx) == LUA_TTABLE && !has_tostring (L, idx))
    {
      g_string_append (out, "{...}");
    }
  else
    {
      append_tostring (L, idx, out);
    }
}

static void
append_table (lua_State *L,
              int        idx,
              GString   *out)
{
  lua_Integer sequence = 0;
  int count = 0;
  gboolean truncated = FALSE;

  idx = lua_absindex (L, idx);

  g_string_append_c (out, '{');

  lua_pushnil (L);
  while (lua_next (L, idx) != 0)
    {
      if (count == MAX_TABLE_ENTRIES)
        {
          /* pop value and key */
          lua_pop (L, 2);
          truncated = TRUE;
          break;
        }

      if (count > 0)
        g_string_append (out, ", ");

      /* stack: key, value */
      if (lua_isinteger (L, -2) && lua_tointeger (L, -2) == sequence + 1)
        {
          sequence++;
        }
      else if (lua_type (L, -2) == LUA_TSTRING)
        {
          size_t len;
          const char *key = lua_tolstring (L, -2, &len);

          g_string_append_len (out, key, len);
          g_string_append (out, " = ");
        }
      else
        {
          g_string_append_c (out, '[');
          append_tostring (L, -2, out);
          g_string_append (out, "] = ");
        }

      append_member (L, -1, out);
      count++;

      /* keep the key for lua_next */
      lua_pop (L, 1);
    }

  if (truncated)
    g_string_append (out, ", ...");

  g_string_append_c (out, '}');
}

/* lua_CFunction. Args: lightuserdata GString*, then the values to render.
 * Renders one value per line into the GString (owned by the caller, so
 * nothing leaks if a __tostring metamethod raises an error). */
static int
render_values (lua_State *L)
{
  GString *out = lua_touserdata (L, 1);
  int n = lua_gettop (L);
  int i;

  luaL_checkstack (L, 8, "rendering results");

  for (i = 2; i <= n; i++)
    {
      if (i > 2)
        g_string_append_c (out, '\n');

      if (lua_type (L, i) == LUA_TTABLE && !has_tostring (L, i))
        append_table (L, i, out);
      else
        append_tostring (L, i, out);
    }

  return 0;
}

/* Runs @code, leaving its values (at most MAX_RESULTS) on the stack above
 * the current top. Returns the number of values or -1 with @error set (the
 * stack is then back at its old level). */
static int
run_chunk (ShellLuaInspector  *self,
           const char         *code,
           GError            **error)
{
  g_autoptr (GError) expr_error = NULL;
  g_autoptr (GError) local_error = NULL;
  g_autofree char *expression = NULL;
  ShellLuaState *state = ensure_state (self);
  lua_State *L = shell_lua_state_get_L (state);
  int base = lua_gettop (L);
  int n;

  /* Same trick as lua.c and Looking Glass: try "return <code>" first so
   * expressions print their value, then the code as written. */
  expression = g_strconcat ("return ", code, NULL);
  if (!shell_lua_state_load_string (state, expression, "=(inspector)",
                                    &expr_error))
    {
      if (!shell_lua_state_load_string (state, code, "=(inspector)",
                                        &local_error))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          lua_settop (L, base);
          return -1;
        }
    }

  if (!shell_lua_state_pcall (state, 0, LUA_MULTRET, &local_error))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      lua_settop (L, base);
      return -1;
    }

  n = lua_gettop (L) - base;
  if (n > MAX_RESULTS)
    {
      lua_settop (L, base + MAX_RESULTS);
      n = MAX_RESULTS;
    }

  return n;
}

char *
shell_lua_inspector_eval (ShellLuaInspector  *self,
                          const char         *code,
                          GError            **error)
{
  g_autoptr (GError) local_error = NULL;
  ShellLuaState *state;
  lua_State *L;
  GString *out;
  int base, n, i;

  g_return_val_if_fail (SHELL_IS_LUA_INSPECTOR (self), NULL);
  g_return_val_if_fail (code != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  state = ensure_state (self);
  L = shell_lua_state_get_L (state);
  base = lua_gettop (L);

  n = run_chunk (self, code, error);
  if (n < 0)
    return NULL;

  out = g_string_new (NULL);

  if (n > 0)
    {
      /* Move the renderer and its GString below the results and call it
       * with copies, so that the originals stay put until we are done. */
      if (!lua_checkstack (L, n + 4))
        {
          g_string_free (out, TRUE);
          lua_settop (L, base);
          g_set_error_literal (error, SHELL_LUA_ERROR,
                               SHELL_LUA_ERROR_RUNTIME,
                               "too many results to display");
          return NULL;
        }

      lua_pushcfunction (L, render_values);
      lua_pushlightuserdata (L, out);
      for (i = 1; i <= n; i++)
        lua_pushvalue (L, base + i);

      if (!shell_lua_state_pcall (state, n + 1, 0, &local_error))
        {
          g_string_free (out, TRUE);
          g_propagate_error (error, g_steal_pointer (&local_error));
          lua_settop (L, base);
          return NULL;
        }
    }

  lua_settop (L, base);
  return g_string_free (out, FALSE);
}

void
shell_lua_inspector_reset (ShellLuaInspector *self)
{
  g_return_if_fail (SHELL_IS_LUA_INSPECTOR (self));

  if (self->state != NULL)
    {
      shell_lua_state_destroy (self->state);
      g_clear_object (&self->state);
    }

  g_string_truncate (self->output, 0);
  self->next_result = 1;
  self->next_pin = PIN_BASE;

  self->state = create_state (self);
}

/* ------------------------------------------------------------------ */
/* Stored values                                                      */
/* ------------------------------------------------------------------ */

/* Pushes the value with the id; FALSE (pushing nothing) if there is none.
 * Needs 3 free stack slots, does not raise. */
static gboolean
push_stored (lua_State *L,
             guint64    id)
{
  if (lua_rawgetp (L, LUA_REGISTRYINDEX,
                   id >= PIN_BASE ? &pins_key : &results_key) != LUA_TTABLE)
    {
      lua_pop (L, 1);
      return FALSE;
    }

  if (lua_rawgeti (L, -1, (lua_Integer) id) != LUA_TTABLE)
    {
      lua_pop (L, 2);
      return FALSE;
    }

  lua_rawgeti (L, -1, 1);
  lua_remove (L, -2);
  lua_remove (L, -2);
  return TRUE;
}

static void
push_stored_or_error (lua_State *L,
                      guint64    id)
{
  if (!push_stored (L, id))
    luaL_error (L, "Unknown result at index %I", (lua_Integer) id);
}

/* Boxes the value at @idx under @id. Needs 3 free stack slots. */
static void
store_value (lua_State *L,
             int        idx,
             guint64    id)
{
  idx = lua_absindex (L, idx);

  lua_rawgetp (L, LUA_REGISTRYINDEX, id >= PIN_BASE ? &pins_key : &results_key);
  lua_createtable (L, 1, 0);
  lua_pushvalue (L, idx);
  lua_rawseti (L, -2, 1);
  lua_rawseti (L, -2, (lua_Integer) id);
  lua_pop (L, 1);
}

static void
forget_value (lua_State *L,
              guint64    id)
{
  if (lua_rawgetp (L, LUA_REGISTRYINDEX,
                   id >= PIN_BASE ? &pins_key : &results_key) == LUA_TTABLE)
    {
      lua_pushnil (L);
      lua_rawseti (L, -2, (lua_Integer) id);
    }
  lua_pop (L, 1);
}

static char *
make_valid (const char *s,
            gssize      len)
{
  return g_utf8_make_valid (s, len);
}

/* Turns the text of a summary into a single line of valid UTF-8 of a sane
 * length. */
static char *
one_line (const GString *text)
{
  g_autofree char *valid = make_valid (text->str, text->len);
  GString *line = g_string_new (NULL);
  const char *p;
  glong chars = 0;

  for (p = valid; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (chars >= MAX_SUMMARY_CHARS)
        {
          g_string_append (line, "...");
          break;
        }

      if (c == '\n')
        g_string_append (line, "\\n");
      else if (c == '\r')
        g_string_append (line, "\\r");
      else if (c == '\t')
        g_string_append_c (line, ' ');
      else
        g_string_append_unichar (line, c);
      chars++;
    }

  return g_string_free (line, FALSE);
}

/* Appends a description of the live object of a handle at @idx. */
static void
append_object (lua_State *L,
               int        idx,
               GString   *out)
{
  GObject *object = shell_lua_handle_peek (L, idx, NULL);
  GParamSpec *pspec;
  g_autofree char *name = NULL;

  /* Actors and many other objects have a name; it tells them apart */
  pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (object), "name");
  if (pspec != NULL && pspec->value_type == G_TYPE_STRING &&
      (pspec->flags & G_PARAM_READABLE))
    g_object_get (object, "name", &name, NULL);

  g_string_append_printf (out, "<%s", G_OBJECT_TYPE_NAME (object));
  if (name != NULL && *name != '\0')
    g_string_append_printf (out, " \"%s\"", name);
  g_string_append_printf (out, " %p>", (gpointer) object);
}

/* May raise (__tostring). */
static void
summarize (lua_State *L,
           int        idx,
           GString   *out)
{
  idx = lua_absindex (L, idx);

  if (shell_lua_handle_is_alive (L, idx))
    append_object (L, idx, out);
  else if (lua_type (L, idx) == LUA_TTABLE && !has_tostring (L, idx))
    append_table (L, idx, out);
  else
    append_tostring (L, idx, out);
}

/* The name of the type of the value: the GType of a live object, otherwise
 * the Lua type (both are ASCII). Does not raise. */
static char *
type_name_of (lua_State *L,
              int        idx)
{
  if (shell_lua_handle_is_alive (L, idx))
    return g_strdup (G_OBJECT_TYPE_NAME (shell_lua_handle_peek (L, idx, NULL)));

  return g_strdup (luaL_typename (L, idx));
}

/* Is it worth giving the value an id of its own? */
static gboolean
has_structure (lua_State *L,
               int        idx)
{
  switch (lua_type (L, idx))
    {
    case LUA_TTABLE:
    case LUA_TUSERDATA:
    case LUA_TFUNCTION:
    case LUA_TTHREAD:
      return TRUE;
    default:
      return FALSE;
    }
}

/* ------------------------------------------------------------------ */
/* Evaluate, insert, add objects                                      */
/* ------------------------------------------------------------------ */

typedef struct
{
  ShellLuaInspector *self;
  GVariantBuilder    builder;   /* a(tss) */
  GString           *scratch;
} StoreContext;

static void
store_context_init (StoreContext       *ctx,
                    ShellLuaInspector  *self)
{
  ctx->self = self;
  ctx->scratch = g_string_new (NULL);
  g_variant_builder_init (&ctx->builder, G_VARIANT_TYPE ("a(tss)"));
}

static void
store_context_clear (StoreContext *ctx)
{
  g_variant_builder_clear (&ctx->builder);
  g_string_free (ctx->scratch, TRUE);
}

/* Stores the value at @idx as the next result and adds its entry. */
static void
store_result (lua_State    *L,
              int           idx,
              StoreContext *ctx)
{
  guint64 id;
  g_autofree char *type_name = NULL;
  g_autofree char *summary = NULL;

  idx = lua_absindex (L, idx);
  id = ctx->self->next_result++;
  store_value (L, idx, id);

  type_name = type_name_of (L, idx);
  g_string_truncate (ctx->scratch, 0);
  summarize (L, idx, ctx->scratch);
  summary = one_line (ctx->scratch);

  g_variant_builder_add (&ctx->builder, "(tss)", id, type_name, summary);

  /* `it` */
  lua_pushglobaltable (L);
  lua_pushvalue (L, idx);
  lua_setfield (L, -2, "it");
  lua_pop (L, 1);
}

/* args: lightuserdata StoreContext, the values */
static int
store_values (lua_State *L)
{
  StoreContext *ctx = lua_touserdata (L, 1);
  int n = lua_gettop (L);
  int i;

  luaL_checkstack (L, 8, "storing results");

  for (i = 2; i <= n; i++)
    store_result (L, i, ctx);

  return 0;
}

/* args: lightuserdata StoreContext, lightuserdata GObject */
static int
store_object (lua_State *L)
{
  StoreContext *ctx = lua_touserdata (L, 1);
  GObject *object = lua_touserdata (L, 2);

  luaL_checkstack (L, 8, "storing an object");
  shell_lua_inspector_push_object (L, object);
  store_result (L, -1, ctx);
  return 0;
}

static char *
take_output (ShellLuaInspector *self)
{
  char *output = make_valid (self->output->str, self->output->len);

  g_string_truncate (self->output, 0);
  return output;
}

GVariant *
shell_lua_inspector_evaluate (ShellLuaInspector  *self,
                              const char         *code,
                              char              **output,
                              GError            **error)
{
  g_autoptr (GError) local_error = NULL;
  StoreContext ctx;
  ShellLuaState *state;
  lua_State *L;
  gboolean ok;
  int base, n, i;

  g_return_val_if_fail (SHELL_IS_LUA_INSPECTOR (self), NULL);
  g_return_val_if_fail (code != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  state = ensure_state (self);
  L = shell_lua_state_get_L (state);
  base = lua_gettop (L);

  g_string_truncate (self->output, 0);

  n = run_chunk (self, code, &local_error);
  if (n < 0)
    {
      if (output != NULL)
        *output = take_output (self);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  if (!lua_checkstack (L, n + 4))
    {
      lua_settop (L, base);
      if (output != NULL)
        *output = take_output (self);
      g_set_error_literal (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_RUNTIME,
                           "too many results to display");
      return NULL;
    }

  store_context_init (&ctx, self);
  lua_pushcfunction (L, store_values);
  lua_pushlightuserdata (L, &ctx);
  for (i = 1; i <= n; i++)
    lua_pushvalue (L, base + i);
  ok = shell_lua_state_pcall (state, n + 1, 0, &local_error);
  lua_settop (L, base);

  if (output != NULL)
    *output = take_output (self);
  else
    g_string_truncate (self->output, 0);

  if (!ok)
    {
      store_context_clear (&ctx);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  g_string_free (ctx.scratch, TRUE);
  return g_variant_ref_sink (g_variant_builder_end (&ctx.builder));
}

static GVariant *
first_entry (GVariant *entries)
{
  GVariant *entry = g_variant_get_child_value (entries, 0);

  g_variant_unref (entries);
  return entry;
}

GVariant *
shell_lua_inspector_insert (ShellLuaInspector  *self,
                            guint64             id,
                            GError            **error)
{
  g_autoptr (GError) local_error = NULL;
  StoreContext ctx;
  ShellLuaState *state;
  lua_State *L;
  gboolean ok;
  int base;

  g_return_val_if_fail (SHELL_IS_LUA_INSPECTOR (self), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  state = ensure_state (self);
  L = shell_lua_state_get_L (state);
  base = lua_gettop (L);

  if (!lua_checkstack (L, 8))
    return NULL;

  store_context_init (&ctx, self);
  lua_pushcfunction (L, store_values);
  lua_pushlightuserdata (L, &ctx);
  if (!push_stored (L, id))
    {
      lua_settop (L, base);
      store_context_clear (&ctx);
      g_set_error (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_RUNTIME,
                   "The value %" G_GUINT64_FORMAT " is no longer available", id);
      return NULL;
    }

  ok = shell_lua_state_pcall (state, 2, 0, &local_error);
  lua_settop (L, base);
  if (!ok)
    {
      store_context_clear (&ctx);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  g_string_free (ctx.scratch, TRUE);
  return first_entry (g_variant_ref_sink (g_variant_builder_end (&ctx.builder)));
}

GVariant *
shell_lua_inspector_add_object_result (ShellLuaInspector  *self,
                                       GObject            *object,
                                       GError            **error)
{
  g_autoptr (GError) local_error = NULL;
  StoreContext ctx;
  ShellLuaState *state;
  lua_State *L;
  gboolean ok;
  int base;

  g_return_val_if_fail (SHELL_IS_LUA_INSPECTOR (self), NULL);
  g_return_val_if_fail (G_IS_OBJECT (object), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  state = ensure_state (self);
  L = shell_lua_state_get_L (state);
  base = lua_gettop (L);

  if (!lua_checkstack (L, 8))
    return NULL;

  store_context_init (&ctx, self);
  lua_pushcfunction (L, store_object);
  lua_pushlightuserdata (L, &ctx);
  lua_pushlightuserdata (L, object);
  ok = shell_lua_state_pcall (state, 2, 0, &local_error);
  lua_settop (L, base);
  if (!ok)
    {
      store_context_clear (&ctx);
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  g_string_free (ctx.scratch, TRUE);
  return first_entry (g_variant_ref_sink (g_variant_builder_end (&ctx.builder)));
}

typedef struct
{
  ShellLuaInspector *self;
  guint64            id;
} PinContext;

/* args: lightuserdata PinContext, lightuserdata GObject */
static int
pin_object_cb (lua_State *L)
{
  PinContext *ctx = lua_touserdata (L, 1);
  GObject *object = lua_touserdata (L, 2);

  luaL_checkstack (L, 8, "pinning an object");
  shell_lua_inspector_push_object (L, object);
  ctx->id = ctx->self->next_pin++;
  store_value (L, -1, ctx->id);
  return 0;
}

guint64
shell_lua_inspector_pin_object (ShellLuaInspector  *self,
                                GObject            *object,
                                GError            **error)
{
  PinContext ctx = { self, 0 };
  ShellLuaState *state;
  lua_State *L;
  gboolean ok;
  int base;

  g_return_val_if_fail (SHELL_IS_LUA_INSPECTOR (self), 0);
  g_return_val_if_fail (G_IS_OBJECT (object), 0);
  g_return_val_if_fail (error == NULL || *error == NULL, 0);

  state = ensure_state (self);
  L = shell_lua_state_get_L (state);
  base = lua_gettop (L);

  if (!lua_checkstack (L, 6))
    return 0;

  lua_pushcfunction (L, pin_object_cb);
  lua_pushlightuserdata (L, &ctx);
  lua_pushlightuserdata (L, object);
  ok = shell_lua_state_pcall (state, 2, 0, error);
  lua_settop (L, base);

  return ok ? ctx.id : 0;
}

GObject *
shell_lua_inspector_get_object (ShellLuaInspector *self,
                                guint64            id)
{
  lua_State *L;
  GObject *object = NULL;
  int base;

  g_return_val_if_fail (SHELL_IS_LUA_INSPECTOR (self), NULL);

  if (self->state == NULL || !shell_lua_state_is_alive (self->state))
    return NULL;

  L = shell_lua_state_get_L (self->state);
  base = lua_gettop (L);
  if (!lua_checkstack (L, 6))
    return NULL;

  if (push_stored (L, id) && shell_lua_handle_is_alive (L, -1))
    object = g_object_ref (shell_lua_handle_peek (L, -1, NULL));

  lua_settop (L, base);
  return object;
}

void
shell_lua_inspector_release (ShellLuaInspector *self,
                             guint64            id)
{
  lua_State *L;
  int base;

  g_return_if_fail (SHELL_IS_LUA_INSPECTOR (self));

  if (self->state == NULL || !shell_lua_state_is_alive (self->state))
    return;

  L = shell_lua_state_get_L (self->state);
  base = lua_gettop (L);
  if (!lua_checkstack (L, 4))
    return;

  forget_value (L, id);
  lua_settop (L, base);
}

/* ------------------------------------------------------------------ */
/* Inspecting a value                                                 */
/* ------------------------------------------------------------------ */

typedef struct
{
  char    *key;
  char    *summary;
  guint64  pin;
  gboolean is_integer;
  lua_Integer integer;
} Member;

static void
member_free (Member *member)
{
  g_free (member->key);
  g_free (member->summary);
  g_free (member);
}

static int
compare_members (gconstpointer a,
                 gconstpointer b)
{
  const Member *ma = *(const Member * const *) a;
  const Member *mb = *(const Member * const *) b;

  if (ma->is_integer && mb->is_integer)
    return ma->integer < mb->integer ? -1 : ma->integer > mb->integer;
  if (ma->is_integer)
    return -1;
  if (mb->is_integer)
    return 1;
  return strcmp (ma->key, mb->key);
}

typedef struct
{
  ShellLuaInspector *self;
  guint64            id;
  GString           *scratch;
  char              *type_name;
  char              *summary;
  GPtrArray         *members;   /* Member */
} InspectContext;

/* Gives the value on the top of the stack an id if it has structure. */
static guint64
pin_top (lua_State      *L,
         InspectContext *ctx)
{
  guint64 pin;

  if (!has_structure (L, -1))
    return 0;

  pin = ctx->self->next_pin++;
  store_value (L, -1, pin);
  return pin;
}

/* Adds a member whose value is at the top of the stack. */
static Member *
add_member (lua_State      *L,
            InspectContext *ctx,
            char           *key)
{
  Member *member = g_new0 (Member, 1);

  member->key = key;
  g_string_truncate (ctx->scratch, 0);
  summarize (L, -1, ctx->scratch);
  member->summary = one_line (ctx->scratch);
  member->pin = pin_top (L, ctx);
  g_ptr_array_add (ctx->members, member);
  return member;
}

static void
add_table_members (lua_State      *L,
                   int             idx,
                   InspectContext *ctx)
{
  idx = lua_absindex (L, idx);

  lua_pushnil (L);
  while (lua_next (L, idx) != 0)
    {
      Member *member;
      GString *key;

      if (ctx->members->len >= MAX_MEMBERS)
        {
          lua_pop (L, 2);
          break;
        }

      key = g_string_new (NULL);
      if (lua_type (L, -2) == LUA_TSTRING)
        {
          size_t len;
          const char *s = lua_tolstring (L, -2, &len);

          g_string_append_len (key, s, len);
        }
      else
        {
          g_string_append_c (key, '[');
          append_tostring (L, -2, key);
          g_string_append_c (key, ']');
        }

      member = add_member (L, ctx, make_valid (key->str, key->len));
      g_string_free (key, TRUE);

      if (lua_isinteger (L, -2))
        {
          member->is_integer = TRUE;
          member->integer = lua_tointeger (L, -2);
        }

      lua_pop (L, 1);
    }
}

static int
l_index (lua_State *L)
{
  lua_settop (L, 2);
  lua_gettable (L, 1);
  return 1;
}

typedef struct
{
  GPtrArray *properties;
  GPtrArray *methods;
} ObjectMembers;

static void
collect_member_name (const char *name,
                     gboolean    is_method,
                     gpointer    user_data)
{
  ObjectMembers *members = user_data;

  g_ptr_array_add (is_method ? members->methods : members->properties,
                   g_strdup (name));
}

static void
add_object_members (lua_State      *L,
                    int             idx,
                    InspectContext *ctx)
{
  ObjectMembers members;
  guint i;

  idx = lua_absindex (L, idx);
  members.properties = g_ptr_array_new_with_free_func (g_free);
  members.methods = g_ptr_array_new_with_free_func (g_free);
  shell_lua_handle_foreach_member (L, idx, collect_member_name, &members);

  for (i = 0; i < members.properties->len && ctx->members->len < MAX_MEMBERS; i++)
    {
      const char *name = g_ptr_array_index (members.properties, i);

      /* A property whose getter fails is shown as an error */
      lua_pushcfunction (L, l_index);
      lua_pushvalue (L, idx);
      lua_pushstring (L, name);
      if (lua_pcall (L, 2, 1, 0) != LUA_OK)
        {
          Member *member = g_new0 (Member, 1);
          size_t len;
          const char *message = lua_tolstring (L, -1, &len);
          g_autofree char *valid = make_valid (message != NULL ? message : "?", message != NULL ? (gssize) len : 1);

          member->key = g_strdup (name);
          member->summary = g_strdup_printf ("<error: %s>", valid);
          g_ptr_array_add (ctx->members, member);
        }
      else
        {
          add_member (L, ctx, g_strdup (name));
        }
      lua_pop (L, 1);
    }

  for (i = 0; i < members.methods->len && ctx->members->len < MAX_MEMBERS; i++)
    {
      Member *member = g_new0 (Member, 1);

      member->key = g_strdup_printf ("%s ()", (char *) g_ptr_array_index (members.methods, i));
      member->summary = g_strdup ("function");
      g_ptr_array_add (ctx->members, member);
    }

  g_ptr_array_unref (members.properties);
  g_ptr_array_unref (members.methods);
}

/* args: lightuserdata InspectContext */
static int
inspect_value (lua_State *L)
{
  InspectContext *ctx = lua_touserdata (L, 1);

  luaL_checkstack (L, 12, "inspecting");

  if (!push_stored (L, ctx->id))
    return luaL_error (L, "The value %I is no longer available",
                       (lua_Integer) ctx->id);

  /* the value is at 2 */
  ctx->type_name = type_name_of (L, 2);
  summarize (L, 2, ctx->scratch);
  ctx->summary = one_line (ctx->scratch);

  if (lua_type (L, 2) == LUA_TTABLE)
    add_table_members (L, 2, ctx);
  else if (shell_lua_handle_is_alive (L, 2))
    add_object_members (L, 2, ctx);

  g_ptr_array_sort (ctx->members, compare_members);
  return 0;
}

GVariant *
shell_lua_inspector_inspect (ShellLuaInspector  *self,
                             guint64             id,
                             GError            **error)
{
  g_autoptr (GError) local_error = NULL;
  InspectContext ctx = { self, id, NULL, NULL, NULL, NULL };
  GVariantBuilder builder;
  ShellLuaState *state;
  lua_State *L;
  GVariant *result;
  gboolean ok;
  guint i;
  int base;

  g_return_val_if_fail (SHELL_IS_LUA_INSPECTOR (self), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  state = ensure_state (self);
  L = shell_lua_state_get_L (state);
  base = lua_gettop (L);

  if (!lua_checkstack (L, 4))
    return NULL;

  ctx.scratch = g_string_new (NULL);
  ctx.members = g_ptr_array_new_with_free_func ((GDestroyNotify) member_free);

  lua_pushcfunction (L, inspect_value);
  lua_pushlightuserdata (L, &ctx);
  ok = shell_lua_state_pcall (state, 1, 0, &local_error);
  lua_settop (L, base);

  if (ok)
    {
      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sst)"));
      for (i = 0; i < ctx.members->len; i++)
        {
          Member *member = g_ptr_array_index (ctx.members, i);

          g_variant_builder_add (&builder, "(sst)", member->key,
                                 member->summary, member->pin);
        }
      result = g_variant_new ("(ss@a(sst))", ctx.type_name, ctx.summary,
                              g_variant_builder_end (&builder));
      g_variant_ref_sink (result);
    }
  else
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      result = NULL;
    }

  g_free (ctx.type_name);
  g_free (ctx.summary);
  g_ptr_array_unref (ctx.members);
  g_string_free (ctx.scratch, TRUE);
  return result;
}

/* ------------------------------------------------------------------ */
/* Completion                                                         */
/* ------------------------------------------------------------------ */

static const char * const lua_keywords[] = {
  "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
  "global", "goto", "if", "in", "local", "nil", "not", "or", "repeat",
  "return", "then", "true", "until", "while", NULL
};

static gboolean
is_identifier_char (char c)
{
  return g_ascii_isalnum (c) || c == '_';
}

static gboolean
is_identifier (const char *s)
{
  const char *p;

  if (!(g_ascii_isalpha (s[0]) || s[0] == '_'))
    return FALSE;
  for (p = s + 1; *p != '\0'; p++)
    if (!is_identifier_char (*p))
      return FALSE;
  return TRUE;
}

/* Splits @text into the path before the last `.` or `:`, which separator it
 * was and the word being typed. FALSE if the text is not completable. */
static gboolean
parse_completion (const char  *text,
                  GPtrArray   *path,
                  char        *separator,
                  char       **partial)
{
  size_t i = strlen (text);
  size_t end = i;

  while (i > 0 && is_identifier_char (text[i - 1]))
    i--;
  *partial = g_strndup (text + i, end - i);

  /* a number */
  if (**partial != '\0' && g_ascii_isdigit (**partial))
    return FALSE;

  if (i == 0 || (text[i - 1] != '.' && text[i - 1] != ':'))
    {
      *separator = '\0';
      return TRUE;
    }

  *separator = text[i - 1];
  i--;

  for (;;)
    {
      size_t id_end = i;

      while (i > 0 && is_identifier_char (text[i - 1]))
        i--;
      if (i == id_end || g_ascii_isdigit (text[i]))
        return FALSE;

      g_ptr_array_insert (path, 0, g_strndup (text + i, id_end - i));

      if (i > 0 && text[i - 1] == '.')
        {
          i--;
          continue;
        }

      /* a call, an index or a method call is not a plain path */
      if (i > 0 && (text[i - 1] == ':' || text[i - 1] == ')' ||
                    text[i - 1] == ']' || text[i - 1] == '"' ||
                    text[i - 1] == '\''))
        return FALSE;

      return TRUE;
    }
}

typedef struct
{
  GPtrArray  *path;       /* char* */
  char        separator;
  const char *partial;
  GHashTable *found;      /* char* -> NULL, owned keys */
} CompleteContext;

static void
add_candidate (CompleteContext *ctx,
               const char      *name)
{
  if (!g_str_has_prefix (name, ctx->partial) || !is_identifier (name))
    return;

  if (!g_hash_table_contains (ctx->found, name))
    g_hash_table_add (ctx->found, g_strdup (name));
}

static void
add_table_candidates (lua_State       *L,
                      int              idx,
                      CompleteContext *ctx)
{
  gboolean methods_only = ctx->separator == ':';

  idx = lua_absindex (L, idx);

  lua_pushnil (L);
  while (lua_next (L, idx) != 0)
    {
      if (lua_type (L, -2) == LUA_TSTRING &&
          (!methods_only || lua_type (L, -1) == LUA_TFUNCTION))
        add_candidate (ctx, lua_tostring (L, -2));
      lua_pop (L, 1);
    }
}

static void
add_member_candidate (const char *name,
                      gboolean    is_method,
                      gpointer    user_data)
{
  CompleteContext *ctx = user_data;

  if (ctx->separator == ':' && !is_method)
    return;

  add_candidate (ctx, name);
}

/* args: lightuserdata CompleteContext */
static int
collect_candidates (lua_State *L)
{
  CompleteContext *ctx = lua_touserdata (L, 1);
  int value;
  int depth;
  guint i;

  luaL_checkstack (L, 16, "completing");

  lua_pushglobaltable (L);
  value = lua_gettop (L);

  if (ctx->separator == '\0')
    {
      add_table_candidates (L, value, ctx);
      for (i = 0; lua_keywords[i] != NULL; i++)
        add_candidate (ctx, lua_keywords[i]);
      return 0;
    }

  for (i = 0; i < ctx->path->len; i++)
    {
      lua_getfield (L, -1, g_ptr_array_index (ctx->path, i));
      lua_remove (L, -2);
    }
  value = lua_gettop (L);

  if (lua_type (L, value) == LUA_TTABLE)
    add_table_candidates (L, value, ctx);
  if (shell_lua_handle_is_alive (L, value))
    shell_lua_handle_foreach_member (L, value, add_member_candidate, ctx);

  /* what the metatables offer (the methods of a string, classes) */
  lua_pushvalue (L, value);
  for (depth = 0; depth < MAX_METATABLE_DEPTH; depth++)
    {
      if (luaL_getmetafield (L, -1, "__index") != LUA_TTABLE)
        {
          if (lua_type (L, -1) != LUA_TNIL)
            lua_pop (L, 1);
          break;
        }

      add_table_candidates (L, -1, ctx);
      lua_remove (L, -2);
    }

  return 0;
}

char **
shell_lua_inspector_complete (ShellLuaInspector  *self,
                              const char         *text,
                              char              **head)
{
  g_autoptr (GPtrArray) path = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *partial = NULL;
  CompleteContext ctx = { NULL, '\0', NULL, NULL };
  g_autoptr (GError) error = NULL;
  ShellLuaState *state;
  lua_State *L;
  GPtrArray *words;
  GHashTableIter iter;
  gpointer key;
  char separator = '\0';
  int base;

  g_return_val_if_fail (SHELL_IS_LUA_INSPECTOR (self), NULL);
  g_return_val_if_fail (text != NULL, NULL);

  if (!parse_completion (text, path, &separator, &partial))
    {
      if (head != NULL)
        *head = g_strdup ("");
      return g_new0 (char *, 1);
    }

  state = ensure_state (self);
  L = shell_lua_state_get_L (state);
  base = lua_gettop (L);

  ctx.path = path;
  ctx.separator = separator;
  ctx.partial = partial;
  ctx.found = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (lua_checkstack (L, 4))
    {
      lua_pushcfunction (L, collect_candidates);
      lua_pushlightuserdata (L, &ctx);
      /* An error (an __index that raises, a path through nil) just means
       * that there is nothing to complete. */
      if (!shell_lua_state_pcall (state, 1, 0, &error))
        g_hash_table_remove_all (ctx.found);
      lua_settop (L, base);
    }

  words = g_ptr_array_new ();
  g_hash_table_iter_init (&iter, ctx.found);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_ptr_array_add (words, key);
  g_ptr_array_sort (words, compare_strings);
  g_ptr_array_add (words, NULL);

  {
    char **result = g_new0 (char *, words->len);
    guint i;

    for (i = 0; i + 1 < words->len; i++)
      result[i] = g_strdup (g_ptr_array_index (words, i));

    g_ptr_array_free (words, TRUE);
    g_hash_table_unref (ctx.found);

    if (head != NULL)
      *head = g_steal_pointer (&partial);
    return result;
  }
}

/* ------------------------------------------------------------------ */
/* Introspection                                                      */
/* ------------------------------------------------------------------ */

void
shell_lua_inspector_gc (ShellLuaInspector *self)
{
  ShellLuaExtensionManager *manager;
  GList *extensions, *l;

  g_return_if_fail (SHELL_IS_LUA_INSPECTOR (self));

  manager = shell_lua_extension_manager_get_default ();
  extensions = shell_lua_extension_manager_list (manager);
  for (l = extensions; l != NULL; l = l->next)
    {
      ShellLuaState *ext_state =
        shell_lua_extension_get_lua_state (l->data);

      if (ext_state != NULL && shell_lua_state_is_alive (ext_state))
        shell_lua_state_gc (ext_state);
    }
  g_list_free (extensions);

  if (self->state != NULL && shell_lua_state_is_alive (self->state))
    shell_lua_state_gc (self->state);
}

/* a(sstuu): uuid, state name, Lua heap bytes, error count, state number
 * (ShellLuaExtensionState; ACTIVE for the inspector itself). */
GVariant *
shell_lua_inspector_get_states (ShellLuaInspector *self)
{
  ShellLuaExtensionManager *manager;
  ShellLuaState *state;
  GVariantBuilder builder;
  GList *extensions, *l;

  g_return_val_if_fail (SHELL_IS_LUA_INSPECTOR (self), NULL);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sstuu)"));

  manager = shell_lua_extension_manager_get_default ();
  extensions = shell_lua_extension_manager_list (manager);
  for (l = extensions; l != NULL; l = l->next)
    {
      ShellLuaExtension *extension = l->data;
      ShellLuaState *ext_state = shell_lua_extension_get_lua_state (extension);
      ShellLuaExtensionState ext_state_id =
        shell_lua_extension_get_state (extension);

      if (ext_state == NULL || !shell_lua_state_is_alive (ext_state))
        continue;

      g_variant_builder_add (&builder, "(sstuu)",
                             shell_lua_extension_get_uuid (extension),
                             shell_lua_extension_state_to_string (ext_state_id),
                             (uint64_t) shell_lua_state_get_memory_usage (ext_state),
                             (uint32_t) shell_lua_state_get_error_count (ext_state),
                             (uint32_t) ext_state_id);
    }
  g_list_free (extensions);

  state = ensure_state (self);
  g_variant_builder_add (&builder, "(sstuu)",
                         INSPECTOR_ID,
                         shell_lua_extension_state_to_string (SHELL_LUA_EXTENSION_STATE_ACTIVE),
                         (uint64_t) shell_lua_state_get_memory_usage (state),
                         (uint32_t) shell_lua_state_get_error_count (state),
                         (uint32_t) SHELL_LUA_EXTENSION_STATE_ACTIVE);

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}
