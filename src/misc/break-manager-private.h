/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * break-manager-private.h: The environment of #ShellBreakManager (clock,
 * timeouts, idle monitor and settings) as a table of functions, so that the
 * unit test can replace it with a simulated clock and event queue. Port of
 * the `clock`, `idleMonitor` and `settingsFactory` constructor arguments of
 * the JS BreakManager.
 *
 * Only tests/unit/test-break-manager.c and break-manager.c use this.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

#include "misc/break-manager.h"

G_BEGIN_DECLS

/**
 * ShellBreakWatchFunc:
 * @user_data: the data given when the watch was added
 *
 * Called when an idle or user active watch fires.
 */
typedef void (* ShellBreakWatchFunc) (gpointer user_data);

/**
 * ShellBreakManagerEnv:
 * @user_data: the first argument of every function
 * @destroy: (nullable): called on @user_data when the manager is finalized
 * @get_real_time_secs: the real (wall clock) time, in seconds since the Unix epoch
 * @timeout_add_seconds: like g_timeout_add_seconds () at %G_PRIORITY_DEFAULT
 * @source_remove: like g_source_remove ()
 * @add_idle_watch: adds an uninhibitable idle watch that starts counting now
 *   and returns its id (`add_idle_watch_full`)
 * @add_user_active_watch: adds a one-shot user active watch and returns its id
 * @remove_watch: removes an idle or user active watch
 * @settings_new: creates the settings of the schema @schema_id, at the default
 *   path if @path is %NULL; returns an opaque object
 * @settings_free: frees the object returned by @settings_new
 * @settings_get_path: the path of the settings object
 * @settings_connect_changed: calls @func when any key of the settings changes,
 *   returns a handler id
 * @settings_disconnect: disconnects a handler returned by
 *   @settings_connect_changed
 * @settings_get_strv: the value of a `as` key
 * @settings_get_uint: the value of a `u` key
 * @settings_get_boolean: the value of a `b` key
 *
 * The functions the manager needs from its environment. Use
 * shell_break_manager_env_new_default () for the real one.
 */
typedef struct
{
  gpointer user_data;
  GDestroyNotify destroy;

  gint64   (* get_real_time_secs)     (gpointer user_data);
  guint    (* timeout_add_seconds)    (gpointer user_data,
                                       guint interval,
                                       GSourceFunc func,
                                       gpointer func_data);
  void     (* source_remove)          (gpointer user_data,
                                       guint id);

  guint    (* add_idle_watch)         (gpointer user_data,
                                       guint64 interval_msec,
                                       ShellBreakWatchFunc func,
                                       gpointer func_data);
  guint    (* add_user_active_watch)  (gpointer user_data,
                                       ShellBreakWatchFunc func,
                                       gpointer func_data);
  void     (* remove_watch)           (gpointer user_data,
                                       guint id);

  gpointer (* settings_new)           (gpointer user_data,
                                       const char *schema_id,
                                       const char *path);
  void     (* settings_free)          (gpointer user_data,
                                       gpointer settings);
  const char * (* settings_get_path)  (gpointer user_data,
                                       gpointer settings);
  gulong   (* settings_connect_changed) (gpointer user_data,
                                         gpointer settings,
                                         ShellBreakWatchFunc func,
                                         gpointer func_data);
  void     (* settings_disconnect)    (gpointer user_data,
                                       gpointer settings,
                                       gulong handler_id);
  char **  (* settings_get_strv)      (gpointer user_data,
                                       gpointer settings,
                                       const char *key);
  guint    (* settings_get_uint)      (gpointer user_data,
                                       gpointer settings,
                                       const char *key);
  gboolean (* settings_get_boolean)   (gpointer user_data,
                                       gpointer settings,
                                       const char *key);
} ShellBreakManagerEnv;

/**
 * shell_break_manager_new_with_env:
 * @env: the environment; the table is copied
 *
 * Creates a manager that uses @env instead of the real clock, idle monitor
 * and GSettings.
 *
 * Returns: (transfer full): a new #ShellBreakManager
 */
ShellBreakManager *shell_break_manager_new_with_env (const ShellBreakManagerEnv *env);

G_END_DECLS
