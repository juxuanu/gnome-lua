/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * window-menu.h: The menu which opens when the window menu of a window is
 * requested (right click on a titlebar, Alt+Space), and the manager which
 * shows it. Port of js/ui/windowMenu.js.
 */

#pragma once

#include <clutter/clutter.h>
#include <meta/meta-enums.h>
#include <meta/window.h>

#include "ui/popup-menu.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellWindowMenu                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WINDOW_MENU (shell_window_menu_get_type ())
G_DECLARE_FINAL_TYPE (ShellWindowMenu, shell_window_menu,
                      SHELL, WINDOW_MENU, ShellPopupMenu)

ShellWindowMenu *shell_window_menu_new        (MetaWindow   *window,
                                               ClutterActor *source_actor);

MetaWindow      *shell_window_menu_get_window (ShellWindowMenu *self);

/* ------------------------------------------------------------------------ */
/* ShellWindowMenuManager                                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WINDOW_MENU_MANAGER (shell_window_menu_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellWindowMenuManager, shell_window_menu_manager,
                      SHELL, WINDOW_MENU_MANAGER, GObject)

ShellWindowMenuManager *shell_window_menu_manager_new (void);

void                    shell_window_menu_manager_show_window_menu_for_window (ShellWindowMenuManager *self,
                                                                               MetaWindow             *window,
                                                                               MetaWindowMenuType      type,
                                                                               const MtkRectangle     *rect);

G_END_DECLS
