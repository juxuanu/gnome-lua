/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-dbus-service.c: Base class of the out-of-process D-Bus services of the
 * shell. Port of js/dbusServices/dbusService.js.
 *
 * #ShellDBusService owns the object that a service exports (the skeleton made
 * by gdbus-codegen), watches org.gnome.Shell and decides when the process
 * exits:
 *
 *  - The service exits two seconds (IDLE_SHUTDOWN_TIME) after the last
 *    activity if nothing holds it (shell_dbus_service_hold ()) and
 *    #ShellDBusService:auto-shutdown is set. The environment variable
 *    SHELL_DBUS_PERSIST disables the exit on idle.
 *  - A service that emits signals keeps itself alive while a client that has
 *    called it is on the bus (a hold per tracked sender).
 *  - If org.gnome.Shell is not on the bus, the service exits at once (without
 *    #ShellDBusService:auto-shutdown) or when it is idle.
 *
 * Differences from the JS version:
 *
 *  - The JS class patched the `return_*` methods of Gio.DBusMethodInvocation
 *    to learn about the callers and the activity; the C code has no such hook,
 *    so the implementations call shell_dbus_service_track_invocation () (or
 *    shell_dbus_service_return_error ()) right before they complete an
 *    invocation.
 *  - shell_dbus_service_run () replaces `DBusService.runAsync ()` and returns
 *    the exit status of the process.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <stdint.h>
#include <string.h>

#include "shell-dbus-service.h"

#define IDLE_SHUTDOWN_TIME 2 /* s */

#define SHELL_BUS_NAME "org.gnome.Shell"

enum
{
  PROP_0,
  PROP_SKELETON,
  PROP_OBJECT_PATH,
  PROP_AUTO_SHUTDOWN,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  SHUTDOWN,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

typedef struct
{
  GDBusInterfaceSkeleton *skeleton;
  char *object_path;
  gboolean auto_shutdown;
  gboolean has_signals;

  uint32_t hold_count;
  GHashTable *senders; /* char * -> watch id */
  char *shell_name;
  unsigned int shell_watch_id;
  unsigned int shutdown_timeout_id;
} ShellDBusServicePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellDBusService, shell_dbus_service, G_TYPE_OBJECT)

static void queue_shutdown_check (ShellDBusService *self);

static void
maybe_shutdown (ShellDBusService *self)
{
  ShellDBusServicePrivate *priv = shell_dbus_service_get_instance_private (self);

  if (!priv->auto_shutdown)
    return;

  if (g_getenv ("SHELL_DBUS_PERSIST") != NULL)
    return;

  if (priv->shell_name != NULL && priv->hold_count > 0)
    return;

  g_signal_emit (self, signals[SHUTDOWN], 0);
}

static void
shutdown_timeout_cb (gpointer user_data)
{
  ShellDBusService *self = user_data;
  ShellDBusServicePrivate *priv = shell_dbus_service_get_instance_private (self);

  priv->shutdown_timeout_id = 0;
  maybe_shutdown (self);
}

static void
queue_shutdown_check (ShellDBusService *self)
{
  ShellDBusServicePrivate *priv = shell_dbus_service_get_instance_private (self);

  g_clear_handle_id (&priv->shutdown_timeout_id, g_source_remove);

  priv->shutdown_timeout_id =
    g_timeout_add_seconds_once (IDLE_SHUTDOWN_TIME, shutdown_timeout_cb, self);
  g_source_set_name_by_id (priv->shutdown_timeout_id,
                           "[gnome-shell] shutdown_timeout_cb");
}

/**
 * shell_dbus_service_hold:
 * @service: a #ShellDBusService
 *
 * Keeps the process alive until the matching shell_dbus_service_release ().
 */
void
shell_dbus_service_hold (ShellDBusService *service)
{
  ShellDBusServicePrivate *priv;

  g_return_if_fail (SHELL_IS_DBUS_SERVICE (service));

  priv = shell_dbus_service_get_instance_private (service);
  priv->hold_count++;
}

/**
 * shell_dbus_service_release:
 * @service: a #ShellDBusService
 *
 * Drops a hold made by shell_dbus_service_hold ().
 */
void
shell_dbus_service_release (ShellDBusService *service)
{
  ShellDBusServicePrivate *priv;

  g_return_if_fail (SHELL_IS_DBUS_SERVICE (service));

  priv = shell_dbus_service_get_instance_private (service);

  if (priv->hold_count == 0)
    {
      g_critical ("Unmatched call to %s ()", G_STRFUNC);
      return;
    }

  priv->hold_count--;

  if (priv->hold_count == 0)
    queue_shutdown_check (service);
}

/**
 * shell_dbus_service_get_skeleton:
 * @service: a #ShellDBusService
 *
 * Returns: (transfer none): the exported object
 */
GDBusInterfaceSkeleton *
shell_dbus_service_get_skeleton (ShellDBusService *service)
{
  ShellDBusServicePrivate *priv;

  g_return_val_if_fail (SHELL_IS_DBUS_SERVICE (service), NULL);

  priv = shell_dbus_service_get_instance_private (service);
  return priv->skeleton;
}

static void
untrack_sender (ShellDBusService *self,
                const char       *sender)
{
  ShellDBusServicePrivate *priv = shell_dbus_service_get_instance_private (self);
  ShellDBusServiceClass *klass = SHELL_DBUS_SERVICE_GET_CLASS (self);
  g_autofree char *sender_copy = g_strdup (sender);
  gpointer watch_id;

  if (g_hash_table_lookup_extended (priv->senders, sender_copy, NULL, &watch_id))
    {
      g_bus_unwatch_name (GPOINTER_TO_UINT (watch_id));
      g_hash_table_remove (priv->senders, sender_copy);
      shell_dbus_service_release (self);
    }

  if (klass->sender_vanished != NULL)
    klass->sender_vanished (self, sender_copy);
}

static void
sender_vanished_cb (GDBusConnection *connection,
                    const char      *name,
                    gpointer         user_data)
{
  untrack_sender (user_data, name);
}

static void
track_sender (ShellDBusService *self,
              const char       *sender)
{
  ShellDBusServicePrivate *priv = shell_dbus_service_get_instance_private (self);
  GDBusConnection *connection;
  unsigned int watch_id;

  if (sender == NULL || g_hash_table_contains (priv->senders, sender))
    return;

  if (g_strcmp0 (sender, priv->shell_name) == 0)
    return; /* don't track the shell */

  connection = g_dbus_interface_skeleton_get_connection (priv->skeleton);
  if (connection == NULL)
    return;

  shell_dbus_service_hold (self);

  /* The watch is on the connection of the skeleton, which is the session bus
   * connection; g_bus_watch_name_on_connection () has the semantics of the
   * JS watch_name () of the connection */
  watch_id = g_bus_watch_name_on_connection (connection, sender,
                                             G_BUS_NAME_WATCHER_FLAGS_NONE,
                                             NULL, sender_vanished_cb,
                                             self, NULL);
  g_hash_table_insert (priv->senders, g_strdup (sender),
                       GUINT_TO_POINTER (watch_id));
}

/**
 * shell_dbus_service_track_invocation:
 * @service: a #ShellDBusService
 * @invocation: a method invocation that the caller is about to complete
 *
 * Takes note of the activity of the service. This is what the JS version
 * did for every `return_*` of a Gio.DBusMethodInvocation: if the interface of
 * the service has signals, the sender of @invocation is tracked (the process
 * stays alive while it is on the bus), and the idle exit is postponed.
 *
 * Call it right before completing @invocation, which must not be completed
 * already.
 */
void
shell_dbus_service_track_invocation (ShellDBusService      *service,
                                     GDBusMethodInvocation *invocation)
{
  ShellDBusServicePrivate *priv;

  g_return_if_fail (SHELL_IS_DBUS_SERVICE (service));
  g_return_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation));

  priv = shell_dbus_service_get_instance_private (service);

  if (priv->has_signals)
    track_sender (service, g_dbus_method_invocation_get_sender (invocation));

  queue_shutdown_check (service);
}

/**
 * shell_dbus_service_return_error:
 * @service: a #ShellDBusService
 * @invocation: (transfer full): the method invocation to complete
 * @error: the error to return
 *
 * Completes @invocation with @error (`_handleError ()` of the JS class): the
 * "GDBus.Error:" prefix of an error that came from another process is
 * stripped before it is returned again.
 */
void
shell_dbus_service_return_error (ShellDBusService      *service,
                                 GDBusMethodInvocation *invocation,
                                 GError                *error)
{
  g_return_if_fail (SHELL_IS_DBUS_SERVICE (service));
  g_return_if_fail (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_return_if_fail (error != NULL);

  g_dbus_error_strip_remote_error (error);

  shell_dbus_service_track_invocation (service, invocation);
  g_dbus_method_invocation_return_gerror (invocation, error);
}

static void
shell_name_appeared_cb (GDBusConnection *connection,
                        const char      *name,
                        const char      *name_owner,
                        gpointer         user_data)
{
  ShellDBusService *self = user_data;
  ShellDBusServicePrivate *priv = shell_dbus_service_get_instance_private (self);

  g_free (priv->shell_name);
  priv->shell_name = g_strdup (name_owner);
}

static void
shell_name_vanished_cb (GDBusConnection *connection,
                        const char      *name,
                        gpointer         user_data)
{
  ShellDBusService *self = user_data;
  ShellDBusServicePrivate *priv = shell_dbus_service_get_instance_private (self);

  g_clear_pointer (&priv->shell_name, g_free);

  /* For auto-shutdown services, delay shutting down in case the shell
   * reappears */
  if (priv->auto_shutdown)
    queue_shutdown_check (self);
  else
    g_signal_emit (self, signals[SHUTDOWN], 0);
}

static void
shell_dbus_service_constructed (GObject *object)
{
  ShellDBusService *self = SHELL_DBUS_SERVICE (object);
  ShellDBusServicePrivate *priv = shell_dbus_service_get_instance_private (self);
  GDBusInterfaceInfo *info;

  G_OBJECT_CLASS (shell_dbus_service_parent_class)->constructed (object);

  g_assert (priv->skeleton != NULL);
  g_assert (priv->object_path != NULL);

  info = g_dbus_interface_skeleton_get_info (priv->skeleton);
  priv->has_signals = info != NULL &&
                      info->signals != NULL &&
                      info->signals[0] != NULL;

  /* Bail out when not running under gnome-shell */
  priv->shell_watch_id = g_bus_watch_name (G_BUS_TYPE_SESSION,
                                           SHELL_BUS_NAME,
                                           G_BUS_NAME_WATCHER_FLAGS_NONE,
                                           shell_name_appeared_cb,
                                           shell_name_vanished_cb,
                                           self, NULL);

  queue_shutdown_check (self);
}

static void
shell_dbus_service_set_property (GObject      *object,
                                 unsigned int  prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellDBusService *self = SHELL_DBUS_SERVICE (object);
  ShellDBusServicePrivate *priv = shell_dbus_service_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SKELETON:
      g_assert (priv->skeleton == NULL);
      priv->skeleton = g_value_dup_object (value);
      break;

    case PROP_OBJECT_PATH:
      g_assert (priv->object_path == NULL);
      priv->object_path = g_value_dup_string (value);
      break;

    case PROP_AUTO_SHUTDOWN:
      priv->auto_shutdown = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_dbus_service_get_property (GObject      *object,
                                 unsigned int  prop_id,
                                 GValue       *value,
                                 GParamSpec   *pspec)
{
  ShellDBusService *self = SHELL_DBUS_SERVICE (object);
  ShellDBusServicePrivate *priv = shell_dbus_service_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SKELETON:
      g_value_set_object (value, priv->skeleton);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;

    case PROP_AUTO_SHUTDOWN:
      g_value_set_boolean (value, priv->auto_shutdown);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_dbus_service_dispose (GObject *object)
{
  ShellDBusService *self = SHELL_DBUS_SERVICE (object);
  ShellDBusServicePrivate *priv = shell_dbus_service_get_instance_private (self);
  GHashTableIter iter;
  gpointer watch_id;

  g_clear_handle_id (&priv->shutdown_timeout_id, g_source_remove);
  g_clear_handle_id (&priv->shell_watch_id, g_bus_unwatch_name);

  if (priv->senders != NULL)
    {
      g_hash_table_iter_init (&iter, priv->senders);
      while (g_hash_table_iter_next (&iter, NULL, &watch_id))
        g_bus_unwatch_name (GPOINTER_TO_UINT (watch_id));
      g_clear_pointer (&priv->senders, g_hash_table_unref);
    }

  if (priv->skeleton != NULL &&
      g_dbus_interface_skeleton_get_connection (priv->skeleton) != NULL)
    g_dbus_interface_skeleton_unexport (priv->skeleton);

  g_clear_object (&priv->skeleton);

  G_OBJECT_CLASS (shell_dbus_service_parent_class)->dispose (object);
}

static void
shell_dbus_service_finalize (GObject *object)
{
  ShellDBusService *self = SHELL_DBUS_SERVICE (object);
  ShellDBusServicePrivate *priv = shell_dbus_service_get_instance_private (self);

  g_free (priv->object_path);
  g_free (priv->shell_name);

  G_OBJECT_CLASS (shell_dbus_service_parent_class)->finalize (object);
}

static void
shell_dbus_service_class_init (ShellDBusServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_dbus_service_constructed;
  object_class->set_property = shell_dbus_service_set_property;
  object_class->get_property = shell_dbus_service_get_property;
  object_class->dispose = shell_dbus_service_dispose;
  object_class->finalize = shell_dbus_service_finalize;

  /**
   * ShellDBusService:skeleton:
   *
   * The object that the service exports.
   */
  props[PROP_SKELETON] =
    g_param_spec_object ("skeleton", NULL, NULL,
                         G_TYPE_DBUS_INTERFACE_SKELETON,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDBusService:object-path:
   *
   * The object path that the skeleton is exported at.
   */
  props[PROP_OBJECT_PATH] =
    g_param_spec_string ("object-path", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDBusService:auto-shutdown:
   *
   * Whether the service exits when it is idle. The services that must stay on
   * the bus while the shell runs turn it off.
   */
  props[PROP_AUTO_SHUTDOWN] =
    g_param_spec_boolean ("auto-shutdown", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellDBusService::shutdown:
   *
   * The process should exit.
   */
  signals[SHUTDOWN] =
    g_signal_new ("shutdown",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_dbus_service_init (ShellDBusService *self)
{
  ShellDBusServicePrivate *priv = shell_dbus_service_get_instance_private (self);

  priv->senders = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

typedef struct
{
  ShellDBusService *service;
  GMainLoop *loop;
} RunData;

static void
bus_acquired_cb (GDBusConnection *connection,
                 const char      *name,
                 gpointer         user_data)
{
  RunData *data = user_data;
  ShellDBusServicePrivate *priv =
    shell_dbus_service_get_instance_private (data->service);
  g_autoptr (GError) error = NULL;

  if (!g_dbus_interface_skeleton_export (priv->skeleton, connection,
                                         priv->object_path, &error))
    {
      g_warning ("Failed to export %s: %s", priv->object_path, error->message);
      g_main_loop_quit (data->loop);
    }
}

static void
name_lost_cb (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
  RunData *data = user_data;

  g_main_loop_quit (data->loop);
}

/**
 * shell_dbus_service_run:
 * @service: a #ShellDBusService
 * @bus_name: the well-known name of the service
 * @argc: the number of arguments of the process
 * @argv: (array length=argc): the arguments of the process
 *
 * Owns @bus_name on the session bus, exports the object of the service, and
 * runs the main loop until the service asks to exit or the name is lost. The
 * name can be replaced by another process, and it replaces its owner if
 * `--replace` is one of the arguments.
 *
 * Returns: the exit status of the process
 */
int
shell_dbus_service_run (ShellDBusService *service,
                        const char       *bus_name,
                        int               argc,
                        char            **argv)
{
  ShellDBusServiceClass *klass;
  GBusNameOwnerFlags flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  g_autoptr (GMainLoop) loop = NULL;
  RunData data;
  unsigned int owner_id;
  int i;

  g_return_val_if_fail (SHELL_IS_DBUS_SERVICE (service), 1);
  g_return_val_if_fail (bus_name != NULL, 1);

  for (i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--replace") == 0)
        flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;
    }

  loop = g_main_loop_new (NULL, FALSE);
  g_signal_connect_swapped (service, "shutdown",
                            G_CALLBACK (g_main_loop_quit), loop);

  klass = SHELL_DBUS_SERVICE_GET_CLASS (service);
  if (klass->register_names != NULL)
    klass->register_names (service);

  data.service = service;
  data.loop = loop;
  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION, bus_name, flags,
                             bus_acquired_cb, NULL, name_lost_cb,
                             &data, NULL);

  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);
  g_signal_handlers_disconnect_by_data (service, loop);

  return 0;
}
