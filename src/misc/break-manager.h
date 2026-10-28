/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * break-manager.h: A GNOME Shell component to support break reminders and
 * screen time statistics. Port of js/misc/breakManager.js.
 *
 * Copyright 2024 GNOME Foundation, Inc.
 *
 * #ShellBreakManager tracks the active and idle time of the user and says
 * when a break is due, according to the break reminder preferences.
 * #ShellBreakDispatcher turns that state into notifications, sounds, screen
 * fades and a countdown for the user.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "ui/layout.h"
#include "ui/message-tray.h"

G_BEGIN_DECLS

/**
 * SHELL_MIN_BREAK_LENGTH_SECONDS:
 *
 * The shortest break, and so the idle time after which the user counts as idle
 * (`MIN_BREAK_LENGTH_SECONDS`).
 */
#define SHELL_MIN_BREAK_LENGTH_SECONDS 10

/**
 * ShellBreakState:
 * @SHELL_BREAK_STATE_DISABLED: break reminders are disabled
 * @SHELL_BREAK_STATE_ACTIVE: break reminders are enabled, the user is active,
 *   no break is needed yet
 * @SHELL_BREAK_STATE_IDLE: the user is idle, but no break is needed
 * @SHELL_BREAK_STATE_IN_BREAK: a break is needed and the user is taking it
 * @SHELL_BREAK_STATE_BREAK_DUE: a break is needed but the user is still
 *   active
 *
 * The state of the break reminders (`BreakManager.BreakState`). This is
 * exposed over D-Bus (/org/gnome/Shell/ScreenTime), so the values are
 * effectively public API.
 */
typedef enum
{
  SHELL_BREAK_STATE_DISABLED = 0,
  SHELL_BREAK_STATE_ACTIVE   = 1,
  SHELL_BREAK_STATE_IDLE     = 2,
  SHELL_BREAK_STATE_IN_BREAK = 3,
  SHELL_BREAK_STATE_BREAK_DUE = 4,
} ShellBreakState;

const char *shell_break_state_to_string (ShellBreakState state);

/* ------------------------------------------------------------------------ */
/* ShellBreakManager                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_BREAK_MANAGER (shell_break_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellBreakManager, shell_break_manager,
                      SHELL, BREAK_MANAGER, GObject)

ShellBreakManager *shell_break_manager_new (void);

ShellBreakState shell_break_manager_get_state                 (ShellBreakManager *self);
const char     *shell_break_manager_get_current_break_type    (ShellBreakManager *self);
gint64          shell_break_manager_get_current_break_start_time (ShellBreakManager *self);
gint64          shell_break_manager_get_last_break_end_time   (ShellBreakManager *self);

gint64          shell_break_manager_get_current_time          (ShellBreakManager *self);
gint64          shell_break_manager_get_next_break_due        (ShellBreakManager  *self,
                                                               gint64              from_time,
                                                               const char        **break_type);
gint64          shell_break_manager_get_next_break_due_time   (ShellBreakManager *self,
                                                               gint64             from_time);

void            shell_break_manager_delay_break               (ShellBreakManager *self);
void            shell_break_manager_skip_break                (ShellBreakManager *self);
void            shell_break_manager_take_break                (ShellBreakManager *self);

gboolean        shell_break_manager_break_type_should_notify          (ShellBreakManager *self,
                                                                       const char        *break_type);
gboolean        shell_break_manager_break_type_should_notify_upcoming  (ShellBreakManager *self,
                                                                       const char        *break_type);
gboolean        shell_break_manager_break_type_should_notify_overdue   (ShellBreakManager *self,
                                                                       const char        *break_type);
gboolean        shell_break_manager_break_type_should_countdown       (ShellBreakManager *self,
                                                                       const char        *break_type);
gboolean        shell_break_manager_break_type_should_play_sound      (ShellBreakManager *self,
                                                                       const char        *break_type);
gboolean        shell_break_manager_break_type_should_fade_screen     (ShellBreakManager *self,
                                                                       const char        *break_type);
gboolean        shell_break_manager_break_type_should_lock_screen     (ShellBreakManager *self,
                                                                       const char        *break_type);
guint           shell_break_manager_get_duration_for_break_type       (ShellBreakManager *self,
                                                                       const char        *break_type);

/* ------------------------------------------------------------------------ */
/* ShellBreakDispatcher                                                      */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_BREAK_DISPATCHER (shell_break_dispatcher_get_type ())
G_DECLARE_FINAL_TYPE (ShellBreakDispatcher, shell_break_dispatcher,
                      SHELL, BREAK_DISPATCHER, GObject)

ShellBreakDispatcher *shell_break_dispatcher_new     (ShellBreakManager  *manager,
                                                      ShellLayoutManager *layout_manager,
                                                      ShellMessageTray   *message_tray);
void                  shell_break_dispatcher_destroy (ShellBreakDispatcher *self);

G_END_DECLS
