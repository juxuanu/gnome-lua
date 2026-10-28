/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * gnome-session.h: Proxies for the org.gnome.SessionManager D-Bus interfaces.
 * Port of js/misc/gnomeSession.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "gnome-session-dbus.h"

G_BEGIN_DECLS

/**
 * ShellPresenceStatus:
 * @SHELL_PRESENCE_STATUS_AVAILABLE: the user is available
 * @SHELL_PRESENCE_STATUS_INVISIBLE: the user does not want to be seen
 * @SHELL_PRESENCE_STATUS_BUSY: the user does not want to be disturbed
 * @SHELL_PRESENCE_STATUS_IDLE: the session is idle
 *
 * The status of org.gnome.SessionManager.Presence.
 */
typedef enum
{
  SHELL_PRESENCE_STATUS_AVAILABLE = 0,
  SHELL_PRESENCE_STATUS_INVISIBLE = 1,
  SHELL_PRESENCE_STATUS_BUSY = 2,
  SHELL_PRESENCE_STATUS_IDLE = 3,
} ShellPresenceStatus;

/**
 * ShellInhibitFlags:
 * @SHELL_INHIBIT_FLAGS_LOGOUT: inhibit logging out
 * @SHELL_INHIBIT_FLAGS_SWITCH: inhibit user switching
 * @SHELL_INHIBIT_FLAGS_SUSPEND: inhibit suspending
 * @SHELL_INHIBIT_FLAGS_IDLE: inhibit the session going idle
 * @SHELL_INHIBIT_FLAGS_AUTOMOUNT: inhibit automounting
 *
 * The flags of a gnome-session inhibitor.
 */
typedef enum
{
  SHELL_INHIBIT_FLAGS_LOGOUT = 1 << 0,
  SHELL_INHIBIT_FLAGS_SWITCH = 1 << 1,
  SHELL_INHIBIT_FLAGS_SUSPEND = 1 << 2,
  SHELL_INHIBIT_FLAGS_IDLE = 1 << 3,
  SHELL_INHIBIT_FLAGS_AUTOMOUNT = 1 << 4,
} ShellInhibitFlags;

/**
 * ShellActionAvailability:
 * @SHELL_ACTION_AVAILABILITY_UNAVAILABLE: the action is not available
 * @SHELL_ACTION_AVAILABILITY_BLOCKED: the action is blocked
 * @SHELL_ACTION_AVAILABILITY_CHALLENGE: the action needs authentication
 * @SHELL_ACTION_AVAILABILITY_AVAILABLE: the action is available
 *
 * The answer of the CanShutdown(), CanReboot() and CanSuspend() calls of
 * org.gnome.SessionManager.
 */
typedef enum
{
  SHELL_ACTION_AVAILABILITY_UNAVAILABLE = 0,
  SHELL_ACTION_AVAILABILITY_BLOCKED = 1,
  SHELL_ACTION_AVAILABILITY_CHALLENGE = 2,
  SHELL_ACTION_AVAILABILITY_AVAILABLE = 3,
} ShellActionAvailability;

void                         shell_gnome_session_presence_new        (GCancellable        *cancellable,
                                                                      GAsyncReadyCallback  callback,
                                                                      gpointer             user_data);
ShellSessionManagerPresence *shell_gnome_session_presence_new_finish (GAsyncResult        *res,
                                                                      GError             **error);

void                  shell_gnome_session_manager_new        (GCancellable        *cancellable,
                                                              GAsyncReadyCallback  callback,
                                                              gpointer             user_data);
ShellSessionManager  *shell_gnome_session_manager_new_finish (GAsyncResult        *res,
                                                              GError             **error);
ShellSessionManager  *shell_gnome_session_manager_new_sync   (GCancellable        *cancellable,
                                                              GError             **error);

void                          shell_gnome_session_inhibitor_new        (const char          *object_path,
                                                                        GCancellable        *cancellable,
                                                                        GAsyncReadyCallback  callback,
                                                                        gpointer             user_data);
ShellSessionManagerInhibitor *shell_gnome_session_inhibitor_new_finish (GAsyncResult        *res,
                                                                        GError             **error);

G_END_DECLS
