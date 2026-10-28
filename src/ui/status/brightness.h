/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * brightness.h: The brightness slider of the quick settings, with a menu
 * that has one slider per monitor. Port of js/ui/status/brightness.js.
 *
 * Only #ShellBrightnessIndicator (`Indicator`) is public, the slider item
 * is private to brightness.c. The brightness manager (`Main.brightnessManager`)
 * is a construct-only property because it has no accessor in C: ShellMain
 * creates it and panel-items.c passes it on.
 */

#pragma once

#include "misc/brightness-manager.h"
#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_BRIGHTNESS_INDICATOR (shell_brightness_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellBrightnessIndicator, shell_brightness_indicator,
                      SHELL, BRIGHTNESS_INDICATOR, ShellSystemIndicator)

ShellBrightnessIndicator *shell_brightness_indicator_new (ShellBrightnessManager *brightness_manager);

G_END_DECLS
