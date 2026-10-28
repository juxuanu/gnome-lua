/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auto-rotate.h: The "Auto Rotate" quick settings toggle. Port of
 * js/ui/status/autoRotate.js.
 *
 * Only #ShellAutoRotateIndicator (`Indicator`) is public, the toggle is
 * private to auto-rotate.c.
 */

#pragma once

#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_AUTO_ROTATE_INDICATOR (shell_auto_rotate_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellAutoRotateIndicator, shell_auto_rotate_indicator,
                      SHELL, AUTO_ROTATE_INDICATOR, ShellSystemIndicator)

ShellAutoRotateIndicator *shell_auto_rotate_indicator_new (void);

G_END_DECLS
