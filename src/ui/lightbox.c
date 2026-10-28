/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * lightbox.c: A dark translucent "shade" covering the contents of a
 * container, with optional highlighted children. Port of js/ui/lightbox.js.
 */

#include "config.h"

#include <cogl/cogl.h>

#include "ui/environment.h"
#include "ui/lightbox.h"

#define DEFAULT_FADE_FACTOR 0.4
#define VIGNETTE_BRIGHTNESS 0.5
#define VIGNETTE_SHARPNESS 0.7

static const char vignette_declarations[] =
  "uniform float brightness;\n"
  "uniform float vignette_sharpness;\n"
  "float rand(vec2 p) {\n"
  "  return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);\n"
  "}\n";

static const char vignette_code[] =
  "cogl_color_out.a = cogl_color_in.a;\n"
  "cogl_color_out.rgb = vec3(0.0, 0.0, 0.0);\n"
  "vec2 position = cogl_tex_coord_in[0].xy - 0.5;\n"
  "float t = clamp(length(1.41421 * position), 0.0, 1.0);\n"
  "float pixel_brightness = mix(1.0, 1.0 - vignette_sharpness, t);\n"
  "cogl_color_out.a *= 1.0 - pixel_brightness * brightness;\n"
  "float noise = 2.0 * rand(position + 0.5) - 1.0;\n"
  "noise = sign(noise) * (1.0 - sqrt(1.0 - abs(noise)));\n"
  "cogl_color_out.a += noise / ((1.0 - cogl_color_out.a) * 255.0);\n";

/* ------------------------------------------------------------------------ */
/* ShellRadialShaderEffect: the vignette used when radial-effect is set.     */
/* Private to this file. The JS version was a Clutter.ShaderEffect subclass. */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_RADIAL_SHADER_EFFECT (shell_radial_shader_effect_get_type ())
G_DECLARE_FINAL_TYPE (ShellRadialShaderEffect, shell_radial_shader_effect,
                      SHELL, RADIAL_SHADER_EFFECT, ClutterShaderEffect)

enum
{
  EFFECT_PROP_0,
  EFFECT_PROP_BRIGHTNESS,
  EFFECT_PROP_SHARPNESS,
  N_EFFECT_PROPS
};

static GParamSpec *effect_props[N_EFFECT_PROPS] = { NULL, };

struct _ShellRadialShaderEffect
{
  ClutterShaderEffect parent;

  float brightness;
  float sharpness;
};

G_DEFINE_FINAL_TYPE (ShellRadialShaderEffect, shell_radial_shader_effect,
                     CLUTTER_TYPE_SHADER_EFFECT)

static CoglSnippet *
shell_radial_shader_effect_get_static_snippet (ClutterShaderEffect *effect)
{
  CoglSnippet *snippet;

  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
                              vignette_declarations, NULL);
  cogl_snippet_set_replace (snippet, vignette_code);

  return snippet;
}

static void
shell_radial_shader_effect_set_brightness (ShellRadialShaderEffect *self,
                                           float                    brightness)
{
  if (self->brightness == brightness)
    return;

  self->brightness = brightness;
  clutter_shader_effect_set_uniform (CLUTTER_SHADER_EFFECT (self),
                                     "brightness", G_TYPE_FLOAT, 1,
                                     (double) self->brightness);
  g_object_notify_by_pspec (G_OBJECT (self), effect_props[EFFECT_PROP_BRIGHTNESS]);
}

static void
shell_radial_shader_effect_set_sharpness (ShellRadialShaderEffect *self,
                                          float                    sharpness)
{
  if (self->sharpness == sharpness)
    return;

  self->sharpness = sharpness;
  clutter_shader_effect_set_uniform (CLUTTER_SHADER_EFFECT (self),
                                     "vignette_sharpness", G_TYPE_FLOAT, 1,
                                     (double) self->sharpness);
  g_object_notify_by_pspec (G_OBJECT (self), effect_props[EFFECT_PROP_SHARPNESS]);
}

static void
shell_radial_shader_effect_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  ShellRadialShaderEffect *self = SHELL_RADIAL_SHADER_EFFECT (object);

  switch (prop_id)
    {
    case EFFECT_PROP_BRIGHTNESS:
      g_value_set_float (value, self->brightness);
      break;

    case EFFECT_PROP_SHARPNESS:
      g_value_set_float (value, self->sharpness);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_radial_shader_effect_set_property (GObject      *object,
                                         guint         prop_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
  ShellRadialShaderEffect *self = SHELL_RADIAL_SHADER_EFFECT (object);

  switch (prop_id)
    {
    case EFFECT_PROP_BRIGHTNESS:
      shell_radial_shader_effect_set_brightness (self, g_value_get_float (value));
      break;

    case EFFECT_PROP_SHARPNESS:
      shell_radial_shader_effect_set_sharpness (self, g_value_get_float (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_radial_shader_effect_class_init (ShellRadialShaderEffectClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterShaderEffectClass *shader_class = CLUTTER_SHADER_EFFECT_CLASS (klass);

  object_class->get_property = shell_radial_shader_effect_get_property;
  object_class->set_property = shell_radial_shader_effect_set_property;

  shader_class->get_static_snippet = shell_radial_shader_effect_get_static_snippet;

  effect_props[EFFECT_PROP_BRIGHTNESS] =
    g_param_spec_float ("brightness", NULL, NULL,
                        0.0, 1.0, 1.0,
                        G_PARAM_READWRITE |
                        G_PARAM_EXPLICIT_NOTIFY |
                        G_PARAM_STATIC_STRINGS);

  effect_props[EFFECT_PROP_SHARPNESS] =
    g_param_spec_float ("sharpness", NULL, NULL,
                        0.0, 1.0, 0.0,
                        G_PARAM_READWRITE |
                        G_PARAM_EXPLICIT_NOTIFY |
                        G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_EFFECT_PROPS, effect_props);
}

static void
shell_radial_shader_effect_init (ShellRadialShaderEffect *self)
{
  /* The uniforms start out undefined, like the JS _brightness/_sharpness,
   * so that these assignments always reach the shader. */
  self->brightness = -1.0;
  self->sharpness = -1.0;

  shell_radial_shader_effect_set_brightness (self, 1.0);
  shell_radial_shader_effect_set_sharpness (self, 0.0);
}

/* ------------------------------------------------------------------------ */
/* ShellLightbox                                                             */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,
  PROP_CONTAINER,
  PROP_INHIBIT_EVENTS,
  PROP_FADE_FACTOR,
  PROP_RADIAL_EFFECT,
  PROP_ACTIVE,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

typedef struct
{
  ClutterActor *container;      /* weak */
  GPtrArray *children;          /* ClutterActor *, unowned, bottom to top,
                                   without the lightbox itself */
  ClutterActor *highlighted;    /* weak */
  ClutterConstraint *size_constraint; /* weak, owned by the actor */

  double fade_factor;
  gboolean inhibit_events;
  gboolean radial_effect;
  gboolean active;
} ShellLightboxPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellLightbox, shell_lightbox, ST_TYPE_BIN)

static void
child_added_cb (ClutterActor  *container,
                ClutterActor  *new_child,
                ShellLightbox *self)
{
  ShellLightboxPrivate *priv = shell_lightbox_get_instance_private (self);
  g_autoptr (GList) children = clutter_actor_get_children (container);
  int my_index = g_list_index (children, self);
  int new_child_index = g_list_index (children, new_child);

  if (my_index < 0) /* the shade was taken out of the container */
    return;

  if (new_child_index > my_index)
    {
      /* The child was added above the shade (presumably it was made the new
       * top-most child). Move it below the shade, and add it to
       * priv->children as the new topmost actor.
       */
      clutter_actor_set_child_above_sibling (container,
                                             CLUTTER_ACTOR (self), new_child);
      g_ptr_array_add (priv->children, new_child);
    }
  else if (new_child_index == 0)
    {
      /* Bottom of stack */
      g_ptr_array_insert (priv->children, 0, new_child);
    }
  else
    {
      /* Somewhere else; insert it into the correct spot */
      ClutterActor *sibling = g_list_nth_data (children, new_child_index - 1);
      unsigned int prev_child;

      if (g_ptr_array_find (priv->children, sibling, &prev_child)) /* paranoia */
        g_ptr_array_insert (priv->children, prev_child + 1, new_child);
    }
}

static void
child_removed_cb (ClutterActor  *container,
                  ClutterActor  *child,
                  ShellLightbox *self)
{
  ShellLightboxPrivate *priv = shell_lightbox_get_instance_private (self);
  unsigned int index;

  if (g_ptr_array_find (priv->children, child, &index)) /* paranoia */
    g_ptr_array_remove_index (priv->children, index);

  if (child == priv->highlighted)
    g_clear_weak_pointer (&priv->highlighted);
}

/* This is called when the lightbox actor is destroyed, either by destroying
 * its container or by explicitly calling clutter_actor_destroy(). */
static void
lightbox_destroy_cb (ClutterActor *actor,
                     gpointer      user_data)
{
  shell_lightbox_highlight (SHELL_LIGHTBOX (actor), NULL);
}

static void
light_on_complete_cb (gboolean finished,
                      gpointer user_data)
{
  ShellLightbox *self = user_data;
  ShellLightboxPrivate *priv = shell_lightbox_get_instance_private (self);

  /* onComplete: only when the animation ran to its end */
  if (!finished)
    return;

  priv->active = TRUE;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE]);
}

static void
light_off_complete_cb (gboolean finished,
                       gpointer user_data)
{
  if (finished)
    clutter_actor_hide (CLUTTER_ACTOR (user_data));
}

static void
ease_effect_property (ShellLightbox         *self,
                      const char            *property,
                      double                 value,
                      const ShellEaseParams *params,
                      ShellEaseCallback      callback)
{
  g_auto (GValue) target = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;

  g_value_init (&target, G_TYPE_DOUBLE);
  g_value_set_double (&target, value);

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (self), property,
                                       &target, params,
                                       callback, self, NULL, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

/**
 * shell_lightbox_light_on:
 * @self: a #ShellLightbox
 * @fade_in_time: the duration of the fade in milliseconds, 0 for none
 *
 * Shows the shade, fading it in. #ShellLightbox:active becomes %TRUE once the
 * fade has completed.
 */
void
shell_lightbox_light_on (ShellLightbox *self,
                         uint32_t       fade_in_time)
{
  ShellLightboxPrivate *priv;
  ShellEaseParams params = {
    .duration = MIN (fade_in_time, G_MAXINT),
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  g_return_if_fail (SHELL_IS_LIGHTBOX (self));

  priv = shell_lightbox_get_instance_private (self);

  clutter_actor_remove_all_transitions (CLUTTER_ACTOR (self));

  clutter_actor_show (CLUTTER_ACTOR (self));

  if (priv->radial_effect)
    {
      ease_effect_property (self, "@effects.radial.brightness",
                            VIGNETTE_BRIGHTNESS, &params, NULL);
      ease_effect_property (self, "@effects.radial.sharpness",
                            VIGNETTE_SHARPNESS, &params, light_on_complete_cb);
    }
  else
    {
      shell_actor_ease (CLUTTER_ACTOR (self), &params,
                        light_on_complete_cb, self, NULL,
                        "opacity", (guint) (255 * priv->fade_factor),
                        NULL);
    }
}

/**
 * shell_lightbox_light_off:
 * @self: a #ShellLightbox
 * @fade_out_time: the duration of the fade in milliseconds, 0 for none
 *
 * Fades the shade out and hides it. #ShellLightbox:active becomes %FALSE at
 * once.
 */
void
shell_lightbox_light_off (ShellLightbox *self,
                          uint32_t       fade_out_time)
{
  ShellLightboxPrivate *priv;
  ShellEaseParams params = {
    .duration = MIN (fade_out_time, G_MAXINT),
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  g_return_if_fail (SHELL_IS_LIGHTBOX (self));

  priv = shell_lightbox_get_instance_private (self);

  clutter_actor_remove_all_transitions (CLUTTER_ACTOR (self));

  priv->active = FALSE;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE]);

  if (priv->radial_effect)
    {
      ease_effect_property (self, "@effects.radial.brightness",
                            1.0, &params, NULL);
      ease_effect_property (self, "@effects.radial.sharpness",
                            0.0, &params, light_off_complete_cb);
    }
  else
    {
      shell_actor_ease (CLUTTER_ACTOR (self), &params,
                        light_off_complete_cb, self, NULL,
                        "opacity", 0u,
                        NULL);
    }
}

/**
 * shell_lightbox_highlight:
 * @self: a #ShellLightbox
 * @window: (nullable): the actor to highlight
 *
 * Highlights the indicated actor and unhighlights any other
 * currently-highlighted actor. With %NULL, all actors will be unhighlighted.
 */
void
shell_lightbox_highlight (ShellLightbox *self,
                          ClutterActor  *window)
{
  ShellLightboxPrivate *priv;
  ClutterActor *below;
  int i;

  g_return_if_fail (SHELL_IS_LIGHTBOX (self));
  g_return_if_fail (window == NULL || CLUTTER_IS_ACTOR (window));

  priv = shell_lightbox_get_instance_private (self);

  if (priv->highlighted == window)
    return;

  /* The container can be gone when this is called from the destroy handler
   * of a lightbox that was removed from it. */
  if (priv->container != NULL)
    {
      /* Walk priv->children raising and lowering actors as needed.
       * Things get a little tricky if the to-be-raised and
       * to-be-lowered actors were originally adjacent, in which
       * case we may need to indicate some *other* actor as the new
       * sibling of the to-be-lowered one.
       */
      below = CLUTTER_ACTOR (self);
      for (i = priv->children->len - 1; i >= 0; i--)
        {
          ClutterActor *child = g_ptr_array_index (priv->children, i);

          if (child == window)
            clutter_actor_set_child_above_sibling (priv->container, child, NULL);
          else if (child == priv->highlighted)
            clutter_actor_set_child_below_sibling (priv->container, child, below);
          else
            below = child;
        }
    }

  g_set_weak_pointer (&priv->highlighted, window);
}

/**
 * shell_lightbox_get_active:
 * @self: a #ShellLightbox
 *
 * Returns: whether the shade is fully lit
 */
gboolean
shell_lightbox_get_active (ShellLightbox *self)
{
  ShellLightboxPrivate *priv;

  g_return_val_if_fail (SHELL_IS_LIGHTBOX (self), FALSE);

  priv = shell_lightbox_get_instance_private (self);

  return priv->active;
}

static const char * const size_properties[] = {
  "width",
  "height",
  "natural-width",
  "natural-height",
  "natural-width-set",
  "natural-height-set",
};

/* By default the shade tracks the size of the container. An explicit width
 * and height, both non-zero, override this. */
static void
update_size_constraint (ShellLightbox *self)
{
  ShellLightboxPrivate *priv = shell_lightbox_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  gboolean width_set, height_set;
  float width, height;
  gboolean explicit_size;

  if (priv->container == NULL)
    return;

  g_object_get (self,
                "natural-width-set", &width_set,
                "natural-width", &width,
                "natural-height-set", &height_set,
                "natural-height", &height,
                NULL);
  explicit_size = width_set && width > 0 && height_set && height > 0;

  if (explicit_size && priv->size_constraint != NULL)
    {
      clutter_actor_remove_constraint (actor, priv->size_constraint);
      g_clear_weak_pointer (&priv->size_constraint);
    }
  else if (!explicit_size && priv->size_constraint == NULL)
    {
      ClutterConstraint *constraint =
        clutter_bind_constraint_new (priv->container, CLUTTER_BIND_ALL, 0);

      clutter_actor_add_constraint (actor, constraint);
      g_set_weak_pointer (&priv->size_constraint, constraint);
    }
}

static void
size_notify_cb (GObject    *object,
                GParamSpec *pspec,
                gpointer    user_data)
{
  update_size_constraint (SHELL_LIGHTBOX (object));
}

static void
shell_lightbox_constructed (GObject *object)
{
  ShellLightbox *self = SHELL_LIGHTBOX (object);
  ShellLightboxPrivate *priv = shell_lightbox_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  g_autoptr (GList) children = NULL;
  unsigned int i;

  G_OBJECT_CLASS (shell_lightbox_parent_class)->constructed (object);

  if (priv->container == NULL)
    {
      g_critical ("ShellLightbox needs a container");
      return;
    }

  clutter_actor_set_reactive (actor, priv->inhibit_events);
  clutter_actor_hide (actor);

  children = clutter_actor_get_children (priv->container);
  for (GList *l = children; l; l = l->next)
    g_ptr_array_add (priv->children, l->data);

  if (priv->radial_effect)
    {
      ClutterEffect *effect = g_object_new (SHELL_TYPE_RADIAL_SHADER_EFFECT,
                                            "name", "radial",
                                            NULL);

      clutter_actor_add_effect (actor, effect);
    }
  else
    {
      clutter_actor_set_opacity (actor, 0);
      st_widget_set_style_class_name (ST_WIDGET (self), "lightbox");
    }

  clutter_actor_add_child (priv->container, actor);
  clutter_actor_set_child_above_sibling (priv->container, actor, NULL);

  g_signal_connect (self, "destroy", G_CALLBACK (lightbox_destroy_cb), NULL);

  /* Properties that are not construct properties, such as the width and
   * height, are only set after constructed() returns: follow them. */
  for (i = 0; i < G_N_ELEMENTS (size_properties); i++)
    {
      g_autofree char *signal_name =
        g_strdup_printf ("notify::%s", size_properties[i]);

      g_signal_connect (self, signal_name, G_CALLBACK (size_notify_cb), NULL);
    }
  update_size_constraint (self);

  g_signal_connect_object (priv->container, "child-added",
                           G_CALLBACK (child_added_cb), self, 0);
  g_signal_connect_object (priv->container, "child-removed",
                           G_CALLBACK (child_removed_cb), self, 0);
}

static void
shell_lightbox_dispose (GObject *object)
{
  ShellLightbox *self = SHELL_LIGHTBOX (object);
  ShellLightboxPrivate *priv = shell_lightbox_get_instance_private (self);

  if (priv->container != NULL)
    {
      g_signal_handlers_disconnect_by_func (priv->container,
                                            child_added_cb, self);
      g_signal_handlers_disconnect_by_func (priv->container,
                                            child_removed_cb, self);
    }

  g_clear_weak_pointer (&priv->highlighted);
  g_clear_weak_pointer (&priv->size_constraint);
  g_clear_weak_pointer (&priv->container);
  g_ptr_array_set_size (priv->children, 0);

  G_OBJECT_CLASS (shell_lightbox_parent_class)->dispose (object);
}

static void
shell_lightbox_finalize (GObject *object)
{
  ShellLightbox *self = SHELL_LIGHTBOX (object);
  ShellLightboxPrivate *priv = shell_lightbox_get_instance_private (self);

  g_clear_pointer (&priv->children, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_lightbox_parent_class)->finalize (object);
}

static void
shell_lightbox_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  ShellLightbox *self = SHELL_LIGHTBOX (object);
  ShellLightboxPrivate *priv = shell_lightbox_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_CONTAINER:
      g_value_set_object (value, priv->container);
      break;

    case PROP_INHIBIT_EVENTS:
      g_value_set_boolean (value, priv->inhibit_events);
      break;

    case PROP_FADE_FACTOR:
      g_value_set_double (value, priv->fade_factor);
      break;

    case PROP_RADIAL_EFFECT:
      g_value_set_boolean (value, priv->radial_effect);
      break;

    case PROP_ACTIVE:
      g_value_set_boolean (value, priv->active);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_lightbox_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  ShellLightbox *self = SHELL_LIGHTBOX (object);
  ShellLightboxPrivate *priv = shell_lightbox_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_CONTAINER:
      g_set_weak_pointer (&priv->container, g_value_get_object (value));
      break;

    case PROP_INHIBIT_EVENTS:
      priv->inhibit_events = g_value_get_boolean (value);
      break;

    case PROP_FADE_FACTOR:
      priv->fade_factor = g_value_get_double (value);
      break;

    case PROP_RADIAL_EFFECT:
      priv->radial_effect = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_lightbox_class_init (ShellLightboxClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_lightbox_constructed;
  object_class->dispose = shell_lightbox_dispose;
  object_class->finalize = shell_lightbox_finalize;
  object_class->get_property = shell_lightbox_get_property;
  object_class->set_property = shell_lightbox_set_property;

  /**
   * ShellLightbox:container:
   *
   * The actor the shade is added to, covering its contents. Its children are
   * tracked while the lightbox exists.
   */
  props[PROP_CONTAINER] =
    g_param_spec_object ("container", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellLightbox:inhibit-events:
   *
   * Whether the shade is reactive, to inhibit events for the container.
   */
  props[PROP_INHIBIT_EVENTS] =
    g_param_spec_boolean ("inhibit-events", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellLightbox:fade-factor:
   *
   * The opacity, between 0 and 1, of the shade when it is lit. Ignored with
   * #ShellLightbox:radial-effect.
   */
  props[PROP_FADE_FACTOR] =
    g_param_spec_double ("fade-factor", NULL, NULL,
                         0.0, 1.0, DEFAULT_FADE_FACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellLightbox:radial-effect:
   *
   * Whether to use the GLSL radial (vignette) effect instead of a plain
   * translucent shade.
   */
  props[PROP_RADIAL_EFFECT] =
    g_param_spec_boolean ("radial-effect", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellLightbox:active:
   *
   * Whether the shade is fully lit.
   */
  props[PROP_ACTIVE] =
    g_param_spec_boolean ("active", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_lightbox_init (ShellLightbox *self)
{
  ShellLightboxPrivate *priv = shell_lightbox_get_instance_private (self);

  priv->children = g_ptr_array_new ();
}
