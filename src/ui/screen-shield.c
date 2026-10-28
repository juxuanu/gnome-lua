/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * screen-shield.c: The lock screen: it locks and unlocks the session, fades
 * the screen when the session goes idle, shows the dialog which asks the user
 * to authenticate and exports org.gnome.ScreenSaver. Port of
 * js/ui/screenShield.js.
 *
 * If you are setting org.gnome.desktop.session.idle-delay directly in dconf,
 * rather than through System Settings, you also need to set
 * org.gnome.settings-daemon.plugins.power.sleep-display-ac and
 * org.gnome.settings-daemon.plugins.power.sleep-display-battery to the same value.
 * This will ensure that the screen blanks at the right time when it fades out.
 * https://bugzilla.gnome.org/show_bug.cgi?id=668703 explains the dependency.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - org.gnome.ScreenSaver is exported by the #ShellScreenSaverService the
 *    shield creates, as before.
 *  - The time limits manager (`Main.timeLimitsManager`) is the
 *    :time-limits-manager property, which ShellMain sets once it exists.
 *  - The dialog is created from the value of #ShellSessionMode:unlock-dialog
 *    (`Main.getUnlockDialogClass ()` of ShellMain): ShellLoginDialog for
 *    `login-dialog`, ShellUnlockDialog for `unlock-dialog`.
 *  - Failing to reach the session manager or logind is a message (the JS
 *    logged the exception), and nothing runs after ShellGlobal::shutdown.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <act/act.h>
#include <clutter/clutter.h>
/* clutter.h does not include the motion controller (mutter 51) */
#define __CLUTTER_H_INSIDE__
#include <clutter/clutter-motion-controller.h>
#undef __CLUTTER_H_INSIDE__
#include <glib/gi18n-lib.h>
#include <meta/meta-cursor-tracker.h>

#include "gdm/credential-manager.h"
#include "gdm/login-dialog.h"
#include "gdm/smartcard-manager.h"
#include "misc/gnome-session.h"
#include "misc/login-manager.h"
#include "shell-action-modes.h"
#include "shell-global.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/lightbox.h"
#include "ui/main.h"
#include "ui/message-tray.h"
#include "ui/overview.h"
#include "ui/screen-shield.h"
#include "ui/session-mode.h"
#include "ui/shell-dbus.h"
#include "ui/unlock-dialog.h"

#define SCREENSAVER_SCHEMA "org.gnome.desktop.screensaver"
#define LOCK_ENABLED_KEY "lock-enabled"
#define LOCK_DELAY_KEY "lock-delay"

#define LOCKDOWN_SCHEMA "org.gnome.desktop.lockdown"
#define DISABLE_LOCK_KEY "disable-lock-screen"

#define LOCKED_STATE_STR "screenShield.locked"

/* ScreenShield animation time
 * - STANDARD_FADE_TIME is used when the session goes idle
 * - MANUAL_FADE_TIME is used for lowering the shield when asked by the user,
 *   or when cancelling the dialog
 * - CURTAIN_SLIDE_TIME is used when raising the shield before unlocking */
#define STANDARD_FADE_TIME 10000
#define MANUAL_FADE_TIME 300
#define CURTAIN_SLIDE_TIME 300

/* Which dialog the shield shows */
typedef enum
{
  DIALOG_KIND_NONE,
  DIALOG_KIND_LOGIN,
  DIALOG_KIND_UNLOCK,
} DialogKind;

/**
 * ShellScreenShield:
 *
 * The lock screen (`ScreenShield`).
 */
struct _ShellScreenShield
{
  GObject parent;

  ShellTimeLimitsManager *time_limits_manager;

  ClutterActor *actor;                   /* weak, the screen shield group */
  ShellNotificationState lock_screen_state;
  ClutterActor *lock_screen_group;       /* weak */
  ClutterActor *lock_dialog_group;       /* weak */

  ShellSessionManagerPresence *presence;
  ShellScreenSaverService *screen_saver_dbus;
  ShellSmartcardManager *smartcard_manager;
  GHashTable *credential_managers;       /* service name -> ShellCredentialManager */

  ShellLoginManager *login_manager;
  ShellLogin1Session *login_session;

  GSettings *settings;
  GSettings *lock_settings;

  ClutterGrab *grab;
  gboolean is_greeter;
  gboolean is_active;
  gboolean is_locked;
  gboolean in_unlock_animation;
  gboolean inhibited;
  gint64 activation_time;
  guint became_active_id;
  guint lock_timeout_id;
  guint activate_fade_id;

  ShellLightbox *long_lightbox;          /* weak */
  ShellLightbox *short_lightbox;         /* weak */

  MetaIdleMonitor *idle_monitor;         /* weak */
  MetaCursorTracker *cursor_tracker;     /* weak */

  ClutterAction *motion_listener;
  gboolean cursor_visible_inhibited;

  ClutterActor *dialog;                  /* weak */
  DialogKind dialog_kind;
  gulong dialog_failed_id;
  gulong dialog_wake_up_screen_id;

  GInputStream *inhibitor;
  GCancellable *inhibit_cancellable;

  GCancellable *cancellable;
  gboolean shutting_down;
  ShellGlobal *global;                   /* weak, for shutdown_id */
  gulong shutdown_id;
};

G_DEFINE_FINAL_TYPE (ShellScreenShield, shell_screen_shield, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_LOCKED,
  PROP_ACTIVE,
  PROP_ACTIVATION_TIME,
  PROP_TIME_LIMITS_MANAGER,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  ACTIVE_CHANGED,
  LOCKED_CHANGED,
  WAKE_UP_SCREEN,
  LOCK_SCREEN_SHOWN,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

static void sync_inhibitor (ShellScreenShield *self);
static gboolean ensure_unlock_dialog (ShellScreenShield *self,
                                      gboolean           allow_cancel);
static void on_user_became_active (ShellScreenShield *self);
static void hide_lock_screen (ShellScreenShield *self,
                              gboolean           animate);

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

static gboolean
use_motion (void)
{
  return st_settings_get_reduced_motion (st_settings_get ()) != ST_REDUCED_MOTION_REDUCE;
}

static ShellMain *
get_main (void)
{
  return shell_main_get_default ();
}

static ClutterActor *
get_ui_group (void)
{
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (get_main ());

  if (layout_manager == NULL)
    return NULL;

  return CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager));
}

static ShellSessionMode *
get_session_mode (void)
{
  return shell_main_get_session_mode (get_main ());
}

static gboolean
session_mode_has_unlock_dialog (void)
{
  ShellSessionMode *session_mode = get_session_mode ();
  g_autofree char *unlock_dialog = NULL;

  if (session_mode == NULL)
    return FALSE;

  g_object_get (session_mode, "unlock-dialog", &unlock_dialog, NULL);
  return unlock_dialog != NULL && unlock_dialog[0] != '\0';
}

static gboolean
should_lock_session (ShellScreenShield *self)
{
  return self->time_limits_manager != NULL &&
         shell_time_limits_manager_get_should_lock_session (self->time_limits_manager);
}

/* ---- the dialog, whichever it is ---- */

static void
dialog_cancel (ShellScreenShield *self)
{
  if (self->dialog == NULL)
    return;

  if (self->dialog_kind == DIALOG_KIND_LOGIN)
    shell_login_dialog_cancel (SHELL_LOGIN_DIALOG (self->dialog));
  else
    shell_unlock_dialog_cancel (SHELL_UNLOCK_DIALOG (self->dialog));
}

static void
dialog_activate (ShellScreenShield *self)
{
  if (self->dialog == NULL)
    return;

  if (self->dialog_kind == DIALOG_KIND_LOGIN)
    shell_login_dialog_activate (SHELL_LOGIN_DIALOG (self->dialog));
  else
    shell_unlock_dialog_activate (SHELL_UNLOCK_DIALOG (self->dialog));
}

static void
dialog_finish (ShellScreenShield         *self,
               ShellAuthPromptFinishFunc  func,
               gpointer                   user_data,
               GDestroyNotify             destroy)
{
  if (self->dialog_kind == DIALOG_KIND_LOGIN)
    shell_login_dialog_finish (SHELL_LOGIN_DIALOG (self->dialog), func, user_data, destroy);
  else
    shell_unlock_dialog_finish (SHELL_UNLOCK_DIALOG (self->dialog), func, user_data, destroy);
}

static void
dialog_pop_modal (ShellScreenShield *self)
{
  if (self->dialog != NULL && self->dialog_kind == DIALOG_KIND_UNLOCK)
    shell_unlock_dialog_pop_modal (SHELL_UNLOCK_DIALOG (self->dialog));
}

static void
dialog_grab_or_activate (ShellScreenShield *self)
{
  if (self->dialog == NULL)
    return;

  if (self->is_greeter)
    dialog_activate (self);
  else
    clutter_actor_grab_key_focus (self->dialog);
}

/* ------------------------------------------------------------------------ */
/* The state                                                                 */
/* ------------------------------------------------------------------------ */

static void
set_active (ShellScreenShield *self,
            gboolean           active)
{
  gboolean prev_is_active = self->is_active;

  self->is_active = active;

  if (prev_is_active != self->is_active)
    {
      g_signal_emit (self, signals[ACTIVE_CHANGED], 0);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE]);
    }

  sync_inhibitor (self);
}

static void
locked_hint_done_cb (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!shell_login1_session_call_set_locked_hint_finish (SHELL_LOGIN1_SESSION (source), res, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("Failed to set the locked hint: %s", error->message);
}

static void
set_locked (ShellScreenShield *self,
            gboolean           locked)
{
  gboolean prev_is_locked = self->is_locked;

  self->is_locked = locked;

  if (prev_is_locked != self->is_locked)
    {
      g_signal_emit (self, signals[LOCKED_CHANGED], 0);
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LOCKED]);
    }

  if (self->login_session != NULL)
    shell_login1_session_call_set_locked_hint (self->login_session, locked,
                                               self->cancellable, locked_hint_done_cb, NULL);
}

/* `_activateDialog ()` */
static void
activate_dialog (ShellScreenShield *self)
{
  if (self->is_locked)
    {
      ensure_unlock_dialog (self, TRUE);
      dialog_activate (self);
    }
  else
    {
      shell_screen_shield_deactivate (self, TRUE);
    }
}

/* `_maybeCancelDialog ()` */
static void
maybe_cancel_dialog (ShellScreenShield *self)
{
  dialog_cancel (self);
}

/* `_becomeModal ()` */
static void
become_modal (ShellScreenShield *self)
{
  if (self->grab != NULL)
    return;

  self->grab = shell_main_push_modal (get_main (), get_ui_group (),
                                      SHELL_ACTION_MODE_LOCK_SCREEN);
  if (self->grab != NULL)
    g_object_ref (self->grab);
}

static void
drop_modal (ShellScreenShield *self)
{
  if (self->grab != NULL)
    {
      g_autoptr (GError) error = NULL;

      shell_main_pop_modal (get_main (), self->grab, &error);
      g_clear_object (&self->grab);
    }
}

/* ---- the inhibitor ---- */

static void
inhibit_ready_cb (GObject      *source,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  g_autoptr (ShellScreenShield) self = user_data;
  g_autoptr (GError) error = NULL;
  GInputStream *inhibitor;

  inhibitor = shell_login_manager_inhibit_finish (SHELL_LOGIN_MANAGER (source), res, &error);
  if (inhibitor == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Failed to inhibit suspend: %s", error->message);
      return;
    }

  if (self->shutting_down)
    {
      g_object_unref (inhibitor);
      return;
    }

  g_clear_object (&self->inhibitor);
  self->inhibitor = inhibitor;
}

/* `_syncInhibitor ()` */
static void
sync_inhibitor (ShellScreenShield *self)
{
  gboolean lock_enabled;
  gboolean lock_locked;
  gboolean inhibit;

  if (self->settings == NULL || self->lock_settings == NULL || self->shutting_down)
    return;

  lock_enabled = g_settings_get_boolean (self->settings, LOCK_ENABLED_KEY);
  lock_locked = g_settings_get_boolean (self->lock_settings, DISABLE_LOCK_KEY);
  inhibit = self->login_session != NULL &&
            shell_login1_session_get_active (self->login_session) &&
            !self->is_active && lock_enabled && !lock_locked &&
            session_mode_has_unlock_dialog ();

  if (inhibit == self->inhibited)
    return;

  self->inhibited = inhibit;

  if (self->inhibit_cancellable != NULL)
    g_cancellable_cancel (self->inhibit_cancellable);
  g_clear_object (&self->inhibit_cancellable);
  self->inhibit_cancellable = g_cancellable_new ();

  if (inhibit)
    {
      shell_login_manager_inhibit_async (self->login_manager,
                                         _("GNOME needs to lock the screen"),
                                         self->inhibit_cancellable,
                                         inhibit_ready_cb, g_object_ref (self));
    }
  else if (self->inhibitor != NULL)
    {
      g_input_stream_close (self->inhibitor, NULL, NULL);
      g_clear_object (&self->inhibitor);
    }
}

static void
settings_sync_inhibitor_cb (GSettings         *settings,
                            const char        *key,
                            ShellScreenShield *self)
{
  sync_inhibitor (self);
}

/* ---- the login session and the login manager ---- */

static void
login_session_lock_cb (ShellLogin1Session *session,
                       ShellScreenShield  *self)
{
  shell_screen_shield_lock (self, FALSE);
}

static void
login_session_unlock_cb (ShellLogin1Session *session,
                         ShellScreenShield  *self)
{
  shell_screen_shield_deactivate (self, FALSE);
}

static void
login_session_properties_changed_cb (GDBusProxy        *proxy,
                                     GVariant          *changed_properties,
                                     GStrv              invalidated_properties,
                                     ShellScreenShield *self)
{
  sync_inhibitor (self);
}

static void
login_session_ready_cb (GObject      *source,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  g_autoptr (ShellScreenShield) self = user_data;
  g_autoptr (GError) error = NULL;
  ShellLogin1Session *session;

  session = shell_login_manager_get_current_session_finish (SHELL_LOGIN_MANAGER (source), res, &error);
  if (session == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Could not get the login session: %s", error->message);
      return;
    }

  if (self->shutting_down)
    {
      g_object_unref (session);
      return;
    }

  g_clear_object (&self->login_session);
  self->login_session = session;

  g_signal_connect_object (session, "lock", G_CALLBACK (login_session_lock_cb), self, 0);
  g_signal_connect_object (session, "unlock", G_CALLBACK (login_session_unlock_cb), self, 0);
  g_signal_connect_object (session, "g-properties-changed",
                           G_CALLBACK (login_session_properties_changed_cb), self, 0);
  sync_inhibitor (self);
}

/* ---- the lightboxes and the idle fade ---- */

static void
user_became_active_watch_cb (MetaIdleMonitor *monitor,
                             guint            watch_id,
                             gpointer         user_data)
{
  /* The watch is removed once it fired */
  SHELL_SCREEN_SHIELD (user_data)->became_active_id = 0;
  on_user_became_active (SHELL_SCREEN_SHIELD (user_data));
}

/* `_activateFade ()` */
static void
activate_fade (ShellScreenShield *self,
               ShellLightbox     *lightbox,
               guint              time)
{
  ClutterActor *ui_group = get_ui_group ();

  if (ui_group != NULL)
    clutter_actor_set_child_above_sibling (ui_group, CLUTTER_ACTOR (lightbox), NULL);
  shell_lightbox_light_on (lightbox, time);

  if (self->became_active_id == 0)
    self->became_active_id =
      meta_idle_monitor_add_user_active_watch (self->idle_monitor,
                                               user_became_active_watch_cb, self, NULL);
}

/* `_onUserBecameActive ()`: called when the user becomes active after a
 * lightbox was activated. There are two possibilities here:
 * - we're called when already locked; we just go back to the lock screen curtain
 * - we're called because the session is IDLE but before the lightbox
 *   is fully shown; at this point isActive is false, so we just hide
 *   the lightbox, reset the activationTime and go back to the unlocked
 *   desktop
 *   using deactivate() is a little of overkill, but it ensures we
 *   don't forget of some bit like modal, DBus properties or idle watches
 *
 * Note: if the (long) lightbox is shown then we're necessarily
 * active, because we call activate() without animation. */
static void
on_user_became_active (ShellScreenShield *self)
{
  if (self->became_active_id != 0)
    meta_idle_monitor_remove_watch (self->idle_monitor, self->became_active_id);
  self->became_active_id = 0;

  if (self->is_locked)
    {
      shell_lightbox_light_off (self->long_lightbox, 0);
      shell_lightbox_light_off (self->short_lightbox, 0);
    }
  else
    {
      shell_screen_shield_deactivate (self, FALSE);
    }
}

static void
long_lightbox_active_cb (GObject           *object,
                         GParamSpec        *pspec,
                         ShellScreenShield *self)
{
  if (shell_lightbox_get_active (SHELL_LIGHTBOX (object)))
    shell_screen_shield_activate (self, FALSE);
}

static void
short_lightbox_active_cb (GObject           *object,
                          GParamSpec        *pspec,
                          ShellScreenShield *self)
{
  if (shell_lightbox_get_active (SHELL_LIGHTBOX (object)))
    set_active (self, TRUE);
}

static gboolean
lock_timeout_cb (gpointer user_data)
{
  ShellScreenShield *self = SHELL_SCREEN_SHIELD (user_data);

  self->lock_timeout_id = 0;
  if (!self->shutting_down)
    shell_screen_shield_lock (self, FALSE);

  return G_SOURCE_REMOVE;
}

/* `_onStatusChanged ()` */
static void
on_status_changed (ShellScreenShield *self,
                   guint              status)
{
  gboolean should_lock;

  if (status != SHELL_PRESENCE_STATUS_IDLE)
    return;

  maybe_cancel_dialog (self);

  if (clutter_actor_is_visible (CLUTTER_ACTOR (self->long_lightbox)))
    {
      /* We're in the process of showing. */
      return;
    }

  become_modal (self);

  if (self->activation_time == 0)
    {
      self->activation_time = g_get_monotonic_time ();
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVATION_TIME]);
    }

  should_lock = g_settings_get_boolean (self->settings, LOCK_ENABLED_KEY) && !self->is_locked;

  if (should_lock)
    {
      guint lock_timeout = MAX (shell_adjust_animation_time (STANDARD_FADE_TIME, FALSE),
                                g_settings_get_uint (self->settings, LOCK_DELAY_KEY) * 1000);

      if (self->lock_timeout_id != 0)
        g_source_remove (self->lock_timeout_id);
      self->lock_timeout_id = g_timeout_add (lock_timeout, lock_timeout_cb, self);
      g_source_set_name_by_id (self->lock_timeout_id, "[gnome-shell] this.lock");
    }

  activate_fade (self, self->long_lightbox, STANDARD_FADE_TIME);
}

static void
presence_status_changed_cb (ShellSessionManagerPresence *presence,
                            guint                        status,
                            ShellScreenShield           *self)
{
  on_status_changed (self, status);
}

static void
presence_ready_cb (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  g_autoptr (ShellScreenShield) self = user_data;
  g_autoptr (GError) error = NULL;
  ShellSessionManagerPresence *presence;

  presence = shell_gnome_session_presence_new_finish (res, &error);
  if (presence == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Error while reading gnome-session presence: %s", error->message);
      return;
    }

  if (self->shutting_down)
    {
      g_object_unref (presence);
      return;
    }

  g_clear_object (&self->presence);
  self->presence = presence;

  g_signal_connect_object (presence, "status-changed",
                           G_CALLBACK (presence_status_changed_cb), self, 0);
  on_status_changed (self, shell_session_manager_presence_get_status (presence));
}

/* `_wakeUpScreen ()` */
static void
wake_up_screen (ShellScreenShield *self)
{
  if (!self->is_active)
    return; /* already woken up, or not yet asleep */

  on_user_became_active (self);
  g_signal_emit (self, signals[WAKE_UP_SCREEN], 0);
}

static void
dialog_wake_up_screen_cb (GObject           *dialog,
                          ShellScreenShield *self)
{
  wake_up_screen (self);
}

/* `_prepareForSleep ()` */
static void
login_manager_prepare_for_sleep_cb (ShellLoginManager *login_manager,
                                    gboolean           about_to_suspend,
                                    ShellScreenShield *self)
{
  if (about_to_suspend)
    {
      dialog_cancel (self);
      if (g_settings_get_boolean (self->settings, LOCK_ENABLED_KEY))
        shell_screen_shield_lock (self, TRUE);
    }
  else
    {
      wake_up_screen (self);
    }
}

/* ---- the pointer ---- */

/* `_showPointer ()` */
static void
show_pointer (ShellScreenShield *self)
{
  if (self->cursor_visible_inhibited)
    {
      ClutterContext *context = clutter_actor_get_context (CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ())));
      ClutterSeat *seat = clutter_backend_get_default_seat (clutter_context_get_backend (context));

      meta_cursor_tracker_uninhibit_cursor_visibility (self->cursor_tracker);
      clutter_seat_uninhibit_unfocus (seat);
      self->cursor_visible_inhibited = FALSE;
    }

  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->motion_listener), FALSE);
}

static void
motion_listener_motion_cb (ClutterMotionController *controller,
                           ShellScreenShield       *self)
{
  show_pointer (self);
}

/* `_hidePointer ()` */
static void
hide_pointer (ShellScreenShield *self)
{
  if (!self->cursor_visible_inhibited)
    {
      ClutterContext *context = clutter_actor_get_context (CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ())));
      ClutterSeat *seat = clutter_backend_get_default_seat (clutter_context_get_backend (context));

      clutter_seat_inhibit_unfocus (seat);
      meta_cursor_tracker_inhibit_cursor_visibility (self->cursor_tracker);
      self->cursor_visible_inhibited = TRUE;
    }
}

/* `_hidePointerUntilMotion ()` */
static void
hide_pointer_until_motion (ShellScreenShield *self)
{
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->motion_listener), TRUE);
  hide_pointer (self);
}

/* ---- the curtain ---- */

/* `_hideLockScreenComplete ()` */
static void
hide_lock_screen_complete (ShellScreenShield *self)
{
  self->lock_screen_state = SHELL_NOTIFICATION_STATE_HIDDEN;
  clutter_actor_hide (self->lock_screen_group);

  if (self->dialog != NULL)
    {
      clutter_actor_grab_key_focus (self->dialog);
      st_widget_navigate_focus (ST_WIDGET (self->dialog), NULL, ST_DIR_TAB_FORWARD, FALSE);
    }
}

static void
hide_lock_screen_done_cb (gboolean finished,
                          gpointer user_data)
{
  ShellScreenShield *self = SHELL_SCREEN_SHIELD (user_data);

  if (finished && !self->shutting_down)
    hide_lock_screen_complete (self);
}

static void
hide_lock_screen (ShellScreenShield *self,
                  gboolean           animate)
{
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());
  float height;
  float translation_y;
  float delta;
  float velocity;
  int duration;
  ShellEaseParams params;

  if (self->lock_screen_state == SHELL_NOTIFICATION_STATE_HIDDEN)
    return;

  self->lock_screen_state = SHELL_NOTIFICATION_STATE_HIDING;

  clutter_actor_remove_all_transitions (self->lock_dialog_group);

  /* Animate the lock screen out of screen
   * if velocity is not specified (i.e. we come here from pressing ESC),
   * use the same speed regardless of original position
   * if velocity is specified, it's in pixels per milliseconds */
  height = clutter_actor_get_height (CLUTTER_ACTOR (stage));
  clutter_actor_get_translation (self->lock_dialog_group, NULL, &translation_y, NULL);
  delta = height + translation_y;
  velocity = height / CURTAIN_SLIDE_TIME;
  duration = animate ? (int) (delta / velocity) : 0;

  params = ease_params (duration, -1, CLUTTER_EASE_OUT_QUAD);

  if (use_motion ())
    shell_actor_ease (self->lock_dialog_group, &params,
                      hide_lock_screen_done_cb, g_object_ref (self), g_object_unref,
                      "translation-y", (double) -height,
                      NULL);
  else
    shell_actor_ease (self->lock_dialog_group, &params,
                      hide_lock_screen_done_cb, g_object_ref (self), g_object_unref,
                      "opacity", 0u,
                      NULL);

  show_pointer (self);
}

/* `_lockScreenShown ()` */
static gboolean
take_a_beat_cb (gpointer user_data)
{
  ShellScreenShield *self = SHELL_SCREEN_SHIELD (user_data);

  self->activate_fade_id = 0;
  if (!self->shutting_down)
    activate_fade (self, self->short_lightbox, MANUAL_FADE_TIME);

  return G_SOURCE_REMOVE;
}

static void
lock_screen_shown (ShellScreenShield *self,
                   gboolean           fade_to_black,
                   gboolean           animate_fade)
{
  hide_pointer_until_motion (self);

  self->lock_screen_state = SHELL_NOTIFICATION_STATE_SHOWN;

  if (fade_to_black && animate_fade)
    {
      /* Take a beat */
      if (self->activate_fade_id != 0)
        g_source_remove (self->activate_fade_id);
      self->activate_fade_id = g_timeout_add (MANUAL_FADE_TIME, take_a_beat_cb, self);
      g_source_set_name_by_id (self->activate_fade_id, "[gnome-shell] this._activateFade");
    }
  else
    {
      if (fade_to_black)
        activate_fade (self, self->short_lightbox, 0);

      set_active (self, TRUE);
    }

  g_signal_emit (self, signals[LOCK_SCREEN_SHOWN], 0);
}

typedef struct
{
  ShellScreenShield *shield;     /* strong */
  gboolean fade_to_black;
} LockScreenShownData;

static void
lock_screen_shown_data_free (LockScreenShownData *data)
{
  g_clear_object (&data->shield);
  g_free (data);
}

static void
reset_lock_screen_done_cb (gboolean finished,
                           gpointer user_data)
{
  LockScreenShownData *data = user_data;

  if (finished && !data->shield->shutting_down)
    lock_screen_shown (data->shield, data->fade_to_black, TRUE);
}

/* `_resetLockScreen ()` */
static void
reset_lock_screen (ShellScreenShield *self,
                   gboolean           animate_lock_screen,
                   gboolean           fade_to_black)
{
  /* Don't reset the lock screen unless it is completely hidden
   * This prevents the shield going down if the lock-delay timeout
   * fires while the user is dragging (which has the potential
   * to confuse our state) */
  if (self->lock_screen_state != SHELL_NOTIFICATION_STATE_HIDDEN)
    return;

  clutter_actor_show (self->lock_screen_group);
  self->lock_screen_state = SHELL_NOTIFICATION_STATE_SHOWING;

  if (animate_lock_screen)
    {
      ShellEaseParams params = ease_params (SHELL_OVERVIEW_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);
      LockScreenShownData *data = g_new0 (LockScreenShownData, 1);

      data->shield = g_object_ref (self);
      data->fade_to_black = fade_to_black;

      if (use_motion ())
        clutter_actor_set_translation (self->lock_dialog_group, 0,
                                       -shell_global_get_screen_height (shell_global_get ()), 0);
      else
        clutter_actor_set_opacity (self->lock_dialog_group, 0);

      clutter_actor_remove_all_transitions (self->lock_dialog_group);
      if (use_motion ())
        shell_actor_ease (self->lock_dialog_group, &params,
                          reset_lock_screen_done_cb, data,
                          (GDestroyNotify) lock_screen_shown_data_free,
                          "translation-y", 0.0,
                          NULL);
      else
        shell_actor_ease (self->lock_dialog_group, &params,
                          reset_lock_screen_done_cb, data,
                          (GDestroyNotify) lock_screen_shown_data_free,
                          "opacity", 255u,
                          NULL);
    }
  else
    {
      clutter_actor_set_translation (self->lock_dialog_group, 0, 0, 0);
      clutter_actor_set_opacity (self->lock_dialog_group, 255);
      lock_screen_shown (self, fade_to_black, FALSE);
    }

  dialog_grab_or_activate (self);
}

/* `_onUnlockFailed ()` */
static void
dialog_failed_cb (GObject           *dialog,
                  ShellScreenShield *self)
{
  reset_lock_screen (self, TRUE, FALSE);
}

static void
dialog_destroyed_cb (ClutterActor      *dialog,
                     ShellScreenShield *self)
{
  /* The shield destroys the dialog itself; this is for the others */
  if (self->dialog == dialog)
    {
      g_clear_weak_pointer (&self->dialog);
      self->dialog_kind = DIALOG_KIND_NONE;
    }
}

/* `_ensureUnlockDialog ()` */
static gboolean
ensure_unlock_dialog (ShellScreenShield *self,
                      gboolean           allow_cancel)
{
  if (self->dialog == NULL)
    {
      ShellSessionMode *session_mode = get_session_mode ();
      g_autofree char *unlock_dialog = NULL;
      ClutterActor *dialog = NULL;
      DialogKind kind = DIALOG_KIND_NONE;
      gboolean opened = FALSE;

      if (session_mode != NULL)
        g_object_get (session_mode, "unlock-dialog", &unlock_dialog, NULL);

      if (g_strcmp0 (unlock_dialog, "login-dialog") == 0)
        {
          dialog = CLUTTER_ACTOR (shell_login_dialog_new (self->lock_dialog_group));
          kind = DIALOG_KIND_LOGIN;
        }
      else if (g_strcmp0 (unlock_dialog, "unlock-dialog") == 0)
        {
          dialog = CLUTTER_ACTOR (shell_unlock_dialog_new (self->lock_dialog_group,
                                                           self->time_limits_manager));
          kind = DIALOG_KIND_UNLOCK;
        }

      if (dialog == NULL)
        {
          /* This session mode has no locking capabilities */
          shell_screen_shield_deactivate (self, TRUE);
          return FALSE;
        }

      g_set_weak_pointer (&self->dialog, dialog);
      self->dialog_kind = kind;

      if (kind == DIALOG_KIND_LOGIN)
        opened = shell_login_dialog_open (SHELL_LOGIN_DIALOG (dialog));
      else
        opened = shell_unlock_dialog_open (SHELL_UNLOCK_DIALOG (dialog));

      if (!opened)
        {
          /* This is kind of an impossible error: we're already modal
           * by the time we reach this... */
          g_message ("Could not open login dialog: failed to acquire grab");
          shell_screen_shield_deactivate (self, TRUE);
          return FALSE;
        }

      self->dialog_failed_id = g_signal_connect (dialog, "failed",
                                                 G_CALLBACK (dialog_failed_cb), self);
      self->dialog_wake_up_screen_id = g_signal_connect (dialog, "wake-up-screen",
                                                         G_CALLBACK (dialog_wake_up_screen_cb), self);
      g_signal_connect (dialog, "destroy", G_CALLBACK (dialog_destroyed_cb), self);
    }

  if (self->dialog_kind == DIALOG_KIND_UNLOCK)
    shell_unlock_dialog_set_allow_cancel (SHELL_UNLOCK_DIALOG (self->dialog), allow_cancel);

  dialog_grab_or_activate (self);
  return TRUE;
}

/* ---- the smartcard and the credentials ---- */

static void
smartcard_inserted_cb (ShellSmartcardManager *manager,
                       GDBusProxy            *token,
                       ShellScreenShield     *self)
{
  g_autoptr (GVariant) used_to_login = g_dbus_proxy_get_cached_property (token, "UsedToLogin");

  if (self->is_locked && used_to_login != NULL &&
      g_variant_is_of_type (used_to_login, G_VARIANT_TYPE_BOOLEAN) &&
      g_variant_get_boolean (used_to_login))
    activate_dialog (self);
}

static void
credential_manager_user_authenticated_cb (ShellCredentialManager *manager,
                                          const char             *token,
                                          ShellScreenShield      *self)
{
  if (self->is_locked)
    activate_dialog (self);
}

/* ---- org.gnome.ScreenSaver ---- */

static void
screen_saver_lock_requested_cb (ShellScreenSaverService *service,
                                ShellScreenShield       *self)
{
  shell_screen_shield_lock (self, TRUE);
}

static void
screen_saver_set_active_requested_cb (ShellScreenSaverService *service,
                                      gboolean                 active,
                                      ShellScreenShield       *self)
{
  if (active)
    shell_screen_shield_activate (self, TRUE);
  else
    shell_screen_shield_deactivate (self, FALSE);
}

static gboolean
screen_saver_query_active_cb (ShellScreenSaverService *service,
                              ShellScreenShield       *self)
{
  return self->is_active;
}

static gint64
screen_saver_query_activation_time_cb (ShellScreenSaverService *service,
                                       ShellScreenShield       *self)
{
  return self->activation_time;
}

static void
active_changed_cb (ShellScreenShield *self)
{
  shell_screen_saver_service_emit_active_changed (self->screen_saver_dbus, self->is_active);
}

static void
wake_up_screen_signal_cb (ShellScreenShield *self)
{
  shell_screen_saver_service_emit_wake_up_screen (self->screen_saver_dbus);
}

static void
lock_screen_shown_signal_cb (ShellScreenShield *self)
{
  shell_screen_saver_service_lock_screen_shown (self->screen_saver_dbus);
}

static void
global_shutdown_cb (ShellGlobal       *global,
                    ShellScreenShield *self)
{
  self->shutting_down = TRUE;

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  if (self->inhibit_cancellable != NULL)
    g_cancellable_cancel (self->inhibit_cancellable);
}

/* ------------------------------------------------------------------------ */
/* Object                                                                    */
/* ------------------------------------------------------------------------ */

static void
shell_screen_shield_constructed (GObject *object)
{
  ShellScreenShield *self = SHELL_SCREEN_SHIELD (object);
  ShellMain *main_obj = get_main ();
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (main_obj);
  ShellGlobal *global = shell_global_get ();
  MetaBackend *backend = shell_global_get_backend (global);
  ClutterActor *ui_group = get_ui_group ();

  G_OBJECT_CLASS (shell_screen_shield_parent_class)->constructed (object);

  self->cancellable = g_cancellable_new ();
  g_set_weak_pointer (&self->global, global);
  self->shutdown_id = g_signal_connect (global, "shutdown",
                                        G_CALLBACK (global_shutdown_cb), self);

  g_return_if_fail (layout_manager != NULL);

  g_set_weak_pointer (&self->actor,
                      CLUTTER_ACTOR (shell_layout_manager_get_screen_shield_group (layout_manager)));

  self->lock_screen_state = SHELL_NOTIFICATION_STATE_HIDDEN;

  self->lock_screen_group = g_object_new (ST_TYPE_WIDGET,
                                          "x-expand", TRUE,
                                          "y-expand", TRUE,
                                          "reactive", TRUE,
                                          "can-focus", TRUE,
                                          "name", "lockScreenGroup",
                                          "visible", FALSE,
                                          NULL);

  self->lock_dialog_group = g_object_new (ST_TYPE_WIDGET,
                                          "x-expand", TRUE,
                                          "y-expand", TRUE,
                                          "reactive", TRUE,
                                          "can-focus", TRUE,
                                          "name", "lockDialogGroup",
                                          NULL);
  clutter_actor_set_pivot_point (self->lock_dialog_group, 0.5f, 0.5f);

  clutter_actor_add_child (self->actor, self->lock_screen_group);
  clutter_actor_add_child (self->actor, self->lock_dialog_group);

  g_object_add_weak_pointer (G_OBJECT (self->lock_screen_group), (gpointer *) &self->lock_screen_group);
  g_object_add_weak_pointer (G_OBJECT (self->lock_dialog_group), (gpointer *) &self->lock_dialog_group);

  shell_gnome_session_presence_new (self->cancellable, presence_ready_cb, g_object_ref (self));

  /* org.gnome.ScreenSaver lives in C (src/ui/shell-dbus.c) */
  self->screen_saver_dbus = shell_screen_saver_service_new ();
  g_signal_connect (self->screen_saver_dbus, "lock-requested",
                    G_CALLBACK (screen_saver_lock_requested_cb), self);
  g_signal_connect (self->screen_saver_dbus, "set-active-requested",
                    G_CALLBACK (screen_saver_set_active_requested_cb), self);
  g_signal_connect (self->screen_saver_dbus, "query-active",
                    G_CALLBACK (screen_saver_query_active_cb), self);
  g_signal_connect (self->screen_saver_dbus, "query-activation-time",
                    G_CALLBACK (screen_saver_query_activation_time_cb), self);
  g_signal_connect (self, "active-changed", G_CALLBACK (active_changed_cb), NULL);
  g_signal_connect (self, "wake-up-screen", G_CALLBACK (wake_up_screen_signal_cb), NULL);
  g_signal_connect (self, "lock-screen-shown", G_CALLBACK (lock_screen_shown_signal_cb), NULL);

  self->smartcard_manager = g_object_ref (shell_smartcard_manager_get_default ());
  g_signal_connect (self->smartcard_manager, "smartcard-inserted",
                    G_CALLBACK (smartcard_inserted_cb), self);

  self->credential_managers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  shell_screen_shield_add_credential_manager (self, SHELL_OVIRT_SERVICE_NAME,
                                              SHELL_CREDENTIAL_MANAGER (shell_ovirt_credential_manager_get_default ()));

  self->login_manager = g_object_ref (shell_login_manager_get_default ());
  g_signal_connect (self->login_manager, "prepare-for-sleep",
                    G_CALLBACK (login_manager_prepare_for_sleep_cb), self);

  shell_login_manager_get_current_session_async (self->login_manager, self->cancellable,
                                                 login_session_ready_cb, g_object_ref (self));

  self->settings = g_settings_new (SCREENSAVER_SCHEMA);
  g_signal_connect (self->settings, "changed::" LOCK_ENABLED_KEY,
                    G_CALLBACK (settings_sync_inhibitor_cb), self);

  self->lock_settings = g_settings_new (LOCKDOWN_SCHEMA);
  g_signal_connect (self->lock_settings, "changed::" DISABLE_LOCK_KEY,
                    G_CALLBACK (settings_sync_inhibitor_cb), self);

  /* The "long" lightbox is used for the longer (20 seconds) fade from session
   * to idle status, the "short" is used for quickly fading to black when locking
   * manually */
  self->long_lightbox = g_object_new (SHELL_TYPE_LIGHTBOX,
                                      "container", ui_group,
                                      "inhibit-events", TRUE,
                                      "fade-factor", 1.0,
                                      NULL);
  g_signal_connect (self->long_lightbox, "notify::active",
                    G_CALLBACK (long_lightbox_active_cb), self);
  self->short_lightbox = g_object_new (SHELL_TYPE_LIGHTBOX,
                                       "container", ui_group,
                                       "inhibit-events", TRUE,
                                       "fade-factor", 1.0,
                                       NULL);
  g_signal_connect (self->short_lightbox, "notify::active",
                    G_CALLBACK (short_lightbox_active_cb), self);
  g_object_add_weak_pointer (G_OBJECT (self->long_lightbox), (gpointer *) &self->long_lightbox);
  g_object_add_weak_pointer (G_OBJECT (self->short_lightbox), (gpointer *) &self->short_lightbox);

  g_set_weak_pointer (&self->idle_monitor, meta_backend_get_core_idle_monitor (backend));
  g_set_weak_pointer (&self->cursor_tracker, meta_backend_get_cursor_tracker (backend));

  self->motion_listener = clutter_motion_controller_new ();
  g_object_ref_sink (self->motion_listener);
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->motion_listener), FALSE);
  g_signal_connect (self->motion_listener, "motion",
                    G_CALLBACK (motion_listener_motion_cb), self);
  clutter_actor_add_action (self->lock_dialog_group, self->motion_listener);

  sync_inhibitor (self);
}

static void
shell_screen_shield_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellScreenShield *self = SHELL_SCREEN_SHIELD (object);

  switch (prop_id)
    {
    case PROP_LOCKED:
      g_value_set_boolean (value, self->is_locked);
      break;

    case PROP_ACTIVE:
      g_value_set_boolean (value, self->is_active);
      break;

    case PROP_ACTIVATION_TIME:
      g_value_set_int64 (value, self->activation_time);
      break;

    case PROP_TIME_LIMITS_MANAGER:
      g_value_set_object (value, self->time_limits_manager);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_screen_shield_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  ShellScreenShield *self = SHELL_SCREEN_SHIELD (object);

  switch (prop_id)
    {
    case PROP_TIME_LIMITS_MANAGER:
      shell_screen_shield_set_time_limits_manager (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_screen_shield_dispose (GObject *object)
{
  ShellScreenShield *self = SHELL_SCREEN_SHIELD (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  if (self->inhibit_cancellable != NULL)
    g_cancellable_cancel (self->inhibit_cancellable);

  g_clear_handle_id (&self->lock_timeout_id, g_source_remove);
  g_clear_handle_id (&self->activate_fade_id, g_source_remove);

  if (self->became_active_id != 0 && self->idle_monitor != NULL)
    meta_idle_monitor_remove_watch (self->idle_monitor, self->became_active_id);
  self->became_active_id = 0;
  g_clear_weak_pointer (&self->idle_monitor);
  g_clear_weak_pointer (&self->cursor_tracker);

  if (self->shutdown_id != 0 && self->global != NULL)
    g_signal_handler_disconnect (self->global, self->shutdown_id);
  self->shutdown_id = 0;
  g_clear_weak_pointer (&self->global);

  if (self->motion_listener != NULL)
    {
      ClutterActor *attached = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->motion_listener));

      g_signal_handlers_disconnect_by_data (self->motion_listener, self);
      if (attached != NULL)
        clutter_actor_remove_action (attached, self->motion_listener);
    }
  g_clear_object (&self->motion_listener);

  if (self->smartcard_manager != NULL)
    g_signal_handlers_disconnect_by_data (self->smartcard_manager, self);
  g_clear_object (&self->smartcard_manager);

  if (self->credential_managers != NULL)
    {
      GHashTableIter iter;
      gpointer value;

      g_hash_table_iter_init (&iter, self->credential_managers);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        g_signal_handlers_disconnect_by_data (value, self);
      g_hash_table_remove_all (self->credential_managers);
    }

  if (self->login_manager != NULL)
    g_signal_handlers_disconnect_by_data (self->login_manager, self);
  g_clear_object (&self->login_manager);
  g_clear_object (&self->login_session);

  if (self->presence != NULL)
    g_signal_handlers_disconnect_by_data (self->presence, self);
  g_clear_object (&self->presence);

  if (self->screen_saver_dbus != NULL)
    g_signal_handlers_disconnect_by_data (self->screen_saver_dbus, self);
  g_clear_object (&self->screen_saver_dbus);

  if (self->settings != NULL)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  g_clear_object (&self->settings);
  if (self->lock_settings != NULL)
    g_signal_handlers_disconnect_by_data (self->lock_settings, self);
  g_clear_object (&self->lock_settings);

  if (self->inhibitor != NULL)
    g_input_stream_close (self->inhibitor, NULL, NULL);
  g_clear_object (&self->inhibitor);

  g_clear_object (&self->grab);
  g_clear_object (&self->time_limits_manager);

  g_clear_weak_pointer (&self->dialog);
  g_clear_weak_pointer (&self->actor);

  G_OBJECT_CLASS (shell_screen_shield_parent_class)->dispose (object);
}

static void
shell_screen_shield_finalize (GObject *object)
{
  ShellScreenShield *self = SHELL_SCREEN_SHIELD (object);

  g_clear_pointer (&self->credential_managers, g_hash_table_unref);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->inhibit_cancellable);

  G_OBJECT_CLASS (shell_screen_shield_parent_class)->finalize (object);
}

static void
shell_screen_shield_class_init (ShellScreenShieldClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_screen_shield_constructed;
  object_class->get_property = shell_screen_shield_get_property;
  object_class->set_property = shell_screen_shield_set_property;
  object_class->dispose = shell_screen_shield_dispose;
  object_class->finalize = shell_screen_shield_finalize;

  /**
   * ShellScreenShield:locked:
   *
   * Whether the session is locked (`locked`).
   */
  props[PROP_LOCKED] =
    g_param_spec_boolean ("locked", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellScreenShield:active:
   *
   * Whether the shield is up (`active`).
   */
  props[PROP_ACTIVE] =
    g_param_spec_boolean ("active", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellScreenShield:activation-time:
   *
   * The monotonic time (see g_get_monotonic_time ()) the shield was
   * activated at, 0 if it is not (`activationTime`). It does not notify.
   */
  props[PROP_ACTIVATION_TIME] =
    g_param_spec_int64 ("activation-time", NULL, NULL,
                        0, G_MAXINT64, 0,
                        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellScreenShield:time-limits-manager:
   *
   * The #ShellTimeLimitsManager (`Main.timeLimitsManager`) whose
   * :should-lock-session blocks the unlocking. ShellMain sets it once the
   * manager exists.
   */
  props[PROP_TIME_LIMITS_MANAGER] =
    g_param_spec_object ("time-limits-manager", NULL, NULL,
                         SHELL_TYPE_TIME_LIMITS_MANAGER,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellScreenShield::active-changed:
   * @self: the #ShellScreenShield
   *
   * Emitted when :active changed.
   */
  signals[ACTIVE_CHANGED] =
    g_signal_new ("active-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellScreenShield::locked-changed:
   * @self: the #ShellScreenShield
   *
   * Emitted when :locked changed.
   */
  signals[LOCKED_CHANGED] =
    g_signal_new ("locked-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellScreenShield::wake-up-screen:
   * @self: the #ShellScreenShield
   *
   * Emitted when the screen should be woken up, which org.gnome.ScreenSaver
   * relays.
   */
  signals[WAKE_UP_SCREEN] =
    g_signal_new ("wake-up-screen", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);

  /**
   * ShellScreenShield::lock-screen-shown:
   * @self: the #ShellScreenShield
   *
   * Emitted when the lock screen is shown, which answers the calls of Lock ()
   * of org.gnome.ScreenSaver.
   */
  signals[LOCK_SCREEN_SHOWN] =
    g_signal_new ("lock-screen-shown", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
shell_screen_shield_init (ShellScreenShield *self)
{
  self->dialog_kind = DIALOG_KIND_NONE;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

/**
 * shell_screen_shield_new:
 *
 * Creates the lock screen (`new ScreenShield ()`). The layout manager and the
 * session mode have to be set in #ShellMain already.
 *
 * Returns: (transfer full): a new #ShellScreenShield
 */
ShellScreenShield *
shell_screen_shield_new (void)
{
  return g_object_new (SHELL_TYPE_SCREEN_SHIELD, NULL);
}

/**
 * shell_screen_shield_get_locked:
 * @self: a #ShellScreenShield
 *
 * Returns: whether the session is locked (`locked`)
 */
gboolean
shell_screen_shield_get_locked (ShellScreenShield *self)
{
  g_return_val_if_fail (SHELL_IS_SCREEN_SHIELD (self), FALSE);

  return self->is_locked;
}

/**
 * shell_screen_shield_get_active:
 * @self: a #ShellScreenShield
 *
 * Returns: whether the shield is up (`active`)
 */
gboolean
shell_screen_shield_get_active (ShellScreenShield *self)
{
  g_return_val_if_fail (SHELL_IS_SCREEN_SHIELD (self), FALSE);

  return self->is_active;
}

/**
 * shell_screen_shield_get_activation_time:
 * @self: a #ShellScreenShield
 *
 * Returns: the monotonic time the shield was activated at, 0 if it is not
 *   (`activationTime`)
 */
gint64
shell_screen_shield_get_activation_time (ShellScreenShield *self)
{
  g_return_val_if_fail (SHELL_IS_SCREEN_SHIELD (self), 0);

  return self->activation_time;
}

/**
 * shell_screen_shield_get_time_limits_manager:
 * @self: a #ShellScreenShield
 *
 * Returns: (transfer none) (nullable): the :time-limits-manager
 */
ShellTimeLimitsManager *
shell_screen_shield_get_time_limits_manager (ShellScreenShield *self)
{
  g_return_val_if_fail (SHELL_IS_SCREEN_SHIELD (self), NULL);

  return self->time_limits_manager;
}

/**
 * shell_screen_shield_set_time_limits_manager:
 * @self: a #ShellScreenShield
 * @time_limits_manager: (nullable): the manager of the time limits
 *
 * Sets :time-limits-manager.
 */
void
shell_screen_shield_set_time_limits_manager (ShellScreenShield      *self,
                                             ShellTimeLimitsManager *time_limits_manager)
{
  g_return_if_fail (SHELL_IS_SCREEN_SHIELD (self));
  g_return_if_fail (time_limits_manager == NULL ||
                    SHELL_IS_TIME_LIMITS_MANAGER (time_limits_manager));

  if (self->time_limits_manager == time_limits_manager)
    return;

  g_set_object (&self->time_limits_manager, time_limits_manager);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TIME_LIMITS_MANAGER]);
}

/**
 * shell_screen_shield_show_dialog:
 * @self: a #ShellScreenShield
 *
 * Shows the dialog without locking the session: the login screen of the gdm
 * session mode (`showDialog ()`).
 */
void
shell_screen_shield_show_dialog (ShellScreenShield *self)
{
  g_return_if_fail (SHELL_IS_SCREEN_SHIELD (self));

  become_modal (self);
  clutter_actor_show (self->actor);
  self->is_greeter = shell_session_mode_is_greeter (get_session_mode ());
  self->is_locked = TRUE;
  ensure_unlock_dialog (self, TRUE);
}

static gboolean
lock_if_was_locked_later_cb (gpointer user_data)
{
  GWeakRef *ref = user_data;
  g_autoptr (ShellScreenShield) self = g_weak_ref_get (ref);

  if (self != NULL && !self->shutting_down)
    shell_screen_shield_lock (self, FALSE);

  return G_SOURCE_REMOVE;
}

static void
weak_ref_free (gpointer data)
{
  GWeakRef *ref = data;

  g_weak_ref_clear (ref);
  g_free (ref);
}

/**
 * shell_screen_shield_lock_if_was_locked:
 * @self: a #ShellScreenShield
 *
 * If the previous shell crashed, and gnome-session restarted us, locks the
 * session again (`lockIfWasLocked ()`).
 */
void
shell_screen_shield_lock_if_was_locked (ShellScreenShield *self)
{
  ShellGlobal *global = shell_global_get ();
  g_autoptr (GVariant) was_locked = NULL;
  GWeakRef *ref;

  g_return_if_fail (SHELL_IS_SCREEN_SHIELD (self));

  if (!g_settings_get_boolean (self->settings, LOCK_ENABLED_KEY))
    return;

  was_locked = shell_global_get_runtime_state (global, "b", LOCKED_STATE_STR);
  if (was_locked == NULL)
    return;

  ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, self);
  meta_laters_add (meta_compositor_get_laters (shell_global_get_compositor (global)),
                   META_LATER_BEFORE_REDRAW, lock_if_was_locked_later_cb, ref, weak_ref_free);
}

/**
 * shell_screen_shield_add_credential_manager:
 * @self: a #ShellScreenShield
 * @service_name: the PAM service the credentials are for
 * @credential_manager: the manager which tells that the user authenticated
 *   without the dialog
 *
 * Activates the dialog of the locked session when @credential_manager
 * emits #ShellCredentialManager::user-authenticated
 * (`addCredentialManager ()`). Does nothing if there is one for @service_name.
 */
void
shell_screen_shield_add_credential_manager (ShellScreenShield      *self,
                                            const char             *service_name,
                                            ShellCredentialManager *credential_manager)
{
  g_return_if_fail (SHELL_IS_SCREEN_SHIELD (self));
  g_return_if_fail (service_name != NULL);
  g_return_if_fail (SHELL_IS_CREDENTIAL_MANAGER (credential_manager));

  if (g_hash_table_contains (self->credential_managers, service_name))
    return;

  g_hash_table_insert (self->credential_managers, g_strdup (service_name),
                       g_object_ref (credential_manager));
  g_signal_connect (credential_manager, "user-authenticated",
                    G_CALLBACK (credential_manager_user_authenticated_cb), self);
}

/**
 * shell_screen_shield_remove_credential_manager:
 * @self: a #ShellScreenShield
 * @service_name: the PAM service of the manager to forget
 *
 * Undoes shell_screen_shield_add_credential_manager () (`removeCredentialManager ()`).
 */
void
shell_screen_shield_remove_credential_manager (ShellScreenShield *self,
                                               const char        *service_name)
{
  gpointer credential_manager;

  g_return_if_fail (SHELL_IS_SCREEN_SHIELD (self));
  g_return_if_fail (service_name != NULL);

  credential_manager = g_hash_table_lookup (self->credential_managers, service_name);
  if (credential_manager == NULL)
    return;

  g_signal_handlers_disconnect_by_data (credential_manager, self);
  g_hash_table_remove (self->credential_managers, service_name);
}

/* ---- deactivate ---- */

/* `_completeDeactivate ()` */
static void
complete_deactivate (ShellScreenShield *self)
{
  if (self->dialog != NULL)
    {
      ClutterActor *dialog = self->dialog;

      g_signal_handlers_disconnect_by_data (dialog, self);
      g_clear_weak_pointer (&self->dialog);
      self->dialog_kind = DIALOG_KIND_NONE;
      clutter_actor_destroy (dialog);
    }

  clutter_actor_hide (self->actor);

  if (self->became_active_id != 0)
    {
      meta_idle_monitor_remove_watch (self->idle_monitor, self->became_active_id);
      self->became_active_id = 0;
    }

  g_clear_handle_id (&self->lock_timeout_id, g_source_remove);

  if (self->activation_time != 0)
    {
      self->activation_time = 0;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVATION_TIME]);
    }
  set_active (self, FALSE);
  set_locked (self, FALSE);
  shell_global_set_runtime_state (shell_global_get (), LOCKED_STATE_STR, NULL);
}

static void
complete_deactivate_done_cb (gboolean finished,
                             gpointer user_data)
{
  ShellScreenShield *self = SHELL_SCREEN_SHIELD (user_data);

  if (finished && !self->shutting_down)
    complete_deactivate (self);
}

/* `_continueDeactivate ()` */
static void
continue_deactivate (ShellScreenShield *self,
                     gboolean           animate)
{
  ShellSessionMode *session_mode = get_session_mode ();
  ShellEaseParams params = ease_params (SHELL_OVERVIEW_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);

  hide_lock_screen (self, animate);

  if (g_strcmp0 (shell_session_mode_get_current_mode (session_mode), "unlock-dialog") == 0)
    {
      g_autoptr (GError) error = NULL;

      if (!shell_session_mode_pop_mode (session_mode, "unlock-dialog", &error))
        g_message ("Could not leave the unlock-dialog mode: %s", error->message);
    }

  g_signal_emit (self, signals[WAKE_UP_SCREEN], 0);

  if (self->is_greeter)
    {
      /* We don't want to "deactivate" any more than
       * this. In particular, we don't want to drop
       * the modal, hide ourselves or destroy the dialog
       * But we do want to set isActive to false, so that
       * gnome-session will reset the idle counter, and
       * gnome-settings-daemon will stop blanking the screen */
      if (self->activation_time != 0)
        {
          self->activation_time = 0;
          g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVATION_TIME]);
        }
      set_active (self, FALSE);
      return;
    }

  dialog_pop_modal (self);

  drop_modal (self);

  shell_lightbox_light_off (self->long_lightbox, 0);
  shell_lightbox_light_off (self->short_lightbox, 0);

  if (use_motion ())
    shell_actor_ease (self->lock_dialog_group, &params,
                      complete_deactivate_done_cb, g_object_ref (self), g_object_unref,
                      "translation-y", (double) -shell_global_get_screen_height (shell_global_get ()),
                      NULL);
  else
    shell_actor_ease (self->lock_dialog_group, &params,
                      complete_deactivate_done_cb, g_object_ref (self), g_object_unref,
                      "opacity", 0u,
                      NULL);
}

typedef struct
{
  ShellScreenShield *shield;     /* strong */
  gboolean animate;
} DeactivateData;

static void
deactivate_data_free (DeactivateData *data)
{
  g_clear_object (&data->shield);
  g_free (data);
}

static void
deactivate_finish_cb (gpointer user_data)
{
  DeactivateData *data = user_data;

  continue_deactivate (data->shield, data->animate);
}

/**
 * shell_screen_shield_deactivate:
 * @self: a #ShellScreenShield
 * @animate: whether the curtain slides up
 *
 * Unlocks the session and takes the shield down (`deactivate ()`). If the
 * time limits lock the session, it locks it instead.
 */
void
shell_screen_shield_deactivate (ShellScreenShield *self,
                                gboolean           animate)
{
  g_return_if_fail (SHELL_IS_SCREEN_SHIELD (self));

  /* block unlock via logind etc. if locked due to parental controls */
  if (should_lock_session (self))
    {
      shell_screen_shield_lock (self, FALSE);
      return;
    }

  if (self->dialog != NULL)
    {
      DeactivateData *data = g_new0 (DeactivateData, 1);

      data->shield = g_object_ref (self);
      data->animate = animate;
      dialog_finish (self, deactivate_finish_cb, data, (GDestroyNotify) deactivate_data_free);
    }
  else
    {
      continue_deactivate (self, animate);
    }
}

/**
 * shell_screen_shield_activate:
 * @self: a #ShellScreenShield
 * @animate: whether the curtain slides down
 *
 * Puts the shield up, without locking the session (`activate ()`).
 */
void
shell_screen_shield_activate (ShellScreenShield *self,
                              gboolean           animate)
{
  ShellSessionMode *session_mode = get_session_mode ();

  g_return_if_fail (SHELL_IS_SCREEN_SHIELD (self));

  if (self->activation_time == 0)
    {
      self->activation_time = g_get_monotonic_time ();
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVATION_TIME]);
    }

  if (!ensure_unlock_dialog (self, TRUE))
    return;

  clutter_actor_show (self->actor);

  if (g_strcmp0 (shell_session_mode_get_current_mode (session_mode), "unlock-dialog") != 0)
    {
      self->is_greeter = shell_session_mode_is_greeter (session_mode);
      if (!self->is_greeter)
        {
          g_autoptr (GError) error = NULL;

          if (!shell_session_mode_push_mode (session_mode, "unlock-dialog", &error))
            g_message ("Could not enter the unlock-dialog mode: %s", error->message);
        }
    }

  reset_lock_screen (self, animate, TRUE);
  /* We used to set isActive and emit active-changed here,
   * but now we do that from lockScreenShown, which means
   * there is a 0.3 seconds window during which the lock
   * screen is effectively visible and the screen is locked, but
   * the DBus interface reports the screensaver is off.
   * This is because when we emit ActiveChanged(true),
   * gnome-settings-daemon blanks the screen, and we don't want
   * blank during the animation.
   * This is not a problem for the idle fade case, because we
   * activate without animation in that case. */
}

/**
 * shell_screen_shield_lock:
 * @self: a #ShellScreenShield
 * @animate: whether the curtain slides down
 *
 * Locks the session (`lock ()`).
 */
void
shell_screen_shield_lock (ShellScreenShield *self,
                          gboolean           animate)
{
  StClipboard *clipboard;
  ActUserManager *user_manager;
  ActUser *user;
  gboolean lock;

  g_return_if_fail (SHELL_IS_SCREEN_SHIELD (self));

  if (g_settings_get_boolean (self->lock_settings, DISABLE_LOCK_KEY))
    {
      g_message ("Screen lock is locked down, not locking");
      return;
    }

  become_modal (self);

  /* Clear the clipboard - otherwise, its contents may be leaked
   * to unauthorized parties by pasting into the unlock dialog's
   * password entry and unmasking the entry */
  clipboard = st_clipboard_get_default ();
  st_clipboard_set_text (clipboard, ST_CLIPBOARD_TYPE_CLIPBOARD, "");
  st_clipboard_set_text (clipboard, ST_CLIPBOARD_TYPE_PRIMARY, "");

  user_manager = act_user_manager_get_default ();
  user = act_user_manager_get_user (user_manager, g_get_user_name ());

  shell_screen_shield_activate (self, animate);

  lock = self->is_greeter ? TRUE : act_user_get_password_mode (user) != ACT_USER_PASSWORD_MODE_NONE;
  set_locked (self, lock);
}
