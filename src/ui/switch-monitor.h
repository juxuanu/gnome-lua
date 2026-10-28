/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * switch-monitor.h: The popup of the display switch key (Super+P): mirror the
 * displays, join them, use only the external or only the built-in displays.
 * Port of js/ui/switchMonitor.js.
 *
 * The list of the popup is private to switch-monitor.c. windowManager.js
 * creates the popup with `new Shell.SwitchMonitorPopup ()` and calls
 * shell_switcher_popup_show_switcher () on it.
 */

#pragma once

#include "ui/switcher-popup.h"

G_BEGIN_DECLS

#define SHELL_TYPE_SWITCH_MONITOR_POPUP (shell_switch_monitor_popup_get_type ())
G_DECLARE_FINAL_TYPE (ShellSwitchMonitorPopup, shell_switch_monitor_popup,
                      SHELL, SWITCH_MONITOR_POPUP, ShellSwitcherPopup)

ShellSwitchMonitorPopup *shell_switch_monitor_popup_new (void);

G_END_DECLS
