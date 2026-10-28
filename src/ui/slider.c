/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * slider.c: A draggable, focusable bar level with a handle. Port of
 * js/ui/slider.js.
 */

#include "config.h"

#include <math.h>

#include <atk/atk.h>
#include <cairo.h>

#include "ui/slider.h"

#define SLIDER_SCROLL_STEP 0.02 /* Slider scrolling step in % */
#define SNAP_THRESHOLD 0.04 /* Snap to marks within 4% */

enum
{
  DRAG_BEGIN,
  DRAG_END,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellSlider
{
  ShellBarLevel parent;

  ClutterGrab *grab;
  ClutterPanGesture *pan_gesture;

  double handle_radius;

  GArray *marks;
  gboolean has_unsnapped_value;
  double unsnapped_value;
};

G_DEFINE_FINAL_TYPE (ShellSlider, shell_slider, SHELL_TYPE_BAR_LEVEL)

static gboolean
is_rtl (ShellSlider *self)
{
  return clutter_actor_get_text_direction (CLUTTER_ACTOR (self)) ==
         CLUTTER_TEXT_DIRECTION_RTL;
}

static double
snap_to_mark (ShellSlider *self,
              double       value)
{
  unsigned int i;

  for (i = 0; i < self->marks->len; i++)
    {
      double mark = g_array_index (self->marks, double, i);

      if (fabs (value - mark) < SNAP_THRESHOLD)
        return mark;
    }

  return value;
}

static gboolean
apply_delta (ShellSlider *self,
             double       delta)
{
  ShellBarLevel *bar_level = SHELL_BAR_LEVEL (self);
  double old_value = shell_bar_level_get_value (bar_level);
  double base;

  /* Track unsnapped value to allow escaping snap zones when scrolling/using
   * arrow keys. Without this, if the scroll step is smaller than the snap
   * threshold, the slider gets stuck at the mark because each scroll snaps
   * back.
   */
  base = self->has_unsnapped_value ? self->unsnapped_value : old_value;
  self->has_unsnapped_value = TRUE;
  self->unsnapped_value = CLAMP (base + delta, 0,
                                 shell_bar_level_get_maximum_value (bar_level));
  shell_bar_level_set_value (bar_level,
                             snap_to_mark (self, self->unsnapped_value));

  return shell_bar_level_get_value (bar_level) != old_value;
}

static void
move_handle (ShellSlider *self,
             double       x)
{
  ShellBarLevel *bar_level = SHELL_BAR_LEVEL (self);
  ClutterActorBox box;
  double width;
  double rel_x = x;
  double new_value;

  clutter_actor_get_allocation_box (CLUTTER_ACTOR (self), &box);
  width = clutter_actor_box_get_width (&box);

  if (is_rtl (self))
    rel_x = width - rel_x;

  if (rel_x < self->handle_radius)
    new_value = 0;
  else if (rel_x > width - self->handle_radius)
    new_value = 1;
  else
    new_value = (rel_x - self->handle_radius) /
                (width - 2 * self->handle_radius);

  self->has_unsnapped_value = TRUE;
  self->unsnapped_value =
    new_value * shell_bar_level_get_maximum_value (bar_level);
  shell_bar_level_set_value (bar_level,
                             snap_to_mark (self, self->unsnapped_value));
}

static gboolean
move_left_binding_cb (GObject             *object,
                      const char          *action_name,
                      guint                key_val,
                      ClutterModifierType  modifiers,
                      gpointer             user_data)
{
  ShellSlider *self = SHELL_SLIDER (object);

  apply_delta (self, is_rtl (self) ? 0.1 : -0.1);
  return CLUTTER_EVENT_STOP;
}

static gboolean
move_right_binding_cb (GObject             *object,
                       const char          *action_name,
                       guint                key_val,
                       ClutterModifierType  modifiers,
                       gpointer             user_data)
{
  ShellSlider *self = SHELL_SLIDER (object);

  apply_delta (self, is_rtl (self) ? -0.1 : 0.1);
  return CLUTTER_EVENT_STOP;
}

static void
dismiss_grab (ShellSlider *self)
{
  if (!self->grab)
    return;

  clutter_grab_dismiss (self->grab);
  g_clear_object (&self->grab);
}

static void
pan_recognize_cb (ClutterPanGesture *gesture,
                  ShellSlider       *self)
{
  ClutterActor *stage = clutter_actor_get_stage (CLUTTER_ACTOR (self));
  graphene_point_t coords;

  dismiss_grab (self);

  if (stage)
    self->grab = clutter_stage_grab (CLUTTER_STAGE (stage),
                                     CLUTTER_ACTOR (self));

  /* We need to emit 'drag-begin' before moving the handle to make
   * sure that no 'notify::value' signal is emitted before this one.
   */
  g_signal_emit (self, signals[DRAG_BEGIN], 0);

  clutter_pan_gesture_get_centroid (gesture, &coords);
  move_handle (self, coords.x);
}

static void
pan_update_cb (ClutterPanGesture *gesture,
               ShellSlider       *self)
{
  graphene_point_t coords;

  clutter_pan_gesture_get_centroid (gesture, &coords);
  move_handle (self, coords.x);
}

static void
pan_end_cb (ClutterPanGesture *gesture,
            ShellSlider       *self)
{
  dismiss_grab (self);

  g_signal_emit (self, signals[DRAG_END], 0);
}

static void
smooth_scroll_cb (ClutterScrollController *controller,
                  ClutterSprite           *sprite,
                  ClutterScrollSource      source,
                  double                   dx,
                  double                   dy,
                  ShellSlider             *self)
{
  double n_steps = dx;

  if (is_rtl (self))
    n_steps *= -1;

  shell_slider_step (self, n_steps);
}

static void
discrete_scroll_cb (ClutterScrollController *controller,
                    ClutterSprite           *sprite,
                    ClutterScrollSource      source,
                    double                   dx,
                    double                   dy,
                    ShellSlider             *self)
{
  shell_slider_step (self, -dy);
}

static double
accessible_get_minimum_increment_cb (StGenericAccessible *accessible,
                                     ShellSlider         *self)
{
  return 0.1;
}

static void
shell_slider_style_changed (StWidget *widget)
{
  ShellSlider *self = SHELL_SLIDER (widget);
  StThemeNode *theme_node;

  ST_WIDGET_CLASS (shell_slider_parent_class)->style_changed (widget);

  theme_node = st_widget_get_theme_node (widget);
  self->handle_radius =
    floor (2 * st_theme_node_get_length (theme_node,
                                         "-slider-handle-radius") + 0.5) / 2;
}

static void
shell_slider_repaint (StDrawingArea *area)
{
  ShellSlider *self = SHELL_SLIDER (area);
  ShellBarLevel *bar_level = SHELL_BAR_LEVEL (area);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (area));
  cairo_t *cr;
  unsigned int surface_width, surface_height;
  double width, height;
  double handle_x, handle_y;
  CoglColor color;

  ST_DRAWING_AREA_CLASS (shell_slider_parent_class)->repaint (area);

  /* Add handle */
  cr = st_drawing_area_get_context (area);
  st_drawing_area_get_surface_size (area, &surface_width, &surface_height);
  width = surface_width;
  height = surface_height;

  handle_y = height / 2;

  handle_x = self->handle_radius +
    (width - 2 * self->handle_radius) *
    shell_bar_level_get_value (bar_level) /
    shell_bar_level_get_maximum_value (bar_level);
  if (is_rtl (self))
    handle_x = width - handle_x;

  st_theme_node_get_foreground_color (theme_node, &color);
  cairo_set_source_rgba (cr,
                         color.red / 255.,
                         color.green / 255.,
                         color.blue / 255.,
                         color.alpha / 255.);
  cairo_arc (cr, handle_x, handle_y, self->handle_radius, 0, 2 * G_PI);
  cairo_fill (cr);
}

static double
shell_slider_get_preferred_bar_height (ShellBarLevel *bar_level)
{
  ShellSlider *self = SHELL_SLIDER (bar_level);
  double bar_height =
    SHELL_BAR_LEVEL_CLASS (shell_slider_parent_class)->get_preferred_bar_height (bar_level);
  double handle_height = 2 * self->handle_radius;

  return MAX (bar_height, handle_height);
}

static double
shell_slider_get_preferred_bar_width (ShellBarLevel *bar_level)
{
  ShellSlider *self = SHELL_SLIDER (bar_level);
  double bar_width =
    SHELL_BAR_LEVEL_CLASS (shell_slider_parent_class)->get_preferred_bar_width (bar_level);
  double handle_width = 2 * self->handle_radius;

  return MAX (bar_width, handle_width);
}

static void
shell_slider_dispose (GObject *object)
{
  ShellSlider *self = SHELL_SLIDER (object);

  dismiss_grab (self);

  G_OBJECT_CLASS (shell_slider_parent_class)->dispose (object);
}

static void
shell_slider_finalize (GObject *object)
{
  ShellSlider *self = SHELL_SLIDER (object);

  g_clear_pointer (&self->marks, g_array_unref);

  G_OBJECT_CLASS (shell_slider_parent_class)->finalize (object);
}

static void
shell_slider_class_init (ShellSliderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);
  StDrawingAreaClass *drawing_area_class = ST_DRAWING_AREA_CLASS (klass);
  ShellBarLevelClass *bar_level_class = SHELL_BAR_LEVEL_CLASS (klass);
  ClutterBindingPool *binding_pool;

  object_class->dispose = shell_slider_dispose;
  object_class->finalize = shell_slider_finalize;

  widget_class->style_changed = shell_slider_style_changed;

  drawing_area_class->repaint = shell_slider_repaint;

  bar_level_class->get_preferred_bar_height = shell_slider_get_preferred_bar_height;
  bar_level_class->get_preferred_bar_width = shell_slider_get_preferred_bar_width;

  /**
   * ShellSlider::drag-begin:
   *
   * Emitted when the user starts dragging the handle, before the value
   * is changed for the first time.
   */
  signals[DRAG_BEGIN] =
    g_signal_new ("drag-begin",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellSlider::drag-end:
   *
   * Emitted when the user stops dragging the handle.
   */
  signals[DRAG_END] =
    g_signal_new ("drag-end",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  binding_pool = clutter_binding_pool_get_for_class (klass);

  clutter_binding_pool_install_action (binding_pool,
                                       "left",
                                       CLUTTER_KEY_Left,
                                       0,
                                       G_CALLBACK (move_left_binding_cb),
                                       NULL,
                                       NULL);
  clutter_binding_pool_install_action (binding_pool,
                                       "right",
                                       CLUTTER_KEY_Right,
                                       0,
                                       G_CALLBACK (move_right_binding_cb),
                                       NULL,
                                       NULL);
}

static void
shell_slider_init (ShellSlider *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  StWidget *widget = ST_WIDGET (self);
  ClutterAction *smooth_scroll_controller, *discrete_scroll_controller;
  AtkObject *accessible;

  st_widget_set_style_class_name (widget, "slider");
  st_widget_set_can_focus (widget, TRUE);
  clutter_actor_set_reactive (actor, TRUE);
  st_widget_set_track_hover (widget, TRUE);
  st_widget_set_hover (widget, FALSE);
  clutter_actor_set_accessible_role (actor, ATK_ROLE_SLIDER);
  clutter_actor_set_x_expand (actor, TRUE);

  self->handle_radius = 0;

  self->pan_gesture = CLUTTER_PAN_GESTURE (clutter_pan_gesture_new ());
  clutter_pan_gesture_set_begin_threshold (self->pan_gesture, 0);
  clutter_actor_meta_set_name (CLUTTER_ACTOR_META (self->pan_gesture),
                               "ShellSlider pan gesture");
  g_signal_connect (self->pan_gesture, "recognize",
                    G_CALLBACK (pan_recognize_cb), self);
  g_signal_connect (self->pan_gesture, "pan-update",
                    G_CALLBACK (pan_update_cb), self);
  g_signal_connect (self->pan_gesture, "end",
                    G_CALLBACK (pan_end_cb), self);
  clutter_actor_add_action (actor, CLUTTER_ACTION (self->pan_gesture));

  smooth_scroll_controller =
    clutter_scroll_controller_new (CLUTTER_SCROLL_CONTROLLER_FLAG_PHYSICAL_DIRECTION |
                                   CLUTTER_SCROLL_CONTROLLER_FLAG_SCROLL_HORIZONTAL);
  g_signal_connect (smooth_scroll_controller, "scroll",
                    G_CALLBACK (smooth_scroll_cb), self);
  clutter_actor_add_action (actor, smooth_scroll_controller);

  discrete_scroll_controller =
    clutter_scroll_controller_new (CLUTTER_SCROLL_CONTROLLER_FLAG_DISCRETE |
                                   CLUTTER_SCROLL_CONTROLLER_FLAG_SCROLL_VERTICAL);
  g_signal_connect (discrete_scroll_controller, "scroll",
                    G_CALLBACK (discrete_scroll_cb), self);
  clutter_actor_add_action (actor, discrete_scroll_controller);

  accessible = clutter_actor_get_accessible (actor);
  g_signal_connect_object (accessible, "get-minimum-increment",
                           G_CALLBACK (accessible_get_minimum_increment_cb),
                           self, 0);

  self->marks = g_array_new (FALSE, FALSE, sizeof (double));
  self->has_unsnapped_value = FALSE;
  self->unsnapped_value = 0;
}

/**
 * shell_slider_new:
 * @value: the initial value
 *
 * Creates a new #ShellSlider.
 *
 * Returns: a new #ShellSlider
 */
ShellSlider *
shell_slider_new (double value)
{
  return g_object_new (SHELL_TYPE_SLIDER,
                       "value", value,
                       NULL);
}

/**
 * shell_slider_add_mark:
 * @self: a #ShellSlider
 * @value: the value of the mark
 *
 * Adds a mark the handle snaps to when it is moved within 4% of it.
 */
void
shell_slider_add_mark (ShellSlider *self,
                       double       value)
{
  unsigned int i;

  g_return_if_fail (SHELL_IS_SLIDER (self));

  for (i = 0; i < self->marks->len; i++)
    {
      if (g_array_index (self->marks, double, i) == value)
        return;
    }

  g_array_append_val (self->marks, value);
}

/**
 * shell_slider_clear_marks:
 * @self: a #ShellSlider
 *
 * Removes all marks added with shell_slider_add_mark().
 */
void
shell_slider_clear_marks (ShellSlider *self)
{
  g_return_if_fail (SHELL_IS_SLIDER (self));

  g_array_set_size (self->marks, 0);
}

/**
 * shell_slider_step:
 * @self: a #ShellSlider
 * @n_steps: the number of steps to move the handle, negative to move it
 *   towards zero
 *
 * Moves the handle by @n_steps scroll steps (2% of the range each).
 *
 * Returns: %TRUE if the value changed
 */
gboolean
shell_slider_step (ShellSlider *self,
                   double       n_steps)
{
  g_return_val_if_fail (SHELL_IS_SLIDER (self), FALSE);

  return apply_delta (self, n_steps * SLIDER_SCROLL_STEP);
}
