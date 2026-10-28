/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * kbd-a11y-dialog.c: The dialog which tells that Slow Keys or Sticky Keys
 * were turned on or off with a keyboard shortcut, and asks whether to keep
 * the change. Port of js/ui/kbdA11yDialog.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - A GSettings schema that is not installed leaves the answer buttons
 *    without effect (the dialog just closes) instead of throwing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <clutter/clutter.h>
#include <meta/meta-enums.h>

#include "shell-global.h"
#include "ui/dialog.h"
#include "ui/kbd-a11y-dialog.h"
#include "ui/modal-dialog.h"
#include "ui/status/status-private.h"

#define KEYBOARD_A11Y_SCHEMA    "org.gnome.desktop.a11y.keyboard"
#define KEY_STICKY_KEYS_ENABLED "stickykeys-enable"
#define KEY_SLOW_KEYS_ENABLED   "slowkeys-enable"

struct _ShellKbdA11yDialog
{
  GObject parent;

  GSettings *a11y_settings;
  ClutterSeat *seat;            /* weak */
};

G_DEFINE_FINAL_TYPE (ShellKbdA11yDialog, shell_kbd_a11y_dialog, G_TYPE_OBJECT)

/* What a button of the dialog does */
typedef struct
{
  GSettings *settings;
  const char *key;
  gboolean value;
  GWeakRef dialog;
} ButtonData;

static void
button_data_free (ButtonData *data)
{
  g_clear_object (&data->settings);
  g_weak_ref_clear (&data->dialog);
  g_free (data);
}

static void
button_cb (gpointer user_data)
{
  ButtonData *data = user_data;
  g_autoptr (ShellModalDialog) dialog = g_weak_ref_get (&data->dialog);

  if (data->settings != NULL)
    g_settings_set_boolean (data->settings, data->key, data->value);

  if (dialog != NULL)
    shell_modal_dialog_close (dialog);
}

static void
add_answer_button (ShellKbdA11yDialog *self,
                   ShellModalDialog   *dialog,
                   const char         *label,
                   uint32_t            key_symbol,
                   gboolean            is_default,
                   const char         *key,
                   gboolean            value)
{
  ButtonData *data = g_new0 (ButtonData, 1);

  data->settings = self->a11y_settings != NULL ? g_object_ref (self->a11y_settings) : NULL;
  data->key = key;
  data->value = value;
  g_weak_ref_init (&data->dialog, dialog);

  shell_modal_dialog_add_button (dialog, label, key_symbol, is_default,
                                 button_cb, data, (GDestroyNotify) button_data_free);
}

/* _showKbdA11yDialog () */
static void
kbd_a11y_flags_changed_cb (ClutterSeat        *seat,
                           guint               new_flags,
                           guint               what_changed,
                           ShellKbdA11yDialog *self)
{
  ShellModalDialog *dialog;
  ShellMessageDialogContent *content;
  const char *title, *description;
  const char *key;
  gboolean enabled;

  if (what_changed & META_A11Y_SLOW_KEYS_ENABLED)
    {
      key = KEY_SLOW_KEYS_ENABLED;
      enabled = (new_flags & META_A11Y_SLOW_KEYS_ENABLED) > 0;
      title = enabled ? _("Slow Keys Turned On")
                      : _("Slow Keys Turned Off");
      description = _("You just held down the Shift key for 8 seconds. This is the shortcut "
                      "for the Slow Keys feature, which affects the way your keyboard works.");
    }
  else if (what_changed & META_A11Y_STICKY_KEYS_ENABLED)
    {
      key = KEY_STICKY_KEYS_ENABLED;
      enabled = (new_flags & META_A11Y_STICKY_KEYS_ENABLED) > 0;
      title = enabled ? _("Sticky Keys Turned On")
                      : _("Sticky Keys Turned Off");
      description = enabled
        ? _("You just pressed the Shift key 5 times in a row. This is the shortcut "
            "for the Sticky Keys feature, which affects the way your keyboard works.")
        : _("You just pressed two keys at once, or pressed the Shift key 5 times in a row. "
            "This turns off the Sticky Keys feature, which affects the way your keyboard works.");
    }
  else
    {
      return;
    }

  dialog = g_object_new (SHELL_TYPE_MODAL_DIALOG, NULL);

  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT,
                          "title", title,
                          "description", description,
                          NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (dialog)),
                           CLUTTER_ACTOR (content));

  add_answer_button (self, dialog,
                     enabled ? _("Leave On") : _("Turn On"),
                     !enabled ? CLUTTER_KEY_Escape : 0,
                     enabled,
                     key, TRUE);

  add_answer_button (self, dialog,
                     enabled ? _("Turn Off") : _("Leave Off"),
                     enabled ? CLUTTER_KEY_Escape : 0,
                     !enabled,
                     key, FALSE);

  shell_modal_dialog_open (dialog);
}

static void
shell_kbd_a11y_dialog_constructed (GObject *object)
{
  ShellKbdA11yDialog *self = SHELL_KBD_A11Y_DIALOG (object);
  ShellGlobal *global = shell_global_get ();
  ClutterContext *context = clutter_actor_get_context (CLUTTER_ACTOR (shell_global_get_stage (global)));
  ClutterBackend *backend = clutter_context_get_backend (context);

  G_OBJECT_CLASS (shell_kbd_a11y_dialog_parent_class)->constructed (object);

  self->a11y_settings = shell_status_settings_new (KEYBOARD_A11Y_SCHEMA);

  g_set_weak_pointer (&self->seat, clutter_backend_get_default_seat (backend));
  g_signal_connect_object (self->seat, "kbd-a11y-flags-changed",
                           G_CALLBACK (kbd_a11y_flags_changed_cb), self, 0);
}

static void
shell_kbd_a11y_dialog_dispose (GObject *object)
{
  ShellKbdA11yDialog *self = SHELL_KBD_A11Y_DIALOG (object);

  g_clear_weak_pointer (&self->seat);
  g_clear_object (&self->a11y_settings);

  G_OBJECT_CLASS (shell_kbd_a11y_dialog_parent_class)->dispose (object);
}

static void
shell_kbd_a11y_dialog_class_init (ShellKbdA11yDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_kbd_a11y_dialog_constructed;
  object_class->dispose = shell_kbd_a11y_dialog_dispose;
}

static void
shell_kbd_a11y_dialog_init (ShellKbdA11yDialog *self)
{
}

/**
 * shell_kbd_a11y_dialog_new:
 *
 * Creates the object which shows the dialog when the keyboard shortcuts of
 * Slow Keys and Sticky Keys change their state (`new KbdA11yDialog ()`).
 *
 * Returns: (transfer full): a new #ShellKbdA11yDialog
 */
ShellKbdA11yDialog *
shell_kbd_a11y_dialog_new (void)
{
  return g_object_new (SHELL_TYPE_KBD_A11Y_DIALOG, NULL);
}
