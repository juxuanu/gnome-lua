/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auth-prompt.c: The widget of the login screen and of the lock screen which
 * asks the user to authenticate, on top of #ShellGdmUserVerifier. Port of
 * js/gdm/authPrompt.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - ::mechanisms-changed has the two `GVariant`s of the verifier (an
 *    `aa{sv}` and an `a{sv}`), not a JS object.
 *  - The mechanisms are `a{sv}` variants: shell_auth_prompt_select_mechanism ()
 *    reads their `serviceName` and `role`.
 *  - A web login has a single button besides Cancel (see web-login.h).
 *  - Completing a prompt that has no pending request (the choice list of a
 *    message) is a debug message; the JS threw an error.
 *  - The answers (passwords) the prompt keeps are wiped from memory when they
 *    are dropped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <atk/atk.h>
#include <glib/gi18n-lib.h>
#include <pango/pango.h>
#include <clutter/clutter-pango.h>
#include <string.h>

#include "gdm/auth-list.h"
#include "gdm/auth-prompt.h"
#include "gdm/web-login.h"
#include "malcontent-timer-dbus.h"
#include "ui/animation.h"
#include "ui/environment.h"
#include "ui/shell-entry.h"
#include "ui/user-widget.h"

#define TIMER_BUS_NAME "org.freedesktop.MalcontentTimer1"
#define TIMER_OBJECT_PATH "/org/freedesktop/MalcontentTimer1"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static ShellEaseParams
ease_params (int                  duration,
             int                  delay,
             ClutterAnimationMode mode)
{
  ShellEaseParams params = {
    .duration = duration,
    .delay = delay,
    .mode = mode,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  return params;
}

static void
set_actor_visible (ClutterActor *actor,
                   gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

/* Frees a string which may hold a password */
static void
free_secret (char **secret)
{
  if (*secret == NULL)
    return;

  explicit_bzero (*secret, strlen (*secret));
  g_clear_pointer (secret, g_free);
}

/* ------------------------------------------------------------------------ */
/* ShellAuthPromptParentalControlsShield                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_AUTH_PROMPT_PARENTAL_CONTROLS_SHIELD (shell_auth_prompt_parental_controls_shield_get_type ())
G_DECLARE_FINAL_TYPE (ShellAuthPromptParentalControlsShield,
                      shell_auth_prompt_parental_controls_shield,
                      SHELL, AUTH_PROMPT_PARENTAL_CONTROLS_SHIELD, StBoxLayout)

/* `ParentalControlsShield`: a widget displayed instead of the unlock prompt
 * when parental controls session limits are reached */
struct _ShellAuthPromptParentalControlsShield
{
  StBoxLayout parent;

  ShellMalcontentTimer1Child *timer_child_proxy;
  GCancellable *cancellable;
  char *request_extension_cookie;
};

G_DEFINE_FINAL_TYPE (ShellAuthPromptParentalControlsShield,
                     shell_auth_prompt_parental_controls_shield,
                     ST_TYPE_BOX_LAYOUT)

static void
shield_on_extension_response (ShellMalcontentTimer1Child *proxy,
                              gboolean                    granted,
                              const char                 *cookie,
                              GVariant                   *extra_data,
                              ShellAuthPromptParentalControlsShield *self)
{
  if (self->request_extension_cookie == NULL ||
      g_strcmp0 (cookie, self->request_extension_cookie) != 0)
    return;

  g_clear_pointer (&self->request_extension_cookie, g_free);
}

static void
shield_proxy_ready_cb (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  g_autoptr (ShellAuthPromptParentalControlsShield) self = user_data;
  g_autoptr (GError) error = NULL;
  ShellMalcontentTimer1Child *proxy;

  proxy = shell_malcontent_timer1_child_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Failed to get TimerChild proxy: %s", error->message);
      return;
    }

  self->timer_child_proxy = proxy;
  g_signal_connect_object (proxy, "extension-response",
                           G_CALLBACK (shield_on_extension_response), self, 0);
}

static void
shield_request_extension_cb (GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  g_autoptr (ShellAuthPromptParentalControlsShield) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) result = NULL;
  const char *cookie = NULL;

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), res, &error);
  if (result == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Failed to obtain screen time extension: %s", error->message);
      return;
    }

  g_variant_get (result, "(&o)", &cookie);
  g_free (self->request_extension_cookie);
  self->request_extension_cookie = g_strdup (cookie);
}

static void
shield_ignore_clicked_cb (StButton *button,
                          int       clicked_button,
                          ShellAuthPromptParentalControlsShield *self)
{
  GVariantBuilder extra_data;

  if (self->request_extension_cookie != NULL || self->timer_child_proxy == NULL)
    return;

  g_variant_builder_init (&extra_data, G_VARIANT_TYPE_VARDICT);
  g_dbus_proxy_call (G_DBUS_PROXY (self->timer_child_proxy),
                     "RequestExtension",
                     g_variant_new ("(ssta{sv})", "login-session", "", (guint64) 0, &extra_data),
                     G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                     -1,
                     self->cancellable,
                     shield_request_extension_cb,
                     g_object_ref (self));
}

static void
shell_auth_prompt_parental_controls_shield_constructed (GObject *object)
{
  ShellAuthPromptParentalControlsShield *self = SHELL_AUTH_PROMPT_PARENTAL_CONTROLS_SHIELD (object);
  ClutterActor *title_label;
  ClutterActor *description_label;
  ClutterActor *ignore_button;

  G_OBJECT_CLASS (shell_auth_prompt_parental_controls_shield_parent_class)->constructed (object);

  self->cancellable = g_cancellable_new ();
  shell_malcontent_timer1_child_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                                   G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START_AT_CONSTRUCTION,
                                                   TIMER_BUS_NAME,
                                                   TIMER_OBJECT_PATH,
                                                   self->cancellable,
                                                   shield_proxy_ready_cb,
                                                   g_object_ref (self));

  title_label = g_object_new (ST_TYPE_LABEL,
                              "style-class", "parental-controls-shield-title",
                              "text", _("Screen Time Limit Reached"),
                              NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), title_label);

  description_label = g_object_new (ST_TYPE_LABEL,
                                    "style-class", "parental-controls-shield-description",
                                    "text", _("Daily limit for screen time on this device has been reached. Resume tomorrow."),
                                    NULL);
  clutter_text_set_line_wrap (st_label_get_clutter_text (ST_LABEL (description_label)), TRUE);
  clutter_actor_add_child (CLUTTER_ACTOR (self), description_label);

  ignore_button = g_object_new (ST_TYPE_BUTTON,
                                "style-class", "parental-controls-shield-button",
                                /* Translators: this is for ignoring a screen time limit for parental controls */
                                "label", _("Ignore"),
                                "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                NULL);
  g_signal_connect (ignore_button, "clicked",
                    G_CALLBACK (shield_ignore_clicked_cb), self);
  clutter_actor_add_child (CLUTTER_ACTOR (self), ignore_button);
}

static void
shell_auth_prompt_parental_controls_shield_dispose (GObject *object)
{
  ShellAuthPromptParentalControlsShield *self = SHELL_AUTH_PROMPT_PARENTAL_CONTROLS_SHIELD (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->timer_child_proxy);
  g_clear_pointer (&self->request_extension_cookie, g_free);

  G_OBJECT_CLASS (shell_auth_prompt_parental_controls_shield_parent_class)->dispose (object);
}

static void
shell_auth_prompt_parental_controls_shield_class_init (ShellAuthPromptParentalControlsShieldClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_auth_prompt_parental_controls_shield_constructed;
  object_class->dispose = shell_auth_prompt_parental_controls_shield_dispose;
}

static void
shell_auth_prompt_parental_controls_shield_init (ShellAuthPromptParentalControlsShield *self)
{
}

static ClutterActor *
parental_controls_shield_new (void)
{
  return g_object_new (SHELL_TYPE_AUTH_PROMPT_PARENTAL_CONTROLS_SHIELD,
                       "style-class", "parental-controls-shield",
                       "orientation", CLUTTER_ORIENTATION_VERTICAL,
                       "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellAuthPrompt                                                           */
/* ------------------------------------------------------------------------ */

/**
 * ShellAuthPrompt:
 *
 * The prompt of the login and unlock screens (`AuthPrompt`). It shows the
 * widgets that go with what the verifier asks: an entry, a list of choices,
 * a button or a web login.
 */
struct _ShellAuthPrompt
{
  StBoxLayout parent;

  GdmClient *client;
  ShellAuthPromptMode mode;
  ShellGdmUserVerifier *verifier;

  ShellAuthPromptStatus verification_status;
  guint prompt_step;

  ClutterActor *user_well;
  ClutterActor *input_well;        /* owned: it may not be a child */
  ClutterActor *main_content;
  ClutterActor *main_box;
  StButton *cancel_button;
  ShellAuthList *auth_list;
  gulong auth_list_activate_id;
  ClutterActor *auth_list_title;
  StLabel *auth_list_title_label;
  ClutterActor *entry_area;
  StEntry *entry;
  StEntry *text_entry;             /* owned: they may not be children */
  StEntry *password_entry;
  StEntry *inactive_entry;
  ClutterActor *timed_login_indicator;
  ClutterActor *default_button_well;
  ClutterActor *default_button_well_actor;
  StButton *next_button;
  ShellSpinner *spinner;
  StButton *auth_button;
  ShellWebLoginDialog *web_login_dialog;
  ClutterActor *caps_lock_warning;
  StLabel *message;
  ClutterActor *parental_controls_shield;   /* owned */

  guint timed_login_timeout_id;
  int cancelled_retries;

  char *querying_service;
  char *preemptive_answer;
  gboolean preemptive_input;
  ShellGdmAuthHandler *pending_handler;
};

G_DEFINE_FINAL_TYPE (ShellAuthPrompt, shell_auth_prompt, ST_TYPE_BOX_LAYOUT)

enum
{
  PROP_0,
  PROP_CLIENT,
  PROP_MODE,
  PROP_VERIFICATION_STATUS,
  PROP_PROMPT_STEP,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  CANCELLED,
  FAILED,
  NEXT,
  PROMPTED,
  MECHANISMS_CHANGED,
  RESET,
  VERIFICATION_COMPLETE,
  LOADING,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

static void shell_auth_prompt_handle_cancel (ShellAuthPrompt *self);

/* ---- status and step ---- */

static void
set_verification_status (ShellAuthPrompt       *self,
                         ShellAuthPromptStatus  status)
{
  if (self->verification_status == status)
    return;

  self->verification_status = status;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_VERIFICATION_STATUS]);
}

static void
set_prompt_step (ShellAuthPrompt *self,
                 guint            step)
{
  if (self->prompt_step == step)
    return;

  self->prompt_step = step;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PROMPT_STEP]);
}

/* `_updateCancelButton ()` */
static void
update_cancel_button (ShellAuthPrompt *self)
{
  gboolean cancel_visible;

  if (self->mode == SHELL_AUTH_PROMPT_MODE_UNLOCK_OR_LOG_IN ||
      self->cancel_button == NULL)
    return;

  cancel_visible = self->prompt_step > 1;
  clutter_actor_set_opacity (CLUTTER_ACTOR (self->cancel_button), cancel_visible ? 255 : 0);
  clutter_actor_set_reactive (CLUTTER_ACTOR (self->cancel_button), cancel_visible);
}

static void
prompt_step_notify_cb (GObject    *object,
                       GParamSpec *pspec,
                       gpointer    user_data)
{
  update_cancel_button (SHELL_AUTH_PROMPT (object));
}

/* ---- fades ---- */

/* `_fadeInElement ()` */
static void
fade_in_element (ClutterActor *element)
{
  ShellEaseParams params = ease_params (SHELL_AUTH_PROMPT_MESSAGE_FADE_OUT_ANIMATION_TIME,
                                        -1, CLUTTER_EASE_OUT_QUAD);

  if (clutter_actor_is_visible (element))
    return;

  clutter_actor_set_opacity (element, 0);
  clutter_actor_show (element);
  shell_actor_ease (element, &params, NULL, NULL, NULL,
                    "opacity", 255u,
                    NULL);
}

/* `_fadeOutMessage ()` */
static void
fade_out_message (ShellAuthPrompt *self)
{
  ShellEaseParams params = ease_params (SHELL_AUTH_PROMPT_MESSAGE_FADE_OUT_ANIMATION_TIME,
                                        -1, CLUTTER_EASE_OUT_QUAD);
  ClutterActor *message = CLUTTER_ACTOR (self->message);

  if (clutter_actor_get_opacity (message) == 0)
    return;

  clutter_actor_remove_all_transitions (message);
  shell_actor_ease (message, &params, NULL, NULL, NULL,
                    "opacity", 0u,
                    NULL);
}

/* ---- the default button well ---- */

static void
well_ease_done_cb (gboolean finished,
                   gpointer user_data)
{
  ShellAuthPrompt *self = SHELL_AUTH_PROMPT (user_data);

  if (finished && self->spinner != NULL)
    shell_spinner_stop (self->spinner);
}

/* `setActorInDefaultButtonWell ()` */
static void
set_actor_in_default_button_well (ShellAuthPrompt *self,
                                  ClutterActor    *actor,
                                  gboolean         animate)
{
  ClutterActor *old_actor = self->default_button_well_actor;
  gboolean was_spinner;
  ShellEaseParams params;

  if (old_actor == NULL && actor == NULL)
    return;

  was_spinner = old_actor != NULL && old_actor == CLUTTER_ACTOR (self->spinner);

  if (old_actor != NULL)
    clutter_actor_remove_all_transitions (old_actor);

  if (actor != NULL && actor == CLUTTER_ACTOR (self->spinner))
    shell_spinner_play (self->spinner);

  if (!animate)
    {
      if (old_actor != NULL)
        {
          clutter_actor_set_opacity (old_actor, 0);
          if (was_spinner)
            shell_spinner_stop (self->spinner);
        }
      if (actor != NULL)
        clutter_actor_set_opacity (actor, 255);

      self->default_button_well_actor = actor;
      return;
    }

  if (old_actor != NULL)
    {
      params = ease_params (SHELL_AUTH_PROMPT_DEFAULT_BUTTON_WELL_ANIMATION_TIME,
                            -1, CLUTTER_LINEAR);

      clutter_actor_set_opacity (old_actor, 255);
      if (was_spinner)
        shell_actor_ease (old_actor, &params,
                          well_ease_done_cb, g_object_ref (self), g_object_unref,
                          "opacity", 0u,
                          NULL);
      else
        shell_actor_ease (old_actor, &params, NULL, NULL, NULL,
                          "opacity", 0u,
                          NULL);
    }

  if (actor != NULL)
    {
      params = ease_params (SHELL_AUTH_PROMPT_DEFAULT_BUTTON_WELL_ANIMATION_TIME,
                            old_actor != NULL ? SHELL_AUTH_PROMPT_DEFAULT_BUTTON_WELL_ANIMATION_TIME : 0,
                            CLUTTER_LINEAR);

      clutter_actor_set_opacity (actor, 0);
      shell_actor_ease (actor, &params, NULL, NULL, NULL,
                        "opacity", 255u,
                        NULL);
    }

  self->default_button_well_actor = actor;
}

/* `startSpinning ()` */
static void
start_spinning (ShellAuthPrompt *self,
                gboolean         animate)
{
  g_signal_emit (self, signals[LOADING], 0, TRUE);
  set_actor_in_default_button_well (self, CLUTTER_ACTOR (self->spinner), animate);
}

/* `stopSpinning ()` */
static void
stop_spinning (ShellAuthPrompt *self,
               gboolean         animate)
{
  g_signal_emit (self, signals[LOADING], 0, FALSE);
  set_actor_in_default_button_well (self, CLUTTER_ACTOR (self->next_button), animate);

  if (self->web_login_dialog != NULL &&
      shell_web_login_dialog_get_is_loading (self->web_login_dialog))
    shell_web_login_dialog_stop_loading (self->web_login_dialog);
}

/* ---- the pending request ---- */

/* What `_completePendingCallback ()` waits for the messages to finish with */
typedef struct
{
  ShellGdmAuthHandler *handler;
  char *argument;
} PendingData;

static void
pending_data_free (PendingData *data)
{
  g_clear_object (&data->handler);
  free_secret (&data->argument);
  g_free (data);
}

static void
pending_messages_handled_cb (GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  PendingData *data = user_data;
  g_autoptr (GError) error = NULL;

  if (!shell_gdm_user_verifier_handle_pending_messages_finish (SHELL_GDM_USER_VERIFIER (source),
                                                               res, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("%s: %s", G_STRFUNC, error->message);
    }
  else
    {
      shell_gdm_auth_handler_invoke (data->handler, data->argument);
    }

  pending_data_free (data);
}

/* `_setPendingCallback ()`; @handler may be %NULL (a choice list which is
 * only a message) */
static void
set_pending_handler (ShellAuthPrompt    *self,
                     ShellGdmAuthHandler *handler)
{
  if (self->pending_handler != NULL)
    {
      g_warning ("%s: A pending request is already active", G_STRFUNC);
      g_clear_object (&self->pending_handler);
    }

  if (handler != NULL)
    self->pending_handler = g_object_ref (handler);
}

/* `_completePendingCallback (argument)` */
static void
complete_pending_callback (ShellAuthPrompt *self,
                           const char      *argument)
{
  PendingData *data;

  if (self->pending_handler == NULL)
    {
      g_debug ("%s: No pending request to complete", G_STRFUNC);
      return;
    }

  if (self->verifier == NULL)
    return;

  data = g_new0 (PendingData, 1);
  data->handler = g_steal_pointer (&self->pending_handler);
  data->argument = g_strdup (argument);

  shell_gdm_user_verifier_handle_pending_messages_async (self->verifier,
                                                         pending_messages_handled_cb,
                                                         data);
}

/* `_clearPreemptiveState ()` */
static void
clear_preemptive_state (ShellAuthPrompt *self)
{
  self->preemptive_input = FALSE;
  free_secret (&self->preemptive_answer);
}

/* ---- the entries ---- */

/* `updateEntry ()` */
void
shell_auth_prompt_update_entry (ShellAuthPrompt *self,
                                gboolean         secret)
{
  StEntry *new_entry = NULL;
  StEntry *inactive_entry = NULL;

  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));

  if (self->entry == NULL)
    return;

  if (secret && self->entry != self->password_entry)
    {
      new_entry = self->password_entry;
      inactive_entry = self->text_entry;
    }
  else if (!secret && self->entry != self->text_entry)
    {
      new_entry = self->text_entry;
      inactive_entry = self->password_entry;
    }

  if (new_entry != NULL)
    {
      ClutterText *old_text = st_entry_get_clutter_text (inactive_entry);
      ClutterText *new_text = st_entry_get_clutter_text (new_entry);
      g_autofree char *text = g_strdup (clutter_text_get_text (old_text));
      int cursor_position = clutter_text_get_cursor_position (old_text);
      int selection_bound = clutter_text_get_selection_bound (old_text);

      clutter_actor_replace_child (self->entry_area,
                                   CLUTTER_ACTOR (self->entry),
                                   CLUTTER_ACTOR (new_entry));
      self->entry = new_entry;
      self->inactive_entry = inactive_entry;

      clutter_text_set_text (new_text, text);
      clutter_text_set_cursor_position (new_text, cursor_position);
      clutter_text_set_selection_bound (new_text, selection_bound);

      if (text != NULL)
        explicit_bzero (text, strlen (text));
    }

  set_actor_visible (self->caps_lock_warning, secret);
}

/* `updateSensitivity ()` */
void
shell_auth_prompt_update_sensitivity (ShellAuthPrompt *self,
                                      gboolean         sensitive)
{
  ClutterActor *auth_widget = NULL;

  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));

  if (self->entry == NULL)
    return;

  if (sensitive && self->preemptive_answer != NULL && self->preemptive_answer[0] != '\0')
    return;

  if (clutter_actor_is_visible (CLUTTER_ACTOR (self->auth_list)))
    auth_widget = CLUTTER_ACTOR (self->auth_list);
  else if (clutter_actor_is_visible (CLUTTER_ACTOR (self->auth_button)))
    auth_widget = CLUTTER_ACTOR (self->auth_button);
  else if (clutter_actor_is_visible (CLUTTER_ACTOR (self->web_login_dialog)))
    auth_widget = CLUTTER_ACTOR (self->web_login_dialog);
  else
    auth_widget = CLUTTER_ACTOR (self->entry);

  if (auth_widget == CLUTTER_ACTOR (self->entry))
    clutter_actor_set_reactive (CLUTTER_ACTOR (self->next_button), sensitive);

  clutter_actor_set_reactive (auth_widget, sensitive);

  if (sensitive)
    {
      clutter_actor_grab_key_focus (auth_widget);
    }
  else
    {
      clutter_actor_grab_key_focus (CLUTTER_ACTOR (self));

      if (auth_widget == CLUTTER_ACTOR (self->password_entry))
        st_password_entry_set_password_visible (ST_PASSWORD_ENTRY (auth_widget), FALSE);
    }
}

/* `_activateNext ()` */
static void
activate_next (ShellAuthPrompt *self)
{
  if (!clutter_actor_get_reactive (CLUTTER_ACTOR (self->entry)))
    return;

  set_verification_status (self, SHELL_AUTH_PROMPT_STATUS_VERIFICATION_IN_PROGRESS);
  shell_auth_prompt_update_sensitivity (self, FALSE);

  if (self->entry == self->password_entry)
    start_spinning (self, TRUE);

  if (self->querying_service != NULL)
    {
      complete_pending_callback (self, st_entry_get_text (self->entry));
    }
  else
    {
      free_secret (&self->preemptive_answer);
      self->preemptive_answer = g_strdup (st_entry_get_text (self->entry));
    }

  self->preemptive_input = FALSE;

  g_signal_emit (self, signals[NEXT], 0);
}

static void
entry_text_changed_cb (ClutterText     *text,
                       ShellAuthPrompt *self)
{
  if (self->verifier == NULL ||
      !shell_gdm_user_verifier_get_has_pending_messages (self->verifier))
    fade_out_message (self);
}

static void
entry_activate_cb (ClutterText     *text,
                   ShellAuthPrompt *self)
{
  activate_next (self);
}

/* ---- the user ---- */

/**
 * shell_auth_prompt_set_user:
 * @self: a #ShellAuthPrompt
 * @user: (nullable): the user to show
 *
 * Shows the avatar and the name of @user (`setUser ()`).
 */
void
shell_auth_prompt_set_user (ShellAuthPrompt *self,
                            ActUser         *user)
{
  ClutterActor *old_child;

  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));
  g_return_if_fail (user == NULL || ACT_IS_USER (user));

  if (self->user_well == NULL)
    return;

  old_child = st_bin_get_child (ST_BIN (self->user_well));
  if (old_child != NULL)
    clutter_actor_destroy (old_child);

  st_bin_set_child (ST_BIN (self->user_well),
                    CLUTTER_ACTOR (shell_user_widget_new (user, CLUTTER_ORIENTATION_VERTICAL)));
}

static void
set_user_avatar_visible (ShellAuthPrompt *self,
                         gboolean         visible)
{
  ClutterActor *child = st_bin_get_child (ST_BIN (self->user_well));

  if (!SHELL_IS_USER_WIDGET (child))
    return;

  if (visible)
    shell_user_widget_show_avatar (SHELL_USER_WIDGET (child));
  else
    shell_user_widget_hide_avatar (SHELL_USER_WIDGET (child));
}

/* ---- the web login ---- */

/* `_closeWebLoginDialog ()` */
static void
close_web_login_dialog (ShellAuthPrompt *self)
{
  clutter_actor_hide (CLUTTER_ACTOR (self->web_login_dialog));

  set_user_avatar_visible (self, TRUE);
  clutter_actor_show (self->main_box);
}

static void
web_login_done_cb (gpointer user_data)
{
  shell_gdm_auth_handler_invoke (SHELL_GDM_AUTH_HANDLER (user_data), NULL);
}

/* ---- the auth list ---- */

/* What the list gives back once it faded out */
typedef struct
{
  ShellAuthPrompt *prompt;
  char *key;
} AuthListActivation;

static void
auth_list_activation_free (AuthListActivation *activation)
{
  g_clear_object (&activation->prompt);
  g_free (activation->key);
  g_free (activation);
}

static void
auth_list_faded_out_cb (gboolean finished,
                        gpointer user_data)
{
  AuthListActivation *activation = user_data;
  ShellAuthPrompt *self = activation->prompt;

  if (!finished || self->auth_list == NULL)
    return;

  st_label_set_text (self->auth_list_title_label, "");
  shell_auth_list_clear (self->auth_list);
  clutter_actor_hide (CLUTTER_ACTOR (self->auth_list));
  complete_pending_callback (self, activation->key);
}

static void
auth_list_activate_cb (ShellAuthList   *list,
                       const char      *key,
                       ShellAuthPrompt *self)
{
  ShellEaseParams params = ease_params (SHELL_AUTH_PROMPT_MESSAGE_FADE_OUT_ANIMATION_TIME * 0.5,
                                        -1, CLUTTER_EASE_OUT_QUAD);
  AuthListActivation *activation = g_new0 (AuthListActivation, 1);

  activation->prompt = g_object_ref (self);
  activation->key = g_strdup (key);

  clutter_actor_set_reactive (CLUTTER_ACTOR (list), FALSE);

  shell_actor_ease (CLUTTER_ACTOR (list), &params,
                    auth_list_faded_out_cb, activation,
                    (GDestroyNotify) auth_list_activation_free,
                    "opacity", 0u,
                    NULL);
}

/* `_connectAuthListActivate ()` */
static void
connect_auth_list_activate (ShellAuthPrompt *self)
{
  if (self->auth_list_activate_id != 0)
    return;

  self->auth_list_activate_id = g_signal_connect (self->auth_list, "activate",
                                                  G_CALLBACK (auth_list_activate_cb), self);
}

/* `_disconnectAuthListActivate ()` */
static void
disconnect_auth_list_activate (ShellAuthPrompt *self)
{
  g_clear_signal_handler (&self->auth_list_activate_id, self->auth_list);
}

/* `setChoiceList ()` */
static void
set_choice_list (ShellAuthPrompt *self,
                 const char      *prompt_message,
                 GVariant        *choice_list)
{
  GVariantIter iter;
  const char *key;
  GVariant *content;

  shell_auth_list_clear (self->auth_list);
  st_label_set_text (self->auth_list_title_label, prompt_message != NULL ? prompt_message : "");

  if (choice_list != NULL)
    {
      g_variant_iter_init (&iter, choice_list);
      while (g_variant_iter_next (&iter, "(&s@a{sv})", &key, &content))
        {
          g_autoptr (GVariant) content_owner = content;

          shell_auth_list_add_item (self->auth_list, key, content_owner);
        }
    }

  clutter_actor_hide (self->entry_area);
  fade_in_element (CLUTTER_ACTOR (self->auth_list));
  shell_auth_prompt_update_sensitivity (self, TRUE);
}

/* ---- clear, question, answer, message ---- */

/**
 * shell_auth_prompt_clear:
 * @self: a #ShellAuthPrompt
 * @reuse_entry_text: whether to keep what the user typed
 *
 * Hides what the verifier asked and forgets the pending request (`clear ()`).
 */
void
shell_auth_prompt_clear (ShellAuthPrompt *self,
                         gboolean         reuse_entry_text)
{
  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));

  if (self->entry == NULL)
    return;

  if (!reuse_entry_text)
    {
      st_entry_set_hint_text (self->entry, "");
      st_entry_set_hint_text (self->inactive_entry, "");
      clutter_actor_hide (self->entry_area);
      st_entry_set_text (self->entry, "");
      st_entry_set_text (self->inactive_entry, "");
      stop_spinning (self, FALSE);
    }

  st_label_set_text (self->auth_list_title_label, "");
  shell_auth_list_clear (self->auth_list);
  clutter_actor_hide (CLUTTER_ACTOR (self->auth_list));
  disconnect_auth_list_activate (self);
  clutter_actor_hide (CLUTTER_ACTOR (self->auth_button));
  close_web_login_dialog (self);
  g_clear_object (&self->pending_handler);

  clutter_actor_set_opacity (self->main_box, 255);
  clutter_actor_set_reactive (self->main_box, TRUE);
  clutter_actor_set_opacity (CLUTTER_ACTOR (self->web_login_dialog), 255);
  clutter_actor_set_reactive (CLUTTER_ACTOR (self->web_login_dialog), TRUE);
}

/**
 * shell_auth_prompt_set_question:
 * @self: a #ShellAuthPrompt
 * @question: the hint of the entry
 *
 * Shows the entry with a hint (`setQuestion ()`).
 */
void
shell_auth_prompt_set_question (ShellAuthPrompt *self,
                                const char      *question)
{
  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));

  if (self->entry == NULL)
    return;

  st_entry_set_hint_text (self->entry, question);

  clutter_actor_hide (CLUTTER_ACTOR (self->auth_list));
  clutter_actor_hide (CLUTTER_ACTOR (self->auth_button));
  close_web_login_dialog (self);

  fade_in_element (self->entry_area);
  shell_auth_prompt_update_sensitivity (self, TRUE);
}

/**
 * shell_auth_prompt_get_answer:
 * @self: a #ShellAuthPrompt
 *
 * Returns: (transfer full): what the user typed, from the entry or typed
 *   before the verifier asked (`getAnswer ()`)
 */
char *
shell_auth_prompt_get_answer (ShellAuthPrompt *self)
{
  g_return_val_if_fail (SHELL_IS_AUTH_PROMPT (self), NULL);

  if (self->preemptive_answer != NULL && self->preemptive_answer[0] != '\0')
    return g_steal_pointer (&self->preemptive_answer);

  if (self->entry == NULL)
    return g_strdup ("");

  free_secret (&self->preemptive_answer);
  return g_strdup (st_entry_get_text (self->entry));
}

/**
 * shell_auth_prompt_set_message:
 * @self: a #ShellAuthPrompt
 * @message: (nullable): the message, hides the current one if %NULL or empty
 * @type: the #ShellGdmMessageType, which selects the style
 *
 * Shows a message below the entry (`setMessage ()`).
 */
void
shell_auth_prompt_set_message (ShellAuthPrompt     *self,
                               const char          *message,
                               ShellGdmMessageType  type)
{
  StWidget *label;

  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));

  if (self->message == NULL)
    return;

  label = ST_WIDGET (self->message);

  if (type == SHELL_GDM_MESSAGE_TYPE_ERROR)
    st_widget_add_style_class_name (label, "login-dialog-message-warning");
  else
    st_widget_remove_style_class_name (label, "login-dialog-message-warning");

  if (type == SHELL_GDM_MESSAGE_TYPE_HINT)
    st_widget_add_style_class_name (label, "login-dialog-message-hint");
  else
    st_widget_remove_style_class_name (label, "login-dialog-message-hint");

  if (message != NULL && message[0] != '\0')
    {
      AtkObject *accessible;

      clutter_actor_remove_all_transitions (CLUTTER_ACTOR (label));
      st_label_set_text (self->message, message);
      clutter_actor_set_opacity (CLUTTER_ACTOR (label), 255);

      accessible = clutter_actor_get_accessible (CLUTTER_ACTOR (self));
      if (accessible != NULL &&
          g_signal_lookup ("notification", G_OBJECT_TYPE (accessible)) != 0)
        g_signal_emit_by_name (accessible, "notification", message, ATK_LIVE_ASSERTIVE);
    }
  else
    {
      clutter_actor_set_opacity (CLUTTER_ACTOR (label), 0);
    }
}

/* ---- the signals of the verifier ---- */

static void
on_ask_question (ShellGdmUserVerifier *verifier,
                 const char           *service_name,
                 const char           *question,
                 gboolean              secret,
                 ShellGdmAuthHandler  *handler,
                 ShellAuthPrompt      *self)
{
  g_autofree char *preemptive_answer = NULL;
  g_autofree char *stripped = NULL;

  if (self->querying_service != NULL)
    shell_auth_prompt_clear (self, FALSE);

  g_free (self->querying_service);
  self->querying_service = g_strdup (service_name);
  set_prompt_step (self, self->prompt_step + 1);

  set_pending_handler (self, handler);

  preemptive_answer = g_steal_pointer (&self->preemptive_answer);
  clear_preemptive_state (self);
  if (preemptive_answer != NULL && preemptive_answer[0] != '\0')
    {
      complete_pending_callback (self, preemptive_answer);
      explicit_bzero (preemptive_answer, strlen (preemptive_answer));
      return;
    }

  shell_auth_prompt_update_entry (self, secret);

  /* Hack: The question string comes directly from PAM, if it's "Password:"
   * we replace it with our own to allow localization, if it's something
   * else we remove the last colon and any trailing or leading spaces. */
  if (g_strcmp0 (question, "Password:") == 0 || g_strcmp0 (question, "Password: ") == 0)
    {
      shell_auth_prompt_set_question (self, _("Password"));
    }
  else
    {
      const char *colon;
      gsize length;

      stripped = g_strdup (question != NULL ? question : "");
      length = strlen (stripped);
      while (length > 0 && stripped[length - 1] == ' ')
        length--;

      /* question.replace (/[:：] *$/, '') */
      colon = NULL;
      if (length > 0 && stripped[length - 1] == ':')
        colon = stripped + length - 1;
      else if (length >= strlen ("\xef\xbc\x9a") &&
               memcmp (stripped + length - strlen ("\xef\xbc\x9a"), "\xef\xbc\x9a",
                       strlen ("\xef\xbc\x9a")) == 0)
        colon = stripped + length - strlen ("\xef\xbc\x9a");

      if (colon != NULL)
        stripped[colon - stripped] = '\0';

      g_strstrip (stripped);
      shell_auth_prompt_set_question (self, stripped);
    }

  g_signal_emit (self, signals[PROMPTED], 0);
}

static void
message_shown_cb (gboolean finished,
                  gpointer user_data)
{
  ShellAuthPrompt *self = SHELL_AUTH_PROMPT (user_data);

  if (self->verifier != NULL)
    shell_gdm_user_verifier_message_shown (self->verifier);
}

static void
on_show_message (ShellGdmUserVerifier *verifier,
                 const char           *message,
                 ShellGdmMessageType   type,
                 gboolean              should_wiggle,
                 ShellAuthPrompt      *self)
{
  shell_auth_prompt_set_message (self, message, type);
  g_signal_emit (self, signals[PROMPTED], 0);

  /* If we're showing a message and no auth widget is currently visible,
   * show the entry area to allow getting a preemptive answer */
  if (message != NULL && message[0] != '\0' &&
      type < SHELL_GDM_MESSAGE_TYPE_ERROR &&
      !clutter_actor_is_visible (self->entry_area) &&
      !clutter_actor_is_visible (CLUTTER_ACTOR (self->auth_list)) &&
      !clutter_actor_is_visible (CLUTTER_ACTOR (self->auth_button)) &&
      !clutter_actor_is_visible (CLUTTER_ACTOR (self->web_login_dialog)))
    {
      fade_in_element (self->entry_area);
      shell_auth_prompt_update_sensitivity (self, TRUE);
    }

  /* The verifier waits for the wiggle to end before it counts the
   * time the user needs to read the message */
  if (should_wiggle)
    shell_animation_wiggle_full (CLUTTER_ACTOR (self->message),
                                 SHELL_WIGGLE_OFFSET, SHELL_WIGGLE_DURATION,
                                 SHELL_WIGGLE_COUNT,
                                 message_shown_cb, g_object_ref (self), g_object_unref);
}

static void
on_show_choice_list (ShellGdmUserVerifier *verifier,
                     const char           *service_name,
                     const char           *prompt_message,
                     GVariant             *choice_list,
                     ShellGdmAuthHandler  *handler,
                     ShellAuthPrompt      *self)
{
  if (self->querying_service != NULL)
    shell_auth_prompt_clear (self, FALSE);

  g_free (self->querying_service);
  self->querying_service = g_strdup (service_name);
  set_prompt_step (self, self->prompt_step + 1);

  clear_preemptive_state (self);

  connect_auth_list_activate (self);
  set_pending_handler (self, handler);

  set_choice_list (self, prompt_message, choice_list);
  shell_auth_prompt_update_sensitivity (self, TRUE);
  g_signal_emit (self, signals[PROMPTED], 0);
}

static void
on_show_button (ShellGdmUserVerifier *verifier,
                const char           *service_name,
                const char           *label,
                ShellGdmAuthHandler  *handler,
                ShellAuthPrompt      *self)
{
  if (self->querying_service != NULL)
    shell_auth_prompt_clear (self, FALSE);

  g_free (self->querying_service);
  self->querying_service = g_strdup (service_name);
  set_prompt_step (self, self->prompt_step + 1);

  clear_preemptive_state (self);

  set_pending_handler (self, handler);

  st_button_set_label (self->auth_button, label);

  fade_in_element (CLUTTER_ACTOR (self->auth_button));
  shell_auth_prompt_update_sensitivity (self, TRUE);
  g_signal_emit (self, signals[PROMPTED], 0);
}

static void
on_mechanisms_changed (ShellGdmUserVerifier *verifier,
                       GVariant             *mechanisms,
                       GVariant             *selected_mechanism,
                       ShellAuthPrompt      *self)
{
  g_signal_emit (self, signals[MECHANISMS_CHANGED], 0, mechanisms, selected_mechanism);
}

static void
on_web_login (ShellGdmUserVerifier *verifier,
              const char           *service_name,
              const char           *message,
              const char           *url,
              const char           *code,
              const char           *done_label,
              ShellGdmAuthHandler  *done_handler,
              ShellAuthPrompt      *self)
{
  if (self->querying_service != NULL)
    shell_auth_prompt_clear (self, FALSE);

  g_free (self->querying_service);
  self->querying_service = g_strdup (service_name);
  set_prompt_step (self, self->prompt_step + 1);

  clutter_actor_hide (self->entry_area);

  clear_preemptive_state (self);

  /* _openWebLoginDialog () */
  set_user_avatar_visible (self, FALSE);
  clutter_actor_hide (self->main_box);

  shell_web_login_dialog_update (self->web_login_dialog, message, url, code,
                                 done_label, web_login_done_cb,
                                 g_object_ref (done_handler), g_object_unref);
  fade_in_element (CLUTTER_ACTOR (self->web_login_dialog));
  shell_auth_prompt_update_sensitivity (self, TRUE);

  g_signal_emit (self, signals[PROMPTED], 0);
}

static void
on_verification_failed (ShellGdmUserVerifier *verifier,
                        const char           *service_name,
                        gboolean              can_retry,
                        ShellAuthPrompt      *self)
{
  gboolean was_querying_service = g_strcmp0 (self->querying_service, service_name) == 0;

  if (was_querying_service)
    g_clear_pointer (&self->querying_service, g_free);

  /* Only allow instant retrying with password authentication.
   * The rest of authentications will retry through the reset flow. */
  if (can_retry && shell_gdm_user_verifier_get_preemptive_input (self->verifier))
    {
      set_verification_status (self, SHELL_AUTH_PROMPT_STATUS_VERIFYING);
      st_entry_set_text (self->entry, "");
      shell_auth_prompt_start_preemptive_input (self, 0);
    }
  stop_spinning (self, FALSE);

  if (!can_retry)
    set_verification_status (self, SHELL_AUTH_PROMPT_STATUS_VERIFICATION_FAILED);

  if (was_querying_service)
    shell_animation_wiggle (self->entry_area);
}

static void
on_verification_complete (ShellGdmUserVerifier *verifier,
                          ShellAuthPrompt      *self)
{
  ShellEaseParams params = ease_params (SHELL_AUTH_PROMPT_MESSAGE_FADE_OUT_ANIMATION_TIME,
                                        -1, CLUTTER_EASE_OUT_QUAD);
  ClutterActor *widgets[] = { self->main_box, CLUTTER_ACTOR (self->web_login_dialog) };
  guint i;

  stop_spinning (self, TRUE);
  set_verification_status (self, SHELL_AUTH_PROMPT_STATUS_VERIFICATION_SUCCEEDED);

  for (i = 0; i < G_N_ELEMENTS (widgets); i++)
    {
      clutter_actor_set_reactive (widgets[i], FALSE);
      shell_actor_ease (widgets[i], &params, NULL, NULL, NULL,
                        "opacity", 0u,
                        NULL);
    }

  g_signal_emit (self, signals[VERIFICATION_COMPLETE], 0);
}

static void
on_reset (ShellGdmUserVerifier *verifier,
          gboolean              soft_reset,
          gboolean              reuse_entry_text,
          ShellAuthPrompt      *self)
{
  if (self->verification_status == SHELL_AUTH_PROMPT_STATUS_VERIFICATION_SUCCEEDED)
    return;

  shell_auth_prompt_reset (self, reuse_entry_text, soft_reset);
}

/* ---- the widgets ---- */

static void
cancel_button_clicked_cb (StButton        *button,
                          int              clicked_button,
                          ShellAuthPrompt *self)
{
  shell_auth_prompt_handle_cancel (self);
}

static void
next_button_clicked_cb (StButton        *button,
                        int              clicked_button,
                        ShellAuthPrompt *self)
{
  activate_next (self);
}

static void
auth_button_clicked_cb (StButton        *button,
                        int              clicked_button,
                        ShellAuthPrompt *self)
{
  complete_pending_callback (self, NULL);
}

static void
web_login_cancel_cb (ShellWebLoginDialog *dialog,
                     ShellAuthPrompt     *self)
{
  shell_auth_prompt_handle_cancel (self);
}

static void
web_login_loading_cb (ShellWebLoginDialog *dialog,
                      ShellAuthPrompt     *self)
{
  g_signal_emit (self, signals[LOADING], 0,
                 shell_web_login_dialog_get_is_loading (dialog));
}

static StEntry *
create_entry (GType type)
{
  StEntry *entry = g_object_new (type,
                                 "style-class", "login-dialog-prompt-entry",
                                 "can-focus", TRUE,
                                 "x-expand", TRUE,
                                 "y-expand", TRUE,
                                 NULL);

  /* The prompt keeps the entries that are not children */
  g_object_ref_sink (entry);
  shell_entry_add_context_menu (entry, SHELL_ACTION_MODE_NONE);

  return entry;
}

/* `_initInputRow ()` */
static void
init_input_row (ShellAuthPrompt *self)
{
  ClutterActor *input_well = self->input_well;
  ClutterConstraint *constraint;
  ClutterActor *spacer;

  self->main_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                 "style-class", "login-dialog-button-box",
                                 "orientation", CLUTTER_ORIENTATION_HORIZONTAL,
                                 "y-expand", FALSE,
                                 NULL);
  clutter_actor_add_child (input_well, self->main_box);

  self->cancel_button = g_object_new (ST_TYPE_BUTTON,
                                      "style-class", "login-dialog-button cancel-button",
                                      "accessible-name", _("Back"),
                                      "button-mask", ST_BUTTON_PRIMARY | ST_BUTTON_SECONDARY,
                                      "reactive", TRUE,
                                      "can-focus", TRUE,
                                      "x-align", CLUTTER_ACTOR_ALIGN_START,
                                      "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                      "icon-name", "go-previous-symbolic",
                                      NULL);
  g_signal_connect (self->cancel_button, "clicked",
                    G_CALLBACK (cancel_button_clicked_cb), self);
  update_cancel_button (self);
  clutter_actor_add_child (self->main_box, CLUTTER_ACTOR (self->cancel_button));

  self->auth_list = shell_auth_list_new ();
  clutter_actor_hide (CLUTTER_ACTOR (self->auth_list));
  self->auth_list_activate_id = 0;
  clutter_actor_add_child (input_well, CLUTTER_ACTOR (self->auth_list));

  self->auth_list_title_label = g_object_new (ST_TYPE_LABEL,
                                              "style-class", "login-dialog-auth-list-title-label",
                                              NULL);
  self->auth_list_title = g_object_new (ST_TYPE_BIN,
                                        "style-class", "login-dialog-auth-list-title",
                                        "x-expand", TRUE,
                                        "y-expand", TRUE,
                                        "child", self->auth_list_title_label,
                                        "visible", FALSE,
                                        NULL);
  g_object_bind_property (self->auth_list, "visible",
                          self->auth_list_title, "visible",
                          G_BINDING_DEFAULT);
  g_object_bind_property (self->auth_list, "opacity",
                          self->auth_list_title, "opacity",
                          G_BINDING_DEFAULT);
  clutter_actor_add_child (self->main_box, self->auth_list_title);

  constraint = clutter_bind_constraint_new (self->auth_list_title, CLUTTER_BIND_WIDTH, 0.0f);
  clutter_actor_add_constraint (CLUTTER_ACTOR (self->auth_list), constraint);
  constraint = clutter_bind_constraint_new (self->auth_list_title, CLUTTER_BIND_X, 0.0f);
  clutter_actor_add_constraint (CLUTTER_ACTOR (self->auth_list), constraint);

  self->entry_area = g_object_new (ST_TYPE_WIDGET,
                                   "style-class", "login-dialog-prompt-entry-area",
                                   "layout-manager", clutter_bin_layout_new (),
                                   "x-expand", TRUE,
                                   "y-expand", TRUE,
                                   "visible", FALSE,
                                   NULL);
  clutter_actor_add_child (self->main_box, self->entry_area);

  self->entry = NULL;

  self->text_entry = create_entry (ST_TYPE_ENTRY);
  self->password_entry = create_entry (ST_TYPE_PASSWORD_ENTRY);
  g_signal_connect (st_entry_get_clutter_text (self->text_entry), "text-changed",
                    G_CALLBACK (entry_text_changed_cb), self);
  g_signal_connect (st_entry_get_clutter_text (self->password_entry), "text-changed",
                    G_CALLBACK (entry_text_changed_cb), self);

  self->entry = self->password_entry;
  clutter_actor_add_child (self->entry_area, CLUTTER_ACTOR (self->entry));
  clutter_actor_grab_key_focus (CLUTTER_ACTOR (self->entry));
  self->inactive_entry = self->text_entry;

  self->timed_login_indicator = g_object_new (ST_TYPE_BIN,
                                              "style-class", "login-dialog-timed-login-indicator",
                                              "scale-x", 0.0,
                                              NULL);
  clutter_actor_add_child (input_well, self->timed_login_indicator);

  g_signal_connect (st_entry_get_clutter_text (self->text_entry), "activate",
                    G_CALLBACK (entry_activate_cb), self);
  g_signal_connect (st_entry_get_clutter_text (self->password_entry), "activate",
                    G_CALLBACK (entry_activate_cb), self);

  self->default_button_well = g_object_new (ST_TYPE_WIDGET,
                                            "layout-manager", clutter_bin_layout_new (),
                                            "style-class", "login-dialog-default-button-well",
                                            "x-expand", TRUE,
                                            "x-align", CLUTTER_ACTOR_ALIGN_END,
                                            "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                            NULL);
  clutter_actor_add_child (self->entry_area, self->default_button_well);

  self->next_button = g_object_new (ST_TYPE_BUTTON,
                                    "style-class", "login-dialog-button next-button",
                                    "accessible-name", _("Submit"),
                                    "button-mask", ST_BUTTON_PRIMARY | ST_BUTTON_SECONDARY,
                                    "reactive", TRUE,
                                    "can-focus", FALSE,
                                    "icon-name", "go-next-symbolic",
                                    NULL);
  g_signal_connect (self->next_button, "clicked",
                    G_CALLBACK (next_button_clicked_cb), self);
  st_widget_add_style_pseudo_class (ST_WIDGET (self->next_button), "default");
  clutter_actor_add_child (self->default_button_well, CLUTTER_ACTOR (self->next_button));

  self->spinner = shell_spinner_new (SHELL_AUTH_PROMPT_DEFAULT_BUTTON_WELL_ICON_SIZE, FALSE, FALSE);
  clutter_actor_add_child (self->default_button_well, CLUTTER_ACTOR (self->spinner));

  set_actor_in_default_button_well (self, CLUTTER_ACTOR (self->next_button), FALSE);

  self->auth_button = g_object_new (ST_TYPE_BUTTON,
                                    "style-class", "login-button",
                                    "button-mask", ST_BUTTON_PRIMARY | ST_BUTTON_SECONDARY,
                                    "can-focus", TRUE,
                                    "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                    "x-expand", TRUE,
                                    "y-expand", TRUE,
                                    NULL);
  g_signal_connect (self->auth_button, "clicked",
                    G_CALLBACK (auth_button_clicked_cb), self);
  clutter_actor_add_child (self->main_box, CLUTTER_ACTOR (self->auth_button));

  self->web_login_dialog = shell_web_login_dialog_new ();
  g_signal_connect (self->web_login_dialog, "cancel",
                    G_CALLBACK (web_login_cancel_cb), self);
  g_signal_connect (self->web_login_dialog, "loading",
                    G_CALLBACK (web_login_loading_cb), self);
  clutter_actor_add_child (input_well, CLUTTER_ACTOR (self->web_login_dialog));

  /* center elements inside main_box between the cancel
   * button on the left and this spacer on the right */
  spacer = clutter_actor_new ();
  constraint = clutter_bind_constraint_new (CLUTTER_ACTOR (self->cancel_button),
                                            CLUTTER_BIND_WIDTH, 0.0f);
  clutter_actor_add_constraint (spacer, constraint);
  clutter_actor_add_child (self->main_box, spacer);
}

/* ---- the actor ---- */

static gboolean
shell_auth_prompt_key_press_event (ClutterActor *actor,
                                   ClutterEvent *event)
{
  if (clutter_event_get_key_symbol (event) == CLUTTER_KEY_Escape)
    {
      shell_auth_prompt_handle_cancel (SHELL_AUTH_PROMPT (actor));
      return CLUTTER_EVENT_STOP;
    }

  return CLUTTER_EVENT_PROPAGATE;
}

/* `vfunc_hide ()` */
static void
shell_auth_prompt_hide (ClutterActor *actor)
{
  ShellAuthPrompt *self = SHELL_AUTH_PROMPT (actor);

  if (self->entry == NULL)
    {
      CLUTTER_ACTOR_CLASS (shell_auth_prompt_parent_class)->hide (actor);
      return;
    }

  stop_spinning (self, FALSE);
  CLUTTER_ACTOR_CLASS (shell_auth_prompt_parent_class)->hide (actor);
  clutter_actor_set_opacity (CLUTTER_ACTOR (self->message), 0);

  shell_auth_prompt_set_user (self, NULL);

  shell_auth_prompt_update_sensitivity (self, TRUE);
  st_entry_set_text (self->entry, "");
}

/* `_onDestroy ()` */
static void
shell_auth_prompt_destroy (ClutterActor *actor)
{
  ShellAuthPrompt *self = SHELL_AUTH_PROMPT (actor);

  if (self->timed_login_timeout_id != 0)
    {
      g_source_remove (self->timed_login_timeout_id);
      self->timed_login_timeout_id = 0;
    }

  if (self->inactive_entry != NULL)
    clutter_actor_destroy (CLUTTER_ACTOR (self->inactive_entry));
  self->inactive_entry = NULL;

  if (self->verifier != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->verifier, self);
      shell_gdm_user_verifier_destroy (self->verifier);
      g_clear_object (&self->verifier);
    }
  self->entry = NULL;

  if (self->parental_controls_shield != NULL)
    clutter_actor_destroy (self->parental_controls_shield);

  if (self->input_well != NULL)
    clutter_actor_destroy (self->input_well);

  /* The children are gone with the actors above and the parent destroys the
   * rest; nothing in the prompt may reach them anymore */
  self->user_well = NULL;
  self->main_content = NULL;
  self->main_box = NULL;
  self->cancel_button = NULL;
  self->auth_list = NULL;
  self->auth_list_title = NULL;
  self->auth_list_title_label = NULL;
  self->entry_area = NULL;
  self->entry = NULL;
  self->inactive_entry = NULL;
  self->timed_login_indicator = NULL;
  self->default_button_well = NULL;
  self->default_button_well_actor = NULL;
  self->next_button = NULL;
  self->spinner = NULL;
  self->auth_button = NULL;
  self->web_login_dialog = NULL;
  self->caps_lock_warning = NULL;
  self->message = NULL;

  CLUTTER_ACTOR_CLASS (shell_auth_prompt_parent_class)->destroy (actor);
}

static void
shell_auth_prompt_constructed (GObject *object)
{
  ShellAuthPrompt *self = SHELL_AUTH_PROMPT (object);
  ClutterActor *capslock_placeholder;
  ClutterText *message_text;
  gboolean reauthentication_only;

  G_OBJECT_CLASS (shell_auth_prompt_parent_class)->constructed (object);

  self->verification_status = SHELL_AUTH_PROMPT_STATUS_NOT_VERIFYING;

  g_signal_connect (self, "notify::prompt-step",
                    G_CALLBACK (prompt_step_notify_cb), NULL);

  reauthentication_only = self->mode == SHELL_AUTH_PROMPT_MODE_UNLOCK_ONLY;
  self->verifier = shell_gdm_user_verifier_new (self->client, reauthentication_only);

  g_signal_connect_object (self->verifier, "ask-question",
                           G_CALLBACK (on_ask_question), self, 0);
  g_signal_connect_object (self->verifier, "show-message",
                           G_CALLBACK (on_show_message), self, 0);
  g_signal_connect_object (self->verifier, "show-choice-list",
                           G_CALLBACK (on_show_choice_list), self, 0);
  g_signal_connect_object (self->verifier, "show-button",
                           G_CALLBACK (on_show_button), self, 0);
  g_signal_connect_object (self->verifier, "mechanisms-changed",
                           G_CALLBACK (on_mechanisms_changed), self, 0);
  g_signal_connect_object (self->verifier, "web-login",
                           G_CALLBACK (on_web_login), self, 0);
  g_signal_connect_object (self->verifier, "verification-failed",
                           G_CALLBACK (on_verification_failed), self, 0);
  g_signal_connect_object (self->verifier, "verification-complete",
                           G_CALLBACK (on_verification_complete), self, 0);
  g_signal_connect_object (self->verifier, "reset",
                           G_CALLBACK (on_reset), self, 0);

  self->user_well = g_object_new (ST_TYPE_BIN, "x-expand", TRUE, NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->user_well);

  self->input_well = g_object_new (ST_TYPE_BOX_LAYOUT,
                                   "style-class", "login-dialog-input-well",
                                   "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                   "y-align", CLUTTER_ACTOR_ALIGN_START,
                                   NULL);
  g_object_ref_sink (self->input_well);
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->input_well);
  self->main_content = self->input_well;

  init_input_row (self);

  capslock_placeholder = g_object_new (ST_TYPE_LABEL, NULL);
  clutter_actor_add_child (self->input_well, capslock_placeholder);

  self->caps_lock_warning = CLUTTER_ACTOR (g_object_new (SHELL_TYPE_CAPS_LOCK_WARNING,
                                                         "x-expand", TRUE,
                                                         "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                                         NULL));
  clutter_actor_add_child (self->input_well, self->caps_lock_warning);

  g_object_bind_property (self->caps_lock_warning, "visible",
                          capslock_placeholder, "visible",
                          G_BINDING_SYNC_CREATE | G_BINDING_INVERT_BOOLEAN);

  self->message = g_object_new (ST_TYPE_LABEL,
                                "opacity", 0,
                                "style-class", "login-dialog-message",
                                "x-expand", TRUE,
                                "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                NULL);
  message_text = st_label_get_clutter_text (self->message);
  clutter_text_set_line_wrap (message_text, TRUE);
  clutter_text_set_ellipsize (message_text, PANGO_ELLIPSIZE_NONE);
  clutter_actor_add_child (self->input_well, CLUTTER_ACTOR (self->message));
}

static void
shell_auth_prompt_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  ShellAuthPrompt *self = SHELL_AUTH_PROMPT (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_set_object (&self->client, g_value_get_object (value));
      break;

    case PROP_MODE:
      self->mode = g_value_get_uint (value);
      break;

    case PROP_VERIFICATION_STATUS:
      set_verification_status (self, g_value_get_uint (value));
      break;

    case PROP_PROMPT_STEP:
      set_prompt_step (self, g_value_get_uint (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_auth_prompt_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  ShellAuthPrompt *self = SHELL_AUTH_PROMPT (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_value_set_object (value, self->client);
      break;

    case PROP_MODE:
      g_value_set_uint (value, self->mode);
      break;

    case PROP_VERIFICATION_STATUS:
      g_value_set_uint (value, self->verification_status);
      break;

    case PROP_PROMPT_STEP:
      g_value_set_uint (value, self->prompt_step);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_auth_prompt_dispose (GObject *object)
{
  ShellAuthPrompt *self = SHELL_AUTH_PROMPT (object);

  if (self->timed_login_timeout_id != 0)
    {
      g_source_remove (self->timed_login_timeout_id);
      self->timed_login_timeout_id = 0;
    }

  if (self->verifier != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->verifier, self);
      shell_gdm_user_verifier_destroy (self->verifier);
      g_clear_object (&self->verifier);
    }

  g_clear_object (&self->pending_handler);
  g_clear_object (&self->text_entry);
  g_clear_object (&self->password_entry);
  g_clear_object (&self->input_well);
  g_clear_object (&self->parental_controls_shield);
  g_clear_object (&self->client);

  free_secret (&self->preemptive_answer);
  g_clear_pointer (&self->querying_service, g_free);

  self->user_well = NULL;
  self->main_content = NULL;
  self->main_box = NULL;
  self->cancel_button = NULL;
  self->auth_list = NULL;
  self->auth_list_title = NULL;
  self->auth_list_title_label = NULL;
  self->entry_area = NULL;
  self->entry = NULL;
  self->inactive_entry = NULL;
  self->timed_login_indicator = NULL;
  self->default_button_well = NULL;
  self->default_button_well_actor = NULL;
  self->next_button = NULL;
  self->spinner = NULL;
  self->auth_button = NULL;
  self->web_login_dialog = NULL;
  self->caps_lock_warning = NULL;
  self->message = NULL;

  G_OBJECT_CLASS (shell_auth_prompt_parent_class)->dispose (object);
}

static void
shell_auth_prompt_class_init (ShellAuthPromptClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_auth_prompt_constructed;
  object_class->set_property = shell_auth_prompt_set_property;
  object_class->get_property = shell_auth_prompt_get_property;
  object_class->dispose = shell_auth_prompt_dispose;

  actor_class->key_press_event = shell_auth_prompt_key_press_event;
  actor_class->hide = shell_auth_prompt_hide;
  actor_class->destroy = shell_auth_prompt_destroy;

  /**
   * ShellAuthPrompt:client:
   *
   * The #GdmClient that provides the user verifier.
   */
  props[PROP_CLIENT] =
    g_param_spec_object ("client", NULL, NULL,
                         GDM_TYPE_CLIENT,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellAuthPrompt:mode:
   *
   * The #ShellAuthPromptMode.
   */
  props[PROP_MODE] =
    g_param_spec_uint ("mode", NULL, NULL,
                       SHELL_AUTH_PROMPT_MODE_UNLOCK_ONLY,
                       SHELL_AUTH_PROMPT_MODE_UNLOCK_OR_LOG_IN,
                       SHELL_AUTH_PROMPT_MODE_UNLOCK_ONLY,
                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellAuthPrompt:verification-status:
   *
   * The #ShellAuthPromptStatus of the verification.
   */
  props[PROP_VERIFICATION_STATUS] =
    g_param_spec_uint ("verification-status", NULL, NULL,
                       SHELL_AUTH_PROMPT_STATUS_NOT_VERIFYING,
                       SHELL_AUTH_PROMPT_STATUS_VERIFICATION_IN_PROGRESS,
                       SHELL_AUTH_PROMPT_STATUS_NOT_VERIFYING,
                       G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  /**
   * ShellAuthPrompt:prompt-step:
   *
   * How many prompts the verifier made since the verification began.
   */
  props[PROP_PROMPT_STEP] =
    g_param_spec_uint ("prompt-step", NULL, NULL,
                       0, G_MAXUINT32, 0,
                       G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellAuthPrompt::cancelled:
   * @self: the #ShellAuthPrompt
   *
   * Emitted after a reset when the user cancelled the verification.
   */
  signals[CANCELLED] =
    g_signal_new ("cancelled", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellAuthPrompt::failed:
   * @self: the #ShellAuthPrompt
   *
   * Emitted after a reset when the verification failed.
   */
  signals[FAILED] =
    g_signal_new ("failed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellAuthPrompt::next:
   * @self: the #ShellAuthPrompt
   *
   * Emitted when the user submitted the entry.
   */
  signals[NEXT] =
    g_signal_new ("next", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellAuthPrompt::prompted:
   * @self: the #ShellAuthPrompt
   *
   * Emitted when the verifier asked something or showed a message.
   */
  signals[PROMPTED] =
    g_signal_new ("prompted", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellAuthPrompt::mechanisms-changed:
   * @self: the #ShellAuthPrompt
   * @mechanisms: an `aa{sv}` with the mechanisms the user can pick (see
   *   #ShellGdmUserVerifier::mechanisms-changed)
   * @selected_mechanism: an `a{sv}` with the selected mechanism, empty if none
   *
   * Emitted when the mechanisms of the verifier changed.
   */
  signals[MECHANISMS_CHANGED] =
    g_signal_new ("mechanisms-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                  G_TYPE_VARIANT, G_TYPE_VARIANT);

  /**
   * ShellAuthPrompt::reset:
   * @self: the #ShellAuthPrompt
   * @reset_type: the #ShellAuthPromptResetType, what the owner has to do
   *
   * Emitted after the prompt was reset.
   */
  signals[RESET] =
    g_signal_new ("reset", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                  G_TYPE_UINT);

  /**
   * ShellAuthPrompt::verification-complete:
   * @self: the #ShellAuthPrompt
   *
   * Emitted when the user is verified.
   */
  signals[VERIFICATION_COMPLETE] =
    g_signal_new ("verification-complete", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellAuthPrompt::loading:
   * @self: the #ShellAuthPrompt
   * @is_loading: whether the prompt waits for the verifier
   *
   * Emitted when the prompt starts or stops waiting.
   */
  signals[LOADING] =
    g_signal_new ("loading", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);
}

static void
shell_auth_prompt_init (ShellAuthPrompt *self)
{
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

/**
 * shell_auth_prompt_new:
 * @client: the #GdmClient of the login or lock screen
 * @mode: what the prompt is for
 *
 * Creates a prompt (`new AuthPrompt (gdmClient, mode)`).
 *
 * Returns: (transfer floating): a new #ShellAuthPrompt
 */
ShellAuthPrompt *
shell_auth_prompt_new (GdmClient          *client,
                       ShellAuthPromptMode mode)
{
  return g_object_new (SHELL_TYPE_AUTH_PROMPT,
                       "style-class", "login-dialog-prompt-layout",
                       "orientation", CLUTTER_ORIENTATION_VERTICAL,
                       "x-expand", TRUE,
                       "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       "reactive", TRUE,
                       "client", client,
                       "mode", (guint) mode,
                       NULL);
}

/**
 * shell_auth_prompt_get_verification_status:
 * @self: a #ShellAuthPrompt
 *
 * Returns: the #ShellAuthPromptStatus of the verification
 */
ShellAuthPromptStatus
shell_auth_prompt_get_verification_status (ShellAuthPrompt *self)
{
  g_return_val_if_fail (SHELL_IS_AUTH_PROMPT (self), SHELL_AUTH_PROMPT_STATUS_NOT_VERIFYING);

  return self->verification_status;
}

/**
 * shell_auth_prompt_set_verification_status:
 * @self: a #ShellAuthPrompt
 * @status: the new #ShellAuthPromptStatus
 *
 * Sets :verification-status.
 */
void
shell_auth_prompt_set_verification_status (ShellAuthPrompt       *self,
                                           ShellAuthPromptStatus  status)
{
  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));

  set_verification_status (self, status);
}

/**
 * shell_auth_prompt_get_prompt_step:
 * @self: a #ShellAuthPrompt
 *
 * Returns: the value of :prompt-step
 */
guint
shell_auth_prompt_get_prompt_step (ShellAuthPrompt *self)
{
  g_return_val_if_fail (SHELL_IS_AUTH_PROMPT (self), 0);

  return self->prompt_step;
}

/**
 * shell_auth_prompt_get_cancel_button:
 * @self: a #ShellAuthPrompt
 *
 * Returns: (transfer none): the button which goes back (`cancelButton`)
 */
StButton *
shell_auth_prompt_get_cancel_button (ShellAuthPrompt *self)
{
  g_return_val_if_fail (SHELL_IS_AUTH_PROMPT (self), NULL);

  return self->cancel_button;
}

typedef struct
{
  ShellBatchHold *hold;
  gint64 start_time;
  double time;
  ShellAuthPrompt *prompt;     /* weak */
} TimedLoginData;

static void
timed_login_data_free (TimedLoginData *data)
{
  g_clear_object (&data->hold);
  g_clear_weak_pointer (&data->prompt);
  g_free (data);
}

static gboolean
timed_login_timeout_cb (gpointer user_data)
{
  TimedLoginData *data = user_data;
  ShellAuthPrompt *self = data->prompt;
  gint64 current_time = g_get_monotonic_time ();
  double elapsed_time = (current_time - data->start_time) / (double) G_USEC_PER_SEC;

  if (self == NULL)
    return G_SOURCE_REMOVE;

  clutter_actor_set_scale (self->timed_login_indicator, elapsed_time / data->time, 1.0);

  if (elapsed_time >= data->time)
    {
      g_autoptr (ShellBatchHold) hold = g_object_ref (data->hold);

      self->timed_login_timeout_id = 0;
      shell_batch_hold_release (hold);
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

/**
 * shell_auth_prompt_show_timed_login_indicator:
 * @self: a #ShellAuthPrompt
 * @time: how long the indicator takes to fill, in seconds
 *
 * Fills the bar of the timed login for @time seconds (`showTimedLoginIndicator ()`).
 *
 * Returns: (transfer none): a #ShellBatchHold which is released when the bar
 *   is full. It is kept alive until then, or until the indicator is hidden
 */
ShellBatchHold *
shell_auth_prompt_show_timed_login_indicator (ShellAuthPrompt *self,
                                              double           time)
{
  TimedLoginData *data;
  ShellBatchHold *hold;

  g_return_val_if_fail (SHELL_IS_AUTH_PROMPT (self), NULL);

  shell_auth_prompt_hide_timed_login_indicator (self);

  hold = shell_batch_hold_new ();

  data = g_new0 (TimedLoginData, 1);
  data->hold = hold;
  data->start_time = g_get_monotonic_time ();
  data->time = time;
  g_set_weak_pointer (&data->prompt, self);

  self->timed_login_timeout_id = g_timeout_add_full (G_PRIORITY_DEFAULT, 33,
                                                     timed_login_timeout_cb, data,
                                                     (GDestroyNotify) timed_login_data_free);
  g_source_set_name_by_id (self->timed_login_timeout_id,
                           "[gnome-shell] this._timedLoginTimeoutId");

  return hold;
}

/**
 * shell_auth_prompt_hide_timed_login_indicator:
 * @self: a #ShellAuthPrompt
 *
 * Empties the bar of the timed login (`hideTimedLoginIndicator ()`).
 */
void
shell_auth_prompt_hide_timed_login_indicator (ShellAuthPrompt *self)
{
  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));

  if (self->timed_login_timeout_id != 0)
    {
      g_source_remove (self->timed_login_timeout_id);
      self->timed_login_timeout_id = 0;
    }

  if (self->timed_login_indicator != NULL)
    clutter_actor_set_scale (self->timed_login_indicator, 0.0, 1.0);
}

/**
 * shell_auth_prompt_start_preemptive_input:
 * @self: a #ShellAuthPrompt
 * @unichar: the character the user typed to start, 0 for none
 *
 * Lets the user type before the verifier asks (`startPreemptiveInput ()`).
 */
void
shell_auth_prompt_start_preemptive_input (ShellAuthPrompt *self,
                                          gunichar         unichar)
{
  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));

  if (self->entry == NULL)
    return;

  self->preemptive_input = TRUE;
  shell_auth_prompt_update_sensitivity (self, TRUE);
  if (unichar != 0)
    clutter_text_insert_unichar (st_entry_get_clutter_text (self->entry), unichar);
}

/**
 * shell_auth_prompt_set_auth_blocked:
 * @self: a #ShellAuthPrompt
 * @should_block: whether to block the authentication
 *
 * Replaces the prompt with the shield of the parental controls session limits
 * (`setAuthBlocked ()`).
 */
void
shell_auth_prompt_set_auth_blocked (ShellAuthPrompt *self,
                                    gboolean         should_block)
{
  ClutterActor *new_main_content;

  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));

  if (self->input_well == NULL)
    return;

  if (self->parental_controls_shield == NULL)
    {
      self->parental_controls_shield = parental_controls_shield_new ();
      g_object_ref_sink (self->parental_controls_shield);
    }

  new_main_content = should_block ? self->parental_controls_shield : self->input_well;

  if (new_main_content != self->main_content)
    {
      clutter_actor_replace_child (CLUTTER_ACTOR (self), self->main_content, new_main_content);
      self->main_content = new_main_content;
    }

  if (self->main_content == self->input_well)
    clutter_actor_grab_key_focus (CLUTTER_ACTOR (self->entry));
}

/**
 * shell_auth_prompt_select_mechanism:
 * @self: a #ShellAuthPrompt
 * @mechanism: (nullable): an `a{sv}` with the `serviceName` and the `role` of
 *   the mechanism to select
 *
 * Selects an authentication mechanism (`selectMechanism ()`).
 *
 * Returns: %FALSE if the verification cannot be interrupted now
 */
gboolean
shell_auth_prompt_select_mechanism (ShellAuthPrompt *self,
                                    GVariant        *mechanism)
{
  g_autofree char *service_name = NULL;
  g_autofree char *role = NULL;
  guint old_prompt_step;

  g_return_val_if_fail (SHELL_IS_AUTH_PROMPT (self), FALSE);
  g_return_val_if_fail (mechanism == NULL || g_variant_is_of_type (mechanism, G_VARIANT_TYPE_VARDICT), FALSE);

  if (self->verification_status == SHELL_AUTH_PROMPT_STATUS_VERIFICATION_SUCCEEDED ||
      self->verification_status == SHELL_AUTH_PROMPT_STATUS_VERIFICATION_IN_PROGRESS)
    return FALSE;

  if (mechanism != NULL)
    {
      g_variant_lookup (mechanism, "serviceName", "s", &service_name);
      g_variant_lookup (mechanism, "role", "s", &role);
    }

  old_prompt_step = self->prompt_step;
  set_prompt_step (self, 0);
  if (self->verifier == NULL ||
      !shell_gdm_user_verifier_select_mechanism (self->verifier, service_name, role))
    set_prompt_step (self, old_prompt_step);

  return TRUE;
}

/**
 * shell_auth_prompt_reset:
 * @self: a #ShellAuthPrompt
 * @reuse_entry_text: whether to keep what the user typed
 * @soft_reset: whether to only cancel the verification of the verifier, to
 *   go back to the prompt of the current user
 *
 * Starts over (`reset ()`) and emits #ShellAuthPrompt::reset, after
 * #ShellAuthPrompt::failed or #ShellAuthPrompt::cancelled if the verification
 * had failed or was cancelled.
 */
void
shell_auth_prompt_reset (ShellAuthPrompt *self,
                         gboolean         reuse_entry_text,
                         gboolean         soft_reset)
{
  ShellAuthPromptStatus old_status;
  ShellAuthPromptResetType reset_type;

  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));

  if (self->entry == NULL)
    return;

  old_status = self->verification_status;
  set_verification_status (self, SHELL_AUTH_PROMPT_STATUS_NOT_VERIFYING);
  if (old_status != SHELL_AUTH_PROMPT_STATUS_VERIFICATION_IN_PROGRESS)
    free_secret (&self->preemptive_answer);
  set_prompt_step (self, 0);

  if (self->verifier != NULL)
    {
      if (soft_reset)
        shell_gdm_user_verifier_cancel (self->verifier);
      else
        shell_gdm_user_verifier_reset (self->verifier);
    }

  reuse_entry_text = reuse_entry_text ||
                     (self->preemptive_answer != NULL && self->preemptive_answer[0] != '\0') ||
                     self->preemptive_input;

  g_clear_pointer (&self->querying_service, g_free);
  shell_auth_prompt_clear (self, reuse_entry_text);
  clutter_actor_set_opacity (CLUTTER_ACTOR (self->message), 0);
  shell_auth_prompt_update_entry (self, TRUE);

  if (old_status == SHELL_AUTH_PROMPT_STATUS_VERIFICATION_FAILED)
    g_signal_emit (self, signals[FAILED], 0);
  else if (old_status == SHELL_AUTH_PROMPT_STATUS_VERIFICATION_CANCELLED)
    g_signal_emit (self, signals[CANCELLED], 0);

  if (self->mode == SHELL_AUTH_PROMPT_MODE_UNLOCK_ONLY)
    {
      /* The user is constant at the unlock screen, so it will immediately
       * respond to the request with the username */
      if (old_status == SHELL_AUTH_PROMPT_STATUS_VERIFICATION_CANCELLED)
        return;
      reset_type = SHELL_AUTH_PROMPT_RESET_PROVIDE_USERNAME;
    }
  else if (self->verifier != NULL &&
           !shell_gdm_user_verifier_needs_username (self->verifier))
    {
      /* We don't need to know the username if the user preempted the login screen
       * with a smartcard or with preauthenticated oVirt credentials */
      reset_type = SHELL_AUTH_PROMPT_RESET_DONT_PROVIDE_USERNAME;
    }
  else if (old_status == SHELL_AUTH_PROMPT_STATUS_VERIFICATION_IN_PROGRESS ||
           soft_reset)
    {
      /* We're going back to retry with current user */
      reset_type = SHELL_AUTH_PROMPT_RESET_REUSE_USERNAME;
    }
  else
    {
      /* In all other cases, we should get the username up front. */
      reset_type = SHELL_AUTH_PROMPT_RESET_PROVIDE_USERNAME;
    }

  g_signal_emit (self, signals[RESET], 0, (guint) reset_type);
}

/**
 * shell_auth_prompt_begin:
 * @self: a #ShellAuthPrompt
 * @user_name: (nullable): the user to verify, or %NULL if the services ask
 * @hold: (nullable): a #ShellBatchHold to release when the verification began
 *
 * Starts the verification (`begin ()`).
 */
void
shell_auth_prompt_begin (ShellAuthPrompt *self,
                         const char      *user_name,
                         ShellBatchHold  *hold)
{
  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));

  if (self->entry == NULL)
    return;

  if (!self->preemptive_input)
    shell_auth_prompt_update_sensitivity (self, FALSE);

  if (self->verifier != NULL)
    shell_gdm_user_verifier_begin (self->verifier, user_name, hold);
  set_verification_status (self, SHELL_AUTH_PROMPT_STATUS_VERIFYING);
}

typedef struct
{
  ShellAuthPromptFinishFunc func;
  gpointer user_data;
  GDestroyNotify destroy;
  ShellGdmUserVerifier *verifier;     /* weak */
  gulong handler_id;
} FinishData;

static void
finish_data_free (FinishData *data)
{
  g_clear_weak_pointer (&data->verifier);
  if (data->destroy != NULL)
    data->destroy (data->user_data);
  g_free (data);
}

static void
finish_no_more_messages_cb (ShellGdmUserVerifier *verifier,
                            FinishData           *data)
{
  g_signal_handler_disconnect (verifier, data->handler_id);
  data->handler_id = 0;

  shell_gdm_user_verifier_clear (verifier);
  data->func (data->user_data);
}

/**
 * shell_auth_prompt_finish:
 * @self: a #ShellAuthPrompt
 * @func: called when the verifier has no more messages to show
 * @user_data: (closure func): data for @func
 * @destroy: (nullable): called on @user_data after @func, or when the prompt
 *   is destroyed before
 *
 * Waits for the messages of the verifier to be shown, clears the verifier
 * and calls @func (`finish (onComplete)`).
 */
void
shell_auth_prompt_finish (ShellAuthPrompt           *self,
                          ShellAuthPromptFinishFunc  func,
                          gpointer                   user_data,
                          GDestroyNotify             destroy)
{
  FinishData *data;

  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));
  g_return_if_fail (func != NULL);

  if (self->verifier == NULL)
    {
      func (user_data);
      if (destroy != NULL)
        destroy (user_data);
      return;
    }

  if (!shell_gdm_user_verifier_get_has_pending_messages (self->verifier))
    {
      shell_gdm_user_verifier_clear (self->verifier);
      func (user_data);
      if (destroy != NULL)
        destroy (user_data);
      return;
    }

  data = g_new0 (FinishData, 1);
  data->func = func;
  data->user_data = user_data;
  data->destroy = destroy;
  g_set_weak_pointer (&data->verifier, self->verifier);
  data->handler_id = g_signal_connect_data (self->verifier, "no-more-messages",
                                            G_CALLBACK (finish_no_more_messages_cb),
                                            data,
                                            (GClosureNotify) finish_data_free,
                                            0);
}

/* `_handleCancel ()` */
static void
shell_auth_prompt_handle_cancel (ShellAuthPrompt *self)
{
  if (self->verifier != NULL &&
      shell_gdm_user_verifier_cancel_requested (self->verifier))
    {
      /* We substract 2 because on cancel we'll receive a new prompt signal
       * which increments promptStep, so that ends with a result of going
       * back one step. */
      if (self->prompt_step < 2)
        set_prompt_step (self, 0);
      else
        set_prompt_step (self, self->prompt_step - 2);
      return;
    }

  shell_auth_prompt_cancel (self);
}

/**
 * shell_auth_prompt_cancel:
 * @self: a #ShellAuthPrompt
 *
 * Goes back one step of the verification, or cancels it (`cancel ()`).
 */
void
shell_auth_prompt_cancel (ShellAuthPrompt *self)
{
  g_return_if_fail (SHELL_IS_AUTH_PROMPT (self));

  if (self->verification_status == SHELL_AUTH_PROMPT_STATUS_VERIFICATION_SUCCEEDED)
    return;

  /* If we're in a multi-step flow (step > 1), go back to step 1 instead of full reset */
  if (self->prompt_step > 1)
    {
      shell_auth_prompt_reset (self, FALSE, TRUE);
      return;
    }

  if (self->verification_status == SHELL_AUTH_PROMPT_STATUS_VERIFICATION_IN_PROGRESS)
    {
      self->cancelled_retries++;
      if (self->verifier != NULL &&
          self->cancelled_retries > shell_gdm_user_verifier_get_allowed_failures (self->verifier))
        set_verification_status (self, SHELL_AUTH_PROMPT_STATUS_VERIFICATION_FAILED);
    }
  else
    {
      set_verification_status (self, SHELL_AUTH_PROMPT_STATUS_VERIFICATION_CANCELLED);
    }

  shell_auth_prompt_reset (self, FALSE, FALSE);
}
