/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Animation helpers ported from js/ui/environment.js (Clutter.Actor.ease(),
 * Clutter.Actor.ease_property(), St.Adjustment.ease()) and
 * js/misc/animationUtils.js (adjustAnimationTime()).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <gobject/gvaluecollector.h>
#include <meta/compositor.h>

#include "ui/environment.h"

#include "shell-global.h"

static const ShellEaseParams default_params = {
  .duration = -1,
  .delay = -1,
  .mode = 0,
  .repeat_count = 0,
  .auto_reverse = FALSE,
  .animation_required = FALSE,
};

/**
 * shell_ease_params_new:
 *
 * Creates a new #ShellEaseParams with default values: duration and delay of
 * -1, no mode, no repeat, no auto-reverse and animations not required.
 *
 * Returns: (transfer full): a new #ShellEaseParams
 */
ShellEaseParams *
shell_ease_params_new (void)
{
  return g_memdup2 (&default_params, sizeof (ShellEaseParams));
}

/**
 * shell_ease_params_copy:
 * @params: a #ShellEaseParams
 *
 * Returns: (transfer full): a copy of @params
 */
ShellEaseParams *
shell_ease_params_copy (const ShellEaseParams *params)
{
  g_return_val_if_fail (params != NULL, NULL);

  return g_memdup2 (params, sizeof (ShellEaseParams));
}

/**
 * shell_ease_params_free:
 * @params: (transfer full): a #ShellEaseParams
 *
 * Frees @params.
 */
void
shell_ease_params_free (ShellEaseParams *params)
{
  g_free (params);
}

G_DEFINE_BOXED_TYPE (ShellEaseParams, shell_ease_params,
                     shell_ease_params_copy, shell_ease_params_free)

/**
 * shell_adjust_animation_time:
 * @msecs: time in milliseconds
 * @animation_required: whether to ignore the enable-animations setting
 *
 * Adjusts @msecs to account for the enable-animations and slow-down-factor
 * settings of #StSettings.
 *
 * Returns: the adjusted time in milliseconds, 0 if animations are disabled
 *   and @animation_required is %FALSE
 */
guint
shell_adjust_animation_time (guint    msecs,
                             gboolean animation_required)
{
  StSettings *settings = st_settings_get ();
  double adjusted;

  if (!st_settings_get_enable_animations (settings) && !animation_required)
    return 0;

  adjusted = st_settings_get_slow_down_factor (settings) * msecs;
  if (adjusted <= 0.0)
    return 0;
  if (adjusted >= (double) G_MAXUINT)
    return G_MAXUINT;

  return (guint) adjusted;
}

/* ------------------------------------------------------------------------ */

/* Pending completion of one ease request. Replaces the closures made by
 * _makeEaseCallback(), _setupTransitionCompletion() and
 * _makeEasePrepareAndCleanup() in js/ui/environment.js.
 */
typedef struct
{
  ShellEaseCallback callback;
  gpointer user_data;
  GDestroyNotify destroy;

  /* the easing duration is > 0: keep the compositor from unredirecting and
   * the shell marked as busy for the lifetime of the animation */
  gboolean uses_work;
  gboolean prepared;
  gboolean done;

  ClutterActor *actor;
  ClutterTransition *transition;
  ShellGlobal *global;

  gulong started_id;
  gulong stopped_id;
  gulong removed_id;
  gulong destroy_id;
} EaseState;

/* Every EaseState waiting on a transition. Dropped wholesale by
 * _shell_ease_shutdown(), which ShellGlobal runs before it emits
 * ::shutdown so that no completion callback fires into JS or Lua while
 * the UI is being torn down (the JS version achieved the same ordering
 * by connecting its session signal holder before any other module). */
static GList *live_states = NULL;

static EaseState *
ease_state_new (ShellEaseCallback callback,
                gpointer          user_data,
                GDestroyNotify    destroy,
                gboolean          uses_work)
{
  EaseState *state = g_new0 (EaseState, 1);

  state->callback = callback;
  state->user_data = user_data;
  state->destroy = destroy;
  state->uses_work = uses_work;

  return state;
}

static void
ease_state_disconnect (EaseState *state)
{
  g_clear_signal_handler (&state->started_id, state->transition);
  g_clear_signal_handler (&state->stopped_id, state->transition);
  g_clear_signal_handler (&state->removed_id, state->actor);
  g_clear_signal_handler (&state->destroy_id, state->actor);
  live_states = g_list_remove (live_states, state);
}

static void
ease_state_free (EaseState *state)
{
  ease_state_disconnect (state);

  g_clear_object (&state->actor);
  g_clear_object (&state->transition);
  g_free (state);
}

static void
ease_state_prepare (EaseState *state)
{
  MetaCompositor *compositor;
  ShellGlobal *global;

  if (!state->uses_work || state->prepared)
    return;

  global = shell_global_get ();
  compositor = shell_global_get_compositor (global);
  if (compositor)
    meta_compositor_disable_unredirect (compositor);
  shell_global_begin_work (global);

  state->prepared = TRUE;
}

static void
ease_state_cleanup (EaseState *state)
{
  MetaCompositor *compositor;
  ShellGlobal *global;

  if (!state->prepared)
    return;

  global = shell_global_get ();
  compositor = shell_global_get_compositor (global);
  if (compositor)
    meta_compositor_enable_unredirect (compositor);
  shell_global_end_work (global);

  state->prepared = FALSE;
}

static void
ease_state_complete (EaseState *state,
                     gboolean   finished)
{
  ShellEaseCallback callback;
  GDestroyNotify destroy;
  gpointer user_data;

  if (state->done)
    return;

  state->done = TRUE;

  ease_state_disconnect (state);
  ease_state_cleanup (state);

  callback = state->callback;
  destroy = state->destroy;
  user_data = state->user_data;

  ease_state_free (state);

  if (callback)
    callback (finished, user_data);
  if (destroy)
    destroy (user_data);
}

static void
ease_state_drop (EaseState *state)
{
  GDestroyNotify destroy = state->destroy;
  gpointer user_data = state->user_data;

  /* Like the JS signal holder tied to the session: drop everything without
   * reporting completion, the shell is going away. */
  state->done = TRUE;
  ease_state_free (state);

  if (destroy)
    destroy (user_data);
}

/**
 * _shell_ease_shutdown: (skip)
 *
 * Drops every pending ease completion without calling its callback.
 * Called by ShellGlobal right before it emits ::shutdown.
 */
void
_shell_ease_shutdown (void)
{
  while (live_states != NULL)
    ease_state_drop (live_states->data);
}

static void
on_transition_started (ClutterTimeline *timeline,
                       EaseState       *state)
{
  ease_state_prepare (state);
}

static void
on_transition_stopped (ClutterTimeline *timeline,
                       gboolean         is_finished,
                       EaseState       *state)
{
  ease_state_complete (state, is_finished);
}

static void
on_actor_transition_removed (ClutterActor      *actor,
                             ClutterTransition *transition,
                             gboolean           is_finished,
                             EaseState         *state)
{
  if (transition == state->transition)
    ease_state_complete (state, is_finished);
}

static void
on_actor_destroy (ClutterActor *actor,
                  EaseState    *state)
{
  ease_state_complete (state, FALSE);
}

/* Takes ownership of @state: it is freed when the transition ends. */
static void
ease_state_watch_transition (EaseState         *state,
                             ClutterActor      *actor,
                             ClutterTransition *transition)
{
  state->transition = g_object_ref (transition);
  state->actor = actor ? g_object_ref (actor) : NULL;
  state->global = shell_global_get ();
  live_states = g_list_prepend (live_states, state);

  if (state->uses_work)
    {
      if (clutter_timeline_get_delay (CLUTTER_TIMELINE (transition)) != 0)
        {
          state->started_id = g_signal_connect (transition, "started",
                                                G_CALLBACK (on_transition_started),
                                                state);
        }
      else
        {
          ease_state_prepare (state);
        }
    }

  state->stopped_id = g_signal_connect (transition, "stopped",
                                        G_CALLBACK (on_transition_stopped),
                                        state);

  if (actor)
    {
      state->removed_id = g_signal_connect (actor, "transition-removed",
                                            G_CALLBACK (on_actor_transition_removed),
                                            state);
      state->destroy_id = g_signal_connect (actor, "destroy",
                                            G_CALLBACK (on_actor_destroy),
                                            state);
    }
}

/* ------------------------------------------------------------------------ */

static gboolean
params_is_reversed (const ShellEaseParams *params)
{
  /* repeat_count doesn't include the initial iteration */
  gint64 n_iterations = (gint64) params->repeat_count + 1;

  /* whether the transition should finish where it started */
  return params->auto_reverse && n_iterations % 2 == 0;
}

/**
 * shell_actor_ease_begin:
 * @actor: a #ClutterActor
 * @params: (nullable): the easing parameters
 * @properties: (nullable) (array zero-terminated=1) (element-type utf8):
 *   names of the properties that are about to be set, as they appear in
 *   their #GParamSpec (with dashes)
 *
 * First half of Clutter.Actor.ease(). Saves the easing state of @actor,
 * applies the duration, delay and mode of @params (the duration and delay go
 * through shell_adjust_animation_time()), and removes the running transitions
 * of @properties, which are about to be overwritten.
 *
 * The caller must then set @properties on @actor, unless this function returns
 * %FALSE, and call shell_actor_ease_end(), which restores the easing state.
 *
 * Returns: %FALSE if setting the properties should be skipped (a zero-length,
 *   reversing animation ends where it started), %TRUE otherwise
 */
gboolean
shell_actor_ease_begin (ClutterActor          *actor,
                        const ShellEaseParams *params,
                        const char * const    *properties)
{
  guint duration;
  int i;

  g_return_val_if_fail (CLUTTER_IS_ACTOR (actor), FALSE);

  if (params == NULL)
    params = &default_params;

  clutter_actor_save_easing_state (actor);

  duration = params->duration >= 0
    ? (guint) params->duration
    : clutter_actor_get_easing_duration (actor);
  clutter_actor_set_easing_duration (actor,
                                     shell_adjust_animation_time (duration,
                                                                  params->animation_required));

  if (params->delay >= 0)
    {
      clutter_actor_set_easing_delay (actor,
                                      shell_adjust_animation_time (params->delay,
                                                                   params->animation_required));
    }

  if (params->mode != 0)
    clutter_actor_set_easing_mode (actor, params->mode);

  /* cancel overwritten transitions */
  for (i = 0; properties != NULL && properties[i] != NULL; i++)
    clutter_actor_remove_transition (actor, properties[i]);

  return clutter_actor_get_easing_duration (actor) > 0 ||
         !params_is_reversed (params);
}

/**
 * shell_actor_ease_end:
 * @actor: a #ClutterActor
 * @params: (nullable): the same parameters given to shell_actor_ease_begin()
 * @properties: (nullable) (array zero-terminated=1) (element-type utf8):
 *   the same property names given to shell_actor_ease_begin()
 * @callback: (scope async) (nullable) (closure user_data) (destroy destroy):
 *   called once the animation of the first property that got a transition
 *   ends, or at once if there is none
 * @user_data: data for @callback
 * @destroy: called on @user_data after @callback, or when the request is
 *   dropped
 *
 * Second half of Clutter.Actor.ease(). Must be called after
 * shell_actor_ease_begin() and the property changes, while the easing state
 * is still the one saved by shell_actor_ease_begin(); it restores it.
 */
void
shell_actor_ease_end (ClutterActor          *actor,
                      const ShellEaseParams *params,
                      const char * const    *properties,
                      ShellEaseCallback      callback,
                      gpointer               user_data,
                      GDestroyNotify         destroy)
{
  ClutterTransition *first = NULL;
  EaseState *state;
  int i;

  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  if (params == NULL)
    params = &default_params;

  state = ease_state_new (callback, user_data, destroy,
                          clutter_actor_get_easing_duration (actor) > 0);

  clutter_actor_restore_easing_state (actor);

  for (i = 0; properties != NULL && properties[i] != NULL; i++)
    {
      ClutterTransition *transition =
        clutter_actor_get_transition (actor, properties[i]);

      if (transition == NULL)
        continue;

      clutter_timeline_set_repeat_count (CLUTTER_TIMELINE (transition),
                                         params->repeat_count);
      clutter_timeline_set_auto_reverse (CLUTTER_TIMELINE (transition),
                                         params->auto_reverse);

      if (first == NULL)
        first = transition;
    }

  if (first != NULL)
    {
      ease_state_watch_transition (state, actor, first);
    }
  else
    {
      ease_state_prepare (state);
      ease_state_complete (state, TRUE);
    }
}

/**
 * shell_actor_ease: (skip)
 * @actor: a #ClutterActor
 * @params: (nullable): the easing parameters
 * @callback: (nullable): see shell_actor_ease_end()
 * @user_data: data for @callback
 * @destroy: called on @user_data after @callback
 * @first_property: name of the first property to animate
 * @...: value of @first_property, then further name/value pairs, terminated
 *   by %NULL
 *
 * C counterpart of Clutter.Actor.ease(): shell_actor_ease_begin(), set the
 * given properties, shell_actor_ease_end().
 */
void
shell_actor_ease (ClutterActor          *actor,
                  const ShellEaseParams *params,
                  ShellEaseCallback      callback,
                  gpointer               user_data,
                  GDestroyNotify         destroy,
                  const char            *first_property,
                  ...)
{
  g_autoptr (GPtrArray) names = g_ptr_array_new ();
  g_autoptr (GArray) values = g_array_new (FALSE, TRUE, sizeof (GValue));
  const char *name = first_property;
  va_list var_args;
  guint i;

  g_return_if_fail (CLUTTER_IS_ACTOR (actor));

  g_array_set_clear_func (values, (GDestroyNotify) g_value_unset);

  va_start (var_args, first_property);
  while (name != NULL)
    {
      g_autofree char *collect_error = NULL;
      GParamSpec *pspec;
      GValue value = G_VALUE_INIT;

      pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (actor), name);
      if (pspec == NULL)
        {
          g_warning ("%s: actor class '%s' has no property named '%s'",
                     G_STRFUNC, G_OBJECT_TYPE_NAME (actor), name);
          break;
        }

      G_VALUE_COLLECT_INIT (&value, pspec->value_type, var_args, 0,
                            &collect_error);
      if (collect_error != NULL)
        {
          g_warning ("%s: %s", G_STRFUNC, collect_error);
          g_value_unset (&value);
          break;
        }

      g_ptr_array_add (names, (gpointer) pspec->name);
      g_array_append_val (values, value);

      name = va_arg (var_args, const char *);
    }
  va_end (var_args);

  g_ptr_array_add (names, NULL);

  if (shell_actor_ease_begin (actor, params, (const char * const *) names->pdata))
    {
      g_object_freeze_notify (G_OBJECT (actor));
      for (i = 0; i < values->len; i++)
        {
          g_object_set_property (G_OBJECT (actor),
                                 g_ptr_array_index (names, i),
                                 &g_array_index (values, GValue, i));
        }
      g_object_thaw_notify (G_OBJECT (actor));
    }

  shell_actor_ease_end (actor, params, (const char * const *) names->pdata,
                        callback, user_data, destroy);
}

/* ------------------------------------------------------------------------ */

static gboolean
animatable_is_supported (ClutterAnimatable *animatable)
{
  return CLUTTER_IS_ACTOR (animatable) || ST_IS_ADJUSTMENT (animatable);
}

static void
animatable_remove_transition (ClutterAnimatable *animatable,
                              const char        *name)
{
  /* ClutterAnimatable has no transition API of its own, but the two
   * implementors we animate through ease_property() have the same one */
  if (CLUTTER_IS_ACTOR (animatable))
    clutter_actor_remove_transition (CLUTTER_ACTOR (animatable), name);
  else
    st_adjustment_remove_transition (ST_ADJUSTMENT (animatable), name);
}

static void
animatable_add_transition (ClutterAnimatable *animatable,
                           const char        *name,
                           ClutterTransition *transition)
{
  if (CLUTTER_IS_ACTOR (animatable))
    clutter_actor_add_transition (CLUTTER_ACTOR (animatable), name, transition);
  else
    st_adjustment_add_transition (ST_ADJUSTMENT (animatable), name, transition);
}

/* Resolves the "@layout.prop", "@actions.name.prop", "@constraints.name.prop",
 * "@content.prop" and "@effects.name.prop" forms of ClutterActor's animatable
 * property names to the object holding the property.
 */
static GObject *
get_property_target (ClutterAnimatable  *animatable,
                     const char         *property,
                     char              **prop_name,
                     GError            **error)
{
  g_auto (GStrv) parts = NULL;
  ClutterActor *actor;
  GObject *object = NULL;
  const char *type;
  guint n_parts;

  if (property[0] != '@')
    {
      *prop_name = g_strdup (property);
      return G_OBJECT (animatable);
    }

  if (!CLUTTER_IS_ACTOR (animatable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Invalid property name %s", property);
      return NULL;
    }

  actor = CLUTTER_ACTOR (animatable);
  parts = g_strsplit (property, ".", -1);
  n_parts = g_strv_length (parts);
  type = parts[0];

  if (g_str_equal (type, "@layout") && n_parts >= 2)
    {
      object = G_OBJECT (clutter_actor_get_layout_manager (actor));
      *prop_name = g_strdup (parts[1]);
    }
  else if (g_str_equal (type, "@actions") && n_parts >= 3)
    {
      object = G_OBJECT (clutter_actor_get_action (actor, parts[1]));
      *prop_name = g_strdup (parts[2]);
    }
  else if (g_str_equal (type, "@constraints") && n_parts >= 3)
    {
      object = G_OBJECT (clutter_actor_get_constraint (actor, parts[1]));
      *prop_name = g_strdup (parts[2]);
    }
  else if (g_str_equal (type, "@content") && n_parts >= 2)
    {
      object = G_OBJECT (clutter_actor_get_content (actor));
      *prop_name = g_strdup (parts[1]);
    }
  else if (g_str_equal (type, "@effects") && n_parts >= 3)
    {
      object = G_OBJECT (clutter_actor_get_effect (actor, parts[1]));
      *prop_name = g_strdup (parts[2]);
    }
  else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Invalid property name %s", property);
      return NULL;
    }

  if (object == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No target found for property name %s", property);
      g_clear_pointer (prop_name, g_free);
      return NULL;
    }

  return object;
}

/* Initializes @out as a @type holding @target, converting it as GObject
 * would for a property of that type. */
static gboolean
convert_target (const GValue  *target,
                GType          type,
                GValue        *out,
                GError       **error)
{
  g_value_init (out, type);

  if (G_TYPE_IS_ENUM (type) &&
      (G_VALUE_HOLDS_INT (target) || G_VALUE_HOLDS_UINT (target)))
    {
      g_value_set_enum (out, G_VALUE_HOLDS_INT (target)
                        ? g_value_get_int (target)
                        : (int) g_value_get_uint (target));
      return TRUE;
    }

  if (G_TYPE_IS_FLAGS (type) &&
      (G_VALUE_HOLDS_INT (target) || G_VALUE_HOLDS_UINT (target)))
    {
      g_value_set_flags (out, G_VALUE_HOLDS_INT (target)
                         ? (guint) g_value_get_int (target)
                         : g_value_get_uint (target));
      return TRUE;
    }

  if (!g_value_transform (target, out))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Cannot convert a %s to a %s",
                   G_VALUE_TYPE_NAME (target), g_type_name (type));
      g_value_unset (out);
      return FALSE;
    }

  return TRUE;
}

static void
drop_callback (GDestroyNotify destroy,
               gpointer       user_data)
{
  if (destroy)
    destroy (user_data);
}

/**
 * shell_animatable_ease_property:
 * @animatable: a #ClutterAnimatable, an actor or a #StAdjustment
 * @property: the property to animate. For actors this also accepts
 *   `@layout.prop`, `@actions.name.prop`, `@constraints.name.prop`,
 *   `@content.prop` and `@effects.name.prop`
 * @target: the final value, converted to the type of the property
 * @params: (nullable): the easing parameters. A @duration <= 0 means no
 *   animation; the property is set at once
 * @callback: (scope async) (nullable) (closure user_data) (destroy destroy):
 *   called once the animation ends, or at once if there is none
 * @user_data: data for @callback
 * @destroy: called on @user_data after @callback, or when the request is
 *   dropped
 * @error: return location for a #GError
 *
 * Implements Clutter.Actor.ease_property() and St.Adjustment.ease(). Any
 * transition running on @property is removed first, which reports it as
 * stopped.
 *
 * If this fails @callback is not called, but @destroy is.
 *
 * Returns: %TRUE on success, %FALSE with @error set if @property or @target
 *   are invalid
 */
gboolean
shell_animatable_ease_property (ClutterAnimatable      *animatable,
                                const char             *property,
                                const GValue           *target,
                                const ShellEaseParams  *params,
                                ShellEaseCallback       callback,
                                gpointer                user_data,
                                GDestroyNotify          destroy,
                                GError                **error)
{
  g_autoptr (ClutterTransition) transition = NULL;
  g_autoptr (ClutterInterval) interval = NULL;
  g_auto (GValue) final_value = G_VALUE_INIT;
  g_autofree char *prop_name = NULL;
  ClutterActor *actor;
  GParamSpec *pspec;
  EaseState *state;
  GObject *object;
  guint duration = 0;
  guint delay = 0;

  g_return_val_if_fail (CLUTTER_IS_ANIMATABLE (animatable), FALSE);
  g_return_val_if_fail (property != NULL, FALSE);
  g_return_val_if_fail (target != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!animatable_is_supported (animatable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "%s cannot be eased", G_OBJECT_TYPE_NAME (animatable));
      drop_callback (destroy, user_data);
      return FALSE;
    }

  if (params == NULL)
    params = &default_params;

  if (params->duration > 0)
    {
      duration = shell_adjust_animation_time (params->duration,
                                              params->animation_required);
    }

  if (params->delay > 0)
    {
      delay = shell_adjust_animation_time (params->delay,
                                           params->animation_required);
    }

  /* The object is a Clutter.Animatable. */
  actor = clutter_animatable_get_actor (animatable);

  /* Copy Clutter's behavior for implicit animations, see
   * should_skip_implicit_transition() */
  if (actor == NULL || !clutter_actor_is_mapped (actor))
    duration = 0;

  /* cancel overwritten transition */
  animatable_remove_transition (animatable, property);

  state = ease_state_new (callback, user_data, destroy, duration > 0);

  if (duration == 0)
    {
      object = get_property_target (animatable, property, &prop_name, error);
      if (object == NULL)
        goto fail;

      pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (object), prop_name);
      if (pspec == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       "%s has no property named '%s'",
                       G_OBJECT_TYPE_NAME (object), prop_name);
          goto fail;
        }

      if (!convert_target (target, pspec->value_type, &final_value, error))
        goto fail;

      if (!params_is_reversed (params))
        g_object_set_property (object, prop_name, &final_value);

      ease_state_complete (state, TRUE);
      return TRUE;
    }

  pspec = clutter_animatable_find_property (animatable, property);
  if (pspec == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "%s has no property named '%s'",
                   G_OBJECT_TYPE_NAME (animatable), property);
      goto fail;
    }

  if (!convert_target (target, pspec->value_type, &final_value, error))
    goto fail;

  interval = g_object_ref_sink (g_object_new (CLUTTER_TYPE_INTERVAL,
                                              "value-type", pspec->value_type,
                                              NULL));

  transition = g_object_new (CLUTTER_TYPE_PROPERTY_TRANSITION,
                             "property-name", property,
                             "interval", interval,
                             "remove-on-complete", TRUE,
                             "repeat-count", params->repeat_count,
                             "auto-reverse", params->auto_reverse,
                             "duration", duration,
                             NULL);
  if (delay > 0)
    g_object_set (transition, "delay", delay, NULL);
  if (params->mode != 0)
    g_object_set (transition, "progress-mode", params->mode, NULL);

  animatable_add_transition (animatable, property, transition);

  clutter_transition_set_to_value (transition, &final_value);

  ease_state_watch_transition (state, actor, transition);

  return TRUE;

fail:
  state->callback = NULL;
  ease_state_free (state);
  drop_callback (destroy, user_data);
  return FALSE;
}
