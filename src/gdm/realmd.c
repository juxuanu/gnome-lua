/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * realmd.c: Finds the login format of the network realms that realmd knows
 * (for example "%U@domain.example" or "DOMAIN\%U"). Port of js/gdm/realmd.js.
 *
 * Differences from the JS module: the proxies of the realms that are already
 * known are not created again when the list of realms changes;
 * ::login-format-changed is only emitted when the format changes (the JS also
 * emitted it with an undefined format the first time it looked); failing to
 * release the service is logged as a message; nothing is emitted after
 * ShellGlobal::shutdown.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "gdm/realmd.h"
#include "realmd-dbus.h"
#include "shell-global.h"

#define REALMD_BUS_NAME "org.freedesktop.realmd"
#define REALMD_OBJECT_PATH "/org/freedesktop/realmd"

enum
{
  PROP_0,

  PROP_LOGIN_FORMAT,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  LOGIN_FORMAT_CHANGED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellRealmdManager
{
  GObject parent;

  ShellRealmdProvider *aggregate_provider;
  gulong provider_changed_id;
  GCancellable *cancellable;

  GHashTable *tracked_realms;   /* object path -> ShellRealmdRealm */
  GPtrArray *realms;            /* the configured ShellRealmdRealm, in the order they were found */
  char *login_format;
  gboolean released;
  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellRealmdManager, shell_realmd_manager, G_TYPE_OBJECT)

static void
update_login_format (ShellRealmdManager *self)
{
  const char *new_login_format = NULL;
  guint i;

  for (i = 0; i < self->realms->len; i++)
    {
      ShellRealmdRealm *realm = g_ptr_array_index (self->realms, i);
      const char * const *formats = shell_realmd_realm_get_login_formats (realm);

      if (formats != NULL && formats[0] != NULL)
        {
          new_login_format = formats[0];
          break;
        }
    }

  if (g_strcmp0 (self->login_format, new_login_format) == 0)
    return;

  g_free (self->login_format);
  self->login_format = g_strdup (new_login_format);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LOGIN_FORMAT]);
  g_signal_emit (self, signals[LOGIN_FORMAT_CHANGED], 0, self->login_format);
}

static gboolean
realm_is_configured (ShellRealmdRealm *realm)
{
  const char *configured = shell_realmd_realm_get_configured (realm);

  return configured != NULL && configured[0] != '\0';
}

static void
reload_realm (ShellRealmdManager *self,
              ShellRealmdRealm   *realm)
{
  const char *object_path = g_dbus_proxy_get_object_path (G_DBUS_PROXY (realm));
  guint i;

  if (!realm_is_configured (realm))
    {
      for (i = 0; i < self->realms->len; i++)
        {
          if (g_ptr_array_index (self->realms, i) == realm)
            {
              g_ptr_array_remove_index (self->realms, i);
              break;
            }
        }

      return;
    }

  for (i = 0; i < self->realms->len; i++)
    {
      ShellRealmdRealm *other = g_ptr_array_index (self->realms, i);

      if (g_strcmp0 (g_dbus_proxy_get_object_path (G_DBUS_PROXY (other)), object_path) == 0)
        {
          g_ptr_array_index (self->realms, i) = g_object_ref (realm);
          g_object_unref (other);
          update_login_format (self);
          return;
        }
    }

  g_ptr_array_add (self->realms, g_object_ref (realm));

  update_login_format (self);
}

static void
on_realm_properties_changed (GDBusProxy         *proxy,
                             GVariant           *changed,
                             GStrv               invalidated,
                             ShellRealmdManager *self)
{
  g_autoptr (GVariant) configured = NULL;

  if (self->shutting_down || self->released)
    return;

  configured = g_variant_lookup_value (changed, "Configured", NULL);
  if (configured != NULL)
    reload_realm (self, SHELL_REALMD_REALM (proxy));
}

static void
realm_proxy_ready (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  g_autoptr (ShellRealmdManager) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (ShellRealmdRealm) realm = NULL;
  const char *object_path;

  realm = shell_realmd_realm_proxy_new_for_bus_finish (res, &error);
  if (realm == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Could not create a realmd realm proxy: %s", error->message);
      return;
    }

  if (self->shutting_down || self->released)
    return;

  object_path = g_dbus_proxy_get_object_path (G_DBUS_PROXY (realm));
  if (g_hash_table_contains (self->tracked_realms, object_path))
    return;

  g_hash_table_insert (self->tracked_realms, g_strdup (object_path),
                       g_object_ref (realm));

  reload_realm (self, realm);

  g_signal_connect_object (realm, "g-properties-changed",
                           G_CALLBACK (on_realm_properties_changed), self, 0);
}

static void
reload_realms (ShellRealmdManager *self)
{
  const char * const *realm_paths;
  int i;

  if (self->aggregate_provider == NULL)
    return;

  realm_paths = shell_realmd_provider_get_realms (self->aggregate_provider);
  if (realm_paths == NULL)
    return;

  for (i = 0; realm_paths[i] != NULL; i++)
    {
      if (g_hash_table_contains (self->tracked_realms, realm_paths[i]))
        continue;

      shell_realmd_realm_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                            G_DBUS_PROXY_FLAGS_NONE,
                                            REALMD_BUS_NAME,
                                            realm_paths[i],
                                            self->cancellable,
                                            realm_proxy_ready,
                                            g_object_ref (self));
    }
}

static void
on_provider_properties_changed (GDBusProxy         *proxy,
                                GVariant           *changed,
                                GStrv               invalidated,
                                ShellRealmdManager *self)
{
  g_autoptr (GVariant) realms_changed = NULL;

  if (self->shutting_down || self->released)
    return;

  realms_changed = g_variant_lookup_value (changed, "Realms", NULL);
  if (realms_changed != NULL)
    reload_realms (self);
}

static void
provider_proxy_ready (GObject      *source,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  g_autoptr (ShellRealmdManager) self = user_data;
  g_autoptr (GError) error = NULL;
  ShellRealmdProvider *provider;

  provider = shell_realmd_provider_proxy_new_for_bus_finish (res, &error);
  if (provider == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Could not create the realmd provider proxy: %s", error->message);
      return;
    }

  if (self->shutting_down || self->released)
    {
      g_object_unref (provider);
      return;
    }

  self->aggregate_provider = provider;
  self->provider_changed_id =
    g_signal_connect (provider, "g-properties-changed",
                      G_CALLBACK (on_provider_properties_changed), self);

  reload_realms (self);
}

static void
on_shutdown (ShellGlobal        *global,
             ShellRealmdManager *self)
{
  self->shutting_down = TRUE;
  g_cancellable_cancel (self->cancellable);
}

static void
shell_realmd_manager_constructed (GObject *object)
{
  ShellRealmdManager *self = SHELL_REALMD_MANAGER (object);

  G_OBJECT_CLASS (shell_realmd_manager_parent_class)->constructed (object);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  shell_realmd_provider_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                           G_DBUS_PROXY_FLAGS_NONE,
                                           REALMD_BUS_NAME,
                                           REALMD_OBJECT_PATH,
                                           self->cancellable,
                                           provider_proxy_ready,
                                           g_object_ref (self));
}

static void
shell_realmd_manager_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  ShellRealmdManager *self = SHELL_REALMD_MANAGER (object);

  switch (prop_id)
    {
    case PROP_LOGIN_FORMAT:
      g_value_set_string (value, self->login_format);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_realmd_manager_dispose (GObject *object)
{
  ShellRealmdManager *self = SHELL_REALMD_MANAGER (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  if (self->aggregate_provider != NULL)
    g_clear_signal_handler (&self->provider_changed_id, self->aggregate_provider);
  g_clear_object (&self->aggregate_provider);

  g_clear_pointer (&self->tracked_realms, g_hash_table_unref);
  g_clear_pointer (&self->realms, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_realmd_manager_parent_class)->dispose (object);
}

static void
shell_realmd_manager_finalize (GObject *object)
{
  ShellRealmdManager *self = SHELL_REALMD_MANAGER (object);

  g_clear_pointer (&self->login_format, g_free);

  G_OBJECT_CLASS (shell_realmd_manager_parent_class)->finalize (object);
}

static void
shell_realmd_manager_class_init (ShellRealmdManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_realmd_manager_constructed;
  object_class->get_property = shell_realmd_manager_get_property;
  object_class->dispose = shell_realmd_manager_dispose;
  object_class->finalize = shell_realmd_manager_finalize;

  /**
   * ShellRealmdManager:login-format:
   *
   * The format of the user names of the first configured realm that has one
   * (`loginFormat` of the JS class), with %U for the user name and %D for the
   * domain, or %NULL if there is none.
   */
  props[PROP_LOGIN_FORMAT] =
    g_param_spec_string ("login-format", NULL, NULL,
                         NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellRealmdManager::login-format-changed:
   * @manager: the #ShellRealmdManager
   * @login_format: (nullable): the new login format
   *
   * Emitted when the login format changes.
   */
  signals[LOGIN_FORMAT_CHANGED] =
    g_signal_new ("login-format-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
}

static void
shell_realmd_manager_init (ShellRealmdManager *self)
{
  self->cancellable = g_cancellable_new ();
  self->tracked_realms = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_object_unref);
  self->realms = g_ptr_array_new_with_free_func (g_object_unref);
}

/**
 * shell_realmd_manager_new:
 *
 * Creates a manager that tracks the realms of realmd.
 *
 * Returns: (transfer full): a new #ShellRealmdManager
 */
ShellRealmdManager *
shell_realmd_manager_new (void)
{
  return g_object_new (SHELL_TYPE_REALMD_MANAGER, NULL);
}

/**
 * shell_realmd_manager_get_login_format:
 * @manager: a #ShellRealmdManager
 *
 * Returns: (transfer none) (nullable): the login format, see
 *   #ShellRealmdManager:login-format
 */
const char *
shell_realmd_manager_get_login_format (ShellRealmdManager *manager)
{
  g_return_val_if_fail (SHELL_IS_REALMD_MANAGER (manager), NULL);

  update_login_format (manager);

  return manager->login_format;
}

static void
service_release_done (GObject      *source,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!shell_realmd_service_call_release_finish (SHELL_REALMD_SERVICE (source),
                                                 res, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("Could not release realmd: %s", error->message);
}

static void
service_proxy_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (ShellRealmdService) service = NULL;

  service = shell_realmd_service_proxy_new_for_bus_finish (res, &error);
  if (service == NULL)
    {
      g_debug ("Could not create the realmd service proxy: %s", error->message);
      return;
    }

  shell_realmd_service_call_release (service, NULL, service_release_done, NULL);
}

/**
 * shell_realmd_manager_release:
 * @manager: a #ShellRealmdManager
 *
 * Tells realmd that the shell does not need it any more, and forgets the
 * realms.
 */
void
shell_realmd_manager_release (ShellRealmdManager *manager)
{
  g_return_if_fail (SHELL_IS_REALMD_MANAGER (manager));

  if (!manager->shutting_down)
    {
      shell_realmd_service_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                              G_DBUS_PROXY_FLAGS_NONE,
                                              REALMD_BUS_NAME,
                                              REALMD_OBJECT_PATH,
                                              NULL,
                                              service_proxy_ready,
                                              NULL);
    }

  manager->released = TRUE;
  if (manager->aggregate_provider != NULL)
    g_clear_signal_handler (&manager->provider_changed_id, manager->aggregate_provider);

  g_ptr_array_set_size (manager->realms, 0);
  update_login_format (manager);
}
