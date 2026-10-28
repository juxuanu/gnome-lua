/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * thunderbolt.h: The indicator which shows while boltd probes the Thunderbolt
 * devices, and the authorization of the new devices. Port of
 * js/ui/status/thunderbolt.js.
 *
 * Only #ShellThunderboltIndicator (`Indicator`) is public. The client of
 * the bolt D-Bus API (`Client`) and the helper which authorizes new devices
 * (`AuthRobot`) are private to thunderbolt.c.
 */

#pragma once

#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_THUNDERBOLT_INDICATOR (shell_thunderbolt_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellThunderboltIndicator, shell_thunderbolt_indicator,
                      SHELL, THUNDERBOLT_INDICATOR, ShellSystemIndicator)

ShellThunderboltIndicator *shell_thunderbolt_indicator_new (void);

G_END_DECLS
