/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * run-dialog.h: The "Run a Command" dialog (Alt+F2). Port of
 * js/ui/runDialog.js.
 */

#pragma once

#include "st.h"

#include "ui/modal-dialog.h"

G_BEGIN_DECLS

#define SHELL_TYPE_RUN_DIALOG (shell_run_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellRunDialog, shell_run_dialog,
                      SHELL, RUN_DIALOG, ShellModalDialog)

ShellRunDialog *shell_run_dialog_new (void);

G_END_DECLS
