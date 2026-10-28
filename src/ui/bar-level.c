/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * bar-level.c: A horizontal bar showing a value between 0 and a maximum,
 * with an optional "overdrive" zone. Port of js/ui/barLevel.js.
 */

#include "config.h"

#include <math.h>

#include <atk/atk.h>
#include <cairo.h>

#include "ui/bar-level.h"

enum
{
  PROP_0,

  PROP_VALUE,
  PROP_MAXIMUM_VALUE,
  PROP_OVERDRIVE_START,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

typedef struct _ShellBarLevelPrivate ShellBarLevelPrivate;

struct _ShellBarLevelPrivate
{
  double value;
  double maximum_value;
  double overdrive_start;

  double bar_height;
  double overdrive_separator_width;

  CoglColor bar_color;
  CoglColor active_color;
  CoglColor overdrive_color;

  AtkObject *accessible;
};

G_DEFINE_TYPE_WITH_PRIVATE (ShellBarLevel, shell_bar_level, ST_TYPE_DRAWING_AREA)

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

/*
 * ATK value interface, provided by StGenericAccessible's signals.
 */

static double
accessible_get_current_value_cb (StGenericAccessible *accessible,
                                 ShellBarLevel       *self)
{
  ShellBarLevelPrivate *priv = shell_bar_level_get_instance_private (self);

  return priv->value;
}

static double
accessible_get_minimum_value_cb (StGenericAccessible *accessible,
                                 ShellBarLevel       *self)
{
  return 0.0;
}

static double
accessible_get_maximum_value_cb (StGenericAccessible *accessible,
                                 ShellBarLevel       *self)
{
  ShellBarLevelPrivate *priv = shell_bar_level_get_instance_private (self);

  return priv->maximum_value;
}

static void
accessible_set_current_value_cb (StGenericAccessible *accessible,
                                 double               value,
                                 ShellBarLevel       *self)
{
  ShellBarLevelPrivate *priv = shell_bar_level_get_instance_private (self);

  /* Like the JS version, this neither clamps nor notifies. */
  priv->value = value;
}

static void
notify_value_cb (GObject    *object,
                 GParamSpec *pspec,
                 gpointer    user_data)
{
  ShellBarLevelPrivate *priv =
    shell_bar_level_get_instance_private (SHELL_BAR_LEVEL (object));

  if (priv->accessible)
    g_object_notify (G_OBJECT (priv->accessible), "accessible-value");
}

/*
 * Virtual functions
 */

static double
shell_bar_level_real_get_preferred_bar_height (ShellBarLevel *self)
{
  ShellBarLevelPrivate *priv = shell_bar_level_get_instance_private (self);

  return priv->bar_height;
}

static double
shell_bar_level_real_get_preferred_bar_width (ShellBarLevel *self)
{
  ShellBarLevelPrivate *priv = shell_bar_level_get_instance_private (self);

  return priv->overdrive_separator_width;
}

static void
shell_bar_level_get_preferred_height (ClutterActor *actor,
                                      float         for_width,
                                      float        *min_height_p,
                                      float        *natural_height_p)
{
  ShellBarLevel *self = SHELL_BAR_LEVEL (actor);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  float height =
    SHELL_BAR_LEVEL_GET_CLASS (self)->get_preferred_bar_height (self);

  st_theme_node_adjust_preferred_height (theme_node, &height, &height);

  if (min_height_p)
    *min_height_p = height;
  if (natural_height_p)
    *natural_height_p = height;
}

static void
shell_bar_level_get_preferred_width (ClutterActor *actor,
                                     float         for_height,
                                     float        *min_width_p,
                                     float        *natural_width_p)
{
  ShellBarLevel *self = SHELL_BAR_LEVEL (actor);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  float width =
    SHELL_BAR_LEVEL_GET_CLASS (self)->get_preferred_bar_width (self);

  st_theme_node_adjust_preferred_width (theme_node, &width, &width);

  if (min_width_p)
    *min_width_p = width;
  if (natural_width_p)
    *natural_width_p = width;
}

static void
shell_bar_level_style_changed (StWidget *widget)
{
  ShellBarLevel *self = SHELL_BAR_LEVEL (widget);
  ShellBarLevelPrivate *priv = shell_bar_level_get_instance_private (self);
  StThemeNode *theme_node = st_widget_get_theme_node (widget);

  priv->bar_height = st_theme_node_get_length (theme_node,
                                               "-barlevel-height");
  priv->overdrive_separator_width =
    st_theme_node_get_length (theme_node, "-barlevel-overdrive-separator-width");

  st_theme_node_get_color (theme_node, "-barlevel-background-color",
                           &priv->bar_color);
  st_theme_node_get_color (theme_node, "-barlevel-active-background-color",
                           &priv->active_color);
  st_theme_node_get_color (theme_node, "-barlevel-overdrive-color",
                           &priv->overdrive_color);

  ST_WIDGET_CLASS (shell_bar_level_parent_class)->style_changed (widget);
}

static void
shell_bar_level_repaint (StDrawingArea *area)
{
  ShellBarLevel *self = SHELL_BAR_LEVEL (area);
  ShellBarLevelPrivate *priv = shell_bar_level_get_instance_private (self);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (area));
  cairo_t *cr = st_drawing_area_get_context (area);
  unsigned int surface_width, surface_height;
  gboolean rtl;
  CoglColor fg_color;
  double width, height, bar_height;
  double border_radius;
  double end_x;
  double overdrive_ratio, overdrive_separator_x, overdrive_separator_width;
  gboolean overdrive_active;
  double arc_start, arc_end;
  double x;
  const double tau = G_PI * 2;

  st_drawing_area_get_surface_size (area, &surface_width, &surface_height);
  width = surface_width;
  height = surface_height;
  bar_height = priv->bar_height;
  rtl = clutter_actor_get_text_direction (CLUTTER_ACTOR (area)) ==
        CLUTTER_TEXT_DIRECTION_RTL;

  border_radius = MIN (width, bar_height) / 2;
  st_theme_node_get_foreground_color (theme_node, &fg_color);

  end_x = 0;
  if (priv->maximum_value > 0)
    {
      double progress = priv->value / priv->maximum_value;

      if (rtl)
        progress = 1 - progress;
      end_x = border_radius + (width - 2 * border_radius) * progress;
    }

  overdrive_ratio = priv->overdrive_start / priv->maximum_value;
  if (rtl)
    overdrive_ratio = 1 - overdrive_ratio;
  overdrive_separator_x =
    border_radius + (width - 2 * border_radius) * overdrive_ratio;

  overdrive_active = priv->overdrive_start != priv->maximum_value;
  overdrive_separator_width =
    overdrive_active ? priv->overdrive_separator_width : 0;

  arc_start = border_radius;
  arc_end = width - arc_start;
  if (rtl)
    {
      double tmp = arc_start;

      arc_start = arc_end;
      arc_end = tmp;
    }

  /* background bar */
  if (!rtl)
    cairo_arc (cr, arc_end, height / 2, border_radius,
               tau * (3. / 4), tau * (1. / 4));
  else
    cairo_arc_negative (cr, arc_end, height / 2, border_radius,
                        tau * (3. / 4), tau * (1. / 4));
  cairo_line_to (cr, end_x, (height + bar_height) / 2);
  cairo_line_to (cr, end_x, (height - bar_height) / 2);
  cairo_line_to (cr, arc_end, (height - bar_height) / 2);
  set_source_color (cr, &priv->bar_color);
  cairo_fill (cr);

  /* normal progress bar */
  if (!rtl)
    {
      x = MIN (end_x, overdrive_separator_x - overdrive_separator_width / 2);
      cairo_arc (cr, arc_start, height / 2, border_radius,
                 tau * (1. / 4), tau * (3. / 4));
    }
  else
    {
      x = MAX (end_x, overdrive_separator_x + overdrive_separator_width / 2);
      cairo_arc_negative (cr, arc_start, height / 2, border_radius,
                          tau * (1. / 4), tau * (3. / 4));
    }
  cairo_line_to (cr, x, (height - bar_height) / 2);
  cairo_line_to (cr, x, (height + bar_height) / 2);
  cairo_line_to (cr, arc_start, (height + bar_height) / 2);
  if (priv->value > 0)
    set_source_color (cr, &priv->active_color);
  cairo_fill (cr);

  /* overdrive progress barLevel */
  if (!rtl)
    x = MIN (end_x, overdrive_separator_x) + overdrive_separator_width / 2;
  else
    x = MAX (end_x, overdrive_separator_x) - overdrive_separator_width / 2;
  if (priv->value > priv->overdrive_start)
    {
      cairo_move_to (cr, x, (height - bar_height) / 2);
      cairo_line_to (cr, end_x, (height - bar_height) / 2);
      cairo_line_to (cr, end_x, (height + bar_height) / 2);
      cairo_line_to (cr, x, (height + bar_height) / 2);
      cairo_line_to (cr, x, (height - bar_height) / 2);
      set_source_color (cr, &priv->overdrive_color);
      cairo_fill (cr);
    }

  /* end progress bar arc */
  if (priv->value > 0)
    {
      if (priv->value <= priv->overdrive_start)
        set_source_color (cr, &priv->active_color);
      else
        set_source_color (cr, &priv->overdrive_color);

      if (!rtl)
        {
          cairo_arc (cr, end_x, height / 2, border_radius,
                     tau * (3. / 4), tau * (1. / 4));
          cairo_line_to (cr, floor (end_x), (height + bar_height) / 2);
          cairo_line_to (cr, floor (end_x), (height - bar_height) / 2);
        }
      else
        {
          cairo_arc_negative (cr, end_x, height / 2, border_radius,
                              tau * (3. / 4), tau * (1. / 4));
          cairo_line_to (cr, ceil (end_x), (height + bar_height) / 2);
          cairo_line_to (cr, ceil (end_x), (height - bar_height) / 2);
        }
      cairo_line_to (cr, end_x, (height - bar_height) / 2);
      cairo_fill (cr);
    }

  /* draw overdrive separator */
  if (overdrive_active)
    {
      cairo_move_to (cr, overdrive_separator_x - overdrive_separator_width / 2,
                     (height - bar_height) / 2);
      cairo_line_to (cr, overdrive_separator_x + overdrive_separator_width / 2,
                     (height - bar_height) / 2);
      cairo_line_to (cr, overdrive_separator_x + overdrive_separator_width / 2,
                     (height + bar_height) / 2);
      cairo_line_to (cr, overdrive_separator_x - overdrive_separator_width / 2,
                     (height + bar_height) / 2);
      cairo_line_to (cr, overdrive_separator_x - overdrive_separator_width / 2,
                     (height - bar_height) / 2);
      if (priv->value <= priv->overdrive_start)
        set_source_color (cr, &fg_color);
      else
        set_source_color (cr, &priv->bar_color);
      cairo_fill (cr);
    }
}

/*
 * Properties
 */

/**
 * shell_bar_level_get_value:
 * @self: a #ShellBarLevel
 *
 * Returns: the current value, between 0 and the maximum value
 */
double
shell_bar_level_get_value (ShellBarLevel *self)
{
  ShellBarLevelPrivate *priv;

  g_return_val_if_fail (SHELL_IS_BAR_LEVEL (self), 0.0);

  priv = shell_bar_level_get_instance_private (self);
  return priv->value;
}

/**
 * shell_bar_level_set_value:
 * @self: a #ShellBarLevel
 * @value: the new value, clamped to the range between 0 and the maximum value
 *
 * Sets the current value of the bar.
 */
void
shell_bar_level_set_value (ShellBarLevel *self,
                           double         value)
{
  ShellBarLevelPrivate *priv;

  g_return_if_fail (SHELL_IS_BAR_LEVEL (self));

  priv = shell_bar_level_get_instance_private (self);

  value = MAX (MIN (value, priv->maximum_value), 0);

  if (priv->value == value)
    return;

  priv->value = value;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_VALUE]);
  st_drawing_area_queue_repaint (ST_DRAWING_AREA (self));
}

/**
 * shell_bar_level_get_maximum_value:
 * @self: a #ShellBarLevel
 *
 * Returns: the maximum value
 */
double
shell_bar_level_get_maximum_value (ShellBarLevel *self)
{
  ShellBarLevelPrivate *priv;

  g_return_val_if_fail (SHELL_IS_BAR_LEVEL (self), 1.0);

  priv = shell_bar_level_get_instance_private (self);
  return priv->maximum_value;
}

/**
 * shell_bar_level_set_maximum_value:
 * @self: a #ShellBarLevel
 * @maximum_value: the new maximum value, at least 1
 *
 * Sets the maximum value of the bar. The overdrive start is lowered if it
 * would end up past the new maximum.
 */
void
shell_bar_level_set_maximum_value (ShellBarLevel *self,
                                   double         maximum_value)
{
  ShellBarLevelPrivate *priv;

  g_return_if_fail (SHELL_IS_BAR_LEVEL (self));

  priv = shell_bar_level_get_instance_private (self);

  maximum_value = MAX (maximum_value, 1);

  if (priv->maximum_value == maximum_value)
    return;

  priv->maximum_value = maximum_value;
  priv->overdrive_start = MIN (priv->overdrive_start, priv->maximum_value);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_MAXIMUM_VALUE]);
  st_drawing_area_queue_repaint (ST_DRAWING_AREA (self));
}

/**
 * shell_bar_level_get_overdrive_start:
 * @self: a #ShellBarLevel
 *
 * Returns: the value at which the overdrive zone starts
 */
double
shell_bar_level_get_overdrive_start (ShellBarLevel *self)
{
  ShellBarLevelPrivate *priv;

  g_return_val_if_fail (SHELL_IS_BAR_LEVEL (self), 1.0);

  priv = shell_bar_level_get_instance_private (self);
  return priv->overdrive_start;
}

/**
 * shell_bar_level_set_overdrive_start:
 * @self: a #ShellBarLevel
 * @overdrive_start: the value at which the overdrive zone starts, not greater
 *   than the maximum value
 *
 * Sets the value at which the overdrive zone starts. If it equals the
 * maximum value there is no overdrive zone.
 */
void
shell_bar_level_set_overdrive_start (ShellBarLevel *self,
                                     double         overdrive_start)
{
  ShellBarLevelPrivate *priv;

  g_return_if_fail (SHELL_IS_BAR_LEVEL (self));

  priv = shell_bar_level_get_instance_private (self);

  if (priv->overdrive_start == overdrive_start)
    return;

  if (overdrive_start > priv->maximum_value)
    {
      g_warning ("Tried to set overdrive value to %g, "
                 "which is a number greater than the maximum allowed value %g",
                 overdrive_start, priv->maximum_value);
      return;
    }

  priv->overdrive_start = overdrive_start;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OVERDRIVE_START]);
  st_drawing_area_queue_repaint (ST_DRAWING_AREA (self));
}

static void
shell_bar_level_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  ShellBarLevel *self = SHELL_BAR_LEVEL (object);

  switch (prop_id)
    {
    case PROP_VALUE:
      g_value_set_double (value, shell_bar_level_get_value (self));
      break;

    case PROP_MAXIMUM_VALUE:
      g_value_set_double (value, shell_bar_level_get_maximum_value (self));
      break;

    case PROP_OVERDRIVE_START:
      g_value_set_double (value, shell_bar_level_get_overdrive_start (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_bar_level_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  ShellBarLevel *self = SHELL_BAR_LEVEL (object);

  switch (prop_id)
    {
    case PROP_VALUE:
      shell_bar_level_set_value (self, g_value_get_double (value));
      break;

    case PROP_MAXIMUM_VALUE:
      shell_bar_level_set_maximum_value (self, g_value_get_double (value));
      break;

    case PROP_OVERDRIVE_START:
      shell_bar_level_set_overdrive_start (self, g_value_get_double (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_bar_level_dispose (GObject *object)
{
  ShellBarLevelPrivate *priv =
    shell_bar_level_get_instance_private (SHELL_BAR_LEVEL (object));

  g_clear_object (&priv->accessible);

  G_OBJECT_CLASS (shell_bar_level_parent_class)->dispose (object);
}

static void
shell_bar_level_class_init (ShellBarLevelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);
  StDrawingAreaClass *drawing_area_class = ST_DRAWING_AREA_CLASS (klass);

  object_class->dispose = shell_bar_level_dispose;
  object_class->get_property = shell_bar_level_get_property;
  object_class->set_property = shell_bar_level_set_property;

  actor_class->get_preferred_height = shell_bar_level_get_preferred_height;
  actor_class->get_preferred_width = shell_bar_level_get_preferred_width;

  widget_class->style_changed = shell_bar_level_style_changed;

  drawing_area_class->repaint = shell_bar_level_repaint;

  klass->get_preferred_bar_height = shell_bar_level_real_get_preferred_bar_height;
  klass->get_preferred_bar_width = shell_bar_level_real_get_preferred_bar_width;

  /**
   * ShellBarLevel:value:
   *
   * The current value, clamped to the range between 0 and
   * #ShellBarLevel:maximum-value.
   */
  props[PROP_VALUE] =
    g_param_spec_double ("value", NULL, NULL,
                         0, 2, 0,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellBarLevel:maximum-value:
   *
   * The maximum value of the bar.
   */
  props[PROP_MAXIMUM_VALUE] =
    g_param_spec_double ("maximum-value", NULL, NULL,
                         1, 2, 1,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellBarLevel:overdrive-start:
   *
   * The value at which the overdrive zone starts. No overdrive zone is
   * drawn when it equals #ShellBarLevel:maximum-value.
   */
  props[PROP_OVERDRIVE_START] =
    g_param_spec_double ("overdrive-start", NULL, NULL,
                         1, 2, 1,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_bar_level_init (ShellBarLevel *self)
{
  ShellBarLevelPrivate *priv = shell_bar_level_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);

  priv->value = 0;
  priv->maximum_value = 1;
  priv->overdrive_start = 1;

  st_widget_set_style_class_name (ST_WIDGET (self), "barlevel");
  clutter_actor_set_accessible_role (actor, ATK_ROLE_LEVEL_BAR);

  priv->accessible = st_generic_accessible_new_for_actor (actor);
  clutter_actor_set_accessible (actor, priv->accessible);

  g_signal_connect_object (priv->accessible, "get-current-value",
                           G_CALLBACK (accessible_get_current_value_cb),
                           self, 0);
  g_signal_connect_object (priv->accessible, "get-minimum-value",
                           G_CALLBACK (accessible_get_minimum_value_cb),
                           self, 0);
  g_signal_connect_object (priv->accessible, "get-maximum-value",
                           G_CALLBACK (accessible_get_maximum_value_cb),
                           self, 0);
  g_signal_connect_object (priv->accessible, "set-current-value",
                           G_CALLBACK (accessible_set_current_value_cb),
                           self, 0);

  g_signal_connect (self, "notify::value", G_CALLBACK (notify_value_cb), NULL);
}
