/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * modal-dialog.c: A dialog that grabs the input of the whole shell while it
 * is open. Port of js/ui/modalDialog.js.
 *
 * The widget tree is the one of the JS class:
 *
 *   ShellModalDialog        bound to the size of the stage, in the container
 *    +- StBin             covers one monitor (see below)
 *    |   +- StWidget      background-stack, a ClutterBinLayout
 *    |       +- ShellDialog   dialog-layout
 *    |       +- ClutterActor  event blocker (not with shell-reactive)
 *    +- ShellLightbox     (not with shell-reactive)
 *
 * Differences from the JS class, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `Main.layoutManager.modalDialogGroup` is not reachable from C yet (the
 *    layout manager is still JS): main.js hands it over with
 *    shell_modal_dialog_set_default_container () once the layout manager
 *    exists, and every dialog is added to it unless it is constructed with an
 *    explicit "container".
 *  - `Layout.MonitorConstraint` is not ported yet either: the bin that
 *    covers the monitor of the dialog uses a private constraint that reads the
 *    geometry of the monitor from the display, as #ShellOsdWindow does. It
 *    has no work-area or primary mode as the dialog never used them.
 *  - `Main.layoutManager.emit ('system-modal-opened')` is
 *    shell_main_system_modal_opened (); main.js forwards that signal to the
 *    layout manager so its JS listeners still get it.
 */

#include "config.h"

#include <atk/atk.h>
#include <clutter/clutter.h>
#include <meta/display.h>
#include <meta/meta-backend.h>
#include <meta/meta-monitor-manager.h>

#include "shell-enum-types.h"
#include "shell-global.h"
#include "ui/environment.h"
#include "ui/lightbox.h"
#include "ui/main.h"
#include "ui/modal-dialog.h"

#define OPEN_AND_CLOSE_TIME 100
#define FADE_OUT_DIALOG_TIME 1000

/* ------------------------------------------------------------------------ */
/* The constraint that covers one monitor                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MODAL_DIALOG_MONITOR_CONSTRAINT (shell_modal_dialog_monitor_constraint_get_type ())
G_DECLARE_FINAL_TYPE (ShellModalDialogMonitorConstraint,
                      shell_modal_dialog_monitor_constraint,
                      SHELL, MODAL_DIALOG_MONITOR_CONSTRAINT,
                      ClutterConstraint)

struct _ShellModalDialogMonitorConstraint
{
  ClutterConstraint parent;

  int index;
};

G_DEFINE_FINAL_TYPE (ShellModalDialogMonitorConstraint,
                     shell_modal_dialog_monitor_constraint,
                     CLUTTER_TYPE_CONSTRAINT)

static void
shell_modal_dialog_monitor_constraint_update_allocation (ClutterConstraint *constraint,
                                                         ClutterActor      *actor,
                                                         ClutterActorBox   *allocation)
{
  ShellModalDialogMonitorConstraint *self =
    SHELL_MODAL_DIALOG_MONITOR_CONSTRAINT (constraint);
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  int n_monitors = meta_display_get_n_monitors (display);
  MtkRectangle geometry;

  /* Like Layout.MonitorConstraint: nothing to do without a monitor index (or
   * monitors), and the index is clamped to the existing monitors */
  if (self->index < 0 || n_monitors <= 0)
    return;

  meta_display_get_monitor_geometry (display,
                                     MIN (self->index, n_monitors - 1),
                                     &geometry);
  clutter_actor_box_init_rect (allocation,
                               geometry.x, geometry.y,
                               geometry.width, geometry.height);
}

static void
shell_modal_dialog_monitor_constraint_class_init (ShellModalDialogMonitorConstraintClass *klass)
{
  ClutterConstraintClass *constraint_class = CLUTTER_CONSTRAINT_CLASS (klass);

  constraint_class->update_allocation =
    shell_modal_dialog_monitor_constraint_update_allocation;
}

static void
shell_modal_dialog_monitor_constraint_init (ShellModalDialogMonitorConstraint *self)
{
  self->index = -1;
}

static void
monitor_constraint_set_index (ShellModalDialogMonitorConstraint *self,
                              int                                index)
{
  ClutterActor *actor;

  self->index = index;

  actor = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self));
  if (actor != NULL)
    clutter_actor_queue_relayout (actor);
}

/* ------------------------------------------------------------------------ */
/* ShellModalDialog                                                         */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_SHELL_REACTIVE,
  PROP_DIALOG_STYLE_CLASS,
  PROP_ACTION_MODE,
  PROP_SHOULD_FADE_IN,
  PROP_SHOULD_FADE_OUT,
  PROP_DESTROY_ON_CLOSE,
  PROP_CONTAINER,

  PROP_STATE,
  PROP_DIALOG_LAYOUT,
  PROP_CONTENT_LAYOUT,
  PROP_BUTTON_LAYOUT,
  PROP_BACKGROUND_STACK,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  OPENED,
  CLOSED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

typedef struct
{
  /* construct properties */
  gboolean shell_reactive;
  char *dialog_style_class;
  ShellActionMode action_mode;
  gboolean should_fade_in;
  gboolean should_fade_out;
  gboolean destroy_on_close;
  ClutterActor *container; /* weak, only until constructed () */

  ShellModalDialogState state;

  /* children; the actor hierarchy owns them */
  StWidget *background_stack; /* weak */
  StWidget *background_bin;   /* weak */
  ShellDialog *dialog_layout; /* weak */
  ShellLightbox *lightbox;    /* weak */
  ClutterActor *event_blocker; /* weak */
  ShellModalDialogMonitorConstraint *monitor_constraint; /* weak */

  gboolean has_modal;
  ClutterGrab *grab; /* strong */

  ClutterActor *initial_key_focus; /* weak */
  ClutterActor *saved_key_focus;   /* weak */
} ShellModalDialogPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellModalDialog, shell_modal_dialog, ST_TYPE_WIDGET)

static ClutterActor *default_container = NULL; /* weak */

/**
 * shell_modal_dialog_set_default_container:
 * @container: (nullable): the actor that new dialogs are added to
 *
 * Sets the actor that a #ShellModalDialog constructed without a
 * #ShellModalDialog:container is added to. main.js calls this once with
 * `Main.layoutManager.modalDialogGroup`, right after the layout manager is
 * created, as the layout manager is not ported to C yet. The actor is not
 * kept alive.
 */
void
shell_modal_dialog_set_default_container (ClutterActor *container)
{
  g_return_if_fail (container == NULL || CLUTTER_IS_ACTOR (container));

  if (default_container == container)
    return;

  g_clear_weak_pointer (&default_container);
  if (container != NULL)
    g_set_weak_pointer (&default_container, container);
}

static void
set_state (ShellModalDialog      *self,
           ShellModalDialogState  state)
{
  ShellModalDialogPrivate *priv = shell_modal_dialog_get_instance_private (self);

  if (priv->state == state)
    return;

  priv->state = state;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_STATE]);
}

/**
 * shell_modal_dialog_get_state:
 * @self: a #ShellModalDialog
 *
 * Returns: the #ShellModalDialogState of the dialog (`state` in the old JS
 *   class)
 */
ShellModalDialogState
shell_modal_dialog_get_state (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv;

  g_return_val_if_fail (SHELL_IS_MODAL_DIALOG (self), SHELL_MODAL_DIALOG_STATE_CLOSED);

  priv = shell_modal_dialog_get_instance_private (self);
  return priv->state;
}

/**
 * shell_modal_dialog_get_dialog_layout:
 * @self: a #ShellModalDialog
 *
 * Returns: (transfer none): the #ShellDialog inside the dialog (`dialogLayout`
 *   in the old JS class)
 */
ShellDialog *
shell_modal_dialog_get_dialog_layout (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv;

  g_return_val_if_fail (SHELL_IS_MODAL_DIALOG (self), NULL);

  priv = shell_modal_dialog_get_instance_private (self);
  return priv->dialog_layout;
}

/**
 * shell_modal_dialog_get_content_layout:
 * @self: a #ShellModalDialog
 *
 * Returns: (transfer none): the content area of the dialog (`contentLayout` in
 *   the old JS class)
 */
StBoxLayout *
shell_modal_dialog_get_content_layout (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv;

  g_return_val_if_fail (SHELL_IS_MODAL_DIALOG (self), NULL);

  priv = shell_modal_dialog_get_instance_private (self);
  if (priv->dialog_layout == NULL)
    return NULL;

  return shell_dialog_get_content_layout (priv->dialog_layout);
}

/**
 * shell_modal_dialog_get_button_layout:
 * @self: a #ShellModalDialog
 *
 * Returns: (transfer none): the row of buttons of the dialog (`buttonLayout`
 *   in the old JS class)
 */
StWidget *
shell_modal_dialog_get_button_layout (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv;

  g_return_val_if_fail (SHELL_IS_MODAL_DIALOG (self), NULL);

  priv = shell_modal_dialog_get_instance_private (self);
  if (priv->dialog_layout == NULL)
    return NULL;

  return shell_dialog_get_button_layout (priv->dialog_layout);
}

/**
 * shell_modal_dialog_get_background_stack:
 * @self: a #ShellModalDialog
 *
 * Returns: (transfer none): the widget that stacks the dialog and whatever
 *   else is shown with it, such as a spinner (`backgroundStack` in the old JS
 *   class)
 */
StWidget *
shell_modal_dialog_get_background_stack (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv;

  g_return_val_if_fail (SHELL_IS_MODAL_DIALOG (self), NULL);

  priv = shell_modal_dialog_get_instance_private (self);
  return priv->background_stack;
}

/**
 * shell_modal_dialog_clear_buttons:
 * @self: a #ShellModalDialog
 *
 * Removes all the buttons of the dialog (`clearButtons ()`). `setButtons
 * ([...])` is a call to this followed by shell_modal_dialog_add_button ()
 * for each button.
 */
void
shell_modal_dialog_clear_buttons (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv;

  g_return_if_fail (SHELL_IS_MODAL_DIALOG (self));

  priv = shell_modal_dialog_get_instance_private (self);
  if (priv->dialog_layout != NULL)
    shell_dialog_clear_buttons (priv->dialog_layout);
}

/**
 * shell_modal_dialog_add_button:
 * @self: a #ShellModalDialog
 * @label: (nullable): the text of the button
 * @key: a key symbol (such as `Clutter.KEY_Escape`) that activates the
 *   button when released, or 0 for none
 * @is_default: whether this is the default button
 * @action: (scope notified) (nullable) (closure user_data) (destroy destroy):
 *   called when the button is clicked or its key is released
 * @user_data: data for @action
 * @destroy: called on @user_data when the button is destroyed
 *
 * Adds a button to the end of the button row, see shell_dialog_add_button ().
 * `addButton ({label, action, key, default})` is `add_button (label, key ||
 * 0, !!default, action)`.
 *
 * Returns: (transfer none) (nullable): the new button
 */
StButton *
shell_modal_dialog_add_button (ShellModalDialog          *self,
                               const char                *label,
                               uint32_t                   key,
                               gboolean                   is_default,
                               ShellDialogButtonCallback  action,
                               gpointer                   user_data,
                               GDestroyNotify             destroy)
{
  ShellModalDialogPrivate *priv;

  g_return_val_if_fail (SHELL_IS_MODAL_DIALOG (self), NULL);

  priv = shell_modal_dialog_get_instance_private (self);
  if (priv->dialog_layout == NULL)
    {
      if (destroy != NULL)
        destroy (user_data);
      return NULL;
    }

  return shell_dialog_add_button (priv->dialog_layout, label, key, is_default,
                                  action, user_data, destroy);
}

static void
clear_initial_key_focus (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv = shell_modal_dialog_get_instance_private (self);

  if (priv->initial_key_focus == NULL)
    return;

  g_signal_handlers_disconnect_by_data (priv->initial_key_focus, self);
  g_clear_weak_pointer (&priv->initial_key_focus);
}

static void
initial_key_focus_destroy_cb (ClutterActor     *actor,
                              ShellModalDialog *self)
{
  clear_initial_key_focus (self);
}

/**
 * shell_modal_dialog_set_initial_key_focus:
 * @self: a #ShellModalDialog
 * @actor: (nullable): the actor that gets the key focus when the dialog is
 *   opened. %NULL goes back to the initial key focus of the #ShellDialog
 *
 * Chooses the actor that gets the key focus when the dialog becomes modal
 * (`setInitialKeyFocus ()`). The dialog forgets it when it is destroyed.
 */
void
shell_modal_dialog_set_initial_key_focus (ShellModalDialog *self,
                                          ClutterActor     *actor)
{
  ShellModalDialogPrivate *priv;

  g_return_if_fail (SHELL_IS_MODAL_DIALOG (self));
  g_return_if_fail (actor == NULL || CLUTTER_IS_ACTOR (actor));

  priv = shell_modal_dialog_get_instance_private (self);

  clear_initial_key_focus (self);

  if (actor != NULL)
    {
      g_set_weak_pointer (&priv->initial_key_focus, actor);
      g_signal_connect (actor, "destroy",
                        G_CALLBACK (initial_key_focus_destroy_cb), self);
    }
}

static void
fade_open_done_cb (gboolean  finished,
                   gpointer  user_data)
{
  ShellModalDialog *self = user_data;

  /* onComplete: only when the animation ran to its end */
  if (!finished)
    return;

  set_state (self, SHELL_MODAL_DIALOG_STATE_OPENED);
  g_signal_emit (self, signals[OPENED], 0);
}

static void
fade_open (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv = shell_modal_dialog_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellEaseParams params = {
    .duration = priv->should_fade_in ? OPEN_AND_CLOSE_TIME : 0,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  if (priv->monitor_constraint != NULL)
    {
      MetaDisplay *display = shell_global_get_display (shell_global_get ());

      monitor_constraint_set_index (priv->monitor_constraint,
                                    meta_display_get_current_monitor (display));
    }

  set_state (self, SHELL_MODAL_DIALOG_STATE_OPENING);

  if (priv->dialog_layout != NULL)
    clutter_actor_set_opacity (CLUTTER_ACTOR (priv->dialog_layout), 255);
  if (priv->lightbox != NULL)
    shell_lightbox_light_on (priv->lightbox, 0);
  clutter_actor_set_opacity (actor, 0);
  clutter_actor_show (actor);

  shell_actor_ease (actor, &params,
                    fade_open_done_cb, g_object_ref (self), g_object_unref,
                    "opacity", 255u,
                    NULL);
}

/**
 * shell_modal_dialog_push_modal:
 * @self: a #ShellModalDialog
 *
 * Makes the dialog modal again after shell_modal_dialog_pop_modal () and gives
 * the key focus back to where it was (`pushModal ()`), or to the initial key
 * focus. Does nothing if the dialog is modal already.
 *
 * Returns: %TRUE
 */
gboolean
shell_modal_dialog_push_modal (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv;
  ShellMain *shell_main;
  ClutterActor *focus;

  g_return_val_if_fail (SHELL_IS_MODAL_DIALOG (self), FALSE);

  priv = shell_modal_dialog_get_instance_private (self);

  if (priv->has_modal)
    return TRUE;

  shell_main = shell_main_get_default ();

  g_clear_object (&priv->grab);
  priv->grab = shell_main_push_modal (shell_main, CLUTTER_ACTOR (self),
                                      priv->action_mode);
  if (priv->grab != NULL)
    g_object_ref (priv->grab);

  shell_main_system_modal_opened (shell_main);

  priv->has_modal = TRUE;
  if (priv->saved_key_focus != NULL)
    {
      focus = priv->saved_key_focus;
      g_clear_weak_pointer (&priv->saved_key_focus);
    }
  else if (priv->initial_key_focus != NULL)
    {
      focus = priv->initial_key_focus;
    }
  else if (priv->dialog_layout != NULL)
    {
      focus = shell_dialog_get_initial_key_focus (priv->dialog_layout);
    }
  else
    {
      focus = NULL;
    }

  if (focus != NULL)
    clutter_actor_grab_key_focus (focus);

  if (!priv->shell_reactive && priv->event_blocker != NULL)
    clutter_actor_set_child_below_sibling (CLUTTER_ACTOR (priv->background_stack),
                                           priv->event_blocker, NULL);

  return TRUE;
}

/**
 * shell_modal_dialog_pop_modal:
 * @self: a #ShellModalDialog
 *
 * Drops the modal status without closing the dialog (`popModal ()`). This
 * makes the dialog insensitive as well, so it needs to be followed shortly by
 * either shell_modal_dialog_close () or shell_modal_dialog_push_modal ().
 */
void
shell_modal_dialog_pop_modal (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv;
  ClutterStage *stage;
  ClutterActor *focus;
  g_autoptr (GError) error = NULL;

  g_return_if_fail (SHELL_IS_MODAL_DIALOG (self));

  priv = shell_modal_dialog_get_instance_private (self);

  if (!priv->has_modal)
    return;

  stage = shell_global_get_stage (shell_global_get ());
  focus = clutter_stage_get_key_focus (stage);
  g_clear_weak_pointer (&priv->saved_key_focus);
  if (focus != NULL && clutter_actor_contains (CLUTTER_ACTOR (self), focus))
    g_set_weak_pointer (&priv->saved_key_focus, focus);

  if (!shell_main_pop_modal (shell_main_get_default (), priv->grab, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
  g_clear_object (&priv->grab);
  priv->has_modal = FALSE;

  if (!priv->shell_reactive && priv->event_blocker != NULL)
    clutter_actor_set_child_above_sibling (CLUTTER_ACTOR (priv->background_stack),
                                           priv->event_blocker, NULL);
}

static gboolean
shell_modal_dialog_real_open (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv = shell_modal_dialog_get_instance_private (self);

  if (priv->state == SHELL_MODAL_DIALOG_STATE_OPENED ||
      priv->state == SHELL_MODAL_DIALOG_STATE_OPENING)
    return TRUE;

  if (!shell_modal_dialog_push_modal (self))
    return FALSE;

  fade_open (self);
  return TRUE;
}

/**
 * shell_modal_dialog_open:
 * @self: a #ShellModalDialog
 *
 * Makes the dialog modal and fades it in (`open ()`). The #ShellModalDialog::opened
 * signal is emitted when the dialog is fully visible. It is safe to call this
 * on a dialog that is open or opening already. This is the `open` virtual
 * function, which JS subclasses override with `vfunc_open ()`.
 *
 * Returns: %TRUE if the dialog is open or opening
 */
gboolean
shell_modal_dialog_open (ShellModalDialog *self)
{
  g_return_val_if_fail (SHELL_IS_MODAL_DIALOG (self), FALSE);

  return SHELL_MODAL_DIALOG_GET_CLASS (self)->open (self);
}

static void
close_complete (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv = shell_modal_dialog_get_instance_private (self);

  set_state (self, SHELL_MODAL_DIALOG_STATE_CLOSED);
  clutter_actor_hide (CLUTTER_ACTOR (self));
  g_signal_emit (self, signals[CLOSED], 0);

  if (priv->destroy_on_close)
    clutter_actor_destroy (CLUTTER_ACTOR (self));
}

static void
fade_close_done_cb (gboolean  finished,
                    gpointer  user_data)
{
  /* onComplete: only when the animation ran to its end */
  if (!finished)
    return;

  close_complete (user_data);
}

static void
shell_modal_dialog_real_close (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv = shell_modal_dialog_get_instance_private (self);

  if (priv->state == SHELL_MODAL_DIALOG_STATE_CLOSED ||
      priv->state == SHELL_MODAL_DIALOG_STATE_CLOSING)
    return;

  set_state (self, SHELL_MODAL_DIALOG_STATE_CLOSING);
  shell_modal_dialog_pop_modal (self);
  g_clear_weak_pointer (&priv->saved_key_focus);

  if (priv->should_fade_out)
    {
      ShellEaseParams params = {
        .duration = OPEN_AND_CLOSE_TIME,
        .delay = -1,
        .mode = CLUTTER_EASE_OUT_QUAD,
        .repeat_count = 0,
        .auto_reverse = FALSE,
        .animation_required = FALSE,
      };

      shell_actor_ease (CLUTTER_ACTOR (self), &params,
                        fade_close_done_cb, g_object_ref (self), g_object_unref,
                        "opacity", 0u,
                        NULL);
    }
  else
    {
      close_complete (self);
    }
}

/**
 * shell_modal_dialog_close:
 * @self: a #ShellModalDialog
 *
 * Drops the modal status and fades the dialog out (`close ()`). The
 * #ShellModalDialog::closed signal is emitted once it is hidden, after which
 * it is destroyed if #ShellModalDialog:destroy-on-close is set. This is the
 * `close` virtual function, which JS subclasses override with `vfunc_close ()`.
 */
void
shell_modal_dialog_close (ShellModalDialog *self)
{
  g_return_if_fail (SHELL_IS_MODAL_DIALOG (self));

  SHELL_MODAL_DIALOG_GET_CLASS (self)->close (self);
}

static void
fade_out_dialog_done_cb (gboolean  finished,
                         gpointer  user_data)
{
  /* onComplete: only when the animation ran to its end */
  if (!finished)
    return;

  set_state (user_data, SHELL_MODAL_DIALOG_STATE_FADED_OUT);
}

/**
 * shell_modal_dialog_fade_out_dialog:
 * @self: a #ShellModalDialog
 *
 * This method is like shell_modal_dialog_close (), but fades the dialog out
 * much slower, and leaves the lightbox in place (`_fadeOutDialog ()`). Once
 * in the faded out state, the dialog can be brought back by an open call, or
 * the lightbox can be dismissed by a close call.
 *
 * The main point of this method is to give some indication to the user that
 * the dialog response has been acknowledged but will take a few moments before
 * being processed. E.g., if a user clicked "Log Out" then the dialog should go
 * away immediately, but the lightbox should remain until the logout is
 * complete.
 */
void
shell_modal_dialog_fade_out_dialog (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv;
  ShellEaseParams params = {
    .duration = FADE_OUT_DIALOG_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  g_return_if_fail (SHELL_IS_MODAL_DIALOG (self));

  priv = shell_modal_dialog_get_instance_private (self);

  if (priv->state == SHELL_MODAL_DIALOG_STATE_CLOSED ||
      priv->state == SHELL_MODAL_DIALOG_STATE_CLOSING)
    return;

  if (priv->state == SHELL_MODAL_DIALOG_STATE_FADED_OUT)
    return;

  shell_modal_dialog_pop_modal (self);

  if (priv->dialog_layout == NULL)
    return;

  shell_actor_ease (CLUTTER_ACTOR (priv->dialog_layout), &params,
                    fade_out_dialog_done_cb, g_object_ref (self), g_object_unref,
                    "opacity", 0u,
                    NULL);
}

static void
monitors_changed_cb (MetaMonitorManager *monitor_manager,
                     ShellModalDialog   *self)
{
  ShellModalDialogPrivate *priv = shell_modal_dialog_get_instance_private (self);

  if (priv->background_bin != NULL)
    clutter_actor_queue_relayout (CLUTTER_ACTOR (priv->background_bin));
}

static void
shell_modal_dialog_constructed (GObject *object)
{
  ShellModalDialog *self = SHELL_MODAL_DIALOG (object);
  ShellModalDialogPrivate *priv = shell_modal_dialog_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellGlobal *global = shell_global_get ();
  ClutterActor *container;
  StWidget *background_stack, *background_bin;
  ClutterConstraint *constraint;
  ShellModalDialogMonitorConstraint *monitor_constraint;
  ShellDialog *dialog_layout;

  G_OBJECT_CLASS (shell_modal_dialog_parent_class)->constructed (object);

  clutter_actor_hide (actor);
  clutter_actor_set_reactive (actor, TRUE);
  clutter_actor_set_position (actor, 0, 0);
  clutter_actor_set_accessible_role (actor, ATK_ROLE_DIALOG);

  container = priv->container != NULL ? priv->container : default_container;
  g_clear_weak_pointer (&priv->container);
  if (container != NULL)
    clutter_actor_add_child (container, actor);
  else
    g_critical ("ShellModalDialog needs a container: call "
                "shell_modal_dialog_set_default_container ()");

  constraint = clutter_bind_constraint_new (CLUTTER_ACTOR (shell_global_get_stage (global)),
                                            CLUTTER_BIND_ALL, 0);
  clutter_actor_add_constraint (actor, constraint);

  background_stack = g_object_new (ST_TYPE_WIDGET,
                                   "layout-manager", clutter_bin_layout_new (),
                                   "x-expand", TRUE,
                                   "y-expand", TRUE,
                                   NULL);
  g_set_weak_pointer (&priv->background_stack, background_stack);

  background_bin = st_bin_new ();
  st_bin_set_child (ST_BIN (background_bin), CLUTTER_ACTOR (background_stack));
  g_set_weak_pointer (&priv->background_bin, background_bin);

  monitor_constraint = g_object_new (SHELL_TYPE_MODAL_DIALOG_MONITOR_CONSTRAINT,
                                     NULL);
  clutter_actor_add_constraint (CLUTTER_ACTOR (background_bin),
                                CLUTTER_CONSTRAINT (monitor_constraint));
  g_set_weak_pointer (&priv->monitor_constraint, monitor_constraint);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (background_bin));

  dialog_layout = shell_dialog_new (CLUTTER_ACTOR (background_stack),
                                    priv->dialog_style_class);
  g_set_weak_pointer (&priv->dialog_layout, dialog_layout);

  if (!priv->shell_reactive)
    {
      ShellLightbox *lightbox;
      ClutterActor *event_blocker;

      lightbox = g_object_new (SHELL_TYPE_LIGHTBOX,
                               "container", actor,
                               "inhibit-events", TRUE,
                               "radial-effect", TRUE,
                               NULL);
      g_set_weak_pointer (&priv->lightbox, lightbox);
      shell_lightbox_highlight (lightbox, CLUTTER_ACTOR (background_bin));

      event_blocker = clutter_actor_new ();
      clutter_actor_set_reactive (event_blocker, TRUE);
      clutter_actor_add_child (CLUTTER_ACTOR (background_stack), event_blocker);
      g_set_weak_pointer (&priv->event_blocker, event_blocker);
    }

  st_focus_manager_add_group (shell_global_get_focus_manager (global),
                              ST_WIDGET (priv->dialog_layout));

  g_signal_connect_object (meta_backend_get_monitor_manager (shell_global_get_backend (global)),
                           "monitors-changed",
                           G_CALLBACK (monitors_changed_cb), self, 0);
}

static void
shell_modal_dialog_dispose (GObject *object)
{
  ShellModalDialog *self = SHELL_MODAL_DIALOG (object);
  ShellModalDialogPrivate *priv = shell_modal_dialog_get_instance_private (self);

  clear_initial_key_focus (self);

  g_clear_weak_pointer (&priv->saved_key_focus);
  g_clear_weak_pointer (&priv->container);
  g_clear_weak_pointer (&priv->background_stack);
  g_clear_weak_pointer (&priv->background_bin);
  g_clear_weak_pointer (&priv->dialog_layout);
  g_clear_weak_pointer (&priv->lightbox);
  g_clear_weak_pointer (&priv->event_blocker);
  g_clear_weak_pointer (&priv->monitor_constraint);
  g_clear_object (&priv->grab);

  G_OBJECT_CLASS (shell_modal_dialog_parent_class)->dispose (object);
}

static void
shell_modal_dialog_finalize (GObject *object)
{
  ShellModalDialog *self = SHELL_MODAL_DIALOG (object);
  ShellModalDialogPrivate *priv = shell_modal_dialog_get_instance_private (self);

  g_clear_pointer (&priv->dialog_style_class, g_free);

  G_OBJECT_CLASS (shell_modal_dialog_parent_class)->finalize (object);
}

static void
shell_modal_dialog_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellModalDialog *self = SHELL_MODAL_DIALOG (object);
  ShellModalDialogPrivate *priv = shell_modal_dialog_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SHELL_REACTIVE:
      g_value_set_boolean (value, priv->shell_reactive);
      break;

    case PROP_DIALOG_STYLE_CLASS:
      g_value_set_string (value, priv->dialog_style_class);
      break;

    case PROP_ACTION_MODE:
      g_value_set_flags (value, priv->action_mode);
      break;

    case PROP_SHOULD_FADE_IN:
      g_value_set_boolean (value, priv->should_fade_in);
      break;

    case PROP_SHOULD_FADE_OUT:
      g_value_set_boolean (value, priv->should_fade_out);
      break;

    case PROP_DESTROY_ON_CLOSE:
      g_value_set_boolean (value, priv->destroy_on_close);
      break;

    case PROP_CONTAINER:
      g_value_set_object (value, priv->container);
      break;

    case PROP_STATE:
      g_value_set_enum (value, priv->state);
      break;

    case PROP_DIALOG_LAYOUT:
      g_value_set_object (value, priv->dialog_layout);
      break;

    case PROP_CONTENT_LAYOUT:
      g_value_set_object (value, shell_modal_dialog_get_content_layout (self));
      break;

    case PROP_BUTTON_LAYOUT:
      g_value_set_object (value, shell_modal_dialog_get_button_layout (self));
      break;

    case PROP_BACKGROUND_STACK:
      g_value_set_object (value, priv->background_stack);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_modal_dialog_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellModalDialog *self = SHELL_MODAL_DIALOG (object);
  ShellModalDialogPrivate *priv = shell_modal_dialog_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SHELL_REACTIVE:
      priv->shell_reactive = g_value_get_boolean (value);
      break;

    case PROP_DIALOG_STYLE_CLASS:
      g_free (priv->dialog_style_class);
      priv->dialog_style_class = g_value_dup_string (value);
      break;

    case PROP_ACTION_MODE:
      priv->action_mode = g_value_get_flags (value);
      break;

    case PROP_SHOULD_FADE_IN:
      priv->should_fade_in = g_value_get_boolean (value);
      break;

    case PROP_SHOULD_FADE_OUT:
      priv->should_fade_out = g_value_get_boolean (value);
      break;

    case PROP_DESTROY_ON_CLOSE:
      priv->destroy_on_close = g_value_get_boolean (value);
      break;

    case PROP_CONTAINER:
      g_clear_weak_pointer (&priv->container);
      if (g_value_get_object (value) != NULL)
        g_set_weak_pointer (&priv->container, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_modal_dialog_class_init (ShellModalDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_modal_dialog_constructed;
  object_class->dispose = shell_modal_dialog_dispose;
  object_class->finalize = shell_modal_dialog_finalize;
  object_class->get_property = shell_modal_dialog_get_property;
  object_class->set_property = shell_modal_dialog_set_property;

  klass->open = shell_modal_dialog_real_open;
  klass->close = shell_modal_dialog_real_close;

  /**
   * ShellModalDialog:shell-reactive:
   *
   * Whether the shell, that is the rest of the screen, still reacts to input
   * while the dialog is open. When it does not, a lightbox dims the monitor
   * and an event blocker keeps the dialog from being clicked while it is not
   * modal. (`shellReactive` in the old JS class)
   */
  props[PROP_SHELL_REACTIVE] =
    g_param_spec_boolean ("shell-reactive", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellModalDialog:dialog-style-class:
   *
   * A style class for the frame of the #ShellDialog inside the dialog
   * (`styleClass` in the old JS class; not the style class of the widget).
   */
  props[PROP_DIALOG_STYLE_CLASS] =
    g_param_spec_string ("dialog-style-class", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellModalDialog:action-mode:
   *
   * The #ShellActionMode the shell is in while the dialog is modal
   * (`actionMode` in the old JS class)
   */
  props[PROP_ACTION_MODE] =
    g_param_spec_flags ("action-mode", NULL, NULL,
                        SHELL_TYPE_ACTION_MODE, SHELL_ACTION_MODE_SYSTEM_MODAL,
                        G_PARAM_READWRITE |
                        G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS);

  /**
   * ShellModalDialog:should-fade-in:
   *
   * Whether the dialog fades in when opened (`shouldFadeIn`)
   */
  props[PROP_SHOULD_FADE_IN] =
    g_param_spec_boolean ("should-fade-in", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellModalDialog:should-fade-out:
   *
   * Whether the dialog fades out when closed (`shouldFadeOut`)
   */
  props[PROP_SHOULD_FADE_OUT] =
    g_param_spec_boolean ("should-fade-out", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellModalDialog:destroy-on-close:
   *
   * Whether the dialog is destroyed once it is closed (`destroyOnClose`)
   */
  props[PROP_DESTROY_ON_CLOSE] =
    g_param_spec_boolean ("destroy-on-close", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellModalDialog:container:
   *
   * The actor the dialog is added to. If %NULL, the actor given to
   * shell_modal_dialog_set_default_container (), which is
   * `Main.layoutManager.modalDialogGroup`.
   */
  props[PROP_CONTAINER] =
    g_param_spec_object ("container", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellModalDialog:state:
   *
   * The #ShellModalDialogState of the dialog
   */
  props[PROP_STATE] =
    g_param_spec_enum ("state", NULL, NULL,
                       SHELL_TYPE_MODAL_DIALOG_STATE,
                       SHELL_MODAL_DIALOG_STATE_CLOSED,
                       G_PARAM_READABLE |
                       G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellModalDialog:dialog-layout:
   *
   * The #ShellDialog inside the dialog (`dialogLayout`)
   */
  props[PROP_DIALOG_LAYOUT] =
    g_param_spec_object ("dialog-layout", NULL, NULL,
                         SHELL_TYPE_DIALOG,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellModalDialog:content-layout:
   *
   * The content area of the dialog (`contentLayout`)
   */
  props[PROP_CONTENT_LAYOUT] =
    g_param_spec_object ("content-layout", NULL, NULL,
                         ST_TYPE_BOX_LAYOUT,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellModalDialog:button-layout:
   *
   * The row of buttons of the dialog (`buttonLayout`)
   */
  props[PROP_BUTTON_LAYOUT] =
    g_param_spec_object ("button-layout", NULL, NULL,
                         ST_TYPE_WIDGET,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellModalDialog:background-stack:
   *
   * The widget that stacks the dialog and whatever is shown along with it
   * (`backgroundStack`)
   */
  props[PROP_BACKGROUND_STACK] =
    g_param_spec_object ("background-stack", NULL, NULL,
                         ST_TYPE_WIDGET,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellModalDialog::opened:
   * @self: the #ShellModalDialog
   *
   * Emitted when the dialog is fully faded in
   */
  signals[OPENED] =
    g_signal_new ("opened",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellModalDialog::closed:
   * @self: the #ShellModalDialog
   *
   * Emitted when the dialog is closed and hidden, before it is destroyed if
   * #ShellModalDialog:destroy-on-close is set
   */
  signals[CLOSED] =
    g_signal_new ("closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_modal_dialog_init (ShellModalDialog *self)
{
  ShellModalDialogPrivate *priv = shell_modal_dialog_get_instance_private (self);

  priv->action_mode = SHELL_ACTION_MODE_SYSTEM_MODAL;
  priv->should_fade_in = TRUE;
  priv->should_fade_out = TRUE;
  priv->destroy_on_close = TRUE;
  priv->state = SHELL_MODAL_DIALOG_STATE_CLOSED;
}
