/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * system.c: The power and system indicator of the top bar and the row of
 * buttons at the bottom of the quick settings (battery, screenshot,
 * settings, lock screen and the power off menu). Port of
 * js/ui/status/system.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The indicator is #ShellSystemStatusIndicator; the screenshot button
 *    does not reach `Main.screenshotUI` (a JS object): after the quick
 *    settings menu closed it emits ::screenshot-ui-requested (before the
 *    next redraw, as the JS scheduled the call) on the button, the row of
 *    buttons and the indicator, and js/ui/panelItems.js opens the UI.
 *  - `Main.overview.hide ()` and `Main.panel.close_quick_settings ()` of the
 *    power and the settings buttons are ShellMain::settings-app-activating,
 *    which main.js answers with exactly those two calls. The buttons of the
 *    power off menu close the quick settings through the panel.
 *  - The battery is read from a gdbus-codegen proxy of
 *    org.freedesktop.UPower.Device; an unreachable UPower is a debug message
 *    instead of an error. Without the proxy no battery is present.
 *  - The percentage is formatted as "%d%%"; the JS used Intl.NumberFormat,
 *    which also applies the digits and the spacing of the locale.
 *  - A missing Settings application logs a message instead of a warning, and
 *    the button stays hidden (activating it would have thrown). A missing
 *    power panel does not activate anything.
 *  - Failed system actions are logged as warnings instead of throwing into
 *    the signal handler.
 *  - Everything that is connected to the session mode is disconnected when
 *    the button is disposed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <atk/atk.h>
#include <glib/gi18n-lib.h>
#include <math.h>
#include <meta/compositor.h>
#include <upower.h>

#include "misc/system-actions.h"
#include "shell-app.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "ui/main.h"
#include "ui/panel.h"
#include "ui/popup-menu.h"
#include "ui/status/status-private.h"
#include "ui/status/system.h"

#include "upower-device-dbus.h"

#define UPOWER_BUS_NAME "org.freedesktop.UPower"
#define UPOWER_OBJECT_PATH "/org/freedesktop/UPower/devices/DisplayDevice"

#define SHOW_BATTERY_PERCENTAGE_KEY "show-battery-percentage"

/* Main.sessionMode.allow_settings */
static gboolean
session_allows_settings (void)
{
  ShellSessionMode *session_mode = shell_status_get_session_mode ();
  gboolean allow_settings = FALSE;

  if (session_mode != NULL)
    g_object_get (session_mode, "allow-settings", &allow_settings, NULL);

  return allow_settings;
}

/* Main.panel.close_quick_settings () */
static void
close_quick_settings (void)
{
  ShellPanel *panel = shell_main_get_panel (shell_main_get_default ());

  if (panel != NULL)
    shell_panel_close_quick_settings (panel);
}

/* ======================================================================== */
/* ShellPowerToggle                                                         */
/* ======================================================================== */

enum
{
  POWER_PROP_0,

  POWER_PROP_FALLBACK_ICON_NAME,

  N_POWER_PROPS
};

static GParamSpec *power_props[N_POWER_PROPS] = { NULL, };

struct _ShellPowerToggle
{
  ShellQuickToggle parent;

  ShellUPowerDevice *proxy;     /* nullable */
  GCancellable *cancellable;
  char *fallback_icon_name;
};

G_DEFINE_FINAL_TYPE (ShellPowerToggle, shell_power_toggle, SHELL_TYPE_QUICK_TOGGLE)

/**
 * shell_power_toggle_get_fallback_icon_name:
 * @self: a #ShellPowerToggle
 *
 * Returns: (nullable): the name of the icon to fall back to if the icon of the
 *   battery level is not in the theme (`fallback-icon-name`)
 */
const char *
shell_power_toggle_get_fallback_icon_name (ShellPowerToggle *self)
{
  g_return_val_if_fail (SHELL_IS_POWER_TOGGLE (self), NULL);

  return self->fallback_icon_name;
}

/**
 * shell_power_toggle_set_fallback_icon_name:
 * @self: a #ShellPowerToggle
 * @fallback_icon_name: (nullable): an icon name
 *
 * Sets #ShellPowerToggle:fallback-icon-name.
 */
void
shell_power_toggle_set_fallback_icon_name (ShellPowerToggle *self,
                                           const char       *fallback_icon_name)
{
  g_return_if_fail (SHELL_IS_POWER_TOGGLE (self));

  if (g_strcmp0 (self->fallback_icon_name, fallback_icon_name) == 0)
    return;

  g_free (self->fallback_icon_name);
  self->fallback_icon_name = g_strdup (fallback_icon_name);
  g_object_notify_by_pspec (G_OBJECT (self), power_props[POWER_PROP_FALLBACK_ICON_NAME]);
}

/* _sync () */
static void
power_toggle_sync (ShellPowerToggle *self)
{
  const char *charging_state;
  g_autofree char *icon = NULL;
  g_autofree char *title = NULL;
  g_autoptr (GIcon) gicon = NULL;
  UpDeviceState state;
  double percentage;
  int fill_level;
  gboolean charged;
  gboolean present;

  /* Do we have batteries or a UPS? */
  present = self->proxy != NULL && shell_upower_device_get_is_present (self->proxy);
  shell_status_set_visible (self, present);
  if (!present)
    return;

  /* The icons */
  state = shell_upower_device_get_state (self->proxy);
  if (state == UP_DEVICE_STATE_CHARGING)
    charging_state = "-charging";
  else if (state == UP_DEVICE_STATE_PENDING_CHARGE &&
           shell_upower_device_get_charge_threshold_enabled (self->proxy))
    charging_state = "-plugged-in";
  else
    charging_state = "";

  percentage = shell_upower_device_get_percentage (self->proxy);
  fill_level = 10 * (int) floor (percentage / 10);
  charged = state == UP_DEVICE_STATE_FULLY_CHARGED ||
            (state == UP_DEVICE_STATE_CHARGING && fill_level == 100);
  if (charged)
    icon = g_strdup ("battery-level-100-charged-symbolic");
  else
    icon = g_strdup_printf ("battery-level-%d%s-symbolic", fill_level, charging_state);

  /* Make sure we fall back to fallback-icon-name and not GThemedIcon's
   * default fallbacks */
  gicon = g_object_new (G_TYPE_THEMED_ICON,
                        "name", icon,
                        "use-default-fallbacks", FALSE,
                        NULL);

  title = g_strdup_printf ("%d%%", (int) round (percentage));

  g_object_freeze_notify (G_OBJECT (self));
  g_object_set (self,
                "title", title,
                "gicon", gicon,
                NULL);
  shell_power_toggle_set_fallback_icon_name (self, shell_upower_device_get_icon_name (self->proxy));
  g_object_thaw_notify (G_OBJECT (self));
}

static void
power_toggle_proxy_properties_changed_cb (GDBusProxy       *proxy,
                                          GVariant         *changed,
                                          const char * const *invalidated,
                                          ShellPowerToggle *self)
{
  power_toggle_sync (self);
}

static void
power_toggle_proxy_ready_cb (GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  ShellUPowerDevice *proxy;
  ShellPowerToggle *self;

  proxy = shell_upower_device_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  self = user_data;
  if (proxy == NULL)
    {
      if (shell_status_is_service_missing_error (error))
        g_debug ("Failed to connect to UPower: %s", error->message);
      else
        g_message ("Failed to connect to UPower: %s", error->message);
    }
  else
    {
      self->proxy = proxy;
      g_signal_connect_object (proxy, "g-properties-changed",
                               G_CALLBACK (power_toggle_proxy_properties_changed_cb),
                               self, G_CONNECT_AFTER);
    }

  power_toggle_sync (self);
}

/* _sessionUpdated () */
static void
power_toggle_session_updated (ShellPowerToggle *self)
{
  clutter_actor_set_reactive (CLUTTER_ACTOR (self), session_allows_settings ());
}

static void
power_toggle_clicked_cb (ShellPowerToggle *self)
{
  ShellApp *app = shell_app_system_lookup_app (shell_app_system_get_default (),
                                               "gnome-power-panel.desktop");

  shell_main_emit_settings_app_activating (shell_main_get_default ());
  if (app != NULL)
    shell_app_activate (app);
}

static void
power_toggle_shutdown_cb (ShellGlobal      *global,
                          ShellPowerToggle *self)
{
  g_cancellable_cancel (self->cancellable);
}

static void
shell_power_toggle_constructed (GObject *object)
{
  ShellPowerToggle *self = SHELL_POWER_TOGGLE (object);
  ShellSessionMode *session_mode = shell_status_get_session_mode ();

  G_OBJECT_CLASS (shell_power_toggle_parent_class)->constructed (object);

  clutter_actor_set_accessible_role (CLUTTER_ACTOR (self), ATK_ROLE_PUSH_BUTTON);
  st_widget_add_style_class_name (ST_WIDGET (self), "power-item");

  shell_upower_device_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         UPOWER_BUS_NAME,
                                         UPOWER_OBJECT_PATH,
                                         self->cancellable,
                                         power_toggle_proxy_ready_cb,
                                         self);

  g_object_bind_property (self, "fallback-icon-name",
                          shell_quick_toggle_get_icon (SHELL_QUICK_TOGGLE (self)),
                          "fallback-icon-name",
                          G_BINDING_SYNC_CREATE);

  g_signal_connect (self, "clicked", G_CALLBACK (power_toggle_clicked_cb), NULL);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (power_toggle_shutdown_cb), self, 0);

  if (session_mode != NULL)
    g_signal_connect_object (session_mode, "updated",
                             G_CALLBACK (power_toggle_session_updated),
                             self, G_CONNECT_SWAPPED);
  power_toggle_session_updated (self);
  power_toggle_sync (self);
}

static void
shell_power_toggle_dispose (GObject *object)
{
  ShellPowerToggle *self = SHELL_POWER_TOGGLE (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  if (self->proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->proxy, self);
  g_clear_object (&self->proxy);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (shell_power_toggle_parent_class)->dispose (object);
}

static void
shell_power_toggle_finalize (GObject *object)
{
  ShellPowerToggle *self = SHELL_POWER_TOGGLE (object);

  g_free (self->fallback_icon_name);

  G_OBJECT_CLASS (shell_power_toggle_parent_class)->finalize (object);
}

static void
shell_power_toggle_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellPowerToggle *self = SHELL_POWER_TOGGLE (object);

  switch (prop_id)
    {
    case POWER_PROP_FALLBACK_ICON_NAME:
      g_value_set_string (value, self->fallback_icon_name != NULL ? self->fallback_icon_name : "");
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_power_toggle_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellPowerToggle *self = SHELL_POWER_TOGGLE (object);

  switch (prop_id)
    {
    case POWER_PROP_FALLBACK_ICON_NAME:
      shell_power_toggle_set_fallback_icon_name (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_power_toggle_class_init (ShellPowerToggleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_power_toggle_constructed;
  object_class->dispose = shell_power_toggle_dispose;
  object_class->finalize = shell_power_toggle_finalize;
  object_class->get_property = shell_power_toggle_get_property;
  object_class->set_property = shell_power_toggle_set_property;

  /**
   * ShellPowerToggle:fallback-icon-name:
   *
   * The icon name which the icon of the toggle falls back to if the icon of
   * the battery level is missing from the icon theme.
   */
  power_props[POWER_PROP_FALLBACK_ICON_NAME] =
    g_param_spec_string ("fallback-icon-name", NULL, NULL,
                         "",
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_POWER_PROPS, power_props);
}

static void
shell_power_toggle_init (ShellPowerToggle *self)
{
  self->cancellable = g_cancellable_new ();
}

/**
 * shell_power_toggle_new:
 *
 * Creates the toggle which shows the battery level and opens the power panel
 * of the settings (`new PowerToggle ()`). It is only visible if the machine has
 * a battery or an UPS.
 *
 * Returns: (transfer floating): a new #ShellPowerToggle
 */
ShellPowerToggle *
shell_power_toggle_new (void)
{
  return g_object_new (SHELL_TYPE_POWER_TOGGLE, NULL);
}

/* ======================================================================== */
/* ShellScreenshotItem                                                      */
/* ======================================================================== */

enum
{
  SCREENSHOT_UI_REQUESTED,

  N_ITEM_SIGNALS
};

static guint screenshot_signals[N_ITEM_SIGNALS] = { 0, };

struct _ShellScreenshotItem
{
  ShellQuickSettingsItem parent;
};

G_DEFINE_FINAL_TYPE (ShellScreenshotItem, shell_screenshot_item,
                     SHELL_TYPE_QUICK_SETTINGS_ITEM)

static gboolean
screenshot_item_later_cb (gpointer user_data)
{
  GWeakRef *ref = user_data;
  g_autoptr (ShellScreenshotItem) self = g_weak_ref_get (ref);

  if (self != NULL)
    g_signal_emit (self, screenshot_signals[SCREENSHOT_UI_REQUESTED], 0);

  return G_SOURCE_REMOVE;
}

static void
screenshot_item_weak_ref_free (gpointer data)
{
  GWeakRef *ref = data;

  g_weak_ref_clear (ref);
  g_free (ref);
}

static void
screenshot_item_clicked_cb (ShellScreenshotItem *self)
{
  ShellPanel *panel = shell_main_get_panel (shell_main_get_default ());
  ShellPanelButtonBox *quick_settings;
  MetaLaters *laters;
  GWeakRef *ref;

  laters = meta_compositor_get_laters (shell_global_get_compositor (shell_global_get ()));
  ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, self);
  meta_laters_add (laters, META_LATER_BEFORE_REDRAW,
                   screenshot_item_later_cb, ref, screenshot_item_weak_ref_free);

  quick_settings = panel != NULL
    ? shell_panel_get_status_area_indicator (panel, "quickSettings")
    : NULL;
  if (quick_settings != NULL && SHELL_IS_PANEL_MENU_BUTTON (quick_settings))
    {
      GObject *menu = shell_panel_menu_button_get_menu (SHELL_PANEL_MENU_BUTTON (quick_settings));

      if (SHELL_IS_POPUP_MENU_BASE (menu))
        shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (menu), SHELL_POPUP_ANIMATION_NONE);
    }
}

static void
shell_screenshot_item_constructed (GObject *object)
{
  ShellScreenshotItem *self = SHELL_SCREENSHOT_ITEM (object);

  G_OBJECT_CLASS (shell_screenshot_item_parent_class)->constructed (object);

  g_signal_connect (self, "clicked", G_CALLBACK (screenshot_item_clicked_cb), NULL);
}

static void
shell_screenshot_item_class_init (ShellScreenshotItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_screenshot_item_constructed;

  /**
   * ShellScreenshotItem::screenshot-ui-requested:
   * @self: the #ShellScreenshotItem
   *
   * Emitted before the redraw which follows a click, once the quick settings
   * menu is closed, so that the owner opens the screenshot UI
   * (`Main.screenshotUI.open ()`).
   */
  screenshot_signals[SCREENSHOT_UI_REQUESTED] =
    g_signal_new ("screenshot-ui-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_screenshot_item_init (ShellScreenshotItem *self)
{
}

/**
 * shell_screenshot_item_new:
 *
 * Creates the button of the screenshot UI (`new ScreenshotItem ()`); it is
 * hidden in the greeter.
 *
 * Returns: (transfer floating): a new #ShellScreenshotItem
 */
ShellScreenshotItem *
shell_screenshot_item_new (void)
{
  ShellSessionMode *session_mode = shell_status_get_session_mode ();

  return g_object_new (SHELL_TYPE_SCREENSHOT_ITEM,
                       "style-class", "icon-button",
                       "can-focus", TRUE,
                       "icon-name", "screenshooter-symbolic",
                       "visible", session_mode == NULL || !shell_session_mode_is_greeter (session_mode),
                       "accessible-name", _("Take Screenshot"),
                       NULL);
}

/* ======================================================================== */
/* ShellSettingsItem                                                        */
/* ======================================================================== */

struct _ShellSettingsItem
{
  ShellQuickSettingsItem parent;

  ShellApp *settings_app;       /* nullable */
};

G_DEFINE_FINAL_TYPE (ShellSettingsItem, shell_settings_item,
                     SHELL_TYPE_QUICK_SETTINGS_ITEM)

/* _sync () */
static void
settings_item_sync (ShellSettingsItem *self)
{
  shell_status_set_visible (self, self->settings_app != NULL && session_allows_settings ());
}

static void
settings_item_clicked_cb (ShellSettingsItem *self)
{
  shell_main_emit_settings_app_activating (shell_main_get_default ());
  if (self->settings_app != NULL)
    shell_app_activate (self->settings_app);
}

static void
shell_settings_item_constructed (GObject *object)
{
  ShellSettingsItem *self = SHELL_SETTINGS_ITEM (object);
  ShellSessionMode *session_mode = shell_status_get_session_mode ();
  ShellApp *settings_app;
  ClutterActor *icon;

  G_OBJECT_CLASS (shell_settings_item_parent_class)->constructed (object);

  settings_app = shell_app_system_lookup_app (shell_app_system_get_default (),
                                              "org.gnome.Settings.desktop");
  if (settings_app != NULL)
    self->settings_app = g_object_ref (settings_app);
  else
    g_message ("Missing required core component Settings, expect trouble…");

  icon = st_icon_new ();
  st_bin_set_child (ST_BIN (self), icon);
  if (self->settings_app != NULL)
    {
      st_icon_set_gicon (ST_ICON (icon), shell_app_get_icon (self->settings_app));
      clutter_actor_set_accessible_name (CLUTTER_ACTOR (self),
                                         shell_app_get_name (self->settings_app));
    }

  g_signal_connect (self, "clicked", G_CALLBACK (settings_item_clicked_cb), NULL);

  if (session_mode != NULL)
    g_signal_connect_object (session_mode, "updated",
                             G_CALLBACK (settings_item_sync),
                             self, G_CONNECT_SWAPPED);
  settings_item_sync (self);
}

static void
shell_settings_item_dispose (GObject *object)
{
  ShellSettingsItem *self = SHELL_SETTINGS_ITEM (object);

  g_clear_object (&self->settings_app);

  G_OBJECT_CLASS (shell_settings_item_parent_class)->dispose (object);
}

static void
shell_settings_item_class_init (ShellSettingsItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_settings_item_constructed;
  object_class->dispose = shell_settings_item_dispose;
}

static void
shell_settings_item_init (ShellSettingsItem *self)
{
}

/**
 * shell_settings_item_new:
 *
 * Creates the button which opens the Settings application
 * (`new SettingsItem ()`); it is hidden if the application is missing or the
 * session does not allow settings.
 *
 * Returns: (transfer floating): a new #ShellSettingsItem
 */
ShellSettingsItem *
shell_settings_item_new (void)
{
  return g_object_new (SHELL_TYPE_SETTINGS_ITEM,
                       "style-class", "icon-button",
                       "can-focus", TRUE,
                       NULL);
}

/* ======================================================================== */
/* ShellShutdownItem                                                        */
/* ======================================================================== */

#define N_SYSTEM_ACTIONS 5

struct _ShellShutdownItem
{
  ShellQuickSettingsItem parent;

  ShellSystemActions *system_actions;
  ClutterActor *items[N_SYSTEM_ACTIONS]; /* weak */
  guint n_items;
};

G_DEFINE_FINAL_TYPE (ShellShutdownItem, shell_shutdown_item,
                     SHELL_TYPE_QUICK_SETTINGS_ITEM)

typedef gboolean (* SystemActionFunc) (ShellSystemActions  *actions,
                                       GError             **error);

/* _sync () */
static void
shutdown_item_sync (ShellShutdownItem *self)
{
  gboolean any_visible = FALSE;
  guint i;

  for (i = 0; i < self->n_items; i++)
    {
      if (self->items[i] != NULL && clutter_actor_is_visible (self->items[i]))
        {
          any_visible = TRUE;
          break;
        }
    }

  shell_status_set_visible (self, any_visible);
}

/* What an item of the power off menu needs when it is activated */
typedef struct
{
  GWeakRef item;
  SystemActionFunc func;
} SystemActionData;

static void
system_action_data_free (gpointer data)
{
  SystemActionData *action = data;

  g_weak_ref_clear (&action->item);
  g_free (action);
}

static void
shutdown_item_action_activated_cb (const ClutterEvent *event,
                                   gpointer            user_data)
{
  SystemActionData *action = user_data;
  g_autoptr (ShellShutdownItem) self = g_weak_ref_get (&action->item);
  g_autoptr (GError) error = NULL;

  if (self == NULL || self->system_actions == NULL)
    return;

  if (!action->func (self->system_actions, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);

  close_quick_settings ();
}

/* _addSystemAction () */
static void
shutdown_item_add_system_action (ShellShutdownItem *self,
                                 const char        *label,
                                 const char        *prop_name,
                                 SystemActionFunc   func)
{
  ShellPopupMenuBase *menu =
    SHELL_POPUP_MENU_BASE (shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self)));
  SystemActionData *action = g_new0 (SystemActionData, 1);
  ShellPopupBaseMenuItem *item;

  g_weak_ref_init (&action->item, self);
  action->func = func;

  item = shell_popup_menu_base_add_action (menu, label,
                                           shutdown_item_action_activated_cb,
                                           action, system_action_data_free);

  g_set_weak_pointer (&self->items[self->n_items], CLUTTER_ACTOR (item));
  self->n_items++;

  g_object_bind_property (self->system_actions, prop_name,
                          item, "visible",
                          G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE);
  g_signal_connect_object (item, "notify::visible",
                           G_CALLBACK (shutdown_item_sync), self, G_CONNECT_SWAPPED);
}

static void
shutdown_item_notify_mapped_cb (ShellShutdownItem *self)
{
  if (!clutter_actor_is_mapped (CLUTTER_ACTOR (self)))
    return;

  /* Whether shutdown is available or not depends on both lockdown
   * settings (disable-log-out) and Polkit policy - the latter doesn't
   * notify, so we update the item each time we become visible or
   * the lockdown setting changes, which should be close enough. */
  shell_system_actions_force_update (self->system_actions);
}

static void
shutdown_item_open_menu (ShellShutdownItem *self)
{
  ShellQuickToggleMenu *menu = shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self));

  if (menu != NULL)
    shell_popup_menu_base_open (SHELL_POPUP_MENU_BASE (menu), SHELL_POPUP_ANIMATION_FULL);
}

static void
shell_shutdown_item_constructed (GObject *object)
{
  ShellShutdownItem *self = SHELL_SHUTDOWN_ITEM (object);
  ShellQuickToggleMenu *menu;

  G_OBJECT_CLASS (shell_shutdown_item_parent_class)->constructed (object);

  self->system_actions = g_object_ref (shell_system_actions_get_default ());

  menu = shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self));
  shell_quick_toggle_menu_set_header (menu, "system-shutdown-symbolic",
                                      C_("title", "Power Off"), NULL);

  shutdown_item_add_system_action (self, _("Suspend"), "can-suspend",
                                   shell_system_actions_activate_suspend);
  shutdown_item_add_system_action (self, _("Restart…"), "can-restart",
                                   shell_system_actions_activate_restart);
  shutdown_item_add_system_action (self, _("Power Off…"), "can-power-off",
                                   shell_system_actions_activate_power_off);

  shell_popup_menu_base_add_menu_item (SHELL_POPUP_MENU_BASE (menu),
                                       g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL));

  shutdown_item_add_system_action (self, _("Log Out…"), "can-logout",
                                   shell_system_actions_activate_logout);
  shutdown_item_add_system_action (self, _("Switch User…"), "can-switch-user",
                                   shell_system_actions_activate_switch_user);

  g_signal_connect (self, "notify::mapped",
                    G_CALLBACK (shutdown_item_notify_mapped_cb), NULL);

  g_signal_connect (self, "clicked", G_CALLBACK (shutdown_item_open_menu), NULL);
  g_signal_connect (self, "popup-menu", G_CALLBACK (shutdown_item_open_menu), NULL);
}

static void
shell_shutdown_item_dispose (GObject *object)
{
  ShellShutdownItem *self = SHELL_SHUTDOWN_ITEM (object);
  guint i;

  for (i = 0; i < self->n_items; i++)
    g_clear_weak_pointer (&self->items[i]);
  self->n_items = 0;

  g_clear_object (&self->system_actions);

  G_OBJECT_CLASS (shell_shutdown_item_parent_class)->dispose (object);
}

static void
shell_shutdown_item_class_init (ShellShutdownItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_shutdown_item_constructed;
  object_class->dispose = shell_shutdown_item_dispose;
}

static void
shell_shutdown_item_init (ShellShutdownItem *self)
{
}

/**
 * shell_shutdown_item_new:
 *
 * Creates the button which opens the power off menu (`new ShutdownItem ()`).
 * It is hidden while none of the actions of the menu is available.
 *
 * Returns: (transfer floating): a new #ShellShutdownItem
 */
ShellShutdownItem *
shell_shutdown_item_new (void)
{
  return g_object_new (SHELL_TYPE_SHUTDOWN_ITEM,
                       "style-class", "icon-button",
                       "has-menu", TRUE,
                       "can-focus", TRUE,
                       "icon-name", "system-shutdown-symbolic",
                       "accessible-name", _("Power Off Menu"),
                       NULL);
}

/* ======================================================================== */
/* ShellLockItem                                                            */
/* ======================================================================== */

struct _ShellLockItem
{
  ShellQuickSettingsItem parent;
};

G_DEFINE_FINAL_TYPE (ShellLockItem, shell_lock_item, SHELL_TYPE_QUICK_SETTINGS_ITEM)

static void
lock_item_clicked_cb (ShellLockItem *self)
{
  g_autoptr (GError) error = NULL;

  if (!shell_system_actions_activate_lock_screen (shell_system_actions_get_default (), &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

static void
shell_lock_item_constructed (GObject *object)
{
  ShellLockItem *self = SHELL_LOCK_ITEM (object);

  G_OBJECT_CLASS (shell_lock_item_parent_class)->constructed (object);

  g_object_bind_property (shell_system_actions_get_default (), "can-lock-screen",
                          self, "visible",
                          G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE);

  g_signal_connect (self, "clicked", G_CALLBACK (lock_item_clicked_cb), NULL);
}

static void
shell_lock_item_class_init (ShellLockItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_lock_item_constructed;
}

static void
shell_lock_item_init (ShellLockItem *self)
{
}

/**
 * shell_lock_item_new:
 *
 * Creates the button which locks the screen (`new LockItem ()`). It is hidden
 * while the screen cannot be locked.
 *
 * Returns: (transfer floating): a new #ShellLockItem
 */
ShellLockItem *
shell_lock_item_new (void)
{
  return g_object_new (SHELL_TYPE_LOCK_ITEM,
                       "style-class", "icon-button",
                       "can-focus", TRUE,
                       "icon-name", "system-lock-screen-symbolic",
                       "accessible-name", C_("action", "Lock Screen"),
                       NULL);
}

/* ======================================================================== */
/* ShellSystemItem                                                          */
/* ======================================================================== */

enum
{
  SYSTEM_ITEM_SCREENSHOT_UI_REQUESTED,

  N_SYSTEM_ITEM_SIGNALS
};

static guint system_item_signals[N_SYSTEM_ITEM_SIGNALS] = { 0, };

struct _ShellSystemItem
{
  ShellQuickSettingsItem parent;

  ShellPowerToggle *power_toggle; /* weak */
};

G_DEFINE_FINAL_TYPE (ShellSystemItem, shell_system_item,
                     SHELL_TYPE_QUICK_SETTINGS_ITEM)

/**
 * shell_system_item_get_power_toggle:
 * @self: a #ShellSystemItem
 *
 * Gets the toggle which shows the battery (`powerToggle`).
 *
 * Returns: (transfer none): the #ShellPowerToggle
 */
ShellPowerToggle *
shell_system_item_get_power_toggle (ShellSystemItem *self)
{
  g_return_val_if_fail (SHELL_IS_SYSTEM_ITEM (self), NULL);

  return self->power_toggle;
}

static void
system_item_screenshot_ui_requested_cb (ShellSystemItem *self)
{
  g_signal_emit (self, system_item_signals[SYSTEM_ITEM_SCREENSHOT_UI_REQUESTED], 0);
}

static void
shell_system_item_constructed (GObject *object)
{
  ShellSystemItem *self = SHELL_SYSTEM_ITEM (object);
  ClutterActor *box;
  ShellPowerToggle *power_toggle;
  ClutterActor *laptop_spacer;
  ClutterActor *desktop_spacer;
  ShellScreenshotItem *screenshot_item;
  ShellShutdownItem *shutdown_item;

  G_OBJECT_CLASS (shell_system_item_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (self), "quick-settings-system-item");
  clutter_actor_set_reactive (CLUTTER_ACTOR (self), FALSE);

  box = g_object_new (ST_TYPE_BOX_LAYOUT, NULL);
  st_bin_set_child (ST_BIN (self), box);

  power_toggle = shell_power_toggle_new ();
  g_set_weak_pointer (&self->power_toggle, power_toggle);
  clutter_actor_add_child (box, CLUTTER_ACTOR (power_toggle));

  laptop_spacer = g_object_new (CLUTTER_TYPE_ACTOR, "x-expand", TRUE, NULL);
  g_object_bind_property (power_toggle, "visible",
                          laptop_spacer, "visible",
                          G_BINDING_SYNC_CREATE);
  clutter_actor_add_child (box, laptop_spacer);

  screenshot_item = shell_screenshot_item_new ();
  g_signal_connect_object (screenshot_item, "screenshot-ui-requested",
                           G_CALLBACK (system_item_screenshot_ui_requested_cb),
                           self, G_CONNECT_SWAPPED);
  clutter_actor_add_child (box, CLUTTER_ACTOR (screenshot_item));

  clutter_actor_add_child (box, CLUTTER_ACTOR (shell_settings_item_new ()));

  desktop_spacer = g_object_new (CLUTTER_TYPE_ACTOR, "x-expand", TRUE, NULL);
  g_object_bind_property (power_toggle, "visible",
                          desktop_spacer, "visible",
                          G_BINDING_INVERT_BOOLEAN | G_BINDING_SYNC_CREATE);
  clutter_actor_add_child (box, desktop_spacer);

  clutter_actor_add_child (box, CLUTTER_ACTOR (shell_lock_item_new ()));

  shutdown_item = shell_shutdown_item_new ();
  clutter_actor_add_child (box, CLUTTER_ACTOR (shutdown_item));

  shell_quick_settings_item_set_menu (SHELL_QUICK_SETTINGS_ITEM (self),
                                      shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (shutdown_item)));
}

static void
shell_system_item_dispose (GObject *object)
{
  ShellSystemItem *self = SHELL_SYSTEM_ITEM (object);

  g_clear_weak_pointer (&self->power_toggle);

  G_OBJECT_CLASS (shell_system_item_parent_class)->dispose (object);
}

static void
shell_system_item_class_init (ShellSystemItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_system_item_constructed;
  object_class->dispose = shell_system_item_dispose;

  /**
   * ShellSystemItem::screenshot-ui-requested:
   * @self: the #ShellSystemItem
   *
   * Emitted when the screenshot button asks for the screenshot UI, see
   * #ShellScreenshotItem::screenshot-ui-requested.
   */
  system_item_signals[SYSTEM_ITEM_SCREENSHOT_UI_REQUESTED] =
    g_signal_new ("screenshot-ui-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_system_item_init (ShellSystemItem *self)
{
}

/**
 * shell_system_item_new:
 *
 * Creates the row of buttons at the bottom of the quick settings
 * (`new SystemItem ()`).
 *
 * Returns: (transfer floating): a new #ShellSystemItem
 */
ShellSystemItem *
shell_system_item_new (void)
{
  return g_object_new (SHELL_TYPE_SYSTEM_ITEM, NULL);
}

/* ======================================================================== */
/* ShellSystemStatusIndicator                                               */
/* ======================================================================== */

enum
{
  INDICATOR_SCREENSHOT_UI_REQUESTED,

  N_INDICATOR_SIGNALS
};

static guint indicator_signals[N_INDICATOR_SIGNALS] = { 0, };

struct _ShellSystemStatusIndicator
{
  ShellSystemIndicator parent;

  GSettings *desktop_settings;  /* nullable */
  StIcon *indicator;            /* weak */
  StLabel *percentage_label;    /* weak */
  ShellSystemItem *system_item; /* weak */
};

G_DEFINE_FINAL_TYPE (ShellSystemStatusIndicator, shell_system_status_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

/**
 * shell_system_status_indicator_get_system_item:
 * @self: a #ShellSystemStatusIndicator
 *
 * Gets the row of buttons which the indicator adds to the quick settings
 * (`_systemItem`).
 *
 * Returns: (transfer none): the #ShellSystemItem
 */
ShellSystemItem *
shell_system_status_indicator_get_system_item (ShellSystemStatusIndicator *self)
{
  g_return_val_if_fail (SHELL_IS_SYSTEM_STATUS_INDICATOR (self), NULL);

  return self->system_item;
}

/* _sync () */
static void
system_status_indicator_sync (ShellSystemStatusIndicator *self)
{
  ShellPowerToggle *power_toggle;

  if (self->system_item == NULL || self->indicator == NULL || self->percentage_label == NULL)
    return;

  power_toggle = shell_system_item_get_power_toggle (self->system_item);
  if (power_toggle != NULL && clutter_actor_is_visible (CLUTTER_ACTOR (power_toggle)))
    {
      st_icon_set_gicon (self->indicator,
                         shell_quick_toggle_get_gicon (SHELL_QUICK_TOGGLE (power_toggle)));
      st_icon_set_fallback_icon_name (self->indicator,
                                      shell_power_toggle_get_fallback_icon_name (power_toggle));
      shell_status_set_visible (self->percentage_label,
                                self->desktop_settings != NULL &&
                                g_settings_get_boolean (self->desktop_settings,
                                                        SHOW_BATTERY_PERCENTAGE_KEY));
    }
  else
    {
      /* If there's no battery, then we use the power icon. */
      st_icon_set_icon_name (self->indicator, "system-shutdown-symbolic");
      clutter_actor_hide (CLUTTER_ACTOR (self->percentage_label));
    }
}

static void
system_status_indicator_screenshot_ui_requested_cb (ShellSystemStatusIndicator *self)
{
  g_signal_emit (self, indicator_signals[INDICATOR_SCREENSHOT_UI_REQUESTED], 0);
}

static void
shell_system_status_indicator_constructed (GObject *object)
{
  ShellSystemStatusIndicator *self = SHELL_SYSTEM_STATUS_INDICATOR (object);
  ShellSystemIndicator *indicator_base = SHELL_SYSTEM_INDICATOR (self);
  ShellPowerToggle *power_toggle;
  ShellSystemItem *system_item;
  StIcon *indicator;
  ClutterActor *label;

  G_OBJECT_CLASS (shell_system_status_indicator_parent_class)->constructed (object);

  self->desktop_settings = shell_status_settings_new ("org.gnome.desktop.interface");
  if (self->desktop_settings != NULL)
    g_signal_connect_object (self->desktop_settings,
                             "changed::" SHOW_BATTERY_PERCENTAGE_KEY,
                             G_CALLBACK (system_status_indicator_sync),
                             self, G_CONNECT_SWAPPED);

  indicator = shell_system_indicator_add_indicator (indicator_base);
  g_set_weak_pointer (&self->indicator, indicator);

  label = g_object_new (ST_TYPE_LABEL,
                        "y-expand", TRUE,
                        "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  g_set_weak_pointer (&self->percentage_label, ST_LABEL (label));
  clutter_actor_add_child (CLUTTER_ACTOR (self), label);
  st_widget_add_style_class_name (ST_WIDGET (self), "power-status");

  system_item = shell_system_item_new ();
  g_set_weak_pointer (&self->system_item, system_item);

  power_toggle = shell_system_item_get_power_toggle (system_item);

  g_object_bind_property (power_toggle, "title",
                          label, "text",
                          G_BINDING_SYNC_CREATE);

  g_signal_connect_object (power_toggle, "notify::visible",
                           G_CALLBACK (system_status_indicator_sync), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (power_toggle, "notify::gicon",
                           G_CALLBACK (system_status_indicator_sync), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (power_toggle, "notify::fallback-icon-name",
                           G_CALLBACK (system_status_indicator_sync), self, G_CONNECT_SWAPPED);

  g_signal_connect_object (system_item, "screenshot-ui-requested",
                           G_CALLBACK (system_status_indicator_screenshot_ui_requested_cb),
                           self, G_CONNECT_SWAPPED);

  shell_system_indicator_add_quick_settings_item (indicator_base, CLUTTER_ACTOR (system_item));

  system_status_indicator_sync (self);
}

static void
shell_system_status_indicator_dispose (GObject *object)
{
  ShellSystemStatusIndicator *self = SHELL_SYSTEM_STATUS_INDICATOR (object);

  g_clear_weak_pointer (&self->indicator);
  g_clear_weak_pointer (&self->percentage_label);
  g_clear_weak_pointer (&self->system_item);
  g_clear_object (&self->desktop_settings);

  G_OBJECT_CLASS (shell_system_status_indicator_parent_class)->dispose (object);
}

static void
shell_system_status_indicator_class_init (ShellSystemStatusIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_system_status_indicator_constructed;
  object_class->dispose = shell_system_status_indicator_dispose;

  /**
   * ShellSystemStatusIndicator::screenshot-ui-requested:
   * @self: the #ShellSystemStatusIndicator
   *
   * Emitted when the screenshot button of the quick settings asks for the
   * screenshot UI: the owner opens it (`Main.screenshotUI.open ()`).
   */
  indicator_signals[INDICATOR_SCREENSHOT_UI_REQUESTED] =
    g_signal_new ("screenshot-ui-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_system_status_indicator_init (ShellSystemStatusIndicator *self)
{
}

/**
 * shell_system_status_indicator_new:
 *
 * Creates the power indicator of the top bar, which shows the battery or the
 * power icon, and the row of buttons of the quick settings (`new Indicator ()`).
 *
 * Returns: (transfer floating): a new #ShellSystemStatusIndicator
 */
ShellSystemStatusIndicator *
shell_system_status_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_SYSTEM_STATUS_INDICATOR, NULL);
}
