/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * system-actions.h: The system actions (power off, restart, suspend, lock
 * screen, log out, switch user, lock orientation, screenshot UI) that the
 * system menu, the search and the session timers can trigger, and whether
 * each of them is currently available. Port of js/misc/systemActions.js.
 *
 * The actions that need the JS-only parts of the shell (the screen shield,
 * the overview and the screenshot UI) are requested with signals that
 * js/ui/main.js connects to those objects.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "ui/session-mode.h"

G_BEGIN_DECLS

#define SHELL_TYPE_SYSTEM_ACTIONS (shell_system_actions_get_type ())
G_DECLARE_FINAL_TYPE (ShellSystemActions, shell_system_actions,
                      SHELL, SYSTEM_ACTIONS, GObject)

ShellSystemActions *shell_system_actions_get_default (void);

gboolean     shell_system_actions_get_can_power_off        (ShellSystemActions *self);
gboolean     shell_system_actions_get_can_restart          (ShellSystemActions *self);
gboolean     shell_system_actions_get_can_suspend          (ShellSystemActions *self);
gboolean     shell_system_actions_get_can_lock_screen      (ShellSystemActions *self);
gboolean     shell_system_actions_get_can_switch_user      (ShellSystemActions *self);
gboolean     shell_system_actions_get_can_logout           (ShellSystemActions *self);
gboolean     shell_system_actions_get_can_lock_orientation (ShellSystemActions *self);
const char  *shell_system_actions_get_orientation_lock_icon (ShellSystemActions *self);

ShellSessionMode *shell_system_actions_get_session_mode (ShellSystemActions *self);
void              shell_system_actions_set_session_mode (ShellSystemActions *self,
                                                         ShellSessionMode   *session_mode);

void         shell_system_actions_force_update          (ShellSystemActions *self);

char       **shell_system_actions_get_matching_actions  (ShellSystemActions  *self,
                                                         const char * const  *terms);
const char  *shell_system_actions_get_name              (ShellSystemActions *self,
                                                         const char         *action_id);
const char  *shell_system_actions_get_icon_name         (ShellSystemActions *self,
                                                         const char         *action_id);

gboolean     shell_system_actions_activate_action       (ShellSystemActions  *self,
                                                         const char          *action_id,
                                                         GError             **error);
gboolean     shell_system_actions_activate_power_off    (ShellSystemActions  *self,
                                                         GError             **error);
gboolean     shell_system_actions_activate_restart      (ShellSystemActions  *self,
                                                         GError             **error);
gboolean     shell_system_actions_activate_suspend      (ShellSystemActions  *self,
                                                         GError             **error);
gboolean     shell_system_actions_activate_lock_screen  (ShellSystemActions  *self,
                                                         GError             **error);
gboolean     shell_system_actions_activate_logout       (ShellSystemActions  *self,
                                                         GError             **error);
gboolean     shell_system_actions_activate_switch_user  (ShellSystemActions  *self,
                                                         GError             **error);
gboolean     shell_system_actions_activate_lock_orientation (ShellSystemActions  *self,
                                                             GError             **error);
gboolean     shell_system_actions_activate_screenshot_ui (ShellSystemActions  *self,
                                                          GError             **error);

G_END_DECLS
