/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * box-pointer.c: An actor which displays a triangle "arrow" pointing to a
 * given side, around a container. Port of js/ui/boxpointer.js.
 *
 * The .bin property is a container in which content can be placed. The arrow
 * position may be controlled via shell_box_pointer_set_arrow_origin(). The
 * arrow side might be temporarily flipped depending on the box size and
 * source position to keep the box totally inside the monitor workarea if
 * possible.
 */

#include "config.h"

#include <math.h>

#include <cairo.h>
#include <graphene.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-workspace-manager.h>
#include <meta/workspace.h>

/* The generated St enum types (ST_TYPE_SIDE) are not part of st.h */
#define ST_H_INSIDE 1
#include "st-enum-types.h"
#undef ST_H_INSIDE
#include "shell-global.h"
#include "ui/box-pointer.h"
#include "ui/environment.h"

enum
{
  PROP_0,

  PROP_BIN,
  PROP_ARROW_SIDE,
  PROP_ARROW_ALIGNMENT,
  PROP_ARROW_ACTOR,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  ARROW_SIDE_CHANGED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

typedef struct _ShellBoxPointerPrivate ShellBoxPointerPrivate;

struct _ShellBoxPointerPrivate
{
  StBin *bin;
  StDrawingArea *border;

  StSide arrow_side;
  StSide user_arrow_side;
  double arrow_origin;
  ClutterActor *arrow_actor;

  ClutterActor *source_actor;
  gulong source_destroy_id;
  double arrow_alignment;
  double source_alignment;

  /* computed by reposition(), used by calculate_arrow_side() */
  graphene_rect_t source_extents;
  MtkRectangle work_area;

  ClutterAction *mute_keys;
  ClutterAction *mute_input;

  gboolean unredirect_disabled;
};

G_DEFINE_TYPE_WITH_PRIVATE (ShellBoxPointer, shell_box_pointer, ST_TYPE_WIDGET)

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
 * Compositor unredirection: a mapped box pointer must not be unredirected
 */

static void
set_unredirect_disabled (ShellBoxPointer *self,
                         gboolean         disabled)
{
  ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);
  MetaDisplay *display;
  MetaCompositor *compositor;

  if (priv->unredirect_disabled == disabled)
    return;

  display = shell_global_get_display (shell_global_get ());
  if (display == NULL)
    return;

  compositor = meta_display_get_compositor (display);
  if (compositor == NULL)
    return;

  priv->unredirect_disabled = disabled;
  if (disabled)
    meta_compositor_disable_unredirect (compositor);
  else
    meta_compositor_enable_unredirect (compositor);
}

static void
shell_box_pointer_map (ClutterActor *actor)
{
  ShellBoxPointer *self = SHELL_BOX_POINTER (actor);

  CLUTTER_ACTOR_CLASS (shell_box_pointer_parent_class)->map (actor);

  if (clutter_actor_is_mapped (actor))
    set_unredirect_disabled (self, TRUE);
}

static void
shell_box_pointer_unmap (ClutterActor *actor)
{
  ShellBoxPointer *self = SHELL_BOX_POINTER (actor);

  CLUTTER_ACTOR_CLASS (shell_box_pointer_parent_class)->unmap (actor);

  set_unredirect_disabled (self, FALSE);
}

static void
shell_box_pointer_destroy (ClutterActor *actor)
{
  ShellBoxPointer *self = SHELL_BOX_POINTER (actor);

  /* ClutterActor stops doing property notifications in destroy, so
   * explicitly restore unredirect */
  set_unredirect_disabled (self, FALSE);

  CLUTTER_ACTOR_CLASS (shell_box_pointer_parent_class)->destroy (actor);
}

/*
 * Source actor tracking
 */

static void
source_actor_destroy_cb (ClutterActor    *source_actor,
                         ShellBoxPointer *self)
{
  ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);

  priv->source_actor = NULL;
  priv->source_destroy_id = 0;
}

static void
clear_source_actor (ShellBoxPointer *self)
{
  ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);

  if (priv->source_actor != NULL && priv->source_destroy_id != 0)
    g_signal_handler_disconnect (priv->source_actor, priv->source_destroy_id);

  priv->source_actor = NULL;
  priv->source_destroy_id = 0;
}

/* The stage allocates each child of a no-layout actor as a root of its own,
 * at its fixed position with its preferred size, the last queued first. The
 * window menu shows its source, such a child of the UI group, in the same
 * turn of the main loop as its box pointer, so the source comes after: never
 * allocated yet (an infinite box, which makes the position NaN) or still at
 * its previous place. It is allocated now as the stage will, as ClutterClone
 * does with its source; when the stage gets to it, nothing has changed. */
static void
ensure_source_allocated (ClutterActor *source_actor)
{
  ClutterActor *parent = clutter_actor_get_parent (source_actor);
  float x = 0.f, y = 0.f;

  if (parent == NULL || !clutter_actor_is_no_layout (parent) ||
      clutter_actor_has_allocation (source_actor))
    return;

  clutter_actor_get_fixed_position (source_actor, &x, &y);
  clutter_actor_allocate_preferred_size (source_actor, x, y);
}

/*
 * Monitor and work area, as Main.layoutManager.findIndexForActor() and
 * Main.layoutManager.getWorkAreaForMonitor() compute them
 */

static int
find_monitor_index_for_actor (MetaDisplay  *display,
                              ClutterActor *actor)
{
  MtkRectangle rect;
  float x, y, width, height;
  double left, top;
  int index;

  clutter_actor_get_transformed_position (actor, &x, &y);
  clutter_actor_get_transformed_size (actor, &width, &height);

  left = floor (x);
  top = floor (y);
  rect.x = (int) left;
  rect.y = (int) top;
  rect.width = (int) (ceil ((double) x + width) - left);
  rect.height = (int) (ceil ((double) y + height) - top);

  index = meta_display_get_monitor_index_for_rect (display, &rect);

  /* LayoutManager guarantees some monitor; the C API would otherwise
   * critical on a negative index. */
  if (index < 0)
    index = meta_display_get_primary_monitor (display);

  return index;
}

static void
get_work_area_for_monitor (MetaDisplay *display,
                           int          monitor_index,
                           MtkRectangle *work_area)
{
  MetaWorkspaceManager *workspace_manager =
    meta_display_get_workspace_manager (display);
  MetaWorkspace *workspace;

  /* A monitor can be gone by the time a work-area change is handled
   * (unplugged, or the last one removed at shutdown) */
  if (monitor_index < 0 || monitor_index >= meta_display_get_n_monitors (display))
    {
      *work_area = (MtkRectangle) { 0, };
      return;
    }

  /* Assume that all workspaces will have the same struts and pick the
   * first one. */
  workspace = meta_workspace_manager_get_workspace_by_index (workspace_manager, 0);
  if (workspace != NULL)
    meta_workspace_get_work_area_for_monitor (workspace, monitor_index, work_area);
  else
    meta_display_get_monitor_geometry (display, monitor_index, work_area);
}

/*
 * Geometry helpers
 */

static void
adjust_allocation_for_arrow (ShellBoxPointer *self,
                             gboolean         is_width,
                             float           *min_size,
                             float           *nat_size)
{
  ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  double border_width = st_theme_node_get_length (theme_node,
                                                  "-arrow-border-width");

  *min_size += border_width * 2;
  *nat_size += border_width * 2;

  if ((!is_width &&
       (priv->arrow_side == ST_SIDE_TOP || priv->arrow_side == ST_SIDE_BOTTOM)) ||
      (is_width &&
       (priv->arrow_side == ST_SIDE_LEFT || priv->arrow_side == ST_SIDE_RIGHT)))
    {
      double rise = st_theme_node_get_length (theme_node, "-arrow-rise");

      *min_size += rise;
      *nat_size += rise;
    }
}

static void
shell_box_pointer_get_preferred_width (ClutterActor *actor,
                                       float         for_height,
                                       float        *min_width_p,
                                       float        *natural_width_p)
{
  ShellBoxPointer *self = SHELL_BOX_POINTER (actor);
  ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  float min_width, natural_width;

  st_theme_node_adjust_for_height (theme_node, &for_height);

  clutter_actor_get_preferred_width (CLUTTER_ACTOR (priv->bin), for_height,
                                     &min_width, &natural_width);
  adjust_allocation_for_arrow (self, TRUE, &min_width, &natural_width);

  st_theme_node_adjust_preferred_width (theme_node, &min_width, &natural_width);

  if (min_width_p)
    *min_width_p = min_width;
  if (natural_width_p)
    *natural_width_p = natural_width;
}

static void
shell_box_pointer_get_preferred_height (ClutterActor *actor,
                                        float         for_width,
                                        float        *min_height_p,
                                        float        *natural_height_p)
{
  ShellBoxPointer *self = SHELL_BOX_POINTER (actor);
  ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  double border_width = st_theme_node_get_length (theme_node,
                                                  "-arrow-border-width");
  float min_height, natural_height;

  st_theme_node_adjust_for_width (theme_node, &for_width);

  clutter_actor_get_preferred_height (CLUTTER_ACTOR (priv->bin),
                                      for_width - 2 * border_width,
                                      &min_height, &natural_height);
  adjust_allocation_for_arrow (self, FALSE, &min_height, &natural_height);

  st_theme_node_adjust_preferred_height (theme_node, &min_height, &natural_height);

  if (min_height_p)
    *min_height_p = min_height;
  if (natural_height_p)
    *natural_height_p = natural_height;
}

/*
 * Positioning
 */

/* @origin: Coordinate specifying middle of the arrow, along the Y axis for
 * ST_SIDE_LEFT, ST_SIDE_RIGHT from the top and X axis from the left for
 * ST_SIDE_TOP and ST_SIDE_BOTTOM. */
static void
reposition (ShellBoxPointer *self,
            ClutterActorBox *allocation_box)
{
  ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);
  ClutterActor *source_actor = priv->source_actor;
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  double alignment = priv->arrow_alignment;
  ClutterActorBox source_allocation, source_content_box;
  graphene_point_t source_top_left, source_bottom_right;
  double source_center_x, source_center_y;
  float nat_width_f, nat_height_f;
  double nat_width, nat_height;
  double border_width, arrow_base, border_radius, margin, gap, padding;
  double res_x = 0, res_y = 0;
  double arrow_origin = 0;
  double half_base, half_border, half_margin;
  double x1, y1, x2, y2;
  MtkRectangle *work_area = &priv->work_area;
  ClutterActor *parent;
  gboolean success = FALSE;
  float x = 0, y = 0;
  int monitor_index;

  monitor_index = find_monitor_index_for_actor (display, source_actor);

  clutter_actor_get_transformed_extents (source_actor, &priv->source_extents);
  get_work_area_for_monitor (display, monitor_index, work_area);

  /* Position correctly relative to the sourceActor */
  clutter_actor_get_allocation_box (source_actor, &source_allocation);
  if (ST_IS_WIDGET (source_actor))
    {
      StThemeNode *source_node = st_widget_get_theme_node (ST_WIDGET (source_actor));

      st_theme_node_get_content_box (source_node, &source_allocation,
                                     &source_content_box);
    }
  else
    {
      source_content_box.x1 = 0;
      source_content_box.y1 = 0;
      source_content_box.x2 = clutter_actor_box_get_width (&source_allocation);
      source_content_box.y2 = clutter_actor_box_get_height (&source_allocation);
    }

  graphene_rect_get_top_left (&priv->source_extents, &source_top_left);
  graphene_rect_get_bottom_right (&priv->source_extents, &source_bottom_right);

  source_center_x = (double) source_top_left.x + source_content_box.x1 +
    ((double) source_content_box.x2 - source_content_box.x1) * priv->source_alignment;
  source_center_y = (double) source_top_left.y + source_content_box.y1 +
    ((double) source_content_box.y2 - source_content_box.y1) * priv->source_alignment;

  clutter_actor_get_preferred_size (CLUTTER_ACTOR (self), NULL, NULL,
                                    &nat_width_f, &nat_height_f);
  nat_width = nat_width_f;
  nat_height = nat_height_f;

  /* We also want to keep it onscreen, and separated from the edge by the
   * same distance as the main part of the box is separated from its
   * sourceActor */
  border_width = st_theme_node_get_length (theme_node, "-arrow-border-width");
  arrow_base = st_theme_node_get_length (theme_node, "-arrow-base");
  border_radius = st_theme_node_get_length (theme_node, "-arrow-border-radius");
  margin = 4 * border_radius + border_width + arrow_base;

  gap = st_theme_node_get_length (theme_node, "-boxpointer-gap");
  padding = st_theme_node_get_length (theme_node, "-arrow-rise");

  switch (priv->arrow_side)
    {
    case ST_SIDE_TOP:
      res_y = source_bottom_right.y + gap;
      break;
    case ST_SIDE_BOTTOM:
      res_y = source_top_left.y - nat_height - gap;
      break;
    case ST_SIDE_LEFT:
      res_x = source_bottom_right.x + gap;
      break;
    case ST_SIDE_RIGHT:
      res_x = source_top_left.x - nat_width - gap;
      break;
    default:
      g_assert_not_reached ();
    }

  /* Now align and position the pointing axis, making sure it fits on
   * screen. If the arrowOrigin is so close to the edge that the arrow
   * will not be isosceles, we try to compensate as follows:
   *   - We skip the rounded corner and settle for a right angled arrow
   *     as shown below. See draw_border() for further details.
   *     |\_____
   *     |
   *     |
   *   - If the arrow was going to be acute angled, we move the position
   *     of the box to maintain the arrow's accuracy.
   */

  half_base = floor (arrow_base / 2);
  half_border = border_width / 2;
  half_margin = margin / 2;
  x1 = half_border;
  y1 = half_border;
  x2 = nat_width - half_border;
  y2 = nat_height - half_border;

  switch (priv->arrow_side)
    {
    case ST_SIDE_TOP:
    case ST_SIDE_BOTTOM:
      if (clutter_actor_get_text_direction (CLUTTER_ACTOR (self)) ==
          CLUTTER_TEXT_DIRECTION_RTL)
        alignment = 1.0 - alignment;

      res_x = source_center_x - (half_margin + (nat_width - margin) * alignment);

      res_x = MAX (res_x, work_area->x + padding);
      res_x = MIN (res_x, work_area->x + work_area->width - (padding + nat_width));

      arrow_origin = source_center_x - res_x;
      if (arrow_origin <= (x1 + (border_radius + half_base)))
        {
          if (arrow_origin > x1)
            res_x += arrow_origin - x1;
          arrow_origin = x1;
        }
      else if (arrow_origin >= (x2 - (border_radius + half_base)))
        {
          if (arrow_origin < x2)
            res_x -= x2 - arrow_origin;
          arrow_origin = x2;
        }
      break;

    case ST_SIDE_LEFT:
    case ST_SIDE_RIGHT:
      res_y = source_center_y - (half_margin + (nat_height - margin) * alignment);

      res_y = MAX (res_y, work_area->y + padding);
      res_y = MIN (res_y, work_area->y + work_area->height - (padding + nat_height));

      arrow_origin = source_center_y - res_y;
      if (arrow_origin <= (y1 + (border_radius + half_base)))
        {
          if (arrow_origin > y1)
            res_y += arrow_origin - y1;
          arrow_origin = y1;
        }
      else if (arrow_origin >= (y2 - (border_radius + half_base)))
        {
          if (arrow_origin < y2)
            res_y -= y2 - arrow_origin;
          arrow_origin = y2;
        }
      break;

    default:
      g_assert_not_reached ();
    }

  shell_box_pointer_set_arrow_origin (self, arrow_origin);

  parent = clutter_actor_get_parent (CLUTTER_ACTOR (self));
  while (parent != NULL && !success)
    {
      success = clutter_actor_transform_stage_point (parent, res_x, res_y, &x, &y);
      parent = clutter_actor_get_parent (parent);
    }

  if (!success)
    return;

  /* Actually set the position */
  clutter_actor_box_set_origin (allocation_box, floorf (x), floorf (y));
}

static StSide
calculate_arrow_side (ShellBoxPointer *self,
                      StSide           arrow_side)
{
  ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);
  graphene_point_t source_top_left, source_bottom_right;
  float box_width_f, box_height_f;
  double box_width, box_height;
  const MtkRectangle *work_area = &priv->work_area;

  graphene_rect_get_top_left (&priv->source_extents, &source_top_left);
  graphene_rect_get_bottom_right (&priv->source_extents, &source_bottom_right);

  clutter_actor_get_preferred_size (CLUTTER_ACTOR (self), NULL, NULL,
                                    &box_width_f, &box_height_f);
  box_width = box_width_f;
  box_height = box_height_f;

  switch (arrow_side)
    {
    case ST_SIDE_TOP:
      if (source_bottom_right.y + box_height > work_area->y + work_area->height &&
          box_height < source_top_left.y - work_area->y)
        return ST_SIDE_BOTTOM;
      break;
    case ST_SIDE_BOTTOM:
      if (source_top_left.y - box_height < work_area->y &&
          box_height < work_area->y + work_area->height - source_bottom_right.y)
        return ST_SIDE_TOP;
      break;
    case ST_SIDE_LEFT:
      if (source_bottom_right.x + box_width > work_area->x + work_area->width &&
          box_width < source_top_left.x - work_area->x)
        return ST_SIDE_RIGHT;
      break;
    case ST_SIDE_RIGHT:
      if (source_top_left.x - box_width < work_area->x &&
          box_width < work_area->x + work_area->width - source_bottom_right.x)
        return ST_SIDE_LEFT;
      break;
    default:
      break;
    }

  return arrow_side;
}

static void
update_flip (ShellBoxPointer *self,
             ClutterActorBox *allocation_box)
{
  ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);
  StSide arrow_side = calculate_arrow_side (self, priv->user_arrow_side);

  if (priv->arrow_side != arrow_side)
    {
      priv->arrow_side = arrow_side;
      reposition (self, allocation_box);

      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ARROW_SIDE]);
      g_signal_emit (self, signals[ARROW_SIDE_CHANGED], 0);
    }
}

static void
shell_box_pointer_allocate (ClutterActor          *actor,
                            const ClutterActorBox *box)
{
  ShellBoxPointer *self = SHELL_BOX_POINTER (actor);
  ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);
  ClutterActorBox allocation = *box;
  ClutterActorBox content_box, child_box;
  StThemeNode *theme_node;
  double border_width, rise;
  float avail_width, avail_height;

  if (priv->source_actor != NULL && clutter_actor_is_mapped (priv->source_actor))
    {
      ensure_source_allocated (priv->source_actor);
      reposition (self, &allocation);
      update_flip (self, &allocation);
    }

  clutter_actor_set_allocation (actor, &allocation);

  theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  border_width = st_theme_node_get_length (theme_node, "-arrow-border-width");
  rise = st_theme_node_get_length (theme_node, "-arrow-rise");
  st_theme_node_get_content_box (theme_node, &allocation, &content_box);
  clutter_actor_box_get_size (&content_box, &avail_width, &avail_height);

  child_box.x1 = 0;
  child_box.y1 = 0;
  child_box.x2 = avail_width;
  child_box.y2 = avail_height;
  clutter_actor_allocate (CLUTTER_ACTOR (priv->border), &child_box);

  child_box.x1 = border_width;
  child_box.y1 = border_width;
  child_box.x2 = avail_width - border_width;
  child_box.y2 = avail_height - border_width;
  switch (priv->arrow_side)
    {
    case ST_SIDE_TOP:
      child_box.y1 += rise;
      break;
    case ST_SIDE_BOTTOM:
      child_box.y2 -= rise;
      break;
    case ST_SIDE_LEFT:
      child_box.x1 += rise;
      break;
    case ST_SIDE_RIGHT:
      child_box.x2 -= rise;
      break;
    default:
      g_assert_not_reached ();
    }
  clutter_actor_allocate (CLUTTER_ACTOR (priv->bin), &child_box);
}

/*
 * Drawing
 */

static void
draw_border (StDrawingArea  *area,
             ShellBoxPointer *self)
{
  ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  double border_width, base, rise, border_radius;
  double half_border, half_base;
  unsigned int width, height;
  double box_width, box_height;
  double x1, y1, x2, y2;
  gboolean skip_top_left = FALSE;
  gboolean skip_top_right = FALSE;
  gboolean skip_bottom_left = FALSE;
  gboolean skip_bottom_right = FALSE;
  gboolean has_color;
  CoglColor bg_color, border_color;
  cairo_t *cr;

  if (priv->arrow_actor != NULL)
    {
      float source_x, source_y, source_width, source_height;
      float abs_x, abs_y;

      clutter_actor_get_transformed_position (priv->arrow_actor,
                                              &source_x, &source_y);
      clutter_actor_get_transformed_size (priv->arrow_actor,
                                          &source_width, &source_height);
      clutter_actor_get_transformed_position (CLUTTER_ACTOR (self),
                                              &abs_x, &abs_y);

      if (priv->arrow_side == ST_SIDE_TOP ||
          priv->arrow_side == ST_SIDE_BOTTOM)
        priv->arrow_origin = (double) source_x - abs_x + source_width / 2.;
      else
        priv->arrow_origin = (double) source_y - abs_y + source_height / 2.;
    }

  border_width = st_theme_node_get_length (theme_node, "-arrow-border-width");
  base = st_theme_node_get_length (theme_node, "-arrow-base");
  rise = st_theme_node_get_length (theme_node, "-arrow-rise");
  border_radius = st_theme_node_get_length (theme_node, "-arrow-border-radius");

  half_border = border_width / 2;
  half_base = floor (base / 2);

  st_drawing_area_get_surface_size (area, &width, &height);
  box_width = width;
  box_height = height;
  if (priv->arrow_side == ST_SIDE_TOP || priv->arrow_side == ST_SIDE_BOTTOM)
    box_height -= rise;
  else
    box_width -= rise;

  cr = st_drawing_area_get_context (area);

  /* Translate so that box goes from 0,0 to boxWidth,boxHeight,
   * with the arrow poking out of that */
  if (priv->arrow_side == ST_SIDE_TOP)
    cairo_translate (cr, 0, rise);
  else if (priv->arrow_side == ST_SIDE_LEFT)
    cairo_translate (cr, rise, 0);

  x1 = half_border;
  y1 = half_border;
  x2 = box_width - half_border;
  y2 = box_height - half_border;

  if (rise != 0)
    {
      switch (priv->arrow_side)
        {
        case ST_SIDE_TOP:
          if (priv->arrow_origin == x1)
            skip_top_left = TRUE;
          else if (priv->arrow_origin == x2)
            skip_top_right = TRUE;
          break;

        case ST_SIDE_RIGHT:
          if (priv->arrow_origin == y1)
            skip_top_right = TRUE;
          else if (priv->arrow_origin == y2)
            skip_bottom_right = TRUE;
          break;

        case ST_SIDE_BOTTOM:
          if (priv->arrow_origin == x1)
            skip_bottom_left = TRUE;
          else if (priv->arrow_origin == x2)
            skip_bottom_right = TRUE;
          break;

        case ST_SIDE_LEFT:
          if (priv->arrow_origin == y1)
            skip_top_left = TRUE;
          else if (priv->arrow_origin == y2)
            skip_bottom_left = TRUE;
          break;

        default:
          g_assert_not_reached ();
        }
    }

  cairo_move_to (cr, x1 + border_radius, y1);
  if (priv->arrow_side == ST_SIDE_TOP && rise != 0)
    {
      if (skip_top_left)
        {
          cairo_move_to (cr, x1, y2 - border_radius);
          cairo_line_to (cr, x1, y1 - rise);
          cairo_line_to (cr, x1 + half_base, y1);
        }
      else if (skip_top_right)
        {
          cairo_line_to (cr, x2 - half_base, y1);
          cairo_line_to (cr, x2, y1 - rise);
          cairo_line_to (cr, x2, y1 + border_radius);
        }
      else
        {
          cairo_line_to (cr, priv->arrow_origin - half_base, y1);
          cairo_line_to (cr, priv->arrow_origin, y1 - rise);
          cairo_line_to (cr, priv->arrow_origin + half_base, y1);
        }
    }

  if (!skip_top_right)
    {
      cairo_line_to (cr, x2 - border_radius, y1);
      cairo_arc (cr, x2 - border_radius, y1 + border_radius, border_radius,
                 3 * G_PI / 2, G_PI * 2);
    }

  if (priv->arrow_side == ST_SIDE_RIGHT && rise != 0)
    {
      if (skip_top_right)
        {
          cairo_line_to (cr, x2 + rise, y1);
          cairo_line_to (cr, x2 + rise, y1 + half_base);
        }
      else if (skip_bottom_right)
        {
          cairo_line_to (cr, x2, y2 - half_base);
          cairo_line_to (cr, x2 + rise, y2);
          cairo_line_to (cr, x2 - border_radius, y2);
        }
      else
        {
          cairo_line_to (cr, x2, priv->arrow_origin - half_base);
          cairo_line_to (cr, x2 + rise, priv->arrow_origin);
          cairo_line_to (cr, x2, priv->arrow_origin + half_base);
        }
    }

  if (!skip_bottom_right)
    {
      cairo_line_to (cr, x2, y2 - border_radius);
      cairo_arc (cr, x2 - border_radius, y2 - border_radius, border_radius,
                 0, G_PI / 2);
    }

  if (priv->arrow_side == ST_SIDE_BOTTOM && rise != 0)
    {
      if (skip_bottom_left)
        {
          cairo_line_to (cr, x1 + half_base, y2);
          cairo_line_to (cr, x1, y2 + rise);
          cairo_line_to (cr, x1, y2 - border_radius);
        }
      else if (skip_bottom_right)
        {
          cairo_line_to (cr, x2, y2 + rise);
          cairo_line_to (cr, x2 - half_base, y2);
        }
      else
        {
          cairo_line_to (cr, priv->arrow_origin + half_base, y2);
          cairo_line_to (cr, priv->arrow_origin, y2 + rise);
          cairo_line_to (cr, priv->arrow_origin - half_base, y2);
        }
    }

  if (!skip_bottom_left)
    {
      cairo_line_to (cr, x1 + border_radius, y2);
      cairo_arc (cr, x1 + border_radius, y2 - border_radius, border_radius,
                 G_PI / 2, G_PI);
    }

  if (priv->arrow_side == ST_SIDE_LEFT && rise != 0)
    {
      if (skip_top_left)
        {
          cairo_line_to (cr, x1, y1 + half_base);
          cairo_line_to (cr, x1 - rise, y1);
          cairo_line_to (cr, x1 + border_radius, y1);
        }
      else if (skip_bottom_left)
        {
          cairo_line_to (cr, x1 - rise, y2);
          cairo_line_to (cr, x1 - rise, y2 - half_base);
        }
      else
        {
          cairo_line_to (cr, x1, priv->arrow_origin + half_base);
          cairo_line_to (cr, x1 - rise, priv->arrow_origin);
          cairo_line_to (cr, x1, priv->arrow_origin - half_base);
        }
    }

  if (!skip_top_left)
    {
      cairo_line_to (cr, x1, y1 + border_radius);
      cairo_arc (cr, x1 + border_radius, y1 + border_radius, border_radius,
                 G_PI, 3 * G_PI / 2);
    }

  has_color = st_theme_node_lookup_color (theme_node, "-arrow-background-color",
                                          FALSE, &bg_color);
  if (has_color)
    {
      set_source_color (cr, &bg_color);
      cairo_fill_preserve (cr);
    }

  if (border_width > 0)
    {
      st_theme_node_get_color (theme_node, "-arrow-border-color", &border_color);
      set_source_color (cr, &border_color);
      cairo_set_line_width (cr, border_width);
      cairo_stroke (cr);
    }
}

/*
 * Opening and closing
 */

typedef struct
{
  GWeakRef self;
  ShellBoxPointerDoneFunc on_complete;
  gpointer user_data;
  GDestroyNotify destroy;
} EaseData;

static EaseData *
ease_data_new (ShellBoxPointer         *self,
               ShellBoxPointerDoneFunc  on_complete,
               gpointer                 user_data,
               GDestroyNotify           destroy)
{
  EaseData *data = g_new0 (EaseData, 1);

  g_weak_ref_init (&data->self, self);
  data->on_complete = on_complete;
  data->user_data = user_data;
  data->destroy = destroy;

  return data;
}

static void
ease_data_free (gpointer user_data)
{
  EaseData *data = user_data;

  if (data->destroy)
    data->destroy (data->user_data);

  g_weak_ref_clear (&data->self);
  g_free (data);
}

static void
open_done_cb (gboolean finished,
              gpointer user_data)
{
  EaseData *data = user_data;
  g_autoptr (ShellBoxPointer) self = NULL;

  /* Like onComplete, only when the animation ran to completion */
  if (!finished)
    return;

  self = g_weak_ref_get (&data->self);
  if (self != NULL)
    {
      ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);

      clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (priv->mute_input), FALSE);
    }

  if (data->on_complete)
    data->on_complete (data->user_data);
}

static void
close_done_cb (gboolean finished,
               gpointer user_data)
{
  EaseData *data = user_data;
  g_autoptr (ShellBoxPointer) self = NULL;

  if (!finished)
    return;

  self = g_weak_ref_get (&data->self);
  if (self != NULL)
    {
      ClutterActor *actor = CLUTTER_ACTOR (self);

      clutter_actor_hide (actor);
      clutter_actor_set_opacity (actor, 0);
      g_object_set (actor,
                    "translation-x", 0.,
                    "translation-y", 0.,
                    NULL);
    }

  if (data->on_complete)
    data->on_complete (data->user_data);
}

static gboolean
use_motion (void)
{
  return st_settings_get_reduced_motion (st_settings_get ()) !=
         ST_REDUCED_MOTION_REDUCE;
}

/**
 * shell_box_pointer_open:
 * @self: a #ShellBoxPointer
 * @animate: how to animate the transition
 * @on_complete: (nullable) (scope notified) (closure user_data) (destroy destroy):
 *   called when the animation has completed
 * @user_data: user data for @on_complete
 * @destroy: called on @user_data after @on_complete was called, or when it
 *   will never be called
 *
 * Shows the box pointer, animated according to @animate.
 */
void
shell_box_pointer_open (ShellBoxPointer         *self,
                       ShellPopupAnimation      animate,
                       ShellBoxPointerDoneFunc  on_complete,
                       gpointer                 user_data,
                       GDestroyNotify           destroy)
{
  ShellBoxPointerPrivate *priv;
  ClutterActor *actor;
  StThemeNode *theme_node;
  double rise;
  ShellEaseParams params = {
    .duration = -1,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  g_return_if_fail (SHELL_IS_BOX_POINTER (self));

  priv = shell_box_pointer_get_instance_private (self);
  actor = CLUTTER_ACTOR (self);

  theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  rise = st_theme_node_get_length (theme_node, "-arrow-rise");
  params.duration = (animate & SHELL_POPUP_ANIMATION_FULL) ?
                    SHELL_POPUP_ANIMATION_TIME : 0;

  if (animate & SHELL_POPUP_ANIMATION_FADE)
    clutter_actor_set_opacity (actor, 0);
  else
    clutter_actor_set_opacity (actor, 255);

  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (priv->mute_keys), FALSE);
  clutter_actor_show (actor);

  if (use_motion () && (animate & SHELL_POPUP_ANIMATION_SLIDE))
    {
      clutter_actor_set_scale (actor, 0.96, 0.96);

      switch (priv->arrow_side)
        {
        case ST_SIDE_TOP:
          g_object_set (actor, "translation-y", -rise, NULL);
          clutter_actor_set_pivot_point (actor, 0.5, 0);
          break;
        case ST_SIDE_BOTTOM:
          g_object_set (actor, "translation-y", rise, NULL);
          clutter_actor_set_pivot_point (actor, 0.5, 1);
          break;
        case ST_SIDE_LEFT:
          g_object_set (actor, "translation-x", -rise, NULL);
          clutter_actor_set_pivot_point (actor, 0, 0.5);
          break;
        case ST_SIDE_RIGHT:
          g_object_set (actor, "translation-x", rise, NULL);
          clutter_actor_set_pivot_point (actor, 1, 0.5);
          break;
        default:
          g_assert_not_reached ();
        }
    }

  shell_actor_ease (actor, &params,
                    open_done_cb,
                    ease_data_new (self, on_complete, user_data, destroy),
                    ease_data_free,
                    "opacity", 255,
                    "translation-x", 0.,
                    "translation-y", 0.,
                    "scale-x", 1.,
                    "scale-y", 1.,
                    NULL);
}

/**
 * shell_box_pointer_close:
 * @self: a #ShellBoxPointer
 * @animate: how to animate the transition
 * @on_complete: (nullable) (scope notified) (closure user_data) (destroy destroy):
 *   called when the animation has completed. It is not called when the box
 *   pointer is not visible.
 * @user_data: user data for @on_complete
 * @destroy: called on @user_data after @on_complete was called, or when it
 *   will never be called
 *
 * Hides the box pointer, animated according to @animate. Does nothing if the
 * box pointer is not visible.
 */
void
shell_box_pointer_close (ShellBoxPointer         *self,
                         ShellPopupAnimation      animate,
                         ShellBoxPointerDoneFunc  on_complete,
                         gpointer                 user_data,
                         GDestroyNotify           destroy)
{
  ShellBoxPointerPrivate *priv;
  ClutterActor *actor;
  StThemeNode *theme_node;
  double translation_x = 0;
  double translation_y = 0;
  double scale_x = 1;
  double scale_y = 1;
  double rise;
  gboolean fade;
  ShellEaseParams params = {
    .duration = -1,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  g_return_if_fail (SHELL_IS_BOX_POINTER (self));

  actor = CLUTTER_ACTOR (self);

  if (!clutter_actor_is_visible (actor))
    {
      /* The callbacks will never run, but we still own the user data */
      if (destroy)
        destroy (user_data);
      return;
    }

  priv = shell_box_pointer_get_instance_private (self);

  theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  rise = st_theme_node_get_length (theme_node, "-arrow-rise");
  fade = (animate & SHELL_POPUP_ANIMATION_FADE) != 0;
  params.duration = (animate & SHELL_POPUP_ANIMATION_FULL) ?
                    SHELL_POPUP_ANIMATION_TIME : 0;

  if (use_motion () && (animate & SHELL_POPUP_ANIMATION_SLIDE))
    {
      scale_x = 0.96;
      scale_y = 0.96;

      switch (priv->arrow_side)
        {
        case ST_SIDE_TOP:
          translation_y = -rise;
          clutter_actor_set_pivot_point (actor, 0.5, 0);
          break;
        case ST_SIDE_BOTTOM:
          translation_y = rise;
          clutter_actor_set_pivot_point (actor, 0.5, 1);
          break;
        case ST_SIDE_LEFT:
          translation_x = -rise;
          clutter_actor_set_pivot_point (actor, 0, 0.5);
          break;
        case ST_SIDE_RIGHT:
          translation_x = rise;
          clutter_actor_set_pivot_point (actor, 1, 0.5);
          break;
        default:
          g_assert_not_reached ();
        }
    }

  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (priv->mute_input), TRUE);
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (priv->mute_keys), TRUE);

  clutter_actor_remove_all_transitions (actor);
  shell_actor_ease (actor, &params,
                    close_done_cb,
                    ease_data_new (self, on_complete, user_data, destroy),
                    ease_data_free,
                    "opacity", fade ? 0 : 255,
                    "translation-x", translation_x,
                    "translation-y", translation_y,
                    "scale-x", scale_x,
                    "scale-y", scale_y,
                    NULL);
}

/*
 * Public API
 */

/**
 * shell_box_pointer_new:
 * @arrow_side: the side to draw the arrow on
 *
 * Returns: (transfer full): a new #ShellBoxPointer
 */
ShellBoxPointer *
shell_box_pointer_new (StSide arrow_side)
{
  return g_object_new (SHELL_TYPE_BOX_POINTER,
                       "arrow-side", arrow_side,
                       NULL);
}

/**
 * shell_box_pointer_get_bin:
 * @self: a #ShellBoxPointer
 *
 * Returns: (transfer none): the container in which content can be placed
 */
StBin *
shell_box_pointer_get_bin (ShellBoxPointer *self)
{
  ShellBoxPointerPrivate *priv;

  g_return_val_if_fail (SHELL_IS_BOX_POINTER (self), NULL);

  priv = shell_box_pointer_get_instance_private (self);
  return priv->bin;
}

/**
 * shell_box_pointer_get_arrow_side:
 * @self: a #ShellBoxPointer
 *
 * Returns: the side the arrow is currently drawn on, which may differ from
 *   the requested one when the box pointer is flipped to stay on screen
 */
StSide
shell_box_pointer_get_arrow_side (ShellBoxPointer *self)
{
  ShellBoxPointerPrivate *priv;

  g_return_val_if_fail (SHELL_IS_BOX_POINTER (self), ST_SIDE_TOP);

  priv = shell_box_pointer_get_instance_private (self);
  return priv->arrow_side;
}

/**
 * shell_box_pointer_update_arrow_side:
 * @self: a #ShellBoxPointer
 * @side: the new side to draw the arrow on
 *
 * Sets the requested arrow side, resetting any flip.
 */
void
shell_box_pointer_update_arrow_side (ShellBoxPointer *self,
                                     StSide           side)
{
  ShellBoxPointerPrivate *priv;
  gboolean changed;

  g_return_if_fail (SHELL_IS_BOX_POINTER (self));

  priv = shell_box_pointer_get_instance_private (self);

  changed = priv->arrow_side != side;
  priv->arrow_side = priv->user_arrow_side = side;
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));

  if (changed)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ARROW_SIDE]);
  g_signal_emit (self, signals[ARROW_SIDE_CHANGED], 0);
}

/**
 * shell_box_pointer_get_arrow_alignment:
 * @self: a #ShellBoxPointer
 *
 * Returns: where the arrow points to along the source actor, between 0 and 1
 */
double
shell_box_pointer_get_arrow_alignment (ShellBoxPointer *self)
{
  ShellBoxPointerPrivate *priv;

  g_return_val_if_fail (SHELL_IS_BOX_POINTER (self), 0.0);

  priv = shell_box_pointer_get_instance_private (self);
  return priv->arrow_alignment;
}

/**
 * shell_box_pointer_set_arrow_alignment:
 * @self: a #ShellBoxPointer
 * @alignment: where the arrow points to along the source actor, clamped to
 *   the range between 0 and 1
 */
void
shell_box_pointer_set_arrow_alignment (ShellBoxPointer *self,
                                       double           alignment)
{
  ShellBoxPointerPrivate *priv;

  g_return_if_fail (SHELL_IS_BOX_POINTER (self));

  priv = shell_box_pointer_get_instance_private (self);

  alignment = CLAMP (alignment, 0.0, 1.0);
  if (priv->arrow_alignment != alignment)
    {
      priv->arrow_alignment = alignment;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ARROW_ALIGNMENT]);
    }

  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

/**
 * shell_box_pointer_get_arrow_actor:
 * @self: a #ShellBoxPointer
 *
 * Returns: (transfer none) (nullable): the actor the arrow is positioned
 *   relative to
 */
ClutterActor *
shell_box_pointer_get_arrow_actor (ShellBoxPointer *self)
{
  ShellBoxPointerPrivate *priv;

  g_return_val_if_fail (SHELL_IS_BOX_POINTER (self), NULL);

  priv = shell_box_pointer_get_instance_private (self);
  return priv->arrow_actor;
}

/**
 * shell_box_pointer_set_arrow_actor:
 * @self: a #ShellBoxPointer
 * @actor: (nullable): an actor relative to which the arrow is positioned
 *
 * Differently from shell_box_pointer_set_position(), this will not move the
 * box pointer itself, only the arrow.
 */
void
shell_box_pointer_set_arrow_actor (ShellBoxPointer *self,
                                   ClutterActor    *actor)
{
  ShellBoxPointerPrivate *priv;

  g_return_if_fail (SHELL_IS_BOX_POINTER (self));
  g_return_if_fail (actor == NULL || CLUTTER_IS_ACTOR (actor));

  priv = shell_box_pointer_get_instance_private (self);

  if (priv->arrow_actor != actor)
    {
      g_set_object (&priv->arrow_actor, actor);
      st_drawing_area_queue_repaint (priv->border);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ARROW_ACTOR]);
    }
}

/**
 * shell_box_pointer_set_arrow_origin:
 * @self: a #ShellBoxPointer
 * @origin: coordinate specifying the middle of the arrow, along the Y axis
 *   from the top for %ST_SIDE_LEFT and %ST_SIDE_RIGHT and along the X axis
 *   from the left for %ST_SIDE_TOP and %ST_SIDE_BOTTOM
 */
void
shell_box_pointer_set_arrow_origin (ShellBoxPointer *self,
                                    double           origin)
{
  ShellBoxPointerPrivate *priv;

  g_return_if_fail (SHELL_IS_BOX_POINTER (self));

  priv = shell_box_pointer_get_instance_private (self);

  if (priv->arrow_origin != origin)
    {
      priv->arrow_origin = origin;
      st_drawing_area_queue_repaint (priv->border);
    }
}

/**
 * shell_box_pointer_set_position:
 * @self: a #ShellBoxPointer
 * @source_actor: (nullable): the actor to point at
 * @alignment: where the arrow points to along the box, clamped to the range
 *   between 0 and 1
 *
 * Positions the box pointer next to @source_actor on every allocation.
 */
void
shell_box_pointer_set_position (ShellBoxPointer *self,
                                ClutterActor    *source_actor,
                                double           alignment)
{
  ShellBoxPointerPrivate *priv;

  g_return_if_fail (SHELL_IS_BOX_POINTER (self));
  g_return_if_fail (source_actor == NULL || CLUTTER_IS_ACTOR (source_actor));

  priv = shell_box_pointer_get_instance_private (self);

  if (priv->source_actor == NULL || source_actor != priv->source_actor)
    {
      clear_source_actor (self);

      priv->source_actor = source_actor;
      if (source_actor != NULL)
        priv->source_destroy_id =
          g_signal_connect (source_actor, "destroy",
                            G_CALLBACK (source_actor_destroy_cb), self);
    }

  shell_box_pointer_set_arrow_alignment (self, alignment);
}

/**
 * shell_box_pointer_set_source_alignment:
 * @self: a #ShellBoxPointer
 * @alignment: where along the source actor the box pointer points at,
 *   clamped to the range between 0 and 1
 */
void
shell_box_pointer_set_source_alignment (ShellBoxPointer *self,
                                        double           alignment)
{
  ShellBoxPointerPrivate *priv;

  g_return_if_fail (SHELL_IS_BOX_POINTER (self));

  priv = shell_box_pointer_get_instance_private (self);

  priv->source_alignment = CLAMP (alignment, 0.0, 1.0);

  if (priv->source_actor == NULL)
    return;

  shell_box_pointer_set_position (self, priv->source_actor,
                                  priv->arrow_alignment);
}

/**
 * shell_box_pointer_get_padding:
 * @self: a #ShellBoxPointer
 * @side: the side to get the padding of
 *
 * Returns: the padding of the content bin on @side
 */
double
shell_box_pointer_get_padding (ShellBoxPointer *self,
                               StSide           side)
{
  ShellBoxPointerPrivate *priv;
  StThemeNode *theme_node;

  g_return_val_if_fail (SHELL_IS_BOX_POINTER (self), 0.0);

  priv = shell_box_pointer_get_instance_private (self);
  theme_node = st_widget_get_theme_node (ST_WIDGET (priv->bin));

  return st_theme_node_get_padding (theme_node, side);
}

/**
 * shell_box_pointer_get_arrow_height:
 * @self: a #ShellBoxPointer
 *
 * Returns: the height of the arrow (`-arrow-rise`)
 */
double
shell_box_pointer_get_arrow_height (ShellBoxPointer *self)
{
  StThemeNode *theme_node;

  g_return_val_if_fail (SHELL_IS_BOX_POINTER (self), 0.0);

  theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  return st_theme_node_get_length (theme_node, "-arrow-rise");
}

/*
 * GObject
 */

static void
shell_box_pointer_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  ShellBoxPointer *self = SHELL_BOX_POINTER (object);

  switch (prop_id)
    {
    case PROP_BIN:
      g_value_set_object (value, shell_box_pointer_get_bin (self));
      break;
    case PROP_ARROW_SIDE:
      g_value_set_enum (value, shell_box_pointer_get_arrow_side (self));
      break;
    case PROP_ARROW_ALIGNMENT:
      g_value_set_double (value, shell_box_pointer_get_arrow_alignment (self));
      break;
    case PROP_ARROW_ACTOR:
      g_value_set_object (value, shell_box_pointer_get_arrow_actor (self));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_box_pointer_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  ShellBoxPointer *self = SHELL_BOX_POINTER (object);

  switch (prop_id)
    {
    case PROP_ARROW_SIDE:
      shell_box_pointer_update_arrow_side (self, g_value_get_enum (value));
      break;
    case PROP_ARROW_ALIGNMENT:
      shell_box_pointer_set_arrow_alignment (self, g_value_get_double (value));
      break;
    case PROP_ARROW_ACTOR:
      shell_box_pointer_set_arrow_actor (self, g_value_get_object (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_box_pointer_dispose (GObject *object)
{
  ShellBoxPointer *self = SHELL_BOX_POINTER (object);
  ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);

  set_unredirect_disabled (self, FALSE);
  clear_source_actor (self);
  g_clear_object (&priv->arrow_actor);

  G_OBJECT_CLASS (shell_box_pointer_parent_class)->dispose (object);

  /* after the chain-up, which destroys the actions with the actor */
  g_clear_object (&priv->mute_keys);
  g_clear_object (&priv->mute_input);
}

static gboolean
mute_keys_key_press_cb (ClutterKeyController *controller,
                        gpointer              user_data)
{
  return CLUTTER_EVENT_STOP;
}

static void
shell_box_pointer_init (ShellBoxPointer *self)
{
  ShellBoxPointerPrivate *priv = shell_box_pointer_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);

  clutter_actor_set_offscreen_redirect (actor, CLUTTER_OFFSCREEN_REDIRECT_ALWAYS);

  priv->arrow_side = ST_SIDE_TOP;
  priv->user_arrow_side = ST_SIDE_TOP;
  priv->arrow_origin = 0;
  priv->arrow_alignment = 0.5;
  priv->source_alignment = 0.5;

  priv->bin = ST_BIN (st_bin_new ());
  clutter_actor_add_child (actor, CLUTTER_ACTOR (priv->bin));

  priv->border = ST_DRAWING_AREA (g_object_new (ST_TYPE_DRAWING_AREA, NULL));
  g_signal_connect (priv->border, "repaint", G_CALLBACK (draw_border), self);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (priv->border));
  clutter_actor_set_child_above_sibling (actor,
                                         CLUTTER_ACTOR (priv->bin),
                                         CLUTTER_ACTOR (priv->border));

  /* Sunk: the actor drops its own reference to the actions when it is
   * destroyed, while the fields are used until the box pointer is gone */
  priv->mute_keys = g_object_ref_sink (clutter_key_controller_new (NULL));
  g_signal_connect (priv->mute_keys, "key-press",
                    G_CALLBACK (mute_keys_key_press_cb), NULL);
  clutter_actor_add_action_full (actor, "key-muting",
                                 CLUTTER_PHASE_CAPTURE, priv->mute_keys);

  priv->mute_input = g_object_ref_sink (clutter_click_gesture_new ());
  clutter_click_gesture_set_recognize_on_press (CLUTTER_CLICK_GESTURE (priv->mute_input),
                                                TRUE);
  clutter_actor_add_action_full (actor, "click-muting",
                                 CLUTTER_PHASE_CAPTURE, priv->mute_input);
}

static void
shell_box_pointer_class_init (ShellBoxPointerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->dispose = shell_box_pointer_dispose;
  object_class->get_property = shell_box_pointer_get_property;
  object_class->set_property = shell_box_pointer_set_property;

  actor_class->map = shell_box_pointer_map;
  actor_class->unmap = shell_box_pointer_unmap;
  actor_class->destroy = shell_box_pointer_destroy;
  actor_class->get_preferred_width = shell_box_pointer_get_preferred_width;
  actor_class->get_preferred_height = shell_box_pointer_get_preferred_height;
  actor_class->allocate = shell_box_pointer_allocate;

  /**
   * ShellBoxPointer:bin:
   *
   * The container in which content can be placed.
   */
  props[PROP_BIN] =
    g_param_spec_object ("bin", NULL, NULL,
                         ST_TYPE_BIN,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellBoxPointer:arrow-side:
   *
   * The side the arrow is drawn on. Reading it gives the side currently in
   * use, which differs from the requested one while the box pointer is
   * flipped to stay inside the work area. Setting it behaves like
   * shell_box_pointer_update_arrow_side().
   */
  props[PROP_ARROW_SIDE] =
    g_param_spec_enum ("arrow-side", NULL, NULL,
                       ST_TYPE_SIDE, ST_SIDE_TOP,
                       G_PARAM_READWRITE |
                       G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellBoxPointer:arrow-alignment:
   *
   * Where the arrow points to along the box, between 0 and 1. Set along with
   * the source actor by shell_box_pointer_set_position().
   */
  props[PROP_ARROW_ALIGNMENT] =
    g_param_spec_double ("arrow-alignment", NULL, NULL,
                         0.0, 1.0, 0.5,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellBoxPointer:arrow-actor:
   *
   * An actor relative to which the arrow is positioned, without moving the
   * box pointer itself.
   */
  props[PROP_ARROW_ACTOR] =
    g_param_spec_object ("arrow-actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellBoxPointer::arrow-side-changed:
   * @self: the #ShellBoxPointer
   *
   * Emitted when the arrow side changed, either because it was updated or
   * because the box pointer was flipped.
   */
  signals[ARROW_SIDE_CHANGED] =
    g_signal_new ("arrow-side-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (ShellBoxPointerClass, arrow_side_changed),
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}
