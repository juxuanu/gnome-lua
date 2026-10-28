/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * pointer-a11y-timeout.c: Visual feedback (a "pie timer" drawn under the
 * pointer) for the pointer accessibility timeouts (simulated secondary
 * click, dwell click and dwell gesture). Port of js/ui/pointerA11yTimeout.js.
 */

#include "config.h"

#include <math.h>

#include <cairo.h>

#include "shell-global.h"
#include "st.h"
#include "ui/environment.h"
#include "ui/pointer-a11y-timeout.h"

#define SUCCESS_ZOOM_OUT_DURATION 150

/* ------------------------------------------------------------------------ */
/* ShellPieTimer: the actor showing the remaining time. Private to this      */
/* file, it is not part of the public API.                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_PIE_TIMER (shell_pie_timer_get_type ())
G_DECLARE_FINAL_TYPE (ShellPieTimer, shell_pie_timer,
                      SHELL, PIE_TIMER, StDrawingArea)

struct _ShellPieTimer
{
  StDrawingArea parent;

  double angle;
};

G_DEFINE_FINAL_TYPE (ShellPieTimer, shell_pie_timer, ST_TYPE_DRAWING_AREA)

enum
{
  PIE_PROP_0,

  PIE_PROP_ANGLE,

  PIE_N_PROPS
};

static GParamSpec *pie_props[PIE_N_PROPS] = { NULL, };

static void
set_source_color (cairo_t         *cr,
                  const CoglColor *color)
{
  cairo_set_source_rgba (cr,
                         color->red / 255.,
                         color->green / 255.,
                         color->blue / 255.,
                         color->alpha / 255.);
}

static void
shell_pie_timer_set_angle (ShellPieTimer *self,
                           double         angle)
{
  if (self->angle == angle)
    return;

  self->angle = angle;
  g_object_notify_by_pspec (G_OBJECT (self), pie_props[PIE_PROP_ANGLE]);
  st_drawing_area_queue_repaint (ST_DRAWING_AREA (self));
}

static void
shell_pie_timer_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  ShellPieTimer *self = SHELL_PIE_TIMER (object);

  switch (prop_id)
    {
    case PIE_PROP_ANGLE:
      g_value_set_double (value, self->angle);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_pie_timer_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  ShellPieTimer *self = SHELL_PIE_TIMER (object);

  switch (prop_id)
    {
    case PIE_PROP_ANGLE:
      shell_pie_timer_set_angle (self, g_value_get_double (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_pie_timer_repaint (StDrawingArea *area)
{
  ShellPieTimer *self = SHELL_PIE_TIMER (area);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (area));
  cairo_t *cr = st_drawing_area_get_context (area);
  CoglColor background_color, border_color;
  unsigned int surface_width, surface_height;
  double width, height, radius, border_width;
  double start_angle, end_angle;

  st_theme_node_get_color (theme_node, "-pie-background-color",
                           &background_color);
  st_theme_node_get_color (theme_node, "-pie-border-color", &border_color);
  border_width = st_theme_node_get_length (theme_node, "-pie-border-width");

  st_drawing_area_get_surface_size (area, &surface_width, &surface_height);
  width = surface_width;
  height = surface_height;
  radius = MIN (width / 2, height / 2);

  start_angle = 3 * G_PI / 2;
  end_angle = start_angle + self->angle;

  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  cairo_translate (cr, width / 2, height / 2);

  if (self->angle < 2 * G_PI)
    cairo_move_to (cr, 0, 0);

  cairo_arc (cr, 0, 0, radius - border_width, start_angle, end_angle);

  if (self->angle < 2 * G_PI)
    cairo_line_to (cr, 0, 0);

  cairo_close_path (cr);

  cairo_set_line_width (cr, 0);
  set_source_color (cr, &background_color);
  cairo_fill_preserve (cr);

  cairo_set_line_width (cr, border_width);
  set_source_color (cr, &border_color);
  cairo_stroke (cr);
}

static void
shell_pie_timer_class_init (ShellPieTimerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StDrawingAreaClass *drawing_area_class = ST_DRAWING_AREA_CLASS (klass);

  object_class->get_property = shell_pie_timer_get_property;
  object_class->set_property = shell_pie_timer_set_property;

  drawing_area_class->repaint = shell_pie_timer_repaint;

  pie_props[PIE_PROP_ANGLE] =
    g_param_spec_double ("angle", NULL, NULL,
                         0, 2 * G_PI, 0,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PIE_N_PROPS, pie_props);
}

static void
shell_pie_timer_init (ShellPieTimer *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  self->angle = 0;

  st_widget_set_style_class_name (ST_WIDGET (self), "pie-timer");
  clutter_actor_set_opacity (actor, 0);
  clutter_actor_hide (actor);
  st_widget_set_can_focus (ST_WIDGET (self), FALSE);
  clutter_actor_set_reactive (actor, FALSE);

  clutter_actor_set_pivot_point (actor, 0.5, 0.5);
}

static void
pie_timer_zoom_out_done_cb (gboolean finished,
                            gpointer user_data)
{
  /* onStopped: runs whether the transition finished or was interrupted */
  clutter_actor_destroy (CLUTTER_ACTOR (user_data));
}

static void
pie_timer_countdown_done_cb (gboolean finished,
                             gpointer user_data)
{
  ClutterActor *actor = user_data;
  ShellEaseParams params = {
    .duration = SUCCESS_ZOOM_OUT_DURATION,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  /* onComplete: only when the countdown ran to the end */
  if (!finished)
    return;

  shell_actor_ease (actor, &params,
                    pie_timer_zoom_out_done_cb, actor, NULL,
                    "scale-x", 2.0,
                    "scale-y", 2.0,
                    "opacity", 0u,
                    NULL);
}

/* Note that the countdown can complete (and destroy @self) before this
 * returns, when animations are disabled. */
static void
shell_pie_timer_start (ShellPieTimer *self,
                       float          x,
                       float          y,
                       guint          duration)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  g_auto (GValue) target = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;
  ShellEaseParams fade_params = {
    .duration = duration / 4,
    .delay = -1,
    .mode = CLUTTER_EASE_IN_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
  ShellEaseParams angle_params = {
    .duration = duration,
    .delay = -1,
    .mode = CLUTTER_LINEAR,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  clutter_actor_set_position (actor,
                              x - clutter_actor_get_width (actor) / 2,
                              y - clutter_actor_get_height (actor) / 2);
  clutter_actor_show (actor);

  shell_actor_ease (actor, &fade_params, NULL, NULL, NULL,
                    "opacity", 255u,
                    NULL);

  g_value_init (&target, G_TYPE_DOUBLE);
  g_value_set_double (&target, 2 * G_PI);

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (actor), "angle",
                                       &target, &angle_params,
                                       pie_timer_countdown_done_cb, actor, NULL,
                                       &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

/* ------------------------------------------------------------------------ */
/* ShellPointerA11yTimeout                                                   */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_UI_GROUP,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellPointerA11yTimeout
{
  GObject parent;

  ClutterActor *ui_group;   /* weak */
  ClutterActor *pie_timer;  /* weak, owned by ui_group */
};

G_DEFINE_FINAL_TYPE (ShellPointerA11yTimeout, shell_pointer_a11y_timeout,
                     G_TYPE_OBJECT)

static void
on_pie_timer_destroy (ClutterActor            *actor,
                      ShellPointerA11yTimeout *self)
{
  /* a newer timer may have replaced the one that is going away */
  if (self->pie_timer == actor)
    self->pie_timer = NULL;
}

static void
on_timeout_started (ClutterSeat                 *seat,
                    ClutterPointerA11yTimeoutType type,
                    guint                         timeout,
                    ShellPointerA11yTimeout      *self)
{
  ShellGlobal *global = shell_global_get ();
  ShellPieTimer *pie_timer;
  int x, y;

  shell_global_get_pointer (global, &x, &y, NULL);

  pie_timer = g_object_new (SHELL_TYPE_PIE_TIMER, NULL);
  self->pie_timer = CLUTTER_ACTOR (pie_timer);
  g_signal_connect_object (pie_timer, "destroy",
                           G_CALLBACK (on_pie_timer_destroy), self, 0);

  clutter_actor_add_child (self->ui_group, self->pie_timer);
  clutter_actor_set_child_above_sibling (self->ui_group, self->pie_timer, NULL);

  /* may destroy the timer before returning, do not touch it afterwards */
  shell_pie_timer_start (pie_timer, x, y, timeout);

  if (type == CLUTTER_A11Y_TIMEOUT_TYPE_GESTURE)
    {
      clutter_actor_set_cursor_type (CLUTTER_ACTOR (shell_global_get_stage (global)),
                                     CLUTTER_CURSOR_CROSSHAIR);
    }
}

static void
on_timeout_stopped (ClutterSeat                  *seat,
                    ClutterPointerA11yTimeoutType type,
                    gboolean                      clicked,
                    ShellPointerA11yTimeout      *self)
{
  if (!clicked && self->pie_timer != NULL)
    clutter_actor_destroy (self->pie_timer);

  if (type == CLUTTER_A11Y_TIMEOUT_TYPE_GESTURE)
    {
      ShellGlobal *global = shell_global_get ();

      clutter_actor_set_cursor_type (CLUTTER_ACTOR (shell_global_get_stage (global)),
                                     CLUTTER_CURSOR_DEFAULT);
    }
}

static void
shell_pointer_a11y_timeout_constructed (GObject *object)
{
  ShellPointerA11yTimeout *self = SHELL_POINTER_A11Y_TIMEOUT (object);
  ShellGlobal *global = shell_global_get ();
  ClutterStage *stage = shell_global_get_stage (global);
  ClutterContext *context = clutter_actor_get_context (CLUTTER_ACTOR (stage));
  ClutterBackend *backend = clutter_context_get_backend (context);
  ClutterSeat *seat = clutter_backend_get_default_seat (backend);

  G_OBJECT_CLASS (shell_pointer_a11y_timeout_parent_class)->constructed (object);

  if (self->ui_group == NULL)
    g_set_weak_pointer (&self->ui_group, CLUTTER_ACTOR (stage));

  g_signal_connect_object (seat, "ptr-a11y-timeout-started",
                           G_CALLBACK (on_timeout_started), self, 0);
  g_signal_connect_object (seat, "ptr-a11y-timeout-stopped",
                           G_CALLBACK (on_timeout_stopped), self, 0);
}

static void
shell_pointer_a11y_timeout_dispose (GObject *object)
{
  ShellPointerA11yTimeout *self = SHELL_POINTER_A11Y_TIMEOUT (object);

  if (self->pie_timer != NULL)
    clutter_actor_destroy (self->pie_timer);
  g_clear_weak_pointer (&self->pie_timer);
  g_clear_weak_pointer (&self->ui_group);

  G_OBJECT_CLASS (shell_pointer_a11y_timeout_parent_class)->dispose (object);
}

static void
shell_pointer_a11y_timeout_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  ShellPointerA11yTimeout *self = SHELL_POINTER_A11Y_TIMEOUT (object);

  switch (prop_id)
    {
    case PROP_UI_GROUP:
      g_value_set_object (value, self->ui_group);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_pointer_a11y_timeout_set_property (GObject      *object,
                                         guint         prop_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
  ShellPointerA11yTimeout *self = SHELL_POINTER_A11Y_TIMEOUT (object);

  switch (prop_id)
    {
    case PROP_UI_GROUP:
      g_set_weak_pointer (&self->ui_group, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_pointer_a11y_timeout_class_init (ShellPointerA11yTimeoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_pointer_a11y_timeout_constructed;
  object_class->dispose = shell_pointer_a11y_timeout_dispose;
  object_class->get_property = shell_pointer_a11y_timeout_get_property;
  object_class->set_property = shell_pointer_a11y_timeout_set_property;

  /**
   * ShellPointerA11yTimeout:ui-group:
   *
   * The actor the pie timer is added to, on top of its other children
   * (`Main.uiGroup`). The stage is used when it is not given.
   */
  props[PROP_UI_GROUP] =
    g_param_spec_object ("ui-group", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_pointer_a11y_timeout_init (ShellPointerA11yTimeout *self)
{
}

/**
 * shell_pointer_a11y_timeout_new:
 * @ui_group: (nullable): the actor the pie timer is added to
 *
 * Starts listening to the pointer accessibility timeouts of the default
 * seat. The feedback stops when the returned object is destroyed, so the
 * caller has to keep it alive.
 *
 * Returns: (transfer full): a new #ShellPointerA11yTimeout
 */
ShellPointerA11yTimeout *
shell_pointer_a11y_timeout_new (ClutterActor *ui_group)
{
  g_return_val_if_fail (ui_group == NULL || CLUTTER_IS_ACTOR (ui_group), NULL);

  return g_object_new (SHELL_TYPE_POINTER_A11Y_TIMEOUT,
                       "ui-group", ui_group,
                       NULL);
}
