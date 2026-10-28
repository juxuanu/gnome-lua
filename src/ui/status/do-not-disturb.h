/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * do-not-disturb.h: The "Do Not Disturb" quick settings toggle and its
 * status icon. Port of js/ui/status/doNotDisturb.js.
 *
 * Only #ShellDoNotDisturbIndicator (`Indicator`) is public, the toggle is
 * private to do-not-disturb.c.
 */

#pragma once

#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_DO_NOT_DISTURB_INDICATOR (shell_do_not_disturb_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellDoNotDisturbIndicator, shell_do_not_disturb_indicator,
                      SHELL, DO_NOT_DISTURB_INDICATOR, ShellSystemIndicator)

ShellDoNotDisturbIndicator *shell_do_not_disturb_indicator_new (void);

G_END_DECLS
