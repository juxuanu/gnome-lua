/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dialog.c: The widgets making up a modal dialog: the dialog frame with its
 * content area and buttons, the title/description message content and the
 * list sections used by the session and mount dialogs. Port of
 * js/ui/dialog.js.
 */

#include "config.h"

#include <clutter/clutter.h>
#include <clutter/clutter-pango.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-later.h>

#include "shell-global.h"
#include "ui/dialog.h"

/* ======================================================================== */
/* ShellDialog                                                              */
/* ======================================================================== */

enum
{
  DIALOG_PROP_0,

  DIALOG_PROP_PARENT_ACTOR,
  DIALOG_PROP_DIALOG_STYLE_CLASS,
  DIALOG_PROP_FRAME,
  DIALOG_PROP_CONTENT_LAYOUT,
  DIALOG_PROP_BUTTON_LAYOUT,
  DIALOG_PROP_INITIAL_KEY_FOCUS,

  N_DIALOG_PROPS
};

static GParamSpec *dialog_props[N_DIALOG_PROPS] = { NULL, };

/*
 * One dialog button: what the former buttonInfo object carried. Reference
 * counted because it is shared by the button's "clicked" handler and the key
 * table, and because an action may clear the buttons (or destroy the dialog)
 * while it is running.
 */
typedef struct
{
  grefcount ref_count;

  StButton *button; /* weak */
  ShellDialogButtonCallback action;
  gpointer user_data;
  GDestroyNotify destroy;
} ShellDialogButton;

typedef struct _ShellDialogPrivate ShellDialogPrivate;

struct _ShellDialogPrivate
{
  StBoxLayout *dialog; /* weak, the "modal-dialog" frame */
  StBoxLayout *content_layout; /* weak */
  StWidget *button_layout; /* weak */

  ClutterActor *parent_actor; /* weak */
  char *dialog_style_class;

  ClutterActor *initial_key_focus; /* weak */

  uint32_t pressed_key;
  GHashTable *button_keys; /* key symbol -> ShellDialogButton */
};

G_DEFINE_TYPE_WITH_PRIVATE (ShellDialog, shell_dialog, ST_TYPE_WIDGET)

static ShellDialogButton *
shell_dialog_button_new (StButton                  *button,
                         ShellDialogButtonCallback  action,
                         gpointer                   user_data,
                         GDestroyNotify             destroy)
{
  ShellDialogButton *info = g_new0 (ShellDialogButton, 1);

  g_ref_count_init (&info->ref_count);
  g_set_weak_pointer (&info->button, button);
  info->action = action;
  info->user_data = user_data;
  info->destroy = destroy;

  return info;
}

static ShellDialogButton *
shell_dialog_button_ref (ShellDialogButton *info)
{
  g_ref_count_inc (&info->ref_count);
  return info;
}

static void
shell_dialog_button_unref (ShellDialogButton *info)
{
  if (!g_ref_count_dec (&info->ref_count))
    return;

  g_clear_weak_pointer (&info->button);
  if (info->destroy != NULL)
    info->destroy (info->user_data);
  g_free (info);
}

static void
shell_dialog_button_activate (ShellDialogButton *info)
{
  if (info->action == NULL)
    return;

  /* The action may well clear the buttons or destroy the dialog */
  shell_dialog_button_ref (info);
  info->action (info->user_data);
  shell_dialog_button_unref (info);
}

static void
on_button_clicked (StButton          *button,
                   int                clicked_button,
                   ShellDialogButton *info)
{
  shell_dialog_button_activate (info);
}

static gboolean
on_key_press (ClutterKeyController *key_controller,
              ShellDialog          *self)
{
  ShellDialogPrivate *priv = shell_dialog_get_instance_private (self);
  uint32_t symbol = 0;

  clutter_key_controller_get_key (key_controller, &symbol, NULL, NULL);
  priv->pressed_key = symbol;

  return CLUTTER_EVENT_PROPAGATE;
}

static gboolean
on_key_release (ClutterKeyController *key_controller,
                ShellDialog          *self)
{
  ShellDialogPrivate *priv = shell_dialog_get_instance_private (self);
  ShellDialogButton *info;
  uint32_t pressed_key, symbol = 0;

  pressed_key = priv->pressed_key;
  priv->pressed_key = 0;

  clutter_key_controller_get_key (key_controller, &symbol, NULL, NULL);
  if (symbol != pressed_key)
    return CLUTTER_EVENT_PROPAGATE;

  info = g_hash_table_lookup (priv->button_keys, GUINT_TO_POINTER (symbol));
  if (info == NULL)
    return CLUTTER_EVENT_PROPAGATE;

  if (info->action != NULL && info->button != NULL &&
      clutter_actor_get_reactive (CLUTTER_ACTOR (info->button)))
    {
      shell_dialog_button_activate (info);
      return CLUTTER_EVENT_STOP;
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static void
shell_dialog_clear_initial_key_focus (ShellDialog *self)
{
  ShellDialogPrivate *priv = shell_dialog_get_instance_private (self);

  if (priv->initial_key_focus == NULL)
    return;

  g_signal_handlers_disconnect_by_data (priv->initial_key_focus, self);
  g_clear_weak_pointer (&priv->initial_key_focus);
}

static void
on_initial_key_focus_destroy (ClutterActor *actor,
                              ShellDialog  *self)
{
  shell_dialog_clear_initial_key_focus (self);
  g_object_notify_by_pspec (G_OBJECT (self),
                            dialog_props[DIALOG_PROP_INITIAL_KEY_FOCUS]);
}

/**
 * shell_dialog_set_initial_key_focus:
 * @self: a #ShellDialog
 * @actor: (nullable): the actor that should get the key focus when the
 *   dialog is opened, or %NULL to focus the dialog itself
 *
 * Sets the actor that gets the key focus first. The dialog forgets it when it
 * is destroyed. The first button added is the initial key focus until this is
 * called; the default button replaces any previous choice.
 */
void
shell_dialog_set_initial_key_focus (ShellDialog  *self,
                                    ClutterActor *actor)
{
  ShellDialogPrivate *priv;

  g_return_if_fail (SHELL_IS_DIALOG (self));
  g_return_if_fail (actor == NULL || CLUTTER_IS_ACTOR (actor));

  priv = shell_dialog_get_instance_private (self);

  if (priv->initial_key_focus == actor)
    return;

  shell_dialog_clear_initial_key_focus (self);

  if (actor != NULL)
    {
      g_set_weak_pointer (&priv->initial_key_focus, actor);
      g_signal_connect (actor, "destroy",
                        G_CALLBACK (on_initial_key_focus_destroy), self);
    }

  g_object_notify_by_pspec (G_OBJECT (self),
                            dialog_props[DIALOG_PROP_INITIAL_KEY_FOCUS]);
}

/**
 * shell_dialog_get_initial_key_focus:
 * @self: a #ShellDialog
 *
 * Returns: (transfer none): the actor to give the key focus to when the
 *   dialog is opened; @self if none was chosen
 */
ClutterActor *
shell_dialog_get_initial_key_focus (ShellDialog *self)
{
  ShellDialogPrivate *priv;

  g_return_val_if_fail (SHELL_IS_DIALOG (self), NULL);

  priv = shell_dialog_get_instance_private (self);

  if (priv->initial_key_focus != NULL)
    return priv->initial_key_focus;

  return CLUTTER_ACTOR (self);
}

/**
 * shell_dialog_get_frame:
 * @self: a #ShellDialog
 *
 * Returns: (transfer none) (nullable): the box with the `modal-dialog` style
 *   class that holds the content and the buttons. It is what close dialogs
 *   animate; the dialog widget itself only fills its parent (`_dialog` in the
 *   old JS class).
 */
StBoxLayout *
shell_dialog_get_frame (ShellDialog *self)
{
  ShellDialogPrivate *priv;

  g_return_val_if_fail (SHELL_IS_DIALOG (self), NULL);

  priv = shell_dialog_get_instance_private (self);
  return priv->dialog;
}

/**
 * shell_dialog_get_content_layout:
 * @self: a #ShellDialog
 *
 * Returns: (transfer none) (nullable): the vertical box where the content of
 *   the dialog goes (`contentLayout` in the old JS API)
 */
StBoxLayout *
shell_dialog_get_content_layout (ShellDialog *self)
{
  ShellDialogPrivate *priv;

  g_return_val_if_fail (SHELL_IS_DIALOG (self), NULL);

  priv = shell_dialog_get_instance_private (self);
  return priv->content_layout;
}

/**
 * shell_dialog_get_button_layout:
 * @self: a #ShellDialog
 *
 * Returns: (transfer none) (nullable): the widget holding the buttons
 *   (`buttonLayout` in the old JS API)
 */
StWidget *
shell_dialog_get_button_layout (ShellDialog *self)
{
  ShellDialogPrivate *priv;

  g_return_val_if_fail (SHELL_IS_DIALOG (self), NULL);

  priv = shell_dialog_get_instance_private (self);
  return priv->button_layout;
}

/**
 * shell_dialog_make_inactive:
 * @self: a #ShellDialog
 *
 * Makes all the buttons insensitive. This is done automatically when the
 * dialog is destroyed.
 */
void
shell_dialog_make_inactive (ShellDialog *self)
{
  ShellDialogPrivate *priv;
  ClutterActor *child;

  g_return_if_fail (SHELL_IS_DIALOG (self));

  priv = shell_dialog_get_instance_private (self);

  if (priv->button_layout == NULL)
    return;

  for (child = clutter_actor_get_first_child (CLUTTER_ACTOR (priv->button_layout));
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    clutter_actor_set_reactive (child, FALSE);
}

/**
 * shell_dialog_add_button:
 * @self: a #ShellDialog
 * @label: (nullable): the text of the button
 * @key: a key symbol (such as `Clutter.KEY_Escape`) that activates the
 *   button when released, or 0 for none. A default button with no key of its
 *   own is activated by Return, KP Enter and ISO Enter.
 * @is_default: whether this is the default button: it is styled with the
 *   `default` pseudo class and becomes the initial key focus
 * @action: (scope notified) (nullable) (closure user_data) (destroy destroy):
 *   called when the button is clicked or its key is released
 * @user_data: data for @action
 * @destroy: called when @user_data is no longer needed,
 *   that is when the button is destroyed and its key binding dropped
 *
 * Adds a button to the end of the button row. This replaces the former
 * `addButton ({label, action, key, default})`; the `button` back-reference
 * that function stored in its argument is not provided.
 *
 * Returns: (transfer none): the new button
 */
StButton *
shell_dialog_add_button (ShellDialog               *self,
                         const char                *label,
                         uint32_t                   key,
                         gboolean                   is_default,
                         ShellDialogButtonCallback  action,
                         gpointer                   user_data,
                         GDestroyNotify             destroy)
{
  ShellDialogPrivate *priv;
  ShellDialogButton *info;
  ClutterActor *actor;
  StButton *button;
  uint32_t keys[3];
  int n_keys = 0;
  int i;

  g_return_val_if_fail (SHELL_IS_DIALOG (self), NULL);

  priv = shell_dialog_get_instance_private (self);
  g_return_val_if_fail (priv->button_layout != NULL, NULL);

  if (key != 0)
    {
      keys[n_keys++] = key;
    }
  else if (is_default)
    {
      keys[n_keys++] = CLUTTER_KEY_Return;
      keys[n_keys++] = CLUTTER_KEY_KP_Enter;
      keys[n_keys++] = CLUTTER_KEY_ISO_Enter;
    }

  button = ST_BUTTON (g_object_new (ST_TYPE_BUTTON,
                                    "style-class", "modal-dialog-button",
                                    "button-mask", ST_BUTTON_PRIMARY | ST_BUTTON_SECONDARY,
                                    "reactive", TRUE,
                                    "can-focus", TRUE,
                                    "x-expand", TRUE,
                                    "y-expand", TRUE,
                                    "label", label,
                                    NULL));
  actor = CLUTTER_ACTOR (button);

  info = shell_dialog_button_new (button, action, user_data, destroy);

  /* The handler owns a reference, dropped when the button is disposed */
  g_signal_connect_data (button, "clicked",
                         G_CALLBACK (on_button_clicked),
                         shell_dialog_button_ref (info),
                         (GClosureNotify) shell_dialog_button_unref, 0);

  if (is_default)
    st_widget_add_style_pseudo_class (ST_WIDGET (button), "default");

  if (priv->initial_key_focus == NULL || is_default)
    shell_dialog_set_initial_key_focus (self, actor);

  for (i = 0; i < n_keys; i++)
    g_hash_table_insert (priv->button_keys,
                         GUINT_TO_POINTER (keys[i]),
                         shell_dialog_button_ref (info));

  /* The signal handler and the key table keep it alive from here */
  shell_dialog_button_unref (info);

  clutter_actor_add_child (CLUTTER_ACTOR (priv->button_layout), actor);

  return button;
}

/**
 * shell_dialog_clear_buttons:
 * @self: a #ShellDialog
 *
 * Destroys all the buttons and forgets their key bindings.
 */
void
shell_dialog_clear_buttons (ShellDialog *self)
{
  ShellDialogPrivate *priv;

  g_return_if_fail (SHELL_IS_DIALOG (self));

  priv = shell_dialog_get_instance_private (self);

  if (priv->button_layout != NULL)
    clutter_actor_destroy_all_children (CLUTTER_ACTOR (priv->button_layout));

  g_hash_table_remove_all (priv->button_keys);
}

static void
set_dialog_style_class (ShellDialog *self,
                        const char  *style_class)
{
  ShellDialogPrivate *priv = shell_dialog_get_instance_private (self);

  if (g_strcmp0 (priv->dialog_style_class, style_class) == 0)
    return;

  if (priv->dialog != NULL && priv->dialog_style_class != NULL)
    st_widget_remove_style_class_name (ST_WIDGET (priv->dialog),
                                       priv->dialog_style_class);

  g_free (priv->dialog_style_class);
  priv->dialog_style_class = g_strdup (style_class);

  if (priv->dialog != NULL && priv->dialog_style_class != NULL)
    st_widget_add_style_class_name (ST_WIDGET (priv->dialog),
                                    priv->dialog_style_class);

  g_object_notify_by_pspec (G_OBJECT (self),
                            dialog_props[DIALOG_PROP_DIALOG_STYLE_CLASS]);
}

static void
shell_dialog_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  ShellDialog *self = SHELL_DIALOG (object);
  ShellDialogPrivate *priv = shell_dialog_get_instance_private (self);

  switch (prop_id)
    {
    case DIALOG_PROP_PARENT_ACTOR:
      g_value_set_object (value, priv->parent_actor);
      break;

    case DIALOG_PROP_DIALOG_STYLE_CLASS:
      g_value_set_string (value, priv->dialog_style_class);
      break;

    case DIALOG_PROP_FRAME:
      g_value_set_object (value, priv->dialog);
      break;

    case DIALOG_PROP_CONTENT_LAYOUT:
      g_value_set_object (value, priv->content_layout);
      break;

    case DIALOG_PROP_BUTTON_LAYOUT:
      g_value_set_object (value, priv->button_layout);
      break;

    case DIALOG_PROP_INITIAL_KEY_FOCUS:
      g_value_set_object (value, shell_dialog_get_initial_key_focus (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_dialog_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  ShellDialog *self = SHELL_DIALOG (object);
  ShellDialogPrivate *priv = shell_dialog_get_instance_private (self);

  switch (prop_id)
    {
    case DIALOG_PROP_PARENT_ACTOR:
      /* construct-only, added to in constructed () */
      g_set_weak_pointer (&priv->parent_actor, g_value_get_object (value));
      break;

    case DIALOG_PROP_DIALOG_STYLE_CLASS:
      set_dialog_style_class (self, g_value_get_string (value));
      break;

    case DIALOG_PROP_INITIAL_KEY_FOCUS:
      shell_dialog_set_initial_key_focus (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_dialog_constructed (GObject *object)
{
  ShellDialog *self = SHELL_DIALOG (object);
  ShellDialogPrivate *priv = shell_dialog_get_instance_private (self);

  G_OBJECT_CLASS (shell_dialog_parent_class)->constructed (object);

  if (priv->parent_actor != NULL)
    clutter_actor_add_child (priv->parent_actor, CLUTTER_ACTOR (self));
}

static void
shell_dialog_dispose (GObject *object)
{
  ShellDialog *self = SHELL_DIALOG (object);
  ShellDialogPrivate *priv = shell_dialog_get_instance_private (self);

  /* The actor's destroy class handler runs from the parent dispose and
   * needs the button layout, so only let go of things afterwards */
  G_OBJECT_CLASS (shell_dialog_parent_class)->dispose (object);

  shell_dialog_clear_initial_key_focus (self);
  g_hash_table_remove_all (priv->button_keys);
  g_clear_weak_pointer (&priv->parent_actor);
  g_clear_weak_pointer (&priv->dialog);
  g_clear_weak_pointer (&priv->content_layout);
  g_clear_weak_pointer (&priv->button_layout);
}

static void
shell_dialog_finalize (GObject *object)
{
  ShellDialog *self = SHELL_DIALOG (object);
  ShellDialogPrivate *priv = shell_dialog_get_instance_private (self);

  g_clear_pointer (&priv->button_keys, g_hash_table_unref);
  g_clear_pointer (&priv->dialog_style_class, g_free);

  G_OBJECT_CLASS (shell_dialog_parent_class)->finalize (object);
}

static void
shell_dialog_destroy (ClutterActor *actor)
{
  shell_dialog_make_inactive (SHELL_DIALOG (actor));

  CLUTTER_ACTOR_CLASS (shell_dialog_parent_class)->destroy (actor);
}

static void
shell_dialog_class_init (ShellDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_dialog_constructed;
  object_class->dispose = shell_dialog_dispose;
  object_class->finalize = shell_dialog_finalize;
  object_class->get_property = shell_dialog_get_property;
  object_class->set_property = shell_dialog_set_property;

  actor_class->destroy = shell_dialog_destroy;

  /**
   * ShellDialog:parent-actor:
   *
   * The actor the dialog is added to when it is created (the former
   * `parentActor` constructor argument).
   */
  dialog_props[DIALOG_PROP_PARENT_ACTOR] =
    g_param_spec_object ("parent-actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDialog:dialog-style-class:
   *
   * A style class added to the dialog frame, the child with the
   * `modal-dialog` class (the former `styleClass` constructor argument).
   * It is not `style-class`, which styles the dialog widget itself.
   */
  dialog_props[DIALOG_PROP_DIALOG_STYLE_CLASS] =
    g_param_spec_string ("dialog-style-class", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDialog:frame:
   *
   * The box with the `modal-dialog` style class holding the content and the
   * buttons.
   */
  dialog_props[DIALOG_PROP_FRAME] =
    g_param_spec_object ("frame", NULL, NULL,
                         ST_TYPE_BOX_LAYOUT,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDialog:content-layout:
   *
   * The box where the content of the dialog goes.
   */
  dialog_props[DIALOG_PROP_CONTENT_LAYOUT] =
    g_param_spec_object ("content-layout", NULL, NULL,
                         ST_TYPE_BOX_LAYOUT,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDialog:button-layout:
   *
   * The widget holding the buttons.
   */
  dialog_props[DIALOG_PROP_BUTTON_LAYOUT] =
    g_param_spec_object ("button-layout", NULL, NULL,
                         ST_TYPE_WIDGET,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellDialog:initial-key-focus:
   *
   * The actor to give the key focus to when the dialog is opened. Reading it
   * returns the dialog itself when none was chosen.
   */
  dialog_props[DIALOG_PROP_INITIAL_KEY_FOCUS] =
    g_param_spec_object ("initial-key-focus", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_DIALOG_PROPS, dialog_props);
}

static void
shell_dialog_init (ShellDialog *self)
{
  ShellDialogPrivate *priv = shell_dialog_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *dialog, *content_layout, *button_layout;
  ClutterLayoutManager *button_layout_manager;
  ClutterAction *key_controller;

  priv->button_keys =
    g_hash_table_new_full (NULL, NULL, NULL,
                           (GDestroyNotify) shell_dialog_button_unref);

  clutter_actor_set_layout_manager (actor, clutter_bin_layout_new ());
  clutter_actor_set_reactive (actor, TRUE);

  /* _createDialog () */
  dialog = CLUTTER_ACTOR (st_box_layout_new ());
  st_widget_set_style_class_name (ST_WIDGET (dialog), "modal-dialog");
  clutter_actor_set_x_align (dialog, CLUTTER_ACTOR_ALIGN_CENTER);
  clutter_actor_set_y_align (dialog, CLUTTER_ACTOR_ALIGN_CENTER);
  st_box_layout_set_orientation (ST_BOX_LAYOUT (dialog),
                                 CLUTTER_ORIENTATION_VERTICAL);

  /* modal dialogs are fixed width and grow vertically; set the request
   * mode accordingly so wrapped labels are handled correctly during
   * size requests. */
  clutter_actor_set_request_mode (dialog, CLUTTER_REQUEST_HEIGHT_FOR_WIDTH);
  clutter_actor_set_offscreen_redirect (dialog, CLUTTER_OFFSCREEN_REDIRECT_ALWAYS);

  content_layout = CLUTTER_ACTOR (st_box_layout_new ());
  st_box_layout_set_orientation (ST_BOX_LAYOUT (content_layout),
                                 CLUTTER_ORIENTATION_VERTICAL);
  st_widget_set_style_class_name (ST_WIDGET (content_layout),
                                  "modal-dialog-content-box");
  clutter_actor_set_y_expand (content_layout, TRUE);
  clutter_actor_add_child (dialog, content_layout);

  button_layout_manager = clutter_box_layout_new ();
  clutter_box_layout_set_spacing (CLUTTER_BOX_LAYOUT (button_layout_manager), 12);
  clutter_box_layout_set_homogeneous (CLUTTER_BOX_LAYOUT (button_layout_manager), TRUE);
  button_layout = CLUTTER_ACTOR (g_object_new (ST_TYPE_WIDGET, NULL));
  st_widget_set_style_class_name (ST_WIDGET (button_layout),
                                  "modal-dialog-button-box");
  clutter_actor_set_layout_manager (button_layout, button_layout_manager);
  clutter_actor_add_child (dialog, button_layout);

  clutter_actor_add_child (actor, dialog);

  g_set_weak_pointer (&priv->dialog, ST_BOX_LAYOUT (dialog));
  g_set_weak_pointer (&priv->content_layout, ST_BOX_LAYOUT (content_layout));
  g_set_weak_pointer (&priv->button_layout, ST_WIDGET (button_layout));

  key_controller = clutter_key_controller_new (NULL);
  g_signal_connect (key_controller, "key-press",
                    G_CALLBACK (on_key_press), self);
  g_signal_connect (key_controller, "key-release",
                    G_CALLBACK (on_key_release), self);
  clutter_actor_add_action (actor, key_controller);
}

/**
 * shell_dialog_new:
 * @parent_actor: (nullable): the actor to add the dialog to
 * @dialog_style_class: (nullable): a style class for the dialog frame
 *
 * Creates a new dialog, the equivalent of `new Dialog.Dialog (parentActor,
 * styleClass)`.
 *
 * Returns: (transfer floating): a new #ShellDialog
 */
ShellDialog *
shell_dialog_new (ClutterActor *parent_actor,
                  const char   *dialog_style_class)
{
  return g_object_new (SHELL_TYPE_DIALOG,
                       "parent-actor", parent_actor,
                       "dialog-style-class", dialog_style_class,
                       NULL);
}

/* ======================================================================== */
/* Helpers shared by the label based widgets                                */
/* ======================================================================== */

/* The former bind_property ()s: the label shows the text and is only visible
 * when the property is not %NULL. */
static void
sync_label (StLabel    *label,
            const char *text)
{
  st_label_set_text (label, text);
  if (text != NULL)
    clutter_actor_show (CLUTTER_ACTOR (label));
  else
    clutter_actor_hide (CLUTTER_ACTOR (label));
}

static StLabel *
create_wrapping_label (const char *style_class)
{
  StLabel *label = ST_LABEL (st_label_new (NULL));
  ClutterText *text = st_label_get_clutter_text (label);

  st_widget_set_style_class_name (ST_WIDGET (label), style_class);
  clutter_text_set_ellipsize (text, PANGO_ELLIPSIZE_NONE);
  clutter_text_set_line_wrap (text, TRUE);

  return label;
}

/* ======================================================================== */
/* ShellMessageDialogContent                                                */
/* ======================================================================== */

enum
{
  MESSAGE_PROP_0,

  MESSAGE_PROP_TITLE,
  MESSAGE_PROP_DESCRIPTION,

  N_MESSAGE_PROPS
};

static GParamSpec *message_props[N_MESSAGE_PROPS] = { NULL, };

typedef struct _ShellMessageDialogContentPrivate ShellMessageDialogContentPrivate;

struct _ShellMessageDialogContentPrivate
{
  StLabel *title_label; /* weak */
  StLabel *description_label; /* weak */

  char *title;
  char *description;

  unsigned int update_title_style_later;
};

G_DEFINE_TYPE_WITH_PRIVATE (ShellMessageDialogContent,
                            shell_message_dialog_content,
                            ST_TYPE_BOX_LAYOUT)

static MetaLaters *
get_laters (void)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  MetaCompositor *compositor;

  if (display == NULL)
    return NULL;

  compositor = meta_display_get_compositor (display);
  if (compositor == NULL)
    return NULL;

  return meta_compositor_get_laters (compositor);
}

static void
remove_update_title_style_later (ShellMessageDialogContent *self)
{
  ShellMessageDialogContentPrivate *priv =
    shell_message_dialog_content_get_instance_private (self);
  MetaLaters *laters;

  if (priv->update_title_style_later == 0)
    return;

  laters = get_laters ();
  if (laters != NULL)
    meta_laters_remove (laters, priv->update_title_style_later);
  priv->update_title_style_later = 0;
}

static gboolean
add_lightweight_style (gpointer user_data)
{
  ShellMessageDialogContent *self = user_data;
  ShellMessageDialogContentPrivate *priv =
    shell_message_dialog_content_get_instance_private (self);

  priv->update_title_style_later = 0;
  if (priv->title_label != NULL)
    st_widget_add_style_class_name (ST_WIDGET (priv->title_label), "lightweight");

  return G_SOURCE_REMOVE;
}

/* A title that does not fit on one line is drawn lighter */
static void
update_title_style (ShellMessageDialogContent *self)
{
  ShellMessageDialogContentPrivate *priv =
    shell_message_dialog_content_get_instance_private (self);
  ClutterActor *title;
  MetaLaters *laters;
  float title_nat_width;

  if (priv->title_label == NULL)
    return;

  title = CLUTTER_ACTOR (priv->title_label);
  if (!clutter_actor_is_mapped (title))
    return;

  st_widget_ensure_style (ST_WIDGET (priv->title_label));
  clutter_actor_get_preferred_width (title, -1, NULL, &title_nat_width);

  if (title_nat_width > clutter_actor_get_width (CLUTTER_ACTOR (self)))
    {
      if (priv->update_title_style_later != 0)
        return;

      laters = get_laters ();
      if (laters == NULL)
        return;

      priv->update_title_style_later =
        meta_laters_add (laters, META_LATER_BEFORE_REDRAW,
                         add_lightweight_style, self, NULL);
    }
}

static void
on_size_notify (ShellMessageDialogContent *self)
{
  update_title_style (self);
}

/**
 * shell_message_dialog_content_get_title:
 * @self: a #ShellMessageDialogContent
 *
 * Returns: (transfer none) (nullable): the title
 */
const char *
shell_message_dialog_content_get_title (ShellMessageDialogContent *self)
{
  ShellMessageDialogContentPrivate *priv;

  g_return_val_if_fail (SHELL_IS_MESSAGE_DIALOG_CONTENT (self), NULL);

  priv = shell_message_dialog_content_get_instance_private (self);
  return priv->title;
}

/**
 * shell_message_dialog_content_set_title:
 * @self: a #ShellMessageDialogContent
 * @title: (nullable): the title; the title label is hidden when %NULL
 */
void
shell_message_dialog_content_set_title (ShellMessageDialogContent *self,
                                        const char                *title)
{
  ShellMessageDialogContentPrivate *priv;

  g_return_if_fail (SHELL_IS_MESSAGE_DIALOG_CONTENT (self));

  priv = shell_message_dialog_content_get_instance_private (self);

  if (g_strcmp0 (priv->title, title) == 0)
    return;

  g_free (priv->title);
  priv->title = g_strdup (title);

  if (priv->title_label != NULL)
    {
      sync_label (priv->title_label, priv->title);

      st_widget_remove_style_class_name (ST_WIDGET (priv->title_label),
                                         "lightweight");
      update_title_style (self);
    }

  g_object_notify_by_pspec (G_OBJECT (self), message_props[MESSAGE_PROP_TITLE]);
}

/**
 * shell_message_dialog_content_get_description:
 * @self: a #ShellMessageDialogContent
 *
 * Returns: (transfer none) (nullable): the description
 */
const char *
shell_message_dialog_content_get_description (ShellMessageDialogContent *self)
{
  ShellMessageDialogContentPrivate *priv;

  g_return_val_if_fail (SHELL_IS_MESSAGE_DIALOG_CONTENT (self), NULL);

  priv = shell_message_dialog_content_get_instance_private (self);
  return priv->description;
}

/**
 * shell_message_dialog_content_set_description:
 * @self: a #ShellMessageDialogContent
 * @description: (nullable): the description, shown as plain text; the
 *   description label is hidden when %NULL
 */
void
shell_message_dialog_content_set_description (ShellMessageDialogContent *self,
                                              const char                *description)
{
  ShellMessageDialogContentPrivate *priv;

  g_return_if_fail (SHELL_IS_MESSAGE_DIALOG_CONTENT (self));

  priv = shell_message_dialog_content_get_instance_private (self);

  if (g_strcmp0 (priv->description, description) == 0)
    return;

  g_free (priv->description);
  priv->description = g_strdup (description);

  if (priv->description_label != NULL)
    sync_label (priv->description_label, priv->description);

  g_object_notify_by_pspec (G_OBJECT (self), message_props[MESSAGE_PROP_DESCRIPTION]);
}

static void
shell_message_dialog_content_get_property (GObject    *object,
                                           guint       prop_id,
                                           GValue     *value,
                                           GParamSpec *pspec)
{
  ShellMessageDialogContent *self = SHELL_MESSAGE_DIALOG_CONTENT (object);

  switch (prop_id)
    {
    case MESSAGE_PROP_TITLE:
      g_value_set_string (value, shell_message_dialog_content_get_title (self));
      break;

    case MESSAGE_PROP_DESCRIPTION:
      g_value_set_string (value, shell_message_dialog_content_get_description (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_message_dialog_content_set_property (GObject      *object,
                                           guint         prop_id,
                                           const GValue *value,
                                           GParamSpec   *pspec)
{
  ShellMessageDialogContent *self = SHELL_MESSAGE_DIALOG_CONTENT (object);

  switch (prop_id)
    {
    case MESSAGE_PROP_TITLE:
      shell_message_dialog_content_set_title (self, g_value_get_string (value));
      break;

    case MESSAGE_PROP_DESCRIPTION:
      shell_message_dialog_content_set_description (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_message_dialog_content_dispose (GObject *object)
{
  ShellMessageDialogContent *self = SHELL_MESSAGE_DIALOG_CONTENT (object);
  ShellMessageDialogContentPrivate *priv =
    shell_message_dialog_content_get_instance_private (self);

  remove_update_title_style_later (self);

  G_OBJECT_CLASS (shell_message_dialog_content_parent_class)->dispose (object);

  g_clear_weak_pointer (&priv->title_label);
  g_clear_weak_pointer (&priv->description_label);
}

static void
shell_message_dialog_content_finalize (GObject *object)
{
  ShellMessageDialogContent *self = SHELL_MESSAGE_DIALOG_CONTENT (object);
  ShellMessageDialogContentPrivate *priv =
    shell_message_dialog_content_get_instance_private (self);

  g_clear_pointer (&priv->title, g_free);
  g_clear_pointer (&priv->description, g_free);

  G_OBJECT_CLASS (shell_message_dialog_content_parent_class)->finalize (object);
}

static void
shell_message_dialog_content_destroy (ClutterActor *actor)
{
  remove_update_title_style_later (SHELL_MESSAGE_DIALOG_CONTENT (actor));

  CLUTTER_ACTOR_CLASS (shell_message_dialog_content_parent_class)->destroy (actor);
}

/* StBoxLayout's layout manager only exists after ClutterActor's
 * constructed(), so the orientation cannot be set from init(). */
static void
shell_message_dialog_content_constructed (GObject *object)
{
  G_OBJECT_CLASS (shell_message_dialog_content_parent_class)->constructed (object);
  st_box_layout_set_orientation (ST_BOX_LAYOUT (object),
                                 CLUTTER_ORIENTATION_VERTICAL);
}

static void
shell_message_dialog_content_class_init (ShellMessageDialogContentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_message_dialog_content_constructed;

  object_class->get_property = shell_message_dialog_content_get_property;
  object_class->set_property = shell_message_dialog_content_set_property;
  object_class->dispose = shell_message_dialog_content_dispose;
  object_class->finalize = shell_message_dialog_content_finalize;

  actor_class->destroy = shell_message_dialog_content_destroy;

  /**
   * ShellMessageDialogContent:title:
   *
   * The title, shown above the description. The label is hidden when it is
   * %NULL.
   */
  message_props[MESSAGE_PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMessageDialogContent:description:
   *
   * The description, shown below the title as plain text that wraps. The
   * label is hidden when it is %NULL.
   */
  message_props[MESSAGE_PROP_DESCRIPTION] =
    g_param_spec_string ("description", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_MESSAGE_PROPS, message_props);
}

static void
shell_message_dialog_content_init (ShellMessageDialogContent *self)
{
  ShellMessageDialogContentPrivate *priv =
    shell_message_dialog_content_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  StLabel *title, *description;

  st_widget_set_style_class_name (ST_WIDGET (self), "message-dialog-content");
  clutter_actor_set_x_expand (actor, TRUE);

  title = create_wrapping_label ("message-dialog-title");
  description = create_wrapping_label ("message-dialog-description");

  /* Hidden until there is something to show */
  sync_label (title, NULL);
  sync_label (description, NULL);

  clutter_actor_add_child (actor, CLUTTER_ACTOR (title));
  clutter_actor_add_child (actor, CLUTTER_ACTOR (description));

  g_set_weak_pointer (&priv->title_label, title);
  g_set_weak_pointer (&priv->description_label, description);

  g_signal_connect_swapped (self, "notify::size",
                            G_CALLBACK (on_size_notify), self);
}

/* ======================================================================== */
/* ShellListSection                                                         */
/* ======================================================================== */

enum
{
  SECTION_PROP_0,

  SECTION_PROP_TITLE,
  SECTION_PROP_LIST,

  N_SECTION_PROPS
};

static GParamSpec *section_props[N_SECTION_PROPS] = { NULL, };

struct _ShellListSection
{
  StBoxLayout parent;

  StLabel *title_label; /* weak */
  StBoxLayout *list; /* weak */

  char *title;
};

G_DEFINE_FINAL_TYPE (ShellListSection, shell_list_section, ST_TYPE_BOX_LAYOUT)

/**
 * shell_list_section_get_title:
 * @self: a #ShellListSection
 *
 * Returns: (transfer none) (nullable): the title
 */
const char *
shell_list_section_get_title (ShellListSection *self)
{
  g_return_val_if_fail (SHELL_IS_LIST_SECTION (self), NULL);

  return self->title;
}

/**
 * shell_list_section_set_title:
 * @self: a #ShellListSection
 * @title: (nullable): the title; the title label is hidden when %NULL
 */
void
shell_list_section_set_title (ShellListSection *self,
                              const char       *title)
{
  g_return_if_fail (SHELL_IS_LIST_SECTION (self));

  if (g_strcmp0 (self->title, title) == 0)
    return;

  g_free (self->title);
  self->title = g_strdup (title);

  if (self->title_label != NULL)
    sync_label (self->title_label, self->title);

  g_object_notify_by_pspec (G_OBJECT (self), section_props[SECTION_PROP_TITLE]);
}

/**
 * shell_list_section_get_list:
 * @self: a #ShellListSection
 *
 * Returns: (transfer none) (nullable): the vertical box, inside a scroll
 *   view, where the items of the section go
 */
StBoxLayout *
shell_list_section_get_list (ShellListSection *self)
{
  g_return_val_if_fail (SHELL_IS_LIST_SECTION (self), NULL);

  return self->list;
}

static void
shell_list_section_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellListSection *self = SHELL_LIST_SECTION (object);

  switch (prop_id)
    {
    case SECTION_PROP_TITLE:
      g_value_set_string (value, self->title);
      break;

    case SECTION_PROP_LIST:
      g_value_set_object (value, self->list);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_list_section_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellListSection *self = SHELL_LIST_SECTION (object);

  switch (prop_id)
    {
    case SECTION_PROP_TITLE:
      shell_list_section_set_title (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_list_section_dispose (GObject *object)
{
  ShellListSection *self = SHELL_LIST_SECTION (object);

  G_OBJECT_CLASS (shell_list_section_parent_class)->dispose (object);

  g_clear_weak_pointer (&self->title_label);
  g_clear_weak_pointer (&self->list);
}

static void
shell_list_section_finalize (GObject *object)
{
  ShellListSection *self = SHELL_LIST_SECTION (object);

  g_clear_pointer (&self->title, g_free);

  G_OBJECT_CLASS (shell_list_section_parent_class)->finalize (object);
}

static void
shell_list_section_constructed (GObject *object)
{
  G_OBJECT_CLASS (shell_list_section_parent_class)->constructed (object);
  st_box_layout_set_orientation (ST_BOX_LAYOUT (object),
                                 CLUTTER_ORIENTATION_VERTICAL);
}

static void
shell_list_section_class_init (ShellListSectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_list_section_constructed;

  object_class->get_property = shell_list_section_get_property;
  object_class->set_property = shell_list_section_set_property;
  object_class->dispose = shell_list_section_dispose;
  object_class->finalize = shell_list_section_finalize;

  /**
   * ShellListSection:title:
   *
   * The title of the section. The label is hidden when it is %NULL.
   */
  section_props[SECTION_PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellListSection:list:
   *
   * The box where the items of the section go.
   */
  section_props[SECTION_PROP_LIST] =
    g_param_spec_object ("list", NULL, NULL,
                         ST_TYPE_BOX_LAYOUT,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_SECTION_PROPS, section_props);
}

static void
shell_list_section_init (ShellListSection *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  StLabel *title;
  StBoxLayout *list;
  StWidget *scroll_view;

  st_widget_set_style_class_name (ST_WIDGET (self), "dialog-list");
  clutter_actor_set_x_expand (actor, TRUE);

  title = create_wrapping_label ("dialog-list-title");
  sync_label (title, NULL);

  list = ST_BOX_LAYOUT (st_box_layout_new ());
  st_widget_set_style_class_name (ST_WIDGET (list), "dialog-list-box");
  st_box_layout_set_orientation (list, CLUTTER_ORIENTATION_VERTICAL);

  scroll_view = st_scroll_view_new ();
  st_widget_set_style_class_name (scroll_view, "dialog-list-scrollview");
  st_scroll_view_set_child (ST_SCROLL_VIEW (scroll_view), ST_SCROLLABLE (list));

  st_widget_set_label_actor (ST_WIDGET (self), CLUTTER_ACTOR (title));
  clutter_actor_add_child (actor, CLUTTER_ACTOR (title));
  clutter_actor_add_child (actor, CLUTTER_ACTOR (scroll_view));

  g_set_weak_pointer (&self->title_label, title);
  g_set_weak_pointer (&self->list, list);
}

/* ======================================================================== */
/* ShellListSectionItem                                                     */
/* ======================================================================== */

enum
{
  ITEM_PROP_0,

  ITEM_PROP_ICON_ACTOR,
  ITEM_PROP_TITLE,
  ITEM_PROP_DESCRIPTION,

  N_ITEM_PROPS
};

static GParamSpec *item_props[N_ITEM_PROPS] = { NULL, };

struct _ShellListSectionItem
{
  StBoxLayout parent;

  StBin *icon_actor_bin; /* weak */
  StLabel *title_label; /* weak */
  StLabel *description_label; /* weak */

  char *title;
  char *description;
};

G_DEFINE_FINAL_TYPE (ShellListSectionItem, shell_list_section_item, ST_TYPE_BOX_LAYOUT)

/**
 * shell_list_section_item_get_icon_actor:
 * @self: a #ShellListSectionItem
 *
 * Returns: (transfer none) (nullable): the icon shown before the text
 */
ClutterActor *
shell_list_section_item_get_icon_actor (ShellListSectionItem *self)
{
  g_return_val_if_fail (SHELL_IS_LIST_SECTION_ITEM (self), NULL);

  if (self->icon_actor_bin == NULL)
    return NULL;

  return st_bin_get_child (self->icon_actor_bin);
}

/**
 * shell_list_section_item_set_icon_actor:
 * @self: a #ShellListSectionItem
 * @icon_actor: (nullable): the icon to show before the text
 */
void
shell_list_section_item_set_icon_actor (ShellListSectionItem *self,
                                        ClutterActor         *icon_actor)
{
  g_return_if_fail (SHELL_IS_LIST_SECTION_ITEM (self));
  g_return_if_fail (icon_actor == NULL || CLUTTER_IS_ACTOR (icon_actor));

  if (self->icon_actor_bin == NULL ||
      st_bin_get_child (self->icon_actor_bin) == icon_actor)
    return;

  st_bin_set_child (self->icon_actor_bin, icon_actor);

  g_object_notify_by_pspec (G_OBJECT (self), item_props[ITEM_PROP_ICON_ACTOR]);
}

/**
 * shell_list_section_item_get_title:
 * @self: a #ShellListSectionItem
 *
 * Returns: (transfer none) (nullable): the title
 */
const char *
shell_list_section_item_get_title (ShellListSectionItem *self)
{
  g_return_val_if_fail (SHELL_IS_LIST_SECTION_ITEM (self), NULL);

  return self->title;
}

/**
 * shell_list_section_item_set_title:
 * @self: a #ShellListSectionItem
 * @title: (nullable): the title; the title label is hidden when %NULL
 */
void
shell_list_section_item_set_title (ShellListSectionItem *self,
                                   const char           *title)
{
  g_return_if_fail (SHELL_IS_LIST_SECTION_ITEM (self));

  if (g_strcmp0 (self->title, title) == 0)
    return;

  g_free (self->title);
  self->title = g_strdup (title);

  if (self->title_label != NULL)
    sync_label (self->title_label, self->title);

  g_object_notify_by_pspec (G_OBJECT (self), item_props[ITEM_PROP_TITLE]);
}

/**
 * shell_list_section_item_get_description:
 * @self: a #ShellListSectionItem
 *
 * Returns: (transfer none) (nullable): the description
 */
const char *
shell_list_section_item_get_description (ShellListSectionItem *self)
{
  g_return_val_if_fail (SHELL_IS_LIST_SECTION_ITEM (self), NULL);

  return self->description;
}

/**
 * shell_list_section_item_set_description:
 * @self: a #ShellListSectionItem
 * @description: (nullable): the description; its label is hidden when %NULL
 */
void
shell_list_section_item_set_description (ShellListSectionItem *self,
                                         const char           *description)
{
  g_return_if_fail (SHELL_IS_LIST_SECTION_ITEM (self));

  if (g_strcmp0 (self->description, description) == 0)
    return;

  g_free (self->description);
  self->description = g_strdup (description);

  if (self->description_label != NULL)
    sync_label (self->description_label, self->description);

  g_object_notify_by_pspec (G_OBJECT (self), item_props[ITEM_PROP_DESCRIPTION]);
}

static void
shell_list_section_item_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  ShellListSectionItem *self = SHELL_LIST_SECTION_ITEM (object);

  switch (prop_id)
    {
    case ITEM_PROP_ICON_ACTOR:
      g_value_set_object (value, shell_list_section_item_get_icon_actor (self));
      break;

    case ITEM_PROP_TITLE:
      g_value_set_string (value, self->title);
      break;

    case ITEM_PROP_DESCRIPTION:
      g_value_set_string (value, self->description);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_list_section_item_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  ShellListSectionItem *self = SHELL_LIST_SECTION_ITEM (object);

  switch (prop_id)
    {
    case ITEM_PROP_ICON_ACTOR:
      shell_list_section_item_set_icon_actor (self, g_value_get_object (value));
      break;

    case ITEM_PROP_TITLE:
      shell_list_section_item_set_title (self, g_value_get_string (value));
      break;

    case ITEM_PROP_DESCRIPTION:
      shell_list_section_item_set_description (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_list_section_item_dispose (GObject *object)
{
  ShellListSectionItem *self = SHELL_LIST_SECTION_ITEM (object);

  G_OBJECT_CLASS (shell_list_section_item_parent_class)->dispose (object);

  g_clear_weak_pointer (&self->icon_actor_bin);
  g_clear_weak_pointer (&self->title_label);
  g_clear_weak_pointer (&self->description_label);
}

static void
shell_list_section_item_finalize (GObject *object)
{
  ShellListSectionItem *self = SHELL_LIST_SECTION_ITEM (object);

  g_clear_pointer (&self->title, g_free);
  g_clear_pointer (&self->description, g_free);

  G_OBJECT_CLASS (shell_list_section_item_parent_class)->finalize (object);
}

static void
shell_list_section_item_class_init (ShellListSectionItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_list_section_item_get_property;
  object_class->set_property = shell_list_section_item_set_property;
  object_class->dispose = shell_list_section_item_dispose;
  object_class->finalize = shell_list_section_item_finalize;

  /**
   * ShellListSectionItem:icon-actor:
   *
   * The icon shown before the text.
   */
  item_props[ITEM_PROP_ICON_ACTOR] =
    g_param_spec_object ("icon-actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellListSectionItem:title:
   *
   * The title of the item. The label is hidden when it is %NULL.
   */
  item_props[ITEM_PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellListSectionItem:description:
   *
   * A second line of text below the title. The label is hidden when it is
   * %NULL.
   */
  item_props[ITEM_PROP_DESCRIPTION] =
    g_param_spec_string ("description", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_ITEM_PROPS, item_props);
}

static void
shell_list_section_item_init (ShellListSectionItem *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  StBin *icon_actor_bin;
  StBoxLayout *text_layout;
  StLabel *title, *description;

  st_widget_set_style_class_name (ST_WIDGET (self), "dialog-list-item");

  icon_actor_bin = ST_BIN (st_bin_new ());

  text_layout = ST_BOX_LAYOUT (st_box_layout_new ());
  st_box_layout_set_orientation (text_layout, CLUTTER_ORIENTATION_VERTICAL);
  clutter_actor_set_y_expand (CLUTTER_ACTOR (text_layout), TRUE);
  clutter_actor_set_y_align (CLUTTER_ACTOR (text_layout), CLUTTER_ACTOR_ALIGN_CENTER);

  /* Unlike the section and message labels, these neither wrap nor ellipsize
   * differently from St's defaults, as before */
  title = ST_LABEL (st_label_new (NULL));
  st_widget_set_style_class_name (ST_WIDGET (title), "dialog-list-item-title");
  sync_label (title, NULL);

  description = ST_LABEL (st_label_new (NULL));
  st_widget_set_style_class_name (ST_WIDGET (description),
                                  "dialog-list-item-title-description");
  sync_label (description, NULL);

  clutter_actor_add_child (CLUTTER_ACTOR (text_layout), CLUTTER_ACTOR (title));
  clutter_actor_add_child (CLUTTER_ACTOR (text_layout), CLUTTER_ACTOR (description));

  st_widget_set_label_actor (ST_WIDGET (self), CLUTTER_ACTOR (title));
  clutter_actor_add_child (actor, CLUTTER_ACTOR (icon_actor_bin));
  clutter_actor_add_child (actor, CLUTTER_ACTOR (text_layout));

  g_set_weak_pointer (&self->icon_actor_bin, icon_actor_bin);
  g_set_weak_pointer (&self->title_label, title);
  g_set_weak_pointer (&self->description_label, description);
}
