/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * background-menu.h: The menu which opens on the desktop background, with the
 * entries which open the background, display and general settings. Port of
 * js/ui/backgroundMenu.js.
 */

#pragma once

#include <clutter/clutter.h>

#include "ui/layout.h"
#include "ui/popup-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_BACKGROUND_MENU (shell_background_menu_get_type ())
G_DECLARE_FINAL_TYPE (ShellBackgroundMenu, shell_background_menu,
                      SHELL, BACKGROUND_MENU, ShellPopupMenu)

ShellBackgroundMenu *shell_background_menu_new    (ShellLayoutManager *layout_manager);

void                 shell_background_menu_add_to (ClutterActor       *actor,
                                                   ShellLayoutManager *layout_manager);

G_END_DECLS
