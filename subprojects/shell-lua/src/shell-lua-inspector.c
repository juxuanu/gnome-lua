/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaInspector: the privileged state behind Looking Glass's Lua tab.
 */

#include <string.h>

#include "shell-lua.h"
#include "shell-lua-extension.h"
#include "shell-lua-extension-private.h"
#include "shell-lua-inspector.h"

#define INSPECTOR_ID "inspector"
#define INSPECTOR_ROOT_URI "resource:///org/gnome/shell/lua"

/* "Unlimited" wall-clock budget: far enough away (about 31 years) not to
 * overflow when added to a monotonic timestamp. */
#define INSPECTOR_BUDGET_US (G_GINT64_CONSTANT (1000000000000000))

#define MAX_TABLE_ENTRIES 32
#define MAX_RESULTS 200

struct _ShellLuaInspector
{
  GObject parent_instance;

  ShellLuaState *state;
};

G_DEFINE_FINAL_TYPE (ShellLuaInspector, shell_lua_inspector, G_TYPE_OBJECT)

static ShellLuaState *
create_state (void)
{
  ShellLuaLimits limits = { 0, };
  g_autoptr (GFile) root = NULL;
  ShellLuaState *state;

  limits.callback_budget_us = INSPECTOR_BUDGET_US;
  limits.memory_limit = 0;

  state = shell_lua_state_new (INSPECTOR_ID, SHELL_LUA_STATE_PRIVILEGED,
                               &limits);

  root = g_file_new_for_uri (INSPECTOR_ROOT_URI);
  shell_lua_state_add_search_root (state, root);

  return state;
}

static ShellLuaState *
ensure_state (ShellLuaInspector *self)
{
  if (self->state != NULL && !shell_lua_state_is_alive (self->state))
    g_clear_object (&self->state);

  if (self->state == NULL)
    self->state = create_state ();

  return self->state;
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
shell_lua_inspector_class_init (ShellLuaInspectorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_lua_inspector_dispose;
}

static void
shell_lua_inspector_init (ShellLuaInspector *self)
{
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

char *
shell_lua_inspector_eval (ShellLuaInspector  *self,
                          const char         *code,
                          GError            **error)
{
  g_autoptr (GError) expr_error = NULL;
  g_autoptr (GError) local_error = NULL;
  g_autofree char *expression = NULL;
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
          return NULL;
        }
    }

  if (!shell_lua_state_pcall (state, 0, LUA_MULTRET, &local_error))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      lua_settop (L, base);
      return NULL;
    }

  n = lua_gettop (L) - base;
  if (n > MAX_RESULTS)
    {
      lua_settop (L, base + MAX_RESULTS);
      n = MAX_RESULTS;
    }

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

  self->state = create_state ();
}

/* ------------------------------------------------------------------ */
/* Introspection                                                      */
/* ------------------------------------------------------------------ */

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
