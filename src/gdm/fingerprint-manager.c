/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * fingerprint-manager.c: Finds out what kind of fingerprint reader fprintd
 * reports. Port of js/gdm/fingerprintManager.js.
 *
 * Differences from the JS module: an unexpected reader type or an fprintd
 * error is logged as a message instead of an error; nothing is emitted after
 * ShellGlobal::shutdown; a check that is asked for before the proxy of the
 * manager is ready is dropped (the first check runs when it is).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include "fprint-dbus.h"
#include "gdm/fingerprint-manager.h"
#include "shell-global.h"

#define FPRINT_BUS_NAME "net.reactivated.Fprint"
#define FPRINT_MANAGER_PATH "/net/reactivated/Fprint/Manager"

enum
{
  PROP_0,

  PROP_READER_TYPE,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  READER_TYPE_CHANGED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellFingerprintManager
{
  GObject parent;

  ShellFprintManager *manager_proxy;
  GCancellable *cancellable;
  ShellFingerprintReaderType reader_type;
  gboolean shutting_down;
  int default_timeout;
};

G_DEFINE_FINAL_TYPE (ShellFingerprintManager, shell_fingerprint_manager, G_TYPE_OBJECT)

/**
 * shell_fingerprint_manager_get_reader_type:
 * @manager: a #ShellFingerprintManager
 *
 * Returns: the type of the default fingerprint reader
 */
ShellFingerprintReaderType
shell_fingerprint_manager_get_reader_type (ShellFingerprintManager *manager)
{
  g_return_val_if_fail (SHELL_IS_FINGERPRINT_MANAGER (manager),
                        SHELL_FINGERPRINT_READER_TYPE_NONE);

  return manager->reader_type;
}

/**
 * shell_fingerprint_manager_get_reader_found:
 * @manager: a #ShellFingerprintManager
 *
 * Returns: whether there is a fingerprint reader
 */
gboolean
shell_fingerprint_manager_get_reader_found (ShellFingerprintManager *manager)
{
  g_return_val_if_fail (SHELL_IS_FINGERPRINT_MANAGER (manager), FALSE);

  return manager->reader_type != SHELL_FINGERPRINT_READER_TYPE_NONE;
}

static void
set_reader_type (ShellFingerprintManager *self,
                 ShellFingerprintReaderType reader_type)
{
  if (self->reader_type == reader_type)
    return;

  self->reader_type = reader_type;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_READER_TYPE]);
  g_signal_emit (self, signals[READER_TYPE_CHANGED], 0);
}

static void
handle_fingerprint_error (ShellFingerprintManager *self,
                          GError                  *error)
{
  set_reader_type (self, SHELL_FINGERPRINT_READER_TYPE_NONE);

  if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
    return;

  if (g_dbus_error_is_remote_error (error))
    {
      g_autofree char *remote_error = g_dbus_error_get_remote_error (error);

      if (g_strcmp0 (remote_error, "net.reactivated.Fprint.Error.NoSuchDevice") == 0)
        return;
    }

  g_message ("Failed to interact with fprintd service: %s", error->message);
}

typedef struct
{
  ShellFingerprintManager *self;
  GCancellable *cancellable;    /* nullable */
} CheckData;

static void
check_data_free (CheckData *data)
{
  g_clear_object (&data->self);
  g_clear_object (&data->cancellable);
  g_free (data);
}

static void
device_proxy_ready (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  g_autoptr (ShellFingerprintManager) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (ShellFprintDevice) device = NULL;
  const char *scan_type;

  device = shell_fprint_device_proxy_new_for_bus_finish (res, &error);
  if (device == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          !self->shutting_down)
        handle_fingerprint_error (self, error);
      return;
    }

  if (self->shutting_down)
    return;

  scan_type = shell_fprint_device_get_scan_type (device);
  if (g_strcmp0 (scan_type, "press") == 0)
    set_reader_type (self, SHELL_FINGERPRINT_READER_TYPE_PRESS);
  else if (g_strcmp0 (scan_type, "swipe") == 0)
    set_reader_type (self, SHELL_FINGERPRINT_READER_TYPE_SWIPE);
  else if (g_strcmp0 (scan_type, "none") == 0)
    set_reader_type (self, SHELL_FINGERPRINT_READER_TYPE_NONE);
  else
    {
      g_message ("Unexpected fingerprint device type '%s'",
                 scan_type != NULL ? scan_type : "(null)");
      set_reader_type (self, SHELL_FINGERPRINT_READER_TYPE_NONE);
    }
}

static void
default_device_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  CheckData *data = user_data;
  ShellFingerprintManager *self = data->self;
  g_autoptr (GError) error = NULL;
  g_autofree char *device_path = NULL;

  if (!shell_fprint_manager_call_get_default_device_finish (SHELL_FPRINT_MANAGER (source),
                                                            &device_path, res, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          !self->shutting_down)
        handle_fingerprint_error (self, error);
      check_data_free (data);
      return;
    }

  if (!self->shutting_down)
    {
      shell_fprint_device_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                             G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                             FPRINT_BUS_NAME,
                                             device_path,
                                             data->cancellable,
                                             device_proxy_ready,
                                             g_object_ref (self));
    }

  check_data_free (data);
}

static void
run_check (ShellFingerprintManager *self,
           GCancellable            *cancellable)
{
  CheckData *data = g_new0 (CheckData, 1);

  data->self = g_object_ref (self);
  g_set_object (&data->cancellable, cancellable);

  shell_fprint_manager_call_get_default_device (self->manager_proxy,
                                                cancellable,
                                                default_device_ready,
                                                data);
}

/**
 * shell_fingerprint_manager_check_reader_type:
 * @manager: a #ShellFingerprintManager
 * @cancellable: (nullable): a #GCancellable to stop the check
 *
 * Asks fprintd for the default reader and updates
 * #ShellFingerprintManager:reader-type. If the manager is not connected to
 * fprintd yet the call is ignored: the first check runs as soon as it is.
 */
void
shell_fingerprint_manager_check_reader_type (ShellFingerprintManager *manager,
                                             GCancellable            *cancellable)
{
  g_return_if_fail (SHELL_IS_FINGERPRINT_MANAGER (manager));

  if (manager->shutting_down)
    return;

  /* The first check runs when the proxy is ready */
  if (manager->manager_proxy == NULL)
    return;

  run_check (manager, cancellable != NULL ? cancellable : manager->cancellable);
}

/**
 * shell_fingerprint_manager_set_default_timeout:
 * @manager: a #ShellFingerprintManager
 * @timeout: the timeout in milliseconds for the calls to fprintd
 *
 * Sets the timeout of the calls to the fprintd manager.
 */
void
shell_fingerprint_manager_set_default_timeout (ShellFingerprintManager *manager,
                                               int                      timeout)
{
  g_return_if_fail (SHELL_IS_FINGERPRINT_MANAGER (manager));

  manager->default_timeout = timeout;
  if (manager->manager_proxy != NULL)
    g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (manager->manager_proxy), timeout);
}

static void
manager_proxy_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  g_autoptr (ShellFingerprintManager) self = user_data;
  g_autoptr (GError) error = NULL;
  ShellFprintManager *proxy;

  proxy = shell_fprint_manager_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          !self->shutting_down)
        handle_fingerprint_error (self, error);
      return;
    }

  if (self->shutting_down)
    {
      g_object_unref (proxy);
      return;
    }

  self->manager_proxy = proxy;
  if (self->default_timeout != 0)
    g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), self->default_timeout);

  /* The JS checked the reader as soon as the proxy was initialized */
  run_check (self, self->cancellable);
}

static void
on_shutdown (ShellGlobal             *global,
             ShellFingerprintManager *self)
{
  self->shutting_down = TRUE;
  g_cancellable_cancel (self->cancellable);
}

static void
shell_fingerprint_manager_constructed (GObject *object)
{
  ShellFingerprintManager *self = SHELL_FINGERPRINT_MANAGER (object);

  G_OBJECT_CLASS (shell_fingerprint_manager_parent_class)->constructed (object);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  shell_fprint_manager_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                                          G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START_AT_CONSTRUCTION |
                                          G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                          FPRINT_BUS_NAME,
                                          FPRINT_MANAGER_PATH,
                                          self->cancellable,
                                          manager_proxy_ready,
                                          g_object_ref (self));
}

static void
shell_fingerprint_manager_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellFingerprintManager *self = SHELL_FINGERPRINT_MANAGER (object);

  switch (prop_id)
    {
    case PROP_READER_TYPE:
      g_value_set_uint (value, self->reader_type);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_fingerprint_manager_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ShellFingerprintManager *self = SHELL_FINGERPRINT_MANAGER (object);

  switch (prop_id)
    {
    case PROP_READER_TYPE:
      set_reader_type (self, g_value_get_uint (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_fingerprint_manager_dispose (GObject *object)
{
  ShellFingerprintManager *self = SHELL_FINGERPRINT_MANAGER (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->manager_proxy);

  G_OBJECT_CLASS (shell_fingerprint_manager_parent_class)->dispose (object);
}

static void
shell_fingerprint_manager_class_init (ShellFingerprintManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_fingerprint_manager_constructed;
  object_class->get_property = shell_fingerprint_manager_get_property;
  object_class->set_property = shell_fingerprint_manager_set_property;
  object_class->dispose = shell_fingerprint_manager_dispose;

  /**
   * ShellFingerprintManager:reader-type:
   *
   * The #ShellFingerprintReaderType of the default fingerprint reader.
   */
  props[PROP_READER_TYPE] =
    g_param_spec_uint ("reader-type", NULL, NULL,
                       SHELL_FINGERPRINT_READER_TYPE_NONE,
                       SHELL_FINGERPRINT_READER_TYPE_SWIPE,
                       SHELL_FINGERPRINT_READER_TYPE_NONE,
                       G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellFingerprintManager::reader-type-changed:
   * @manager: the #ShellFingerprintManager
   *
   * Emitted when the type of the reader changes.
   */
  signals[READER_TYPE_CHANGED] =
    g_signal_new ("reader-type-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_fingerprint_manager_init (ShellFingerprintManager *self)
{
  self->cancellable = g_cancellable_new ();
}

/**
 * shell_fingerprint_manager_get_default:
 *
 * Gets the fingerprint manager, creating it if needed.
 *
 * Returns: (transfer none): the #ShellFingerprintManager
 */
ShellFingerprintManager *
shell_fingerprint_manager_get_default (void)
{
  static ShellFingerprintManager *default_manager = NULL;

  if (default_manager == NULL)
    default_manager = g_object_new (SHELL_TYPE_FINGERPRINT_MANAGER, NULL);

  return default_manager;
}
