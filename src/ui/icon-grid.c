/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * icon-grid.c: The paged grid of icons of the application grid, the folders
 * and the search results, and the icon its items show. Port of
 * js/ui/iconGrid.js.
 *
 * Differences from the JS module:
 *
 *  - `new BaseIcon (label, {createIcon, setSizeManually, showLabel})` is
 *    `new Shell.BaseIcon ({label_text, set_size_manually, show_label})`
 *    followed by shell_base_icon_set_create_icon_func (). The `icon` and
 *    `label` fields are the read-only properties of the same names and
 *    `iconSize` is `icon-size`.
 *  - The size of the icon is an integer.
 *  - `IconGrid` creates its own #ShellIconGridLayout; the layout parameters
 *    that `IconGrid._init ()` took are properties of the grid which forward
 *    to the layout (`allow-incomplete-pages`, `orientation`,
 *    `columns-per-page`, `rows-per-page`, `page-halign`, `page-valign`,
 *    `last-row-align`, `column-spacing`, `row-spacing`, `page-padding`).
 *  - The functions which threw in JS (adding an item twice, a page that does
 *    not exist, ...) fail with a #GError in %G_IO_ERROR, which GJS throws.
 *  - `getDropTarget ()` returns the page, the position and the location as
 *    out parameters, GJS returns them as the same array.
 *  - `setGridModes (modes)` takes a #GVariant of type `a(ii)` holding
 *    (rows, columns) pairs; %NULL restores the default modes.
 *  - `ensureIconSizeUpdated ()` (which could never have worked: it pushed to
 *    an array nothing created), the `request_mode` of the layout (not a
 *    property of a layout manager) and the unexported
 *    `LEFT_DIVIDER_LEEWAY` and `RIGHT_DIVIDER_LEEWAY` constants are gone.
 */

#include "config.h"

#include <math.h>

#include <clutter/clutter.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-later.h>

#include "shell-global.h"
#include "ui/environment.h"
#include "ui/icon-grid.h"
#include "ui/layout.h"
#include "ui/main.h"

#define ICON_SIZE_LARGE 96
#define ICON_SIZE_TINY 16
#define ICON_SIZE ICON_SIZE_LARGE

#define PAGE_SWITCH_TIME 300
#define ICON_POSITION_DELAY 10

#define LEFT_DIVIDER_LEEWAY 20
#define RIGHT_DIVIDER_LEEWAY 20

/* IconSize, largest first */
static const int icon_sizes[] = { 96, 64, 48, 32, 24, 16 };

typedef struct
{
  int rows;
  int columns;
} GridMode;

static const GridMode default_grid_modes[] = {
  { 8, 3 },
  { 6, 4 },
  { 4, 6 },
  { 3, 8 },
};

static MetaLaters *
get_laters (void)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  MetaCompositor *compositor;

  if (display == NULL)
    return NULL;

  compositor = meta_display_get_compositor (display);
  if (compositor == NULL)
    return NULL;

  return meta_compositor_get_laters (compositor);
}

static gboolean
is_rtl (void)
{
  return clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL;
}

static int
swap (int value,
      int length)
{
  return length - value - 1;
}

/* ------------------------------------------------------------------------ */
/* zoomOutActor ()                                                           */
/* ------------------------------------------------------------------------ */

static void
zoom_out_done_cb (gboolean finished,
                  gpointer user_data)
{
  clutter_actor_destroy (CLUTTER_ACTOR (user_data));
}

/**
 * shell_icon_grid_zoom_out_actor_at_pos:
 * @actor: the actor to zoom out
 * @x: x coordinate in the stage of the clone
 * @y: y coordinate in the stage of the clone
 *
 * Shows a clone of @actor at (@x, @y), which grows, without leaving the
 * monitor, while it fades out; this is the animation of a launched icon.
 */
void
shell_icon_grid_zoom_out_actor_at_pos (ClutterActor *actor,
                                       float         x,
                                       float         y)
{
  ShellLayoutManager *layout_manager =
    shell_main_get_layout_manager (shell_main_get_default ());
  ShellEaseParams params = {
    .duration = SHELL_ICON_GRID_APPICON_ANIMATION_OUT_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
  };
  ShellMonitor *monitor;
  ClutterActor *clone;
  float width, height;
  double scaled_width, scaled_height, scaled_x, scaled_y;
  double contained_x, contained_y;

  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  if (layout_manager == NULL)
    return;

  monitor = shell_layout_manager_find_monitor_for_actor (layout_manager, actor);
  if (monitor == NULL)
    return;

  clone = g_object_new (CLUTTER_TYPE_CLONE,
                        "source", actor,
                        "reactive", FALSE,
                        NULL);
  clutter_actor_get_transformed_size (actor, &width, &height);

  clutter_actor_set_size (clone, width, height);
  clutter_actor_set_position (clone, x, y);
  clutter_actor_set_opacity (clone, 255);
  clutter_actor_set_pivot_point (clone, 0.5, 0.5);

  clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager)),
                           clone);

  /* Avoid monitor edges to not zoom outside the current monitor */
  scaled_width = width * SHELL_ICON_GRID_APPICON_ANIMATION_OUT_SCALE;
  scaled_height = height * SHELL_ICON_GRID_APPICON_ANIMATION_OUT_SCALE;
  scaled_x = x - (scaled_width - width) / 2;
  scaled_y = y - (scaled_height - height) / 2;
  contained_x = MIN (MAX (scaled_x, monitor->x),
                     monitor->x + monitor->width - scaled_width);
  contained_y = MIN (MAX (scaled_y, monitor->y),
                     monitor->y + monitor->height - scaled_height);

  shell_actor_ease (clone, &params,
                    zoom_out_done_cb, g_object_ref (clone), g_object_unref,
                    "scale-x", (double) SHELL_ICON_GRID_APPICON_ANIMATION_OUT_SCALE,
                    "scale-y", (double) SHELL_ICON_GRID_APPICON_ANIMATION_OUT_SCALE,
                    "translation-x", contained_x - scaled_x,
                    "translation-y", contained_y - scaled_y,
                    "opacity", 0,
                    NULL);

  shell_monitor_free (monitor);
}

/**
 * shell_icon_grid_zoom_out_actor:
 * @actor: the actor to zoom out
 *
 * Like shell_icon_grid_zoom_out_actor_at_pos (), at the position of @actor.
 */
void
shell_icon_grid_zoom_out_actor (ClutterActor *actor)
{
  float x, y;

  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  clutter_actor_get_transformed_position (actor, &x, &y);
  shell_icon_grid_zoom_out_actor_at_pos (actor, x, y);
}

/* ------------------------------------------------------------------------ */
/* ShellBaseIcon                                                             */
/* ------------------------------------------------------------------------ */

enum
{
  BASE_ICON_PROP_0,
  BASE_ICON_PROP_LABEL_TEXT,
  BASE_ICON_PROP_SHOW_LABEL,
  BASE_ICON_PROP_SET_SIZE_MANUALLY,
  BASE_ICON_PROP_ICON_SIZE,
  BASE_ICON_PROP_ICON,
  BASE_ICON_PROP_LABEL,
  BASE_ICON_N_PROPS
};

static GParamSpec *base_icon_props[BASE_ICON_N_PROPS] = { NULL, };

typedef struct
{
  StBoxLayout *box;
  StBin *icon_bin;
  StLabel *label;

  char *label_text;
  int icon_size;
  gboolean show_label;
  gboolean set_size_manually;

  ShellBaseIconCreateIconFunc create_icon_func;
  gpointer create_icon_data;
  GDestroyNotify create_icon_destroy;
} ShellBaseIconPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellBaseIcon, shell_base_icon, ST_TYPE_BIN)

static ClutterActor *
real_create_icon (ShellBaseIcon *self,
                  int            size)
{
  ShellBaseIconPrivate *priv = shell_base_icon_get_instance_private (self);

  if (priv->create_icon_func != NULL)
    return priv->create_icon_func (self, size, priv->create_icon_data);

  g_warning ("createIcon in %s is not implemented", G_OBJECT_TYPE_NAME (self));
  return NULL;
}

/**
 * shell_base_icon_create_icon:
 * @self: a #ShellBaseIcon
 * @size: the size of the icon in logical pixels
 *
 * Makes an icon actor for @size with the `create_icon` virtual function
 * (`createIcon ()`), without changing @self.
 *
 * Returns: (transfer full) (nullable): a new icon actor
 */
ClutterActor *
shell_base_icon_create_icon (ShellBaseIcon *self,
                             int            size)
{
  g_return_val_if_fail (SHELL_IS_BASE_ICON (self), NULL);

  return SHELL_BASE_ICON_GET_CLASS (self)->create_icon (self, size);
}

static void
create_icon_texture (ShellBaseIcon *self,
                     int            size)
{
  ShellBaseIconPrivate *priv = shell_base_icon_get_instance_private (self);
  ClutterActor *old_icon, *icon;

  if (priv->icon_bin == NULL)
    return;

  old_icon = st_bin_get_child (priv->icon_bin);
  if (old_icon != NULL)
    clutter_actor_destroy (old_icon);

  priv->icon_size = size;

  icon = shell_base_icon_create_icon (self, size);
  if (icon != NULL)
    {
      /* The icon is ours, floating or not. The bin sinks a floating one
       * and takes its own reference otherwise. */
      gboolean floating = g_object_is_floating (icon);

      st_bin_set_child (priv->icon_bin, icon);
      if (!floating)
        g_object_unref (icon);
    }
  else
    {
      st_bin_set_child (priv->icon_bin, NULL);
    }

  g_object_freeze_notify (G_OBJECT (self));
  g_object_notify_by_pspec (G_OBJECT (self), base_icon_props[BASE_ICON_PROP_ICON_SIZE]);
  g_object_notify_by_pspec (G_OBJECT (self), base_icon_props[BASE_ICON_PROP_ICON]);
  g_object_thaw_notify (G_OBJECT (self));
}

static void
icon_theme_changed_cb (StTextureCache *cache,
                       ShellBaseIcon  *self)
{
  ShellBaseIconPrivate *priv = shell_base_icon_get_instance_private (self);

  /* St.Icon updates automatically */
  if (priv->icon_bin == NULL ||
      !ST_IS_ICON (st_bin_get_child (priv->icon_bin)))
    create_icon_texture (self, priv->icon_size);
}

static void
shell_base_icon_style_changed (StWidget *widget)
{
  ShellBaseIcon *self = SHELL_BASE_ICON (widget);
  ShellBaseIconPrivate *priv = shell_base_icon_get_instance_private (self);
  int size;

  ST_WIDGET_CLASS (shell_base_icon_parent_class)->style_changed (widget);

  if (priv->set_size_manually)
    {
      size = priv->icon_size;
    }
  else
    {
      StThemeNode *node = st_widget_get_theme_node (widget);
      ClutterActor *stage = clutter_actor_get_stage (CLUTTER_ACTOR (self));
      int scale_factor = 1;
      double len;

      if (stage != NULL)
        {
          StThemeContext *context =
            st_theme_context_get_for_stage (CLUTTER_STAGE (stage));

          scale_factor = st_theme_context_get_scale_factor (context);
        }

      if (st_theme_node_lookup_length (node, "icon-size", FALSE, &len))
        size = (int) floor (len / scale_factor + 0.5);
      else
        size = ICON_SIZE;
    }

  if (priv->icon_size == size &&
      priv->icon_bin != NULL && st_bin_get_child (priv->icon_bin) != NULL)
    return;

  create_icon_texture (self, size);
}

/* The JS class derived from Shell.SquareBin */
static void
shell_base_icon_get_preferred_width (ClutterActor *actor,
                                     float         for_height,
                                     float        *min_width_p,
                                     float        *natural_width_p)
{
  /* Return the actual height to keep the squared aspect */
  clutter_actor_get_preferred_height (actor, -1, min_width_p, natural_width_p);
}

/**
 * shell_base_icon_set_create_icon_func:
 * @self: a #ShellBaseIcon
 * @func: (nullable) (scope notified) (closure user_data) (destroy destroy):
 *   makes the icon actor for a size
 * @user_data: data for @func
 * @destroy: called on @user_data when @func is replaced or @self is disposed
 *
 * Sets the function which the default `create_icon` virtual function calls
 * (the `createIcon` parameter of the JS constructor). It is first used when
 * the icon gets its style, so set it before the icon is added to the stage.
 */
void
shell_base_icon_set_create_icon_func (ShellBaseIcon              *self,
                                      ShellBaseIconCreateIconFunc func,
                                      gpointer                    user_data,
                                      GDestroyNotify              destroy)
{
  ShellBaseIconPrivate *priv;
  GDestroyNotify old_destroy;
  gpointer old_data;

  g_return_if_fail (SHELL_IS_BASE_ICON (self));

  priv = shell_base_icon_get_instance_private (self);

  old_destroy = priv->create_icon_destroy;
  old_data = priv->create_icon_data;

  priv->create_icon_func = func;
  priv->create_icon_data = user_data;
  priv->create_icon_destroy = destroy;

  if (old_destroy != NULL)
    old_destroy (old_data);
}

/**
 * shell_base_icon_set_icon_size:
 * @self: a #ShellBaseIcon
 * @size: the size of the icon in logical pixels
 * @error: return location for a #GError
 *
 * Makes the icon again if @size is not its size. This is `setIconSize ()`,
 * which is only allowed on icons made with #ShellBaseIcon:set-size-manually.
 *
 * Returns: %TRUE, or %FALSE with @error set if the size is not set manually
 */
gboolean
shell_base_icon_set_icon_size (ShellBaseIcon  *self,
                               int             size,
                               GError        **error)
{
  ShellBaseIconPrivate *priv;

  g_return_val_if_fail (SHELL_IS_BASE_ICON (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  priv = shell_base_icon_get_instance_private (self);

  if (!priv->set_size_manually)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "set-size-manually has to be set to use set_icon_size");
      return FALSE;
    }

  if (size != priv->icon_size)
    create_icon_texture (self, size);

  return TRUE;
}

/**
 * shell_base_icon_get_icon_size:
 * @self: a #ShellBaseIcon
 *
 * Returns: the size of the icon in logical pixels
 */
int
shell_base_icon_get_icon_size (ShellBaseIcon *self)
{
  ShellBaseIconPrivate *priv;

  g_return_val_if_fail (SHELL_IS_BASE_ICON (self), 0);

  priv = shell_base_icon_get_instance_private (self);
  return priv->icon_size;
}

/**
 * shell_base_icon_get_icon:
 * @self: a #ShellBaseIcon
 *
 * Returns: (transfer none) (nullable): the icon actor made by `create_icon`
 */
ClutterActor *
shell_base_icon_get_icon (ShellBaseIcon *self)
{
  ShellBaseIconPrivate *priv;

  g_return_val_if_fail (SHELL_IS_BASE_ICON (self), NULL);

  priv = shell_base_icon_get_instance_private (self);
  return priv->icon_bin != NULL ? st_bin_get_child (priv->icon_bin) : NULL;
}

/**
 * shell_base_icon_get_label:
 * @self: a #ShellBaseIcon
 *
 * Returns: (transfer none) (nullable): the label, or %NULL if #ShellBaseIcon:show-label
 *   is %FALSE
 */
StLabel *
shell_base_icon_get_label (ShellBaseIcon *self)
{
  ShellBaseIconPrivate *priv;

  g_return_val_if_fail (SHELL_IS_BASE_ICON (self), NULL);

  priv = shell_base_icon_get_instance_private (self);
  return priv->label;
}

/**
 * shell_base_icon_update:
 * @self: a #ShellBaseIcon
 *
 * Makes the icon again at its current size.
 */
void
shell_base_icon_update (ShellBaseIcon *self)
{
  ShellBaseIconPrivate *priv;

  g_return_if_fail (SHELL_IS_BASE_ICON (self));

  priv = shell_base_icon_get_instance_private (self);
  create_icon_texture (self, priv->icon_size);
}

/**
 * shell_base_icon_animate_zoom_out:
 * @self: a #ShellBaseIcon
 *
 * Zooms out a clone of the icon at its position, see
 * shell_icon_grid_zoom_out_actor ().
 */
void
shell_base_icon_animate_zoom_out (ShellBaseIcon *self)
{
  ClutterActor *child;

  g_return_if_fail (SHELL_IS_BASE_ICON (self));

  /* Animate only the child instead of the entire actor, so the
   * styles like hover and running are not applied while
   * animating. */
  child = st_bin_get_child (ST_BIN (self));
  if (child != NULL)
    shell_icon_grid_zoom_out_actor (child);
}

/**
 * shell_base_icon_animate_zoom_out_at_pos:
 * @self: a #ShellBaseIcon
 * @x: x coordinate in the stage
 * @y: y coordinate in the stage
 *
 * Like shell_base_icon_animate_zoom_out (), with the clone at (@x, @y).
 */
void
shell_base_icon_animate_zoom_out_at_pos (ShellBaseIcon *self,
                                         float          x,
                                         float          y)
{
  ClutterActor *child;

  g_return_if_fail (SHELL_IS_BASE_ICON (self));

  child = st_bin_get_child (ST_BIN (self));
  if (child != NULL)
    shell_icon_grid_zoom_out_actor_at_pos (child, x, y);
}

static void
shell_base_icon_constructed (GObject *object)
{
  ShellBaseIcon *self = SHELL_BASE_ICON (object);
  ShellBaseIconPrivate *priv = shell_base_icon_get_instance_private (self);
  ClutterActor *box, *icon_bin;

  G_OBJECT_CLASS (shell_base_icon_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (self),
                                  priv->show_label
                                  ? "overview-icon overview-icon-with-label"
                                  : "overview-icon");

  box = g_object_new (ST_TYPE_BOX_LAYOUT,
                      "orientation", CLUTTER_ORIENTATION_VERTICAL,
                      "x-expand", TRUE,
                      "y-expand", TRUE,
                      NULL);
  g_set_weak_pointer (&priv->box, ST_BOX_LAYOUT (box));
  st_bin_set_child (ST_BIN (self), box);

  icon_bin = g_object_new (ST_TYPE_BIN,
                           "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                           NULL);
  g_set_weak_pointer (&priv->icon_bin, ST_BIN (icon_bin));
  clutter_actor_add_child (box, icon_bin);

  if (priv->show_label)
    {
      ClutterText *text;
      StLabel *label = ST_LABEL (st_label_new (priv->label_text));

      text = st_label_get_clutter_text (label);
      clutter_actor_set_x_align (CLUTTER_ACTOR (text), CLUTTER_ACTOR_ALIGN_CENTER);
      clutter_actor_set_y_align (CLUTTER_ACTOR (text), CLUTTER_ACTOR_ALIGN_CENTER);

      g_set_weak_pointer (&priv->label, label);
      clutter_actor_add_child (box, CLUTTER_ACTOR (label));
    }

  g_signal_connect_object (st_texture_cache_get_default (),
                           "icon-theme-changed",
                           G_CALLBACK (icon_theme_changed_cb), self, 0);
}

static void
shell_base_icon_dispose (GObject *object)
{
  ShellBaseIcon *self = SHELL_BASE_ICON (object);
  ShellBaseIconPrivate *priv = shell_base_icon_get_instance_private (self);

  /* The function usually closes over the actor which owns the icon */
  shell_base_icon_set_create_icon_func (self, NULL, NULL, NULL);

  g_clear_weak_pointer (&priv->box);
  g_clear_weak_pointer (&priv->icon_bin);
  g_clear_weak_pointer (&priv->label);

  G_OBJECT_CLASS (shell_base_icon_parent_class)->dispose (object);
}

static void
shell_base_icon_finalize (GObject *object)
{
  ShellBaseIcon *self = SHELL_BASE_ICON (object);
  ShellBaseIconPrivate *priv = shell_base_icon_get_instance_private (self);

  g_free (priv->label_text);

  G_OBJECT_CLASS (shell_base_icon_parent_class)->finalize (object);
}

static void
shell_base_icon_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  ShellBaseIcon *self = SHELL_BASE_ICON (object);
  ShellBaseIconPrivate *priv = shell_base_icon_get_instance_private (self);

  switch (prop_id)
    {
    case BASE_ICON_PROP_LABEL_TEXT:
      if (g_strcmp0 (priv->label_text, g_value_get_string (value)) != 0)
        {
          g_free (priv->label_text);
          priv->label_text = g_value_dup_string (value);
          if (priv->label != NULL)
            st_label_set_text (priv->label, priv->label_text);
          g_object_notify_by_pspec (object, pspec);
        }
      break;

    case BASE_ICON_PROP_SHOW_LABEL:
      priv->show_label = g_value_get_boolean (value);
      break;

    case BASE_ICON_PROP_SET_SIZE_MANUALLY:
      priv->set_size_manually = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_base_icon_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  ShellBaseIcon *self = SHELL_BASE_ICON (object);
  ShellBaseIconPrivate *priv = shell_base_icon_get_instance_private (self);

  switch (prop_id)
    {
    case BASE_ICON_PROP_LABEL_TEXT:
      g_value_set_string (value, priv->label_text);
      break;

    case BASE_ICON_PROP_SHOW_LABEL:
      g_value_set_boolean (value, priv->show_label);
      break;

    case BASE_ICON_PROP_SET_SIZE_MANUALLY:
      g_value_set_boolean (value, priv->set_size_manually);
      break;

    case BASE_ICON_PROP_ICON_SIZE:
      g_value_set_int (value, priv->icon_size);
      break;

    case BASE_ICON_PROP_ICON:
      g_value_set_object (value, shell_base_icon_get_icon (self));
      break;

    case BASE_ICON_PROP_LABEL:
      g_value_set_object (value, priv->label);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_base_icon_class_init (ShellBaseIconClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);

  object_class->constructed = shell_base_icon_constructed;
  object_class->dispose = shell_base_icon_dispose;
  object_class->finalize = shell_base_icon_finalize;
  object_class->set_property = shell_base_icon_set_property;
  object_class->get_property = shell_base_icon_get_property;

  actor_class->get_preferred_width = shell_base_icon_get_preferred_width;

  widget_class->style_changed = shell_base_icon_style_changed;

  klass->create_icon = real_create_icon;

  /**
   * ShellBaseIcon:label-text:
   *
   * The text of the label, the first parameter of the JS constructor.
   */
  base_icon_props[BASE_ICON_PROP_LABEL_TEXT] =
    g_param_spec_string ("label-text", NULL, NULL,
                         "",
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellBaseIcon:show-label:
   *
   * Whether the icon has a label (`showLabel`).
   */
  base_icon_props[BASE_ICON_PROP_SHOW_LABEL] =
    g_param_spec_boolean ("show-label", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellBaseIcon:set-size-manually:
   *
   * Whether the owner sets the size of the icon with
   * shell_base_icon_set_icon_size (), instead of the icon-size of the CSS
   * (`setSizeManually`).
   */
  base_icon_props[BASE_ICON_PROP_SET_SIZE_MANUALLY] =
    g_param_spec_boolean ("set-size-manually", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellBaseIcon:icon-size:
   *
   * The size of the icon in logical pixels (`iconSize`).
   */
  base_icon_props[BASE_ICON_PROP_ICON_SIZE] =
    g_param_spec_int ("icon-size", NULL, NULL,
                      0, G_MAXINT, ICON_SIZE,
                      G_PARAM_READABLE |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellBaseIcon:icon:
   *
   * The icon actor made by `create_icon`.
   */
  base_icon_props[BASE_ICON_PROP_ICON] =
    g_param_spec_object ("icon", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellBaseIcon:label:
   *
   * The label, %NULL if #ShellBaseIcon:show-label is %FALSE.
   */
  base_icon_props[BASE_ICON_PROP_LABEL] =
    g_param_spec_object ("label", NULL, NULL,
                         ST_TYPE_LABEL,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, BASE_ICON_N_PROPS,
                                     base_icon_props);
}

static void
shell_base_icon_init (ShellBaseIcon *self)
{
  ShellBaseIconPrivate *priv = shell_base_icon_get_instance_private (self);

  priv->icon_size = ICON_SIZE;
}

/* ------------------------------------------------------------------------ */
/* ShellIconGridLayout                                                       */
/* ------------------------------------------------------------------------ */

enum
{
  LAYOUT_PROP_0,
  LAYOUT_PROP_ALLOW_INCOMPLETE_PAGES,
  LAYOUT_PROP_COLUMN_SPACING,
  LAYOUT_PROP_COLUMNS_PER_PAGE,
  LAYOUT_PROP_FIXED_ICON_SIZE,
  LAYOUT_PROP_ICON_SIZE,
  LAYOUT_PROP_LAST_ROW_ALIGN,
  LAYOUT_PROP_MAX_COLUMN_SPACING,
  LAYOUT_PROP_MAX_ROW_SPACING,
  LAYOUT_PROP_ORIENTATION,
  LAYOUT_PROP_PAGE_HALIGN,
  LAYOUT_PROP_PAGE_PADDING,
  LAYOUT_PROP_PAGE_VALIGN,
  LAYOUT_PROP_ROW_SPACING,
  LAYOUT_PROP_ROWS_PER_PAGE,
  LAYOUT_PROP_N_PAGES,
  LAYOUT_PROP_PAGE_HEIGHT,
  LAYOUT_PROP_PAGE_WIDTH,
  LAYOUT_N_PROPS
};

static GParamSpec *layout_props[LAYOUT_N_PROPS] = { NULL, };

enum
{
  LAYOUT_PAGES_CHANGED,
  LAYOUT_LAST_SIGNAL
};

static guint layout_signals[LAYOUT_LAST_SIGNAL] = { 0, };

typedef struct
{
  GPtrArray *children;
  GPtrArray *visible_children;
} Page;

typedef struct
{
  ClutterActor *actor;
  int page_index;
  gulong destroy_id;
  gulong visible_id;
  gulong queue_relayout_id;
} ItemData;

struct _ShellIconGridLayout
{
  ClutterLayoutManager parent;

  gboolean allow_incomplete_pages;
  int column_spacing;
  int columns_per_page;
  int fixed_icon_size;
  ClutterActorAlign last_row_align;
  int max_column_spacing;
  int max_row_spacing;
  ClutterOrientation orientation;
  ClutterActorAlign page_halign;
  ClutterMargin page_padding;
  ClutterActorAlign page_valign;
  int row_spacing;
  int rows_per_page;

  int icon_size;

  gboolean page_size_changed;
  gboolean should_ease_items;
  float page_height;
  float page_width;
  float children_max_size;

  /* Page *, in order */
  GPtrArray *pages;
  /* ClutterActor * -> ItemData * */
  GHashTable *items;

  ClutterActor *container;
  MetaLaters *laters;
  guint update_icon_sizes_later;
};

G_DEFINE_FINAL_TYPE (ShellIconGridLayout, shell_icon_grid_layout,
                     CLUTTER_TYPE_LAYOUT_MANAGER)

static Page *
page_new (void)
{
  Page *page = g_new0 (Page, 1);

  page->children = g_ptr_array_new ();
  page->visible_children = g_ptr_array_new ();

  return page;
}

static void
page_free (Page *page)
{
  g_ptr_array_unref (page->children);
  g_ptr_array_unref (page->visible_children);
  g_free (page);
}

static inline Page *
get_page (ShellIconGridLayout *self,
          int                  page_index)
{
  return g_ptr_array_index (self->pages, page_index);
}

static inline int
get_n_pages (ShellIconGridLayout *self)
{
  return (int) self->pages->len;
}

static inline int
get_items_per_page (ShellIconGridLayout *self)
{
  return self->columns_per_page * self->rows_per_page;
}

/* Finds the icon of an item of the grid, the former `item.icon` */
static ShellBaseIcon *
find_base_icon (ClutterActor *item)
{
  ClutterActor *child;

  if (SHELL_IS_BASE_ICON (item))
    return SHELL_BASE_ICON (item);

  for (child = clutter_actor_get_first_child (item);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      ShellBaseIcon *icon = find_base_icon (child);

      if (icon != NULL)
        return icon;
    }

  return NULL;
}

static void
set_item_icon_size (ClutterActor *item,
                    int           size)
{
  ShellBaseIcon *icon = find_base_icon (item);

  if (icon != NULL)
    shell_base_icon_set_icon_size (icon, size, NULL);
}

static GPtrArray *
ref_snapshot (GPtrArray *items)
{
  GPtrArray *snapshot = g_ptr_array_new_full (items->len, g_object_unref);
  guint i;

  for (i = 0; i < items->len; i++)
    g_ptr_array_add (snapshot, g_object_ref (g_ptr_array_index (items, i)));

  return snapshot;
}

static void
update_visible_children_for_page (ShellIconGridLayout *self,
                                  int                  page_index)
{
  Page *page = get_page (self, page_index);
  guint i;

  g_ptr_array_set_size (page->visible_children, 0);
  for (i = 0; i < page->children->len; i++)
    {
      ClutterActor *actor = g_ptr_array_index (page->children, i);

      if (clutter_actor_is_visible (actor))
        g_ptr_array_add (page->visible_children, actor);
    }
}

static void
unlink_item (ShellIconGridLayout *self,
             ClutterActor        *item)
{
  ItemData *item_data = g_hash_table_lookup (self->items, item);

  if (item_data == NULL)
    return;

  g_signal_handler_disconnect (item, item_data->destroy_id);
  g_signal_handler_disconnect (item, item_data->visible_id);
  g_signal_handler_disconnect (item, item_data->queue_relayout_id);

  g_hash_table_remove (self->items, item);
}

static void
remove_page (ShellIconGridLayout *self,
             int                  page_index)
{
  Page *page = get_page (self, page_index);
  GHashTableIter iter;
  gpointer value;
  guint i;

  /* Make sure to not leave any icon left here */
  for (i = 0; i < page->children->len; i++)
    unlink_item (self, g_ptr_array_index (page->children, i));

  /* Adjust the page indexes of items after this page */
  g_hash_table_iter_init (&iter, self->items);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      ItemData *item_data = value;

      if (item_data->page_index > page_index)
        item_data->page_index--;
    }

  g_ptr_array_remove_index (self->pages, page_index);

  g_object_notify_by_pspec (G_OBJECT (self), layout_props[LAYOUT_PROP_N_PAGES]);
  g_signal_emit (self, layout_signals[LAYOUT_PAGES_CHANGED], 0);
}

static void
append_page (ShellIconGridLayout *self)
{
  g_ptr_array_add (self->pages, page_new ());

  g_object_notify_by_pspec (G_OBJECT (self), layout_props[LAYOUT_PROP_N_PAGES]);
  g_signal_emit (self, layout_signals[LAYOUT_PAGES_CHANGED], 0);
}

static void remove_item_data         (ShellIconGridLayout *self,
                                      ClutterActor        *item);
static void add_item_to_page         (ShellIconGridLayout *self,
                                      ClutterActor        *item,
                                      int                  page_index,
                                      int                  index);
static void relocate_surplus_items   (ShellIconGridLayout *self,
                                      int                  page_index);

static void
fill_item_vacancies (ShellIconGridLayout *self,
                     int                  page_index)
{
  g_autoptr (GPtrArray) next_items = NULL;
  int items_per_page = get_items_per_page (self);
  Page *page, *next_page;
  int n_missing;
  guint i;

  if (page_index >= get_n_pages (self) - 1)
    return;

  page = get_page (self, page_index);

  /* No reduce needed */
  if ((int) page->visible_children->len == items_per_page)
    return;

  next_page = get_page (self, page_index + 1);
  n_missing = MIN (items_per_page - (int) page->visible_children->len,
                   (int) next_page->visible_children->len);
  if (n_missing <= 0)
    return;

  /* The items are moved out of the page they are read from */
  next_items = ref_snapshot (next_page->visible_children);

  /* Append to the current page the first items of the next page */
  for (i = 0; i < (guint) n_missing; i++)
    {
      ClutterActor *reduced_item = g_ptr_array_index (next_items, i);

      remove_item_data (self, reduced_item);
      add_item_to_page (self, reduced_item, page_index, -1);
    }
}

static void
remove_item_data (ShellIconGridLayout *self,
                  ClutterActor        *item)
{
  ItemData *item_data = g_hash_table_lookup (self->items, item);
  int page_index;
  Page *page;
  guint item_index;

  if (item_data == NULL)
    return;

  page_index = item_data->page_index;
  page = get_page (self, page_index);

  unlink_item (self, item);

  if (g_ptr_array_find (page->children, item, &item_index))
    g_ptr_array_remove_index (page->children, item_index);

  update_visible_children_for_page (self, page_index);

  /* Delete the page if this is the last icon in it */
  if (get_page (self, page_index)->visible_children->len == 0)
    remove_page (self, page_index);

  if (!self->allow_incomplete_pages)
    fill_item_vacancies (self, page_index);
}

static void
relocate_surplus_items (ShellIconGridLayout *self,
                        int                  page_index)
{
  g_autoptr (GPtrArray) visible_items = NULL;
  int items_per_page = get_items_per_page (self);
  Page *page = get_page (self, page_index);
  int n_extra_items;
  int i;

  /* No overflow needed */
  if ((int) page->visible_children->len <= items_per_page)
    return;

  n_extra_items = (int) page->visible_children->len - items_per_page;
  visible_items = ref_snapshot (page->visible_children);

  for (i = 0; i < n_extra_items; i++)
    {
      int overflow_index = (int) visible_items->len - i - 1;
      ClutterActor *overflow_item = g_ptr_array_index (visible_items, overflow_index);

      remove_item_data (self, overflow_item);
      add_item_to_page (self, overflow_item, page_index + 1, 0);
    }
}

static void
item_destroy_cb (ClutterActor        *item,
                 ShellIconGridLayout *self)
{
  remove_item_data (self, item);
}

static void
item_visible_cb (ClutterActor        *item,
                 GParamSpec          *pspec,
                 ShellIconGridLayout *self)
{
  ItemData *item_data = g_hash_table_lookup (self->items, item);
  int page_index;

  if (item_data == NULL)
    return;

  page_index = item_data->page_index;
  update_visible_children_for_page (self, page_index);

  if (clutter_actor_is_visible (item))
    relocate_surplus_items (self, page_index);
  else if (!self->allow_incomplete_pages)
    fill_item_vacancies (self, page_index);
}

static void
item_queue_relayout_cb (ClutterActor        *item,
                        ShellIconGridLayout *self)
{
  self->children_max_size = -1;
}

static void
add_item_to_page (ShellIconGridLayout *self,
                  ClutterActor        *item,
                  int                  page_index,
                  int                  index)
{
  ItemData *item_data;
  Page *page;

  /* Ensure we have at least one page */
  if (get_n_pages (self) == 0)
    append_page (self);

  /* Append a new page if necessary */
  if (page_index == get_n_pages (self))
    append_page (self);

  if (page_index < 0 || page_index >= get_n_pages (self))
    page_index = get_n_pages (self) - 1;

  page = get_page (self, page_index);

  /* Array.splice () appended for any index past the end (the saved page
   * layout can name a position the page does not have yet) */
  if (index < 0 || index > (int) page->children->len)
    index = (int) page->children->len;

  item_data = g_new0 (ItemData, 1);
  item_data->actor = item;
  item_data->page_index = page_index;
  item_data->destroy_id =
    g_signal_connect (item, "destroy", G_CALLBACK (item_destroy_cb), self);
  item_data->visible_id =
    g_signal_connect (item, "notify::visible", G_CALLBACK (item_visible_cb), self);
  item_data->queue_relayout_id =
    g_signal_connect (item, "queue-relayout", G_CALLBACK (item_queue_relayout_cb), self);
  g_hash_table_insert (self->items, item, item_data);

  set_item_icon_size (item, self->icon_size);

  g_ptr_array_insert (page->children, index, item);
  update_visible_children_for_page (self, page_index);
  relocate_surplus_items (self, page_index);
}

static int
find_best_page_to_append (ShellIconGridLayout *self,
                          int                  start_page)
{
  int items_per_page = get_items_per_page (self);
  int i;

  for (i = MAX (start_page, 0); i < get_n_pages (self); i++)
    {
      if ((int) get_page (self, i)->visible_children->len < items_per_page)
        return i;
    }

  return get_n_pages (self);
}

static float
get_children_max_size (ShellIconGridLayout *self)
{
  if (self->children_max_size == -1)
    {
      float min_width = 0;
      float min_height = 0;
      int page_index;

      for (page_index = 0; page_index < get_n_pages (self); page_index++)
        {
          Page *page = get_page (self, page_index);
          guint item_index;

          for (item_index = 0; item_index < page->visible_children->len; item_index++)
            {
              ClutterActor *item = g_ptr_array_index (page->visible_children, item_index);
              float child_min_width, child_min_height;

              clutter_actor_get_preferred_height (item, -1, &child_min_height, NULL);
              clutter_actor_get_preferred_width (item, -1, &child_min_width, NULL);

              min_width = MAX (min_width, child_min_width);
              min_height = MAX (min_height, child_min_height);
            }
        }

      self->children_max_size = MAX (min_width, min_height);
    }

  return self->children_max_size;
}

static int
find_best_icon_size (ShellIconGridLayout *self)
{
  int n_columns = self->columns_per_page;
  int n_rows = self->rows_per_page;
  double column_spacing_per_page = self->column_spacing * (n_columns - 1);
  double row_spacing_per_page = self->row_spacing * (n_rows - 1);
  ClutterActor *first_item = NULL;
  guint i;

  if (self->container != NULL)
    first_item = clutter_actor_get_first_child (self->container);

  if (self->fixed_icon_size != -1)
    return self->fixed_icon_size;

  for (i = 0; i < G_N_ELEMENTS (icon_sizes); i++)
    {
      int size = icon_sizes[i];
      double used_width, used_height, empty_h_space, empty_v_space;

      if (first_item != NULL)
        {
          float first_item_width, first_item_height, item_size;

          set_item_icon_size (first_item, size);
          clutter_actor_get_preferred_size (first_item,
                                            &first_item_width, &first_item_height,
                                            NULL, NULL);

          item_size = MAX (first_item_width, first_item_height);

          used_width = item_size * n_columns;
          used_height = item_size * n_rows;
        }
      else
        {
          used_width = size * n_columns;
          used_height = size * n_rows;
        }

      empty_h_space =
        self->page_width - used_width - column_spacing_per_page -
        self->page_padding.left - self->page_padding.right;
      empty_v_space =
        self->page_height - used_height - row_spacing_per_page -
        self->page_padding.top - self->page_padding.bottom;

      if (empty_h_space >= 0 && empty_v_space > 0)
        return size;
    }

  return ICON_SIZE_TINY;
}

static void
calculate_spacing (ShellIconGridLayout *self,
                   double               child_size,
                   double              *left_empty_space_out,
                   double              *top_empty_space_out,
                   double              *h_spacing_out,
                   double              *v_spacing_out)
{
  int n_columns = self->columns_per_page;
  int n_rows = self->rows_per_page;
  double used_width = child_size * n_columns;
  double used_height = child_size * n_rows;
  double column_spacing_per_page = self->column_spacing * (n_columns - 1);
  double row_spacing_per_page = self->row_spacing * (n_rows - 1);
  double empty_h_space =
    self->page_width - used_width - column_spacing_per_page -
    self->page_padding.left - self->page_padding.right;
  double empty_v_space =
    self->page_height - used_height - row_spacing_per_page -
    self->page_padding.top - self->page_padding.bottom;
  double left_empty_space = self->page_padding.left;
  double top_empty_space = self->page_padding.top;
  double h_spacing = 0;
  double v_spacing = 0;

  switch (self->page_halign)
    {
    case CLUTTER_ACTOR_ALIGN_START:
      h_spacing = self->column_spacing;
      break;

    case CLUTTER_ACTOR_ALIGN_CENTER:
      left_empty_space += floor (empty_h_space / 2);
      h_spacing = self->column_spacing;
      break;

    case CLUTTER_ACTOR_ALIGN_END:
      left_empty_space += empty_h_space;
      h_spacing = self->column_spacing;
      break;

    case CLUTTER_ACTOR_ALIGN_FILL:
      h_spacing = self->column_spacing + empty_h_space / (n_columns - 1);

      /* Maybe constraint horizontal spacing */
      if (self->max_column_spacing != -1 && h_spacing > self->max_column_spacing)
        {
          double extra_h_spacing =
            (self->max_column_spacing - self->column_spacing) * (n_columns - 1);

          h_spacing = self->max_column_spacing;
          left_empty_space += MAX ((empty_h_space - extra_h_spacing) / 2, 0);
        }
      break;

    default:
      break;
    }

  switch (self->page_valign)
    {
    case CLUTTER_ACTOR_ALIGN_START:
      v_spacing = self->row_spacing;
      break;

    case CLUTTER_ACTOR_ALIGN_CENTER:
      top_empty_space += floor (empty_v_space / 2);
      v_spacing = self->row_spacing;
      break;

    case CLUTTER_ACTOR_ALIGN_END:
      top_empty_space += empty_v_space;
      v_spacing = self->row_spacing;
      break;

    case CLUTTER_ACTOR_ALIGN_FILL:
      v_spacing = self->row_spacing + empty_v_space / (n_rows - 1);

      /* Maybe constraint vertical spacing */
      if (self->max_row_spacing != -1 && v_spacing > self->max_row_spacing)
        {
          double extra_v_spacing =
            (self->max_row_spacing - self->row_spacing) * (n_rows - 1);

          v_spacing = self->max_row_spacing;
          top_empty_space += MAX ((empty_v_space - extra_v_spacing) / 2, 0);
        }
      break;

    default:
      break;
    }

  *left_empty_space_out = left_empty_space;
  *top_empty_space_out = top_empty_space;
  *h_spacing_out = h_spacing;
  *v_spacing_out = v_spacing;
}

static double
get_row_padding (ShellIconGridLayout *self,
                 ClutterActorAlign    align,
                 GPtrArray           *items,
                 int                  item_index,
                 double               child_size,
                 double               spacing)
{
  int n_rows, row, row_start, row_end, items_in_this_row, n_empty;
  double available_width, row_align = 0;

  if (align == CLUTTER_ACTOR_ALIGN_START ||
      align == CLUTTER_ACTOR_ALIGN_FILL)
    return 0;

  n_rows = (int) ceil ((double) items->len / self->columns_per_page);

  row = item_index / self->columns_per_page;

  /* Only apply to the last row */
  if (row < n_rows - 1)
    return 0;

  row_start = row * self->columns_per_page;
  row_end = MIN ((row + 1) * self->columns_per_page - 1, (int) items->len - 1);
  items_in_this_row = row_end - row_start + 1;
  n_empty = self->columns_per_page - items_in_this_row;
  available_width = n_empty * (spacing + child_size);

  switch (align)
    {
    case CLUTTER_ACTOR_ALIGN_CENTER:
      row_align = available_width / 2;
      break;

    case CLUTTER_ACTOR_ALIGN_END:
      row_align = available_width;
      break;

    default:
      /* START and FILL align are handled at the beginning of the function */
      break;
    }

  return is_rtl () ? row_align * -1 : row_align;
}

static gboolean
animate_icon_position (ClutterActor     *icon,
                       ClutterActorBox  *box,
                       int               n_changed_icons)
{
  ClutterActorBox allocation;

  if (clutter_actor_has_allocation (icon))
    clutter_actor_get_allocation_box (icon, &allocation);

  if (!clutter_actor_has_allocation (icon) ||
      clutter_actor_box_equal (&allocation, box) ||
      clutter_actor_get_opacity (icon) == 0)
    {
      clutter_actor_allocate (icon, box);
      return FALSE;
    }

  clutter_actor_save_easing_state (icon);
  clutter_actor_set_easing_mode (icon, CLUTTER_EASE_OUT_QUAD);
  clutter_actor_set_easing_delay (icon, n_changed_icons * ICON_POSITION_DELAY);

  clutter_actor_allocate (icon, box);

  clutter_actor_restore_easing_state (icon);

  return TRUE;
}

static void
clear_update_icon_sizes_later (ShellIconGridLayout *self)
{
  if (self->update_icon_sizes_later != 0 && self->laters != NULL)
    meta_laters_remove (self->laters, self->update_icon_sizes_later);

  self->update_icon_sizes_later = 0;
}

static void
container_destroy_cb (ClutterActor        *container,
                      ShellIconGridLayout *self)
{
  clear_update_icon_sizes_later (self);
}

static void
shell_icon_grid_layout_set_container (ClutterLayoutManager *manager,
                                      ClutterActor         *container)
{
  ShellIconGridLayout *self = SHELL_ICON_GRID_LAYOUT (manager);

  if (self->container != NULL)
    g_signal_handlers_disconnect_by_func (self->container,
                                          container_destroy_cb, self);

  g_set_weak_pointer (&self->container, container);

  if (self->container != NULL)
    g_signal_connect (self->container, "destroy",
                      G_CALLBACK (container_destroy_cb), self);
}

static void
shell_icon_grid_layout_get_preferred_width (ClutterLayoutManager *manager,
                                            ClutterActor         *container,
                                            float                 for_height,
                                            float                *min_width_p,
                                            float                *natural_width_p)
{
  ShellIconGridLayout *self = SHELL_ICON_GRID_LAYOUT (manager);
  float min_width = -1;
  float natural_width = -1;

  switch (self->orientation)
    {
    case CLUTTER_ORIENTATION_VERTICAL:
      min_width = ICON_SIZE_TINY;
      natural_width = self->page_width;
      break;

    case CLUTTER_ORIENTATION_HORIZONTAL:
      min_width = self->page_width * get_n_pages (self);
      natural_width = min_width;
      break;

    default:
      break;
    }

  if (min_width_p)
    *min_width_p = min_width;
  if (natural_width_p)
    *natural_width_p = natural_width;
}

static void
shell_icon_grid_layout_get_preferred_height (ClutterLayoutManager *manager,
                                             ClutterActor         *container,
                                             float                 for_width,
                                             float                *min_height_p,
                                             float                *natural_height_p)
{
  ShellIconGridLayout *self = SHELL_ICON_GRID_LAYOUT (manager);
  float min_height = -1;
  float natural_height = -1;

  switch (self->orientation)
    {
    case CLUTTER_ORIENTATION_VERTICAL:
      min_height = self->page_height * get_n_pages (self);
      natural_height = min_height;
      break;

    case CLUTTER_ORIENTATION_HORIZONTAL:
      min_height = ICON_SIZE_TINY;
      natural_height = self->page_height;
      break;

    default:
      break;
    }

  if (min_height_p)
    *min_height_p = min_height;
  if (natural_height_p)
    *natural_height_p = natural_height;
}

static void
shell_icon_grid_layout_allocate (ClutterLayoutManager   *manager,
                                 ClutterActor           *container,
                                 const ClutterActorBox  *allocation)
{
  ShellIconGridLayout *self = SHELL_ICON_GRID_LAYOUT (manager);
  gboolean rtl = is_rtl ();
  double child_size, left_empty_space, top_empty_space, h_spacing, v_spacing;
  int n_changed_icons = 0;
  int columns_per_page = self->columns_per_page;
  ClutterOrientation orientation = self->orientation;
  float page_width = self->page_width;
  float page_height = self->page_height;
  gboolean page_size_changed = self->page_size_changed;
  ClutterActorAlign last_row_align = self->last_row_align;
  gboolean should_ease_items = self->should_ease_items;
  int page_index;

  /* IconGridLayout.adaptToSize wasn't called before the allocation */
  if (self->page_width == 0 || self->page_height == 0)
    return;

  child_size = get_children_max_size (self);

  calculate_spacing (self, child_size,
                     &left_empty_space, &top_empty_space, &h_spacing, &v_spacing);

  for (page_index = 0; page_index < get_n_pages (self); page_index++)
    {
      Page *page = get_page (self, page_index);
      int visual_page_index = page_index;
      guint item_index;

      if (rtl && orientation == CLUTTER_ORIENTATION_HORIZONTAL)
        visual_page_index = swap (page_index, get_n_pages (self));

      for (item_index = 0; item_index < page->visible_children->len; item_index++)
        {
          ClutterActor *item = g_ptr_array_index (page->visible_children, item_index);
          ClutterActorBox child_box;
          float natural_width, natural_height;
          int row = item_index / columns_per_page;
          int column = item_index % columns_per_page;
          double row_padding, x, y;

          if (rtl)
            column = swap (column, columns_per_page);

          row_padding = get_row_padding (self, last_row_align,
                                         page->visible_children, item_index,
                                         child_size, h_spacing);

          /* Icon position */
          x = left_empty_space + row_padding + column * (child_size + h_spacing);
          y = top_empty_space + row * (child_size + v_spacing);

          /* Page start */
          switch (orientation)
            {
            case CLUTTER_ORIENTATION_HORIZONTAL:
              x += visual_page_index * page_width;
              break;

            case CLUTTER_ORIENTATION_VERTICAL:
              y += visual_page_index * page_height;
              break;

            default:
              break;
            }

          clutter_actor_get_preferred_size (item, NULL, NULL,
                                            &natural_width, &natural_height);
          clutter_actor_box_init_rect (&child_box, floor (x), floor (y),
                                       MAX (child_size, natural_width),
                                       MAX (child_size, natural_height));

          if (!should_ease_items || page_size_changed)
            clutter_actor_allocate (item, &child_box);
          else if (animate_icon_position (item, &child_box, n_changed_icons))
            n_changed_icons++;
        }
    }

  self->page_size_changed = FALSE;
  self->should_ease_items = FALSE;
}

static gboolean
update_icon_sizes_later_cb (gpointer user_data)
{
  ShellIconGridLayout *self = user_data;
  int icon_size = find_best_icon_size (self);

  if (self->icon_size != icon_size)
    {
      ClutterActor *child;

      self->icon_size = icon_size;

      if (self->container != NULL)
        {
          for (child = clutter_actor_get_first_child (self->container);
               child != NULL;
               child = clutter_actor_get_next_sibling (child))
            set_item_icon_size (child, icon_size);
        }

      g_object_notify_by_pspec (G_OBJECT (self), layout_props[LAYOUT_PROP_ICON_SIZE]);
    }

  self->update_icon_sizes_later = 0;
  return G_SOURCE_REMOVE;
}

/**
 * shell_icon_grid_layout_adapt_to_size:
 * @self: a #ShellIconGridLayout
 * @page_width: the width of a page
 * @page_height: the height of a page
 *
 * Sets the size of a page, which must be done before the allocation, and
 * finds the best icon size for it before the next redraw.
 */
void
shell_icon_grid_layout_adapt_to_size (ShellIconGridLayout *self,
                                      float                page_width,
                                      float                page_height)
{
  g_return_if_fail (SHELL_IS_ICON_GRID_LAYOUT (self));

  if (self->page_width == page_width && self->page_height == page_height)
    return;

  self->page_width = page_width;
  self->page_height = page_height;
  self->page_size_changed = TRUE;

  g_object_freeze_notify (G_OBJECT (self));
  g_object_notify_by_pspec (G_OBJECT (self), layout_props[LAYOUT_PROP_PAGE_WIDTH]);
  g_object_notify_by_pspec (G_OBJECT (self), layout_props[LAYOUT_PROP_PAGE_HEIGHT]);
  g_object_thaw_notify (G_OBJECT (self));

  if (self->update_icon_sizes_later == 0)
    {
      MetaLaters *laters = get_laters ();

      if (laters != NULL)
        {
          g_set_weak_pointer (&self->laters, laters);
          self->update_icon_sizes_later =
            meta_laters_add (laters, META_LATER_BEFORE_REDRAW,
                             update_icon_sizes_later_cb, self, NULL);
        }
    }
}

static void
set_error_invalid (GError    **error,
                   const char *format,
                   ...) G_GNUC_PRINTF (2, 3);

static void
set_error_invalid (GError    **error,
                   const char *format,
                   ...)
{
  g_autofree char *message = NULL;
  va_list args;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, message);
}

/**
 * shell_icon_grid_layout_add_item:
 * @self: a #ShellIconGridLayout
 * @item: the item to add to the grid, which must not be part of it. It needs
 *   a #ShellBaseIcon among its descendants
 * @page: the page number, -1 for the last page. It must be between 0 and
 *   the number of pages, adding to the page after the last one creates a
 *   page
 * @index: the position in the page, -1 for the end. If it exceeds the
 *   number of items per page, @item is added to the next page
 * @error: return location for a #GError
 *
 * Adds @item to the container of the layout and to the grid.
 *
 * Returns: %TRUE, or %FALSE with @error set if @item is already part of the
 *   grid, @page does not exist or @item has no icon
 */
gboolean
shell_icon_grid_layout_add_item (ShellIconGridLayout  *self,
                                 ClutterActor         *item,
                                 int                   page,
                                 int                   index,
                                 GError              **error)
{
  g_return_val_if_fail (SHELL_IS_ICON_GRID_LAYOUT (self), FALSE);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (item), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (g_hash_table_contains (self->items, item))
    {
      set_error_invalid (error, "Item %s already added to IconGridLayout",
                         G_OBJECT_TYPE_NAME (item));
      return FALSE;
    }

  if (page > get_n_pages (self))
    {
      set_error_invalid (error, "Cannot add %s to page %d",
                         G_OBJECT_TYPE_NAME (item), page);
      return FALSE;
    }

  if (find_base_icon (item) == NULL)
    {
      set_error_invalid (error, "Only items with a ShellBaseIcon can be added to the grid");
      return FALSE;
    }

  if (self->container == NULL)
    return TRUE;

  if (page != -1 && index == -1)
    page = find_best_page_to_append (self, page);

  self->should_ease_items = TRUE;

  clutter_actor_add_child (self->container, item);
  add_item_to_page (self, item, page, index);

  return TRUE;
}

/**
 * shell_icon_grid_layout_append_item:
 * @self: a #ShellIconGridLayout
 * @item: the item to append to the grid, which must not be part of it
 * @error: return location for a #GError
 *
 * Appends @item to the grid.
 *
 * Returns: %TRUE, or %FALSE with @error set
 */
gboolean
shell_icon_grid_layout_append_item (ShellIconGridLayout  *self,
                                    ClutterActor         *item,
                                    GError              **error)
{
  return shell_icon_grid_layout_add_item (self, item, -1, -1, error);
}

/**
 * shell_icon_grid_layout_move_item:
 * @self: a #ShellIconGridLayout
 * @item: the item to move, which must be part of the grid
 * @new_page: the new page of @item, -1 for the last one
 * @new_position: the new position of @item in the page, -1 for the end
 * @error: return location for a #GError
 *
 * Moves @item in the grid.
 *
 * Returns: %TRUE, or %FALSE with @error set if @item is not part of the grid
 */
gboolean
shell_icon_grid_layout_move_item (ShellIconGridLayout  *self,
                                  ClutterActor         *item,
                                  int                   new_page,
                                  int                   new_position,
                                  GError              **error)
{
  g_return_val_if_fail (SHELL_IS_ICON_GRID_LAYOUT (self), FALSE);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (item), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!g_hash_table_contains (self->items, item))
    {
      set_error_invalid (error, "Item %s is not part of the IconGridLayout",
                         G_OBJECT_TYPE_NAME (item));
      return FALSE;
    }

  self->should_ease_items = TRUE;

  remove_item_data (self, item);

  if (new_page != -1 && new_position == -1)
    new_page = find_best_page_to_append (self, new_page);

  add_item_to_page (self, item, new_page, new_position);

  return TRUE;
}

/**
 * shell_icon_grid_layout_remove_item:
 * @self: a #ShellIconGridLayout
 * @item: the item to remove, which must be part of the grid
 * @error: return location for a #GError
 *
 * Removes @item from the container of the layout and from the grid.
 *
 * Returns: %TRUE, or %FALSE with @error set if @item is not part of the grid
 */
gboolean
shell_icon_grid_layout_remove_item (ShellIconGridLayout  *self,
                                    ClutterActor         *item,
                                    GError              **error)
{
  g_return_val_if_fail (SHELL_IS_ICON_GRID_LAYOUT (self), FALSE);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (item), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!g_hash_table_contains (self->items, item))
    {
      set_error_invalid (error, "Item %s is not part of the IconGridLayout",
                         G_OBJECT_TYPE_NAME (item));
      return FALSE;
    }

  if (self->container == NULL)
    return TRUE;

  self->should_ease_items = TRUE;

  if (clutter_actor_get_parent (item) == self->container)
    clutter_actor_remove_child (self->container, item);
  remove_item_data (self, item);

  return TRUE;
}

/**
 * shell_icon_grid_layout_get_items_at_page:
 * @self: a #ShellIconGridLayout
 * @page: the page index
 * @error: return location for a #GError
 *
 * Retrieves the children at @page. Children may be invisible.
 *
 * Returns: (transfer container) (element-type Clutter.Actor): the items of
 *   the page, or %NULL with @error set if the page does not exist
 */
GList *
shell_icon_grid_layout_get_items_at_page (ShellIconGridLayout  *self,
                                          int                   page,
                                          GError              **error)
{
  GList *items = NULL;
  Page *page_data;
  guint i;

  g_return_val_if_fail (SHELL_IS_ICON_GRID_LAYOUT (self), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (page < 0 || page >= get_n_pages (self))
    {
      set_error_invalid (error, "IconGridLayout does not have page %d", page);
      return NULL;
    }

  page_data = get_page (self, page);
  for (i = page_data->children->len; i > 0; i--)
    items = g_list_prepend (items, g_ptr_array_index (page_data->children, i - 1));

  return items;
}

/**
 * shell_icon_grid_layout_get_item_position:
 * @self: a #ShellIconGridLayout
 * @item: the item
 * @page: (out): return location for the page of @item, -1 if it is not part
 *   of the grid
 * @position: (out): return location for the position of @item among the
 *   visible items of its page, -1 if it is not part of the grid or not
 *   visible
 *
 * Retrieves the position of @item in its page.
 */
void
shell_icon_grid_layout_get_item_position (ShellIconGridLayout *self,
                                          ClutterActor        *item,
                                          int                 *page,
                                          int                 *position)
{
  ItemData *item_data;
  guint index;

  g_return_if_fail (SHELL_IS_ICON_GRID_LAYOUT (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (item));

  item_data = g_hash_table_lookup (self->items, item);
  if (item_data == NULL)
    {
      if (page)
        *page = -1;
      if (position)
        *position = -1;
      return;
    }

  if (page)
    *page = item_data->page_index;

  if (position)
    {
      Page *page_data = get_page (self, item_data->page_index);

      if (g_ptr_array_find (page_data->visible_children, item, &index))
        *position = (int) index;
      else
        *position = -1;
    }
}

/**
 * shell_icon_grid_layout_get_item_at:
 * @self: a #ShellIconGridLayout
 * @page: the page
 * @position: the position among the visible items of the page
 *
 * Returns: (transfer none) (nullable): the item at @page and @position
 */
ClutterActor *
shell_icon_grid_layout_get_item_at (ShellIconGridLayout *self,
                                    int                  page,
                                    int                  position)
{
  Page *page_data;

  g_return_val_if_fail (SHELL_IS_ICON_GRID_LAYOUT (self), NULL);

  if (page < 0 || page >= get_n_pages (self))
    return NULL;

  page_data = get_page (self, page);

  if (position < 0 || position >= (int) page_data->visible_children->len)
    return NULL;

  return g_ptr_array_index (page_data->visible_children, position);
}

/**
 * shell_icon_grid_layout_get_item_page:
 * @self: a #ShellIconGridLayout
 * @item: the item
 *
 * Returns: the page @item is in, or -1 if @item is not part of the grid
 */
int
shell_icon_grid_layout_get_item_page (ShellIconGridLayout *self,
                                      ClutterActor        *item)
{
  ItemData *item_data;

  g_return_val_if_fail (SHELL_IS_ICON_GRID_LAYOUT (self), -1);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (item), -1);

  item_data = g_hash_table_lookup (self->items, item);
  return item_data != NULL ? item_data->page_index : -1;
}

/**
 * shell_icon_grid_layout_get_drop_target:
 * @self: a #ShellIconGridLayout
 * @x: the position on the horizontal axis, relative to the grid
 * @y: the position on the vertical axis, relative to the grid
 * @page: (out): return location for the page under (@x, @y)
 * @position: (out): return location for the position of the item under
 *   (@x, @y), -1 if there is none
 * @location: (out): return location for where (@x, @y) is relative to the item
 *
 * Retrieves the item located at (@x, @y), as well as the drag location.
 */
void
shell_icon_grid_layout_get_drop_target (ShellIconGridLayout       *self,
                                        double                     x,
                                        double                     y,
                                        int                       *page,
                                        int                       *position,
                                        ShellIconGridDragLocation *location)
{
  double child_size;
  double left_empty_space, top_empty_space, h_spacing, v_spacing;
  gboolean rtl = is_rtl ();
  double adj_x, adj_y, grid_width, grid_height, half_h_spacing, half_v_spacing;
  gboolean in_top_empty_space, in_left_empty_space;
  gboolean in_right_empty_space, in_bottom_empty_space;
  double page_double;
  int page_index;
  Page *page_data;
  guint i;

  g_return_if_fail (SHELL_IS_ICON_GRID_LAYOUT (self));

  child_size = get_children_max_size (self);
  calculate_spacing (self, child_size,
                     &left_empty_space, &top_empty_space, &h_spacing, &v_spacing);

  page_double = self->orientation == CLUTTER_ORIENTATION_VERTICAL
    ? floor (y / self->page_height)
    : floor (x / self->page_width);

  /* Out of bounds */
  if (!(page_double >= 0 && page_double < get_n_pages (self)))
    goto invalid;

  page_index = (int) page_double;

  if (rtl && self->orientation == CLUTTER_ORIENTATION_HORIZONTAL)
    page_index = swap (page_index, get_n_pages (self));

  /* Get page-relative coordinates */
  adj_x = x;
  adj_y = y;
  if (self->orientation == CLUTTER_ORIENTATION_HORIZONTAL)
    adj_x = fmod (adj_x, self->page_width);
  else
    adj_y = fmod (adj_y, self->page_height);

  grid_width =
    child_size * self->columns_per_page +
    h_spacing * (self->columns_per_page - 1);
  grid_height =
    child_size * self->rows_per_page +
    v_spacing * (self->rows_per_page - 1);

  in_top_empty_space = adj_y < top_empty_space;
  in_left_empty_space = adj_x < left_empty_space;
  in_right_empty_space = adj_x > left_empty_space + grid_width;
  in_bottom_empty_space = adj_y > top_empty_space + grid_height;

  if (in_top_empty_space || in_bottom_empty_space)
    goto invalid;

  half_h_spacing = h_spacing / 2;
  half_v_spacing = v_spacing / 2;
  page_data = get_page (self, page_index);

  for (i = 0; i < page_data->visible_children->len; i++)
    {
      ClutterActor *item = g_ptr_array_index (page_data->visible_children, i);
      ClutterActorBox child_box;
      gboolean first_in_row = (int) i % self->columns_per_page == 0;
      gboolean last_in_row =
        (int) i % self->columns_per_page == self->columns_per_page - 1;
      ShellIconGridDragLocation drag_location;

      clutter_actor_get_allocation_box (item, &child_box);

      /* Check icon boundaries */
      if ((in_left_empty_space && first_in_row) ||
          (in_right_empty_space && last_in_row))
        {
          if (y < child_box.y1 - half_v_spacing ||
              y > child_box.y2 + half_v_spacing)
            continue;
        }
      else
        {
          if (x < child_box.x1 - half_h_spacing ||
              x > child_box.x2 + half_h_spacing ||
              y < child_box.y1 - half_v_spacing ||
              y > child_box.y2 + half_v_spacing)
            continue;
        }

      if (x < child_box.x1 + LEFT_DIVIDER_LEEWAY)
        drag_location = SHELL_ICON_GRID_DRAG_LOCATION_START_EDGE;
      else if (x > child_box.x2 - RIGHT_DIVIDER_LEEWAY)
        drag_location = SHELL_ICON_GRID_DRAG_LOCATION_END_EDGE;
      else
        drag_location = SHELL_ICON_GRID_DRAG_LOCATION_ON_ICON;

      if (rtl)
        {
          if (drag_location == SHELL_ICON_GRID_DRAG_LOCATION_START_EDGE)
            drag_location = SHELL_ICON_GRID_DRAG_LOCATION_END_EDGE;
          else if (drag_location == SHELL_ICON_GRID_DRAG_LOCATION_END_EDGE)
            drag_location = SHELL_ICON_GRID_DRAG_LOCATION_START_EDGE;
        }

      if (page)
        *page = page_index;
      if (position)
        *position = (int) i;
      if (location)
        *location = drag_location;
      return;
    }

  if (page)
    *page = page_index;
  if (position)
    *position = -1;
  if (location)
    *location = SHELL_ICON_GRID_DRAG_LOCATION_EMPTY_SPACE;
  return;

invalid:
  if (page)
    *page = 0;
  if (position)
    *position = 0;
  if (location)
    *location = SHELL_ICON_GRID_DRAG_LOCATION_INVALID;
}

static void
layout_set_int (ShellIconGridLayout *self,
                int                 *field,
                int                  value,
                int                  prop)
{
  if (*field == value)
    return;

  *field = value;
  g_object_notify_by_pspec (G_OBJECT (self), layout_props[prop]);
}

static void
layout_set_align (ShellIconGridLayout *self,
                  ClutterActorAlign   *field,
                  ClutterActorAlign    value,
                  int                  prop)
{
  if (*field == value)
    return;

  *field = value;
  g_object_notify_by_pspec (G_OBJECT (self), layout_props[prop]);
}

static void
shell_icon_grid_layout_set_property (GObject      *object,
                                     guint         prop_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  ShellIconGridLayout *self = SHELL_ICON_GRID_LAYOUT (object);

  switch (prop_id)
    {
    case LAYOUT_PROP_ALLOW_INCOMPLETE_PAGES:
      if (self->allow_incomplete_pages != g_value_get_boolean (value))
        {
          self->allow_incomplete_pages = g_value_get_boolean (value);
          g_object_notify_by_pspec (object, pspec);
        }
      break;

    case LAYOUT_PROP_COLUMN_SPACING:
      layout_set_int (self, &self->column_spacing, g_value_get_int (value), prop_id);
      break;

    case LAYOUT_PROP_COLUMNS_PER_PAGE:
      layout_set_int (self, &self->columns_per_page, g_value_get_int (value), prop_id);
      break;

    case LAYOUT_PROP_FIXED_ICON_SIZE:
      layout_set_int (self, &self->fixed_icon_size, g_value_get_int (value), prop_id);
      break;

    case LAYOUT_PROP_LAST_ROW_ALIGN:
      layout_set_align (self, &self->last_row_align, g_value_get_enum (value), prop_id);
      break;

    case LAYOUT_PROP_MAX_COLUMN_SPACING:
      layout_set_int (self, &self->max_column_spacing, g_value_get_int (value), prop_id);
      break;

    case LAYOUT_PROP_MAX_ROW_SPACING:
      layout_set_int (self, &self->max_row_spacing, g_value_get_int (value), prop_id);
      break;

    case LAYOUT_PROP_ORIENTATION:
      if (self->orientation != (ClutterOrientation) g_value_get_enum (value))
        {
          self->orientation = g_value_get_enum (value);
          g_object_notify_by_pspec (object, pspec);
        }
      break;

    case LAYOUT_PROP_PAGE_HALIGN:
      layout_set_align (self, &self->page_halign, g_value_get_enum (value), prop_id);
      break;

    case LAYOUT_PROP_PAGE_PADDING:
      {
        const ClutterMargin *margin = g_value_get_boxed (value);

        if (margin != NULL)
          self->page_padding = *margin;
        else
          memset (&self->page_padding, 0, sizeof (ClutterMargin));
        g_object_notify_by_pspec (object, pspec);
      }
      break;

    case LAYOUT_PROP_PAGE_VALIGN:
      layout_set_align (self, &self->page_valign, g_value_get_enum (value), prop_id);
      break;

    case LAYOUT_PROP_ROW_SPACING:
      layout_set_int (self, &self->row_spacing, g_value_get_int (value), prop_id);
      break;

    case LAYOUT_PROP_ROWS_PER_PAGE:
      layout_set_int (self, &self->rows_per_page, g_value_get_int (value), prop_id);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_icon_grid_layout_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  ShellIconGridLayout *self = SHELL_ICON_GRID_LAYOUT (object);

  switch (prop_id)
    {
    case LAYOUT_PROP_ALLOW_INCOMPLETE_PAGES:
      g_value_set_boolean (value, self->allow_incomplete_pages);
      break;

    case LAYOUT_PROP_COLUMN_SPACING:
      g_value_set_int (value, self->column_spacing);
      break;

    case LAYOUT_PROP_COLUMNS_PER_PAGE:
      g_value_set_int (value, self->columns_per_page);
      break;

    case LAYOUT_PROP_FIXED_ICON_SIZE:
      g_value_set_int (value, self->fixed_icon_size);
      break;

    case LAYOUT_PROP_ICON_SIZE:
      g_value_set_int (value, self->icon_size);
      break;

    case LAYOUT_PROP_LAST_ROW_ALIGN:
      g_value_set_enum (value, self->last_row_align);
      break;

    case LAYOUT_PROP_MAX_COLUMN_SPACING:
      g_value_set_int (value, self->max_column_spacing);
      break;

    case LAYOUT_PROP_MAX_ROW_SPACING:
      g_value_set_int (value, self->max_row_spacing);
      break;

    case LAYOUT_PROP_ORIENTATION:
      g_value_set_enum (value, self->orientation);
      break;

    case LAYOUT_PROP_PAGE_HALIGN:
      g_value_set_enum (value, self->page_halign);
      break;

    case LAYOUT_PROP_PAGE_PADDING:
      g_value_set_boxed (value, &self->page_padding);
      break;

    case LAYOUT_PROP_PAGE_VALIGN:
      g_value_set_enum (value, self->page_valign);
      break;

    case LAYOUT_PROP_ROW_SPACING:
      g_value_set_int (value, self->row_spacing);
      break;

    case LAYOUT_PROP_ROWS_PER_PAGE:
      g_value_set_int (value, self->rows_per_page);
      break;

    case LAYOUT_PROP_N_PAGES:
      g_value_set_int (value, get_n_pages (self));
      break;

    case LAYOUT_PROP_PAGE_HEIGHT:
      g_value_set_float (value, self->page_height);
      break;

    case LAYOUT_PROP_PAGE_WIDTH:
      g_value_set_float (value, self->page_width);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_icon_grid_layout_constructed (GObject *object)
{
  ShellIconGridLayout *self = SHELL_ICON_GRID_LAYOUT (object);

  G_OBJECT_CLASS (shell_icon_grid_layout_parent_class)->constructed (object);

  self->icon_size = self->fixed_icon_size != -1
    ? self->fixed_icon_size
    : ICON_SIZE_LARGE;
}

static void
shell_icon_grid_layout_dispose (GObject *object)
{
  ShellIconGridLayout *self = SHELL_ICON_GRID_LAYOUT (object);
  GHashTableIter iter;
  gpointer key, value;

  clear_update_icon_sizes_later (self);
  g_clear_weak_pointer (&self->laters);

  if (self->container != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->container,
                                            container_destroy_cb, self);
      g_clear_weak_pointer (&self->container);
    }

  g_hash_table_iter_init (&iter, self->items);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      ItemData *item_data = value;

      g_signal_handler_disconnect (key, item_data->destroy_id);
      g_signal_handler_disconnect (key, item_data->visible_id);
      g_signal_handler_disconnect (key, item_data->queue_relayout_id);
    }
  g_hash_table_remove_all (self->items);

  G_OBJECT_CLASS (shell_icon_grid_layout_parent_class)->dispose (object);
}

static void
shell_icon_grid_layout_finalize (GObject *object)
{
  ShellIconGridLayout *self = SHELL_ICON_GRID_LAYOUT (object);

  g_clear_pointer (&self->items, g_hash_table_unref);
  g_clear_pointer (&self->pages, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_icon_grid_layout_parent_class)->finalize (object);
}

static void
shell_icon_grid_layout_class_init (ShellIconGridLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);
  GParamFlags rw = G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
  GParamFlags ro = G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;

  object_class->constructed = shell_icon_grid_layout_constructed;
  object_class->dispose = shell_icon_grid_layout_dispose;
  object_class->finalize = shell_icon_grid_layout_finalize;
  object_class->set_property = shell_icon_grid_layout_set_property;
  object_class->get_property = shell_icon_grid_layout_get_property;

  layout_class->set_container = shell_icon_grid_layout_set_container;
  layout_class->get_preferred_width = shell_icon_grid_layout_get_preferred_width;
  layout_class->get_preferred_height = shell_icon_grid_layout_get_preferred_height;
  layout_class->allocate = shell_icon_grid_layout_allocate;

  layout_props[LAYOUT_PROP_ALLOW_INCOMPLETE_PAGES] =
    g_param_spec_boolean ("allow-incomplete-pages", NULL, NULL, TRUE, rw);

  layout_props[LAYOUT_PROP_COLUMN_SPACING] =
    g_param_spec_int ("column-spacing", NULL, NULL, 0, G_MAXINT32, 0, rw);

  layout_props[LAYOUT_PROP_COLUMNS_PER_PAGE] =
    g_param_spec_int ("columns-per-page", NULL, NULL, 1, G_MAXINT32, 6, rw);

  /* A construct property, the icon size is derived from it when the
   * layout is created */
  layout_props[LAYOUT_PROP_FIXED_ICON_SIZE] =
    g_param_spec_int ("fixed-icon-size", NULL, NULL, -1, G_MAXINT32, -1,
                      rw | G_PARAM_CONSTRUCT);

  layout_props[LAYOUT_PROP_ICON_SIZE] =
    g_param_spec_int ("icon-size", NULL, NULL, 0, G_MAXINT32, 0, ro);

  layout_props[LAYOUT_PROP_LAST_ROW_ALIGN] =
    g_param_spec_enum ("last-row-align", NULL, NULL,
                       CLUTTER_TYPE_ACTOR_ALIGN, CLUTTER_ACTOR_ALIGN_FILL, rw);

  layout_props[LAYOUT_PROP_MAX_COLUMN_SPACING] =
    g_param_spec_int ("max-column-spacing", NULL, NULL, -1, G_MAXINT32, -1, rw);

  layout_props[LAYOUT_PROP_MAX_ROW_SPACING] =
    g_param_spec_int ("max-row-spacing", NULL, NULL, -1, G_MAXINT32, -1, rw);

  layout_props[LAYOUT_PROP_ORIENTATION] =
    g_param_spec_enum ("orientation", NULL, NULL,
                       CLUTTER_TYPE_ORIENTATION, CLUTTER_ORIENTATION_VERTICAL, rw);

  layout_props[LAYOUT_PROP_PAGE_HALIGN] =
    g_param_spec_enum ("page-halign", NULL, NULL,
                       CLUTTER_TYPE_ACTOR_ALIGN, CLUTTER_ACTOR_ALIGN_FILL, rw);

  layout_props[LAYOUT_PROP_PAGE_PADDING] =
    g_param_spec_boxed ("page-padding", NULL, NULL, CLUTTER_TYPE_MARGIN, rw);

  layout_props[LAYOUT_PROP_PAGE_VALIGN] =
    g_param_spec_enum ("page-valign", NULL, NULL,
                       CLUTTER_TYPE_ACTOR_ALIGN, CLUTTER_ACTOR_ALIGN_FILL, rw);

  layout_props[LAYOUT_PROP_ROW_SPACING] =
    g_param_spec_int ("row-spacing", NULL, NULL, 0, G_MAXINT32, 0, rw);

  layout_props[LAYOUT_PROP_ROWS_PER_PAGE] =
    g_param_spec_int ("rows-per-page", NULL, NULL, 1, G_MAXINT32, 4, rw);

  layout_props[LAYOUT_PROP_N_PAGES] =
    g_param_spec_int ("n-pages", NULL, NULL, 0, G_MAXINT32, 0, ro);

  layout_props[LAYOUT_PROP_PAGE_HEIGHT] =
    g_param_spec_float ("page-height", NULL, NULL, 0, G_MAXFLOAT, 0, ro);

  layout_props[LAYOUT_PROP_PAGE_WIDTH] =
    g_param_spec_float ("page-width", NULL, NULL, 0, G_MAXFLOAT, 0, ro);

  g_object_class_install_properties (object_class, LAYOUT_N_PROPS, layout_props);

  /**
   * ShellIconGridLayout::pages-changed:
   * @self: the #ShellIconGridLayout
   *
   * Emitted when a page is added or removed.
   */
  layout_signals[LAYOUT_PAGES_CHANGED] =
    g_signal_new ("pages-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_icon_grid_layout_init (ShellIconGridLayout *self)
{
  self->allow_incomplete_pages = TRUE;
  self->columns_per_page = 6;
  self->rows_per_page = 4;
  self->fixed_icon_size = -1;
  self->max_column_spacing = -1;
  self->max_row_spacing = -1;
  self->last_row_align = CLUTTER_ACTOR_ALIGN_FILL;
  self->page_halign = CLUTTER_ACTOR_ALIGN_FILL;
  self->page_valign = CLUTTER_ACTOR_ALIGN_FILL;
  self->orientation = CLUTTER_ORIENTATION_VERTICAL;
  self->icon_size = ICON_SIZE_LARGE;
  self->children_max_size = -1;

  self->pages = g_ptr_array_new_with_free_func ((GDestroyNotify) page_free);
  self->items = g_hash_table_new_full (NULL, NULL, NULL, g_free);
}

/* ------------------------------------------------------------------------ */
/* ShellIconGrid                                                             */
/* ------------------------------------------------------------------------ */

enum
{
  GRID_PROP_0,
  GRID_PROP_CURRENT_PAGE,
  GRID_PROP_N_PAGES,
  GRID_PROP_ITEMS_PER_PAGE,
  /* Forwarded to the layout, see forwarded_props */
  GRID_PROP_ALLOW_INCOMPLETE_PAGES,
  GRID_PROP_ORIENTATION,
  GRID_PROP_COLUMNS_PER_PAGE,
  GRID_PROP_ROWS_PER_PAGE,
  GRID_PROP_PAGE_HALIGN,
  GRID_PROP_PAGE_VALIGN,
  GRID_PROP_LAST_ROW_ALIGN,
  GRID_PROP_COLUMN_SPACING,
  GRID_PROP_ROW_SPACING,
  GRID_PROP_PAGE_PADDING,
  GRID_N_PROPS
};

static GParamSpec *grid_props[GRID_N_PROPS] = { NULL, };

static const char * const forwarded_props[GRID_N_PROPS] = {
  [GRID_PROP_ALLOW_INCOMPLETE_PAGES] = "allow-incomplete-pages",
  [GRID_PROP_ORIENTATION] = "orientation",
  [GRID_PROP_COLUMNS_PER_PAGE] = "columns-per-page",
  [GRID_PROP_ROWS_PER_PAGE] = "rows-per-page",
  [GRID_PROP_PAGE_HALIGN] = "page-halign",
  [GRID_PROP_PAGE_VALIGN] = "page-valign",
  [GRID_PROP_LAST_ROW_ALIGN] = "last-row-align",
  [GRID_PROP_COLUMN_SPACING] = "column-spacing",
  [GRID_PROP_ROW_SPACING] = "row-spacing",
  [GRID_PROP_PAGE_PADDING] = "page-padding",
};

enum
{
  GRID_PAGES_CHANGED,
  GRID_LAST_SIGNAL
};

static guint grid_signals[GRID_LAST_SIGNAL] = { 0, };

typedef struct
{
  ShellIconGridLayout *layout;

  int current_page;
  int current_mode;
  /* GridMode, NULL for the default modes */
  GArray *grid_modes;
} ShellIconGridPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellIconGrid, shell_icon_grid, ST_TYPE_VIEWPORT)

typedef struct
{
  StAdjustment *adjustment;
  double value;
  int duration;
} GoToPageData;

static void
go_to_page_data_free (GoToPageData *data)
{
  g_object_unref (data->adjustment);
  g_free (data);
}

static gboolean
go_to_page_later_cb (gpointer user_data)
{
  GoToPageData *data = user_data;
  ShellEaseParams params = {
    .duration = data->duration,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_CUBIC,
  };
  g_auto (GValue) target = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;

  g_value_init (&target, G_TYPE_DOUBLE);
  g_value_set_double (&target, data->value);

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (data->adjustment),
                                       "value", &target, &params,
                                       NULL, NULL, NULL, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);

  return G_SOURCE_REMOVE;
}

/**
 * shell_icon_grid_go_to_page:
 * @self: a #ShellIconGrid
 * @page: the page, which must exist
 * @animate: whether to animate the page transition
 * @error: return location for a #GError
 *
 * Moves the current page to @page. The scrolling starts before the next
 * redraw.
 *
 * Returns: %TRUE, or %FALSE with @error set if @page does not exist
 */
gboolean
shell_icon_grid_go_to_page (ShellIconGrid  *self,
                            int             page,
                            gboolean        animate,
                            GError        **error)
{
  ShellIconGridPrivate *priv;
  StAdjustment *hadjustment = NULL, *vadjustment = NULL, *adjustment;
  GoToPageData *data;
  MetaLaters *laters;
  double new_value;

  g_return_val_if_fail (SHELL_IS_ICON_GRID (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  priv = shell_icon_grid_get_instance_private (self);

  if (priv->layout == NULL || page < 0 || page >= get_n_pages (priv->layout))
    {
      set_error_invalid (error, "IconGrid does not have page %d", page);
      return FALSE;
    }

  st_scrollable_get_adjustments (ST_SCROLLABLE (self), &hadjustment, &vadjustment);

  switch (priv->layout->orientation)
    {
    case CLUTTER_ORIENTATION_VERTICAL:
      adjustment = vadjustment;
      new_value = page * (double) priv->layout->page_height;
      break;

    case CLUTTER_ORIENTATION_HORIZONTAL:
    default:
      adjustment = hadjustment;
      new_value = page * (double) priv->layout->page_width;
      break;
    }

  if (priv->current_page != page)
    {
      priv->current_page = page;
      g_object_notify_by_pspec (G_OBJECT (self), grid_props[GRID_PROP_CURRENT_PAGE]);
    }

  if (!clutter_actor_is_mapped (CLUTTER_ACTOR (self)))
    animate = FALSE;

  laters = get_laters ();
  if (laters == NULL || adjustment == NULL)
    return TRUE;

  data = g_new0 (GoToPageData, 1);
  data->adjustment = g_object_ref (adjustment);
  data->value = new_value;
  data->duration = animate ? PAGE_SWITCH_TIME : 0;

  meta_laters_add (laters, META_LATER_BEFORE_REDRAW,
                   go_to_page_later_cb, data,
                   (GDestroyNotify) go_to_page_data_free);

  return TRUE;
}

static void
ensure_item_is_visible (ShellIconGrid *self,
                        ClutterActor  *item)
{
  ShellIconGridPrivate *priv = shell_icon_grid_get_instance_private (self);
  g_autoptr (GError) error = NULL;
  int item_page;

  if (!clutter_actor_contains (CLUTTER_ACTOR (self), item))
    return;

  item_page = shell_icon_grid_layout_get_item_page (priv->layout, item);
  if (item_page < 0)
    return;

  if (!shell_icon_grid_go_to_page (self, item_page, TRUE, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

static void
item_key_focus_in_cb (ClutterActor  *item,
                      ShellIconGrid *self)
{
  ensure_item_is_visible (self, item);
}

static void
shell_icon_grid_child_added (ClutterActor *actor,
                             ClutterActor *child)
{
  ClutterActorClass *parent_class = CLUTTER_ACTOR_CLASS (shell_icon_grid_parent_class);

  if (parent_class->child_added != NULL)
    parent_class->child_added (actor, child);

  g_signal_connect_object (child, "key-focus-in",
                           G_CALLBACK (item_key_focus_in_cb), actor, 0);
}

static void
shell_icon_grid_child_removed (ClutterActor *actor,
                               ClutterActor *child)
{
  ClutterActorClass *parent_class = CLUTTER_ACTOR_CLASS (shell_icon_grid_parent_class);

  g_signal_handlers_disconnect_by_func (child, item_key_focus_in_cb, actor);

  if (parent_class->child_removed != NULL)
    parent_class->child_removed (actor, child);
}

static void
set_grid_mode (ShellIconGrid *self,
               int            mode_index)
{
  ShellIconGridPrivate *priv = shell_icon_grid_get_instance_private (self);
  const GridMode *modes = priv->grid_modes != NULL
    ? (const GridMode *) priv->grid_modes->data : default_grid_modes;
  guint n_modes = priv->grid_modes != NULL
    ? priv->grid_modes->len : G_N_ELEMENTS (default_grid_modes);

  if (priv->current_mode == mode_index)
    return;

  priv->current_mode = mode_index;

  if (mode_index != -1 && (guint) mode_index < n_modes)
    {
      g_object_set (priv->layout,
                    "rows-per-page", modes[mode_index].rows,
                    "columns-per-page", modes[mode_index].columns,
                    NULL);
    }
}

static void
find_best_mode_for_size (ShellIconGrid *self,
                         float          width,
                         float          height)
{
  ShellIconGridPrivate *priv = shell_icon_grid_get_instance_private (self);
  const GridMode *modes = priv->grid_modes != NULL
    ? (const GridMode *) priv->grid_modes->data : default_grid_modes;
  guint n_modes = priv->grid_modes != NULL
    ? priv->grid_modes->len : G_N_ELEMENTS (default_grid_modes);
  double size_ratio, closest_ratio = INFINITY;
  int best_mode = -1;
  guint i;

  width -= priv->layout->page_padding.left + priv->layout->page_padding.right;
  height -= priv->layout->page_padding.top + priv->layout->page_padding.bottom;

  size_ratio = (double) width / height;

  for (i = 0; i < n_modes; i++)
    {
      double mode_ratio = (double) modes[i].columns / modes[i].rows;

      if (fabs (size_ratio - mode_ratio) < fabs (size_ratio - closest_ratio))
        {
          closest_ratio = mode_ratio;
          best_mode = (int) i;
        }
    }

  set_grid_mode (self, best_mode);
}

static void
shell_icon_grid_allocate (ClutterActor          *actor,
                          const ClutterActorBox *box)
{
  ShellIconGrid *self = SHELL_ICON_GRID (actor);
  ShellIconGridPrivate *priv = shell_icon_grid_get_instance_private (self);
  float width, height;

  clutter_actor_box_get_size (box, &width, &height);

  if (priv->layout == NULL)
    {
      CLUTTER_ACTOR_CLASS (shell_icon_grid_parent_class)->allocate (actor, box);
      return;
    }

  find_best_mode_for_size (self, width, height);
  shell_icon_grid_layout_adapt_to_size (priv->layout, width, height);

  CLUTTER_ACTOR_CLASS (shell_icon_grid_parent_class)->allocate (actor, box);
}

static void
shell_icon_grid_style_changed (StWidget *widget)
{
  ShellIconGrid *self = SHELL_ICON_GRID (widget);
  ShellIconGridPrivate *priv = shell_icon_grid_get_instance_private (self);
  StThemeNode *node;
  ClutterMargin padding;
  double value;

  ST_WIDGET_CLASS (shell_icon_grid_parent_class)->style_changed (widget);

  if (priv->layout == NULL)
    return;

  node = st_widget_get_theme_node (widget);

  g_object_set (priv->layout,
                "column-spacing", (int) st_theme_node_get_length (node, "column-spacing"),
                "row-spacing", (int) st_theme_node_get_length (node, "row-spacing"),
                NULL);

  if (st_theme_node_lookup_length (node, "max-column-spacing", FALSE, &value))
    g_object_set (priv->layout, "max-column-spacing", (int) value, NULL);
  else
    g_object_set (priv->layout, "max-column-spacing", -1, NULL);

  if (st_theme_node_lookup_length (node, "max-row-spacing", FALSE, &value))
    g_object_set (priv->layout, "max-row-spacing", (int) value, NULL);
  else
    g_object_set (priv->layout, "max-row-spacing", -1, NULL);

  padding.top = st_theme_node_get_length (node, "page-padding-top");
  padding.right = st_theme_node_get_length (node, "page-padding-right");
  padding.bottom = st_theme_node_get_length (node, "page-padding-bottom");
  padding.left = st_theme_node_get_length (node, "page-padding-left");
  g_object_set (priv->layout, "page-padding", &padding, NULL);
}

/**
 * shell_icon_grid_add_item:
 * @self: a #ShellIconGrid
 * @item: the item to add to the grid, which must not be part of it. It needs
 *   a #ShellBaseIcon among its descendants
 * @page: the page number, -1 for the last page. It must be between 0 and
 *   the number of pages, adding to the page after the last one creates a
 *   page
 * @index: the position in the page, -1 for the end. If it exceeds the
 *   number of items per page, @item is added to the next page
 * @error: return location for a #GError
 *
 * Adds @item to the grid.
 *
 * Returns: %TRUE, or %FALSE with @error set
 */
gboolean
shell_icon_grid_add_item (ShellIconGrid  *self,
                          ClutterActor   *item,
                          int             page,
                          int             index,
                          GError        **error)
{
  ShellIconGridPrivate *priv;

  g_return_val_if_fail (SHELL_IS_ICON_GRID (self), FALSE);

  priv = shell_icon_grid_get_instance_private (self);
  return shell_icon_grid_layout_add_item (priv->layout, item, page, index, error);
}

/**
 * shell_icon_grid_append_item:
 * @self: a #ShellIconGrid
 * @item: the item to append to the grid, which must not be part of it
 * @error: return location for a #GError
 *
 * Appends @item to the grid.
 *
 * Returns: %TRUE, or %FALSE with @error set
 */
gboolean
shell_icon_grid_append_item (ShellIconGrid  *self,
                             ClutterActor   *item,
                             GError        **error)
{
  return shell_icon_grid_add_item (self, item, -1, -1, error);
}

/**
 * shell_icon_grid_move_item:
 * @self: a #ShellIconGrid
 * @item: the item to move, which must be part of the grid
 * @new_page: the new page of @item, -1 for the last one
 * @new_position: the new position of @item in the page, -1 for the end
 * @error: return location for a #GError
 *
 * Moves @item in the grid.
 *
 * Returns: %TRUE, or %FALSE with @error set
 */
gboolean
shell_icon_grid_move_item (ShellIconGrid  *self,
                           ClutterActor   *item,
                           int             new_page,
                           int             new_position,
                           GError        **error)
{
  ShellIconGridPrivate *priv;

  g_return_val_if_fail (SHELL_IS_ICON_GRID (self), FALSE);

  priv = shell_icon_grid_get_instance_private (self);
  if (!shell_icon_grid_layout_move_item (priv->layout, item, new_page, new_position, error))
    return FALSE;

  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
  return TRUE;
}

/**
 * shell_icon_grid_remove_item:
 * @self: a #ShellIconGrid
 * @item: the item to remove, which must be part of the grid
 * @error: return location for a #GError
 *
 * Removes @item from the grid.
 *
 * Returns: %TRUE, or %FALSE with @error set
 */
gboolean
shell_icon_grid_remove_item (ShellIconGrid  *self,
                             ClutterActor   *item,
                             GError        **error)
{
  ShellIconGridPrivate *priv;

  g_return_val_if_fail (SHELL_IS_ICON_GRID (self), FALSE);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (item), FALSE);

  priv = shell_icon_grid_get_instance_private (self);

  if (!clutter_actor_contains (CLUTTER_ACTOR (self), item))
    {
      set_error_invalid (error, "Item %s is not part of the IconGrid",
                         G_OBJECT_TYPE_NAME (item));
      return FALSE;
    }

  return shell_icon_grid_layout_remove_item (priv->layout, item, error);
}

/**
 * shell_icon_grid_get_item_page:
 * @self: a #ShellIconGrid
 * @item: the item
 *
 * Returns: the page @item is in, or -1 if @item is not part of the grid
 */
int
shell_icon_grid_get_item_page (ShellIconGrid *self,
                               ClutterActor  *item)
{
  ShellIconGridPrivate *priv;

  g_return_val_if_fail (SHELL_IS_ICON_GRID (self), -1);

  priv = shell_icon_grid_get_instance_private (self);
  return shell_icon_grid_layout_get_item_page (priv->layout, item);
}

/**
 * shell_icon_grid_get_item_position:
 * @self: a #ShellIconGrid
 * @item: the item
 * @page: (out): return location for the page of @item, -1 if it is not part
 *   of the grid
 * @position: (out): return location for the position of @item among the
 *   visible items of its page, -1 if it is not part of the grid
 *
 * Retrieves the position of @item in its page.
 */
void
shell_icon_grid_get_item_position (ShellIconGrid *self,
                                   ClutterActor  *item,
                                   int           *page,
                                   int           *position)
{
  ShellIconGridPrivate *priv;

  g_return_if_fail (SHELL_IS_ICON_GRID (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (item));

  priv = shell_icon_grid_get_instance_private (self);

  if (!clutter_actor_contains (CLUTTER_ACTOR (self), item))
    {
      if (page)
        *page = -1;
      if (position)
        *position = -1;
      return;
    }

  shell_icon_grid_layout_get_item_position (priv->layout, item, page, position);
}

/**
 * shell_icon_grid_get_item_at:
 * @self: a #ShellIconGrid
 * @page: the page
 * @position: the position among the visible items of the page
 *
 * Returns: (transfer none) (nullable): the item at @page and @position
 */
ClutterActor *
shell_icon_grid_get_item_at (ShellIconGrid *self,
                             int            page,
                             int            position)
{
  ShellIconGridPrivate *priv;

  g_return_val_if_fail (SHELL_IS_ICON_GRID (self), NULL);

  priv = shell_icon_grid_get_instance_private (self);
  return shell_icon_grid_layout_get_item_at (priv->layout, page, position);
}

/**
 * shell_icon_grid_get_items_at_page:
 * @self: a #ShellIconGrid
 * @page: the page index
 * @error: return location for a #GError
 *
 * Retrieves the children at @page, including invisible children.
 *
 * Returns: (transfer container) (element-type Clutter.Actor): the items of
 *   the page, or %NULL with @error set if the page does not exist
 */
GList *
shell_icon_grid_get_items_at_page (ShellIconGrid  *self,
                                   int             page,
                                   GError        **error)
{
  ShellIconGridPrivate *priv;

  g_return_val_if_fail (SHELL_IS_ICON_GRID (self), NULL);

  priv = shell_icon_grid_get_instance_private (self);
  return shell_icon_grid_layout_get_items_at_page (priv->layout, page, error);
}

/**
 * shell_icon_grid_get_drop_target:
 * @self: a #ShellIconGrid
 * @x: the position on the horizontal axis, relative to the grid
 * @y: the position on the vertical axis, relative to the grid
 * @page: (out): return location for the page under (@x, @y)
 * @position: (out): return location for the position of the item under
 *   (@x, @y), -1 if there is none
 * @location: (out): return location for where (@x, @y) is relative to the item
 *
 * Retrieves the item located at (@x, @y), as well as the drag location.
 */
void
shell_icon_grid_get_drop_target (ShellIconGrid             *self,
                                 double                     x,
                                 double                     y,
                                 int                       *page,
                                 int                       *position,
                                 ShellIconGridDragLocation *location)
{
  ShellIconGridPrivate *priv;

  g_return_if_fail (SHELL_IS_ICON_GRID (self));

  priv = shell_icon_grid_get_instance_private (self);
  shell_icon_grid_layout_get_drop_target (priv->layout, x, y, page, position, location);
}

/**
 * shell_icon_grid_set_grid_modes:
 * @self: a #ShellIconGrid
 * @modes: (nullable): a #GVariant of type `a(ii)` with the (rows, columns)
 *   pairs of the modes the grid chooses from, or %NULL for the default ones
 *
 * Sets the modes the grid chooses from: the grid uses the one whose ratio of
 * columns to rows is closest to the ratio of its size.
 */
void
shell_icon_grid_set_grid_modes (ShellIconGrid *self,
                                GVariant      *modes)
{
  ShellIconGridPrivate *priv;

  g_return_if_fail (SHELL_IS_ICON_GRID (self));
  g_return_if_fail (modes == NULL ||
                    g_variant_is_of_type (modes, G_VARIANT_TYPE ("a(ii)")));

  priv = shell_icon_grid_get_instance_private (self);

  g_clear_pointer (&priv->grid_modes, g_array_unref);

  if (modes != NULL && g_variant_n_children (modes) > 0)
    {
      GVariantIter iter;
      GridMode mode;

      priv->grid_modes = g_array_new (FALSE, FALSE, sizeof (GridMode));

      g_variant_iter_init (&iter, modes);
      while (g_variant_iter_next (&iter, "(ii)", &mode.rows, &mode.columns))
        {
          if (mode.rows > 0 && mode.columns > 0)
            g_array_append_val (priv->grid_modes, mode);
        }

      if (priv->grid_modes->len == 0)
        g_clear_pointer (&priv->grid_modes, g_array_unref);
    }

  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

/**
 * shell_icon_grid_get_current_page:
 * @self: a #ShellIconGrid
 *
 * Returns: the current page, see shell_icon_grid_go_to_page ()
 */
int
shell_icon_grid_get_current_page (ShellIconGrid *self)
{
  ShellIconGridPrivate *priv;

  g_return_val_if_fail (SHELL_IS_ICON_GRID (self), 0);

  priv = shell_icon_grid_get_instance_private (self);
  return priv->current_page;
}

/**
 * shell_icon_grid_get_n_pages:
 * @self: a #ShellIconGrid
 *
 * Returns: the number of pages
 */
int
shell_icon_grid_get_n_pages (ShellIconGrid *self)
{
  ShellIconGridPrivate *priv;

  g_return_val_if_fail (SHELL_IS_ICON_GRID (self), 0);

  priv = shell_icon_grid_get_instance_private (self);
  return get_n_pages (priv->layout);
}

/**
 * shell_icon_grid_get_items_per_page:
 * @self: a #ShellIconGrid
 *
 * Returns: the number of rows times the number of columns of a page
 */
int
shell_icon_grid_get_items_per_page (ShellIconGrid *self)
{
  ShellIconGridPrivate *priv;

  g_return_val_if_fail (SHELL_IS_ICON_GRID (self), 0);

  priv = shell_icon_grid_get_instance_private (self);
  return get_items_per_page (priv->layout);
}

static void
layout_pages_changed_cb (ShellIconGridLayout *layout,
                         ShellIconGrid       *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), grid_props[GRID_PROP_N_PAGES]);
  g_signal_emit (self, grid_signals[GRID_PAGES_CHANGED], 0);
}

static void
grid_destroy_cb (ClutterActor  *actor,
                 ShellIconGrid *self)
{
  ShellIconGridPrivate *priv = shell_icon_grid_get_instance_private (self);

  /* The items go away with the grid, which is no news to its users */
  if (priv->layout != NULL)
    g_signal_handlers_disconnect_by_func (priv->layout, layout_pages_changed_cb, self);
}

static void
shell_icon_grid_constructed (GObject *object)
{
  ShellIconGrid *self = SHELL_ICON_GRID (object);
  ShellIconGridPrivate *priv = shell_icon_grid_get_instance_private (self);
  ClutterLayoutManager *layout;

  G_OBJECT_CLASS (shell_icon_grid_parent_class)->constructed (object);

  /* The layout parameters of the former IconGrid._init () */
  layout = g_object_new (SHELL_TYPE_ICON_GRID_LAYOUT,
                         "allow-incomplete-pages", FALSE,
                         "orientation", CLUTTER_ORIENTATION_HORIZONTAL,
                         "columns-per-page", 6,
                         "rows-per-page", 4,
                         "page-halign", CLUTTER_ACTOR_ALIGN_FILL,
                         "page-valign", CLUTTER_ACTOR_ALIGN_FILL,
                         "last-row-align", CLUTTER_ACTOR_ALIGN_START,
                         "column-spacing", 0,
                         "row-spacing", 0,
                         NULL);
  clutter_actor_set_layout_manager (CLUTTER_ACTOR (self), layout);
  g_set_weak_pointer (&priv->layout, SHELL_ICON_GRID_LAYOUT (layout));

  g_signal_connect_object (layout, "pages-changed",
                           G_CALLBACK (layout_pages_changed_cb), self, 0);
  g_signal_connect (self, "destroy", G_CALLBACK (grid_destroy_cb), self);

  st_widget_set_style_class_name (ST_WIDGET (self), "icon-grid");
  clutter_actor_set_x_expand (CLUTTER_ACTOR (self), TRUE);
  clutter_actor_set_y_expand (CLUTTER_ACTOR (self), TRUE);
}

static void
shell_icon_grid_dispose (GObject *object)
{
  ShellIconGrid *self = SHELL_ICON_GRID (object);
  ShellIconGridPrivate *priv = shell_icon_grid_get_instance_private (self);

  /* The items are destroyed while ClutterActor disposes, which the layout
   * is still around for */
  G_OBJECT_CLASS (shell_icon_grid_parent_class)->dispose (object);

  g_clear_weak_pointer (&priv->layout);
}

static void
shell_icon_grid_finalize (GObject *object)
{
  ShellIconGrid *self = SHELL_ICON_GRID (object);
  ShellIconGridPrivate *priv = shell_icon_grid_get_instance_private (self);

  g_clear_pointer (&priv->grid_modes, g_array_unref);

  G_OBJECT_CLASS (shell_icon_grid_parent_class)->finalize (object);
}

static void
shell_icon_grid_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  ShellIconGrid *self = SHELL_ICON_GRID (object);
  ShellIconGridPrivate *priv = shell_icon_grid_get_instance_private (self);

  switch (prop_id)
    {
    case GRID_PROP_CURRENT_PAGE:
      {
        g_autoptr (GError) error = NULL;

        if (!shell_icon_grid_go_to_page (self, g_value_get_int (value), TRUE, &error))
          g_warning ("%s: %s", G_STRFUNC, error->message);
      }
      break;

    case GRID_PROP_ALLOW_INCOMPLETE_PAGES:
    case GRID_PROP_ORIENTATION:
    case GRID_PROP_COLUMNS_PER_PAGE:
    case GRID_PROP_ROWS_PER_PAGE:
    case GRID_PROP_PAGE_HALIGN:
    case GRID_PROP_PAGE_VALIGN:
    case GRID_PROP_LAST_ROW_ALIGN:
    case GRID_PROP_COLUMN_SPACING:
    case GRID_PROP_ROW_SPACING:
    case GRID_PROP_PAGE_PADDING:
      if (priv->layout != NULL)
        g_object_set_property (G_OBJECT (priv->layout),
                               forwarded_props[prop_id], value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_icon_grid_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  ShellIconGrid *self = SHELL_ICON_GRID (object);
  ShellIconGridPrivate *priv = shell_icon_grid_get_instance_private (self);

  switch (prop_id)
    {
    case GRID_PROP_CURRENT_PAGE:
      g_value_set_int (value, priv->current_page);
      break;

    case GRID_PROP_N_PAGES:
      g_value_set_int (value, priv->layout != NULL ? get_n_pages (priv->layout) : 0);
      break;

    case GRID_PROP_ITEMS_PER_PAGE:
      g_value_set_int (value, priv->layout != NULL ? get_items_per_page (priv->layout) : 0);
      break;

    case GRID_PROP_ALLOW_INCOMPLETE_PAGES:
    case GRID_PROP_ORIENTATION:
    case GRID_PROP_COLUMNS_PER_PAGE:
    case GRID_PROP_ROWS_PER_PAGE:
    case GRID_PROP_PAGE_HALIGN:
    case GRID_PROP_PAGE_VALIGN:
    case GRID_PROP_LAST_ROW_ALIGN:
    case GRID_PROP_COLUMN_SPACING:
    case GRID_PROP_ROW_SPACING:
    case GRID_PROP_PAGE_PADDING:
      if (priv->layout != NULL)
        g_object_get_property (G_OBJECT (priv->layout),
                               forwarded_props[prop_id], value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_icon_grid_class_init (ShellIconGridClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);
  GParamFlags rw = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS;

  object_class->constructed = shell_icon_grid_constructed;
  object_class->dispose = shell_icon_grid_dispose;
  object_class->finalize = shell_icon_grid_finalize;
  object_class->set_property = shell_icon_grid_set_property;
  object_class->get_property = shell_icon_grid_get_property;

  actor_class->allocate = shell_icon_grid_allocate;
  actor_class->child_added = shell_icon_grid_child_added;
  actor_class->child_removed = shell_icon_grid_child_removed;

  widget_class->style_changed = shell_icon_grid_style_changed;

  /**
   * ShellIconGrid:current-page:
   *
   * The current page. Setting it is shell_icon_grid_go_to_page () with an
   * animation.
   */
  grid_props[GRID_PROP_CURRENT_PAGE] =
    g_param_spec_int ("current-page", NULL, NULL, 0, G_MAXINT32, 0,
                      rw | G_PARAM_EXPLICIT_NOTIFY);

  /**
   * ShellIconGrid:n-pages:
   *
   * The number of pages.
   */
  grid_props[GRID_PROP_N_PAGES] =
    g_param_spec_int ("n-pages", NULL, NULL, 0, G_MAXINT32, 0,
                      G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellIconGrid:items-per-page:
   *
   * The number of items on a page, rows times columns.
   */
  grid_props[GRID_PROP_ITEMS_PER_PAGE] =
    g_param_spec_int ("items-per-page", NULL, NULL, 0, G_MAXINT32, 0,
                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /* The next ones are the properties of the layout, set through the grid */
  grid_props[GRID_PROP_ALLOW_INCOMPLETE_PAGES] =
    g_param_spec_boolean ("allow-incomplete-pages", NULL, NULL, FALSE, rw);
  grid_props[GRID_PROP_ORIENTATION] =
    g_param_spec_enum ("orientation", NULL, NULL,
                       CLUTTER_TYPE_ORIENTATION, CLUTTER_ORIENTATION_HORIZONTAL, rw);
  grid_props[GRID_PROP_COLUMNS_PER_PAGE] =
    g_param_spec_int ("columns-per-page", NULL, NULL, 1, G_MAXINT32, 6, rw);
  grid_props[GRID_PROP_ROWS_PER_PAGE] =
    g_param_spec_int ("rows-per-page", NULL, NULL, 1, G_MAXINT32, 4, rw);
  grid_props[GRID_PROP_PAGE_HALIGN] =
    g_param_spec_enum ("page-halign", NULL, NULL,
                       CLUTTER_TYPE_ACTOR_ALIGN, CLUTTER_ACTOR_ALIGN_FILL, rw);
  grid_props[GRID_PROP_PAGE_VALIGN] =
    g_param_spec_enum ("page-valign", NULL, NULL,
                       CLUTTER_TYPE_ACTOR_ALIGN, CLUTTER_ACTOR_ALIGN_FILL, rw);
  grid_props[GRID_PROP_LAST_ROW_ALIGN] =
    g_param_spec_enum ("last-row-align", NULL, NULL,
                       CLUTTER_TYPE_ACTOR_ALIGN, CLUTTER_ACTOR_ALIGN_START, rw);
  grid_props[GRID_PROP_COLUMN_SPACING] =
    g_param_spec_int ("column-spacing", NULL, NULL, 0, G_MAXINT32, 0, rw);
  grid_props[GRID_PROP_ROW_SPACING] =
    g_param_spec_int ("row-spacing", NULL, NULL, 0, G_MAXINT32, 0, rw);
  grid_props[GRID_PROP_PAGE_PADDING] =
    g_param_spec_boxed ("page-padding", NULL, NULL, CLUTTER_TYPE_MARGIN, rw);

  g_object_class_install_properties (object_class, GRID_N_PROPS, grid_props);

  /**
   * ShellIconGrid::pages-changed:
   * @self: the #ShellIconGrid
   *
   * Emitted when a page is added or removed.
   */
  grid_signals[GRID_PAGES_CHANGED] =
    g_signal_new ("pages-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_icon_grid_init (ShellIconGrid *self)
{
  ShellIconGridPrivate *priv = shell_icon_grid_get_instance_private (self);

  priv->current_mode = -1;
}
