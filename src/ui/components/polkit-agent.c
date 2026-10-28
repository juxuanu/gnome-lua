/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * polkit-agent.c: The user interface of the polkit authentication agent.
 * Port of js/ui/components/polkitAgent.js.
 *
 * The agent which talks to polkit is #ShellPolkitAuthenticationAgent
 * (shell-polkit-authentication-agent.c), which emits `initiate` and `cancel`.
 * This file has what the JS module had around it:
 *
 *   AuthenticationDialog   ShellPolkitDialog (final #ShellModalDialog, private)
 *   AuthenticationAgent    ShellPolkitAgentComponent (public)
 *
 * The JS AuthenticationAgent was a subclass of the native agent, the
 * component owns it now.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The first user check of a dialog (which opens it or starts the
 *    authentication) runs when the component has connected the `done` signal,
 *    so a dialog which cannot be shown dismisses its request (the JS emitted
 *    `done` from the constructor, before anyone listened).
 *  - A request which polkit cancels while it waits for the session to be
 *    unlocked is completed (the JS threw a TypeError and left the agent
 *    waiting for it), and the requests that wait for the unlock are all
 *    shown, not only the last one.
 *  - A user name that polkit cannot resolve dismisses the request (the JS
 *    threw from the constructor).
 *  - disable () closes the dialog which is open; the request itself is
 *    dismissed by the unregistration of the agent. The JS left the dialog
 *    open, and completed a request that did not exist anymore when the
 *    user answered it.
 *  - A dialog which was never shown is destroyed when its request is
 *    completed instead of staying in the stage, hidden.
 *  - A failure to register the agent (no polkit session) is a message with
 *    the reason, not an error.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <act/act.h>
#include <clutter/clutter.h>
#include <clutter/clutter-pango.h>
#include <glib/gi18n-lib.h>
#include <pango/pango.h>

#include "shell-polkit-authentication-agent.h"
#include "st.h"
#include "ui/components/polkit-agent.h"
#include "ui/dialog.h"
#include "ui/environment.h"
#include "ui/main.h"
#include "ui/modal-dialog.h"
#include "ui/session-mode.h"
#include "ui/shell-entry.h"
#include "ui/user-widget.h"

#define DIALOG_ICON_SIZE 96

#define DELAYED_RESET_TIMEOUT 200

#define MALCONTENT_SESSION_LIMITS_EXTEND "org.freedesktop.Malcontent.SessionLimits.Extend"

/* ------------------------------------------------------------------------ */
/* ShellPolkitDialog                                                        */
/* ------------------------------------------------------------------------ */

/* DialogMode */
typedef enum
{
  DIALOG_MODE_UNSET = -1,
  DIALOG_MODE_AUTH = 0,
  DIALOG_MODE_CONFIRM = 1,
} DialogMode;

#define SHELL_TYPE_POLKIT_DIALOG (shell_polkit_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellPolkitDialog, shell_polkit_dialog,
                      SHELL, POLKIT_DIALOG, ShellModalDialog)

struct _ShellPolkitDialog
{
  ShellModalDialog parent;

  char *action_id;
  char *cookie;
  PolkitIdentity *identity_to_auth;

  ActUser *user;
  gulong user_loaded_id;
  gulong user_changed_id;

  ShellSessionMode *session_mode;       /* weak */
  gulong session_updated_id;

  ShellUserAvatar *user_avatar;         /* weak */
  StLabel *user_label;                  /* weak */
  StPasswordEntry *password_entry;      /* weak */
  StLabel *error_message_label;         /* weak */
  StLabel *info_message_label;          /* weak */
  StLabel *null_message_label;          /* weak */
  StButton *cancel_button;              /* weak */
  StButton *ok_button;                  /* weak */

  PolkitAgentSession *session;
  guint session_request_timeout_id;

  DialogMode mode;
  gboolean done_emitted;
  gboolean completed;
  gboolean disposed;
};

G_DEFINE_FINAL_TYPE (ShellPolkitDialog, shell_polkit_dialog, SHELL_TYPE_MODAL_DIALOG)

enum
{
  DONE,
  N_DIALOG_SIGNALS
};

static guint dialog_signals[N_DIALOG_SIGNALS] = { 0 };

static void dialog_initiate_session (ShellPolkitDialog *self);
static void dialog_on_dialog_closed (ShellPolkitDialog *self);

/* _emitDone () */
static void
dialog_emit_done (ShellPolkitDialog *self,
                  gboolean           dismissed)
{
  if (self->done_emitted)
    return;

  self->done_emitted = TRUE;
  g_signal_emit (self, dialog_signals[DONE], 0, dismissed);
}

/* _ensureOpen () */
static void
dialog_ensure_open (ShellPolkitDialog *self)
{
  /* shell_modal_dialog_open () is safe to call if the dialog is already
   * open: it just returns TRUE without side-effects. */
  if (!shell_modal_dialog_open (SHELL_MODAL_DIALOG (self)))
    {
      /* This can fail if e.g. unable to get input grab.
       *
       * In an ideal world this wouldn't happen (because the Shell is in
       * complete control of the session) but that's just not how things
       * work right now.
       *
       * One way to make this happen is by running 'sleep 3; pkexec bash'
       * and then opening a popup menu.
       *
       * We could add retrying if this turns out to be a problem. */
      g_message ("polkitAuthenticationAgent: Failed to show modal dialog. "
                 "Dismissing authentication request for action-id %s "
                 "cookie %s", self->action_id, self->cookie);
      dialog_emit_done (self, TRUE);
    }
}

/* The reset of _destroySession (): shown after the session was dropped, until
 * the next request of the new one arrives */
static void
dialog_reset (ShellPolkitDialog *self)
{
  if (self->disposed)
    return;

  if (shell_modal_dialog_get_state (SHELL_MODAL_DIALOG (self)) != SHELL_MODAL_DIALOG_STATE_OPENED)
    return;

  if (self->password_entry != NULL)
    clutter_actor_hide (CLUTTER_ACTOR (self->password_entry));
  if (self->cancel_button != NULL)
    clutter_actor_grab_key_focus (CLUTTER_ACTOR (self->cancel_button));
  if (self->ok_button != NULL)
    clutter_actor_set_reactive (CLUTTER_ACTOR (self->ok_button), FALSE);
}

static void
dialog_reset_timeout_cb (gpointer user_data)
{
  ShellPolkitDialog *self = user_data;

  self->session_request_timeout_id = 0;
  dialog_reset (self);
}

/* The part of _destroySession () that drops the session */
static void
dialog_stop_session (ShellPolkitDialog *self)
{
  if (self->session != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->session, self);

      if (!self->completed)
        polkit_agent_session_cancel (self->session);
    }

  self->completed = FALSE;
  g_clear_object (&self->session);

  g_clear_handle_id (&self->session_request_timeout_id, g_source_remove);
}

/* _destroySession () */
static void
dialog_destroy_session (ShellPolkitDialog *self,
                        guint              delay)
{
  dialog_stop_session (self);

  if (delay > 0)
    {
      self->session_request_timeout_id =
        g_timeout_add_once (delay, dialog_reset_timeout_cb, self);
      g_source_set_name_by_id (self->session_request_timeout_id,
                               "[gnome-shell] this._sessionRequestTimeoutId");
    }
  else
    {
      dialog_reset (self);
    }
}

/* _onEntryActivate () */
static void
dialog_on_entry_activate (ShellPolkitDialog *self)
{
  const char *response;

  if (self->password_entry == NULL)
    return;

  response = st_entry_get_text (ST_ENTRY (self->password_entry));
  if (response == NULL || response[0] == '\0')
    return;

  clutter_actor_set_reactive (CLUTTER_ACTOR (self->password_entry), FALSE);
  if (self->ok_button != NULL)
    clutter_actor_set_reactive (CLUTTER_ACTOR (self->ok_button), FALSE);

  if (self->session != NULL)
    {
      /* The response is cleared by the session, copy it before */
      g_autofree char *copy = g_strdup (response);

      polkit_agent_session_response (self->session, copy);
    }

  /* When the user responds, dismiss already shown info and error texts (if
   * any) */
  clutter_actor_hide (CLUTTER_ACTOR (self->error_message_label));
  clutter_actor_hide (CLUTTER_ACTOR (self->info_message_label));
  clutter_actor_show (CLUTTER_ACTOR (self->null_message_label));
}

static void
dialog_entry_activate_cb (ClutterText       *text,
                          ShellPolkitDialog *self)
{
  dialog_on_entry_activate (self);
}

/* _onAuthenticateButtonPressed () */
static void
dialog_authenticate_button_cb (gpointer user_data)
{
  ShellPolkitDialog *self = user_data;

  if (self->mode == DIALOG_MODE_CONFIRM)
    dialog_initiate_session (self);
  else
    dialog_on_entry_activate (self);
}

/* cancel () */
static void
dialog_cancel_button_cb (gpointer user_data)
{
  dialog_emit_done (SHELL_POLKIT_DIALOG (user_data), TRUE);
}

static void
dialog_text_changed_cb (ClutterText       *text,
                        ShellPolkitDialog *self)
{
  const char *contents = clutter_text_get_text (text);

  if (self->ok_button != NULL)
    clutter_actor_set_reactive (CLUTTER_ACTOR (self->ok_button),
                                contents != NULL && contents[0] != '\0');
}

/* _onSessionCompleted () */
static void
dialog_session_completed_cb (PolkitAgentSession *session,
                             gboolean            gained_authorization,
                             ShellPolkitDialog  *self)
{
  if (self->completed || self->done_emitted)
    return;

  self->completed = TRUE;

  /* Yay, all done */
  if (gained_authorization)
    {
      dialog_emit_done (self, FALSE);
    }
  else
    {
      /* Unless we are showing an existing error message from the PAM
       * module (the PAM module could be reporting the authentication
       * error providing authentication-method specific information),
       * show "Sorry, that didn't work. Please try again."
       */
      if (!clutter_actor_is_visible (CLUTTER_ACTOR (self->error_message_label)))
        {
          /* Translators: "that didn't work" refers to the fact that the
           * requested authentication was not gained; this can happen
           * because of an authentication error (like invalid password),
           * for instance. */
          st_label_set_text (self->error_message_label,
                             _("Sorry, that didn’t work. Please try again."));
          clutter_actor_show (CLUTTER_ACTOR (self->error_message_label));
          clutter_actor_hide (CLUTTER_ACTOR (self->info_message_label));
          clutter_actor_hide (CLUTTER_ACTOR (self->null_message_label));

          shell_animation_wiggle (CLUTTER_ACTOR (self->password_entry));
        }

      /* Try and authenticate again */
      dialog_initiate_session (self);
    }
}

/* _onSessionRequest () */
static void
dialog_session_request_cb (PolkitAgentSession *session,
                           const char         *request,
                           gboolean            echo_on,
                           ShellPolkitDialog  *self)
{
  g_clear_handle_id (&self->session_request_timeout_id, g_source_remove);

  /* Hack: The request string comes directly from PAM, if it's "Password:"
   * we replace it with our own to allow localization, if it's something
   * else we remove the last colon and any trailing or leading spaces. */
  if (g_strcmp0 (request, "Password:") == 0 || g_strcmp0 (request, "Password: ") == 0)
    {
      st_entry_set_hint_text (ST_ENTRY (self->password_entry), _("Password"));
    }
  else
    {
      g_autofree char *hint = g_strdup (request != NULL ? request : "");
      char *end = hint + strlen (hint);

      /* request.replace (/: *$/, '').trim () */
      while (end > hint && end[-1] == ' ')
        end--;
      if (end > hint && end[-1] == ':')
        {
          end[-1] = '\0';
        }

      st_entry_set_hint_text (ST_ENTRY (self->password_entry), g_strstrip (hint));
    }

  st_password_entry_set_password_visible (self->password_entry, echo_on);

  clutter_actor_show (CLUTTER_ACTOR (self->password_entry));
  st_entry_set_text (ST_ENTRY (self->password_entry), "");
  clutter_actor_set_reactive (CLUTTER_ACTOR (self->password_entry), TRUE);
  clutter_actor_set_reactive (CLUTTER_ACTOR (self->ok_button), FALSE);

  dialog_ensure_open (self);
  clutter_actor_grab_key_focus (CLUTTER_ACTOR (self->password_entry));
}

/* _onSessionShowError () */
static void
dialog_session_show_error_cb (PolkitAgentSession *session,
                              const char         *text,
                              ShellPolkitDialog  *self)
{
  st_entry_set_text (ST_ENTRY (self->password_entry), "");
  st_label_set_text (self->error_message_label, text);
  clutter_actor_show (CLUTTER_ACTOR (self->error_message_label));
  clutter_actor_hide (CLUTTER_ACTOR (self->info_message_label));
  clutter_actor_hide (CLUTTER_ACTOR (self->null_message_label));
  dialog_ensure_open (self);
}

/* _onSessionShowInfo () */
static void
dialog_session_show_info_cb (PolkitAgentSession *session,
                             const char         *text,
                             ShellPolkitDialog  *self)
{
  st_entry_set_text (ST_ENTRY (self->password_entry), "");
  st_label_set_text (self->info_message_label, text);
  clutter_actor_show (CLUTTER_ACTOR (self->info_message_label));
  clutter_actor_hide (CLUTTER_ACTOR (self->error_message_label));
  clutter_actor_hide (CLUTTER_ACTOR (self->null_message_label));
  dialog_ensure_open (self);
}

/* _initiateSession () */
static void
dialog_initiate_session (ShellPolkitDialog *self)
{
  dialog_destroy_session (self, DELAYED_RESET_TIMEOUT);

  self->session = polkit_agent_session_new (self->identity_to_auth, self->cookie);
  g_signal_connect (self->session, "completed",
                    G_CALLBACK (dialog_session_completed_cb), self);
  g_signal_connect (self->session, "request",
                    G_CALLBACK (dialog_session_request_cb), self);
  g_signal_connect (self->session, "show-error",
                    G_CALLBACK (dialog_session_show_error_cb), self);
  g_signal_connect (self->session, "show-info",
                    G_CALLBACK (dialog_session_show_info_cb), self);
  polkit_agent_session_initiate (self->session);
}

/* _onUserChanged () */
static void
dialog_on_user_changed (ShellPolkitDialog *self)
{
  const char *user_name;
  const char *real_name;

  if (self->user == NULL || !act_user_is_loaded (self->user))
    return;

  user_name = act_user_get_user_name (self->user);
  real_name = act_user_get_real_name (self->user);

  if (g_strcmp0 (user_name, "root") != 0)
    st_label_set_text (self->user_label, real_name);

  shell_user_avatar_update (self->user_avatar);

  if (act_user_get_password_mode (self->user) == ACT_USER_PASSWORD_MODE_NONE)
    {
      if (self->mode == DIALOG_MODE_CONFIRM)
        return;

      self->mode = DIALOG_MODE_CONFIRM;
      dialog_destroy_session (self, 0);

      clutter_actor_set_reactive (CLUTTER_ACTOR (self->ok_button), TRUE);

      /* We normally open the dialog when we get a "request" signal, but
       * since in this case initiating a session would perform the
       * authentication, only open the dialog and initiate the session
       * when the user confirmed. */
      dialog_ensure_open (self);
    }
  else
    {
      if (self->mode == DIALOG_MODE_AUTH)
        return;

      self->mode = DIALOG_MODE_AUTH;
      dialog_initiate_session (self);
    }
}

static void
dialog_user_changed_cb (ActUser           *user,
                        ShellPolkitDialog *self)
{
  dialog_on_user_changed (self);
}

static void
dialog_user_notify_loaded_cb (ActUser           *user,
                              GParamSpec        *pspec,
                              ShellPolkitDialog *self)
{
  dialog_on_user_changed (self);
}

static void
dialog_session_mode_updated_cb (ShellSessionMode  *session_mode,
                                ShellPolkitDialog *self)
{
  g_object_set (self, "visible", !shell_session_mode_is_locked (session_mode), NULL);
}

/* The part of _onDialogClosed () and of dispose () which lets go of what the
 * dialog listens to */
static void
dialog_disconnect (ShellPolkitDialog *self)
{
  if (self->session_mode != NULL)
    g_clear_signal_handler (&self->session_updated_id, self->session_mode);
  g_clear_weak_pointer (&self->session_mode);

  if (self->user != NULL)
    {
      g_clear_signal_handler (&self->user_loaded_id, self->user);
      g_clear_signal_handler (&self->user_changed_id, self->user);
    }

  g_clear_object (&self->user);
}

/* _onDialogClosed () */
static void
dialog_on_dialog_closed (ShellPolkitDialog *self)
{
  g_clear_handle_id (&self->session_request_timeout_id, g_source_remove);

  dialog_disconnect (self);

  dialog_destroy_session (self, 0);
}

static void
dialog_closed_cb (ShellModalDialog  *dialog,
                  ShellPolkitDialog *self)
{
  dialog_on_dialog_closed (self);
}

static void
shell_polkit_dialog_close (ShellModalDialog *dialog)
{
  ShellPolkitDialog *self = SHELL_POLKIT_DIALOG (dialog);

  /* Ensure cleanup if the dialog was never shown */
  if (shell_modal_dialog_get_state (dialog) == SHELL_MODAL_DIALOG_STATE_CLOSED)
    dialog_on_dialog_closed (self);

  SHELL_MODAL_DIALOG_CLASS (shell_polkit_dialog_parent_class)->close (dialog);
}

static void
shell_polkit_dialog_dispose (GObject *object)
{
  ShellPolkitDialog *self = SHELL_POLKIT_DIALOG (object);

  self->disposed = TRUE;

  dialog_stop_session (self);
  dialog_disconnect (self);

  g_clear_weak_pointer (&self->user_avatar);
  g_clear_weak_pointer (&self->user_label);
  g_clear_weak_pointer (&self->password_entry);
  g_clear_weak_pointer (&self->error_message_label);
  g_clear_weak_pointer (&self->info_message_label);
  g_clear_weak_pointer (&self->null_message_label);
  g_clear_weak_pointer (&self->cancel_button);
  g_clear_weak_pointer (&self->ok_button);

  g_clear_object (&self->identity_to_auth);

  G_OBJECT_CLASS (shell_polkit_dialog_parent_class)->dispose (object);
}

static void
shell_polkit_dialog_finalize (GObject *object)
{
  ShellPolkitDialog *self = SHELL_POLKIT_DIALOG (object);

  g_clear_pointer (&self->action_id, g_free);
  g_clear_pointer (&self->cookie, g_free);

  G_OBJECT_CLASS (shell_polkit_dialog_parent_class)->finalize (object);
}

static void
shell_polkit_dialog_class_init (ShellPolkitDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellModalDialogClass *dialog_class = SHELL_MODAL_DIALOG_CLASS (klass);

  object_class->dispose = shell_polkit_dialog_dispose;
  object_class->finalize = shell_polkit_dialog_finalize;

  dialog_class->close = shell_polkit_dialog_close;

  /* ShellPolkitDialog::done: the user answered, or the request has to be
   * dismissed: %TRUE if it was not authorized */
  dialog_signals[DONE] =
    g_signal_new ("done",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);
}

static void
shell_polkit_dialog_init (ShellPolkitDialog *self)
{
  self->mode = DIALOG_MODE_UNSET;
}

/* Wraps a label that has to show a long text */
static void
label_set_wrap (StLabel *label)
{
  ClutterText *text = st_label_get_clutter_text (label);

  clutter_text_set_ellipsize (text, PANGO_ELLIPSIZE_NONE);
  clutter_text_set_line_wrap (text, TRUE);
}

/*
 * shell_polkit_dialog_new:
 *
 * Returns: (nullable): the dialog, %NULL if polkit knows no user @user_name
 */
static ShellPolkitDialog *
shell_polkit_dialog_new (const char         *action_id,
                         const char         *description,
                         const char         *cookie,
                         const char * const *user_names)
{
  g_autoptr (ShellPolkitDialog) keep = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (PolkitIdentity) identity = NULL;
  ShellPolkitDialog *self;
  ShellModalDialog *modal_dialog;
  ShellMain *main_obj = shell_main_get_default ();
  ShellSessionMode *session_mode = shell_main_get_session_mode (main_obj);
  ActUserManager *user_manager;
  ClutterActor *header_content;
  ClutterActor *body_content;
  ClutterActor *user_box;
  ClutterActor *avatar;
  ClutterActor *password_box;
  ClutterActor *entry;
  ClutterActor *warning_box;
  ClutterActor *caps_lock_warning;
  ClutterActor *label;
  ClutterText *text;
  const char *user_name;
  StButton *button;
  guint n_user_names = user_names != NULL ? g_strv_length ((char **) user_names) : 0;

  if (n_user_names == 0)
    {
      g_message ("polkitAuthenticationAgent: Received no identities that can "
                 "be used for authentication for action-id %s", action_id);
      return NULL;
    }

  if (n_user_names > 1)
    g_message ("polkitAuthenticationAgent: Received %u identities that can "
               "be used for authentication. Only considering one.",
               n_user_names);

  user_name = g_get_user_name ();
  if (!g_strv_contains (user_names, user_name))
    user_name = "root";
  if (!g_strv_contains (user_names, user_name))
    user_name = user_names[0];

  identity = polkit_unix_user_new_for_name (user_name, &error);
  if (identity == NULL)
    {
      g_message ("polkitAuthenticationAgent: Failed to get the identity of %s: %s",
                 user_name, error->message);
      return NULL;
    }

  self = g_object_new (SHELL_TYPE_POLKIT_DIALOG,
                       "dialog-style-class", "prompt-dialog",
                       NULL);
  keep = g_object_ref_sink (self);
  modal_dialog = SHELL_MODAL_DIALOG (self);

  self->action_id = g_strdup (action_id);
  self->cookie = g_strdup (cookie);
  self->identity_to_auth = g_steal_pointer (&identity);

  if (session_mode != NULL)
    {
      g_set_weak_pointer (&self->session_mode, session_mode);
      self->session_updated_id =
        g_signal_connect (session_mode, "updated",
                          G_CALLBACK (dialog_session_mode_updated_cb), self);
    }

  g_signal_connect (self, "closed", G_CALLBACK (dialog_closed_cb), self);

  header_content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT,
                                 "title", _("Authentication Required"),
                                 "description", description,
                                 NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (modal_dialog)),
                           header_content);

  body_content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT, NULL);

  user_manager = act_user_manager_get_default ();
  self->user = g_object_ref (act_user_manager_get_user (user_manager, user_name));

  user_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                           "style-class", "polkit-dialog-user-layout",
                           "orientation", CLUTTER_ORIENTATION_VERTICAL,
                           NULL);
  clutter_actor_add_child (body_content, user_box);

  avatar = CLUTTER_ACTOR (shell_user_avatar_new (self->user, DIALOG_ICON_SIZE));
  clutter_actor_set_x_align (avatar, CLUTTER_ACTOR_ALIGN_CENTER);
  g_set_weak_pointer (&self->user_avatar, SHELL_USER_AVATAR (avatar));
  clutter_actor_add_child (user_box, avatar);

  label = g_object_new (ST_TYPE_LABEL,
                        "style-class", g_strcmp0 (user_name, "root") == 0
                                       ? "polkit-dialog-user-root-label"
                                       : "polkit-dialog-user-label",
                        NULL);
  g_set_weak_pointer (&self->user_label, ST_LABEL (label));
  if (g_strcmp0 (user_name, "root") == 0)
    st_label_set_text (ST_LABEL (label), _("Administrator"));
  clutter_actor_add_child (user_box, label);

  password_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                               "style-class", "prompt-dialog-password-layout",
                               "orientation", CLUTTER_ORIENTATION_VERTICAL,
                               NULL);

  entry = g_object_new (ST_TYPE_PASSWORD_ENTRY,
                        "style-class", "prompt-dialog-password-entry",
                        "text", "",
                        "can-focus", TRUE,
                        "visible", FALSE,
                        "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  g_set_weak_pointer (&self->password_entry, ST_PASSWORD_ENTRY (entry));
  shell_entry_add_context_menu (ST_ENTRY (entry), SHELL_ACTION_MODE_POPUP);
  text = st_entry_get_clutter_text (ST_ENTRY (entry));
  g_signal_connect_object (text, "activate",
                           G_CALLBACK (dialog_entry_activate_cb), self, 0);
  g_object_bind_property (entry, "reactive", text, "editable", G_BINDING_SYNC_CREATE);
  clutter_actor_add_child (password_box, entry);

  warning_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                              "orientation", CLUTTER_ORIENTATION_VERTICAL,
                              NULL);

  caps_lock_warning = CLUTTER_ACTOR (shell_caps_lock_warning_new ());
  g_object_bind_property (entry, "visible", caps_lock_warning, "visible",
                          G_BINDING_SYNC_CREATE);
  clutter_actor_add_child (warning_box, caps_lock_warning);

  label = g_object_new (ST_TYPE_LABEL,
                        "style-class", "prompt-dialog-error-label",
                        "visible", FALSE,
                        NULL);
  label_set_wrap (ST_LABEL (label));
  g_set_weak_pointer (&self->error_message_label, ST_LABEL (label));
  clutter_actor_add_child (warning_box, label);

  label = g_object_new (ST_TYPE_LABEL,
                        "style-class", "prompt-dialog-info-label",
                        "visible", FALSE,
                        NULL);
  label_set_wrap (ST_LABEL (label));
  g_set_weak_pointer (&self->info_message_label, ST_LABEL (label));
  clutter_actor_add_child (warning_box, label);

  /* text is intentionally non-blank otherwise the height is not the same as for
   * infoMessage and errorMessageLabel - but it is still invisible because
   * gnome-shell.css sets the color to be transparent
   */
  label = g_object_new (ST_TYPE_LABEL,
                        "style-class", "prompt-dialog-null-label",
                        NULL);
  label_set_wrap (ST_LABEL (label));
  g_set_weak_pointer (&self->null_message_label, ST_LABEL (label));
  clutter_actor_add_child (warning_box, label);

  clutter_actor_add_child (password_box, warning_box);
  clutter_actor_add_child (body_content, password_box);

  button = shell_modal_dialog_add_button (modal_dialog, _("Cancel"), CLUTTER_KEY_Escape,
                                          FALSE, dialog_cancel_button_cb, self, NULL);
  g_set_weak_pointer (&self->cancel_button, button);
  button = shell_modal_dialog_add_button (modal_dialog, _("Authenticate"), 0,
                                          FALSE, dialog_authenticate_button_cb, self, NULL);
  g_set_weak_pointer (&self->ok_button, button);
  g_object_bind_property (button, "reactive", button, "can-focus", G_BINDING_SYNC_CREATE);

  g_signal_connect_object (text, "text-changed",
                           G_CALLBACK (dialog_text_changed_cb), self, 0);

  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (modal_dialog)),
                           body_content);

  return g_steal_pointer (&keep);
}

/*
 * The end of the constructor of the JS dialog: watches the user, which opens
 * the dialog or starts the authentication as soon as it is loaded. Called
 * once the owner is listening to ::done.
 */
static void
shell_polkit_dialog_start (ShellPolkitDialog *self)
{
  self->user_loaded_id =
    g_signal_connect (self->user, "notify::is-loaded",
                      G_CALLBACK (dialog_user_notify_loaded_cb), self);
  self->user_changed_id =
    g_signal_connect (self->user, "changed",
                      G_CALLBACK (dialog_user_changed_cb), self);
  dialog_on_user_changed (self);
}

/* ------------------------------------------------------------------------ */
/* ShellPolkitAgentComponent                                                */
/* ------------------------------------------------------------------------ */

/* A request that arrived while the session is locked */
typedef struct
{
  char *action_id;
  char *message;
  char *cookie;
  char **user_names;
} PendingRequest;

static void
pending_request_free (PendingRequest *request)
{
  g_free (request->action_id);
  g_free (request->message);
  g_free (request->cookie);
  g_strfreev (request->user_names);
  g_free (request);
}

struct _ShellPolkitAgentComponent
{
  GObject parent;

  ShellPolkitAuthenticationAgent *agent;
  ShellPolkitDialog *current_dialog;
  GList *pending;                       /* PendingRequest */

  ShellSessionMode *session_mode;       /* weak */
  gulong session_updated_id;
};

G_DEFINE_FINAL_TYPE (ShellPolkitAgentComponent, shell_polkit_agent_component, G_TYPE_OBJECT)

static void component_initiate (ShellPolkitAgentComponent *self,
                                const char                *action_id,
                                const char                *message,
                                const char                *cookie,
                                char                     **user_names);

/* Main.sessionMode.disconnectObject (this) */
static void
component_stop_waiting_for_unlock (ShellPolkitAgentComponent *self)
{
  if (self->session_mode != NULL)
    g_clear_signal_handler (&self->session_updated_id, self->session_mode);
  g_clear_weak_pointer (&self->session_mode);

  g_list_free_full (g_steal_pointer (&self->pending), (GDestroyNotify) pending_request_free);
}

static void
component_session_updated_cb (ShellSessionMode          *session_mode,
                              ShellPolkitAgentComponent *self)
{
  g_autoptr (ShellPolkitAgentComponent) keep = g_object_ref (self);
  GList *pending;
  GList *l;

  g_clear_signal_handler (&self->session_updated_id, session_mode);
  g_clear_weak_pointer (&self->session_mode);

  pending = g_steal_pointer (&self->pending);
  for (l = pending; l != NULL; l = l->next)
    {
      PendingRequest *request = l->data;

      component_initiate (self, request->action_id, request->message,
                          request->cookie, request->user_names);
    }
  g_list_free_full (pending, (GDestroyNotify) pending_request_free);
}

/* Closes and lets go of the dialog without completing the request */
static void
component_drop_dialog (ShellPolkitAgentComponent *self)
{
  g_autoptr (ShellPolkitDialog) dialog = g_steal_pointer (&self->current_dialog);

  if (dialog == NULL)
    return;

  g_signal_handlers_disconnect_by_data (dialog, self);

  if (!dialog->disposed)
    {
      shell_modal_dialog_close (SHELL_MODAL_DIALOG (dialog));

      /* A dialog which was never shown has nothing that would destroy it */
      if (!dialog->disposed &&
          shell_modal_dialog_get_state (SHELL_MODAL_DIALOG (dialog)) == SHELL_MODAL_DIALOG_STATE_CLOSED)
        clutter_actor_destroy (CLUTTER_ACTOR (dialog));
    }
}

/* _completeRequest () */
static void
component_complete_request (ShellPolkitAgentComponent *self,
                            gboolean                   dismissed)
{
  component_drop_dialog (self);
  component_stop_waiting_for_unlock (self);

  shell_polkit_authentication_agent_complete (self->agent, dismissed);
}

static void
component_dialog_done_cb (ShellPolkitDialog         *dialog,
                          gboolean                   dismissed,
                          ShellPolkitAgentComponent *self)
{
  component_complete_request (self, dismissed);
}

/* _onInitiate () */
static void
component_initiate (ShellPolkitAgentComponent *self,
                    const char                *action_id,
                    const char                *message,
                    const char                *cookie,
                    char                     **user_names)
{
  ShellSessionMode *session_mode = shell_main_get_session_mode (shell_main_get_default ());
  ShellPolkitDialog *dialog;

  /* Don't pop up a dialog while locked, unless it's triggered by user
   * action from the lock screen, such as extending the session limits */
  if (session_mode != NULL && shell_session_mode_is_locked (session_mode) &&
      g_strcmp0 (action_id, MALCONTENT_SESSION_LIMITS_EXTEND) != 0)
    {
      PendingRequest *request = g_new0 (PendingRequest, 1);

      request->action_id = g_strdup (action_id);
      request->message = g_strdup (message);
      request->cookie = g_strdup (cookie);
      request->user_names = g_strdupv (user_names);
      self->pending = g_list_append (self->pending, request);

      if (self->session_updated_id == 0)
        {
          g_set_weak_pointer (&self->session_mode, session_mode);
          self->session_updated_id =
            g_signal_connect (session_mode, "updated",
                              G_CALLBACK (component_session_updated_cb), self);
        }
      return;
    }

  component_drop_dialog (self);

  dialog = shell_polkit_dialog_new (action_id, message, cookie,
                                    (const char * const *) user_names);
  if (dialog == NULL)
    {
      shell_polkit_authentication_agent_complete (self->agent, TRUE);
      return;
    }

  self->current_dialog = dialog;
  g_signal_connect (dialog, "done", G_CALLBACK (component_dialog_done_cb), self);
  shell_polkit_dialog_start (dialog);
}

static void
component_initiate_cb (ShellPolkitAuthenticationAgent *agent,
                       const char                     *action_id,
                       const char                     *message,
                       const char                     *icon_name,
                       const char                     *cookie,
                       char                          **user_names,
                       ShellPolkitAgentComponent      *self)
{
  component_initiate (self, action_id, message, cookie, user_names);
}

/* _onCancel () */
static void
component_cancel_cb (ShellPolkitAuthenticationAgent *agent,
                     ShellPolkitAgentComponent      *self)
{
  component_complete_request (self, FALSE);
}

static void
shell_polkit_agent_component_dispose (GObject *object)
{
  ShellPolkitAgentComponent *self = SHELL_POLKIT_AGENT_COMPONENT (object);

  component_stop_waiting_for_unlock (self);
  component_drop_dialog (self);

  if (self->agent != NULL)
    g_signal_handlers_disconnect_by_data (self->agent, self);
  g_clear_object (&self->agent);

  G_OBJECT_CLASS (shell_polkit_agent_component_parent_class)->dispose (object);
}

static void
shell_polkit_agent_component_class_init (ShellPolkitAgentComponentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_polkit_agent_component_dispose;
}

static void
shell_polkit_agent_component_init (ShellPolkitAgentComponent *self)
{
  self->agent = shell_polkit_authentication_agent_new ();

  g_signal_connect (self->agent, "initiate",
                    G_CALLBACK (component_initiate_cb), self);
  g_signal_connect (self->agent, "cancel",
                    G_CALLBACK (component_cancel_cb), self);
}

/**
 * shell_polkit_agent_component_new:
 *
 * Creates the component which asks the user to authenticate when polkit
 * needs an authorization (`new AuthenticationAgent ()`). It registers the
 * agent with polkit once it is enabled.
 *
 * Returns: (transfer full): a new #ShellPolkitAgentComponent
 */
ShellPolkitAgentComponent *
shell_polkit_agent_component_new (void)
{
  return g_object_new (SHELL_TYPE_POLKIT_AGENT_COMPONENT, NULL);
}

/**
 * shell_polkit_agent_component_enable:
 * @self: a #ShellPolkitAgentComponent
 *
 * Registers the authentication agent with polkit (`enable ()`). Not being
 * able to, because there is no polkit session, is a message.
 */
void
shell_polkit_agent_component_enable (ShellPolkitAgentComponent *self)
{
  g_autoptr (GError) error = NULL;

  g_return_if_fail (SHELL_IS_POLKIT_AGENT_COMPONENT (self));

  if (self->agent == NULL)
    return;

  shell_polkit_authentication_agent_register (self->agent, &error);
  if (error != NULL)
    g_message ("Failed to register AuthenticationAgent: %s", error->message);
}

/**
 * shell_polkit_agent_component_disable:
 * @self: a #ShellPolkitAgentComponent
 *
 * Unregisters the authentication agent (`disable ()`), which dismisses the
 * request which is being answered, and closes its dialog.
 */
void
shell_polkit_agent_component_disable (ShellPolkitAgentComponent *self)
{
  g_return_if_fail (SHELL_IS_POLKIT_AGENT_COMPONENT (self));

  if (self->agent == NULL)
    return;

  component_stop_waiting_for_unlock (self);
  component_drop_dialog (self);

  shell_polkit_authentication_agent_unregister (self->agent);
}
