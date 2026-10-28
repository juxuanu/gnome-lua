/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dnd.c: Drag and drop inside the shell. Port of js/ui/dnd.js.
 *
 * Differences from the JS that every reader of this file should know about:
 *
 *  - The `actor._delegate` duck typing is gone. Drop targets implement
 *    #ShellDragTarget (`handleDragOver ()` is handle_drag_over, `acceptDrop ()`
 *    is accept_drop) and drag sources implement #ShellDragSource
 *    (`getDragActor ()` and `getDragActorSource ()`). The target of an actor is
 *    the one registered with shell_dnd_set_drag_target () or else the actor
 *    itself if it implements the interface. The `source` handed to the
 *    targets and the monitors is the dragged actor itself (it was always
 *    `actor._delegate`, which was the actor in every user), or the
 *    #ShellXdndHandler for the drags of X11 clients.
 *  - The events handed to the drag monitors are #ShellDragEvent and
 *    #ShellDropEvent objects (`dragEvent.dragActor` is
 *    `drag_event.drag_actor`, `targetActor` is `target_actor`, `dropEvent.dropActor` is
 *    `drop_event.drop_actor`, `clutterEvent` is `clutter_event`).
 *  - A monitor is added with shell_dnd_add_drag_monitor () and removed by the
 *    id this returns, not by the object that was added. The monitors are
 *    called from a snapshot of the list: a monitor removed by an earlier one
 *    during the same dispatch is not called (the JS array shifted under its
 *    loop and skipped the next monitor instead).
 *  - `_Draggable` is #ShellDraggable, a GObject (it was an
 *    `Signals.EventEmitter`) with construct properties instead of the params
 *    object. The actor owns the draggable (like it owns the gesture), so a
 *    draggable lives as long as its actor, like the closure of the "recognize"
 *    handler kept it alive in JS. An actor has at most one draggable.
 *  - The drag-actor-max-size and drag-actor-opacity properties use -1 for the
 *    `undefined` of the JS params.
 *  - DND.handleXdndMotion () is the "drag-motion" handler that
 *    shell_dnd_set_xdnd_handler () connects.
 */

#include "config.h"

#include "shell-global.h"
#include "shell-util.h"
#include "ui/dnd.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"

#define DRAG_TARGET_KEY "shell-dnd-drag-target"
#define DRAGGABLE_KEY "shell-dnd-draggable"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static ClutterStage *
get_stage (void)
{
  return shell_global_get_stage (shell_global_get ());
}

/* Main.uiGroup */
static ClutterActor *
get_ui_group (void)
{
  ShellLayoutManager *layout_manager =
    shell_main_get_layout_manager (shell_main_get_default ());

  g_return_val_if_fail (layout_manager != NULL, NULL);

  return CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager));
}

static ClutterCursorType
cursor_for_motion_result (ShellDragMotionResult result)
{
  switch (result)
    {
    case SHELL_DRAG_MOTION_RESULT_NO_DROP:
      return CLUTTER_CURSOR_NO_DROP;
    case SHELL_DRAG_MOTION_RESULT_COPY_DROP:
      return CLUTTER_CURSOR_COPY;
    case SHELL_DRAG_MOTION_RESULT_MOVE_DROP:
      return CLUTTER_CURSOR_MOVE;
    case SHELL_DRAG_MOTION_RESULT_CONTINUE:
    default:
      /* the JS looked up an undefined cursor here, which was never reached */
      return CLUTTER_CURSOR_NO_DROP;
    }
}

static void
update_cursor (ClutterCursorType cursor_type)
{
  ClutterActor *grab_actor = clutter_stage_get_grab_actor (get_stage ());

  if (grab_actor != NULL)
    clutter_actor_set_cursor_type (grab_actor, cursor_type);
}

/* actor.scale_x */
static float
get_scale_x (ClutterActor *actor)
{
  double scale_x;

  clutter_actor_get_scale (actor, &scale_x, NULL);

  return scale_x;
}

static float
get_real_actor_scale (ClutterActor *actor)
{
  float scale = 1.0f;

  while (actor != NULL)
    {
      scale *= get_scale_x (actor);
      actor = clutter_actor_get_parent (actor);
    }

  return scale;
}

/* ------------------------------------------------------------------------ */
/* ShellDragEvent                                                            */
/* ------------------------------------------------------------------------ */

/**
 * ShellDragEvent:
 *
 * What a drag monitor is told when the dragged item moves
 * (`{x, y, dragActor, source, targetActor}` of the JS).
 */
struct _ShellDragEvent
{
  GObject parent;

  float x;
  float y;
  ClutterActor *drag_actor;
  GObject *source;
  ClutterActor *target_actor;
};

enum
{
  DRAG_EVENT_PROP_0,
  DRAG_EVENT_PROP_X,
  DRAG_EVENT_PROP_Y,
  DRAG_EVENT_PROP_DRAG_ACTOR,
  DRAG_EVENT_PROP_SOURCE,
  DRAG_EVENT_PROP_TARGET_ACTOR,
  DRAG_EVENT_N_PROPS
};

static GParamSpec *drag_event_props[DRAG_EVENT_N_PROPS] = { NULL, };

G_DEFINE_FINAL_TYPE (ShellDragEvent, shell_drag_event, G_TYPE_OBJECT)

static void
shell_drag_event_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  ShellDragEvent *self = SHELL_DRAG_EVENT (object);

  switch (prop_id)
    {
    case DRAG_EVENT_PROP_X:
      g_value_set_float (value, self->x);
      break;
    case DRAG_EVENT_PROP_Y:
      g_value_set_float (value, self->y);
      break;
    case DRAG_EVENT_PROP_DRAG_ACTOR:
      g_value_set_object (value, self->drag_actor);
      break;
    case DRAG_EVENT_PROP_SOURCE:
      g_value_set_object (value, self->source);
      break;
    case DRAG_EVENT_PROP_TARGET_ACTOR:
      g_value_set_object (value, self->target_actor);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_drag_event_dispose (GObject *object)
{
  ShellDragEvent *self = SHELL_DRAG_EVENT (object);

  g_clear_object (&self->drag_actor);
  g_clear_object (&self->source);
  g_clear_object (&self->target_actor);

  G_OBJECT_CLASS (shell_drag_event_parent_class)->dispose (object);
}

static void
shell_drag_event_class_init (ShellDragEventClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_drag_event_get_property;
  object_class->dispose = shell_drag_event_dispose;

  /**
   * ShellDragEvent:x:
   *
   * The X position of the pointer in stage coordinates.
   */
  drag_event_props[DRAG_EVENT_PROP_X] =
    g_param_spec_float ("x", NULL, NULL,
                        -G_MAXFLOAT, G_MAXFLOAT, 0.f,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellDragEvent:y:
   *
   * The Y position of the pointer in stage coordinates.
   */
  drag_event_props[DRAG_EVENT_PROP_Y] =
    g_param_spec_float ("y", NULL, NULL,
                        -G_MAXFLOAT, G_MAXFLOAT, 0.f,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellDragEvent:drag-actor:
   *
   * The actor standing for the dragged item.
   */
  drag_event_props[DRAG_EVENT_PROP_DRAG_ACTOR] =
    g_param_spec_object ("drag-actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellDragEvent:source:
   *
   * The source of the drag: the dragged actor, or the #ShellXdndHandler for
   * the drag of an X11 client. %NULL if the dragged actor is gone.
   */
  drag_event_props[DRAG_EVENT_PROP_SOURCE] =
    g_param_spec_object ("source", NULL, NULL,
                         G_TYPE_OBJECT,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellDragEvent:target-actor:
   *
   * The innermost actor under the pointer. The monitor can walk its parents
   * itself.
   */
  drag_event_props[DRAG_EVENT_PROP_TARGET_ACTOR] =
    g_param_spec_object ("target-actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, DRAG_EVENT_N_PROPS,
                                     drag_event_props);
}

static void
shell_drag_event_init (ShellDragEvent *self)
{
}

static ShellDragEvent *
shell_drag_event_new (float         x,
                      float         y,
                      ClutterActor *drag_actor,
                      GObject      *source,
                      ClutterActor *target_actor)
{
  ShellDragEvent *self = g_object_new (SHELL_TYPE_DRAG_EVENT, NULL);

  self->x = x;
  self->y = y;
  self->drag_actor = drag_actor != NULL ? g_object_ref (drag_actor) : NULL;
  self->source = source != NULL ? g_object_ref (source) : NULL;
  self->target_actor = target_actor != NULL ? g_object_ref (target_actor) : NULL;

  return self;
}

static void
shell_drag_event_set_target_actor (ShellDragEvent *self,
                                   ClutterActor   *target_actor)
{
  if (self->target_actor == target_actor)
    return;

  g_set_object (&self->target_actor, target_actor);
  g_object_notify_by_pspec (G_OBJECT (self),
                            drag_event_props[DRAG_EVENT_PROP_TARGET_ACTOR]);
}

/**
 * shell_drag_event_get_x:
 * @self: a #ShellDragEvent
 *
 * Returns: the X position of the pointer in stage coordinates
 */
float
shell_drag_event_get_x (ShellDragEvent *self)
{
  g_return_val_if_fail (SHELL_IS_DRAG_EVENT (self), 0.f);

  return self->x;
}

/**
 * shell_drag_event_get_y:
 * @self: a #ShellDragEvent
 *
 * Returns: the Y position of the pointer in stage coordinates
 */
float
shell_drag_event_get_y (ShellDragEvent *self)
{
  g_return_val_if_fail (SHELL_IS_DRAG_EVENT (self), 0.f);

  return self->y;
}

/**
 * shell_drag_event_get_drag_actor:
 * @self: a #ShellDragEvent
 *
 * Returns: (transfer none): the actor standing for the dragged item
 */
ClutterActor *
shell_drag_event_get_drag_actor (ShellDragEvent *self)
{
  g_return_val_if_fail (SHELL_IS_DRAG_EVENT (self), NULL);

  return self->drag_actor;
}

/**
 * shell_drag_event_get_source:
 * @self: a #ShellDragEvent
 *
 * Returns: (transfer none) (nullable): the source of the drag
 */
GObject *
shell_drag_event_get_source (ShellDragEvent *self)
{
  g_return_val_if_fail (SHELL_IS_DRAG_EVENT (self), NULL);

  return self->source;
}

/**
 * shell_drag_event_get_target_actor:
 * @self: a #ShellDragEvent
 *
 * Returns: (transfer none) (nullable): the innermost actor under the pointer
 */
ClutterActor *
shell_drag_event_get_target_actor (ShellDragEvent *self)
{
  g_return_val_if_fail (SHELL_IS_DRAG_EVENT (self), NULL);

  return self->target_actor;
}

/* ------------------------------------------------------------------------ */
/* ShellDropEvent                                                            */
/* ------------------------------------------------------------------------ */

/**
 * ShellDropEvent:
 *
 * What a drag monitor is told when the dragged item is dropped
 * (`{dropActor, targetActor, clutterEvent}` of the JS).
 */
struct _ShellDropEvent
{
  GObject parent;

  ClutterActor *drop_actor;
  ClutterActor *target_actor;
  ClutterEvent *clutter_event;
};

enum
{
  DROP_EVENT_PROP_0,
  DROP_EVENT_PROP_DROP_ACTOR,
  DROP_EVENT_PROP_TARGET_ACTOR,
  DROP_EVENT_PROP_CLUTTER_EVENT,
  DROP_EVENT_N_PROPS
};

static GParamSpec *drop_event_props[DROP_EVENT_N_PROPS] = { NULL, };

G_DEFINE_FINAL_TYPE (ShellDropEvent, shell_drop_event, G_TYPE_OBJECT)

static void
shell_drop_event_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  ShellDropEvent *self = SHELL_DROP_EVENT (object);

  switch (prop_id)
    {
    case DROP_EVENT_PROP_DROP_ACTOR:
      g_value_set_object (value, self->drop_actor);
      break;
    case DROP_EVENT_PROP_TARGET_ACTOR:
      g_value_set_object (value, self->target_actor);
      break;
    case DROP_EVENT_PROP_CLUTTER_EVENT:
      g_value_set_boxed (value, self->clutter_event);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_drop_event_dispose (GObject *object)
{
  ShellDropEvent *self = SHELL_DROP_EVENT (object);

  g_clear_object (&self->drop_actor);
  g_clear_object (&self->target_actor);
  g_clear_pointer (&self->clutter_event, clutter_event_free);

  G_OBJECT_CLASS (shell_drop_event_parent_class)->dispose (object);
}

static void
shell_drop_event_class_init (ShellDropEventClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_drop_event_get_property;
  object_class->dispose = shell_drop_event_dispose;

  /**
   * ShellDropEvent:drop-actor:
   *
   * The actor standing for the dropped item.
   */
  drop_event_props[DROP_EVENT_PROP_DROP_ACTOR] =
    g_param_spec_object ("drop-actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellDropEvent:target-actor:
   *
   * The innermost actor under the pointer. The monitor can walk its parents
   * itself.
   */
  drop_event_props[DROP_EVENT_PROP_TARGET_ACTOR] =
    g_param_spec_object ("target-actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellDropEvent:clutter-event:
   *
   * The event that released the dragged item.
   */
  drop_event_props[DROP_EVENT_PROP_CLUTTER_EVENT] =
    g_param_spec_boxed ("clutter-event", NULL, NULL,
                        CLUTTER_TYPE_EVENT,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, DROP_EVENT_N_PROPS,
                                     drop_event_props);
}

static void
shell_drop_event_init (ShellDropEvent *self)
{
}

static ShellDropEvent *
shell_drop_event_new (ClutterActor       *drop_actor,
                      ClutterActor       *target_actor,
                      const ClutterEvent *clutter_event)
{
  ShellDropEvent *self = g_object_new (SHELL_TYPE_DROP_EVENT, NULL);

  self->drop_actor = drop_actor != NULL ? g_object_ref (drop_actor) : NULL;
  self->target_actor = target_actor != NULL ? g_object_ref (target_actor) : NULL;
  self->clutter_event = clutter_event != NULL ? clutter_event_copy (clutter_event) : NULL;

  return self;
}

/**
 * shell_drop_event_get_drop_actor:
 * @self: a #ShellDropEvent
 *
 * Returns: (transfer none): the actor standing for the dropped item
 */
ClutterActor *
shell_drop_event_get_drop_actor (ShellDropEvent *self)
{
  g_return_val_if_fail (SHELL_IS_DROP_EVENT (self), NULL);

  return self->drop_actor;
}

/**
 * shell_drop_event_get_target_actor:
 * @self: a #ShellDropEvent
 *
 * Returns: (transfer none) (nullable): the innermost actor under the pointer
 */
ClutterActor *
shell_drop_event_get_target_actor (ShellDropEvent *self)
{
  g_return_val_if_fail (SHELL_IS_DROP_EVENT (self), NULL);

  return self->target_actor;
}

/**
 * shell_drop_event_get_clutter_event:
 * @self: a #ShellDropEvent
 *
 * Returns: (transfer none) (nullable): the event that released the dragged item
 */
const ClutterEvent *
shell_drop_event_get_clutter_event (ShellDropEvent *self)
{
  g_return_val_if_fail (SHELL_IS_DROP_EVENT (self), NULL);

  return self->clutter_event;
}

/* ------------------------------------------------------------------------ */
/* ShellDragTarget                                                           */
/* ------------------------------------------------------------------------ */

G_DEFINE_INTERFACE (ShellDragTarget, shell_drag_target, G_TYPE_OBJECT)

static ShellDragMotionResult
shell_drag_target_real_handle_drag_over (ShellDragTarget *target,
                                         GObject         *source,
                                         ClutterActor    *drag_actor,
                                         float            x,
                                         float            y,
                                         uint32_t         time)
{
  return SHELL_DRAG_MOTION_RESULT_CONTINUE;
}

static gboolean
shell_drag_target_real_accept_drop (ShellDragTarget *target,
                                    GObject         *source,
                                    ClutterActor    *drag_actor,
                                    float            x,
                                    float            y,
                                    uint32_t         time)
{
  return FALSE;
}

static void
shell_drag_target_default_init (ShellDragTargetInterface *iface)
{
  iface->handle_drag_over = shell_drag_target_real_handle_drag_over;
  iface->accept_drop = shell_drag_target_real_accept_drop;
}

/**
 * shell_drag_target_handle_drag_over:
 * @target: a #ShellDragTarget
 * @source: (transfer none) (nullable): the source of the drag
 * @drag_actor: (transfer none): the actor standing for the dragged item
 * @x: the X position of the pointer in the coordinates of @target's actor
 * @y: the Y position of the pointer in the coordinates of @target's actor
 * @time: the time of the event, 0 if it is not known
 *
 * Called when a dragged item is over the actor of @target or one of its
 * descendants, from the innermost actor to the stage until one target does
 * not answer %SHELL_DRAG_MOTION_RESULT_CONTINUE (`handleDragOver ()`).
 * @source is the dragged actor, or the #ShellXdndHandler for the drag of an
 * X11 client.
 *
 * Returns: how the drop would be handled by @target
 */
ShellDragMotionResult
shell_drag_target_handle_drag_over (ShellDragTarget *target,
                                    GObject         *source,
                                    ClutterActor    *drag_actor,
                                    float            x,
                                    float            y,
                                    uint32_t         time)
{
  g_return_val_if_fail (SHELL_IS_DRAG_TARGET (target),
                        SHELL_DRAG_MOTION_RESULT_CONTINUE);

  return SHELL_DRAG_TARGET_GET_IFACE (target)->handle_drag_over (target,
                                                                 source,
                                                                 drag_actor,
                                                                 x, y, time);
}

/**
 * shell_drag_target_accept_drop:
 * @target: a #ShellDragTarget
 * @source: (transfer none) (nullable): the source of the drag
 * @drag_actor: (transfer none): the actor standing for the dragged item
 * @x: the X position of the pointer in the coordinates of @target's actor
 * @y: the Y position of the pointer in the coordinates of @target's actor
 * @time: the time of the event
 *
 * Called when the dragged item is dropped over the actor of @target or one of
 * its descendants, from the innermost actor to the stage until one target
 * accepts (`acceptDrop ()`). If @target returns %TRUE without taking
 * @drag_actor off the ui group, the drag destroys it.
 *
 * Returns: %TRUE if @target accepted the drop
 */
gboolean
shell_drag_target_accept_drop (ShellDragTarget *target,
                               GObject         *source,
                               ClutterActor    *drag_actor,
                               float            x,
                               float            y,
                               uint32_t         time)
{
  g_return_val_if_fail (SHELL_IS_DRAG_TARGET (target), FALSE);

  return SHELL_DRAG_TARGET_GET_IFACE (target)->accept_drop (target,
                                                            source,
                                                            drag_actor,
                                                            x, y, time);
}

/* ------------------------------------------------------------------------ */
/* ShellDragSource                                                           */
/* ------------------------------------------------------------------------ */

G_DEFINE_INTERFACE (ShellDragSource, shell_drag_source, G_TYPE_OBJECT)

static ClutterActor *
shell_drag_source_real_get_drag_actor (ShellDragSource *source)
{
  return NULL;
}

static ClutterActor *
shell_drag_source_real_get_drag_actor_source (ShellDragSource *source)
{
  return NULL;
}

static ShellApp *
shell_drag_source_real_get_app (ShellDragSource *source)
{
  return NULL;
}

static void
shell_drag_source_real_animate_launch_at_pos (ShellDragSource *source,
                                              float            x,
                                              float            y)
{
}

static void
shell_drag_source_default_init (ShellDragSourceInterface *iface)
{
  iface->get_drag_actor = shell_drag_source_real_get_drag_actor;
  iface->get_drag_actor_source = shell_drag_source_real_get_drag_actor_source;
  iface->get_app = shell_drag_source_real_get_app;
  iface->animate_launch_at_pos = shell_drag_source_real_animate_launch_at_pos;
}

/**
 * shell_drag_source_get_drag_actor:
 * @source: a #ShellDragSource
 *
 * Called when the drag of @source starts, to get the actor which is dragged
 * in its place (`getDragActor ()`). The drag adds it to the ui group and
 * destroys it when the drag ends.
 *
 * Returns: (transfer full) (nullable): a new actor, or %NULL to drag the
 *   actor of @source itself
 */
ClutterActor *
shell_drag_source_get_drag_actor (ShellDragSource *source)
{
  g_return_val_if_fail (SHELL_IS_DRAG_SOURCE (source), NULL);

  return SHELL_DRAG_SOURCE_GET_IFACE (source)->get_drag_actor (source);
}

/**
 * shell_drag_source_get_drag_actor_source:
 * @source: a #ShellDragSource
 *
 * The drag actor does not always have to be the same as the actor of the
 * source: for example it can be an image that is part of that actor. To
 * "snap back" correctly the drag needs to know which actor the drag actor
 * stands for (`getDragActorSource ()`). The drag actor starts over it if the
 * drag started on it, and snaps back to it if it is visible.
 *
 * Returns: (transfer none) (nullable): the actor which the drag actor
 *   stands for, or %NULL for the actor of @source
 */
ClutterActor *
shell_drag_source_get_drag_actor_source (ShellDragSource *source)
{
  g_return_val_if_fail (SHELL_IS_DRAG_SOURCE (source), NULL);

  return SHELL_DRAG_SOURCE_GET_IFACE (source)->get_drag_actor_source (source);
}

/**
 * shell_drag_source_get_app:
 * @source: a #ShellDragSource
 *
 * The application the dragged item stands for, if it is an application
 * launcher (`source.app` of the duck-typed JS protocol). The workspaces of the
 * overview accept such a drop to open a new window of the application.
 *
 * Returns: (transfer none) (nullable): the application, or %NULL
 */
ShellApp *
shell_drag_source_get_app (ShellDragSource *source)
{
  g_return_val_if_fail (SHELL_IS_DRAG_SOURCE (source), NULL);

  return SHELL_DRAG_SOURCE_GET_IFACE (source)->get_app (source);
}

/**
 * shell_drag_source_animate_launch_at_pos:
 * @source: a #ShellDragSource
 * @x: the X position of the drag actor in the stage
 * @y: the Y position of the drag actor in the stage
 *
 * Called after an application launcher was dropped to launch a new window
 * of its application, to animate the launch from the position of the drop
 * (`source.animateLaunchAtPos ()`). The default implementation does nothing.
 */
void
shell_drag_source_animate_launch_at_pos (ShellDragSource *source,
                                         float            x,
                                         float            y)
{
  g_return_if_fail (SHELL_IS_DRAG_SOURCE (source));

  SHELL_DRAG_SOURCE_GET_IFACE (source)->animate_launch_at_pos (source, x, y);
}

/* ------------------------------------------------------------------------ */
/* Drop targets                                                              */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellDragTarget *target;
} DragTargetRegistration;

static void
drag_target_registration_free (DragTargetRegistration *registration)
{
  g_clear_object (&registration->target);
  g_free (registration);
}

static void
registered_actor_destroyed_cb (ClutterActor *actor,
                               gpointer      user_data)
{
  DragTargetRegistration *registration =
    g_object_get_data (G_OBJECT (actor), DRAG_TARGET_KEY);

  /* The registration keeps a strong reference to the target, which usually
   * refers to the actor in turn: let go of it as soon as the actor is
   * destroyed, like the JS garbage collector did */
  if (registration != NULL)
    g_clear_object (&registration->target);
}

/**
 * shell_dnd_set_drag_target:
 * @actor: an actor
 * @target: (nullable): the target of the drops on @actor, or %NULL to
 *   remove it
 *
 * Makes @target handle the drags over @actor and its descendants, as if
 * @actor were @target (`actor._delegate = target`). It is only needed when
 * the drop target is not an actor, or is not @actor; an actor which
 * implements #ShellDragTarget is its own target. If @actor has a registered
 * target it is used instead of @actor, even if @actor implements the
 * interface itself.
 *
 * The registration is dropped when @actor is destroyed. Registering @actor
 * itself only removes the registration.
 */
void
shell_dnd_set_drag_target (ClutterActor    *actor,
                           ShellDragTarget *target)
{
  DragTargetRegistration *registration;

  g_return_if_fail (CLUTTER_IS_ACTOR (actor));
  g_return_if_fail (target == NULL || SHELL_IS_DRAG_TARGET (target));

  if ((gpointer) target == (gpointer) actor)
    target = NULL;

  registration = g_object_get_data (G_OBJECT (actor), DRAG_TARGET_KEY);
  if (registration == NULL)
    {
      if (target == NULL)
        return;

      registration = g_new0 (DragTargetRegistration, 1);
      g_object_set_data_full (G_OBJECT (actor), DRAG_TARGET_KEY, registration,
                              (GDestroyNotify) drag_target_registration_free);
      g_signal_connect (actor, "destroy",
                        G_CALLBACK (registered_actor_destroyed_cb), NULL);
    }

  g_set_object (&registration->target, target);
}

/**
 * shell_dnd_get_drag_target:
 * @actor: an actor
 *
 * Gets the target of the drags over @actor (without looking at its
 * ancestors): the one registered with shell_dnd_set_drag_target (), or else
 * @actor itself if it implements #ShellDragTarget.
 *
 * Returns: (transfer none) (nullable): the target
 */
ShellDragTarget *
shell_dnd_get_drag_target (ClutterActor *actor)
{
  DragTargetRegistration *registration;

  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), NULL);

  registration = g_object_get_data (G_OBJECT (actor), DRAG_TARGET_KEY);
  if (registration != NULL && registration->target != NULL)
    return registration->target;

  if (SHELL_IS_DRAG_TARGET (actor))
    return SHELL_DRAG_TARGET (actor);

  return NULL;
}

/* ------------------------------------------------------------------------ */
/* Drag monitors                                                             */
/* ------------------------------------------------------------------------ */

typedef struct
{
  guint ref_count;
  guint id;
  gboolean removed;

  ShellDragMotionFunc motion;
  gpointer motion_data;
  GDestroyNotify motion_destroy;

  ShellDragDropFunc drop;
  gpointer drop_data;
  GDestroyNotify drop_destroy;
} DragMonitor;

/* `dragMonitors`, in the order they were added */
static GList *drag_monitors = NULL;
static guint last_monitor_id = 0;

static DragMonitor *
drag_monitor_ref (DragMonitor *monitor)
{
  monitor->ref_count++;
  return monitor;
}

static void
drag_monitor_unref (DragMonitor *monitor)
{
  if (--monitor->ref_count > 0)
    return;

  if (monitor->motion_destroy != NULL)
    monitor->motion_destroy (monitor->motion_data);
  if (monitor->drop_destroy != NULL)
    monitor->drop_destroy (monitor->drop_data);

  g_free (monitor);
}

static gpointer
drag_monitor_ref_copy_func (gconstpointer src,
                            gpointer      data)
{
  return drag_monitor_ref ((DragMonitor *) src);
}

static GList *
snapshot_drag_monitors (void)
{
  return g_list_copy_deep (drag_monitors, drag_monitor_ref_copy_func, NULL);
}

/**
 * shell_dnd_add_drag_monitor:
 * @motion: (nullable) (scope notified) (closure motion_data) (destroy motion_destroy):
 *   called when the dragged item moves
 * @motion_data: user data for @motion
 * @motion_destroy: (nullable): called on @motion_data when the monitor is
 *   removed
 * @drop: (nullable) (scope notified) (closure drop_data) (destroy drop_destroy):
 *   called when the dragged item is dropped
 * @drop_data: user data for @drop
 * @drop_destroy: (nullable): called on @drop_data when the monitor is removed
 *
 * Adds a drag monitor (`DND.addDragMonitor ({dragMotion, dragDrop})`): it
 * sees every drag of the shell, before the drop targets, in the order the
 * monitors were added. Either callback can be %NULL.
 *
 * Returns: an id for shell_dnd_remove_drag_monitor (), never 0
 */
guint
shell_dnd_add_drag_monitor (ShellDragMotionFunc motion,
                            gpointer            motion_data,
                            GDestroyNotify      motion_destroy,
                            ShellDragDropFunc   drop,
                            gpointer            drop_data,
                            GDestroyNotify      drop_destroy)
{
  DragMonitor *monitor = g_new0 (DragMonitor, 1);

  monitor->ref_count = 1;
  monitor->id = ++last_monitor_id;
  monitor->motion = motion;
  monitor->motion_data = motion_data;
  monitor->motion_destroy = motion_destroy;
  monitor->drop = drop;
  monitor->drop_data = drop_data;
  monitor->drop_destroy = drop_destroy;

  drag_monitors = g_list_append (drag_monitors, monitor);

  return monitor->id;
}

/**
 * shell_dnd_remove_drag_monitor:
 * @id: the id given by shell_dnd_add_drag_monitor (), or 0
 *
 * Removes a drag monitor (`DND.removeDragMonitor ()`). It is not an error to
 * remove a monitor that is gone already, or 0.
 */
void
shell_dnd_remove_drag_monitor (guint id)
{
  GList *l;

  for (l = drag_monitors; l != NULL; l = l->next)
    {
      DragMonitor *monitor = l->data;

      if (monitor->id != id)
        continue;

      drag_monitors = g_list_delete_link (drag_monitors, l);
      monitor->removed = TRUE;
      drag_monitor_unref (monitor);
      return;
    }
}

/* Calls the motion function of the monitors until one answers something
 * else than CONTINUE. Returns whether one did, and its answer in @result. */
static gboolean
run_motion_monitors (ShellDragEvent        *event,
                     ShellDragMotionResult *result)
{
  GList *snapshot = snapshot_drag_monitors ();
  gboolean handled = FALSE;
  GList *l;

  for (l = snapshot; l != NULL && !handled; l = l->next)
    {
      DragMonitor *monitor = l->data;

      if (monitor->removed || monitor->motion == NULL)
        continue;

      *result = monitor->motion (event, monitor->motion_data);
      if (*result != SHELL_DRAG_MOTION_RESULT_CONTINUE)
        handled = TRUE;
    }

  g_list_free_full (snapshot, (GDestroyNotify) drag_monitor_unref);

  return handled;
}

/* Calls the drop function of the monitors until one takes the drop over.
 * Returns whether one did. */
static gboolean
run_drop_monitors (ShellDropEvent *event)
{
  GList *snapshot = snapshot_drag_monitors ();
  gboolean handled = FALSE;
  GList *l;

  for (l = snapshot; l != NULL && !handled; l = l->next)
    {
      DragMonitor *monitor = l->data;

      if (monitor->removed || monitor->drop == NULL)
        continue;

      switch (monitor->drop (event, monitor->drop_data))
        {
        case SHELL_DRAG_DROP_RESULT_FAILURE:
        case SHELL_DRAG_DROP_RESULT_SUCCESS:
          handled = TRUE;
          break;

        case SHELL_DRAG_DROP_RESULT_CONTINUE:
        default:
          break;
        }
    }

  g_list_free_full (snapshot, (GDestroyNotify) drag_monitor_unref);

  return handled;
}

/* ------------------------------------------------------------------------ */
/* ShellDraggable                                                            */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,
  PROP_ACTOR,
  PROP_MANUAL_MODE,
  PROP_TIMEOUT_THRESHOLD,
  PROP_RESTORE_ON_SUCCESS,
  PROP_DRAG_ACTOR_MAX_SIZE,
  PROP_DRAG_ACTOR_OPACITY,
  PROP_START_GESTURE,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  DRAG_BEGIN,
  DRAG_CANCELLED,
  DRAG_END,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct _ShellDraggable
{
  GObject parent;

  /* Weak: the actor owns the draggable. Strongly held (actor_held) while
   * the drag lasts */
  ClutterActor *actor;
  ClutterActor *actor_held;

  StDndStartGesture *dnd_gesture;

  gboolean manual_mode;
  guint timeout_threshold;
  gboolean restore_on_success;
  int drag_actor_max_size;
  int drag_actor_opacity;

  /* The drag is over and the item is in the process of animating to its
   * original position (snapping back or reverting) */
  gboolean animation_in_progress;
  gboolean drag_cancellable;

  ClutterGrab *grab;
  ClutterSprite *sprite;

  float drag_start_x;
  float drag_start_y;
  float drag_x;
  float drag_y;
  float drag_offset_x;
  float drag_offset_y;

  ClutterActor *drag_actor;
  gulong drag_actor_destroy_id;
  guint8 drag_orig_opacity;
  float snap_back_x;
  float snap_back_y;
  float snap_back_scale;

  /* %NULL if the actor is not dragged itself */
  ClutterActor *drag_actor_source;
  gulong drag_actor_source_destroy_id;

  /* Only if the actor is dragged itself: where to put it back */
  ClutterActor *drag_orig_parent;
  gulong drag_orig_parent_destroy_id;
  gboolean drag_actor_had_fixed_pos;
  gboolean drag_actor_had_nat_width;
  gboolean drag_actor_had_nat_height;
  float drag_orig_x;
  float drag_orig_y;
  float drag_orig_width;
  float drag_orig_height;
  float drag_orig_scale;

  guint update_hover_id;
};

G_DEFINE_FINAL_TYPE (ShellDraggable, shell_draggable, G_TYPE_OBJECT)

/* `currentDraggable` and `eventHandlerActor` */
static ShellDraggable *current_draggable = NULL;
static ClutterActor *event_handler_actor = NULL;

static void drag_complete (ShellDraggable *self);
static void cancel_drag   (ShellDraggable *self,
                           uint32_t        event_time);

static gboolean
event_handler_event_cb (ClutterActor *actor,
                        ClutterEvent *event,
                        gpointer      user_data);

static void
event_handler_destroyed_cb (ClutterActor *actor,
                            gpointer      user_data)
{
  event_handler_actor = NULL;
}

static ClutterActor *
get_event_handler_actor (void)
{
  if (event_handler_actor == NULL)
    {
      ClutterActor *ui_group = get_ui_group ();

      g_return_val_if_fail (ui_group != NULL, NULL);

      event_handler_actor = g_object_new (CLUTTER_TYPE_ACTOR,
                                          "width", 0.f,
                                          "height", 0.f,
                                          "reactive", TRUE,
                                          NULL);
      clutter_actor_add_child (ui_group, event_handler_actor);

      /* We connect to 'event' rather than 'captured-event' because the
       * capturing phase doesn't happen when you've grabbed the pointer. */
      g_signal_connect (event_handler_actor, "event",
                        G_CALLBACK (event_handler_event_cb), NULL);
      g_signal_connect (event_handler_actor, "destroy",
                        G_CALLBACK (event_handler_destroyed_cb), NULL);
    }

  return event_handler_actor;
}

static void
grab_events (ShellDraggable *self,
             ClutterSprite  *sprite)
{
  ClutterGrab *grab = shell_main_push_modal (shell_main_get_default (),
                                             get_event_handler_actor (),
                                             SHELL_ACTION_MODE_NONE);

  g_set_object (&self->grab, grab);
  g_set_object (&self->sprite, sprite);
}

static void
ungrab_events (ShellDraggable *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (ClutterGrab) grab = g_steal_pointer (&self->grab);

  g_clear_object (&self->sprite);

  if (!shell_main_pop_modal (shell_main_get_default (), grab, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

static ClutterActor *
pick_target_actor (ShellDraggable *self)
{
  ClutterActor *stage = NULL;

  if (self->drag_actor != NULL)
    stage = clutter_actor_get_stage (self->drag_actor);
  if (stage == NULL)
    stage = CLUTTER_ACTOR (get_stage ());

  return clutter_stage_get_actor_at_pos (CLUTTER_STAGE (stage),
                                         CLUTTER_PICK_ALL,
                                         self->drag_x, self->drag_y);
}

static void
update_actor_position (ShellDraggable *self,
                       float           orig_scale,
                       float           orig_drag_offset_x,
                       float           orig_drag_offset_y,
                       float           trans_x,
                       float           trans_y)
{
  float current_scale;

  if (self->drag_actor == NULL)
    return;

  current_scale = get_scale_x (self->drag_actor) / orig_scale;

  self->drag_offset_x = current_scale * orig_drag_offset_x - trans_x;
  self->drag_offset_y = current_scale * orig_drag_offset_y - trans_y;
  clutter_actor_set_position (self->drag_actor,
                              self->drag_x + self->drag_offset_x,
                              self->drag_y + self->drag_offset_y);
}

/* The position of the actor changes as we scale around the drag position,
 * but we can't just tween to the final position because that tween would
 * fight with updates as the user continues dragging the mouse; instead we do
 * the position computations in a ::new-frame handler. */
typedef struct
{
  ShellDraggable *draggable;
  float orig_scale;
  float orig_drag_offset_x;
  float orig_drag_offset_y;
  float trans_x;
  float trans_y;
} ScaleData;

static ScaleData *
scale_data_ref (ScaleData *data)
{
  return g_rc_box_acquire (data);
}

static void
scale_data_clear (ScaleData *data)
{
  g_clear_object (&data->draggable);
}

static void
scale_data_unref (ScaleData *data)
{
  g_rc_box_release_full (data, (GDestroyNotify) scale_data_clear);
}

static void
scale_closure_notify (gpointer  data,
                      GClosure *closure)
{
  scale_data_unref (data);
}

static void
scale_done_cb (gboolean finished,
               gpointer user_data)
{
  ScaleData *data = user_data;

  if (finished)
    update_actor_position (data->draggable, data->orig_scale,
                           data->orig_drag_offset_x, data->orig_drag_offset_y,
                           data->trans_x, data->trans_y);
}

static void
scale_new_frame_cb (ClutterTimeline *timeline,
                    int              elapsed_msecs,
                    gpointer         user_data)
{
  ScaleData *data = user_data;

  update_actor_position (data->draggable, data->orig_scale,
                         data->orig_drag_offset_x, data->orig_drag_offset_y,
                         data->trans_x, data->trans_y);
}

static void
update_drag_hover_stop (ShellDraggable *self)
{
  g_clear_handle_id (&self->update_hover_id, g_source_remove);
}

static void
drag_actor_destroyed_cb (ClutterActor   *actor,
                         ShellDraggable *self)
{
  g_autoptr (ShellDraggable) keep = g_object_ref (self);

  /* Cancel ongoing animation (if any). The handler of the actor is still
   * connected when this completes the drag: it is disconnecting itself. */
  if (self->animation_in_progress)
    {
      self->animation_in_progress = FALSE;
      drag_complete (self);
      update_cursor (CLUTTER_CURSOR_DEFAULT);
    }

  g_clear_object (&self->drag_actor);
  self->drag_actor_destroy_id = 0;

  if (self->drag_cancellable)
    cancel_drag (self, shell_global_get_current_time (shell_global_get ()));
}

static void
drag_actor_source_destroyed_cb (ClutterActor   *actor,
                                ShellDraggable *self)
{
  g_clear_object (&self->drag_actor_source);
  self->drag_actor_source_destroy_id = 0;
}

static void
drag_orig_parent_destroyed_cb (ClutterActor   *actor,
                               ShellDraggable *self)
{
  g_clear_object (&self->drag_orig_parent);
  self->drag_orig_parent_destroy_id = 0;
}

static void
gesture_recognized_cb (ClutterGesture *gesture,
                       ShellDraggable *self)
{
  g_autoptr (ShellDraggable) keep = g_object_ref (self);
  ClutterActor *actor = self->actor;
  ClutterActor *ui_group = get_ui_group ();
  const ClutterEvent *point_begin_event;
  const ClutterEvent *triggering_event;
  ClutterBackend *backend;
  ClutterSprite *sprite;
  float stage_x, stage_y;
  float scaled_width, scaled_height;
  float orig_drag_offset_x, orig_drag_offset_y;
  float trans_x, trans_y;
  uint32_t event_time;

  g_return_if_fail (actor != NULL);
  g_return_if_fail (ui_group != NULL);

  /* A drag of this actor is going on (still animating back): the JS started a
   * second one on top of it, this leaves the first one alone */
  if (self->grab != NULL)
    return;

  point_begin_event =
    st_dnd_start_gesture_get_point_begin_event (self->dnd_gesture);
  clutter_event_get_coords (point_begin_event,
                            &self->drag_start_x, &self->drag_start_y);

  triggering_event =
    st_dnd_start_gesture_get_drag_triggering_event (self->dnd_gesture);
  clutter_event_get_coords (triggering_event, &stage_x, &stage_y);
  backend = clutter_context_get_backend (clutter_actor_get_context (actor));
  sprite = clutter_backend_get_sprite (backend, get_stage (),
                                       triggering_event);
  event_time = clutter_event_get_time (triggering_event);

  g_set_object (&current_draggable, self);

  grab_events (self, sprite);
  if (self->grab == NULL)
    return;

  /* Keep the actor around until the drag is over, whatever happens to it */
  g_set_object (&self->actor_held, actor);

  self->animation_in_progress = FALSE;
  self->drag_cancellable = TRUE;

  g_signal_emit (self, signals[DRAG_BEGIN], 0, event_time);
  update_cursor (CLUTTER_CURSOR_NO_DROP);

  self->drag_x = stage_x;
  self->drag_y = stage_y;

  if (SHELL_IS_DRAG_SOURCE (actor) &&
      (self->drag_actor = shell_drag_source_get_drag_actor (SHELL_DRAG_SOURCE (actor))) != NULL)
    {
      ClutterActor *source;

      clutter_actor_add_child (ui_group, self->drag_actor);
      clutter_actor_set_child_above_sibling (ui_group, self->drag_actor, NULL);
      shell_util_set_hidden_from_pick (self->drag_actor, TRUE);

      /* Drag actor does not always have to be the same as actor. For example
       * drag actor can be an image that's part of the actor. So to perform
       * "snap back" correctly we need to know what was the drag actor
       * source. */
      source = shell_drag_source_get_drag_actor_source (SHELL_DRAG_SOURCE (actor));
      if (source != NULL)
        {
          float source_x, source_y, x, y;

          /* If the user dragged from the source, then position the
           * dragActor over it. Otherwise, center it around the pointer */
          clutter_actor_get_transformed_position (source, &source_x, &source_y);
          if (self->drag_start_x > source_x &&
              self->drag_start_x <= source_x + clutter_actor_get_width (self->drag_actor) &&
              self->drag_start_y > source_y &&
              self->drag_start_y <= source_y + clutter_actor_get_height (self->drag_actor))
            {
              x = source_x;
              y = source_y;
            }
          else
            {
              x = self->drag_start_x - clutter_actor_get_width (self->drag_actor) / 2;
              y = self->drag_start_y - clutter_actor_get_height (self->drag_actor) / 2;
            }
          clutter_actor_set_position (self->drag_actor, x, y);
        }
      else
        {
          source = actor;
        }

      self->drag_actor_source = g_object_ref (source);
      self->drag_actor_source_destroy_id =
        g_signal_connect (source, "destroy",
                          G_CALLBACK (drag_actor_source_destroyed_cb), self);

      g_clear_object (&self->drag_orig_parent);

      self->drag_offset_x = clutter_actor_get_x (self->drag_actor) - self->drag_start_x;
      self->drag_offset_y = clutter_actor_get_y (self->drag_actor) - self->drag_start_y;

      clutter_actor_get_transformed_size (self->drag_actor,
                                          &scaled_width, &scaled_height);
    }
  else
    {
      graphene_rect_t transformed_extents;
      ClutterActorBox allocation;
      float scale_x, scale_y;

      /* An actor which is dragged itself has to be put back where it came
       * from */
      if (clutter_actor_get_parent (actor) == NULL)
        {
          g_warning ("%s: %s %p has no parent and does not provide a drag actor",
                     G_STRFUNC, G_OBJECT_TYPE_NAME (actor), actor);
          update_cursor (CLUTTER_CURSOR_DEFAULT);
          drag_complete (self);
          g_signal_emit (self, signals[DRAG_END], 0, event_time, FALSE);
          return;
        }

      self->drag_actor = g_object_ref (actor);

      g_clear_object (&self->drag_actor_source);
      self->drag_orig_parent = g_object_ref (clutter_actor_get_parent (actor));

      clutter_actor_get_allocation_box (actor, &allocation);

      self->drag_actor_had_fixed_pos = clutter_actor_get_fixed_position_set (actor);
      self->drag_orig_x = allocation.x1;
      self->drag_orig_y = allocation.y1;
      g_object_get (actor,
                    "natural-width-set", &self->drag_actor_had_nat_width,
                    "natural-height-set", &self->drag_actor_had_nat_height,
                    NULL);
      self->drag_orig_width = clutter_actor_box_get_width (&allocation);
      self->drag_orig_height = clutter_actor_box_get_height (&allocation);
      self->drag_orig_scale = get_scale_x (actor);

      /* Ensure actors with an allocation smaller than their natural size
       * retain their size */
      clutter_actor_set_size (actor,
                              self->drag_orig_width, self->drag_orig_height);

      clutter_actor_get_transformed_extents (actor, &transformed_extents);

      self->drag_offset_x = transformed_extents.origin.x - self->drag_start_x;
      self->drag_offset_y = transformed_extents.origin.y - self->drag_start_y;

      scaled_width = transformed_extents.size.width;
      scaled_height = transformed_extents.size.height;

      scale_x = scaled_width / self->drag_orig_width;
      scale_y = scaled_height / self->drag_orig_height;
      clutter_actor_set_scale (actor, scale_x, scale_y);

      clutter_actor_remove_child (self->drag_orig_parent, actor);
      clutter_actor_add_child (ui_group, actor);
      clutter_actor_set_child_above_sibling (ui_group, actor, NULL);
      shell_util_set_hidden_from_pick (actor, TRUE);

      self->drag_orig_parent_destroy_id =
        g_signal_connect (self->drag_orig_parent, "destroy",
                          G_CALLBACK (drag_orig_parent_destroyed_cb), self);
    }

  self->drag_actor_destroy_id =
    g_signal_connect (self->drag_actor, "destroy",
                      G_CALLBACK (drag_actor_destroyed_cb), self);
  self->drag_orig_opacity = clutter_actor_get_opacity (self->drag_actor);
  if (self->drag_actor_opacity >= 0)
    clutter_actor_set_opacity (self->drag_actor, self->drag_actor_opacity);

  self->snap_back_x = self->drag_start_x + self->drag_offset_x;
  self->snap_back_y = self->drag_start_y + self->drag_offset_y;
  self->snap_back_scale = get_scale_x (self->drag_actor);

  orig_drag_offset_x = self->drag_offset_x;
  orig_drag_offset_y = self->drag_offset_y;
  clutter_actor_get_translation (self->drag_actor, &trans_x, &trans_y, NULL);
  self->drag_offset_x -= trans_x;
  self->drag_offset_y -= trans_y;

  clutter_actor_set_position (self->drag_actor,
                              self->drag_x + self->drag_offset_x,
                              self->drag_y + self->drag_offset_y);

  if (self->drag_actor_max_size >= 0)
    {
      float current_size = MAX (scaled_width, scaled_height);

      if (current_size > self->drag_actor_max_size)
        {
          ShellEaseParams params = {
            .duration = SHELL_DND_SCALE_ANIMATION_TIME,
            .delay = -1,
            .mode = CLUTTER_EASE_OUT_QUAD,
            .repeat_count = 0,
            .auto_reverse = FALSE,
            .animation_required = FALSE,
          };
          ScaleData *data;
          ClutterTransition *transition;
          float scale = self->drag_actor_max_size / current_size;
          float orig_scale = get_scale_x (self->drag_actor);

          data = g_rc_box_new0 (ScaleData);
          data->draggable = g_object_ref (self);
          data->orig_scale = orig_scale;
          data->orig_drag_offset_x = orig_drag_offset_x;
          data->orig_drag_offset_y = orig_drag_offset_y;
          data->trans_x = trans_x;
          data->trans_y = trans_y;

          shell_actor_ease (self->drag_actor, &params,
                            scale_done_cb, scale_data_ref (data),
                            (GDestroyNotify) scale_data_unref,
                            "scale-x", (double) (scale * orig_scale),
                            "scale-y", (double) (scale * orig_scale),
                            NULL);

          /* The ease completed synchronously if animations are disabled, and
           * may even have destroyed the drag actor */
          transition = self->drag_actor != NULL ?
            clutter_actor_get_transition (self->drag_actor, "scale-x") : NULL;
          if (transition != NULL)
            g_signal_connect_data (transition, "new-frame",
                                   G_CALLBACK (scale_new_frame_cb),
                                   scale_data_ref (data),
                                   scale_closure_notify, 0);

          scale_data_unref (data);
        }
    }
}

typedef struct
{
  ShellDraggable *draggable;
  ShellDragEvent *event;
  gulong destroy_id;
} HoverState;

static void
hover_target_destroyed_cb (ClutterActor *target,
                           HoverState   *state)
{
  ClutterActor *new_target = pick_target_actor (state->draggable);

  /* The destroyed actor goes away with its handler */
  state->destroy_id = 0;
  shell_drag_event_set_target_actor (state->event, new_target);
  state->destroy_id = g_signal_connect (new_target, "destroy",
                                        G_CALLBACK (hover_target_destroyed_cb),
                                        state);
}

static void
hover_state_stop (HoverState *state)
{
  ClutterActor *target = state->event->target_actor;

  if (state->destroy_id != 0 && target != NULL)
    g_signal_handler_disconnect (target, state->destroy_id);
  state->destroy_id = 0;
}

static void
update_drag_hover (ShellDraggable *self)
{
  g_autoptr (ShellDraggable) keep = g_object_ref (self);
  g_autoptr (ShellDragEvent) event = NULL;
  g_autoptr (ClutterActor) target = NULL;
  ShellDragMotionResult result;
  HoverState state;

  if (self->drag_actor == NULL)
    return;

  target = g_object_ref (pick_target_actor (self));
  event = shell_drag_event_new (self->drag_x, self->drag_y, self->drag_actor,
                                G_OBJECT (self->actor_held), target);

  state.draggable = self;
  state.event = event;
  state.destroy_id = g_signal_connect (target, "destroy",
                                       G_CALLBACK (hover_target_destroyed_cb),
                                       &state);

  if (run_motion_monitors (event, &result))
    {
      update_cursor (cursor_for_motion_result (result));
      hover_state_stop (&state);
      return;
    }
  hover_state_stop (&state);

  /* The target the destroy handler picked, if the old one went away */
  g_set_object (&target, event->target_actor);

  while (target != NULL)
    {
      ShellDragTarget *drag_target = shell_dnd_get_drag_target (target);

      /* A monitor or a target can end the drag */
      if (self->drag_actor == NULL)
        return;

      if (drag_target != NULL)
        {
          float target_x, target_y;

          clutter_actor_transform_stage_point (target,
                                               self->drag_x, self->drag_y,
                                               &target_x, &target_y);

          /* We currently loop through all parents on drag-over even if one of
           * the children has handled it. We can check the return value of the
           * function and break the loop if it's true if we don't want to
           * continue checking the parents. */
          result = shell_drag_target_handle_drag_over (drag_target,
                                                       G_OBJECT (self->actor_held),
                                                       self->drag_actor,
                                                       target_x, target_y,
                                                       0);
          if (result != SHELL_DRAG_MOTION_RESULT_CONTINUE)
            {
              update_cursor (cursor_for_motion_result (result));
              return;
            }
        }

      g_set_object (&target, clutter_actor_get_parent (target));
    }

  update_cursor (CLUTTER_CURSOR_NO_DROP);
}

static void
update_drag_hover_idle_cb (gpointer user_data)
{
  ShellDraggable *self = user_data;

  self->update_hover_id = 0;
  update_drag_hover (self);
}

static void
queue_update_drag_hover (ShellDraggable *self)
{
  if (self->update_hover_id != 0)
    return;

  self->update_hover_id = g_idle_add_once (update_drag_hover_idle_cb, self);
  g_source_set_name_by_id (self->update_hover_id,
                           "[gnome-shell] this._updateDragHover");
}

static void
update_drag_position (ShellDraggable     *self,
                      const ClutterEvent *event)
{
  float stage_x, stage_y;

  if (self->drag_actor == NULL)
    return;

  clutter_event_get_coords (event, &stage_x, &stage_y);
  self->drag_x = stage_x;
  self->drag_y = stage_y;
  clutter_actor_set_position (self->drag_actor,
                              stage_x + self->drag_offset_x,
                              stage_y + self->drag_offset_y);

  queue_update_drag_hover (self);
}

static void
get_restore_location (ShellDraggable *self,
                      float          *x,
                      float          *y,
                      float          *scale)
{
  if (self->drag_actor_source != NULL &&
      clutter_actor_is_visible (self->drag_actor_source))
    {
      float source_scaled_width;

      /* Snap the clone back to its source */
      clutter_actor_get_transformed_position (self->drag_actor_source, x, y);
      clutter_actor_get_transformed_size (self->drag_actor_source,
                                          &source_scaled_width, NULL);
      *scale = source_scaled_width != 0.f ?
        source_scaled_width / clutter_actor_get_width (self->drag_actor) : 0.f;
    }
  else if (self->drag_orig_parent != NULL)
    {
      float parent_x, parent_y, parent_scale;

      /* Snap the actor back to its original position within its parent,
       * adjusting for the fact that the parent may have been moved or
       * scaled */
      clutter_actor_get_transformed_position (self->drag_orig_parent,
                                              &parent_x, &parent_y);
      parent_scale = get_real_actor_scale (self->drag_orig_parent);

      *x = parent_x + parent_scale * self->drag_orig_x;
      *y = parent_y + parent_scale * self->drag_orig_y;
      *scale = self->drag_orig_scale * parent_scale;
    }
  else
    {
      /* Snap back actor to its original stage position */
      *x = self->snap_back_x;
      *y = self->snap_back_y;
      *scale = self->snap_back_scale;
    }
}

static void
finish_animation (ShellDraggable *self)
{
  if (!self->animation_in_progress)
    return;

  self->animation_in_progress = FALSE;
  drag_complete (self);

  update_cursor (CLUTTER_CURSOR_DEFAULT);
}

static void
on_animation_complete (ShellDraggable *self,
                       uint32_t        event_time)
{
  g_autoptr (ShellDraggable) keep = g_object_ref (self);

  if (self->drag_orig_parent != NULL && self->drag_actor != NULL)
    {
      ClutterActor *ui_group = get_ui_group ();

      if (clutter_actor_get_parent (self->drag_actor) == ui_group)
        clutter_actor_remove_child (ui_group, self->drag_actor);
      clutter_actor_add_child (self->drag_orig_parent, self->drag_actor);
      clutter_actor_set_scale (self->drag_actor,
                               self->drag_orig_scale, self->drag_orig_scale);
      if (self->drag_actor_had_fixed_pos)
        clutter_actor_set_position (self->drag_actor,
                                    self->drag_orig_x, self->drag_orig_y);
      else
        clutter_actor_set_fixed_position_set (self->drag_actor, FALSE);
      if (self->drag_actor_had_nat_width)
        clutter_actor_set_width (self->drag_actor, -1);
      if (self->drag_actor_had_nat_height)
        clutter_actor_set_height (self->drag_actor, -1);
    }
  else if (self->drag_orig_parent == NULL && self->drag_actor != NULL)
    {
      clutter_actor_destroy (self->drag_actor);
    }

  g_signal_emit (self, signals[DRAG_END], 0, event_time, FALSE);
  finish_animation (self);
}

typedef struct
{
  ShellDraggable *draggable;
  uint32_t event_time;
} AnimationData;

static void
animation_done_cb (gboolean finished,
                   gpointer user_data)
{
  AnimationData *data = user_data;

  on_animation_complete (data->draggable, data->event_time);
}

static void
animation_data_free (AnimationData *data)
{
  g_object_unref (data->draggable);
  g_free (data);
}

/* Animates the drag actor back to where @x, @y and @scale tell if @position
 * is set, and always fades it back to its original opacity */
static void
animate_drag_end (ShellDraggable *self,
                  uint32_t        event_time,
                  int             duration,
                  gboolean        position,
                  float           x,
                  float           y,
                  float           scale)
{
  ShellEaseParams params = {
    .duration = duration,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
  AnimationData *data = g_new0 (AnimationData, 1);

  data->draggable = g_object_ref (self);
  data->event_time = event_time;

  self->animation_in_progress = TRUE;

  /* start the animation */
  if (position)
    shell_actor_ease (self->drag_actor, &params,
                      animation_done_cb, data,
                      (GDestroyNotify) animation_data_free,
                      "x", (double) x,
                      "y", (double) y,
                      "scale-x", (double) scale,
                      "scale-y", (double) scale,
                      "opacity", (int) self->drag_orig_opacity,
                      NULL);
  else
    shell_actor_ease (self->drag_actor, &params,
                      animation_done_cb, data,
                      (GDestroyNotify) animation_data_free,
                      "opacity", (int) self->drag_orig_opacity,
                      NULL);
}

static void
restore_drag_actor (ShellDraggable *self,
                    uint32_t        event_time)
{
  float restore_x, restore_y, restore_scale;

  get_restore_location (self, &restore_x, &restore_y, &restore_scale);

  /* fade the actor back in at its original location */
  clutter_actor_set_position (self->drag_actor, restore_x, restore_y);
  clutter_actor_set_scale (self->drag_actor, restore_scale, restore_scale);
  clutter_actor_set_opacity (self->drag_actor, 0);

  animate_drag_end (self, event_time, SHELL_DND_REVERT_ANIMATION_TIME,
                    FALSE, 0.f, 0.f, 0.f);
}

static void
cancel_drag (ShellDraggable *self,
             uint32_t        event_time)
{
  g_autoptr (ShellDraggable) keep = g_object_ref (self);
  float snap_back_x, snap_back_y, snap_back_scale;

  g_signal_emit (self, signals[DRAG_CANCELLED], 0, event_time);

  if (self->drag_actor == NULL)
    {
      update_cursor (CLUTTER_CURSOR_DEFAULT);
      drag_complete (self);
      g_signal_emit (self, signals[DRAG_END], 0, event_time, FALSE);
      return;
    }

  get_restore_location (self, &snap_back_x, &snap_back_y, &snap_back_scale);

  animate_drag_end (self, event_time, SHELL_DND_SNAP_BACK_ANIMATION_TIME,
                    TRUE, snap_back_x, snap_back_y, snap_back_scale);
}

static void
drag_actor_dropped (ShellDraggable     *self,
                    const ClutterEvent *event)
{
  g_autoptr (ShellDraggable) keep = g_object_ref (self);
  g_autoptr (ShellDropEvent) drop_event = NULL;
  g_autoptr (ClutterActor) target = NULL;
  ClutterActor *ui_group = get_ui_group ();
  ClutterActor *stage;
  float drop_x, drop_y;

  if (self->drag_actor == NULL)
    return;

  clutter_event_get_coords (event, &drop_x, &drop_y);
  stage = clutter_actor_get_stage (self->drag_actor);
  if (stage == NULL)
    stage = CLUTTER_ACTOR (get_stage ());
  target = g_object_ref (clutter_stage_get_actor_at_pos (CLUTTER_STAGE (stage),
                                                         CLUTTER_PICK_ALL,
                                                         drop_x, drop_y));

  /* We call observers only once per motion with the innermost target actor.
   * If necessary, the observer can walk the parent itself. */
  drop_event = shell_drop_event_new (self->drag_actor, target, event);
  if (run_drop_monitors (drop_event))
    return;

  /* At this point it is too late to cancel a drag by destroying the actor,
   * the fate of which is decided by accept_drop and its side-effects */
  self->drag_cancellable = FALSE;

  while (target != NULL)
    {
      ShellDragTarget *drag_target = shell_dnd_get_drag_target (target);

      if (drag_target != NULL)
        {
          float target_x, target_y;
          gboolean accepted;

          clutter_actor_transform_stage_point (target, drop_x, drop_y,
                                               &target_x, &target_y);

          /* A drag actor gone while the targets are asked has nothing to
           * offer them any more */
          if (self->drag_actor == NULL)
            break;

          /* On error (an exception of the JS target), skip this target */
          accepted = shell_drag_target_accept_drop (drag_target,
                                                    G_OBJECT (self->actor_held),
                                                    self->drag_actor,
                                                    target_x, target_y,
                                                    clutter_event_get_time (event));
          if (accepted)
            {
              /* If it accepted the drop without taking the actor, handle it
               * ourselves. */
              if (self->drag_actor != NULL &&
                  clutter_actor_get_parent (self->drag_actor) == ui_group)
                {
                  if (self->restore_on_success)
                    {
                      restore_drag_actor (self, clutter_event_get_time (event));
                      return;
                    }
                  else
                    {
                      clutter_actor_destroy (self->drag_actor);
                    }
                }

              update_cursor (CLUTTER_CURSOR_DEFAULT);
              g_signal_emit (self, signals[DRAG_END], 0,
                             clutter_event_get_time (event), TRUE);
              drag_complete (self);
              return;
            }
        }

      g_set_object (&target, clutter_actor_get_parent (target));
    }

  /* If no target has been found, cancel the drag */
  cancel_drag (self, clutter_event_get_time (event));
}

static void
drag_complete (ShellDraggable *self)
{
  g_autoptr (ShellDraggable) keep = g_object_ref (self);

  if (self->drag_actor != NULL)
    shell_util_set_hidden_from_pick (self->drag_actor, FALSE);

  if (self->grab != NULL)
    ungrab_events (self);

  update_drag_hover_stop (self);

  if (self->drag_actor != NULL)
    {
      if (self->drag_actor_destroy_id != 0)
        g_signal_handler_disconnect (self->drag_actor,
                                     self->drag_actor_destroy_id);
      self->drag_actor_destroy_id = 0;
      g_clear_object (&self->drag_actor);
    }

  if (self->drag_orig_parent != NULL)
    {
      if (self->drag_orig_parent_destroy_id != 0)
        g_signal_handler_disconnect (self->drag_orig_parent,
                                     self->drag_orig_parent_destroy_id);
      self->drag_orig_parent_destroy_id = 0;
      g_clear_object (&self->drag_orig_parent);
    }

  if (self->drag_actor_source != NULL)
    {
      if (self->drag_actor_source_destroy_id != 0)
        g_signal_handler_disconnect (self->drag_actor_source,
                                     self->drag_actor_source_destroy_id);
      self->drag_actor_source_destroy_id = 0;
      g_clear_object (&self->drag_actor_source);
    }

  if (current_draggable == self)
    g_clear_object (&current_draggable);
  g_clear_object (&self->actor_held);
}

static gboolean
event_handler_event_cb (ClutterActor *actor,
                        ClutterEvent *event,
                        gpointer      user_data)
{
  g_autoptr (ShellDraggable) self = NULL;
  ClutterEventType type = clutter_event_type (event);
  ClutterBackend *backend;
  ClutterSprite *sprite;

  if (current_draggable == NULL)
    return CLUTTER_EVENT_PROPAGATE;

  self = g_object_ref (current_draggable);

  if (type == CLUTTER_KEY_PRESS)
    {
      if (clutter_event_get_key_symbol (event) == CLUTTER_KEY_Escape)
        cancel_drag (self, clutter_event_get_time (event));

      return CLUTTER_EVENT_PROPAGATE;
    }

  backend = clutter_context_get_backend (clutter_actor_get_context (actor));
  sprite = clutter_backend_get_sprite (backend, get_stage (), event);
  if (sprite != self->sprite)
    return CLUTTER_EVENT_PROPAGATE;

  if (type == CLUTTER_BUTTON_RELEASE || type == CLUTTER_TOUCH_END)
    drag_actor_dropped (self, event);

  if (type == CLUTTER_MOTION || type == CLUTTER_TOUCH_UPDATE)
    update_drag_position (self, event);

  return CLUTTER_EVENT_PROPAGATE;
}

static void
actor_weak_notify_cb (gpointer  data,
                      GObject  *where_the_object_was)
{
  ShellDraggable *self = data;

  self->actor = NULL;
}

static void
shell_draggable_constructed (GObject *object)
{
  ShellDraggable *self = SHELL_DRAGGABLE (object);

  G_OBJECT_CLASS (shell_draggable_parent_class)->constructed (object);

  g_return_if_fail (CLUTTER_IS_ACTOR (self->actor));

  /* Hold our own reference: the actor sinks the floating one when the
   * action is added and drops it when the actor is finalized, which can
   * happen before the draggable is disposed */
  self->dnd_gesture = g_object_ref_sink (g_object_new (st_dnd_start_gesture_get_type (),
                                                       "manual-mode", self->manual_mode,
                                                       "timeout-threshold", self->timeout_threshold,
                                                       NULL));
  g_signal_connect_object (self->dnd_gesture, "recognize",
                           G_CALLBACK (gesture_recognized_cb), self, 0);

  clutter_actor_add_action (self->actor, CLUTTER_ACTION (self->dnd_gesture));

  /* The actor owns the draggable */
  g_object_set_data_full (G_OBJECT (self->actor), DRAGGABLE_KEY,
                          g_object_ref (self), g_object_unref);
}

static void
shell_draggable_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  ShellDraggable *self = SHELL_DRAGGABLE (object);

  switch (prop_id)
    {
    case PROP_ACTOR:
      self->actor = g_value_get_object (value);
      if (self->actor != NULL)
        g_object_weak_ref (G_OBJECT (self->actor), actor_weak_notify_cb, self);
      break;
    case PROP_MANUAL_MODE:
      self->manual_mode = g_value_get_boolean (value);
      break;
    case PROP_TIMEOUT_THRESHOLD:
      self->timeout_threshold = g_value_get_uint (value);
      break;
    case PROP_RESTORE_ON_SUCCESS:
      self->restore_on_success = g_value_get_boolean (value);
      break;
    case PROP_DRAG_ACTOR_MAX_SIZE:
      self->drag_actor_max_size = g_value_get_int (value);
      break;
    case PROP_DRAG_ACTOR_OPACITY:
      self->drag_actor_opacity = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_draggable_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  ShellDraggable *self = SHELL_DRAGGABLE (object);

  switch (prop_id)
    {
    case PROP_ACTOR:
      g_value_set_object (value, self->actor);
      break;
    case PROP_MANUAL_MODE:
      g_value_set_boolean (value, self->manual_mode);
      break;
    case PROP_TIMEOUT_THRESHOLD:
      g_value_set_uint (value, self->timeout_threshold);
      break;
    case PROP_RESTORE_ON_SUCCESS:
      g_value_set_boolean (value, self->restore_on_success);
      break;
    case PROP_DRAG_ACTOR_MAX_SIZE:
      g_value_set_int (value, self->drag_actor_max_size);
      break;
    case PROP_DRAG_ACTOR_OPACITY:
      g_value_set_int (value, self->drag_actor_opacity);
      break;
    case PROP_START_GESTURE:
      g_value_set_object (value, self->dnd_gesture);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_draggable_dispose (GObject *object)
{
  ShellDraggable *self = SHELL_DRAGGABLE (object);

  update_drag_hover_stop (self);

  if (self->grab != NULL)
    ungrab_events (self);

  if (self->drag_actor != NULL && self->drag_actor_destroy_id != 0)
    g_signal_handler_disconnect (self->drag_actor, self->drag_actor_destroy_id);
  self->drag_actor_destroy_id = 0;
  g_clear_object (&self->drag_actor);

  if (self->drag_orig_parent != NULL && self->drag_orig_parent_destroy_id != 0)
    g_signal_handler_disconnect (self->drag_orig_parent,
                                 self->drag_orig_parent_destroy_id);
  self->drag_orig_parent_destroy_id = 0;
  g_clear_object (&self->drag_orig_parent);

  if (self->drag_actor_source != NULL && self->drag_actor_source_destroy_id != 0)
    g_signal_handler_disconnect (self->drag_actor_source,
                                 self->drag_actor_source_destroy_id);
  self->drag_actor_source_destroy_id = 0;
  g_clear_object (&self->drag_actor_source);

  g_clear_object (&self->actor_held);

  if (self->actor != NULL)
    {
      g_object_weak_unref (G_OBJECT (self->actor), actor_weak_notify_cb, self);
      /* A destroyed actor dropped its actions already; removing the
       * gesture again would warn */
      if (self->dnd_gesture != NULL &&
          clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->dnd_gesture)) == self->actor)
        clutter_actor_remove_action (self->actor,
                                     CLUTTER_ACTION (self->dnd_gesture));
      self->actor = NULL;
    }

  g_clear_object (&self->dnd_gesture);

  G_OBJECT_CLASS (shell_draggable_parent_class)->dispose (object);
}

static void
shell_draggable_class_init (ShellDraggableClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_draggable_constructed;
  object_class->set_property = shell_draggable_set_property;
  object_class->get_property = shell_draggable_get_property;
  object_class->dispose = shell_draggable_dispose;

  /**
   * ShellDraggable:actor:
   *
   * The actor which can be dragged (`_Draggable.actor`). It owns the
   * draggable.
   */
  props[PROP_ACTOR] =
    g_param_spec_object ("actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDraggable:manual-mode:
   *
   * Whether the drag is not started automatically when the actor is clicked
   * and dragged (`manualMode`), see #StDndStartGesture:manual-mode.
   */
  props[PROP_MANUAL_MODE] =
    g_param_spec_boolean ("manual-mode", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellDraggable:timeout-threshold:
   *
   * How long in milliseconds the actor has to be pressed before it can be
   * dragged, 0 for no timeout (`timeoutThreshold`), see
   * #StDndStartGesture:timeout-threshold.
   */
  props[PROP_TIMEOUT_THRESHOLD] =
    g_param_spec_uint ("timeout-threshold", NULL, NULL,
                       0, G_MAXUINT, 0,
                       G_PARAM_READWRITE |
                       G_PARAM_CONSTRUCT_ONLY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellDraggable:restore-on-success:
   *
   * Whether the drag actor is animated back to where it came from when the
   * drop succeeds, instead of being destroyed (`restoreOnSuccess`). Note that
   * when the drag actor is the source actor and the drop succeeds without
   * this, the actor scale and opacity aren't reset; if the drop target wants
   * to reuse the actor, it's up to the drop target to reset these values.
   */
  props[PROP_RESTORE_ON_SUCCESS] =
    g_param_spec_boolean ("restore-on-success", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellDraggable:drag-actor-max-size:
   *
   * The size in pixels the drag actor is scaled down to if it is larger, or
   * -1 to keep its size (`dragActorMaxSize`).
   */
  props[PROP_DRAG_ACTOR_MAX_SIZE] =
    g_param_spec_int ("drag-actor-max-size", NULL, NULL,
                      -1, G_MAXINT, -1,
                      G_PARAM_READWRITE |
                      G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellDraggable:drag-actor-opacity:
   *
   * The opacity the drag actor has during the drag, or -1 to keep its
   * opacity (`dragActorOpacity`).
   */
  props[PROP_DRAG_ACTOR_OPACITY] =
    g_param_spec_int ("drag-actor-opacity", NULL, NULL,
                      -1, 255, -1,
                      G_PARAM_READWRITE |
                      G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellDraggable:start-gesture:
   *
   * The gesture which starts the drag (`startGesture`).
   */
  props[PROP_START_GESTURE] =
    g_param_spec_object ("start-gesture", NULL, NULL,
                         st_dnd_start_gesture_get_type (),
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellDraggable::drag-begin:
   * @self: the #ShellDraggable
   * @time: the time of the event that started the drag
   *
   * Emitted when the drag starts, the pointer is grabbed already.
   */
  signals[DRAG_BEGIN] =
    g_signal_new ("drag-begin",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_UINT);

  /**
   * ShellDraggable::drag-cancelled:
   * @self: the #ShellDraggable
   * @time: the time of the event that cancelled the drag
   *
   * Emitted when the drag is cancelled, because it was dropped nowhere or
   * Escape was pressed or the drag actor was destroyed, before the drag actor
   * snaps back. Then ::drag-end follows.
   */
  signals[DRAG_CANCELLED] =
    g_signal_new ("drag-cancelled",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_UINT);

  /**
   * ShellDraggable::drag-end:
   * @self: the #ShellDraggable
   * @time: the time of the event that ended the drag
   * @snapback: whether the drag actor snapped back, or reverted after a
   *   drop. %FALSE if the drop was accepted
   *
   * Emitted when the drag is over, whatever the outcome.
   */
  signals[DRAG_END] =
    g_signal_new ("drag-end",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_UINT,
                  G_TYPE_BOOLEAN);
}

static void
shell_draggable_init (ShellDraggable *self)
{
  self->drag_actor_max_size = -1;
  self->drag_actor_opacity = -1;
  self->drag_cancellable = TRUE;
}

/**
 * shell_draggable_new:
 * @actor: the actor to make draggable
 *
 * Creates an object which controls drag and drop for @actor
 * (`DND.makeDraggable (actor)`), and which @actor owns. Use g_object_new () to
 * set the other properties.
 *
 * Returns: (transfer full): a new #ShellDraggable
 */
ShellDraggable *
shell_draggable_new (ClutterActor *actor)
{
  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), NULL);

  return g_object_new (SHELL_TYPE_DRAGGABLE,
                       "actor", actor,
                       NULL);
}

/**
 * shell_draggable_get_actor:
 * @self: a #ShellDraggable
 *
 * Returns: (transfer none) (nullable): the actor which can be dragged
 */
ClutterActor *
shell_draggable_get_actor (ShellDraggable *self)
{
  g_return_val_if_fail (SHELL_IS_DRAGGABLE (self), NULL);

  return self->actor;
}

/**
 * shell_draggable_get_start_gesture:
 * @self: a #ShellDraggable
 *
 * Returns: (transfer none): the gesture which starts the drag
 */
StDndStartGesture *
shell_draggable_get_start_gesture (ShellDraggable *self)
{
  g_return_val_if_fail (SHELL_IS_DRAGGABLE (self), NULL);

  return self->dnd_gesture;
}

/**
 * shell_dnd_is_dragging:
 *
 * Whether a drag started by a #ShellDraggable is in progress. That includes
 * the animation after the drop, until the pointer is released from the drag.
 *
 * Returns: %TRUE if the shell is dragging something
 */
gboolean
shell_dnd_is_dragging (void)
{
  return current_draggable != NULL && current_draggable->grab != NULL;
}

/* ------------------------------------------------------------------------ */
/* Drags of X11 clients                                                      */
/* ------------------------------------------------------------------------ */

static ShellXdndHandler *xdnd_handler = NULL;
static gulong xdnd_motion_id = 0;

static void
xdnd_drag_motion_cb (ShellXdndHandler *handler,
                     int               x,
                     int               y,
                     ClutterActor     *drag_actor,
                     ClutterActor     *target_actor,
                     gpointer          user_data)
{
  g_autoptr (ShellDragEvent) event = NULL;
  g_autoptr (ClutterActor) picked_actor = NULL;
  ShellDragMotionResult result;

  event = shell_drag_event_new (x, y, drag_actor, G_OBJECT (handler),
                                target_actor);

  if (run_motion_monitors (event, &result))
    return;

  if (target_actor != NULL)
    picked_actor = g_object_ref (target_actor);

  while (picked_actor != NULL)
    {
      ShellDragTarget *drag_target = shell_dnd_get_drag_target (picked_actor);

      if (drag_target != NULL)
        {
          float target_x, target_y;

          clutter_actor_transform_stage_point (picked_actor, x, y,
                                               &target_x, &target_y);
          result = shell_drag_target_handle_drag_over (drag_target,
                                                       G_OBJECT (handler),
                                                       drag_actor,
                                                       target_x, target_y,
                                                       shell_global_get_current_time (shell_global_get ()));
          if (result != SHELL_DRAG_MOTION_RESULT_CONTINUE)
            return;
        }

      g_set_object (&picked_actor, clutter_actor_get_parent (picked_actor));
    }
}

static void
xdnd_handler_weak_notify_cb (gpointer  data,
                             GObject  *where_the_object_was)
{
  xdnd_handler = NULL;
  xdnd_motion_id = 0;
}

/**
 * shell_dnd_set_xdnd_handler:
 * @handler: (nullable): the handler of the drags of X11 clients
 *
 * Dispatches the motion of the drags coming from X11 clients, reported by
 * the ::drag-motion signal of @handler (`DND.handleXdndMotion ()`), to the
 * drag monitors and to the drop targets of the actors under the pointer,
 * with @handler as the source of the drag. The cursor is left to the X
 * client, and the answers of the targets are ignored except to stop at the
 * first one that does not say %SHELL_DRAG_MOTION_RESULT_CONTINUE. Replaces
 * the handler set before.
 */
void
shell_dnd_set_xdnd_handler (ShellXdndHandler *handler)
{
  g_return_if_fail (handler == NULL || SHELL_IS_XDND_HANDLER (handler));

  if (xdnd_handler == handler)
    return;

  if (xdnd_handler != NULL)
    {
      g_signal_handler_disconnect (xdnd_handler, xdnd_motion_id);
      g_object_weak_unref (G_OBJECT (xdnd_handler),
                           xdnd_handler_weak_notify_cb, NULL);
      xdnd_handler = NULL;
      xdnd_motion_id = 0;
    }

  if (handler == NULL)
    return;

  xdnd_handler = handler;
  g_object_weak_ref (G_OBJECT (handler), xdnd_handler_weak_notify_cb, NULL);
  xdnd_motion_id = g_signal_connect (handler, "drag-motion",
                                     G_CALLBACK (xdnd_drag_motion_cb), NULL);
}
