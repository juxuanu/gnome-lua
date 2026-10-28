/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * xdnd-handler.c: Tracks drags coming from X11 clients (XDND) and presents
 * them to the shell as a drag source. Port of js/ui/xdndHandler.js.
 *
 * The handler itself is the `source` of the drag, so consumers compare the
 * source of a drag with the handler (`source === Main.xdndHandler`). Looking
 * up the JS delegates of the actors under the pointer is left to the
 * consumers of #ShellXdndHandler::drag-motion.
 */

#include "config.h"

#include <meta/compositor.h>
#include <meta/meta-backend.h>
#include <meta/meta-dnd.h>
#include <meta/meta-window-actor.h>
#include <meta/window.h>

#include "shell-global.h"
#include "ui/xdnd-handler.h"

enum
{
  PROP_0,

  PROP_UI_GROUP,
  PROP_SESSION_LOCKED,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  DRAG_BEGIN,
  DRAG_END,
  DRAG_MOTION,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellXdndHandler
{
  GObject parent;

  ClutterActor *ui_group;            /* weak */

  /* Used to display a clone of the cursor window when the window group is
   * hidden (like it happens in the overview) */
  ClutterActor *cursor_window_clone; /* weak, owned by ui_group */

  /* Used as a drag actor in case we don't have a cursor window clone */
  ClutterActor *dummy;               /* weak, owned by ui_group */

  ClutterActor *window_group;        /* weak, only set during a drag */

  gboolean session_locked;
  gboolean drag_active;
};

G_DEFINE_FINAL_TYPE (ShellXdndHandler, shell_xdnd_handler, G_TYPE_OBJECT)

static void
destroy_cursor_window_clone (ShellXdndHandler *self)
{
  ClutterActor *clone = self->cursor_window_clone;

  if (clone == NULL)
    return;

  clutter_actor_destroy (clone);
  g_clear_weak_pointer (&self->cursor_window_clone);
}

static void
sync_cursor_window_clone (ShellXdndHandler *self)
{
  ShellGlobal *global = shell_global_get ();
  gboolean window_group_visible = self->window_group != NULL &&
    clutter_actor_is_visible (self->window_group);
  gboolean needs_cursor_clone = !window_group_visible && !self->session_locked;

  if (needs_cursor_clone)
    {
      g_autoptr (GList) windows = NULL;
      ClutterActor *cursor_window;
      ClutterConstraint *constraint;
      ClutterActor *clone;

      if (self->cursor_window_clone != NULL)
        return;

      windows = shell_global_get_window_actors (global);
      if (windows == NULL)
        return;

      cursor_window = g_list_last (windows)->data;

      /* FIXME: more reliable way? */
      if (!meta_window_is_override_redirect (meta_window_actor_get_meta_window (META_WINDOW_ACTOR (cursor_window))))
        return;

      constraint = clutter_bind_constraint_new (cursor_window,
                                                CLUTTER_BIND_POSITION, 0);

      clone = clutter_clone_new (cursor_window);
      g_set_weak_pointer (&self->cursor_window_clone, clone);
      clutter_actor_add_child (self->ui_group, clone);

      /* Make sure that the clone has the same position as the source */
      clutter_actor_add_constraint (clone, constraint);
    }
  else
    {
      destroy_cursor_window_clone (self);
    }
}

static void
window_group_visible_changed_cb (ClutterActor     *window_group,
                                 GParamSpec       *pspec,
                                 ShellXdndHandler *self)
{
  sync_cursor_window_clone (self);
}

/* Called when the user cancels the drag (i.e release the button) */
static void
on_dnd_leave (MetaDnd          *dnd,
              ShellXdndHandler *self)
{
  ShellGlobal *global = shell_global_get ();

  if (self->window_group != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->window_group,
                                            window_group_visible_changed_cb,
                                            self);
      g_clear_weak_pointer (&self->window_group);
    }

  self->drag_active = FALSE;

  destroy_cursor_window_clone (self);

  clutter_actor_show (meta_compositor_get_feedback_group (shell_global_get_compositor (global)));

  g_signal_emit (self, signals[DRAG_END], 0);
}

static void
on_dnd_enter (MetaDnd          *dnd,
              ShellXdndHandler *self)
{
  ShellGlobal *global = shell_global_get ();

  if (self->window_group != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->window_group,
                                            window_group_visible_changed_cb,
                                            self);
      g_clear_weak_pointer (&self->window_group);
    }

  g_set_weak_pointer (&self->window_group, shell_global_get_window_group (global));
  g_signal_connect_object (self->window_group, "notify::visible",
                           G_CALLBACK (window_group_visible_changed_cb),
                           self, 0);

  self->drag_active = TRUE;

  g_signal_emit (self, signals[DRAG_BEGIN], 0, shell_global_get_current_time (global));
}

static void
on_dnd_position_change (MetaDnd          *dnd,
                        int               x,
                        int               y,
                        ShellXdndHandler *self)
{
  ShellGlobal *global = shell_global_get ();
  ClutterStage *stage = shell_global_get_stage (global);
  g_autoptr (ClutterActor) picked_actor = NULL;
  g_autoptr (ClutterActor) drag_actor = NULL;

  picked_actor = clutter_stage_get_actor_at_pos (stage, CLUTTER_PICK_REACTIVE, x, y);
  if (picked_actor != NULL)
    g_object_ref (picked_actor);

  /* Make sure that the cursor window is on top */
  if (self->cursor_window_clone != NULL)
    {
      clutter_actor_set_child_above_sibling (self->ui_group,
                                             self->cursor_window_clone, NULL);
    }

  drag_actor = g_object_ref (self->cursor_window_clone != NULL
                             ? self->cursor_window_clone
                             : self->dummy);

  g_signal_emit (self, signals[DRAG_MOTION], 0, x, y, drag_actor, picked_actor);
}

/**
 * shell_xdnd_handler_get_session_locked:
 * @self: a #ShellXdndHandler
 *
 * Returns: whether the session is locked
 */
gboolean
shell_xdnd_handler_get_session_locked (ShellXdndHandler *self)
{
  g_return_val_if_fail (SHELL_IS_XDND_HANDLER (self), FALSE);

  return self->session_locked;
}

/**
 * shell_xdnd_handler_set_session_locked:
 * @self: a #ShellXdndHandler
 * @locked: whether the session is locked
 *
 * Tells the handler whether the session is locked (`Main.sessionMode.isLocked`).
 * The cursor window is not cloned while the session is locked, and the
 * feedback group is hidden for the rest of a running drag. Call it whenever
 * the session mode is updated.
 */
void
shell_xdnd_handler_set_session_locked (ShellXdndHandler *self,
                                       gboolean          locked)
{
  g_return_if_fail (SHELL_IS_XDND_HANDLER (self));

  locked = !!locked;

  if (self->session_locked != locked)
    {
      self->session_locked = locked;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SESSION_LOCKED]);
    }

  if (self->drag_active)
    {
      ShellGlobal *global = shell_global_get ();
      ClutterActor *feedback_group =
        meta_compositor_get_feedback_group (shell_global_get_compositor (global));

      sync_cursor_window_clone (self);

      if (self->session_locked)
        clutter_actor_hide (feedback_group);
      else
        clutter_actor_show (feedback_group);
    }
}

static void
shell_xdnd_handler_constructed (GObject *object)
{
  ShellXdndHandler *self = SHELL_XDND_HANDLER (object);
  ShellGlobal *global = shell_global_get ();
  MetaDnd *dnd;
  ClutterActor *dummy;

  G_OBJECT_CLASS (shell_xdnd_handler_parent_class)->constructed (object);

  if (self->ui_group == NULL)
    {
      g_set_weak_pointer (&self->ui_group,
                          CLUTTER_ACTOR (shell_global_get_stage (global)));
    }

  dummy = g_object_new (CLUTTER_TYPE_ACTOR,
                        "width", 1.0,
                        "height", 1.0,
                        "opacity", 0,
                        NULL);
  g_set_weak_pointer (&self->dummy, dummy);
  clutter_actor_add_child (self->ui_group, dummy);
  clutter_actor_hide (dummy);

  dnd = meta_backend_get_dnd (shell_global_get_backend (global));
  g_signal_connect_object (dnd, "dnd-enter",
                           G_CALLBACK (on_dnd_enter), self, 0);
  g_signal_connect_object (dnd, "dnd-position-change",
                           G_CALLBACK (on_dnd_position_change), self, 0);
  g_signal_connect_object (dnd, "dnd-leave",
                           G_CALLBACK (on_dnd_leave), self, 0);
}

static void
shell_xdnd_handler_dispose (GObject *object)
{
  ShellXdndHandler *self = SHELL_XDND_HANDLER (object);

  if (self->window_group != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->window_group,
                                            window_group_visible_changed_cb,
                                            self);
      g_clear_weak_pointer (&self->window_group);
    }

  destroy_cursor_window_clone (self);

  if (self->dummy != NULL)
    {
      clutter_actor_destroy (self->dummy);
      g_clear_weak_pointer (&self->dummy);
    }

  g_clear_weak_pointer (&self->ui_group);

  G_OBJECT_CLASS (shell_xdnd_handler_parent_class)->dispose (object);
}

static void
shell_xdnd_handler_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellXdndHandler *self = SHELL_XDND_HANDLER (object);

  switch (prop_id)
    {
    case PROP_UI_GROUP:
      g_value_set_object (value, self->ui_group);
      break;

    case PROP_SESSION_LOCKED:
      g_value_set_boolean (value, self->session_locked);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_xdnd_handler_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellXdndHandler *self = SHELL_XDND_HANDLER (object);

  switch (prop_id)
    {
    case PROP_UI_GROUP:
      g_set_weak_pointer (&self->ui_group, g_value_get_object (value));
      break;

    case PROP_SESSION_LOCKED:
      shell_xdnd_handler_set_session_locked (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_xdnd_handler_class_init (ShellXdndHandlerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_xdnd_handler_constructed;
  object_class->dispose = shell_xdnd_handler_dispose;
  object_class->get_property = shell_xdnd_handler_get_property;
  object_class->set_property = shell_xdnd_handler_set_property;

  /**
   * ShellXdndHandler:ui-group:
   *
   * The actor the dummy drag actor and the clone of the cursor window are
   * added to (`Main.uiGroup`). The stage is used when it is not given.
   */
  props[PROP_UI_GROUP] =
    g_param_spec_object ("ui-group", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellXdndHandler:session-locked:
   *
   * Whether the session is locked, mirrors `Main.sessionMode.isLocked`.
   */
  props[PROP_SESSION_LOCKED] =
    g_param_spec_boolean ("session-locked", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellXdndHandler::drag-begin:
   * @self: the #ShellXdndHandler
   * @time: the current X server time
   *
   * Emitted when a drag from an X11 client enters the shell.
   */
  signals[DRAG_BEGIN] =
    g_signal_new ("drag-begin",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_UINT);

  /**
   * ShellXdndHandler::drag-end:
   * @self: the #ShellXdndHandler
   *
   * Emitted when the drag ends, whether it was dropped or cancelled.
   */
  signals[DRAG_END] =
    g_signal_new ("drag-end",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellXdndHandler::drag-motion:
   * @self: the #ShellXdndHandler
   * @x: the pointer X position in stage coordinates
   * @y: the pointer Y position in stage coordinates
   * @drag_actor: the actor standing for the dragged item: a clone of the
   *   cursor window if there is one, an invisible dummy otherwise
   * @target_actor: (nullable): the reactive actor under the pointer
   *
   * Emitted when the pointer moves during a drag. The consumers run the
   * drag monitors and the `handleDragOver` delegates of @target_actor and its
   * ancestors, with @self as the source of the drag.
   */
  signals[DRAG_MOTION] =
    g_signal_new ("drag-motion",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 4,
                  G_TYPE_INT,
                  G_TYPE_INT,
                  CLUTTER_TYPE_ACTOR,
                  CLUTTER_TYPE_ACTOR);
}

static void
shell_xdnd_handler_init (ShellXdndHandler *self)
{
}

/**
 * shell_xdnd_handler_new:
 * @ui_group: (nullable): the actor the helper actors are added to
 *
 * Starts listening to the XDND events of the backend.
 *
 * Returns: (transfer full): a new #ShellXdndHandler
 */
ShellXdndHandler *
shell_xdnd_handler_new (ClutterActor *ui_group)
{
  g_return_val_if_fail (ui_group == NULL || CLUTTER_IS_ACTOR (ui_group), NULL);

  return g_object_new (SHELL_TYPE_XDND_HANDLER,
                       "ui-group", ui_group,
                       NULL);
}
