/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * grab-helper.c: Keyboard and pointer grabs associated with a set of
 * actors. Port of js/ui/grabHelper.js.
 *
 * Creates a #ShellGrabHelper object, for dealing with keyboard and pointer
 * grabs associated with a set of actors.
 *
 * Note that the grab can be automatically dropped at any time by the user, and
 * your code just needs to deal with it; you shouldn't adjust behavior directly
 * after you call shell_grab_helper_ungrab (), but instead pass an @on_ungrab
 * callback when you call shell_grab_helper_grab ().
 *
 * The modal itself is pushed on the #ShellMain modal stack when the first
 * actor is grabbed and popped when the last one is ungrabbed.
 */

#include "config.h"

#include "shell-global.h"
#include "st.h"
#include "ui/grab-helper.h"
#include "shell-enum-types.h"
#include "ui/main.h"

enum
{
  PROP_0,

  PROP_OWNER,
  PROP_ACTION_MODE,
  PROP_GRABBED,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

/* One entry of the grab stack: the former `params` objects */
typedef struct
{
  ClutterActor *actor;       /* weak */
  ClutterActor *saved_focus; /* weak, the key focus when the grab was taken */

  ShellGrabHelperUngrabFunc on_ungrab;
  gpointer user_data;
  GDestroyNotify destroy;
} GrabRecord;

struct _ShellGrabHelper
{
  GObject parent;

  ClutterActor *owner;       /* weak, the actor that owns the grab helper */
  ShellActionMode action_mode;

  GPtrArray *grab_stack;     /* of GrabRecord */

  guint modal_count;
  ClutterGrab *grab;         /* the grab of the modal pushed for @owner */
  ClutterAction *click_gesture;   /* on @owner while the modal is pushed */
  ClutterAction *key_controller;  /* on @owner once it has been grabbed */
};

G_DEFINE_FINAL_TYPE (ShellGrabHelper, shell_grab_helper, G_TYPE_OBJECT)

static void
grab_record_free (GrabRecord *record)
{
  if (record->destroy != NULL)
    record->destroy (record->user_data);

  g_clear_weak_pointer (&record->saved_focus);
  g_clear_weak_pointer (&record->actor);
  g_free (record);
}

static ClutterStage *
get_stage (void)
{
  return shell_global_get_stage (shell_global_get ());
}

static GrabRecord *
get_current_grab (ShellGrabHelper *self)
{
  if (self->grab_stack->len == 0)
    return NULL;

  return g_ptr_array_index (self->grab_stack, self->grab_stack->len - 1);
}

/* _isWithinGrabbedActor () */
static gboolean
is_within_grabbed_actor (ShellGrabHelper *self,
                         ClutterActor    *actor)
{
  GrabRecord *current = get_current_grab (self);
  ClutterActor *current_actor = current != NULL ? current->actor : NULL;

  while (actor != NULL)
    {
      if (actor == current_actor)
        return TRUE;

      actor = clutter_actor_get_parent (actor);
    }

  return FALSE;
}

/* _findStackIndex () */
static int
find_stack_index (ShellGrabHelper *self,
                  ClutterActor    *actor)
{
  guint i;

  if (actor == NULL)
    return -1;

  for (i = 0; i < self->grab_stack->len; i++)
    {
      GrabRecord *record = g_ptr_array_index (self->grab_stack, i);

      if (record->actor == actor)
        return i;
    }

  return -1;
}

/* _actorInGrabStack () */
static int
actor_in_grab_stack (ShellGrabHelper *self,
                     ClutterActor    *actor)
{
  while (actor != NULL)
    {
      int index = find_stack_index (self, actor);

      if (index >= 0)
        return index;

      actor = clutter_actor_get_parent (actor);
    }

  return -1;
}

static void
release_modal_grab (ShellGrabHelper *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (ClutterGrab) grab = NULL;

  self->modal_count--;
  if (self->modal_count > 0)
    return;

  if (self->click_gesture != NULL)
    {
      if (self->owner != NULL &&
          clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->click_gesture)) == self->owner)
        clutter_actor_remove_action (self->owner, self->click_gesture);

      g_clear_object (&self->click_gesture);
    }

  grab = g_steal_pointer (&self->grab);
  if (!shell_main_pop_modal (shell_main_get_default (), grab, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

/* Drops the grabs from @index up, the body of ungrab () */
static void
ungrab_from_index (ShellGrabHelper *self,
                   guint            index,
                   gboolean         is_user)
{
  g_autoptr (ShellGrabHelper) self_ref = g_object_ref (self);
  g_autoptr (GPtrArray) popped_grabs = NULL;
  ClutterActor *focus = clutter_stage_get_key_focus (get_stage ());
  gboolean had_focus = focus != NULL && is_within_grabbed_actor (self, focus);
  gboolean was_grabbed = self->grab_stack->len > 0;
  int i;

  /* "Pop" all newly ungrabbed actors off the grab stack */
  popped_grabs = g_ptr_array_new_with_free_func ((GDestroyNotify) grab_record_free);
  while (self->grab_stack->len > index)
    g_ptr_array_add (popped_grabs, g_ptr_array_steal_index (self->grab_stack, index));

  if (was_grabbed != (self->grab_stack->len > 0))
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_GRABBED]);

  for (i = popped_grabs->len - 1; i >= 0; i--)
    {
      GrabRecord *popped_grab = g_ptr_array_index (popped_grabs, i);

      if (popped_grab->on_ungrab != NULL)
        popped_grab->on_ungrab (is_user, popped_grab->user_data);

      release_modal_grab (self);
    }

  if (had_focus)
    {
      GrabRecord *popped_grab = g_ptr_array_index (popped_grabs, 0);

      if (popped_grab->saved_focus != NULL)
        clutter_actor_grab_key_focus (popped_grab->saved_focus);
    }
}

static void
click_recognized_cb (ClutterGesture   *gesture,
                     ShellGrabHelper  *self)
{
  g_autoptr (ShellGrabHelper) self_ref = g_object_ref (self);
  const ClutterEvent *event = clutter_gesture_get_point_event (gesture, 0);
  ClutterActor *target_actor;
  GrabRecord *current = get_current_grab (self);

  if (current == NULL || event == NULL)
    return;

  target_actor = clutter_stage_get_event_actor (get_stage (), event);

  if (current->actor == NULL || target_actor == NULL ||
      !clutter_actor_contains (current->actor, target_actor))
    {
      guint index = actor_in_grab_stack (self, target_actor) + 1;

      if (index < self->grab_stack->len)
        ungrab_from_index (self, index, TRUE);
    }
}

static gboolean
key_press_cb (ClutterKeyController *key_controller,
              ShellGrabHelper      *self)
{
  g_autoptr (ShellGrabHelper) self_ref = g_object_ref (self);
  uint32_t key = 0;

  /* The controller stays on the owner between grabs */
  if (self->grab_stack->len == 0)
    return CLUTTER_EVENT_PROPAGATE;

  clutter_key_controller_get_key (key_controller, &key, NULL, NULL);
  if (key == CLUTTER_KEY_Escape)
    {
      ungrab_from_index (self, self->grab_stack->len - 1, TRUE);
      return CLUTTER_EVENT_STOP;
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static gboolean
take_modal_grab (ShellGrabHelper *self)
{
  if (self->modal_count == 0)
    {
      ClutterGrab *grab;

      if (self->owner == NULL)
        return FALSE;

      grab = shell_main_push_modal (shell_main_get_default (),
                                    self->owner, self->action_mode);
      if (grab == NULL)
        return FALSE;

      self->grab = g_object_ref (grab);

      self->click_gesture = g_object_ref_sink (clutter_click_gesture_new ());
      g_signal_connect_object (self->click_gesture, "recognize",
                               G_CALLBACK (click_recognized_cb), self, 0);
      clutter_actor_add_action (self->owner, self->click_gesture);

      if (self->key_controller == NULL)
        {
          self->key_controller = g_object_ref_sink (clutter_key_controller_new (NULL));
          g_signal_connect_object (self->key_controller, "key-press",
                                   G_CALLBACK (key_press_cb), self, 0);
          clutter_actor_add_action_full (self->owner,
                                         "grab-helper-key-capture",
                                         CLUTTER_PHASE_CAPTURE,
                                         self->key_controller);
        }
    }

  self->modal_count++;
  return TRUE;
}

static void
shell_grab_helper_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  ShellGrabHelper *self = SHELL_GRAB_HELPER (object);

  switch (prop_id)
    {
    case PROP_OWNER:
      g_value_set_object (value, self->owner);
      break;

    case PROP_ACTION_MODE:
      g_value_set_flags (value, self->action_mode);
      break;

    case PROP_GRABBED:
      g_value_set_boolean (value, self->grab_stack->len > 0);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_grab_helper_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  ShellGrabHelper *self = SHELL_GRAB_HELPER (object);

  switch (prop_id)
    {
    case PROP_OWNER:
      g_set_weak_pointer (&self->owner, g_value_get_object (value));
      break;

    case PROP_ACTION_MODE:
      self->action_mode = g_value_get_flags (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_grab_helper_constructed (GObject *object)
{
  ShellGrabHelper *self = SHELL_GRAB_HELPER (object);

  G_OBJECT_CLASS (shell_grab_helper_parent_class)->constructed (object);

  if (self->owner == NULL)
    g_critical ("GrabHelper owner must be a Clutter.Actor");
}

static void
shell_grab_helper_dispose (GObject *object)
{
  ShellGrabHelper *self = SHELL_GRAB_HELPER (object);

  /* A modal still pushed for the owner is left to the modal stack, which pops
   * it when the owner is destroyed. */
  if (self->click_gesture != NULL)
    {
      if (self->owner != NULL &&
          clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->click_gesture)) == self->owner)
        clutter_actor_remove_action (self->owner, self->click_gesture);

      g_clear_object (&self->click_gesture);
    }

  if (self->key_controller != NULL)
    {
      if (self->owner != NULL &&
          clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->key_controller)) == self->owner)
        clutter_actor_remove_action (self->owner, self->key_controller);

      g_clear_object (&self->key_controller);
    }

  g_clear_pointer (&self->grab_stack, g_ptr_array_unref);
  g_clear_object (&self->grab);
  g_clear_weak_pointer (&self->owner);

  G_OBJECT_CLASS (shell_grab_helper_parent_class)->dispose (object);
}

static void
shell_grab_helper_class_init (ShellGrabHelperClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_grab_helper_constructed;
  object_class->dispose = shell_grab_helper_dispose;
  object_class->get_property = shell_grab_helper_get_property;
  object_class->set_property = shell_grab_helper_set_property;

  /**
   * ShellGrabHelper:owner:
   *
   * The actor that owns the grab helper: the modal pushed for the grabs is
   * grabbed for it, and the click and Escape handling is attached to it.
   */
  props[PROP_OWNER] =
    g_param_spec_object ("owner", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellGrabHelper:action-mode:
   *
   * The #ShellActionMode of the modal pushed for the grabs (the `actionMode`
   * of the modal params of the former GrabHelper constructor).
   */
  props[PROP_ACTION_MODE] =
    g_param_spec_flags ("action-mode", NULL, NULL,
                        SHELL_TYPE_ACTION_MODE, SHELL_ACTION_MODE_NONE,
                       G_PARAM_READWRITE |
                       G_PARAM_CONSTRUCT_ONLY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellGrabHelper:grabbed:
   *
   * Whether at least one actor is currently grabbed.
   */
  props[PROP_GRABBED] =
    g_param_spec_boolean ("grabbed", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_grab_helper_init (ShellGrabHelper *self)
{
  self->grab_stack = g_ptr_array_new_with_free_func ((GDestroyNotify) grab_record_free);
}

/**
 * shell_grab_helper_new:
 * @owner: the actor that owns the grab helper
 * @action_mode: the #ShellActionMode of the modal pushed for the grabs,
 *   %SHELL_ACTION_MODE_NONE filters out all keybindings
 *
 * Creates a new grab helper, the equivalent of `new GrabHelper (owner,
 * { actionMode })`.
 *
 * Returns: (transfer full): a new #ShellGrabHelper
 */
ShellGrabHelper *
shell_grab_helper_new (ClutterActor    *owner,
                       ShellActionMode  action_mode)
{
  g_return_val_if_fail (CLUTTER_IS_ACTOR (owner), NULL);

  return g_object_new (SHELL_TYPE_GRAB_HELPER,
                       "owner", owner,
                       "action-mode", action_mode,
                       NULL);
}

/**
 * shell_grab_helper_grab:
 * @self: a #ShellGrabHelper
 * @actor: the actor to grab
 * @focus: (nullable): the actor to give the key focus to
 * @on_ungrab: (nullable) (scope notified) (closure user_data) (destroy destroy):
 *   called when the grab is dropped
 * @user_data: data for @on_ungrab
 * @destroy: (nullable): called when @on_ungrab is not needed any more
 *
 * The general effect of a "grab" is to ensure that the passed in @actor
 * and all actors inside the grab get exclusive control of the mouse and
 * keyboard, with the grab automatically being dropped if the user tries
 * to dismiss it.
 *
 * This can be called multiple times, with the scope of the grab being
 * changed to a different actor every time. A nested grab does not have
 * to have its grabbed actor inside the parent grab actors.
 *
 * Grabs can be automatically dropped if the user tries to dismiss it
 * in one of two ways: the user clicking outside the currently grabbed
 * actor, or the user typing the Escape key.
 *
 * If the user clicks outside the grabbed actors, and the clicked on
 * actor is part of a previous grab in the stack, grabs will be popped
 * until that grab is active. However, the click event will not be
 * replayed to the actor.
 *
 * If the user types the Escape key, one grab from the grab stack will
 * be popped.
 *
 * When a grab is popped by user interacting as described above, @on_ungrab
 * is called with %TRUE.
 *
 * If @focus is not %NULL, we'll set the key focus directly to that actor
 * instead of navigating in @actor. This is for use cases like menus, where
 * we want to grab the menu actor, but keep focus on the clicked on menu item.
 *
 * If @actor is already grabbed nothing happens and @on_ungrab is dropped
 * without being called.
 *
 * Returns: %TRUE if @actor is grabbed
 */
gboolean
shell_grab_helper_grab (ShellGrabHelper           *self,
                        ClutterActor              *actor,
                        ClutterActor              *focus,
                        ShellGrabHelperUngrabFunc  on_ungrab,
                        gpointer                   user_data,
                        GDestroyNotify             destroy)
{
  g_autoptr (ShellGrabHelper) self_ref = NULL;
  ClutterActor *key_focus;
  gboolean had_focus;
  GrabRecord *record;
  gboolean grabbed = FALSE;

  g_return_val_if_fail (SHELL_IS_GRAB_HELPER (self), FALSE);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), FALSE);
  g_return_val_if_fail (focus == NULL || CLUTTER_IS_ACTOR (focus), FALSE);

  self_ref = g_object_ref (self);

  key_focus = clutter_stage_get_key_focus (get_stage ());
  had_focus = key_focus != NULL && is_within_grabbed_actor (self, key_focus);

  if (find_stack_index (self, actor) >= 0)
    {
      if (destroy != NULL)
        destroy (user_data);

      return TRUE;
    }

  if (!take_modal_grab (self))
    {
      if (destroy != NULL)
        destroy (user_data);

      return FALSE;
    }

  record = g_new0 (GrabRecord, 1);
  g_set_weak_pointer (&record->actor, actor);
  g_set_weak_pointer (&record->saved_focus, key_focus);
  record->on_ungrab = on_ungrab;
  record->user_data = user_data;
  record->destroy = destroy;

  g_ptr_array_add (self->grab_stack, record);
  if (self->grab_stack->len == 1)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_GRABBED]);
  grabbed = TRUE;

  if (focus != NULL)
    {
      clutter_actor_grab_key_focus (focus);
    }
  else if (had_focus)
    {
      if (!ST_IS_WIDGET (actor) ||
          !st_widget_navigate_focus (ST_WIDGET (actor), NULL,
                                     ST_DIR_TAB_FORWARD, FALSE))
        clutter_actor_grab_key_focus (actor);
    }

  return grabbed;
}

static void
grab_async_ungrab_cb (gboolean  is_user,
                      gpointer  user_data)
{
  GTask *task = user_data;

  g_task_return_boolean (task, is_user);
}

/**
 * shell_grab_helper_grab_async:
 * @self: a #ShellGrabHelper
 * @actor: the actor to grab
 * @focus: (nullable): the actor to give the key focus to
 * @callback: (scope async): called when the grab has been dropped
 * @user_data: data for @callback
 *
 * Like shell_grab_helper_grab (), but completes when the grab is dropped
 * instead of calling an `on_ungrab` function (`grabAsync ()`).
 */
void
shell_grab_helper_grab_async (ShellGrabHelper     *self,
                              ClutterActor        *actor,
                              ClutterActor        *focus,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_GRAB_HELPER (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, shell_grab_helper_grab_async);

  if (!shell_grab_helper_grab (self, actor, focus,
                               grab_async_ungrab_cb,
                               g_object_ref (task), g_object_unref))
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Grab failed");
}

/**
 * shell_grab_helper_grab_finish:
 * @self: a #ShellGrabHelper
 * @result: a #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns: whether the user dropped the grab (the argument the `on_ungrab`
 *   function would have got); %FALSE with @error set if the grab failed
 */
gboolean
shell_grab_helper_grab_finish (ShellGrabHelper  *self,
                               GAsyncResult     *result,
                               GError          **error)
{
  g_return_val_if_fail (SHELL_IS_GRAB_HELPER (self), FALSE);
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/**
 * shell_grab_helper_ungrab_actor:
 * @self: a #ShellGrabHelper
 * @actor: (nullable): the actor to ungrab, the one of the topmost grab if
 *   %NULL
 * @is_user: whether the user is dropping the grab; passed to the `on_ungrab`
 *   functions of the dropped grabs
 *
 * Pops @actor from the grab stack, potentially dropping the grab. If the
 * actor is not on the grab stack, this call is ignored with no ill effects.
 *
 * If the actor is not at the top of the grab stack, grabs are popped until
 * the grabbed actor is at the top of the grab stack. The `on_ungrab`
 * function of every popped grab is called with @is_user, the topmost first.
 */
void
shell_grab_helper_ungrab_actor (ShellGrabHelper *self,
                                ClutterActor    *actor,
                                gboolean         is_user)
{
  int index;

  g_return_if_fail (SHELL_IS_GRAB_HELPER (self));
  g_return_if_fail (actor == NULL || CLUTTER_IS_ACTOR (actor));

  if (actor == NULL)
    index = (int) self->grab_stack->len - 1;
  else
    index = find_stack_index (self, actor);

  if (index < 0)
    return;

  ungrab_from_index (self, index, !!is_user);
}

/**
 * shell_grab_helper_ungrab:
 * @self: a #ShellGrabHelper
 * @is_user: whether the user is dropping the grab
 *
 * Pops the topmost grab, see shell_grab_helper_ungrab_actor ().
 */
void
shell_grab_helper_ungrab (ShellGrabHelper *self,
                          gboolean         is_user)
{
  shell_grab_helper_ungrab_actor (self, NULL, is_user);
}

/**
 * shell_grab_helper_is_actor_grabbed:
 * @self: a #ShellGrabHelper
 * @actor: (nullable): an actor
 *
 * Returns: whether @actor is on the grab stack
 */
gboolean
shell_grab_helper_is_actor_grabbed (ShellGrabHelper *self,
                                    ClutterActor    *actor)
{
  g_return_val_if_fail (SHELL_IS_GRAB_HELPER (self), FALSE);

  return find_stack_index (self, actor) >= 0;
}

/**
 * shell_grab_helper_get_grabbed:
 * @self: a #ShellGrabHelper
 *
 * Returns: whether at least one actor is grabbed
 */
gboolean
shell_grab_helper_get_grabbed (ShellGrabHelper *self)
{
  g_return_val_if_fail (SHELL_IS_GRAB_HELPER (self), FALSE);

  return self->grab_stack->len > 0;
}

/**
 * shell_grab_helper_get_current_actor:
 * @self: a #ShellGrabHelper
 *
 * Returns: (transfer none) (nullable): the actor of the topmost grab
 *   (`currentGrab.actor`), or %NULL if nothing is grabbed
 */
ClutterActor *
shell_grab_helper_get_current_actor (ShellGrabHelper *self)
{
  GrabRecord *current;

  g_return_val_if_fail (SHELL_IS_GRAB_HELPER (self), NULL);

  current = get_current_grab (self);
  return current != NULL ? current->actor : NULL;
}
