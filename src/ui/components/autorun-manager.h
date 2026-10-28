/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * autorun-manager.h: Runs the application for the content of the media that
 * are mounted, or asks the user what to do with them in a notification. Port
 * of js/ui/components/autorunManager.js.
 *
 * The component manager of the shell (js/ui/components.js) creates it for
 * the session modes that list `autorunManager` and calls shell_autorun_manager_enable ()
 * and shell_autorun_manager_disable () when the session mode changes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

#include "ui/message-tray.h"

G_BEGIN_DECLS

#define SHELL_TYPE_AUTORUN_MANAGER (shell_autorun_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellAutorunManager, shell_autorun_manager,
                      SHELL, AUTORUN_MANAGER, GObject)

ShellAutorunManager *shell_autorun_manager_new     (ShellMessageTray *message_tray);

void                 shell_autorun_manager_enable  (ShellAutorunManager *self);
void                 shell_autorun_manager_disable (ShellAutorunManager *self);

G_END_DECLS
