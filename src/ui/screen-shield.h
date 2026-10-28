/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * screen-shield.h: The lock screen: it locks and unlocks the session, fades
 * the screen when the session goes idle, shows the dialog which asks the user
 * to authenticate and exports org.gnome.ScreenSaver. Port of
 * js/ui/screenShield.js.
 *
 * #ShellScreenShield is created by main.js (`new Shell.ScreenShield ()`, kept
 * in `Main.screenShield`, only when the session can lock). The dialog it
 * shows is chosen by #ShellSessionMode:unlock-dialog: the login screen
 * (js/gdm/loginDialog.js, `login-dialog`) in the gdm session mode, the unlock
 * dialog (js/ui/unlockDialog.js, `unlock-dialog`) otherwise.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

#include "gdm/credential-manager.h"
#include "misc/time-limits-manager.h"

G_BEGIN_DECLS

#define SHELL_TYPE_SCREEN_SHIELD (shell_screen_shield_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenShield, shell_screen_shield,
                      SHELL, SCREEN_SHIELD, GObject)

ShellScreenShield *shell_screen_shield_new                 (void);

gboolean           shell_screen_shield_get_locked          (ShellScreenShield *self);
gboolean           shell_screen_shield_get_active          (ShellScreenShield *self);
gint64             shell_screen_shield_get_activation_time (ShellScreenShield *self);

ShellTimeLimitsManager *shell_screen_shield_get_time_limits_manager (ShellScreenShield *self);
void               shell_screen_shield_set_time_limits_manager (ShellScreenShield       *self,
                                                                ShellTimeLimitsManager  *time_limits_manager);

void               shell_screen_shield_lock                (ShellScreenShield *self,
                                                            gboolean           animate);
void               shell_screen_shield_activate            (ShellScreenShield *self,
                                                            gboolean           animate);
void               shell_screen_shield_deactivate          (ShellScreenShield *self,
                                                            gboolean           animate);
void               shell_screen_shield_show_dialog         (ShellScreenShield *self);
void               shell_screen_shield_lock_if_was_locked  (ShellScreenShield *self);

void               shell_screen_shield_add_credential_manager    (ShellScreenShield      *self,
                                                                  const char             *service_name,
                                                                  ShellCredentialManager *credential_manager);
void               shell_screen_shield_remove_credential_manager (ShellScreenShield      *self,
                                                                  const char             *service_name);

G_END_DECLS
