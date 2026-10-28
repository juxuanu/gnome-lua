/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellLuaConnection: the object behind shell.Connection handles, plus the
 * ownership / anchoring / protected-call helpers shared by the api-*.c
 * modules (see shell-lua-connection-private.h).
 */

#include "shell-lua-connection-private.h"

#include "shell-lua-api-private.h"
#include "shell-lua-extension.h"
#include "shell-lua-handle.h"

#define ANCHOR_MT "shell.lua.anchor"

static char anchors_key;

/* ------------------------------------------------------------------------ */
/* Ownership                                                                 */
/* ------------------------------------------------------------------------ */

static void
teardown_cb (gpointer data)
{
  GObject *object = data;

  g_object_run_dispose (object);
  g_object_unref (object);
}

void
shell_lua_api_own (ShellLuaState    *state,
                   GObject          *object,
                   ShellLuaOwnToken *token)
{
  gpointer owner;

  g_return_if_fail (SHELL_IS_LUA_STATE (state));
  g_return_if_fail (G_IS_OBJECT (object));
  g_return_if_fail (token != NULL);

  token->state = state;
  token->id = 0;
  token->object = NULL;

  owner = shell_lua_state_get_owner (state);
  if (SHELL_IS_LUA_EXTENSION (owner))
    {
      token->extension = TRUE;
      token->id = shell_lua_extension_own (SHELL_LUA_EXTENSION (owner), object, NULL);
    }
  else
    {
      token->extension = FALSE;
      token->object = g_object_ref (object);
      token->id = shell_lua_state_add_teardown (state, teardown_cb, token->object);
      if (token->id == 0)
        g_clear_object (&token->object);
    }
}

void
shell_lua_api_disown (ShellLuaOwnToken *token)
{
  guint id;

  if (token == NULL || token->id == 0 || token->state == NULL)
    return;

  id = token->id;
  token->id = 0;

  if (token->extension)
    {
      gpointer owner = shell_lua_state_get_owner (token->state);

      /* NULL once the extension tore its state down: the entry has already
       * been disposed then. */
      if (SHELL_IS_LUA_EXTENSION (owner))
        shell_lua_extension_release (SHELL_LUA_EXTENSION (owner), id);
    }
  else
    {
      GObject *object = g_steal_pointer (&token->object);

      shell_lua_state_remove_teardown (token->state, id);
      g_clear_object (&object);
    }
}

/* ------------------------------------------------------------------------ */
/* Anchors                                                                   */
/* ------------------------------------------------------------------------ */

static int
anchor_gc (lua_State *L)
{
  GObject **p = lua_touserdata (L, 1);

  if (p != NULL && *p != NULL)
    {
      GObject *object = *p;

      *p = NULL;
      g_object_unref (object);
    }
  return 0;
}

void
shell_lua_api_anchor (lua_State *L,
                      int        idx,
                      GObject   *object)
{
  GObject **p;

  idx = lua_absindex (L, idx);
  luaL_checkstack (L, 6, "shell_lua_api_anchor");

  /* registry[&anchors_key]: weak keys, so an anchor lives exactly as long as
   * the handle it is attached to (ephemeron semantics). */
  if (lua_rawgetp (L, LUA_REGISTRYINDEX, &anchors_key) != LUA_TTABLE)
    {
      lua_pop (L, 1);
      lua_newtable (L);
      lua_newtable (L);
      lua_pushliteral (L, "k");
      lua_setfield (L, -2, "__mode");
      lua_setmetatable (L, -2);
      lua_pushvalue (L, -1);
      lua_rawsetp (L, LUA_REGISTRYINDEX, &anchors_key);
    }

  p = lua_newuserdatauv (L, sizeof (GObject *), 0);
  *p = NULL;
  if (luaL_newmetatable (L, ANCHOR_MT))
    {
      lua_pushcfunction (L, anchor_gc);
      lua_setfield (L, -2, "__gc");
    }
  lua_setmetatable (L, -2);
  *p = g_object_ref (object);

  lua_pushvalue (L, idx);
  lua_insert (L, -2);
  lua_rawset (L, -3);
  lua_pop (L, 1);
}

/* ------------------------------------------------------------------------ */
/* Protected calls from C                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
  ShellLuaProtectedFunc fn;
  gpointer              data;
} Trampoline;

static int
trampoline (lua_State *L)
{
  Trampoline *t = lua_touserdata (L, 1);

  lua_settop (L, 0);
  return t->fn (L, t->data);
}

gboolean
shell_lua_api_call_protected (ShellLuaState        *state,
                              int                   nresults,
                              ShellLuaProtectedFunc fn,
                              gpointer              data)
{
  g_autoptr(GError) error = NULL;
  Trampoline t = { fn, data };
  lua_State *L;

  if (state == NULL || !shell_lua_state_is_alive (state))
    return FALSE;

  L = shell_lua_state_get_L (state);
  if (!lua_checkstack (L, 4))
    return FALSE;

  lua_pushcfunction (L, trampoline);
  lua_pushlightuserdata (L, &t);
  return shell_lua_state_pcall (state, 1, nresults, &error);
}

/* ------------------------------------------------------------------------ */
/* ShellLuaConnection                                                        */
/* ------------------------------------------------------------------------ */

struct _ShellLuaConnection
{
  GObject parent_instance;

  ShellLuaState   *state;          /* unowned; NULL once the state is gone */
  int              callback_ref;
  GDestroyNotify   disconnect_fn;
  gpointer         data;
  gboolean         connected;
  gboolean         disposing;
  ShellLuaOwnToken own;
};

G_DEFINE_FINAL_TYPE (ShellLuaConnection, shell_lua_connection, G_TYPE_OBJECT)

static void
state_gone (gpointer data,
            GObject *where_the_object_was)
{
  ShellLuaConnection *self = data;

  /* The Lua state is gone with its registry: nothing to unref, nothing to
   * release in the owner. */
  self->state = NULL;
  self->callback_ref = LUA_NOREF;
  self->own.state = NULL;
  shell_lua_connection_disconnect (self);
}

void
shell_lua_connection_disconnect (ShellLuaConnection *self)
{
  GDestroyNotify fn;
  gpointer data;

  g_return_if_fail (SHELL_IS_LUA_CONNECTION (self));

  if (!self->connected)
    return;
  self->connected = FALSE;

  /* Releasing the ownership may drop the last reference. */
  if (!self->disposing)
    g_object_ref (self);

  fn = self->disconnect_fn;
  data = self->data;
  self->disconnect_fn = NULL;
  self->data = NULL;
  if (fn != NULL)
    fn (data);

  if (self->callback_ref != LUA_NOREF)
    {
      int ref = self->callback_ref;

      self->callback_ref = LUA_NOREF;
      if (self->state != NULL && shell_lua_state_is_alive (self->state))
        shell_lua_state_unref (self->state, ref);
    }

  if (!self->disposing)
    {
      shell_lua_api_disown (&self->own);
      g_object_unref (self);
    }
}

gboolean
shell_lua_connection_is_connected (ShellLuaConnection *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_CONNECTION (self), FALSE);

  return self->connected;
}

int
shell_lua_connection_get_callback_ref (ShellLuaConnection *self)
{
  g_return_val_if_fail (SHELL_IS_LUA_CONNECTION (self), LUA_NOREF);

  return self->connected ? self->callback_ref : LUA_NOREF;
}

void
shell_lua_connection_own (ShellLuaConnection *self)
{
  g_return_if_fail (SHELL_IS_LUA_CONNECTION (self));

  if (self->state != NULL && self->connected && self->own.id == 0)
    shell_lua_api_own (self->state, G_OBJECT (self), &self->own);
}

static void
shell_lua_connection_dispose (GObject *object)
{
  ShellLuaConnection *self = SHELL_LUA_CONNECTION (object);

  self->disposing = TRUE;
  shell_lua_connection_disconnect (self);
  if (self->state != NULL)
    {
      g_object_weak_unref (G_OBJECT (self->state), state_gone, self);
      self->state = NULL;
    }

  G_OBJECT_CLASS (shell_lua_connection_parent_class)->dispose (object);
}

static void
shell_lua_connection_class_init (ShellLuaConnectionClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = shell_lua_connection_dispose;
}

static void
shell_lua_connection_init (ShellLuaConnection *self)
{
  self->callback_ref = LUA_NOREF;
}

ShellLuaConnection *
shell_lua_connection_new (ShellLuaState *state,
                          int            callback_ref,
                          GDestroyNotify disconnect_fn,
                          gpointer       data)
{
  ShellLuaConnection *self;

  g_return_val_if_fail (SHELL_IS_LUA_STATE (state), NULL);

  self = g_object_new (SHELL_TYPE_LUA_CONNECTION, NULL);
  self->state = state;
  self->callback_ref = callback_ref;
  self->disconnect_fn = disconnect_fn;
  self->data = data;
  self->connected = TRUE;
  g_object_weak_ref (G_OBJECT (state), state_gone, self);
  return self;
}

/* ------------------------------------------------------------------------ */
/* shell.Connection                                                          */
/* ------------------------------------------------------------------------ */

static int
l_connection_disconnect (lua_State *L)
{
  GObject *object = shell_lua_handle_check (L, 1, "shell.Connection");

  shell_lua_connection_disconnect (SHELL_LUA_CONNECTION (object));
  return 0;
}

static int
l_connection_is_connected (lua_State *L)
{
  GObject *object = shell_lua_handle_check (L, 1, "shell.Connection");

  lua_pushboolean (L, shell_lua_connection_is_connected (SHELL_LUA_CONNECTION (object)));
  return 1;
}

static const luaL_Reg connection_methods[] = {
  { "disconnect",   l_connection_disconnect },
  { "is_connected", l_connection_is_connected },
  { NULL, NULL }
};

static const ShellLuaHandleProperty connection_properties[] = {
  { NULL }
};

static ShellLuaHandleType connection_type = {
  .name = "shell.Connection",
  .methods = connection_methods,
  .properties = connection_properties,
};

void
shell_lua_connection_register_handle_type (lua_State *L)
{
  connection_type.gtype = SHELL_TYPE_LUA_CONNECTION;
  shell_lua_handle_register_type (L, &connection_type);
}

void
shell_lua_connection_push (ShellLuaConnection *self,
                           lua_State          *L)
{
  g_return_if_fail (SHELL_IS_LUA_CONNECTION (self));

  shell_lua_connection_register_handle_type (L);
  shell_lua_handle_push (L, "shell.Connection", G_OBJECT (self));
  shell_lua_api_anchor (L, -1, G_OBJECT (self));
}
