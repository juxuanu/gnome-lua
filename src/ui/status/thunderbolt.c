/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * thunderbolt.c: The indicator which shows while boltd probes the Thunderbolt
 * devices, and the authorization of the new devices. Port of
 * js/ui/status/thunderbolt.js, which is a modified version of
 * bolt/contrib/js/client.js.
 *
 * The bolt proxies are gdbus-codegen proxies (`bolt-dbus`, types
 * ShellBolt1Manager and ShellBolt1Device). The manager proxy is created with
 * DO_NOT_AUTO_START: without boltd the indicator stays hidden, quietly.
 *
 * `Client` and `AuthRobot` (two `Signals.EventEmitter` classes that only
 * the indicator used, wired together with signals) are folded into the
 * indicator: the proxy and its two handlers are the client, the queue of the
 * devices to enroll and the idle that works on it are the robot.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - After a device could not be enrolled the queue goes on with the next
 *    one; in the JS the robot never enrolled again.
 *  - The queue is dropped on ShellGlobal::shutdown, and the pending
 *    enrollment is cancelled.
 *  - Without a message tray (`Main.messageTray`) or a source no
 *    notification is shown.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <polkit/polkit.h>

#include "shell-app-system.h"
#include "shell-app.h"
#include "shell-global.h"
#include "ui/message-tray.h"
#include "ui/status/status-private.h"
#include "ui/status/thunderbolt.h"

#include "bolt-dbus.h"

#define BOLT_DBUS_NAME "org.freedesktop.bolt"
#define BOLT_DBUS_PATH "/org/freedesktop/bolt"

#define BOLT_ENROLL_ACTION "org.freedesktop.bolt.enroll"

/* Status, Policy, AuthCtrl, AuthMode of the bolt API */
#define STATUS_CONNECTED "connected"
#define POLICY_DEFAULT "default"
#define AUTH_CTRL_NONE "none"
#define AUTH_MODE_ENABLED "enabled"

struct _ShellThunderboltIndicator
{
  ShellSystemIndicator parent;

  StIcon *icon;                         /* weak */

  /* Bolt.Client */
  ShellBolt1Manager *proxy;
  GCancellable *cancellable;
  gboolean probing;

  /* AuthRobot */
  GQueue devices_to_enroll;             /* ShellBolt1Device */
  gboolean enrolling;
  guint enroll_idle_id;

  GPermission *perm;
  ShellNotification *notification;      /* weak */
};

G_DEFINE_FINAL_TYPE (ShellThunderboltIndicator, shell_thunderbolt_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static gboolean
session_is_unlocked (void)
{
  ShellSessionMode *session_mode = shell_status_get_session_mode ();

  return session_mode == NULL ||
         (!shell_session_mode_is_locked (session_mode) &&
          !shell_session_mode_is_greeter (session_mode));
}

/* _sync () */
static void
indicator_sync (ShellThunderboltIndicator *self)
{
  if (self->icon == NULL)
    return;

  shell_status_set_visible (self->icon, session_is_unlocked () && self->probing);
}

static void
notification_activated_cb (ShellNotification *notification,
                           gpointer           user_data)
{
  ShellApp *app = shell_app_system_lookup_app (shell_app_system_get_default (),
                                               "gnome-thunderbolt-panel.desktop");

  if (app != NULL)
    shell_app_activate (app);
}

static void
notification_destroyed_cb (ShellNotification         *notification,
                           ShellNotificationDestroyedReason reason,
                           ShellThunderboltIndicator *self)
{
  g_clear_weak_pointer (&self->notification);
}

/* _notify () */
static void
indicator_notify (ShellThunderboltIndicator *self,
                  const char                *title,
                  const char                *body)
{
  ShellMessageTray *message_tray = shell_main_get_message_tray (shell_main_get_default ());
  ShellSource *source;
  ShellNotification *notification;

  if (self->notification != NULL)
    shell_notification_destroy (self->notification,
                                SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);

  if (message_tray == NULL)
    return;

  source = shell_message_tray_get_system_source (message_tray);
  if (source == NULL)
    return;

  notification = shell_notification_new (source, title, body);
  shell_notification_set_icon_name (notification, "thunderbolt-symbolic");
  shell_notification_set_urgency (notification, SHELL_NOTIFICATION_URGENCY_HIGH);
  g_set_weak_pointer (&self->notification, notification);
  g_signal_connect_object (notification, "destroy",
                           G_CALLBACK (notification_destroyed_cb), self, 0);
  g_signal_connect (notification, "activated",
                    G_CALLBACK (notification_activated_cb), NULL);
  shell_source_add_notification (source, notification);
}

/* AuthRobot._onEnrollDevice (): whether the device is to be authorized */
static gboolean
indicator_should_enroll_device (ShellThunderboltIndicator *self,
                                ShellBolt1Device          *device)
{
  /* only authorize new devices when in an unlocked user session */
  gboolean unlocked = session_is_unlocked ();
  /* and if we have the permission to do so, otherwise we trigger a PolKit dialog */
  gboolean allowed = self->perm != NULL && g_permission_get_allowed (self->perm);
  gboolean auth = unlocked && allowed;

  g_message ("thunderbolt: [%s] auto enrollment: %s (allowed: %s)",
             shell_bolt1_device_get_name (device),
             auth ? "yes" : "no",
             allowed ? "yes" : "no");

  if (auth)
    return TRUE; /* we are done */

  if (!unlocked)
    {
      indicator_notify (self,
                        _("Unknown Thunderbolt device"),
                        _("New device has been detected while you were away. Please disconnect and reconnect the device to start using it."));
    }
  else
    {
      indicator_notify (self,
                        _("Unauthorized Thunderbolt device"),
                        _("New device has been detected and needs to be authorized by an administrator"));
    }

  return FALSE;
}

static void
enroll_devices_idle_cb (gpointer user_data);

static void
enroll_schedule (ShellThunderboltIndicator *self)
{
  self->enrolling = TRUE;
  self->enroll_idle_id = g_idle_add_once (enroll_devices_idle_cb, self);
}

static void
enroll_device_cb (GObject      *source,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  g_autoptr (ShellThunderboltIndicator) self = user_data;
  g_autoptr (GError) error = NULL;

  if (!shell_bolt1_manager_call_enroll_device_finish (SHELL_BOLT1_MANAGER (source),
                                                      NULL, res, &error))
    {
      g_autofree char *body = NULL;

      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      /* 'enroll-failed' */
      g_dbus_error_strip_remote_error (error);
      body = g_strdup_printf (_("Could not authorize the Thunderbolt device: %s"),
                              error->message);
      indicator_notify (self, _("Thunderbolt authorization error"), body);
    }

  /* TODO: scan the list of devices to be authorized for children
   *  of this device and remove them (and their children and
   *  their children and ....) from the device queue
   */
  self->enrolling = !g_queue_is_empty (&self->devices_to_enroll);

  if (self->enrolling)
    enroll_schedule (self);
}

/* AuthRobot._enrollDevicesIdle () */
static void
enroll_devices_idle_cb (gpointer user_data)
{
  ShellThunderboltIndicator *self = user_data;
  g_autoptr (ShellBolt1Device) device = NULL;

  self->enroll_idle_id = 0;

  device = g_queue_pop_head (&self->devices_to_enroll);
  if (device == NULL || self->proxy == NULL)
    {
      self->enrolling = FALSE;
      return;
    }

  shell_bolt1_manager_call_enroll_device (self->proxy,
                                          shell_bolt1_device_get_uid (device),
                                          POLICY_DEFAULT, AUTH_CTRL_NONE,
                                          self->cancellable,
                                          enroll_device_cb,
                                          g_object_ref (self));
}

/* AuthRobot._enrollDevices (): an idle callback keeps calling itself as
 * long as there are devices to be enrolled */
static void
enroll_devices (ShellThunderboltIndicator *self)
{
  if (self->enrolling)
    return;

  enroll_schedule (self);
}

/* AuthRobot._onDeviceAdded (): the "device-added" signal will be emitted by
 * boltd for every device that is not currently stored in the database. We
 * are only interested in those devices, because all known devices will be
 * handled by the user himself */
static void
indicator_device_added (ShellThunderboltIndicator *self,
                        ShellBolt1Device          *device)
{
  g_auto (GStrv) auth_modes = NULL;

  if (g_strcmp0 (shell_bolt1_device_get_status (device), STATUS_CONNECTED) != 0)
    return;

  /* check if authorization is enabled in the daemon. if not
   * we won't even bother authorizing, because we will only
   * get an error back. The exact contents of AuthMode might
   * change in the future, but must contain AuthMode.ENABLED
   * if it is enabled. */
  if (self->proxy == NULL)
    return;

  auth_modes = g_strsplit (shell_bolt1_manager_get_auth_mode (self->proxy), "|", -1);
  if (!g_strv_contains ((const char * const *) auth_modes, AUTH_MODE_ENABLED))
    return;

  /* check if we should enroll the device */
  if (!indicator_should_enroll_device (self, device))
    return;

  /* ok, we should authorize the device, add it to the back
   * of the list  */
  g_queue_push_tail (&self->devices_to_enroll, g_object_ref (device));
  enroll_devices (self);
}

/* Client._onDeviceAdded () */
static void
proxy_device_added_cb (ShellBolt1Manager         *proxy,
                       const char                *path,
                       ShellThunderboltIndicator *self)
{
  g_autoptr (ShellBolt1Device) device = NULL;
  g_autoptr (GError) error = NULL;

  device = shell_bolt1_device_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                      G_DBUS_PROXY_FLAGS_NONE,
                                                      BOLT_DBUS_NAME, path,
                                                      NULL, &error);
  if (device == NULL)
    {
      g_message ("%s: error creating bolt device proxy: %s", G_STRFUNC, error->message);
      return;
    }

  indicator_device_added (self, device);
}

/* _onProbing () */
static void
indicator_set_probing (ShellThunderboltIndicator *self,
                       gboolean                   probing)
{
  self->probing = probing;

  if (self->icon != NULL)
    st_icon_set_icon_name (self->icon,
                           probing ? "thunderbolt-acquiring-symbolic"
                                   : "thunderbolt-symbolic");

  indicator_sync (self);
}

/* Client._onPropertiesChanged () */
static void
proxy_properties_changed_cb (GDBusProxy                *proxy,
                             GVariant                  *changed_properties,
                             const char * const        *invalidated_properties,
                             ShellThunderboltIndicator *self)
{
  g_autoptr (GVariant) value = g_variant_lookup_value (changed_properties, "Probing", NULL);

  if (value != NULL)
    indicator_set_probing (self, shell_bolt1_manager_get_probing (self->proxy));
}

/* Client._getProxy () */
static void
proxy_ready_cb (GObject      *source,
                GAsyncResult *res,
                gpointer      user_data)
{
  g_autoptr (ShellThunderboltIndicator) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (ShellBolt1Manager) proxy = NULL;

  proxy = shell_bolt1_manager_proxy_new_for_bus_finish (res, &error);

  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          if (shell_status_is_service_missing_error (error))
            g_debug ("%s: %s", G_STRFUNC, error->message);
          else
            g_message ("error creating bolt proxy: %s", error->message);
        }
      return;
    }

  if (self->cancellable == NULL || g_cancellable_is_cancelled (self->cancellable))
    return;

  self->proxy = g_steal_pointer (&proxy);
  g_signal_connect_object (self->proxy, "g-properties-changed",
                           G_CALLBACK (proxy_properties_changed_cb), self, 0);
  g_signal_connect_object (self->proxy, "device-added",
                           G_CALLBACK (proxy_device_added_cb), self, 0);

  if (shell_bolt1_manager_get_probing (self->proxy))
    indicator_set_probing (self, TRUE);
}

static void
permission_ready_cb (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  g_autoptr (ShellThunderboltIndicator) self = user_data;
  g_autoptr (GError) error = NULL;
  GPermission *perm;

  perm = polkit_permission_new_finish (res, &error);
  if (perm == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Failed to get PolKit permission: %s", error->message);
      return;
    }

  if (self->cancellable == NULL || g_cancellable_is_cancelled (self->cancellable))
    {
      g_object_unref (perm);
      return;
    }

  g_set_object (&self->perm, perm);
  g_object_unref (perm);
}

/* Client.close () and AuthRobot.close (), and the pending work */
static void
indicator_release (ShellThunderboltIndicator *self)
{
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_handle_id (&self->enroll_idle_id, g_source_remove);
  g_queue_clear_full (&self->devices_to_enroll, g_object_unref);
  self->enrolling = FALSE;
  g_clear_object (&self->proxy);
}

static void
on_shutdown (ShellGlobal                *global,
             ShellThunderboltIndicator *self)
{
  indicator_release (self);
}

static void
shell_thunderbolt_indicator_constructed (GObject *object)
{
  ShellThunderboltIndicator *self = SHELL_THUNDERBOLT_INDICATOR (object);
  ShellSessionMode *session_mode = shell_status_get_session_mode ();
  StIcon *icon;

  G_OBJECT_CLASS (shell_thunderbolt_indicator_parent_class)->constructed (object);

  icon = shell_system_indicator_add_indicator (SHELL_SYSTEM_INDICATOR (self));
  g_set_weak_pointer (&self->icon, icon);
  st_icon_set_icon_name (icon, "thunderbolt-symbolic");

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  self->cancellable = g_cancellable_new ();
  shell_bolt1_manager_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                         G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                         BOLT_DBUS_NAME, BOLT_DBUS_PATH,
                                         self->cancellable,
                                         proxy_ready_cb,
                                         g_object_ref (self));

  if (session_mode != NULL)
    g_signal_connect_object (session_mode, "updated",
                             G_CALLBACK (indicator_sync), self, G_CONNECT_SWAPPED);
  indicator_sync (self);

  polkit_permission_new (BOLT_ENROLL_ACTION, NULL, self->cancellable,
                         permission_ready_cb, g_object_ref (self));
}

static void
shell_thunderbolt_indicator_dispose (GObject *object)
{
  ShellThunderboltIndicator *self = SHELL_THUNDERBOLT_INDICATOR (object);

  indicator_release (self);
  g_clear_object (&self->perm);
  g_clear_weak_pointer (&self->icon);
  g_clear_weak_pointer (&self->notification);

  G_OBJECT_CLASS (shell_thunderbolt_indicator_parent_class)->dispose (object);
}

static void
shell_thunderbolt_indicator_class_init (ShellThunderboltIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_thunderbolt_indicator_constructed;
  object_class->dispose = shell_thunderbolt_indicator_dispose;
}

static void
shell_thunderbolt_indicator_init (ShellThunderboltIndicator *self)
{
  g_queue_init (&self->devices_to_enroll);
}

/**
 * shell_thunderbolt_indicator_new:
 *
 * Creates the indicator which shows while boltd probes the Thunderbolt
 * devices, and which authorizes the new devices if it is allowed to
 * (`new Indicator ()`).
 *
 * Returns: (transfer floating): a new #ShellThunderboltIndicator
 */
ShellThunderboltIndicator *
shell_thunderbolt_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_THUNDERBOLT_INDICATOR, NULL);
}
