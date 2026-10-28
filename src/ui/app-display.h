/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * app-display.h: The application grid of the overview, its folders and the
 * search provider of the installed applications. Port of js/ui/appDisplay.js.
 *
 * The types, with the name they had in the JS module:
 *
 *  - #ShellAppViewItem (`AppViewItem`, derivable, abstract): an icon in an
 *    application view. It is a drag source and a drop target.
 *  - #ShellAppIcon (`AppIcon`, derivable): the icon of an application.
 *    dash.c extends it with `DashIcon`.
 *  - #ShellFolderIcon (`FolderIcon`) and #ShellFolderView (`FolderView`):
 *    the icon of an application folder and the view of its applications.
 *  - #ShellAppFolderDialog (`AppFolderDialog`): the popup that shows a folder.
 *  - #ShellPageManager (`PageManager`): the `app-picker-layout` setting.
 *  - #ShellBaseAppView (`BaseAppView`, derivable, abstract): the paged view.
 *  - #ShellAppDisplay (`AppDisplay`): the application grid.
 *  - #ShellAppSearchProvider (`AppSearchProvider`): the applications provider
 *    of the search, which overviewControls.js hands to the search controller.
 *
 * `AppGrid`, `FolderGrid`, the layout of the page indicators and
 * `SystemActionIcon` are private to the C file.
 *
 * The overview and the other parts of the shell that are still JS are reached
 * through the #ShellWindowManager that every object gets with its
 * `window-manager` property: `Main.overview.visible`, `closing` and
 * `animationInProgress` are its `overview-visible`, `overview-closing` and
 * `overview-animating` properties and `Main.overview.hide ()` is its
 * `overview-hide-requested` signal. The drags of the items are told through
 * its `item-drag-begin`, `item-drag-cancelled` and `item-drag-end` signals
 * (see window-manager.h), which main.js hands to the overview.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>

#include "shell-app.h"
#include "st.h"
#include "ui/dnd.h"
#include "ui/icon-grid.h"
#include "ui/window-manager.h"

G_BEGIN_DECLS

typedef struct _ShellAppDisplay ShellAppDisplay;
typedef struct _ShellFolderView ShellFolderView;

/* ------------------------------------------------------------------------ */
/* ShellAppViewItem                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_APP_VIEW_ITEM (shell_app_view_item_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellAppViewItem, shell_app_view_item,
                          SHELL, APP_VIEW_ITEM, StButton)

/**
 * ShellAppViewItemClass:
 * @parent_class: the parent class
 * @can_accept: whether @source can be dropped on the item (`_canAccept ()`).
 *   The default accepts everything but the item itself
 * @set_hovering_by_dnd: the dragged item is over the item or left it
 *   (`_setHoveringByDnd ()`)
 * @handle_drag_over: the implementation of shell_drag_target_handle_drag_over ()
 *   (`handleDragOver ()`)
 * @accept_drop: the implementation of shell_drag_target_accept_drop ()
 *   (`acceptDrop ()`)
 * @scale_and_fade: hides the item while it is dragged (`scaleAndFade ()`).
 *   The dash icons do nothing
 * @undo_scale_and_fade: shows the item again (`undoScaleAndFade ()`)
 * @drag_motion: called by the drag monitor which is installed while another
 *   item is over this one (`_onDragMotion ()`)
 */
struct _ShellAppViewItemClass
{
  StButtonClass parent_class;

  gboolean              (* can_accept)         (ShellAppViewItem *self,
                                                GObject          *source);
  void                  (* set_hovering_by_dnd) (ShellAppViewItem *self,
                                                 gboolean          hovering);
  ShellDragMotionResult (* handle_drag_over)   (ShellAppViewItem *self,
                                                GObject          *source,
                                                ClutterActor     *actor,
                                                float             x,
                                                float             y,
                                                uint32_t          time);
  gboolean              (* accept_drop)        (ShellAppViewItem *self,
                                                GObject          *source,
                                                ClutterActor     *actor,
                                                float             x,
                                                float             y,
                                                uint32_t          time);
  void                  (* scale_and_fade)     (ShellAppViewItem *self);
  void                  (* undo_scale_and_fade) (ShellAppViewItem *self);
  ShellDragMotionResult (* drag_motion)        (ShellAppViewItem *self,
                                                ShellDragEvent   *event);

  /*< private >*/
  gpointer padding[8];
};

const char         *shell_app_view_item_get_id             (ShellAppViewItem *self);
void                shell_app_view_item_set_id             (ShellAppViewItem *self,
                                                            const char       *id);
const char         *shell_app_view_item_get_name           (ShellAppViewItem *self);
void                shell_app_view_item_set_name           (ShellAppViewItem *self,
                                                            const char       *name);
ShellBaseIcon      *shell_app_view_item_get_icon           (ShellAppViewItem *self);
void                shell_app_view_item_set_icon           (ShellAppViewItem *self,
                                                            ShellBaseIcon    *icon);
ShellWindowManager *shell_app_view_item_get_window_manager (ShellAppViewItem *self);
int                 shell_app_view_item_get_drag_icon_size (ShellAppViewItem *self);
void                shell_app_view_item_set_drag_icon_size (ShellAppViewItem *self,
                                                            int               size);
gboolean            shell_app_view_item_get_dragging       (ShellAppViewItem *self);
void                shell_app_view_item_scale_in           (ShellAppViewItem *self);
void                shell_app_view_item_scale_and_fade     (ShellAppViewItem *self);
void                shell_app_view_item_undo_scale_and_fade (ShellAppViewItem *self);
void                shell_app_view_item_set_forced_highlight (ShellAppViewItem *self,
                                                              gboolean          highlighted);

/* ------------------------------------------------------------------------ */
/* ShellAppIcon                                                              */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_APP_ICON (shell_app_icon_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellAppIcon, shell_app_icon,
                          SHELL, APP_ICON, ShellAppViewItem)

/**
 * ShellAppIconClass:
 * @parent_class: the parent class
 */
struct _ShellAppIconClass
{
  ShellAppViewItemClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

ShellAppIcon *shell_app_icon_new                  (ShellApp           *app,
                                                   ShellWindowManager *window_manager);
ShellApp     *shell_app_icon_get_app              (ShellAppIcon *self);
void          shell_app_icon_popup_menu           (ShellAppIcon *self);
void          shell_app_icon_activate             (ShellAppIcon *self,
                                                   uint32_t      button);
void          shell_app_icon_animate_launch       (ShellAppIcon *self);
void          shell_app_icon_animate_launch_at_pos (ShellAppIcon *self,
                                                    float         x,
                                                    float         y);
gboolean      shell_app_icon_should_show_tooltip  (ShellAppIcon *self);

/* ------------------------------------------------------------------------ */
/* ShellFolderIcon, ShellAppFolderDialog                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_FOLDER_ICON (shell_folder_icon_get_type ())
G_DECLARE_FINAL_TYPE (ShellFolderIcon, shell_folder_icon,
                      SHELL, FOLDER_ICON, ShellAppViewItem)

#define SHELL_TYPE_APP_FOLDER_DIALOG (shell_app_folder_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellAppFolderDialog, shell_app_folder_dialog,
                      SHELL, APP_FOLDER_DIALOG, StBin)

ShellFolderIcon *shell_folder_icon_new         (const char         *folder_id,
                                                const char         *path,
                                                ShellAppDisplay    *parent_view,
                                                ShellWindowManager *window_manager);
ShellFolderView *shell_folder_icon_get_view    (ShellFolderIcon *self);
void             shell_folder_icon_open        (ShellFolderIcon *self);
char           **shell_folder_icon_get_app_ids (ShellFolderIcon *self);

void shell_app_folder_dialog_toggle  (ShellAppFolderDialog *self);
void shell_app_folder_dialog_popup   (ShellAppFolderDialog *self);

/**
 * ShellAppFolderDialogPopdownFunc:
 * @user_data: (closure): the data given to shell_app_folder_dialog_popdown ()
 *
 * Called when the dialog is gone.
 */
typedef void (* ShellAppFolderDialogPopdownFunc) (gpointer user_data);

void shell_app_folder_dialog_popdown (ShellAppFolderDialog         *self,
                                      ShellAppFolderDialogPopdownFunc callback,
                                      gpointer                      user_data,
                                      GDestroyNotify                destroy);

/* ------------------------------------------------------------------------ */
/* ShellPageManager                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_PAGE_MANAGER (shell_page_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellPageManager, shell_page_manager,
                      SHELL, PAGE_MANAGER, GObject)

ShellPageManager *shell_page_manager_new              (GSettings        *settings);
void              shell_page_manager_get_app_position (ShellPageManager *self,
                                                       const char       *app_id,
                                                       int              *page,
                                                       int              *position);
void              shell_page_manager_set_pages        (ShellPageManager *self,
                                                       GVariant         *pages);
GVariant         *shell_page_manager_get_pages        (ShellPageManager *self);

/* ------------------------------------------------------------------------ */
/* ShellBaseAppView                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_BASE_APP_VIEW (shell_base_app_view_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellBaseAppView, shell_base_app_view,
                          SHELL, BASE_APP_VIEW, StWidget)

/**
 * ShellBaseAppViewClass:
 * @parent_class: the parent class
 * @create_grid: makes the grid of the view (`_createGrid ()`). It has to be
 *   the private application grid, which the default implementation makes
 * @load_apps: makes the items of the view (`_loadApps ()`). The items are
 *   those which exist already or new ones; the view adds the new ones
 * @compare_items: orders two items (`_compareItems ()`)
 * @get_item_position: where an item should be, -1 for page and position if
 *   it has none (`_getItemPosition ()`)
 * @can_accept: whether the item @source can be dropped on the view
 *   (`_canAccept ()`)
 * @drag_begin: a drag of an item of the overview started (`_onDragBegin ()`)
 * @drag_motion: the dragged item moved (`_onDragMotion ()`)
 * @drag_end: the drag ended (`_onDragEnd ()`)
 * @drag_cancelled: the drag was cancelled (`_onDragCancelled ()`)
 * @accept_drop: an item was dropped on the view (`acceptDrop ()`)
 * @maybe_move_item: moves the dragged item after a delay (`_maybeMoveItem ()`)
 * @go_to_page: shows a page (`goToPage ()`)
 * @redisplay: loads the items again (`_redisplay ()`)
 * @can_scroll: whether the view reacts to the scroll wheel (the application
 *   grid does not while a folder is open)
 *
 * The base class of the paged views of application icons.
 */
struct _ShellBaseAppViewClass
{
  StWidgetClass parent_class;

  ShellIconGrid *       (* create_grid)       (ShellBaseAppView *self);
  GPtrArray *           (* load_apps)         (ShellBaseAppView *self);
  int                   (* compare_items)     (ShellBaseAppView *self,
                                               ShellAppViewItem *a,
                                               ShellAppViewItem *b);
  void                  (* get_item_position) (ShellBaseAppView *self,
                                               ShellAppViewItem *item,
                                               int              *page,
                                               int              *position);
  gboolean              (* can_accept)        (ShellBaseAppView *self,
                                               GObject          *source);
  void                  (* drag_begin)        (ShellBaseAppView *self,
                                               GObject          *source);
  ShellDragMotionResult (* drag_motion)       (ShellBaseAppView *self,
                                               ShellDragEvent   *event);
  void                  (* drag_end)          (ShellBaseAppView *self);
  void                  (* drag_cancelled)    (ShellBaseAppView *self,
                                               GObject          *source);
  gboolean              (* accept_drop)       (ShellBaseAppView *self,
                                               GObject          *source);
  void                  (* maybe_move_item)   (ShellBaseAppView *self,
                                               ShellDragEvent   *event);
  void                  (* go_to_page)        (ShellBaseAppView *self,
                                               int               page,
                                               gboolean          animate);
  void                  (* redisplay)         (ShellBaseAppView *self);
  gboolean              (* can_scroll)        (ShellBaseAppView *self);

  /*< private >*/
  gpointer padding[8];
};

ShellIconGrid *shell_base_app_view_get_grid         (ShellBaseAppView *self);
StScrollView  *shell_base_app_view_get_scroll_view  (ShellBaseAppView *self);
ShellWindowManager *shell_base_app_view_get_window_manager (ShellBaseAppView *self);
int            shell_base_app_view_get_drag_icon_size (ShellBaseAppView *self);
void           shell_base_app_view_set_drag_icon_size (ShellBaseAppView *self,
                                                       int               size);
GPtrArray     *shell_base_app_view_get_all_items    (ShellBaseAppView *self);
void           shell_base_app_view_go_to_page       (ShellBaseAppView *self,
                                                     int               page,
                                                     gboolean          animate);
void           shell_base_app_view_select_app       (ShellBaseAppView *self,
                                                     const char       *id);
void           shell_base_app_view_redisplay        (ShellBaseAppView *self);
void           shell_base_app_view_drag_begin       (ShellBaseAppView *self,
                                                     GObject          *source);
void           shell_base_app_view_drag_end         (ShellBaseAppView *self);
void           shell_base_app_view_drag_cancelled   (ShellBaseAppView *self,
                                                     GObject          *source);

/* ------------------------------------------------------------------------ */
/* ShellFolderView                                                           */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_FOLDER_VIEW (shell_folder_view_get_type ())
G_DECLARE_FINAL_TYPE (ShellFolderView, shell_folder_view,
                      SHELL, FOLDER_VIEW, ShellBaseAppView)

ShellFolderView *shell_folder_view_new               (GSettings          *folder,
                                                      const char         *id,
                                                      ShellAppDisplay    *parent_view,
                                                      ShellWindowManager *window_manager);
ClutterActor    *shell_folder_view_create_folder_icon (ShellFolderView *self,
                                                       int              size);
void             shell_folder_view_add_app           (ShellFolderView *self,
                                                      ShellApp        *app);
void             shell_folder_view_remove_app        (ShellFolderView *self,
                                                      ShellApp        *app);
gboolean         shell_folder_view_get_deleting_folder (ShellFolderView *self);

/* ------------------------------------------------------------------------ */
/* ShellAppDisplay                                                           */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_APP_DISPLAY (shell_app_display_get_type ())
G_DECLARE_FINAL_TYPE (ShellAppDisplay, shell_app_display,
                      SHELL, APP_DISPLAY, ShellBaseAppView)

ShellAppDisplay *shell_app_display_new                (ShellWindowManager *window_manager);
void             shell_app_display_add_folder_dialog  (ShellAppDisplay       *self,
                                                       ShellAppFolderDialog  *dialog);
GList           *shell_app_display_get_app_infos      (ShellAppDisplay *self);
gboolean         shell_app_display_create_folder      (ShellAppDisplay    *self,
                                                       const char * const *apps);

/* ------------------------------------------------------------------------ */
/* ShellAppSearchProvider                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_APP_SEARCH_PROVIDER (shell_app_search_provider_get_type ())
G_DECLARE_FINAL_TYPE (ShellAppSearchProvider, shell_app_search_provider,
                      SHELL, APP_SEARCH_PROVIDER, GObject)

ShellAppSearchProvider *shell_app_search_provider_new (ShellWindowManager *window_manager);

G_END_DECLS
