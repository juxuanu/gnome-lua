/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * background-apps.h: The quick settings entry which lists the applications
 * that run without a window, and lets the user quit them. Port of
 * js/ui/status/backgroundApps.js.
 *
 * Only #ShellBackgroundAppsIndicator (`Indicator`) is public, the toggle and
 * the menu items (`BackgroundAppMenuItem`, `BackgroundAppsToggle`) are
 * private to background-apps.c. The overview is still JS: an item emits
 * #ShellBackgroundAppsIndicator::overview-hide-requested before it opens its
 * application.
 */

#pragma once

#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_BACKGROUND_APPS_INDICATOR (shell_background_apps_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellBackgroundAppsIndicator, shell_background_apps_indicator,
                      SHELL, BACKGROUND_APPS_INDICATOR, ShellSystemIndicator)

ShellBackgroundAppsIndicator *shell_background_apps_indicator_new (void);

G_END_DECLS
