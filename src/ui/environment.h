/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Animation helpers ported from js/ui/environment.js (Clutter.Actor.ease(),
 * Clutter.Actor.ease_property(), St.Adjustment.ease()) and
 * js/misc/animationUtils.js (adjustAnimationTime()).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>

G_BEGIN_DECLS

/**
 * ShellEaseCallback:
 * @finished: %TRUE if the animation ran to completion (or no animation was
 *   needed), %FALSE if it was stopped, removed or its actor destroyed
 * @user_data: (closure): the user data given along with the callback
 *
 * Called exactly once for every ease request, after the animation ends. It is
 * called synchronously, with @finished set to %TRUE, when no transition was
 * created.
 */
typedef void (* ShellEaseCallback) (gboolean  finished,
                                    gpointer  user_data);

#define SHELL_TYPE_EASE_PARAMS (shell_ease_params_get_type ())

/**
 * ShellEaseParams:
 * @duration: duration in milliseconds before the slow-down factor is applied,
 *   or -1 to keep the actor's current easing duration (actors only; for
 *   properties of animatables a value <= 0 means no animation)
 * @delay: delay in milliseconds before the slow-down factor is applied, or -1
 *   for none (actors: keep the current easing delay)
 * @mode: the #ClutterAnimationMode, or 0 to keep the default
 * @repeat_count: number of repetitions after the first iteration, -1 for
 *   infinite
 * @auto_reverse: whether the transition plays backwards on every other
 *   iteration
 * @animation_required: whether to ignore the enable-animations setting
 *
 * Parameters shared by shell_actor_ease_begin() and
 * shell_animatable_ease_property(). Use shell_ease_params_new() for the
 * defaults (duration and delay -1, mode 0, no repeat) and set every field you
 * care about.
 */
typedef struct _ShellEaseParams ShellEaseParams;

struct _ShellEaseParams
{
  gint                 duration;
  gint                 delay;
  ClutterAnimationMode mode;
  gint                 repeat_count;
  gboolean             auto_reverse;
  gboolean             animation_required;
};

GType            shell_ease_params_get_type (void);
ShellEaseParams *shell_ease_params_new      (void);
ShellEaseParams *shell_ease_params_copy     (const ShellEaseParams *params);
void             shell_ease_params_free     (ShellEaseParams       *params);

guint shell_adjust_animation_time (guint    msecs,
                                   gboolean animation_required);

gboolean shell_actor_ease_begin (ClutterActor          *actor,
                                 const ShellEaseParams *params,
                                 const char * const    *properties);

void     shell_actor_ease_end   (ClutterActor          *actor,
                                 const ShellEaseParams *params,
                                 const char * const    *properties,
                                 ShellEaseCallback      callback,
                                 gpointer               user_data,
                                 GDestroyNotify         destroy);

void     shell_actor_ease       (ClutterActor          *actor,
                                 const ShellEaseParams *params,
                                 ShellEaseCallback      callback,
                                 gpointer               user_data,
                                 GDestroyNotify         destroy,
                                 const char            *first_property,
                                 ...) G_GNUC_NULL_TERMINATED;

gboolean shell_animatable_ease_property (ClutterAnimatable      *animatable,
                                         const char             *property,
                                         const GValue           *target,
                                         const ShellEaseParams  *params,
                                         ShellEaseCallback       callback,
                                         gpointer                user_data,
                                         GDestroyNotify          destroy,
                                         GError                **error);

G_END_DECLS
