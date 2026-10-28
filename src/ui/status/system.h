/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * system.h: The power and system indicator of the top bar and the row of
 * buttons at the bottom of the quick settings (battery, screenshot,
 * settings, lock screen and the power off menu). Port of
 * js/ui/status/system.js.
 *
 * #ShellSystemStatusIndicator is the `Indicator` of the module (the name
 * #ShellSystemIndicator is the base class of every indicator, see
 * panel-menu.h). The screenshot UI is JS: the screenshot button asks for it
 * with ::screenshot-ui-requested, which the owner of the indicator answers
 * with `Main.screenshotUI.open ()`.
 */

#pragma once

#include "ui/panel-menu.h"
#include "ui/quick-settings.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellPowerToggle                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_POWER_TOGGLE (shell_power_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellPowerToggle, shell_power_toggle,
                      SHELL, POWER_TOGGLE, ShellQuickToggle)

ShellPowerToggle *shell_power_toggle_new                    (void);
const char       *shell_power_toggle_get_fallback_icon_name (ShellPowerToggle *self);
void              shell_power_toggle_set_fallback_icon_name (ShellPowerToggle *self,
                                                             const char       *fallback_icon_name);

/* ------------------------------------------------------------------------ */
/* ShellScreenshotItem, ShellSettingsItem, ShellLockItem, ShellShutdownItem */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREENSHOT_ITEM (shell_screenshot_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenshotItem, shell_screenshot_item,
                      SHELL, SCREENSHOT_ITEM, ShellQuickSettingsItem)

#define SHELL_TYPE_SETTINGS_ITEM (shell_settings_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellSettingsItem, shell_settings_item,
                      SHELL, SETTINGS_ITEM, ShellQuickSettingsItem)

#define SHELL_TYPE_SHUTDOWN_ITEM (shell_shutdown_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellShutdownItem, shell_shutdown_item,
                      SHELL, SHUTDOWN_ITEM, ShellQuickSettingsItem)

#define SHELL_TYPE_LOCK_ITEM (shell_lock_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellLockItem, shell_lock_item,
                      SHELL, LOCK_ITEM, ShellQuickSettingsItem)

ShellScreenshotItem *shell_screenshot_item_new (void);
ShellSettingsItem   *shell_settings_item_new   (void);
ShellShutdownItem   *shell_shutdown_item_new   (void);
ShellLockItem       *shell_lock_item_new       (void);

/* ------------------------------------------------------------------------ */
/* ShellSystemItem                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SYSTEM_ITEM (shell_system_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellSystemItem, shell_system_item,
                      SHELL, SYSTEM_ITEM, ShellQuickSettingsItem)

ShellSystemItem  *shell_system_item_new             (void);
ShellPowerToggle *shell_system_item_get_power_toggle (ShellSystemItem *self);

/* ------------------------------------------------------------------------ */
/* ShellSystemStatusIndicator                                               */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SYSTEM_STATUS_INDICATOR (shell_system_status_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellSystemStatusIndicator, shell_system_status_indicator,
                      SHELL, SYSTEM_STATUS_INDICATOR, ShellSystemIndicator)

ShellSystemStatusIndicator *shell_system_status_indicator_new             (void);
ShellSystemItem            *shell_system_status_indicator_get_system_item (ShellSystemStatusIndicator *self);

G_END_DECLS
