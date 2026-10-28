/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * session-mode.h: The session mode stack (user, gdm, unlock-dialog, ...) and
 * the capabilities of the mode on top of it. Port of js/ui/sessionMode.js.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define SHELL_TYPE_SESSION_MODE (shell_session_mode_get_type ())
G_DECLARE_FINAL_TYPE (ShellSessionMode, shell_session_mode,
                      SHELL, SESSION_MODE, GObject)

ShellSessionMode *shell_session_mode_new            (void);

gboolean          shell_session_mode_push_mode      (ShellSessionMode  *self,
                                                     const char        *mode,
                                                     GError           **error);
gboolean          shell_session_mode_pop_mode       (ShellSessionMode  *self,
                                                     const char        *mode,
                                                     GError           **error);
gboolean          shell_session_mode_switch_mode    (ShellSessionMode  *self,
                                                     const char        *to,
                                                     GError           **error);

const char       *shell_session_mode_get_current_mode (ShellSessionMode *self);
const char       *shell_session_mode_get_parent_mode  (ShellSessionMode *self);

gboolean          shell_session_mode_is_locked      (ShellSessionMode *self);
gboolean          shell_session_mode_is_greeter     (ShellSessionMode *self);
gboolean          shell_session_mode_is_primary     (ShellSessionMode *self);

GVariant         *shell_session_mode_get_field      (ShellSessionMode *self,
                                                     const char       *name);

char            **shell_session_mode_list_modes     (void);

G_END_DECLS
