/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * extensions-service.h: The org.gnome.Shell.Extensions service, which forwards
 * to the extension manager of the shell and shows the window of the
 * preferences of an extension. Port of
 * js/dbusServices/extensions/extensionsService.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "shell-dbus-service.h"

G_BEGIN_DECLS

#define EXT_TYPE_SERVICE (ext_service_get_type ())
G_DECLARE_FINAL_TYPE (ExtService, ext_service, EXT, SERVICE, ShellDBusService)

ExtService *ext_service_new (void);

G_END_DECLS
