/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * util.c: Helpers of the login screen. Port of js/gdm/util.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "gdm/util.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"

typedef struct
{
  ClutterActor *clone;
  ShellBatchHold *hold;
} CloneFade;

static void
clone_fade_free (CloneFade *fade)
{
  g_clear_object (&fade->clone);
  g_clear_object (&fade->hold);
  g_free (fade);
}

static void
clone_fade_done_cb (gboolean finished,
                    gpointer user_data)
{
  CloneFade *fade = user_data;

  clutter_actor_destroy (fade->clone);
  shell_batch_hold_release (fade->hold);
}

/**
 * shell_gdm_clone_and_fade_out_actor:
 * @actor: the #ClutterActor to hide
 *
 * Hides @actor immediately, so its sibling can have its space and position,
 * but leaves a non-reactive clone on-screen that fades away, so from the
 * point of view of the user the actor smoothly fades away and reveals its
 * sibling. The clone is added to the UI group of the layout manager.
 *
 * Returns: (transfer full): a #ShellBatchHold that is released when the clone
 *   is gone
 */
ShellBatchHold *
shell_gdm_clone_and_fade_out_actor (ClutterActor *actor)
{
  ShellEaseParams params = {
    .duration = SHELL_GDM_CLONE_FADE_ANIMATION_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
  ShellLayoutManager *layout_manager;
  ShellBatchHold *hold;
  CloneFade *fade;
  ClutterActor *clone;
  float x, y;

  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), NULL);

  /* Immediately hide actor so its sibling can have its space
   * and position, but leave a non-reactive clone on-screen,
   * so from the user's point of view it smoothly fades away
   * and reveals its sibling. */
  clutter_actor_hide (actor);

  hold = shell_batch_hold_new ();

  layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  if (layout_manager == NULL)
    {
      g_debug ("%s: no layout manager, not fading the actor out", G_STRFUNC);
      shell_batch_hold_release (hold);
      return hold;
    }

  clone = clutter_clone_new (actor);
  clutter_actor_set_reactive (clone, FALSE);

  clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager)),
                           clone);

  clutter_actor_get_transformed_position (actor, &x, &y);
  clutter_actor_set_position (clone, x, y);

  fade = g_new0 (CloneFade, 1);
  fade->clone = g_object_ref (clone);
  fade->hold = g_object_ref (hold);

  shell_actor_ease (clone, &params,
                    clone_fade_done_cb, fade, (GDestroyNotify) clone_fade_free,
                    "opacity", (guint) 0,
                    NULL);

  return hold;
}
