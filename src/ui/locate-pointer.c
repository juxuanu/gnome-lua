/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * locate-pointer.c: Plays a ripple animation at the pointer position when
 * the "locate pointer" accessibility setting is on. Port of
 * js/ui/locatePointer.js.
 */

#include "config.h"

#include <gio/gio.h>

#include "shell-global.h"
#include "ui/locate-pointer.h"
#include "ui/ripples.h"

#define LOCATE_POINTER_KEY "locate-pointer"
#define LOCATE_POINTER_SCHEMA "org.gnome.desktop.interface"

enum
{
  PROP_0,
  PROP_UI_GROUP,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellLocatePointer
{
  GObject parent;

  ClutterActor *ui_group; /* weak */
  GSettings *settings;
  ShellRipples *ripples;
};

G_DEFINE_FINAL_TYPE (ShellLocatePointer, shell_locate_pointer, G_TYPE_OBJECT)

static void
sync_enabled (ShellLocatePointer *self)
{
  gboolean enabled = g_settings_get_boolean (self->settings,
                                             LOCATE_POINTER_KEY);

  if (enabled == (self->ripples != NULL))
    return;

  if (enabled)
    {
      self->ripples = shell_ripples_new (0.5, 0.5,
                                         "ripple-pointer-location", TRUE);
      shell_ripples_add_to (self->ripples, self->ui_group);
    }
  else
    {
      shell_ripples_destroy (self->ripples);
      g_clear_object (&self->ripples);
    }
}

static void
locate_pointer_changed_cb (GSettings          *settings,
                           const char         *key,
                           ShellLocatePointer *self)
{
  sync_enabled (self);
}

/**
 * shell_locate_pointer_show:
 * @self: a #ShellLocatePointer
 *
 * Plays the ripple animation at the current pointer position, if the
 * `locate-pointer` setting is enabled.
 */
void
shell_locate_pointer_show (ShellLocatePointer *self)
{
  ClutterModifierType mods;
  int x, y;

  g_return_if_fail (SHELL_IS_LOCATE_POINTER (self));

  if (self->ripples == NULL)
    return;

  shell_global_get_pointer (shell_global_get (), &x, &y, &mods);
  shell_ripples_play_animation (self->ripples, x, y);
}

static void
shell_locate_pointer_constructed (GObject *object)
{
  ShellLocatePointer *self = SHELL_LOCATE_POINTER (object);

  G_OBJECT_CLASS (shell_locate_pointer_parent_class)->constructed (object);

  if (self->ui_group == NULL)
    {
      g_critical ("ShellLocatePointer needs a ui-group");
      return;
    }

  self->settings = g_settings_new (LOCATE_POINTER_SCHEMA);
  g_signal_connect_object (self->settings, "changed::" LOCATE_POINTER_KEY,
                           G_CALLBACK (locate_pointer_changed_cb), self, 0);
  sync_enabled (self);
}

static void
shell_locate_pointer_dispose (GObject *object)
{
  ShellLocatePointer *self = SHELL_LOCATE_POINTER (object);

  if (self->ripples != NULL)
    {
      shell_ripples_destroy (self->ripples);
      g_clear_object (&self->ripples);
    }

  g_clear_object (&self->settings);
  g_clear_weak_pointer (&self->ui_group);

  G_OBJECT_CLASS (shell_locate_pointer_parent_class)->dispose (object);
}

static void
shell_locate_pointer_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  ShellLocatePointer *self = SHELL_LOCATE_POINTER (object);

  switch (prop_id)
    {
    case PROP_UI_GROUP:
      g_value_set_object (value, self->ui_group);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_locate_pointer_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  ShellLocatePointer *self = SHELL_LOCATE_POINTER (object);

  switch (prop_id)
    {
    case PROP_UI_GROUP:
      g_set_weak_pointer (&self->ui_group, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_locate_pointer_class_init (ShellLocatePointerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_locate_pointer_constructed;
  object_class->dispose = shell_locate_pointer_dispose;
  object_class->get_property = shell_locate_pointer_get_property;
  object_class->set_property = shell_locate_pointer_set_property;

  /**
   * ShellLocatePointer:ui-group:
   *
   * The actor the ripples are added to. Main.uiGroup in the shell.
   */
  props[PROP_UI_GROUP] =
    g_param_spec_object ("ui-group", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_locate_pointer_init (ShellLocatePointer *self)
{
}

/**
 * shell_locate_pointer_new:
 * @ui_group: the actor the ripples are added to
 *
 * Returns: (transfer full): a new #ShellLocatePointer
 */
ShellLocatePointer *
shell_locate_pointer_new (ClutterActor *ui_group)
{
  g_return_val_if_fail (CLUTTER_IS_ACTOR (ui_group), NULL);

  return g_object_new (SHELL_TYPE_LOCATE_POINTER,
                       "ui-group", ui_group,
                       NULL);
}
