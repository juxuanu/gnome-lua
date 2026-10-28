/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * smartcard-manager.c: Tracks the smartcards that gnome-settings-daemon
 * reports. Port of js/gdm/smartcardManager.js.
 *
 * The tokens are the #GDBusProxy objects of the
 * org.gnome.SettingsDaemon.Smartcard.Token interface that
 * #ShellDBusObjectManager creates: read their properties (Name, Driver,
 * IsInserted, UsedToLogin) with g_dbus_proxy_get_cached_property (), the JS
 * property accessors the old module added to them are gone.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include "gdm/smartcard-manager.h"
#include "misc/dbus-object-manager.h"

#define SMARTCARD_BUS_NAME "org.gnome.SettingsDaemon.Smartcard"
#define SMARTCARD_OBJECT_PATH "/org/gnome/SettingsDaemon/Smartcard"
#define SMARTCARD_TOKEN_INTERFACE "org.gnome.SettingsDaemon.Smartcard.Token"

#define SMARTCARD_TOKEN_IFACE_XML \
  "<node>" \
  "<interface name='" SMARTCARD_TOKEN_INTERFACE "'>" \
  "  <property name='Name' type='s' access='read'/>" \
  "  <property name='Driver' type='o' access='read'/>" \
  "  <property name='IsInserted' type='b' access='read'/>" \
  "  <property name='UsedToLogin' type='b' access='read'/>" \
  "</interface>" \
  "</node>"

enum
{
  SMARTCARD_INSERTED,
  SMARTCARD_REMOVED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellSmartcardManager
{
  GObject parent;

  ShellDBusObjectManager *object_manager;
  GHashTable *tokens;           /* GDBusProxy -> handler id of g-properties-changed */
  GHashTable *inserted_tokens;  /* object path -> GDBusProxy */
  GDBusProxy *login_token;
};

G_DEFINE_FINAL_TYPE (ShellSmartcardManager, shell_smartcard_manager, G_TYPE_OBJECT)

static gboolean
token_get_boolean (GDBusProxy *token,
                   const char *property)
{
  g_autoptr (GVariant) value = g_dbus_proxy_get_cached_property (token, property);

  return value != NULL && g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN) &&
         g_variant_get_boolean (value);
}

static void
update_token (ShellSmartcardManager *self,
              GDBusProxy            *token)
{
  const char *object_path = g_dbus_proxy_get_object_path (token);

  g_hash_table_remove (self->inserted_tokens, object_path);

  if (token_get_boolean (token, "IsInserted"))
    g_hash_table_insert (self->inserted_tokens, g_strdup (object_path),
                         g_object_ref (token));

  if (token_get_boolean (token, "UsedToLogin"))
    g_set_object (&self->login_token, token);
}

static void
on_token_properties_changed (GDBusProxy            *token,
                             GVariant              *changed,
                             GStrv                  invalidated,
                             ShellSmartcardManager *self)
{
  g_autoptr (GVariant) is_inserted = NULL;

  is_inserted = g_variant_lookup_value (changed, "IsInserted", NULL);
  if (is_inserted == NULL)
    return;

  update_token (self, token);

  if (token_get_boolean (token, "IsInserted"))
    g_signal_emit (self, signals[SMARTCARD_INSERTED], 0, token);
  else
    g_signal_emit (self, signals[SMARTCARD_REMOVED], 0, token);
}

static void
add_token (ShellSmartcardManager *self,
           GDBusProxy            *token)
{
  const char *object_path = g_dbus_proxy_get_object_path (token);
  gulong handler_id;

  if (g_hash_table_contains (self->tokens, token))
    return;

  /* This is an alias, mirroring the real token.
   * Skip it to avoid duplicate events */
  if (g_str_has_suffix (object_path, "/login_token"))
    return;

  update_token (self, token);

  handler_id = g_signal_connect (token, "g-properties-changed",
                                 G_CALLBACK (on_token_properties_changed), self);
  g_hash_table_insert (self->tokens, g_object_ref (token),
                       GSIZE_TO_POINTER (handler_id));

  /* Emit a smartcard-inserted at startup if it's already plugged in */
  if (token_get_boolean (token, "IsInserted"))
    g_signal_emit (self, signals[SMARTCARD_INSERTED], 0, token);
}

static void
remove_token (ShellSmartcardManager *self,
              GDBusProxy            *token)
{
  g_autoptr (GDBusProxy) keep = g_object_ref (token);
  const char *object_path = g_dbus_proxy_get_object_path (token);
  gpointer handler_id;

  if (g_hash_table_lookup (self->inserted_tokens, object_path) == token)
    {
      g_hash_table_remove (self->inserted_tokens, object_path);
      g_signal_emit (self, signals[SMARTCARD_REMOVED], 0, token);
    }

  if (self->login_token == token)
    g_clear_object (&self->login_token);

  if (g_hash_table_lookup_extended (self->tokens, token, NULL, &handler_id))
    {
      g_signal_handler_disconnect (token, GPOINTER_TO_SIZE (handler_id));
      g_hash_table_remove (self->tokens, token);
    }
}

static void
on_loaded (ShellDBusObjectManager *object_manager,
           ShellSmartcardManager  *self)
{
  GList *tokens, *l;

  tokens = shell_dbus_object_manager_get_proxies_for_interface (object_manager,
                                                               SMARTCARD_TOKEN_INTERFACE);
  for (l = tokens; l != NULL; l = l->next)
    add_token (self, l->data);

  g_list_free (tokens);
}

static void
on_interface_added (ShellDBusObjectManager *object_manager,
                    const char             *interface_name,
                    GDBusProxy             *proxy,
                    ShellSmartcardManager  *self)
{
  if (g_str_equal (interface_name, SMARTCARD_TOKEN_INTERFACE))
    add_token (self, proxy);
}

static void
on_interface_removed (ShellDBusObjectManager *object_manager,
                      const char             *interface_name,
                      GDBusProxy             *proxy,
                      ShellSmartcardManager  *self)
{
  if (g_str_equal (interface_name, SMARTCARD_TOKEN_INTERFACE))
    remove_token (self, proxy);
}

static void
token_disconnect (gpointer key,
                  gpointer value,
                  gpointer user_data)
{
  g_signal_handler_disconnect (key, GPOINTER_TO_SIZE (value));
}

static void
shell_smartcard_manager_dispose (GObject *object)
{
  ShellSmartcardManager *self = SHELL_SMARTCARD_MANAGER (object);

  if (self->object_manager != NULL)
    g_signal_handlers_disconnect_by_data (self->object_manager, self);
  g_clear_object (&self->object_manager);

  if (self->tokens != NULL)
    g_hash_table_foreach (self->tokens, token_disconnect, NULL);
  g_clear_pointer (&self->tokens, g_hash_table_unref);
  g_clear_pointer (&self->inserted_tokens, g_hash_table_unref);
  g_clear_object (&self->login_token);

  G_OBJECT_CLASS (shell_smartcard_manager_parent_class)->dispose (object);
}

static void
shell_smartcard_manager_class_init (ShellSmartcardManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_smartcard_manager_dispose;

  /**
   * ShellSmartcardManager::smartcard-inserted:
   * @manager: the #ShellSmartcardManager
   * @token: the #GDBusProxy of the token
   *
   * Emitted when a smartcard is inserted, and for the smartcards that are
   * inserted when the manager is loaded.
   */
  signals[SMARTCARD_INSERTED] =
    g_signal_new ("smartcard-inserted",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_DBUS_PROXY);

  /**
   * ShellSmartcardManager::smartcard-removed:
   * @manager: the #ShellSmartcardManager
   * @token: the #GDBusProxy of the token
   *
   * Emitted when a smartcard is removed.
   */
  signals[SMARTCARD_REMOVED] =
    g_signal_new ("smartcard-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_DBUS_PROXY);
}

static void
shell_smartcard_manager_init (ShellSmartcardManager *self)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GDBusConnection) connection = NULL;
  const char * const known_interfaces[] = { SMARTCARD_TOKEN_IFACE_XML, NULL };

  self->tokens = g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);
  self->inserted_tokens = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, g_object_unref);

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (connection == NULL)
    {
      g_message ("Could not connect to the session bus to track smartcards: %s",
                 error->message);
      return;
    }

  self->object_manager = shell_dbus_object_manager_new (connection,
                                                        SMARTCARD_BUS_NAME,
                                                        SMARTCARD_OBJECT_PATH,
                                                        known_interfaces,
                                                        NULL);
  g_signal_connect (self->object_manager, "loaded",
                    G_CALLBACK (on_loaded), self);
  g_signal_connect (self->object_manager, "interface-added",
                    G_CALLBACK (on_interface_added), self);
  g_signal_connect (self->object_manager, "interface-removed",
                    G_CALLBACK (on_interface_removed), self);
}

/**
 * shell_smartcard_manager_get_default:
 *
 * Gets the smartcard manager, creating it if needed.
 *
 * Returns: (transfer none): the #ShellSmartcardManager
 */
ShellSmartcardManager *
shell_smartcard_manager_get_default (void)
{
  static ShellSmartcardManager *default_manager = NULL;

  if (default_manager == NULL)
    default_manager = g_object_new (SHELL_TYPE_SMARTCARD_MANAGER, NULL);

  return default_manager;
}

/**
 * shell_smartcard_manager_has_inserted_tokens:
 * @manager: a #ShellSmartcardManager
 *
 * Returns: whether any smartcard is inserted
 */
gboolean
shell_smartcard_manager_has_inserted_tokens (ShellSmartcardManager *manager)
{
  g_return_val_if_fail (SHELL_IS_SMARTCARD_MANAGER (manager), FALSE);

  return g_hash_table_size (manager->inserted_tokens) > 0;
}

/**
 * shell_smartcard_manager_has_inserted_login_token:
 * @manager: a #ShellSmartcardManager
 *
 * Returns: whether the smartcard that was used to log in is inserted
 */
gboolean
shell_smartcard_manager_has_inserted_login_token (ShellSmartcardManager *manager)
{
  g_return_val_if_fail (SHELL_IS_SMARTCARD_MANAGER (manager), FALSE);

  if (manager->login_token == NULL)
    return FALSE;

  return token_get_boolean (manager->login_token, "IsInserted");
}
