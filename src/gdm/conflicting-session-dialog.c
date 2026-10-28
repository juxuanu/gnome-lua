/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * conflicting-session-dialog.c: The dialog which tells the user that a session
 * is running already, and offers to force stop it. Port of
 * js/gdm/conflictingSessionDialog.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <cogl/cogl.h>
#include <glib/gi18n-lib.h>
#include <pango/pango.h>
#include <clutter/clutter-pango.h>

#include "gdm/conflicting-session-dialog.h"
#include "ui/animation.h"
#include "ui/environment.h"

#define SPINNER_SIZE 32
#define DIM_BRIGHTNESS 0.7
#define DIM_BRIGHTNESS_NAME "conflicting-session-dialog-dim"
#define SPINNER_DELAY 3000
#define SPINNER_ANIMATION_TIME 200

/**
 * ShellConflictingSessionDialog:
 *
 * The dialog shown when the session the user logs in to is running already
 * (`ConflictingSessionDialog`).
 */
struct _ShellConflictingSessionDialog
{
  ShellModalDialog parent;

  ClutterEffect *dim_effect;
  ShellSpinner *spinner;
  StButton *cancel_button;
  StButton *force_stop_button;
};

G_DEFINE_FINAL_TYPE (ShellConflictingSessionDialog, shell_conflicting_session_dialog,
                     SHELL_TYPE_MODAL_DIALOG)

enum
{
  CANCEL,
  FORCE_STOP,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

static void
set_button_sensitive (StButton *button,
                      gboolean  sensitive)
{
  clutter_actor_set_reactive (CLUTTER_ACTOR (button), sensitive);
  st_widget_set_can_focus (ST_WIDGET (button), sensitive);
}

/* _animateDimEffect () */
static void
animate_dim_effect (ShellConflictingSessionDialog *self)
{
  ShellModalDialog *modal_dialog = SHELL_MODAL_DIALOG (self);
  ClutterActor *dialog_layout = CLUTTER_ACTOR (shell_modal_dialog_get_dialog_layout (modal_dialog));
  guint8 val = 127 * DIM_BRIGHTNESS;
  CoglColor color = COGL_COLOR_INIT (val, val, val, 255);
  g_auto (GValue) value = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;
  ShellEaseParams params = {
    .duration = SPINNER_ANIMATION_TIME,
    .delay = SPINNER_DELAY,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  g_value_init (&value, COGL_TYPE_COLOR);
  g_value_set_boxed (&value, &color);

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (dialog_layout),
                                       "@effects." DIM_BRIGHTNESS_NAME ".brightness",
                                       &value, &params, NULL, NULL, NULL, &error))
    g_debug ("%s: %s", G_STRFUNC, error->message);

  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->dim_effect), TRUE);
}

/* _startSpinning () */
static void
start_spinning (ShellConflictingSessionDialog *self)
{
  ShellEaseParams params = {
    .duration = SPINNER_ANIMATION_TIME,
    .delay = SPINNER_DELAY,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  shell_modal_dialog_pop_modal (SHELL_MODAL_DIALOG (self));

  set_button_sensitive (self->cancel_button, FALSE);
  set_button_sensitive (self->force_stop_button, FALSE);

  shell_spinner_play (self->spinner);
  clutter_actor_set_opacity (CLUTTER_ACTOR (self->spinner), 0);
  shell_actor_ease (CLUTTER_ACTOR (self->spinner), &params, NULL, NULL, NULL,
                    "opacity", 255u,
                    NULL);

  animate_dim_effect (self);
}

static void
cancel_button_cb (gpointer user_data)
{
  g_signal_emit (user_data, signals[CANCEL], 0);
}

static void
force_stop_button_cb (gpointer user_data)
{
  ShellConflictingSessionDialog *self = SHELL_CONFLICTING_SESSION_DIALOG (user_data);

  start_spinning (self);
  g_signal_emit (self, signals[FORCE_STOP], 0);
}

/* _createSpinner () */
static void
create_spinner (ShellConflictingSessionDialog *self)
{
  ShellModalDialog *modal_dialog = SHELL_MODAL_DIALOG (self);
  ClutterActor *dialog_layout = CLUTTER_ACTOR (shell_modal_dialog_get_dialog_layout (modal_dialog));
  ClutterConstraint *constraint;

  self->dim_effect = clutter_brightness_contrast_effect_new ();
  g_object_ref_sink (self->dim_effect);
  clutter_actor_meta_set_name (CLUTTER_ACTOR_META (self->dim_effect), DIM_BRIGHTNESS_NAME);
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->dim_effect), FALSE);
  clutter_actor_add_effect (dialog_layout, self->dim_effect);

  self->spinner = shell_spinner_new (SPINNER_SIZE, FALSE, TRUE);

  constraint = clutter_bind_constraint_new (dialog_layout, CLUTTER_BIND_POSITION, 0.0f);
  clutter_actor_add_constraint (CLUTTER_ACTOR (self->spinner), constraint);
  constraint = clutter_align_constraint_new (dialog_layout, CLUTTER_ALIGN_BOTH, 0.5f);
  clutter_actor_add_constraint (CLUTTER_ACTOR (self->spinner), constraint);

  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_background_stack (modal_dialog)),
                           CLUTTER_ACTOR (self->spinner));
}

static void
shell_conflicting_session_dialog_dispose (GObject *object)
{
  ShellConflictingSessionDialog *self = SHELL_CONFLICTING_SESSION_DIALOG (object);

  if (self->dim_effect != NULL)
    {
      ClutterActor *actor = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->dim_effect));

      if (actor != NULL)
        clutter_actor_remove_effect (actor, self->dim_effect);
    }
  g_clear_object (&self->dim_effect);

  self->spinner = NULL;
  self->cancel_button = NULL;
  self->force_stop_button = NULL;

  G_OBJECT_CLASS (shell_conflicting_session_dialog_parent_class)->dispose (object);
}

static void
shell_conflicting_session_dialog_class_init (ShellConflictingSessionDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_conflicting_session_dialog_dispose;

  /**
   * ShellConflictingSessionDialog::cancel:
   * @self: the #ShellConflictingSessionDialog
   *
   * Emitted when the user cancels the login.
   */
  signals[CANCEL] =
    g_signal_new ("cancel",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellConflictingSessionDialog::force-stop:
   * @self: the #ShellConflictingSessionDialog
   *
   * Emitted when the user asks to stop the session which is running.
   */
  signals[FORCE_STOP] =
    g_signal_new ("force-stop",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_conflicting_session_dialog_init (ShellConflictingSessionDialog *self)
{
}

/**
 * shell_conflicting_session_dialog_new:
 * @conflicting_session: the logind session which is running already
 * @greeter_session: the logind session of the login screen
 *
 * Creates the dialog (`new ConflictingSessionDialog (conflictingSession,
 * greeterSession)`).
 *
 * Returns: (transfer full): a new #ShellConflictingSessionDialog
 */
ShellConflictingSessionDialog *
shell_conflicting_session_dialog_new (ShellLogin1Session *conflicting_session,
                                      ShellLogin1Session *greeter_session)
{
  ShellConflictingSessionDialog *self;
  ShellModalDialog *modal_dialog;
  const char *user_name;
  g_autofree char *banner_text = NULL;
  gboolean greeter_remote;
  gboolean conflicting_remote;
  ClutterActor *text_layout;
  ClutterActor *title;
  ClutterActor *banner;
  ClutterActor *warning_banner;

  g_return_val_if_fail (SHELL_IS_LOGIN1_SESSION (conflicting_session), NULL);
  g_return_val_if_fail (SHELL_IS_LOGIN1_SESSION (greeter_session), NULL);

  self = g_object_new (SHELL_TYPE_CONFLICTING_SESSION_DIALOG, NULL);
  modal_dialog = SHELL_MODAL_DIALOG (self);

  user_name = shell_login1_session_get_name (conflicting_session);
  greeter_remote = shell_login1_session_get_remote (greeter_session);
  conflicting_remote = shell_login1_session_get_remote (conflicting_session);

  if (greeter_remote && conflicting_remote)
    /* Translators: is running for <username> */
    banner_text = g_strdup_printf (_("Remote login is not possible because a remote session is already running for %s. To login remotely, you must log out from the remote session or force stop it."), user_name);
  else if (!greeter_remote && conflicting_remote)
    /* Translators: is running for <username> */
    banner_text = g_strdup_printf (_("Login is not possible because a remote session is already running for %s. To login, you must log out from the remote session or force stop it."), user_name);
  else if (greeter_remote && !conflicting_remote)
    /* Translators: is running for <username> */
    banner_text = g_strdup_printf (_("Remote login is not possible because a local session is already running for %s. To login remotely, you must log out from the local session or force stop it."), user_name);
  else
    /* Translators: is running for <username> */
    banner_text = g_strdup_printf (_("Login is not possible because a session is already running for %s. To login, you must log out from the session or force stop it."), user_name);

  text_layout = g_object_new (ST_TYPE_BOX_LAYOUT,
                              "style-class", "conflicting-session-dialog-content",
                              "orientation", CLUTTER_ORIENTATION_VERTICAL,
                              "x-expand", TRUE,
                              NULL);

  title = g_object_new (ST_TYPE_LABEL,
                        "text", _("Session Already Running"),
                        "style-class", "conflicting-session-dialog-title",
                        NULL);

  banner = g_object_new (ST_TYPE_LABEL,
                         "text", banner_text,
                         "style-class", "conflicting-session-dialog-desc",
                         NULL);
  clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (banner)), PANGO_ELLIPSIZE_NONE);
  clutter_text_set_line_wrap (st_label_get_clutter_text (ST_LABEL (banner)), TRUE);

  warning_banner = g_object_new (ST_TYPE_LABEL,
                                 "text", _("Force stopping will quit any running apps and processes, and could result in data loss"),
                                 "style-class", "conflicting-session-dialog-desc-warning",
                                 NULL);
  clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (warning_banner)), PANGO_ELLIPSIZE_NONE);
  clutter_text_set_line_wrap (st_label_get_clutter_text (ST_LABEL (warning_banner)), TRUE);

  clutter_actor_add_child (text_layout, title);
  clutter_actor_add_child (text_layout, banner);
  clutter_actor_add_child (text_layout, warning_banner);
  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (modal_dialog)),
                           text_layout);

  create_spinner (self);

  self->cancel_button = shell_modal_dialog_add_button (modal_dialog, _("Cancel"),
                                                       CLUTTER_KEY_Escape, TRUE,
                                                       cancel_button_cb, self, NULL);
  self->force_stop_button = shell_modal_dialog_add_button (modal_dialog, _("Force Stop"),
                                                           0, FALSE,
                                                           force_stop_button_cb, self, NULL);

  return self;
}
