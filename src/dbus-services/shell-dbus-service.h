/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-dbus-service.h: Base class of the out-of-process D-Bus services of the
 * shell (org.gnome.Shell.Screencast, org.gnome.ScreenSaver and
 * org.gnome.Shell.Notifications). Port of js/dbusServices/dbusService.js
 * (`ServiceImplementation` and `DBusService`).
 *
 * The services run in their own process, outside of the compositor, and are
 * linked with GLib and GIO only.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define SHELL_TYPE_DBUS_SERVICE (shell_dbus_service_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellDBusService, shell_dbus_service,
                          SHELL, DBUS_SERVICE, GObject)

/**
 * ShellDBusServiceClass:
 * @register_names: called before the bus name of the service is requested;
 *   subclasses may own additional names (`register ()` of the JS class)
 * @sender_vanished: called when a bus name that called the service vanishes,
 *   whether it was tracked or not (`_untrackSender ()` of the JS class)
 */
struct _ShellDBusServiceClass
{
  GObjectClass parent_class;

  void (* register_names)  (ShellDBusService *service);
  void (* sender_vanished) (ShellDBusService *service,
                            const char       *sender);
};

void     shell_dbus_service_hold  (ShellDBusService *service);
void     shell_dbus_service_release (ShellDBusService *service);

GDBusInterfaceSkeleton *shell_dbus_service_get_skeleton (ShellDBusService *service);

void     shell_dbus_service_track_invocation (ShellDBusService       *service,
                                              GDBusMethodInvocation  *invocation);

void     shell_dbus_service_return_error (ShellDBusService       *service,
                                          GDBusMethodInvocation  *invocation,
                                          GError                 *error);

int      shell_dbus_service_run (ShellDBusService *service,
                                 const char       *bus_name,
                                 int               argc,
                                 char            **argv);

G_END_DECLS
