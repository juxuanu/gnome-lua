/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * system-actions.c: The system actions (power off, restart, suspend, lock
 * screen, log out, switch user, lock orientation, screenshot UI) and whether
 * each of them is currently available. Port of js/misc/systemActions.js.
 *
 * What the JS reached through the `Main` module is done like this:
 *
 *  - `Main.sessionMode` is the #ShellSystemActions:session-mode property,
 *    which js/ui/main.js sets right after it creates the session mode. While
 *    it is unset the session counts as neither locked nor a greeter.
 *  - `Main.screenShield.lock (animate)` is the #ShellSystemActions::lock-screen-requested
 *    signal.
 *  - `Main.overview.hide ()` (when logging out) is the
 *    #ShellSystemActions::hide-overview-requested signal.
 *  - `Main.overview` 'hidden' followed by `Screenshot.showScreenshotUI ()` is
 *    the #ShellSystemActions::screenshot-ui-requested signal; main.js waits
 *    for the overview to be hidden before it opens the UI.
 *  - `Main.layoutManager`'s 'monitors-changed' is the signal of the same name
 *    of the #MetaMonitorManager, which is what the layout manager forwards.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <act/act.h>
#include <gdm/gdm-sessions.h>
#include <gdm/gdm-user-switching.h>
#include <glib/gi18n.h>
#include <meta/meta-backend.h>
#include <meta/meta-monitor-manager.h>

#include "misc/gnome-session.h"
#include "misc/login-manager.h"
#include "misc/system-actions.h"
#include "shell-global.h"

#define LOCKDOWN_SCHEMA "org.gnome.desktop.lockdown"
#define LOGIN_SCREEN_SCHEMA "org.gnome.login-screen"
#define SCREENSAVER_SCHEMA "org.gnome.desktop.screensaver"
#define ORIENTATION_SCHEMA "org.gnome.settings-daemon.peripherals.touchscreen"
#define DISABLE_USER_SWITCH_KEY "disable-user-switching"
#define DISABLE_LOCK_SCREEN_KEY "disable-lock-screen"
#define DISABLE_LOG_OUT_KEY "disable-log-out"
#define DISABLE_RESTART_KEY "disable-restart-buttons"
#define RESTART_ENABLED_KEY "restart-enabled"
#define ALWAYS_SHOW_LOG_OUT_KEY "always-show-log-out"
#define ORIENTATION_LOCK_KEY "orientation-lock"

/* The order of the JS `_actions` map, which is the order of the search results */
typedef enum
{
  ACTION_POWER_OFF,
  ACTION_RESTART,
  ACTION_LOCK_SCREEN,
  ACTION_LOGOUT,
  ACTION_SUSPEND,
  ACTION_SWITCH_USER,
  ACTION_LOCK_ORIENTATION,
  ACTION_SCREENSHOT_UI,

  N_ACTIONS
} ActionId;

static const char *action_ids[N_ACTIONS] = {
  [ACTION_POWER_OFF]       = "power-off",
  [ACTION_RESTART]         = "restart",
  [ACTION_LOCK_SCREEN]     = "lock-screen",
  [ACTION_LOGOUT]          = "logout",
  [ACTION_SUSPEND]         = "suspend",
  [ACTION_SWITCH_USER]     = "switch-user",
  [ACTION_LOCK_ORIENTATION] = "lock-orientation",
  [ACTION_SCREENSHOT_UI]   = "open-screenshot-ui",
};

/* For the "The %s action is not available!" error of the activate functions */
static const char *action_error_names[N_ACTIONS] = {
  [ACTION_POWER_OFF]       = "power-off",
  [ACTION_RESTART]         = "restart",
  [ACTION_LOCK_SCREEN]     = "lock-screen",
  [ACTION_LOGOUT]          = "logout",
  [ACTION_SUSPEND]         = "suspend",
  [ACTION_SWITCH_USER]     = "switch-user",
  [ACTION_LOCK_ORIENTATION] = "lock-orientation",
  [ACTION_SCREENSHOT_UI]   = "screenshot UI",
};

enum
{
  PROP_0,

  PROP_CAN_POWER_OFF,
  PROP_CAN_RESTART,
  PROP_CAN_SUSPEND,
  PROP_CAN_LOCK_SCREEN,
  PROP_CAN_SWITCH_USER,
  PROP_CAN_LOGOUT,
  PROP_CAN_LOCK_ORIENTATION,
  PROP_ORIENTATION_LOCK_ICON,
  PROP_SESSION_MODE,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  LOCK_SCREEN_REQUESTED,
  HIDE_OVERVIEW_REQUESTED,
  SCREENSHOT_UI_REQUESTED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

typedef struct
{
  char *name;
  char *icon_name;
  GStrv keywords;
  gboolean available;

  /* power off, restart and suspend: the answer of gnome-session */
  gboolean can_have;
  gboolean needs_auth;
} SystemAction;

struct _ShellSystemActions
{
  GObject parent;

  SystemAction actions[N_ACTIONS];

  GSettings *login_screen_settings;  /* nullable */
  GSettings *lockdown_settings;      /* nullable */
  GSettings *orientation_settings;   /* nullable */
  GSettings *screensaver_settings;   /* nullable */

  ShellSessionManager *session;      /* nullable */
  MetaMonitorManager *monitor_manager;

  ActUserManager *user_manager;
  ActUser *user;

  ShellSessionMode *session_mode;    /* nullable */
};

G_DEFINE_FINAL_TYPE (ShellSystemActions, shell_system_actions, G_TYPE_OBJECT)

static ShellSystemActions *default_instance = NULL;

/* tokenizeKeywords (): split at ';' and tokenize and fold every keyword,
 * keeping the ASCII alternatives too like `.flat (2)` did */
static GStrv
tokenize_keywords (const char *keywords)
{
  g_autoptr (GPtrArray) result = g_ptr_array_new_with_free_func (g_free);
  g_auto (GStrv) parts = g_strsplit (keywords, ";", -1);
  int i, j;

  for (i = 0; parts[i] != NULL; i++)
    {
      g_auto (GStrv) tokens = NULL;
      g_auto (GStrv) alternates = NULL;

      tokens = g_str_tokenize_and_fold (parts[i], NULL, &alternates);

      for (j = 0; tokens != NULL && tokens[j] != NULL; j++)
        g_ptr_array_add (result, g_strdup (tokens[j]));
      for (j = 0; alternates != NULL && alternates[j] != NULL; j++)
        g_ptr_array_add (result, g_strdup (alternates[j]));
    }

  g_ptr_array_add (result, NULL);
  return (GStrv) g_ptr_array_free (g_steal_pointer (&result), FALSE);
}

static void
init_action (SystemAction *action,
             const char   *name,
             const char   *icon_name,
             const char   *keywords,
             gboolean      available)
{
  action->name = g_strdup (name);
  action->icon_name = g_strdup (icon_name);
  action->keywords = tokenize_keywords (keywords);
  action->available = available;
}

static void
clear_action (SystemAction *action)
{
  g_clear_pointer (&action->name, g_free);
  g_clear_pointer (&action->icon_name, g_free);
  g_clear_pointer (&action->keywords, g_strfreev);
}

/* new Gio.Settings ({schema_id}) throws for a missing schema, which
 * g_settings_new () would turn into an abort */
static GSettings *
settings_new (const char *schema_id)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = NULL;

  if (source != NULL)
    schema = g_settings_schema_source_lookup (source, schema_id, TRUE);

  if (schema == NULL)
    {
      g_warning ("GSettings schema %s is not installed", schema_id);
      return NULL;
    }

  return g_settings_new_full (schema, NULL, NULL);
}

static gboolean
settings_get_boolean (GSettings  *settings,
                      const char *key)
{
  if (settings == NULL)
    return FALSE;

  return g_settings_get_boolean (settings, key);
}

static gboolean
session_is_locked (ShellSystemActions *self)
{
  return self->session_mode != NULL &&
         shell_session_mode_is_locked (self->session_mode);
}

static gboolean
session_is_greeter (ShellSystemActions *self)
{
  return self->session_mode != NULL &&
         shell_session_mode_is_greeter (self->session_mode);
}

static gboolean
shown_in_session_mode (ShellSystemActions *self)
{
  return !session_is_locked (self) && !session_is_greeter (self);
}

static void
set_available (ShellSystemActions *self,
               ActionId            id,
               gboolean            available,
               int                 prop)
{
  if (self->actions[id].available == available)
    return;

  self->actions[id].available = available;
  g_object_notify_by_pspec (G_OBJECT (self), props[prop]);
}

static void
update_orientation_lock (ShellSystemActions *self)
{
  gboolean available = FALSE;

  if (self->monitor_manager != NULL)
    available = meta_monitor_manager_get_panel_orientation_managed (self->monitor_manager);

  set_available (self, ACTION_LOCK_ORIENTATION, available, PROP_CAN_LOCK_ORIENTATION);
}

static void
update_orientation_lock_status (ShellSystemActions *self)
{
  SystemAction *action = &self->actions[ACTION_LOCK_ORIENTATION];
  gboolean locked = settings_get_boolean (self->orientation_settings, ORIENTATION_LOCK_KEY);
  const char *name;
  const char *icon_name;

  /* Translators: The name of the lock orientation action in search
   * and in the system status menu */
  name = locked
    ? C_("search-result", "Unlock Screen Rotation")
    : C_("search-result", "Lock Screen Rotation");
  icon_name = locked
    ? "rotation-locked-symbolic"
    : "rotation-allowed-symbolic";

  g_free (action->name);
  action->name = g_strdup (name);

  g_free (action->icon_name);
  action->icon_name = g_strdup (icon_name);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ORIENTATION_LOCK_ICON]);
}

static void
update_lock_screen (ShellSystemActions *self)
{
  gboolean show_lock = shown_in_session_mode (self);
  gboolean allow_lock_screen =
    !settings_get_boolean (self->lockdown_settings, DISABLE_LOCK_SCREEN_KEY);

  set_available (self, ACTION_LOCK_SCREEN,
                 show_lock && allow_lock_screen && shell_login_manager_can_lock (),
                 PROP_CAN_LOCK_SCREEN);
}

static void
update_power_off (ShellSystemActions *self)
{
  SystemAction *action = &self->actions[ACTION_POWER_OFF];
  gboolean disabled =
    (session_is_locked (self) &&
     !settings_get_boolean (self->screensaver_settings, RESTART_ENABLED_KEY)) ||
    (session_is_locked (self) && action->needs_auth) ||
    (session_is_greeter (self) &&
     settings_get_boolean (self->login_screen_settings, DISABLE_RESTART_KEY));

  set_available (self, ACTION_POWER_OFF, action->can_have && !disabled, PROP_CAN_POWER_OFF);
}

static void
update_reboot (ShellSystemActions *self)
{
  SystemAction *action = &self->actions[ACTION_RESTART];
  gboolean disabled =
    (session_is_locked (self) &&
     !settings_get_boolean (self->screensaver_settings, RESTART_ENABLED_KEY)) ||
    (session_is_locked (self) && action->needs_auth) ||
    (session_is_greeter (self) &&
     settings_get_boolean (self->login_screen_settings, DISABLE_RESTART_KEY));

  set_available (self, ACTION_RESTART, action->can_have && !disabled, PROP_CAN_RESTART);
}

static void
update_suspend (ShellSystemActions *self)
{
  SystemAction *action = &self->actions[ACTION_SUSPEND];
  gboolean disabled =
    (session_is_locked (self) && action->needs_auth) ||
    (session_is_greeter (self) &&
     settings_get_boolean (self->login_screen_settings, DISABLE_RESTART_KEY));

  set_available (self, ACTION_SUSPEND, action->can_have && !disabled, PROP_CAN_SUSPEND);
}

static void
update_switch_user (ShellSystemActions *self)
{
  gboolean allow_switch =
    !settings_get_boolean (self->lockdown_settings, DISABLE_USER_SWITCH_KEY);
  gboolean has_multiple_users = FALSE;
  gboolean multi_user;

  g_object_get (self->user_manager, "has-multiple-users", &has_multiple_users, NULL);
  multi_user = act_user_manager_can_switch (self->user_manager) && has_multiple_users;

  set_available (self, ACTION_SWITCH_USER,
                 allow_switch && multi_user && shown_in_session_mode (self),
                 PROP_CAN_SWITCH_USER);
}

static void
update_logout (ShellSystemActions *self)
{
  gboolean allow_logout =
    !settings_get_boolean (self->lockdown_settings, DISABLE_LOG_OUT_KEY);
  gboolean always_show =
    g_settings_get_boolean (shell_global_get_settings (shell_global_get ()),
                            ALWAYS_SHOW_LOG_OUT_KEY);
  gboolean system_account = FALSE;
  gboolean local_account = FALSE;
  gboolean multi_user = FALSE;
  gboolean multi_session;
  g_auto (GStrv) session_ids = NULL;

  if (self->user != NULL)
    {
      system_account = act_user_is_system_account (self->user);
      local_account = act_user_is_local_account (self->user);
    }

  g_object_get (self->user_manager, "has-multiple-users", &multi_user, NULL);

  session_ids = gdm_get_session_ids ();
  multi_session = session_ids != NULL && g_strv_length (session_ids) > 1;

  set_available (self, ACTION_LOGOUT,
                 allow_logout &&
                 (always_show || multi_user || multi_session ||
                  system_account || !local_account) &&
                 shown_in_session_mode (self),
                 PROP_CAN_LOGOUT);
}

static void
update_multi_user (ShellSystemActions *self)
{
  update_logout (self);
  update_switch_user (self);
}

static void
session_updated (ShellSystemActions *self)
{
  update_lock_screen (self);
  update_power_off (self);
  update_reboot (self);
  update_suspend (self);
  update_multi_user (self);
}

typedef struct
{
  ShellSystemActions *self;  /* strong */
  ActionId id;
} AvailabilityCall;

static void
availability_call_done_cb (GObject      *source,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  AvailabilityCall *call = user_data;
  g_autoptr (ShellSystemActions) self = call->self;
  ShellSessionManager *session = SHELL_SESSION_MANAGER (source);
  SystemAction *action = &self->actions[call->id];
  g_autoptr (GError) error = NULL;
  guint availability = SHELL_ACTION_AVAILABILITY_UNAVAILABLE;
  gboolean ok = FALSE;
  ActionId id = call->id;

  g_free (call);

  switch (id)
    {
    case ACTION_POWER_OFF:
      ok = shell_session_manager_call_can_shutdown_finish (session, &availability, res, &error);
      break;

    case ACTION_RESTART:
      ok = shell_session_manager_call_can_reboot_finish (session, &availability, res, &error);
      break;

    case ACTION_SUSPEND:
      ok = shell_session_manager_call_can_suspend_finish (session, &availability, res, &error);
      break;

    default:
      g_assert_not_reached ();
    }

  if (ok)
    {
      action->can_have = availability != SHELL_ACTION_AVAILABILITY_UNAVAILABLE;
      action->needs_auth = availability == SHELL_ACTION_AVAILABILITY_CHALLENGE;
    }
  else
    {
      action->can_have = FALSE;
      action->needs_auth = FALSE;
    }

  switch (id)
    {
    case ACTION_POWER_OFF:
      update_power_off (self);
      break;

    case ACTION_RESTART:
      update_reboot (self);
      break;

    case ACTION_SUSPEND:
      update_suspend (self);
      break;

    default:
      g_assert_not_reached ();
    }
}

/* _updateHaveShutdown (), _updateHaveReboot () and _updateHaveSuspend () */
static void
update_have_action (ShellSystemActions *self,
                    ActionId            id)
{
  AvailabilityCall *call;

  if (self->session == NULL)
    {
      /* The JS call threw: the action is not available */
      self->actions[id].can_have = FALSE;
      self->actions[id].needs_auth = FALSE;

      switch (id)
        {
        case ACTION_POWER_OFF:
          update_power_off (self);
          break;

        case ACTION_RESTART:
          update_reboot (self);
          break;

        case ACTION_SUSPEND:
          update_suspend (self);
          break;

        default:
          g_assert_not_reached ();
        }

      return;
    }

  call = g_new0 (AvailabilityCall, 1);
  call->self = g_object_ref (self);
  call->id = id;

  switch (id)
    {
    case ACTION_POWER_OFF:
      shell_session_manager_call_can_shutdown (self->session, NULL,
                                               availability_call_done_cb, call);
      break;

    case ACTION_RESTART:
      shell_session_manager_call_can_reboot (self->session, NULL,
                                             availability_call_done_cb, call);
      break;

    case ACTION_SUSPEND:
      shell_session_manager_call_can_suspend (self->session, NULL,
                                              availability_call_done_cb, call);
      break;

    default:
      g_assert_not_reached ();
    }
}

/**
 * shell_system_actions_force_update:
 * @self: a #ShellSystemActions
 *
 * Asks gnome-session again whether the session can be shut down, restarted
 * and suspended. Whether those actions are available depends on both lockdown
 * settings and Polkit policy; there are no change notifications for the
 * latter, so their value may be outdated.
 */
void
shell_system_actions_force_update (ShellSystemActions *self)
{
  g_return_if_fail (SHELL_IS_SYSTEM_ACTIONS (self));

  update_have_action (self, ACTION_POWER_OFF);
  update_have_action (self, ACTION_RESTART);
  update_have_action (self, ACTION_SUSPEND);
}

/**
 * shell_system_actions_get_matching_actions:
 * @self: a #ShellSystemActions
 * @terms: (array zero-terminated=1) (element-type utf8) (nullable): the search terms
 *
 * Finds the available actions whose keywords all start with one of the
 * (tokenized and folded) @terms.
 *
 * Returns: (transfer full) (array zero-terminated=1) (element-type utf8): the
 *   ids of the matching actions, in search result order
 */
char **
shell_system_actions_get_matching_actions (ShellSystemActions *self,
                                           const char * const *terms)
{
  g_autoptr (GPtrArray) folded_terms = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) results = g_ptr_array_new_with_free_func (g_free);
  int i, j;

  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), NULL);

  for (i = 0; terms != NULL && terms[i] != NULL; i++)
    {
      g_auto (GStrv) tokens = g_str_tokenize_and_fold (terms[i], NULL, NULL);

      for (j = 0; tokens != NULL && tokens[j] != NULL; j++)
        g_ptr_array_add (folded_terms, g_strdup (tokens[j]));
    }

  /* tokenizing may return an empty array */
  if (folded_terms->len > 0)
    {
      for (i = 0; i < N_ACTIONS; i++)
        {
          SystemAction *action = &self->actions[i];
          gboolean matches = action->available;
          guint t;

          for (t = 0; matches && t < folded_terms->len; t++)
            {
              const char *term = g_ptr_array_index (folded_terms, t);
              gboolean found = FALSE;

              for (j = 0; action->keywords[j] != NULL; j++)
                {
                  if (g_str_has_prefix (action->keywords[j], term))
                    {
                      found = TRUE;
                      break;
                    }
                }

              matches = found;
            }

          if (matches)
            g_ptr_array_add (results, g_strdup (action_ids[i]));
        }
    }

  g_ptr_array_add (results, NULL);
  return (char **) g_ptr_array_free (g_steal_pointer (&results), FALSE);
}

static int
find_action (const char *action_id)
{
  int i;

  for (i = 0; i < N_ACTIONS; i++)
    {
      if (g_strcmp0 (action_ids[i], action_id) == 0)
        return i;
    }

  return -1;
}

/**
 * shell_system_actions_get_name:
 * @self: a #ShellSystemActions
 * @action_id: the id of an action, like `power-off`
 *
 * Returns: (transfer none) (nullable): the name of the action shown in
 *   search, or %NULL for an unknown @action_id
 */
const char *
shell_system_actions_get_name (ShellSystemActions *self,
                               const char         *action_id)
{
  int id;

  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), NULL);
  g_return_val_if_fail (action_id != NULL, NULL);

  id = find_action (action_id);
  g_return_val_if_fail (id >= 0, NULL);

  return self->actions[id].name;
}

/**
 * shell_system_actions_get_icon_name:
 * @self: a #ShellSystemActions
 * @action_id: the id of an action, like `power-off`
 *
 * Returns: (transfer none) (nullable): the icon name of the action, or %NULL
 *   for an unknown @action_id
 */
const char *
shell_system_actions_get_icon_name (ShellSystemActions *self,
                                    const char         *action_id)
{
  int id;

  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), NULL);
  g_return_val_if_fail (action_id != NULL, NULL);

  id = find_action (action_id);
  g_return_val_if_fail (id >= 0, NULL);

  return self->actions[id].icon_name;
}

static gboolean
check_available (ShellSystemActions  *self,
                 ActionId             id,
                 GError             **error)
{
  if (self->actions[id].available)
    return TRUE;

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "The %s action is not available!", action_error_names[id]);
  return FALSE;
}

static void
session_call_done_cb (GObject      *source,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  ShellSessionManager *session = SHELL_SESSION_MANAGER (source);
  ActionId id = GPOINTER_TO_INT (user_data);
  g_autoptr (GError) error = NULL;
  gboolean ok = FALSE;

  switch (id)
    {
    case ACTION_LOGOUT:
      ok = shell_session_manager_call_logout_finish (session, res, &error);
      break;

    case ACTION_POWER_OFF:
      ok = shell_session_manager_call_shutdown_finish (session, res, &error);
      break;

    case ACTION_RESTART:
      ok = shell_session_manager_call_reboot_finish (session, res, &error);
      break;

    case ACTION_SUSPEND:
      ok = shell_session_manager_call_suspend_finish (session, res, &error);
      break;

    default:
      g_assert_not_reached ();
    }

  if (!ok && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("Failed to call the %s action of gnome-session: %s",
               action_error_names[id], error->message);
}

/**
 * shell_system_actions_activate_lock_orientation:
 * @self: a #ShellSystemActions
 * @error: return location for a #GError
 *
 * Toggles the orientation lock.
 *
 * Returns: %TRUE on success, %FALSE with @error set if the action is not
 *   available
 */
gboolean
shell_system_actions_activate_lock_orientation (ShellSystemActions  *self,
                                                GError             **error)
{
  gboolean locked;

  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), FALSE);

  if (!check_available (self, ACTION_LOCK_ORIENTATION, error))
    return FALSE;

  if (self->orientation_settings == NULL)
    return TRUE;

  locked = g_settings_get_boolean (self->orientation_settings, ORIENTATION_LOCK_KEY);
  g_settings_set_boolean (self->orientation_settings, ORIENTATION_LOCK_KEY, !locked);
  return TRUE;
}

/**
 * shell_system_actions_activate_lock_screen:
 * @self: a #ShellSystemActions
 * @error: return location for a #GError
 *
 * Locks the screen by emitting #ShellSystemActions::lock-screen-requested.
 *
 * Returns: %TRUE on success, %FALSE with @error set if the action is not
 *   available
 */
gboolean
shell_system_actions_activate_lock_screen (ShellSystemActions  *self,
                                           GError             **error)
{
  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), FALSE);

  if (!check_available (self, ACTION_LOCK_SCREEN, error))
    return FALSE;

  g_signal_emit (self, signals[LOCK_SCREEN_REQUESTED], 0, TRUE);
  return TRUE;
}

static gboolean
goto_login_session_cb (gpointer user_data)
{
  g_autoptr (GError) error = NULL;

  if (!gdm_goto_login_session_sync (NULL, &error))
    g_warning ("Failed to switch to the login session: %s", error->message);

  return G_SOURCE_REMOVE;
}

/**
 * shell_system_actions_activate_switch_user:
 * @self: a #ShellSystemActions
 * @error: return location for a #GError
 *
 * Locks the screen without animation (#ShellSystemActions::lock-screen-requested)
 * and, after the next paint, goes to the login screen.
 *
 * Returns: %TRUE on success, %FALSE with @error set if the action is not
 *   available
 */
gboolean
shell_system_actions_activate_switch_user (ShellSystemActions  *self,
                                           GError             **error)
{
  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), FALSE);

  if (!check_available (self, ACTION_SWITCH_USER, error))
    return FALSE;

  g_signal_emit (self, signals[LOCK_SCREEN_REQUESTED], 0, FALSE);

  clutter_threads_add_repaint_func (CLUTTER_REPAINT_FLAGS_POST_PAINT,
                                    goto_login_session_cb, NULL, NULL);
  return TRUE;
}

/**
 * shell_system_actions_activate_logout:
 * @self: a #ShellSystemActions
 * @error: return location for a #GError
 *
 * Hides the overview (#ShellSystemActions::hide-overview-requested) and asks
 * gnome-session to log out.
 *
 * Returns: %TRUE on success, %FALSE with @error set if the action is not
 *   available
 */
gboolean
shell_system_actions_activate_logout (ShellSystemActions  *self,
                                      GError             **error)
{
  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), FALSE);

  if (!check_available (self, ACTION_LOGOUT, error))
    return FALSE;

  g_signal_emit (self, signals[HIDE_OVERVIEW_REQUESTED], 0);

  if (self->session != NULL)
    shell_session_manager_call_logout (self->session, 0, NULL,
                                       session_call_done_cb,
                                       GINT_TO_POINTER (ACTION_LOGOUT));
  return TRUE;
}

/**
 * shell_system_actions_activate_power_off:
 * @self: a #ShellSystemActions
 * @error: return location for a #GError
 *
 * Returns: %TRUE on success, %FALSE with @error set if the action is not
 *   available
 */
gboolean
shell_system_actions_activate_power_off (ShellSystemActions  *self,
                                         GError             **error)
{
  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), FALSE);

  if (!check_available (self, ACTION_POWER_OFF, error))
    return FALSE;

  if (self->session != NULL)
    shell_session_manager_call_shutdown (self->session, NULL,
                                         session_call_done_cb,
                                         GINT_TO_POINTER (ACTION_POWER_OFF));
  return TRUE;
}

/**
 * shell_system_actions_activate_restart:
 * @self: a #ShellSystemActions
 * @error: return location for a #GError
 *
 * Returns: %TRUE on success, %FALSE with @error set if the action is not
 *   available
 */
gboolean
shell_system_actions_activate_restart (ShellSystemActions  *self,
                                       GError             **error)
{
  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), FALSE);

  if (!check_available (self, ACTION_RESTART, error))
    return FALSE;

  if (self->session != NULL)
    shell_session_manager_call_reboot (self->session, NULL,
                                       session_call_done_cb,
                                       GINT_TO_POINTER (ACTION_RESTART));
  return TRUE;
}

/**
 * shell_system_actions_activate_suspend:
 * @self: a #ShellSystemActions
 * @error: return location for a #GError
 *
 * Returns: %TRUE on success, %FALSE with @error set if the action is not
 *   available
 */
gboolean
shell_system_actions_activate_suspend (ShellSystemActions  *self,
                                       GError             **error)
{
  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), FALSE);

  if (!check_available (self, ACTION_SUSPEND, error))
    return FALSE;

  if (self->session != NULL)
    shell_session_manager_call_suspend (self->session, NULL,
                                        session_call_done_cb,
                                        GINT_TO_POINTER (ACTION_SUSPEND));
  return TRUE;
}

/**
 * shell_system_actions_activate_screenshot_ui:
 * @self: a #ShellSystemActions
 * @error: return location for a #GError
 *
 * Requests the screenshot UI with #ShellSystemActions::screenshot-ui-requested.
 *
 * Returns: %TRUE on success, %FALSE with @error set if the action is not
 *   available
 */
gboolean
shell_system_actions_activate_screenshot_ui (ShellSystemActions  *self,
                                             GError             **error)
{
  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), FALSE);

  if (!check_available (self, ACTION_SCREENSHOT_UI, error))
    return FALSE;

  g_signal_emit (self, signals[SCREENSHOT_UI_REQUESTED], 0);
  return TRUE;
}

/**
 * shell_system_actions_activate_action:
 * @self: a #ShellSystemActions
 * @action_id: the id of an action, like `power-off`
 * @error: return location for a #GError
 *
 * Activates the action with the given id. Unknown ids are ignored.
 *
 * Returns: %TRUE on success, %FALSE with @error set if the action is not
 *   available
 */
gboolean
shell_system_actions_activate_action (ShellSystemActions  *self,
                                      const char          *action_id,
                                      GError             **error)
{
  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), FALSE);
  g_return_val_if_fail (action_id != NULL, FALSE);

  switch (find_action (action_id))
    {
    case ACTION_POWER_OFF:
      return shell_system_actions_activate_power_off (self, error);

    case ACTION_RESTART:
      return shell_system_actions_activate_restart (self, error);

    case ACTION_LOCK_SCREEN:
      return shell_system_actions_activate_lock_screen (self, error);

    case ACTION_LOGOUT:
      return shell_system_actions_activate_logout (self, error);

    case ACTION_SUSPEND:
      return shell_system_actions_activate_suspend (self, error);

    case ACTION_SWITCH_USER:
      return shell_system_actions_activate_switch_user (self, error);

    case ACTION_LOCK_ORIENTATION:
      return shell_system_actions_activate_lock_orientation (self, error);

    case ACTION_SCREENSHOT_UI:
      return shell_system_actions_activate_screenshot_ui (self, error);

    default:
      return TRUE;
    }
}

#define DEFINE_GETTER(name, ACTION)                                         \
  gboolean                                                                  \
  shell_system_actions_get_##name (ShellSystemActions *self)                \
  {                                                                         \
    g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), FALSE);           \
    return self->actions[ACTION].available;                                 \
  }

/**
 * shell_system_actions_get_can_power_off:
 * @self: a #ShellSystemActions
 *
 * Returns: the value of #ShellSystemActions:can-power-off
 */
DEFINE_GETTER (can_power_off, ACTION_POWER_OFF)

/**
 * shell_system_actions_get_can_restart:
 * @self: a #ShellSystemActions
 *
 * Returns: the value of #ShellSystemActions:can-restart
 */
DEFINE_GETTER (can_restart, ACTION_RESTART)

/**
 * shell_system_actions_get_can_suspend:
 * @self: a #ShellSystemActions
 *
 * Returns: the value of #ShellSystemActions:can-suspend
 */
DEFINE_GETTER (can_suspend, ACTION_SUSPEND)

/**
 * shell_system_actions_get_can_lock_screen:
 * @self: a #ShellSystemActions
 *
 * Returns: the value of #ShellSystemActions:can-lock-screen
 */
DEFINE_GETTER (can_lock_screen, ACTION_LOCK_SCREEN)

/**
 * shell_system_actions_get_can_switch_user:
 * @self: a #ShellSystemActions
 *
 * Returns: the value of #ShellSystemActions:can-switch-user
 */
DEFINE_GETTER (can_switch_user, ACTION_SWITCH_USER)

/**
 * shell_system_actions_get_can_logout:
 * @self: a #ShellSystemActions
 *
 * Returns: the value of #ShellSystemActions:can-logout
 */
DEFINE_GETTER (can_logout, ACTION_LOGOUT)

/**
 * shell_system_actions_get_can_lock_orientation:
 * @self: a #ShellSystemActions
 *
 * Returns: the value of #ShellSystemActions:can-lock-orientation
 */
DEFINE_GETTER (can_lock_orientation, ACTION_LOCK_ORIENTATION)

/**
 * shell_system_actions_get_orientation_lock_icon:
 * @self: a #ShellSystemActions
 *
 * Returns: (transfer none): the value of #ShellSystemActions:orientation-lock-icon
 */
const char *
shell_system_actions_get_orientation_lock_icon (ShellSystemActions *self)
{
  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), NULL);

  return self->actions[ACTION_LOCK_ORIENTATION].icon_name;
}

/**
 * shell_system_actions_get_session_mode:
 * @self: a #ShellSystemActions
 *
 * Returns: (transfer none) (nullable): the value of #ShellSystemActions:session-mode
 */
ShellSessionMode *
shell_system_actions_get_session_mode (ShellSystemActions *self)
{
  g_return_val_if_fail (SHELL_IS_SYSTEM_ACTIONS (self), NULL);

  return self->session_mode;
}

static void
session_mode_updated_cb (ShellSessionMode   *session_mode,
                         ShellSystemActions *self)
{
  session_updated (self);
}

/**
 * shell_system_actions_set_session_mode:
 * @self: a #ShellSystemActions
 * @session_mode: (nullable): the session mode of the shell
 *
 * Sets the session mode whose `is_locked` and `is_greeter` decide which
 * actions are available, and updates the actions whenever it emits `updated`.
 */
void
shell_system_actions_set_session_mode (ShellSystemActions *self,
                                       ShellSessionMode   *session_mode)
{
  g_return_if_fail (SHELL_IS_SYSTEM_ACTIONS (self));
  g_return_if_fail (session_mode == NULL || SHELL_IS_SESSION_MODE (session_mode));

  if (self->session_mode == session_mode)
    return;

  if (self->session_mode != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->session_mode,
                                            session_mode_updated_cb, self);
      g_clear_object (&self->session_mode);
    }

  if (session_mode != NULL)
    {
      self->session_mode = g_object_ref (session_mode);
      g_signal_connect_object (self->session_mode, "updated",
                               G_CALLBACK (session_mode_updated_cb), self, 0);
    }

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SESSION_MODE]);

  if (self->session_mode != NULL)
    session_updated (self);
}

static void
lockdown_user_switch_changed_cb (GSettings          *settings,
                                 const char         *key,
                                 ShellSystemActions *self)
{
  update_switch_user (self);
}

static void
lockdown_log_out_changed_cb (GSettings          *settings,
                             const char         *key,
                             ShellSystemActions *self)
{
  update_logout (self);

  update_have_action (self, ACTION_POWER_OFF);
  update_have_action (self, ACTION_RESTART);
}

static void
lockdown_lock_screen_changed_cb (GSettings          *settings,
                                 const char         *key,
                                 ShellSystemActions *self)
{
  update_lock_screen (self);
}

static void
always_show_log_out_changed_cb (GSettings          *settings,
                                const char         *key,
                                ShellSystemActions *self)
{
  update_logout (self);
}

static void
restart_enabled_changed_cb (GSettings          *settings,
                            const char         *key,
                            ShellSystemActions *self)
{
  update_have_action (self, ACTION_POWER_OFF);
  update_have_action (self, ACTION_RESTART);
}

static void
orientation_lock_changed_cb (GSettings          *settings,
                             const char         *key,
                             ShellSystemActions *self)
{
  update_orientation_lock (self);
  update_orientation_lock_status (self);
}

static void
orientation_managed_changed_cb (MetaMonitorManager *monitor_manager,
                                GParamSpec         *pspec,
                                ShellSystemActions *self)
{
  update_orientation_lock (self);
}

static void
monitors_changed_cb (MetaMonitorManager *monitor_manager,
                     ShellSystemActions *self)
{
  update_orientation_lock (self);
}

static void
multi_user_changed_cb (ShellSystemActions *self)
{
  update_multi_user (self);
}

static void
user_loaded_cb (ShellSystemActions *self)
{
  update_logout (self);
}

static void
shell_system_actions_constructed (GObject *object)
{
  ShellSystemActions *self = SHELL_SYSTEM_ACTIONS (object);
  ShellGlobal *global = shell_global_get ();
  GSettings *shell_settings = shell_global_get_settings (global);
  g_autoptr (GError) error = NULL;

  G_OBJECT_CLASS (shell_system_actions_parent_class)->constructed (object);

  self->actions[ACTION_POWER_OFF].can_have = TRUE;
  self->actions[ACTION_RESTART].can_have = TRUE;
  self->actions[ACTION_SUSPEND].can_have = TRUE;

  /* Translators: The name of the power-off action in search */
  init_action (&self->actions[ACTION_POWER_OFF],
               C_("search-result", "Power Off"),
               "system-shutdown-symbolic",
               /* Translators: A list of keywords that match the power-off action, separated by semicolons */
               _("power off;poweroff;shutdown;halt;stop"),
               FALSE);
  /* Translators: The name of the restart action in search */
  init_action (&self->actions[ACTION_RESTART],
               C_("search-result", "Restart"),
               "system-reboot-symbolic",
               /* Translators: A list of keywords that match the restart action, separated by semicolons */
               _("reboot;restart;"),
               FALSE);
  /* Translators: The name of the lock screen action in search */
  init_action (&self->actions[ACTION_LOCK_SCREEN],
               C_("search-result", "Lock Screen"),
               "system-lock-screen-symbolic",
               /* Translators: A list of keywords that match the lock screen action, separated by semicolons */
               _("lock screen"),
               FALSE);
  /* Translators: The name of the logout action in search */
  init_action (&self->actions[ACTION_LOGOUT],
               C_("search-result", "Log Out"),
               "system-log-out-symbolic",
               /* Translators: A list of keywords that match the logout action, separated by semicolons */
               _("logout;log out;sign off"),
               FALSE);
  /* Translators: The name of the suspend action in search */
  init_action (&self->actions[ACTION_SUSPEND],
               C_("search-result", "Suspend"),
               "media-playback-pause-symbolic",
               /* Translators: A list of keywords that match the suspend action, separated by semicolons */
               _("suspend;sleep"),
               FALSE);
  /* Translators: The name of the switch user action in search */
  init_action (&self->actions[ACTION_SWITCH_USER],
               C_("search-result", "Switch User"),
               "system-switch-user-symbolic",
               /* Translators: A list of keywords that match the switch user action, separated by semicolons */
               _("switch user"),
               FALSE);
  init_action (&self->actions[ACTION_LOCK_ORIENTATION],
               "",
               "",
               /* Translators: A list of keywords that match the lock orientation action, separated by semicolons */
               _("lock orientation;unlock orientation;screen;rotation"),
               FALSE);
  /* Translators: The name of the screenshot UI action in search */
  init_action (&self->actions[ACTION_SCREENSHOT_UI],
               C_("search-result", "Take a Screenshot"),
               "record-screen-symbolic",
               /* Translators: A list of keywords that match the screenshot UI action, separated by semicolons */
               _("screenshot;screencast;snip;capture;record"),
               TRUE);

  self->login_screen_settings = settings_new (LOGIN_SCREEN_SCHEMA);
  self->lockdown_settings = settings_new (LOCKDOWN_SCHEMA);
  self->orientation_settings = settings_new (ORIENTATION_SCHEMA);
  self->screensaver_settings = settings_new (SCREENSAVER_SCHEMA);

  self->session = shell_gnome_session_manager_new_sync (NULL, &error);
  if (self->session == NULL)
    g_warning ("Failed to connect to gnome-session: %s", error->message);

  self->monitor_manager =
    meta_backend_get_monitor_manager (shell_global_get_backend (global));

  self->user_manager = act_user_manager_get_default ();

  g_signal_connect_object (self->user_manager, "notify::is-loaded",
                           G_CALLBACK (multi_user_changed_cb), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->user_manager, "notify::has-multiple-users",
                           G_CALLBACK (multi_user_changed_cb), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->user_manager, "user-added",
                           G_CALLBACK (multi_user_changed_cb), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->user_manager, "user-removed",
                           G_CALLBACK (multi_user_changed_cb), self, G_CONNECT_SWAPPED);

  self->user = act_user_manager_get_user (self->user_manager, g_get_user_name ());
  if (self->user != NULL)
    g_signal_connect_object (self->user, "notify::is-loaded",
                             G_CALLBACK (user_loaded_cb), self, G_CONNECT_SWAPPED);

  if (self->lockdown_settings != NULL)
    {
      g_signal_connect_object (self->lockdown_settings,
                               "changed::" DISABLE_USER_SWITCH_KEY,
                               G_CALLBACK (lockdown_user_switch_changed_cb), self, 0);
      g_signal_connect_object (self->lockdown_settings,
                               "changed::" DISABLE_LOG_OUT_KEY,
                               G_CALLBACK (lockdown_log_out_changed_cb), self, 0);
      g_signal_connect_object (self->lockdown_settings,
                               "changed::" DISABLE_LOCK_SCREEN_KEY,
                               G_CALLBACK (lockdown_lock_screen_changed_cb), self, 0);
    }

  g_signal_connect_object (shell_settings,
                           "changed::" ALWAYS_SHOW_LOG_OUT_KEY,
                           G_CALLBACK (always_show_log_out_changed_cb), self, 0);

  if (self->screensaver_settings != NULL)
    g_signal_connect_object (self->screensaver_settings,
                             "changed::" RESTART_ENABLED_KEY,
                             G_CALLBACK (restart_enabled_changed_cb), self, 0);

  shell_system_actions_force_update (self);

  if (self->orientation_settings != NULL)
    g_signal_connect_object (self->orientation_settings,
                             "changed::" ORIENTATION_LOCK_KEY,
                             G_CALLBACK (orientation_lock_changed_cb), self, 0);

  g_signal_connect_object (self->monitor_manager, "monitors-changed",
                           G_CALLBACK (monitors_changed_cb), self, 0);
  g_signal_connect_object (self->monitor_manager, "notify::panel-orientation-managed",
                           G_CALLBACK (orientation_managed_changed_cb), self, 0);
  update_orientation_lock (self);
  update_orientation_lock_status (self);

  /* The rest of what _sessionUpdated () does needs the session mode, and
   * runs when it is set */
}

static void
shell_system_actions_dispose (GObject *object)
{
  ShellSystemActions *self = SHELL_SYSTEM_ACTIONS (object);

  if (self->session_mode != NULL)
    g_signal_handlers_disconnect_by_func (self->session_mode,
                                          session_mode_updated_cb, self);
  g_clear_object (&self->session_mode);

  g_clear_object (&self->login_screen_settings);
  g_clear_object (&self->lockdown_settings);
  g_clear_object (&self->orientation_settings);
  g_clear_object (&self->screensaver_settings);
  g_clear_object (&self->session);

  G_OBJECT_CLASS (shell_system_actions_parent_class)->dispose (object);
}

static void
shell_system_actions_finalize (GObject *object)
{
  ShellSystemActions *self = SHELL_SYSTEM_ACTIONS (object);
  int i;

  for (i = 0; i < N_ACTIONS; i++)
    clear_action (&self->actions[i]);

  if (default_instance == self)
    default_instance = NULL;

  G_OBJECT_CLASS (shell_system_actions_parent_class)->finalize (object);
}

static void
shell_system_actions_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  ShellSystemActions *self = SHELL_SYSTEM_ACTIONS (object);

  switch (prop_id)
    {
    case PROP_CAN_POWER_OFF:
      g_value_set_boolean (value, self->actions[ACTION_POWER_OFF].available);
      break;

    case PROP_CAN_RESTART:
      g_value_set_boolean (value, self->actions[ACTION_RESTART].available);
      break;

    case PROP_CAN_SUSPEND:
      g_value_set_boolean (value, self->actions[ACTION_SUSPEND].available);
      break;

    case PROP_CAN_LOCK_SCREEN:
      g_value_set_boolean (value, self->actions[ACTION_LOCK_SCREEN].available);
      break;

    case PROP_CAN_SWITCH_USER:
      g_value_set_boolean (value, self->actions[ACTION_SWITCH_USER].available);
      break;

    case PROP_CAN_LOGOUT:
      g_value_set_boolean (value, self->actions[ACTION_LOGOUT].available);
      break;

    case PROP_CAN_LOCK_ORIENTATION:
      g_value_set_boolean (value, self->actions[ACTION_LOCK_ORIENTATION].available);
      break;

    case PROP_ORIENTATION_LOCK_ICON:
      g_value_set_string (value, self->actions[ACTION_LOCK_ORIENTATION].icon_name);
      break;

    case PROP_SESSION_MODE:
      g_value_set_object (value, self->session_mode);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_system_actions_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  ShellSystemActions *self = SHELL_SYSTEM_ACTIONS (object);

  switch (prop_id)
    {
    case PROP_SESSION_MODE:
      shell_system_actions_set_session_mode (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_system_actions_class_init (ShellSystemActionsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  const GParamFlags ro_flags = G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;

  object_class->constructed = shell_system_actions_constructed;
  object_class->dispose = shell_system_actions_dispose;
  object_class->finalize = shell_system_actions_finalize;
  object_class->get_property = shell_system_actions_get_property;
  object_class->set_property = shell_system_actions_set_property;

  /**
   * ShellSystemActions:can-power-off:
   *
   * Whether the power-off action is available.
   */
  props[PROP_CAN_POWER_OFF] =
    g_param_spec_boolean ("can-power-off", NULL, NULL, FALSE, ro_flags);

  /**
   * ShellSystemActions:can-restart:
   *
   * Whether the restart action is available.
   */
  props[PROP_CAN_RESTART] =
    g_param_spec_boolean ("can-restart", NULL, NULL, FALSE, ro_flags);

  /**
   * ShellSystemActions:can-suspend:
   *
   * Whether the suspend action is available.
   */
  props[PROP_CAN_SUSPEND] =
    g_param_spec_boolean ("can-suspend", NULL, NULL, FALSE, ro_flags);

  /**
   * ShellSystemActions:can-lock-screen:
   *
   * Whether the lock-screen action is available.
   */
  props[PROP_CAN_LOCK_SCREEN] =
    g_param_spec_boolean ("can-lock-screen", NULL, NULL, FALSE, ro_flags);

  /**
   * ShellSystemActions:can-switch-user:
   *
   * Whether the switch-user action is available.
   */
  props[PROP_CAN_SWITCH_USER] =
    g_param_spec_boolean ("can-switch-user", NULL, NULL, FALSE, ro_flags);

  /**
   * ShellSystemActions:can-logout:
   *
   * Whether the logout action is available.
   */
  props[PROP_CAN_LOGOUT] =
    g_param_spec_boolean ("can-logout", NULL, NULL, FALSE, ro_flags);

  /**
   * ShellSystemActions:can-lock-orientation:
   *
   * Whether the lock-orientation action is available.
   */
  props[PROP_CAN_LOCK_ORIENTATION] =
    g_param_spec_boolean ("can-lock-orientation", NULL, NULL, FALSE, ro_flags);

  /**
   * ShellSystemActions:orientation-lock-icon:
   *
   * The icon name of the lock-orientation action, which depends on whether
   * the orientation is locked.
   */
  props[PROP_ORIENTATION_LOCK_ICON] =
    g_param_spec_string ("orientation-lock-icon", NULL, NULL, NULL, ro_flags);

  /**
   * ShellSystemActions:session-mode:
   *
   * The session mode of the shell (`Main.sessionMode`), set by js/ui/main.js.
   */
  props[PROP_SESSION_MODE] =
    g_param_spec_object ("session-mode", NULL, NULL,
                         SHELL_TYPE_SESSION_MODE,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellSystemActions::lock-screen-requested:
   * @self: the #ShellSystemActions
   * @animate: whether the screen shield should animate
   *
   * Emitted when the screen has to be locked (`Main.screenShield.lock (animate)`).
   */
  signals[LOCK_SCREEN_REQUESTED] =
    g_signal_new ("lock-screen-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);

  /**
   * ShellSystemActions::hide-overview-requested:
   * @self: the #ShellSystemActions
   *
   * Emitted when the overview has to be hidden (`Main.overview.hide ()`).
   */
  signals[HIDE_OVERVIEW_REQUESTED] =
    g_signal_new ("hide-overview-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellSystemActions::screenshot-ui-requested:
   * @self: the #ShellSystemActions
   *
   * Emitted when the screenshot UI has to be opened once the overview is
   * hidden (`Screenshot.showScreenshotUI ()`).
   */
  signals[SCREENSHOT_UI_REQUESTED] =
    g_signal_new ("screenshot-ui-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_system_actions_init (ShellSystemActions *self)
{
}

/**
 * shell_system_actions_get_default:
 *
 * Returns: (transfer none): the #ShellSystemActions singleton
 */
ShellSystemActions *
shell_system_actions_get_default (void)
{
  if (default_instance == NULL)
    default_instance = g_object_new (SHELL_TYPE_SYSTEM_ACTIONS, NULL);

  return default_instance;
}
