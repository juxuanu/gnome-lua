/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * notification-daemon.h: The notification services of the shell: the
 * org.freedesktop.Notifications object that the notification helper process
 * forwards the calls of the applications to, and the org.gtk.Notifications
 * service that GApplication uses. Port of js/ui/notificationDaemon.js.
 *
 * Only #ShellNotificationDaemon is public (it was the only export that was
 * used, by ShellMain); the two daemons, the sources and the notifications they
 * create are private to notification-daemon.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

#include "ui/message-tray.h"

G_BEGIN_DECLS

#define SHELL_TYPE_NOTIFICATION_DAEMON (shell_notification_daemon_get_type ())
G_DECLARE_FINAL_TYPE (ShellNotificationDaemon, shell_notification_daemon,
                      SHELL, NOTIFICATION_DAEMON, GObject)

ShellNotificationDaemon *shell_notification_daemon_new (ShellMessageTray *message_tray);

G_END_DECLS
