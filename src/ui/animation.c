/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * animation.c: A spinner that fades in and out. Port of js/ui/animation.js.
 */

#include "config.h"

#include "ui/animation.h"
#include "ui/environment.h"
#include "shell-global.h"

#define SPINNER_ANIMATION_DELAY 1000

enum
{
  PROP_0,
  PROP_SPINNER_SIZE,
  PROP_ANIMATE,
  PROP_HIDE_ON_STOP,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellSpinner
{
  StWidget parent;

  int size;
  gboolean animate;
  gboolean hide_on_stop;
};

G_DEFINE_FINAL_TYPE (ShellSpinner, shell_spinner, ST_TYPE_WIDGET)

static gboolean
is_playing (ShellSpinner *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  return clutter_actor_is_visible (actor) &&
         clutter_actor_get_content (actor) != NULL;
}

static void
stop_done_cb (gboolean finished,
              gpointer user_data)
{
  ShellSpinner *self = user_data;

  /* onComplete: only when the fade-out ran to its end */
  if (!finished)
    return;

  clutter_actor_set_content (CLUTTER_ACTOR (self), NULL);
  if (self->hide_on_stop)
    clutter_actor_hide (CLUTTER_ACTOR (self));
}

/**
 * shell_spinner_play:
 * @self: a #ShellSpinner
 *
 * Shows the spinner and starts spinning. If #ShellSpinner:animate is set the
 * spinner fades in after a delay, so that quick operations don't make it
 * flash.
 */
void
shell_spinner_play (ShellSpinner *self)
{
  g_autoptr (ClutterContent) content = NULL;
  ClutterActor *actor;

  g_return_if_fail (SHELL_IS_SPINNER (self));

  if (is_playing (self))
    return;

  actor = CLUTTER_ACTOR (self);

  clutter_actor_remove_all_transitions (actor);
  content = st_spinner_content_new ();
  clutter_actor_set_content (actor, content);
  clutter_actor_show (actor);

  if (self->animate)
    {
      ShellEaseParams params = {
        .duration = SHELL_SPINNER_ANIMATION_TIME,
        .delay = SPINNER_ANIMATION_DELAY,
        .mode = CLUTTER_LINEAR,
      };

      shell_actor_ease (actor, &params, NULL, NULL, NULL,
                        "opacity", 255,
                        NULL);
    }
  else
    {
      clutter_actor_set_opacity (actor, 255);
    }
}

/**
 * shell_spinner_stop:
 * @self: a #ShellSpinner
 *
 * Stops spinning, fading out first if #ShellSpinner:animate is set. The
 * spinner is hidden afterwards if #ShellSpinner:hide-on-stop is set.
 */
void
shell_spinner_stop (ShellSpinner *self)
{
  ClutterActor *actor;

  g_return_if_fail (SHELL_IS_SPINNER (self));

  if (!is_playing (self))
    return;

  actor = CLUTTER_ACTOR (self);

  clutter_actor_remove_all_transitions (actor);

  if (self->animate)
    {
      ShellEaseParams params = {
        .duration = SHELL_SPINNER_ANIMATION_TIME,
        .delay = -1,
        .mode = CLUTTER_LINEAR,
      };

      shell_actor_ease (actor, &params, stop_done_cb, self, NULL,
                        "opacity", 0,
                        NULL);
    }
  else
    {
      clutter_actor_set_opacity (actor, 0);
      clutter_actor_set_content (actor, NULL);

      if (self->hide_on_stop)
        clutter_actor_hide (actor);
    }
}

static void
shell_spinner_map (ClutterActor *actor)
{
  ShellSpinner *self = SHELL_SPINNER (actor);
  StThemeContext *context;
  int logical_size;

  context = st_theme_context_get_for_stage (shell_global_get_stage (shell_global_get ()));
  logical_size = self->size * st_theme_context_get_scale_factor (context);
  clutter_actor_set_size (actor, logical_size, logical_size);

  CLUTTER_ACTOR_CLASS (shell_spinner_parent_class)->map (actor);
}

static void
shell_spinner_constructed (GObject *object)
{
  ShellSpinner *self = SHELL_SPINNER (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);

  G_OBJECT_CLASS (shell_spinner_parent_class)->constructed (object);

  clutter_actor_set_opacity (actor, 0);
  if (self->hide_on_stop)
    clutter_actor_hide (actor);
  else
    clutter_actor_show (actor);
}

static void
shell_spinner_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  ShellSpinner *self = SHELL_SPINNER (object);

  switch (prop_id)
    {
    case PROP_SPINNER_SIZE:
      g_value_set_int (value, self->size);
      break;
    case PROP_ANIMATE:
      g_value_set_boolean (value, self->animate);
      break;
    case PROP_HIDE_ON_STOP:
      g_value_set_boolean (value, self->hide_on_stop);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_spinner_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  ShellSpinner *self = SHELL_SPINNER (object);

  switch (prop_id)
    {
    case PROP_SPINNER_SIZE:
      self->size = g_value_get_int (value);
      break;
    case PROP_ANIMATE:
      self->animate = g_value_get_boolean (value);
      break;
    case PROP_HIDE_ON_STOP:
      self->hide_on_stop = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_spinner_class_init (ShellSpinnerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_spinner_constructed;
  object_class->get_property = shell_spinner_get_property;
  object_class->set_property = shell_spinner_set_property;

  actor_class->map = shell_spinner_map;

  /**
   * ShellSpinner:spinner-size:
   *
   * The size of the spinner, in logical pixels before the scale factor of
   * the theme context is applied. Named this way because #ClutterActor
   * already has a "size" property.
   */
  props[PROP_SPINNER_SIZE] =
    g_param_spec_int ("spinner-size", NULL, NULL,
                      0, G_MAXINT, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellSpinner:animate:
   *
   * Whether the spinner fades in and out when played and stopped.
   */
  props[PROP_ANIMATE] =
    g_param_spec_boolean ("animate", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellSpinner:hide-on-stop:
   *
   * Whether the spinner is hidden, rather than just made transparent, while
   * it isn't playing. It starts out hidden if set.
   */
  props[PROP_HIDE_ON_STOP] =
    g_param_spec_boolean ("hide-on-stop", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_spinner_init (ShellSpinner *self)
{
}

/**
 * shell_spinner_new:
 * @size: the size of the spinner, before applying the scale factor
 * @animate: whether to fade the spinner in and out
 * @hide_on_stop: whether to hide the spinner when it is stopped
 *
 * Creates a new spinner. It is transparent, and hidden if @hide_on_stop is
 * set, until shell_spinner_play() is called.
 *
 * Returns: (transfer full): a new #ShellSpinner
 */
ShellSpinner *
shell_spinner_new (int      size,
                   gboolean animate,
                   gboolean hide_on_stop)
{
  return g_object_new (SHELL_TYPE_SPINNER,
                       "spinner-size", size,
                       "animate", animate,
                       "hide-on-stop", hide_on_stop,
                       NULL);
}
