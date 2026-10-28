/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * time-limits-manager.h: A GNOME Shell component to support screen time
 * limits and statistics. Port of js/misc/timeLimitsManager.js.
 *
 * Copyright 2024, 2025 GNOME Foundation, Inc.
 *
 * #ShellTimeLimitsManager tracks the total active and inactive time of the
 * user and says when the user has reached their session time limit (parental
 * controls) or their wellbeing daily time limit.
 * #ShellTimeLimitsDispatcher turns that state into notifications, a screen
 * lock and a grayscale screen for the user.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "ui/layout.h"
#include "ui/message-tray.h"

G_BEGIN_DECLS

/**
 * SHELL_TIME_LIMITS_HISTORY_THRESHOLD_SECONDS:
 *
 * The maximum time history entries are kept, 14 weeks
 * (`HISTORY_THRESHOLD_SECONDS`).
 */
#define SHELL_TIME_LIMITS_HISTORY_THRESHOLD_SECONDS 8467200

/**
 * ShellTimeLimitsState:
 * @SHELL_TIME_LIMITS_STATE_DISABLED: screen time limit history is disabled
 * @SHELL_TIME_LIMITS_STATE_ACTIVE: screen time limit history recording is
 *   enabled, but limits are disabled or the limit has not been hit yet
 * @SHELL_TIME_LIMITS_STATE_LIMIT_REACHED: the limit has been reached
 *
 * The state of the time limits (`TimeLimitsManager.TimeLimitsState`).
 */
typedef enum
{
  SHELL_TIME_LIMITS_STATE_DISABLED      = 0,
  SHELL_TIME_LIMITS_STATE_ACTIVE        = 1,
  SHELL_TIME_LIMITS_STATE_LIMIT_REACHED = 2,
} ShellTimeLimitsState;

const char *shell_time_limits_state_to_string (ShellTimeLimitsState state);

/* ------------------------------------------------------------------------ */
/* ShellTimeLimitsManager                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_TIME_LIMITS_MANAGER (shell_time_limits_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellTimeLimitsManager, shell_time_limits_manager,
                      SHELL, TIME_LIMITS_MANAGER, GObject)

ShellTimeLimitsManager *shell_time_limits_manager_new (void);

void     shell_time_limits_manager_shutdown_async  (ShellTimeLimitsManager *self,
                                                    GCancellable           *cancellable,
                                                    GAsyncReadyCallback     callback,
                                                    gpointer                user_data);
gboolean shell_time_limits_manager_shutdown_finish (ShellTimeLimitsManager *self,
                                                    GAsyncResult           *result,
                                                    GError                **error);

gint64   shell_time_limits_manager_get_current_time (ShellTimeLimitsManager *self);

ShellTimeLimitsState shell_time_limits_manager_get_state (ShellTimeLimitsManager *self);
gboolean shell_time_limits_manager_get_parental_controls_session_limits_enabled (ShellTimeLimitsManager *self);
gboolean shell_time_limits_manager_get_wellbeing_daily_limit_enabled (ShellTimeLimitsManager *self);
guint64  shell_time_limits_manager_get_daily_limit_time    (ShellTimeLimitsManager *self);
gboolean shell_time_limits_manager_get_daily_limit_enabled (ShellTimeLimitsManager *self);
gboolean shell_time_limits_manager_get_grayscale_enabled   (ShellTimeLimitsManager *self);
gboolean shell_time_limits_manager_get_should_lock_session (ShellTimeLimitsManager *self);

/* ------------------------------------------------------------------------ */
/* ShellTimeLimitsDispatcher                                                 */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_TIME_LIMITS_DISPATCHER (shell_time_limits_dispatcher_get_type ())
G_DECLARE_FINAL_TYPE (ShellTimeLimitsDispatcher, shell_time_limits_dispatcher,
                      SHELL, TIME_LIMITS_DISPATCHER, GObject)

ShellTimeLimitsDispatcher *shell_time_limits_dispatcher_new     (ShellTimeLimitsManager *manager,
                                                                 ShellLayoutManager     *layout_manager,
                                                                 ShellMessageTray       *message_tray);
void                       shell_time_limits_dispatcher_destroy (ShellTimeLimitsDispatcher *self);

G_END_DECLS
