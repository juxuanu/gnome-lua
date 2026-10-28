/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * user-widget.c: A widget showing the avatar and the name of a user. Port of
 * js/ui/userWidget.js.
 *
 * Adapted from gdm/gui/user-switch-applet/applet.c
 *
 * Copyright (C) 2004-2005 James M. Cape <jcape@ignore-your.tv>.
 * Copyright (C) 2008,2009 Red Hat, Inc.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The `params` object of `Avatar` (`styleClass`, `reactive`, `iconSize`)
 *    are the properties of #ShellUserAvatar: `style-class` (default
 *    "user-icon"), `reactive` (default %FALSE) and `icon-size`.
 *  - The orientation of #ShellUserWidget is the `orientation` property: the
 *    style class and the alignment which depend on it follow its changes.
 *  - `UserWidgetLabel` is private to this file.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "shell-global.h"
#include "ui/user-widget.h"

/* ------------------------------------------------------------------------ */
/* ShellUserAvatar                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  AVATAR_PROP_0,

  AVATAR_PROP_USER,
  AVATAR_PROP_ICON_SIZE,

  AVATAR_N_PROPS
};

static GParamSpec *avatar_props[AVATAR_N_PROPS] = { NULL, };

struct _ShellUserAvatar
{
  StBin parent;

  ActUser *user;
  double icon_size;
};

G_DEFINE_FINAL_TYPE (ShellUserAvatar, shell_user_avatar, ST_TYPE_BIN)

static StThemeContext *
get_theme_context (void)
{
  return st_theme_context_get_for_stage (shell_global_get_stage (shell_global_get ()));
}

/**
 * shell_user_avatar_update:
 * @self: a #ShellUserAvatar
 *
 * Shows the icon file of the user, or the default avatar if there is none
 * (`update ()`)
 */
void
shell_user_avatar_update (ShellUserAvatar *self)
{
  ClutterActor *actor;
  const char *icon_file = NULL;
  int scale_factor;

  g_return_if_fail (SHELL_IS_USER_AVATAR (self));

  actor = CLUTTER_ACTOR (self);

  if (self->user != NULL)
    {
      icon_file = act_user_get_icon_file (self->user);
      if (icon_file != NULL &&
          (*icon_file == '\0' || !g_file_test (icon_file, G_FILE_TEST_EXISTS)))
        icon_file = NULL;
    }

  scale_factor = st_theme_context_get_scale_factor (get_theme_context ());
  clutter_actor_set_size (actor,
                          self->icon_size * scale_factor,
                          self->icon_size * scale_factor);

  if (icon_file != NULL)
    {
      g_autofree char *style = NULL;

      st_bin_set_child (ST_BIN (self), NULL);
      st_widget_add_style_class_name (ST_WIDGET (self), "user-avatar");
      style = g_strdup_printf ("background-image: url(\"%s\");\n"
                               "background-size: cover;",
                               icon_file);
      st_widget_set_style (ST_WIDGET (self), style);
    }
  else
    {
      ClutterActor *icon;

      st_widget_set_style (ST_WIDGET (self), NULL);
      icon = g_object_new (ST_TYPE_ICON,
                           "icon-name", "avatar-default-symbolic",
                           "icon-size", (int) self->icon_size,
                           "x-expand", TRUE,
                           "y-expand", TRUE,
                           NULL);
      st_bin_set_child (ST_BIN (self), icon);
    }
}

static void
scale_factor_changed_cb (StThemeContext *context,
                         GParamSpec     *pspec,
                         ShellUserAvatar *self)
{
  shell_user_avatar_update (self);
}

static void
shell_user_avatar_style_changed (StWidget *widget)
{
  ShellUserAvatar *self = SHELL_USER_AVATAR (widget);
  StThemeNode *node;
  double icon_size;

  ST_WIDGET_CLASS (shell_user_avatar_parent_class)->style_changed (widget);

  node = st_widget_get_theme_node (widget);
  if (!st_theme_node_lookup_length (node, "icon-size", FALSE, &icon_size))
    return;

  /* st_theme_node_lookup_length () returns a scaled value, but we need
   * unscaled */
  self->icon_size = icon_size / st_theme_context_get_scale_factor (get_theme_context ());
  shell_user_avatar_update (self);
}

/**
 * shell_user_avatar_set_sensitive:
 * @self: a #ShellUserAvatar
 * @sensitive: whether the avatar reacts to the pointer
 *
 * Sets #ClutterActor:reactive (`setSensitive ()`)
 */
void
shell_user_avatar_set_sensitive (ShellUserAvatar *self,
                                 gboolean         sensitive)
{
  g_return_if_fail (SHELL_IS_USER_AVATAR (self));

  clutter_actor_set_reactive (CLUTTER_ACTOR (self), sensitive);
}

/**
 * shell_user_avatar_get_user:
 * @self: a #ShellUserAvatar
 *
 * Returns: (transfer none) (nullable): the user whose icon is shown
 */
ActUser *
shell_user_avatar_get_user (ShellUserAvatar *self)
{
  g_return_val_if_fail (SHELL_IS_USER_AVATAR (self), NULL);

  return self->user;
}

/**
 * shell_user_avatar_get_icon_size:
 * @self: a #ShellUserAvatar
 *
 * Returns: the unscaled size of the icon
 */
int
shell_user_avatar_get_icon_size (ShellUserAvatar *self)
{
  g_return_val_if_fail (SHELL_IS_USER_AVATAR (self), 0);

  return (int) self->icon_size;
}

static void
shell_user_avatar_constructed (GObject *object)
{
  ShellUserAvatar *self = SHELL_USER_AVATAR (object);
  StWidget *widget = ST_WIDGET (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  StThemeContext *context;
  int scale_factor;

  G_OBJECT_CLASS (shell_user_avatar_parent_class)->constructed (object);

  context = get_theme_context ();
  scale_factor = st_theme_context_get_scale_factor (context);

  /* A style class passed to g_object_new () is applied after this */
  st_widget_set_style_class_name (widget, "user-icon");
  clutter_actor_set_size (actor,
                          self->icon_size * scale_factor,
                          self->icon_size * scale_factor);
  clutter_actor_set_x_expand (actor, FALSE);
  clutter_actor_set_y_expand (actor, FALSE);

  g_object_bind_property (self, "reactive", self, "track-hover",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self, "reactive", self, "can-focus",
                          G_BINDING_SYNC_CREATE);

  /* Monitor the scaling factor to make sure we recreate the avatar when
   * needed */
  g_signal_connect_object (context, "notify::scale-factor",
                           G_CALLBACK (scale_factor_changed_cb), self, 0);
}

static void
shell_user_avatar_dispose (GObject *object)
{
  ShellUserAvatar *self = SHELL_USER_AVATAR (object);

  g_clear_object (&self->user);

  G_OBJECT_CLASS (shell_user_avatar_parent_class)->dispose (object);
}

static void
shell_user_avatar_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  ShellUserAvatar *self = SHELL_USER_AVATAR (object);

  switch (prop_id)
    {
    case AVATAR_PROP_USER:
      g_value_set_object (value, self->user);
      break;

    case AVATAR_PROP_ICON_SIZE:
      g_value_set_int (value, (int) self->icon_size);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_user_avatar_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  ShellUserAvatar *self = SHELL_USER_AVATAR (object);

  switch (prop_id)
    {
    case AVATAR_PROP_USER:
      g_set_object (&self->user, g_value_get_object (value));
      break;

    case AVATAR_PROP_ICON_SIZE:
      self->icon_size = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_user_avatar_class_init (ShellUserAvatarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);

  object_class->constructed = shell_user_avatar_constructed;
  object_class->dispose = shell_user_avatar_dispose;
  object_class->get_property = shell_user_avatar_get_property;
  object_class->set_property = shell_user_avatar_set_property;

  widget_class->style_changed = shell_user_avatar_style_changed;

  /**
   * ShellUserAvatar:user:
   *
   * The user whose icon is shown. May be %NULL for a username-based login.
   */
  avatar_props[AVATAR_PROP_USER] =
    g_param_spec_object ("user", NULL, NULL,
                         ACT_TYPE_USER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellUserAvatar:icon-size:
   *
   * The unscaled size of the icon (`iconSize`). The `icon-size` of the theme
   * node overrides it.
   */
  avatar_props[AVATAR_PROP_ICON_SIZE] =
    g_param_spec_int ("icon-size", NULL, NULL,
                      0, G_MAXINT, SHELL_USER_AVATAR_DEFAULT_ICON_SIZE,
                      G_PARAM_READWRITE |
                      G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, AVATAR_N_PROPS, avatar_props);
}

static void
shell_user_avatar_init (ShellUserAvatar *self)
{
  self->icon_size = SHELL_USER_AVATAR_DEFAULT_ICON_SIZE;
}

/**
 * shell_user_avatar_new:
 * @user: (nullable): the user
 * @icon_size: the unscaled size of the icon, `SHELL_USER_AVATAR_DEFAULT_ICON_SIZE`
 *   for `new Avatar (user)`
 *
 * Creates an avatar. Call shell_user_avatar_update () to show the icon.
 *
 * Returns: (transfer full): a new #ShellUserAvatar
 */
ShellUserAvatar *
shell_user_avatar_new (ActUser *user,
                       int      icon_size)
{
  return g_object_new (SHELL_TYPE_USER_AVATAR,
                       "user", user,
                       "icon-size", icon_size,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellUserWidgetLabel                                                     */
/* ------------------------------------------------------------------------ */

/* The real name of the user, or the user name if the real name does not fit */

#define SHELL_TYPE_USER_WIDGET_LABEL (shell_user_widget_label_get_type ())
G_DECLARE_FINAL_TYPE (ShellUserWidgetLabel, shell_user_widget_label,
                      SHELL, USER_WIDGET_LABEL, StWidget)

enum
{
  LABEL_PROP_0,

  LABEL_PROP_USER,

  LABEL_N_PROPS
};

static GParamSpec *label_props[LABEL_N_PROPS] = { NULL, };

struct _ShellUserWidgetLabel
{
  StWidget parent;

  ActUser *user;

  StLabel *real_name_label;       /* weak */
  StLabel *user_name_label;       /* weak */
  ClutterActor *current_label;    /* weak */
};

G_DEFINE_FINAL_TYPE (ShellUserWidgetLabel, shell_user_widget_label, ST_TYPE_WIDGET)

static void
update_label_user (ShellUserWidgetLabel *self)
{
  if (self->real_name_label == NULL || self->user_name_label == NULL)
    return;

  if (act_user_is_loaded (self->user))
    {
      st_label_set_text (self->real_name_label, act_user_get_real_name (self->user));
      st_label_set_text (self->user_name_label, act_user_get_user_name (self->user));
    }
  else
    {
      st_label_set_text (self->real_name_label, "");
      st_label_set_text (self->user_name_label, "");
    }
}

static void
label_user_changed_cb (ActUser              *user,
                       ShellUserWidgetLabel *self)
{
  update_label_user (self);
}

static void
label_user_loaded_cb (ActUser              *user,
                      GParamSpec           *pspec,
                      ShellUserWidgetLabel *self)
{
  update_label_user (self);
}

static void
shell_user_widget_label_allocate (ClutterActor          *actor,
                                  const ClutterActorBox *box)
{
  ShellUserWidgetLabel *self = SHELL_USER_WIDGET_LABEL (actor);
  ClutterActor *real_name = CLUTTER_ACTOR (self->real_name_label);
  ClutterActor *user_name = CLUTTER_ACTOR (self->user_name_label);
  ClutterActor *hidden_label;
  ClutterActorBox child_box;
  float avail_width, avail_height;
  float nat_real_name_width;

  clutter_actor_set_allocation (actor, box);

  if (real_name == NULL || user_name == NULL)
    return;

  avail_width = box->x2 - box->x1;
  avail_height = box->y2 - box->y1;

  clutter_actor_get_preferred_size (real_name, NULL, NULL, &nat_real_name_width, NULL);

  clutter_actor_box_init_rect (&child_box, 0, 0, 0, 0);

  if (nat_real_name_width <= avail_width)
    {
      g_set_weak_pointer (&self->current_label, real_name);
      hidden_label = user_name;
    }
  else
    {
      g_set_weak_pointer (&self->current_label, user_name);
      hidden_label = real_name;
    }
  st_widget_set_label_actor (ST_WIDGET (self), self->current_label);

  clutter_actor_allocate (hidden_label, &child_box);

  clutter_actor_box_init_rect (&child_box, 0, 0, avail_width, avail_height);
  clutter_actor_allocate (self->current_label, &child_box);
}

static void
shell_user_widget_label_paint (ClutterActor        *actor,
                               ClutterPaintContext *paint_context)
{
  ShellUserWidgetLabel *self = SHELL_USER_WIDGET_LABEL (actor);

  if (self->current_label != NULL)
    clutter_actor_paint (self->current_label, paint_context);
}

static void
shell_user_widget_label_constructed (GObject *object)
{
  ShellUserWidgetLabel *self = SHELL_USER_WIDGET_LABEL (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *label;

  G_OBJECT_CLASS (shell_user_widget_label_parent_class)->constructed (object);

  clutter_actor_set_layout_manager (actor, clutter_bin_layout_new ());

  label = g_object_new (ST_TYPE_LABEL,
                        "style-class", "user-widget-label",
                        "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  g_set_weak_pointer (&self->real_name_label, ST_LABEL (label));
  clutter_actor_add_child (actor, label);

  label = g_object_new (ST_TYPE_LABEL,
                        "style-class", "user-widget-label",
                        "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  g_set_weak_pointer (&self->user_name_label, ST_LABEL (label));
  clutter_actor_add_child (actor, label);

  g_signal_connect_object (self->user, "notify::is-loaded",
                           G_CALLBACK (label_user_loaded_cb), self, 0);
  g_signal_connect_object (self->user, "changed",
                           G_CALLBACK (label_user_changed_cb), self, 0);
  update_label_user (self);
}

static void
shell_user_widget_label_dispose (GObject *object)
{
  ShellUserWidgetLabel *self = SHELL_USER_WIDGET_LABEL (object);

  g_clear_weak_pointer (&self->real_name_label);
  g_clear_weak_pointer (&self->user_name_label);
  g_clear_weak_pointer (&self->current_label);
  g_clear_object (&self->user);

  G_OBJECT_CLASS (shell_user_widget_label_parent_class)->dispose (object);
}

static void
shell_user_widget_label_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  ShellUserWidgetLabel *self = SHELL_USER_WIDGET_LABEL (object);

  switch (prop_id)
    {
    case LABEL_PROP_USER:
      self->user = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_user_widget_label_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  ShellUserWidgetLabel *self = SHELL_USER_WIDGET_LABEL (object);

  switch (prop_id)
    {
    case LABEL_PROP_USER:
      g_value_set_object (value, self->user);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_user_widget_label_class_init (ShellUserWidgetLabelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_user_widget_label_constructed;
  object_class->dispose = shell_user_widget_label_dispose;
  object_class->get_property = shell_user_widget_label_get_property;
  object_class->set_property = shell_user_widget_label_set_property;

  actor_class->allocate = shell_user_widget_label_allocate;
  actor_class->paint = shell_user_widget_label_paint;

  label_props[LABEL_PROP_USER] =
    g_param_spec_object ("user", NULL, NULL,
                         ACT_TYPE_USER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LABEL_N_PROPS, label_props);
}

static void
shell_user_widget_label_init (ShellUserWidgetLabel *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellUserWidget                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_USER,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellUserWidget
{
  StBoxLayout parent;

  /* If the user is NULL, that implies a username-based login authorization */
  ActUser *user;

  ShellUserAvatar *avatar;    /* weak */
  ClutterActor *label;        /* weak */
};

G_DEFINE_FINAL_TYPE (ShellUserWidget, shell_user_widget, ST_TYPE_BOX_LAYOUT)

static void
update_user (ShellUserWidget *self)
{
  if (self->avatar != NULL)
    shell_user_avatar_update (self->avatar);
}

static void
user_changed_cb (ActUser         *user,
                 ShellUserWidget *self)
{
  update_user (self);
}

static void
user_loaded_cb (ActUser         *user,
                GParamSpec      *pspec,
                ShellUserWidget *self)
{
  update_user (self);
}

static void
update_orientation (ShellUserWidget *self)
{
  gboolean vertical =
    st_box_layout_get_orientation (ST_BOX_LAYOUT (self)) == CLUTTER_ORIENTATION_VERTICAL;

  st_widget_set_style_class_name (ST_WIDGET (self),
                                  vertical ? "user-widget vertical" : "user-widget horizontal");
  clutter_actor_set_x_align (CLUTTER_ACTOR (self),
                             vertical ? CLUTTER_ACTOR_ALIGN_CENTER : CLUTTER_ACTOR_ALIGN_START);
}

static void
orientation_changed_cb (ShellUserWidget *self)
{
  update_orientation (self);
}

static void
shell_user_widget_constructed (GObject *object)
{
  ShellUserWidget *self = SHELL_USER_WIDGET (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellUserAvatar *avatar;

  G_OBJECT_CLASS (shell_user_widget_parent_class)->constructed (object);

  /* The orientation property is applied after this */
  update_orientation (self);
  g_signal_connect (self, "notify::orientation",
                    G_CALLBACK (orientation_changed_cb), NULL);

  avatar = shell_user_avatar_new (self->user, SHELL_USER_AVATAR_DEFAULT_ICON_SIZE);
  clutter_actor_set_x_align (CLUTTER_ACTOR (avatar), CLUTTER_ACTOR_ALIGN_CENTER);
  g_set_weak_pointer (&self->avatar, avatar);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (avatar));

  if (self->user != NULL)
    {
      ClutterActor *label;

      label = g_object_new (SHELL_TYPE_USER_WIDGET_LABEL,
                            "user", self->user,
                            NULL);
      g_set_weak_pointer (&self->label, label);
      clutter_actor_add_child (actor, label);

      g_object_bind_property (label, "label-actor",
                              self, "label-actor",
                              G_BINDING_SYNC_CREATE);

      g_signal_connect_object (self->user, "notify::is-loaded",
                               G_CALLBACK (user_loaded_cb), self, 0);
      g_signal_connect_object (self->user, "changed",
                               G_CALLBACK (user_changed_cb), self, 0);
    }
  else
    {
      ClutterActor *label;

      label = g_object_new (ST_TYPE_LABEL,
                            "style-class", "user-widget-label",
                            "text", "Empty User",
                            "opacity", 0,
                            NULL);
      g_set_weak_pointer (&self->label, label);
      clutter_actor_add_child (actor, label);
    }

  update_user (self);
}

static void
shell_user_widget_dispose (GObject *object)
{
  ShellUserWidget *self = SHELL_USER_WIDGET (object);

  g_signal_handlers_disconnect_by_func (self, orientation_changed_cb, NULL);
  g_clear_weak_pointer (&self->avatar);
  g_clear_weak_pointer (&self->label);
  g_clear_object (&self->user);

  G_OBJECT_CLASS (shell_user_widget_parent_class)->dispose (object);
}

static void
shell_user_widget_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  ShellUserWidget *self = SHELL_USER_WIDGET (object);

  switch (prop_id)
    {
    case PROP_USER:
      g_value_set_object (value, self->user);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_user_widget_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  ShellUserWidget *self = SHELL_USER_WIDGET (object);

  switch (prop_id)
    {
    case PROP_USER:
      self->user = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_user_widget_class_init (ShellUserWidgetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_user_widget_constructed;
  object_class->dispose = shell_user_widget_dispose;
  object_class->get_property = shell_user_widget_get_property;
  object_class->set_property = shell_user_widget_set_property;

  /**
   * ShellUserWidget:user:
   *
   * The user to show. %NULL implies a username-based login authorization:
   * the widget shows the default avatar and an invisible label.
   */
  props[PROP_USER] =
    g_param_spec_object ("user", NULL, NULL,
                         ACT_TYPE_USER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_user_widget_init (ShellUserWidget *self)
{
}

/**
 * shell_user_widget_new:
 * @user: (nullable): the user
 * @orientation: whether the name is next to the avatar
 *   (%CLUTTER_ORIENTATION_HORIZONTAL, the default of the JS class) or below it
 *
 * Creates a widget with the avatar and the name of @user (`new UserWidget
 * (user, orientation)`). Its #StWidget:label-actor is the label of the name.
 *
 * Returns: (transfer full): a new #ShellUserWidget
 */
ShellUserWidget *
shell_user_widget_new (ActUser            *user,
                       ClutterOrientation  orientation)
{
  return g_object_new (SHELL_TYPE_USER_WIDGET,
                       "user", user,
                       "orientation", orientation,
                       NULL);
}

/**
 * shell_user_widget_get_user:
 * @self: a #ShellUserWidget
 *
 * Returns: (transfer none) (nullable): the user shown
 */
ActUser *
shell_user_widget_get_user (ShellUserWidget *self)
{
  g_return_val_if_fail (SHELL_IS_USER_WIDGET (self), NULL);

  return self->user;
}

/**
 * shell_user_widget_hide_avatar:
 * @self: a #ShellUserWidget
 *
 * Hides the avatar and leaves the name (`hideAvatar ()`)
 */
void
shell_user_widget_hide_avatar (ShellUserWidget *self)
{
  g_return_if_fail (SHELL_IS_USER_WIDGET (self));

  if (self->avatar != NULL)
    clutter_actor_hide (CLUTTER_ACTOR (self->avatar));
}

/**
 * shell_user_widget_show_avatar:
 * @self: a #ShellUserWidget
 *
 * Shows the avatar again (`showAvatar ()`)
 */
void
shell_user_widget_show_avatar (ShellUserWidget *self)
{
  g_return_if_fail (SHELL_IS_USER_WIDGET (self));

  if (self->avatar != NULL)
    clutter_actor_show (CLUTTER_ACTOR (self->avatar));
}
