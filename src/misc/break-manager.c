/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * break-manager.c: A GNOME Shell component to support break reminders and
 * screen time statistics. Port of js/misc/breakManager.js.
 *
 * Copyright 2024 GNOME Foundation, Inc.
 *
 *  - #ShellBreakManager is the state machine. Its clock, timeouts, idle
 *    monitor and settings come from a ShellBreakManagerEnv, so that
 *    tests/unit/test-break-manager.c can drive it with a simulated clock.
 *  - #ShellBreakDispatcher turns the state into notifications, sounds, a screen
 *    fade and a countdown (private types BreakNotificationSource,
 *    BreakNotification and BreakCountdownLabel). It reaches the parts of the
 *    shell it needs through its construct properties.
 *
 * Times are whole seconds (the JS used fractional ones).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>
#include <meta/meta-backend.h>
#include <meta/meta-idle-monitor.h>
#include <meta/compositor.h>

#include "misc/break-manager.h"
#include "misc/break-manager-private.h"
#include "misc/system-actions.h"
#include "shell-app-system.h"
#include "shell-enum-types.h"
#include "shell-global.h"
#include "ui/environment.h"
#include "st.h"

#define BREAK_OVERDUE_TIME_SECONDS 60 /* time after a break is due when the user is notified it is overdue */
#define BREAK_UPCOMING_NOTIFICATION_TIME_SECONDS 2 * 60 /* notify the user 2min before a break is due */
#define BREAK_COUNTDOWN_TIME_SECONDS 60

#define BRIGHTNESS_FADE_TIME_SECONDS 3
#define BRIGHTNESS_FACTOR 0.6

#define WELLBEING_DESKTOP_ID "gnome-wellbeing-panel.desktop"

typedef enum
{
  IDLE_STATE_ACTIVE = 1,
  IDLE_STATE_IDLE = 2,
} IdleState;

/* The code supports whatever break types the user wants. However, they each
 * need to be backed by a org.gnome.desktop.break-reminders.* child schema, and
 * those have to be pre-defined because they all have different default values.
 * So we need to validate the break types to prevent GSettings complaining about
 * non-installed schemas.
 *
 * So this needs to mirror the child schemas defined in
 * `schemas/org.gnome.desktop.break-reminders.gschema.xml.in` in
 * gsettings-desktop-schemas. */
static const char * const supported_break_types[] = {
  "eyesight",
  "movement",
  NULL
};

/**
 * shell_break_state_to_string:
 * @state: a #ShellBreakState
 *
 * Returns: (transfer none) (nullable): a string form of @state
 */
const char *
shell_break_state_to_string (ShellBreakState state)
{
  switch (state)
    {
    case SHELL_BREAK_STATE_DISABLED:
      return "DISABLED";
    case SHELL_BREAK_STATE_ACTIVE:
      return "ACTIVE";
    case SHELL_BREAK_STATE_IDLE:
      return "IDLE";
    case SHELL_BREAK_STATE_IN_BREAK:
      return "IN_BREAK";
    case SHELL_BREAK_STATE_BREAK_DUE:
      return "BREAK_DUE";
    default:
      return NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* ShellBreakManager                                                         */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_STATE,
  PROP_LAST_BREAK_END_TIME,
  PROP_NEXT_BREAK_DUE_TIME,
  PROP_ENV,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  BREAK_DUE,
  BREAK_FINISHED,
  TAKE_BREAK,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

typedef struct
{
  const char *type;     /* one of supported_break_types */
  gpointer settings;
  gulong changed_id;
  gint64 last_end;      /* wall clock time in seconds */
} BreakType;

struct _ShellBreakManager
{
  GObject parent;

  ShellBreakManagerEnv env;
  gboolean have_env;
  gboolean shut_down;

  gpointer break_settings;
  gulong break_settings_changed_id;

  ShellBreakState state;
  GPtrArray *break_types;     /* BreakType, in the order they were enabled */

  guint idle_watch_id;
  guint active_watch_id;
  guint timer_id;

  const char *current_break_type;
  gint64 current_break_start_time;
  IdleState idle_state;
  gint64 idle_start_time;
};

G_DEFINE_FINAL_TYPE (ShellBreakManager, shell_break_manager, G_TYPE_OBJECT)

static void update_state (ShellBreakManager *self,
                          gint64             current_time);

/* The real environment */

typedef struct
{
  MetaIdleMonitor *idle_monitor;
} DefaultEnv;

typedef struct
{
  ShellBreakWatchFunc func;
  gpointer func_data;
} WatchData;

static void
default_watch_cb (MetaIdleMonitor *monitor,
                  guint            watch_id,
                  gpointer         user_data)
{
  WatchData *data = user_data;

  data->func (data->func_data);
}

static gint64
default_get_real_time_secs (gpointer user_data)
{
  return g_get_real_time () / G_USEC_PER_SEC;
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
default_add_idle_watch (gpointer             user_data,
                        guint64              interval_msec,
                        ShellBreakWatchFunc  func,
                        gpointer             func_data)
{
  DefaultEnv *env = user_data;
  WatchData *data = g_new0 (WatchData, 1);

  data->func = func;
  data->func_data = func_data;

  return meta_idle_monitor_add_idle_watch_full (env->idle_monitor, interval_msec,
                                                default_watch_cb, data, g_free,
                                                META_IDLE_MONITOR_WATCH_FLAGS_UNINHIBITABLE |
                                                META_IDLE_MONITOR_WATCH_FLAGS_START_NOW);
}

static guint
default_add_user_active_watch (gpointer             user_data,
                               ShellBreakWatchFunc  func,
                               gpointer             func_data)
{
  DefaultEnv *env = user_data;
  WatchData *data = g_new0 (WatchData, 1);

  data->func = func;
  data->func_data = func_data;

  return meta_idle_monitor_add_user_active_watch (env->idle_monitor,
                                                  default_watch_cb, data, g_free);
}

static void
default_remove_watch (gpointer user_data,
                      guint    id)
{
  DefaultEnv *env = user_data;

  meta_idle_monitor_remove_watch (env->idle_monitor, id);
}

static gpointer
default_settings_new (gpointer    user_data,
                      const char *schema_id,
                      const char *path)
{
  if (path != NULL)
    return g_settings_new_with_path (schema_id, path);

  return g_settings_new (schema_id);
}

static void
default_settings_free (gpointer user_data,
                       gpointer settings)
{
  g_object_unref (settings);
}

static const char *
default_settings_get_path (gpointer user_data,
                           gpointer settings)
{
  g_autofree char *path = NULL;

  /* The path is a construct-only property of the settings: it is kept alive by
   * the settings object */
  g_object_get (settings, "path", &path, NULL);

  return g_intern_string (path);
}

static void
default_settings_changed_cb (GSettings  *settings,
                             const char *key,
                             gpointer    user_data)
{
  WatchData *data = user_data;

  data->func (data->func_data);
}

static void
watch_data_free (gpointer data,
                 GClosure *closure)
{
  g_free (data);
}

static gulong
default_settings_connect_changed (gpointer             user_data,
                                  gpointer             settings,
                                  ShellBreakWatchFunc  func,
                                  gpointer             func_data)
{
  WatchData *data = g_new0 (WatchData, 1);

  data->func = func;
  data->func_data = func_data;

  return g_signal_connect_data (settings, "changed",
                                G_CALLBACK (default_settings_changed_cb), data,
                                (GClosureNotify) watch_data_free, 0);
}

static void
default_settings_disconnect (gpointer user_data,
                             gpointer settings,
                             gulong   handler_id)
{
  g_signal_handler_disconnect (settings, handler_id);
}

static char **
default_settings_get_strv (gpointer    user_data,
                           gpointer    settings,
                           const char *key)
{
  return g_settings_get_strv (settings, key);
}

static guint
default_settings_get_uint (gpointer    user_data,
                           gpointer    settings,
                           const char *key)
{
  return g_settings_get_uint (settings, key);
}

static gboolean
default_settings_get_boolean (gpointer    user_data,
                              gpointer    settings,
                              const char *key)
{
  return g_settings_get_boolean (settings, key);
}

static void
default_env_free (gpointer user_data)
{
  DefaultEnv *env = user_data;

  g_clear_object (&env->idle_monitor);
  g_free (env);
}

static void
default_env_init (ShellBreakManagerEnv *env)
{
  DefaultEnv *data = g_new0 (DefaultEnv, 1);
  MetaBackend *backend = shell_global_get_backend (shell_global_get ());

  data->idle_monitor = g_object_ref (meta_backend_get_core_idle_monitor (backend));

  env->user_data = data;
  env->destroy = default_env_free;
  env->get_real_time_secs = default_get_real_time_secs;
  env->timeout_add_seconds = default_timeout_add_seconds;
  env->source_remove = default_source_remove;
  env->add_idle_watch = default_add_idle_watch;
  env->add_user_active_watch = default_add_user_active_watch;
  env->remove_watch = default_remove_watch;
  env->settings_new = default_settings_new;
  env->settings_free = default_settings_free;
  env->settings_get_path = default_settings_get_path;
  env->settings_connect_changed = default_settings_connect_changed;
  env->settings_disconnect = default_settings_disconnect;
  env->settings_get_strv = default_settings_get_strv;
  env->settings_get_uint = default_settings_get_uint;
  env->settings_get_boolean = default_settings_get_boolean;
}

/* Helpers */

static BreakType *
find_break_type (ShellBreakManager *self,
                 const char        *break_type)
{
  guint i;

  if (break_type == NULL)
    return NULL;

  for (i = 0; i < self->break_types->len; i++)
    {
      BreakType *bt = g_ptr_array_index (self->break_types, i);

      if (g_str_equal (bt->type, break_type))
        return bt;
    }

  return NULL;
}

static guint
break_type_get_uint (ShellBreakManager *self,
                     BreakType         *bt,
                     const char        *key)
{
  return self->env.settings_get_uint (self->env.user_data, bt->settings, key);
}

static gboolean
break_type_get_boolean (ShellBreakManager *self,
                        const char        *break_type,
                        const char        *key)
{
  BreakType *bt = find_break_type (self, break_type);

  if (bt == NULL)
    return FALSE;

  return self->env.settings_get_boolean (self->env.user_data, bt->settings, key);
}

static void
break_type_free (BreakType *bt)
{
  /* The settings were already disconnected and freed by the caller */
  g_free (bt);
}

static void
set_state (ShellBreakManager *self,
           ShellBreakState    state,
           const char        *break_type,
           gint64             start_time)
{
  self->state = state;
  self->current_break_type = break_type;
  self->current_break_start_time = start_time;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_STATE]);
}

static gboolean
on_update_timeout (gpointer user_data)
{
  ShellBreakManager *self = user_data;

  self->timer_id = 0;
  g_debug ("BreakManager: Scheduled state update");
  update_state (self, shell_break_manager_get_current_time (self));

  return G_SOURCE_REMOVE;
}

static void
schedule_update_state (ShellBreakManager *self,
                       gint64             timeout)
{
  if (self->timer_id != 0)
    self->env.source_remove (self->env.user_data, self->timer_id);

  /* Never spin */
  if (timeout < 1)
    timeout = 1;

  g_debug ("BreakManager: Scheduling state update in %" G_GINT64_FORMAT "s", timeout);

  self->timer_id = self->env.timeout_add_seconds (self->env.user_data, (guint) timeout,
                                                  on_update_timeout, self);
}

static void
update_state (ShellBreakManager *self,
              gint64             current_time)
{
  gint64 idle_time_seconds = current_time - self->idle_start_time;

  if (self->idle_state == IDLE_STATE_IDLE)
    {
      const char *due_break_type = NULL;
      gint64 next_due_time;
      gboolean in_break = FALSE;
      gint64 due_break_start_time = 0;
      ShellBreakState new_state;

      /* What kind of break are we due, if any? */
      next_due_time = shell_break_manager_get_next_break_due (self, current_time,
                                                              &due_break_type);

      if (due_break_type != NULL && next_due_time <= current_time)
        {
          BreakType *bt = find_break_type (self, due_break_type);
          guint duration = break_type_get_uint (self, bt, "duration-seconds");

          /* Has the break been finished? */
          if (next_due_time + duration <= current_time)
            {
              if (self->state == SHELL_BREAK_STATE_IN_BREAK)
                g_signal_emit (self, signals[BREAK_FINISHED], 0);
            }
          else
            {
              /* Start a timer to announce the end of the break. */
              in_break = TRUE;
              due_break_start_time = next_due_time;
              schedule_update_state (self, next_due_time + duration - current_time);
            }
        }
      else if (next_due_time != 0)
        {
          /* The user is idle but is not due a break. Store up the idle time
           * and update the break end times when the user next becomes active.
           * But also schedule a timer to notify the user about the start of
           * their scheduled break. */
          schedule_update_state (self, next_due_time - current_time);
        }

      new_state = in_break ? SHELL_BREAK_STATE_IN_BREAK : SHELL_BREAK_STATE_IDLE;
      if (self->state != new_state)
        {
          g_debug ("BreakManager: Changing state to %s",
                   shell_break_state_to_string (new_state));
          set_state (self, new_state, due_break_type, due_break_start_time);
        }
    }
  else if (self->idle_state == IDLE_STATE_ACTIVE)
    {
      gboolean emit_break_due = FALSE;
      const char *due_break_type = NULL;
      gint64 next_due_time;
      gboolean is_break_due;

      g_object_freeze_notify (G_OBJECT (self));

      /* How long was the user idle before becoming active? Use that to reset
       * the break end times. Reset every break type which is shorter than the
       * idle time. This allows the user to start a break a little early, or
       * take an unexpected break, and avoid their computer pestering them to
       * take a break on schedule shortly after they get back. */
      g_debug ("BreakManager: idleStartTime: %" G_GINT64_FORMAT "s", self->idle_start_time);

      if (self->idle_start_time > 0)
        {
          guint i;

          for (i = 0; i < self->break_types->len; i++)
            {
              BreakType *bt = g_ptr_array_index (self->break_types, i);
              guint duration = break_type_get_uint (self, bt, "duration-seconds");

              if (idle_time_seconds >= duration)
                bt->last_end = current_time;
            }

          g_object_notify_by_pspec (G_OBJECT (self), props[PROP_NEXT_BREAK_DUE_TIME]);
          g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LAST_BREAK_END_TIME]);
        }

      /* Reset the idle start time state. */
      self->idle_start_time = 0;

      /* Are any breaks due now? */
      next_due_time = shell_break_manager_get_next_break_due (self, current_time,
                                                              &due_break_type);
      is_break_due = due_break_type != NULL && next_due_time <= current_time;

      g_debug ("BreakManager: nextDueTime: %" G_GINT64_FORMAT "s, currentTime: %" G_GINT64_FORMAT "s, dueBreakType: %s",
               next_due_time, current_time, due_break_type ? due_break_type : "(null)");

      if (is_break_due)
        {
          /* Notify that a break is due if we have not done so already. */
          if (self->state == SHELL_BREAK_STATE_ACTIVE)
            {
              set_state (self, SHELL_BREAK_STATE_BREAK_DUE, due_break_type, next_due_time);
              emit_break_due = TRUE;
            }
        }
      else
        {
          if (next_due_time != 0)
            {
              /* Set a timer for when the next break is due. */
              schedule_update_state (self, next_due_time - current_time);
            }

          if (self->state != SHELL_BREAK_STATE_ACTIVE)
            {
              g_debug ("BreakManager: Changing state to ACTIVE");
              set_state (self, SHELL_BREAK_STATE_ACTIVE, NULL, 0);
            }
        }

      g_object_thaw_notify (G_OBJECT (self));
      if (emit_break_due)
        g_signal_emit (self, signals[BREAK_DUE], 0);
    }
}

static void
on_user_active_watch (gpointer user_data)
{
  ShellBreakManager *self = user_data;
  gint64 current_time;

  if (self->state == SHELL_BREAK_STATE_DISABLED)
    {
      g_debug ("BreakManager: Active received when manager is disabled");
      return;
    }

  current_time = shell_break_manager_get_current_time (self);
  g_debug ("BreakManager: on_user_active_watch at %" G_GINT64_FORMAT "s", current_time);

  /* Tidy up after the active watch, which is one-shot.
   * (The idle watch stays installed until explicitly removed.) */
  self->env.remove_watch (self->env.user_data, self->active_watch_id);
  self->active_watch_id = 0;

  self->idle_state = IDLE_STATE_ACTIVE;
  update_state (self, current_time);
}

static void
on_idle_watch (gpointer user_data)
{
  ShellBreakManager *self = user_data;
  gint64 current_time;

  if (self->state == SHELL_BREAK_STATE_DISABLED)
    {
      g_debug ("BreakManager: Idle received when manager is disabled");
      return;
    }

  current_time = shell_break_manager_get_current_time (self);
  g_debug ("BreakManager: on_idle_watch at %" G_GINT64_FORMAT "s", current_time);

  /* Start watching to see if the user becomes active again. */
  if (self->active_watch_id == 0)
    {
      self->active_watch_id =
        self->env.add_user_active_watch (self->env.user_data,
                                         on_user_active_watch, self);
    }

  self->idle_state = IDLE_STATE_IDLE;
  /* already waited MIN_BREAK_LENGTH_SECONDS for the idle watch */
  self->idle_start_time = current_time - SHELL_MIN_BREAK_LENGTH_SECONDS;
  update_state (self, current_time);
}

static void
start_state_machine (ShellBreakManager *self)
{
  self->idle_watch_id =
    self->env.add_idle_watch (self->env.user_data,
                              SHELL_MIN_BREAK_LENGTH_SECONDS * 1000,
                              on_idle_watch, self);

  self->state = SHELL_BREAK_STATE_ACTIVE;
  self->current_break_type = NULL;
  self->current_break_start_time = 0;
  self->idle_state = IDLE_STATE_ACTIVE;
  self->idle_start_time = 0;

  update_state (self, shell_break_manager_get_current_time (self));
}

static void
break_type_disable (ShellBreakManager *self,
                    BreakType         *bt)
{
  self->env.settings_disconnect (self->env.user_data, bt->settings, bt->changed_id);
  self->env.settings_free (self->env.user_data, bt->settings);
  bt->settings = NULL;
}

static void
stop_state_machine (ShellBreakManager *self)
{
  if (self->idle_watch_id != 0)
    self->env.remove_watch (self->env.user_data, self->idle_watch_id);
  self->idle_watch_id = 0;

  if (self->active_watch_id != 0)
    self->env.remove_watch (self->env.user_data, self->active_watch_id);
  self->active_watch_id = 0;

  if (self->timer_id != 0)
    self->env.source_remove (self->env.user_data, self->timer_id);
  self->timer_id = 0;

  while (self->break_types->len > 0)
    {
      BreakType *bt = g_ptr_array_index (self->break_types, 0);

      break_type_disable (self, bt);
      g_ptr_array_remove_index (self->break_types, 0);
    }

  self->state = SHELL_BREAK_STATE_DISABLED;
  self->current_break_type = NULL;
  self->current_break_start_time = 0;
  self->idle_state = IDLE_STATE_ACTIVE;
  self->idle_start_time = 0;

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_STATE]);
}

static void
on_break_type_settings_changed (gpointer user_data)
{
  ShellBreakManager *self = user_data;

  if (self->shut_down)
    return;

  update_state (self, shell_break_manager_get_current_time (self));
}

static void
update_settings (ShellBreakManager *self)
{
  g_auto (GStrv) selected_breaks = NULL;
  gint64 current_time;
  guint i;

  if (self->shut_down)
    return;

  /* Load settings for each enabled type of break */
  selected_breaks = self->env.settings_get_strv (self->env.user_data,
                                                 self->break_settings,
                                                 "selected-breaks");

  if (selected_breaks == NULL || selected_breaks[0] == NULL)
    {
      stop_state_machine (self);
      return;
    }

  current_time = shell_break_manager_get_current_time (self);

  for (i = 0; supported_break_types[i] != NULL; i++)
    {
      const char *type = supported_break_types[i];
      gboolean selected = g_strv_contains ((const char * const *) selected_breaks, type);
      BreakType *bt = find_break_type (self, type);

      if (selected && bt == NULL)
        {
          /* Enabling a previously disabled break */
          g_autofree char *schema_id = g_strdup_printf ("org.gnome.desktop.break-reminders.%s", type);
          g_autofree char *path = g_strdup_printf ("%s%s/",
                                                   self->env.settings_get_path (self->env.user_data,
                                                                                self->break_settings),
                                                   type);

          bt = g_new0 (BreakType, 1);
          bt->type = type;
          bt->settings = self->env.settings_new (self->env.user_data, schema_id, path);
          bt->changed_id = self->env.settings_connect_changed (self->env.user_data,
                                                               bt->settings,
                                                               on_break_type_settings_changed,
                                                               self);
          bt->last_end = current_time;
          g_ptr_array_add (self->break_types, bt);
        }
      else if (!selected && bt != NULL)
        {
          /* Disabling a previously enabled break */
          break_type_disable (self, bt);
          g_ptr_array_remove (self->break_types, bt);
        }
    }

  g_object_freeze_notify (G_OBJECT (self));
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_NEXT_BREAK_DUE_TIME]);

  if (self->state == SHELL_BREAK_STATE_DISABLED)
    start_state_machine (self);
  else
    update_state (self, current_time);

  g_object_thaw_notify (G_OBJECT (self));
}

static void
on_break_settings_changed (gpointer user_data)
{
  update_settings (user_data);
}

/* GObject */

static void
shell_break_manager_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellBreakManager *self = SHELL_BREAK_MANAGER (object);

  switch (prop_id)
    {
    case PROP_STATE:
      g_value_set_enum (value, self->state);
      break;

    case PROP_LAST_BREAK_END_TIME:
      g_value_set_uint64 (value, shell_break_manager_get_last_break_end_time (self));
      break;

    case PROP_NEXT_BREAK_DUE_TIME:
      g_value_set_uint64 (value,
                          shell_break_manager_get_next_break_due_time (self,
                                                                       shell_break_manager_get_current_time (self)));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_break_manager_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  ShellBreakManager *self = SHELL_BREAK_MANAGER (object);

  switch (prop_id)
    {
    case PROP_ENV:
      {
        const ShellBreakManagerEnv *env = g_value_get_pointer (value);

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

/* The idle monitor and the timers are of no use any more; the nested main
 * loop of the shutdown must not make the manager notify anything */
static void
on_global_shutdown (ShellGlobal       *global,
                    ShellBreakManager *self)
{
  self->shut_down = TRUE;

  if (self->idle_watch_id != 0)
    self->env.remove_watch (self->env.user_data, self->idle_watch_id);
  self->idle_watch_id = 0;

  if (self->active_watch_id != 0)
    self->env.remove_watch (self->env.user_data, self->active_watch_id);
  self->active_watch_id = 0;

  if (self->timer_id != 0)
    self->env.source_remove (self->env.user_data, self->timer_id);
  self->timer_id = 0;
}

static void
shell_break_manager_constructed (GObject *object)
{
  ShellBreakManager *self = SHELL_BREAK_MANAGER (object);

  G_OBJECT_CLASS (shell_break_manager_parent_class)->constructed (object);

  /* Allow the global state to be overridden for unit testing */
  if (!self->have_env)
    {
      default_env_init (&self->env);
      self->have_env = TRUE;

      g_signal_connect_object (shell_global_get (), "shutdown",
                               G_CALLBACK (on_global_shutdown), self, 0);
    }

  self->break_settings = self->env.settings_new (self->env.user_data,
                                                 "org.gnome.desktop.break-reminders",
                                                 NULL);
  self->break_settings_changed_id =
    self->env.settings_connect_changed (self->env.user_data, self->break_settings,
                                        on_break_settings_changed, self);

  /* Start tracking timings */
  update_settings (self);
}

static void
shell_break_manager_dispose (GObject *object)
{
  ShellBreakManager *self = SHELL_BREAK_MANAGER (object);

  if (self->have_env)
    {
      if (self->idle_watch_id != 0)
        self->env.remove_watch (self->env.user_data, self->idle_watch_id);
      self->idle_watch_id = 0;

      if (self->active_watch_id != 0)
        self->env.remove_watch (self->env.user_data, self->active_watch_id);
      self->active_watch_id = 0;

      if (self->timer_id != 0)
        self->env.source_remove (self->env.user_data, self->timer_id);
      self->timer_id = 0;

      while (self->break_types->len > 0)
        {
          BreakType *bt = g_ptr_array_index (self->break_types, 0);

          break_type_disable (self, bt);
          g_ptr_array_remove_index (self->break_types, 0);
        }

      if (self->break_settings != NULL)
        {
          self->env.settings_disconnect (self->env.user_data, self->break_settings,
                                         self->break_settings_changed_id);
          self->env.settings_free (self->env.user_data, self->break_settings);
          self->break_settings = NULL;
        }
    }

  G_OBJECT_CLASS (shell_break_manager_parent_class)->dispose (object);
}

static void
shell_break_manager_finalize (GObject *object)
{
  ShellBreakManager *self = SHELL_BREAK_MANAGER (object);

  g_ptr_array_free (self->break_types, TRUE);

  if (self->have_env && self->env.destroy != NULL)
    self->env.destroy (self->env.user_data);

  G_OBJECT_CLASS (shell_break_manager_parent_class)->finalize (object);
}

static void
shell_break_manager_class_init (ShellBreakManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_break_manager_get_property;
  object_class->set_property = shell_break_manager_set_property;
  object_class->constructed = shell_break_manager_constructed;
  object_class->dispose = shell_break_manager_dispose;
  object_class->finalize = shell_break_manager_finalize;

  /**
   * ShellBreakManager:state:
   *
   * The current state of the state machine.
   */
  props[PROP_STATE] =
    g_param_spec_enum ("state", NULL, NULL,
                       SHELL_TYPE_BREAK_STATE,
                       SHELL_BREAK_STATE_DISABLED,
                       G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellBreakManager:last-break-end-time:
   *
   * End time for the most recent break, in seconds since the Unix epoch, or 0
   * if a break is currently in progress or no breaks have happened yet.
   */
  props[PROP_LAST_BREAK_END_TIME] =
    g_param_spec_uint64 ("last-break-end-time", NULL, NULL,
                         0, G_MAXUINT64, 0,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellBreakManager:next-break-due-time:
   *
   * Time when the next break is due. This is the result of
   * shell_break_manager_get_next_break_due_time () with the current time.
   */
  props[PROP_NEXT_BREAK_DUE_TIME] =
    g_param_spec_uint64 ("next-break-due-time", NULL, NULL,
                         0, G_MAXUINT64, 0,
                         G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /*
   * ShellBreakManager:env: (skip)
   *
   * A ShellBreakManagerEnv for the unit tests; see break-manager-private.h.
   */
  props[PROP_ENV] =
    g_param_spec_pointer ("env", NULL, NULL,
                          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellBreakManager::break-due:
   * @self: the break manager
   *
   * A break became due while the user was active.
   */
  signals[BREAK_DUE] =
    g_signal_new ("break-due", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellBreakManager::break-finished:
   * @self: the break manager
   *
   * The break the user was taking is over.
   */
  signals[BREAK_FINISHED] =
    g_signal_new ("break-finished", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellBreakManager::take-break:
   * @self: the break manager
   *
   * The user explicitly wants to start taking a break now, even if they are
   * technically still active (shell_break_manager_take_break ()).
   */
  signals[TAKE_BREAK] =
    g_signal_new ("take-break", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
shell_break_manager_init (ShellBreakManager *self)
{
  self->break_types = g_ptr_array_new_with_free_func ((GDestroyNotify) break_type_free);
  self->state = SHELL_BREAK_STATE_DISABLED;
  self->idle_state = IDLE_STATE_ACTIVE;
}

/**
 * shell_break_manager_new:
 *
 * Creates a break manager that tracks the real clock and idle time and reads
 * the `org.gnome.desktop.break-reminders` settings. src/ui/main.c creates the
 * only instance.
 *
 * Returns: (transfer full): a new #ShellBreakManager
 */
ShellBreakManager *
shell_break_manager_new (void)
{
  return g_object_new (SHELL_TYPE_BREAK_MANAGER, NULL);
}

ShellBreakManager *
shell_break_manager_new_with_env (const ShellBreakManagerEnv *env)
{
  return g_object_new (SHELL_TYPE_BREAK_MANAGER, "env", env, NULL);
}

/**
 * shell_break_manager_get_state:
 * @self: a #ShellBreakManager
 *
 * Returns: the current state machine state
 */
ShellBreakState
shell_break_manager_get_state (ShellBreakManager *self)
{
  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), SHELL_BREAK_STATE_DISABLED);

  return self->state;
}

/**
 * shell_break_manager_get_current_break_type:
 * @self: a #ShellBreakManager
 *
 * String identifier for the break type which is currently in progress or due.
 *
 * Returns: (transfer none) (nullable): the break type, or %NULL if no break is
 *   currently due or happening
 */
const char *
shell_break_manager_get_current_break_type (ShellBreakManager *self)
{
  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), NULL);

  return self->current_break_type;
}

/**
 * shell_break_manager_get_current_break_start_time:
 * @self: a #ShellBreakManager
 *
 * Start time for the break which is currently in progress, or 0 if no break is
 * in progress. If the user was idle before the break started, the start time
 * will be during the idle period, not the start of the idle period.
 *
 * Returns: the start time in seconds since the Unix epoch
 */
gint64
shell_break_manager_get_current_break_start_time (ShellBreakManager *self)
{
  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), 0);

  if (self->state != SHELL_BREAK_STATE_IN_BREAK)
    return 0;

  return self->current_break_start_time;
}

/**
 * shell_break_manager_get_last_break_end_time:
 * @self: a #ShellBreakManager
 *
 * End time for the most recent break, or 0 if a break is currently in progress
 * or no breaks have happened yet.
 *
 * Returns: the end time in seconds since the Unix epoch
 */
gint64
shell_break_manager_get_last_break_end_time (ShellBreakManager *self)
{
  gint64 last_end = 0;
  guint i;

  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), 0);

  if (self->state == SHELL_BREAK_STATE_IN_BREAK)
    return 0;

  for (i = 0; i < self->break_types->len; i++)
    {
      BreakType *bt = g_ptr_array_index (self->break_types, i);

      last_end = MAX (last_end, bt->last_end);
    }

  return last_end;
}

/**
 * shell_break_manager_get_current_time:
 * @self: a #ShellBreakManager
 *
 * Returns: the current real time, in seconds since the Unix epoch
 */
gint64
shell_break_manager_get_current_time (ShellBreakManager *self)
{
  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), 0);

  return self->env.get_real_time_secs (self->env.user_data);
}

/**
 * shell_break_manager_get_next_break_due:
 * @self: a #ShellBreakManager
 * @from_time: `current` time to calculate from
 * @break_type: (out) (optional) (transfer none) (nullable): return location
 *   for the type of break which is currently due or which will be due next, or
 *   %NULL if no break types are enabled
 *
 * Gets the time when the next break is due and its break type. If a break is
 * currently due, the time is when that break was due.
 *
 * Returns: the time when the next break is due, or 0 if no break types are
 *   enabled
 */
gint64
shell_break_manager_get_next_break_due (ShellBreakManager  *self,
                                        gint64              from_time,
                                        const char        **break_type)
{
  guint max_duration = 0;
  const char *max_duration_type = NULL;
  gint64 next_due_time = 0;
  guint i;

  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), 0);

  g_debug ("BreakManager: Current time: %" G_GINT64_FORMAT "s", from_time);

  for (i = 0; i < self->break_types->len; i++)
    {
      BreakType *bt = g_ptr_array_index (self->break_types, i);
      guint interval = break_type_get_uint (self, bt, "interval-seconds");
      guint duration = break_type_get_uint (self, bt, "duration-seconds");
      gint64 would_be_due_at;

      /* Work out which break types are now due. */
      would_be_due_at = bt->last_end + interval;

      g_debug ("BreakManager: Break type %s: would be due at %" G_GINT64_FORMAT,
               bt->type, would_be_due_at);

      if (would_be_due_at <= from_time)
        {
          next_due_time = would_be_due_at;

          /* Of the break types which are now due, which has the longest
           * duration? */
          if (max_duration == 0 || duration > max_duration)
            {
              max_duration = duration;
              max_duration_type = bt->type;
            }
        }
      else if (next_due_time == 0 || next_due_time > would_be_due_at)
        {
          /* This break type is not due now, but when is it due? */
          next_due_time = would_be_due_at;
          max_duration_type = bt->type;
        }
    }

  g_debug ("BreakManager: max duration type: %s, next due time: %" G_GINT64_FORMAT,
           max_duration_type ? max_duration_type : "(null)", next_due_time);

  /* max_duration_type is NULL and next_due_time is zero if no break types are
   * enabled */
  if (break_type != NULL)
    *break_type = max_duration_type;

  return next_due_time;
}

/**
 * shell_break_manager_get_next_break_due_time:
 * @self: a #ShellBreakManager
 * @from_time: `current` time to calculate from
 *
 * Gets the time when the next break is due. If a break is currently due, then
 * this is the time when that break was due. This is zero if no break types are
 * enabled.
 *
 * Returns: the time in seconds since the Unix epoch
 */
gint64
shell_break_manager_get_next_break_due_time (ShellBreakManager *self,
                                             gint64             from_time)
{
  return shell_break_manager_get_next_break_due (self, from_time, NULL);
}

static void
delay_or_skip_break (ShellBreakManager *self,
                     gboolean           delay)
{
  gint64 current_time;
  guint i;

  if (self->state == SHELL_BREAK_STATE_DISABLED)
    return;

  current_time = shell_break_manager_get_current_time (self);

  g_debug ("BreakManager: %s current break", delay ? "Delaying" : "Skipping");

  /* Change the last break end times for any break types which are currently
   * due. We do this over all break types, rather than just the current break
   * type, because multiple break types might be due at the same time, and it
   * would be a bit annoying to delay or skip one only for another break type
   * to immediately become due. */
  for (i = 0; i < self->break_types->len; i++)
    {
      BreakType *bt = g_ptr_array_index (self->break_types, i);
      guint interval = break_type_get_uint (self, bt, "interval-seconds");

      if (bt->last_end + interval <= current_time)
        {
          if (delay)
            bt->last_end += break_type_get_uint (self, bt, "delay-seconds");
          else
            bt->last_end = current_time;
        }
    }

  g_object_freeze_notify (G_OBJECT (self));
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_NEXT_BREAK_DUE_TIME]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LAST_BREAK_END_TIME]);

  update_state (self, current_time);
  g_object_thaw_notify (G_OBJECT (self));
}

/**
 * shell_break_manager_delay_break:
 * @self: a #ShellBreakManager
 *
 * Delays the currently upcoming, due or in-progress break. This is a no-op if
 * no break is currently due or in progress.
 */
void
shell_break_manager_delay_break (ShellBreakManager *self)
{
  g_return_if_fail (SHELL_IS_BREAK_MANAGER (self));

  delay_or_skip_break (self, TRUE);
}

/**
 * shell_break_manager_skip_break:
 * @self: a #ShellBreakManager
 *
 * Skips the currently upcoming, due or in-progress break. This is a no-op if
 * no break is currently due or in progress.
 */
void
shell_break_manager_skip_break (ShellBreakManager *self)
{
  g_return_if_fail (SHELL_IS_BREAK_MANAGER (self));

  delay_or_skip_break (self, FALSE);
}

/**
 * shell_break_manager_take_break:
 * @self: a #ShellBreakManager
 *
 * Signals that the user explicitly wants to start taking a break now, even if
 * they are technically still active. We cannot force the user to be idle, but
 * we can indicate to the break dispatcher that it should try and make the user
 * be idle (#ShellBreakManager::take-break).
 */
void
shell_break_manager_take_break (ShellBreakManager *self)
{
  g_return_if_fail (SHELL_IS_BREAK_MANAGER (self));

  g_signal_emit (self, signals[TAKE_BREAK], 0);
}

/**
 * shell_break_manager_break_type_should_notify:
 * @self: a #ShellBreakManager
 * @break_type: (nullable): a break type
 *
 * Returns: whether the break type should emit notification popups to the user
 */
gboolean
shell_break_manager_break_type_should_notify (ShellBreakManager *self,
                                              const char        *break_type)
{
  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), FALSE);

  return break_type_get_boolean (self, break_type, "notify");
}

/**
 * shell_break_manager_break_type_should_notify_upcoming:
 * @self: a #ShellBreakManager
 * @break_type: (nullable): a break type
 *
 * Returns: whether the break type should emit notification popups to the user
 *   when it is upcoming
 */
gboolean
shell_break_manager_break_type_should_notify_upcoming (ShellBreakManager *self,
                                                       const char        *break_type)
{
  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), FALSE);

  return break_type_get_boolean (self, break_type, "notify-upcoming");
}

/**
 * shell_break_manager_break_type_should_notify_overdue:
 * @self: a #ShellBreakManager
 * @break_type: (nullable): a break type
 *
 * Returns: whether the break type should emit notification popups to the user
 *   if it is overdue
 */
gboolean
shell_break_manager_break_type_should_notify_overdue (ShellBreakManager *self,
                                                      const char        *break_type)
{
  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), FALSE);

  return break_type_get_boolean (self, break_type, "notify-overdue");
}

/**
 * shell_break_manager_break_type_should_countdown:
 * @self: a #ShellBreakManager
 * @break_type: (nullable): a break type
 *
 * Returns: whether the break type should show a prominent countdown for the
 *   last 60s before it is due
 */
gboolean
shell_break_manager_break_type_should_countdown (ShellBreakManager *self,
                                                 const char        *break_type)
{
  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), FALSE);

  return break_type_get_boolean (self, break_type, "countdown");
}

/**
 * shell_break_manager_break_type_should_play_sound:
 * @self: a #ShellBreakManager
 * @break_type: (nullable): a break type
 *
 * Returns: whether the break type should play sounds to notify the user
 */
gboolean
shell_break_manager_break_type_should_play_sound (ShellBreakManager *self,
                                                  const char        *break_type)
{
  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), FALSE);

  return break_type_get_boolean (self, break_type, "play-sound");
}

/**
 * shell_break_manager_break_type_should_fade_screen:
 * @self: a #ShellBreakManager
 * @break_type: (nullable): a break type
 *
 * Returns: whether the break type should fade the screen during breaks
 */
gboolean
shell_break_manager_break_type_should_fade_screen (ShellBreakManager *self,
                                                   const char        *break_type)
{
  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), FALSE);

  return break_type_get_boolean (self, break_type, "fade-screen");
}

/**
 * shell_break_manager_break_type_should_lock_screen:
 * @self: a #ShellBreakManager
 * @break_type: (nullable): a break type
 *
 * Returns: whether the break type should lock the screen during breaks
 */
gboolean
shell_break_manager_break_type_should_lock_screen (ShellBreakManager *self,
                                                   const char        *break_type)
{
  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), FALSE);

  return break_type_get_boolean (self, break_type, "lock-screen");
}

/**
 * shell_break_manager_get_duration_for_break_type:
 * @self: a #ShellBreakManager
 * @break_type: (nullable): a break type
 *
 * Returns: the duration in seconds of @break_type, or 0 if it is not enabled
 */
guint
shell_break_manager_get_duration_for_break_type (ShellBreakManager *self,
                                                 const char        *break_type)
{
  BreakType *bt;

  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (self), 0);

  bt = find_break_type (self, break_type);
  if (bt == NULL)
    return 0;

  return break_type_get_uint (self, bt, "duration-seconds");
}

/* ------------------------------------------------------------------------ */
/* BreakNotification                                                         */
/* ------------------------------------------------------------------------ */

#define BREAK_TYPE_NOTIFICATION (break_notification_get_type ())
G_DECLARE_FINAL_TYPE (BreakNotification, break_notification,
                      BREAK, NOTIFICATION, ShellNotification)

struct _BreakNotification
{
  ShellNotification parent;

  ShellBreakManager *manager;    /* weak */
  ShellApp *app;

  ShellNotificationAction *delay_action;
  ShellNotificationAction *skip_action;
  ShellNotificationAction *take_action;
};

G_DEFINE_FINAL_TYPE (BreakNotification, break_notification, SHELL_TYPE_NOTIFICATION)

static void
break_notification_activate (ShellNotification *notification)
{
  BreakNotification *self = BREAK_NOTIFICATION (notification);

  shell_app_activate (self->app);

  SHELL_NOTIFICATION_CLASS (break_notification_parent_class)->activate (notification);
}

static void
on_break_notification_destroy (ShellNotification                *notification,
                               ShellNotificationDestroyedReason  reason,
                               BreakNotification                *self)
{
  /* If it was destroyed by the user (by pressing the close button), skip the
   * current break. */
  if (reason == SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED && self->manager != NULL)
    shell_break_manager_skip_break (self->manager);

  g_clear_weak_pointer (&self->manager);
}

static void
break_notification_dispose (GObject *object)
{
  BreakNotification *self = BREAK_NOTIFICATION (object);

  g_clear_weak_pointer (&self->manager);
  g_clear_object (&self->app);

  G_OBJECT_CLASS (break_notification_parent_class)->dispose (object);
}

static void
break_notification_class_init (BreakNotificationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellNotificationClass *notification_class = SHELL_NOTIFICATION_CLASS (klass);

  object_class->dispose = break_notification_dispose;
  notification_class->activate = break_notification_activate;
}

static void
break_notification_init (BreakNotification *self)
{
  g_signal_connect (self, "destroy", G_CALLBACK (on_break_notification_destroy), self);
}

static BreakNotification *
break_notification_new (ShellSource       *source,
                        ShellBreakManager *manager,
                        ShellApp          *app)
{
  BreakNotification *self;

  self = g_object_new (BREAK_TYPE_NOTIFICATION,
                       "source", source,
                       "resident", TRUE,
                       NULL);
  g_set_weak_pointer (&self->manager, manager);
  self->app = g_object_ref (app);

  return self;
}

static void
on_delay_action (ShellNotification *notification,
                 gpointer           user_data)
{
  BreakNotification *self = BREAK_NOTIFICATION (notification);

  if (self->manager != NULL)
    shell_break_manager_delay_break (self->manager);
  shell_notification_destroy (notification, SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED);
}

static void
on_skip_action (ShellNotification *notification,
                gpointer           user_data)
{
  BreakNotification *self = BREAK_NOTIFICATION (notification);

  if (self->manager != NULL)
    shell_break_manager_skip_break (self->manager);
  shell_notification_destroy (notification, SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED);
}

static void
on_take_action (ShellNotification *notification,
                gpointer           user_data)
{
  BreakNotification *self = BREAK_NOTIFICATION (notification);

  if (self->manager != NULL)
    shell_break_manager_take_break (self->manager);
  shell_notification_destroy (notification, SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED);
}

static void
break_notification_set_allowed (BreakNotification *self,
                                gboolean           allow_delay,
                                gboolean           allow_skip,
                                gboolean           allow_take)
{
  ShellNotification *notification = SHELL_NOTIFICATION (self);

  if (allow_delay != (self->delay_action != NULL))
    {
      if (allow_delay)
        /* Translators: As in "Delay the break" */
        self->delay_action = shell_notification_add_action (notification, _("Delay"),
                                                            on_delay_action, NULL, NULL);
      else
        {
          shell_notification_remove_action (notification, self->delay_action);
          self->delay_action = NULL;
        }
    }

  if (allow_skip != (self->skip_action != NULL))
    {
      if (allow_skip)
        /* Translators: As in "Skip the break" */
        self->skip_action = shell_notification_add_action (notification, _("Skip"),
                                                           on_skip_action, NULL, NULL);
      else
        {
          shell_notification_remove_action (notification, self->skip_action);
          self->skip_action = NULL;
        }
    }

  if (allow_take != (self->take_action != NULL))
    {
      if (allow_take)
        /* Translators: As in "Take the break" */
        self->take_action = shell_notification_add_action (notification, _("Take"),
                                                           on_take_action, NULL, NULL);
      else
        {
          shell_notification_remove_action (notification, self->take_action);
          self->take_action = NULL;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* BreakNotificationSource                                                   */
/* ------------------------------------------------------------------------ */

/* This cannot directly extend ShellSource (even though it conceptually does)
 * because the shell will automatically destroy a ShellSource when it becomes
 * empty, and that destroys the state which we need for correctly showing the
 * next notification (e.g. on a timer).
 *
 * So instead this is a normal GObject which closely wraps a ShellSource. */

#define BREAK_TYPE_NOTIFICATION_SOURCE (break_notification_source_get_type ())
G_DECLARE_FINAL_TYPE (BreakNotificationSource, break_notification_source,
                      BREAK, NOTIFICATION_SOURCE, GObject)

struct _BreakNotificationSource
{
  GObject parent;

  ShellApp *app;
  ShellSource *source;
  BreakNotification *notification;
  ShellMessageTray *message_tray;    /* weak */
  ShellBreakManager *manager;        /* weak */
  guint timer_id;

  ShellBreakState previous_state;
  gboolean destroyed;
};

G_DEFINE_FINAL_TYPE (BreakNotificationSource, break_notification_source, G_TYPE_OBJECT)

/**
 * format_time_span:
 * @seconds: the time span
 * @value: (out): the number which determines whether the string is plural, for
 *   use with ngettext () calls to embed the returned string into
 * @update_timeout: (out): the number of seconds until the string would change
 *   if re-generated, for use in setting a timer to update a notification
 *
 * Formats the given time span as a translated string including units.
 *
 * Returns: (transfer full): the translated string
 */
static char *
format_time_span (gint64  seconds,
                  gint64 *value,
                  gint64 *update_timeout)
{
  gint64 minutes = seconds / 60;
  gint64 hours = minutes / 60;
  gint64 days = hours / 24;

  if (seconds < 60)
    {
      *value = seconds;
      *update_timeout = 1;
      return g_strdup_printf (ngettext ("%d second", "%d seconds", seconds), (int) seconds);
    }
  else if (minutes < 60)
    {
      *value = minutes;
      *update_timeout = 60 - (seconds % 60);
      return g_strdup_printf (ngettext ("%d minute", "%d minutes", minutes), (int) minutes);
    }
  else if (hours < 24)
    {
      *value = hours;
      *update_timeout = 3600 - (seconds % 3600);
      return g_strdup_printf (ngettext ("%d hour", "%d hours", hours), (int) hours);
    }
  else
    {
      *value = days;
      *update_timeout = 86400 - (seconds % 86400);
      return g_strdup_printf (ngettext ("%d day", "%d days", days), (int) days);
    }
}

static void
notification_source_update_state (BreakNotificationSource *self);

static void
on_notification_source_notification_destroy (ShellNotification                *notification,
                                             ShellNotificationDestroyedReason  reason,
                                             BreakNotificationSource          *self)
{
  g_signal_handlers_disconnect_by_data (notification, self);
  g_clear_object (&self->notification);
}

static void
on_notification_source_source_destroy (ShellSource                      *source,
                                       ShellNotificationDestroyedReason  reason,
                                       BreakNotificationSource          *self)
{
  g_signal_handlers_disconnect_by_data (source, self);
  g_clear_object (&self->source);
}

static gboolean
notification_source_ensure_notification (BreakNotificationSource *self,
                                         const char              *title,
                                         const char              *body,
                                         ShellNotificationUrgency urgency,
                                         gboolean                 allow_delay,
                                         gboolean                 allow_skip,
                                         gboolean                 allow_take)
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
      g_signal_connect (self->source, "destroy",
                        G_CALLBACK (on_notification_source_source_destroy), self);
      shell_message_tray_add (self->message_tray, self->source);
    }

  if (self->notification == NULL)
    {
      self->notification = break_notification_new (self->source, self->manager, self->app);
      g_signal_connect (self->notification, "destroy",
                        G_CALLBACK (on_notification_source_notification_destroy), self);
    }

  notification = SHELL_NOTIFICATION (self->notification);

  /* Unacknowledge the notification when it is updated, by default. */
  shell_notification_set_acknowledged (notification, FALSE);
  shell_notification_set_title (notification, title);
  shell_notification_set_body (notification, body);
  shell_notification_set_sound (notification, NULL);
  shell_notification_set_urgency (notification, urgency);
  break_notification_set_allowed (self->notification, allow_delay, allow_skip, allow_take);

  shell_source_add_notification (self->source, notification);

  return TRUE;
}

static void
notification_source_detach (BreakNotificationSource *self)
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
notification_source_destroy (BreakNotificationSource *self)
{
  g_autoptr (BreakNotification) notification = NULL;
  g_autoptr (ShellSource) source = NULL;

  if (self->destroyed)
    return;

  notification = self->notification != NULL ? g_object_ref (self->notification) : NULL;
  source = self->source != NULL ? g_object_ref (self->source) : NULL;

  notification_source_detach (self);

  if (notification != NULL)
    shell_notification_destroy (SHELL_NOTIFICATION (notification),
                                SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
  if (source != NULL)
    shell_source_destroy (source, SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
}

static void
on_notification_source_timeout (gpointer user_data)
{
  BreakNotificationSource *self = user_data;

  self->timer_id = 0;
  g_debug ("BreakNotificationSource: Scheduled notification state update");
  notification_source_update_state (self);
}

static void
notification_source_schedule_update_state (BreakNotificationSource *self,
                                           gint64                   timeout)
{
  g_clear_handle_id (&self->timer_id, g_source_remove);

  /* Never spin */
  if (timeout < 1)
    timeout = 1;

  g_debug ("BreakNotificationSource: Scheduling notification state update in %" G_GINT64_FORMAT "s",
           timeout);

  self->timer_id = g_timeout_add_seconds_once ((guint) timeout,
                                               on_notification_source_timeout,
                                               self);
}

static ShellNotificationUrgency
urgency_for_break_type (BreakNotificationSource *self,
                        const char              *break_type)
{
  /* While the preferences indicate that certain break types should notify
   * and others should not, that is not really possible: we only have one
   * notification which gets constantly updated with information about
   * breaks, interleaving messages from multiple breaks over time. Having
   * it disappear and reappear for different break types would be jarring.
   * Instead, set the urgency to LOW for break types which should not
   * notify. This means the notification remains, but is not presented to
   * the user: it is just visible if they expand the message tray. */
  if (shell_break_manager_break_type_should_notify (self->manager, break_type))
    return SHELL_NOTIFICATION_URGENCY_HIGH;
  else
    return SHELL_NOTIFICATION_URGENCY_LOW;
}

static void
notification_source_update_state (BreakNotificationSource *self)
{
  static const gint64 upcoming_notification_times[] = { BREAK_UPCOMING_NOTIFICATION_TIME_SECONDS };
  ShellBreakManager *manager = self->manager;
  ShellBreakState state;
  gint64 current_time;

  if (self->destroyed || manager == NULL)
    return;

  current_time = shell_break_manager_get_current_time (manager);
  state = shell_break_manager_get_state (manager);

  g_debug ("BreakNotificationSource: Break notification source state changed from %s to %s",
           shell_break_state_to_string (self->previous_state),
           shell_break_state_to_string (state));

  switch (state)
    {
    case SHELL_BREAK_STATE_DISABLED:
      notification_source_destroy (self);
      break;

    case SHELL_BREAK_STATE_ACTIVE:
      if (self->previous_state == SHELL_BREAK_STATE_IN_BREAK)
        {
          /* Break is complete. */
          if (self->notification != NULL)
            shell_notification_destroy (SHELL_NOTIFICATION (self->notification),
                                        SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED);
        }
      else
        {
          const char *next_break_type = NULL;
          gint64 next_break_due_time;
          gint64 remaining_secs;
          guint i;

          /* Work out when the next break is due, and display some warnings
           * that it is impending. */
          next_break_due_time = shell_break_manager_get_next_break_due (manager, current_time,
                                                                        &next_break_type);
          remaining_secs = next_break_due_time - current_time;
          g_debug ("BreakNotificationSource: %" G_GINT64_FORMAT "s left before next break",
                   remaining_secs);

          if (shell_break_manager_break_type_should_notify_upcoming (manager, next_break_type))
            {
              for (i = 0; i < G_N_ELEMENTS (upcoming_notification_times); i++)
                {
                  gint64 notification_time = upcoming_notification_times[i];

                  g_debug ("BreakNotificationSource: Considering upcoming notification %" G_GINT64_FORMAT "s",
                           notification_time);

                  if (remaining_secs > notification_time)
                    {
                      /* Schedule to show the first (longest) notification
                       * which is less than the remaining time before the break. */
                      notification_source_schedule_update_state (self,
                                                                 remaining_secs - notification_time);
                      break;
                    }
                  else if (remaining_secs == notification_time)
                    {
                      g_autofree char *remaining_text = NULL;
                      g_autofree char *body_text = NULL;
                      const char *title_text;
                      gint64 remaining_value, unused;

                      /* Bang on time to show this notification. */
                      remaining_text = format_time_span (remaining_secs, &remaining_value, &unused);
                      body_text = g_strdup_printf (ngettext (
                          /* Translators: %s will be replaced with a string that describes a time interval, such as “2 minutes”, “40 seconds” or “1 hour” */
                          "It will be time for a break in %s",
                          "It will be time for a break in %s",
                          remaining_value), remaining_text);

                      if (g_strcmp0 (next_break_type, "movement") == 0)
                        title_text = _("Movement Break Soon");
                      else if (g_strcmp0 (next_break_type, "eyesight") == 0)
                        title_text = _("Eyesight Break Soon");
                      else
                        title_text = _("Break Soon");

                      notification_source_ensure_notification (self, title_text, body_text,
                                                               urgency_for_break_type (self, next_break_type),
                                                               TRUE, TRUE, FALSE);

                      /* Continue to the next iteration, to schedule an update
                       * for the next-biggest notification. */
                    }
                }
            }
        }
      break;

    case SHELL_BREAK_STATE_IDLE:
      /* Do nothing. */
      break;

    case SHELL_BREAK_STATE_IN_BREAK:
      {
        const char *break_type = shell_break_manager_get_current_break_type (manager);
        gint64 break_duration_remaining;

        /* Clamp to a minimum of 1s because saying “you have 0 seconds left”
         * is a bit odd. The state machine may remain in
         * `SHELL_BREAK_STATE_IN_BREAK` even after the break duration is over,
         * because the user might remain idle. In that case, display no
         * further notifications until the user becomes active again — at
         * that point they get a ‘Break is over’ notification. */
        break_duration_remaining =
          (gint64) shell_break_manager_get_duration_for_break_type (manager, break_type) -
          (current_time - shell_break_manager_get_current_break_start_time (manager));

        if (break_duration_remaining >= 1)
          {
            g_autofree char *remaining_text = NULL;
            g_autofree char *body_text = NULL;
            const char *title_text;
            gint64 remaining_value, update_timeout_seconds;

            remaining_text = format_time_span (break_duration_remaining, &remaining_value,
                                               &update_timeout_seconds);

            if (g_strcmp0 (break_type, "movement") == 0)
              {
                title_text = _("Movement Break in Progress");
                body_text = g_strdup_printf (ngettext (
                    /* Translators: %s will be replaced with a string that describes a time interval, such as “2 minutes”, “40 seconds” or “1 hour” */
                    "Continue moving around for %s",
                    "Continue moving around for %s",
                    remaining_value), remaining_text);
              }
            else if (g_strcmp0 (break_type, "eyesight") == 0)
              {
                title_text = _("Eyesight Break in Progress");
                body_text = g_strdup_printf (ngettext (
                    /* Translators: %s will be replaced with a string that describes a time interval, such as “2 minutes”, “40 seconds” or “1 hour” */
                    "Continue looking away for %s",
                    "Continue looking away for %s",
                    remaining_value), remaining_text);
              }
            else
              {
                title_text = _("Break in Progress");
                body_text = g_strdup_printf (ngettext (
                    /* Translators: %s will be replaced with a string that describes a time interval, such as “2 minutes”, “40 seconds” or “1 hour” */
                    "%s left in your break",
                    "%s left in your break",
                    remaining_value), remaining_text);
              }

            notification_source_ensure_notification (self, title_text, body_text,
                                                     urgency_for_break_type (self, break_type),
                                                     FALSE, FALSE, FALSE);

            notification_source_schedule_update_state (self, update_timeout_seconds);
          }
        else if (self->notification != NULL)
          {
            shell_notification_destroy (SHELL_NOTIFICATION (self->notification),
                                        SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED);
          }
      }
      break;

    case SHELL_BREAK_STATE_BREAK_DUE:
      {
        const char *next_break_type = NULL;
        gint64 next_break_due_time;
        gint64 break_due_ago;

        next_break_due_time = shell_break_manager_get_next_break_due (manager, current_time,
                                                                      &next_break_type);
        break_due_ago = current_time - next_break_due_time;
        if (break_due_ago < 0)
          g_debug ("BreakNotificationSource: breakDueAgo should be non-negative");

        if ((self->previous_state == SHELL_BREAK_STATE_ACTIVE ||
             self->previous_state == SHELL_BREAK_STATE_DISABLED) &&
            break_due_ago < BREAK_OVERDUE_TIME_SECONDS)
          {
            g_autofree char *duration_text = NULL;
            g_autofree char *body_text = NULL;
            const char *title_text;
            gint64 duration_value, unused;
            gint64 duration_secs = shell_break_manager_get_duration_for_break_type (manager,
                                                                                    next_break_type);

            duration_text = format_time_span (duration_secs, &duration_value, &unused);

            if (g_strcmp0 (next_break_type, "movement") == 0)
              {
                title_text = _("Time for a Movement Break");
                body_text = g_strdup_printf (ngettext (
                    /* Translators: %s will be replaced with a string that describes a time interval, such as “2 minutes”, “40 seconds” or “1 hour” */
                    "Take a break from the device and move around for %s",
                    "Take a break from the device and move around for %s",
                    duration_value), duration_text);
              }
            else if (g_strcmp0 (next_break_type, "eyesight") == 0)
              {
                title_text = _("Time for an Eyesight Break");
                body_text = g_strdup_printf (ngettext (
                    /* Translators: %s will be replaced with a string that describes a time interval, such as “2 minutes”, “40 seconds” or “1 hour” */
                    "Take a break from the screen. Look at least 6 meters away for at least %s",
                    "Take a break from the screen. Look at least 6 meters away for at least %s",
                    duration_value), duration_text);
              }
            else
              {
                title_text = _("Time for a Break");
                body_text = g_strdup_printf (ngettext (
                    /* Translators: %s will be replaced with a string that describes a time interval, such as “2 minutes”, “40 seconds” or “1 hour” */
                    "It’s time to take a break. Get away from the device for %s!",
                    "It’s time to take a break. Get away from the device for %s!",
                    duration_value), duration_text);
              }

            notification_source_ensure_notification (self, title_text, body_text,
                                                     urgency_for_break_type (self, next_break_type),
                                                     TRUE, FALSE, TRUE);

            notification_source_schedule_update_state (self, BREAK_OVERDUE_TIME_SECONDS);
          }
        else if (break_due_ago >= BREAK_OVERDUE_TIME_SECONDS &&
                 shell_break_manager_break_type_should_notify_overdue (manager, next_break_type))
          {
            g_autofree char *delay_text = NULL;
            g_autofree char *body_text = NULL;
            gint64 delay_value, update_timeout_seconds;

            delay_text = format_time_span (break_due_ago, &delay_value, &update_timeout_seconds);
            body_text = g_strdup_printf (ngettext (
                /* Translators: %s will be replaced with a string that describes a time interval, such as “2 minutes”, “40 seconds” or “1 hour” */
                "You were due to take a break %s ago",
                "You were due to take a break %s ago",
                delay_value), delay_text);

            notification_source_ensure_notification (self, _("Break Overdue"), body_text,
                                                     urgency_for_break_type (self, next_break_type),
                                                     TRUE, FALSE, TRUE);

            notification_source_schedule_update_state (self, update_timeout_seconds);
          }
        else if (self->previous_state == SHELL_BREAK_STATE_IN_BREAK)
          {
            g_autofree char *countdown_text = NULL;
            g_autofree char *body_text = NULL;
            gint64 countdown_value, update_timeout_seconds;
            gint64 duration_secs = shell_break_manager_get_duration_for_break_type (manager,
                                                                                    next_break_type);

            countdown_text = format_time_span (duration_secs - break_due_ago, &countdown_value,
                                               &update_timeout_seconds);
            body_text = g_strdup_printf (ngettext (
                /* Translators: %s will be replaced with a string that describes a time interval, such as “2 minutes”, “40 seconds” or “1 hour” */
                "There is %s remaining in your break",
                "There are %s remaining in your break",
                countdown_value), countdown_text);

            notification_source_ensure_notification (self, _("Break Interrupted"), body_text,
                                                     urgency_for_break_type (self, next_break_type),
                                                     FALSE, FALSE, FALSE);

            notification_source_schedule_update_state (self, update_timeout_seconds);
          }
      }
      break;

    default:
      g_debug ("Unknown BreakManager state: %d", state);
      break;
    }
}

static void
on_notification_source_state_changed (BreakNotificationSource *self)
{
  if (self->destroyed || self->manager == NULL)
    return;

  notification_source_update_state (self);

  if (self->destroyed || self->manager == NULL)
    return;

  self->previous_state = shell_break_manager_get_state (self->manager);
}

static void
break_notification_source_dispose (GObject *object)
{
  BreakNotificationSource *self = BREAK_NOTIFICATION_SOURCE (object);

  notification_source_detach (self);
  g_clear_weak_pointer (&self->message_tray);
  g_clear_object (&self->app);

  G_OBJECT_CLASS (break_notification_source_parent_class)->dispose (object);
}

static void
break_notification_source_class_init (BreakNotificationSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = break_notification_source_dispose;
}

static void
break_notification_source_init (BreakNotificationSource *self)
{
  self->previous_state = SHELL_BREAK_STATE_DISABLED;
}

static BreakNotificationSource *
break_notification_source_new (ShellBreakManager *manager,
                               ShellMessageTray  *message_tray)
{
  BreakNotificationSource *self = g_object_new (BREAK_TYPE_NOTIFICATION_SOURCE, NULL);
  ShellApp *app;

  app = shell_app_system_lookup_app (shell_app_system_get_default (), WELLBEING_DESKTOP_ID);
  if (app != NULL)
    self->app = g_object_ref (app);
  else
    g_message ("%s not found, the break notifications are not shown", WELLBEING_DESKTOP_ID);

  g_set_weak_pointer (&self->message_tray, message_tray);
  g_set_weak_pointer (&self->manager, manager);
  g_signal_connect_swapped (manager, "notify::state",
                            G_CALLBACK (on_notification_source_state_changed), self);
  g_signal_connect_swapped (manager, "notify::next-break-due-time",
                            G_CALLBACK (on_notification_source_state_changed), self);

  notification_source_update_state (self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* BreakCountdownLabel                                                       */
/* ------------------------------------------------------------------------ */

#define BREAK_TYPE_COUNTDOWN_LABEL (break_countdown_label_get_type ())
G_DECLARE_FINAL_TYPE (BreakCountdownLabel, break_countdown_label,
                      BREAK, COUNTDOWN_LABEL, StWidget)

struct _BreakCountdownLabel
{
  StWidget parent;

  ShellBreakManager *manager;   /* weak */
  MetaCompositor *compositor;   /* weak, unredirect is disabled while set */
  ClutterActor *box;
  StWidget *label;
  guint timer_id;
};

G_DEFINE_FINAL_TYPE (BreakCountdownLabel, break_countdown_label, ST_TYPE_WIDGET)

/* Returns FALSE if the countdown is over */
static gboolean
countdown_label_update_state (BreakCountdownLabel *self)
{
  gint64 current_time;
  gint64 next_due_time;
  gint64 break_due_in_secs;
  g_autofree char *text = NULL;

  if (self->manager == NULL)
    return FALSE;

  current_time = shell_break_manager_get_current_time (self->manager);
  next_due_time = shell_break_manager_get_next_break_due_time (self->manager, current_time);
  break_due_in_secs = next_due_time - current_time;
  g_debug ("OsdBreakCountdownLabel: breakDueInSecs %" G_GINT64_FORMAT " nextDueTime %" G_GINT64_FORMAT " currentTime %" G_GINT64_FORMAT,
           break_due_in_secs, next_due_time, current_time);

  if (break_due_in_secs < 1)
    return FALSE;

  text = g_strdup_printf (ngettext (
      /* Translators: This is a notification to warn the user that a
       * screen time break will start shortly. */
      "Break in %d second",
      "Break in %d seconds",
      break_due_in_secs), (int) break_due_in_secs);
  st_label_set_text (ST_LABEL (self->label), text);

  return TRUE;
}

static gboolean
on_countdown_timeout (gpointer user_data)
{
  BreakCountdownLabel *self = user_data;

  if (countdown_label_update_state (self))
    return G_SOURCE_CONTINUE;

  self->timer_id = 0;
  clutter_actor_destroy (CLUTTER_ACTOR (self));

  return G_SOURCE_REMOVE;
}

static void
break_countdown_label_dispose (GObject *object)
{
  BreakCountdownLabel *self = BREAK_COUNTDOWN_LABEL (object);

  g_clear_handle_id (&self->timer_id, g_source_remove);

  if (self->compositor != NULL)
    {
      meta_compositor_enable_unredirect (self->compositor);
      g_clear_weak_pointer (&self->compositor);
    }

  g_clear_weak_pointer (&self->manager);

  G_OBJECT_CLASS (break_countdown_label_parent_class)->dispose (object);
}

static void
break_countdown_label_class_init (BreakCountdownLabelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = break_countdown_label_dispose;
}

static void
break_countdown_label_init (BreakCountdownLabel *self)
{
}

static void
countdown_label_position (BreakCountdownLabel *self,
                          ShellLayoutManager  *layout_manager)
{
  MtkRectangle work_area;

  shell_layout_manager_get_work_area_for_monitor (layout_manager,
                                                  shell_layout_manager_get_primary_index (layout_manager),
                                                  &work_area);

  if (clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL)
    clutter_actor_set_x (self->box,
                         work_area.x + (work_area.width - clutter_actor_get_width (self->box)));
  else
    clutter_actor_set_x (self->box, work_area.x);

  clutter_actor_set_y (self->box, work_area.y);
}

/* Returns NULL if the break is due in less than a second */
static BreakCountdownLabel *
break_countdown_label_new (ShellBreakManager  *manager,
                           ShellLayoutManager *layout_manager,
                           ClutterActor       *ui_group)
{
  BreakCountdownLabel *self;

  self = g_object_new (BREAK_TYPE_COUNTDOWN_LABEL,
                       "x-expand", TRUE,
                       "y-expand", TRUE,
                       NULL);
  g_set_weak_pointer (&self->manager, manager);

  self->timer_id = g_timeout_add_seconds (1, on_countdown_timeout, self);

  self->box = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "orientation", CLUTTER_ORIENTATION_VERTICAL,
                            NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->box);

  self->label = g_object_new (ST_TYPE_LABEL,
                              "style-class", "osd-break-countdown-label",
                              "text", "",
                              NULL);
  clutter_actor_add_child (self->box, CLUTTER_ACTOR (self->label));

  clutter_actor_add_child (ui_group, CLUTTER_ACTOR (self));
  clutter_actor_set_child_above_sibling (ui_group, CLUTTER_ACTOR (self), NULL);

  if (!countdown_label_update_state (self))
    {
      clutter_actor_destroy (CLUTTER_ACTOR (self));
      return NULL;
    }

  countdown_label_position (self, layout_manager);

  g_set_weak_pointer (&self->compositor,
                      shell_global_get_compositor (shell_global_get ()));
  meta_compositor_disable_unredirect (self->compositor);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellBreakDispatcher                                                      */
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

struct _ShellBreakDispatcher
{
  GObject parent;

  ShellBreakManager *manager;
  ShellLayoutManager *layout_manager;   /* weak */
  ShellMessageTray *message_tray;       /* weak */
  ShellSystemActions *system_actions;   /* unowned singleton */

  ShellBreakState previous_state;
  const char *previous_break_type;

  BreakNotificationSource *notification_source;
  ClutterEffect *brightness_effect;
  ClutterActor *effect_actor;           /* weak, where brightness_effect is */
  gulong effect_actor_destroy_id;
  ClutterActor *countdown_osd;          /* weak */
  guint countdown_timer_id;

  gulong manager_handler_ids[3];
  gboolean destroyed;
};

G_DEFINE_FINAL_TYPE (ShellBreakDispatcher, shell_break_dispatcher, G_TYPE_OBJECT)

static ClutterActor *
dispatcher_get_ui_group (ShellBreakDispatcher *self)
{
  if (self->layout_manager == NULL)
    return NULL;

  return CLUTTER_ACTOR (shell_layout_manager_get_ui_group (self->layout_manager));
}

static void
dispatcher_drop_effect (ShellBreakDispatcher *self,
                        gboolean              remove)
{
  if (self->effect_actor != NULL)
    {
      g_clear_signal_handler (&self->effect_actor_destroy_id, self->effect_actor);

      if (remove && self->brightness_effect != NULL &&
          clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->brightness_effect)) == self->effect_actor)
        clutter_actor_remove_effect (self->effect_actor, self->brightness_effect);

      g_clear_weak_pointer (&self->effect_actor);
    }

  self->effect_actor_destroy_id = 0;
  g_clear_object (&self->brightness_effect);
}

static void
on_ui_group_destroy (ClutterActor         *actor,
                     ShellBreakDispatcher *self)
{
  dispatcher_drop_effect (self, FALSE);
}

static void
dispatcher_ensure_enabled (ShellBreakDispatcher *self)
{
  ClutterActor *ui_group;

  if (self->notification_source != NULL && self->notification_source->destroyed)
    g_clear_object (&self->notification_source);

  if (self->notification_source == NULL && self->message_tray != NULL)
    self->notification_source = break_notification_source_new (self->manager,
                                                               self->message_tray);

  ui_group = dispatcher_get_ui_group (self);
  if (self->brightness_effect == NULL && ui_group != NULL)
    {
      self->brightness_effect = g_object_ref_sink (clutter_brightness_contrast_effect_new ());
      clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->brightness_effect), FALSE);
      clutter_actor_add_effect_with_name (ui_group, "brightness", self->brightness_effect);

      g_set_weak_pointer (&self->effect_actor, ui_group);
      self->effect_actor_destroy_id =
        g_signal_connect (ui_group, "destroy", G_CALLBACK (on_ui_group_destroy), self);
    }
}

static void
dispatcher_remove_countdown (ShellBreakDispatcher *self)
{
  g_clear_handle_id (&self->countdown_timer_id, g_source_remove);

  if (self->countdown_osd != NULL)
    {
      ClutterActor *osd = self->countdown_osd;

      g_clear_weak_pointer (&self->countdown_osd);
      clutter_actor_destroy (osd);
    }
}

static void
dispatcher_ensure_disabled (ShellBreakDispatcher *self)
{
  if (self->notification_source != NULL)
    {
      notification_source_destroy (self->notification_source);
      g_clear_object (&self->notification_source);
    }

  dispatcher_drop_effect (self, TRUE);

  dispatcher_remove_countdown (self);

  self->previous_state = SHELL_BREAK_STATE_DISABLED;
  self->previous_break_type = NULL;
}

static void
dispatcher_maybe_play_complete_sound (ShellBreakDispatcher *self)
{
  if (shell_break_manager_break_type_should_play_sound (self->manager, self->previous_break_type))
    {
      ShellGlobal *global = shell_global_get ();
      MetaSoundPlayer *player = meta_display_get_sound_player (shell_global_get_display (global));
      g_autofree char *path = g_build_filename (shell_global_get_datadir (global),
                                                "sounds", "complete.oga", NULL);
      g_autoptr (GFile) file = g_file_new_for_path (path);

      meta_sound_player_play_from_file (player, file, _("Break complete sound"), NULL);
    }
}

static void
on_countdown_start (gpointer user_data)
{
  ShellBreakDispatcher *self = user_data;
  ClutterActor *ui_group = dispatcher_get_ui_group (self);

  self->countdown_timer_id = 0;

  if (self->countdown_osd == NULL && ui_group != NULL && self->layout_manager != NULL)
    {
      BreakCountdownLabel *osd = break_countdown_label_new (self->manager,
                                                            self->layout_manager,
                                                            ui_group);

      if (osd != NULL)
        g_set_weak_pointer (&self->countdown_osd, CLUTTER_ACTOR (osd));
    }
}

static void
dispatcher_brightness_effect_on (ShellBreakDispatcher *self)
{
  /* The effect value is in the range [0, 255] with 127 as the `no change`
   * mid-point */
  const guint8 start_val = 127;
  const guint8 finish_val = (guint8) (127 * BRIGHTNESS_FACTOR);
  CoglColor start_color = COGL_COLOR_INIT (start_val, start_val, start_val, 255);
  CoglColor finish_color = COGL_COLOR_INIT (finish_val, finish_val, finish_val, 255);
  g_auto (GValue) start = G_VALUE_INIT;
  g_auto (GValue) target = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;
  ShellEaseParams params;
  ClutterActor *ui_group = dispatcher_get_ui_group (self);

  if (self->brightness_effect == NULL || ui_group == NULL)
    return;

  g_value_init (&start, COGL_TYPE_COLOR);
  g_value_set_boxed (&start, &start_color);
  g_object_set_property (G_OBJECT (self->brightness_effect), "brightness", &start);
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->brightness_effect), TRUE);

  g_value_init (&target, COGL_TYPE_COLOR);
  g_value_set_boxed (&target, &finish_color);

  params.duration = BRIGHTNESS_FADE_TIME_SECONDS * 1000;
  params.delay = -1;
  params.mode = CLUTTER_EASE_OUT_QUAD;
  params.repeat_count = 0;
  params.auto_reverse = FALSE;
  params.animation_required = FALSE;

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (ui_group),
                                       "@effects.brightness.brightness",
                                       &target, &params, NULL, NULL, NULL, &error))
    g_debug ("%s: %s", G_STRFUNC, error->message);
}

static void
dispatcher_activate_lock_screen (ShellBreakDispatcher *self)
{
  g_autoptr (GError) error = NULL;

  if (!shell_system_actions_activate_lock_screen (self->system_actions, &error))
    g_message ("Failed to lock the screen for a break: %s", error->message);
}

static void
dispatcher_set_effect_enabled (ShellBreakDispatcher *self,
                               gboolean              enabled)
{
  if (self->brightness_effect != NULL)
    clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->brightness_effect), enabled);
}

static void
on_dispatcher_state_changed (ShellBreakDispatcher *self)
{
  ShellBreakState state;

  if (self->destroyed)
    return;

  state = shell_break_manager_get_state (self->manager);

  switch (state)
    {
    case SHELL_BREAK_STATE_DISABLED:
      dispatcher_ensure_disabled (self);
      break;

    case SHELL_BREAK_STATE_ACTIVE:
      {
        const char *next_break_type = NULL;
        gint64 current_time, next_break_due_time;

        dispatcher_ensure_enabled (self);

        if (self->previous_state == SHELL_BREAK_STATE_IN_BREAK)
          dispatcher_maybe_play_complete_sound (self);

        dispatcher_set_effect_enabled (self, FALSE);

        /* Work out when the next break is due, and schedule a countdown. */
        current_time = shell_break_manager_get_current_time (self->manager);
        next_break_due_time = shell_break_manager_get_next_break_due (self->manager,
                                                                      current_time,
                                                                      &next_break_type);

        if (shell_break_manager_break_type_should_countdown (self->manager, next_break_type))
          {
            gint64 due_in_seconds = next_break_due_time - current_time;
            gint64 countdown_start = MAX (due_in_seconds - BREAK_COUNTDOWN_TIME_SECONDS, 0);

            g_debug ("BreakDispatcher: Scheduling break countdown to start in %" G_GINT64_FORMAT "s",
                     countdown_start);

            g_clear_handle_id (&self->countdown_timer_id, g_source_remove);
            self->countdown_timer_id = g_timeout_add_seconds_once ((guint) countdown_start,
                                                                   on_countdown_start,
                                                                   self);
          }
      }
      break;

    case SHELL_BREAK_STATE_IDLE:
      dispatcher_ensure_enabled (self);

      if (self->previous_state == SHELL_BREAK_STATE_IN_BREAK)
        dispatcher_maybe_play_complete_sound (self);

      dispatcher_set_effect_enabled (self, FALSE);
      break;

    case SHELL_BREAK_STATE_IN_BREAK:
      {
        const char *break_type = shell_break_manager_get_current_break_type (self->manager);

        dispatcher_ensure_enabled (self);

        if (shell_break_manager_break_type_should_lock_screen (self->manager, break_type) &&
            self->previous_state != SHELL_BREAK_STATE_IN_BREAK)
          dispatcher_activate_lock_screen (self);
        else if (shell_break_manager_break_type_should_fade_screen (self->manager, break_type))
          dispatcher_brightness_effect_on (self);

        dispatcher_remove_countdown (self);
      }
      break;

    case SHELL_BREAK_STATE_BREAK_DUE:
      dispatcher_ensure_enabled (self);
      dispatcher_remove_countdown (self);
      break;

    default:
      g_debug ("Unknown BreakManager state: %d", state);
      break;
    }

  self->previous_state = shell_break_manager_get_state (self->manager);
  self->previous_break_type = shell_break_manager_get_current_break_type (self->manager);
}

static void
on_dispatcher_take_break (ShellBreakDispatcher *self)
{
  const char *break_type;

  if (self->destroyed)
    return;

  break_type = shell_break_manager_get_current_break_type (self->manager);

  if (shell_break_manager_break_type_should_lock_screen (self->manager, break_type))
    dispatcher_activate_lock_screen (self);
  else if (shell_break_manager_break_type_should_fade_screen (self->manager, break_type))
    dispatcher_brightness_effect_on (self);
}

/* Stops reacting to the manager and drops everything without touching the
 * actors and the message tray, which may already be destroyed */
static void
dispatcher_detach (ShellBreakDispatcher *self)
{
  guint i;

  self->destroyed = TRUE;

  for (i = 0; i < G_N_ELEMENTS (self->manager_handler_ids); i++)
    g_clear_signal_handler (&self->manager_handler_ids[i], self->manager);

  g_clear_handle_id (&self->countdown_timer_id, g_source_remove);
  g_clear_weak_pointer (&self->countdown_osd);

  if (self->notification_source != NULL)
    notification_source_detach (self->notification_source);
  g_clear_object (&self->notification_source);

  dispatcher_drop_effect (self, FALSE);
}

static void
on_dispatcher_shutdown (ShellGlobal          *global,
                        ShellBreakDispatcher *self)
{
  if (!self->destroyed)
    dispatcher_detach (self);
}

/**
 * shell_break_dispatcher_destroy:
 * @self: a #ShellBreakDispatcher
 *
 * Removes the notifications, the screen fade and the countdown and stops
 * following the break manager.
 */
void
shell_break_dispatcher_destroy (ShellBreakDispatcher *self)
{
  g_return_if_fail (SHELL_IS_BREAK_DISPATCHER (self));

  if (self->destroyed)
    return;

  dispatcher_ensure_disabled (self);
  dispatcher_detach (self);
}

static void
shell_break_dispatcher_set_property (GObject      *object,
                                     guint         prop_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  ShellBreakDispatcher *self = SHELL_BREAK_DISPATCHER (object);

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
shell_break_dispatcher_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  ShellBreakDispatcher *self = SHELL_BREAK_DISPATCHER (object);

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
shell_break_dispatcher_constructed (GObject *object)
{
  ShellBreakDispatcher *self = SHELL_BREAK_DISPATCHER (object);

  G_OBJECT_CLASS (shell_break_dispatcher_parent_class)->constructed (object);

  self->system_actions = shell_system_actions_get_default ();

  self->manager_handler_ids[0] =
    g_signal_connect_swapped (self->manager, "take-break",
                              G_CALLBACK (on_dispatcher_take_break), self);
  self->manager_handler_ids[1] =
    g_signal_connect_swapped (self->manager, "notify::state",
                              G_CALLBACK (on_dispatcher_state_changed), self);
  self->manager_handler_ids[2] =
    g_signal_connect_swapped (self->manager, "notify::next-break-due-time",
                              G_CALLBACK (on_dispatcher_state_changed), self);

  /* The actors and the message tray are destroyed at shutdown */
  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_dispatcher_shutdown), self, 0);

  if (shell_break_manager_get_state (self->manager) == SHELL_BREAK_STATE_DISABLED)
    dispatcher_ensure_disabled (self);
  else
    dispatcher_ensure_enabled (self);
}

static void
shell_break_dispatcher_dispose (GObject *object)
{
  ShellBreakDispatcher *self = SHELL_BREAK_DISPATCHER (object);

  if (!self->destroyed && self->manager != NULL)
    dispatcher_detach (self);

  g_clear_weak_pointer (&self->layout_manager);
  g_clear_weak_pointer (&self->message_tray);
  g_clear_object (&self->manager);

  G_OBJECT_CLASS (shell_break_dispatcher_parent_class)->dispose (object);
}

static void
shell_break_dispatcher_class_init (ShellBreakDispatcherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = shell_break_dispatcher_set_property;
  object_class->get_property = shell_break_dispatcher_get_property;
  object_class->constructed = shell_break_dispatcher_constructed;
  object_class->dispose = shell_break_dispatcher_dispose;

  /**
   * ShellBreakDispatcher:manager:
   *
   * The break manager to follow.
   */
  disp_props[DISP_PROP_MANAGER] =
    g_param_spec_object ("manager", NULL, NULL,
                         SHELL_TYPE_BREAK_MANAGER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellBreakDispatcher:layout-manager:
   *
   * The layout manager, for the `ui_group` the screen fade applies to and the
   * work area the countdown is shown in.
   */
  disp_props[DISP_PROP_LAYOUT_MANAGER] =
    g_param_spec_object ("layout-manager", NULL, NULL,
                         SHELL_TYPE_LAYOUT_MANAGER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellBreakDispatcher:message-tray:
   *
   * The message tray the break notifications are added to.
   */
  disp_props[DISP_PROP_MESSAGE_TRAY] =
    g_param_spec_object ("message-tray", NULL, NULL,
                         SHELL_TYPE_MESSAGE_TRAY,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, DISP_N_PROPS, disp_props);
}

static void
shell_break_dispatcher_init (ShellBreakDispatcher *self)
{
}

/**
 * shell_break_dispatcher_new:
 * @manager: the #ShellBreakManager to follow
 * @layout_manager: the #ShellLayoutManager
 * @message_tray: the #ShellMessageTray
 *
 * Creates the glue which takes the state-based output from @manager and
 * converts it to event-based notifications, sounds and screen fades for the
 * user to tell them when to take breaks. It factors the user's UI preferences
 * into account.
 *
 * Returns: (transfer full): a new #ShellBreakDispatcher
 */
ShellBreakDispatcher *
shell_break_dispatcher_new (ShellBreakManager  *manager,
                            ShellLayoutManager *layout_manager,
                            ShellMessageTray   *message_tray)
{
  g_return_val_if_fail (SHELL_IS_BREAK_MANAGER (manager), NULL);

  return g_object_new (SHELL_TYPE_BREAK_DISPATCHER,
                       "manager", manager,
                       "layout-manager", layout_manager,
                       "message-tray", message_tray,
                       NULL);
}
