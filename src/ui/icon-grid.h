/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * icon-grid.h: The paged grid of icons of the application grid, the folders
 * and the search results, and the icon its items show. Port of
 * js/ui/iconGrid.js.
 *
 * #ShellBaseIcon is the icon (an icon actor and an optional label),
 * #ShellIconGridLayout the layout manager which distributes items over pages
 * and #ShellIconGrid the viewport that holds the items. #ShellBaseIcon and
 * #ShellIconGrid are derivable: app-display.c extends #ShellIconGrid
 * (`AppGrid`).
 *
 * The icon of a #ShellBaseIcon is made by the `create_icon` virtual function,
 * whose default implementation calls the function set with
 * shell_base_icon_set_create_icon_func (). The JS callers of the old
 * `createIcon` parameter use the function, subclasses can override
 * `vfunc_create_icon ()`.
 *
 * The items of an icon grid are actors with a #ShellBaseIcon among their
 * descendants (the old `item.icon` property); the grid sets the size of that
 * icon.
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

/**
 * SHELL_ICON_GRID_APPICON_ANIMATION_OUT_SCALE:
 *
 * Scale an icon reaches while zooming out when it is launched
 * (`APPICON_ANIMATION_OUT_SCALE`).
 */
#define SHELL_ICON_GRID_APPICON_ANIMATION_OUT_SCALE 3

/**
 * SHELL_ICON_GRID_APPICON_ANIMATION_OUT_TIME:
 *
 * Duration in milliseconds of the zoom out of a launched icon
 * (`APPICON_ANIMATION_OUT_TIME`).
 */
#define SHELL_ICON_GRID_APPICON_ANIMATION_OUT_TIME 250

/**
 * ShellIconGridDragLocation:
 * @SHELL_ICON_GRID_DRAG_LOCATION_INVALID: outside the grid
 * @SHELL_ICON_GRID_DRAG_LOCATION_START_EDGE: on the start edge of an icon
 * @SHELL_ICON_GRID_DRAG_LOCATION_ON_ICON: on an icon
 * @SHELL_ICON_GRID_DRAG_LOCATION_END_EDGE: on the end edge of an icon
 * @SHELL_ICON_GRID_DRAG_LOCATION_EMPTY_SPACE: in the empty space of a page
 *
 * Where a drag is relative to the icons of a grid (`DragLocation`).
 */
typedef enum
{
  SHELL_ICON_GRID_DRAG_LOCATION_INVALID = 0,
  SHELL_ICON_GRID_DRAG_LOCATION_START_EDGE = 1,
  SHELL_ICON_GRID_DRAG_LOCATION_ON_ICON = 2,
  SHELL_ICON_GRID_DRAG_LOCATION_END_EDGE = 3,
  SHELL_ICON_GRID_DRAG_LOCATION_EMPTY_SPACE = 4,
} ShellIconGridDragLocation;

/* ------------------------------------------------------------------------ */
/* ShellBaseIcon                                                             */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_BASE_ICON (shell_base_icon_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellBaseIcon, shell_base_icon,
                          SHELL, BASE_ICON, StBin)

/**
 * ShellBaseIconClass:
 * @parent_class: the parent class
 * @create_icon: makes the icon actor for a size in logical pixels
 *   (`createIcon ()`). The default calls the function of
 *   shell_base_icon_set_create_icon_func () and returns %NULL, with a
 *   warning, if there is none. The caller owns the returned actor, which may
 *   be floating. Call it with shell_base_icon_create_icon ()
 */
struct _ShellBaseIconClass
{
  StBinClass parent_class;

  ClutterActor * (* create_icon) (ShellBaseIcon *self,
                                  int            size);

  /*< private >*/
  gpointer padding[8];
};

/**
 * ShellBaseIconCreateIconFunc:
 * @size: the size of the icon in logical pixels
 * @user_data: (closure): the data given to
 *   shell_base_icon_set_create_icon_func ()
 *
 * Makes the icon actor of @icon.
 *
 * Returns: (transfer full): the icon actor
 */
typedef ClutterActor * (* ShellBaseIconCreateIconFunc) (int      size,
                                                       gpointer user_data);

ClutterActor *shell_base_icon_create_icon          (ShellBaseIcon *self,
                                                    int            size);
void          shell_base_icon_set_create_icon_func (ShellBaseIcon                *self,
                                                    ShellBaseIconCreateIconFunc   func,
                                                    gpointer                      user_data,
                                                    GDestroyNotify                destroy);
gboolean      shell_base_icon_set_icon_size        (ShellBaseIcon  *self,
                                                    int             size,
                                                    GError        **error);
int           shell_base_icon_get_icon_size        (ShellBaseIcon *self);
ClutterActor *shell_base_icon_get_icon             (ShellBaseIcon *self);
StLabel      *shell_base_icon_get_label            (ShellBaseIcon *self);
void          shell_base_icon_update               (ShellBaseIcon *self);
void          shell_base_icon_animate_zoom_out     (ShellBaseIcon *self);
void          shell_base_icon_animate_zoom_out_at_pos (ShellBaseIcon *self,
                                                       float          x,
                                                       float          y);

void          shell_icon_grid_zoom_out_actor        (ClutterActor *actor);
void          shell_icon_grid_zoom_out_actor_at_pos (ClutterActor *actor,
                                                     float         x,
                                                     float         y);

/* ------------------------------------------------------------------------ */
/* ShellIconGridLayout                                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_ICON_GRID_LAYOUT (shell_icon_grid_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellIconGridLayout, shell_icon_grid_layout,
                      SHELL, ICON_GRID_LAYOUT, ClutterLayoutManager)

gboolean      shell_icon_grid_layout_add_item         (ShellIconGridLayout *self,
                                                       ClutterActor        *item,
                                                       int                  page,
                                                       int                  index,
                                                       GError             **error);
gboolean      shell_icon_grid_layout_append_item      (ShellIconGridLayout *self,
                                                       ClutterActor        *item,
                                                       GError             **error);
gboolean      shell_icon_grid_layout_move_item        (ShellIconGridLayout *self,
                                                       ClutterActor        *item,
                                                       int                  new_page,
                                                       int                  new_position,
                                                       GError             **error);
gboolean      shell_icon_grid_layout_remove_item      (ShellIconGridLayout *self,
                                                       ClutterActor        *item,
                                                       GError             **error);
GList        *shell_icon_grid_layout_get_items_at_page (ShellIconGridLayout *self,
                                                        int                  page,
                                                        GError             **error);
void          shell_icon_grid_layout_get_item_position (ShellIconGridLayout *self,
                                                        ClutterActor        *item,
                                                        int                 *page,
                                                        int                 *position);
ClutterActor *shell_icon_grid_layout_get_item_at      (ShellIconGridLayout *self,
                                                       int                  page,
                                                       int                  position);
int           shell_icon_grid_layout_get_item_page    (ShellIconGridLayout *self,
                                                       ClutterActor        *item);
void          shell_icon_grid_layout_adapt_to_size    (ShellIconGridLayout *self,
                                                       float                page_width,
                                                       float                page_height);
void          shell_icon_grid_layout_get_drop_target  (ShellIconGridLayout       *self,
                                                       double                     x,
                                                       double                     y,
                                                       int                       *page,
                                                       int                       *position,
                                                       ShellIconGridDragLocation *location);

/* ------------------------------------------------------------------------ */
/* ShellIconGrid                                                             */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_ICON_GRID (shell_icon_grid_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellIconGrid, shell_icon_grid,
                          SHELL, ICON_GRID, StViewport)

/**
 * ShellIconGridClass:
 * @parent_class: the parent class
 */
struct _ShellIconGridClass
{
  StViewportClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

gboolean      shell_icon_grid_add_item         (ShellIconGrid  *self,
                                                ClutterActor   *item,
                                                int             page,
                                                int             index,
                                                GError        **error);
gboolean      shell_icon_grid_append_item      (ShellIconGrid  *self,
                                                ClutterActor   *item,
                                                GError        **error);
gboolean      shell_icon_grid_move_item        (ShellIconGrid  *self,
                                                ClutterActor   *item,
                                                int             new_page,
                                                int             new_position,
                                                GError        **error);
gboolean      shell_icon_grid_remove_item      (ShellIconGrid  *self,
                                                ClutterActor   *item,
                                                GError        **error);
gboolean      shell_icon_grid_go_to_page       (ShellIconGrid  *self,
                                                int             page,
                                                gboolean        animate,
                                                GError        **error);
int           shell_icon_grid_get_item_page    (ShellIconGrid  *self,
                                                ClutterActor   *item);
void          shell_icon_grid_get_item_position (ShellIconGrid *self,
                                                 ClutterActor  *item,
                                                 int           *page,
                                                 int           *position);
ClutterActor *shell_icon_grid_get_item_at      (ShellIconGrid  *self,
                                                int             page,
                                                int             position);
GList        *shell_icon_grid_get_items_at_page (ShellIconGrid  *self,
                                                 int             page,
                                                 GError        **error);
void          shell_icon_grid_get_drop_target  (ShellIconGrid   *self,
                                                double           x,
                                                double           y,
                                                int             *page,
                                                int             *position,
                                                ShellIconGridDragLocation *location);
void          shell_icon_grid_set_grid_modes   (ShellIconGrid   *self,
                                                GVariant        *modes);
int           shell_icon_grid_get_current_page (ShellIconGrid   *self);
int           shell_icon_grid_get_n_pages      (ShellIconGrid   *self);
int           shell_icon_grid_get_items_per_page (ShellIconGrid *self);

G_END_DECLS
