/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * workspace-thumbnail.c: The strip of workspace thumbnails of the overview.
 * Port of js/ui/workspaceThumbnail.js.
 *
 * #ShellWindowClone (was WindowClone) is the clone of a window in a
 * thumbnail and a drag source, #ShellWorkspaceThumbnail (WorkspaceThumbnail)
 * is the thumbnail of one workspace on one monitor and #ShellThumbnailsBox
 * (ThumbnailsBox) is the strip, a #ShellDragTarget that moves windows between
 * workspaces and creates workspaces.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `PrimaryActorLayout` is gone, #ShellWindowClone requests the size of
 *    its clone itself.
 *  - The `_delegate` lookups of the clones are gone: the clone above another
 *    one is the #ShellWindowClone:stack-above property.
 *  - `Main.overview` is reached through #ShellWindowManager: the changes of
 *    #ShellWindowManager:overview-visible are the overview's `showing` and
 *    `hidden` signals, and clicking the thumbnail of the active workspace
 *    emits #ShellWindowManager::overview-hide-requested. The window stacking
 *    is read from the display's `restacked` signal instead of the overview's
 *    `windows-restacked`. The drags of the overview are told to the box with
 *    shell_thumbnails_box_drag_begin () and its siblings, and the drags of
 *    the windows of the thumbnails are told to the overview by the
 *    #ShellThumbnailsBox::window-drag-begin family of signals (see
 *    overviewControls.js).
 *  - The JS inspected the drag source with `source.metaWindow`,
 *    `source.app`, `source.animateLaunchAtPos ()` and
 *    `source.shellWorkspaceLaunch ()`. The window that is dragged is the
 *    "meta-window" property of the source (the window clones and the window
 *    previews have it), the application is shell_drag_source_get_app () of
 *    the launchers, which implement #ShellDragSource, and the launch
 *    animation is shell_drag_source_animate_launch_at_pos ().
 *    `shellWorkspaceLaunch`, which only extensions used, is dropped.
 *  - The drag actor of the window clones keeps its size and opacity: the JS
 *    passed `Workspace.WINDOW_DND_SIZE` and `DRAGGING_WINDOW_OPACITY`, which
 *    workspace.js has not exported for a long time, so they were `undefined`.
 *  - `Main.moveWindowToMonitorAndWorkspace ()` is a private function here.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>

#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-later.h>
#include <meta/meta-window-actor.h>
#include <meta/meta-workspace-manager.h>
#include <meta/prefs.h>

#include "shell-app.h"
#include "shell-enum-types.h"
#include "shell-global.h"
#include "shell-util.h"
#include "ui/dnd.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/window-manager.h"
#include "ui/workspace-thumbnail.h"
#include "ui/xdnd-handler.h"

#define NUM_WORKSPACES_THRESHOLD 2

#define RESCALE_ANIMATION_TIME 200
#define SLIDE_ANIMATION_TIME 200

/* When we create workspaces by dragging, we add a "cut" into the top and
 * bottom of each workspace so that the user doesn't have to hit the
 * placeholder exactly. */
#define WORKSPACE_CUT_SIZE 10

#define WORKSPACE_KEEP_ALIVE_TIME 100

#define MUTTER_SCHEMA "org.gnome.mutter"

#define N_THUMBNAIL_STATES (SHELL_THUMBNAIL_STATE_DESTROYED + 1)

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

/* Math.round () rounds halves up, round () rounds them away from zero */
static double
js_round (double value)
{
  return floor (value + 0.5);
}

static double
lerp (double start,
      double end,
      double progress)
{
  return start + progress * (end - start);
}

static gboolean
is_rtl (void)
{
  return clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL;
}

static MetaLaters *
get_laters (void)
{
  ShellGlobal *global = shell_global_get ();
  MetaCompositor *compositor;

  if (global == NULL)
    return NULL;

  compositor = shell_global_get_compositor (global);

  return compositor != NULL ? meta_compositor_get_laters (compositor) : NULL;
}

/* ------------------------------------------------------------------------ */
/* Main.moveWindowToMonitorAndWorkspace ()                                   */
/* ------------------------------------------------------------------------ */

typedef struct
{
  MetaWindow *window; /* weak */
  MetaDisplay *display; /* weak */
  gulong handler_id;
  int workspace_index;
  gboolean append;
} MoveWindowData;

static void
move_window_data_free (gpointer  user_data,
                       GClosure *closure)
{
  MoveWindowData *data = user_data;

  g_clear_weak_pointer (&data->window);
  g_clear_weak_pointer (&data->display);
  g_free (data);
}

static void
window_entered_monitor_once_cb (MetaDisplay    *display,
                                int             monitor,
                                MetaWindow     *window,
                                MoveWindowData *data)
{
  if (window != data->window)
    return;

  meta_window_change_workspace_by_index (window, data->workspace_index,
                                         data->append);
  g_signal_handler_disconnect (display, data->handler_id);
}

static void
move_window_to_monitor_and_workspace (MetaDisplay *display,
                                      MetaWindow  *window,
                                      int          monitor_index,
                                      int          workspace_index,
                                      gboolean     append)
{
  /* We need to move the window before changing the workspace, because
   * the move itself could cause a workspace change if the window enters
   * the primary monitor */
  if (meta_window_get_monitor (window) != monitor_index)
    {
      MoveWindowData *data = g_new0 (MoveWindowData, 1);

      g_set_weak_pointer (&data->window, window);
      g_set_weak_pointer (&data->display, display);
      data->workspace_index = workspace_index;
      data->append = append;

      /* Wait for the monitor change to take effect */
      data->handler_id =
        g_signal_connect_data (display, "window-entered-monitor",
                               G_CALLBACK (window_entered_monitor_once_cb),
                               data, move_window_data_free, 0);
      meta_window_move_to_monitor (window, monitor_index);
    }
  else
    {
      meta_window_change_workspace_by_index (window, workspace_index, append);
    }
}

/* ------------------------------------------------------------------------ */
/* ShellWindowClone (WindowClone)                                            */
/* ------------------------------------------------------------------------ */

enum
{
  CLONE_PROP_0,
  CLONE_PROP_WINDOW_ACTOR,
  CLONE_PROP_META_WINDOW,
  CLONE_PROP_IN_DRAG,
  CLONE_PROP_STACK_ABOVE,
  CLONE_N_PROPS
};

static GParamSpec *clone_props[CLONE_N_PROPS] = { NULL, };

enum
{
  CLONE_DRAG_BEGIN,
  CLONE_DRAG_CANCELLED,
  CLONE_DRAG_END,
  CLONE_SELECTED,
  CLONE_LAST_SIGNAL
};

static guint clone_signals[CLONE_LAST_SIGNAL] = { 0 };

struct _ShellWindowClone
{
  ClutterActor parent;

  MetaWindowActor *window_actor;    /* strong */
  MetaWindow *meta_window;          /* strong */
  ClutterActor *clone;              /* weak: the child that is the clone */
  ShellWindowClone *stack_above;    /* weak */
  gboolean in_drag;
};

G_DEFINE_FINAL_TYPE (ShellWindowClone, shell_window_clone, CLUTTER_TYPE_ACTOR)

static void
window_clone_set_in_drag (ShellWindowClone *self,
                          gboolean          in_drag)
{
  if (self->in_drag == in_drag)
    return;

  self->in_drag = in_drag;
  g_object_notify_by_pspec (G_OBJECT (self), clone_props[CLONE_PROP_IN_DRAG]);
}

static void
window_clone_update_position (ShellWindowClone *self)
{
  ClutterActor *real = CLUTTER_ACTOR (self->window_actor);

  clutter_actor_set_position (CLUTTER_ACTOR (self),
                              clutter_actor_get_x (real),
                              clutter_actor_get_y (real));
}

static void
window_clone_position_changed_cb (MetaWindowActor   *window_actor,
                                  GParamSpec        *pspec,
                                  ShellWindowClone  *self)
{
  window_clone_update_position (self);
}

static void
window_clone_window_actor_destroyed_cb (MetaWindowActor  *window_actor,
                                        ShellWindowClone *self)
{
  /* First destroy the clone and then destroy everything */
  if (self->clone != NULL)
    clutter_actor_destroy (self->clone);

  clutter_actor_destroy (CLUTTER_ACTOR (self));
}

static void
window_clone_update_dialog_position (ShellWindowClone *self,
                                     MetaWindowActor  *dialog_actor,
                                     ClutterActor     *dialog_clone)
{
  MetaWindow *dialog = meta_window_actor_get_meta_window (dialog_actor);
  MtkRectangle dialog_rect, rect;

  if (dialog == NULL)
    return;

  meta_window_get_frame_rect (dialog, &dialog_rect);
  meta_window_get_frame_rect (self->meta_window, &rect);

  clutter_actor_set_position (dialog_clone,
                              dialog_rect.x - rect.x,
                              dialog_rect.y - rect.y);
}

static void
attached_dialog_position_changed_cb (MetaWindowActor *dialog_actor,
                                     GParamSpec      *pspec,
                                     ClutterActor    *dialog_clone)
{
  ClutterActor *parent = clutter_actor_get_parent (dialog_clone);

  if (SHELL_IS_WINDOW_CLONE (parent))
    window_clone_update_dialog_position (SHELL_WINDOW_CLONE (parent),
                                         dialog_actor, dialog_clone);
}

static void
attached_dialog_destroyed_cb (MetaWindowActor *dialog_actor,
                              ClutterActor    *dialog_clone)
{
  clutter_actor_destroy (dialog_clone);
}

static void
window_clone_do_add_attached_dialog (ShellWindowClone *self,
                                     MetaWindowActor  *dialog_actor)
{
  ClutterActor *dialog_clone = clutter_clone_new (CLUTTER_ACTOR (dialog_actor));

  clutter_actor_add_child (CLUTTER_ACTOR (self), dialog_clone);
  window_clone_update_dialog_position (self, dialog_actor, dialog_clone);

  /* The handlers go away with the clone of the dialog */
  g_signal_connect_object (dialog_actor, "notify::position",
                           G_CALLBACK (attached_dialog_position_changed_cb),
                           dialog_clone, 0);
  g_signal_connect_object (dialog_actor, "destroy",
                           G_CALLBACK (attached_dialog_destroyed_cb),
                           dialog_clone, 0);
}

static gboolean
add_attached_dialogs_cb (MetaWindow *window,
                         gpointer    user_data)
{
  ShellWindowClone *self = user_data;
  GObject *actor = meta_window_get_compositor_private (window);

  if (actor == NULL)
    return FALSE;
  if (!meta_window_is_attached_dialog (window))
    return FALSE;

  window_clone_do_add_attached_dialog (self, META_WINDOW_ACTOR (actor));
  meta_window_foreach_transient (window, add_attached_dialogs_cb, self);

  return TRUE;
}

static void
window_clone_restack (ShellWindowClone *self)
{
  ClutterActor *parent = clutter_actor_get_parent (CLUTTER_ACTOR (self));
  ShellWindowClone *above;

  if (parent == NULL)
    return;

  above = shell_window_clone_get_actual_stack_above (self);

  if (above == NULL)
    clutter_actor_set_child_below_sibling (parent, CLUTTER_ACTOR (self), NULL);
  else if (clutter_actor_get_parent (CLUTTER_ACTOR (above)) == parent)
    clutter_actor_set_child_above_sibling (parent, CLUTTER_ACTOR (self),
                                           CLUTTER_ACTOR (above));
}

static void
window_clone_draggable_begin_cb (ShellDraggable  *draggable,
                                 guint            time,
                                 ShellWindowClone *self)
{
  window_clone_set_in_drag (self, TRUE);
  g_signal_emit (self, clone_signals[CLONE_DRAG_BEGIN], 0);
}

static void
window_clone_draggable_cancelled_cb (ShellDraggable  *draggable,
                                     guint            time,
                                     ShellWindowClone *self)
{
  g_signal_emit (self, clone_signals[CLONE_DRAG_CANCELLED], 0);
}

static void
window_clone_draggable_end_cb (ShellDraggable  *draggable,
                               guint            time,
                               gboolean         snapback,
                               ShellWindowClone *self)
{
  ClutterActor *parent;

  window_clone_set_in_drag (self, FALSE);

  /* We may not have a parent if DnD completed successfully, in
   * which case our clone will shortly be destroyed and replaced
   * with a new one on the target workspace. */
  parent = clutter_actor_get_parent (CLUTTER_ACTOR (self));
  if (parent != NULL)
    {
      if (self->stack_above == NULL)
        clutter_actor_set_child_below_sibling (parent, CLUTTER_ACTOR (self), NULL);
      else if (clutter_actor_get_parent (CLUTTER_ACTOR (self->stack_above)) == parent)
        clutter_actor_set_child_above_sibling (parent, CLUTTER_ACTOR (self),
                                               CLUTTER_ACTOR (self->stack_above));
    }

  g_signal_emit (self, clone_signals[CLONE_DRAG_END], 0);
}

static void
window_clone_click_recognize_cb (ClutterGesture   *gesture,
                                 ShellWindowClone *self)
{
  g_signal_emit (self, clone_signals[CLONE_SELECTED], 0,
                 clutter_get_current_event_time ());
}

static void
shell_window_clone_get_preferred_width (ClutterActor *actor,
                                        float         for_height,
                                        float        *min_width_p,
                                        float        *natural_width_p)
{
  ShellWindowClone *self = SHELL_WINDOW_CLONE (actor);

  /* Request the size of the clone only, the dialogs are allocated at their
   * fixed positions. */
  if (self->clone != NULL)
    {
      clutter_actor_get_preferred_width (self->clone, for_height,
                                         min_width_p, natural_width_p);
      return;
    }

  if (min_width_p != NULL)
    *min_width_p = 0;
  if (natural_width_p != NULL)
    *natural_width_p = 0;
}

static void
shell_window_clone_get_preferred_height (ClutterActor *actor,
                                         float         for_width,
                                         float        *min_height_p,
                                         float        *natural_height_p)
{
  ShellWindowClone *self = SHELL_WINDOW_CLONE (actor);

  if (self->clone != NULL)
    {
      clutter_actor_get_preferred_height (self->clone, for_width,
                                          min_height_p, natural_height_p);
      return;
    }

  if (min_height_p != NULL)
    *min_height_p = 0;
  if (natural_height_p != NULL)
    *natural_height_p = 0;
}

static void
shell_window_clone_destroy (ClutterActor *actor)
{
  ShellWindowClone *self = SHELL_WINDOW_CLONE (actor);

  if (self->in_drag)
    {
      g_signal_emit (self, clone_signals[CLONE_DRAG_END], 0);
      window_clone_set_in_drag (self, FALSE);
    }

  CLUTTER_ACTOR_CLASS (shell_window_clone_parent_class)->destroy (actor);
}

static void
shell_window_clone_constructed (GObject *object)
{
  ShellWindowClone *self = SHELL_WINDOW_CLONE (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterAction *click_gesture;
  ClutterActor *clone;
  ShellDraggable *draggable;

  G_OBJECT_CLASS (shell_window_clone_parent_class)->constructed (object);

  g_return_if_fail (META_IS_WINDOW_ACTOR (self->window_actor));

  self->meta_window = g_object_ref (meta_window_actor_get_meta_window (self->window_actor));

  clutter_actor_set_reactive (actor, TRUE);

  clone = clutter_clone_new (CLUTTER_ACTOR (self->window_actor));
  clutter_actor_add_child (actor, clone);
  g_set_weak_pointer (&self->clone, clone);

  g_signal_connect_object (self->window_actor, "notify::position",
                           G_CALLBACK (window_clone_position_changed_cb),
                           self, 0);
  g_signal_connect_object (self->window_actor, "destroy",
                           G_CALLBACK (window_clone_window_actor_destroyed_cb),
                           self, 0);
  window_clone_update_position (self);

  /* The clone owns the draggable */
  draggable = g_object_new (SHELL_TYPE_DRAGGABLE,
                            "actor", self,
                            "restore-on-success", TRUE,
                            NULL);
  g_signal_connect_object (draggable, "drag-begin",
                           G_CALLBACK (window_clone_draggable_begin_cb),
                           self, 0);
  g_signal_connect_object (draggable, "drag-cancelled",
                           G_CALLBACK (window_clone_draggable_cancelled_cb),
                           self, 0);
  g_signal_connect_object (draggable, "drag-end",
                           G_CALLBACK (window_clone_draggable_end_cb),
                           self, 0);
  g_object_unref (draggable);

  click_gesture = CLUTTER_ACTION (clutter_click_gesture_new ());
  g_signal_connect_object (click_gesture, "recognize",
                           G_CALLBACK (window_clone_click_recognize_cb),
                           self, 0);
  clutter_actor_add_action (actor, click_gesture);

  meta_window_foreach_transient (self->meta_window, add_attached_dialogs_cb, self);
}

static void
shell_window_clone_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellWindowClone *self = SHELL_WINDOW_CLONE (object);

  switch (prop_id)
    {
    case CLONE_PROP_WINDOW_ACTOR:
      g_set_object (&self->window_actor, g_value_get_object (value));
      break;
    case CLONE_PROP_STACK_ABOVE:
      shell_window_clone_set_stack_above (self, g_value_get_object (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_window_clone_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellWindowClone *self = SHELL_WINDOW_CLONE (object);

  switch (prop_id)
    {
    case CLONE_PROP_WINDOW_ACTOR:
      g_value_set_object (value, self->window_actor);
      break;
    case CLONE_PROP_META_WINDOW:
      g_value_set_object (value, self->meta_window);
      break;
    case CLONE_PROP_IN_DRAG:
      g_value_set_boolean (value, self->in_drag);
      break;
    case CLONE_PROP_STACK_ABOVE:
      g_value_set_object (value, self->stack_above);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_window_clone_dispose (GObject *object)
{
  ShellWindowClone *self = SHELL_WINDOW_CLONE (object);

  /* Destroys the actor */
  G_OBJECT_CLASS (shell_window_clone_parent_class)->dispose (object);

  g_clear_weak_pointer (&self->clone);
  g_clear_weak_pointer (&self->stack_above);
  g_clear_object (&self->meta_window);
  g_clear_object (&self->window_actor);
}

static void
shell_window_clone_class_init (ShellWindowCloneClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_window_clone_constructed;
  object_class->set_property = shell_window_clone_set_property;
  object_class->get_property = shell_window_clone_get_property;
  object_class->dispose = shell_window_clone_dispose;

  actor_class->get_preferred_width = shell_window_clone_get_preferred_width;
  actor_class->get_preferred_height = shell_window_clone_get_preferred_height;
  actor_class->destroy = shell_window_clone_destroy;

  clutter_actor_class_set_layout_manager_type (actor_class,
                                               CLUTTER_TYPE_FIXED_LAYOUT);

  /**
   * ShellWindowClone:window-actor:
   *
   * The actor of the window that is cloned (`realWindow`).
   */
  clone_props[CLONE_PROP_WINDOW_ACTOR] =
    g_param_spec_object ("window-actor", NULL, NULL,
                         META_TYPE_WINDOW_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWindowClone:meta-window:
   *
   * The window that is cloned (`metaWindow`). Drop targets ask the source
   * of a drag for this property.
   */
  clone_props[CLONE_PROP_META_WINDOW] =
    g_param_spec_object ("meta-window", NULL, NULL,
                         META_TYPE_WINDOW,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWindowClone:in-drag:
   *
   * Whether the clone is being dragged (`inDrag`).
   */
  clone_props[CLONE_PROP_IN_DRAG] =
    g_param_spec_boolean ("in-drag", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellWindowClone:stack-above:
   *
   * The clone this one is stacked above, or %NULL when this is the lowest
   * (`_stackAbove`). Setting it restacks the clone, unless that clone is
   * being dragged: then the stacking is applied when the drag ends.
   */
  clone_props[CLONE_PROP_STACK_ABOVE] =
    g_param_spec_object ("stack-above", NULL, NULL,
                         SHELL_TYPE_WINDOW_CLONE,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, CLONE_N_PROPS, clone_props);

  /**
   * ShellWindowClone::drag-begin:
   * @self: the #ShellWindowClone
   *
   * Emitted when the clone starts being dragged.
   */
  clone_signals[CLONE_DRAG_BEGIN] =
    g_signal_new ("drag-begin",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellWindowClone::drag-cancelled:
   * @self: the #ShellWindowClone
   *
   * Emitted when the drag of the clone is cancelled.
   */
  clone_signals[CLONE_DRAG_CANCELLED] =
    g_signal_new ("drag-cancelled",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellWindowClone::drag-end:
   * @self: the #ShellWindowClone
   *
   * Emitted when the drag of the clone ends, or when the clone is destroyed
   * while it is dragged.
   */
  clone_signals[CLONE_DRAG_END] =
    g_signal_new ("drag-end",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellWindowClone::selected:
   * @self: the #ShellWindowClone
   * @time: the timestamp of the click
   *
   * Emitted when the clone is clicked.
   */
  clone_signals[CLONE_SELECTED] =
    g_signal_new ("selected",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_UINT);
}

static void
shell_window_clone_init (ShellWindowClone *self)
{
}

/**
 * shell_window_clone_new:
 * @window_actor: the actor of the window to clone
 *
 * Creates a clone of @window_actor, and of its attached dialogs, that can be
 * dragged to the thumbnails of the other workspaces.
 *
 * Returns: (transfer none): a new #ShellWindowClone, which is floating
 */
ShellWindowClone *
shell_window_clone_new (MetaWindowActor *window_actor)
{
  g_return_val_if_fail (META_IS_WINDOW_ACTOR (window_actor), NULL);

  return g_object_new (SHELL_TYPE_WINDOW_CLONE,
                       "window-actor", window_actor,
                       NULL);
}

/**
 * shell_window_clone_get_window_actor:
 * @self: a #ShellWindowClone
 *
 * Returns: (transfer none): the actor of the window that is cloned
 */
MetaWindowActor *
shell_window_clone_get_window_actor (ShellWindowClone *self)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_CLONE (self), NULL);

  return self->window_actor;
}

/**
 * shell_window_clone_get_meta_window:
 * @self: a #ShellWindowClone
 *
 * Returns: (transfer none): the window that is cloned
 */
MetaWindow *
shell_window_clone_get_meta_window (ShellWindowClone *self)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_CLONE (self), NULL);

  return self->meta_window;
}

/**
 * shell_window_clone_get_in_drag:
 * @self: a #ShellWindowClone
 *
 * Returns: whether the clone is being dragged
 */
gboolean
shell_window_clone_get_in_drag (ShellWindowClone *self)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_CLONE (self), FALSE);

  return self->in_drag;
}

/**
 * shell_window_clone_get_stack_above:
 * @self: a #ShellWindowClone
 *
 * Returns: (transfer none) (nullable): the clone @self is stacked above
 */
ShellWindowClone *
shell_window_clone_get_stack_above (ShellWindowClone *self)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_CLONE (self), NULL);

  return self->stack_above;
}

/**
 * shell_window_clone_get_actual_stack_above:
 * @self: a #ShellWindowClone
 *
 * Finds the clone just below @self, respecting the reparenting done by the
 * DND code (`getActualStackAbove ()`): while @self is dragged this is the
 * clone below the one @self is stacked above.
 *
 * Returns: (transfer none) (nullable): the clone just below @self
 */
ShellWindowClone *
shell_window_clone_get_actual_stack_above (ShellWindowClone *self)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_CLONE (self), NULL);

  if (self->stack_above == NULL)
    return NULL;

  if (self->in_drag)
    return shell_window_clone_get_actual_stack_above (self->stack_above);

  return self->stack_above;
}

/**
 * shell_window_clone_set_stack_above:
 * @self: a #ShellWindowClone
 * @stack_above: (nullable): the clone to stack @self above
 *
 * Stacks @self above @stack_above in the container of the clones
 * (`setStackAbove ()`). Nothing is restacked now if @stack_above is being
 * dragged, the stacking is applied when the drag ends and the windows are
 * stacked again.
 */
void
shell_window_clone_set_stack_above (ShellWindowClone *self,
                                    ShellWindowClone *stack_above)
{
  g_return_if_fail (SHELL_IS_WINDOW_CLONE (self));
  g_return_if_fail (stack_above == NULL || SHELL_IS_WINDOW_CLONE (stack_above));

  if (self->stack_above != stack_above)
    {
      g_set_weak_pointer (&self->stack_above, stack_above);
      g_object_notify_by_pspec (G_OBJECT (self), clone_props[CLONE_PROP_STACK_ABOVE]);
    }

  if (stack_above != NULL && stack_above->in_drag)
    return;

  window_clone_restack (self);
}

/**
 * shell_window_clone_add_attached_dialog:
 * @self: a #ShellWindowClone
 * @dialog: an attached dialog of the window of @self
 *
 * Adds a clone of @dialog, which follows the position of the dialog.
 */
void
shell_window_clone_add_attached_dialog (ShellWindowClone *self,
                                        MetaWindow       *dialog)
{
  GObject *actor;

  g_return_if_fail (SHELL_IS_WINDOW_CLONE (self));
  g_return_if_fail (META_IS_WINDOW (dialog));

  actor = meta_window_get_compositor_private (dialog);
  if (actor == NULL)
    return;

  window_clone_do_add_attached_dialog (self, META_WINDOW_ACTOR (actor));
}

/* ------------------------------------------------------------------------ */
/* ShellWorkspaceThumbnail (WorkspaceThumbnail)                              */
/* ------------------------------------------------------------------------ */

enum
{
  THUMB_PROP_0,
  THUMB_PROP_META_WORKSPACE,
  THUMB_PROP_MONITOR_INDEX,
  THUMB_PROP_WINDOW_MANAGER,
  THUMB_PROP_STATE,
  THUMB_PROP_SLIDE_POSITION,
  THUMB_PROP_COLLAPSE_FRACTION,
  THUMB_N_PROPS
};

static GParamSpec *thumb_props[THUMB_N_PROPS] = { NULL, };

enum
{
  THUMB_WINDOW_DRAG_BEGIN,
  THUMB_WINDOW_DRAG_CANCELLED,
  THUMB_WINDOW_DRAG_END,
  THUMB_LAST_SIGNAL
};

static guint thumb_signals[THUMB_LAST_SIGNAL] = { 0 };

typedef struct
{
  MetaWindow *window;         /* strong */
  gulong minimized_id;
} TrackedWindow;

struct _ShellWorkspaceThumbnail
{
  StWidget parent;

  MetaWorkspace *meta_workspace;       /* strong */
  int monitor_index;
  ShellWindowManager *window_manager;  /* weak */
  MetaDisplay *display;                /* weak */

  ShellThumbnailState state;
  double slide_position;
  double collapse_fraction;
  gboolean removed;

  ClutterActor *viewport;              /* weak */
  ClutterActor *contents;              /* weak */

  GPtrArray *windows;                  /* ShellWindowClone, in stacking order */
  GPtrArray *all_windows;              /* TrackedWindow */

  gulong window_added_id;
  gulong window_removed_id;
  gulong entered_monitor_id;
  gulong left_monitor_id;
};

G_DEFINE_FINAL_TYPE (ShellWorkspaceThumbnail, shell_workspace_thumbnail, ST_TYPE_WIDGET)

static void thumbnail_do_add_window    (ShellWorkspaceThumbnail *self,
                                        MetaWindow              *window);
static void thumbnail_do_remove_window (ShellWorkspaceThumbnail *self,
                                        MetaWindow              *window);

static void
tracked_window_free (TrackedWindow *tracked)
{
  if (tracked->minimized_id != 0)
    g_signal_handler_disconnect (tracked->window, tracked->minimized_id);

  g_clear_object (&tracked->window);
  g_free (tracked);
}

static int
thumbnail_lookup_index (ShellWorkspaceThumbnail *self,
                        MetaWindow              *window)
{
  guint i;

  for (i = 0; i < self->windows->len; i++)
    {
      ShellWindowClone *clone = g_ptr_array_index (self->windows, i);

      if (shell_window_clone_get_meta_window (clone) == window)
        return i;
    }

  return -1;
}

static TrackedWindow *
thumbnail_find_tracked_window (ShellWorkspaceThumbnail *self,
                               MetaWindow              *window)
{
  guint i;

  for (i = 0; i < self->all_windows->len; i++)
    {
      TrackedWindow *tracked = g_ptr_array_index (self->all_windows, i);

      if (tracked->window == window)
        return tracked;
    }

  return NULL;
}

static void
thumbnail_minimized_cb (MetaWindow              *window,
                        GParamSpec              *pspec,
                        ShellWorkspaceThumbnail *self)
{
  gboolean minimized;

  g_object_get (window, "minimized", &minimized, NULL);

  if (minimized)
    thumbnail_do_remove_window (self, window);
  else
    thumbnail_do_add_window (self, window);
}

static void
thumbnail_track_window (ShellWorkspaceThumbnail *self,
                        MetaWindow              *window)
{
  TrackedWindow *tracked = g_new0 (TrackedWindow, 1);

  tracked->window = g_object_ref (window);
  tracked->minimized_id = g_signal_connect (window, "notify::minimized",
                                            G_CALLBACK (thumbnail_minimized_cb),
                                            self);
  g_ptr_array_add (self->all_windows, tracked);
}

/* Tests if @window belongs to this workspace and monitor */
static gboolean
thumbnail_is_my_window (ShellWorkspaceThumbnail *self,
                        MetaWindow              *window)
{
  return meta_window_located_on_workspace (window, self->meta_workspace) &&
         meta_window_get_monitor (window) == self->monitor_index;
}

/* Tests if @window should be shown in the Overview */
static gboolean
thumbnail_is_overview_window (MetaWindow *window)
{
  return !meta_window_is_skip_taskbar (window) &&
         meta_window_showing_on_its_workspace (window);
}

static void
thumbnail_clone_selected_cb (ShellWindowClone        *clone,
                             guint                    time,
                             ShellWorkspaceThumbnail *self)
{
  shell_workspace_thumbnail_activate (self, time);
}

static void
thumbnail_clone_drag_begin_cb (ShellWindowClone        *clone,
                               ShellWorkspaceThumbnail *self)
{
  g_signal_emit (self, thumb_signals[THUMB_WINDOW_DRAG_BEGIN], 0,
                 shell_window_clone_get_meta_window (clone));
}

static void
thumbnail_clone_drag_cancelled_cb (ShellWindowClone        *clone,
                                   ShellWorkspaceThumbnail *self)
{
  g_signal_emit (self, thumb_signals[THUMB_WINDOW_DRAG_CANCELLED], 0,
                 shell_window_clone_get_meta_window (clone));
}

static void
thumbnail_clone_drag_end_cb (ShellWindowClone        *clone,
                             ShellWorkspaceThumbnail *self)
{
  g_signal_emit (self, thumb_signals[THUMB_WINDOW_DRAG_END], 0,
                 shell_window_clone_get_meta_window (clone));
}

static ShellWindowClone *
thumbnail_steal_window_clone (ShellWorkspaceThumbnail *self,
                              MetaWindow              *window)
{
  int index = thumbnail_lookup_index (self, window);

  if (index == -1)
    return NULL;

  return g_ptr_array_steal_index (self->windows, index);
}

static void
thumbnail_clone_destroyed_cb (ClutterActor            *actor,
                              ShellWorkspaceThumbnail *self)
{
  g_autoptr (ShellWindowClone) removed =
    thumbnail_steal_window_clone (self,
                                  shell_window_clone_get_meta_window (SHELL_WINDOW_CLONE (actor)));
}

/* Create a clone of a (non-desktop) window and add it to the window list */
static void
thumbnail_add_window_clone (ShellWorkspaceThumbnail *self,
                            MetaWindowActor         *window_actor)
{
  ShellWindowClone *clone = shell_window_clone_new (window_actor);

  g_signal_connect_object (clone, "selected",
                           G_CALLBACK (thumbnail_clone_selected_cb), self, 0);
  g_signal_connect_object (clone, "drag-begin",
                           G_CALLBACK (thumbnail_clone_drag_begin_cb), self, 0);
  g_signal_connect_object (clone, "drag-cancelled",
                           G_CALLBACK (thumbnail_clone_drag_cancelled_cb), self, 0);
  g_signal_connect_object (clone, "drag-end",
                           G_CALLBACK (thumbnail_clone_drag_end_cb), self, 0);
  g_signal_connect_object (clone, "destroy",
                           G_CALLBACK (thumbnail_clone_destroyed_cb), self, 0);
  clutter_actor_add_child (self->contents, CLUTTER_ACTOR (clone));

  if (self->windows->len > 0)
    shell_window_clone_set_stack_above (clone,
                                        g_ptr_array_index (self->windows,
                                                           self->windows->len - 1));

  g_ptr_array_add (self->windows, g_object_ref (clone));
}

typedef struct
{
  GWeakRef thumbnail;
  MetaWindow *window;   /* strong */
} PendingAdd;

static void
pending_add_free (PendingAdd *pending)
{
  g_weak_ref_clear (&pending->thumbnail);
  g_clear_object (&pending->window);
  g_free (pending);
}

static gboolean
pending_add_cb (gpointer user_data)
{
  PendingAdd *pending = user_data;
  g_autoptr (ShellWorkspaceThumbnail) self = g_weak_ref_get (&pending->thumbnail);

  if (self != NULL && !self->removed &&
      meta_window_get_compositor_private (pending->window) != NULL &&
      meta_window_get_workspace (pending->window) == self->meta_workspace)
    thumbnail_do_add_window (self, pending->window);

  return G_SOURCE_REMOVE;
}

static void
thumbnail_do_remove_window (ShellWorkspaceThumbnail *self,
                            MetaWindow              *window)
{
  g_autoptr (ShellWindowClone) clone = thumbnail_steal_window_clone (self, window);

  if (clone != NULL)
    clutter_actor_destroy (CLUTTER_ACTOR (clone));
}

static void
thumbnail_do_add_window (ShellWorkspaceThumbnail *self,
                         MetaWindow              *window)
{
  GObject *actor;

  if (self->removed)
    return;

  actor = meta_window_get_compositor_private (window);

  if (actor == NULL)
    {
      /* Newly-created windows are added to a workspace before
       * the compositor finds out about them... */
      PendingAdd *pending = g_new0 (PendingAdd, 1);
      guint id;

      g_weak_ref_init (&pending->thumbnail, self);
      pending->window = g_object_ref (window);
      id = g_idle_add_full (G_PRIORITY_DEFAULT, pending_add_cb, pending,
                            (GDestroyNotify) pending_add_free);
      g_source_set_name_by_id (id, "[gnome-shell] thumbnail_do_add_window");
      return;
    }

  if (thumbnail_find_tracked_window (self, window) == NULL)
    thumbnail_track_window (self, window);

  /* We might have the window in our list already if it was on all workspaces and
   * now was moved to this workspace */
  if (thumbnail_lookup_index (self, window) != -1)
    return;

  if (!thumbnail_is_my_window (self, window))
    return;

  if (thumbnail_is_overview_window (window))
    {
      thumbnail_add_window_clone (self, META_WINDOW_ACTOR (actor));
    }
  else if (meta_window_is_attached_dialog (window))
    {
      MetaWindow *parent = meta_window_get_transient_for (window);
      int index;

      while (parent != NULL && meta_window_is_attached_dialog (parent))
        parent = meta_window_get_transient_for (parent);

      if (parent == NULL)
        return;

      index = thumbnail_lookup_index (self, parent);
      if (index < 0)
        {
          /* parent was not created yet, it will take care
           * of the dialog when created */
          return;
        }

      shell_window_clone_add_attached_dialog (g_ptr_array_index (self->windows, index),
                                              window);
    }
}

static void
thumbnail_window_added_cb (MetaWorkspace            *workspace,
                           MetaWindow               *window,
                           ShellWorkspaceThumbnail  *self)
{
  thumbnail_do_add_window (self, window);
}

static void
thumbnail_window_removed_cb (MetaWorkspace            *workspace,
                             MetaWindow               *window,
                             ShellWorkspaceThumbnail  *self)
{
  guint i;

  for (i = 0; i < self->all_windows->len; i++)
    {
      TrackedWindow *tracked = g_ptr_array_index (self->all_windows, i);

      if (tracked->window == window)
        {
          g_ptr_array_remove_index (self->all_windows, i);
          break;
        }
    }

  thumbnail_do_remove_window (self, window);
}

static void
thumbnail_window_entered_monitor_cb (MetaDisplay             *display,
                                     int                      monitor,
                                     MetaWindow              *window,
                                     ShellWorkspaceThumbnail *self)
{
  if (monitor == self->monitor_index)
    thumbnail_do_add_window (self, window);
}

static void
thumbnail_window_left_monitor_cb (MetaDisplay             *display,
                                  int                      monitor,
                                  MetaWindow              *window,
                                  ShellWorkspaceThumbnail *self)
{
  if (monitor == self->monitor_index)
    thumbnail_do_remove_window (self, window);
}

/* Orders the clones like the windows are stacked. @indices maps the stable
 * sequence of a window to its index in the stack. */
static int
thumbnail_compare_clones (gconstpointer a,
                          gconstpointer b,
                          gpointer      user_data)
{
  GHashTable *indices = user_data;
  gpointer index_a = NULL, index_b = NULL;

  g_hash_table_lookup_extended (indices,
                                GUINT_TO_POINTER (meta_window_get_stable_sequence (shell_window_clone_get_meta_window ((ShellWindowClone *) a))),
                                NULL, &index_a);
  g_hash_table_lookup_extended (indices,
                                GUINT_TO_POINTER (meta_window_get_stable_sequence (shell_window_clone_get_meta_window ((ShellWindowClone *) b))),
                                NULL, &index_b);

  return (int) GPOINTER_TO_UINT (index_a) - (int) GPOINTER_TO_UINT (index_b);
}

static void
thumbnail_sync_stacking (ShellWorkspaceThumbnail *self,
                         GHashTable              *indices)
{
  guint i;

  g_ptr_array_sort_values_with_data (self->windows, thumbnail_compare_clones, indices);

  for (i = 1; i < self->windows->len; i++)
    shell_window_clone_set_stack_above (g_ptr_array_index (self->windows, i),
                                        g_ptr_array_index (self->windows, i - 1));
}

static void
shell_workspace_thumbnail_destroy (ClutterActor *actor)
{
  ShellWorkspaceThumbnail *self = SHELL_WORKSPACE_THUMBNAIL (actor);

  shell_workspace_thumbnail_workspace_removed (self);
  g_ptr_array_set_size (self->windows, 0);

  CLUTTER_ACTOR_CLASS (shell_workspace_thumbnail_parent_class)->destroy (actor);
}

static void
shell_workspace_thumbnail_constructed (GObject *object)
{
  ShellWorkspaceThumbnail *self = SHELL_WORKSPACE_THUMBNAIL (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellGlobal *global = shell_global_get ();
  ShellLayoutManager *layout_manager =
    shell_main_get_layout_manager (shell_main_get_default ());
  MtkRectangle work_area = { 0, };
  GList *window_actors, *l;

  G_OBJECT_CLASS (shell_workspace_thumbnail_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (self), "workspace-thumbnail");
  clutter_actor_set_clip_to_allocation (actor, TRUE);
  clutter_actor_set_pivot_point (actor, 0.5f, 0.5f);

  self->viewport = clutter_actor_new ();
  clutter_actor_add_child (actor, self->viewport);
  g_object_add_weak_pointer (G_OBJECT (self->viewport), (gpointer *) &self->viewport);

  self->contents = clutter_actor_new ();
  clutter_actor_add_child (self->viewport, self->contents);
  g_object_add_weak_pointer (G_OBJECT (self->contents), (gpointer *) &self->contents);

  g_set_weak_pointer (&self->display, shell_global_get_display (global));

  if (layout_manager != NULL)
    shell_layout_manager_get_work_area_for_monitor (layout_manager,
                                                    self->monitor_index,
                                                    &work_area);
  shell_workspace_thumbnail_set_porthole (self, work_area.x, work_area.y,
                                          work_area.width, work_area.height);

  /* Create clones for windows that should be visible in the Overview */
  window_actors = shell_global_get_window_actors (global);
  for (l = window_actors; l != NULL; l = l->next)
    {
      MetaWindowActor *window_actor = l->data;
      MetaWindow *window = meta_window_actor_get_meta_window (window_actor);

      if (window == NULL ||
          !meta_window_located_on_workspace (window, self->meta_workspace))
        continue;

      thumbnail_track_window (self, window);

      if (thumbnail_is_my_window (self, window) &&
          thumbnail_is_overview_window (window))
        thumbnail_add_window_clone (self, window_actor);
    }
  g_list_free (window_actors);

  /* Track window changes */
  self->window_added_id =
    g_signal_connect_object (self->meta_workspace, "window-added",
                             G_CALLBACK (thumbnail_window_added_cb), self, 0);
  self->window_removed_id =
    g_signal_connect_object (self->meta_workspace, "window-removed",
                             G_CALLBACK (thumbnail_window_removed_cb), self, 0);
  if (self->display != NULL)
    {
      self->entered_monitor_id =
        g_signal_connect_object (self->display, "window-entered-monitor",
                                 G_CALLBACK (thumbnail_window_entered_monitor_cb),
                                 self, 0);
      self->left_monitor_id =
        g_signal_connect_object (self->display, "window-left-monitor",
                                 G_CALLBACK (thumbnail_window_left_monitor_cb),
                                 self, 0);
    }

  self->state = SHELL_THUMBNAIL_STATE_NORMAL;
}

static void
shell_workspace_thumbnail_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellWorkspaceThumbnail *self = SHELL_WORKSPACE_THUMBNAIL (object);

  switch (prop_id)
    {
    case THUMB_PROP_META_WORKSPACE:
      g_set_object (&self->meta_workspace, g_value_get_object (value));
      break;
    case THUMB_PROP_MONITOR_INDEX:
      self->monitor_index = g_value_get_int (value);
      break;
    case THUMB_PROP_WINDOW_MANAGER:
      g_set_weak_pointer (&self->window_manager, g_value_get_object (value));
      break;
    case THUMB_PROP_STATE:
      shell_workspace_thumbnail_set_state (self, g_value_get_enum (value));
      break;
    case THUMB_PROP_SLIDE_POSITION:
      shell_workspace_thumbnail_set_slide_position (self, g_value_get_double (value));
      break;
    case THUMB_PROP_COLLAPSE_FRACTION:
      shell_workspace_thumbnail_set_collapse_fraction (self, g_value_get_double (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_workspace_thumbnail_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellWorkspaceThumbnail *self = SHELL_WORKSPACE_THUMBNAIL (object);

  switch (prop_id)
    {
    case THUMB_PROP_META_WORKSPACE:
      g_value_set_object (value, self->meta_workspace);
      break;
    case THUMB_PROP_MONITOR_INDEX:
      g_value_set_int (value, self->monitor_index);
      break;
    case THUMB_PROP_WINDOW_MANAGER:
      g_value_set_object (value, self->window_manager);
      break;
    case THUMB_PROP_STATE:
      g_value_set_enum (value, self->state);
      break;
    case THUMB_PROP_SLIDE_POSITION:
      g_value_set_double (value, self->slide_position);
      break;
    case THUMB_PROP_COLLAPSE_FRACTION:
      g_value_set_double (value, self->collapse_fraction);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_workspace_thumbnail_dispose (GObject *object)
{
  ShellWorkspaceThumbnail *self = SHELL_WORKSPACE_THUMBNAIL (object);

  /* Destroys the actor, which disconnects from the workspace and the display */
  G_OBJECT_CLASS (shell_workspace_thumbnail_parent_class)->dispose (object);

  g_clear_weak_pointer (&self->viewport);
  g_clear_weak_pointer (&self->contents);
  g_clear_weak_pointer (&self->display);
  g_clear_weak_pointer (&self->window_manager);
  g_clear_object (&self->meta_workspace);
}

static void
shell_workspace_thumbnail_finalize (GObject *object)
{
  ShellWorkspaceThumbnail *self = SHELL_WORKSPACE_THUMBNAIL (object);

  g_clear_pointer (&self->windows, g_ptr_array_unref);
  g_clear_pointer (&self->all_windows, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_workspace_thumbnail_parent_class)->finalize (object);
}

static void
shell_workspace_thumbnail_class_init (ShellWorkspaceThumbnailClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_workspace_thumbnail_constructed;
  object_class->set_property = shell_workspace_thumbnail_set_property;
  object_class->get_property = shell_workspace_thumbnail_get_property;
  object_class->dispose = shell_workspace_thumbnail_dispose;
  object_class->finalize = shell_workspace_thumbnail_finalize;

  actor_class->destroy = shell_workspace_thumbnail_destroy;

  /**
   * ShellWorkspaceThumbnail:meta-workspace:
   *
   * The workspace that the thumbnail shows (`metaWorkspace`).
   */
  thumb_props[THUMB_PROP_META_WORKSPACE] =
    g_param_spec_object ("meta-workspace", NULL, NULL,
                         META_TYPE_WORKSPACE,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWorkspaceThumbnail:monitor-index:
   *
   * The monitor whose windows the thumbnail shows (`monitorIndex`).
   */
  thumb_props[THUMB_PROP_MONITOR_INDEX] =
    g_param_spec_int ("monitor-index", NULL, NULL,
                      G_MININT, G_MAXINT, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellWorkspaceThumbnail:window-manager:
   *
   * The window manager whose #ShellWindowManager::overview-hide-requested
   * is emitted when the thumbnail of the active workspace is activated
   * (`Main.overview.hide ()`).
   */
  thumb_props[THUMB_PROP_WINDOW_MANAGER] =
    g_param_spec_object ("window-manager", NULL, NULL,
                         SHELL_TYPE_WINDOW_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWorkspaceThumbnail:state:
   *
   * Where the thumbnail is in its life (`state`).
   */
  thumb_props[THUMB_PROP_STATE] =
    g_param_spec_enum ("state", NULL, NULL,
                       SHELL_TYPE_THUMBNAIL_STATE,
                       SHELL_THUMBNAIL_STATE_NORMAL,
                       G_PARAM_READWRITE |
                       G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellWorkspaceThumbnail:slide-position:
   *
   * How far the thumbnail is slid out, from 0 (slid in) to 1 (slid out).
   * Sliding scales the thumbnail down and fades it out.
   */
  thumb_props[THUMB_PROP_SLIDE_POSITION] =
    g_param_spec_double ("slide-position", NULL, NULL,
                         0, 1, 0,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellWorkspaceThumbnail:collapse-fraction:
   *
   * How far the thumbnail is collapsed, from 0 (it has its width) to 1 (no
   * width).
   */
  thumb_props[THUMB_PROP_COLLAPSE_FRACTION] =
    g_param_spec_double ("collapse-fraction", NULL, NULL,
                         0, 1, 0,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, THUMB_N_PROPS, thumb_props);

  /**
   * ShellWorkspaceThumbnail::window-drag-begin:
   * @self: the #ShellWorkspaceThumbnail
   * @window: the window whose clone starts being dragged
   *
   * Emitted when the clone of a window of the thumbnail starts being dragged
   * (the clone's `Main.overview.beginWindowDrag ()`).
   */
  thumb_signals[THUMB_WINDOW_DRAG_BEGIN] =
    g_signal_new ("window-drag-begin",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  META_TYPE_WINDOW);

  /**
   * ShellWorkspaceThumbnail::window-drag-cancelled:
   * @self: the #ShellWorkspaceThumbnail
   * @window: the window whose clone was dragged
   *
   * Emitted when the drag of a clone of the thumbnail is cancelled.
   */
  thumb_signals[THUMB_WINDOW_DRAG_CANCELLED] =
    g_signal_new ("window-drag-cancelled",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  META_TYPE_WINDOW);

  /**
   * ShellWorkspaceThumbnail::window-drag-end:
   * @self: the #ShellWorkspaceThumbnail
   * @window: the window whose clone was dragged
   *
   * Emitted when the drag of a clone of the thumbnail ends.
   */
  thumb_signals[THUMB_WINDOW_DRAG_END] =
    g_signal_new ("window-drag-end",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  META_TYPE_WINDOW);
}

static void
shell_workspace_thumbnail_init (ShellWorkspaceThumbnail *self)
{
  self->state = SHELL_THUMBNAIL_STATE_NORMAL;
  self->windows = g_ptr_array_new_with_free_func (g_object_unref);
  self->all_windows = g_ptr_array_new_with_free_func ((GDestroyNotify) tracked_window_free);
}

/**
 * shell_workspace_thumbnail_new:
 * @meta_workspace: the workspace to show
 * @monitor_index: the monitor whose windows are shown
 * @window_manager: (nullable): the window manager to ask to hide the overview
 *
 * Creates the thumbnail of the windows of @meta_workspace on a monitor.
 *
 * Returns: (transfer none): a new #ShellWorkspaceThumbnail, which is floating
 */
ShellWorkspaceThumbnail *
shell_workspace_thumbnail_new (MetaWorkspace      *meta_workspace,
                               int                 monitor_index,
                               ShellWindowManager *window_manager)
{
  g_return_val_if_fail (META_IS_WORKSPACE (meta_workspace), NULL);

  return g_object_new (SHELL_TYPE_WORKSPACE_THUMBNAIL,
                       "meta-workspace", meta_workspace,
                       "monitor-index", monitor_index,
                       "window-manager", window_manager,
                       NULL);
}

/**
 * shell_workspace_thumbnail_get_meta_workspace:
 * @self: a #ShellWorkspaceThumbnail
 *
 * Returns: (transfer none): the workspace that the thumbnail shows
 */
MetaWorkspace *
shell_workspace_thumbnail_get_meta_workspace (ShellWorkspaceThumbnail *self)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACE_THUMBNAIL (self), NULL);

  return self->meta_workspace;
}

/**
 * shell_workspace_thumbnail_get_monitor_index:
 * @self: a #ShellWorkspaceThumbnail
 *
 * Returns: the monitor whose windows the thumbnail shows
 */
int
shell_workspace_thumbnail_get_monitor_index (ShellWorkspaceThumbnail *self)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACE_THUMBNAIL (self), -1);

  return self->monitor_index;
}

/**
 * shell_workspace_thumbnail_get_state:
 * @self: a #ShellWorkspaceThumbnail
 *
 * Returns: where the thumbnail is in its life
 */
ShellThumbnailState
shell_workspace_thumbnail_get_state (ShellWorkspaceThumbnail *self)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACE_THUMBNAIL (self), SHELL_THUMBNAIL_STATE_NORMAL);

  return self->state;
}

/**
 * shell_workspace_thumbnail_set_state:
 * @self: a #ShellWorkspaceThumbnail
 * @state: the new state
 *
 * Sets where the thumbnail is in its life. This does not animate anything,
 * #ShellThumbnailsBox does when it updates the states.
 */
void
shell_workspace_thumbnail_set_state (ShellWorkspaceThumbnail *self,
                                     ShellThumbnailState      state)
{
  g_return_if_fail (SHELL_IS_WORKSPACE_THUMBNAIL (self));

  if (self->state == state)
    return;

  self->state = state;
  g_object_notify_by_pspec (G_OBJECT (self), thumb_props[THUMB_PROP_STATE]);
}

/**
 * shell_workspace_thumbnail_get_slide_position:
 * @self: a #ShellWorkspaceThumbnail
 *
 * Returns: how far the thumbnail is slid out, from 0 to 1
 */
double
shell_workspace_thumbnail_get_slide_position (ShellWorkspaceThumbnail *self)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACE_THUMBNAIL (self), 0);

  return self->slide_position;
}

/**
 * shell_workspace_thumbnail_set_slide_position:
 * @self: a #ShellWorkspaceThumbnail
 * @slide_position: how far to slide the thumbnail out, from 0 to 1
 *
 * Scales the thumbnail down to 75% and fades it out as it slides out.
 */
void
shell_workspace_thumbnail_set_slide_position (ShellWorkspaceThumbnail *self,
                                              double                   slide_position)
{
  ClutterActor *actor;
  double scale;

  g_return_if_fail (SHELL_IS_WORKSPACE_THUMBNAIL (self));

  if (self->slide_position == slide_position)
    return;

  actor = CLUTTER_ACTOR (self);

  scale = lerp (1, 0.75, slide_position);
  clutter_actor_set_scale (actor, scale, scale);
  clutter_actor_set_opacity (actor, (guint8) lerp (255, 0, slide_position));

  self->slide_position = slide_position;
  g_object_notify_by_pspec (G_OBJECT (self), thumb_props[THUMB_PROP_SLIDE_POSITION]);
  clutter_actor_queue_relayout (actor);
}

/**
 * shell_workspace_thumbnail_get_collapse_fraction:
 * @self: a #ShellWorkspaceThumbnail
 *
 * Returns: how far the thumbnail is collapsed, from 0 to 1
 */
double
shell_workspace_thumbnail_get_collapse_fraction (ShellWorkspaceThumbnail *self)
{
  g_return_val_if_fail (SHELL_IS_WORKSPACE_THUMBNAIL (self), 0);

  return self->collapse_fraction;
}

/**
 * shell_workspace_thumbnail_set_collapse_fraction:
 * @self: a #ShellWorkspaceThumbnail
 * @collapse_fraction: how far to collapse the thumbnail, from 0 to 1
 *
 * Sets how much of its width the thumbnail gives up in the strip.
 */
void
shell_workspace_thumbnail_set_collapse_fraction (ShellWorkspaceThumbnail *self,
                                                 double                   collapse_fraction)
{
  g_return_if_fail (SHELL_IS_WORKSPACE_THUMBNAIL (self));

  if (self->collapse_fraction == collapse_fraction)
    return;

  self->collapse_fraction = collapse_fraction;
  g_object_notify_by_pspec (G_OBJECT (self), thumb_props[THUMB_PROP_COLLAPSE_FRACTION]);
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

/**
 * shell_workspace_thumbnail_set_porthole:
 * @self: a #ShellWorkspaceThumbnail
 * @x: the x coordinate of the part of the screen to show
 * @y: the y coordinate of the part of the screen to show
 * @width: the width of the part of the screen to show
 * @height: the height of the part of the screen to show
 *
 * Sets the part of the screen that the thumbnail shows (`setPorthole ()`).
 */
void
shell_workspace_thumbnail_set_porthole (ShellWorkspaceThumbnail *self,
                                        float                    x,
                                        float                    y,
                                        float                    width,
                                        float                    height)
{
  g_return_if_fail (SHELL_IS_WORKSPACE_THUMBNAIL (self));

  if (self->viewport == NULL || self->contents == NULL)
    return;

  clutter_actor_set_size (self->viewport, width, height);
  clutter_actor_set_position (self->contents, -x, -y);
}

/**
 * shell_workspace_thumbnail_set_scale:
 * @self: a #ShellWorkspaceThumbnail
 * @scale_x: the horizontal scale of the contents
 * @scale_y: the vertical scale of the contents
 *
 * Scales the windows of the thumbnail (`setScale ()`).
 */
void
shell_workspace_thumbnail_set_scale (ShellWorkspaceThumbnail *self,
                                     double                   scale_x,
                                     double                   scale_y)
{
  g_return_if_fail (SHELL_IS_WORKSPACE_THUMBNAIL (self));

  if (self->viewport != NULL)
    clutter_actor_set_scale (self->viewport, scale_x, scale_y);
}

/**
 * shell_workspace_thumbnail_activate:
 * @self: a #ShellWorkspaceThumbnail
 * @time: the timestamp of the event that activates the thumbnail
 *
 * Switches to the workspace of the thumbnail. A click on the thumbnail of
 * the current workspace goes back to the main view, which emits
 * #ShellWindowManager::overview-hide-requested.
 */
void
shell_workspace_thumbnail_activate (ShellWorkspaceThumbnail *self,
                                    guint32                  time)
{
  gboolean active = FALSE;

  g_return_if_fail (SHELL_IS_WORKSPACE_THUMBNAIL (self));

  if (self->state > SHELL_THUMBNAIL_STATE_NORMAL)
    return;

  g_object_get (self->meta_workspace, "active", &active, NULL);

  /* a click on the already current workspace should go back to the main view */
  if (active)
    {
      if (self->window_manager != NULL)
        g_signal_emit_by_name (self->window_manager, "overview-hide-requested");
    }
  else
    {
      meta_workspace_activate (self->meta_workspace, time);
    }
}

/**
 * shell_workspace_thumbnail_workspace_removed:
 * @self: a #ShellWorkspaceThumbnail
 *
 * Tells the thumbnail that its workspace is gone: it stops following the
 * windows (`workspaceRemoved ()`).
 */
void
shell_workspace_thumbnail_workspace_removed (ShellWorkspaceThumbnail *self)
{
  guint i;

  g_return_if_fail (SHELL_IS_WORKSPACE_THUMBNAIL (self));

  if (self->removed)
    return;

  self->removed = TRUE;

  g_clear_signal_handler (&self->window_added_id, self->meta_workspace);
  g_clear_signal_handler (&self->window_removed_id, self->meta_workspace);
  if (self->display != NULL)
    {
      g_clear_signal_handler (&self->entered_monitor_id, self->display);
      g_clear_signal_handler (&self->left_monitor_id, self->display);
    }

  for (i = 0; i < self->all_windows->len; i++)
    {
      TrackedWindow *tracked = g_ptr_array_index (self->all_windows, i);

      g_clear_signal_handler (&tracked->minimized_id, tracked->window);
    }
}

/* ------------------------------------------------------------------------ */
/* ShellThumbnailsBox (ThumbnailsBox)                                        */
/* ------------------------------------------------------------------------ */

enum
{
  BOX_PROP_0,
  BOX_PROP_SCROLL_ADJUSTMENT,
  BOX_PROP_MONITOR_INDEX,
  BOX_PROP_WINDOW_MANAGER,
  BOX_PROP_EXPAND_FRACTION,
  BOX_PROP_SCALE,
  BOX_PROP_SHOULD_SHOW,
  BOX_PROP_MAX_THUMBNAIL_SCALE,
  BOX_N_PROPS
};

static GParamSpec *box_props[BOX_N_PROPS] = { NULL, };

enum
{
  BOX_WINDOW_DRAG_BEGIN,
  BOX_WINDOW_DRAG_CANCELLED,
  BOX_WINDOW_DRAG_END,
  BOX_LAST_SIGNAL
};

static guint box_signals[BOX_LAST_SIGNAL] = { 0 };

struct _ShellThumbnailsBox
{
  StWidget parent;

  StAdjustment *scroll_adjustment;           /* strong */
  ShellWindowManager *window_manager;        /* weak */
  ShellGlobal *global;                       /* weak */
  MetaDisplay *display;                      /* weak */
  MetaWorkspaceManager *workspace_manager;   /* weak */
  MetaLaters *laters;                        /* weak */
  ShellLayoutManager *layout_manager;        /* weak */
  GSettings *settings;
  int monitor_index;

  ClutterActor *indicator;                   /* weak */
  ClutterActor *drop_placeholder;            /* weak */
  int drop_workspace;
  int drop_placeholder_pos;
  int splice_index;
  guint drop_placeholder_later;

  double max_thumbnail_scale;
  double target_scale;
  double scale;
  double expand_fraction;
  guint update_state_id;
  gboolean pending_scale_update;
  gboolean animating_indicator;
  gboolean should_show;

  int state_counts[N_THUMBNAIL_STATES];
  GPtrArray *thumbnails;                     /* ShellWorkspaceThumbnail */

  /* The part of the screen that is shown in the thumbnails */
  double porthole_x;
  double porthole_y;
  double porthole_width;
  double porthole_height;

  guint drag_monitor;
  gboolean drag_cancelled;

  gulong n_workspaces_id;
  gulong active_workspace_id;
  gulong reordered_id;
  gulong restacked_id;
};

static void shell_thumbnails_box_drag_target_init (ShellDragTargetInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellThumbnailsBox, shell_thumbnails_box, ST_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (SHELL_TYPE_DRAG_TARGET,
                                                      shell_thumbnails_box_drag_target_init))

static void box_queue_update_states (ShellThumbnailsBox *self);

/* A drop source, the thing that is dragged over the box */
typedef struct
{
  GObject *source;
  gboolean xdnd;          /* the drag of an X11 client */
  MetaWindow *window;     /* strong, the window that is dragged */
  ShellApp *app;          /* strong, the application that is dragged */
} DropSource;

static void
drop_source_clear (DropSource *src)
{
  g_clear_object (&src->window);
  g_clear_object (&src->app);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (DropSource, drop_source_clear)

static void
box_classify_source (GObject    *source,
                     DropSource *src)
{
  src->source = source;
  src->xdnd = SHELL_IS_XDND_HANDLER (source);
  src->window = NULL;
  src->app = NULL;

  if (src->xdnd)
    return;

  /* The window that is dragged: the "meta-window" property of the clones
   * and of the window previews */
  if (SHELL_IS_WINDOW_CLONE (source))
    {
      src->window = g_object_ref (shell_window_clone_get_meta_window (SHELL_WINDOW_CLONE (source)));
    }
  else
    {
      GParamSpec *pspec =
        g_object_class_find_property (G_OBJECT_GET_CLASS (source), "meta-window");

      if (pspec != NULL && g_type_is_a (pspec->value_type, META_TYPE_WINDOW))
        g_object_get (source, "meta-window", &src->window, NULL);
    }

  /* The application that is dragged: the launchers implement ShellDragSource */
  if (SHELL_IS_DRAG_SOURCE (source))
    {
      ShellApp *app = shell_drag_source_get_app (SHELL_DRAG_SOURCE (source));

      if (app != NULL)
        src->app = g_object_ref (app);
    }
}

static gboolean
drop_source_can_open_new_window (const DropSource *src)
{
  return src->app != NULL && shell_app_can_open_new_window (src->app);
}

static void
box_launch_app (const DropSource *src,
                ClutterActor     *drag_actor,
                int               workspace_index)
{
  if (drag_actor != NULL && SHELL_IS_DRAG_SOURCE (src->source))
    shell_drag_source_animate_launch_at_pos (SHELL_DRAG_SOURCE (src->source),
                                             clutter_actor_get_x (drag_actor),
                                             clutter_actor_get_y (drag_actor));

  shell_app_open_new_window (src->app, workspace_index);
}

/* The states */

static void
box_set_thumbnail_state (ShellThumbnailsBox      *self,
                         ShellWorkspaceThumbnail *thumbnail,
                         ShellThumbnailState      state)
{
  self->state_counts[shell_workspace_thumbnail_get_state (thumbnail)]--;
  shell_workspace_thumbnail_set_state (thumbnail, state);
  self->state_counts[shell_workspace_thumbnail_get_state (thumbnail)]++;
}

static void
box_reset_state_counts (ShellThumbnailsBox *self)
{
  memset (self->state_counts, 0, sizeof (self->state_counts));
}

static void
box_update_should_show (ShellThumbnailsBox *self)
{
  gboolean should_show;
  int n_workspaces;

  if (self->settings == NULL || self->workspace_manager == NULL)
    return;

  n_workspaces = meta_workspace_manager_get_n_workspaces (self->workspace_manager);
  should_show = g_settings_get_boolean (self->settings, "dynamic-workspaces")
    ? n_workspaces > NUM_WORKSPACES_THRESHOLD
    : n_workspaces > 1;

  if (self->should_show == should_show)
    return;

  self->should_show = should_show;
  g_object_notify_by_pspec (G_OBJECT (self), box_props[BOX_PROP_SHOULD_SHOW]);
}

static void
box_update_indicator (ShellThumbnailsBox *self)
{
  double value = self->scroll_adjustment != NULL ?
    st_adjustment_get_value (self->scroll_adjustment) : 0;
  int active_index = 0;

  if (self->workspace_manager != NULL)
    active_index = meta_workspace_manager_get_active_workspace_index (self->workspace_manager);

  self->animating_indicator = value != active_index;

  if (!self->animating_indicator)
    box_queue_update_states (self);

  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

typedef struct
{
  GWeakRef box;
  GWeakRef thumbnail;
} StateEase;

static StateEase *
state_ease_new (ShellThumbnailsBox      *box,
                ShellWorkspaceThumbnail *thumbnail)
{
  StateEase *data = g_new0 (StateEase, 1);

  g_weak_ref_init (&data->box, box);
  g_weak_ref_init (&data->thumbnail, thumbnail);

  return data;
}

static void
state_ease_free (StateEase *data)
{
  g_weak_ref_clear (&data->box);
  g_weak_ref_clear (&data->thumbnail);
  g_free (data);
}

static void
ease_double (ClutterActor      *actor,
             const char        *property,
             double             target,
             int                duration,
             ShellEaseCallback  callback,
             gpointer           user_data,
             GDestroyNotify     destroy)
{
  ShellEaseParams params = {
    .duration = duration,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };
  g_auto (GValue) value = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;

  g_value_init (&value, G_TYPE_DOUBLE);
  g_value_set_double (&value, target);

  /* On failure the callback is dropped and the data freed */
  shell_animatable_ease_property (CLUTTER_ANIMATABLE (actor), property, &value,
                                  &params, callback, user_data, destroy, &error);
}

static void
animating_out_done_cb (gboolean finished,
                       gpointer user_data)
{
  StateEase *data = user_data;
  g_autoptr (ShellThumbnailsBox) box = g_weak_ref_get (&data->box);
  g_autoptr (ShellWorkspaceThumbnail) thumbnail = g_weak_ref_get (&data->thumbnail);

  if (!finished || box == NULL || thumbnail == NULL)
    return;

  box_set_thumbnail_state (box, thumbnail, SHELL_THUMBNAIL_STATE_ANIMATED_OUT);
  box_queue_update_states (box);
}

static void
collapsing_done_cb (gboolean finished,
                    gpointer user_data)
{
  StateEase *data = user_data;
  g_autoptr (ShellThumbnailsBox) box = g_weak_ref_get (&data->box);
  g_autoptr (ShellWorkspaceThumbnail) thumbnail = g_weak_ref_get (&data->thumbnail);
  guint index;

  if (!finished || box == NULL || thumbnail == NULL)
    return;

  box->state_counts[shell_workspace_thumbnail_get_state (thumbnail)]--;
  shell_workspace_thumbnail_set_state (thumbnail, SHELL_THUMBNAIL_STATE_DESTROYED);

  if (g_ptr_array_find (box->thumbnails, thumbnail, &index))
    g_ptr_array_remove_index (box->thumbnails, index);
  clutter_actor_destroy (CLUTTER_ACTOR (thumbnail));

  box_queue_update_states (box);
}

static void
expanding_done_cb (gboolean finished,
                   gpointer user_data)
{
  StateEase *data = user_data;
  g_autoptr (ShellThumbnailsBox) box = g_weak_ref_get (&data->box);
  g_autoptr (ShellWorkspaceThumbnail) thumbnail = g_weak_ref_get (&data->thumbnail);

  if (!finished || box == NULL || thumbnail == NULL)
    return;

  box_set_thumbnail_state (box, thumbnail, SHELL_THUMBNAIL_STATE_EXPANDED);
  box_queue_update_states (box);
}

static void
animating_in_done_cb (gboolean finished,
                      gpointer user_data)
{
  StateEase *data = user_data;
  g_autoptr (ShellThumbnailsBox) box = g_weak_ref_get (&data->box);
  g_autoptr (ShellWorkspaceThumbnail) thumbnail = g_weak_ref_get (&data->thumbnail);

  if (!finished || box == NULL || thumbnail == NULL)
    return;

  box_set_thumbnail_state (box, thumbnail, SHELL_THUMBNAIL_STATE_NORMAL);
}

static void
scale_ease_done_cb (gboolean finished,
                    gpointer user_data)
{
  g_autoptr (ShellThumbnailsBox) box = g_weak_ref_get (user_data);

  if (!finished || box == NULL)
    return;

  box_queue_update_states (box);
}

static void
weak_ref_free (GWeakRef *weak_ref)
{
  g_weak_ref_clear (weak_ref);
  g_free (weak_ref);
}

/* Takes each thumbnail in @state through @func, which can remove it. The
 * list is walked by index and its length checked every time, as the JS
 * did. */
#define FOREACH_STATE_THUMBNAIL(self, state, thumbnail)                      \
  for (guint _i = 0; _i < (self)->thumbnails->len; _i++)                     \
    for (g_autoptr (ShellWorkspaceThumbnail) thumbnail =                     \
           g_object_ref (g_ptr_array_index ((self)->thumbnails, _i));        \
         thumbnail != NULL;                                                  \
         (g_object_unref (thumbnail), thumbnail = NULL))                     \
      if (shell_workspace_thumbnail_get_state (thumbnail) == (state))

static void
box_update_states (ShellThumbnailsBox *self)
{
  self->update_state_id = 0;

  /* If we are animating the indicator, wait */
  if (self->animating_indicator)
    return;

  /* Likewise if we are in the process of hiding */
  if (!self->should_show && clutter_actor_is_visible (CLUTTER_ACTOR (self)))
    return;

  /* Then slide out any thumbnails that have been destroyed */
  if (self->state_counts[SHELL_THUMBNAIL_STATE_REMOVING] != 0)
    {
      FOREACH_STATE_THUMBNAIL (self, SHELL_THUMBNAIL_STATE_REMOVING, thumbnail)
        {
          box_set_thumbnail_state (self, thumbnail, SHELL_THUMBNAIL_STATE_ANIMATING_OUT);
          ease_double (CLUTTER_ACTOR (thumbnail), "slide-position", 1,
                       SLIDE_ANIMATION_TIME, animating_out_done_cb,
                       state_ease_new (self, thumbnail),
                       (GDestroyNotify) state_ease_free);
        }
    }

  /* As long as things are sliding out, don't proceed */
  if (self->state_counts[SHELL_THUMBNAIL_STATE_ANIMATING_OUT] > 0)
    return;

  /* Once that's complete, we can start scaling to the new size,
   * collapse any removed thumbnails and expand added ones */
  if (self->state_counts[SHELL_THUMBNAIL_STATE_ANIMATED_OUT] != 0)
    {
      FOREACH_STATE_THUMBNAIL (self, SHELL_THUMBNAIL_STATE_ANIMATED_OUT, thumbnail)
        {
          box_set_thumbnail_state (self, thumbnail, SHELL_THUMBNAIL_STATE_COLLAPSING);
          ease_double (CLUTTER_ACTOR (thumbnail), "collapse-fraction", 1,
                       RESCALE_ANIMATION_TIME, collapsing_done_cb,
                       state_ease_new (self, thumbnail),
                       (GDestroyNotify) state_ease_free);
        }
    }

  if (self->state_counts[SHELL_THUMBNAIL_STATE_NEW] != 0)
    {
      FOREACH_STATE_THUMBNAIL (self, SHELL_THUMBNAIL_STATE_NEW, thumbnail)
        {
          box_set_thumbnail_state (self, thumbnail, SHELL_THUMBNAIL_STATE_EXPANDING);
          ease_double (CLUTTER_ACTOR (thumbnail), "collapse-fraction", 0,
                       SLIDE_ANIMATION_TIME, expanding_done_cb,
                       state_ease_new (self, thumbnail),
                       (GDestroyNotify) state_ease_free);
        }
    }

  if (self->pending_scale_update)
    {
      GWeakRef *box_ref = g_new0 (GWeakRef, 1);

      g_weak_ref_init (box_ref, self);
      self->pending_scale_update = FALSE;
      ease_double (CLUTTER_ACTOR (self), "scale", self->target_scale,
                   RESCALE_ANIMATION_TIME, scale_ease_done_cb,
                   box_ref, (GDestroyNotify) weak_ref_free);
    }

  /* Wait until that's done */
  if (self->scale != self->target_scale ||
      self->state_counts[SHELL_THUMBNAIL_STATE_COLLAPSING] > 0 ||
      self->state_counts[SHELL_THUMBNAIL_STATE_EXPANDING] > 0)
    return;

  /* And then slide in any new thumbnails */
  if (self->state_counts[SHELL_THUMBNAIL_STATE_EXPANDED] != 0)
    {
      FOREACH_STATE_THUMBNAIL (self, SHELL_THUMBNAIL_STATE_EXPANDED, thumbnail)
        {
          box_set_thumbnail_state (self, thumbnail, SHELL_THUMBNAIL_STATE_ANIMATING_IN);
          ease_double (CLUTTER_ACTOR (thumbnail), "slide-position", 0,
                       SLIDE_ANIMATION_TIME, animating_in_done_cb,
                       state_ease_new (self, thumbnail),
                       (GDestroyNotify) state_ease_free);
        }
    }
}

static gboolean
box_update_states_later_cb (gpointer user_data)
{
  box_update_states (user_data);

  return G_SOURCE_REMOVE;
}

static void
box_queue_update_states (ShellThumbnailsBox *self)
{
  if (self->update_state_id > 0 || self->laters == NULL)
    return;

  self->update_state_id = meta_laters_add (self->laters,
                                           META_LATER_BEFORE_REDRAW,
                                           box_update_states_later_cb,
                                           self, NULL);
}

static void
box_unqueue_update_states (ShellThumbnailsBox *self)
{
  if (self->update_state_id != 0 && self->laters != NULL)
    meta_laters_remove (self->laters, self->update_state_id);

  self->update_state_id = 0;
}

/* The thumbnails */

static void
box_thumbnail_window_drag_begin_cb (ShellWorkspaceThumbnail *thumbnail,
                                    MetaWindow              *window,
                                    ShellThumbnailsBox      *self)
{
  g_signal_emit (self, box_signals[BOX_WINDOW_DRAG_BEGIN], 0, window);
}

static void
box_thumbnail_window_drag_cancelled_cb (ShellWorkspaceThumbnail *thumbnail,
                                        MetaWindow              *window,
                                        ShellThumbnailsBox      *self)
{
  g_signal_emit (self, box_signals[BOX_WINDOW_DRAG_CANCELLED], 0, window);
}

static void
box_thumbnail_window_drag_end_cb (ShellWorkspaceThumbnail *thumbnail,
                                  MetaWindow              *window,
                                  ShellThumbnailsBox      *self)
{
  g_signal_emit (self, box_signals[BOX_WINDOW_DRAG_END], 0, window);
}

/**
 * shell_thumbnails_box_add_thumbnails:
 * @self: a #ShellThumbnailsBox
 * @start: the index of the first workspace to add a thumbnail for
 * @count: the number of thumbnails to add
 *
 * Adds the thumbnails of @count workspaces starting at @start
 * (`addThumbnails ()`). Unless it is the initial fill or the workspace was
 * created by dropping on the strip, the new thumbnails slide in.
 */
void
shell_thumbnails_box_add_thumbnails (ShellThumbnailsBox *self,
                                     int                 start,
                                     int                 count)
{
  int k;

  g_return_if_fail (SHELL_IS_THUMBNAILS_BOX (self));

  if (self->workspace_manager == NULL)
    return;

  for (k = start; k < start + count; k++)
    {
      MetaWorkspace *meta_workspace =
        meta_workspace_manager_get_workspace_by_index (self->workspace_manager, k);
      ShellWorkspaceThumbnail *thumbnail;

      if (meta_workspace == NULL)
        continue;

      thumbnail = shell_workspace_thumbnail_new (meta_workspace,
                                                 self->monitor_index,
                                                 self->window_manager);
      shell_workspace_thumbnail_set_porthole (thumbnail,
                                              self->porthole_x, self->porthole_y,
                                              self->porthole_width, self->porthole_height);
      g_signal_connect_object (thumbnail, "window-drag-begin",
                               G_CALLBACK (box_thumbnail_window_drag_begin_cb), self, 0);
      g_signal_connect_object (thumbnail, "window-drag-cancelled",
                               G_CALLBACK (box_thumbnail_window_drag_cancelled_cb), self, 0);
      g_signal_connect_object (thumbnail, "window-drag-end",
                               G_CALLBACK (box_thumbnail_window_drag_end_cb), self, 0);
      clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (thumbnail));
      g_ptr_array_add (self->thumbnails, g_object_ref (thumbnail));

      if (self->should_show && start > 0 && self->splice_index == -1)
        {
          /* not the initial fill, and not splicing via DND */
          shell_workspace_thumbnail_set_state (thumbnail, SHELL_THUMBNAIL_STATE_NEW);
          shell_workspace_thumbnail_set_slide_position (thumbnail, 1); /* start slid out */
          shell_workspace_thumbnail_set_collapse_fraction (thumbnail, 1); /* start fully collapsed */
        }
      else
        {
          shell_workspace_thumbnail_set_state (thumbnail, SHELL_THUMBNAIL_STATE_NORMAL);
        }

      self->state_counts[shell_workspace_thumbnail_get_state (thumbnail)]++;
    }

  box_queue_update_states (self);

  /* The thumbnails indicator actually needs to be on top of the thumbnails */
  clutter_actor_set_child_above_sibling (CLUTTER_ACTOR (self), self->indicator, NULL);

  /* Clear the splice index, we got the message */
  self->splice_index = -1;
}

/**
 * shell_thumbnails_box_remove_thumbnails:
 * @self: a #ShellThumbnailsBox
 * @start: the index of the first thumbnail to remove
 * @count: the number of thumbnails to remove
 *
 * Removes the thumbnails of @count workspaces starting at @start, which slide
 * out and collapse (`removeThumbnails ()`).
 */
void
shell_thumbnails_box_remove_thumbnails (ShellThumbnailsBox *self,
                                        int                 start,
                                        int                 count)
{
  int current_pos = 0;
  guint k;

  g_return_if_fail (SHELL_IS_THUMBNAILS_BOX (self));

  for (k = 0; k < self->thumbnails->len; k++)
    {
      ShellWorkspaceThumbnail *thumbnail = g_ptr_array_index (self->thumbnails, k);

      if (shell_workspace_thumbnail_get_state (thumbnail) > SHELL_THUMBNAIL_STATE_NORMAL)
        continue;

      if (current_pos >= start && current_pos < start + count)
        {
          shell_workspace_thumbnail_workspace_removed (thumbnail);
          box_set_thumbnail_state (self, thumbnail, SHELL_THUMBNAIL_STATE_REMOVING);
        }

      current_pos++;
    }

  box_queue_update_states (self);
}

static void
box_workspaces_changed (ShellThumbnailsBox *self)
{
  guint n_valid = 0;
  int old_n_workspaces, new_n_workspaces;
  guint i;

  if (self->workspace_manager == NULL)
    return;

  for (i = 0; i < self->thumbnails->len; i++)
    {
      ShellWorkspaceThumbnail *thumbnail = g_ptr_array_index (self->thumbnails, i);

      if (shell_workspace_thumbnail_get_state (thumbnail) <= SHELL_THUMBNAIL_STATE_NORMAL)
        n_valid++;
    }

  old_n_workspaces = n_valid;
  new_n_workspaces = meta_workspace_manager_get_n_workspaces (self->workspace_manager);

  if (new_n_workspaces > old_n_workspaces)
    {
      shell_thumbnails_box_add_thumbnails (self, old_n_workspaces,
                                           new_n_workspaces - old_n_workspaces);
    }
  else
    {
      int removed_index = -1;
      int removed_num = old_n_workspaces - new_n_workspaces;
      int w;

      for (w = 0; w < old_n_workspaces && (guint) w < self->thumbnails->len; w++)
        {
          MetaWorkspace *meta_workspace =
            meta_workspace_manager_get_workspace_by_index (self->workspace_manager, w);
          ShellWorkspaceThumbnail *thumbnail = g_ptr_array_index (self->thumbnails, w);

          if (shell_workspace_thumbnail_get_meta_workspace (thumbnail) != meta_workspace)
            {
              removed_index = w;
              break;
            }
        }

      /* The JS passed undefined here when nothing changed, which removed
       * nothing */
      if (removed_index != -1)
        shell_thumbnails_box_remove_thumbnails (self, removed_index, removed_num);
      else
        box_queue_update_states (self);
    }

  box_update_should_show (self);
}

static void
box_n_workspaces_changed_cb (MetaWorkspaceManager *workspace_manager,
                             GParamSpec           *pspec,
                             ShellThumbnailsBox   *self)
{
  box_workspaces_changed (self);
}

static void
box_active_workspace_changed_cb (MetaWorkspaceManager *workspace_manager,
                                 ShellThumbnailsBox   *self)
{
  box_update_indicator (self);
}

static int
compare_thumbnails_by_workspace (gconstpointer a,
                                 gconstpointer b)
{
  MetaWorkspace *workspace_a =
    shell_workspace_thumbnail_get_meta_workspace ((ShellWorkspaceThumbnail *) a);
  MetaWorkspace *workspace_b =
    shell_workspace_thumbnail_get_meta_workspace ((ShellWorkspaceThumbnail *) b);

  return meta_workspace_index (workspace_a) - meta_workspace_index (workspace_b);
}

static void
box_workspaces_reordered_cb (MetaWorkspaceManager *workspace_manager,
                             ShellThumbnailsBox   *self)
{
  g_ptr_array_sort_values (self->thumbnails, compare_thumbnails_by_workspace);
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

static void
box_restacked_cb (MetaDisplay        *display,
                  ShellThumbnailsBox *self)
{
  g_autoptr (GHashTable) indices = g_hash_table_new (NULL, NULL);
  GList *window_actors, *l;
  guint i;

  if (self->global == NULL)
    return;

  /* Use the stable sequence for an integer to use as a hash key */
  window_actors = shell_global_get_window_actors (self->global);
  for (l = window_actors, i = 0; l != NULL; l = l->next, i++)
    {
      MetaWindow *window = meta_window_actor_get_meta_window (l->data);

      if (window != NULL)
        g_hash_table_insert (indices,
                             GUINT_TO_POINTER (meta_window_get_stable_sequence (window)),
                             GUINT_TO_POINTER (i));
    }
  g_list_free (window_actors);

  for (i = 0; i < self->thumbnails->len; i++)
    thumbnail_sync_stacking (g_ptr_array_index (self->thumbnails, i), indices);
}

static void
box_create_thumbnails (ShellThumbnailsBox *self)
{
  if (self->thumbnails->len > 0 || self->workspace_manager == NULL)
    return;

  self->n_workspaces_id =
    g_signal_connect (self->workspace_manager, "notify::n-workspaces",
                      G_CALLBACK (box_n_workspaces_changed_cb), self);
  self->active_workspace_id =
    g_signal_connect (self->workspace_manager, "active-workspace-changed",
                      G_CALLBACK (box_active_workspace_changed_cb), self);
  self->reordered_id =
    g_signal_connect (self->workspace_manager, "workspaces-reordered",
                      G_CALLBACK (box_workspaces_reordered_cb), self);
  if (self->display != NULL)
    self->restacked_id =
      g_signal_connect (self->display, "restacked",
                        G_CALLBACK (box_restacked_cb), self);

  self->target_scale = 0;
  self->scale = 0;
  self->pending_scale_update = FALSE;
  box_unqueue_update_states (self);

  box_reset_state_counts (self);

  shell_thumbnails_box_add_thumbnails (self, 0,
                                       meta_workspace_manager_get_n_workspaces (self->workspace_manager));
  clutter_actor_show (self->indicator);

  box_update_should_show (self);
}

static void
box_destroy_thumbnails (ShellThumbnailsBox *self)
{
  guint w;

  if (self->thumbnails->len == 0)
    return;

  if (self->workspace_manager != NULL)
    {
      g_clear_signal_handler (&self->n_workspaces_id, self->workspace_manager);
      g_clear_signal_handler (&self->active_workspace_id, self->workspace_manager);
      g_clear_signal_handler (&self->reordered_id, self->workspace_manager);
    }
  if (self->display != NULL)
    g_clear_signal_handler (&self->restacked_id, self->display);

  for (w = 0; w < self->thumbnails->len; w++)
    clutter_actor_destroy (g_ptr_array_index (self->thumbnails, w));
  g_ptr_array_set_size (self->thumbnails, 0);

  if (self->indicator != NULL)
    clutter_actor_hide (self->indicator);
}

static void
box_overview_visible_changed_cb (ShellWindowManager *window_manager,
                                 GParamSpec         *pspec,
                                 ShellThumbnailsBox *self)
{
  /* The overview's `showing` and `hidden` signals */
  if (shell_window_manager_get_overview_visible (window_manager))
    box_create_thumbnails (self);
  else
    box_destroy_thumbnails (self);
}

static void
box_monitors_changed_cb (ShellLayoutManager *layout_manager,
                         ShellThumbnailsBox *self)
{
  box_destroy_thumbnails (self);

  if (self->window_manager != NULL &&
      shell_window_manager_get_overview_visible (self->window_manager))
    box_create_thumbnails (self);
}

static void
box_update_porthole (ShellThumbnailsBox *self)
{
  ShellMonitor *monitor = NULL;

  if (self->layout_manager != NULL)
    monitor = shell_layout_manager_get_monitor (self->layout_manager, self->monitor_index);

  if (monitor == NULL)
    {
      ClutterActor *stage = self->global != NULL ?
        CLUTTER_ACTOR (shell_global_get_stage (self->global)) : NULL;
      float x = 0, y = 0, width = 0, height = 0;

      if (stage != NULL)
        {
          clutter_actor_get_position (stage, &x, &y);
          clutter_actor_get_size (stage, &width, &height);
        }

      self->porthole_x = x;
      self->porthole_y = y;
      self->porthole_width = width;
      self->porthole_height = height;
    }
  else
    {
      MtkRectangle work_area;

      shell_layout_manager_get_work_area_for_monitor (self->layout_manager,
                                                      self->monitor_index,
                                                      &work_area);
      self->porthole_x = work_area.x;
      self->porthole_y = work_area.y;
      self->porthole_width = work_area.width;
      self->porthole_height = work_area.height;
    }

  g_clear_pointer (&monitor, shell_monitor_free);

  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

static void
box_workareas_changed_cb (MetaDisplay        *display,
                          ShellThumbnailsBox *self)
{
  box_update_porthole (self);
}

/* The drag */

static void
box_clear_drag_placeholder (ShellThumbnailsBox *self)
{
  if (self->drop_placeholder_pos == -1)
    return;

  self->drop_placeholder_pos = -1;
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

static void
box_clear_drop_placeholder_later (ShellThumbnailsBox *self)
{
  if (self->drop_placeholder_later == 0)
    return;

  if (self->laters != NULL)
    meta_laters_remove (self->laters, self->drop_placeholder_later);
  self->drop_placeholder_later = 0;
}

static ShellDragMotionResult
box_drag_motion_cb (ShellDragEvent *event,
                    gpointer        user_data)
{
  ShellThumbnailsBox *self = user_data;
  ClutterActor *target = shell_drag_event_get_target_actor (event);

  if (target == NULL || !clutter_actor_contains (CLUTTER_ACTOR (self), target))
    box_clear_drag_placeholder (self);

  return SHELL_DRAG_MOTION_RESULT_CONTINUE;
}

static void
box_remove_drag_monitor (ShellThumbnailsBox *self)
{
  if (self->drag_monitor != 0)
    shell_dnd_remove_drag_monitor (self->drag_monitor);

  self->drag_monitor = 0;
}

static void
box_end_drag (ShellThumbnailsBox *self)
{
  box_clear_drag_placeholder (self);
  box_remove_drag_monitor (self);
}

/**
 * shell_thumbnails_box_drag_begin:
 * @self: a #ShellThumbnailsBox
 *
 * Tells the box that the drag of an item (an application) or of a window
 * started, which is the overview's `item-drag-begin` and `window-drag-begin`
 * signals. The box follows the drag to show where a new workspace would be
 * created.
 */
void
shell_thumbnails_box_drag_begin (ShellThumbnailsBox *self)
{
  g_return_if_fail (SHELL_IS_THUMBNAILS_BOX (self));

  self->drag_cancelled = FALSE;

  box_remove_drag_monitor (self);
  self->drag_monitor = shell_dnd_add_drag_monitor (box_drag_motion_cb, self, NULL,
                                                   NULL, NULL, NULL);
}

/**
 * shell_thumbnails_box_drag_end:
 * @self: a #ShellThumbnailsBox
 *
 * Tells the box that the drag ended (`item-drag-end` and `window-drag-end` of
 * the overview).
 */
void
shell_thumbnails_box_drag_end (ShellThumbnailsBox *self)
{
  g_return_if_fail (SHELL_IS_THUMBNAILS_BOX (self));

  if (self->drag_cancelled)
    return;

  box_end_drag (self);
}

/**
 * shell_thumbnails_box_drag_cancelled:
 * @self: a #ShellThumbnailsBox
 *
 * Tells the box that the drag was cancelled (`item-drag-cancelled` and
 * `window-drag-cancelled` of the overview).
 */
void
shell_thumbnails_box_drag_cancelled (ShellThumbnailsBox *self)
{
  g_return_if_fail (SHELL_IS_THUMBNAILS_BOX (self));

  self->drag_cancelled = TRUE;
  box_end_drag (self);
}

static void
box_get_placeholder_target (ShellThumbnailsBox *self,
                            int                 index,
                            double              spacing,
                            gboolean            rtl,
                            double             *target_x1,
                            double             *target_x2)
{
  ClutterActor *workspace = g_ptr_array_index (self->thumbnails, index);
  double x1, x2;

  if (rtl)
    {
      double base_x = clutter_actor_get_x (workspace) + clutter_actor_get_width (workspace);

      x1 = base_x - WORKSPACE_CUT_SIZE;
      x2 = base_x + spacing + WORKSPACE_CUT_SIZE;
    }
  else
    {
      x1 = clutter_actor_get_x (workspace) - spacing - WORKSPACE_CUT_SIZE;
      x2 = clutter_actor_get_x (workspace) + WORKSPACE_CUT_SIZE;
    }

  if (index == 0)
    {
      if (rtl)
        x2 -= spacing + WORKSPACE_CUT_SIZE;
      else
        x1 += spacing + WORKSPACE_CUT_SIZE;
    }

  if (index == self->drop_placeholder_pos)
    {
      double placeholder_width = clutter_actor_get_width (self->drop_placeholder) + spacing;

      if (rtl)
        x2 += placeholder_width;
      else
        x1 -= placeholder_width;
    }

  *target_x1 = x1;
  *target_x2 = x2;
}

static gboolean
box_within_workspace (ShellThumbnailsBox *self,
                      double              x,
                      int                 index,
                      gboolean            rtl)
{
  int length = self->thumbnails->len;
  ClutterActor *workspace = g_ptr_array_index (self->thumbnails, index);
  double workspace_x1 = clutter_actor_get_x (workspace) + WORKSPACE_CUT_SIZE;
  double workspace_x2 = clutter_actor_get_x (workspace) +
                        clutter_actor_get_width (workspace) - WORKSPACE_CUT_SIZE;

  if (index == length - 1)
    {
      if (rtl)
        workspace_x1 -= WORKSPACE_CUT_SIZE;
      else
        workspace_x2 += WORKSPACE_CUT_SIZE;
    }

  return x > workspace_x1 && x <= workspace_x2;
}

/* Draggable target interface used only by the box */
static ShellDragMotionResult
thumbnail_handle_drag_over (ShellWorkspaceThumbnail *thumbnail,
                            const DropSource        *src,
                            uint32_t                 time)
{
  if (src->xdnd)
    {
      meta_workspace_activate (thumbnail->meta_workspace, time);
      return SHELL_DRAG_MOTION_RESULT_CONTINUE;
    }

  if (thumbnail->state > SHELL_THUMBNAIL_STATE_NORMAL)
    return SHELL_DRAG_MOTION_RESULT_CONTINUE;

  if (src->window != NULL && !thumbnail_is_my_window (thumbnail, src->window))
    return SHELL_DRAG_MOTION_RESULT_MOVE_DROP;
  if (drop_source_can_open_new_window (src))
    return SHELL_DRAG_MOTION_RESULT_COPY_DROP;

  return SHELL_DRAG_MOTION_RESULT_CONTINUE;
}

static gboolean
box_thumbnail_accept_drop (ShellThumbnailsBox      *self,
                           ShellWorkspaceThumbnail *thumbnail,
                           const DropSource        *src,
                           ClutterActor            *drag_actor)
{
  if (thumbnail->state > SHELL_THUMBNAIL_STATE_NORMAL)
    return FALSE;

  if (src->window != NULL)
    {
      if (thumbnail_is_my_window (thumbnail, src->window))
        return FALSE;

      if (self->display != NULL)
        move_window_to_monitor_and_workspace (self->display, src->window,
                                              thumbnail->monitor_index,
                                              meta_workspace_index (thumbnail->meta_workspace),
                                              FALSE);
      return TRUE;
    }
  else if (drop_source_can_open_new_window (src))
    {
      box_launch_app (src, drag_actor,
                      meta_workspace_index (thumbnail->meta_workspace));
      return TRUE;
    }

  return FALSE;
}

static ShellDragMotionResult
shell_thumbnails_box_handle_drag_over (ShellDragTarget *target,
                                       GObject         *source,
                                       ClutterActor    *drag_actor,
                                       float            x,
                                       float            y,
                                       uint32_t         time)
{
  ShellThumbnailsBox *self = SHELL_THUMBNAILS_BOX (target);
  g_auto (DropSource) src = { NULL, };
  ShellDragMotionResult result;
  StThemeNode *theme_node;
  gboolean rtl, can_create_workspaces;
  double spacing;
  int placeholder_pos = -1;
  int length, i;

  box_classify_source (source, &src);

  if (src.window == NULL && !drop_source_can_open_new_window (&src) && !src.xdnd)
    return SHELL_DRAG_MOTION_RESULT_CONTINUE;

  rtl = is_rtl ();
  can_create_workspaces = meta_prefs_get_dynamic_workspaces ();
  theme_node = st_widget_get_theme_node (ST_WIDGET (self));
  spacing = st_theme_node_get_length (theme_node, "spacing");

  self->drop_workspace = -1;
  length = self->thumbnails->len;
  for (i = 0; i < length; i++)
    {
      int index = rtl ? length - i - 1 : i;

      if (can_create_workspaces && !src.xdnd)
        {
          double target_start, target_end;

          box_get_placeholder_target (self, index, spacing, rtl,
                                      &target_start, &target_end);

          if (x > target_start && x <= target_end)
            {
              placeholder_pos = index;
              break;
            }
        }

      if (box_within_workspace (self, x, index, rtl))
        {
          self->drop_workspace = index;
          break;
        }
    }

  if (self->drop_placeholder_pos != placeholder_pos)
    {
      self->drop_placeholder_pos = placeholder_pos;
      clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
    }

  if (self->drop_workspace != -1)
    result = thumbnail_handle_drag_over (g_ptr_array_index (self->thumbnails,
                                                            self->drop_workspace),
                                         &src, time);
  else if (self->drop_placeholder_pos != -1)
    result = src.window != NULL ? SHELL_DRAG_MOTION_RESULT_MOVE_DROP
                                : SHELL_DRAG_MOTION_RESULT_COPY_DROP;
  else
    result = SHELL_DRAG_MOTION_RESULT_CONTINUE;

  return result;
}

static gboolean
shell_thumbnails_box_accept_drop (ShellDragTarget *target,
                                  GObject         *source,
                                  ClutterActor    *drag_actor,
                                  float            x,
                                  float            y,
                                  uint32_t         time)
{
  ShellThumbnailsBox *self = SHELL_THUMBNAILS_BOX (target);
  g_auto (DropSource) src = { NULL, };
  int new_workspace_index;
  gboolean is_window;
  ShellWorkspaceThumbnail *thumbnail;

  box_classify_source (source, &src);

  if (self->drop_workspace != -1)
    {
      if ((guint) self->drop_workspace >= self->thumbnails->len)
        return FALSE;

      return box_thumbnail_accept_drop (self,
                                        g_ptr_array_index (self->thumbnails, self->drop_workspace),
                                        &src, drag_actor);
    }

  if (self->drop_placeholder_pos == -1)
    return FALSE;

  if (src.window == NULL && !drop_source_can_open_new_window (&src))
    return FALSE;

  is_window = src.window != NULL;

  new_workspace_index = self->drop_placeholder_pos;
  self->drop_placeholder_pos = -1;
  self->splice_index = new_workspace_index;

  if (self->window_manager != NULL)
    shell_window_manager_insert_workspace (self->window_manager, new_workspace_index);

  thumbnail = (guint) new_workspace_index < self->thumbnails->len ?
    g_ptr_array_index (self->thumbnails, new_workspace_index) : NULL;

  if (is_window)
    {
      /* Move the window to our monitor first if necessary. */
      int thumb_monitor = thumbnail != NULL ?
        shell_workspace_thumbnail_get_monitor_index (thumbnail) : self->monitor_index;

      if (self->display != NULL)
        move_window_to_monitor_and_workspace (self->display, src.window,
                                              thumb_monitor, new_workspace_index, TRUE);
    }
  else if (drop_source_can_open_new_window (&src))
    {
      box_launch_app (&src, drag_actor, new_workspace_index);
    }

  if (src.app != NULL && self->window_manager != NULL && self->workspace_manager != NULL)
    {
      /* This new workspace will be automatically removed if the application fails
       * to open its first window within some time, as tracked by Shell.WindowTracker.
       * Here, we only add a very brief timeout to avoid the _immediate_ removal of the
       * workspace while we wait for the startup sequence to load. */
      MetaWorkspace *workspace =
        meta_workspace_manager_get_workspace_by_index (self->workspace_manager,
                                                       new_workspace_index);

      if (workspace != NULL)
        shell_window_manager_keep_workspace_alive (self->window_manager, workspace,
                                                   WORKSPACE_KEEP_ALIVE_TIME);
    }

  /* Start the animation on the workspace (which is actually
   * an old one which just became empty) */
  if (thumbnail != NULL)
    {
      box_set_thumbnail_state (self, thumbnail, SHELL_THUMBNAIL_STATE_NEW);
      shell_workspace_thumbnail_set_slide_position (thumbnail, 1);
      shell_workspace_thumbnail_set_collapse_fraction (thumbnail, 1);
    }

  box_queue_update_states (self);

  return TRUE;
}

static void
shell_thumbnails_box_drag_target_init (ShellDragTargetInterface *iface)
{
  iface->handle_drag_over = shell_thumbnails_box_handle_drag_over;
  iface->accept_drop = shell_thumbnails_box_accept_drop;
}

/* The size */

static void
shell_thumbnails_box_get_preferred_height (ClutterActor *actor,
                                           float         for_width,
                                           float        *min_height_p,
                                           float        *natural_height_p)
{
  ShellThumbnailsBox *self = SHELL_THUMBNAILS_BOX (actor);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  double spacing, total_spacing, avail, scale, n_workspaces;
  float min_height, natural_height;

  st_theme_node_adjust_for_width (theme_node, &for_width);

  spacing = st_theme_node_get_length (theme_node, "spacing");
  n_workspaces = self->thumbnails->len;
  total_spacing = (n_workspaces - 1) * spacing;

  avail = for_width - total_spacing;

  scale = (avail / n_workspaces) / self->porthole_width;
  scale = MIN (scale, self->max_thumbnail_scale);

  min_height = natural_height = js_round (self->porthole_height * scale);
  st_theme_node_adjust_preferred_height (theme_node, &min_height, &natural_height);

  if (min_height_p != NULL)
    *min_height_p = min_height;
  if (natural_height_p != NULL)
    *natural_height_p = natural_height;
}

static void
shell_thumbnails_box_get_preferred_width (ClutterActor *actor,
                                          float         for_height,
                                          float        *min_width_p,
                                          float        *natural_width_p)
{
  ShellThumbnailsBox *self = SHELL_THUMBNAILS_BOX (actor);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  double spacing, total_spacing, natural_width = 0;
  float min_width, natural_width_f;
  guint i, n_workspaces;

  /* Note that for getPreferredHeight/Width we cheat a bit and skip propagating
   * the size request to our children because we know how big they are and know
   * that the actors aren't depending on the virtual functions being called. */
  spacing = st_theme_node_get_length (theme_node, "spacing");
  n_workspaces = self->thumbnails->len;
  total_spacing = ((double) n_workspaces - 1) * spacing;

  for (i = 0; i < n_workspaces; i++)
    {
      ShellWorkspaceThumbnail *thumbnail = g_ptr_array_index (self->thumbnails, i);
      double workspace_spacing = 0;
      double progress, width;

      if (i > 0)
        workspace_spacing += spacing / 2;
      if (i < n_workspaces - 1)
        workspace_spacing += spacing / 2;

      progress = 1 - shell_workspace_thumbnail_get_collapse_fraction (thumbnail);
      width = (self->porthole_width * self->max_thumbnail_scale + workspace_spacing) * progress;
      natural_width += width;
    }

  min_width = total_spacing;
  natural_width_f = natural_width;
  st_theme_node_adjust_preferred_width (theme_node, &min_width, &natural_width_f);

  if (min_width_p != NULL)
    *min_width_p = min_width;
  if (natural_width_p != NULL)
    *natural_width_p = natural_width_f;
}

static gboolean
box_hide_placeholder_later_cb (gpointer user_data)
{
  ShellThumbnailsBox *self = user_data;

  self->drop_placeholder_later = 0;
  if (self->drop_placeholder != NULL)
    clutter_actor_hide (self->drop_placeholder);

  return G_SOURCE_REMOVE;
}

static gboolean
box_show_placeholder_later_cb (gpointer user_data)
{
  ShellThumbnailsBox *self = user_data;

  self->drop_placeholder_later = 0;
  if (self->drop_placeholder != NULL)
    clutter_actor_show (self->drop_placeholder);

  return G_SOURCE_REMOVE;
}

static void
shell_thumbnails_box_allocate (ClutterActor          *actor,
                               const ClutterActorBox *box)
{
  ShellThumbnailsBox *self = SHELL_THUMBNAILS_BOX (actor);
  StThemeNode *theme_node, *indicator_theme_node;
  ClutterActorBox content_box, child_box;
  double porthole_width, porthole_height, spacing;
  double ratio, thumbnail_full_height, thumbnail_width, thumbnail_height;
  double rounded_vscale, extra_width;
  double indicator_value;
  int indicator_upper_ws, indicator_lower_ws;
  double indicator_lower_x1 = 0, indicator_lower_x2 = 0;
  double indicator_upper_x1 = 0, indicator_upper_x2 = 0;
  double indicator_top_full_border, indicator_bottom_full_border;
  double indicator_left_full_border, indicator_right_full_border;
  double indicator_x1, indicator_x2;
  double x;
  gboolean rtl;
  int n_workspaces;
  guint i;

  clutter_actor_set_allocation (actor, box);

  rtl = is_rtl ();

  if (self->thumbnails->len == 0) /* not visible */
    return;

  theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  st_theme_node_get_content_box (theme_node, box, &content_box);

  porthole_width = self->porthole_width;
  porthole_height = self->porthole_height;
  spacing = st_theme_node_get_length (theme_node, "spacing");

  n_workspaces = self->thumbnails->len;

  /* Compute the scale we'll need once everything is updated,
   * unless we are currently transitioning */
  if (self->expand_fraction == 0 || self->expand_fraction == 1)
    {
      float nat_width, nat_height;
      double total_spacing, available_width, available_height;
      double hscale, vscale, new_scale;

      clutter_actor_get_preferred_width (actor, -1, NULL, &nat_width);
      clutter_actor_get_preferred_height (actor, nat_width, NULL, &nat_height);
      total_spacing = (n_workspaces - 1) * spacing;
      available_width = (clutter_actor_box_get_width (&content_box) - total_spacing) / n_workspaces;
      available_height =
        MIN ((double) nat_height, porthole_height * self->max_thumbnail_scale) -
        st_theme_node_get_vertical_padding (theme_node) -
        st_theme_node_get_border_width (theme_node, ST_SIDE_TOP) -
        st_theme_node_get_border_width (theme_node, ST_SIDE_BOTTOM);

      hscale = available_width / porthole_width;
      vscale = available_height / porthole_height;
      new_scale = MIN (hscale, vscale);

      if (new_scale != self->target_scale)
        {
          if (self->target_scale > 0)
            {
              /* We don't ease immediately because we need to observe the
               * ordering in queueUpdateStates - if workspaces have been
               * removed we need to slide them out as the first thing. */
              self->target_scale = new_scale;
              self->pending_scale_update = TRUE;
            }
          else
            {
              self->target_scale = self->scale = new_scale;
            }

          box_queue_update_states (self);
        }
    }

  ratio = porthole_width / porthole_height;
  thumbnail_full_height = js_round (porthole_height * self->scale);
  thumbnail_width = js_round (thumbnail_full_height * ratio);
  thumbnail_height = thumbnail_full_height * self->expand_fraction;
  rounded_vscale = thumbnail_height / porthole_height;

  /* We always request size for maxThumbnailScale, distribute
   * space evently if we use smaller thumbnails */
  extra_width = (self->max_thumbnail_scale * porthole_width - thumbnail_width) * n_workspaces;
  content_box.x1 += js_round (extra_width / 2);
  content_box.x2 -= js_round (extra_width / 2);

  indicator_value = self->scroll_adjustment != NULL ?
    st_adjustment_get_value (self->scroll_adjustment) : 0;
  indicator_upper_ws = ceil (indicator_value);
  indicator_lower_ws = floor (indicator_value);

  indicator_theme_node = st_widget_get_theme_node (ST_WIDGET (self->indicator));
  indicator_top_full_border =
    st_theme_node_get_padding (indicator_theme_node, ST_SIDE_TOP) +
    st_theme_node_get_border_width (indicator_theme_node, ST_SIDE_TOP);
  indicator_bottom_full_border =
    st_theme_node_get_padding (indicator_theme_node, ST_SIDE_BOTTOM) +
    st_theme_node_get_border_width (indicator_theme_node, ST_SIDE_BOTTOM);
  indicator_left_full_border =
    st_theme_node_get_padding (indicator_theme_node, ST_SIDE_LEFT) +
    st_theme_node_get_border_width (indicator_theme_node, ST_SIDE_LEFT);
  indicator_right_full_border =
    st_theme_node_get_padding (indicator_theme_node, ST_SIDE_RIGHT) +
    st_theme_node_get_border_width (indicator_theme_node, ST_SIDE_RIGHT);

  x = rtl ? 0 : content_box.x1;

  if (self->drop_placeholder_pos == -1)
    {
      float placeholder_x, placeholder_y;

      clutter_actor_get_position (self->drop_placeholder, &placeholder_x, &placeholder_y);
      clutter_actor_allocate_preferred_size (self->drop_placeholder,
                                             placeholder_x, placeholder_y);

      box_clear_drop_placeholder_later (self);
      if (clutter_actor_is_visible (self->drop_placeholder) && self->laters != NULL)
        self->drop_placeholder_later =
          meta_laters_add (self->laters, META_LATER_BEFORE_REDRAW,
                           box_hide_placeholder_later_cb, self, NULL);
    }

  for (i = 0; i < self->thumbnails->len; i++)
    {
      ShellWorkspaceThumbnail *thumbnail = g_ptr_array_index (self->thumbnails, i);
      double collapse_fraction = shell_workspace_thumbnail_get_collapse_fraction (thumbnail);
      double y1, y2;
      double x1, x2, rounded_hscale;

      if (i > 0)
        x += spacing - js_round (collapse_fraction * spacing);

      y1 = content_box.y1;
      y2 = y1 + thumbnail_height;

      if ((int) i == self->drop_placeholder_pos)
        {
          float placeholder_width;

          clutter_actor_get_preferred_width (self->drop_placeholder, -1, NULL,
                                             &placeholder_width);
          child_box.y1 = y1;
          child_box.y2 = y2;

          if (rtl)
            {
              child_box.x2 = content_box.x2 - js_round (x);
              child_box.x1 = content_box.x2 - js_round (x + placeholder_width);
            }
          else
            {
              child_box.x1 = js_round (x);
              child_box.x2 = js_round (x + placeholder_width);
            }

          clutter_actor_allocate (self->drop_placeholder, &child_box);

          box_clear_drop_placeholder_later (self);
          if (!clutter_actor_is_visible (self->drop_placeholder) && self->laters != NULL)
            self->drop_placeholder_later =
              meta_laters_add (self->laters, META_LATER_BEFORE_REDRAW,
                               box_show_placeholder_later_cb, self, NULL);

          x += placeholder_width + spacing;
        }

      /* We might end up with thumbnailWidth being something like 99.33
       * pixels. To make this work and not end up with a gap at the end,
       * we need some thumbnails to be 99 pixels and some 100 pixels width;
       * we compute an actual scale separately for each thumbnail. */
      x1 = js_round (x);
      x2 = js_round (x + thumbnail_width);
      rounded_hscale = (x2 - x1) / porthole_width;

      /* Allocating a scaled actor is funny - x1/y1 correspond to the origin
       * of the actor, but x2/y2 are increased by the *unscaled* size. */
      if (rtl)
        {
          child_box.x2 = content_box.x2 - x1;
          child_box.x1 = content_box.x2 - (x1 + thumbnail_width);
        }
      else
        {
          child_box.x1 = x1;
          child_box.x2 = x1 + thumbnail_width;
        }
      child_box.y1 = y1;
      child_box.y2 = y1 + thumbnail_height;

      shell_workspace_thumbnail_set_scale (thumbnail, rounded_hscale, rounded_vscale);
      clutter_actor_allocate (CLUTTER_ACTOR (thumbnail), &child_box);

      if ((int) i == indicator_upper_ws)
        {
          indicator_upper_x1 = child_box.x1;
          indicator_upper_x2 = child_box.x2;
        }
      if ((int) i == indicator_lower_ws)
        {
          indicator_lower_x1 = child_box.x1;
          indicator_lower_x2 = child_box.x2;
        }

      /* We round the collapsing portion so that we don't get thumbnails resizing
       * during an animation due to differences in rounded, but leave the uncollapsed
       * portion unrounded so that non-animating we end up with the right total */
      x += thumbnail_width - js_round (thumbnail_width * collapse_fraction);
    }

  child_box.y1 = content_box.y1;
  child_box.y2 = content_box.y1 + thumbnail_height;

  indicator_x1 = indicator_lower_x1 +
    (indicator_upper_x1 - indicator_lower_x1) * fmod (indicator_value, 1);
  indicator_x2 = indicator_lower_x2 +
    (indicator_upper_x2 - indicator_lower_x2) * fmod (indicator_value, 1);

  child_box.x1 = indicator_x1 - indicator_left_full_border;
  child_box.x2 = indicator_x2 + indicator_right_full_border;
  child_box.y1 -= indicator_top_full_border;
  child_box.y2 += indicator_bottom_full_border;
  clutter_actor_allocate (self->indicator, &child_box);
}

/* The widget */

static void
box_settings_dynamic_workspaces_changed_cb (GSettings          *settings,
                                            const char         *key,
                                            ShellThumbnailsBox *self)
{
  box_update_should_show (self);
}

static void
box_scroll_adjustment_value_changed_cb (StAdjustment       *adjustment,
                                        GParamSpec         *pspec,
                                        ShellThumbnailsBox *self)
{
  box_update_indicator (self);
}

static void
box_notify_visible_cb (ShellThumbnailsBox *self,
                       GParamSpec         *pspec)
{
  if (!clutter_actor_is_visible (CLUTTER_ACTOR (self)))
    box_queue_update_states (self);
}

static void
box_click_recognize_cb (ClutterGesture     *gesture,
                        ShellThumbnailsBox *self)
{
  graphene_point_t coords;
  guint32 time = clutter_get_current_event_time ();
  guint i;

  clutter_press_gesture_get_coords (CLUTTER_PRESS_GESTURE (gesture), &coords);

  for (i = 0; i < self->thumbnails->len; i++)
    {
      ClutterActor *thumbnail = g_ptr_array_index (self->thumbnails, i);
      float x = clutter_actor_get_x (thumbnail);

      if (coords.x >= x && coords.x <= x + clutter_actor_get_width (thumbnail))
        {
          shell_workspace_thumbnail_activate (SHELL_WORKSPACE_THUMBNAIL (thumbnail), time);
          break;
        }
    }
}

static void
shell_thumbnails_box_destroy (ClutterActor *actor)
{
  ShellThumbnailsBox *self = SHELL_THUMBNAILS_BOX (actor);

  box_destroy_thumbnails (self);
  box_unqueue_update_states (self);
  box_clear_drop_placeholder_later (self);
  box_remove_drag_monitor (self);

  g_clear_object (&self->settings);

  CLUTTER_ACTOR_CLASS (shell_thumbnails_box_parent_class)->destroy (actor);
}

static void
shell_thumbnails_box_constructed (GObject *object)
{
  ShellThumbnailsBox *self = SHELL_THUMBNAILS_BOX (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellGlobal *global = shell_global_get ();
  ClutterAction *click_gesture;

  G_OBJECT_CLASS (shell_thumbnails_box_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (self), "workspace-thumbnails");
  clutter_actor_set_reactive (actor, TRUE);
  clutter_actor_set_x_align (actor, CLUTTER_ACTOR_ALIGN_CENTER);
  clutter_actor_set_pivot_point (actor, 0.5f, 0.5f);

  self->indicator = g_object_new (ST_TYPE_BIN,
                                  "style-class", "workspace-thumbnail-indicator",
                                  "visible", FALSE,
                                  NULL);
  /* We don't want the indicator to affect drag-and-drop */
  shell_util_set_hidden_from_pick (self->indicator, TRUE);
  clutter_actor_add_child (actor, self->indicator);
  g_object_add_weak_pointer (G_OBJECT (self->indicator), (gpointer *) &self->indicator);

  self->drop_placeholder = g_object_new (ST_TYPE_BIN,
                                         "style-class", "placeholder",
                                         "visible", FALSE,
                                         NULL);
  clutter_actor_add_child (actor, self->drop_placeholder);
  g_object_add_weak_pointer (G_OBJECT (self->drop_placeholder),
                             (gpointer *) &self->drop_placeholder);

  g_set_weak_pointer (&self->global, global);
  g_set_weak_pointer (&self->display, shell_global_get_display (global));
  g_set_weak_pointer (&self->workspace_manager, shell_global_get_workspace_manager (global));
  g_set_weak_pointer (&self->laters, get_laters ());
  g_set_weak_pointer (&self->layout_manager,
                      shell_main_get_layout_manager (shell_main_get_default ()));

  click_gesture = CLUTTER_ACTION (clutter_click_gesture_new ());
  g_signal_connect_object (click_gesture, "recognize",
                           G_CALLBACK (box_click_recognize_cb), self, 0);
  clutter_actor_add_action (actor, click_gesture);

  /* The overview's `showing` and `hidden` signals */
  if (self->window_manager != NULL)
    g_signal_connect_object (self->window_manager, "notify::overview-visible",
                             G_CALLBACK (box_overview_visible_changed_cb), self, 0);

  self->settings = g_settings_new (MUTTER_SCHEMA);
  g_signal_connect_object (self->settings, "changed::dynamic-workspaces",
                           G_CALLBACK (box_settings_dynamic_workspaces_changed_cb),
                           self, 0);
  box_update_should_show (self);

  if (self->layout_manager != NULL)
    g_signal_connect_object (self->layout_manager, "monitors-changed",
                             G_CALLBACK (box_monitors_changed_cb), self, 0);

  /* The porthole is the part of the screen we're showing in the thumbnails */
  if (self->display != NULL)
    g_signal_connect_object (self->display, "workareas-changed",
                             G_CALLBACK (box_workareas_changed_cb), self, 0);
  box_update_porthole (self);

  g_signal_connect (self, "notify::visible", G_CALLBACK (box_notify_visible_cb), NULL);

  if (self->scroll_adjustment != NULL)
    g_signal_connect_object (self->scroll_adjustment, "notify::value",
                             G_CALLBACK (box_scroll_adjustment_value_changed_cb),
                             self, 0);
}

static void
shell_thumbnails_box_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  ShellThumbnailsBox *self = SHELL_THUMBNAILS_BOX (object);

  switch (prop_id)
    {
    case BOX_PROP_SCROLL_ADJUSTMENT:
      g_set_object (&self->scroll_adjustment, g_value_get_object (value));
      break;
    case BOX_PROP_MONITOR_INDEX:
      shell_thumbnails_box_set_monitor_index (self, g_value_get_int (value));
      break;
    case BOX_PROP_WINDOW_MANAGER:
      g_set_weak_pointer (&self->window_manager, g_value_get_object (value));
      break;
    case BOX_PROP_EXPAND_FRACTION:
      shell_thumbnails_box_set_expand_fraction (self, g_value_get_double (value));
      break;
    case BOX_PROP_SCALE:
      shell_thumbnails_box_set_scale (self, g_value_get_double (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_thumbnails_box_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  ShellThumbnailsBox *self = SHELL_THUMBNAILS_BOX (object);

  switch (prop_id)
    {
    case BOX_PROP_SCROLL_ADJUSTMENT:
      g_value_set_object (value, self->scroll_adjustment);
      break;
    case BOX_PROP_MONITOR_INDEX:
      g_value_set_int (value, self->monitor_index);
      break;
    case BOX_PROP_WINDOW_MANAGER:
      g_value_set_object (value, self->window_manager);
      break;
    case BOX_PROP_EXPAND_FRACTION:
      g_value_set_double (value, self->expand_fraction);
      break;
    case BOX_PROP_SCALE:
      g_value_set_double (value, self->scale);
      break;
    case BOX_PROP_SHOULD_SHOW:
      g_value_set_boolean (value, self->should_show);
      break;
    case BOX_PROP_MAX_THUMBNAIL_SCALE:
      g_value_set_double (value, self->max_thumbnail_scale);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_thumbnails_box_dispose (GObject *object)
{
  ShellThumbnailsBox *self = SHELL_THUMBNAILS_BOX (object);

  /* Destroys the actor, which destroys the thumbnails */
  G_OBJECT_CLASS (shell_thumbnails_box_parent_class)->dispose (object);

  g_clear_weak_pointer (&self->indicator);
  g_clear_weak_pointer (&self->drop_placeholder);
  g_clear_weak_pointer (&self->window_manager);
  g_clear_weak_pointer (&self->global);
  g_clear_weak_pointer (&self->display);
  g_clear_weak_pointer (&self->workspace_manager);
  g_clear_weak_pointer (&self->laters);
  g_clear_weak_pointer (&self->layout_manager);
  g_clear_object (&self->settings);
  g_clear_object (&self->scroll_adjustment);
}

static void
shell_thumbnails_box_finalize (GObject *object)
{
  ShellThumbnailsBox *self = SHELL_THUMBNAILS_BOX (object);

  g_clear_pointer (&self->thumbnails, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_thumbnails_box_parent_class)->finalize (object);
}

static void
shell_thumbnails_box_class_init (ShellThumbnailsBoxClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_thumbnails_box_constructed;
  object_class->set_property = shell_thumbnails_box_set_property;
  object_class->get_property = shell_thumbnails_box_get_property;
  object_class->dispose = shell_thumbnails_box_dispose;
  object_class->finalize = shell_thumbnails_box_finalize;

  actor_class->destroy = shell_thumbnails_box_destroy;
  actor_class->get_preferred_width = shell_thumbnails_box_get_preferred_width;
  actor_class->get_preferred_height = shell_thumbnails_box_get_preferred_height;
  actor_class->allocate = shell_thumbnails_box_allocate;

  /**
   * ShellThumbnailsBox:scroll-adjustment:
   *
   * The adjustment of the workspaces, whose value is the position of the
   * indicator in the strip (`scrollAdjustment`).
   */
  box_props[BOX_PROP_SCROLL_ADJUSTMENT] =
    g_param_spec_object ("scroll-adjustment", NULL, NULL,
                         ST_TYPE_ADJUSTMENT,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellThumbnailsBox:monitor-index:
   *
   * The monitor whose windows the thumbnails show (`monitorIndex`). Changing
   * it does not change the thumbnails that exist; they are made again when
   * the overview is shown.
   */
  box_props[BOX_PROP_MONITOR_INDEX] =
    g_param_spec_int ("monitor-index", NULL, NULL,
                      G_MININT, G_MAXINT, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_CONSTRUCT |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellThumbnailsBox:window-manager:
   *
   * The window manager that the box follows the overview through: the
   * thumbnails are made while #ShellWindowManager:overview-visible is %TRUE.
   * It also creates the workspaces that are dropped on the strip and
   * emits #ShellWindowManager::overview-hide-requested when the thumbnail
   * of the active workspace is clicked (`Main.wm` and `Main.overview`).
   */
  box_props[BOX_PROP_WINDOW_MANAGER] =
    g_param_spec_object ("window-manager", NULL, NULL,
                         SHELL_TYPE_WINDOW_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellThumbnailsBox:expand-fraction:
   *
   * How much of its height the strip takes, from 0 (collapsed) to 1.
   */
  box_props[BOX_PROP_EXPAND_FRACTION] =
    g_param_spec_double ("expand-fraction", NULL, NULL,
                         0, 1, 1,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellThumbnailsBox:scale:
   *
   * The scale of the windows in the thumbnails.
   */
  box_props[BOX_PROP_SCALE] =
    g_param_spec_double ("scale", NULL, NULL,
                         0, G_MAXDOUBLE, 0,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellThumbnailsBox:should-show:
   *
   * Whether the strip is worth showing: there is more than one workspace,
   * more than two with dynamic workspaces (`shouldShow`).
   */
  box_props[BOX_PROP_SHOULD_SHOW] =
    g_param_spec_boolean ("should-show", NULL, NULL,
                          TRUE,
                          G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellThumbnailsBox:max-thumbnail-scale:
   *
   * The largest scale of a thumbnail, %SHELL_MAX_THUMBNAIL_SCALE
   * (`maxThumbnailScale`).
   */
  box_props[BOX_PROP_MAX_THUMBNAIL_SCALE] =
    g_param_spec_double ("max-thumbnail-scale", NULL, NULL,
                         0, 1, SHELL_MAX_THUMBNAIL_SCALE,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, BOX_N_PROPS, box_props);

  /**
   * ShellThumbnailsBox::window-drag-begin:
   * @self: the #ShellThumbnailsBox
   * @window: the window whose clone starts being dragged
   *
   * Emitted when the clone of a window in a thumbnail starts being dragged.
   * overviewControls.js connects it to `Main.overview.beginWindowDrag ()`.
   * The handler must not return a value.
   */
  box_signals[BOX_WINDOW_DRAG_BEGIN] =
    g_signal_new ("window-drag-begin",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  META_TYPE_WINDOW);

  /**
   * ShellThumbnailsBox::window-drag-cancelled:
   * @self: the #ShellThumbnailsBox
   * @window: the window whose clone was dragged
   *
   * Emitted when the drag of the clone of a window is cancelled
   * (`Main.overview.cancelledWindowDrag ()`).
   */
  box_signals[BOX_WINDOW_DRAG_CANCELLED] =
    g_signal_new ("window-drag-cancelled",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  META_TYPE_WINDOW);

  /**
   * ShellThumbnailsBox::window-drag-end:
   * @self: the #ShellThumbnailsBox
   * @window: the window whose clone was dragged
   *
   * Emitted when the drag of the clone of a window ends
   * (`Main.overview.endWindowDrag ()`).
   */
  box_signals[BOX_WINDOW_DRAG_END] =
    g_signal_new ("window-drag-end",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  META_TYPE_WINDOW);
}

static void
shell_thumbnails_box_init (ShellThumbnailsBox *self)
{
  self->drop_workspace = -1;
  self->drop_placeholder_pos = -1;
  self->splice_index = -1;
  self->max_thumbnail_scale = SHELL_MAX_THUMBNAIL_SCALE;
  self->expand_fraction = 1;
  self->should_show = TRUE;
  self->thumbnails = g_ptr_array_new_with_free_func (g_object_unref);
}

/**
 * shell_thumbnails_box_new:
 * @scroll_adjustment: the adjustment of the workspaces, whose value is where
 *   the indicator is
 * @monitor_index: the monitor whose windows the thumbnails show
 * @window_manager: the window manager
 *
 * Creates a strip of thumbnails.
 *
 * Returns: (transfer none): a new #ShellThumbnailsBox, which is floating
 */
ShellThumbnailsBox *
shell_thumbnails_box_new (StAdjustment       *scroll_adjustment,
                          int                 monitor_index,
                          ShellWindowManager *window_manager)
{
  return g_object_new (SHELL_TYPE_THUMBNAILS_BOX,
                       "scroll-adjustment", scroll_adjustment,
                       "monitor-index", monitor_index,
                       "window-manager", window_manager,
                       NULL);
}

/**
 * shell_thumbnails_box_get_monitor_index:
 * @self: a #ShellThumbnailsBox
 *
 * Returns: the monitor whose windows the thumbnails show
 */
int
shell_thumbnails_box_get_monitor_index (ShellThumbnailsBox *self)
{
  g_return_val_if_fail (SHELL_IS_THUMBNAILS_BOX (self), 0);

  return self->monitor_index;
}

/**
 * shell_thumbnails_box_set_monitor_index:
 * @self: a #ShellThumbnailsBox
 * @monitor_index: the monitor whose windows the thumbnails show
 *
 * Sets the monitor of the thumbnails that are made from now on
 * (`setMonitorIndex ()`).
 */
void
shell_thumbnails_box_set_monitor_index (ShellThumbnailsBox *self,
                                        int                 monitor_index)
{
  g_return_if_fail (SHELL_IS_THUMBNAILS_BOX (self));

  if (self->monitor_index == monitor_index)
    return;

  self->monitor_index = monitor_index;
  g_object_notify_by_pspec (G_OBJECT (self), box_props[BOX_PROP_MONITOR_INDEX]);
}

/**
 * shell_thumbnails_box_get_expand_fraction:
 * @self: a #ShellThumbnailsBox
 *
 * Returns: how much of its height the strip takes, from 0 to 1
 */
double
shell_thumbnails_box_get_expand_fraction (ShellThumbnailsBox *self)
{
  g_return_val_if_fail (SHELL_IS_THUMBNAILS_BOX (self), 0);

  return self->expand_fraction;
}

/**
 * shell_thumbnails_box_set_expand_fraction:
 * @self: a #ShellThumbnailsBox
 * @expand_fraction: how much of its height the strip takes, from 0 to 1
 *
 * Sets how much of its height the strip takes.
 */
void
shell_thumbnails_box_set_expand_fraction (ShellThumbnailsBox *self,
                                          double              expand_fraction)
{
  g_return_if_fail (SHELL_IS_THUMBNAILS_BOX (self));

  if (self->expand_fraction == expand_fraction)
    return;

  self->expand_fraction = expand_fraction;
  g_object_notify_by_pspec (G_OBJECT (self), box_props[BOX_PROP_EXPAND_FRACTION]);
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

/**
 * shell_thumbnails_box_get_scale:
 * @self: a #ShellThumbnailsBox
 *
 * Returns: the scale of the windows in the thumbnails
 */
double
shell_thumbnails_box_get_scale (ShellThumbnailsBox *self)
{
  g_return_val_if_fail (SHELL_IS_THUMBNAILS_BOX (self), 0);

  return self->scale;
}

/**
 * shell_thumbnails_box_set_scale:
 * @self: a #ShellThumbnailsBox
 * @scale: the scale of the windows in the thumbnails
 *
 * Sets the scale of the windows in the thumbnails.
 */
void
shell_thumbnails_box_set_scale (ShellThumbnailsBox *self,
                                double              scale)
{
  g_return_if_fail (SHELL_IS_THUMBNAILS_BOX (self));

  if (self->scale == scale)
    return;

  self->scale = scale;
  g_object_notify_by_pspec (G_OBJECT (self), box_props[BOX_PROP_SCALE]);
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

/**
 * shell_thumbnails_box_get_should_show:
 * @self: a #ShellThumbnailsBox
 *
 * Returns: whether the strip is worth showing
 */
gboolean
shell_thumbnails_box_get_should_show (ShellThumbnailsBox *self)
{
  g_return_val_if_fail (SHELL_IS_THUMBNAILS_BOX (self), FALSE);

  return self->should_show;
}

/**
 * shell_thumbnails_box_get_max_thumbnail_scale:
 * @self: a #ShellThumbnailsBox
 *
 * Returns: the largest scale of a thumbnail
 */
double
shell_thumbnails_box_get_max_thumbnail_scale (ShellThumbnailsBox *self)
{
  g_return_val_if_fail (SHELL_IS_THUMBNAILS_BOX (self), 0);

  return self->max_thumbnail_scale;
}
