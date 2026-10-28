/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * deferred-work.c: Work that an actor does when it is mapped or, if it never
 * is, after a while. Counterpart of `Main.initializeDeferredWork ()` and
 * `Main.queueDeferredWork ()`.
 *
 * Differences from the JS functions:
 *
 *  - The JS shared one queue and one 20 second timeout between all the work;
 *    every piece of work has its own timeout and "before redraw" later here.
 *    The order in which pending work of different actors runs may differ.
 *  - The work is owned by its actor and dies with it (the JS removed it on the
 *    "destroy" signal), and is dropped on ShellGlobal::shutdown.
 *  - The work is queued by shell_deferred_work_new () just as the JS did.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <meta/compositor.h>
#include <meta/meta-later.h>

#include "shell-global.h"
#include "ui/deferred-work.h"

#define DEFERRED_TIMEOUT_SECONDS 20

struct _ShellDeferredWork
{
  ClutterActor *actor; /* weak */
  ShellDeferredWorkFunc func;
  gpointer user_data;

  gboolean pending;
  guint timeout_id;
  MetaLaters *laters; /* weak */
  unsigned int later_id;
};

static void
work_clear_sources (ShellDeferredWork *work)
{
  g_clear_handle_id (&work->timeout_id, g_source_remove);

  if (work->later_id != 0)
    {
      if (work->laters != NULL)
        meta_laters_remove (work->laters, work->later_id);
      work->later_id = 0;
    }
}

static void
work_free (ShellDeferredWork *work)
{
  work_clear_sources (work);
  g_clear_weak_pointer (&work->laters);
  g_clear_weak_pointer (&work->actor);
  g_free (work);
}

static void
work_run (ShellDeferredWork *work)
{
  if (!work->pending)
    return;

  work->pending = FALSE;
  work_clear_sources (work);
  work->func (work->user_data);
}

static gboolean
work_later_cb (gpointer user_data)
{
  ShellDeferredWork *work = user_data;

  work->later_id = 0;
  work_run (work);

  return G_SOURCE_REMOVE;
}

static void
work_queue_before_redraw (ShellDeferredWork *work)
{
  MetaCompositor *compositor;

  if (work->later_id != 0)
    return;

  compositor = shell_global_get_compositor (shell_global_get ());
  if (compositor == NULL)
    return;

  g_set_weak_pointer (&work->laters, meta_compositor_get_laters (compositor));
  work->later_id = meta_laters_add (work->laters, META_LATER_BEFORE_REDRAW,
                                    work_later_cb, work, NULL);
}

static gboolean
work_timeout_cb (gpointer user_data)
{
  ShellDeferredWork *work = user_data;

  work->timeout_id = 0;
  work_run (work);

  return G_SOURCE_REMOVE;
}

static void
actor_mapped_cb (GObject    *object,
                 GParamSpec *pspec,
                 gpointer    user_data)
{
  ShellDeferredWork *work = user_data;

  if (clutter_actor_is_mapped (CLUTTER_ACTOR (object)) && work->pending)
    work_queue_before_redraw (work);
}

static void
actor_destroy_cb (ClutterActor *actor,
                  gpointer      user_data)
{
  ShellDeferredWork *work = user_data;

  g_signal_handlers_disconnect_by_data (actor, work);
  work->pending = FALSE;
  g_clear_weak_pointer (&work->actor);
  work_clear_sources (work);
}

static void
global_shutdown_cb (ShellGlobal *global,
                    gpointer     user_data)
{
  ShellDeferredWork *work = user_data;

  work->pending = FALSE;
  work_clear_sources (work);
}

/**
 * shell_deferred_work_new: (skip)
 * @actor: the actor the work is for; the work is freed with it. An actor can
 *   have one piece of work
 * @func: the work to do
 * @user_data: data for @func
 *
 * Calls @func when @actor is mapped, before the next redraw, or 20 seconds
 * after it was queued if it is not mapped by then. The work is queued
 * already.
 *
 * Returns: (transfer none): the work, valid as long as @actor
 */
ShellDeferredWork *
shell_deferred_work_new (ClutterActor         *actor,
                         ShellDeferredWorkFunc func,
                         gpointer              user_data)
{
  ShellDeferredWork *work;

  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), NULL);

  work = g_new0 (ShellDeferredWork, 1);
  g_set_weak_pointer (&work->actor, actor);
  work->func = func;
  work->user_data = user_data;

  g_signal_connect (actor, "notify::mapped", G_CALLBACK (actor_mapped_cb), work);
  g_signal_connect (actor, "destroy", G_CALLBACK (actor_destroy_cb), work);
  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (global_shutdown_cb), actor, 0);
  g_object_set_data_full (G_OBJECT (actor), "shell-deferred-work-last", work,
                          (GDestroyNotify) work_free);

  shell_deferred_work_queue (work);

  return work;
}

/**
 * shell_deferred_work_queue: (skip)
 * @work: a #ShellDeferredWork
 *
 * Makes sure the work runs once the actor is mapped, or after a timeout.
 */
void
shell_deferred_work_queue (ShellDeferredWork *work)
{
  g_return_if_fail (work != NULL);

  if (work->actor == NULL)
    return;

  work->pending = TRUE;

  if (clutter_actor_is_mapped (work->actor))
    work_queue_before_redraw (work);
  else if (work->timeout_id == 0)
    work->timeout_id = g_timeout_add_seconds (DEFERRED_TIMEOUT_SECONDS,
                                              work_timeout_cb, work);
}
