/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * backlight.h: The "Keyboard" quick settings toggle, with the menu to set the
 * brightness of the keyboard backlight. Port of js/ui/status/backlight.js.
 *
 * Only #ShellBacklightIndicator (`Indicator`) is public, the toggle and the
 * two ways to pick a level (a slider, or up to three buttons) are private to
 * backlight.c.
 */

#pragma once

#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_BACKLIGHT_INDICATOR (shell_backlight_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellBacklightIndicator, shell_backlight_indicator,
                      SHELL, BACKLIGHT_INDICATOR, ShellSystemIndicator)

ShellBacklightIndicator *shell_backlight_indicator_new (void);

G_END_DECLS
