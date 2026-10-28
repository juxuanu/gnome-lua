/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * ctrl-alt-tab.h: The keyboard navigation between the "focus groups" of the
 * shell (top bar, dash, overview, ...) and the windows of the workspace, for
 * users that cannot use the pointer. Port of js/ui/ctrlAltTab.js.
 *
 * #ShellCtrlAltTabManager is created by main.js (`new Shell.CtrlAltTabManager ()`
 * and kept in `Main.ctrlAltTabManager`). The popup it shows and the list in
 * the popup are private to ctrl-alt-tab.c; they are subclasses of
 * #ShellSwitcherPopup and #ShellSwitcherList.
 */

#pragma once

#include <clutter/clutter.h>
#include <meta/window.h>

G_BEGIN_DECLS

/**
 * ShellCtrlAltTabSortGroup:
 * @SHELL_CTRL_ALT_TAB_SORT_GROUP_TOP: shown first (the top bar and the
 *   windows)
 * @SHELL_CTRL_ALT_TAB_SORT_GROUP_MIDDLE: shown after the top group, sorted by
 *   the position of the proxy actor on screen
 * @SHELL_CTRL_ALT_TAB_SORT_GROUP_BOTTOM: shown last
 *
 * Where a group is shown in the popup. Same values as `CtrlAltTab.SortGroup`.
 */
typedef enum
{
  SHELL_CTRL_ALT_TAB_SORT_GROUP_TOP = 0,
  SHELL_CTRL_ALT_TAB_SORT_GROUP_MIDDLE = 1,
  SHELL_CTRL_ALT_TAB_SORT_GROUP_BOTTOM = 2,
} ShellCtrlAltTabSortGroup;

/**
 * ShellCtrlAltTabFocusCallback:
 * @timestamp: the time of the event that selected the group
 * @user_data: (closure): the user data given along with the callback
 *
 * Called to give the key focus to a group, instead of navigating into its
 * root actor.
 */
typedef void (* ShellCtrlAltTabFocusCallback) (uint32_t timestamp,
                                               gpointer user_data);

#define SHELL_TYPE_CTRL_ALT_TAB_MANAGER (shell_ctrl_alt_tab_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellCtrlAltTabManager, shell_ctrl_alt_tab_manager,
                      SHELL, CTRL_ALT_TAB_MANAGER, GObject)

ShellCtrlAltTabManager *shell_ctrl_alt_tab_manager_new         (void);

void     shell_ctrl_alt_tab_manager_add_group                  (ShellCtrlAltTabManager       *self,
                                                                ClutterActor                 *root,
                                                                const char                   *name,
                                                                const char                   *icon_name);
void     shell_ctrl_alt_tab_manager_add_group_full             (ShellCtrlAltTabManager       *self,
                                                                ClutterActor                 *root,
                                                                const char                   *name,
                                                                const char                   *icon_name,
                                                                ShellCtrlAltTabSortGroup      sort_group,
                                                                ClutterActor                 *proxy,
                                                                ShellCtrlAltTabFocusCallback  focus_callback,
                                                                gpointer                      user_data,
                                                                GDestroyNotify                destroy);
void     shell_ctrl_alt_tab_manager_remove_group               (ShellCtrlAltTabManager       *self,
                                                                ClutterActor                 *root);

void     shell_ctrl_alt_tab_manager_popup                      (ShellCtrlAltTabManager       *self,
                                                                gboolean                      backward,
                                                                const char                   *binding,
                                                                uint32_t                      mask,
                                                                gboolean                      overview_visible);

G_END_DECLS
