/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * end-session-dialog.c: The dialog which gnome-session shows to confirm a
 * logout, power off or restart. Port of js/ui/endSessionDialog.js.
 *
 * Copyright 2010-2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * The interface is exported with a gdbus-codegen skeleton
 * (#ShellSessionManagerEndSessionDialog); gnome-software is reached with a
 * gdbus-codegen proxy (#ShellSoftwareOfflineUpdates), the display device of
 * UPower with a plain #GDBusProxy, and the inhibitors of gnome-session with
 * the proxy of misc/gnome-session.h.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - A call to Open () while an earlier one still waits for gnome-software
 *    supersedes it: the earlier call is answered without doing anything (the
 *    JS ran both to the end, in an unspecified order).
 *  - Open () restarts the countdown timer instead of leaking the one of an
 *    earlier call that was still running.
 *  - The list of the other sessions of a superseded Open () is dropped.
 *  - The Alt key handler does nothing once the buttons were released by
 *    cancel () (the JS threw a TypeError).
 *  - The "Boot Options" button connects to the dialog being closed before
 *    closing it: the JS did it the other way round, which loses the
 *    confirmation if the dialog closes without fading.
 *  - Failures to reach UPower, gnome-software, logind and the inhibitors of
 *    gnome-session are not warnings (they use g_message () or g_debug ()),
 *    so that a session without them is quiet. Calls that are still running
 *    when ShellGlobal::shutdown is emitted are cancelled, the Open () call
 *    that waits for them fails.
 *  - The JS `log ()` of a dead inhibitor is a g_message ().
 *  - `_closeSilently ()` of the JS `ModalDialog` is private here too.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <act/act.h>
#include <clutter/clutter-pango.h>
#include <glib/gi18n-lib.h>
#include <math.h>
#include <string.h>

#include "misc/gnome-session.h"
#include "misc/login-manager.h"
#include "shell-app.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "ui/check-box.h"
#include "ui/dialog.h"
#include "ui/end-session-dialog.h"
#include "ui/main.h"
#include "ui/user-widget.h"

#include "end-session-dialog-dbus.h"
#include "software-offline-updates-dbus.h"

#define ITEM_ICON_SIZE 64

#define LOW_BATTERY_THRESHOLD 30

#define MAX_USERS_IN_SESSION_DIALOG 5

#define OFFLINE_UPDATE_ACTION_REBOOT "reboot"
#define OFFLINE_UPDATE_ACTION_SHUTDOWN "shutdown"

#define END_SESSION_DIALOG_OBJECT_PATH "/org/gnome/SessionManager/EndSessionDialog"

#define SOFTWARE_BUS_NAME "org.gnome.Software"
#define SOFTWARE_OFFLINE_UPDATES_PATH "/org/gnome/Software/OfflineUpdates"

#define UPOWER_BUS_NAME "org.freedesktop.UPower"
#define UPOWER_DISPLAY_DEVICE_PATH "/org/freedesktop/UPower/devices/DisplayDevice"
#define UPOWER_DEVICE_INTERFACE "org.freedesktop.UPower.Device"

/* UP_DEVICE_STATE_* of libupower-glib */
#define UP_DEVICE_STATE_CHARGING 1
#define UP_DEVICE_STATE_FULLY_CHARGED 4

#define MODAL_DIALOG_ERROR_UNKNOWN_TYPE "org.gnome.Shell.ModalDialog.Error.UnknownType"
#define MODAL_DIALOG_ERROR_GRAB_FAILED "org.gnome.Shell.ModalDialog.Error.GrabFailed"

typedef enum
{
  DIALOG_TYPE_LOGOUT = 0, /* GSM_SHELL_END_SESSION_DIALOG_TYPE_LOGOUT */
  DIALOG_TYPE_SHUTDOWN = 1, /* GSM_SHELL_END_SESSION_DIALOG_TYPE_SHUTDOWN */
  DIALOG_TYPE_RESTART = 2, /* GSM_SHELL_END_SESSION_DIALOG_TYPE_RESTART */
  DIALOG_TYPE_UPDATE_RESTART = 3,
} DialogType;

#define N_DIALOG_TYPES 4

typedef enum
{
  CONFIRM_LOGOUT,
  CONFIRM_REBOOT,
  CONFIRM_SHUTDOWN,
} ConfirmSignal;

/* ------------------------------------------------------------------------ */
/* The content of the dialog types                                          */
/* ------------------------------------------------------------------------ */

static gboolean
dialog_type_is_valid (guint type)
{
  return type < N_DIALOG_TYPES;
}

static const char *
get_subject (DialogType type)
{
  switch (type)
    {
    case DIALOG_TYPE_LOGOUT:
      return C_("title", "Log Out");
    case DIALOG_TYPE_SHUTDOWN:
      return C_("title", "Power Off");
    case DIALOG_TYPE_RESTART:
      return C_("title", "Restart");
    case DIALOG_TYPE_UPDATE_RESTART:
      return C_("title", "Restart & Install Updates");
    default:
      g_assert_not_reached ();
    }
}

/* The title used when the updates are installed, or %NULL */
static const char *
get_subject_with_updates (DialogType type)
{
  switch (type)
    {
    case DIALOG_TYPE_SHUTDOWN:
      return C_("title", "Install Updates & Power Off");
    case DIALOG_TYPE_RESTART:
      return C_("title", "Install Updates & Restart");
    default:
      return NULL;
    }
}

/* The title for the real name of the user, or %NULL */
static char *
get_subject_with_user (DialogType type,
                       const char *real_name)
{
  if (type != DIALOG_TYPE_LOGOUT)
    return NULL;

  return g_strdup_printf (C_("title", "Log Out %s"), real_name);
}

static char *
get_description (DialogType  type,
                 const char *real_name,
                 int         seconds)
{
  switch (type)
    {
    case DIALOG_TYPE_LOGOUT:
      if (real_name != NULL)
        return g_strdup_printf (ngettext ("%s will be logged out automatically in %d second",
                                          "%s will be logged out automatically in %d seconds",
                                          seconds),
                                real_name, seconds);

      return g_strdup_printf (ngettext ("You will be logged out automatically in %d second",
                                        "You will be logged out automatically in %d seconds",
                                        seconds),
                              seconds);

    case DIALOG_TYPE_SHUTDOWN:
      return g_strdup_printf (ngettext ("The system will power off automatically in %d second",
                                        "The system will power off automatically in %d seconds",
                                        seconds),
                              seconds);

    case DIALOG_TYPE_RESTART:
      return g_strdup_printf (ngettext ("The system will restart automatically in %d second",
                                        "The system will restart automatically in %d seconds",
                                        seconds),
                              seconds);

    case DIALOG_TYPE_UPDATE_RESTART:
      return g_strdup_printf (ngettext ("The system will automatically restart and install updates in %d second",
                                        "The system will automatically restart and install updates in %d seconds",
                                        seconds),
                              seconds);

    default:
      g_assert_not_reached ();
    }
}

static const char *
get_check_box_text (DialogType type)
{
  switch (type)
    {
    case DIALOG_TYPE_SHUTDOWN:
    case DIALOG_TYPE_RESTART:
      return C_("checkbox", "Install pending software updates");
    default:
      return NULL;
    }
}

static gboolean
get_show_battery_warning (DialogType type)
{
  return type != DIALOG_TYPE_LOGOUT;
}

static gboolean
get_show_other_sessions (DialogType type)
{
  return type == DIALOG_TYPE_SHUTDOWN || type == DIALOG_TYPE_RESTART;
}

static ConfirmSignal
get_confirm_signal (DialogType type)
{
  switch (type)
    {
    case DIALOG_TYPE_LOGOUT:
      return CONFIRM_LOGOUT;
    case DIALOG_TYPE_SHUTDOWN:
      return CONFIRM_SHUTDOWN;
    case DIALOG_TYPE_RESTART:
    case DIALOG_TYPE_UPDATE_RESTART:
      return CONFIRM_REBOOT;
    default:
      g_assert_not_reached ();
    }
}

static const char *
get_confirm_label (DialogType type)
{
  switch (type)
    {
    case DIALOG_TYPE_LOGOUT:
      return C_("button", "Log Out");
    case DIALOG_TYPE_SHUTDOWN:
      return C_("button", "Power Off");
    case DIALOG_TYPE_RESTART:
      return C_("button", "Restart");
    case DIALOG_TYPE_UPDATE_RESTART:
      return C_("button", "Restart & Install");
    default:
      g_assert_not_reached ();
    }
}

/* Strings of future features, kept for the translators */
G_GNUC_UNUSED static void
mark_unused_strings (void)
{
  (void) C_("button", "Install & Power Off");
  (void) C_("checkbox", "Power off after updates are installed");
}

/* The logout timer only shows updates every 10 seconds until the last 10
 * seconds, then it shows updates every second. This function takes a given
 * time and returns what we should show to the user for that time. */
static int
round_seconds_to_interval (double total_seconds,
                           double seconds_left,
                           int    interval)
{
  int time;

  time = (int) ceil (seconds_left);

  /* Final count down is in decrements of 1 */
  if (time <= interval)
    return time;

  /* Round up higher than last displayable time interval */
  time += interval - 1;

  /* Then round down to that time interval */
  if (time > total_seconds)
    time = (int) ceil (total_seconds);
  else
    time -= time % interval;

  return time;
}

/* ------------------------------------------------------------------------ */
/* ShellEndSessionDialog                                                    */
/* ------------------------------------------------------------------------ */

struct _ShellEndSessionDialog
{
  ShellModalDialog parent;

  ShellSessionManagerEndSessionDialog *skeleton;
  GCancellable *cancellable;          /* cancelled on dispose and shutdown */
  gboolean disposed;

  ShellLoginManager *login_manager;   /* the default one */
  gboolean can_reboot_to_boot_loader_menu;

  ActUserManager *user_manager;       /* the default one */
  ActUser *user;

  /* Opened when the dialog opens, so that gnome-software does not start too
   * early after the login */
  ShellSoftwareOfflineUpdates *software_proxy;
  GDBusProxy *power_proxy;

  double seconds_left;
  guint total_seconds_to_stay_open;
  gboolean type_valid;
  DialogType type;
  gboolean update_prepared;
  gboolean update_scheduled;

  GArray *applications;               /* of guint, the inhibitor requests */
  guint next_request_id;
  guint n_sessions;
  guint open_serial;

  StButton *reboot_button;            /* weak */
  StButton *reboot_button_alt;        /* weak */
  ClutterKeyController *alt_capture;

  ShellMessageDialogContent *message_dialog_content;  /* weak */
  ShellCheckBox *check_box;           /* weak */
  StLabel *battery_warning;           /* weak */
  ShellListSection *application_section;  /* weak */
  ShellListSection *session_section;      /* weak */

  guint timer_id;
  gboolean skip_closed_signal;
};

G_DEFINE_FINAL_TYPE (ShellEndSessionDialog, shell_end_session_dialog, SHELL_TYPE_MODAL_DIALOG)

static void sync_dialog (ShellEndSessionDialog *self);

static void
set_visible (ClutterActor *actor,
             gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

/* ------------------------------------------------------------------------ */
/* The battery                                                              */
/* ------------------------------------------------------------------------ */

static gboolean
get_power_bool (ShellEndSessionDialog *self,
                const char            *name)
{
  g_autoptr (GVariant) value = NULL;

  if (self->power_proxy == NULL)
    return FALSE;

  value = g_dbus_proxy_get_cached_property (self->power_proxy, name);
  return value != NULL && g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN) &&
         g_variant_get_boolean (value);
}

static gboolean
is_discharging_battery (ShellEndSessionDialog *self)
{
  g_autoptr (GVariant) state = NULL;
  guint32 state_value;

  if (self->power_proxy == NULL || !get_power_bool (self, "IsPresent"))
    return FALSE;

  state = g_dbus_proxy_get_cached_property (self->power_proxy, "State");
  if (state == NULL || !g_variant_is_of_type (state, G_VARIANT_TYPE_UINT32))
    return FALSE;

  state_value = g_variant_get_uint32 (state);
  return state_value != UP_DEVICE_STATE_CHARGING &&
         state_value != UP_DEVICE_STATE_FULLY_CHARGED;
}

static gboolean
is_battery_low (ShellEndSessionDialog *self)
{
  g_autoptr (GVariant) percentage = NULL;

  if (!is_discharging_battery (self))
    return FALSE;

  percentage = g_dbus_proxy_get_cached_property (self->power_proxy, "Percentage");
  return percentage != NULL && g_variant_is_of_type (percentage, G_VARIANT_TYPE_DOUBLE) &&
         g_variant_get_double (percentage) < LOW_BATTERY_THRESHOLD;
}

static gboolean
should_show_low_battery_warning (ShellEndSessionDialog *self)
{
  if (!get_show_battery_warning (self->type))
    return FALSE;

  if (!is_battery_low (self))
    return FALSE;

  if (st_button_get_checked (ST_BUTTON (self->check_box)))
    return TRUE;

  /* Show the warning if updates have already been triggered */
  return self->update_scheduled;
}

static void
power_properties_changed_cb (GDBusProxy            *proxy,
                             GVariant              *changed,
                             const char * const    *invalidated,
                             ShellEndSessionDialog *self)
{
  sync_dialog (self);
}

static void
power_proxy_ready (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  g_autoptr (ShellEndSessionDialog) self = user_data;
  g_autoptr (GError) error = NULL;
  GDBusProxy *proxy;

  proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("%s: %s", G_STRFUNC, error->message);
      return;
    }

  if (self->disposed)
    {
      g_object_unref (proxy);
      return;
    }

  self->power_proxy = proxy;
  g_signal_connect_object (proxy, "g-properties-changed",
                           G_CALLBACK (power_properties_changed_cb), self, 0);
  sync_dialog (self);
}

/* ------------------------------------------------------------------------ */
/* The dialog                                                               */
/* ------------------------------------------------------------------------ */

static void
sync_dialog (ShellEndSessionDialog *self)
{
  ShellModalDialogState state;
  const char *subject;
  const char *subject_with_updates;
  const char *real_name = NULL;
  g_autofree char *user_subject = NULL;
  g_autofree char *description = NULL;
  int display_time;

  if (self->disposed)
    return;

  state = shell_modal_dialog_get_state (SHELL_MODAL_DIALOG (self));
  if (state != SHELL_MODAL_DIALOG_STATE_OPENING &&
      state != SHELL_MODAL_DIALOG_STATE_OPENED)
    return;

  if (!self->type_valid)
    return;

  subject = get_subject (self->type);

  /* Use a different title when we are installing updates */
  subject_with_updates = get_subject_with_updates (self->type);
  if (subject_with_updates != NULL &&
      st_button_get_checked (ST_BUTTON (self->check_box)))
    subject = subject_with_updates;

  set_visible (CLUTTER_ACTOR (self->battery_warning),
               should_show_low_battery_warning (self));

  display_time = round_seconds_to_interval (self->total_seconds_to_stay_open,
                                            self->seconds_left, 10);

  if (act_user_is_loaded (self->user))
    {
      real_name = act_user_get_real_name (self->user);

      if (real_name != NULL)
        {
          user_subject = get_subject_with_user (self->type, real_name);
          if (user_subject != NULL)
            subject = user_subject;
        }
    }

  /* Falls back to the regular description */
  description = get_description (self->type, real_name, display_time);

  g_object_set (self->message_dialog_content,
                "title", subject,
                "description", description,
                NULL);

  set_visible (CLUTTER_ACTOR (self->application_section),
               self->applications->len > 0);
  set_visible (CLUTTER_ACTOR (self->session_section),
               self->n_sessions > 0);
}

static void
user_changed_cb (ActUser               *user,
                 ShellEndSessionDialog *self)
{
  sync_dialog (self);
}

static void
user_loaded_cb (ActUser               *user,
                GParamSpec            *pspec,
                ShellEndSessionDialog *self)
{
  sync_dialog (self);
}

static void
check_box_clicked_cb (StButton              *button,
                      int                    clicked_button,
                      ShellEndSessionDialog *self)
{
  sync_dialog (self);
}

static void
opened_cb (ShellEndSessionDialog *self,
           gpointer               user_data)
{
  sync_dialog (self);
}

static gboolean
alt_key_captured (ShellEndSessionDialog *self,
                  gboolean               pressed)
{
  uint32_t key = 0;

  clutter_key_controller_get_key (self->alt_capture, &key, NULL, NULL);
  if (key != CLUTTER_KEY_Alt_L && key != CLUTTER_KEY_Alt_R)
    return CLUTTER_EVENT_PROPAGATE;

  if (self->reboot_button != NULL && self->reboot_button_alt != NULL)
    {
      set_visible (CLUTTER_ACTOR (self->reboot_button), !pressed);
      set_visible (CLUTTER_ACTOR (self->reboot_button_alt), pressed);
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static gboolean
alt_key_press_cb (ClutterKeyController  *controller,
                  ShellEndSessionDialog *self)
{
  return alt_key_captured (self, TRUE);
}

static gboolean
alt_key_release_cb (ClutterKeyController  *controller,
                    ShellEndSessionDialog *self)
{
  return alt_key_captured (self, FALSE);
}

static void
stop_alt_capture (ShellEndSessionDialog *self)
{
  g_clear_weak_pointer (&self->reboot_button);
  g_clear_weak_pointer (&self->reboot_button_alt);
}

static void
stop_timer (ShellEndSessionDialog *self)
{
  g_clear_handle_id (&self->timer_id, g_source_remove);
  self->seconds_left = 0;
}

static void
emit_confirm_signal (ShellEndSessionDialog *self,
                     ConfirmSignal          signal)
{
  switch (signal)
    {
    case CONFIRM_LOGOUT:
      shell_session_manager_end_session_dialog_emit_confirmed_logout (self->skeleton);
      break;

    case CONFIRM_REBOOT:
      shell_session_manager_end_session_dialog_emit_confirmed_reboot (self->skeleton);
      break;

    case CONFIRM_SHUTDOWN:
      shell_session_manager_end_session_dialog_emit_confirmed_shutdown (self->skeleton);
      break;

    default:
      g_assert_not_reached ();
    }
}

/* ------------------------------------------------------------------------ */
/* Confirming                                                               */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellEndSessionDialog *self;
  ConfirmSignal signal;
  gboolean reboot_if_action_changed;   /* a shutdown that installs updates */
} ConfirmCall;

static void
confirm_call_free (ConfirmCall *call)
{
  g_clear_object (&call->self);
  g_free (call);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ConfirmCall, confirm_call_free)

static void
confirm_finish (ConfirmCall *call)
{
  g_autoptr (ConfirmCall) owned_call = call;
  ShellEndSessionDialog *self = call->self;

  if (self->disposed)
    return;

  shell_modal_dialog_fade_out_dialog (SHELL_MODAL_DIALOG (self));
  stop_timer (self);
  stop_alt_capture (self);
  emit_confirm_signal (self, call->signal);
}

static void
confirm_after_set_action (ConfirmCall *call,
                          gboolean     action_changed)
{
  /* The app may not necessarily require reboot to apply the updates, thus
   * do that only if the action was changed; it may fail to set the action
   * too, then the right way is to shutdown, not reboot. If the app supports
   * changing the action after the offline updates are applied, reboot now
   * to apply the offline updates immediately, and the app will shutdown
   * the computer for us when the update is finished. */
  if (call->reboot_if_action_changed && action_changed)
    call->signal = CONFIRM_REBOOT;

  confirm_finish (call);
}

static void
set_action_ready (GObject      *source,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  ConfirmCall *call = user_data;
  g_autoptr (GError) error = NULL;
  gboolean changed;

  changed = shell_software_offline_updates_call_set_action_finish (SHELL_SOFTWARE_OFFLINE_UPDATES (source),
                                                                   res, &error);
  if (!changed)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          confirm_call_free (call);
          return;
        }

      /* Not all implementations can change the action after the update is
       * applied; it's indicated by returning the "not-supported" error by
       * gnome-software */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
        g_message ("%s", error->message);
    }

  confirm_after_set_action (call, changed);
}

static void
set_post_update_action (ConfirmCall *call,
                        const char  *action)
{
  ShellEndSessionDialog *self = call->self;

  /* Handle this gracefully if gnome-software is not available */
  if (self->software_proxy == NULL)
    {
      confirm_after_set_action (call, FALSE);
      return;
    }

  shell_software_offline_updates_call_set_action (self->software_proxy, action,
                                                  self->cancellable,
                                                  set_action_ready, call);
}

static void
cancel_update_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  ConfirmCall *call = user_data;
  g_autoptr (GError) error = NULL;

  if (!shell_software_offline_updates_call_cancel_finish (SHELL_SOFTWARE_OFFLINE_UPDATES (source),
                                                          res, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          confirm_call_free (call);
          return;
        }

      g_message ("%s", error->message);
    }

  confirm_finish (call);
}

static void
trigger_offline_update_cancel (ConfirmCall *call)
{
  ShellEndSessionDialog *self = call->self;

  /* Handle this gracefully if gnome-software is not available */
  if (self->software_proxy == NULL)
    {
      confirm_finish (call);
      return;
    }

  shell_software_offline_updates_call_cancel (self->software_proxy,
                                              self->cancellable,
                                              cancel_update_ready, call);
}

static void
confirm (ShellEndSessionDialog *self,
         ConfirmSignal          signal)
{
  ConfirmCall *call;

  if (self->disposed)
    return;

  call = g_new0 (ConfirmCall, 1);
  call->self = g_object_ref (self);
  call->signal = signal;

  if (clutter_actor_is_visible (CLUTTER_ACTOR (self->check_box)))
    {
      /* Trigger the offline update as requested */
      if (st_button_get_checked (ST_BUTTON (self->check_box)))
        {
          switch (signal)
            {
            case CONFIRM_REBOOT:
              set_post_update_action (call, OFFLINE_UPDATE_ACTION_REBOOT);
              return;

            case CONFIRM_SHUTDOWN:
              call->reboot_if_action_changed = TRUE;
              set_post_update_action (call, OFFLINE_UPDATE_ACTION_SHUTDOWN);
              return;

            default:
              break;
            }
        }
      else
        {
          trigger_offline_update_cancel (call);
          return;
        }
    }
  else if (self->type == DIALOG_TYPE_UPDATE_RESTART &&
           (self->update_prepared || self->update_scheduled))
    {
      set_post_update_action (call, OFFLINE_UPDATE_ACTION_REBOOT);
      return;
    }

  confirm_finish (call);
}

static void
confirm_reboot_to_boot_loader_menu (ShellEndSessionDialog *self)
{
  shell_login_manager_set_reboot_to_boot_loader_menu (self->login_manager);
  confirm (self, CONFIRM_REBOOT);
}

/**
 * shell_end_session_dialog_cancel:
 * @self: a #ShellEndSessionDialog
 *
 * Tells gnome-session that the user canceled the dialog and closes it
 * (`cancel ()`).
 */
void
shell_end_session_dialog_cancel (ShellEndSessionDialog *self)
{
  g_return_if_fail (SHELL_IS_END_SESSION_DIALOG (self));

  stop_timer (self);
  stop_alt_capture (self);
  shell_session_manager_end_session_dialog_emit_canceled (self->skeleton);
  shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
}

static void
cancel_button_cb (gpointer user_data)
{
  shell_end_session_dialog_cancel (user_data);
}

/* Closes the dialog without telling the caller of the D-Bus API, as the
 * dialog is dismissed by its answer */
static void
close_silently (ShellEndSessionDialog *self)
{
  self->skip_closed_signal = TRUE;
  shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
}

/* A one-shot handler of ::closed that confirms */
typedef struct
{
  ShellEndSessionDialog *self;
  ConfirmSignal signal;
  gboolean boot_loader_menu;
  gulong handler_id;
} ClosedConfirm;

static void
closed_confirm_cb (ShellEndSessionDialog *self,
                   ClosedConfirm         *data)
{
  gboolean boot_loader_menu = data->boot_loader_menu;
  ConfirmSignal signal = data->signal;

  g_signal_handler_disconnect (self, data->handler_id);

  if (boot_loader_menu)
    confirm_reboot_to_boot_loader_menu (self);
  else
    confirm (self, signal);
}

static void
connect_closed_confirm (ShellEndSessionDialog *self,
                        ConfirmSignal          signal,
                        gboolean               boot_loader_menu)
{
  ClosedConfirm *data = g_new0 (ClosedConfirm, 1);

  data->self = self;
  data->signal = signal;
  data->boot_loader_menu = boot_loader_menu;
  data->handler_id = g_signal_connect_data (self, "closed",
                                            G_CALLBACK (closed_confirm_cb), data,
                                            (GClosureNotify) g_free, 0);
}

static void
confirm_button_cb (gpointer user_data)
{
  ShellEndSessionDialog *self = user_data;

  connect_closed_confirm (self, get_confirm_signal (self->type), FALSE);
  close_silently (self);
}

static void
boot_options_button_cb (gpointer user_data)
{
  ShellEndSessionDialog *self = user_data;

  connect_closed_confirm (self, CONFIRM_REBOOT, TRUE);
  close_silently (self);
}

static void
update_buttons (ShellEndSessionDialog *self)
{
  ShellModalDialog *dialog = SHELL_MODAL_DIALOG (self);
  StButton *button;

  shell_modal_dialog_clear_buttons (dialog);
  stop_alt_capture (self);
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->alt_capture), FALSE);

  shell_modal_dialog_add_button (dialog, _("Cancel"), CLUTTER_KEY_Escape, FALSE,
                                 cancel_button_cb, self, NULL);

  button = shell_modal_dialog_add_button (dialog, get_confirm_label (self->type), 0, FALSE,
                                          confirm_button_cb, self, NULL);

  /* Add Alt "Boot Options" option to the Reboot button */
  if (self->can_reboot_to_boot_loader_menu &&
      get_confirm_signal (self->type) == CONFIRM_REBOOT)
    {
      StButton *alt_button;

      g_set_weak_pointer (&self->reboot_button, button);
      alt_button = shell_modal_dialog_add_button (dialog,
                                                  C_("button", "Boot Options"), 0, FALSE,
                                                  boot_options_button_cb, self, NULL);
      g_set_weak_pointer (&self->reboot_button_alt, alt_button);
      clutter_actor_hide (CLUTTER_ACTOR (alt_button));
      clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->alt_capture), TRUE);
    }
}

static void
shell_end_session_dialog_real_close (ShellModalDialog *dialog)
{
  ShellEndSessionDialog *self = SHELL_END_SESSION_DIALOG (dialog);
  gboolean skip_signal = self->skip_closed_signal;

  self->skip_closed_signal = FALSE;

  SHELL_MODAL_DIALOG_CLASS (shell_end_session_dialog_parent_class)->close (dialog);

  if (!skip_signal && !self->disposed)
    shell_session_manager_end_session_dialog_emit_closed (self->skeleton);
}

/* ------------------------------------------------------------------------ */
/* The countdown                                                            */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellEndSessionDialog *self;   /* the timeout is removed in dispose () */
  gint64 start_time;
} TimerData;

static gboolean
timer_cb (gpointer user_data)
{
  TimerData *data = user_data;
  ShellEndSessionDialog *self = data->self;
  gint64 current_time = g_get_monotonic_time ();
  double seconds_elapsed = (current_time - data->start_time) / 1000000.0;

  self->seconds_left = self->total_seconds_to_stay_open - seconds_elapsed;
  if (self->seconds_left > 0)
    {
      sync_dialog (self);
      return G_SOURCE_CONTINUE;
    }

  self->timer_id = 0;
  confirm (self, get_confirm_signal (self->type));

  return G_SOURCE_REMOVE;
}

static void
start_timer (ShellEndSessionDialog *self)
{
  TimerData *data;

  stop_timer (self);

  data = g_new0 (TimerData, 1);
  data->self = self;
  data->start_time = g_get_monotonic_time ();
  self->seconds_left = self->total_seconds_to_stay_open;

  self->timer_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, 1, timer_cb,
                                               data, g_free);
  g_source_set_name_by_id (self->timer_id, "[gnome-shell] ShellEndSessionDialog confirm");
}

/* ------------------------------------------------------------------------ */
/* The applications which inhibit the end of the session                    */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellEndSessionDialog *self;
  guint request_id;
} InhibitorCall;

static void
inhibitor_call_free (InhibitorCall *call)
{
  g_clear_object (&call->self);
  g_free (call);
}

static ShellApp *
find_app_from_inhibitor (ShellSessionManagerInhibitor *inhibitor)
{
  g_autofree char *app_id = NULL;
  g_autofree char *desktop_file = NULL;
  g_autoptr (GError) error = NULL;

  if (!shell_session_manager_inhibitor_call_get_app_id_sync (inhibitor, &app_id, NULL, &error))
    {
      /* XXX -- sometimes JIT inhibitors generated by gnome-session get
       * removed too soon. Don't fail in this case. */
      g_message ("gnome-session gave us a dead inhibitor: %s",
                 g_dbus_proxy_get_object_path (G_DBUS_PROXY (inhibitor)));
      return NULL;
    }

  if (g_str_has_suffix (app_id, ".desktop"))
    desktop_file = g_steal_pointer (&app_id);
  else
    desktop_file = g_strconcat (app_id, ".desktop", NULL);

  return shell_app_system_lookup_heuristic_basename (shell_global_get_app_system (shell_global_get ()),
                                                     desktop_file);
}

static gboolean
remove_application_request (ShellEndSessionDialog *self,
                            guint                  request_id)
{
  guint i;

  for (i = 0; i < self->applications->len; i++)
    {
      if (g_array_index (self->applications, guint, i) == request_id)
        {
          g_array_remove_index (self->applications, i);
          return TRUE;
        }
    }

  return FALSE;
}

static gboolean
has_application_request (ShellEndSessionDialog *self,
                         guint                  request_id)
{
  guint i;

  for (i = 0; i < self->applications->len; i++)
    {
      if (g_array_index (self->applications, guint, i) == request_id)
        return TRUE;
    }

  return FALSE;
}

static void
on_inhibitor_loaded (ShellEndSessionDialog         *self,
                     guint                          request_id,
                     ShellSessionManagerInhibitor  *inhibitor)
{
  ShellApp *app;
  guint32 flags = 0;

  if (!has_application_request (self, request_id))
    {
      /* Stale inhibitor */
      return;
    }

  app = inhibitor != NULL ? find_app_from_inhibitor (inhibitor) : NULL;
  if (app != NULL)
    shell_session_manager_inhibitor_call_get_flags_sync (inhibitor, &flags, NULL, NULL);

  if (app != NULL && (flags & SHELL_INHIBIT_FLAGS_LOGOUT))
    {
      g_autofree char *description = NULL;
      ClutterActor *list_item;

      shell_session_manager_inhibitor_call_get_reason_sync (inhibitor, &description, NULL, NULL);
      list_item = g_object_new (SHELL_TYPE_LIST_SECTION_ITEM,
                                "icon-actor", shell_app_create_icon_texture (app, ITEM_ICON_SIZE),
                                "title", shell_app_get_name (app),
                                "description", description,
                                NULL);
      clutter_actor_add_child (CLUTTER_ACTOR (shell_list_section_get_list (self->application_section)),
                               list_item);
    }
  else
    {
      /* The inhibiting app is a service (not an application) or is not
       * inhibiting logout/shutdown */
      remove_application_request (self, request_id);
    }

  sync_dialog (self);
}

static void
inhibitor_ready (GObject      *source,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  InhibitorCall *call = user_data;
  g_autoptr (ShellSessionManagerInhibitor) inhibitor = NULL;
  g_autoptr (GError) error = NULL;

  inhibitor = shell_gnome_session_inhibitor_new_finish (res, &error);
  if (inhibitor == NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          inhibitor_call_free (call);
          return;
        }

      g_message ("Failed to get a proxy for inhibitor: %s", error->message);
    }

  if (!call->self->disposed)
    on_inhibitor_loaded (call->self, call->request_id, inhibitor);

  inhibitor_call_free (call);
}

/* ------------------------------------------------------------------------ */
/* The other users which are logged in                                      */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellEndSessionDialog *self;
  guint serial;
  char *session_id;
} SessionsCall;

static void
sessions_call_free (SessionsCall *call)
{
  g_clear_object (&call->self);
  g_free (call->session_id);
  g_free (call);
}

static void
add_session (ShellEndSessionDialog *self,
             const char            *user_name,
             const char            *type,
             gboolean               remote)
{
  ActUser *user = act_user_manager_get_user (self->user_manager, user_name);
  ShellUserAvatar *avatar;
  const char *real_name;
  g_autofree char *user_label_text = NULL;
  ClutterActor *list_item;

  avatar = shell_user_avatar_new (user, ITEM_ICON_SIZE);
  shell_user_avatar_update (avatar);

  real_name = act_user_get_real_name (user);
  if (real_name == NULL)
    real_name = user_name;

  if (remote)
    /* Translators: Remote here refers to a remote session, like a ssh login */
    user_label_text = g_strdup_printf (_("%s (remote)"), real_name);
  else if (g_strcmp0 (type, "tty") == 0)
    /* Translators: Console here refers to a tty like a VT console */
    user_label_text = g_strdup_printf (_("%s (console)"), real_name);
  else
    user_label_text = g_strdup (user_name);

  list_item = g_object_new (SHELL_TYPE_LIST_SECTION_ITEM,
                            "icon-actor", avatar,
                            "title", user_label_text,
                            NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (shell_list_section_get_list (self->session_section)),
                           list_item);
}

static void
list_sessions_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  SessionsCall *call = user_data;
  ShellEndSessionDialog *self = call->self;
  g_autoptr (GVariant) sessions = NULL;
  GVariantIter iter;
  const char *session_path, *user_name;

  sessions = shell_login_manager_list_sessions_finish (SHELL_LOGIN_MANAGER (source), res);

  if (self->disposed || call->serial != self->open_serial)
    {
      sessions_call_free (call);
      return;
    }

  g_variant_iter_init (&iter, sessions);
  while (g_variant_iter_next (&iter, "(&su&s&s&o)", NULL, NULL, &user_name, NULL, &session_path))
    {
      g_autoptr (ShellLogin1Session) proxy = NULL;
      g_autoptr (GError) error = NULL;

      proxy = shell_login_manager_get_session (self->login_manager, session_path, &error);
      if (proxy == NULL)
        {
          if (error != NULL)
            g_debug ("%s: %s", G_STRFUNC, error->message);
          continue;
        }

      if (g_strcmp0 (shell_login1_session_get_class (proxy), "user") != 0)
        continue;

      if (g_strcmp0 (shell_login1_session_get_state (proxy), "closing") == 0)
        continue;

      if (g_strcmp0 (shell_login1_session_get_id (proxy), call->session_id) == 0)
        continue;

      add_session (self, user_name,
                   shell_login1_session_get_type_ (proxy),
                   shell_login1_session_get_remote (proxy));
      self->n_sessions++;

      /* Limit the number of entries */
      if (self->n_sessions == MAX_USERS_IN_SESSION_DIALOG)
        break;
    }

  sync_dialog (self);
  sessions_call_free (call);
}

static void
list_sessions (SessionsCall *call)
{
  shell_login_manager_list_sessions_async (call->self->login_manager,
                                           call->self->cancellable,
                                           list_sessions_ready, call);
}

static void
current_session_ready (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  SessionsCall *call = user_data;
  g_autoptr (ShellLogin1Session) session = NULL;
  g_autoptr (GError) error = NULL;

  session = shell_login_manager_get_current_session_finish (SHELL_LOGIN_MANAGER (source),
                                                             res, &error);
  if (session == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("endSessionDialog: could not get the session from logind: %s", error->message);
      sessions_call_free (call);
      return;
    }

  call->session_id = g_strdup (shell_login1_session_get_id (session));
  g_message ("endSessionDialog: No XDG_SESSION_ID, fetched from logind: %s", call->session_id);

  if (call->self->disposed || call->serial != call->self->open_serial)
    {
      sessions_call_free (call);
      return;
    }

  list_sessions (call);
}

static void
load_sessions (ShellEndSessionDialog *self)
{
  SessionsCall *call = g_new0 (SessionsCall, 1);
  const char *session_id = g_getenv ("XDG_SESSION_ID");

  call->self = g_object_ref (self);
  call->serial = self->open_serial;

  if (session_id == NULL || session_id[0] == '\0')
    {
      shell_login_manager_get_current_session_async (self->login_manager,
                                                     self->cancellable,
                                                     current_session_ready, call);
      return;
    }

  call->session_id = g_strdup (session_id);
  list_sessions (call);
}

/* ------------------------------------------------------------------------ */
/* Open ()                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellEndSessionDialog *self;
  GDBusMethodInvocation *invocation;
  guint serial;
  guint type;
  char **inhibitor_paths;
} OpenRequest;

static void
open_request_free (OpenRequest *request)
{
  g_clear_object (&request->self);
  g_clear_object (&request->invocation);
  g_strfreev (request->inhibitor_paths);
  g_free (request);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OpenRequest, open_request_free)

/* Whether the Open () call still has to be carried out; answers it if not */
static gboolean
open_request_is_current (OpenRequest *request)
{
  ShellEndSessionDialog *self = request->self;

  if (self->disposed || g_cancellable_is_cancelled (self->cancellable))
    {
      g_dbus_method_invocation_return_error_literal (request->invocation,
                                                     G_DBUS_ERROR,
                                                     G_DBUS_ERROR_FAILED,
                                                     "The shell is shutting down");
      return FALSE;
    }

  if (request->serial != self->open_serial)
    {
      /* Superseded by a later call */
      shell_session_manager_end_session_dialog_complete_open (self->skeleton,
                                                              request->invocation);
      return FALSE;
    }

  return TRUE;
}

static void
set_check_box_label (ShellEndSessionDialog *self,
                     const char            *text)
{
  if (text != NULL)
    {
      shell_check_box_set_label (self->check_box, text);
      clutter_actor_show (CLUTTER_ACTOR (self->check_box));
    }
  else
    {
      shell_check_box_set_label (self->check_box, "");
      clutter_actor_hide (CLUTTER_ACTOR (self->check_box));
    }
}

static void
open_request_finish (OpenRequest *request)
{
  ShellEndSessionDialog *self = request->self;
  const char *check_box_text;
  gboolean check_box_visible;
  guint i;

  self->type = request->type;

  /* Only consider updates if gnome-software is available */
  if (self->software_proxy != NULL && self->type == DIALOG_TYPE_RESTART)
    {
      if (self->update_scheduled)
        self->type = DIALOG_TYPE_UPDATE_RESTART;
    }

  g_array_set_size (self->applications, 0);
  clutter_actor_destroy_all_children (CLUTTER_ACTOR (shell_list_section_get_list (self->application_section)));

  self->n_sessions = 0;
  clutter_actor_destroy_all_children (CLUTTER_ACTOR (shell_list_section_get_list (self->session_section)));

  self->type_valid = dialog_type_is_valid (self->type);
  if (!self->type_valid)
    {
      g_dbus_method_invocation_return_dbus_error (request->invocation,
                                                  MODAL_DIALOG_ERROR_UNKNOWN_TYPE,
                                                  "Unknown dialog type requested");
      return;
    }

  for (i = 0; request->inhibitor_paths[i] != NULL; i++)
    {
      InhibitorCall *call = g_new0 (InhibitorCall, 1);

      /* Identifies this request until the proxy is ready, so that the
       * inhibitors of a previous Open () can be told apart */
      call->self = g_object_ref (self);
      call->request_id = ++self->next_request_id;
      g_array_append_val (self->applications, call->request_id);

      shell_gnome_session_inhibitor_new (request->inhibitor_paths[i],
                                         self->cancellable,
                                         inhibitor_ready, call);
    }

  if (get_show_other_sessions (self->type))
    load_sessions (self);

  check_box_text = get_check_box_text (self->type);
  set_check_box_label (self, check_box_text);
  check_box_visible = check_box_text != NULL &&
                      (self->update_prepared || self->update_scheduled);
  set_visible (CLUTTER_ACTOR (self->check_box), check_box_visible);

  st_button_set_checked (ST_BUTTON (self->check_box),
                         check_box_visible && !is_battery_low (self));

  set_visible (CLUTTER_ACTOR (self->battery_warning),
               should_show_low_battery_warning (self));

  update_buttons (self);

  if (!shell_modal_dialog_open (SHELL_MODAL_DIALOG (self)))
    {
      g_dbus_method_invocation_return_dbus_error (request->invocation,
                                                  MODAL_DIALOG_ERROR_GRAB_FAILED,
                                                  "Cannot grab pointer and keyboard");
      return;
    }

  start_timer (self);

  sync_dialog (self);

  shell_session_manager_end_session_dialog_complete_open (self->skeleton,
                                                          request->invocation);
}

static void
open_request_state_known (OpenRequest *request,
                          const char  *state)
{
  g_autoptr (OpenRequest) owned_request = request;
  ShellEndSessionDialog *self = request->self;

  if (!open_request_is_current (request))
    return;

  self->update_prepared = g_strcmp0 (state, "prepared") == 0;
  self->update_scheduled = g_strcmp0 (state, "scheduled") == 0;

  open_request_finish (request);
}

static void
get_state_ready (GObject      *source,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  OpenRequest *request = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *state = NULL;

  if (!shell_software_offline_updates_call_get_state_finish (SHELL_SOFTWARE_OFFLINE_UPDATES (source),
                                                             &state, res, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Failed to get update info from gnome-software: %s", error->message);
    }

  open_request_state_known (request, state);
}

static void
software_proxy_ready (GObject      *source,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  OpenRequest *request = user_data;
  ShellEndSessionDialog *self = request->self;
  g_autoptr (GError) error = NULL;
  ShellSoftwareOfflineUpdates *proxy;

  proxy = shell_software_offline_updates_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("%s", error->message);
    }
  else if (g_dbus_proxy_get_name_owner (G_DBUS_PROXY (proxy)) == NULL)
    {
      /* Creating a D-Bus proxy won't propagate SERVICE_UNKNOWN or
       * NAME_HAS_NO_OWNER errors if gnome-software is not available, but
       * the GIO implementation will make sure in that case that the
       * proxy's g-name-owner is set to NULL, so check that. */
      g_clear_object (&proxy);
    }

  if (proxy != NULL)
    {
      if (self->disposed || self->software_proxy != NULL)
        g_object_unref (proxy);
      else
        self->software_proxy = proxy;
    }

  if (self->software_proxy == NULL)
    {
      open_request_state_known (request, "unknown");
      return;
    }

  shell_software_offline_updates_call_get_state (self->software_proxy,
                                                 self->cancellable,
                                                 get_state_ready, request);
}

static void
get_update_state (OpenRequest *request)
{
  ShellEndSessionDialog *self = request->self;
  ShellSessionMode *session_mode = shell_main_get_session_mode (shell_main_get_default ());

  if (self->software_proxy != NULL)
    {
      shell_software_offline_updates_call_get_state (self->software_proxy,
                                                     self->cancellable,
                                                     get_state_ready, request);
      return;
    }

  if (session_mode != NULL && shell_session_mode_is_greeter (session_mode))
    {
      open_request_state_known (request, "unknown");
      return;
    }

  shell_software_offline_updates_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                    G_DBUS_PROXY_FLAGS_NONE,
                                                    SOFTWARE_BUS_NAME,
                                                    SOFTWARE_OFFLINE_UPDATES_PATH,
                                                    self->cancellable,
                                                    software_proxy_ready, request);
}

static gboolean
on_handle_open (ShellSessionManagerEndSessionDialog *skeleton,
                GDBusMethodInvocation               *invocation,
                guint                                type,
                guint                                timestamp,
                guint                                total_seconds_to_stay_open,
                const char * const                  *inhibitor_object_paths,
                ShellEndSessionDialog               *self)
{
  OpenRequest *request = g_new0 (OpenRequest, 1);

  if (self->disposed)
    {
      g_free (request);
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_DBUS_ERROR,
                                                     G_DBUS_ERROR_FAILED,
                                                     "The shell is shutting down");
      return TRUE;
    }

  self->total_seconds_to_stay_open = total_seconds_to_stay_open;

  request->self = g_object_ref (self);
  request->invocation = g_object_ref (invocation);
  request->serial = ++self->open_serial;
  request->type = type;
  request->inhibitor_paths = g_strdupv ((char **) inhibitor_object_paths);

  get_update_state (request);
  return TRUE;
}

static gboolean
on_handle_close (ShellSessionManagerEndSessionDialog *skeleton,
                 GDBusMethodInvocation               *invocation,
                 ShellEndSessionDialog               *self)
{
  shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
  shell_session_manager_end_session_dialog_complete_close (skeleton, invocation);
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* The class                                                                */
/* ------------------------------------------------------------------------ */

static void
can_reboot_to_boot_loader_menu_ready (GObject      *source,
                                      GAsyncResult *res,
                                      gpointer      user_data)
{
  g_autoptr (ShellEndSessionDialog) self = user_data;
  gboolean can_reboot;

  can_reboot = shell_login_manager_can_reboot_to_boot_loader_menu_finish (SHELL_LOGIN_MANAGER (source),
                                                                          res, NULL);
  if (!self->disposed)
    self->can_reboot_to_boot_loader_menu = can_reboot;
}

static void
on_shutdown (ShellGlobal           *global,
             ShellEndSessionDialog *self)
{
  g_cancellable_cancel (self->cancellable);
  g_clear_handle_id (&self->timer_id, g_source_remove);
}

static void
shell_end_session_dialog_constructed (GObject *object)
{
  ShellEndSessionDialog *self = SHELL_END_SESSION_DIALOG (object);
  ShellModalDialog *dialog = SHELL_MODAL_DIALOG (self);
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GError) error = NULL;
  ClutterActor *content_layout;
  ClutterActor *content;
  ClutterActor *battery_warning;
  ClutterActor *section;
  ShellCheckBox *check_box;
  ClutterText *text;
  ClutterAction *alt_capture;

  G_OBJECT_CLASS (shell_end_session_dialog_parent_class)->constructed (object);

  self->cancellable = g_cancellable_new ();
  self->applications = g_array_new (FALSE, FALSE, sizeof (guint));

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  self->login_manager = shell_login_manager_get_default ();
  shell_login_manager_can_reboot_to_boot_loader_menu_async (self->login_manager,
                                                            self->cancellable,
                                                            can_reboot_to_boot_loader_menu_ready,
                                                            g_object_ref (self));

  self->user_manager = act_user_manager_get_default ();
  self->user = g_object_ref (act_user_manager_get_user (self->user_manager,
                                                        g_get_user_name ()));

  /* The proxy of gnome-software is opened only when the dialog is opening,
   * to avoid early start of gnome-software, which is delayed by its systemd
   * file, to not use too many resources right after login */

  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE,
                            NULL,
                            UPOWER_BUS_NAME,
                            UPOWER_DISPLAY_DEVICE_PATH,
                            UPOWER_DEVICE_INTERFACE,
                            self->cancellable,
                            power_proxy_ready, g_object_ref (self));

  g_signal_connect (self, "opened", G_CALLBACK (opened_cb), NULL);

  g_signal_connect_object (self->user, "notify::is-loaded",
                           G_CALLBACK (user_loaded_cb), self, 0);
  g_signal_connect_object (self->user, "changed",
                           G_CALLBACK (user_changed_cb), self, 0);

  content_layout = CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (dialog));

  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT, NULL);
  g_set_weak_pointer (&self->message_dialog_content, SHELL_MESSAGE_DIALOG_CONTENT (content));

  check_box = shell_check_box_new (NULL);
  g_set_weak_pointer (&self->check_box, check_box);
  g_signal_connect_object (check_box, "clicked",
                           G_CALLBACK (check_box_clicked_cb), self, 0);
  clutter_actor_add_child (content, CLUTTER_ACTOR (check_box));

  battery_warning = g_object_new (ST_TYPE_LABEL,
                                  "style-class", "end-session-dialog-battery-warning",
                                  "text", _("Low battery power: please plug in before installing updates"),
                                  NULL);
  g_set_weak_pointer (&self->battery_warning, ST_LABEL (battery_warning));
  text = st_label_get_clutter_text (ST_LABEL (battery_warning));
  clutter_text_set_ellipsize (text, PANGO_ELLIPSIZE_NONE);
  clutter_text_set_line_wrap (text, TRUE);
  clutter_actor_add_child (content, battery_warning);

  clutter_actor_add_child (content_layout, content);

  section = g_object_new (SHELL_TYPE_LIST_SECTION,
                          "title", _("Some applications are busy or have unsaved work"),
                          NULL);
  g_set_weak_pointer (&self->application_section, SHELL_LIST_SECTION (section));
  clutter_actor_add_child (content_layout, section);

  section = g_object_new (SHELL_TYPE_LIST_SECTION,
                          "title", _("Other users are logged in"),
                          NULL);
  g_set_weak_pointer (&self->session_section, SHELL_LIST_SECTION (section));
  clutter_actor_add_child (content_layout, section);

  self->skeleton = shell_session_manager_end_session_dialog_skeleton_new ();
  g_signal_connect (self->skeleton, "handle-open",
                    G_CALLBACK (on_handle_open), self);
  g_signal_connect (self->skeleton, "handle-close",
                    G_CALLBACK (on_handle_close), self);

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (connection == NULL ||
      !g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                         connection, END_SESSION_DIALOG_OBJECT_PATH,
                                         &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);

  alt_capture = clutter_key_controller_new (NULL);
  self->alt_capture = CLUTTER_KEY_CONTROLLER (g_object_ref_sink (alt_capture));
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (alt_capture), FALSE);
  g_signal_connect_object (alt_capture, "key-press",
                           G_CALLBACK (alt_key_press_cb), self, 0);
  g_signal_connect_object (alt_capture, "key-release",
                           G_CALLBACK (alt_key_release_cb), self, 0);
  clutter_actor_add_action_full (CLUTTER_ACTOR (self), "alt-capture-controller",
                                 CLUTTER_PHASE_CAPTURE, alt_capture);
}

/* dialog-style-class and destroy-on-close are construct-only properties of
 * the parent, which the JS class forced in `_init ()` */
static GObject *
shell_end_session_dialog_constructor (GType                  type,
                                      guint                  n_construct_properties,
                                      GObjectConstructParam *construct_properties)
{
  guint i;

  for (i = 0; i < n_construct_properties; i++)
    {
      const char *name = g_param_spec_get_name (construct_properties[i].pspec);

      if (strcmp (name, "dialog-style-class") == 0)
        g_value_set_string (construct_properties[i].value, "end-session-dialog");
      else if (strcmp (name, "destroy-on-close") == 0)
        g_value_set_boolean (construct_properties[i].value, FALSE);
    }

  return G_OBJECT_CLASS (shell_end_session_dialog_parent_class)->constructor (type,
                                                                              n_construct_properties,
                                                                              construct_properties);
}

static void
shell_end_session_dialog_dispose (GObject *object)
{
  ShellEndSessionDialog *self = SHELL_END_SESSION_DIALOG (object);

  self->disposed = TRUE;

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_handle_id (&self->timer_id, g_source_remove);

  if (self->skeleton != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->skeleton, self);
      if (g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (self->skeleton)) != NULL)
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->skeleton));
      g_clear_object (&self->skeleton);
    }

  if (self->user != NULL)
    g_signal_handlers_disconnect_by_data (self->user, self);
  g_clear_object (&self->user);

  if (self->power_proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->power_proxy, self);
  g_clear_object (&self->power_proxy);
  g_clear_object (&self->software_proxy);

  g_clear_weak_pointer (&self->reboot_button);
  g_clear_weak_pointer (&self->reboot_button_alt);
  g_clear_weak_pointer (&self->message_dialog_content);
  g_clear_weak_pointer (&self->check_box);
  g_clear_weak_pointer (&self->battery_warning);
  g_clear_weak_pointer (&self->application_section);
  g_clear_weak_pointer (&self->session_section);

  /* The action holds its own reference, it is dropped with the actor */
  g_clear_object (&self->alt_capture);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (shell_end_session_dialog_parent_class)->dispose (object);
}

static void
shell_end_session_dialog_finalize (GObject *object)
{
  ShellEndSessionDialog *self = SHELL_END_SESSION_DIALOG (object);

  g_clear_pointer (&self->applications, g_array_unref);

  G_OBJECT_CLASS (shell_end_session_dialog_parent_class)->finalize (object);
}

static void
shell_end_session_dialog_class_init (ShellEndSessionDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellModalDialogClass *dialog_class = SHELL_MODAL_DIALOG_CLASS (klass);

  object_class->constructor = shell_end_session_dialog_constructor;
  object_class->constructed = shell_end_session_dialog_constructed;
  object_class->dispose = shell_end_session_dialog_dispose;
  object_class->finalize = shell_end_session_dialog_finalize;

  dialog_class->close = shell_end_session_dialog_real_close;
}

static void
shell_end_session_dialog_init (ShellEndSessionDialog *self)
{
}

/**
 * shell_end_session_dialog_new:
 *
 * Creates the dialog and exports its D-Bus interface (`new EndSessionDialog
 * ()`). It stays alive when closed. `dialog-style-class` and
 * `destroy-on-close` are forced to "end-session-dialog" and %FALSE.
 *
 * Returns: (transfer full): a new #ShellEndSessionDialog
 */
ShellEndSessionDialog *
shell_end_session_dialog_new (void)
{
  return g_object_new (SHELL_TYPE_END_SESSION_DIALOG, NULL);
}
