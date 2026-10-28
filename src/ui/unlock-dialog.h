/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * unlock-dialog.h: The dialog of the lock screen: the clock, the
 * notifications, the authentication prompt and the buttons of the bottom.
 * Port of js/ui/unlockDialog.js.
 *
 * #ShellUnlockDialog is what the screen shield shows in the user session
 * (the `unlock-dialog` value of #ShellSessionMode:unlock-dialog).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "gdm/auth-prompt.h"
#include "misc/time-limits-manager.h"
#include "st.h"

G_BEGIN_DECLS

#define SHELL_TYPE_UNLOCK_DIALOG (shell_unlock_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellUnlockDialog, shell_unlock_dialog,
                      SHELL, UNLOCK_DIALOG, StWidget)

ShellUnlockDialog *shell_unlock_dialog_new       (ClutterActor           *parent_actor,
                                                  ShellTimeLimitsManager *time_limits_manager);

gboolean           shell_unlock_dialog_get_allow_cancel (ShellUnlockDialog *self);
void               shell_unlock_dialog_set_allow_cancel (ShellUnlockDialog *self,
                                                         gboolean           allow_cancel);

void               shell_unlock_dialog_cancel    (ShellUnlockDialog *self);
void               shell_unlock_dialog_finish    (ShellUnlockDialog          *self,
                                                  ShellAuthPromptFinishFunc   func,
                                                  gpointer                    user_data,
                                                  GDestroyNotify              destroy);
gboolean           shell_unlock_dialog_open      (ShellUnlockDialog *self);
void               shell_unlock_dialog_activate  (ShellUnlockDialog *self);
void               shell_unlock_dialog_pop_modal (ShellUnlockDialog *self);

G_END_DECLS
