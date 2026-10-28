/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * web-login.h: A widget showing a URL (and a QR code of it) for web login,
 * and the dialog around it. Port of js/gdm/webLogin.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

#define SHELL_WEB_LOGIN_QR_CODE_SIZE 150
#define SHELL_WEB_LOGIN_SPINNER_SIZE 35
#define SHELL_WEB_LOGIN_URL_LABEL_LONG_THRESHOLD 45

/* ------------------------------------------------------------------------ */
/* ShellWebLoginPrompt                                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WEB_LOGIN_PROMPT (shell_web_login_prompt_get_type ())
G_DECLARE_FINAL_TYPE (ShellWebLoginPrompt, shell_web_login_prompt,
                      SHELL, WEB_LOGIN_PROMPT, StBoxLayout)

ShellWebLoginPrompt *shell_web_login_prompt_new    (int         qr_size,
                                                    const char *message,
                                                    const char *url,
                                                    const char *code);
void                 shell_web_login_prompt_update (ShellWebLoginPrompt *self,
                                                    const char          *message,
                                                    const char          *url,
                                                    const char          *code);

/* ------------------------------------------------------------------------ */
/* ShellWebLoginDialog                                                       */
/* ------------------------------------------------------------------------ */

/**
 * ShellWebLoginButtonFunc:
 * @user_data: (closure): the data given to shell_web_login_dialog_update ()
 *
 * Called when the user clicks the button which says they are done.
 */
typedef void (* ShellWebLoginButtonFunc) (gpointer user_data);

#define SHELL_TYPE_WEB_LOGIN_DIALOG (shell_web_login_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellWebLoginDialog, shell_web_login_dialog,
                      SHELL, WEB_LOGIN_DIALOG, StWidget)

ShellWebLoginDialog *shell_web_login_dialog_new         (void);

void                 shell_web_login_dialog_update      (ShellWebLoginDialog     *self,
                                                         const char              *message,
                                                         const char              *url,
                                                         const char              *code,
                                                         const char              *done_label,
                                                         ShellWebLoginButtonFunc  done_func,
                                                         gpointer                 user_data,
                                                         GDestroyNotify           destroy);
void                 shell_web_login_dialog_stop_loading (ShellWebLoginDialog *self);
gboolean             shell_web_login_dialog_get_is_loading (ShellWebLoginDialog *self);

G_END_DECLS
