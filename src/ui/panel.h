/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * panel.h: The top bar: three boxes of indicators (the "status area"), the
 * menu manager that lets one of their menus be open at a time, and the
 * dragging of maximized windows by their title area. Port of js/ui/panel.js.
 *
 * #ShellPanel is created by main.js (`new Shell.Panel ()`), which then
 * connects #ShellPanel::create-indicator and calls shell_panel_begin_startup ().
 * The indicators of the shell itself (the activities button, the date menu,
 * the quick settings, ...) are still JS classes; they live in
 * js/ui/panelItems.js, and that module's createPanelItem () answers the
 * signal. They extend #ShellPanelMenuButton (see panel-menu.h).
 *
 * Hooks declared here: "panel.indicator-added" and "panel.menu-opened"
 * (docs/api/hooks.md).
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>

#include "st.h"
#include "ui/panel-menu.h"
#include "ui/popup-menu.h"

G_BEGIN_DECLS

/**
 * ShellPanelBox:
 * @SHELL_PANEL_BOX_LEFT: the box at the start of the top bar (`'left'`)
 * @SHELL_PANEL_BOX_CENTER: the box in the middle (`'center'`)
 * @SHELL_PANEL_BOX_RIGHT: the box at the end of the top bar (`'right'`)
 *
 * One of the three boxes of the top bar. They keep their place in right to
 * left locales: "left" is the start of the bar.
 */
typedef enum
{
  SHELL_PANEL_BOX_LEFT,
  SHELL_PANEL_BOX_CENTER,
  SHELL_PANEL_BOX_RIGHT,
} ShellPanelBox;

#define SHELL_TYPE_PANEL (shell_panel_get_type ())
G_DECLARE_FINAL_TYPE (ShellPanel, shell_panel, SHELL, PANEL, StWidget)

void                 shell_panel_begin_startup              (ShellPanel           *self);

gboolean             shell_panel_add_to_status_area         (ShellPanel           *self,
                                                             const char           *role,
                                                             ShellPanelMenuButton *indicator,
                                                             int                   position,
                                                             ShellPanelBox         box,
                                                             GError              **error);
gboolean             shell_panel_add_to_status_area_with_owner (ShellPanel           *self,
                                                                const char           *role,
                                                                ShellPanelMenuButton *indicator,
                                                                int                   position,
                                                                ShellPanelBox         box,
                                                                const char           *owner,
                                                                GError              **error);
ShellPanelButtonBox *shell_panel_get_status_area_indicator  (ShellPanel           *self,
                                                             const char           *role);
char               **shell_panel_list_status_area_roles     (ShellPanel           *self);
gboolean             shell_panel_is_builtin_role            (const char           *role);

ShellPopupMenuManager *shell_panel_get_menu_manager         (ShellPanel           *self);
StBoxLayout         *shell_panel_get_left_box               (ShellPanel           *self);
StBoxLayout         *shell_panel_get_center_box             (ShellPanel           *self);
StBoxLayout         *shell_panel_get_right_box              (ShellPanel           *self);

void                 shell_panel_toggle_calendar            (ShellPanel           *self);
void                 shell_panel_toggle_quick_settings      (ShellPanel           *self);
void                 shell_panel_close_calendar             (ShellPanel           *self);
void                 shell_panel_close_quick_settings       (ShellPanel           *self);

gboolean             shell_panel_get_overview_visible       (ShellPanel           *self);
void                 shell_panel_set_overview_visible       (ShellPanel           *self,
                                                             gboolean              visible);
guint                shell_panel_get_box_opacity            (ShellPanel           *self);
void                 shell_panel_set_box_opacity            (ShellPanel           *self,
                                                             guint                 opacity);
ClutterActorAlign    shell_panel_get_banner_alignment       (ShellPanel           *self);
gboolean             shell_panel_get_banner_blocked         (ShellPanel           *self);

G_END_DECLS
