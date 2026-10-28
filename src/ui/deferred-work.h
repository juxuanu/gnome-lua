/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * deferred-work.h: Work that an actor does when it is mapped or, if it never
 * is, after a while. The C counterpart of `Main.initializeDeferredWork ()`
 * and `Main.queueDeferredWork ()` of js/ui/main.js, for the C modules that
 * used them (the application grid and the dash). Private to libshell.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>

G_BEGIN_DECLS

typedef struct _ShellDeferredWork ShellDeferredWork;

typedef void (* ShellDeferredWorkFunc) (gpointer user_data);

ShellDeferredWork *shell_deferred_work_new   (ClutterActor         *actor,
                                              ShellDeferredWorkFunc func,
                                              gpointer              user_data);
void               shell_deferred_work_queue (ShellDeferredWork    *work);

G_END_DECLS
