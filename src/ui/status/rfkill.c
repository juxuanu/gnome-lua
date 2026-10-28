/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * rfkill.c: The airplane mode of gnome-settings-daemon, its quick settings
 * toggle and the status icon which shows while it is on. Port of
 * js/ui/status/rfkill.js.
 *
 * The org.gnome.SettingsDaemon.Rfkill proxy is a gdbus-codegen proxy
 * (`rfkill-dbus`, type ShellSettingsDaemonRfkill). Without the service the
 * manager reports no airplane mode, quietly.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `airplane-mode`, `hw-airplane-mode` and `show-airplane-mode` are also
 *    notified when the service takes them out of its cache (it leaves the
 *    bus), where the JS only followed the changed values.
 *  - After ShellGlobal::shutdown the proxy is dropped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>

#include "shell-global.h"
#include "ui/quick-settings.h"
#include "ui/status/rfkill.h"
#include "ui/status/status-private.h"

#include "rfkill-dbus.h"

#define BUS_NAME "org.gnome.SettingsDaemon.Rfkill"
#define OBJECT_PATH "/org/gnome/SettingsDaemon/Rfkill"

/* ======================================================================== */
/* ShellRfkillManager                                                       */
/* ======================================================================== */

enum
{
  PROP_0,

  PROP_AIRPLANE_MODE,
  PROP_HW_AIRPLANE_MODE,
  PROP_SHOW_AIRPLANE_MODE,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellRfkillManager
{
  GObject parent;

  ShellSettingsDaemonRfkill *proxy;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (ShellRfkillManager, shell_rfkill_manager, G_TYPE_OBJECT)

static ShellRfkillManager *default_instance = NULL;

/**
 * shell_rfkill_manager_get_airplane_mode:
 * @self: a #ShellRfkillManager
 *
 * Returns: whether the airplane mode is on (`airplane_mode`)
 */
gboolean
shell_rfkill_manager_get_airplane_mode (ShellRfkillManager *self)
{
  g_return_val_if_fail (SHELL_IS_RFKILL_MANAGER (self), FALSE);

  if (self->proxy == NULL)
    return FALSE;

  return shell_settings_daemon_rfkill_get_airplane_mode (self->proxy);
}

/**
 * shell_rfkill_manager_set_airplane_mode:
 * @self: a #ShellRfkillManager
 * @airplane_mode: whether to turn the airplane mode on
 *
 * Asks the service to turn the airplane mode on or off. #ShellRfkillManager:airplane-mode
 * changes when the service has done it.
 */
void
shell_rfkill_manager_set_airplane_mode (ShellRfkillManager *self,
                                        gboolean            airplane_mode)
{
  g_return_if_fail (SHELL_IS_RFKILL_MANAGER (self));

  if (self->proxy == NULL)
    return;

  shell_settings_daemon_rfkill_set_airplane_mode (self->proxy, airplane_mode);
}

/**
 * shell_rfkill_manager_get_hw_airplane_mode:
 * @self: a #ShellRfkillManager
 *
 * Returns: whether the airplane mode is on because of a hardware switch
 *   (`hw_airplane_mode`)
 */
gboolean
shell_rfkill_manager_get_hw_airplane_mode (ShellRfkillManager *self)
{
  g_return_val_if_fail (SHELL_IS_RFKILL_MANAGER (self), FALSE);

  if (self->proxy == NULL)
    return FALSE;

  return shell_settings_daemon_rfkill_get_hardware_airplane_mode (self->proxy);
}

/**
 * shell_rfkill_manager_get_show_airplane_mode:
 * @self: a #ShellRfkillManager
 *
 * Returns: whether the airplane mode should be offered
 *   (`show_airplane_mode`)
 */
gboolean
shell_rfkill_manager_get_show_airplane_mode (ShellRfkillManager *self)
{
  g_return_val_if_fail (SHELL_IS_RFKILL_MANAGER (self), FALSE);

  if (self->proxy == NULL)
    return FALSE;

  return shell_settings_daemon_rfkill_get_has_airplane_mode (self->proxy) &&
         shell_settings_daemon_rfkill_get_should_show_airplane_mode (self->proxy);
}

static void
rfkill_manager_notify_property (ShellRfkillManager *self,
                                const char         *dbus_property)
{
  if (strcmp (dbus_property, "AirplaneMode") == 0)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_AIRPLANE_MODE]);
  else if (strcmp (dbus_property, "HardwareAirplaneMode") == 0)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HW_AIRPLANE_MODE]);
  else if (strcmp (dbus_property, "HasAirplaneMode") == 0 ||
           strcmp (dbus_property, "ShouldShowAirplaneMode") == 0)
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SHOW_AIRPLANE_MODE]);
}

static void
proxy_properties_changed_cb (GDBusProxy         *proxy,
                             GVariant           *changed_properties,
                             const char * const *invalidated_properties,
                             ShellRfkillManager *self)
{
  GVariantIter iter;
  const char *name;
  guint i;

  g_object_freeze_notify (G_OBJECT (self));

  g_variant_iter_init (&iter, changed_properties);
  while (g_variant_iter_next (&iter, "{&sv}", &name, NULL))
    rfkill_manager_notify_property (self, name);

  for (i = 0; invalidated_properties[i] != NULL; i++)
    rfkill_manager_notify_property (self, invalidated_properties[i]);

  g_object_thaw_notify (G_OBJECT (self));
}

static void
proxy_ready_cb (GObject      *source,
                GAsyncResult *res,
                gpointer      user_data)
{
  g_autoptr (ShellRfkillManager) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (ShellSettingsDaemonRfkill) proxy = NULL;

  proxy = shell_settings_daemon_rfkill_proxy_new_for_bus_finish (res, &error);

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

  /* The values were not there before the proxy */
  g_object_freeze_notify (G_OBJECT (self));
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_AIRPLANE_MODE]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HW_AIRPLANE_MODE]);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SHOW_AIRPLANE_MODE]);
  g_object_thaw_notify (G_OBJECT (self));
}

static void
rfkill_manager_release (ShellRfkillManager *self)
{
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->proxy);
}

static void
on_shutdown (ShellGlobal        *global,
             ShellRfkillManager *self)
{
  rfkill_manager_release (self);
}

static void
shell_rfkill_manager_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  ShellRfkillManager *self = SHELL_RFKILL_MANAGER (object);

  switch (prop_id)
    {
    case PROP_AIRPLANE_MODE:
      g_value_set_boolean (value, shell_rfkill_manager_get_airplane_mode (self));
      break;

    case PROP_HW_AIRPLANE_MODE:
      g_value_set_boolean (value, shell_rfkill_manager_get_hw_airplane_mode (self));
      break;

    case PROP_SHOW_AIRPLANE_MODE:
      g_value_set_boolean (value, shell_rfkill_manager_get_show_airplane_mode (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_rfkill_manager_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  ShellRfkillManager *self = SHELL_RFKILL_MANAGER (object);

  switch (prop_id)
    {
    case PROP_AIRPLANE_MODE:
      shell_rfkill_manager_set_airplane_mode (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_rfkill_manager_constructed (GObject *object)
{
  ShellRfkillManager *self = SHELL_RFKILL_MANAGER (object);

  G_OBJECT_CLASS (shell_rfkill_manager_parent_class)->constructed (object);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  self->cancellable = g_cancellable_new ();
  shell_settings_daemon_rfkill_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  BUS_NAME, OBJECT_PATH,
                                                  self->cancellable,
                                                  proxy_ready_cb,
                                                  g_object_ref (self));
}

static void
shell_rfkill_manager_dispose (GObject *object)
{
  ShellRfkillManager *self = SHELL_RFKILL_MANAGER (object);

  rfkill_manager_release (self);

  G_OBJECT_CLASS (shell_rfkill_manager_parent_class)->dispose (object);
}

static void
shell_rfkill_manager_class_init (ShellRfkillManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_rfkill_manager_constructed;
  object_class->dispose = shell_rfkill_manager_dispose;
  object_class->get_property = shell_rfkill_manager_get_property;
  object_class->set_property = shell_rfkill_manager_set_property;

  /**
   * ShellRfkillManager:airplane-mode:
   *
   * Whether the airplane mode is on (`airplane-mode`).
   */
  props[PROP_AIRPLANE_MODE] =
    g_param_spec_boolean ("airplane-mode", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellRfkillManager:hw-airplane-mode:
   *
   * Whether the airplane mode is on because of a hardware switch.
   */
  props[PROP_HW_AIRPLANE_MODE] =
    g_param_spec_boolean ("hw-airplane-mode", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellRfkillManager:show-airplane-mode:
   *
   * Whether the airplane mode should be offered to the user.
   */
  props[PROP_SHOW_AIRPLANE_MODE] =
    g_param_spec_boolean ("show-airplane-mode", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_rfkill_manager_init (ShellRfkillManager *self)
{
}

/**
 * shell_rfkill_manager_get_default:
 *
 * Gets the connection to the rfkill service of gnome-settings-daemon,
 * which is created on the first call (`getRfkillManager ()`).
 *
 * Returns: (transfer none): the #ShellRfkillManager singleton
 */
ShellRfkillManager *
shell_rfkill_manager_get_default (void)
{
  if (default_instance == NULL)
    default_instance = g_object_new (SHELL_TYPE_RFKILL_MANAGER, NULL);

  return default_instance;
}

/* ======================================================================== */
/* ShellRfkillToggle                                                        */
/* ======================================================================== */

#define SHELL_TYPE_RFKILL_TOGGLE (shell_rfkill_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellRfkillToggle, shell_rfkill_toggle,
                      SHELL, RFKILL_TOGGLE, ShellQuickToggle)

struct _ShellRfkillToggle
{
  ShellQuickToggle parent;

  ShellRfkillManager *manager;
};

G_DEFINE_FINAL_TYPE (ShellRfkillToggle, shell_rfkill_toggle, SHELL_TYPE_QUICK_TOGGLE)

static void
shell_rfkill_toggle_clicked (StButton *button,
                             int       clicked_button)
{
  ShellRfkillToggle *self = SHELL_RFKILL_TOGGLE (button);

  shell_rfkill_manager_set_airplane_mode (self->manager,
                                          !shell_rfkill_manager_get_airplane_mode (self->manager));
}

static void
shell_rfkill_toggle_class_init (ShellRfkillToggleClass *klass)
{
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  button_class->clicked = shell_rfkill_toggle_clicked;
}

static void
shell_rfkill_toggle_init (ShellRfkillToggle *self)
{
  self->manager = shell_rfkill_manager_get_default ();

  g_object_bind_property (self->manager, "show-airplane-mode",
                          self, "visible",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->manager, "airplane-mode",
                          self, "checked",
                          G_BINDING_SYNC_CREATE);
}

static ShellRfkillToggle *
shell_rfkill_toggle_new (void)
{
  return g_object_new (SHELL_TYPE_RFKILL_TOGGLE,
                       "title", _("Airplane Mode"),
                       "icon-name", "airplane-mode-symbolic",
                       NULL);
}

/* ======================================================================== */
/* ShellRfkillIndicator                                                     */
/* ======================================================================== */

struct _ShellRfkillIndicator
{
  ShellSystemIndicator parent;

  StIcon *icon;                 /* weak */
  ShellRfkillToggle *toggle;    /* weak */
};

G_DEFINE_FINAL_TYPE (ShellRfkillIndicator, shell_rfkill_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
rfkill_indicator_sync (ShellRfkillIndicator *self)
{
  if (self->icon == NULL || self->toggle == NULL)
    return;

  /* Only show indicator when airplane mode is on */
  shell_status_set_visible (self->icon,
                            clutter_actor_is_visible (CLUTTER_ACTOR (self->toggle)) &&
                            st_button_get_checked (ST_BUTTON (self->toggle)));
}

static void
shell_rfkill_indicator_constructed (GObject *object)
{
  ShellRfkillIndicator *self = SHELL_RFKILL_INDICATOR (object);
  ShellSystemIndicator *indicator = SHELL_SYSTEM_INDICATOR (object);
  StIcon *icon;
  ShellRfkillToggle *toggle;

  G_OBJECT_CLASS (shell_rfkill_indicator_parent_class)->constructed (object);

  icon = shell_system_indicator_add_indicator (indicator);
  g_set_weak_pointer (&self->icon, icon);
  st_icon_set_icon_name (icon, "airplane-mode-symbolic");

  toggle = shell_rfkill_toggle_new ();
  g_set_weak_pointer (&self->toggle, toggle);
  g_signal_connect_object (self->toggle, "notify::visible",
                           G_CALLBACK (rfkill_indicator_sync), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->toggle, "notify::checked",
                           G_CALLBACK (rfkill_indicator_sync), self, G_CONNECT_SWAPPED);
  shell_system_indicator_add_quick_settings_item (indicator, CLUTTER_ACTOR (self->toggle));

  rfkill_indicator_sync (self);
}

static void
shell_rfkill_indicator_dispose (GObject *object)
{
  ShellRfkillIndicator *self = SHELL_RFKILL_INDICATOR (object);

  if (self->toggle != NULL)
    g_signal_handlers_disconnect_by_data (self->toggle, self);
  g_clear_weak_pointer (&self->icon);
  g_clear_weak_pointer (&self->toggle);

  G_OBJECT_CLASS (shell_rfkill_indicator_parent_class)->dispose (object);
}

static void
shell_rfkill_indicator_class_init (ShellRfkillIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_rfkill_indicator_constructed;
  object_class->dispose = shell_rfkill_indicator_dispose;
}

static void
shell_rfkill_indicator_init (ShellRfkillIndicator *self)
{
}

/**
 * shell_rfkill_indicator_new:
 *
 * Creates the indicator with the "Airplane Mode" toggle and the icon which
 * shows while the airplane mode is on (`new Indicator ()`).
 *
 * Returns: (transfer floating): a new #ShellRfkillIndicator
 */
ShellRfkillIndicator *
shell_rfkill_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_RFKILL_INDICATOR, NULL);
}
