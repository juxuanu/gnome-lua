/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * keyring.c: The user interface of the system prompter of gnome-keyring.
 * Port of js/ui/components/keyring.js.
 *
 * The prompts are #ShellKeyringPrompt (shell-keyring-prompt.c), which emits
 * `show-password`, `show-confirm` and `prompt-close`. This file has what the
 * JS module had around it:
 *
 *   KeyringDialog        ShellKeyringDialog (final #ShellModalDialog, private)
 *   KeyringDummyDialog   a prompt which cancels what it is asked (private)
 *   KeyringPrompter      ShellKeyringComponent (public)
 *
 * The JS KeyringPrompter was a subclass of #GcrSystemPrompter, the component
 * owns one.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - A dialog which was never shown is destroyed when its prompt is closed
 *    instead of staying in the stage, hidden.
 *  - The dialog lets go of its prompt when it is destroyed: the properties
 *    of the prompt are not bound to destroyed widgets anymore.
 *  - Without a session bus the prompter is not registered, which is a
 *    message (the JS threw from enable ()).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <clutter/clutter.h>
#include <clutter/clutter-pango.h>
#define GCR_API_SUBJECT_TO_CHANGE
#include <gcr/gcr.h>
#include <gio/gio.h>
#include <glib/gi18n-lib.h>
#include <pango/pango.h>

#include "shell-keyring-prompt.h"
#include "st.h"
#include "ui/check-box.h"
#include "ui/components/keyring.h"
#include "ui/dialog.h"
#include "ui/environment.h"
#include "ui/modal-dialog.h"
#include "ui/shell-entry.h"

#define SYSTEM_PROMPTER_BUS_NAME "org.gnome.keyring.SystemPrompter"

/* ------------------------------------------------------------------------ */
/* ShellKeyringDialog                                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_KEYRING_DIALOG (shell_keyring_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellKeyringDialog, shell_keyring_dialog,
                      SHELL, KEYRING_DIALOG, ShellModalDialog)

struct _ShellKeyringDialog
{
  ShellModalDialog parent;

  ShellKeyringPrompt *prompt;
  GPtrArray *bindings;                  /* GBinding */

  StPasswordEntry *password_entry;      /* weak */
  StPasswordEntry *confirm_entry;       /* weak */
  ShellCapsLockWarning *caps_lock_warning;  /* weak */
  StLabel *warning;                     /* weak */
  StButton *cancel_button;              /* weak */
  StButton *continue_button;            /* weak */

  gboolean disposed;
};

G_DEFINE_FINAL_TYPE (ShellKeyringDialog, shell_keyring_dialog, SHELL_TYPE_MODAL_DIALOG)

static void
dialog_bind (ShellKeyringDialog *self,
             gpointer            target,
             const char         *source_property,
             const char         *target_property,
             GBindingFlags       flags)
{
  GBinding *binding = g_object_bind_property (self->prompt, source_property,
                                              target, target_property, flags);

  g_ptr_array_add (self->bindings, g_object_ref (binding));
}

/* _updateSensitivity () */
static void
dialog_update_sensitivity (ShellKeyringDialog *self,
                           gboolean            sensitive)
{
  if (self->password_entry != NULL)
    clutter_actor_set_reactive (CLUTTER_ACTOR (self->password_entry), sensitive);

  if (self->confirm_entry != NULL)
    clutter_actor_set_reactive (CLUTTER_ACTOR (self->confirm_entry), sensitive);

  if (self->continue_button != NULL)
    {
      st_widget_set_can_focus (ST_WIDGET (self->continue_button), sensitive);
      clutter_actor_set_reactive (CLUTTER_ACTOR (self->continue_button), sensitive);
    }
}

/* _ensureOpen () */
static gboolean
dialog_ensure_open (ShellKeyringDialog *self)
{
  /* shell_modal_dialog_open () is safe to call if the dialog is already
   * open: it just returns TRUE without side-effects. */
  if (shell_modal_dialog_open (SHELL_MODAL_DIALOG (self)))
    return TRUE;

  /* The above fail if e.g. unable to get input grab.
   *
   * In an ideal world this wouldn't happen (because the Shell is in
   * complete control of the session) but that's just not how things work
   * right now. */
  g_message ("keyringPrompt: Failed to show modal dialog. "
             "Dismissing prompt request");
  shell_keyring_prompt_cancel (self->prompt);
  return FALSE;
}

/* _onShowPassword () */
static void
dialog_show_password_cb (ShellKeyringPrompt *prompt,
                         ShellKeyringDialog *self)
{
  dialog_ensure_open (self);
  dialog_update_sensitivity (self, TRUE);
  st_entry_set_text (ST_ENTRY (self->password_entry), "");
  clutter_actor_grab_key_focus (CLUTTER_ACTOR (self->password_entry));
}

/* _onShowConfirm () */
static void
dialog_show_confirm_cb (ShellKeyringPrompt *prompt,
                        ShellKeyringDialog *self)
{
  dialog_ensure_open (self);
  dialog_update_sensitivity (self, TRUE);
  st_entry_set_text (ST_ENTRY (self->confirm_entry), "");
  clutter_actor_grab_key_focus (CLUTTER_ACTOR (self->continue_button));
}

/* _onHidePrompt () */
static void
dialog_prompt_close_cb (ShellKeyringPrompt *prompt,
                        ShellKeyringDialog *self)
{
  ShellModalDialog *dialog = SHELL_MODAL_DIALOG (self);
  g_autoptr (ShellKeyringDialog) keep = g_object_ref (self);

  shell_modal_dialog_close (dialog);

  /* A dialog which was never shown has nothing that would destroy it */
  if (!self->disposed &&
      shell_modal_dialog_get_state (dialog) == SHELL_MODAL_DIALOG_STATE_CLOSED)
    clutter_actor_destroy (CLUTTER_ACTOR (self));
}

/* _onContinueButton () */
static void
dialog_on_continue (ShellKeyringDialog *self)
{
  dialog_update_sensitivity (self, FALSE);
  if (shell_keyring_prompt_complete (self->prompt))
    return;

  /* Allow user to correct it when passwords mismatch. */
  dialog_update_sensitivity (self, TRUE);
  if (shell_keyring_prompt_get_password_visible (self->prompt))
    clutter_actor_grab_key_focus (CLUTTER_ACTOR (self->password_entry));
}

static void
dialog_continue_button_cb (gpointer user_data)
{
  dialog_on_continue (SHELL_KEYRING_DIALOG (user_data));
}

/* _onCancelButton () */
static void
dialog_cancel_button_cb (gpointer user_data)
{
  shell_keyring_prompt_cancel (SHELL_KEYRING_DIALOG (user_data)->prompt);
}

/* _onPasswordActivate () */
static void
dialog_password_activate_cb (ClutterText        *text,
                             ShellKeyringDialog *self)
{
  if (shell_keyring_prompt_get_confirm_visible (self->prompt))
    clutter_actor_grab_key_focus (CLUTTER_ACTOR (self->confirm_entry));
  else
    dialog_on_continue (self);
}

/* _onConfirmActivate () */
static void
dialog_confirm_activate_cb (ClutterText        *text,
                            ShellKeyringDialog *self)
{
  dialog_on_continue (self);
}

/* syncCapsLockWarningVisibility () */
static void
dialog_sync_caps_lock_warning_visibility (ShellKeyringDialog *self)
{
  if (self->caps_lock_warning == NULL)
    return;

  g_object_set (self->caps_lock_warning,
                "visible", shell_keyring_prompt_get_password_visible (self->prompt) ||
                           shell_keyring_prompt_get_confirm_visible (self->prompt),
                NULL);
}

static void
dialog_notify_visible_cb (GObject            *object,
                          GParamSpec         *pspec,
                          ShellKeyringDialog *self)
{
  dialog_sync_caps_lock_warning_visibility (self);
}

static void
dialog_notify_warning_visible_cb (GObject            *object,
                                  GParamSpec         *pspec,
                                  ShellKeyringDialog *self)
{
  if (self->warning == NULL)
    return;

  clutter_actor_set_opacity (CLUTTER_ACTOR (self->warning),
                             shell_keyring_prompt_get_warning_visible (self->prompt) ? 255 : 0);
}

static void
dialog_notify_warning_cb (GObject            *object,
                          GParamSpec         *pspec,
                          ShellKeyringDialog *self)
{
  g_autofree char *warning = NULL;

  if (self->password_entry == NULL)
    return;

  g_object_get (self->prompt, "warning", &warning, NULL);
  if (g_strcmp0 (warning, "") != 0)
    shell_animation_wiggle (CLUTTER_ACTOR (self->password_entry));
}

static void
shell_keyring_dialog_dispose (GObject *object)
{
  ShellKeyringDialog *self = SHELL_KEYRING_DIALOG (object);

  self->disposed = TRUE;

  if (self->bindings != NULL)
    {
      guint i;

      for (i = 0; i < self->bindings->len; i++)
        g_binding_unbind (g_ptr_array_index (self->bindings, i));
      g_ptr_array_set_size (self->bindings, 0);
    }

  if (self->prompt != NULL)
    g_signal_handlers_disconnect_by_data (self->prompt, self);
  g_clear_object (&self->prompt);

  g_clear_weak_pointer (&self->password_entry);
  g_clear_weak_pointer (&self->confirm_entry);
  g_clear_weak_pointer (&self->caps_lock_warning);
  g_clear_weak_pointer (&self->warning);
  g_clear_weak_pointer (&self->cancel_button);
  g_clear_weak_pointer (&self->continue_button);

  G_OBJECT_CLASS (shell_keyring_dialog_parent_class)->dispose (object);
}

static void
shell_keyring_dialog_finalize (GObject *object)
{
  ShellKeyringDialog *self = SHELL_KEYRING_DIALOG (object);

  g_clear_pointer (&self->bindings, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_keyring_dialog_parent_class)->finalize (object);
}

static void
shell_keyring_dialog_class_init (ShellKeyringDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_keyring_dialog_dispose;
  object_class->finalize = shell_keyring_dialog_finalize;
}

static void
shell_keyring_dialog_init (ShellKeyringDialog *self)
{
  self->bindings = g_ptr_array_new_with_free_func (g_object_unref);
}

static ShellKeyringDialog *
shell_keyring_dialog_new (void)
{
  ShellKeyringDialog *self = g_object_new (SHELL_TYPE_KEYRING_DIALOG,
                                           "dialog-style-class", "prompt-dialog",
                                           NULL);
  ShellModalDialog *modal_dialog = SHELL_MODAL_DIALOG (self);
  ClutterActor *content;
  ClutterActor *password_box;
  ClutterActor *entry;
  ClutterActor *warning_box;
  ClutterActor *caps_lock_warning;
  ClutterActor *warning;
  ClutterActor *choice;
  ClutterText *text;
  StButton *button;

  self->prompt = shell_keyring_prompt_new ();
  g_signal_connect_object (self->prompt, "show-password",
                           G_CALLBACK (dialog_show_password_cb), self, 0);
  g_signal_connect_object (self->prompt, "show-confirm",
                           G_CALLBACK (dialog_show_confirm_cb), self, 0);
  g_signal_connect_object (self->prompt, "prompt-close",
                           G_CALLBACK (dialog_prompt_close_cb), self, 0);

  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT, NULL);

  dialog_bind (self, content, "message", "title", G_BINDING_SYNC_CREATE);
  dialog_bind (self, content, "description", "description", G_BINDING_SYNC_CREATE);

  password_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                               "style-class", "prompt-dialog-password-layout",
                               "orientation", CLUTTER_ORIENTATION_VERTICAL,
                               NULL);

  entry = g_object_new (ST_TYPE_PASSWORD_ENTRY,
                        "style-class", "prompt-dialog-password-entry",
                        "can-focus", TRUE,
                        "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  g_set_weak_pointer (&self->password_entry, ST_PASSWORD_ENTRY (entry));
  shell_entry_add_context_menu (ST_ENTRY (entry), SHELL_ACTION_MODE_POPUP);
  text = st_entry_get_clutter_text (ST_ENTRY (entry));
  g_signal_connect_object (text, "activate",
                           G_CALLBACK (dialog_password_activate_cb), self, 0);
  dialog_bind (self, entry, "password-visible", "visible", G_BINDING_SYNC_CREATE);
  clutter_actor_add_child (password_box, entry);
  shell_keyring_prompt_set_password_actor (self->prompt, text);

  entry = g_object_new (ST_TYPE_PASSWORD_ENTRY,
                        "style-class", "prompt-dialog-password-entry",
                        "can-focus", TRUE,
                        "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  g_set_weak_pointer (&self->confirm_entry, ST_PASSWORD_ENTRY (entry));
  shell_entry_add_context_menu (ST_ENTRY (entry), SHELL_ACTION_MODE_POPUP);
  text = st_entry_get_clutter_text (ST_ENTRY (entry));
  g_signal_connect_object (text, "activate",
                           G_CALLBACK (dialog_confirm_activate_cb), self, 0);
  dialog_bind (self, entry, "confirm-visible", "visible", G_BINDING_SYNC_CREATE);
  clutter_actor_add_child (password_box, entry);
  shell_keyring_prompt_set_confirm_actor (self->prompt, text);

  warning_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                              "orientation", CLUTTER_ORIENTATION_VERTICAL,
                              NULL);

  caps_lock_warning = CLUTTER_ACTOR (shell_caps_lock_warning_new ());
  g_set_weak_pointer (&self->caps_lock_warning, SHELL_CAPS_LOCK_WARNING (caps_lock_warning));
  g_signal_connect_object (self->prompt, "notify::password-visible",
                           G_CALLBACK (dialog_notify_visible_cb), self, 0);
  g_signal_connect_object (self->prompt, "notify::confirm-visible",
                           G_CALLBACK (dialog_notify_visible_cb), self, 0);
  clutter_actor_add_child (warning_box, caps_lock_warning);

  warning = g_object_new (ST_TYPE_LABEL,
                          "style-class", "prompt-dialog-error-label",
                          NULL);
  clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (warning)), PANGO_ELLIPSIZE_NONE);
  clutter_text_set_line_wrap (st_label_get_clutter_text (ST_LABEL (warning)), TRUE);
  g_set_weak_pointer (&self->warning, ST_LABEL (warning));
  dialog_bind (self, warning, "warning", "text", G_BINDING_SYNC_CREATE);
  g_signal_connect_object (self->prompt, "notify::warning-visible",
                           G_CALLBACK (dialog_notify_warning_visible_cb), self, 0);
  g_signal_connect_object (self->prompt, "notify::warning",
                           G_CALLBACK (dialog_notify_warning_cb), self, 0);
  clutter_actor_add_child (warning_box, warning);

  clutter_actor_add_child (password_box, warning_box);
  clutter_actor_add_child (content, password_box);

  choice = CLUTTER_ACTOR (shell_check_box_new (NULL));
  dialog_bind (self, shell_check_box_get_label_actor (SHELL_CHECK_BOX (choice)),
               "choice-label", "text", G_BINDING_SYNC_CREATE);
  dialog_bind (self, choice, "choice-chosen", "checked",
               G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  dialog_bind (self, choice, "choice-visible", "visible", G_BINDING_SYNC_CREATE);
  clutter_actor_add_child (content, choice);

  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (modal_dialog)),
                           content);

  button = shell_modal_dialog_add_button (modal_dialog, "", CLUTTER_KEY_Escape, FALSE,
                                          dialog_cancel_button_cb, self, NULL);
  g_set_weak_pointer (&self->cancel_button, button);
  dialog_bind (self, button, "cancel-label", "label", G_BINDING_SYNC_CREATE);

  button = shell_modal_dialog_add_button (modal_dialog, "", 0, TRUE,
                                          dialog_continue_button_cb, self, NULL);
  g_set_weak_pointer (&self->continue_button, button);
  dialog_bind (self, button, "continue-label", "label", G_BINDING_SYNC_CREATE);

  dialog_sync_caps_lock_warning_visibility (self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* KeyringDummyDialog                                                       */
/* ------------------------------------------------------------------------ */

/* _cancelPrompt () */
static void
dummy_cancel_prompt_cb (ShellKeyringPrompt *prompt,
                        gpointer            user_data)
{
  shell_keyring_prompt_cancel (prompt);
}

/* ------------------------------------------------------------------------ */
/* ShellKeyringComponent                                                    */
/* ------------------------------------------------------------------------ */

struct _ShellKeyringComponent
{
  GObject parent;

  GcrSystemPrompter *prompter;
  GDBusConnection *connection;
  guint dbus_id;
  gboolean registered;
  gboolean enabled;

  GcrPrompt *current_prompt;            /* weak */
  GcrPrompt *dummy_prompt;              /* kept until the next prompt is created */
};

G_DEFINE_FINAL_TYPE (ShellKeyringComponent, shell_keyring_component, G_TYPE_OBJECT)

/* The `new-prompt` handler of the constructor of the JS prompter */
static GcrPrompt *
component_new_prompt_cb (GcrSystemPrompter     *prompter,
                         ShellKeyringComponent *self)
{
  GcrPrompt *prompt;

  g_clear_object (&self->dummy_prompt);

  if (self->enabled)
    {
      ShellKeyringDialog *dialog = shell_keyring_dialog_new ();

      prompt = GCR_PROMPT (dialog->prompt);
    }
  else
    {
      ShellKeyringPrompt *dummy = shell_keyring_prompt_new ();

      g_signal_connect (dummy, "show-password",
                        G_CALLBACK (dummy_cancel_prompt_cb), NULL);
      g_signal_connect (dummy, "show-confirm",
                        G_CALLBACK (dummy_cancel_prompt_cb), NULL);
      prompt = GCR_PROMPT (dummy);
      self->dummy_prompt = prompt;
    }

  g_clear_weak_pointer (&self->current_prompt);
  g_set_weak_pointer (&self->current_prompt, prompt);

  return prompt;
}

static void
shell_keyring_component_dispose (GObject *object)
{
  ShellKeyringComponent *self = SHELL_KEYRING_COMPONENT (object);

  self->enabled = FALSE;
  g_clear_weak_pointer (&self->current_prompt);
  g_clear_object (&self->dummy_prompt);

  if (self->prompter != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->prompter, self);
      if (self->registered)
        gcr_system_prompter_unregister (self->prompter, FALSE);
    }
  self->registered = FALSE;
  g_clear_object (&self->prompter);

  if (self->dbus_id != 0 && self->connection != NULL)
    g_bus_unown_name (self->dbus_id);
  self->dbus_id = 0;
  g_clear_object (&self->connection);

  G_OBJECT_CLASS (shell_keyring_component_parent_class)->dispose (object);
}

static void
shell_keyring_component_class_init (ShellKeyringComponentClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = shell_keyring_component_dispose;
}

static void
shell_keyring_component_init (ShellKeyringComponent *self)
{
  self->prompter = g_object_new (GCR_TYPE_SYSTEM_PROMPTER,
                                 "mode", GCR_SYSTEM_PROMPTER_SINGLE,
                                 "prompt-type", SHELL_TYPE_KEYRING_PROMPT,
                                 NULL);
  g_signal_connect (self->prompter, "new-prompt",
                    G_CALLBACK (component_new_prompt_cb), self);
}

/**
 * shell_keyring_component_new:
 *
 * Creates the component which shows the prompts of the system prompter of
 * gnome-keyring (`new KeyringPrompter ()`). It exports the prompter on the
 * session bus once it is enabled.
 *
 * Returns: (transfer full): a new #ShellKeyringComponent
 */
ShellKeyringComponent *
shell_keyring_component_new (void)
{
  return g_object_new (SHELL_TYPE_KEYRING_COMPONENT, NULL);
}

/**
 * shell_keyring_component_enable:
 * @self: a #ShellKeyringComponent
 *
 * Exports the system prompter on the session bus and owns its name, if that
 * was not done yet, and shows the prompts from now on (`enable ()`). Without
 * a session bus the prompts are not served, which is a message.
 */
void
shell_keyring_component_enable (ShellKeyringComponent *self)
{
  g_return_if_fail (SHELL_IS_KEYRING_COMPONENT (self));

  if (!self->registered)
    {
      g_autoptr (GError) error = NULL;

      self->connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
      if (self->connection == NULL)
        {
          g_message ("Failed to register the keyring prompter: %s", error->message);
          return;
        }

      gcr_system_prompter_register (self->prompter, self->connection);
      self->dbus_id = g_bus_own_name_on_connection (self->connection,
                                                    SYSTEM_PROMPTER_BUS_NAME,
                                                    G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
                                                    NULL, NULL, NULL, NULL);
      self->registered = TRUE;
    }

  self->enabled = TRUE;
}

/**
 * shell_keyring_component_disable:
 * @self: a #ShellKeyringComponent
 *
 * Cancels the prompt which is being answered and cancels the new prompts
 * instead of showing them (`disable ()`).
 */
void
shell_keyring_component_disable (ShellKeyringComponent *self)
{
  g_return_if_fail (SHELL_IS_KEYRING_COMPONENT (self));

  self->enabled = FALSE;

  if (self->current_prompt != NULL &&
      gcr_system_prompter_get_prompting (self->prompter))
    shell_keyring_prompt_cancel (SHELL_KEYRING_PROMPT (self->current_prompt));

  g_clear_weak_pointer (&self->current_prompt);
}
