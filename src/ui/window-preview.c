/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * window-preview.c: The clone of a window in the window picker of the
 * overview. Port of js/ui/windowPreview.js (`WindowPreview extends
 * Shell.WindowPreview`); the JS subclass is merged into the C class that it
 * extended, which keeps its JS name `Shell.WindowPreview`.
 *
 * Differences from the JS module that every reader of this file should know
 * about:
 *
 *  - `new WindowPreview (metaWindow, workspace, overviewAdjustment)` is
 *    `new Shell.WindowPreview ({meta_window, workspace, overview_adjustment})`
 *    (all three are construct-only). `workspace` is the #ShellDragTarget
 *    that the drags over the preview are forwarded to (the workspace that
 *    created it), it is held weakly. `window_container` is created by the
 *    preview when it is not passed.
 *  - `metaWindow` is the read-only `meta-window` property. The
 *    `metaWindow._delegate` and `_delegate` back pointers are gone: the
 *    preview is its own #ShellDragTarget and the overview reads
 *    `meta_window` of the preview under a drag.
 *  - The methods are snake_case: `showOverlay (animate)` is
 *    `show_overlay (animate)`, `hideOverlay`, `addDialog`, `setStackAbove
 *    (actor)` is `set_stack_above (preview)` (the preview is always the
 *    previous preview of the same workspace, or null), `overlapHeights ()`,
 *    `chromeHeights ()` and `chromeWidths ()` return the two numbers as an
 *    array, as they did. `boundingBox` and `windowCenter` are
 *    `get_bounding_box ()` (a `Clutter.ActorBox`) and `get_window_center ()`.
 *    `overlayEnabled` is the `overlay-enabled` property. `inDrag` and the
 *    other `_` members are private.
 *  - The `selected`, `show-chrome`, `size-changed`, `drag-begin`,
 *    `drag-cancelled` and `drag-end` signals are unchanged.
 *  - `WINDOW_DND_SIZE` and `DRAGGING_WINDOW_OPACITY` are the constants
 *    `Shell.WINDOW_PREVIEW_DND_SIZE` and `Shell.WINDOW_PREVIEW_DRAGGING_OPACITY`.
 *  - The state of the overview that scales the icon is read from the
 *    adjustment itself (shell_window_preview_get_state_transition_params ()
 *    is `OverviewAdjustment.getStateTransitionParams ()` without the gesture
 *    state, which the preview never used).
 *  - A preview whose stack-above preview was destroyed forgets it instead of
 *    keeping a destroyed actor, and restacking never raises a warning for a
 *    sibling that is not in the parent any more (reparented by a drag).
 */

#include "config.h"

#include <math.h>

#include <atk/atk.h>
#include <clutter/clutter.h>
/* clutter.h does not include the motion controller (mutter 51) */
#define __CLUTTER_H_INSIDE__
#include <clutter/clutter-motion-controller.h>
#undef __CLUTTER_H_INSIDE__
#include <clutter/clutter-pango.h>
#include <meta/common.h>
#include <meta/prefs.h>

#include "shell-app.h"
#include "shell-global.h"
#include "shell-util.h"
#include "shell-window-preview-layout.h"
#include "shell-window-tracker.h"
#include "ui/environment.h"
#include "ui/window-preview.h"

#define WINDOW_OVERLAY_IDLE_HIDE_TIMEOUT 750
#define WINDOW_OVERLAY_FADE_TIME 200

#define WINDOW_SCALE_TIME 200
#define WINDOW_ACTIVE_SIZE_INC 5 /* in each direction */

#define ICON_SIZE 64
#define ICON_OVERLAP 0.7

#define ICON_TITLE_SPACING 6

/* OverviewControls.ControlsState.WINDOW_PICKER */
#define CONTROLS_STATE_WINDOW_PICKER 1.0

enum
{
  PROP_0,

  PROP_WINDOW_CONTAINER,
  PROP_META_WINDOW,
  PROP_WORKSPACE,
  PROP_OVERVIEW_ADJUSTMENT,
  PROP_OVERLAY_ENABLED,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  DRAG_BEGIN,
  DRAG_CANCELLED,
  DRAG_END,
  SELECTED,
  SHOW_CHROME,
  SIZE_CHANGED,

  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0, };

struct _ShellWindowPreview
{
  /*< private >*/
  StWidget parent_instance;

  ClutterActor *window_container;
  ShellWindowPreviewLayout *container_layout; /* owned by window_container */

  MetaWindow *meta_window;
  ShellDragTarget *workspace;           /* weak */
  StAdjustment *overview_adjustment;

  ClutterActorBox cached_bounding_box;

  ShellWindowPreview *stack_above;      /* weak */

  ClutterActor *icon;
  ClutterActor *title;
  ClutterActor *close_button;
  StSide close_button_side;

  gboolean overlay_enabled;
  gboolean overlay_shown;
  gboolean close_requested;
  gboolean in_drag;
  gboolean destroyed;

  guint idle_hide_overlay_id;
};

static void shell_window_preview_drag_target_iface_init (ShellDragTargetInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellWindowPreview, shell_window_preview, ST_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (SHELL_TYPE_DRAG_TARGET,
                                                      shell_window_preview_drag_target_iface_init))

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static ClutterStage *
get_stage (void)
{
  return shell_global_get_stage (shell_global_get ());
}

static int
get_scale_factor (void)
{
  StThemeContext *context = st_theme_context_get_for_stage (get_stage ());

  return st_theme_context_get_scale_factor (context);
}

/* Whether the opacity transition of @actor, if it is running, goes to
 * @opacity. */
static gboolean
opacity_transition_goes_to (ClutterActor *actor,
                            int           opacity)
{
  ClutterTransition *transition = clutter_actor_get_transition (actor, "opacity");
  ClutterInterval *interval;
  const GValue *final_value;
  GValue value = G_VALUE_INIT;
  gboolean result = FALSE;

  if (transition == NULL)
    return FALSE;

  interval = clutter_transition_get_interval (transition);
  if (interval == NULL)
    return FALSE;

  final_value = clutter_interval_peek_final_value (interval);
  if (final_value == NULL)
    return FALSE;

  g_value_init (&value, G_TYPE_INT);
  if (g_value_transform (final_value, &value))
    result = g_value_get_int (&value) == opacity;
  g_value_unset (&value);

  return result;
}

static void
hide_actor_done_cb (gboolean finished,
                    gpointer user_data)
{
  if (finished)
    clutter_actor_hide (CLUTTER_ACTOR (user_data));
}

static void
add_bind_constraint (ClutterActor        *actor,
                     ClutterActor        *source,
                     ClutterBindCoordinate coordinate,
                     float                offset)
{
  clutter_actor_add_constraint (actor,
                                clutter_bind_constraint_new (source,
                                                             coordinate,
                                                             offset));
}

static void
add_align_constraint (ClutterActor *actor,
                      ClutterActor *source,
                      ClutterAlignAxis axis,
                      float         factor,
                      float         pivot_x,
                      float         pivot_y)
{
  ClutterConstraint *constraint = clutter_align_constraint_new (source, axis, factor);

  if (pivot_x != -1 || pivot_y != -1)
    {
      graphene_point_t pivot = GRAPHENE_POINT_INIT (pivot_x, pivot_y);

      clutter_align_constraint_set_pivot_point (CLUTTER_ALIGN_CONSTRAINT (constraint),
                                                &pivot);
    }

  clutter_actor_add_constraint (actor, constraint);
}

/**
 * shell_window_preview_get_state_transition_params: (skip)
 * @adjustment: the adjustment of the overview (`OverviewAdjustment`)
 * @current_state: (out): the current value
 * @initial_state: (out): the state the running transition started from, or
 *   the current value
 * @final_state: (out): the state the running transition goes to, or the
 *   current value
 *
 * The part of `OverviewAdjustment.getStateTransitionParams ()` that the
 * window previews and the workspaces use. The initial and final states are
 * rounded to the states they lie between.
 */
void
shell_window_preview_get_state_transition_params (StAdjustment *adjustment,
                                                  double       *current_state,
                                                  double       *initial_state,
                                                  double       *final_state)
{
  ClutterTransition *transition;
  double current, initial, final_;

  g_return_if_fail (ST_IS_ADJUSTMENT (adjustment));

  current = st_adjustment_get_value (adjustment);
  initial = current;
  final_ = current;

  transition = st_adjustment_get_transition (adjustment, "value");
  if (transition != NULL)
    {
      ClutterInterval *interval = clutter_transition_get_interval (transition);

      if (interval != NULL)
        {
          const GValue *initial_value = clutter_interval_peek_initial_value (interval);
          const GValue *final_value = clutter_interval_peek_final_value (interval);

          if (initial_value != NULL && G_VALUE_HOLDS_DOUBLE (initial_value))
            initial = g_value_get_double (initial_value);
          if (final_value != NULL && G_VALUE_HOLDS_DOUBLE (final_value))
            final_ = g_value_get_double (final_value);
        }
    }

  if (initial > final_)
    {
      initial = ceil (initial);
      final_ = floor (final_);
    }
  else
    {
      initial = floor (initial);
      final_ = ceil (final_);
    }

  if (current_state != NULL)
    *current_state = current;
  if (initial_state != NULL)
    *initial_state = initial;
  if (final_state != NULL)
    *final_state = final_;
}

static gboolean
has_attached_dialogs (ShellWindowPreview *self)
{
  g_autoptr (GList) windows = NULL;

  if (self->container_layout == NULL)
    return FALSE;

  windows = shell_window_preview_layout_get_windows (self->container_layout);

  return windows != NULL && windows->next != NULL;
}

static gboolean
window_can_close (ShellWindowPreview *self)
{
  return meta_window_can_close (self->meta_window) &&
         !has_attached_dialogs (self);
}

static char *
get_caption (ShellWindowPreview *self)
{
  ShellWindowTracker *tracker = shell_window_tracker_get_default ();
  g_autoptr (ShellApp) app = NULL;
  const char *title = meta_window_get_title (self->meta_window);

  if (title != NULL && *title != '\0')
    return g_strdup (title);

  app = shell_window_tracker_get_window_app (tracker, self->meta_window);

  return g_strdup (app != NULL ? shell_app_get_name (app) : "");
}

static void
activate (ShellWindowPreview *self)
{
  g_signal_emit (self, signals[SELECTED], 0,
                 shell_global_get_current_time (shell_global_get ()));
}

static void
set_child_above_sibling_checked (ClutterActor *parent,
                                 ClutterActor *child,
                                 ClutterActor *sibling)
{
  /* The sibling may have been reparented by a drag */
  if (sibling != NULL && clutter_actor_get_parent (sibling) != parent)
    return;

  clutter_actor_set_child_above_sibling (parent, child, sibling);
}

static void
restack (ShellWindowPreview *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *parent = clutter_actor_get_parent (actor);
  ShellWindowPreview *stack_above = self->stack_above;

  /* We may not have a parent if DnD completed successfully, in
   * which case our clone will shortly be destroyed and replaced
   * with a new one on the target workspace. */
  if (parent == NULL)
    return;

  if (self->overlay_shown)
    {
      clutter_actor_set_child_above_sibling (parent, actor, NULL);
    }
  else if (stack_above == NULL)
    {
      clutter_actor_set_child_below_sibling (parent, actor, NULL);
    }
  else if (!stack_above->overlay_shown)
    {
      set_child_above_sibling_checked (parent, actor, CLUTTER_ACTOR (stack_above));
    }
  else
    {
      /* This window shall still be above stack_above->stack_above */
      ShellWindowPreview *above = stack_above->stack_above;

      if (above == NULL)
        clutter_actor_set_child_below_sibling (parent, actor, NULL);
      else
        set_child_above_sibling_checked (parent, actor, CLUTTER_ACTOR (above));
    }
}

/* Find the actor just below us, respecting reparenting done by DND code */
static ShellWindowPreview *
get_actual_stack_above (ShellWindowPreview *self)
{
  if (self->stack_above == NULL)
    return NULL;

  if (self->in_drag)
    {
      if (self->stack_above->destroyed)
        return NULL;

      return get_actual_stack_above (self->stack_above);
    }

  return self->stack_above;
}

static void
update_icon_scale (ShellWindowPreview *self)
{
  double current_state, initial_state, final_state;
  gboolean visible;
  double scale;

  if (self->overview_adjustment == NULL || self->icon == NULL)
    return;

  shell_window_preview_get_state_transition_params (self->overview_adjustment,
                                                    &current_state,
                                                    &initial_state,
                                                    &final_state);

  visible = initial_state == CONTROLS_STATE_WINDOW_PICKER ||
            final_state == CONTROLS_STATE_WINDOW_PICKER;
  scale = visible
    ? 1 - fabs (CONTROLS_STATE_WINDOW_PICKER - current_state)
    : 0;

  clutter_actor_set_scale (self->icon, scale, scale);
}

static void
adjust_overlay_offsets (ShellWindowPreview *self)
{
  ClutterActorBox box;
  double preview_scale;
  double preview_width, preview_height;
  double height_increase, width_increase, close_align;

  /* Assume that scale-x and scale-y update always set
   * in lock-step; that allows us to not use separate
   * handlers for horizontal and vertical offsets */
  clutter_actor_get_scale (self->window_container, &preview_scale, NULL);
  clutter_actor_get_allocation_box (self->window_container, &box);
  preview_width = clutter_actor_box_get_width (&box);
  preview_height = clutter_actor_box_get_height (&box);

  height_increase = floor (preview_height * (preview_scale - 1) / 2);
  width_increase = floor (preview_width * (preview_scale - 1) / 2);

  close_align = self->close_button_side == ST_SIDE_LEFT ? -1 : 1;

  clutter_actor_set_translation (self->icon, 0, height_increase, 0);
  clutter_actor_set_translation (self->title, 0, height_increase, 0);
  clutter_actor_set_translation (self->close_button,
                                 close_align * width_increase,
                                 -height_increase,
                                 0);
}

static void
add_window (ShellWindowPreview *self,
            MetaWindow         *window)
{
  ClutterActor *clone;

  /* The compositor does not know about the window yet */
  if (meta_window_get_compositor_private (window) == NULL)
    return;

  clone = shell_window_preview_layout_add_window (self->container_layout, window);
  if (clone == NULL)
    return;

  /* We expect this to be used for all interaction rather than
   * the ClutterClone; as the former is reactive and the latter
   * is not, this just works for most cases. However, for DND all
   * actors are picked, so DND operations would operate on the clone.
   * To avoid this, we hide it from pick. */
  shell_util_set_hidden_from_pick (clone, TRUE);
}

static gboolean
update_attached_dialogs_iter (MetaWindow *window,
                              gpointer    user_data)
{
  ShellWindowPreview *self = user_data;

  if (meta_window_get_compositor_private (window) == NULL)
    return FALSE;
  if (!meta_window_is_attached_dialog (window))
    return FALSE;

  add_window (self, window);
  meta_window_foreach_transient (window, update_attached_dialogs_iter, self);
  return TRUE;
}

static void
update_attached_dialogs (ShellWindowPreview *self)
{
  meta_window_foreach_transient (self->meta_window,
                                 update_attached_dialogs_iter,
                                 self);
}

/* ------------------------------------------------------------------------ */
/* Overlay                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * shell_window_preview_show_overlay:
 * @preview: a #ShellWindowPreview
 * @animate: whether to fade in the title and the close button
 *
 * Shows the title and the close button of the preview, and enlarges it
 * (`showOverlay ()`). Does nothing when the overlay is disabled.
 */
void
shell_window_preview_show_overlay (ShellWindowPreview *self,
                                   gboolean            animate)
{
  ShellEaseParams fade_params = {
    .duration = animate ? WINDOW_OVERLAY_FADE_TIME : 0,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
  };
  ShellEaseParams scale_params = {
    .duration = animate ? WINDOW_SCALE_TIME : 0,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
  };
  ClutterActor *to_show[2];
  int n_to_show = 0;
  float width, height, orig_size;
  double active_extra_size, scale;
  int i;

  g_return_if_fail (SHELL_IS_WINDOW_PREVIEW (self));

  if (!self->overlay_enabled)
    return;

  if (self->overlay_shown)
    return;

  self->overlay_shown = TRUE;
  restack (self);

  /* If we're supposed to animate and an animation in our direction
   * is already happening, let that one continue */
  if (animate && opacity_transition_goes_to (self->title, 255))
    return;

  to_show[n_to_show++] = self->title;
  if (window_can_close (self))
    to_show[n_to_show++] = self->close_button;

  for (i = 0; i < n_to_show; i++)
    {
      clutter_actor_set_opacity (to_show[i], 0);
      clutter_actor_show (to_show[i]);
      shell_actor_ease (to_show[i], &fade_params, NULL, NULL, NULL,
                        "opacity", 255,
                        NULL);
    }

  clutter_actor_get_size (self->window_container, &width, &height);
  active_extra_size = WINDOW_ACTIVE_SIZE_INC * 2 * get_scale_factor ();
  orig_size = MAX (width, height);
  scale = (orig_size + active_extra_size) / orig_size;

  shell_actor_ease (self->window_container, &scale_params, NULL, NULL, NULL,
                    "scale-x", scale,
                    "scale-y", scale,
                    NULL);

  g_signal_emit (self, signals[SHOW_CHROME], 0);
}

/**
 * shell_window_preview_hide_overlay:
 * @preview: a #ShellWindowPreview
 * @animate: whether to fade out the title and the close button
 *
 * Hides the title and the close button of the preview, and gives it its size
 * back (`hideOverlay ()`).
 */
void
shell_window_preview_hide_overlay (ShellWindowPreview *self,
                                   gboolean            animate)
{
  ShellEaseParams fade_params = {
    .duration = animate ? WINDOW_OVERLAY_FADE_TIME : 0,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
  };
  ShellEaseParams scale_params = {
    .duration = animate ? WINDOW_SCALE_TIME : 0,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
  };
  ClutterActor *to_hide[2];
  int i;

  g_return_if_fail (SHELL_IS_WINDOW_PREVIEW (self));

  if (!self->overlay_shown)
    return;

  self->overlay_shown = FALSE;
  restack (self);

  /* If we're supposed to animate and an animation in our direction
   * is already happening, let that one continue */
  if (animate && opacity_transition_goes_to (self->title, 0))
    return;

  to_hide[0] = self->title;
  to_hide[1] = self->close_button;

  for (i = 0; i < 2; i++)
    {
      clutter_actor_set_opacity (to_hide[i], 255);
      shell_actor_ease (to_hide[i], &fade_params,
                        hide_actor_done_cb, g_object_ref (to_hide[i]), g_object_unref,
                        "opacity", 0,
                        NULL);
    }

  shell_actor_ease (self->window_container, &scale_params, NULL, NULL, NULL,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    NULL);
}

/**
 * shell_window_preview_get_overlay_enabled:
 * @preview: a #ShellWindowPreview
 *
 * Returns: whether the title and close button are shown when the pointer or
 *   the key focus is on the preview
 */
gboolean
shell_window_preview_get_overlay_enabled (ShellWindowPreview *self)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_PREVIEW (self), FALSE);

  return self->overlay_enabled;
}

/**
 * shell_window_preview_set_overlay_enabled:
 * @preview: a #ShellWindowPreview
 * @enabled: whether to show the overlay
 *
 * Sets #ShellWindowPreview:overlay-enabled. Disabling hides the overlay at
 * once, enabling shows it if the pointer or the key focus is on the preview.
 */
void
shell_window_preview_set_overlay_enabled (ShellWindowPreview *self,
                                          gboolean            enabled)
{
  g_return_if_fail (SHELL_IS_WINDOW_PREVIEW (self));

  enabled = !!enabled;
  if (self->overlay_enabled == enabled)
    return;

  self->overlay_enabled = enabled;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OVERLAY_ENABLED]);

  if (!enabled)
    {
      shell_window_preview_hide_overlay (self, FALSE);
    }
  else if (clutter_actor_has_pointer (CLUTTER_ACTOR (self)) ||
           clutter_stage_get_key_focus (get_stage ()) == CLUTTER_ACTOR (self))
    {
      shell_window_preview_show_overlay (self, TRUE);
    }
}

/**
 * shell_window_preview_overlap_heights:
 * @preview: a #ShellWindowPreview
 * @top: (out): the height the overlay extends above the preview
 * @bottom: (out): the height the overlay extends below the preview
 *
 * How far the title extends over the preview's neighbours (`overlapHeights ()`).
 */
void
shell_window_preview_overlap_heights (ShellWindowPreview *self,
                                      double             *top,
                                      double             *bottom)
{
  float title_height;

  g_return_if_fail (SHELL_IS_WINDOW_PREVIEW (self));

  clutter_actor_get_preferred_height (self->title, -1, NULL, &title_height);

  if (top != NULL)
    *top = 0;
  if (bottom != NULL)
    *bottom = ICON_TITLE_SPACING + title_height;
}

/**
 * shell_window_preview_chrome_heights:
 * @preview: a #ShellWindowPreview
 * @top: (out): the height the chrome extends above the preview
 * @bottom: (out): the height the chrome extends below the preview
 *
 * How far the close button, the icon and the enlarged preview extend over
 * the preview itself (`chromeHeights ()`).
 */
void
shell_window_preview_chrome_heights (ShellWindowPreview *self,
                                     double             *top,
                                     double             *bottom)
{
  float close_button_height, icon_height;
  double active_extra_size;

  g_return_if_fail (SHELL_IS_WINDOW_PREVIEW (self));

  clutter_actor_get_preferred_height (self->close_button, -1, NULL, &close_button_height);
  clutter_actor_get_preferred_height (self->icon, -1, NULL, &icon_height);
  active_extra_size = WINDOW_ACTIVE_SIZE_INC * get_scale_factor ();

  if (top != NULL)
    *top = close_button_height / 2 + active_extra_size;
  if (bottom != NULL)
    *bottom = (1 - ICON_OVERLAP) * icon_height + active_extra_size;
}

/**
 * shell_window_preview_chrome_widths:
 * @preview: a #ShellWindowPreview
 * @left: (out): the width the chrome extends left of the preview
 * @right: (out): the width the chrome extends right of the preview
 *
 * How far the close button and the enlarged preview extend over the preview
 * itself (`chromeWidths ()`).
 */
void
shell_window_preview_chrome_widths (ShellWindowPreview *self,
                                    double             *left,
                                    double             *right)
{
  float close_button_width;
  double active_extra_size;

  g_return_if_fail (SHELL_IS_WINDOW_PREVIEW (self));

  clutter_actor_get_preferred_width (self->close_button, -1, NULL, &close_button_width);
  active_extra_size = WINDOW_ACTIVE_SIZE_INC * get_scale_factor ();

  if (left != NULL)
    {
      *left = (self->close_button_side == ST_SIDE_LEFT ? close_button_width / 2 : 0) +
              active_extra_size;
    }
  if (right != NULL)
    {
      *right = (self->close_button_side == ST_SIDE_LEFT ? 0 : close_button_width / 2) +
               active_extra_size;
    }
}

/* ------------------------------------------------------------------------ */
/* Windows                                                                  */
/* ------------------------------------------------------------------------ */

static void
delete_all (ShellWindowPreview *self)
{
  g_autoptr (GList) windows =
    shell_window_preview_layout_get_windows (self->container_layout);
  GList *l;
  guint32 time = shell_global_get_current_time (shell_global_get ());

  /* Delete all windows, starting from the bottom-most (most-modal) one */
  windows = g_list_reverse (windows);
  for (l = windows; l != NULL; l = l->next)
    meta_window_delete (l->data, time);

  self->close_requested = TRUE;
}

/**
 * shell_window_preview_add_dialog:
 * @preview: a #ShellWindowPreview
 * @window: a window that was added to the workspace of @preview
 *
 * Shows @window in the preview if it is a dialog attached to the window of
 * the preview. If the user asked to close the window before, @window is
 * taken for a close confirmation and the preview is activated.
 */
void
shell_window_preview_add_dialog (ShellWindowPreview *self,
                                 MetaWindow         *window)
{
  MetaWindow *parent;

  g_return_if_fail (SHELL_IS_WINDOW_PREVIEW (self));
  g_return_if_fail (META_IS_WINDOW (window));

  parent = meta_window_get_transient_for (window);
  while (parent != NULL && meta_window_is_attached_dialog (parent))
    parent = meta_window_get_transient_for (parent);

  /* Display dialog if it is attached to our meta_window */
  if (meta_window_is_attached_dialog (window) && parent == self->meta_window)
    add_window (self, window);

  /* The dialog popped up after the user tried to close the window,
   * assume it's a close confirmation and leave the overview */
  if (self->close_requested)
    activate (self);
}

/**
 * shell_window_preview_get_meta_window:
 * @preview: a #ShellWindowPreview
 *
 * Returns: (transfer none): the window that @preview shows (`metaWindow`)
 */
MetaWindow *
shell_window_preview_get_meta_window (ShellWindowPreview *self)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_PREVIEW (self), NULL);

  return self->meta_window;
}

/**
 * shell_window_preview_get_window_container:
 * @preview: a #ShellWindowPreview
 *
 * Returns: (transfer none): the actor that holds the clones of the window and
 *   of its attached dialogs
 */
ClutterActor *
shell_window_preview_get_window_container (ShellWindowPreview *self)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_PREVIEW (self), NULL);

  return self->window_container;
}

/**
 * shell_window_preview_get_bounding_box:
 * @preview: a #ShellWindowPreview
 *
 * Returns: (transfer none): the bounding box of the window and its attached
 *   dialogs in the coordinates of the workspace, as it was when the size of
 *   the preview last changed (`boundingBox`)
 */
const ClutterActorBox *
shell_window_preview_get_bounding_box (ShellWindowPreview *self)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_PREVIEW (self), NULL);

  return &self->cached_bounding_box;
}

/**
 * shell_window_preview_get_window_center:
 * @preview: a #ShellWindowPreview
 * @x: (out): the X coordinate of the center of the bounding box
 * @y: (out): the Y coordinate of the center of the bounding box
 *
 * `windowCenter`
 */
void
shell_window_preview_get_window_center (ShellWindowPreview *self,
                                        double             *x,
                                        double             *y)
{
  const ClutterActorBox *box;

  g_return_if_fail (SHELL_IS_WINDOW_PREVIEW (self));

  box = &self->cached_bounding_box;
  if (x != NULL)
    *x = box->x1 + (box->x2 - box->x1) / 2;
  if (y != NULL)
    *y = box->y1 + (box->y2 - box->y1) / 2;
}

/**
 * shell_window_preview_set_stack_above:
 * @preview: a #ShellWindowPreview
 * @above: (nullable): the preview that is directly below @preview in the
 *   stacking order of the workspace, or %NULL for the bottom-most
 *
 * Restacks @preview above @above (`setStackAbove ()`), unless it is being
 * dragged, in which case the stacking is fixed up when the drag ends.
 */
void
shell_window_preview_set_stack_above (ShellWindowPreview *self,
                                      ShellWindowPreview *above)
{
  ClutterActor *parent;
  ShellWindowPreview *actual_above;

  g_return_if_fail (SHELL_IS_WINDOW_PREVIEW (self));
  g_return_if_fail (above == NULL || SHELL_IS_WINDOW_PREVIEW (above));

  g_set_weak_pointer (&self->stack_above, above);

  if (self->in_drag)
    /* We'll fix up the stack after the drag */
    return;

  parent = clutter_actor_get_parent (CLUTTER_ACTOR (self));
  if (parent == NULL)
    return;

  actual_above = get_actual_stack_above (self);
  if (actual_above == NULL)
    clutter_actor_set_child_below_sibling (parent, CLUTTER_ACTOR (self), NULL);
  else
    set_child_above_sibling_checked (parent, CLUTTER_ACTOR (self), CLUTTER_ACTOR (actual_above));
}

/* ------------------------------------------------------------------------ */
/* Events                                                                   */
/* ------------------------------------------------------------------------ */

static void
on_enter (ShellWindowPreview *self)
{
  shell_window_preview_show_overlay (self, TRUE);
}

static gboolean
idle_hide_overlay_cb (gpointer user_data)
{
  ShellWindowPreview *self = user_data;

  if (clutter_actor_has_pointer (self->close_button) ||
      clutter_actor_has_pointer (self->icon) ||
      clutter_actor_has_pointer (self->title))
    return G_SOURCE_CONTINUE;

  if (!clutter_actor_has_pointer (CLUTTER_ACTOR (self)))
    shell_window_preview_hide_overlay (self, TRUE);

  self->idle_hide_overlay_id = 0;
  return G_SOURCE_REMOVE;
}

static void
on_leave (ShellWindowPreview *self)
{
  if (self->destroyed)
    return;

  g_clear_handle_id (&self->idle_hide_overlay_id, g_source_remove);

  self->idle_hide_overlay_id =
    g_timeout_add (WINDOW_OVERLAY_IDLE_HIDE_TIMEOUT, idle_hide_overlay_cb, self);
  g_source_set_name_by_id (self->idle_hide_overlay_id,
                           "[gnome-shell] this._idleHideOverlayId");
}

static void
on_long_press (ShellWindowPreview *self)
{
  shell_window_preview_show_overlay (self, TRUE);
}

static void
on_click (ShellWindowPreview *self)
{
  activate (self);
}

static void
on_close_clicked (ShellWindowPreview *self)
{
  delete_all (self);
}

static gboolean
activate_binding_cb (GObject             *object,
                     const char          *action_name,
                     guint                key_val,
                     ClutterModifierType  modifiers,
                     gpointer             user_data)
{
  activate (SHELL_WINDOW_PREVIEW (object));
  return CLUTTER_EVENT_STOP;
}

static void
on_window_title_changed (ShellWindowPreview *self)
{
  g_autofree char *caption = get_caption (self);

  st_label_set_text (ST_LABEL (self->title), caption);
}

static void
on_window_actor_destroyed (ClutterActor       *window_actor,
                           ShellWindowPreview *self)
{
  clutter_actor_destroy (CLUTTER_ACTOR (self));
}

static void
on_bounding_box_changed (GObject            *layout,
                         GParamSpec         *pspec,
                         ShellWindowPreview *self)
{
  const ClutterActorBox *box =
    shell_window_preview_layout_get_bounding_box (SHELL_WINDOW_PREVIEW_LAYOUT (layout));

  self->cached_bounding_box = *box;

  /* A bounding box of 0x0 means all windows were removed */
  if (clutter_actor_box_get_area (box) > 0)
    g_signal_emit (self, signals[SIZE_CHANGED], 0);
}

static void
on_realized_changed (ShellWindowPreview *self)
{
  if (!clutter_actor_is_realized (CLUTTER_ACTOR (self)))
    return;

  st_widget_ensure_style (ST_WIDGET (self->title));
  st_widget_ensure_style (ST_WIDGET (self->icon));
}

static void
on_drag_begin (ShellDraggable      *draggable,
               guint                time,
               ShellWindowPreview *self)
{
  self->in_drag = TRUE;
  shell_window_preview_hide_overlay (self, FALSE);
  g_signal_emit (self, signals[DRAG_BEGIN], 0);
}

static void
on_drag_cancelled (ShellDraggable      *draggable,
                   guint                time,
                   ShellWindowPreview *self)
{
  g_signal_emit (self, signals[DRAG_CANCELLED], 0);
}

static void
on_drag_end (ShellDraggable      *draggable,
             guint                time,
             gboolean             snapback,
             ShellWindowPreview *self)
{
  self->in_drag = FALSE;

  restack (self);

  if (clutter_actor_has_pointer (CLUTTER_ACTOR (self)))
    shell_window_preview_show_overlay (self, TRUE);

  g_signal_emit (self, signals[DRAG_END], 0);
}

/* ------------------------------------------------------------------------ */
/* ShellDragTarget                                                          */
/* ------------------------------------------------------------------------ */

static ShellDragMotionResult
shell_window_preview_handle_drag_over (ShellDragTarget *target,
                                       GObject         *source,
                                       ClutterActor    *drag_actor,
                                       float            x,
                                       float            y,
                                       uint32_t         time)
{
  ShellWindowPreview *self = SHELL_WINDOW_PREVIEW (target);

  if (self->workspace == NULL)
    return SHELL_DRAG_MOTION_RESULT_CONTINUE;

  return shell_drag_target_handle_drag_over (self->workspace, source,
                                             drag_actor, x, y, time);
}

static gboolean
shell_window_preview_accept_drop (ShellDragTarget *target,
                                  GObject         *source,
                                  ClutterActor    *drag_actor,
                                  float            x,
                                  float            y,
                                  uint32_t         time)
{
  ShellWindowPreview *self = SHELL_WINDOW_PREVIEW (target);

  if (self->workspace == NULL)
    return FALSE;

  return shell_drag_target_accept_drop (self->workspace, source,
                                        drag_actor, x, y, time);
}

static void
shell_window_preview_drag_target_iface_init (ShellDragTargetInterface *iface)
{
  iface->handle_drag_over = shell_window_preview_handle_drag_over;
  iface->accept_drop = shell_window_preview_accept_drop;
}

/* ------------------------------------------------------------------------ */
/* ClutterActor                                                             */
/* ------------------------------------------------------------------------ */

static void
shell_window_preview_get_preferred_width (ClutterActor *actor,
                                          float         for_height,
                                          float        *min_width_p,
                                          float        *natural_width_p)
{
  ShellWindowPreview *self = SHELL_WINDOW_PREVIEW (actor);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  float min_width, nat_width;

  st_theme_node_adjust_for_height (theme_node, &for_height);

  clutter_actor_get_preferred_width (self->window_container, for_height,
                                     &min_width, &nat_width);

  st_theme_node_adjust_preferred_width (theme_node, &min_width, &nat_width);

  if (min_width_p)
    *min_width_p = min_width;

  if (natural_width_p)
    *natural_width_p = nat_width;
}

static void
shell_window_preview_get_preferred_height (ClutterActor *actor,
                                           float         for_width,
                                           float        *min_height_p,
                                           float        *natural_height_p)
{
  ShellWindowPreview *self = SHELL_WINDOW_PREVIEW (actor);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  float min_height, nat_height;

  st_theme_node_adjust_for_width (theme_node, &for_width);

  clutter_actor_get_preferred_height (self->window_container, for_width,
                                      &min_height, &nat_height);

  st_theme_node_adjust_preferred_height (theme_node, &min_height, &nat_height);

  if (min_height_p)
    *min_height_p = min_height;

  if (natural_height_p)
    *natural_height_p = nat_height;
}

static void
shell_window_preview_allocate (ClutterActor          *actor,
                               const ClutterActorBox *box)
{
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  ClutterActorBox content_box;
  float x, y, max_width, max_height;
  ClutterActorIter iter;
  ClutterActor *child;

  clutter_actor_set_allocation (actor, box);

  st_theme_node_get_content_box (theme_node, box, &content_box);

  clutter_actor_box_get_origin (&content_box, &x, &y);
  clutter_actor_box_get_size (&content_box, &max_width, &max_height);

  clutter_actor_iter_init (&iter, actor);
  while (clutter_actor_iter_next (&iter, &child))
    clutter_actor_allocate_available_size (child, x, y, max_width, max_height);
}

static gboolean
shell_window_preview_has_overlaps (ClutterActor *actor)
{
  ShellWindowPreview *self = SHELL_WINDOW_PREVIEW (actor);

  return has_attached_dialogs (self) ||
         (self->icon != NULL && clutter_actor_is_visible (self->icon)) ||
         (self->close_button != NULL && clutter_actor_is_visible (self->close_button));
}

static void
shell_window_preview_key_focus_in (ClutterActor *actor)
{
  ShellWindowPreview *self = SHELL_WINDOW_PREVIEW (actor);
  ClutterActorClass *parent_class =
    CLUTTER_ACTOR_CLASS (shell_window_preview_parent_class);

  if (parent_class->key_focus_in != NULL)
    parent_class->key_focus_in (actor);

  shell_window_preview_show_overlay (self, TRUE);
}

static void
shell_window_preview_key_focus_out (ClutterActor *actor)
{
  ShellWindowPreview *self = SHELL_WINDOW_PREVIEW (actor);
  ClutterActorClass *parent_class =
    CLUTTER_ACTOR_CLASS (shell_window_preview_parent_class);

  if (parent_class->key_focus_out != NULL)
    parent_class->key_focus_out (actor);

  if (clutter_stage_get_grab_actor (get_stage ()) != self->close_button)
    shell_window_preview_hide_overlay (self, TRUE);
}

static void
shell_window_preview_destroy (ClutterActor *actor)
{
  ShellWindowPreview *self = SHELL_WINDOW_PREVIEW (actor);

  self->destroyed = TRUE;

  g_clear_handle_id (&self->idle_hide_overlay_id, g_source_remove);

  if (self->in_drag)
    {
      g_signal_emit (self, signals[DRAG_END], 0);
      self->in_drag = FALSE;
    }

  CLUTTER_ACTOR_CLASS (shell_window_preview_parent_class)->destroy (actor);
}

/* ------------------------------------------------------------------------ */
/* GObject                                                                  */
/* ------------------------------------------------------------------------ */

static void
shell_window_preview_constructed (GObject *object)
{
  ShellWindowPreview *self = SHELL_WINDOW_PREVIEW (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellWindowTracker *tracker = shell_window_tracker_get_default ();
  g_autoptr (ShellApp) app = NULL;
  g_autofree char *caption = NULL;
  ShellDraggable *draggable;
  ClutterAction *action;
  ClutterActor *window_actor;
  MetaButtonLayout button_layout;
  int scale_factor = get_scale_factor ();
  float icon_bottom_overlap;
  int i;

  G_OBJECT_CLASS (shell_window_preview_parent_class)->constructed (object);

  g_return_if_fail (META_IS_WINDOW (self->meta_window));

  clutter_actor_set_reactive (actor, TRUE);
  st_widget_set_can_focus (ST_WIDGET (self), TRUE);
  clutter_actor_set_accessible_role (actor, ATK_ROLE_PUSH_BUTTON);
  clutter_actor_set_offscreen_redirect (actor,
                                        CLUTTER_OFFSCREEN_REDIRECT_AUTOMATIC_FOR_OPACITY);

  self->overlay_enabled = TRUE;

  if (self->window_container == NULL)
    {
      self->window_container = g_object_ref_sink (clutter_actor_new ());
      clutter_actor_set_pivot_point (self->window_container, 0.5, 0.5);
    }

  g_signal_connect_object (self->window_container, "notify::scale-x",
                           G_CALLBACK (adjust_overlay_offsets), self,
                           G_CONNECT_SWAPPED);

  self->container_layout = g_object_new (SHELL_TYPE_WINDOW_PREVIEW_LAYOUT, NULL);
  clutter_actor_set_layout_manager (self->window_container,
                                    CLUTTER_LAYOUT_MANAGER (self->container_layout));
  clutter_actor_add_child (actor, self->window_container);

  add_window (self, self->meta_window);

  self->cached_bounding_box =
    *shell_window_preview_layout_get_bounding_box (self->container_layout);

  g_signal_connect_object (self->container_layout, "notify::bounding-box",
                           G_CALLBACK (on_bounding_box_changed), self, 0);

  window_actor = CLUTTER_ACTOR (meta_window_get_compositor_private (self->meta_window));
  if (window_actor != NULL)
    {
      g_signal_connect_object (window_actor, "destroy",
                               G_CALLBACK (on_window_actor_destroyed), self, 0);
    }

  update_attached_dialogs (self);

  draggable = g_object_new (SHELL_TYPE_DRAGGABLE,
                            "actor", actor,
                            "restore-on-success", TRUE,
                            "drag-actor-max-size", SHELL_WINDOW_PREVIEW_DND_SIZE,
                            "drag-actor-opacity", SHELL_WINDOW_PREVIEW_DRAGGING_OPACITY,
                            NULL);
  g_signal_connect_object (draggable, "drag-begin",
                           G_CALLBACK (on_drag_begin), self, 0);
  g_signal_connect_object (draggable, "drag-cancelled",
                           G_CALLBACK (on_drag_cancelled), self, 0);
  g_signal_connect_object (draggable, "drag-end",
                           G_CALLBACK (on_drag_end), self, 0);

  action = clutter_click_gesture_new ();
  g_signal_connect_swapped (action, "recognize", G_CALLBACK (on_click), self);
  clutter_actor_add_action (actor, action);

  action = clutter_long_press_gesture_new ();
  g_signal_connect_swapped (action, "recognize", G_CALLBACK (on_long_press), self);
  clutter_actor_add_action (actor, action);

  clutter_gesture_can_not_cancel (CLUTTER_GESTURE (action),
                                  CLUTTER_GESTURE (shell_draggable_get_start_gesture (draggable)));

  /* The actor owns the draggable, we are done with our reference */
  g_object_unref (draggable);

  /* The application icon */
  app = shell_window_tracker_get_window_app (tracker, self->meta_window);
  if (app != NULL)
    self->icon = shell_app_create_icon_texture (app, ICON_SIZE);
  else
    self->icon = st_icon_new ();
  g_object_ref_sink (self->icon);
  st_widget_add_style_class_name (ST_WIDGET (self->icon), "window-icon");
  st_widget_add_style_class_name (ST_WIDGET (self->icon), "icon-dropshadow");
  clutter_actor_set_reactive (self->icon, TRUE);
  clutter_actor_set_pivot_point (self->icon, 0.5, 0.5);
  add_bind_constraint (self->icon, self->window_container,
                       CLUTTER_BIND_POSITION, 0);
  add_align_constraint (self->icon, self->window_container,
                        CLUTTER_ALIGN_X_AXIS, 0.5, -1, -1);
  add_align_constraint (self->icon, self->window_container,
                        CLUTTER_ALIGN_Y_AXIS, 1, -1, ICON_OVERLAP);

  /* The title */
  caption = get_caption (self);
  self->title = g_object_ref_sink (CLUTTER_ACTOR (st_label_new (caption)));
  clutter_actor_hide (self->title);
  st_widget_add_style_class_name (ST_WIDGET (self->title), "window-caption");
  clutter_actor_set_reactive (self->title, TRUE);
  clutter_text_set_single_line_mode (st_label_get_clutter_text (ST_LABEL (self->title)),
                                     TRUE);
  add_bind_constraint (self->title, self->window_container,
                       CLUTTER_BIND_X, 0);
  icon_bottom_overlap = ICON_SIZE * (1 - ICON_OVERLAP);
  add_bind_constraint (self->title, self->window_container,
                       CLUTTER_BIND_Y,
                       scale_factor * (icon_bottom_overlap + ICON_TITLE_SPACING));
  add_align_constraint (self->title, self->window_container,
                        CLUTTER_ALIGN_X_AXIS, 0.5, -1, -1);
  add_align_constraint (self->title, self->window_container,
                        CLUTTER_ALIGN_Y_AXIS, 1, -1, 0);
  clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (self->title)),
                              PANGO_ELLIPSIZE_END);
  st_widget_set_label_actor (ST_WIDGET (self), self->title);
  g_signal_connect_object (self->meta_window, "notify::title",
                           G_CALLBACK (on_window_title_changed), self,
                           G_CONNECT_SWAPPED);

  /* The close button, on the side of the close button of the decorations */
  meta_prefs_get_button_layout (&button_layout);
  self->close_button_side = ST_SIDE_RIGHT;
  for (i = 0; i < MAX_BUTTONS_PER_CORNER; i++)
    {
      if (button_layout.left_buttons[i] == META_BUTTON_FUNCTION_CLOSE)
        {
          self->close_button_side = ST_SIDE_LEFT;
          break;
        }
    }

  self->close_button = g_object_ref_sink (g_object_new (ST_TYPE_BUTTON,
                                                        "visible", FALSE,
                                                        "style-class", "window-close",
                                                        "icon-name", "preview-close-symbolic",
                                                        NULL));
  add_bind_constraint (self->close_button, self->window_container,
                       CLUTTER_BIND_POSITION, 0);
  add_align_constraint (self->close_button, self->window_container,
                        CLUTTER_ALIGN_X_AXIS,
                        self->close_button_side == ST_SIDE_LEFT ? 0 : 1,
                        0.5, -1);
  add_align_constraint (self->close_button, self->window_container,
                        CLUTTER_ALIGN_Y_AXIS, 0, -1, 0.5);
  g_signal_connect_swapped (self->close_button, "clicked",
                            G_CALLBACK (on_close_clicked), self);

  clutter_actor_add_child (actor, self->title);
  clutter_actor_add_child (actor, self->icon);
  clutter_actor_add_child (actor, self->close_button);

  if (self->overview_adjustment != NULL)
    {
      g_signal_connect_object (self->overview_adjustment, "notify::value",
                               G_CALLBACK (update_icon_scale), self,
                               G_CONNECT_SWAPPED);
    }
  update_icon_scale (self);

  g_signal_connect_object (self, "notify::realized",
                           G_CALLBACK (on_realized_changed), self,
                           G_CONNECT_SWAPPED);

  action = clutter_motion_controller_new ();
  g_signal_connect_swapped (action, "enter", G_CALLBACK (on_enter), self);
  g_signal_connect_swapped (action, "leave", G_CALLBACK (on_leave), self);
  clutter_actor_add_action (actor, action);
}

static void
shell_window_preview_get_property (GObject      *gobject,
                                   unsigned int  property_id,
                                   GValue       *value,
                                   GParamSpec   *pspec)
{
  ShellWindowPreview *self = SHELL_WINDOW_PREVIEW (gobject);

  switch (property_id)
    {
    case PROP_WINDOW_CONTAINER:
      g_value_set_object (value, self->window_container);
      break;

    case PROP_META_WINDOW:
      g_value_set_object (value, self->meta_window);
      break;

    case PROP_WORKSPACE:
      g_value_set_object (value, self->workspace);
      break;

    case PROP_OVERVIEW_ADJUSTMENT:
      g_value_set_object (value, self->overview_adjustment);
      break;

    case PROP_OVERLAY_ENABLED:
      g_value_set_boolean (value, self->overlay_enabled);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, property_id, pspec);
    }
}

static void
shell_window_preview_set_property (GObject      *gobject,
                                   unsigned int  property_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  ShellWindowPreview *self = SHELL_WINDOW_PREVIEW (gobject);

  switch (property_id)
    {
    case PROP_WINDOW_CONTAINER:
      g_set_object (&self->window_container, g_value_get_object (value));
      break;

    case PROP_META_WINDOW:
      g_set_object (&self->meta_window, g_value_get_object (value));
      break;

    case PROP_WORKSPACE:
      g_set_weak_pointer (&self->workspace, g_value_get_object (value));
      break;

    case PROP_OVERVIEW_ADJUSTMENT:
      g_set_object (&self->overview_adjustment, g_value_get_object (value));
      break;

    case PROP_OVERLAY_ENABLED:
      shell_window_preview_set_overlay_enabled (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, property_id, pspec);
    }
}

static void
shell_window_preview_dispose (GObject *gobject)
{
  ShellWindowPreview *self = SHELL_WINDOW_PREVIEW (gobject);

  g_clear_handle_id (&self->idle_hide_overlay_id, g_source_remove);

  g_clear_weak_pointer (&self->workspace);
  g_clear_weak_pointer (&self->stack_above);

  self->container_layout = NULL;
  g_clear_object (&self->icon);
  g_clear_object (&self->title);
  g_clear_object (&self->close_button);
  g_clear_object (&self->window_container);
  g_clear_object (&self->overview_adjustment);

  G_OBJECT_CLASS (shell_window_preview_parent_class)->dispose (gobject);

  /* Clear after the chain-up: ClutterActor's dispose emits ::destroy, and the
   * handlers of ShellWorkspace (preview_destroyed_cb ()) and the ::drag-end
   * emitted by destroy () read the window of the preview */
  g_clear_object (&self->meta_window);
}

static void
shell_window_preview_init (ShellWindowPreview *self)
{
  self->overlay_enabled = TRUE;
}

static void
shell_window_preview_class_init (ShellWindowPreviewClass *klass)
{
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  ClutterBindingPool *binding_pool;

  actor_class->get_preferred_width = shell_window_preview_get_preferred_width;
  actor_class->get_preferred_height = shell_window_preview_get_preferred_height;
  actor_class->allocate = shell_window_preview_allocate;
  actor_class->has_overlaps = shell_window_preview_has_overlaps;
  actor_class->key_focus_in = shell_window_preview_key_focus_in;
  actor_class->key_focus_out = shell_window_preview_key_focus_out;
  actor_class->destroy = shell_window_preview_destroy;

  gobject_class->constructed = shell_window_preview_constructed;
  gobject_class->dispose = shell_window_preview_dispose;
  gobject_class->get_property = shell_window_preview_get_property;
  gobject_class->set_property = shell_window_preview_set_property;

  /**
   * ShellWindowPreview:window-container:
   *
   * The actor that holds the clones of the window and its attached dialogs.
   * It is created by the preview when it is not given.
   */
  props[PROP_WINDOW_CONTAINER] =
    g_param_spec_object ("window-container", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWindowPreview:meta-window:
   *
   * The window that the preview shows (`metaWindow`).
   */
  props[PROP_META_WINDOW] =
    g_param_spec_object ("meta-window", NULL, NULL,
                         META_TYPE_WINDOW,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWindowPreview:workspace:
   *
   * The drag target that drags over the preview and drops on it are
   * forwarded to, held weakly. It is the workspace that the preview is in.
   */
  props[PROP_WORKSPACE] =
    g_param_spec_object ("workspace", NULL, NULL,
                         SHELL_TYPE_DRAG_TARGET,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWindowPreview:overview-adjustment:
   *
   * The adjustment of the state of the overview, which scales the icon of
   * the preview with the window picker.
   */
  props[PROP_OVERVIEW_ADJUSTMENT] =
    g_param_spec_object ("overview-adjustment", NULL, NULL,
                         ST_TYPE_ADJUSTMENT,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWindowPreview:overlay-enabled:
   *
   * Whether the title and the close button are shown when the pointer or the
   * key focus is on the preview.
   */
  props[PROP_OVERLAY_ENABLED] =
    g_param_spec_boolean ("overlay-enabled", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, N_PROPS, props);

  /**
   * ShellWindowPreview::drag-begin:
   * @preview: the #ShellWindowPreview
   *
   * Emitted when the preview starts to be dragged.
   */
  signals[DRAG_BEGIN] =
    g_signal_new ("drag-begin",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellWindowPreview::drag-cancelled:
   * @preview: the #ShellWindowPreview
   *
   * Emitted when the drag of the preview is cancelled.
   */
  signals[DRAG_CANCELLED] =
    g_signal_new ("drag-cancelled",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellWindowPreview::drag-end:
   * @preview: the #ShellWindowPreview
   *
   * Emitted when the drag of the preview is over, whatever its outcome, or
   * when the preview is destroyed during a drag.
   */
  signals[DRAG_END] =
    g_signal_new ("drag-end",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellWindowPreview::selected:
   * @preview: the #ShellWindowPreview
   * @time: the time of the event that selected the preview
   *
   * Emitted when the user activates the preview, by clicking on it, by
   * pressing Return or by closing its window when a dialog asks for a
   * confirmation.
   */
  signals[SELECTED] =
    g_signal_new ("selected",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_UINT);

  /**
   * ShellWindowPreview::show-chrome:
   * @preview: the #ShellWindowPreview
   *
   * Emitted when the title and the close button of the preview are shown.
   */
  signals[SHOW_CHROME] =
    g_signal_new ("show-chrome",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellWindowPreview::size-changed:
   * @preview: the #ShellWindowPreview
   *
   * Emitted when the bounding box of the window and its attached dialogs
   * changes.
   */
  signals[SIZE_CHANGED] =
    g_signal_new ("size-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  binding_pool = clutter_binding_pool_get_for_class (klass);
  clutter_binding_pool_install_action (binding_pool,
                                       "activate",
                                       CLUTTER_KEY_Return,
                                       0,
                                       G_CALLBACK (activate_binding_cb),
                                       NULL,
                                       NULL);
}

/**
 * shell_window_preview_new:
 * @window: the window to show
 * @workspace: (nullable): the drag target that drops on the preview are
 *   forwarded to
 * @overview_adjustment: (nullable): the adjustment of the state of the overview
 *
 * Creates the preview of @window, which must have a compositor actor.
 *
 * Returns: (transfer floating): a new #ShellWindowPreview
 */
ShellWindowPreview *
shell_window_preview_new (MetaWindow      *window,
                          ShellDragTarget *workspace,
                          StAdjustment    *overview_adjustment)
{
  g_return_val_if_fail (META_IS_WINDOW (window), NULL);

  return g_object_new (SHELL_TYPE_WINDOW_PREVIEW,
                       "meta-window", window,
                       "workspace", workspace,
                       "overview-adjustment", overview_adjustment,
                       NULL);
}
