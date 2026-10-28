/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * power-profiles.h: The "Power Mode" quick settings toggle with the menu
 * that lists the profiles of power-profiles-daemon, and its status icon.
 * Port of js/ui/status/powerProfiles.js.
 *
 * Only #ShellPowerProfilesIndicator (`Indicator`) is public, the toggle is
 * private to power-profiles.c.
 */

#pragma once

#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_POWER_PROFILES_INDICATOR (shell_power_profiles_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellPowerProfilesIndicator, shell_power_profiles_indicator,
                      SHELL, POWER_PROFILES_INDICATOR, ShellSystemIndicator)

ShellPowerProfilesIndicator *shell_power_profiles_indicator_new (void);

G_END_DECLS
