/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * network.c: The network indicator of the top bar and its quick settings
 * toggles. Port of js/ui/status/network.js.
 *
 * The module is a tree of private classes, all but the indicator are only
 * used from this file. The JS names and what they are now:
 *
 *   NMMenuItem                    ShellNmMenuItem (abstract, derivable)
 *   NMSectionItem                 ShellNmSectionItem (abstract, derivable)
 *   NMConnectionItem,             ShellNmConnectionItem (final); the kind
 *    NMDeviceConnectionItem,       property tells the device connection
 *    NMVpnConnectionItem           from the VPN connection
 *   NMDeviceItem, NMWiredDeviceItem,
 *    NMModemDeviceItem,            ShellNmDeviceItem (final) with a kind
 *    NMBluetoothDeviceItem
 *   NMWirelessDeviceItem          ShellNmWirelessDeviceItem (final)
 *   WirelessNetwork               ShellNmWirelessNetwork (final GObject)
 *   NMWirelessNetworkItem         ShellNmWirelessNetworkItem (final)
 *   NMToggle, NMVpnToggle,
 *    NMDeviceToggle, NMWiredToggle,
 *    NMWirelessToggle, NMModemToggle,
 *    NMBluetoothToggle            ShellNmToggle (final) with a kind
 *   ItemSorter                    ItemSorter (a struct)
 *   CaptivePortalHandler          ShellNmCaptivePortalHandler (final GObject)
 *   Indicator                     ShellNetworkIndicator
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The `name` property of the menu items is `item-name` (ClutterActor
 *    already has a `name`).
 *  - The kinds above are enumerations that the code switches on, where the
 *    JS had a subclass with an override.
 *  - Everything that talked to NetworkManager or to the secret service
 *    without anyone watching is silent when it fails: a missing system bus or
 *    NetworkManager is the normal case in a test session (the JS logged the
 *    errors). The failure to get the permission to control the network is
 *    only a debug message.
 *  - The modem item creates its modem before it syncs with its device (the
 *    JS constructor ran the sync of the base class first, which read the
 *    signal quality of a modem that did not exist yet).
 *  - A device that fails the Wi-Fi scan counts as scanned (the JS rejected
 *    the scan and left the spinner running); the scan is polled for 1.5 s
 *    like before.
 *  - The empty set of devices of a device toggle has no default name (the JS
 *    passed `undefined` to `NM.Device.disambiguate_names ()` and threw); the
 *    title is kept.
 *  - `NM.Device.disconnect ()` and `NM.Client.deactivate_connection ()` are
 *    the asynchronous calls.
 *  - The 'changed' handler of the connection of a device item is removed
 *    together with the connection item (the JS kept it, and threw if the
 *    connection changed afterwards).
 *  - The network objects of the Wi-Fi device item are kept in the order they
 *    were found, like the JS array, the access points of a network too.
 *  - `NM.UtilsSecurityType.UNKOWN` and `.OWE_TM`, which the JS compared with,
 *    do not exist: a network is secure unless it is open or uses OWE.
 *  - The captive portal handler asks the owner of the indicator to hide the
 *    overview (`Main.overview.hide ()`) with the new signal
 *    #ShellNetworkIndicator::overview-hide-requested.
 *  - After ShellGlobal::shutdown the completions of the asynchronous calls
 *    do nothing, and the client is not created if it was not yet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <atk/atk.h>
#include <clutter/clutter.h>
#include <glib/gi18n-lib.h>
#include <NetworkManager.h>
#include <polkit/polkit.h>

#include "misc/modem-manager.h"
#include "shell-app.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "st.h"
#include "ui/animation.h"
#include "ui/main.h"
#include "ui/message-tray.h"
#include "ui/panel-menu.h"
#include "ui/panel.h"
#include "ui/popup-menu.h"
#include "ui/quick-settings.h"
#include "ui/session-mode.h"
#include "ui/status/network.h"

#ifdef HAVE_PORTAL_HELPER
#include "portal-helper-dbus.h"
#endif

#define WIFI_SCAN_FREQUENCY 15
#define WIFI_SCAN_POLL_INTERVAL 1500 /* milliseconds */
#define MAX_VISIBLE_NETWORKS 8

#define POLKIT_NETWORK_CONTROL "org.freedesktop.NetworkManager.network-control"
#define POLKIT_SETTINGS_MODIFY_SYSTEM "org.freedesktop.NetworkManager.settings.modify.system"

/* Whether the result of org.gnome.Shell.PortalHelper::StatusChanged ends
 * the captive portal login */
typedef enum
{
  PORTAL_HELPER_RESULT_CANCELLED = 0,
  PORTAL_HELPER_RESULT_COMPLETED = 1,
  PORTAL_HELPER_RESULT_RECHECK   = 2,
} PortalHelperResult;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static ShellSessionMode *
get_session_mode (void)
{
  return shell_main_get_session_mode (shell_main_get_default ());
}

/* `Main.sessionMode.has_windows` */
static gboolean
session_has_windows (void)
{
  ShellSessionMode *session_mode = get_session_mode ();
  gboolean has_windows = FALSE;

  if (session_mode != NULL)
    g_object_get (session_mode, "has-windows", &has_windows, NULL);

  return has_windows;
}

/* signalToIcon () */
static const char *
signal_to_icon (int value)
{
  if (value < 20)
    return "none";
  else if (value < 40)
    return "weak";
  else if (value < 50)
    return "ok";
  else if (value < 80)
    return "good";
  else
    return "excellent";
}

/* ssidToLabel () */
static char *
ssid_to_label (GBytes *ssid)
{
  gsize len = 0;
  const guint8 *data = g_bytes_get_data (ssid, &len);
  char *label = nm_utils_ssid_to_utf8 (data, len);

  if (label == NULL || *label == '\0')
    {
      g_free (label);
      label = g_strdup (_("<unknown>"));
    }

  return label;
}

static void
launch_settings_panel_cb (GObject      *source_object,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!shell_app_activate_action_finish (SHELL_APP (source_object), res, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_message ("Failed to launch Settings panel: %s", error->message);
}

/* launchSettingsPanel (panel, ...args); @args is NULL terminated */
static void
launch_settings_panel (const char         *panel,
                       const char * const *args)
{
  GVariantBuilder panel_args;
  GVariantBuilder params;
  g_autoptr (GVariant) param = NULL;
  ShellApp *app;
  int i;

  g_variant_builder_init (&panel_args, G_VARIANT_TYPE ("av"));
  for (i = 0; args[i] != NULL; i++)
    g_variant_builder_add (&panel_args, "v", g_variant_new_string (args[i]));

  g_variant_builder_init (&params, G_VARIANT_TYPE ("av"));
  g_variant_builder_add (&params, "v", g_variant_new ("(sav)", panel, &panel_args));
  param = g_variant_ref_sink (g_variant_new ("av", &params));

  app = shell_app_system_lookup_app (shell_app_system_get_default (),
                                     "org.gnome.Settings.desktop");
  if (app == NULL)
    {
      g_message ("Failed to launch Settings panel: org.gnome.Settings.desktop is not installed");
      return;
    }

  shell_app_activate_action (app, "launch-panel", param, 0, -1, NULL,
                             launch_settings_panel_cb, NULL);
}

static void
set_actor_visible (gpointer actor,
                   gboolean visible)
{
  if (visible)
    clutter_actor_show (CLUTTER_ACTOR (actor));
  else
    clutter_actor_hide (CLUTTER_ACTOR (actor));
}

/* Checks and sets the accessible state of an actor */
static void
set_accessible_checked (ClutterActor *actor,
                        gboolean      checked)
{
  if (checked)
    clutter_actor_add_accessible_state (actor, ATK_STATE_CHECKED);
  else
    clutter_actor_remove_accessible_state (actor, ATK_STATE_CHECKED);
}

/* _canReachInternet () of the device items */
static gboolean
device_can_reach_internet (NMClient *client,
                           NMDevice *device)
{
  if (nm_client_get_primary_connection (client) !=
      nm_device_get_active_connection (device))
    return TRUE;

  return nm_client_get_connectivity (client) == NM_CONNECTIVITY_FULL;
}

/* ------------------------------------------------------------------------ */
/* ItemSorter                                                               */
/* ------------------------------------------------------------------------ */

/* Maintains a list of sorted items, and optionally a second one sorted by
 * recency. The sorter owns a reference to its items; the order by recency
 * holds the same objects. */
typedef struct
{
  GPtrArray *items_order;
  GPtrArray *items_mru_order; /* NULL unless the recency is tracked */
  GCompareFunc sort_func;
} ItemSorter;

/* The position of the first element that is not lower than @val
 * (`Util.lowerBound ()`) */
static guint
lower_bound (GPtrArray   *array,
             gpointer     val,
             GCompareFunc cmp)
{
  guint min, max, mid;
  int v;

  if (array->len == 0)
    return 0;

  min = 0;
  max = array->len;
  while (min + 1 < max)
    {
      mid = (min + max) / 2;
      v = cmp (g_ptr_array_index (array, mid), val);

      if (v < 0)
        min = mid + 1;
      else
        max = mid;
    }

  return (min == max || cmp (g_ptr_array_index (array, min), val) < 0) ? max : min;
}

static ItemSorter *
item_sorter_new (GCompareFunc sort_func,
                 gboolean     track_mru)
{
  ItemSorter *sorter = g_new0 (ItemSorter, 1);

  sorter->sort_func = sort_func;
  sorter->items_order = g_ptr_array_new_with_free_func (g_object_unref);
  if (track_mru)
    sorter->items_mru_order = g_ptr_array_new ();

  return sorter;
}

static void
item_sorter_free (ItemSorter *sorter)
{
  if (sorter == NULL)
    return;

  g_ptr_array_unref (sorter->items_order);
  g_clear_pointer (&sorter->items_mru_order, g_ptr_array_unref);
  g_free (sorter);
}

/* Drops all the items */
static void
item_sorter_clear (ItemSorter *sorter)
{
  if (sorter->items_mru_order != NULL)
    g_ptr_array_set_size (sorter->items_mru_order, 0);
  g_ptr_array_set_size (sorter->items_order, 0);
}

/* Removes @item; it can be gone afterwards */
static void
item_sorter_delete (ItemSorter *sorter,
                    gpointer    item)
{
  if (sorter->items_mru_order != NULL)
    g_ptr_array_remove (sorter->items_mru_order, item);
  g_ptr_array_remove (sorter->items_order, item);
}

static gpointer
item_sorter_first (ItemSorter *sorter)
{
  return sorter->items_order->len > 0 ? g_ptr_array_index (sorter->items_order, 0) : NULL;
}

static gpointer
item_sorter_first_by_mru (ItemSorter *sorter)
{
  g_assert (sorter->items_mru_order != NULL);

  return sorter->items_mru_order->len > 0 ? g_ptr_array_index (sorter->items_mru_order, 0) : NULL;
}

/* A copy of the items by recency, to iterate while they change */
static GPtrArray *
item_sorter_dup_items_by_mru (ItemSorter *sorter)
{
  GPtrArray *copy;
  guint i;

  g_assert (sorter->items_mru_order != NULL);

  copy = g_ptr_array_new_full (sorter->items_mru_order->len, g_object_unref);
  for (i = 0; i < sorter->items_mru_order->len; i++)
    g_ptr_array_add (copy, g_object_ref (g_ptr_array_index (sorter->items_mru_order, i)));

  return copy;
}

/* ------------------------------------------------------------------------ */
/* ShellNmMenuItem                                                          */
/* ------------------------------------------------------------------------ */

/* G_DECLARE_DERIVABLE_TYPE () defines the instance structure itself, which
 * these types (shared by the classes below) want to define with their
 * fields */
#define DECLARE_PRIVATE_DERIVABLE_TYPE(ModuleObjName, module_obj_name, MODULE, OBJ_NAME, ParentName) \
  GType module_obj_name##_get_type (void); \
  typedef struct _##ModuleObjName ModuleObjName; \
  typedef struct _##ModuleObjName##Class ModuleObjName##Class; \
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS \
  static inline ModuleObjName *MODULE##_##OBJ_NAME (gpointer ptr) \
  { \
    return G_TYPE_CHECK_INSTANCE_CAST (ptr, module_obj_name##_get_type (), ModuleObjName); \
  } \
  static inline ModuleObjName##Class *MODULE##_##OBJ_NAME##_CLASS (gpointer ptr) \
  { \
    return G_TYPE_CHECK_CLASS_CAST (ptr, module_obj_name##_get_type (), ModuleObjName##Class); \
  } \
  static inline gboolean MODULE##_IS_##OBJ_NAME (gpointer ptr) \
  { \
    return G_TYPE_CHECK_INSTANCE_TYPE (ptr, module_obj_name##_get_type ()); \
  } \
  static inline ModuleObjName##Class *MODULE##_##OBJ_NAME##_GET_CLASS (gpointer ptr) \
  { \
    return G_TYPE_INSTANCE_GET_CLASS (ptr, module_obj_name##_get_type (), ModuleObjName##Class); \
  } \
  G_GNUC_END_IGNORE_DEPRECATIONS \
  G_DEFINE_AUTOPTR_CLEANUP_FUNC (ModuleObjName, g_object_unref)


#define SHELL_TYPE_NM_MENU_ITEM (shell_nm_menu_item_get_type ())
DECLARE_PRIVATE_DERIVABLE_TYPE (ShellNmMenuItem, shell_nm_menu_item,
                                SHELL, NM_MENU_ITEM, ShellPopupBaseMenuItem)

/*
 * ShellNmMenuItemClass:
 * @get_name: the name of the item (`name`); the default is the stored one
 * @get_icon_name: the icon of the item (`icon_name`); the default is the
 *   stored one
 * @get_timestamp: when the item was used last (`timestamp`), 0 by default
 * @sync: updates the item from its state (`_sync ()`)
 * @active_connection_state_changed: the state of the active connection of
 *   the item changed (`_activeConnectionStateChanged ()`)
 */
struct _ShellNmMenuItemClass
{
  ShellPopupBaseMenuItemClass parent_class;

  char   * (* get_name)      (ShellNmMenuItem *self);
  char   * (* get_icon_name) (ShellNmMenuItem *self);
  gint64   (* get_timestamp) (ShellNmMenuItem *self);
  void     (* sync)          (ShellNmMenuItem *self);
  void     (* active_connection_state_changed) (ShellNmMenuItem *self);
};

struct _ShellNmMenuItem
{
  ShellPopupBaseMenuItem parent;

  gboolean radio_mode;
  char *item_name;
  char *icon_name;
  NMActiveConnection *active_connection;
};

enum
{
  MENU_ITEM_PROP_0,

  MENU_ITEM_PROP_RADIO_MODE,
  MENU_ITEM_PROP_IS_ACTIVE,
  MENU_ITEM_PROP_NAME,
  MENU_ITEM_PROP_ICON_NAME,

  N_MENU_ITEM_PROPS
};

static GParamSpec *menu_item_props[N_MENU_ITEM_PROPS] = { NULL, };

G_DEFINE_ABSTRACT_TYPE (ShellNmMenuItem, shell_nm_menu_item,
                        SHELL_TYPE_POPUP_BASE_MENU_ITEM)

static char *
shell_nm_menu_item_dup_name (ShellNmMenuItem *self)
{
  ShellNmMenuItemClass *klass = SHELL_NM_MENU_ITEM_GET_CLASS (self);

  if (klass->get_name != NULL)
    return klass->get_name (self);

  return g_strdup (self->item_name != NULL ? self->item_name : "");
}

static char *
shell_nm_menu_item_dup_icon_name (ShellNmMenuItem *self)
{
  ShellNmMenuItemClass *klass = SHELL_NM_MENU_ITEM_GET_CLASS (self);

  if (klass->get_icon_name != NULL)
    return klass->get_icon_name (self);

  return g_strdup (self->icon_name != NULL ? self->icon_name : "");
}

static gint64
shell_nm_menu_item_get_timestamp (ShellNmMenuItem *self)
{
  ShellNmMenuItemClass *klass = SHELL_NM_MENU_ITEM_GET_CLASS (self);

  if (klass->get_timestamp != NULL)
    return klass->get_timestamp (self);

  return 0;
}

static void
shell_nm_menu_item_sync (ShellNmMenuItem *self)
{
  ShellNmMenuItemClass *klass = SHELL_NM_MENU_ITEM_GET_CLASS (self);

  if (klass->sync != NULL)
    klass->sync (self);
}

static void
shell_nm_menu_item_notify_name (ShellNmMenuItem *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), menu_item_props[MENU_ITEM_PROP_NAME]);
}

static void
shell_nm_menu_item_notify_icon_name (ShellNmMenuItem *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), menu_item_props[MENU_ITEM_PROP_ICON_NAME]);
}

/* `state` */
static NMActiveConnectionState
shell_nm_menu_item_get_state (ShellNmMenuItem *self)
{
  if (self->active_connection == NULL)
    return NM_ACTIVE_CONNECTION_STATE_DEACTIVATED;

  return nm_active_connection_get_state (self->active_connection);
}

/* `is_active` */
static gboolean
shell_nm_menu_item_get_is_active (ShellNmMenuItem *self)
{
  return shell_nm_menu_item_get_state (self) <= NM_ACTIVE_CONNECTION_STATE_ACTIVATED;
}

static void
shell_nm_menu_item_set_radio_mode (ShellNmMenuItem *self,
                                   gboolean         radio_mode)
{
  if (self->radio_mode == radio_mode)
    return;

  self->radio_mode = radio_mode;
  g_object_notify_by_pspec (G_OBJECT (self), menu_item_props[MENU_ITEM_PROP_RADIO_MODE]);
}

static void
shell_nm_menu_item_real_active_connection_state_changed (ShellNmMenuItem *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), menu_item_props[MENU_ITEM_PROP_IS_ACTIVE]);
  shell_nm_menu_item_notify_icon_name (self);

  shell_nm_menu_item_sync (self);
}

static void
active_connection_notify_state_cb (NMActiveConnection *active_connection,
                                   GParamSpec         *pspec,
                                   ShellNmMenuItem    *self)
{
  SHELL_NM_MENU_ITEM_GET_CLASS (self)->active_connection_state_changed (self);
}

/* _setActiveConnection () */
static void
shell_nm_menu_item_set_active_connection (ShellNmMenuItem    *self,
                                          NMActiveConnection *active_connection)
{
  if (self->active_connection != NULL)
    g_signal_handlers_disconnect_by_data (self->active_connection, self);

  g_set_object (&self->active_connection, active_connection);

  if (self->active_connection != NULL)
    g_signal_connect (self->active_connection, "notify::state",
                      G_CALLBACK (active_connection_notify_state_cb), self);

  SHELL_NM_MENU_ITEM_GET_CLASS (self)->active_connection_state_changed (self);
}

static void
shell_nm_menu_item_activate (ShellPopupBaseMenuItem *item,
                             const ClutterEvent     *event)
{
  ShellPopupBaseMenuItemClass *parent_class =
    SHELL_POPUP_BASE_MENU_ITEM_CLASS (shell_nm_menu_item_parent_class);

  /* vfunc_activate (_event) { super.vfunc_activate (Clutter.get_current_event ()) } */
  parent_class->activate (item, clutter_get_current_event ());
}

static void
shell_nm_menu_item_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellNmMenuItem *self = SHELL_NM_MENU_ITEM (object);

  switch (prop_id)
    {
    case MENU_ITEM_PROP_RADIO_MODE:
      g_value_set_boolean (value, self->radio_mode);
      break;

    case MENU_ITEM_PROP_IS_ACTIVE:
      g_value_set_boolean (value, shell_nm_menu_item_get_is_active (self));
      break;

    case MENU_ITEM_PROP_NAME:
      g_value_take_string (value, shell_nm_menu_item_dup_name (self));
      break;

    case MENU_ITEM_PROP_ICON_NAME:
      g_value_take_string (value, shell_nm_menu_item_dup_icon_name (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_nm_menu_item_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellNmMenuItem *self = SHELL_NM_MENU_ITEM (object);

  switch (prop_id)
    {
    case MENU_ITEM_PROP_RADIO_MODE:
      shell_nm_menu_item_set_radio_mode (self, g_value_get_boolean (value));
      break;

    case MENU_ITEM_PROP_NAME:
      if (g_set_str (&self->item_name, g_value_get_string (value)))
        shell_nm_menu_item_notify_name (self);
      break;

    case MENU_ITEM_PROP_ICON_NAME:
      if (g_set_str (&self->icon_name, g_value_get_string (value)))
        shell_nm_menu_item_notify_icon_name (self);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_nm_menu_item_dispose (GObject *object)
{
  ShellNmMenuItem *self = SHELL_NM_MENU_ITEM (object);

  if (self->active_connection != NULL)
    g_signal_handlers_disconnect_by_data (self->active_connection, self);
  g_clear_object (&self->active_connection);

  G_OBJECT_CLASS (shell_nm_menu_item_parent_class)->dispose (object);
}

static void
shell_nm_menu_item_finalize (GObject *object)
{
  ShellNmMenuItem *self = SHELL_NM_MENU_ITEM (object);

  g_clear_pointer (&self->item_name, g_free);
  g_clear_pointer (&self->icon_name, g_free);

  G_OBJECT_CLASS (shell_nm_menu_item_parent_class)->finalize (object);
}

static void
shell_nm_menu_item_class_init (ShellNmMenuItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupBaseMenuItemClass *item_class = SHELL_POPUP_BASE_MENU_ITEM_CLASS (klass);

  object_class->dispose = shell_nm_menu_item_dispose;
  object_class->finalize = shell_nm_menu_item_finalize;
  object_class->get_property = shell_nm_menu_item_get_property;
  object_class->set_property = shell_nm_menu_item_set_property;

  item_class->activate = shell_nm_menu_item_activate;

  klass->active_connection_state_changed =
    shell_nm_menu_item_real_active_connection_state_changed;

  menu_item_props[MENU_ITEM_PROP_RADIO_MODE] =
    g_param_spec_boolean ("radio-mode", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  menu_item_props[MENU_ITEM_PROP_IS_ACTIVE] =
    g_param_spec_boolean ("is-active", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  menu_item_props[MENU_ITEM_PROP_NAME] =
    g_param_spec_string ("item-name", NULL, NULL,
                         "",
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  menu_item_props[MENU_ITEM_PROP_ICON_NAME] =
    g_param_spec_string ("icon-name", NULL, NULL,
                         "",
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_MENU_ITEM_PROPS, menu_item_props);
}

static void
shell_nm_menu_item_init (ShellNmMenuItem *self)
{
}

/* ItemSorter.upsert () and itemsByMru () need the timestamp of the items */

static int
compare_items_by_name (gconstpointer a,
                       gconstpointer b)
{
  g_autofree char *name_a = shell_nm_menu_item_dup_name ((ShellNmMenuItem *) a);
  g_autofree char *name_b = shell_nm_menu_item_dup_name ((ShellNmMenuItem *) b);

  return g_utf8_collate (name_a != NULL ? name_a : "", name_b != NULL ? name_b : "");
}

/* _sortByMru (): the more recent first */
static int
compare_items_by_mru (gconstpointer a,
                      gconstpointer b)
{
  gint64 timestamp_a = shell_nm_menu_item_get_timestamp ((ShellNmMenuItem *) a);
  gint64 timestamp_b = shell_nm_menu_item_get_timestamp ((ShellNmMenuItem *) b);

  if (timestamp_b < timestamp_a)
    return -1;
  else if (timestamp_b > timestamp_a)
    return 1;

  return 0;
}

/* Insert or update @item; returns the sorted position of @item */
static guint
item_sorter_upsert (ItemSorter *sorter,
                    gpointer    item)
{
  g_autoptr (GObject) keep = g_object_ref (item);
  guint pos;

  if (sorter->items_mru_order != NULL)
    {
      g_ptr_array_remove (sorter->items_mru_order, item);
      pos = lower_bound (sorter->items_mru_order, item, compare_items_by_mru);
      g_ptr_array_insert (sorter->items_mru_order, pos, item);
    }

  g_ptr_array_remove (sorter->items_order, item);
  pos = lower_bound (sorter->items_order, item, sorter->sort_func);
  g_ptr_array_insert (sorter->items_order, pos, g_object_ref (item));

  return pos;
}

/* ------------------------------------------------------------------------ */
/* ShellNmSectionItem                                                       */
/* ------------------------------------------------------------------------ */

/* An item that contains a section, and can be collapsed into a submenu */

#define SHELL_TYPE_NM_SECTION_ITEM (shell_nm_section_item_get_type ())
DECLARE_PRIVATE_DERIVABLE_TYPE (ShellNmSectionItem, shell_nm_section_item,
                                SHELL, NM_SECTION_ITEM, ShellNmMenuItem)

struct _ShellNmSectionItemClass
{
  ShellNmMenuItemClass parent_class;
};

struct _ShellNmSectionItem
{
  ShellNmMenuItem parent;

  gboolean use_submenu;
  ShellPopupMenuSection *main_section;       /* weak */
  ShellPopupSubMenuMenuItem *submenu_item;   /* weak */
  ShellPopupMenuSection *section;            /* weak */
};

enum
{
  SECTION_ITEM_PROP_0,

  SECTION_ITEM_PROP_USE_SUBMENU,

  N_SECTION_ITEM_PROPS
};

static GParamSpec *section_item_props[N_SECTION_ITEM_PROPS] = { NULL, };

G_DEFINE_ABSTRACT_TYPE (ShellNmSectionItem, shell_nm_section_item,
                        SHELL_TYPE_NM_MENU_ITEM)

static ShellPopupMenuBase *
shell_nm_section_item_get_section (ShellNmSectionItem *self)
{
  return SHELL_POPUP_MENU_BASE (self->section);
}

static void
shell_nm_section_item_set_use_submenu (ShellNmSectionItem *self,
                                       gboolean            use_submenu)
{
  ShellPopupMenuBase *main_section = SHELL_POPUP_MENU_BASE (self->main_section);
  ShellPopupMenuBase *submenu =
    SHELL_POPUP_MENU_BASE (shell_popup_sub_menu_menu_item_get_menu (self->submenu_item));
  g_autoptr (ClutterActor) section_actor = NULL;
  ClutterActor *main_box = CLUTTER_ACTOR (shell_popup_menu_base_get_box (main_section));
  ClutterActor *submenu_box = CLUTTER_ACTOR (shell_popup_menu_base_get_box (submenu));

  if (self->use_submenu == use_submenu)
    return;

  self->use_submenu = use_submenu;
  set_actor_visible (self->submenu_item, use_submenu);

  /* The section keeps its actor alive, but let the actor not go away
   * between the removal and the addition */
  section_actor = g_object_ref (shell_popup_menu_base_get_actor (shell_nm_section_item_get_section (self)));

  if (use_submenu)
    {
      clutter_actor_remove_child (main_box, section_actor);
      clutter_actor_add_child (submenu_box, section_actor);
    }
  else
    {
      clutter_actor_remove_child (submenu_box, section_actor);
      clutter_actor_add_child (main_box, section_actor);
    }

  g_object_notify_by_pspec (G_OBJECT (self), section_item_props[SECTION_ITEM_PROP_USE_SUBMENU]);
}

static void
section_item_parent_menu_closed_cb (ShellPopupMenuBase *parent,
                                    ShellNmSectionItem *self)
{
  if (self->main_section != NULL)
    g_signal_emit_by_name (self->main_section, "menu-closed");
}

static void
shell_nm_section_item_set_parent_menu (ShellPopupBaseMenuItem *item,
                                       ShellPopupMenuBase     *parent)
{
  ShellNmSectionItem *self = SHELL_NM_SECTION_ITEM (item);

  SHELL_POPUP_BASE_MENU_ITEM_CLASS (shell_nm_section_item_parent_class)->set_parent_menu (item, parent);

  if (self->main_section != NULL)
    shell_popup_menu_base_set_parent_menu (SHELL_POPUP_MENU_BASE (self->main_section), parent);

  if (parent != NULL)
    g_signal_connect_object (parent, "menu-closed",
                             G_CALLBACK (section_item_parent_menu_closed_cb),
                             self, 0);
}

static void
shell_nm_section_item_constructed (GObject *object)
{
  ShellNmSectionItem *self = SHELL_NM_SECTION_ITEM (object);
  ShellPopupMenuBase *main_section;
  g_autoptr (ShellPopupMenuSection) main_section_ref = NULL;
  g_autoptr (ShellPopupMenuSection) section_ref = NULL;
  ShellPopupSubMenuMenuItem *submenu_item;

  G_OBJECT_CLASS (shell_nm_section_item_parent_class)->constructed (object);

  /* Turn into an empty container with no padding */
  st_widget_set_style_class_name (ST_WIDGET (self), "");

  /* Add intermediate section; we need this for submenu support. The sections
   * are kept alive by their actors. */
  main_section_ref = shell_popup_menu_section_new ();
  g_set_weak_pointer (&self->main_section, main_section_ref);
  main_section = SHELL_POPUP_MENU_BASE (main_section_ref);
  clutter_actor_add_child (CLUTTER_ACTOR (self),
                           shell_popup_menu_base_get_actor (main_section));

  submenu_item = g_object_new (SHELL_TYPE_POPUP_SUB_MENU_MENU_ITEM,
                               "text", "",
                               "want-icon", TRUE,
                               NULL);
  g_set_weak_pointer (&self->submenu_item, submenu_item);
  shell_popup_menu_base_add_menu_item (main_section, G_OBJECT (submenu_item));
  clutter_actor_hide (CLUTTER_ACTOR (submenu_item));

  section_ref = shell_popup_menu_section_new ();
  g_set_weak_pointer (&self->section, section_ref);
  shell_popup_menu_base_add_menu_item (main_section, G_OBJECT (section_ref));

  /* Represents the item as a whole when shown */
  g_object_bind_property (self, "item-name",
                          shell_popup_sub_menu_menu_item_get_label (submenu_item), "text",
                          G_BINDING_DEFAULT);
  g_object_bind_property (self, "icon-name",
                          shell_popup_sub_menu_menu_item_get_icon (submenu_item), "icon-name",
                          G_BINDING_DEFAULT);
}

static void
shell_nm_section_item_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  ShellNmSectionItem *self = SHELL_NM_SECTION_ITEM (object);

  switch (prop_id)
    {
    case SECTION_ITEM_PROP_USE_SUBMENU:
      g_value_set_boolean (value, self->use_submenu);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_nm_section_item_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  ShellNmSectionItem *self = SHELL_NM_SECTION_ITEM (object);

  switch (prop_id)
    {
    case SECTION_ITEM_PROP_USE_SUBMENU:
      shell_nm_section_item_set_use_submenu (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_nm_section_item_dispose (GObject *object)
{
  ShellNmSectionItem *self = SHELL_NM_SECTION_ITEM (object);

  g_clear_weak_pointer (&self->main_section);
  g_clear_weak_pointer (&self->submenu_item);
  g_clear_weak_pointer (&self->section);

  G_OBJECT_CLASS (shell_nm_section_item_parent_class)->dispose (object);
}

static void
shell_nm_section_item_class_init (ShellNmSectionItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupBaseMenuItemClass *item_class = SHELL_POPUP_BASE_MENU_ITEM_CLASS (klass);

  object_class->constructed = shell_nm_section_item_constructed;
  object_class->dispose = shell_nm_section_item_dispose;
  object_class->get_property = shell_nm_section_item_get_property;
  object_class->set_property = shell_nm_section_item_set_property;

  item_class->set_parent_menu = shell_nm_section_item_set_parent_menu;

  section_item_props[SECTION_ITEM_PROP_USE_SUBMENU] =
    g_param_spec_boolean ("use-submenu", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_SECTION_ITEM_PROPS, section_item_props);
}

static void
shell_nm_section_item_init (ShellNmSectionItem *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellNmConnectionItem                                                    */
/* ------------------------------------------------------------------------ */

/* An item that activates (and deactivates) a connection of a device
 * (`NMDeviceConnectionItem`) or a VPN connection (`NMVpnConnectionItem`) */

typedef enum
{
  CONNECTION_ITEM_KIND_DEVICE,
  CONNECTION_ITEM_KIND_VPN,
} ConnectionItemKind;

/* `section.activateConnection (connection)` */
typedef void (* ConnectionActivateFunc)   (GObject            *section,
                                           NMConnection       *connection);
/* `section.deactivateConnection (activeConnection)` */
typedef void (* ConnectionDeactivateFunc) (GObject            *section,
                                           NMActiveConnection *active_connection);

#define SHELL_TYPE_NM_CONNECTION_ITEM (shell_nm_connection_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellNmConnectionItem, shell_nm_connection_item,
                      SHELL, NM_CONNECTION_ITEM, ShellNmMenuItem)

struct _ShellNmConnectionItem
{
  ShellNmMenuItem parent;

  ConnectionItemKind kind;
  GObject *section;                         /* weak */
  ConnectionActivateFunc activate_connection;
  ConnectionDeactivateFunc deactivate_connection;
  NMConnection *connection;
  char *device_name;

  StIcon *icon;                             /* weak */
  StLabel *label;                           /* weak */
  StLabel *subtitle;                        /* weak */
  ShellSwitch *toggle_switch;               /* weak, VPN only */
};

enum
{
  CONNECTION_ITEM_PROP_0,

  CONNECTION_ITEM_PROP_DEVICE_NAME,

  N_CONNECTION_ITEM_PROPS
};

static GParamSpec *connection_item_props[N_CONNECTION_ITEM_PROPS] = { NULL, };

enum
{
  CONNECTION_ITEM_ACTIVATION_FAILED,

  N_CONNECTION_ITEM_SIGNALS
};

static guint connection_item_signals[N_CONNECTION_ITEM_SIGNALS] = { 0, };

G_DEFINE_FINAL_TYPE (ShellNmConnectionItem, shell_nm_connection_item,
                     SHELL_TYPE_NM_MENU_ITEM)

static char *
shell_nm_connection_item_get_name (ShellNmMenuItem *item)
{
  ShellNmConnectionItem *self = SHELL_NM_CONNECTION_ITEM (item);
  const char *id = NULL;

  /* NMDeviceConnectionItem: the name of the device unless the device has
   * several connections */
  if (self->kind == CONNECTION_ITEM_KIND_DEVICE && !item->radio_mode)
    return g_strdup (self->device_name != NULL ? self->device_name : "");

  if (self->connection != NULL)
    id = nm_connection_get_id (self->connection);

  return g_strdup (id != NULL ? id : "");
}

static char *
shell_nm_connection_item_get_icon_name (ShellNmMenuItem *item)
{
  ShellNmConnectionItem *self = SHELL_NM_CONNECTION_ITEM (item);

  /* The icon of a device connection is bound to the one of its device */
  if (self->kind == CONNECTION_ITEM_KIND_DEVICE)
    return g_strdup (item->icon_name != NULL ? item->icon_name : "");

  switch (shell_nm_menu_item_get_state (item))
    {
    case NM_ACTIVE_CONNECTION_STATE_ACTIVATING:
      return g_strdup ("network-vpn-acquiring-symbolic");

    case NM_ACTIVE_CONNECTION_STATE_ACTIVATED:
      return g_strdup ("network-vpn-symbolic");

    default:
      return g_strdup ("network-vpn-disabled-symbolic");
    }
}

static gint64
shell_nm_connection_item_get_timestamp (ShellNmMenuItem *item)
{
  ShellNmConnectionItem *self = SHELL_NM_CONNECTION_ITEM (item);
  NMSettingConnection *setting;

  if (self->connection == NULL)
    return 0;

  setting = nm_connection_get_setting_connection (self->connection);
  return setting != NULL ? (gint64) nm_setting_connection_get_timestamp (setting) : 0;
}

static void
shell_nm_connection_item_sync (ShellNmMenuItem *item)
{
  ShellNmConnectionItem *self = SHELL_NM_CONNECTION_ITEM (item);
  g_autofree char *name = NULL;
  gboolean is_active = shell_nm_menu_item_get_is_active (item);

  if (self->kind == CONNECTION_ITEM_KIND_VPN)
    {
      set_accessible_checked (CLUTTER_ACTOR (self), is_active);
      return;
    }

  if (self->label == NULL)
    return;

  name = shell_nm_menu_item_dup_name (item);
  st_label_set_text (self->label, name);

  if (item->radio_mode)
    {
      st_label_set_text (self->subtitle, "");
      clutter_actor_set_accessible_name (CLUTTER_ACTOR (self), name);
      clutter_actor_set_accessible_role (CLUTTER_ACTOR (self), ATK_ROLE_CHECK_MENU_ITEM);
      shell_popup_base_menu_item_set_ornament (SHELL_POPUP_BASE_MENU_ITEM (self),
                                               is_active ? SHELL_ORNAMENT_DOT
                                                         : SHELL_ORNAMENT_NO_DOT);
    }
  else
    {
      g_autofree char *accessible_name = NULL;

      if (is_active)
        {
          /* Translators: %s is a device name like "MyPhone" */
          accessible_name = g_strdup_printf (_("Disconnect %s"), name);
        }
      else
        {
          /* Translators: %s is a device name like "MyPhone" */
          accessible_name = g_strdup_printf (_("Connect to %s"), name);
        }

      clutter_actor_set_accessible_name (CLUTTER_ACTOR (self), accessible_name);
      st_label_set_text (self->subtitle, is_active ? _("Disconnect") : _("Connect"));
      clutter_actor_set_accessible_role (CLUTTER_ACTOR (self), ATK_ROLE_MENU_ITEM);
      shell_popup_base_menu_item_set_ornament (SHELL_POPUP_BASE_MENU_ITEM (self),
                                               SHELL_ORNAMENT_HIDDEN);
    }
}

static void
shell_nm_connection_item_active_connection_state_changed (ShellNmMenuItem *item)
{
  ShellNmConnectionItem *self = SHELL_NM_CONNECTION_ITEM (item);

  if (self->kind == CONNECTION_ITEM_KIND_VPN && item->active_connection != NULL)
    {
      NMActiveConnectionState state = nm_active_connection_get_state (item->active_connection);
      NMActiveConnectionStateReason reason =
        nm_active_connection_get_state_reason (item->active_connection);

      if (state == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED &&
          reason != NM_ACTIVE_CONNECTION_STATE_REASON_NO_SECRETS &&
          reason != NM_ACTIVE_CONNECTION_STATE_REASON_USER_DISCONNECTED)
        g_signal_emit (self, connection_item_signals[CONNECTION_ITEM_ACTIVATION_FAILED], 0);
    }

  /* super._activeConnectionStateChanged () */
  g_object_notify_by_pspec (G_OBJECT (self), menu_item_props[MENU_ITEM_PROP_IS_ACTIVE]);
  shell_nm_menu_item_notify_icon_name (item);
  shell_nm_menu_item_sync (item);
}

static void
shell_nm_connection_item_activate (ShellPopupBaseMenuItem *base_item,
                                   const ClutterEvent     *event)
{
  ShellNmConnectionItem *self = SHELL_NM_CONNECTION_ITEM (base_item);
  ShellNmMenuItem *item = SHELL_NM_MENU_ITEM (self);

  SHELL_POPUP_BASE_MENU_ITEM_CLASS (shell_nm_connection_item_parent_class)->activate (base_item, event);

  if (item->radio_mode && item->active_connection != NULL)
    return; /* only activate in radio mode */

  if (self->section != NULL)
    {
      if (item->active_connection == NULL)
        self->activate_connection (self->section, self->connection);
      else
        self->deactivate_connection (self->section, item->active_connection);
    }

  shell_nm_menu_item_sync (item);
}

/* `updateForConnection ()`:
 * the connection should always be the same object (and object path) as the
 * one of the item, but this can be false if NetworkManager was restarted and
 * picked up connections in a different order. Just to be safe, set it here
 * again. */
static void
shell_nm_connection_item_update_for_connection (ShellNmConnectionItem *self,
                                                NMConnection          *connection)
{
  g_set_object (&self->connection, connection);

  shell_nm_menu_item_notify_name (SHELL_NM_MENU_ITEM (self));
  shell_nm_menu_item_sync (SHELL_NM_MENU_ITEM (self));
}

static void
shell_nm_connection_item_set_device_name (ShellNmConnectionItem *self,
                                          const char            *device_name)
{
  if (!g_set_str (&self->device_name, device_name))
    return;

  g_object_notify_by_pspec (G_OBJECT (self), connection_item_props[CONNECTION_ITEM_PROP_DEVICE_NAME]);
}

static void
connection_item_notify_sync_cb (GObject    *object,
                                GParamSpec *pspec,
                                gpointer    user_data)
{
  shell_nm_menu_item_sync (SHELL_NM_MENU_ITEM (object));
}

static void
connection_item_notify_name_cb (GObject    *object,
                                GParamSpec *pspec,
                                gpointer    user_data)
{
  shell_nm_menu_item_notify_name (SHELL_NM_MENU_ITEM (object));
}

static void
connection_item_switch_state_cb (ShellSwitch           *toggle_switch,
                                 GParamSpec            *pspec,
                                 ShellNmConnectionItem *self)
{
  /* Switch handle is reactive, so events don't propagate to the item;
   * activate it manually in that case */
  if (shell_nm_menu_item_get_is_active (SHELL_NM_MENU_ITEM (self)) ==
      shell_switch_get_state (toggle_switch))
    return;

  shell_popup_base_menu_item_activate (SHELL_POPUP_BASE_MENU_ITEM (self), NULL);
}

static void
shell_nm_connection_item_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellNmConnectionItem *self = SHELL_NM_CONNECTION_ITEM (object);

  switch (prop_id)
    {
    case CONNECTION_ITEM_PROP_DEVICE_NAME:
      g_value_set_string (value, self->device_name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_nm_connection_item_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellNmConnectionItem *self = SHELL_NM_CONNECTION_ITEM (object);

  switch (prop_id)
    {
    case CONNECTION_ITEM_PROP_DEVICE_NAME:
      shell_nm_connection_item_set_device_name (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_nm_connection_item_dispose (GObject *object)
{
  ShellNmConnectionItem *self = SHELL_NM_CONNECTION_ITEM (object);

  g_clear_weak_pointer (&self->section);
  g_clear_weak_pointer (&self->icon);
  g_clear_weak_pointer (&self->label);
  g_clear_weak_pointer (&self->subtitle);
  g_clear_weak_pointer (&self->toggle_switch);
  g_clear_object (&self->connection);

  G_OBJECT_CLASS (shell_nm_connection_item_parent_class)->dispose (object);
}

static void
shell_nm_connection_item_finalize (GObject *object)
{
  ShellNmConnectionItem *self = SHELL_NM_CONNECTION_ITEM (object);

  g_clear_pointer (&self->device_name, g_free);

  G_OBJECT_CLASS (shell_nm_connection_item_parent_class)->finalize (object);
}

static void
shell_nm_connection_item_class_init (ShellNmConnectionItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupBaseMenuItemClass *item_class = SHELL_POPUP_BASE_MENU_ITEM_CLASS (klass);
  ShellNmMenuItemClass *nm_item_class = SHELL_NM_MENU_ITEM_CLASS (klass);

  object_class->dispose = shell_nm_connection_item_dispose;
  object_class->finalize = shell_nm_connection_item_finalize;
  object_class->get_property = shell_nm_connection_item_get_property;
  object_class->set_property = shell_nm_connection_item_set_property;

  item_class->activate = shell_nm_connection_item_activate;

  nm_item_class->get_name = shell_nm_connection_item_get_name;
  nm_item_class->get_icon_name = shell_nm_connection_item_get_icon_name;
  nm_item_class->get_timestamp = shell_nm_connection_item_get_timestamp;
  nm_item_class->sync = shell_nm_connection_item_sync;
  nm_item_class->active_connection_state_changed =
    shell_nm_connection_item_active_connection_state_changed;

  connection_item_props[CONNECTION_ITEM_PROP_DEVICE_NAME] =
    g_param_spec_string ("device-name", NULL, NULL,
                         "",
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_CONNECTION_ITEM_PROPS,
                                     connection_item_props);

  connection_item_signals[CONNECTION_ITEM_ACTIVATION_FAILED] =
    g_signal_new ("activation-failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_nm_connection_item_init (ShellNmConnectionItem *self)
{
}

/*
 * shell_nm_connection_item_new:
 * @section: the object which activates and deactivates the connections
 *   (the device item or the VPN toggle)
 *
 * Creates the item of @connection (`new NMDeviceConnectionItem (section,
 * connection)` and `new NMVpnConnectionItem (section, connection)`).
 */
static ShellNmConnectionItem *
shell_nm_connection_item_new (ConnectionItemKind        kind,
                              GObject                  *section,
                              ConnectionActivateFunc    activate_connection,
                              ConnectionDeactivateFunc  deactivate_connection,
                              NMConnection             *connection)
{
  ShellNmConnectionItem *self = g_object_new (SHELL_TYPE_NM_CONNECTION_ITEM, NULL);
  ShellNmMenuItem *item = SHELL_NM_MENU_ITEM (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *icon, *label, *subtitle;

  self->kind = kind;
  g_set_weak_pointer (&self->section, section);
  self->activate_connection = activate_connection;
  self->deactivate_connection = deactivate_connection;
  self->connection = g_object_ref (connection);

  icon = g_object_new (ST_TYPE_ICON,
                       "style-class", "popup-menu-icon",
                       "x-align", CLUTTER_ACTOR_ALIGN_END,
                       "visible", !item->radio_mode,
                       NULL);
  g_set_weak_pointer (&self->icon, ST_ICON (icon));
  clutter_actor_add_child (actor, icon);

  label = g_object_new (ST_TYPE_LABEL,
                        "x-expand", TRUE,
                        "y-expand", TRUE,
                        "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  g_set_weak_pointer (&self->label, ST_LABEL (label));
  clutter_actor_add_child (actor, label);

  subtitle = g_object_new (ST_TYPE_LABEL,
                           "style-class", "device-subtitle",
                           "y-expand", TRUE,
                           "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                           NULL);
  g_set_weak_pointer (&self->subtitle, ST_LABEL (subtitle));
  clutter_actor_add_child (actor, subtitle);

  g_object_bind_property (self, "icon-name", icon, "icon-name", G_BINDING_DEFAULT);
  g_object_bind_property (self, "radio-mode", icon, "visible", G_BINDING_INVERT_BOOLEAN);

  g_signal_connect (self, "notify::radio-mode",
                    G_CALLBACK (connection_item_notify_sync_cb), NULL);
  g_signal_connect (self, "notify::item-name",
                    G_CALLBACK (connection_item_notify_sync_cb), NULL);

  /* NMDeviceConnectionItem: the name depends on the radio mode and on the
   * name of the device */
  if (kind == CONNECTION_ITEM_KIND_DEVICE)
    {
      g_signal_connect (self, "notify::radio-mode",
                        G_CALLBACK (connection_item_notify_name_cb), NULL);
      g_signal_connect (self, "notify::device-name",
                        G_CALLBACK (connection_item_notify_name_cb), NULL);
    }

  shell_nm_menu_item_sync (item);

  if (kind == CONNECTION_ITEM_KIND_VPN)
    {
      ShellSwitch *toggle_switch;

      clutter_actor_set_x_expand (label, TRUE);
      clutter_actor_set_accessible_role (actor, ATK_ROLE_CHECK_MENU_ITEM);
      clutter_actor_hide (icon);
      st_widget_set_label_actor (ST_WIDGET (self), label);

      toggle_switch = shell_switch_new (shell_nm_menu_item_get_is_active (item));
      g_set_weak_pointer (&self->toggle_switch, toggle_switch);
      clutter_actor_add_child (actor, CLUTTER_ACTOR (toggle_switch));

      g_object_bind_property (self, "is-active", toggle_switch, "state", G_BINDING_SYNC_CREATE);

      g_signal_connect (toggle_switch, "notify::state",
                        G_CALLBACK (connection_item_switch_state_cb), self);

      g_object_bind_property (self, "item-name", label, "text", G_BINDING_SYNC_CREATE);
    }

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellNmDeviceItem                                                        */
/* ------------------------------------------------------------------------ */

/* The item of a wired, mobile broadband or Bluetooth device: a section with
 * an item for each connection of the device */

typedef enum
{
  DEVICE_ITEM_KIND_WIRED,     /* NMWiredDeviceItem */
  DEVICE_ITEM_KIND_MODEM,     /* NMModemDeviceItem */
  DEVICE_ITEM_KIND_BLUETOOTH, /* NMBluetoothDeviceItem */
} DeviceItemKind;

#define SHELL_TYPE_NM_DEVICE_ITEM (shell_nm_device_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellNmDeviceItem, shell_nm_device_item,
                      SHELL, NM_DEVICE_ITEM, ShellNmSectionItem)

struct _ShellNmDeviceItem
{
  ShellNmSectionItem parent;

  DeviceItemKind kind;
  NMClient *client;
  NMDevice *device;
  char *device_name;

  GHashTable *connection_items;    /* NMConnection -> ShellNmConnectionItem */
  ItemSorter *item_sorter;

  ShellPopupBaseMenuItem *auto_connect_item;  /* weak */
  ShellPopupBaseMenuItem *deactivate_item;    /* weak */

  gboolean single_device_mode;

  ShellModem *mobile_device;                  /* modems only */
};

enum
{
  DEVICE_ITEM_PROP_0,

  DEVICE_ITEM_PROP_SINGLE_DEVICE_MODE,

  N_DEVICE_ITEM_PROPS
};

static GParamSpec *device_item_props[N_DEVICE_ITEM_PROPS] = { NULL, };

G_DEFINE_FINAL_TYPE (ShellNmDeviceItem, shell_nm_device_item,
                     SHELL_TYPE_NM_SECTION_ITEM)

static void shell_nm_device_item_sync_connections (ShellNmDeviceItem *self);

static void
disconnect_session_mode (gpointer data)
{
  ShellSessionMode *session_mode = get_session_mode ();

  if (session_mode != NULL)
    g_signal_handlers_disconnect_by_data (session_mode, data);
}

/* `wwanPanelSupported`: currently, the wwan panel doesn't support the CDMA
 * modems */
static gboolean
shell_nm_device_item_get_wwan_panel_supported (ShellNmDeviceItem *self)
{
  NMDeviceModemCapabilities supported_caps =
    NM_DEVICE_MODEM_CAPABILITY_GSM_UMTS | NM_DEVICE_MODEM_CAPABILITY_LTE;

  if (!NM_IS_DEVICE_MODEM (self->device))
    return FALSE;

  return (nm_device_modem_get_current_capabilities (NM_DEVICE_MODEM (self->device)) &
          supported_caps) != 0;
}

static char *
shell_nm_device_item_get_name (ShellNmMenuItem *item)
{
  ShellNmDeviceItem *self = SHELL_NM_DEVICE_ITEM (item);
  const char *name = NULL;

  switch (self->kind)
    {
    case DEVICE_ITEM_KIND_MODEM:
      if (self->mobile_device != NULL)
        name = shell_modem_get_operator_name (self->mobile_device);
      if (name == NULL || *name == '\0')
        name = self->device_name;
      break;

    case DEVICE_ITEM_KIND_BLUETOOTH:
      if (self->device != NULL && NM_IS_DEVICE_BT (self->device))
        name = nm_device_bt_get_name (NM_DEVICE_BT (self->device));
      break;

    case DEVICE_ITEM_KIND_WIRED:
    default:
      name = self->device_name;
      break;
    }

  return g_strdup (name != NULL ? name : "");
}

static char *
shell_nm_device_item_get_icon_name (ShellNmMenuItem *item)
{
  ShellNmDeviceItem *self = SHELL_NM_DEVICE_ITEM (item);
  NMActiveConnectionState state = shell_nm_menu_item_get_state (item);

  switch (self->kind)
    {
    case DEVICE_ITEM_KIND_WIRED:
      switch (state)
        {
        case NM_ACTIVE_CONNECTION_STATE_ACTIVATING:
          return g_strdup ("network-wired-acquiring-symbolic");

        case NM_ACTIVE_CONNECTION_STATE_ACTIVATED:
          return g_strdup (device_can_reach_internet (self->client, self->device)
                           ? "network-wired-symbolic"
                           : "network-wired-no-route-symbolic");

        default:
          return g_strdup ("network-wired-disconnected-symbolic");
        }

    case DEVICE_ITEM_KIND_MODEM:
      switch (state)
        {
        case NM_ACTIVE_CONNECTION_STATE_ACTIVATING:
          return g_strdup ("network-cellular-acquiring-symbolic");

        case NM_ACTIVE_CONNECTION_STATE_ACTIVATED:
          {
            int quality = self->mobile_device != NULL
              ? shell_modem_get_signal_quality (self->mobile_device) : 0;

            return g_strdup_printf ("network-cellular-signal-%s-symbolic",
                                    signal_to_icon (quality));
          }

        default:
          return g_strdup (item->active_connection != NULL
                           ? "network-cellular-signal-none-symbolic"
                           : "network-cellular-disabled-symbolic");
        }

    case DEVICE_ITEM_KIND_BLUETOOTH:
      switch (state)
        {
        case NM_ACTIVE_CONNECTION_STATE_ACTIVATING:
          return g_strdup ("network-cellular-acquiring-symbolic");

        case NM_ACTIVE_CONNECTION_STATE_ACTIVATED:
          return g_strdup ("network-cellular-connected-symbolic");

        default:
          return g_strdup (item->active_connection != NULL
                           ? "network-cellular-signal-none-symbolic"
                           : "network-cellular-disabled-symbolic");
        }

    default:
      g_assert_not_reached ();
    }
}

static gint64
shell_nm_device_item_get_timestamp (ShellNmMenuItem *item)
{
  ShellNmDeviceItem *self = SHELL_NM_DEVICE_ITEM (item);
  ShellNmMenuItem *last_used;

  if (self->item_sorter == NULL)
    return 0;

  last_used = item_sorter_first_by_mru (self->item_sorter);
  return last_used != NULL ? shell_nm_menu_item_get_timestamp (last_used) : 0;
}

/* NMWiredDeviceItem._hasCarrier () */
static gboolean
shell_nm_device_item_has_carrier (ShellNmDeviceItem *self)
{
  if (NM_IS_DEVICE_ETHERNET (self->device))
    return nm_device_ethernet_get_carrier (NM_DEVICE_ETHERNET (self->device));

  return TRUE;
}

static void
shell_nm_device_item_sync (ShellNmMenuItem *item)
{
  ShellNmDeviceItem *self = SHELL_NM_DEVICE_ITEM (item);
  guint n_items;

  /* Not set up yet */
  if (self->connection_items == NULL || self->auto_connect_item == NULL)
    return;

  if (self->kind == DEVICE_ITEM_KIND_WIRED)
    set_actor_visible (self, shell_nm_device_item_has_carrier (self));

  n_items = g_hash_table_size (self->connection_items);
  shell_nm_menu_item_set_radio_mode (item, n_items > 1);
  shell_nm_section_item_set_use_submenu (SHELL_NM_SECTION_ITEM (self),
                                         item->radio_mode && !self->single_device_mode);
  set_actor_visible (self->auto_connect_item, n_items == 0);
  set_actor_visible (self->deactivate_item,
                     item->radio_mode && shell_nm_menu_item_get_is_active (item));
}

/* _autoConnect () */
static void
shell_nm_device_item_auto_connect (ShellNmDeviceItem *self)
{
  if (self->kind == DEVICE_ITEM_KIND_MODEM)
    {
      if (shell_nm_device_item_get_wwan_panel_supported (self))
        {
          const char * const args[] = { "show-device", nm_device_get_udi (self->device), NULL };

          launch_settings_panel ("wwan", args);
        }
      else
        {
          const char * const args[] = { "connect-3g", nm_device_get_path (self->device), NULL };

          launch_settings_panel ("network", args);
        }
    }
  else
    {
      g_autoptr (NMConnection) connection = nm_simple_connection_new ();

      nm_client_add_and_activate_connection_async (self->client, connection, self->device,
                                                   NULL, NULL, NULL, NULL);
    }
}

static void
device_item_auto_connect_cb (const ClutterEvent *event,
                             gpointer            user_data)
{
  shell_nm_device_item_auto_connect (SHELL_NM_DEVICE_ITEM (user_data));
}

static void
shell_nm_device_item_deactivate_connection (ShellNmDeviceItem *self)
{
  nm_device_disconnect_async (self->device, NULL, NULL, NULL);
}

static void
device_item_deactivate_cb (const ClutterEvent *event,
                           gpointer            user_data)
{
  shell_nm_device_item_deactivate_connection (SHELL_NM_DEVICE_ITEM (user_data));
}

static void
device_item_activate_connection (GObject      *section,
                                 NMConnection *connection)
{
  ShellNmDeviceItem *self = SHELL_NM_DEVICE_ITEM (section);

  nm_client_activate_connection_async (self->client, connection, self->device,
                                       NULL, NULL, NULL, NULL);
}

static void
device_item_deactivate_connection (GObject            *section,
                                   NMActiveConnection *active_connection)
{
  shell_nm_device_item_deactivate_connection (SHELL_NM_DEVICE_ITEM (section));
}

/* _getActivatableItem () */
static ShellPopupBaseMenuItem *
shell_nm_device_item_get_activatable_item (ShellNmDeviceItem *self)
{
  ShellNmMenuItem *last_used = item_sorter_first_by_mru (self->item_sorter);
  ShellNmMenuItem *first_item;

  if (last_used != NULL && shell_nm_menu_item_get_timestamp (last_used) > 0)
    return SHELL_POPUP_BASE_MENU_ITEM (last_used);

  first_item = item_sorter_first (self->item_sorter);
  if (first_item != NULL)
    return SHELL_POPUP_BASE_MENU_ITEM (first_item);

  return self->auto_connect_item;
}

static void
shell_nm_device_item_activate (ShellPopupBaseMenuItem *base_item,
                               const ClutterEvent     *event)
{
  ShellNmDeviceItem *self = SHELL_NM_DEVICE_ITEM (base_item);
  ShellNmMenuItem *item = SHELL_NM_MENU_ITEM (self);

  SHELL_POPUP_BASE_MENU_ITEM_CLASS (shell_nm_device_item_parent_class)->activate (base_item, event);

  if (item->active_connection != NULL)
    {
      shell_nm_device_item_deactivate_connection (self);
    }
  else
    {
      ShellPopupBaseMenuItem *activatable = shell_nm_device_item_get_activatable_item (self);

      if (activatable != NULL)
        shell_popup_base_menu_item_activate (activatable, NULL);
    }
}

/* _activeConnectionChanged () */
static void
shell_nm_device_item_active_connection_changed (ShellNmDeviceItem *self)
{
  ShellNmMenuItem *item = SHELL_NM_MENU_ITEM (self);
  NMActiveConnection *active_connection;
  ShellNmConnectionItem *old_item = NULL;
  ShellNmConnectionItem *new_item = NULL;

  if (item->active_connection != NULL)
    old_item = g_hash_table_lookup (self->connection_items,
                                    nm_active_connection_get_connection (item->active_connection));
  if (old_item != NULL)
    shell_nm_menu_item_set_active_connection (SHELL_NM_MENU_ITEM (old_item), NULL);

  shell_nm_menu_item_set_active_connection (item, nm_device_get_active_connection (self->device));

  active_connection = item->active_connection;
  if (active_connection != NULL)
    new_item = g_hash_table_lookup (self->connection_items,
                                    nm_active_connection_get_connection (active_connection));
  if (new_item != NULL)
    shell_nm_menu_item_set_active_connection (SHELL_NM_MENU_ITEM (new_item), active_connection);
}

/* _resortItem () */
static void
shell_nm_device_item_resort_item (ShellNmDeviceItem     *self,
                                  ShellNmConnectionItem *item)
{
  guint pos = item_sorter_upsert (self->item_sorter, item);

  shell_popup_menu_base_move_menu_item (shell_nm_section_item_get_section (SHELL_NM_SECTION_ITEM (self)),
                                        G_OBJECT (item), pos);
}

static void
device_item_connection_item_name_cb (ShellNmConnectionItem *item,
                                     GParamSpec            *pspec,
                                     ShellNmDeviceItem     *self)
{
  shell_nm_device_item_resort_item (self, item);
}

/* _onConnectionChanged () */
static void
device_item_connection_changed_cb (NMConnection      *connection,
                                   ShellNmDeviceItem *self)
{
  ShellNmConnectionItem *item = g_hash_table_lookup (self->connection_items, connection);

  if (item != NULL)
    shell_nm_connection_item_update_for_connection (item, connection);
}

/* _addConnection () */
static void
shell_nm_device_item_add_connection (ShellNmDeviceItem *self,
                                     NMConnection      *connection)
{
  ShellNmConnectionItem *item;
  guint pos;

  if (g_hash_table_contains (self->connection_items, connection))
    return;

  g_signal_connect (connection, "changed",
                    G_CALLBACK (device_item_connection_changed_cb), self);

  item = shell_nm_connection_item_new (CONNECTION_ITEM_KIND_DEVICE, G_OBJECT (self),
                                       device_item_activate_connection,
                                       device_item_deactivate_connection,
                                       connection);

  g_object_bind_property (self, "radio-mode", item, "radio-mode", G_BINDING_SYNC_CREATE);
  g_object_bind_property (self, "item-name", item, "device-name", G_BINDING_SYNC_CREATE);
  g_object_bind_property (self, "icon-name", item, "icon-name", G_BINDING_SYNC_CREATE);
  g_signal_connect (item, "notify::item-name",
                    G_CALLBACK (device_item_connection_item_name_cb), self);

  pos = item_sorter_upsert (self->item_sorter, item);
  shell_popup_menu_base_insert_menu_item (shell_nm_section_item_get_section (SHELL_NM_SECTION_ITEM (self)),
                                          G_OBJECT (item), pos);
  g_hash_table_insert (self->connection_items, g_object_ref (connection), g_object_ref (item));
  shell_nm_menu_item_sync (SHELL_NM_MENU_ITEM (self));
}

/* _removeConnection () */
static void
shell_nm_device_item_remove_connection (ShellNmDeviceItem *self,
                                        NMConnection      *connection)
{
  g_autoptr (NMConnection) keep_connection = g_object_ref (connection);
  g_autoptr (ShellNmConnectionItem) item = NULL;
  ShellNmConnectionItem *found = g_hash_table_lookup (self->connection_items, connection);

  if (found == NULL)
    return;

  item = g_object_ref (found);

  g_signal_handlers_disconnect_by_data (connection, self);
  g_signal_handlers_disconnect_by_data (item, self);

  item_sorter_delete (self->item_sorter, item);
  g_hash_table_remove (self->connection_items, connection);
  clutter_actor_destroy (CLUTTER_ACTOR (item));

  shell_nm_menu_item_sync (SHELL_NM_MENU_ITEM (self));
}

/* _syncConnections () */
static void
shell_nm_device_item_sync_connections (ShellNmDeviceItem *self)
{
  const GPtrArray *all = nm_device_get_available_connections (self->device);
  g_autoptr (GPtrArray) available = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GPtrArray) removed = g_ptr_array_new_with_free_func (g_object_unref);
  GHashTableIter iter;
  gpointer key;
  guint i;

  for (i = 0; all != NULL && i < all->len; i++)
    {
      NMConnection *connection = g_ptr_array_index (all, i);

      if (nm_connection_get_id (connection) != NULL)
        g_ptr_array_add (available, g_object_ref (connection));
    }

  g_hash_table_iter_init (&iter, self->connection_items);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      if (!g_ptr_array_find (available, key, NULL))
        g_ptr_array_add (removed, g_object_ref (key));
    }

  for (i = 0; i < removed->len; i++)
    shell_nm_device_item_remove_connection (self, g_ptr_array_index (removed, i));

  for (i = 0; i < available->len; i++)
    shell_nm_device_item_add_connection (self, g_ptr_array_index (available, i));
}

static void
device_item_notify_available_connections_cb (NMDevice          *device,
                                             GParamSpec        *pspec,
                                             ShellNmDeviceItem *self)
{
  shell_nm_device_item_sync_connections (self);
}

static void
device_item_notify_active_connection_cb (NMDevice          *device,
                                         GParamSpec        *pspec,
                                         ShellNmDeviceItem *self)
{
  shell_nm_device_item_active_connection_changed (self);
}

static void
device_item_notify_icon_name_cb (GObject           *object,
                                 GParamSpec        *pspec,
                                 ShellNmDeviceItem *self)
{
  shell_nm_menu_item_notify_icon_name (SHELL_NM_MENU_ITEM (self));
}

static void
device_item_notify_name_cb (GObject           *object,
                            GParamSpec        *pspec,
                            ShellNmDeviceItem *self)
{
  shell_nm_menu_item_notify_name (SHELL_NM_MENU_ITEM (self));
}

static void
device_item_mobile_operator_name_cb (GObject           *object,
                                     GParamSpec        *pspec,
                                     ShellNmDeviceItem *self)
{
  shell_nm_menu_item_sync (SHELL_NM_MENU_ITEM (self));
}

static void
device_item_notify_single_device_mode_cb (GObject           *object,
                                          GParamSpec        *pspec,
                                          ShellNmDeviceItem *self)
{
  shell_nm_menu_item_sync (SHELL_NM_MENU_ITEM (self));
}

/* NMModemDeviceItem._sessionUpdated () */
static void
device_item_session_updated_cb (ShellNmDeviceItem *self)
{
  if (self->auto_connect_item != NULL)
    shell_popup_base_menu_item_set_sensitive (self->auto_connect_item, session_has_windows ());
}

static void
shell_nm_device_item_set_single_device_mode (ShellNmDeviceItem *self,
                                             gboolean           single_device_mode)
{
  if (self->single_device_mode == single_device_mode)
    return;

  self->single_device_mode = single_device_mode;
  g_object_notify_by_pspec (G_OBJECT (self), device_item_props[DEVICE_ITEM_PROP_SINGLE_DEVICE_MODE]);
}

/* setDeviceName () */
static void
shell_nm_device_item_set_device_name (ShellNmDeviceItem *self,
                                      const char        *name)
{
  g_set_str (&self->device_name, name);
  shell_nm_menu_item_notify_name (SHELL_NM_MENU_ITEM (self));
}

static void
shell_nm_device_item_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  ShellNmDeviceItem *self = SHELL_NM_DEVICE_ITEM (object);

  switch (prop_id)
    {
    case DEVICE_ITEM_PROP_SINGLE_DEVICE_MODE:
      g_value_set_boolean (value, self->single_device_mode);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_nm_device_item_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  ShellNmDeviceItem *self = SHELL_NM_DEVICE_ITEM (object);

  switch (prop_id)
    {
    case DEVICE_ITEM_PROP_SINGLE_DEVICE_MODE:
      shell_nm_device_item_set_single_device_mode (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_nm_device_item_dispose (GObject *object)
{
  ShellNmDeviceItem *self = SHELL_NM_DEVICE_ITEM (object);

  if (self->client != NULL)
    g_signal_handlers_disconnect_by_data (self->client, self);
  if (self->device != NULL)
    g_signal_handlers_disconnect_by_data (self->device, self);
  if (self->mobile_device != NULL)
    g_signal_handlers_disconnect_by_data (self->mobile_device, self);
  disconnect_session_mode (self);

  if (self->connection_items != NULL)
    {
      GHashTableIter iter;
      gpointer key, value;

      g_hash_table_iter_init (&iter, self->connection_items);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          g_signal_handlers_disconnect_by_data (key, self);
          g_signal_handlers_disconnect_by_data (value, self);
        }
      g_hash_table_remove_all (self->connection_items);
    }
  if (self->item_sorter != NULL)
    item_sorter_clear (self->item_sorter);

  g_clear_weak_pointer (&self->auto_connect_item);
  g_clear_weak_pointer (&self->deactivate_item);
  g_clear_object (&self->mobile_device);
  g_clear_object (&self->device);
  g_clear_object (&self->client);

  G_OBJECT_CLASS (shell_nm_device_item_parent_class)->dispose (object);
}

static void
shell_nm_device_item_finalize (GObject *object)
{
  ShellNmDeviceItem *self = SHELL_NM_DEVICE_ITEM (object);

  g_clear_pointer (&self->connection_items, g_hash_table_unref);
  g_clear_pointer (&self->item_sorter, item_sorter_free);
  g_clear_pointer (&self->device_name, g_free);

  G_OBJECT_CLASS (shell_nm_device_item_parent_class)->finalize (object);
}

static void
shell_nm_device_item_class_init (ShellNmDeviceItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupBaseMenuItemClass *item_class = SHELL_POPUP_BASE_MENU_ITEM_CLASS (klass);
  ShellNmMenuItemClass *nm_item_class = SHELL_NM_MENU_ITEM_CLASS (klass);

  object_class->dispose = shell_nm_device_item_dispose;
  object_class->finalize = shell_nm_device_item_finalize;
  object_class->get_property = shell_nm_device_item_get_property;
  object_class->set_property = shell_nm_device_item_set_property;

  item_class->activate = shell_nm_device_item_activate;

  nm_item_class->get_name = shell_nm_device_item_get_name;
  nm_item_class->get_icon_name = shell_nm_device_item_get_icon_name;
  nm_item_class->get_timestamp = shell_nm_device_item_get_timestamp;
  nm_item_class->sync = shell_nm_device_item_sync;

  device_item_props[DEVICE_ITEM_PROP_SINGLE_DEVICE_MODE] =
    g_param_spec_boolean ("single-device-mode", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_DEVICE_ITEM_PROPS, device_item_props);
}

static void
shell_nm_device_item_init (ShellNmDeviceItem *self)
{
}

static ShellNmDeviceItem *
shell_nm_device_item_new (DeviceItemKind kind,
                          NMClient      *client,
                          NMDevice      *device)
{
  ShellNmDeviceItem *self = g_object_new (SHELL_TYPE_NM_DEVICE_ITEM,
                                          "activatable", FALSE,
                                          "can-focus", FALSE,
                                          NULL);
  ShellNmSectionItem *section_item = SHELL_NM_SECTION_ITEM (self);
  ShellPopupMenuBase *section = shell_nm_section_item_get_section (section_item);
  ShellPopupBaseMenuItem *auto_connect_item;
  ShellPopupBaseMenuItem *deactivate_item;

  self->kind = kind;
  self->client = g_object_ref (client);
  self->device = g_object_ref (device);
  self->device_name = g_strdup ("");
  self->connection_items = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                  g_object_unref, g_object_unref);
  self->item_sorter = item_sorter_new (compare_items_by_name, TRUE);

  if (kind == DEVICE_ITEM_KIND_MODEM)
    {
      NMDeviceModemCapabilities capabilities =
        nm_device_modem_get_current_capabilities (NM_DEVICE_MODEM (device));
      const char *udi = nm_device_get_udi (device);

      if (g_str_has_prefix (udi, "/org/freedesktop/ModemManager1/Modem"))
        self->mobile_device = shell_modem_new_broadband (udi, capabilities);
      else if (capabilities & NM_DEVICE_MODEM_CAPABILITY_GSM_UMTS)
        self->mobile_device = shell_modem_new_gsm (udi);
      else if (capabilities & NM_DEVICE_MODEM_CAPABILITY_CDMA_EVDO)
        self->mobile_device = shell_modem_new_cdma (udi);
      else if (capabilities & NM_DEVICE_MODEM_CAPABILITY_LTE)
        self->mobile_device = shell_modem_new_gsm (udi);

      if (self->mobile_device != NULL)
        {
          g_signal_connect (self->mobile_device, "notify::operator-name",
                            G_CALLBACK (device_item_mobile_operator_name_cb), self);
          g_signal_connect (self->mobile_device, "notify::signal-quality",
                            G_CALLBACK (device_item_notify_icon_name_cb), self);
        }
    }

  /* Item shown in the 0-connections case */
  auto_connect_item =
    shell_popup_menu_base_add_action_with_icon (section, _("Connect"), NULL, "",
                                                device_item_auto_connect_cb, self, NULL);
  g_set_weak_pointer (&self->auto_connect_item, auto_connect_item);

  /* Represents the device as a whole when shown */
  g_object_bind_property (self, "item-name",
                          shell_popup_image_menu_item_get_label (SHELL_POPUP_IMAGE_MENU_ITEM (auto_connect_item)),
                          "text", G_BINDING_SYNC_CREATE);
  g_object_bind_property (self, "icon-name",
                          shell_popup_image_menu_item_get_icon (SHELL_POPUP_IMAGE_MENU_ITEM (auto_connect_item)),
                          "icon-name", G_BINDING_SYNC_CREATE);

  deactivate_item = shell_popup_menu_base_add_action (section, _("Turn Off"),
                                                      device_item_deactivate_cb, self, NULL);
  g_set_weak_pointer (&self->deactivate_item, deactivate_item);

  g_signal_connect (client, "notify::connectivity",
                    G_CALLBACK (device_item_notify_icon_name_cb), self);
  g_signal_connect (client, "notify::primary-connection",
                    G_CALLBACK (device_item_notify_icon_name_cb), self);

  g_signal_connect (device, "notify::available-connections",
                    G_CALLBACK (device_item_notify_available_connections_cb), self);
  g_signal_connect (device, "notify::active-connection",
                    G_CALLBACK (device_item_notify_active_connection_cb), self);

  g_signal_connect (self, "notify::single-device-mode",
                    G_CALLBACK (device_item_notify_single_device_mode_cb), self);

  /* The name of the device of a Bluetooth item is the name of the device */
  if (kind == DEVICE_ITEM_KIND_BLUETOOTH)
    g_signal_connect (device, "notify::name",
                      G_CALLBACK (device_item_notify_name_cb), self);

  if (kind == DEVICE_ITEM_KIND_MODEM)
    {
      ShellSessionMode *session_mode = get_session_mode ();

      if (session_mode != NULL)
        g_signal_connect_swapped (session_mode, "updated",
                                  G_CALLBACK (device_item_session_updated_cb), self);
      device_item_session_updated_cb (self);
    }

  shell_nm_device_item_sync_connections (self);
  shell_nm_device_item_active_connection_changed (self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellNmWirelessNetwork                                                   */
/* ------------------------------------------------------------------------ */

/* The access points which have the same name and security: a Wi-Fi network
 * as the user knows it */

#define SHELL_TYPE_NM_WIRELESS_NETWORK (shell_nm_wireless_network_get_type ())
G_DECLARE_FINAL_TYPE (ShellNmWirelessNetwork, shell_nm_wireless_network,
                      SHELL, NM_WIRELESS_NETWORK, GObject)

struct _ShellNmWirelessNetwork
{
  GObject parent;

  NMDeviceWifi *device;
  GPtrArray *access_points;     /* NMAccessPoint, in the order they were added */
  GPtrArray *connections;       /* NMConnection which fit an access point */
  char *name;
  GBytes *ssid;
  NMAccessPoint *best_ap;       /* one of the access points */
  NM80211Mode mode;
  NMUtilsSecurityType security_type;
};

enum
{
  WIRELESS_NETWORK_PROP_0,

  WIRELESS_NETWORK_PROP_NAME,
  WIRELESS_NETWORK_PROP_ICON_NAME,
  WIRELESS_NETWORK_PROP_SECURE,
  WIRELESS_NETWORK_PROP_IS_ACTIVE,
  WIRELESS_NETWORK_PROP_SIGNAL_STRENGTH,

  N_WIRELESS_NETWORK_PROPS
};

static GParamSpec *wireless_network_props[N_WIRELESS_NETWORK_PROPS] = { NULL, };

enum
{
  WIRELESS_NETWORK_DESTROY,

  N_WIRELESS_NETWORK_SIGNALS
};

static guint wireless_network_signals[N_WIRELESS_NETWORK_SIGNALS] = { 0, };

G_DEFINE_FINAL_TYPE (ShellNmWirelessNetwork, shell_nm_wireless_network, G_TYPE_OBJECT)

/* The security types from the best to the worst, the former
 * `Object.values (NM.UtilsSecurityType).sort ((a, b) => b - a)` (without
 * NMU_SEC_INVALID, which is never valid) */
static const NMUtilsSecurityType security_types[] =
{
  NMU_SEC_WPA3_SUITE_B_192,
  NMU_SEC_OWE,
  NMU_SEC_SAE,
  NMU_SEC_WPA2_ENTERPRISE,
  NMU_SEC_WPA2_PSK,
  NMU_SEC_WPA_ENTERPRISE,
  NMU_SEC_WPA_PSK,
  NMU_SEC_DYNAMIC_WEP,
  NMU_SEC_LEAP,
  NMU_SEC_STATIC_WEP,
  NMU_SEC_NONE,
};

static void
wireless_network_notify (ShellNmWirelessNetwork *self,
                         int                     prop)
{
  g_object_notify_by_pspec (G_OBJECT (self), wireless_network_props[prop]);
}

/* `signal_strength` */
static guint
shell_nm_wireless_network_get_signal_strength (ShellNmWirelessNetwork *self)
{
  return self->best_ap != NULL ? nm_access_point_get_strength (self->best_ap) : 0;
}

static char *
shell_nm_wireless_network_dup_icon_name (ShellNmWirelessNetwork *self)
{
  if (self->mode == NM_802_11_MODE_ADHOC)
    return g_strdup ("network-workgroup-symbolic");

  if (self->best_ap == NULL)
    return g_strdup ("");

  return g_strdup_printf ("network-wireless-signal-%s-symbolic",
                          signal_to_icon (nm_access_point_get_strength (self->best_ap)));
}

/* `secure` */
static gboolean
shell_nm_wireless_network_get_secure (ShellNmWirelessNetwork *self)
{
  return self->security_type != NMU_SEC_NONE &&
         self->security_type != NMU_SEC_OWE;
}

/* `is_active` */
static gboolean
shell_nm_wireless_network_get_is_active (ShellNmWirelessNetwork *self)
{
  NMAccessPoint *active_ap = nm_device_wifi_get_active_access_point (self->device);
  NMActiveConnection *active_connection;

  if (active_ap != NULL && g_ptr_array_find (self->access_points, active_ap, NULL))
    return TRUE;

  active_connection = nm_device_get_active_connection (NM_DEVICE (self->device));
  if (active_connection != NULL)
    return g_ptr_array_find (self->connections,
                             nm_active_connection_get_connection (active_connection),
                             NULL);

  return FALSE;
}

static gboolean
shell_nm_wireless_network_has_access_points (ShellNmWirelessNetwork *self)
{
  return self->access_points->len > 0;
}

static gboolean
shell_nm_wireless_network_has_connections (ShellNmWirelessNetwork *self)
{
  return self->connections->len > 0;
}

static gboolean
shell_nm_wireless_network_can_autoconnect (ShellNmWirelessNetwork *self)
{
  return self->security_type != NMU_SEC_WPA_ENTERPRISE &&
         self->security_type != NMU_SEC_WPA2_ENTERPRISE;
}

/* _getApSecurityType () */
static NMUtilsSecurityType
shell_nm_wireless_network_get_ap_security_type (ShellNmWirelessNetwork *self,
                                                NMAccessPoint          *ap)
{
  NMDeviceWifiCapabilities caps = nm_device_wifi_get_capabilities (self->device);
  NM80211ApFlags flags = nm_access_point_get_flags (ap);
  NM80211ApSecurityFlags wpa_flags = nm_access_point_get_wpa_flags (ap);
  NM80211ApSecurityFlags rsn_flags = nm_access_point_get_rsn_flags (ap);
  gboolean ad_hoc = nm_access_point_get_mode (ap) == NM_802_11_MODE_ADHOC;
  guint i;

  for (i = 0; i < G_N_ELEMENTS (security_types); i++)
    {
      if (nm_utils_security_valid (security_types[i], caps, TRUE, ad_hoc,
                                   flags, wpa_flags, rsn_flags))
        return security_types[i];
    }

  return NMU_SEC_INVALID;
}

/* checkAccessPoint () */
static gboolean
shell_nm_wireless_network_check_access_point (ShellNmWirelessNetwork *self,
                                              NMAccessPoint          *ap)
{
  NMUtilsSecurityType sec_type;

  if (nm_access_point_get_ssid (ap) == NULL)
    return FALSE;

  sec_type = shell_nm_wireless_network_get_ap_security_type (self, ap);
  if (sec_type == NMU_SEC_INVALID)
    return FALSE;

  if (self->access_points->len == 0)
    return TRUE;

  return g_bytes_equal (self->ssid, nm_access_point_get_ssid (ap)) &&
         self->mode == nm_access_point_get_mode (ap) &&
         self->security_type == sec_type;
}

/* _updateBestAp (): the access point with the strongest signal, the first
 * one added if several have the same */
static void
shell_nm_wireless_network_update_best_ap (ShellNmWirelessNetwork *self)
{
  NMAccessPoint *best_ap = NULL;
  guint i;

  for (i = 0; i < self->access_points->len; i++)
    {
      NMAccessPoint *ap = g_ptr_array_index (self->access_points, i);

      if (best_ap == NULL ||
          nm_access_point_get_strength (ap) > nm_access_point_get_strength (best_ap))
        best_ap = ap;
    }

  if (self->best_ap == best_ap)
    return;

  self->best_ap = best_ap;
  wireless_network_notify (self, WIRELESS_NETWORK_PROP_ICON_NAME);
}

static void
wireless_network_ap_strength_cb (NMAccessPoint         *ap,
                                 GParamSpec            *pspec,
                                 ShellNmWirelessNetwork *self)
{
  wireless_network_notify (self, WIRELESS_NETWORK_PROP_ICON_NAME);
  wireless_network_notify (self, WIRELESS_NETWORK_PROP_SIGNAL_STRENGTH);
  shell_nm_wireless_network_update_best_ap (self);
}

/* addAccessPoint (): returns whether the access point was added */
static gboolean
shell_nm_wireless_network_add_access_point (ShellNmWirelessNetwork *self,
                                            NMAccessPoint          *ap)
{
  gboolean was_active;

  if (!shell_nm_wireless_network_check_access_point (self, ap))
    return FALSE;

  if (self->access_points->len == 0)
    {
      gsize len = 0;
      const guint8 *data;

      g_clear_pointer (&self->ssid, g_bytes_unref);
      self->ssid = g_bytes_ref (nm_access_point_get_ssid (ap));
      self->mode = nm_access_point_get_mode (ap);
      self->security_type = shell_nm_wireless_network_get_ap_security_type (self, ap);

      g_free (self->name);
      data = g_bytes_get_data (self->ssid, &len);
      self->name = nm_utils_ssid_to_utf8 (data, len);
      if (self->name == NULL || *self->name == '\0')
        {
          g_free (self->name);
          self->name = g_strdup ("<unknown>");
        }

      wireless_network_notify (self, WIRELESS_NETWORK_PROP_NAME);
      wireless_network_notify (self, WIRELESS_NETWORK_PROP_SECURE);
    }

  was_active = shell_nm_wireless_network_get_is_active (self);
  g_ptr_array_add (self->access_points, g_object_ref (ap));

  g_signal_connect (ap, "notify::strength",
                    G_CALLBACK (wireless_network_ap_strength_cb), self);
  shell_nm_wireless_network_update_best_ap (self);

  if (was_active != shell_nm_wireless_network_get_is_active (self))
    wireless_network_notify (self, WIRELESS_NETWORK_PROP_IS_ACTIVE);

  return TRUE;
}

/* removeAccessPoint (): returns whether the access point was removed */
static gboolean
shell_nm_wireless_network_remove_access_point (ShellNmWirelessNetwork *self,
                                               NMAccessPoint          *ap)
{
  g_autoptr (NMAccessPoint) keep = g_object_ref (ap);
  gboolean was_active = shell_nm_wireless_network_get_is_active (self);

  if (!g_ptr_array_remove (self->access_points, ap))
    return FALSE;

  g_signal_handlers_disconnect_by_data (ap, self);
  shell_nm_wireless_network_update_best_ap (self);

  if (was_active != shell_nm_wireless_network_get_is_active (self))
    wireless_network_notify (self, WIRELESS_NETWORK_PROP_IS_ACTIVE);

  return TRUE;
}

/* compare (): the order of the networks in the menu */
static int
shell_nm_wireless_network_compare (ShellNmWirelessNetwork *self,
                                   ShellNmWirelessNetwork *other)
{
  int cmp;

  /* place known connections first */
  cmp = (int) shell_nm_wireless_network_has_connections (other) -
        (int) shell_nm_wireless_network_has_connections (self);
  if (cmp != 0)
    return cmp;

  cmp = (int) shell_nm_wireless_network_has_access_points (other) -
        (int) shell_nm_wireless_network_has_access_points (self);
  if (cmp != 0)
    return cmp;

  /* place stronger connections first */
  cmp = (int) shell_nm_wireless_network_get_signal_strength (other) -
        (int) shell_nm_wireless_network_get_signal_strength (self);
  if (cmp != 0)
    return cmp;

  /* place secure connections first */
  cmp = (int) shell_nm_wireless_network_get_secure (other) -
        (int) shell_nm_wireless_network_get_secure (self);
  if (cmp != 0)
    return cmp;

  /* sort alphabetically */
  return g_utf8_collate (self->name, other->name);
}

/* checkConnections () */
static void
shell_nm_wireless_network_check_connections (ShellNmWirelessNetwork *self,
                                             const GPtrArray        *connections)
{
  guint i, j;

  g_ptr_array_set_size (self->connections, 0);

  for (i = 0; connections != NULL && i < connections->len; i++)
    {
      NMConnection *connection = g_ptr_array_index (connections, i);

      for (j = 0; j < self->access_points->len; j++)
        {
          if (nm_access_point_connection_valid (g_ptr_array_index (self->access_points, j),
                                                connection))
            {
              g_ptr_array_add (self->connections, g_object_ref (connection));
              break;
            }
        }
    }
}

/* activate () */
static void
shell_nm_wireless_network_activate (ShellNmWirelessNetwork *self)
{
  NMAccessPoint *ap = self->access_points->len > 0
    ? g_ptr_array_index (self->access_points, 0) : NULL;
  NMConnection *connection = self->connections->len > 0
    ? g_ptr_array_index (self->connections, 0) : NULL;
  NMClient *client = nm_object_get_client (NM_OBJECT (self->device));

  if (connection != NULL)
    {
      nm_client_activate_connection_async (client, connection, NM_DEVICE (self->device),
                                           NULL, NULL, NULL, NULL);
    }
  else if (ap == NULL)
    {
      return;
    }
  else if (!shell_nm_wireless_network_can_autoconnect (self))
    {
      /* nm_object_get_path () is shadowed by nm_device_get_path () in JS;
       * this is the D-Bus path of the device */
      const char * const args[] = {
        "connect-8021x-wifi",
        nm_object_get_path (NM_OBJECT (self->device)),
        nm_object_get_path (NM_OBJECT (ap)),
        NULL
      };

      launch_settings_panel ("wifi", args);
    }
  else
    {
      g_autoptr (NMConnection) new_connection = nm_simple_connection_new ();
      g_autoptr (GPermission) permission = NULL;
      gboolean allowed_to_share = FALSE;

      permission = G_PERMISSION (polkit_permission_new_sync (POLKIT_SETTINGS_MODIFY_SYSTEM,
                                                             NULL, NULL, NULL));
      if (permission != NULL)
        allowed_to_share = g_permission_get_allowed (permission);

      if (!allowed_to_share)
        {
          NMSettingConnection *setting = NM_SETTING_CONNECTION (nm_setting_connection_new ());

          nm_setting_connection_add_permission (setting, "user", g_get_user_name (), NULL);
          nm_connection_add_setting (new_connection, NM_SETTING (setting));
        }

      nm_client_add_and_activate_connection_async (client, new_connection,
                                                   NM_DEVICE (self->device),
                                                   nm_object_get_path (NM_OBJECT (ap)),
                                                   NULL, NULL, NULL);
    }
}

/* destroy () */
static void
shell_nm_wireless_network_destroy (ShellNmWirelessNetwork *self)
{
  g_signal_emit (self, wireless_network_signals[WIRELESS_NETWORK_DESTROY], 0);
}

static void
wireless_network_notify_is_active_cb (GObject                *object,
                                      GParamSpec             *pspec,
                                      ShellNmWirelessNetwork *self)
{
  wireless_network_notify (self, WIRELESS_NETWORK_PROP_IS_ACTIVE);
}

static void
shell_nm_wireless_network_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ShellNmWirelessNetwork *self = SHELL_NM_WIRELESS_NETWORK (object);

  switch (prop_id)
    {
    case WIRELESS_NETWORK_PROP_NAME:
      g_value_set_string (value, self->name);
      break;

    case WIRELESS_NETWORK_PROP_ICON_NAME:
      g_value_take_string (value, shell_nm_wireless_network_dup_icon_name (self));
      break;

    case WIRELESS_NETWORK_PROP_SECURE:
      g_value_set_boolean (value, shell_nm_wireless_network_get_secure (self));
      break;

    case WIRELESS_NETWORK_PROP_IS_ACTIVE:
      g_value_set_boolean (value, shell_nm_wireless_network_get_is_active (self));
      break;

    case WIRELESS_NETWORK_PROP_SIGNAL_STRENGTH:
      g_value_set_uint (value, shell_nm_wireless_network_get_signal_strength (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_nm_wireless_network_dispose (GObject *object)
{
  ShellNmWirelessNetwork *self = SHELL_NM_WIRELESS_NETWORK (object);
  guint i;

  for (i = 0; i < self->access_points->len; i++)
    g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->access_points, i), self);
  g_ptr_array_set_size (self->access_points, 0);
  self->best_ap = NULL;
  g_ptr_array_set_size (self->connections, 0);

  if (self->device != NULL)
    g_signal_handlers_disconnect_by_data (self->device, self);
  g_clear_object (&self->device);

  G_OBJECT_CLASS (shell_nm_wireless_network_parent_class)->dispose (object);
}

static void
shell_nm_wireless_network_finalize (GObject *object)
{
  ShellNmWirelessNetwork *self = SHELL_NM_WIRELESS_NETWORK (object);

  g_clear_pointer (&self->access_points, g_ptr_array_unref);
  g_clear_pointer (&self->connections, g_ptr_array_unref);
  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->ssid, g_bytes_unref);

  G_OBJECT_CLASS (shell_nm_wireless_network_parent_class)->finalize (object);
}

static void
shell_nm_wireless_network_class_init (ShellNmWirelessNetworkClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_nm_wireless_network_dispose;
  object_class->finalize = shell_nm_wireless_network_finalize;
  object_class->get_property = shell_nm_wireless_network_get_property;

  wireless_network_props[WIRELESS_NETWORK_PROP_NAME] =
    g_param_spec_string ("name", NULL, NULL,
                         "",
                         G_PARAM_READABLE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  wireless_network_props[WIRELESS_NETWORK_PROP_ICON_NAME] =
    g_param_spec_string ("icon-name", NULL, NULL,
                         "",
                         G_PARAM_READABLE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  wireless_network_props[WIRELESS_NETWORK_PROP_SECURE] =
    g_param_spec_boolean ("secure", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  wireless_network_props[WIRELESS_NETWORK_PROP_IS_ACTIVE] =
    g_param_spec_boolean ("is-active", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  wireless_network_props[WIRELESS_NETWORK_PROP_SIGNAL_STRENGTH] =
    g_param_spec_uint ("signal-strength", NULL, NULL,
                       0, G_MAXUINT, 0,
                       G_PARAM_READABLE |
                       G_PARAM_EXPLICIT_NOTIFY |
                       G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_WIRELESS_NETWORK_PROPS,
                                     wireless_network_props);

  wireless_network_signals[WIRELESS_NETWORK_DESTROY] =
    g_signal_new ("destroy",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_nm_wireless_network_init (ShellNmWirelessNetwork *self)
{
  self->access_points = g_ptr_array_new_with_free_func (g_object_unref);
  self->connections = g_ptr_array_new_with_free_func (g_object_unref);
  self->name = g_strdup ("");
  self->security_type = NMU_SEC_NONE;
}

static ShellNmWirelessNetwork *
shell_nm_wireless_network_new (NMDeviceWifi *device)
{
  ShellNmWirelessNetwork *self = g_object_new (SHELL_TYPE_NM_WIRELESS_NETWORK, NULL);

  self->device = g_object_ref (device);

  g_signal_connect (device, "notify::active-access-point",
                    G_CALLBACK (wireless_network_notify_is_active_cb), self);
  g_signal_connect (device, "notify::active-connection",
                    G_CALLBACK (wireless_network_notify_is_active_cb), self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellNmWirelessNetworkItem                                               */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_NM_WIRELESS_NETWORK_ITEM (shell_nm_wireless_network_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellNmWirelessNetworkItem, shell_nm_wireless_network_item,
                      SHELL, NM_WIRELESS_NETWORK_ITEM, ShellPopupBaseMenuItem)

struct _ShellNmWirelessNetworkItem
{
  ShellPopupBaseMenuItem parent;

  ShellNmWirelessNetwork *network;
  StLabel *label;              /* weak */
};

G_DEFINE_FINAL_TYPE (ShellNmWirelessNetworkItem, shell_nm_wireless_network_item,
                     SHELL_TYPE_POPUP_BASE_MENU_ITEM)

static void
wireless_network_item_is_active_changed_cb (GObject                    *object,
                                            GParamSpec                 *pspec,
                                            ShellNmWirelessNetworkItem *self)
{
  set_accessible_checked (CLUTTER_ACTOR (self),
                          shell_nm_wireless_network_get_is_active (self->network));
}

static void
wireless_network_item_update_accessible_name_cb (GObject                    *object,
                                                 GParamSpec                 *pspec,
                                                 ShellNmWirelessNetworkItem *self)
{
  g_autofree char *strength = NULL;
  g_autofree char *signal_strength = NULL;
  g_autofree char *accessible_name = NULL;
  const char *secure_string = shell_nm_wireless_network_get_secure (self->network)
    ? _("Secure") : _("Not secure");

  strength = g_strdup_printf ("%u", shell_nm_wireless_network_get_signal_strength (self->network));
  signal_strength = g_strdup_printf (_("Signal strength %s%%"), strength);

  /* translators: The first placeholder is the network name, the second and indication whether it is secure, and the last the signal strength indication */
  accessible_name = g_strdup_printf (_("%s, %s, %s"),
                                     self->label != NULL ? st_label_get_text (self->label) : "",
                                     secure_string, signal_strength);

  clutter_actor_set_accessible_name (CLUTTER_ACTOR (self), accessible_name);
}

static gboolean
wireless_network_item_secure_to_icon_name (GBinding     *binding,
                                           const GValue *from_value,
                                           GValue       *to_value,
                                           gpointer      user_data)
{
  g_value_set_string (to_value,
                      g_value_get_boolean (from_value)
                      ? "network-wireless-encrypted-symbolic" : "");
  return TRUE;
}

static void
shell_nm_wireless_network_item_dispose (GObject *object)
{
  ShellNmWirelessNetworkItem *self = SHELL_NM_WIRELESS_NETWORK_ITEM (object);

  if (self->network != NULL)
    g_signal_handlers_disconnect_by_data (self->network, self);
  g_clear_object (&self->network);
  g_clear_weak_pointer (&self->label);

  G_OBJECT_CLASS (shell_nm_wireless_network_item_parent_class)->dispose (object);
}

static void
shell_nm_wireless_network_item_class_init (ShellNmWirelessNetworkItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_nm_wireless_network_item_dispose;
}

static void
shell_nm_wireless_network_item_init (ShellNmWirelessNetworkItem *self)
{
}

static ShellNmWirelessNetworkItem *
shell_nm_wireless_network_item_new (ShellNmWirelessNetwork *network)
{
  ShellNmWirelessNetworkItem *self =
    g_object_new (SHELL_TYPE_NM_WIRELESS_NETWORK_ITEM,
                  "extra-style-class", "nm-network-item",
                  NULL);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *icons, *signal_icon, *secure_icon, *label, *selected_icon;

  self->network = g_object_ref (network);

  icons = CLUTTER_ACTOR (st_box_layout_new ());
  clutter_actor_add_child (actor, icons);

  signal_icon = g_object_new (ST_TYPE_ICON,
                              "style-class", "popup-menu-icon",
                              NULL);
  clutter_actor_add_child (icons, signal_icon);

  secure_icon = g_object_new (ST_TYPE_ICON,
                              "style-class", "wireless-secure-icon",
                              "y-align", CLUTTER_ACTOR_ALIGN_END,
                              NULL);
  clutter_actor_add_child (icons, secure_icon);

  label = CLUTTER_ACTOR (st_label_new (NULL));
  g_set_weak_pointer (&self->label, ST_LABEL (label));
  clutter_actor_add_child (actor, label);

  selected_icon = g_object_new (ST_TYPE_ICON,
                                "style-class", "popup-menu-icon",
                                "icon-name", "object-select-symbolic",
                                NULL);
  clutter_actor_add_child (actor, selected_icon);

  g_object_bind_property (network, "icon-name", signal_icon, "icon-name", G_BINDING_SYNC_CREATE);
  g_object_bind_property (network, "name", label, "text", G_BINDING_SYNC_CREATE);
  g_object_bind_property (network, "is-active", selected_icon, "visible", G_BINDING_SYNC_CREATE);
  g_object_bind_property_full (network, "secure",
                               secure_icon, "icon-name",
                               G_BINDING_SYNC_CREATE,
                               wireless_network_item_secure_to_icon_name,
                               NULL, NULL, NULL);

  g_signal_connect (network, "notify::is-active",
                    G_CALLBACK (wireless_network_item_is_active_changed_cb), self);
  g_signal_connect (network, "notify::secure",
                    G_CALLBACK (wireless_network_item_update_accessible_name_cb), self);
  g_signal_connect (network, "notify::signal-strength",
                    G_CALLBACK (wireless_network_item_update_accessible_name_cb), self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellNmWirelessDeviceItem                                                */
/* ------------------------------------------------------------------------ */

/* The item of a Wi-Fi device: a section with an item for each network */

#define SHELL_TYPE_NM_WIRELESS_DEVICE_ITEM (shell_nm_wireless_device_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellNmWirelessDeviceItem, shell_nm_wireless_device_item,
                      SHELL, NM_WIRELESS_DEVICE_ITEM, ShellNmSectionItem)

struct _ShellNmWirelessDeviceItem
{
  ShellNmSectionItem parent;

  NMClient *client;
  NMDeviceWifi *device;
  char *device_name;

  GPtrArray *networks;                /* ShellNmWirelessNetwork */
  ItemSorter *item_sorter;            /* ShellNmWirelessNetworkItem */

  NMAccessPoint *active_access_point;
  gboolean single_device_mode;
};

enum
{
  WIRELESS_DEVICE_ITEM_PROP_0,

  WIRELESS_DEVICE_ITEM_PROP_IS_HOTSPOT,
  WIRELESS_DEVICE_ITEM_PROP_SINGLE_DEVICE_MODE,

  N_WIRELESS_DEVICE_ITEM_PROPS
};

static GParamSpec *wireless_device_item_props[N_WIRELESS_DEVICE_ITEM_PROPS] = { NULL, };

G_DEFINE_FINAL_TYPE (ShellNmWirelessDeviceItem, shell_nm_wireless_device_item,
                     SHELL_TYPE_NM_SECTION_ITEM)

static int
compare_wireless_network_items (gconstpointer a,
                                gconstpointer b)
{
  const ShellNmWirelessNetworkItem *item_a = a;
  const ShellNmWirelessNetworkItem *item_b = b;

  return shell_nm_wireless_network_compare (item_a->network, item_b->network);
}

/* `is_hotspot` */
static gboolean
shell_nm_wireless_device_item_get_is_hotspot (ShellNmWirelessDeviceItem *self)
{
  NMActiveConnection *active_connection;
  NMConnection *connection;
  NMSettingIPConfig *ip4_config;

  active_connection = nm_device_get_active_connection (NM_DEVICE (self->device));
  if (active_connection == NULL)
    return FALSE;

  connection = NM_CONNECTION (nm_active_connection_get_connection (active_connection));
  if (connection == NULL)
    return FALSE;

  ip4_config = nm_connection_get_setting_ip4_config (connection);
  if (ip4_config == NULL)
    return FALSE;

  return g_strcmp0 (nm_setting_ip_config_get_method (ip4_config),
                    NM_SETTING_IP4_CONFIG_METHOD_SHARED) == 0;
}

static char *
shell_nm_wireless_device_item_get_name (ShellNmMenuItem *item)
{
  ShellNmWirelessDeviceItem *self = SHELL_NM_WIRELESS_DEVICE_ITEM (item);
  NMActiveConnection *active_connection;
  GBytes *ssid = NULL;

  if (self->device == NULL)
    return g_strdup ("");

  if (shell_nm_wireless_device_item_get_is_hotspot (self))
    {
      /* Translators: %s is a network identifier */
      return g_strdup_printf (_("%s Hotspot"), self->device_name);
    }

  if (self->active_access_point != NULL)
    ssid = nm_access_point_get_ssid (self->active_access_point);
  if (ssid != NULL)
    return ssid_to_label (ssid);

  /* Use connection name when connected to hidden AP */
  active_connection = nm_device_get_active_connection (NM_DEVICE (self->device));
  if (active_connection != NULL &&
      nm_active_connection_get_connection (active_connection) != NULL)
    {
      const char *id =
        nm_connection_get_id (NM_CONNECTION (nm_active_connection_get_connection (active_connection)));

      return g_strdup (id != NULL ? id : "");
    }

  return g_strdup (self->device_name);
}

static char *
shell_nm_wireless_device_item_get_icon_name (ShellNmMenuItem *item)
{
  ShellNmWirelessDeviceItem *self = SHELL_NM_WIRELESS_DEVICE_ITEM (item);

  if (self->device == NULL || !nm_client_wireless_get_enabled (self->client))
    return g_strdup ("network-wireless-disabled-symbolic");

  switch (shell_nm_menu_item_get_state (item))
    {
    case NM_ACTIVE_CONNECTION_STATE_ACTIVATING:
    case NM_ACTIVE_CONNECTION_STATE_DEACTIVATING:
      return g_strdup ("network-wireless-acquiring-symbolic");

    case NM_ACTIVE_CONNECTION_STATE_ACTIVATED:
      {
        if (shell_nm_wireless_device_item_get_is_hotspot (self))
          return g_strdup ("network-wireless-hotspot-symbolic");

        if (!device_can_reach_internet (self->client, NM_DEVICE (self->device)))
          return g_strdup ("network-wireless-no-route-symbolic");

        if (self->active_access_point == NULL)
          {
            if (nm_device_wifi_get_mode (self->device) != NM_802_11_MODE_ADHOC)
              g_info ("An active wireless connection, in infrastructure mode, involves no access point?");

            return g_strdup ("network-wireless-offline-symbolic");
          }

        return g_strdup_printf ("network-wireless-signal-%s-symbolic",
                                signal_to_icon (nm_access_point_get_strength (self->active_access_point)));
      }

    default:
      return g_strdup ("network-wireless-signal-none-symbolic");
    }
}

/* _updateItemsVisibility () */
static void
shell_nm_wireless_device_item_update_items_visibility (ShellNmWirelessDeviceItem *self)
{
  gboolean has_windows = session_has_windows ();
  guint n_visible = 0;
  guint i;

  for (i = 0; i < self->item_sorter->items_order->len; i++)
    {
      ShellNmWirelessNetworkItem *item = g_ptr_array_index (self->item_sorter->items_order, i);
      ShellNmWirelessNetwork *network = item->network;
      gboolean visible = (has_windows ||
                          shell_nm_wireless_network_has_connections (network) ||
                          shell_nm_wireless_network_can_autoconnect (network)) &&
                         n_visible < MAX_VISIBLE_NETWORKS;

      set_actor_visible (item, visible);
      if (visible)
        n_visible++;
    }
}

/* _resortItem () */
static void
shell_nm_wireless_device_item_resort_item (ShellNmWirelessDeviceItem *self,
                                           ShellNmWirelessNetworkItem *item)
{
  guint pos = item_sorter_upsert (self->item_sorter, item);

  shell_popup_menu_base_move_menu_item (shell_nm_section_item_get_section (SHELL_NM_SECTION_ITEM (self)),
                                        G_OBJECT (item), pos);

  shell_nm_wireless_device_item_update_items_visibility (self);
}

static ShellNmWirelessNetworkItem *
shell_nm_wireless_device_item_find_item (ShellNmWirelessDeviceItem *self,
                                         ShellNmWirelessNetwork    *network)
{
  guint i;

  for (i = 0; i < self->item_sorter->items_order->len; i++)
    {
      ShellNmWirelessNetworkItem *item = g_ptr_array_index (self->item_sorter->items_order, i);

      if (item->network == network)
        return item;
    }

  return NULL;
}

static void
wireless_device_item_network_changed_cb (ShellNmWirelessNetwork    *network,
                                         GParamSpec                *pspec,
                                         ShellNmWirelessDeviceItem *self)
{
  ShellNmWirelessNetworkItem *item = shell_nm_wireless_device_item_find_item (self, network);

  if (item != NULL)
    shell_nm_wireless_device_item_resort_item (self, item);
}

static void
wireless_device_item_network_destroy_cb (ShellNmWirelessNetwork    *network,
                                         ShellNmWirelessDeviceItem *self)
{
  ShellNmWirelessNetworkItem *found = shell_nm_wireless_device_item_find_item (self, network);
  g_autoptr (ShellNmWirelessNetworkItem) item = found != NULL ? g_object_ref (found) : NULL;
  g_autoptr (ShellNmWirelessNetwork) keep = g_object_ref (network);

  g_ptr_array_remove (self->networks, network);

  if (item != NULL)
    {
      item_sorter_delete (self->item_sorter, item);
      clutter_actor_destroy (CLUTTER_ACTOR (item));
    }
}

static void
wireless_network_item_activate_cb (ShellNmWirelessNetworkItem *item,
                                   const ClutterEvent         *event,
                                   ShellNmWirelessNetwork     *network)
{
  shell_nm_wireless_network_activate (network);
}

static void shell_nm_wireless_device_item_add_access_point (ShellNmWirelessDeviceItem *self,
                                                            NMAccessPoint             *ap);

static void
wireless_device_item_pending_ap_ssid_cb (NMAccessPoint             *ap,
                                         GParamSpec                *pspec,
                                         ShellNmWirelessDeviceItem *self)
{
  g_autoptr (NMAccessPoint) keep = NULL;

  if (nm_access_point_get_ssid (ap) == NULL)
    return;

  keep = g_object_ref (ap);
  g_signal_handlers_disconnect_by_func (ap, wireless_device_item_pending_ap_ssid_cb, self);
  shell_nm_wireless_device_item_add_access_point (self, ap);
}

/* _addAccessPoint () */
static void
shell_nm_wireless_device_item_add_access_point (ShellNmWirelessDeviceItem *self,
                                                NMAccessPoint             *ap)
{
  ShellNmWirelessNetwork *network = NULL;
  guint i;

  if (nm_access_point_get_ssid (ap) == NULL)
    {
      /* This access point is not visible yet. Wait for it to get a ssid */
      g_signal_connect (ap, "notify::ssid",
                        G_CALLBACK (wireless_device_item_pending_ap_ssid_cb), self);
      return;
    }

  for (i = 0; i < self->networks->len; i++)
    {
      ShellNmWirelessNetwork *candidate = g_ptr_array_index (self->networks, i);

      if (shell_nm_wireless_network_check_access_point (candidate, ap))
        {
          network = candidate;
          break;
        }
    }

  if (network == NULL)
    {
      g_autoptr (ShellNmWirelessNetwork) new_network = shell_nm_wireless_network_new (self->device);
      ShellNmWirelessNetworkItem *item = shell_nm_wireless_network_item_new (new_network);
      guint pos;

      network = new_network;

      g_signal_connect (item, "activate",
                        G_CALLBACK (wireless_network_item_activate_cb), network);

      g_signal_connect (network, "notify::icon-name",
                        G_CALLBACK (wireless_device_item_network_changed_cb), self);
      g_signal_connect (network, "notify::is-active",
                        G_CALLBACK (wireless_device_item_network_changed_cb), self);
      g_signal_connect (network, "destroy",
                        G_CALLBACK (wireless_device_item_network_destroy_cb), self);

      pos = item_sorter_upsert (self->item_sorter, item);
      shell_popup_menu_base_insert_menu_item (shell_nm_section_item_get_section (SHELL_NM_SECTION_ITEM (self)),
                                              G_OBJECT (item), pos);
      g_ptr_array_add (self->networks, g_steal_pointer (&new_network));
    }

  shell_nm_wireless_network_add_access_point (network, ap);
}

/* _removeAccessPoint () */
static void
shell_nm_wireless_device_item_remove_access_point (ShellNmWirelessDeviceItem *self,
                                                   NMAccessPoint             *ap)
{
  g_autoptr (ShellNmWirelessNetwork) network = NULL;
  guint i;

  g_signal_handlers_disconnect_by_data (ap, self);

  for (i = 0; i < self->networks->len; i++)
    {
      ShellNmWirelessNetwork *candidate = g_ptr_array_index (self->networks, i);

      if (shell_nm_wireless_network_remove_access_point (candidate, ap))
        {
          network = g_object_ref (candidate);
          break;
        }
    }

  if (network == NULL || shell_nm_wireless_network_has_access_points (network))
    return;

  shell_nm_wireless_network_destroy (network);
}

static void
wireless_device_item_access_point_added_cb (NMDeviceWifi             *device,
                                            NMAccessPoint            *ap,
                                            ShellNmWirelessDeviceItem *self)
{
  shell_nm_wireless_device_item_add_access_point (self, ap);
  shell_nm_wireless_device_item_update_items_visibility (self);
}

static void
wireless_device_item_access_point_removed_cb (NMDeviceWifi             *device,
                                              NMAccessPoint            *ap,
                                              ShellNmWirelessDeviceItem *self)
{
  shell_nm_wireless_device_item_remove_access_point (self, ap);
  shell_nm_wireless_device_item_update_items_visibility (self);
}

static void
wireless_device_item_active_ap_notify_strength_cb (NMAccessPoint             *ap,
                                                   GParamSpec                *pspec,
                                                   ShellNmWirelessDeviceItem *self)
{
  shell_nm_menu_item_notify_icon_name (SHELL_NM_MENU_ITEM (self));
}

static void
wireless_device_item_active_ap_notify_ssid_cb (NMAccessPoint             *ap,
                                               GParamSpec                *pspec,
                                               ShellNmWirelessDeviceItem *self)
{
  shell_nm_menu_item_notify_name (SHELL_NM_MENU_ITEM (self));
}

/* _activeApChanged () */
static void
shell_nm_wireless_device_item_active_ap_changed (ShellNmWirelessDeviceItem *self)
{
  NMAccessPoint *active_ap;

  if (self->active_access_point != NULL)
    g_signal_handlers_disconnect_by_data (self->active_access_point, self);

  active_ap = nm_device_wifi_get_active_access_point (self->device);
  g_set_object (&self->active_access_point, active_ap);

  if (self->active_access_point != NULL)
    {
      g_signal_connect (self->active_access_point, "notify::strength",
                        G_CALLBACK (wireless_device_item_active_ap_notify_strength_cb), self);
      g_signal_connect (self->active_access_point, "notify::ssid",
                        G_CALLBACK (wireless_device_item_active_ap_notify_ssid_cb), self);
    }

  shell_nm_menu_item_notify_icon_name (SHELL_NM_MENU_ITEM (self));
  shell_nm_menu_item_notify_name (SHELL_NM_MENU_ITEM (self));
}

static void
wireless_device_item_notify_active_ap_cb (NMDeviceWifi             *device,
                                          GParamSpec               *pspec,
                                          ShellNmWirelessDeviceItem *self)
{
  shell_nm_wireless_device_item_active_ap_changed (self);
}

static void
wireless_device_item_notify_active_connection_cb (NMDeviceWifi             *device,
                                                  GParamSpec               *pspec,
                                                  ShellNmWirelessDeviceItem *self)
{
  /* _activeConnectionChanged () */
  shell_nm_menu_item_set_active_connection (SHELL_NM_MENU_ITEM (self),
                                            nm_device_get_active_connection (NM_DEVICE (device)));
}

/* _availableConnectionsChanged () */
static void
wireless_device_item_notify_available_connections_cb (NMDeviceWifi             *device,
                                                      GParamSpec               *pspec,
                                                      ShellNmWirelessDeviceItem *self)
{
  const GPtrArray *connections = nm_device_get_available_connections (NM_DEVICE (device));
  guint i;

  for (i = 0; i < self->networks->len; i++)
    shell_nm_wireless_network_check_connections (g_ptr_array_index (self->networks, i), connections);
}

static void
wireless_device_item_state_changed_cb (NMDeviceWifi             *device,
                                       guint                     new_state,
                                       guint                     old_state,
                                       guint                     reason,
                                       ShellNmWirelessDeviceItem *self)
{
  g_object_notify_by_pspec (G_OBJECT (self),
                            wireless_device_item_props[WIRELESS_DEVICE_ITEM_PROP_IS_HOTSPOT]);
}

static void
wireless_device_item_client_notify_cb (NMClient                  *client,
                                       GParamSpec                *pspec,
                                       ShellNmWirelessDeviceItem *self)
{
  shell_nm_menu_item_notify_icon_name (SHELL_NM_MENU_ITEM (self));
}

static void
wireless_device_item_session_updated_cb (ShellNmWirelessDeviceItem *self)
{
  shell_nm_wireless_device_item_update_items_visibility (self);
}

/* activate () */
static void
shell_nm_wireless_device_item_activate_hotspot (ShellNmWirelessDeviceItem *self)
{
  NMActiveConnection *active_connection;

  if (!shell_nm_wireless_device_item_get_is_hotspot (self))
    return;

  active_connection = nm_device_get_active_connection (NM_DEVICE (self->device));
  nm_client_deactivate_connection_async (self->client, active_connection, NULL, NULL, NULL);
}

static void
shell_nm_wireless_device_item_set_single_device_mode (ShellNmWirelessDeviceItem *self,
                                                      gboolean                   single_device_mode)
{
  if (self->single_device_mode == single_device_mode)
    return;

  self->single_device_mode = single_device_mode;
  g_object_notify_by_pspec (G_OBJECT (self),
                            wireless_device_item_props[WIRELESS_DEVICE_ITEM_PROP_SINGLE_DEVICE_MODE]);
}

/* setDeviceName () */
static void
shell_nm_wireless_device_item_set_device_name (ShellNmWirelessDeviceItem *self,
                                               const char                *name)
{
  g_set_str (&self->device_name, name);
  shell_nm_menu_item_notify_name (SHELL_NM_MENU_ITEM (self));
}

static void
shell_nm_wireless_device_item_get_property (GObject    *object,
                                            guint       prop_id,
                                            GValue     *value,
                                            GParamSpec *pspec)
{
  ShellNmWirelessDeviceItem *self = SHELL_NM_WIRELESS_DEVICE_ITEM (object);

  switch (prop_id)
    {
    case WIRELESS_DEVICE_ITEM_PROP_IS_HOTSPOT:
      g_value_set_boolean (value, self->device != NULL &&
                           shell_nm_wireless_device_item_get_is_hotspot (self));
      break;

    case WIRELESS_DEVICE_ITEM_PROP_SINGLE_DEVICE_MODE:
      g_value_set_boolean (value, self->single_device_mode);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_nm_wireless_device_item_set_property (GObject      *object,
                                            guint         prop_id,
                                            const GValue *value,
                                            GParamSpec   *pspec)
{
  ShellNmWirelessDeviceItem *self = SHELL_NM_WIRELESS_DEVICE_ITEM (object);

  switch (prop_id)
    {
    case WIRELESS_DEVICE_ITEM_PROP_SINGLE_DEVICE_MODE:
      shell_nm_wireless_device_item_set_single_device_mode (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_nm_wireless_device_item_destroy (ClutterActor *actor)
{
  ShellNmWirelessDeviceItem *self = SHELL_NM_WIRELESS_DEVICE_ITEM (actor);

  if (self->networks != NULL)
    {
      /* Iterate a copy because our 'destroy' handler modifies the array */
      g_autoptr (GPtrArray) networks = g_ptr_array_new_with_free_func (g_object_unref);
      guint i;

      for (i = 0; i < self->networks->len; i++)
        g_ptr_array_add (networks, g_object_ref (g_ptr_array_index (self->networks, i)));

      for (i = 0; i < networks->len; i++)
        shell_nm_wireless_network_destroy (g_ptr_array_index (networks, i));
    }

  CLUTTER_ACTOR_CLASS (shell_nm_wireless_device_item_parent_class)->destroy (actor);
}

static void
shell_nm_wireless_device_item_dispose (GObject *object)
{
  ShellNmWirelessDeviceItem *self = SHELL_NM_WIRELESS_DEVICE_ITEM (object);

  if (self->client != NULL)
    g_signal_handlers_disconnect_by_data (self->client, self);

  if (self->device != NULL)
    {
      const GPtrArray *access_points = nm_device_wifi_get_access_points (self->device);
      guint i;

      for (i = 0; access_points != NULL && i < access_points->len; i++)
        g_signal_handlers_disconnect_by_data (g_ptr_array_index (access_points, i), self);

      g_signal_handlers_disconnect_by_data (self->device, self);
    }
  disconnect_session_mode (self);

  if (self->active_access_point != NULL)
    g_signal_handlers_disconnect_by_data (self->active_access_point, self);
  g_clear_object (&self->active_access_point);

  if (self->networks != NULL)
    {
      guint i;

      for (i = 0; i < self->networks->len; i++)
        g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->networks, i), self);
      g_ptr_array_set_size (self->networks, 0);
    }
  if (self->item_sorter != NULL)
    item_sorter_clear (self->item_sorter);

  g_clear_object (&self->device);
  g_clear_object (&self->client);

  G_OBJECT_CLASS (shell_nm_wireless_device_item_parent_class)->dispose (object);
}

static void
shell_nm_wireless_device_item_finalize (GObject *object)
{
  ShellNmWirelessDeviceItem *self = SHELL_NM_WIRELESS_DEVICE_ITEM (object);

  g_clear_pointer (&self->networks, g_ptr_array_unref);
  g_clear_pointer (&self->item_sorter, item_sorter_free);
  g_clear_pointer (&self->device_name, g_free);

  G_OBJECT_CLASS (shell_nm_wireless_device_item_parent_class)->finalize (object);
}

static void
shell_nm_wireless_device_item_class_init (ShellNmWirelessDeviceItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  ShellNmMenuItemClass *nm_item_class = SHELL_NM_MENU_ITEM_CLASS (klass);

  object_class->dispose = shell_nm_wireless_device_item_dispose;
  object_class->finalize = shell_nm_wireless_device_item_finalize;
  object_class->get_property = shell_nm_wireless_device_item_get_property;
  object_class->set_property = shell_nm_wireless_device_item_set_property;

  actor_class->destroy = shell_nm_wireless_device_item_destroy;

  nm_item_class->get_name = shell_nm_wireless_device_item_get_name;
  nm_item_class->get_icon_name = shell_nm_wireless_device_item_get_icon_name;

  wireless_device_item_props[WIRELESS_DEVICE_ITEM_PROP_IS_HOTSPOT] =
    g_param_spec_boolean ("is-hotspot", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  wireless_device_item_props[WIRELESS_DEVICE_ITEM_PROP_SINGLE_DEVICE_MODE] =
    g_param_spec_boolean ("single-device-mode", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_WIRELESS_DEVICE_ITEM_PROPS,
                                     wireless_device_item_props);
}

static void
shell_nm_wireless_device_item_init (ShellNmWirelessDeviceItem *self)
{
}

static ShellNmWirelessDeviceItem *
shell_nm_wireless_device_item_new (NMClient     *client,
                                   NMDeviceWifi *device)
{
  ShellNmWirelessDeviceItem *self = g_object_new (SHELL_TYPE_NM_WIRELESS_DEVICE_ITEM,
                                                  "activatable", FALSE,
                                                  "can-focus", FALSE,
                                                  NULL);
  ShellSessionMode *session_mode = get_session_mode ();
  const GPtrArray *access_points;
  guint i;

  self->client = g_object_ref (client);
  self->device = g_object_ref (device);
  self->device_name = g_strdup ("");
  self->networks = g_ptr_array_new_with_free_func (g_object_unref);
  self->item_sorter = item_sorter_new (compare_wireless_network_items, FALSE);

  g_signal_connect (client, "notify::wireless-enabled",
                    G_CALLBACK (wireless_device_item_client_notify_cb), self);
  g_signal_connect (client, "notify::connectivity",
                    G_CALLBACK (wireless_device_item_client_notify_cb), self);
  g_signal_connect (client, "notify::primary-connection",
                    G_CALLBACK (wireless_device_item_client_notify_cb), self);

  g_signal_connect (device, "notify::active-access-point",
                    G_CALLBACK (wireless_device_item_notify_active_ap_cb), self);
  g_signal_connect (device, "notify::active-connection",
                    G_CALLBACK (wireless_device_item_notify_active_connection_cb), self);
  g_signal_connect (device, "notify::available-connections",
                    G_CALLBACK (wireless_device_item_notify_available_connections_cb), self);
  g_signal_connect (device, "state-changed",
                    G_CALLBACK (wireless_device_item_state_changed_cb), self);
  g_signal_connect (device, "access-point-added",
                    G_CALLBACK (wireless_device_item_access_point_added_cb), self);
  g_signal_connect (device, "access-point-removed",
                    G_CALLBACK (wireless_device_item_access_point_removed_cb), self);

  g_object_bind_property (self, "single-device-mode",
                          self, "use-submenu",
                          G_BINDING_INVERT_BOOLEAN);

  if (session_mode != NULL)
    g_signal_connect_swapped (session_mode, "updated",
                              G_CALLBACK (wireless_device_item_session_updated_cb), self);

  access_points = nm_device_wifi_get_access_points (device);
  for (i = 0; access_points != NULL && i < access_points->len; i++)
    shell_nm_wireless_device_item_add_access_point (self, g_ptr_array_index (access_points, i));

  shell_nm_wireless_device_item_active_ap_changed (self);
  wireless_device_item_notify_active_connection_cb (device, NULL, self);
  wireless_device_item_notify_available_connections_cb (device, NULL, self);
  shell_nm_wireless_device_item_update_items_visibility (self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellNmToggle                                                            */
/* ------------------------------------------------------------------------ */

/* A quick settings toggle with a menu which lists the devices (wired, Wi-Fi,
 * mobile broadband and Bluetooth tethering toggles) or the connections (VPN
 * toggle) it handles */

typedef enum
{
  TOGGLE_KIND_WIRED,      /* NMWiredToggle */
  TOGGLE_KIND_WIRELESS,   /* NMWirelessToggle */
  TOGGLE_KIND_MODEM,      /* NMModemToggle */
  TOGGLE_KIND_BLUETOOTH,  /* NMBluetoothToggle */
  TOGGLE_KIND_VPN,        /* NMVpnToggle */
} ToggleKind;

/* An item of the menu, and the NMDevice or NMConnection it stands for */
typedef struct
{
  GObject *key;
  ClutterActor *item;
} ToggleEntry;

#define SHELL_TYPE_NM_TOGGLE (shell_nm_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellNmToggle, shell_nm_toggle,
                      SHELL, NM_TOGGLE, ShellQuickMenuToggle)

struct _ShellNmToggle
{
  ShellQuickMenuToggle parent;

  ToggleKind kind;
  NMDeviceType device_type;       /* device toggles */
  NMClient *client;

  GPtrArray *entries;             /* ToggleEntry, in the order they were added */
  ItemSorter *item_sorter;
  ShellPopupMenuSection *items_section;     /* weak */
  GBindingGroup *item_binding;

  GPtrArray *nm_devices;          /* NMDevice, device toggles */
  GHashTable *device_names;       /* NMDevice -> char *, device toggles */

  ShellSpinner *scanning_spinner; /* weak, Wi-Fi toggle */
  guint scan_timeout_id;
  GCancellable *cancellable;

  ShellPopupBaseMenuItem *wwan_settings;    /* weak, mobile broadband toggle */
  ShellPopupBaseMenuItem *legacy_settings;  /* weak, mobile broadband toggle */
};

enum
{
  TOGGLE_ACTIVATION_FAILED,

  N_TOGGLE_SIGNALS
};

static guint toggle_signals[N_TOGGLE_SIGNALS] = { 0, };

G_DEFINE_FINAL_TYPE (ShellNmToggle, shell_nm_toggle, SHELL_TYPE_QUICK_MENU_TOGGLE)

static void shell_nm_toggle_sync (ShellNmToggle *self);

static void
toggle_entry_free (ToggleEntry *entry)
{
  g_clear_object (&entry->key);
  g_clear_object (&entry->item);
  g_free (entry);
}

static gboolean
toggle_is_device_toggle (ShellNmToggle *self)
{
  return self->kind != TOGGLE_KIND_VPN;
}

static ShellPopupMenuBase *
toggle_get_menu (ShellNmToggle *self)
{
  return SHELL_POPUP_MENU_BASE (shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self)));
}

static ToggleEntry *
toggle_find_entry (ShellNmToggle *self,
                   gpointer       key)
{
  guint i;

  for (i = 0; i < self->entries->len; i++)
    {
      ToggleEntry *entry = g_ptr_array_index (self->entries, i);

      if (entry->key == key)
        return entry;
    }

  return NULL;
}

static ToggleEntry *
toggle_find_entry_by_item (ShellNmToggle *self,
                           gpointer       item)
{
  guint i;

  for (i = 0; i < self->entries->len; i++)
    {
      ToggleEntry *entry = g_ptr_array_index (self->entries, i);

      if ((gpointer) entry->item == item)
        return entry;
    }

  return NULL;
}

static ClutterActor *
toggle_get_item (ShellNmToggle *self,
                 gpointer       key)
{
  ToggleEntry *entry = toggle_find_entry (self, key);

  return entry != NULL ? entry->item : NULL;
}

/* _getDefaultName (): the title when there is a primary item. @self may have
 * no device, then there is no name. */
static char *
toggle_dup_default_name (ShellNmToggle *self)
{
  switch (self->kind)
    {
    case TOGGLE_KIND_VPN:
      return g_strdup (_("VPN"));

    case TOGGLE_KIND_BLUETOOTH:
      /* Translators: "Tether" from "Bluetooth Tether" */
      return g_strdup (_("Tether"));

    case TOGGLE_KIND_MODEM:
      /* Translators: "Mobile" from "Mobile Broadband" */
      return g_strdup (_("Mobile"));

    case TOGGLE_KIND_WIRED:
    case TOGGLE_KIND_WIRELESS:
      {
        NMDevice *device;
        g_auto (GStrv) names = NULL;

        if (self->nm_devices->len == 0)
          return NULL;

        device = g_ptr_array_index (self->nm_devices, 0);
        names = nm_device_disambiguate_names (&device, 1);
        return names != NULL ? g_strdup (names[0]) : NULL;
      }

    default:
      g_assert_not_reached ();
    }
}

/* The items which are active, in order; free with g_ptr_array_unref () */
static GPtrArray *
toggle_dup_active_items (ShellNmToggle *self)
{
  GPtrArray *active_items = g_ptr_array_new_with_free_func (g_object_unref);
  guint i;

  for (i = 0; i < self->item_sorter->items_order->len; i++)
    {
      ShellNmMenuItem *item = g_ptr_array_index (self->item_sorter->items_order, i);

      if (shell_nm_menu_item_get_is_active (item))
        g_ptr_array_add (active_items, g_object_ref (item));
    }

  return active_items;
}

static ShellNmMenuItem *
toggle_get_first_active_item (ShellNmToggle *self)
{
  guint i;

  for (i = 0; i < self->item_sorter->items_order->len; i++)
    {
      ShellNmMenuItem *item = g_ptr_array_index (self->item_sorter->items_order, i);

      if (shell_nm_menu_item_get_is_active (item))
        return item;
    }

  return NULL;
}

/* The item whose properties the toggle shows (`_itemBinding.source`) */
static ShellNmMenuItem *
toggle_get_binding_source (ShellNmToggle *self)
{
  g_autoptr (GObject) source = NULL;

  if (self->item_binding == NULL)
    return NULL;

  source = g_binding_group_dup_source (self->item_binding);

  /* The items are kept alive by the toggle, which is what the caller needs */
  return source != NULL ? SHELL_NM_MENU_ITEM (source) : NULL;
}

/* _transformSubtitle (): ignore the provided label if there are multiple
 * active items, and replace it with something like "2 connected" */
static char *
toggle_dup_subtitle (ShellNmToggle *self,
                     const char    *source)
{
  guint n_active = 0;
  const char *title;
  char *subtitle;

  if (st_button_get_checked (ST_BUTTON (self)))
    {
      g_autoptr (GPtrArray) active_items = toggle_dup_active_items (self);

      n_active = active_items->len;
    }

  if (n_active > 1)
    subtitle = g_strdup_printf (ngettext ("%d connected", "%d connected", n_active), (int) n_active);
  else
    subtitle = g_strdup (source);

  /* NMDeviceToggle: no subtitle that says the same as the title */
  title = shell_quick_menu_toggle_get_title (SHELL_QUICK_MENU_TOGGLE (self));
  if (toggle_is_device_toggle (self) && subtitle != NULL && g_strcmp0 (subtitle, title) == 0)
    g_clear_pointer (&subtitle, g_free);

  return subtitle;
}

static gboolean
toggle_title_transform_cb (GBinding     *binding,
                           const GValue *from_value,
                           GValue       *to_value,
                           gpointer      user_data)
{
  ShellNmToggle *self = user_data;
  char *name = toggle_dup_default_name (self);

  /* Without a name the title stays what it is */
  if (name == NULL)
    return FALSE;

  g_value_take_string (to_value, name);
  return TRUE;
}

static gboolean
toggle_subtitle_transform_cb (GBinding     *binding,
                              const GValue *from_value,
                              GValue       *to_value,
                              gpointer      user_data)
{
  ShellNmToggle *self = user_data;

  g_value_take_string (to_value, toggle_dup_subtitle (self, g_value_get_string (from_value)));
  return TRUE;
}

/* _updateItemsVisibility () */
static void
toggle_update_items_visibility (ShellNmToggle *self)
{
  g_autoptr (GPtrArray) items = item_sorter_dup_items_by_mru (self->item_sorter);
  guint i;

  for (i = 0; i < items->len; i++)
    set_actor_visible (g_ptr_array_index (items, i), i < MAX_VISIBLE_NETWORKS);
}

/* _itemActiveChanged () */
static void
toggle_item_active_changed_cb (GObject       *item,
                               GParamSpec    *pspec,
                               ShellNmToggle *self)
{
  ShellNmMenuItem *source = toggle_get_binding_source (self);

  /* force an update in case we changed from or to multiple active items */
  if (source != NULL)
    shell_nm_menu_item_notify_name (source);

  shell_nm_toggle_sync (self);
}

/* _updateChecked () */
static void
toggle_update_checked (ShellNmToggle *self)
{
  /* The Wi-Fi toggle is checked through a property binding */
  if (self->kind == TOGGLE_KIND_WIRELESS)
    return;

  st_button_set_checked (ST_BUTTON (self), toggle_get_first_active_item (self) != NULL);
}

/* _resortItem () */
static void
toggle_resort_item (ShellNmToggle *self,
                    ClutterActor  *item)
{
  guint pos = item_sorter_upsert (self->item_sorter, item);

  if (self->items_section != NULL)
    shell_popup_menu_base_move_menu_item (SHELL_POPUP_MENU_BASE (self->items_section),
                                          G_OBJECT (item), pos);
}

static void
toggle_item_name_changed_cb (GObject       *item,
                             GParamSpec    *pspec,
                             ShellNmToggle *self)
{
  toggle_resort_item (self, CLUTTER_ACTOR (item));
}

/* _removeItem () */
static void
toggle_remove_item (ShellNmToggle *self,
                    gpointer       key)
{
  ToggleEntry *entry = toggle_find_entry (self, key);
  g_autoptr (ClutterActor) item = NULL;

  if (entry == NULL)
    return;

  item = g_object_ref (entry->item);

  item_sorter_delete (self->item_sorter, item);
  g_signal_handlers_disconnect_by_data (item, self);
  g_ptr_array_remove (self->entries, entry);

  clutter_actor_destroy (item);
  shell_nm_toggle_sync (self);
}

static void
toggle_item_destroy_cb (ClutterActor  *item,
                        ShellNmToggle *self)
{
  ToggleEntry *entry = toggle_find_entry_by_item (self, item);

  if (entry != NULL)
    {
      g_autoptr (GObject) key = g_object_ref (entry->key);

      toggle_remove_item (self, key);
    }
}

/* _addItem () */
static void
toggle_add_item (ShellNmToggle *self,
                 GObject       *key,
                 ClutterActor  *item)
{
  ToggleEntry *entry;
  guint pos;

  if (toggle_find_entry (self, key) != NULL)
    {
      g_warning ("%s: the toggle already has an item for the key", G_STRFUNC);
      clutter_actor_destroy (item);
      return;
    }

  /* The menu is gone (the shell is shutting down) */
  if (self->items_section == NULL)
    {
      clutter_actor_destroy (item);
      return;
    }

  g_signal_connect (item, "notify::is-active",
                    G_CALLBACK (toggle_item_active_changed_cb), self);
  g_signal_connect (item, "notify::item-name",
                    G_CALLBACK (toggle_item_name_changed_cb), self);
  g_signal_connect (item, "destroy",
                    G_CALLBACK (toggle_item_destroy_cb), self);

  entry = g_new0 (ToggleEntry, 1);
  entry->key = g_object_ref (key);
  entry->item = g_object_ref (item);
  g_ptr_array_add (self->entries, entry);

  pos = item_sorter_upsert (self->item_sorter, item);
  shell_popup_menu_base_insert_menu_item (SHELL_POPUP_MENU_BASE (self->items_section),
                                          G_OBJECT (item), pos);
  shell_nm_toggle_sync (self);
}

/* _getPrimaryItem () */
static ShellNmMenuItem *
toggle_get_primary_item (ShellNmToggle *self)
{
  ShellNmMenuItem *item;
  guint i;

  /* the hotspot of the Wi-Fi toggle comes first */
  if (self->kind == TOGGLE_KIND_WIRELESS)
    {
      for (i = 0; i < self->entries->len; i++)
        {
          ToggleEntry *entry = g_ptr_array_index (self->entries, i);
          gboolean is_hotspot = FALSE;

          g_object_get (entry->item, "is-hotspot", &is_hotspot, NULL);
          if (is_hotspot)
            return SHELL_NM_MENU_ITEM (entry->item);
        }
    }

  /* prefer active items */
  item = toggle_get_first_active_item (self);
  if (item != NULL)
    return item;

  /* otherwise prefer the most-recently used */
  item = item_sorter_first_by_mru (self->item_sorter);
  if (item != NULL && shell_nm_menu_item_get_timestamp (item) > 0)
    return item;

  /* as a last resort, return the top-most visible item */
  for (i = 0; i < self->item_sorter->items_order->len; i++)
    {
      item = g_ptr_array_index (self->item_sorter->items_order, i);

      if (clutter_actor_is_visible (CLUTTER_ACTOR (item)))
        return item;
    }

  return NULL;
}

static void
shell_nm_toggle_sync (ShellNmToggle *self)
{
  gboolean visible;
  guint i;

  /* Disposed */
  if (self->item_binding == NULL)
    return;

  if (self->client == NULL)
    {
      /* this._client?.networking_enabled && ... */
      visible = FALSE;
    }
  else
    {
      visible = nm_client_networking_get_enabled (self->client) && self->entries->len > 0;
    }
  set_actor_visible (self, visible);

  toggle_update_items_visibility (self);
  toggle_update_checked (self);
  g_binding_group_set_source (self->item_binding, toggle_get_primary_item (self));

  /* NMDeviceToggle._sync () */
  if (toggle_is_device_toggle (self))
    {
      guint n_items = self->entries->len;

      for (i = 0; i < n_items; i++)
        {
          ToggleEntry *entry = g_ptr_array_index (self->entries, i);

          if (self->kind == TOGGLE_KIND_WIRELESS)
            shell_nm_wireless_device_item_set_single_device_mode (SHELL_NM_WIRELESS_DEVICE_ITEM (entry->item),
                                                                  n_items == 1);
          else
            shell_nm_device_item_set_single_device_mode (SHELL_NM_DEVICE_ITEM (entry->item),
                                                         n_items == 1);
        }
    }

  /* NMModemToggle._sync () */
  if (self->kind == TOGGLE_KIND_MODEM &&
      self->wwan_settings != NULL && self->legacy_settings != NULL)
    {
      gboolean use_wwan_panel = FALSE;

      for (i = 0; i < self->entries->len; i++)
        {
          ToggleEntry *entry = g_ptr_array_index (self->entries, i);

          if (shell_nm_device_item_get_wwan_panel_supported (SHELL_NM_DEVICE_ITEM (entry->item)))
            {
              use_wwan_panel = TRUE;
              break;
            }
        }

      set_actor_visible (self->wwan_settings, use_wwan_panel);
      set_actor_visible (self->legacy_settings, !use_wwan_panel);
    }
}

/* ---- The VPN toggle ---- */

static void
vpn_toggle_activate_connection (GObject      *section,
                                NMConnection *connection)
{
  ShellNmToggle *self = SHELL_NM_TOGGLE (section);

  nm_client_activate_connection_async (self->client, connection, NULL, NULL, NULL, NULL, NULL);
}

static void
vpn_toggle_deactivate_connection (GObject            *section,
                                  NMActiveConnection *active_connection)
{
  ShellNmToggle *self = SHELL_NM_TOGGLE (section);

  nm_client_deactivate_connection_async (self->client, active_connection, NULL, NULL, NULL);
}

/* _shouldHandleConnection () */
static gboolean
vpn_toggle_should_handle_connection (NMConnection *connection)
{
  NMSettingConnection *setting = nm_connection_get_setting_connection (connection);
  const char *type;

  if (setting == NULL)
    return FALSE;

  /* Ignore slave connection */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  if (nm_setting_connection_get_master (setting) != NULL)
    return FALSE;
  G_GNUC_END_IGNORE_DEPRECATIONS

  type = nm_setting_connection_get_connection_type (setting);
  return g_strcmp0 (type, NM_SETTING_VPN_SETTING_NAME) == 0 ||
         g_strcmp0 (type, NM_SETTING_WIREGUARD_SETTING_NAME) == 0;
}

/* _syncActiveConnections () */
static void
vpn_toggle_sync_active_connections (ShellNmToggle *self)
{
  const GPtrArray *active_connections = nm_client_get_active_connections (self->client);
  guint i;

  for (i = 0; i < self->entries->len; i++)
    {
      ToggleEntry *entry = g_ptr_array_index (self->entries, i);

      shell_nm_menu_item_set_active_connection (SHELL_NM_MENU_ITEM (entry->item), NULL);
    }

  for (i = 0; active_connections != NULL && i < active_connections->len; i++)
    {
      NMActiveConnection *active_connection = g_ptr_array_index (active_connections, i);
      NMConnection *connection = NM_CONNECTION (nm_active_connection_get_connection (active_connection));
      ClutterActor *item;

      if (connection == NULL || !vpn_toggle_should_handle_connection (connection))
        continue;

      item = toggle_get_item (self, connection);
      if (item != NULL)
        shell_nm_menu_item_set_active_connection (SHELL_NM_MENU_ITEM (item), active_connection);
    }
}

/* _onConnectionChanged () */
static void
vpn_toggle_connection_changed_cb (NMConnection  *connection,
                                  ShellNmToggle *self)
{
  ClutterActor *item = toggle_get_item (self, connection);

  if (item != NULL)
    shell_nm_connection_item_update_for_connection (SHELL_NM_CONNECTION_ITEM (item), connection);
}

static void
toggle_item_activation_failed_cb (GObject       *item,
                                  ShellNmToggle *self)
{
  g_signal_emit (self, toggle_signals[TOGGLE_ACTIVATION_FAILED], 0);
}

/* _addConnection () */
static void
vpn_toggle_add_connection (ShellNmToggle *self,
                           NMConnection  *connection)
{
  ShellNmConnectionItem *item;

  if (toggle_find_entry (self, connection) != NULL)
    return;

  if (!vpn_toggle_should_handle_connection (connection))
    return;

  g_signal_connect (connection, "changed",
                    G_CALLBACK (vpn_toggle_connection_changed_cb), self);

  item = shell_nm_connection_item_new (CONNECTION_ITEM_KIND_VPN, G_OBJECT (self),
                                       vpn_toggle_activate_connection,
                                       vpn_toggle_deactivate_connection,
                                       connection);
  g_signal_connect (item, "activation-failed",
                    G_CALLBACK (toggle_item_activation_failed_cb), self);
  toggle_add_item (self, G_OBJECT (connection), CLUTTER_ACTOR (item));

  /* FIXME: NM is emitting "connection-added" after "notify::active-connections",
   * so we need to sync connections here once again. */
  vpn_toggle_sync_active_connections (self);
}

/* _removeConnection () */
static void
vpn_toggle_remove_connection (ShellNmToggle *self,
                              NMConnection  *connection)
{
  g_autoptr (NMConnection) keep = g_object_ref (connection);

  toggle_remove_item (self, connection);
  g_signal_handlers_disconnect_by_data (connection, self);
}

static void
vpn_toggle_connection_added_cb (NMClient      *client,
                                NMConnection  *connection,
                                ShellNmToggle *self)
{
  vpn_toggle_add_connection (self, connection);
}

static void
vpn_toggle_connection_removed_cb (NMClient      *client,
                                  NMConnection  *connection,
                                  ShellNmToggle *self)
{
  vpn_toggle_remove_connection (self, connection);
}

static void
vpn_toggle_notify_active_connections_cb (NMClient      *client,
                                         GParamSpec    *pspec,
                                         ShellNmToggle *self)
{
  vpn_toggle_sync_active_connections (self);
}

/* ---- The device toggles ---- */

/* _shouldShowDevice () */
static gboolean
device_toggle_should_show_device (ShellNmToggle *self,
                                  NMDevice      *device)
{
  /* don't disappear if wireless-enabled is false */
  if (self->kind == TOGGLE_KIND_WIRELESS &&
      nm_device_get_state (device) == NM_DEVICE_STATE_UNAVAILABLE)
    return TRUE;

  switch (nm_device_get_state (device))
    {
    case NM_DEVICE_STATE_DISCONNECTED:
    case NM_DEVICE_STATE_ACTIVATED:
    case NM_DEVICE_STATE_DEACTIVATING:
    case NM_DEVICE_STATE_PREPARE:
    case NM_DEVICE_STATE_CONFIG:
    case NM_DEVICE_STATE_IP_CONFIG:
    case NM_DEVICE_STATE_IP_CHECK:
    case NM_DEVICE_STATE_SECONDARIES:
    case NM_DEVICE_STATE_NEED_AUTH:
    case NM_DEVICE_STATE_FAILED:
      return TRUE;

    case NM_DEVICE_STATE_UNMANAGED:
    case NM_DEVICE_STATE_UNAVAILABLE:
    default:
      return FALSE;
    }
}

static void
device_toggle_set_item_device_name (ClutterActor *item,
                                    const char   *name)
{
  if (SHELL_IS_NM_WIRELESS_DEVICE_ITEM (item))
    shell_nm_wireless_device_item_set_device_name (SHELL_NM_WIRELESS_DEVICE_ITEM (item), name);
  else
    shell_nm_device_item_set_device_name (SHELL_NM_DEVICE_ITEM (item), name);
}

/* _syncDeviceNames () */
static void
device_toggle_sync_device_names (ShellNmToggle *self)
{
  g_autofree NMDevice **devices = NULL;
  g_auto (GStrv) names = NULL;
  guint n_devices = self->nm_devices->len;
  guint i;

  g_hash_table_remove_all (self->device_names);
  if (n_devices == 0)
    return;

  devices = g_new0 (NMDevice *, n_devices);
  for (i = 0; i < n_devices; i++)
    devices[i] = g_ptr_array_index (self->nm_devices, i);

  names = nm_device_disambiguate_names (devices, n_devices);
  if (names == NULL)
    return;

  for (i = 0; i < n_devices && names[i] != NULL; i++)
    {
      ClutterActor *item = toggle_get_item (self, devices[i]);

      g_hash_table_insert (self->device_names, g_object_ref (devices[i]), g_strdup (names[i]));
      if (item != NULL)
        device_toggle_set_item_device_name (item, names[i]);
    }
}

static ClutterActor *
device_toggle_create_device_menu_item (ShellNmToggle *self,
                                       NMDevice      *device)
{
  switch (self->kind)
    {
    case TOGGLE_KIND_WIRED:
      return CLUTTER_ACTOR (shell_nm_device_item_new (DEVICE_ITEM_KIND_WIRED, self->client, device));

    case TOGGLE_KIND_WIRELESS:
      return CLUTTER_ACTOR (shell_nm_wireless_device_item_new (self->client, NM_DEVICE_WIFI (device)));

    case TOGGLE_KIND_MODEM:
      return CLUTTER_ACTOR (shell_nm_device_item_new (DEVICE_ITEM_KIND_MODEM, self->client, device));

    case TOGGLE_KIND_BLUETOOTH:
      return CLUTTER_ACTOR (shell_nm_device_item_new (DEVICE_ITEM_KIND_BLUETOOTH, self->client, device));

    default:
      g_assert_not_reached ();
    }
}

/* _ensureDeviceItem () */
static void
device_toggle_ensure_device_item (ShellNmToggle *self,
                                  NMDevice      *device)
{
  ClutterActor *item;
  const char *name;

  if (toggle_find_entry (self, device) != NULL)
    return;

  item = device_toggle_create_device_menu_item (self, device);
  name = g_hash_table_lookup (self->device_names, device);
  device_toggle_set_item_device_name (item, name != NULL ? name : "");
  toggle_add_item (self, G_OBJECT (device), item);
}

/* _syncDeviceItem () */
static void
device_toggle_sync_device_item (ShellNmToggle *self,
                                NMDevice      *device)
{
  if (device_toggle_should_show_device (self, device))
    device_toggle_ensure_device_item (self, device);
  else
    toggle_remove_item (self, device);
}

/* _deviceStateChanged () */
static void
device_toggle_state_changed_cb (NMDevice      *device,
                                guint          new_state,
                                guint          old_state,
                                guint          reason,
                                ShellNmToggle *self)
{
  if (new_state == old_state)
    {
      g_info ("%s emitted state-changed without actually changing state",
              nm_device_get_iface (device));
      return;
    }

  /* Emit a notification if activation fails, but don't do it
     if the reason is no secrets, as that indicates the user
     cancelled the agent dialog */
  if (new_state == NM_DEVICE_STATE_FAILED &&
      reason != NM_DEVICE_STATE_REASON_NO_SECRETS)
    g_signal_emit (self, toggle_signals[TOGGLE_ACTIVATION_FAILED], 0);
}

static void
device_toggle_notify_interface_cb (NMDevice      *device,
                                   GParamSpec    *pspec,
                                   ShellNmToggle *self)
{
  device_toggle_sync_device_names (self);
}

static void
device_toggle_notify_state_cb (NMDevice      *device,
                               GParamSpec    *pspec,
                               ShellNmToggle *self)
{
  device_toggle_sync_device_item (self, device);
}

/* _addDevice () */
static void
device_toggle_add_device (ShellNmToggle *self,
                          NMDevice      *device)
{
  if (g_ptr_array_find (self->nm_devices, device, NULL))
    return;

  if (nm_device_get_device_type (device) != self->device_type)
    return;

  g_signal_connect (device, "state-changed",
                    G_CALLBACK (device_toggle_state_changed_cb), self);
  g_signal_connect (device, "notify::interface",
                    G_CALLBACK (device_toggle_notify_interface_cb), self);
  g_signal_connect (device, "notify::state",
                    G_CALLBACK (device_toggle_notify_state_cb), self);

  g_ptr_array_add (self->nm_devices, g_object_ref (device));
  device_toggle_sync_device_item (self, device);
}

/* _removeDevice () */
static void
device_toggle_remove_device (ShellNmToggle *self,
                             NMDevice      *device)
{
  g_autoptr (NMDevice) keep = g_object_ref (device);

  if (!g_ptr_array_remove (self->nm_devices, device))
    return;

  g_signal_handlers_disconnect_by_data (device, self);
  toggle_remove_item (self, device);
}

static void
device_toggle_device_added_cb (NMClient      *client,
                               NMDevice      *device,
                               ShellNmToggle *self)
{
  device_toggle_add_device (self, device);
  device_toggle_sync_device_names (self);
}

static void
device_toggle_device_removed_cb (NMClient      *client,
                                 NMDevice      *device,
                                 ShellNmToggle *self)
{
  device_toggle_remove_device (self, device);
  device_toggle_sync_device_names (self);
}

/* ---- The Wi-Fi toggle ---- */

typedef struct
{
  int ref_count;
  GWeakRef toggle;
  GCancellable *cancellable;
  guint remaining;
} ScanRound;

typedef struct
{
  ScanRound *round;
  NMDeviceWifi *device;
  gint64 last_scan;
} ScanDevice;

static ScanRound *
scan_round_ref (ScanRound *round)
{
  round->ref_count++;
  return round;
}

static void
scan_round_unref (ScanRound *round)
{
  if (--round->ref_count > 0)
    return;

  g_weak_ref_clear (&round->toggle);
  g_clear_object (&round->cancellable);
  g_free (round);
}

static void
scan_device_free (ScanDevice *data)
{
  g_clear_object (&data->device);
  g_clear_pointer (&data->round, scan_round_unref);
  g_free (data);
}

/* The end of the scan of a device; the spinner stops with the last one */
static void
scan_device_done (ScanDevice *data)
{
  ScanRound *round = data->round;

  if (g_cancellable_is_cancelled (round->cancellable))
    return;

  if (round->remaining > 0 && --round->remaining == 0)
    {
      g_autoptr (ShellNmToggle) self = g_weak_ref_get (&round->toggle);

      if (self != NULL && self->scanning_spinner != NULL)
        shell_spinner_stop (self->scanning_spinner);
    }
}

/* Wait for the lastScan property to update, which indicates the end of the
 * scan */
static gboolean
scan_device_poll_cb (gpointer user_data)
{
  ScanDevice *data = user_data;

  if (g_cancellable_is_cancelled (data->round->cancellable))
    return G_SOURCE_REMOVE;

  if (nm_device_wifi_get_last_scan (data->device) == data->last_scan)
    return G_SOURCE_CONTINUE;

  scan_device_done (data);
  return G_SOURCE_REMOVE;
}

static void
scan_device_request_scan_cb (GObject      *source_object,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  ScanDevice *data = user_data;
  g_autoptr (GError) error = NULL;
  guint id;

  if (!nm_device_wifi_request_scan_finish (NM_DEVICE_WIFI (source_object), res, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_debug ("Failed to request a Wi-Fi scan: %s", error->message);
          scan_device_done (data);
        }

      scan_device_free (data);
      return;
    }

  if (g_cancellable_is_cancelled (data->round->cancellable))
    {
      scan_device_free (data);
      return;
    }

  id = g_timeout_add_full (G_PRIORITY_DEFAULT, WIFI_SCAN_POLL_INTERVAL,
                           scan_device_poll_cb, data,
                           (GDestroyNotify) scan_device_free);
  g_source_set_name_by_id (id, "[gnome-shell] wifi scan poll");
}

/* _scanDevices () */
static void
wireless_toggle_scan_devices (ShellNmToggle *self)
{
  ScanRound *round;
  guint i;

  if (self->client == NULL || !nm_client_wireless_get_enabled (self->client))
    return;

  if (self->scanning_spinner != NULL)
    shell_spinner_play (self->scanning_spinner);

  if (self->entries->len == 0)
    {
      if (self->scanning_spinner != NULL)
        shell_spinner_stop (self->scanning_spinner);
      return;
    }

  round = g_new0 (ScanRound, 1);
  round->ref_count = 1;
  g_weak_ref_init (&round->toggle, self);
  round->cancellable = g_object_ref (self->cancellable);
  round->remaining = self->entries->len;

  for (i = 0; i < self->entries->len; i++)
    {
      ToggleEntry *entry = g_ptr_array_index (self->entries, i);
      ScanDevice *data = g_new0 (ScanDevice, 1);

      data->round = scan_round_ref (round);
      data->device = g_object_ref (NM_DEVICE_WIFI (entry->key));
      data->last_scan = nm_device_wifi_get_last_scan (data->device);

      nm_device_wifi_request_scan_async (data->device, round->cancellable,
                                         scan_device_request_scan_cb, data);
    }

  scan_round_unref (round);
}

static gboolean
wireless_toggle_scan_timeout_cb (gpointer user_data)
{
  wireless_toggle_scan_devices (SHELL_NM_TOGGLE (user_data));

  return G_SOURCE_CONTINUE;
}

/* _stopScanning () */
static void
wireless_toggle_stop_scanning (ShellNmToggle *self)
{
  g_clear_handle_id (&self->scan_timeout_id, g_source_remove);
}

/* _startScanning () */
static void
wireless_toggle_start_scanning (ShellNmToggle *self)
{
  wireless_toggle_stop_scanning (self);

  self->scan_timeout_id = g_timeout_add_seconds (WIFI_SCAN_FREQUENCY,
                                                 wireless_toggle_scan_timeout_cb, self);
  g_source_set_name_by_id (self->scan_timeout_id, "[gnome-shell] wifi scan");

  wireless_toggle_scan_devices (self);
}

static void
wireless_toggle_menu_open_state_changed_cb (ShellPopupMenuBase *menu,
                                            gboolean            is_open,
                                            ShellNmToggle      *self)
{
  if (is_open)
    wireless_toggle_start_scanning (self);
  else
    wireless_toggle_stop_scanning (self);
}

static void
wireless_toggle_set_enabled_cb (GObject      *source_object,
                                GAsyncResult *res,
                                gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!nm_client_dbus_set_property_finish (NM_CLIENT (source_object), res, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("Failed to toggle the Wi-Fi: %s", error->message);
}

/* _toggleWirelessEnabled () */
static void
wireless_toggle_toggle_wireless_enabled (ShellNmToggle *self)
{
  gboolean enabled = !nm_client_wireless_get_enabled (self->client);

  nm_client_dbus_set_property (self->client,
                               NM_DBUS_PATH,
                               NM_DBUS_INTERFACE,
                               "WirelessEnabled",
                               g_variant_new_boolean (enabled),
                               2000,
                               self->cancellable,
                               wireless_toggle_set_enabled_cb,
                               NULL);
}

/* ---- Common ---- */

/* activate () */
static void
shell_nm_toggle_activate (ShellNmToggle *self)
{
  g_autoptr (GPtrArray) active_items = NULL;
  ShellNmMenuItem *primary_item;
  guint i;

  if (self->client == NULL)
    return;

  if (self->kind == TOGGLE_KIND_WIRELESS)
    {
      gboolean is_hotspot = FALSE;

      primary_item = toggle_get_binding_source (self);
      if (primary_item != NULL)
        g_object_get (primary_item, "is-hotspot", &is_hotspot, NULL);

      if (is_hotspot)
        shell_nm_wireless_device_item_activate_hotspot (SHELL_NM_WIRELESS_DEVICE_ITEM (primary_item));
      else
        wireless_toggle_toggle_wireless_enabled (self);

      return;
    }

  active_items = toggle_dup_active_items (self);
  if (active_items->len > 0)
    {
      for (i = 0; i < active_items->len; i++)
        shell_popup_base_menu_item_activate (g_ptr_array_index (active_items, i), NULL);
    }
  else
    {
      primary_item = toggle_get_binding_source (self);
      if (primary_item != NULL)
        shell_popup_base_menu_item_activate (SHELL_POPUP_BASE_MENU_ITEM (primary_item), NULL);
    }
}

static void
toggle_clicked_cb (StButton      *button,
                   int            clicked_button,
                   ShellNmToggle *self)
{
  shell_nm_toggle_activate (self);
}

static void
toggle_client_networking_enabled_cb (NMClient      *client,
                                     GParamSpec    *pspec,
                                     ShellNmToggle *self)
{
  shell_nm_toggle_sync (self);
}

/* _loadInitialItems () */
static void
toggle_load_initial_items (ShellNmToggle *self)
{
  guint i;

  if (self->kind == TOGGLE_KIND_VPN)
    {
      g_autoptr (GPtrArray) connections = g_ptr_array_new_with_free_func (g_object_unref);
      const GPtrArray *all = nm_client_get_connections (self->client);

      for (i = 0; all != NULL && i < all->len; i++)
        g_ptr_array_add (connections, g_object_ref (g_ptr_array_index (all, i)));

      for (i = 0; i < connections->len; i++)
        vpn_toggle_add_connection (self, g_ptr_array_index (connections, i));

      vpn_toggle_sync_active_connections (self);
    }
  else
    {
      g_autoptr (GPtrArray) devices = g_ptr_array_new_with_free_func (g_object_unref);
      const GPtrArray *all = nm_client_get_devices (self->client);

      for (i = 0; all != NULL && i < all->len; i++)
        g_ptr_array_add (devices, g_object_ref (g_ptr_array_index (all, i)));

      for (i = 0; i < devices->len; i++)
        device_toggle_add_device (self, g_ptr_array_index (devices, i));

      device_toggle_sync_device_names (self);
    }
}

/* setClient () */
static void
shell_nm_toggle_set_client (ShellNmToggle *self,
                            NMClient      *client)
{
  g_autoptr (GPtrArray) items = NULL;
  guint i;

  if (toggle_is_device_toggle (self))
    g_ptr_array_set_size (self->nm_devices, 0);

  if (self->client == client)
    return;

  if (self->client != NULL)
    g_signal_handlers_disconnect_by_data (self->client, self);
  g_set_object (&self->client, client);
  if (self->client != NULL)
    g_signal_connect (self->client, "notify::networking-enabled",
                      G_CALLBACK (toggle_client_networking_enabled_cb), self);

  items = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = 0; i < self->entries->len; i++)
    g_ptr_array_add (items, g_object_ref (((ToggleEntry *) g_ptr_array_index (self->entries, i))->item));
  for (i = 0; i < items->len; i++)
    clutter_actor_destroy (g_ptr_array_index (items, i));
  g_ptr_array_set_size (self->entries, 0);
  item_sorter_clear (self->item_sorter);

  if (self->client != NULL)
    toggle_load_initial_items (self);
  shell_nm_toggle_sync (self);

  if (self->client == NULL)
    return;

  if (self->kind == TOGGLE_KIND_VPN)
    {
      g_signal_connect (self->client, "connection-added",
                        G_CALLBACK (vpn_toggle_connection_added_cb), self);
      g_signal_connect (self->client, "connection-removed",
                        G_CALLBACK (vpn_toggle_connection_removed_cb), self);
      g_signal_connect (self->client, "notify::active-connections",
                        G_CALLBACK (vpn_toggle_notify_active_connections_cb), self);
    }
  else
    {
      g_signal_connect (self->client, "device-added",
                        G_CALLBACK (device_toggle_device_added_cb), self);
      g_signal_connect (self->client, "device-removed",
                        G_CALLBACK (device_toggle_device_removed_cb), self);
    }

  if (self->kind == TOGGLE_KIND_WIRELESS)
    {
      g_object_bind_property (self->client, "wireless-enabled",
                              self, "checked", G_BINDING_SYNC_CREATE);
      g_object_bind_property (self->client, "wireless-hardware-enabled",
                              self, "reactive", G_BINDING_SYNC_CREATE);
    }
}

static void
toggle_shutdown_cb (ShellGlobal   *global,
                    ShellNmToggle *self)
{
  g_cancellable_cancel (self->cancellable);
  wireless_toggle_stop_scanning (self);
}

static void
shell_nm_toggle_dispose (GObject *object)
{
  ShellNmToggle *self = SHELL_NM_TOGGLE (object);
  guint i;

  g_cancellable_cancel (self->cancellable);
  wireless_toggle_stop_scanning (self);

  if (self->client != NULL)
    g_signal_handlers_disconnect_by_data (self->client, self);

  for (i = 0; i < self->entries->len; i++)
    {
      ToggleEntry *entry = g_ptr_array_index (self->entries, i);

      g_signal_handlers_disconnect_by_data (entry->item, self);
      g_signal_handlers_disconnect_by_data (entry->key, self);
    }
  g_ptr_array_set_size (self->entries, 0);
  item_sorter_clear (self->item_sorter);

  for (i = 0; i < self->nm_devices->len; i++)
    g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->nm_devices, i), self);
  g_ptr_array_set_size (self->nm_devices, 0);
  g_hash_table_remove_all (self->device_names);

  g_clear_object (&self->item_binding);
  g_clear_weak_pointer (&self->items_section);
  g_clear_weak_pointer (&self->scanning_spinner);
  g_clear_weak_pointer (&self->wwan_settings);
  g_clear_weak_pointer (&self->legacy_settings);
  g_clear_object (&self->client);

  G_OBJECT_CLASS (shell_nm_toggle_parent_class)->dispose (object);
}

static void
shell_nm_toggle_finalize (GObject *object)
{
  ShellNmToggle *self = SHELL_NM_TOGGLE (object);

  g_clear_pointer (&self->entries, g_ptr_array_unref);
  g_clear_pointer (&self->item_sorter, item_sorter_free);
  g_clear_pointer (&self->nm_devices, g_ptr_array_unref);
  g_clear_pointer (&self->device_names, g_hash_table_unref);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (shell_nm_toggle_parent_class)->finalize (object);
}

static void
shell_nm_toggle_class_init (ShellNmToggleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_nm_toggle_dispose;
  object_class->finalize = shell_nm_toggle_finalize;

  toggle_signals[TOGGLE_ACTIVATION_FAILED] =
    g_signal_new ("activation-failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_nm_toggle_init (ShellNmToggle *self)
{
  self->entries = g_ptr_array_new_with_free_func ((GDestroyNotify) toggle_entry_free);
  self->item_sorter = item_sorter_new (compare_items_by_name, TRUE);
  self->nm_devices = g_ptr_array_new_with_free_func (g_object_unref);
  self->device_names = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                              g_object_unref, g_free);
  self->cancellable = g_cancellable_new ();
}

static ShellNmToggle *
shell_nm_toggle_new (ToggleKind kind)
{
  ShellNmToggle *self = g_object_new (SHELL_TYPE_NM_TOGGLE, NULL);
  ShellPopupMenuBase *menu = toggle_get_menu (self);
  ShellQuickToggleMenu *toggle_menu = SHELL_QUICK_TOGGLE_MENU (menu);
  g_autoptr (ShellPopupMenuSection) items_section = shell_popup_menu_section_new ();

  self->kind = kind;

  switch (kind)
    {
    case TOGGLE_KIND_WIRED:
      self->device_type = NM_DEVICE_TYPE_ETHERNET;
      break;

    case TOGGLE_KIND_WIRELESS:
      self->device_type = NM_DEVICE_TYPE_WIFI;
      break;

    case TOGGLE_KIND_MODEM:
      self->device_type = NM_DEVICE_TYPE_MODEM;
      break;

    case TOGGLE_KIND_BLUETOOTH:
      self->device_type = NM_DEVICE_TYPE_BT;
      break;

    case TOGGLE_KIND_VPN:
    default:
      break;
    }

  g_set_weak_pointer (&self->items_section, items_section);
  shell_popup_menu_base_add_menu_item (menu, G_OBJECT (items_section));

  shell_popup_menu_base_add_menu_item (menu, g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL));

  self->item_binding = g_binding_group_new ();
  g_binding_group_bind (self->item_binding, "icon-name",
                        self, "icon-name", G_BINDING_DEFAULT);
  g_object_bind_property_full (self->item_binding, "source",
                               self, "title", G_BINDING_DEFAULT,
                               toggle_title_transform_cb, NULL, self, NULL);
  g_binding_group_bind_full (self->item_binding, "item-name",
                             self, "subtitle", G_BINDING_DEFAULT,
                             toggle_subtitle_transform_cb, NULL, self, NULL);

  g_signal_connect (self, "clicked", G_CALLBACK (toggle_clicked_cb), self);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (toggle_shutdown_cb), self, 0);

  switch (kind)
    {
    case TOGGLE_KIND_VPN:
      shell_quick_toggle_menu_set_header (toggle_menu, "network-vpn-symbolic", _("VPN"), NULL);
      g_object_set (self, "menu-button-accessible-name", _("Open VPN menu"), NULL);
      shell_popup_menu_base_add_settings_action (menu, _("VPN Settings"),
                                                 "gnome-network-panel.desktop");
      break;

    case TOGGLE_KIND_WIRELESS:
      g_binding_group_bind (self->item_binding, "is-hotspot",
                            self, "menu-enabled", G_BINDING_INVERT_BOOLEAN);

      {
        ShellSpinner *spinner = shell_spinner_new (16, FALSE, FALSE);

        g_set_weak_pointer (&self->scanning_spinner, spinner);

        g_signal_connect_object (menu, "open-state-changed",
                                 G_CALLBACK (wireless_toggle_menu_open_state_changed_cb),
                                 self, 0);

        shell_quick_toggle_menu_set_header (toggle_menu, "network-wireless-symbolic", _("Wi–Fi"), NULL);
        g_object_set (self, "menu-button-accessible-name", _("Open Wi–Fi menu"), NULL);
        shell_quick_toggle_menu_add_header_suffix (toggle_menu, CLUTTER_ACTOR (spinner));
        shell_popup_menu_base_add_settings_action (menu, _("All Networks"),
                                                   "gnome-wifi-panel.desktop");
      }
      break;

    case TOGGLE_KIND_WIRED:
      shell_quick_toggle_menu_set_header (toggle_menu, "network-wired-symbolic",
                                          _("Wired Connections"), NULL);
      g_object_set (self, "menu-button-accessible-name", _("Open wired connections menu"), NULL);
      shell_popup_menu_base_add_settings_action (menu, _("Wired Settings"),
                                                 "gnome-network-panel.desktop");
      break;

    case TOGGLE_KIND_BLUETOOTH:
      shell_quick_toggle_menu_set_header (toggle_menu, "network-cellular-symbolic",
                                          _("Bluetooth Tethers"), NULL);
      g_object_set (self, "menu-button-accessible-name", _("Open Bluetooth tethers menu"), NULL);
      shell_popup_menu_base_add_settings_action (menu, _("Bluetooth Settings"),
                                                 "gnome-network-panel.desktop");
      break;

    case TOGGLE_KIND_MODEM:
      {
        const char *settings_label = _("Mobile Broadband Settings");
        ShellPopupBaseMenuItem *wwan_settings, *legacy_settings;

        shell_quick_toggle_menu_set_header (toggle_menu, "network-cellular-symbolic",
                                            _("Mobile Connections"), NULL);
        g_object_set (self, "menu-button-accessible-name", _("Open mobile connections menu"), NULL);

        wwan_settings = shell_popup_menu_base_add_settings_action (menu, settings_label,
                                                                   "gnome-wwan-panel.desktop");
        g_set_weak_pointer (&self->wwan_settings, wwan_settings);
        legacy_settings = shell_popup_menu_base_add_settings_action (menu, settings_label,
                                                                     "gnome-network-panel.desktop");
        g_set_weak_pointer (&self->legacy_settings, legacy_settings);
      }
      break;

    default:
      g_assert_not_reached ();
    }

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellNmCaptivePortalHandler                                              */
/* ------------------------------------------------------------------------ */

/* Notifies the user that the network they are connected to asks for a login,
 * and helps them with it */

#define SHELL_TYPE_NM_CAPTIVE_PORTAL_HANDLER (shell_nm_captive_portal_handler_get_type ())
G_DECLARE_FINAL_TYPE (ShellNmCaptivePortalHandler, shell_nm_captive_portal_handler,
                      SHELL, NM_CAPTIVE_PORTAL_HANDLER, GObject)

struct _ShellNmCaptivePortalHandler
{
  GObject parent;

  char *check_uri;
  GHashTable *connectivity_queue;   /* path -> path */
  GHashTable *notifications;        /* path -> ShellNotification */

#ifdef HAVE_PORTAL_HELPER
  ShellPortalHelper *portal_helper_proxy;
  gboolean creating_proxy;
  GList *pending_authentications;   /* PendingAuthentication */
  GCancellable *cancellable;
#endif
};

enum
{
  CAPTIVE_PORTAL_HANDLER_RECHECK,
  CAPTIVE_PORTAL_HANDLER_OVERVIEW_HIDE_REQUESTED,

  N_CAPTIVE_PORTAL_HANDLER_SIGNALS
};

static guint captive_portal_handler_signals[N_CAPTIVE_PORTAL_HANDLER_SIGNALS] = { 0, };

G_DEFINE_FINAL_TYPE (ShellNmCaptivePortalHandler, shell_nm_captive_portal_handler, G_TYPE_OBJECT)

#define NOTIFICATION_PATH_KEY "shell-nm-captive-portal-path"

#ifdef HAVE_PORTAL_HELPER

typedef struct
{
  char *path;
  guint32 timestamp;
} PendingAuthentication;

static void
pending_authentication_free (PendingAuthentication *pending)
{
  g_free (pending->path);
  g_free (pending);
}

/* _portalHelperStatusChanged () */
static void shell_nm_captive_portal_handler_remove_connection (ShellNmCaptivePortalHandler *self,
                                                               const char                  *path);

static void
portal_helper_status_changed_cb (ShellPortalHelper            *proxy,
                                 const char                   *path,
                                 guint                         result,
                                 ShellNmCaptivePortalHandler  *self)
{
  switch (result)
    {
    case PORTAL_HELPER_RESULT_CANCELLED:
      /* Keep the connection in the queue, so the user is not
       * spammed with more logins until we next flush the queue,
       * which will happen once they choose a better connection
       * or we get to full connectivity through other means */
      break;

    case PORTAL_HELPER_RESULT_COMPLETED:
      shell_nm_captive_portal_handler_remove_connection (self, path);
      break;

    case PORTAL_HELPER_RESULT_RECHECK:
      g_signal_emit (self, captive_portal_handler_signals[CAPTIVE_PORTAL_HANDLER_RECHECK], 0, path);
      break;

    default:
      g_message ("Invalid result from portal helper: %u", result);
      break;
    }
}

static void
portal_helper_authenticate_cb (GObject      *source_object,
                               GAsyncResult *res,
                               gpointer      user_data)
{
  g_autoptr (GError) error = NULL;

  if (!shell_portal_helper_call_authenticate_finish (SHELL_PORTAL_HELPER (source_object), res, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("Failed to authenticate with the portal helper: %s", error->message);
}

static void
portal_helper_authenticate (ShellNmCaptivePortalHandler *self,
                            const char                  *path,
                            guint32                      timestamp)
{
  shell_portal_helper_call_authenticate (self->portal_helper_proxy, path,
                                         self->check_uri, timestamp,
                                         self->cancellable,
                                         portal_helper_authenticate_cb, NULL);
}

static void
portal_helper_proxy_ready_cb (GObject      *source_object,
                              GAsyncResult *res,
                              gpointer      user_data)
{
  g_autoptr (ShellNmCaptivePortalHandler) self = user_data;
  g_autoptr (GError) error = NULL;
  ShellPortalHelper *proxy;
  GList *pending;
  GList *l;

  proxy = shell_portal_helper_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  self->creating_proxy = FALSE;
  pending = g_steal_pointer (&self->pending_authentications);

  if (proxy != NULL)
    {
      self->portal_helper_proxy = proxy;
      g_signal_connect (proxy, "status-changed",
                        G_CALLBACK (portal_helper_status_changed_cb), self);
    }
  else
    {
      g_warning ("Error launching the portal helper: %s", error->message);
    }

  for (l = pending; l != NULL; l = l->next)
    {
      PendingAuthentication *authentication = l->data;

      if (self->portal_helper_proxy != NULL)
        portal_helper_authenticate (self, authentication->path, authentication->timestamp);
    }

  g_list_free_full (pending, (GDestroyNotify) pending_authentication_free);
}

/* _launchPortalHelper () */
static void
shell_nm_captive_portal_handler_launch_portal_helper (ShellNmCaptivePortalHandler *self,
                                                      const char                  *path,
                                                      guint32                      timestamp)
{
  if (self->portal_helper_proxy != NULL)
    {
      portal_helper_authenticate (self, path, timestamp);
    }
  else
    {
      PendingAuthentication *pending = g_new0 (PendingAuthentication, 1);

      pending->path = g_strdup (path);
      pending->timestamp = timestamp;
      self->pending_authentications = g_list_append (self->pending_authentications, pending);

      if (!self->creating_proxy)
        {
          self->creating_proxy = TRUE;
          shell_portal_helper_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 "org.gnome.Shell.PortalHelper",
                                                 "/org/gnome/Shell/PortalHelper",
                                                 self->cancellable,
                                                 portal_helper_proxy_ready_cb,
                                                 g_object_ref (self));
        }
    }

  g_hash_table_add (self->connectivity_queue, g_strdup (path));
}

#endif /* HAVE_PORTAL_HELPER */

/* _onNotificationActivated () */
static void
captive_portal_handler_notification_activated_cb (ShellNotification           *notification,
                                                  ShellNmCaptivePortalHandler *self)
{
  ShellGlobal *global = shell_global_get ();
  guint32 timestamp = shell_global_get_current_time (global);
  g_autoptr (GAppLaunchContext) context = NULL;
  ShellPanel *panel;

  context = shell_global_create_app_launch_context (global, timestamp, -1);

#ifdef HAVE_PORTAL_HELPER
  shell_nm_captive_portal_handler_launch_portal_helper (self,
                                                        g_object_get_data (G_OBJECT (notification),
                                                                           NOTIFICATION_PATH_KEY),
                                                        timestamp);
#else
  {
    g_autoptr (GError) error = NULL;

    if (!g_app_info_launch_default_for_uri (self->check_uri, context, &error))
      g_warning ("Failed to open %s: %s", self->check_uri, error->message);
  }
#endif

  /* Main.overview.hide (); Main.panel.close_calendar () */
  g_signal_emit (self, captive_portal_handler_signals[CAPTIVE_PORTAL_HANDLER_OVERVIEW_HIDE_REQUESTED], 0);

  panel = shell_main_get_panel (shell_main_get_default ());
  if (panel != NULL)
    shell_panel_close_calendar (panel);
}

static void
captive_portal_handler_notification_destroy_cb (ShellNotification           *notification,
                                                ShellNotificationDestroyedReason reason,
                                                ShellNmCaptivePortalHandler *self)
{
  const char *path = g_object_get_data (G_OBJECT (notification), NOTIFICATION_PATH_KEY);

  /* Only the notification of the path: another one can have replaced it */
  if (path != NULL && g_hash_table_lookup (self->notifications, path) == notification)
    g_hash_table_remove (self->notifications, path);
}

/* addConnection () */
static void
shell_nm_captive_portal_handler_add_connection (ShellNmCaptivePortalHandler *self,
                                                const char                  *name,
                                                const char                  *path)
{
  ShellMessageTray *message_tray = shell_main_get_message_tray (shell_main_get_default ());
  ShellSource *source;
  g_autoptr (ShellNotification) notification = NULL;

  if (g_hash_table_contains (self->connectivity_queue, path) ||
      g_hash_table_contains (self->notifications, path))
    return;

  if (message_tray == NULL)
    return;

  source = shell_message_tray_get_system_source (message_tray);
  if (source == NULL)
    return;

  notification = shell_notification_new (source, _("Sign in to Network"), name);
  g_object_set_data_full (G_OBJECT (notification), NOTIFICATION_PATH_KEY,
                          g_strdup (path), g_free);
  g_signal_connect (notification, "activated",
                    G_CALLBACK (captive_portal_handler_notification_activated_cb), self);
  g_signal_connect (notification, "destroy",
                    G_CALLBACK (captive_portal_handler_notification_destroy_cb), self);
  g_hash_table_insert (self->notifications, g_strdup (path), g_object_ref (notification));
  shell_source_add_notification (source, notification);
}

/* removeConnection () */
static void
shell_nm_captive_portal_handler_remove_connection (ShellNmCaptivePortalHandler *self,
                                                   const char                  *path)
{
  g_autofree char *path_copy = g_strdup (path);
  g_autoptr (ShellNotification) notification = NULL;
  gpointer value;

#ifdef HAVE_PORTAL_HELPER
  if (g_hash_table_remove (self->connectivity_queue, path_copy))
    {
      if (self->portal_helper_proxy != NULL)
        shell_portal_helper_call_close (self->portal_helper_proxy, path_copy,
                                        self->cancellable, NULL, NULL);
    }
#else
  g_hash_table_remove (self->connectivity_queue, path_copy);
#endif

  value = g_hash_table_lookup (self->notifications, path_copy);
  if (value != NULL)
    notification = g_object_ref (value);

  if (notification != NULL)
    shell_notification_destroy (notification, SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED);
  g_hash_table_remove (self->notifications, path_copy);
}

/* clear () */
static void
shell_nm_captive_portal_handler_clear (ShellNmCaptivePortalHandler *self)
{
  g_autoptr (GPtrArray) notifications = g_ptr_array_new_with_free_func (g_object_unref);
  GHashTableIter iter;
  gpointer value;
  guint i;

#ifdef HAVE_PORTAL_HELPER
  if (self->portal_helper_proxy != NULL)
    {
      gpointer key;

      g_hash_table_iter_init (&iter, self->connectivity_queue);
      while (g_hash_table_iter_next (&iter, &key, NULL))
        shell_portal_helper_call_close (self->portal_helper_proxy, key,
                                        self->cancellable, NULL, NULL);
    }
#endif
  g_hash_table_remove_all (self->connectivity_queue);

  g_hash_table_iter_init (&iter, self->notifications);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    g_ptr_array_add (notifications, g_object_ref (value));

  for (i = 0; i < notifications->len; i++)
    shell_notification_destroy (g_ptr_array_index (notifications, i),
                                SHELL_NOTIFICATION_DESTROYED_REASON_SOURCE_CLOSED);
  g_hash_table_remove_all (self->notifications);
}

static void
captive_portal_handler_shutdown_cb (ShellGlobal                 *global,
                                    ShellNmCaptivePortalHandler *self)
{
#ifdef HAVE_PORTAL_HELPER
  g_cancellable_cancel (self->cancellable);
#endif
}

static void
shell_nm_captive_portal_handler_dispose (GObject *object)
{
  ShellNmCaptivePortalHandler *self = SHELL_NM_CAPTIVE_PORTAL_HANDLER (object);

#ifdef HAVE_PORTAL_HELPER
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->portal_helper_proxy);
  g_list_free_full (g_steal_pointer (&self->pending_authentications),
                    (GDestroyNotify) pending_authentication_free);
#endif

  /* The notifications are not destroyed: they stay for the user */
  if (self->notifications != NULL)
    {
      GHashTableIter iter;
      gpointer value;

      g_hash_table_iter_init (&iter, self->notifications);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        g_signal_handlers_disconnect_by_data (value, self);
      g_hash_table_remove_all (self->notifications);
    }

  G_OBJECT_CLASS (shell_nm_captive_portal_handler_parent_class)->dispose (object);
}

static void
shell_nm_captive_portal_handler_finalize (GObject *object)
{
  ShellNmCaptivePortalHandler *self = SHELL_NM_CAPTIVE_PORTAL_HANDLER (object);

  g_clear_pointer (&self->check_uri, g_free);
  g_clear_pointer (&self->connectivity_queue, g_hash_table_unref);
  g_clear_pointer (&self->notifications, g_hash_table_unref);
#ifdef HAVE_PORTAL_HELPER
  g_clear_object (&self->cancellable);
#endif

  G_OBJECT_CLASS (shell_nm_captive_portal_handler_parent_class)->finalize (object);
}

static void
shell_nm_captive_portal_handler_class_init (ShellNmCaptivePortalHandlerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_nm_captive_portal_handler_dispose;
  object_class->finalize = shell_nm_captive_portal_handler_finalize;

  captive_portal_handler_signals[CAPTIVE_PORTAL_HANDLER_RECHECK] =
    g_signal_new ("recheck",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_STRING);

  captive_portal_handler_signals[CAPTIVE_PORTAL_HANDLER_OVERVIEW_HIDE_REQUESTED] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_nm_captive_portal_handler_init (ShellNmCaptivePortalHandler *self)
{
  self->connectivity_queue = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->notifications = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
#ifdef HAVE_PORTAL_HELPER
  self->cancellable = g_cancellable_new ();
#endif
}

static ShellNmCaptivePortalHandler *
shell_nm_captive_portal_handler_new (const char *check_uri)
{
  ShellNmCaptivePortalHandler *self = g_object_new (SHELL_TYPE_NM_CAPTIVE_PORTAL_HANDLER, NULL);

  self->check_uri = g_strdup (check_uri);
  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (captive_portal_handler_shutdown_cb), self, 0);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellNetworkIndicator                                                    */
/* ------------------------------------------------------------------------ */

struct _ShellNetworkIndicator
{
  ShellSystemIndicator parent;

  ShellNmCaptivePortalHandler *portal_handler;
  NMActiveConnection *main_connection;
  ShellNotification *notification;      /* weak */
  NMClient *client;

  ShellNmToggle *wired_toggle;          /* weak */
  ShellNmToggle *wireless_toggle;       /* weak */
  ShellNmToggle *modem_toggle;          /* weak */
  ShellNmToggle *bt_toggle;             /* weak */
  ShellNmToggle *vpn_toggle;            /* weak */

  StIcon *primary_indicator;            /* weak */
  StIcon *vpn_indicator;                /* weak */
  GBindingGroup *primary_indicator_binding;

  GPermission *config_permission;
  GCancellable *cancellable;
};

enum
{
  INDICATOR_OVERVIEW_HIDE_REQUESTED,

  N_INDICATOR_SIGNALS
};

static guint indicator_signals[N_INDICATOR_SIGNALS] = { 0, };

G_DEFINE_FINAL_TYPE (ShellNetworkIndicator, shell_network_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
indicator_destroy_notification (ShellNetworkIndicator *self)
{
  g_autoptr (ShellNotification) notification = NULL;

  if (self->notification == NULL)
    return;

  notification = g_object_ref (self->notification);
  shell_notification_destroy (notification, SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
}

/* _onActivationFailed () */
static void
indicator_activation_failed_cb (ShellNmToggle         *toggle,
                                ShellNetworkIndicator *self)
{
  ShellMessageTray *message_tray = shell_main_get_message_tray (shell_main_get_default ());
  ShellSource *source;
  g_autoptr (ShellNotification) notification = NULL;

  indicator_destroy_notification (self);

  if (message_tray == NULL)
    return;

  source = shell_message_tray_get_system_source (message_tray);
  if (source == NULL)
    return;

  notification = shell_notification_new (source,
                                         _("Connection failed"),
                                         _("Activation of network connection failed"));
  shell_notification_set_icon_name (notification, "network-error-symbolic");
  shell_notification_set_urgency (notification, SHELL_NOTIFICATION_URGENCY_HIGH);
  shell_notification_set_is_transient (notification, TRUE);
  g_set_weak_pointer (&self->notification, notification);

  shell_source_add_notification (source, notification);
}

/* _mainConnectionStateChanged () */
static void
indicator_main_connection_state_changed (ShellNetworkIndicator *self)
{
  if (nm_active_connection_get_state (self->main_connection) == NM_ACTIVE_CONNECTION_STATE_ACTIVATED)
    indicator_destroy_notification (self);
}

static void
indicator_main_connection_notify_state_cb (NMActiveConnection    *connection,
                                           GParamSpec            *pspec,
                                           ShellNetworkIndicator *self)
{
  indicator_main_connection_state_changed (self);
}

static ShellNmToggle *
indicator_get_device_toggle (ShellNetworkIndicator *self,
                             NMDeviceType           device_type)
{
  switch (device_type)
    {
    case NM_DEVICE_TYPE_ETHERNET:
      return self->wired_toggle;

    case NM_DEVICE_TYPE_WIFI:
      return self->wireless_toggle;

    case NM_DEVICE_TYPE_MODEM:
      return self->modem_toggle;

    case NM_DEVICE_TYPE_BT:
      return self->bt_toggle;

    default:
      return NULL;
    }
}

/* _updateIcon () */
static void
indicator_update_icon (ShellNetworkIndicator *self)
{
  const GPtrArray *devices = NULL;
  ShellNmToggle *primary_toggle = NULL;
  gboolean connected;

  if (self->main_connection != NULL)
    devices = nm_active_connection_get_devices (self->main_connection);

  if (devices != NULL && devices->len > 0)
    primary_toggle = indicator_get_device_toggle (self,
                                                  nm_device_get_device_type (g_ptr_array_index (devices, 0)));

  g_binding_group_set_source (self->primary_indicator_binding, primary_toggle);

  if (primary_toggle == NULL && self->primary_indicator != NULL)
    {
      if (nm_client_get_connectivity (self->client) == NM_CONNECTIVITY_FULL)
        st_icon_set_icon_name (self->primary_indicator, "network-wired-symbolic");
      else
        st_icon_set_icon_name (self->primary_indicator, "network-wired-no-route-symbolic");
    }

  connected = nm_client_get_state (self->client) == NM_STATE_CONNECTED_GLOBAL;
  if (self->primary_indicator != NULL)
    set_actor_visible (self->primary_indicator, primary_toggle != NULL || connected);
}

/* _syncConnectivity () */
static void
indicator_sync_connectivity (ShellNetworkIndicator *self)
{
  ShellSessionMode *session_mode = get_session_mode ();
  NMConnectivityState connectivity;
  gboolean is_portal;

  indicator_update_icon (self);

  if (self->main_connection == NULL ||
      nm_active_connection_get_state (self->main_connection) != NM_ACTIVE_CONNECTION_STATE_ACTIVATED)
    {
      shell_nm_captive_portal_handler_clear (self->portal_handler);
      return;
    }

  if (session_mode != NULL && shell_session_mode_is_greeter (session_mode))
    return;

  connectivity = nm_client_get_connectivity (self->client);
  is_portal = connectivity == NM_CONNECTIVITY_PORTAL;

  /* For testing, allow interpreting any value != FULL as PORTAL, because
   * LIMITED (no upstream route after the default gateway) is easy to obtain
   * with a tethered phone
   * NONE is also possible, with a connection configured to force no default route
   * (but in general we should only prompt a portal if we know there is a portal) */
  if (g_getenv ("GNOME_SHELL_CONNECTIVITY_TEST") != NULL)
    is_portal = is_portal || connectivity < NM_CONNECTIVITY_FULL;

  if (is_portal)
    shell_nm_captive_portal_handler_add_connection (self->portal_handler,
                                                    nm_active_connection_get_id (self->main_connection),
                                                    nm_object_get_path (NM_OBJECT (self->main_connection)));
  else
    shell_nm_captive_portal_handler_clear (self->portal_handler);
}

/* _syncMainConnection () */
static void
indicator_sync_main_connection (ShellNetworkIndicator *self)
{
  NMActiveConnection *main_connection;

  if (self->main_connection != NULL)
    g_signal_handlers_disconnect_by_data (self->main_connection, self);

  main_connection = nm_client_get_primary_connection (self->client);
  if (main_connection == NULL)
    main_connection = nm_client_get_activating_connection (self->client);
  g_set_object (&self->main_connection, main_connection);

  if (self->main_connection != NULL)
    {
      g_signal_connect (self->main_connection, "notify::state",
                        G_CALLBACK (indicator_main_connection_notify_state_cb), self);
      indicator_main_connection_state_changed (self);
    }

  indicator_sync_connectivity (self);
}

static void
indicator_sync_main_connection_cb (NMClient              *client,
                                   GParamSpec            *pspec,
                                   ShellNetworkIndicator *self)
{
  indicator_sync_main_connection (self);
}

static void
indicator_sync_connectivity_cb (NMClient              *client,
                                GParamSpec            *pspec,
                                ShellNetworkIndicator *self)
{
  indicator_sync_connectivity (self);
}

typedef struct
{
  GWeakRef indicator;
  char *path;
} RecheckData;

static void
recheck_data_free (RecheckData *data)
{
  g_weak_ref_clear (&data->indicator);
  g_free (data->path);
  g_free (data);
}

static void
indicator_check_connectivity_cb (GObject      *source_object,
                                 GAsyncResult *res,
                                 gpointer      user_data)
{
  RecheckData *data = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (ShellNetworkIndicator) self = NULL;
  NMConnectivityState state;

  state = nm_client_check_connectivity_finish (NM_CLIENT (source_object), res, &error);
  if (error != NULL)
    {
      recheck_data_free (data);
      return;
    }

  self = g_weak_ref_get (&data->indicator);
  if (self != NULL && self->portal_handler != NULL && state >= NM_CONNECTIVITY_FULL)
    shell_nm_captive_portal_handler_remove_connection (self->portal_handler, data->path);

  recheck_data_free (data);
}

static void
indicator_portal_recheck_cb (ShellNmCaptivePortalHandler *handler,
                             const char                  *path,
                             ShellNetworkIndicator       *self)
{
  RecheckData *data = g_new0 (RecheckData, 1);

  g_weak_ref_init (&data->indicator, self);
  data->path = g_strdup (path);

  nm_client_check_connectivity_async (self->client, self->cancellable,
                                      indicator_check_connectivity_cb, data);
}

static void
indicator_portal_overview_hide_requested_cb (ShellNmCaptivePortalHandler *handler,
                                             ShellNetworkIndicator       *self)
{
  g_signal_emit (self, indicator_signals[INDICATOR_OVERVIEW_HIDE_REQUESTED], 0);
}

static void
indicator_permission_ready_cb (GObject      *source_object,
                               GAsyncResult *res,
                               gpointer      user_data)
{
  g_autoptr (ShellNetworkIndicator) self = user_data;
  g_autoptr (GError) error = NULL;
  GPermission *permission;
  GPtrArray *toggles;
  guint i;

  permission = G_PERMISSION (polkit_permission_new_finish (res, &error));
  if (permission == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("No permission to control network connections: %s", error->message);
      return;
    }

  if (g_cancellable_is_cancelled (self->cancellable))
    {
      g_object_unref (permission);
      return;
    }

  self->config_permission = permission;

  toggles = shell_system_indicator_get_quick_settings_items (SHELL_SYSTEM_INDICATOR (self));
  for (i = 0; i < toggles->len; i++)
    g_object_bind_property (permission, "allowed",
                            g_ptr_array_index (toggles, i), "reactive",
                            G_BINDING_SYNC_CREATE);
}

static void
indicator_client_ready_cb (GObject      *source_object,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  g_autoptr (ShellNetworkIndicator) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (NMClient) client = NULL;
  GPtrArray *toggles;
  guint i;

  client = nm_client_new_finish (res, &error);
  if (client == NULL)
    {
      /* NetworkManager is not required to run, nor is the system bus */
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_debug ("Failed to create the NetworkManager client: %s", error->message);

      /* Without a client the toggles have nothing to show: hide them
       * rather than leave them visible with no title */
      toggles = shell_system_indicator_get_quick_settings_items (SHELL_SYSTEM_INDICATOR (self));
      for (i = 0; i < toggles->len; i++)
        shell_nm_toggle_sync (g_ptr_array_index (toggles, i));
      return;
    }

  if (g_cancellable_is_cancelled (self->cancellable))
    return;

  self->client = g_steal_pointer (&client);

  toggles = shell_system_indicator_get_quick_settings_items (SHELL_SYSTEM_INDICATOR (self));
  for (i = 0; i < toggles->len; i++)
    shell_nm_toggle_set_client (g_ptr_array_index (toggles, i), self->client);

  g_object_bind_property (self->client, "nm-running", self, "visible", G_BINDING_SYNC_CREATE);

  self->portal_handler =
    shell_nm_captive_portal_handler_new (nm_client_connectivity_check_get_uri (self->client));
  g_signal_connect (self->portal_handler, "recheck",
                    G_CALLBACK (indicator_portal_recheck_cb), self);
  g_signal_connect (self->portal_handler, "overview-hide-requested",
                    G_CALLBACK (indicator_portal_overview_hide_requested_cb), self);

  g_signal_connect (self->client, "notify::primary-connection",
                    G_CALLBACK (indicator_sync_main_connection_cb), self);
  g_signal_connect (self->client, "notify::activating-connection",
                    G_CALLBACK (indicator_sync_main_connection_cb), self);
  g_signal_connect (self->client, "notify::connectivity",
                    G_CALLBACK (indicator_sync_connectivity_cb), self);
  indicator_sync_main_connection (self);

  polkit_permission_new (POLKIT_NETWORK_CONTROL, NULL, self->cancellable,
                         indicator_permission_ready_cb, g_object_ref (self));
}

static void
indicator_shutdown_cb (ShellGlobal           *global,
                       ShellNetworkIndicator *self)
{
  g_cancellable_cancel (self->cancellable);
}

static void
shell_network_indicator_constructed (GObject *object)
{
  ShellNetworkIndicator *self = SHELL_NETWORK_INDICATOR (object);
  ShellSystemIndicator *indicator = SHELL_SYSTEM_INDICATOR (self);
  ShellNmToggle *wired_toggle, *wireless_toggle, *modem_toggle, *bt_toggle, *vpn_toggle;
  GPtrArray *toggles;
  StIcon *primary_indicator, *vpn_indicator;
  guint i;

  G_OBJECT_CLASS (shell_network_indicator_parent_class)->constructed (object);

  wired_toggle = shell_nm_toggle_new (TOGGLE_KIND_WIRED);
  g_set_weak_pointer (&self->wired_toggle, wired_toggle);
  wireless_toggle = shell_nm_toggle_new (TOGGLE_KIND_WIRELESS);
  g_set_weak_pointer (&self->wireless_toggle, wireless_toggle);
  modem_toggle = shell_nm_toggle_new (TOGGLE_KIND_MODEM);
  g_set_weak_pointer (&self->modem_toggle, modem_toggle);
  bt_toggle = shell_nm_toggle_new (TOGGLE_KIND_BLUETOOTH);
  g_set_weak_pointer (&self->bt_toggle, bt_toggle);
  vpn_toggle = shell_nm_toggle_new (TOGGLE_KIND_VPN);
  g_set_weak_pointer (&self->vpn_toggle, vpn_toggle);

  shell_system_indicator_add_quick_settings_item (indicator, CLUTTER_ACTOR (wired_toggle));
  shell_system_indicator_add_quick_settings_item (indicator, CLUTTER_ACTOR (wireless_toggle));
  shell_system_indicator_add_quick_settings_item (indicator, CLUTTER_ACTOR (modem_toggle));
  shell_system_indicator_add_quick_settings_item (indicator, CLUTTER_ACTOR (bt_toggle));
  shell_system_indicator_add_quick_settings_item (indicator, CLUTTER_ACTOR (vpn_toggle));

  toggles = shell_system_indicator_get_quick_settings_items (indicator);
  for (i = 0; i < toggles->len; i++)
    g_signal_connect (g_ptr_array_index (toggles, i), "activation-failed",
                      G_CALLBACK (indicator_activation_failed_cb), self);

  primary_indicator = shell_system_indicator_add_indicator (indicator);
  g_set_weak_pointer (&self->primary_indicator, primary_indicator);
  vpn_indicator = shell_system_indicator_add_indicator (indicator);
  g_set_weak_pointer (&self->vpn_indicator, vpn_indicator);

  self->primary_indicator_binding = g_binding_group_new ();
  g_binding_group_bind (self->primary_indicator_binding, "icon-name",
                        primary_indicator, "icon-name", G_BINDING_DEFAULT);

  g_object_bind_property (vpn_toggle, "checked", vpn_indicator, "visible", G_BINDING_SYNC_CREATE);
  g_object_bind_property (vpn_toggle, "icon-name", vpn_indicator, "icon-name", G_BINDING_SYNC_CREATE);

  self->cancellable = g_cancellable_new ();
  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (indicator_shutdown_cb), self, 0);

  /* _getClient () */
  nm_client_new_async (self->cancellable, indicator_client_ready_cb, g_object_ref (self));
}

static void
shell_network_indicator_dispose (GObject *object)
{
  ShellNetworkIndicator *self = SHELL_NETWORK_INDICATOR (object);
  GPtrArray *toggles;
  guint i;

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  toggles = shell_system_indicator_get_quick_settings_items (SHELL_SYSTEM_INDICATOR (self));
  for (i = 0; toggles != NULL && i < toggles->len; i++)
    g_signal_handlers_disconnect_by_data (g_ptr_array_index (toggles, i), self);

  if (self->client != NULL)
    g_signal_handlers_disconnect_by_data (self->client, self);
  if (self->main_connection != NULL)
    g_signal_handlers_disconnect_by_data (self->main_connection, self);
  g_clear_object (&self->main_connection);

  if (self->portal_handler != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->portal_handler, self);
      g_clear_object (&self->portal_handler);
    }

  g_clear_object (&self->primary_indicator_binding);
  g_clear_object (&self->config_permission);
  g_clear_object (&self->client);

  g_clear_weak_pointer (&self->notification);
  g_clear_weak_pointer (&self->wired_toggle);
  g_clear_weak_pointer (&self->wireless_toggle);
  g_clear_weak_pointer (&self->modem_toggle);
  g_clear_weak_pointer (&self->bt_toggle);
  g_clear_weak_pointer (&self->vpn_toggle);
  g_clear_weak_pointer (&self->primary_indicator);
  g_clear_weak_pointer (&self->vpn_indicator);

  G_OBJECT_CLASS (shell_network_indicator_parent_class)->dispose (object);
}

static void
shell_network_indicator_finalize (GObject *object)
{
  ShellNetworkIndicator *self = SHELL_NETWORK_INDICATOR (object);

  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (shell_network_indicator_parent_class)->finalize (object);
}

static void
shell_network_indicator_class_init (ShellNetworkIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_network_indicator_constructed;
  object_class->dispose = shell_network_indicator_dispose;
  object_class->finalize = shell_network_indicator_finalize;

  /**
   * ShellNetworkIndicator::overview-hide-requested:
   * @self: the #ShellNetworkIndicator
   *
   * Emitted when the user started the login of a captive portal from its
   * notification, so that the owner of the indicator hides the overview
   * (`Main.overview.hide ()`).
   */
  indicator_signals[INDICATOR_OVERVIEW_HIDE_REQUESTED] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_network_indicator_init (ShellNetworkIndicator *self)
{
}

/**
 * shell_network_indicator_new:
 *
 * Creates the network indicator of the top bar (`new Indicator ()`): the
 * icons of the primary and of the VPN connection and the quick settings
 * toggles of the network. It gets the NetworkManager client asynchronously;
 * without NetworkManager it stays hidden.
 *
 * Returns: (transfer floating): a new #ShellNetworkIndicator
 */
ShellNetworkIndicator *
shell_network_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_NETWORK_INDICATOR, NULL);
}
