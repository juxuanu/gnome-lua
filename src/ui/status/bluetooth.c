/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * bluetooth.c: The Bluetooth indicator of the top bar and its toggle in the
 * quick settings. Port of js/ui/status/bluetooth.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - libgnome-bluetooth does not install its C headers (the library is meant
 *    for the GI users, that is the shell), so the few functions of
 *    #BluetoothClient and #BluetoothDevice which are not properties are
 *    declared below, with the signatures of the typelib. Everything else is
 *    read through GObject properties. The adapter states are the values of
 *    `BluetoothAdapterState`.
 *  - The GnomeBluetooth client is created once org.bluez has an owner on the
 *    system bus (it is started if needed, as the client did), not at once:
 *    the library reports a missing BlueZ or system bus as a warning, which
 *    must not happen at startup. Until then there are no devices, the adapter
 *    is absent and not powered. The toggle is shown as soon as the rfkill
 *    service of the settings daemon says the Bluetooth switch exists.
 *  - The proxy of org.gnome.SettingsDaemon.Rfkill is a gdbus-codegen proxy;
 *    the `available` property is notified once the proxy is ready, not only
 *    when one of its properties changes. An unreachable settings daemon is
 *    logged as a debug message instead of an error.
 *  - The "force a notify after 30 seconds" timeout of toggleActive () is one
 *    timeout which is restarted by each toggle, and it clears the predicted
 *    adapter state itself.
 *  - The notify handlers of the devices are connected once for each device,
 *    also after the default adapter changed, and are disconnected when the
 *    client is disposed.
 *  - Device names are sorted with g_utf8_collate () where the JS used
 *    String.localeCompare ().
 *  - Connecting or disconnecting a device is cancelled when the shell shuts
 *    down.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <atk/atk.h>
#include <clutter/clutter-pango.h>
#include <glib/gi18n-lib.h>

#include "shell-global.h"
#include "ui/animation.h"
#include "ui/popup-menu.h"
#include "ui/quick-settings.h"
#include "ui/status/bluetooth.h"
#include "ui/status/status-private.h"

#include "bluetooth-rfkill-dbus.h"

#define RFKILL_BUS_NAME "org.gnome.SettingsDaemon.Rfkill"
#define RFKILL_OBJECT_PATH "/org/gnome/SettingsDaemon/Rfkill"

#define STATE_CHANGE_FAILED_TIMEOUT_SECS 30

/* The values of BluetoothAdapterState */
typedef enum
{
  ADAPTER_STATE_ABSENT = 0,
  ADAPTER_STATE_ON = 1,
  ADAPTER_STATE_TURNING_ON = 2,
  ADAPTER_STATE_TURNING_OFF = 3,
  ADAPTER_STATE_OFF = 4,
} AdapterState;

/* ------------------------------------------------------------------------ */
/* libgnome-bluetooth                                                       */
/* ------------------------------------------------------------------------ */

/* BluetoothClient * and BluetoothDevice *, as GObjects */
GObject    *bluetooth_client_new                     (void);
GListStore *bluetooth_client_get_devices             (GObject             *client);
void        bluetooth_client_connect_service         (GObject             *client,
                                                      const char          *path,
                                                      gboolean             connect,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);
gboolean    bluetooth_client_connect_service_finish  (GObject             *client,
                                                      GAsyncResult        *res,
                                                      GError             **error);
const char *bluetooth_device_get_object_path         (GObject             *device);

static gboolean
device_get_boolean (GObject    *device,
                    const char *property)
{
  gboolean value = FALSE;

  g_object_get (device, property, &value, NULL);
  return value;
}

static char *
device_dup_alias (GObject *device)
{
  char *alias = NULL;

  g_object_get (device, "alias", &alias, NULL);
  if (alias == NULL)
    alias = g_strdup ("");

  return alias;
}

/* ======================================================================== */
/* ShellBluetoothClient                                                     */
/* ======================================================================== */

enum
{
  CLIENT_PROP_0,

  CLIENT_PROP_AVAILABLE,
  CLIENT_PROP_ACTIVE,
  CLIENT_PROP_ADAPTER_STATE,

  N_CLIENT_PROPS
};

static GParamSpec *client_props[N_CLIENT_PROPS] = { NULL, };

enum
{
  DEVICES_CHANGED,
  DEVICE_REMOVED,

  N_CLIENT_SIGNALS
};

static guint client_signals[N_CLIENT_SIGNALS] = { 0, };

#define SHELL_TYPE_BLUETOOTH_CLIENT (shell_bluetooth_client_get_type ())
G_DECLARE_FINAL_TYPE (ShellBluetoothClient, shell_bluetooth_client,
                      SHELL, BLUETOOTH_CLIENT, GObject)

struct _ShellBluetoothClient
{
  GObject parent;

  GObject *client;                    /* BluetoothClient, created when BlueZ is there */
  ShellBluetoothRfkill *proxy;        /* nullable */
  GCancellable *cancellable;

  guint bluez_watch_id;
  GHashTable *device_notify_connected; /* object path -> NULL */

  int predicted_state;                /* -1 if there is none */
  guint devices_changed_id;
  guint state_change_failed_id;
};

G_DEFINE_FINAL_TYPE (ShellBluetoothClient, shell_bluetooth_client, G_TYPE_OBJECT)

static gboolean
bt_client_get_available (ShellBluetoothClient *self)
{
  /* If we have an rfkill switch, make sure it's not a hardware
   * one as we can't get out of it in software */
  return self->proxy != NULL &&
         shell_bluetooth_rfkill_get_bluetooth_has_airplane_mode (SHELL_BLUETOOTH_RFKILL (self->proxy)) &&
         !shell_bluetooth_rfkill_get_bluetooth_hardware_airplane_mode (SHELL_BLUETOOTH_RFKILL (self->proxy));
}

static gboolean
bt_client_get_active (ShellBluetoothClient *self)
{
  return self->client != NULL &&
         device_get_boolean (self->client, "default-adapter-powered");
}

static int
bt_client_get_adapter_state (ShellBluetoothClient *self)
{
  int state = ADAPTER_STATE_ABSENT;

  if (self->predicted_state >= 0)
    return self->predicted_state;

  if (self->client != NULL)
    g_object_get (self->client, "default-adapter-state", &state, NULL);

  return state;
}

/* getDevices (): the paired or trusted devices, none while turned off.
 * Returns a new array of new references. */
static GPtrArray *
bt_client_get_devices (ShellBluetoothClient *self)
{
  GPtrArray *devices = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GListStore) store = NULL;
  guint n_items;
  guint i;

  /* Ignore any lingering device references when turned off */
  if (!bt_client_get_active (self))
    return devices;

  store = bluetooth_client_get_devices (self->client);
  n_items = g_list_model_get_n_items (G_LIST_MODEL (store));
  for (i = 0; i < n_items; i++)
    {
      GObject *device = g_list_model_get_item (G_LIST_MODEL (store), i);

      if (device_get_boolean (device, "paired") || device_get_boolean (device, "trusted"))
        g_ptr_array_add (devices, device);
      else
        g_object_unref (device);
    }

  return devices;
}

static void
bt_client_devices_changed_idle_cb (gpointer user_data)
{
  ShellBluetoothClient *self = user_data;

  self->devices_changed_id = 0;
  g_signal_emit (self, client_signals[DEVICES_CHANGED], 0);
}

/* _queueDevicesChanged () */
static void
bt_client_queue_devices_changed (ShellBluetoothClient *self)
{
  if (self->devices_changed_id != 0)
    return;

  self->devices_changed_id = g_idle_add_once (bt_client_devices_changed_idle_cb, self);
  g_source_set_name_by_id (self->devices_changed_id,
                           "[gnome-shell] bt_client_devices_changed_idle_cb");
}

static void
bt_client_device_notify_cb (GObject              *device,
                            GParamSpec           *pspec,
                            ShellBluetoothClient *self)
{
  bt_client_queue_devices_changed (self);
}

/* _connectDeviceNotify () */
static void
bt_client_connect_device_notify (ShellBluetoothClient *self,
                                 GObject              *device)
{
  const char *path = bluetooth_device_get_object_path (device);

  if (path == NULL || g_hash_table_contains (self->device_notify_connected, path))
    return;

  g_signal_connect_object (device, "notify::alias",
                           G_CALLBACK (bt_client_device_notify_cb), self, 0);
  g_signal_connect_object (device, "notify::paired",
                           G_CALLBACK (bt_client_device_notify_cb), self, 0);
  g_signal_connect_object (device, "notify::trusted",
                           G_CALLBACK (bt_client_device_notify_cb), self, 0);
  g_signal_connect_object (device, "notify::connected",
                           G_CALLBACK (bt_client_device_notify_cb), self, 0);

  g_hash_table_add (self->device_notify_connected, g_strdup (path));
}

/* Connects to the devices of the store which are not connected yet; with
 * @disconnect_first the handlers are removed beforehand */
static void
bt_client_connect_all_devices (ShellBluetoothClient *self,
                               gboolean              disconnect_first)
{
  g_autoptr (GListStore) store = NULL;
  guint n_items;
  guint i;

  if (self->client == NULL)
    return;

  store = bluetooth_client_get_devices (self->client);
  n_items = g_list_model_get_n_items (G_LIST_MODEL (store));

  if (disconnect_first)
    {
      for (i = 0; i < n_items; i++)
        {
          g_autoptr (GObject) device = g_list_model_get_item (G_LIST_MODEL (store), i);

          g_signal_handlers_disconnect_by_data (device, self);
        }
      g_hash_table_remove_all (self->device_notify_connected);
    }

  for (i = 0; i < n_items; i++)
    {
      g_autoptr (GObject) device = g_list_model_get_item (G_LIST_MODEL (store), i);

      bt_client_connect_device_notify (self, device);
    }
}

static void
bt_client_default_adapter_powered_cb (GObject              *client,
                                      GParamSpec           *pspec,
                                      ShellBluetoothClient *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), client_props[CLIENT_PROP_ACTIVE]);
}

static void
bt_client_default_adapter_state_cb (GObject              *client,
                                    GParamSpec           *pspec,
                                    ShellBluetoothClient *self)
{
  self->predicted_state = -1;
  g_object_notify_by_pspec (G_OBJECT (self), client_props[CLIENT_PROP_ADAPTER_STATE]);
}

static void
bt_client_default_adapter_cb (GObject              *client,
                              GParamSpec           *pspec,
                              ShellBluetoothClient *self)
{
  bt_client_connect_all_devices (self, TRUE);
  g_signal_emit (self, client_signals[DEVICES_CHANGED], 0);

  g_object_notify_by_pspec (G_OBJECT (self), client_props[CLIENT_PROP_ACTIVE]);
}

static void
bt_client_device_removed_cb (GObject              *client,
                             const char           *path,
                             ShellBluetoothClient *self)
{
  g_hash_table_remove (self->device_notify_connected, path);
  g_signal_emit (self, client_signals[DEVICE_REMOVED], 0, path);
  g_signal_emit (self, client_signals[DEVICES_CHANGED], 0);
}

static void
bt_client_device_added_cb (GObject              *client,
                           GObject              *device,
                           ShellBluetoothClient *self)
{
  bt_client_connect_device_notify (self, device);
  g_signal_emit (self, client_signals[DEVICES_CHANGED], 0);
}

/* Creates the GnomeBluetooth client; it is only done once BlueZ is there */
static void
bt_client_ensure_client (ShellBluetoothClient *self)
{
  if (self->client != NULL)
    return;

  self->client = bluetooth_client_new ();

  g_signal_connect_object (self->client, "notify::default-adapter-powered",
                           G_CALLBACK (bt_client_default_adapter_powered_cb), self, 0);
  g_signal_connect_object (self->client, "notify::default-adapter-state",
                           G_CALLBACK (bt_client_default_adapter_state_cb), self, 0);
  g_signal_connect_object (self->client, "notify::default-adapter",
                           G_CALLBACK (bt_client_default_adapter_cb), self, 0);
  g_signal_connect_object (self->client, "device-removed",
                           G_CALLBACK (bt_client_device_removed_cb), self, 0);
  g_signal_connect_object (self->client, "device-added",
                           G_CALLBACK (bt_client_device_added_cb), self, 0);

  bt_client_connect_all_devices (self, FALSE);

  /* The state of the client changed from absent to what it is now */
  g_object_notify_by_pspec (G_OBJECT (self), client_props[CLIENT_PROP_ACTIVE]);
  g_object_notify_by_pspec (G_OBJECT (self), client_props[CLIENT_PROP_ADAPTER_STATE]);
  g_signal_emit (self, client_signals[DEVICES_CHANGED], 0);
}

static void
bt_client_bluez_appeared_cb (GDBusConnection *connection,
                             const char      *name,
                             const char      *name_owner,
                             gpointer         user_data)
{
  bt_client_ensure_client (user_data);
}

static void
bt_client_rfkill_notify_cb (GObject              *proxy,
                            GParamSpec           *pspec,
                            ShellBluetoothClient *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), client_props[CLIENT_PROP_AVAILABLE]);
}

static void
bt_client_rfkill_proxy_ready_cb (GObject      *source,
                                 GAsyncResult *res,
                                 gpointer      user_data)
{
  ShellBluetoothClient *self;
  g_autoptr (GError) error = NULL;
  ShellBluetoothRfkill *proxy;

  proxy = shell_bluetooth_rfkill_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Failed to connect to the rfkill service: %s", error->message);
      return;
    }

  self = user_data;
  self->proxy = proxy;
  g_signal_connect_object (proxy, "notify::bluetooth-has-airplane-mode",
                           G_CALLBACK (bt_client_rfkill_notify_cb), self, 0);
  g_signal_connect_object (proxy, "notify::bluetooth-hardware-airplane-mode",
                           G_CALLBACK (bt_client_rfkill_notify_cb), self, 0);

  g_object_notify_by_pspec (G_OBJECT (self), client_props[CLIENT_PROP_AVAILABLE]);
}

static void
bt_client_state_change_failed_cb (gpointer user_data)
{
  ShellBluetoothClient *self = user_data;

  self->state_change_failed_id = 0;
  self->predicted_state = -1;
  g_object_notify_by_pspec (G_OBJECT (self), client_props[CLIENT_PROP_ADAPTER_STATE]);
}

/* toggleActive () */
static void
bt_client_toggle_active (ShellBluetoothClient *self)
{
  gboolean active = bt_client_get_active (self);

  /* on many systems, there's a significant delay until the rfkill
   * state results in an adapter state change; work around that by
   * overriding the current state with the expected transition */
  self->predicted_state = active ? ADAPTER_STATE_TURNING_OFF : ADAPTER_STATE_TURNING_ON;
  g_object_notify_by_pspec (G_OBJECT (self), client_props[CLIENT_PROP_ADAPTER_STATE]);

  /* toggling the state *really* should result in an adapter-state
   * change eventually (even on error), but just to be sure to not
   * be stuck with the overriden state, clear it after a timeout */
  g_clear_handle_id (&self->state_change_failed_id, g_source_remove);
  self->state_change_failed_id =
    g_timeout_add_seconds_once (STATE_CHANGE_FAILED_TIMEOUT_SECS,
                                bt_client_state_change_failed_cb, self);
  g_source_set_name_by_id (self->state_change_failed_id,
                           "[gnome-shell] bt_client_state_change_failed_cb");

  if (self->proxy != NULL)
    shell_bluetooth_rfkill_set_bluetooth_airplane_mode (SHELL_BLUETOOTH_RFKILL (self->proxy),
                                                        active);

  if (self->client != NULL && !device_get_boolean (self->client, "default-adapter-powered"))
    g_object_set (self->client, "default-adapter-powered", TRUE, NULL);
}

/* What a toggled device tells its item when it is done */
typedef struct
{
  void (* done) (gpointer user_data);
  gpointer user_data;
  GDestroyNotify destroy;
  GObject *device;
  gboolean connect;
  ShellBluetoothClient *client;
} ToggleDeviceData;

static void
toggle_device_data_free (ToggleDeviceData *data)
{
  if (data->destroy != NULL)
    data->destroy (data->user_data);
  g_clear_object (&data->device);
  g_clear_object (&data->client);
  g_free (data);
}

static void
bt_client_connect_service_cb (GObject      *source,
                              GAsyncResult *res,
                              gpointer      user_data)
{
  ToggleDeviceData *data = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *alias = device_dup_alias (data->device);

  if (bluetooth_client_connect_service_finish (source, res, &error))
    {
      g_debug ("Device \"%s\" %s", alias, data->connect ? "connected" : "disconnected");
    }
  else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      toggle_device_data_free (data);
      return;
    }
  else
    {
      g_warning ("Failed to %s device \"%s\": %s",
                 data->connect ? "connect" : "disconnect", alias, error->message);
    }

  if (data->done != NULL)
    data->done (data->user_data);

  toggle_device_data_free (data);
}

/* toggleDevice (): connects or disconnects the device; @done is called when
 * it is finished, unless the shell shut down meanwhile */
static void
bt_client_toggle_device (ShellBluetoothClient *self,
                         GObject              *device,
                         void                (* done) (gpointer user_data),
                         gpointer              user_data,
                         GDestroyNotify        destroy)
{
  ToggleDeviceData *data;
  g_autofree char *alias = device_dup_alias (device);
  gboolean connect = !device_get_boolean (device, "connected");

  if (self->client == NULL)
    {
      if (done != NULL)
        done (user_data);
      if (destroy != NULL)
        destroy (user_data);
      return;
    }

  g_debug ("%s device \"%s\"", connect ? "Connect" : "Disconnect", alias);

  data = g_new0 (ToggleDeviceData, 1);
  data->done = done;
  data->user_data = user_data;
  data->destroy = destroy;
  data->device = g_object_ref (device);
  data->connect = connect;
  data->client = g_object_ref (self);

  bluetooth_client_connect_service (self->client,
                                    bluetooth_device_get_object_path (device),
                                    connect,
                                    self->cancellable,
                                    bt_client_connect_service_cb,
                                    data);
}

static void
bt_client_shutdown_cb (ShellGlobal          *global,
                       ShellBluetoothClient *self)
{
  g_cancellable_cancel (self->cancellable);
  g_clear_handle_id (&self->devices_changed_id, g_source_remove);
  g_clear_handle_id (&self->state_change_failed_id, g_source_remove);
  g_clear_handle_id (&self->bluez_watch_id, g_bus_unwatch_name);
}

static void
shell_bluetooth_client_constructed (GObject *object)
{
  ShellBluetoothClient *self = SHELL_BLUETOOTH_CLIENT (object);

  G_OBJECT_CLASS (shell_bluetooth_client_parent_class)->constructed (object);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (bt_client_shutdown_cb), self, 0);

  shell_bluetooth_rfkill_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                            G_DBUS_PROXY_FLAGS_NONE,
                                            RFKILL_BUS_NAME,
                                            RFKILL_OBJECT_PATH,
                                            self->cancellable,
                                            bt_client_rfkill_proxy_ready_cb,
                                            self);

  self->bluez_watch_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
                                           "org.bluez",
                                           G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                           bt_client_bluez_appeared_cb,
                                           NULL,
                                           self,
                                           NULL);
}

static void
shell_bluetooth_client_dispose (GObject *object)
{
  ShellBluetoothClient *self = SHELL_BLUETOOTH_CLIENT (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  g_clear_handle_id (&self->devices_changed_id, g_source_remove);
  g_clear_handle_id (&self->state_change_failed_id, g_source_remove);
  g_clear_handle_id (&self->bluez_watch_id, g_bus_unwatch_name);

  g_clear_object (&self->proxy);
  g_clear_object (&self->client);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->device_notify_connected, g_hash_table_unref);

  G_OBJECT_CLASS (shell_bluetooth_client_parent_class)->dispose (object);
}

static void
shell_bluetooth_client_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  ShellBluetoothClient *self = SHELL_BLUETOOTH_CLIENT (object);

  switch (prop_id)
    {
    case CLIENT_PROP_AVAILABLE:
      g_value_set_boolean (value, bt_client_get_available (self));
      break;

    case CLIENT_PROP_ACTIVE:
      g_value_set_boolean (value, bt_client_get_active (self));
      break;

    case CLIENT_PROP_ADAPTER_STATE:
      g_value_set_int (value, bt_client_get_adapter_state (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_bluetooth_client_class_init (ShellBluetoothClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_bluetooth_client_constructed;
  object_class->dispose = shell_bluetooth_client_dispose;
  object_class->get_property = shell_bluetooth_client_get_property;

  client_props[CLIENT_PROP_AVAILABLE] =
    g_param_spec_boolean ("available", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  client_props[CLIENT_PROP_ACTIVE] =
    g_param_spec_boolean ("active", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /* The value of BluetoothAdapterState */
  client_props[CLIENT_PROP_ADAPTER_STATE] =
    g_param_spec_int ("adapter-state", NULL, NULL,
                      ADAPTER_STATE_ABSENT, ADAPTER_STATE_OFF, ADAPTER_STATE_ABSENT,
                      G_PARAM_READABLE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_CLIENT_PROPS, client_props);

  client_signals[DEVICES_CHANGED] =
    g_signal_new ("devices-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  client_signals[DEVICE_REMOVED] =
    g_signal_new ("device-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
shell_bluetooth_client_init (ShellBluetoothClient *self)
{
  self->cancellable = g_cancellable_new ();
  self->device_notify_connected = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->predicted_state = -1;
}

static ShellBluetoothClient *
bt_client_new (void)
{
  return g_object_new (SHELL_TYPE_BLUETOOTH_CLIENT, NULL);
}

/* ======================================================================== */
/* ShellBluetoothDeviceItem                                                 */
/* ======================================================================== */

#define SHELL_TYPE_BLUETOOTH_DEVICE_ITEM (shell_bluetooth_device_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellBluetoothDeviceItem, shell_bluetooth_device_item,
                      SHELL, BLUETOOTH_DEVICE_ITEM, ShellPopupBaseMenuItem)

struct _ShellBluetoothDeviceItem
{
  ShellPopupBaseMenuItem parent;

  GObject *device;
  ShellBluetoothClient *client;
  ShellSpinner *spinner;        /* weak */
};

G_DEFINE_FINAL_TYPE (ShellBluetoothDeviceItem, shell_bluetooth_device_item,
                     SHELL_TYPE_POPUP_BASE_MENU_ITEM)

/* _updateAccessibleName () */
static void
device_item_update_accessible_name (ShellBluetoothDeviceItem *self)
{
  g_autofree char *alias = device_dup_alias (self->device);
  g_autofree char *name = NULL;

  if (device_get_boolean (self->device, "connected"))
    /* Translators: %s is a device name like "MyPhone" */
    name = g_strdup_printf (_("Disconnect %s"), alias);
  else
    /* Translators: %s is a device name like "MyPhone" */
    name = g_strdup_printf (_("Connect to %s"), alias);

  clutter_actor_set_accessible_name (CLUTTER_ACTOR (self), name);
}

static void
device_item_device_notify_cb (GObject                  *device,
                              GParamSpec               *pspec,
                              ShellBluetoothDeviceItem *self)
{
  device_item_update_accessible_name (self);
}

static gboolean
device_item_connected_to_subtitle (GBinding     *binding,
                                   const GValue *from_value,
                                   GValue       *to_value,
                                   gpointer      user_data)
{
  g_value_set_string (to_value,
                      g_value_get_boolean (from_value) ? _("Disconnect") : _("Connect"));
  return TRUE;
}

static void
device_item_toggled_cb (gpointer user_data)
{
  GWeakRef *item_ref = user_data;
  g_autoptr (ShellBluetoothDeviceItem) self = g_weak_ref_get (item_ref);

  if (self != NULL && self->spinner != NULL)
    shell_spinner_stop (self->spinner);
}

static void
device_item_weak_ref_free (GWeakRef *item_ref)
{
  g_weak_ref_clear (item_ref);
  g_free (item_ref);
}

/* _toggleConnected () */
static void
device_item_activate_cb (ShellBluetoothDeviceItem *self,
                         const ClutterEvent       *event,
                         gpointer                  user_data)
{
  GWeakRef *item_ref;

  if (self->spinner != NULL)
    shell_spinner_play (self->spinner);

  item_ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (item_ref, self);
  bt_client_toggle_device (self->client, self->device,
                           device_item_toggled_cb, item_ref,
                           (GDestroyNotify) device_item_weak_ref_free);
}

static ShellBluetoothDeviceItem *
device_item_new (GObject              *device,
                 ShellBluetoothClient *client)
{
  ShellBluetoothDeviceItem *self;
  ClutterActor *actor;
  ClutterActor *icon;
  ClutterActor *label;
  ClutterActor *subtitle;
  ClutterActor *spinner;

  self = g_object_new (SHELL_TYPE_BLUETOOTH_DEVICE_ITEM,
                       "extra-style-class", "bt-device-item",
                       NULL);
  actor = CLUTTER_ACTOR (self);

  self->device = g_object_ref (device);
  self->client = g_object_ref (client);

  icon = g_object_new (ST_TYPE_ICON,
                       "style-class", "popup-menu-icon",
                       NULL);
  clutter_actor_add_child (actor, icon);

  label = g_object_new (ST_TYPE_LABEL,
                        "x-expand", TRUE,
                        NULL);
  clutter_actor_add_child (actor, label);

  subtitle = g_object_new (ST_TYPE_LABEL,
                           "style-class", "device-subtitle",
                           NULL);
  clutter_actor_add_child (actor, subtitle);

  spinner = CLUTTER_ACTOR (shell_spinner_new (16, FALSE, TRUE));
  g_set_weak_pointer (&self->spinner, SHELL_SPINNER (spinner));
  clutter_actor_add_child (actor, spinner);

  g_object_bind_property (spinner, "visible",
                          subtitle, "visible",
                          G_BINDING_SYNC_CREATE | G_BINDING_INVERT_BOOLEAN);

  g_object_bind_property (device, "connectable",
                          self, "visible",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (device, "icon",
                          icon, "icon-name",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (device, "alias",
                          label, "text",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property_full (device, "connected",
                               subtitle, "text",
                               G_BINDING_SYNC_CREATE,
                               device_item_connected_to_subtitle,
                               NULL, NULL, NULL);

  g_signal_connect (self, "activate",
                    G_CALLBACK (device_item_activate_cb), NULL);
  g_signal_connect_object (device, "notify::alias",
                           G_CALLBACK (device_item_device_notify_cb), self, 0);
  g_signal_connect_object (device, "notify::connected",
                           G_CALLBACK (device_item_device_notify_cb), self, 0);
  device_item_update_accessible_name (self);

  return self;
}

static void
shell_bluetooth_device_item_dispose (GObject *object)
{
  ShellBluetoothDeviceItem *self = SHELL_BLUETOOTH_DEVICE_ITEM (object);

  g_clear_weak_pointer (&self->spinner);
  g_clear_object (&self->device);
  g_clear_object (&self->client);

  G_OBJECT_CLASS (shell_bluetooth_device_item_parent_class)->dispose (object);
}

static void
shell_bluetooth_device_item_class_init (ShellBluetoothDeviceItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_bluetooth_device_item_dispose;
}

static void
shell_bluetooth_device_item_init (ShellBluetoothDeviceItem *self)
{
}

/* ======================================================================== */
/* ShellBluetoothToggle                                                     */
/* ======================================================================== */

#define SHELL_TYPE_BLUETOOTH_TOGGLE (shell_bluetooth_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellBluetoothToggle, shell_bluetooth_toggle,
                      SHELL, BLUETOOTH_TOGGLE, ShellQuickMenuToggle)

struct _ShellBluetoothToggle
{
  ShellQuickMenuToggle parent;

  ShellBluetoothClient *client;
  GHashTable *device_items;          /* object path -> ShellBluetoothDeviceItem, not owned */
  ShellPopupMenuSection *device_section;
  ShellPopupMenuItem *placeholder_item; /* weak */
};

G_DEFINE_FINAL_TYPE (ShellBluetoothToggle, shell_bluetooth_toggle,
                     SHELL_TYPE_QUICK_MENU_TOGGLE)

/* _updatePlaceholder () */
static void
toggle_update_placeholder (ShellBluetoothToggle *self)
{
  if (self->placeholder_item == NULL)
    return;

  shell_popup_menu_item_set_text (self->placeholder_item,
                                  bt_client_get_active (self->client)
                                  ? _("No available or connected devices")
                                  : _("Turn on Bluetooth to connect to devices"));
}

/* _updatePlaceholderRelation () */
static void
toggle_update_placeholder_relation (ShellBluetoothToggle *self)
{
  AtkObject *accel;
  AtkObject *placeholder_accel;
  ClutterActor *section_actor;

  if (self->placeholder_item == NULL || self->device_section == NULL)
    return;

  accel = clutter_actor_get_accessible (CLUTTER_ACTOR (self));
  placeholder_accel = clutter_actor_get_accessible (CLUTTER_ACTOR (self->placeholder_item));
  section_actor = shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (self->device_section));

  if (clutter_actor_is_visible (section_actor))
    atk_object_remove_relationship (accel, ATK_RELATION_DESCRIBED_BY, placeholder_accel);
  else
    atk_object_add_relationship (accel, ATK_RELATION_DESCRIBED_BY, placeholder_accel);
}

/* _updateDeviceVisibility () */
static void
toggle_update_device_visibility (ShellBluetoothToggle *self)
{
  GHashTableIter iter;
  gpointer value;
  gboolean any_visible = FALSE;

  if (self->device_section == NULL)
    return;

  g_hash_table_iter_init (&iter, self->device_items);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      if (clutter_actor_is_visible (CLUTTER_ACTOR (value)))
        {
          any_visible = TRUE;
          break;
        }
    }

  shell_status_set_visible (shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (self->device_section)),
                     any_visible);
  toggle_update_placeholder_relation (self);
}

static int
compare_devices (gconstpointer a,
                 gconstpointer b)
{
  GObject *dev1 = *(GObject * const *) a;
  GObject *dev2 = *(GObject * const *) b;
  gboolean connected1 = device_get_boolean (dev1, "connected");
  gboolean connected2 = device_get_boolean (dev2, "connected");
  g_autofree char *alias1 = NULL;
  g_autofree char *alias2 = NULL;

  if (connected1 != connected2)
    return connected2 - connected1;

  alias1 = device_dup_alias (dev1);
  alias2 = device_dup_alias (dev2);
  return g_utf8_collate (alias1, alias2);
}

/* _getSortedDevices (): connected devices first, then by name */
static GPtrArray *
toggle_get_sorted_devices (ShellBluetoothToggle *self)
{
  GPtrArray *devices = bt_client_get_devices (self->client);

  g_ptr_array_sort (devices, compare_devices);
  return devices;
}

static void
toggle_device_item_destroy_cb (ClutterActor         *item,
                               ShellBluetoothToggle *self)
{
  GHashTableIter iter;
  gpointer value;

  if (self->device_items == NULL)
    return;

  g_hash_table_iter_init (&iter, self->device_items);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      if (value == item)
        {
          g_hash_table_iter_remove (&iter);
          return;
        }
    }
}

/* _removeDevice () */
static void
toggle_remove_device (ShellBluetoothToggle *self,
                      const char           *path)
{
  ClutterActor *item = g_hash_table_lookup (self->device_items, path);

  if (item != NULL)
    clutter_actor_destroy (item);

  toggle_update_device_visibility (self);
}

/* _reorderDeviceItems () */
static void
toggle_reorder_device_items (ShellBluetoothToggle *self)
{
  g_autoptr (GPtrArray) devices = toggle_get_sorted_devices (self);
  guint i;

  if (self->device_section == NULL)
    return;

  for (i = 0; i < devices->len; i++)
    {
      GObject *device = g_ptr_array_index (devices, i);
      GObject *item = g_hash_table_lookup (self->device_items,
                                           bluetooth_device_get_object_path (device));

      if (item == NULL)
        continue;

      shell_popup_menu_base_move_menu_item (SHELL_POPUP_MENU_BASE (self->device_section),
                                            item, i);
    }
}

static void
toggle_device_item_visible_cb (GObject              *item,
                               GParamSpec           *pspec,
                               ShellBluetoothToggle *self)
{
  toggle_update_device_visibility (self);
}

/* _sync () */
static void
toggle_sync (ShellBluetoothToggle *self)
{
  g_autoptr (GPtrArray) devices = toggle_get_sorted_devices (self);
  GObject *first_connected = NULL;
  guint n_connected = 0;
  guint i;

  if (self->device_section == NULL)
    return;

  for (i = 0; i < devices->len; i++)
    {
      GObject *device = g_ptr_array_index (devices, i);
      const char *path = bluetooth_device_get_object_path (device);
      ShellBluetoothDeviceItem *item;

      if (device_get_boolean (device, "connected"))
        {
          if (n_connected == 0)
            first_connected = device;
          n_connected++;
        }

      if (path == NULL || g_hash_table_contains (self->device_items, path))
        continue;

      item = device_item_new (device, self->client);
      g_signal_connect_object (item, "notify::visible",
                               G_CALLBACK (toggle_device_item_visible_cb), self, 0);
      g_signal_connect_object (item, "destroy",
                               G_CALLBACK (toggle_device_item_destroy_cb), self, 0);

      shell_popup_menu_base_add_menu_item (SHELL_POPUP_MENU_BASE (self->device_section),
                                           G_OBJECT (item));
      g_hash_table_insert (self->device_items, g_strdup (path), item);
    }

  if (n_connected > 1)
    {
      /* Translators: This is the number of connected bluetooth devices */
      g_autofree char *subtitle = g_strdup_printf (ngettext ("%d Connected", "%d Connected", n_connected),
                                                   n_connected);

      shell_quick_menu_toggle_set_subtitle (SHELL_QUICK_MENU_TOGGLE (self), subtitle);
    }
  else if (n_connected == 1)
    {
      g_autofree char *alias = device_dup_alias (first_connected);

      shell_quick_menu_toggle_set_subtitle (SHELL_QUICK_MENU_TOGGLE (self), alias);
    }
  else
    {
      shell_quick_menu_toggle_set_subtitle (SHELL_QUICK_MENU_TOGGLE (self), NULL);
    }

  toggle_update_device_visibility (self);
}

/* _onActiveChanged () */
static void
toggle_on_active_changed (ShellBluetoothToggle *self)
{
  g_autoptr (GList) items = g_hash_table_get_values (self->device_items);
  GList *l;

  toggle_update_placeholder (self);

  for (l = items; l != NULL; l = l->next)
    clutter_actor_destroy (CLUTTER_ACTOR (l->data));
  g_hash_table_remove_all (self->device_items);

  toggle_sync (self);
}

static void
toggle_active_notify_cb (GObject              *client,
                         GParamSpec           *pspec,
                         ShellBluetoothToggle *self)
{
  toggle_on_active_changed (self);
}

static void
toggle_devices_changed_cb (ShellBluetoothClient *client,
                           ShellBluetoothToggle *self)
{
  toggle_sync (self);
}

static void
toggle_device_removed_cb (ShellBluetoothClient *client,
                          const char           *path,
                          ShellBluetoothToggle *self)
{
  toggle_remove_device (self, path);
}

static void
toggle_menu_open_state_changed_cb (ShellPopupMenuBase  *menu,
                                   gboolean             open,
                                   ShellBluetoothToggle *self)
{
  /* We don't reorder the list while the menu is open,
   * so do it now to start with the proper order */
  if (open)
    toggle_reorder_device_items (self);
}

static void
toggle_clicked_cb (ShellBluetoothToggle *self)
{
  bt_client_toggle_active (self->client);
}

/* _getIconNameFromState () */
static const char *
toggle_get_icon_name_from_state (int state)
{
  switch (state)
    {
    case ADAPTER_STATE_ON:
      return "bluetooth-active-symbolic";

    case ADAPTER_STATE_OFF:
    case ADAPTER_STATE_ABSENT:
      return "bluetooth-disabled-symbolic";

    case ADAPTER_STATE_TURNING_ON:
    case ADAPTER_STATE_TURNING_OFF:
      return "bluetooth-acquiring-symbolic";

    default:
      g_warning ("Unexpected state %d", state);
      return "";
    }
}

static gboolean
toggle_adapter_state_to_icon_name (GBinding     *binding,
                                   const GValue *from_value,
                                   GValue       *to_value,
                                   gpointer      user_data)
{
  g_value_set_string (to_value,
                      toggle_get_icon_name_from_state (g_value_get_int (from_value)));
  return TRUE;
}

static ShellBluetoothToggle *
toggle_new (void)
{
  return g_object_new (SHELL_TYPE_BLUETOOTH_TOGGLE,
                       "title", _("Bluetooth"),
                       "menu-button-accessible-name", _("Open Bluetooth menu"),
                       NULL);
}

/* The part of the constructor which needs the client */
static void
toggle_setup (ShellBluetoothToggle *self,
              ShellBluetoothClient *client)
{
  ShellPopupMenuBase *menu =
    SHELL_POPUP_MENU_BASE (shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self)));
  ShellPopupMenuItem *placeholder;
  ClutterText *placeholder_text;

  shell_quick_toggle_menu_set_header (shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self)),
                                      "bluetooth-active-symbolic", _("Bluetooth"), NULL);

  self->client = g_object_ref (client);

  self->device_section = shell_popup_menu_section_new ();
  shell_popup_menu_base_add_menu_item (menu, G_OBJECT (self->device_section));

  placeholder = g_object_new (SHELL_TYPE_POPUP_MENU_ITEM,
                              "text", "",
                              "extra-style-class", "bt-menu-placeholder",
                              "activatable", FALSE,
                              "active-on-hover", FALSE,
                              NULL);
  clutter_actor_set_reactive (CLUTTER_ACTOR (placeholder), FALSE);
  st_widget_set_can_focus (ST_WIDGET (placeholder), FALSE);
  g_set_weak_pointer (&self->placeholder_item, placeholder);
  placeholder_text = st_label_get_clutter_text (shell_popup_menu_item_get_label (placeholder));
  clutter_text_set_ellipsize (placeholder_text, PANGO_ELLIPSIZE_NONE);
  clutter_text_set_line_wrap (placeholder_text, TRUE);
  shell_popup_menu_base_add_menu_item (menu, G_OBJECT (placeholder));

  g_object_bind_property (shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (self->device_section)),
                          "visible",
                          placeholder, "visible",
                          G_BINDING_SYNC_CREATE | G_BINDING_INVERT_BOOLEAN);

  shell_popup_menu_base_add_menu_item (menu,
                                       g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL));
  shell_popup_menu_base_add_settings_action (menu, _("Bluetooth Settings"),
                                             "gnome-bluetooth-panel.desktop");

  g_object_bind_property (client, "available",
                          self, "visible",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (client, "active",
                          self, "checked",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property_full (client, "adapter-state",
                               self, "icon-name",
                               G_BINDING_SYNC_CREATE,
                               toggle_adapter_state_to_icon_name,
                               NULL, NULL, NULL);

  g_signal_connect_object (client, "notify::active",
                           G_CALLBACK (toggle_active_notify_cb), self, 0);
  g_signal_connect_object (client, "devices-changed",
                           G_CALLBACK (toggle_devices_changed_cb), self, 0);
  g_signal_connect_object (client, "device-removed",
                           G_CALLBACK (toggle_device_removed_cb), self, 0);

  g_signal_connect_object (menu, "open-state-changed",
                           G_CALLBACK (toggle_menu_open_state_changed_cb), self, 0);

  g_signal_connect (self, "clicked", G_CALLBACK (toggle_clicked_cb), NULL);

  toggle_update_placeholder (self);
  toggle_sync (self);
}

static void
shell_bluetooth_toggle_dispose (GObject *object)
{
  ShellBluetoothToggle *self = SHELL_BLUETOOTH_TOGGLE (object);

  g_clear_weak_pointer (&self->placeholder_item);
  g_clear_object (&self->device_section);
  g_clear_pointer (&self->device_items, g_hash_table_unref);
  g_clear_object (&self->client);

  G_OBJECT_CLASS (shell_bluetooth_toggle_parent_class)->dispose (object);
}

static void
shell_bluetooth_toggle_class_init (ShellBluetoothToggleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_bluetooth_toggle_dispose;
}

static void
shell_bluetooth_toggle_init (ShellBluetoothToggle *self)
{
  self->device_items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

/* ======================================================================== */
/* ShellBluetoothIndicator                                                  */
/* ======================================================================== */

struct _ShellBluetoothIndicator
{
  ShellSystemIndicator parent;

  ShellBluetoothClient *client;
  StIcon *indicator;            /* weak */
};

G_DEFINE_FINAL_TYPE (ShellBluetoothIndicator, shell_bluetooth_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

/* _sync () */
static void
bluetooth_indicator_sync (ShellBluetoothIndicator *self)
{
  g_autoptr (GPtrArray) devices = NULL;
  gboolean any_connected = FALSE;
  guint i;

  if (self->indicator == NULL)
    return;

  devices = bt_client_get_devices (self->client);
  for (i = 0; i < devices->len; i++)
    {
      if (device_get_boolean (g_ptr_array_index (devices, i), "connected"))
        {
          any_connected = TRUE;
          break;
        }
    }

  shell_status_set_visible (CLUTTER_ACTOR (self->indicator), any_connected);
}

static void
bluetooth_indicator_devices_changed_cb (ShellBluetoothClient    *client,
                                        ShellBluetoothIndicator *self)
{
  bluetooth_indicator_sync (self);
}

static void
shell_bluetooth_indicator_constructed (GObject *object)
{
  ShellBluetoothIndicator *self = SHELL_BLUETOOTH_INDICATOR (object);
  ShellBluetoothToggle *toggle;
  StIcon *indicator;

  G_OBJECT_CLASS (shell_bluetooth_indicator_parent_class)->constructed (object);

  self->client = bt_client_new ();
  g_signal_connect_object (self->client, "devices-changed",
                           G_CALLBACK (bluetooth_indicator_devices_changed_cb), self, 0);

  indicator = shell_system_indicator_add_indicator (SHELL_SYSTEM_INDICATOR (self));
  g_set_weak_pointer (&self->indicator, indicator);
  st_icon_set_icon_name (indicator, "bluetooth-active-symbolic");

  toggle = toggle_new ();
  toggle_setup (toggle, self->client);
  shell_system_indicator_add_quick_settings_item (SHELL_SYSTEM_INDICATOR (self),
                                                  CLUTTER_ACTOR (toggle));

  bluetooth_indicator_sync (self);
}

static void
shell_bluetooth_indicator_dispose (GObject *object)
{
  ShellBluetoothIndicator *self = SHELL_BLUETOOTH_INDICATOR (object);

  g_clear_weak_pointer (&self->indicator);
  g_clear_object (&self->client);

  G_OBJECT_CLASS (shell_bluetooth_indicator_parent_class)->dispose (object);
}

static void
shell_bluetooth_indicator_class_init (ShellBluetoothIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_bluetooth_indicator_constructed;
  object_class->dispose = shell_bluetooth_indicator_dispose;
}

static void
shell_bluetooth_indicator_init (ShellBluetoothIndicator *self)
{
}

/**
 * shell_bluetooth_indicator_new:
 *
 * Creates the Bluetooth indicator: the icon of the top bar, which is shown
 * while a device is connected, and the Bluetooth toggle of the quick settings
 * (`new Indicator ()`).
 *
 * Returns: (transfer floating): a new #ShellBluetoothIndicator
 */
ShellBluetoothIndicator *
shell_bluetooth_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_BLUETOOTH_INDICATOR, NULL);
}
