/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-dbus.h: The D-Bus interfaces the shell exports on the session bus:
 * org.gnome.Shell (and its accelerator grabbing), org.gnome.Shell.Extensions,
 * org.gnome.ScreenSaver, org.gnome.Shell.ScreenTime and
 * org.gnome.Shell.Brightness. Port of js/ui/shellDBus.js.
 *
 * What the JS reached through singletons that are still JS (the overview,
 * the window manager, the screen shield, ...) is a signal of the service,
 * which js/ui/main.js (and src/ui/screen-shield.c) connect to those
 * singletons. The extension service answers through the extension manager
 * (extension-system.h), which it is given.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "misc/brightness-manager.h"
#include "ui/extension-system.h"
#include "ui/osd-monitor-labeler.h"
#include "ui/osd-window.h"

G_BEGIN_DECLS

/* org.gnome.Shell */

#define SHELL_TYPE_DBUS_SERVICE (shell_dbus_service_get_type ())
G_DECLARE_FINAL_TYPE (ShellDBusService, shell_dbus_service,
                      SHELL, DBUS_SERVICE, GObject)

ShellDBusService *shell_dbus_service_new (void);

void              shell_dbus_service_complete_eval (ShellDBusService    *self,
                                                    GDBusMethodInvocation *invocation,
                                                    gboolean             success,
                                                    const char          *result);

ShellOsdWindowManager  *shell_dbus_service_get_osd_window_manager  (ShellDBusService *self);
void                    shell_dbus_service_set_osd_window_manager  (ShellDBusService       *self,
                                                                    ShellOsdWindowManager  *manager);
ShellOsdMonitorLabeler *shell_dbus_service_get_osd_monitor_labeler (ShellDBusService *self);
void                    shell_dbus_service_set_osd_monitor_labeler (ShellDBusService        *self,
                                                                    ShellOsdMonitorLabeler  *labeler);

gboolean          shell_dbus_service_get_overview_active (ShellDBusService *self);
void              shell_dbus_service_set_overview_active (ShellDBusService *self,
                                                          gboolean          active);

/* org.gnome.Shell.Extensions */

#define SHELL_TYPE_DBUS_EXTENSIONS_SERVICE (shell_dbus_extensions_service_get_type ())
G_DECLARE_FINAL_TYPE (ShellDBusExtensionsService, shell_dbus_extensions_service,
                      SHELL, DBUS_EXTENSIONS_SERVICE, GObject)

ShellDBusExtensionsService *shell_dbus_extensions_service_new (ShellExtensionManager *manager);

/* org.gnome.ScreenSaver */

#define SHELL_TYPE_SCREEN_SAVER_SERVICE (shell_screen_saver_service_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenSaverService, shell_screen_saver_service,
                      SHELL, SCREEN_SAVER_SERVICE, GObject)

ShellScreenSaverService *shell_screen_saver_service_new (void);

void              shell_screen_saver_service_emit_active_changed (ShellScreenSaverService *self,
                                                                  gboolean                 active);
void              shell_screen_saver_service_emit_wake_up_screen (ShellScreenSaverService *self);
void              shell_screen_saver_service_lock_screen_shown   (ShellScreenSaverService *self);

/* org.gnome.Shell.ScreenTime */

#define SHELL_TYPE_SCREEN_TIME_SERVICE (shell_screen_time_service_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenTimeService, shell_screen_time_service,
                      SHELL, SCREEN_TIME_SERVICE, GObject)

ShellScreenTimeService *shell_screen_time_service_new (GObject *manager);

/* org.gnome.Shell.Brightness */

#define SHELL_TYPE_BRIGHTNESS_SERVICE (shell_brightness_service_get_type ())
G_DECLARE_FINAL_TYPE (ShellBrightnessService, shell_brightness_service,
                      SHELL, BRIGHTNESS_SERVICE, GObject)

ShellBrightnessService *shell_brightness_service_new (ShellBrightnessManager *manager);

G_END_DECLS
