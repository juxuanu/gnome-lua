/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * inhibit-shortcuts-dialog.h: The dialog which asks whether an application
 * may inhibit the keyboard shortcuts of the shell. Port of
 * js/ui/inhibitShortcutsDialog.js.
 */

#pragma once

#include <glib-object.h>
#include <meta/meta-inhibit-shortcuts-dialog.h>
#include <meta/window.h>

G_BEGIN_DECLS

#define SHELL_TYPE_INHIBIT_SHORTCUTS_DIALOG (shell_inhibit_shortcuts_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellInhibitShortcutsDialog, shell_inhibit_shortcuts_dialog,
                      SHELL, INHIBIT_SHORTCUTS_DIALOG, GObject)

ShellInhibitShortcutsDialog *shell_inhibit_shortcuts_dialog_new (MetaWindow *window);

G_END_DECLS
