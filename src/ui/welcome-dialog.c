/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * welcome-dialog.c: The dialog which welcomes the user after the first login
 * and offers GNOME Tour. Port of js/ui/welcomeDialog.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `Main.overview.hide ()` is the ::overview-hide-requested signal.
 *  - A tour that cannot be launched is a warning (the JS threw).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>

#include "shell-app.h"
#include "shell-app-system.h"
#include "st.h"
#include "ui/dialog.h"
#include "ui/welcome-dialog.h"

#define TOUR_DESKTOP_ID "org.gnome.Tour.desktop"

/**
 * ShellWelcomeDialog:
 *
 * The dialog which welcomes the user (`WelcomeDialog`).
 */
struct _ShellWelcomeDialog
{
  ShellModalDialog parent;

  ShellApp *tour_app_info;
};

G_DEFINE_FINAL_TYPE (ShellWelcomeDialog, shell_welcome_dialog, SHELL_TYPE_MODAL_DIALOG)

enum
{
  OVERVIEW_HIDE_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/* DialogResponse.SKIP and DialogResponse.TAKE_TOUR */
static void
send_response (ShellWelcomeDialog *self,
               gboolean            take_tour)
{
  if (take_tour)
    {
      g_autoptr (GError) error = NULL;

      if (!shell_app_launch (self->tour_app_info, 0, -1,
                             SHELL_APP_LAUNCH_GPU_APP_PREF, &error))
        g_warning ("%s: Failed to launch %s: %s", G_STRFUNC,
                   TOUR_DESKTOP_ID, error->message);

      g_signal_emit (self, signals[OVERVIEW_HIDE_REQUESTED], 0);
    }

  shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
}

static void
skip_button_cb (gpointer user_data)
{
  send_response (SHELL_WELCOME_DIALOG (user_data), FALSE);
}

static void
take_tour_button_cb (gpointer user_data)
{
  send_response (SHELL_WELCOME_DIALOG (user_data), TRUE);
}

/* _getOSName () */
static char *
get_os_name (void)
{
  g_autofree char *pretty_name = g_get_os_info (G_OS_INFO_KEY_PRETTY_NAME);
  g_autofree char *name = NULL;
  g_autofree char *version = NULL;
  const char *dot;
  g_autofree char *major_version = NULL;

  if (pretty_name != NULL && pretty_name[0] != '\0')
    return g_steal_pointer (&pretty_name);

  name = g_get_os_info (G_OS_INFO_KEY_NAME);
  version = g_get_os_info (G_OS_INFO_KEY_VERSION);
  if (name != NULL && name[0] != '\0')
    {
      if (version != NULL && version[0] != '\0')
        return g_strdup_printf ("%s %s", name, version);

      return g_steal_pointer (&name);
    }

  dot = strchr (PACKAGE_VERSION, '.');
  major_version = dot != NULL ? g_strndup (PACKAGE_VERSION, dot - PACKAGE_VERSION)
                              : g_strdup (PACKAGE_VERSION);
  return g_strdup_printf (_("GNOME %s"), major_version);
}

/* _buildLayout () */
static void
build_layout (ShellWelcomeDialog *self)
{
  ShellModalDialog *modal_dialog = SHELL_MODAL_DIALOG (self);
  g_autofree char *os_name = get_os_name ();
  g_autofree char *title = g_strdup_printf (_("Welcome to %s"), os_name);
  const char *description = _("If you want to learn your way around, check out the tour");
  ClutterActor *content;
  ClutterActor *icon;

  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT,
                          "title", title,
                          "description", description,
                          NULL);

  icon = g_object_new (ST_TYPE_WIDGET,
                       "style-class", "welcome-dialog-image",
                       NULL);
  clutter_actor_insert_child_at_index (content, icon, 0);

  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (modal_dialog)),
                           content);

  shell_modal_dialog_add_button (modal_dialog, _("Skip"), CLUTTER_KEY_Escape, FALSE,
                                 skip_button_cb, self, NULL);
  shell_modal_dialog_add_button (modal_dialog, _("Take Tour"), 0, FALSE,
                                 take_tour_button_cb, self, NULL);
}

static gboolean
shell_welcome_dialog_open (ShellModalDialog *dialog)
{
  ShellWelcomeDialog *self = SHELL_WELCOME_DIALOG (dialog);

  if (self->tour_app_info == NULL)
    return FALSE;

  return SHELL_MODAL_DIALOG_CLASS (shell_welcome_dialog_parent_class)->open (dialog);
}

static void
shell_welcome_dialog_constructed (GObject *object)
{
  ShellWelcomeDialog *self = SHELL_WELCOME_DIALOG (object);
  ShellAppSystem *app_system = shell_app_system_get_default ();

  G_OBJECT_CLASS (shell_welcome_dialog_parent_class)->constructed (object);

  self->tour_app_info = shell_app_system_lookup_app (app_system, TOUR_DESKTOP_ID);
  if (self->tour_app_info != NULL)
    g_object_ref (self->tour_app_info);

  build_layout (self);
}

static void
shell_welcome_dialog_dispose (GObject *object)
{
  ShellWelcomeDialog *self = SHELL_WELCOME_DIALOG (object);

  g_clear_object (&self->tour_app_info);

  G_OBJECT_CLASS (shell_welcome_dialog_parent_class)->dispose (object);
}

static void
shell_welcome_dialog_class_init (ShellWelcomeDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellModalDialogClass *dialog_class = SHELL_MODAL_DIALOG_CLASS (klass);

  object_class->constructed = shell_welcome_dialog_constructed;
  object_class->dispose = shell_welcome_dialog_dispose;

  dialog_class->open = shell_welcome_dialog_open;

  /**
   * ShellWelcomeDialog::overview-hide-requested:
   * @self: the #ShellWelcomeDialog
   *
   * Emitted when the user takes the tour, which hides the overview
   * (`Main.overview.hide ()`). main.js connects it. The handler must not
   * return a value.
   */
  signals[OVERVIEW_HIDE_REQUESTED] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_welcome_dialog_init (ShellWelcomeDialog *self)
{
}

/**
 * shell_welcome_dialog_new:
 *
 * Creates the welcome dialog (`new WelcomeDialog ()`). Call
 * shell_modal_dialog_open () to show it.
 *
 * Returns: (transfer floating): a new #ShellWelcomeDialog
 */
ShellWelcomeDialog *
shell_welcome_dialog_new (void)
{
  return g_object_new (SHELL_TYPE_WELCOME_DIALOG,
                       "dialog-style-class", "welcome-dialog",
                       NULL);
}
