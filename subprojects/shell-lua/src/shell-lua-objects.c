/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The owned-object registry of a ShellLuaState (docs/api/README.md
 * "Ownership and teardown"), the callbacks bound to those objects, and the
 * two rules that govern callbacks:
 *
 *   rule 7: no callback is invoked because of an assignment made by Lua code
 *           (silence counter, see shell_lua_call_silenced);
 *   rule 8: destroying or reparenting the object that is emitting is deferred
 *           until its callback returns (emitting frames + deferred queue).
 *
 * The registry hangs off the ShellLuaState as qdata; it is created on first
 * use and destroyed through a state teardown (so before lua_close), and the
 * owning extension additionally blocks callbacks and destroys everything at
 * the start of disable (shell-lua-extension.c).
 */

#ifndef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ShellLua"
#endif

#include "shell-lua-api-private.h"

#define OBJECTS_QDATA "shell-lua-objects"

typedef struct _Objects Objects;
typedef struct _Entry Entry;

struct _ShellLuaCallback
{
  guint          ref_count;
  ShellLuaState *state;   /* unowned; NULL once dead */
  GObject       *owner;   /* unowned; NULL once dead */
  int            ref;     /* LUA_NOREF once dead */
};

struct _Entry
{
  Objects        *objects;
  GObject        *object;     /* strong */
  ShellLuaDestroyFunc destroy;
  gulong          destroyed_id;
  GSList         *callbacks;  /* ShellLuaCallback, one reference each */
  GList          *link;       /* in objects->order */
};

typedef struct
{
  GObject             *object;   /* strong; what the deferral is about */
  ShellLuaDeferredFunc func;
  gpointer             data;
  GDestroyNotify       notify;
} Deferred;

struct _Objects
{
  ShellLuaState *state;      /* unowned */
  GQueue         order;      /* Entry, creation order */
  GHashTable    *by_object;  /* GObject* -> Entry* */
  gboolean       blocked;
  int            silence;
  GPtrArray     *emitting;   /* GObject*, strong, innermost last */
  GQueue         deferred;   /* Deferred */
  gboolean       flushing;
  guint          teardown_id;
};

static ShellLuaContainsFunc contains_func = NULL;

/* ------------------------------------------------------------------------ */
/* Registry lookup                                                           */
/* ------------------------------------------------------------------------ */

static void
objects_free (gpointer data)
{
  Objects *o = data;
  Deferred *d;

  while ((d = g_queue_pop_head (&o->deferred)) != NULL)
    {
      if (d->notify != NULL)
        d->notify (d->data);
      g_clear_object (&d->object);
      g_free (d);
    }
  g_ptr_array_unref (o->emitting);
  g_hash_table_unref (o->by_object);
  g_free (o);
}

static void entry_finish (Entry *e, gboolean run_destroy);

static void
objects_teardown (gpointer data)
{
  Objects *o = data;

  o->teardown_id = 0;
  o->blocked = TRUE;
  shell_lua_objects_destroy_all (o->state);
}

static Objects *
objects_get (ShellLuaState *state,
             gboolean       create)
{
  Objects *o;

  if (state == NULL)
    return NULL;

  o = g_object_get_data (G_OBJECT (state), OBJECTS_QDATA);
  if (o != NULL || !create)
    return o;

  if (!shell_lua_state_is_alive (state))
    return NULL;

  o = g_new0 (Objects, 1);
  o->state = state;
  g_queue_init (&o->order);
  g_queue_init (&o->deferred);
  o->by_object = g_hash_table_new (g_direct_hash, g_direct_equal);
  o->emitting = g_ptr_array_new_with_free_func (g_object_unref);
  g_object_set_data_full (G_OBJECT (state), OBJECTS_QDATA, o, objects_free);
  o->teardown_id = shell_lua_state_add_teardown (state, objects_teardown, o);
  return o;
}

static Objects *
objects_from_L (lua_State *L,
                gboolean   create)
{
  return objects_get (L != NULL ? shell_lua_state_from_L (L) : NULL, create);
}

static lua_State *
objects_L (Objects *o)
{
  return shell_lua_state_is_alive (o->state) ? shell_lua_state_get_L (o->state) : NULL;
}

/* ------------------------------------------------------------------------ */
/* Extension, capabilities                                                   */
/* ------------------------------------------------------------------------ */

ShellLuaExtension *
shell_lua_extension_from_L (lua_State *L)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  gpointer owner = state != NULL ? shell_lua_state_get_owner (state) : NULL;

  return SHELL_IS_LUA_EXTENSION (owner) ? SHELL_LUA_EXTENSION (owner) : NULL;
}

void
shell_lua_require_capability (lua_State  *L,
                              const char *capability,
                              const char *function)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  ShellLuaExtension *extension = shell_lua_extension_from_L (L);

  if (extension != NULL)
    {
      if (shell_lua_extension_has_capability (extension, capability))
        return;
    }
  else if (state != NULL && shell_lua_state_get_kind (state) == SHELL_LUA_STATE_PRIVILEGED)
    {
      return;
    }

  luaL_error (L, "%s requires the \"%s\" capability", function, capability);
}

/* ------------------------------------------------------------------------ */
/* Callbacks                                                                 */
/* ------------------------------------------------------------------------ */

ShellLuaCallback *
shell_lua_callback_ref (ShellLuaCallback *cb)
{
  g_return_val_if_fail (cb != NULL, NULL);

  cb->ref_count++;
  return cb;
}

void
shell_lua_callback_kill (ShellLuaCallback *cb)
{
  int ref;

  g_return_if_fail (cb != NULL);

  ref = cb->ref;
  cb->ref = LUA_NOREF;
  cb->owner = NULL;
  if (ref != LUA_NOREF && cb->state != NULL && shell_lua_state_is_alive (cb->state))
    shell_lua_state_unref (cb->state, ref);
  cb->state = NULL;
}

void
shell_lua_callback_unref (ShellLuaCallback *cb)
{
  g_return_if_fail (cb != NULL);
  g_return_if_fail (cb->ref_count > 0);

  if (--cb->ref_count == 0)
    {
      shell_lua_callback_kill (cb);
      g_free (cb);
    }
}

gboolean
shell_lua_callback_is_live (ShellLuaCallback *cb)
{
  return cb != NULL && cb->ref != LUA_NOREF && cb->state != NULL;
}

ShellLuaCallback *
shell_lua_callback_new (lua_State *L,
                        GObject   *owner,
                        int        ref)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  Objects *o = objects_get (state, FALSE);
  Entry *e = o != NULL ? g_hash_table_lookup (o->by_object, owner) : NULL;
  ShellLuaCallback *cb;

  if (e == NULL)
    {
      if (ref != LUA_NOREF && ref != LUA_REFNIL)
        luaL_unref (L, LUA_REGISTRYINDEX, ref);
      luaL_error (L, "internal error: callback for an object that is not owned");
      return NULL;
    }

  cb = g_new0 (ShellLuaCallback, 1);
  cb->ref_count = 1;   /* the entry's */
  cb->state = state;
  cb->owner = owner;
  cb->ref = ref;
  e->callbacks = g_slist_prepend (e->callbacks, cb);
  return cb;
}

static void
callback_closure_notify (gpointer  data,
                         GClosure *closure)
{
  shell_lua_callback_unref (data);
}

gulong
shell_lua_callback_connect (ShellLuaCallback *cb,
                            gpointer          instance,
                            const char       *detailed_signal,
                            GCallback         c_handler)
{
  g_return_val_if_fail (cb != NULL, 0);
  g_return_val_if_fail (G_IS_OBJECT (instance), 0);

  return g_signal_connect_data (instance, detailed_signal, c_handler,
                                shell_lua_callback_ref (cb),
                                callback_closure_notify, 0);
}

/* ------------------------------------------------------------------------ */
/* Emitting frames, deferral                                                 */
/* ------------------------------------------------------------------------ */

void
shell_lua_set_contains_func (ShellLuaContainsFunc func)
{
  contains_func = func;
}

static gboolean
is_emitting_related (Objects *o,
                     GObject *object)
{
  guint i;

  for (i = 0; i < o->emitting->len; i++)
    {
      GObject *frame = g_ptr_array_index (o->emitting, i);

      if (frame == object)
        return TRUE;
      if (contains_func != NULL && contains_func (object, frame))
        return TRUE;
    }
  return FALSE;
}

static void
flush_deferred (Objects *o)
{
  gboolean progress;

  if (o->flushing)
    return;
  o->flushing = TRUE;

  do
    {
      GList *l;

      progress = FALSE;
      for (l = o->deferred.head; l != NULL; l = l->next)
        {
          Deferred *d = l->data;

          if (is_emitting_related (o, d->object))
            continue;

          g_queue_delete_link (&o->deferred, l);
          d->func (d->data);
          if (d->notify != NULL)
            d->notify (d->data);
          g_clear_object (&d->object);
          g_free (d);
          progress = TRUE;
          break;   /* the queue may have changed */
        }
    }
  while (progress);

  o->flushing = FALSE;
}

gboolean
shell_lua_defer_if_emitting (lua_State           *L,
                             GObject             *object,
                             ShellLuaDeferredFunc func,
                             gpointer             data,
                             GDestroyNotify       notify)
{
  Objects *o = objects_from_L (L, FALSE);
  Deferred *d;

  if (o == NULL || o->blocked || !is_emitting_related (o, object))
    return FALSE;

  d = g_new0 (Deferred, 1);
  d->object = g_object_ref (object);
  d->func = func;
  d->data = data;
  d->notify = notify;
  g_queue_push_tail (&o->deferred, d);
  return TRUE;
}

typedef struct
{
  GObject              *emitter;
  gboolean              with_self;
  ShellLuaPushArgsFunc  push_args;
  gpointer              user_data;
} EmitContext;

static void
emit_push (lua_State *L,
           gpointer   data)
{
  EmitContext *ctx = data;

  if (ctx->with_self)
    shell_lua_handle_push (L, NULL, ctx->emitter);
  if (ctx->push_args != NULL)
    ctx->push_args (L, ctx->user_data);
}

static gboolean
emit_with (ShellLuaState        *state,
           GObject              *emitter,
           gboolean              with_self,
           int                   ref,
           int                   nargs,
           ShellLuaPushArgsFunc  push_args,
           gpointer              user_data)
{
  Objects *o;
  EmitContext ctx = { emitter, with_self && emitter != NULL, push_args, user_data };
  gboolean ok;

  if (state == NULL || ref == LUA_NOREF || ref == LUA_REFNIL || !shell_lua_state_is_alive (state))
    return FALSE;

  o = objects_get (state, TRUE);
  if (o == NULL || o->blocked)
    return FALSE;
  if (with_self && o->silence > 0)
    return FALSE;

  if (emitter != NULL)
    g_ptr_array_add (o->emitting, g_object_ref (emitter));

  ok = shell_lua_call_ref (state, ref, nargs + (ctx.with_self ? 1 : 0), emit_push, &ctx);

  if (emitter != NULL)
    {
      guint i;

      /* Innermost last; remove the last occurrence of @emitter. */
      for (i = o->emitting->len; i > 0; i--)
        if (g_ptr_array_index (o->emitting, i - 1) == emitter)
          {
            g_ptr_array_remove_index (o->emitting, i - 1);
            break;
          }
      flush_deferred (o);
    }

  return ok;
}

gboolean
shell_lua_emit_ref (ShellLuaState        *state,
                    GObject              *emitter,
                    int                   ref,
                    int                   nargs,
                    ShellLuaPushArgsFunc  push_args,
                    gpointer              user_data)
{
  return emit_with (state, emitter, TRUE, ref, nargs, push_args, user_data);
}

gboolean
shell_lua_callback_emit (ShellLuaCallback    *cb,
                         int                  nargs,
                         ShellLuaPushArgsFunc push_args,
                         gpointer             user_data)
{
  gboolean ok;

  if (!shell_lua_callback_is_live (cb) || cb->owner == NULL)
    return FALSE;

  /* The callback may kill itself or its owner; keep both until it returns. */
  shell_lua_callback_ref (cb);
  g_object_ref (cb->owner);
  {
    GObject *owner = cb->owner;

    ok = emit_with (cb->state, owner, TRUE, cb->ref, nargs, push_args, user_data);
    g_object_unref (owner);
  }
  shell_lua_callback_unref (cb);
  return ok;
}

gboolean
shell_lua_callback_emit_data (ShellLuaCallback    *cb,
                              int                  nargs,
                              ShellLuaPushArgsFunc push_args,
                              gpointer             user_data)
{
  gboolean ok;

  if (!shell_lua_callback_is_live (cb))
    return FALSE;

  shell_lua_callback_ref (cb);
  ok = emit_with (cb->state, cb->owner, FALSE, cb->ref, nargs, push_args, user_data);
  shell_lua_callback_unref (cb);
  return ok;
}

/* ------------------------------------------------------------------------ */
/* Connections returned by h:on_<event>(fn)                                  */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellLuaCallback *cb;
  gpointer          instance;   /* weak */
  gulong            handler_id;
} ConnData;

static void
conn_disconnect (gpointer data)
{
  ConnData *c = data;

  if (c->instance != NULL)
    {
      g_signal_handler_disconnect (c->instance, c->handler_id);
      g_clear_weak_pointer (&c->instance);
    }
  shell_lua_callback_kill (c->cb);
  shell_lua_callback_unref (c->cb);
  g_free (c);
}

#include "shell-lua-connection-private.h"

int
shell_lua_callback_push_connection (lua_State        *L,
                                    ShellLuaCallback *cb,
                                    gpointer          instance,
                                    gulong            handler_id)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  ConnData *c = g_new0 (ConnData, 1);
  ShellLuaConnection *connection;

  c->cb = shell_lua_callback_ref (cb);
  c->handler_id = handler_id;
  g_set_weak_pointer (&c->instance, instance);

  connection = shell_lua_connection_new (state, LUA_NOREF, conn_disconnect, c);
  shell_lua_connection_own (connection);
  shell_lua_connection_register_handle_type (L);
  shell_lua_connection_push (connection, L);
  g_object_unref (connection);
  return 1;
}

/* ------------------------------------------------------------------------ */
/* Silence (rule 7)                                                          */
/* ------------------------------------------------------------------------ */

void
shell_lua_silence_enter (lua_State *L)
{
  Objects *o = objects_from_L (L, TRUE);

  if (o != NULL)
    o->silence++;
}

void
shell_lua_silence_leave (lua_State *L)
{
  Objects *o = objects_from_L (L, FALSE);

  if (o != NULL && o->silence > 0)
    o->silence--;
}

typedef struct
{
  ShellLuaSilencedFunc fn;
  gpointer             data;
} SilencedCall;

static int
silenced_trampoline (lua_State *L)
{
  SilencedCall *c = lua_touserdata (L, 1);

  lua_remove (L, 1);   /* the function sees only its own arguments */
  c->fn (L, c->data);
  return 0;
}

gboolean
shell_lua_try_silenced_args (lua_State           *L,
                             int                  nargs,
                             ShellLuaSilencedFunc fn,
                             gpointer             user_data)
{
  SilencedCall call = { fn, user_data };
  int status;

  luaL_checkstack (L, 3, "silenced call");
  lua_pushcfunction (L, silenced_trampoline);
  lua_insert (L, -(nargs + 1));
  lua_pushlightuserdata (L, &call);
  lua_insert (L, -(nargs + 1));

  shell_lua_silence_enter (L);
  status = lua_pcall (L, nargs + 1, 0, 0);
  shell_lua_silence_leave (L);

  return status == LUA_OK;
}

gboolean
shell_lua_try_silenced (lua_State           *L,
                        ShellLuaSilencedFunc fn,
                        gpointer             user_data)
{
  return shell_lua_try_silenced_args (L, 0, fn, user_data);
}

void
shell_lua_call_silenced (lua_State           *L,
                         ShellLuaSilencedFunc fn,
                         gpointer             user_data)
{
  if (!shell_lua_try_silenced (L, fn, user_data))
    lua_error (L);
}

/* ------------------------------------------------------------------------ */
/* Owned objects                                                             */
/* ------------------------------------------------------------------------ */

static void
entry_unlink (Entry *e)
{
  Objects *o = e->objects;

  if (e->link == NULL)
    return;

  g_queue_delete_link (&o->order, e->link);
  e->link = NULL;
  g_hash_table_remove (o->by_object, e->object);
  if (e->destroyed_id != 0)
    {
      g_signal_handler_disconnect (e->object, e->destroyed_id);
      e->destroyed_id = 0;
    }
}

/* Takes @e out of the registry, kills its callbacks, optionally destroys the
 * object, kills its handles and drops the reference. */
static void
entry_finish (Entry *e,
              gboolean run_destroy)
{
  Objects *o = e->objects;
  GObject *object = e->object;
  GSList *callbacks = e->callbacks;
  lua_State *L = objects_L (o);
  GSList *l;

  e->callbacks = NULL;
  entry_unlink (e);

  for (l = callbacks; l != NULL; l = l->next)
    {
      shell_lua_callback_kill (l->data);
      shell_lua_callback_unref (l->data);
    }
  g_slist_free (callbacks);

  if (run_destroy)
    {
      if (e->destroy != NULL)
        e->destroy (object);
      else
        g_object_run_dispose (object);
    }

  if (L != NULL)
    shell_lua_handle_invalidate (L, object);

  g_object_unref (object);
  g_free (e);
}

/* The handler of ShellLuaOwnOps.destroyed_signal. It is a closure with a
 * marshaller of its own so that it does not care what the signal carries
 * (ShellNotification::destroy has a reason, ClutterActor::destroy nothing):
 * only the instance, the first parameter, is read. */
static void
destroyed_marshal (GClosure     *closure,
                   GValue       *return_value,
                   guint         n_param_values,
                   const GValue *param_values,
                   gpointer      invocation_hint,
                   gpointer      marshal_data)
{
  Objects *o = closure->data;
  GObject *object;
  Entry *e;

  if (n_param_values < 1 || !G_VALUE_HOLDS_OBJECT (&param_values[0]))
    return;

  object = g_value_get_object (&param_values[0]);
  e = g_hash_table_lookup (o->by_object, object);
  if (e != NULL)
    entry_finish (e, FALSE);
}

static gulong
connect_destroyed (Objects    *o,
                   GObject    *object,
                   const char *signal)
{
  GClosure *closure = g_closure_new_simple (sizeof (GClosure), o);

  g_closure_set_marshal (closure, destroyed_marshal);
  return g_signal_connect_closure (object, signal, closure, FALSE);
}

void
shell_lua_check_owned_capacity (lua_State *L)
{
  Objects *o = objects_from_L (L, FALSE);

  if (o != NULL && g_queue_get_length (&o->order) >= SHELL_LUA_MAX_OWNED_OBJECTS)
    luaL_error (L, "too many objects (limit %d)", SHELL_LUA_MAX_OWNED_OBJECTS);
}

void
shell_lua_own_object (lua_State            *L,
                      GObject              *object,
                      const ShellLuaOwnOps *ops)
{
  Objects *o = objects_from_L (L, TRUE);
  Entry *e;

  g_return_if_fail (G_IS_OBJECT (object));

  if (o == NULL)
    {
      luaL_error (L, "cannot own an object: no live state");
      return;
    }
  if (g_hash_table_lookup (o->by_object, object) != NULL)
    return;

  if (g_queue_get_length (&o->order) >= SHELL_LUA_MAX_OWNED_OBJECTS)
    {
      /* Callers check the capacity before creating (shell_lua_check_owned_
       * capacity); this is the backstop. The object is not kept. */
      if (ops != NULL && ops->destroy != NULL)
        ops->destroy (object);
      else
        g_object_run_dispose (object);
      if (g_object_is_floating (object))
        g_object_unref (g_object_ref_sink (object));
      luaL_error (L, "too many objects (limit %d)", SHELL_LUA_MAX_OWNED_OBJECTS);
      return;
    }

  /* The context's reference: the floating reference of a new actor is
   * sunk into it (nothing for the caller to release); any other object gets
   * a reference of its own. */
  g_object_ref_sink (object);

  e = g_new0 (Entry, 1);
  e->objects = o;
  e->object = object;
  e->destroy = ops != NULL ? ops->destroy : NULL;
  g_queue_push_tail (&o->order, e);
  e->link = o->order.tail;
  g_hash_table_insert (o->by_object, object, e);

  if (ops != NULL && ops->destroyed_signal != NULL)
    e->destroyed_id = connect_destroyed (o, object, ops->destroyed_signal);
}

gboolean
shell_lua_object_is_owned (lua_State *L,
                           GObject   *object)
{
  Objects *o = objects_from_L (L, FALSE);

  return o != NULL && g_hash_table_lookup (o->by_object, object) != NULL;
}

guint
shell_lua_n_owned_objects (lua_State *L)
{
  Objects *o = objects_from_L (L, FALSE);

  return o != NULL ? g_queue_get_length (&o->order) : 0;
}

typedef struct
{
  Objects *objects;
  GObject *object;   /* the Deferred holds the reference */
} DestroyLater;

static void
deferred_destroy (gpointer data)
{
  DestroyLater *later = data;
  Entry *e = g_hash_table_lookup (later->objects->by_object, later->object);

  if (e != NULL)
    entry_finish (e, TRUE);
}

gboolean
shell_lua_destroy_object (lua_State *L,
                          GObject   *object)
{
  Objects *o = objects_from_L (L, FALSE);
  Entry *e;

  if (o == NULL)
    return FALSE;
  e = g_hash_table_lookup (o->by_object, object);
  if (e == NULL)
    return FALSE;

  if (!o->blocked && is_emitting_related (o, object))
    {
      DestroyLater *later = g_new0 (DestroyLater, 1);

      later->objects = o;
      later->object = object;
      shell_lua_defer_if_emitting (L, object, deferred_destroy, later, g_free);
      return TRUE;
    }

  entry_finish (e, TRUE);
  return TRUE;
}

int
shell_lua_handle_destroy_method (lua_State *L)
{
  GObject *object = shell_lua_handle_get (L, 1, NULL);

  if (object != NULL && !shell_lua_destroy_object (L, object))
    return luaL_error (L, "a %s cannot be destroyed", G_OBJECT_TYPE_NAME (object));
  return 0;
}

/* ------------------------------------------------------------------------ */
/* Teardown                                                                  */
/* ------------------------------------------------------------------------ */

void
shell_lua_objects_block (ShellLuaState *state)
{
  Objects *o = objects_get (state, TRUE);

  if (o != NULL)
    o->blocked = TRUE;
}

gboolean
shell_lua_objects_is_blocked (ShellLuaState *state)
{
  Objects *o = objects_get (state, FALSE);

  return o != NULL && o->blocked;
}

void
shell_lua_objects_destroy_all (ShellLuaState *state)
{
  Objects *o = objects_get (state, FALSE);
  Entry *e;

  if (o == NULL)
    return;

  while ((e = g_queue_peek_tail (&o->order)) != NULL)
    entry_finish (e, TRUE);

  /* Nothing is deferred any more: the objects are gone. */
  while (o->deferred.length > 0)
    {
      Deferred *d = g_queue_pop_head (&o->deferred);

      if (d->notify != NULL)
        d->notify (d->data);
      g_clear_object (&d->object);
      g_free (d);
    }
}
