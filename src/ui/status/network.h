/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * network.h: The network indicator of the top bar, and the quick settings
 * toggles (wired, Wi-Fi, mobile, Bluetooth tethering and VPN) that go with
 * it. Port of js/ui/status/network.js.
 *
 * #ShellNetworkIndicator is the only public type: the menu items, the toggles
 * and the captive portal handler of the JS module are private to network.c.
 * It is only built with NetworkManager support (HAVE_NETWORKMANAGER).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_NETWORK_INDICATOR (shell_network_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellNetworkIndicator, shell_network_indicator,
                      SHELL, NETWORK_INDICATOR, ShellSystemIndicator)

ShellNetworkIndicator *shell_network_indicator_new (void);

G_END_DECLS
