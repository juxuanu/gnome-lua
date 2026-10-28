/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dash.h: The dash of the overview: the favorite and the running
 * applications, and the button that shows the application grid. Port of
 * js/ui/dash.js.
 *
 * The types, with the name they had in the JS module:
 *
 *  - #ShellDashIcon (`DashIcon`): the icon of an application in the dash.
 *  - #ShellDashItemContainer (`DashItemContainer`, derivable): the container
 *    of an item of the dash, which animates in and out and has the label that
 *    is shown above it.
 *  - #ShellShowAppsIcon (`ShowAppsIcon`): the item with the button that shows
 *    the application grid, and the drop target that unpins applications.
 *  - #ShellDash (`Dash`): the dash itself.
 *
 * The drag placeholder, the empty drop target, the layout of the icons and
 * the drop target of the box of the icons are private to the C file.
 *
 * What the JS reached through `Main.overview` is reached through the
 * #ShellWindowManager that the dash is created with (see app-display.h); the
 * drags of the items it listens to itself (#ShellWindowManager::item-drag-begin
 * and its siblings), the drags of windows are told with
 * shell_dash_window_drag_begin () and shell_dash_window_drag_end (), which
 * overviewControls.js calls from the overview signals.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>

#include "shell-app.h"
#include "st.h"
#include "ui/app-display.h"
#include "ui/dnd.h"
#include "ui/window-manager.h"

G_BEGIN_DECLS

/**
 * SHELL_DASH_ANIMATION_TIME:
 *
 * Time in milliseconds of the animations of the items of the dash
 * (`DASH_ANIMATION_TIME`).
 */
#define SHELL_DASH_ANIMATION_TIME 200

/* ------------------------------------------------------------------------ */
/* ShellDashIcon                                                             */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_DASH_ICON (shell_dash_icon_get_type ())
G_DECLARE_FINAL_TYPE (ShellDashIcon, shell_dash_icon,
                      SHELL, DASH_ICON, ShellAppIcon)

ShellDashIcon *shell_dash_icon_new (ShellApp           *app,
                                    ShellWindowManager *window_manager);

/* ------------------------------------------------------------------------ */
/* ShellDashItemContainer                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_DASH_ITEM_CONTAINER (shell_dash_item_container_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellDashItemContainer, shell_dash_item_container,
                          SHELL, DASH_ITEM_CONTAINER, StWidget)

/**
 * ShellDashItemContainerClass:
 * @parent_class: the parent class
 *
 * A container like #StBin, but taking the scale of the child into account
 * when requesting a size.
 */
struct _ShellDashItemContainerClass
{
  StWidgetClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

ShellDashItemContainer *shell_dash_item_container_new               (void);
void                    shell_dash_item_container_set_child         (ShellDashItemContainer *self,
                                                                     ClutterActor           *child);
ClutterActor           *shell_dash_item_container_get_child         (ShellDashItemContainer *self);
StLabel                *shell_dash_item_container_get_label         (ShellDashItemContainer *self);
void                    shell_dash_item_container_set_label_text    (ShellDashItemContainer *self,
                                                                     const char             *text);
const char             *shell_dash_item_container_get_label_text    (ShellDashItemContainer *self);
void                    shell_dash_item_container_show_label        (ShellDashItemContainer *self);
void                    shell_dash_item_container_hide_label        (ShellDashItemContainer *self);
void                    shell_dash_item_container_show              (ShellDashItemContainer *self,
                                                                     gboolean                animate);
void                    shell_dash_item_container_animate_out_and_destroy (ShellDashItemContainer *self);
gboolean                shell_dash_item_container_get_animating_out (ShellDashItemContainer *self);

/* ------------------------------------------------------------------------ */
/* ShellShowAppsIcon                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SHOW_APPS_ICON (shell_show_apps_icon_get_type ())
G_DECLARE_FINAL_TYPE (ShellShowAppsIcon, shell_show_apps_icon,
                      SHELL, SHOW_APPS_ICON, ShellDashItemContainer)

ShellShowAppsIcon *shell_show_apps_icon_new               (void);
StButton          *shell_show_apps_icon_get_toggle_button (ShellShowAppsIcon *self);
ShellBaseIcon     *shell_show_apps_icon_get_icon          (ShellShowAppsIcon *self);
void               shell_show_apps_icon_set_drag_app      (ShellShowAppsIcon *self,
                                                           ShellApp          *app);

/* ------------------------------------------------------------------------ */
/* ShellDash                                                                 */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_DASH (shell_dash_get_type ())
G_DECLARE_FINAL_TYPE (ShellDash, shell_dash, SHELL, DASH, StWidget)

ShellDash    *shell_dash_new                 (ShellWindowManager *window_manager);
ShellApp     *shell_dash_get_app_from_source (GObject            *source);
int           shell_dash_get_icon_size       (ShellDash *self);
StButton     *shell_dash_get_show_apps_button (ShellDash *self);
StBoxLayout  *shell_dash_get_dash_container  (ShellDash *self);
ClutterActor *shell_dash_get_box             (ShellDash *self);
void          shell_dash_set_max_size        (ShellDash *self,
                                              int        max_width,
                                              int        max_height);
void          shell_dash_window_drag_begin   (ShellDash *self);
void          shell_dash_window_drag_end     (ShellDash *self);

G_END_DECLS
