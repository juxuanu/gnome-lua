/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Glue between ShellLuaState and the api-*.c modules: the module registry,
 * the state lookup from a lua_State, the option-table helpers and the
 * callback runner. See shell-lua-api-private.h.
 */

#ifndef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ShellLua"
#endif

#include "shell-lua-api-private.h"

/* Must match shell-lua.c */
#define KEY_STATE "shell.lua.state"

typedef struct {
  char                    *name;
  ShellLuaApiRegisterFunc  func;
} ApiModule;

static GArray *modules = NULL;

static void
api_module_clear (gpointer data)
{
  ApiModule *module = data;

  g_free (module->name);
}

/* Constructors in different translation units run in unspecified order, so
 * the list is created lazily and sorted by name when it is used. */
void
shell_lua_api_register (const char              *name,
                        ShellLuaApiRegisterFunc  func)
{
  ApiModule module;
  guint i;

  g_return_if_fail (name != NULL);
  g_return_if_fail (func != NULL);

  if (modules == NULL)
    {
      modules = g_array_new (FALSE, FALSE, sizeof (ApiModule));
      g_array_set_clear_func (modules, api_module_clear);
    }

  for (i = 0; i < modules->len; i++)
    {
      if (g_array_index (modules, ApiModule, i).func == func)
        return;
    }

  module.name = g_strdup (name);
  module.func = func;
  g_array_append_val (modules, module);
}

static int
compare_modules (gconstpointer a,
                 gconstpointer b)
{
  const ApiModule *ma = a;
  const ApiModule *mb = b;

  return g_strcmp0 (ma->name, mb->name);
}

void
shell_lua_api_populate (ShellLuaState *state,
                        lua_State     *L)
{
  guint i;

  g_return_if_fail (SHELL_IS_LUA_STATE (state));
  g_return_if_fail (L != NULL);
  g_return_if_fail (lua_istable (L, -1));

  if (modules == NULL)
    return;

  /* Deterministic order independent of link order */
  g_array_sort (modules, compare_modules);

  for (i = 0; i < modules->len; i++)
    {
      ApiModule *module = &g_array_index (modules, ApiModule, i);
      int top = lua_gettop (L);

      module->func (state, L);

      if (lua_gettop (L) != top)
        g_error ("Lua API module '%s' left the stack unbalanced (%d -> %d)",
                 module->name, top, lua_gettop (L));
    }
}

ShellLuaState *
shell_lua_state_from_L (lua_State *L)
{
  ShellLuaState *state;

  g_return_val_if_fail (L != NULL, NULL);

  lua_getfield (L, LUA_REGISTRYINDEX, KEY_STATE);
  state = lua_touserdata (L, -1);
  lua_pop (L, 1);

  return state;
}

/* ------------------------------------------------------------------------ */
/* Option helpers                                                            */
/* ------------------------------------------------------------------------ */

static void
check_options_table (lua_State *L,
                     int        tbl)
{
  if (!lua_istable (L, tbl))
    luaL_error (L, "options table expected, got %s", luaL_typename (L, tbl));
}

const char *
shell_lua_opt_string (lua_State  *L,
                      int         tbl,
                      const char *field,
                      const char *def)
{
  const char *value;

  check_options_table (L, tbl);

  if (lua_getfield (L, tbl, field) == LUA_TNIL)
    {
      lua_pop (L, 1);
      return def;
    }

  if (lua_type (L, -1) != LUA_TSTRING)
    luaL_error (L, "field '%s' must be a string, got %s", field, luaL_typename (L, -1));

  /* The table keeps the string alive, so the pointer stays valid as long as
   * the field is not reassigned. */
  value = lua_tostring (L, -1);
  lua_pop (L, 1);
  return value;
}

lua_Integer
shell_lua_opt_integer (lua_State  *L,
                       int         tbl,
                       const char *field,
                       lua_Integer def)
{
  lua_Integer value;
  int isnum;

  check_options_table (L, tbl);

  if (lua_getfield (L, tbl, field) == LUA_TNIL)
    {
      lua_pop (L, 1);
      return def;
    }

  if (lua_type (L, -1) != LUA_TNUMBER)
    luaL_error (L, "field '%s' must be an integer, got %s", field, luaL_typename (L, -1));

  value = lua_tointegerx (L, -1, &isnum);
  if (!isnum)
    luaL_error (L, "field '%s' must be an integer (number has no integer representation)", field);

  lua_pop (L, 1);
  return value;
}

lua_Number
shell_lua_opt_number (lua_State  *L,
                      int         tbl,
                      const char *field,
                      lua_Number  def)
{
  lua_Number value;

  check_options_table (L, tbl);

  if (lua_getfield (L, tbl, field) == LUA_TNIL)
    {
      lua_pop (L, 1);
      return def;
    }

  if (lua_type (L, -1) != LUA_TNUMBER)
    luaL_error (L, "field '%s' must be a number, got %s", field, luaL_typename (L, -1));

  value = lua_tonumber (L, -1);
  lua_pop (L, 1);
  return value;
}

gboolean
shell_lua_opt_boolean (lua_State  *L,
                       int         tbl,
                       const char *field,
                       gboolean    def)
{
  gboolean value;

  check_options_table (L, tbl);

  if (lua_getfield (L, tbl, field) == LUA_TNIL)
    {
      lua_pop (L, 1);
      return def;
    }

  if (lua_type (L, -1) != LUA_TBOOLEAN)
    luaL_error (L, "field '%s' must be a boolean, got %s", field, luaL_typename (L, -1));

  value = lua_toboolean (L, -1);
  lua_pop (L, 1);
  return value;
}

int
shell_lua_opt_callback (lua_State  *L,
                        int         tbl,
                        const char *field,
                        gboolean    required)
{
  check_options_table (L, tbl);

  if (lua_getfield (L, tbl, field) == LUA_TNIL)
    {
      lua_pop (L, 1);
      if (required)
        luaL_error (L, "missing required field '%s' (a function)", field);
      return LUA_NOREF;
    }

  if (lua_type (L, -1) != LUA_TFUNCTION)
    {
      /* callable tables / userdata are accepted */
      if (luaL_getmetafield (L, -1, "__call") == LUA_TNIL)
        luaL_error (L, "field '%s' must be a function, got %s", field, luaL_typename (L, -1));
      lua_pop (L, 1);
    }

  return luaL_ref (L, LUA_REGISTRYINDEX);
}

/* ------------------------------------------------------------------------ */
/* Callback runner                                                           */
/* ------------------------------------------------------------------------ */

typedef struct {
  int                   ref;
  int                   nargs;
  ShellLuaPushArgsFunc  push_args;
  gpointer              user_data;
} CallContext;

/* Runs inside the protected call so that errors raised while pushing
 * arguments (e.g. a handle for an unregistered type) are caught too. */
static int
call_trampoline (lua_State *L)
{
  CallContext *ctx = lua_touserdata (L, 1);
  int top;

  lua_settop (L, 0);
  luaL_checkstack (L, ctx->nargs + 2, "too many callback arguments");

  lua_rawgeti (L, LUA_REGISTRYINDEX, ctx->ref);
  top = lua_gettop (L);
  if (ctx->push_args != NULL)
    ctx->push_args (L, ctx->user_data);
  if (lua_gettop (L) != top + ctx->nargs)
    return luaL_error (L, "callback argument count mismatch (expected %d, pushed %d)",
                       ctx->nargs, lua_gettop (L) - top);

  lua_call (L, ctx->nargs, 0);
  return 0;
}

gboolean
shell_lua_call_ref (ShellLuaState        *state,
                    int                   ref,
                    int                   nargs,
                    ShellLuaPushArgsFunc  push_args,
                    gpointer              user_data)
{
  g_autoptr(GError) error = NULL;
  CallContext ctx = { ref, nargs, push_args, user_data };
  lua_State *L;
  int top;

  g_return_val_if_fail (SHELL_IS_LUA_STATE (state), FALSE);
  g_return_val_if_fail (ref != LUA_NOREF && ref != LUA_REFNIL, FALSE);
  g_return_val_if_fail (nargs >= 0, FALSE);
  g_return_val_if_fail (nargs == 0 || push_args != NULL, FALSE);

  if (!shell_lua_state_is_alive (state))
    {
      g_debug ("[%s] not calling callback ref %d: state is dead",
               shell_lua_state_get_id (state), ref);
      return FALSE;
    }

  L = shell_lua_state_get_L (state);
  top = lua_gettop (L);

  if (!lua_checkstack (L, 3))
    return FALSE;

  lua_pushcfunction (L, call_trampoline);
  lua_pushlightuserdata (L, &ctx);

  if (!shell_lua_state_pcall (state, 1, 0, &error))
    {
      g_debug ("[%s] callback ref %d failed: %s",
               shell_lua_state_get_id (state), ref, error->message);
      return FALSE;
    }

  g_assert (lua_gettop (L) == top);
  return TRUE;
}
