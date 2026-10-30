/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * login-manager.c: An abstraction over systemd/logind. Port of
 * js/misc/loginManager.js.
 *
 * Where the JS picked LoginManagerSystemd or LoginManagerDummy, there is a
 * single class here: without systemd (or if the logind proxy cannot be
 * created) it behaves like LoginManagerDummy.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include <gio/gunixfdlist.h>
#include <gio/gunixinputstream.h>
#include <glib/gstdio.h>

#include "misc/login-manager.h"

#define LOGIND_BUS_NAME "org.freedesktop.login1"
#define LOGIND_OBJECT_PATH "/org/freedesktop/login1"
#define LOGIND_AUTO_SESSION_PATH "/org/freedesktop/login1/session/auto"

#define GDM_BUS_NAME "org.gnome.DisplayManager"
#define GDM_OBJECT_PATH "/org/gnome/DisplayManager/Manager"
#define GDM_MANAGER_INTERFACE "org.gnome.DisplayManager.Manager"

/* Show the boot loader menu for 60 seconds, in microseconds */
#define BOOT_LOADER_MENU_TIMEOUT_USEC G_GUINT64_CONSTANT (60000000)

enum
{
  PROP_0,

  PROP_PREPARING_FOR_SLEEP,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  PREPARE_FOR_SLEEP,
  SESSION_REMOVED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellLoginManager
{
  GObject parent;

  ShellLogin1Manager *proxy;  /* NULL when logind is not available */
  ShellLogin1User *user;      /* cached by get_current_user_async() */
  ShellLogin1Session *session; /* cached by get_current_session_async() */

  gboolean preparing_for_sleep;
};

G_DEFINE_FINAL_TYPE (ShellLoginManager, shell_login_manager, G_TYPE_OBJECT)

static gboolean
have_systemd (void)
{
  return g_access ("/run/systemd/seats", 0) >= 0;
}

/* Parses the leading integer of @str like JavaScript's parseInt() */
static gboolean
parse_int (const char *str,
           int64_t    *value)
{
  char *end = NULL;

  errno = 0;
  *value = g_ascii_strtoll (str, &end, 10);

  return end != str && errno == 0;
}

static gboolean
version_compare (const char *required,
                 const char *reference)
{
  g_auto (GStrv) required_parts = g_strsplit (required, ".", -1);
  g_auto (GStrv) reference_parts = g_strsplit (reference, ".", -1);
  int i;

  for (i = 0; required_parts[i] != NULL; i++)
    {
      int64_t required_int, reference_int;

      if (reference_parts[i] == NULL ||
          !parse_int (required_parts[i], &required_int) ||
          !parse_int (reference_parts[i], &reference_int))
        return FALSE;

      if (required_int != reference_int)
        return required_int < reference_int;
    }

  return TRUE;
}

/**
 * shell_login_manager_can_lock:
 *
 * Checks whether the session can be locked, i.e. whether systemd is in use
 * and GDM is recent enough (3.5.91) to support it. This does a blocking call
 * to GDM.
 *
 * Returns: %TRUE if the screen can be locked
 */
gboolean
shell_login_manager_can_lock (void)
{
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GVariant) result = NULL;
  g_autoptr (GVariant) value = NULL;
  const char *version;

  connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (connection == NULL)
    return FALSE;

  result = g_dbus_connection_call_sync (connection,
                                        GDM_BUS_NAME,
                                        GDM_OBJECT_PATH,
                                        "org.freedesktop.DBus.Properties",
                                        "Get",
                                        g_variant_new ("(ss)",
                                                       GDM_MANAGER_INTERFACE,
                                                       "Version"),
                                        G_VARIANT_TYPE ("(v)"),
                                        G_DBUS_CALL_FLAGS_NONE,
                                        -1, NULL, NULL);
  if (result == NULL)
    return FALSE;

  g_variant_get (result, "(v)", &value);
  if (!g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
    return FALSE;

  version = g_variant_get_string (value, NULL);

  return have_systemd () && version_compare ("3.5.91", version);
}

static void
register_with_gdm_done (GObject      *source,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  g_autofree char *method = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) result = NULL;

  result = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  if (result != NULL)
    return;

  if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
    g_message ("Not calling %s(): method not exported, GDM too old?", method);
  else
    g_message ("Error calling %s on GDM: %s", method, error->message);
}

static void
register_with_gdm (const char *method)
{
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GError) error = NULL;

  connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  if (connection == NULL)
    {
      g_message ("Error calling %s on GDM: %s", method, error->message);
      return;
    }

  g_dbus_connection_call (connection,
                          GDM_BUS_NAME,
                          GDM_OBJECT_PATH,
                          GDM_MANAGER_INTERFACE,
                          method,
                          NULL, NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1, NULL,
                          register_with_gdm_done,
                          g_strdup (method));
}

/**
 * shell_login_manager_register_session_with_gdm:
 *
 * Calls RegisterSession() on GDM, without waiting for the reply. Failures
 * are logged; a GDM that does not export the method is not an error.
 */
void
shell_login_manager_register_session_with_gdm (void)
{
  g_message ("Registering session with GDM");
  register_with_gdm ("RegisterSession");
}

/**
 * shell_login_manager_register_display_with_gdm:
 *
 * Calls RegisterDisplay() on GDM, without waiting for the reply. Failures
 * are logged; a GDM that does not export the method is not an error.
 */
void
shell_login_manager_register_display_with_gdm (void)
{
  g_message ("Registering display with GDM");
  register_with_gdm ("RegisterDisplay");
}

static void
on_prepare_for_sleep (ShellLogin1Manager *proxy,
                      gboolean            about_to_suspend,
                      gpointer            user_data)
{
  ShellLoginManager *self = user_data;

  if (self->preparing_for_sleep != about_to_suspend)
    {
      self->preparing_for_sleep = about_to_suspend;
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PREPARING_FOR_SLEEP]);
    }

  g_signal_emit (self, signals[PREPARE_FOR_SLEEP], 0, about_to_suspend);
}

static void
on_session_removed (ShellLogin1Manager *proxy,
                    const char         *session_id,
                    const char         *object_path,
                    gpointer            user_data)
{
  ShellLoginManager *self = user_data;

  g_signal_emit (self, signals[SESSION_REMOVED], 0, session_id);
}

static void
shell_login_manager_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellLoginManager *self = SHELL_LOGIN_MANAGER (object);

  switch (prop_id)
    {
    case PROP_PREPARING_FOR_SLEEP:
      g_value_set_boolean (value, self->preparing_for_sleep);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_login_manager_dispose (GObject *object)
{
  ShellLoginManager *self = SHELL_LOGIN_MANAGER (object);

  g_clear_object (&self->user);
  g_clear_object (&self->session);
  g_clear_object (&self->proxy);

  G_OBJECT_CLASS (shell_login_manager_parent_class)->dispose (object);
}

static void
shell_login_manager_class_init (ShellLoginManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_login_manager_get_property;
  object_class->dispose = shell_login_manager_dispose;

  /**
   * ShellLoginManager:preparing-for-sleep:
   *
   * Whether the machine is preparing to sleep. This is %TRUE between paired
   * emissions of #ShellLoginManager::prepare-for-sleep.
   */
  props[PROP_PREPARING_FOR_SLEEP] =
    g_param_spec_boolean ("preparing-for-sleep", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellLoginManager::prepare-for-sleep:
   * @self: the #ShellLoginManager
   * @about_to_suspend: %TRUE just before the machine suspends, %FALSE after
   *   it resumed
   *
   * Emitted when logind announces that the machine is about to sleep or has
   * just woken up.
   */
  signals[PREPARE_FOR_SLEEP] =
    g_signal_new ("prepare-for-sleep",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_BOOLEAN);

  /**
   * ShellLoginManager::session-removed:
   * @self: the #ShellLoginManager
   * @session_id: the logind ID of the removed session
   *
   * Emitted when logind removes a session.
   */
  signals[SESSION_REMOVED] =
    g_signal_new ("session-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
}

static void
shell_login_manager_init (ShellLoginManager *self)
{
  g_autoptr (GError) error = NULL;

  if (!have_systemd ())
    return;

  self->proxy = shell_login1_manager_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                             G_DBUS_PROXY_FLAGS_NONE,
                                                             LOGIND_BUS_NAME,
                                                             LOGIND_OBJECT_PATH,
                                                             NULL, &error);
  if (self->proxy == NULL)
    {
      g_warning ("Could not create a proxy for logind: %s", error->message);
      return;
    }

  g_signal_connect_object (self->proxy, "prepare-for-sleep",
                           G_CALLBACK (on_prepare_for_sleep), self, 0);
  g_signal_connect_object (self->proxy, "session-removed",
                           G_CALLBACK (on_session_removed), self, 0);
}

/**
 * shell_login_manager_get_default:
 *
 * Gets the #ShellLoginManager singleton. If systemd is not running, the
 * returned object behaves like the former LoginManagerDummy: nothing can be
 * rebooted to the boot loader menu, no sessions are listed, inhibiting
 * returns no stream, and the asynchronous operations that need logind to
 * hand out a proxy never complete.
 *
 * Returns: (transfer none): the #ShellLoginManager
 */
ShellLoginManager *
shell_login_manager_get_default (void)
{
  static ShellLoginManager *default_manager = NULL;

  if (default_manager == NULL)
    default_manager = g_object_new (SHELL_TYPE_LOGIN_MANAGER, NULL);

  return default_manager;
}

/**
 * shell_login_manager_get_preparing_for_sleep:
 * @self: a #ShellLoginManager
 *
 * Returns: whether the machine is preparing to sleep
 */
gboolean
shell_login_manager_get_preparing_for_sleep (ShellLoginManager *self)
{
  g_return_val_if_fail (SHELL_IS_LOGIN_MANAGER (self), FALSE);

  return self->preparing_for_sleep;
}

static void
current_user_proxy_ready (GObject      *source,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  ShellLoginManager *self = g_task_get_source_object (task);
  g_autoptr (ShellLogin1User) user = NULL;
  g_autoptr (GError) error = NULL;

  user = shell_login1_user_proxy_new_for_bus_finish (res, &error);
  if (user == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  /* A concurrent call may have been faster, hand out the same object */
  if (self->user == NULL)
    self->user = g_object_ref (user);

  g_task_return_pointer (task, g_object_ref (self->user), g_object_unref);
}

static void
current_user_path_ready (GObject      *source,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  g_autofree char *object_path = NULL;
  g_autoptr (GError) error = NULL;
  GCancellable *cancellable;

  if (!shell_login1_manager_call_get_user_finish (SHELL_LOGIN1_MANAGER (source),
                                                  &object_path, res, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  cancellable = g_task_get_cancellable (task);
  shell_login1_user_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                       G_DBUS_PROXY_FLAGS_NONE,
                                       LOGIND_BUS_NAME,
                                       object_path,
                                       cancellable,
                                       current_user_proxy_ready,
                                       g_steal_pointer (&task));
}

/**
 * shell_login_manager_get_current_user_async:
 * @self: a #ShellLoginManager
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (closure user_data): called when the proxy is available
 * @user_data: user data for @callback
 *
 * Gets a proxy for the logind object of the user running the shell. The
 * proxy is created once and then cached.
 *
 * Without logind, @callback is never called.
 */
void
shell_login_manager_get_current_user_async (ShellLoginManager   *self,
                                            GCancellable        *cancellable,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_LOGIN_MANAGER (self));

  /* Like the JS dummy implementation, never settle: callers can't do
   * anything useful with a fake user. */
  if (self->proxy == NULL)
    return;

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, shell_login_manager_get_current_user_async);

  if (self->user != NULL)
    {
      g_task_return_pointer (task, g_object_ref (self->user), g_object_unref);
      return;
    }

  shell_login1_manager_call_get_user (self->proxy, getuid (), cancellable,
                                      current_user_path_ready,
                                      g_steal_pointer (&task));
}

/**
 * shell_login_manager_get_current_user_finish:
 * @self: a #ShellLoginManager
 * @res: a #GAsyncResult
 * @error: return location for a #GError
 *
 * Finishes shell_login_manager_get_current_user_async().
 *
 * Returns: (transfer full): the proxy for the current user, or %NULL with
 *   @error set
 */
ShellLogin1User *
shell_login_manager_get_current_user_finish (ShellLoginManager  *self,
                                             GAsyncResult       *res,
                                             GError            **error)
{
  g_return_val_if_fail (SHELL_IS_LOGIN_MANAGER (self), NULL);
  g_return_val_if_fail (g_task_is_valid (res, self), NULL);

  return g_task_propagate_pointer (G_TASK (res), error);
}

static void
current_session_proxy_ready (GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  ShellLoginManager *self = g_task_get_source_object (task);
  g_autoptr (ShellLogin1Session) session = NULL;
  g_autoptr (GError) error = NULL;

  session = shell_login1_session_proxy_new_for_bus_finish (res, &error);
  if (session == NULL)
    {
      g_prefix_error (&error, "Could not get proxy for session %s: ",
                      (const char *) g_task_get_task_data (task));
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  /* A concurrent call may have been faster, hand out the same object */
  if (self->session == NULL)
    self->session = g_object_ref (session);

  g_task_return_pointer (task, g_object_ref (self->session), g_object_unref);
}

static void
current_session_path_ready (GObject      *source,
                            GAsyncResult *res,
                            gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  g_autofree char *object_path = NULL;
  g_autoptr (GError) error = NULL;
  GCancellable *cancellable;

  if (!shell_login1_manager_call_get_session_finish (SHELL_LOGIN1_MANAGER (source),
                                                     &object_path, res, &error))
    {
      g_prefix_error (&error, "Could not get proxy for session %s: ",
                      (const char *) g_task_get_task_data (task));
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  cancellable = g_task_get_cancellable (task);
  shell_login1_session_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          LOGIND_BUS_NAME,
                                          object_path,
                                          cancellable,
                                          current_session_proxy_ready,
                                          g_steal_pointer (&task));
}

static void
look_up_current_session (GTask      *task,
                         const char *session_id)
{
  ShellLoginManager *self = g_task_get_source_object (task);

  g_message ("Will monitor session %s", session_id);

  g_task_set_task_data (task, g_strdup (session_id), g_free);
  shell_login1_manager_call_get_session (self->proxy, session_id,
                                         g_task_get_cancellable (task),
                                         current_session_path_ready,
                                         task);
}

static void
auto_session_proxy_ready (GObject      *source,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  g_autoptr (ShellLogin1Session) auto_session = NULL;
  g_autoptr (GError) error = NULL;
  const char *session_id;

  auto_session = shell_login1_session_proxy_new_for_bus_finish (res, &error);
  if (auto_session == NULL)
    {
      g_prefix_error (&error, "Failed to get session from logind: ");
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  /* A proxy is made even when logind is not running: it has no ID then */
  session_id = shell_login1_session_get_id (auto_session);
  if (session_id == NULL || *session_id == '\0')
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "Failed to get session from logind: no session");
      return;
    }

  look_up_current_session (g_steal_pointer (&task), session_id);
}

/**
 * shell_login_manager_get_current_session_async:
 * @self: a #ShellLoginManager
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (closure user_data): called when the proxy is available
 * @user_data: user data for @callback
 *
 * Gets a proxy for the logind session the shell runs in, found through
 * `XDG_SESSION_ID` or, if that is unset, by asking logind directly. The
 * proxy is created once and then cached.
 *
 * Without logind, @callback is never called.
 */
void
shell_login_manager_get_current_session_async (ShellLoginManager   *self,
                                               GCancellable        *cancellable,
                                               GAsyncReadyCallback  callback,
                                               gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;
  const char *session_id;

  g_return_if_fail (SHELL_IS_LOGIN_MANAGER (self));

  /* Like the JS dummy implementation, never settle: callers can't do
   * anything useful with a fake session. */
  if (self->proxy == NULL)
    return;

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, shell_login_manager_get_current_session_async);

  if (self->session != NULL)
    {
      g_task_return_pointer (task, g_object_ref (self->session), g_object_unref);
      return;
    }

  session_id = g_getenv ("XDG_SESSION_ID");
  if (session_id == NULL || *session_id == '\0')
    {
      g_message ("Unset XDG_SESSION_ID, get_current_session() called outside a user session. Asking logind directly.");
      shell_login1_session_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                              G_DBUS_PROXY_FLAGS_NONE,
                                              LOGIND_BUS_NAME,
                                              LOGIND_AUTO_SESSION_PATH,
                                              cancellable,
                                              auto_session_proxy_ready,
                                              g_steal_pointer (&task));
      return;
    }

  look_up_current_session (g_steal_pointer (&task), session_id);
}

/**
 * shell_login_manager_get_current_session_finish:
 * @self: a #ShellLoginManager
 * @res: a #GAsyncResult
 * @error: return location for a #GError
 *
 * Finishes shell_login_manager_get_current_session_async().
 *
 * Returns: (transfer full): the proxy for the current session, or %NULL with
 *   @error set
 */
ShellLogin1Session *
shell_login_manager_get_current_session_finish (ShellLoginManager  *self,
                                                GAsyncResult       *res,
                                                GError            **error)
{
  g_return_val_if_fail (SHELL_IS_LOGIN_MANAGER (self), NULL);
  g_return_val_if_fail (g_task_is_valid (res, self), NULL);

  return g_task_propagate_pointer (G_TASK (res), error);
}

static void
can_reboot_to_boot_loader_menu_ready (GObject      *source,
                                      GAsyncResult *res,
                                      gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  g_autofree char *result = NULL;
  gboolean can_reboot = FALSE;
  gboolean needs_auth = FALSE;

  /* Errors are not interesting, the menu is just not available */
  if (shell_login1_manager_call_can_reboot_to_boot_loader_menu_finish (SHELL_LOGIN1_MANAGER (source),
                                                                       &result, res, NULL))
    {
      needs_auth = g_strcmp0 (result, "challenge") == 0;
      can_reboot = needs_auth || g_strcmp0 (result, "yes") == 0;
    }

  g_task_return_int (task, (can_reboot ? 1 : 0) | (needs_auth ? 2 : 0));
}

/**
 * shell_login_manager_can_reboot_to_boot_loader_menu_async:
 * @self: a #ShellLoginManager
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (closure user_data): called when the answer is known
 * @user_data: user data for @callback
 *
 * Asks logind whether the machine can be rebooted into the boot loader menu.
 */
void
shell_login_manager_can_reboot_to_boot_loader_menu_async (ShellLoginManager   *self,
                                                          GCancellable        *cancellable,
                                                          GAsyncReadyCallback  callback,
                                                          gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_LOGIN_MANAGER (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, shell_login_manager_can_reboot_to_boot_loader_menu_async);

  if (self->proxy == NULL)
    {
      g_task_return_int (task, 0);
      return;
    }

  shell_login1_manager_call_can_reboot_to_boot_loader_menu (self->proxy,
                                                            cancellable,
                                                            can_reboot_to_boot_loader_menu_ready,
                                                            g_steal_pointer (&task));
}

/**
 * shell_login_manager_can_reboot_to_boot_loader_menu_finish:
 * @self: a #ShellLoginManager
 * @res: a #GAsyncResult
 * @needs_auth: (out) (optional): whether authentication would be required
 *
 * Finishes shell_login_manager_can_reboot_to_boot_loader_menu_async(). This
 * never fails: if logind could not be asked, the answer is %FALSE.
 *
 * Returns: whether the machine can be rebooted into the boot loader menu
 */
gboolean
shell_login_manager_can_reboot_to_boot_loader_menu_finish (ShellLoginManager *self,
                                                           GAsyncResult      *res,
                                                           gboolean          *needs_auth)
{
  gssize flags;

  g_return_val_if_fail (SHELL_IS_LOGIN_MANAGER (self), FALSE);
  g_return_val_if_fail (g_task_is_valid (res, self), FALSE);

  flags = g_task_propagate_int (G_TASK (res), NULL);
  if (flags < 0)
    flags = 0;

  if (needs_auth != NULL)
    *needs_auth = (flags & 2) != 0;

  return (flags & 1) != 0;
}

static void
set_reboot_to_boot_loader_menu_ready (GObject      *source,
                                      GAsyncResult *res,
                                      gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!shell_login1_manager_call_set_reboot_to_boot_loader_menu_finish (SHELL_LOGIN1_MANAGER (source),
                                                                        res, &error))
    g_warning ("Could not set the reboot to boot loader menu: %s", error->message);
}

/**
 * shell_login_manager_set_reboot_to_boot_loader_menu:
 * @self: a #ShellLoginManager
 *
 * Asks logind to show the boot loader menu on the next boot, for 60
 * seconds. The reply is not waited for.
 */
void
shell_login_manager_set_reboot_to_boot_loader_menu (ShellLoginManager *self)
{
  g_return_if_fail (SHELL_IS_LOGIN_MANAGER (self));

  if (self->proxy == NULL)
    return;

  shell_login1_manager_call_set_reboot_to_boot_loader_menu (self->proxy,
                                                            BOOT_LOADER_MENU_TIMEOUT_USEC,
                                                            NULL,
                                                            set_reboot_to_boot_loader_menu_ready,
                                                            NULL);
}

static GVariant *
empty_session_list (void)
{
  return g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("(susso)"),
                                                  NULL, 0));
}

static void
list_sessions_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  GVariant *sessions = NULL;

  if (!shell_login1_manager_call_list_sessions_finish (SHELL_LOGIN1_MANAGER (source),
                                                       &sessions, res, NULL))
    sessions = empty_session_list ();

  g_task_return_pointer (task, sessions, (GDestroyNotify) g_variant_unref);
}

/**
 * shell_login_manager_list_sessions_async:
 * @self: a #ShellLoginManager
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (closure user_data): called when the sessions are known
 * @user_data: user data for @callback
 *
 * Lists the sessions known to logind.
 */
void
shell_login_manager_list_sessions_async (ShellLoginManager   *self,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_LOGIN_MANAGER (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, shell_login_manager_list_sessions_async);

  if (self->proxy == NULL)
    {
      g_task_return_pointer (task, empty_session_list (),
                             (GDestroyNotify) g_variant_unref);
      return;
    }

  shell_login1_manager_call_list_sessions (self->proxy, cancellable,
                                           list_sessions_ready,
                                           g_steal_pointer (&task));
}

/**
 * shell_login_manager_list_sessions_finish:
 * @self: a #ShellLoginManager
 * @res: a #GAsyncResult
 *
 * Finishes shell_login_manager_list_sessions_async(). This never fails: if
 * logind could not be asked, the list is empty.
 *
 * Returns: (transfer full): a #GVariant of type `a(susso)` holding
 *   (session id, user id, user name, seat id, object path) for each session
 */
GVariant *
shell_login_manager_list_sessions_finish (ShellLoginManager *self,
                                          GAsyncResult      *res)
{
  GVariant *sessions;

  g_return_val_if_fail (SHELL_IS_LOGIN_MANAGER (self), NULL);
  g_return_val_if_fail (g_task_is_valid (res, self), NULL);

  sessions = g_task_propagate_pointer (G_TASK (res), NULL);

  return sessions != NULL ? sessions : empty_session_list ();
}

/**
 * shell_login_manager_get_session:
 * @self: a #ShellLoginManager
 * @object_path: the logind object path of a session
 * @error: return location for a #GError
 *
 * Creates a proxy for a logind session. This blocks until the proxy is ready.
 *
 * Returns: (transfer full) (nullable): the session proxy, or %NULL if there
 *   is no logind (without setting @error), or on failure (setting @error)
 */
ShellLogin1Session *
shell_login_manager_get_session (ShellLoginManager  *self,
                                 const char         *object_path,
                                 GError            **error)
{
  g_return_val_if_fail (SHELL_IS_LOGIN_MANAGER (self), NULL);
  g_return_val_if_fail (object_path != NULL, NULL);

  if (self->proxy == NULL)
    return NULL;

  return shell_login1_session_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                      G_DBUS_PROXY_FLAGS_NONE,
                                                      LOGIND_BUS_NAME,
                                                      object_path,
                                                      NULL, error);
}

static void
inhibit_ready (GObject      *source,
               GAsyncResult *res,
               gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  g_autoptr (GVariant) fd_handle = NULL;
  g_autoptr (GUnixFDList) fd_list = NULL;
  g_autoptr (GError) error = NULL;
  int fd;

  if (!shell_login1_manager_call_inhibit_finish (SHELL_LOGIN1_MANAGER (source),
                                                 &fd_handle, &fd_list,
                                                 res, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  if (fd_list == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "logind did not return a file descriptor");
      return;
    }

  fd = g_unix_fd_list_get (fd_list, g_variant_get_handle (fd_handle), &error);
  if (fd < 0)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_pointer (task, g_unix_input_stream_new (fd, TRUE),
                         g_object_unref);
}

/**
 * shell_login_manager_inhibit_async:
 * @self: a #ShellLoginManager
 * @reason: why sleep is being delayed, shown to the user by logind tools
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (closure user_data): called when the inhibitor is taken
 * @user_data: user data for @callback
 *
 * Takes a delay inhibitor on sleep. The inhibitor is released by closing the
 * stream returned by shell_login_manager_inhibit_finish().
 */
void
shell_login_manager_inhibit_async (ShellLoginManager   *self,
                                   const char          *reason,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (SHELL_IS_LOGIN_MANAGER (self));
  g_return_if_fail (reason != NULL);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, shell_login_manager_inhibit_async);

  if (self->proxy == NULL)
    {
      g_task_return_pointer (task, NULL, NULL);
      return;
    }

  shell_login1_manager_call_inhibit (self->proxy,
                                     "sleep", "GNOME Shell", reason, "delay",
                                     NULL, cancellable,
                                     inhibit_ready,
                                     g_steal_pointer (&task));
}

/**
 * shell_login_manager_inhibit_finish:
 * @self: a #ShellLoginManager
 * @res: a #GAsyncResult
 * @error: return location for a #GError
 *
 * Finishes shell_login_manager_inhibit_async().
 *
 * Returns: (transfer full) (nullable): a stream holding the inhibitor, which
 *   is released when the stream is closed or finalized; %NULL without
 *   logind (@error is not set), or %NULL with @error set on failure
 */
GInputStream *
shell_login_manager_inhibit_finish (ShellLoginManager  *self,
                                    GAsyncResult       *res,
                                    GError            **error)
{
  g_return_val_if_fail (SHELL_IS_LOGIN_MANAGER (self), NULL);
  g_return_val_if_fail (g_task_is_valid (res, self), NULL);

  return g_task_propagate_pointer (G_TASK (res), error);
}
