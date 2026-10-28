/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dark-mode.h: The "Dark Style" quick settings toggle. Port of
 * js/ui/status/darkMode.js.
 *
 * Only #ShellDarkModeIndicator (`Indicator`) is public, the toggle is
 * private to dark-mode.c.
 */

#pragma once

#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_DARK_MODE_INDICATOR (shell_dark_mode_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellDarkModeIndicator, shell_dark_mode_indicator,
                      SHELL, DARK_MODE_INDICATOR, ShellSystemIndicator)

ShellDarkModeIndicator *shell_dark_mode_indicator_new (void);

G_END_DECLS
