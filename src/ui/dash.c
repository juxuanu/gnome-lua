/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dash.c: The dash of the overview: the favorite and the running
 * applications, and the button that shows the application grid. Port of
 * js/ui/dash.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `Main.overview` is reached through a #ShellWindowManager (see
 *    app-display.c): `Main.overview.visible`, `closing` and
 *    `animationInProgress` are its `overview-visible`, `overview-closing`
 *    and `overview-animating` properties, `hiding` is the change of
 *    `overview-closing` to %TRUE, and `item-drag-begin`, `item-drag-end` and
 *    `item-drag-cancelled` are its signals of the same name. The drags of
 *    windows are shell_dash_window_drag_begin () and
 *    shell_dash_window_drag_end (), which overview-controls.c calls
 *    (`window-drag-begin`, and `window-drag-cancelled` and `-end`).
 *  - `Main.ctrlAltTabManager` is the one of the window manager, and
 *    `Main.initializeDeferredWork ()` is shell_deferred_work_new ().
 *  - `Dash.iconSize` is the read-only property `icon-size` and
 *    `showAppsButton` the read-only property `show-apps-button`;
 *    `setMaxSize ()` is shell_dash_set_max_size ().
 *  - `Overview.ANIMATION_TIME` is 250 here, written as 125 for half of it.
 *  - The icons of the dash are given the icon size of the dash as their
 *    `drag-icon-size`, which they used to read from `Main.overview.dash`.
 *  - The hook `dash.item-added` is emitted for every application that is
 *    added to the dash (see docs/api/hooks.md).
 *  - The labels of the items are hooked up with the signals of the child
 *    of the item, and find their dash and item from it, where the JS closed
 *    over both.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <math.h>
#include <meta/compositor.h>
#include <meta/meta-later.h>

#include "shell-app-system.h"
#include "shell-global.h"
#include "shell-hooks.h"
#include "ui/app-favorites.h"
#include "ui/ctrl-alt-tab.h"
#include "ui/dash.h"
#include "ui/deferred-work.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"

#define DASH_ITEM_LABEL_SHOW_TIME 150
#define DASH_ITEM_LABEL_HIDE_TIME 100
#define DASH_ITEM_HOVER_TIMEOUT 300

/* Overview.ANIMATION_TIME / 2 */
#define DASH_WINDOW_DRAG_FADE_TIME 125

static const int base_icon_sizes[] = { 16, 22, 24, 32, 48, 64 };

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

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

static gboolean
is_overview_visible (ShellWindowManager *window_manager)
{
  return window_manager != NULL && shell_window_manager_get_overview_visible (window_manager);
}

static gboolean
is_overview_animating (ShellWindowManager *window_manager)
{
  return window_manager != NULL && shell_window_manager_get_overview_animating (window_manager);
}

/* Main.layoutManager */
static ShellLayoutManager *
get_layout_manager (void)
{
  return shell_main_get_layout_manager (shell_main_get_default ());
}

/**
 * shell_dash_get_app_from_source:
 * @source: (nullable): the source of a drag
 *
 * Returns: (transfer none) (nullable): the application of the icon which is
 *   dragged, or %NULL if the source is not an application icon
 *   (`Dash.getAppFromSource ()`)
 */
ShellApp *
shell_dash_get_app_from_source (GObject *source)
{
  if (SHELL_IS_APP_ICON (source))
    return shell_app_icon_get_app (SHELL_APP_ICON (source));

  return NULL;
}

/* ------------------------------------------------------------------------ */
/* ShellDashIcon                                                             */
/* ------------------------------------------------------------------------ */

struct _ShellDashIcon
{
  ShellAppIcon parent;
};

G_DEFINE_FINAL_TYPE (ShellDashIcon, shell_dash_icon, SHELL_TYPE_APP_ICON)

/* Disable scale-n-fade methods used during DND by parent */
static void
dash_icon_scale_and_fade (ShellAppViewItem *item)
{
}

static void
dash_icon_undo_scale_and_fade (ShellAppViewItem *item)
{
}

static ShellDragMotionResult
dash_icon_handle_drag_over (ShellAppViewItem *item,
                            GObject          *source,
                            ClutterActor     *actor,
                            float             x,
                            float             y,
                            uint32_t          time)
{
  return SHELL_DRAG_MOTION_RESULT_CONTINUE;
}

static gboolean
dash_icon_accept_drop (ShellAppViewItem *item,
                       GObject          *source,
                       ClutterActor     *actor,
                       float             x,
                       float             y,
                       uint32_t          time)
{
  return FALSE;
}

static void
shell_dash_icon_class_init (ShellDashIconClass *klass)
{
  ShellAppViewItemClass *item_class = SHELL_APP_VIEW_ITEM_CLASS (klass);

  item_class->scale_and_fade = dash_icon_scale_and_fade;
  item_class->undo_scale_and_fade = dash_icon_undo_scale_and_fade;
  item_class->handle_drag_over = dash_icon_handle_drag_over;
  item_class->accept_drop = dash_icon_accept_drop;
}

static void
shell_dash_icon_init (ShellDashIcon *self)
{
}

/**
 * shell_dash_icon_new:
 * @app: the application
 * @window_manager: (nullable): the window manager the overview is reached
 *   through
 *
 * Creates the icon of an application for the dash: no label, the menu opens
 * below it and the size is set by the dash (`new DashIcon (app)`).
 *
 * Returns: (transfer full): a new #ShellDashIcon
 */
ShellDashIcon *
shell_dash_icon_new (ShellApp           *app,
                     ShellWindowManager *window_manager)
{
  g_return_val_if_fail (SHELL_IS_APP (app), NULL);

  return g_object_new (SHELL_TYPE_DASH_ICON,
                       "app", app,
                       "show-label", FALSE,
                       "popup-menu-side", ST_SIDE_BOTTOM,
                       "window-manager", window_manager,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellDashItemContainer                                                    */
/* ------------------------------------------------------------------------ */

enum
{
  CONTAINER_PROP_0,
  CONTAINER_PROP_CHILD,
  CONTAINER_PROP_LABEL,
  CONTAINER_PROP_ANIMATING_OUT,
  CONTAINER_N_PROPS
};

static GParamSpec *container_props[CONTAINER_N_PROPS] = { NULL, };

typedef struct
{
  char *label_text;
  StLabel *label; /* weak */
  ClutterActor *child; /* weak */
  gboolean animating_out;
} ShellDashItemContainerPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellDashItemContainer, shell_dash_item_container, ST_TYPE_WIDGET)

/**
 * shell_dash_item_container_get_child:
 * @self: a #ShellDashItemContainer
 *
 * Returns: (transfer none) (nullable): the child (`child`)
 */
ClutterActor *
shell_dash_item_container_get_child (ShellDashItemContainer *self)
{
  ShellDashItemContainerPrivate *priv;

  g_return_val_if_fail (SHELL_IS_DASH_ITEM_CONTAINER (self), NULL);

  priv = shell_dash_item_container_get_instance_private (self);
  return priv->child;
}

/**
 * shell_dash_item_container_get_label:
 * @self: a #ShellDashItemContainer
 *
 * Returns: (transfer none) (nullable): the label which is shown above the
 *   item, %NULL once it was destroyed (`label`)
 */
StLabel *
shell_dash_item_container_get_label (ShellDashItemContainer *self)
{
  ShellDashItemContainerPrivate *priv;

  g_return_val_if_fail (SHELL_IS_DASH_ITEM_CONTAINER (self), NULL);

  priv = shell_dash_item_container_get_instance_private (self);
  return priv->label;
}

/**
 * shell_dash_item_container_get_animating_out:
 * @self: a #ShellDashItemContainer
 *
 * Returns: whether the item is animating out, and will be destroyed
 *   (`animatingOut`)
 */
gboolean
shell_dash_item_container_get_animating_out (ShellDashItemContainer *self)
{
  ShellDashItemContainerPrivate *priv;

  g_return_val_if_fail (SHELL_IS_DASH_ITEM_CONTAINER (self), FALSE);

  priv = shell_dash_item_container_get_instance_private (self);
  return priv->animating_out;
}

/**
 * shell_dash_item_container_set_child:
 * @self: a #ShellDashItemContainer
 * @child: the actor to show, which replaces the one shown so far
 *
 * Sets the child (`setChild ()`).
 */
void
shell_dash_item_container_set_child (ShellDashItemContainer *self,
                                     ClutterActor           *child)
{
  ShellDashItemContainerPrivate *priv;
  ClutterActor *new_child = child;

  g_return_if_fail (SHELL_IS_DASH_ITEM_CONTAINER (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (child));

  priv = shell_dash_item_container_get_instance_private (self);
  if (priv->child == child)
    return;

  clutter_actor_destroy_all_children (CLUTTER_ACTOR (self));

  g_clear_weak_pointer (&priv->child);
  g_set_weak_pointer (&priv->child, new_child);
  clutter_actor_set_y_expand (child, TRUE);
  clutter_actor_add_child (CLUTTER_ACTOR (self), child);

  g_object_notify_by_pspec (G_OBJECT (self), container_props[CONTAINER_PROP_CHILD]);
}

/**
 * shell_dash_item_container_set_label_text:
 * @self: a #ShellDashItemContainer
 * @text: the text of the label, which is also the accessible name of the
 *   child
 *
 * Sets the text of the label (`setLabelText ()`). The item needs a child.
 */
void
shell_dash_item_container_set_label_text (ShellDashItemContainer *self,
                                          const char             *text)
{
  ShellDashItemContainerPrivate *priv;

  g_return_if_fail (SHELL_IS_DASH_ITEM_CONTAINER (self));

  priv = shell_dash_item_container_get_instance_private (self);
  g_free (priv->label_text);
  priv->label_text = g_strdup (text);

  if (priv->child != NULL)
    clutter_actor_set_accessible_name (priv->child, text);
}

/**
 * shell_dash_item_container_get_label_text:
 * @self: a #ShellDashItemContainer
 *
 * Returns: (transfer none): the text of the label
 */
const char *
shell_dash_item_container_get_label_text (ShellDashItemContainer *self)
{
  ShellDashItemContainerPrivate *priv;

  g_return_val_if_fail (SHELL_IS_DASH_ITEM_CONTAINER (self), NULL);

  priv = shell_dash_item_container_get_instance_private (self);
  return priv->label_text != NULL ? priv->label_text : "";
}

/**
 * shell_dash_item_container_show_label:
 * @self: a #ShellDashItemContainer
 *
 * Shows the label above the item (`showLabel ()`).
 */
void
shell_dash_item_container_show_label (ShellDashItemContainer *self)
{
  ShellDashItemContainerPrivate *priv;
  ShellEaseParams params = ease_params (DASH_ITEM_LABEL_SHOW_TIME, CLUTTER_EASE_OUT_QUAD);
  ClutterActor *label, *stage;
  StThemeNode *node;
  ClutterActorBox box;
  float stage_x, stage_y;
  float item_width, label_width, x_offset, x, y;
  double y_offset = 0;

  g_return_if_fail (SHELL_IS_DASH_ITEM_CONTAINER (self));

  priv = shell_dash_item_container_get_instance_private (self);
  if (priv->label_text == NULL || priv->label_text[0] == '\0' || priv->label == NULL)
    return;

  label = CLUTTER_ACTOR (priv->label);
  st_label_set_text (priv->label, priv->label_text);
  clutter_actor_set_opacity (label, 0);
  clutter_actor_show (label);

  clutter_actor_get_transformed_position (CLUTTER_ACTOR (self), &stage_x, &stage_y);

  clutter_actor_get_allocation_box (CLUTTER_ACTOR (self), &box);
  item_width = clutter_actor_box_get_width (&box);

  label_width = clutter_actor_get_width (label);
  x_offset = floorf ((item_width - label_width) / 2);
  stage = CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ()));
  x = CLAMP (stage_x + x_offset, 0, clutter_actor_get_width (stage) - label_width);

  node = st_widget_get_theme_node (ST_WIDGET (label));
  y_offset = st_theme_node_get_length (node, "-y-offset");

  y = stage_y - clutter_actor_get_height (label) - y_offset;

  clutter_actor_set_position (label, x, y);
  shell_actor_ease (label, &params, NULL, NULL, NULL,
                    "opacity", 255,
                    NULL);
}

typedef struct
{
  ClutterActor *actor;
} HideLabelData;

static void
hide_label_done_cb (gboolean finished,
                    gpointer user_data)
{
  HideLabelData *data = user_data;

  if (finished)
    clutter_actor_hide (data->actor);
}

static void
hide_label_data_free (gpointer user_data)
{
  HideLabelData *data = user_data;

  g_object_unref (data->actor);
  g_free (data);
}

/**
 * shell_dash_item_container_hide_label:
 * @self: a #ShellDashItemContainer
 *
 * Hides the label (`hideLabel ()`).
 */
void
shell_dash_item_container_hide_label (ShellDashItemContainer *self)
{
  ShellDashItemContainerPrivate *priv;
  ShellEaseParams params = ease_params (DASH_ITEM_LABEL_HIDE_TIME, CLUTTER_EASE_OUT_QUAD);
  HideLabelData *data;

  g_return_if_fail (SHELL_IS_DASH_ITEM_CONTAINER (self));

  priv = shell_dash_item_container_get_instance_private (self);
  if (priv->label == NULL)
    return;

  data = g_new0 (HideLabelData, 1);
  data->actor = g_object_ref (CLUTTER_ACTOR (priv->label));

  shell_actor_ease (CLUTTER_ACTOR (priv->label), &params,
                    hide_label_done_cb, data, hide_label_data_free,
                    "opacity", 0,
                    NULL);
}

/**
 * shell_dash_item_container_show:
 * @self: a #ShellDashItemContainer
 * @animate: whether to animate the item in
 *
 * Shows the item, which is hidden until then (`show ()`). Nothing happens
 * if the item has no child.
 */
void
shell_dash_item_container_show (ShellDashItemContainer *self,
                                gboolean                animate)
{
  ShellDashItemContainerPrivate *priv;
  ShellEaseParams params;

  g_return_if_fail (SHELL_IS_DASH_ITEM_CONTAINER (self));

  priv = shell_dash_item_container_get_instance_private (self);
  if (priv->child == NULL)
    return;

  params = ease_params (animate ? SHELL_DASH_ANIMATION_TIME : 0, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    "opacity", 255,
                    NULL);
}

static void
animate_out_done_cb (gboolean finished,
                     gpointer user_data)
{
  GWeakRef *ref = user_data;
  g_autoptr (ShellDashItemContainer) self = g_weak_ref_get (ref);

  if (finished && self != NULL)
    clutter_actor_destroy (CLUTTER_ACTOR (self));
}

static void
weak_ref_free (gpointer data)
{
  GWeakRef *ref = data;

  g_weak_ref_clear (ref);
  g_free (ref);
}

/**
 * shell_dash_item_container_animate_out_and_destroy:
 * @self: a #ShellDashItemContainer
 *
 * Hides the item with an animation and destroys it
 * (`animateOutAndDestroy ()`).
 */
void
shell_dash_item_container_animate_out_and_destroy (ShellDashItemContainer *self)
{
  ShellDashItemContainerPrivate *priv;
  ShellEaseParams params = ease_params (SHELL_DASH_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
  GWeakRef *ref;

  g_return_if_fail (SHELL_IS_DASH_ITEM_CONTAINER (self));

  priv = shell_dash_item_container_get_instance_private (self);
  if (priv->label != NULL)
    clutter_actor_hide (CLUTTER_ACTOR (priv->label));

  if (priv->child == NULL)
    {
      clutter_actor_destroy (CLUTTER_ACTOR (self));
      return;
    }

  priv->animating_out = TRUE;
  g_object_notify_by_pspec (G_OBJECT (self), container_props[CONTAINER_PROP_ANIMATING_OUT]);

  ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, self);
  shell_actor_ease (CLUTTER_ACTOR (self), &params, animate_out_done_cb, ref, weak_ref_free,
                    "scale-x", 0.0,
                    "scale-y", 0.0,
                    "opacity", 0,
                    NULL);
}

static void
container_get_preferred_height (ClutterActor *actor,
                                float         for_width,
                                float        *min_height_p,
                                float        *natural_height_p)
{
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  double scale_y;
  float min_height, natural_height;

  st_theme_node_adjust_for_width (theme_node, &for_width);
  CLUTTER_ACTOR_CLASS (shell_dash_item_container_parent_class)->get_preferred_height (actor,
                                                                                     for_width,
                                                                                     &min_height,
                                                                                     &natural_height);
  clutter_actor_get_scale (actor, NULL, &scale_y);
  min_height *= scale_y;
  natural_height *= scale_y;
  st_theme_node_adjust_preferred_height (theme_node, &min_height, &natural_height);

  if (min_height_p != NULL)
    *min_height_p = min_height;
  if (natural_height_p != NULL)
    *natural_height_p = natural_height;
}

static void
container_get_preferred_width (ClutterActor *actor,
                               float         for_height,
                               float        *min_width_p,
                               float        *natural_width_p)
{
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  double scale_x;
  float min_width, natural_width;

  st_theme_node_adjust_for_height (theme_node, &for_height);
  CLUTTER_ACTOR_CLASS (shell_dash_item_container_parent_class)->get_preferred_width (actor,
                                                                                    for_height,
                                                                                    &min_width,
                                                                                    &natural_width);
  clutter_actor_get_scale (actor, &scale_x, NULL);
  min_width *= scale_x;
  natural_width *= scale_x;
  st_theme_node_adjust_preferred_width (theme_node, &min_width, &natural_width);

  if (min_width_p != NULL)
    *min_width_p = min_width;
  if (natural_width_p != NULL)
    *natural_width_p = natural_width;
}

static void
container_scale_changed_cb (GObject    *object,
                            GParamSpec *pspec,
                            gpointer    user_data)
{
  clutter_actor_queue_relayout (CLUTTER_ACTOR (object));
}

static void
container_label_destroyed_cb (ClutterActor *label,
                              gpointer      user_data)
{
  ShellDashItemContainer *self = user_data;
  ShellDashItemContainerPrivate *priv = shell_dash_item_container_get_instance_private (self);

  g_clear_weak_pointer (&priv->label);
  g_object_notify_by_pspec (G_OBJECT (self), container_props[CONTAINER_PROP_LABEL]);
}

static void
container_destroy (ClutterActor *actor)
{
  ShellDashItemContainer *self = SHELL_DASH_ITEM_CONTAINER (actor);
  ShellDashItemContainerPrivate *priv = shell_dash_item_container_get_instance_private (self);

  if (priv->child != NULL)
    clutter_actor_destroy (priv->child);

  if (priv->label != NULL)
    {
      ClutterActor *label = CLUTTER_ACTOR (priv->label);

      g_signal_handlers_disconnect_by_func (label, container_label_destroyed_cb, self);
      g_clear_weak_pointer (&priv->label);
      clutter_actor_destroy (label);
    }

  CLUTTER_ACTOR_CLASS (shell_dash_item_container_parent_class)->destroy (actor);
}

static void
shell_dash_item_container_constructed (GObject *object)
{
  ShellDashItemContainer *self = SHELL_DASH_ITEM_CONTAINER (object);
  ShellDashItemContainerPrivate *priv = shell_dash_item_container_get_instance_private (self);
  ShellLayoutManager *layout_manager = get_layout_manager ();
  ClutterActor *label;

  G_OBJECT_CLASS (shell_dash_item_container_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (self), "dash-item-container");
  clutter_actor_set_pivot_point (CLUTTER_ACTOR (self), 0.5f, 0.5f);
  clutter_actor_set_layout_manager (CLUTTER_ACTOR (self), clutter_bin_layout_new ());
  clutter_actor_set_scale (CLUTTER_ACTOR (self), 0, 0);
  clutter_actor_set_opacity (CLUTTER_ACTOR (self), 0);
  clutter_actor_set_x_expand (CLUTTER_ACTOR (self), TRUE);
  clutter_actor_set_x_align (CLUTTER_ACTOR (self), CLUTTER_ACTOR_ALIGN_CENTER);

  label = g_object_new (ST_TYPE_LABEL,
                        "style-class", "dash-label",
                        NULL);
  g_set_weak_pointer (&priv->label, ST_LABEL (label));
  clutter_actor_hide (label);
  if (layout_manager != NULL)
    shell_layout_manager_add_chrome (layout_manager, label, SHELL_CHROME_FLAGS_NONE);
  g_signal_connect (label, "destroy", G_CALLBACK (container_label_destroyed_cb), self);

  g_signal_connect (self, "notify::scale-x", G_CALLBACK (container_scale_changed_cb), NULL);
  g_signal_connect (self, "notify::scale-y", G_CALLBACK (container_scale_changed_cb), NULL);
}

static void
shell_dash_item_container_dispose (GObject *object)
{
  ShellDashItemContainer *self = SHELL_DASH_ITEM_CONTAINER (object);
  ShellDashItemContainerPrivate *priv = shell_dash_item_container_get_instance_private (self);

  if (priv->label != NULL)
    g_signal_handlers_disconnect_by_func (priv->label, container_label_destroyed_cb, self);

  G_OBJECT_CLASS (shell_dash_item_container_parent_class)->dispose (object);

  g_clear_weak_pointer (&priv->label);
  g_clear_weak_pointer (&priv->child);
}

static void
shell_dash_item_container_finalize (GObject *object)
{
  ShellDashItemContainer *self = SHELL_DASH_ITEM_CONTAINER (object);
  ShellDashItemContainerPrivate *priv = shell_dash_item_container_get_instance_private (self);

  g_free (priv->label_text);

  G_OBJECT_CLASS (shell_dash_item_container_parent_class)->finalize (object);
}

static void
shell_dash_item_container_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellDashItemContainer *self = SHELL_DASH_ITEM_CONTAINER (object);
  ShellDashItemContainerPrivate *priv = shell_dash_item_container_get_instance_private (self);

  switch (prop_id)
    {
    case CONTAINER_PROP_CHILD:
      g_value_set_object (value, priv->child);
      break;

    case CONTAINER_PROP_LABEL:
      g_value_set_object (value, priv->label);
      break;

    case CONTAINER_PROP_ANIMATING_OUT:
      g_value_set_boolean (value, priv->animating_out);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_dash_item_container_class_init (ShellDashItemContainerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_dash_item_container_constructed;
  object_class->dispose = shell_dash_item_container_dispose;
  object_class->finalize = shell_dash_item_container_finalize;
  object_class->get_property = shell_dash_item_container_get_property;

  actor_class->destroy = container_destroy;
  actor_class->get_preferred_height = container_get_preferred_height;
  actor_class->get_preferred_width = container_get_preferred_width;

  /**
   * ShellDashItemContainer:child:
   *
   * The actor the container shows (`child`).
   */
  container_props[CONTAINER_PROP_CHILD] =
    g_param_spec_object ("child", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READABLE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDashItemContainer:label:
   *
   * The label shown above the item, %NULL once it is destroyed (`label`).
   */
  container_props[CONTAINER_PROP_LABEL] =
    g_param_spec_object ("label", NULL, NULL,
                         ST_TYPE_LABEL,
                         G_PARAM_READABLE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDashItemContainer:animating-out:
   *
   * Whether the item animates out and will be destroyed (`animatingOut`).
   */
  container_props[CONTAINER_PROP_ANIMATING_OUT] =
    g_param_spec_boolean ("animating-out", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, CONTAINER_N_PROPS, container_props);
}

static void
shell_dash_item_container_init (ShellDashItemContainer *self)
{
}

/**
 * shell_dash_item_container_new:
 *
 * Creates an empty item container (`new DashItemContainer ()`). It is
 * invisible until shell_dash_item_container_show () is called.
 *
 * Returns: (transfer full): a new #ShellDashItemContainer
 */
ShellDashItemContainer *
shell_dash_item_container_new (void)
{
  return g_object_new (SHELL_TYPE_DASH_ITEM_CONTAINER, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellShowAppsIcon                                                         */
/* ------------------------------------------------------------------------ */

struct _ShellShowAppsIcon
{
  ShellDashItemContainer parent;

  StButton *toggle_button; /* weak */
  ShellBaseIcon *icon; /* weak */
  ClutterActor *icon_actor; /* weak */
};

static void shell_show_apps_icon_drag_target_iface_init (ShellDragTargetInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellShowAppsIcon, shell_show_apps_icon,
                               SHELL_TYPE_DASH_ITEM_CONTAINER,
                               G_IMPLEMENT_INTERFACE (SHELL_TYPE_DRAG_TARGET,
                                                      shell_show_apps_icon_drag_target_iface_init))

/**
 * shell_show_apps_icon_get_toggle_button:
 * @self: a #ShellShowAppsIcon
 *
 * Returns: (transfer none): the button that shows the application grid
 *   (`toggleButton`)
 */
StButton *
shell_show_apps_icon_get_toggle_button (ShellShowAppsIcon *self)
{
  g_return_val_if_fail (SHELL_IS_SHOW_APPS_ICON (self), NULL);

  return self->toggle_button;
}

/**
 * shell_show_apps_icon_get_icon:
 * @self: a #ShellShowAppsIcon
 *
 * Returns: (transfer none): the icon of the button (`icon`)
 */
ShellBaseIcon *
shell_show_apps_icon_get_icon (ShellShowAppsIcon *self)
{
  g_return_val_if_fail (SHELL_IS_SHOW_APPS_ICON (self), NULL);

  return self->icon;
}

static ClutterActor *
show_apps_create_icon_cb (int      size,
                          gpointer user_data)
{
  ShellShowAppsIcon *self = user_data;
  ClutterActor *icon;

  icon = g_object_new (ST_TYPE_ICON,
                       "icon-name", "view-app-grid-symbolic",
                       "icon-size", size,
                       "style-class", "show-apps-icon",
                       "track-hover", TRUE,
                       NULL);
  g_clear_weak_pointer (&self->icon_actor);
  g_set_weak_pointer (&self->icon_actor, icon);

  return icon;
}

static gboolean
show_apps_can_remove_app (ShellApp *app)
{
  if (app == NULL)
    return FALSE;

  if (!g_settings_is_writable (shell_global_get_settings (shell_global_get ()), "favorite-apps"))
    return FALSE;

  return shell_app_favorites_is_favorite (shell_app_favorites_get_default (),
                                          shell_app_get_id (app));
}

/**
 * shell_show_apps_icon_set_drag_app:
 * @self: a #ShellShowAppsIcon
 * @app: (nullable): the application which is dragged over the dash, or %NULL
 *
 * Shows the button as the place to drop a favorite to unpin it
 * (`setDragApp ()`).
 */
void
shell_show_apps_icon_set_drag_app (ShellShowAppsIcon *self,
                                   ShellApp          *app)
{
  gboolean can_remove;

  g_return_if_fail (SHELL_IS_SHOW_APPS_ICON (self));

  can_remove = show_apps_can_remove_app (app);

  st_widget_set_hover (ST_WIDGET (self->toggle_button), can_remove);
  if (self->icon_actor != NULL)
    st_widget_set_hover (ST_WIDGET (self->icon_actor), can_remove);

  if (can_remove)
    shell_dash_item_container_set_label_text (SHELL_DASH_ITEM_CONTAINER (self), _("Unpin"));
  else
    shell_dash_item_container_set_label_text (SHELL_DASH_ITEM_CONTAINER (self), _("Show Apps"));
}

static ShellDragMotionResult
show_apps_handle_drag_over (ShellDragTarget *target,
                            GObject         *source,
                            ClutterActor    *actor,
                            float            x,
                            float            y,
                            uint32_t         time)
{
  if (!show_apps_can_remove_app (shell_dash_get_app_from_source (source)))
    return SHELL_DRAG_MOTION_RESULT_NO_DROP;

  return SHELL_DRAG_MOTION_RESULT_MOVE_DROP;
}

static gboolean
remove_favorite_later_cb (gpointer user_data)
{
  const char *id = user_data;

  shell_app_favorites_remove_favorite (shell_app_favorites_get_default (), id);

  return G_SOURCE_REMOVE;
}

static void
add_later_before_redraw (GSourceFunc    func,
                         gpointer       user_data,
                         GDestroyNotify destroy)
{
  MetaCompositor *compositor = shell_global_get_compositor (shell_global_get ());
  MetaLaters *laters = meta_compositor_get_laters (compositor);

  meta_laters_add (laters, META_LATER_BEFORE_REDRAW, func, user_data, destroy);
}

static gboolean
show_apps_accept_drop (ShellDragTarget *target,
                       GObject         *source,
                       ClutterActor    *actor,
                       float            x,
                       float            y,
                       uint32_t         time)
{
  ShellApp *app = shell_dash_get_app_from_source (source);

  if (!show_apps_can_remove_app (app))
    return FALSE;

  add_later_before_redraw (remove_favorite_later_cb, g_strdup (shell_app_get_id (app)), g_free);

  return TRUE;
}

static void
shell_show_apps_icon_drag_target_iface_init (ShellDragTargetInterface *iface)
{
  iface->handle_drag_over = show_apps_handle_drag_over;
  iface->accept_drop = show_apps_accept_drop;
}

static void
shell_show_apps_icon_constructed (GObject *object)
{
  ShellShowAppsIcon *self = SHELL_SHOW_APPS_ICON (object);
  ClutterActor *toggle_button;
  ShellBaseIcon *icon;

  G_OBJECT_CLASS (shell_show_apps_icon_parent_class)->constructed (object);

  toggle_button = g_object_new (ST_TYPE_BUTTON,
                                "style-class", "show-apps",
                                "track-hover", TRUE,
                                "can-focus", TRUE,
                                "toggle-mode", TRUE,
                                NULL);
  g_set_weak_pointer (&self->toggle_button, ST_BUTTON (toggle_button));

  icon = g_object_new (SHELL_TYPE_BASE_ICON,
                       "label-text", _("Show Apps"),
                       "set-size-manually", TRUE,
                       "show-label", FALSE,
                       NULL);
  g_set_weak_pointer (&self->icon, icon);
  shell_base_icon_set_create_icon_func (icon, show_apps_create_icon_cb, self, NULL);
  clutter_actor_set_y_align (CLUTTER_ACTOR (icon), CLUTTER_ACTOR_ALIGN_CENTER);

  st_bin_set_child (ST_BIN (toggle_button), CLUTTER_ACTOR (icon));

  shell_dash_item_container_set_child (SHELL_DASH_ITEM_CONTAINER (self), toggle_button);
  shell_show_apps_icon_set_drag_app (self, NULL);
}

static void
shell_show_apps_icon_dispose (GObject *object)
{
  ShellShowAppsIcon *self = SHELL_SHOW_APPS_ICON (object);

  G_OBJECT_CLASS (shell_show_apps_icon_parent_class)->dispose (object);

  g_clear_weak_pointer (&self->toggle_button);
  g_clear_weak_pointer (&self->icon);
  g_clear_weak_pointer (&self->icon_actor);
}

static void
shell_show_apps_icon_class_init (ShellShowAppsIconClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_show_apps_icon_constructed;
  object_class->dispose = shell_show_apps_icon_dispose;
}

static void
shell_show_apps_icon_init (ShellShowAppsIcon *self)
{
}

/**
 * shell_show_apps_icon_new:
 *
 * Creates the item with the button that shows the application grid
 * (`new ShowAppsIcon ()`).
 *
 * Returns: (transfer full): a new #ShellShowAppsIcon
 */
ShellShowAppsIcon *
shell_show_apps_icon_new (void)
{
  return g_object_new (SHELL_TYPE_SHOW_APPS_ICON, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellDashIconsLayout (private, DashIconsLayout)                           */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_DASH_ICONS_LAYOUT (shell_dash_icons_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellDashIconsLayout, shell_dash_icons_layout,
                      SHELL, DASH_ICONS_LAYOUT, ClutterBoxLayout)

struct _ShellDashIconsLayout
{
  ClutterBoxLayout parent;
};

G_DEFINE_FINAL_TYPE (ShellDashIconsLayout, shell_dash_icons_layout, CLUTTER_TYPE_BOX_LAYOUT)

static void
dash_icons_layout_get_preferred_width (ClutterLayoutManager *manager,
                                       ClutterActor         *container,
                                       float                 for_height,
                                       float                *min_width_p,
                                       float                *natural_width_p)
{
  float nat_width;

  CLUTTER_LAYOUT_MANAGER_CLASS (shell_dash_icons_layout_parent_class)->get_preferred_width (manager,
                                                                                           container,
                                                                                           for_height,
                                                                                           NULL,
                                                                                           &nat_width);
  if (min_width_p != NULL)
    *min_width_p = 0;
  if (natural_width_p != NULL)
    *natural_width_p = nat_width;
}

static void
shell_dash_icons_layout_class_init (ShellDashIconsLayoutClass *klass)
{
  CLUTTER_LAYOUT_MANAGER_CLASS (klass)->get_preferred_width = dash_icons_layout_get_preferred_width;
}

static void
shell_dash_icons_layout_init (ShellDashIconsLayout *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellDashDragTarget (private, DashDragTarget)                             */
/* ------------------------------------------------------------------------ */

/* The drop target of the icons box of the dash. The dash itself is not
 * registered as a target: the positions it is given are relative to the box,
 * and dropping next to the box is not accepted */
#define SHELL_TYPE_DASH_DRAG_TARGET (shell_dash_drag_target_get_type ())
G_DECLARE_FINAL_TYPE (ShellDashDragTarget, shell_dash_drag_target,
                      SHELL, DASH_DRAG_TARGET, GObject)

struct _ShellDashDragTarget
{
  GObject parent;

  ShellDash *dash; /* weak */
};

static void shell_dash_drag_target_iface_init (ShellDragTargetInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellDashDragTarget, shell_dash_drag_target, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (SHELL_TYPE_DRAG_TARGET,
                                                      shell_dash_drag_target_iface_init))

static ShellDragMotionResult dash_handle_drag_over (ShellDash    *self,
                                                    GObject      *source,
                                                    ClutterActor *actor,
                                                    float         x);
static gboolean              dash_accept_drop      (ShellDash    *self,
                                                    GObject      *source);

static ShellDragMotionResult
dash_target_handle_drag_over (ShellDragTarget *target,
                              GObject         *source,
                              ClutterActor    *actor,
                              float            x,
                              float            y,
                              uint32_t         time)
{
  ShellDashDragTarget *self = SHELL_DASH_DRAG_TARGET (target);

  if (self->dash == NULL)
    return SHELL_DRAG_MOTION_RESULT_CONTINUE;

  return dash_handle_drag_over (self->dash, source, actor, x);
}

static gboolean
dash_target_accept_drop (ShellDragTarget *target,
                         GObject         *source,
                         ClutterActor    *actor,
                         float            x,
                         float            y,
                         uint32_t         time)
{
  ShellDashDragTarget *self = SHELL_DASH_DRAG_TARGET (target);

  if (self->dash == NULL)
    return FALSE;

  return dash_accept_drop (self->dash, source);
}

static void
shell_dash_drag_target_iface_init (ShellDragTargetInterface *iface)
{
  iface->handle_drag_over = dash_target_handle_drag_over;
  iface->accept_drop = dash_target_accept_drop;
}

static void
shell_dash_drag_target_dispose (GObject *object)
{
  ShellDashDragTarget *self = SHELL_DASH_DRAG_TARGET (object);

  g_clear_weak_pointer (&self->dash);

  G_OBJECT_CLASS (shell_dash_drag_target_parent_class)->dispose (object);
}

static void
shell_dash_drag_target_class_init (ShellDashDragTargetClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = shell_dash_drag_target_dispose;
}

static void
shell_dash_drag_target_init (ShellDashDragTarget *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellDash                                                                 */
/* ------------------------------------------------------------------------ */

enum
{
  DASH_PROP_0,
  DASH_PROP_WINDOW_MANAGER,
  DASH_PROP_ICON_SIZE,
  DASH_PROP_SHOW_APPS_BUTTON,
  DASH_N_PROPS
};

static GParamSpec *dash_props[DASH_N_PROPS] = { NULL, };

enum
{
  DASH_ICON_SIZE_CHANGED,
  DASH_LAST_SIGNAL
};

static guint dash_signals[DASH_LAST_SIGNAL] = { 0, };

struct _ShellDash
{
  StWidget parent;

  ShellWindowManager *window_manager; /* weak */

  int max_width;
  int max_height;
  int icon_size;
  gboolean shown_initially;

  ClutterActor *separator; /* weak */
  ShellDashItemContainer *drag_placeholder; /* weak */
  int drag_placeholder_pos;
  int animating_placeholders_count;
  guint show_label_timeout_id;
  ShellDashItemContainer *pending_label_item; /* weak */
  guint reset_hover_timeout_id;
  gboolean label_showing;

  StBoxLayout *dash_container; /* weak */
  ClutterActor *box; /* weak */
  ShellShowAppsIcon *show_apps_icon; /* weak */
  ClutterActor *background; /* weak */
  ShellDashDragTarget *drag_target;

  ShellDeferredWork *work;
  ShellAppSystem *app_system;

  gboolean drag_cancelled;
  guint drag_monitor;
  ShellDashItemContainer *empty_drop_target; /* weak */
};

G_DEFINE_FINAL_TYPE (ShellDash, shell_dash, ST_TYPE_WIDGET)

/**
 * shell_dash_get_icon_size:
 * @self: a #ShellDash
 *
 * Returns: the size of the application icons, in logical pixels (`iconSize`)
 */
int
shell_dash_get_icon_size (ShellDash *self)
{
  g_return_val_if_fail (SHELL_IS_DASH (self), 0);

  return self->icon_size;
}

/**
 * shell_dash_get_show_apps_button:
 * @self: a #ShellDash
 *
 * Returns: (transfer none): the button that shows the application grid
 *   (`showAppsButton`)
 */
StButton *
shell_dash_get_show_apps_button (ShellDash *self)
{
  g_return_val_if_fail (SHELL_IS_DASH (self), NULL);

  return shell_show_apps_icon_get_toggle_button (self->show_apps_icon);
}

/**
 * shell_dash_get_dash_container:
 * @self: a #ShellDash
 *
 * Returns: (transfer none): the box that holds the box of the icons and the
 *   button that shows the application grid (`_dashContainer`)
 */
StBoxLayout *
shell_dash_get_dash_container (ShellDash *self)
{
  g_return_val_if_fail (SHELL_IS_DASH (self), NULL);

  return self->dash_container;
}

/**
 * shell_dash_get_box:
 * @self: a #ShellDash
 *
 * Returns: (transfer none): the box of the icons: the favorites, then a
 *   separator and the running applications (`_box`)
 */
ClutterActor *
shell_dash_get_box (ShellDash *self)
{
  g_return_val_if_fail (SHELL_IS_DASH (self), NULL);

  return self->box;
}

static ShellDash *
find_dash (ClutterActor *actor)
{
  for (; actor != NULL; actor = clutter_actor_get_parent (actor))
    {
      if (SHELL_IS_DASH (actor))
        return SHELL_DASH (actor);
    }

  return NULL;
}

/* The app of the item, which has a #ShellDashIcon as its child */
static ShellApp *
item_get_app (ClutterActor *actor)
{
  ClutterActor *child;

  if (!SHELL_IS_DASH_ITEM_CONTAINER (actor))
    return NULL;

  child = shell_dash_item_container_get_child (SHELL_DASH_ITEM_CONTAINER (actor));
  if (!SHELL_IS_DASH_ICON (child))
    return NULL;

  return shell_app_icon_get_app (SHELL_APP_ICON (child));
}

/* The icon the container shows. The show-apps icon wraps a plain button;
 * app items wrap an app icon */
static ShellBaseIcon *
dash_icon_of (ShellDash    *self,
              ClutterActor *container)
{
  ClutterActor *child;

  if (container == CLUTTER_ACTOR (self->show_apps_icon))
    return shell_show_apps_icon_get_icon (self->show_apps_icon);

  child = shell_dash_item_container_get_child (SHELL_DASH_ITEM_CONTAINER (container));
  if (SHELL_IS_APP_VIEW_ITEM (child))
    return shell_app_view_item_get_icon (SHELL_APP_VIEW_ITEM (child));

  return NULL;
}

static void
dash_queue_redisplay (ShellDash *self)
{
  if (self->work != NULL)
    shell_deferred_work_queue (self->work);
}

static void
queue_redisplay_cb (gpointer instance,
                    gpointer user_data)
{
  dash_queue_redisplay (user_data);
}

static void
app_state_changed_cb (ShellAppSystem *app_system,
                      ShellApp       *app,
                      ShellDash      *self)
{
  dash_queue_redisplay (self);
}

static void
dash_remove_item_drag_monitor (ShellDash *self)
{
  if (self->drag_monitor != 0)
    {
      shell_dnd_remove_drag_monitor (self->drag_monitor);
      self->drag_monitor = 0;
    }
}

static void
dash_clear_drag_placeholder_destroyed_cb (ClutterActor *actor,
                                          ShellDash    *self)
{
  self->animating_placeholders_count--;
}

static void
dash_clear_drag_placeholder (ShellDash *self)
{
  if (self->drag_placeholder != NULL)
    {
      ShellDashItemContainer *placeholder = self->drag_placeholder;

      self->animating_placeholders_count++;
      g_signal_connect_object (placeholder, "destroy",
                               G_CALLBACK (dash_clear_drag_placeholder_destroyed_cb), self, 0);
      g_clear_weak_pointer (&self->drag_placeholder);
      shell_dash_item_container_animate_out_and_destroy (placeholder);
    }

  self->drag_placeholder_pos = -1;
}

static void
dash_clear_empty_drop_target (ShellDash *self)
{
  if (self->empty_drop_target != NULL)
    {
      ShellDashItemContainer *target = self->empty_drop_target;

      g_clear_weak_pointer (&self->empty_drop_target);
      shell_dash_item_container_animate_out_and_destroy (target);
    }
}

static void
dash_end_item_drag (ShellDash *self)
{
  dash_clear_drag_placeholder (self);
  dash_clear_empty_drop_target (self);
  shell_show_apps_icon_set_drag_app (self->show_apps_icon, NULL);
  dash_remove_item_drag_monitor (self);
}

static ShellDragMotionResult
dash_item_drag_motion_cb (ShellDragEvent *event,
                          gpointer        user_data)
{
  ShellDash *self = user_data;
  ShellApp *app = shell_dash_get_app_from_source (shell_drag_event_get_source (event));
  ClutterActor *target_actor = shell_drag_event_get_target_actor (event);
  gboolean show_apps_hovered;

  if (app == NULL)
    return SHELL_DRAG_MOTION_RESULT_CONTINUE;

  show_apps_hovered = target_actor != NULL &&
                      clutter_actor_contains (CLUTTER_ACTOR (self->show_apps_icon), target_actor);

  if (target_actor == NULL || !clutter_actor_contains (self->box, target_actor) || show_apps_hovered)
    dash_clear_drag_placeholder (self);

  shell_show_apps_icon_set_drag_app (self->show_apps_icon, show_apps_hovered ? app : NULL);

  return SHELL_DRAG_MOTION_RESULT_CONTINUE;
}

static void
dash_item_drag_begin_cb (ShellWindowManager *window_manager,
                         GObject            *source,
                         ShellDash          *self)
{
  self->drag_cancelled = FALSE;
  dash_remove_item_drag_monitor (self);
  self->drag_monitor = shell_dnd_add_drag_monitor (dash_item_drag_motion_cb, self, NULL,
                                                   NULL, NULL, NULL);

  if (clutter_actor_get_n_children (self->box) == 0)
    {
      ShellDashItemContainer *target = shell_dash_item_container_new ();
      ClutterActor *bin = g_object_new (ST_TYPE_BIN,
                                        "style-class", "empty-dash-drop-target",
                                        NULL);

      shell_dash_item_container_set_child (target, bin);
      g_set_weak_pointer (&self->empty_drop_target, target);
      clutter_actor_insert_child_at_index (self->box, CLUTTER_ACTOR (target), 0);
      shell_dash_item_container_show (target, TRUE);
    }
}

static void
dash_item_drag_cancelled_cb (ShellWindowManager *window_manager,
                             GObject            *source,
                             ShellDash          *self)
{
  self->drag_cancelled = TRUE;
  dash_end_item_drag (self);
}

static void
dash_item_drag_end_cb (ShellWindowManager *window_manager,
                       GObject            *source,
                       ShellDash          *self)
{
  if (self->drag_cancelled)
    return;

  dash_end_item_drag (self);
}

/**
 * shell_dash_window_drag_begin:
 * @self: a #ShellDash
 *
 * Tells the dash that the drag of a window started: it becomes half
 * transparent (`window-drag-begin` of the overview).
 */
void
shell_dash_window_drag_begin (ShellDash *self)
{
  ShellEaseParams params = ease_params (DASH_WINDOW_DRAG_FADE_TIME, CLUTTER_EASE_OUT_QUAD);

  g_return_if_fail (SHELL_IS_DASH (self));

  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "opacity", 128,
                    NULL);
}

/**
 * shell_dash_window_drag_end:
 * @self: a #ShellDash
 *
 * Tells the dash that the drag of a window ended or was cancelled
 * (`window-drag-end` and `window-drag-cancelled` of the overview).
 */
void
shell_dash_window_drag_end (ShellDash *self)
{
  ShellEaseParams params = ease_params (DASH_WINDOW_DRAG_FADE_TIME, CLUTTER_EASE_IN_QUAD);

  g_return_if_fail (SHELL_IS_DASH (self));

  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "opacity", 255,
                    NULL);
}

static void
dash_sync_label (ShellDash              *self,
                 ShellDashItemContainer *item,
                 ShellAppIcon           *app_icon);

typedef struct
{
  GWeakRef dash;
  GWeakRef item;
} ShowLabelData;

static void
show_label_data_free (gpointer user_data)
{
  ShowLabelData *data = user_data;

  g_weak_ref_clear (&data->dash);
  g_weak_ref_clear (&data->item);
  g_free (data);
}

static gboolean
show_label_timeout_cb (gpointer user_data)
{
  ShowLabelData *data = user_data;
  g_autoptr (ShellDash) self = g_weak_ref_get (&data->dash);
  g_autoptr (ShellDashItemContainer) item = g_weak_ref_get (&data->item);

  if (self == NULL)
    return G_SOURCE_REMOVE;

  self->show_label_timeout_id = 0;
  self->label_showing = TRUE;
  if (item != NULL)
    shell_dash_item_container_show_label (item);

  return G_SOURCE_REMOVE;
}

static gboolean
reset_hover_timeout_cb (gpointer user_data)
{
  GWeakRef *ref = user_data;
  g_autoptr (ShellDash) self = g_weak_ref_get (ref);

  if (self == NULL)
    return G_SOURCE_REMOVE;

  self->label_showing = FALSE;
  self->reset_hover_timeout_id = 0;

  return G_SOURCE_REMOVE;
}

static void
dash_sync_label (ShellDash              *self,
                 ShellDashItemContainer *item,
                 ShellAppIcon           *app_icon)
{
  gboolean should_show;

  if (app_icon != NULL)
    should_show = shell_app_icon_should_show_tooltip (app_icon);
  else
    should_show = st_widget_get_hover (ST_WIDGET (shell_dash_item_container_get_child (item)));

  if (should_show)
    {
      if (self->show_label_timeout_id == 0)
        {
          int timeout = self->label_showing ? 0 : DASH_ITEM_HOVER_TIMEOUT;
          ShowLabelData *data = g_new0 (ShowLabelData, 1);

          g_weak_ref_init (&data->dash, self);
          g_weak_ref_init (&data->item, item);
          self->show_label_timeout_id = g_timeout_add_full (G_PRIORITY_DEFAULT, timeout,
                                                            show_label_timeout_cb, data,
                                                            show_label_data_free);
          g_source_set_name_by_id (self->show_label_timeout_id, "[gnome-shell] item.showLabel");
          if (self->reset_hover_timeout_id > 0)
            {
              g_source_remove (self->reset_hover_timeout_id);
              self->reset_hover_timeout_id = 0;
            }
        }
    }
  else
    {
      if (self->show_label_timeout_id > 0)
        g_source_remove (self->show_label_timeout_id);
      self->show_label_timeout_id = 0;
      shell_dash_item_container_hide_label (item);
      if (self->label_showing)
        {
          GWeakRef *ref = g_new0 (GWeakRef, 1);

          g_weak_ref_init (ref, self);
          self->reset_hover_timeout_id = g_timeout_add_full (G_PRIORITY_DEFAULT,
                                                             DASH_ITEM_HOVER_TIMEOUT,
                                                             reset_hover_timeout_cb, ref,
                                                             weak_ref_free);
          g_source_set_name_by_id (self->reset_hover_timeout_id,
                                   "[gnome-shell] this._labelShowing");
        }
    }
}

/* The item and the app icon (if any) of a child of an item */
static gboolean
dash_find_item (ClutterActor             *child,
                ShellDash               **dash,
                ShellDashItemContainer  **item,
                ShellAppIcon            **app_icon)
{
  ClutterActor *parent = clutter_actor_get_parent (child);

  if (!SHELL_IS_DASH_ITEM_CONTAINER (parent))
    return FALSE;

  *item = SHELL_DASH_ITEM_CONTAINER (parent);
  *dash = find_dash (parent);
  *app_icon = SHELL_IS_APP_ICON (child) ? SHELL_APP_ICON (child) : NULL;

  return *dash != NULL;
}

static void
child_hover_changed_cb (GObject    *object,
                        GParamSpec *pspec,
                        gpointer    user_data)
{
  ShellDash *self;
  ShellDashItemContainer *item;
  ShellAppIcon *app_icon;

  if (dash_find_item (CLUTTER_ACTOR (object), &self, &item, &app_icon))
    dash_sync_label (self, item, app_icon);
}

static void
child_clicked_cb (StButton *button,
                  gpointer  user_data)
{
  ShellDash *self;
  ShellDashItemContainer *item;
  ShellAppIcon *app_icon;

  if (!dash_find_item (CLUTTER_ACTOR (button), &self, &item, &app_icon))
    return;

  self->label_showing = FALSE;
  shell_dash_item_container_hide_label (item);
}

static void
child_overview_closing_changed_cb (GObject    *window_manager,
                                   GParamSpec *pspec,
                                   gpointer    user_data)
{
  ClutterActor *child = user_data;
  ShellDash *self;
  ShellDashItemContainer *item;
  ShellAppIcon *app_icon;

  /* The overview's `hiding` */
  if (!shell_window_manager_get_overview_closing (SHELL_WINDOW_MANAGER (window_manager)))
    return;

  if (!dash_find_item (child, &self, &item, &app_icon))
    return;

  self->label_showing = FALSE;
  shell_dash_item_container_hide_label (item);
}

static void
app_icon_sync_tooltip_cb (ShellAppIcon *app_icon,
                          gpointer      user_data)
{
  ShellDash *self;
  ShellDashItemContainer *item;
  ShellAppIcon *icon;

  if (dash_find_item (CLUTTER_ACTOR (app_icon), &self, &item, &icon))
    dash_sync_label (self, item, icon);
}

static void
app_icon_menu_state_changed_cb (ShellAppIcon *app_icon,
                                gboolean      opened,
                                gpointer      user_data)
{
  ShellDash *self;
  ShellDashItemContainer *item;
  ShellAppIcon *icon;

  if (!dash_find_item (CLUTTER_ACTOR (app_icon), &self, &item, &icon))
    return;

  /* When the menu closes, it calls sync_hover, which means
   * that the notify::hover handler does everything we need to. */
  if (opened)
    {
      if (self->show_label_timeout_id > 0)
        {
          g_source_remove (self->show_label_timeout_id);
          self->show_label_timeout_id = 0;
        }

      shell_dash_item_container_hide_label (item);
    }
}

static void
dash_hook_up_label (ShellDash              *self,
                    ShellDashItemContainer *item,
                    ShellAppIcon           *app_icon)
{
  ClutterActor *child = shell_dash_item_container_get_child (item);

  g_signal_connect_object (child, "notify::hover",
                           G_CALLBACK (child_hover_changed_cb), self, 0);
  g_signal_connect_object (child, "clicked",
                           G_CALLBACK (child_clicked_cb), self, 0);

  if (self->window_manager != NULL)
    g_signal_connect_object (self->window_manager, "notify::overview-closing",
                             G_CALLBACK (child_overview_closing_changed_cb), child, 0);

  if (app_icon != NULL)
    g_signal_connect_object (app_icon, "sync-tooltip",
                             G_CALLBACK (app_icon_sync_tooltip_cb), self, 0);
}

static ShellDashItemContainer *
dash_create_app_item (ShellDash *self,
                      ShellApp  *app)
{
  ShellDashItemContainer *item = shell_dash_item_container_new ();
  ShellDashIcon *app_icon = shell_dash_icon_new (app, self->window_manager);

  g_signal_connect_object (app_icon, "menu-state-changed",
                           G_CALLBACK (app_icon_menu_state_changed_cb), self, 0);

  shell_dash_item_container_set_child (item, CLUTTER_ACTOR (app_icon));

  /* Override default AppIcon label_actor, now the
   * accessible_name is set at DashItemContainer.setLabelText */
  st_widget_set_label_actor (ST_WIDGET (app_icon), NULL);
  shell_dash_item_container_set_label_text (item, shell_app_get_name (app));

  shell_app_view_item_set_drag_icon_size (SHELL_APP_VIEW_ITEM (app_icon), self->icon_size);
  shell_base_icon_set_icon_size (shell_app_view_item_get_icon (SHELL_APP_VIEW_ITEM (app_icon)),
                                 self->icon_size, NULL);
  dash_hook_up_label (self, item, SHELL_APP_ICON (app_icon));

  return item;
}

static void
dash_adjust_icon_size (ShellDash *self)
{
  g_autoptr (GPtrArray) icon_children = g_ptr_array_new ();
  g_autoptr (GList) children = NULL;
  StThemeNode *theme_node;
  ClutterActorBox max_allocation, max_content;
  ShellBaseIcon *first_icon;
  ClutterActor *first_button;
  ClutterActor *first_icon_actor;
  float icon_width, icon_height, button_width, button_height;
  float avail_width, avail_height;
  double spacing;
  float max_icon_size;
  double scale_factor;
  int new_icon_size, old_icon_size;
  double scale;
  GList *l;
  guint i;

  /* For the icon size, we only consider children which are "proper"
   * icons (i.e. ignoring drag placeholders) and which are not
   * animating out (which means they will be destroyed at the end of
   * the animation) */
  children = clutter_actor_get_children (self->box);
  for (l = children; l != NULL; l = l->next)
    {
      ClutterActor *actor = l->data;

      if (!SHELL_IS_DASH_ITEM_CONTAINER (actor) ||
          shell_dash_item_container_get_animating_out (SHELL_DASH_ITEM_CONTAINER (actor)))
        continue;

      if (dash_icon_of (self, actor) == NULL)
        continue;

      g_ptr_array_add (icon_children, actor);
    }

  g_ptr_array_add (icon_children, self->show_apps_icon);

  if (self->max_width == -1 || self->max_height == -1)
    return;

  theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  max_allocation.x1 = 0;
  max_allocation.y1 = 0;
  max_allocation.x2 = self->max_width;
  max_allocation.y2 = 42; /* whatever */
  st_theme_node_get_content_box (theme_node, &max_allocation, &max_content);
  avail_width = max_content.x2 - max_content.x1;
  spacing = st_theme_node_get_length (theme_node, "spacing");

  first_button = shell_dash_item_container_get_child (g_ptr_array_index (icon_children, 0));
  first_icon = dash_icon_of (self, g_ptr_array_index (icon_children, 0));
  if (first_icon == NULL || first_button == NULL)
    return;
  first_icon_actor = shell_base_icon_get_icon (first_icon);
  if (first_icon_actor == NULL)
    return;

  /* Enforce valid spacings during the size request */
  if (ST_IS_WIDGET (first_icon_actor))
    st_widget_ensure_style (ST_WIDGET (first_icon_actor));
  clutter_actor_get_preferred_size (first_icon_actor, NULL, NULL, &icon_width, &icon_height);
  clutter_actor_get_preferred_size (first_button, NULL, NULL, &button_width, &button_height);

  /* Subtract icon padding and box spacing from the available width */
  avail_width -= icon_children->len * (button_width - icon_width) +
                 (icon_children->len - 1) * spacing;

  avail_height = self->max_height;
  avail_height -= clutter_actor_get_margin_top (CLUTTER_ACTOR (self)) +
                  clutter_actor_get_margin_bottom (CLUTTER_ACTOR (self));
  avail_height -= st_theme_node_get_vertical_padding (st_widget_get_theme_node (ST_WIDGET (self->background)));
  avail_height -= st_theme_node_get_vertical_padding (theme_node);
  avail_height -= button_height - icon_height;

  max_icon_size = MIN (avail_width / icon_children->len, avail_height);

  scale_factor = st_theme_context_get_scale_factor
    (st_theme_context_get_for_stage (shell_global_get_stage (shell_global_get ())));

  new_icon_size = base_icon_sizes[0];
  for (i = 0; i < G_N_ELEMENTS (base_icon_sizes); i++)
    {
      if (base_icon_sizes[i] * scale_factor <= max_icon_size)
        new_icon_size = base_icon_sizes[i];
    }

  if (new_icon_size == self->icon_size)
    return;

  old_icon_size = self->icon_size;
  self->icon_size = new_icon_size;
  g_object_notify_by_pspec (G_OBJECT (self), dash_props[DASH_PROP_ICON_SIZE]);
  g_signal_emit (self, dash_signals[DASH_ICON_SIZE_CHANGED], 0);

  scale = (double) old_icon_size / new_icon_size;
  for (i = 0; i < icon_children->len; i++)
    {
      ClutterActor *container = g_ptr_array_index (icon_children, i);
      ShellBaseIcon *icon = dash_icon_of (self, container);
      ClutterActor *icon_actor;
      ShellEaseParams params = ease_params (SHELL_DASH_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
      float target_width, target_height, width, height;
      ClutterActor *child = shell_dash_item_container_get_child (SHELL_DASH_ITEM_CONTAINER (container));

      if (SHELL_IS_APP_VIEW_ITEM (child))
        shell_app_view_item_set_drag_icon_size (SHELL_APP_VIEW_ITEM (child), self->icon_size);

      /* Set the new size immediately, to keep the icons' sizes
       * in sync with this.iconSize */
      shell_base_icon_set_icon_size (icon, self->icon_size, NULL);

      /* Don't animate the icon size change when the overview
       * is transitioning, not visible or when initially filling
       * the dash */
      if (!is_overview_visible (self->window_manager) ||
          is_overview_animating (self->window_manager) ||
          !self->shown_initially)
        continue;

      icon_actor = shell_base_icon_get_icon (icon);
      if (icon_actor == NULL)
        continue;

      clutter_actor_get_size (icon_actor, &target_width, &target_height);

      /* Scale the icon's texture to the previous size and
       * tween to the new size */
      width = clutter_actor_get_width (icon_actor) * scale;
      height = clutter_actor_get_height (icon_actor) * scale;
      clutter_actor_set_size (icon_actor, width, height);

      shell_actor_ease (icon_actor, &params, NULL, NULL, NULL,
                        "width", (double) target_width,
                        "height", (double) target_height,
                        NULL);
    }

  if (self->separator != NULL)
    {
      ShellEaseParams params = ease_params (SHELL_DASH_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);

      shell_actor_ease (self->separator, &params, NULL, NULL, NULL,
                        "height", (double) self->icon_size,
                        NULL);
    }
}

typedef struct
{
  ShellApp *app;
  ShellDashItemContainer *item;
  int pos;
} AddedItem;

static void
emit_item_added_hook (ShellApp *app,
                      int       index)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  GVariantBuilder builder;

  if (!shell_hook_registry_is_declared (registry, "dash.item-added"))
    return;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "kind", g_variant_new_string ("app"));
  g_variant_builder_add (&builder, "{sv}", "owner", g_variant_new_string (""));
  g_variant_builder_add (&builder, "{sv}", "index", g_variant_new_int32 (index));
  g_variant_builder_add (&builder, "{sv}", "app-id", g_variant_new_string (shell_app_get_id (app)));

  shell_hook_registry_emit_after (registry, "dash.item-added", g_variant_builder_end (&builder));
}

static gboolean
ptr_array_contains (GPtrArray *array,
                    gpointer   item)
{
  return g_ptr_array_find (array, item, NULL);
}

static void
dash_redisplay (ShellDash *self)
{
  ShellAppFavorites *app_favorites = shell_app_favorites_get_default ();
  g_autoptr (GList) favorites = shell_app_favorites_get_favorites (app_favorites);
  g_autoptr (GSList) running = shell_app_system_get_running (self->app_system);
  g_autoptr (GList) box_children = clutter_actor_get_children (self->box);
  g_autoptr (GPtrArray) children = g_ptr_array_new ();
  g_autoptr (GPtrArray) old_apps = g_ptr_array_new ();
  g_autoptr (GPtrArray) new_apps = g_ptr_array_new ();
  g_autoptr (GArray) added_items = g_array_new (FALSE, TRUE, sizeof (AddedItem));
  g_autoptr (GPtrArray) removed_actors = g_ptr_array_new ();
  gboolean animate;
  guint n_favorites;
  int n_icons;
  guint new_index = 0, old_index = 0;
  GList *l;
  GSList *s;
  guint i;

  for (l = box_children; l != NULL; l = l->next)
    {
      ShellApp *app = item_get_app (l->data);

      if (app == NULL)
        continue;

      /* Apps currently in the dash */
      g_ptr_array_add (children, l->data);
      g_ptr_array_add (old_apps, app);
    }

  /* Apps supposed to be in the dash */
  for (l = favorites; l != NULL; l = l->next)
    g_ptr_array_add (new_apps, l->data);

  for (s = running; s != NULL; s = s->next)
    {
      ShellApp *app = s->data;

      if (shell_app_favorites_is_favorite (app_favorites, shell_app_get_id (app)))
        continue;

      g_ptr_array_add (new_apps, app);
    }

  /* Figure out the actual changes to the list of items; we iterate
   * over both the list of items currently in the dash and the list
   * of items expected there, and collect additions and removals.
   * Moves are both an addition and a removal, where the order of
   * the operations depends on whether we encounter the position
   * where the item has been added first or the one from where it
   * was removed.
   * There is an assumption that only one item is moved at a given
   * time; when moving several items at once, everything will still
   * end up at the right position, but there might be additional
   * additions/removals (e.g. it might remove all the launchers
   * and add them back in the new order even if a smaller set of
   * additions and removals is possible).
   * If above assumptions turns out to be a problem, we might need
   * to use a more sophisticated algorithm, e.g. Longest Common
   * Subsequence as used by diff. */
  while (new_index < new_apps->len || old_index < old_apps->len)
    {
      ShellApp *old_app = old_apps->len > old_index ? g_ptr_array_index (old_apps, old_index) : NULL;
      ShellApp *new_app = new_apps->len > new_index ? g_ptr_array_index (new_apps, new_index) : NULL;
      ShellApp *next_app;
      gboolean insert_here, already_removed = FALSE;
      AddedItem added;

      /* No change at oldIndex/newIndex */
      if (old_app == new_app)
        {
          old_index++;
          new_index++;
          continue;
        }

      /* App removed at oldIndex */
      if (old_app != NULL && !ptr_array_contains (new_apps, old_app))
        {
          g_ptr_array_add (removed_actors, g_ptr_array_index (children, old_index));
          old_index++;
          continue;
        }

      /* App added at newIndex */
      if (new_app != NULL && !ptr_array_contains (old_apps, new_app))
        {
          added.app = new_app;
          added.item = g_object_ref_sink (dash_create_app_item (self, new_app));
          added.pos = new_index;
          g_array_append_val (added_items, added);
          new_index++;
          continue;
        }

      /* App moved */
      next_app = new_apps->len > new_index + 1 ? g_ptr_array_index (new_apps, new_index + 1) : NULL;
      insert_here = next_app != NULL && next_app == old_app;
      for (i = 0; i < removed_actors->len; i++)
        {
          if (item_get_app (g_ptr_array_index (removed_actors, i)) == new_app)
            already_removed = TRUE;
        }

      if (insert_here || already_removed)
        {
          added.app = new_app;
          added.item = g_object_ref_sink (dash_create_app_item (self, new_app));
          added.pos = new_index + removed_actors->len;
          g_array_append_val (added_items, added);
          new_index++;
        }
      else
        {
          g_ptr_array_add (removed_actors, g_ptr_array_index (children, old_index));
          old_index++;
        }
    }

  for (i = 0; i < added_items->len; i++)
    {
      AddedItem *added = &g_array_index (added_items, AddedItem, i);

      clutter_actor_insert_child_at_index (self->box, CLUTTER_ACTOR (added->item), added->pos);
    }

  for (i = 0; i < removed_actors->len; i++)
    {
      ShellDashItemContainer *item = g_ptr_array_index (removed_actors, i);

      /* Don't animate item removal when the overview is transitioning
       * or hidden */
      if (is_overview_visible (self->window_manager) &&
          !is_overview_animating (self->window_manager))
        shell_dash_item_container_animate_out_and_destroy (item);
      else
        clutter_actor_destroy (CLUTTER_ACTOR (item));
    }

  dash_adjust_icon_size (self);

  /* Skip animations on first run when adding the initial set
   * of items, to avoid all items zooming in at once */

  animate = self->shown_initially &&
            is_overview_visible (self->window_manager) &&
            !is_overview_animating (self->window_manager);

  if (!self->shown_initially)
    self->shown_initially = TRUE;

  for (i = 0; i < added_items->len; i++)
    {
      AddedItem *added = &g_array_index (added_items, AddedItem, i);

      shell_dash_item_container_show (added->item, animate);
      emit_item_added_hook (added->app, added->pos + 1);
    }

  /* Update separator */
  n_favorites = g_list_length (favorites);
  n_icons = (int) children->len + (int) added_items->len - (int) removed_actors->len;
  if (n_favorites > 0 && (int) n_favorites < n_icons)
    {
      g_autoptr (GList) current_children = NULL;
      int pos = (int) n_favorites;
      guint items_before_separator = 0;

      if (self->separator == NULL)
        {
          ClutterActor *separator = g_object_new (ST_TYPE_WIDGET,
                                                  "style-class", "dash-separator",
                                                  "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                                  "height", (float) self->icon_size,
                                                  NULL);

          g_set_weak_pointer (&self->separator, separator);
          clutter_actor_add_child (self->box, separator);
        }

      current_children = clutter_actor_get_children (self->box);
      for (l = current_children; l != NULL; l = l->next)
        {
          if (SHELL_IS_DASH_ITEM_CONTAINER (l->data) &&
              shell_dash_item_container_get_animating_out (l->data))
            pos++;
          items_before_separator++;
          /* Assuming only one icon is moved at a time
           * Or else we need to track removed favorite items */
          if (items_before_separator > n_favorites)
            break;
        }
      if (self->drag_placeholder != NULL)
        pos++;
      clutter_actor_set_child_at_index (self->box, self->separator,
                                        MIN (pos, (int) clutter_actor_get_n_children (self->box) - 1));
    }
  else if (self->separator != NULL)
    {
      clutter_actor_destroy (self->separator);
      g_clear_weak_pointer (&self->separator);
    }

  /* Workaround for https://bugzilla.gnome.org/show_bug.cgi?id=692744
   * Without it, StBoxLayout may use a stale size cache */
  clutter_actor_queue_relayout (self->box);

  for (i = 0; i < added_items->len; i++)
    g_object_unref (g_array_index (added_items, AddedItem, i).item);
}

static void
dash_redisplay_work_cb (gpointer user_data)
{
  dash_redisplay (user_data);
}

/**
 * shell_dash_set_max_size:
 * @self: a #ShellDash
 * @max_width: the width the dash may take, in pixels
 * @max_height: the height the dash may take, in pixels
 *
 * Sets the size the icons have to fit in (`setMaxSize ()`).
 */
void
shell_dash_set_max_size (ShellDash *self,
                         int        max_width,
                         int        max_height)
{
  g_return_if_fail (SHELL_IS_DASH (self));

  if (self->max_width == max_width && self->max_height == max_height)
    return;

  self->max_width = max_width;
  self->max_height = max_height;
  dash_queue_redisplay (self);
}

static ShellDragMotionResult
dash_handle_drag_over (ShellDash    *self,
                       GObject      *source,
                       ClutterActor *actor,
                       float         x)
{
  ShellApp *app = shell_dash_get_app_from_source (source);
  ShellAppFavorites *app_favorites = shell_app_favorites_get_default ();
  g_autoptr (GList) favorites = NULL;
  g_autoptr (GList) children = NULL;
  int num_favorites;
  int fav_pos;
  int num_children;
  float box_width;
  int pos;

  /* Don't allow favoriting of transient apps */
  if (app == NULL || shell_app_is_window_backed (app))
    return SHELL_DRAG_MOTION_RESULT_NO_DROP;

  if (!g_settings_is_writable (shell_global_get_settings (shell_global_get ()), "favorite-apps"))
    return SHELL_DRAG_MOTION_RESULT_NO_DROP;

  favorites = shell_app_favorites_get_favorites (app_favorites);
  num_favorites = (int) g_list_length (favorites);

  fav_pos = g_list_index (favorites, app);

  children = clutter_actor_get_children (self->box);
  num_children = (int) g_list_length (children);
  box_width = clutter_actor_get_width (self->box);

  /* Keep the placeholder out of the index calculation; assuming that
   * the remove target has the same size as "normal" items, we don't
   * need to do the same adjustment there. */
  if (self->drag_placeholder != NULL)
    {
      box_width -= clutter_actor_get_width (CLUTTER_ACTOR (self->drag_placeholder));
      num_children--;
    }

  /* Same with the separator */
  if (self->separator != NULL)
    {
      box_width -= clutter_actor_get_width (self->separator);
      num_children--;
    }

  if (self->empty_drop_target != NULL)
    pos = 0; /* always insert at the start when dash is empty */
  else if (box_width <= 0)
    pos = 0;
  else if (clutter_actor_get_text_direction (CLUTTER_ACTOR (self)) == CLUTTER_TEXT_DIRECTION_RTL)
    pos = num_children - (int) floorf (x * num_children / box_width);
  else
    pos = (int) floorf (x * num_children / box_width);

  /* Put the placeholder after the last favorite if we are not
   * in the favorites zone */
  if (pos > num_favorites)
    pos = num_favorites;
  if (pos < 0)
    pos = 0;

  if (pos != self->drag_placeholder_pos && self->animating_placeholders_count == 0)
    {
      ShellDashItemContainer *placeholder;
      ClutterActor *bin;
      gboolean fade_in;

      self->drag_placeholder_pos = pos;

      /* Don't allow positioning before or after self */
      if (fav_pos != -1 && (pos == fav_pos || pos == fav_pos + 1))
        {
          dash_clear_drag_placeholder (self);
          return SHELL_DRAG_MOTION_RESULT_CONTINUE;
        }

      /* If the placeholder already exists, we just move
       * it, but if we are adding it, expand its size in
       * an animation */
      if (self->drag_placeholder != NULL)
        {
          ShellDashItemContainer *old = self->drag_placeholder;

          g_clear_weak_pointer (&self->drag_placeholder);
          clutter_actor_destroy (CLUTTER_ACTOR (old));
          fade_in = FALSE;
        }
      else
        {
          fade_in = TRUE;
        }

      placeholder = shell_dash_item_container_new ();
      bin = g_object_new (ST_TYPE_BIN, "style-class", "placeholder", NULL);
      shell_dash_item_container_set_child (placeholder, bin);
      g_set_weak_pointer (&self->drag_placeholder, placeholder);
      clutter_actor_set_width (bin, self->icon_size);
      clutter_actor_set_height (bin, self->icon_size / 2.0f);
      clutter_actor_insert_child_at_index (self->box, CLUTTER_ACTOR (placeholder),
                                           self->drag_placeholder_pos);
      shell_dash_item_container_show (placeholder, fade_in);
    }

  if (self->drag_placeholder == NULL)
    return SHELL_DRAG_MOTION_RESULT_NO_DROP;

  return SHELL_DRAG_MOTION_RESULT_MOVE_DROP;
}

typedef struct
{
  char *id;
  int fav_pos;
  gboolean src_is_favorite;
} FavoriteChange;

static gboolean
favorite_change_later_cb (gpointer user_data)
{
  FavoriteChange *change = user_data;
  ShellAppFavorites *app_favorites = shell_app_favorites_get_default ();

  if (change->src_is_favorite)
    shell_app_favorites_move_favorite_to_pos (app_favorites, change->id, change->fav_pos);
  else
    shell_app_favorites_add_favorite_at_pos (app_favorites, change->id, change->fav_pos);

  return G_SOURCE_REMOVE;
}

static void
favorite_change_free (gpointer data)
{
  FavoriteChange *change = data;

  g_free (change->id);
  g_free (change);
}

/* Draggable target interface */
static gboolean
dash_accept_drop (ShellDash *self,
                  GObject   *source)
{
  ShellApp *app = shell_dash_get_app_from_source (source);
  ShellAppFavorites *app_favorites = shell_app_favorites_get_default ();
  g_autoptr (GList) children = NULL;
  FavoriteChange *change;
  const char *id;
  int fav_pos = 0;
  int i;
  GList *l;

  /* Don't allow favoriting of transient apps */
  if (app == NULL || shell_app_is_window_backed (app))
    return FALSE;

  if (!g_settings_is_writable (shell_global_get_settings (shell_global_get ()), "favorite-apps"))
    return FALSE;

  id = shell_app_get_id (app);

  children = clutter_actor_get_children (self->box);
  for (l = children, i = 0; l != NULL && i < self->drag_placeholder_pos; l = l->next, i++)
    {
      ShellApp *child_app;
      const char *child_id;

      if (self->drag_placeholder != NULL && l->data == self->drag_placeholder)
        continue;

      child_app = item_get_app (l->data);
      if (child_app == NULL)
        continue;

      child_id = shell_app_get_id (child_app);
      if (g_strcmp0 (child_id, id) == 0)
        continue;
      if (shell_app_favorites_is_favorite (app_favorites, child_id))
        fav_pos++;
    }

  /* No drag placeholder means we don't want to favorite the app
   * and we are dragging it to its original position */
  if (self->drag_placeholder == NULL)
    return TRUE;

  change = g_new0 (FavoriteChange, 1);
  change->id = g_strdup (id);
  change->fav_pos = fav_pos;
  change->src_is_favorite = shell_app_favorites_is_favorite (app_favorites, id);
  add_later_before_redraw (favorite_change_later_cb, change, favorite_change_free);

  return TRUE;
}

static void
shell_dash_constructed (GObject *object)
{
  ShellDash *self = SHELL_DASH (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellCtrlAltTabManager *ctrl_alt_tab_manager;
  ShellShowAppsIcon *show_apps_icon;
  ClutterActor *dash_container, *box, *background, *sizer_box;
  ShellAppFavorites *app_favorites;

  G_OBJECT_CLASS (shell_dash_parent_class)->constructed (object);

  clutter_actor_set_name (actor, "dash");
  clutter_actor_set_offscreen_redirect (actor, CLUTTER_OFFSCREEN_REDIRECT_ALWAYS);
  clutter_actor_set_layout_manager (actor, clutter_bin_layout_new ());
  clutter_actor_set_reactive (actor, TRUE);

  dash_container = g_object_new (ST_TYPE_BOX_LAYOUT,
                                 "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                 "y-expand", TRUE,
                                 NULL);
  g_set_weak_pointer (&self->dash_container, ST_BOX_LAYOUT (dash_container));

  box = g_object_new (ST_TYPE_WIDGET,
                      "clip-to-allocation", TRUE,
                      "layout-manager", g_object_new (SHELL_TYPE_DASH_ICONS_LAYOUT,
                                                      "orientation", CLUTTER_ORIENTATION_HORIZONTAL,
                                                      NULL),
                      "y-expand", TRUE,
                      NULL);
  g_set_weak_pointer (&self->box, box);
  self->drag_target = g_object_new (SHELL_TYPE_DASH_DRAG_TARGET, NULL);
  g_set_weak_pointer (&self->drag_target->dash, self);
  shell_dnd_set_drag_target (box, SHELL_DRAG_TARGET (self->drag_target));

  clutter_actor_add_child (dash_container, box);

  show_apps_icon = shell_show_apps_icon_new ();
  g_set_weak_pointer (&self->show_apps_icon, show_apps_icon);
  shell_dash_item_container_show (SHELL_DASH_ITEM_CONTAINER (show_apps_icon), FALSE);
  shell_base_icon_set_icon_size (shell_show_apps_icon_get_icon (show_apps_icon),
                                 self->icon_size, NULL);
  dash_hook_up_label (self, SHELL_DASH_ITEM_CONTAINER (show_apps_icon), NULL);
  clutter_actor_add_child (dash_container, CLUTTER_ACTOR (show_apps_icon));

  background = g_object_new (ST_TYPE_WIDGET,
                             "style-class", "dash-background",
                             NULL);
  g_set_weak_pointer (&self->background, background);

  sizer_box = clutter_actor_new ();
  clutter_actor_add_constraint (sizer_box,
                                clutter_bind_constraint_new (CLUTTER_ACTOR (shell_show_apps_icon_get_icon (show_apps_icon)),
                                                             CLUTTER_BIND_HEIGHT, 0));
  clutter_actor_add_constraint (sizer_box,
                                clutter_bind_constraint_new (dash_container,
                                                             CLUTTER_BIND_WIDTH, 0));
  clutter_actor_add_child (background, sizer_box);

  clutter_actor_add_child (actor, background);
  clutter_actor_add_child (actor, dash_container);

  self->work = shell_deferred_work_new (box, dash_redisplay_work_cb, self);

  self->app_system = shell_app_system_get_default ();
  g_signal_connect_object (self->app_system, "installed-changed",
                           G_CALLBACK (queue_redisplay_cb), self, 0);
  g_signal_connect_object (self->app_system, "app-state-changed",
                           G_CALLBACK (app_state_changed_cb), self, 0);

  app_favorites = shell_app_favorites_get_default ();
  g_signal_connect_object (app_favorites, "changed",
                           G_CALLBACK (queue_redisplay_cb), self, 0);

  if (self->window_manager != NULL)
    {
      g_signal_connect_object (self->window_manager, "item-drag-begin",
                               G_CALLBACK (dash_item_drag_begin_cb), self, 0);
      g_signal_connect_object (self->window_manager, "item-drag-end",
                               G_CALLBACK (dash_item_drag_end_cb), self, 0);
      g_signal_connect_object (self->window_manager, "item-drag-cancelled",
                               G_CALLBACK (dash_item_drag_cancelled_cb), self, 0);

      ctrl_alt_tab_manager = shell_window_manager_get_ctrl_alt_tab_manager (self->window_manager);
      if (ctrl_alt_tab_manager != NULL)
        shell_ctrl_alt_tab_manager_add_group (ctrl_alt_tab_manager, actor,
                                              /* Translators: this is the name of the dock/favorites area on
                                                 the bottom of the overview */
                                              _("Dash"), "shell-focus-dash-symbolic");
    }
}

static void
shell_dash_destroy (ClutterActor *actor)
{
  ShellDash *self = SHELL_DASH (actor);

  dash_remove_item_drag_monitor (self);
  g_clear_handle_id (&self->show_label_timeout_id, g_source_remove);
  g_clear_handle_id (&self->reset_hover_timeout_id, g_source_remove);

  CLUTTER_ACTOR_CLASS (shell_dash_parent_class)->destroy (actor);
}

static void
shell_dash_dispose (GObject *object)
{
  ShellDash *self = SHELL_DASH (object);

  dash_remove_item_drag_monitor (self);
  g_clear_handle_id (&self->show_label_timeout_id, g_source_remove);
  g_clear_handle_id (&self->reset_hover_timeout_id, g_source_remove);

  g_clear_object (&self->drag_target);

  G_OBJECT_CLASS (shell_dash_parent_class)->dispose (object);

  g_clear_weak_pointer (&self->window_manager);
  g_clear_weak_pointer (&self->separator);
  g_clear_weak_pointer (&self->drag_placeholder);
  g_clear_weak_pointer (&self->pending_label_item);
  g_clear_weak_pointer (&self->dash_container);
  g_clear_weak_pointer (&self->box);
  g_clear_weak_pointer (&self->show_apps_icon);
  g_clear_weak_pointer (&self->background);
  g_clear_weak_pointer (&self->empty_drop_target);
}

static void
shell_dash_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  ShellDash *self = SHELL_DASH (object);

  switch (prop_id)
    {
    case DASH_PROP_WINDOW_MANAGER:
      g_clear_weak_pointer (&self->window_manager);
      g_set_weak_pointer (&self->window_manager, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_dash_get_property (GObject    *object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  ShellDash *self = SHELL_DASH (object);

  switch (prop_id)
    {
    case DASH_PROP_WINDOW_MANAGER:
      g_value_set_object (value, self->window_manager);
      break;

    case DASH_PROP_ICON_SIZE:
      g_value_set_int (value, self->icon_size);
      break;

    case DASH_PROP_SHOW_APPS_BUTTON:
      g_value_set_object (value, self->show_apps_icon != NULL
                                 ? shell_show_apps_icon_get_toggle_button (self->show_apps_icon)
                                 : NULL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_dash_class_init (ShellDashClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  ShellHookRegistry *registry;

  object_class->constructed = shell_dash_constructed;
  object_class->dispose = shell_dash_dispose;
  object_class->set_property = shell_dash_set_property;
  object_class->get_property = shell_dash_get_property;

  actor_class->destroy = shell_dash_destroy;

  /**
   * ShellDash:window-manager:
   *
   * The window manager the overview is reached through, which also
   * provides the Ctrl+Alt+Tab manager the dash adds itself to.
   */
  dash_props[DASH_PROP_WINDOW_MANAGER] =
    g_param_spec_object ("window-manager", NULL, NULL,
                         SHELL_TYPE_WINDOW_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDash:icon-size:
   *
   * The size of the application icons, in logical pixels (`iconSize`).
   */
  dash_props[DASH_PROP_ICON_SIZE] =
    g_param_spec_int ("icon-size", NULL, NULL,
                      1, G_MAXINT, 64,
                      G_PARAM_READABLE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellDash:show-apps-button:
   *
   * The button that shows the application grid (`showAppsButton`).
   */
  dash_props[DASH_PROP_SHOW_APPS_BUTTON] =
    g_param_spec_object ("show-apps-button", NULL, NULL,
                         ST_TYPE_BUTTON,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, DASH_N_PROPS, dash_props);

  /**
   * ShellDash::icon-size-changed:
   * @self: the #ShellDash
   *
   * Emitted when the size of the application icons changed.
   */
  dash_signals[DASH_ICON_SIZE_CHANGED] =
    g_signal_new ("icon-size-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /* Lua extensions (gnome-lua): the hook point of the dash */
  registry = shell_hook_registry_get_default ();
  shell_hook_registry_declare (registry, "dash.item-added",
                               "kind:s owner:s index:i app-id:s", FALSE,
                               "An item was added to the dash");
}

static void
shell_dash_init (ShellDash *self)
{
  self->max_width = -1;
  self->max_height = -1;
  self->icon_size = 64;
  self->drag_placeholder_pos = -1;
}

/**
 * shell_dash_new:
 * @window_manager: (nullable): the window manager the overview is reached
 *   through
 *
 * Creates the dash (`new Dash ()`).
 *
 * Returns: (transfer full): a new #ShellDash
 */
ShellDash *
shell_dash_new (ShellWindowManager *window_manager)
{
  return g_object_new (SHELL_TYPE_DASH,
                       "window-manager", window_manager,
                       NULL);
}
