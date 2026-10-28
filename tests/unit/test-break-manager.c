/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Copyright 2024 GNOME Foundation, Inc.
 *
 * Unit tests for ShellBreakManager (src/misc/break-manager.c), a GNOME Shell
 * component to support break reminders and screen time statistics. Port of
 * tests/unit/breakManager.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <gio/gio.h>

#include "misc/break-manager.h"
#include "misc/break-manager-private.h"

/*
 * A harness for testing the ShellBreakManager class. It simulates the passage
 * of time, maintaining an internal ordered queue of events, and providing
 * three groups of mock functions which the ShellBreakManager uses to interact
 * with it: a mock version of the idle monitor, mock versions of the clock and
 * timeout functions, and a mock version of GSettings (ShellBreakManagerEnv).
 *
 * The internal ordered queue of events is sorted by time (in seconds since an
 * arbitrary epoch; the tests arbitrarily start from 100s to avoid potential
 * issues around time zero). On each harness_tick (), the next event is shifted
 * off the head of the queue and processed. An event might be a simulated idle
 * watch (mocking the user being idle), a simulated active watch, a scheduled
 * timeout, or an assertion for actually running test assertions.
 *
 * The simulated clock jumps from the scheduled time of one event to the
 * scheduled time of the next. This way, we can simulate half an hour of
 * active time (waiting for the next rest break to be due) on the computer
 * instantly.
 */

typedef enum
{
  EVENT_TIMEOUT,
  EVENT_IDLE,
  EVENT_ACTIVE,
  EVENT_DELAY_ACTION,
  EVENT_SKIP_ACTION,
  EVENT_TAKE_ACTION,
  EVENT_EXPECT_STATE,
  EVENT_EXPECT_PROPERTIES,
} EventType;

typedef struct
{
  EventType type;
  gint64 time;
  guint source_id;                 /* EVENT_TIMEOUT */
  guint interval_secs;
  GSourceFunc callback;
  gpointer callback_data;
  ShellBreakState expected_state;  /* EVENT_EXPECT_* */
  const char *expected_break_type;
  gint64 expected_break_start_time;
  gint64 expected_last_break_end_time;
} Event;

typedef struct
{
  gint64 current_time_secs;
  guint next_source_id;
  GArray *events;                  /* Event, sorted by time */

  gboolean have_idle_watch;
  guint64 idle_watch_msec;
  ShellBreakWatchFunc idle_watch_func;
  gpointer idle_watch_data;
  ShellBreakWatchFunc active_watch_func;
  gpointer active_watch_data;

  GHashTable *settings;            /* schema id -> (key -> GVariant) */
  ShellBreakManager *manager;
} Harness;

static void
free_key_values (gpointer data)
{
  g_hash_table_unref (data);
}

static void
harness_set_setting (Harness    *harness,
                     const char *schema_id,
                     const char *key,
                     GVariant   *value)
{
  GHashTable *keys = g_hash_table_lookup (harness->settings, schema_id);

  if (keys == NULL)
    {
      keys = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                    (GDestroyNotify) g_variant_unref);
      g_hash_table_insert (harness->settings, g_strdup (schema_id), keys);
    }

  g_hash_table_insert (keys, g_strdup (key), g_variant_ref_sink (value));
}

static GVariant *
harness_get_setting (Harness    *harness,
                     const char *schema_id,
                     const char *key)
{
  GHashTable *keys = g_hash_table_lookup (harness->settings, schema_id);
  GVariant *value;

  g_assert_nonnull (keys);
  value = g_hash_table_lookup (keys, key);
  g_assert_nonnull (value);

  return value;
}

/* Mock clock */

static gint64
mock_get_real_time_secs (gpointer user_data)
{
  Harness *harness = user_data;

  return harness->current_time_secs;
}

static void
harness_insert_event (Harness *harness,
                      Event    event)
{
  guint i;

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
  event.callback = func;
  event.callback_data = func_data;
  harness_insert_event (harness, event);

  return event.source_id;
}

static void
mock_source_remove (gpointer user_data,
                    guint    source_id)
{
  Harness *harness = user_data;
  guint i;

  for (i = 0; i < harness->events->len; i++)
    {
      Event *event = &g_array_index (harness->events, Event, i);

      if (event->type == EVENT_TIMEOUT && event->source_id == source_id)
        {
          g_array_remove_index (harness->events, i);
          return;
        }
    }

  g_assert_not_reached ();
}

/* Mock idle monitor */

static guint
mock_add_idle_watch (gpointer             user_data,
                     guint64              interval_msec,
                     ShellBreakWatchFunc  func,
                     gpointer             func_data)
{
  Harness *harness = user_data;

  g_assert_false (harness->have_idle_watch);
  harness->have_idle_watch = TRUE;
  harness->idle_watch_msec = interval_msec;
  harness->idle_watch_func = func;
  harness->idle_watch_data = func_data;

  return 1;
}

static guint
mock_add_user_active_watch (gpointer             user_data,
                            ShellBreakWatchFunc  func,
                            gpointer             func_data)
{
  Harness *harness = user_data;

  g_assert_null (harness->active_watch_func);
  harness->active_watch_func = func;
  harness->active_watch_data = func_data;

  return 2;
}

static void
mock_remove_watch (gpointer user_data,
                   guint    id)
{
  Harness *harness = user_data;

  g_assert_true (id == 1 || id == 2);
  if (id == 1)
    {
      harness->have_idle_watch = FALSE;
      harness->idle_watch_func = NULL;
    }
  else
    {
      harness->active_watch_func = NULL;
    }
}

/* Mock settings. The handle is the schema id. */

static gpointer
mock_settings_new (gpointer    user_data,
                   const char *schema_id,
                   const char *path)
{
  return g_strdup (schema_id);
}

static void
mock_settings_free (gpointer user_data,
                    gpointer settings)
{
  g_free (settings);
}

static const char *
mock_settings_get_path (gpointer user_data,
                        gpointer settings)
{
  return "/org/gnome/desktop/break-reminders/";
}

static gulong
mock_settings_connect_changed (gpointer             user_data,
                               gpointer             settings,
                               ShellBreakWatchFunc  func,
                               gpointer             func_data)
{
  /* no-op for mock purposes */
  return 1;
}

static void
mock_settings_disconnect (gpointer user_data,
                          gpointer settings,
                          gulong   handler_id)
{
}

static char **
mock_settings_get_strv (gpointer    user_data,
                        gpointer    settings,
                        const char *key)
{
  return g_variant_dup_strv (harness_get_setting (user_data, settings, key), NULL);
}

static guint
mock_settings_get_uint (gpointer    user_data,
                        gpointer    settings,
                        const char *key)
{
  return g_variant_get_uint32 (harness_get_setting (user_data, settings, key));
}

static gboolean
mock_settings_get_boolean (gpointer    user_data,
                           gpointer    settings,
                           const char *key)
{
  return g_variant_get_boolean (harness_get_setting (user_data, settings, key));
}

static Harness *
harness_new (void)
{
  Harness *harness = g_new0 (Harness, 1);

  harness->current_time_secs = 100;
  harness->next_source_id = 1;
  harness->events = g_array_new (FALSE, TRUE, sizeof (Event));
  harness->settings = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                             free_key_values);

  return harness;
}

static void
harness_set_break_type (Harness    *harness,
                        const char *break_type,
                        guint       duration_seconds,
                        guint       interval_seconds,
                        guint       delay_seconds)
{
  g_autofree char *schema_id = g_strdup_printf ("org.gnome.desktop.break-reminders.%s", break_type);

  harness_set_setting (harness, schema_id, "duration-seconds", g_variant_new_uint32 (duration_seconds));
  harness_set_setting (harness, schema_id, "interval-seconds", g_variant_new_uint32 (interval_seconds));
  harness_set_setting (harness, schema_id, "delay-seconds", g_variant_new_uint32 (delay_seconds));
  harness_set_setting (harness, schema_id, "notify", g_variant_new_boolean (TRUE));
  harness_set_setting (harness, schema_id, "play-sound", g_variant_new_boolean (FALSE));
  harness_set_setting (harness, schema_id, "fade-screen", g_variant_new_boolean (FALSE));
}

static void
harness_create_manager (Harness *harness)
{
  ShellBreakManagerEnv env = {
    .user_data = harness,
    .get_real_time_secs = mock_get_real_time_secs,
    .timeout_add_seconds = mock_timeout_add_seconds,
    .source_remove = mock_source_remove,
    .add_idle_watch = mock_add_idle_watch,
    .add_user_active_watch = mock_add_user_active_watch,
    .remove_watch = mock_remove_watch,
    .settings_new = mock_settings_new,
    .settings_free = mock_settings_free,
    .settings_get_path = mock_settings_get_path,
    .settings_connect_changed = mock_settings_connect_changed,
    .settings_disconnect = mock_settings_disconnect,
    .settings_get_strv = mock_settings_get_strv,
    .settings_get_uint = mock_settings_get_uint,
    .settings_get_boolean = mock_settings_get_boolean,
  };

  harness->manager = shell_break_manager_new_with_env (&env);
}

static void
harness_free (Harness *harness)
{
  g_clear_object (&harness->manager);
  g_array_unref (harness->events);
  g_hash_table_unref (harness->settings);
  g_free (harness);
}

/* Add an idle watch event to the event queue. This simulates the user
 * becoming idle (no keyboard or mouse input) at time @time_secs. */
static void
harness_add_idle_event (Harness *harness,
                        gint64   time_secs)
{
  Event event = { .type = EVENT_IDLE, .time = time_secs };

  harness_insert_event (harness, event);
}

/* Add an active watch event to the event queue. This simulates the user
 * becoming active (using the keyboard or mouse after a period of inactivity)
 * at time @time_secs. */
static void
harness_add_active_event (Harness *harness,
                          gint64   time_secs)
{
  Event event = { .type = EVENT_ACTIVE, .time = time_secs };

  harness_insert_event (harness, event);
}

/* Add a delay, skip or take action invocation to the event queue. This
 * simulates the user invoking the action (typically via a notification) at
 * time @time_secs. */
static void
harness_add_action (Harness  *harness,
                    EventType type,
                    gint64    time_secs)
{
  Event event = { .type = type, .time = time_secs };

  harness_insert_event (harness, event);
}

/* Add a state assertion event to the event queue. This asserts that the
 * ShellBreakManager state equals @expected_state at time @time_secs. */
static void
harness_expect_state (Harness        *harness,
                      gint64          time_secs,
                      ShellBreakState expected_state)
{
  Event event = {
    .type = EVENT_EXPECT_STATE,
    .time = time_secs,
    .expected_state = expected_state,
  };

  harness_insert_event (harness, event);
}

/* Add a properties assertion event to the event queue. This asserts that the
 * given ShellBreakManager properties equal the expected values at time
 * @time_secs. */
static void
harness_expect_properties (Harness        *harness,
                           gint64          time_secs,
                           ShellBreakState expected_state,
                           const char     *expected_break_type,
                           gint64          expected_break_start_time,
                           gint64          expected_last_break_end_time)
{
  Event event = {
    .type = EVENT_EXPECT_PROPERTIES,
    .time = time_secs,
    .expected_state = expected_state,
    .expected_break_type = expected_break_type,
    .expected_break_start_time = expected_break_start_time,
    .expected_last_break_end_time = expected_last_break_end_time,
  };

  harness_insert_event (harness, event);
}

static void
assert_state (Harness        *harness,
              gint64          time_secs,
              ShellBreakState expected_state)
{
  g_test_message ("%" G_GINT64_FORMAT "s state", time_secs);
  g_assert_cmpstr (shell_break_state_to_string (shell_break_manager_get_state (harness->manager)),
                   ==,
                   shell_break_state_to_string (expected_state));
}

static gboolean
harness_tick (Harness *harness)
{
  Event event;

  if (harness->events->len == 0)
    return FALSE;

  event = g_array_index (harness->events, Event, 0);
  g_array_remove_index (harness->events, 0);

  harness->current_time_secs = event.time;

  switch (event.type)
    {
    case EVENT_TIMEOUT:
      if (event.callback (event.callback_data))
        {
          event.time += event.interval_secs;
          harness_insert_event (harness, event);
        }
      break;

    case EVENT_IDLE:
      if (harness->have_idle_watch && harness->idle_watch_func != NULL)
        harness->idle_watch_func (harness->idle_watch_data);
      break;

    case EVENT_ACTIVE:
      if (harness->active_watch_func != NULL)
        {
          ShellBreakWatchFunc func = harness->active_watch_func;

          func (harness->active_watch_data);
          harness->active_watch_func = NULL;  /* one-shot */
        }
      break;

    case EVENT_DELAY_ACTION:
      shell_break_manager_delay_break (harness->manager);
      break;

    case EVENT_SKIP_ACTION:
      shell_break_manager_skip_break (harness->manager);
      break;

    case EVENT_TAKE_ACTION:
      shell_break_manager_take_break (harness->manager);
      break;

    case EVENT_EXPECT_STATE:
      assert_state (harness, event.time, event.expected_state);
      break;

    case EVENT_EXPECT_PROPERTIES:
      assert_state (harness, event.time, event.expected_state);
      g_test_message ("%" G_GINT64_FORMAT "s currentBreakType", event.time);
      g_assert_cmpstr (shell_break_manager_get_current_break_type (harness->manager),
                       ==, event.expected_break_type);
      g_test_message ("%" G_GINT64_FORMAT "s currentBreakStartTime", event.time);
      g_assert_cmpint (shell_break_manager_get_current_break_start_time (harness->manager),
                       ==, event.expected_break_start_time);
      g_test_message ("%" G_GINT64_FORMAT "s lastBreakEndTime", event.time);
      g_assert_cmpint (shell_break_manager_get_last_break_end_time (harness->manager),
                       ==, event.expected_last_break_end_time);
      break;

    default:
      g_assert_not_reached ();
    }

  return TRUE;
}

/* Run the test, until all events are processed or an assertion fails. */
static void
harness_run (Harness *harness)
{
  while (harness_tick (harness))
    ;
}

static void
harness_set_selected_breaks (Harness           *harness,
                             const char * const *breaks)
{
  harness_set_setting (harness, "org.gnome.desktop.break-reminders", "selected-breaks",
                       g_variant_new_strv (breaks, -1));
}

static void
test_disabled_via_gsettings (void)
{
  Harness *harness = harness_new ();

  harness_set_selected_breaks (harness, (const char * const []) { NULL });
  harness_create_manager (harness);

  harness_add_active_event (harness, 101);
  harness_expect_state (harness, 102, SHELL_BREAK_STATE_DISABLED);
  harness_add_idle_event (harness, 130);
  harness_expect_state (harness, 135, SHELL_BREAK_STATE_DISABLED);

  harness_run (harness);
  harness_free (harness);
}

static Harness *
harness_new_movement (void)
{
  Harness *harness = harness_new ();

  harness_set_selected_breaks (harness, (const char * const []) { "movement", NULL });
  harness_set_break_type (harness, "movement",
                          300,    /* 5 minutes */
                          1800,   /* 30 minutes */
                          300);   /* 5 minutes */
  harness_create_manager (harness);

  return harness;
}

/* A simple test which simulates the user being active briefly, taking a short
 * break before one is due, and then being active again until their next break
 * is overdue. */
static void
test_single_break_type (void)
{
  Harness *harness = harness_new_movement ();

  harness_add_active_event (harness, 101);
  harness_expect_state (harness, 102, SHELL_BREAK_STATE_ACTIVE);
  harness_add_idle_event (harness, 130);
  harness_expect_state (harness, 135, SHELL_BREAK_STATE_IDLE);
  harness_add_active_event (harness, 200);  /* cut the break short before its duration */
  harness_expect_state (harness, 201, SHELL_BREAK_STATE_ACTIVE);
  harness_expect_properties (harness, 2001,  /* break is due after 30 mins */
                             SHELL_BREAK_STATE_BREAK_DUE, "movement", 0, 100);
  harness_add_idle_event (harness, 2005);
  harness_expect_properties (harness, 2006,
                             SHELL_BREAK_STATE_IN_BREAK, "movement", 1900, 0);
  harness_expect_state (harness, 2195, SHELL_BREAK_STATE_IN_BREAK);  /* near the end of the break */
  harness_expect_state (harness, 2210, SHELL_BREAK_STATE_IDLE);  /* just after the end of the break */
  harness_add_active_event (harness, 2320);
  harness_expect_properties (harness, 2321,
                             SHELL_BREAK_STATE_ACTIVE, NULL, 0, 2320);
  harness_add_idle_event (harness, 4100);  /* start the next break a little early */
  harness_expect_state (harness, 4101, SHELL_BREAK_STATE_IDLE);
  harness_expect_state (harness, 4121, SHELL_BREAK_STATE_IN_BREAK);
  harness_add_active_event (harness, 4420);
  harness_expect_properties (harness, 4421,
                             SHELL_BREAK_STATE_ACTIVE, NULL, 0, 4420);

  harness_run (harness);
  harness_free (harness);
}

/* Test requesting to delay a break. */
static void
test_delay_break (void)
{
  Harness *harness = harness_new_movement ();

  harness_add_active_event (harness, 101);
  harness_expect_state (harness, 102, SHELL_BREAK_STATE_ACTIVE);
  harness_expect_properties (harness, 1901,  /* break is due after 30 mins */
                             SHELL_BREAK_STATE_BREAK_DUE, "movement", 0, 100);
  harness_add_action (harness, EVENT_DELAY_ACTION, 1902);
  harness_expect_properties (harness, 1903,  /* break is delayed by 5 mins */
                             SHELL_BREAK_STATE_ACTIVE, NULL, 0, 400);
  harness_expect_properties (harness, 2201,  /* break is due after another 5 mins */
                             SHELL_BREAK_STATE_BREAK_DUE, "movement", 0, 400);
  harness_add_idle_event (harness, 2202);
  harness_expect_state (harness, 2203, SHELL_BREAK_STATE_IN_BREAK);

  harness_run (harness);
  harness_free (harness);
}

/* Test requesting to skip a break. */
static void
test_skip_break (void)
{
  Harness *harness = harness_new_movement ();

  harness_add_active_event (harness, 101);
  harness_expect_state (harness, 102, SHELL_BREAK_STATE_ACTIVE);
  harness_expect_properties (harness, 1901,  /* break is due after 30 mins */
                             SHELL_BREAK_STATE_BREAK_DUE, "movement", 0, 100);
  harness_add_action (harness, EVENT_SKIP_ACTION, 1902);
  harness_expect_properties (harness, 1903,  /* break is skipped for 30 mins */
                             SHELL_BREAK_STATE_ACTIVE, NULL, 0, 1902);
  harness_expect_properties (harness, 3703,  /* break is due after another 30 mins */
                             SHELL_BREAK_STATE_BREAK_DUE, "movement", 0, 1902);
  harness_add_idle_event (harness, 3704);
  harness_expect_state (harness, 3704, SHELL_BREAK_STATE_IN_BREAK);

  harness_run (harness);
  harness_free (harness);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/break-manager/can-be-disabled-via-gsettings", test_disabled_via_gsettings);
  g_test_add_func ("/break-manager/tracks-a-single-break-type", test_single_break_type);
  g_test_add_func ("/break-manager/supports-delaying-a-break", test_delay_break);
  g_test_add_func ("/break-manager/supports-skipping-a-break", test_skip_break);

  return g_test_run ();
}
