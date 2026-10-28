/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * end-session-dialog.h: The dialog which gnome-session shows to confirm a
 * logout, power off or restart. Port of js/ui/endSessionDialog.js.
 *
 * Copyright 2010-2016 Red Hat, Inc
 *
 * #ShellEndSessionDialog exports org.gnome.SessionManager.EndSessionDialog at
 * /org/gnome/SessionManager/EndSessionDialog of the session bus (a
 * gdbus-codegen skeleton, #ShellSessionManagerEndSessionDialog): gnome-session
 * calls Open () and Close (), the dialog answers with the Canceled,
 * Confirmed* and Closed signals.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "ui/modal-dialog.h"

G_BEGIN_DECLS

#define SHELL_TYPE_END_SESSION_DIALOG (shell_end_session_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellEndSessionDialog, shell_end_session_dialog,
                      SHELL, END_SESSION_DIALOG, ShellModalDialog)

ShellEndSessionDialog *shell_end_session_dialog_new (void);

void shell_end_session_dialog_cancel (ShellEndSessionDialog *self);

G_END_DECLS
