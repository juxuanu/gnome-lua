/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * web-login.c: A widget showing a URL (and a QR code of it) for web login,
 * and the dialog around it. Port of js/gdm/webLogin.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The dialog has one button besides Cancel: the one which says the user is
 *    done (`default` and `needsLoading` in the JS `buttons`). It is given to
 *    shell_web_login_dialog_update () with its callback.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>
#include <string.h>

#include <glib/gi18n-lib.h>
#include <pango/pango.h>
#include <clutter/clutter-pango.h>

#include "gdm/web-login.h"
#include "ui/animation.h"
#include "ui/qr-code.h"

/* ------------------------------------------------------------------------ */
/* ShellLabelOverlayLayout                                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LABEL_OVERLAY_LAYOUT (shell_label_overlay_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellLabelOverlayLayout, shell_label_overlay_layout,
                      SHELL, LABEL_OVERLAY_LAYOUT, ClutterLayoutManager)

/* `LabelOverlayLayout`: has the height of its child but no width of its own,
 * and lets the child overflow on both sides if it does not fit */
struct _ShellLabelOverlayLayout
{
  ClutterLayoutManager parent;
};

G_DEFINE_FINAL_TYPE (ShellLabelOverlayLayout, shell_label_overlay_layout,
                     CLUTTER_TYPE_LAYOUT_MANAGER)

static void
label_overlay_get_preferred_width (ClutterLayoutManager *manager,
                                   ClutterActor         *container,
                                   float                 for_height,
                                   float                *min_width_p,
                                   float                *natural_width_p)
{
  if (min_width_p != NULL)
    *min_width_p = 0;
  if (natural_width_p != NULL)
    *natural_width_p = 0;
}

static void
label_overlay_get_preferred_height (ClutterLayoutManager *manager,
                                    ClutterActor         *container,
                                    float                 for_width,
                                    float                *min_height_p,
                                    float                *natural_height_p)
{
  ClutterActor *child = clutter_actor_get_first_child (container);

  if (child != NULL)
    {
      clutter_actor_get_preferred_height (child, for_width, min_height_p, natural_height_p);
      return;
    }

  if (min_height_p != NULL)
    *min_height_p = 0;
  if (natural_height_p != NULL)
    *natural_height_p = 0;
}

static void
label_overlay_allocate (ClutterLayoutManager   *manager,
                        ClutterActor           *container,
                        const ClutterActorBox  *box)
{
  ClutterActor *child = clutter_actor_get_first_child (container);
  ClutterActorBox child_box;
  float natural_width;
  float center;

  if (child == NULL)
    return;

  clutter_actor_get_preferred_width (child, -1, NULL, &natural_width);

  if (natural_width <= clutter_actor_box_get_width (box))
    {
      clutter_actor_allocate (child, box);
      return;
    }

  center = (box->x1 + box->x2) / 2;
  child_box.x1 = floorf (center - natural_width / 2);
  child_box.x2 = floorf (center + natural_width / 2);
  child_box.y1 = box->y1;
  child_box.y2 = box->y2;

  clutter_actor_allocate (child, &child_box);
}

static void
shell_label_overlay_layout_class_init (ShellLabelOverlayLayoutClass *klass)
{
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  layout_class->get_preferred_width = label_overlay_get_preferred_width;
  layout_class->get_preferred_height = label_overlay_get_preferred_height;
  layout_class->allocate = label_overlay_allocate;
}

static void
shell_label_overlay_layout_init (ShellLabelOverlayLayout *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellWebLoginPrompt                                                       */
/* ------------------------------------------------------------------------ */

/**
 * ShellWebLoginPrompt:
 *
 * The instructions, QR code, address and code of a web login
 * (`WebLoginPrompt`).
 */
struct _ShellWebLoginPrompt
{
  StBoxLayout parent;

  StLabel *url_title_label;
  ShellQrCode *qr_code;
  StLabel *url_label;
  ClutterActor *code_box;
  StLabel *code_label;
};

G_DEFINE_FINAL_TYPE (ShellWebLoginPrompt, shell_web_login_prompt, ST_TYPE_BOX_LAYOUT)

/* `_formatURLForDisplay ()` */
static char *
format_url_for_display (const char *url)
{
  g_autofree char *trimmed = NULL;

  if (url == NULL || url[0] == '\0')
    return g_strdup ("http://");

  trimmed = g_strdup (url);
  if (g_str_has_suffix (trimmed, "/"))
    trimmed[strlen (trimmed) - 1] = '\0';

  if (g_str_has_prefix (trimmed, "http://"))
    return g_strdup (trimmed + strlen ("http://"));

  if (g_str_has_prefix (trimmed, "https://"))
    return g_strdup (trimmed + strlen ("https://"));

  return g_steal_pointer (&trimmed);
}

static void
shell_web_login_prompt_constructed (GObject *object)
{
  ShellWebLoginPrompt *self = SHELL_WEB_LOGIN_PROMPT (object);
  ClutterActor *url_label_bin;
  ClutterActor *code_title_label;

  G_OBJECT_CLASS (shell_web_login_prompt_parent_class)->constructed (object);

  self->url_title_label = g_object_new (ST_TYPE_LABEL,
                                        "style-class", "web-login-title-label",
                                        NULL);
  clutter_text_set_line_wrap (st_label_get_clutter_text (self->url_title_label), TRUE);
  clutter_text_set_ellipsize (st_label_get_clutter_text (self->url_title_label),
                              PANGO_ELLIPSIZE_NONE);
  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (self->url_title_label));

  self->qr_code = g_object_new (SHELL_TYPE_QR_CODE,
                                "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (self->qr_code));

  self->url_label = g_object_new (ST_TYPE_LABEL,
                                  "style-class", "web-login-url-label",
                                  "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                  NULL);
  url_label_bin = g_object_new (ST_TYPE_WIDGET,
                                "layout-manager", g_object_new (SHELL_TYPE_LABEL_OVERLAY_LAYOUT, NULL),
                                "x-expand", TRUE,
                                NULL);
  clutter_actor_add_child (url_label_bin, CLUTTER_ACTOR (self->url_label));
  clutter_actor_add_child (CLUTTER_ACTOR (self), url_label_bin);

  self->code_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                 "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                 "x-expand", TRUE,
                                 "visible", FALSE,
                                 NULL);

  code_title_label = g_object_new (ST_TYPE_LABEL,
                                   "text", _("Login code: "),
                                   "style-class", "web-login-code-title-label",
                                   NULL);
  clutter_actor_add_child (self->code_box, code_title_label);

  self->code_label = g_object_new (ST_TYPE_LABEL,
                                   "style-class", "web-login-code-label",
                                   NULL);
  clutter_actor_add_child (self->code_box, CLUTTER_ACTOR (self->code_label));
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->code_box);
}

static void
shell_web_login_prompt_dispose (GObject *object)
{
  ShellWebLoginPrompt *self = SHELL_WEB_LOGIN_PROMPT (object);

  self->url_title_label = NULL;
  self->qr_code = NULL;
  self->url_label = NULL;
  self->code_box = NULL;
  self->code_label = NULL;

  G_OBJECT_CLASS (shell_web_login_prompt_parent_class)->dispose (object);
}

static void
shell_web_login_prompt_class_init (ShellWebLoginPromptClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_web_login_prompt_constructed;
  object_class->dispose = shell_web_login_prompt_dispose;
}

static void
shell_web_login_prompt_init (ShellWebLoginPrompt *self)
{
}

/**
 * shell_web_login_prompt_update:
 * @self: a #ShellWebLoginPrompt
 * @message: (nullable): the instructions, unchanged if %NULL
 * @url: (nullable): the address to open, unchanged if %NULL
 * @code: (nullable): the code to type there, hidden if %NULL
 *
 * Changes what the prompt shows (`update ()`).
 */
void
shell_web_login_prompt_update (ShellWebLoginPrompt *self,
                               const char          *message,
                               const char          *url,
                               const char          *code)
{
  g_return_if_fail (SHELL_IS_WEB_LOGIN_PROMPT (self));

  if (self->url_title_label == NULL)
    return;

  if (message != NULL)
    st_label_set_text (self->url_title_label, message);

  if (url != NULL)
    {
      g_autofree char *formatted_url = format_url_for_display (url);

      shell_qr_code_set_text (self->qr_code, url);
      st_label_set_text (self->url_label, formatted_url);

      if (g_utf8_strlen (formatted_url, -1) > SHELL_WEB_LOGIN_URL_LABEL_LONG_THRESHOLD)
        st_widget_add_style_class_name (ST_WIDGET (self->url_label), "web-login-url-label-long");
      else
        st_widget_remove_style_class_name (ST_WIDGET (self->url_label), "web-login-url-label-long");
    }

  if (code != NULL)
    {
      st_label_set_text (self->code_label, code);
      clutter_actor_show (self->code_box);
    }
  else
    {
      clutter_actor_hide (self->code_box);
    }
}

/**
 * shell_web_login_prompt_new:
 * @qr_size: the size of the QR code
 * @message: (nullable): the instructions
 * @url: (nullable): the address to open
 * @code: (nullable): the code to type there
 *
 * Creates a prompt (`new WebLoginPrompt ()`).
 *
 * Returns: (transfer floating): a new #ShellWebLoginPrompt
 */
ShellWebLoginPrompt *
shell_web_login_prompt_new (int         qr_size,
                            const char *message,
                            const char *url,
                            const char *code)
{
  ShellWebLoginPrompt *self;

  self = g_object_new (SHELL_TYPE_WEB_LOGIN_PROMPT,
                       "style-class", "web-login-prompt",
                       "orientation", CLUTTER_ORIENTATION_VERTICAL,
                       NULL);

  clutter_actor_set_size (CLUTTER_ACTOR (self->qr_code), qr_size, qr_size);
  shell_web_login_prompt_update (self, message, url, code);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellWebLoginDialog                                                       */
/* ------------------------------------------------------------------------ */

/**
 * ShellWebLoginDialog:
 *
 * The web login prompt with a Cancel button and a button which says the user
 * is done (`WebLoginDialog`).
 */
struct _ShellWebLoginDialog
{
  StWidget parent;

  ClutterActor *content_box;
  ShellWebLoginPrompt *web_login_prompt;
  ClutterActor *button_box;
  ClutterActor *cancel_button;
  ClutterActor *spinner_frame;
  ShellSpinner *spinner;

  gboolean is_loading;
};

G_DEFINE_FINAL_TYPE (ShellWebLoginDialog, shell_web_login_dialog, ST_TYPE_WIDGET)

enum
{
  CANCEL,
  LOADING,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/* What a button of the dialog does when it is clicked */
typedef struct
{
  ShellWebLoginDialog *dialog;   /* weak */
  gboolean needs_loading;
  gboolean is_default;
  ShellWebLoginButtonFunc func;
  gpointer user_data;
  GDestroyNotify destroy;
} ButtonData;

static void
button_data_free (ButtonData *data)
{
  g_clear_weak_pointer (&data->dialog);

  if (data->destroy != NULL)
    data->destroy (data->user_data);

  g_free (data);
}

static void
start_loading (ShellWebLoginDialog *self)
{
  g_autoptr (GList) buttons = clutter_actor_get_children (self->button_box);
  GList *l;

  clutter_actor_set_opacity (CLUTTER_ACTOR (self->web_login_prompt), 128);

  for (l = buttons; l != NULL; l = l->next)
    {
      if (l->data == self->cancel_button)
        continue;

      clutter_actor_set_opacity (l->data, 128);
      st_widget_set_can_focus (l->data, FALSE);
      clutter_actor_set_reactive (l->data, FALSE);
    }
  clutter_actor_grab_key_focus (self->cancel_button);

  clutter_actor_show (self->spinner_frame);
  shell_spinner_play (self->spinner);

  self->is_loading = TRUE;
  g_signal_emit (self, signals[LOADING], 0);
}

static void
button_clicked_cb (StButton   *button,
                   int         clicked_button,
                   ButtonData *data)
{
  if (data->dialog == NULL)
    return;

  if (data->needs_loading)
    start_loading (data->dialog);

  if (data->func != NULL)
    data->func (data->user_data);
}

static void
cancel_clicked_cb (StButton   *button,
                   int         clicked_button,
                   ButtonData *data)
{
  if (data->dialog != NULL)
    g_signal_emit (data->dialog, signals[CANCEL], 0);
}

/* `_addButton ()` */
static ClutterActor *
add_button (ShellWebLoginDialog *self,
            const char          *label,
            ButtonData          *data,
            GCallback            clicked_cb)
{
  ClutterActor *button;

  button = g_object_new (ST_TYPE_BUTTON,
                         "style-class", "web-login-prompt-button",
                         "can-focus", TRUE,
                         "accessible-name", label,
                         "button-mask", ST_BUTTON_PRIMARY | ST_BUTTON_SECONDARY,
                         "x-expand", TRUE,
                         NULL);
  st_bin_set_child (ST_BIN (button),
                    g_object_new (ST_TYPE_LABEL,
                                  "text", label,
                                  "style-class", "login-button-label",
                                  NULL));

  g_set_weak_pointer (&data->dialog, self);
  g_signal_connect (button, "clicked", clicked_cb, data);
  g_object_set_data_full (G_OBJECT (button), "shell-web-login-button", data,
                          (GDestroyNotify) button_data_free);

  clutter_actor_add_child (self->button_box, button);

  g_object_bind_property (self, "reactive", button, "reactive", G_BINDING_SYNC_CREATE);
  g_object_bind_property (self, "reactive", button, "can-focus", G_BINDING_SYNC_CREATE);

  return button;
}

/* `_updateButtons ()` */
static void
update_buttons (ShellWebLoginDialog *self,
                const char          *done_label,
                ShellWebLoginButtonFunc done_func,
                gpointer             user_data,
                GDestroyNotify       destroy)
{
  ButtonData *cancel_data = g_new0 (ButtonData, 1);

  clutter_actor_destroy_all_children (self->button_box);

  self->cancel_button = add_button (self, _("Cancel"), cancel_data, G_CALLBACK (cancel_clicked_cb));

  if (done_label != NULL)
    {
      ButtonData *done_data = g_new0 (ButtonData, 1);

      done_data->needs_loading = TRUE;
      done_data->is_default = TRUE;
      done_data->func = done_func;
      done_data->user_data = user_data;
      done_data->destroy = destroy;

      add_button (self, done_label, done_data, G_CALLBACK (button_clicked_cb));
    }
  else if (destroy != NULL)
    {
      destroy (user_data);
    }
}

static void
shell_web_login_dialog_key_focus_in (ClutterActor *actor)
{
  ShellWebLoginDialog *self = SHELL_WEB_LOGIN_DIALOG (actor);
  g_autoptr (GList) buttons = NULL;
  GList *l;

  CLUTTER_ACTOR_CLASS (shell_web_login_dialog_parent_class)->key_focus_in (actor);

  if (self->button_box == NULL)
    return;

  buttons = clutter_actor_get_children (self->button_box);
  for (l = buttons; l != NULL; l = l->next)
    {
      ButtonData *data = g_object_get_data (l->data, "shell-web-login-button");

      if (data != NULL && data->is_default)
        {
          clutter_actor_grab_key_focus (l->data);
          return;
        }
    }
}

/**
 * shell_web_login_dialog_stop_loading:
 * @self: a #ShellWebLoginDialog
 *
 * Hides the spinner and makes the buttons usable again (`stopLoading ()`).
 * Emits #ShellWebLoginDialog::loading.
 */
void
shell_web_login_dialog_stop_loading (ShellWebLoginDialog *self)
{
  g_autoptr (GList) buttons = NULL;
  GList *l;

  g_return_if_fail (SHELL_IS_WEB_LOGIN_DIALOG (self));

  if (self->button_box == NULL)
    return;

  clutter_actor_set_opacity (CLUTTER_ACTOR (self->web_login_prompt), 255);

  buttons = clutter_actor_get_children (self->button_box);
  for (l = buttons; l != NULL; l = l->next)
    {
      gboolean reactive = clutter_actor_get_reactive (CLUTTER_ACTOR (self));

      if (l->data == self->cancel_button)
        continue;

      clutter_actor_set_opacity (l->data, 255);
      st_widget_set_can_focus (l->data, reactive);
      clutter_actor_set_reactive (l->data, reactive);
    }

  clutter_actor_hide (self->spinner_frame);
  shell_spinner_stop (self->spinner);

  self->is_loading = FALSE;
  g_signal_emit (self, signals[LOADING], 0);
}

/**
 * shell_web_login_dialog_get_is_loading:
 * @self: a #ShellWebLoginDialog
 *
 * Returns: whether the dialog waits for the service (`isLoading`)
 */
gboolean
shell_web_login_dialog_get_is_loading (ShellWebLoginDialog *self)
{
  g_return_val_if_fail (SHELL_IS_WEB_LOGIN_DIALOG (self), FALSE);

  return self->is_loading;
}

/**
 * shell_web_login_dialog_update:
 * @self: a #ShellWebLoginDialog
 * @message: (nullable): the instructions, unchanged if %NULL
 * @url: (nullable): the address to open, unchanged if %NULL
 * @code: (nullable): the code to type there, hidden if %NULL
 * @done_label: (nullable): the label of the button which says the user is
 *   done, there is only a Cancel button without it. It is the default button
 *   and shows the spinner when it is clicked
 * @done_func: (nullable): called when that button is clicked
 * @user_data: (closure done_func): data for @done_func
 * @destroy: (nullable): called on @user_data when the buttons are replaced
 *
 * Changes what the dialog shows (`update ()`). Stops the spinner.
 */
void
shell_web_login_dialog_update (ShellWebLoginDialog     *self,
                               const char              *message,
                               const char              *url,
                               const char              *code,
                               const char              *done_label,
                               ShellWebLoginButtonFunc  done_func,
                               gpointer                 user_data,
                               GDestroyNotify           destroy)
{
  g_return_if_fail (SHELL_IS_WEB_LOGIN_DIALOG (self));

  if (self->button_box == NULL)
    {
      if (destroy != NULL)
        destroy (user_data);
      return;
    }

  shell_web_login_dialog_stop_loading (self);

  shell_web_login_prompt_update (self->web_login_prompt, message, url, code);

  update_buttons (self, done_label, done_func, user_data, destroy);
}

static gboolean
reactive_to_opacity_cb (GBinding     *binding,
                        const GValue *from_value,
                        GValue       *to_value,
                        gpointer      user_data)
{
  g_value_set_uchar (to_value, g_value_get_boolean (from_value) ? 255 : 128);
  return TRUE;
}

static void
shell_web_login_dialog_constructed (GObject *object)
{
  ShellWebLoginDialog *self = SHELL_WEB_LOGIN_DIALOG (object);

  G_OBJECT_CLASS (shell_web_login_dialog_parent_class)->constructed (object);

  g_object_bind_property_full (self, "reactive",
                               self, "opacity",
                               G_BINDING_SYNC_CREATE,
                               reactive_to_opacity_cb, NULL, NULL, NULL);

  self->content_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                    "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                    "x-expand", TRUE,
                                    NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->content_box);

  self->web_login_prompt = shell_web_login_prompt_new (SHELL_WEB_LOGIN_QR_CODE_SIZE,
                                                       NULL, NULL, NULL);
  clutter_actor_add_child (self->content_box, CLUTTER_ACTOR (self->web_login_prompt));

  self->button_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                   "orientation", CLUTTER_ORIENTATION_HORIZONTAL,
                                   "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                   "x-expand", TRUE,
                                   NULL);
  clutter_box_layout_set_homogeneous (CLUTTER_BOX_LAYOUT (clutter_actor_get_layout_manager (self->button_box)),
                                      TRUE);
  clutter_actor_add_child (self->content_box, self->button_box);

  update_buttons (self, NULL, NULL, NULL, NULL);

  self->spinner_frame = g_object_new (ST_TYPE_WIDGET,
                                      "layout-manager", clutter_bin_layout_new (),
                                      "style-class", "web-login-spinner",
                                      NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->spinner_frame);

  self->spinner = shell_spinner_new (SHELL_WEB_LOGIN_SPINNER_SIZE, FALSE, TRUE);
  clutter_actor_add_child (self->spinner_frame, CLUTTER_ACTOR (self->spinner));
}

static void
shell_web_login_dialog_dispose (GObject *object)
{
  ShellWebLoginDialog *self = SHELL_WEB_LOGIN_DIALOG (object);

  self->content_box = NULL;
  self->web_login_prompt = NULL;
  self->button_box = NULL;
  self->cancel_button = NULL;
  self->spinner_frame = NULL;
  self->spinner = NULL;

  G_OBJECT_CLASS (shell_web_login_dialog_parent_class)->dispose (object);
}

static void
shell_web_login_dialog_class_init (ShellWebLoginDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_web_login_dialog_constructed;
  object_class->dispose = shell_web_login_dialog_dispose;

  actor_class->key_focus_in = shell_web_login_dialog_key_focus_in;

  /**
   * ShellWebLoginDialog::cancel:
   * @self: the #ShellWebLoginDialog
   *
   * Emitted when the user clicks the Cancel button.
   */
  signals[CANCEL] =
    g_signal_new ("cancel",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellWebLoginDialog::loading:
   * @self: the #ShellWebLoginDialog
   *
   * Emitted when the dialog starts or stops waiting for the service. See
   * shell_web_login_dialog_get_is_loading ().
   */
  signals[LOADING] =
    g_signal_new ("loading",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_web_login_dialog_init (ShellWebLoginDialog *self)
{
}

/**
 * shell_web_login_dialog_new:
 *
 * Creates a hidden dialog (`new WebLoginDialog ()`).
 *
 * Returns: (transfer floating): a new #ShellWebLoginDialog
 */
ShellWebLoginDialog *
shell_web_login_dialog_new (void)
{
  return g_object_new (SHELL_TYPE_WEB_LOGIN_DIALOG,
                       "layout-manager", clutter_bin_layout_new (),
                       "x-expand", TRUE,
                       "y-expand", TRUE,
                       "visible", FALSE,
                       NULL);
}
