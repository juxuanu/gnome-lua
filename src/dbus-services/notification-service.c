/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * notification-service.c: The org.freedesktop.Notifications forwarder. Port of
 * js/dbusServices/notifications/notificationDaemon.js and main.js.
 *
 * The process owns org.gnome.Shell.Notifications and, with REPLACE, the
 * well-known name org.freedesktop.Notifications, and exports
 * /org/freedesktop/Notifications. The calls of the applications are forwarded
 * to the notification daemon of the shell (org.gnome.Shell at the same object
 * path) with two more hints, `x-shell-sender-pid` and `x-shell-sender`. The
 * signals of the shell are sent only to the application that owns the
 * notification. A notification belongs to the sender of the Notify call that
 * created it, and only that sender may replace or close it.
 *
 * The service does not exit when it is idle, only when org.gnome.Shell leaves
 * the bus.
 *
 * Differences from the JS version:
 *
 *  - a failure to get the process id of the sender is returned as the error of
 *    the call (the JS function threw and never replied);
 *  - a call that arrives before the proxy of the shell is ready, or after its
 *    creation failed, is answered with an error.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <stdint.h>

#include <gio/gio.h>

#include "notification-service-dbus.h"
#include "shell-dbus-service.h"

#define NOTIFICATIONS_BUS_NAME "org.gnome.Shell.Notifications"
#define FDO_NOTIFICATIONS_BUS_NAME "org.freedesktop.Notifications"
#define FDO_NOTIFICATIONS_OBJECT_PATH "/org/freedesktop/Notifications"
#define SHELL_BUS_NAME "org.gnome.Shell"

#define SHELL_TYPE_NOTIFICATION_FORWARDER (shell_notification_forwarder_get_type ())
G_DECLARE_FINAL_TYPE (ShellNotificationForwarder, shell_notification_forwarder,
                      SHELL, NOTIFICATION_FORWARDER, ShellDBusService)

struct _ShellNotificationForwarder
{
  ShellDBusService parent;

  ShellFreedesktopNotifications *skeleton;
  ShellFreedesktopNotifications *proxy;
  GCancellable *cancellable;
  unsigned int fdo_name_id;

  GHashTable *active_notifications; /* uint32_t id -> char * sender */
};

G_DEFINE_FINAL_TYPE (ShellNotificationForwarder, shell_notification_forwarder,
                     SHELL_TYPE_DBUS_SERVICE)

typedef struct
{
  ShellNotificationForwarder *forwarder;
  GDBusMethodInvocation *invocation;
  char *sender;

  /* The arguments of Notify */
  char *app_name;
  uint32_t replaces_id;
  char *app_icon;
  char *summary;
  char *body;
  char **actions;
  GVariant *hints;
  int32_t expire_timeout;
} CallData;

static CallData *
call_data_new (ShellNotificationForwarder *self,
               GDBusMethodInvocation      *invocation)
{
  CallData *data = g_new0 (CallData, 1);

  data->forwarder = g_object_ref (self);
  data->invocation = invocation;
  data->sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));

  return data;
}

static void
call_data_free (CallData *data)
{
  g_object_unref (data->forwarder);
  g_free (data->sender);
  g_free (data->app_name);
  g_free (data->app_icon);
  g_free (data->summary);
  g_free (data->body);
  g_strfreev (data->actions);
  g_clear_pointer (&data->hints, g_variant_unref);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CallData, call_data_free)

static gboolean
check_proxy (ShellNotificationForwarder *self,
             GDBusMethodInvocation      *invocation)
{
  g_autoptr (GError) error = NULL;

  if (self->proxy != NULL)
    return TRUE;

  g_set_error_literal (&error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN,
                       "The notification daemon of the shell is not available");
  shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self), invocation, error);
  return FALSE;
}

static void
emit_signal_to_sender (ShellNotificationForwarder *self,
                       const char                 *sender,
                       const char                 *signal_name,
                       GVariant                   *params)
{
  GDBusInterfaceSkeleton *skeleton =
    shell_dbus_service_get_skeleton (SHELL_DBUS_SERVICE (self));
  GDBusConnection *connection;
  g_autoptr (GError) error = NULL;

  g_variant_ref_sink (params);

  if (sender == NULL)
    {
      g_variant_unref (params);
      return;
    }

  connection = g_dbus_interface_skeleton_get_connection (skeleton);
  if (connection != NULL &&
      !g_dbus_connection_emit_signal (connection, sender,
                                      FDO_NOTIFICATIONS_OBJECT_PATH,
                                      "org.freedesktop.Notifications",
                                      signal_name, params, &error))
    g_warning ("Failed to emit %s to %s: %s", signal_name, sender, error->message);

  g_variant_unref (params);
}

/* Is the sender of @invocation allowed to use the notification @id? Completes
 * the invocation with an error if it is not. */
static gboolean
check_notification_id (ShellNotificationForwarder *self,
                       GDBusMethodInvocation      *invocation,
                       uint32_t                    id)
{
  const char *owner;
  g_autoptr (GError) error = NULL;

  if (id == 0)
    return TRUE;

  owner = g_hash_table_lookup (self->active_notifications, GUINT_TO_POINTER (id));
  if (owner == NULL)
    return TRUE;

  if (g_strcmp0 (owner, g_dbus_method_invocation_get_sender (invocation)) == 0)
    return TRUE;

  g_set_error_literal (&error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                       "Invalid notification ID");
  shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self), invocation, error);
  return FALSE;
}

static void
proxy_activation_token_cb (ShellFreedesktopNotifications *proxy,
                           uint32_t                       id,
                           const char                    *token,
                           ShellNotificationForwarder    *self)
{
  emit_signal_to_sender (self,
                         g_hash_table_lookup (self->active_notifications,
                                              GUINT_TO_POINTER (id)),
                         "ActivationToken",
                         g_variant_new ("(us)", id, token));
}

static void
proxy_action_invoked_cb (ShellFreedesktopNotifications *proxy,
                         uint32_t                       id,
                         const char                    *action_key,
                         ShellNotificationForwarder    *self)
{
  emit_signal_to_sender (self,
                         g_hash_table_lookup (self->active_notifications,
                                              GUINT_TO_POINTER (id)),
                         "ActionInvoked",
                         g_variant_new ("(us)", id, action_key));
}

static void
proxy_notification_closed_cb (ShellFreedesktopNotifications *proxy,
                              uint32_t                       id,
                              uint32_t                       reason,
                              ShellNotificationForwarder    *self)
{
  emit_signal_to_sender (self,
                         g_hash_table_lookup (self->active_notifications,
                                              GUINT_TO_POINTER (id)),
                         "NotificationClosed",
                         g_variant_new ("(uu)", id, reason));
  g_hash_table_remove (self->active_notifications, GUINT_TO_POINTER (id));
}

/* Notify */

static void
notify_cb (GObject      *source,
           GAsyncResult *result,
           gpointer      user_data)
{
  g_autoptr (CallData) data = user_data;
  ShellNotificationForwarder *self = data->forwarder;
  g_autoptr (GError) error = NULL;
  uint32_t id;

  if (!shell_freedesktop_notifications_call_notify_finish (SHELL_FREEDESKTOP_NOTIFICATIONS (source),
                                                           &id, result, &error))
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self),
                                       data->invocation, error);
      return;
    }

  g_hash_table_insert (self->active_notifications, GUINT_TO_POINTER (id),
                       g_strdup (data->sender));

  shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (self),
                                       data->invocation);
  shell_freedesktop_notifications_complete_notify (self->skeleton,
                                                   data->invocation, id);
}

static void
sender_pid_cb (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr (CallData) data = user_data;
  ShellNotificationForwarder *self = data->forwarder;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariantDict) new_hints = NULL;
  CallData *call;
  uint32_t pid;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result,
                                         &error);
  if (reply == NULL)
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self),
                                       data->invocation, error);
      return;
    }

  g_variant_get (reply, "(u)", &pid);

  if (!check_notification_id (self, data->invocation, data->replaces_id))
    return;

  call = g_steal_pointer (&data);

  new_hints = g_variant_dict_new (call->hints);
  g_variant_dict_insert (new_hints, "x-shell-sender-pid", "u", pid);
  g_variant_dict_insert (new_hints, "x-shell-sender", "s", call->sender);

  shell_freedesktop_notifications_call_notify (self->proxy,
                                               call->app_name,
                                               call->replaces_id,
                                               call->app_icon,
                                               call->summary,
                                               call->body,
                                               (const char * const *) call->actions,
                                               g_variant_dict_end (new_hints),
                                               call->expire_timeout,
                                               self->cancellable,
                                               notify_cb,
                                               call);
}

static gboolean
handle_notify (ShellFreedesktopNotifications *skeleton,
               GDBusMethodInvocation         *invocation,
               const char                    *app_name,
               uint32_t                       replaces_id,
               const char                    *app_icon,
               const char                    *summary,
               const char                    *body,
               const char * const            *actions,
               GVariant                      *hints,
               int32_t                        expire_timeout,
               ShellNotificationForwarder    *self)
{
  CallData *data;
  GDBusConnection *connection;

  if (!check_proxy (self, invocation))
    return TRUE;

  data = call_data_new (self, invocation);
  data->app_name = g_strdup (app_name);
  data->replaces_id = replaces_id;
  data->app_icon = g_strdup (app_icon);
  data->summary = g_strdup (summary);
  data->body = g_strdup (body);
  data->actions = g_strdupv ((char **) actions);
  data->hints = g_variant_ref (hints);
  data->expire_timeout = expire_timeout;

  connection = g_dbus_method_invocation_get_connection (invocation);
  g_dbus_connection_call (connection,
                          "org.freedesktop.DBus",
                          "/",
                          "org.freedesktop.DBus",
                          "GetConnectionUnixProcessID",
                          g_variant_new ("(s)", data->sender),
                          G_VARIANT_TYPE ("(u)"),
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          self->cancellable,
                          sender_pid_cb,
                          data);
  return TRUE;
}

/* CloseNotification */

static void
close_notification_cb (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr (CallData) data = user_data;
  ShellNotificationForwarder *self = data->forwarder;
  g_autoptr (GError) error = NULL;

  if (!shell_freedesktop_notifications_call_close_notification_finish (SHELL_FREEDESKTOP_NOTIFICATIONS (source),
                                                                       result, &error))
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self),
                                       data->invocation, error);
      return;
    }

  shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (self),
                                       data->invocation);
  shell_freedesktop_notifications_complete_close_notification (self->skeleton,
                                                               data->invocation);
}

static gboolean
handle_close_notification (ShellFreedesktopNotifications *skeleton,
                           GDBusMethodInvocation         *invocation,
                           uint32_t                       id,
                           ShellNotificationForwarder    *self)
{
  if (!check_proxy (self, invocation) ||
      !check_notification_id (self, invocation, id))
    return TRUE;

  shell_freedesktop_notifications_call_close_notification (self->proxy, id,
                                                           self->cancellable,
                                                           close_notification_cb,
                                                           call_data_new (self, invocation));
  return TRUE;
}

/* GetCapabilities */

static void
get_capabilities_cb (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  g_autoptr (CallData) data = user_data;
  ShellNotificationForwarder *self = data->forwarder;
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) capabilities = NULL;

  if (!shell_freedesktop_notifications_call_get_capabilities_finish (SHELL_FREEDESKTOP_NOTIFICATIONS (source),
                                                                     &capabilities, result, &error))
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self),
                                       data->invocation, error);
      return;
    }

  shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (self),
                                       data->invocation);
  shell_freedesktop_notifications_complete_get_capabilities (self->skeleton,
                                                             data->invocation,
                                                             (const char * const *) capabilities);
}

static gboolean
handle_get_capabilities (ShellFreedesktopNotifications *skeleton,
                         GDBusMethodInvocation         *invocation,
                         ShellNotificationForwarder    *self)
{
  if (!check_proxy (self, invocation))
    return TRUE;

  shell_freedesktop_notifications_call_get_capabilities (self->proxy,
                                                         self->cancellable,
                                                         get_capabilities_cb,
                                                         call_data_new (self, invocation));
  return TRUE;
}

/* GetServerInformation */

static void
get_server_information_cb (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  g_autoptr (CallData) data = user_data;
  ShellNotificationForwarder *self = data->forwarder;
  g_autoptr (GError) error = NULL;
  g_autofree char *name = NULL;
  g_autofree char *vendor = NULL;
  g_autofree char *version = NULL;
  g_autofree char *spec_version = NULL;

  if (!shell_freedesktop_notifications_call_get_server_information_finish (SHELL_FREEDESKTOP_NOTIFICATIONS (source),
                                                                           &name, &vendor,
                                                                           &version, &spec_version,
                                                                           result, &error))
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self),
                                       data->invocation, error);
      return;
    }

  shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (self),
                                       data->invocation);
  shell_freedesktop_notifications_complete_get_server_information (self->skeleton,
                                                                   data->invocation,
                                                                   name, vendor,
                                                                   version, spec_version);
}

static gboolean
handle_get_server_information (ShellFreedesktopNotifications *skeleton,
                               GDBusMethodInvocation         *invocation,
                               ShellNotificationForwarder    *self)
{
  if (!check_proxy (self, invocation))
    return TRUE;

  shell_freedesktop_notifications_call_get_server_information (self->proxy,
                                                               self->cancellable,
                                                               get_server_information_cb,
                                                               call_data_new (self, invocation));
  return TRUE;
}

static void
proxy_ready_cb (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  ShellNotificationForwarder *self;
  g_autoptr (ShellFreedesktopNotifications) proxy = NULL;
  g_autoptr (GError) error = NULL;

  proxy = shell_freedesktop_notifications_proxy_new_for_bus_finish (result, &error);
  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to connect to %s: %s", SHELL_BUS_NAME, error->message);
      return;
    }

  self = SHELL_NOTIFICATION_FORWARDER (user_data);

  g_signal_connect (proxy, "activation-token",
                    G_CALLBACK (proxy_activation_token_cb), self);
  g_signal_connect (proxy, "action-invoked",
                    G_CALLBACK (proxy_action_invoked_cb), self);
  g_signal_connect (proxy, "notification-closed",
                    G_CALLBACK (proxy_notification_closed_cb), self);

  self->proxy = g_steal_pointer (&proxy);
}

static void
shell_notification_forwarder_register_names (ShellDBusService *service)
{
  ShellNotificationForwarder *self = SHELL_NOTIFICATION_FORWARDER (service);

  self->fdo_name_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                      FDO_NOTIFICATIONS_BUS_NAME,
                                      G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                      NULL, NULL, NULL, NULL, NULL);
}

static void
shell_notification_forwarder_sender_vanished (ShellDBusService *service,
                                              const char       *sender)
{
  ShellNotificationForwarder *self = SHELL_NOTIFICATION_FORWARDER (service);
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, self->active_notifications);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (g_strcmp0 (value, sender) == 0)
        g_hash_table_iter_remove (&iter);
    }
}

static void
shell_notification_forwarder_dispose (GObject *object)
{
  ShellNotificationForwarder *self = SHELL_NOTIFICATION_FORWARDER (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  if (self->proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->proxy, self);

  g_clear_handle_id (&self->fdo_name_id, g_bus_unown_name);
  g_clear_object (&self->proxy);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->skeleton);

  G_OBJECT_CLASS (shell_notification_forwarder_parent_class)->dispose (object);
}

static void
shell_notification_forwarder_finalize (GObject *object)
{
  ShellNotificationForwarder *self = SHELL_NOTIFICATION_FORWARDER (object);

  g_clear_pointer (&self->active_notifications, g_hash_table_unref);

  G_OBJECT_CLASS (shell_notification_forwarder_parent_class)->finalize (object);
}

static void
shell_notification_forwarder_constructed (GObject *object)
{
  ShellNotificationForwarder *self = SHELL_NOTIFICATION_FORWARDER (object);

  G_OBJECT_CLASS (shell_notification_forwarder_parent_class)->constructed (object);

  g_set_object (&self->skeleton,
                SHELL_FREEDESKTOP_NOTIFICATIONS (shell_dbus_service_get_skeleton (SHELL_DBUS_SERVICE (self))));

  g_signal_connect (self->skeleton, "handle-notify",
                    G_CALLBACK (handle_notify), self);
  g_signal_connect (self->skeleton, "handle-close-notification",
                    G_CALLBACK (handle_close_notification), self);
  g_signal_connect (self->skeleton, "handle-get-capabilities",
                    G_CALLBACK (handle_get_capabilities), self);
  g_signal_connect (self->skeleton, "handle-get-server-information",
                    G_CALLBACK (handle_get_server_information), self);

  self->cancellable = g_cancellable_new ();
  shell_freedesktop_notifications_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     SHELL_BUS_NAME,
                                                     FDO_NOTIFICATIONS_OBJECT_PATH,
                                                     self->cancellable,
                                                     proxy_ready_cb, self);
}

static void
shell_notification_forwarder_class_init (ShellNotificationForwarderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellDBusServiceClass *service_class = SHELL_DBUS_SERVICE_CLASS (klass);

  object_class->constructed = shell_notification_forwarder_constructed;
  object_class->dispose = shell_notification_forwarder_dispose;
  object_class->finalize = shell_notification_forwarder_finalize;

  service_class->register_names = shell_notification_forwarder_register_names;
  service_class->sender_vanished = shell_notification_forwarder_sender_vanished;
}

static void
shell_notification_forwarder_init (ShellNotificationForwarder *self)
{
  self->active_notifications = g_hash_table_new_full (NULL, NULL, NULL, g_free);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (ShellNotificationForwarder) service = NULL;
  g_autoptr (ShellFreedesktopNotifications) skeleton = NULL;

  skeleton = shell_freedesktop_notifications_skeleton_new ();
  service = g_object_new (SHELL_TYPE_NOTIFICATION_FORWARDER,
                          "skeleton", skeleton,
                          "object-path", FDO_NOTIFICATIONS_OBJECT_PATH,
                          "auto-shutdown", FALSE,
                          NULL);

  return shell_dbus_service_run (SHELL_DBUS_SERVICE (service),
                                 NOTIFICATIONS_BUS_NAME, argc, argv);
}
