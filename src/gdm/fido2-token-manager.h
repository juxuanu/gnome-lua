/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * fido2-token-manager.h: Tracks the FIDO2 security keys that are plugged in.
 * Port of js/gdm/fido2TokenManager.js.
 *
 * Only the authentication services of the login screen use it, so the type is
 * private to libshell (it is not in the GIR).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SHELL_TYPE_FIDO2_TOKEN_MANAGER (shell_fido2_token_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellFido2TokenManager, shell_fido2_token_manager,
                      SHELL, FIDO2_TOKEN_MANAGER, GObject)

ShellFido2TokenManager *shell_fido2_token_manager_get_default (void);

gboolean shell_fido2_token_manager_has_inserted_tokens (ShellFido2TokenManager *manager);

G_END_DECLS
