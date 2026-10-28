/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * login-manager.h: An abstraction over systemd/logind. Port of
 * js/misc/loginManager.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "login1-dbus.h"

G_BEGIN_DECLS


#define SHELL_TYPE_LOGIN_MANAGER (shell_login_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellLoginManager, shell_login_manager,
                      SHELL, LOGIN_MANAGER, GObject)

ShellLoginManager *shell_login_manager_get_default (void);

gboolean shell_login_manager_can_lock (void);

void     shell_login_manager_register_session_with_gdm (void);
void     shell_login_manager_register_display_with_gdm (void);

gboolean shell_login_manager_get_preparing_for_sleep (ShellLoginManager *self);

void                 shell_login_manager_get_current_user_async  (ShellLoginManager   *self,
                                                                  GCancellable        *cancellable,
                                                                  GAsyncReadyCallback  callback,
                                                                  gpointer             user_data);
ShellLogin1User     *shell_login_manager_get_current_user_finish (ShellLoginManager   *self,
                                                                  GAsyncResult        *res,
                                                                  GError             **error);

void                 shell_login_manager_get_current_session_async  (ShellLoginManager   *self,
                                                                     GCancellable        *cancellable,
                                                                     GAsyncReadyCallback  callback,
                                                                     gpointer             user_data);
ShellLogin1Session  *shell_login_manager_get_current_session_finish (ShellLoginManager   *self,
                                                                     GAsyncResult        *res,
                                                                     GError             **error);

void     shell_login_manager_can_reboot_to_boot_loader_menu_async  (ShellLoginManager   *self,
                                                                    GCancellable        *cancellable,
                                                                    GAsyncReadyCallback  callback,
                                                                    gpointer             user_data);
gboolean shell_login_manager_can_reboot_to_boot_loader_menu_finish (ShellLoginManager   *self,
                                                                    GAsyncResult        *res,
                                                                    gboolean            *needs_auth);

void     shell_login_manager_set_reboot_to_boot_loader_menu (ShellLoginManager *self);

void      shell_login_manager_list_sessions_async  (ShellLoginManager   *self,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
GVariant *shell_login_manager_list_sessions_finish (ShellLoginManager   *self,
                                                    GAsyncResult        *res);

ShellLogin1Session *shell_login_manager_get_session (ShellLoginManager  *self,
                                                     const char         *object_path,
                                                     GError            **error);

void          shell_login_manager_inhibit_async  (ShellLoginManager   *self,
                                                  const char          *reason,
                                                  GCancellable        *cancellable,
                                                  GAsyncReadyCallback  callback,
                                                  gpointer             user_data);
GInputStream *shell_login_manager_inhibit_finish (ShellLoginManager   *self,
                                                  GAsyncResult        *res,
                                                  GError             **error);

G_END_DECLS
