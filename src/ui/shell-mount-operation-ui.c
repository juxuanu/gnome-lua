/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-mount-operation-ui.c: The dialogs of the mount operations. Port of
 * js/ui/shellMountOperation.js.
 *
 * Three dialogs, private to this file, answer to the user:
 *
 *  - `ShellMountQuestionDialog` asks a question with one button per choice;
 *  - `ShellMountPasswordDialog` asks for a password (and, for a TrueCrypt
 *    volume, the PIM, a hidden or system volume and the keyfiles);
 *  - `ShellProcessesDialog` lists the applications which keep a volume busy.
 *
 * They emit ::response (the index of the choice, or -1 if the user dismissed
 * the dialog; the password dialog adds the answers) and stay open until the
 * owner closes them.
 *
 * #ShellMountOperationUI (`ShellMountOperation` of the JS module) connects a
 * #ShellMountOperation, which the volume is mounted with, to these dialogs
 * and to the notification of the unmount progress; the automount manager
 * creates it for each volume. #ShellMountOpHandler (`GnomeShellMountOpHandler`)
 * exports org.Gtk.MountOperationHandler (a gdbus-codegen skeleton,
 * #ShellMountOperationHandler) at /org/gtk/MountOperationHandler of the
 * session bus and owns the name of the same name; the same dialogs show the
 * requests of other processes.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The signals of the automount manager that connected the JS
 *    `ShellMountOperation` objects (create-mount-operation,
 *    close-mount-operation and borrow-dialog) are gone: the manager holds a
 *    #ShellMountOperationUI per volume and calls shell_mount_operation_ui_close ()
 *    and shell_mount_operation_ui_borrow_dialog () itself.
 *  - The PIM is parsed like `Number ()` and `parseInt ()` did for the usual
 *    inputs (decimal numbers, spaces around): other JS number syntaxes such as
 *    hexadecimal or exponents are not accepted, and a negative number
 *    wraps around like the conversion of a JS number to a D-Bus uint32.
 *  - Toggling "Uses Keyfiles" without the remember check box (the dialog is
 *    not asked to save the password) works; the JS threw.
 *  - A mount operation drops its dialogs without closing them when it is
 *    finalized, like the JS object that was garbage collected; use
 *    shell_mount_operation_ui_close ().
 *  - The call of the handler that is waiting for the user is answered with
 *    "unhandled" when the handler is disposed.
 *  - A request of the handler is answered with an error when the dialog
 *    cannot be opened (the JS ignored the failure to grab the input).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <clutter/clutter-pango.h>
#include <glib/gi18n-lib.h>
#include <string.h>

#include "shell-app.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "shell-mount-operation.h"
#include "shell-window-tracker.h"
#include "ui/animation.h"
#include "ui/check-box.h"
#include "ui/dialog.h"
#include "ui/environment.h"
#include "ui/main.h"
#include "ui/message-tray.h"
#include "ui/shell-entry.h"
#include "ui/shell-mount-operation-ui.h"

#include "mount-operation-handler-dbus.h"

#define LIST_ITEM_ICON_SIZE 48
#define WORK_SPINNER_ICON_SIZE 16

#define REMEMBER_MOUNT_PASSWORD_KEY "remember-mount-password"

#define MOUNT_OP_HANDLER_BUS_NAME "org.gtk.MountOperationHandler"
#define MOUNT_OP_HANDLER_OBJECT_PATH "/org/gtk/MountOperationHandler"

/* ------------------------------------------------------------------------ */
/* Common utils                                                             */
/* ------------------------------------------------------------------------ */

static void
set_visible (ClutterActor *actor,
             gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

typedef struct
{
  GObject *dialog;
  int index;
} ChoiceButton;

static void
choice_button_cb (gpointer user_data)
{
  ChoiceButton *button = user_data;

  g_signal_emit_by_name (button->dialog, "response", button->index);
}

/* Shows one button per choice; the first choice is the rightmost button. The
 * buttons are only replaced if the choices changed */
static void
set_buttons_for_choices (ShellModalDialog  *dialog,
                         char            ***old_choices,
                         const char * const *choices)
{
  guint n_choices = choices != NULL ? g_strv_length ((char **) choices) : 0;
  gboolean buttons_changed;
  int i;

  buttons_changed = g_strv_length (*old_choices) != n_choices ||
                    (n_choices > 0 && !g_strv_equal ((const char * const *) *old_choices, choices));

  if (buttons_changed)
    {
      shell_modal_dialog_clear_buttons (dialog);

      for (i = (int) n_choices - 1; i >= 0; i--)
        {
          ChoiceButton *button = g_new0 (ChoiceButton, 1);

          button->dialog = G_OBJECT (dialog);
          button->index = i;
          shell_modal_dialog_add_button (dialog, choices[i], 0, FALSE,
                                         choice_button_cb, button, g_free);
        }
    }

  g_strfreev (*old_choices);
  *old_choices = g_strdupv ((char **) choices);
  if (*old_choices == NULL)
    *old_choices = g_new0 (char *, 1);
}

/* The first line of @message is the title, the others the description */
static void
set_labels_for_message (ShellMessageDialogContent *content,
                        const char                *message)
{
  g_auto (GStrv) lines = g_strsplit (message, "\n", 2);

  g_object_set (content,
                "title", lines[0],
                "description", lines[1] != NULL ? lines[1] : "",
                NULL);
}

static gboolean
escape_binding_cb (GObject             *object,
                   const char          *action_name,
                   guint                key_val,
                   ClutterModifierType  modifiers,
                   gpointer             user_data)
{
  g_signal_emit_by_name (object, "response", -1);
  return CLUTTER_EVENT_STOP;
}

static void
install_escape_binding (gpointer klass)
{
  ClutterBindingPool *binding_pool = clutter_binding_pool_get_for_class (klass);

  clutter_binding_pool_install_action (binding_pool, "close",
                                       CLUTTER_KEY_Escape, CLUTTER_RELEASE_MASK,
                                       G_CALLBACK (escape_binding_cb),
                                       NULL, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellMountQuestionDialog                                                 */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MOUNT_QUESTION_DIALOG (shell_mount_question_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellMountQuestionDialog, shell_mount_question_dialog,
                      SHELL, MOUNT_QUESTION_DIALOG, ShellModalDialog)

struct _ShellMountQuestionDialog
{
  ShellModalDialog parent;

  ShellMessageDialogContent *content;  /* weak */
  char **old_choices;
};

G_DEFINE_FINAL_TYPE (ShellMountQuestionDialog, shell_mount_question_dialog,
                     SHELL_TYPE_MODAL_DIALOG)

static void
shell_mount_question_dialog_update (ShellMountQuestionDialog *self,
                                    const char               *message,
                                    const char * const       *choices)
{
  if (self->content == NULL)
    return;

  set_labels_for_message (self->content, message);
  set_buttons_for_choices (SHELL_MODAL_DIALOG (self), &self->old_choices, choices);
}

static void
shell_mount_question_dialog_constructed (GObject *object)
{
  ShellMountQuestionDialog *self = SHELL_MOUNT_QUESTION_DIALOG (object);
  ShellMessageDialogContent *content;

  G_OBJECT_CLASS (shell_mount_question_dialog_parent_class)->constructed (object);

  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT, NULL);
  g_set_weak_pointer (&self->content, content);
  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (SHELL_MODAL_DIALOG (self))),
                           CLUTTER_ACTOR (content));
}

static void
shell_mount_question_dialog_dispose (GObject *object)
{
  ShellMountQuestionDialog *self = SHELL_MOUNT_QUESTION_DIALOG (object);

  g_clear_weak_pointer (&self->content);

  G_OBJECT_CLASS (shell_mount_question_dialog_parent_class)->dispose (object);
}

static void
shell_mount_question_dialog_finalize (GObject *object)
{
  ShellMountQuestionDialog *self = SHELL_MOUNT_QUESTION_DIALOG (object);

  g_strfreev (self->old_choices);

  G_OBJECT_CLASS (shell_mount_question_dialog_parent_class)->finalize (object);
}

static void
shell_mount_question_dialog_class_init (ShellMountQuestionDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_mount_question_dialog_constructed;
  object_class->dispose = shell_mount_question_dialog_dispose;
  object_class->finalize = shell_mount_question_dialog_finalize;

  /* ShellMountQuestionDialog::response (choice: int): -1 if dismissed */
  g_signal_new ("response",
                G_TYPE_FROM_CLASS (klass),
                G_SIGNAL_RUN_LAST,
                0, NULL, NULL, NULL,
                G_TYPE_NONE, 1, G_TYPE_INT);

  install_escape_binding (klass);
}

static void
shell_mount_question_dialog_init (ShellMountQuestionDialog *self)
{
  self->old_choices = g_new0 (char *, 1);
}

static ShellMountQuestionDialog *
shell_mount_question_dialog_new (void)
{
  return g_object_new (SHELL_TYPE_MOUNT_QUESTION_DIALOG,
                       "dialog-style-class", "mount-question-dialog",
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellProcessesDialog                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_PROCESSES_DIALOG (shell_processes_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellProcessesDialog, shell_processes_dialog,
                      SHELL, PROCESSES_DIALOG, ShellModalDialog)

struct _ShellProcessesDialog
{
  ShellModalDialog parent;

  ShellMessageDialogContent *content;     /* weak */
  ShellListSection *application_section;  /* weak */
  char **old_choices;
};

G_DEFINE_FINAL_TYPE (ShellProcessesDialog, shell_processes_dialog,
                     SHELL_TYPE_MODAL_DIALOG)

static void
set_apps_for_pids (ShellProcessesDialog *self,
                   GArray               *pids)
{
  ShellWindowTracker *tracker = shell_window_tracker_get_default ();
  StBoxLayout *list = shell_list_section_get_list (self->application_section);
  guint i;

  /* Remove all the items */
  clutter_actor_destroy_all_children (CLUTTER_ACTOR (list));

  for (i = 0; pids != NULL && i < pids->len; i++)
    {
      ShellApp *app = shell_window_tracker_get_app_from_pid (tracker,
                                                              g_array_index (pids, GPid, i));
      ClutterActor *list_item;

      if (app == NULL)
        continue;

      list_item = g_object_new (SHELL_TYPE_LIST_SECTION_ITEM,
                                "icon-actor", shell_app_create_icon_texture (app, LIST_ITEM_ICON_SIZE),
                                "title", shell_app_get_name (app),
                                NULL);
      clutter_actor_add_child (CLUTTER_ACTOR (list), list_item);
    }

  set_visible (CLUTTER_ACTOR (self->application_section),
               clutter_actor_get_n_children (CLUTTER_ACTOR (list)) > 0);
}

static void
shell_processes_dialog_update (ShellProcessesDialog *self,
                               const char           *message,
                               GArray               *processes,
                               const char * const   *choices)
{
  if (self->content == NULL || self->application_section == NULL)
    return;

  set_apps_for_pids (self, processes);
  set_labels_for_message (self->content, message);
  set_buttons_for_choices (SHELL_MODAL_DIALOG (self), &self->old_choices, choices);
}

static void
shell_processes_dialog_constructed (GObject *object)
{
  ShellProcessesDialog *self = SHELL_PROCESSES_DIALOG (object);
  ClutterActor *content_layout;
  ShellMessageDialogContent *content;
  ShellListSection *section;

  G_OBJECT_CLASS (shell_processes_dialog_parent_class)->constructed (object);

  content_layout = CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (SHELL_MODAL_DIALOG (self)));

  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT, NULL);
  g_set_weak_pointer (&self->content, content);
  clutter_actor_add_child (content_layout, CLUTTER_ACTOR (content));

  section = g_object_new (SHELL_TYPE_LIST_SECTION, NULL);
  g_set_weak_pointer (&self->application_section, section);
  clutter_actor_hide (CLUTTER_ACTOR (section));
  clutter_actor_add_child (content_layout, CLUTTER_ACTOR (section));
}

static void
shell_processes_dialog_dispose (GObject *object)
{
  ShellProcessesDialog *self = SHELL_PROCESSES_DIALOG (object);

  g_clear_weak_pointer (&self->content);
  g_clear_weak_pointer (&self->application_section);

  G_OBJECT_CLASS (shell_processes_dialog_parent_class)->dispose (object);
}

static void
shell_processes_dialog_finalize (GObject *object)
{
  ShellProcessesDialog *self = SHELL_PROCESSES_DIALOG (object);

  g_strfreev (self->old_choices);

  G_OBJECT_CLASS (shell_processes_dialog_parent_class)->finalize (object);
}

static void
shell_processes_dialog_class_init (ShellProcessesDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_processes_dialog_constructed;
  object_class->dispose = shell_processes_dialog_dispose;
  object_class->finalize = shell_processes_dialog_finalize;

  /* ShellProcessesDialog::response (choice: int): -1 if dismissed */
  g_signal_new ("response",
                G_TYPE_FROM_CLASS (klass),
                G_SIGNAL_RUN_LAST,
                0, NULL, NULL, NULL,
                G_TYPE_NONE, 1, G_TYPE_INT);

  install_escape_binding (klass);
}

static void
shell_processes_dialog_init (ShellProcessesDialog *self)
{
  self->old_choices = g_new0 (char *, 1);
}

static ShellProcessesDialog *
shell_processes_dialog_new (void)
{
  return g_object_new (SHELL_TYPE_PROCESSES_DIALOG,
                       "dialog-style-class", "processes-dialog",
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellMountPasswordDialog                                                 */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MOUNT_PASSWORD_DIALOG (shell_mount_password_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellMountPasswordDialog, shell_mount_password_dialog,
                      SHELL, MOUNT_PASSWORD_DIALOG, ShellModalDialog)

enum
{
  PASSWORD_PROP_0,

  PASSWORD_PROP_MESSAGE,
  PASSWORD_PROP_FLAGS,

  PASSWORD_N_PROPS
};

static GParamSpec *password_props[PASSWORD_N_PROPS] = { NULL, };

struct _ShellMountPasswordDialog
{
  ShellModalDialog parent;

  char *message;
  GAskPasswordFlags flags;

  ShellCheckBox *hidden_volume;       /* weak, TCRYPT only */
  ShellCheckBox *system_volume;       /* weak, TCRYPT only */
  ShellCheckBox *keyfiles_checkbox;   /* weak, TCRYPT only */
  StLabel *keyfiles_label;            /* weak, TCRYPT only */
  StEntry *pim_entry;                 /* weak, TCRYPT only */
  StEntry *password_entry;            /* weak */
  ShellSpinner *work_spinner;         /* weak */
  StLabel *error_message_label;       /* weak */
  ShellCheckBox *remember_choice;     /* weak, if saving is supported */

  ShellApp *disks_app;                /* the Disks application, if installed */
};

G_DEFINE_FINAL_TYPE (ShellMountPasswordDialog, shell_mount_password_dialog,
                     SHELL_TYPE_MODAL_DIALOG)

static void
password_dialog_set_default_buttons (ShellMountPasswordDialog *self);

static void
password_dialog_emit_cancel (ShellMountPasswordDialog *self)
{
  g_signal_emit_by_name (self, "response", -1, "", FALSE, FALSE, FALSE, 0u);
}

static void
cancel_button_cb (gpointer user_data)
{
  password_dialog_emit_cancel (user_data);
}

static void
open_disks_button_cb (gpointer user_data)
{
  ShellMountPasswordDialog *self = user_data;

  shell_app_activate (self->disks_app);
  password_dialog_emit_cancel (self);
}

/* Whether @text is empty or a decimal number, like `isNaN (text)` is false
 * for it; @value gets what `parseInt (text)` returns (0 if empty) */
static gboolean
parse_pim (const char *text,
           gint64     *value)
{
  g_autofree char *stripped = g_strstrip (g_strdup (text));
  char *end = NULL;
  const char *p;

  *value = 0;

  if (stripped[0] == '\0')
    return TRUE;

  g_ascii_strtod (stripped, &end);
  if (end == stripped || *end != '\0')
    return FALSE;

  /* hexadecimal, infinity and nan are not decimal numbers */
  for (p = stripped; *p != '\0'; p++)
    {
      if (!g_ascii_isdigit (*p) && *p != '+' && *p != '-' && *p != '.' &&
          *p != 'e' && *p != 'E')
        return FALSE;
    }

  /* parseInt () reads the integer part and stops at ".", "e" */
  *value = g_ascii_strtoll (stripped, NULL, 10);
  return TRUE;
}

static void
password_dialog_entry_activate (ShellMountPasswordDialog *self)
{
  ShellGlobal *global = shell_global_get ();
  gint64 pim = 0;
  gboolean remember;
  gboolean hidden_volume;
  gboolean system_volume;

  if (self->pim_entry != NULL)
    {
      if (!parse_pim (st_entry_get_text (self->pim_entry), &pim))
        {
          st_entry_set_text (self->pim_entry, "");
          st_label_set_text (self->error_message_label, _("The PIM must be a number or empty"));
          clutter_actor_set_opacity (CLUTTER_ACTOR (self->error_message_label), 255);
          return;
        }

      clutter_actor_set_opacity (CLUTTER_ACTOR (self->error_message_label), 0);
    }

  remember = self->remember_choice != NULL &&
             st_button_get_checked (ST_BUTTON (self->remember_choice));
  hidden_volume = self->hidden_volume != NULL &&
                  st_button_get_checked (ST_BUTTON (self->hidden_volume));
  system_volume = self->system_volume != NULL &&
                  st_button_get_checked (ST_BUTTON (self->system_volume));

  g_settings_set_boolean (shell_global_get_settings (global),
                          REMEMBER_MOUNT_PASSWORD_KEY, remember);

  shell_spinner_play (self->work_spinner);
  g_signal_emit_by_name (self, "response", 1,
                         st_entry_get_text (self->password_entry),
                         remember, hidden_volume, system_volume,
                         (guint) pim);
}

static void
entry_activate_cb (ClutterText              *text,
                   ShellMountPasswordDialog *self)
{
  password_dialog_entry_activate (self);
}

static void
unlock_button_cb (gpointer user_data)
{
  password_dialog_entry_activate (user_data);
}

static void
password_dialog_set_default_buttons (ShellMountPasswordDialog *self)
{
  ShellModalDialog *dialog = SHELL_MODAL_DIALOG (self);

  shell_modal_dialog_clear_buttons (dialog);
  shell_modal_dialog_add_button (dialog, _("Cancel"), CLUTTER_KEY_Escape, FALSE,
                                 cancel_button_cb, self, NULL);
  shell_modal_dialog_add_button (dialog, _("Unlock"), 0, TRUE,
                                 unlock_button_cb, self, NULL);
}

static void
password_dialog_set_keyfiles_buttons (ShellMountPasswordDialog *self)
{
  ShellModalDialog *dialog = SHELL_MODAL_DIALOG (self);

  shell_modal_dialog_clear_buttons (dialog);
  shell_modal_dialog_add_button (dialog, _("Cancel"), CLUTTER_KEY_Escape, FALSE,
                                 cancel_button_cb, self, NULL);

  if (self->disks_app != NULL)
    {
      /* Translators: %s is the Disks application */
      g_autofree char *label = g_strdup_printf (_("Open %s"),
                                                shell_app_get_name (self->disks_app));

      shell_modal_dialog_add_button (dialog, label, 0, TRUE,
                                     open_disks_button_cb, self, NULL);
    }
}

static void
set_entry_sensitive (ClutterActor *actor,
                     gboolean      sensitive)
{
  clutter_actor_set_reactive (actor, sensitive);
  g_object_set (actor, "can-focus", sensitive, NULL);
}

static void
keyfiles_checkbox_clicked_cb (StButton                 *button,
                              int                       clicked_button,
                              ShellMountPasswordDialog *self)
{
  gboolean use_keyfiles = st_button_get_checked (ST_BUTTON (self->keyfiles_checkbox));

  set_entry_sensitive (CLUTTER_ACTOR (self->password_entry), !use_keyfiles);
  set_entry_sensitive (CLUTTER_ACTOR (self->pim_entry), !use_keyfiles);
  if (self->remember_choice != NULL)
    set_entry_sensitive (CLUTTER_ACTOR (self->remember_choice), !use_keyfiles);
  set_visible (CLUTTER_ACTOR (self->keyfiles_label), use_keyfiles);

  if (use_keyfiles)
    password_dialog_set_keyfiles_buttons (self);
  else
    password_dialog_set_default_buttons (self);
}

static StEntry *
new_password_entry (ShellMountPasswordDialog *self,
                    const char               *hint_text)
{
  ClutterActor *entry;

  entry = g_object_new (ST_TYPE_PASSWORD_ENTRY,
                        "style-class", "prompt-dialog-password-entry",
                        "hint-text", hint_text,
                        "can-focus", TRUE,
                        "x-expand", TRUE,
                        NULL);
  g_signal_connect_object (st_entry_get_clutter_text (ST_ENTRY (entry)), "activate",
                           G_CALLBACK (entry_activate_cb), self, 0);
  shell_entry_add_context_menu (ST_ENTRY (entry), SHELL_ACTION_MODE_POPUP);

  return ST_ENTRY (entry);
}

/* `layout.hookup_style (container)` of environment.js for a grid layout */
static void
grid_style_changed_cb (StWidget          *container,
                       ClutterGridLayout *layout)
{
  StThemeNode *node = st_widget_get_theme_node (container);
  double length;

  if (st_theme_node_lookup_length (node, "spacing-rows", FALSE, &length))
    clutter_grid_layout_set_row_spacing (layout, (guint) length);

  if (st_theme_node_lookup_length (node, "spacing-columns", FALSE, &length))
    clutter_grid_layout_set_column_spacing (layout, (guint) length);
}

static void
shell_mount_password_dialog_constructed (GObject *object)
{
  ShellMountPasswordDialog *self = SHELL_MOUNT_PASSWORD_DIALOG (object);
  ShellModalDialog *dialog = SHELL_MODAL_DIALOG (self);
  g_auto (GStrv) strings = NULL;
  const char *title = NULL;
  const char *description = NULL;
  ShellMessageDialogContent *content;
  ClutterLayoutManager *password_grid_layout;
  ClutterGridLayout *grid_layout;
  ClutterActor *password_grid;
  ClutterActor *warning_box;
  ClutterActor *caps_lock_warning;
  ClutterActor *error_label;
  ClutterActor *spinner;
  StEntry *password_entry;
  gboolean rtl;
  int cur_grid_row = 0;

  G_OBJECT_CLASS (shell_mount_password_dialog_parent_class)->constructed (object);

  strings = g_strsplit (self->message != NULL ? self->message : "", "\n", 0);
  if (strings[0] != NULL && strings[0][0] != '\0')
    {
      title = strings[0];
      if (strings[1] != NULL && strings[1][0] != '\0')
        description = strings[1];
    }
  else if (strings[0] != NULL && strings[1] != NULL && strings[1][0] != '\0')
    {
      /* An empty first line is no title, the second line is still the
       * description */
      description = strings[1];
    }

  self->disks_app = shell_app_system_lookup_app (shell_global_get_app_system (shell_global_get ()),
                                                 "org.gnome.DiskUtility.desktop");
  if (self->disks_app != NULL)
    g_object_ref (self->disks_app);

  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT,
                          "title", title,
                          "description", description,
                          NULL);

  password_grid_layout = clutter_grid_layout_new ();
  grid_layout = CLUTTER_GRID_LAYOUT (password_grid_layout);
  clutter_grid_layout_set_orientation (grid_layout, CLUTTER_ORIENTATION_VERTICAL);
  password_grid = g_object_new (ST_TYPE_WIDGET,
                                "style-class", "prompt-dialog-password-grid",
                                "layout-manager", password_grid_layout,
                                NULL);
  g_signal_connect_object (password_grid, "style-changed",
                           G_CALLBACK (grid_style_changed_cb), grid_layout, 0);

  rtl = clutter_actor_get_text_direction (password_grid) == CLUTTER_TEXT_DIRECTION_RTL;

  if (self->flags & G_ASK_PASSWORD_TCRYPT)
    {
      ShellCheckBox *check;
      ClutterActor *keyfiles_label;
      ClutterText *keyfiles_text;
      g_autofree char *markup = NULL;
      StEntry *pim_entry;

      check = shell_check_box_new (_("Hidden Volume"));
      g_set_weak_pointer (&self->hidden_volume, check);
      clutter_actor_add_child (CLUTTER_ACTOR (content), CLUTTER_ACTOR (check));

      check = shell_check_box_new (_("Windows System Volume"));
      g_set_weak_pointer (&self->system_volume, check);
      clutter_actor_add_child (CLUTTER_ACTOR (content), CLUTTER_ACTOR (check));

      check = shell_check_box_new (_("Uses Keyfiles"));
      g_set_weak_pointer (&self->keyfiles_checkbox, check);
      g_signal_connect_object (check, "clicked",
                               G_CALLBACK (keyfiles_checkbox_clicked_cb), self, 0);
      clutter_actor_add_child (CLUTTER_ACTOR (content), CLUTTER_ACTOR (check));

      keyfiles_label = g_object_new (ST_TYPE_LABEL, "visible", FALSE, NULL);
      g_set_weak_pointer (&self->keyfiles_label, ST_LABEL (keyfiles_label));
      keyfiles_text = st_label_get_clutter_text (ST_LABEL (keyfiles_label));
      if (self->disks_app != NULL)
        /* Translators: %s is the Disks application */
        markup = g_strdup_printf (_("To unlock a volume that uses keyfiles, use the <i>%s</i> utility instead"),
                                  shell_app_get_name (self->disks_app));
      else
        markup = g_strdup (_("You need an external utility like <i>Disks</i> to unlock a volume that uses keyfiles"));
      clutter_text_set_markup (keyfiles_text, markup);
      clutter_text_set_ellipsize (keyfiles_text, PANGO_ELLIPSIZE_NONE);
      clutter_text_set_line_wrap (keyfiles_text, TRUE);
      clutter_actor_add_child (CLUTTER_ACTOR (content), keyfiles_label);

      pim_entry = new_password_entry (self, _("PIM Number"));
      g_set_weak_pointer (&self->pim_entry, pim_entry);

      clutter_grid_layout_attach (grid_layout, CLUTTER_ACTOR (pim_entry),
                                  rtl ? 1 : 0, cur_grid_row, 1, 1);
      cur_grid_row += 1;
    }

  password_entry = new_password_entry (self, _("Password"));
  g_set_weak_pointer (&self->password_entry, password_entry);
  shell_modal_dialog_set_initial_key_focus (dialog, CLUTTER_ACTOR (password_entry));

  spinner = CLUTTER_ACTOR (shell_spinner_new (WORK_SPINNER_ICON_SIZE, TRUE, FALSE));
  g_set_weak_pointer (&self->work_spinner, SHELL_SPINNER (spinner));

  if (rtl)
    {
      clutter_grid_layout_attach (grid_layout, spinner, 0, cur_grid_row, 1, 1);
      clutter_grid_layout_attach (grid_layout, CLUTTER_ACTOR (password_entry), 1, cur_grid_row, 1, 1);
    }
  else
    {
      clutter_grid_layout_attach (grid_layout, CLUTTER_ACTOR (password_entry), 0, cur_grid_row, 1, 1);
      clutter_grid_layout_attach (grid_layout, spinner, 1, cur_grid_row, 1, 1);
    }
  cur_grid_row += 1;

  warning_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                              "orientation", CLUTTER_ORIENTATION_VERTICAL,
                              NULL);

  caps_lock_warning = CLUTTER_ACTOR (shell_caps_lock_warning_new ());
  clutter_actor_add_child (warning_box, caps_lock_warning);

  error_label = g_object_new (ST_TYPE_LABEL,
                              "style-class", "prompt-dialog-error-label",
                              "opacity", 0,
                              NULL);
  g_set_weak_pointer (&self->error_message_label, ST_LABEL (error_label));
  clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (error_label)),
                              PANGO_ELLIPSIZE_NONE);
  clutter_text_set_line_wrap (st_label_get_clutter_text (ST_LABEL (error_label)), TRUE);
  clutter_actor_add_child (warning_box, error_label);

  clutter_grid_layout_attach (grid_layout, warning_box, 0, cur_grid_row, 2, 1);

  clutter_actor_add_child (CLUTTER_ACTOR (content), password_grid);

  if (self->flags & G_ASK_PASSWORD_SAVING_SUPPORTED)
    {
      ShellCheckBox *check = shell_check_box_new (_("Remember Password"));

      g_set_weak_pointer (&self->remember_choice, check);
      st_button_set_checked (ST_BUTTON (check),
                             g_settings_get_boolean (shell_global_get_settings (shell_global_get ()),
                                                     REMEMBER_MOUNT_PASSWORD_KEY));
      clutter_actor_add_child (CLUTTER_ACTOR (content), CLUTTER_ACTOR (check));
    }

  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (dialog)),
                           CLUTTER_ACTOR (content));

  password_dialog_set_default_buttons (self);
}

/**
 * shell_mount_password_dialog_reask_password:
 * @self: a #ShellMountPasswordDialog
 *
 * Tells the user that the password was wrong and clears the entry
 * (`reaskPassword ()`)
 */
static void
shell_mount_password_dialog_reask_password (ShellMountPasswordDialog *self)
{
  if (self->work_spinner == NULL || self->password_entry == NULL ||
      self->error_message_label == NULL)
    return;

  shell_spinner_stop (self->work_spinner);
  st_entry_set_text (self->password_entry, "");
  st_label_set_text (self->error_message_label,
                     _("Sorry, that didn’t work. Please try again."));
  clutter_actor_set_opacity (CLUTTER_ACTOR (self->error_message_label), 255);

  shell_animation_wiggle (CLUTTER_ACTOR (self->password_entry));
}

static void
shell_mount_password_dialog_dispose (GObject *object)
{
  ShellMountPasswordDialog *self = SHELL_MOUNT_PASSWORD_DIALOG (object);

  g_clear_weak_pointer (&self->hidden_volume);
  g_clear_weak_pointer (&self->system_volume);
  g_clear_weak_pointer (&self->keyfiles_checkbox);
  g_clear_weak_pointer (&self->keyfiles_label);
  g_clear_weak_pointer (&self->pim_entry);
  g_clear_weak_pointer (&self->password_entry);
  g_clear_weak_pointer (&self->work_spinner);
  g_clear_weak_pointer (&self->error_message_label);
  g_clear_weak_pointer (&self->remember_choice);
  g_clear_object (&self->disks_app);

  G_OBJECT_CLASS (shell_mount_password_dialog_parent_class)->dispose (object);
}

static void
shell_mount_password_dialog_finalize (GObject *object)
{
  ShellMountPasswordDialog *self = SHELL_MOUNT_PASSWORD_DIALOG (object);

  g_free (self->message);

  G_OBJECT_CLASS (shell_mount_password_dialog_parent_class)->finalize (object);
}

static void
shell_mount_password_dialog_set_property (GObject      *object,
                                          guint         prop_id,
                                          const GValue *value,
                                          GParamSpec   *pspec)
{
  ShellMountPasswordDialog *self = SHELL_MOUNT_PASSWORD_DIALOG (object);

  switch (prop_id)
    {
    case PASSWORD_PROP_MESSAGE:
      g_free (self->message);
      self->message = g_value_dup_string (value);
      break;

    case PASSWORD_PROP_FLAGS:
      self->flags = g_value_get_flags (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_mount_password_dialog_get_property (GObject    *object,
                                          guint       prop_id,
                                          GValue     *value,
                                          GParamSpec *pspec)
{
  ShellMountPasswordDialog *self = SHELL_MOUNT_PASSWORD_DIALOG (object);

  switch (prop_id)
    {
    case PASSWORD_PROP_MESSAGE:
      g_value_set_string (value, self->message);
      break;

    case PASSWORD_PROP_FLAGS:
      g_value_set_flags (value, self->flags);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_mount_password_dialog_class_init (ShellMountPasswordDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_mount_password_dialog_constructed;
  object_class->dispose = shell_mount_password_dialog_dispose;
  object_class->finalize = shell_mount_password_dialog_finalize;
  object_class->set_property = shell_mount_password_dialog_set_property;
  object_class->get_property = shell_mount_password_dialog_get_property;

  password_props[PASSWORD_PROP_MESSAGE] =
    g_param_spec_string ("message", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  password_props[PASSWORD_PROP_FLAGS] =
    g_param_spec_flags ("flags", NULL, NULL,
                        G_TYPE_ASK_PASSWORD_FLAGS, 0,
                        G_PARAM_READWRITE |
                        G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PASSWORD_N_PROPS, password_props);

  /* ShellMountPasswordDialog::response (choice: int, password: string,
   * remember: boolean, hidden-volume: boolean, system-volume: boolean,
   * pim: uint): choice is -1 if the user cancelled */
  g_signal_new ("response",
                G_TYPE_FROM_CLASS (klass),
                G_SIGNAL_RUN_LAST,
                0, NULL, NULL, NULL,
                G_TYPE_NONE, 6,
                G_TYPE_INT, G_TYPE_STRING,
                G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN,
                G_TYPE_UINT);
}

static void
shell_mount_password_dialog_init (ShellMountPasswordDialog *self)
{
}

static ShellMountPasswordDialog *
shell_mount_password_dialog_new (const char        *message,
                                 GAskPasswordFlags  flags)
{
  return g_object_new (SHELL_TYPE_MOUNT_PASSWORD_DIALOG,
                       "dialog-style-class", "prompt-dialog",
                       "message", message,
                       "flags", flags,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellMountOperationUI                                                    */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_SOURCE,
  PROP_EXISTING_DIALOG,
  PROP_MOUNT_OP,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellMountOperationUI
{
  GObject parent;

  GVolume *source;
  ShellMountOperation *mount_op;

  ShellModalDialog *dialog;             /* weak */
  ShellModalDialog *existing_dialog;    /* weak */
  ShellProcessesDialog *processes_dialog;   /* weak */
  ShellNotification *notification;      /* weak */
  GDrive *drive;
};

G_DEFINE_FINAL_TYPE (ShellMountOperationUI, shell_mount_operation_ui, G_TYPE_OBJECT)

static void
close_existing_dialog (ShellMountOperationUI *self)
{
  g_autoptr (ShellModalDialog) existing = NULL;

  if (self->existing_dialog == NULL)
    return;

  existing = g_object_ref (self->existing_dialog);
  g_clear_weak_pointer (&self->existing_dialog);
  shell_modal_dialog_close (existing);
}

static void
question_response_cb (ShellMountQuestionDialog *dialog,
                      int                       choice,
                      ShellMountOperationUI    *self)
{
  g_mount_operation_set_choice (G_MOUNT_OPERATION (self->mount_op), choice);
  g_mount_operation_reply (G_MOUNT_OPERATION (self->mount_op), G_MOUNT_OPERATION_HANDLED);

  shell_mount_operation_ui_close (self);
}

static void
on_ask_question (GMountOperation       *op,
                 const char            *message,
                 const char * const    *choices,
                 ShellMountOperationUI *self)
{
  ShellMountQuestionDialog *dialog;

  close_existing_dialog (self);

  dialog = shell_mount_question_dialog_new ();
  g_set_weak_pointer (&self->dialog, SHELL_MODAL_DIALOG (dialog));

  g_signal_connect_object (dialog, "response",
                           G_CALLBACK (question_response_cb), self, 0);

  shell_mount_question_dialog_update (dialog, message, choices);
  shell_modal_dialog_open (SHELL_MODAL_DIALOG (dialog));
}

static void
password_response_cb (ShellMountPasswordDialog *dialog,
                      int                       choice,
                      const char               *password,
                      gboolean                  remember,
                      gboolean                  hidden_volume,
                      gboolean                  system_volume,
                      guint                     pim,
                      ShellMountOperationUI    *self)
{
  GMountOperation *op = G_MOUNT_OPERATION (self->mount_op);

  if (choice == -1)
    {
      g_mount_operation_reply (op, G_MOUNT_OPERATION_ABORTED);
      return;
    }

  g_mount_operation_set_password_save (op, remember ? G_PASSWORD_SAVE_PERMANENTLY
                                                    : G_PASSWORD_SAVE_NEVER);
  g_mount_operation_set_password (op, password);
  g_mount_operation_set_is_tcrypt_hidden_volume (op, hidden_volume);
  g_mount_operation_set_is_tcrypt_system_volume (op, system_volume);
  g_mount_operation_set_pim (op, pim);
  g_mount_operation_reply (op, G_MOUNT_OPERATION_HANDLED);
}

static void
on_ask_password (GMountOperation       *op,
                 const char            *message,
                 const char            *default_user,
                 const char            *default_domain,
                 GAskPasswordFlags      flags,
                 ShellMountOperationUI *self)
{
  ShellModalDialog *dialog;

  if (self->existing_dialog != NULL &&
      !SHELL_IS_MOUNT_PASSWORD_DIALOG (self->existing_dialog))
    {
      /* Only a password dialog can be asked again */
      close_existing_dialog (self);
    }

  if (self->existing_dialog != NULL)
    {
      dialog = self->existing_dialog;
      g_set_weak_pointer (&self->dialog, dialog);
      shell_mount_password_dialog_reask_password (SHELL_MOUNT_PASSWORD_DIALOG (dialog));
    }
  else
    {
      dialog = SHELL_MODAL_DIALOG (shell_mount_password_dialog_new (message, flags));
      g_set_weak_pointer (&self->dialog, dialog);
    }

  g_signal_connect_object (dialog, "response",
                           G_CALLBACK (password_response_cb), self, 0);
  shell_modal_dialog_open (dialog);
}

static void
processes_response_cb (ShellProcessesDialog  *dialog,
                       int                    choice,
                       ShellMountOperationUI *self)
{
  if (choice == -1)
    {
      g_mount_operation_reply (G_MOUNT_OPERATION (self->mount_op), G_MOUNT_OPERATION_ABORTED);
    }
  else
    {
      g_mount_operation_set_choice (G_MOUNT_OPERATION (self->mount_op), choice);
      g_mount_operation_reply (G_MOUNT_OPERATION (self->mount_op), G_MOUNT_OPERATION_HANDLED);
    }

  shell_mount_operation_ui_close (self);
}

static void
on_show_processes (ShellMountOperation    *op,
                   ShellMountOperationUI  *self)
{
  g_autoptr (GArray) processes = NULL;
  g_auto (GStrv) choices = NULL;
  g_autofree char *message = NULL;

  close_existing_dialog (self);

  processes = shell_mount_operation_get_show_processes_pids (op);
  choices = shell_mount_operation_get_show_processes_choices (op);
  message = shell_mount_operation_get_show_processes_message (op);

  if (self->processes_dialog == NULL)
    {
      ShellProcessesDialog *dialog = shell_processes_dialog_new ();

      g_set_weak_pointer (&self->processes_dialog, dialog);
      g_set_weak_pointer (&self->dialog, SHELL_MODAL_DIALOG (dialog));

      g_signal_connect_object (dialog, "response",
                               G_CALLBACK (processes_response_cb), self, 0);
      shell_modal_dialog_open (SHELL_MODAL_DIALOG (dialog));
    }

  shell_processes_dialog_update (self->processes_dialog, message, processes,
                                 (const char * const *) choices);
}

static void
notification_destroyed_cb (ShellNotification     *notification,
                           guint                  reason,
                           ShellMountOperationUI *self)
{
  g_clear_weak_pointer (&self->notification);
}

static void
create_notification (ShellMountOperationUI *self,
                     const char            *title,
                     const char            *body)
{
  ShellMessageTray *message_tray = shell_main_get_message_tray (shell_main_get_default ());
  g_autoptr (ShellNotification) notification = NULL;
  ShellSource *source;

  if (self->notification != NULL)
    shell_notification_destroy (self->notification,
                                SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);

  if (message_tray == NULL)
    return;

  source = shell_message_tray_get_system_source (message_tray);
  notification = shell_notification_new (source, title, body);
  shell_notification_set_is_transient (notification, TRUE);
  shell_notification_set_icon_name (notification, "media-removable-symbolic");

  g_set_weak_pointer (&self->notification, notification);
  g_signal_connect_object (notification, "destroy",
                           G_CALLBACK (notification_destroyed_cb), self, 0);
  shell_source_add_notification (source, notification);
}

static void
show_unmount_notification_done (ShellMountOperationUI *self,
                                const char            *message)
{
  if (message != NULL && message[0] != '\0')
    create_notification (self, message, NULL);
}

static void
show_unmount_notification (ShellMountOperationUI *self,
                           const char            *message)
{
  g_auto (GStrv) lines = g_strsplit (message, "\n", 0);
  const char *title = lines[0];
  const char *body = lines[0] != NULL ? lines[1] : NULL;

  if (self->notification == NULL)
    {
      create_notification (self, title, body);
      return;
    }

  shell_notification_set_title (self->notification, title);
  shell_notification_set_body (self->notification, body);
}

static void
on_show_unmount_progress (GMountOperation       *op,
                          const char            *message,
                          gint64                 time_left,
                          gint64                 bytes_left,
                          ShellMountOperationUI *self)
{
  if (bytes_left == 0)
    show_unmount_notification_done (self, message);
  else
    show_unmount_notification (self, message);
}

static void
on_aborted (GMountOperation       *op,
            ShellMountOperationUI *self)
{
  shell_mount_operation_ui_close (self);
}

static void
on_drive_disconnected (GDrive                *drive,
                       ShellMountOperationUI *self)
{
  shell_mount_operation_ui_close (self);
}

/**
 * shell_mount_operation_ui_close:
 * @self: a #ShellMountOperationUI
 *
 * Closes the dialogs and the notification (`close ()`). Called when the
 * operation is over.
 */
void
shell_mount_operation_ui_close (ShellMountOperationUI *self)
{
  g_autoptr (ShellMountOperationUI) keep_alive = NULL;
  g_autoptr (ShellModalDialog) dialog = NULL;

  g_return_if_fail (SHELL_IS_MOUNT_OPERATION_UI (self));

  keep_alive = g_object_ref (self);

  close_existing_dialog (self);
  g_clear_weak_pointer (&self->processes_dialog);

  if (self->dialog != NULL)
    {
      dialog = g_object_ref (self->dialog);
      g_clear_weak_pointer (&self->dialog);
      shell_modal_dialog_close (dialog);
    }

  if (self->notification != NULL)
    {
      g_autoptr (ShellNotification) notification = g_object_ref (self->notification);

      g_clear_weak_pointer (&self->notification);
      shell_notification_destroy (notification,
                                  SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
    }

  if (self->drive != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->drive, self);
      g_clear_object (&self->drive);
    }
}

/**
 * shell_mount_operation_ui_borrow_dialog:
 * @self: a #ShellMountOperationUI
 *
 * Takes the dialog away from the operation, so that it stops answering to it
 * (`borrowDialog ()`): the next password prompt of the volume is shown in the
 * same dialog, which tells the user that the password was wrong.
 *
 * Returns: (transfer full) (nullable): the dialog that is open, if any
 */
ShellModalDialog *
shell_mount_operation_ui_borrow_dialog (ShellMountOperationUI *self)
{
  g_return_val_if_fail (SHELL_IS_MOUNT_OPERATION_UI (self), NULL);

  if (self->dialog == NULL)
    return NULL;

  g_signal_handlers_disconnect_by_data (self->dialog, self);
  return g_object_ref (self->dialog);
}

/**
 * shell_mount_operation_ui_get_mount_op:
 * @self: a #ShellMountOperationUI
 *
 * Returns: (transfer none): the mount operation to mount the volume with
 */
GMountOperation *
shell_mount_operation_ui_get_mount_op (ShellMountOperationUI *self)
{
  g_return_val_if_fail (SHELL_IS_MOUNT_OPERATION_UI (self), NULL);

  return G_MOUNT_OPERATION (self->mount_op);
}

static void
shell_mount_operation_ui_constructed (GObject *object)
{
  ShellMountOperationUI *self = SHELL_MOUNT_OPERATION_UI (object);
  GMountOperation *op;

  G_OBJECT_CLASS (shell_mount_operation_ui_parent_class)->constructed (object);

  op = shell_mount_operation_new ();
  self->mount_op = SHELL_MOUNT_OPERATION (op);

  g_signal_connect_object (op, "ask-question",
                           G_CALLBACK (on_ask_question), self, 0);
  g_signal_connect_object (op, "ask-password",
                           G_CALLBACK (on_ask_password), self, 0);
  g_signal_connect_object (op, "show-processes-2",
                           G_CALLBACK (on_show_processes), self, 0);
  g_signal_connect_object (op, "aborted",
                           G_CALLBACK (on_aborted), self, 0);
  g_signal_connect_object (op, "show-unmount-progress",
                           G_CALLBACK (on_show_unmount_progress), self, 0);

  if (self->source != NULL)
    {
      self->drive = g_volume_get_drive (self->source);
      if (self->drive != NULL)
        g_signal_connect_object (self->drive, "disconnected",
                                 G_CALLBACK (on_drive_disconnected), self, 0);
    }
}

static void
shell_mount_operation_ui_dispose (GObject *object)
{
  ShellMountOperationUI *self = SHELL_MOUNT_OPERATION_UI (object);

  if (self->drive != NULL)
    g_signal_handlers_disconnect_by_data (self->drive, self);
  g_clear_object (&self->drive);

  g_clear_weak_pointer (&self->dialog);
  g_clear_weak_pointer (&self->existing_dialog);
  g_clear_weak_pointer (&self->processes_dialog);
  g_clear_weak_pointer (&self->notification);

  if (self->mount_op != NULL)
    g_signal_handlers_disconnect_by_data (self->mount_op, self);
  g_clear_object (&self->mount_op);
  g_clear_object (&self->source);

  G_OBJECT_CLASS (shell_mount_operation_ui_parent_class)->dispose (object);
}

static void
shell_mount_operation_ui_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellMountOperationUI *self = SHELL_MOUNT_OPERATION_UI (object);

  switch (prop_id)
    {
    case PROP_SOURCE:
      g_value_set_object (value, self->source);
      break;

    case PROP_EXISTING_DIALOG:
      g_value_set_object (value, self->existing_dialog);
      break;

    case PROP_MOUNT_OP:
      g_value_set_object (value, self->mount_op);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_mount_operation_ui_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellMountOperationUI *self = SHELL_MOUNT_OPERATION_UI (object);

  switch (prop_id)
    {
    case PROP_SOURCE:
      self->source = g_value_dup_object (value);
      break;

    case PROP_EXISTING_DIALOG:
      {
        ShellModalDialog *dialog = g_value_get_object (value);

        if (dialog != NULL)
          g_set_weak_pointer (&self->existing_dialog, dialog);
      }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_mount_operation_ui_class_init (ShellMountOperationUIClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_mount_operation_ui_constructed;
  object_class->dispose = shell_mount_operation_ui_dispose;
  object_class->get_property = shell_mount_operation_ui_get_property;
  object_class->set_property = shell_mount_operation_ui_set_property;

  /**
   * ShellMountOperationUI:source:
   *
   * The volume which is mounted (`source`): the operation closes when its
   * drive is disconnected.
   */
  props[PROP_SOURCE] =
    g_param_spec_object ("source", NULL, NULL,
                         G_TYPE_VOLUME,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMountOperationUI:existing-dialog:
   *
   * A password dialog of a previous operation (`existingDialog`, see
   * shell_mount_operation_ui_borrow_dialog ()) to ask the password in again.
   */
  props[PROP_EXISTING_DIALOG] =
    g_param_spec_object ("existing-dialog", NULL, NULL,
                         SHELL_TYPE_MODAL_DIALOG,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMountOperationUI:mount-op:
   *
   * The mount operation to mount the volume with (`mountOp`)
   */
  props[PROP_MOUNT_OP] =
    g_param_spec_object ("mount-op", NULL, NULL,
                         G_TYPE_MOUNT_OPERATION,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_mount_operation_ui_init (ShellMountOperationUI *self)
{
}

/**
 * shell_mount_operation_ui_new:
 * @source: (nullable): the volume which is going to be mounted
 * @existing_dialog: (nullable): the dialog of an earlier operation of the
 *   volume, to ask for the password again in
 *
 * Creates a mount operation which shows its dialogs in the shell
 * (`new ShellMountOperation (source, {existingDialog})`).
 *
 * Returns: (transfer full): a new #ShellMountOperationUI
 */
ShellMountOperationUI *
shell_mount_operation_ui_new (GVolume          *source,
                              ShellModalDialog *existing_dialog)
{
  return g_object_new (SHELL_TYPE_MOUNT_OPERATION_UI,
                       "source", source,
                       "existing-dialog", existing_dialog,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellMountOpHandler                                                      */
/* ------------------------------------------------------------------------ */

typedef enum
{
  MOUNT_OPERATION_TYPE_NONE,
  MOUNT_OPERATION_TYPE_ASK_PASSWORD,
  MOUNT_OPERATION_TYPE_ASK_QUESTION,
  MOUNT_OPERATION_TYPE_SHOW_PROCESSES,
} MountOperationType;

struct _ShellMountOpHandler
{
  GObject parent;

  ShellMountOperationHandler *skeleton;
  guint name_id;

  ShellModalDialog *dialog;             /* weak */

  char *current_id;
  GDBusMethodInvocation *current_invocation;
  MountOperationType current_type;
};

G_DEFINE_FINAL_TYPE (ShellMountOpHandler, shell_mount_op_handler, G_TYPE_OBJECT)

static void
ensure_empty_request (ShellMountOpHandler *self)
{
  g_clear_pointer (&self->current_id, g_free);
  g_clear_object (&self->current_invocation);
  self->current_type = MOUNT_OPERATION_TYPE_NONE;
}

/* Answers the pending call with @response and @details (a floating or full
 * reference) and forgets about it */
static void
clear_current_request (ShellMountOpHandler *self,
                       GMountOperationResult response,
                       GVariant             *details)
{
  g_autoptr (GVariant) owned_details = g_variant_ref_sink (details);

  if (self->current_invocation != NULL)
    g_dbus_method_invocation_return_value (self->current_invocation,
                                           g_variant_new ("(u@a{sv})",
                                                          (guint32) response,
                                                          owned_details));

  ensure_empty_request (self);
}

static GVariant *
empty_details (void)
{
  return g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0);
}

/* Returns whether the request is the one of the open dialog, which then
 * only has to be updated */
static gboolean
set_current_request (ShellMountOpHandler    *self,
                     GDBusMethodInvocation  *invocation,
                     const char             *id,
                     MountOperationType      type)
{
  g_autofree char *old_id = g_steal_pointer (&self->current_id);
  MountOperationType old_type = self->current_type;
  g_autofree char *request_id = g_strdup_printf ("%s@%s", id,
                                                 g_dbus_method_invocation_get_sender (invocation));

  /* The old id was dropped with the old request */
  self->current_id = NULL;
  clear_current_request (self, G_MOUNT_OPERATION_UNHANDLED, empty_details ());

  self->current_invocation = g_object_ref (invocation);
  self->current_id = g_strdup (request_id);
  self->current_type = type;

  return self->dialog != NULL && g_strcmp0 (old_id, request_id) == 0 && old_type == type;
}

static void
close_dialog (ShellMountOpHandler *self)
{
  g_autoptr (ShellModalDialog) dialog = NULL;

  if (self->dialog == NULL)
    return;

  dialog = g_object_ref (self->dialog);
  g_clear_weak_pointer (&self->dialog);
  shell_modal_dialog_close (dialog);
}

static void
handler_password_response_cb (ShellMountPasswordDialog *dialog,
                              int                       choice,
                              const char               *password,
                              gboolean                  remember,
                              gboolean                  hidden_volume,
                              gboolean                  system_volume,
                              guint                     pim,
                              ShellMountOpHandler      *self)
{
  GVariantBuilder details;
  GMountOperationResult response;

  g_variant_builder_init (&details, G_VARIANT_TYPE ("a{sv}"));

  if (choice == -1)
    {
      response = G_MOUNT_OPERATION_ABORTED;
    }
  else
    {
      GPasswordSave pass_save = remember ? G_PASSWORD_SAVE_PERMANENTLY : G_PASSWORD_SAVE_NEVER;

      response = G_MOUNT_OPERATION_HANDLED;
      g_variant_builder_add (&details, "{sv}", "password_save",
                             g_variant_new_uint32 (pass_save));
      g_variant_builder_add (&details, "{sv}", "password",
                             g_variant_new_string (password));
      g_variant_builder_add (&details, "{sv}", "hidden_volume",
                             g_variant_new_boolean (hidden_volume));
      g_variant_builder_add (&details, "{sv}", "system_volume",
                             g_variant_new_boolean (system_volume));
      g_variant_builder_add (&details, "{sv}", "pim",
                             g_variant_new_uint32 (pim));
    }

  clear_current_request (self, response, g_variant_builder_end (&details));
}

static void
handler_choice_response_cb (GObject             *dialog,
                            int                  choice,
                            ShellMountOpHandler *self)
{
  GVariantBuilder details;
  GMountOperationResult response;

  g_variant_builder_init (&details, G_VARIANT_TYPE ("a{sv}"));

  if (choice == -1)
    {
      response = G_MOUNT_OPERATION_ABORTED;
    }
  else
    {
      response = G_MOUNT_OPERATION_HANDLED;
      g_variant_builder_add (&details, "{sv}", "choice", g_variant_new_int32 (choice));
    }

  clear_current_request (self, response, g_variant_builder_end (&details));
}

/* Opens the new dialog; answers the call with an error if it cannot */
static void
open_handler_dialog (ShellMountOpHandler *self,
                     ShellModalDialog    *dialog)
{
  g_autoptr (GDBusMethodInvocation) invocation = NULL;

  if (shell_modal_dialog_open (dialog))
    return;

  invocation = g_steal_pointer (&self->current_invocation);
  ensure_empty_request (self);
  g_clear_weak_pointer (&self->dialog);
  clutter_actor_destroy (CLUTTER_ACTOR (dialog));

  if (invocation != NULL)
    g_dbus_method_invocation_return_error_literal (invocation,
                                                   G_DBUS_ERROR,
                                                   G_DBUS_ERROR_FAILED,
                                                   "Cannot grab pointer and keyboard");
}

/* The dialog stays visible until clients call the Close () method, or another
 * dialog becomes visible. Calling AskPassword () again for the same id has
 * the effect to clear the existing dialog and update it with a message
 * indicating the previous attempt went wrong. */
static gboolean
on_handle_ask_password (ShellMountOperationHandler *skeleton,
                        GDBusMethodInvocation      *invocation,
                        const char                 *id,
                        const char                 *message,
                        const char                 *icon_name,
                        const char                 *default_user,
                        const char                 *default_domain,
                        guint                       flags,
                        ShellMountOpHandler        *self)
{
  ShellMountPasswordDialog *dialog;

  if (set_current_request (self, invocation, id, MOUNT_OPERATION_TYPE_ASK_PASSWORD))
    {
      shell_mount_password_dialog_reask_password (SHELL_MOUNT_PASSWORD_DIALOG (self->dialog));
      return TRUE;
    }

  close_dialog (self);

  dialog = shell_mount_password_dialog_new (message, flags);
  g_set_weak_pointer (&self->dialog, SHELL_MODAL_DIALOG (dialog));
  g_signal_connect_object (dialog, "response",
                           G_CALLBACK (handler_password_response_cb), self, 0);
  open_handler_dialog (self, SHELL_MODAL_DIALOG (dialog));
  return TRUE;
}

/* Calling AskQuestion () again for the same id has the effect to update the
 * dialog with the new question. */
static gboolean
on_handle_ask_question (ShellMountOperationHandler *skeleton,
                        GDBusMethodInvocation      *invocation,
                        const char                 *id,
                        const char                 *message,
                        const char                 *icon_name,
                        const char * const         *choices,
                        ShellMountOpHandler        *self)
{
  ShellMountQuestionDialog *dialog;

  if (set_current_request (self, invocation, id, MOUNT_OPERATION_TYPE_ASK_QUESTION))
    {
      shell_mount_question_dialog_update (SHELL_MOUNT_QUESTION_DIALOG (self->dialog),
                                          message, choices);
      return TRUE;
    }

  close_dialog (self);

  dialog = shell_mount_question_dialog_new ();
  g_set_weak_pointer (&self->dialog, SHELL_MODAL_DIALOG (dialog));
  g_signal_connect_object (dialog, "response",
                           G_CALLBACK (handler_choice_response_cb), self, 0);

  shell_mount_question_dialog_update (dialog, message, choices);
  open_handler_dialog (self, SHELL_MODAL_DIALOG (dialog));
  return TRUE;
}

/* Calling ShowProcesses () again for the same id has the effect to clear the
 * existing dialog and update it with the new message and the new list of
 * processes. */
static gboolean
on_handle_show_processes (ShellMountOperationHandler *skeleton,
                          GDBusMethodInvocation      *invocation,
                          const char                 *id,
                          const char                 *message,
                          const char                 *icon_name,
                          GVariant                   *application_pids,
                          const char * const         *choices,
                          ShellMountOpHandler        *self)
{
  g_autoptr (GArray) pids = g_array_new (FALSE, FALSE, sizeof (GPid));
  ShellProcessesDialog *dialog;
  GVariantIter iter;
  gint32 pid;

  g_variant_iter_init (&iter, application_pids);
  while (g_variant_iter_next (&iter, "i", &pid))
    {
      GPid gpid = pid;

      g_array_append_val (pids, gpid);
    }

  if (set_current_request (self, invocation, id, MOUNT_OPERATION_TYPE_SHOW_PROCESSES))
    {
      shell_processes_dialog_update (SHELL_PROCESSES_DIALOG (self->dialog),
                                     message, pids, choices);
      return TRUE;
    }

  close_dialog (self);

  dialog = shell_processes_dialog_new ();
  g_set_weak_pointer (&self->dialog, SHELL_MODAL_DIALOG (dialog));
  g_signal_connect_object (dialog, "response",
                           G_CALLBACK (handler_choice_response_cb), self, 0);

  shell_processes_dialog_update (dialog, message, pids, choices);
  open_handler_dialog (self, SHELL_MODAL_DIALOG (dialog));
  return TRUE;
}

/* Closes a dialog previously opened by AskPassword (), AskQuestion () or
 * ShowProcesses (). If no dialog is open, does nothing. */
static gboolean
on_handle_close (ShellMountOperationHandler *skeleton,
                 GDBusMethodInvocation      *invocation,
                 ShellMountOpHandler        *self)
{
  clear_current_request (self, G_MOUNT_OPERATION_UNHANDLED, empty_details ());
  close_dialog (self);

  shell_mount_operation_handler_complete_close (skeleton, invocation);
  return TRUE;
}

static void
shell_mount_op_handler_constructed (GObject *object)
{
  ShellMountOpHandler *self = SHELL_MOUNT_OP_HANDLER (object);
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GError) error = NULL;

  G_OBJECT_CLASS (shell_mount_op_handler_parent_class)->constructed (object);

  self->skeleton = shell_mount_operation_handler_skeleton_new ();
  g_signal_connect (self->skeleton, "handle-ask-password",
                    G_CALLBACK (on_handle_ask_password), self);
  g_signal_connect (self->skeleton, "handle-ask-question",
                    G_CALLBACK (on_handle_ask_question), self);
  g_signal_connect (self->skeleton, "handle-show-processes",
                    G_CALLBACK (on_handle_show_processes), self);
  g_signal_connect (self->skeleton, "handle-close",
                    G_CALLBACK (on_handle_close), self);

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (connection == NULL ||
      !g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                         connection, MOUNT_OP_HANDLER_OBJECT_PATH,
                                         &error))
    {
      g_warning ("%s: %s", G_STRFUNC, error->message);
      return;
    }

  self->name_id = g_bus_own_name_on_connection (connection,
                                                MOUNT_OP_HANDLER_BUS_NAME,
                                                G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                                NULL, NULL, NULL, NULL);
}

static void
shell_mount_op_handler_dispose (GObject *object)
{
  ShellMountOpHandler *self = SHELL_MOUNT_OP_HANDLER (object);

  g_clear_handle_id (&self->name_id, g_bus_unown_name);

  if (self->skeleton != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->skeleton, self);
      if (g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (self->skeleton)) != NULL)
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->skeleton));
    }

  /* Do not leave the caller waiting */
  clear_current_request (self, G_MOUNT_OPERATION_UNHANDLED, empty_details ());
  g_clear_weak_pointer (&self->dialog);
  g_clear_object (&self->skeleton);

  G_OBJECT_CLASS (shell_mount_op_handler_parent_class)->dispose (object);
}

static void
shell_mount_op_handler_class_init (ShellMountOpHandlerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_mount_op_handler_constructed;
  object_class->dispose = shell_mount_op_handler_dispose;
}

static void
shell_mount_op_handler_init (ShellMountOpHandler *self)
{
  ensure_empty_request (self);
}

/**
 * shell_mount_op_handler_new:
 *
 * Exports org.Gtk.MountOperationHandler on the session bus and owns its name
 * (`new GnomeShellMountOpHandler ()`).
 *
 * Returns: (transfer full): a new #ShellMountOpHandler
 */
ShellMountOpHandler *
shell_mount_op_handler_new (void)
{
  return g_object_new (SHELL_TYPE_MOUNT_OP_HANDLER, NULL);
}
