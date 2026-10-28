/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * components.h: The component manager of the shell: it enables and disables
 * the components (the polkit agent, the keyring prompter, the automounter,
 * ...) that the current session mode lists in its `components` field. Port of
 * js/ui/components.js.
 *
 * Every component is a C class with `enable ()` and `disable ()`; the table
 * of names is in components.c. A component is created the first time a
 * session mode lists it and kept for the lifetime of the manager.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

#include "ui/session-mode.h"

G_BEGIN_DECLS

#define SHELL_TYPE_COMPONENT_MANAGER (shell_component_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellComponentManager, shell_component_manager,
                      SHELL, COMPONENT_MANAGER, GObject)

ShellComponentManager *shell_component_manager_new (ShellSessionMode *session_mode);

G_END_DECLS
