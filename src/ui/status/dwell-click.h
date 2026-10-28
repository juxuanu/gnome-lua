/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dwell-click.h: The menu of the top bar which chooses the kind of click the
 * dwell click accessibility feature performs. Port of
 * js/ui/status/dwellClick.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_DWELL_CLICK_INDICATOR (shell_dwell_click_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellDwellClickIndicator, shell_dwell_click_indicator,
                      SHELL, DWELL_CLICK_INDICATOR, ShellPanelMenuButton)

ShellDwellClickIndicator *shell_dwell_click_indicator_new (void);

G_END_DECLS
