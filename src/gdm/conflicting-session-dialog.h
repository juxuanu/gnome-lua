/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * conflicting-session-dialog.h: The dialog which tells the user that a session
 * is running already, and offers to force stop it. Port of
 * js/gdm/conflictingSessionDialog.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "login1-dbus.h"
#include "ui/modal-dialog.h"

G_BEGIN_DECLS

#define SHELL_TYPE_CONFLICTING_SESSION_DIALOG (shell_conflicting_session_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellConflictingSessionDialog, shell_conflicting_session_dialog,
                      SHELL, CONFLICTING_SESSION_DIALOG, ShellModalDialog)

ShellConflictingSessionDialog *shell_conflicting_session_dialog_new (ShellLogin1Session *conflicting_session,
                                                                     ShellLogin1Session *greeter_session);

G_END_DECLS
