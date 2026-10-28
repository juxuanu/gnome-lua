/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * smartcard-manager.h: Tracks the smartcards that gnome-settings-daemon
 * reports. Port of js/gdm/smartcardManager.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define SHELL_TYPE_SMARTCARD_MANAGER (shell_smartcard_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellSmartcardManager, shell_smartcard_manager,
                      SHELL, SMARTCARD_MANAGER, GObject)

ShellSmartcardManager *shell_smartcard_manager_get_default (void);

gboolean shell_smartcard_manager_has_inserted_tokens      (ShellSmartcardManager *manager);
gboolean shell_smartcard_manager_has_inserted_login_token (ShellSmartcardManager *manager);

G_END_DECLS
