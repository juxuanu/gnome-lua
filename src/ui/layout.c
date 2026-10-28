/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * layout.c: The stage hierarchy of the shell (the "chrome"), the monitors, the
 * hot corners and the startup animation. Port of js/ui/layout.js.
 *
 * What the JS reached through other JS modules is now a signal that ShellMain
 * connects (it is the only part of the shell that knows all the JS objects):
 *
 *  - Main.overview.toggle () for the hot corners:
 *    #ShellLayoutManager::hot-corner-activated
 *  - Main.overview.runStartupAnimation (): #ShellLayoutManager::startup-animation-requested
 *    and shell_layout_manager_startup_animation_finished ()
 *  - Shell.BackgroundMenu.add_to (): #ShellLayoutManager::background-added
 *
 * Main.sessionMode and Main.actionMode are read from #ShellMain.
 *
 * The async functions of the JS (_doStartupAnimation () and the functions it
 * awaits) are a small state machine (StartupState); "await _ensurePrimaryMonitor ()"
 * is the state STARTUP_WAIT_PRIMARY_* which monitors_changed () resumes.
 */

#include "config.h"

#include <math.h>

#include <gio/gio.h>
#include <meta/barrier.h>
#include <meta/boxes.h>
#include <meta/common.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-background-group.h>
#include <meta/meta-backend.h>
#include <meta/meta-later.h>
#include <meta/meta-monitor-manager.h>
#include <meta/meta-workspace-manager.h>
#include <meta/window.h>
#include <meta/workspace.h>

#include "shell-global.h"
#include "ui/background.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/ripples.h"

#define BACKGROUND_FADE_ANIMATION_TIME 1000

#define HOT_CORNER_PRESSURE_THRESHOLD 100 /* pixels */
#define HOT_CORNER_PRESSURE_TIMEOUT 1000 /* ms */

#define SCREEN_TRANSITION_DELAY 250 /* ms */
#define SCREEN_TRANSITION_DURATION 500 /* ms */

static void
set_actor_visible (ClutterActor *actor,
                   gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

/* Math.round () */
static int
js_round (double value)
{
  return (int) floor (value + 0.5);
}

/* ------------------------------------------------------------------------ */
/* Helpers on MetaDisplay shared by the types of this file                   */
/* ------------------------------------------------------------------------ */

static MetaDisplay *
get_display (void)
{
  return shell_global_get_display (shell_global_get ());
}

/* LayoutManager.primaryIndex, which does not depend on the snapshot of the
 * monitors: -1 without monitors, 0 with a single one. */
static int
get_display_primary_index (MetaDisplay *display)
{
  int n_monitors = meta_display_get_n_monitors (display);

  if (n_monitors == 0)
    return -1;

  if (n_monitors == 1)
    return 0;

  return meta_display_get_primary_monitor (display);
}

/* LayoutManager.getWorkAreaForMonitor () */
static void
get_display_work_area_for_monitor (MetaDisplay  *display,
                                   int           monitor_index,
                                   MtkRectangle *work_area)
{
  MetaWorkspaceManager *workspace_manager =
    meta_display_get_workspace_manager (display);
  MetaWorkspace *workspace;

  /* Assume that all workspaces will have the same struts and pick the
   * first one. */
  workspace = meta_workspace_manager_get_workspace_by_index (workspace_manager, 0);
  if (workspace != NULL)
    meta_workspace_get_work_area_for_monitor (workspace, monitor_index, work_area);
  else
    meta_display_get_monitor_geometry (display, monitor_index, work_area);
}

/* LayoutManager._findIndexForRect () */
static int
find_index_for_rect (double x,
                     double y,
                     double width,
                     double height)
{
  MtkRectangle rect;

  rect.x = (int) floor (x);
  rect.y = (int) floor (y);
  rect.width = (int) (ceil (x + width) - floor (x));
  rect.height = (int) (ceil (y + height) - floor (y));

  return meta_display_get_monitor_index_for_rect (get_display (), &rect);
}

/* ------------------------------------------------------------------------ */
/* ShellMonitor                                                              */
/* ------------------------------------------------------------------------ */

G_DEFINE_BOXED_TYPE (ShellMonitor, shell_monitor,
                     shell_monitor_copy, shell_monitor_free)

/**
 * shell_monitor_copy:
 * @self: a #ShellMonitor
 *
 * Returns: (transfer full): a copy of @self
 */
ShellMonitor *
shell_monitor_copy (const ShellMonitor *self)
{
  g_return_val_if_fail (self != NULL, NULL);

  return g_memdup2 (self, sizeof (ShellMonitor));
}

/**
 * shell_monitor_free:
 * @self: a #ShellMonitor
 *
 * Frees @self.
 */
void
shell_monitor_free (ShellMonitor *self)
{
  g_free (self);
}

/**
 * shell_monitor_get_in_fullscreen:
 * @self: a #ShellMonitor
 *
 * Returns: %TRUE if a fullscreen window is shown on the monitor now
 *   (`monitor.inFullscreen`)
 */
gboolean
shell_monitor_get_in_fullscreen (const ShellMonitor *self)
{
  MetaDisplay *display = get_display ();

  g_return_val_if_fail (self != NULL, FALSE);

  if (self->index < 0 || self->index >= meta_display_get_n_monitors (display))
    return FALSE;

  return meta_display_get_monitor_in_fullscreen (display, self->index);
}

/* ------------------------------------------------------------------------ */
/* ShellMonitorConstraint                                                    */
/* ------------------------------------------------------------------------ */

enum
{
  CONSTRAINT_PROP_0,

  CONSTRAINT_PROP_PRIMARY,
  CONSTRAINT_PROP_INDEX,
  CONSTRAINT_PROP_WORK_AREA,

  CONSTRAINT_N_PROPS
};

static GParamSpec *constraint_props[CONSTRAINT_N_PROPS] = { NULL, };

struct _ShellMonitorConstraint
{
  ClutterConstraint parent;

  gboolean primary;
  int index;
  gboolean work_area;

  MetaMonitorManager *monitor_manager;
  gulong monitors_changed_id;
  MetaDisplay *display;
  gulong workareas_changed_id;
};

/**
 * ShellMonitorConstraint:
 *
 * A constraint which gives its actor the geometry (or the work area) of a
 * monitor, given by its index or as the primary monitor. Port of
 * `Layout.MonitorConstraint`. The properties are not construct properties:
 * `new Shell.MonitorConstraint ({index: 1})` sets them after the constructor
 * ran, as the JS class did.
 */
G_DEFINE_FINAL_TYPE (ShellMonitorConstraint, shell_monitor_constraint,
                     CLUTTER_TYPE_CONSTRAINT)

static void
constraint_queue_relayout (ShellMonitorConstraint *self)
{
  ClutterActor *actor = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self));

  if (actor != NULL)
    clutter_actor_queue_relayout (actor);
}

/**
 * shell_monitor_constraint_new:
 *
 * Returns: (transfer full): a new #ShellMonitorConstraint, which has no
 *   effect until #ShellMonitorConstraint:primary or
 *   #ShellMonitorConstraint:index is set
 */
ShellMonitorConstraint *
shell_monitor_constraint_new (void)
{
  return g_object_new (SHELL_TYPE_MONITOR_CONSTRAINT, NULL);
}

gboolean
shell_monitor_constraint_get_primary (ShellMonitorConstraint *self)
{
  g_return_val_if_fail (SHELL_IS_MONITOR_CONSTRAINT (self), FALSE);

  return self->primary;
}

void
shell_monitor_constraint_set_primary (ShellMonitorConstraint *self,
                                      gboolean                primary)
{
  g_return_if_fail (SHELL_IS_MONITOR_CONSTRAINT (self));

  primary = !!primary;

  g_object_freeze_notify (G_OBJECT (self));

  /* Setting the primary monitor resets the index */
  if (primary && self->index != -1)
    {
      self->index = -1;
      g_object_notify_by_pspec (G_OBJECT (self), constraint_props[CONSTRAINT_PROP_INDEX]);
    }

  if (self->primary != primary)
    {
      self->primary = primary;
      g_object_notify_by_pspec (G_OBJECT (self), constraint_props[CONSTRAINT_PROP_PRIMARY]);
    }

  constraint_queue_relayout (self);

  g_object_thaw_notify (G_OBJECT (self));
}

int
shell_monitor_constraint_get_index (ShellMonitorConstraint *self)
{
  g_return_val_if_fail (SHELL_IS_MONITOR_CONSTRAINT (self), -1);

  return self->index;
}

void
shell_monitor_constraint_set_index (ShellMonitorConstraint *self,
                                    int                     index)
{
  g_return_if_fail (SHELL_IS_MONITOR_CONSTRAINT (self));

  g_object_freeze_notify (G_OBJECT (self));

  /* Setting an index resets the primary monitor */
  if (self->primary)
    {
      self->primary = FALSE;
      g_object_notify_by_pspec (G_OBJECT (self), constraint_props[CONSTRAINT_PROP_PRIMARY]);
    }

  if (self->index != index)
    {
      self->index = index;
      g_object_notify_by_pspec (G_OBJECT (self), constraint_props[CONSTRAINT_PROP_INDEX]);
    }

  constraint_queue_relayout (self);

  g_object_thaw_notify (G_OBJECT (self));
}

gboolean
shell_monitor_constraint_get_work_area (ShellMonitorConstraint *self)
{
  g_return_val_if_fail (SHELL_IS_MONITOR_CONSTRAINT (self), FALSE);

  return self->work_area;
}

void
shell_monitor_constraint_set_work_area (ShellMonitorConstraint *self,
                                        gboolean                work_area)
{
  g_return_if_fail (SHELL_IS_MONITOR_CONSTRAINT (self));

  work_area = !!work_area;

  if (self->work_area == work_area)
    return;

  self->work_area = work_area;
  constraint_queue_relayout (self);
  g_object_notify_by_pspec (G_OBJECT (self), constraint_props[CONSTRAINT_PROP_WORK_AREA]);
}

static void
constraint_monitors_changed_cb (MetaMonitorManager     *monitor_manager,
                                ShellMonitorConstraint *self)
{
  constraint_queue_relayout (self);
}

static void
constraint_workareas_changed_cb (MetaDisplay            *display,
                                 ShellMonitorConstraint *self)
{
  if (self->work_area)
    constraint_queue_relayout (self);
}

static void
constraint_disconnect (ShellMonitorConstraint *self)
{
  if (self->monitors_changed_id != 0)
    {
      g_signal_handler_disconnect (self->monitor_manager, self->monitors_changed_id);
      self->monitors_changed_id = 0;
    }

  if (self->workareas_changed_id != 0)
    {
      g_signal_handler_disconnect (self->display, self->workareas_changed_id);
      self->workareas_changed_id = 0;
    }

  g_clear_weak_pointer (&self->monitor_manager);
  g_clear_weak_pointer (&self->display);
}

static void
shell_monitor_constraint_set_actor (ClutterActorMeta *meta,
                                    ClutterActor     *actor)
{
  ShellMonitorConstraint *self = SHELL_MONITOR_CONSTRAINT (meta);

  if (actor != NULL)
    {
      ShellGlobal *global = shell_global_get ();

      if (self->monitors_changed_id == 0)
        {
          MetaBackend *backend = shell_global_get_backend (global);

          g_set_weak_pointer (&self->monitor_manager,
                              meta_backend_get_monitor_manager (backend));
          self->monitors_changed_id =
            g_signal_connect (self->monitor_manager, "monitors-changed",
                              G_CALLBACK (constraint_monitors_changed_cb), self);
        }

      if (self->workareas_changed_id == 0)
        {
          g_set_weak_pointer (&self->display, shell_global_get_display (global));
          self->workareas_changed_id =
            g_signal_connect (self->display, "workareas-changed",
                              G_CALLBACK (constraint_workareas_changed_cb), self);
        }
    }
  else
    {
      constraint_disconnect (self);
    }

  CLUTTER_ACTOR_META_CLASS (shell_monitor_constraint_parent_class)->set_actor (meta, actor);
}

static void
shell_monitor_constraint_update_allocation (ClutterConstraint *constraint,
                                            ClutterActor      *actor,
                                            ClutterActorBox   *allocation)
{
  ShellMonitorConstraint *self = SHELL_MONITOR_CONSTRAINT (constraint);
  MetaDisplay *display = get_display ();
  MtkRectangle rect;
  int n_monitors;
  int index;

  if (!self->primary && self->index < 0)
    return;

  n_monitors = meta_display_get_n_monitors (display);
  if (n_monitors == 0)
    return;

  if (self->primary)
    index = get_display_primary_index (display);
  else
    index = MIN (self->index, n_monitors - 1);

  if (self->work_area)
    get_display_work_area_for_monitor (display, index, &rect);
  else
    meta_display_get_monitor_geometry (display, index, &rect);

  clutter_actor_box_init_rect (allocation, rect.x, rect.y, rect.width, rect.height);
}

static void
shell_monitor_constraint_dispose (GObject *object)
{
  ShellMonitorConstraint *self = SHELL_MONITOR_CONSTRAINT (object);

  constraint_disconnect (self);

  G_OBJECT_CLASS (shell_monitor_constraint_parent_class)->dispose (object);
}

static void
shell_monitor_constraint_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellMonitorConstraint *self = SHELL_MONITOR_CONSTRAINT (object);

  switch (prop_id)
    {
    case CONSTRAINT_PROP_PRIMARY:
      g_value_set_boolean (value, self->primary);
      break;

    case CONSTRAINT_PROP_INDEX:
      g_value_set_int (value, self->index);
      break;

    case CONSTRAINT_PROP_WORK_AREA:
      g_value_set_boolean (value, self->work_area);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_monitor_constraint_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellMonitorConstraint *self = SHELL_MONITOR_CONSTRAINT (object);

  switch (prop_id)
    {
    case CONSTRAINT_PROP_PRIMARY:
      shell_monitor_constraint_set_primary (self, g_value_get_boolean (value));
      break;

    case CONSTRAINT_PROP_INDEX:
      shell_monitor_constraint_set_index (self, g_value_get_int (value));
      break;

    case CONSTRAINT_PROP_WORK_AREA:
      shell_monitor_constraint_set_work_area (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_monitor_constraint_class_init (ShellMonitorConstraintClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorMetaClass *meta_class = CLUTTER_ACTOR_META_CLASS (klass);
  ClutterConstraintClass *constraint_class = CLUTTER_CONSTRAINT_CLASS (klass);

  object_class->dispose = shell_monitor_constraint_dispose;
  object_class->get_property = shell_monitor_constraint_get_property;
  object_class->set_property = shell_monitor_constraint_set_property;

  meta_class->set_actor = shell_monitor_constraint_set_actor;

  constraint_class->update_allocation = shell_monitor_constraint_update_allocation;

  /**
   * ShellMonitorConstraint:primary:
   *
   * Whether the actor covers the primary monitor. Setting it resets
   * #ShellMonitorConstraint:index to -1.
   */
  constraint_props[CONSTRAINT_PROP_PRIMARY] =
    g_param_spec_boolean ("primary", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellMonitorConstraint:index:
   *
   * The index of the monitor the actor covers, -1 for none. An index past
   * the last monitor means the last monitor. Setting it resets
   * #ShellMonitorConstraint:primary.
   */
  constraint_props[CONSTRAINT_PROP_INDEX] =
    g_param_spec_int ("index", NULL, NULL,
                      -1, 64, -1,
                      G_PARAM_READWRITE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellMonitorConstraint:work-area:
   *
   * Whether the actor covers the work area of the monitor (the monitor
   * without the struts) instead of the whole monitor.
   */
  constraint_props[CONSTRAINT_PROP_WORK_AREA] =
    g_param_spec_boolean ("work-area", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, CONSTRAINT_N_PROPS, constraint_props);
}

static void
shell_monitor_constraint_init (ShellMonitorConstraint *self)
{
  self->index = -1;
}

/* ------------------------------------------------------------------------ */
/* ShellUiActor                                                              */
/* ------------------------------------------------------------------------ */

/**
 * ShellUiActor:
 *
 * The actor of `uiGroup`: it always has the size of the stage.
 */
struct _ShellUiActor
{
  StWidget parent;
};

G_DEFINE_FINAL_TYPE (ShellUiActor, shell_ui_actor, ST_TYPE_WIDGET)

static void
shell_ui_actor_get_preferred_width (ClutterActor *actor,
                                    float         for_height,
                                    float        *min_width_p,
                                    float        *natural_width_p)
{
  float width = clutter_actor_get_width (CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ())));

  if (min_width_p != NULL)
    *min_width_p = width;

  if (natural_width_p != NULL)
    *natural_width_p = width;
}

static void
shell_ui_actor_get_preferred_height (ClutterActor *actor,
                                     float         for_width,
                                     float        *min_height_p,
                                     float        *natural_height_p)
{
  float height = clutter_actor_get_height (CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ())));

  if (min_height_p != NULL)
    *min_height_p = height;

  if (natural_height_p != NULL)
    *natural_height_p = height;
}

static void
ui_actor_stage_size_changed_cb (ClutterStage *stage,
                                GParamSpec   *pspec,
                                ShellUiActor *self)
{
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

static void
shell_ui_actor_constructed (GObject *object)
{
  ShellUiActor *self = SHELL_UI_ACTOR (object);
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());

  G_OBJECT_CLASS (shell_ui_actor_parent_class)->constructed (object);

  g_signal_connect_object (stage, "notify::size",
                           G_CALLBACK (ui_actor_stage_size_changed_cb), self, 0);
}

static void
shell_ui_actor_class_init (ShellUiActorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_ui_actor_constructed;

  actor_class->get_preferred_width = shell_ui_actor_get_preferred_width;
  actor_class->get_preferred_height = shell_ui_actor_get_preferred_height;
}

static void
shell_ui_actor_init (ShellUiActor *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellScreenTransition                                                     */
/* ------------------------------------------------------------------------ */

/**
 * ShellScreenTransition:
 *
 * A screenshot of the stage which fades out, to hide a change of the whole
 * screen (the dark/light style switch, the theme switch). See
 * shell_screen_transition_run ().
 */
struct _ShellScreenTransition
{
  ClutterActor parent;
};

G_DEFINE_FINAL_TYPE (ShellScreenTransition, shell_screen_transition,
                     CLUTTER_TYPE_ACTOR)

static void
screen_transition_stopped_cb (gboolean finished,
                              gpointer user_data)
{
  ClutterActor *self = user_data;

  /* onStopped: both when the animation ran to the end and when it was
   * stopped */
  clutter_actor_hide (self);
}

/**
 * shell_screen_transition_run:
 * @self: a #ShellScreenTransition
 *
 * Takes a screenshot of the stage, shows it on top of everything and fades it
 * out. Does nothing while the transition is visible.
 */
void
shell_screen_transition_run (ShellScreenTransition *self)
{
  ShellGlobal *global = shell_global_get ();
  ClutterStage *stage = shell_global_get_stage (global);
  ClutterActor *actor;
  g_autoptr (GError) error = NULL;
  g_autoptr (ClutterContent) content = NULL;
  ShellEaseParams params = {
    .duration = SCREEN_TRANSITION_DURATION,
    .delay = SCREEN_TRANSITION_DELAY,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
  MtkRectangle rect;
  int capture_width, capture_height;
  float scale = 1.0;

  g_return_if_fail (SHELL_IS_SCREEN_TRANSITION (self));

  actor = CLUTTER_ACTOR (self);

  if (clutter_actor_is_visible (actor))
    return;

  /* Main.uiGroup.set_child_above_sibling (this, null) */
  if (clutter_actor_get_parent (actor) != NULL)
    clutter_actor_set_child_above_sibling (clutter_actor_get_parent (actor), actor, NULL);

  rect = (MtkRectangle) {
    .x = 0,
    .y = 0,
    .width = shell_global_get_screen_width (global),
    .height = shell_global_get_screen_height (global),
  };

  clutter_stage_get_capture_final_size (stage, &rect,
                                        &capture_width, &capture_height, &scale);
  content = clutter_stage_paint_to_content (stage, &rect, scale, NULL,
                                            CLUTTER_PAINT_FLAG_NO_CURSORS,
                                            &error);
  if (content == NULL)
    {
      g_warning ("%s: Failed to take a screenshot of the stage: %s",
                 G_STRFUNC, error->message);
      return;
    }

  clutter_actor_set_content (actor, content);

  clutter_actor_set_opacity (actor, 255);
  clutter_actor_show (actor);

  shell_actor_ease (actor, &params,
                    screen_transition_stopped_cb, g_object_ref (actor), g_object_unref,
                    "opacity", 0u,
                    NULL);
}

static void
shell_screen_transition_hide (ClutterActor *actor)
{
  clutter_actor_set_content (actor, NULL);

  CLUTTER_ACTOR_CLASS (shell_screen_transition_parent_class)->hide (actor);
}

static void
shell_screen_transition_constructed (GObject *object)
{
  G_OBJECT_CLASS (shell_screen_transition_parent_class)->constructed (object);

  clutter_actor_hide (CLUTTER_ACTOR (object));
}

static void
shell_screen_transition_class_init (ShellScreenTransitionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_screen_transition_constructed;

  actor_class->hide = shell_screen_transition_hide;
}

static void
shell_screen_transition_init (ShellScreenTransition *self)
{
}

/* ------------------------------------------------------------------------ */
/* PressureBarrier                                                           */
/* ------------------------------------------------------------------------ */

typedef struct _PressureBarrier PressureBarrier;

typedef struct
{
  PressureBarrier *pressure;
  MetaBarrier *barrier;
  gboolean horizontal;
  gboolean is_hit;
  gulong hit_id;
  gulong left_id;
} PressureBarrierEntry;

typedef struct
{
  guint32 time;
  double distance;
} PressureBarrierEvent;

typedef void (* PressureBarrierTriggerFunc) (gpointer user_data);

struct _PressureBarrier
{
  int threshold;
  int timeout;
  ShellActionMode action_mode;

  GPtrArray *barriers;          /* of PressureBarrierEntry */
  GArray *barrier_events;       /* of PressureBarrierEvent, oldest first */
  double current_pressure;
  guint32 last_time;
  gboolean is_triggered;

  PressureBarrierTriggerFunc trigger;
  gpointer trigger_data;
};

static void
pressure_barrier_reset (PressureBarrier *self)
{
  g_array_set_size (self->barrier_events, 0);
  self->current_pressure = 0;
  self->last_time = 0;
}

static void
pressure_barrier_entry_free (PressureBarrierEntry *entry)
{
  g_clear_signal_handler (&entry->hit_id, entry->barrier);
  g_clear_signal_handler (&entry->left_id, entry->barrier);
  g_free (entry);
}

static PressureBarrier *
pressure_barrier_new (int                        threshold,
                      int                        timeout,
                      ShellActionMode            action_mode,
                      PressureBarrierTriggerFunc trigger,
                      gpointer                   trigger_data)
{
  PressureBarrier *self = g_new0 (PressureBarrier, 1);

  self->threshold = threshold;
  self->timeout = timeout;
  self->action_mode = action_mode;
  self->barriers = g_ptr_array_new_with_free_func ((GDestroyNotify) pressure_barrier_entry_free);
  self->barrier_events = g_array_new (FALSE, FALSE, sizeof (PressureBarrierEvent));
  self->trigger = trigger;
  self->trigger_data = trigger_data;

  pressure_barrier_reset (self);

  return self;
}

/* PressureBarrier.destroy () */
static void
pressure_barrier_free (PressureBarrier *self)
{
  g_ptr_array_unref (self->barriers);
  g_array_unref (self->barrier_events);
  g_free (self);
}

static void
pressure_barrier_trim_events (PressureBarrier *self)
{
  /* Events are guaranteed to be sorted in time order from oldest to newest,
   * so just look for the first old event, and then chop events after that
   * off. */
  int64_t threshold = (int64_t) self->last_time - self->timeout;
  guint first_new_event = 0;
  guint i;

  while (first_new_event < self->barrier_events->len)
    {
      PressureBarrierEvent *event =
        &g_array_index (self->barrier_events, PressureBarrierEvent, first_new_event);

      if ((int64_t) event->time >= threshold)
        break;

      first_new_event++;
    }

  for (i = 0; i < first_new_event; i++)
    {
      PressureBarrierEvent *event =
        &g_array_index (self->barrier_events, PressureBarrierEvent, i);

      self->current_pressure -= event->distance;
    }

  g_array_remove_range (self->barrier_events, 0, first_new_event);
}

static void
pressure_barrier_trigger (PressureBarrier *self)
{
  self->is_triggered = TRUE;
  self->trigger (self->trigger_data);
  pressure_barrier_reset (self);
}

static void
pressure_barrier_on_left (MetaBarrier          *barrier,
                          MetaBarrierEvent     *event,
                          PressureBarrierEntry *entry)
{
  PressureBarrier *self = entry->pressure;
  guint i;

  entry->is_hit = FALSE;

  for (i = 0; i < self->barriers->len; i++)
    {
      PressureBarrierEntry *other = g_ptr_array_index (self->barriers, i);

      if (other->is_hit)
        return;
    }

  pressure_barrier_reset (self);
  self->is_triggered = FALSE;
}

static void
pressure_barrier_on_hit (MetaBarrier          *barrier,
                         MetaBarrierEvent     *event,
                         PressureBarrierEntry *entry)
{
  PressureBarrier *self = entry->pressure;
  PressureBarrierEvent barrier_event;
  double slide, distance;

  entry->is_hit = TRUE;

  /* If we've triggered the barrier, wait until the pointer has the left the
   * barrier hitbox until we trigger it again. */
  if (self->is_triggered)
    return;

  /* Throw out all events not in the proper keybinding mode */
  if (!(self->action_mode & shell_main_get_action_mode (shell_main_get_default ())))
    return;

  if (entry->horizontal)
    {
      slide = fabs (event->dx);
      distance = fabs (event->dy);
    }
  else
    {
      slide = fabs (event->dy);
      distance = fabs (event->dx);
    }

  if (distance >= self->threshold)
    {
      pressure_barrier_trigger (self);
      return;
    }

  /* Throw out events where the cursor is move more along the axis of the
   * barrier than moving with the barrier. */
  if (slide > distance)
    return;

  self->last_time = event->time;

  pressure_barrier_trim_events (self);
  distance = MIN (15, distance);

  barrier_event.time = event->time;
  barrier_event.distance = distance;
  g_array_append_val (self->barrier_events, barrier_event);
  self->current_pressure += distance;

  if (self->current_pressure >= self->threshold)
    pressure_barrier_trigger (self);
}

static PressureBarrierEntry *
pressure_barrier_add (PressureBarrier *self,
                      MetaBarrier     *barrier,
                      gboolean         horizontal)
{
  PressureBarrierEntry *entry = g_new0 (PressureBarrierEntry, 1);

  entry->pressure = self;
  entry->barrier = barrier;
  entry->horizontal = horizontal;
  entry->hit_id = g_signal_connect (barrier, "hit",
                                    G_CALLBACK (pressure_barrier_on_hit), entry);
  entry->left_id = g_signal_connect (barrier, "left",
                                     G_CALLBACK (pressure_barrier_on_left), entry);

  g_ptr_array_add (self->barriers, entry);

  return entry;
}

static void
pressure_barrier_remove (PressureBarrier *self,
                         MetaBarrier     *barrier)
{
  guint i;

  for (i = 0; i < self->barriers->len; i++)
    {
      PressureBarrierEntry *entry = g_ptr_array_index (self->barriers, i);

      if (entry->barrier == barrier)
        {
          g_ptr_array_remove_index (self->barriers, i);
          return;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Barriers                                                                  */
/* ------------------------------------------------------------------------ */

static MetaBarrier *
barrier_create (int                  x1,
                int                  y1,
                int                  x2,
                int                  y2,
                MetaBarrierDirection directions)
{
  MetaBackend *backend = shell_global_get_backend (shell_global_get ());
  g_autoptr (GError) error = NULL;
  MetaBarrier *barrier;

  barrier = meta_barrier_new (backend, x1, y1, x2, y2, directions,
                              META_BARRIER_FLAG_NONE, &error);
  if (barrier == NULL)
    g_warning ("%s: Failed to create a barrier: %s", G_STRFUNC, error->message);

  return barrier;
}

static void
barrier_destroy (MetaBarrier **barrier)
{
  if (*barrier == NULL)
    return;

  meta_barrier_destroy (*barrier);
  g_clear_object (barrier);
}

/* ------------------------------------------------------------------------ */
/* HotCorner                                                                 */
/* ------------------------------------------------------------------------ */

/*
 * HotCorner:
 *
 * A "hot corner" that can toggle switching to overview. Private to this
 * file, and not an actor any more: the JS actor was never added to the
 * stage. So `HotCorner.handleDragOver ()`, which toggled the overview when an
 * XDND drag moved over the corner, was unreachable and is not ported.
 */
typedef struct
{
  ShellLayoutManager *manager;  /* weak: the manager owns the corners */
  int monitor_index;
  int x;
  int y;

  PressureBarrier *pressure_barrier;
  MetaBarrier *vertical_barrier;
  MetaBarrier *horizontal_barrier;
  ShellRipples *ripples;
} HotCorner;

/* ------------------------------------------------------------------------ */
/* ShellLayoutManager                                                        */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_UI_GROUP,
  PROP_OVERVIEW_GROUP,
  PROP_SCREEN_SHIELD_GROUP,
  PROP_PANEL_BOX,
  PROP_MODAL_DIALOG_GROUP,
  PROP_KEYBOARD_BOX,
  PROP_SCREENSHOT_UI_GROUP,
  PROP_DUMMY_CURSOR,
  PROP_SCREEN_TRANSITION,
  PROP_N_MONITORS,
  PROP_PRIMARY_INDEX,
  PROP_PRIMARY_MONITOR,
  PROP_BOTTOM_INDEX,
  PROP_BOTTOM_MONITOR,
  PROP_FOCUS_INDEX,
  PROP_FOCUS_MONITOR,
  PROP_CURRENT_MONITOR,
  PROP_KEYBOARD_INDEX,
  PROP_KEYBOARD_MONITOR,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  HOT_CORNERS_CHANGED,
  STARTUP_COMPLETE,
  STARTUP_PREPARED,
  MONITORS_CHANGED,
  SYSTEM_MODAL_OPENED,
  BACKGROUND_ADDED,
  HOT_CORNER_ACTIVATED,
  STARTUP_ANIMATION_REQUESTED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

/* Where the (async) startup sequence waits */
typedef enum
{
  STARTUP_NOT_STARTED,
  STARTUP_WAIT_PRIMARY_LOAD,    /* _loadBackground (): _ensurePrimaryMonitor () */
  STARTUP_WAIT_SYSTEM_BACKGROUND, /* _loadBackground (): the 'loaded' signal */
  STARTUP_WAIT_PRIMARY_PREPARE, /* _prepareStartupAnimation (): _ensurePrimaryMonitor () */
  STARTUP_WAIT_BACKGROUNDS,     /* _prepareStartupAnimation (): _updateBackgrounds () */
  STARTUP_ANIMATING,            /* _startupAnimation () */
  STARTUP_DONE,
} StartupState;

/* What _trackedActors held */
typedef struct
{
  ClutterActor *actor;
  ShellChromeFlags flags;
  gulong visible_id;
  gulong allocation_id;
  gulong destroy_id;
} TrackedActor;

struct _ShellLayoutManager
{
  GObject parent;

  gboolean rtl;
  gboolean torn_down;

  GPtrArray *monitors;          /* of ShellMonitor */
  int primary_index;
  int bottom_index;
  int keyboard_index;

  GPtrArray *hot_corners;       /* of HotCorner, NULL where the monitor has none */
  MetaBarrier *right_panel_barrier;

  gboolean in_overview;
  guint update_region_later;

  GPtrArray *tracked_actors;    /* of TrackedActor */
  gboolean starting_up;

  /* Strong references to the actors, the stage hierarchy owns them too */
  ShellUiActor *ui_group;
  StWidget *overview_group;
  StWidget *screen_shield_group;
  StBoxLayout *panel_box;
  StWidget *modal_dialog_group;
  StBoxLayout *keyboard_box;
  StWidget *screenshot_ui_group;
  StWidget *dummy_cursor;
  ShellScreenTransition *screen_transition;
  ClutterActor *background_group;
  ClutterActor *input_panel_group;

  GPtrArray *bg_managers;       /* of ShellBackgroundManager */
  int bg_pending;               /* backgrounds which did not emit 'loaded' yet */
  gboolean bg_loaded;           /* the backgrounds of the last _updateBackgrounds () loaded */

  GSettings *interface_settings;
  MetaMonitorManager *monitor_manager;
  gulong monitors_changed_id;
  ShellGlobal *global; /* weak */
  gulong shutdown_id;
  gulong session_updated_id;
  ShellSessionMode *session_mode; /* weak */

  /* Startup */
  StartupState startup_state;
  ShellSystemBackground *system_background;
  ClutterActor *cover_pane;
  guint startup_prepared_timeout_id;
};

G_DEFINE_FINAL_TYPE (ShellLayoutManager, shell_layout_manager, G_TYPE_OBJECT)

static void update_visibility (ShellLayoutManager *self);
static void queue_update_regions (ShellLayoutManager *self);
static void update_regions (ShellLayoutManager *self);
static void update_hot_corners (ShellLayoutManager *self);
static void startup_resume (ShellLayoutManager *self);

/* Main.sessionMode.<flag>, with a default for a shell without a session
 * mode (never in practice: ShellMain sets it before it creates the layout
 * manager) */
static gboolean
session_mode_get_flag (const char *property,
                       gboolean    fallback)
{
  ShellSessionMode *session_mode = shell_main_get_session_mode (shell_main_get_default ());
  gboolean value;

  if (session_mode == NULL)
    return fallback;

  g_object_get (session_mode, property, &value, NULL);

  return value;
}

static gboolean
is_greeter (void)
{
  return session_mode_get_flag ("is-greeter", FALSE);
}

/* ---- monitors ---------------------------------------------------------- */

static ShellMonitor *
find_monitor_for_index (ShellLayoutManager *self,
                        int                 index)
{
  if (index >= 0 && index < (int) self->monitors->len)
    return g_ptr_array_index (self->monitors, index);

  return NULL;
}

static gboolean
is_above_or_below_primary (ShellLayoutManager *self,
                           const ShellMonitor *monitor)
{
  const ShellMonitor *primary = g_ptr_array_index (self->monitors, self->primary_index);
  int monitor_left = monitor->x, monitor_right = monitor->x + monitor->width;
  int primary_left = primary->x, primary_right = primary->x + primary->width;

  if ((monitor_left >= primary_left && monitor_left < primary_right) ||
      (monitor_right > primary_left && monitor_right <= primary_right) ||
      (primary_left >= monitor_left && primary_left < monitor_right) ||
      (primary_right > monitor_left && primary_right <= monitor_right))
    return TRUE;

  return FALSE;
}

static void
update_monitors (ShellLayoutManager *self)
{
  MetaDisplay *display = get_display ();
  int n_monitors = meta_display_get_n_monitors (display);
  int i;

  g_ptr_array_set_size (self->monitors, 0);

  for (i = 0; i < n_monitors; i++)
    {
      ShellMonitor *monitor = g_new0 (ShellMonitor, 1);
      MtkRectangle geometry;

      meta_display_get_monitor_geometry (display, i, &geometry);

      monitor->index = i;
      monitor->x = geometry.x;
      monitor->y = geometry.y;
      monitor->width = geometry.width;
      monitor->height = geometry.height;
      monitor->geometry_scale = meta_display_get_monitor_scale (display, i);

      g_ptr_array_add (self->monitors, monitor);
    }

  if (n_monitors == 0)
    {
      self->primary_index = self->bottom_index = -1;
    }
  else if (n_monitors == 1)
    {
      self->primary_index = self->bottom_index = 0;
    }
  else
    {
      /* If there are monitors below the primary, then we need to split
       * primary from bottom. */
      self->primary_index = self->bottom_index = meta_display_get_primary_monitor (display);

      for (i = 0; i < n_monitors; i++)
        {
          const ShellMonitor *monitor = g_ptr_array_index (self->monitors, i);
          const ShellMonitor *bottom = g_ptr_array_index (self->monitors, self->bottom_index);

          if (is_above_or_below_primary (self, monitor) && monitor->y > bottom->y)
            self->bottom_index = i;
        }
    }
}

static void
update_keyboard_box (ShellLayoutManager *self)
{
  const ShellMonitor *monitor = find_monitor_for_index (self, self->keyboard_index);

  /* The JS threw a TypeError here without a keyboard monitor */
  if (monitor == NULL)
    return;

  clutter_actor_set_position (CLUTTER_ACTOR (self->keyboard_box),
                              monitor->x, monitor->y + monitor->height);
  clutter_actor_set_size (CLUTTER_ACTOR (self->keyboard_box), monitor->width, -1);
}

static void
update_boxes (ShellLayoutManager *self)
{
  const ShellMonitor *primary = find_monitor_for_index (self, self->primary_index);

  if (primary == NULL)
    return;

  clutter_actor_set_position (CLUTTER_ACTOR (self->panel_box), primary->x, primary->y);
  clutter_actor_set_size (CLUTTER_ACTOR (self->panel_box), primary->width, -1);

  shell_layout_manager_set_keyboard_index (self, self->primary_index);
}

/* ---- barriers and hot corners ------------------------------------------ */

static void
destroy_panel_barrier (ShellLayoutManager *self)
{
  barrier_destroy (&self->right_panel_barrier);
}

static void
update_panel_barrier (ShellLayoutManager *self)
{
  const ShellMonitor *primary;
  float panel_height;

  destroy_panel_barrier (self);

  primary = find_monitor_for_index (self, self->primary_index);
  if (primary == NULL)
    return;

  panel_height = clutter_actor_get_height (CLUTTER_ACTOR (self->panel_box));
  if (panel_height != 0)
    {
      self->right_panel_barrier =
        barrier_create (primary->x + primary->width, primary->y,
                        primary->x + primary->width, primary->y + (int) panel_height,
                        META_BARRIER_DIRECTION_NEGATIVE_X);
    }
}

static void
hot_corner_toggle_overview (gpointer user_data)
{
  HotCorner *corner = user_data;
  gboolean play_ripples = FALSE;

  g_signal_emit (corner->manager, signals[HOT_CORNER_ACTIVATED], 0,
                 corner->monitor_index,
                 meta_display_get_monitor_in_fullscreen (get_display (), corner->monitor_index),
                 &play_ripples);

  if (play_ripples)
    shell_ripples_play_animation (corner->ripples, corner->x, corner->y);
}

static void
hot_corner_set_barrier_size (HotCorner *corner,
                             float      size)
{
  if (corner->vertical_barrier != NULL)
    {
      pressure_barrier_remove (corner->pressure_barrier, corner->vertical_barrier);
      barrier_destroy (&corner->vertical_barrier);
    }

  if (corner->horizontal_barrier != NULL)
    {
      pressure_barrier_remove (corner->pressure_barrier, corner->horizontal_barrier);
      barrier_destroy (&corner->horizontal_barrier);
    }

  if (size > 0)
    {
      int int_size = (int) size;

      if (clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL)
        {
          corner->vertical_barrier =
            barrier_create (corner->x, corner->y, corner->x, corner->y + int_size,
                            META_BARRIER_DIRECTION_NEGATIVE_X);
          corner->horizontal_barrier =
            barrier_create (corner->x - int_size, corner->y, corner->x, corner->y,
                            META_BARRIER_DIRECTION_POSITIVE_Y);
        }
      else
        {
          corner->vertical_barrier =
            barrier_create (corner->x, corner->y, corner->x, corner->y + int_size,
                            META_BARRIER_DIRECTION_POSITIVE_X);
          corner->horizontal_barrier =
            barrier_create (corner->x, corner->y, corner->x + int_size, corner->y,
                            META_BARRIER_DIRECTION_POSITIVE_Y);
        }

      if (corner->vertical_barrier != NULL)
        pressure_barrier_add (corner->pressure_barrier, corner->vertical_barrier, FALSE);

      if (corner->horizontal_barrier != NULL)
        pressure_barrier_add (corner->pressure_barrier, corner->horizontal_barrier, TRUE);
    }
}

static void
hot_corner_free (HotCorner *corner)
{
  if (corner == NULL)
    return;

  hot_corner_set_barrier_size (corner, 0);
  g_clear_pointer (&corner->pressure_barrier, pressure_barrier_free);

  if (corner->ripples != NULL)
    {
      shell_ripples_destroy (corner->ripples);
      g_clear_object (&corner->ripples);
    }

  g_free (corner);
}

static HotCorner *
hot_corner_new (ShellLayoutManager *manager,
                int                 monitor_index,
                int                 x,
                int                 y)
{
  HotCorner *corner = g_new0 (HotCorner, 1);
  double px = 0.0;
  double py = 0.0;

  corner->manager = manager;
  corner->monitor_index = monitor_index;
  corner->x = x;
  corner->y = y;

  corner->pressure_barrier =
    pressure_barrier_new (HOT_CORNER_PRESSURE_THRESHOLD,
                          HOT_CORNER_PRESSURE_TIMEOUT,
                          SHELL_ACTION_MODE_NORMAL | SHELL_ACTION_MODE_OVERVIEW,
                          hot_corner_toggle_overview, corner);

  if (clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL)
    {
      px = 1.0;
      py = 0.0;
    }

  corner->ripples = shell_ripples_new (px, py, "ripple-box", FALSE);
  shell_ripples_add_to (corner->ripples, CLUTTER_ACTOR (manager->ui_group));

  return corner;
}

static void
destroy_hot_corners (ShellLayoutManager *self)
{
  g_ptr_array_set_size (self->hot_corners, 0);
}

static void
update_hot_corners (ShellLayoutManager *self)
{
  float size;
  guint i, j;

  /* destroy old hot corners */
  destroy_hot_corners (self);

  if (!g_settings_get_boolean (self->interface_settings, "enable-hot-corners"))
    {
      g_signal_emit (self, signals[HOT_CORNERS_CHANGED], 0);
      return;
    }

  size = clutter_actor_get_height (CLUTTER_ACTOR (self->panel_box));

  /* build new hot corners */
  for (i = 0; i < self->monitors->len; i++)
    {
      const ShellMonitor *monitor = g_ptr_array_index (self->monitors, i);
      int corner_x = self->rtl ? monitor->x + monitor->width : monitor->x;
      int corner_y = monitor->y;
      gboolean have_top_left_corner = TRUE;

      if ((int) i != self->primary_index)
        {
          /* Check if we have a top left (right for RTL) corner. I.e. if
           * there is no monitor directly above or to the left(right) */
          int beside_x = self->rtl ? monitor->x + 1 : corner_x - 1;
          int beside_y = corner_y;
          int above_x = corner_x;
          int above_y = corner_y - 1;

          for (j = 0; j < self->monitors->len; j++)
            {
              const ShellMonitor *other;

              if (i == j)
                continue;

              other = g_ptr_array_index (self->monitors, j);

              if (beside_x >= other->x &&
                  beside_x < other->x + other->width &&
                  beside_y >= other->y &&
                  beside_y < other->y + other->height)
                {
                  have_top_left_corner = FALSE;
                  break;
                }

              if (above_x >= other->x &&
                  above_x < other->x + other->width &&
                  above_y >= other->y &&
                  above_y < other->y + other->height)
                {
                  have_top_left_corner = FALSE;
                  break;
                }
            }
        }

      if (have_top_left_corner)
        {
          HotCorner *corner = hot_corner_new (self, i, corner_x, corner_y);

          hot_corner_set_barrier_size (corner, size);
          g_ptr_array_add (self->hot_corners, corner);
        }
      else
        {
          g_ptr_array_add (self->hot_corners, NULL);
        }
    }

  g_signal_emit (self, signals[HOT_CORNERS_CHANGED], 0);
}

static void
panel_box_changed (ShellLayoutManager *self)
{
  float size;
  guint i;

  if (self->torn_down)
    return;

  update_panel_barrier (self);

  size = clutter_actor_get_height (CLUTTER_ACTOR (self->panel_box));
  for (i = 0; i < self->hot_corners->len; i++)
    {
      HotCorner *corner = g_ptr_array_index (self->hot_corners, i);

      if (corner != NULL)
        hot_corner_set_barrier_size (corner, size);
    }
}

static void
panel_box_allocation_changed_cb (ClutterActor       *panel_box,
                                 GParamSpec         *pspec,
                                 ShellLayoutManager *self)
{
  panel_box_changed (self);
}

/* ---- backgrounds -------------------------------------------------------- */

static void
bg_manager_free (ShellBackgroundManager *manager)
{
  g_signal_handlers_disconnect_by_data (manager, g_object_get_data (G_OBJECT (manager),
                                                                    "shell-layout-manager"));
  shell_background_manager_destroy (manager);
  g_object_unref (manager);
}

static void
emit_background_added (ShellLayoutManager     *self,
                       ShellBackgroundManager *manager)
{
  guint index;

  if (!g_ptr_array_find (self->bg_managers, manager, &index))
    return;

  /* Shell.BackgroundMenu.add_to () */
  g_signal_emit (self, signals[BACKGROUND_ADDED], 0,
                 shell_background_manager_get_background_actor (manager),
                 (int) index);
}

static void
bg_manager_changed_cb (ShellBackgroundManager *manager,
                       ShellLayoutManager     *self)
{
  emit_background_added (self, manager);
}

static void
bg_manager_loaded_cb (ShellBackgroundManager *manager,
                      ShellLayoutManager     *self)
{
  /* _waitLoaded () only waits for the first 'loaded' */
  g_signal_handlers_disconnect_by_func (manager, bg_manager_loaded_cb, self);

  self->bg_pending--;
  if (self->bg_pending > 0)
    return;

  self->bg_loaded = TRUE;

  if (self->startup_state == STARTUP_WAIT_BACKGROUNDS)
    startup_resume (self);
}

static ShellBackgroundManager *
create_background_manager (ShellLayoutManager *self,
                           int                 monitor_index)
{
  ShellBackgroundManager *manager;

  manager = shell_background_manager_new (self->background_group, monitor_index);
  g_object_set_data (G_OBJECT (manager), "shell-layout-manager", self);
  g_signal_connect (manager, "changed", G_CALLBACK (bg_manager_changed_cb), self);

  return manager;
}

/* LayoutManager._updateBackgrounds (): the part which ran synchronously. The
 * promise it returned is bg_loaded, startup_resume () is the await. */
static void
update_backgrounds (ShellLayoutManager *self)
{
  int i;

  g_ptr_array_set_size (self->bg_managers, 0);
  self->bg_pending = 0;

  if (is_greeter ())
    {
      self->bg_loaded = TRUE;
      return;
    }

  self->bg_loaded = FALSE;

  for (i = 0; i < (int) self->monitors->len; i++)
    {
      ShellBackgroundManager *manager = create_background_manager (self, i);

      g_ptr_array_add (self->bg_managers, manager);

      if (i != self->primary_index && self->starting_up)
        {
          clutter_actor_hide (shell_background_manager_get_background_actor (manager));
        }

      emit_background_added (self, manager);
    }

  self->bg_pending = self->bg_managers->len;
  if (self->bg_pending == 0)
    {
      self->bg_loaded = TRUE;
      return;
    }

  for (i = 0; i < (int) self->bg_managers->len; i++)
    {
      g_signal_connect (g_ptr_array_index (self->bg_managers, i), "loaded",
                        G_CALLBACK (bg_manager_loaded_cb), self);
    }
}

static void
show_secondary_backgrounds (ShellLayoutManager *self)
{
  guint i;

  for (i = 0; i < self->monitors->len && i < self->bg_managers->len; i++)
    {
      ShellBackgroundManager *manager = g_ptr_array_index (self->bg_managers, i);
      ClutterActor *background_actor;
      ShellEaseParams params = {
        .duration = BACKGROUND_FADE_ANIMATION_TIME,
        .delay = -1,
        .mode = CLUTTER_EASE_OUT_QUAD,
        .repeat_count = 0,
        .auto_reverse = FALSE,
        .animation_required = FALSE,
      };

      if ((int) i == self->primary_index)
        continue;

      background_actor = shell_background_manager_get_background_actor (manager);
      clutter_actor_show (background_actor);
      clutter_actor_set_opacity (background_actor, 0);
      shell_actor_ease (background_actor, &params, NULL, NULL, NULL,
                        "opacity", 255u,
                        NULL);
    }
}

/* ---- chrome ------------------------------------------------------------- */

static void
tracked_actor_free (TrackedActor *tracked)
{
  g_clear_signal_handler (&tracked->visible_id, tracked->actor);
  g_clear_signal_handler (&tracked->allocation_id, tracked->actor);
  g_clear_signal_handler (&tracked->destroy_id, tracked->actor);
  g_free (tracked);
}

static int
find_tracked_actor (ShellLayoutManager *self,
                    ClutterActor       *actor)
{
  guint i;

  if (actor == NULL)
    return -1;

  for (i = 0; i < self->tracked_actors->len; i++)
    {
      TrackedActor *tracked = g_ptr_array_index (self->tracked_actors, i);

      if (tracked->actor == actor)
        return i;
    }

  return -1;
}

static void
update_actor_visibility (ShellLayoutManager *self,
                         TrackedActor       *tracked)
{
  ShellGlobal *global = shell_global_get ();
  ShellMonitor *monitor;

  if (!(tracked->flags & SHELL_CHROME_FLAGS_TRACK_FULLSCREEN))
    return;

  monitor = find_monitor_for_index (self, shell_layout_manager_find_index_for_actor (self, tracked->actor));

  set_actor_visible (tracked->actor,
                     !(clutter_actor_is_visible (shell_global_get_window_group (global)) &&
                       monitor != NULL &&
                       shell_monitor_get_in_fullscreen (monitor)));
}

static void
untrack_actor (ShellLayoutManager *self,
               ClutterActor       *actor)
{
  int i = find_tracked_actor (self, actor);

  if (i == -1)
    return;

  g_ptr_array_remove_index (self->tracked_actors, i);

  queue_update_regions (self);
}

static void
tracked_actor_changed_cb (ClutterActor       *actor,
                          GParamSpec         *pspec,
                          ShellLayoutManager *self)
{
  queue_update_regions (self);
}

static void
tracked_actor_destroyed_cb (ClutterActor       *actor,
                            ShellLayoutManager *self)
{
  /* Note that destroying actor will unset its parent, so we don't need to
   * connect to 'destroy' too. */
  untrack_actor (self, actor);
}

static void
track_actor (ShellLayoutManager *self,
             ClutterActor       *actor,
             ShellChromeFlags    flags)
{
  TrackedActor *tracked;

  if (find_tracked_actor (self, actor) != -1)
    {
      g_warning ("%s: trying to re-track existing chrome actor", G_STRFUNC);
      return;
    }

  tracked = g_new0 (TrackedActor, 1);
  tracked->actor = actor;
  tracked->flags = flags;
  tracked->visible_id = g_signal_connect (actor, "notify::visible",
                                          G_CALLBACK (tracked_actor_changed_cb), self);
  tracked->allocation_id = g_signal_connect (actor, "notify::allocation",
                                             G_CALLBACK (tracked_actor_changed_cb), self);
  tracked->destroy_id = g_signal_connect (actor, "destroy",
                                          G_CALLBACK (tracked_actor_destroyed_cb), self);

  g_ptr_array_add (self->tracked_actors, tracked);
  update_actor_visibility (self, tracked);
  queue_update_regions (self);
}

/* ---- visibility and regions --------------------------------------------- */

static void
update_visibility (ShellLayoutManager *self)
{
  ShellGlobal *global = shell_global_get ();
  gboolean windows_visible;
  guint i;

  if (self->torn_down)
    return;

  windows_visible = session_mode_get_flag ("has-windows", TRUE) && !self->in_overview;

  set_actor_visible (shell_global_get_window_group (global), windows_visible);
  set_actor_visible (shell_global_get_top_window_group (global), windows_visible);

  for (i = 0; i < self->tracked_actors->len; i++)
    update_actor_visibility (self, g_ptr_array_index (self->tracked_actors, i));
}

static void
clear_update_region_later (ShellLayoutManager *self)
{
  if (self->update_region_later != 0)
    {
      /* Without a global there is no compositor left to own the later */
      if (self->global != NULL)
        {
          MetaLaters *laters = meta_compositor_get_laters (shell_global_get_compositor (self->global));

          meta_laters_remove (laters, self->update_region_later);
        }
      self->update_region_later = 0;
    }
}

static gboolean
update_regions_later_cb (gpointer user_data)
{
  ShellLayoutManager *self = user_data;

  /* The later is being removed by returning G_SOURCE_REMOVE */
  self->update_region_later = 0;
  update_regions (self);

  return G_SOURCE_REMOVE;
}

static void
queue_update_regions (ShellLayoutManager *self)
{
  if (self->torn_down || self->update_region_later != 0)
    return;

  self->update_region_later =
    meta_laters_add (meta_compositor_get_laters (shell_global_get_compositor (shell_global_get ())),
                     META_LATER_BEFORE_REDRAW,
                     update_regions_later_cb, self, NULL);
}

static void
update_regions (ShellLayoutManager *self)
{
  ShellGlobal *global = shell_global_get ();
  MetaWorkspaceManager *workspace_manager =
    meta_display_get_workspace_manager (get_display ());
  GSList *struts = NULL;
  int screen_width = shell_global_get_screen_width (global);
  int screen_height = shell_global_get_screen_height (global);
  int n_workspaces;
  guint i;
  int w;

  if (self->torn_down)
    return;

  clear_update_region_later (self);

  for (i = 0; i < self->tracked_actors->len; i++)
    {
      TrackedActor *tracked = g_ptr_array_index (self->tracked_actors, i);
      const ShellMonitor *monitor;
      float actor_x, actor_y, actor_width, actor_height;
      int x, y, width, height;
      int x1, x2, y1, y2;
      MetaSide side;
      MetaStrut *strut;

      if (!(tracked->flags & SHELL_CHROME_FLAGS_AFFECTS_STRUTS))
        continue;

      monitor = find_monitor_for_index (self,
                                        shell_layout_manager_find_index_for_actor (self, tracked->actor));
      if (monitor == NULL)
        continue;

      clutter_actor_get_transformed_position (tracked->actor, &actor_x, &actor_y);
      clutter_actor_get_transformed_size (tracked->actor, &actor_width, &actor_height);
      x = js_round (actor_x);
      y = js_round (actor_y);
      width = js_round (actor_width);
      height = js_round (actor_height);

      /* Limit struts to the size of the screen */
      x1 = MAX (x, 0);
      x2 = MIN (x + width, screen_width);
      y1 = MAX (y, 0);
      y2 = MIN (y + height, screen_height);

      /* Metacity wants to know what side of the monitor the strut is
       * considered to be attached to. First, we find the monitor that
       * contains the strut. If the actor is only touching one edge, or is
       * touching the entire border of that monitor, then it's obvious which
       * side to call it. If it's in a corner, we pick a side arbitrarily. If
       * it doesn't touch any edges, or it spans the width/height across the
       * middle of the screen, then we don't create a strut for it at all. */
      if (x1 <= monitor->x && x2 >= monitor->x + monitor->width)
        {
          if (y1 <= monitor->y)
            side = META_SIDE_TOP;
          else if (y2 >= monitor->y + monitor->height)
            side = META_SIDE_BOTTOM;
          else
            continue;
        }
      else if (y1 <= monitor->y && y2 >= monitor->y + monitor->height)
        {
          if (x1 <= monitor->x)
            side = META_SIDE_LEFT;
          else if (x2 >= monitor->x + monitor->width)
            side = META_SIDE_RIGHT;
          else
            continue;
        }
      else if (x1 <= monitor->x)
        {
          side = META_SIDE_LEFT;
        }
      else if (y1 <= monitor->y)
        {
          side = META_SIDE_TOP;
        }
      else if (x2 >= monitor->x + monitor->width)
        {
          side = META_SIDE_RIGHT;
        }
      else if (y2 >= monitor->y + monitor->height)
        {
          side = META_SIDE_BOTTOM;
        }
      else
        {
          continue;
        }

      strut = g_new0 (MetaStrut, 1);
      strut->rect.x = x1;
      strut->rect.y = y1;
      strut->rect.width = x2 - x1;
      strut->rect.height = y2 - y1;
      strut->side = side;
      struts = g_slist_prepend (struts, strut);
    }

  struts = g_slist_reverse (struts);

  n_workspaces = meta_workspace_manager_get_n_workspaces (workspace_manager);
  for (w = 0; w < n_workspaces; w++)
    {
      MetaWorkspace *workspace =
        meta_workspace_manager_get_workspace_by_index (workspace_manager, w);

      meta_workspace_set_builtin_struts (workspace, struts);
    }

  g_slist_free_full (struts, g_free);
}

/* ---- signals of the display and the monitor manager --------------------- */

static void
monitors_changed (ShellLayoutManager *self)
{
  if (self->torn_down)
    return;

  update_monitors (self);
  update_boxes (self);
  update_hot_corners (self);
  update_backgrounds (self);

  /* _updateFullscreen () */
  update_visibility (self);
  queue_update_regions (self);

  g_object_freeze_notify (G_OBJECT (self));
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_N_MONITORS]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PRIMARY_INDEX]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PRIMARY_MONITOR]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BOTTOM_INDEX]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BOTTOM_MONITOR]);
  g_object_thaw_notify (G_OBJECT (self));

  g_signal_emit (self, signals[MONITORS_CHANGED], 0);

  /* The await of _ensurePrimaryMonitor () */
  startup_resume (self);
}

static void
monitors_changed_cb (MetaMonitorManager *monitor_manager,
                     ShellLayoutManager *self)
{
  monitors_changed (self);
}

static void
update_fullscreen_cb (MetaDisplay        *display,
                      ShellLayoutManager *self)
{
  update_visibility (self);
  queue_update_regions (self);
}

static void
n_workspaces_changed_cb (MetaWorkspaceManager *workspace_manager,
                         GParamSpec           *pspec,
                         ShellLayoutManager   *self)
{
  /* Need to update struts on new workspaces when they are added */
  queue_update_regions (self);
}

static void
hot_corners_enabled_changed_cb (GSettings          *settings,
                                const char         *key,
                                ShellLayoutManager *self)
{
  if (self->torn_down)
    return;

  update_hot_corners (self);
}

static void
session_updated_cb (ShellSessionMode   *session_mode,
                    ShellLayoutManager *self)
{
  update_visibility (self);
  queue_update_regions (self);
}

/* ---- startup ------------------------------------------------------------ */

/*
 * Startup Animations
 *
 * We have two different animations, depending on whether we're a greeter or
 * a normal session.
 *
 * In the greeter, we want to animate the panel from the top, and smoothly
 * fade the login dialog on top of whatever plymouth left on screen which we
 * get as a still frame background before drawing anything else.
 *
 * Here we just have the code to animate the panel, and fade up the
 * background. The login dialog animation is handled by modalDialog.js
 *
 * When starting a normal user session, we want to grow it out of the middle
 * of the screen.
 */

static void
startup_animation_complete (ShellLayoutManager *self)
{
  ShellGlobal *global = shell_global_get ();

  if (self->torn_down)
    return;

  if (self->cover_pane != NULL)
    {
      clutter_actor_destroy (self->cover_pane);
      g_clear_object (&self->cover_pane);
    }

  if (self->system_background != NULL)
    {
      clutter_actor_destroy (CLUTTER_ACTOR (self->system_background));
      g_clear_object (&self->system_background);
    }

  self->starting_up = FALSE;
  self->startup_state = STARTUP_DONE;

  clutter_actor_show (CLUTTER_ACTOR (self->keyboard_box));

  if (!is_greeter ())
    {
      show_secondary_backgrounds (self);
      clutter_actor_remove_clip (shell_global_get_window_group (global));
    }

  queue_update_regions (self);

  g_signal_emit (self, signals[STARTUP_COMPLETE], 0);
}

static void
startup_ease_complete_cb (gboolean finished,
                          gpointer user_data)
{
  ShellLayoutManager *self = user_data;

  /* logErrorUnlessCancelled (): a stopped animation ends the startup too */
  if (self->startup_state == STARTUP_ANIMATING)
    startup_animation_complete (self);
}

static void
startup_animation (ShellLayoutManager *self)
{
  ShellEaseParams params = {
    .duration = SHELL_STARTUP_ANIMATION_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  self->startup_state = STARTUP_ANIMATING;

  if (is_greeter ())
    {
      shell_actor_ease (CLUTTER_ACTOR (self->panel_box), &params,
                        startup_ease_complete_cb, g_object_ref (self), g_object_unref,
                        "translation-y", 0.0,
                        NULL);
    }
  else
    {
      gboolean handled = FALSE;

      if (session_mode_get_flag ("has-overview", FALSE))
        g_signal_emit (self, signals[STARTUP_ANIMATION_REQUESTED], 0, &handled);

      if (!handled)
        {
          shell_actor_ease (CLUTTER_ACTOR (self->ui_group), &params,
                            startup_ease_complete_cb, g_object_ref (self), g_object_unref,
                            "scale-x", 1.0,
                            "scale-y", 1.0,
                            "opacity", 255u,
                            NULL);
        }
    }
}

static gboolean
startup_prepared_timeout_cb (gpointer user_data)
{
  ShellLayoutManager *self = user_data;

  self->startup_prepared_timeout_id = 0;
  g_signal_emit (self, signals[STARTUP_PREPARED], 0);

  return G_SOURCE_REMOVE;
}

/* The end of _prepareStartupAnimation (), then _startupAnimation () */
static void
startup_prepared (ShellLayoutManager *self)
{
  /* Hack: Work around grab issue when testing greeter UI in nested */
  if (g_strcmp0 (g_getenv ("GDM_GREETER_TEST"), "1") == 0)
    {
      self->startup_prepared_timeout_id =
        g_timeout_add (200, startup_prepared_timeout_cb, self);
    }
  else
    {
      g_signal_emit (self, signals[STARTUP_PREPARED], 0);
    }

  startup_animation (self);
}

static void
startup_prepare (ShellLayoutManager *self)
{
  ShellGlobal *global = shell_global_get ();
  const ShellMonitor *monitor;
  int screen_width = shell_global_get_screen_width (global);
  int screen_height = shell_global_get_screen_height (global);

  /* await this._ensurePrimaryMonitor () */
  monitor = find_monitor_for_index (self, self->primary_index);
  if (monitor == NULL)
    {
      self->startup_state = STARTUP_WAIT_PRIMARY_PREPARE;
      return;
    }

  /* During the initial transition, add a simple actor to block all events,
   * so they don't get delivered to X11 windows that have been transformed. */
  self->cover_pane = g_object_ref_sink (g_object_new (CLUTTER_TYPE_ACTOR,
                                                      "opacity", 0u,
                                                      "width", (double) screen_width,
                                                      "height", (double) screen_height,
                                                      "reactive", TRUE,
                                                      NULL));
  shell_layout_manager_add_chrome (self, self->cover_pane, SHELL_CHROME_FLAGS_NONE);

  /* Force an update of the regions before we scale the UI group to get the
   * correct allocation for the struts. Do this even when we don't animate on
   * restart, so that maximized windows restore to the right size. */
  update_regions (self);

  if (is_greeter ())
    {
      clutter_actor_set_translation (CLUTTER_ACTOR (self->panel_box), 0,
                                     -clutter_actor_get_height (CLUTTER_ACTOR (self->panel_box)),
                                     0);
    }
  else
    {
      clutter_actor_hide (CLUTTER_ACTOR (self->keyboard_box));

      if (!session_mode_get_flag ("has-overview", FALSE))
        {
          double x = monitor->x + monitor->width / 2.0;
          double y = monitor->y + monitor->height / 2.0;

          clutter_actor_set_pivot_point (CLUTTER_ACTOR (self->ui_group),
                                         x / screen_width, y / screen_height);
          clutter_actor_set_scale (CLUTTER_ACTOR (self->ui_group), 0.75, 0.75);
          clutter_actor_set_opacity (CLUTTER_ACTOR (self->ui_group), 0);
        }

      clutter_actor_set_clip (shell_global_get_window_group (global),
                              monitor->x, monitor->y, monitor->width, monitor->height);

      /* await this._updateBackgrounds () */
      update_backgrounds (self);
      if (!self->bg_loaded)
        {
          self->startup_state = STARTUP_WAIT_BACKGROUNDS;
          return;
        }
    }

  startup_prepared (self);
}

static void
system_background_loaded_cb (ShellSystemBackground *background,
                             ShellLayoutManager    *self)
{
  g_signal_handlers_disconnect_by_func (background, system_background_loaded_cb, self);

  clutter_actor_show (CLUTTER_ACTOR (background));
  clutter_actor_show (CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ())));

  startup_prepare (self);
}

static void
startup_load_background (ShellLayoutManager *self)
{
  ClutterActor *stage = CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ()));

  /* await this._ensurePrimaryMonitor () */
  if (find_monitor_for_index (self, self->primary_index) == NULL)
    {
      self->startup_state = STARTUP_WAIT_PRIMARY_LOAD;
      return;
    }

  self->system_background = g_object_ref_sink (shell_system_background_new ());
  clutter_actor_hide (CLUTTER_ACTOR (self->system_background));

  clutter_actor_insert_child_below (stage, CLUTTER_ACTOR (self->system_background), NULL);

  clutter_actor_add_constraint (CLUTTER_ACTOR (self->system_background),
                                clutter_bind_constraint_new (stage, CLUTTER_BIND_ALL, 0));

  self->startup_state = STARTUP_WAIT_SYSTEM_BACKGROUND;
  g_signal_connect (self->system_background, "loaded",
                    G_CALLBACK (system_background_loaded_cb), self);
}

/* The primary monitor appeared, or the backgrounds loaded */
static void
startup_resume (ShellLayoutManager *self)
{
  if (self->torn_down)
    return;

  switch (self->startup_state)
    {
    case STARTUP_WAIT_PRIMARY_LOAD:
      if (self->primary_index >= 0)
        startup_load_background (self);
      break;

    case STARTUP_WAIT_PRIMARY_PREPARE:
      if (self->primary_index >= 0)
        startup_prepare (self);
      break;

    case STARTUP_WAIT_BACKGROUNDS:
      if (self->bg_loaded)
        startup_prepared (self);
      break;

    case STARTUP_NOT_STARTED:
    case STARTUP_WAIT_SYSTEM_BACKGROUND:
    case STARTUP_ANIMATING:
    case STARTUP_DONE:
    default:
      break;
    }
}

/* ---- shutdown ----------------------------------------------------------- */

static void
on_shutdown (ShellGlobal        *global,
             ShellLayoutManager *self)
{
  ClutterActor *stage = CLUTTER_ACTOR (shell_global_get_stage (global));
  ClutterActor *adopted_actors[4];
  guint i;

  if (self->torn_down)
    return;

  if (self->monitors_changed_id != 0)
    {
      g_signal_handler_disconnect (self->monitor_manager, self->monitors_changed_id);
      self->monitors_changed_id = 0;
    }

  /* Nothing may complete into the shell while it is going away */
  g_clear_handle_id (&self->startup_prepared_timeout_id, g_source_remove);
  clear_update_region_later (self);

  adopted_actors[0] = shell_global_get_window_group (global);
  adopted_actors[1] = shell_global_get_top_window_group (global);
  adopted_actors[2] = self->input_panel_group;
  adopted_actors[3] = meta_compositor_get_feedback_group (shell_global_get_compositor (global));

  for (i = 0; i < G_N_ELEMENTS (adopted_actors); i++)
    {
      g_object_ref (adopted_actors[i]);
      clutter_actor_remove_child (CLUTTER_ACTOR (self->ui_group), adopted_actors[i]);
      clutter_actor_add_child (stage, adopted_actors[i]);
      g_object_unref (adopted_actors[i]);
    }

  destroy_hot_corners (self);
  destroy_panel_barrier (self);
  self->torn_down = TRUE;
  clutter_actor_destroy (CLUTTER_ACTOR (self->ui_group));
}

/* ---- GObject ------------------------------------------------------------ */

static ClutterActor *
new_widget (const char *name)
{
  return g_object_ref_sink (g_object_new (ST_TYPE_WIDGET, "name", name, NULL));
}

static void
shell_layout_manager_constructed (GObject *object)
{
  ShellLayoutManager *self = SHELL_LAYOUT_MANAGER (object);
  ShellGlobal *global = shell_global_get ();
  ClutterStage *stage = shell_global_get_stage (global);
  ClutterActor *stage_actor = CLUTTER_ACTOR (stage);
  ClutterActor *window_group = shell_global_get_window_group (global);
  ClutterActor *top_window_group = shell_global_get_top_window_group (global);
  MetaCompositor *compositor = shell_global_get_compositor (global);
  MetaDisplay *display = shell_global_get_display (global);
  MetaWorkspaceManager *workspace_manager = shell_global_get_workspace_manager (global);
  ClutterActor *feedback_group;

  G_OBJECT_CLASS (shell_layout_manager_parent_class)->constructed (object);

  self->rtl = clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL;

  /* Set up stage hierarchy to group all UI actors under one container. */
  self->ui_group = g_object_ref_sink (g_object_new (SHELL_TYPE_UI_ACTOR,
                                                    "name", "uiGroup",
                                                    NULL));
  clutter_actor_set_no_layout (CLUTTER_ACTOR (self->ui_group), TRUE);

  clutter_actor_add_child (stage_actor, CLUTTER_ACTOR (self->ui_group));

  g_object_ref (window_group);
  clutter_actor_remove_child (stage_actor, window_group);
  clutter_actor_add_child (CLUTTER_ACTOR (self->ui_group), window_group);
  g_object_unref (window_group);

  g_set_weak_pointer (&self->global, global);
  self->shutdown_id = g_signal_connect (global, "shutdown",
                                        G_CALLBACK (on_shutdown), self);

  /* Using addChrome() to add actors to uiGroup will position actors
   * underneath the top_window_group.
   * To insert actors at the top of uiGroup, we use addTopChrome() or add the
   * actor directly using uiGroup.add_child(). */
  g_object_ref (top_window_group);
  clutter_actor_remove_child (stage_actor, top_window_group);
  clutter_actor_add_child (CLUTTER_ACTOR (self->ui_group), top_window_group);
  g_object_unref (top_window_group);

  self->overview_group = ST_WIDGET (new_widget ("overviewGroup"));
  clutter_actor_hide (CLUTTER_ACTOR (self->overview_group));
  clutter_actor_set_reactive (CLUTTER_ACTOR (self->overview_group), TRUE);
  clutter_actor_add_constraint (CLUTTER_ACTOR (self->overview_group),
                                clutter_bind_constraint_new (CLUTTER_ACTOR (self->ui_group),
                                                             CLUTTER_BIND_ALL, 0));
  shell_layout_manager_add_chrome (self, CLUTTER_ACTOR (self->overview_group),
                                   SHELL_CHROME_FLAGS_NONE);

  self->screen_shield_group = ST_WIDGET (new_widget ("screenShieldGroup"));
  clutter_actor_hide (CLUTTER_ACTOR (self->screen_shield_group));
  clutter_actor_set_clip_to_allocation (CLUTTER_ACTOR (self->screen_shield_group), TRUE);
  clutter_actor_set_layout_manager (CLUTTER_ACTOR (self->screen_shield_group),
                                    clutter_bin_layout_new ());
  clutter_actor_add_constraint (CLUTTER_ACTOR (self->screen_shield_group),
                                clutter_bind_constraint_new (CLUTTER_ACTOR (self->ui_group),
                                                             CLUTTER_BIND_ALL, 0));
  shell_layout_manager_add_chrome (self, CLUTTER_ACTOR (self->screen_shield_group),
                                   SHELL_CHROME_FLAGS_NONE);

  self->panel_box = ST_BOX_LAYOUT (g_object_ref_sink (g_object_new (ST_TYPE_BOX_LAYOUT,
                                                                    "name", "panelBox",
                                                                    NULL)));
  st_box_layout_set_orientation (self->panel_box, CLUTTER_ORIENTATION_VERTICAL);
  shell_layout_manager_add_chrome (self, CLUTTER_ACTOR (self->panel_box),
                                   SHELL_CHROME_FLAGS_AFFECTS_STRUTS |
                                   SHELL_CHROME_FLAGS_TRACK_FULLSCREEN);
  g_signal_connect_object (self->panel_box, "notify::allocation",
                           G_CALLBACK (panel_box_allocation_changed_cb), self, 0);

  self->modal_dialog_group = ST_WIDGET (new_widget ("modalDialogGroup"));
  clutter_actor_set_layout_manager (CLUTTER_ACTOR (self->modal_dialog_group),
                                    clutter_bin_layout_new ());
  clutter_actor_add_child (CLUTTER_ACTOR (self->ui_group),
                           CLUTTER_ACTOR (self->modal_dialog_group));

  self->input_panel_group = g_object_ref (meta_compositor_get_input_panel_group (compositor));
  clutter_actor_remove_child (stage_actor, self->input_panel_group);
  clutter_actor_add_child (CLUTTER_ACTOR (self->ui_group), self->input_panel_group);

  self->keyboard_box = ST_BOX_LAYOUT (g_object_ref_sink (g_object_new (ST_TYPE_BOX_LAYOUT,
                                                                       "name", "keyboardBox",
                                                                       "reactive", TRUE,
                                                                       "track-hover", TRUE,
                                                                       NULL)));
  clutter_actor_add_child (self->input_panel_group, CLUTTER_ACTOR (self->keyboard_box));

  self->screenshot_ui_group = ST_WIDGET (new_widget ("screenshotUIGroup"));
  clutter_actor_set_layout_manager (CLUTTER_ACTOR (self->screenshot_ui_group),
                                    clutter_bin_layout_new ());
  shell_layout_manager_add_top_chrome (self, CLUTTER_ACTOR (self->screenshot_ui_group),
                                       SHELL_CHROME_FLAGS_NONE);

  /* A dummy actor that tracks the mouse or text cursor, based on the position
   * and size set in setDummyCursorGeometry. */
  self->dummy_cursor = ST_WIDGET (g_object_ref_sink (g_object_new (ST_TYPE_WIDGET,
                                                                   "width", 0.0,
                                                                   "height", 0.0,
                                                                   "opacity", 0u,
                                                                   "name", "Dummy Cursor",
                                                                   NULL)));
  clutter_actor_add_child (CLUTTER_ACTOR (self->ui_group),
                           CLUTTER_ACTOR (self->dummy_cursor));

  feedback_group = meta_compositor_get_feedback_group (compositor);
  g_object_ref (feedback_group);
  clutter_actor_remove_child (stage_actor, feedback_group);
  clutter_actor_add_child (CLUTTER_ACTOR (self->ui_group), feedback_group);
  g_object_unref (feedback_group);

  self->background_group = g_object_ref_sink (meta_background_group_new ());
  clutter_actor_add_child (window_group, self->background_group);
  clutter_actor_set_child_below_sibling (window_group, self->background_group, NULL);

  self->interface_settings = g_settings_new ("org.gnome.desktop.interface");
  g_signal_connect_object (self->interface_settings, "changed::enable-hot-corners",
                           G_CALLBACK (hot_corners_enabled_changed_cb), self, 0);

  g_signal_connect_object (workspace_manager, "notify::n-workspaces",
                           G_CALLBACK (n_workspaces_changed_cb), self, 0);

  g_signal_connect_object (display, "in-fullscreen-changed",
                           G_CALLBACK (update_fullscreen_cb), self, 0);

  g_set_weak_pointer (&self->monitor_manager,
                      meta_backend_get_monitor_manager (shell_global_get_backend (global)));
  self->monitors_changed_id = g_signal_connect (self->monitor_manager, "monitors-changed",
                                                G_CALLBACK (monitors_changed_cb), self);
  /* Nobody is connected to the signals yet, ShellMain attaches the background
   * menus from shell_layout_manager_begin_startup () */
  monitors_changed (self);

  self->screen_transition = g_object_ref_sink (g_object_new (SHELL_TYPE_SCREEN_TRANSITION, NULL));
  shell_background_manager_set_screen_transition (CLUTTER_ACTOR (self->screen_transition));
  clutter_actor_add_child (CLUTTER_ACTOR (self->ui_group),
                           CLUTTER_ACTOR (self->screen_transition));
  clutter_actor_add_constraint (CLUTTER_ACTOR (self->screen_transition),
                                clutter_bind_constraint_new (CLUTTER_ACTOR (self->ui_group),
                                                             CLUTTER_BIND_ALL, 0));
}

static void
shell_layout_manager_dispose (GObject *object)
{
  ShellLayoutManager *self = SHELL_LAYOUT_MANAGER (object);

  g_clear_handle_id (&self->startup_prepared_timeout_id, g_source_remove);

  /* Normally the shutdown handler ran and the compositor is gone by now */
  if (!self->torn_down)
    {
      self->torn_down = TRUE;
      clear_update_region_later (self);
      destroy_hot_corners (self);
      destroy_panel_barrier (self);
    }

  /* ShellGlobal may already be gone when the JS owner is collected */
  if (self->shutdown_id != 0 && self->global != NULL)
    g_signal_handler_disconnect (self->global, self->shutdown_id);
  self->shutdown_id = 0;
  g_clear_weak_pointer (&self->global);

  if (self->monitors_changed_id != 0 && self->monitor_manager != NULL)
    g_signal_handler_disconnect (self->monitor_manager, self->monitors_changed_id);
  self->monitors_changed_id = 0;
  g_clear_weak_pointer (&self->monitor_manager);

  if (self->session_updated_id != 0 && self->session_mode != NULL)
    g_signal_handler_disconnect (self->session_mode, self->session_updated_id);
  self->session_updated_id = 0;
  g_clear_weak_pointer (&self->session_mode);

  g_clear_pointer (&self->bg_managers, g_ptr_array_unref);
  g_clear_pointer (&self->tracked_actors, g_ptr_array_unref);
  g_clear_pointer (&self->hot_corners, g_ptr_array_unref);

  g_clear_object (&self->system_background);
  g_clear_object (&self->cover_pane);
  g_clear_object (&self->screen_transition);
  g_clear_object (&self->dummy_cursor);
  g_clear_object (&self->screenshot_ui_group);
  g_clear_object (&self->keyboard_box);
  g_clear_object (&self->input_panel_group);
  g_clear_object (&self->modal_dialog_group);
  g_clear_object (&self->panel_box);
  g_clear_object (&self->screen_shield_group);
  g_clear_object (&self->overview_group);
  g_clear_object (&self->background_group);
  g_clear_object (&self->ui_group);
  g_clear_object (&self->interface_settings);

  G_OBJECT_CLASS (shell_layout_manager_parent_class)->dispose (object);
}

static void
shell_layout_manager_finalize (GObject *object)
{
  ShellLayoutManager *self = SHELL_LAYOUT_MANAGER (object);

  g_clear_pointer (&self->monitors, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_layout_manager_parent_class)->finalize (object);
}

static void
shell_layout_manager_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  ShellLayoutManager *self = SHELL_LAYOUT_MANAGER (object);

  switch (prop_id)
    {
    case PROP_UI_GROUP:
      g_value_set_object (value, self->ui_group);
      break;

    case PROP_OVERVIEW_GROUP:
      g_value_set_object (value, self->overview_group);
      break;

    case PROP_SCREEN_SHIELD_GROUP:
      g_value_set_object (value, self->screen_shield_group);
      break;

    case PROP_PANEL_BOX:
      g_value_set_object (value, self->panel_box);
      break;

    case PROP_MODAL_DIALOG_GROUP:
      g_value_set_object (value, self->modal_dialog_group);
      break;

    case PROP_KEYBOARD_BOX:
      g_value_set_object (value, self->keyboard_box);
      break;

    case PROP_SCREENSHOT_UI_GROUP:
      g_value_set_object (value, self->screenshot_ui_group);
      break;

    case PROP_DUMMY_CURSOR:
      g_value_set_object (value, self->dummy_cursor);
      break;

    case PROP_SCREEN_TRANSITION:
      g_value_set_object (value, self->screen_transition);
      break;

    case PROP_N_MONITORS:
      g_value_set_int (value, shell_layout_manager_get_n_monitors (self));
      break;

    case PROP_PRIMARY_INDEX:
      g_value_set_int (value, self->primary_index);
      break;

    case PROP_PRIMARY_MONITOR:
      g_value_take_boxed (value, shell_layout_manager_get_primary_monitor (self));
      break;

    case PROP_BOTTOM_INDEX:
      g_value_set_int (value, self->bottom_index);
      break;

    case PROP_BOTTOM_MONITOR:
      g_value_take_boxed (value, shell_layout_manager_get_bottom_monitor (self));
      break;

    case PROP_FOCUS_INDEX:
      g_value_set_int (value, shell_layout_manager_get_focus_index (self));
      break;

    case PROP_FOCUS_MONITOR:
      g_value_take_boxed (value, shell_layout_manager_get_focus_monitor (self));
      break;

    case PROP_CURRENT_MONITOR:
      g_value_take_boxed (value, shell_layout_manager_get_current_monitor (self));
      break;

    case PROP_KEYBOARD_INDEX:
      g_value_set_int (value, self->keyboard_index);
      break;

    case PROP_KEYBOARD_MONITOR:
      g_value_take_boxed (value, shell_layout_manager_get_keyboard_monitor (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_layout_manager_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  ShellLayoutManager *self = SHELL_LAYOUT_MANAGER (object);

  switch (prop_id)
    {
    case PROP_KEYBOARD_INDEX:
      shell_layout_manager_set_keyboard_index (self, g_value_get_int (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_layout_manager_class_init (ShellLayoutManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamFlags read_flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;

  object_class->constructed = shell_layout_manager_constructed;
  object_class->dispose = shell_layout_manager_dispose;
  object_class->finalize = shell_layout_manager_finalize;
  object_class->get_property = shell_layout_manager_get_property;
  object_class->set_property = shell_layout_manager_set_property;

  /**
   * ShellLayoutManager:ui-group:
   *
   * The container of all the actors of the shell, including the window group
   * (`layoutManager.uiGroup`). It always has the size of the stage.
   */
  props[PROP_UI_GROUP] =
    g_param_spec_object ("ui-group", NULL, NULL,
                         SHELL_TYPE_UI_ACTOR, read_flags);

  /**
   * ShellLayoutManager:overview-group:
   *
   * The container of the overview, hidden outside of it
   * (`layoutManager.overviewGroup`).
   */
  props[PROP_OVERVIEW_GROUP] =
    g_param_spec_object ("overview-group", NULL, NULL,
                         ST_TYPE_WIDGET, read_flags);

  /**
   * ShellLayoutManager:screen-shield-group:
   *
   * The container of the lock screen and the login dialog
   * (`layoutManager.screenShieldGroup`).
   */
  props[PROP_SCREEN_SHIELD_GROUP] =
    g_param_spec_object ("screen-shield-group", NULL, NULL,
                         ST_TYPE_WIDGET, read_flags);

  /**
   * ShellLayoutManager:panel-box:
   *
   * The box of the top bar, on the primary monitor. It affects the struts
   * and is hidden when a fullscreen window is on its monitor
   * (`layoutManager.panelBox`).
   */
  props[PROP_PANEL_BOX] =
    g_param_spec_object ("panel-box", NULL, NULL,
                         ST_TYPE_BOX_LAYOUT, read_flags);

  /**
   * ShellLayoutManager:modal-dialog-group:
   *
   * The container of the modal dialogs (`layoutManager.modalDialogGroup`).
   */
  props[PROP_MODAL_DIALOG_GROUP] =
    g_param_spec_object ("modal-dialog-group", NULL, NULL,
                         ST_TYPE_WIDGET, read_flags);

  /**
   * ShellLayoutManager:keyboard-box:
   *
   * The box of the on-screen keyboard, at the bottom of the keyboard monitor
   * (`layoutManager.keyboardBox`).
   */
  props[PROP_KEYBOARD_BOX] =
    g_param_spec_object ("keyboard-box", NULL, NULL,
                         ST_TYPE_BOX_LAYOUT, read_flags);

  /**
   * ShellLayoutManager:screenshot-ui-group:
   *
   * The container of the screenshot UI, above all windows
   * (`layoutManager.screenshotUIGroup`).
   */
  props[PROP_SCREENSHOT_UI_GROUP] =
    g_param_spec_object ("screenshot-ui-group", NULL, NULL,
                         ST_TYPE_WIDGET, read_flags);

  /**
   * ShellLayoutManager:dummy-cursor:
   *
   * An invisible actor which tracks the mouse or text cursor, to pop up
   * menus at it. See shell_layout_manager_set_dummy_cursor_geometry ()
   * (`layoutManager.dummyCursor`).
   */
  props[PROP_DUMMY_CURSOR] =
    g_param_spec_object ("dummy-cursor", NULL, NULL,
                         ST_TYPE_WIDGET, read_flags);

  /**
   * ShellLayoutManager:screen-transition:
   *
   * The actor which hides a change of the whole screen
   * (`layoutManager.screenTransition`).
   */
  props[PROP_SCREEN_TRANSITION] =
    g_param_spec_object ("screen-transition", NULL, NULL,
                         SHELL_TYPE_SCREEN_TRANSITION, read_flags);

  /**
   * ShellLayoutManager:n-monitors:
   *
   * The number of monitors (`layoutManager.monitors.length`). The monitors
   * are shell_layout_manager_get_monitor ().
   */
  props[PROP_N_MONITORS] =
    g_param_spec_int ("n-monitors", NULL, NULL,
                      0, G_MAXINT, 0,
                      read_flags | G_PARAM_EXPLICIT_NOTIFY);

  /**
   * ShellLayoutManager:primary-index:
   *
   * The index of the primary monitor, -1 without monitors
   * (`layoutManager.primaryIndex`).
   */
  props[PROP_PRIMARY_INDEX] =
    g_param_spec_int ("primary-index", NULL, NULL,
                      -1, G_MAXINT, -1,
                      read_flags | G_PARAM_EXPLICIT_NOTIFY);

  /**
   * ShellLayoutManager:primary-monitor:
   *
   * The primary monitor, %NULL without monitors
   * (`layoutManager.primaryMonitor`).
   */
  props[PROP_PRIMARY_MONITOR] =
    g_param_spec_boxed ("primary-monitor", NULL, NULL,
                        SHELL_TYPE_MONITOR,
                        read_flags | G_PARAM_EXPLICIT_NOTIFY);

  /**
   * ShellLayoutManager:bottom-index:
   *
   * The index of the monitor at the bottom of the monitors above or below the
   * primary monitor, -1 without monitors (`layoutManager.bottomIndex`).
   */
  props[PROP_BOTTOM_INDEX] =
    g_param_spec_int ("bottom-index", NULL, NULL,
                      -1, G_MAXINT, -1,
                      read_flags | G_PARAM_EXPLICIT_NOTIFY);

  /**
   * ShellLayoutManager:bottom-monitor:
   *
   * The monitor of #ShellLayoutManager:bottom-index
   * (`layoutManager.bottomMonitor`).
   */
  props[PROP_BOTTOM_MONITOR] =
    g_param_spec_boxed ("bottom-monitor", NULL, NULL,
                        SHELL_TYPE_MONITOR,
                        read_flags | G_PARAM_EXPLICIT_NOTIFY);

  /**
   * ShellLayoutManager:focus-index:
   *
   * The index of the monitor of the key focus actor, or of the focus window,
   * or the primary monitor (`layoutManager.focusIndex`). Computed on every
   * read, there is no notification.
   */
  props[PROP_FOCUS_INDEX] =
    g_param_spec_int ("focus-index", NULL, NULL,
                      -1, G_MAXINT, -1,
                      read_flags);

  /**
   * ShellLayoutManager:focus-monitor:
   *
   * The monitor of #ShellLayoutManager:focus-index
   * (`layoutManager.focusMonitor`).
   */
  props[PROP_FOCUS_MONITOR] =
    g_param_spec_boxed ("focus-monitor", NULL, NULL,
                        SHELL_TYPE_MONITOR,
                        read_flags);

  /**
   * ShellLayoutManager:current-monitor:
   *
   * The monitor the pointer is on (`layoutManager.currentMonitor`).
   * Computed on every read, there is no notification.
   */
  props[PROP_CURRENT_MONITOR] =
    g_param_spec_boxed ("current-monitor", NULL, NULL,
                        SHELL_TYPE_MONITOR,
                        read_flags);

  /**
   * ShellLayoutManager:keyboard-index:
   *
   * The index of the monitor the on-screen keyboard is on. It follows the
   * primary monitor until the keyboard moves it (`layoutManager.keyboardIndex`).
   */
  props[PROP_KEYBOARD_INDEX] =
    g_param_spec_int ("keyboard-index", NULL, NULL,
                      -1, G_MAXINT, -1,
                      G_PARAM_READWRITE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellLayoutManager:keyboard-monitor:
   *
   * The monitor of #ShellLayoutManager:keyboard-index
   * (`layoutManager.keyboardMonitor`).
   */
  props[PROP_KEYBOARD_MONITOR] =
    g_param_spec_boxed ("keyboard-monitor", NULL, NULL,
                        SHELL_TYPE_MONITOR,
                        read_flags | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellLayoutManager::hot-corners-changed:
   * @self: the #ShellLayoutManager
   *
   * Emitted after the hot corners were rebuilt, or removed.
   */
  signals[HOT_CORNERS_CHANGED] =
    g_signal_new ("hot-corners-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellLayoutManager::startup-complete:
   * @self: the #ShellLayoutManager
   *
   * Emitted when the startup animation is over.
   */
  signals[STARTUP_COMPLETE] =
    g_signal_new ("startup-complete",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellLayoutManager::startup-prepared:
   * @self: the #ShellLayoutManager
   *
   * Emitted when the stage is ready for the startup animation, which starts
   * right after.
   */
  signals[STARTUP_PREPARED] =
    g_signal_new ("startup-prepared",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellLayoutManager::monitors-changed:
   * @self: the #ShellLayoutManager
   *
   * Emitted after the monitors changed and the layout manager updated
   * itself.
   */
  signals[MONITORS_CHANGED] =
    g_signal_new ("monitors-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellLayoutManager::system-modal-opened:
   * @self: the #ShellLayoutManager
   *
   * Emitted when a system modal dialog opens. See
   * shell_layout_manager_system_modal_opened ().
   */
  signals[SYSTEM_MODAL_OPENED] =
    g_signal_new ("system-modal-opened",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellLayoutManager::background-added:
   * @self: the #ShellLayoutManager
   * @background_actor: the new background actor of a monitor
   * @monitor_index: the index of the monitor
   *
   * Emitted when the background of a monitor was created, or replaced because
   * the user changed it. ShellMain adds the background menu to it
   * (`Shell.BackgroundMenu.add_to ()`). The backgrounds which exist
   * before ShellMain connects are announced again by
   * shell_layout_manager_begin_startup ().
   */
  signals[BACKGROUND_ADDED] =
    g_signal_new ("background-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  CLUTTER_TYPE_ACTOR, G_TYPE_INT);

  /**
   * ShellLayoutManager::hot-corner-activated:
   * @self: the #ShellLayoutManager
   * @monitor_index: the monitor of the hot corner
   * @in_fullscreen: whether a fullscreen window is on the monitor
   *
   * Emitted when the pointer pressed enough against a hot corner. ShellMain
   * toggles the overview (`Main.overview.toggle ()`), unless it is not
   * supposed to be by a corner (a fullscreen window and the overview is not
   * visible, or `shouldToggleByCornerOrButton ()` is false).
   *
   * Returns: %TRUE if the overview started to animate, in which case the
   *   ripples of the hot corner play. Without a handler the corner does
   *   nothing.
   */
  signals[HOT_CORNER_ACTIVATED] =
    g_signal_new ("hot-corner-activated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, g_signal_accumulator_true_handled, NULL, NULL,
                  G_TYPE_BOOLEAN, 2,
                  G_TYPE_INT, G_TYPE_BOOLEAN);

  /**
   * ShellLayoutManager::startup-animation-requested:
   * @self: the #ShellLayoutManager
   *
   * Emitted at the end of the startup of a session which has an overview
   * (and is not the greeter), instead of the default animation. A handler
   * runs the animation of the overview (`Main.overview.runStartupAnimation ()`)
   * and calls shell_layout_manager_startup_animation_finished () when it
   * ended.
   *
   * Returns: %TRUE if a handler took over the animation. Without one the
   *   default animation, which grows the UI out of the middle of the screen,
   *   runs.
   */
  signals[STARTUP_ANIMATION_REQUESTED] =
    g_signal_new ("startup-animation-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, g_signal_accumulator_true_handled, NULL, NULL,
                  G_TYPE_BOOLEAN, 0);
}

static void
shell_layout_manager_init (ShellLayoutManager *self)
{
  self->monitors = g_ptr_array_new_with_free_func ((GDestroyNotify) shell_monitor_free);
  self->primary_index = -1;
  self->bottom_index = -1;
  self->keyboard_index = -1;
  self->hot_corners = g_ptr_array_new_with_free_func ((GDestroyNotify) hot_corner_free);
  self->tracked_actors = g_ptr_array_new_with_free_func ((GDestroyNotify) tracked_actor_free);
  self->bg_managers = g_ptr_array_new_with_free_func ((GDestroyNotify) bg_manager_free);
  self->starting_up = TRUE;
  self->startup_state = STARTUP_NOT_STARTED;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

/**
 * shell_layout_manager_new:
 *
 * Creates the stage hierarchy: moves the window groups of the compositor
 * into #ShellLayoutManager:ui-group and creates the chrome actors. The
 * backgrounds are created, but the startup only begins with
 * shell_layout_manager_begin_startup ().
 *
 * There is a single layout manager, created by ShellMain.
 *
 * Returns: (transfer full): a new #ShellLayoutManager
 */
ShellLayoutManager *
shell_layout_manager_new (void)
{
  return g_object_new (SHELL_TYPE_LAYOUT_MANAGER, NULL);
}

/**
 * shell_layout_manager_begin_startup:
 * @self: a #ShellLayoutManager
 *
 * Starts following the session mode and runs the startup sequence: load the
 * background, emit #ShellLayoutManager::startup-prepared, run the startup
 * animation and emit #ShellLayoutManager::startup-complete. ShellMain calls
 * this after everything else is constructed and connected (`layoutManager.init ()`
 * of old; the name `init` is taken by GObject).
 *
 * The backgrounds which already exist are announced with
 * #ShellLayoutManager::background-added, as nobody could be connected when
 * they were created.
 */
void
shell_layout_manager_begin_startup (ShellLayoutManager *self)
{
  ShellSessionMode *session_mode;
  guint i;

  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));
  g_return_if_fail (self->startup_state == STARTUP_NOT_STARTED);

  session_mode = shell_main_get_session_mode (shell_main_get_default ());
  if (session_mode != NULL && self->session_mode == NULL)
    {
      g_set_weak_pointer (&self->session_mode, session_mode);
      self->session_updated_id = g_signal_connect (session_mode, "updated",
                                                   G_CALLBACK (session_updated_cb), self);
    }

  for (i = 0; i < self->bg_managers->len; i++)
    emit_background_added (self, g_ptr_array_index (self->bg_managers, i));

  startup_load_background (self);
}

/**
 * shell_layout_manager_startup_animation_finished:
 * @self: a #ShellLayoutManager
 *
 * To be called when the animation started by
 * #ShellLayoutManager::startup-animation-requested ended, whether it was
 * successful or not. Completes the startup.
 */
void
shell_layout_manager_startup_animation_finished (ShellLayoutManager *self)
{
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));

  if (self->startup_state == STARTUP_ANIMATING)
    startup_animation_complete (self);
}

/**
 * shell_layout_manager_show_overview:
 * @self: a #ShellLayoutManager
 *
 * Shows #ShellLayoutManager:overview-group and hides the screen transition.
 * Called by the overview.
 */
void
shell_layout_manager_show_overview (ShellLayoutManager *self)
{
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));

  clutter_actor_show (CLUTTER_ACTOR (self->overview_group));
  clutter_actor_hide (CLUTTER_ACTOR (self->screen_transition));

  self->in_overview = TRUE;
  update_visibility (self);
}

/**
 * shell_layout_manager_hide_overview:
 * @self: a #ShellLayoutManager
 *
 * Hides #ShellLayoutManager:overview-group and the screen transition.
 * Called by the overview.
 */
void
shell_layout_manager_hide_overview (ShellLayoutManager *self)
{
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));

  clutter_actor_hide (CLUTTER_ACTOR (self->overview_group));
  clutter_actor_hide (CLUTTER_ACTOR (self->screen_transition));

  self->in_overview = FALSE;
  update_visibility (self);
}

/**
 * shell_layout_manager_modal_ended:
 * @self: a #ShellLayoutManager
 *
 * Called by ShellMain when the last modal ended, to update the regions
 * ("We don't update the stage input region while in a modal").
 */
void
shell_layout_manager_modal_ended (ShellLayoutManager *self)
{
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));

  queue_update_regions (self);
}

/**
 * shell_layout_manager_system_modal_opened:
 * @self: a #ShellLayoutManager
 *
 * Emits #ShellLayoutManager::system-modal-opened, as the modal dialogs and
 * the screenshot UI do when they open (`layoutManager.emit ('system-modal-opened')`).
 */
void
shell_layout_manager_system_modal_opened (ShellLayoutManager *self)
{
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));

  g_signal_emit (self, signals[SYSTEM_MODAL_OPENED], 0);
}

/**
 * shell_layout_manager_get_ui_group:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer none): #ShellLayoutManager:ui-group
 */
ShellUiActor *
shell_layout_manager_get_ui_group (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return self->ui_group;
}

/**
 * shell_layout_manager_get_overview_group:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer none): #ShellLayoutManager:overview-group
 */
StWidget *
shell_layout_manager_get_overview_group (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return self->overview_group;
}

/**
 * shell_layout_manager_get_screen_shield_group:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer none): #ShellLayoutManager:screen-shield-group
 */
StWidget *
shell_layout_manager_get_screen_shield_group (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return self->screen_shield_group;
}

/**
 * shell_layout_manager_get_panel_box:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer none): #ShellLayoutManager:panel-box
 */
StBoxLayout *
shell_layout_manager_get_panel_box (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return self->panel_box;
}

/**
 * shell_layout_manager_get_modal_dialog_group:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer none): #ShellLayoutManager:modal-dialog-group
 */
StWidget *
shell_layout_manager_get_modal_dialog_group (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return self->modal_dialog_group;
}

/**
 * shell_layout_manager_get_keyboard_box:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer none): #ShellLayoutManager:keyboard-box
 */
StBoxLayout *
shell_layout_manager_get_keyboard_box (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return self->keyboard_box;
}

/**
 * shell_layout_manager_get_screenshot_ui_group:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer none): #ShellLayoutManager:screenshot-ui-group
 */
StWidget *
shell_layout_manager_get_screenshot_ui_group (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return self->screenshot_ui_group;
}

/**
 * shell_layout_manager_get_dummy_cursor:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer none): #ShellLayoutManager:dummy-cursor
 */
StWidget *
shell_layout_manager_get_dummy_cursor (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return self->dummy_cursor;
}

/**
 * shell_layout_manager_get_screen_transition:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer none): #ShellLayoutManager:screen-transition
 */
ShellScreenTransition *
shell_layout_manager_get_screen_transition (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return self->screen_transition;
}

/**
 * shell_layout_manager_get_n_monitors:
 * @self: a #ShellLayoutManager
 *
 * Returns: the number of monitors (`layoutManager.monitors.length`)
 */
int
shell_layout_manager_get_n_monitors (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), 0);

  return self->monitors->len;
}

/**
 * shell_layout_manager_get_monitor:
 * @self: a #ShellLayoutManager
 * @index: the index of a monitor
 *
 * Returns the monitor as it was when the layout manager last saw the
 * monitors change (`layoutManager.monitors[index]`).
 *
 * Returns: (transfer full) (nullable): the monitor, or %NULL if there is no
 *   such monitor
 */
ShellMonitor *
shell_layout_manager_get_monitor (ShellLayoutManager *self,
                                  int                 index)
{
  ShellMonitor *monitor;

  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  monitor = find_monitor_for_index (self, index);

  return monitor != NULL ? shell_monitor_copy (monitor) : NULL;
}

/**
 * shell_layout_manager_get_primary_index:
 * @self: a #ShellLayoutManager
 *
 * Returns: #ShellLayoutManager:primary-index
 */
int
shell_layout_manager_get_primary_index (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), -1);

  return self->primary_index;
}

/**
 * shell_layout_manager_get_primary_monitor:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer full) (nullable): #ShellLayoutManager:primary-monitor
 */
ShellMonitor *
shell_layout_manager_get_primary_monitor (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return shell_layout_manager_get_monitor (self, self->primary_index);
}

/**
 * shell_layout_manager_get_bottom_index:
 * @self: a #ShellLayoutManager
 *
 * Returns: #ShellLayoutManager:bottom-index
 */
int
shell_layout_manager_get_bottom_index (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), -1);

  return self->bottom_index;
}

/**
 * shell_layout_manager_get_bottom_monitor:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer full) (nullable): #ShellLayoutManager:bottom-monitor
 */
ShellMonitor *
shell_layout_manager_get_bottom_monitor (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return shell_layout_manager_get_monitor (self, self->bottom_index);
}

/**
 * shell_layout_manager_get_focus_index:
 * @self: a #ShellLayoutManager
 *
 * Returns: #ShellLayoutManager:focus-index
 */
int
shell_layout_manager_get_focus_index (ShellLayoutManager *self)
{
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());
  ClutterActor *key_focus;
  MetaWindow *focus_window;
  int i;

  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), -1);

  i = self->primary_index;

  /* The stage is the key focus when no actor has it, so this is the
   * only branch taken; the other one is kept as the JS had it. */
  key_focus = clutter_stage_get_key_focus (stage);
  if (key_focus != NULL)
    {
      i = shell_layout_manager_find_index_for_actor (self, key_focus);
    }
  else
    {
      focus_window = meta_display_get_focus_window (get_display ());
      if (focus_window != NULL)
        i = meta_window_get_monitor (focus_window);
    }

  return i;
}

/**
 * shell_layout_manager_get_focus_monitor:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer full) (nullable): #ShellLayoutManager:focus-monitor
 */
ShellMonitor *
shell_layout_manager_get_focus_monitor (ShellLayoutManager *self)
{
  int focus_index;

  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  focus_index = shell_layout_manager_get_focus_index (self);
  if (focus_index < 0)
    return NULL;

  return shell_layout_manager_get_monitor (self, focus_index);
}

/**
 * shell_layout_manager_get_current_monitor:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer full) (nullable): #ShellLayoutManager:current-monitor
 */
ShellMonitor *
shell_layout_manager_get_current_monitor (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return shell_layout_manager_get_monitor (self, meta_display_get_current_monitor (get_display ()));
}

/**
 * shell_layout_manager_get_keyboard_index:
 * @self: a #ShellLayoutManager
 *
 * Returns: #ShellLayoutManager:keyboard-index
 */
int
shell_layout_manager_get_keyboard_index (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), -1);

  return self->keyboard_index;
}

/**
 * shell_layout_manager_set_keyboard_index:
 * @self: a #ShellLayoutManager
 * @index: the index of a monitor
 *
 * Moves the box of the on-screen keyboard to the bottom of the monitor.
 */
void
shell_layout_manager_set_keyboard_index (ShellLayoutManager *self,
                                         int                 index)
{
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));

  self->keyboard_index = index;
  update_keyboard_box (self);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_KEYBOARD_INDEX]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_KEYBOARD_MONITOR]);
}

/**
 * shell_layout_manager_get_keyboard_monitor:
 * @self: a #ShellLayoutManager
 *
 * Returns: (transfer full) (nullable): #ShellLayoutManager:keyboard-monitor
 */
ShellMonitor *
shell_layout_manager_get_keyboard_monitor (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return shell_layout_manager_get_monitor (self, self->keyboard_index);
}

/**
 * shell_layout_manager_find_index_for_actor:
 * @self: a #ShellLayoutManager
 * @actor: an actor
 *
 * Returns the index of the monitor which @actor is mostly on. This
 * guarantees that some monitor is returned to simplify usage of it, as long
 * as there are monitors: in practice all tracked actors should be visible on
 * some monitor anyway.
 *
 * Returns: the index of a monitor, or -1 without monitors
 */
int
shell_layout_manager_find_index_for_actor (ShellLayoutManager *self,
                                           ClutterActor       *actor)
{
  float x, y, width, height;

  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), -1);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), -1);

  clutter_actor_get_transformed_position (actor, &x, &y);
  clutter_actor_get_transformed_size (actor, &width, &height);

  return find_index_for_rect (x, y, width, height);
}

/**
 * shell_layout_manager_find_monitor_for_actor:
 * @self: a #ShellLayoutManager
 * @actor: an actor
 *
 * Returns: (transfer full) (nullable): the monitor which @actor is mostly on
 */
ShellMonitor *
shell_layout_manager_find_monitor_for_actor (ShellLayoutManager *self,
                                             ClutterActor       *actor)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), NULL);

  return shell_layout_manager_get_monitor (self,
                                           shell_layout_manager_find_index_for_actor (self, actor));
}

/**
 * shell_layout_manager_find_monitor_for_point:
 * @self: a #ShellLayoutManager
 * @x: x coordinate in the stage
 * @y: y coordinate in the stage
 *
 * Returns: (transfer full) (nullable): the monitor which the point is on
 */
ShellMonitor *
shell_layout_manager_find_monitor_for_point (ShellLayoutManager *self,
                                             double              x,
                                             double              y)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), NULL);

  return shell_layout_manager_get_monitor (self, find_index_for_rect (x, y, 1, 1));
}

/**
 * shell_layout_manager_get_work_area_for_monitor:
 * @self: a #ShellLayoutManager
 * @monitor_index: the index of a monitor
 * @work_area: (out caller-allocates): the work area of the monitor, which is
 *   the monitor without the struts
 *
 * Gets the work area of the first workspace, assuming that all workspaces
 * have the same struts.
 */
void
shell_layout_manager_get_work_area_for_monitor (ShellLayoutManager *self,
                                                int                 monitor_index,
                                                MtkRectangle       *work_area)
{
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));
  g_return_if_fail (work_area != NULL);

  get_display_work_area_for_monitor (get_display (), monitor_index, work_area);
}

/**
 * shell_layout_manager_add_chrome:
 * @self: a #ShellLayoutManager
 * @actor: an actor to add to the chrome
 * @flags: how to track @actor
 *
 * Adds @actor to the chrome, below the top window group (the popup windows).
 * Changes in @actor's size, position, and visibility result in appropriate
 * updates of the struts, see #ShellChromeFlags.
 */
void
shell_layout_manager_add_chrome (ShellLayoutManager *self,
                                 ClutterActor       *actor,
                                 ShellChromeFlags    flags)
{
  ClutterActor *ui_group;
  ClutterActor *top_window_group;

  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  ui_group = CLUTTER_ACTOR (self->ui_group);
  top_window_group = shell_global_get_top_window_group (shell_global_get ());

  clutter_actor_add_child (ui_group, actor);
  if (clutter_actor_contains (ui_group, top_window_group))
    clutter_actor_set_child_below_sibling (ui_group, actor, top_window_group);
  track_actor (self, actor, flags);
}

/**
 * shell_layout_manager_add_top_chrome:
 * @self: a #ShellLayoutManager
 * @actor: an actor to add to the chrome
 * @flags: how to track @actor
 *
 * Like shell_layout_manager_add_chrome (), but adds @actor above all windows,
 * including popups.
 */
void
shell_layout_manager_add_top_chrome (ShellLayoutManager *self,
                                     ClutterActor       *actor,
                                     ShellChromeFlags    flags)
{
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  clutter_actor_add_child (CLUTTER_ACTOR (self->ui_group), actor);
  track_actor (self, actor, flags);
}

/**
 * shell_layout_manager_track_chrome:
 * @self: a #ShellLayoutManager
 * @actor: a descendant of the chrome to begin tracking
 *
 * Tells the chrome to track @actor. This can be used to extend the struts or
 * input region to cover specific children. @actor is tracked with the flags
 * of its closest tracked ancestor.
 */
void
shell_layout_manager_track_chrome (ShellLayoutManager *self,
                                   ClutterActor       *actor)
{
  ClutterActor *ancestor;
  int index;
  ShellChromeFlags flags = SHELL_CHROME_FLAGS_NONE;

  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  ancestor = clutter_actor_get_parent (actor);
  index = find_tracked_actor (self, ancestor);
  while (ancestor != NULL && index == -1)
    {
      ancestor = clutter_actor_get_parent (ancestor);
      index = find_tracked_actor (self, ancestor);
    }

  if (ancestor != NULL)
    {
      TrackedActor *ancestor_data = g_ptr_array_index (self->tracked_actors, index);

      flags = ancestor_data->flags;
    }

  track_actor (self, actor, flags);
}

/**
 * shell_layout_manager_track_chrome_with_flags:
 * @self: a #ShellLayoutManager
 * @actor: a descendant of the chrome to begin tracking
 * @flags: how to track @actor, instead of the flags of its tracked ancestor
 *
 * Like shell_layout_manager_track_chrome (), with explicit flags.
 */
void
shell_layout_manager_track_chrome_with_flags (ShellLayoutManager *self,
                                              ClutterActor       *actor,
                                              ShellChromeFlags    flags)
{
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  track_actor (self, actor, flags);
}

/**
 * shell_layout_manager_untrack_chrome:
 * @self: a #ShellLayoutManager
 * @actor: an actor previously tracked via shell_layout_manager_track_chrome ()
 *
 * Undoes the effect of shell_layout_manager_track_chrome ().
 */
void
shell_layout_manager_untrack_chrome (ShellLayoutManager *self,
                                     ClutterActor       *actor)
{
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  untrack_actor (self, actor);
}

/**
 * shell_layout_manager_remove_chrome:
 * @self: a #ShellLayoutManager
 * @actor: a chrome actor
 *
 * Removes @actor from the chrome.
 */
void
shell_layout_manager_remove_chrome (ShellLayoutManager *self,
                                    ClutterActor       *actor)
{
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  /* Removing the actor may drop the last reference to it */
  g_object_ref (actor);
  untrack_actor (self, actor);
  clutter_actor_remove_child (CLUTTER_ACTOR (self->ui_group), actor);
  g_object_unref (actor);
}

/**
 * shell_layout_manager_set_dummy_cursor_geometry:
 * @self: a #ShellLayoutManager
 * @x: x coordinate of the cursor
 * @y: y coordinate of the cursor
 * @width: width of the cursor
 * @height: height of the cursor
 *
 * The cursor dummy is a standard widget commonly used for popup menus and box
 * pointers to track, as the box pointer API only tracks actors. If you want
 * to pop up a menu based on where the user clicked, or where the text cursor
 * is, the cursor dummy is what you should use. Given that the menu should not
 * track the actual mouse pointer as it moves, you need to call this function
 * before you show the menu to ensure it is at the right position and has the
 * right size.
 */
void
shell_layout_manager_set_dummy_cursor_geometry (ShellLayoutManager *self,
                                                double              x,
                                                double              y,
                                                double              width,
                                                double              height)
{
  g_return_if_fail (SHELL_IS_LAYOUT_MANAGER (self));

  clutter_actor_set_position (CLUTTER_ACTOR (self->dummy_cursor), js_round (x), js_round (y));
  clutter_actor_set_size (CLUTTER_ACTOR (self->dummy_cursor), js_round (width), js_round (height));
}

/**
 * shell_layout_manager_get_n_background_managers:
 * @self: a #ShellLayoutManager
 *
 * Returns: the number of background managers, one per monitor outside of the
 *   greeter (`layoutManager._bgManagers.length`, for the tests)
 */
int
shell_layout_manager_get_n_background_managers (ShellLayoutManager *self)
{
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (self), 0);

  return self->bg_managers->len;
}
