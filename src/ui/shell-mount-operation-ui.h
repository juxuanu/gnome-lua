/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-mount-operation-ui.h: The dialogs of the mount operations: the ones
 * which ask for a password or a choice, list the processes that block an
 * unmount, and the notification that shows the progress of an unmount. Port
 * of js/ui/shellMountOperation.js (the C backend, #ShellMountOperation, is in
 * shell-mount-operation.h).
 *
 * #ShellMountOperationUI is what `new ShellMountOperation (source, params)` of
 * the JS module was: it owns a #ShellMountOperation and shows the dialogs
 * that answer its signals. #ShellMountOpHandler exports
 * org.Gtk.MountOperationHandler, with which other processes (gvfs clients
 * such as Files) show the same dialogs in the shell. The dialogs themselves
 * (`ShellMountQuestionDialog`, `ShellMountPasswordDialog` and
 * `ShellProcessesDialog`) are private to shell-mount-operation-ui.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "ui/modal-dialog.h"

G_BEGIN_DECLS

#define SHELL_TYPE_MOUNT_OPERATION_UI (shell_mount_operation_ui_get_type ())
G_DECLARE_FINAL_TYPE (ShellMountOperationUI, shell_mount_operation_ui,
                      SHELL, MOUNT_OPERATION_UI, GObject)

ShellMountOperationUI *shell_mount_operation_ui_new           (GVolume          *source,
                                                               ShellModalDialog *existing_dialog);

GMountOperation       *shell_mount_operation_ui_get_mount_op  (ShellMountOperationUI *self);

void                   shell_mount_operation_ui_close         (ShellMountOperationUI *self);
ShellModalDialog      *shell_mount_operation_ui_borrow_dialog (ShellMountOperationUI *self);

#define SHELL_TYPE_MOUNT_OP_HANDLER (shell_mount_op_handler_get_type ())
G_DECLARE_FINAL_TYPE (ShellMountOpHandler, shell_mount_op_handler,
                      SHELL, MOUNT_OP_HANDLER, GObject)

ShellMountOpHandler   *shell_mount_op_handler_new (void);

G_END_DECLS
