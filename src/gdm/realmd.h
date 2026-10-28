/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * realmd.h: Finds the login format of the network realms that realmd knows.
 * Port of js/gdm/realmd.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SHELL_TYPE_REALMD_MANAGER (shell_realmd_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellRealmdManager, shell_realmd_manager,
                      SHELL, REALMD_MANAGER, GObject)

ShellRealmdManager *shell_realmd_manager_new (void);

const char *shell_realmd_manager_get_login_format (ShellRealmdManager *manager);

void        shell_realmd_manager_release          (ShellRealmdManager *manager);

G_END_DECLS
