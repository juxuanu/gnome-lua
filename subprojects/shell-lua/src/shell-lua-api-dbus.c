/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.dbus (docs/api/dbus.md): proxies, method calls, properties, signals,
 * name watches, owned names and one exported object per call of `export`,
 * all asynchronous and all gated by the "dbus" capability.
 *
 * Notes for maintainers:
 *
 *  - Values are converted by shell-lua-variant.c ("with a target type" going
 *    out, GVariant -> Lua coming in); this file only checks that a signature
 *    is something the bus can carry (no maybe types, no file descriptors).
 *  - A shell.Bus asks for its connection with g_bus_get() when it is made and
 *    keeps whatever needs the connection (calls, subscriptions, the
 *    registration of exported objects) in a FIFO until it arrives, so nothing
 *    here ever waits for the bus. Name watches and name ownership use the
 *    asynchronous g_bus_* helpers, which connect on their own.
 *  - Long lived objects (the Bus, proxies, owned names, exported objects) are
 *    owned objects of the runtime (shell_lua_own_object): destroyed in
 *    reverse creation order at disable, with a destroy() of their own. Calls
 *    are owned like timers (shell_lua_api_own, early release at completion),
 *    so that a handle to a finished call stays harmless instead of raising
 *    and the finished calls do not count against the object cap. Signal
 *    subscriptions and name watches are shell.Connection objects.
 *  - Nothing here calls Lua except through shell_lua_call_ref() and
 *    shell_lua_api_call_protected(), i.e. under the watchdog and the error
 *    policy and never after disable began. The method handlers of an
 *    exported object run synchronously and convert their results inside the
 *    protected call, so that a wrong return is a counted extension error.
 */

#ifndef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ShellLua"
#endif

#include <string.h>

#include <gio/gio.h>

#include "shell-lua-api-private.h"
#include "shell-lua-connection-private.h"

#define ERR_NO_REPLY     "org.freedesktop.DBus.Error.NoReply"
#define ERR_DISCONNECTED "org.freedesktop.DBus.Error.Disconnected"
#define ERR_FAILED       "org.freedesktop.DBus.Error.Failed"
#define ERR_UNKNOWN_METHOD "org.freedesktop.DBus.Error.UnknownMethod"

#define DEFAULT_TIMEOUT_MS 5000
#define MAX_TIMEOUT_MS 60000

static void __attribute__ ((constructor))
shell_lua_api_dbus_init (void)
{
  shell_lua_api_register ("dbus", shell_lua_api_dbus_register);
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

G_GNUC_PRINTF (3, 4)
static void
ext_log (ShellLuaState  *state,
         GLogLevelFlags  level,
         const char     *format,
         ...)
{
  g_autofree char *domain = NULL;
  g_autofree char *message = NULL;
  va_list args;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  domain = g_strdup_printf ("shell-lua/%s",
                            state != NULL ? shell_lua_state_get_id (state) : "dbus");
  g_log (domain, level, "%s", message);
}

static gboolean
state_usable (ShellLuaState *state)
{
  return state != NULL && shell_lua_state_is_alive (state);
}

static void
release_ref (ShellLuaState *state,
             int           *ref)
{
  int r = *ref;

  *ref = LUA_NOREF;
  if (r != LUA_NOREF && r != LUA_REFNIL && state_usable (state))
    shell_lua_state_unref (state, r);
}

/* Names the shell keeps for itself, and the namespaces of system services. */
static gboolean
is_shell_name (const char *name)
{
  return g_str_equal (name, "org.gnome.Shell") || g_str_has_prefix (name, "org.gnome.Shell.");
}

static gboolean
is_system_name (const char *name)
{
  return g_str_has_prefix (name, "org.gnome.") || g_str_has_prefix (name, "org.freedesktop.");
}

/* @sig is a (possibly empty) D-Bus signature the extension supplied. */
static void
check_signature (lua_State  *L,
                 const char *sig,
                 const char *what)
{
  if (!g_variant_is_signature (sig))
    luaL_error (L, "%s '%s' is not a valid D-Bus signature (maybe types are not valid on the bus)",
                what, sig);
  if (strchr (sig, 'h') != NULL)
    luaL_error (L, "%s '%s' contains 'h': file descriptors are not supported", what, sig);
}

static void
check_single_type (lua_State  *L,
                   const char *sig,
                   const char *what)
{
  check_signature (L, sig, what);
  if (!g_variant_type_string_is_valid (sig))
    luaL_error (L, "%s '%s' must be exactly one complete D-Bus type", what, sig);
}

static void
check_options (lua_State *L,
               int        idx)
{
  if (!lua_istable (L, idx))
    luaL_error (L, "options table expected, got %s", luaL_typename (L, idx));
}

/* Raise unless the field is absent or callable (shell_lua_opt_callback then
 * cannot raise). */
static void
check_callback_field (lua_State  *L,
                      int         tbl,
                      const char *field)
{
  int type = lua_getfield (L, tbl, field);

  if (type != LUA_TNIL && type != LUA_TFUNCTION &&
      luaL_getmetafield (L, -1, "__call") == LUA_TNIL)
    luaL_error (L, "field '%s' must be a function, got %s", field, luaL_typename (L, -1));
  else if (type != LUA_TNIL && type != LUA_TFUNCTION)
    lua_pop (L, 1);
  lua_pop (L, 1);
}

static char *
required_string (lua_State  *L,
                 int         tbl,
                 const char *field)
{
  const char *value = shell_lua_opt_string (L, tbl, field, NULL);

  if (value == NULL)
    luaL_error (L, "missing required field '%s' (a string)", field);
  return (char *) value;
}

/* ------------------------------------------------------------------------ */
/* Errors                                                                    */
/* ------------------------------------------------------------------------ */

/* The error table of on_error(err): name, message, remote. */
typedef struct {
  char     *name;
  char     *message;
  gboolean  remote;
} WireError;

static void
wire_error_clear (WireError *e)
{
  g_clear_pointer (&e->name, g_free);
  g_clear_pointer (&e->message, g_free);
}

static void
wire_error_from_gerror (WireError    *e,
                        const GError *error)
{
  g_autofree char *remote_name = NULL;
  g_autoptr (GError) copy = g_error_copy (error);

  e->remote = g_dbus_error_is_remote_error (copy);
  if (e->remote)
    {
      remote_name = g_dbus_error_get_remote_error (copy);
      g_dbus_error_strip_remote_error (copy);
      e->name = g_steal_pointer (&remote_name);
    }
  else if (g_error_matches (copy, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
    e->name = g_strdup (ERR_NO_REPLY);
  else if (g_error_matches (copy, G_IO_ERROR, G_IO_ERROR_CLOSED))
    e->name = g_strdup (ERR_DISCONNECTED);
  else if (copy->domain == G_DBUS_ERROR)
    {
      e->name = g_dbus_error_encode_gerror (copy);
      /* encode_gerror prefixes unmapped codes with "org.gtk.GDBus"; the
       * mapped ones are the standard names. */
      if (e->name != NULL && g_str_has_prefix (e->name, "org.gtk.GDBus."))
        g_clear_pointer (&e->name, g_free);
    }
  if (e->name == NULL)
    e->name = g_strdup (ERR_FAILED);
  e->message = g_strdup (copy->message != NULL ? copy->message : "");
}

static void
wire_error_local (WireError  *e,
                  const char *name,
                  const char *message)
{
  e->remote = FALSE;
  e->name = g_strdup (name);
  e->message = g_strdup (message);
}

static void
push_wire_error (lua_State *L,
                 gpointer   data)
{
  WireError *e = data;

  lua_createtable (L, 0, 3);
  lua_pushstring (L, e->name);
  lua_setfield (L, -2, "name");
  lua_pushstring (L, e->message);
  lua_setfield (L, -2, "message");
  lua_pushboolean (L, e->remote);
  lua_setfield (L, -2, "remote");
}

/* Spreads the children of the tuple @data into arguments. The whole tuple is
 * converted at once and unpacked, so that no reference to a child is held
 * while a conversion that can raise runs. */
static void
push_tuple_children (lua_State *L,
                     gpointer   data)
{
  GVariant *tuple = data;
  gsize i, n = g_variant_n_children (tuple);

  luaL_checkstack (L, (int) MIN (n, (gsize) 10000) + 3, "too many values in a message");
  shell_lua_push_variant (L, tuple);
  for (i = 1; i <= n; i++)
    lua_rawgeti (L, -(int) i, (lua_Integer) i);
  lua_remove (L, -(int) n - 1);
}

/* ------------------------------------------------------------------------ */
/* ShellLuaBus                                                               */
/* ------------------------------------------------------------------------ */

typedef struct _ShellLuaBus ShellLuaBus;

typedef void (* BusOpFunc) (ShellLuaBus     *bus,
                            GDBusConnection *connection,   /* NULL: no connection */
                            gpointer         data);

typedef struct {
  BusOpFunc      fn;
  gpointer       data;
  GDestroyNotify free;
} BusOp;

#define SHELL_TYPE_LUA_BUS (shell_lua_bus_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaBus, shell_lua_bus, SHELL, LUA_BUS, GObject)

struct _ShellLuaBus
{
  GObject parent_instance;

  ShellLuaState   *state;       /* unowned; NULL once destroyed */
  GBusType         bus_type;
  GDBusConnection *connection;
  GCancellable    *cancellable;
  GQueue           ops;         /* BusOp, until the connection arrives */
  gboolean         failed;
};

G_DEFINE_FINAL_TYPE (ShellLuaBus, shell_lua_bus, G_TYPE_OBJECT)

static void
bus_op_free (BusOp *op)
{
  if (op->free != NULL)
    op->free (op->data);
  g_free (op);
}

static void
bus_op_run (ShellLuaBus *bus,
            BusOp       *op,
            gboolean     with_connection)
{
  op->fn (bus, with_connection ? bus->connection : NULL, op->data);
  bus_op_free (op);
}

typedef struct {
  ShellLuaBus *bus;
  BusOp       *op;
} LateOp;

static gboolean
late_op_cb (gpointer data)
{
  LateOp *late = data;

  if (late->bus->state != NULL)
    bus_op_run (late->bus, late->op, FALSE);
  else
    bus_op_free (late->op);
  g_object_unref (late->bus);
  g_free (late);
  return G_SOURCE_REMOVE;
}

/* Run @fn(bus, connection, data) once the connection is there: at once when
 * it already is, else FIFO after it arrives. If the bus cannot be reached @fn
 * gets a NULL connection (always from the main loop, never from inside the
 * call that asked). @free_data is called with @data after @fn ran, or when
 * the bus is destroyed first. */
static void
bus_when_ready (ShellLuaBus   *bus,
                BusOpFunc      fn,
                gpointer       data,
                GDestroyNotify free_data)
{
  BusOp *op = g_new0 (BusOp, 1);

  op->fn = fn;
  op->data = data;
  op->free = free_data;

  if (bus->state == NULL)
    {
      bus_op_free (op);
    }
  else if (bus->connection != NULL)
    {
      bus_op_run (bus, op, TRUE);
    }
  else if (bus->failed)
    {
      LateOp *late = g_new0 (LateOp, 1);

      late->bus = g_object_ref (bus);
      late->op = op;
      g_idle_add (late_op_cb, late);
    }
  else
    {
      g_queue_push_tail (&bus->ops, op);
    }
}

static void
bus_got_connection (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  ShellLuaBus *bus = user_data;
  g_autoptr (GError) error = NULL;
  GDBusConnection *connection = g_bus_get_finish (result, &error);
  BusOp *op;

  if (bus->state == NULL)
    {
      g_clear_object (&connection);
      g_object_unref (bus);
      return;
    }

  if (connection == NULL)
    {
      bus->failed = TRUE;
      ext_log (bus->state, G_LOG_LEVEL_MESSAGE, "cannot connect to the %s bus: %s",
               bus->bus_type == G_BUS_TYPE_SYSTEM ? "system" : "session",
               error->message);
    }
  else
    {
      bus->connection = connection;
    }

  /* An op may destroy the bus (through a Lua callback running synchronously
   * is impossible here, but be safe) */
  while ((op = g_queue_pop_head (&bus->ops)) != NULL)
    {
      if (bus->state == NULL)
        bus_op_free (op);
      else
        bus_op_run (bus, op, bus->connection != NULL);
    }

  g_object_unref (bus);
}

static void
bus_teardown (ShellLuaBus *bus)
{
  BusOp *op;

  bus->state = NULL;
  if (bus->cancellable != NULL)
    g_cancellable_cancel (bus->cancellable);
  while ((op = g_queue_pop_head (&bus->ops)) != NULL)
    bus_op_free (op);
  g_clear_object (&bus->connection);
}

static void
bus_destroy_op (GObject *object)
{
  bus_teardown (SHELL_LUA_BUS (object));
}

static void
shell_lua_bus_dispose (GObject *object)
{
  ShellLuaBus *bus = SHELL_LUA_BUS (object);

  bus_teardown (bus);
  g_clear_object (&bus->cancellable);

  G_OBJECT_CLASS (shell_lua_bus_parent_class)->dispose (object);
}

static void
shell_lua_bus_class_init (ShellLuaBusClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = shell_lua_bus_dispose;
}

static void
shell_lua_bus_init (ShellLuaBus *bus)
{
  g_queue_init (&bus->ops);
}

static const ShellLuaOwnOps bus_ops = { .destroy = bus_destroy_op };

/* ------------------------------------------------------------------------ */
/* ShellLuaCall                                                              */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LUA_CALL (shell_lua_call_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaCall, shell_lua_call, SHELL, LUA_CALL, GObject)

struct _ShellLuaCall
{
  GObject parent_instance;

  ShellLuaState   *state;        /* unowned; NULL once the state is gone */
  GCancellable    *cancellable;
  int              reply_ref;    /* on_reply or on_done */
  int              error_ref;
  gboolean         done;
  gboolean         disposing;
  ShellLuaOwnToken own;
};

G_DEFINE_FINAL_TYPE (ShellLuaCall, shell_lua_call, G_TYPE_OBJECT)

enum { CALL_PROP_0, CALL_PROP_PENDING, CALL_N_PROPS };
static GParamSpec *call_props[CALL_N_PROPS];

static void
call_release_refs (ShellLuaCall *call)
{
  release_ref (call->state, &call->reply_ref);
  release_ref (call->state, &call->error_ref);
}

/* The call is over (completed, failed or cancelled): drop what it holds and
 * hand it back to its owner. The handle stays valid. */
static void
call_finish (ShellLuaCall *call)
{
  if (call->done)
    return;

  g_object_ref (call);
  call->done = TRUE;
  call_release_refs (call);
  if (!call->disposing)
    {
      g_object_notify_by_pspec (G_OBJECT (call), call_props[CALL_PROP_PENDING]);
      shell_lua_api_disown (&call->own);
    }
  g_object_unref (call);
}

static void
call_cancel (ShellLuaCall *call)
{
  if (call->done)
    return;

  if (call->cancellable != NULL)
    g_cancellable_cancel (call->cancellable);
  call_finish (call);
}

static void
call_state_gone (gpointer data,
                 GObject *where_the_object_was)
{
  ShellLuaCall *call = data;

  call->state = NULL;
  call->reply_ref = LUA_NOREF;
  call->error_ref = LUA_NOREF;
  call->own.state = NULL;
  call_cancel (call);
}

static void
shell_lua_call_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  ShellLuaCall *call = SHELL_LUA_CALL (object);

  switch (prop_id)
    {
    case CALL_PROP_PENDING:
      g_value_set_boolean (value, !call->done);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_lua_call_dispose (GObject *object)
{
  ShellLuaCall *call = SHELL_LUA_CALL (object);

  call->disposing = TRUE;
  if (call->cancellable != NULL)
    g_cancellable_cancel (call->cancellable);
  call->done = TRUE;
  call_release_refs (call);
  if (call->state != NULL)
    {
      g_object_weak_unref (G_OBJECT (call->state), call_state_gone, call);
      call->state = NULL;
    }
  g_clear_object (&call->cancellable);

  G_OBJECT_CLASS (shell_lua_call_parent_class)->dispose (object);
}

static void
shell_lua_call_class_init (ShellLuaCallClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_lua_call_get_property;
  object_class->dispose = shell_lua_call_dispose;

  call_props[CALL_PROP_PENDING] =
    g_param_spec_boolean ("pending", NULL, NULL, FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties (object_class, CALL_N_PROPS, call_props);
}

static void
shell_lua_call_init (ShellLuaCall *call)
{
  call->reply_ref = LUA_NOREF;
  call->error_ref = LUA_NOREF;
  call->cancellable = g_cancellable_new ();
}

/* Takes the callback out of @call, which is over from now on (pending is
 * already FALSE while the callback runs, and the call no longer counts
 * against the objects of the extension). */
static int
call_take_ref (ShellLuaCall *call,
               int          *ref)
{
  int taken = *ref;

  *ref = LUA_NOREF;
  call_finish (call);
  return taken;
}

/* Report a failure of @call to on_error (or the log) and finish it. */
static void
call_fail (ShellLuaCall *call,
           WireError    *error,
           const char   *what)
{
  ShellLuaState *state = call->state;
  gboolean was_done = call->done;
  int ref;

  g_object_ref (call);
  ref = call_take_ref (call, &call->error_ref);
  if (!was_done && state_usable (state))
    {
      if (ref != LUA_NOREF)
        shell_lua_call_ref (state, ref, 1, push_wire_error, error);
      else
        ext_log (state, G_LOG_LEVEL_WARNING, "%s failed: %s: %s", what, error->name, error->message);
    }
  release_ref (state, &ref);
  g_object_unref (call);
}

static void
call_succeed (ShellLuaCall *call,
              GVariant     *reply)
{
  ShellLuaState *state = call->state;
  gboolean was_done = call->done;
  int ref;

  g_object_ref (call);
  ref = call_take_ref (call, &call->reply_ref);
  if (!was_done && state_usable (state) && ref != LUA_NOREF)
    shell_lua_call_ref (state, ref, (int) g_variant_n_children (reply), push_tuple_children, reply);
  release_ref (state, &ref);
  g_object_unref (call);
}

typedef struct {
  ShellLuaCall   *call;      /* strong */
  char           *what;      /* for the log: "Interface.Method" */
  char           *dest;
  char           *path;
  char           *iface;
  char           *method;
  GVariant       *params;    /* sunk, may be NULL */
  int             timeout_ms;
  gboolean        no_reply;
} CallOp;

static void
call_op_free (gpointer data)
{
  CallOp *op = data;

  g_clear_object (&op->call);
  g_free (op->what);
  g_free (op->dest);
  g_free (op->path);
  g_free (op->iface);
  g_free (op->method);
  g_clear_pointer (&op->params, g_variant_unref);
  g_free (op);
}

static void
call_reply_cb (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  ShellLuaCall *call = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source),
                                                              result, &error);

  if (!call->done)
    {
      if (reply != NULL)
        {
          call_succeed (call, reply);
        }
      else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          WireError e = { 0 };
          g_autofree char *what = g_strdup (g_object_get_data (G_OBJECT (call), "what"));

          wire_error_from_gerror (&e, error);
          call_fail (call, &e, what != NULL ? what : "D-Bus call");
          wire_error_clear (&e);
        }
    }
  g_object_unref (call);
}

static void
call_op_run (ShellLuaBus     *bus,
             GDBusConnection *connection,
             gpointer         data)
{
  CallOp *op = data;
  ShellLuaCall *call = op->call;

  if (call->done)
    return;

  if (connection == NULL)
    {
      WireError e = { 0 };

      wire_error_local (&e, ERR_DISCONNECTED, "cannot connect to the bus");
      call_fail (call, &e, op->what);
      wire_error_clear (&e);
      return;
    }

  if (op->no_reply)
    {
      g_autoptr (GDBusMessage) message =
        g_dbus_message_new_method_call (op->dest, op->path, op->iface, op->method);
      g_autoptr (GError) error = NULL;

      g_dbus_message_set_flags (message, G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
      if (op->params != NULL)
        g_dbus_message_set_body (message, op->params);
      if (!g_dbus_connection_send_message (connection, message,
                                           G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, &error))
        ext_log (call->state, G_LOG_LEVEL_WARNING, "%s failed: %s", op->what, error->message);
      call_finish (call);
      return;
    }

  g_object_set_data_full (G_OBJECT (call), "what", g_strdup (op->what), g_free);
  g_dbus_connection_call (connection, op->dest, op->path, op->iface, op->method,
                          op->params, NULL, G_DBUS_CALL_FLAGS_NONE,
                          op->timeout_ms, call->cancellable, call_reply_cb,
                          g_object_ref (call));
}

/* Starts a call on @bus. Takes over the references of @on_reply_ref and
 * @on_error_ref (either may be LUA_NOREF); with neither, the call is
 * fire-and-forget. @params is consumed. Pushes the shell.DBusCall handle. */
static void
start_call (lua_State  *L,
            ShellLuaBus *bus,
            const char *dest,
            const char *path,
            const char *iface,
            const char *method,
            GVariant   *params,
            int         timeout_ms,
            int         on_reply_ref,
            int         on_error_ref)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  ShellLuaCall *call = g_object_new (SHELL_TYPE_LUA_CALL, NULL);
  CallOp *op = g_new0 (CallOp, 1);

  call->state = state;
  call->reply_ref = on_reply_ref;
  call->error_ref = on_error_ref;
  g_object_weak_ref (G_OBJECT (state), call_state_gone, call);

  op->call = g_object_ref (call);
  op->what = g_strdup_printf ("%s.%s", iface, method);
  op->dest = g_strdup (dest);
  op->path = g_strdup (path);
  op->iface = g_strdup (iface);
  op->method = g_strdup (method);
  op->params = params != NULL ? g_variant_ref_sink (params) : NULL;
  op->timeout_ms = timeout_ms;
  op->no_reply = (on_reply_ref == LUA_NOREF && on_error_ref == LUA_NOREF);

  if (!op->no_reply)
    shell_lua_api_own (state, G_OBJECT (call), &call->own);

  bus_when_ready (bus, call_op_run, op, call_op_free);

  shell_lua_handle_push (L, "shell.DBusCall", G_OBJECT (call));
  shell_lua_api_anchor (L, -1, G_OBJECT (call));
  g_object_unref (call);
}

static int
l_call_cancel (lua_State *L)
{
  GObject *object = shell_lua_handle_check (L, 1, "shell.DBusCall");

  call_cancel (SHELL_LUA_CALL (object));
  return 0;
}

static const luaL_Reg call_methods[] = {
  { "cancel", l_call_cancel },
  { NULL, NULL }
};

static const ShellLuaHandleProperty call_properties[] = {
  { "pending", "pending", TRUE, FALSE },
  { NULL }
};

static ShellLuaHandleType call_type = {
  .name = "shell.DBusCall",
  .methods = call_methods,
  .properties = call_properties,
};

/* ------------------------------------------------------------------------ */
/* ShellLuaProxy                                                             */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LUA_PROXY (shell_lua_proxy_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaProxy, shell_lua_proxy, SHELL, LUA_PROXY, GObject)

struct _ShellLuaProxy
{
  GObject parent_instance;

  ShellLuaBus *bus;       /* strong */
  char        *name;
  char        *path;
  char        *interface;
};

G_DEFINE_FINAL_TYPE (ShellLuaProxy, shell_lua_proxy, G_TYPE_OBJECT)

enum {
  PROXY_PROP_0,
  PROXY_PROP_NAME,
  PROXY_PROP_PATH,
  PROXY_PROP_INTERFACE,
  PROXY_PROP_BUS,
  PROXY_N_PROPS
};

static void
shell_lua_proxy_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  ShellLuaProxy *proxy = SHELL_LUA_PROXY (object);

  switch (prop_id)
    {
    case PROXY_PROP_NAME:
      g_value_set_string (value, proxy->name);
      break;
    case PROXY_PROP_PATH:
      g_value_set_string (value, proxy->path);
      break;
    case PROXY_PROP_INTERFACE:
      g_value_set_string (value, proxy->interface);
      break;
    case PROXY_PROP_BUS:
      g_value_set_string (value, proxy->bus != NULL && proxy->bus->bus_type == G_BUS_TYPE_SYSTEM ?
                                 "system" : "session");
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_lua_proxy_finalize (GObject *object)
{
  ShellLuaProxy *proxy = SHELL_LUA_PROXY (object);

  g_free (proxy->name);
  g_free (proxy->path);
  g_free (proxy->interface);

  G_OBJECT_CLASS (shell_lua_proxy_parent_class)->finalize (object);
}

static void
shell_lua_proxy_dispose (GObject *object)
{
  ShellLuaProxy *proxy = SHELL_LUA_PROXY (object);

  /* keep proxy->bus: the "bus" property is still read after destroy() by
   * code holding a reference; it is released with the last one */
  g_clear_object (&proxy->bus);

  G_OBJECT_CLASS (shell_lua_proxy_parent_class)->dispose (object);
}

static void
shell_lua_proxy_class_init (ShellLuaProxyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  const GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;

  object_class->get_property = shell_lua_proxy_get_property;
  object_class->dispose = shell_lua_proxy_dispose;
  object_class->finalize = shell_lua_proxy_finalize;

  g_object_class_install_property (object_class, PROXY_PROP_NAME,
                                   g_param_spec_string ("name", NULL, NULL, NULL, flags));
  g_object_class_install_property (object_class, PROXY_PROP_PATH,
                                   g_param_spec_string ("path", NULL, NULL, NULL, flags));
  g_object_class_install_property (object_class, PROXY_PROP_INTERFACE,
                                   g_param_spec_string ("interface", NULL, NULL, NULL, flags));
  g_object_class_install_property (object_class, PROXY_PROP_BUS,
                                   g_param_spec_string ("bus", NULL, NULL, NULL, flags));
}

static void
shell_lua_proxy_init (ShellLuaProxy *proxy)
{
}

static void
proxy_destroy_op (GObject *object)
{
  /* Nothing to undo: the triple has no resources of its own. */
}

static const ShellLuaOwnOps proxy_ops = { .destroy = proxy_destroy_op };

static ShellLuaProxy *
check_proxy (lua_State *L)
{
  return SHELL_LUA_PROXY (shell_lua_handle_check (L, 1, "shell.DBusProxy"));
}

static int
timeout_field (lua_State *L,
               int        tbl)
{
  lua_Integer ms = shell_lua_opt_integer (L, tbl, "timeout_ms", DEFAULT_TIMEOUT_MS);

  if (ms < 1 || ms > MAX_TIMEOUT_MS)
    luaL_error (L, "field 'timeout_ms' must be between 1 and %d", MAX_TIMEOUT_MS);
  return (int) ms;
}

/* proxy:call{ method, signature, args, on_reply, on_error, timeout_ms } */
static int
l_proxy_call (lua_State *L)
{
  static const char * const fields[] = {
    "method", "signature", "args", "on_reply", "on_error", "timeout_ms", NULL
  };
  ShellLuaProxy *proxy = check_proxy (L);
  const char *method, *signature;
  GVariant *params = NULL;
  int timeout_ms, reply_ref, error_ref;
  gsize n_args = 0;

  check_options (L, 2);
  shell_lua_check_fields (L, 2, fields);
  method = required_string (L, 2, "method");
  if (!g_dbus_is_member_name (method))
    luaL_error (L, "field 'method' is not a valid D-Bus method name: '%s'", method);
  signature = shell_lua_opt_string (L, 2, "signature", NULL);
  timeout_ms = timeout_field (L, 2);
  check_callback_field (L, 2, "on_reply");
  check_callback_field (L, 2, "on_error");

  if (lua_getfield (L, 2, "args") != LUA_TNIL)
    {
      if (!lua_istable (L, -1))
        luaL_error (L, "field 'args' must be a sequence, got %s", luaL_typename (L, -1));
      n_args = lua_rawlen (L, -1);
    }
  if (signature == NULL && n_args > 0)
    luaL_error (L, "field 'signature' is required when 'args' is not empty");
  if (signature != NULL && *signature != '\0')
    {
      const char *tuple;
      const GVariantType *type;

      check_signature (L, signature, "field 'signature'");
      /* a Lua string, so that a later error cannot leak it */
      tuple = lua_pushfstring (L, "(%s)", signature);
      type = G_VARIANT_TYPE (tuple);
      if (n_args != g_variant_type_n_items (type))
        luaL_error (L, "field 'args' has %d elements but signature '%s' has %d",
                    (int) n_args, signature, (int) g_variant_type_n_items (type));
      if (lua_isnil (L, -2))
        lua_newtable (L);
      else
        lua_pushvalue (L, -2);
      params = shell_lua_to_variant (L, -1, type);
      lua_pop (L, 2);
    }
  else if (n_args > 0)
    {
      luaL_error (L, "field 'args' has %d elements but the signature is empty", (int) n_args);
    }
  lua_pop (L, 1);

  reply_ref = shell_lua_opt_callback (L, 2, "on_reply", FALSE);
  error_ref = shell_lua_opt_callback (L, 2, "on_error", FALSE);

  start_call (L, proxy->bus, proxy->name, proxy->path, proxy->interface, method,
              params, timeout_ms, reply_ref, error_ref);
  if (params != NULL)
    g_variant_unref (params);
  return 1;
}

/* The three org.freedesktop.DBus.Properties methods share the call path. */
static int
properties_call (lua_State  *L,
                 const char *method,
                 const char *const *fields,
                 const char *reply_field,
                 gboolean    needs_name,
                 gboolean    needs_value)
{
  ShellLuaProxy *proxy = check_proxy (L);
  const char *name = NULL;
  GVariant *params;
  int timeout_ms, reply_ref, error_ref;

  check_options (L, 2);
  shell_lua_check_fields (L, 2, fields);
  if (needs_name)
    {
      name = required_string (L, 2, "name");
      if (!g_dbus_is_member_name (name))
        luaL_error (L, "field 'name' is not a valid property name: '%s'", name);
    }
  timeout_ms = timeout_field (L, 2);
  check_callback_field (L, 2, reply_field);
  check_callback_field (L, 2, "on_error");

  if (needs_value)
    {
      const char *signature = required_string (L, 2, "signature");
      GVariant *value;

      check_single_type (L, signature, "field 'signature'");
      if (lua_getfield (L, 2, "value") == LUA_TNIL)
        luaL_error (L, "missing required field 'value'");
      value = shell_lua_to_variant (L, -1, G_VARIANT_TYPE (signature));
      lua_pop (L, 1);

      /* Set carries the value in a `v`; a property of type `v` therefore
       * travels as a `v` inside that `v` */
      params = g_variant_new ("(ss@v)", proxy->interface, name, g_variant_new_variant (value));
      g_variant_unref (value);
    }
  else if (needs_name)
    params = g_variant_new ("(ss)", proxy->interface, name);
  else
    params = g_variant_new ("(s)", proxy->interface);

  reply_ref = shell_lua_opt_callback (L, 2, reply_field, FALSE);
  error_ref = shell_lua_opt_callback (L, 2, "on_error", FALSE);

  start_call (L, proxy->bus, proxy->name, proxy->path, "org.freedesktop.DBus.Properties", method,
              params, timeout_ms, reply_ref, error_ref);
  return 1;
}

static int
l_proxy_get_property (lua_State *L)
{
  static const char * const fields[] = { "name", "on_reply", "on_error", "timeout_ms", NULL };

  return properties_call (L, "Get", fields, "on_reply", TRUE, FALSE);
}

static int
l_proxy_get_all (lua_State *L)
{
  static const char * const fields[] = { "on_reply", "on_error", "timeout_ms", NULL };

  return properties_call (L, "GetAll", fields, "on_reply", FALSE, FALSE);
}

static int
l_proxy_set_property (lua_State *L)
{
  static const char * const fields[] = {
    "name", "signature", "value", "on_done", "on_error", "timeout_ms", NULL
  };

  return properties_call (L, "Set", fields, "on_done", TRUE, TRUE);
}

/* ---- signal subscriptions ------------------------------------------------ */

typedef struct {
  grefcount           ref_count;
  ShellLuaState      *state;       /* unowned; NULL once disconnected */
  ShellLuaConnection *connection;  /* unowned; NULL once disconnected */
  GDBusConnection    *bus_connection;
  guint               id;          /* g_dbus_connection_signal_subscribe */
  gboolean            cancelled;
  gboolean            properties;  /* PropertiesChanged: fn(changed, invalidated) */
  char               *sender;
  char               *path;
  char               *interface;   /* the interface of the signal */
  char               *member;      /* NULL: every signal */
  char               *arg0;        /* NULL: no filter */
} SignalSub;

static SignalSub *
signal_sub_ref (SignalSub *sub)
{
  g_ref_count_inc (&sub->ref_count);
  return sub;
}

static void
signal_sub_unref (gpointer data)
{
  SignalSub *sub = data;

  if (g_ref_count_dec (&sub->ref_count))
    {
      g_clear_object (&sub->bus_connection);
      g_free (sub->sender);
      g_free (sub->path);
      g_free (sub->interface);
      g_free (sub->member);
      g_free (sub->arg0);
      g_free (sub);
    }
}

/* disconnect_fn of the shell.Connection */
static void
signal_sub_disconnect (gpointer data)
{
  SignalSub *sub = data;

  sub->cancelled = TRUE;
  sub->connection = NULL;
  sub->state = NULL;
  if (sub->id != 0 && sub->bus_connection != NULL)
    {
      guint id = sub->id;

      sub->id = 0;
      g_dbus_connection_signal_unsubscribe (sub->bus_connection, id);
    }
  signal_sub_unref (sub);
}

typedef struct {
  const char *signal_name;
  GVariant   *parameters;
  gboolean    properties;
} SignalArgs;

static void
push_signal_args (lua_State *L,
                  gpointer   data)
{
  SignalArgs *args = data;

  if (args->properties)
    {
      /* (s, a{sv}, as): the table of the tuple, two of its fields */
      shell_lua_push_variant (L, args->parameters);
      lua_rawgeti (L, -1, 2);
      lua_rawgeti (L, -2, 3);
      lua_remove (L, -3);
    }
  else
    {
      lua_pushstring (L, args->signal_name);
      push_tuple_children (L, args->parameters);
    }
}

static void
signal_sub_cb (GDBusConnection *connection,
               const char      *sender_name,
               const char      *object_path,
               const char      *interface_name,
               const char      *signal_name,
               GVariant        *parameters,
               gpointer         user_data)
{
  SignalSub *sub = user_data;
  SignalArgs args = { signal_name, parameters, sub->properties };
  int ref, nargs;

  if (sub->cancelled || sub->connection == NULL || !state_usable (sub->state))
    return;
  ref = shell_lua_connection_get_callback_ref (sub->connection);
  if (ref == LUA_NOREF)
    return;

  if (sub->properties)
    {
      if (!g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(sa{sv}as)")))
        return;
      nargs = 2;
    }
  else
    {
      nargs = 1 + (int) g_variant_n_children (parameters);
    }

  shell_lua_call_ref (sub->state, ref, nargs, push_signal_args, &args);
}

static void
signal_sub_op (ShellLuaBus     *bus,
               GDBusConnection *connection,
               gpointer         data)
{
  SignalSub *sub = data;

  if (sub->cancelled || connection == NULL)
    return;

  sub->bus_connection = g_object_ref (connection);
  sub->id = g_dbus_connection_signal_subscribe (connection, sub->sender, sub->interface,
                                                sub->member, sub->path, sub->arg0,
                                                G_DBUS_SIGNAL_FLAGS_NONE,
                                                signal_sub_cb, signal_sub_ref (sub),
                                                signal_sub_unref);
}

/* Subscribes and pushes the shell.Connection; takes over the callback at @fn. */
static int
subscribe (lua_State     *L,
           ShellLuaProxy *proxy,
           int            fn,
           const char    *interface,
           const char    *member,
           const char    *arg0,
           gboolean       properties)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  ShellLuaConnection *connection;
  SignalSub *sub = g_new0 (SignalSub, 1);
  int ref;

  g_ref_count_init (&sub->ref_count);
  sub->state = state;
  sub->properties = properties;
  sub->sender = g_strdup (proxy->name);
  sub->path = g_strdup (proxy->path);
  sub->interface = g_strdup (interface);
  sub->member = g_strdup (member);
  sub->arg0 = g_strdup (arg0);

  lua_pushvalue (L, fn);
  ref = luaL_ref (L, LUA_REGISTRYINDEX);

  connection = shell_lua_connection_new (state, ref, signal_sub_disconnect, sub);
  sub->connection = connection;
  shell_lua_connection_own (connection);

  bus_when_ready (proxy->bus, signal_sub_op, signal_sub_ref (sub), signal_sub_unref);

  return shell_lua_connection_push_take (connection, L);
}

/* proxy:on_signal(name_or_nil, fn) */
static int
l_proxy_on_signal (lua_State *L)
{
  ShellLuaProxy *proxy = check_proxy (L);
  const char *member = NULL;

  if (!lua_isnil (L, 2))
    {
      member = luaL_checkstring (L, 2);
      if (!g_dbus_is_member_name (member))
        return luaL_argerror (L, 2, "not a valid D-Bus signal name");
    }
  luaL_checktype (L, 3, LUA_TFUNCTION);

  return subscribe (L, proxy, 3, proxy->interface, member, NULL, FALSE);
}

/* proxy:on_properties_changed(fn) */
static int
l_proxy_on_properties_changed (lua_State *L)
{
  ShellLuaProxy *proxy = check_proxy (L);

  luaL_checktype (L, 2, LUA_TFUNCTION);
  return subscribe (L, proxy, 2, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                    proxy->interface, TRUE);
}

static const luaL_Reg proxy_methods[] = {
  { "call",                  l_proxy_call },
  { "get_property",          l_proxy_get_property },
  { "get_all",               l_proxy_get_all },
  { "set_property",          l_proxy_set_property },
  { "on_signal",             l_proxy_on_signal },
  { "on_properties_changed", l_proxy_on_properties_changed },
  { NULL, NULL }
};

static const ShellLuaHandleProperty proxy_properties[] = {
  { "name",      "name",      TRUE, FALSE },
  { "path",      "path",      TRUE, FALSE },
  { "interface", "interface", TRUE, FALSE },
  { "bus",       "bus",       TRUE, FALSE },
  { NULL }
};

static ShellLuaHandleType proxy_type = {
  .name = "shell.DBusProxy",
  .methods = proxy_methods,
  .properties = proxy_properties,
};

/* ------------------------------------------------------------------------ */
/* ShellLuaBusName                                                           */
/* ------------------------------------------------------------------------ */

typedef struct _ShellLuaBusName ShellLuaBusName;

/* Shared with g_bus_own_name(), which may call back after we let go */
typedef struct {
  grefcount        ref_count;
  ShellLuaBusName *name;    /* unowned; NULL once released */
} NameShared;

#define SHELL_TYPE_LUA_BUS_NAME (shell_lua_bus_name_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaBusName, shell_lua_bus_name, SHELL, LUA_BUS_NAME, GObject)

struct _ShellLuaBusName
{
  GObject parent_instance;

  ShellLuaState *state;
  char          *name;
  guint          owner_id;
  gboolean       acquired;
  int            acquired_ref;
  int            lost_ref;
  NameShared    *shared;
};

G_DEFINE_FINAL_TYPE (ShellLuaBusName, shell_lua_bus_name, G_TYPE_OBJECT)

enum { NAME_PROP_0, NAME_PROP_NAME, NAME_PROP_ACQUIRED, NAME_N_PROPS };
static GParamSpec *name_props[NAME_N_PROPS];

static void
name_shared_unref (gpointer data)
{
  NameShared *shared = data;

  if (g_ref_count_dec (&shared->ref_count))
    g_free (shared);
}

static void
bus_name_release (ShellLuaBusName *self)
{
  guint id = self->owner_id;

  self->owner_id = 0;
  if (self->shared != NULL)
    {
      self->shared->name = NULL;
      g_clear_pointer (&self->shared, name_shared_unref);
    }
  if (id != 0)
    g_bus_unown_name (id);
  self->acquired = FALSE;
  release_ref (self->state, &self->acquired_ref);
  release_ref (self->state, &self->lost_ref);
  self->state = NULL;
}

static void
bus_name_destroy_op (GObject *object)
{
  bus_name_release (SHELL_LUA_BUS_NAME (object));
}

static void
shell_lua_bus_name_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellLuaBusName *self = SHELL_LUA_BUS_NAME (object);

  switch (prop_id)
    {
    case NAME_PROP_NAME:
      g_value_set_string (value, self->name);
      break;
    case NAME_PROP_ACQUIRED:
      g_value_set_boolean (value, self->acquired);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_lua_bus_name_dispose (GObject *object)
{
  bus_name_release (SHELL_LUA_BUS_NAME (object));

  G_OBJECT_CLASS (shell_lua_bus_name_parent_class)->dispose (object);
}

static void
shell_lua_bus_name_finalize (GObject *object)
{
  g_free (SHELL_LUA_BUS_NAME (object)->name);

  G_OBJECT_CLASS (shell_lua_bus_name_parent_class)->finalize (object);
}

static void
shell_lua_bus_name_class_init (ShellLuaBusNameClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_lua_bus_name_get_property;
  object_class->dispose = shell_lua_bus_name_dispose;
  object_class->finalize = shell_lua_bus_name_finalize;

  name_props[NAME_PROP_NAME] =
    g_param_spec_string ("name", NULL, NULL, NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  name_props[NAME_PROP_ACQUIRED] =
    g_param_spec_boolean ("acquired", NULL, NULL, FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties (object_class, NAME_N_PROPS, name_props);
}

static void
shell_lua_bus_name_init (ShellLuaBusName *self)
{
  self->acquired_ref = LUA_NOREF;
  self->lost_ref = LUA_NOREF;
}

static void
bus_name_call (ShellLuaBusName *self,
               int              ref)
{
  if (ref != LUA_NOREF && state_usable (self->state))
    shell_lua_call_ref (self->state, ref, 0, NULL, NULL);
}

static void
name_acquired_cb (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
  NameShared *shared = user_data;
  ShellLuaBusName *self = shared->name;

  if (self == NULL)
    return;
  g_object_ref (self);
  self->acquired = TRUE;
  g_object_notify_by_pspec (G_OBJECT (self), name_props[NAME_PROP_ACQUIRED]);
  bus_name_call (self, self->acquired_ref);
  g_object_unref (self);
}

static void
name_lost_cb (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
  NameShared *shared = user_data;
  ShellLuaBusName *self = shared->name;

  if (self == NULL)
    return;
  g_object_ref (self);
  if (self->acquired)
    {
      self->acquired = FALSE;
      g_object_notify_by_pspec (G_OBJECT (self), name_props[NAME_PROP_ACQUIRED]);
    }
  bus_name_call (self, self->lost_ref);
  g_object_unref (self);
}

static const ShellLuaOwnOps bus_name_ops = { .destroy = bus_name_destroy_op };

static int
l_bus_name_release (lua_State *L)
{
  GObject *object = shell_lua_handle_get (L, 1, "shell.BusName");

  if (object != NULL)
    shell_lua_destroy_object (L, object);
  return 0;
}

static const luaL_Reg bus_name_methods[] = {
  { NULL, NULL }
};

static const ShellLuaHandleMethod bus_name_methods_ex[] = {
  { "release", l_bus_name_release,              SHELL_LUA_METHOD_DEAD_OK },
  { "destroy", shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty bus_name_properties[] = {
  { "name",     "name",     TRUE, FALSE },
  { "acquired", "acquired", TRUE, FALSE },
  { NULL }
};

static ShellLuaHandleType bus_name_type = {
  .name = "shell.BusName",
  .methods = bus_name_methods,
  .methods_ex = bus_name_methods_ex,
  .properties = bus_name_properties,
};

/* ------------------------------------------------------------------------ */
/* Name watches                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
  grefcount           ref_count;
  ShellLuaState      *state;      /* unowned; NULL once disconnected */
  ShellLuaConnection *connection; /* unowned */
  guint               watch_id;
  int                 appeared_ref;
  int                 vanished_ref;
} NameWatch;

static void
name_watch_unref (gpointer data)
{
  NameWatch *watch = data;

  if (g_ref_count_dec (&watch->ref_count))
    g_free (watch);
}

static void
name_watch_disconnect (gpointer data)
{
  NameWatch *watch = data;
  guint id = watch->watch_id;

  watch->watch_id = 0;
  release_ref (watch->state, &watch->appeared_ref);
  release_ref (watch->state, &watch->vanished_ref);
  watch->state = NULL;
  watch->connection = NULL;
  if (id != 0)
    g_bus_unwatch_name (id);
  name_watch_unref (watch);
}

static void
push_owner (lua_State *L,
            gpointer   data)
{
  lua_pushstring (L, data);
}

static void
name_appeared_cb (GDBusConnection *connection,
                  const char      *name,
                  const char      *name_owner,
                  gpointer         user_data)
{
  NameWatch *watch = user_data;

  if (watch->state != NULL && watch->appeared_ref != LUA_NOREF && state_usable (watch->state))
    shell_lua_call_ref (watch->state, watch->appeared_ref, 1, push_owner, (gpointer) name_owner);
}

static void
name_vanished_cb (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
  NameWatch *watch = user_data;

  if (watch->state != NULL && watch->vanished_ref != LUA_NOREF && state_usable (watch->state))
    shell_lua_call_ref (watch->state, watch->vanished_ref, 0, NULL, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellLuaDBusObject: bus:export{...}                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LUA_DBUS_OBJECT (shell_lua_dbus_object_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaDBusObject, shell_lua_dbus_object, SHELL, LUA_DBUS_OBJECT, GObject)

struct _ShellLuaDBusObject
{
  GObject parent_instance;

  ShellLuaState   *state;       /* unowned; NULL once destroyed */
  ShellLuaBus     *bus;         /* strong */
  char            *path;
  char            *interface;
  GDBusNodeInfo   *node;
  GDBusInterfaceInfo *info;     /* borrowed from @node */
  GHashTable      *methods;     /* method name -> GINT_TO_POINTER (registry ref) */
  GHashTable      *properties;  /* property name -> GVariant (sunk) */
  GDBusConnection *connection;  /* set once registered */
  guint            registration_id;
};

G_DEFINE_FINAL_TYPE (ShellLuaDBusObject, shell_lua_dbus_object, G_TYPE_OBJECT)

enum { OBJECT_PROP_0, OBJECT_PROP_PATH, OBJECT_PROP_INTERFACE, OBJECT_N_PROPS };

static void
object_unregister (ShellLuaDBusObject *self)
{
  if (self->registration_id != 0 && self->connection != NULL)
    g_dbus_connection_unregister_object (self->connection, self->registration_id);
  self->registration_id = 0;
  g_clear_object (&self->connection);
}

static void
object_release_methods (ShellLuaDBusObject *self)
{
  GHashTableIter iter;
  gpointer key, value;

  if (self->methods == NULL)
    return;

  g_hash_table_iter_init (&iter, self->methods);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      int ref = GPOINTER_TO_INT (value);

      release_ref (self->state, &ref);
    }
  g_hash_table_remove_all (self->methods);
}

static void
object_destroy_op (GObject *object)
{
  ShellLuaDBusObject *self = SHELL_LUA_DBUS_OBJECT (object);

  object_unregister (self);
  object_release_methods (self);
  self->state = NULL;
}

static void
shell_lua_dbus_object_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  ShellLuaDBusObject *self = SHELL_LUA_DBUS_OBJECT (object);

  switch (prop_id)
    {
    case OBJECT_PROP_PATH:
      g_value_set_string (value, self->path);
      break;
    case OBJECT_PROP_INTERFACE:
      g_value_set_string (value, self->interface);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_lua_dbus_object_dispose (GObject *object)
{
  ShellLuaDBusObject *self = SHELL_LUA_DBUS_OBJECT (object);

  object_unregister (self);
  object_release_methods (self);
  self->state = NULL;
  g_clear_object (&self->bus);

  G_OBJECT_CLASS (shell_lua_dbus_object_parent_class)->dispose (object);
}

static void
shell_lua_dbus_object_finalize (GObject *object)
{
  ShellLuaDBusObject *self = SHELL_LUA_DBUS_OBJECT (object);

  g_free (self->path);
  g_free (self->interface);
  g_clear_pointer (&self->methods, g_hash_table_unref);
  g_clear_pointer (&self->properties, g_hash_table_unref);
  g_clear_pointer (&self->node, g_dbus_node_info_unref);

  G_OBJECT_CLASS (shell_lua_dbus_object_parent_class)->finalize (object);
}

static void
shell_lua_dbus_object_class_init (ShellLuaDBusObjectClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  const GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;

  object_class->get_property = shell_lua_dbus_object_get_property;
  object_class->dispose = shell_lua_dbus_object_dispose;
  object_class->finalize = shell_lua_dbus_object_finalize;

  g_object_class_install_property (object_class, OBJECT_PROP_PATH,
                                   g_param_spec_string ("path", NULL, NULL, NULL, flags));
  g_object_class_install_property (object_class, OBJECT_PROP_INTERFACE,
                                   g_param_spec_string ("interface", NULL, NULL, NULL, flags));
}

static void
shell_lua_dbus_object_init (ShellLuaDBusObject *self)
{
  self->methods = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->properties = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                            (GDestroyNotify) g_variant_unref);
}

static const ShellLuaOwnOps object_ops = { .destroy = object_destroy_op };

/* ---- the value a property has until the extension sets one ---------------- */

static GVariant *
default_value (const GVariantType *type)
{
  const char kind = g_variant_type_peek_string (type)[0];

  if (g_variant_type_is_array (type))
    return g_variant_new_array (g_variant_type_element (type), NULL, 0);
  if (g_variant_type_is_tuple (type))
    {
      GPtrArray *children = g_ptr_array_new ();
      const GVariantType *item;
      GVariant *result;

      for (item = g_variant_type_first (type); item != NULL; item = g_variant_type_next (item))
        g_ptr_array_add (children, default_value (item));
      result = g_variant_new_tuple ((GVariant * const *) children->pdata, children->len);
      g_ptr_array_free (children, TRUE);
      return result;
    }
  if (g_variant_type_is_dict_entry (type))
    return g_variant_new_dict_entry (default_value (g_variant_type_key (type)),
                                     default_value (g_variant_type_value (type)));

  switch (kind)
    {
    case 'b': return g_variant_new_boolean (FALSE);
    case 'y': return g_variant_new_byte (0);
    case 'n': return g_variant_new_int16 (0);
    case 'q': return g_variant_new_uint16 (0);
    case 'i': return g_variant_new_int32 (0);
    case 'u': return g_variant_new_uint32 (0);
    case 'x': return g_variant_new_int64 (0);
    case 't': return g_variant_new_uint64 (0);
    case 'd': return g_variant_new_double (0);
    case 's': return g_variant_new_string ("");
    case 'o': return g_variant_new_object_path ("/");
    case 'g': return g_variant_new_signature ("");
    case 'v': return g_variant_new_variant (g_variant_new_string (""));
    default:
      g_assert_not_reached ();
    }
}

/* ---- the vtable ----------------------------------------------------------- */

typedef struct {
  ShellLuaDBusObject *self;
  GDBusMethodInfo    *info;
  int                 fn_ref;
  GVariant           *parameters;
  GPtrArray          *results;      /* GVariant, sunk, one per out argument */
  char               *error_name;   /* set when the function returned nil, name, message */
  char               *error_message;
} MethodCall;

/* Runs in the protected call. Anything that is not freed by the caller must
 * not be allocated here. */
static int
method_call_trampoline (lua_State *L,
                        gpointer   data)
{
  MethodCall *call = data;
  gsize i, n_in = g_variant_n_children (call->parameters);
  guint n_out = 0;
  int base, n_results;

  shell_lua_state_push_ref (call->self->state, call->fn_ref);
  lua_checkstack (L, (int) n_in + 4);
  push_tuple_children (L, call->parameters);
  base = lua_gettop (L) - (int) n_in - 1;   /* index below the function */
  lua_call (L, (int) n_in, LUA_MULTRET);
  n_results = lua_gettop (L) - base;

  if (call->info->out_args != NULL)
    for (; call->info->out_args[n_out] != NULL; n_out++)
      ;

  /* `return nil, "org.example.Error.X", "message"` */
  if (n_results >= 1 && lua_isnil (L, base + 1))
    {
      const char *name, *message;

      if (n_results < 2 || lua_type (L, base + 2) != LUA_TSTRING)
        return luaL_error (L, "method '%s' returned nil without an error name "
                           "(return nil, name, message to fail)", call->info->name);
      name = lua_tostring (L, base + 2);
      if (!g_dbus_is_interface_name (name))
        return luaL_error (L, "method '%s' returned '%s', which is not a valid error name",
                           call->info->name, name);
      message = n_results >= 3 && lua_type (L, base + 3) == LUA_TSTRING ?
                lua_tostring (L, base + 3) : "";
      if (!g_utf8_validate (message, -1, NULL))
        message = "";
      call->error_name = g_strdup (name);
      call->error_message = g_strdup (message);
      return 0;
    }

  if ((guint) n_results != n_out)
    return luaL_error (L, "method '%s' returned %d values but its XML declares %d out arguments",
                       call->info->name, n_results, (int) n_out);

  for (i = 0; i < n_out; i++)
    {
      GVariant *value = shell_lua_to_variant (L, base + 1 + (int) i,
                                              G_VARIANT_TYPE (call->info->out_args[i]->signature));

      g_ptr_array_add (call->results, value);
    }
  return 0;
}

static void
object_method_call (GDBusConnection       *connection,
                    const char            *sender,
                    const char            *object_path,
                    const char            *interface_name,
                    const char            *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
  ShellLuaDBusObject *self = user_data;
  gpointer found = NULL;
  MethodCall call = { 0 };
  gboolean ok;

  if (self->state == NULL || !g_hash_table_lookup_extended (self->methods, method_name, NULL, &found))
    {
      g_dbus_method_invocation_return_dbus_error (invocation, ERR_UNKNOWN_METHOD,
                                                  "No function implements this method");
      return;
    }

  call.self = g_object_ref (self);
  call.info = (GDBusMethodInfo *) g_dbus_method_invocation_get_method_info (invocation);
  call.fn_ref = GPOINTER_TO_INT (found);
  call.parameters = parameters;
  call.results = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);

  ok = shell_lua_api_call_protected (self->state, 0, method_call_trampoline, &call);

  if (!ok)
    {
      g_dbus_method_invocation_return_dbus_error (invocation, ERR_FAILED, "The method failed");
    }
  else if (call.error_name != NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation, call.error_name,
                                                  call.error_message);
    }
  else
    {
      GVariant *reply = g_variant_new_tuple ((GVariant * const *) call.results->pdata,
                                             call.results->len);

      /* the tuple took its own references */
      g_dbus_method_invocation_return_value (invocation, reply);
    }

  g_ptr_array_unref (call.results);
  g_free (call.error_name);
  g_free (call.error_message);
  g_object_unref (call.self);
}

static GVariant *
object_get_property (GDBusConnection  *connection,
                     const char       *sender,
                     const char       *object_path,
                     const char       *interface_name,
                     const char       *property_name,
                     GError          **error,
                     gpointer          user_data)
{
  ShellLuaDBusObject *self = user_data;
  GVariant *value = g_hash_table_lookup (self->properties, property_name);

  if (value == NULL)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                   "No such property '%s'", property_name);
      return NULL;
    }
  return g_variant_ref (value);
}

static const GDBusInterfaceVTable object_vtable = {
  object_method_call,
  object_get_property,
  NULL,
  { 0 }
};

static void
object_register_op (ShellLuaBus     *bus,
                    GDBusConnection *connection,
                    gpointer         data)
{
  ShellLuaDBusObject *self = data;
  g_autoptr (GError) error = NULL;

  if (self->state == NULL)
    return;
  if (connection == NULL)
    {
      ext_log (self->state, G_LOG_LEVEL_WARNING, "cannot export %s: no connection to the bus",
               self->path);
      return;
    }

  self->registration_id = g_dbus_connection_register_object (connection, self->path, self->info,
                                                             &object_vtable, self, NULL, &error);
  if (self->registration_id == 0)
    ext_log (self->state, G_LOG_LEVEL_WARNING, "cannot export %s on %s: %s", self->interface,
             self->path, error->message);
  else
    self->connection = g_object_ref (connection);
}

/* ---- construction ---------------------------------------------------------- */

typedef struct {
  ShellLuaBus        *bus;
  ShellLuaDBusObject *object;   /* filled in by the build function */
  const char         *path;
  const char         *xml;
} ExportContext;

static void
check_node (lua_State *L,
            GDBusNodeInfo *node)
{
  GDBusInterfaceInfo *info;
  guint i;

  if (node->interfaces == NULL || node->interfaces[0] == NULL || node->interfaces[1] != NULL)
    luaL_error (L, "field 'interface_xml' must contain exactly one <interface>");
  if (node->nodes != NULL && node->nodes[0] != NULL)
    luaL_error (L, "field 'interface_xml' must not contain child <node> elements");

  info = node->interfaces[0];
  if (is_shell_name (info->name) || is_system_name (info->name))
    luaL_error (L, "field 'interface_xml' declares the reserved interface '%s'", info->name);

  for (i = 0; info->methods != NULL && info->methods[i] != NULL; i++)
    {
      GDBusMethodInfo *method = info->methods[i];
      guint a;

      for (a = 0; method->in_args != NULL && method->in_args[a] != NULL; a++)
        check_single_type (L, method->in_args[a]->signature, "method argument type");
      for (a = 0; method->out_args != NULL && method->out_args[a] != NULL; a++)
        check_single_type (L, method->out_args[a]->signature, "method argument type");
    }
  for (i = 0; info->signals != NULL && info->signals[i] != NULL; i++)
    {
      GDBusSignalInfo *signal = info->signals[i];
      guint a;

      for (a = 0; signal->args != NULL && signal->args[a] != NULL; a++)
        check_single_type (L, signal->args[a]->signature, "signal argument type");
    }
  for (i = 0; info->properties != NULL && info->properties[i] != NULL; i++)
    {
      GDBusPropertyInfo *property = info->properties[i];

      check_single_type (L, property->signature, "property type");
      if (property->flags & G_DBUS_PROPERTY_INFO_FLAGS_WRITABLE)
        luaL_error (L, "property '%s' is writable: exported properties must be read-only",
                    property->name);
    }
}

/* Runs under lua_pcall: validates the options table (index 2 of the caller,
 * copied to index 1 here) and fills ctx->object. May raise. */
static int
export_build (lua_State *L)
{
  static const char * const fields[] = { "path", "interface_xml", "methods", "properties", NULL };
  ExportContext *ctx = lua_touserdata (L, 1);
  ShellLuaDBusObject *self = ctx->object;
  GError *error = NULL;
  const char *path, *xml;
  int tbl = 2;
  guint i;

  /* the options table is at 2 */
  shell_lua_check_fields (L, tbl, fields);
  path = required_string (L, tbl, "path");
  xml = required_string (L, tbl, "interface_xml");
  if (!g_variant_is_object_path (path))
    luaL_error (L, "field 'path' is not a valid object path: '%s'", path);

  self->node = g_dbus_node_info_new_for_xml (xml, &error);
  if (self->node == NULL)
    {
      /* a Lua string, so that the error can go before raising */
      lua_pushstring (L, error->message);
      g_error_free (error);
      luaL_error (L, "field 'interface_xml' is not valid: %s", lua_tostring (L, -1));
    }
  check_node (L, self->node);
  self->info = self->node->interfaces[0];
  self->path = g_strdup (path);
  self->interface = g_strdup (self->info->name);

  /* methods = { Name = function(...) } */
  if (lua_getfield (L, tbl, "methods") != LUA_TNIL)
    {
      if (!lua_istable (L, -1))
        luaL_error (L, "field 'methods' must be a table, got %s", luaL_typename (L, -1));
      lua_pushnil (L);
      while (lua_next (L, -2) != 0)
        {
          const char *name;

          if (lua_type (L, -2) != LUA_TSTRING)
            luaL_error (L, "field 'methods' has a non-string key (%s)", luaL_typename (L, -2));
          name = lua_tostring (L, -2);
          if (g_dbus_interface_info_lookup_method (self->info, name) == NULL)
            luaL_error (L, "field 'methods' names '%s', which interface_xml does not declare", name);
          if (lua_type (L, -1) != LUA_TFUNCTION)
            luaL_error (L, "field 'methods.%s' must be a function, got %s", name,
                        luaL_typename (L, -1));
          lua_pushvalue (L, -1);
          g_hash_table_insert (self->methods, g_strdup (name),
                               GINT_TO_POINTER (luaL_ref (L, LUA_REGISTRYINDEX)));
          lua_pop (L, 1);
        }
    }
  lua_pop (L, 1);

  /* properties = { Name = value }, converted by the XML type */
  if (lua_getfield (L, tbl, "properties") != LUA_TNIL)
    {
      if (!lua_istable (L, -1))
        luaL_error (L, "field 'properties' must be a table, got %s", luaL_typename (L, -1));
      lua_pushnil (L);
      while (lua_next (L, -2) != 0)
        {
          GDBusPropertyInfo *property;
          const char *name;

          if (lua_type (L, -2) != LUA_TSTRING)
            luaL_error (L, "field 'properties' has a non-string key (%s)", luaL_typename (L, -2));
          name = lua_tostring (L, -2);
          property = g_dbus_interface_info_lookup_property (self->info, name);
          if (property == NULL)
            luaL_error (L, "field 'properties' names '%s', which interface_xml does not declare",
                        name);
          g_hash_table_insert (self->properties, g_strdup (name),
                               shell_lua_to_variant (L, -1, G_VARIANT_TYPE (property->signature)));
          lua_pop (L, 1);
        }
    }
  lua_pop (L, 1);

  for (i = 0; self->info->properties != NULL && self->info->properties[i] != NULL; i++)
    {
      GDBusPropertyInfo *property = self->info->properties[i];

      if (!g_hash_table_contains (self->properties, property->name))
        g_hash_table_insert (self->properties, g_strdup (property->name),
                             g_variant_ref_sink (default_value (G_VARIANT_TYPE (property->signature))));
    }
  return 0;
}

static int
l_bus_export (lua_State *L)
{
  ShellLuaBus *bus = SHELL_LUA_BUS (shell_lua_handle_check (L, 1, "shell.Bus"));
  ExportContext ctx = { bus, NULL, NULL, NULL };
  int status;

  check_options (L, 2);
  shell_lua_check_owned_capacity (L);

  ctx.object = g_object_new (SHELL_TYPE_LUA_DBUS_OBJECT, NULL);
  ctx.object->state = shell_lua_state_from_L (L);
  ctx.object->bus = g_object_ref (bus);

  lua_pushcfunction (L, export_build);
  lua_pushlightuserdata (L, &ctx);
  lua_pushvalue (L, 2);
  status = lua_pcall (L, 2, 0, 0);
  if (status != LUA_OK)
    {
      /* object_release_methods needs the state to give the references back */
      g_object_run_dispose (G_OBJECT (ctx.object));
      g_object_unref (ctx.object);
      return lua_error (L);
    }

  /* Owning raises at the cap: the capacity check above makes that unlikely;
   * if it still happens the object is disposed by shell_lua_own_object. */
  shell_lua_own_object (L, G_OBJECT (ctx.object), &object_ops);
  g_object_unref (ctx.object);   /* the owner holds it now */

  bus_when_ready (bus, object_register_op, g_object_ref (ctx.object), g_object_unref);

  shell_lua_handle_push (L, "shell.DBusObject", G_OBJECT (ctx.object));
  return 1;
}

/* ---- obj:set_property, obj:emit_signal ------------------------------------- */

typedef struct {
  ShellLuaDBusObject *object;   /* strong */
  char               *interface;
  char               *member;
  GVariant           *parameters;   /* sunk */
  gboolean            properties_changed;
} EmitOp;

static void
emit_op_free (gpointer data)
{
  EmitOp *op = data;

  g_clear_object (&op->object);
  g_free (op->interface);
  g_free (op->member);
  g_clear_pointer (&op->parameters, g_variant_unref);
  g_free (op);
}

static void
emit_op_run (ShellLuaBus     *bus,
             GDBusConnection *connection,
             gpointer         data)
{
  EmitOp *op = data;
  ShellLuaDBusObject *self = op->object;
  g_autoptr (GError) error = NULL;

  /* Not registered (yet or any more): nothing to tell anybody */
  if (connection == NULL || self->state == NULL || self->registration_id == 0)
    return;

  if (!g_dbus_connection_emit_signal (connection, NULL, self->path, op->interface, op->member,
                                      op->parameters, &error))
    ext_log (self->state, G_LOG_LEVEL_WARNING, "cannot emit %s.%s: %s", op->interface,
             op->member, error->message);
}

static void
queue_emit (ShellLuaDBusObject *self,
            const char         *interface,
            const char         *member,
            GVariant           *parameters)
{
  EmitOp *op = g_new0 (EmitOp, 1);

  op->object = g_object_ref (self);
  op->interface = g_strdup (interface);
  op->member = g_strdup (member);
  op->parameters = g_variant_ref_sink (parameters);
  bus_when_ready (self->bus, emit_op_run, op, emit_op_free);
}

static int
l_object_set_property (lua_State *L)
{
  ShellLuaDBusObject *self = SHELL_LUA_DBUS_OBJECT (shell_lua_handle_check (L, 1, "shell.DBusObject"));
  const char *name = luaL_checkstring (L, 2);
  GDBusPropertyInfo *property = g_dbus_interface_info_lookup_property (self->info, name);
  GVariant *value;
  GVariantBuilder changed;

  if (property == NULL)
    return luaL_error (L, "%s has no property '%s'", self->interface, name);
  luaL_checkany (L, 3);

  value = shell_lua_to_variant (L, 3, G_VARIANT_TYPE (property->signature));
  g_hash_table_insert (self->properties, g_strdup (name), g_variant_ref (value));

  g_variant_builder_init (&changed, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&changed, "{sv}", name, value);
  g_variant_unref (value);
  queue_emit (self, "org.freedesktop.DBus.Properties", "PropertiesChanged",
              g_variant_new ("(s@a{sv}@as)", self->interface, g_variant_builder_end (&changed),
                             g_variant_new_strv (NULL, 0)));
  return 0;
}

/* obj:emit_signal{ name, args } */
static int
l_object_emit_signal (lua_State *L)
{
  static const char * const fields[] = { "name", "args", NULL };
  ShellLuaDBusObject *self = SHELL_LUA_DBUS_OBJECT (shell_lua_handle_check (L, 1, "shell.DBusObject"));
  const char *name;
  GDBusSignalInfo *signal;
  GVariant *params;
  GString *sig;
  gsize n_args = 0, n_expected = 0, i;

  check_options (L, 2);
  shell_lua_check_fields (L, 2, fields);
  name = required_string (L, 2, "name");
  signal = g_dbus_interface_info_lookup_signal (self->info, name);
  if (signal == NULL)
    return luaL_error (L, "%s has no signal '%s'", self->interface, name);

  sig = g_string_new ("(");
  for (i = 0; signal->args != NULL && signal->args[i] != NULL; i++)
    {
      g_string_append (sig, signal->args[i]->signature);
      n_expected++;
    }
  g_string_append_c (sig, ')');

  if (lua_getfield (L, 2, "args") != LUA_TNIL)
    {
      if (!lua_istable (L, -1))
        {
          g_string_free (sig, TRUE);
          return luaL_error (L, "field 'args' must be a sequence, got %s", luaL_typename (L, -1));
        }
      n_args = lua_rawlen (L, -1);
    }
  if (n_args != n_expected)
    {
      char *message = g_strdup_printf ("signal '%s' takes %d arguments, field 'args' has %d", name,
                                       (int) n_expected, (int) n_args);

      g_string_free (sig, TRUE);
      lua_pushstring (L, message);
      g_free (message);
      return lua_error (L);
    }
  if (lua_isnil (L, -1))
    {
      lua_pop (L, 1);
      lua_newtable (L);
    }

  /* may raise: only the signature string is live, and it is the one thing
   * we cannot free afterwards, so copy it into a Lua string first */
  lua_pushstring (L, sig->str);
  g_string_free (sig, TRUE);
  params = shell_lua_to_variant (L, -2, G_VARIANT_TYPE (lua_tostring (L, -1)));
  lua_pop (L, 2);

  queue_emit (self, self->interface, name, params);
  g_variant_unref (params);
  return 0;
}

static const luaL_Reg object_methods[] = {
  { "set_property", l_object_set_property },
  { "emit_signal",  l_object_emit_signal },
  { NULL, NULL }
};

static const ShellLuaHandleMethod object_methods_ex[] = {
  { "destroy", shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty object_properties[] = {
  { "path",      "path",      TRUE, FALSE },
  { "interface", "interface", TRUE, FALSE },
  { NULL }
};

static ShellLuaHandleType object_type = {
  .name = "shell.DBusObject",
  .methods = object_methods,
  .methods_ex = object_methods_ex,
  .properties = object_properties,
};

/* ------------------------------------------------------------------------ */
/* shell.Bus                                                                 */
/* ------------------------------------------------------------------------ */

static ShellLuaBus *
check_bus (lua_State *L)
{
  return SHELL_LUA_BUS (shell_lua_handle_check (L, 1, "shell.Bus"));
}

/* bus:proxy{ name, path, interface } */
static int
l_bus_proxy (lua_State *L)
{
  static const char * const fields[] = { "name", "path", "interface", NULL };
  ShellLuaBus *bus = check_bus (L);
  ShellLuaProxy *proxy;
  const char *name, *path, *interface;

  check_options (L, 2);
  shell_lua_check_fields (L, 2, fields);
  name = required_string (L, 2, "name");
  path = required_string (L, 2, "path");
  interface = required_string (L, 2, "interface");

  if (!g_dbus_is_name (name))
    return luaL_error (L, "field 'name' is not a valid bus name: '%s'", name);
  if (!g_variant_is_object_path (path))
    return luaL_error (L, "field 'path' is not a valid object path: '%s'", path);
  if (!g_dbus_is_interface_name (interface))
    return luaL_error (L, "field 'interface' is not a valid interface name: '%s'", interface);
  if (is_shell_name (name) || is_shell_name (interface))
    return luaL_error (L, "'%s' is reserved: the shell is reached through shell.*",
                       is_shell_name (name) ? name : interface);

  shell_lua_check_owned_capacity (L);
  proxy = g_object_new (SHELL_TYPE_LUA_PROXY, NULL);
  proxy->bus = g_object_ref (bus);
  proxy->name = g_strdup (name);
  proxy->path = g_strdup (path);
  proxy->interface = g_strdup (interface);

  shell_lua_own_object (L, G_OBJECT (proxy), &proxy_ops);
  g_object_unref (proxy);
  shell_lua_handle_push (L, "shell.DBusProxy", G_OBJECT (proxy));
  return 1;
}

/* bus:watch_name{ name, on_appeared, on_vanished } */
static int
l_bus_watch_name (lua_State *L)
{
  static const char * const fields[] = { "name", "on_appeared", "on_vanished", NULL };
  ShellLuaBus *bus = check_bus (L);
  ShellLuaState *state = shell_lua_state_from_L (L);
  ShellLuaConnection *connection;
  NameWatch *watch;
  const char *name;
  int appeared, vanished;

  check_options (L, 2);
  shell_lua_check_fields (L, 2, fields);
  name = required_string (L, 2, "name");
  if (!g_dbus_is_name (name))
    return luaL_error (L, "field 'name' is not a valid bus name: '%s'", name);
  check_callback_field (L, 2, "on_appeared");
  check_callback_field (L, 2, "on_vanished");
  lua_getfield (L, 2, "on_appeared");
  lua_getfield (L, 2, "on_vanished");
  if (lua_isnil (L, -1) && lua_isnil (L, -2))
    return luaL_error (L, "at least one of 'on_appeared' and 'on_vanished' is required");
  lua_pop (L, 2);

  appeared = shell_lua_opt_callback (L, 2, "on_appeared", FALSE);
  vanished = shell_lua_opt_callback (L, 2, "on_vanished", FALSE);

  watch = g_new0 (NameWatch, 1);
  g_ref_count_init (&watch->ref_count);
  watch->state = state;
  watch->appeared_ref = appeared;
  watch->vanished_ref = vanished;

  connection = shell_lua_connection_new (state, LUA_NOREF, name_watch_disconnect, watch);
  watch->connection = connection;
  shell_lua_connection_own (connection);

  g_ref_count_inc (&watch->ref_count);   /* for g_bus_watch_name */
  watch->watch_id = g_bus_watch_name (bus->bus_type, name, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                      appeared != LUA_NOREF ? name_appeared_cb : NULL,
                                      vanished != LUA_NOREF ? name_vanished_cb : NULL,
                                      watch, name_watch_unref);

  return shell_lua_connection_push_take (connection, L);
}

/* bus:own_name{ name, allow_replacement, replace, on_acquired, on_lost } */
static int
l_bus_own_name (lua_State *L)
{
  static const char * const fields[] = {
    "name", "allow_replacement", "replace", "on_acquired", "on_lost", NULL
  };
  ShellLuaBus *bus = check_bus (L);
  ShellLuaBusName *self;
  GBusNameOwnerFlags flags = G_BUS_NAME_OWNER_FLAGS_NONE;
  const char *name;

  check_options (L, 2);
  shell_lua_check_fields (L, 2, fields);
  name = required_string (L, 2, "name");
  if (!g_dbus_is_name (name) || name[0] == ':')
    return luaL_error (L, "field 'name' is not a valid well-known bus name: '%s'", name);
  if (is_system_name (name))
    return luaL_error (L, "field 'name' is in a reserved namespace: '%s'", name);
  if (shell_lua_opt_boolean (L, 2, "allow_replacement", FALSE))
    flags |= G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (shell_lua_opt_boolean (L, 2, "replace", FALSE))
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;
  check_callback_field (L, 2, "on_acquired");
  check_callback_field (L, 2, "on_lost");
  shell_lua_check_owned_capacity (L);

  self = g_object_new (SHELL_TYPE_LUA_BUS_NAME, NULL);
  self->state = shell_lua_state_from_L (L);
  self->name = g_strdup (name);
  self->acquired_ref = shell_lua_opt_callback (L, 2, "on_acquired", FALSE);
  self->lost_ref = shell_lua_opt_callback (L, 2, "on_lost", FALSE);
  self->shared = g_new0 (NameShared, 1);
  g_ref_count_init (&self->shared->ref_count);
  self->shared->name = self;

  shell_lua_own_object (L, G_OBJECT (self), &bus_name_ops);
  g_object_unref (self);

  g_ref_count_inc (&self->shared->ref_count);   /* for g_bus_own_name */
  self->owner_id = g_bus_own_name (bus->bus_type, name, flags, NULL,
                                   name_acquired_cb, name_lost_cb,
                                   self->shared, name_shared_unref);

  shell_lua_handle_push (L, "shell.BusName", G_OBJECT (self));
  return 1;
}

static const luaL_Reg bus_methods[] = {
  { "proxy",      l_bus_proxy },
  { "watch_name", l_bus_watch_name },
  { "own_name",   l_bus_own_name },
  { "export",     l_bus_export },
  { NULL, NULL }
};

static const ShellLuaHandleProperty bus_properties[] = {
  { NULL }
};

static ShellLuaHandleType bus_type = {
  .name = "shell.Bus",
  .methods = bus_methods,
  .properties = bus_properties,
};

/* ------------------------------------------------------------------------ */
/* shell.dbus                                                                */
/* ------------------------------------------------------------------------ */

static char session_key, system_key;

/* The Bus of this state for @type: one per state, made on first use. */
static int
push_bus (lua_State *L,
          GBusType   bus_type,
          const char *function)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  void *key = bus_type == G_BUS_TYPE_SYSTEM ? (void *) &system_key : (void *) &session_key;
  ShellLuaBus *bus;

  shell_lua_require_capability (L, "dbus", function);

  if (lua_rawgetp (L, LUA_REGISTRYINDEX, key) != LUA_TNIL && shell_lua_handle_is_alive (L, -1))
    return 1;
  lua_pop (L, 1);

  shell_lua_check_owned_capacity (L);
  bus = g_object_new (SHELL_TYPE_LUA_BUS, NULL);
  bus->state = state;
  bus->bus_type = bus_type;
  bus->cancellable = g_cancellable_new ();
  g_bus_get (bus_type, bus->cancellable, bus_got_connection, g_object_ref (bus));

  shell_lua_own_object (L, G_OBJECT (bus), &bus_ops);
  g_object_unref (bus);

  shell_lua_handle_push (L, "shell.Bus", G_OBJECT (bus));
  lua_pushvalue (L, -1);
  lua_rawsetp (L, LUA_REGISTRYINDEX, key);
  return 1;
}

static int
l_dbus_session (lua_State *L)
{
  return push_bus (L, G_BUS_TYPE_SESSION, "shell.dbus.session()");
}

static int
l_dbus_system (lua_State *L)
{
  return push_bus (L, G_BUS_TYPE_SYSTEM, "shell.dbus.system()");
}

/* shell.dbus.variant(signature, value) */
static int
l_dbus_variant (lua_State *L)
{
  const char *signature;
  GVariant *value;

  shell_lua_require_capability (L, "dbus", "shell.dbus.variant()");
  signature = luaL_checkstring (L, 1);
  luaL_checkany (L, 2);
  check_single_type (L, signature, "signature");

  value = shell_lua_to_variant (L, 2, G_VARIANT_TYPE (signature));
  shell_lua_push_variant_box (L, value);
  g_variant_unref (value);
  return 1;
}

static const luaL_Reg dbus_funcs[] = {
  { "session", l_dbus_session },
  { "system",  l_dbus_system },
  { "variant", l_dbus_variant },
  { NULL, NULL }
};

void
shell_lua_api_dbus_register (ShellLuaState *state,
                             lua_State     *L)
{
  int shell = lua_absindex (L, -1);

  luaL_checkstack (L, 6, "shell dbus api");

  call_type.gtype = SHELL_TYPE_LUA_CALL;
  shell_lua_handle_register_type (L, &call_type);
  proxy_type.gtype = SHELL_TYPE_LUA_PROXY;
  shell_lua_handle_register_type (L, &proxy_type);
  bus_type.gtype = SHELL_TYPE_LUA_BUS;
  shell_lua_handle_register_type (L, &bus_type);
  bus_name_type.gtype = SHELL_TYPE_LUA_BUS_NAME;
  shell_lua_handle_register_type (L, &bus_name_type);
  object_type.gtype = SHELL_TYPE_LUA_DBUS_OBJECT;
  shell_lua_handle_register_type (L, &object_type);
  shell_lua_connection_register_handle_type (L);

  lua_newtable (L);
  luaL_setfuncs (L, dbus_funcs, 0);
  lua_setfield (L, shell, "dbus");
}
