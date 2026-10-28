/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.hooks: connect Lua functions to the named hook points of
 * ShellHookRegistry. See docs/api/core.md.
 *
 *   shell.hooks.on(name, fn)       -> shell.Connection   (after phase)
 *   shell.hooks.before(name, fn)   -> shell.Connection   (before phase; fn may
 *                                     return shell.hooks.CANCEL or a table of
 *                                     amendments)
 *   shell.hooks.list()             -> { {name=, cancellable=, doc=, schema=}, ... }
 *   shell.hooks.CANCEL             unique sentinel (lightuserdata)
 *   shell.hooks.emit_before(name, payload) -> cancelled (boolean), amendments (table|nil)
 *   shell.hooks.emit_after(name, payload)
 *       (the two emit functions raise unless the state is PRIVILEGED)
 */

#include <glib.h>

#include "shell-hooks.h"
#include <string.h>

#include "shell-lua-api-private.h"
#include "shell-lua-connection-private.h"

/* shell.hooks.CANCEL is the address of this variable. */
static char cancel_sentinel;

static void __attribute__ ((constructor))
shell_lua_api_hooks_init (void)
{
  shell_lua_api_register ("hooks", shell_lua_api_hooks_register);
}

/* ------------------------------------------------------------------------ */
/* Listeners                                                                 */
/* ------------------------------------------------------------------------ */

/* Shared between the registry (which frees it through its destroy notify,
 * possibly late if an emission still holds the listener) and the
 * ShellLuaConnection (which disconnects it). */
typedef struct {
  int            refcount;
  ShellLuaState *state;
  int            ref;         /* callback; owned by the connection */
  ShellHookPhase phase;
  guint          id;
  gboolean       connected;
} Listener;

static void
listener_unref (gpointer data)
{
  Listener *d = data;

  if (--d->refcount == 0)
    g_free (d);
}

/* disconnect_fn of the ShellLuaConnection */
static void
listener_disconnect (gpointer data)
{
  Listener *d = data;

  d->connected = FALSE;
  if (d->id != 0)
    shell_hook_registry_disconnect (shell_hook_registry_get_default (), d->id);
  listener_unref (d);
}

typedef struct {
  Listener        *listener;
  GVariant        *payload;
  GVariantDict    *amend;
  ShellHookVerdict verdict;
} HookCall;

static void
merge_amendments (GVariant     *amendments,
                  GVariantDict *amend)
{
  GVariantIter iter;
  const char *key;
  GVariant *value;

  g_variant_iter_init (&iter, amendments);
  while (g_variant_iter_next (&iter, "{&sv}", &key, &value))
    {
      g_variant_dict_insert_value (amend, key, value);
      g_variant_unref (value);
    }
}

/* Runs under shell_lua_state_pcall(): free to raise. */
static int
hook_trampoline (lua_State *L,
                 gpointer   data)
{
  HookCall *call = data;
  Listener *d = call->listener;

  shell_lua_state_push_ref (d->state, d->ref);
  if (call->payload != NULL)
    shell_lua_push_variant (L, call->payload);
  else
    lua_newtable (L);
  lua_call (L, 1, 1);

  if (d->phase == SHELL_HOOK_PHASE_BEFORE && call->amend != NULL)
    {
      if (lua_islightuserdata (L, -1) && lua_touserdata (L, -1) == (void *) &cancel_sentinel)
        {
          call->verdict = SHELL_HOOK_VERDICT_CANCEL;
        }
      else if (lua_istable (L, -1))
        {
          /* Converted as a whole first: a table that cannot be represented
           * raises here and contributes no amendment at all. */
          GVariant *amendments = shell_lua_to_variant (L, -1, G_VARIANT_TYPE_VARDICT);

          merge_amendments (amendments, call->amend);
          g_variant_unref (amendments);
        }
    }

  return 0;
}

static ShellHookVerdict
hook_listener (const char     *name,
               ShellHookPhase  phase,
               GVariant       *payload,
               GVariantDict   *amend,
               gpointer        user_data)
{
  Listener *d = user_data;
  HookCall call = { d, payload, amend, SHELL_HOOK_VERDICT_CONTINUE };

  if (!d->connected || !shell_lua_state_is_alive (d->state))
    return SHELL_HOOK_VERDICT_CONTINUE;

  /* A failed call (error, watchdog, ...) is logged by the state and never
   * cancels. */
  if (!shell_lua_api_call_protected (d->state, 0, hook_trampoline, &call))
    return SHELL_HOOK_VERDICT_CONTINUE;

  return phase == SHELL_HOOK_PHASE_BEFORE ? call.verdict : SHELL_HOOK_VERDICT_CONTINUE;
}

/* Hooks that carry something sensitive (what the user types) and need a
 * capability to be connected to (docs/api/hooks.md). */
static const struct {
  const char *hook;
  const char *capability;
} gated_hooks[] = {
  { "search.query", "search" },
};

static const char *
hook_capability (const char *name)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (gated_hooks); i++)
    if (strcmp (gated_hooks[i].hook, name) == 0)
      return gated_hooks[i].capability;
  return NULL;
}

static int
hooks_connect (lua_State     *L,
               ShellHookPhase phase)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  const char *name = luaL_checkstring (L, 1);
  const char *capability;
  ShellLuaConnection *connection;
  Listener *d;
  int ref;

  luaL_checktype (L, 2, LUA_TFUNCTION);

  if (!shell_hook_registry_is_declared (registry, name))
    return luaL_error (L, "unknown hook '%s'", name);
  if (phase == SHELL_HOOK_PHASE_BEFORE && !shell_hook_registry_is_cancellable (registry, name))
    return luaL_error (L, "hook '%s' is not cancellable", name);

  capability = hook_capability (name);
  if (capability != NULL)
    {
      /* `hook 'search.query' requires the "search" capability` */
      lua_pushfstring (L, "hook '%s'", name);
      shell_lua_require_capability (L, capability, lua_tostring (L, -1));
      lua_pop (L, 1);
    }

  shell_lua_connection_register_handle_type (L);

  lua_pushvalue (L, 2);
  ref = shell_lua_state_ref (state);
  if (ref == LUA_NOREF)
    return luaL_error (L, "cannot store the callback");

  d = g_new0 (Listener, 1);
  d->refcount = 2;           /* registry + connection */
  d->state = state;
  d->ref = ref;
  d->phase = phase;
  d->connected = TRUE;

  connection = shell_lua_connection_new (state, ref, listener_disconnect, d);
  d->id = shell_hook_registry_connect (registry, name, phase, hook_listener, d, listener_unref);
  if (d->id == 0)
    {
      /* Cannot happen after the checks above, unless the registry changed. */
      shell_lua_connection_disconnect (connection);
      g_object_unref (connection);
      return luaL_error (L, "cannot connect to hook '%s'", name);
    }

  shell_lua_connection_own (connection);
  shell_lua_connection_push (connection, L);
  g_object_unref (connection);
  return 1;
}

static int
l_hooks_on (lua_State *L)
{
  return hooks_connect (L, SHELL_HOOK_PHASE_AFTER);
}

static int
l_hooks_before (lua_State *L)
{
  return hooks_connect (L, SHELL_HOOK_PHASE_BEFORE);
}

/* ------------------------------------------------------------------------ */
/* list / emit                                                               */
/* ------------------------------------------------------------------------ */

static int
l_hooks_list (lua_State *L)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  GStrv names = shell_hook_registry_list (registry);
  int i;

  luaL_checkstack (L, 6, "shell.hooks.list");
  lua_newtable (L);
  for (i = 0; names != NULL && names[i] != NULL; i++)
    {
      const char *doc = shell_hook_registry_get_doc (registry, names[i]);
      const char *schema = shell_hook_registry_get_schema (registry, names[i]);

      const char *capability = hook_capability (names[i]);

      lua_createtable (L, 0, 5);
      lua_pushstring (L, names[i]);
      lua_setfield (L, -2, "name");
      if (capability != NULL)
        {
          lua_pushstring (L, capability);
          lua_setfield (L, -2, "capability");
        }
      lua_pushboolean (L, shell_hook_registry_is_cancellable (registry, names[i]));
      lua_setfield (L, -2, "cancellable");
      if (doc != NULL)
        {
          lua_pushstring (L, doc);
          lua_setfield (L, -2, "doc");
        }
      if (schema != NULL)
        {
          lua_pushstring (L, schema);
          lua_setfield (L, -2, "schema");
        }
      lua_rawseti (L, -2, i + 1);
    }
  g_strfreev (names);
  return 1;
}

static void
require_privileged (lua_State  *L,
                    const char *what)
{
  ShellLuaState *state = shell_lua_state_from_L (L);

  if (state == NULL || shell_lua_state_get_kind (state) != SHELL_LUA_STATE_PRIVILEGED)
    luaL_error (L, "shell.hooks.%s is only available in privileged states", what);
}

/* Name and optional payload (argument 2) of an emit call. */
static GVariant *
emit_args (lua_State   *L,
           const char **name)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();

  *name = luaL_checkstring (L, 1);
  if (!lua_isnoneornil (L, 2))
    luaL_checktype (L, 2, LUA_TTABLE);
  if (!shell_hook_registry_is_declared (registry, *name))
    luaL_error (L, "unknown hook '%s'", *name);

  return lua_isnoneornil (L, 2) ? NULL : shell_lua_to_variant (L, 2, G_VARIANT_TYPE_VARDICT);
}

static int
l_hooks_emit_after (lua_State *L)
{
  const char *name;
  GVariant *payload;

  require_privileged (L, "emit_after");
  payload = emit_args (L, &name);
  shell_hook_registry_emit_after (shell_hook_registry_get_default (), name, payload);
  if (payload != NULL)
    g_variant_unref (payload);
  return 0;
}

static int
l_hooks_emit_before (lua_State *L)
{
  const char *name;
  GVariant *payload, *amendments = NULL;
  ShellHookVerdict verdict;

  require_privileged (L, "emit_before");
  payload = emit_args (L, &name);
  verdict = shell_hook_registry_emit_before (shell_hook_registry_get_default (),
                                             name, payload, &amendments);
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

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

static const luaL_Reg hooks_funcs[] = {
  { "on",          l_hooks_on },
  { "before",      l_hooks_before },
  { "list",        l_hooks_list },
  { "emit_after",  l_hooks_emit_after },
  { "emit_before", l_hooks_emit_before },
  { NULL, NULL }
};

void
shell_lua_api_hooks_register (ShellLuaState *state,
                              lua_State     *L)
{
  int shell = lua_absindex (L, -1);

  luaL_checkstack (L, 4, "shell.hooks");

  shell_lua_connection_register_handle_type (L);

  lua_newtable (L);
  luaL_setfuncs (L, hooks_funcs, 0);
  lua_pushlightuserdata (L, &cancel_sentinel);
  lua_setfield (L, -2, "CANCEL");
  lua_setfield (L, shell, "hooks");
}
