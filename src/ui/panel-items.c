/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * panel-items.c: The indicators of the shell itself in the top bar. Port of
 * js/ui/panelItems.js.
 *
 * What the JS module had:
 *
 *   WorkspaceDot           ShellWorkspaceDot (final #ClutterActor, private)
 *   WorkspaceIndicators    ShellWorkspaceIndicators (final #StBoxLayout, private)
 *   ActivitiesButton       ShellActivitiesButton (final #ShellPanelMenuButton,
 *                          private)
 *   UnsafeModeIndicator    ShellUnsafeModeIndicator (final
 *                          #ShellSystemIndicator, private)
 *   QuickSettings          ShellQuickSettingsButton (final
 *                          #ShellPanelMenuButton, private)
 *   PANEL_ITEM_IMPLEMENTATIONS and createPanelItem ()
 *                          shell_panel_item_create ()
 *
 * Differences from the JS module:
 *
 *  - `Main.overview`, `Main.wm`, `Main.xdndHandler`, `Main.screenshotUI` and
 *    `Main.brightnessManager` are read from #ShellMain when they are needed,
 *    not captured, so an indicator that is created again after a session mode
 *    change finds the current ones.
 *  - The quick settings indicators are created synchronously (the JS ran
 *    `_setupIndicators ()` from an async function that nothing awaited).
 *  - `QuickSettings.addExternalIndicator ()` is
 *    shell_panel_items_add_external_indicator ().
 *  - `Config.HAVE_BLUETOOTH` and `Config.HAVE_NETWORKMANAGER` are the
 *    HAVE_BLUETOOTH and HAVE_NETWORKMANAGER defines of config.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>
#include <string.h>

#include <atk/atk.h>
#include <clutter/clutter.h>
#include <glib/gi18n-lib.h>
#include <meta/meta-context.h>

#include "shell-global.h"
#include "misc/brightness-manager.h"
#include "ui/date-menu.h"
#include "ui/dnd.h"
#include "ui/environment.h"
#include "ui/extension-order.h"
#include "ui/main.h"
#include "ui/overview.h"
#include "ui/panel-items.h"
#include "ui/panel-menu.h"
#include "ui/popup-menu.h"
#include "ui/quick-settings.h"
#include "ui/screenshot-ui.h"
#include "ui/status/accessibility.h"
#include "ui/status/auto-rotate.h"
#include "ui/status/background-apps.h"
#include "ui/status/backlight.h"
#include "ui/status/brightness.h"
#include "ui/status/camera.h"
#include "ui/status/dark-mode.h"
#include "ui/status/do-not-disturb.h"
#include "ui/status/dwell-click.h"
#include "ui/status/keyboard.h"
#include "ui/status/location.h"
#include "ui/status/night-light.h"
#include "ui/status/power-profiles.h"
#include "ui/status/remote-access.h"
#include "ui/status/rfkill.h"
#include "ui/status/system.h"
#include "ui/status/thunderbolt.h"
#include "ui/status/volume.h"
#ifdef HAVE_NETWORKMANAGER
#include "ui/status/network.h"
#endif
#ifdef HAVE_BLUETOOTH
#include "ui/status/bluetooth.h"
#endif
#include "ui/window-manager.h"

#define BUTTON_DND_ACTIVATION_TIMEOUT 250

#define N_QUICK_SETTINGS_COLUMNS 2

#define INACTIVE_WORKSPACE_DOT_SCALE 0.75

static inline double
lerp (double start,
      double end,
      double progress)
{
  return start + progress * (end - start);
}

static void
overview_hide (void)
{
  ShellOverview *overview = shell_main_get_overview (shell_main_get_default ());

  if (overview != NULL)
    shell_overview_hide (overview);
}

static void
overview_hide_requested_cb (GObject *source,
                            gpointer user_data)
{
  overview_hide ();
}

/* ------------------------------------------------------------------------ */
/* ShellWorkspaceDot                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WORKSPACE_DOT (shell_workspace_dot_get_type ())
G_DECLARE_FINAL_TYPE (ShellWorkspaceDot, shell_workspace_dot,
                      SHELL, WORKSPACE_DOT, ClutterActor)

enum
{
  DOT_PROP_0,

  DOT_PROP_EXPANSION,
  DOT_PROP_WIDTH_MULTIPLIER,

  DOT_N_PROPS
};

static GParamSpec *dot_props[DOT_N_PROPS] = { NULL, };

struct _ShellWorkspaceDot
{
  ClutterActor parent;

  ClutterActor *dot;      /* weak */

  double expansion;
  double width_multiplier;
  gboolean destroying;
};

G_DEFINE_FINAL_TYPE (ShellWorkspaceDot, shell_workspace_dot, CLUTTER_TYPE_ACTOR)

static void
workspace_dot_update_visuals (ShellWorkspaceDot *self)
{
  double scale;

  if (self->dot == NULL)
    return;

  scale = lerp (INACTIVE_WORKSPACE_DOT_SCALE, 1.0, self->expansion);

  clutter_actor_set_opacity (self->dot,
                             (guint8) (lerp (0.50, 1.0, self->expansion) * 255));
  clutter_actor_set_scale (self->dot, scale, scale);
}

static void
shell_workspace_dot_get_preferred_width (ClutterActor *actor,
                                         float         for_height,
                                         float        *min_width_p,
                                         float        *nat_width_p)
{
  ShellWorkspaceDot *self = SHELL_WORKSPACE_DOT (actor);
  double factor = lerp (1.0, self->width_multiplier, self->expansion);
  float min_width = 0, nat_width = 0;

  if (self->dot != NULL)
    clutter_actor_get_preferred_width (self->dot, for_height, &min_width, &nat_width);

  if (min_width_p != NULL)
    *min_width_p = roundf (min_width * factor);
  if (nat_width_p != NULL)
    *nat_width_p = roundf (nat_width * factor);
}

static void
shell_workspace_dot_get_preferred_height (ClutterActor *actor,
                                          float         for_width,
                                          float        *min_height_p,
                                          float        *nat_height_p)
{
  ShellWorkspaceDot *self = SHELL_WORKSPACE_DOT (actor);
  float min_height = 0, nat_height = 0;

  if (self->dot != NULL)
    clutter_actor_get_preferred_height (self->dot, for_width, &min_height, &nat_height);

  if (min_height_p != NULL)
    *min_height_p = min_height;
  if (nat_height_p != NULL)
    *nat_height_p = nat_height;
}

static void
shell_workspace_dot_allocate (ClutterActor          *actor,
                              const ClutterActorBox *box)
{
  ShellWorkspaceDot *self = SHELL_WORKSPACE_DOT (actor);
  ClutterActorBox child_box = *box;

  clutter_actor_set_allocation (actor, box);

  clutter_actor_box_set_origin (&child_box, 0, 0);
  if (self->dot != NULL)
    clutter_actor_allocate (self->dot, &child_box);
}

static void
shell_workspace_dot_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellWorkspaceDot *self = SHELL_WORKSPACE_DOT (object);

  switch (prop_id)
    {
    case DOT_PROP_EXPANSION:
      g_value_set_double (value, self->expansion);
      break;

    case DOT_PROP_WIDTH_MULTIPLIER:
      g_value_set_double (value, self->width_multiplier);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_workspace_dot_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  ShellWorkspaceDot *self = SHELL_WORKSPACE_DOT (object);

  switch (prop_id)
    {
    case DOT_PROP_EXPANSION:
      {
        double expansion = g_value_get_double (value);

        if (self->expansion != expansion)
          {
            self->expansion = expansion;
            g_object_notify_by_pspec (object, pspec);
            workspace_dot_update_visuals (self);
            clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
          }
      }
      break;

    case DOT_PROP_WIDTH_MULTIPLIER:
      {
        double width_multiplier = g_value_get_double (value);

        if (self->width_multiplier != width_multiplier)
          {
            self->width_multiplier = width_multiplier;
            g_object_notify_by_pspec (object, pspec);
            clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
          }
      }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_workspace_dot_constructed (GObject *object)
{
  ShellWorkspaceDot *self = SHELL_WORKSPACE_DOT (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *dot;

  G_OBJECT_CLASS (shell_workspace_dot_parent_class)->constructed (object);

  clutter_actor_set_pivot_point (actor, 0.5, 0.5);

  dot = g_object_new (ST_TYPE_WIDGET,
                      "style-class", "workspace-dot",
                      "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                      "request-mode", CLUTTER_REQUEST_WIDTH_FOR_HEIGHT,
                      NULL);
  clutter_actor_set_pivot_point (dot, 0.5, 0.5);
  clutter_actor_add_child (actor, dot);
  g_set_weak_pointer (&self->dot, dot);

  workspace_dot_update_visuals (self);
}

static void
shell_workspace_dot_dispose (GObject *object)
{
  ShellWorkspaceDot *self = SHELL_WORKSPACE_DOT (object);

  g_clear_weak_pointer (&self->dot);

  G_OBJECT_CLASS (shell_workspace_dot_parent_class)->dispose (object);
}

static void
shell_workspace_dot_class_init (ShellWorkspaceDotClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_workspace_dot_constructed;
  object_class->dispose = shell_workspace_dot_dispose;
  object_class->get_property = shell_workspace_dot_get_property;
  object_class->set_property = shell_workspace_dot_set_property;

  actor_class->get_preferred_width = shell_workspace_dot_get_preferred_width;
  actor_class->get_preferred_height = shell_workspace_dot_get_preferred_height;
  actor_class->allocate = shell_workspace_dot_allocate;

  dot_props[DOT_PROP_EXPANSION] =
    g_param_spec_double ("expansion", NULL, NULL,
                         0.0, 1.0, 0.0,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  dot_props[DOT_PROP_WIDTH_MULTIPLIER] =
    g_param_spec_double ("width-multiplier", NULL, NULL,
                         1.0, 10.0, 1.0,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, DOT_N_PROPS, dot_props);
}

static void
shell_workspace_dot_init (ShellWorkspaceDot *self)
{
  self->expansion = 0.0;
  self->width_multiplier = 1.0;
}

/* scaleIn () */
static void
workspace_dot_scale_in (ShellWorkspaceDot *self)
{
  ShellEaseParams params = {
    .duration = 500,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_CUBIC,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  clutter_actor_set_scale (CLUTTER_ACTOR (self), 0, 0);

  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    NULL);
}

static void
workspace_dot_scale_out_done_cb (gboolean finished,
                                 gpointer user_data)
{
  ClutterActor *actor = user_data;

  if (finished)
    clutter_actor_destroy (actor);
}

/* scaleOutAndDestroy () */
static void
workspace_dot_scale_out_and_destroy (ShellWorkspaceDot *self)
{
  ShellEaseParams params = {
    .duration = 500,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_CUBIC,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  self->destroying = TRUE;

  shell_actor_ease (CLUTTER_ACTOR (self), &params,
                    workspace_dot_scale_out_done_cb,
                    g_object_ref (self), g_object_unref,
                    "scale-x", 0.0,
                    "scale-y", 0.0,
                    NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellWorkspaceIndicators                                                 */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WORKSPACE_INDICATORS (shell_workspace_indicators_get_type ())
G_DECLARE_FINAL_TYPE (ShellWorkspaceIndicators, shell_workspace_indicators,
                      SHELL, WORKSPACE_INDICATORS, StBoxLayout)

struct _ShellWorkspaceIndicators
{
  StBoxLayout parent;

  StAdjustment *workspaces_adjustment;     /* strong */
};

G_DEFINE_FINAL_TYPE (ShellWorkspaceIndicators, shell_workspace_indicators,
                     ST_TYPE_BOX_LAYOUT)

/* _getActiveIndicators () */
static GPtrArray *
workspace_indicators_get_active (ShellWorkspaceIndicators *self)
{
  GPtrArray *active = g_ptr_array_new ();
  ClutterActor *child;

  for (child = clutter_actor_get_first_child (CLUTTER_ACTOR (self));
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      if (SHELL_IS_WORKSPACE_DOT (child) &&
          !SHELL_WORKSPACE_DOT (child)->destroying)
        g_ptr_array_add (active, child);
    }

  return active;
}

/* _updateExpansion () */
static void
workspace_indicators_update_expansion (ShellWorkspaceIndicators *self)
{
  g_autoptr (GPtrArray) active = workspace_indicators_get_active (self);
  double active_workspace = st_adjustment_get_value (self->workspaces_adjustment);
  double width_multiplier;
  ClutterActor *child;
  int index = 0;

  if (active->len <= 2)
    width_multiplier = 3.625;
  else if (active->len <= 5)
    width_multiplier = 3.25;
  else
    width_multiplier = 2.75;

  for (child = clutter_actor_get_first_child (CLUTTER_ACTOR (self));
       child != NULL;
       child = clutter_actor_get_next_sibling (child), index++)
    {
      double distance = fabs (index - active_workspace);

      g_object_set (child,
                    "expansion", CLAMP (1 - distance, 0, 1),
                    "width-multiplier", width_multiplier,
                    NULL);
    }
}

/* _recalculateDots () */
static void
workspace_indicators_recalculate_dots (ShellWorkspaceIndicators *self)
{
  g_autoptr (GPtrArray) active = workspace_indicators_get_active (self);
  int n_indicators = active->len;
  int target_indicators = (int) st_adjustment_get_upper (self->workspaces_adjustment);
  int remaining = abs (n_indicators - target_indicators);

  while (remaining--)
    {
      if (n_indicators < target_indicators)
        {
          ClutterActor *indicator = g_object_new (SHELL_TYPE_WORKSPACE_DOT, NULL);

          clutter_actor_add_child (CLUTTER_ACTOR (self), indicator);
          workspace_dot_scale_in (SHELL_WORKSPACE_DOT (indicator));
        }
      else
        {
          ClutterActor *indicator = g_ptr_array_index (active, n_indicators - remaining - 1);

          workspace_dot_scale_out_and_destroy (SHELL_WORKSPACE_DOT (indicator));
        }
    }

  workspace_indicators_update_expansion (self);
}

static void
workspaces_value_changed_cb (StAdjustment             *adjustment,
                             GParamSpec               *pspec,
                             ShellWorkspaceIndicators *self)
{
  workspace_indicators_update_expansion (self);
}

static void
workspaces_upper_changed_cb (StAdjustment             *adjustment,
                             GParamSpec               *pspec,
                             ShellWorkspaceIndicators *self)
{
  workspace_indicators_recalculate_dots (self);
}

static void
shell_workspace_indicators_constructed (GObject *object)
{
  ShellWorkspaceIndicators *self = SHELL_WORKSPACE_INDICATORS (object);
  ShellWindowManager *window_manager = shell_main_get_window_manager (shell_main_get_default ());
  int i, upper;

  G_OBJECT_CLASS (shell_workspace_indicators_parent_class)->constructed (object);

  if (window_manager == NULL)
    return;

  /* `Main.createWorkspacesAdjustment (this)` */
  self->workspaces_adjustment =
    shell_window_manager_create_workspaces_adjustment (window_manager,
                                                       CLUTTER_ACTOR (self));

  g_signal_connect_object (self->workspaces_adjustment, "notify::value",
                           G_CALLBACK (workspaces_value_changed_cb), self, 0);
  g_signal_connect_object (self->workspaces_adjustment, "notify::upper",
                           G_CALLBACK (workspaces_upper_changed_cb), self, 0);

  upper = (int) st_adjustment_get_upper (self->workspaces_adjustment);
  for (i = 0; i < upper; i++)
    clutter_actor_insert_child_at_index (CLUTTER_ACTOR (self),
                                         g_object_new (SHELL_TYPE_WORKSPACE_DOT, NULL),
                                         i);
  workspace_indicators_update_expansion (self);
}

static void
shell_workspace_indicators_dispose (GObject *object)
{
  ShellWorkspaceIndicators *self = SHELL_WORKSPACE_INDICATORS (object);

  g_clear_object (&self->workspaces_adjustment);

  G_OBJECT_CLASS (shell_workspace_indicators_parent_class)->dispose (object);
}

static void
shell_workspace_indicators_class_init (ShellWorkspaceIndicatorsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_workspace_indicators_constructed;
  object_class->dispose = shell_workspace_indicators_dispose;
}

static void
shell_workspace_indicators_init (ShellWorkspaceIndicators *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellActivitiesButton                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_ACTIVITIES_BUTTON (shell_activities_button_get_type ())
G_DECLARE_FINAL_TYPE (ShellActivitiesButton, shell_activities_button,
                      SHELL, ACTIVITIES_BUTTON, ShellPanelMenuButton)

struct _ShellActivitiesButton
{
  ShellPanelMenuButton parent;

  guint xdnd_timeout_id;
};

static ShellDragMotionResult activities_button_handle_drag_over (ShellDragTarget *target,
                                                                 GObject         *source,
                                                                 ClutterActor    *drag_actor,
                                                                 float            x,
                                                                 float            y,
                                                                 uint32_t         time);

static void
activities_button_drag_target_init (ShellDragTargetInterface *iface)
{
  iface->handle_drag_over = activities_button_handle_drag_over;
}

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellActivitiesButton, shell_activities_button,
                               SHELL_TYPE_PANEL_MENU_BUTTON,
                               G_IMPLEMENT_INTERFACE (SHELL_TYPE_DRAG_TARGET,
                                                      activities_button_drag_target_init))

static void
activities_button_toggle_overview (void)
{
  shell_overview_toggle_for_reason (shell_main_get_overview (shell_main_get_default ()),
                                    "activities-button");
}

static gboolean
activities_button_should_toggle (void)
{
  ShellOverview *overview = shell_main_get_overview (shell_main_get_default ());

  return overview != NULL && shell_overview_should_toggle_by_corner_or_button (overview);
}

/* _toggleAction () */
static gboolean
activities_button_toggle_action (ShellActivitiesButton *self)
{
  if (activities_button_should_toggle ())
    {
      activities_button_toggle_overview ();
      return CLUTTER_EVENT_STOP;
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static gboolean
activities_button_binding_toggle_cb (GObject             *object,
                                     const char          *action_name,
                                     guint                key_val,
                                     ClutterModifierType  modifiers,
                                     gpointer             user_data)
{
  return activities_button_toggle_action (SHELL_ACTIVITIES_BUTTON (object));
}

/* _xdndToggleOverview () */
static void
activities_button_xdnd_toggle_overview (gpointer user_data)
{
  ShellActivitiesButton *self = user_data;
  ShellGlobal *global = shell_global_get ();
  ClutterActor *picked_actor;
  int x, y;

  self->xdnd_timeout_id = 0;

  shell_global_get_pointer (global, &x, &y, NULL);
  picked_actor = clutter_stage_get_actor_at_pos (shell_global_get_stage (global),
                                                 CLUTTER_PICK_REACTIVE, x, y);

  if (picked_actor == CLUTTER_ACTOR (self) && activities_button_should_toggle ())
    activities_button_toggle_overview ();
}

static ShellDragMotionResult
activities_button_handle_drag_over (ShellDragTarget *target,
                                    GObject         *source,
                                    ClutterActor    *drag_actor,
                                    float            x,
                                    float            y,
                                    uint32_t         time)
{
  ShellActivitiesButton *self = SHELL_ACTIVITIES_BUTTON (target);
  ShellMain *shell_main = shell_main_get_default ();

  if (source != (GObject *) shell_main_get_xdnd_handler (shell_main))
    return SHELL_DRAG_MOTION_RESULT_CONTINUE;

  g_clear_handle_id (&self->xdnd_timeout_id, g_source_remove);
  self->xdnd_timeout_id = g_timeout_add_once (BUTTON_DND_ACTIVATION_TIMEOUT,
                                              activities_button_xdnd_toggle_overview,
                                              self);
  g_source_set_name_by_id (self->xdnd_timeout_id,
                           "[gnome-shell] activities_button_xdnd_toggle_overview");

  return SHELL_DRAG_MOTION_RESULT_CONTINUE;
}

static gboolean
shell_activities_button_scroll_event (ClutterActor *actor,
                                      ClutterEvent *event)
{
  ShellWindowManager *window_manager = shell_main_get_window_manager (shell_main_get_default ());

  if (window_manager == NULL)
    return CLUTTER_EVENT_PROPAGATE;

  return shell_window_manager_handle_workspace_scroll (window_manager, event);
}

static void
activities_button_overview_showing_cb (ShellOverview          *overview,
                                       ShellActivitiesButton  *self)
{
  st_widget_add_style_pseudo_class (ST_WIDGET (self), "checked");
}

static void
activities_button_overview_hiding_cb (ShellOverview          *overview,
                                      ShellActivitiesButton  *self)
{
  st_widget_remove_style_pseudo_class (ST_WIDGET (self), "checked");
}

static void
activities_button_click_recognize_cb (ClutterGesture        *gesture,
                                      ShellActivitiesButton *self)
{
  if (activities_button_should_toggle ())
    activities_button_toggle_overview ();
}

static void
shell_activities_button_constructed (GObject *object)
{
  ShellActivitiesButton *self = SHELL_ACTIVITIES_BUTTON (object);
  ShellOverview *overview = shell_main_get_overview (shell_main_get_default ());
  ClutterAction *click_gesture;

  G_OBJECT_CLASS (shell_activities_button_parent_class)->constructed (object);

  g_object_set (self,
                "name", "panelActivities",
                "accessible-role", ATK_ROLE_TOGGLE_BUTTON,
                /* Translators: If there is no suitable word for "Activities"
                   in your language, you can use the word for "Overview". */
                "accessible-name", _("Activities"),
                NULL);

  clutter_actor_add_child (CLUTTER_ACTOR (self),
                           g_object_new (SHELL_TYPE_WORKSPACE_INDICATORS, NULL));

  if (overview != NULL)
    {
      g_signal_connect_object (overview, "showing",
                               G_CALLBACK (activities_button_overview_showing_cb), self, 0);
      g_signal_connect_object (overview, "hiding",
                               G_CALLBACK (activities_button_overview_hiding_cb), self, 0);
    }

  click_gesture = CLUTTER_ACTION (clutter_click_gesture_new ());
  g_signal_connect (click_gesture, "recognize",
                    G_CALLBACK (activities_button_click_recognize_cb), self);
  clutter_actor_add_action (CLUTTER_ACTOR (self), click_gesture);
}

static void
shell_activities_button_dispose (GObject *object)
{
  ShellActivitiesButton *self = SHELL_ACTIVITIES_BUTTON (object);

  g_clear_handle_id (&self->xdnd_timeout_id, g_source_remove);

  G_OBJECT_CLASS (shell_activities_button_parent_class)->dispose (object);
}

static void
shell_activities_button_class_init (ShellActivitiesButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  ClutterBindingPool *binding_pool;

  object_class->constructed = shell_activities_button_constructed;
  object_class->dispose = shell_activities_button_dispose;

  actor_class->scroll_event = shell_activities_button_scroll_event;

  binding_pool = clutter_binding_pool_get_for_class (klass);

  clutter_binding_pool_install_action (binding_pool, "toggle",
                                       CLUTTER_KEY_Return, CLUTTER_RELEASE_MASK,
                                       G_CALLBACK (activities_button_binding_toggle_cb),
                                       NULL, NULL);
  clutter_binding_pool_install_action (binding_pool, "toggle",
                                       CLUTTER_KEY_space, CLUTTER_RELEASE_MASK,
                                       G_CALLBACK (activities_button_binding_toggle_cb),
                                       NULL, NULL);
}

static void
shell_activities_button_init (ShellActivitiesButton *self)
{
}

static ShellPanelButtonBox *
activities_button_new (void)
{
  return g_object_new (SHELL_TYPE_ACTIVITIES_BUTTON,
                       "menu-alignment", 0.0,
                       "dont-create-menu", TRUE,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellUnsafeModeIndicator                                                 */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_UNSAFE_MODE_INDICATOR (shell_unsafe_mode_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellUnsafeModeIndicator, shell_unsafe_mode_indicator,
                      SHELL, UNSAFE_MODE_INDICATOR, ShellSystemIndicator)

struct _ShellUnsafeModeIndicator
{
  ShellSystemIndicator parent;
};

G_DEFINE_FINAL_TYPE (ShellUnsafeModeIndicator, shell_unsafe_mode_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
shell_unsafe_mode_indicator_constructed (GObject *object)
{
  ShellUnsafeModeIndicator *self = SHELL_UNSAFE_MODE_INDICATOR (object);
  StIcon *icon;

  G_OBJECT_CLASS (shell_unsafe_mode_indicator_parent_class)->constructed (object);

  icon = shell_system_indicator_add_indicator (SHELL_SYSTEM_INDICATOR (self));
  st_icon_set_icon_name (icon, "channel-insecure-symbolic");

  g_object_bind_property (G_OBJECT (shell_global_get_context (shell_global_get ())),
                          "unsafe-mode",
                          icon, "visible",
                          G_BINDING_SYNC_CREATE);
}

static void
shell_unsafe_mode_indicator_class_init (ShellUnsafeModeIndicatorClass *klass)
{
  G_OBJECT_CLASS (klass)->constructed = shell_unsafe_mode_indicator_constructed;
}

static void
shell_unsafe_mode_indicator_init (ShellUnsafeModeIndicator *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellQuickSettingsButton                                                 */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_QUICK_SETTINGS_BUTTON (shell_quick_settings_button_get_type ())
G_DECLARE_FINAL_TYPE (ShellQuickSettingsButton, shell_quick_settings_button,
                      SHELL, QUICK_SETTINGS_BUTTON, ShellPanelMenuButton)

struct _ShellQuickSettingsButton
{
  ShellPanelMenuButton parent;

  StBoxLayout *indicators;                 /* weak */

  ShellSystemIndicator *brightness;        /* weak, the first non-privacy indicator */
  ShellSystemIndicator *background_apps;   /* weak */

  GPtrArray *ext_items;                    /* ExtensionEntry: the items of extensions in the grid */
  GPtrArray *ext_indicators;               /* ExtensionEntry: the icons of extensions */
  gboolean tracks_enabled_extensions;
};

G_DEFINE_FINAL_TYPE (ShellQuickSettingsButton, shell_quick_settings_button,
                     SHELL_TYPE_PANEL_MENU_BUTTON)

static ShellQuickSettingsMenu *
quick_settings_button_get_menu (ShellQuickSettingsButton *self)
{
  return SHELL_QUICK_SETTINGS_MENU (shell_panel_menu_button_get_menu (SHELL_PANEL_MENU_BUTTON (self)));
}

/* _addItemsBefore () */
static void
quick_settings_button_add_items_before (ShellQuickSettingsButton *self,
                                        ShellSystemIndicator     *indicator,
                                        ClutterActor             *sibling,
                                        int                       col_span)
{
  ShellQuickSettingsMenu *menu = quick_settings_button_get_menu (self);
  GPtrArray *items = shell_system_indicator_get_quick_settings_items (indicator);
  guint i;

  for (i = 0; i < items->len; i++)
    shell_quick_settings_menu_insert_item_before (menu, g_ptr_array_index (items, i),
                                                  sibling, col_span);
}

static void
quick_settings_network_screenshot_requested_cb (GObject *source,
                                                gpointer user_data)
{
  ShellScreenshotUI *screenshot_ui = shell_main_get_screenshot_ui (shell_main_get_default ());

  if (screenshot_ui != NULL)
    shell_screenshot_ui_open (screenshot_ui, SHELL_SCREENSHOT_UI_MODE_SCREENSHOT);
}

/* _setupIndicators () */
static void
quick_settings_button_setup_indicators (ShellQuickSettingsButton *self)
{
  ShellMain *shell_main = shell_main_get_default ();
  ShellQuickSettingsMenu *menu = quick_settings_button_get_menu (self);
  ClutterActor *indicators = CLUTTER_ACTOR (self->indicators);
  ShellScreenshotUI *screenshot_ui = shell_main_get_screenshot_ui (shell_main);
  ShellSystemIndicator *network = NULL;
  ShellSystemIndicator *bluetooth = NULL;
  ShellSystemIndicator *system;
  ShellSystemIndicator *camera;
  ShellSystemIndicator *volume_output;
  ShellSystemIndicator *volume_input;
  ShellSystemIndicator *brightness;
  ShellSystemIndicator *remote_access;
  ShellSystemIndicator *location;
  ShellSystemIndicator *thunderbolt;
  ShellSystemIndicator *night_light;
  ShellSystemIndicator *dark_mode;
  ShellSystemIndicator *do_not_disturb;
  ShellSystemIndicator *backlight;
  ShellSystemIndicator *power_profiles;
  ShellSystemIndicator *rfkill;
  ShellSystemIndicator *auto_rotate;
  ShellSystemIndicator *unsafe_mode;
  ShellSystemIndicator *background_apps;
  ClutterActor *sibling;
  GPtrArray *background_items;
  int pos = 0;
  guint i;

#ifdef HAVE_NETWORKMANAGER
  network = SHELL_SYSTEM_INDICATOR (shell_network_indicator_new ());
  g_signal_connect (network, "overview-hide-requested",
                    G_CALLBACK (overview_hide_requested_cb), NULL);
#endif

#ifdef HAVE_BLUETOOTH
  bluetooth = SHELL_SYSTEM_INDICATOR (shell_bluetooth_indicator_new ());
#endif

  system = SHELL_SYSTEM_INDICATOR (shell_system_status_indicator_new ());
  g_signal_connect (system, "screenshot-ui-requested",
                    G_CALLBACK (quick_settings_network_screenshot_requested_cb), NULL);
  camera = SHELL_SYSTEM_INDICATOR (shell_camera_indicator_new ());
  volume_output = SHELL_SYSTEM_INDICATOR (shell_volume_output_indicator_new ());
  volume_input = SHELL_SYSTEM_INDICATOR (shell_volume_input_indicator_new ());
  brightness = SHELL_SYSTEM_INDICATOR (shell_brightness_indicator_new (shell_main_get_brightness_manager (shell_main)));
  remote_access = SHELL_SYSTEM_INDICATOR (shell_remote_access_applet_new ());
  if (screenshot_ui != NULL)
    g_object_bind_property (screenshot_ui, "screencast-in-progress",
                            remote_access, "screencast-in-progress",
                            G_BINDING_SYNC_CREATE);
  location = SHELL_SYSTEM_INDICATOR (shell_location_indicator_new ());
  thunderbolt = SHELL_SYSTEM_INDICATOR (shell_thunderbolt_indicator_new ());
  night_light = SHELL_SYSTEM_INDICATOR (shell_night_light_indicator_new ());
  dark_mode = SHELL_SYSTEM_INDICATOR (shell_dark_mode_indicator_new ());
  do_not_disturb = SHELL_SYSTEM_INDICATOR (shell_do_not_disturb_indicator_new ());
  backlight = SHELL_SYSTEM_INDICATOR (shell_backlight_indicator_new ());
  power_profiles = SHELL_SYSTEM_INDICATOR (shell_power_profiles_indicator_new ());
  rfkill = SHELL_SYSTEM_INDICATOR (shell_rfkill_indicator_new ());
  auto_rotate = SHELL_SYSTEM_INDICATOR (shell_auto_rotate_indicator_new ());
  unsafe_mode = g_object_new (SHELL_TYPE_UNSAFE_MODE_INDICATOR, NULL);
  background_apps = SHELL_SYSTEM_INDICATOR (shell_background_apps_indicator_new ());
  g_signal_connect (background_apps, "overview-hide-requested",
                    G_CALLBACK (overview_hide_requested_cb), NULL);

  g_set_weak_pointer (&self->brightness, brightness);
  g_set_weak_pointer (&self->background_apps, background_apps);

  /* add privacy-related indicators before any external indicators */
  clutter_actor_insert_child_at_index (indicators, CLUTTER_ACTOR (remote_access), pos++);
  clutter_actor_insert_child_at_index (indicators, CLUTTER_ACTOR (camera), pos++);
  clutter_actor_insert_child_at_index (indicators, CLUTTER_ACTOR (volume_input), pos++);
  clutter_actor_insert_child_at_index (indicators, CLUTTER_ACTOR (location), pos++);

  /* append all other indicators */
  clutter_actor_add_child (indicators, CLUTTER_ACTOR (brightness));
  clutter_actor_add_child (indicators, CLUTTER_ACTOR (thunderbolt));
  clutter_actor_add_child (indicators, CLUTTER_ACTOR (night_light));
  if (network != NULL)
    clutter_actor_add_child (indicators, CLUTTER_ACTOR (network));
  clutter_actor_add_child (indicators, CLUTTER_ACTOR (dark_mode));
  clutter_actor_add_child (indicators, CLUTTER_ACTOR (do_not_disturb));
  clutter_actor_add_child (indicators, CLUTTER_ACTOR (backlight));
  if (bluetooth != NULL)
    clutter_actor_add_child (indicators, CLUTTER_ACTOR (bluetooth));
  clutter_actor_add_child (indicators, CLUTTER_ACTOR (rfkill));
  clutter_actor_add_child (indicators, CLUTTER_ACTOR (auto_rotate));
  clutter_actor_add_child (indicators, CLUTTER_ACTOR (volume_output));
  clutter_actor_add_child (indicators, CLUTTER_ACTOR (unsafe_mode));
  clutter_actor_add_child (indicators, CLUTTER_ACTOR (power_profiles));
  clutter_actor_add_child (indicators, CLUTTER_ACTOR (system));

  /* add our quick settings items before any external ones */
  sibling = shell_quick_settings_menu_get_first_item (menu);
  quick_settings_button_add_items_before (self, system, sibling, N_QUICK_SETTINGS_COLUMNS);
  quick_settings_button_add_items_before (self, volume_output, sibling, N_QUICK_SETTINGS_COLUMNS);
  quick_settings_button_add_items_before (self, volume_input, sibling, N_QUICK_SETTINGS_COLUMNS);
  quick_settings_button_add_items_before (self, brightness, sibling, N_QUICK_SETTINGS_COLUMNS);

  quick_settings_button_add_items_before (self, camera, sibling, 1);
  quick_settings_button_add_items_before (self, remote_access, sibling, 1);
  quick_settings_button_add_items_before (self, thunderbolt, sibling, 1);
  quick_settings_button_add_items_before (self, location, sibling, 1);
  if (network != NULL)
    quick_settings_button_add_items_before (self, network, sibling, 1);
  if (bluetooth != NULL)
    quick_settings_button_add_items_before (self, bluetooth, sibling, 1);
  quick_settings_button_add_items_before (self, power_profiles, sibling, 1);
  quick_settings_button_add_items_before (self, night_light, sibling, 1);
  quick_settings_button_add_items_before (self, dark_mode, sibling, 1);
  quick_settings_button_add_items_before (self, do_not_disturb, sibling, 1);
  quick_settings_button_add_items_before (self, backlight, sibling, 1);
  quick_settings_button_add_items_before (self, rfkill, sibling, 1);
  quick_settings_button_add_items_before (self, auto_rotate, sibling, 1);
  quick_settings_button_add_items_before (self, unsafe_mode, sibling, 1);

  /* append background apps */
  background_items = shell_system_indicator_get_quick_settings_items (background_apps);
  for (i = 0; i < background_items->len; i++)
    shell_quick_settings_menu_add_item (menu, g_ptr_array_index (background_items, i),
                                        N_QUICK_SETTINGS_COLUMNS);
}

static void
shell_quick_settings_button_constructed (GObject *object)
{
  ShellQuickSettingsButton *self = SHELL_QUICK_SETTINGS_BUTTON (object);
  ClutterActor *indicators;
  g_autoptr (ShellQuickSettingsMenu) menu = NULL;

  G_OBJECT_CLASS (shell_quick_settings_button_parent_class)->constructed (object);

  indicators = g_object_new (ST_TYPE_BOX_LAYOUT,
                             "style-class", "panel-status-indicators-box",
                             NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), indicators);
  g_set_weak_pointer (&self->indicators, ST_BOX_LAYOUT (indicators));

  menu = shell_quick_settings_menu_new (CLUTTER_ACTOR (self), N_QUICK_SETTINGS_COLUMNS);
  shell_panel_menu_button_set_menu (SHELL_PANEL_MENU_BUTTON (self), G_OBJECT (menu));

  quick_settings_button_setup_indicators (self);
}

static void
shell_quick_settings_button_dispose (GObject *object)
{
  ShellQuickSettingsButton *self = SHELL_QUICK_SETTINGS_BUTTON (object);

  g_clear_weak_pointer (&self->indicators);
  g_clear_weak_pointer (&self->brightness);
  g_clear_weak_pointer (&self->background_apps);

  g_clear_pointer (&self->ext_items, g_ptr_array_unref);
  g_clear_pointer (&self->ext_indicators, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_quick_settings_button_parent_class)->dispose (object);
}

static void
shell_quick_settings_button_class_init (ShellQuickSettingsButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_quick_settings_button_constructed;
  object_class->dispose = shell_quick_settings_button_dispose;
}

static void
shell_quick_settings_button_init (ShellQuickSettingsButton *self)
{
}

static ShellPanelButtonBox *
quick_settings_button_new (void)
{
  return g_object_new (SHELL_TYPE_QUICK_SETTINGS_BUTTON,
                       "menu-alignment", 0.0,
                       "accessible-name", C_("System menu in the top bar", "System"),
                       "dont-create-menu", TRUE,
                       NULL);
}

/**
 * shell_panel_items_add_external_indicator:
 * @panel: the top bar
 * @indicator: the indicator to add
 * @col_span: the number of columns its quick settings items span
 *
 * Inserts an indicator and its quick settings items at appropriate positions
 * in the quick settings button of @panel
 * (`QuickSettings.addExternalIndicator ()`): the indicator before the first
 * non-privacy indicator, the items before the background apps.
 *
 * Returns: %FALSE if the panel has no quick settings button
 */
gboolean
shell_panel_items_add_external_indicator (ShellPanel           *panel,
                                          ShellSystemIndicator *indicator,
                                          int                   col_span)
{
  ShellPanelButtonBox *box;
  ShellQuickSettingsButton *self;
  ClutterActor *sibling = NULL;

  g_return_val_if_fail (SHELL_IS_PANEL (panel), FALSE);
  g_return_val_if_fail (SHELL_IS_SYSTEM_INDICATOR (indicator), FALSE);

  box = shell_panel_get_status_area_indicator (panel, "quickSettings");
  if (!SHELL_IS_QUICK_SETTINGS_BUTTON (box))
    return FALSE;

  self = SHELL_QUICK_SETTINGS_BUTTON (box);

  /* Insert before first non-privacy indicator if it exists */
  clutter_actor_insert_child_below (CLUTTER_ACTOR (self->indicators),
                                    CLUTTER_ACTOR (indicator),
                                    self->brightness != NULL ? CLUTTER_ACTOR (self->brightness) : NULL);

  /* Insert before background apps if it exists */
  if (self->background_apps != NULL)
    {
      GPtrArray *background_items =
        shell_system_indicator_get_quick_settings_items (self->background_apps);

      if (background_items->len > 0)
        sibling = g_ptr_array_index (background_items, background_items->len - 1);
    }

  quick_settings_button_add_items_before (self, indicator, sibling, col_span);

  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Items and icons of extensions                                            */
/* ------------------------------------------------------------------------ */

/*
 * The items (toggles and sliders) and the status icons that extensions add
 * to the quick settings button (docs/api/quick-settings.md). Both lists are
 * kept grouped by owner and ordered by the rank of the owner in
 * `enabled-extensions` (ui/extension-order.h), the items of one owner in the
 * order they were asked for. The items sit between the built-in toggles and
 * the background apps section, the icons between the privacy icons and the
 * first other icon of the shell, so the built-in items never mix with them.
 */

typedef struct
{
  char *owner;
  ClutterActor *actor; /* the entry goes when the actor is destroyed */
} ExtensionEntry;

static void
extension_entry_free (gpointer data)
{
  ExtensionEntry *entry = data;

  g_free (entry->owner);
  g_free (entry);
}

static const char *
extension_entry_get_owner (gpointer data)
{
  return ((ExtensionEntry *) data)->owner;
}

static gboolean
extension_entry_in_slot (gpointer entry,
                         gpointer slot_data)
{
  return TRUE;
}

static ShellQuickSettingsButton *
get_quick_settings_button (ShellPanel *panel)
{
  ShellPanelButtonBox *box;

  box = shell_panel_get_status_area_indicator (panel, "quickSettings");
  return SHELL_IS_QUICK_SETTINGS_BUTTON (box) ? SHELL_QUICK_SETTINGS_BUTTON (box) : NULL;
}

/* The child of the grid the items of the extensions go before: the end of
 * the background apps section, or NULL for the end of the grid */
static ClutterActor *
quick_settings_button_get_items_anchor (ShellQuickSettingsButton *self)
{
  if (self->background_apps != NULL)
    {
      GPtrArray *background_items =
        shell_system_indicator_get_quick_settings_items (self->background_apps);

      if (background_items->len > 0)
        return g_ptr_array_index (background_items, background_items->len - 1);
    }

  return NULL;
}

/* Puts the actors of @entries in the order of the array, each before the
 * next one and the last before @anchor. Only the children out of place move. */
static void
relayout_extension_entries (ClutterActor *parent,
                            GPtrArray    *entries,
                            ClutterActor *anchor)
{
  guint i;

  if (parent == NULL || entries == NULL)
    return;

  for (i = entries->len; i > 0; i--)
    {
      ExtensionEntry *entry = g_ptr_array_index (entries, i - 1);
      ClutterActor *sibling = i == entries->len
        ? anchor
        : ((ExtensionEntry *) g_ptr_array_index (entries, i))->actor;

      if (clutter_actor_get_parent (entry->actor) != parent)
        continue;

      if (clutter_actor_get_next_sibling (entry->actor) != sibling)
        clutter_actor_set_child_below_sibling (parent, entry->actor, sibling);
    }
}

static void
quick_settings_button_relayout_extensions (ShellQuickSettingsButton *self)
{
  ShellQuickSettingsMenu *menu = quick_settings_button_get_menu (self);

  relayout_extension_entries (shell_quick_settings_menu_get_grid (menu),
                              self->ext_items,
                              quick_settings_button_get_items_anchor (self));
  relayout_extension_entries (CLUTTER_ACTOR (self->indicators),
                              self->ext_indicators,
                              self->brightness != NULL ? CLUTTER_ACTOR (self->brightness) : NULL);
}

static void
extension_enabled_extensions_changed_cb (GSettings                *settings,
                                         const char               *key,
                                         ShellQuickSettingsButton *self)
{
  if (self->ext_items != NULL)
    shell_extension_order_sort (self->ext_items, extension_entry_get_owner);
  if (self->ext_indicators != NULL)
    shell_extension_order_sort (self->ext_indicators, extension_entry_get_owner);

  quick_settings_button_relayout_extensions (self);
}

static void
extension_actor_destroy_cb (ClutterActor             *actor,
                            ShellQuickSettingsButton *self)
{
  GPtrArray *lists[2] = { self->ext_items, self->ext_indicators };
  guint i, j;

  for (i = 0; i < G_N_ELEMENTS (lists); i++)
    {
      if (lists[i] == NULL)
        continue;

      for (j = 0; j < lists[i]->len; j++)
        {
          ExtensionEntry *entry = g_ptr_array_index (lists[i], j);

          if (entry->actor == actor)
            {
              g_ptr_array_remove_index (lists[i], j);
              return;
            }
        }
    }
}

static void
quick_settings_button_prepare_extensions (ShellQuickSettingsButton *self)
{
  if (self->ext_items == NULL)
    self->ext_items = g_ptr_array_new_with_free_func (extension_entry_free);
  if (self->ext_indicators == NULL)
    self->ext_indicators = g_ptr_array_new_with_free_func (extension_entry_free);

  if (!self->tracks_enabled_extensions)
    {
      self->tracks_enabled_extensions = TRUE;
      g_signal_connect_object (shell_global_get_settings (shell_global_get ()),
                               "changed::enabled-extensions",
                               G_CALLBACK (extension_enabled_extensions_changed_cb),
                               self, 0);
    }
}

/* Registers @actor in @entries at the place the rules of @owner and @index
 * give, and returns the actor it has to be inserted before */
static ClutterActor *
quick_settings_button_register_extension_actor (ShellQuickSettingsButton *self,
                                                GPtrArray                *entries,
                                                ClutterActor             *actor,
                                                const char               *owner,
                                                int                       index,
                                                ClutterActor             *anchor)
{
  ExtensionEntry *entry = g_new0 (ExtensionEntry, 1);
  ClutterActor *sibling = anchor;
  guint position;

  position = shell_extension_order_insert_position (entries,
                                                    extension_entry_get_owner,
                                                    extension_entry_in_slot, NULL,
                                                    owner, index);
  position = MIN (position, entries->len);

  if (position < entries->len)
    sibling = ((ExtensionEntry *) g_ptr_array_index (entries, position))->actor;

  entry->owner = g_strdup (owner);
  entry->actor = actor;
  g_ptr_array_insert (entries, position, entry);
  g_signal_connect_object (actor, "destroy",
                           G_CALLBACK (extension_actor_destroy_cb), self, 0);

  return sibling;
}

/**
 * shell_panel_items_get_quick_settings_menu:
 * @panel: the top bar
 *
 * Returns: (transfer none) (nullable): the quick settings menu of the top bar,
 *   or %NULL if the panel has no quick settings button (the session mode
 *   does not list it)
 */
ShellQuickSettingsMenu *
shell_panel_items_get_quick_settings_menu (ShellPanel *panel)
{
  ShellQuickSettingsButton *self;

  g_return_val_if_fail (SHELL_IS_PANEL (panel), NULL);

  self = get_quick_settings_button (panel);
  return self != NULL ? quick_settings_button_get_menu (self) : NULL;
}

/**
 * shell_panel_items_add_extension_item:
 * @panel: the top bar
 * @item: (transfer none): a toggle, menu toggle or slider that has no parent
 * @col_span: the number of columns it spans
 * @owner: the uuid of the extension that adds it
 * @index: the 1-based position among the items of @owner, or 0 or less to go
 *   after the last one; values past the end clamp
 * @error: return location for a #GError
 *
 * Adds the item of an extension to the grid of the quick settings menu: after
 * the items of the shell and before the background apps, among the items of
 * the extensions ordered by the position of their extension in
 * `enabled-extensions` and then by @index. The item leaves its place when it
 * is destroyed.
 *
 * Returns: %FALSE with %G_IO_ERROR_NOT_FOUND if the panel has no quick
 *   settings button
 */
gboolean
shell_panel_items_add_extension_item (ShellPanel   *panel,
                                      ClutterActor *item,
                                      int           col_span,
                                      const char   *owner,
                                      int           index,
                                      GError      **error)
{
  ShellQuickSettingsButton *self;
  ClutterActor *sibling;

  g_return_val_if_fail (SHELL_IS_PANEL (panel), FALSE);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (item), FALSE);
  g_return_val_if_fail (clutter_actor_get_parent (item) == NULL, FALSE);
  g_return_val_if_fail (owner != NULL && owner[0] != '\0', FALSE);

  self = get_quick_settings_button (panel);
  if (self == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "The top bar has no quick settings menu in this session mode");
      return FALSE;
    }

  quick_settings_button_prepare_extensions (self);
  sibling = quick_settings_button_register_extension_actor (self, self->ext_items,
                                                            item, owner, index,
                                                            quick_settings_button_get_items_anchor (self));
  shell_quick_settings_menu_insert_item_before (quick_settings_button_get_menu (self),
                                                item, sibling,
                                                CLAMP (col_span, 1, N_QUICK_SETTINGS_COLUMNS));
  return TRUE;
}

/**
 * shell_panel_items_add_extension_indicator:
 * @panel: the top bar
 * @icon: (transfer none): a status icon that has no parent
 * @owner: the uuid of the extension that adds it
 * @index: the 1-based position among the icons of @owner, or 0 or less to go
 *   after the last one; values past the end clamp
 * @error: return location for a #GError
 *
 * Adds the status icon of an extension to the system area of the top bar,
 * after the privacy icons and before the first other icon of the shell, among
 * the icons of the extensions ordered like the items of
 * shell_panel_items_add_extension_item (). The icon leaves its place when it
 * is destroyed.
 *
 * Returns: %FALSE with %G_IO_ERROR_NOT_FOUND if the panel has no quick
 *   settings button
 */
gboolean
shell_panel_items_add_extension_indicator (ShellPanel   *panel,
                                           ClutterActor *icon,
                                           const char   *owner,
                                           int           index,
                                           GError      **error)
{
  ShellQuickSettingsButton *self;
  ClutterActor *sibling;

  g_return_val_if_fail (SHELL_IS_PANEL (panel), FALSE);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (icon), FALSE);
  g_return_val_if_fail (clutter_actor_get_parent (icon) == NULL, FALSE);
  g_return_val_if_fail (owner != NULL && owner[0] != '\0', FALSE);

  self = get_quick_settings_button (panel);
  if (self == NULL || self->indicators == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "The top bar has no quick settings menu in this session mode");
      return FALSE;
    }

  quick_settings_button_prepare_extensions (self);
  sibling = quick_settings_button_register_extension_actor (self, self->ext_indicators,
                                                            icon, owner, index,
                                                            self->brightness != NULL
                                                              ? CLUTTER_ACTOR (self->brightness)
                                                              : NULL);
  clutter_actor_insert_child_below (CLUTTER_ACTOR (self->indicators), icon, sibling);
  return TRUE;
}

/**
 * shell_panel_items_get_extension_index:
 * @panel: the top bar
 * @actor: an item or an icon added by an extension
 *
 * Returns: the 1-based position of @actor among the items (or icons) of its
 *   owner, or 0 if it is not one added by an extension
 */
int
shell_panel_items_get_extension_index (ShellPanel   *panel,
                                       ClutterActor *actor)
{
  ShellQuickSettingsButton *self;
  GPtrArray *lists[2];
  guint i, j;

  g_return_val_if_fail (SHELL_IS_PANEL (panel), 0);

  self = get_quick_settings_button (panel);
  if (self == NULL)
    return 0;

  lists[0] = self->ext_items;
  lists[1] = self->ext_indicators;

  for (i = 0; i < G_N_ELEMENTS (lists); i++)
    {
      const char *owner = NULL;
      int position = 0;

      if (lists[i] == NULL)
        continue;

      for (j = 0; j < lists[i]->len; j++)
        {
          ExtensionEntry *entry = g_ptr_array_index (lists[i], j);

          if (entry->actor == actor)
            {
              owner = entry->owner;
              break;
            }
        }

      if (owner == NULL)
        continue;

      for (j = 0; j < lists[i]->len; j++)
        {
          ExtensionEntry *entry = g_ptr_array_index (lists[i], j);

          if (strcmp (entry->owner, owner) != 0)
            continue;

          position++;
          if (entry->actor == actor)
            return position;
        }
    }

  return 0;
}

/* ------------------------------------------------------------------------ */
/* PANEL_ITEM_IMPLEMENTATIONS                                               */
/* ------------------------------------------------------------------------ */

static void
screen_recording_stop_requested_cb (GObject *source,
                                    gpointer user_data)
{
  ShellScreenshotUI *screenshot_ui = shell_main_get_screenshot_ui (shell_main_get_default ());

  if (screenshot_ui != NULL)
    shell_screenshot_ui_stop_screencast (screenshot_ui);
}

/*
 * _setupScreenRecording (): the screenshot UI is not known to the screen
 * recording indicator: it shows while the screenshot UI records a screencast,
 * and a click on it stops the screencast.
 */
static void
setup_screen_recording (ShellScreenRecordingIndicator *indicator)
{
  ShellScreenshotUI *screenshot_ui = shell_main_get_screenshot_ui (shell_main_get_default ());

  if (screenshot_ui != NULL)
    g_object_bind_property (screenshot_ui, "screencast-in-progress",
                            indicator, "screencast-in-progress",
                            G_BINDING_SYNC_CREATE);

  g_signal_connect (indicator, "stop-screencast-requested",
                    G_CALLBACK (screen_recording_stop_requested_cb), NULL);
}

typedef struct
{
  const char *role;
  ShellPanelButtonBox *(* create) (void);
} PanelItemImplementation;

static ShellPanelButtonBox *
create_date_menu (void)
{
  ShellPanelButtonBox *item = SHELL_PANEL_BUTTON_BOX (shell_date_menu_button_new ());

  g_signal_connect (item, "overview-hide-requested",
                    G_CALLBACK (overview_hide_requested_cb), NULL);

  return item;
}

static ShellPanelButtonBox *
create_a11y (void)
{
  return SHELL_PANEL_BUTTON_BOX (shell_at_indicator_new ());
}

static ShellPanelButtonBox *
create_keyboard (void)
{
  ShellPanelButtonBox *item = SHELL_PANEL_BUTTON_BOX (shell_input_source_indicator_new ());

  g_signal_connect (item, "overview-hide-requested",
                    G_CALLBACK (overview_hide_requested_cb), NULL);

  return item;
}

static ShellPanelButtonBox *
create_dwell_click (void)
{
  return SHELL_PANEL_BUTTON_BOX (shell_dwell_click_indicator_new ());
}

static ShellPanelButtonBox *
create_screen_recording (void)
{
  ShellScreenRecordingIndicator *item = shell_screen_recording_indicator_new ();

  setup_screen_recording (item);

  return SHELL_PANEL_BUTTON_BOX (item);
}

static ShellPanelButtonBox *
create_screen_sharing (void)
{
  return SHELL_PANEL_BUTTON_BOX (shell_screen_sharing_indicator_new ());
}

static const PanelItemImplementation panel_item_implementations[] = {
  { "activities", activities_button_new },
  { "quickSettings", quick_settings_button_new },
  { "dateMenu", create_date_menu },
  { "a11y", create_a11y },
  { "keyboard", create_keyboard },
  { "dwellClick", create_dwell_click },
  { "screenRecording", create_screen_recording },
  { "screenSharing", create_screen_sharing },
};

/**
 * shell_panel_item_create:
 * @role: the role of a status area indicator, one of the roles that
 *   shell_panel_is_builtin_role () knows
 *
 * Creates the indicator of the shell for a status area role
 * (`createPanelItem ()`). The panel asks for it through
 * #ShellPanel::create-indicator when the session mode lists a role that has
 * no indicator yet.
 *
 * Returns: (transfer none) (nullable): the new indicator (floating), or %NULL
 *   if the role is not implemented
 */
ShellPanelButtonBox *
shell_panel_item_create (const char *role)
{
  guint i;

  g_return_val_if_fail (role != NULL, NULL);

  for (i = 0; i < G_N_ELEMENTS (panel_item_implementations); i++)
    {
      if (strcmp (panel_item_implementations[i].role, role) == 0)
        return panel_item_implementations[i].create ();
    }

  /* This icon is not implemented (this is a bug) */
  return NULL;
}
