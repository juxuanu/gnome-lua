/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * bluetooth.h: The Bluetooth indicator of the top bar and its toggle in the
 * quick settings. Port of js/ui/status/bluetooth.js.
 *
 * The client, the device items and the toggle are private to the C file; the
 * top bar only creates the indicator (`new Shell.BluetoothIndicator ()`).
 */

#pragma once

#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_BLUETOOTH_INDICATOR (shell_bluetooth_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellBluetoothIndicator, shell_bluetooth_indicator,
                      SHELL, BLUETOOTH_INDICATOR, ShellSystemIndicator)

ShellBluetoothIndicator *shell_bluetooth_indicator_new (void);

G_END_DECLS
