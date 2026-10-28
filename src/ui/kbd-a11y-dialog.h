/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * kbd-a11y-dialog.h: The dialog which tells that Slow Keys or Sticky Keys
 * were turned on or off with a keyboard shortcut, and asks whether to keep
 * the change. Port of js/ui/kbdA11yDialog.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SHELL_TYPE_KBD_A11Y_DIALOG (shell_kbd_a11y_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellKbdA11yDialog, shell_kbd_a11y_dialog,
                      SHELL, KBD_A11Y_DIALOG, GObject)

ShellKbdA11yDialog *shell_kbd_a11y_dialog_new (void);

G_END_DECLS
