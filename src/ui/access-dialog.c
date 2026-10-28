/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * access-dialog.c: The dialog of the org.freedesktop.impl.portal.Access
 * backend. Port of js/ui/accessDialog.js.
 *
 * #ShellAccessDialogDBus exports the Access interface (a gdbus-codegen
 * skeleton, #ShellPortalAccess) at /org/freedesktop/portal/desktop of the
 * session bus; the methods can only be called by the well-known names of
 * ALLOWED_SENDERS. Each AccessDialog () call shows a #ShellAccessDialog, which
 * exports the org.freedesktop.impl.portal.Request object of the call
 * (#ShellPortalRequest) at the handle given by the caller while it is open.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - Request.Close () works: the JS method took its two arguments in the
 *    wrong order and failed with a TypeError for every caller, so the
 *    portal could never close a dialog. The caller must still be the one
 *    that opened the dialog, and the AccessDialog () call is answered with
 *    the "closed" response (2).
 *  - A call whose app id does not match the focus app is denied also when
 *    there is no focus app (the JS threw a TypeError, which is an error
 *    reply too).
 *  - If the dialog cannot be opened (the shell cannot grab the input) the
 *    call fails; the JS never answered it.
 *  - A call which is still waiting for the answer of the user when the
 *    dialog is destroyed (the shell shuts down) is answered with the
 *    "closed" response.
 *  - The request object is not exported if the handle is not a valid object
 *    path or is in use: the call is still shown and answered.
 *  - The calls that wait for the sender check fail after
 *    ShellGlobal::shutdown.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <clutter/clutter-pango.h>
#include <glib/gi18n-lib.h>

#include "misc/dbus-utils.h"
#include "shell-global.h"
#include "shell-app.h"
#include "shell-window-tracker.h"
#include "ui/access-dialog.h"
#include "ui/check-box.h"
#include "ui/dialog.h"
#include "ui/modal-dialog.h"

#include "portal-access-dbus.h"

#define ACCESS_OBJECT_PATH "/org/freedesktop/portal/desktop"

static const char * const ALLOWED_SENDERS[] = {
  "org.gnome.RemoteDesktop.Handover",
  "org.freedesktop.impl.portal.desktop.gnome",
  NULL
};

typedef enum
{
  DIALOG_RESPONSE_OK = 0,
  DIALOG_RESPONSE_CANCEL = 1,
  DIALOG_RESPONSE_CLOSED = 2,
} DialogResponse;

/* ------------------------------------------------------------------------ */
/* ShellAccessDialog                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_ACCESS_DIALOG (shell_access_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellAccessDialog, shell_access_dialog,
                      SHELL, ACCESS_DIALOG, ShellModalDialog)

typedef struct
{
  char *id;
  ShellCheckBox *check; /* weak, owned by the content */
} Choice;

struct _ShellAccessDialog
{
  ShellModalDialog parent;

  GDBusMethodInvocation *invocation;   /* of AccessDialog (), until answered */
  char *sender;
  char *handle;

  ShellPortalRequest *request;         /* exported while open */
  GPtrArray *choices;                  /* of Choice */
  gboolean response_sent;
};

G_DEFINE_FINAL_TYPE (ShellAccessDialog, shell_access_dialog, SHELL_TYPE_MODAL_DIALOG)

static void
choice_free (Choice *choice)
{
  g_clear_weak_pointer (&choice->check);
  g_free (choice->id);
  g_free (choice);
}

static void
answer_invocation (ShellAccessDialog *self,
                   DialogResponse     response,
                   GVariant          *results)
{
  g_autoptr (GDBusMethodInvocation) invocation = g_steal_pointer (&self->invocation);
  g_autoptr (GVariant) owned_results = g_variant_ref_sink (results);

  if (invocation == NULL)
    return;

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(u@a{sv})",
                                                        (guint32) response,
                                                        owned_results));
}

static void
unexport_request (ShellAccessDialog *self)
{
  if (self->request == NULL)
    return;

  g_signal_handlers_disconnect_by_data (self->request, self);
  if (g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (self->request)) != NULL)
    g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->request));
  g_clear_object (&self->request);
}

typedef struct
{
  DialogResponse response;
  GVariant *results;
} PendingResponse;

static void
pending_response_free (PendingResponse *pending)
{
  g_clear_pointer (&pending->results, g_variant_unref);
  g_free (pending);
}

static void
send_response_on_closed_cb (ShellAccessDialog *self,
                            PendingResponse   *pending)
{
  /* Delay the actual response until the end of the close animation (if any) */
  answer_invocation (self, pending->response, g_steal_pointer (&pending->results));
}

static void
send_response (ShellAccessDialog *self,
               DialogResponse     response)
{
  GVariantBuilder builder;
  PendingResponse *pending;
  guint i;

  if (self->response_sent)
    return;
  self->response_sent = TRUE;

  unexport_request (self);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  if (response == DIALOG_RESPONSE_OK)
    {
      for (i = 0; i < self->choices->len; i++)
        {
          Choice *choice = g_ptr_array_index (self->choices, i);
          gboolean checked;

          if (choice->check == NULL)
            continue;

          checked = st_button_get_checked (ST_BUTTON (choice->check));
          g_variant_builder_add (&builder, "{sv}", choice->id,
                                 g_variant_new_string (checked ? "true" : "false"));
        }
    }

  pending = g_new0 (PendingResponse, 1);
  pending->response = response;
  pending->results = g_variant_ref_sink (g_variant_builder_end (&builder));
  g_signal_connect_data (self, "closed",
                         G_CALLBACK (send_response_on_closed_cb),
                         pending,
                         (GClosureNotify) pending_response_free,
                         G_CONNECT_AFTER);

  shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
}

static void
deny_button_cb (gpointer user_data)
{
  send_response (user_data, DIALOG_RESPONSE_CANCEL);
}

static void
grant_button_cb (gpointer user_data)
{
  send_response (user_data, DIALOG_RESPONSE_OK);
}

static gboolean
on_handle_request_close (ShellPortalRequest    *request,
                         GDBusMethodInvocation *invocation,
                         ShellAccessDialog     *self)
{
  if (self->invocation == NULL ||
      g_strcmp0 (self->sender, g_dbus_method_invocation_get_sender (invocation)) != 0)
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_DBUS_ERROR,
                                                     G_DBUS_ERROR_ACCESS_DENIED,
                                                     "");
      return TRUE;
    }

  send_response (self, DIALOG_RESPONSE_CLOSED);
  shell_portal_request_complete_close (request, invocation);
  return TRUE;
}

static gboolean
shell_access_dialog_open (ShellModalDialog *dialog)
{
  ShellAccessDialog *self = SHELL_ACCESS_DIALOG (dialog);
  g_autoptr (GError) error = NULL;
  GDBusConnection *connection;

  if (!SHELL_MODAL_DIALOG_CLASS (shell_access_dialog_parent_class)->open (dialog))
    return FALSE;

  if (self->request != NULL || self->invocation == NULL)
    return TRUE;

  connection = g_dbus_method_invocation_get_connection (self->invocation);
  self->request = shell_portal_request_skeleton_new ();
  g_signal_connect (self->request, "handle-close",
                    G_CALLBACK (on_handle_request_close), self);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->request),
                                         connection, self->handle, &error))
    {
      g_message ("%s: cannot export the request %s: %s",
                 G_STRFUNC, self->handle, error->message);
      unexport_request (self);
    }

  return TRUE;
}

/* The checkboxes of the "choices" option, a(ssa(ss)s) of (id, name, options,
 * selected). The ones with options would be radio buttons, which are not
 * implemented */
static void
add_choices (ShellAccessDialog         *self,
             ShellMessageDialogContent *content,
             GVariant                  *choices)
{
  GVariantIter iter;
  const char *id, *name, *selected;
  GVariant *choice_options;

  g_variant_iter_init (&iter, choices);
  while (g_variant_iter_next (&iter, "(&s&s@a(ss)&s)", &id, &name, &choice_options, &selected))
    {
      g_autoptr (GVariant) choice_options_owner = choice_options;
      ShellCheckBox *check;
      Choice *choice;

      if (g_variant_n_children (choice_options) > 0)
        continue; /* radio buttons, not implemented */

      check = shell_check_box_new (name);
      st_button_set_checked (ST_BUTTON (check), g_strcmp0 (selected, "true") == 0);
      clutter_actor_add_child (CLUTTER_ACTOR (content), CLUTTER_ACTOR (check));

      choice = g_new0 (Choice, 1);
      choice->id = g_strdup (id);
      g_set_weak_pointer (&choice->check, check);
      g_ptr_array_add (self->choices, choice);
    }
}

static void
build_layout (ShellAccessDialog *self,
              const char        *title,
              const char        *description,
              const char        *body,
              GVariant          *options)
{
  ShellModalDialog *dialog = SHELL_MODAL_DIALOG (self);
  ShellMessageDialogContent *content;
  const char *deny_label = NULL;
  const char *grant_label = NULL;
  g_autoptr (GVariant) choices = NULL;

  /* No support for non-modal system dialogs, so ignore the "modal" option */
  if (!g_variant_lookup (options, "deny_label", "&s", &deny_label) ||
      deny_label[0] == '\0')
    deny_label = _("Deny");
  if (!g_variant_lookup (options, "grant_label", "&s", &grant_label) ||
      grant_label[0] == '\0')
    grant_label = _("Allow");
  choices = g_variant_lookup_value (options, "choices", G_VARIANT_TYPE ("a(ssa(ss)s)"));

  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT,
                          "title", title,
                          "description", description,
                          NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (dialog)),
                           CLUTTER_ACTOR (content));

  if (choices != NULL)
    add_choices (self, content, choices);

  if (body != NULL && body[0] != '\0')
    {
      ClutterActor *body_label;
      ClutterText *text;

      body_label = g_object_new (ST_TYPE_LABEL,
                                 "text", body,
                                 "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                 NULL);
      text = st_label_get_clutter_text (ST_LABEL (body_label));
      clutter_text_set_ellipsize (text, PANGO_ELLIPSIZE_NONE);
      clutter_text_set_line_wrap (text, TRUE);
      clutter_actor_add_child (CLUTTER_ACTOR (content), body_label);
    }

  shell_modal_dialog_add_button (dialog, deny_label, CLUTTER_KEY_Escape, FALSE,
                                 deny_button_cb, self, NULL);
  shell_modal_dialog_add_button (dialog, grant_label, 0, FALSE,
                                 grant_button_cb, self, NULL);
}

static void
shell_access_dialog_dispose (GObject *object)
{
  ShellAccessDialog *self = SHELL_ACCESS_DIALOG (object);

  unexport_request (self);

  /* Closed while waiting for the user: shutdown */
  if (self->invocation != NULL)
    answer_invocation (self, DIALOG_RESPONSE_CLOSED,
                       g_variant_new ("a{sv}", NULL));

  G_OBJECT_CLASS (shell_access_dialog_parent_class)->dispose (object);
}

static void
shell_access_dialog_finalize (GObject *object)
{
  ShellAccessDialog *self = SHELL_ACCESS_DIALOG (object);

  g_clear_pointer (&self->choices, g_ptr_array_unref);
  g_clear_pointer (&self->sender, g_free);
  g_clear_pointer (&self->handle, g_free);

  G_OBJECT_CLASS (shell_access_dialog_parent_class)->finalize (object);
}

static void
shell_access_dialog_class_init (ShellAccessDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellModalDialogClass *dialog_class = SHELL_MODAL_DIALOG_CLASS (klass);

  object_class->dispose = shell_access_dialog_dispose;
  object_class->finalize = shell_access_dialog_finalize;

  dialog_class->open = shell_access_dialog_open;
}

static void
shell_access_dialog_init (ShellAccessDialog *self)
{
  self->choices = g_ptr_array_new_with_free_func ((GDestroyNotify) choice_free);
}

static ShellAccessDialog *
shell_access_dialog_new (GDBusMethodInvocation *invocation,
                         const char            *handle,
                         const char            *title,
                         const char            *description,
                         const char            *body,
                         GVariant              *options)
{
  ShellAccessDialog *self;

  self = g_object_new (SHELL_TYPE_ACCESS_DIALOG,
                       "dialog-style-class", "access-dialog",
                       NULL);
  self->invocation = g_object_ref (invocation);
  self->sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  self->handle = g_strdup (handle);

  build_layout (self, title, description, body, options);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellAccessDialogDBus                                                    */
/* ------------------------------------------------------------------------ */

struct _ShellAccessDialogDBus
{
  GObject parent;

  ShellPortalAccess *skeleton;
  ShellDBusSenderChecker *sender_checker;
  ShellAccessDialog *access_dialog;    /* weak */
  GCancellable *cancellable;           /* cancelled on shutdown */
};

G_DEFINE_FINAL_TYPE (ShellAccessDialogDBus, shell_access_dialog_dbus, G_TYPE_OBJECT)

typedef struct
{
  ShellAccessDialogDBus *self;
  GDBusMethodInvocation *invocation;
  char *handle;
  char *app_id;
  char *title;
  char *description;
  char *body;
  GVariant *options;
} AccessCall;

static void
access_call_free (AccessCall *call)
{
  g_clear_object (&call->self);
  g_clear_object (&call->invocation);
  g_free (call->handle);
  g_free (call->app_id);
  g_free (call->title);
  g_free (call->description);
  g_free (call->body);
  g_clear_pointer (&call->options, g_variant_unref);
  g_free (call);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AccessCall, access_call_free)

static void
access_dialog_closed_cb (ShellAccessDialog      *dialog,
                         ShellAccessDialogDBus  *self)
{
  g_clear_weak_pointer (&self->access_dialog);
}

static void
run_access_call (ShellAccessDialogDBus *self,
                 AccessCall            *call)
{
  ShellWindowTracker *tracker = shell_global_get_window_tracker (shell_global_get ());
  ShellAccessDialog *dialog;

  if (self->access_dialog != NULL)
    {
      g_dbus_method_invocation_return_error_literal (call->invocation,
                                                     G_DBUS_ERROR,
                                                     G_DBUS_ERROR_LIMITS_EXCEEDED,
                                                     "Already showing a system access dialog");
      return;
    }

  /* We probably want to use parentWindow and global.display.focus_window
   * for this check in the future */
  if (call->app_id[0] != '\0')
    {
      ShellApp *focus_app = shell_window_tracker_get_focus_app (tracker);
      g_autofree char *desktop_id = g_strdup_printf ("%s.desktop", call->app_id);

      if (focus_app == NULL ||
          g_strcmp0 (desktop_id, shell_app_get_id (focus_app)) != 0)
        {
          g_dbus_method_invocation_return_error_literal (call->invocation,
                                                         G_DBUS_ERROR,
                                                         G_DBUS_ERROR_ACCESS_DENIED,
                                                         "Only the focused app is allowed to show a system access dialog");
          return;
        }
    }

  dialog = shell_access_dialog_new (call->invocation, call->handle,
                                    call->title, call->description,
                                    call->body, call->options);
  g_set_weak_pointer (&self->access_dialog, dialog);
  g_signal_connect_object (dialog, "closed",
                           G_CALLBACK (access_dialog_closed_cb), self, 0);

  if (!shell_modal_dialog_open (SHELL_MODAL_DIALOG (dialog)))
    {
      g_autoptr (GDBusMethodInvocation) invocation = g_steal_pointer (&dialog->invocation);

      g_clear_weak_pointer (&self->access_dialog);
      clutter_actor_destroy (CLUTTER_ACTOR (dialog));
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_DBUS_ERROR,
                                                     G_DBUS_ERROR_FAILED,
                                                     "Cannot grab pointer and keyboard");
    }
}

static void
on_sender_checked (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  g_autoptr (AccessCall) call = user_data;
  g_autoptr (GError) error = NULL;

  if (!shell_dbus_sender_checker_check_invocation_finish (SHELL_DBUS_SENDER_CHECKER (source),
                                                          result, &error))
    {
      g_dbus_method_invocation_return_gerror (call->invocation, error);
      return;
    }

  run_access_call (call->self, call);
}

static gboolean
on_handle_access_dialog (ShellPortalAccess      *skeleton,
                         GDBusMethodInvocation  *invocation,
                         const char             *handle,
                         const char             *app_id,
                         const char             *parent_window,
                         const char             *title,
                         const char             *subtitle,
                         const char             *body,
                         GVariant               *options,
                         ShellAccessDialogDBus  *self)
{
  AccessCall *call = g_new0 (AccessCall, 1);

  call->self = g_object_ref (self);
  call->invocation = g_object_ref (invocation);
  call->handle = g_strdup (handle);
  call->app_id = g_strdup (app_id);
  call->title = g_strdup (title);
  call->description = g_strdup (subtitle);
  call->body = g_strdup (body);
  call->options = g_variant_ref (options);

  shell_dbus_sender_checker_check_invocation_async (self->sender_checker, invocation,
                                                    self->cancellable,
                                                    on_sender_checked, call);
  return TRUE;
}

static void
on_shutdown (ShellGlobal           *global,
             ShellAccessDialogDBus *self)
{
  g_cancellable_cancel (self->cancellable);
}

static void
shell_access_dialog_dbus_constructed (GObject *object)
{
  ShellAccessDialogDBus *self = SHELL_ACCESS_DIALOG_DBUS (object);
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GError) error = NULL;

  G_OBJECT_CLASS (shell_access_dialog_dbus_parent_class)->constructed (object);

  self->cancellable = g_cancellable_new ();
  self->sender_checker = shell_dbus_sender_checker_new (ALLOWED_SENDERS);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  self->skeleton = shell_portal_access_skeleton_new ();
  g_signal_connect (self->skeleton, "handle-access-dialog",
                    G_CALLBACK (on_handle_access_dialog), self);

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (connection == NULL ||
      !g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                         connection, ACCESS_OBJECT_PATH, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

static void
shell_access_dialog_dbus_dispose (GObject *object)
{
  ShellAccessDialogDBus *self = SHELL_ACCESS_DIALOG_DBUS (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  if (self->skeleton != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->skeleton, self);
      if (g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (self->skeleton)) != NULL)
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->skeleton));
      g_clear_object (&self->skeleton);
    }

  g_clear_object (&self->sender_checker);
  g_clear_object (&self->cancellable);
  g_clear_weak_pointer (&self->access_dialog);

  G_OBJECT_CLASS (shell_access_dialog_dbus_parent_class)->dispose (object);
}

static void
shell_access_dialog_dbus_class_init (ShellAccessDialogDBusClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_access_dialog_dbus_constructed;
  object_class->dispose = shell_access_dialog_dbus_dispose;
}

static void
shell_access_dialog_dbus_init (ShellAccessDialogDBus *self)
{
}

/**
 * shell_access_dialog_dbus_new:
 *
 * Exports org.freedesktop.impl.portal.Access at /org/freedesktop/portal/desktop
 * on the session bus (`new AccessDialogDBus ()`).
 *
 * Returns: (transfer full): a new #ShellAccessDialogDBus
 */
ShellAccessDialogDBus *
shell_access_dialog_dbus_new (void)
{
  return g_object_new (SHELL_TYPE_ACCESS_DIALOG_DBUS, NULL);
}
