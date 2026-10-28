/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * alt-tab.h: The popups of the window switching shortcuts: the application
 * switcher (Alt+Tab) with the thumbnails of the windows of an application,
 * the window switcher, and the window and group cyclers that have no popup,
 * only a highlight on the window. Port of js/ui/altTab.js.
 *
 * The four popups are subclasses of #ShellSwitcherPopup. window-manager.c
 * creates one for the key binding that was pressed, connects the signals
 * below and calls shell_switcher_popup_show_switcher () on it. The lists and
 * the items of the popups are private to alt-tab.c.
 */

#pragma once

#include <meta/workspace.h>

#include "ui/switcher-popup.h"

G_BEGIN_DECLS

/**
 * ShellAltTabAppIconMode:
 * @SHELL_ALT_TAB_APP_ICON_MODE_THUMBNAIL_ONLY: the items of the window
 *   switcher show a thumbnail of the window
 * @SHELL_ALT_TAB_APP_ICON_MODE_APP_ICON_ONLY: the items show the icon of the
 *   application of the window
 * @SHELL_ALT_TAB_APP_ICON_MODE_BOTH: the items show a thumbnail of the window
 *   with a small icon of its application on top
 *
 * How the window switcher shows a window. Same values as the
 * `org.gnome.shell.window-switcher.AppIconMode` enumeration of the
 * `app-icon-mode` setting and as `AppIconMode` of altTab.js.
 */
typedef enum
{
  SHELL_ALT_TAB_APP_ICON_MODE_THUMBNAIL_ONLY = 1,
  SHELL_ALT_TAB_APP_ICON_MODE_APP_ICON_ONLY = 2,
  SHELL_ALT_TAB_APP_ICON_MODE_BOTH = 3,
} ShellAltTabAppIconMode;

GList *shell_alt_tab_get_windows (MetaWorkspace *workspace);

/* ------------------------------------------------------------------------ */
/* ShellAppSwitcherPopup                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_APP_SWITCHER_POPUP (shell_app_switcher_popup_get_type ())
G_DECLARE_FINAL_TYPE (ShellAppSwitcherPopup, shell_app_switcher_popup,
                      SHELL, APP_SWITCHER_POPUP, ShellSwitcherPopup)

ShellAppSwitcherPopup *shell_app_switcher_popup_new (void);

/* ------------------------------------------------------------------------ */
/* ShellWindowSwitcherPopup                                                  */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WINDOW_SWITCHER_POPUP (shell_window_switcher_popup_get_type ())
G_DECLARE_FINAL_TYPE (ShellWindowSwitcherPopup, shell_window_switcher_popup,
                      SHELL, WINDOW_SWITCHER_POPUP, ShellSwitcherPopup)

ShellWindowSwitcherPopup *shell_window_switcher_popup_new (void);

/* ------------------------------------------------------------------------ */
/* ShellCyclerPopup, ShellWindowCyclerPopup and ShellGroupCyclerPopup        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_CYCLER_POPUP (shell_cycler_popup_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellCyclerPopup, shell_cycler_popup,
                          SHELL, CYCLER_POPUP, ShellSwitcherPopup)

/**
 * ShellCyclerPopupClass:
 * @parent_class: the parent class
 *
 * The class of the popups that do not show a list but highlight the window
 * that is selected. The windows are set by the subclass.
 */
struct _ShellCyclerPopupClass
{
  ShellSwitcherPopupClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

#define SHELL_TYPE_WINDOW_CYCLER_POPUP (shell_window_cycler_popup_get_type ())
G_DECLARE_FINAL_TYPE (ShellWindowCyclerPopup, shell_window_cycler_popup,
                      SHELL, WINDOW_CYCLER_POPUP, ShellCyclerPopup)

ShellWindowCyclerPopup *shell_window_cycler_popup_new (void);

#define SHELL_TYPE_GROUP_CYCLER_POPUP (shell_group_cycler_popup_get_type ())
G_DECLARE_FINAL_TYPE (ShellGroupCyclerPopup, shell_group_cycler_popup,
                      SHELL, GROUP_CYCLER_POPUP, ShellCyclerPopup)

ShellGroupCyclerPopup *shell_group_cycler_popup_new (void);

G_END_DECLS
