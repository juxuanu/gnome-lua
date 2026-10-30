/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * overview-controls.c: The controls of the overview. Port of
 * js/ui/overviewControls.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The bridges of the JS module to the overview are gone: the controls
 *    connect to the #ShellOverview they are created with (its window drags,
 *    `showing` and `hiding`), the drags of items are handled by the overview
 *    itself, and the search controller asks the overview to hide or toggle
 *    directly. connectThumbnailsBox () is shell_controls_connect_thumbnails_box ().
 *  - `animateToOverview ()`, `animateFromOverview ()`, `gestureEnd ()` and
 *    `runStartupAnimation ()` take a callback and its data instead of
 *    returning a promise. The startup animation calls its callback after every
 *    animation it started ended (`Promise.allSettled ()`).
 *  - `ControlsManagerLayout.ensureAllocation ()` takes a callback instead of
 *    returning a promise.
 *  - A state of the adjustment which is not a whole number is rounded when
 *    the layout looks up the box of the workspaces; the JS looked up the
 *    fractional state in a Map and failed.
 */

#include "config.h"

#include <math.h>

#include <glib/gi18n-lib.h>

#include "shell-global.h"
#include "ui/app-display.h"
#include "ui/environment.h"
#include "ui/main.h"
#include "ui/overview-controls.h"
#include "ui/workspaces-view.h"

#define DASH_MAX_HEIGHT_RATIO 0.16
#define VERTICAL_SPACING_RATIO 0.02
#define THUMBNAILS_SPACING_ADJUSTMENT_TOP 0.6
#define THUMBNAILS_SPACING_ADJUSTMENT_BOTTOM 0.4

#define A11Y_SCHEMA "org.gnome.desktop.a11y.keyboard"
#define SHELL_KEYBINDINGS_SCHEMA "org.gnome.shell.keybindings"

#define N_STATES 3

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ShellMonitor, shell_monitor_free)

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static double
lerp (double start,
      double end,
      double progress)
{
  return start + progress * (end - start);
}

/* Math.round () */
static double
js_round (double value)
{
  return floor (value + 0.5);
}

static GWeakRef *
weak_ref_new (gpointer object)
{
  GWeakRef *ref = g_new0 (GWeakRef, 1);

  g_weak_ref_init (ref, object);

  return ref;
}

static void
weak_ref_free (gpointer data)
{
  GWeakRef *ref = data;

  g_weak_ref_clear (ref);
  g_free (ref);
}

/* A weak reference to an object and a flag */
typedef struct
{
  GWeakRef ref;
  gboolean flag;
} WeakFlag;

static WeakFlag *
weak_flag_new (gpointer  object,
               gboolean  flag)
{
  WeakFlag *data = g_new0 (WeakFlag, 1);

  g_weak_ref_init (&data->ref, object);
  data->flag = flag;

  return data;
}

static void
weak_flag_free (gpointer user_data)
{
  WeakFlag *data = user_data;

  g_weak_ref_clear (&data->ref);
  g_free (data);
}

static ShellEaseParams
ease_params (int                  duration,
             ClutterAnimationMode mode)
{
  ShellEaseParams params = {
    .duration = duration,
    .delay = -1,
    .mode = mode,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  return params;
}

/* adjustment.ease (target, {mode, duration, onComplete / onStopped}) */
static void
ease_adjustment (StAdjustment         *adjustment,
                 double                target,
                 int                   duration,
                 ClutterAnimationMode  mode,
                 ShellEaseCallback     callback,
                 gpointer              user_data,
                 GDestroyNotify        destroy)
{
  ShellEaseParams params = ease_params (duration, mode);
  g_auto (GValue) value = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;

  g_value_init (&value, G_TYPE_DOUBLE);
  g_value_set_double (&value, target);

  /* On failure the callback is dropped and the data freed */
  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (adjustment), "value", &value,
                                       &params, callback, user_data, destroy, &error))
    g_debug ("%s: %s", G_STRFUNC, error->message);
}

static void
set_actor_visible (ClutterActor *actor,
                   gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

static double
get_fit_mode_for_state (double state)
{
  switch ((int) state)
    {
    case SHELL_CONTROLS_STATE_APP_GRID:
      return SHELL_FIT_MODE_ALL;
    case SHELL_CONTROLS_STATE_HIDDEN:
    case SHELL_CONTROLS_STATE_WINDOW_PICKER:
    default:
      return SHELL_FIT_MODE_SINGLE;
    }
}

/* ------------------------------------------------------------------------ */
/* ShellOverviewAdjustment                                                   */
/* ------------------------------------------------------------------------ */

enum
{
  ADJ_PROP_0,

  ADJ_PROP_GESTURE_IN_PROGRESS,

  ADJ_N_PROPS
};

static GParamSpec *adj_props[ADJ_N_PROPS] = { NULL, };

struct _ShellOverviewAdjustment
{
  StAdjustment parent;

  gboolean gesture_in_progress;
};

G_DEFINE_FINAL_TYPE (ShellOverviewAdjustment, shell_overview_adjustment, ST_TYPE_ADJUSTMENT)

static void
shell_overview_adjustment_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellOverviewAdjustment *self = SHELL_OVERVIEW_ADJUSTMENT (object);

  switch (prop_id)
    {
    case ADJ_PROP_GESTURE_IN_PROGRESS:
      g_value_set_boolean (value, self->gesture_in_progress);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_overview_adjustment_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellOverviewAdjustment *self = SHELL_OVERVIEW_ADJUSTMENT (object);

  switch (prop_id)
    {
    case ADJ_PROP_GESTURE_IN_PROGRESS:
      shell_overview_adjustment_set_gesture_in_progress (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_overview_adjustment_class_init (ShellOverviewAdjustmentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_overview_adjustment_get_property;
  object_class->set_property = shell_overview_adjustment_set_property;

  /**
   * ShellOverviewAdjustment:gesture-in-progress:
   *
   * Whether a gesture is changing the value of the adjustment, in which case
   * the state of the overview is in transition without a running animation.
   */
  adj_props[ADJ_PROP_GESTURE_IN_PROGRESS] =
    g_param_spec_boolean ("gesture-in-progress", NULL, NULL, FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, ADJ_N_PROPS, adj_props);
}

static void
shell_overview_adjustment_init (ShellOverviewAdjustment *self)
{
}

/**
 * shell_overview_adjustment_new:
 * @actor: the actor the adjustment is animated through
 *
 * Creates the adjustment with the #ShellControlsState of the overview, which
 * starts at %SHELL_CONTROLS_STATE_WINDOW_PICKER.
 *
 * Returns: (transfer full): a new #ShellOverviewAdjustment
 */
ShellOverviewAdjustment *
shell_overview_adjustment_new (ClutterActor *actor)
{
  ShellOverviewAdjustment *self = g_object_new (SHELL_TYPE_OVERVIEW_ADJUSTMENT,
                                                "actor", actor,
                                                NULL);

  /* The value is clamped to the bounds, which have to be set first */
  st_adjustment_set_values (ST_ADJUSTMENT (self),
                            SHELL_CONTROLS_STATE_WINDOW_PICKER,
                            SHELL_CONTROLS_STATE_HIDDEN,
                            SHELL_CONTROLS_STATE_APP_GRID,
                            0, 0, 0);

  return self;
}

/**
 * shell_overview_adjustment_get_gesture_in_progress:
 * @self: a #ShellOverviewAdjustment
 *
 * Returns: the value of #ShellOverviewAdjustment:gesture-in-progress
 */
gboolean
shell_overview_adjustment_get_gesture_in_progress (ShellOverviewAdjustment *self)
{
  g_return_val_if_fail (SHELL_IS_OVERVIEW_ADJUSTMENT (self), FALSE);

  return self->gesture_in_progress;
}

/**
 * shell_overview_adjustment_set_gesture_in_progress:
 * @self: a #ShellOverviewAdjustment
 * @gesture_in_progress: whether a gesture changes the value
 *
 * Sets #ShellOverviewAdjustment:gesture-in-progress.
 */
void
shell_overview_adjustment_set_gesture_in_progress (ShellOverviewAdjustment *self,
                                                   gboolean                gesture_in_progress)
{
  g_return_if_fail (SHELL_IS_OVERVIEW_ADJUSTMENT (self));

  if (self->gesture_in_progress == gesture_in_progress)
    return;

  self->gesture_in_progress = gesture_in_progress;
  g_object_notify_by_pspec (G_OBJECT (self), adj_props[ADJ_PROP_GESTURE_IN_PROGRESS]);
}

/**
 * shell_overview_adjustment_get_state_transition_params: (skip)
 * @self: a #ShellOverviewAdjustment
 * @params: (out caller-allocates): the state of the transition
 *
 * Gets the states the overview is in transition between
 * (`getStateTransitionParams ()`). The initial and final states are rounded
 * to the states they lie between.
 */
void
shell_overview_adjustment_get_state_transition_params (ShellOverviewAdjustment    *self,
                                                       ShellStateTransitionParams *params)
{
  ClutterTransition *transition;
  double current_state, initial_state, final_state, length;

  g_return_if_fail (SHELL_IS_OVERVIEW_ADJUSTMENT (self));
  g_return_if_fail (params != NULL);

  current_state = st_adjustment_get_value (ST_ADJUSTMENT (self));
  initial_state = current_state;
  final_state = current_state;

  transition = st_adjustment_get_transition (ST_ADJUSTMENT (self), "value");
  if (transition != NULL)
    {
      ClutterInterval *interval = clutter_transition_get_interval (transition);

      if (interval != NULL)
        {
          const GValue *initial_value = clutter_interval_peek_initial_value (interval);
          const GValue *final_value = clutter_interval_peek_final_value (interval);

          if (initial_value != NULL && G_VALUE_HOLDS_DOUBLE (initial_value))
            initial_state = g_value_get_double (initial_value);
          if (final_value != NULL && G_VALUE_HOLDS_DOUBLE (final_value))
            final_state = g_value_get_double (final_value);
        }
    }

  if (initial_state > final_state)
    {
      initial_state = ceil (initial_state);
      final_state = floor (final_state);
    }
  else
    {
      initial_state = floor (initial_state);
      final_state = ceil (final_state);
    }

  length = fabs (final_state - initial_state);

  params->transitioning = transition != NULL || self->gesture_in_progress;
  params->current_state = current_state;
  params->initial_state = initial_state;
  params->final_state = final_state;
  params->progress = length > 0 ? fabs ((current_state - initial_state) / length) : 1;
}

/* ------------------------------------------------------------------------ */
/* ShellControlsLayout (private; ControlsManagerLayout)                      */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellControlsCallback callback;
  gpointer user_data;
  GDestroyNotify destroy;
} PostAllocation;

#define SHELL_TYPE_CONTROLS_LAYOUT (shell_controls_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellControlsLayout, shell_controls_layout,
                      SHELL, CONTROLS_LAYOUT, ClutterLayoutManager)

struct _ShellControlsLayout
{
  ClutterLayoutManager parent;

  ClutterActor *search_entry;           /* weak */
  ClutterActor *app_display;            /* weak */
  ClutterActor *workspaces_display;     /* weak */
  ClutterActor *workspaces_thumbnails;  /* weak */
  ClutterActor *search_controller;      /* weak */
  ClutterActor *dash;                   /* weak */
  ShellOverviewAdjustment *state_adjustment; /* weak */
  ShellLayoutManager *layout_manager;   /* weak */
  MetaDisplay *display;                 /* weak */

  ClutterActorBox work_area_box;
  ClutterActorBox cached_workspace_boxes[N_STATES];
  gboolean has_cached_boxes;

  GList *post_allocation_callbacks;     /* of PostAllocation */
};

G_DEFINE_FINAL_TYPE (ShellControlsLayout, shell_controls_layout, CLUTTER_TYPE_LAYOUT_MANAGER)

static void
post_allocation_free (PostAllocation *post)
{
  if (post->destroy != NULL)
    post->destroy (post->user_data);
  g_free (post);
}

static void
controls_layout_update_work_area_box (ShellControlsLayout *self)
{
  g_autoptr (ShellMonitor) monitor = NULL;
  MtkRectangle work_area;

  if (self->layout_manager == NULL)
    return;

  monitor = shell_layout_manager_get_primary_monitor (self->layout_manager);
  if (monitor == NULL)
    return;

  shell_layout_manager_get_work_area_for_monitor (self->layout_manager, monitor->index, &work_area);
  clutter_actor_box_init_rect (&self->work_area_box,
                               work_area.x - monitor->x,
                               work_area.y - monitor->y,
                               work_area.width, work_area.height);
}

static void
controls_layout_work_area_changed_cb (ShellControlsLayout *self,
                                      gpointer             instance)
{
  controls_layout_update_work_area_box (self);
}

static void
controls_layout_state_changed_cb (StAdjustment        *adjustment,
                                  GParamSpec          *pspec,
                                  ShellControlsLayout *self)
{
  clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
}

static void
compute_workspaces_box_for_state (ShellControlsLayout    *self,
                                  ShellControlsState      state,
                                  const ClutterActorBox  *box,
                                  float                   search_height,
                                  float                   dash_height,
                                  float                   thumbnails_height,
                                  float                   spacing,
                                  ClutterActorBox        *workspace_box)
{
  float width = clutter_actor_box_get_width (box);
  float height = clutter_actor_box_get_height (box);
  float start_y = self->work_area_box.y1;
  double expand_fraction = 0;
  float x, y;

  if (self->workspaces_thumbnails != NULL)
    expand_fraction = shell_thumbnails_box_get_expand_fraction (SHELL_THUMBNAILS_BOX (self->workspaces_thumbnails));

  *workspace_box = *box;

  switch (state)
    {
    case SHELL_CONTROLS_STATE_HIDDEN:
      clutter_actor_box_get_origin (&self->work_area_box, &x, &y);
      clutter_actor_box_set_origin (workspace_box, x, y);
      clutter_actor_box_get_size (&self->work_area_box, &x, &y);
      clutter_actor_box_set_size (workspace_box, x, y);
      break;

    case SHELL_CONTROLS_STATE_WINDOW_PICKER:
      clutter_actor_box_set_origin (workspace_box, 0,
                                    start_y + search_height +
                                    js_round (spacing * THUMBNAILS_SPACING_ADJUSTMENT_TOP) +
                                    thumbnails_height +
                                    js_round (spacing * THUMBNAILS_SPACING_ADJUSTMENT_BOTTOM) * expand_fraction);
      clutter_actor_box_set_size (workspace_box, width,
                                  height -
                                  dash_height - spacing -
                                  search_height - js_round (spacing * THUMBNAILS_SPACING_ADJUSTMENT_TOP) -
                                  thumbnails_height -
                                  js_round (spacing * THUMBNAILS_SPACING_ADJUSTMENT_BOTTOM) * expand_fraction);
      break;

    case SHELL_CONTROLS_STATE_APP_GRID:
      clutter_actor_box_set_origin (workspace_box, 0, start_y + search_height + spacing);
      clutter_actor_box_set_size (workspace_box, width,
                                  js_round (height * SHELL_SMALL_WORKSPACE_RATIO));
      break;

    default:
      break;
    }
}

static void
get_app_display_box_for_state (ShellControlsLayout   *self,
                               ShellControlsState     state,
                               const ClutterActorBox *box,
                               float                  search_height,
                               float                  dash_height,
                               const ClutterActorBox *workspaces_box,
                               float                  spacing,
                               ClutterActorBox       *app_display_box)
{
  float width = clutter_actor_box_get_width (box);
  float height = clutter_actor_box_get_height (box);
  float start_y = self->work_area_box.y1;
  float workspaces_height = clutter_actor_box_get_height (workspaces_box);
  float x = 0, y = 0;

  switch (state)
    {
    case SHELL_CONTROLS_STATE_HIDDEN:
    case SHELL_CONTROLS_STATE_WINDOW_PICKER:
      y = box->y2;
      break;

    case SHELL_CONTROLS_STATE_APP_GRID:
      y = start_y + search_height + spacing + workspaces_height + spacing;
      break;

    default:
      break;
    }

  clutter_actor_box_init_rect (app_display_box, x, y, width,
                               height -
                               search_height - spacing -
                               workspaces_height - spacing -
                               dash_height - spacing);
}

static void
run_post_allocation (ShellControlsLayout *self)
{
  GList *callbacks, *l;

  if (self->post_allocation_callbacks == NULL)
    return;

  callbacks = g_list_reverse (self->post_allocation_callbacks);
  self->post_allocation_callbacks = NULL;

  for (l = callbacks; l != NULL; l = l->next)
    {
      PostAllocation *post = l->data;

      post->callback (TRUE, post->user_data);
      post_allocation_free (post);
    }

  g_list_free (callbacks);
}

static void
controls_layout_get_preferred_width (ClutterLayoutManager *manager,
                                     ClutterActor         *container,
                                     float                 for_height,
                                     float                *min_width_p,
                                     float                *natural_width_p)
{
  /* The MonitorConstraint will allocate us a fixed size anyway */
  if (min_width_p != NULL)
    *min_width_p = 0;
  if (natural_width_p != NULL)
    *natural_width_p = 0;
}

static void
controls_layout_get_preferred_height (ClutterLayoutManager *manager,
                                      ClutterActor         *container,
                                      float                 for_width,
                                      float                *min_height_p,
                                      float                *natural_height_p)
{
  /* The MonitorConstraint will allocate us a fixed size anyway */
  if (min_height_p != NULL)
    *min_height_p = 0;
  if (natural_height_p != NULL)
    *natural_height_p = 0;
}

static int
clamp_state (double state)
{
  return CLAMP ((int) js_round (state), SHELL_CONTROLS_STATE_HIDDEN, SHELL_CONTROLS_STATE_APP_GRID);
}

static void
controls_layout_allocate (ClutterLayoutManager *manager,
                          ClutterActor         *container,
                          const ClutterActorBox *allocation)
{
  ShellControlsLayout *self = SHELL_CONTROLS_LAYOUT (manager);
  ShellStateTransitionParams transition_params;
  ClutterActorBox box = *allocation;
  ClutterActorBox child_box, workspaces_box;
  float start_y = self->work_area_box.y1;
  float width, height, spacing, available_height;
  float search_height, dash_height, max_dash_height, thumbnails_height = 0;
  int state;

  if (self->search_entry == NULL || self->dash == NULL ||
      self->workspaces_thumbnails == NULL || self->workspaces_display == NULL ||
      self->app_display == NULL || self->search_controller == NULL ||
      self->state_adjustment == NULL)
    return;

  box.y1 += start_y;
  clutter_actor_box_get_size (&box, &width, &height);
  spacing = js_round (height * VERTICAL_SPACING_RATIO);
  available_height = height;

  /* Search entry */
  clutter_actor_get_preferred_height (self->search_entry, width, &search_height, NULL);
  clutter_actor_box_init_rect (&child_box, 0, start_y, width, search_height);
  clutter_actor_allocate (self->search_entry, &child_box);

  available_height -= search_height + spacing;

  /* Dash */
  max_dash_height = js_round (clutter_actor_box_get_height (&box) * DASH_MAX_HEIGHT_RATIO);
  shell_dash_set_max_size (SHELL_DASH (self->dash), width, max_dash_height);

  clutter_actor_get_preferred_height (self->dash, width, NULL, &dash_height);
  dash_height = MIN (dash_height, max_dash_height);
  clutter_actor_box_init_rect (&child_box, 0, start_y + height - dash_height, width, dash_height);
  clutter_actor_allocate (self->dash, &child_box);

  available_height -= dash_height + spacing;

  /* Workspace Thumbnails */
  if (clutter_actor_is_visible (self->workspaces_thumbnails))
    {
      ShellThumbnailsBox *thumbnails = SHELL_THUMBNAILS_BOX (self->workspaces_thumbnails);
      double expand_fraction = shell_thumbnails_box_get_expand_fraction (thumbnails);

      clutter_actor_get_preferred_height (self->workspaces_thumbnails, width,
                                          &thumbnails_height, NULL);
      thumbnails_height = MIN (thumbnails_height * expand_fraction,
                               height * shell_thumbnails_box_get_max_thumbnail_scale (thumbnails));
      clutter_actor_box_init_rect (&child_box, 0,
                                   start_y + search_height +
                                   js_round (spacing * THUMBNAILS_SPACING_ADJUSTMENT_TOP),
                                   width, thumbnails_height);
      clutter_actor_allocate (self->workspaces_thumbnails, &child_box);
    }

  /* Workspaces */
  shell_overview_adjustment_get_state_transition_params (self->state_adjustment,
                                                         &transition_params);

  /* Update cached boxes */
  for (state = 0; state < N_STATES; state++)
    compute_workspaces_box_for_state (self, state, &box, search_height, dash_height,
                                      thumbnails_height, spacing,
                                      &self->cached_workspace_boxes[state]);
  self->has_cached_boxes = TRUE;

  if (!transition_params.transitioning)
    {
      workspaces_box = self->cached_workspace_boxes[clamp_state (transition_params.current_state)];
    }
  else
    {
      clutter_actor_box_interpolate (&self->cached_workspace_boxes[clamp_state (transition_params.initial_state)],
                                     &self->cached_workspace_boxes[clamp_state (transition_params.final_state)],
                                     transition_params.progress, &workspaces_box);
    }

  clutter_actor_allocate (self->workspaces_display, &workspaces_box);

  /* AppDisplay */
  if (clutter_actor_is_visible (self->app_display))
    {
      const ClutterActorBox *workspace_app_grid_box =
        &self->cached_workspace_boxes[SHELL_CONTROLS_STATE_APP_GRID];
      ClutterActorBox app_display_box;

      if (!transition_params.transitioning)
        {
          get_app_display_box_for_state (self, clamp_state (transition_params.current_state),
                                         &box, search_height, dash_height,
                                         workspace_app_grid_box, spacing, &app_display_box);
        }
      else
        {
          ClutterActorBox initial_box, final_box;

          get_app_display_box_for_state (self, clamp_state (transition_params.initial_state),
                                         &box, search_height, dash_height,
                                         workspace_app_grid_box, spacing, &initial_box);
          get_app_display_box_for_state (self, clamp_state (transition_params.final_state),
                                         &box, search_height, dash_height,
                                         workspace_app_grid_box, spacing, &final_box);
          clutter_actor_box_interpolate (&initial_box, &final_box,
                                         transition_params.progress, &app_display_box);
        }

      clutter_actor_allocate (self->app_display, &app_display_box);
    }

  /* Search */
  clutter_actor_box_init_rect (&child_box, 0, start_y + search_height + spacing,
                               width, available_height);
  clutter_actor_allocate (self->search_controller, &child_box);

  run_post_allocation (self);
}

static void
shell_controls_layout_dispose (GObject *object)
{
  ShellControlsLayout *self = SHELL_CONTROLS_LAYOUT (object);

  g_list_free_full (g_steal_pointer (&self->post_allocation_callbacks),
                    (GDestroyNotify) post_allocation_free);

  g_clear_weak_pointer (&self->search_entry);
  g_clear_weak_pointer (&self->app_display);
  g_clear_weak_pointer (&self->workspaces_display);
  g_clear_weak_pointer (&self->workspaces_thumbnails);
  g_clear_weak_pointer (&self->search_controller);
  g_clear_weak_pointer (&self->dash);
  g_clear_weak_pointer (&self->state_adjustment);
  g_clear_weak_pointer (&self->layout_manager);
  g_clear_weak_pointer (&self->display);

  G_OBJECT_CLASS (shell_controls_layout_parent_class)->dispose (object);
}

static void
shell_controls_layout_class_init (ShellControlsLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  object_class->dispose = shell_controls_layout_dispose;

  layout_class->get_preferred_width = controls_layout_get_preferred_width;
  layout_class->get_preferred_height = controls_layout_get_preferred_height;
  layout_class->allocate = controls_layout_allocate;
}

static void
shell_controls_layout_init (ShellControlsLayout *self)
{
}

static ShellControlsLayout *
controls_layout_new (ClutterActor            *search_entry,
                     ClutterActor            *app_display,
                     ClutterActor            *workspaces_display,
                     ClutterActor            *workspaces_thumbnails,
                     ClutterActor            *search_controller,
                     ClutterActor            *dash,
                     ShellOverviewAdjustment *state_adjustment,
                     ShellLayoutManager      *layout_manager)
{
  ShellControlsLayout *self = g_object_new (SHELL_TYPE_CONTROLS_LAYOUT, NULL);
  MetaDisplay *display = shell_global_get_display (shell_global_get ());

  g_set_weak_pointer (&self->search_entry, search_entry);
  g_set_weak_pointer (&self->app_display, app_display);
  g_set_weak_pointer (&self->workspaces_display, workspaces_display);
  g_set_weak_pointer (&self->workspaces_thumbnails, workspaces_thumbnails);
  g_set_weak_pointer (&self->search_controller, search_controller);
  g_set_weak_pointer (&self->dash, dash);
  g_set_weak_pointer (&self->state_adjustment, state_adjustment);
  g_set_weak_pointer (&self->layout_manager, layout_manager);
  g_set_weak_pointer (&self->display, display);

  g_signal_connect_object (state_adjustment, "notify::value",
                           G_CALLBACK (controls_layout_state_changed_cb), self, 0);

  clutter_actor_box_init_rect (&self->work_area_box, 0, 0, 0, 0);
  g_signal_connect_object (display, "workareas-changed",
                           G_CALLBACK (controls_layout_work_area_changed_cb), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (layout_manager, "monitors-changed",
                           G_CALLBACK (controls_layout_work_area_changed_cb), self, G_CONNECT_SWAPPED);
  controls_layout_update_work_area_box (self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellControlsManager                                                      */
/* ------------------------------------------------------------------------ */

struct _ShellControlsManager
{
  StWidget parent;

  ShellOverview *overview;              /* weak */
  ShellWindowManager *window_manager;   /* weak */
  ShellLayoutManager *layout_manager;   /* weak */

  gboolean ignore_show_apps_button_toggle;

  ShellOverviewAdjustment *state_adjustment;
  StAdjustment *workspace_adjustment;

  /* Children, weak */
  ClutterActor *search_entry;
  ClutterActor *search_entry_bin;
  ShellDash *dash;
  ShellSearchController *search_controller;
  ShellThumbnailsBox *thumbnails_box;
  ShellWorkspacesDisplay *workspaces_display;
  ClutterActor *app_display;

  GSettings *a11y_settings;
  gint64 last_overlay_key_time;
};

G_DEFINE_FINAL_TYPE (ShellControlsManager, shell_controls_manager, ST_TYPE_WIDGET)

static ShellControlsLayout *
controls_get_layout (ShellControlsManager *self)
{
  ClutterLayoutManager *layout = clutter_actor_get_layout_manager (CLUTTER_ACTOR (self));

  return SHELL_IS_CONTROLS_LAYOUT (layout) ? SHELL_CONTROLS_LAYOUT (layout) : NULL;
}

static StButton *
controls_get_show_apps_button (ShellControlsManager *self)
{
  return self->dash != NULL ? shell_dash_get_show_apps_button (self->dash) : NULL;
}

static void
controls_set_show_apps_checked (ShellControlsManager *self,
                                gboolean              checked)
{
  StButton *button = controls_get_show_apps_button (self);

  if (button != NULL)
    st_button_set_checked (button, checked);
}

static void
controls_get_thumbnails_box_params (ShellControlsManager *self,
                                    double               *opacity,
                                    double               *scale,
                                    double               *translation_y)
{
  ShellStateTransitionParams params;
  double values[2][3];
  double states[2];
  int i;

  shell_overview_adjustment_get_state_transition_params (self->state_adjustment, &params);
  states[0] = params.initial_state;
  states[1] = params.final_state;

  for (i = 0; i < 2; i++)
    {
      switch ((int) states[i])
        {
        case SHELL_CONTROLS_STATE_APP_GRID:
          values[i][0] = 0;
          values[i][1] = 0.5;
          /* The size is only read when it is needed: the box is not on the
           * stage when the controls are created */
          values[i][2] = clutter_actor_get_height (CLUTTER_ACTOR (self->thumbnails_box)) / 2;
          break;

        case SHELL_CONTROLS_STATE_HIDDEN:
        case SHELL_CONTROLS_STATE_WINDOW_PICKER:
        default:
          values[i][0] = 255;
          values[i][1] = 1;
          values[i][2] = 0;
          break;
        }
    }

  *opacity = lerp (values[0][0], values[1][0], params.progress);
  *scale = lerp (values[0][1], values[1][1], params.progress);
  *translation_y = lerp (values[0][2], values[1][2], params.progress);
}

static void controls_update_thumbnails_box (ShellControlsManager *self,
                                            gboolean              animate);

static void
thumbnails_box_eased_cb (gboolean finished,
                         gpointer user_data)
{
  WeakFlag *data = user_data;
  g_autoptr (ShellControlsManager) self = g_weak_ref_get (&data->ref);

  if (self == NULL || !finished || self->thumbnails_box == NULL)
    return;

  /* flag: thumbnailsBoxVisible */
  set_actor_visible (CLUTTER_ACTOR (self->thumbnails_box), data->flag);
  if (!data->flag)
    shell_thumbnails_box_set_expand_fraction (self->thumbnails_box, 0.0);
}

static void
controls_update_thumbnails_box (ShellControlsManager *self,
                                gboolean              animate)
{
  ClutterActor *box = CLUTTER_ACTOR (self->thumbnails_box);
  gboolean should_show = shell_thumbnails_box_get_should_show (self->thumbnails_box);
  gboolean search_active = shell_search_controller_get_search_active (self->search_controller);
  ShellEaseParams params = ease_params (animate ? SHELL_SIDE_CONTROLS_ANIMATION_TIME : 0,
                                        CLUTTER_EASE_OUT_QUAD);
  double opacity, scale, translation_y;
  gboolean thumbnails_box_visible;
  WeakFlag *data;

  controls_get_thumbnails_box_params (self, &opacity, &scale, &translation_y);

  thumbnails_box_visible = should_show && !search_active && opacity != 0;
  if (thumbnails_box_visible)
    {
      clutter_actor_set_opacity (box, 0);
      set_actor_visible (box, thumbnails_box_visible);
      shell_thumbnails_box_set_expand_fraction (self->thumbnails_box, 1.0);
    }

  data = weak_flag_new (self, thumbnails_box_visible);

  if (!search_active)
    {
      shell_actor_ease (box, &params, thumbnails_box_eased_cb, data, weak_flag_free,
                        "opacity", (int) opacity,
                        "scale-x", scale,
                        "scale-y", scale,
                        "translation-y", translation_y,
                        NULL);
    }
  else
    {
      shell_actor_ease (box, &params, thumbnails_box_eased_cb, data, weak_flag_free,
                        "opacity", 0,
                        NULL);
    }
}

static void
controls_update_app_display_visibility (ShellControlsManager *self,
                                        const ShellStateTransitionParams *params)
{
  ShellStateTransitionParams own_params;
  double state;

  if (params == NULL)
    {
      shell_overview_adjustment_get_state_transition_params (self->state_adjustment, &own_params);
      params = &own_params;
    }

  state = MAX (params->initial_state, params->final_state);

  set_actor_visible (self->app_display,
                     state > SHELL_CONTROLS_STATE_WINDOW_PICKER &&
                     !shell_search_controller_get_search_active (self->search_controller));
}

static void
controls_update (ShellControlsManager *self)
{
  ShellStateTransitionParams params;
  StAdjustment *fit_mode_adjustment;
  double fit_mode;

  if (self->workspaces_display == NULL || self->thumbnails_box == NULL ||
      self->search_controller == NULL || self->app_display == NULL)
    return;

  shell_overview_adjustment_get_state_transition_params (self->state_adjustment, &params);

  fit_mode = lerp (get_fit_mode_for_state (params.initial_state),
                   get_fit_mode_for_state (params.final_state),
                   params.progress);

  fit_mode_adjustment = shell_workspaces_display_get_fit_mode_adjustment (self->workspaces_display);
  st_adjustment_set_value (fit_mode_adjustment, fit_mode);

  controls_update_thumbnails_box (self, FALSE);
  controls_update_app_display_visibility (self, &params);

  if (params.progress == 1.0)
    clutter_actor_grab_key_focus (CLUTTER_ACTOR (self));
}

static void
controls_state_changed_cb (StAdjustment         *adjustment,
                           GParamSpec           *pspec,
                           ShellControlsManager *self)
{
  controls_update (self);
}

static void
app_display_eased_cb (gboolean finished,
                      gpointer user_data)
{
  g_autoptr (ShellControlsManager) self = g_weak_ref_get (user_data);

  if (self == NULL || !finished || self->app_display == NULL)
    return;

  controls_update_app_display_visibility (self, NULL);
}

static void
workspaces_display_eased_cb (gboolean finished,
                             gpointer user_data)
{
  WeakFlag *data = user_data;
  g_autoptr (ShellControlsManager) self = g_weak_ref_get (&data->ref);

  if (self == NULL || !finished || self->workspaces_display == NULL)
    return;

  /* flag: searchActive */
  clutter_actor_set_reactive (CLUTTER_ACTOR (self->workspaces_display), !data->flag);
  shell_workspaces_display_set_primary_workspace_visible (self->workspaces_display, !data->flag);
}

static void
search_controller_eased_cb (gboolean finished,
                            gpointer user_data)
{
  WeakFlag *data = user_data;
  g_autoptr (ShellControlsManager) self = g_weak_ref_get (&data->ref);

  if (self == NULL || !finished || self->search_controller == NULL)
    return;

  set_actor_visible (CLUTTER_ACTOR (self->search_controller), data->flag);
}

static void
controls_search_changed_cb (ShellSearchController *controller,
                            GParamSpec            *pspec,
                            ShellControlsManager  *self)
{
  gboolean search_active = shell_search_controller_get_search_active (controller);
  ShellEaseParams params = ease_params (SHELL_SIDE_CONTROLS_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);

  if (self->workspaces_display == NULL || self->app_display == NULL)
    return;

  if (!search_active)
    {
      controls_update_app_display_visibility (self, NULL);
      clutter_actor_set_reactive (CLUTTER_ACTOR (self->workspaces_display), TRUE);
      shell_workspaces_display_set_primary_workspace_visible (self->workspaces_display, TRUE);
    }
  else
    {
      clutter_actor_show (CLUTTER_ACTOR (self->search_controller));
    }

  controls_update_thumbnails_box (self, TRUE);

  shell_actor_ease (self->app_display, &params,
                    app_display_eased_cb, weak_ref_new (self), weak_ref_free,
                    "opacity", search_active ? 0 : 255,
                    NULL);
  shell_actor_ease (CLUTTER_ACTOR (self->workspaces_display), &params,
                    workspaces_display_eased_cb, weak_flag_new (self, search_active),
                    weak_flag_free,
                    "opacity", search_active ? 0 : 255,
                    NULL);
  shell_actor_ease (CLUTTER_ACTOR (self->search_controller), &params,
                    search_controller_eased_cb, weak_flag_new (self, search_active),
                    weak_flag_free,
                    "opacity", search_active ? 255 : 0,
                    NULL);
}

static void
controls_show_apps_button_toggled_cb (StButton             *button,
                                      GParamSpec           *pspec,
                                      ShellControlsManager *self)
{
  double value;

  if (self->ignore_show_apps_button_toggle)
    return;

  value = st_button_get_checked (button)
    ? SHELL_CONTROLS_STATE_APP_GRID : SHELL_CONTROLS_STATE_WINDOW_PICKER;
  st_adjustment_remove_transition (ST_ADJUSTMENT (self->state_adjustment), "value");
  ease_adjustment (ST_ADJUSTMENT (self->state_adjustment), value,
                   SHELL_SIDE_CONTROLS_ANIMATION_TIME, CLUTTER_EASE_OUT_SINE,
                   NULL, NULL, NULL);
}

static void
shift_done_cb (gboolean finished,
               gpointer user_data)
{
  WeakFlag *data = user_data;
  g_autoptr (ShellControlsManager) self = g_weak_ref_get (&data->ref);

  if (self == NULL || !finished)
    return;

  /* flag: finalState === APP_GRID */
  controls_set_show_apps_checked (self, data->flag);
}

static void
controls_shift_state (ShellControlsManager *self,
                      MetaMotionDirection   direction,
                      const char           *reason)
{
  ShellStateTransitionParams params;
  double final_state;

  shell_overview_adjustment_get_state_transition_params (self->state_adjustment, &params);
  final_state = params.final_state;

  if (direction == META_MOTION_DOWN)
    final_state = MAX (final_state - 1, SHELL_CONTROLS_STATE_HIDDEN);
  else if (direction == META_MOTION_UP)
    final_state = MIN (final_state + 1, SHELL_CONTROLS_STATE_APP_GRID);

  if (final_state == params.current_state)
    return;

  if (params.current_state == SHELL_CONTROLS_STATE_HIDDEN &&
      final_state == SHELL_CONTROLS_STATE_WINDOW_PICKER)
    {
      shell_overview_show_for_reason (self->overview, SHELL_CONTROLS_STATE_WINDOW_PICKER, reason);
    }
  else if (final_state == SHELL_CONTROLS_STATE_HIDDEN)
    {
      shell_overview_hide_for_reason (self->overview, reason);
    }
  else
    {
      ease_adjustment (ST_ADJUSTMENT (self->state_adjustment), final_state,
                       SHELL_SIDE_CONTROLS_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD,
                       shift_done_cb,
                       weak_flag_new (self, final_state == SHELL_CONTROLS_STATE_APP_GRID),
                       weak_flag_free);
    }
}

static void
controls_toggle_apps_page (ShellControlsManager *self)
{
  if (shell_overview_get_visible (self->overview))
    {
      StButton *button = controls_get_show_apps_button (self);

      if (button != NULL)
        st_button_set_checked (button, !st_button_get_checked (button));
    }
  else
    {
      shell_overview_show_for_reason (self->overview, SHELL_CONTROLS_STATE_APP_GRID, "keybinding");
    }
}

static void
controls_overlay_key_cb (MetaDisplay          *display,
                         ShellControlsManager *self)
{
  ShellStateTransitionParams params;
  gboolean should_shift;
  gint64 time, time_diff;

  if (g_settings_get_boolean (self->a11y_settings, "stickykeys-enable"))
    return;

  shell_overview_adjustment_get_state_transition_params (self->state_adjustment, &params);

  time = g_get_monotonic_time () / 1000;
  time_diff = time - self->last_overlay_key_time;
  self->last_overlay_key_time = time;

  if (st_settings_get_enable_animations (st_settings_get ()))
    {
      should_shift = params.transitioning && params.final_state > params.initial_state;
    }
  else
    {
      should_shift = shell_overview_get_visible (self->overview) &&
                     time_diff < SHELL_OVERVIEW_ANIMATION_TIME;
    }

  if (should_shift)
    controls_shift_state (self, META_MOTION_UP, "overlay-key");
  else
    shell_overview_toggle_for_reason (self->overview, "overlay-key");
}

/* The key bindings keep a weak reference to the controls */
static void
toggle_apps_page_keybinding_cb (MetaDisplay        *display,
                                MetaWindow         *window,
                                const ClutterEvent *event,
                                MetaKeyBinding     *binding,
                                gpointer            user_data)
{
  g_autoptr (ShellControlsManager) self = g_weak_ref_get (user_data);

  if (self != NULL)
    controls_toggle_apps_page (self);
}

static void
shift_up_keybinding_cb (MetaDisplay        *display,
                        MetaWindow         *window,
                        const ClutterEvent *event,
                        MetaKeyBinding     *binding,
                        gpointer            user_data)
{
  g_autoptr (ShellControlsManager) self = g_weak_ref_get (user_data);

  if (self != NULL)
    controls_shift_state (self, META_MOTION_UP, "keybinding");
}

static void
shift_down_keybinding_cb (MetaDisplay        *display,
                          MetaWindow         *window,
                          const ClutterEvent *event,
                          MetaKeyBinding     *binding,
                          gpointer            user_data)
{
  g_autoptr (ShellControlsManager) self = g_weak_ref_get (user_data);

  if (self != NULL)
    controls_shift_state (self, META_MOTION_DOWN, "keybinding");
}

static void
controls_add_keybindings (ShellControlsManager *self)
{
  g_autoptr (GSettings) settings = g_settings_new (SHELL_KEYBINDINGS_SCHEMA);
  ShellActionMode modes = SHELL_ACTION_MODE_NORMAL | SHELL_ACTION_MODE_OVERVIEW;

  shell_window_manager_add_keybinding (self->window_manager, "toggle-application-view", settings,
                                       META_KEY_BINDING_IGNORE_AUTOREPEAT, modes,
                                       toggle_apps_page_keybinding_cb,
                                       weak_ref_new (self), weak_ref_free);
  shell_window_manager_add_keybinding (self->window_manager, "shift-overview-up", settings,
                                       META_KEY_BINDING_IGNORE_AUTOREPEAT, modes,
                                       shift_up_keybinding_cb,
                                       weak_ref_new (self), weak_ref_free);
  shell_window_manager_add_keybinding (self->window_manager, "shift-overview-down", settings,
                                       META_KEY_BINDING_IGNORE_AUTOREPEAT, modes,
                                       shift_down_keybinding_cb,
                                       weak_ref_new (self), weak_ref_free);
}

/* ---- Ctrl+Alt+Tab ---------------------------------------------------- */

static void
focus_app_grid_cb (uint32_t  timestamp,
                   gpointer  user_data)
{
  g_autoptr (ShellControlsManager) self = g_weak_ref_get (user_data);

  if (self == NULL || self->app_display == NULL)
    return;

  controls_set_show_apps_checked (self, TRUE);
  st_widget_navigate_focus (ST_WIDGET (self->app_display), NULL, ST_DIR_TAB_FORWARD, FALSE);
}

static void
focus_windows_cb (uint32_t  timestamp,
                  gpointer  user_data)
{
  g_autoptr (ShellControlsManager) self = g_weak_ref_get (user_data);

  if (self == NULL || self->workspaces_display == NULL)
    return;

  controls_set_show_apps_checked (self, FALSE);
  st_widget_navigate_focus (ST_WIDGET (self->workspaces_display), NULL, ST_DIR_TAB_FORWARD, FALSE);
}

/* ---- The thumbnails box ---------------------------------------------- */

static void
thumbnails_expand_done_cb (gboolean finished,
                           gpointer user_data)
{
  g_autoptr (ShellControlsManager) self = g_weak_ref_get (user_data);

  if (self == NULL || !finished || self->thumbnails_box == NULL)
    return;

  controls_update_thumbnails_box (self, FALSE);
}

static void
controls_thumbnails_should_show_cb (ShellThumbnailsBox   *box,
                                    GParamSpec           *pspec,
                                    ShellControlsManager *self)
{
  ShellEaseParams params = ease_params (SHELL_SIDE_CONTROLS_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
  g_auto (GValue) value = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;

  clutter_actor_show (CLUTTER_ACTOR (box));

  g_value_init (&value, G_TYPE_DOUBLE);
  g_value_set_double (&value, shell_thumbnails_box_get_should_show (box) ? 1 : 0);
  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (box), "expand-fraction", &value,
                                       &params, thumbnails_expand_done_cb,
                                       weak_ref_new (self), weak_ref_free, &error))
    g_debug ("%s: %s", G_STRFUNC, error->message);
}

static void
controls_monitors_changed_cb (ShellLayoutManager  *layout_manager,
                              ShellControlsManager *self)
{
  if (self->thumbnails_box != NULL)
    shell_thumbnails_box_set_monitor_index (self->thumbnails_box,
                                            shell_layout_manager_get_primary_index (layout_manager));
}

/* The window drags of the overview are told to the dash */
static void
controls_window_drag_begin_cb (ShellOverview        *overview,
                               MetaWindow           *window,
                               ShellControlsManager *self)
{
  if (self->dash != NULL)
    shell_dash_window_drag_begin (self->dash);
}

static void
controls_window_drag_end_cb (ShellOverview        *overview,
                             MetaWindow           *window,
                             ShellControlsManager *self)
{
  if (self->dash != NULL)
    shell_dash_window_drag_end (self->dash);
}

static void
controls_overview_showing_cb (ShellOverview        *overview,
                              ShellControlsManager *self)
{
  if (self->search_controller != NULL)
    shell_search_controller_overview_showing (self->search_controller);
}

static void
controls_overview_hiding_cb (ShellOverview        *overview,
                             ShellControlsManager *self)
{
  if (self->search_controller != NULL)
    shell_search_controller_overview_hiding (self->search_controller);
}

static void
search_overview_hide_requested_cb (ShellSearchController *controller,
                                   ShellOverview         *overview)
{
  shell_overview_hide_for_reason (overview, "escape");
}

static void
search_overview_toggle_requested_cb (ShellSearchController *controller,
                                     ShellOverview         *overview)
{
  shell_overview_toggle_for_reason (overview, "app-launched");
}

/* ---- The thumbnails box and the overview ----------------------------- */

static void
thumbnails_window_drag_begin_cb (ShellThumbnailsBox *box,
                                 MetaWindow         *window,
                                 ShellOverview      *overview)
{
  shell_overview_begin_window_drag (overview, window);
}

static void
thumbnails_window_drag_cancelled_cb (ShellThumbnailsBox *box,
                                     MetaWindow         *window,
                                     ShellOverview      *overview)
{
  shell_overview_cancelled_window_drag (overview, window);
}

static void
thumbnails_window_drag_end_cb (ShellThumbnailsBox *box,
                               MetaWindow         *window,
                               ShellOverview      *overview)
{
  shell_overview_end_window_drag (overview, window);
}

static void
overview_drag_begin_cb (ShellOverview      *overview,
                        gpointer            source,
                        ShellThumbnailsBox *box)
{
  shell_thumbnails_box_drag_begin (box);
}

static void
overview_drag_end_cb (ShellOverview      *overview,
                      gpointer            source,
                      ShellThumbnailsBox *box)
{
  shell_thumbnails_box_drag_end (box);
}

static void
overview_drag_cancelled_cb (ShellOverview      *overview,
                            gpointer            source,
                            ShellThumbnailsBox *box)
{
  shell_thumbnails_box_drag_cancelled (box);
}

/**
 * shell_controls_connect_thumbnails_box: (skip)
 * @overview: the overview
 * @thumbnails_box: the box to connect
 *
 * Keeps a thumbnails box in step with the overview: the drags of items and
 * windows tell the box where a workspace could be dropped, and the drags of
 * the windows of its thumbnails tell the overview. (The box follows the
 * visibility of the overview through the window manager.)
 */
void
shell_controls_connect_thumbnails_box (ShellOverview      *overview,
                                       ShellThumbnailsBox *thumbnails_box)
{
  g_return_if_fail (SHELL_IS_OVERVIEW (overview));
  g_return_if_fail (SHELL_IS_THUMBNAILS_BOX (thumbnails_box));

  g_signal_connect_object (thumbnails_box, "window-drag-begin",
                           G_CALLBACK (thumbnails_window_drag_begin_cb), overview, 0);
  g_signal_connect_object (thumbnails_box, "window-drag-cancelled",
                           G_CALLBACK (thumbnails_window_drag_cancelled_cb), overview, 0);
  g_signal_connect_object (thumbnails_box, "window-drag-end",
                           G_CALLBACK (thumbnails_window_drag_end_cb), overview, 0);

  g_signal_connect_object (overview, "item-drag-begin",
                           G_CALLBACK (overview_drag_begin_cb), thumbnails_box, 0);
  g_signal_connect_object (overview, "item-drag-end",
                           G_CALLBACK (overview_drag_end_cb), thumbnails_box, 0);
  g_signal_connect_object (overview, "item-drag-cancelled",
                           G_CALLBACK (overview_drag_cancelled_cb), thumbnails_box, 0);
  g_signal_connect_object (overview, "window-drag-begin",
                           G_CALLBACK (overview_drag_begin_cb), thumbnails_box, 0);
  g_signal_connect_object (overview, "window-drag-end",
                           G_CALLBACK (overview_drag_end_cb), thumbnails_box, 0);
  g_signal_connect_object (overview, "window-drag-cancelled",
                           G_CALLBACK (overview_drag_cancelled_cb), thumbnails_box, 0);
}

/* ---- vfuncs ---------------------------------------------------------- */

static gboolean
controls_navigate_focus (StWidget        *widget,
                         ClutterActor    *from,
                         StDirectionType  direction)
{
  ShellControlsManager *self = SHELL_CONTROLS_MANAGER (widget);
  ShellStateTransitionParams params;
  ClutterActor *keynav_display;

  if (from != NULL)
    return FALSE;

  shell_overview_adjustment_get_state_transition_params (self->state_adjustment, &params);

  if (params.final_state == SHELL_CONTROLS_STATE_WINDOW_PICKER)
    keynav_display = CLUTTER_ACTOR (self->workspaces_display);
  else if (params.final_state == SHELL_CONTROLS_STATE_APP_GRID)
    keynav_display = self->app_display;
  else
    return FALSE;

  if (keynav_display == NULL)
    return FALSE;

  clutter_actor_grab_key_focus (keynav_display);
  return TRUE;
}

static void
controls_key_focus_in (ClutterActor *actor)
{
  CLUTTER_ACTOR_CLASS (shell_controls_manager_parent_class)->key_focus_in (actor);

  st_widget_navigate_focus (ST_WIDGET (actor), NULL, ST_DIR_TAB_FORWARD, FALSE);
}

static void
controls_unmap (ClutterActor *actor)
{
  ShellControlsManager *self = SHELL_CONTROLS_MANAGER (actor);

  CLUTTER_ACTOR_CLASS (shell_controls_manager_parent_class)->unmap (actor);

  if (self->workspaces_display != NULL)
    clutter_actor_hide (CLUTTER_ACTOR (self->workspaces_display));
}

static void
controls_destroy (ClutterActor *actor)
{
  ShellControlsManager *self = SHELL_CONTROLS_MANAGER (actor);

  /* _onDestroy (): the children are destroyed after this */
  g_clear_weak_pointer (&self->search_entry);
  g_clear_weak_pointer (&self->search_entry_bin);
  g_clear_weak_pointer (&self->dash);
  g_clear_weak_pointer (&self->search_controller);
  g_clear_weak_pointer (&self->thumbnails_box);
  g_clear_weak_pointer (&self->workspaces_display);
  g_clear_weak_pointer (&self->app_display);

  CLUTTER_ACTOR_CLASS (shell_controls_manager_parent_class)->destroy (actor);
}

static void
shell_controls_manager_dispose (GObject *object)
{
  ShellControlsManager *self = SHELL_CONTROLS_MANAGER (object);

  g_clear_object (&self->a11y_settings);
  g_clear_object (&self->state_adjustment);
  g_clear_object (&self->workspace_adjustment);

  g_clear_weak_pointer (&self->search_entry);
  g_clear_weak_pointer (&self->search_entry_bin);
  g_clear_weak_pointer (&self->dash);
  g_clear_weak_pointer (&self->search_controller);
  g_clear_weak_pointer (&self->thumbnails_box);
  g_clear_weak_pointer (&self->workspaces_display);
  g_clear_weak_pointer (&self->app_display);
  g_clear_weak_pointer (&self->overview);
  g_clear_weak_pointer (&self->window_manager);
  g_clear_weak_pointer (&self->layout_manager);

  G_OBJECT_CLASS (shell_controls_manager_parent_class)->dispose (object);
}

static void
shell_controls_manager_class_init (ShellControlsManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);

  object_class->dispose = shell_controls_manager_dispose;

  actor_class->key_focus_in = controls_key_focus_in;
  actor_class->unmap = controls_unmap;
  actor_class->destroy = controls_destroy;

  widget_class->navigate_focus = controls_navigate_focus;
}

static void
shell_controls_manager_init (ShellControlsManager *self)
{
}

/**
 * shell_controls_manager_new:
 * @overview: the overview the controls belong to
 * @window_manager: the window manager
 * @layout_manager: the layout manager
 * @ctrl_alt_tab_manager: (nullable): the manager the application grid and the
 *   workspaces are added to as keyboard navigation groups
 *
 * Creates the controls of the overview (`new ControlsManager ()`).
 *
 * Returns: (transfer none): a new #ShellControlsManager, which is floating
 */
ShellControlsManager *
shell_controls_manager_new (ShellOverview          *overview,
                            ShellWindowManager     *window_manager,
                            ShellLayoutManager     *layout_manager,
                            ShellCtrlAltTabManager *ctrl_alt_tab_manager)
{
  ShellControlsManager *self;
  ShellGlobal *global = shell_global_get ();
  ClutterActor *actor;
  ShellAppSearchProvider *app_search_provider;
  ShellControlsLayout *layout;
  ShellDash *dash;
  ShellSearchController *search_controller;
  ShellThumbnailsBox *thumbnails_box;
  ShellWorkspacesDisplay *workspaces_display;
  ClutterActor *app_display, *search_entry, *search_entry_bin;
  StButton *show_apps_button;

  g_return_val_if_fail (SHELL_IS_OVERVIEW (overview), NULL);
  g_return_val_if_fail (SHELL_IS_WINDOW_MANAGER (window_manager), NULL);
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (layout_manager), NULL);

  self = g_object_new (SHELL_TYPE_CONTROLS_MANAGER,
                       "style-class", "controls-manager",
                       "x-expand", TRUE,
                       "y-expand", TRUE,
                       "clip-to-allocation", TRUE,
                       "reactive", TRUE,
                       NULL);
  actor = CLUTTER_ACTOR (self);

  g_set_weak_pointer (&self->overview, overview);
  g_set_weak_pointer (&self->window_manager, window_manager);
  g_set_weak_pointer (&self->layout_manager, layout_manager);

  search_entry = g_object_new (SHELL_TYPE_SEARCH_ENTRY,
                               "style-class", "search-entry",
                               /* Translators: this is the text displayed
                                  in the search entry when no search is
                                  active; it should not exceed ~30
                                  characters. */
                               "hint-text", _("Type to search"),
                               "track-hover", TRUE,
                               "can-focus", TRUE,
                               NULL);
  clutter_actor_set_offscreen_redirect (search_entry, CLUTTER_OFFSCREEN_REDIRECT_ALWAYS);
  search_entry_bin = g_object_new (ST_TYPE_BIN,
                                   "child", search_entry,
                                   "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                   NULL);

  dash = shell_dash_new (window_manager);
  show_apps_button = shell_dash_get_show_apps_button (dash);

  /* The window drags of the overview are told to the dash. The drags of the
   * items are told by the window manager to the dash and the app grid */
  g_signal_connect_object (overview, "window-drag-begin",
                           G_CALLBACK (controls_window_drag_begin_cb), self, 0);
  g_signal_connect_object (overview, "window-drag-cancelled",
                           G_CALLBACK (controls_window_drag_end_cb), self, 0);
  g_signal_connect_object (overview, "window-drag-end",
                           G_CALLBACK (controls_window_drag_end_cb), self, 0);

  self->workspace_adjustment = shell_window_manager_create_workspaces_adjustment (window_manager, actor);

  self->state_adjustment = shell_overview_adjustment_new (actor);
  g_signal_connect_object (self->state_adjustment, "notify::value",
                           G_CALLBACK (controls_state_changed_cb), self, 0);

  /* The size of the icon that is dragged out of the app grid and the
   * search results is the one of the dash */
  app_search_provider = shell_app_search_provider_new (window_manager);
  g_object_bind_property (dash, "icon-size", app_search_provider, "drag-icon-size",
                          G_BINDING_SYNC_CREATE);

  search_controller = shell_search_controller_new (ST_ENTRY (search_entry), show_apps_button,
                                                   ctrl_alt_tab_manager,
                                                   SHELL_SEARCH_PROVIDER (app_search_provider));
  g_object_unref (app_search_provider);
  g_signal_connect_object (search_controller, "notify::search-active",
                           G_CALLBACK (controls_search_changed_cb), self, 0);
  g_signal_connect_object (search_controller, "overview-hide-requested",
                           G_CALLBACK (search_overview_hide_requested_cb), overview, 0);
  g_signal_connect_object (search_controller, "overview-toggle-requested",
                           G_CALLBACK (search_overview_toggle_requested_cb), overview, 0);
  g_signal_connect_object (overview, "showing",
                           G_CALLBACK (controls_overview_showing_cb), self, 0);
  g_signal_connect_object (overview, "hiding",
                           G_CALLBACK (controls_overview_hiding_cb), self, 0);

  g_signal_connect_object (layout_manager, "monitors-changed",
                           G_CALLBACK (controls_monitors_changed_cb), self, 0);
  thumbnails_box = shell_thumbnails_box_new (self->workspace_adjustment,
                                             shell_layout_manager_get_primary_index (layout_manager),
                                             window_manager);
  shell_controls_connect_thumbnails_box (overview, thumbnails_box);
  g_signal_connect_object (thumbnails_box, "notify::should-show",
                           G_CALLBACK (controls_thumbnails_should_show_cb), self, 0);

  workspaces_display = shell_workspaces_display_new (overview, self, self->workspace_adjustment,
                                                     self->state_adjustment,
                                                     window_manager, layout_manager);
  app_display = CLUTTER_ACTOR (shell_app_display_new (window_manager));
  g_object_bind_property (dash, "icon-size", app_display, "drag-icon-size",
                          G_BINDING_SYNC_CREATE);

  clutter_actor_add_child (actor, search_entry_bin);
  clutter_actor_add_child (actor, app_display);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (dash));
  clutter_actor_add_child (actor, CLUTTER_ACTOR (search_controller));
  clutter_actor_add_child (actor, CLUTTER_ACTOR (thumbnails_box));
  clutter_actor_add_child (actor, CLUTTER_ACTOR (workspaces_display));

  g_set_weak_pointer (&self->search_entry, search_entry);
  g_set_weak_pointer (&self->search_entry_bin, search_entry_bin);
  g_set_weak_pointer (&self->dash, dash);
  g_set_weak_pointer (&self->search_controller, search_controller);
  g_set_weak_pointer (&self->thumbnails_box, thumbnails_box);
  g_set_weak_pointer (&self->workspaces_display, workspaces_display);
  g_set_weak_pointer (&self->app_display, app_display);

  st_focus_manager_add_group (shell_global_get_focus_manager (global), ST_WIDGET (self));

  layout = controls_layout_new (search_entry_bin, app_display, CLUTTER_ACTOR (workspaces_display),
                                CLUTTER_ACTOR (thumbnails_box), CLUTTER_ACTOR (search_controller),
                                CLUTTER_ACTOR (dash), self->state_adjustment, layout_manager);
  clutter_actor_set_layout_manager (actor, CLUTTER_LAYOUT_MANAGER (layout));

  g_signal_connect_object (show_apps_button, "notify::checked",
                           G_CALLBACK (controls_show_apps_button_toggled_cb), self, 0);

  if (ctrl_alt_tab_manager != NULL)
    {
      shell_ctrl_alt_tab_manager_add_group_full (ctrl_alt_tab_manager, app_display,
                                                 _("Apps"),
                                                 "shell-focus-app-grid-symbolic",
                                                 SHELL_CTRL_ALT_TAB_SORT_GROUP_MIDDLE,
                                                 actor,
                                                 focus_app_grid_cb,
                                                 weak_ref_new (self), weak_ref_free);

      shell_ctrl_alt_tab_manager_add_group_full (ctrl_alt_tab_manager,
                                                 CLUTTER_ACTOR (workspaces_display),
                                                 _("Windows"),
                                                 "shell-focus-windows-symbolic",
                                                 SHELL_CTRL_ALT_TAB_SORT_GROUP_MIDDLE,
                                                 actor,
                                                 focus_windows_cb,
                                                 weak_ref_new (self), weak_ref_free);
    }

  self->a11y_settings = g_settings_new (A11Y_SCHEMA);

  g_signal_connect_object (shell_global_get_display (global), "overlay-key",
                           G_CALLBACK (controls_overlay_key_cb), self, 0);

  controls_add_keybindings (self);

  controls_update (self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellControlsManager: API                                                 */
/* ------------------------------------------------------------------------ */

/**
 * shell_controls_manager_get_dash:
 * @self: a #ShellControlsManager
 *
 * Returns: (transfer none) (nullable): the dash (`dash`)
 */
ShellDash *
shell_controls_manager_get_dash (ShellControlsManager *self)
{
  g_return_val_if_fail (SHELL_IS_CONTROLS_MANAGER (self), NULL);

  return self->dash;
}

/**
 * shell_controls_manager_get_search_controller:
 * @self: a #ShellControlsManager
 *
 * Returns: (transfer none) (nullable): the search controller (`searchController`)
 */
ShellSearchController *
shell_controls_manager_get_search_controller (ShellControlsManager *self)
{
  g_return_val_if_fail (SHELL_IS_CONTROLS_MANAGER (self), NULL);

  return self->search_controller;
}

/**
 * shell_controls_manager_get_search_entry:
 * @self: a #ShellControlsManager
 *
 * Returns: (transfer none) (nullable): the search entry (`searchEntry`)
 */
StEntry *
shell_controls_manager_get_search_entry (ShellControlsManager *self)
{
  g_return_val_if_fail (SHELL_IS_CONTROLS_MANAGER (self), NULL);

  return self->search_entry != NULL ? ST_ENTRY (self->search_entry) : NULL;
}

/**
 * shell_controls_manager_get_app_display:
 * @self: a #ShellControlsManager
 *
 * Returns: (transfer none) (nullable): the application grid (`appDisplay`)
 */
ClutterActor *
shell_controls_manager_get_app_display (ShellControlsManager *self)
{
  g_return_val_if_fail (SHELL_IS_CONTROLS_MANAGER (self), NULL);

  return self->app_display;
}

/**
 * shell_controls_manager_get_state_adjustment:
 * @self: a #ShellControlsManager
 *
 * Returns: (transfer none): the adjustment with the state of the overview
 */
ShellOverviewAdjustment *
shell_controls_manager_get_state_adjustment (ShellControlsManager *self)
{
  g_return_val_if_fail (SHELL_IS_CONTROLS_MANAGER (self), NULL);

  return self->state_adjustment;
}

/**
 * shell_controls_manager_prepare_to_enter_overview:
 * @self: a #ShellControlsManager
 *
 * Resets the search and creates the workspaces before the overview shows
 * (`prepareToEnterOverview ()`).
 */
void
shell_controls_manager_prepare_to_enter_overview (ShellControlsManager *self)
{
  g_return_if_fail (SHELL_IS_CONTROLS_MANAGER (self));

  if (self->search_controller == NULL || self->workspaces_display == NULL)
    return;

  shell_search_controller_prepare_to_enter_overview (self->search_controller);
  shell_workspaces_display_prepare_to_enter_overview (self->workspaces_display);
}

/**
 * shell_controls_manager_prepare_to_leave_overview:
 * @self: a #ShellControlsManager
 *
 * Ends the search and freezes the workspaces before the overview hides
 * (`prepareToLeaveOverview ()`).
 */
void
shell_controls_manager_prepare_to_leave_overview (ShellControlsManager *self)
{
  g_return_if_fail (SHELL_IS_CONTROLS_MANAGER (self));

  if (self->search_controller == NULL || self->workspaces_display == NULL)
    return;

  shell_search_controller_prepare_to_leave_overview (self->search_controller);
  shell_workspaces_display_prepare_to_leave_overview (self->workspaces_display);
}

/**
 * shell_controls_manager_animate_to_overview: (skip)
 * @self: a #ShellControlsManager
 * @state: the state to animate to
 * @callback: (nullable): called when the animation stopped
 * @user_data: data for @callback
 * @destroy: called on @user_data after @callback
 *
 * Animates the controls from the hidden state to @state
 * (`animateToOverview ()`).
 */
void
shell_controls_manager_animate_to_overview (ShellControlsManager  *self,
                                            ShellControlsState     state,
                                            ShellControlsCallback  callback,
                                            gpointer               user_data,
                                            GDestroyNotify         destroy)
{
  g_return_if_fail (SHELL_IS_CONTROLS_MANAGER (self));

  self->ignore_show_apps_button_toggle = TRUE;

  st_adjustment_set_value (ST_ADJUSTMENT (self->state_adjustment), SHELL_CONTROLS_STATE_HIDDEN);
  ease_adjustment (ST_ADJUSTMENT (self->state_adjustment), state,
                   SHELL_OVERVIEW_ANIMATION_TIME, CLUTTER_EASE_OUT_SINE,
                   callback, user_data, destroy);

  controls_set_show_apps_checked (self, state == SHELL_CONTROLS_STATE_APP_GRID);

  self->ignore_show_apps_button_toggle = FALSE;
}

typedef struct
{
  GWeakRef *controls;
  ShellControlsCallback callback;
  gpointer user_data;
  GDestroyNotify destroy;
} AnimateFromData;

static void
animate_from_data_free (gpointer user_data)
{
  AnimateFromData *data = user_data;

  weak_ref_free (data->controls);
  if (data->destroy != NULL)
    data->destroy (data->user_data);
  g_free (data);
}

static void
animate_from_overview_done_cb (gboolean finished,
                               gpointer user_data)
{
  AnimateFromData *data = user_data;
  g_autoptr (ShellControlsManager) self = g_weak_ref_get (data->controls);

  if (self != NULL)
    {
      controls_set_show_apps_checked (self, FALSE);
      self->ignore_show_apps_button_toggle = FALSE;
    }

  if (data->callback != NULL)
    data->callback (finished, data->user_data);
}

/**
 * shell_controls_manager_animate_from_overview: (skip)
 * @self: a #ShellControlsManager
 * @callback: (nullable): called when the animation stopped
 * @user_data: data for @callback
 * @destroy: called on @user_data after @callback
 *
 * Animates the controls to the hidden state (`animateFromOverview ()`).
 */
void
shell_controls_manager_animate_from_overview (ShellControlsManager  *self,
                                              ShellControlsCallback  callback,
                                              gpointer               user_data,
                                              GDestroyNotify         destroy)
{
  AnimateFromData *data;

  g_return_if_fail (SHELL_IS_CONTROLS_MANAGER (self));

  self->ignore_show_apps_button_toggle = TRUE;

  data = g_new0 (AnimateFromData, 1);
  data->controls = weak_ref_new (self);
  data->callback = callback;
  data->user_data = user_data;
  data->destroy = destroy;

  ease_adjustment (ST_ADJUSTMENT (self->state_adjustment), SHELL_CONTROLS_STATE_HIDDEN,
                   SHELL_OVERVIEW_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD,
                   animate_from_overview_done_cb, data, animate_from_data_free);
}

/**
 * shell_controls_manager_get_workspaces_box_for_state: (skip)
 * @self: a #ShellControlsManager
 * @state: the state of the overview
 * @box: (out caller-allocates): the box of the workspaces in that state,
 *   relative to the controls
 *
 * Returns: %FALSE if the controls were not allocated yet
 */
gboolean
shell_controls_manager_get_workspaces_box_for_state (ShellControlsManager *self,
                                                     ShellControlsState    state,
                                                     ClutterActorBox      *box)
{
  ShellControlsLayout *layout;

  g_return_val_if_fail (SHELL_IS_CONTROLS_MANAGER (self), FALSE);
  g_return_val_if_fail (box != NULL, FALSE);

  layout = controls_get_layout (self);
  if (layout == NULL || !layout->has_cached_boxes || state < 0 || state >= N_STATES)
    return FALSE;

  *box = layout->cached_workspace_boxes[state];

  return TRUE;
}

/**
 * shell_controls_manager_gesture_begin:
 * @self: a #ShellControlsManager
 * @tracker: the tracker of the swipe
 *
 * Confirms the swipe of the overview (`gestureBegin ()`).
 */
void
shell_controls_manager_gesture_begin (ShellControlsManager *self,
                                      ShellSwipeTracker    *tracker)
{
  static const double points[] = {
    SHELL_CONTROLS_STATE_HIDDEN,
    SHELL_CONTROLS_STATE_WINDOW_PICKER,
    SHELL_CONTROLS_STATE_APP_GRID,
  };
  double base_distance = shell_global_get_screen_height (shell_global_get ());
  StAdjustment *adjustment;
  ClutterTransition *transition;
  double progress, cancel_progress;

  g_return_if_fail (SHELL_IS_CONTROLS_MANAGER (self));
  g_return_if_fail (SHELL_IS_SWIPE_TRACKER (tracker));

  adjustment = ST_ADJUSTMENT (self->state_adjustment);
  progress = st_adjustment_get_value (adjustment);

  transition = st_adjustment_get_transition (adjustment, "value");
  cancel_progress = js_round (progress);
  if (transition != NULL)
    {
      ClutterInterval *interval = clutter_transition_get_interval (transition);
      const GValue *final_value = interval != NULL ? clutter_interval_peek_final_value (interval) : NULL;

      if (final_value != NULL && G_VALUE_HOLDS_DOUBLE (final_value))
        cancel_progress = g_value_get_double (final_value);
    }
  st_adjustment_remove_transition (adjustment, "value");

  shell_swipe_tracker_confirm_swipe (tracker, base_distance, points, G_N_ELEMENTS (points),
                                     progress, cancel_progress);
  shell_controls_manager_prepare_to_enter_overview (self);
  shell_overview_adjustment_set_gesture_in_progress (self->state_adjustment, TRUE);
}

/**
 * shell_controls_manager_gesture_progress:
 * @self: a #ShellControlsManager
 * @progress: the state the swipe is at
 *
 * Moves the controls with the swipe (`gestureProgress ()`).
 */
void
shell_controls_manager_gesture_progress (ShellControlsManager *self,
                                         double                progress)
{
  g_return_if_fail (SHELL_IS_CONTROLS_MANAGER (self));

  st_adjustment_set_value (ST_ADJUSTMENT (self->state_adjustment), progress);
}

/**
 * shell_controls_manager_gesture_end: (skip)
 * @self: a #ShellControlsManager
 * @target: the state the swipe ends at
 * @duration: the duration of the animation to @target in milliseconds
 * @callback: (nullable): called when the animation stopped; the overview
 *   only cares when it @finished
 * @user_data: data for @callback
 * @destroy: called on @user_data after @callback
 *
 * Animates the controls to @target after a swipe (`gestureEnd ()`).
 */
void
shell_controls_manager_gesture_end (ShellControlsManager  *self,
                                    double                 target,
                                    guint                  duration,
                                    ShellControlsCallback  callback,
                                    gpointer               user_data,
                                    GDestroyNotify         destroy)
{
  StAdjustment *adjustment;

  g_return_if_fail (SHELL_IS_CONTROLS_MANAGER (self));

  if (target == SHELL_CONTROLS_STATE_HIDDEN)
    shell_controls_manager_prepare_to_leave_overview (self);

  controls_set_show_apps_checked (self, target == SHELL_CONTROLS_STATE_APP_GRID);

  adjustment = ST_ADJUSTMENT (self->state_adjustment);
  st_adjustment_remove_transition (adjustment, "value");
  ease_adjustment (adjustment, target, duration, CLUTTER_EASE_OUT_CUBIC,
                   callback, user_data, destroy);

  shell_overview_adjustment_set_gesture_in_progress (self->state_adjustment, FALSE);
}

/* ---- The startup animation ---------------------------------------------- */

/* Calls the callback when every animation of the startup ended
 * (Promise.allSettled ()). Every operation in flight holds a reference to the
 * data, which its destroy notify drops, so that the data is freed whether the
 * operation completes or is dropped (the shell shuts down). */
typedef struct
{
  grefcount ref_count;
  int pending;
  ShellControlsCallback callback;
  gpointer user_data;
  GDestroyNotify destroy;
  GWeakRef *controls;
} StartupData;

static StartupData *
startup_data_ref (StartupData *data)
{
  g_ref_count_inc (&data->ref_count);

  return data;
}

static void
startup_data_unref (gpointer user_data)
{
  StartupData *data = user_data;

  if (!g_ref_count_dec (&data->ref_count))
    return;

  if (data->destroy != NULL)
    data->destroy (data->user_data);
  weak_ref_free (data->controls);
  g_free (data);
}

/* Starts an operation which ends with startup_op_done () */
static StartupData *
startup_op_begin (StartupData *data)
{
  data->pending++;

  return startup_data_ref (data);
}

static void
startup_op_done (StartupData *data)
{
  data->pending--;
  if (data->pending > 0 || data->callback == NULL)
    return;

  /* The callback is called once */
  data->callback (TRUE, data->user_data);
  data->callback = NULL;
}

static void
startup_ease_done_cb (gboolean finished,
                      gpointer user_data)
{
  startup_op_done (user_data);
}

static void
startup_allocated_cb (gboolean finished,
                      gpointer user_data)
{
  StartupData *data = user_data;
  g_autoptr (ShellControlsManager) self = g_weak_ref_get (data->controls);
  ShellEaseParams opacity_params, search_params, dash_params;
  g_autoptr (ShellMonitor) primary_monitor = NULL;
  ClutterActor *dash;
  float y = 0;
  float y_offset;

  if (self != NULL && self->search_entry_bin != NULL && self->dash != NULL &&
      self->layout_manager != NULL)
    {
      dash = CLUTTER_ACTOR (self->dash);

      /* Opacity */
      opacity_params = ease_params (SHELL_STARTUP_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
      shell_actor_ease (CLUTTER_ACTOR (self), &opacity_params,
                        startup_ease_done_cb, startup_op_begin (data), startup_data_unref,
                        "opacity", 255,
                        NULL);

      /* Search bar falls from the ceiling */
      primary_monitor = shell_layout_manager_get_primary_monitor (self->layout_manager);
      clutter_actor_get_transformed_position (self->search_entry_bin, NULL, &y);
      y_offset = y - (primary_monitor != NULL ? primary_monitor->y : 0);
      /* A stage without a monitor yet (the devkit backend adds it when its
       * viewer connects) has no projection, and an eased NaN would keep the
       * entry out of sight for good */
      if (!isfinite (y_offset))
        y_offset = 0;

      clutter_actor_set_translation (self->search_entry_bin, 0,
                                     -(y_offset + clutter_actor_get_height (self->search_entry_bin)), 0);
      search_params = ease_params (SHELL_STARTUP_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
      shell_actor_ease (self->search_entry_bin, &search_params,
                        startup_ease_done_cb, startup_op_begin (data), startup_data_unref,
                        "translation-y", 0.0,
                        NULL);

      /* The Dash rises from the bottom. This is the last animation to finish,
       * so resolve the promise there. */
      clutter_actor_set_translation (dash, 0,
                                     clutter_actor_get_height (dash) +
                                     clutter_actor_get_margin_bottom (dash), 0);
      dash_params = ease_params (SHELL_STARTUP_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
      dash_params.delay = SHELL_STARTUP_ANIMATION_TIME;
      shell_actor_ease (dash, &dash_params,
                        startup_ease_done_cb, startup_op_begin (data), startup_data_unref,
                        "translation-y", 0.0,
                        NULL);
    }

  /* The wait for the allocation is over */
  startup_op_done (data);
}

/**
 * shell_controls_manager_run_startup_animation: (skip)
 * @self: a #ShellControlsManager
 * @callback: (nullable): called when the animations ended
 * @user_data: data for @callback
 * @destroy: called on @user_data after @callback, or when the animation is
 *   dropped
 *
 * Runs the animation of the controls at the end of the startup
 * (`runStartupAnimation ()`): the controls fade in, the search entry falls
 * from the top and the dash rises from the bottom.
 */
void
shell_controls_manager_run_startup_animation (ShellControlsManager  *self,
                                              ShellControlsCallback  callback,
                                              gpointer               user_data,
                                              GDestroyNotify         destroy)
{
  StartupData *data;
  ShellControlsLayout *layout;
  PostAllocation *post;

  g_return_if_fail (SHELL_IS_CONTROLS_MANAGER (self));

  self->ignore_show_apps_button_toggle = TRUE;

  shell_controls_manager_prepare_to_enter_overview (self);

  data = g_new0 (StartupData, 1);
  g_ref_count_init (&data->ref_count);
  data->callback = callback;
  data->user_data = user_data;
  data->destroy = destroy;
  data->controls = weak_ref_new (self);

  /* Hold the animation open until the allocation step is registered */
  data->pending = 1;

  st_adjustment_set_value (ST_ADJUSTMENT (self->state_adjustment), SHELL_CONTROLS_STATE_HIDDEN);
  ease_adjustment (ST_ADJUSTMENT (self->state_adjustment), SHELL_CONTROLS_STATE_WINDOW_PICKER,
                   SHELL_OVERVIEW_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD,
                   startup_ease_done_cb, startup_op_begin (data), startup_data_unref);

  controls_set_show_apps_checked (self, FALSE);
  self->ignore_show_apps_button_toggle = FALSE;

  /* Set the opacity here to avoid a 1-frame flicker */
  clutter_actor_set_opacity (CLUTTER_ACTOR (self), 0);

  /* We can't run the animation before the first allocation happens */
  layout = controls_get_layout (self);
  if (layout != NULL)
    {
      post = g_new0 (PostAllocation, 1);
      post->callback = startup_allocated_cb;
      post->user_data = startup_op_begin (data);
      post->destroy = startup_data_unref;
      layout->post_allocation_callbacks = g_list_prepend (layout->post_allocation_callbacks, post);
      clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (layout));
    }

  startup_op_done (data);
  startup_data_unref (data);
}
