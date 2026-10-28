/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * time-limits-manager-private.h: The environment of #ShellTimeLimitsManager
 * (clock, logind, settings, parental controls and the malcontent timer daemon)
 * as a table of functions, so that the unit test can replace it with a
 * simulated clock and event queue. Port of the `clock`, `loginManagerFactory`,
 * `loginUserFactory`, `settingsFactory`, `parentalControlsManagerFactory` and
 * `timerChildProxyFactory` constructor arguments of the JS TimeLimitsManager.
 *
 * Only tests/unit/test-time-limits-manager.c and time-limits-manager.c use
 * this.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "misc/time-limits-manager.h"

G_BEGIN_DECLS

/**
 * ShellTimeLimitsUserState:
 * @SHELL_TIME_LIMITS_USER_STATE_INACTIVE: the user is not using the device
 * @SHELL_TIME_LIMITS_USER_STATE_ACTIVE: the user is using the device
 *
 * The user state, as it is saved in the history file, so the values cannot
 * be changed (`UserState`).
 */
typedef enum
{
  SHELL_TIME_LIMITS_USER_STATE_INACTIVE = 0,
  SHELL_TIME_LIMITS_USER_STATE_ACTIVE   = 1,
} ShellTimeLimitsUserState;

/**
 * ShellTimeLimitsWatchFunc:
 * @user_data: the data given when the watch was added
 *
 * Called when a signal of the environment is emitted.
 */
typedef void (* ShellTimeLimitsWatchFunc) (gpointer user_data);

/**
 * ShellTimeLimitsSleepFunc:
 * @preparing_for_sleep: whether the machine is about to sleep
 * @user_data: the data given when the watch was added
 *
 * Called for `prepare-for-sleep` of the login manager.
 */
typedef void (* ShellTimeLimitsSleepFunc) (gboolean preparing_for_sleep,
                                           gpointer user_data);

/**
 * ShellTimeLimitsSettingsFunc:
 * @key: the key which changed
 * @user_data: the data given when the watch was added
 *
 * Called for `changed` of the settings.
 */
typedef void (* ShellTimeLimitsSettingsFunc) (const char *key,
                                              gpointer    user_data);

/**
 * ShellTimeLimitsManagerEnv:
 * @user_data: the first argument of every function
 * @destroy: (nullable): called on @user_data when the manager is finalized
 * @get_real_time_secs: the real (wall clock) time, in seconds since the Unix epoch
 * @get_monotonic_time_secs: the monotonic time in seconds
 * @timeout_add_seconds: like g_timeout_add_seconds () at %G_PRIORITY_DEFAULT
 * @source_remove: like g_source_remove () for the ids returned by
 *   @timeout_add_seconds and @time_change_notify
 * @time_change_notify: calls @func when the real time clock changes with
 *   respect to the monotonic clock, returns the id of the source or 0
 * @login_manager_connect: connects to `prepare-for-sleep` of logind
 * @login_manager_disconnect: disconnects the handler
 * @login_manager_get_preparing_for_sleep: whether the machine is preparing to
 *   sleep
 * @login_manager_inhibit_async: takes a delay inhibitor on sleep
 * @login_manager_inhibit_finish: the stream to close to release the inhibitor
 *   (may be %NULL without an error)
 * @login_user_get_async: gets the logind object of the current user
 * @login_user_get_finish: finishes @login_user_get_async
 * @login_user_connect: calls @func when properties of the user change
 * @login_user_disconnect: disconnects the handler
 * @login_user_get_state: the `State` of the user
 * @login_user_get_idle_hint: the `IdleHint` of the user
 * @settings_get_boolean: a `b` key of `org.gnome.desktop.screen-time-limits`
 * @settings_get_uint: a `u` key of `org.gnome.desktop.screen-time-limits`
 * @settings_connect_changed: calls @func when a key of the settings changes
 * @settings_disconnect: disconnects the handler
 * @session_limits_enabled: whether the parental controls session limits are
 *   enabled
 * @session_limits_connect_changed: connects to `session-limits-changed` of the
 *   parental controls manager
 * @session_limits_disconnect: disconnects the handler
 * @timer_connect_estimated_times_changed: connects to `EstimatedTimesChanged`
 *   of the malcontent timer daemon
 * @timer_disconnect: disconnects the handler
 * @timer_record_usage_async: calls `RecordUsage` with the `a(ttss)` @entries
 * @timer_record_usage_finish: finishes @timer_record_usage_async
 * @timer_get_estimated_times_async: calls `GetEstimatedTimes` for the record
 *   type `login-session`
 * @timer_get_estimated_times_finish: finishes it, returns the `a{s(btttt)}`
 *   times
 *
 * The functions the manager needs from its environment. The handler ids
 * returned by the connect functions are passed to the disconnect functions.
 */
typedef struct
{
  gpointer user_data;
  GDestroyNotify destroy;

  gint64   (* get_real_time_secs)      (gpointer user_data);
  gint64   (* get_monotonic_time_secs) (gpointer user_data);
  guint    (* timeout_add_seconds)     (gpointer user_data,
                                        guint interval,
                                        GSourceFunc func,
                                        gpointer func_data);
  void     (* source_remove)           (gpointer user_data,
                                        guint id);
  guint    (* time_change_notify)      (gpointer user_data,
                                        GSourceFunc func,
                                        gpointer func_data);

  gulong   (* login_manager_connect)   (gpointer user_data,
                                        ShellTimeLimitsSleepFunc func,
                                        gpointer func_data);
  void     (* login_manager_disconnect) (gpointer user_data,
                                         gulong handler_id);
  gboolean (* login_manager_get_preparing_for_sleep) (gpointer user_data);
  void     (* login_manager_inhibit_async) (gpointer user_data,
                                            const char *reason,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer callback_data);
  GInputStream * (* login_manager_inhibit_finish) (gpointer user_data,
                                                   GAsyncResult *result,
                                                   GError **error);

  void     (* login_user_get_async)    (gpointer user_data,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer callback_data);
  gboolean (* login_user_get_finish)   (gpointer user_data,
                                        GAsyncResult *result,
                                        GError **error);
  gulong   (* login_user_connect)      (gpointer user_data,
                                        ShellTimeLimitsWatchFunc func,
                                        gpointer func_data);
  void     (* login_user_disconnect)   (gpointer user_data,
                                        gulong handler_id);
  const char * (* login_user_get_state) (gpointer user_data);
  gboolean (* login_user_get_idle_hint) (gpointer user_data);

  gboolean (* settings_get_boolean)    (gpointer user_data,
                                        const char *key);
  guint    (* settings_get_uint)       (gpointer user_data,
                                        const char *key);
  gulong   (* settings_connect_changed) (gpointer user_data,
                                         ShellTimeLimitsSettingsFunc func,
                                         gpointer func_data);
  void     (* settings_disconnect)     (gpointer user_data,
                                        gulong handler_id);

  gboolean (* session_limits_enabled)  (gpointer user_data);
  gulong   (* session_limits_connect_changed) (gpointer user_data,
                                               ShellTimeLimitsWatchFunc func,
                                               gpointer func_data);
  void     (* session_limits_disconnect) (gpointer user_data,
                                          gulong handler_id);

  gulong   (* timer_connect_estimated_times_changed) (gpointer user_data,
                                                      ShellTimeLimitsWatchFunc func,
                                                      gpointer func_data);
  void     (* timer_disconnect)        (gpointer user_data,
                                        gulong handler_id);
  void     (* timer_record_usage_async) (gpointer user_data,
                                         GVariant *entries,
                                         GAsyncReadyCallback callback,
                                         gpointer callback_data);
  gboolean (* timer_record_usage_finish) (gpointer user_data,
                                          GAsyncResult *result,
                                          GError **error);
  void     (* timer_get_estimated_times_async) (gpointer user_data,
                                                GAsyncReadyCallback callback,
                                                gpointer callback_data);
  gboolean (* timer_get_estimated_times_finish) (gpointer user_data,
                                                 GAsyncResult *result,
                                                 GVariant **times,
                                                 GError **error);
} ShellTimeLimitsManagerEnv;

/**
 * shell_time_limits_manager_new_with_env:
 * @env: the environment; the table is copied
 * @history_file: the file the history is saved in
 *
 * Creates a manager that uses @env instead of the real clock, logind,
 * GSettings, parental controls and timer daemon.
 *
 * Returns: (transfer full): a new #ShellTimeLimitsManager
 */
ShellTimeLimitsManager *shell_time_limits_manager_new_with_env (const ShellTimeLimitsManagerEnv *env,
                                                                GFile                           *history_file);

/*
 * shell_time_limits_manager_is_busy:
 *
 * Whether asynchronous operations (file or D-Bus) are still in flight. The
 * unit test iterates the main context until this is %FALSE after every event
 * of its simulated clock, which keeps the tests deterministic.
 */
gboolean shell_time_limits_manager_is_busy (ShellTimeLimitsManager *self);

G_END_DECLS
