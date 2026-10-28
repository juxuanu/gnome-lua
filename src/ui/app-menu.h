/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * app-menu.h: The context menu of an application icon of the app grid and
 * the dash: new window, the desktop actions of the application, its open
 * windows, pin to the dash, details and quit. Port of js/ui/appMenu.js.
 */

#pragma once

#include "st.h"

#include "shell-app.h"
#include "ui/popup-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_APP_MENU (shell_app_menu_get_type ())
G_DECLARE_FINAL_TYPE (ShellAppMenu, shell_app_menu, SHELL, APP_MENU, ShellPopupMenu)

ShellAppMenu *shell_app_menu_new                    (ClutterActor *source_actor,
                                                     StSide        side,
                                                     gboolean      favorites_section,
                                                     gboolean      show_single_windows);

ShellApp     *shell_app_menu_get_app                (ShellAppMenu *self);
void          shell_app_menu_set_app                (ShellAppMenu *self,
                                                     ShellApp     *app);

gboolean      shell_app_menu_get_favorites_section  (ShellAppMenu *self);
gboolean      shell_app_menu_get_show_single_windows (ShellAppMenu *self);

void          shell_app_menu_update_windows_section (ShellAppMenu *self);

G_END_DECLS
