/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * polkit-agent.h: The user interface of the polkit authentication agent: the
 * dialog which asks the user for the password of the administrator (or of
 * the user itself) when an action needs an authorization. Port of
 * js/ui/components/polkitAgent.js.
 *
 * The component manager of the shell (src/ui/components.c) creates it for the
 * session modes that list `polkitAgent` and calls
 * shell_polkit_agent_component_enable () and
 * shell_polkit_agent_component_disable () when the session mode changes. The
 * agent which talks to polkit is #ShellPolkitAuthenticationAgent, the
 * component owns it; the dialog is private to polkit-agent.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SHELL_TYPE_POLKIT_AGENT_COMPONENT (shell_polkit_agent_component_get_type ())
G_DECLARE_FINAL_TYPE (ShellPolkitAgentComponent, shell_polkit_agent_component,
                      SHELL, POLKIT_AGENT_COMPONENT, GObject)

ShellPolkitAgentComponent *shell_polkit_agent_component_new     (void);

void                       shell_polkit_agent_component_enable  (ShellPolkitAgentComponent *self);
void                       shell_polkit_agent_component_disable (ShellPolkitAgentComponent *self);

G_END_DECLS
