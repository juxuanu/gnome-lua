/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * close-dialog.h: The dialog which offers to force quit an application which
 * does not respond to the ping of the window manager. Port of
 * js/ui/closeDialog.js.
 */

#pragma once

#include <glib-object.h>
#include <meta/meta-close-dialog.h>
#include <meta/window.h>

G_BEGIN_DECLS

#define SHELL_TYPE_CLOSE_DIALOG (shell_close_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellCloseDialog, shell_close_dialog,
                      SHELL, CLOSE_DIALOG, GObject)

ShellCloseDialog *shell_close_dialog_new (MetaWindow *window);

G_END_DECLS
