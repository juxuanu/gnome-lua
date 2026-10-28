/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * login-dialog.h: The login screen: the list of users, the prompt, the
 * banner, the logo and the buttons of the bottom. Port of
 * js/gdm/loginDialog.js.
 *
 * #ShellLoginDialog is what the screen shield shows in the gdm session mode
 * (the `login-dialog` value of #ShellSessionMode:unlock-dialog).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "gdm/auth-prompt.h"
#include "st.h"

G_BEGIN_DECLS

#define SHELL_TYPE_LOGIN_DIALOG (shell_login_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellLoginDialog, shell_login_dialog,
                      SHELL, LOGIN_DIALOG, StWidget)

ShellLoginDialog *shell_login_dialog_new      (ClutterActor *parent_actor);

void              shell_login_dialog_activate (ShellLoginDialog *self);
gboolean          shell_login_dialog_open     (ShellLoginDialog *self);
void              shell_login_dialog_close    (ShellLoginDialog *self);
void              shell_login_dialog_cancel   (ShellLoginDialog *self);
void              shell_login_dialog_finish   (ShellLoginDialog          *self,
                                               ShellAuthPromptFinishFunc  func,
                                               gpointer                   user_data,
                                               GDestroyNotify             destroy);

G_END_DECLS
