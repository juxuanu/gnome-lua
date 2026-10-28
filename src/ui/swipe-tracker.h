/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * swipe-tracker.h: Turns touchscreen/mouse drags, three finger touchpad
 * swipes and touchpad scrolling on an actor into one stream of progress
 * updates. Port of js/ui/swipeTracker.js.
 *
 * The handlers of #ShellSwipeTracker::begin, #ShellSwipeTracker::update and
 * #ShellSwipeTracker::end must follow the protocol documented at
 * shell_swipe_tracker_confirm_swipe ().
 */

#pragma once

#include <clutter/clutter.h>

#include "shell-action-modes.h"

G_BEGIN_DECLS

#define SHELL_TYPE_SWIPE_TRACKER (shell_swipe_tracker_get_type ())
G_DECLARE_FINAL_TYPE (ShellSwipeTracker, shell_swipe_tracker,
                      SHELL, SWIPE_TRACKER, GObject)

ShellSwipeTracker *shell_swipe_tracker_new                   (ClutterActor      *actor,
                                                              ClutterOrientation orientation,
                                                              ShellActionMode    allowed_modes);

gboolean           shell_swipe_tracker_get_enabled           (ShellSwipeTracker *self);
void               shell_swipe_tracker_set_enabled           (ShellSwipeTracker *self,
                                                              gboolean           enabled);

ClutterOrientation shell_swipe_tracker_get_orientation       (ShellSwipeTracker *self);
void               shell_swipe_tracker_set_orientation       (ShellSwipeTracker *self,
                                                              ClutterOrientation orientation);

double             shell_swipe_tracker_get_distance          (ShellSwipeTracker *self);
void               shell_swipe_tracker_set_distance          (ShellSwipeTracker *self,
                                                              double             distance);

gboolean           shell_swipe_tracker_get_allow_long_swipes (ShellSwipeTracker *self);
void               shell_swipe_tracker_set_allow_long_swipes (ShellSwipeTracker *self,
                                                              gboolean           allow_long_swipes);

ClutterModifierType shell_swipe_tracker_get_scroll_modifiers (ShellSwipeTracker *self);
void                shell_swipe_tracker_set_scroll_modifiers (ShellSwipeTracker *self,
                                                              ClutterModifierType scroll_modifiers);

ShellActionMode    shell_swipe_tracker_get_allowed_modes     (ShellSwipeTracker *self);

gboolean           shell_swipe_tracker_can_handle_scroll_event (ShellSwipeTracker  *self,
                                                                const ClutterEvent *scroll_event);

void               shell_swipe_tracker_confirm_swipe         (ShellSwipeTracker *self,
                                                              double             distance,
                                                              const double      *snap_points,
                                                              gsize              n_snap_points,
                                                              double             current_progress,
                                                              double             cancel_progress);

void               shell_swipe_tracker_destroy               (ShellSwipeTracker *self);

G_END_DECLS
