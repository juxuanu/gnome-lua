/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * parental-controls-manager.h: Cached access to the parental controls
 * settings of the user running the shell. Port of
 * js/misc/parentalControlsManager.js.
 *
 * Copyright (C) 2018, 2019, 2020 Endless Mobile, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS


#define SHELL_TYPE_PARENTAL_CONTROLS_MANAGER (shell_parental_controls_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellParentalControlsManager, shell_parental_controls_manager,
                      SHELL, PARENTAL_CONTROLS_MANAGER, GObject)

ShellParentalControlsManager *shell_parental_controls_manager_get_default (void);

gboolean shell_parental_controls_manager_get_initialized (ShellParentalControlsManager *manager);

gboolean shell_parental_controls_manager_should_show_app (ShellParentalControlsManager *manager,
                                                          GAppInfo                     *app_info);

gboolean shell_parental_controls_manager_app_filter_enabled     (ShellParentalControlsManager *manager);
gboolean shell_parental_controls_manager_session_limits_enabled (ShellParentalControlsManager *manager);

gboolean shell_parental_controls_manager_get_any_parental_controls_enabled (ShellParentalControlsManager *manager);

G_END_DECLS
