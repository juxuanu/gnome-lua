/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * rfkill.h: The airplane mode of gnome-settings-daemon, its quick settings
 * toggle and the status icon which shows while it is on. Port of
 * js/ui/status/rfkill.js.
 *
 * #ShellRfkillManager (`RfkillManager`) is the connection to the
 * org.gnome.SettingsDaemon.Rfkill service, shared by the shell:
 * shell_rfkill_manager_get_default () is `getRfkillManager ()`.
 * #ShellRfkillIndicator (`Indicator`) has the toggle, which is private to
 * rfkill.c.
 */

#pragma once

#include "ui/panel-menu.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellRfkillManager                                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_RFKILL_MANAGER (shell_rfkill_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellRfkillManager, shell_rfkill_manager,
                      SHELL, RFKILL_MANAGER, GObject)

ShellRfkillManager *shell_rfkill_manager_get_default           (void);

gboolean            shell_rfkill_manager_get_airplane_mode     (ShellRfkillManager *self);
void                shell_rfkill_manager_set_airplane_mode     (ShellRfkillManager *self,
                                                                gboolean            airplane_mode);
gboolean            shell_rfkill_manager_get_hw_airplane_mode  (ShellRfkillManager *self);
gboolean            shell_rfkill_manager_get_show_airplane_mode (ShellRfkillManager *self);

/* ------------------------------------------------------------------------ */
/* ShellRfkillIndicator                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_RFKILL_INDICATOR (shell_rfkill_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellRfkillIndicator, shell_rfkill_indicator,
                      SHELL, RFKILL_INDICATOR, ShellSystemIndicator)

ShellRfkillIndicator *shell_rfkill_indicator_new (void);

G_END_DECLS
