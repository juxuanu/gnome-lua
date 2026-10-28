/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * ripples.c: Three concentric ripples expanding outwards, used by the hot
 * corner and the pointer locator. Port of js/ui/ripples.js.
 *
 * Shamelessly copied from the layout "hotcorner" ripples implementation.
 */

#include "config.h"

#include <math.h>

#include "ui/environment.h"
#include "ui/ripples.h"

#define N_RIPPLES 3

enum
{
  PROP_0,
  PROP_PX,
  PROP_PY,
  PROP_STYLE_CLASS,
  PROP_ANIMATION_REQUIRED,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellRipples
{
  GObject parent;

  double px;
  double py;
  char *style_class;
  gboolean animation_required;

  double x;
  double y;

  ClutterActor *ripples[N_RIPPLES];

  ClutterActor *stage;
  gboolean added;
};

G_DEFINE_FINAL_TYPE (ShellRipples, shell_ripples, G_TYPE_OBJECT)

static void
ripple_hide_cb (gboolean finished,
                gpointer user_data)
{
  /* onComplete: only when the animation ran to its end */
  if (finished)
    clutter_actor_hide (CLUTTER_ACTOR (user_data));
}

static void
anim_ripple (ShellRipples *self,
             ClutterActor *ripple,
             int           delay,
             int           duration,
             double        start_scale,
             double        start_opacity,
             double        final_scale)
{
  ShellEaseParams params = {
    .duration = duration,
    .delay = delay,
    .mode = CLUTTER_EASE_IN_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = self->animation_required,
  };

  /* We draw a ripple by using a source image and animating it scaling
   * outwards and fading away. We want the ripples to move linearly
   * or it looks unrealistic, but if the opacity of the ripple goes
   * linearly to zero it fades away too quickly, so we use a separate
   * tween to give a non-linear curve to the fade-away and make
   * it more visible in the middle section.
   */

  clutter_actor_set_position (ripple, self->x, self->y);
  clutter_actor_show (ripple);
  clutter_actor_set_opacity (ripple, (guint8) (255 * sqrt (start_opacity)));
  clutter_actor_set_scale (ripple, start_scale, start_scale);
  clutter_actor_set_translation (ripple,
                                 -self->px * clutter_actor_get_width (ripple),
                                 -self->py * clutter_actor_get_height (ripple),
                                 0.0);

  shell_actor_ease (ripple, &params, NULL, NULL, NULL,
                    "opacity", 0,
                    NULL);

  params.mode = CLUTTER_EASE_OUT_QUAD;
  shell_actor_ease (ripple, &params,
                    ripple_hide_cb, g_object_ref (ripple), g_object_unref,
                    "scale-x", final_scale,
                    "scale-y", final_scale,
                    NULL);
}

/**
 * shell_ripples_add_to:
 * @self: a #ShellRipples
 * @stage: the actor to add the ripples to
 *
 * Adds the three ripple actors as children of @stage. This can only be done
 * once.
 */
void
shell_ripples_add_to (ShellRipples *self,
                      ClutterActor *stage)
{
  int i;

  g_return_if_fail (SHELL_IS_RIPPLES (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (stage));
  g_return_if_fail (self->ripples[0] != NULL);
  g_return_if_fail (!self->added);

  self->added = TRUE;
  g_set_weak_pointer (&self->stage, stage);

  for (i = 0; i < N_RIPPLES; i++)
    clutter_actor_add_child (stage, self->ripples[i]);
}

/**
 * shell_ripples_play_animation:
 * @self: a #ShellRipples
 * @x: the x coordinate of the ripples' origin, in the stage's coordinates
 * @y: the y coordinate of the ripples' origin, in the stage's coordinates
 *
 * Shows three concentric ripples expanding outwards from (@x, @y). The
 * ripples must have been added with shell_ripples_add_to() first.
 */
void
shell_ripples_play_animation (ShellRipples *self,
                              double        x,
                              double        y)
{
  g_return_if_fail (SHELL_IS_RIPPLES (self));
  g_return_if_fail (self->added);

  /* The stage was destroyed, and took the ripples with it */
  if (self->stage == NULL || self->ripples[0] == NULL)
    return;

  self->x = x;
  self->y = y;

  clutter_actor_set_child_above_sibling (self->stage, self->ripples[0], NULL);
  clutter_actor_set_child_above_sibling (self->stage, self->ripples[1],
                                         self->ripples[0]);
  clutter_actor_set_child_above_sibling (self->stage, self->ripples[2],
                                         self->ripples[1]);

  /* Show three concentric ripples expanding outwards; the exact
   * parameters were found by trial and error, so don't look
   * for them to make perfect sense mathematically
   *
   *                                    delay  time   scale opacity => scale
   */
  anim_ripple (self, self->ripples[0],   0,    830,   0.25,  1.0,     1.5);
  anim_ripple (self, self->ripples[1],  50,   1000,   0.0,   0.7,     1.25);
  anim_ripple (self, self->ripples[2], 350,   1000,   0.0,   0.3,     1);
}

/**
 * shell_ripples_destroy:
 * @self: a #ShellRipples
 *
 * Destroys the ripple actors. The #ShellRipples can't be used afterwards.
 */
void
shell_ripples_destroy (ShellRipples *self)
{
  int i;

  g_return_if_fail (SHELL_IS_RIPPLES (self));

  for (i = 0; i < N_RIPPLES; i++)
    {
      if (self->ripples[i] != NULL)
        clutter_actor_destroy (self->ripples[i]);
      g_clear_object (&self->ripples[i]);
    }
}

static void
shell_ripples_constructed (GObject *object)
{
  ShellRipples *self = SHELL_RIPPLES (object);
  int i;

  G_OBJECT_CLASS (shell_ripples_parent_class)->constructed (object);

  for (i = 0; i < N_RIPPLES; i++)
    {
      ClutterActor *ripple;

      ripple = g_object_new (ST_TYPE_BOX_LAYOUT,
                             "style-class", self->style_class,
                             "opacity", 0,
                             "can-focus", FALSE,
                             "reactive", FALSE,
                             "visible", FALSE,
                             NULL);
      clutter_actor_set_pivot_point (ripple, self->px, self->py);

      /* Keep our own reference, the stage only owns the actors once they
       * have been added to it */
      self->ripples[i] = g_object_ref_sink (ripple);
    }
}

static void
shell_ripples_dispose (GObject *object)
{
  ShellRipples *self = SHELL_RIPPLES (object);

  shell_ripples_destroy (self);
  g_clear_weak_pointer (&self->stage);

  G_OBJECT_CLASS (shell_ripples_parent_class)->dispose (object);
}

static void
shell_ripples_finalize (GObject *object)
{
  ShellRipples *self = SHELL_RIPPLES (object);

  g_free (self->style_class);

  G_OBJECT_CLASS (shell_ripples_parent_class)->finalize (object);
}

static void
shell_ripples_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  ShellRipples *self = SHELL_RIPPLES (object);

  switch (prop_id)
    {
    case PROP_PX:
      g_value_set_double (value, self->px);
      break;
    case PROP_PY:
      g_value_set_double (value, self->py);
      break;
    case PROP_STYLE_CLASS:
      g_value_set_string (value, self->style_class);
      break;
    case PROP_ANIMATION_REQUIRED:
      g_value_set_boolean (value, self->animation_required);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_ripples_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  ShellRipples *self = SHELL_RIPPLES (object);

  switch (prop_id)
    {
    case PROP_PX:
      self->px = g_value_get_double (value);
      break;
    case PROP_PY:
      self->py = g_value_get_double (value);
      break;
    case PROP_STYLE_CLASS:
      g_free (self->style_class);
      self->style_class = g_value_dup_string (value);
      break;
    case PROP_ANIMATION_REQUIRED:
      self->animation_required = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_ripples_class_init (ShellRipplesClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_ripples_constructed;
  object_class->dispose = shell_ripples_dispose;
  object_class->finalize = shell_ripples_finalize;
  object_class->get_property = shell_ripples_get_property;
  object_class->set_property = shell_ripples_set_property;

  /**
   * ShellRipples:px:
   *
   * The horizontal pivot of the ripples, as a fraction of their width. The
   * ripples are positioned so that this point is at the origin given to
   * shell_ripples_play_animation().
   */
  props[PROP_PX] =
    g_param_spec_double ("px", NULL, NULL,
                         -G_MAXDOUBLE, G_MAXDOUBLE, 0,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellRipples:py:
   *
   * The vertical pivot of the ripples, as a fraction of their height.
   */
  props[PROP_PY] =
    g_param_spec_double ("py", NULL, NULL,
                         -G_MAXDOUBLE, G_MAXDOUBLE, 0,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellRipples:style-class:
   *
   * The CSS style class of the ripple actors.
   */
  props[PROP_STYLE_CLASS] =
    g_param_spec_string ("style-class", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellRipples:animation-required:
   *
   * Whether the ripples are animated even when animations are disabled in
   * the settings.
   */
  props[PROP_ANIMATION_REQUIRED] =
    g_param_spec_boolean ("animation-required", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_ripples_init (ShellRipples *self)
{
}

/**
 * shell_ripples_new:
 * @px: horizontal pivot of the ripples, as a fraction of their width
 * @py: vertical pivot of the ripples, as a fraction of their height
 * @style_class: the CSS style class of the ripple actors
 * @animation_required: whether to animate even when animations are disabled
 *
 * Creates three hidden ripple actors. Add them to a stage with
 * shell_ripples_add_to().
 *
 * Returns: (transfer full): a new #ShellRipples
 */
ShellRipples *
shell_ripples_new (double      px,
                   double      py,
                   const char *style_class,
                   gboolean    animation_required)
{
  return g_object_new (SHELL_TYPE_RIPPLES,
                       "px", px,
                       "py", py,
                       "style-class", style_class,
                       "animation-required", animation_required,
                       NULL);
}
