/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * permission-store.h: Proxy for the org.freedesktop.impl.portal.PermissionStore
 * D-Bus interface. Port of js/misc/permissionStore.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "permission-store-dbus.h"

G_BEGIN_DECLS

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ShellPortalPermissionStore, g_object_unref)

void                        shell_permission_store_new        (GCancellable        *cancellable,
                                                               GAsyncReadyCallback  callback,
                                                               gpointer             user_data);
ShellPortalPermissionStore *shell_permission_store_new_finish (GAsyncResult        *res,
                                                               GError             **error);

G_END_DECLS
