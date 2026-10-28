/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * power-profiles.c: The "Power Mode" quick settings toggle with the menu
 * that lists the profiles of power-profiles-daemon, and its status icon.
 * Port of js/ui/status/powerProfiles.js.
 *
 * The org.freedesktop.UPower.PowerProfiles proxy is a gdbus-codegen proxy
 * (`power-profiles-dbus`, type ShellPowerProfiles). Without the daemon the
 * toggle stays hidden, quietly.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - A failure to reach the daemon is logged as a message only when it is
 *    not "the service is not there" (the JS always logged it).
 *  - The toggle also follows the owner of the bus name, where the JS only
 *    followed the properties (the daemon leaving the bus invalidates them
 *    anyway).
 *  - After ShellGlobal::shutdown the proxy is dropped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>

#include "shell-global.h"
#include "ui/popup-menu.h"
#include "ui/quick-settings.h"
#include "ui/status/power-profiles.h"
#include "ui/status/status-private.h"

#include "power-profiles-dbus.h"

#define BUS_NAME "org.freedesktop.UPower.PowerProfiles"
#define OBJECT_PATH "/org/freedesktop/UPower/PowerProfiles"

#define LAST_PROFILE_KEY "last-selected-power-profile"

typedef struct
{
  const char *profile;
  const char *name;
  const char *icon_name;
} ProfileParams;

/* PROFILE_PARAMS. The names are translated when they are used. */
static const ProfileParams profile_params[] = {
  { "performance", NC_("Power profile", "Performance"), "power-profile-performance-symbolic" },
  { "balanced", NC_("Power profile", "Balanced"), "power-profile-balanced-symbolic" },
  { "power-saver", NC_("Power profile", "Power Saver"), "power-profile-power-saver-symbolic" },
};

static const ProfileParams fallback_params = {
  NULL, NC_("Power profile", "Custom"), "gnome-power-manager-symbolic"
};

static const ProfileParams *
lookup_profile_params (const char *profile)
{
  guint i;

  if (profile == NULL)
    return NULL;

  for (i = 0; i < G_N_ELEMENTS (profile_params); i++)
    {
      if (strcmp (profile_params[i].profile, profile) == 0)
        return &profile_params[i];
    }

  return NULL;
}

/* ------------------------------------------------------------------------ */
/* ShellPowerProfilesToggle                                                 */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_POWER_PROFILES_TOGGLE (shell_power_profiles_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellPowerProfilesToggle, shell_power_profiles_toggle,
                      SHELL, POWER_PROFILES_TOGGLE, ShellQuickMenuToggle)

struct _ShellPowerProfilesToggle
{
  ShellQuickMenuToggle parent;

  ShellPowerProfiles *proxy;
  GCancellable *cancellable;
  GHashTable *profile_items;            /* profile -> ShellPopupImageMenuItem */
  ShellPopupMenuSection *profile_section;  /* weak */
};

G_DEFINE_FINAL_TYPE (ShellPowerProfilesToggle, shell_power_profiles_toggle,
                     SHELL_TYPE_QUICK_MENU_TOGGLE)

static gboolean
toggle_has_name_owner (ShellPowerProfilesToggle *self)
{
  g_autofree char *owner = NULL;

  if (self->proxy == NULL)
    return FALSE;

  owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (self->proxy));
  return owner != NULL;
}

static void
toggle_set_active_profile (ShellPowerProfilesToggle *self,
                           const char               *profile)
{
  if (self->proxy != NULL)
    shell_power_profiles_set_active_profile (self->proxy, profile);
}

static void
toggle_sync (ShellPowerProfilesToggle *self)
{
  gboolean visible = toggle_has_name_owner (self);
  const char *active_profile;
  const ProfileParams *params;
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  gboolean checked;

  shell_status_set_visible (self, visible);

  if (!visible)
    return;

  active_profile = shell_power_profiles_get_active_profile (self->proxy);

  g_hash_table_iter_init (&iter, self->profile_items);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      shell_popup_base_menu_item_set_ornament (SHELL_POPUP_BASE_MENU_ITEM (value),
                                               g_strcmp0 (key, active_profile) == 0
                                               ? SHELL_ORNAMENT_CHECK
                                               : SHELL_ORNAMENT_NONE);
    }

  params = lookup_profile_params (active_profile);
  if (params == NULL)
    params = &fallback_params;
  g_object_set (self,
                "subtitle", g_dpgettext2 (GETTEXT_PACKAGE, "Power profile", params->name),
                "icon-name", params->icon_name,
                NULL);

  checked = g_strcmp0 (active_profile, "balanced") != 0;
  st_button_set_checked (ST_BUTTON (self), checked);

  if (checked)
    g_settings_set_string (shell_global_get_settings (shell_global_get ()),
                           LAST_PROFILE_KEY, active_profile);
}

static void
profile_item_activate_cb (ShellPopupBaseMenuItem   *item,
                          const ClutterEvent       *event,
                          ShellPowerProfilesToggle *self)
{
  toggle_set_active_profile (self, g_object_get_data (G_OBJECT (item), "power-profile"));
}

static void
toggle_sync_profiles (ShellPowerProfilesToggle *self)
{
  GVariant *profiles;
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  GVariantIter iter;
  GVariant *dict;
  guint i;

  if (self->profile_section == NULL)
    return;

  shell_popup_menu_base_remove_all (SHELL_POPUP_MENU_BASE (self->profile_section));
  g_hash_table_remove_all (self->profile_items);

  if (self->proxy == NULL)
    return;

  profiles = shell_power_profiles_get_profiles (self->proxy);
  if (profiles == NULL)
    {
      shell_quick_menu_toggle_set_menu_enabled (SHELL_QUICK_MENU_TOGGLE (self), FALSE);
      return;
    }

  g_variant_iter_init (&iter, profiles);
  while ((dict = g_variant_iter_next_value (&iter)) != NULL)
    {
      g_autoptr (GVariant) owned = dict;
      const char *profile = NULL;

      if (g_variant_lookup (owned, "Profile", "&s", &profile))
        g_ptr_array_add (names, g_strdup (profile));
    }

  /* .reverse () */
  for (i = names->len; i > 0; i--)
    {
      const char *profile = g_ptr_array_index (names, i - 1);
      const ProfileParams *params = lookup_profile_params (profile);
      ShellPopupImageMenuItem *item;

      if (params == NULL)
        continue;

      item = g_object_new (SHELL_TYPE_POPUP_IMAGE_MENU_ITEM,
                           "text", g_dpgettext2 (GETTEXT_PACKAGE, "Power profile", params->name),
                           "icon-name", params->icon_name,
                           NULL);
      g_object_set_data_full (G_OBJECT (item), "power-profile", g_strdup (profile), g_free);
      g_signal_connect_object (item, "activate",
                               G_CALLBACK (profile_item_activate_cb), self, 0);
      shell_popup_menu_base_add_menu_item (SHELL_POPUP_MENU_BASE (self->profile_section),
                                           G_OBJECT (item));
      g_hash_table_replace (self->profile_items, g_strdup (profile),
                            g_object_ref (item));
    }

  shell_quick_menu_toggle_set_menu_enabled (SHELL_QUICK_MENU_TOGGLE (self),
                                            g_hash_table_size (self->profile_items) > 2);
}

static void
proxy_properties_changed_cb (GDBusProxy               *proxy,
                             GVariant                 *changed_properties,
                             const char * const       *invalidated_properties,
                             ShellPowerProfilesToggle *self)
{
  g_autoptr (GVariant) value = g_variant_lookup_value (changed_properties, "Profiles", NULL);

  if (value != NULL || g_strv_contains (invalidated_properties, "Profiles"))
    toggle_sync_profiles (self);

  toggle_sync (self);
}

static void
proxy_name_owner_changed_cb (GObject                  *proxy,
                             GParamSpec               *pspec,
                             ShellPowerProfilesToggle *self)
{
  toggle_sync (self);
}

static void
proxy_ready_cb (GObject      *source,
                GAsyncResult *res,
                gpointer      user_data)
{
  g_autoptr (ShellPowerProfilesToggle) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (ShellPowerProfiles) proxy = NULL;

  proxy = shell_power_profiles_proxy_new_for_bus_finish (res, &error);

  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          if (shell_status_is_service_missing_error (error))
            g_debug ("%s: %s", G_STRFUNC, error->message);
          else
            g_message ("%s: %s", G_STRFUNC, error->message);

          toggle_sync (self);
        }
      return;
    }

  if (self->cancellable == NULL || g_cancellable_is_cancelled (self->cancellable))
    return;

  self->proxy = g_steal_pointer (&proxy);
  g_signal_connect_object (self->proxy, "g-properties-changed",
                           G_CALLBACK (proxy_properties_changed_cb), self, 0);
  g_signal_connect_object (self->proxy, "notify::g-name-owner",
                           G_CALLBACK (proxy_name_owner_changed_cb), self, 0);

  if (toggle_has_name_owner (self))
    toggle_sync_profiles (self);

  toggle_sync (self);
}

static void
toggle_release (ShellPowerProfilesToggle *self)
{
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->proxy);
}

static void
on_shutdown (ShellGlobal              *global,
             ShellPowerProfilesToggle *self)
{
  toggle_release (self);
}

static void
shell_power_profiles_toggle_clicked (StButton *button,
                                     int       clicked_button)
{
  ShellPowerProfilesToggle *self = SHELL_POWER_PROFILES_TOGGLE (button);

  if (st_button_get_checked (button))
    {
      toggle_set_active_profile (self, "balanced");
    }
  else
    {
      g_autofree char *last_profile =
        g_settings_get_string (shell_global_get_settings (shell_global_get ()),
                               LAST_PROFILE_KEY);

      toggle_set_active_profile (self, last_profile);
    }
}

static void
shell_power_profiles_toggle_constructed (GObject *object)
{
  ShellPowerProfilesToggle *self = SHELL_POWER_PROFILES_TOGGLE (object);
  ShellQuickToggleMenu *menu;
  ShellPopupMenuBase *menu_base;
  g_autoptr (ShellPopupMenuSection) section = NULL;
  g_autoptr (ShellPopupSeparatorMenuItem) separator = NULL;

  G_OBJECT_CLASS (shell_power_profiles_toggle_parent_class)->constructed (object);

  menu = shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self));
  menu_base = SHELL_POPUP_MENU_BASE (menu);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  section = shell_popup_menu_section_new ();
  shell_popup_menu_base_add_menu_item (menu_base, G_OBJECT (section));
  g_set_weak_pointer (&self->profile_section, section);

  shell_quick_toggle_menu_set_header (menu, "power-profile-balanced-symbolic",
                                      C_("Quick settings menu header", "Power Mode"), NULL);
  separator = g_object_ref_sink (g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL));
  shell_popup_menu_base_add_menu_item (menu_base, G_OBJECT (separator));
  shell_popup_menu_base_add_settings_action (menu_base, _("Power Settings"),
                                             "gnome-power-panel.desktop");

  self->cancellable = g_cancellable_new ();
  shell_power_profiles_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                          G_DBUS_PROXY_FLAGS_NONE,
                                          BUS_NAME, OBJECT_PATH,
                                          self->cancellable,
                                          proxy_ready_cb,
                                          g_object_ref (self));

  toggle_sync (self);
}

static void
shell_power_profiles_toggle_dispose (GObject *object)
{
  ShellPowerProfilesToggle *self = SHELL_POWER_PROFILES_TOGGLE (object);

  toggle_release (self);
  g_clear_weak_pointer (&self->profile_section);
  g_hash_table_remove_all (self->profile_items);

  G_OBJECT_CLASS (shell_power_profiles_toggle_parent_class)->dispose (object);
}

static void
shell_power_profiles_toggle_finalize (GObject *object)
{
  ShellPowerProfilesToggle *self = SHELL_POWER_PROFILES_TOGGLE (object);

  g_clear_pointer (&self->profile_items, g_hash_table_unref);

  G_OBJECT_CLASS (shell_power_profiles_toggle_parent_class)->finalize (object);
}

static void
shell_power_profiles_toggle_class_init (ShellPowerProfilesToggleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->constructed = shell_power_profiles_toggle_constructed;
  object_class->dispose = shell_power_profiles_toggle_dispose;
  object_class->finalize = shell_power_profiles_toggle_finalize;

  button_class->clicked = shell_power_profiles_toggle_clicked;
}

static void
shell_power_profiles_toggle_init (ShellPowerProfilesToggle *self)
{
  self->profile_items = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, g_object_unref);
}

static ShellPowerProfilesToggle *
shell_power_profiles_toggle_new (void)
{
  return g_object_new (SHELL_TYPE_POWER_PROFILES_TOGGLE,
                       "title", C_("Quick settings button title", "Power Mode"),
                       "menu-button-accessible-name", _("Open power profiles menu"),
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellPowerProfilesIndicator                                              */
/* ------------------------------------------------------------------------ */

struct _ShellPowerProfilesIndicator
{
  ShellSystemIndicator parent;
};

G_DEFINE_FINAL_TYPE (ShellPowerProfilesIndicator, shell_power_profiles_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
shell_power_profiles_indicator_constructed (GObject *object)
{
  ShellSystemIndicator *indicator = SHELL_SYSTEM_INDICATOR (object);
  StIcon *icon;
  ShellPowerProfilesToggle *toggle;

  G_OBJECT_CLASS (shell_power_profiles_indicator_parent_class)->constructed (object);

  icon = shell_system_indicator_add_indicator (indicator);

  toggle = shell_power_profiles_toggle_new ();
  g_object_bind_property (toggle, "icon-name", icon, "icon-name", G_BINDING_SYNC_CREATE);
  g_object_bind_property (toggle, "checked", icon, "visible", G_BINDING_SYNC_CREATE);

  shell_system_indicator_add_quick_settings_item (indicator, CLUTTER_ACTOR (toggle));
}

static void
shell_power_profiles_indicator_class_init (ShellPowerProfilesIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_power_profiles_indicator_constructed;
}

static void
shell_power_profiles_indicator_init (ShellPowerProfilesIndicator *self)
{
}

/**
 * shell_power_profiles_indicator_new:
 *
 * Creates the indicator with the "Power Mode" toggle and the icon which
 * shows while a profile other than the balanced one is active
 * (`new Indicator ()`).
 *
 * Returns: (transfer floating): a new #ShellPowerProfilesIndicator
 */
ShellPowerProfilesIndicator *
shell_power_profiles_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_POWER_PROFILES_INDICATOR, NULL);
}
