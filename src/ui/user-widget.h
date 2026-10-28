/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * user-widget.h: A widget showing the avatar and the name of a user. Port of
 * js/ui/userWidget.js.
 *
 * Adapted from gdm/gui/user-switch-applet/applet.c
 *
 * Copyright (C) 2004-2005 James M. Cape <jcape@ignore-your.tv>.
 * Copyright (C) 2008,2009 Red Hat, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <act/act.h>

#include "st.h"

G_BEGIN_DECLS

#define SHELL_USER_AVATAR_DEFAULT_ICON_SIZE 64

/* ------------------------------------------------------------------------ */
/* ShellUserAvatar                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_USER_AVATAR (shell_user_avatar_get_type ())
G_DECLARE_FINAL_TYPE (ShellUserAvatar, shell_user_avatar, SHELL, USER_AVATAR, StBin)

ShellUserAvatar *shell_user_avatar_new           (ActUser         *user,
                                                  int              icon_size);

ActUser         *shell_user_avatar_get_user      (ShellUserAvatar *self);
int              shell_user_avatar_get_icon_size (ShellUserAvatar *self);

void             shell_user_avatar_set_sensitive (ShellUserAvatar *self,
                                                  gboolean         sensitive);
void             shell_user_avatar_update        (ShellUserAvatar *self);

/* ------------------------------------------------------------------------ */
/* ShellUserWidget                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_USER_WIDGET (shell_user_widget_get_type ())
G_DECLARE_FINAL_TYPE (ShellUserWidget, shell_user_widget, SHELL, USER_WIDGET, StBoxLayout)

ShellUserWidget *shell_user_widget_new         (ActUser            *user,
                                                ClutterOrientation  orientation);

ActUser         *shell_user_widget_get_user    (ShellUserWidget *self);

void             shell_user_widget_hide_avatar (ShellUserWidget *self);
void             shell_user_widget_show_avatar (ShellUserWidget *self);

G_END_DECLS
