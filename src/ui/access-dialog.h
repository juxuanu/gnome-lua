/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * access-dialog.h: The dialog of the org.freedesktop.impl.portal.Access
 * backend, which the desktop portal uses to ask the user for a permission on
 * behalf of an application. Port of js/ui/accessDialog.js.
 *
 * The dialog itself (`AccessDialog`) is private to access-dialog.c:
 * #ShellAccessDialogDBus creates it for each AccessDialog () call.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SHELL_TYPE_ACCESS_DIALOG_DBUS (shell_access_dialog_dbus_get_type ())
G_DECLARE_FINAL_TYPE (ShellAccessDialogDBus, shell_access_dialog_dbus,
                      SHELL, ACCESS_DIALOG_DBUS, GObject)

ShellAccessDialogDBus *shell_access_dialog_dbus_new (void);

G_END_DECLS
