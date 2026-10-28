/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * qr-code.h: A square widget which shows a text as a QR code, drawn in the
 * foreground color of its style. Port of js/ui/qrCode.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

#define SHELL_QR_CODE_DEFAULT_SIZE 150

#define SHELL_TYPE_QR_CODE (shell_qr_code_get_type ())
G_DECLARE_FINAL_TYPE (ShellQrCode, shell_qr_code, SHELL, QR_CODE, StBin)

ShellQrCode *shell_qr_code_new      (void);

const char  *shell_qr_code_get_text (ShellQrCode *self);
void         shell_qr_code_set_text (ShellQrCode *self,
                                     const char  *text);

G_END_DECLS
