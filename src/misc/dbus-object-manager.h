/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dbus-object-manager.h: Tracks the objects of a service implementing
 * org.freedesktop.DBus.ObjectManager and hands out #GDBusProxy objects for
 * the interfaces it was told about. Port of js/misc/objectManager.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS


#define SHELL_TYPE_DBUS_OBJECT_MANAGER (shell_dbus_object_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellDBusObjectManager, shell_dbus_object_manager,
                      SHELL, DBUS_OBJECT_MANAGER, GObject)

ShellDBusObjectManager *shell_dbus_object_manager_new (GDBusConnection    *connection,
                                                       const char         *name,
                                                       const char         *object_path,
                                                       const char * const *known_interfaces,
                                                       GCancellable       *cancellable);

GDBusProxy *shell_dbus_object_manager_get_proxy (ShellDBusObjectManager *manager,
                                                 const char             *object_path,
                                                 const char             *interface_name);

GList      *shell_dbus_object_manager_get_proxies_for_interface (ShellDBusObjectManager *manager,
                                                                 const char             *interface_name);

G_END_DECLS
