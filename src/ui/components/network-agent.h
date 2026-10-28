/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * network-agent.h: The user interface of the NetworkManager secret agent:
 * the dialogs and notifications which ask for the passwords of the wireless
 * networks, wired 802.1X networks, DSL, mobile broadband and VPN connections.
 * Port of js/ui/components/networkAgent.js.
 *
 * The component manager of the shell (js/ui/components.js) creates it for the
 * session modes that list `networkAgent` and calls
 * shell_network_agent_component_enable () and
 * shell_network_agent_component_disable () when the session mode changes.
 * It is only built with NetworkManager support (HAVE_NETWORKMANAGER).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SHELL_TYPE_NETWORK_AGENT_COMPONENT (shell_network_agent_component_get_type ())
G_DECLARE_FINAL_TYPE (ShellNetworkAgentComponent, shell_network_agent_component,
                      SHELL, NETWORK_AGENT_COMPONENT, GObject)

ShellNetworkAgentComponent *shell_network_agent_component_new     (void);

void                        shell_network_agent_component_enable  (ShellNetworkAgentComponent *self);
void                        shell_network_agent_component_disable (ShellNetworkAgentComponent *self);

G_END_DECLS
