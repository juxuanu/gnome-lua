/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * night-light.c: The "Night Light" quick settings toggle and its status icon.
 * Port of js/ui/status/nightLight.js.
 *
 * The org.gnome.SettingsDaemon.Color proxy is a gdbus-codegen proxy
 * (`night-light-dbus`, type ShellSettingsDaemonColor). Without the service
 * the icon stays hidden, quietly.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - A missing org.gnome.settings-daemon.plugins.color schema leaves the
 *    toggle unbound (it stays unchecked) instead of throwing.
 *  - After ShellGlobal::shutdown the proxy is dropped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include "shell-global.h"
#include "ui/quick-settings.h"
#include "ui/status/night-light.h"
#include "ui/status/status-private.h"

#include "night-light-dbus.h"

#define COLOR_SCHEMA "org.gnome.settings-daemon.plugins.color"
#define KEY_NIGHT_LIGHT_ENABLED "night-light-enabled"

#define BUS_NAME "org.gnome.SettingsDaemon.Color"
#define OBJECT_PATH "/org/gnome/SettingsDaemon/Color"

/* ------------------------------------------------------------------------ */
/* ShellNightLightToggle                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_NIGHT_LIGHT_TOGGLE (shell_night_light_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellNightLightToggle, shell_night_light_toggle,
                      SHELL, NIGHT_LIGHT_TOGGLE, ShellQuickToggle)

struct _ShellNightLightToggle
{
  ShellQuickToggle parent;
};

G_DEFINE_FINAL_TYPE (ShellNightLightToggle, shell_night_light_toggle,
                     SHELL_TYPE_QUICK_TOGGLE)

static void
shell_night_light_toggle_class_init (ShellNightLightToggleClass *klass)
{
}

static void
shell_night_light_toggle_init (ShellNightLightToggle *self)
{
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (shell_global_get_backend (shell_global_get ()));
  g_autoptr (GSettings) settings = NULL;

  g_object_bind_property (monitor_manager, "night-light-supported",
                          self, "visible",
                          G_BINDING_SYNC_CREATE);

  /* The binding is dropped with the toggle */
  settings = shell_status_settings_new (COLOR_SCHEMA);
  if (settings != NULL)
    g_settings_bind (settings, KEY_NIGHT_LIGHT_ENABLED,
                     self, "checked",
                     G_SETTINGS_BIND_DEFAULT);
}

static ShellNightLightToggle *
shell_night_light_toggle_new (void)
{
  return g_object_new (SHELL_TYPE_NIGHT_LIGHT_TOGGLE,
                       "title", _("Night Light"),
                       "icon-name", "night-light-symbolic",
                       "toggle-mode", TRUE,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellNightLightIndicator                                                 */
/* ------------------------------------------------------------------------ */

struct _ShellNightLightIndicator
{
  ShellSystemIndicator parent;

  StIcon *icon;                       /* weak */
  ShellSettingsDaemonColor *proxy;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (ShellNightLightIndicator, shell_night_light_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
night_light_indicator_sync (ShellNightLightIndicator *self)
{
  gboolean active = FALSE;

  if (self->icon == NULL)
    return;

  if (self->proxy != NULL)
    active = shell_settings_daemon_color_get_night_light_active (self->proxy);

  shell_status_set_visible (self->icon, active);
}

static void
proxy_properties_changed_cb (GDBusProxy               *proxy,
                             GVariant                 *changed_properties,
                             const char * const       *invalidated_properties,
                             ShellNightLightIndicator *self)
{
  g_autoptr (GVariant) value =
    g_variant_lookup_value (changed_properties, "NightLightActive", NULL);

  if (value != NULL)
    night_light_indicator_sync (self);
}

static void
proxy_ready_cb (GObject      *source,
                GAsyncResult *res,
                gpointer      user_data)
{
  g_autoptr (ShellNightLightIndicator) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (ShellSettingsDaemonColor) proxy = NULL;

  proxy = shell_settings_daemon_color_proxy_new_for_bus_finish (res, &error);

  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          if (shell_status_is_service_missing_error (error))
            g_debug ("%s: %s", G_STRFUNC, error->message);
          else
            g_message ("%s: %s", G_STRFUNC, error->message);
        }
      return;
    }

  if (self->cancellable == NULL || g_cancellable_is_cancelled (self->cancellable))
    return;

  self->proxy = g_steal_pointer (&proxy);
  g_signal_connect_object (self->proxy, "g-properties-changed",
                           G_CALLBACK (proxy_properties_changed_cb), self, 0);
  night_light_indicator_sync (self);
}

static void
night_light_indicator_release (ShellNightLightIndicator *self)
{
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->proxy);
}

static void
on_shutdown (ShellGlobal              *global,
             ShellNightLightIndicator *self)
{
  night_light_indicator_release (self);
}

static void
shell_night_light_indicator_constructed (GObject *object)
{
  ShellNightLightIndicator *self = SHELL_NIGHT_LIGHT_INDICATOR (object);
  ShellSystemIndicator *indicator = SHELL_SYSTEM_INDICATOR (object);
  StIcon *icon;

  G_OBJECT_CLASS (shell_night_light_indicator_parent_class)->constructed (object);

  icon = shell_system_indicator_add_indicator (indicator);
  g_set_weak_pointer (&self->icon, icon);
  st_icon_set_icon_name (icon, "night-light-symbolic");

  shell_system_indicator_add_quick_settings_item (indicator,
                                                  CLUTTER_ACTOR (shell_night_light_toggle_new ()));

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  self->cancellable = g_cancellable_new ();
  shell_settings_daemon_color_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 BUS_NAME, OBJECT_PATH,
                                                 self->cancellable,
                                                 proxy_ready_cb,
                                                 g_object_ref (self));

  night_light_indicator_sync (self);
}

static void
shell_night_light_indicator_dispose (GObject *object)
{
  ShellNightLightIndicator *self = SHELL_NIGHT_LIGHT_INDICATOR (object);

  night_light_indicator_release (self);
  g_clear_weak_pointer (&self->icon);

  G_OBJECT_CLASS (shell_night_light_indicator_parent_class)->dispose (object);
}

static void
shell_night_light_indicator_class_init (ShellNightLightIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_night_light_indicator_constructed;
  object_class->dispose = shell_night_light_indicator_dispose;
}

static void
shell_night_light_indicator_init (ShellNightLightIndicator *self)
{
}

/**
 * shell_night_light_indicator_new:
 *
 * Creates the indicator with the "Night Light" toggle and the icon which
 * shows while the night light is active (`new Indicator ()`).
 *
 * Returns: (transfer floating): a new #ShellNightLightIndicator
 */
ShellNightLightIndicator *
shell_night_light_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_NIGHT_LIGHT_INDICATOR, NULL);
}
