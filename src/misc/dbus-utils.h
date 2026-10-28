/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dbus-utils.h: Helpers for the D-Bus interfaces of the shell: loading the
 * interface XML from the gnome-shell-dbus-interfaces gresource, emitting a
 * signal to a single bus name, and the sender check of the services that
 * only some well-known names may call. Port of js/misc/dbusUtils.js (and of
 * the DBusSenderChecker of js/misc/util.js, which is still there for the JS
 * services).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

char               *shell_dbus_load_interface_xml     (const char *iface);
char               *shell_dbus_load_sub_interface_xml (const char *iface,
                                                       const char *iface_file);
GDBusInterfaceInfo *shell_dbus_get_interface_info     (const char *iface);

void                shell_dbus_emit_signal_to_destination (GDBusInterfaceSkeleton *skeleton,
                                                           const char             *destination,
                                                           const char             *signal_name,
                                                           GVariant               *params);

#define SHELL_TYPE_DBUS_SENDER_CHECKER (shell_dbus_sender_checker_get_type ())
G_DECLARE_FINAL_TYPE (ShellDBusSenderChecker, shell_dbus_sender_checker,
                      SHELL, DBUS_SENDER_CHECKER, GObject)

ShellDBusSenderChecker *shell_dbus_sender_checker_new (const char * const *allow_list);

void     shell_dbus_sender_checker_check_invocation_async  (ShellDBusSenderChecker *self,
                                                            GDBusMethodInvocation  *invocation,
                                                            GCancellable           *cancellable,
                                                            GAsyncReadyCallback     callback,
                                                            gpointer                user_data);
gboolean shell_dbus_sender_checker_check_invocation_finish (ShellDBusSenderChecker *self,
                                                            GAsyncResult           *result,
                                                            GError                **error);

G_END_DECLS
