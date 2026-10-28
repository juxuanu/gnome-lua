/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * fingerprint-manager.h: Finds out what kind of fingerprint reader fprintd
 * reports. Port of js/gdm/fingerprintManager.js.
 *
 * Only the authentication services of the login screen use it, so the type is
 * private to libshell (it is not in the GIR).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum
{
  SHELL_FINGERPRINT_READER_TYPE_NONE = 0,
  SHELL_FINGERPRINT_READER_TYPE_PRESS = 1,
  SHELL_FINGERPRINT_READER_TYPE_SWIPE = 2,
} ShellFingerprintReaderType;

#define SHELL_TYPE_FINGERPRINT_MANAGER (shell_fingerprint_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellFingerprintManager, shell_fingerprint_manager,
                      SHELL, FINGERPRINT_MANAGER, GObject)

ShellFingerprintManager *shell_fingerprint_manager_get_default (void);

ShellFingerprintReaderType shell_fingerprint_manager_get_reader_type  (ShellFingerprintManager *manager);
gboolean                   shell_fingerprint_manager_get_reader_found (ShellFingerprintManager *manager);

void shell_fingerprint_manager_set_default_timeout (ShellFingerprintManager *manager,
                                                    int                      timeout);
void shell_fingerprint_manager_check_reader_type   (ShellFingerprintManager *manager,
                                                    GCancellable            *cancellable);

G_END_DECLS
