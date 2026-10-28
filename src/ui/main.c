/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * main.c: The modal stack of the shell (`Main.pushModal ()` and
 * `Main.popModal ()`) and the action mode derived from it. Port of the modal
 * part of js/ui/main.js (`pushModal`, `popModal`, `_findModal`, `modalCount`,
 * `actionMode` and `modalActorFocusStack`); the rest of that file is still JS
 * and mirrors #ShellMain:action-mode and #ShellMain:modal-count into its
 * `actionMode` and `modalCount` exports.
 */

#include "config.h"

#include <gio/gio.h>
#include <meta/compositor.h>

#include "shell-global.h"
#include "ui/main.h"

enum
{
  PROP_0,

  PROP_ACTION_MODE,
  PROP_MODAL_COUNT,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  MODAL_ENDED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

/*
 * One entry of the former `modalActorFocusStack`: what a pushModal ()
 * invocation recorded so that popModal () can undo it. The records are
 * never moved in memory, popping a record from the middle of the stack
 * shifts the focus chain between them instead.
 */
typedef struct
{
  ShellMain *main;

  ClutterActor *actor;            /* strong, as the JS record held it */
  ClutterGrab *grab;              /* strong */
  gulong destroy_id;              /* "destroy" handler on @actor */

  ClutterActor *prev_focus;       /* strong, nullable */
  gulong prev_focus_destroy_id;   /* "destroy" handler on @prev_focus */

  ShellActionMode action_mode;    /* the action mode before this push */
} ModalRecord;

struct _ShellMain
{
  GObject parent;

  GPtrArray *modal_stack;         /* of ModalRecord */
  guint modal_count;
  ShellActionMode action_mode;
};

G_DEFINE_FINAL_TYPE (ShellMain, shell_main, G_TYPE_OBJECT)

static void
modal_record_free (ModalRecord *record)
{
  if (record->destroy_id != 0 && record->actor != NULL)
    g_signal_handler_disconnect (record->actor, record->destroy_id);

  if (record->prev_focus != NULL && record->prev_focus_destroy_id != 0)
    g_signal_handler_disconnect (record->prev_focus, record->prev_focus_destroy_id);

  g_clear_object (&record->prev_focus);
  g_clear_object (&record->grab);
  g_clear_object (&record->actor);
  g_free (record);
}

/* _findModal () */
static int
find_modal (ShellMain   *self,
            ClutterGrab *grab)
{
  guint i;

  if (grab == NULL)
    return -1;

  for (i = 0; i < self->modal_stack->len; i++)
    {
      ModalRecord *record = g_ptr_array_index (self->modal_stack, i);

      if (record->grab == grab)
        return i;
    }

  return -1;
}

static void
set_modal_count (ShellMain *self,
                 guint      modal_count)
{
  if (self->modal_count == modal_count)
    return;

  self->modal_count = modal_count;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_MODAL_COUNT]);
}

static void
prev_focus_destroyed_cb (ClutterActor *prev_focus,
                         ShellMain    *self)
{
  guint i;

  /* Only the first record is updated, like Array.findIndex (): every record
   * that recorded this actor has a handler of its own. */
  for (i = 0; i < self->modal_stack->len; i++)
    {
      ModalRecord *record = g_ptr_array_index (self->modal_stack, i);

      if (record->prev_focus == prev_focus)
        {
          /* The handler stays connected to the dying actor; it is not
           * disconnected later because prev_focus is now NULL. */
          record->prev_focus_destroy_id = 0;
          g_clear_object (&record->prev_focus);
          break;
        }
    }
}

static void
modal_actor_destroyed_cb (ClutterActor *actor,
                          ModalRecord  *record)
{
  ShellMain *self = record->main;
  g_autoptr (ClutterGrab) grab = g_object_ref (record->grab);
  g_autoptr (GError) error = NULL;

  if (find_modal (self, grab) >= 0 &&
      !shell_main_pop_modal (self, grab, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

static void
set_action_mode (ShellMain       *self,
                 ShellActionMode  action_mode)
{
  if (self->action_mode == action_mode)
    return;

  self->action_mode = action_mode;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTION_MODE]);
}

static void
shell_main_get_property (GObject    *object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  ShellMain *self = SHELL_MAIN (object);

  switch (prop_id)
    {
    case PROP_ACTION_MODE:
      g_value_set_uint (value, self->action_mode);
      break;

    case PROP_MODAL_COUNT:
      g_value_set_uint (value, self->modal_count);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_main_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  ShellMain *self = SHELL_MAIN (object);

  switch (prop_id)
    {
    case PROP_ACTION_MODE:
      shell_main_set_action_mode (self, g_value_get_uint (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_main_dispose (GObject *object)
{
  ShellMain *self = SHELL_MAIN (object);

  g_clear_pointer (&self->modal_stack, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_main_parent_class)->dispose (object);
}

static void
shell_main_class_init (ShellMainClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_main_dispose;
  object_class->get_property = shell_main_get_property;
  object_class->set_property = shell_main_set_property;

  /**
   * ShellMain:action-mode:
   *
   * The current #ShellActionMode, used to filter global keybindings and
   * gestures (`Main.actionMode`). It is the mode of the topmost modal, and
   * %SHELL_ACTION_MODE_NORMAL once the modal stack is empty. It starts as
   * %SHELL_ACTION_MODE_NONE until the startup is complete, when the shell
   * sets it to %SHELL_ACTION_MODE_NORMAL. Notified whenever it changes.
   */
  props[PROP_ACTION_MODE] =
    g_param_spec_uint ("action-mode", NULL, NULL,
                       0, G_MAXUINT, SHELL_ACTION_MODE_NONE,
                       G_PARAM_READWRITE |
                       G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellMain:modal-count:
   *
   * The number of modals currently pushed (`Main.modalCount`).
   */
  props[PROP_MODAL_COUNT] =
    g_param_spec_uint ("modal-count", NULL, NULL,
                       0, G_MAXUINT, 0,
                       G_PARAM_READABLE |
                       G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellMain::modal-ended:
   * @self: the #ShellMain
   *
   * Emitted when the last modal was popped, after the modal count has
   * dropped to 0 and the grab has been dismissed, and before the action mode
   * goes back to %SHELL_ACTION_MODE_NORMAL (`layoutManager.modalEnded ()`).
   */
  signals[MODAL_ENDED] =
    g_signal_new ("modal-ended",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_main_init (ShellMain *self)
{
  self->modal_stack = g_ptr_array_new_with_free_func ((GDestroyNotify) modal_record_free);
  self->action_mode = SHELL_ACTION_MODE_NONE;
}

/**
 * shell_main_get_default:
 *
 * Gets the modal stack of the shell.
 *
 * Returns: (transfer none): the #ShellMain singleton
 */
ShellMain *
shell_main_get_default (void)
{
  static ShellMain *instance = NULL;

  if (instance == NULL)
    instance = g_object_new (SHELL_TYPE_MAIN, NULL);

  return instance;
}

/**
 * shell_main_push_modal:
 * @self: a #ShellMain
 * @actor: actor which will be given keyboard focus
 * @action_mode: used to set the current #ShellActionMode to filter global
 *   keybindings; %SHELL_ACTION_MODE_NONE filters out all keybindings
 *
 * Ensure we are in a mode where all keyboard and mouse input goes to the
 * stage, and focus @actor. Multiple calls to this function act in a stacking
 * fashion; the effect will be undone when an equal number of
 * shell_main_pop_modal () invocations have been made.
 *
 * Next, record the current Clutter keyboard focus on a stack. If the modal
 * stack returns to this actor, reset the focus to the actor which was focused
 * at the time shell_main_push_modal () was invoked.
 *
 * The modal is popped automatically if @actor is destroyed.
 *
 * Returns: (transfer none): the grab handle created. It is owned by the modal
 *   stack until it is passed to shell_main_pop_modal (); keep a reference to
 *   it if it is needed afterwards. `Main.pushModal ()` returned the same
 *   grab.
 */
ClutterGrab *
shell_main_push_modal (ShellMain       *self,
                       ClutterActor    *actor,
                       ShellActionMode  action_mode)
{
  ShellGlobal *global = shell_global_get ();
  ClutterStage *stage = shell_global_get_stage (global);
  ClutterActor *prev_focus;
  ModalRecord *record;
  ClutterGrab *grab;

  g_return_val_if_fail (SHELL_IS_MAIN (self), NULL);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), NULL);

  grab = clutter_stage_grab (stage, actor);

  if (self->modal_count == 0)
    meta_compositor_disable_unredirect (shell_global_get_compositor (global));

  set_modal_count (self, self->modal_count + 1);

  record = g_new0 (ModalRecord, 1);
  record->main = self;
  record->actor = g_object_ref (actor);
  record->grab = grab;
  record->destroy_id = g_signal_connect (actor, "destroy",
                                         G_CALLBACK (modal_actor_destroyed_cb),
                                         record);

  prev_focus = clutter_stage_get_key_focus (stage);
  if (prev_focus != NULL)
    {
      record->prev_focus = g_object_ref (prev_focus);
      record->prev_focus_destroy_id =
        g_signal_connect (prev_focus, "destroy",
                          G_CALLBACK (prev_focus_destroyed_cb), self);
    }

  record->action_mode = self->action_mode;
  g_ptr_array_add (self->modal_stack, record);

  set_action_mode (self, action_mode);
  clutter_stage_set_key_focus (stage,
                               actor == CLUTTER_ACTOR (stage) ? NULL : actor);

  return grab;
}

/**
 * shell_main_pop_modal:
 * @self: a #ShellMain
 * @grab: (nullable): the grab given by shell_main_push_modal ()
 * @error: return location for a #GError
 *
 * Reverse the effect of shell_main_push_modal (). If this invocation is
 * undoing the topmost invocation, then the focus will be restored to the
 * previous focus at the time when shell_main_push_modal () was invoked.
 *
 * If @grab is not on the modal stack, the key focus is cleared, the action
 * mode goes back to %SHELL_ACTION_MODE_NORMAL and @error is set (`Main.popModal ()`
 * threw an "incorrect pop" error).
 *
 * Returns: %TRUE if the modal was popped
 */
gboolean
shell_main_pop_modal (ShellMain    *self,
                      ClutterGrab  *grab,
                      GError      **error)
{
  ShellGlobal *global = shell_global_get ();
  ClutterStage *stage = shell_global_get_stage (global);
  g_autoptr (ClutterGrab) grab_ref = NULL;
  g_autoptr (ClutterActor) prev_focus = NULL;
  ModalRecord *record;
  ShellActionMode record_action_mode;
  gboolean is_top;
  guint index;
  int focus_index;

  g_return_val_if_fail (SHELL_IS_MAIN (self), FALSE);

  /* The record can go away while the grab is being dismissed */
  if (grab != NULL)
    grab_ref = g_object_ref (grab);

  focus_index = find_modal (self, grab);
  if (focus_index < 0)
    {
      clutter_stage_set_key_focus (stage, NULL);
      set_action_mode (self, SHELL_ACTION_MODE_NORMAL);

      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "incorrect pop");
      return FALSE;
    }

  set_modal_count (self, self->modal_count - 1);

  record = g_ptr_array_index (self->modal_stack, focus_index);
  g_signal_handler_disconnect (record->actor, record->destroy_id);
  record->destroy_id = 0;

  clutter_grab_dismiss (record->grab);

  is_top = (guint) focus_index == self->modal_stack->len - 1;
  if (is_top)
    {
      if (record->prev_focus != NULL)
        {
          g_signal_handler_disconnect (record->prev_focus,
                                       record->prev_focus_destroy_id);
          record->prev_focus_destroy_id = 0;
        }

      /* record->prev_focus is kept (and still owned by the record) until the
       * record is removed below, like the JS record kept it */
      prev_focus = record->prev_focus != NULL ?
        g_object_ref (record->prev_focus) : NULL;
      record_action_mode = record->action_mode;

      set_action_mode (self, record_action_mode);
      clutter_stage_set_key_focus (stage, prev_focus);
    }
  else
    {
      ModalRecord *top;

      /* If we have:
       *     global.stage.set_focus (a);
       *     push_modal (b);
       *     push_modal (c);
       *     push_modal (d);
       *
       * then we have the stack:
       *     [{ prev_focus: a, actor: b },
       *      { prev_focus: b, actor: c },
       *      { prev_focus: c, actor: d }]
       *
       * When actor c is destroyed/popped, if we only simply remove the
       * record, then the focus stack will be [a, c], rather than the correct
       * [a, b]. Shift the focus stack up before removing the record to ensure
       * that we get the correct result.
       */
      top = g_ptr_array_index (self->modal_stack, self->modal_stack->len - 1);
      if (top->prev_focus != NULL)
        {
          g_signal_handler_disconnect (top->prev_focus,
                                       top->prev_focus_destroy_id);
          top->prev_focus_destroy_id = 0;
          g_clear_object (&top->prev_focus);
        }

      /* Remove from the middle, shift the focus chain up. The prev_focus of
       * the record being removed moves to the record above it. */
      for (index = self->modal_stack->len - 1; index > (guint) focus_index; index--)
        {
          ModalRecord *cur = g_ptr_array_index (self->modal_stack, index);
          ModalRecord *below = g_ptr_array_index (self->modal_stack, index - 1);

          cur->prev_focus = below->prev_focus;
          cur->prev_focus_destroy_id = below->prev_focus_destroy_id;
          cur->action_mode = below->action_mode;

          below->prev_focus = NULL;
          below->prev_focus_destroy_id = 0;
        }
    }

  /* The set_key_focus () above can run arbitrary handlers: look the record up
   * again instead of trusting the index. */
  if (g_ptr_array_find (self->modal_stack, record, &index))
    g_ptr_array_remove_index (self->modal_stack, index);

  if (self->modal_count > 0)
    return TRUE;

  g_signal_emit (self, signals[MODAL_ENDED], 0);
  meta_compositor_enable_unredirect (shell_global_get_compositor (global));
  set_action_mode (self, SHELL_ACTION_MODE_NORMAL);

  return TRUE;
}

/**
 * shell_main_get_modal_count:
 * @self: a #ShellMain
 *
 * Returns: the number of modals currently pushed
 */
guint
shell_main_get_modal_count (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), 0);

  return self->modal_count;
}

/**
 * shell_main_get_action_mode:
 * @self: a #ShellMain
 *
 * Returns: the current action mode
 */
ShellActionMode
shell_main_get_action_mode (ShellMain *self)
{
  g_return_val_if_fail (SHELL_IS_MAIN (self), SHELL_ACTION_MODE_NONE);

  return self->action_mode;
}

/**
 * shell_main_set_action_mode:
 * @self: a #ShellMain
 * @action_mode: the new action mode
 *
 * Sets the action mode directly. The modal stack manages the action mode on
 * its own; this is only for the shell to leave %SHELL_ACTION_MODE_NONE once
 * the startup is complete.
 */
void
shell_main_set_action_mode (ShellMain       *self,
                            ShellActionMode  action_mode)
{
  g_return_if_fail (SHELL_IS_MAIN (self));

  set_action_mode (self, action_mode);
}
