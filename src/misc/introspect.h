/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * introspect.h: The org.gnome.Shell.Introspect D-Bus service, which tells the
 * desktop portal backends about the running applications and windows.
 * Port of js/misc/introspect.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SHELL_TYPE_INTROSPECT_SERVICE (shell_introspect_service_get_type ())
G_DECLARE_FINAL_TYPE (ShellIntrospectService, shell_introspect_service,
                      SHELL, INTROSPECT_SERVICE, GObject)

ShellIntrospectService *shell_introspect_service_new (void);

G_END_DECLS
