/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * time-limits-manager.c: A GNOME Shell component to support screen time
 * limits and statistics. Port of js/misc/timeLimitsManager.js.
 *
 * Copyright 2024, 2025 GNOME Foundation, Inc.
 *
 * A manager class which tracks total active/inactive time for a user, and
 * signals when the user has reached their session time limit for actively
 * using the device if the user has parental controls session limits set. When
 * they are disabled, the wellbeing daily time limit is considered, and manager
 * signals when the user has reached their daily time limit for actively using
 * the device, if limits are enabled.
 *
 * For parental controls session time limit, the manager tracks the active/inactive
 * time via malcontent-timerd.
 *
 * For wellbeing daily time limit, the way in which the manager tracks the
 * active/inactive time is based off the total time the user account has spent
 * logged in to at least one active session, not idle (and not locked, but
 * that is a subset of idle time), and not suspended.
 * This corresponds to the `active` state from sd_uid_get_state()
 * (https://www.freedesktop.org/software/systemd/man/latest/sd_uid_get_state.html),
 * plus `IdleHint` from
 * https://www.freedesktop.org/software/systemd/man/latest/org.freedesktop.login1.html#User%20Objects,
 * plus `PreparingForSleep` from
 * https://www.freedesktop.org/software/systemd/man/latest/org.freedesktop.login1.html#The%20Manager%20Object.
 * `Inactive` time corresponds to all the other states from sd_uid_get_state(),
 * or if `IdleHint` is true or if `PreparingForSleep` is true.
 *
 * All times within the class are handled in terms of wall/real clock time,
 * rather than monotonic time. This is because it is necessary to continue
 * counting time while the process or device is suspended. If the real clock
 * is adjusted (e.g. as a result of an NTP sync) then everything has to be
 * recalculated. See `time_change_id`.
 *
 * The asynchronous operations (loading and saving the history, logind and the
 * timer daemon) are chains of callbacks. Starting or stopping the state
 * machine increments `run_id`; a chain that finds a different `run_id` than
 * the one it started with is over and does nothing else. Failures are logged
 * with g_message (): a missing or malformed history file must not make the
 * shell warn on startup or shutdown. The shell waits for
 * shell_time_limits_manager_shutdown_async () in a nested main loop.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

#include "malcontent-timer-dbus.h"
#include "misc/login-manager.h"
#include "misc/parental-controls-manager.h"
#include "misc/system-actions.h"
#include "misc/time-limits-manager.h"
#include "misc/time-limits-manager-private.h"
#include "shell-app-system.h"
#include "shell-enum-types.h"
#include "shell-global.h"
#include "shell-time-change-source.h"
#include "ui/environment.h"
#include "st.h"

#define LIMIT_UPCOMING_NOTIFICATION_TIME_SECONDS (10 * 60) /* notify the user 10min before their limit is reached */
#define PARENTAL_CONTROLS_LIMIT_UPCOMING_NOTIFICATION_TIME_SECONDS 60 /* notify the child 60s before their limit is reached */
#define GRAYSCALE_FADE_TIME_SECONDS 3
#define GRAYSCALE_SATURATION 1.0 /* saturation ([0.0, 1.0]) when grayscale mode is activated, 1.0 means full desaturation */

#define WELLBEING_DESKTOP_ID "gnome-wellbeing-panel.desktop"

#define MALCONTENT_TIMER_NAME "org.freedesktop.MalcontentTimer1"
#define MALCONTENT_TIMER_PATH "/org/freedesktop/MalcontentTimer1"

/* The longest timeout we ask GLib for: a daily limit of "never" would
 * otherwise overflow */
#define MAX_TIMEOUT_SECONDS (10 * 365 * 24 * 60 * 60)

/**
 * shell_time_limits_state_to_string:
 * @state: a #ShellTimeLimitsState
 *
 * Returns: (transfer none) (nullable): a string form of @state
 */
const char *
shell_time_limits_state_to_string (ShellTimeLimitsState state)
{
  switch (state)
    {
    case SHELL_TIME_LIMITS_STATE_DISABLED:
      return "DISABLED";
    case SHELL_TIME_LIMITS_STATE_ACTIVE:
      return "ACTIVE";
    case SHELL_TIME_LIMITS_STATE_LIMIT_REACHED:
      return "LIMIT_REACHED";
    default:
      return NULL;
    }
}

static const char *
user_state_to_string (ShellTimeLimitsUserState user_state)
{
  switch (user_state)
    {
    case SHELL_TIME_LIMITS_USER_STATE_INACTIVE:
      return "INACTIVE";
    case SHELL_TIME_LIMITS_USER_STATE_ACTIVE:
      return "ACTIVE";
    default:
      return NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* The real environment                                                      */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellLoginManager *login_manager;           /* unowned singleton */
  ShellLogin1User *login_user;
  GSettings *settings;
  ShellParentalControlsManager *parental_controls_manager;   /* unowned singleton */
  ShellMalcontentTimer1Child *timer;
  gboolean timer_tried;
} DefaultEnv;

typedef struct
{
  gpointer func;
  gpointer func_data;
} DefaultWatch;

static void
default_watch_free (gpointer data,
                    GClosure *closure)
{
  g_free (data);
}

static DefaultWatch *
default_watch_new (gpointer func,
                   gpointer func_data)
{
  DefaultWatch *watch = g_new0 (DefaultWatch, 1);

  watch->func = func;
  watch->func_data = func_data;

  return watch;
}

static gint64
default_get_real_time_secs (gpointer user_data)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

static gint64
default_get_monotonic_time_secs (gpointer user_data)
{
  return g_get_monotonic_time () / G_USEC_PER_SEC;
}

static guint
default_timeout_add_seconds (gpointer    user_data,
                             guint       interval,
                             GSourceFunc func,
                             gpointer    func_data)
{
  return g_timeout_add_seconds (interval, func, func_data);
}

static void
default_source_remove (gpointer user_data,
                       guint    id)
{
  g_source_remove (id);
}

static guint
default_time_change_notify (gpointer    user_data,
                            GSourceFunc func,
                            gpointer    func_data)
{
  g_autoptr (GError) error = NULL;
  GSource *source = shell_time_change_source_new (&error);
  guint id;

  if (source == NULL)
    {
      g_message ("Failed to listen for system clock changes: %s", error->message);
      return 0;
    }

  g_source_set_callback (source, func, func_data, NULL);
  id = g_source_attach (source, NULL);
  g_source_unref (source);

  return id;
}

static void
default_on_prepare_for_sleep (ShellLoginManager *login_manager,
                              gboolean           preparing_for_sleep,
                              gpointer           user_data)
{
  DefaultWatch *watch = user_data;

  ((ShellTimeLimitsSleepFunc) watch->func) (preparing_for_sleep, watch->func_data);
}

static gulong
default_login_manager_connect (gpointer                  user_data,
                               ShellTimeLimitsSleepFunc  func,
                               gpointer                  func_data)
{
  DefaultEnv *env = user_data;

  return g_signal_connect_data (env->login_manager, "prepare-for-sleep",
                                G_CALLBACK (default_on_prepare_for_sleep),
                                default_watch_new (func, func_data),
                                default_watch_free, 0);
}

static void
default_login_manager_disconnect (gpointer user_data,
                                  gulong   handler_id)
{
  DefaultEnv *env = user_data;

  g_signal_handler_disconnect (env->login_manager, handler_id);
}

static gboolean
default_login_manager_get_preparing_for_sleep (gpointer user_data)
{
  DefaultEnv *env = user_data;

  return shell_login_manager_get_preparing_for_sleep (env->login_manager);
}

static void
default_login_manager_inhibit_async (gpointer             user_data,
                                     const char          *reason,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             callback_data)
{
  DefaultEnv *env = user_data;

  shell_login_manager_inhibit_async (env->login_manager, reason, cancellable,
                                     callback, callback_data);
}

static GInputStream *
default_login_manager_inhibit_finish (gpointer       user_data,
                                      GAsyncResult  *result,
                                      GError       **error)
{
  DefaultEnv *env = user_data;

  return shell_login_manager_inhibit_finish (env->login_manager, result, error);
}

static void
default_login_user_get_async (gpointer             user_data,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             callback_data)
{
  DefaultEnv *env = user_data;

  shell_login_manager_get_current_user_async (env->login_manager, cancellable,
                                              callback, callback_data);
}

static gboolean
default_login_user_get_finish (gpointer       user_data,
                               GAsyncResult  *result,
                               GError       **error)
{
  DefaultEnv *env = user_data;
  ShellLogin1User *user;

  user = shell_login_manager_get_current_user_finish (env->login_manager, result, error);
  if (user == NULL)
    return FALSE;

  g_clear_object (&env->login_user);
  env->login_user = user;

  return TRUE;
}

static void
default_on_user_properties_changed (GDBusProxy *proxy,
                                    GVariant   *changed_properties,
                                    GStrv       invalidated_properties,
                                    gpointer    user_data)
{
  DefaultWatch *watch = user_data;

  ((ShellTimeLimitsWatchFunc) watch->func) (watch->func_data);
}

static gulong
default_login_user_connect (gpointer                  user_data,
                            ShellTimeLimitsWatchFunc  func,
                            gpointer                  func_data)
{
  DefaultEnv *env = user_data;

  if (env->login_user == NULL)
    return 0;

  return g_signal_connect_data (env->login_user, "g-properties-changed",
                                G_CALLBACK (default_on_user_properties_changed),
                                default_watch_new (func, func_data),
                                default_watch_free, 0);
}

static void
default_login_user_disconnect (gpointer user_data,
                               gulong   handler_id)
{
  DefaultEnv *env = user_data;

  if (env->login_user != NULL && handler_id != 0)
    g_signal_handler_disconnect (env->login_user, handler_id);
}

static const char *
default_login_user_get_state (gpointer user_data)
{
  DefaultEnv *env = user_data;

  return env->login_user != NULL ? shell_login1_user_get_state (env->login_user) : NULL;
}

static gboolean
default_login_user_get_idle_hint (gpointer user_data)
{
  DefaultEnv *env = user_data;

  return env->login_user != NULL ? shell_login1_user_get_idle_hint (env->login_user) : FALSE;
}

static gboolean
default_settings_get_boolean (gpointer    user_data,
                              const char *key)
{
  DefaultEnv *env = user_data;

  return g_settings_get_boolean (env->settings, key);
}

static guint
default_settings_get_uint (gpointer    user_data,
                           const char *key)
{
  DefaultEnv *env = user_data;

  return g_settings_get_uint (env->settings, key);
}

static void
default_on_settings_changed (GSettings  *settings,
                             const char *key,
                             gpointer    user_data)
{
  DefaultWatch *watch = user_data;

  ((ShellTimeLimitsSettingsFunc) watch->func) (key, watch->func_data);
}

static gulong
default_settings_connect_changed (gpointer                     user_data,
                                  ShellTimeLimitsSettingsFunc  func,
                                  gpointer                     func_data)
{
  DefaultEnv *env = user_data;

  return g_signal_connect_data (env->settings, "changed",
                                G_CALLBACK (default_on_settings_changed),
                                default_watch_new (func, func_data),
                                default_watch_free, 0);
}

static void
default_settings_disconnect (gpointer user_data,
                             gulong   handler_id)
{
  DefaultEnv *env = user_data;

  g_signal_handler_disconnect (env->settings, handler_id);
}

static gboolean
default_session_limits_enabled (gpointer user_data)
{
  DefaultEnv *env = user_data;

  return shell_parental_controls_manager_session_limits_enabled (env->parental_controls_manager);
}

static void
default_on_session_limits_changed (ShellParentalControlsManager *manager,
                                   gpointer                      user_data)
{
  DefaultWatch *watch = user_data;

  ((ShellTimeLimitsWatchFunc) watch->func) (watch->func_data);
}

static gulong
default_session_limits_connect_changed (gpointer                  user_data,
                                        ShellTimeLimitsWatchFunc  func,
                                        gpointer                  func_data)
{
  DefaultEnv *env = user_data;

  return g_signal_connect_data (env->parental_controls_manager, "session-limits-changed",
                                G_CALLBACK (default_on_session_limits_changed),
                                default_watch_new (func, func_data),
                                default_watch_free, 0);
}

static void
default_session_limits_disconnect (gpointer user_data,
                                   gulong   handler_id)
{
  DefaultEnv *env = user_data;

  g_signal_handler_disconnect (env->parental_controls_manager, handler_id);
}

static ShellMalcontentTimer1Child *
default_get_timer (DefaultEnv *env)
{
  if (env->timer == NULL && !env->timer_tried)
    {
      g_autoptr (GError) error = NULL;

      env->timer_tried = TRUE;
      env->timer = shell_malcontent_timer1_child_proxy_new_for_bus_sync (
        G_BUS_TYPE_SYSTEM,
        G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START_AT_CONSTRUCTION,
        MALCONTENT_TIMER_NAME,
        MALCONTENT_TIMER_PATH,
        NULL, &error);
      if (env->timer == NULL)
        g_debug ("Failed to get TimerChild proxy: %s", error->message);
    }

  return env->timer;
}

static void
default_on_estimated_times_changed (ShellMalcontentTimer1Child *timer,
                                    gpointer                    user_data)
{
  DefaultWatch *watch = user_data;

  ((ShellTimeLimitsWatchFunc) watch->func) (watch->func_data);
}

static gulong
default_timer_connect_estimated_times_changed (gpointer                  user_data,
                                               ShellTimeLimitsWatchFunc  func,
                                               gpointer                  func_data)
{
  DefaultEnv *env = user_data;
  ShellMalcontentTimer1Child *timer = default_get_timer (env);

  if (timer == NULL)
    return 0;

  return g_signal_connect_data (timer, "estimated-times-changed",
                                G_CALLBACK (default_on_estimated_times_changed),
                                default_watch_new (func, func_data),
                                default_watch_free, 0);
}

static void
default_timer_disconnect (gpointer user_data,
                          gulong   handler_id)
{
  DefaultEnv *env = user_data;

  if (env->timer != NULL && handler_id != 0)
    g_signal_handler_disconnect (env->timer, handler_id);
}

static void
default_timer_report_unavailable (GAsyncReadyCallback  callback,
                                  gpointer             callback_data,
                                  gpointer             tag)
{
  g_task_report_new_error (NULL, callback, callback_data, tag,
                           G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN,
                           "The timer daemon is not available");
}

static void
default_timer_record_usage_async (gpointer             user_data,
                                  GVariant            *entries,
                                  GAsyncReadyCallback  callback,
                                  gpointer             callback_data)
{
  DefaultEnv *env = user_data;
  ShellMalcontentTimer1Child *timer = default_get_timer (env);

  if (timer == NULL)
    {
      g_variant_unref (g_variant_ref_sink (entries));
      default_timer_report_unavailable (callback, callback_data,
                                        default_timer_record_usage_async);
      return;
    }

  shell_malcontent_timer1_child_call_record_usage (timer, entries, NULL,
                                                   callback, callback_data);
}

static gboolean
default_timer_record_usage_finish (gpointer       user_data,
                                   GAsyncResult  *result,
                                   GError       **error)
{
  DefaultEnv *env = user_data;

  if (g_async_result_is_tagged (result, default_timer_record_usage_async))
    return g_task_propagate_boolean (G_TASK (result), error);

  return shell_malcontent_timer1_child_call_record_usage_finish (env->timer, result, error);
}

static void
default_timer_get_estimated_times_async (gpointer             user_data,
                                         GAsyncReadyCallback  callback,
                                         gpointer             callback_data)
{
  DefaultEnv *env = user_data;
  ShellMalcontentTimer1Child *timer = default_get_timer (env);

  if (timer == NULL)
    {
      default_timer_report_unavailable (callback, callback_data,
                                        default_timer_get_estimated_times_async);
      return;
    }

  shell_malcontent_timer1_child_call_get_estimated_times (timer, "login-session", NULL,
                                                          callback, callback_data);
}

static gboolean
default_timer_get_estimated_times_finish (gpointer       user_data,
                                          GAsyncResult  *result,
                                          GVariant     **times,
                                          GError       **error)
{
  DefaultEnv *env = user_data;
  guint64 now_secs;

  if (g_async_result_is_tagged (result, default_timer_get_estimated_times_async))
    return g_task_propagate_boolean (G_TASK (result), error);

  return shell_malcontent_timer1_child_call_get_estimated_times_finish (env->timer, &now_secs,
                                                                        times, result, error);
}

static void
default_env_free (gpointer user_data)
{
  DefaultEnv *env = user_data;

  g_clear_object (&env->login_user);
  g_clear_object (&env->settings);
  g_clear_object (&env->timer);
  g_free (env);
}

static void
default_env_init (ShellTimeLimitsManagerEnv *env)
{
  DefaultEnv *data = g_new0 (DefaultEnv, 1);

  data->login_manager = shell_login_manager_get_default ();
  data->parental_controls_manager = shell_parental_controls_manager_get_default ();
  data->settings = g_settings_new ("org.gnome.desktop.screen-time-limits");

  env->user_data = data;
  env->destroy = default_env_free;
  env->get_real_time_secs = default_get_real_time_secs;
  env->get_monotonic_time_secs = default_get_monotonic_time_secs;
  env->timeout_add_seconds = default_timeout_add_seconds;
  env->source_remove = default_source_remove;
  env->time_change_notify = default_time_change_notify;
  env->login_manager_connect = default_login_manager_connect;
  env->login_manager_disconnect = default_login_manager_disconnect;
  env->login_manager_get_preparing_for_sleep = default_login_manager_get_preparing_for_sleep;
  env->login_manager_inhibit_async = default_login_manager_inhibit_async;
  env->login_manager_inhibit_finish = default_login_manager_inhibit_finish;
  env->login_user_get_async = default_login_user_get_async;
  env->login_user_get_finish = default_login_user_get_finish;
  env->login_user_connect = default_login_user_connect;
  env->login_user_disconnect = default_login_user_disconnect;
  env->login_user_get_state = default_login_user_get_state;
  env->login_user_get_idle_hint = default_login_user_get_idle_hint;
  env->settings_get_boolean = default_settings_get_boolean;
  env->settings_get_uint = default_settings_get_uint;
  env->settings_connect_changed = default_settings_connect_changed;
  env->settings_disconnect = default_settings_disconnect;
  env->session_limits_enabled = default_session_limits_enabled;
  env->session_limits_connect_changed = default_session_limits_connect_changed;
  env->session_limits_disconnect = default_session_limits_disconnect;
  env->timer_connect_estimated_times_changed = default_timer_connect_estimated_times_changed;
  env->timer_disconnect = default_timer_disconnect;
  env->timer_record_usage_async = default_timer_record_usage_async;
  env->timer_record_usage_finish = default_timer_record_usage_finish;
  env->timer_get_estimated_times_async = default_timer_get_estimated_times_async;
  env->timer_get_estimated_times_finish = default_timer_get_estimated_times_finish;
}

/* ------------------------------------------------------------------------ */
/* ShellTimeLimitsManager                                                    */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_STATE,
  PROP_DAILY_LIMIT_TIME,
  PROP_DAILY_LIMIT_ENABLED,
  PROP_GRAYSCALE_ENABLED,
  PROP_SHOULD_LOCK_SESSION,
  PROP_HISTORY_FILE,
  PROP_ENV,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  DAILY_LIMIT_REACHED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

typedef struct
{
  ShellTimeLimitsUserState old_state;
  ShellTimeLimitsUserState new_state;
  gint64 wall_time_secs;
} Transition;

typedef struct
{
  gint64 start_secs;
  gint64 end_secs;
} UsageRecord;

struct _ShellTimeLimitsManager
{
  GObject parent;

  ShellTimeLimitsManagerEnv env;
  gboolean have_env;
  GFile *history_file;

  gulong settings_changed_id;
  gulong session_limits_changed_id;
  gulong estimated_times_changed_id;

  ShellTimeLimitsState state;
  ShellTimeLimitsUserState user_state;
  GArray *transitions;                  /* Transition */
  GCancellable *cancellable;
  guint run_id;                         /* incremented by every start and stop */
  guint n_pending;                      /* asynchronous operations in flight */
  gulong login_manager_id;
  gulong login_user_id;
  gboolean login_user_ready;
  gboolean shut_down;                   /* shutdown_async () was called */
  GInputStream *inhibitor;
  gint64 last_state_change_time_secs;
  guint timer_id;
  guint time_change_id;
  gint64 clock_offset_secs;
  gboolean ignore_clock_offset_changes;
  gint64 latest_usage_end_secs;

  gboolean have_estimated_times;
  guint64 estimated_times[5];           /* limit reached, current start, current end, next start, next end */
};

G_DEFINE_FINAL_TYPE (ShellTimeLimitsManager, shell_time_limits_manager, G_TYPE_OBJECT)

typedef void (* DoneFunc) (ShellTimeLimitsManager *self,
                           const GError           *error,
                           gpointer                user_data);

static void update_state (ShellTimeLimitsManager *self);
static void start_state_machine (ShellTimeLimitsManager *self);
static void stop_state_machine_async (ShellTimeLimitsManager *self,
                                      DoneFunc                done,
                                      gpointer                done_data);
static void store_transitions_async (ShellTimeLimitsManager *self,
                                     GCancellable           *cancellable,
                                     DoneFunc                done,
                                     gpointer                done_data);
static void update_estimated_times_async (ShellTimeLimitsManager *self,
                                          DoneFunc                done,
                                          gpointer                done_data);

static void
op_begin (ShellTimeLimitsManager *self)
{
  self->n_pending++;
}

static void
op_end (ShellTimeLimitsManager *self)
{
  g_assert (self->n_pending > 0);
  self->n_pending--;
}

static void
log_error (ShellTimeLimitsManager *self,
           const GError           *error,
           gpointer                user_data)
{
  const char *what = user_data;

  if (error != NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("%s: %s", what, error->message);
}

static char *
unix_to_string (gint64 secs)
{
  g_autoptr (GDateTime) date_time = g_date_time_new_from_unix_local (secs);

  if (date_time == NULL)
    return g_strdup ("(invalid)");

  return g_date_time_format_iso8601 (date_time);
}

static gboolean
storing_transitions_enabled (ShellTimeLimitsManager *self)
{
  return self->env.session_limits_enabled (self->env.user_data) ||
         self->env.settings_get_boolean (self->env.user_data, "history-enabled");
}

static gboolean
wellbeing_daily_limit_enabled (ShellTimeLimitsManager *self)
{
  return self->env.settings_get_boolean (self->env.user_data, "daily-limit-enabled");
}

static gint64
get_clock_offset (ShellTimeLimitsManager *self)
{
  return self->env.get_real_time_secs (self->env.user_data) -
         self->env.get_monotonic_time_secs (self->env.user_data);
}

/* Get the Unix timestamp (in real seconds since the epoch) for the start of
 * today and the start of tomorrow.
 *
 * To avoid problems due to 01:00-02:00 not existing (or existing twice) on
 * daylight savings time transition days, arbitrarily say that the day
 * starts at 03:00. */
static void
get_start_of_today_secs (gint64  now_secs,
                         gint64 *start_of_today_secs,
                         gint64 *start_of_tomorrow_secs)
{
  g_autoptr (GDateTime) now_date = g_date_time_new_from_unix_local (now_secs);
  g_autoptr (GDateTime) start_of_today_date = NULL;
  g_autoptr (GDateTime) start_of_tomorrow_date = NULL;

  if (now_date == NULL)
    {
      *start_of_today_secs = now_secs;
      *start_of_tomorrow_secs = now_secs + 24 * 60 * 60;
      return;
    }

  start_of_today_date = g_date_time_new_local (g_date_time_get_year (now_date),
                                               g_date_time_get_month (now_date),
                                               g_date_time_get_day_of_month (now_date),
                                               3, 0, 0);

  /* The downside of using 03:00 is that we need to adjust if now_secs is in
   * the range [24:00–03:00). */
  if (g_date_time_compare (now_date, start_of_today_date) < 0)
    {
      GDateTime *yesterday = g_date_time_add_days (start_of_today_date, -1);

      g_date_time_unref (start_of_today_date);
      start_of_today_date = yesterday;
    }

  start_of_tomorrow_date = g_date_time_add_days (start_of_today_date, 1);

  *start_of_today_secs = g_date_time_to_unix (start_of_today_date);
  *start_of_tomorrow_secs = g_date_time_to_unix (start_of_tomorrow_date);
}

static Transition *
get_transition (ShellTimeLimitsManager *self,
                guint                   idx)
{
  return &g_array_index (self->transitions, Transition, idx);
}

/* Work out how much time the user has spent at the screen today, in real
 * clock seconds.
 *
 * This is probably an under-estimate, because it uses the systemd-logind
 * `active` user time, which is based on when the user is logged in rather
 * than necessarily when they are actively moving the mouse. Tracking the
 * latter is a lot more work, does not take into account time spent in other
 * login sessions (e.g. KDE) and could be considered more invasive.
 *
 * If the system clock changes, this will become inaccurate until the set of
 * state transitions catches up. It will never return a negative number,
 * though. */
static gint64
calculate_active_time_today_secs (ShellTimeLimitsManager *self,
                                  gint64                  now_secs,
                                  gint64                  start_of_today_secs)
{
  gint64 first_transition_today_idx = -1;
  gint64 active_time_today_secs = 0;
  /* In case the first transition is active → inactive, or is unset. */
  gint64 active_start_time_secs = start_of_today_secs;
  guint i;

  for (i = 0; i < self->transitions->len; i++)
    {
      if (get_transition (self, i)->wall_time_secs >= start_of_today_secs)
        {
          first_transition_today_idx = i;
          break;
        }
    }

  if (first_transition_today_idx != -1)
    {
      for (i = first_transition_today_idx; i < self->transitions->len; i++)
        {
          Transition *transition = get_transition (self, i);

          if (transition->new_state == SHELL_TIME_LIMITS_USER_STATE_ACTIVE)
            active_start_time_secs = transition->wall_time_secs;
          else if (transition->old_state == SHELL_TIME_LIMITS_USER_STATE_ACTIVE)
            active_time_today_secs += MAX (transition->wall_time_secs - active_start_time_secs, 0);
        }
    }

  if (self->transitions->len > 0 &&
      get_transition (self, self->transitions->len - 1)->new_state == SHELL_TIME_LIMITS_USER_STATE_ACTIVE)
    active_time_today_secs += MAX (now_secs - active_start_time_secs, 0);

  return active_time_today_secs;
}

/* Work out the timestamp at which the wellbeing daily limit was reached.
 *
 * If the user has not reached the wellbeing daily limit yet today, this will
 * return 0. */
static gint64
calculate_wellbeing_daily_limit_reached_at_secs (ShellTimeLimitsManager *self,
                                                 gint64                  now_secs,
                                                 gint64                  wellbeing_daily_limit_secs,
                                                 gint64                  start_of_today_secs)
{
  gint64 first_transition_today_idx = -1;
  gint64 active_time_today_secs = 0;
  gint64 active_start_time_secs = start_of_today_secs;
  guint i;

  for (i = 0; i < self->transitions->len; i++)
    {
      if (get_transition (self, i)->wall_time_secs >= start_of_today_secs)
        {
          first_transition_today_idx = i;
          break;
        }
    }

  if (first_transition_today_idx != -1)
    {
      for (i = first_transition_today_idx; i < self->transitions->len; i++)
        {
          Transition *transition = get_transition (self, i);

          if (transition->new_state == SHELL_TIME_LIMITS_USER_STATE_ACTIVE)
            active_start_time_secs = transition->wall_time_secs;
          else if (transition->old_state == SHELL_TIME_LIMITS_USER_STATE_ACTIVE)
            active_time_today_secs += MAX (transition->wall_time_secs - active_start_time_secs, 0);

          if (active_time_today_secs >= wellbeing_daily_limit_secs)
            return transition->wall_time_secs - (active_time_today_secs - wellbeing_daily_limit_secs);
        }
    }

  if (self->transitions->len > 0 &&
      get_transition (self, self->transitions->len - 1)->new_state == SHELL_TIME_LIMITS_USER_STATE_ACTIVE)
    active_time_today_secs += MAX (now_secs - active_start_time_secs, 0);

  if (active_time_today_secs >= wellbeing_daily_limit_secs)
    return now_secs - (active_time_today_secs - wellbeing_daily_limit_secs);

  /* Limit not reached yet. */
  return 0;
}

static void
notify_prop (ShellTimeLimitsManager *self,
             int                     prop)
{
  g_object_notify_by_pspec (G_OBJECT (self), props[prop]);
}

/* Callback for filtering to remove old entries from the transition array,
 * making sure to always keep the last one as a starting point for the future.
 *
 * Always drop entries which are in the future (even if that would remove all
 * history) as they can only exist as a result of file corruption or the clock
 * offset changing while we were offline. There is nothing useful we can
 * recover from them. */
static gboolean
filter_old_transition (gint64 wall_time_secs,
                       guint  idx,
                       guint  n_transitions,
                       gint64 now_secs)
{
  return wall_time_secs <= now_secs &&
         (wall_time_secs >= now_secs - SHELL_TIME_LIMITS_HISTORY_THRESHOLD_SECONDS ||
          idx == n_transitions - 1);
}

static void
add_transition (ShellTimeLimitsManager *self,
                ShellTimeLimitsUserState old_state,
                ShellTimeLimitsUserState new_state,
                gint64                   wall_time_secs,
                gboolean                 recalculate_state,
                gboolean                 debug_log)
{
  Transition transition = { old_state, new_state, wall_time_secs };

  g_array_append_val (self->transitions, transition);

  self->user_state = new_state;

  if (debug_log)
    {
      g_autofree char *time_str = unix_to_string (wall_time_secs);

      g_debug ("TimeLimitsManager: User state changed from %s to %s at %s",
               user_state_to_string (old_state), user_state_to_string (new_state), time_str);
    }

  /* This potentially changed the limit time and timeout calculations. */
  if (recalculate_state && self->state != SHELL_TIME_LIMITS_STATE_DISABLED)
    {
      g_object_freeze_notify (G_OBJECT (self));
      notify_prop (self, PROP_DAILY_LIMIT_TIME);
      update_state (self);
      g_object_thaw_notify (G_OBJECT (self));
    }
}

/* Adjust all the stored real/wall clock times by +@offset_secs.
 *
 * This is used when the system real clock changes with respect to the
 * monotonic clock (for example, after an NTP synchronisation). At that
 * point, all of the stored real/wall clock times have a constant non-zero
 * offset to the new real/wall clock time, which leads to incorrect
 * calculations of daily usage. Hence, they need to be adjusted.
 *
 * We can do this while the system is running. If the clock offset changes
 * while offline (for example, while another user is logged in instead), we
 * cannot do anything about it and will just have to skip erroneous old state
 * until everything eventually gets updated to the new clock by the passage
 * of time.
 *
 * It is recommended to store the transitions after this, as the in-memory list
 * of transitions is adjusted. */
static void
adjust_all_times (ShellTimeLimitsManager *self,
                  gint64                  offset_secs)
{
  guint i;

  g_debug ("TimeLimitsManager: Adjusting all times by %" G_GINT64_FORMAT "s", offset_secs);

  for (i = 0; i < self->transitions->len; i++)
    get_transition (self, i)->wall_time_secs += offset_secs;

  if (self->last_state_change_time_secs != 0)
    self->last_state_change_time_secs += offset_secs;

  self->clock_offset_secs += offset_secs;
}

static ShellTimeLimitsUserState
calculate_user_state_from_logind (ShellTimeLimitsManager *self)
{
  gboolean is_active = g_strcmp0 (self->env.login_user_get_state (self->env.user_data), "active") == 0 &&
                       !self->env.login_user_get_idle_hint (self->env.user_data) &&
                       !self->env.login_manager_get_preparing_for_sleep (self->env.user_data);

  return is_active ? SHELL_TIME_LIMITS_USER_STATE_ACTIVE : SHELL_TIME_LIMITS_USER_STATE_INACTIVE;
}

static gboolean
on_update_state_timeout (gpointer user_data)
{
  ShellTimeLimitsManager *self = user_data;

  self->timer_id = 0;
  g_debug ("TimeLimitsManager: Scheduled state update");
  update_state (self);

  return G_SOURCE_REMOVE;
}

static void
schedule_update_state (ShellTimeLimitsManager *self,
                       gint64                  timeout)
{
  if (self->timer_id != 0)
    self->env.source_remove (self->env.user_data, self->timer_id);

  /* Never spin, and never ask GLib for something it cannot represent */
  timeout = CLAMP (timeout, 1, MAX_TIMEOUT_SECONDS);

  g_debug ("TimeLimitsManager: Scheduling state update in %" G_GINT64_FORMAT "s", timeout);

  self->timer_id = self->env.timeout_add_seconds (self->env.user_data, (guint) timeout,
                                                  on_update_state_timeout, self);
}

static void
update_state (ShellTimeLimitsManager *self)
{
  gint64 now_secs, start_of_today_secs, start_of_tomorrow_secs;
  ShellTimeLimitsState new_state = self->state;
  gboolean session_limits_enabled;
  guint64 current_session_start = 0, current_session_end = 0, next_session_start = 0;
  gint64 active_time_today_secs;
  gint64 wellbeing_daily_limit_secs;
  gboolean limit_enabled;

  if (self->state == SHELL_TIME_LIMITS_STATE_DISABLED)
    {
      g_debug ("TimeLimitsManager: Time limits should not be disabled when updating timer");
      return;
    }

  now_secs = shell_time_limits_manager_get_current_time (self);
  get_start_of_today_secs (now_secs, &start_of_today_secs, &start_of_tomorrow_secs);

  /* Is it a new day since we last updated the state? If so, reset the time
   * limit. */
  if (start_of_today_secs > self->last_state_change_time_secs)
    new_state = SHELL_TIME_LIMITS_STATE_ACTIVE;

  /* Work out estimated times for the parental controls session limits
   * calculations. */
  session_limits_enabled = self->env.session_limits_enabled (self->env.user_data);

  if (self->have_estimated_times)
    {
      current_session_start = self->estimated_times[1];
      current_session_end = self->estimated_times[2];
      next_session_start = self->estimated_times[3];
    }

  if (session_limits_enabled)
    {
      /* Parental controls session limits have either just been enabled and
       * the estimated times have not been cached yet, or disabled but the
       * manager has not detected that yet, so skip updating state. */
      if (!self->have_estimated_times)
        return;

      {
        g_autofree char *start_str = unix_to_string ((gint64) current_session_start);
        g_autofree char *end_str = unix_to_string ((gint64) current_session_end);

        g_debug ("TimeLimitsManager: Parental controls session limits: current session start: %s, current session end: %s",
                 start_str, end_str);
      }
    }
  else
    {
      g_debug ("TimeLimitsManager: Parental controls session limits: disabled");
    }

  /* Work out how much time the user has spent at the screen today for the
   * wellbeing daily limit calculations. */
  active_time_today_secs = calculate_active_time_today_secs (self, now_secs, start_of_today_secs);
  wellbeing_daily_limit_secs = self->env.settings_get_uint (self->env.user_data, "daily-limit-seconds");
  limit_enabled = wellbeing_daily_limit_enabled (self);

  g_debug ("TimeLimitsManager: Wellbeing active time today: %" G_GINT64_FORMAT "s, daily limit %" G_GINT64_FORMAT "s (enabled: %d)",
           active_time_today_secs, wellbeing_daily_limit_secs, limit_enabled);

  /* Update the state. When the user is inactive, there is no point scheduling
   * anything until they become active again. */
  if (session_limits_enabled)
    {
      if (next_session_start <= (guint64) now_secs)
        {
          /* Just entered daily schedule or a new day, so update cached
           * estimated times, which will perform state update afterwards. */
          update_estimated_times_async (self, log_error, (gpointer) "Failed to update the estimated times");
          return;
        }

      if ((guint64) now_secs >= current_session_end)
        {
          new_state = SHELL_TIME_LIMITS_STATE_LIMIT_REACHED;

          /* Schedule an update for when the session limit will be reset again. */
          schedule_update_state (self, (gint64) MIN (next_session_start, (guint64) G_MAXINT64) - now_secs);
        }
      else if (self->user_state == SHELL_TIME_LIMITS_USER_STATE_ACTIVE)
        {
          new_state = SHELL_TIME_LIMITS_STATE_ACTIVE;

          /* Schedule an update for when we expect the session limit will be reached. */
          schedule_update_state (self, (gint64) MIN (current_session_end, (guint64) G_MAXINT64) - now_secs);
        }
    }
  else if (limit_enabled && active_time_today_secs >= wellbeing_daily_limit_secs)
    {
      new_state = SHELL_TIME_LIMITS_STATE_LIMIT_REACHED;

      /* Schedule an update for when the daily limit will be reset again. */
      schedule_update_state (self, start_of_tomorrow_secs - now_secs);
    }
  else if (self->user_state == SHELL_TIME_LIMITS_USER_STATE_ACTIVE)
    {
      new_state = SHELL_TIME_LIMITS_STATE_ACTIVE;

      /* Schedule an update for when we expect the wellbeing daily limit will
       * be reached. */
      if (limit_enabled)
        schedule_update_state (self, wellbeing_daily_limit_secs - active_time_today_secs);
    }
  else
    {
      /* User is inactive, so no point scheduling anything until they become
       * active again. */
    }

  /* Update the saved state. */
  if (new_state != self->state)
    {
      g_debug ("TimeLimitsManager: State changed from %s to %s",
               shell_time_limits_state_to_string (self->state),
               shell_time_limits_state_to_string (new_state));
      self->state = new_state;
      self->last_state_change_time_secs = now_secs;
      notify_prop (self, PROP_STATE);
      notify_prop (self, PROP_SHOULD_LOCK_SESSION);
      notify_prop (self, PROP_DAILY_LIMIT_TIME);

      if (new_state == SHELL_TIME_LIMITS_STATE_LIMIT_REACHED)
        g_signal_emit (self, signals[DAILY_LIMIT_REACHED], 0);
    }
}

/* Returns TRUE if the state machine was started or is running */
static gboolean
update_state_machine (ShellTimeLimitsManager *self)
{
  /* The state file is written for the last time by shutdown_async () */
  if (self->shut_down)
    return FALSE;

  if (!storing_transitions_enabled (self))
    {
      if (self->state != SHELL_TIME_LIMITS_STATE_DISABLED)
        stop_state_machine_async (self, log_error, (gpointer) "Failed to stop state machine");
      return FALSE;
    }

  /* If this is the first time update_state_machine () has been called, start
   * the state machine. */
  if (self->state == SHELL_TIME_LIMITS_STATE_DISABLED)
    start_state_machine (self);
  else
    update_state (self);

  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Inhibitor                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellTimeLimitsManager *self;
  DoneFunc done;
  gpointer done_data;
} InhibitData;

static void
on_inhibit_ready (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  InhibitData *data = user_data;
  ShellTimeLimitsManager *self = data->self;
  g_autoptr (GError) error = NULL;
  g_autoptr (GInputStream) inhibitor = NULL;

  inhibitor = self->env.login_manager_inhibit_finish (self->env.user_data, result, &error);
  if (inhibitor == NULL && error != NULL &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("Failed to inhibit suspend: %s", error->message);

  if (inhibitor != NULL)
    {
      if (self->inhibitor == NULL && self->state != SHELL_TIME_LIMITS_STATE_DISABLED)
        self->inhibitor = g_steal_pointer (&inhibitor);
      else
        g_input_stream_close (inhibitor, NULL, NULL);
    }

  if (data->done != NULL)
    data->done (self, NULL, data->done_data);

  op_end (self);
  g_object_unref (self);
  g_free (data);
}

static void
ensure_inhibitor_async (ShellTimeLimitsManager *self,
                        DoneFunc                done,
                        gpointer                done_data)
{
  InhibitData *data;

  if (self->inhibitor != NULL)
    {
      if (done != NULL)
        done (self, NULL, done_data);
      return;
    }

  data = g_new0 (InhibitData, 1);
  data->self = g_object_ref (self);
  data->done = done;
  data->done_data = done_data;

  op_begin (self);
  self->env.login_manager_inhibit_async (self->env.user_data,
                                         _("GNOME needs to save screen time data"),
                                         self->cancellable,
                                         on_inhibit_ready, data);
}

static void
release_inhibitor (ShellTimeLimitsManager *self)
{
  if (self->inhibitor != NULL)
    g_input_stream_close (self->inhibitor, NULL, NULL);
  g_clear_object (&self->inhibitor);
}

/* ------------------------------------------------------------------------ */
/* The timer daemon                                                          */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellTimeLimitsManager *self;
  gint64 latest_usage_end_secs;
  DoneFunc done;
  gpointer done_data;
} RecordUsageData;

static void
on_record_usage_ready (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  RecordUsageData *data = user_data;
  ShellTimeLimitsManager *self = data->self;
  g_autoptr (GError) error = NULL;
  g_autoptr (GError) propagated = NULL;

  if (self->env.timer_record_usage_finish (self->env.user_data, result, &error))
    {
      self->latest_usage_end_secs = data->latest_usage_end_secs;
    }
  else if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
    {
      /* Limits should only be enabled when the daemon is available. */
      if (self->env.session_limits_enabled (self->env.user_data))
        propagated = g_steal_pointer (&error);
      else
        g_debug ("Parental controls timer daemon not available");
    }
  else
    {
      g_message ("Failed to record usage: %s", error->message);
    }

  data->done (self, propagated, data->done_data);

  op_end (self);
  g_object_unref (self);
  g_free (data);
}

static void
record_usage_async (ShellTimeLimitsManager *self,
                    GArray                 *records,
                    DoneFunc                done,
                    gpointer                done_data)
{
  RecordUsageData *data;
  GVariantBuilder builder;
  gint64 latest_usage_end_secs = 0;
  guint i;

  /* Build the array of records expected by the D-Bus API */
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ttss)"));

  for (i = 0; i < records->len; i++)
    {
      UsageRecord *record = &g_array_index (records, UsageRecord, i);
      g_autofree char *start_str = unix_to_string (record->start_secs);
      g_autofree char *end_str = unix_to_string (record->end_secs);

      g_debug ("Recording usage to the parental controls daemon: %s %s", start_str, end_str);

      g_variant_builder_add (&builder, "(ttss)",
                             (guint64) record->start_secs,
                             (guint64) record->end_secs,
                             "login-session", "");

      /* Update the limit of which records to re-send in future. We expect all
       * the records to be in order, but it seems more robust to assume they
       * might not be, just in case. Records might end up being out of order
       * wrt self->latest_usage_end_secs when initially loading the session
       * history at startup. */
      if (record->end_secs > latest_usage_end_secs)
        latest_usage_end_secs = record->end_secs;
    }

  data = g_new0 (RecordUsageData, 1);
  data->self = g_object_ref (self);
  data->latest_usage_end_secs = latest_usage_end_secs;
  data->done = done;
  data->done_data = done_data;

  op_begin (self);
  self->env.timer_record_usage_async (self->env.user_data,
                                      g_variant_builder_end (&builder),
                                      on_record_usage_ready, data);
}

typedef struct
{
  ShellTimeLimitsManager *self;
  DoneFunc done;
  gpointer done_data;
} EstimatedTimesData;

static void
on_estimated_times_ready (GObject      *source,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  EstimatedTimesData *data = user_data;
  ShellTimeLimitsManager *self = data->self;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) times = NULL;

  if (self->env.timer_get_estimated_times_finish (self->env.user_data, result, &times, &error))
    {
      gboolean limit_reached;
      guint64 current_start, current_end, next_start, next_end;

      if (g_variant_lookup (times, "", "(btttt)",
                            &limit_reached, &current_start, &current_end,
                            &next_start, &next_end))
        {
          self->have_estimated_times = TRUE;
          self->estimated_times[0] = limit_reached;
          self->estimated_times[1] = current_start;
          self->estimated_times[2] = current_end;
          self->estimated_times[3] = next_start;
          self->estimated_times[4] = next_end;
        }
      else
        {
          self->have_estimated_times = FALSE;
        }

      update_state (self);
      notify_prop (self, PROP_DAILY_LIMIT_TIME);
    }

  if (data->done != NULL)
    data->done (self, error, data->done_data);

  op_end (self);
  g_object_unref (self);
  g_free (data);
}

static void
update_estimated_times_async (ShellTimeLimitsManager *self,
                              DoneFunc                done,
                              gpointer                done_data)
{
  EstimatedTimesData *data = g_new0 (EstimatedTimesData, 1);

  data->self = g_object_ref (self);
  data->done = done;
  data->done_data = done_data;

  op_begin (self);
  self->env.timer_get_estimated_times_async (self->env.user_data,
                                             on_estimated_times_ready, data);
}

/* ------------------------------------------------------------------------ */
/* The history file                                                          */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellTimeLimitsManager *self;
  GCancellable *cancellable;
  DoneFunc done;
  gpointer done_data;
} FileData;

static FileData *
file_data_new (ShellTimeLimitsManager *self,
               GCancellable           *cancellable,
               DoneFunc                done,
               gpointer                done_data)
{
  FileData *data = g_new0 (FileData, 1);

  data->self = g_object_ref (self);
  data->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;
  data->done = done;
  data->done_data = done_data;

  op_begin (self);

  return data;
}

static void
file_data_finish (FileData     *data,
                  const GError *error)
{
  ShellTimeLimitsManager *self = data->self;

  if (data->done != NULL)
    data->done (self, error, data->done_data);

  op_end (self);
  g_clear_object (&data->cancellable);
  g_free (data);
  g_object_unref (self);
}

static void
on_delete_ready (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  FileData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!g_file_delete_finish (G_FILE (source), result, &error) &&
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    g_clear_error (&error);

  file_data_finish (data, error);
}

static void
delete_history_file_async (ShellTimeLimitsManager *self,
                           GCancellable           *cancellable,
                           DoneFunc                done,
                           gpointer                done_data)
{
  FileData *data = file_data_new (self, cancellable, done, done_data);
  g_autofree char *path = g_file_get_path (self->history_file);

  g_debug ("TimeLimitsManager: Deleting screen time limits data in ‘%s’", path);

  g_file_delete_async (self->history_file, G_PRIORITY_DEFAULT, cancellable,
                       on_delete_ready, data);
}

static void
on_replace_ready (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  FileData *data = user_data;
  g_autoptr (GError) error = NULL;

  g_file_replace_contents_finish (G_FILE (source), result, NULL, &error);

  file_data_finish (data, error);
}

static GBytes *
serialize_transitions (ShellTimeLimitsManager *self)
{
  GString *string = g_string_new ("[");
  guint i;

  for (i = 0; i < self->transitions->len; i++)
    {
      Transition *transition = get_transition (self, i);

      if (i > 0)
        g_string_append_c (string, ',');
      g_string_append_printf (string,
                              "{\"oldState\":%d,\"newState\":%d,\"wallTimeSecs\":%" G_GINT64_FORMAT "}",
                              transition->old_state, transition->new_state,
                              transition->wall_time_secs);
    }

  g_string_append_c (string, ']');

  return g_string_free_to_bytes (string);
}

typedef struct
{
  ShellTimeLimitsManager *self;
  GCancellable *cancellable;
  GArray *usage_records;
  DoneFunc done;
  gpointer done_data;
} StoreData;

static void
store_data_free (StoreData *data)
{
  g_clear_object (&data->cancellable);
  g_clear_pointer (&data->usage_records, g_array_unref);
  g_object_unref (data->self);
  g_free (data);
}

static void
store_write_history (StoreData *data)
{
  ShellTimeLimitsManager *self = data->self;
  g_autofree char *path = g_file_get_path (self->history_file);

  g_debug ("TimeLimitsManager: Storing screen time limits data to ‘%s’", path);

  if (self->transitions->len == 0)
    {
      delete_history_file_async (self, data->cancellable, data->done, data->done_data);
    }
  else
    {
      g_autoptr (GBytes) contents = serialize_transitions (self);
      FileData *file_data = file_data_new (self, data->cancellable, data->done, data->done_data);

      g_file_replace_contents_bytes_async (self->history_file, contents, NULL, FALSE,
                                           G_FILE_CREATE_PRIVATE, data->cancellable,
                                           on_replace_ready, file_data);
    }

  store_data_free (data);
}

static void
on_store_usage_recorded (ShellTimeLimitsManager *self,
                         const GError           *error,
                         gpointer                user_data)
{
  StoreData *data = user_data;

  if (error != NULL)
    {
      data->done (self, error, data->done_data);
      store_data_free (data);
      return;
    }

  store_write_history (data);
}

static void
store_transitions_async (ShellTimeLimitsManager *self,
                         GCancellable           *cancellable,
                         DoneFunc                done,
                         gpointer                done_data)
{
  StoreData *data;
  gint64 now_secs = shell_time_limits_manager_get_current_time (self);
  GArray *new_transitions;
  guint i, j;

  /* Trim the transitions array to drop old history. */
  new_transitions = g_array_new (FALSE, FALSE, sizeof (Transition));
  for (i = 0; i < self->transitions->len; i++)
    {
      Transition *transition = get_transition (self, i);

      if (filter_old_transition (transition->wall_time_secs, i, self->transitions->len, now_secs))
        g_array_append_val (new_transitions, *transition);
    }
  g_array_unref (self->transitions);
  self->transitions = new_transitions;

  /* Filter the transitions pairwise to remove consecutive pairs where the
   * wall_time_secs are equal and the states cancel each other out. This can
   * happen if the user rapidly toggles screen time support on and off. */
  new_transitions = g_array_new (FALSE, FALSE, sizeof (Transition));
  for (i = 0; i < self->transitions->len; i++)
    {
      Transition *first = get_transition (self, i);
      Transition *second = i + 1 < self->transitions->len ? get_transition (self, i + 1) : NULL;

      if (second == NULL ||
          !(first->wall_time_secs == second->wall_time_secs &&
            first->old_state == second->new_state))
        g_array_append_val (new_transitions, *first);
    }
  g_array_unref (self->transitions);
  self->transitions = new_transitions;

  data = g_new0 (StoreData, 1);
  data->self = g_object_ref (self);
  data->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;
  data->done = done;
  data->done_data = done_data;

  /* Record all of the usage records to the parental controls timer daemon if
   * the parental controls session limits are in place. */
  if (self->env.session_limits_enabled (self->env.user_data))
    {
      GArray *usage_records = g_array_new (FALSE, FALSE, sizeof (UsageRecord));

      for (j = 0; j + 1 < self->transitions->len; j++)
        {
          Transition *start = get_transition (self, j);
          Transition *end = get_transition (self, j + 1);
          UsageRecord record;

          /* Make sure the transition is correct */
          if (start->new_state != SHELL_TIME_LIMITS_USER_STATE_ACTIVE ||
              end->new_state != SHELL_TIME_LIMITS_USER_STATE_INACTIVE)
            continue;

          record.start_secs = start->wall_time_secs;
          record.end_secs = end->wall_time_secs;

          /* Avoid re-sending the old state transitions */
          if (record.end_secs <= self->latest_usage_end_secs)
            continue;

          g_array_append_val (usage_records, record);
        }

      if (usage_records->len > 0)
        {
          record_usage_async (self, usage_records, on_store_usage_recorded, data);
          g_array_unref (usage_records);
          return;
        }

      g_array_unref (usage_records);
    }

  store_write_history (data);
}

typedef struct
{
  ShellTimeLimitsManager *self;
  DoneFunc done;
  gpointer done_data;
} LoadData;

/**
 * parse_transitions:
 *
 * Load the transitions JSON.
 *
 * The format is a top-level array. Each array element is an object
 * containing exactly the following elements:
 *  - `oldState`: integer value from #ShellTimeLimitsUserState
 *  - `newState`: integer value from #ShellTimeLimitsUserState
 *  - `wallTimeSecs`: number of seconds since the UNIX epoch at which the
 *       state transition happened
 *
 * `oldState` and `newState` must not be equal. `wallTimeSecs` must
 * monotonically increase from one array element to the next, and must be a
 * ‘safe integer’ (an integer of at most 53 bits).
 *
 * The use of wall/real time in the file means its contents become invalid
 * if the system real time clock changes (relative to the monotonic clock).
 */
static gboolean
node_get_integer (JsonNode *node,
                  gint64   *value)
{
  GValue gvalue = G_VALUE_INIT;
  gboolean ok = FALSE;

  if (node == NULL || json_node_get_node_type (node) != JSON_NODE_VALUE)
    return FALSE;

  json_node_get_value (node, &gvalue);

  if (G_VALUE_HOLDS_INT64 (&gvalue))
    {
      *value = g_value_get_int64 (&gvalue);
      ok = TRUE;
    }
  else if (G_VALUE_HOLDS_DOUBLE (&gvalue))
    {
      double d = g_value_get_double (&gvalue);

      if (isfinite (d) && floor (d) == d && fabs (d) < 9007199254740992.0)
        {
          *value = (gint64) d;
          ok = TRUE;
        }
    }

  g_value_unset (&gvalue);

  return ok;
}

static gboolean
parse_transitions (ShellTimeLimitsManager  *self,
                   const char              *contents,
                   gsize                    length,
                   GError                 **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autofree char *path = g_file_get_path (self->history_file);
  JsonNode *root;
  JsonArray *history;
  gint64 now_secs = shell_time_limits_manager_get_current_time (self);
  gint64 previous_wall_time_secs = 0;
  guint n_history, i;

  if (!json_parser_load_from_data (parser, contents, length, error))
    return FALSE;

  root = json_parser_get_root (parser);
  if (root == NULL || json_node_get_node_type (root) != JSON_NODE_ARRAY)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "%s does not contain an array", path);
      return FALSE;
    }

  history = json_node_get_array (root);
  n_history = json_array_get_length (history);

  g_object_freeze_notify (G_OBJECT (self));

  for (i = 0; i < n_history; i++)
    {
      JsonNode *entry_node = json_array_get_element (history, i);
      JsonObject *entry;
      gint64 old_state, new_state, wall_time_secs;

      if (json_node_get_node_type (entry_node) != JSON_NODE_OBJECT)
        goto malformed;

      entry = json_node_get_object (entry_node);

      if (!json_object_has_member (entry, "oldState") ||
          !json_object_has_member (entry, "newState") ||
          !json_object_has_member (entry, "wallTimeSecs") ||
          !node_get_integer (json_object_get_member (entry, "oldState"), &old_state) ||
          !node_get_integer (json_object_get_member (entry, "newState"), &new_state) ||
          !node_get_integer (json_object_get_member (entry, "wallTimeSecs"), &wall_time_secs) ||
          (old_state != SHELL_TIME_LIMITS_USER_STATE_INACTIVE &&
           old_state != SHELL_TIME_LIMITS_USER_STATE_ACTIVE) ||
          (new_state != SHELL_TIME_LIMITS_USER_STATE_INACTIVE &&
           new_state != SHELL_TIME_LIMITS_USER_STATE_ACTIVE) ||
          old_state == new_state ||
          wall_time_secs < previous_wall_time_secs)
        goto malformed;

      /* Skip entries older than the threshold, but always keep at least one. */
      if (!filter_old_transition (wall_time_secs, i, n_history, now_secs))
        continue;

      add_transition (self, old_state, new_state, wall_time_secs,
                      i == n_history - 1, FALSE);
      previous_wall_time_secs = wall_time_secs;
    }

  g_debug ("TimeLimitsManager: Loaded %u transitions from history", n_history);

  g_object_thaw_notify (G_OBJECT (self));

  return TRUE;

malformed:
  g_object_thaw_notify (G_OBJECT (self));
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "Malformed entry (index %u) in %s", i, path);

  return FALSE;
}

static void
load_data_finish (LoadData     *data,
                  const GError *error)
{
  ShellTimeLimitsManager *self = data->self;

  data->done (self, error, data->done_data);

  op_end (self);
  g_free (data);
  g_object_unref (self);
}

static void
on_load_contents_ready (GObject      *source,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  LoadData *data = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  gsize length = 0;

  if (!g_file_load_contents_finish (G_FILE (source), result, &contents, &length, NULL, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_clear_error (&error);

      load_data_finish (data, error);
      return;
    }

  parse_transitions (data->self, contents, length, &error);
  load_data_finish (data, error);
}

static void
load_history_file (LoadData *data)
{
  ShellTimeLimitsManager *self = data->self;

  g_file_load_contents_async (self->history_file, self->cancellable,
                              on_load_contents_ready, data);
}

static void
on_load_estimated_times (ShellTimeLimitsManager *self,
                         const GError           *error,
                         gpointer                user_data)
{
  LoadData *data = user_data;

  if (error != NULL)
    {
      load_data_finish (data, error);
      return;
    }

  g_debug ("TimeLimitsManager: Loaded time estimates from the timer daemon");
  load_history_file (data);
}

static void
load_transitions_async (ShellTimeLimitsManager *self,
                        DoneFunc                done,
                        gpointer                done_data)
{
  LoadData *data = g_new0 (LoadData, 1);

  data->self = g_object_ref (self);
  data->done = done;
  data->done_data = done_data;

  op_begin (self);

  /* When parental controls session limits are enabled, additionally update
   * the cached estimated times from the parental controls timer daemon. */
  if (self->env.session_limits_enabled (self->env.user_data))
    update_estimated_times_async (self, on_load_estimated_times, data);
  else
    load_history_file (data);
}

/* ------------------------------------------------------------------------ */
/* The state machine                                                         */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellTimeLimitsManager *self;
  DoneFunc done;
  gpointer done_data;
} UserStateData;

static void
on_user_state_stored (ShellTimeLimitsManager *self,
                      const GError           *error,
                      gpointer                user_data)
{
  UserStateData *data = user_data;

  log_error (self, error, (gpointer) "Failed to store screen time limits data");

  if (data->done != NULL)
    data->done (self, NULL, data->done_data);

  op_end (self);
  g_object_unref (self);
  g_free (data);
}

static void
update_user_state_async (ShellTimeLimitsManager *self,
                         gboolean                store_updates,
                         DoneFunc                done,
                         gpointer                done_data)
{
  ShellTimeLimitsUserState old_state = self->user_state;
  ShellTimeLimitsUserState new_state;

  if (!self->login_user_ready)
    {
      if (done != NULL)
        done (self, NULL, done_data);
      return;
    }

  new_state = calculate_user_state_from_logind (self);

  if (old_state != new_state)
    {
      add_transition (self, old_state, new_state,
                      shell_time_limits_manager_get_current_time (self), TRUE, TRUE);

      if (store_updates)
        {
          UserStateData *data = g_new0 (UserStateData, 1);

          data->self = g_object_ref (self);
          data->done = done;
          data->done_data = done_data;

          op_begin (self);
          store_transitions_async (self, self->cancellable, on_user_state_stored, data);
          return;
        }
    }

  if (done != NULL)
    done (self, NULL, done_data);
}

static void
on_user_changed (gpointer user_data)
{
  ShellTimeLimitsManager *self = user_data;

  update_user_state_async (self, TRUE, NULL, NULL);
}

typedef struct
{
  gboolean preparing_for_sleep;
} SleepData;

static void
on_sleep_user_state_updated (ShellTimeLimitsManager *self,
                             const GError           *error,
                             gpointer                user_data)
{
  SleepData *data = user_data;

  /* Release the inhibitor if we are preparing to sleep. Also avoid adjusting
   * the time due to offset changes caused by the monotonic clock not
   * advancing during sleep until we have updated the current offset to
   * account for this. */
  if (data->preparing_for_sleep)
    {
      self->ignore_clock_offset_changes = TRUE;
      release_inhibitor (self);
    }

  g_free (data);
}

static void
on_prepare_for_sleep (gboolean preparing_for_sleep,
                      gpointer user_data)
{
  ShellTimeLimitsManager *self = user_data;
  SleepData *data = g_new0 (SleepData, 1);

  data->preparing_for_sleep = preparing_for_sleep;

  /* Just come back from sleep, so take another inhibitor. Also update the
   * clock offset to account for the monotonic clock not advancing during
   * sleep. */
  if (!preparing_for_sleep)
    {
      ensure_inhibitor_async (self, NULL, NULL);
      self->clock_offset_secs = get_clock_offset (self);
      self->ignore_clock_offset_changes = FALSE;
    }

  update_user_state_async (self, TRUE, on_sleep_user_state_updated, data);
}

static gboolean
on_time_change (gpointer user_data)
{
  ShellTimeLimitsManager *self = user_data;
  gint64 new_clock_offset_secs, old_clock_offset_secs;

  if (self->ignore_clock_offset_changes)
    return G_SOURCE_CONTINUE;

  new_clock_offset_secs = get_clock_offset (self);
  old_clock_offset_secs = self->clock_offset_secs;

  g_debug ("TimeLimitsManager: System clock changed, old offset %" G_GINT64_FORMAT "s, new offset %" G_GINT64_FORMAT "s",
           old_clock_offset_secs, new_clock_offset_secs);

  if (new_clock_offset_secs == old_clock_offset_secs)
    return G_SOURCE_CONTINUE;

  adjust_all_times (self, new_clock_offset_secs - old_clock_offset_secs);
  self->clock_offset_secs = new_clock_offset_secs;

  store_transitions_async (self, self->cancellable, log_error,
                           (gpointer) "Failed to store screen time limits data");

  g_object_freeze_notify (G_OBJECT (self));
  notify_prop (self, PROP_DAILY_LIMIT_TIME);
  update_state (self);
  g_object_thaw_notify (G_OBJECT (self));

  return G_SOURCE_CONTINUE;
}

typedef struct
{
  ShellTimeLimitsManager *self;
  guint run_id;
  gboolean frozen;
} StartData;

static void
start_data_free (StartData *data)
{
  ShellTimeLimitsManager *self = data->self;

  if (data->frozen)
    g_object_thaw_notify (G_OBJECT (self));

  op_end (self);
  g_free (data);
  g_object_unref (self);
}

static gboolean
start_data_is_current (StartData *data)
{
  return data->run_id == data->self->run_id;
}

static void
on_start_user_state_updated (ShellTimeLimitsManager *self,
                             const GError           *error,
                             gpointer                user_data)
{
  StartData *data = user_data;

  if (start_data_is_current (data))
    update_state (self);

  start_data_free (data);
}

static void
on_start_login_user_ready (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  StartData *data = user_data;
  ShellTimeLimitsManager *self = data->self;
  g_autoptr (GError) error = NULL;

  if (!self->env.login_user_get_finish (self->env.user_data, result, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Failed to start state machine: %s", error->message);
      start_data_free (data);
      return;
    }

  if (!start_data_is_current (data))
    {
      start_data_free (data);
      return;
    }

  self->login_user_ready = TRUE;
  self->login_user_id = self->env.login_user_connect (self->env.user_data,
                                                      on_user_changed, self);
  update_user_state_async (self, FALSE, on_start_user_state_updated, data);
}

static void
on_start_inhibited (ShellTimeLimitsManager *self,
                    const GError           *error,
                    gpointer                user_data)
{
  StartData *data = user_data;

  if (!start_data_is_current (data))
    {
      start_data_free (data);
      return;
    }

  self->login_manager_id = self->env.login_manager_connect (self->env.user_data,
                                                            on_prepare_for_sleep, self);

  self->env.login_user_get_async (self->env.user_data, self->cancellable,
                                  on_start_login_user_ready, data);
}

static void
on_start_loaded (ShellTimeLimitsManager *self,
                 const GError           *error,
                 gpointer                user_data)
{
  StartData *data = user_data;

  if (!start_data_is_current (data))
    {
      start_data_free (data);
      return;
    }

  /* Warn on failure, but carry on anyway. */
  if (error != NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("Failed to load screen time limits data: %s", error->message);

  /* Add a fake transition to show the startup. */
  if (self->transitions->len == 0 ||
      self->user_state != SHELL_TIME_LIMITS_USER_STATE_ACTIVE)
    {
      gint64 now_secs = shell_time_limits_manager_get_current_time (self);

      add_transition (self, SHELL_TIME_LIMITS_USER_STATE_INACTIVE,
                      SHELL_TIME_LIMITS_USER_STATE_ACTIVE, now_secs, TRUE, TRUE);
    }

  /* Start listening for clock change notifications. */
  self->clock_offset_secs = get_clock_offset (self);
  self->time_change_id = self->env.time_change_notify (self->env.user_data,
                                                       on_time_change, self);

  /* Start listening for notifications to the user's state. Listening to the
   * prepare-for-sleep signal requires taking a delay inhibitor to avoid
   * races. */
  ensure_inhibitor_async (self, on_start_inhibited, data);
}

static void
start_state_machine (ShellTimeLimitsManager *self)
{
  StartData *data;

  /* Start off active because gnome-shell is started inside the user's
   * session, so by the time we get to this code, the user should be active. */
  self->user_state = SHELL_TIME_LIMITS_USER_STATE_ACTIVE;
  g_array_set_size (self->transitions, 0);
  self->state = SHELL_TIME_LIMITS_STATE_ACTIVE;
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();
  self->run_id++;

  data = g_new0 (StartData, 1);
  data->self = g_object_ref (self);
  data->run_id = self->run_id;
  data->frozen = TRUE;
  op_begin (self);

  g_object_freeze_notify (G_OBJECT (self));

  /* Load the previously saved transitions. Discard any entries older than the
   * threshold, since we do not care about historical data. */
  load_transitions_async (self, on_start_loaded, data);
}

typedef struct
{
  GCancellable *cancellable;
  DoneFunc done;
  gpointer done_data;
} StopData;

static void
on_stop_finished (ShellTimeLimitsManager *self,
                  const GError           *error,
                  gpointer                user_data)
{
  StopData *data = user_data;

  if (error != NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("Failed to store screen time limits data: %s", error->message);

  /* Make sure no async operations are still pending. */
  g_cancellable_cancel (data->cancellable);
  g_object_unref (data->cancellable);

  if (data->done != NULL)
    data->done (self, NULL, data->done_data);

  op_end (self);
  g_free (data);
  g_object_unref (self);
}

static void
stop_state_machine_async (ShellTimeLimitsManager *self,
                          DoneFunc                done,
                          gpointer                done_data)
{
  StopData *data;

  if (self->time_change_id != 0)
    self->env.source_remove (self->env.user_data, self->time_change_id);
  self->time_change_id = 0;
  self->clock_offset_secs = 0;

  if (self->timer_id != 0)
    self->env.source_remove (self->env.user_data, self->timer_id);
  self->timer_id = 0;

  if (self->login_user_id != 0)
    self->env.login_user_disconnect (self->env.user_data, self->login_user_id);
  self->login_user_id = 0;
  self->login_user_ready = FALSE;

  release_inhibitor (self);

  if (self->login_manager_id != 0)
    self->env.login_manager_disconnect (self->env.user_data, self->login_manager_id);
  self->login_manager_id = 0;

  self->state = SHELL_TIME_LIMITS_STATE_DISABLED;
  self->last_state_change_time_secs = 0;
  self->run_id++;
  notify_prop (self, PROP_STATE);

  data = g_new0 (StopData, 1);
  /* A new run gets a cancellable of its own */
  data->cancellable = self->cancellable != NULL ? g_steal_pointer (&self->cancellable) : g_cancellable_new ();
  data->done = done;
  data->done_data = done_data;

  g_object_ref (self);
  op_begin (self);

  if (storing_transitions_enabled (self))
    {
      /* Add a fake transition to show the shutdown. */
      if (self->user_state != SHELL_TIME_LIMITS_USER_STATE_INACTIVE)
        add_transition (self, SHELL_TIME_LIMITS_USER_STATE_ACTIVE,
                        SHELL_TIME_LIMITS_USER_STATE_INACTIVE,
                        shell_time_limits_manager_get_current_time (self), TRUE, TRUE);

      store_transitions_async (self, data->cancellable, on_stop_finished, data);
    }
  else
    {
      delete_history_file_async (self, data->cancellable, on_stop_finished, data);
    }
}

/* Environment signals */

static void
on_session_limits_changed (gpointer user_data)
{
  ShellTimeLimitsManager *self = user_data;

  /* New session limits may mean that the state machine will need to start/stop. */
  update_state_machine (self);

  /* Checkpoint current active usage to the parental controls timer daemon, so
   * that it can make a correct estimate of the remaining screen time.
   * Wellbeing monitoring uses the stored edge events (state transitions),
   * whereas the parental controls timer daemon stores time periods only after
   * one is finished, which is why it cannot know the start of the usage
   * period, without recording it first. Recording will trigger
   * EstimatedTimesChanged signal, which handler updates the estimated times
   * and state. */
  if (self->transitions->len > 0 &&
      get_transition (self, self->transitions->len - 1)->new_state == SHELL_TIME_LIMITS_USER_STATE_ACTIVE)
    {
      GArray *records = g_array_new (FALSE, FALSE, sizeof (UsageRecord));
      UsageRecord record;

      record.start_secs = get_transition (self, self->transitions->len - 1)->wall_time_secs;
      record.end_secs = shell_time_limits_manager_get_current_time (self);
      g_array_append_val (records, record);

      record_usage_async (self, records, log_error, (gpointer) "Failed to record usage");
      g_array_unref (records);
    }

  notify_prop (self, PROP_DAILY_LIMIT_TIME);
  notify_prop (self, PROP_DAILY_LIMIT_ENABLED);
  notify_prop (self, PROP_SHOULD_LOCK_SESSION);
}

static void
on_estimated_times_changed (gpointer user_data)
{
  ShellTimeLimitsManager *self = user_data;

  update_estimated_times_async (self, log_error, (gpointer) "Failed to update the estimated times");
}

static void
on_settings_changed (const char *key,
                     gpointer    user_data)
{
  ShellTimeLimitsManager *self = user_data;

  update_state_machine (self);

  if (g_strcmp0 (key, "daily-limit-seconds") == 0)
    notify_prop (self, PROP_DAILY_LIMIT_TIME);
  else if (g_strcmp0 (key, "daily-limit-enabled") == 0)
    notify_prop (self, PROP_DAILY_LIMIT_ENABLED);
  else if (g_strcmp0 (key, "grayscale") == 0)
    notify_prop (self, PROP_GRAYSCALE_ENABLED);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

static void
on_shutdown_done (ShellTimeLimitsManager *self,
                  const GError           *error,
                  gpointer                user_data)
{
  GTask *task = user_data;

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

/**
 * shell_time_limits_manager_shutdown_async:
 * @self: a #ShellTimeLimitsManager
 * @cancellable: (nullable): unused, the history is always written
 * @callback: (scope async) (closure user_data): called when the state machine
 *   is shut down and the state file is written
 * @user_data: user data for @callback
 *
 * Shuts down the state machine and writes out the state file. src/ui/main.c
 * blocks the shutdown of the shell until @callback is called.
 */
void
shell_time_limits_manager_shutdown_async (ShellTimeLimitsManager *self,
                                          GCancellable           *cancellable,
                                          GAsyncReadyCallback     callback,
                                          gpointer                user_data)
{
  GTask *task;

  g_return_if_fail (SHELL_IS_TIME_LIMITS_MANAGER (self));

  self->shut_down = TRUE;

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, shell_time_limits_manager_shutdown_async);

  stop_state_machine_async (self, on_shutdown_done, task);
}

/**
 * shell_time_limits_manager_shutdown_finish:
 * @self: a #ShellTimeLimitsManager
 * @result: a #GAsyncResult
 * @error: return location for a #GError
 *
 * Finishes shell_time_limits_manager_shutdown_async ().
 *
 * Returns: %TRUE
 */
gboolean
shell_time_limits_manager_shutdown_finish (ShellTimeLimitsManager  *self,
                                           GAsyncResult            *result,
                                           GError                 **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/**
 * shell_time_limits_manager_get_current_time:
 * @self: a #ShellTimeLimitsManager
 *
 * Returns: the current real time, in seconds since the Unix epoch
 */
gint64
shell_time_limits_manager_get_current_time (ShellTimeLimitsManager *self)
{
  g_return_val_if_fail (SHELL_IS_TIME_LIMITS_MANAGER (self), 0);

  return self->env.get_real_time_secs (self->env.user_data);
}

/**
 * shell_time_limits_manager_get_state:
 * @self: a #ShellTimeLimitsManager
 *
 * Returns: the current state machine state
 */
ShellTimeLimitsState
shell_time_limits_manager_get_state (ShellTimeLimitsManager *self)
{
  g_return_val_if_fail (SHELL_IS_TIME_LIMITS_MANAGER (self), SHELL_TIME_LIMITS_STATE_DISABLED);

  return self->state;
}

/**
 * shell_time_limits_manager_get_parental_controls_session_limits_enabled:
 * @self: a #ShellTimeLimitsManager
 *
 * Returns: whether the parental controls session limits are enabled
 */
gboolean
shell_time_limits_manager_get_parental_controls_session_limits_enabled (ShellTimeLimitsManager *self)
{
  g_return_val_if_fail (SHELL_IS_TIME_LIMITS_MANAGER (self), FALSE);

  return self->env.session_limits_enabled (self->env.user_data);
}

/**
 * shell_time_limits_manager_get_wellbeing_daily_limit_enabled:
 * @self: a #ShellTimeLimitsManager
 *
 * Returns: whether the wellbeing daily limit is enabled
 */
gboolean
shell_time_limits_manager_get_wellbeing_daily_limit_enabled (ShellTimeLimitsManager *self)
{
  g_return_val_if_fail (SHELL_IS_TIME_LIMITS_MANAGER (self), FALSE);

  return wellbeing_daily_limit_enabled (self);
}

/**
 * shell_time_limits_manager_get_daily_limit_time:
 * @self: a #ShellTimeLimitsManager
 *
 * The time when the daily limit will be reached. If the user is currently
 * active, and has not reached the limit, this is a non-zero value in the
 * future. If the user has already reached the limit, this is the time when
 * the limit was reached. If the user is inactive, and has not reached the
 * limit, or if time limits are disabled, this is zero. It is measured in real
 * time seconds.
 *
 * Considers both the parental controls and wellbeing limits.
 *
 * Returns: the time in seconds since the Unix epoch
 */
guint64
shell_time_limits_manager_get_daily_limit_time (ShellTimeLimitsManager *self)
{
  g_return_val_if_fail (SHELL_IS_TIME_LIMITS_MANAGER (self), 0);

  /* Check parental controls session limits */
  if (shell_time_limits_manager_get_parental_controls_session_limits_enabled (self))
    {
      if (self->state == SHELL_TIME_LIMITS_STATE_DISABLED ||
          self->user_state == SHELL_TIME_LIMITS_USER_STATE_INACTIVE ||
          !self->have_estimated_times)
        return 0;

      return self->estimated_times[2];
    }

  /* Handle wellbeing daily limit otherwise */
  switch (self->state)
    {
    case SHELL_TIME_LIMITS_STATE_DISABLED:
      return 0;

    case SHELL_TIME_LIMITS_STATE_ACTIVE:
      {
        gint64 now_secs, start_of_today_secs, start_of_tomorrow_secs;
        gint64 active_time_today_secs, wellbeing_daily_limit_secs;

        if (!wellbeing_daily_limit_enabled (self))
          return 0;

        now_secs = shell_time_limits_manager_get_current_time (self);
        get_start_of_today_secs (now_secs, &start_of_today_secs, &start_of_tomorrow_secs);
        active_time_today_secs = calculate_active_time_today_secs (self, now_secs, start_of_today_secs);
        wellbeing_daily_limit_secs = self->env.settings_get_uint (self->env.user_data,
                                                                  "daily-limit-seconds");

        if (self->user_state == SHELL_TIME_LIMITS_USER_STATE_ACTIVE)
          return now_secs + MAX (wellbeing_daily_limit_secs - active_time_today_secs, 0);
        else
          return 0;
      }

    case SHELL_TIME_LIMITS_STATE_LIMIT_REACHED:
      {
        gint64 now_secs, start_of_today_secs, start_of_tomorrow_secs;
        gint64 wellbeing_daily_limit_secs;

        now_secs = shell_time_limits_manager_get_current_time (self);
        get_start_of_today_secs (now_secs, &start_of_today_secs, &start_of_tomorrow_secs);
        wellbeing_daily_limit_secs = self->env.settings_get_uint (self->env.user_data,
                                                                  "daily-limit-seconds");

        return MAX (calculate_wellbeing_daily_limit_reached_at_secs (self, now_secs,
                                                                     wellbeing_daily_limit_secs,
                                                                     start_of_today_secs), 0);
      }

    default:
      g_assert_not_reached ();
    }
}

/**
 * shell_time_limits_manager_get_daily_limit_enabled:
 * @self: a #ShellTimeLimitsManager
 *
 * Whether the daily limit is enabled. Considers both the parental controls and
 * wellbeing limits.
 *
 * Returns: whether the daily limit is enabled
 */
gboolean
shell_time_limits_manager_get_daily_limit_enabled (ShellTimeLimitsManager *self)
{
  g_return_val_if_fail (SHELL_IS_TIME_LIMITS_MANAGER (self), FALSE);

  return shell_time_limits_manager_get_parental_controls_session_limits_enabled (self) ||
         wellbeing_daily_limit_enabled (self);
}

/**
 * shell_time_limits_manager_get_grayscale_enabled:
 * @self: a #ShellTimeLimitsManager
 *
 * Returns: whether the screen should be made grayscale once the daily limit
 *   is reached
 */
gboolean
shell_time_limits_manager_get_grayscale_enabled (ShellTimeLimitsManager *self)
{
  g_return_val_if_fail (SHELL_IS_TIME_LIMITS_MANAGER (self), FALSE);

  return self->env.settings_get_boolean (self->env.user_data, "grayscale");
}

/**
 * shell_time_limits_manager_get_should_lock_session:
 * @self: a #ShellTimeLimitsManager
 *
 * Returns: whether the session should be locked; %TRUE when the screen time
 *   limits has been reached with parental controls enabled
 */
gboolean
shell_time_limits_manager_get_should_lock_session (ShellTimeLimitsManager *self)
{
  g_return_val_if_fail (SHELL_IS_TIME_LIMITS_MANAGER (self), FALSE);

  return self->state == SHELL_TIME_LIMITS_STATE_LIMIT_REACHED &&
         shell_time_limits_manager_get_parental_controls_session_limits_enabled (self);
}

gboolean
shell_time_limits_manager_is_busy (ShellTimeLimitsManager *self)
{
  g_return_val_if_fail (SHELL_IS_TIME_LIMITS_MANAGER (self), FALSE);

  return self->n_pending > 0;
}

/* GObject */

static void
shell_time_limits_manager_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellTimeLimitsManager *self = SHELL_TIME_LIMITS_MANAGER (object);

  switch (prop_id)
    {
    case PROP_STATE:
      g_value_set_enum (value, self->state);
      break;

    case PROP_DAILY_LIMIT_TIME:
      g_value_set_uint64 (value, shell_time_limits_manager_get_daily_limit_time (self));
      break;

    case PROP_DAILY_LIMIT_ENABLED:
      g_value_set_boolean (value, shell_time_limits_manager_get_daily_limit_enabled (self));
      break;

    case PROP_GRAYSCALE_ENABLED:
      g_value_set_boolean (value, shell_time_limits_manager_get_grayscale_enabled (self));
      break;

    case PROP_SHOULD_LOCK_SESSION:
      g_value_set_boolean (value, shell_time_limits_manager_get_should_lock_session (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_time_limits_manager_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellTimeLimitsManager *self = SHELL_TIME_LIMITS_MANAGER (object);

  switch (prop_id)
    {
    case PROP_HISTORY_FILE:
      self->history_file = g_value_dup_object (value);
      break;

    case PROP_ENV:
      {
        const ShellTimeLimitsManagerEnv *env = g_value_get_pointer (value);

        if (env != NULL)
          {
            self->env = *env;
            self->have_env = TRUE;
          }
      }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_time_limits_manager_constructed (GObject *object)
{
  ShellTimeLimitsManager *self = SHELL_TIME_LIMITS_MANAGER (object);

  G_OBJECT_CLASS (shell_time_limits_manager_parent_class)->constructed (object);

  /* Allow the global state to be overridden for unit testing */
  if (!self->have_env)
    {
      default_env_init (&self->env);
      self->have_env = TRUE;
    }

  if (self->history_file == NULL)
    {
      g_autofree char *path = g_build_filename (shell_global_get_userdatadir (shell_global_get ()),
                                                "session-active-history.json", NULL);

      self->history_file = g_file_new_for_path (path);
    }

  self->session_limits_changed_id =
    self->env.session_limits_connect_changed (self->env.user_data,
                                              on_session_limits_changed, self);
  self->estimated_times_changed_id =
    self->env.timer_connect_estimated_times_changed (self->env.user_data,
                                                     on_estimated_times_changed, self);
  self->settings_changed_id =
    self->env.settings_connect_changed (self->env.user_data, on_settings_changed, self);

  /* Start tracking timings */
  update_state_machine (self);
}

static void
shell_time_limits_manager_dispose (GObject *object)
{
  ShellTimeLimitsManager *self = SHELL_TIME_LIMITS_MANAGER (object);

  if (self->have_env)
    {
      if (self->time_change_id != 0)
        self->env.source_remove (self->env.user_data, self->time_change_id);
      self->time_change_id = 0;

      if (self->timer_id != 0)
        self->env.source_remove (self->env.user_data, self->timer_id);
      self->timer_id = 0;

      if (self->login_user_id != 0)
        self->env.login_user_disconnect (self->env.user_data, self->login_user_id);
      self->login_user_id = 0;

      if (self->login_manager_id != 0)
        self->env.login_manager_disconnect (self->env.user_data, self->login_manager_id);
      self->login_manager_id = 0;

      if (self->settings_changed_id != 0)
        self->env.settings_disconnect (self->env.user_data, self->settings_changed_id);
      self->settings_changed_id = 0;

      if (self->session_limits_changed_id != 0)
        self->env.session_limits_disconnect (self->env.user_data, self->session_limits_changed_id);
      self->session_limits_changed_id = 0;

      if (self->estimated_times_changed_id != 0)
        self->env.timer_disconnect (self->env.user_data, self->estimated_times_changed_id);
      self->estimated_times_changed_id = 0;
    }

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->inhibitor);

  G_OBJECT_CLASS (shell_time_limits_manager_parent_class)->dispose (object);
}

static void
shell_time_limits_manager_finalize (GObject *object)
{
  ShellTimeLimitsManager *self = SHELL_TIME_LIMITS_MANAGER (object);

  g_clear_pointer (&self->transitions, g_array_unref);
  g_clear_object (&self->history_file);

  if (self->have_env && self->env.destroy != NULL)
    self->env.destroy (self->env.user_data);

  G_OBJECT_CLASS (shell_time_limits_manager_parent_class)->finalize (object);
}

static void
shell_time_limits_manager_class_init (ShellTimeLimitsManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_time_limits_manager_get_property;
  object_class->set_property = shell_time_limits_manager_set_property;
  object_class->constructed = shell_time_limits_manager_constructed;
  object_class->dispose = shell_time_limits_manager_dispose;
  object_class->finalize = shell_time_limits_manager_finalize;

  /**
   * ShellTimeLimitsManager:state:
   *
   * The current state of the state machine.
   */
  props[PROP_STATE] =
    g_param_spec_enum ("state", NULL, NULL,
                       SHELL_TYPE_TIME_LIMITS_STATE,
                       SHELL_TIME_LIMITS_STATE_DISABLED,
                       G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellTimeLimitsManager:daily-limit-time:
   *
   * The time when the daily limit will be reached
   * (shell_time_limits_manager_get_daily_limit_time ()).
   */
  props[PROP_DAILY_LIMIT_TIME] =
    g_param_spec_uint64 ("daily-limit-time", NULL, NULL,
                         0, G_MAXUINT64, 0,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellTimeLimitsManager:daily-limit-enabled:
   *
   * Whether the daily limit is enabled.
   */
  props[PROP_DAILY_LIMIT_ENABLED] =
    g_param_spec_boolean ("daily-limit-enabled", NULL, NULL,
                          TRUE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellTimeLimitsManager:grayscale-enabled:
   *
   * Whether the screen should be made grayscale once the daily limit is
   * reached.
   */
  props[PROP_GRAYSCALE_ENABLED] =
    g_param_spec_boolean ("grayscale-enabled", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellTimeLimitsManager:should-lock-session:
   *
   * Whether the session should be locked; %TRUE when the screen time limits
   * has been reached with parental controls enabled.
   */
  props[PROP_SHOULD_LOCK_SESSION] =
    g_param_spec_boolean ("should-lock-session", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellTimeLimitsManager:history-file:
   *
   * The file the history is saved in; by default
   * `session-active-history.json` in the user data directory of the shell.
   */
  props[PROP_HISTORY_FILE] =
    g_param_spec_object ("history-file", NULL, NULL,
                         G_TYPE_FILE,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /*
   * ShellTimeLimitsManager:env: (skip)
   *
   * A ShellTimeLimitsManagerEnv for the unit tests; see
   * time-limits-manager-private.h.
   */
  props[PROP_ENV] =
    g_param_spec_pointer ("env", NULL, NULL,
                          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellTimeLimitsManager::daily-limit-reached:
   * @self: the time limits manager
   *
   * The user reached their daily limit.
   */
  signals[DAILY_LIMIT_REACHED] =
    g_signal_new ("daily-limit-reached", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
shell_time_limits_manager_init (ShellTimeLimitsManager *self)
{
  self->transitions = g_array_new (FALSE, FALSE, sizeof (Transition));
  self->state = SHELL_TIME_LIMITS_STATE_DISABLED;
  self->user_state = SHELL_TIME_LIMITS_USER_STATE_INACTIVE;
}

/**
 * shell_time_limits_manager_new:
 *
 * Creates a time limits manager that tracks the real clock, logind, the
 * `org.gnome.desktop.screen-time-limits` settings and the parental controls.
 * src/ui/main.c creates the only instance.
 *
 * Returns: (transfer full): a new #ShellTimeLimitsManager
 */
ShellTimeLimitsManager *
shell_time_limits_manager_new (void)
{
  return g_object_new (SHELL_TYPE_TIME_LIMITS_MANAGER, NULL);
}

ShellTimeLimitsManager *
shell_time_limits_manager_new_with_env (const ShellTimeLimitsManagerEnv *env,
                                        GFile                           *history_file)
{
  return g_object_new (SHELL_TYPE_TIME_LIMITS_MANAGER,
                       "env", env,
                       "history-file", history_file,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* TimeLimitsNotification                                                    */
/* ------------------------------------------------------------------------ */

#define TIME_LIMITS_TYPE_NOTIFICATION (time_limits_notification_get_type ())
G_DECLARE_FINAL_TYPE (TimeLimitsNotification, time_limits_notification,
                      TIME_LIMITS, NOTIFICATION, ShellNotification)

struct _TimeLimitsNotification
{
  ShellNotification parent;

  ShellApp *app;
};

G_DEFINE_FINAL_TYPE (TimeLimitsNotification, time_limits_notification, SHELL_TYPE_NOTIFICATION)

static void
time_limits_notification_activate (ShellNotification *notification)
{
  TimeLimitsNotification *self = TIME_LIMITS_NOTIFICATION (notification);

  shell_app_activate (self->app);

  SHELL_NOTIFICATION_CLASS (time_limits_notification_parent_class)->activate (notification);
}

static void
time_limits_notification_dispose (GObject *object)
{
  TimeLimitsNotification *self = TIME_LIMITS_NOTIFICATION (object);

  g_clear_object (&self->app);

  G_OBJECT_CLASS (time_limits_notification_parent_class)->dispose (object);
}

static void
time_limits_notification_class_init (TimeLimitsNotificationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellNotificationClass *notification_class = SHELL_NOTIFICATION_CLASS (klass);

  object_class->dispose = time_limits_notification_dispose;
  notification_class->activate = time_limits_notification_activate;
}

static void
time_limits_notification_init (TimeLimitsNotification *self)
{
}

static TimeLimitsNotification *
time_limits_notification_new (ShellSource *source,
                              ShellApp    *app)
{
  TimeLimitsNotification *self;

  self = g_object_new (TIME_LIMITS_TYPE_NOTIFICATION,
                       "source", source,
                       "resident", TRUE,
                       NULL);
  self->app = g_object_ref (app);

  return self;
}

/* ------------------------------------------------------------------------ */
/* TimeLimitsNotificationSource                                              */
/* ------------------------------------------------------------------------ */

/* This cannot directly extend ShellSource (even though it conceptually does)
 * because the shell will automatically destroy a ShellSource when it becomes
 * empty, and that destroys the state which we need for correctly showing the
 * next notification (e.g. on a timer).
 *
 * So instead this is a normal GObject which closely wraps a ShellSource. */

#define TIME_LIMITS_TYPE_NOTIFICATION_SOURCE (time_limits_notification_source_get_type ())
G_DECLARE_FINAL_TYPE (TimeLimitsNotificationSource, time_limits_notification_source,
                      TIME_LIMITS, NOTIFICATION_SOURCE, GObject)

struct _TimeLimitsNotificationSource
{
  GObject parent;

  ShellApp *app;
  ShellSource *source;
  TimeLimitsNotification *notification;
  ShellMessageTray *message_tray;      /* weak */
  ShellTimeLimitsManager *manager;     /* weak */
  guint timer_id;

  ShellTimeLimitsState previous_state;
  gboolean destroyed;
};

G_DEFINE_FINAL_TYPE (TimeLimitsNotificationSource, time_limits_notification_source, G_TYPE_OBJECT)

static void time_limits_notification_source_update_state (TimeLimitsNotificationSource *self);

static void
on_tl_notification_destroy (ShellNotification                *notification,
                            ShellNotificationDestroyedReason  reason,
                            TimeLimitsNotificationSource     *self)
{
  g_signal_handlers_disconnect_by_data (notification, self);
  g_clear_object (&self->notification);
}

static void
on_tl_source_destroy (ShellSource                      *source,
                      ShellNotificationDestroyedReason  reason,
                      TimeLimitsNotificationSource     *self)
{
  g_signal_handlers_disconnect_by_data (source, self);
  g_clear_object (&self->source);
}

static gboolean
tl_source_ensure_notification (TimeLimitsNotificationSource *self,
                               const char                   *title,
                               const char                   *body,
                               ShellNotificationUrgency      urgency)
{
  ShellNotification *notification;

  if (self->app == NULL || self->message_tray == NULL)
    return FALSE;

  if (self->source == NULL)
    {
      g_autoptr (ShellNotificationPolicy) policy = shell_notification_policy_new_for_app (self->app);

      self->source = g_object_new (SHELL_TYPE_SOURCE,
                                   "title", shell_app_get_name (self->app),
                                   "icon", shell_app_get_icon (self->app),
                                   "policy", policy,
                                   NULL);
      g_signal_connect (self->source, "destroy", G_CALLBACK (on_tl_source_destroy), self);
      shell_message_tray_add (self->message_tray, self->source);
    }

  if (self->notification == NULL)
    {
      self->notification = time_limits_notification_new (self->source, self->app);
      g_signal_connect (self->notification, "destroy",
                        G_CALLBACK (on_tl_notification_destroy), self);
    }

  notification = SHELL_NOTIFICATION (self->notification);

  /* Unacknowledge the notification when it is updated, by default. */
  shell_notification_set_acknowledged (notification, FALSE);
  shell_notification_set_title (notification, title);
  shell_notification_set_body (notification, body);
  shell_notification_set_urgency (notification, urgency);

  shell_source_add_notification (self->source, notification);

  return TRUE;
}

static void
tl_source_detach (TimeLimitsNotificationSource *self)
{
  self->destroyed = TRUE;

  g_clear_handle_id (&self->timer_id, g_source_remove);

  if (self->manager != NULL)
    g_signal_handlers_disconnect_by_data (self->manager, self);
  g_clear_weak_pointer (&self->manager);

  if (self->notification != NULL)
    g_signal_handlers_disconnect_by_data (self->notification, self);
  g_clear_object (&self->notification);

  if (self->source != NULL)
    g_signal_handlers_disconnect_by_data (self->source, self);
  g_clear_object (&self->source);
}

static void
tl_source_destroy (TimeLimitsNotificationSource *self)
{
  g_autoptr (TimeLimitsNotification) notification = NULL;
  g_autoptr (ShellSource) source = NULL;

  if (self->destroyed)
    return;

  notification = self->notification != NULL ? g_object_ref (self->notification) : NULL;
  source = self->source != NULL ? g_object_ref (self->source) : NULL;

  tl_source_detach (self);

  if (notification != NULL)
    shell_notification_destroy (SHELL_NOTIFICATION (notification),
                                SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
  if (source != NULL)
    shell_source_destroy (source, SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
}

static void
tl_source_destroy_notification (TimeLimitsNotificationSource *self)
{
  g_autoptr (TimeLimitsNotification) notification = NULL;

  if (self->notification == NULL)
    return;

  notification = g_object_ref (self->notification);
  shell_notification_destroy (SHELL_NOTIFICATION (notification),
                              SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
}

static void
on_tl_source_timeout (gpointer user_data)
{
  TimeLimitsNotificationSource *self = user_data;

  self->timer_id = 0;
  g_debug ("TimeLimitsNotificationSource: Scheduled notification state update");
  time_limits_notification_source_update_state (self);
}

static void
tl_source_schedule_update_state (TimeLimitsNotificationSource *self,
                                 gint64                        timeout)
{
  g_clear_handle_id (&self->timer_id, g_source_remove);

  /* Never spin, and never ask GLib for something it cannot represent */
  timeout = CLAMP (timeout, 1, MAX_TIMEOUT_SECONDS);

  g_debug ("TimeLimitsNotificationSource: Scheduling notification state update in %" G_GINT64_FORMAT "s",
           timeout);

  self->timer_id = g_timeout_add_seconds_once ((guint) timeout, on_tl_source_timeout, self);
}

static void
time_limits_notification_source_update_state (TimeLimitsNotificationSource *self)
{
  ShellTimeLimitsManager *manager = self->manager;
  ShellTimeLimitsState state;
  gint64 current_time;

  if (self->destroyed || manager == NULL)
    return;

  current_time = shell_time_limits_manager_get_current_time (manager);
  state = shell_time_limits_manager_get_state (manager);

  g_debug ("TimeLimitsNotificationSource: Time limits notification source state changed from %s to %s",
           shell_time_limits_state_to_string (self->previous_state),
           shell_time_limits_state_to_string (state));

  switch (state)
    {
    case SHELL_TIME_LIMITS_STATE_DISABLED:
      tl_source_destroy (self);
      break;

    case SHELL_TIME_LIMITS_STATE_ACTIVE:
      {
        gint64 limit_due_time, remaining_secs, timeout_limit;
        gboolean parental_controls;

        /* Remove active notifications if limits have been disabled. */
        if (!shell_time_limits_manager_get_daily_limit_enabled (manager))
          {
            tl_source_destroy (self);
            break;
          }

        /* Work out when the time limit will be, and display some warnings
         * that it is impending. */
        limit_due_time = shell_time_limits_manager_get_daily_limit_time (manager);
        remaining_secs = limit_due_time - current_time;
        g_debug ("TimeLimitsNotificationSource: %" G_GINT64_FORMAT "s left before limit is reached",
                 remaining_secs);

        parental_controls =
          shell_time_limits_manager_get_parental_controls_session_limits_enabled (manager);
        if (parental_controls)
          timeout_limit = PARENTAL_CONTROLS_LIMIT_UPCOMING_NOTIFICATION_TIME_SECONDS;
        else
          timeout_limit = LIMIT_UPCOMING_NOTIFICATION_TIME_SECONDS;

        if (remaining_secs > timeout_limit)
          {
            tl_source_destroy_notification (self);

            /* Schedule to show a notification when the upcoming notification
             * time is reached. */
            tl_source_schedule_update_state (self, remaining_secs - timeout_limit);
            break;
          }
        else if (remaining_secs == timeout_limit)
          {
            g_autofree char *title_text = NULL;
            g_autofree char *body_text = NULL;
            gint64 remaining_time;

            if (parental_controls)
              {
                remaining_time = timeout_limit;
                title_text = g_strdup (_("Screen Time Limit is almost up"));
                body_text = g_strdup_printf (ngettext ("The computer will lock in %d second.",
                                                       "The computer will lock in %d seconds.",
                                                       remaining_time),
                                             (int) remaining_time);
              }
            else
              {
                /* Bang on time to show this notification. */
                remaining_time = timeout_limit / 60;
                title_text = g_strdup_printf (ngettext ("Screen Time Limit in %d Minute",
                                                        "Screen Time Limit in %d Minutes",
                                                        remaining_time),
                                              (int) remaining_time);
                body_text = g_strdup (_("Your screen time limit is approaching"));
              }

            tl_source_ensure_notification (self, title_text, body_text,
                                           SHELL_NOTIFICATION_URGENCY_HIGH);
          }
      }
      break;

    case SHELL_TIME_LIMITS_STATE_LIMIT_REACHED:
      /* Notify the user that they have reached their limit, when we
       * transition from any state to LIMIT_REACHED. Noop if parental controls
       * session limits are enabled, since we are locking then. */
      if (!shell_time_limits_manager_get_parental_controls_session_limits_enabled (manager) &&
          self->previous_state != SHELL_TIME_LIMITS_STATE_LIMIT_REACHED)
        tl_source_ensure_notification (self,
                                       _("Screen Time Limit Reached"),
                                       _("It’s time to stop using the device"),
                                       SHELL_NOTIFICATION_URGENCY_HIGH);
      break;

    default:
      g_debug ("Unknown TimeLimitsManager state: %d", state);
      break;
    }
}

static void
on_tl_source_state_changed (TimeLimitsNotificationSource *self)
{
  if (self->destroyed || self->manager == NULL)
    return;

  time_limits_notification_source_update_state (self);

  if (self->destroyed || self->manager == NULL)
    return;

  self->previous_state = shell_time_limits_manager_get_state (self->manager);
}

static void
time_limits_notification_source_dispose (GObject *object)
{
  TimeLimitsNotificationSource *self = TIME_LIMITS_NOTIFICATION_SOURCE (object);

  tl_source_detach (self);
  g_clear_weak_pointer (&self->message_tray);
  g_clear_object (&self->app);

  G_OBJECT_CLASS (time_limits_notification_source_parent_class)->dispose (object);
}

static void
time_limits_notification_source_class_init (TimeLimitsNotificationSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = time_limits_notification_source_dispose;
}

static void
time_limits_notification_source_init (TimeLimitsNotificationSource *self)
{
  self->previous_state = SHELL_TIME_LIMITS_STATE_DISABLED;
}

static TimeLimitsNotificationSource *
time_limits_notification_source_new (ShellTimeLimitsManager *manager,
                                     ShellMessageTray       *message_tray)
{
  TimeLimitsNotificationSource *self = g_object_new (TIME_LIMITS_TYPE_NOTIFICATION_SOURCE, NULL);
  ShellApp *app;

  app = shell_app_system_lookup_app (shell_app_system_get_default (), WELLBEING_DESKTOP_ID);
  if (app != NULL)
    self->app = g_object_ref (app);
  else
    g_message ("%s not found, the screen time notifications are not shown", WELLBEING_DESKTOP_ID);

  g_set_weak_pointer (&self->message_tray, message_tray);
  g_set_weak_pointer (&self->manager, manager);
  g_signal_connect_swapped (manager, "notify::state",
                            G_CALLBACK (on_tl_source_state_changed), self);
  g_signal_connect_swapped (manager, "notify::daily-limit-enabled",
                            G_CALLBACK (on_tl_source_state_changed), self);
  g_signal_connect_swapped (manager, "notify::daily-limit-time",
                            G_CALLBACK (on_tl_source_state_changed), self);

  time_limits_notification_source_update_state (self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellTimeLimitsDispatcher                                                 */
/* ------------------------------------------------------------------------ */

enum
{
  DISP_PROP_0,

  DISP_PROP_MANAGER,
  DISP_PROP_LAYOUT_MANAGER,
  DISP_PROP_MESSAGE_TRAY,

  DISP_N_PROPS
};

static GParamSpec *disp_props[DISP_N_PROPS] = { NULL, };

struct _ShellTimeLimitsDispatcher
{
  GObject parent;

  ShellTimeLimitsManager *manager;
  ShellLayoutManager *layout_manager;   /* weak */
  ShellMessageTray *message_tray;       /* weak */
  ShellSystemActions *system_actions;   /* unowned singleton */

  TimeLimitsNotificationSource *notification_source;
  ClutterEffect *desaturation_effect;
  ClutterActor *effect_actor;           /* weak, where desaturation_effect is */
  gulong effect_actor_destroy_id;

  gulong manager_handler_ids[3];
  gboolean destroyed;
};

G_DEFINE_FINAL_TYPE (ShellTimeLimitsDispatcher, shell_time_limits_dispatcher, G_TYPE_OBJECT)

static ClutterActor *
tl_dispatcher_get_ui_group (ShellTimeLimitsDispatcher *self)
{
  if (self->layout_manager == NULL)
    return NULL;

  return CLUTTER_ACTOR (shell_layout_manager_get_ui_group (self->layout_manager));
}

static void
tl_dispatcher_drop_effect (ShellTimeLimitsDispatcher *self,
                           gboolean                   remove)
{
  if (self->effect_actor != NULL)
    {
      g_clear_signal_handler (&self->effect_actor_destroy_id, self->effect_actor);

      if (remove && self->desaturation_effect != NULL &&
          clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->desaturation_effect)) == self->effect_actor)
        clutter_actor_remove_effect (self->effect_actor, self->desaturation_effect);

      g_clear_weak_pointer (&self->effect_actor);
    }

  self->effect_actor_destroy_id = 0;
  g_clear_object (&self->desaturation_effect);
}

static void
on_tl_ui_group_destroy (ClutterActor              *actor,
                        ShellTimeLimitsDispatcher *self)
{
  tl_dispatcher_drop_effect (self, FALSE);
}

static void
tl_dispatcher_ensure_enabled (ShellTimeLimitsDispatcher *self)
{
  ClutterActor *ui_group;

  if (self->notification_source != NULL && self->notification_source->destroyed)
    g_clear_object (&self->notification_source);

  if (self->notification_source == NULL && self->message_tray != NULL)
    self->notification_source = time_limits_notification_source_new (self->manager,
                                                                     self->message_tray);

  ui_group = tl_dispatcher_get_ui_group (self);
  if (self->desaturation_effect == NULL && ui_group != NULL)
    {
      self->desaturation_effect = g_object_ref_sink (clutter_desaturate_effect_new (0.0));
      clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->desaturation_effect), FALSE);
      clutter_actor_add_effect_with_name (ui_group, "desaturate", self->desaturation_effect);

      g_set_weak_pointer (&self->effect_actor, ui_group);
      self->effect_actor_destroy_id =
        g_signal_connect (ui_group, "destroy", G_CALLBACK (on_tl_ui_group_destroy), self);
    }
}

static void
tl_dispatcher_ensure_disabled (ShellTimeLimitsDispatcher *self)
{
  if (self->notification_source != NULL)
    {
      tl_source_destroy (self->notification_source);
      g_clear_object (&self->notification_source);
    }

  tl_dispatcher_drop_effect (self, TRUE);
}

static void
tl_dispatcher_set_effect_enabled (ShellTimeLimitsDispatcher *self,
                                  gboolean                   enabled)
{
  if (self->desaturation_effect != NULL)
    clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->desaturation_effect), enabled);
}

static void
tl_dispatcher_grayscale_on (ShellTimeLimitsDispatcher *self)
{
  g_auto (GValue) target = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;
  ShellEaseParams params;
  ClutterActor *ui_group = tl_dispatcher_get_ui_group (self);

  if (self->desaturation_effect == NULL || ui_group == NULL)
    return;

  clutter_desaturate_effect_set_factor (CLUTTER_DESATURATE_EFFECT (self->desaturation_effect), 0.0);
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->desaturation_effect), TRUE);

  g_value_init (&target, G_TYPE_DOUBLE);
  g_value_set_double (&target, GRAYSCALE_SATURATION);

  params.duration = GRAYSCALE_FADE_TIME_SECONDS * 1000;
  params.delay = -1;
  params.mode = CLUTTER_EASE_OUT_QUAD;
  params.repeat_count = 0;
  params.auto_reverse = FALSE;
  params.animation_required = FALSE;

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (ui_group),
                                      "@effects.desaturate.factor",
                                      &target, &params, NULL, NULL, NULL, &error))
    g_debug ("%s: %s", G_STRFUNC, error->message);
}

static void
on_tl_dispatcher_state_changed (ShellTimeLimitsDispatcher *self)
{
  ShellTimeLimitsState state;

  if (self->destroyed)
    return;

  state = shell_time_limits_manager_get_state (self->manager);

  switch (state)
    {
    case SHELL_TIME_LIMITS_STATE_DISABLED:
      tl_dispatcher_ensure_disabled (self);
      break;

    case SHELL_TIME_LIMITS_STATE_ACTIVE:
      if (shell_time_limits_manager_get_daily_limit_enabled (self->manager))
        {
          tl_dispatcher_ensure_enabled (self);
          tl_dispatcher_set_effect_enabled (self, FALSE);
        }
      else
        {
          tl_dispatcher_ensure_disabled (self);
        }
      break;

    case SHELL_TIME_LIMITS_STATE_LIMIT_REACHED:
      if (shell_time_limits_manager_get_should_lock_session (self->manager))
        {
          /* Trying to lock the screen will fail if the action is not
           * available. This happens when admin disables lock screen, or when
           * not running under GDM/logind. Those are corner cases which do not
           * need to be handled. */
          if (shell_system_actions_get_can_lock_screen (self->system_actions))
            {
              g_autoptr (GError) error = NULL;

              if (!shell_system_actions_activate_lock_screen (self->system_actions, &error))
                g_message ("Failed to lock the screen: %s", error->message);
            }
          break;
        }

      tl_dispatcher_ensure_enabled (self);

      if (shell_time_limits_manager_get_grayscale_enabled (self->manager))
        tl_dispatcher_grayscale_on (self);
      else
        tl_dispatcher_set_effect_enabled (self, FALSE);
      break;

    default:
      g_debug ("Unknown TimeLimitsManager state: %d", state);
      break;
    }
}

/* Stops reacting to the manager and drops everything without touching the
 * actors and the message tray, which may already be destroyed */
static void
tl_dispatcher_detach (ShellTimeLimitsDispatcher *self)
{
  guint i;

  self->destroyed = TRUE;

  for (i = 0; i < G_N_ELEMENTS (self->manager_handler_ids); i++)
    g_clear_signal_handler (&self->manager_handler_ids[i], self->manager);

  if (self->notification_source != NULL)
    tl_source_detach (self->notification_source);
  g_clear_object (&self->notification_source);

  tl_dispatcher_drop_effect (self, FALSE);
}

static void
on_tl_dispatcher_shutdown (ShellGlobal               *global,
                           ShellTimeLimitsDispatcher *self)
{
  if (!self->destroyed)
    tl_dispatcher_detach (self);
}

/**
 * shell_time_limits_dispatcher_destroy:
 * @self: a #ShellTimeLimitsDispatcher
 *
 * Removes the notifications and the grayscale effect and stops following the
 * time limits manager.
 */
void
shell_time_limits_dispatcher_destroy (ShellTimeLimitsDispatcher *self)
{
  g_return_if_fail (SHELL_IS_TIME_LIMITS_DISPATCHER (self));

  if (self->destroyed)
    return;

  tl_dispatcher_ensure_disabled (self);
  tl_dispatcher_detach (self);
}

static void
shell_time_limits_dispatcher_set_property (GObject      *object,
                                           guint         prop_id,
                                           const GValue *value,
                                           GParamSpec   *pspec)
{
  ShellTimeLimitsDispatcher *self = SHELL_TIME_LIMITS_DISPATCHER (object);

  switch (prop_id)
    {
    case DISP_PROP_MANAGER:
      self->manager = g_value_dup_object (value);
      break;

    case DISP_PROP_LAYOUT_MANAGER:
      g_set_weak_pointer (&self->layout_manager, g_value_get_object (value));
      break;

    case DISP_PROP_MESSAGE_TRAY:
      g_set_weak_pointer (&self->message_tray, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_time_limits_dispatcher_get_property (GObject    *object,
                                           guint       prop_id,
                                           GValue     *value,
                                           GParamSpec *pspec)
{
  ShellTimeLimitsDispatcher *self = SHELL_TIME_LIMITS_DISPATCHER (object);

  switch (prop_id)
    {
    case DISP_PROP_MANAGER:
      g_value_set_object (value, self->manager);
      break;

    case DISP_PROP_LAYOUT_MANAGER:
      g_value_set_object (value, self->layout_manager);
      break;

    case DISP_PROP_MESSAGE_TRAY:
      g_value_set_object (value, self->message_tray);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_time_limits_dispatcher_constructed (GObject *object)
{
  ShellTimeLimitsDispatcher *self = SHELL_TIME_LIMITS_DISPATCHER (object);

  G_OBJECT_CLASS (shell_time_limits_dispatcher_parent_class)->constructed (object);

  self->system_actions = shell_system_actions_get_default ();

  self->manager_handler_ids[0] =
    g_signal_connect_swapped (self->manager, "notify::state",
                              G_CALLBACK (on_tl_dispatcher_state_changed), self);
  self->manager_handler_ids[1] =
    g_signal_connect_swapped (self->manager, "notify::daily-limit-enabled",
                              G_CALLBACK (on_tl_dispatcher_state_changed), self);
  self->manager_handler_ids[2] =
    g_signal_connect_swapped (self->manager, "notify::grayscale-enabled",
                              G_CALLBACK (on_tl_dispatcher_state_changed), self);

  /* The actors and the message tray are destroyed at shutdown */
  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_tl_dispatcher_shutdown), self, 0);

  if (shell_time_limits_manager_get_state (self->manager) == SHELL_TIME_LIMITS_STATE_DISABLED ||
      !shell_time_limits_manager_get_daily_limit_enabled (self->manager))
    tl_dispatcher_ensure_disabled (self);
  else
    tl_dispatcher_ensure_enabled (self);
}

static void
shell_time_limits_dispatcher_dispose (GObject *object)
{
  ShellTimeLimitsDispatcher *self = SHELL_TIME_LIMITS_DISPATCHER (object);

  if (!self->destroyed && self->manager != NULL)
    tl_dispatcher_detach (self);

  g_clear_weak_pointer (&self->layout_manager);
  g_clear_weak_pointer (&self->message_tray);
  g_clear_object (&self->manager);

  G_OBJECT_CLASS (shell_time_limits_dispatcher_parent_class)->dispose (object);
}

static void
shell_time_limits_dispatcher_class_init (ShellTimeLimitsDispatcherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = shell_time_limits_dispatcher_set_property;
  object_class->get_property = shell_time_limits_dispatcher_get_property;
  object_class->constructed = shell_time_limits_dispatcher_constructed;
  object_class->dispose = shell_time_limits_dispatcher_dispose;

  /**
   * ShellTimeLimitsDispatcher:manager:
   *
   * The time limits manager to follow.
   */
  disp_props[DISP_PROP_MANAGER] =
    g_param_spec_object ("manager", NULL, NULL,
                         SHELL_TYPE_TIME_LIMITS_MANAGER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellTimeLimitsDispatcher:layout-manager:
   *
   * The layout manager, for the `ui_group` the grayscale effect applies to.
   */
  disp_props[DISP_PROP_LAYOUT_MANAGER] =
    g_param_spec_object ("layout-manager", NULL, NULL,
                         SHELL_TYPE_LAYOUT_MANAGER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellTimeLimitsDispatcher:message-tray:
   *
   * The message tray the notifications are added to.
   */
  disp_props[DISP_PROP_MESSAGE_TRAY] =
    g_param_spec_object ("message-tray", NULL, NULL,
                         SHELL_TYPE_MESSAGE_TRAY,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, DISP_N_PROPS, disp_props);
}

static void
shell_time_limits_dispatcher_init (ShellTimeLimitsDispatcher *self)
{
}

/**
 * shell_time_limits_dispatcher_new:
 * @manager: the #ShellTimeLimitsManager to follow
 * @layout_manager: the #ShellLayoutManager
 * @message_tray: the #ShellMessageTray
 *
 * Creates the glue which takes the state-based output from @manager and
 * converts it to event-based notifications for the user to tell them when
 * their time limit has been reached. It factors the user's UI preferences
 * into account, and whether parental controls session limits are enabled.
 *
 * Returns: (transfer full): a new #ShellTimeLimitsDispatcher
 */
ShellTimeLimitsDispatcher *
shell_time_limits_dispatcher_new (ShellTimeLimitsManager *manager,
                                  ShellLayoutManager     *layout_manager,
                                  ShellMessageTray       *message_tray)
{
  g_return_val_if_fail (SHELL_IS_TIME_LIMITS_MANAGER (manager), NULL);

  return g_object_new (SHELL_TYPE_TIME_LIMITS_DISPATCHER,
                       "manager", manager,
                       "layout-manager", layout_manager,
                       "message-tray", message_tray,
                       NULL);
}
