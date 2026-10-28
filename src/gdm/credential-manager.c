/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * credential-manager.c: Sources of preauthenticated credentials of the login
 * and lock screens. Port of js/gdm/credentialManager.js, js/gdm/oVirt.js and
 * js/gdm/vmware.js.
 *
 * A #ShellCredentialManager holds the token an agent of a virtualization
 * environment (oVirt's vdsm on the system bus, VMware's view agent on the
 * session bus) sent in its UserAuthenticated signal, and tells the screen
 * shield and the authentication services about it.
 *
 * Differences from the JS modules: the proxies are created asynchronously and
 * a failure to create one is logged as a message (the JS constructor threw
 * and broke the authentication services); the signal is ignored after
 * ShellGlobal::shutdown.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "gdm/credential-manager.h"
#include "ovirt-dbus.h"
#include "shell-global.h"
#include "vmware-dbus.h"

/* ------------------------------------------------------------------------ */
/* ShellCredentialManager                                                    */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_TOKEN,
  PROP_SERVICE,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  USER_AUTHENTICATED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

typedef struct
{
  char *token;
  char *service;
} ShellCredentialManagerPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellCredentialManager, shell_credential_manager, G_TYPE_OBJECT)

/**
 * shell_credential_manager_get_token:
 * @manager: a #ShellCredentialManager
 *
 * Returns: (transfer none) (nullable): the token of the user that
 *   authenticated, if any
 */
const char *
shell_credential_manager_get_token (ShellCredentialManager *manager)
{
  ShellCredentialManagerPrivate *priv;

  g_return_val_if_fail (SHELL_IS_CREDENTIAL_MANAGER (manager), NULL);

  priv = shell_credential_manager_get_instance_private (manager);
  return priv->token;
}

/**
 * shell_credential_manager_set_token:
 * @manager: a #ShellCredentialManager
 * @token: (nullable): the new token, or %NULL to forget it
 *
 * Sets the token. ::user-authenticated is emitted if @token is not %NULL.
 */
void
shell_credential_manager_set_token (ShellCredentialManager *manager,
                                    const char             *token)
{
  ShellCredentialManagerPrivate *priv;
  g_autofree char *emitted = NULL;

  g_return_if_fail (SHELL_IS_CREDENTIAL_MANAGER (manager));

  priv = shell_credential_manager_get_instance_private (manager);

  g_free (priv->token);
  priv->token = g_strdup (token);
  g_object_notify_by_pspec (G_OBJECT (manager), props[PROP_TOKEN]);

  if (priv->token != NULL)
    {
      emitted = g_strdup (priv->token);
      g_signal_emit (manager, signals[USER_AUTHENTICATED], 0, emitted);
    }
}

/**
 * shell_credential_manager_get_service:
 * @manager: a #ShellCredentialManager
 *
 * Returns: (transfer none): the name of the PAM service that takes the token
 */
const char *
shell_credential_manager_get_service (ShellCredentialManager *manager)
{
  ShellCredentialManagerPrivate *priv;

  g_return_val_if_fail (SHELL_IS_CREDENTIAL_MANAGER (manager), NULL);

  priv = shell_credential_manager_get_instance_private (manager);
  return priv->service;
}

static void
shell_credential_manager_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellCredentialManager *manager = SHELL_CREDENTIAL_MANAGER (object);
  ShellCredentialManagerPrivate *priv = shell_credential_manager_get_instance_private (manager);

  switch (prop_id)
    {
    case PROP_TOKEN:
      shell_credential_manager_set_token (manager, g_value_get_string (value));
      break;

    case PROP_SERVICE:
      g_free (priv->service);
      priv->service = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_credential_manager_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellCredentialManager *manager = SHELL_CREDENTIAL_MANAGER (object);
  ShellCredentialManagerPrivate *priv = shell_credential_manager_get_instance_private (manager);

  switch (prop_id)
    {
    case PROP_TOKEN:
      g_value_set_string (value, priv->token);
      break;

    case PROP_SERVICE:
      g_value_set_string (value, priv->service);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_credential_manager_finalize (GObject *object)
{
  ShellCredentialManager *manager = SHELL_CREDENTIAL_MANAGER (object);
  ShellCredentialManagerPrivate *priv = shell_credential_manager_get_instance_private (manager);

  g_clear_pointer (&priv->token, g_free);
  g_clear_pointer (&priv->service, g_free);

  G_OBJECT_CLASS (shell_credential_manager_parent_class)->finalize (object);
}

static void
shell_credential_manager_class_init (ShellCredentialManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = shell_credential_manager_set_property;
  object_class->get_property = shell_credential_manager_get_property;
  object_class->finalize = shell_credential_manager_finalize;

  /**
   * ShellCredentialManager:token:
   *
   * The token of the user that authenticated. Setting it to a token emits
   * ::user-authenticated.
   */
  props[PROP_TOKEN] =
    g_param_spec_string ("token", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellCredentialManager:service:
   *
   * The name of the PAM service that takes the token.
   */
  props[PROP_SERVICE] =
    g_param_spec_string ("service", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellCredentialManager::user-authenticated:
   * @manager: the #ShellCredentialManager
   * @token: the token
   *
   * Emitted when the token is set.
   */
  signals[USER_AUTHENTICATED] =
    g_signal_new ("user-authenticated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
}

static void
shell_credential_manager_init (ShellCredentialManager *manager)
{
}

/* ------------------------------------------------------------------------ */
/* ShellOvirtCredentialManager                                               */
/* ------------------------------------------------------------------------ */

struct _ShellOvirtCredentialManager
{
  ShellCredentialManager parent;

  ShellOvirtCredentials *credentials;
  GCancellable *cancellable;
  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellOvirtCredentialManager, shell_ovirt_credential_manager,
                     SHELL_TYPE_CREDENTIAL_MANAGER)

static void
on_ovirt_user_authenticated (ShellOvirtCredentials         *credentials,
                             const char                    *token,
                             ShellOvirtCredentialManager   *self)
{
  if (self->shutting_down)
    return;

  shell_credential_manager_set_token (SHELL_CREDENTIAL_MANAGER (self), token);
}

static void
ovirt_proxy_ready (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  g_autoptr (ShellOvirtCredentialManager) self = user_data;
  g_autoptr (GError) error = NULL;
  ShellOvirtCredentials *credentials;

  credentials = shell_ovirt_credentials_proxy_new_for_bus_finish (res, &error);
  if (credentials == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Could not connect to the oVirt credentials service: %s",
                   error->message);
      return;
    }

  if (self->shutting_down)
    {
      g_object_unref (credentials);
      return;
    }

  self->credentials = credentials;
  g_signal_connect (self->credentials, "user-authenticated",
                    G_CALLBACK (on_ovirt_user_authenticated), self);
}

static void
on_ovirt_shutdown (ShellGlobal                 *global,
                   ShellOvirtCredentialManager *self)
{
  self->shutting_down = TRUE;
  g_cancellable_cancel (self->cancellable);
}

static void
shell_ovirt_credential_manager_constructed (GObject *object)
{
  ShellOvirtCredentialManager *self = SHELL_OVIRT_CREDENTIAL_MANAGER (object);

  G_OBJECT_CLASS (shell_ovirt_credential_manager_parent_class)->constructed (object);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_ovirt_shutdown), self, 0);

  self->cancellable = g_cancellable_new ();
  shell_ovirt_credentials_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                             G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                             "org.ovirt.vdsm.Credentials",
                                             "/org/ovirt/vdsm/Credentials",
                                             self->cancellable,
                                             ovirt_proxy_ready,
                                             g_object_ref (self));
}

static void
shell_ovirt_credential_manager_dispose (GObject *object)
{
  ShellOvirtCredentialManager *self = SHELL_OVIRT_CREDENTIAL_MANAGER (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  if (self->credentials != NULL)
    g_signal_handlers_disconnect_by_data (self->credentials, self);
  g_clear_object (&self->credentials);

  G_OBJECT_CLASS (shell_ovirt_credential_manager_parent_class)->dispose (object);
}

static void
shell_ovirt_credential_manager_class_init (ShellOvirtCredentialManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_ovirt_credential_manager_constructed;
  object_class->dispose = shell_ovirt_credential_manager_dispose;
}

static void
shell_ovirt_credential_manager_init (ShellOvirtCredentialManager *self)
{
}

/**
 * shell_ovirt_credential_manager_get_default:
 *
 * Gets the credential manager of the oVirt agent, creating it if needed.
 *
 * Returns: (transfer none): the #ShellOvirtCredentialManager
 */
ShellOvirtCredentialManager *
shell_ovirt_credential_manager_get_default (void)
{
  static ShellOvirtCredentialManager *default_manager = NULL;

  if (default_manager == NULL)
    default_manager = g_object_new (SHELL_TYPE_OVIRT_CREDENTIAL_MANAGER,
                                    "service", SHELL_OVIRT_SERVICE_NAME,
                                    NULL);

  return default_manager;
}

/* ------------------------------------------------------------------------ */
/* ShellVmwareCredentialManager                                              */
/* ------------------------------------------------------------------------ */

struct _ShellVmwareCredentialManager
{
  ShellCredentialManager parent;

  ShellVmwareCredentials *credentials;
  GCancellable *cancellable;
  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellVmwareCredentialManager, shell_vmware_credential_manager,
                     SHELL_TYPE_CREDENTIAL_MANAGER)

static void
on_vmware_user_authenticated (ShellVmwareCredentials       *credentials,
                              const char                   *token,
                              ShellVmwareCredentialManager *self)
{
  if (self->shutting_down)
    return;

  shell_credential_manager_set_token (SHELL_CREDENTIAL_MANAGER (self), token);
}

static void
vmware_proxy_ready (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  g_autoptr (ShellVmwareCredentialManager) self = user_data;
  g_autoptr (GError) error = NULL;
  ShellVmwareCredentials *credentials;

  credentials = shell_vmware_credentials_proxy_new_for_bus_finish (res, &error);
  if (credentials == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Could not connect to the VMware credentials service: %s",
                   error->message);
      return;
    }

  if (self->shutting_down)
    {
      g_object_unref (credentials);
      return;
    }

  self->credentials = credentials;
  g_signal_connect (self->credentials, "user-authenticated",
                    G_CALLBACK (on_vmware_user_authenticated), self);
}

static void
on_vmware_shutdown (ShellGlobal                  *global,
                    ShellVmwareCredentialManager *self)
{
  self->shutting_down = TRUE;
  g_cancellable_cancel (self->cancellable);
}

static void
shell_vmware_credential_manager_constructed (GObject *object)
{
  ShellVmwareCredentialManager *self = SHELL_VMWARE_CREDENTIAL_MANAGER (object);

  G_OBJECT_CLASS (shell_vmware_credential_manager_parent_class)->constructed (object);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_vmware_shutdown), self, 0);

  self->cancellable = g_cancellable_new ();
  shell_vmware_credentials_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                              G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                              "org.vmware.viewagent.Credentials",
                                              "/org/vmware/viewagent/Credentials",
                                              self->cancellable,
                                              vmware_proxy_ready,
                                              g_object_ref (self));
}

static void
shell_vmware_credential_manager_dispose (GObject *object)
{
  ShellVmwareCredentialManager *self = SHELL_VMWARE_CREDENTIAL_MANAGER (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  if (self->credentials != NULL)
    g_signal_handlers_disconnect_by_data (self->credentials, self);
  g_clear_object (&self->credentials);

  G_OBJECT_CLASS (shell_vmware_credential_manager_parent_class)->dispose (object);
}

static void
shell_vmware_credential_manager_class_init (ShellVmwareCredentialManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_vmware_credential_manager_constructed;
  object_class->dispose = shell_vmware_credential_manager_dispose;
}

static void
shell_vmware_credential_manager_init (ShellVmwareCredentialManager *self)
{
}

/**
 * shell_vmware_credential_manager_get_default:
 *
 * Gets the credential manager of the VMware agent, creating it if needed.
 *
 * Returns: (transfer none): the #ShellVmwareCredentialManager
 */
ShellVmwareCredentialManager *
shell_vmware_credential_manager_get_default (void)
{
  static ShellVmwareCredentialManager *default_manager = NULL;

  if (default_manager == NULL)
    default_manager = g_object_new (SHELL_TYPE_VMWARE_CREDENTIAL_MANAGER,
                                    "service", SHELL_VMWARE_SERVICE_NAME,
                                    NULL);

  return default_manager;
}
