/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * panel-menu.h: The widgets which sit in the top bar: the button box that
 * every top bar item is made of, the button with a popup menu, and the system
 * indicator that groups status icons and quick settings items. Port of
 * js/ui/panelMenu.js, plus `SystemIndicator`, which lived in
 * js/ui/quickSettings.js but is used by the panel and by every status module.
 *
 * All three classes are derivable: the activities button, the date menu, the
 * quick settings button, the accessibility, keyboard and dwell click
 * indicators and every status indicator are still JS classes registered with
 * GObject.registerClass () that extend them. What `PanelMenu.Button (
 * menuAlignment, nameText, dontCreateMenu)` took as arguments are construct
 * properties; what a subclass overrides is a ClutterActor or StWidget vfunc
 * (`vfunc_scroll_event`, `vfunc_hide`, ...).
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>

#include "st.h"
#include "ui/box-pointer.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellPanelButtonBox                                                      */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_PANEL_BUTTON_BOX (shell_panel_button_box_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellPanelButtonBox, shell_panel_button_box,
                          SHELL, PANEL_BUTTON_BOX, StWidget)

/**
 * ShellPanelButtonBoxClass:
 * @parent_class: the parent class
 */
struct _ShellPanelButtonBoxClass
{
  StWidgetClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

StBin *shell_panel_button_box_get_container (ShellPanelButtonBox *self);

/* ------------------------------------------------------------------------ */
/* ShellPanelMenuButton                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_PANEL_MENU_BUTTON (shell_panel_menu_button_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellPanelMenuButton, shell_panel_menu_button,
                          SHELL, PANEL_MENU_BUTTON, ShellPanelButtonBox)

/**
 * ShellPanelMenuButtonClass:
 * @parent_class: the parent class
 */
struct _ShellPanelMenuButtonClass
{
  ShellPanelButtonBoxClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

GObject *shell_panel_menu_button_get_menu            (ShellPanelMenuButton *self);
void     shell_panel_menu_button_set_menu            (ShellPanelMenuButton *self,
                                                      GObject              *menu);
void     shell_panel_menu_button_set_sensitive       (ShellPanelMenuButton *self,
                                                      gboolean              sensitive);
gboolean shell_panel_menu_button_get_menu_is_open    (ShellPanelMenuButton *self);
void     shell_panel_menu_button_toggle_menu         (ShellPanelMenuButton *self);
void     shell_panel_menu_button_close_menu          (ShellPanelMenuButton *self,
                                                      ShellPopupAnimation   animate);

/* ------------------------------------------------------------------------ */
/* ShellSystemIndicator                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SYSTEM_INDICATOR (shell_system_indicator_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellSystemIndicator, shell_system_indicator,
                          SHELL, SYSTEM_INDICATOR, StBoxLayout)

/**
 * ShellSystemIndicatorClass:
 * @parent_class: the parent class
 */
struct _ShellSystemIndicatorClass
{
  StBoxLayoutClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

StIcon     *shell_system_indicator_add_indicator            (ShellSystemIndicator *self);
void        shell_system_indicator_sync_indicators_visible  (ShellSystemIndicator *self);

void        shell_system_indicator_add_quick_settings_item  (ShellSystemIndicator *self,
                                                             ClutterActor         *item);
GPtrArray  *shell_system_indicator_get_quick_settings_items (ShellSystemIndicator *self);

G_END_DECLS
