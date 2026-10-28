/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * workspaces-view.c: The workspaces of the overview, on every monitor. Port
 * of js/ui/workspacesView.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `Main.overview`, `Main.wm` and `Main.layoutManager` are the objects the
 *    #ShellWorkspacesDisplay is created with; the views get them from it. The
 *    window pickers (`createWorkspace ()`) are connected to the overview here,
 *    where the JS module connected them to `Main.overview`.
 *  - The overview does not emit `windows-restacked` and `scroll-event` any
 *    more: the display listens to the `restacked` signal of the display and to
 *    the `scroll-event` of the overview group while the overview is shown.
 *    The scroll handler never stops the event, as the JS one never did (the
 *    value returned by the handler of a JS signal was ignored).
 *  - `WorkspacesViewBase._inDrag` was never read and is gone.
 *  - The classes are not constructed with properties: the new functions of the
 *    types (workspaces_view_new (), ...) create them and then set them up.
 */

#include "config.h"

#include <math.h>

#include "shell-global.h"
#include "ui/environment.h"
#include "ui/main.h"
#include "ui/swipe-tracker.h"
#include "ui/workspace-thumbnail.h"
#include "ui/workspace.h"
#include "ui/workspaces-view.h"

#define WORKSPACE_SWITCH_TIME 250

#define MUTTER_SCHEMA "org.gnome.mutter"

#define WORKSPACE_MIN_SPACING 24
#define WORKSPACE_MAX_SPACING 80

#define WORKSPACE_INACTIVE_SCALE 0.94

#define SECONDARY_WORKSPACE_SCALE 0.80

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

/* adjustment.ease (target, {mode, duration, onComplete}) */
static void
ease_adjustment (StAdjustment         *adjustment,
                 double                target,
                 int                   duration,
                 ClutterAnimationMode  mode,
                 ShellEaseCallback     callback,
                 gpointer              user_data,
                 GDestroyNotify        destroy)
{
  ShellEaseParams params = {
    .duration = duration,
    .delay = -1,
    .mode = mode,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
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

static MetaWorkspaceManager *
get_workspace_manager (void)
{
  return shell_global_get_workspace_manager (shell_global_get ());
}

static MetaDisplay *
get_display (void)
{
  return shell_global_get_display (shell_global_get ());
}

static gboolean
is_vertical_layout (void)
{
  return meta_workspace_manager_get_layout_rows (get_workspace_manager ()) == -1;
}

static void
box_set_origin_size (ClutterActorBox *box,
                     float            x,
                     float            y,
                     float            width,
                     float            height)
{
  clutter_actor_box_init_rect (box, x, y, width, height);
}

static void
get_preferred_natural_width (ClutterActor *actor,
                             float         for_height,
                             float        *natural)
{
  clutter_actor_get_preferred_width (actor, for_height, NULL, natural);
}

static void
get_preferred_natural_height (ClutterActor *actor,
                              float         for_width,
                              float        *natural)
{
  clutter_actor_get_preferred_height (actor, for_width, NULL, natural);
}

/* ------------------------------------------------------------------------ */
/* createWorkspace ()                                                        */
/* ------------------------------------------------------------------------ */

static void
workspace_overview_hide_requested_cb (ShellWorkspace *workspace,
                                      ShellOverview  *overview)
{
  shell_overview_hide_for_reason (overview, "window-activated");
}

static void
workspace_window_activated_cb (ShellWorkspace *workspace,
                               MetaWindow     *window,
                               guint           time,
                               ShellOverview  *overview)
{
  ShellPanel *panel = shell_main_get_panel (shell_main_get_default ());

  shell_overview_hide_for_reason (overview, "window-activated");
  if (panel != NULL)
    shell_panel_close_calendar (panel);
}

static void
workspace_window_drag_begin_cb (ShellWorkspace *workspace,
                                MetaWindow     *window,
                                ShellOverview  *overview)
{
  shell_overview_begin_window_drag (overview, window);
}

static void
workspace_window_drag_cancelled_cb (ShellWorkspace *workspace,
                                    MetaWindow     *window,
                                    ShellOverview  *overview)
{
  shell_overview_cancelled_window_drag (overview, window);
}

static void
workspace_window_drag_end_cb (ShellWorkspace *workspace,
                              MetaWindow     *window,
                              ShellOverview  *overview)
{
  shell_overview_end_window_drag (overview, window);
}

/* Creates the window picker of a workspace and connects it to the overview */
static ShellWorkspace *
create_workspace (MetaWorkspace       *meta_workspace,
                  int                  monitor_index,
                  StAdjustment        *overview_adjustment,
                  ShellWindowManager  *window_manager,
                  ShellOverview       *overview)
{
  ShellWorkspace *workspace = shell_workspace_new (meta_workspace, monitor_index,
                                                   overview_adjustment, window_manager);

  g_signal_connect_object (workspace, "overview-hide-requested",
                           G_CALLBACK (workspace_overview_hide_requested_cb), overview, 0);
  g_signal_connect_object (workspace, "window-activated",
                           G_CALLBACK (workspace_window_activated_cb), overview, 0);
  g_signal_connect_object (workspace, "window-drag-begin",
                           G_CALLBACK (workspace_window_drag_begin_cb), overview, 0);
  g_signal_connect_object (workspace, "window-drag-cancelled",
                           G_CALLBACK (workspace_window_drag_cancelled_cb), overview, 0);
  g_signal_connect_object (workspace, "window-drag-end",
                           G_CALLBACK (workspace_window_drag_end_cb), overview, 0);

  return workspace;
}

/* ------------------------------------------------------------------------ */
/* ShellWorkspacesViewBase (private, abstract)                               */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WORKSPACES_VIEW_BASE (shell_workspaces_view_base_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellWorkspacesViewBase, shell_workspaces_view_base,
                          SHELL, WORKSPACES_VIEW_BASE, StWidget)

struct _ShellWorkspacesViewBaseClass
{
  StWidgetClass parent_class;

  void (* update_workspace_mode)   (ShellWorkspacesViewBase *self);
  ClutterActor * (* get_active_workspace) (ShellWorkspacesViewBase *self);
  void (* prepare_to_leave_overview) (ShellWorkspacesViewBase *self);
  void (* sync_stacking)           (ShellWorkspacesViewBase *self,
                                    const guint32           *sequences,
                                    gsize                    n_sequences);
  void (* start_touch_gesture)     (ShellWorkspacesViewBase *self);
  void (* end_touch_gesture)       (ShellWorkspacesViewBase *self);
};

typedef struct
{
  int monitor_index;
  ShellOverview *overview;                  /* weak */
  ShellWindowManager *window_manager;       /* weak */
  ShellLayoutManager *layout_manager;       /* weak */
  ShellOverviewAdjustment *overview_adjustment; /* weak */
} ShellWorkspacesViewBasePrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (ShellWorkspacesViewBase, shell_workspaces_view_base,
                                     ST_TYPE_WIDGET)

static void
view_base_overview_adjustment_changed_cb (StAdjustment            *adjustment,
                                          GParamSpec              *pspec,
                                          ShellWorkspacesViewBase *self)
{
  ShellWorkspacesViewBaseClass *klass = SHELL_WORKSPACES_VIEW_BASE_GET_CLASS (self);

  if (klass->update_workspace_mode != NULL)
    klass->update_workspace_mode (self);
}

static void
view_base_setup (ShellWorkspacesViewBase *self,
                 int                      monitor_index,
                 ShellOverview           *overview,
                 ShellWindowManager      *window_manager,
                 ShellLayoutManager      *layout_manager,
                 ShellOverviewAdjustment *overview_adjustment)
{
  ShellWorkspacesViewBasePrivate *priv = shell_workspaces_view_base_get_instance_private (self);

  priv->monitor_index = monitor_index;
  g_set_weak_pointer (&priv->overview, overview);
  g_set_weak_pointer (&priv->window_manager, window_manager);
  g_set_weak_pointer (&priv->layout_manager, layout_manager);
  g_set_weak_pointer (&priv->overview_adjustment, overview_adjustment);

  g_signal_connect_object (overview_adjustment, "notify::value",
                           G_CALLBACK (view_base_overview_adjustment_changed_cb), self, 0);
}

static void
shell_workspaces_view_base_constructed (GObject *object)
{
  ShellWorkspacesViewBase *self = SHELL_WORKSPACES_VIEW_BASE (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);

  G_OBJECT_CLASS (shell_workspaces_view_base_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (self), "workspaces-view");
  clutter_actor_set_x_expand (actor, TRUE);
  clutter_actor_set_y_expand (actor, TRUE);
  clutter_actor_set_reactive (actor, TRUE);

  st_focus_manager_add_group (shell_global_get_focus_manager (shell_global_get ()),
                              ST_WIDGET (self));
}

static void
shell_workspaces_view_base_dispose (GObject *object)
{
  ShellWorkspacesViewBase *self = SHELL_WORKSPACES_VIEW_BASE (object);
  ShellWorkspacesViewBasePrivate *priv = shell_workspaces_view_base_get_instance_private (self);

  g_clear_weak_pointer (&priv->overview);
  g_clear_weak_pointer (&priv->window_manager);
  g_clear_weak_pointer (&priv->layout_manager);
  g_clear_weak_pointer (&priv->overview_adjustment);

  G_OBJECT_CLASS (shell_workspaces_view_base_parent_class)->dispose (object);
}

static void
view_base_allocate (ClutterActor          *actor,
                    const ClutterActorBox *box)
{
  ClutterActorIter iter;
  ClutterActor *child;

  clutter_actor_set_allocation (actor, box);

  clutter_actor_iter_init (&iter, actor);
  while (clutter_actor_iter_next (&iter, &child))
    clutter_actor_allocate_available_size (child, 0, 0,
                                           clutter_actor_box_get_width (box),
                                           clutter_actor_box_get_height (box));
}

static void
view_base_get_preferred_width (ClutterActor *actor,
                               float         for_height,
                               float        *min_width_p,
                               float        *natural_width_p)
{
  if (min_width_p != NULL)
    *min_width_p = 0;
  if (natural_width_p != NULL)
    *natural_width_p = 0;
}

static void
view_base_get_preferred_height (ClutterActor *actor,
                                float         for_width,
                                float        *min_height_p,
                                float        *natural_height_p)
{
  if (min_height_p != NULL)
    *min_height_p = 0;
  if (natural_height_p != NULL)
    *natural_height_p = 0;
}

static void
shell_workspaces_view_base_class_init (ShellWorkspacesViewBaseClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_workspaces_view_base_constructed;
  object_class->dispose = shell_workspaces_view_base_dispose;

  actor_class->allocate = view_base_allocate;
  actor_class->get_preferred_width = view_base_get_preferred_width;
  actor_class->get_preferred_height = view_base_get_preferred_height;
}

static void
shell_workspaces_view_base_init (ShellWorkspacesViewBase *self)
{
}

static ClutterActor *
view_base_get_active_workspace (ShellWorkspacesViewBase *self)
{
  ShellWorkspacesViewBaseClass *klass = SHELL_WORKSPACES_VIEW_BASE_GET_CLASS (self);

  return klass->get_active_workspace != NULL ? klass->get_active_workspace (self) : NULL;
}

/* ------------------------------------------------------------------------ */
/* ShellWorkspacesView (private)                                             */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WORKSPACES_VIEW (shell_workspaces_view_get_type ())
G_DECLARE_FINAL_TYPE (ShellWorkspacesView, shell_workspaces_view,
                      SHELL, WORKSPACES_VIEW, ShellWorkspacesViewBase)

struct _ShellWorkspacesView
{
  ShellWorkspacesViewBase parent;

  ShellControlsManager *controls;   /* weak */
  StAdjustment *fit_mode_adjustment; /* weak */
  StAdjustment *scroll_adjustment;  /* weak */

  gboolean animating;       /* tweening */
  gboolean gesture_active;  /* touch(pad) gestures */

  GPtrArray *workspaces;    /* of ShellWorkspace, children of the view */
};

G_DEFINE_FINAL_TYPE (ShellWorkspacesView, shell_workspaces_view,
                     SHELL_TYPE_WORKSPACES_VIEW_BASE)

static ShellWorkspacesViewBasePrivate *
view_priv (ShellWorkspacesView *self)
{
  return shell_workspaces_view_base_get_instance_private (SHELL_WORKSPACES_VIEW_BASE (self));
}

static ShellWorkspace *
view_first_workspace (ShellWorkspacesView *self)
{
  return g_ptr_array_index (self->workspaces, 0);
}

static void
get_first_fit_all_workspace_box (ShellWorkspacesView  *self,
                                 const ClutterActorBox *box,
                                 float                  spacing,
                                 gboolean               vertical,
                                 ClutterActorBox       *fit_all_box)
{
  int n_workspaces = meta_workspace_manager_get_n_workspaces (get_workspace_manager ());
  float width = clutter_actor_box_get_width (box);
  float height = clutter_actor_box_get_height (box);
  ClutterActor *workspace = CLUTTER_ACTOR (view_first_workspace (self));
  float x1, y1;

  clutter_actor_box_get_origin (box, &x1, &y1);

  /* Spacing here is not only the space between workspaces, but also the
   * space before the first workspace, and after the last one. This prevents
   * workspaces from touching the edges of the allocation box. */
  if (vertical)
    {
      float available_height = height - spacing * (n_workspaces + 1);
      float workspace_height = available_height / n_workspaces;
      float workspace_width;

      get_preferred_natural_width (workspace, workspace_height, &workspace_width);

      y1 = spacing;
      if (workspace_width > width)
        {
          get_preferred_natural_height (workspace, width, &workspace_height);
          y1 += MAX ((available_height - workspace_height * n_workspaces) / 2, 0);
        }

      box_set_origin_size (fit_all_box, x1, y1, width, workspace_height);
    }
  else
    {
      float available_width = width - spacing * (n_workspaces + 1);
      float workspace_width = available_width / n_workspaces;
      float workspace_height;

      get_preferred_natural_height (workspace, workspace_width, &workspace_height);

      x1 = spacing;
      if (workspace_height > height)
        {
          get_preferred_natural_width (workspace, height, &workspace_width);
          x1 += MAX ((available_width - workspace_width * n_workspaces) / 2, 0);
        }

      box_set_origin_size (fit_all_box, x1, y1, workspace_width, height);
    }
}

static void
get_first_fit_single_workspace_box (ShellWorkspacesView   *self,
                                    const ClutterActorBox *box,
                                    float                  spacing,
                                    gboolean               vertical,
                                    ClutterActorBox       *fit_single_box)
{
  float width = clutter_actor_box_get_width (box);
  float height = clutter_actor_box_get_height (box);
  ClutterActor *workspace = CLUTTER_ACTOR (view_first_workspace (self));
  gboolean rtl = clutter_actor_get_text_direction (CLUTTER_ACTOR (self)) == CLUTTER_TEXT_DIRECTION_RTL;
  double value = st_adjustment_get_value (self->scroll_adjustment);
  double upper = st_adjustment_get_upper (self->scroll_adjustment);
  double current_workspace = vertical || !rtl ? value : upper - value - 1;
  float x1, y1;

  /* Single fit mode implies centered too */
  clutter_actor_box_get_origin (box, &x1, &y1);
  if (vertical)
    {
      float workspace_height;

      get_preferred_natural_height (workspace, width, &workspace_height);
      y1 += (height - workspace_height) / 2;
      y1 -= current_workspace * (workspace_height + spacing);
      box_set_origin_size (fit_single_box, x1, y1, width, workspace_height);
    }
  else
    {
      float workspace_width;

      get_preferred_natural_width (workspace, height, &workspace_width);
      x1 += (width - workspace_width) / 2;
      x1 -= current_workspace * (workspace_width + spacing);
      box_set_origin_size (fit_single_box, x1, y1, workspace_width, height);
    }
}

static float
get_spacing (ShellWorkspacesView   *self,
             const ClutterActorBox *box,
             ShellFitMode           fit_mode,
             gboolean               vertical)
{
  float width = clutter_actor_box_get_width (box);
  float height = clutter_actor_box_get_height (box);
  ClutterActor *workspace = CLUTTER_ACTOR (view_first_workspace (self));
  StThemeContext *context = st_theme_context_get_for_stage (shell_global_get_stage (shell_global_get ()));
  float available_space, workspace_size;
  double spacing;
  int scale_factor;

  if (vertical)
    {
      get_preferred_natural_height (workspace, width, &workspace_size);
      available_space = (height - workspace_size) / 2;
    }
  else
    {
      get_preferred_natural_width (workspace, height, &workspace_size);
      available_space = (width - workspace_size) / 2;
    }

  spacing = (available_space - workspace_size * 0.4) * (1 - fit_mode);
  scale_factor = st_theme_context_get_scale_factor (context);

  return CLAMP (spacing, WORKSPACE_MIN_SPACING * scale_factor,
                WORKSPACE_MAX_SPACING * scale_factor);
}

static double
get_workspace_mode_for_overview_state (double state)
{
  switch ((int) state)
    {
    case SHELL_CONTROLS_STATE_HIDDEN:
      return 0;
    case SHELL_CONTROLS_STATE_WINDOW_PICKER:
      return 1;
    case SHELL_CONTROLS_STATE_APP_GRID:
      return 0;
    default:
      return 0;
    }
}

static double
get_fit_mode_for_state (double state)
{
  switch ((int) state)
    {
    case SHELL_CONTROLS_STATE_HIDDEN:
    case SHELL_CONTROLS_STATE_WINDOW_PICKER:
      return SHELL_FIT_MODE_SINGLE;
    case SHELL_CONTROLS_STATE_APP_GRID:
      return SHELL_FIT_MODE_ALL;
    default:
      return SHELL_FIT_MODE_SINGLE;
    }
}

static void
view_update_workspaces_state (ShellWorkspacesView *self)
{
  ShellWorkspacesViewBasePrivate *priv = view_priv (self);
  ShellStateTransitionParams params;
  double fit_mode, workspace_mode;
  guint i;

  if (self->fit_mode_adjustment == NULL || self->scroll_adjustment == NULL ||
      priv->overview_adjustment == NULL)
    return;

  fit_mode = st_adjustment_get_value (self->fit_mode_adjustment);
  shell_overview_adjustment_get_state_transition_params (priv->overview_adjustment, &params);

  workspace_mode = (1 - fit_mode) *
    lerp (get_workspace_mode_for_overview_state (params.initial_state),
          get_workspace_mode_for_overview_state (params.final_state),
          params.progress);

  /* Fade and scale inactive workspaces */
  for (i = 0; i < self->workspaces->len; i++)
    {
      ShellWorkspace *workspace = g_ptr_array_index (self->workspaces, i);
      double distance = fabs (st_adjustment_get_value (self->scroll_adjustment) - i);
      double scale_progress = 1 - CLAMP (distance, 0, 1);
      double scale = lerp (WORKSPACE_INACTIVE_SCALE, 1, scale_progress);

      st_adjustment_set_value (shell_workspace_get_state_adjustment (workspace), workspace_mode);
      clutter_actor_set_scale (CLUTTER_ACTOR (workspace), scale, scale);
    }
}

static void
view_get_initial_boxes (ShellWorkspacesView *self,
                        const ClutterActorBox *box,
                        ClutterActorBox     *fit_single_box,
                        ClutterActorBox     *fit_all_box)
{
  ShellWorkspacesViewBasePrivate *priv = view_priv (self);
  ShellStateTransitionParams params;
  gboolean is_primary;

  box_set_origin_size (fit_single_box, 0, 0,
                       clutter_actor_box_get_width (box),
                       clutter_actor_box_get_height (box));
  *fit_all_box = *fit_single_box;

  shell_overview_adjustment_get_state_transition_params (priv->overview_adjustment, &params);

  is_primary = priv->layout_manager != NULL &&
               shell_layout_manager_get_primary_index (priv->layout_manager) == priv->monitor_index;

  if (is_primary && params.transitioning && self->controls != NULL)
    {
      double initial_fit_mode = get_fit_mode_for_state (params.initial_state);
      double final_fit_mode = get_fit_mode_for_state (params.final_state);

      /* Only use the relative boxes when the overview is in a state
       * transition, and the corresponding fit modes are different. */
      if (initial_fit_mode != final_fit_mode)
        {
          ClutterActorBox initial_box, final_box, parent_box;
          ClutterActor *parent = clutter_actor_get_parent (CLUTTER_ACTOR (self));
          float parent_offset_x = 0, parent_offset_y = 0;

          if (shell_controls_manager_get_workspaces_box_for_state (self->controls,
                                                                   (int) params.initial_state,
                                                                   &initial_box) &&
              shell_controls_manager_get_workspaces_box_for_state (self->controls,
                                                                   (int) params.final_state,
                                                                   &final_box))
            {
              /* Boxes are relative to ControlsManager, transform them;
               * apply_relative_transform_to_point would be more correct,
               * but also more expensive */
              if (parent != NULL)
                {
                  clutter_actor_get_allocation_box (parent, &parent_box);
                  clutter_actor_box_get_origin (&parent_box, &parent_offset_x, &parent_offset_y);
                }

              clutter_actor_box_set_origin (&initial_box,
                                            initial_box.x1 - parent_offset_x,
                                            initial_box.y1 - parent_offset_y);
              clutter_actor_box_set_origin (&final_box,
                                            final_box.x1 - parent_offset_x,
                                            final_box.y1 - parent_offset_y);

              if (initial_fit_mode == SHELL_FIT_MODE_SINGLE)
                {
                  *fit_single_box = initial_box;
                  *fit_all_box = final_box;
                }
              else
                {
                  *fit_all_box = initial_box;
                  *fit_single_box = final_box;
                }
            }
        }
    }
}

static void
view_update_visibility (ShellWorkspacesView *self)
{
  int active = meta_workspace_manager_get_active_workspace_index (get_workspace_manager ());
  gboolean single_fit_mode;
  guint i;

  if (self->fit_mode_adjustment == NULL)
    return;

  single_fit_mode = st_adjustment_get_value (self->fit_mode_adjustment) == SHELL_FIT_MODE_SINGLE;

  for (i = 0; i < self->workspaces->len; i++)
    {
      ClutterActor *workspace = g_ptr_array_index (self->workspaces, i);

      if (self->animating || self->gesture_active || !single_fit_mode)
        clutter_actor_show (workspace);
      else
        set_actor_visible (workspace, ABS ((int) i - active) <= 1);
    }
}

static ShellWorkspace *
view_get_active_workspace_internal (ShellWorkspacesView *self)
{
  int active = meta_workspace_manager_get_active_workspace_index (get_workspace_manager ());

  if (active < 0 || (guint) active >= self->workspaces->len)
    return NULL;

  return g_ptr_array_index (self->workspaces, active);
}

static void
view_raise_active_workspace (ShellWorkspacesView *self)
{
  ShellWorkspace *active_workspace = view_get_active_workspace_internal (self);

  if (active_workspace != NULL)
    clutter_actor_set_child_above_sibling (CLUTTER_ACTOR (self),
                                           CLUTTER_ACTOR (active_workspace), NULL);
}

static void
view_scroll_done_cb (gboolean finished,
                     gpointer user_data)
{
  g_autoptr (ShellWorkspacesView) self = g_weak_ref_get (user_data);

  if (self == NULL || !finished)
    return;

  self->animating = FALSE;
  view_update_visibility (self);
}

static void
view_scroll_to_active (ShellWorkspacesView *self)
{
  int active = meta_workspace_manager_get_active_workspace_index (get_workspace_manager ());

  self->animating = TRUE;
  view_update_visibility (self);

  if (self->scroll_adjustment == NULL)
    return;

  st_adjustment_remove_transition (self->scroll_adjustment, "value");
  ease_adjustment (self->scroll_adjustment, active, WORKSPACE_SWITCH_TIME,
                   CLUTTER_EASE_OUT_CUBIC,
                   view_scroll_done_cb, weak_ref_new (self), weak_ref_free);
}

static void
view_update_workspaces (ShellWorkspacesView *self)
{
  ShellWorkspacesViewBasePrivate *priv = view_priv (self);
  MetaWorkspaceManager *workspace_manager = get_workspace_manager ();
  int new_num_workspaces = meta_workspace_manager_get_n_workspaces (workspace_manager);
  int j;

  for (j = 0; j < new_num_workspaces; j++)
    {
      MetaWorkspace *meta_workspace = meta_workspace_manager_get_workspace_by_index (workspace_manager, j);

      if ((guint) j >= self->workspaces->len) /* added */
        {
          ShellWorkspace *workspace =
            create_workspace (meta_workspace, priv->monitor_index,
                              ST_ADJUSTMENT (priv->overview_adjustment),
                              priv->window_manager, priv->overview);

          clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (workspace));
          g_ptr_array_add (self->workspaces, workspace);
        }
      else
        {
          ShellWorkspace *workspace = g_ptr_array_index (self->workspaces, j);

          if (shell_workspace_get_meta_workspace (workspace) != meta_workspace) /* removed */
            {
              g_ptr_array_remove_index (self->workspaces, j);
              clutter_actor_destroy (CLUTTER_ACTOR (workspace));
            } /* else kept */
        }
    }

  for (j = (int) self->workspaces->len - 1; j >= new_num_workspaces; j--)
    {
      ShellWorkspace *workspace = g_ptr_array_index (self->workspaces, j);

      g_ptr_array_remove_index (self->workspaces, j);
      clutter_actor_destroy (CLUTTER_ACTOR (workspace));
    }

  view_update_workspaces_state (self);
  view_update_visibility (self);
  view_raise_active_workspace (self);
}

static void
view_n_workspaces_changed_cb (MetaWorkspaceManager *workspace_manager,
                              GParamSpec           *pspec,
                              ShellWorkspacesView  *self)
{
  view_update_workspaces (self);
}

static int
compare_workspaces_by_index (gconstpointer a,
                             gconstpointer b)
{
  ShellWorkspace *workspace_a = *(ShellWorkspace **) a;
  ShellWorkspace *workspace_b = *(ShellWorkspace **) b;

  return meta_workspace_index (shell_workspace_get_meta_workspace (workspace_a)) -
         meta_workspace_index (shell_workspace_get_meta_workspace (workspace_b));
}

static void
view_workspaces_reordered_cb (MetaWorkspaceManager *workspace_manager,
                              ShellWorkspacesView  *self)
{
  guint i;

  g_ptr_array_sort (self->workspaces, compare_workspaces_by_index);
  for (i = 0; i < self->workspaces->len; i++)
    clutter_actor_set_child_at_index (CLUTTER_ACTOR (self),
                                      g_ptr_array_index (self->workspaces, i), i);
}

static void
view_switch_workspace_cb (ShellWM             *shell_wm,
                          int                  from,
                          int                  to,
                          int                  direction,
                          ShellWorkspacesView *self)
{
  view_scroll_to_active (self);
  view_raise_active_workspace (self);
}

static void
view_fit_mode_changed_cb (StAdjustment        *adjustment,
                          GParamSpec          *pspec,
                          ShellWorkspacesView *self)
{
  view_update_visibility (self);
  view_update_workspaces_state (self);
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

/* Syncs the workspaces' positions to the value of the scroll adjustment and
 * changes the active workspace if appropriate */
static void
view_scroll_adjustment_changed_cb (StAdjustment        *adjustment,
                                   GParamSpec          *pspec,
                                   ShellWorkspacesView *self)
{
  MetaWorkspaceManager *workspace_manager = get_workspace_manager ();
  gboolean allow_switch;
  int active, current;

  if (!clutter_actor_has_allocation (CLUTTER_ACTOR (self)))
    return;

  allow_switch = st_adjustment_get_transition (adjustment, "value") == NULL &&
                 !self->gesture_active;

  active = meta_workspace_manager_get_active_workspace_index (workspace_manager);
  current = (int) js_round (st_adjustment_get_value (adjustment));

  if (allow_switch && active != current)
    {
      MetaWorkspace *meta_workspace;

      if (current < 0 || (guint) current >= self->workspaces->len)
        {
          /* The current workspace was destroyed. This could happen
           * when you are on the last empty workspace, and consolidate
           * windows using the thumbnail bar.
           * In that case, the intended behavior is to stay on the empty
           * workspace, which is the last one, so pick it. */
          current = (int) self->workspaces->len - 1;
        }

      if (current >= 0)
        {
          meta_workspace =
            shell_workspace_get_meta_workspace (g_ptr_array_index (self->workspaces, current));
          meta_workspace_activate (meta_workspace, shell_global_get_current_time (shell_global_get ()));
        }
    }

  view_update_workspaces_state (self);
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

static void
workspaces_view_update_workspace_mode (ShellWorkspacesViewBase *base)
{
  view_update_workspaces_state (SHELL_WORKSPACES_VIEW (base));
}

static void
workspaces_view_allocate (ClutterActor          *actor,
                          const ClutterActorBox *box)
{
  ShellWorkspacesView *self = SHELL_WORKSPACES_VIEW (actor);
  gboolean vertical, rtl;
  double fit_mode;
  ClutterActorBox fit_single_box, fit_all_box;
  float fit_single_spacing, fit_all_spacing;
  float fit_single_x1, fit_single_y1, fit_single_width, fit_single_height;
  float fit_all_x1, fit_all_y1, fit_all_width, fit_all_height;
  ClutterActorBox initial_single, initial_all;
  guint i;

  clutter_actor_set_allocation (actor, box);

  if (self->workspaces->len == 0 || self->fit_mode_adjustment == NULL)
    return;

  vertical = is_vertical_layout ();
  rtl = clutter_actor_get_text_direction (actor) == CLUTTER_TEXT_DIRECTION_RTL;

  fit_mode = st_adjustment_get_value (self->fit_mode_adjustment);

  view_get_initial_boxes (self, box, &initial_single, &initial_all);

  fit_single_spacing = get_spacing (self, &initial_single, SHELL_FIT_MODE_SINGLE, vertical);
  get_first_fit_single_workspace_box (self, &initial_single, fit_single_spacing, vertical,
                                      &fit_single_box);

  fit_all_spacing = get_spacing (self, &initial_all, SHELL_FIT_MODE_ALL, vertical);
  get_first_fit_all_workspace_box (self, &initial_all, fit_all_spacing, vertical,
                                   &fit_all_box);

  clutter_actor_box_get_origin (&fit_single_box, &fit_single_x1, &fit_single_y1);
  clutter_actor_box_get_size (&fit_single_box, &fit_single_width, &fit_single_height);
  clutter_actor_box_get_origin (&fit_all_box, &fit_all_x1, &fit_all_y1);
  clutter_actor_box_get_size (&fit_all_box, &fit_all_width, &fit_all_height);

  /* Account for RTL locales by reversing the list */
  for (i = 0; i < self->workspaces->len; i++)
    {
      ClutterActor *child = g_ptr_array_index (self->workspaces,
                                               rtl ? self->workspaces->len - 1 - i : i);
      ClutterActorBox child_box;

      if (fit_mode == SHELL_FIT_MODE_SINGLE)
        child_box = fit_single_box;
      else if (fit_mode == SHELL_FIT_MODE_ALL)
        child_box = fit_all_box;
      else
        clutter_actor_box_interpolate (&fit_single_box, &fit_all_box, fit_mode, &child_box);

      clutter_actor_allocate_align_fill (child, &child_box, 0.5, 0.5, FALSE, FALSE);

      if (vertical)
        {
          clutter_actor_box_set_origin (&fit_single_box, fit_single_x1,
                                        fit_single_box.y1 + fit_single_height + fit_single_spacing);
          clutter_actor_box_set_origin (&fit_all_box, fit_all_x1,
                                        fit_all_box.y1 + fit_all_height + fit_all_spacing);
        }
      else
        {
          clutter_actor_box_set_origin (&fit_single_box,
                                        fit_single_box.x1 + fit_single_width + fit_single_spacing,
                                        fit_single_y1);
          clutter_actor_box_set_origin (&fit_all_box,
                                        fit_all_box.x1 + fit_all_width + fit_all_spacing,
                                        fit_all_y1);
        }
    }
}

static ClutterActor *
workspaces_view_get_active_workspace (ShellWorkspacesViewBase *base)
{
  return CLUTTER_ACTOR (view_get_active_workspace_internal (SHELL_WORKSPACES_VIEW (base)));
}

static void
workspaces_view_prepare_to_leave_overview (ShellWorkspacesViewBase *base)
{
  ShellWorkspacesView *self = SHELL_WORKSPACES_VIEW (base);
  guint i;

  for (i = 0; i < self->workspaces->len; i++)
    shell_workspace_prepare_to_leave_overview (g_ptr_array_index (self->workspaces, i));
}

static void
workspaces_view_sync_stacking (ShellWorkspacesViewBase *base,
                               const guint32           *sequences,
                               gsize                    n_sequences)
{
  ShellWorkspacesView *self = SHELL_WORKSPACES_VIEW (base);
  guint i;

  for (i = 0; i < self->workspaces->len; i++)
    shell_workspace_sync_stacking (g_ptr_array_index (self->workspaces, i),
                                   sequences, n_sequences);
}

static void
workspaces_view_start_touch_gesture (ShellWorkspacesViewBase *base)
{
  ShellWorkspacesView *self = SHELL_WORKSPACES_VIEW (base);

  self->gesture_active = TRUE;
  view_update_visibility (self);
}

static void
workspaces_view_end_touch_gesture (ShellWorkspacesViewBase *base)
{
  ShellWorkspacesView *self = SHELL_WORKSPACES_VIEW (base);

  self->gesture_active = FALSE;

  /* Make sure title captions etc are shown as necessary */
  view_scroll_to_active (self);
  view_update_visibility (self);
}

static void
shell_workspaces_view_dispose (GObject *object)
{
  ShellWorkspacesView *self = SHELL_WORKSPACES_VIEW (object);

  g_clear_weak_pointer (&self->controls);
  g_clear_weak_pointer (&self->fit_mode_adjustment);
  g_clear_weak_pointer (&self->scroll_adjustment);

  /* The workspaces are children, destroyed with the view */
  if (self->workspaces != NULL)
    g_ptr_array_set_size (self->workspaces, 0);

  G_OBJECT_CLASS (shell_workspaces_view_parent_class)->dispose (object);
}

static void
shell_workspaces_view_finalize (GObject *object)
{
  ShellWorkspacesView *self = SHELL_WORKSPACES_VIEW (object);

  g_clear_pointer (&self->workspaces, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_workspaces_view_parent_class)->finalize (object);
}

static void
shell_workspaces_view_class_init (ShellWorkspacesViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  ShellWorkspacesViewBaseClass *base_class = SHELL_WORKSPACES_VIEW_BASE_CLASS (klass);

  object_class->dispose = shell_workspaces_view_dispose;
  object_class->finalize = shell_workspaces_view_finalize;

  actor_class->allocate = workspaces_view_allocate;

  base_class->update_workspace_mode = workspaces_view_update_workspace_mode;
  base_class->get_active_workspace = workspaces_view_get_active_workspace;
  base_class->prepare_to_leave_overview = workspaces_view_prepare_to_leave_overview;
  base_class->sync_stacking = workspaces_view_sync_stacking;
  base_class->start_touch_gesture = workspaces_view_start_touch_gesture;
  base_class->end_touch_gesture = workspaces_view_end_touch_gesture;
}

static void
shell_workspaces_view_init (ShellWorkspacesView *self)
{
  self->workspaces = g_ptr_array_new ();
}

static ClutterActor *
workspaces_view_new (int                      monitor_index,
                     ShellOverview           *overview,
                     ShellWindowManager      *window_manager,
                     ShellLayoutManager      *layout_manager,
                     ShellControlsManager    *controls,
                     StAdjustment            *scroll_adjustment,
                     StAdjustment            *fit_mode_adjustment,
                     ShellOverviewAdjustment *overview_adjustment)
{
  ShellWorkspacesView *self = g_object_new (SHELL_TYPE_WORKSPACES_VIEW, NULL);
  MetaWorkspaceManager *workspace_manager = get_workspace_manager ();
  ShellWM *shell_wm = shell_global_get_window_manager (shell_global_get ());

  view_base_setup (SHELL_WORKSPACES_VIEW_BASE (self), monitor_index, overview,
                   window_manager, layout_manager, overview_adjustment);

  g_set_weak_pointer (&self->controls, controls);
  g_set_weak_pointer (&self->fit_mode_adjustment, fit_mode_adjustment);
  g_set_weak_pointer (&self->scroll_adjustment, scroll_adjustment);

  g_signal_connect_object (fit_mode_adjustment, "notify::value",
                           G_CALLBACK (view_fit_mode_changed_cb), self, 0);
  g_signal_connect_object (scroll_adjustment, "notify::value",
                           G_CALLBACK (view_scroll_adjustment_changed_cb), self, 0);

  view_update_workspaces (self);
  g_signal_connect_object (workspace_manager, "notify::n-workspaces",
                           G_CALLBACK (view_n_workspaces_changed_cb), self, 0);
  g_signal_connect_object (workspace_manager, "workspaces-reordered",
                           G_CALLBACK (view_workspaces_reordered_cb), self, 0);

  g_signal_connect_object (shell_wm, "switch-workspace",
                           G_CALLBACK (view_switch_workspace_cb), self, 0);

  return CLUTTER_ACTOR (self);
}

/* ------------------------------------------------------------------------ */
/* ShellExtraWorkspaceView (private)                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_EXTRA_WORKSPACE_VIEW (shell_extra_workspace_view_get_type ())
G_DECLARE_FINAL_TYPE (ShellExtraWorkspaceView, shell_extra_workspace_view,
                      SHELL, EXTRA_WORKSPACE_VIEW, ShellWorkspacesViewBase)

struct _ShellExtraWorkspaceView
{
  ShellWorkspacesViewBase parent;

  ShellWorkspace *workspace; /* a child */
};

G_DEFINE_FINAL_TYPE (ShellExtraWorkspaceView, shell_extra_workspace_view,
                     SHELL_TYPE_WORKSPACES_VIEW_BASE)

static void
extra_view_update_workspace_mode (ShellWorkspacesViewBase *base)
{
  ShellExtraWorkspaceView *self = SHELL_EXTRA_WORKSPACE_VIEW (base);
  ShellWorkspacesViewBasePrivate *priv = shell_workspaces_view_base_get_instance_private (base);
  double overview_state, progress;

  if (self->workspace == NULL || priv->overview_adjustment == NULL)
    return;

  overview_state = st_adjustment_get_value (ST_ADJUSTMENT (priv->overview_adjustment));
  progress = CLAMP (overview_state, SHELL_CONTROLS_STATE_HIDDEN,
                    SHELL_CONTROLS_STATE_WINDOW_PICKER);

  st_adjustment_set_value (shell_workspace_get_state_adjustment (self->workspace), progress);
}

static void
extra_view_allocate (ClutterActor          *actor,
                     const ClutterActorBox *box)
{
  ShellExtraWorkspaceView *self = SHELL_EXTRA_WORKSPACE_VIEW (actor);
  float width = clutter_actor_box_get_width (box);
  float height = clutter_actor_box_get_height (box);
  ClutterActorBox child_box;
  float child_width;

  clutter_actor_set_allocation (actor, box);

  if (self->workspace == NULL)
    return;

  get_preferred_natural_width (CLUTTER_ACTOR (self->workspace), height, &child_width);

  box_set_origin_size (&child_box, js_round ((width - child_width) / 2), 0, child_width, height);
  clutter_actor_allocate (CLUTTER_ACTOR (self->workspace), &child_box);
}

static ClutterActor *
extra_view_get_active_workspace (ShellWorkspacesViewBase *base)
{
  return CLUTTER_ACTOR (SHELL_EXTRA_WORKSPACE_VIEW (base)->workspace);
}

static void
extra_view_prepare_to_leave_overview (ShellWorkspacesViewBase *base)
{
  ShellExtraWorkspaceView *self = SHELL_EXTRA_WORKSPACE_VIEW (base);

  if (self->workspace != NULL)
    shell_workspace_prepare_to_leave_overview (self->workspace);
}

static void
extra_view_sync_stacking (ShellWorkspacesViewBase *base,
                          const guint32           *sequences,
                          gsize                    n_sequences)
{
  ShellExtraWorkspaceView *self = SHELL_EXTRA_WORKSPACE_VIEW (base);

  if (self->workspace != NULL)
    shell_workspace_sync_stacking (self->workspace, sequences, n_sequences);
}

static void
shell_extra_workspace_view_dispose (GObject *object)
{
  ShellExtraWorkspaceView *self = SHELL_EXTRA_WORKSPACE_VIEW (object);

  self->workspace = NULL;

  G_OBJECT_CLASS (shell_extra_workspace_view_parent_class)->dispose (object);
}

static void
shell_extra_workspace_view_class_init (ShellExtraWorkspaceViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  ShellWorkspacesViewBaseClass *base_class = SHELL_WORKSPACES_VIEW_BASE_CLASS (klass);

  object_class->dispose = shell_extra_workspace_view_dispose;

  actor_class->allocate = extra_view_allocate;

  base_class->update_workspace_mode = extra_view_update_workspace_mode;
  base_class->get_active_workspace = extra_view_get_active_workspace;
  base_class->prepare_to_leave_overview = extra_view_prepare_to_leave_overview;
  base_class->sync_stacking = extra_view_sync_stacking;
}

static void
shell_extra_workspace_view_init (ShellExtraWorkspaceView *self)
{
}

static ClutterActor *
extra_workspace_view_new (int                      monitor_index,
                          ShellOverview           *overview,
                          ShellWindowManager      *window_manager,
                          ShellLayoutManager      *layout_manager,
                          ShellOverviewAdjustment *overview_adjustment)
{
  ShellExtraWorkspaceView *self = g_object_new (SHELL_TYPE_EXTRA_WORKSPACE_VIEW, NULL);

  view_base_setup (SHELL_WORKSPACES_VIEW_BASE (self), monitor_index, overview,
                   window_manager, layout_manager, overview_adjustment);

  self->workspace = create_workspace (NULL, monitor_index,
                                      ST_ADJUSTMENT (overview_adjustment),
                                      window_manager, overview);
  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (self->workspace));

  return CLUTTER_ACTOR (self);
}

/* ------------------------------------------------------------------------ */
/* The views as the display sees them                                        */
/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* ShellSecondaryMonitorDisplay (private)                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SECONDARY_MONITOR_DISPLAY (shell_secondary_monitor_display_get_type ())
G_DECLARE_FINAL_TYPE (ShellSecondaryMonitorDisplay, shell_secondary_monitor_display,
                      SHELL, SECONDARY_MONITOR_DISPLAY, StWidget)

struct _ShellSecondaryMonitorDisplay
{
  StWidget parent;

  int monitor_index;
  ShellOverview *overview;                      /* weak */
  ShellWindowManager *window_manager;           /* weak */
  ShellLayoutManager *layout_manager;           /* weak */
  ShellControlsManager *controls;               /* weak */
  StAdjustment *scroll_adjustment;              /* weak */
  StAdjustment *fit_mode_adjustment;            /* weak */
  ShellOverviewAdjustment *overview_adjustment; /* weak */

  ShellThumbnailsBox *thumbnails;               /* a child */
  ClutterActor *workspaces_view;                /* a child, weak */
  GSettings *settings;
};

G_DEFINE_FINAL_TYPE (ShellSecondaryMonitorDisplay, shell_secondary_monitor_display,
                     ST_TYPE_WIDGET)

static void
get_thumbnail_params_for_state (double  state,
                                double *opacity,
                                double *scale)
{
  switch ((int) state)
    {
    case SHELL_CONTROLS_STATE_APP_GRID:
      *opacity = 0;
      *scale = 0.5;
      break;
    case SHELL_CONTROLS_STATE_HIDDEN:
    case SHELL_CONTROLS_STATE_WINDOW_PICKER:
    default:
      *opacity = 255;
      *scale = 1;
      break;
    }
}

static float
secondary_get_thumbnails_height (ShellSecondaryMonitorDisplay *self,
                                 const ClutterActorBox        *box)
{
  float width = clutter_actor_box_get_width (box);
  float height = clutter_actor_box_get_height (box);
  double expand_fraction;
  float thumbnails_height;

  if (!clutter_actor_is_visible (CLUTTER_ACTOR (self->thumbnails)))
    return 0;

  expand_fraction = shell_thumbnails_box_get_expand_fraction (self->thumbnails);
  get_preferred_natural_height (CLUTTER_ACTOR (self->thumbnails), width, &thumbnails_height);

  return MIN (thumbnails_height * expand_fraction,
              height * shell_thumbnails_box_get_max_thumbnail_scale (self->thumbnails));
}

static void
secondary_get_workspaces_box_for_state (double                 state,
                                        const ClutterActorBox *box,
                                        float                  padding,
                                        float                  thumbnails_height,
                                        float                  spacing,
                                        ClutterActorBox       *workspace_box)
{
  float width = clutter_actor_box_get_width (box);
  float height = clutter_actor_box_get_height (box);

  *workspace_box = *box;

  switch ((int) state)
    {
    case SHELL_CONTROLS_STATE_HIDDEN:
      break;
    case SHELL_CONTROLS_STATE_WINDOW_PICKER:
      box_set_origin_size (workspace_box, 0, padding + thumbnails_height + spacing,
                           width, height - 2 * padding - thumbnails_height - spacing);
      break;
    case SHELL_CONTROLS_STATE_APP_GRID:
      box_set_origin_size (workspace_box, 0, padding, width, height - 2 * padding);
      break;
    default:
      break;
    }
}

static void
secondary_allocate (ClutterActor          *actor,
                    const ClutterActorBox *box)
{
  ShellSecondaryMonitorDisplay *self = SHELL_SECONDARY_MONITOR_DISPLAY (actor);
  StThemeNode *theme_node;
  ClutterActorBox content_box, workspaces_box;
  ShellStateTransitionParams params;
  float width, height, spacing, padding, thumbnails_height;
  double expand_fraction;

  clutter_actor_set_allocation (actor, box);

  if (self->thumbnails == NULL || self->overview_adjustment == NULL)
    return;

  theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  st_theme_node_get_content_box (theme_node, box, &content_box);
  clutter_actor_box_get_size (&content_box, &width, &height);
  expand_fraction = shell_thumbnails_box_get_expand_fraction (self->thumbnails);
  spacing = st_theme_node_get_length (theme_node, "spacing") * expand_fraction;
  padding = js_round ((1 - SECONDARY_WORKSPACE_SCALE) * height / 2);

  thumbnails_height = secondary_get_thumbnails_height (self, &content_box);

  if (clutter_actor_is_visible (CLUTTER_ACTOR (self->thumbnails)))
    {
      ClutterActorBox child_box;

      box_set_origin_size (&child_box, 0, padding, width, thumbnails_height);
      clutter_actor_allocate (CLUTTER_ACTOR (self->thumbnails), &child_box);
    }

  shell_overview_adjustment_get_state_transition_params (self->overview_adjustment, &params);

  if (!params.transitioning)
    {
      secondary_get_workspaces_box_for_state (params.current_state, &content_box, padding,
                                              thumbnails_height, spacing, &workspaces_box);
    }
  else
    {
      ClutterActorBox initial_box, final_box;

      secondary_get_workspaces_box_for_state (params.initial_state, &content_box, padding,
                                              thumbnails_height, spacing, &initial_box);
      secondary_get_workspaces_box_for_state (params.final_state, &content_box, padding,
                                              thumbnails_height, spacing, &final_box);
      clutter_actor_box_interpolate (&initial_box, &final_box, params.progress, &workspaces_box);
    }

  if (self->workspaces_view != NULL)
    clutter_actor_allocate (self->workspaces_view, &workspaces_box);
}

static void
secondary_update_thumbnail_params (ShellSecondaryMonitorDisplay *self)
{
  ShellStateTransitionParams params;
  double initial_opacity, initial_scale, final_opacity, final_scale;
  double opacity, scale;

  if (!clutter_actor_is_visible (CLUTTER_ACTOR (self->thumbnails)) ||
      self->overview_adjustment == NULL)
    return;

  shell_overview_adjustment_get_state_transition_params (self->overview_adjustment, &params);

  get_thumbnail_params_for_state (params.initial_state, &initial_opacity, &initial_scale);
  get_thumbnail_params_for_state (params.final_state, &final_opacity, &final_scale);

  opacity = lerp (initial_opacity, final_opacity, params.progress);
  scale = lerp (initial_scale, final_scale, params.progress);

  clutter_actor_set_opacity (CLUTTER_ACTOR (self->thumbnails), (guint8) CLAMP (opacity, 0, 255));
  clutter_actor_set_scale (CLUTTER_ACTOR (self->thumbnails), scale, scale);
}

typedef struct
{
  GWeakRef *display;
  gboolean visible;
} ThumbnailVisibilityData;

static void
thumbnail_visibility_data_free (gpointer user_data)
{
  ThumbnailVisibilityData *data = user_data;

  weak_ref_free (data->display);
  g_free (data);
}

static void
thumbnail_visibility_done_cb (gboolean finished,
                              gpointer user_data)
{
  ThumbnailVisibilityData *data = user_data;
  g_autoptr (ShellSecondaryMonitorDisplay) self = g_weak_ref_get (data->display);

  if (self == NULL || !finished || self->thumbnails == NULL)
    return;

  set_actor_visible (CLUTTER_ACTOR (self->thumbnails), data->visible);
}

static void
secondary_update_thumbnail_visibility (ShellSecondaryMonitorDisplay *self)
{
  gboolean visible;
  ShellEaseParams params = {
    .duration = SHELL_SIDE_CONTROLS_ANIMATION_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
  ThumbnailVisibilityData *data;
  g_auto (GValue) value = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;

  if (self->thumbnails == NULL || self->settings == NULL)
    return;

  visible = shell_thumbnails_box_get_should_show (self->thumbnails) &&
            !g_settings_get_boolean (self->settings, "workspaces-only-on-primary");

  if (clutter_actor_is_visible (CLUTTER_ACTOR (self->thumbnails)) == visible)
    return;

  clutter_actor_show (CLUTTER_ACTOR (self->thumbnails));
  secondary_update_thumbnail_params (self);

  data = g_new0 (ThumbnailVisibilityData, 1);
  data->display = weak_ref_new (self);
  data->visible = visible;

  g_value_init (&value, G_TYPE_DOUBLE);
  g_value_set_double (&value, visible ? 1 : 0);
  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (self->thumbnails), "expand-fraction",
                                       &value, &params, thumbnail_visibility_done_cb,
                                       data, thumbnail_visibility_data_free, &error))
    g_debug ("%s: %s", G_STRFUNC, error->message);
}

static void
secondary_thumbnails_should_show_cb (ShellThumbnailsBox            *box,
                                     GParamSpec                    *pspec,
                                     ShellSecondaryMonitorDisplay  *self)
{
  secondary_update_thumbnail_visibility (self);
}

static void
secondary_overview_adjustment_changed_cb (StAdjustment                 *adjustment,
                                          GParamSpec                   *pspec,
                                          ShellSecondaryMonitorDisplay *self)
{
  secondary_update_thumbnail_params (self);
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

static void
secondary_update_workspaces_view (ShellSecondaryMonitorDisplay *self)
{
  ClutterActor *view;

  if (self->workspaces_view != NULL)
    clutter_actor_destroy (self->workspaces_view);
  g_clear_weak_pointer (&self->workspaces_view);

  if (g_settings_get_boolean (self->settings, "workspaces-only-on-primary"))
    {
      view = extra_workspace_view_new (self->monitor_index, self->overview,
                                       self->window_manager, self->layout_manager,
                                       self->overview_adjustment);
    }
  else
    {
      view = workspaces_view_new (self->monitor_index, self->overview,
                                  self->window_manager, self->layout_manager,
                                  self->controls, self->scroll_adjustment,
                                  self->fit_mode_adjustment, self->overview_adjustment);
    }

  g_set_weak_pointer (&self->workspaces_view, view);
  clutter_actor_add_child (CLUTTER_ACTOR (self), view);
}

static void
secondary_workspaces_on_primary_changed (ShellSecondaryMonitorDisplay *self)
{
  secondary_update_workspaces_view (self);
  secondary_update_thumbnail_visibility (self);
}

static void
secondary_settings_changed_cb (GSettings                    *settings,
                               const char                   *key,
                               ShellSecondaryMonitorDisplay *self)
{
  secondary_workspaces_on_primary_changed (self);
}

static void
secondary_dispose (GObject *object)
{
  ShellSecondaryMonitorDisplay *self = SHELL_SECONDARY_MONITOR_DISPLAY (object);

  g_clear_object (&self->settings);

  g_clear_weak_pointer (&self->overview);
  g_clear_weak_pointer (&self->window_manager);
  g_clear_weak_pointer (&self->layout_manager);
  g_clear_weak_pointer (&self->controls);
  g_clear_weak_pointer (&self->scroll_adjustment);
  g_clear_weak_pointer (&self->fit_mode_adjustment);
  g_clear_weak_pointer (&self->overview_adjustment);
  g_clear_weak_pointer (&self->workspaces_view);
  self->thumbnails = NULL;

  G_OBJECT_CLASS (shell_secondary_monitor_display_parent_class)->dispose (object);
}

static void
shell_secondary_monitor_display_class_init (ShellSecondaryMonitorDisplayClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->dispose = secondary_dispose;

  actor_class->allocate = secondary_allocate;
}

static void
shell_secondary_monitor_display_init (ShellSecondaryMonitorDisplay *self)
{
}

static ClutterActor *
secondary_monitor_display_new (int                      monitor_index,
                               ShellOverview           *overview,
                               ShellWindowManager      *window_manager,
                               ShellLayoutManager      *layout_manager,
                               ShellControlsManager    *controls,
                               StAdjustment            *scroll_adjustment,
                               StAdjustment            *fit_mode_adjustment,
                               ShellOverviewAdjustment *overview_adjustment)
{
  ShellSecondaryMonitorDisplay *self =
    g_object_new (SHELL_TYPE_SECONDARY_MONITOR_DISPLAY,
                  "style-class", "secondary-monitor-workspaces",
                  "clip-to-allocation", TRUE,
                  NULL);
  ClutterConstraint *constraint;
  ClutterActor *thumbnails;

  self->monitor_index = monitor_index;
  g_set_weak_pointer (&self->overview, overview);
  g_set_weak_pointer (&self->window_manager, window_manager);
  g_set_weak_pointer (&self->layout_manager, layout_manager);
  g_set_weak_pointer (&self->controls, controls);
  g_set_weak_pointer (&self->scroll_adjustment, scroll_adjustment);
  g_set_weak_pointer (&self->fit_mode_adjustment, fit_mode_adjustment);
  g_set_weak_pointer (&self->overview_adjustment, overview_adjustment);

  constraint = g_object_new (SHELL_TYPE_MONITOR_CONSTRAINT,
                             "index", monitor_index,
                             "work-area", TRUE,
                             NULL);
  clutter_actor_add_constraint (CLUTTER_ACTOR (self), constraint);

  thumbnails = CLUTTER_ACTOR (shell_thumbnails_box_new (scroll_adjustment, monitor_index,
                                                        window_manager));
  self->thumbnails = SHELL_THUMBNAILS_BOX (thumbnails);
  shell_controls_connect_thumbnails_box (overview, self->thumbnails);
  clutter_actor_add_child (CLUTTER_ACTOR (self), thumbnails);

  g_signal_connect_object (thumbnails, "notify::should-show",
                           G_CALLBACK (secondary_thumbnails_should_show_cb), self, 0);

  g_signal_connect_object (overview_adjustment, "notify::value",
                           G_CALLBACK (secondary_overview_adjustment_changed_cb), self, 0);

  self->settings = g_settings_new (MUTTER_SCHEMA);
  g_signal_connect_object (self->settings, "changed::workspaces-only-on-primary",
                           G_CALLBACK (secondary_settings_changed_cb), self, 0);
  secondary_workspaces_on_primary_changed (self);

  return CLUTTER_ACTOR (self);
}

static ClutterActor *
secondary_monitor_display_get_active_workspace (ClutterActor *actor)
{
  ShellSecondaryMonitorDisplay *self = SHELL_SECONDARY_MONITOR_DISPLAY (actor);

  if (self->workspaces_view == NULL)
    return NULL;

  return view_base_get_active_workspace (SHELL_WORKSPACES_VIEW_BASE (self->workspaces_view));
}

/* ------------------------------------------------------------------------ */
/* ShellWorkspacesDisplay                                                    */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ClutterActor *actor; /* weak */
} ViewSlot;

static void
view_slot_free (gpointer data)
{
  ViewSlot *slot = data;

  g_clear_weak_pointer (&slot->actor);
  g_free (slot);
}

struct _ShellWorkspacesDisplay
{
  StWidget parent;

  ShellOverview *overview;                      /* weak */
  ShellControlsManager *controls;               /* weak */
  ShellWindowManager *window_manager;           /* weak */
  ShellLayoutManager *layout_manager;           /* weak */
  StAdjustment *scroll_adjustment;              /* weak */
  ShellOverviewAdjustment *overview_adjustment; /* weak */
  StAdjustment *fit_mode_adjustment;

  ShellSwipeTracker *swipe_tracker;
  GSettings *settings;

  GPtrArray *views;  /* of ViewSlot, one per monitor */
  int primary_index;

  gboolean primary_visible;
  gboolean in_window_drag;
  gboolean leaving_overview;
  gboolean gesture_active;

  gulong restacked_id;
  gulong scroll_id;
  MetaDisplay *display;       /* weak, while the handlers are connected */
  ClutterActor *scroll_actor; /* weak, the overview group */
};

G_DEFINE_FINAL_TYPE (ShellWorkspacesDisplay, shell_workspaces_display, ST_TYPE_WIDGET)

static ClutterActor *
display_view_at (ShellWorkspacesDisplay *self,
                 int                     index)
{
  ViewSlot *slot;

  if (index < 0 || (guint) index >= self->views->len)
    return NULL;

  slot = g_ptr_array_index (self->views, index);

  return slot->actor;
}

static ClutterActor *
display_get_primary_view (ShellWorkspacesDisplay *self)
{
  if (self->views->len == 0)
    return NULL;

  return display_view_at (self, self->primary_index);
}

static gboolean
workspaces_only_on_primary (ShellWorkspacesDisplay *self)
{
  return g_settings_get_boolean (self->settings, "workspaces-only-on-primary");
}

static ClutterActor *
view_get_active_workspace (ClutterActor *view)
{
  if (SHELL_IS_SECONDARY_MONITOR_DISPLAY (view))
    return secondary_monitor_display_get_active_workspace (view);

  return view_base_get_active_workspace (SHELL_WORKSPACES_VIEW_BASE (view));
}

static void
view_prepare_to_leave_overview (ClutterActor *view)
{
  ShellWorkspacesViewBase *base;

  if (SHELL_IS_SECONDARY_MONITOR_DISPLAY (view))
    {
      ShellSecondaryMonitorDisplay *secondary = SHELL_SECONDARY_MONITOR_DISPLAY (view);

      if (secondary->workspaces_view != NULL)
        view_prepare_to_leave_overview (secondary->workspaces_view);
      return;
    }

  base = SHELL_WORKSPACES_VIEW_BASE (view);
  if (SHELL_WORKSPACES_VIEW_BASE_GET_CLASS (base)->prepare_to_leave_overview != NULL)
    SHELL_WORKSPACES_VIEW_BASE_GET_CLASS (base)->prepare_to_leave_overview (base);
}

static void
view_sync_stacking (ClutterActor *view,
                    const guint32 *sequences,
                    gsize          n_sequences)
{
  ShellWorkspacesViewBase *base;

  if (SHELL_IS_SECONDARY_MONITOR_DISPLAY (view))
    {
      ShellSecondaryMonitorDisplay *secondary = SHELL_SECONDARY_MONITOR_DISPLAY (view);

      if (secondary->workspaces_view != NULL)
        view_sync_stacking (secondary->workspaces_view, sequences, n_sequences);
      return;
    }

  base = SHELL_WORKSPACES_VIEW_BASE (view);
  if (SHELL_WORKSPACES_VIEW_BASE_GET_CLASS (base)->sync_stacking != NULL)
    SHELL_WORKSPACES_VIEW_BASE_GET_CLASS (base)->sync_stacking (base, sequences, n_sequences);
}

static void
view_start_touch_gesture (ClutterActor *view)
{
  ShellWorkspacesViewBase *base;

  if (SHELL_IS_SECONDARY_MONITOR_DISPLAY (view))
    {
      ShellSecondaryMonitorDisplay *secondary = SHELL_SECONDARY_MONITOR_DISPLAY (view);

      if (secondary->workspaces_view != NULL)
        view_start_touch_gesture (secondary->workspaces_view);
      return;
    }

  base = SHELL_WORKSPACES_VIEW_BASE (view);
  if (SHELL_WORKSPACES_VIEW_BASE_GET_CLASS (base)->start_touch_gesture != NULL)
    SHELL_WORKSPACES_VIEW_BASE_GET_CLASS (base)->start_touch_gesture (base);
}

static void
view_end_touch_gesture (ClutterActor *view)
{
  ShellWorkspacesViewBase *base;

  if (SHELL_IS_SECONDARY_MONITOR_DISPLAY (view))
    {
      ShellSecondaryMonitorDisplay *secondary = SHELL_SECONDARY_MONITOR_DISPLAY (view);

      if (secondary->workspaces_view != NULL)
        view_end_touch_gesture (secondary->workspaces_view);
      return;
    }

  base = SHELL_WORKSPACES_VIEW_BASE (view);
  if (SHELL_WORKSPACES_VIEW_BASE_GET_CLASS (base)->end_touch_gesture != NULL)
    SHELL_WORKSPACES_VIEW_BASE_GET_CLASS (base)->end_touch_gesture (base);
}

static void
display_update_swipe_tracker (ShellWorkspacesDisplay *self)
{
  if (self->swipe_tracker == NULL)
    return;

  shell_swipe_tracker_set_enabled (self->swipe_tracker,
                                   clutter_actor_is_mapped (CLUTTER_ACTOR (self)) &&
                                   !self->in_window_drag &&
                                   !self->leaving_overview);
}

static void
display_mapped_changed_cb (ShellWorkspacesDisplay *self,
                           GParamSpec             *pspec)
{
  display_update_swipe_tracker (self);
}

static void
display_window_drag_begin_cb (ShellOverview          *overview,
                              MetaWindow             *window,
                              ShellWorkspacesDisplay *self)
{
  self->in_window_drag = TRUE;
  display_update_swipe_tracker (self);
}

static void
display_window_drag_end_cb (ShellOverview          *overview,
                            MetaWindow             *window,
                            ShellWorkspacesDisplay *self)
{
  self->in_window_drag = FALSE;
  display_update_swipe_tracker (self);
}

static void
display_workspaces_reordered_cb (MetaWorkspaceManager  *workspace_manager,
                                 ShellWorkspacesDisplay *self)
{
  if (self->scroll_adjustment == NULL)
    return;

  st_adjustment_set_value (self->scroll_adjustment,
                           meta_workspace_manager_get_active_workspace_index (workspace_manager));
}

static void
display_switch_workspace_cb (ShellWM                *shell_wm,
                             int                     from,
                             int                     to,
                             int                     direction,
                             ShellWorkspacesDisplay *self)
{
  if (self->gesture_active || self->scroll_adjustment == NULL)
    return;

  ease_adjustment (self->scroll_adjustment, to, WORKSPACE_SWITCH_TIME,
                   CLUTTER_EASE_OUT_CUBIC, NULL, NULL, NULL);
}

static void
display_update_tracker_orientation (ShellWorkspacesDisplay *self)
{
  if (self->swipe_tracker == NULL)
    return;

  shell_swipe_tracker_set_orientation (self->swipe_tracker,
                                       !is_vertical_layout ()
                                       ? CLUTTER_ORIENTATION_HORIZONTAL
                                       : CLUTTER_ORIENTATION_VERTICAL);
}

static void
display_layout_rows_changed_cb (MetaWorkspaceManager  *workspace_manager,
                                GParamSpec            *pspec,
                                ShellWorkspacesDisplay *self)
{
  display_update_tracker_orientation (self);
}

static void
display_switch_workspace_begin_cb (ShellSwipeTracker      *tracker,
                                   guint                   monitor,
                                   ShellWorkspacesDisplay *self)
{
  MetaWorkspaceManager *workspace_manager = get_workspace_manager ();
  g_autofree double *points = NULL;
  double distance, progress;
  int n_workspaces, i;

  if (workspaces_only_on_primary (self) && (int) monitor != self->primary_index)
    return;

  if (self->scroll_adjustment == NULL)
    return;

  if (self->gesture_active)
    st_adjustment_remove_transition (self->scroll_adjustment, "value");

  distance = is_vertical_layout ()
    ? clutter_actor_get_height (CLUTTER_ACTOR (self))
    : clutter_actor_get_width (CLUTTER_ACTOR (self));

  for (i = 0; (guint) i < self->views->len; i++)
    {
      ClutterActor *view = display_view_at (self, i);

      if (view != NULL)
        view_start_touch_gesture (view);
    }

  progress = st_adjustment_get_value (self->scroll_adjustment) /
             st_adjustment_get_page_size (self->scroll_adjustment);

  n_workspaces = meta_workspace_manager_get_n_workspaces (workspace_manager);
  points = g_new (double, n_workspaces);
  for (i = 0; i < n_workspaces; i++)
    points[i] = i;

  shell_swipe_tracker_confirm_swipe (tracker, distance, points, n_workspaces,
                                     progress, js_round (progress));

  self->gesture_active = TRUE;
}

static void
display_switch_workspace_update_cb (ShellSwipeTracker      *tracker,
                                    double                  progress,
                                    ShellWorkspacesDisplay *self)
{
  if (self->scroll_adjustment == NULL)
    return;

  st_adjustment_set_value (self->scroll_adjustment,
                           progress * st_adjustment_get_page_size (self->scroll_adjustment));
}

static void
display_end_touch_gesture (ShellWorkspacesDisplay *self)
{
  guint i;

  for (i = 0; i < self->views->len; i++)
    {
      ClutterActor *view = display_view_at (self, i);

      if (view != NULL)
        view_end_touch_gesture (view);
    }
  self->gesture_active = FALSE;
}

typedef struct
{
  GWeakRef *display;
  MetaWorkspace *workspace;
} SwitchEndData;

static void
switch_end_data_free (gpointer user_data)
{
  SwitchEndData *data = user_data;

  weak_ref_free (data->display);
  g_clear_object (&data->workspace);
  g_free (data);
}

static gboolean
workspace_is_active (MetaWorkspace *workspace)
{
  gboolean active = FALSE;

  g_object_get (workspace, "active", &active, NULL);

  return active;
}

static void
display_switch_workspace_end_done_cb (gboolean finished,
                                      gpointer user_data)
{
  SwitchEndData *data = user_data;
  g_autoptr (ShellWorkspacesDisplay) self = g_weak_ref_get (data->display);

  if (self == NULL || !finished)
    return;

  if (data->workspace != NULL && !workspace_is_active (data->workspace))
    meta_workspace_activate (data->workspace, shell_global_get_current_time (shell_global_get ()));
  display_end_touch_gesture (self);
}

static void
display_switch_workspace_end_cb (ShellSwipeTracker      *tracker,
                                 guint64                 duration,
                                 double                  end_progress,
                                 ShellWorkspacesDisplay *self)
{
  SwitchEndData *data;

  if (self->scroll_adjustment == NULL)
    return;

  data = g_new0 (SwitchEndData, 1);
  data->display = weak_ref_new (self);
  data->workspace = meta_workspace_manager_get_workspace_by_index (get_workspace_manager (),
                                                                   (int) end_progress);
  if (data->workspace != NULL)
    g_object_ref (data->workspace);

  ease_adjustment (self->scroll_adjustment, end_progress, duration,
                   CLUTTER_EASE_OUT_CUBIC,
                   display_switch_workspace_end_done_cb, data, switch_end_data_free);
}

static gboolean
display_navigate_focus (StWidget        *widget,
                        ClutterActor    *from,
                        StDirectionType  direction)
{
  ShellWorkspacesDisplay *self = SHELL_WORKSPACES_DISPLAY (widget);
  ClutterActor *primary_view;

  if (from == CLUTTER_ACTOR (self) && direction == ST_DIR_DOWN)
    direction = ST_DIR_TAB_FORWARD;

  primary_view = display_get_primary_view (self);
  if (primary_view == NULL)
    return FALSE;

  return st_widget_navigate_focus (ST_WIDGET (primary_view), from, direction, FALSE);
}

static void
display_destroy_views (ShellWorkspacesDisplay *self)
{
  g_autoptr (GPtrArray) old_views = g_steal_pointer (&self->views);
  guint i;

  self->views = g_ptr_array_new_with_free_func (view_slot_free);

  for (i = 0; i < old_views->len; i++)
    {
      ViewSlot *slot = g_ptr_array_index (old_views, i);

      if (slot->actor != NULL)
        clutter_actor_destroy (slot->actor);
    }
}

static void
display_disconnect_overview_handlers (ShellWorkspacesDisplay *self)
{
  if (self->display != NULL)
    g_clear_signal_handler (&self->restacked_id, self->display);
  if (self->scroll_actor != NULL)
    g_clear_signal_handler (&self->scroll_id, self->scroll_actor);
  self->restacked_id = 0;
  self->scroll_id = 0;
  g_clear_weak_pointer (&self->display);
  g_clear_weak_pointer (&self->scroll_actor);
}

static void
display_hide (ClutterActor *actor)
{
  ShellWorkspacesDisplay *self = SHELL_WORKSPACES_DISPLAY (actor);

  display_disconnect_overview_handlers (self);
  display_destroy_views (self);

  self->leaving_overview = FALSE;

  CLUTTER_ACTOR_CLASS (shell_workspaces_display_parent_class)->hide (actor);
}

static void
display_update_workspaces_views (ShellWorkspacesDisplay *self)
{
  ClutterActor *overview_group = CLUTTER_ACTOR (shell_layout_manager_get_overview_group (self->layout_manager));
  int n_monitors, i;

  display_destroy_views (self);

  self->primary_index = shell_layout_manager_get_primary_index (self->layout_manager);
  n_monitors = shell_layout_manager_get_n_monitors (self->layout_manager);
  for (i = 0; i < n_monitors; i++)
    {
      ClutterActor *view;
      ViewSlot *slot;

      if (i == self->primary_index)
        {
          view = workspaces_view_new (i, self->overview, self->window_manager,
                                      self->layout_manager, self->controls,
                                      self->scroll_adjustment, self->fit_mode_adjustment,
                                      self->overview_adjustment);

          set_actor_visible (view, self->primary_visible);
          g_object_bind_property (self, "opacity", view, "opacity", G_BINDING_SYNC_CREATE);
          clutter_actor_add_child (CLUTTER_ACTOR (self), view);
        }
      else
        {
          view = secondary_monitor_display_new (i, self->overview, self->window_manager,
                                                self->layout_manager, self->controls,
                                                self->scroll_adjustment, self->fit_mode_adjustment,
                                                self->overview_adjustment);
          clutter_actor_add_child (overview_group, view);
        }

      slot = g_new0 (ViewSlot, 1);
      g_set_weak_pointer (&slot->actor, view);
      g_ptr_array_add (self->views, slot);
    }
}

static int
get_monitor_index_for_event (const ClutterEvent *event)
{
  MtkRectangle rect = { 0, 0, 1, 1 };
  float x, y;

  clutter_event_get_coords (event, &x, &y);
  rect.x = x;
  rect.y = y;

  return meta_display_get_monitor_index_for_rect (get_display (), &rect);
}

static void
display_restacked_cb (MetaDisplay            *display,
                      ShellWorkspacesDisplay *self)
{
  g_autoptr (GList) actors = shell_global_get_window_actors (shell_global_get ());
  g_autoptr (GArray) sequences = g_array_new (FALSE, FALSE, sizeof (guint32));
  GList *l;
  guint i;

  /* The workspaces want the stable sequences of the windows in stacking order */
  for (l = actors; l != NULL; l = l->next)
    {
      MetaWindow *window = meta_window_actor_get_meta_window (l->data);
      guint32 sequence = meta_window_get_stable_sequence (window);

      g_array_append_val (sequences, sequence);
    }

  for (i = 0; i < self->views->len; i++)
    {
      ClutterActor *view = display_view_at (self, i);

      if (view != NULL)
        view_sync_stacking (view, (const guint32 *) sequences->data, sequences->len);
    }
}

static gboolean
display_scroll_event_cb (ClutterActor           *actor,
                         const ClutterEvent     *event,
                         ShellWorkspacesDisplay *self)
{
  /* The overview did not care about the value its handlers returned */
  if (shell_swipe_tracker_can_handle_scroll_event (self->swipe_tracker, event))
    return CLUTTER_EVENT_PROPAGATE;

  if (!clutter_actor_is_mapped (CLUTTER_ACTOR (self)))
    return CLUTTER_EVENT_PROPAGATE;

  if (workspaces_only_on_primary (self) &&
      get_monitor_index_for_event (event) != self->primary_index)
    return CLUTTER_EVENT_PROPAGATE;

  if (self->window_manager != NULL)
    shell_window_manager_handle_workspace_scroll (self->window_manager, event);

  return CLUTTER_EVENT_PROPAGATE;
}

static gboolean
display_move_to_workspace (ShellWorkspacesDisplay *self,
                           MetaMotionDirection     which)
{
  MetaWorkspace *workspace;

  if (st_adjustment_get_value (ST_ADJUSTMENT (self->overview_adjustment)) !=
      SHELL_CONTROLS_STATE_WINDOW_PICKER)
    return CLUTTER_EVENT_PROPAGATE;

  if (!clutter_actor_get_reactive (CLUTTER_ACTOR (self)))
    return CLUTTER_EVENT_PROPAGATE;

  workspace = meta_workspace_get_neighbor (meta_workspace_manager_get_active_workspace (get_workspace_manager ()),
                                           which);
  if (workspace != NULL && self->window_manager != NULL)
    shell_window_manager_action_move_workspace (self->window_manager, workspace);

  return CLUTTER_EVENT_STOP;
}

static gboolean
display_binding_next_cb (GObject             *object,
                         const char          *action_name,
                         guint                key_val,
                         ClutterModifierType  modifiers,
                         gpointer             user_data)
{
  ShellWorkspacesDisplay *self = SHELL_WORKSPACES_DISPLAY (object);
  gboolean rtl = clutter_actor_get_text_direction (CLUTTER_ACTOR (self)) == CLUTTER_TEXT_DIRECTION_RTL;
  MetaMotionDirection which;

  if (is_vertical_layout ())
    which = META_MOTION_DOWN;
  else if (rtl)
    which = META_MOTION_LEFT;
  else
    which = META_MOTION_RIGHT;

  return display_move_to_workspace (self, which);
}

static gboolean
display_binding_prev_cb (GObject             *object,
                         const char          *action_name,
                         guint                key_val,
                         ClutterModifierType  modifiers,
                         gpointer             user_data)
{
  ShellWorkspacesDisplay *self = SHELL_WORKSPACES_DISPLAY (object);
  gboolean rtl = clutter_actor_get_text_direction (CLUTTER_ACTOR (self)) == CLUTTER_TEXT_DIRECTION_RTL;
  MetaMotionDirection which;

  if (is_vertical_layout ())
    which = META_MOTION_UP;
  else if (rtl)
    which = META_MOTION_RIGHT;
  else
    which = META_MOTION_LEFT;

  return display_move_to_workspace (self, which);
}

static gboolean
display_binding_first_cb (GObject             *object,
                          const char          *action_name,
                          guint                key_val,
                          ClutterModifierType  modifiers,
                          gpointer             user_data)
{
  ShellWorkspacesDisplay *self = SHELL_WORKSPACES_DISPLAY (object);
  MetaWorkspace *workspace = meta_workspace_manager_get_workspace_by_index (get_workspace_manager (), 0);

  if (workspace != NULL && self->window_manager != NULL)
    shell_window_manager_action_move_workspace (self->window_manager, workspace);

  return CLUTTER_EVENT_PROPAGATE;
}

static gboolean
display_binding_last_cb (GObject             *object,
                         const char          *action_name,
                         guint                key_val,
                         ClutterModifierType  modifiers,
                         gpointer             user_data)
{
  ShellWorkspacesDisplay *self = SHELL_WORKSPACES_DISPLAY (object);
  MetaWorkspaceManager *workspace_manager = get_workspace_manager ();
  MetaWorkspace *workspace =
    meta_workspace_manager_get_workspace_by_index (workspace_manager,
                                                   meta_workspace_manager_get_n_workspaces (workspace_manager) - 1);

  if (workspace != NULL && self->window_manager != NULL)
    shell_window_manager_action_move_workspace (self->window_manager, workspace);

  return CLUTTER_EVENT_PROPAGATE;
}

static void
shell_workspaces_display_dispose (GObject *object)
{
  ShellWorkspacesDisplay *self = SHELL_WORKSPACES_DISPLAY (object);

  display_disconnect_overview_handlers (self);

  if (self->swipe_tracker != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->swipe_tracker, self);
      shell_swipe_tracker_destroy (self->swipe_tracker);
      g_clear_object (&self->swipe_tracker);
    }

  g_clear_object (&self->settings);
  g_clear_object (&self->fit_mode_adjustment);

  if (self->views != NULL)
    g_ptr_array_set_size (self->views, 0);

  g_clear_weak_pointer (&self->overview);
  g_clear_weak_pointer (&self->controls);
  g_clear_weak_pointer (&self->window_manager);
  g_clear_weak_pointer (&self->layout_manager);
  g_clear_weak_pointer (&self->scroll_adjustment);
  g_clear_weak_pointer (&self->overview_adjustment);

  G_OBJECT_CLASS (shell_workspaces_display_parent_class)->dispose (object);
}

static void
shell_workspaces_display_finalize (GObject *object)
{
  ShellWorkspacesDisplay *self = SHELL_WORKSPACES_DISPLAY (object);

  g_clear_pointer (&self->views, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_workspaces_display_parent_class)->finalize (object);
}

static void
shell_workspaces_display_class_init (ShellWorkspacesDisplayClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);
  ClutterBindingPool *binding_pool;

  object_class->dispose = shell_workspaces_display_dispose;
  object_class->finalize = shell_workspaces_display_finalize;

  actor_class->hide = display_hide;

  widget_class->navigate_focus = display_navigate_focus;

  binding_pool = clutter_binding_pool_get_for_class (klass);

  clutter_binding_pool_install_action (binding_pool, "next", CLUTTER_KEY_Page_Down, 0,
                                       G_CALLBACK (display_binding_next_cb), NULL, NULL);
  clutter_binding_pool_install_action (binding_pool, "prev", CLUTTER_KEY_Page_Up, 0,
                                       G_CALLBACK (display_binding_prev_cb), NULL, NULL);
  clutter_binding_pool_install_action (binding_pool, "first", CLUTTER_KEY_Home, 0,
                                       G_CALLBACK (display_binding_first_cb), NULL, NULL);
  clutter_binding_pool_install_action (binding_pool, "last", CLUTTER_KEY_End, 0,
                                       G_CALLBACK (display_binding_last_cb), NULL, NULL);
}

static void
shell_workspaces_display_init (ShellWorkspacesDisplay *self)
{
  self->views = g_ptr_array_new_with_free_func (view_slot_free);
  self->primary_visible = TRUE;
  self->primary_index = -1;
}

/**
 * shell_workspaces_display_new:
 * @overview: the overview
 * @controls: the controls of the overview, which know the boxes of the
 *   workspaces
 * @scroll_adjustment: the adjustment with the active workspace
 * @overview_adjustment: the adjustment with the state of the overview
 * @window_manager: the window manager
 * @layout_manager: the layout manager
 *
 * Creates the workspaces of the overview (`new WorkspacesDisplay (controls,
 * scrollAdjustment, overviewAdjustment)`).
 *
 * Returns: (transfer none): a new #ShellWorkspacesDisplay, which is floating
 */
ShellWorkspacesDisplay *
shell_workspaces_display_new (ShellOverview           *overview,
                              ShellControlsManager    *controls,
                              StAdjustment            *scroll_adjustment,
                              ShellOverviewAdjustment *overview_adjustment,
                              ShellWindowManager      *window_manager,
                              ShellLayoutManager      *layout_manager)
{
  ShellWorkspacesDisplay *self;
  ShellWM *shell_wm = shell_global_get_window_manager (shell_global_get ());
  MetaWorkspaceManager *workspace_manager = get_workspace_manager ();

  g_return_val_if_fail (SHELL_IS_OVERVIEW (overview), NULL);
  g_return_val_if_fail (SHELL_IS_CONTROLS_MANAGER (controls), NULL);
  g_return_val_if_fail (ST_IS_ADJUSTMENT (scroll_adjustment), NULL);
  g_return_val_if_fail (SHELL_IS_OVERVIEW_ADJUSTMENT (overview_adjustment), NULL);
  g_return_val_if_fail (SHELL_IS_LAYOUT_MANAGER (layout_manager), NULL);

  self = g_object_new (SHELL_TYPE_WORKSPACES_DISPLAY,
                       "layout-manager", clutter_bin_layout_new (),
                       "reactive", TRUE,
                       NULL);

  g_set_weak_pointer (&self->overview, overview);
  g_set_weak_pointer (&self->controls, controls);
  g_set_weak_pointer (&self->window_manager, window_manager);
  g_set_weak_pointer (&self->layout_manager, layout_manager);
  g_set_weak_pointer (&self->scroll_adjustment, scroll_adjustment);
  g_set_weak_pointer (&self->overview_adjustment, overview_adjustment);

  self->primary_index = shell_layout_manager_get_primary_index (layout_manager);

  self->fit_mode_adjustment = st_adjustment_new (CLUTTER_ACTOR (self),
                                                 SHELL_FIT_MODE_SINGLE,
                                                 SHELL_FIT_MODE_SINGLE,
                                                 SHELL_FIT_MODE_ALL,
                                                 0, 0, 0);

  self->settings = g_settings_new (MUTTER_SCHEMA);

  g_signal_connect_object (shell_wm, "switch-workspace",
                           G_CALLBACK (display_switch_workspace_cb), self, 0);

  self->swipe_tracker = g_object_new (SHELL_TYPE_SWIPE_TRACKER,
                                      "actor", shell_layout_manager_get_overview_group (layout_manager),
                                      "orientation", CLUTTER_ORIENTATION_HORIZONTAL,
                                      "allowed-modes", SHELL_ACTION_MODE_OVERVIEW,
                                      "allow-drag", FALSE,
                                      "name", "WorkspacesView swipe tracker",
                                      "allow-long-swipes", TRUE,
                                      NULL);
  g_signal_connect (self->swipe_tracker, "begin",
                    G_CALLBACK (display_switch_workspace_begin_cb), self);
  g_signal_connect (self->swipe_tracker, "update",
                    G_CALLBACK (display_switch_workspace_update_cb), self);
  g_signal_connect (self->swipe_tracker, "end",
                    G_CALLBACK (display_switch_workspace_end_cb), self);
  g_signal_connect (self, "notify::mapped",
                    G_CALLBACK (display_mapped_changed_cb), NULL);

  g_signal_connect_object (workspace_manager, "workspaces-reordered",
                           G_CALLBACK (display_workspaces_reordered_cb), self, 0);
  g_signal_connect_object (workspace_manager, "notify::layout-rows",
                           G_CALLBACK (display_layout_rows_changed_cb), self, 0);
  display_update_tracker_orientation (self);

  g_signal_connect_object (overview, "window-drag-begin",
                           G_CALLBACK (display_window_drag_begin_cb), self, 0);
  g_signal_connect_object (overview, "window-drag-end",
                           G_CALLBACK (display_window_drag_end_cb), self, 0);

  return self;
}

/**
 * shell_workspaces_display_get_fit_mode_adjustment:
 * @self: a #ShellWorkspacesDisplay
 *
 * Returns: (transfer none): the adjustment with the #ShellFitMode of the
 *   views (`fitModeAdjustment`)
 */
StAdjustment *
shell_workspaces_display_get_fit_mode_adjustment (ShellWorkspacesDisplay *self)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACES_DISPLAY (self), NULL);

  return self->fit_mode_adjustment;
}

/**
 * shell_workspaces_display_set_primary_workspace_visible:
 * @self: a #ShellWorkspacesDisplay
 * @visible: whether the workspaces of the primary monitor are shown
 *
 * Shows or hides the workspaces of the primary monitor (they are hidden
 * while the user searches).
 */
void
shell_workspaces_display_set_primary_workspace_visible (ShellWorkspacesDisplay *self,
                                                        gboolean                visible)
{
  ClutterActor *primary_view;

  g_return_if_fail (SHELL_IS_WORKSPACES_DISPLAY (self));

  if (self->primary_visible == visible)
    return;

  self->primary_visible = visible;

  primary_view = display_view_at (self, shell_layout_manager_get_primary_index (self->layout_manager));
  if (primary_view != NULL)
    set_actor_visible (primary_view, visible);
}

/**
 * shell_workspaces_display_prepare_to_enter_overview:
 * @self: a #ShellWorkspacesDisplay
 *
 * Shows the display and creates the views of the monitors
 * (`prepareToEnterOverview ()`).
 */
void
shell_workspaces_display_prepare_to_enter_overview (ShellWorkspacesDisplay *self)
{
  g_return_if_fail (SHELL_IS_WORKSPACES_DISPLAY (self));

  clutter_actor_show (CLUTTER_ACTOR (self));
  display_update_workspaces_views (self);

  display_disconnect_overview_handlers (self);

  g_set_weak_pointer (&self->display, get_display ());
  self->restacked_id = g_signal_connect_object (self->display, "restacked",
                                                G_CALLBACK (display_restacked_cb), self, 0);

  g_set_weak_pointer (&self->scroll_actor,
                      CLUTTER_ACTOR (shell_layout_manager_get_overview_group (self->layout_manager)));
  self->scroll_id = g_signal_connect_object (self->scroll_actor, "scroll-event",
                                             G_CALLBACK (display_scroll_event_cb), self, 0);
}

/**
 * shell_workspaces_display_prepare_to_leave_overview:
 * @self: a #ShellWorkspacesDisplay
 *
 * Freezes the workspaces and the swipes before the overview hides
 * (`prepareToLeaveOverview ()`).
 */
void
shell_workspaces_display_prepare_to_leave_overview (ShellWorkspacesDisplay *self)
{
  guint i;

  g_return_if_fail (SHELL_IS_WORKSPACES_DISPLAY (self));

  for (i = 0; i < self->views->len; i++)
    {
      ClutterActor *view = display_view_at (self, i);

      if (view != NULL)
        view_prepare_to_leave_overview (view);
    }

  self->leaving_overview = TRUE;
  display_update_swipe_tracker (self);
}

/**
 * shell_workspaces_display_active_workspace_has_maximized_windows:
 * @self: a #ShellWorkspacesDisplay
 *
 * Returns: whether the active workspace of the primary monitor has maximized
 *   windows (`activeWorkspaceHasMaximizedWindows ()`)
 */
gboolean
shell_workspaces_display_active_workspace_has_maximized_windows (ShellWorkspacesDisplay *self)
{
  ClutterActor *primary_view, *workspace;

  g_return_val_if_fail (SHELL_IS_WORKSPACES_DISPLAY (self), FALSE);

  primary_view = display_get_primary_view (self);
  if (primary_view == NULL)
    return FALSE;

  workspace = view_get_active_workspace (primary_view);
  if (workspace == NULL)
    return FALSE;

  return shell_workspace_has_maximized_windows (SHELL_WORKSPACE (workspace));
}
