/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * modem-manager.h: The operator name and signal quality of a mobile
 * broadband modem. Port of js/misc/modemManager.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>
#include <NetworkManager.h>

G_BEGIN_DECLS

/**
 * ShellModemType:
 * @SHELL_MODEM_TYPE_BROADBAND: a modem of the ModemManager1 D-Bus API
 *   (ModemManager 0.7 and newer), the former `BroadbandModem`
 * @SHELL_MODEM_TYPE_GSM: a GSM modem of the old ModemManager D-Bus API
 *   (before ModemManager 0.7), the former `ModemGsm`
 * @SHELL_MODEM_TYPE_CDMA: a CDMA modem of the old ModemManager D-Bus API
 *   (before ModemManager 0.7), the former `ModemCdma`
 *
 * Which of the ModemManager D-Bus APIs a #ShellModem talks to.
 */
typedef enum
{
  SHELL_MODEM_TYPE_BROADBAND,
  SHELL_MODEM_TYPE_GSM,
  SHELL_MODEM_TYPE_CDMA,
} ShellModemType;

#define SHELL_TYPE_MODEM (shell_modem_get_type ())
G_DECLARE_FINAL_TYPE (ShellModem, shell_modem, SHELL, MODEM, GObject)

ShellModem *shell_modem_new_broadband (const char                *path,
                                       NMDeviceModemCapabilities  capabilities);
ShellModem *shell_modem_new_gsm       (const char                *path);
ShellModem *shell_modem_new_cdma      (const char                *path);

const char *shell_modem_get_operator_name (ShellModem *modem);
int         shell_modem_get_signal_quality (ShellModem *modem);

G_END_DECLS
