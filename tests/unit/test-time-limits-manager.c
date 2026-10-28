/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Copyright 2024, 2025 GNOME Foundation, Inc.
 *
 * Unit tests for ShellTimeLimitsManager (src/misc/time-limits-manager.c), a
 * GNOME Shell component to support screen time limits and statistics. Port of
 * tests/unit/timeLimitsManager.js. The tests hard-code UTC to keep the code
 * brief, so must be run in UTC (meson sets TZ).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <time.h>

#include "misc/time-limits-manager.h"
#include "misc/time-limits-manager-private.h"

#define SCREEN_TIME_LIMITS_SCHEMA "org.gnome.desktop.screen-time-limits"

/*
 * A harness for testing the ShellTimeLimitsManager class. It simulates the
 * passage of time, maintaining an internal ordered queue of events, and
 * providing groups of mock functions (a ShellTimeLimitsManagerEnv) which the
 * ShellTimeLimitsManager uses to interact with it: mock versions of the clock
 * and timeout functions, a mock of the logind manager and user, a mock of the
 * parental controls manager, a mock of the malcontent-timerd `Child` D-Bus
 * object, and a mock version of GSettings.
 *
 * The internal ordered queue of events is sorted by time (in real/wall clock
 * seconds, i.e. UNIX timestamps). On each harness_tick (), the next event is
 * shifted off the head of the queue and processed. An event might be a
 * simulated user state change (mocking the user starting or stopping a
 * session), a scheduled timeout, or an assertion for actually running test
 * assertions.
 *
 * The simulated clock jumps from the scheduled time of one event to the
 * scheduled time of the next. This way, we can simulate half an hour of active
 * time (simulating usage) on the device instantly.
 *
 * Times are provided as ISO 8601 date/time strings, to allow tests which span
 * multiple days to be written more easily. This simplifies things because the
 * daily time limit is reset at a specific time each morning.
 *
 * The manager does its file and mock D-Bus I/O asynchronously. After every
 * event the harness iterates the main context until the manager has no
 * operation in flight (shell_time_limits_manager_is_busy ()), so a test is
 * deterministic: the next event starts after everything the previous one
 * triggered has finished.
 */

typedef struct _Harness Harness;

typedef void (* HarnessFunc) (Harness  *harness,
                              gpointer  data);

typedef enum
{
  EVENT_TIMEOUT,
  EVENT_TIME_CHANGE,
  EVENT_PREPARING_FOR_SLEEP,
  EVENT_LOGIN_USER_STATE_CHANGE,
  EVENT_SESSION_LIMITS_CHANGED,
  EVENT_ESTIMATED_TIMES_CHANGED,
  EVENT_SETTINGS_CHANGE,
  EVENT_EXPECT_STATE,
  EVENT_EXPECT_PROPERTIES,
  EVENT_EXPECT_USAGE,
  EVENT_CALLBACK,
  EVENT_SHUTDOWN,
  EVENT_EXPECT_SHUTDOWN,
} EventType;

typedef struct
{
  EventType type;
  gint64 time;

  guint source_id;                    /* EVENT_TIMEOUT */
  guint interval_secs;
  GSourceFunc timeout_func;
  gpointer timeout_data;

  gint64 new_time;                    /* EVENT_TIME_CHANGE */
  HarnessFunc after_func;
  gpointer after_data;

  gboolean new_preparing_for_sleep;   /* EVENT_PREPARING_FOR_SLEEP */

  const char *new_user_state;         /* EVENT_LOGIN_USER_STATE_CHANGE */
  gboolean new_user_idle_hint;

  gboolean new_session_limits_enabled; /* EVENT_SESSION_LIMITS_CHANGED */

  gboolean new_limit_reached;         /* EVENT_ESTIMATED_TIMES_CHANGED */
  guint64 new_times[4];

  const char *key;                    /* EVENT_SETTINGS_CHANGE */
  GVariant *new_value;

  ShellTimeLimitsState expected_state; /* EVENT_EXPECT_* */
  gboolean check_daily_limit_time;
  guint64 expected_daily_limit_time;
  gint64 expected_usage_start;
  gint64 expected_usage_end;
  char *context;

  HarnessFunc func;                   /* EVENT_CALLBACK */
  gpointer func_data;
} Event;

struct _Harness
{
  gint64 current_time_secs;
  gint64 clock_offset;               /* make the monotonic clock lag by 100s, arbitrarily */
  guint next_source_id;
  GArray *events;                    /* Event, sorted by time */

  GSourceFunc time_change_notify_func;
  gpointer time_change_notify_data;
  guint time_change_notify_source_id;

  GHashTable *settings;              /* key -> GVariant, of SCREEN_TIME_LIMITS_SCHEMA */
  ShellTimeLimitsSettingsFunc settings_changed_func;
  gpointer settings_changed_data;

  /* These two emulate relevant bits of the o.fdo.login1.User API
   * See https://www.freedesktop.org/software/systemd/man/latest/org.freedesktop.login1.html#User%20Objects */
  const char *current_user_state;
  gboolean current_user_idle_hint;
  ShellTimeLimitsWatchFunc login_user_properties_changed_func;
  gpointer login_user_properties_changed_data;

  gboolean current_preparing_for_sleep_state;
  ShellTimeLimitsSleepFunc login_manager_prepare_for_sleep_func;
  gpointer login_manager_prepare_for_sleep_data;

  ShellTimeLimitsWatchFunc session_limits_changed_func;
  gpointer session_limits_changed_data;
  ShellTimeLimitsWatchFunc estimated_times_changed_func;
  gpointer estimated_times_changed_data;

  GFile *history_file;

  gboolean session_limits_enabled;
  gboolean limit_reached;
  guint64 current_session_start_time_secs;
  guint64 current_session_estimated_end_time_secs;
  guint64 next_session_start_time_secs;
  guint64 next_session_estimated_end_time_secs;
  gboolean have_time_store;
  gint64 time_store_start;
  gint64 time_store_end;

  ShellTimeLimitsManager *manager;
};

/* Convert an ISO 8601 string to a UNIX timestamp for use in tests.
 *
 * Internally, the tests are all based on UNIX timestamps using wall clock
 * time. Those are not very easy to reason about when reading or writing tests
 * though, so we allow the tests to be written using ISO 8601 strings. */
static gint64
ts (const char *time_str)
{
  g_autoptr (GDateTime) dt = g_date_time_new_from_iso8601 (time_str, NULL);

  if (dt == NULL)
    g_error ("Time string ‘%s’ could not be parsed", time_str);

  return g_date_time_to_unix (dt);
}

/* Inverse of ts () */
static char *
time_secs_to_str (gint64 time_secs)
{
  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_utc (time_secs);

  g_assert_nonnull (dt);

  return g_date_time_format_iso8601 (dt);
}

/* Mock completions. The result of the operations is always returned from an
 * idle, as the real ones are. */

static void
complete_task_cb (gpointer data)
{
  GTask *task = data;

  g_task_return_pointer (task, g_task_get_task_data (task), NULL);
  g_object_unref (task);
}

static void
complete_task_later (GAsyncReadyCallback callback,
                     gpointer            callback_data,
                     gpointer            result,
                     GDestroyNotify      result_destroy)
{
  GTask *task = g_task_new (NULL, NULL, callback, callback_data);

  /* The result is owned by the finish function */
  g_task_set_task_data (task, result, NULL);
  g_idle_add_once (complete_task_cb, task);
}

/* Mock clock */

static gint64
mock_get_real_time_secs (gpointer user_data)
{
  Harness *harness = user_data;

  return harness->current_time_secs;
}

static gint64
mock_get_monotonic_time_secs (gpointer user_data)
{
  Harness *harness = user_data;

  return harness->current_time_secs - harness->clock_offset;
}

static void
event_clear (Event *event)
{
  g_clear_pointer (&event->new_value, g_variant_unref);
  g_clear_pointer (&event->context, g_free);
}

static void
harness_insert_event (Harness *harness,
                      Event    event)
{
  guint i;

  if (event.time < harness->current_time_secs)
    {
      g_autofree char *event_str = time_secs_to_str (event.time);
      g_autofree char *now_str = time_secs_to_str (harness->current_time_secs);

      g_error ("Event cannot be before current mock clock time (%s vs %s)", event_str, now_str);
    }

  /* Keep the events in insertion order if they have the same time */
  for (i = 0; i < harness->events->len; i++)
    {
      if (g_array_index (harness->events, Event, i).time > event.time)
        break;
    }

  g_array_insert_val (harness->events, i, event);
}

static guint
mock_timeout_add_seconds (gpointer    user_data,
                          guint       interval,
                          GSourceFunc func,
                          gpointer    func_data)
{
  Harness *harness = user_data;
  Event event = { 0, };

  event.type = EVENT_TIMEOUT;
  event.time = harness->current_time_secs + interval;
  event.source_id = harness->next_source_id++;
  event.interval_secs = interval;
  event.timeout_func = func;
  event.timeout_data = func_data;
  harness_insert_event (harness, event);

  return event.source_id;
}

static void
mock_source_remove (gpointer user_data,
                    guint    source_id)
{
  Harness *harness = user_data;
  guint i;

  if (harness->time_change_notify_func != NULL &&
      source_id == harness->time_change_notify_source_id)
    {
      harness->time_change_notify_func = NULL;
      harness->time_change_notify_data = NULL;
      harness->time_change_notify_source_id = 0;
      return;
    }

  for (i = 0; i < harness->events->len; i++)
    {
      Event *event = &g_array_index (harness->events, Event, i);

      if (event->type == EVENT_TIMEOUT && event->source_id == source_id)
        {
          g_array_remove_index (harness->events, i);
          return;
        }
    }

  g_error ("Removing non-existent source with ID %u", source_id);
}

static guint
mock_time_change_notify (gpointer    user_data,
                         GSourceFunc func,
                         gpointer    func_data)
{
  Harness *harness = user_data;

  if (harness->time_change_notify_func != NULL)
    g_error ("Duplicate time_change_notify() call");

  harness->time_change_notify_func = func;
  harness->time_change_notify_data = func_data;
  harness->time_change_notify_source_id = harness->next_source_id++;

  return harness->time_change_notify_source_id;
}

/* Mock logind manager: roughly a wrapper around the
 * org.freedesktop.login1.Manager D-Bus API. Each function uses the current
 * state from the harness. */

static gulong
mock_login_manager_connect (gpointer                  user_data,
                            ShellTimeLimitsSleepFunc  func,
                            gpointer                  func_data)
{
  Harness *harness = user_data;

  if (harness->login_manager_prepare_for_sleep_func != NULL)
    g_error ("Duplicate prepare-for-sleep connection");

  harness->login_manager_prepare_for_sleep_func = func;
  harness->login_manager_prepare_for_sleep_data = func_data;

  return 1;
}

static void
mock_login_manager_disconnect (gpointer user_data,
                               gulong   handler_id)
{
  Harness *harness = user_data;

  /* Very simple implementation for mock purposes */
  harness->login_manager_prepare_for_sleep_func = NULL;
  harness->login_manager_prepare_for_sleep_data = NULL;
}

static gboolean
mock_login_manager_get_preparing_for_sleep (gpointer user_data)
{
  Harness *harness = user_data;

  return harness->current_preparing_for_sleep_state;
}

static void
mock_login_manager_inhibit_async (gpointer             user_data,
                                  const char          *reason,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             callback_data)
{
  /* Basically a no-op for mock purposes: the inhibitor is a stream which can
   * be closed */
  complete_task_later (callback, callback_data, g_memory_input_stream_new (), NULL);
}

static GInputStream *
mock_login_manager_inhibit_finish (gpointer       user_data,
                                   GAsyncResult  *result,
                                   GError       **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

/* Mock logind user: roughly a wrapper around the org.freedesktop.login1.User
 * D-Bus API */

static void
mock_login_user_get_async (gpointer             user_data,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             callback_data)
{
  complete_task_later (callback, callback_data, GINT_TO_POINTER (TRUE), NULL);
}

static gboolean
mock_login_user_get_finish (gpointer       user_data,
                            GAsyncResult  *result,
                            GError       **error)
{
  return g_task_propagate_pointer (G_TASK (result), error) != NULL;
}

static gulong
mock_login_user_connect (gpointer                  user_data,
                         ShellTimeLimitsWatchFunc  func,
                         gpointer                  func_data)
{
  Harness *harness = user_data;

  if (harness->login_user_properties_changed_func != NULL)
    g_error ("Duplicate g-properties-changed connection");

  harness->login_user_properties_changed_func = func;
  harness->login_user_properties_changed_data = func_data;

  return 1;
}

static void
mock_login_user_disconnect (gpointer user_data,
                            gulong   handler_id)
{
  Harness *harness = user_data;

  /* Very simple implementation for mock purposes */
  harness->login_user_properties_changed_func = NULL;
  harness->login_user_properties_changed_data = NULL;
}

static const char *
mock_login_user_get_state (gpointer user_data)
{
  Harness *harness = user_data;

  return harness->current_user_state;
}

static gboolean
mock_login_user_get_idle_hint (gpointer user_data)
{
  Harness *harness = user_data;

  return harness->current_user_idle_hint;
}

/* Mock settings: roughly a wrapper around GSettings which uses the settings
 * dictionary passed to the harness. */

static gboolean
mock_settings_get_boolean (gpointer    user_data,
                           const char *key)
{
  Harness *harness = user_data;
  GVariant *value = g_hash_table_lookup (harness->settings, key);

  g_assert_nonnull (value);

  return g_variant_get_boolean (value);
}

static guint
mock_settings_get_uint (gpointer    user_data,
                        const char *key)
{
  Harness *harness = user_data;
  GVariant *value = g_hash_table_lookup (harness->settings, key);

  g_assert_nonnull (value);

  return g_variant_get_uint32 (value);
}

static gulong
mock_settings_connect_changed (gpointer                     user_data,
                               ShellTimeLimitsSettingsFunc  func,
                               gpointer                     func_data)
{
  Harness *harness = user_data;

  if (harness->settings_changed_func != NULL)
    g_error ("Settings signals already connected");

  harness->settings_changed_func = func;
  harness->settings_changed_data = func_data;

  return 1;
}

static void
mock_settings_disconnect (gpointer user_data,
                          gulong   handler_id)
{
  Harness *harness = user_data;

  harness->settings_changed_func = NULL;
  harness->settings_changed_data = NULL;
}

/* Mock parental controls manager: a wrapper around the malcontent manager
 * which uses the session limits flag of the harness. */

static gboolean
mock_session_limits_enabled (gpointer user_data)
{
  Harness *harness = user_data;

  return harness->session_limits_enabled;
}

static gulong
mock_session_limits_connect_changed (gpointer                  user_data,
                                     ShellTimeLimitsWatchFunc  func,
                                     gpointer                  func_data)
{
  Harness *harness = user_data;

  if (harness->session_limits_changed_func != NULL)
    g_error ("ParentalControlsManager signals already connected");

  harness->session_limits_changed_func = func;
  harness->session_limits_changed_data = func_data;

  return 1;
}

static void
mock_session_limits_disconnect (gpointer user_data,
                                gulong   handler_id)
{
  Harness *harness = user_data;

  harness->session_limits_changed_func = NULL;
  harness->session_limits_changed_data = NULL;
}

/* Mock timer child proxy: roughly a wrapper around the
 * org.freedesktop.MalcontentTimer1.Child D-Bus API */

static gulong
mock_timer_connect_estimated_times_changed (gpointer                  user_data,
                                            ShellTimeLimitsWatchFunc  func,
                                            gpointer                  func_data)
{
  Harness *harness = user_data;

  if (harness->estimated_times_changed_func != NULL)
    g_error ("TimerChildProxy signals already connected");

  harness->estimated_times_changed_func = func;
  harness->estimated_times_changed_data = func_data;

  return 1;
}

static void
mock_timer_disconnect (gpointer user_data,
                       gulong   handler_id)
{
  Harness *harness = user_data;

  harness->estimated_times_changed_func = NULL;
  harness->estimated_times_changed_data = NULL;
}

static void
mock_timer_record_usage_async (gpointer             user_data,
                               GVariant            *entries,
                               GAsyncReadyCallback  callback,
                               gpointer             callback_data)
{
  Harness *harness = user_data;
  g_autoptr (GVariant) floating = g_variant_ref_sink (entries);
  g_autoptr (GVariant) first = NULL;
  guint64 start_secs, end_secs;

  g_assert_cmpuint (g_variant_n_children (floating), >=, 1);

  first = g_variant_get_child_value (floating, 0);
  g_variant_get (first, "(tt&s&s)", &start_secs, &end_secs, NULL, NULL);
  harness->have_time_store = TRUE;
  harness->time_store_start = start_secs;
  harness->time_store_end = end_secs;

  complete_task_later (callback, callback_data, GINT_TO_POINTER (TRUE), NULL);
}

static gboolean
mock_timer_record_usage_finish (gpointer       user_data,
                                GAsyncResult  *result,
                                GError       **error)
{
  return g_task_propagate_pointer (G_TASK (result), error) != NULL;
}

static void
mock_timer_get_estimated_times_async (gpointer             user_data,
                                      GAsyncReadyCallback  callback,
                                      gpointer             callback_data)
{
  Harness *harness = user_data;
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{s(btttt)}"));
  g_variant_builder_add (&builder, "{s(btttt)}", "",
                         harness->limit_reached,
                         harness->current_session_start_time_secs,
                         harness->current_session_estimated_end_time_secs,
                         harness->next_session_start_time_secs,
                         harness->next_session_estimated_end_time_secs);

  complete_task_later (callback, callback_data,
                       g_variant_ref_sink (g_variant_builder_end (&builder)), NULL);
}

static gboolean
mock_timer_get_estimated_times_finish (gpointer       user_data,
                                       GAsyncResult  *result,
                                       GVariant     **times,
                                       GError       **error)
{
  GVariant *value = g_task_propagate_pointer (G_TASK (result), error);

  if (value == NULL)
    return FALSE;

  *times = value;
  return TRUE;
}

/* Harness */

static void
harness_settings_set_boolean (Harness    *harness,
                              const char *key,
                              gboolean    value)
{
  g_hash_table_insert (harness->settings, g_strdup (key),
                       g_variant_ref_sink (g_variant_new_boolean (value)));
}

static void
harness_settings_set_uint (Harness    *harness,
                           const char *key,
                           guint       value)
{
  g_hash_table_insert (harness->settings, g_strdup (key),
                       g_variant_ref_sink (g_variant_new_uint32 (value)));
}

/*
 * harness_new:
 * @history_enabled: the value of the `history-enabled` setting
 * @daily_limit_enabled: the value of the `daily-limit-enabled` setting
 * @daily_limit_seconds: the value of the `daily-limit-seconds` setting
 * @history_file_contents: (nullable): the contents of the history file. Create
 *   a fake history file containing the given contents. Or, if no contents are
 *   given, reserve a distinct new history file name but then delete it so it
 *   does not exist for the manager.
 * @session_limits_enabled: whether the parental controls session limits are
 *   enabled
 */
static Harness *
harness_new (gboolean    history_enabled,
             gboolean    daily_limit_enabled,
             guint       daily_limit_seconds,
             const char *history_file_contents,
             gboolean    session_limits_enabled)
{
  Harness *harness = g_new0 (Harness, 1);
  g_autoptr (GError) error = NULL;
  g_autoptr (GFileIOStream) stream = NULL;

  harness->current_time_secs = 0;
  harness->clock_offset = 100;  /* make the monotonic clock lag by 100s, arbitrarily */
  harness->next_source_id = 1;
  harness->events = g_array_new (FALSE, TRUE, sizeof (Event));
  harness->settings = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify) g_variant_unref);
  harness_settings_set_boolean (harness, "history-enabled", history_enabled);
  harness_settings_set_boolean (harness, "daily-limit-enabled", daily_limit_enabled);
  harness_settings_set_uint (harness, "daily-limit-seconds", daily_limit_seconds);
  harness_settings_set_boolean (harness, "grayscale", FALSE);

  harness->current_user_state = "active";
  harness->current_user_idle_hint = FALSE;

  /* Create a fake history file containing the given contents. */
  harness->history_file = g_file_new_tmp ("gnome-shell-time-limits-manager-test-XXXXXX.json",
                                          &stream, &error);
  g_assert_no_error (error);
  if (history_file_contents != NULL && history_file_contents[0] != '\0')
    {
      g_assert_true (g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (stream)),
                                                history_file_contents,
                                                strlen (history_file_contents),
                                                NULL, NULL, &error));
      g_assert_no_error (error);
    }
  g_assert_true (g_io_stream_close (G_IO_STREAM (stream), NULL, &error));
  g_assert_no_error (error);
  if (history_file_contents == NULL || history_file_contents[0] == '\0')
    {
      g_assert_true (g_file_delete (harness->history_file, NULL, &error));
      g_assert_no_error (error);
    }

  harness->session_limits_enabled = session_limits_enabled;
  harness->limit_reached = FALSE;
  harness->current_session_start_time_secs = 0;
  harness->current_session_estimated_end_time_secs = G_MAXUINT64;
  harness->next_session_start_time_secs = G_MAXUINT64;
  harness->next_session_estimated_end_time_secs = G_MAXUINT64;

  return harness;
}

static void
harness_free (Harness *harness)
{
  g_autoptr (GError) error = NULL;
  guint i;

  /* Delete the temporary history file */
  if (!g_file_delete (harness->history_file, NULL, &error))
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);

  g_clear_object (&harness->manager);
  for (i = 0; i < harness->events->len; i++)
    event_clear (&g_array_index (harness->events, Event, i));
  g_array_unref (harness->events);
  g_hash_table_unref (harness->settings);
  g_object_unref (harness->history_file);
  g_free (harness);
}

/* Set the initial time for the mock real/wall clock.
 *
 * This will typically become the time that the mock user first becomes active,
 * when the ShellTimeLimitsManager is created. */
static void
harness_initialize_mock_clock (Harness    *harness,
                               const char *time_str)
{
  g_assert_cmpint (harness->current_time_secs, ==, 0);

  harness->current_time_secs = ts (time_str);
}

static void
harness_create_manager (Harness *harness)
{
  ShellTimeLimitsManagerEnv env = {
    .user_data = harness,
    .get_real_time_secs = mock_get_real_time_secs,
    .get_monotonic_time_secs = mock_get_monotonic_time_secs,
    .timeout_add_seconds = mock_timeout_add_seconds,
    .source_remove = mock_source_remove,
    .time_change_notify = mock_time_change_notify,
    .login_manager_connect = mock_login_manager_connect,
    .login_manager_disconnect = mock_login_manager_disconnect,
    .login_manager_get_preparing_for_sleep = mock_login_manager_get_preparing_for_sleep,
    .login_manager_inhibit_async = mock_login_manager_inhibit_async,
    .login_manager_inhibit_finish = mock_login_manager_inhibit_finish,
    .login_user_get_async = mock_login_user_get_async,
    .login_user_get_finish = mock_login_user_get_finish,
    .login_user_connect = mock_login_user_connect,
    .login_user_disconnect = mock_login_user_disconnect,
    .login_user_get_state = mock_login_user_get_state,
    .login_user_get_idle_hint = mock_login_user_get_idle_hint,
    .settings_get_boolean = mock_settings_get_boolean,
    .settings_get_uint = mock_settings_get_uint,
    .settings_connect_changed = mock_settings_connect_changed,
    .settings_disconnect = mock_settings_disconnect,
    .session_limits_enabled = mock_session_limits_enabled,
    .session_limits_connect_changed = mock_session_limits_connect_changed,
    .session_limits_disconnect = mock_session_limits_disconnect,
    .timer_connect_estimated_times_changed = mock_timer_connect_estimated_times_changed,
    .timer_disconnect = mock_timer_disconnect,
    .timer_record_usage_async = mock_timer_record_usage_async,
    .timer_record_usage_finish = mock_timer_record_usage_finish,
    .timer_get_estimated_times_async = mock_timer_get_estimated_times_async,
    .timer_get_estimated_times_finish = mock_timer_get_estimated_times_finish,
  };

  harness->manager = shell_time_limits_manager_new_with_env (&env, harness->history_file);
}

/* Add a time change event to the event queue. This simulates the machine's
 * real time clock changing relative to its monotonic clock, at date/time
 * @time_str. Such a change can happen as the result of an NTP sync, for
 * example.
 *
 * When the event is reached, the mock real/wall clock will have its time set
 * to @new_time_str, and then @after_func will be invoked. @after_func should
 * be used to enqueue any events *after* the time change event. If they are
 * enqueued in the same scope as harness_add_time_change (), they will be
 * mis-ordered as the event queue is sorted by mock real/wall clock time. */
static void
harness_add_time_change_event (Harness     *harness,
                               const char  *time_str,
                               const char  *new_time_str,
                               HarnessFunc  after_func,
                               gpointer     after_data)
{
  Event event = {
    .type = EVENT_TIME_CHANGE,
    .time = ts (time_str),
    .new_time = ts (new_time_str),
    .after_func = after_func,
    .after_data = after_data,
  };

  harness_insert_event (harness, event);
}

/* Add a pair of sleep and resume events to the event queue. This simulates the
 * machine being asleep (suspended) and then resumed after a period of time. No
 * other events should be inserted into the queue between these two.
 *
 * This simulates the D-Bus API for logind notifying to prepare for sleep at
 * date/time @time_str, then sleeping, then notifying that the machine has
 * resumed from sleep at date/time @time_str plus @duration (in seconds). */
static void
harness_add_sleep_and_resume_event (Harness    *harness,
                                    const char *time_str,
                                    gint64      duration)
{
  gint64 time_secs = ts (time_str);
  Event prepare = {
    .type = EVENT_PREPARING_FOR_SLEEP,
    .time = time_secs,
    .new_preparing_for_sleep = TRUE,
  };
  Event change = {
    .type = EVENT_TIME_CHANGE,
    .time = time_secs + 1,
    .new_time = time_secs + duration - 1,
  };
  Event resume = {
    .type = EVENT_PREPARING_FOR_SLEEP,
    .time = time_secs + duration,
    .new_preparing_for_sleep = FALSE,
  };

  harness_insert_event (harness, prepare);
  harness_insert_event (harness, change);
  harness_insert_event (harness, resume);
}

/* Add a login user state change event to the event queue. This simulates the
 * D-Bus API for logind notifying that the user has changed state at date/time
 * @time_str. For example, this could represent the user logging out.
 *
 * @new_state is the new user state as if returned by sd_ui_get_state () and
 * @new_idle_hint the new user idle hint as per the logind API. */
static void
harness_add_login_user_state_change_event (Harness    *harness,
                                           const char *time_str,
                                           const char *new_state,
                                           gboolean    new_idle_hint)
{
  Event event = {
    .type = EVENT_LOGIN_USER_STATE_CHANGE,
    .time = ts (time_str),
    .new_user_state = new_state,
    .new_user_idle_hint = new_idle_hint,
  };

  harness_insert_event (harness, event);
}

/* Add a parental controls manager session limits changed event to the event
 * queue. This simulates the `session-limits-changed` signal of the malcontent
 * manager, notifying gnome-shell that the user's session limits were changed,
 * for example from malcontent-control. */
static void
harness_add_session_limits_changed_event (Harness    *harness,
                                          const char *time_str,
                                          gboolean    new_session_limits_enabled)
{
  Event event = {
    .type = EVENT_SESSION_LIMITS_CHANGED,
    .time = ts (time_str),
    .new_session_limits_enabled = new_session_limits_enabled,
  };

  harness_insert_event (harness, event);
}

/* Add a timer child proxy estimated times changed event to the event queue.
 * This simulates the `EstimatedTimesChanged` signal of the
 * `org.freedesktop.MalcontentTimer1.Child` object, notifying gnome-shell that
 * the user's estimated session times were changed, for example as a result of
 * gnome-shell recording usage. */
static void
harness_add_estimated_times_changed_event (Harness    *harness,
                                           const char *time_str,
                                           gboolean    new_limit_reached,
                                           const char *new_current_session_start_time,
                                           const char *new_current_session_estimated_end_time,
                                           const char *new_next_session_start_time,
                                           const char *new_next_session_estimated_end_time)
{
  Event event = {
    .type = EVENT_ESTIMATED_TIMES_CHANGED,
    .time = ts (time_str),
    .new_limit_reached = new_limit_reached,
    .new_times = {
      ts (new_current_session_start_time),
      ts (new_current_session_estimated_end_time),
      ts (new_next_session_start_time),
      ts (new_next_session_estimated_end_time),
    },
  };

  harness_insert_event (harness, event);
}

/* Add a settings change event to the event queue. This simulates dconf
 * notifying gnome-shell that the user has changed a setting, for example from
 * gnome-control-center. */
static void
harness_add_settings_change_event (Harness    *harness,
                                   const char *time_str,
                                   const char *key,
                                   GVariant   *new_value)
{
  Event event = {
    .type = EVENT_SETTINGS_CHANGE,
    .time = ts (time_str),
    .key = key,
    .new_value = g_variant_ref_sink (new_value),
  };

  harness_insert_event (harness, event);
}

/* Add a callback to the event queue. It is invoked when the simulated clock
 * reaches @time_str and can contain whatever test assertions you like. */
static void
harness_add_callback_event (Harness     *harness,
                            const char  *time_str,
                            HarnessFunc  func,
                            gpointer     func_data)
{
  Event event = {
    .type = EVENT_CALLBACK,
    .time = ts (time_str),
    .func = func,
    .func_data = func_data,
  };

  harness_insert_event (harness, event);
}

/* Add a shutdown action to the event queue. This shuts down the manager at
 * date/time @time_str, and asserts that the state after shutdown is as
 * expected. */
static void
harness_shutdown_manager (Harness    *harness,
                          const char *time_str)
{
  Event event = {
    .type = EVENT_SHUTDOWN,
    .time = ts (time_str),
  };

  harness_insert_event (harness, event);
}

/* Add a state assertion event to the event queue. This asserts that the
 * manager state equals @expected_state at date/time @time_str. */
static void
harness_expect_state (Harness              *harness,
                      const char           *time_str,
                      ShellTimeLimitsState  expected_state)
{
  Event event = {
    .type = EVENT_EXPECT_STATE,
    .time = ts (time_str),
    .expected_state = expected_state,
    .context = g_strdup (time_str),
  };

  harness_insert_event (harness, event);
}

/* Add a properties assertion event to the event queue. This asserts that the
 * state and the daily-limit-time of the manager equal the expected values at
 * date/time @time_str. */
static void
harness_expect_properties (Harness              *harness,
                           const char           *time_str,
                           ShellTimeLimitsState  expected_state,
                           guint64               expected_daily_limit_time)
{
  Event event = {
    .type = EVENT_EXPECT_PROPERTIES,
    .time = ts (time_str),
    .expected_state = expected_state,
    .check_daily_limit_time = TRUE,
    .expected_daily_limit_time = expected_daily_limit_time,
    .context = g_strdup (time_str),
  };

  harness_insert_event (harness, event);
}

/* Add a usage assertion event to the event queue. This asserts that the start
 * and end date/times of the most recent usage recording equal the expected
 * values at date/time @time_str. */
static void
harness_expect_usage (Harness    *harness,
                      const char *time_str,
                      const char *start_str,
                      const char *end_str)
{
  Event event = {
    .type = EVENT_EXPECT_USAGE,
    .time = ts (time_str),
    .expected_usage_start = ts (start_str),
    .expected_usage_end = ts (end_str),
  };

  harness_insert_event (harness, event);
}

static void
on_shutdown_ready (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  shell_time_limits_manager_shutdown_finish (SHELL_TIME_LIMITS_MANAGER (source), result, &error);
  g_assert_no_error (error);
}

static void
harness_drain (Harness *harness)
{
  while (shell_time_limits_manager_is_busy (harness->manager))
    g_main_context_iteration (NULL, TRUE);

  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static gboolean
harness_tick (Harness *harness)
{
  g_autofree char *event_time_str = NULL;
  Event event;

  if (harness->events->len == 0)
    return FALSE;

  event = g_array_index (harness->events, Event, 0);
  g_array_remove_index (harness->events, 0);

  event_time_str = time_secs_to_str (event.time);
  g_test_message ("Test tick: %d at %s", event.type, event_time_str);

  harness->current_time_secs = event.time;

  switch (event.type)
    {
    case EVENT_TIMEOUT:
      if (event.timeout_func (event.timeout_data))
        {
          event.time += event.interval_secs;
          harness_insert_event (harness, event);
        }
      break;

    case EVENT_TIME_CHANGE:
      harness->clock_offset += event.new_time - harness->current_time_secs;
      harness->current_time_secs = event.new_time;

      if (event.after_func != NULL)
        event.after_func (harness, event.after_data);

      if (harness->time_change_notify_func != NULL)
        harness->time_change_notify_func (harness->time_change_notify_data);
      break;

    case EVENT_PREPARING_FOR_SLEEP:
      harness->current_preparing_for_sleep_state = event.new_preparing_for_sleep;

      if (harness->login_manager_prepare_for_sleep_func != NULL)
        harness->login_manager_prepare_for_sleep_func (harness->current_preparing_for_sleep_state,
                                                       harness->login_manager_prepare_for_sleep_data);
      break;

    case EVENT_LOGIN_USER_STATE_CHANGE:
      harness->current_user_state = event.new_user_state;
      harness->current_user_idle_hint = event.new_user_idle_hint;

      if (harness->login_user_properties_changed_func != NULL)
        harness->login_user_properties_changed_func (harness->login_user_properties_changed_data);
      break;

    case EVENT_SESSION_LIMITS_CHANGED:
      harness->session_limits_enabled = event.new_session_limits_enabled;

      if (harness->session_limits_changed_func != NULL)
        harness->session_limits_changed_func (harness->session_limits_changed_data);
      break;

    case EVENT_ESTIMATED_TIMES_CHANGED:
      harness->limit_reached = event.new_limit_reached;
      harness->current_session_start_time_secs = event.new_times[0];
      harness->current_session_estimated_end_time_secs = event.new_times[1];
      harness->next_session_start_time_secs = event.new_times[2];
      harness->next_session_estimated_end_time_secs = event.new_times[3];

      if (harness->estimated_times_changed_func != NULL)
        harness->estimated_times_changed_func (harness->estimated_times_changed_data);
      break;

    case EVENT_SETTINGS_CHANGE:
      g_hash_table_insert (harness->settings, g_strdup (event.key),
                           g_variant_ref (event.new_value));

      if (harness->settings_changed_func != NULL)
        harness->settings_changed_func (event.key, harness->settings_changed_data);
      break;

    case EVENT_EXPECT_STATE:
    case EVENT_EXPECT_PROPERTIES:
      g_test_message ("%s state", event.context);
      g_assert_cmpstr (shell_time_limits_state_to_string (shell_time_limits_manager_get_state (harness->manager)),
                       ==,
                       shell_time_limits_state_to_string (event.expected_state));

      if (event.check_daily_limit_time)
        {
          g_test_message ("%s dailyLimitTime", event.context);
          g_assert_cmpuint (shell_time_limits_manager_get_daily_limit_time (harness->manager),
                            ==,
                            event.expected_daily_limit_time);
        }
      break;

    case EVENT_EXPECT_USAGE:
      g_assert_true (harness->have_time_store);
      g_test_message ("start");
      g_assert_cmpint (harness->time_store_start, ==, event.expected_usage_start);
      g_test_message ("end");
      g_assert_cmpint (harness->time_store_end, ==, event.expected_usage_end);
      break;

    case EVENT_CALLBACK:
      event.func (harness, event.func_data);
      break;

    case EVENT_SHUTDOWN:
      {
        Event expect = {
          .type = EVENT_EXPECT_SHUTDOWN,
          .time = event.time + 1,
        };

        shell_time_limits_manager_shutdown_async (harness->manager, NULL, on_shutdown_ready, NULL);

        /* Everything the shutdown started is finished before the next event,
         * so the state is as expected one second later */
        harness_insert_event (harness, expect);
      }
      break;

    case EVENT_EXPECT_SHUTDOWN:
      g_assert_cmpstr (shell_time_limits_state_to_string (shell_time_limits_manager_get_state (harness->manager)),
                       ==,
                       shell_time_limits_state_to_string (SHELL_TIME_LIMITS_STATE_DISABLED));
      g_assert_cmpuint (shell_time_limits_manager_get_daily_limit_time (harness->manager), ==, 0);
      break;

    default:
      g_assert_not_reached ();
    }

  event_clear (&event);
  harness_drain (harness);

  return TRUE;
}

/* Run the test, until all events are processed or an assertion fails. */
static void
harness_run (Harness *harness)
{
  g_test_message ("Starting new unit test");

  harness_drain (harness);

  while (harness_tick (harness))
    ;
}

/* The assertions about the history file */

typedef struct
{
  int old_state;
  int new_state;
  const char *wall_time;
} HistoryEntry;

typedef struct
{
  const HistoryEntry *entries;
  gsize n_entries;
  gboolean contains_only_one;
} HistoryExpectation;

static void
assert_history_file_contents (Harness  *harness,
                              gpointer  data)
{
  const HistoryExpectation *expectation = data;
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonArray *history;
  gsize length, i, j;

  g_assert_true (g_file_load_contents (harness->history_file, NULL, &contents, &length, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (json_parser_load_from_data (parser, contents, length, &error));
  g_assert_no_error (error);
  g_assert_true (JSON_NODE_HOLDS_ARRAY (json_parser_get_root (parser)));
  history = json_node_get_array (json_parser_get_root (parser));

  if (!expectation->contains_only_one)
    g_assert_cmpuint (json_array_get_length (history), ==, expectation->n_entries);

  for (i = 0; i < expectation->n_entries; i++)
    {
      const HistoryEntry *expected = &expectation->entries[i];
      gboolean found = FALSE;

      for (j = 0; j < json_array_get_length (history); j++)
        {
          JsonObject *entry = json_array_get_object_element (history, j);

          if (json_object_get_int_member (entry, "oldState") == expected->old_state &&
              json_object_get_int_member (entry, "newState") == expected->new_state &&
              json_object_get_int_member (entry, "wallTimeSecs") == ts (expected->wall_time))
            {
              /* The entries are in order */
              if (!expectation->contains_only_one)
                g_assert_cmpuint (j, ==, i);
              found = TRUE;
              break;
            }
        }

      g_assert_true (found);
    }
}

static void
assert_history_file_missing (Harness  *harness,
                             gpointer  data)
{
  g_test_message ("History file is deleted");
  g_assert_false (g_file_query_exists (harness->history_file, NULL));
}

/* Build the contents of a history file */

static char *
history_json (const HistoryEntry *entries,
              gsize               n_entries,
              gint64              time_offset)
{
  GString *string = g_string_new ("[");
  gsize i;

  for (i = 0; i < n_entries; i++)
    {
      if (i > 0)
        g_string_append_c (string, ',');
      g_string_append_printf (string,
                              "{\"oldState\":%d,\"newState\":%d,\"wallTimeSecs\":%" G_GINT64_FORMAT "}",
                              entries[i].old_state, entries[i].new_state,
                              ts (entries[i].wall_time) + time_offset);
    }

  g_string_append_c (string, ']');

  return g_string_free (string, FALSE);
}

#define USER_INACTIVE SHELL_TIME_LIMITS_USER_STATE_INACTIVE
#define USER_ACTIVE SHELL_TIME_LIMITS_USER_STATE_ACTIVE

#define S_DISABLED SHELL_TIME_LIMITS_STATE_DISABLED
#define S_ACTIVE SHELL_TIME_LIMITS_STATE_ACTIVE
#define S_LIMIT_REACHED SHELL_TIME_LIMITS_STATE_LIMIT_REACHED

#define HOURS(n) ((n) * 60 * 60)

static GVariant *
bool_value (gboolean value)
{
  return g_variant_new_boolean (value);
}

static void
test_disabled_via_gsettings (void)
{
  Harness *harness = harness_new (FALSE, FALSE, HOURS (4), NULL, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_DISABLED);
  harness_expect_state (harness, "2024-06-01T15:00:00Z", S_DISABLED);
  harness_add_login_user_state_change_event (harness, "2024-06-01T15:00:10Z", "active", FALSE);
  harness_add_login_user_state_change_event (harness, "2024-06-01T15:00:20Z", "lingering", TRUE);
  harness_expect_properties (harness, "2024-06-01T15:00:30Z", S_DISABLED, 0);
  harness_shutdown_manager (harness, "2024-06-01T15:10:00Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_cannot_be_disabled_with_parental_controls (void)
{
  Harness *harness = harness_new (FALSE, FALSE, HOURS (4), NULL, TRUE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_ACTIVE);
  harness_expect_state (harness, "2024-06-01T15:00:00Z", S_ACTIVE);
  harness_add_login_user_state_change_event (harness, "2024-06-01T15:00:10Z", "active", FALSE);
  harness_add_login_user_state_change_event (harness, "2024-06-01T15:00:20Z", "lingering", TRUE);
  harness_expect_state (harness, "2024-06-01T15:00:30Z", S_ACTIVE);
  harness_shutdown_manager (harness, "2024-06-01T15:10:00Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_toggled_via_gsettings (void)
{
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), NULL, FALSE);
  guint i;

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_ACTIVE);
  harness_add_settings_change_event (harness, "2024-06-01T11:00:00Z", "history-enabled", bool_value (FALSE));
  harness_expect_properties (harness, "2024-06-01T11:00:01Z", S_DISABLED, 0);

  /* Test that toggling it on and off fast is handled OK */
  for (i = 0; i < 3; i++)
    {
      harness_add_settings_change_event (harness, "2024-06-01T11:00:02Z", "history-enabled", bool_value (TRUE));
      harness_add_settings_change_event (harness, "2024-06-01T11:00:02Z", "history-enabled", bool_value (FALSE));
    }

  harness_add_settings_change_event (harness, "2024-06-01T11:00:03Z", "history-enabled", bool_value (TRUE));
  harness_expect_state (harness, "2024-06-01T11:00:04Z", S_ACTIVE);
  harness_shutdown_manager (harness, "2024-06-01T15:10:00Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_single_days_usage (void)
{
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), NULL, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_ACTIVE);
  harness_expect_properties (harness, "2024-06-01T13:59:59Z", S_ACTIVE, ts ("2024-06-01T14:00:00Z"));
  harness_expect_state (harness, "2024-06-01T14:00:01Z", S_LIMIT_REACHED);
  harness_shutdown_manager (harness, "2024-06-01T14:10:00Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_single_days_usage_with_parental_controls (void)
{
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), NULL, TRUE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_add_estimated_times_changed_event (harness, "2024-06-01T10:00:01Z", FALSE,
                                             "2024-06-01T10:00:00Z",
                                             "2024-06-01T13:00:00Z",
                                             "2024-06-02T00:00:00Z",
                                             "2024-06-02T03:00:00Z");
  harness_add_session_limits_changed_event (harness, "2024-06-01T10:00:02Z", TRUE);

  harness_create_manager (harness);

  harness_expect_state (harness, "2024-06-01T10:00:03Z", S_ACTIVE);
  harness_expect_state (harness, "2024-06-01T12:59:59Z", S_ACTIVE);
  harness_expect_state (harness, "2024-06-01T13:00:01Z", S_LIMIT_REACHED);
  harness_shutdown_manager (harness, "2024-06-01T13:00:02Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_single_days_usage_early_in_the_morning (void)
{
  Harness *harness = harness_new (TRUE, TRUE, HOURS (1), NULL, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T00:30:00Z");
  harness_create_manager (harness);

  harness_expect_state (harness, "2024-06-01T00:30:01Z", S_ACTIVE);
  harness_expect_properties (harness, "2024-06-01T01:29:59Z", S_ACTIVE, ts ("2024-06-01T01:30:00Z"));
  harness_expect_state (harness, "2024-06-01T01:30:01Z", S_LIMIT_REACHED);
  harness_shutdown_manager (harness, "2024-06-01T01:40:00Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_single_days_usage_with_limits_disabled (void)
{
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), NULL, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_ACTIVE);
  harness_expect_properties (harness, "2024-06-01T13:59:59Z", S_ACTIVE, ts ("2024-06-01T14:00:00Z"));
  harness_expect_state (harness, "2024-06-01T14:00:01Z", S_LIMIT_REACHED);

  /* Now disable limits and the state should change back to ACTIVE: */
  harness_add_settings_change_event (harness, "2024-06-01T14:10:00Z", "daily-limit-enabled", bool_value (FALSE));
  harness_expect_state (harness, "2024-06-01T14:10:01Z", S_ACTIVE);

  /* And try enabling it again: */
  harness_add_settings_change_event (harness, "2024-06-01T14:20:00Z", "daily-limit-enabled", bool_value (TRUE));
  harness_expect_state (harness, "2024-06-01T14:20:01Z", S_LIMIT_REACHED);

  harness_shutdown_manager (harness, "2024-06-01T14:30:00Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_resets_usage_at_end_of_day (void)
{
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), NULL, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_ACTIVE);
  harness_expect_properties (harness, "2024-06-01T15:00:00Z", S_LIMIT_REACHED, ts ("2024-06-01T14:00:00Z"));
  harness_add_login_user_state_change_event (harness, "2024-06-01T15:00:10Z", "offline", TRUE);

  /* the next day (after 03:00 in the morning) usage should be reset: */
  harness_expect_properties (harness, "2024-06-02T13:59:59Z", S_ACTIVE, 0);
  harness_add_login_user_state_change_event (harness, "2024-06-02T14:00:00Z", "active", FALSE);
  harness_expect_properties (harness, "2024-06-02T14:00:00Z", S_ACTIVE, ts ("2024-06-02T18:00:00Z"));

  /* and that limit should be reached eventually */
  harness_expect_properties (harness, "2024-06-02T18:00:01Z", S_LIMIT_REACHED, ts ("2024-06-02T18:00:00Z"));

  harness_shutdown_manager (harness, "2024-06-02T18:10:00Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_resets_usage_if_limit_changed (void)
{
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), NULL, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  /* Run until the limit is reached. */
  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_ACTIVE);
  harness_expect_properties (harness, "2024-06-01T15:00:00Z", S_LIMIT_REACHED, ts ("2024-06-01T14:00:00Z"));

  /* Increase the limit. */
  harness_add_settings_change_event (harness, "2024-06-01T15:10:00Z", "daily-limit-seconds",
                                     g_variant_new_uint32 (HOURS (8)));
  harness_expect_properties (harness, "2024-06-01T15:10:01Z", S_ACTIVE, ts ("2024-06-01T18:00:00Z"));

  /* The new limit should be reached eventually */
  harness_expect_properties (harness, "2024-06-01T18:00:01Z", S_LIMIT_REACHED, ts ("2024-06-01T18:00:00Z"));

  harness_shutdown_manager (harness, "2024-06-01T18:10:00Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_resets_usage_if_session_limit_changed (void)
{
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), NULL, TRUE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_add_estimated_times_changed_event (harness, "2024-06-01T10:00:01Z", FALSE,
                                             "2024-06-01T10:00:00Z",
                                             "2024-06-01T13:00:00Z",
                                             "2024-06-02T00:00:00Z",
                                             "2024-06-02T03:00:00Z");
  harness_add_session_limits_changed_event (harness, "2024-06-01T10:00:02Z", TRUE);
  harness_create_manager (harness);

  /* Run before the limit is reached */
  harness_expect_state (harness, "2024-06-01T10:00:03Z", S_ACTIVE);
  harness_expect_state (harness, "2024-06-01T12:00:00Z", S_ACTIVE);

  /* Change session limits part-way through the day in a way which means the
   * child still has time left that day */
  harness_add_estimated_times_changed_event (harness, "2024-06-01T12:30:00Z", FALSE,
                                             "2024-06-01T10:00:00Z",
                                             "2024-06-01T14:00:00Z",
                                             "2024-06-02T00:00:00Z",
                                             "2024-06-02T04:00:00Z");
  harness_add_session_limits_changed_event (harness, "2024-06-01T12:30:01Z", TRUE);

  /* Run before the limit is reached */
  harness_expect_state (harness, "2024-06-01T12:30:02Z", S_ACTIVE);
  harness_expect_state (harness, "2024-06-01T13:30:00Z", S_ACTIVE);

  /* Change session limits part-way through the day in a way which means the
   * child no longer has time left that day */
  harness_add_estimated_times_changed_event (harness, "2024-06-01T13:45:00Z", TRUE,
                                             "2024-06-01T10:00:00Z",
                                             "2024-06-01T13:00:00Z",
                                             "2024-06-02T00:00:00Z",
                                             "2024-06-02T03:00:00Z");
  harness_add_session_limits_changed_event (harness, "2024-06-01T13:45:01Z", TRUE);

  /* Run before the limit is over */
  harness_expect_state (harness, "2024-06-01T13:45:02Z", S_LIMIT_REACHED);
  harness_expect_state (harness, "2024-06-01T14:00:00Z", S_LIMIT_REACHED);

  /* Change session limits after the child has run out of time in a way which
   * means the child has still run out of time */
  harness_add_estimated_times_changed_event (harness, "2024-06-01T14:30:00Z", TRUE,
                                             "2024-06-01T10:00:00Z",
                                             "2024-06-01T14:00:00Z",
                                             "2024-06-02T00:00:00Z",
                                             "2024-06-02T04:00:00Z");
  harness_add_session_limits_changed_event (harness, "2024-06-01T14:30:01Z", TRUE);

  /* Run before the limit is over */
  harness_expect_state (harness, "2024-06-01T14:30:02Z", S_LIMIT_REACHED);
  harness_expect_state (harness, "2024-06-01T15:00:00Z", S_LIMIT_REACHED);

  /* Change session limits after the child has run out of time in a way which
   * means the child has more time remaining */
  harness_add_estimated_times_changed_event (harness, "2024-06-01T15:30:00Z", TRUE,
                                             "2024-06-01T10:00:00Z",
                                             "2024-06-01T16:00:00Z",
                                             "2024-06-02T00:00:00Z",
                                             "2024-06-02T06:00:00Z");
  harness_add_session_limits_changed_event (harness, "2024-06-01T15:30:01Z", TRUE);

  /* Run until the limit is reached */
  harness_expect_state (harness, "2024-06-01T15:30:02Z", S_ACTIVE);
  harness_expect_state (harness, "2024-06-01T16:00:05Z", S_LIMIT_REACHED);

  /* Run before the limit is over */
  harness_expect_state (harness, "2024-06-01T16:00:06Z", S_LIMIT_REACHED);
  harness_expect_state (harness, "2024-06-01T23:59:59Z", S_LIMIT_REACHED);

  /* After the limit is over, GetEstimatedTimes will return new values of the
   * current and next sessions, so change the values accordingly */
  harness_add_estimated_times_changed_event (harness, "2024-06-02T00:00:00Z", FALSE,
                                             "2024-06-02T00:00:00Z",
                                             "2024-06-02T06:00:00Z",
                                             "2024-06-03T00:00:00Z",
                                             "2024-06-03T06:00:00Z");
  harness_add_session_limits_changed_event (harness, "2024-06-02T00:00:01Z", TRUE);

  /* Run after the limit is over */
  harness_expect_state (harness, "2024-06-02T00:00:02Z", S_ACTIVE);

  harness_shutdown_manager (harness, "2024-06-02T01:00:00Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_records_usage_to_timer_daemon (void)
{
  Harness *harness = harness_new (FALSE, TRUE, HOURS (4), NULL, TRUE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_add_estimated_times_changed_event (harness, "2024-06-01T10:00:01Z", FALSE,
                                             "2024-06-01T10:00:00Z",
                                             "2024-06-01T20:00:00Z",
                                             "2024-06-02T00:00:00Z",
                                             "2024-06-02T10:00:00Z");
  harness_add_session_limits_changed_event (harness, "2024-06-01T10:00:02Z", TRUE);
  harness_create_manager (harness);

  /* Session limits changed */
  harness_add_session_limits_changed_event (harness, "2024-06-01T11:00:00Z", TRUE);
  harness_expect_usage (harness, "2024-06-01T11:00:01Z", "2024-06-01T10:00:00Z", "2024-06-01T11:00:00Z");

  /* User state changed */
  harness_add_login_user_state_change_event (harness, "2024-06-01T12:00:00Z", "lingering", TRUE);
  harness_expect_usage (harness, "2024-06-01T12:00:01Z", "2024-06-01T10:00:00Z", "2024-06-01T12:00:00Z");

  harness_add_login_user_state_change_event (harness, "2024-06-01T13:00:00Z", "active", FALSE);
  harness_expect_usage (harness, "2024-06-01T13:00:01Z", "2024-06-01T10:00:00Z", "2024-06-01T12:00:00Z");

  harness_add_login_user_state_change_event (harness, "2024-06-01T14:00:00Z", "offline", TRUE);
  harness_expect_usage (harness, "2024-06-01T14:00:01Z", "2024-06-01T13:00:00Z", "2024-06-01T14:00:00Z");

  harness_add_login_user_state_change_event (harness, "2024-06-01T15:00:00Z", "active", FALSE);
  harness_expect_usage (harness, "2024-06-01T15:00:01Z", "2024-06-01T13:00:00Z", "2024-06-01T14:00:00Z");

  /* Disable wellbeing & parental */
  harness_add_session_limits_changed_event (harness, "2024-06-01T16:00:00Z", FALSE);
  harness_expect_usage (harness, "2024-06-01T16:00:01Z", "2024-06-01T15:00:00Z", "2024-06-01T16:00:00Z");

  harness_add_session_limits_changed_event (harness, "2024-06-01T17:00:00Z", TRUE);
  harness_expect_usage (harness, "2024-06-01T17:00:01Z", "2024-06-01T15:00:00Z", "2024-06-01T16:00:00Z");

  /* Shutdown */
  harness_shutdown_manager (harness, "2024-06-01T18:00:00Z");
  harness_expect_usage (harness, "2024-06-01T18:00:00Z", "2024-06-01T17:00:00Z", "2024-06-01T18:00:00Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_existing_history_file (void)
{
  static const HistoryEntry entries[] = {
    { USER_INACTIVE, USER_ACTIVE, "2024-06-01T07:30:00Z" },
    { USER_ACTIVE, USER_INACTIVE, "2024-06-01T08:00:00Z" },
    { USER_INACTIVE, USER_ACTIVE, "2024-06-01T08:30:00Z" },
    { USER_ACTIVE, USER_INACTIVE, "2024-06-01T09:30:00Z" },
  };
  g_autofree char *contents = history_json (entries, G_N_ELEMENTS (entries), 0);
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), contents, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  /* The existing history file (above) lists two active periods, 07:30–08:00
   * and 08:30–09:30 that morning. So the user should have 2.5h left today. */
  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_ACTIVE);
  harness_expect_properties (harness, "2024-06-01T12:29:59Z", S_ACTIVE, ts ("2024-06-01T12:30:00Z"));
  harness_expect_state (harness, "2024-06-01T12:30:01Z", S_LIMIT_REACHED);
  harness_shutdown_manager (harness, "2024-06-01T12:40:00Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_immediately_limits_from_history_file (void)
{
  static const HistoryEntry entries[] = {
    { USER_INACTIVE, USER_ACTIVE, "2024-06-01T04:30:00Z" },
    { USER_ACTIVE, USER_INACTIVE, "2024-06-01T08:50:00Z" },
  };
  g_autofree char *contents = history_json (entries, G_N_ELEMENTS (entries), 0);
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), contents, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  /* The existing history file (above) lists one active period, 04:30–08:50
   * that morning. So the user should have no time left today. */
  harness_expect_properties (harness, "2024-06-01T10:00:01Z", S_LIMIT_REACHED, ts ("2024-06-01T08:30:00Z"));
  harness_shutdown_manager (harness, "2024-06-01T10:10:00Z");

  harness_run (harness);
  harness_free (harness);
}

static const char * const invalid_history_file_contents[] = {
  "",
  "not valid JSON",
  "[]",
  "[{}]",
  "[{\"newState\": 1, \"wallTimeSecs\": 123}]",
  "[{\"oldState\": 0, \"wallTimeSecs\": 123}]",
  "[{\"oldState\": 0, \"newState\": 1}]",
  "[{\"oldState\": \"not a number\", \"newState\": 1, \"wallTimeSecs\": 123}]",
  "[{\"oldState\": 0, \"newState\": \"not a number\", \"wallTimeSecs\": 123}]",
  "[{\"oldState\": 0, \"newState\": 1, \"wallTimeSecs\": \"not a number\"}]",
  "[{\"oldState\": 0, \"newState\": 1, \"wallTimeSecs\": 123.456}]",
  "[{\"oldState\": 666, \"newState\": 1, \"wallTimeSecs\": 123}]",
  "[{\"oldState\": 0, \"newState\": 666, \"wallTimeSecs\": 123}]",
  "[{\"oldState\": 0, \"newState\": 0, \"wallTimeSecs\": 123}]",
  "[{\"oldState\": 0, \"newState\": 1, \"wallTimeSecs\": 123},{\"oldState\": 1, \"newState\": 0, \"wallTimeSecs\": 1}]",
};

static void
test_ignores_invalid_history_file_syntax (gconstpointer data)
{
  const char *invalid_contents = data;
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), invalid_contents, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  /* The existing history file (above) is invalid or a no-op and should be
   * ignored. */
  harness_expect_properties (harness, "2024-06-01T10:00:01Z", S_ACTIVE, ts ("2024-06-01T14:00:00Z"));
  harness_shutdown_manager (harness, "2024-06-01T10:10:00Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_expires_old_entries (void)
{
  static const HistoryEntry old_entries[] = {
    { USER_INACTIVE, USER_ACTIVE, "2024-06-01T07:30:00Z" },
    { USER_ACTIVE, USER_INACTIVE, "2024-06-01T08:00:00Z" },
  };
  static const HistoryEntry recent_entries[] = {
    { USER_INACTIVE, USER_ACTIVE, "2024-06-01T08:30:00Z" },
    { USER_ACTIVE, USER_INACTIVE, "2024-06-01T09:30:00Z" },
  };
  static const HistoryEntry expected_entries[] = {
    /* Recent entries */
    { USER_INACTIVE, USER_ACTIVE, "2024-06-01T08:30:00Z" },
    { USER_ACTIVE, USER_INACTIVE, "2024-06-01T09:30:00Z" },
    /* New entries */
    { USER_INACTIVE, USER_ACTIVE, "2024-06-01T10:00:00Z" },
    { USER_ACTIVE, USER_INACTIVE, "2024-06-01T12:40:00Z" },
  };
  static const HistoryExpectation expectation = {
    expected_entries, G_N_ELEMENTS (expected_entries), FALSE
  };
  g_autofree char *old_json = history_json (old_entries, G_N_ELEMENTS (old_entries),
                                            -2 * SHELL_TIME_LIMITS_HISTORY_THRESHOLD_SECONDS);
  g_autofree char *recent_json = history_json (recent_entries, G_N_ELEMENTS (recent_entries), 0);
  g_autofree char *contents = NULL;
  Harness *harness;

  /* Old entries followed by recent entries: join the two arrays */
  old_json[strlen (old_json) - 1] = ',';
  contents = g_strconcat (old_json, recent_json + 1, NULL);

  harness = harness_new (TRUE, TRUE, HOURS (4), contents, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  /* The existing history file (above) lists two active periods, one of which
   * is a long time ago and the other is `this` morning in June. After the
   * manager is shut down and the history file stored again, the older entry
   * should have been expired. */
  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_ACTIVE);
  harness_expect_properties (harness, "2024-06-01T12:29:59Z", S_ACTIVE, ts ("2024-06-01T13:00:00Z"));
  harness_shutdown_manager (harness, "2024-06-01T12:40:00Z");
  harness_add_callback_event (harness, "2024-06-01T12:50:00Z", assert_history_file_contents,
                              (gpointer) &expectation);

  harness_run (harness);
  harness_free (harness);
}

static void
test_expires_future_entries (void)
{
  static const HistoryEntry entries[] = {
    { USER_INACTIVE, USER_ACTIVE, "3000-06-01T04:30:00Z" },
    { USER_ACTIVE, USER_INACTIVE, "3000-06-01T08:50:00Z" },
  };
  g_autofree char *contents = history_json (entries, G_N_ELEMENTS (entries), 0);
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), contents, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  /* The existing history file (above) lists one active period, 04:30–08:50
   * that morning IN THE YEAR 3000. This could have resulted from the clock
   * offset changing while offline. Ignore it; the user should still have
   * their full limit for the day. */
  harness_expect_properties (harness, "2024-06-01T10:00:01Z", S_ACTIVE, ts ("2024-06-01T14:00:00Z"));
  harness_shutdown_manager (harness, "2024-06-01T10:10:00Z");

  harness_run (harness);
  harness_free (harness);
}

static const HistoryEntry morning_entries[] = {
  { USER_INACTIVE, USER_ACTIVE, "2024-06-01T08:30:00Z" },
  { USER_ACTIVE, USER_INACTIVE, "2024-06-01T09:30:00Z" },
};

static void
test_removes_history_file_when_disabled (void)
{
  g_autofree char *contents = history_json (morning_entries, G_N_ELEMENTS (morning_entries), 0);
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), contents, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_ACTIVE);
  harness_add_settings_change_event (harness, "2024-06-01T10:00:02Z", "history-enabled", bool_value (FALSE));
  harness_expect_state (harness, "2024-06-01T10:00:03Z", S_DISABLED);
  harness_add_callback_event (harness, "2024-06-01T10:00:04Z", assert_history_file_missing, NULL);
  harness_shutdown_manager (harness, "2024-06-01T10:10:00Z");

  harness_run (harness);
  harness_free (harness);
}

static void
test_removes_history_file_when_disabled_after_limit_reached (void)
{
  g_autofree char *contents = history_json (morning_entries, G_N_ELEMENTS (morning_entries), 0);
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), contents, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_ACTIVE);
  harness_expect_state (harness, "2024-06-01T14:00:01Z", S_LIMIT_REACHED);

  /* Disable history storage and the history file should be removed and the
   * state changed to DISABLED: */
  harness_add_settings_change_event (harness, "2024-06-01T14:00:02Z", "history-enabled", bool_value (FALSE));
  harness_expect_state (harness, "2024-06-01T14:00:03Z", S_DISABLED);
  harness_add_callback_event (harness, "2024-06-01T14:00:04Z", assert_history_file_missing, NULL);

  /* Re-enable history storage and things should start afresh: */
  harness_add_settings_change_event (harness, "2024-06-01T14:10:02Z", "history-enabled", bool_value (TRUE));
  harness_expect_properties (harness, "2024-06-01T14:10:03Z", S_ACTIVE, ts ("2024-06-01T18:10:02Z"));

  harness_shutdown_manager (harness, "2024-06-01T14:20:00Z");

  harness_run (harness);
  harness_free (harness);
}

/* Time change events */

static void
after_time_change_forwards (Harness  *harness,
                            gpointer  data)
{
  /* The following events are in the new time epoch. There should be 2h of time
   * limit left for the day. */
  harness_expect_properties (harness, "2024-06-01T16:00:01Z", S_ACTIVE, ts ("2024-06-01T17:59:59Z"));
  harness_expect_properties (harness, "2024-06-01T18:00:00Z", S_LIMIT_REACHED, ts ("2024-06-01T17:59:59Z"));
  harness_shutdown_manager (harness, "2024-06-01T18:10:00Z");
}

static void
test_time_change_forwards (void)
{
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), NULL, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  /* Use up 2h of the daily limit. */
  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_ACTIVE);
  harness_expect_properties (harness, "2024-06-01T12:00:00Z", S_ACTIVE, ts ("2024-06-01T14:00:00Z"));

  harness_add_time_change_event (harness, "2024-06-01T12:00:01Z", "2024-06-01T16:00:00Z",
                                 after_time_change_forwards, NULL);

  harness_run (harness);
  harness_free (harness);
}

static void
after_time_change_backwards (Harness  *harness,
                             gpointer  data)
{
  /* The following events are in the new time epoch. There should be 2h of time
   * limit left for the day. */
  harness_expect_properties (harness, "2024-06-01T09:00:01Z", S_ACTIVE, ts ("2024-06-01T10:59:59Z"));
  harness_expect_properties (harness, "2024-06-01T11:00:00Z", S_LIMIT_REACHED, ts ("2024-06-01T10:59:59Z"));
  harness_shutdown_manager (harness, "2024-06-01T11:10:00Z");
}

static void
test_time_change_backwards (void)
{
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), NULL, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  /* Use up 2h of the daily limit. */
  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_ACTIVE);
  harness_expect_properties (harness, "2024-06-01T12:00:00Z", S_ACTIVE, ts ("2024-06-01T14:00:00Z"));

  harness_add_time_change_event (harness, "2024-06-01T12:00:01Z", "2024-06-01T09:00:00Z",
                                 after_time_change_backwards, NULL);

  harness_run (harness);
  harness_free (harness);
}

static void
test_no_usage_when_asleep (void)
{
  static const HistoryEntry suspend_entries[] = {
    { USER_ACTIVE, USER_INACTIVE, "2024-06-01T12:00:01Z" },
  };
  static const HistoryExpectation expectation = {
    suspend_entries, G_N_ELEMENTS (suspend_entries), TRUE
  };
  Harness *harness = harness_new (TRUE, TRUE, HOURS (4), NULL, FALSE);

  harness_initialize_mock_clock (harness, "2024-06-01T10:00:00Z");
  harness_create_manager (harness);

  /* Use up 2h of the daily limit. */
  harness_expect_state (harness, "2024-06-01T10:00:01Z", S_ACTIVE);
  harness_expect_properties (harness, "2024-06-01T12:00:00Z", S_ACTIVE, ts ("2024-06-01T14:00:00Z"));

  /* Sleep for 3h and that should not use up limit time. */
  harness_add_sleep_and_resume_event (harness, "2024-06-01T12:00:01Z", 3 * 60 * 60);
  harness_expect_properties (harness, "2024-06-01T15:00:02Z", S_ACTIVE, ts ("2024-06-01T17:00:00Z"));

  /* Ensure the suspend event caused a state change to inactive and its time
   * was not adjusted due to offset changes on resume */
  harness_add_callback_event (harness, "2024-06-01T15:00:02Z", assert_history_file_contents,
                              (gpointer) &expectation);

  harness_shutdown_manager (harness, "2024-06-01T15:20:00Z");

  harness_run (harness);
  harness_free (harness);
}

int
main (int   argc,
      char *argv[])
{
  gsize i;

  g_setenv ("TZ", "UTC", TRUE);
  tzset ();

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/time-limits-manager/can-be-disabled-via-gsettings",
                   test_disabled_via_gsettings);
  g_test_add_func ("/time-limits-manager/cannot-be-disabled-via-gsettings-if-parental-controls-enabled",
                   test_cannot_be_disabled_with_parental_controls);
  g_test_add_func ("/time-limits-manager/can-be-toggled-on-and-off-via-gsettings",
                   test_toggled_via_gsettings);
  g_test_add_func ("/time-limits-manager/tracks-a-single-days-usage",
                   test_single_days_usage);
  g_test_add_func ("/time-limits-manager/tracks-a-single-days-usage-with-parental-controls-limit-enabled",
                   test_single_days_usage_with_parental_controls);
  g_test_add_func ("/time-limits-manager/tracks-a-single-days-usage-early-in-the-morning",
                   test_single_days_usage_early_in_the_morning);
  g_test_add_func ("/time-limits-manager/tracks-a-single-days-usage-with-limits-disabled",
                   test_single_days_usage_with_limits_disabled);
  g_test_add_func ("/time-limits-manager/resets-usage-at-the-end-of-the-day",
                   test_resets_usage_at_end_of_day);
  g_test_add_func ("/time-limits-manager/resets-usage-if-the-time-limit-is-changed",
                   test_resets_usage_if_limit_changed);
  g_test_add_func ("/time-limits-manager/resets-usage-if-the-parental-controls-session-limit-is-changed",
                   test_resets_usage_if_session_limit_changed);
  g_test_add_func ("/time-limits-manager/records-usage-correctly-to-the-malcontent-timer-daemon",
                   test_records_usage_to_timer_daemon);
  g_test_add_func ("/time-limits-manager/tracks-usage-correctly-from-an-existing-history-file",
                   test_existing_history_file);
  g_test_add_func ("/time-limits-manager/immediately-limits-usage-from-an-existing-history-file",
                   test_immediately_limits_from_history_file);

  for (i = 0; i < G_N_ELEMENTS (invalid_history_file_contents); i++)
    {
      g_autofree char *path = g_strdup_printf ("/time-limits-manager/ignores-invalid-history-file-syntax-%" G_GSIZE_FORMAT,
                                               i + 1);

      g_test_add_data_func (path, invalid_history_file_contents[i],
                            test_ignores_invalid_history_file_syntax);
    }

  g_test_add_func ("/time-limits-manager/expires-old-entries-from-an-existing-history-file",
                   test_expires_old_entries);
  g_test_add_func ("/time-limits-manager/expires-future-entries-from-an-existing-history-file",
                   test_expires_future_entries);
  g_test_add_func ("/time-limits-manager/removes-an-existing-history-file-when-history-is-disabled",
                   test_removes_history_file_when_disabled);
  g_test_add_func ("/time-limits-manager/removes-an-existing-history-file-when-history-is-disabled-after-limit-is-reached",
                   test_removes_history_file_when_disabled_after_limit_reached);
  g_test_add_func ("/time-limits-manager/doesnt-count-usage-across-time-change-events-forwards",
                   test_time_change_forwards);
  g_test_add_func ("/time-limits-manager/doesnt-count-usage-across-time-change-events-backwards",
                   test_time_change_backwards);
  g_test_add_func ("/time-limits-manager/doesnt-count-usage-when-asleep-suspended",
                   test_no_usage_when_asleep);

  return g_test_run ();
}
