/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dnd.h: Drag and drop inside the shell: the draggable that makes an actor a
 * drag source, the two interfaces the drag sources and the drop targets
 * implement, and the drag monitors. Port of js/ui/dnd.js.
 *
 * The JS protocol was duck-typed on `actor._delegate`:
 *
 *   - a drop target had a `handleDragOver ()` and an `acceptDrop ()` method;
 *   - a drag source had an optional `getDragActor ()` and
 *     `getDragActorSource ()`, and was handed to the targets as `source`.
 *
 * Both are interfaces now. JS classes implement them with
 * `Implements: [Shell.DragTarget]` and `vfunc_handle_drag_over ()` /
 * `vfunc_accept_drop ()`, or `Implements: [Shell.DragSource]` and
 * `vfunc_get_drag_actor ()` / `vfunc_get_drag_actor_source ()`. The
 * "delegate" of an actor is looked up like this, by shell_dnd_get_drag_target ():
 * the target registered on the actor with shell_dnd_set_drag_target () if
 * there is one, else the actor itself if it implements #ShellDragTarget.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>

#include "shell-app.h"
#include "st.h"
#include "ui/xdnd-handler.h"

G_BEGIN_DECLS

/**
 * SHELL_DND_SCALE_ANIMATION_TIME:
 *
 * Time in milliseconds to scale the drag actor down to its maximum size
 * (`DND.SCALE_ANIMATION_TIME`).
 */
#define SHELL_DND_SCALE_ANIMATION_TIME 250

/**
 * SHELL_DND_SNAP_BACK_ANIMATION_TIME:
 *
 * Time in milliseconds to animate the drag actor to its original position
 * when the drag is cancelled (`DND.SNAP_BACK_ANIMATION_TIME`).
 */
#define SHELL_DND_SNAP_BACK_ANIMATION_TIME 250

/**
 * SHELL_DND_REVERT_ANIMATION_TIME:
 *
 * Time in milliseconds to animate the drag actor to its original position
 * when the drop succeeded (`DND.REVERT_ANIMATION_TIME`).
 */
#define SHELL_DND_REVERT_ANIMATION_TIME 750

/**
 * ShellDragMotionResult:
 * @SHELL_DRAG_MOTION_RESULT_NO_DROP: the target does not accept the drop;
 *   the pointer shows the "no drop" cursor
 * @SHELL_DRAG_MOTION_RESULT_COPY_DROP: the drop would copy; the pointer shows
 *   the "copy" cursor
 * @SHELL_DRAG_MOTION_RESULT_MOVE_DROP: the drop would move; the pointer shows
 *   the "move" cursor
 * @SHELL_DRAG_MOTION_RESULT_CONTINUE: this target does not decide; the next
 *   target (the parent of the actor, or the next drag monitor) is asked
 *
 * What a drop target or a drag monitor answers when the drag moves over it
 * (`DND.DragMotionResult`).
 */
typedef enum
{
  SHELL_DRAG_MOTION_RESULT_NO_DROP   = 0,
  SHELL_DRAG_MOTION_RESULT_COPY_DROP = 1,
  SHELL_DRAG_MOTION_RESULT_MOVE_DROP = 2,
  SHELL_DRAG_MOTION_RESULT_CONTINUE  = 3,
} ShellDragMotionResult;

/**
 * ShellDragDropResult:
 * @SHELL_DRAG_DROP_RESULT_FAILURE: the monitor handled the drop, which failed
 * @SHELL_DRAG_DROP_RESULT_SUCCESS: the monitor handled the drop, which
 *   succeeded
 * @SHELL_DRAG_DROP_RESULT_CONTINUE: the monitor did not handle the drop; the
 *   next monitor and then the drop targets are asked
 *
 * What a drag monitor answers when the dragged item is dropped
 * (`DND.DragDropResult`).
 */
typedef enum
{
  SHELL_DRAG_DROP_RESULT_FAILURE  = 0,
  SHELL_DRAG_DROP_RESULT_SUCCESS  = 1,
  SHELL_DRAG_DROP_RESULT_CONTINUE = 2,
} ShellDragDropResult;

/* ------------------------------------------------------------------------ */
/* ShellDragEvent, ShellDropEvent                                            */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_DRAG_EVENT (shell_drag_event_get_type ())
G_DECLARE_FINAL_TYPE (ShellDragEvent, shell_drag_event,
                      SHELL, DRAG_EVENT, GObject)

float         shell_drag_event_get_x            (ShellDragEvent *self);
float         shell_drag_event_get_y            (ShellDragEvent *self);
ClutterActor *shell_drag_event_get_drag_actor   (ShellDragEvent *self);
GObject      *shell_drag_event_get_source       (ShellDragEvent *self);
ClutterActor *shell_drag_event_get_target_actor (ShellDragEvent *self);

#define SHELL_TYPE_DROP_EVENT (shell_drop_event_get_type ())
G_DECLARE_FINAL_TYPE (ShellDropEvent, shell_drop_event,
                      SHELL, DROP_EVENT, GObject)

ClutterActor       *shell_drop_event_get_drop_actor    (ShellDropEvent *self);
ClutterActor       *shell_drop_event_get_target_actor  (ShellDropEvent *self);
const ClutterEvent *shell_drop_event_get_clutter_event (ShellDropEvent *self);

/* ------------------------------------------------------------------------ */
/* ShellDragTarget                                                           */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_DRAG_TARGET (shell_drag_target_get_type ())
G_DECLARE_INTERFACE (ShellDragTarget, shell_drag_target,
                     SHELL, DRAG_TARGET, GObject)

/**
 * ShellDragTargetInterface:
 * @parent_iface: the parent interface
 * @handle_drag_over: virtual function for shell_drag_target_handle_drag_over ().
 *   The default implementation returns %SHELL_DRAG_MOTION_RESULT_CONTINUE
 * @accept_drop: virtual function for shell_drag_target_accept_drop (). The
 *   default implementation returns %FALSE
 *
 * The interface of the actors (and the objects registered with
 * shell_dnd_set_drag_target ()) that items can be dropped on.
 */
struct _ShellDragTargetInterface
{
  GTypeInterface parent_iface;

  ShellDragMotionResult (* handle_drag_over) (ShellDragTarget *target,
                                              GObject         *source,
                                              ClutterActor    *drag_actor,
                                              float            x,
                                              float            y,
                                              uint32_t         time);
  gboolean              (* accept_drop)      (ShellDragTarget *target,
                                              GObject         *source,
                                              ClutterActor    *drag_actor,
                                              float            x,
                                              float            y,
                                              uint32_t         time);
};

ShellDragMotionResult shell_drag_target_handle_drag_over (ShellDragTarget *target,
                                                          GObject         *source,
                                                          ClutterActor    *drag_actor,
                                                          float            x,
                                                          float            y,
                                                          uint32_t         time);
gboolean              shell_drag_target_accept_drop      (ShellDragTarget *target,
                                                          GObject         *source,
                                                          ClutterActor    *drag_actor,
                                                          float            x,
                                                          float            y,
                                                          uint32_t         time);

/* ------------------------------------------------------------------------ */
/* ShellDragSource                                                           */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_DRAG_SOURCE (shell_drag_source_get_type ())
G_DECLARE_INTERFACE (ShellDragSource, shell_drag_source,
                     SHELL, DRAG_SOURCE, GObject)

/**
 * ShellDragSourceInterface:
 * @parent_iface: the parent interface
 * @get_drag_actor: virtual function for shell_drag_source_get_drag_actor ().
 *   The default implementation returns %NULL
 * @get_drag_actor_source: virtual function for
 *   shell_drag_source_get_drag_actor_source (). The default implementation
 *   returns %NULL
 * @get_app: virtual function for shell_drag_source_get_app (). The default
 *   implementation returns %NULL
 * @animate_launch_at_pos: virtual function for
 *   shell_drag_source_animate_launch_at_pos (). The default implementation
 *   does nothing
 *
 * The interface of the draggable actors which are not dragged themselves but
 * are represented by another actor while the drag lasts. A draggable actor
 * which does not implement it (or whose get_drag_actor returns %NULL) is
 * dragged itself.
 */
struct _ShellDragSourceInterface
{
  GTypeInterface parent_iface;

  ClutterActor * (* get_drag_actor)        (ShellDragSource *source);
  ClutterActor * (* get_drag_actor_source) (ShellDragSource *source);
  ShellApp *     (* get_app)               (ShellDragSource *source);
  void           (* animate_launch_at_pos) (ShellDragSource *source,
                                            float            x,
                                            float            y);
};

ClutterActor *shell_drag_source_get_drag_actor        (ShellDragSource *source);
ClutterActor *shell_drag_source_get_drag_actor_source (ShellDragSource *source);
ShellApp     *shell_drag_source_get_app               (ShellDragSource *source);
void          shell_drag_source_animate_launch_at_pos (ShellDragSource *source,
                                                      float            x,
                                                      float            y);

/* ------------------------------------------------------------------------ */
/* ShellDraggable                                                            */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_DRAGGABLE (shell_draggable_get_type ())
G_DECLARE_FINAL_TYPE (ShellDraggable, shell_draggable,
                      SHELL, DRAGGABLE, GObject)

ShellDraggable    *shell_draggable_new               (ClutterActor *actor);

ClutterActor      *shell_draggable_get_actor         (ShellDraggable *self);
StDndStartGesture *shell_draggable_get_start_gesture (ShellDraggable *self);

/* ------------------------------------------------------------------------ */
/* Targets, monitors                                                         */
/* ------------------------------------------------------------------------ */

/**
 * ShellDragMotionFunc:
 * @event: the drag event
 * @user_data: (closure): the user data given along with the callback
 *
 * Called by a drag monitor when the dragged item moves, with the innermost
 * actor under the pointer. The monitor can walk the parents of
 * shell_drag_event_get_target_actor () itself.
 *
 * Returns: %SHELL_DRAG_MOTION_RESULT_CONTINUE to let the next monitor and the
 *   drop targets see the event, another value to stop here and show the
 *   matching cursor (not for the drags of X11 clients)
 */
typedef ShellDragMotionResult (* ShellDragMotionFunc) (ShellDragEvent *event,
                                                       gpointer        user_data);

/**
 * ShellDragDropFunc:
 * @event: the drop event
 * @user_data: (closure): the user data given along with the callback
 *
 * Called by a drag monitor when the dragged item is dropped.
 *
 * Returns: %SHELL_DRAG_DROP_RESULT_CONTINUE to let the next monitor and the
 *   drop targets see the event, another value to take the drop over
 */
typedef ShellDragDropResult (* ShellDragDropFunc) (ShellDropEvent *event,
                                                   gpointer        user_data);

void             shell_dnd_set_drag_target  (ClutterActor    *actor,
                                             ShellDragTarget *target);
ShellDragTarget *shell_dnd_get_drag_target  (ClutterActor    *actor);

guint            shell_dnd_add_drag_monitor (ShellDragMotionFunc motion,
                                             gpointer            motion_data,
                                             GDestroyNotify      motion_destroy,
                                             ShellDragDropFunc   drop,
                                             gpointer            drop_data,
                                             GDestroyNotify      drop_destroy);
void             shell_dnd_remove_drag_monitor (guint id);

gboolean         shell_dnd_is_dragging      (void);

void             shell_dnd_set_xdnd_handler (ShellXdndHandler *handler);

G_END_DECLS
