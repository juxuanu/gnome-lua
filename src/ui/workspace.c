/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * workspace.c: The window picker of one workspace on one monitor in the
 * overview. Port of js/ui/workspace.js.
 *
 * Window Thumbnail Layout Algorithm
 * =================================
 *
 * General overview
 * ----------------
 *
 * The window thumbnail layout algorithm calculates some optimal layout
 * by computing layouts with some number of rows, calculating how good
 * each layout is, and stopping iterating when it finds one that is worse
 * than the previous layout. A layout consists of which windows are in
 * which rows, row sizes and other general state tracking that would make
 * calculating window positions from this information fairly easy.
 *
 * After a layout is computed that's considered the best layout, we
 * compute the layout scale to fit it in the area, and then compute
 * slots (sizes and positions) for each thumbnail.
 *
 * Layout generation
 * -----------------
 *
 * Layout generation is naive and simple: we simply add windows to a row
 * until we've added too many windows to a row, and then make a new row,
 * until we have our required N rows. The potential issue with this strategy
 * is that we may have too many windows at the bottom in some pathological
 * cases, which tends to make the thumbnails have the shape of a pile of
 * sand with a peak, with one window at the top.
 *
 * Scaling factors
 * ---------------
 *
 * Thumbnail position is mostly straightforward -- the main issue is
 * computing an optimal scale for each window that fits the constraints,
 * and doesn't make the thumbnail too small to see. There are two factors
 * involved in thumbnail scale to make sure that these two goals are met:
 * the window scale (calculated by compute_window_scale ()) and the layout
 * scale (calculated by compute_scale_and_space ()).
 *
 * The calculation logic becomes slightly more complicated because row
 * and column spacing are not scaled, they're constant, so we can't
 * simply generate a bunch of window positions and then scale it. In
 * practice, it's not too bad -- we can simply try to fit the layout
 * in the input area minus whatever spacing we have, and then add
 * it back afterwards.
 *
 * The window scale is constant for the window's size regardless of the
 * input area or the layout scale or rows or anything else, and right
 * now just enlarges the window if it's too small. The fact that this
 * factor is stable makes it easy to calculate, so there's no sense
 * in not applying it in most calculations.
 *
 * The layout scale depends on the input area, the rows, etc, but is the
 * same for the entire layout, rather than being per-window. After
 * generating the rows of windows, we basically do some basic math to
 * fit the full, unscaled layout to the input area, as described above.
 *
 * With these two factors combined, the final scale of each thumbnail is
 * simply windowScale * layoutScale... almost.
 *
 * There's one additional constraint: the thumbnail scale must never be
 * larger than WINDOW_PREVIEW_MAXIMUM_SCALE, which means that the inequality:
 *
 *   windowScale * layoutScale <= WINDOW_PREVIEW_MAXIMUM_SCALE
 *
 * must always be true. This is for each individual window -- while we
 * could adjust layoutScale to make the largest thumbnail smaller than
 * WINDOW_PREVIEW_MAXIMUM_SCALE, it would shrink windows which are already
 * under the inequality. To solve this, we simply cheat: we simply keep
 * each window's "cell" area to be the same, but we shrink the thumbnail
 * and center it horizontally, and align it to the bottom vertically.
 *
 * Differences from the JS module that every reader of this file should know
 * about:
 *
 *  - `new Workspace (metaWorkspace, monitorIndex, overviewAdjustment)` is
 *    `new Shell.Workspace ({meta_workspace, monitor_index,
 *    overview_adjustment, window_manager})`. The construct-only
 *    `window-manager` is the #ShellWindowManager (`Main.wm`), whose
 *    `overview-closing`, `overview-animating` and `overview-visible`
 *    properties replace `Main.overview.closing`, `animationInProgress` and
 *    the `hidden` signal. `metaWorkspace` and `monitorIndex` are the
 *    read-only `meta-workspace` and `monitor-index` properties,
 *    `stateAdjustment` is `state-adjustment`.
 *  - The parts of the shell that are still JS are signals of the workspace:
 *    `overview-hide-requested` (`Main.overview.hide ()`),
 *    `window-activated (window, time)` (what `Main.activateWindow ()` does on
 *    top of activating the window: hide the overview, close the calendar) and
 *    `window-drag-begin`, `window-drag-cancelled` and `window-drag-end
 *    (window)` (`Main.overview.beginWindowDrag ()` and the like).
 *  - The methods are snake_case. `syncStacking (stackIndices)` takes the
 *    array of the stable sequences of the windows in stacking order instead
 *    of the object that maps them to their stacking index.
 *  - `WorkspaceLayout` (a JS class that screenshot.js extended) is private;
 *    the window selector of the screenshot UI uses #ShellWindowSlotLayout,
 *    which shares the layout algorithm, `new Shell.WindowSlotLayout
 *    ({monitor_index})`, with `add_window (window, bounding_box)`, `reset ()`
 *    and `get_windows ()`.
 *  - `WorkspaceBackground` is part of the workspace (it is not a type of its
 *    own any more).
 *  - The #ShellDragTarget of the workspace answers the drag of a window by
 *    the `meta-window` property of the dragged object (a window preview or a
 *    window clone with such a property), and the drag of an application by
 *    shell_drag_source_get_app () (the JS read `source.metaWindow`,
 *    `source.app` and `source.animateLaunchAtPos ()`). The
 *    `shellWorkspaceLaunch` hook for extensions is gone.
 *  - The layout used to connect to `workareas-changed` once per
 *    `set_container ()` without ever noticing it had (a typo in the check),
 *    it connects once.
 *  - The pointer is not looked at from a layout that has no windows, and a
 *    slot whose window is gone is skipped, where the JS threw.
 */

#include "config.h"

#include <math.h>

#include <clutter/clutter.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-background-content.h>
#include <meta/meta-background-group.h>
#include <meta/meta-workspace-manager.h>

#include "shell-app.h"
#include "shell-global.h"
#include "shell-workspace-background.h"
#include "ui/background.h"
#include "ui/dnd.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/workspace.h"

#define WINDOW_PREVIEW_MAXIMUM_SCALE 0.95

#define WINDOW_REPOSITIONING_DELAY 750

/* When calculating a layout, we calculate the scale of windows and the percent
 * of the available area the new layout uses. If the values for the new layout,
 * when weighted with the values as below, are worse than the previous layout's,
 * we stop looking for a new layout and use the previous layout.
 * Otherwise, we keep looking for a new layout. */
#define LAYOUT_SCALE_WEIGHT 1
#define LAYOUT_SPACE_WEIGHT 0.1

#define BACKGROUND_CORNER_RADIUS_PIXELS 30

/* OverviewControls.ControlsState.WINDOW_PICKER */
#define CONTROLS_STATE_WINDOW_PICKER 1.0

#define BOUNDING_BOX_KEY "shell-window-slot-bounding-box"

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ShellMonitor, shell_monitor_free)

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static double
lerp (double start,
      double end,
      double progress)
{
  return start + progress * (end - start);
}

static ClutterStage *
get_stage (void)
{
  return shell_global_get_stage (shell_global_get ());
}

static MetaDisplay *
get_display (void)
{
  return shell_global_get_display (shell_global_get ());
}

static ShellLayoutManager *
get_layout_manager (void)
{
  return shell_main_get_layout_manager (shell_main_get_default ());
}

typedef struct
{
  double x;
  double y;
  double width;
  double height;
} Area;

/* A slot of the layout: the final position and size of a window */
typedef struct
{
  double x;
  double y;
  double width;
  double height;
  ClutterActor *window;         /* borrowed */
} Slot;

static void
window_get_bounding_box (ClutterActor *window,
                         Area         *area)
{
  if (SHELL_IS_WINDOW_PREVIEW (window))
    {
      const ClutterActorBox *box =
        shell_window_preview_get_bounding_box (SHELL_WINDOW_PREVIEW (window));

      area->x = box->x1;
      area->y = box->y1;
      area->width = box->x2 - box->x1;
      area->height = box->y2 - box->y1;
    }
  else
    {
      const MtkRectangle *rect = g_object_get_data (G_OBJECT (window),
                                                    BOUNDING_BOX_KEY);

      if (rect != NULL)
        {
          area->x = rect->x;
          area->y = rect->y;
          area->width = rect->width;
          area->height = rect->height;
        }
      else
        {
          area->x = 0;
          area->y = 0;
          area->width = 0;
          area->height = 0;
        }
    }
}

static void
window_get_center (ClutterActor *window,
                   double       *x,
                   double       *y)
{
  Area area;

  window_get_bounding_box (window, &area);
  *x = area.x + area.width / 2;
  *y = area.y + area.height / 2;
}

static void
window_chrome_heights (ClutterActor *window,
                       double       *top,
                       double       *bottom)
{
  *top = 0;
  *bottom = 0;

  if (SHELL_IS_WINDOW_PREVIEW (window))
    shell_window_preview_chrome_heights (SHELL_WINDOW_PREVIEW (window), top, bottom);
}

static void
window_chrome_widths (ClutterActor *window,
                      double       *left,
                      double       *right)
{
  *left = 0;
  *right = 0;

  if (SHELL_IS_WINDOW_PREVIEW (window))
    shell_window_preview_chrome_widths (SHELL_WINDOW_PREVIEW (window), left, right);
}

static void
window_overlap_heights (ClutterActor *window,
                        double       *top,
                        double       *bottom)
{
  *top = 0;
  *bottom = 0;

  if (SHELL_IS_WINDOW_PREVIEW (window))
    shell_window_preview_overlap_heights (SHELL_WINDOW_PREVIEW (window), top, bottom);
}

static int
compare_doubles (double a,
                 double b)
{
  if (a < b)
    return -1;
  if (a > b)
    return 1;
  return 0;
}

static int
compare_windows_by_center_x (gconstpointer a,
                             gconstpointer b,
                             gpointer      user_data)
{
  double ax, ay, bx, by;

  window_get_center ((ClutterActor *) a, &ax, &ay);
  window_get_center ((ClutterActor *) b, &bx, &by);

  return compare_doubles (ax, bx);
}

static int
compare_windows_by_center_y (gconstpointer a,
                             gconstpointer b,
                             gpointer      user_data)
{
  double ax, ay, bx, by;

  window_get_center ((ClutterActor *) a, &ax, &ay);
  window_get_center ((ClutterActor *) b, &bx, &by);

  return compare_doubles (ay, by);
}

/* ------------------------------------------------------------------------ */
/* The layout strategy (UnalignedLayoutStrategy)                            */
/* ------------------------------------------------------------------------ */

typedef struct
{
  double x;
  double y;
  double width;
  double height;
  double full_width;
  double full_height;
  double additional_scale;
  GPtrArray *windows;           /* of ClutterActor, borrowed */
} Row;

typedef struct
{
  int num_rows;
  GPtrArray *rows;              /* of Row */
  int max_columns;
  double grid_width;
  double grid_height;
  double scale;
} Layout;

typedef struct
{
  double monitor_height;
  double row_spacing;
  double column_spacing;
} Strategy;

static void
row_free (Row *row)
{
  g_ptr_array_unref (row->windows);
  g_free (row);
}

static void
layout_free (Layout *layout)
{
  if (layout == NULL)
    return;

  g_ptr_array_unref (layout->rows);
  g_free (layout);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Layout, layout_free)

static Row *
row_new (void)
{
  Row *row = g_new0 (Row, 1);

  row->windows = g_ptr_array_new ();
  return row;
}

/* Computes and returns an individual scaling factor for @window,
 * to be applied in addition to the overall layout scale. */
static double
strategy_compute_window_scale (Strategy     *strategy,
                               ClutterActor *window)
{
  Area area;
  double ratio;

  /* Since we align windows next to each other, the height of the
   * thumbnails is much more important to preserve than the width of
   * them, so two windows with equal height, but maybe differering
   * widths line up. */
  window_get_bounding_box (window, &area);
  ratio = area.height / strategy->monitor_height;

  /* The purpose of this manipulation here is to prevent windows
   * from getting too small. For something like a calculator window,
   * we need to bump up the size just a bit to make sure it looks
   * good. We'll use a multiplier of 1.5 for this.
   *
   * Map from [0, 1] to [1.5, 1] */
  return lerp (1.5, 1, ratio);
}

static void
strategy_compute_row_sizes (Strategy *strategy,
                            Layout   *layout)
{
  guint i;

  for (i = 0; i < layout->rows->len; i++)
    {
      Row *row = g_ptr_array_index (layout->rows, i);

      row->width = row->full_width * layout->scale +
                   ((double) row->windows->len - 1) * strategy->column_spacing;
      row->height = row->full_height * layout->scale;
    }
}

static gboolean
strategy_keep_same_row (Row    *row,
                        double  width,
                        double  ideal_row_width)
{
  double old_ratio, new_ratio;

  if (row->full_width + width <= ideal_row_width)
    return TRUE;

  old_ratio = row->full_width / ideal_row_width;
  new_ratio = (row->full_width + width) / ideal_row_width;

  if (fabs (1 - new_ratio) < fabs (1 - old_ratio))
    return TRUE;

  return FALSE;
}

static Layout *
strategy_compute_layout (Strategy   *strategy,
                         GPtrArray  *windows,
                         int         num_rows)
{
  Layout *layout = g_new0 (Layout, 1);
  g_autoptr (GPtrArray) sorted_windows = NULL;
  double total_width = 0;
  double ideal_row_width;
  Row *max_row = NULL;
  double grid_height = 0;
  guint window_idx = 0;
  guint i;
  int r;

  layout->rows = g_ptr_array_new_with_free_func ((GDestroyNotify) row_free);

  for (i = 0; i < windows->len; i++)
    {
      ClutterActor *window = g_ptr_array_index (windows, i);
      double s = strategy_compute_window_scale (strategy, window);
      Area area;

      window_get_bounding_box (window, &area);
      total_width += area.width * s;
    }

  ideal_row_width = total_width / num_rows;

  /* Sort windows vertically to minimize travel distance.
   * This affects what rows the windows get placed in. */
  sorted_windows = g_ptr_array_copy (windows, NULL, NULL);
  g_ptr_array_sort_values_with_data (sorted_windows,
                                     compare_windows_by_center_y, NULL);

  for (r = 0; r < num_rows; r++)
    {
      Row *row = row_new ();

      g_ptr_array_add (layout->rows, row);

      for (; window_idx < sorted_windows->len; window_idx++)
        {
          ClutterActor *window = g_ptr_array_index (sorted_windows, window_idx);
          double s = strategy_compute_window_scale (strategy, window);
          Area area;
          double width, height;

          window_get_bounding_box (window, &area);
          width = area.width * s;
          height = area.height * s;
          row->full_height = MAX (row->full_height, height);

          /* either new width is < idealWidth or new width is nearer from idealWidth then oldWidth */
          if (strategy_keep_same_row (row, width, ideal_row_width) ||
              r == num_rows - 1)
            {
              g_ptr_array_add (row->windows, window);
              row->full_width += width;
            }
          else
            {
              break;
            }
        }
    }

  for (r = 0; r < num_rows; r++)
    {
      Row *row = g_ptr_array_index (layout->rows, r);

      /* Sort windows horizontally to minimize travel distance.
       * This affects in what order the windows end up in a row. */
      g_ptr_array_sort_values_with_data (row->windows,
                                         compare_windows_by_center_x, NULL);

      if (max_row == NULL || row->full_width > max_row->full_width)
        max_row = row;
      grid_height += row->full_height;
    }

  layout->num_rows = num_rows;
  layout->max_columns = max_row->windows->len;
  layout->grid_width = max_row->full_width;
  layout->grid_height = grid_height;

  return layout;
}

/* Given @layout and @area, compute the overall scale of the layout and
 * space occupied by the layout. It sets the fixed overall scale of the
 * layout, and so must be called before strategy_compute_window_slots (). */
static void
strategy_compute_scale_and_space (Strategy     *strategy,
                                  Layout       *layout,
                                  const Area   *area,
                                  double       *scale_out,
                                  double       *space_out)
{
  double hspacing = ((double) layout->max_columns - 1) * strategy->column_spacing;
  double vspacing = ((double) layout->num_rows - 1) * strategy->row_spacing;
  double spaced_width = area->width - hspacing;
  double spaced_height = area->height - vspacing;
  double horizontal_scale = spaced_width / layout->grid_width;
  double vertical_scale = spaced_height / layout->grid_height;
  double scale, scaled_layout_width, scaled_layout_height;

  /* Thumbnails should be less than 70% of the original size */
  scale = MIN (MIN (horizontal_scale, vertical_scale),
               WINDOW_PREVIEW_MAXIMUM_SCALE);

  scaled_layout_width = layout->grid_width * scale + hspacing;
  scaled_layout_height = layout->grid_height * scale + vspacing;

  layout->scale = scale;

  *scale_out = scale;
  *space_out = (scaled_layout_width * scaled_layout_height) /
               (area->width * area->height);
}

/* Returns an array of Slot with final position and size information for each
 * window of the layout, given a bounding area that it will be inside of. */
static GArray *
strategy_compute_window_slots (Strategy     *strategy,
                               Layout       *layout,
                               const Area   *area)
{
  GArray *slots = g_array_new (FALSE, FALSE, sizeof (Slot));
  double height_without_spacing = 0;
  double vertical_spacing, additional_vertical_scale;
  double compensation = 0;
  double y = 0;
  guint i, j;

  strategy_compute_row_sizes (strategy, layout);

  /* Do this in three parts. */
  for (i = 0; i < layout->rows->len; i++)
    {
      Row *row = g_ptr_array_index (layout->rows, i);

      height_without_spacing += row->height;
    }

  vertical_spacing = ((double) layout->rows->len - 1) * strategy->row_spacing;
  additional_vertical_scale = MIN (1, (area->height - vertical_spacing) / height_without_spacing);

  /* keep track how much smaller the grid becomes due to scaling
   * so it can be centered again */
  for (i = 0; i < layout->rows->len; i++)
    {
      Row *row = g_ptr_array_index (layout->rows, i);

      /* If this window layout row doesn't fit in the actual
       * geometry, then apply an additional scale to it. */
      double horizontal_spacing = ((double) row->windows->len - 1) * strategy->column_spacing;
      double width_without_spacing = row->width - horizontal_spacing;
      double additional_horizontal_scale =
        MIN (1, (area->width - horizontal_spacing) / width_without_spacing);

      if (additional_horizontal_scale < additional_vertical_scale)
        {
          row->additional_scale = additional_horizontal_scale;
          /* Only consider the scaling in addition to the vertical scaling for centering. */
          compensation += (additional_vertical_scale - additional_horizontal_scale) * row->height;
        }
      else
        {
          row->additional_scale = additional_vertical_scale;
          /* No compensation when scaling vertically since centering based on a too large
           * height would undo what vertical scaling is trying to achieve. */
        }

      row->x = area->x + (MAX (area->width - (width_without_spacing * row->additional_scale + horizontal_spacing), 0) / 2);
      row->y = area->y + (MAX (area->height - (height_without_spacing + vertical_spacing), 0) / 2) + y;
      y += row->height * row->additional_scale + strategy->row_spacing;
    }

  compensation /= 2;

  for (i = 0; i < layout->rows->len; i++)
    {
      Row *row = g_ptr_array_index (layout->rows, i);
      double row_y = row->y + compensation;
      double row_height = row->height * row->additional_scale;
      double x = row->x;

      for (j = 0; j < row->windows->len; j++)
        {
          ClutterActor *window = g_ptr_array_index (row->windows, j);
          Area bounding_box;
          double s, cell_width, cell_height, clone_width, clone_height;
          double clone_x, clone_y;
          Slot slot;

          window_get_bounding_box (window, &bounding_box);

          s = layout->scale * strategy_compute_window_scale (strategy, window) *
              row->additional_scale;
          cell_width = bounding_box.width * s;
          cell_height = bounding_box.height * s;

          s = MIN (s, WINDOW_PREVIEW_MAXIMUM_SCALE);
          clone_width = bounding_box.width * s;
          clone_height = bounding_box.height * s;

          clone_x = x + (cell_width - clone_width) / 2;

          /* If there's only one row, align windows vertically centered inside the row */
          if (layout->rows->len == 1)
            clone_y = row_y + (row_height - clone_height) / 2;
          /* If there are multiple rows, align windows to the bottom edge of the row */
          else
            clone_y = row_y + row_height - cell_height;

          /* Align with the pixel grid to prevent blurry windows at scale = 1 */
          clone_x = floor (clone_x);
          clone_y = floor (clone_y);

          slot.x = clone_x;
          slot.y = clone_y;
          slot.width = clone_width;
          slot.height = clone_height;
          slot.window = window;
          g_array_append_val (slots, slot);

          x += cell_width + strategy->column_spacing;
        }
    }

  return slots;
}

/* ------------------------------------------------------------------------ */
/* LayoutCore: the state that the layouts of the workspace and of the        */
/* screenshot window selector share                                          */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ClutterLayoutManager *manager;
  int monitor_index;
  double spacing;

  ClutterActor *container;      /* weak, set by set_container () */

  MtkRectangle workarea;
  gboolean has_workarea;
  MetaDisplay *display;         /* weak, while connected */
  gulong workareas_changed_id;

  GPtrArray *sorted_windows;    /* of ClutterActor, strong */

  Strategy strategy;
  Layout *layout;
  GArray *window_slots;         /* of Slot */

  ClutterActorBox last_box;
  gboolean has_last_box;
  ClutterActorBox window_slots_box;
  gboolean has_window_slots_box;
} LayoutCore;

static void
core_init (LayoutCore           *core,
           ClutterLayoutManager *manager,
           int                   monitor_index)
{
  core->manager = manager;
  core->monitor_index = monitor_index;
  core->spacing = 20;
  core->sorted_windows = g_ptr_array_new_with_free_func (g_object_unref);
  core->window_slots = g_array_new (FALSE, FALSE, sizeof (Slot));
}

static void
core_disconnect_workareas (LayoutCore *core)
{
  if (core->display != NULL)
    {
      g_clear_signal_handler (&core->workareas_changed_id, core->display);
      g_clear_weak_pointer (&core->display);
    }
  core->workareas_changed_id = 0;
}

static void
core_clear (LayoutCore *core)
{
  core_disconnect_workareas (core);

  g_clear_pointer (&core->sorted_windows, g_ptr_array_unref);
  g_clear_pointer (&core->window_slots, g_array_unref);
  g_clear_pointer (&core->layout, layout_free);
}

static void
core_update_workarea (LayoutCore *core)
{
  shell_layout_manager_get_work_area_for_monitor (get_layout_manager (),
                                                  core->monitor_index,
                                                  &core->workarea);
  core->has_workarea = TRUE;
}

static void
core_workareas_changed_cb (MetaDisplay *display,
                           LayoutCore  *core)
{
  core_update_workarea (core);
  clutter_layout_manager_layout_changed (core->manager);
}

static void
core_sync_workarea_tracking (LayoutCore *core)
{
  if (core->container != NULL)
    {
      if (core->workareas_changed_id != 0)
        return;

      core_update_workarea (core);

      /* Set through the weak-pointer helper only: assigning the field first
       * makes g_set_weak_pointer () a no-op and the clear in dispose fails */
      g_set_weak_pointer (&core->display, get_display ());
      core->workareas_changed_id =
        g_signal_connect (core->display, "workareas-changed",
                          G_CALLBACK (core_workareas_changed_cb), core);
    }
  else
    {
      core_disconnect_workareas (core);
    }
}

static void
core_set_container (LayoutCore   *core,
                    ClutterActor *container)
{
  core->container = container;
  core_sync_workarea_tracking (core);
}

static void
core_get_adjusted_workarea (LayoutCore   *core,
                            ClutterActor *container,
                            double       *width,
                            double       *height)
{
  if (!core->has_workarea)
    core_update_workarea (core);

  *width = core->workarea.width;
  *height = core->workarea.height;

  if (ST_IS_WIDGET (container))
    {
      StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (container));

      *width -= st_theme_node_get_horizontal_padding (theme_node);
      *height -= st_theme_node_get_vertical_padding (theme_node);
    }
}

static void
core_get_preferred_width (LayoutCore   *core,
                          ClutterActor *container,
                          float         for_height,
                          float        *min_width_p,
                          float        *natural_width_p)
{
  double width, height;

  core_get_adjusted_workarea (core, container, &width, &height);

  if (min_width_p)
    *min_width_p = 0;

  if (natural_width_p)
    {
      if (for_height == -1)
        *natural_width_p = width;
      else
        *natural_width_p = for_height * (width / height);
    }
}

static void
core_get_preferred_height (LayoutCore   *core,
                           ClutterActor *container,
                           float         for_width,
                           float        *min_height_p,
                           float        *natural_height_p)
{
  double width, height;

  core_get_adjusted_workarea (core, container, &width, &height);

  if (min_height_p)
    *min_height_p = 0;

  if (natural_height_p)
    {
      if (for_width == -1)
        *natural_height_p = height;
      else
        *natural_height_p = for_width / (width / height);
    }
}

static gboolean
is_better_scale_and_space (double old_scale,
                           double old_space,
                           double scale,
                           double space)
{
  double space_power = (space - old_space) * LAYOUT_SPACE_WEIGHT;
  double scale_power = (scale - old_scale) * LAYOUT_SCALE_WEIGHT;

  if (scale > old_scale && space > old_space)
    /* Win win -- better scale and better space */
    return TRUE;
  else if (scale > old_scale && space <= old_space)
    /* Keep new layout only if scale gain outweighs aspect space loss */
    return scale_power > space_power;
  else if (scale <= old_scale && space > old_space)
    /* Keep new layout only if aspect space gain outweighs scale loss */
    return space_power > scale_power;
  else
    /* Lose -- worse scale and space */
    return FALSE;
}

/* The biggest of the chrome sizes of the windows. All of the overlays have
 * the same chrome sizes, so just pick the first window. */
static double
core_get_oversize (LayoutCore *core)
{
  ClutterActor *window;
  double top, bottom, left, right;

  if (core->sorted_windows->len == 0)
    return 0;

  window = g_ptr_array_index (core->sorted_windows, 0);
  window_chrome_heights (window, &top, &bottom);
  window_chrome_widths (window, &left, &right);

  return MAX (MAX (top, bottom), MAX (left, right));
}

static void
core_adjust_container_box (LayoutCore      *core,
                           ClutterActorBox *container_box)
{
  g_autoptr (ShellMonitor) monitor = NULL;
  ClutterActor *window;
  graphene_point3d_t bottom_point, transformed_bottom_point;
  double oversize, top_overlap, bottom_overlap, bottom_free_space;

  if (core->sorted_windows->len == 0)
    return;

  window = g_ptr_array_index (core->sorted_windows, 0);
  oversize = core_get_oversize (core);

  monitor = shell_layout_manager_get_monitor (get_layout_manager (),
                                              core->monitor_index);
  if (monitor == NULL || core->container == NULL)
    return;

  bottom_point = GRAPHENE_POINT3D_INIT (0, container_box->y2, 0);
  clutter_actor_apply_transform_to_point (core->container, &bottom_point,
                                          &transformed_bottom_point);
  bottom_free_space =
    ((double) monitor->y + monitor->height) - transformed_bottom_point.y;

  window_overlap_heights (window, &top_overlap, &bottom_overlap);

  if ((bottom_overlap + oversize) > bottom_free_space)
    container_box->y2 -= (bottom_overlap + oversize) - bottom_free_space;
}

static Layout *
core_create_best_layout (LayoutCore  *core,
                         const Area  *area)
{
  g_autoptr (ShellMonitor) monitor =
    shell_layout_manager_get_monitor (get_layout_manager (), core->monitor_index);
  double oversize = core_get_oversize (core);
  Layout *last_layout = NULL;
  int last_num_columns = -1;
  double last_scale = 0;
  double last_space = 0;
  int num_rows;

  /* We look for the largest scale that allows us to fit the
   * largest row/tallest column on the workspace. */
  core->strategy.monitor_height = monitor != NULL ? monitor->height : 1;
  core->strategy.row_spacing = core->spacing + oversize;
  core->strategy.column_spacing = core->spacing + oversize;

  for (num_rows = 1; ; num_rows++)
    {
      int num_columns = (int) ceil ((double) core->sorted_windows->len / num_rows);
      Layout *layout;
      double scale, space;

      /* If adding a new row does not change column count just stop
       * (for instance: 9 windows, with 3 rows -> 3 columns, 4 rows ->
       * 3 columns as well => just use 3 rows then) */
      if (num_columns == last_num_columns)
        break;

      layout = strategy_compute_layout (&core->strategy, core->sorted_windows,
                                        num_rows);

      strategy_compute_scale_and_space (&core->strategy, layout, area,
                                        &scale, &space);

      if (last_layout != NULL &&
          !is_better_scale_and_space (last_scale, last_space, scale, space))
        {
          layout_free (layout);
          break;
        }

      layout_free (last_layout);
      last_layout = layout;
      last_num_columns = num_columns;
      last_scale = scale;
      last_space = space;
    }

  return last_layout;
}

static GArray *
core_get_window_slots (LayoutCore      *core,
                       ClutterActorBox *container_box)
{
  Area avail_area;

  core_adjust_container_box (core, container_box);

  avail_area.x = trunc (container_box->x1);
  avail_area.y = trunc (container_box->y1);
  avail_area.width = trunc (clutter_actor_box_get_width (container_box));
  avail_area.height = trunc (clutter_actor_box_get_height (container_box));

  if (core->layout == NULL)
    return g_array_new (FALSE, FALSE, sizeof (Slot));

  return strategy_compute_window_slots (&core->strategy, core->layout, &avail_area);
}

static void
core_remove_slot (LayoutCore   *core,
                  ClutterActor *window)
{
  guint i;

  for (i = 0; i < core->window_slots->len; i++)
    {
      if (g_array_index (core->window_slots, Slot, i).window == window)
        {
          g_array_remove_index (core->window_slots, i);
          return;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* ShellWorkspaceLayout (WorkspaceLayout)                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WORKSPACE_LAYOUT (shell_workspace_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellWorkspaceLayout, shell_workspace_layout,
                      SHELL, WORKSPACE_LAYOUT, ClutterLayoutManager)

typedef struct
{
  ShellWorkspaceLayout *layout;
  ClutterActor *window;         /* strong */
  MetaWindow *meta_window;      /* strong */
  gulong size_changed_id;
  gulong destroy_id;
  ClutterTransition *current_transition; /* strong */
  gulong stopped_id;
} WindowInfo;

struct _ShellWorkspaceLayout
{
  ClutterLayoutManager parent;

  LayoutCore core;

  MetaWorkspace *meta_workspace;        /* weak, nullable */
  StAdjustment *overview_adjustment;
  ShellWindowManager *window_manager;   /* weak */
  StAdjustment *state_adjustment;

  GPtrArray *infos;                     /* of WindowInfo */

  gboolean layout_frozen;
  gboolean needs_layout;
};

G_DEFINE_FINAL_TYPE (ShellWorkspaceLayout, shell_workspace_layout,
                     CLUTTER_TYPE_LAYOUT_MANAGER)

static void workspace_layout_remove_window (ShellWorkspaceLayout *self,
                                            ClutterActor         *window);

static gboolean
overview_animating (ShellWorkspaceLayout *self)
{
  return self->window_manager != NULL &&
         shell_window_manager_get_overview_animating (self->window_manager);
}

static WindowInfo *
workspace_layout_find_info (ShellWorkspaceLayout *self,
                            ClutterActor         *window)
{
  guint i;

  for (i = 0; i < self->infos->len; i++)
    {
      WindowInfo *info = g_ptr_array_index (self->infos, i);

      if (info->window == window)
        return info;
    }

  return NULL;
}

static void
window_info_release_transition (WindowInfo *info)
{
  if (info->current_transition != NULL)
    {
      g_clear_signal_handler (&info->stopped_id, info->current_transition);
      g_clear_object (&info->current_transition);
    }
  info->stopped_id = 0;
}

static void
window_info_free (WindowInfo *info)
{
  window_info_release_transition (info);
  g_clear_signal_handler (&info->size_changed_id, info->meta_window);
  g_clear_signal_handler (&info->destroy_id, info->window);
  g_clear_object (&info->meta_window);
  g_clear_object (&info->window);
  g_free (info);
}

static void
workspace_layout_sync_opacity (ShellWorkspaceLayout *self,
                               ClutterActor         *actor,
                               MetaWindow           *meta_window)
{
  if (!meta_window_showing_on_its_workspace (meta_window))
    {
      clutter_actor_set_opacity (actor,
                                 (guint8) (st_adjustment_get_value (self->state_adjustment) * 255));
    }
}

static void
workspace_layout_sync_opacities (ShellWorkspaceLayout *self)
{
  guint i;

  for (i = 0; i < self->infos->len; i++)
    {
      WindowInfo *info = g_ptr_array_index (self->infos, i);

      workspace_layout_sync_opacity (self, info->window, info->meta_window);
    }
}

static void
workspace_layout_sync_overlay (ShellWorkspaceLayout *self,
                               ClutterActor         *preview)
{
  gboolean active = TRUE;

  if (self->meta_workspace != NULL)
    g_object_get (self->meta_workspace, "active", &active, NULL);

  shell_window_preview_set_overlay_enabled (SHELL_WINDOW_PREVIEW (preview),
                                            active &&
                                            st_adjustment_get_value (self->state_adjustment) == 1);
}

/* Synchronizes the overlay state of all window previews. */
static void
workspace_layout_sync_overlays (ShellWorkspaceLayout *self)
{
  g_autoptr (GPtrArray) windows = g_ptr_array_new_with_free_func (g_object_unref);
  guint i;

  /* the previews can run arbitrary code when their overlay changes */
  for (i = 0; i < self->infos->len; i++)
    {
      WindowInfo *info = g_ptr_array_index (self->infos, i);

      g_ptr_array_add (windows, g_object_ref (info->window));
    }

  for (i = 0; i < windows->len; i++)
    workspace_layout_sync_overlay (self, g_ptr_array_index (windows, i));
}

static void
state_adjustment_value_changed_cb (ShellWorkspaceLayout *self)
{
  workspace_layout_sync_opacities (self);
  workspace_layout_sync_overlays (self);
  clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
}

static void
workspace_layout_set_container (ClutterLayoutManager *manager,
                                ClutterActor         *container)
{
  ShellWorkspaceLayout *self = SHELL_WORKSPACE_LAYOUT (manager);

  core_set_container (&self->core, container);
  st_adjustment_set_actor (self->state_adjustment, container);
}

static void
workspace_layout_get_preferred_width (ClutterLayoutManager *manager,
                                      ClutterActor         *container,
                                      float                 for_height,
                                      float                *min_width_p,
                                      float                *natural_width_p)
{
  ShellWorkspaceLayout *self = SHELL_WORKSPACE_LAYOUT (manager);

  core_get_preferred_width (&self->core, container, for_height,
                            min_width_p, natural_width_p);
}

static void
workspace_layout_get_preferred_height (ClutterLayoutManager *manager,
                                       ClutterActor         *container,
                                       float                 for_width,
                                       float                *min_height_p,
                                       float                *natural_height_p)
{
  ShellWorkspaceLayout *self = SHELL_WORKSPACE_LAYOUT (manager);

  core_get_preferred_height (&self->core, container, for_width,
                             min_height_p, natural_height_p);
}

static ClutterTransition *
animate_allocation (ClutterActor          *actor,
                    const ClutterActorBox *box)
{
  clutter_actor_save_easing_state (actor);
  clutter_actor_set_easing_mode (actor, CLUTTER_EASE_OUT_QUAD);
  clutter_actor_set_easing_duration (actor, 200);

  clutter_actor_allocate (actor, box);

  clutter_actor_restore_easing_state (actor);

  return clutter_actor_get_transition (actor, "allocation");
}

static void
transition_stopped_cb (ClutterTransition *transition,
                       gboolean           is_finished,
                       WindowInfo        *info)
{
  window_info_release_transition (info);
}

static void
workspace_layout_allocate (ClutterLayoutManager *manager,
                           ClutterActor         *container,
                           const ClutterActorBox *box)
{
  ShellWorkspaceLayout *self = SHELL_WORKSPACE_LAYOUT (manager);
  LayoutCore *core = &self->core;
  ClutterActorBox container_box;
  float container_width, container_height;
  gboolean container_allocation_changed;
  double current_state;
  gboolean in_session_transition;
  ClutterActor *first_window;
  gboolean layout_changed = FALSE;
  double slots_scale;
  double workarea_x, workarea_y, workarea_width;
  double state_adjustment_value;
  double allocation_scale;
  guint i, n_slots;

  clutter_actor_get_allocation_box (container, &container_box);
  clutter_actor_box_get_size (&container_box, &container_width, &container_height);
  container_allocation_changed =
    !core->has_last_box || !clutter_actor_box_equal (&core->last_box, &container_box);

  if (!core->has_workarea)
    core_update_workarea (core);

  /* If the containers size changed, we can no longer keep around
   * the old windowSlots, so we must unfreeze the layout.
   *
   * However, if the overview animation is in progress, don't unfreeze
   * the layout. This is needed to prevent windows "snapping" to their
   * new positions during the overview closing animation when the
   * allocation subtly expands every frame. */
  if (self->layout_frozen && container_allocation_changed &&
      !overview_animating (self))
    self->layout_frozen = FALSE;

  if (self->overview_adjustment != NULL)
    {
      shell_window_preview_get_state_transition_params (self->overview_adjustment,
                                                        &current_state, NULL, NULL);
    }
  else
    {
      current_state = CONTROLS_STATE_WINDOW_PICKER;
    }
  in_session_transition = current_state <= CONTROLS_STATE_WINDOW_PICKER;

  first_window = core->sorted_windows->len > 0
    ? g_ptr_array_index (core->sorted_windows, 0) : NULL;

  if (in_session_transition || first_window == NULL)
    {
      clutter_actor_remove_clip (container);
    }
  else
    {
      double top_oversize, bottom_oversize;
      float container_x, container_y;
      double extra_height_progress, extra_clip_height;

      window_chrome_heights (first_window, &top_oversize, &bottom_oversize);
      clutter_actor_box_get_origin (&container_box, &container_x, &container_y);

      extra_height_progress = current_state - CONTROLS_STATE_WINDOW_PICKER;
      extra_clip_height = bottom_oversize * (1 - extra_height_progress);

      clutter_actor_set_clip (container, container_x, container_y,
                              container_width, container_height + extra_clip_height);
    }

  if (!self->layout_frozen || !core->has_last_box)
    {
      if (self->needs_layout)
        {
          Area workarea = { core->workarea.x, core->workarea.y,
                            core->workarea.width, core->workarea.height };

          layout_free (core->layout);
          core->layout = core_create_best_layout (core, &workarea);
          self->needs_layout = FALSE;
          layout_changed = TRUE;
        }

      if (layout_changed || container_allocation_changed)
        {
          ClutterActorBox slots_box = *box;

          core->window_slots_box = *box;
          core->has_window_slots_box = TRUE;

          g_array_unref (core->window_slots);
          core->window_slots = core_get_window_slots (core, &slots_box);
        }
    }

  if (!core->has_window_slots_box)
    {
      core->last_box = container_box;
      core->has_last_box = TRUE;
      return;
    }

  slots_scale = clutter_actor_box_get_width (box) /
                clutter_actor_box_get_width (&core->window_slots_box);
  workarea_x = core->workarea.x;
  workarea_y = core->workarea.y;
  workarea_width = core->workarea.width;
  state_adjustment_value = st_adjustment_get_value (self->state_adjustment);

  allocation_scale = container_width / workarea_width;

  n_slots = core->window_slots->len;
  for (i = 0; i < n_slots; i++)
    {
      Slot slot = g_array_index (core->window_slots, Slot, i);
      ClutterActor *child = slot.window;
      WindowInfo *window_info;
      ClutterActorBox child_box;
      double x, y, width, height;
      double workspace_box_x, workspace_box_y;
      double workspace_box_width, workspace_box_height;

      if (!clutter_actor_is_visible (child))
        continue;

      x = slot.x * slots_scale;
      y = slot.y * slots_scale;
      width = slot.width * slots_scale;
      height = slot.height * slots_scale;

      window_info = workspace_layout_find_info (self, child);
      if (window_info == NULL)
        continue;

      if (meta_window_showing_on_its_workspace (window_info->meta_window))
        {
          const ClutterActorBox *bounding_box =
            shell_window_preview_get_bounding_box (SHELL_WINDOW_PREVIEW (child));

          workspace_box_x = (bounding_box->x1 - workarea_x) * allocation_scale;
          workspace_box_y = (bounding_box->y1 - workarea_y) * allocation_scale;
          workspace_box_width = (bounding_box->x2 - bounding_box->x1) * allocation_scale;
          workspace_box_height = (bounding_box->y2 - bounding_box->y1) * allocation_scale;
        }
      else
        {
          workspace_box_x = workarea_x * allocation_scale;
          workspace_box_y = workarea_y * allocation_scale;
          workspace_box_width = 0;
          workspace_box_height = 0;
        }

      /* Don't allow the scaled floating size to drop below
       * the target layout size.
       * We only want to apply this when the scaled floating size is
       * actually larger than the target layout size, that is while
       * animating between the session and the window picker. */
      if (in_session_transition)
        {
          workspace_box_width = MAX (workspace_box_width, width);
          workspace_box_height = MAX (workspace_box_height, height);
        }

      x = lerp (workspace_box_x, x, state_adjustment_value);
      y = lerp (workspace_box_y, y, state_adjustment_value);
      width = lerp (workspace_box_width, width, state_adjustment_value);
      height = lerp (workspace_box_height, height, state_adjustment_value);

      clutter_actor_box_set_origin (&child_box, x, y);
      clutter_actor_box_set_size (&child_box, width, height);

      if (window_info->current_transition != NULL)
        {
          ClutterInterval *interval =
            clutter_transition_get_interval (window_info->current_transition);
          GValue value = G_VALUE_INIT;
          ClutterActorBox child_allocation;

          g_value_init (&value, CLUTTER_TYPE_ACTOR_BOX);
          g_value_set_boxed (&value, &child_box);
          clutter_interval_set_final_value (interval, &value);
          g_value_unset (&value);

          /* The timeline of the transition might not have been updated
           * before this allocation cycle, so make sure the child
           * still updates needs_allocation to FALSE.
           * Unfortunately, this relies on the fast paths in
           * clutter_actor_allocate(), otherwise we'd start a new
           * transition on the child, replacing the current one. */
          clutter_actor_get_allocation_box (child, &child_allocation);
          clutter_actor_allocate (child, &child_allocation);
          continue;
        }

      /* We want layout changes (ie. larger changes to the layout like
       * reshuffling the window order) to be animated, but small changes
       * like changes to the container size to happen immediately (for
       * example if the container height is being animated, we want to
       * avoid animating the children allocations to make sure they
       * don't "lag behind" the other animation). */
      if (layout_changed && !overview_animating (self))
        {
          ClutterTransition *transition = animate_allocation (child, &child_box);

          if (transition != NULL)
            {
              window_info->current_transition = g_object_ref (transition);
              window_info->stopped_id =
                g_signal_connect (transition, "stopped",
                                  G_CALLBACK (transition_stopped_cb), window_info);
            }
        }
      else
        {
          clutter_actor_allocate (child, &child_box);
        }
    }

  core->last_box = container_box;
  core->has_last_box = TRUE;
}

static void
window_size_changed_cb (MetaWindow *meta_window,
                        WindowInfo *info)
{
  info->layout->needs_layout = TRUE;
  clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (info->layout));
}

static void
window_destroyed_cb (ClutterActor *window,
                     WindowInfo   *info)
{
  workspace_layout_remove_window (info->layout, window);
}

static int
compare_windows_by_stable_sequence (gconstpointer a,
                                    gconstpointer b)
{
  MetaWindow *window_a =
    shell_window_preview_get_meta_window (SHELL_WINDOW_PREVIEW ((ClutterActor *) a));
  MetaWindow *window_b =
    shell_window_preview_get_meta_window (SHELL_WINDOW_PREVIEW ((ClutterActor *) b));
  guint32 seq_a = meta_window_get_stable_sequence (window_a);
  guint32 seq_b = meta_window_get_stable_sequence (window_b);

  return seq_a < seq_b ? -1 : (seq_a > seq_b ? 1 : 0);
}

/* Adds @window to the workspace, it will be shown immediately if
 * the layout isn't frozen. If @window is already part of the workspace,
 * nothing will happen. */
static void
workspace_layout_add_window (ShellWorkspaceLayout *self,
                             ShellWindowPreview   *window,
                             MetaWindow           *meta_window)
{
  ClutterActor *actor = CLUTTER_ACTOR (window);
  WindowInfo *info;

  if (workspace_layout_find_info (self, actor) != NULL)
    return;

  info = g_new0 (WindowInfo, 1);
  info->layout = self;
  info->window = g_object_ref (actor);
  info->meta_window = g_object_ref (meta_window);
  info->size_changed_id = g_signal_connect (meta_window, "size-changed",
                                            G_CALLBACK (window_size_changed_cb), info);
  info->destroy_id = g_signal_connect (actor, "destroy",
                                       G_CALLBACK (window_destroyed_cb), info);
  g_ptr_array_add (self->infos, info);

  g_ptr_array_add (self->core.sorted_windows, g_object_ref (actor));
  g_ptr_array_sort_values (self->core.sorted_windows,
                           compare_windows_by_stable_sequence);

  workspace_layout_sync_opacity (self, actor, meta_window);
  workspace_layout_sync_overlay (self, actor);
  if (self->core.container != NULL)
    clutter_actor_add_child (self->core.container, actor);

  self->needs_layout = TRUE;
  clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
}

/* Removes @window from the workspace if @window is a part of the
 * workspace. If the layout is frozen, the window will still be visible
 * until it is unfrozen. */
static void
workspace_layout_remove_window (ShellWorkspaceLayout *self,
                                ClutterActor         *window)
{
  WindowInfo *info = workspace_layout_find_info (self, window);
  g_autoptr (ClutterActor) window_ref = NULL;

  if (info == NULL)
    return;

  window_ref = g_object_ref (window);

  if (info->current_transition != NULL)
    {
      window_info_release_transition (info);
      clutter_actor_remove_transition (window, "allocation");
    }

  g_ptr_array_remove (self->core.sorted_windows, window);
  g_ptr_array_remove (self->infos, info);   /* frees info */

  /* The layout might be frozen and we might not update the windowSlots
   * on the next allocation, so remove the slot now already */
  core_remove_slot (&self->core, window);

  /* The layout refers to the windows */
  g_clear_pointer (&self->core.layout, layout_free);

  /* The window might have been reparented by DND */
  if (self->core.container != NULL &&
      clutter_actor_get_parent (window) == self->core.container)
    clutter_actor_remove_child (self->core.container, window);

  self->needs_layout = TRUE;
  clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
}

static int
compare_by_stack_index (gconstpointer a,
                        gconstpointer b,
                        gpointer      user_data)
{
  GHashTable *indices = user_data;
  MetaWindow *window_a =
    shell_window_preview_get_meta_window (SHELL_WINDOW_PREVIEW ((ClutterActor *) a));
  MetaWindow *window_b =
    shell_window_preview_get_meta_window (SHELL_WINDOW_PREVIEW ((ClutterActor *) b));
  gpointer index_a = g_hash_table_lookup (indices,
                                          GUINT_TO_POINTER (meta_window_get_stable_sequence (window_a)));
  gpointer index_b = g_hash_table_lookup (indices,
                                          GUINT_TO_POINTER (meta_window_get_stable_sequence (window_b)));

  /* indices are stored plus one, windows that are not stacked come last */
  return compare_doubles (GPOINTER_TO_UINT (index_a) == 0 ? G_MAXDOUBLE : GPOINTER_TO_UINT (index_a),
                          GPOINTER_TO_UINT (index_b) == 0 ? G_MAXDOUBLE : GPOINTER_TO_UINT (index_b));
}

static void
workspace_layout_sync_stacking (ShellWorkspaceLayout *self,
                                const guint32        *stable_sequences,
                                gsize                 n_stable_sequences)
{
  g_autoptr (GHashTable) indices = g_hash_table_new (NULL, NULL);
  g_autoptr (GPtrArray) windows = g_ptr_array_new_with_free_func (g_object_unref);
  ShellWindowPreview *last_window = NULL;
  gsize i;

  for (i = 0; i < n_stable_sequences; i++)
    {
      g_hash_table_insert (indices, GUINT_TO_POINTER (stable_sequences[i]),
                           GUINT_TO_POINTER (i + 1));
    }

  for (i = 0; i < self->infos->len; i++)
    {
      WindowInfo *info = g_ptr_array_index (self->infos, i);

      g_ptr_array_add (windows, g_object_ref (info->window));
    }

  g_ptr_array_sort_values_with_data (windows, compare_by_stack_index, indices);

  for (i = 0; i < windows->len; i++)
    {
      ShellWindowPreview *window = g_ptr_array_index (windows, i);

      shell_window_preview_set_stack_above (window, last_window);
      last_window = window;
    }

  self->needs_layout = TRUE;
  clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
}

/* The focus chain of the workspace. It is empty if the floating window
 * layout is used. */
static GList *
workspace_layout_get_focus_chain (ShellWorkspaceLayout *self)
{
  GList *chain = NULL;
  guint i;

  if (st_adjustment_get_value (self->state_adjustment) == 0)
    return NULL;

  for (i = 0; i < self->core.window_slots->len; i++)
    chain = g_list_prepend (chain, g_array_index (self->core.window_slots, Slot, i).window);

  return g_list_reverse (chain);
}

static void
workspace_layout_set_spacing (ShellWorkspaceLayout *self,
                              double                spacing)
{
  if (self->core.spacing == spacing)
    return;

  self->core.spacing = spacing;

  self->needs_layout = TRUE;
  clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
}

static void
workspace_layout_set_layout_frozen (ShellWorkspaceLayout *self,
                                    gboolean              frozen)
{
  frozen = !!frozen;
  if (self->layout_frozen == frozen)
    return;

  self->layout_frozen = frozen;

  if (!frozen)
    clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
}

static void
shell_workspace_layout_dispose (GObject *object)
{
  ShellWorkspaceLayout *self = SHELL_WORKSPACE_LAYOUT (object);

  if (self->infos != NULL)
    g_ptr_array_set_size (self->infos, 0);

  core_clear (&self->core);

  g_clear_weak_pointer (&self->meta_workspace);
  g_clear_weak_pointer (&self->window_manager);
  g_clear_object (&self->overview_adjustment);
  if (self->state_adjustment != NULL)
    g_signal_handlers_disconnect_by_data (self->state_adjustment, self);
  g_clear_object (&self->state_adjustment);

  G_OBJECT_CLASS (shell_workspace_layout_parent_class)->dispose (object);
}

static void
shell_workspace_layout_finalize (GObject *object)
{
  ShellWorkspaceLayout *self = SHELL_WORKSPACE_LAYOUT (object);

  g_clear_pointer (&self->infos, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_workspace_layout_parent_class)->finalize (object);
}

static void
shell_workspace_layout_class_init (ShellWorkspaceLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  object_class->dispose = shell_workspace_layout_dispose;
  object_class->finalize = shell_workspace_layout_finalize;

  layout_class->set_container = workspace_layout_set_container;
  layout_class->get_preferred_width = workspace_layout_get_preferred_width;
  layout_class->get_preferred_height = workspace_layout_get_preferred_height;
  layout_class->allocate = workspace_layout_allocate;
}

static void
shell_workspace_layout_init (ShellWorkspaceLayout *self)
{
  core_init (&self->core, CLUTTER_LAYOUT_MANAGER (self), 0);

  self->infos = g_ptr_array_new_with_free_func ((GDestroyNotify) window_info_free);
  self->needs_layout = TRUE;

  self->state_adjustment = st_adjustment_new (NULL, 0, 0, 1, 0, 0, 0);
  g_signal_connect_swapped (self->state_adjustment, "notify::value",
                            G_CALLBACK (state_adjustment_value_changed_cb), self);
}

static ShellWorkspaceLayout *
workspace_layout_new (MetaWorkspace      *meta_workspace,
                      int                 monitor_index,
                      StAdjustment       *overview_adjustment,
                      ShellWindowManager *window_manager)
{
  ShellWorkspaceLayout *self = g_object_new (SHELL_TYPE_WORKSPACE_LAYOUT, NULL);

  self->core.monitor_index = monitor_index;
  g_set_weak_pointer (&self->meta_workspace, meta_workspace);
  g_set_object (&self->overview_adjustment, overview_adjustment);
  g_set_weak_pointer (&self->window_manager, window_manager);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellWindowSlotLayout (UIWindowSelectorLayout)                            */
/* ------------------------------------------------------------------------ */

struct _ShellWindowSlotLayout
{
  ClutterLayoutManager parent;

  LayoutCore core;
};

enum
{
  SLOT_PROP_0,
  SLOT_PROP_MONITOR_INDEX,
  SLOT_N_PROPS
};

static GParamSpec *slot_props[SLOT_N_PROPS] = { NULL, };

G_DEFINE_FINAL_TYPE (ShellWindowSlotLayout, shell_window_slot_layout,
                     CLUTTER_TYPE_LAYOUT_MANAGER)

static void
slot_layout_set_container (ClutterLayoutManager *manager,
                           ClutterActor         *container)
{
  ShellWindowSlotLayout *self = SHELL_WINDOW_SLOT_LAYOUT (manager);

  core_set_container (&self->core, container);
}

static void
slot_layout_get_preferred_width (ClutterLayoutManager *manager,
                                 ClutterActor         *container,
                                 float                 for_height,
                                 float                *min_width_p,
                                 float                *natural_width_p)
{
  ShellWindowSlotLayout *self = SHELL_WINDOW_SLOT_LAYOUT (manager);

  core_get_preferred_width (&self->core, container, for_height,
                            min_width_p, natural_width_p);
}

static void
slot_layout_get_preferred_height (ClutterLayoutManager *manager,
                                  ClutterActor         *container,
                                  float                 for_width,
                                  float                *min_height_p,
                                  float                *natural_height_p)
{
  ShellWindowSlotLayout *self = SHELL_WINDOW_SLOT_LAYOUT (manager);

  core_get_preferred_height (&self->core, container, for_width,
                             min_height_p, natural_height_p);
}

static void
slot_layout_allocate (ClutterLayoutManager   *manager,
                      ClutterActor           *container,
                      const ClutterActorBox  *box)
{
  ShellWindowSlotLayout *self = SHELL_WINDOW_SLOT_LAYOUT (manager);
  LayoutCore *core = &self->core;
  ClutterActorBox container_box;
  gboolean container_allocation_changed;
  gboolean layout_changed = FALSE;
  guint i;

  clutter_actor_get_allocation_box (container, &container_box);
  container_allocation_changed =
    !core->has_last_box || !clutter_actor_box_equal (&core->last_box, &container_box);
  core->last_box = container_box;
  core->has_last_box = TRUE;

  if (!core->has_workarea)
    core_update_workarea (core);

  if (core->layout == NULL)
    {
      Area workarea = { core->workarea.x, core->workarea.y,
                        core->workarea.width, core->workarea.height };

      core->layout = core_create_best_layout (core, &workarea);
      layout_changed = TRUE;
    }

  if (layout_changed || container_allocation_changed)
    {
      ClutterActorBox slots_box = *box;

      g_array_unref (core->window_slots);
      core->window_slots = core_get_window_slots (core, &slots_box);
    }

  for (i = 0; i < core->window_slots->len; i++)
    {
      Slot slot = g_array_index (core->window_slots, Slot, i);
      ClutterActorBox child_box;

      clutter_actor_box_set_origin (&child_box, slot.x, slot.y);
      clutter_actor_box_set_size (&child_box, slot.width, slot.height);

      clutter_actor_allocate (slot.window, &child_box);
    }
}

static void
slot_layout_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  ShellWindowSlotLayout *self = SHELL_WINDOW_SLOT_LAYOUT (object);

  switch (prop_id)
    {
    case SLOT_PROP_MONITOR_INDEX:
      g_value_set_int (value, self->core.monitor_index);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
slot_layout_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  ShellWindowSlotLayout *self = SHELL_WINDOW_SLOT_LAYOUT (object);

  switch (prop_id)
    {
    case SLOT_PROP_MONITOR_INDEX:
      self->core.monitor_index = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_window_slot_layout_dispose (GObject *object)
{
  ShellWindowSlotLayout *self = SHELL_WINDOW_SLOT_LAYOUT (object);

  core_clear (&self->core);

  G_OBJECT_CLASS (shell_window_slot_layout_parent_class)->dispose (object);
}

static void
shell_window_slot_layout_class_init (ShellWindowSlotLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  object_class->dispose = shell_window_slot_layout_dispose;
  object_class->get_property = slot_layout_get_property;
  object_class->set_property = slot_layout_set_property;

  layout_class->set_container = slot_layout_set_container;
  layout_class->get_preferred_width = slot_layout_get_preferred_width;
  layout_class->get_preferred_height = slot_layout_get_preferred_height;
  layout_class->allocate = slot_layout_allocate;

  /**
   * ShellWindowSlotLayout:monitor-index:
   *
   * The monitor whose work area the windows are laid out in.
   */
  slot_props[SLOT_PROP_MONITOR_INDEX] =
    g_param_spec_int ("monitor-index", NULL, NULL,
                      0, G_MAXINT, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, SLOT_N_PROPS, slot_props);
}

static void
shell_window_slot_layout_init (ShellWindowSlotLayout *self)
{
  core_init (&self->core, CLUTTER_LAYOUT_MANAGER (self), 0);
}

/**
 * shell_window_slot_layout_new:
 * @monitor_index: the monitor whose work area the windows are laid out in
 *
 * Creates the layout of the window selector of the screenshot UI
 * (`UIWindowSelectorLayout`): it arranges its windows like the window picker
 * of the overview does, without chrome and without animations.
 *
 * Returns: (transfer floating): a new #ShellWindowSlotLayout
 */
ShellWindowSlotLayout *
shell_window_slot_layout_new (int monitor_index)
{
  return g_object_new (SHELL_TYPE_WINDOW_SLOT_LAYOUT,
                       "monitor-index", monitor_index,
                       NULL);
}

/**
 * shell_window_slot_layout_add_window:
 * @self: a #ShellWindowSlotLayout
 * @window: the actor of a window, which is added to the container
 * @bounding_box: the frame rectangle of the window
 *
 * Adds @window to the container of the layout, unless it was added already.
 */
void
shell_window_slot_layout_add_window (ShellWindowSlotLayout *self,
                                     ClutterActor          *window,
                                     const MtkRectangle    *bounding_box)
{
  guint i;

  g_return_if_fail (SHELL_IS_WINDOW_SLOT_LAYOUT (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (window));
  g_return_if_fail (bounding_box != NULL);

  for (i = 0; i < self->core.sorted_windows->len; i++)
    {
      if (g_ptr_array_index (self->core.sorted_windows, i) == window)
        return;
    }

  g_object_set_data_full (G_OBJECT (window), BOUNDING_BOX_KEY,
                          g_memdup2 (bounding_box, sizeof (MtkRectangle)),
                          g_free);

  g_ptr_array_add (self->core.sorted_windows, g_object_ref (window));

  if (self->core.container != NULL)
    clutter_actor_add_child (self->core.container, window);

  g_clear_pointer (&self->core.layout, layout_free);
  clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
}

/**
 * shell_window_slot_layout_reset:
 * @self: a #ShellWindowSlotLayout
 *
 * Destroys all the windows of the layout.
 */
void
shell_window_slot_layout_reset (ShellWindowSlotLayout *self)
{
  g_autoptr (GPtrArray) windows = NULL;
  guint i;

  g_return_if_fail (SHELL_IS_WINDOW_SLOT_LAYOUT (self));

  windows = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = 0; i < self->core.sorted_windows->len; i++)
    {
      g_ptr_array_add (windows,
                       g_object_ref (g_ptr_array_index (self->core.sorted_windows, i)));
    }

  g_ptr_array_set_size (self->core.sorted_windows, 0);
  g_array_set_size (self->core.window_slots, 0);
  g_clear_pointer (&self->core.layout, layout_free);

  for (i = 0; i < windows->len; i++)
    clutter_actor_destroy (g_ptr_array_index (windows, i));
}

/**
 * shell_window_slot_layout_get_windows:
 * @self: a #ShellWindowSlotLayout
 *
 * Returns: (transfer container) (element-type Clutter.Actor): the windows
 *   of the layout, in the order they were added
 */
GList *
shell_window_slot_layout_get_windows (ShellWindowSlotLayout *self)
{
  GList *windows = NULL;
  guint i;

  g_return_val_if_fail (SHELL_IS_WINDOW_SLOT_LAYOUT (self), NULL);

  for (i = 0; i < self->core.sorted_windows->len; i++)
    windows = g_list_prepend (windows, g_ptr_array_index (self->core.sorted_windows, i));

  return g_list_reverse (windows);
}

/* ------------------------------------------------------------------------ */
/* ShellWorkspace                                                            */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_META_WORKSPACE,
  PROP_MONITOR_INDEX,
  PROP_OVERVIEW_ADJUSTMENT,
  PROP_WINDOW_MANAGER,
  PROP_STATE_ADJUSTMENT,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  OVERVIEW_HIDE_REQUESTED,
  WINDOW_ACTIVATED,
  WINDOW_DRAG_BEGIN,
  WINDOW_DRAG_CANCELLED,
  WINDOW_DRAG_END,

  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0, };

struct _ShellWorkspace
{
  StWidget parent_instance;

  MetaWorkspace *meta_workspace;
  int monitor_index;
  StAdjustment *overview_adjustment;
  ShellWindowManager *window_manager;   /* weak */

  ClutterActor *container;
  ShellWorkspaceLayout *layout;         /* owned by container */

  /* WorkspaceBackground */
  ClutterActor *background;
  ShellBackgroundManager *bg_manager;
  MtkRectangle bg_workarea;

  GHashTable *skip_taskbar_signals;     /* MetaWindow, strong -> handler id */
  GPtrArray *windows;                   /* of ShellWindowPreview, strong */

  guint layout_frozen_id;
  int frozen_pointer_x;
  int frozen_pointer_y;

  gulong overview_visible_id;
  gboolean destroyed;
};

static void shell_workspace_drag_target_iface_init (ShellDragTargetInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellWorkspace, shell_workspace, ST_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (SHELL_TYPE_DRAG_TARGET,
                                                      shell_workspace_drag_target_iface_init))

static void do_add_window    (ShellWorkspace *self,
                              MetaWindow     *meta_win);
static void do_remove_window (ShellWorkspace *self,
                              MetaWindow     *meta_win);

static gboolean
overview_closing (ShellWorkspace *self)
{
  return self->window_manager != NULL &&
         shell_window_manager_get_overview_closing (self->window_manager);
}

static int
lookup_index (ShellWorkspace *self,
              MetaWindow     *meta_window)
{
  guint i;

  for (i = 0; i < self->windows->len; i++)
    {
      ShellWindowPreview *preview = g_ptr_array_index (self->windows, i);

      if (shell_window_preview_get_meta_window (preview) == meta_window)
        return i;
    }

  return -1;
}

static gboolean
workspace_is_active (ShellWorkspace *self)
{
  gboolean active = FALSE;

  g_object_get (self->meta_workspace, "active", &active, NULL);
  return active;
}

static gboolean
should_leave_overview (ShellWorkspace *self)
{
  if (self->meta_workspace == NULL || workspace_is_active (self))
    return TRUE;

  return st_adjustment_get_value (self->overview_adjustment) >
         CONTROLS_STATE_WINDOW_PICKER;
}

static gboolean
is_my_window (ShellWorkspace *self,
              MetaWindow     *window)
{
  gboolean is_on_workspace = self->meta_workspace == NULL ||
    meta_window_located_on_workspace (window, self->meta_workspace);
  gboolean is_on_monitor = meta_window_get_monitor (window) == self->monitor_index;

  return is_on_workspace && is_on_monitor;
}

static gboolean
is_overview_window (MetaWindow *window)
{
  return !meta_window_is_skip_taskbar (window);
}

static void
clear_skip_taskbar_signals (ShellWorkspace *self)
{
  GHashTableIter iter;
  gpointer key, value;

  if (self->skip_taskbar_signals == NULL)
    return;

  g_hash_table_iter_init (&iter, self->skip_taskbar_signals);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_signal_handler_disconnect (key, GPOINTER_TO_SIZE (value));
  g_hash_table_remove_all (self->skip_taskbar_signals);
}

static void
unfreeze_layout (ShellWorkspace *self)
{
  workspace_layout_set_layout_frozen (self->layout, FALSE);
}

static void
freeze_layout (ShellWorkspace *self)
{
  workspace_layout_set_layout_frozen (self->layout, TRUE);
}

/* ---- the windows -------------------------------------------------------- */

static void
on_skip_taskbar_changed (MetaWindow     *meta_win,
                         GParamSpec     *pspec,
                         ShellWorkspace *self)
{
  if (meta_window_is_skip_taskbar (meta_win))
    do_remove_window (self, meta_win);
  else
    do_add_window (self, meta_win);
}

static void
preview_selected_cb (ShellWindowPreview *clone,
                     guint               time,
                     ShellWorkspace     *self)
{
  MetaWindow *window = shell_window_preview_get_meta_window (clone);

  if (should_leave_overview (self))
    {
      /* Main.activateWindow () */
      MetaWorkspaceManager *workspace_manager =
        shell_global_get_workspace_manager (shell_global_get ());
      int active_workspace_num =
        meta_workspace_manager_get_active_workspace_index (workspace_manager);
      int window_workspace_num;

      if (self->meta_workspace != NULL)
        {
          window_workspace_num = meta_workspace_index (self->meta_workspace);
        }
      else
        {
          MetaWorkspace *window_workspace = meta_window_get_workspace (window);

          window_workspace_num = window_workspace != NULL
            ? meta_workspace_index (window_workspace) : active_workspace_num;
        }

      if (time == 0)
        time = shell_global_get_current_time (shell_global_get ());

      if (window_workspace_num != active_workspace_num)
        {
          MetaWorkspace *workspace =
            meta_workspace_manager_get_workspace_by_index (workspace_manager,
                                                           window_workspace_num);

          meta_workspace_activate_with_focus (workspace, window, time);
        }
      else
        {
          meta_window_activate (window, time);
        }

      g_signal_emit (self, signals[WINDOW_ACTIVATED], 0, window, time);
    }
  else if (self->meta_workspace != NULL)
    {
      meta_workspace_activate (self->meta_workspace, time);
    }
}

static void
preview_drag_begin_cb (ShellWindowPreview *clone,
                       ShellWorkspace     *self)
{
  g_signal_emit (self, signals[WINDOW_DRAG_BEGIN], 0,
                 shell_window_preview_get_meta_window (clone));
}

static void
preview_drag_cancelled_cb (ShellWindowPreview *clone,
                           ShellWorkspace     *self)
{
  g_signal_emit (self, signals[WINDOW_DRAG_CANCELLED], 0,
                 shell_window_preview_get_meta_window (clone));
}

static void
preview_drag_end_cb (ShellWindowPreview *clone,
                     ShellWorkspace     *self)
{
  g_signal_emit (self, signals[WINDOW_DRAG_END], 0,
                 shell_window_preview_get_meta_window (clone));
}

static void
preview_show_chrome_cb (ShellWindowPreview *clone,
                        ShellWorkspace     *self)
{
  ClutterActor *focus = clutter_stage_get_key_focus (get_stage ());
  g_autoptr (GPtrArray) windows = g_ptr_array_new_with_free_func (g_object_unref);
  guint i;

  for (i = 0; i < self->windows->len; i++)
    g_ptr_array_add (windows, g_object_ref (g_ptr_array_index (self->windows, i)));

  if (focus == NULL || clutter_actor_contains (CLUTTER_ACTOR (self), focus))
    clutter_actor_grab_key_focus (CLUTTER_ACTOR (clone));

  for (i = 0; i < windows->len; i++)
    {
      ShellWindowPreview *c = g_ptr_array_index (windows, i);

      if (c != clone)
        shell_window_preview_hide_overlay (c, TRUE);
    }
}

static void
preview_destroyed_cb (ClutterActor   *clone,
                      ShellWorkspace *self)
{
  MetaWindow *meta_window =
    shell_window_preview_get_meta_window (SHELL_WINDOW_PREVIEW (clone));

  if (meta_window != NULL)
    do_remove_window (self, meta_window);
}

/* Create a clone of a (non-desktop) window and add it to the window list */
static ShellWindowPreview *
add_window_clone (ShellWorkspace *self,
                  MetaWindow     *meta_window)
{
  ShellWindowPreview *clone =
    shell_window_preview_new (meta_window,
                              SHELL_DRAG_TARGET (self),
                              self->overview_adjustment);

  g_object_ref_sink (clone);

  g_signal_connect_object (clone, "selected",
                           G_CALLBACK (preview_selected_cb), self, 0);
  g_signal_connect_object (clone, "drag-begin",
                           G_CALLBACK (preview_drag_begin_cb), self, 0);
  g_signal_connect_object (clone, "drag-cancelled",
                           G_CALLBACK (preview_drag_cancelled_cb), self, 0);
  g_signal_connect_object (clone, "drag-end",
                           G_CALLBACK (preview_drag_end_cb), self, 0);
  g_signal_connect_object (clone, "show-chrome",
                           G_CALLBACK (preview_show_chrome_cb), self, 0);
  g_signal_connect_object (clone, "destroy",
                           G_CALLBACK (preview_destroyed_cb), self, 0);

  workspace_layout_add_window (self->layout, clone, meta_window);

  if (self->windows->len == 0)
    shell_window_preview_set_stack_above (clone, NULL);
  else
    shell_window_preview_set_stack_above (clone, g_ptr_array_index (self->windows,
                                                                    self->windows->len - 1));

  g_ptr_array_add (self->windows, clone);

  return clone;
}

/* The caller owns the returned clone */
static ShellWindowPreview *
remove_window_clone (ShellWorkspace *self,
                     MetaWindow     *meta_win)
{
  ShellWindowPreview *clone;
  int index;

  /* find the position of the window in our list */
  index = lookup_index (self, meta_win);
  if (index == -1)
    return NULL;

  clone = g_ptr_array_index (self->windows, index);
  g_object_ref (clone);

  workspace_layout_remove_window (self->layout, CLUTTER_ACTOR (clone));

  g_ptr_array_remove_index (self->windows, index);

  return clone;
}

static gboolean
layout_frozen_timeout_cb (gpointer user_data)
{
  ShellWorkspace *self = user_data;
  ShellGlobal *global = shell_global_get ();
  int new_x, new_y;
  gboolean pointer_has_moved;
  ClutterActor *actor_under_pointer;
  guint i;

  shell_global_get_pointer (global, &new_x, &new_y, NULL);
  pointer_has_moved = self->frozen_pointer_x != new_x ||
                      self->frozen_pointer_y != new_y;
  actor_under_pointer =
    clutter_stage_get_actor_at_pos (shell_global_get_stage (global),
                                    CLUTTER_PICK_REACTIVE, new_x, new_y);

  if (actor_under_pointer != NULL)
    {
      gboolean keep_frozen =
        pointer_has_moved &&
        clutter_actor_contains (CLUTTER_ACTOR (self), actor_under_pointer);

      for (i = 0; !keep_frozen && i < self->windows->len; i++)
        {
          ClutterActor *window = g_ptr_array_index (self->windows, i);

          keep_frozen = clutter_actor_contains (window, actor_under_pointer);
        }

      if (keep_frozen)
        {
          self->frozen_pointer_x = new_x;
          self->frozen_pointer_y = new_y;
          return G_SOURCE_CONTINUE;
        }
    }

  unfreeze_layout (self);
  self->layout_frozen_id = 0;
  return G_SOURCE_REMOVE;
}

static void
do_remove_window (ShellWorkspace *self,
                  MetaWindow     *meta_win)
{
  g_autoptr (ShellWindowPreview) clone = remove_window_clone (self, meta_win);
  int x, y;

  if (clone == NULL)
    return;

  clutter_actor_destroy (CLUTTER_ACTOR (clone));

  if (self->destroyed)
    return;

  /* We need to reposition the windows; to avoid shuffling windows
   * around while the user is interacting with the workspace, we delay
   * the positioning until the pointer remains still for at least 750 ms
   * or is moved outside the workspace */
  freeze_layout (self);

  g_clear_handle_id (&self->layout_frozen_id, g_source_remove);

  shell_global_get_pointer (shell_global_get (), &x, &y, NULL);
  self->frozen_pointer_x = x;
  self->frozen_pointer_y = y;

  self->layout_frozen_id = g_timeout_add (WINDOW_REPOSITIONING_DELAY,
                                          layout_frozen_timeout_cb, self);
  g_source_set_name_by_id (self->layout_frozen_id,
                           "[gnome-shell] this._layoutFrozenId");
}

/* onStopped of the animation of a new preview */
static void
clone_scale_done_cb (gboolean finished,
                     gpointer user_data)
{
  clutter_actor_set_pivot_point (CLUTTER_ACTOR (user_data), 0, 0);
}

typedef struct
{
  GWeakRef workspace;
  MetaWindow *window;
} DelayedAdd;

static void
delayed_add_free (DelayedAdd *data)
{
  g_weak_ref_clear (&data->workspace);
  g_object_unref (data->window);
  g_free (data);
}

static gboolean
delayed_add_cb (gpointer user_data)
{
  DelayedAdd *data = user_data;
  g_autoptr (ShellWorkspace) self = g_weak_ref_get (&data->workspace);

  /* Newly-created windows are added to a workspace before the compositor
   * finds out about them... */
  if (self != NULL && !self->destroyed &&
      meta_window_get_compositor_private (data->window) != NULL &&
      meta_window_get_workspace (data->window) == self->meta_workspace)
    do_add_window (self, data->window);

  return G_SOURCE_REMOVE;
}

static void
do_add_window (ShellWorkspace *self,
               MetaWindow     *meta_win)
{
  ShellWindowPreview *clone;
  ShellEaseParams params = {
    .duration = 250,
    .delay = -1,
    .mode = 0,
  };

  if (self->destroyed)
    return;

  if (meta_window_get_compositor_private (meta_win) == NULL)
    {
      DelayedAdd *data = g_new0 (DelayedAdd, 1);
      guint id;

      g_weak_ref_init (&data->workspace, self);
      data->window = g_object_ref (meta_win);

      id = g_idle_add_full (G_PRIORITY_DEFAULT, delayed_add_cb, data,
                            (GDestroyNotify) delayed_add_free);
      g_source_set_name_by_id (id, "[gnome-shell] this._doAddWindow");
      return;
    }

  /* We might have the window in our list already if it was on all workspaces and
   * now was moved to this workspace */
  if (shell_workspace_contains_meta_window (self, meta_win))
    return;

  if (!is_my_window (self, meta_win))
    return;

  if (!g_hash_table_contains (self->skip_taskbar_signals, meta_win))
    {
      gulong id = g_signal_connect (meta_win, "notify::skip-taskbar",
                                    G_CALLBACK (on_skip_taskbar_changed), self);

      g_hash_table_insert (self->skip_taskbar_signals,
                           g_object_ref (meta_win), GSIZE_TO_POINTER (id));
    }

  if (!is_overview_window (meta_win))
    {
      MetaWindow *parent;
      int index;

      if (meta_window_get_transient_for (meta_win) == NULL)
        return;

      /* Let the top-most ancestor handle all transients */
      parent = meta_window_find_root_ancestor (meta_win);
      index = lookup_index (self, parent);

      /* If no clone was found, the parent hasn't been created yet
       * and will take care of the dialog when added */
      if (index >= 0)
        shell_window_preview_add_dialog (g_ptr_array_index (self->windows, index),
                                         meta_win);

      return;
    }

  clone = add_window_clone (self, meta_win);

  clutter_actor_set_pivot_point (CLUTTER_ACTOR (clone), 0.5, 0.5);
  clutter_actor_set_scale (CLUTTER_ACTOR (clone), 0, 0);
  shell_actor_ease (CLUTTER_ACTOR (clone), &params,
                    clone_scale_done_cb, g_object_ref (clone), g_object_unref,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    NULL);

  if (self->layout_frozen_id > 0)
    {
      /* If a window was closed before, unfreeze the layout to ensure
       * the new window is immediately shown */
      unfreeze_layout (self);

      g_clear_handle_id (&self->layout_frozen_id, g_source_remove);
    }
}

/* ---- the events of the workspace and of the display ---------------------- */

static void
on_window_added (MetaWorkspace  *meta_workspace,
                 MetaWindow     *meta_win,
                 ShellWorkspace *self)
{
  if (!overview_closing (self))
    do_add_window (self, meta_win);
}

static void
on_window_removed (MetaWorkspace  *meta_workspace,
                   MetaWindow     *meta_win,
                   ShellWorkspace *self)
{
  do_remove_window (self, meta_win);
}

static void
on_workspace_active_changed (ShellWorkspace *self)
{
  workspace_layout_sync_overlays (self->layout);
}

static void
on_window_entered_monitor (MetaDisplay    *display,
                           int             monitor_index,
                           MetaWindow     *meta_win,
                           ShellWorkspace *self)
{
  if (monitor_index == self->monitor_index && !overview_closing (self))
    do_add_window (self, meta_win);
}

static void
on_window_left_monitor (MetaDisplay    *display,
                        int             monitor_index,
                        MetaWindow     *meta_win,
                        ShellWorkspace *self)
{
  if (monitor_index == self->monitor_index)
    do_remove_window (self, meta_win);
}

static void
on_background_clicked (ShellWorkspace *self)
{
  /* Switch to the workspace when not the active one, leave the
   * overview otherwise. */
  gboolean leave_overview = should_leave_overview (self);

  if (self->meta_workspace != NULL)
    {
      meta_workspace_activate (self->meta_workspace,
                               shell_global_get_current_time (shell_global_get ()));
    }

  if (leave_overview)
    g_signal_emit (self, signals[OVERVIEW_HIDE_REQUESTED], 0);
}

static void
on_overview_visible_changed (ShellWindowManager *window_manager,
                             GParamSpec         *pspec,
                             ShellWorkspace     *self)
{
  /* The overview emitted `hidden` */
  if (!shell_window_manager_get_overview_visible (window_manager))
    unfreeze_layout (self);
}

/**
 * shell_workspace_prepare_to_leave_overview:
 * @self: a #ShellWorkspace
 *
 * Called when the overview starts to hide: stops the animations of the
 * previews and freezes the layout until the overview is hidden
 * (`prepareToLeaveOverview ()`).
 */
void
shell_workspace_prepare_to_leave_overview (ShellWorkspace *self)
{
  guint i;

  g_return_if_fail (SHELL_IS_WORKSPACE (self));

  clear_skip_taskbar_signals (self);

  for (i = 0; i < self->windows->len; i++)
    clutter_actor_remove_all_transitions (g_ptr_array_index (self->windows, i));

  g_clear_handle_id (&self->layout_frozen_id, g_source_remove);

  freeze_layout (self);

  if (self->window_manager != NULL && self->overview_visible_id == 0)
    {
      self->overview_visible_id =
        g_signal_connect_object (self->window_manager, "notify::overview-visible",
                                 G_CALLBACK (on_overview_visible_changed), self, 0);
    }
}

/* ---- WorkspaceBackground ------------------------------------------------- */

static int
get_scale_factor (void)
{
  StThemeContext *context = st_theme_context_get_for_stage (get_stage ());

  return st_theme_context_get_scale_factor (context);
}

static void
background_update_border_radius (ShellWorkspace *self)
{
  ClutterActor *background_actor;
  ClutterContent *content;
  double corner_radius;

  if (self->bg_manager == NULL)
    return;

  background_actor = shell_background_manager_get_background_actor (self->bg_manager);
  if (background_actor == NULL)
    return;

  content = clutter_actor_get_content (background_actor);
  if (!META_IS_BACKGROUND_CONTENT (content))
    return;

  corner_radius = get_scale_factor () * BACKGROUND_CORNER_RADIUS_PIXELS;

  meta_background_content_set_rounded_clip_radius (META_BACKGROUND_CONTENT (content),
                                                   lerp (0, corner_radius,
                                                         st_adjustment_get_value (self->layout->state_adjustment)));
}

static void
background_update_rounded_clip_bounds (ShellWorkspace *self)
{
  g_autoptr (ShellMonitor) monitor = NULL;
  ClutterActor *background_actor;
  ClutterContent *content;
  graphene_rect_t rect;

  if (self->bg_manager == NULL)
    return;

  background_actor = shell_background_manager_get_background_actor (self->bg_manager);
  if (background_actor == NULL)
    return;

  content = clutter_actor_get_content (background_actor);
  if (!META_IS_BACKGROUND_CONTENT (content))
    return;

  monitor = shell_layout_manager_get_monitor (get_layout_manager (),
                                              self->monitor_index);
  if (monitor == NULL)
    return;

  graphene_rect_init (&rect,
                      self->bg_workarea.x - monitor->x,
                      self->bg_workarea.y - monitor->y,
                      self->bg_workarea.width,
                      self->bg_workarea.height);

  meta_background_content_set_rounded_clip_bounds (META_BACKGROUND_CONTENT (content),
                                                   &rect);
}

static void
background_state_adjustment_changed (ShellWorkspace *self)
{
  background_update_border_radius (self);
  if (self->background != NULL)
    clutter_actor_queue_relayout (self->background);
}

static void
background_manager_changed (ShellWorkspace *self)
{
  background_update_rounded_clip_bounds (self);
  background_update_border_radius (self);
}

static void
background_workareas_changed (ShellWorkspace *self)
{
  shell_layout_manager_get_work_area_for_monitor (get_layout_manager (),
                                                  self->monitor_index,
                                                  &self->bg_workarea);
  background_update_rounded_clip_bounds (self);
  if (self->background != NULL)
    clutter_actor_queue_relayout (self->background);
}

static ClutterActor *
background_create (ShellWorkspace *self)
{
  StAdjustment *state_adjustment = self->layout->state_adjustment;
  ClutterActor *background, *bin, *background_group;

  background = g_object_new (SHELL_TYPE_WORKSPACE_BACKGROUND,
                             "style-class", "workspace-background",
                             "x-expand", TRUE,
                             "y-expand", TRUE,
                             "monitor-index", self->monitor_index,
                             NULL);

  shell_layout_manager_get_work_area_for_monitor (get_layout_manager (),
                                                  self->monitor_index,
                                                  &self->bg_workarea);

  g_signal_connect_object (state_adjustment, "notify::value",
                           G_CALLBACK (background_state_adjustment_changed), self,
                           G_CONNECT_SWAPPED);
  g_object_bind_property (state_adjustment, "value",
                          background, "state-adjustment-value",
                          G_BINDING_SYNC_CREATE);

  bin = g_object_new (CLUTTER_TYPE_ACTOR,
                      "layout-manager", clutter_bin_layout_new (),
                      "clip-to-allocation", TRUE,
                      NULL);

  background_group = meta_background_group_new ();
  clutter_actor_set_layout_manager (background_group, clutter_bin_layout_new ());
  clutter_actor_set_x_expand (background_group, TRUE);
  clutter_actor_set_y_expand (background_group, TRUE);
  clutter_actor_add_child (bin, background_group);
  clutter_actor_add_child (background, bin);

  self->bg_manager = g_object_new (SHELL_TYPE_BACKGROUND_MANAGER,
                                   "container", background_group,
                                   "monitor-index", self->monitor_index,
                                   "control-position", FALSE,
                                   "use-content-size", FALSE,
                                   NULL);
  g_signal_connect_object (self->bg_manager, "changed",
                           G_CALLBACK (background_manager_changed), self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (get_display (), "workareas-changed",
                           G_CALLBACK (background_workareas_changed), self,
                           G_CONNECT_SWAPPED);
  background_update_rounded_clip_bounds (self);

  background_update_border_radius (self);

  return background;
}

/* ---- ShellDragTarget ------------------------------------------------------ */

/* The window that the dragged object stands for (`source.metaWindow`) */
static MetaWindow *
get_source_meta_window (GObject *source)
{
  GParamSpec *pspec;
  MetaWindow *window = NULL;

  if (source == NULL)
    return NULL;

  if (SHELL_IS_WINDOW_PREVIEW (source))
    return g_object_ref (shell_window_preview_get_meta_window (SHELL_WINDOW_PREVIEW (source)));

  pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (source), "meta-window");
  if (pspec == NULL || !g_type_is_a (pspec->value_type, META_TYPE_WINDOW) ||
      !(pspec->flags & G_PARAM_READABLE))
    return NULL;

  g_object_get (source, "meta-window", &window, NULL);
  return window;
}

/* The application of a dragged launcher (`source.app`) */
static ShellApp *
get_source_app (GObject *source)
{
  if (source == NULL || !SHELL_IS_DRAG_SOURCE (source))
    return NULL;

  return shell_drag_source_get_app (SHELL_DRAG_SOURCE (source));
}

static ShellDragMotionResult
shell_workspace_handle_drag_over (ShellDragTarget *target,
                                  GObject         *source,
                                  ClutterActor    *drag_actor,
                                  float            x,
                                  float            y,
                                  uint32_t         time)
{
  ShellWorkspace *self = SHELL_WORKSPACE (target);
  g_autoptr (MetaWindow) window = get_source_meta_window (source);
  ShellApp *app;

  if (window != NULL && !is_my_window (self, window))
    return SHELL_DRAG_MOTION_RESULT_MOVE_DROP;

  app = get_source_app (source);
  if (app != NULL && shell_app_can_open_new_window (app))
    return SHELL_DRAG_MOTION_RESULT_COPY_DROP;

  return SHELL_DRAG_MOTION_RESULT_CONTINUE;
}

typedef struct
{
  MetaWindow *window;
  int workspace_index;
  gulong id;
} MoveWindowData;

static void
move_window_data_free (MoveWindowData *data,
                       GClosure       *closure)
{
  g_object_unref (data->window);
  g_free (data);
}

static void
move_window_entered_monitor_cb (MetaDisplay    *display,
                                int             monitor_index,
                                MetaWindow     *window,
                                MoveWindowData *data)
{
  gulong id;

  if (window != data->window)
    return;

  meta_window_change_workspace_by_index (window, data->workspace_index, FALSE);

  /* frees data */
  id = data->id;
  g_signal_handler_disconnect (display, id);
}

/* Main.moveWindowToMonitorAndWorkspace () */
static void
move_window_to_monitor_and_workspace (MetaWindow *window,
                                      int         monitor_index,
                                      int         workspace_index)
{
  /* We need to move the window before changing the workspace, because
   * the move itself could cause a workspace change if the window enters
   * the primary monitor */
  if (meta_window_get_monitor (window) != monitor_index)
    {
      MoveWindowData *data = g_new0 (MoveWindowData, 1);

      /* Wait for the monitor change to take effect */
      data->window = g_object_ref (window);
      data->workspace_index = workspace_index;
      data->id = g_signal_connect_data (get_display (), "window-entered-monitor",
                                        G_CALLBACK (move_window_entered_monitor_cb),
                                        data,
                                        (GClosureNotify) move_window_data_free, 0);
      meta_window_move_to_monitor (window, monitor_index);
    }
  else
    {
      meta_window_change_workspace_by_index (window, workspace_index, FALSE);
    }
}

static gboolean
shell_workspace_accept_drop (ShellDragTarget *target,
                             GObject         *source,
                             ClutterActor    *drag_actor,
                             float            x,
                             float            y,
                             uint32_t         time)
{
  ShellWorkspace *self = SHELL_WORKSPACE (target);
  g_autoptr (MetaWindow) window = get_source_meta_window (source);
  MetaWorkspaceManager *workspace_manager =
    shell_global_get_workspace_manager (shell_global_get ());
  int workspace_index = self->meta_workspace != NULL
    ? meta_workspace_index (self->meta_workspace)
    : meta_workspace_manager_get_active_workspace_index (workspace_manager);
  ShellApp *app;

  if (window != NULL)
    {
      if (is_my_window (self, window))
        return FALSE;

      move_window_to_monitor_and_workspace (window, self->monitor_index,
                                            workspace_index);
      return TRUE;
    }

  app = get_source_app (source);
  if (app != NULL && shell_app_can_open_new_window (app))
    {
      shell_drag_source_animate_launch_at_pos (SHELL_DRAG_SOURCE (source),
                                               clutter_actor_get_x (drag_actor),
                                               clutter_actor_get_y (drag_actor));

      shell_app_open_new_window (app, workspace_index);
      return TRUE;
    }

  return FALSE;
}

static void
shell_workspace_drag_target_iface_init (ShellDragTargetInterface *iface)
{
  iface->handle_drag_over = shell_workspace_handle_drag_over;
  iface->accept_drop = shell_workspace_accept_drop;
}

/* ---- ClutterActor / StWidget ---------------------------------------------- */

static GList *
shell_workspace_get_focus_chain (StWidget *widget)
{
  ShellWorkspace *self = SHELL_WORKSPACE (widget);

  return workspace_layout_get_focus_chain (self->layout);
}

static void
shell_workspace_style_changed (StWidget *widget)
{
  ShellWorkspace *self = SHELL_WORKSPACE (widget);
  StThemeNode *theme_node = st_widget_get_theme_node (widget);

  ST_WIDGET_CLASS (shell_workspace_parent_class)->style_changed (widget);

  workspace_layout_set_spacing (self->layout,
                                st_theme_node_get_length (theme_node, "spacing"));
}

static void
shell_workspace_destroy (ClutterActor *actor)
{
  ShellWorkspace *self = SHELL_WORKSPACE (actor);

  self->destroyed = TRUE;

  clear_skip_taskbar_signals (self);

  g_clear_handle_id (&self->layout_frozen_id, g_source_remove);

  g_ptr_array_set_size (self->windows, 0);

  if (self->bg_manager != NULL)
    {
      shell_background_manager_destroy (self->bg_manager);
      g_clear_object (&self->bg_manager);
    }

  CLUTTER_ACTOR_CLASS (shell_workspace_parent_class)->destroy (actor);
}

/* ---- GObject ---------------------------------------------------------------- */

static void
shell_workspace_constructed (GObject *object)
{
  ShellWorkspace *self = SHELL_WORKSPACE (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellLayoutManager *layout_manager = get_layout_manager ();
  ClutterAction *click_gesture;
  GList *window_actors, *l;

  G_OBJECT_CLASS (shell_workspace_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (self), "window-picker");
  clutter_actor_set_pivot_point (actor, 0.5, 0.5);
  clutter_actor_set_layout_manager (actor, clutter_bin_layout_new ());

  /* the container owns the layout too, but the workspace uses it until it is
   * finalized */
  self->layout = g_object_ref_sink (workspace_layout_new (self->meta_workspace,
                                                          self->monitor_index,
                                                          self->overview_adjustment,
                                                          self->window_manager));

  /* Background */
  self->background = g_object_ref_sink (background_create (self));
  clutter_actor_add_child (actor, self->background);

  /* Window previews */
  self->container = g_object_ref_sink (clutter_actor_new ());
  clutter_actor_set_reactive (self->container, TRUE);
  clutter_actor_set_x_expand (self->container, TRUE);
  clutter_actor_set_y_expand (self->container, TRUE);
  clutter_actor_set_layout_manager (self->container,
                                    CLUTTER_LAYOUT_MANAGER (self->layout));
  clutter_actor_add_child (actor, self->container);

  if (layout_manager != NULL &&
      self->monitor_index != shell_layout_manager_get_primary_index (layout_manager))
    st_widget_add_style_class_name (ST_WIDGET (self), "external-monitor");

  click_gesture = clutter_click_gesture_new ();
  clutter_press_gesture_set_required_button (CLUTTER_PRESS_GESTURE (click_gesture),
                                             CLUTTER_BUTTON_PRIMARY);
  g_signal_connect_swapped (click_gesture, "recognize",
                            G_CALLBACK (on_background_clicked), self);
  g_object_bind_property (self, "mapped", click_gesture, "enabled",
                          G_BINDING_SYNC_CREATE);
  clutter_actor_add_action (self->container, click_gesture);

  /* Track window changes, but let the window tracker process them first */
  if (self->meta_workspace != NULL)
    {
      g_signal_connect_object (self->meta_workspace, "window-added",
                               G_CALLBACK (on_window_added), self, G_CONNECT_AFTER);
      g_signal_connect_object (self->meta_workspace, "window-removed",
                               G_CALLBACK (on_window_removed), self, G_CONNECT_AFTER);
      g_signal_connect_object (self->meta_workspace, "notify::active",
                               G_CALLBACK (on_workspace_active_changed), self,
                               G_CONNECT_SWAPPED);
    }

  g_signal_connect_object (get_display (), "window-entered-monitor",
                           G_CALLBACK (on_window_entered_monitor), self, G_CONNECT_AFTER);
  g_signal_connect_object (get_display (), "window-left-monitor",
                           G_CALLBACK (on_window_left_monitor), self, G_CONNECT_AFTER);

  /* Create clones for windows that should be
   * visible in the Overview */
  window_actors = shell_global_get_window_actors (shell_global_get ());
  for (l = window_actors; l != NULL; l = l->next)
    {
      MetaWindow *meta_window = meta_window_actor_get_meta_window (l->data);

      if (meta_window != NULL)
        do_add_window (self, meta_window);
    }
  g_list_free (window_actors);
}

static void
shell_workspace_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  ShellWorkspace *self = SHELL_WORKSPACE (object);

  switch (prop_id)
    {
    case PROP_META_WORKSPACE:
      g_value_set_object (value, self->meta_workspace);
      break;

    case PROP_MONITOR_INDEX:
      g_value_set_int (value, self->monitor_index);
      break;

    case PROP_OVERVIEW_ADJUSTMENT:
      g_value_set_object (value, self->overview_adjustment);
      break;

    case PROP_WINDOW_MANAGER:
      g_value_set_object (value, self->window_manager);
      break;

    case PROP_STATE_ADJUSTMENT:
      g_value_set_object (value, self->layout != NULL ? self->layout->state_adjustment : NULL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_workspace_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  ShellWorkspace *self = SHELL_WORKSPACE (object);

  switch (prop_id)
    {
    case PROP_META_WORKSPACE:
      g_set_object (&self->meta_workspace, g_value_get_object (value));
      break;

    case PROP_MONITOR_INDEX:
      self->monitor_index = g_value_get_int (value);
      break;

    case PROP_OVERVIEW_ADJUSTMENT:
      g_set_object (&self->overview_adjustment, g_value_get_object (value));
      break;

    case PROP_WINDOW_MANAGER:
      g_set_weak_pointer (&self->window_manager, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_workspace_dispose (GObject *object)
{
  ShellWorkspace *self = SHELL_WORKSPACE (object);

  g_clear_handle_id (&self->layout_frozen_id, g_source_remove);

  /* the arrays are used by destroy (), which the actor runs from its
   * dispose when it was not destroyed yet: they are freed in finalize () */
  g_clear_object (&self->bg_manager);
  g_clear_object (&self->background);
  g_clear_object (&self->container);
  g_clear_weak_pointer (&self->window_manager);
  g_clear_object (&self->overview_adjustment);
  g_clear_object (&self->meta_workspace);

  G_OBJECT_CLASS (shell_workspace_parent_class)->dispose (object);
}

static void
shell_workspace_finalize (GObject *object)
{
  ShellWorkspace *self = SHELL_WORKSPACE (object);

  g_clear_pointer (&self->skip_taskbar_signals, g_hash_table_unref);
  g_clear_pointer (&self->windows, g_ptr_array_unref);
  g_clear_object (&self->layout);

  G_OBJECT_CLASS (shell_workspace_parent_class)->finalize (object);
}

static void
shell_workspace_init (ShellWorkspace *self)
{
  self->skip_taskbar_signals = g_hash_table_new_full (NULL, NULL,
                                                      g_object_unref, NULL);
  self->windows = g_ptr_array_new_with_free_func (g_object_unref);
}

static void
shell_workspace_class_init (ShellWorkspaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);

  object_class->constructed = shell_workspace_constructed;
  object_class->dispose = shell_workspace_dispose;
  object_class->finalize = shell_workspace_finalize;
  object_class->get_property = shell_workspace_get_property;
  object_class->set_property = shell_workspace_set_property;

  actor_class->destroy = shell_workspace_destroy;

  widget_class->get_focus_chain = shell_workspace_get_focus_chain;
  widget_class->style_changed = shell_workspace_style_changed;

  /**
   * ShellWorkspace:meta-workspace:
   *
   * The workspace whose windows are shown (`metaWorkspace`), or %NULL for
   * the windows of all the workspaces (the view that shows one workspace on
   * the monitors that do not have a workspace strip).
   */
  props[PROP_META_WORKSPACE] =
    g_param_spec_object ("meta-workspace", NULL, NULL,
                         META_TYPE_WORKSPACE,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWorkspace:monitor-index:
   *
   * The monitor whose windows are shown (`monitorIndex`).
   */
  props[PROP_MONITOR_INDEX] =
    g_param_spec_int ("monitor-index", NULL, NULL,
                      0, G_MAXINT, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellWorkspace:overview-adjustment:
   *
   * The adjustment of the state of the overview.
   */
  props[PROP_OVERVIEW_ADJUSTMENT] =
    g_param_spec_object ("overview-adjustment", NULL, NULL,
                         ST_TYPE_ADJUSTMENT,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWorkspace:window-manager:
   *
   * The window manager (`Main.wm`), whose #ShellWindowManager:overview-closing,
   * #ShellWindowManager:overview-animating and
   * #ShellWindowManager:overview-visible properties tell the workspace what
   * the overview is doing. It is held weakly.
   */
  props[PROP_WINDOW_MANAGER] =
    g_param_spec_object ("window-manager", NULL, NULL,
                         SHELL_TYPE_WINDOW_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWorkspace:state-adjustment:
   *
   * An adjustment for controlling and transitioning between the alignment
   * of windows using the layout strategy and the floating window layout
   * (`stateAdjustment`). A value of 0 of the adjustment completely uses the
   * floating window layout while a value of 1 completely aligns windows
   * using the layout strategy.
   */
  props[PROP_STATE_ADJUSTMENT] =
    g_param_spec_object ("state-adjustment", NULL, NULL,
                         ST_TYPE_ADJUSTMENT,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellWorkspace::overview-hide-requested:
   * @self: the #ShellWorkspace
   *
   * Emitted when the workspace is clicked and the overview should be left
   * (`Main.overview.hide ()`).
   */
  signals[OVERVIEW_HIDE_REQUESTED] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellWorkspace::window-activated:
   * @self: the #ShellWorkspace
   * @window: the window that was activated
   * @time: the time of the activation
   *
   * Emitted after a preview was selected and its window activated: the
   * overview should be hidden and the calendar closed (the rest of
   * `Main.activateWindow ()`).
   */
  signals[WINDOW_ACTIVATED] =
    g_signal_new ("window-activated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  META_TYPE_WINDOW,
                  G_TYPE_UINT);

  /**
   * ShellWorkspace::window-drag-begin:
   * @self: the #ShellWorkspace
   * @window: the window whose preview starts to be dragged
   *
   * `Main.overview.beginWindowDrag ()`
   */
  signals[WINDOW_DRAG_BEGIN] =
    g_signal_new ("window-drag-begin",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  META_TYPE_WINDOW);

  /**
   * ShellWorkspace::window-drag-cancelled:
   * @self: the #ShellWorkspace
   * @window: the window whose drag was cancelled
   *
   * `Main.overview.cancelledWindowDrag ()`
   */
  signals[WINDOW_DRAG_CANCELLED] =
    g_signal_new ("window-drag-cancelled",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  META_TYPE_WINDOW);

  /**
   * ShellWorkspace::window-drag-end:
   * @self: the #ShellWorkspace
   * @window: the window whose drag ended
   *
   * `Main.overview.endWindowDrag ()`
   */
  signals[WINDOW_DRAG_END] =
    g_signal_new ("window-drag-end",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  META_TYPE_WINDOW);
}

/* ---- API ---------------------------------------------------------------------- */

/**
 * shell_workspace_new:
 * @meta_workspace: (nullable): the workspace, or %NULL for the windows of
 *   all workspaces
 * @monitor_index: the monitor whose windows are shown
 * @overview_adjustment: the adjustment of the state of the overview
 * @window_manager: (nullable): the window manager, which tells the workspace
 *   what the overview is doing
 *
 * Creates the window picker of @meta_workspace on a monitor.
 *
 * Returns: (transfer floating): a new #ShellWorkspace
 */
ShellWorkspace *
shell_workspace_new (MetaWorkspace      *meta_workspace,
                     int                 monitor_index,
                     StAdjustment       *overview_adjustment,
                     ShellWindowManager *window_manager)
{
  return g_object_new (SHELL_TYPE_WORKSPACE,
                       "meta-workspace", meta_workspace,
                       "monitor-index", monitor_index,
                       "overview-adjustment", overview_adjustment,
                       "window-manager", window_manager,
                       NULL);
}

/**
 * shell_workspace_get_meta_workspace:
 * @self: a #ShellWorkspace
 *
 * Returns: (transfer none) (nullable): the workspace whose windows are shown
 */
MetaWorkspace *
shell_workspace_get_meta_workspace (ShellWorkspace *self)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACE (self), NULL);

  return self->meta_workspace;
}

/**
 * shell_workspace_get_monitor_index:
 * @self: a #ShellWorkspace
 *
 * Returns: the monitor whose windows are shown
 */
int
shell_workspace_get_monitor_index (ShellWorkspace *self)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACE (self), -1);

  return self->monitor_index;
}

/**
 * shell_workspace_get_state_adjustment:
 * @self: a #ShellWorkspace
 *
 * Returns: (transfer none): the adjustment that transitions between the
 *   floating window layout (0) and the layout strategy (1)
 */
StAdjustment *
shell_workspace_get_state_adjustment (ShellWorkspace *self)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACE (self), NULL);

  return self->layout->state_adjustment;
}

/**
 * shell_workspace_contains_meta_window:
 * @self: a #ShellWorkspace
 * @meta_window: a window
 *
 * Returns: whether @self has a preview of @meta_window
 */
gboolean
shell_workspace_contains_meta_window (ShellWorkspace *self,
                                      MetaWindow     *meta_window)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACE (self), FALSE);

  return lookup_index (self, meta_window) >= 0;
}

/**
 * shell_workspace_is_empty:
 * @self: a #ShellWorkspace
 *
 * Returns: whether @self has no window previews
 */
gboolean
shell_workspace_is_empty (ShellWorkspace *self)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACE (self), TRUE);

  return self->windows->len == 0;
}

/**
 * shell_workspace_has_maximized_windows:
 * @self: a #ShellWorkspace
 *
 * Returns: whether a window of @self that is showing is maximized
 */
gboolean
shell_workspace_has_maximized_windows (ShellWorkspace *self)
{
  guint i;

  g_return_val_if_fail (SHELL_IS_WORKSPACE (self), FALSE);

  for (i = 0; i < self->windows->len; i++)
    {
      MetaWindow *meta_window =
        shell_window_preview_get_meta_window (g_ptr_array_index (self->windows, i));

      if (meta_window_showing_on_its_workspace (meta_window) &&
          meta_window_is_maximized (meta_window))
        return TRUE;
    }

  return FALSE;
}

/**
 * shell_workspace_sync_stacking:
 * @self: a #ShellWorkspace
 * @stable_sequences: (array length=n_stable_sequences): the stable sequences
 *   of the windows (`MetaWindow.get_stable_sequence ()`), from the bottom-most
 * @n_stable_sequences: the number of @stable_sequences
 *
 * Restacks the previews like the windows are stacked (`syncStacking ()`;
 * the JS took an object that mapped the stable sequences to their index).
 */
void
shell_workspace_sync_stacking (ShellWorkspace *self,
                               const guint32  *stable_sequences,
                               gsize           n_stable_sequences)
{
  g_return_if_fail (SHELL_IS_WORKSPACE (self));

  workspace_layout_sync_stacking (self->layout, stable_sequences,
                                  n_stable_sequences);
}
