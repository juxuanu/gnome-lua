/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * night-light.h: The "Night Light" quick settings toggle and its status icon.
 * Port of js/ui/status/nightLight.js.
 *
 * Only #ShellNightLightIndicator (`Indicator`) is public, the toggle is
 * private to night-light.c.
 */

#pragma once

#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_NIGHT_LIGHT_INDICATOR (shell_night_light_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellNightLightIndicator, shell_night_light_indicator,
                      SHELL, NIGHT_LIGHT_INDICATOR, ShellSystemIndicator)

ShellNightLightIndicator *shell_night_light_indicator_new (void);

G_END_DECLS
