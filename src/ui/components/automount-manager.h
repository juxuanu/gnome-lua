/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * automount-manager.h: Mounts the volumes of the removable media when they
 * are added, unless the session is inactive or inhibited. Port of
 * js/ui/components/automountManager.js.
 *
 * The component manager of the shell (js/ui/components.js) creates it for
 * the session modes that list `automountManager` and calls shell_automount_manager_enable ()
 * and shell_automount_manager_disable () when the session mode changes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define SHELL_TYPE_AUTOMOUNT_MANAGER (shell_automount_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellAutomountManager, shell_automount_manager,
                      SHELL, AUTOMOUNT_MANAGER, GObject)

ShellAutomountManager *shell_automount_manager_new     (void);

void                   shell_automount_manager_enable  (ShellAutomountManager *self);
void                   shell_automount_manager_disable (ShellAutomountManager *self);

gboolean               shell_automount_manager_volume_allows_autorun (GVolume *volume);

G_END_DECLS
