/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * fido2-token-manager.c: Tracks the FIDO2 security keys that are plugged in.
 * Port of js/gdm/fido2TokenManager.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <gudev/gudev.h>

#include "gdm/fido2-token-manager.h"

enum
{
  FIDO2_TOKEN_INSERTED,
  FIDO2_TOKEN_REMOVED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellFido2TokenManager
{
  GObject parent;

  GUdevClient *udev_client;
  GHashTable *inserted_tokens;   /* sysfs path -> GUdevDevice */
};

G_DEFINE_FINAL_TYPE (ShellFido2TokenManager, shell_fido2_token_manager, G_TYPE_OBJECT)

static void
add_token (ShellFido2TokenManager *self,
           GUdevDevice            *device)
{
  const char *sysfs_path = g_udev_device_get_sysfs_path (device);
  gboolean is_fido = g_udev_device_get_property_as_int (device, "ID_FIDO_TOKEN") == 1;

  if (sysfs_path == NULL || !is_fido ||
      g_hash_table_contains (self->inserted_tokens, sysfs_path))
    return;

  g_hash_table_insert (self->inserted_tokens, g_strdup (sysfs_path),
                       g_object_ref (device));
  g_signal_emit (self, signals[FIDO2_TOKEN_INSERTED], 0, device);
}

static void
remove_token (ShellFido2TokenManager *self,
              GUdevDevice            *device)
{
  const char *sysfs_path = g_udev_device_get_sysfs_path (device);

  if (sysfs_path == NULL || !g_hash_table_contains (self->inserted_tokens, sysfs_path))
    return;

  g_hash_table_remove (self->inserted_tokens, sysfs_path);
  g_signal_emit (self, signals[FIDO2_TOKEN_REMOVED], 0, device);
}

static void
on_uevent (GUdevClient            *client,
           const char             *action,
           GUdevDevice            *device,
           ShellFido2TokenManager *self)
{
  if (g_strcmp0 (action, "add") == 0)
    add_token (self, device);
  else if (g_strcmp0 (action, "remove") == 0)
    remove_token (self, device);
}

static void
shell_fido2_token_manager_dispose (GObject *object)
{
  ShellFido2TokenManager *self = SHELL_FIDO2_TOKEN_MANAGER (object);

  if (self->udev_client != NULL)
    g_signal_handlers_disconnect_by_data (self->udev_client, self);
  g_clear_object (&self->udev_client);
  g_clear_pointer (&self->inserted_tokens, g_hash_table_unref);

  G_OBJECT_CLASS (shell_fido2_token_manager_parent_class)->dispose (object);
}

static void
shell_fido2_token_manager_class_init (ShellFido2TokenManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_fido2_token_manager_dispose;

  /**
   * ShellFido2TokenManager::fido2-token-inserted:
   * @manager: the #ShellFido2TokenManager
   * @device: the #GUdevDevice of the token
   *
   * Emitted when a security key is plugged in.
   */
  signals[FIDO2_TOKEN_INSERTED] =
    g_signal_new ("fido2-token-inserted",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_UDEV_TYPE_DEVICE);

  /**
   * ShellFido2TokenManager::fido2-token-removed:
   * @manager: the #ShellFido2TokenManager
   * @device: the #GUdevDevice of the token
   *
   * Emitted when a security key is unplugged.
   */
  signals[FIDO2_TOKEN_REMOVED] =
    g_signal_new ("fido2-token-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_UDEV_TYPE_DEVICE);
}

static void
shell_fido2_token_manager_init (ShellFido2TokenManager *self)
{
  const char * const subsystems[] = { "hidraw", NULL };
  GList *devices, *l;

  self->inserted_tokens = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, g_object_unref);
  self->udev_client = g_udev_client_new (subsystems);

  devices = g_udev_client_query_by_subsystem (self->udev_client, "hidraw");
  for (l = devices; l != NULL; l = l->next)
    add_token (self, l->data);
  g_list_free_full (devices, g_object_unref);

  g_signal_connect (self->udev_client, "uevent", G_CALLBACK (on_uevent), self);
}

/**
 * shell_fido2_token_manager_get_default:
 *
 * Gets the FIDO2 token manager, creating it if needed.
 *
 * Returns: (transfer none): the #ShellFido2TokenManager
 */
ShellFido2TokenManager *
shell_fido2_token_manager_get_default (void)
{
  static ShellFido2TokenManager *default_manager = NULL;

  if (default_manager == NULL)
    default_manager = g_object_new (SHELL_TYPE_FIDO2_TOKEN_MANAGER, NULL);

  return default_manager;
}

/**
 * shell_fido2_token_manager_has_inserted_tokens:
 * @manager: a #ShellFido2TokenManager
 *
 * Returns: whether any security key is plugged in
 */
gboolean
shell_fido2_token_manager_has_inserted_tokens (ShellFido2TokenManager *manager)
{
  g_return_val_if_fail (SHELL_IS_FIDO2_TOKEN_MANAGER (manager), FALSE);

  return g_hash_table_size (manager->inserted_tokens) > 0;
}
