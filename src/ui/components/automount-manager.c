/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * automount-manager.c: Mounts the volumes of the removable media when they
 * are added. Port of js/ui/components/automountManager.js.
 *
 * The dialogs that ask for a password or a choice during the mount are
 * created by #ShellMountOperationUI (js/ui/shellMountOperation.js): the
 * manager holds one for each volume that is being mounted, mounts the volume
 * with its mount operation, asks it for the password dialog it showed (to
 * reuse it when the password was wrong) and closes it when the mount is over.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The `allowAutorun` expando of the volumes is read through
 *    shell_automount_manager_volume_allows_autorun () (autorun-manager.c).
 *  - If gnome-session is not reachable the manager still works, with an
 *    inactive session (the JS constructor threw and the component was not
 *    enabled).
 *  - A mount that was cancelled (the component was disabled) does not log
 *    "Unable to mount volume"; after ShellGlobal::shutdown the completions do
 *    nothing.
 *  - The create-mount-operation, close-mount-operation and borrow-dialog
 *    signals which connected the JS dialogs through js/ui/components.js are
 *    gone.
 *  - The entry of a volume that was mounted without a mount operation is
 *    removed from the table of the active operations.
 *  - The expiry timeouts of `allowAutorun` are dropped when the manager is
 *    disposed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>
#include <meta/display.h>
#include <meta/meta-sound-player.h>

#include "misc/gnome-session.h"
#include "shell-global.h"
#include "ui/components/automount-manager.h"
#include "ui/shell-mount-operation-ui.h"

#define SETTINGS_SCHEMA "org.gnome.desktop.media-handling"
#define SETTING_ENABLE_AUTOMOUNT "automount"

#define AUTORUN_EXPIRE_TIMEOUT_SECS 10

#define ALLOW_AUTORUN_KEY "shell-allow-autorun"

struct _ShellAutomountManager
{
  GObject parent;

  GSettings *settings;
  GHashTable *active_operations;      /* GVolume -> ShellMountOperationUI (nullable) */
  GHashTable *expire_ids;             /* GVolume -> source id */
  ShellSessionManager *session;       /* nullable */
  GCancellable *inhibit_cancellable;
  gboolean inhibited;

  GVolumeMonitor *volume_monitor;
  GCancellable *cancellable;          /* set while enabled */
  guint mount_all_id;
  gboolean enabled;
  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellAutomountManager, shell_automount_manager, G_TYPE_OBJECT)

typedef struct
{
  gboolean check_session;
  gboolean use_mount_op;
  gboolean allow_autorun;
} CheckParams;

/* ------------------------------------------------------------------------ */
/* allowAutorun                                                              */
/* ------------------------------------------------------------------------ */

/**
 * shell_automount_manager_volume_allows_autorun:
 * @volume: a #GVolume
 *
 * Returns: whether the autorun manager may run the autorun for @volume: the
 *   volume was just mounted by the automount manager, or the autorun was
 *   allowed otherwise, and the allowance did not expire (`volume.allowAutorun`)
 */
gboolean
shell_automount_manager_volume_allows_autorun (GVolume *volume)
{
  g_return_val_if_fail (G_IS_VOLUME (volume), FALSE);

  return g_object_get_data (G_OBJECT (volume), ALLOW_AUTORUN_KEY) != NULL;
}

static void
set_allow_autorun (GVolume  *volume,
                   gboolean  allow)
{
  g_object_set_data (G_OBJECT (volume), ALLOW_AUTORUN_KEY,
                     allow ? GINT_TO_POINTER (TRUE) : NULL);
}

typedef struct
{
  ShellAutomountManager *self;     /* the timeout is removed in dispose () */
  GVolume *volume;
} ExpireData;

static void
expire_data_free (ExpireData *data)
{
  g_clear_object (&data->volume);
  g_free (data);
}

static gboolean
allow_autorun_expire_cb (gpointer user_data)
{
  ExpireData *data = user_data;
  gpointer key;

  set_allow_autorun (data->volume, FALSE);

  /* This source is being dispatched: forget its id without removing it */
  if (g_hash_table_steal_extended (data->self->expire_ids, data->volume, &key, NULL))
    g_object_unref (key);

  return G_SOURCE_REMOVE;
}

static void
allow_autorun_expire (ShellAutomountManager *self,
                      GVolume               *volume)
{
  ExpireData *data = g_new0 (ExpireData, 1);
  guint id;

  data->self = self;
  data->volume = g_object_ref (volume);

  /* The JS code did not remove an id it replaced: do */
  g_hash_table_remove (self->expire_ids, volume);

  id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, AUTORUN_EXPIRE_TIMEOUT_SECS,
                                   allow_autorun_expire_cb, data,
                                   (GDestroyNotify) expire_data_free);
  g_source_set_name_by_id (id, "[gnome-shell] volume.allowAutorun");
  g_hash_table_insert (self->expire_ids, g_object_ref (volume), GUINT_TO_POINTER (id));
}

static void
remove_expire_source (gpointer value)
{
  g_source_remove (GPOINTER_TO_UINT (value));
}

/* ------------------------------------------------------------------------ */
/* The session                                                               */
/* ------------------------------------------------------------------------ */

static gboolean
session_is_active (ShellAutomountManager *self)
{
  return self->session != NULL &&
         shell_session_manager_get_session_is_active (self->session);
}

static void
is_inhibited_cb (GObject      *source_object,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  g_autoptr (ShellAutomountManager) self = user_data;
  gboolean inhibited = FALSE;

  if (shell_session_manager_call_is_inhibited_finish (SHELL_SESSION_MANAGER (source_object),
                                                      &inhibited, res, NULL))
    self->inhibited = inhibited;
}

static void
inhibitors_changed_cb (ShellSessionManager    *session,
                       const char             *inhibitor,
                       ShellAutomountManager  *self)
{
  if (self->shutting_down)
    return;

  shell_session_manager_call_is_inhibited (session,
                                           SHELL_INHIBIT_FLAGS_AUTOMOUNT,
                                           self->inhibit_cancellable,
                                           is_inhibited_cb,
                                           g_object_ref (self));
}

/* ------------------------------------------------------------------------ */
/* The mount operations                                                      */
/* ------------------------------------------------------------------------ */

static void
close_operation (ShellAutomountManager *self,
                 GVolume               *volume)
{
  g_autoptr (ShellMountOperationUI) operation = NULL;
  gpointer value;

  if (!g_hash_table_lookup_extended (self->active_operations, volume, NULL, &value))
    return;

  if (value != NULL)
    operation = g_object_ref (value);
  g_hash_table_remove (self->active_operations, volume);

  if (operation != NULL && !self->shutting_down)
    shell_mount_operation_ui_close (operation);
}

static void
on_volume_mounted (GObject      *source_object,
                   GAsyncResult *res,
                   gpointer      user_data);

typedef struct
{
  ShellAutomountManager *self;
} MountData;

static void
mount_volume (ShellAutomountManager *self,
              GVolume               *volume,
              ShellMountOperationUI *operation,
              gboolean               allow_autorun)
{
  if (allow_autorun)
    set_allow_autorun (volume, TRUE);

  g_hash_table_insert (self->active_operations, g_object_ref (volume),
                       operation != NULL ? g_object_ref (operation) : NULL);

  g_volume_mount (volume, G_MOUNT_MOUNT_NONE,
                  operation != NULL ? shell_mount_operation_ui_get_mount_op (operation) : NULL,
                  self->cancellable,
                  on_volume_mounted, g_object_ref (self));
}

static void
reask_password (ShellAutomountManager *self,
                GVolume               *volume)
{
  g_autoptr (ShellModalDialog) existing_dialog = NULL;
  g_autoptr (ShellMountOperationUI) operation = NULL;
  gpointer previous = NULL;

  g_hash_table_lookup_extended (self->active_operations, volume, NULL, &previous);
  if (previous != NULL)
    existing_dialog = shell_mount_operation_ui_borrow_dialog (previous);

  operation = shell_mount_operation_ui_new (volume, existing_dialog);
  mount_volume (self, volume, operation, FALSE);
}

static void
on_volume_mounted (GObject      *source_object,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  GVolume *volume = G_VOLUME (source_object);
  g_autoptr (ShellAutomountManager) self = user_data;
  g_autoptr (GError) error = NULL;
  const char *message;

  if (self->shutting_down)
    return;

  allow_autorun_expire (self, volume);

  if (g_volume_mount_finish (volume, res, &error))
    {
      close_operation (self, volume);
      return;
    }

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      close_operation (self, volume);
      return;
    }

  /* FIXME: we will always get G_IO_ERROR_FAILED from the gvfs udisks
   * backend, see https://bugs.freedesktop.org/show_bug.cgi?id=51271
   * To reask the password if the user input was empty or wrong, we
   * will check for corresponding error messages. However, these
   * error strings are not unique for the cases in the comments below. */
  message = error->message;
  if (strstr (message, "No key available with this passphrase") != NULL ||  /* cryptsetup */
      strstr (message, "No key available to unlock device") != NULL ||      /* udisks (no password) */
      /* libblockdev wrong password opening LUKS device */
      strstr (message, "Failed to activate device: Incorrect passphrase") != NULL ||
      /* cryptsetup returns EINVAL (< v2.5.0) or EPERM (>= v2.5.0)
       * when the TCRYPT header can't be decrypted with the provided
       * password/parameters. */
      strstr (message, "Failed to load device's parameters: Invalid argument") != NULL ||
      strstr (message, "Failed to load device's parameters: Operation not permitted") != NULL)
    {
      reask_password (self, volume);
    }
  else
    {
      if (strstr (message, "Compiled against a version of libcryptsetup that does not support the VeraCrypt PIM setting") != NULL)
        {
          shell_global_notify_error (shell_global_get (),
                                     _("Unable to unlock volume"),
                                     _("The installed udisks version does not support the PIM setting"));
        }

      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
        {
          g_autofree char *name = g_volume_get_name (volume);

          g_message ("Unable to mount volume %s: %s", name, message);
        }

      close_operation (self, volume);
    }
}

/* ------------------------------------------------------------------------ */
/* Mounting                                                                  */
/* ------------------------------------------------------------------------ */

static void
check_and_mount_volume (ShellAutomountManager *self,
                        GVolume               *volume,
                        const CheckParams     *params)
{
  g_autoptr (GMount) mount = NULL;

  if (params->check_session)
    {
      /* if we're not in the current ConsoleKit session,
       * don't attempt automount */
      if (!session_is_active (self))
        return;
    }

  if (self->inhibited)
    return;

  /* Volume is already mounted, don't bother. */
  mount = g_volume_get_mount (volume);
  if (mount != NULL)
    return;

  if (!g_settings_get_boolean (self->settings, SETTING_ENABLE_AUTOMOUNT) ||
      !g_volume_should_automount (volume) ||
      !g_volume_can_mount (volume))
    {
      /* allow the autorun to run anyway; this can happen if the
       * mount gets added programmatically later, even if
       * should_automount() or can_mount() are false, like for
       * blank optical media. */
      set_allow_autorun (volume, TRUE);
      allow_autorun_expire (self, volume);

      return;
    }

  if (params->use_mount_op)
    {
      g_autoptr (ShellMountOperationUI) operation = shell_mount_operation_ui_new (volume, NULL);

      mount_volume (self, volume, operation, params->allow_autorun);
    }
  else
    {
      mount_volume (self, volume, NULL, params->allow_autorun);
    }
}

static void
startup_mount_all (gpointer user_data)
{
  ShellAutomountManager *self = user_data;
  const CheckParams params = {
    .check_session = FALSE,
    .use_mount_op = FALSE,
    .allow_autorun = FALSE,
  };
  g_autolist (GVolume) volumes = NULL;
  GList *l;

  self->mount_all_id = 0;

  volumes = g_volume_monitor_get_volumes (self->volume_monitor);
  for (l = volumes; l != NULL; l = l->next)
    check_and_mount_volume (self, l->data, &params);
}

static void
on_volume_added (GVolumeMonitor        *monitor,
                 GVolume               *volume,
                 ShellAutomountManager *self)
{
  const CheckParams params = {
    .check_session = TRUE,
    .use_mount_op = TRUE,
    .allow_autorun = TRUE,
  };

  check_and_mount_volume (self, volume, &params);
}

static void
on_volume_removed (GVolumeMonitor        *monitor,
                   GVolume               *volume,
                   ShellAutomountManager *self)
{
  g_hash_table_remove (self->expire_ids, volume);
}

static void
play_drive_sound (ShellAutomountManager *self,
                  const char            *file_name,
                  const char            *description)
{
  MetaSoundPlayer *player;
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;

  /* if we're not in the current ConsoleKit session,
   * or screensaver is active, don't play sounds */
  if (!session_is_active (self) || self->shutting_down)
    return;

  player = meta_display_get_sound_player (shell_global_get_display (shell_global_get ()));
  if (player == NULL)
    return;

  path = g_build_filename (shell_global_get_datadir (shell_global_get ()), "sounds", file_name, NULL);
  file = g_file_new_for_path (path);
  meta_sound_player_play_from_file (player, file, description, NULL);
}

static void
on_drive_connected (GVolumeMonitor        *monitor,
                    GDrive                *drive,
                    ShellAutomountManager *self)
{
  play_drive_sound (self, "device-added.oga", _("External drive connected"));
}

static void
on_drive_disconnected (GVolumeMonitor        *monitor,
                       GDrive                *drive,
                       ShellAutomountManager *self)
{
  play_drive_sound (self, "device-removed.oga", _("External drive disconnected"));
}

static void
drive_stop_cb (GObject      *source_object,
               GAsyncResult *res,
               gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!g_drive_stop_finish (G_DRIVE (source_object), res, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("Unable to stop the drive after drive-eject-button %s", error->message);
}

static void
drive_eject_cb (GObject      *source_object,
                GAsyncResult *res,
                gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!g_drive_eject_with_operation_finish (G_DRIVE (source_object), res, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("Unable to eject the drive after drive-eject-button %s", error->message);
}

static void
on_drive_eject_button (GVolumeMonitor        *monitor,
                       GDrive                *drive,
                       ShellAutomountManager *self)
{
  /* TODO: this code path is not tested, as the GVfs volume monitor
   * doesn't emit this signal just yet. */
  if (!session_is_active (self) || self->shutting_down)
    return;

  /* we force stop/eject in this case, so we don't have to pass a
   * mount operation object */
  if (g_drive_can_stop (drive))
    g_drive_stop (drive, G_MOUNT_UNMOUNT_FORCE, NULL, NULL, drive_stop_cb, NULL);
  else if (g_drive_can_eject (drive))
    g_drive_eject_with_operation (drive, G_MOUNT_UNMOUNT_FORCE, NULL, NULL, drive_eject_cb, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellAutomountManager                                                     */
/* ------------------------------------------------------------------------ */

static void
on_shutdown (ShellGlobal           *global,
             ShellAutomountManager *self)
{
  self->shutting_down = TRUE;

  g_cancellable_cancel (self->inhibit_cancellable);
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_handle_id (&self->mount_all_id, g_source_remove);
  g_hash_table_remove_all (self->expire_ids);
}

/**
 * shell_automount_manager_enable:
 * @self: a #ShellAutomountManager
 *
 * Starts to mount the volumes that are added, and mounts the existing ones
 * (`enable ()`).
 */
void
shell_automount_manager_enable (ShellAutomountManager *self)
{
  g_return_if_fail (SHELL_IS_AUTOMOUNT_MANAGER (self));

  if (self->enabled || self->shutting_down)
    return;

  self->enabled = TRUE;
  self->cancellable = g_cancellable_new ();

  g_signal_connect (self->volume_monitor, "volume-added",
                    G_CALLBACK (on_volume_added), self);
  g_signal_connect (self->volume_monitor, "volume-removed",
                    G_CALLBACK (on_volume_removed), self);
  g_signal_connect (self->volume_monitor, "drive-connected",
                    G_CALLBACK (on_drive_connected), self);
  g_signal_connect (self->volume_monitor, "drive-disconnected",
                    G_CALLBACK (on_drive_disconnected), self);
  g_signal_connect (self->volume_monitor, "drive-eject-button",
                    G_CALLBACK (on_drive_eject_button), self);

  self->mount_all_id = g_idle_add_once (startup_mount_all, self);
  g_source_set_name_by_id (self->mount_all_id, "[gnome-shell] startup_mount_all");
}

/**
 * shell_automount_manager_disable:
 * @self: a #ShellAutomountManager
 *
 * Stops mounting the volumes and cancels the mounts in progress
 * (`disable ()`).
 */
void
shell_automount_manager_disable (ShellAutomountManager *self)
{
  g_return_if_fail (SHELL_IS_AUTOMOUNT_MANAGER (self));

  if (!self->enabled)
    return;

  self->enabled = FALSE;

  if (self->cancellable != NULL)
    {
      g_cancellable_cancel (self->cancellable);
      g_clear_object (&self->cancellable);
    }

  g_signal_handlers_disconnect_by_data (self->volume_monitor, self);
  g_clear_handle_id (&self->mount_all_id, g_source_remove);
}

static void
unref_if_set (gpointer object)
{
  g_clear_object (&object);
}

static void
shell_automount_manager_constructed (GObject *object)
{
  ShellAutomountManager *self = SHELL_AUTOMOUNT_MANAGER (object);
  g_autoptr (GError) error = NULL;

  G_OBJECT_CLASS (shell_automount_manager_parent_class)->constructed (object);

  self->settings = g_settings_new (SETTINGS_SCHEMA);
  self->inhibit_cancellable = g_cancellable_new ();

  self->session = shell_gnome_session_manager_new_sync (NULL, &error);
  if (self->session != NULL)
    {
      g_signal_connect (self->session, "inhibitor-added",
                        G_CALLBACK (inhibitors_changed_cb), self);
      g_signal_connect (self->session, "inhibitor-removed",
                        G_CALLBACK (inhibitors_changed_cb), self);
    }
  else
    {
      g_message ("Unable to connect to gnome-session: %s", error->message);
    }

  self->volume_monitor = g_volume_monitor_get ();

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);
}

static void
shell_automount_manager_dispose (GObject *object)
{
  ShellAutomountManager *self = SHELL_AUTOMOUNT_MANAGER (object);

  self->shutting_down = TRUE;

  if (self->volume_monitor != NULL)
    g_signal_handlers_disconnect_by_data (self->volume_monitor, self);
  if (self->session != NULL)
    g_signal_handlers_disconnect_by_data (self->session, self);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  if (self->inhibit_cancellable != NULL)
    g_cancellable_cancel (self->inhibit_cancellable);
  g_clear_handle_id (&self->mount_all_id, g_source_remove);

  g_clear_pointer (&self->expire_ids, g_hash_table_unref);
  g_clear_pointer (&self->active_operations, g_hash_table_unref);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->inhibit_cancellable);
  g_clear_object (&self->session);
  g_clear_object (&self->volume_monitor);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (shell_automount_manager_parent_class)->dispose (object);
}

static void
shell_automount_manager_class_init (ShellAutomountManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_automount_manager_constructed;
  object_class->dispose = shell_automount_manager_dispose;
}

static void
shell_automount_manager_init (ShellAutomountManager *self)
{
  self->active_operations = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                   g_object_unref, unref_if_set);
  self->expire_ids = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                            g_object_unref, remove_expire_source);
}

/**
 * shell_automount_manager_new:
 *
 * Creates the manager; it does nothing until shell_automount_manager_enable ()
 * (`new AutomountManager ()`).
 *
 * Returns: (transfer full): a new #ShellAutomountManager
 */
ShellAutomountManager *
shell_automount_manager_new (void)
{
  return g_object_new (SHELL_TYPE_AUTOMOUNT_MANAGER, NULL);
}
