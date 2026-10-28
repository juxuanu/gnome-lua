/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * screensaver-service.c: org.gnome.ScreenSaver for the clients that talk to
 * the session while the shell does not own the name. Port of
 * js/dbusServices/screensaver/screenSaverService.js and main.js.
 *
 * The service exports org.gnome.ScreenSaver at /org/gnome/ScreenSaver and
 * forwards the calls and the signals to the screen shield of the shell, which
 * owns org.gnome.Shell.ScreenShield at the same object path. It does not exit
 * when it is idle, only when org.gnome.Shell leaves the bus.
 *
 * Differences from the JS version: a call that arrives before the proxy of the
 * shell is ready, or after its creation failed, is answered with an error
 * instead of being sent through a proxy that was not initialized.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <stdint.h>

#include <gio/gio.h>

#include "screensaver-service-dbus.h"
#include "shell-dbus-service.h"

#define SCREEN_SAVER_BUS_NAME "org.gnome.ScreenSaver"
#define SCREEN_SAVER_OBJECT_PATH "/org/gnome/ScreenSaver"
#define SHIELD_BUS_NAME "org.gnome.Shell.ScreenShield"

#define SHELL_TYPE_SCREEN_SAVER_FORWARDER (shell_screen_saver_forwarder_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenSaverForwarder, shell_screen_saver_forwarder,
                      SHELL, SCREEN_SAVER_FORWARDER, ShellDBusService)

struct _ShellScreenSaverForwarder
{
  ShellDBusService parent;

  ShellDBusScreenSaver *skeleton;
  ShellDBusScreenSaver *proxy;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (ShellScreenSaverForwarder, shell_screen_saver_forwarder,
                     SHELL_TYPE_DBUS_SERVICE)

typedef struct
{
  ShellScreenSaverForwarder *forwarder;
  GDBusMethodInvocation *invocation;
} CallData;

static CallData *
call_data_new (ShellScreenSaverForwarder *self,
               GDBusMethodInvocation     *invocation)
{
  CallData *data = g_new0 (CallData, 1);

  data->forwarder = g_object_ref (self);
  data->invocation = invocation;

  return data;
}

static void
call_data_free (CallData *data)
{
  g_object_unref (data->forwarder);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CallData, call_data_free)

static gboolean
check_proxy (ShellScreenSaverForwarder *self,
             GDBusMethodInvocation     *invocation)
{
  g_autoptr (GError) error = NULL;

  if (self->proxy != NULL)
    return TRUE;

  g_set_error_literal (&error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN,
                       "The screen shield is not available");
  shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self), invocation, error);
  return FALSE;
}

static void
lock_cb (GObject      *source,
         GAsyncResult *result,
         gpointer      user_data)
{
  g_autoptr (CallData) data = user_data;
  g_autoptr (GError) error = NULL;

  if (!shell_dbus_screen_saver_call_lock_finish (SHELL_DBUS_SCREEN_SAVER (source),
                                                 result, &error))
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (data->forwarder),
                                       data->invocation, error);
      return;
    }

  shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (data->forwarder),
                                       data->invocation);
  shell_dbus_screen_saver_complete_lock (data->forwarder->skeleton,
                                         data->invocation);
}

static gboolean
handle_lock (ShellDBusScreenSaver     *skeleton,
             GDBusMethodInvocation    *invocation,
             ShellScreenSaverForwarder *self)
{
  if (!check_proxy (self, invocation))
    return TRUE;

  shell_dbus_screen_saver_call_lock (self->proxy, self->cancellable,
                                     lock_cb, call_data_new (self, invocation));
  return TRUE;
}

static void
get_active_cb (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr (CallData) data = user_data;
  g_autoptr (GError) error = NULL;
  gboolean active;

  if (!shell_dbus_screen_saver_call_get_active_finish (SHELL_DBUS_SCREEN_SAVER (source),
                                                       &active, result, &error))
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (data->forwarder),
                                       data->invocation, error);
      return;
    }

  shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (data->forwarder),
                                       data->invocation);
  shell_dbus_screen_saver_complete_get_active (data->forwarder->skeleton,
                                               data->invocation, active);
}

static gboolean
handle_get_active (ShellDBusScreenSaver     *skeleton,
                   GDBusMethodInvocation    *invocation,
                   ShellScreenSaverForwarder *self)
{
  if (!check_proxy (self, invocation))
    return TRUE;

  shell_dbus_screen_saver_call_get_active (self->proxy, self->cancellable,
                                           get_active_cb,
                                           call_data_new (self, invocation));
  return TRUE;
}

static void
set_active_cb (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr (CallData) data = user_data;
  g_autoptr (GError) error = NULL;

  if (!shell_dbus_screen_saver_call_set_active_finish (SHELL_DBUS_SCREEN_SAVER (source),
                                                       result, &error))
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (data->forwarder),
                                       data->invocation, error);
      return;
    }

  shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (data->forwarder),
                                       data->invocation);
  shell_dbus_screen_saver_complete_set_active (data->forwarder->skeleton,
                                               data->invocation);
}

static gboolean
handle_set_active (ShellDBusScreenSaver     *skeleton,
                   GDBusMethodInvocation    *invocation,
                   gboolean                  value,
                   ShellScreenSaverForwarder *self)
{
  if (!check_proxy (self, invocation))
    return TRUE;

  shell_dbus_screen_saver_call_set_active (self->proxy, value,
                                           self->cancellable, set_active_cb,
                                           call_data_new (self, invocation));
  return TRUE;
}

static void
get_active_time_cb (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr (CallData) data = user_data;
  g_autoptr (GError) error = NULL;
  uint32_t value;

  if (!shell_dbus_screen_saver_call_get_active_time_finish (SHELL_DBUS_SCREEN_SAVER (source),
                                                            &value, result, &error))
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (data->forwarder),
                                       data->invocation, error);
      return;
    }

  shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (data->forwarder),
                                       data->invocation);
  shell_dbus_screen_saver_complete_get_active_time (data->forwarder->skeleton,
                                                    data->invocation, value);
}

static gboolean
handle_get_active_time (ShellDBusScreenSaver     *skeleton,
                        GDBusMethodInvocation    *invocation,
                        ShellScreenSaverForwarder *self)
{
  if (!check_proxy (self, invocation))
    return TRUE;

  shell_dbus_screen_saver_call_get_active_time (self->proxy, self->cancellable,
                                                get_active_time_cb,
                                                call_data_new (self, invocation));
  return TRUE;
}

static void
proxy_active_changed_cb (ShellDBusScreenSaver     *proxy,
                         gboolean                  new_value,
                         ShellScreenSaverForwarder *self)
{
  shell_dbus_screen_saver_emit_active_changed (self->skeleton, new_value);
}

static void
proxy_wake_up_screen_cb (ShellDBusScreenSaver     *proxy,
                         ShellScreenSaverForwarder *self)
{
  shell_dbus_screen_saver_emit_wake_up_screen (self->skeleton);
}

static void
proxy_ready_cb (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  ShellScreenSaverForwarder *self;
  g_autoptr (ShellDBusScreenSaver) proxy = NULL;
  g_autoptr (GError) error = NULL;

  proxy = shell_dbus_screen_saver_proxy_new_for_bus_finish (result, &error);
  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to connect to %s: %s", SHIELD_BUS_NAME, error->message);
      return;
    }

  self = SHELL_SCREEN_SAVER_FORWARDER (user_data);

  g_signal_connect (proxy, "active-changed",
                    G_CALLBACK (proxy_active_changed_cb), self);
  g_signal_connect (proxy, "wake-up-screen",
                    G_CALLBACK (proxy_wake_up_screen_cb), self);

  self->proxy = g_steal_pointer (&proxy);
}

static void
shell_screen_saver_forwarder_dispose (GObject *object)
{
  ShellScreenSaverForwarder *self = SHELL_SCREEN_SAVER_FORWARDER (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  if (self->proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->proxy, self);

  g_clear_object (&self->proxy);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->skeleton);

  G_OBJECT_CLASS (shell_screen_saver_forwarder_parent_class)->dispose (object);
}

static void
shell_screen_saver_forwarder_constructed (GObject *object)
{
  ShellScreenSaverForwarder *self = SHELL_SCREEN_SAVER_FORWARDER (object);

  G_OBJECT_CLASS (shell_screen_saver_forwarder_parent_class)->constructed (object);

  g_set_object (&self->skeleton,
                SHELL_DBUS_SCREEN_SAVER (shell_dbus_service_get_skeleton (SHELL_DBUS_SERVICE (self))));

  g_signal_connect (self->skeleton, "handle-lock",
                    G_CALLBACK (handle_lock), self);
  g_signal_connect (self->skeleton, "handle-get-active",
                    G_CALLBACK (handle_get_active), self);
  g_signal_connect (self->skeleton, "handle-set-active",
                    G_CALLBACK (handle_set_active), self);
  g_signal_connect (self->skeleton, "handle-get-active-time",
                    G_CALLBACK (handle_get_active_time), self);

  self->cancellable = g_cancellable_new ();
  shell_dbus_screen_saver_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                             G_DBUS_PROXY_FLAGS_NONE,
                                             SHIELD_BUS_NAME,
                                             SCREEN_SAVER_OBJECT_PATH,
                                             self->cancellable,
                                             proxy_ready_cb, self);
}

static void
shell_screen_saver_forwarder_class_init (ShellScreenSaverForwarderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_screen_saver_forwarder_constructed;
  object_class->dispose = shell_screen_saver_forwarder_dispose;
}

static void
shell_screen_saver_forwarder_init (ShellScreenSaverForwarder *self)
{
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (ShellScreenSaverForwarder) service = NULL;
  g_autoptr (ShellDBusScreenSaver) skeleton = NULL;

  skeleton = shell_dbus_screen_saver_skeleton_new ();
  service = g_object_new (SHELL_TYPE_SCREEN_SAVER_FORWARDER,
                          "skeleton", skeleton,
                          "object-path", SCREEN_SAVER_OBJECT_PATH,
                          "auto-shutdown", FALSE,
                          NULL);

  return shell_dbus_service_run (SHELL_DBUS_SERVICE (service),
                                 SCREEN_SAVER_BUS_NAME, argc, argv);
}
