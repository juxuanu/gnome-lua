/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * magnifier.c: The screen magnifier of the accessibility menu: a list of zoom
 * regions showing a magnified clone of the `uiGroup`, the magnified mouse
 * pointer and the crosshairs. Port of js/ui/magnifier.js.
 *
 * Copyright 2011 Red Hat, Inc.
 * Copyright 2012 Inclusive Design Research Centre, OCAD University.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>
#include <string.h>

#include <atspi/atspi.h>
#include <gdesktop-enums.h>
#include <gio/gio.h>
#include <meta/meta-backend.h>
#include <meta/meta-cursor-tracker.h>
#include <meta/meta-later.h>
#include <meta/display.h>
#include <meta/compositor.h>
#include <meta/prefs.h>
#include <meta/window.h>

#include "shell-global.h"
#include "shell-invert-lightness-effect.h"
#include "shell-util.h"
#include "st.h"
#include "ui/background.h"
#include "ui/environment.h"
#include "ui/focus-caret-tracker.h"
#include "ui/layout.h"
#include "ui/magnifier.h"
#include "ui/main.h"

#define CROSSHAIRS_CLIP_WIDTH 100
#define CROSSHAIRS_CLIP_HEIGHT 100
#define NO_CHANGE 0.0

#define POINTER_REST_TIME 1000 /* milliseconds */

#define CLONE_EASE_DURATION 100 /* milliseconds */

/* Settings */
#define MAGNIFIER_SCHEMA          "org.gnome.desktop.a11y.magnifier"
#define SCREEN_POSITION_KEY       "screen-position"
#define MAG_FACTOR_KEY            "mag-factor"
#define INVERT_LIGHTNESS_KEY      "invert-lightness"
#define COLOR_SATURATION_KEY      "color-saturation"
#define BRIGHT_RED_KEY            "brightness-red"
#define BRIGHT_GREEN_KEY          "brightness-green"
#define BRIGHT_BLUE_KEY           "brightness-blue"
#define CONTRAST_RED_KEY          "contrast-red"
#define CONTRAST_GREEN_KEY        "contrast-green"
#define CONTRAST_BLUE_KEY         "contrast-blue"
#define LENS_MODE_KEY             "lens-mode"
#define CLAMP_MODE_KEY            "scroll-at-edges"
#define MOUSE_TRACKING_KEY        "mouse-tracking"
#define FOCUS_TRACKING_KEY        "focus-tracking"
#define CARET_TRACKING_KEY        "caret-tracking"
#define SHOW_CROSS_HAIRS_KEY      "show-cross-hairs"
#define CROSS_HAIRS_THICKNESS_KEY "cross-hairs-thickness"
#define CROSS_HAIRS_COLOR_KEY     "cross-hairs-color"
#define CROSS_HAIRS_OPACITY_KEY   "cross-hairs-opacity"
#define CROSS_HAIRS_LENGTH_KEY    "cross-hairs-length"
#define CROSS_HAIRS_CLIP_KEY      "cross-hairs-clip"

/* Math.round () */
static double
js_round (double value)
{
  return floor (value + 0.5);
}

/* ------------------------------------------------------------------------ */
/* ShellMouseSpriteContent: the content of the magnified mouse pointer. Private */
/* to this file.                                                             */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MOUSE_SPRITE_CONTENT (shell_mouse_sprite_content_get_type ())
G_DECLARE_FINAL_TYPE (ShellMouseSpriteContent, shell_mouse_sprite_content,
                      SHELL, MOUSE_SPRITE_CONTENT, GObject)

struct _ShellMouseSpriteContent
{
  GObject parent;

  double scale;
  double monitor_scale;
  CoglTexture *texture;
};

static void mouse_sprite_content_iface_init (ClutterContentInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellMouseSpriteContent,
                               shell_mouse_sprite_content,
                               G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (CLUTTER_TYPE_CONTENT,
                                                      mouse_sprite_content_iface_init))

static gboolean
mouse_sprite_content_get_preferred_size (ClutterContent *content,
                                         float          *width,
                                         float          *height)
{
  ShellMouseSpriteContent *self = SHELL_MOUSE_SPRITE_CONTENT (content);

  if (self->texture == NULL)
    {
      *width = 0;
      *height = 0;
      return FALSE;
    }

  *width = cogl_texture_get_width (self->texture) / self->scale;
  *height = cogl_texture_get_height (self->texture) / self->scale;
  return TRUE;
}

static void
mouse_sprite_content_paint_content (ClutterContent      *content,
                                    ClutterActor        *actor,
                                    ClutterPaintNode    *node,
                                    ClutterPaintContext *paint_context)
{
  ShellMouseSpriteContent *self = SHELL_MOUSE_SPRITE_CONTENT (content);
  ClutterScalingFilter min_filter, mag_filter;
  ClutterPaintNode *texture_node;
  ClutterActorBox box;

  if (self->texture == NULL)
    return;

  clutter_actor_get_content_scaling_filters (actor, &min_filter, &mag_filter);
  texture_node = clutter_texture_node_new (self->texture, NULL,
                                           min_filter, mag_filter);
  clutter_paint_node_set_static_name (texture_node, "MouseSpriteContent");
  clutter_paint_node_add_child (node, texture_node);

  clutter_actor_get_content_box (actor, &box);
  clutter_paint_node_add_rectangle (texture_node, &box);
  clutter_paint_node_unref (texture_node);
}

static void
mouse_sprite_content_iface_init (ClutterContentInterface *iface)
{
  iface->get_preferred_size = mouse_sprite_content_get_preferred_size;
  iface->paint_content = mouse_sprite_content_paint_content;
}

static double
mouse_sprite_content_texture_scale (ShellMouseSpriteContent *self)
{
  double avg_size;
  int cursor_size;

  if (self->texture == NULL)
    return 1;

  /* This is a workaround to guess the sprite scale; while it works fine
   * in normal scenarios, it's not guaranteed to work in all the cases,
   * and so we should actually add an API to mutter that will allow us
   * to know the real sprite texture scaling in order to adapt it to the
   * wanted one. */
  cursor_size = meta_prefs_get_cursor_size ();
  if (cursor_size <= 0)
    return 1;

  avg_size = (cogl_texture_get_width (self->texture) +
              cogl_texture_get_height (self->texture)) / 2.0;
  return MAX (1, floor (avg_size / cursor_size + .1));
}

static gboolean
mouse_sprite_content_recompute_scale (ShellMouseSpriteContent *self)
{
  double scale = mouse_sprite_content_texture_scale (self) / self->monitor_scale;

  if (self->scale != scale)
    {
      self->scale = scale;
      return TRUE;
    }

  return FALSE;
}

static void
mouse_sprite_content_set_texture (ShellMouseSpriteContent *self,
                                  CoglTexture             *texture)
{
  g_autoptr (CoglTexture) old_texture = NULL;

  if (self->texture == texture)
    return;

  old_texture = self->texture;
  self->texture = texture != NULL ? g_object_ref (texture) : NULL;
  clutter_content_invalidate (CLUTTER_CONTENT (self));

  if (old_texture == NULL || texture == NULL ||
      cogl_texture_get_width (old_texture) != cogl_texture_get_width (texture) ||
      cogl_texture_get_height (old_texture) != cogl_texture_get_height (texture))
    {
      mouse_sprite_content_recompute_scale (self);
      clutter_content_invalidate_size (CLUTTER_CONTENT (self));
    }
}

static void
mouse_sprite_content_set_monitor_scale (ShellMouseSpriteContent *self,
                                        double                   monitor_scale)
{
  self->monitor_scale = monitor_scale;
  if (mouse_sprite_content_recompute_scale (self))
    clutter_content_invalidate_size (CLUTTER_CONTENT (self));
}

static void
shell_mouse_sprite_content_dispose (GObject *object)
{
  ShellMouseSpriteContent *self = SHELL_MOUSE_SPRITE_CONTENT (object);

  g_clear_object (&self->texture);

  G_OBJECT_CLASS (shell_mouse_sprite_content_parent_class)->dispose (object);
}

static void
shell_mouse_sprite_content_class_init (ShellMouseSpriteContentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_mouse_sprite_content_dispose;
}

static void
shell_mouse_sprite_content_init (ShellMouseSpriteContent *self)
{
  self->scale = 1.0;
  self->monitor_scale = 1.0;
  self->texture = NULL;
}

/* ------------------------------------------------------------------------ */
/* ShellMagnifier: instance data, shared with ShellZoomRegion                */
/* ------------------------------------------------------------------------ */

enum
{
  ACTIVE_CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct _ShellMagnifier
{
  GObject parent;

  /* Magnifier is a manager of ZoomRegions. */
  GPtrArray *zoom_regions;

  /* Things of the shell, all weak: they are not to be touched after they are
   * gone, as the magnifier is torn down along with them. */
  MetaCursorTracker *cursor_tracker;
  MetaCompositor *compositor;
  MetaDisplay *display;
  ClutterStage *stage;
  ClutterSeat *seat;
  ShellLayoutManager *layout_manager;
  ClutterActor *ui_group;

  gulong pointer_position_id;
  guint pointer_position_later_id;
  gulong cursor_changed_id;

  /* Small clutter tree for the magnified mouse. */
  ClutterActor *cursor_root;
  ClutterActor *mouse_sprite; /* weak, owned by cursor_root */
  ShellMouseSpriteContent *mouse_sprite_content;

  double x_mouse;
  double y_mouse;

  GSettings *settings;
  ShellMagnifierCrosshairs *cross_hairs;

  gboolean cursor_unfocus_inhibited;
  gboolean cursor_visible_inhibited;
};

G_DEFINE_FINAL_TYPE (ShellMagnifier, shell_magnifier, G_TYPE_OBJECT)

static void
magnifier_get_screen_size (ShellMagnifier *self,
                           int            *width,
                           int            *height)
{
  int w = 0, h = 0;

  if (self != NULL && self->display != NULL)
    meta_display_get_size (self->display, &w, &h);

  *width = w;
  *height = h;
}

static int
magnifier_get_scale_factor (ShellMagnifier *self)
{
  if (self == NULL || self->stage == NULL)
    return 1;

  return st_theme_context_get_scale_factor (st_theme_context_get_for_stage (self->stage));
}

/* ------------------------------------------------------------------------ */
/* ShellMagnifierCrosshairs                                                  */
/* ------------------------------------------------------------------------ */

struct _ShellMagnifierCrosshairs
{
  ClutterActor parent;

  ClutterActor *horiz_left_hair;
  ClutterActor *horiz_right_hair;
  ClutterActor *vert_top_hair;
  ClutterActor *vert_bottom_hair;

  double clip_width;
  double clip_height;

  MetaDisplay *display;               /* weak */
  ShellLayoutManager *layout_manager; /* weak */
  gulong monitors_changed_id;
};

G_DEFINE_FINAL_TYPE (ShellMagnifierCrosshairs, shell_magnifier_crosshairs,
                     CLUTTER_TYPE_ACTOR)

static void
crosshairs_get_screen_size (ShellMagnifierCrosshairs *self,
                            int                      *width,
                            int                      *height)
{
  int w = 0, h = 0;

  if (self->display != NULL)
    meta_display_get_size (self->display, &w, &h);

  *width = w;
  *height = h;
}

/**
 * shell_magnifier_crosshairs_re_center:
 * @self: a #ShellMagnifierCrosshairs
 *
 * Repositions the horizontal and vertical hairs such that they cross at the
 * center of the crosshairs group, leaving the clip rectangle in between.
 */
void
shell_magnifier_crosshairs_re_center (ShellMagnifierCrosshairs *self)
{
  float group_width, group_height;
  float left_length, top_length, thickness;
  double left, right, top, bottom;

  g_return_if_fail (SHELL_IS_MAGNIFIER_CROSSHAIRS (self));

  clutter_actor_get_size (CLUTTER_ACTOR (self), &group_width, &group_height);
  left_length = clutter_actor_get_width (self->horiz_left_hair);
  top_length = clutter_actor_get_height (self->vert_top_hair);
  thickness = clutter_actor_get_height (self->horiz_left_hair);

  left = group_width / 2 - self->clip_width / 2 - left_length - thickness / 2;
  right = group_width / 2 + self->clip_width / 2 + thickness / 2;
  top = group_height / 2 - self->clip_height / 2 - top_length - thickness / 2;
  bottom = group_height / 2 + self->clip_height / 2 + thickness / 2;

  clutter_actor_set_position (self->horiz_left_hair,
                              left, (group_height - thickness) / 2);
  clutter_actor_set_position (self->horiz_right_hair,
                              right, (group_height - thickness) / 2);
  clutter_actor_set_position (self->vert_top_hair,
                              (group_width - thickness) / 2, top);
  clutter_actor_set_position (self->vert_bottom_hair,
                              (group_width - thickness) / 2, bottom);
}

static void
crosshairs_monitors_changed_cb (ShellLayoutManager       *layout_manager,
                                ShellMagnifierCrosshairs *self)
{
  int width, height;

  crosshairs_get_screen_size (self, &width, &height);
  clutter_actor_set_size (CLUTTER_ACTOR (self), width * 3, height * 3);
  shell_magnifier_crosshairs_re_center (self);
}

/**
 * shell_magnifier_crosshairs_set_enabled:
 * @self: a #ShellMagnifierCrosshairs
 * @enabled: whether the crosshairs follow the monitor changes
 *
 * Makes the crosshairs group track the size of the screen, or not.
 */
void
shell_magnifier_crosshairs_set_enabled (ShellMagnifierCrosshairs *self,
                                        gboolean                  enabled)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER_CROSSHAIRS (self));

  if (enabled && self->monitors_changed_id == 0)
    {
      if (self->layout_manager == NULL)
        return;

      self->monitors_changed_id =
        g_signal_connect_object (self->layout_manager, "monitors-changed",
                                 G_CALLBACK (crosshairs_monitors_changed_cb),
                                 self, 0);
    }
  else if (!enabled && self->monitors_changed_id != 0)
    {
      if (self->layout_manager != NULL)
        g_clear_signal_handler (&self->monitors_changed_id, self->layout_manager);
      self->monitors_changed_id = 0;
    }
}

/**
 * shell_magnifier_crosshairs_add_to_zoom_region:
 * @self: a #ShellMagnifierCrosshairs
 * @zoom_region: the zoom region to add the crosshairs to
 * @magnified_mouse: the mouse actor of the zoom region, used to position the
 *   crosshairs and to layer them below the mouse
 *
 * Either adds the crosshairs actor to the given zoom region or, if it is
 * already part of some other zoom region, creates a clone of the crosshairs
 * actor and adds the clone instead.
 *
 * Returns: (transfer none) (nullable): the crosshairs actor, or its clone
 */
ClutterActor *
shell_magnifier_crosshairs_add_to_zoom_region (ShellMagnifierCrosshairs *self,
                                               ShellZoomRegion          *zoom_region,
                                               ClutterActor             *magnified_mouse)
{
  ClutterActor *crosshairs_actor = NULL;
  ClutterActor *container;
  float x_mouse, y_mouse, crosshairs_width, crosshairs_height;

  g_return_val_if_fail (SHELL_IS_MAGNIFIER_CROSSHAIRS (self), NULL);

  if (zoom_region == NULL || magnified_mouse == NULL)
    return NULL;

  container = clutter_actor_get_parent (magnified_mouse);
  if (container == NULL)
    return NULL;

  crosshairs_actor = CLUTTER_ACTOR (self);
  if (clutter_actor_get_parent (CLUTTER_ACTOR (self)) != NULL)
    {
      crosshairs_actor = clutter_clone_new (CLUTTER_ACTOR (self));

      /* Clones don't share visibility. */
      g_object_bind_property (self, "visible",
                              crosshairs_actor, "visible",
                              G_BINDING_SYNC_CREATE);
    }

  clutter_actor_add_child (container, crosshairs_actor);
  clutter_actor_set_child_above_sibling (container, magnified_mouse,
                                         crosshairs_actor);
  clutter_actor_get_position (magnified_mouse, &x_mouse, &y_mouse);
  clutter_actor_get_size (crosshairs_actor, &crosshairs_width, &crosshairs_height);
  clutter_actor_set_position (crosshairs_actor,
                              x_mouse - crosshairs_width / 2,
                              y_mouse - crosshairs_height / 2);

  return crosshairs_actor;
}

/**
 * shell_magnifier_crosshairs_remove_from_parent:
 * @self: a #ShellMagnifierCrosshairs
 * @child_actor: the actor returned from
 *   shell_magnifier_crosshairs_add_to_zoom_region ()
 *
 * Removes the crosshairs actor from its parent container, or destroys
 * @child_actor if it was just a clone of the crosshairs actor.
 */
void
shell_magnifier_crosshairs_remove_from_parent (ShellMagnifierCrosshairs *self,
                                               ClutterActor             *child_actor)
{
  ClutterActor *parent;

  g_return_if_fail (SHELL_IS_MAGNIFIER_CROSSHAIRS (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (child_actor));

  if (child_actor == CLUTTER_ACTOR (self))
    {
      parent = clutter_actor_get_parent (child_actor);
      if (parent != NULL)
        clutter_actor_remove_child (parent, child_actor);
    }
  else
    {
      clutter_actor_destroy (child_actor);
    }
}

/**
 * shell_magnifier_crosshairs_set_color:
 * @self: a #ShellMagnifierCrosshairs
 * @color: the color of the hairs
 */
void
shell_magnifier_crosshairs_set_color (ShellMagnifierCrosshairs *self,
                                      const CoglColor          *color)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER_CROSSHAIRS (self));
  g_return_if_fail (color != NULL);

  clutter_actor_set_background_color (self->horiz_left_hair, color);
  clutter_actor_set_background_color (self->horiz_right_hair, color);
  clutter_actor_set_background_color (self->vert_top_hair, color);
  clutter_actor_set_background_color (self->vert_bottom_hair, color);
}

/**
 * shell_magnifier_crosshairs_set_thickness:
 * @self: a #ShellMagnifierCrosshairs
 * @thickness: the width of the vertical and horizontal lines
 */
void
shell_magnifier_crosshairs_set_thickness (ShellMagnifierCrosshairs *self,
                                          double                    thickness)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER_CROSSHAIRS (self));

  clutter_actor_set_height (self->horiz_left_hair, thickness);
  clutter_actor_set_height (self->horiz_right_hair, thickness);
  clutter_actor_set_width (self->vert_top_hair, thickness);
  clutter_actor_set_width (self->vert_bottom_hair, thickness);
  shell_magnifier_crosshairs_re_center (self);
}

/**
 * shell_magnifier_crosshairs_get_thickness:
 * @self: a #ShellMagnifierCrosshairs
 *
 * Returns: the width of the vertical and horizontal lines
 */
double
shell_magnifier_crosshairs_get_thickness (ShellMagnifierCrosshairs *self)
{
  g_return_val_if_fail (SHELL_IS_MAGNIFIER_CROSSHAIRS (self), 0);

  return clutter_actor_get_height (self->horiz_left_hair);
}

/**
 * shell_magnifier_crosshairs_set_hair_opacity:
 * @self: a #ShellMagnifierCrosshairs
 * @opacity: how opaque the hairs are, between 0 (fully transparent) and 255
 *   (fully opaque); other values are clamped
 *
 * This is what `setOpacity ()` of the JS class was; it is not
 * clutter_actor_set_opacity (), which affects the whole group.
 */
void
shell_magnifier_crosshairs_set_hair_opacity (ShellMagnifierCrosshairs *self,
                                             double                    opacity)
{
  guint8 value;

  g_return_if_fail (SHELL_IS_MAGNIFIER_CROSSHAIRS (self));

  value = (guint8) CLAMP (opacity, 0, 255);
  clutter_actor_set_opacity (self->horiz_left_hair, value);
  clutter_actor_set_opacity (self->horiz_right_hair, value);
  clutter_actor_set_opacity (self->vert_top_hair, value);
  clutter_actor_set_opacity (self->vert_bottom_hair, value);
}

/**
 * shell_magnifier_crosshairs_get_hair_opacity:
 * @self: a #ShellMagnifierCrosshairs
 *
 * Returns: the opacity of the hairs, between 0 and 255
 */
double
shell_magnifier_crosshairs_get_hair_opacity (ShellMagnifierCrosshairs *self)
{
  g_return_val_if_fail (SHELL_IS_MAGNIFIER_CROSSHAIRS (self), 0);

  return clutter_actor_get_opacity (self->horiz_left_hair);
}

/**
 * shell_magnifier_crosshairs_set_length:
 * @self: a #ShellMagnifierCrosshairs
 * @length: the length of the vertical and horizontal lines
 */
void
shell_magnifier_crosshairs_set_length (ShellMagnifierCrosshairs *self,
                                       double                    length)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER_CROSSHAIRS (self));

  clutter_actor_set_width (self->horiz_left_hair, length);
  clutter_actor_set_width (self->horiz_right_hair, length);
  clutter_actor_set_height (self->vert_top_hair, length);
  clutter_actor_set_height (self->vert_bottom_hair, length);
  shell_magnifier_crosshairs_re_center (self);
}

/**
 * shell_magnifier_crosshairs_get_length:
 * @self: a #ShellMagnifierCrosshairs
 *
 * Returns: the length of the vertical and horizontal lines
 */
double
shell_magnifier_crosshairs_get_length (ShellMagnifierCrosshairs *self)
{
  g_return_val_if_fail (SHELL_IS_MAGNIFIER_CROSSHAIRS (self), 0);

  return clutter_actor_get_width (self->horiz_left_hair);
}

/**
 * shell_magnifier_crosshairs_set_clip_size:
 * @self: a #ShellMagnifierCrosshairs
 * @width: the width of the rectangle that clips the crosshairs at their
 *   intersection, 0 for no clip
 * @height: the height of that rectangle
 */
void
shell_magnifier_crosshairs_set_clip_size (ShellMagnifierCrosshairs *self,
                                          double                    width,
                                          double                    height)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER_CROSSHAIRS (self));

  /* Take a chunk out of the crosshairs where it intersects the mouse, or
   * restore the missing chunk. */
  self->clip_width = width;
  self->clip_height = height;
  shell_magnifier_crosshairs_re_center (self);
}

/**
 * shell_magnifier_crosshairs_get_clip_size:
 * @self: a #ShellMagnifierCrosshairs
 * @width: (out) (optional): return location for the width of the clip
 * @height: (out) (optional): return location for the height of the clip
 */
void
shell_magnifier_crosshairs_get_clip_size (ShellMagnifierCrosshairs *self,
                                          double                   *width,
                                          double                   *height)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER_CROSSHAIRS (self));

  if (width != NULL)
    *width = self->clip_width;
  if (height != NULL)
    *height = self->clip_height;
}

static void
shell_magnifier_crosshairs_dispose (GObject *object)
{
  ShellMagnifierCrosshairs *self = SHELL_MAGNIFIER_CROSSHAIRS (object);

  if (self->layout_manager != NULL)
    g_clear_signal_handler (&self->monitors_changed_id, self->layout_manager);
  self->monitors_changed_id = 0;

  g_clear_weak_pointer (&self->layout_manager);
  g_clear_weak_pointer (&self->display);

  G_OBJECT_CLASS (shell_magnifier_crosshairs_parent_class)->dispose (object);
}

static ClutterActor *
new_hair (ShellMagnifierCrosshairs *self)
{
  ClutterActor *hair = clutter_actor_new ();

  clutter_actor_add_child (CLUTTER_ACTOR (self), hair);
  return hair;
}

static void
shell_magnifier_crosshairs_constructed (GObject *object)
{
  ShellMagnifierCrosshairs *self = SHELL_MAGNIFIER_CROSSHAIRS (object);
  ShellGlobal *global = shell_global_get ();
  ShellMain *shell_main = shell_main_get_default ();
  int screen_width, screen_height;

  G_OBJECT_CLASS (shell_magnifier_crosshairs_parent_class)->constructed (object);

  g_set_weak_pointer (&self->display, shell_global_get_display (global));
  g_set_weak_pointer (&self->layout_manager,
                      shell_main_get_layout_manager (shell_main));

  /* Set the group containing the crosshairs to three times the desktop
   * size in case the crosshairs need to appear to be infinite in
   * length (i.e., extend beyond the edges of the view they appear in). */
  crosshairs_get_screen_size (self, &screen_width, &screen_height);
  clutter_actor_set_clip_to_allocation (CLUTTER_ACTOR (self), FALSE);
  clutter_actor_set_size (CLUTTER_ACTOR (self),
                          screen_width * 3, screen_height * 3);

  self->horiz_left_hair = new_hair (self);
  self->horiz_right_hair = new_hair (self);
  self->vert_top_hair = new_hair (self);
  self->vert_bottom_hair = new_hair (self);
  self->clip_width = 0;
  self->clip_height = 0;
  self->monitors_changed_id = 0;
  shell_magnifier_crosshairs_re_center (self);
}

static void
shell_magnifier_crosshairs_init (ShellMagnifierCrosshairs *self)
{
}

static void
shell_magnifier_crosshairs_class_init (ShellMagnifierCrosshairsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_magnifier_crosshairs_constructed;
  object_class->dispose = shell_magnifier_crosshairs_dispose;
}

/**
 * shell_magnifier_crosshairs_new:
 *
 * Creates the crosshairs, three times as large as the screen, with no hair
 * size, color or opacity set.
 *
 * Returns: (transfer full): a new #ShellMagnifierCrosshairs
 */
ShellMagnifierCrosshairs *
shell_magnifier_crosshairs_new (void)
{
  return g_object_ref_sink (g_object_new (SHELL_TYPE_MAGNIFIER_CROSSHAIRS, NULL));
}

/* ------------------------------------------------------------------------ */
/* ShellZoomRegion                                                           */
/* ------------------------------------------------------------------------ */

typedef struct
{
  double x_mag_factor;
  double y_mag_factor;
  double x_center;
  double y_center;
  gboolean redo_cursor_tracking;
  gboolean animate;
} ChangeRoiParams;

struct _ShellZoomRegion
{
  GObject parent;

  ShellMagnifier *magnifier; /* weak */
  ShellFocusCaretTracker *focus_caret_tracker;

  int mouse_tracking_mode;
  int focus_tracking_mode;
  int caret_tracking_mode;
  gboolean clamp_scrolling_at_edges;
  gboolean lens_mode;
  int screen_position;
  gboolean invert_lightness;
  double color_saturation;
  double brightness[3]; /* r, g, b */
  double contrast[3];   /* r, g, b */

  /* The actors exist while the region is active. They are owned by the stage
   * and by each other, so they are only tracked weakly. */
  ClutterActor *mag_view;
  ClutterActor *background;
  ClutterActor *ui_group_clone;
  ClutterActor *mouse_source_actor; /* strong */
  ClutterActor *mouse_actor;
  ShellMagnifierCrosshairs *cross_hairs; /* strong */
  ClutterActor *cross_hairs_actor;

  /* MagShaderEffects: the effects of the clipping group */
  ClutterActor *effects_group;
  ClutterEffect *inverse_effect;
  ClutterEffect *brightness_contrast_effect;
  ClutterEffect *desaturate_effect;

  double view_port_x;
  double view_port_y;
  double view_port_width;
  double view_port_height;
  double x_center;
  double y_center;
  double x_mag_factor;
  double y_mag_factor;
  gboolean following_cursor;
  double x_focus;
  double y_focus;
  double x_caret;
  double y_caret;

  guint scroll_contents_timer_id;
  gboolean has_delayed;
  double x_delayed;
  double y_delayed;

  gulong monitors_changed_id;
  gulong caret_moved_id;
  gulong focus_changed_id;
};

G_DEFINE_FINAL_TYPE (ShellZoomRegion, shell_zoom_region, G_TYPE_OBJECT)

static void change_roi (ShellZoomRegion       *self,
                        const ChangeRoiParams *params);
static void update_clone_geometry (ShellZoomRegion *self,
                                   gboolean         animate);

static void
zoom_region_get_screen_size (ShellZoomRegion *self,
                             int             *width,
                             int             *height)
{
  magnifier_get_screen_size (self->magnifier, width, height);
}

static void
zoom_region_get_mouse (ShellZoomRegion *self,
                       double          *x_mouse,
                       double          *y_mouse)
{
  *x_mouse = self->magnifier != NULL ? self->magnifier->x_mouse : 0;
  *y_mouse = self->magnifier != NULL ? self->magnifier->y_mouse : 0;
}

static void
roi_params_init (ShellZoomRegion *self,
                 ChangeRoiParams *params)
{
  params->x_mag_factor = self->x_mag_factor;
  params->y_mag_factor = self->y_mag_factor;
  params->x_center = self->x_center;
  params->y_center = self->y_center;
  params->redo_cursor_tracking = FALSE;
  params->animate = FALSE;
}

static void
zoom_region_set_ui_group_opacity (ShellZoomRegion *self,
                                  guint8           opacity)
{
  if (self->magnifier != NULL && self->magnifier->ui_group != NULL)
    clutter_actor_set_opacity (self->magnifier->ui_group, opacity);
}

/* ---- Focus and caret events ---- */

typedef enum
{
  CONVERT_OK,
  CONVERT_IGNORE,
  CONVERT_FAILED
} ConvertResult;

typedef struct
{
  int x;
  int y;
  int width;
  int height;
} ScreenExtents;

static ConvertResult
convert_extents_to_screen_space (ShellZoomRegion *self,
                                 AtspiAccessible *accessible,
                                 const AtspiRect *extents,
                                 ScreenExtents   *screen_extents,
                                 GError         **error)
{
  g_autoptr (GObject) iter = g_object_ref (G_OBJECT (accessible));
  g_autoptr (GObject) app = NULL;
  g_autoptr (GObject) parent_window = NULL;
  g_autoptr (GError) local_error = NULL;
  g_autoptr (GObject) state_set = NULL;
  gboolean window_active = FALSE;
  gboolean accessible_focused;
  MetaWindow *focus_window;
  MtkRectangle window_rect;
  int scale_factor;

  while (iter != NULL)
    {
      AtspiAccessible *current = ATSPI_ACCESSIBLE (iter);
      AtspiRole role = atspi_accessible_get_role (current, &local_error);
      GObject *next;

      if (local_error != NULL)
        goto validate_failed;

      if (role == ATSPI_ROLE_APPLICATION)
        {
          /* This is the last Accessible we are interested in */
          app = g_steal_pointer (&iter);
          break;
        }
      else if (role == ATSPI_ROLE_FRAME ||
               role == ATSPI_ROLE_DIALOG ||
               role == ATSPI_ROLE_WINDOW)
        {
          g_set_object (&parent_window, iter);
        }

      next = G_OBJECT (atspi_accessible_get_parent (current, &local_error));
      if (local_error != NULL)
        goto validate_failed;

      g_object_unref (iter);
      iter = next;
    }

  /* We don't want to translate our own events to the focus window.
   * They are also already scaled by clutter before being sent, so
   * we don't need to do that here either. */
  if (app != NULL)
    {
      g_autofree char *app_name = atspi_accessible_get_name (ATSPI_ACCESSIBLE (app),
                                                             &local_error);

      if (local_error != NULL)
        goto validate_failed;

      if (g_strcmp0 (app_name, "gnome-shell") == 0)
        {
          screen_extents->x = extents->x;
          screen_extents->y = extents->y;
          screen_extents->width = extents->width;
          screen_extents->height = extents->height;
          return CONVERT_OK;
        }
    }

  /* Only events from the focused widget of the focused window. Some
   * widgets seem to claim to have focus when the window does not so
   * check both. */
  if (parent_window != NULL)
    {
      state_set = G_OBJECT (atspi_accessible_get_state_set (ATSPI_ACCESSIBLE (parent_window)));
      window_active = atspi_state_set_contains (ATSPI_STATE_SET (state_set),
                                                ATSPI_STATE_ACTIVE);
      g_clear_object (&state_set);
    }

  if (!window_active)
    return CONVERT_IGNORE;

  state_set = G_OBJECT (atspi_accessible_get_state_set (accessible));
  accessible_focused = atspi_state_set_contains (ATSPI_STATE_SET (state_set),
                                                 ATSPI_STATE_FOCUSED);
  if (!accessible_focused)
    return CONVERT_IGNORE;

  if (self->magnifier == NULL || self->magnifier->display == NULL)
    return CONVERT_IGNORE;

  focus_window = meta_display_get_focus_window (self->magnifier->display);
  if (focus_window == NULL)
    return CONVERT_IGNORE;

  meta_window_get_client_content_rect (focus_window, &window_rect);
  scale_factor = magnifier_get_scale_factor (self->magnifier);

  screen_extents->x = window_rect.x + (scale_factor * extents->x);
  screen_extents->y = window_rect.y + (scale_factor * extents->y);
  screen_extents->width = scale_factor * extents->width;
  screen_extents->height = scale_factor * extents->height;

  return CONVERT_OK;

validate_failed:
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "Failed to validate parent window: %s", local_error->message);
  return CONVERT_FAILED;
}

static void
center_from_focus_position (ShellZoomRegion *self);
static void
center_from_caret_position (ShellZoomRegion *self);

static void
zoom_region_update_focus (ShellFocusCaretTracker *tracker,
                          AtspiEvent             *event,
                          ShellZoomRegion        *self)
{
  g_autoptr (GObject) component = NULL;
  g_autoptr (GError) error = NULL;
  AtspiRect *extents;
  ScreenExtents screen_extents;
  ConvertResult result;
  double x_focus, y_focus;

  if (event->source == NULL)
    return;

  component = G_OBJECT (atspi_accessible_get_component_iface (event->source));
  if (component == NULL || event->detail1 != 1)
    return;

  extents = atspi_component_get_extents (ATSPI_COMPONENT (component),
                                         ATSPI_COORD_TYPE_WINDOW, &error);
  if (extents == NULL)
    {
      g_message ("Failed to read extents of focused component: %s",
                 error != NULL ? error->message : "unknown error");
      return;
    }

  result = convert_extents_to_screen_space (self, event->source, extents,
                                            &screen_extents, &error);
  g_boxed_free (ATSPI_TYPE_RECT, extents);

  if (result == CONVERT_FAILED)
    {
      g_message ("Failed to read extents of focused component: %s",
                 error->message);
      return;
    }
  else if (result == CONVERT_IGNORE)
    {
      return;
    }

  x_focus = screen_extents.x + (screen_extents.width / 2.0);
  y_focus = screen_extents.y + (screen_extents.height / 2.0);

  if (self->x_focus != x_focus || self->y_focus != y_focus)
    {
      self->x_focus = x_focus;
      self->y_focus = y_focus;
      center_from_focus_position (self);
    }
}

static void
zoom_region_update_caret (ShellFocusCaretTracker *tracker,
                          AtspiEvent             *event,
                          ShellZoomRegion        *self)
{
  g_autoptr (GObject) text = NULL;
  g_autoptr (GError) error = NULL;
  AtspiRect *extents;
  ScreenExtents screen_extents;
  ConvertResult result;
  int caret_offset;
  double x_caret, y_caret;

  if (event->source == NULL)
    return;

  text = G_OBJECT (atspi_accessible_get_text_iface (event->source));
  if (text == NULL)
    return;

  caret_offset = atspi_text_get_caret_offset (ATSPI_TEXT (text), &error);
  if (error != NULL)
    {
      g_message ("Failed to read extents of text caret: %s", error->message);
      return;
    }

  extents = atspi_text_get_character_extents (ATSPI_TEXT (text), caret_offset,
                                              ATSPI_COORD_TYPE_WINDOW, &error);
  if (extents == NULL)
    {
      g_message ("Failed to read extents of text caret: %s",
                 error != NULL ? error->message : "unknown error");
      return;
    }

  result = convert_extents_to_screen_space (self, ATSPI_ACCESSIBLE (text),
                                            extents, &screen_extents, &error);
  g_boxed_free (ATSPI_TYPE_RECT, extents);

  if (result == CONVERT_FAILED)
    {
      g_message ("Failed to read extents of text caret: %s", error->message);
      return;
    }
  else if (result == CONVERT_IGNORE)
    {
      return;
    }

  x_caret = screen_extents.x;
  y_caret = screen_extents.y;

  /* Ignore event(s) if the caret size is none (0x0). This happens a lot if
   * the cursor offset can't be translated into a location. This is a work
   * around. */
  if (screen_extents.width == 0 && screen_extents.height == 0)
    return;

  if (self->x_caret != x_caret || self->y_caret != y_caret)
    {
      self->x_caret = x_caret;
      self->y_caret = y_caret;
      center_from_caret_position (self);
    }
}

/* ---- Geometry ---- */

static gboolean
zoom_region_is_active_internal (ShellZoomRegion *self)
{
  return self->mag_view != NULL;
}

static gboolean
is_mouse_over_region (ShellZoomRegion *self)
{
  /* Return whether the system mouse sprite is over this ZoomRegion. */
  double x_mouse, y_mouse;

  if (!zoom_region_is_active_internal (self))
    return FALSE;

  zoom_region_get_mouse (self, &x_mouse, &y_mouse);

  return x_mouse >= self->view_port_x &&
         x_mouse < (self->view_port_x + self->view_port_width) &&
         y_mouse >= self->view_port_y &&
         y_mouse < (self->view_port_y + self->view_port_height);
}

static gboolean
is_full_screen (ShellZoomRegion *self)
{
  /* Does the magnified view occupy the whole screen? Note that this
   * doesn't necessarily imply
   * self->screen_position = G_DESKTOP_MAGNIFIER_SCREEN_POSITION_FULL_SCREEN. */
  int screen_width, screen_height;

  zoom_region_get_screen_size (self, &screen_width, &screen_height);

  if (self->view_port_x != 0 || self->view_port_y != 0)
    return FALSE;
  if (self->view_port_width != screen_width ||
      self->view_port_height != screen_height)
    return FALSE;
  return TRUE;
}

static void
screen_to_view_port (ShellZoomRegion *self,
                     double           screen_x,
                     double           screen_y,
                     double          *view_x,
                     double          *view_y)
{
  /* Converts coordinates relative to the (unmagnified) screen to coordinates
   * relative to the origin of the magnified view */
  *view_x = self->view_port_width / 2 +
            (screen_x - self->x_center) * self->x_mag_factor;
  *view_y = self->view_port_height / 2 +
            (screen_y - self->y_center) * self->y_mag_factor;
}

static void
get_mouse_position (ShellZoomRegion *self,
                    double          *x_mag_mouse,
                    double          *y_mag_mouse)
{
  double x_mouse, y_mouse;

  zoom_region_get_mouse (self, &x_mouse, &y_mouse);
  screen_to_view_port (self, x_mouse, y_mouse, x_mag_mouse, y_mag_mouse);
  *x_mag_mouse = js_round (*x_mag_mouse);
  *y_mag_mouse = js_round (*y_mag_mouse);
}

static void
get_cross_hairs_position (ShellZoomRegion *self,
                          double          *x,
                          double          *y)
{
  double x_mag_mouse, y_mag_mouse;
  float group_width, group_height;

  get_mouse_position (self, &x_mag_mouse, &y_mag_mouse);
  clutter_actor_get_size (self->cross_hairs_actor, &group_width, &group_height);

  *x = x_mag_mouse - group_width / 2;
  *y = y_mag_mouse - group_height / 2;
}

static void
update_mag_view_geometry (ShellZoomRegion *self)
{
  if (!zoom_region_is_active_internal (self))
    return;

  if (is_full_screen (self))
    st_widget_add_style_class_name (ST_WIDGET (self->mag_view), "full-screen");
  else
    st_widget_remove_style_class_name (ST_WIDGET (self->mag_view), "full-screen");

  clutter_actor_set_size (self->mag_view,
                          self->view_port_width, self->view_port_height);
  clutter_actor_set_position (self->mag_view,
                              self->view_port_x, self->view_port_y);
}

static void
update_clone_geometry (ShellZoomRegion *self,
                       gboolean         animate)
{
  ShellEaseParams params = {
    .duration = animate ? CLONE_EASE_DURATION : 0,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
  double x, y, mouse_x, mouse_y, cross_x, cross_y;
  float source_mouse_scale;

  if (!zoom_region_is_active_internal (self))
    return;

  screen_to_view_port (self, 0, 0, &x, &y);
  shell_actor_ease (self->ui_group_clone, &params, NULL, NULL, NULL,
                    "x", js_round (x),
                    "y", js_round (y),
                    "scale-x", self->x_mag_factor,
                    "scale-y", self->y_mag_factor,
                    NULL);

  get_mouse_position (self, &mouse_x, &mouse_y);
  source_mouse_scale = clutter_actor_get_resource_scale (self->mouse_source_actor);
  shell_actor_ease (self->mouse_actor, &params, NULL, NULL, NULL,
                    "x", mouse_x,
                    "y", mouse_y,
                    "scale-x", self->x_mag_factor / source_mouse_scale,
                    "scale-y", self->y_mag_factor / source_mouse_scale,
                    NULL);

  if (self->cross_hairs_actor != NULL)
    {
      get_cross_hairs_position (self, &cross_x, &cross_y);
      shell_actor_ease (self->cross_hairs_actor, &params, NULL, NULL, NULL,
                        "x", cross_x,
                        "y", cross_y,
                        NULL);
    }
}

static void
update_mouse_position (ShellZoomRegion *self)
{
  double x_mag_mouse, y_mag_mouse;

  get_mouse_position (self, &x_mag_mouse, &y_mag_mouse);
  clutter_actor_set_position (self->mouse_actor, x_mag_mouse, y_mag_mouse);

  if (self->cross_hairs_actor != NULL)
    {
      double cross_x, cross_y;

      get_cross_hairs_position (self, &cross_x, &cross_y);
      clutter_actor_set_position (self->cross_hairs_actor, cross_x, cross_y);
    }
}

static void
set_view_port (ShellZoomRegion *self,
               double           x,
               double           y,
               double           width,
               double           height,
               gboolean         from_roi_update)
{
  /* Sets the position of the zoom region on the screen */
  int screen_width, screen_height;
  gboolean ui_group_is_occluded;

  zoom_region_get_screen_size (self, &screen_width, &screen_height);

  width = js_round (MIN (width, screen_width));
  height = js_round (MIN (height, screen_height));
  x = MAX (x, 0);
  y = MAX (y, 0);

  x = js_round (MIN (x, screen_width - width));
  y = js_round (MIN (y, screen_height - height));

  self->view_port_x = x;
  self->view_port_y = y;
  self->view_port_width = width;
  self->view_port_height = height;

  update_mag_view_geometry (self);

  if (!from_roi_update)
    {
      ChangeRoiParams params;

      roi_params_init (self, &params);
      params.redo_cursor_tracking = self->following_cursor;
      change_roi (self, &params); /* will update mouse */
    }

  if (zoom_region_is_active_internal (self) && is_mouse_over_region (self) &&
      self->magnifier != NULL)
    shell_magnifier_hide_system_cursor (self->magnifier);

  ui_group_is_occluded = zoom_region_is_active_internal (self) && is_full_screen (self);
  zoom_region_set_ui_group_opacity (self, ui_group_is_occluded ? 0 : 255);
}

/* ---- Centering ---- */

static void
get_roi (ShellZoomRegion *self,
         double          *x,
         double          *y,
         double          *width,
         double          *height)
{
  double roi_width = self->view_port_width / self->x_mag_factor;
  double roi_height = self->view_port_height / self->y_mag_factor;

  *x = self->x_center - roi_width / 2;
  *y = self->y_center - roi_height / 2;
  *width = roi_width;
  *height = roi_height;
}

static void
center_from_point_push (ShellZoomRegion *self,
                        double           x_point,
                        double           y_point,
                        double          *x_out,
                        double          *y_out)
{
  double x_roi, y_roi, width_roi, height_roi;
  float cursor_width, cursor_height;
  double x_pos, y_pos, x_roi_right, y_roi_bottom;

  get_roi (self, &x_roi, &y_roi, &width_roi, &height_roi);
  clutter_actor_get_size (self->mouse_source_actor, &cursor_width, &cursor_height);
  x_pos = x_roi + width_roi / 2;
  y_pos = y_roi + height_roi / 2;
  x_roi_right = x_roi + width_roi - cursor_width;
  y_roi_bottom = y_roi + height_roi - cursor_height;

  if (x_point < x_roi)
    x_pos -= x_roi - x_point;
  else if (x_point > x_roi_right)
    x_pos += x_point - x_roi_right;

  if (y_point < y_roi)
    y_pos -= y_roi - y_point;
  else if (y_point > y_roi_bottom)
    y_pos += y_point - y_roi_bottom;

  *x_out = x_pos;
  *y_out = y_pos;
}

static void
center_from_point_proportional (ShellZoomRegion *self,
                                double           x_point,
                                double           y_point,
                                double          *x_out,
                                double          *y_out)
{
  double x_roi, y_roi, width_roi, height_roi;
  int screen_width, screen_height;
  double half_screen_width, half_screen_height;
  double unscaled_padding, x_padding, y_padding, x_proportion, y_proportion;

  get_roi (self, &x_roi, &y_roi, &width_roi, &height_roi);
  zoom_region_get_screen_size (self, &screen_width, &screen_height);
  half_screen_width = screen_width / 2.0;
  half_screen_height = screen_height / 2.0;

  /* We want to pad with a constant distance after zooming, so divide
   * by the magnification factor. */
  unscaled_padding = MIN (self->view_port_width, self->view_port_height) / 5;
  x_padding = unscaled_padding / self->x_mag_factor;
  y_padding = unscaled_padding / self->y_mag_factor;
  x_proportion = (x_point - half_screen_width) / half_screen_width;    /* -1 ... 1 */
  y_proportion = (y_point - half_screen_height) / half_screen_height;  /* -1 ... 1 */

  *x_out = x_point - x_proportion * (width_roi / 2 - x_padding);
  *y_out = y_point - y_proportion * (height_roi / 2 - y_padding);
}

static void
center_from_mouse_position (ShellZoomRegion *self,
                            double          *x_center,
                            double          *y_center)
{
  /* Determines where the center should be given the current cursor
   * position and mouse tracking mode */
  double x_mouse, y_mouse;

  zoom_region_get_mouse (self, &x_mouse, &y_mouse);

  if (self->mouse_tracking_mode == G_DESKTOP_MAGNIFIER_MOUSE_TRACKING_MODE_PROPORTIONAL)
    center_from_point_proportional (self, x_mouse, y_mouse, x_center, y_center);
  else if (self->mouse_tracking_mode == G_DESKTOP_MAGNIFIER_MOUSE_TRACKING_MODE_PUSH)
    center_from_point_push (self, x_mouse, y_mouse, x_center, y_center);
  else if (self->mouse_tracking_mode == G_DESKTOP_MAGNIFIER_MOUSE_TRACKING_MODE_CENTERED)
    {
      *x_center = x_mouse;
      *y_center = y_mouse;
    }
  /* else: should never be hit, the center is left alone */
}

static void
scroll_contents_to_delayed (ShellZoomRegion *self,
                            double           x,
                            double           y)
{
  if (self->following_cursor)
    {
      self->x_delayed = x;
      self->y_delayed = y;
      self->has_delayed = TRUE;
    }
  else
    {
      shell_zoom_region_scroll_contents_to (self, x, y);
    }
}

static void
center_from_caret_position (ShellZoomRegion *self)
{
  double x_caret = self->x_caret;
  double y_caret = self->y_caret;

  if (self->caret_tracking_mode == G_DESKTOP_MAGNIFIER_CARET_TRACKING_MODE_PROPORTIONAL)
    center_from_point_proportional (self, x_caret, y_caret, &x_caret, &y_caret);
  else if (self->caret_tracking_mode == G_DESKTOP_MAGNIFIER_CARET_TRACKING_MODE_PUSH)
    center_from_point_push (self, x_caret, y_caret, &x_caret, &y_caret);
  /* CENTERED: the point is the center */

  scroll_contents_to_delayed (self, x_caret, y_caret);
}

static void
center_from_focus_position (ShellZoomRegion *self)
{
  double x_focus = self->x_focus;
  double y_focus = self->y_focus;

  if (self->focus_tracking_mode == G_DESKTOP_MAGNIFIER_FOCUS_TRACKING_MODE_PROPORTIONAL)
    center_from_point_proportional (self, x_focus, y_focus, &x_focus, &y_focus);
  else if (self->focus_tracking_mode == G_DESKTOP_MAGNIFIER_FOCUS_TRACKING_MODE_PUSH)
    center_from_point_push (self, x_focus, y_focus, &x_focus, &y_focus);
  /* CENTERED: the point is the center */

  scroll_contents_to_delayed (self, x_focus, y_focus);
}

static void
change_roi (ShellZoomRegion       *self,
            const ChangeRoiParams *in_params)
{
  /* Updates the area we are viewing; the magnification factors
   * and center can be set explicitly, or we can recompute
   * the position based on the mouse cursor position */
  ChangeRoiParams params = *in_params;
  int screen_width, screen_height;

  if (params.x_mag_factor <= 0)
    params.x_mag_factor = self->x_mag_factor;
  if (params.y_mag_factor <= 0)
    params.y_mag_factor = self->y_mag_factor;

  self->x_mag_factor = params.x_mag_factor;
  self->y_mag_factor = params.y_mag_factor;

  if (params.redo_cursor_tracking &&
      self->mouse_tracking_mode != G_DESKTOP_MAGNIFIER_MOUSE_TRACKING_MODE_NONE)
    {
      /* This depends on x_mag_factor/y_mag_factor already being updated */
      center_from_mouse_position (self, &params.x_center, &params.y_center);
    }

  zoom_region_get_screen_size (self, &screen_width, &screen_height);

  if (self->clamp_scrolling_at_edges)
    {
      double roi_width = self->view_port_width / self->x_mag_factor;
      double roi_height = self->view_port_height / self->y_mag_factor;

      params.x_center = MIN (params.x_center, screen_width - roi_width / 2);
      params.x_center = MAX (params.x_center, roi_width / 2);
      params.y_center = MIN (params.y_center, screen_height - roi_height / 2);
      params.y_center = MAX (params.y_center, roi_height / 2);
    }

  self->x_center = params.x_center;
  self->y_center = params.y_center;

  /* If in lens mode, move the magnified view such that it is centered
   * over the actual mouse. However, in full screen mode, the "lens" is
   * the size of the screen -- pointless to move such a large lens around. */
  if (self->lens_mode && !is_full_screen (self))
    {
      set_view_port (self,
                     self->x_center - self->view_port_width / 2,
                     self->y_center - self->view_port_height / 2,
                     self->view_port_width,
                     self->view_port_height,
                     TRUE);
    }

  update_clone_geometry (self, params.animate);
}

static void
update_screen_position (ShellZoomRegion *self)
{
  if (self->screen_position == G_DESKTOP_MAGNIFIER_SCREEN_POSITION_NONE)
    {
      set_view_port (self,
                     self->view_port_x, self->view_port_y,
                     self->view_port_width, self->view_port_height,
                     FALSE);
    }
  else
    {
      shell_zoom_region_set_screen_position (self, self->screen_position);
    }
}

/* ---- Effects ---- */

static void
effects_set_invert_lightness (ShellZoomRegion *self,
                              gboolean         invert)
{
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->inverse_effect),
                                  invert);
}

static void
effects_set_color_saturation (ShellZoomRegion *self,
                              double           factor)
{
  clutter_desaturate_effect_set_factor (CLUTTER_DESATURATE_EFFECT (self->desaturate_effect),
                                        1.0 - factor);
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->desaturate_effect),
                                  factor != 1.0);
}

static void
effects_update_brightness_contrast_enabled (ShellZoomRegion *self)
{
  ClutterBrightnessContrastEffect *effect =
    CLUTTER_BRIGHTNESS_CONTRAST_EFFECT (self->brightness_contrast_effect);
  float b_red, b_green, b_blue, c_red, c_green, c_blue;

  clutter_brightness_contrast_effect_get_brightness (effect, &b_red, &b_green, &b_blue);
  clutter_brightness_contrast_effect_get_contrast (effect, &c_red, &c_green, &c_blue);

  /* Enable the effect if the brightness OR contrast change are such that
   * it modifies the brightness and/or contrast. */
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (effect),
                                  b_red != NO_CHANGE || b_green != NO_CHANGE ||
                                  b_blue != NO_CHANGE || c_red != NO_CHANGE ||
                                  c_green != NO_CHANGE || c_blue != NO_CHANGE);
}

static void
effects_set_brightness (ShellZoomRegion *self,
                        double           red,
                        double           green,
                        double           blue)
{
  clutter_brightness_contrast_effect_set_brightness_full (
    CLUTTER_BRIGHTNESS_CONTRAST_EFFECT (self->brightness_contrast_effect),
    red, green, blue);
  effects_update_brightness_contrast_enabled (self);
}

static void
effects_set_contrast (ShellZoomRegion *self,
                      double           red,
                      double           green,
                      double           blue)
{
  clutter_brightness_contrast_effect_set_contrast_full (
    CLUTTER_BRIGHTNESS_CONTRAST_EFFECT (self->brightness_contrast_effect),
    red, green, blue);
  effects_update_brightness_contrast_enabled (self);
}

static gboolean
has_effects (ShellZoomRegion *self)
{
  return self->effects_group != NULL;
}

static void
create_effects (ShellZoomRegion *self,
                ClutterActor    *main_group)
{
  ClutterEffect *inverse = shell_invert_lightness_effect_new ();
  ClutterEffect *brightness_contrast = clutter_brightness_contrast_effect_new ();
  ClutterEffect *desaturate = clutter_desaturate_effect_new (0.0);

  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (inverse), FALSE);
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (brightness_contrast), FALSE);
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (desaturate), FALSE);

  g_set_weak_pointer (&self->effects_group, main_group);
  g_set_weak_pointer (&self->inverse_effect, inverse);
  g_set_weak_pointer (&self->brightness_contrast_effect, brightness_contrast);
  g_set_weak_pointer (&self->desaturate_effect, desaturate);

  clutter_actor_add_effect (main_group, inverse);
  clutter_actor_add_effect (main_group, brightness_contrast);
  clutter_actor_add_effect (main_group, desaturate);
}

/* Remove contrast and brightness effects from the magnified view, and lose the
 * reference to the actor they were applied to. */
static void
destroy_effects (ShellZoomRegion *self)
{
  if (self->effects_group != NULL)
    clutter_actor_clear_effects (self->effects_group);

  g_clear_weak_pointer (&self->desaturate_effect);
  g_clear_weak_pointer (&self->brightness_contrast_effect);
  g_clear_weak_pointer (&self->inverse_effect);
  g_clear_weak_pointer (&self->effects_group);
}

/* ---- Actors ---- */

static void
create_actors (ShellZoomRegion *self)
{
  ShellMagnifier *magnifier = self->magnifier;
  ClutterActor *main_group, *mag_view, *background, *clone, *mouse_actor;

  /* Add a group to clip the contents of the magnified view. */
  main_group = clutter_actor_new ();
  clutter_actor_set_clip_to_allocation (main_group, TRUE);

  /* The root actor for the zoom region */
  mag_view = CLUTTER_ACTOR (st_bin_new ());
  st_widget_set_style_class_name (ST_WIDGET (mag_view), "magnifier-zoom-region");
  st_bin_set_child (ST_BIN (mag_view), main_group);
  clutter_actor_add_child (CLUTTER_ACTOR (magnifier->stage), mag_view);
  g_set_weak_pointer (&self->mag_view, mag_view);

  /* hide the magnified region from CLUTTER_PICK_ALL */
  shell_util_set_hidden_from_pick (mag_view, TRUE);

  /* Add a background for when the magnified uiGroup is scrolled
   * out of view (don't want to see desktop showing through). */
  background = CLUTTER_ACTOR (shell_system_background_new ());
  clutter_actor_add_child (main_group, background);
  g_set_weak_pointer (&self->background, background);

  /* Clone the group that contains all of UI on the screen.  This is the
   * chrome, the windows, etc. */
  clone = clutter_clone_new (magnifier->ui_group);
  clutter_actor_set_clip_to_allocation (clone, TRUE);
  clutter_actor_add_child (main_group, clone);
  g_set_weak_pointer (&self->ui_group_clone, clone);

  /* Add either the given mouse_source_actor to the ZoomRegion, or a clone of
   * it. */
  if (clutter_actor_get_parent (self->mouse_source_actor) != NULL)
    mouse_actor = clutter_clone_new (self->mouse_source_actor);
  else
    mouse_actor = self->mouse_source_actor;
  clutter_actor_add_child (main_group, mouse_actor);
  g_set_weak_pointer (&self->mouse_actor, mouse_actor);

  if (self->cross_hairs != NULL)
    {
      ClutterActor *cross_hairs_actor =
        shell_magnifier_crosshairs_add_to_zoom_region (self->cross_hairs,
                                                       self, mouse_actor);

      g_set_weak_pointer (&self->cross_hairs_actor, cross_hairs_actor);
    }
  else
    {
      g_clear_weak_pointer (&self->cross_hairs_actor);
    }

  /* Contrast and brightness effects. */
  create_effects (self, main_group);
  effects_set_color_saturation (self, self->color_saturation);
  effects_set_invert_lightness (self, self->invert_lightness);
  effects_set_brightness (self, self->brightness[0], self->brightness[1],
                          self->brightness[2]);
  effects_set_contrast (self, self->contrast[0], self->contrast[1],
                        self->contrast[2]);
}

static void
destroy_actors (ShellZoomRegion *self)
{
  if (self->mouse_actor != NULL && self->mouse_actor == self->mouse_source_actor)
    {
      ClutterActor *parent = clutter_actor_get_parent (self->mouse_actor);

      if (parent != NULL)
        clutter_actor_remove_child (parent, self->mouse_actor);
    }

  if (self->cross_hairs != NULL && self->cross_hairs_actor != NULL)
    {
      shell_magnifier_crosshairs_remove_from_parent (self->cross_hairs,
                                                     self->cross_hairs_actor);
    }

  if (has_effects (self))
    destroy_effects (self);

  if (self->mag_view != NULL)
    clutter_actor_destroy (self->mag_view);

  g_clear_weak_pointer (&self->mag_view);
  g_clear_weak_pointer (&self->background);
  g_clear_weak_pointer (&self->ui_group_clone);
  g_clear_weak_pointer (&self->mouse_actor);
  g_clear_weak_pointer (&self->cross_hairs_actor);
}

/* ---- Signals ---- */

static void
zoom_region_monitors_changed_cb (ShellLayoutManager *layout_manager,
                                 ShellZoomRegion    *self)
{
  int screen_width, screen_height;

  zoom_region_get_screen_size (self, &screen_width, &screen_height);
  if (self->background != NULL)
    clutter_actor_set_size (self->background, screen_width, screen_height);
  update_screen_position (self);
}

static void
connect_signals (ShellZoomRegion *self)
{
  ShellLayoutManager *layout_manager;

  if (self->monitors_changed_id != 0)
    return;

  layout_manager = self->magnifier != NULL ? self->magnifier->layout_manager : NULL;
  if (layout_manager != NULL)
    {
      self->monitors_changed_id =
        g_signal_connect_object (layout_manager, "monitors-changed",
                                 G_CALLBACK (zoom_region_monitors_changed_cb),
                                 self, 0);
    }

  self->caret_moved_id =
    g_signal_connect_object (self->focus_caret_tracker, "caret-moved",
                             G_CALLBACK (zoom_region_update_caret), self, 0);
  self->focus_changed_id =
    g_signal_connect_object (self->focus_caret_tracker, "focus-changed",
                             G_CALLBACK (zoom_region_update_focus), self, 0);
}

static void
disconnect_signals (ShellZoomRegion *self)
{
  if (self->monitors_changed_id != 0 &&
      self->magnifier != NULL && self->magnifier->layout_manager != NULL)
    {
      g_clear_signal_handler (&self->monitors_changed_id,
                              self->magnifier->layout_manager);
    }
  self->monitors_changed_id = 0;

  g_clear_signal_handler (&self->caret_moved_id, self->focus_caret_tracker);
  g_clear_signal_handler (&self->focus_changed_id, self->focus_caret_tracker);
}

static void
sync_focus_tracking (ShellZoomRegion *self)
{
  gboolean enabled =
    self->focus_tracking_mode != G_DESKTOP_MAGNIFIER_FOCUS_TRACKING_MODE_NONE &&
    zoom_region_is_active_internal (self);

  if (enabled)
    shell_focus_caret_tracker_register_focus_listener (self->focus_caret_tracker);
  else
    shell_focus_caret_tracker_deregister_focus_listener (self->focus_caret_tracker);
}

static void
sync_caret_tracking (ShellZoomRegion *self)
{
  gboolean enabled =
    self->caret_tracking_mode != G_DESKTOP_MAGNIFIER_CARET_TRACKING_MODE_NONE &&
    zoom_region_is_active_internal (self);

  if (enabled)
    shell_focus_caret_tracker_register_caret_listener (self->focus_caret_tracker);
  else
    shell_focus_caret_tracker_deregister_caret_listener (self->focus_caret_tracker);
}

static void
clear_scroll_contents_timer (ShellZoomRegion *self)
{
  g_clear_handle_id (&self->scroll_contents_timer_id, g_source_remove);
}

static void
scroll_contents_timeout_cb (gpointer user_data)
{
  ShellZoomRegion *self = SHELL_ZOOM_REGION (user_data);

  self->scroll_contents_timer_id = 0;
  self->following_cursor = FALSE;

  if (self->has_delayed)
    {
      double x = self->x_delayed;
      double y = self->y_delayed;

      self->has_delayed = FALSE;
      scroll_contents_to_delayed (self, x, y);
    }
}

/* ---- Public API ---- */

/**
 * shell_zoom_region_set_active:
 * @self: a #ShellZoomRegion
 * @activate: whether to show or hide the zoom region
 */
void
shell_zoom_region_set_active (ShellZoomRegion *self,
                              gboolean         activate)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  activate = !!activate;
  if (activate == zoom_region_is_active_internal (self))
    return;

  if (activate)
    {
      if (self->magnifier == NULL || self->magnifier->stage == NULL ||
          self->magnifier->ui_group == NULL)
        return;

      create_actors (self);
      if (is_mouse_over_region (self))
        shell_magnifier_hide_system_cursor (self->magnifier);
      update_screen_position (self);
      update_mag_view_geometry (self);
      update_clone_geometry (self, FALSE);
      update_mouse_position (self);
      connect_signals (self);
    }
  else
    {
      zoom_region_set_ui_group_opacity (self, 255);
      disconnect_signals (self);
      destroy_actors (self);
    }

  sync_caret_tracking (self);
  sync_focus_tracking (self);
}

/**
 * shell_zoom_region_is_active:
 * @self: a #ShellZoomRegion
 *
 * Returns: whether the zoom region is shown
 */
gboolean
shell_zoom_region_is_active (ShellZoomRegion *self)
{
  g_return_val_if_fail (SHELL_IS_ZOOM_REGION (self), FALSE);

  return zoom_region_is_active_internal (self);
}

/**
 * shell_zoom_region_set_mag_factor:
 * @self: a #ShellZoomRegion
 * @x_mag_factor: the horizontal magnification factor of the magnified view,
 *   1.0 means no magnification and 2.0 doubles the size
 * @y_mag_factor: the vertical magnification factor
 */
void
shell_zoom_region_set_mag_factor (ShellZoomRegion *self,
                                  double           x_mag_factor,
                                  double           y_mag_factor)
{
  ChangeRoiParams params;

  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  roi_params_init (self, &params);
  params.x_mag_factor = x_mag_factor;
  params.y_mag_factor = y_mag_factor;
  params.redo_cursor_tracking = self->following_cursor;
  params.animate = TRUE;
  change_roi (self, &params);
}

/**
 * shell_zoom_region_get_mag_factor:
 * @self: a #ShellZoomRegion
 * @x_mag_factor: (out) (optional): return location for the horizontal
 *   magnification power, 1.0 means no magnification
 * @y_mag_factor: (out) (optional): return location for the vertical
 *   magnification power
 */
void
shell_zoom_region_get_mag_factor (ShellZoomRegion *self,
                                  double          *x_mag_factor,
                                  double          *y_mag_factor)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  if (x_mag_factor != NULL)
    *x_mag_factor = self->x_mag_factor;
  if (y_mag_factor != NULL)
    *y_mag_factor = self->y_mag_factor;
}

/**
 * shell_zoom_region_set_mouse_tracking_mode:
 * @self: a #ShellZoomRegion
 * @mode: a `GDesktopEnums.MagnifierMouseTrackingMode`; invalid values are
 *   ignored
 */
void
shell_zoom_region_set_mouse_tracking_mode (ShellZoomRegion *self,
                                           int              mode)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  if (mode >= G_DESKTOP_MAGNIFIER_MOUSE_TRACKING_MODE_NONE &&
      mode <= G_DESKTOP_MAGNIFIER_MOUSE_TRACKING_MODE_PUSH)
    self->mouse_tracking_mode = mode;
}

/**
 * shell_zoom_region_get_mouse_tracking_mode:
 * @self: a #ShellZoomRegion
 *
 * Returns: the current `GDesktopEnums.MagnifierMouseTrackingMode`
 */
int
shell_zoom_region_get_mouse_tracking_mode (ShellZoomRegion *self)
{
  g_return_val_if_fail (SHELL_IS_ZOOM_REGION (self), 0);

  return self->mouse_tracking_mode;
}

/**
 * shell_zoom_region_set_focus_tracking_mode:
 * @self: a #ShellZoomRegion
 * @mode: a `GDesktopEnums.MagnifierFocusTrackingMode`
 */
void
shell_zoom_region_set_focus_tracking_mode (ShellZoomRegion *self,
                                           int              mode)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  self->focus_tracking_mode = mode;
  sync_focus_tracking (self);
}

/**
 * shell_zoom_region_set_caret_tracking_mode:
 * @self: a #ShellZoomRegion
 * @mode: a `GDesktopEnums.MagnifierCaretTrackingMode`
 */
void
shell_zoom_region_set_caret_tracking_mode (ShellZoomRegion *self,
                                           int              mode)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  self->caret_tracking_mode = mode;
  sync_caret_tracking (self);
}

/**
 * shell_zoom_region_set_view_port:
 * @self: a #ShellZoomRegion
 * @view_port: the position and size of the view port, in stage coordinates
 *
 * Sets the position and size of the zoom region on screen. The screen
 * position of the region becomes `GDesktopEnums.MagnifierScreenPosition.NONE`.
 */
void
shell_zoom_region_set_view_port (ShellZoomRegion       *self,
                                 const graphene_rect_t *view_port)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));
  g_return_if_fail (view_port != NULL);

  set_view_port (self,
                 graphene_rect_get_x (view_port),
                 graphene_rect_get_y (view_port),
                 graphene_rect_get_width (view_port),
                 graphene_rect_get_height (view_port),
                 FALSE);
  self->screen_position = G_DESKTOP_MAGNIFIER_SCREEN_POSITION_NONE;
}

/**
 * shell_zoom_region_set_roi:
 * @self: a #ShellZoomRegion
 * @roi: the region of the screen to magnify, in screen (unmagnified)
 *   coordinates
 *
 * Sets the "region of interest" that the zoom region is magnifying. Empty
 * regions are ignored.
 */
void
shell_zoom_region_set_roi (ShellZoomRegion       *self,
                           const graphene_rect_t *roi)
{
  ChangeRoiParams params;
  double x, y, width, height;

  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));
  g_return_if_fail (roi != NULL);

  x = graphene_rect_get_x (roi);
  y = graphene_rect_get_y (roi);
  width = graphene_rect_get_width (roi);
  height = graphene_rect_get_height (roi);

  if (width <= 0 || height <= 0)
    return;

  self->following_cursor = FALSE;

  roi_params_init (self, &params);
  params.x_mag_factor = self->view_port_width / width;
  params.y_mag_factor = self->view_port_height / height;
  params.x_center = x + width / 2;
  params.y_center = y + height / 2;
  change_roi (self, &params);
}

/**
 * shell_zoom_region_get_roi:
 * @self: a #ShellZoomRegion
 * @roi: (out caller-allocates): return location for the bounding rectangle
 *   of what is shown in the magnified view, in non-magnified coordinates
 */
void
shell_zoom_region_get_roi (ShellZoomRegion *self,
                           graphene_rect_t *roi)
{
  double x, y, width, height;

  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));
  g_return_if_fail (roi != NULL);

  get_roi (self, &x, &y, &width, &height);
  graphene_rect_init (roi, x, y, width, height);
}

/**
 * shell_zoom_region_set_lens_mode:
 * @self: a #ShellZoomRegion
 * @lens_mode: whether lens mode is on
 *
 * Turns lens mode on or off. In full screen mode, lens mode does nothing
 * since a lens the size of the screen is pointless.
 */
void
shell_zoom_region_set_lens_mode (ShellZoomRegion *self,
                                 gboolean         lens_mode)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  self->lens_mode = !!lens_mode;
  if (!self->lens_mode)
    shell_zoom_region_set_screen_position (self, self->screen_position);
}

/**
 * shell_zoom_region_is_lens_mode:
 * @self: a #ShellZoomRegion
 *
 * Returns: whether lens mode is on
 */
gboolean
shell_zoom_region_is_lens_mode (ShellZoomRegion *self)
{
  g_return_val_if_fail (SHELL_IS_ZOOM_REGION (self), FALSE);

  return self->lens_mode;
}

/**
 * shell_zoom_region_set_clamp_scrolling_at_edges:
 * @self: a #ShellZoomRegion
 * @clamp: whether to stop scrolling the magnified contents beyond the edges
 *   of the screen
 */
void
shell_zoom_region_set_clamp_scrolling_at_edges (ShellZoomRegion *self,
                                                gboolean         clamp)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  self->clamp_scrolling_at_edges = !!clamp;
  if (clamp)
    {
      ChangeRoiParams params;

      roi_params_init (self, &params);
      change_roi (self, &params);
    }
}

/**
 * shell_zoom_region_set_top_half:
 * @self: a #ShellZoomRegion
 *
 * The magnified view occupies the top half of the screen.
 */
void
shell_zoom_region_set_top_half (ShellZoomRegion *self)
{
  int screen_width, screen_height;

  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  zoom_region_get_screen_size (self, &screen_width, &screen_height);
  set_view_port (self, 0, 0, screen_width, screen_height / 2.0, FALSE);
  self->screen_position = G_DESKTOP_MAGNIFIER_SCREEN_POSITION_TOP_HALF;
}

/**
 * shell_zoom_region_set_bottom_half:
 * @self: a #ShellZoomRegion
 *
 * The magnified view occupies the bottom half of the screen.
 */
void
shell_zoom_region_set_bottom_half (ShellZoomRegion *self)
{
  int screen_width, screen_height;

  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  zoom_region_get_screen_size (self, &screen_width, &screen_height);
  set_view_port (self, 0, screen_height / 2.0, screen_width, screen_height / 2.0,
                 FALSE);
  self->screen_position = G_DESKTOP_MAGNIFIER_SCREEN_POSITION_BOTTOM_HALF;
}

/**
 * shell_zoom_region_set_left_half:
 * @self: a #ShellZoomRegion
 *
 * The magnified view occupies the left half of the screen.
 */
void
shell_zoom_region_set_left_half (ShellZoomRegion *self)
{
  int screen_width, screen_height;

  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  zoom_region_get_screen_size (self, &screen_width, &screen_height);
  set_view_port (self, 0, 0, screen_width / 2.0, screen_height, FALSE);
  self->screen_position = G_DESKTOP_MAGNIFIER_SCREEN_POSITION_LEFT_HALF;
}

/**
 * shell_zoom_region_set_right_half:
 * @self: a #ShellZoomRegion
 *
 * The magnified view occupies the right half of the screen.
 */
void
shell_zoom_region_set_right_half (ShellZoomRegion *self)
{
  int screen_width, screen_height;

  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  zoom_region_get_screen_size (self, &screen_width, &screen_height);
  set_view_port (self, screen_width / 2.0, 0, screen_width / 2.0, screen_height,
                 FALSE);
  self->screen_position = G_DESKTOP_MAGNIFIER_SCREEN_POSITION_RIGHT_HALF;
}

/**
 * shell_zoom_region_set_full_screen_mode:
 * @self: a #ShellZoomRegion
 *
 * Sets the zoom region to full-screen mode. This disallows lens mode.
 */
void
shell_zoom_region_set_full_screen_mode (ShellZoomRegion *self)
{
  int screen_width, screen_height;

  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  zoom_region_get_screen_size (self, &screen_width, &screen_height);
  set_view_port (self, 0, 0, screen_width, screen_height, FALSE);
  self->screen_position = G_DESKTOP_MAGNIFIER_SCREEN_POSITION_FULL_SCREEN;
}

/**
 * shell_zoom_region_set_screen_position:
 * @self: a #ShellZoomRegion
 * @position: a `GDesktopEnums.MagnifierScreenPosition`
 *
 * Positions the zoom region to one of the enumerated positions on the screen.
 */
void
shell_zoom_region_set_screen_position (ShellZoomRegion *self,
                                       int              position)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  switch (position)
    {
    case G_DESKTOP_MAGNIFIER_SCREEN_POSITION_FULL_SCREEN:
      shell_zoom_region_set_full_screen_mode (self);
      break;
    case G_DESKTOP_MAGNIFIER_SCREEN_POSITION_TOP_HALF:
      shell_zoom_region_set_top_half (self);
      break;
    case G_DESKTOP_MAGNIFIER_SCREEN_POSITION_BOTTOM_HALF:
      shell_zoom_region_set_bottom_half (self);
      break;
    case G_DESKTOP_MAGNIFIER_SCREEN_POSITION_LEFT_HALF:
      shell_zoom_region_set_left_half (self);
      break;
    case G_DESKTOP_MAGNIFIER_SCREEN_POSITION_RIGHT_HALF:
      shell_zoom_region_set_right_half (self);
      break;
    default:
      break;
    }
}

/**
 * shell_zoom_region_get_screen_position:
 * @self: a #ShellZoomRegion
 *
 * Returns: the current `GDesktopEnums.MagnifierScreenPosition`: whether the
 *   top half, bottom half, etc. is magnified
 */
int
shell_zoom_region_get_screen_position (ShellZoomRegion *self)
{
  g_return_val_if_fail (SHELL_IS_ZOOM_REGION (self), 0);

  return self->screen_position;
}

/**
 * shell_zoom_region_scroll_to_mouse_pos:
 * @self: a #ShellZoomRegion
 *
 * Sets the region of interest based on the position of the system pointer.
 *
 * Returns: whether the system mouse pointer is over the magnified view
 */
gboolean
shell_zoom_region_scroll_to_mouse_pos (ShellZoomRegion *self)
{
  g_return_val_if_fail (SHELL_IS_ZOOM_REGION (self), FALSE);

  self->following_cursor = TRUE;
  if (self->mouse_tracking_mode != G_DESKTOP_MAGNIFIER_MOUSE_TRACKING_MODE_NONE)
    {
      ChangeRoiParams params;

      roi_params_init (self, &params);
      params.redo_cursor_tracking = TRUE;
      change_roi (self, &params);
    }
  else if (zoom_region_is_active_internal (self))
    {
      update_clone_geometry (self, FALSE);
      update_mouse_position (self);
    }

  clear_scroll_contents_timer (self);
  self->scroll_contents_timer_id =
    g_timeout_add_once (POINTER_REST_TIME, scroll_contents_timeout_cb, self);

  /* Determine whether the system mouse pointer is over this zoom region. */
  return is_mouse_over_region (self);
}

/**
 * shell_zoom_region_scroll_contents_to:
 * @self: a #ShellZoomRegion
 * @x: the x coordinate of the point to center on
 * @y: the y coordinate of the point to center on
 *
 * Shifts the contents of the magnified view such that it is centered on the
 * given coordinate.
 */
void
shell_zoom_region_scroll_contents_to (ShellZoomRegion *self,
                                      double           x,
                                      double           y)
{
  ChangeRoiParams params;
  int screen_width, screen_height;

  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  zoom_region_get_screen_size (self, &screen_width, &screen_height);
  if (x < 0 || x > screen_width || y < 0 || y > screen_height)
    return;

  clear_scroll_contents_timer (self);

  self->following_cursor = FALSE;

  roi_params_init (self, &params);
  params.x_center = x;
  params.y_center = y;
  params.animate = TRUE;
  change_roi (self, &params);
}

/**
 * shell_zoom_region_add_crosshairs:
 * @self: a #ShellZoomRegion
 * @cross_hairs: (nullable): the crosshairs to show centered on the magnified
 *   mouse
 */
void
shell_zoom_region_add_crosshairs (ShellZoomRegion          *self,
                                  ShellMagnifierCrosshairs *cross_hairs)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));
  g_return_if_fail (cross_hairs == NULL || SHELL_IS_MAGNIFIER_CROSSHAIRS (cross_hairs));

  g_set_object (&self->cross_hairs, cross_hairs);

  /* If the cross_hairs is not already within a larger container, add it
   * to this zoom region.  Otherwise, add a clone. */
  if (cross_hairs != NULL && zoom_region_is_active_internal (self))
    {
      ClutterActor *cross_hairs_actor =
        shell_magnifier_crosshairs_add_to_zoom_region (cross_hairs, self,
                                                       self->mouse_actor);

      g_set_weak_pointer (&self->cross_hairs_actor, cross_hairs_actor);
    }
}

/**
 * shell_zoom_region_set_invert_lightness:
 * @self: a #ShellZoomRegion
 * @flag: whether the lightness of the magnified view is inverted
 */
void
shell_zoom_region_set_invert_lightness (ShellZoomRegion *self,
                                        gboolean         flag)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  self->invert_lightness = !!flag;
  if (has_effects (self))
    effects_set_invert_lightness (self, self->invert_lightness);
}

/**
 * shell_zoom_region_get_invert_lightness:
 * @self: a #ShellZoomRegion
 *
 * Returns: whether the lightness is inverted
 */
gboolean
shell_zoom_region_get_invert_lightness (ShellZoomRegion *self)
{
  g_return_val_if_fail (SHELL_IS_ZOOM_REGION (self), FALSE);

  return self->invert_lightness;
}

/**
 * shell_zoom_region_set_color_saturation:
 * @self: a #ShellZoomRegion
 * @saturation: the color saturation of the magnified view, from 0.0 (no
 *   color, grayscale) to 1.0 (full color)
 */
void
shell_zoom_region_set_color_saturation (ShellZoomRegion *self,
                                        double           saturation)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  self->color_saturation = saturation;
  if (has_effects (self))
    effects_set_color_saturation (self, self->color_saturation);
}

/**
 * shell_zoom_region_get_color_saturation:
 * @self: a #ShellZoomRegion
 *
 * Returns: the color saturation of the magnified view
 */
double
shell_zoom_region_get_color_saturation (ShellZoomRegion *self)
{
  g_return_val_if_fail (SHELL_IS_ZOOM_REGION (self), 0);

  return self->color_saturation;
}

/**
 * shell_zoom_region_set_brightness:
 * @self: a #ShellZoomRegion
 * @red: the brightness of the red channel
 * @green: the brightness of the green channel
 * @blue: the brightness of the blue channel
 *
 * Alters the brightness of the magnified view. Values of 0.0 represent
 * "standard" brightness (no change), whereas values less or greater than 0.0
 * indicate decreased or increased brightness, respectively.
 */
void
shell_zoom_region_set_brightness (ShellZoomRegion *self,
                                  double           red,
                                  double           green,
                                  double           blue)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  self->brightness[0] = red;
  self->brightness[1] = green;
  self->brightness[2] = blue;
  if (has_effects (self))
    effects_set_brightness (self, red, green, blue);
}

/**
 * shell_zoom_region_get_brightness:
 * @self: a #ShellZoomRegion
 * @red: (out) (optional): return location for the red brightness
 * @green: (out) (optional): return location for the green brightness
 * @blue: (out) (optional): return location for the blue brightness
 */
void
shell_zoom_region_get_brightness (ShellZoomRegion *self,
                                  double          *red,
                                  double          *green,
                                  double          *blue)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  if (red != NULL)
    *red = self->brightness[0];
  if (green != NULL)
    *green = self->brightness[1];
  if (blue != NULL)
    *blue = self->brightness[2];
}

/**
 * shell_zoom_region_set_contrast:
 * @self: a #ShellZoomRegion
 * @red: the contrast of the red channel
 * @green: the contrast of the green channel
 * @blue: the contrast of the blue channel
 *
 * Alters the contrast of the magnified view. Values of 0.0 represent
 * "standard" contrast (no change), whereas values less or greater than 0.0
 * indicate decreased or increased contrast, respectively.
 */
void
shell_zoom_region_set_contrast (ShellZoomRegion *self,
                                double           red,
                                double           green,
                                double           blue)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  self->contrast[0] = red;
  self->contrast[1] = green;
  self->contrast[2] = blue;
  if (has_effects (self))
    effects_set_contrast (self, red, green, blue);
}

/**
 * shell_zoom_region_get_contrast:
 * @self: a #ShellZoomRegion
 * @red: (out) (optional): return location for the red contrast
 * @green: (out) (optional): return location for the green contrast
 * @blue: (out) (optional): return location for the blue contrast
 */
void
shell_zoom_region_get_contrast (ShellZoomRegion *self,
                                double          *red,
                                double          *green,
                                double          *blue)
{
  g_return_if_fail (SHELL_IS_ZOOM_REGION (self));

  if (red != NULL)
    *red = self->contrast[0];
  if (green != NULL)
    *green = self->contrast[1];
  if (blue != NULL)
    *blue = self->contrast[2];
}

static void
shell_zoom_region_dispose (GObject *object)
{
  ShellZoomRegion *self = SHELL_ZOOM_REGION (object);

  clear_scroll_contents_timer (self);

  if (self->focus_caret_tracker != NULL)
    {
      shell_zoom_region_set_active (self, FALSE);
      disconnect_signals (self);
    }

  g_clear_object (&self->focus_caret_tracker);
  g_clear_object (&self->cross_hairs);
  g_clear_object (&self->mouse_source_actor);
  g_clear_weak_pointer (&self->magnifier);

  G_OBJECT_CLASS (shell_zoom_region_parent_class)->dispose (object);
}

static void
shell_zoom_region_class_init (ShellZoomRegionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_zoom_region_dispose;
}

static void
shell_zoom_region_init (ShellZoomRegion *self)
{
  self->focus_caret_tracker = shell_focus_caret_tracker_new ();

  self->mouse_tracking_mode = G_DESKTOP_MAGNIFIER_MOUSE_TRACKING_MODE_NONE;
  self->focus_tracking_mode = G_DESKTOP_MAGNIFIER_FOCUS_TRACKING_MODE_NONE;
  self->caret_tracking_mode = G_DESKTOP_MAGNIFIER_CARET_TRACKING_MODE_NONE;
  self->clamp_scrolling_at_edges = FALSE;
  self->lens_mode = FALSE;
  self->screen_position = G_DESKTOP_MAGNIFIER_SCREEN_POSITION_FULL_SCREEN;
  self->invert_lightness = FALSE;
  self->color_saturation = 1.0;
  self->brightness[0] = self->brightness[1] = self->brightness[2] = NO_CHANGE;
  self->contrast[0] = self->contrast[1] = self->contrast[2] = NO_CHANGE;

  self->x_mag_factor = 1;
  self->y_mag_factor = 1;
}

static ShellZoomRegion *
zoom_region_new (ShellMagnifier *magnifier,
                 ClutterActor   *mouse_source_actor)
{
  ShellZoomRegion *self = g_object_new (SHELL_TYPE_ZOOM_REGION, NULL);
  int screen_width, screen_height;

  g_set_weak_pointer (&self->magnifier, magnifier);
  self->mouse_source_actor = g_object_ref (mouse_source_actor);

  magnifier_get_screen_size (magnifier, &screen_width, &screen_height);
  self->view_port_x = 0;
  self->view_port_y = 0;
  self->view_port_width = screen_width;
  self->view_port_height = screen_height;
  self->x_center = self->view_port_width / 2;
  self->y_center = self->view_port_height / 2;

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellMagnifier                                                            */
/* ------------------------------------------------------------------------ */

static ShellZoomRegion *
first_zoom_region (ShellMagnifier *self)
{
  if (self->zoom_regions->len == 0)
    return NULL;

  return g_ptr_array_index (self->zoom_regions, 0);
}

static void
update_content_scale (ShellMagnifier *self)
{
  ShellMonitor *monitor = NULL;
  double scale = 1;

  if (self->layout_manager != NULL)
    {
      monitor = shell_layout_manager_find_monitor_for_point (self->layout_manager,
                                                             self->x_mouse,
                                                             self->y_mouse);
    }

  if (monitor != NULL)
    scale = monitor->geometry_scale;
  g_clear_pointer (&monitor, shell_monitor_free);

  mouse_sprite_content_set_monitor_scale (self->mouse_sprite_content, scale);
}

/**
 * shell_magnifier_show_system_cursor:
 * @self: a #ShellMagnifier
 *
 * Shows the system mouse pointer.
 */
void
shell_magnifier_show_system_cursor (ShellMagnifier *self)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  if (self->cursor_unfocus_inhibited)
    {
      if (self->seat != NULL)
        clutter_seat_uninhibit_unfocus (self->seat);
      self->cursor_unfocus_inhibited = FALSE;
    }

  if (self->cursor_visible_inhibited)
    {
      if (self->cursor_tracker != NULL)
        meta_cursor_tracker_uninhibit_cursor_visibility (self->cursor_tracker);
      self->cursor_visible_inhibited = FALSE;
    }
}

/**
 * shell_magnifier_hide_system_cursor:
 * @self: a #ShellMagnifier
 *
 * Hides the system mouse pointer.
 */
void
shell_magnifier_hide_system_cursor (ShellMagnifier *self)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  if (!self->cursor_unfocus_inhibited)
    {
      if (self->seat != NULL)
        clutter_seat_inhibit_unfocus (self->seat);
      self->cursor_unfocus_inhibited = TRUE;
    }

  if (!self->cursor_visible_inhibited)
    {
      if (self->cursor_tracker != NULL)
        meta_cursor_tracker_inhibit_cursor_visibility (self->cursor_tracker);
      self->cursor_visible_inhibited = TRUE;
    }
}

static void
update_sprite_texture (ShellMagnifier *self)
{
  CoglTexture *sprite = NULL;

  if (self->cursor_tracker != NULL)
    sprite = meta_cursor_tracker_get_sprite (self->cursor_tracker);

  if (sprite != NULL)
    {
      mouse_sprite_content_set_texture (self->mouse_sprite_content, sprite);
      clutter_actor_show (self->mouse_sprite);
    }
  else
    {
      clutter_actor_hide (self->mouse_sprite);
    }
}

static void
update_mouse_sprite (ShellMagnifier *self)
{
  int x_hot = 0, y_hot = 0;

  update_sprite_texture (self);
  if (self->cursor_tracker != NULL)
    meta_cursor_tracker_get_hot (self->cursor_tracker, &x_hot, &y_hot);

  clutter_actor_set_translation (self->mouse_sprite, -x_hot, -y_hot, 0);
}

static void
get_pointer_position (ShellMagnifier *self,
                      double         *x,
                      double         *y)
{
  graphene_point_t coords = GRAPHENE_POINT_INIT (0, 0);

  if (self->cursor_tracker != NULL)
    meta_cursor_tracker_get_pointer (self->cursor_tracker, &coords, NULL);

  *x = coords.x;
  *y = coords.y;
}

static void
scroll_to_mouse_pos_internal (ShellMagnifier *self,
                              gboolean        has_position,
                              double          x_mouse,
                              double          y_mouse)
{
  gboolean sys_mouse_over_any = FALSE;
  guint i;

  if (!has_position)
    get_pointer_position (self, &x_mouse, &y_mouse);

  if (x_mouse == self->x_mouse && y_mouse == self->y_mouse)
    return;

  self->x_mouse = x_mouse;
  self->y_mouse = y_mouse;

  update_content_scale (self);

  for (i = 0; i < self->zoom_regions->len; i++)
    {
      ShellZoomRegion *zoom_region = g_ptr_array_index (self->zoom_regions, i);

      if (shell_zoom_region_scroll_to_mouse_pos (zoom_region))
        sys_mouse_over_any = TRUE;
    }

  if (sys_mouse_over_any)
    shell_magnifier_hide_system_cursor (self);
  else
    shell_magnifier_show_system_cursor (self);
}

/**
 * shell_magnifier_scroll_to_mouse_pos:
 * @self: a #ShellMagnifier
 *
 * Positions the region of interest of all zoom regions relative to the
 * current location of the system pointer.
 */
void
shell_magnifier_scroll_to_mouse_pos (ShellMagnifier *self)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  scroll_to_mouse_pos_internal (self, FALSE, 0, 0);
}

/**
 * shell_magnifier_scroll_to_point:
 * @self: a #ShellMagnifier
 * @x_mouse: the x coordinate of the point
 * @y_mouse: the y coordinate of the point
 *
 * Like shell_magnifier_scroll_to_mouse_pos (), for a mouse at the given
 * position instead of the system pointer.
 */
void
shell_magnifier_scroll_to_point (ShellMagnifier *self,
                                 double          x_mouse,
                                 double          y_mouse)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  scroll_to_mouse_pos_internal (self, TRUE, x_mouse, y_mouse);
}

static gboolean
pointer_position_later_cb (gpointer user_data)
{
  ShellMagnifier *self = SHELL_MAGNIFIER (user_data);

  self->pointer_position_later_id = 0;
  scroll_to_mouse_pos_internal (self, FALSE, 0, 0);
  return G_SOURCE_REMOVE;
}

static void
queue_pointer_position_update (ShellMagnifier *self)
{
  MetaLaters *laters;

  if (self->pointer_position_later_id != 0 || self->compositor == NULL)
    return;

  laters = meta_compositor_get_laters (self->compositor);
  self->pointer_position_later_id =
    meta_laters_add (laters, META_LATER_BEFORE_REDRAW,
                     pointer_position_later_cb, self, NULL);
}

static void
clear_pointer_position_later (ShellMagnifier *self)
{
  if (self->pointer_position_later_id == 0)
    return;

  if (self->compositor != NULL)
    {
      MetaLaters *laters = meta_compositor_get_laters (self->compositor);

      meta_laters_remove (laters, self->pointer_position_later_id);
    }
  self->pointer_position_later_id = 0;
}

static void
on_position_invalidated (MetaCursorTracker *tracker,
                         ShellMagnifier    *self)
{
  queue_pointer_position_update (self);
}

/**
 * shell_magnifier_start_tracking_mouse:
 * @self: a #ShellMagnifier
 *
 * Turns on mouse tracking, if not already doing so.
 */
void
shell_magnifier_start_tracking_mouse (ShellMagnifier *self)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  if (self->pointer_position_id != 0 || self->cursor_tracker == NULL)
    return;

  self->pointer_position_id =
    g_signal_connect_object (self->cursor_tracker, "position-invalidated",
                             G_CALLBACK (on_position_invalidated), self, 0);
  shell_magnifier_scroll_to_mouse_pos (self);
}

/**
 * shell_magnifier_stop_tracking_mouse:
 * @self: a #ShellMagnifier
 *
 * Turns off mouse tracking, if not already doing so.
 */
void
shell_magnifier_stop_tracking_mouse (ShellMagnifier *self)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  if (self->pointer_position_id == 0)
    return;

  if (self->cursor_tracker != NULL)
    g_clear_signal_handler (&self->pointer_position_id, self->cursor_tracker);
  self->pointer_position_id = 0;

  clear_pointer_position_later (self);
}

/**
 * shell_magnifier_is_tracking_mouse:
 * @self: a #ShellMagnifier
 *
 * Returns: whether the magnifier is currently tracking the mouse
 */
gboolean
shell_magnifier_is_tracking_mouse (ShellMagnifier *self)
{
  g_return_val_if_fail (SHELL_IS_MAGNIFIER (self), FALSE);

  return self->pointer_position_id != 0;
}

/**
 * shell_magnifier_get_mouse_position:
 * @self: a #ShellMagnifier
 * @x_mouse: (out) (optional): return location for the x coordinate of the
 *   mouse the zoom regions are scrolled to
 * @y_mouse: (out) (optional): return location for the y coordinate
 */
void
shell_magnifier_get_mouse_position (ShellMagnifier *self,
                                    double         *x_mouse,
                                    double         *y_mouse)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  if (x_mouse != NULL)
    *x_mouse = self->x_mouse;
  if (y_mouse != NULL)
    *y_mouse = self->y_mouse;
}

/**
 * shell_magnifier_is_active:
 * @self: a #ShellMagnifier
 *
 * Returns: whether the magnifier is active
 */
gboolean
shell_magnifier_is_active (ShellMagnifier *self)
{
  g_return_val_if_fail (SHELL_IS_MAGNIFIER (self), FALSE);

  /* Sufficient to check one ZoomRegion since Magnifier's active
   * state applies to all of them. */
  if (self->zoom_regions->len == 0)
    return FALSE;

  return shell_zoom_region_is_active (first_zoom_region (self));
}

static void
on_cursor_changed (MetaCursorTracker *tracker,
                   ShellMagnifier    *self)
{
  update_mouse_sprite (self);
}

/**
 * shell_magnifier_set_active:
 * @self: a #ShellMagnifier
 * @activate: whether to activate or de-activate the magnifier
 *
 * Shows or hides all the zoom regions.
 */
void
shell_magnifier_set_active (ShellMagnifier *self,
                            gboolean        activate)
{
  gboolean was_active;
  guint i;

  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  activate = !!activate;
  was_active = shell_magnifier_is_active (self);

  for (i = 0; i < self->zoom_regions->len; i++)
    shell_zoom_region_set_active (g_ptr_array_index (self->zoom_regions, i),
                                  activate);

  if (was_active == activate)
    return;

  if (activate)
    {
      update_mouse_sprite (self);
      if (self->cursor_tracker != NULL && self->cursor_changed_id == 0)
        {
          self->cursor_changed_id =
            g_signal_connect_object (self->cursor_tracker, "cursor-changed",
                                     G_CALLBACK (on_cursor_changed), self, 0);
        }
      if (self->compositor != NULL)
        meta_compositor_disable_unredirect (self->compositor);
      shell_magnifier_start_tracking_mouse (self);
    }
  else
    {
      if (self->cursor_tracker != NULL)
        g_clear_signal_handler (&self->cursor_changed_id, self->cursor_tracker);
      self->cursor_changed_id = 0;
      mouse_sprite_content_set_texture (self->mouse_sprite_content, NULL);
      if (self->compositor != NULL)
        meta_compositor_enable_unredirect (self->compositor);
      shell_magnifier_stop_tracking_mouse (self);
    }

  if (self->cross_hairs != NULL)
    shell_magnifier_crosshairs_set_enabled (self->cross_hairs, activate);

  /* Make sure system mouse pointer is shown when all zoom regions are
   * invisible. */
  if (!activate)
    shell_magnifier_show_system_cursor (self);

  /* Notify interested parties of this change */
  g_signal_emit (self, signals[ACTIVE_CHANGED], 0, activate);
}

/**
 * shell_magnifier_create_zoom_region:
 * @self: a #ShellMagnifier
 * @x_mag_factor: the power to set horizontal magnification of the zoom
 *   region, 1.0 means no magnification and 2.0 doubles the size
 * @y_mag_factor: the power to set the vertical magnification of the zoom
 *   region
 * @roi: the region to magnify, in unmagnified coordinates; its size is
 *   ignored, the one of the view port and the magnification factors is used
 * @view_port: the position of the zoom region on screen
 *
 * Creates a zoom region with the given properties. It is not part of the
 * magnifier until shell_magnifier_add_zoom_region () is called.
 *
 * Returns: (transfer full): the newly created #ShellZoomRegion
 */
ShellZoomRegion *
shell_magnifier_create_zoom_region (ShellMagnifier        *self,
                                    double                 x_mag_factor,
                                    double                 y_mag_factor,
                                    const graphene_rect_t *roi,
                                    const graphene_rect_t *view_port)
{
  ShellZoomRegion *zoom_region;
  graphene_rect_t fixed_roi;

  g_return_val_if_fail (SHELL_IS_MAGNIFIER (self), NULL);
  g_return_val_if_fail (roi != NULL, NULL);
  g_return_val_if_fail (view_port != NULL, NULL);

  zoom_region = zoom_region_new (self, self->cursor_root);
  shell_zoom_region_set_view_port (zoom_region, view_port);

  /* We ignore the redundant width/height on the ROI */
  graphene_rect_init (&fixed_roi,
                      graphene_rect_get_x (roi),
                      graphene_rect_get_y (roi),
                      graphene_rect_get_width (view_port) / x_mag_factor,
                      graphene_rect_get_height (view_port) / y_mag_factor);
  shell_zoom_region_set_roi (zoom_region, &fixed_roi);

  shell_zoom_region_add_crosshairs (zoom_region, self->cross_hairs);
  return zoom_region;
}

/**
 * shell_magnifier_add_zoom_region:
 * @self: a #ShellMagnifier
 * @zoom_region: the zoom region to add
 *
 * Appends the given zoom region to the list of currently defined zoom regions
 * for this magnifier, and starts tracking the mouse.
 */
void
shell_magnifier_add_zoom_region (ShellMagnifier  *self,
                                 ShellZoomRegion *zoom_region)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER (self));
  g_return_if_fail (zoom_region == NULL || SHELL_IS_ZOOM_REGION (zoom_region));

  if (zoom_region == NULL)
    return;

  g_ptr_array_add (self->zoom_regions, g_object_ref (zoom_region));
  if (!shell_magnifier_is_tracking_mouse (self))
    shell_magnifier_start_tracking_mouse (self);
}

/**
 * shell_magnifier_get_zoom_regions:
 * @self: a #ShellMagnifier
 *
 * Returns: (transfer container) (element-type ShellZoomRegion): the zoom
 *   regions of the magnifier
 */
GList *
shell_magnifier_get_zoom_regions (ShellMagnifier *self)
{
  GList *regions = NULL;
  guint i;

  g_return_val_if_fail (SHELL_IS_MAGNIFIER (self), NULL);

  for (i = self->zoom_regions->len; i > 0; i--)
    regions = g_list_prepend (regions, g_ptr_array_index (self->zoom_regions, i - 1));

  return regions;
}

/**
 * shell_magnifier_clear_all_zoom_regions:
 * @self: a #ShellMagnifier
 *
 * Removes all the zoom regions from the list of this magnifier.
 */
void
shell_magnifier_clear_all_zoom_regions (ShellMagnifier *self)
{
  guint i;

  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  for (i = 0; i < self->zoom_regions->len; i++)
    shell_zoom_region_set_active (g_ptr_array_index (self->zoom_regions, i), FALSE);

  g_ptr_array_set_size (self->zoom_regions, 0);
  shell_magnifier_stop_tracking_mouse (self);
  shell_magnifier_show_system_cursor (self);
}

/**
 * shell_magnifier_add_crosshairs:
 * @self: a #ShellMagnifier
 *
 * Adds and shows a crosshair centered on the magnified mouse, with the
 * settings of `org.gnome.desktop.a11y.magnifier`.
 */
void
shell_magnifier_add_crosshairs (ShellMagnifier *self)
{
  g_autofree char *color = NULL;
  guint i;

  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  if (self->cross_hairs == NULL)
    self->cross_hairs = shell_magnifier_crosshairs_new ();

  color = g_settings_get_string (self->settings, CROSS_HAIRS_COLOR_KEY);

  shell_magnifier_set_crosshairs_thickness (self,
    g_settings_get_int (self->settings, CROSS_HAIRS_THICKNESS_KEY));
  shell_magnifier_set_crosshairs_color (self, color);
  shell_magnifier_set_crosshairs_opacity (self,
    g_settings_get_double (self->settings, CROSS_HAIRS_OPACITY_KEY));
  shell_magnifier_set_crosshairs_length (self,
    g_settings_get_int (self->settings, CROSS_HAIRS_LENGTH_KEY));
  shell_magnifier_set_crosshairs_clip (self,
    g_settings_get_boolean (self->settings, CROSS_HAIRS_CLIP_KEY));

  for (i = 0; i < self->zoom_regions->len; i++)
    shell_zoom_region_add_crosshairs (g_ptr_array_index (self->zoom_regions, i),
                                      self->cross_hairs);
}

/**
 * shell_magnifier_set_crosshairs_visible:
 * @self: a #ShellMagnifier
 * @visible: whether to show or hide the crosshairs
 */
void
shell_magnifier_set_crosshairs_visible (ShellMagnifier *self,
                                        gboolean        visible)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  if (visible)
    {
      if (self->cross_hairs == NULL)
        shell_magnifier_add_crosshairs (self);
      clutter_actor_show (CLUTTER_ACTOR (self->cross_hairs));
    }
  else if (self->cross_hairs != NULL)
    {
      clutter_actor_hide (CLUTTER_ACTOR (self->cross_hairs));
    }
}

/**
 * shell_magnifier_set_crosshairs_color:
 * @self: a #ShellMagnifier
 * @color: the color as a string, e.g. `#ff0000ff` or `red`
 *
 * Sets the color of the crosshairs for all zoom regions.
 */
void
shell_magnifier_set_crosshairs_color (ShellMagnifier *self,
                                      const char     *color)
{
  CoglColor cogl_color = { 0, };

  g_return_if_fail (SHELL_IS_MAGNIFIER (self));
  g_return_if_fail (color != NULL);

  if (self->cross_hairs == NULL)
    return;

  /* An invalid color string leaves the color transparent black, as the JS
   * ignored the result of the parsing */
  if (!cogl_color_from_string (&cogl_color, color))
    memset (&cogl_color, 0, sizeof (cogl_color));

  shell_magnifier_crosshairs_set_color (self->cross_hairs, &cogl_color);
}

/**
 * shell_magnifier_set_crosshairs_thickness:
 * @self: a #ShellMagnifier
 * @thickness: the width of the vertical and horizontal lines of the
 *   crosshairs
 */
void
shell_magnifier_set_crosshairs_thickness (ShellMagnifier *self,
                                          double          thickness)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  if (self->cross_hairs != NULL)
    shell_magnifier_crosshairs_set_thickness (self->cross_hairs, thickness);
}

/**
 * shell_magnifier_get_crosshairs_thickness:
 * @self: a #ShellMagnifier
 *
 * Returns: the width of the vertical and horizontal lines of the crosshairs,
 *   0 if there are none
 */
double
shell_magnifier_get_crosshairs_thickness (ShellMagnifier *self)
{
  g_return_val_if_fail (SHELL_IS_MAGNIFIER (self), 0);

  if (self->cross_hairs == NULL)
    return 0;

  return shell_magnifier_crosshairs_get_thickness (self->cross_hairs);
}

/**
 * shell_magnifier_set_crosshairs_opacity:
 * @self: a #ShellMagnifier
 * @opacity: a value between 0.0 (transparent) and 1.0 (fully opaque)
 */
void
shell_magnifier_set_crosshairs_opacity (ShellMagnifier *self,
                                        double          opacity)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  if (self->cross_hairs != NULL)
    shell_magnifier_crosshairs_set_hair_opacity (self->cross_hairs, opacity * 255);
}

/**
 * shell_magnifier_get_crosshairs_opacity:
 * @self: a #ShellMagnifier
 *
 * Returns: a value between 0.0 (transparent) and 1.0 (fully opaque)
 */
double
shell_magnifier_get_crosshairs_opacity (ShellMagnifier *self)
{
  g_return_val_if_fail (SHELL_IS_MAGNIFIER (self), 0);

  if (self->cross_hairs == NULL)
    return 0.0;

  return shell_magnifier_crosshairs_get_hair_opacity (self->cross_hairs) / 255.0;
}

/**
 * shell_magnifier_set_crosshairs_length:
 * @self: a #ShellMagnifier
 * @length: the length of the vertical and horizontal lines making up the
 *   crosshairs, in physical pixels
 */
void
shell_magnifier_set_crosshairs_length (ShellMagnifier *self,
                                       double          length)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  if (self->cross_hairs != NULL)
    {
      shell_magnifier_crosshairs_set_length (self->cross_hairs,
                                             length / magnifier_get_scale_factor (self));
    }
}

/**
 * shell_magnifier_get_crosshairs_length:
 * @self: a #ShellMagnifier
 *
 * Returns: the length of the vertical and horizontal lines making up the
 *   crosshairs, 0 if there are none
 */
double
shell_magnifier_get_crosshairs_length (ShellMagnifier *self)
{
  g_return_val_if_fail (SHELL_IS_MAGNIFIER (self), 0);

  if (self->cross_hairs == NULL)
    return 0;

  return shell_magnifier_crosshairs_get_length (self->cross_hairs);
}

/**
 * shell_magnifier_set_crosshairs_clip:
 * @self: a #ShellMagnifier
 * @clip: whether the crosshairs are clipped at their intersection
 */
void
shell_magnifier_set_crosshairs_clip (ShellMagnifier *self,
                                     gboolean        clip)
{
  g_return_if_fail (SHELL_IS_MAGNIFIER (self));

  if (self->cross_hairs == NULL)
    return;

  /* Setting no clipping on crosshairs means a zero sized clip rectangle. */
  if (clip)
    {
      shell_magnifier_crosshairs_set_clip_size (self->cross_hairs,
                                                CROSSHAIRS_CLIP_WIDTH,
                                                CROSSHAIRS_CLIP_HEIGHT);
    }
  else
    {
      shell_magnifier_crosshairs_set_clip_size (self->cross_hairs, 0, 0);
    }
}

/**
 * shell_magnifier_get_crosshairs_clip:
 * @self: a #ShellMagnifier
 *
 * Returns: whether the crosshairs are clipped by the mouse image
 */
gboolean
shell_magnifier_get_crosshairs_clip (ShellMagnifier *self)
{
  double clip_width, clip_height;

  g_return_val_if_fail (SHELL_IS_MAGNIFIER (self), FALSE);

  if (self->cross_hairs == NULL)
    return FALSE;

  shell_magnifier_crosshairs_get_clip_size (self->cross_hairs,
                                            &clip_width, &clip_height);
  return clip_width > 0 && clip_height > 0;
}

/* ---- Settings ---- */

static void
update_lens_mode (ShellMagnifier *self)
{
  /* Applies only to the first zoom region. */
  ShellZoomRegion *zoom_region = first_zoom_region (self);

  if (zoom_region != NULL)
    {
      shell_zoom_region_set_lens_mode (zoom_region,
                                       g_settings_get_boolean (self->settings,
                                                               LENS_MODE_KEY));
    }
}

/* Mag factor is accurate to two decimal places. */
static double
get_mag_factor_setting (ShellMagnifier *self)
{
  return floor (g_settings_get_double (self->settings, MAG_FACTOR_KEY) * 100 + 0.5) / 100;
}

static void
update_screen_position_cb (GSettings      *settings,
                           const char     *key,
                           ShellMagnifier *self)
{
  /* Applies only to the first zoom region. */
  ShellZoomRegion *zoom_region = first_zoom_region (self);

  if (zoom_region != NULL)
    {
      int position = g_settings_get_enum (self->settings, SCREEN_POSITION_KEY);

      shell_zoom_region_set_screen_position (zoom_region, position);
      if (position != G_DESKTOP_MAGNIFIER_SCREEN_POSITION_FULL_SCREEN)
        update_lens_mode (self);
    }
}

static void
update_mag_factor_cb (GSettings      *settings,
                      const char     *key,
                      ShellMagnifier *self)
{
  ShellZoomRegion *zoom_region = first_zoom_region (self);

  if (zoom_region != NULL)
    {
      double mag_factor = get_mag_factor_setting (self);

      shell_zoom_region_set_mag_factor (zoom_region, mag_factor, mag_factor);
    }
}

static void
update_lens_mode_cb (GSettings      *settings,
                     const char     *key,
                     ShellMagnifier *self)
{
  update_lens_mode (self);
}

static void
update_clamp_mode_cb (GSettings      *settings,
                      const char     *key,
                      ShellMagnifier *self)
{
  ShellZoomRegion *zoom_region = first_zoom_region (self);

  if (zoom_region != NULL)
    {
      shell_zoom_region_set_clamp_scrolling_at_edges (zoom_region,
        !g_settings_get_boolean (self->settings, CLAMP_MODE_KEY));
    }
}

static void
update_mouse_tracking_mode_cb (GSettings      *settings,
                               const char     *key,
                               ShellMagnifier *self)
{
  ShellZoomRegion *zoom_region = first_zoom_region (self);

  if (zoom_region != NULL)
    {
      shell_zoom_region_set_mouse_tracking_mode (zoom_region,
        g_settings_get_enum (self->settings, MOUSE_TRACKING_KEY));
    }
}

static void
update_focus_tracking_mode_cb (GSettings      *settings,
                               const char     *key,
                               ShellMagnifier *self)
{
  ShellZoomRegion *zoom_region = first_zoom_region (self);

  if (zoom_region != NULL)
    {
      shell_zoom_region_set_focus_tracking_mode (zoom_region,
        g_settings_get_enum (self->settings, FOCUS_TRACKING_KEY));
    }
}

static void
update_caret_tracking_mode_cb (GSettings      *settings,
                               const char     *key,
                               ShellMagnifier *self)
{
  ShellZoomRegion *zoom_region = first_zoom_region (self);

  if (zoom_region != NULL)
    {
      shell_zoom_region_set_caret_tracking_mode (zoom_region,
        g_settings_get_enum (self->settings, CARET_TRACKING_KEY));
    }
}

static void
update_invert_lightness_cb (GSettings      *settings,
                            const char     *key,
                            ShellMagnifier *self)
{
  ShellZoomRegion *zoom_region = first_zoom_region (self);

  if (zoom_region != NULL)
    {
      shell_zoom_region_set_invert_lightness (zoom_region,
        g_settings_get_boolean (self->settings, INVERT_LIGHTNESS_KEY));
    }
}

static void
update_color_saturation_cb (GSettings      *settings,
                            const char     *key,
                            ShellMagnifier *self)
{
  ShellZoomRegion *zoom_region = first_zoom_region (self);

  if (zoom_region != NULL)
    {
      shell_zoom_region_set_color_saturation (zoom_region,
        g_settings_get_double (self->settings, COLOR_SATURATION_KEY));
    }
}

static void
update_brightness_cb (GSettings      *settings,
                      const char     *key,
                      ShellMagnifier *self)
{
  ShellZoomRegion *zoom_region = first_zoom_region (self);

  if (zoom_region != NULL)
    {
      shell_zoom_region_set_brightness (zoom_region,
        g_settings_get_double (self->settings, BRIGHT_RED_KEY),
        g_settings_get_double (self->settings, BRIGHT_GREEN_KEY),
        g_settings_get_double (self->settings, BRIGHT_BLUE_KEY));
    }
}

static void
update_contrast_cb (GSettings      *settings,
                    const char     *key,
                    ShellMagnifier *self)
{
  ShellZoomRegion *zoom_region = first_zoom_region (self);

  if (zoom_region != NULL)
    {
      shell_zoom_region_set_contrast (zoom_region,
        g_settings_get_double (self->settings, CONTRAST_RED_KEY),
        g_settings_get_double (self->settings, CONTRAST_GREEN_KEY),
        g_settings_get_double (self->settings, CONTRAST_BLUE_KEY));
    }
}

static void
show_cross_hairs_changed_cb (GSettings      *settings,
                             const char     *key,
                             ShellMagnifier *self)
{
  shell_magnifier_set_crosshairs_visible (self,
    g_settings_get_boolean (self->settings, SHOW_CROSS_HAIRS_KEY));
}

static void
cross_hairs_thickness_changed_cb (GSettings      *settings,
                                  const char     *key,
                                  ShellMagnifier *self)
{
  shell_magnifier_set_crosshairs_thickness (self,
    g_settings_get_int (self->settings, CROSS_HAIRS_THICKNESS_KEY));
}

static void
cross_hairs_color_changed_cb (GSettings      *settings,
                              const char     *key,
                              ShellMagnifier *self)
{
  g_autofree char *color =
    g_settings_get_string (self->settings, CROSS_HAIRS_COLOR_KEY);

  shell_magnifier_set_crosshairs_color (self, color);
}

static void
cross_hairs_opacity_changed_cb (GSettings      *settings,
                                const char     *key,
                                ShellMagnifier *self)
{
  shell_magnifier_set_crosshairs_opacity (self,
    g_settings_get_double (self->settings, CROSS_HAIRS_OPACITY_KEY));
}

static void
cross_hairs_length_changed_cb (GSettings      *settings,
                               const char     *key,
                               ShellMagnifier *self)
{
  shell_magnifier_set_crosshairs_length (self,
    g_settings_get_int (self->settings, CROSS_HAIRS_LENGTH_KEY));
}

static void
cross_hairs_clip_changed_cb (GSettings      *settings,
                             const char     *key,
                             ShellMagnifier *self)
{
  shell_magnifier_set_crosshairs_clip (self,
    g_settings_get_boolean (self->settings, CROSS_HAIRS_CLIP_KEY));
}

static void
connect_setting (ShellMagnifier *self,
                 const char     *key,
                 GCallback       callback)
{
  g_autofree char *detailed_signal = g_strdup_printf ("changed::%s", key);

  g_signal_connect_object (self->settings, detailed_signal, callback, self, 0);
}

static void
settings_init (ShellMagnifier *self,
               ShellZoomRegion *zoom_region)
{
  gboolean show_crosshairs;

  self->settings = g_settings_new (MAGNIFIER_SCHEMA);

  connect_setting (self, SCREEN_POSITION_KEY, G_CALLBACK (update_screen_position_cb));
  connect_setting (self, MAG_FACTOR_KEY, G_CALLBACK (update_mag_factor_cb));
  connect_setting (self, LENS_MODE_KEY, G_CALLBACK (update_lens_mode_cb));
  connect_setting (self, CLAMP_MODE_KEY, G_CALLBACK (update_clamp_mode_cb));
  connect_setting (self, MOUSE_TRACKING_KEY, G_CALLBACK (update_mouse_tracking_mode_cb));
  connect_setting (self, FOCUS_TRACKING_KEY, G_CALLBACK (update_focus_tracking_mode_cb));
  connect_setting (self, CARET_TRACKING_KEY, G_CALLBACK (update_caret_tracking_mode_cb));

  connect_setting (self, INVERT_LIGHTNESS_KEY, G_CALLBACK (update_invert_lightness_cb));
  connect_setting (self, COLOR_SATURATION_KEY, G_CALLBACK (update_color_saturation_cb));

  connect_setting (self, BRIGHT_RED_KEY, G_CALLBACK (update_brightness_cb));
  connect_setting (self, BRIGHT_GREEN_KEY, G_CALLBACK (update_brightness_cb));
  connect_setting (self, BRIGHT_BLUE_KEY, G_CALLBACK (update_brightness_cb));

  connect_setting (self, CONTRAST_RED_KEY, G_CALLBACK (update_contrast_cb));
  connect_setting (self, CONTRAST_GREEN_KEY, G_CALLBACK (update_contrast_cb));
  connect_setting (self, CONTRAST_BLUE_KEY, G_CALLBACK (update_contrast_cb));

  connect_setting (self, SHOW_CROSS_HAIRS_KEY, G_CALLBACK (show_cross_hairs_changed_cb));
  connect_setting (self, CROSS_HAIRS_THICKNESS_KEY, G_CALLBACK (cross_hairs_thickness_changed_cb));
  connect_setting (self, CROSS_HAIRS_COLOR_KEY, G_CALLBACK (cross_hairs_color_changed_cb));
  connect_setting (self, CROSS_HAIRS_OPACITY_KEY, G_CALLBACK (cross_hairs_opacity_changed_cb));
  connect_setting (self, CROSS_HAIRS_LENGTH_KEY, G_CALLBACK (cross_hairs_length_changed_cb));
  connect_setting (self, CROSS_HAIRS_CLIP_KEY, G_CALLBACK (cross_hairs_clip_changed_cb));

  if (zoom_region != NULL)
    {
      double mag_factor;
      int pref;

      mag_factor = get_mag_factor_setting (self);
      if (mag_factor != 0.0)
        shell_zoom_region_set_mag_factor (zoom_region, mag_factor, mag_factor);

      pref = g_settings_get_enum (self->settings, SCREEN_POSITION_KEY);
      if (pref)
        shell_zoom_region_set_screen_position (zoom_region, pref);

      shell_zoom_region_set_lens_mode (zoom_region,
        g_settings_get_boolean (self->settings, LENS_MODE_KEY));
      shell_zoom_region_set_clamp_scrolling_at_edges (zoom_region,
        !g_settings_get_boolean (self->settings, CLAMP_MODE_KEY));

      pref = g_settings_get_enum (self->settings, MOUSE_TRACKING_KEY);
      if (pref)
        shell_zoom_region_set_mouse_tracking_mode (zoom_region, pref);

      pref = g_settings_get_enum (self->settings, FOCUS_TRACKING_KEY);
      if (pref)
        shell_zoom_region_set_focus_tracking_mode (zoom_region, pref);

      pref = g_settings_get_enum (self->settings, CARET_TRACKING_KEY);
      if (pref)
        shell_zoom_region_set_caret_tracking_mode (zoom_region, pref);

      if (g_settings_get_boolean (self->settings, INVERT_LIGHTNESS_KEY))
        shell_zoom_region_set_invert_lightness (zoom_region, TRUE);

      shell_zoom_region_set_color_saturation (zoom_region,
        g_settings_get_double (self->settings, COLOR_SATURATION_KEY));

      shell_zoom_region_set_brightness (zoom_region,
        g_settings_get_double (self->settings, BRIGHT_RED_KEY),
        g_settings_get_double (self->settings, BRIGHT_GREEN_KEY),
        g_settings_get_double (self->settings, BRIGHT_BLUE_KEY));

      shell_zoom_region_set_contrast (zoom_region,
        g_settings_get_double (self->settings, CONTRAST_RED_KEY),
        g_settings_get_double (self->settings, CONTRAST_GREEN_KEY),
        g_settings_get_double (self->settings, CONTRAST_BLUE_KEY));
    }

  show_crosshairs = g_settings_get_boolean (self->settings, SHOW_CROSS_HAIRS_KEY);
  shell_magnifier_add_crosshairs (self);
  shell_magnifier_set_crosshairs_visible (self, show_crosshairs);
}

/* ---- Object ---- */

static void
on_magnifier_active_changed (StSettings     *st_settings,
                             GParamSpec     *pspec,
                             ShellMagnifier *self)
{
  shell_magnifier_set_active (self, st_settings_get_magnifier_active (st_settings));
}

static void
shell_magnifier_constructed (GObject *object)
{
  ShellMagnifier *self = SHELL_MAGNIFIER (object);
  ShellGlobal *global = shell_global_get ();
  ShellLayoutManager *layout_manager;
  ShellZoomRegion *zoom_region;
  ClutterActor *mouse_sprite;
  ClutterContext *context;
  ClutterBackend *backend;
  StSettings *st_settings = st_settings_get ();

  G_OBJECT_CLASS (shell_magnifier_parent_class)->constructed (object);

  layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  if (layout_manager == NULL)
    {
      g_critical ("ShellMagnifier needs the layout manager of ShellMain");
      return;
    }

  g_set_weak_pointer (&self->layout_manager, layout_manager);
  g_set_weak_pointer (&self->ui_group,
                      CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager)));
  g_set_weak_pointer (&self->stage, shell_global_get_stage (global));
  g_set_weak_pointer (&self->display, shell_global_get_display (global));
  g_set_weak_pointer (&self->compositor, shell_global_get_compositor (global));

  /* Create small clutter tree for the magnified mouse. */
  g_set_weak_pointer (&self->cursor_tracker,
                      meta_backend_get_cursor_tracker (shell_global_get_backend (global)));

  mouse_sprite = clutter_actor_new ();
  clutter_actor_set_request_mode (mouse_sprite, CLUTTER_REQUEST_CONTENT_SIZE);
  self->mouse_sprite_content = g_object_new (SHELL_TYPE_MOUSE_SPRITE_CONTENT, NULL);
  clutter_actor_set_content (mouse_sprite,
                             CLUTTER_CONTENT (self->mouse_sprite_content));

  self->cursor_root = g_object_ref_sink (clutter_actor_new ());
  clutter_actor_add_child (self->cursor_root, mouse_sprite);
  g_set_weak_pointer (&self->mouse_sprite, mouse_sprite);

  context = clutter_actor_get_context (self->cursor_root);
  backend = clutter_context_get_backend (context);
  g_set_weak_pointer (&self->seat, clutter_backend_get_default_seat (backend));

  /* Create the first ZoomRegion and initialize it according to the
   * magnification settings. */
  get_pointer_position (self, &self->x_mouse, &self->y_mouse);

  zoom_region = zoom_region_new (self, self->cursor_root);
  g_ptr_array_add (self->zoom_regions, zoom_region);
  settings_init (self, zoom_region);
  shell_zoom_region_scroll_contents_to (zoom_region, self->x_mouse, self->y_mouse);

  update_content_scale (self);

  g_signal_connect_object (st_settings, "notify::magnifier-active",
                           G_CALLBACK (on_magnifier_active_changed), self, 0);

  shell_magnifier_set_active (self, st_settings_get_magnifier_active (st_settings));
}

static void
shell_magnifier_dispose (GObject *object)
{
  ShellMagnifier *self = SHELL_MAGNIFIER (object);

  if (self->zoom_regions != NULL)
    {
      guint i;

      for (i = 0; i < self->zoom_regions->len; i++)
        {
          ShellZoomRegion *zoom_region = g_ptr_array_index (self->zoom_regions, i);

          shell_zoom_region_set_active (zoom_region, FALSE);
        }
      g_ptr_array_set_size (self->zoom_regions, 0);
    }

  shell_magnifier_stop_tracking_mouse (self);
  if (self->cursor_tracker != NULL)
    g_clear_signal_handler (&self->cursor_changed_id, self->cursor_tracker);
  self->cursor_changed_id = 0;

  if (self->cross_hairs != NULL)
    {
      shell_magnifier_crosshairs_set_enabled (self->cross_hairs, FALSE);
      clutter_actor_destroy (CLUTTER_ACTOR (self->cross_hairs));
      g_clear_object (&self->cross_hairs);
    }

  if (self->cursor_root != NULL)
    {
      clutter_actor_destroy (self->cursor_root);
      g_clear_object (&self->cursor_root);
    }
  g_clear_object (&self->mouse_sprite_content);
  g_clear_object (&self->settings);

  g_clear_weak_pointer (&self->mouse_sprite);
  g_clear_weak_pointer (&self->cursor_tracker);
  g_clear_weak_pointer (&self->compositor);
  g_clear_weak_pointer (&self->display);
  g_clear_weak_pointer (&self->stage);
  g_clear_weak_pointer (&self->seat);
  g_clear_weak_pointer (&self->layout_manager);
  g_clear_weak_pointer (&self->ui_group);

  G_OBJECT_CLASS (shell_magnifier_parent_class)->dispose (object);
}

static void
shell_magnifier_finalize (GObject *object)
{
  ShellMagnifier *self = SHELL_MAGNIFIER (object);

  g_clear_pointer (&self->zoom_regions, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_magnifier_parent_class)->finalize (object);
}

static void
shell_magnifier_class_init (ShellMagnifierClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_magnifier_constructed;
  object_class->dispose = shell_magnifier_dispose;
  object_class->finalize = shell_magnifier_finalize;

  /**
   * ShellMagnifier::active-changed:
   * @self: the #ShellMagnifier
   * @active: whether the magnifier is now active
   *
   * Emitted when the magnifier is shown or hidden.
   */
  signals[ACTIVE_CHANGED] =
    g_signal_new ("active-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);
}

static void
shell_magnifier_init (ShellMagnifier *self)
{
  self->zoom_regions = g_ptr_array_new_with_free_func (g_object_unref);
}

/**
 * shell_magnifier_new:
 *
 * Creates the magnifier, with its first zoom region initialized from the
 * settings, and makes it follow #StSettings:magnifier-active. The layout
 * manager of #ShellMain has to be set.
 *
 * Returns: (transfer full): a new #ShellMagnifier
 */
ShellMagnifier *
shell_magnifier_new (void)
{
  return g_object_new (SHELL_TYPE_MAGNIFIER, NULL);
}
