/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * keyring.h: The user interface of the system prompter of gnome-keyring: the
 * dialog which asks for the passwords of the keyrings and the confirmations
 * of the secret service. Port of js/ui/components/keyring.js.
 *
 * The component manager of the shell (js/ui/components.js) creates it for the
 * session modes that list `keyring` and calls shell_keyring_component_enable ()
 * and shell_keyring_component_disable () when the session mode changes. The
 * prompts themselves are #ShellKeyringPrompt, the dialog is private to
 * keyring.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SHELL_TYPE_KEYRING_COMPONENT (shell_keyring_component_get_type ())
G_DECLARE_FINAL_TYPE (ShellKeyringComponent, shell_keyring_component,
                      SHELL, KEYRING_COMPONENT, GObject)

ShellKeyringComponent *shell_keyring_component_new     (void);

void                   shell_keyring_component_enable  (ShellKeyringComponent *self);
void                   shell_keyring_component_disable (ShellKeyringComponent *self);

G_END_DECLS
