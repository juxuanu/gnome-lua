/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * welcome-dialog.h: The dialog which welcomes the user after the first login
 * and offers GNOME Tour. Port of js/ui/welcomeDialog.js.
 *
 * #ShellWelcomeDialog is a #ShellModalDialog. Opening it does nothing, and
 * returns %FALSE, if GNOME Tour is not installed. The tour hides the overview
 * when it is launched: the dialog emits ::overview-hide-requested, which
 * main.js connects to `Main.overview.hide ()`.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "ui/modal-dialog.h"

G_BEGIN_DECLS

#define SHELL_TYPE_WELCOME_DIALOG (shell_welcome_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellWelcomeDialog, shell_welcome_dialog,
                      SHELL, WELCOME_DIALOG, ShellModalDialog)

ShellWelcomeDialog *shell_welcome_dialog_new (void);

G_END_DECLS
