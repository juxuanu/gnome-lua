/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * autorun-manager.c: Runs the application for the content of the media that
 * are mounted, or asks the user what to do with them in a notification. Port
 * of js/ui/components/autorunManager.js.
 *
 * The types of the JS module are folded into #ShellAutorunManager:
 * `ContentTypeDiscoverer` is the asynchronous guess_content_types () and
 * `AutorunDispatcher` the dispatcher_* functions and the table of the
 * notifications. The content types that the mount does not know are sniffed
 * by the org.gnome.Shell.HotplugSniffer service, through a gdbus-codegen
 * proxy.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The message tray is the construct-only property
 *    #ShellAutorunManager:message-tray (`Main.messageTray`).
 *  - The title of the notification of a mount is translated: the JS
 *    formatted the string before it asked for its translation, so it never
 *    matched a translation.
 *  - The default application of `inode/directory` is skipped when there is
 *    none (the JS pushed null and failed later).
 *  - If gnome-session is not reachable the manager still works, with an
 *    inactive session (the JS constructor threw).
 *  - After ShellGlobal::shutdown, and once the manager is disposed, the pending
 *    content type guesses are dropped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>

#include "misc/gnome-session.h"
#include "shell-global.h"
#include "ui/components/automount-manager.h"
#include "ui/components/autorun-manager.h"

#include "hotplug-sniffer-dbus.h"

/* GSettings keys */
#define SETTINGS_SCHEMA "org.gnome.desktop.media-handling"
#define SETTING_DISABLE_AUTORUN "autorun-never"
#define SETTING_START_APP "autorun-x-content-start-app"
#define SETTING_IGNORE "autorun-x-content-ignore"
#define SETTING_OPEN_FOLDER "autorun-x-content-open-folder"

#define HOTPLUG_SNIFFER_BUS_NAME "org.gnome.Shell.HotplugSniffer"
#define HOTPLUG_SNIFFER_OBJECT_PATH "/org/gnome/Shell/HotplugSniffer"

#define WIN32_SOFTWARE_CONTENT_TYPE "x-content/win32-software"
#define DIRECTORY_CONTENT_TYPE "inode/directory"

/* AutorunSetting */
typedef enum
{
  AUTORUN_SETTING_RUN = 0,
  AUTORUN_SETTING_IGNORE = 1,
  AUTORUN_SETTING_FILES = 2,
  AUTORUN_SETTING_ASK = 3,
} AutorunSetting;

enum
{
  PROP_0,

  PROP_MESSAGE_TRAY,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellAutorunManager
{
  GObject parent;

  ShellMessageTray *message_tray;     /* weak */
  ShellSessionManager *session;       /* nullable */
  GVolumeMonitor *volume_monitor;
  GSettings *settings;
  GCancellable *cancellable;
  GHashTable *notifications;          /* GMount -> ShellNotification */
  gboolean enabled;
  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellAutorunManager, shell_autorun_manager, G_TYPE_OBJECT)

/* ------------------------------------------------------------------------ */
/* Misc utils                                                                */
/* ------------------------------------------------------------------------ */

static gboolean
is_mount_root_hidden (GFile *root)
{
  g_autofree char *path = g_file_get_path (root);

  /* skip any mounts in hidden directory hierarchies */
  return path != NULL && strstr (path, "/.") != NULL;
}

static gboolean
should_autorun_mount (GMount *mount)
{
  g_autoptr (GFile) root = g_mount_get_root (mount);
  g_autoptr (GVolume) volume = g_mount_get_volume (mount);

  if (volume == NULL || !shell_automount_manager_volume_allows_autorun (volume))
    return FALSE;

  if (g_file_is_native (root) && is_mount_root_hidden (root))
    return FALSE;

  return TRUE;
}

static gboolean
is_mount_non_local (GMount *mount)
{
  g_autoptr (GVolume) volume = g_mount_get_volume (mount);
  g_autofree char *volume_class = NULL;

  /* If the mount doesn't have an associated volume, that means it's
   * an uninteresting filesystem. Most devices that we care about will
   * have a mount, like media players and USB sticks. */
  if (volume == NULL)
    return TRUE;

  volume_class = g_volume_get_identifier (volume, "class");
  return g_strcmp0 (volume_class, "network") == 0;
}

static gboolean
start_app_for_mount (GAppInfo *app,
                     GMount   *mount)
{
  g_autoptr (GFile) root = g_mount_get_root (mount);
  g_autoptr (GAppLaunchContext) context = NULL;
  g_autoptr (GError) error = NULL;
  GList files = { root, NULL, NULL };

  context = shell_global_create_app_launch_context (shell_global_get (), 0, -1);
  if (!g_app_info_launch (app, &files, context, &error))
    {
      g_message ("Unable to launch the app %s: %s",
                 g_app_info_get_name (app), error->message);
      return FALSE;
    }

  return TRUE;
}

static GAppInfo *
get_default_for_type (const char *content_type)
{
  return g_app_info_get_default_for_type (content_type, FALSE);
}

/* ------------------------------------------------------------------------ */
/* AutorunDispatcher                                                         */
/* ------------------------------------------------------------------------ */

typedef struct
{
  GAppInfo *app;
  GMount *mount;
} AppForMount;

static void
app_for_mount_free (AppForMount *data)
{
  g_clear_object (&data->app);
  g_clear_object (&data->mount);
  g_free (data);
}

static AppForMount *
app_for_mount_new (GAppInfo *app,
                   GMount   *mount)
{
  AppForMount *data = g_new0 (AppForMount, 1);

  /* @app is %NULL for the default file manager, which is looked up when the
   * notification is activated */
  data->app = app != NULL ? g_object_ref (app) : NULL;
  data->mount = g_object_ref (mount);
  return data;
}

static void
open_with_action_cb (ShellNotification *notification,
                     gpointer           user_data)
{
  AppForMount *data = user_data;

  start_app_for_mount (data->app, data->mount);
}

static void
open_folder_activated_cb (ShellNotification *notification,
                          AppForMount       *data)
{
  g_autoptr (GAppInfo) app = get_default_for_type (DIRECTORY_CONTENT_TYPE);

  if (app != NULL)
    start_app_for_mount (app, data->mount);
}

static void
notification_destroy_cb (ShellNotification                *notification,
                         ShellNotificationDestroyedReason  reason,
                         ShellAutorunManager              *self)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, self->notifications);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (value == notification)
        {
          g_hash_table_iter_remove (&iter);
          return;
        }
    }
}

static AutorunSetting
get_autorun_setting_for_type (ShellAutorunManager *self,
                              const char          *content_type)
{
  g_auto (GStrv) run_app = g_settings_get_strv (self->settings, SETTING_START_APP);
  g_auto (GStrv) ignore = NULL;
  g_auto (GStrv) open_files = NULL;

  if (g_strv_contains ((const char * const *) run_app, content_type))
    return AUTORUN_SETTING_RUN;

  ignore = g_settings_get_strv (self->settings, SETTING_IGNORE);
  if (g_strv_contains ((const char * const *) ignore, content_type))
    return AUTORUN_SETTING_IGNORE;

  open_files = g_settings_get_strv (self->settings, SETTING_OPEN_FOLDER);
  if (g_strv_contains ((const char * const *) open_files, content_type))
    return AUTORUN_SETTING_FILES;

  return AUTORUN_SETTING_ASK;
}

static void
dispatcher_add_notification (ShellAutorunManager *self,
                             GMount              *mount,
                             GPtrArray           *apps)
{
  ShellSource *source;
  g_autoptr (ShellNotification) notification = NULL;
  g_autofree char *name = NULL;
  g_autofree char *title = NULL;
  guint i;

  /* Only show a new notification if there isn't already an existing one */
  if (g_hash_table_contains (self->notifications, mount))
    return;

  if (self->message_tray == NULL)
    return;

  source = shell_message_tray_get_system_source (self->message_tray);

  name = g_mount_get_name (mount);
  /* Translators: %s is the name of a partition on a external drive */
  title = g_strdup_printf (_("“%s” connected"), name);

  notification = shell_notification_new (source, title, _("Disk can now be used"));
  g_signal_connect_data (notification, "activated",
                         G_CALLBACK (open_folder_activated_cb),
                         app_for_mount_new (NULL, mount),
                         (GClosureNotify) app_for_mount_free, 0);

  for (i = 0; i < apps->len; i++)
    {
      GAppInfo *app = g_ptr_array_index (apps, i);
      g_autofree char *label = g_strdup_printf (_("Open with %s"), g_app_info_get_name (app));

      shell_notification_add_action (notification, label, open_with_action_cb,
                                     app_for_mount_new (app, mount),
                                     (GDestroyNotify) app_for_mount_free);
    }

  g_signal_connect (notification, "destroy",
                    G_CALLBACK (notification_destroy_cb), self);
  g_hash_table_insert (self->notifications, g_object_ref (mount),
                       g_object_ref (notification));
  shell_source_add_notification (source, notification);
}

static void
dispatcher_add_mount (ShellAutorunManager *self,
                      GMount              *mount,
                      GPtrArray           *apps,
                      const char * const  *content_types)
{
  AutorunSetting setting;
  g_autoptr (GAppInfo) app = NULL;
  gboolean success = FALSE;

  /* if autorun is disabled globally, return */
  if (g_settings_get_boolean (self->settings, SETTING_DISABLE_AUTORUN))
    return;

  /* if the mount doesn't want to be autorun, return */
  if (!should_autorun_mount (mount))
    return;

  if (content_types[0] != NULL)
    setting = get_autorun_setting_for_type (self, content_types[0]);
  else
    setting = AUTORUN_SETTING_ASK;

  /* check at the settings for the first content type
   * to see whether we should ask */
  if (setting == AUTORUN_SETTING_IGNORE)
    return; /* return right away */

  if (setting == AUTORUN_SETTING_RUN)
    app = get_default_for_type (content_types[0]);
  else if (setting == AUTORUN_SETTING_FILES)
    app = get_default_for_type (DIRECTORY_CONTENT_TYPE);

  if (app != NULL)
    success = start_app_for_mount (app, mount);

  /* we fallback here also in case the settings did not specify 'ask',
   * but we failed launching the default app or the default file manager */
  if (!success)
    dispatcher_add_notification (self, mount, apps);
}

static void
dispatcher_remove_mount (ShellAutorunManager *self,
                         GMount              *mount)
{
  ShellNotification *notification = g_hash_table_lookup (self->notifications, mount);

  if (notification != NULL)
    {
      g_autoptr (ShellNotification) keep_alive = g_object_ref (notification);

      shell_notification_destroy (keep_alive, SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
    }
}

/* ------------------------------------------------------------------------ */
/* ContentTypeDiscoverer                                                     */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellAutorunManager *self;
  GMount *mount;
  char **content_types;     /* the types guessed so far, %NULL-terminated */
} GuessData;

static void
guess_data_free (GuessData *data)
{
  g_clear_object (&data->self);
  g_clear_object (&data->mount);
  g_clear_pointer (&data->content_types, g_strfreev);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GuessData, guess_data_free)

/* Turns the content types into the apps for them and calls the dispatcher */
static void
guess_finished (GuessData *data)
{
  ShellAutorunManager *self = data->self;
  g_autoptr (GPtrArray) apps = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
  g_auto (GStrv) content_types = NULL;
  guint i;

  /* we're not interested in win32 software content types here */
  for (i = 0; data->content_types != NULL && data->content_types[i] != NULL; i++)
    {
      if (g_strcmp0 (data->content_types[i], WIN32_SOFTWARE_CONTENT_TYPE) != 0)
        g_strv_builder_add (builder, data->content_types[i]);
    }
  content_types = g_strv_builder_end (builder);

  for (i = 0; content_types[i] != NULL; i++)
    {
      GAppInfo *app = get_default_for_type (content_types[i]);

      if (app != NULL)
        g_ptr_array_add (apps, app);
    }

  if (apps->len == 0)
    {
      GAppInfo *app = get_default_for_type (DIRECTORY_CONTENT_TYPE);

      if (app != NULL)
        g_ptr_array_add (apps, app);
    }

  dispatcher_add_mount (self, data->mount, apps, (const char * const *) content_types);
}

static void
sniff_uri_cb (GObject      *source_object,
              GAsyncResult *res,
              gpointer      user_data)
{
  g_autoptr (GuessData) data = user_data;
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) content_types = NULL;

  if (!shell_hotplug_sniffer_call_sniff_uri_finish (SHELL_HOTPLUG_SNIFFER (source_object),
                                                    &content_types, res, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_autofree char *name = g_mount_get_name (data->mount);

          g_warning ("Unable to sniff the content types of the mount %s: %s",
                     name, error->message);
        }
      return;
    }

  if (data->self->shutting_down)
    return;

  g_clear_pointer (&data->content_types, g_strfreev);
  data->content_types = g_steal_pointer (&content_types);
  guess_finished (data);
}

static void
sniffer_proxy_ready_cb (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  g_autoptr (GuessData) data = user_data;
  g_autoptr (ShellHotplugSniffer) sniffer = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) root = NULL;
  g_autofree char *uri = NULL;

  sniffer = shell_hotplug_sniffer_proxy_new_for_bus_finish (res, &error);
  if (sniffer == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_autofree char *name = g_mount_get_name (data->mount);

          g_warning ("Unable to sniff the content types of the mount %s: %s",
                     name, error->message);
        }
      return;
    }

  if (data->self->shutting_down)
    return;

  root = g_mount_get_root (data->mount);
  uri = g_file_get_uri (root);
  shell_hotplug_sniffer_call_sniff_uri (sniffer, uri, data->self->cancellable,
                                        sniff_uri_cb, g_steal_pointer (&data));
}

static void
sniff_content_types (GuessData *data)
{
  shell_hotplug_sniffer_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                           G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                                           G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                           HOTPLUG_SNIFFER_BUS_NAME,
                                           HOTPLUG_SNIFFER_OBJECT_PATH,
                                           data->self->cancellable,
                                           sniffer_proxy_ready_cb,
                                           data);
}

static void
guess_content_type_cb (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  GuessData *data = user_data;
  g_autoptr (GError) error = NULL;

  data->content_types = g_mount_guess_content_type_finish (G_MOUNT (source_object),
                                                           res, &error);
  if (error != NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          guess_data_free (data);
          return;
        }

      g_autofree char *name = g_mount_get_name (data->mount);

      g_message ("Unable to guess content types on added mount %s: %s",
                 name, error->message);
    }

  if (data->self->shutting_down)
    {
      guess_data_free (data);
      return;
    }

  if (data->content_types == NULL || data->content_types[0] == NULL)
    {
      sniff_content_types (data);
      return;
    }

  guess_finished (data);
  guess_data_free (data);
}

/* ContentTypeDiscoverer.guessContentTypes () and the dispatch of the mount */
static void
guess_content_types (ShellAutorunManager *self,
                     GMount              *mount)
{
  gboolean autorun_enabled = !g_settings_get_boolean (self->settings, SETTING_DISABLE_AUTORUN);
  gboolean should_scan = autorun_enabled && !is_mount_non_local (mount);
  GuessData *data = g_new0 (GuessData, 1);

  data->self = g_object_ref (self);
  data->mount = g_object_ref (mount);

  if (should_scan)
    {
      g_mount_guess_content_type (mount, FALSE, self->cancellable,
                                  guess_content_type_cb, data);
    }
  else
    {
      guess_finished (data);
      guess_data_free (data);
    }
}

/* ------------------------------------------------------------------------ */
/* ShellAutorunManager                                                       */
/* ------------------------------------------------------------------------ */

static void
on_mount_added (GVolumeMonitor      *monitor,
                GMount              *mount,
                ShellAutorunManager *self)
{
  /* don't do anything if our session is not the currently
   * active one */
  if (self->session == NULL || !shell_session_manager_get_session_is_active (self->session))
    return;

  guess_content_types (self, mount);
}

static void
on_mount_removed (GVolumeMonitor      *monitor,
                  GMount              *mount,
                  ShellAutorunManager *self)
{
  dispatcher_remove_mount (self, mount);
}

/**
 * shell_autorun_manager_enable:
 * @self: a #ShellAutorunManager
 *
 * Starts to follow the mounts that are added and removed (`enable ()`).
 */
void
shell_autorun_manager_enable (ShellAutorunManager *self)
{
  g_return_if_fail (SHELL_IS_AUTORUN_MANAGER (self));

  if (self->enabled || self->shutting_down)
    return;

  self->enabled = TRUE;
  g_signal_connect (self->volume_monitor, "mount-added",
                    G_CALLBACK (on_mount_added), self);
  g_signal_connect (self->volume_monitor, "mount-removed",
                    G_CALLBACK (on_mount_removed), self);
}

/**
 * shell_autorun_manager_disable:
 * @self: a #ShellAutorunManager
 *
 * Stops following the mounts (`disable ()`).
 */
void
shell_autorun_manager_disable (ShellAutorunManager *self)
{
  g_return_if_fail (SHELL_IS_AUTORUN_MANAGER (self));

  if (!self->enabled)
    return;

  self->enabled = FALSE;
  g_signal_handlers_disconnect_by_data (self->volume_monitor, self);
}

static void
on_shutdown (ShellGlobal        *global,
             ShellAutorunManager *self)
{
  self->shutting_down = TRUE;
  g_cancellable_cancel (self->cancellable);
}

static void
shell_autorun_manager_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  ShellAutorunManager *self = SHELL_AUTORUN_MANAGER (object);

  switch (prop_id)
    {
    case PROP_MESSAGE_TRAY:
      g_value_set_object (value, self->message_tray);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_autorun_manager_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  ShellAutorunManager *self = SHELL_AUTORUN_MANAGER (object);

  switch (prop_id)
    {
    case PROP_MESSAGE_TRAY:
      g_set_weak_pointer (&self->message_tray, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_autorun_manager_constructed (GObject *object)
{
  ShellAutorunManager *self = SHELL_AUTORUN_MANAGER (object);
  g_autoptr (GError) error = NULL;

  G_OBJECT_CLASS (shell_autorun_manager_parent_class)->constructed (object);

  self->settings = g_settings_new (SETTINGS_SCHEMA);
  self->cancellable = g_cancellable_new ();

  self->session = shell_gnome_session_manager_new_sync (NULL, &error);
  if (self->session == NULL)
    g_message ("Unable to connect to gnome-session: %s", error->message);

  self->volume_monitor = g_volume_monitor_get ();

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);
}

static void
shell_autorun_manager_dispose (GObject *object)
{
  ShellAutorunManager *self = SHELL_AUTORUN_MANAGER (object);
  GHashTableIter iter;
  gpointer key, value;

  self->shutting_down = TRUE;

  if (self->volume_monitor != NULL)
    g_signal_handlers_disconnect_by_data (self->volume_monitor, self);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  if (self->notifications != NULL)
    {
      g_hash_table_iter_init (&iter, self->notifications);
      while (g_hash_table_iter_next (&iter, &key, &value))
        g_signal_handlers_disconnect_by_data (value, self);
      g_clear_pointer (&self->notifications, g_hash_table_unref);
    }

  g_clear_object (&self->cancellable);
  g_clear_object (&self->session);
  g_clear_object (&self->volume_monitor);
  g_clear_object (&self->settings);
  g_clear_weak_pointer (&self->message_tray);

  G_OBJECT_CLASS (shell_autorun_manager_parent_class)->dispose (object);
}

static void
shell_autorun_manager_class_init (ShellAutorunManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_autorun_manager_get_property;
  object_class->set_property = shell_autorun_manager_set_property;
  object_class->constructed = shell_autorun_manager_constructed;
  object_class->dispose = shell_autorun_manager_dispose;

  /**
   * ShellAutorunManager:message-tray:
   *
   * The message tray whose system source gets the notifications that ask what
   * to do with a mount (`Main.messageTray`).
   */
  props[PROP_MESSAGE_TRAY] =
    g_param_spec_object ("message-tray", NULL, NULL,
                         SHELL_TYPE_MESSAGE_TRAY,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_autorun_manager_init (ShellAutorunManager *self)
{
  self->notifications = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                               g_object_unref, g_object_unref);
}

/**
 * shell_autorun_manager_new:
 * @message_tray: the message tray to notify with
 *
 * Creates the manager; it does nothing until shell_autorun_manager_enable ()
 * (`new AutorunManager ()`).
 *
 * Returns: (transfer full): a new #ShellAutorunManager
 */
ShellAutorunManager *
shell_autorun_manager_new (ShellMessageTray *message_tray)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_TRAY (message_tray), NULL);

  return g_object_new (SHELL_TYPE_AUTORUN_MANAGER,
                       "message-tray", message_tray,
                       NULL);
}
