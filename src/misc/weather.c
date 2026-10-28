/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * weather.c: The weather location and forecast shown in the date menu.
 * Port of js/misc/weather.js.
 *
 * The location is either the most recent one the GNOME Weather application
 * saved in org.gnome.shell.weather:locations or, if the user asked for
 * automatic locations and location services are allowed (for the Flatpak
 * version of GNOME Weather also by the portal permission store), the one
 * Geoclue reports.
 *
 * Differences with the JS version:
 *
 *  - #ShellWeatherClient:available, :loading and :has-location are GObject
 *    properties that notify, the JS getters did not;
 *  - the GNOME Weather proxy is a gdbus-codegen proxy, so it is created
 *    without a GDBusInterfaceInfo of its own;
 *  - nothing reaches the callers after ShellGlobal::shutdown.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <gio/gdesktopappinfo.h>
#include <geoclue.h>

#include "misc/permission-store.h"
#include "misc/weather.h"
#include "shell-app.h"
#include "shell-app-system.h"
#include "shell-global.h"

#include "weather-integration-dbus.h"

#define WEATHER_BUS_NAME "org.gnome.Weather"
#define WEATHER_OBJECT_PATH "/org/gnome/Weather"

#define WEATHER_APP_ID "org.gnome.Weather.desktop"

#define CONTACT_INFO "https://gitlab.gnome.org/GNOME/gnome-shell/-/raw/HEAD/gnome-shell.doap"

/* Minimum time between updates to show loading indication */
#define UPDATE_THRESHOLD (10 * G_TIME_SPAN_MINUTE)

enum
{
  PROP_0,

  PROP_AVAILABLE,
  PROP_LOADING,
  PROP_HAS_LOCATION,
  PROP_INFO,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  CHANGED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellWeatherClient
{
  GObject parent;

  GCancellable *cancellable;
  gboolean shutting_down;

  gboolean loading;
  gboolean location_valid;
  GDateTime *last_update;

  gboolean auto_location_requested;
  GWeatherLocation *most_recent_location;

  GClueSimple *gclue_service;
  gboolean gclue_started;
  gboolean gclue_starting;
  gulong gclue_location_changed_id;

  gboolean needs_auth;
  gboolean weather_authorized;
  ShellPortalPermissionStore *perm_store;

  GSettings *location_settings;
  GSettings *settings;

  GWeatherLocation *world;  /* (transfer none) */
  GWeatherInfo *info;
  gulong info_updated_once_id;

  ShellAppSystem *app_system;
  ShellApp *weather_app;
  ShellWeatherIntegration *weather_proxy;
};

G_DEFINE_FINAL_TYPE (ShellWeatherClient, shell_weather_client, G_TYPE_OBJECT)

static void update_auto_location (ShellWeatherClient *self);

static void
emit_changed (ShellWeatherClient *self)
{
  g_signal_emit (self, signals[CHANGED], 0);
}

static void
set_loading (ShellWeatherClient *self,
             gboolean            loading)
{
  if (self->loading == loading)
    return;

  self->loading = loading;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LOADING]);
}

static void
set_location_valid (ShellWeatherClient *self,
                    gboolean            location_valid)
{
  if (self->location_valid == location_valid)
    return;

  self->location_valid = location_valid;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HAS_LOCATION]);
}

static gboolean
use_auto_location (ShellWeatherClient *self)
{
  return self->auto_location_requested &&
         g_settings_get_boolean (self->location_settings, "enabled") &&
         (!self->needs_auth || self->weather_authorized);
}

static gboolean
locations_equal (GWeatherLocation *loc1,
                 GWeatherLocation *loc2)
{
  if (loc1 == loc2)
    return TRUE;

  if (loc1 == NULL || loc2 == NULL)
    return FALSE;

  return gweather_location_equal (loc1, loc2);
}

static void
on_info_updated_once (GWeatherInfo       *info,
                      ShellWeatherClient *self)
{
  g_clear_signal_handler (&self->info_updated_once_id, info);
  set_loading (self, FALSE);
}

static void
on_info_updated (GWeatherInfo       *info,
                 ShellWeatherClient *self)
{
  g_clear_pointer (&self->last_update, g_date_time_unref);
  self->last_update = g_date_time_new_now_local ();
  emit_changed (self);
}

static void
load_info (ShellWeatherClient *self)
{
  if (self->info_updated_once_id == 0)
    {
      self->info_updated_once_id =
        g_signal_connect (self->info, "updated",
                          G_CALLBACK (on_info_updated_once), self);
    }

  set_loading (self, TRUE);
  emit_changed (self);

  gweather_info_update (self->info);
}

static void
set_location (ShellWeatherClient *self,
              GWeatherLocation   *location)
{
  GWeatherLocation *current;

  current = (GWeatherLocation *) gweather_info_get_location (self->info);
  if (locations_equal (current, location))
    return;

  gweather_info_abort (self->info);
  gweather_info_set_location (self->info, location);
  set_location_valid (self, location != NULL);

  if (location != NULL)
    load_info (self);
  else
    emit_changed (self);
}

static void
on_gclue_location_changed (ShellWeatherClient *self)
{
  GClueLocation *geo_location;
  g_autoptr (GWeatherLocation) location = NULL;

  geo_location = gclue_simple_get_location (self->gclue_service);
  if (geo_location == NULL)
    return;

  /* Provide empty name so GWeather sets location name */
  location = gweather_location_new_detached ("", NULL,
                                             gclue_location_get_latitude (geo_location),
                                             gclue_location_get_longitude (geo_location));
  set_location (self, location);
}

static void
update_location_monitoring (ShellWeatherClient *self)
{
  if (use_auto_location (self))
    {
      if (self->gclue_location_changed_id != 0 || self->gclue_service == NULL)
        return;

      self->gclue_location_changed_id =
        g_signal_connect_swapped (self->gclue_service, "notify::location",
                                  G_CALLBACK (on_gclue_location_changed), self);
      on_gclue_location_changed (self);
    }
  else
    {
      if (self->gclue_location_changed_id != 0)
        g_clear_signal_handler (&self->gclue_location_changed_id,
                                self->gclue_service);
    }
}

static void
gclue_simple_ready (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  g_autoptr (ShellWeatherClient) self = user_data;
  g_autoptr (GClueSimple) service = NULL;
  g_autoptr (GError) error = NULL;

  service = gclue_simple_new_finish (res, &error);
  if (self->shutting_down ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  if (service == NULL)
    {
      g_message ("Failed to connect to Geoclue2 service: %s", error->message);
      set_location (self, self->most_recent_location);
      return;
    }

  self->gclue_service = g_steal_pointer (&service);
  self->gclue_started = TRUE;
  gclue_client_set_distance_threshold (gclue_simple_get_client (self->gclue_service),
                                       100);
  update_location_monitoring (self);
}

static void
start_gclue_service (ShellWeatherClient *self)
{
  if (self->gclue_starting)
    return;

  self->gclue_starting = TRUE;

  gclue_simple_new ("org.gnome.Shell", GCLUE_ACCURACY_LEVEL_CITY,
                    self->cancellable,
                    gclue_simple_ready, g_object_ref (self));
}

static void
update_auto_location (ShellWeatherClient *self)
{
  update_location_monitoring (self);

  if (use_auto_location (self))
    start_gclue_service (self);
  else
    set_location (self, self->most_recent_location);
}

static void
on_location_settings_enabled_changed (GSettings          *settings,
                                      const char         *key,
                                      ShellWeatherClient *self)
{
  update_auto_location (self);
}

static void
on_automatic_location_changed (ShellWeatherClient *self)
{
  gboolean use_auto = g_settings_get_boolean (self->settings,
                                              "automatic-location");

  if (self->auto_location_requested == use_auto)
    return;

  self->auto_location_requested = use_auto;

  update_auto_location (self);
}

static void
on_locations_changed (ShellWeatherClient *self)
{
  g_autoptr (GVariant) locations = NULL;
  g_autoptr (GWeatherLocation) most_recent = NULL;

  locations = g_settings_get_value (self->settings, "locations");

  if (self->world != NULL && g_variant_n_children (locations) > 0)
    {
      g_autoptr (GVariant) boxed = g_variant_get_child_value (locations, 0);
      g_autoptr (GVariant) serialized = g_variant_get_variant (boxed);

      most_recent = gweather_location_deserialize (self->world, serialized);
    }

  if (locations_equal (self->most_recent_location, most_recent))
    return;

  g_set_object (&self->most_recent_location, most_recent);

  if (!use_auto_location (self) || !self->gclue_started)
    set_location (self, self->most_recent_location);
}

static void
sync_weather_properties (ShellWeatherClient *self)
{
  g_autofree char *name_owner = NULL;
  GVariant *locations;

  name_owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (self->weather_proxy));
  if (name_owner == NULL)
    return;

  g_settings_set_boolean (self->settings, "automatic-location",
                          shell_weather_integration_get_automatic_location (self->weather_proxy));

  locations = shell_weather_integration_get_locations (self->weather_proxy);
  if (locations != NULL)
    g_settings_set_value (self->settings, "locations", locations);
}

static void
on_weather_properties_changed (GDBusProxy         *proxy,
                               GVariant           *changed,
                               const char * const *invalidated,
                               ShellWeatherClient *self)
{
  sync_weather_properties (self);
}

static void
weather_proxy_ready (GObject      *source,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  g_autoptr (ShellWeatherClient) self = user_data;
  g_autoptr (ShellWeatherIntegration) proxy = NULL;
  g_autoptr (GError) error = NULL;

  proxy = shell_weather_integration_proxy_new_for_bus_finish (res, &error);
  if (self->shutting_down ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  if (proxy == NULL)
    {
      g_message ("Failed to create GNOME Weather proxy: %s", error->message);
      return;
    }

  self->weather_proxy = g_steal_pointer (&proxy);

  g_signal_connect_object (self->weather_proxy, "g-properties-changed",
                           G_CALLBACK (on_weather_properties_changed), self, 0);
  sync_weather_properties (self);
}

static void
set_weather_app (ShellWeatherClient *self,
                 ShellApp           *app)
{
  if (self->weather_app == app)
    return;

  g_set_object (&self->weather_app, app);
}

static void
on_installed_changed (ShellWeatherClient *self)
{
  gboolean had_app = self->weather_app != NULL;
  gboolean have_app;
  gboolean needed_auth;
  GDesktopAppInfo *app_info;

  set_weather_app (self,
                   shell_app_system_lookup_app (self->app_system,
                                                WEATHER_APP_ID));
  have_app = self->weather_app != NULL;

  if (had_app != have_app)
    {
      g_object_notify_by_pspec (G_OBJECT (self), props[PROP_AVAILABLE]);
      emit_changed (self);
    }

  needed_auth = self->needs_auth;
  app_info = self->weather_app != NULL ?
    shell_app_get_app_info (self->weather_app) : NULL;
  self->needs_auth = self->weather_app == NULL ||
                     (app_info != NULL &&
                      g_desktop_app_info_has_key (app_info, "X-Flatpak"));

  if (needed_auth != self->needs_auth)
    update_auto_location (self);
}

static void
handle_perm_store_changed (ShellWeatherClient *self,
                           const char         *table,
                           const char         *id,
                           GVariant           *permissions)
{
  g_auto (GStrv) permission = NULL;
  const char *accuracy;

  if (g_strcmp0 (table, "gnome") != 0 || g_strcmp0 (id, "geolocation") != 0)
    return;

  if (permissions != NULL &&
      g_variant_lookup (permissions, "org.gnome.Weather", "^as", &permission))
    accuracy = permission[0];
  else
    accuracy = "NONE";

  /* An empty permission array is not "NONE" either */
  self->weather_authorized = g_strcmp0 (accuracy, "NONE") != 0;

  update_auto_location (self);
}

static void
on_perm_store_changed (ShellPortalPermissionStore *store,
                       const char                 *table,
                       const char                 *id,
                       gboolean                    deleted,
                       GVariant                   *data,
                       GVariant                   *permissions,
                       ShellWeatherClient         *self)
{
  handle_perm_store_changed (self, table, id, permissions);
}

static void
perm_store_lookup_ready (GObject      *source,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  g_autoptr (ShellWeatherClient) self = user_data;
  g_autoptr (GVariant) permissions = NULL;
  g_autoptr (GVariant) data = NULL;
  g_autoptr (GError) error = NULL;

  if (!shell_portal_permission_store_call_lookup_finish (SHELL_PORTAL_PERMISSION_STORE (source),
                                                         &permissions, &data,
                                                         res, &error))
    {
      if (self->shutting_down ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      g_message ("Error looking up permission: %s", error->message);
    }

  if (self->shutting_down)
    return;

  handle_perm_store_changed (self, "gnome", "geolocation", permissions);
}

static void
perm_store_ready (GObject      *source,
                  GAsyncResult *res,
                  gpointer      user_data)
{
  g_autoptr (ShellWeatherClient) self = user_data;
  g_autoptr (ShellPortalPermissionStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *name_owner = NULL;

  store = shell_permission_store_new_finish (res, &error);
  if (self->shutting_down ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  if (store == NULL)
    {
      g_message ("Failed to connect to permissionStore: %s", error->message);
      return;
    }

  self->perm_store = g_steal_pointer (&store);

  g_signal_connect_object (self->perm_store, "changed",
                           G_CALLBACK (on_perm_store_changed), self, 0);

  name_owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (self->perm_store));
  if (name_owner == NULL)
    {
      /* Failed to auto-start, likely because xdg-desktop-portal
       * isn't installed; don't restrict access to location service */
      self->weather_authorized = TRUE;
      update_auto_location (self);
      return;
    }

  shell_portal_permission_store_call_lookup (self->perm_store,
                                             "gnome", "geolocation",
                                             self->cancellable,
                                             perm_store_lookup_ready,
                                             g_object_ref (self));
}

/**
 * shell_weather_client_get_available:
 * @client: a #ShellWeatherClient
 *
 * Returns: whether the GNOME Weather application is installed
 */
gboolean
shell_weather_client_get_available (ShellWeatherClient *client)
{
  g_return_val_if_fail (SHELL_IS_WEATHER_CLIENT (client), FALSE);

  return client->weather_app != NULL;
}

/**
 * shell_weather_client_get_loading:
 * @client: a #ShellWeatherClient
 *
 * Returns: whether the weather information is being loaded and no recent
 *   enough information is available
 */
gboolean
shell_weather_client_get_loading (ShellWeatherClient *client)
{
  g_return_val_if_fail (SHELL_IS_WEATHER_CLIENT (client), FALSE);

  return client->loading;
}

/**
 * shell_weather_client_get_has_location:
 * @client: a #ShellWeatherClient
 *
 * Returns: whether a location is set
 */
gboolean
shell_weather_client_get_has_location (ShellWeatherClient *client)
{
  g_return_val_if_fail (SHELL_IS_WEATHER_CLIENT (client), FALSE);

  return client->location_valid;
}

/**
 * shell_weather_client_get_info:
 * @client: a #ShellWeatherClient
 *
 * Returns: (transfer none): the #GWeatherInfo for the current location
 */
GWeatherInfo *
shell_weather_client_get_info (ShellWeatherClient *client)
{
  g_return_val_if_fail (SHELL_IS_WEATHER_CLIENT (client), NULL);

  return client->info;
}

/**
 * shell_weather_client_activate_app:
 * @client: a #ShellWeatherClient
 *
 * Launches the GNOME Weather application, if it is installed.
 */
void
shell_weather_client_activate_app (ShellWeatherClient *client)
{
  g_return_if_fail (SHELL_IS_WEATHER_CLIENT (client));

  if (client->weather_app != NULL)
    shell_app_activate (client->weather_app);
}

/**
 * shell_weather_client_update:
 * @client: a #ShellWeatherClient
 *
 * Refreshes the weather information. If the current information is recent
 * enough it is updated in the background, otherwise
 * #ShellWeatherClient:loading is set until the new information arrived.
 */
void
shell_weather_client_update (ShellWeatherClient *client)
{
  g_autoptr (GDateTime) now = NULL;

  g_return_if_fail (SHELL_IS_WEATHER_CLIENT (client));

  if (!client->location_valid)
    return;

  now = g_date_time_new_now_local ();

  /* Update without loading indication if the current info is recent enough */
  if (gweather_info_is_valid (client->info) &&
      g_date_time_difference (now, client->last_update) < UPDATE_THRESHOLD)
    gweather_info_update (client->info);
  else
    load_info (client);
}

static void
on_shutdown (ShellGlobal        *global,
             ShellWeatherClient *self)
{
  /* Pending operations are cancelled and everything that would emit
   * ::changed into JS is disconnected. */
  self->shutting_down = TRUE;

  g_cancellable_cancel (self->cancellable);

  g_signal_handlers_disconnect_by_data (self->info, self);
  self->info_updated_once_id = 0;
  gweather_info_abort (self->info);

  g_signal_handlers_disconnect_by_data (self->location_settings, self);
  g_signal_handlers_disconnect_by_data (self->settings, self);
  g_signal_handlers_disconnect_by_data (self->app_system, self);
  if (self->perm_store != NULL)
    g_signal_handlers_disconnect_by_data (self->perm_store, self);
  if (self->weather_proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->weather_proxy, self);
  if (self->gclue_service != NULL)
    g_signal_handlers_disconnect_by_data (self->gclue_service, self);
  self->gclue_location_changed_id = 0;
}

static void
shell_weather_client_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  ShellWeatherClient *self = SHELL_WEATHER_CLIENT (object);

  switch (prop_id)
    {
    case PROP_AVAILABLE:
      g_value_set_boolean (value, self->weather_app != NULL);
      break;

    case PROP_LOADING:
      g_value_set_boolean (value, self->loading);
      break;

    case PROP_HAS_LOCATION:
      g_value_set_boolean (value, self->location_valid);
      break;

    case PROP_INFO:
      g_value_set_object (value, self->info);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_weather_client_constructed (GObject *object)
{
  ShellWeatherClient *self = SHELL_WEATHER_CLIENT (object);
  GWeatherProvider providers;

  G_OBJECT_CLASS (shell_weather_client_parent_class)->constructed (object);

  self->cancellable = g_cancellable_new ();

  self->loading = FALSE;
  self->location_valid = FALSE;
  self->last_update = g_date_time_new_from_unix_local (0);

  self->needs_auth = TRUE;
  self->weather_authorized = FALSE;
  shell_permission_store_new (self->cancellable, perm_store_ready,
                              g_object_ref (self));

  self->location_settings = g_settings_new ("org.gnome.system.location");
  g_signal_connect_object (self->location_settings, "changed::enabled",
                           G_CALLBACK (on_location_settings_enabled_changed),
                           self, 0);

  self->world = gweather_location_get_world ();

  providers = GWEATHER_PROVIDER_METAR |
              GWEATHER_PROVIDER_MET_NO |
              GWEATHER_PROVIDER_OWM;
  self->info = g_object_new (GWEATHER_TYPE_INFO,
                             "application-id", "org.gnome.Shell",
                             "contact-info", CONTACT_INFO,
                             "enabled-providers", providers,
                             NULL);
  g_signal_connect_data (self->info, "updated",
                         G_CALLBACK (on_info_updated), self, NULL,
                         G_CONNECT_AFTER);

  shell_weather_integration_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                               G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START |
                                               G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES,
                                               WEATHER_BUS_NAME,
                                               WEATHER_OBJECT_PATH,
                                               self->cancellable,
                                               weather_proxy_ready,
                                               g_object_ref (self));

  self->settings = g_settings_new ("org.gnome.shell.weather");
  g_signal_connect_swapped (self->settings, "changed::automatic-location",
                            G_CALLBACK (on_automatic_location_changed), self);
  on_automatic_location_changed (self);
  g_signal_connect_swapped (self->settings, "changed::locations",
                            G_CALLBACK (on_locations_changed), self);
  on_locations_changed (self);

  self->app_system = shell_app_system_get_default ();
  g_signal_connect_swapped (self->app_system, "installed-changed",
                            G_CALLBACK (on_installed_changed), self);
  on_installed_changed (self);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);
}

static void
shell_weather_client_dispose (GObject *object)
{
  ShellWeatherClient *self = SHELL_WEATHER_CLIENT (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  if (self->info != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->info, self);
      gweather_info_abort (self->info);
    }
  self->info_updated_once_id = 0;

  if (self->location_settings != NULL)
    g_signal_handlers_disconnect_by_data (self->location_settings, self);
  if (self->settings != NULL)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  if (self->app_system != NULL)
    g_signal_handlers_disconnect_by_data (self->app_system, self);
  if (self->gclue_service != NULL)
    g_signal_handlers_disconnect_by_data (self->gclue_service, self);
  self->gclue_location_changed_id = 0;
  self->app_system = NULL;

  g_clear_object (&self->gclue_service);
  g_clear_object (&self->perm_store);
  g_clear_object (&self->weather_proxy);
  g_clear_object (&self->weather_app);
  g_clear_object (&self->location_settings);
  g_clear_object (&self->settings);
  g_clear_object (&self->most_recent_location);
  g_clear_object (&self->info);
  self->world = NULL;

  G_OBJECT_CLASS (shell_weather_client_parent_class)->dispose (object);
}

static void
shell_weather_client_finalize (GObject *object)
{
  ShellWeatherClient *self = SHELL_WEATHER_CLIENT (object);

  g_clear_pointer (&self->last_update, g_date_time_unref);

  G_OBJECT_CLASS (shell_weather_client_parent_class)->finalize (object);
}

static void
shell_weather_client_class_init (ShellWeatherClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_weather_client_constructed;
  object_class->get_property = shell_weather_client_get_property;
  object_class->dispose = shell_weather_client_dispose;
  object_class->finalize = shell_weather_client_finalize;

  /**
   * ShellWeatherClient:available:
   *
   * Whether the GNOME Weather application is installed.
   */
  props[PROP_AVAILABLE] =
    g_param_spec_boolean ("available", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellWeatherClient:loading:
   *
   * Whether weather information is being loaded for the current location.
   */
  props[PROP_LOADING] =
    g_param_spec_boolean ("loading", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellWeatherClient:has-location:
   *
   * Whether a location is set.
   */
  props[PROP_HAS_LOCATION] =
    g_param_spec_boolean ("has-location", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellWeatherClient:info:
   *
   * The weather information for the current location.
   */
  props[PROP_INFO] =
    g_param_spec_object ("info", NULL, NULL,
                         GWEATHER_TYPE_INFO,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellWeatherClient::changed:
   * @client: the #ShellWeatherClient
   *
   * Emitted when the location, the loading state, the availability of GNOME
   * Weather or the weather information changed.
   */
  signals[CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_weather_client_init (ShellWeatherClient *self)
{
}
