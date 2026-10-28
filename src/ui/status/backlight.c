/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * backlight.c: The "Keyboard" quick settings toggle, with the menu to set the
 * brightness of the keyboard backlight. Port of js/ui/status/backlight.js.
 *
 * The org.gnome.SettingsDaemon.Power.Keyboard proxy is a gdbus-codegen proxy
 * (`backlight-dbus`, type ShellSettingsDaemonPowerKeyboard). Without the
 * service the toggle stays hidden, quietly. `SliderItem` is
 * ShellBacklightSliderItem, `DiscreteItem` is ShellBacklightDiscreteItem and
 * `KeyboardBrightnessToggle` is ShellKeyboardBrightnessToggle.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The brightness the slider sets is rounded to a whole percent (the JS
 *    handed the D-Bus proxy a fraction for an `i` property).
 *  - The button of a discrete level for a backlight with a single level
 *    selects 0 (the JS computed NaN).
 *  - The number of levels is clamped to 1..3 before it is set on the
 *    `n-levels` property of the discrete item.
 *  - After ShellGlobal::shutdown the proxy is dropped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>

#include <glib/gi18n-lib.h>

#include "shell-global.h"
#include "ui/popup-menu.h"
#include "ui/quick-settings.h"
#include "ui/slider.h"
#include "ui/status/backlight.h"
#include "ui/status/status-private.h"

#include "backlight-dbus.h"

#define BUS_NAME "org.gnome.SettingsDaemon.Power"
#define OBJECT_PATH "/org/gnome/SettingsDaemon/Power"

/* ======================================================================== */
/* ShellBacklightSliderItem                                                 */
/* ======================================================================== */

enum
{
  SLIDER_ITEM_PROP_0,

  SLIDER_ITEM_PROP_VALUE,

  N_SLIDER_ITEM_PROPS
};

static GParamSpec *slider_item_props[N_SLIDER_ITEM_PROPS] = { NULL, };

#define SHELL_TYPE_BACKLIGHT_SLIDER_ITEM (shell_backlight_slider_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellBacklightSliderItem, shell_backlight_slider_item,
                      SHELL, BACKLIGHT_SLIDER_ITEM, ShellPopupBaseMenuItem)

struct _ShellBacklightSliderItem
{
  ShellPopupBaseMenuItem parent;

  ShellSlider *slider;      /* weak */
};

G_DEFINE_FINAL_TYPE (ShellBacklightSliderItem, shell_backlight_slider_item,
                     SHELL_TYPE_POPUP_BASE_MENU_ITEM)

static int
slider_item_get_value (ShellBacklightSliderItem *self)
{
  if (self->slider == NULL)
    return 0;

  return (int) round (shell_bar_level_get_value (SHELL_BAR_LEVEL (self->slider)) * 100);
}

static void
slider_item_set_value (ShellBacklightSliderItem *self,
                       int                       value)
{
  if (self->slider == NULL || slider_item_get_value (self) == value)
    return;

  /* The slider notifies, which notifies the item once */
  shell_bar_level_set_value (SHELL_BAR_LEVEL (self->slider), value / 100.0);
}

static void
slider_item_slider_notify_value_cb (GObject                  *slider,
                                    GParamSpec               *pspec,
                                    ShellBacklightSliderItem *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), slider_item_props[SLIDER_ITEM_PROP_VALUE]);
}

static gboolean
shell_backlight_slider_item_navigate_focus (StWidget         *widget,
                                            ClutterActor     *from,
                                            StDirectionType   direction)
{
  ShellBacklightSliderItem *self = SHELL_BACKLIGHT_SLIDER_ITEM (widget);

  if (self->slider == NULL)
    return FALSE;

  return st_widget_navigate_focus (ST_WIDGET (self->slider), from, direction, FALSE);
}

static void
shell_backlight_slider_item_get_property (GObject    *object,
                                          guint       prop_id,
                                          GValue     *value,
                                          GParamSpec *pspec)
{
  ShellBacklightSliderItem *self = SHELL_BACKLIGHT_SLIDER_ITEM (object);

  switch (prop_id)
    {
    case SLIDER_ITEM_PROP_VALUE:
      g_value_set_int (value, slider_item_get_value (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_backlight_slider_item_set_property (GObject      *object,
                                          guint         prop_id,
                                          const GValue *value,
                                          GParamSpec   *pspec)
{
  ShellBacklightSliderItem *self = SHELL_BACKLIGHT_SLIDER_ITEM (object);

  switch (prop_id)
    {
    case SLIDER_ITEM_PROP_VALUE:
      slider_item_set_value (self, g_value_get_int (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_backlight_slider_item_constructed (GObject *object)
{
  ShellBacklightSliderItem *self = SHELL_BACKLIGHT_SLIDER_ITEM (object);
  ShellSlider *slider;

  G_OBJECT_CLASS (shell_backlight_slider_item_parent_class)->constructed (object);

  slider = shell_slider_new (0);
  g_set_weak_pointer (&self->slider, slider);

  g_signal_connect_object (slider, "notify::value",
                           G_CALLBACK (slider_item_slider_notify_value_cb), self, 0);
  clutter_actor_set_accessible_name (CLUTTER_ACTOR (slider), _("Keyboard Brightness"));

  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (slider));
}

static void
shell_backlight_slider_item_dispose (GObject *object)
{
  ShellBacklightSliderItem *self = SHELL_BACKLIGHT_SLIDER_ITEM (object);

  if (self->slider != NULL)
    g_signal_handlers_disconnect_by_data (self->slider, self);
  g_clear_weak_pointer (&self->slider);

  G_OBJECT_CLASS (shell_backlight_slider_item_parent_class)->dispose (object);
}

static void
shell_backlight_slider_item_class_init (ShellBacklightSliderItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);

  object_class->constructed = shell_backlight_slider_item_constructed;
  object_class->dispose = shell_backlight_slider_item_dispose;
  object_class->get_property = shell_backlight_slider_item_get_property;
  object_class->set_property = shell_backlight_slider_item_set_property;

  widget_class->navigate_focus = shell_backlight_slider_item_navigate_focus;

  slider_item_props[SLIDER_ITEM_PROP_VALUE] =
    g_param_spec_int ("value", NULL, NULL,
                      0, 100, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_SLIDER_ITEM_PROPS, slider_item_props);
}

static void
shell_backlight_slider_item_init (ShellBacklightSliderItem *self)
{
}

static ShellBacklightSliderItem *
shell_backlight_slider_item_new (void)
{
  return g_object_new (SHELL_TYPE_BACKLIGHT_SLIDER_ITEM,
                       "activatable", FALSE,
                       "extra-style-class", "keyboard-brightness-item",
                       NULL);
}

/* ======================================================================== */
/* ShellBacklightDiscreteItem                                               */
/* ======================================================================== */

enum
{
  DISCRETE_ITEM_PROP_0,

  DISCRETE_ITEM_PROP_VALUE,
  DISCRETE_ITEM_PROP_N_LEVELS,

  N_DISCRETE_ITEM_PROPS
};

static GParamSpec *discrete_item_props[N_DISCRETE_ITEM_PROPS] = { NULL, };

#define SHELL_TYPE_BACKLIGHT_DISCRETE_ITEM (shell_backlight_discrete_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellBacklightDiscreteItem, shell_backlight_discrete_item,
                      SHELL, BACKLIGHT_DISCRETE_ITEM, StBoxLayout)

typedef enum
{
  LEVEL_OFF,
  LEVEL_LOW,
  LEVEL_HIGH,
  N_LEVELS
} Level;

struct _ShellBacklightDiscreteItem
{
  StBoxLayout parent;

  int value;
  int n_levels;

  ClutterActor *level_boxes[N_LEVELS];      /* weak */
  StButton *level_buttons[N_LEVELS];        /* weak */
};

G_DEFINE_FINAL_TYPE (ShellBacklightDiscreteItem, shell_backlight_discrete_item,
                     ST_TYPE_BOX_LAYOUT)

/* _valueToLevel () */
static Level
discrete_item_value_to_level (ShellBacklightDiscreteItem *self)
{
  int checked_index = (int) round (self->value * (self->n_levels - 1) / 100.0);

  if (checked_index == self->n_levels - 1)
    return LEVEL_HIGH;

  return CLAMP (checked_index, 0, N_LEVELS - 1);
}

/* _levelToValue () */
static int
discrete_item_level_to_value (ShellBacklightDiscreteItem *self,
                              Level                       level)
{
  if (self->n_levels <= 1)
    return 0;

  return (int) (100 * MIN ((int) level, self->n_levels - 1) / (self->n_levels - 1));
}

/* _syncChecked () */
static void
discrete_item_sync_checked (ShellBacklightDiscreteItem *self)
{
  Level checked_level = discrete_item_value_to_level (self);
  int i;

  for (i = 0; i < N_LEVELS; i++)
    {
      if (self->level_buttons[i] != NULL)
        st_button_set_checked (self->level_buttons[i], i == (int) checked_level);
    }
}

/* _syncLevels () */
static void
discrete_item_sync_levels (ShellBacklightDiscreteItem *self)
{
  if (self->level_boxes[LEVEL_OFF] != NULL)
    shell_status_set_visible (self->level_boxes[LEVEL_OFF], self->n_levels > 0);
  if (self->level_boxes[LEVEL_HIGH] != NULL)
    shell_status_set_visible (self->level_boxes[LEVEL_HIGH], self->n_levels > 1);
  if (self->level_boxes[LEVEL_LOW] != NULL)
    shell_status_set_visible (self->level_boxes[LEVEL_LOW], self->n_levels > 2);

  discrete_item_sync_checked (self);
}

static void
discrete_item_set_value (ShellBacklightDiscreteItem *self,
                         int                         value)
{
  if (self->value == value)
    return;

  self->value = value;
  discrete_item_sync_checked (self);
  g_object_notify_by_pspec (G_OBJECT (self), discrete_item_props[DISCRETE_ITEM_PROP_VALUE]);
}

static void
discrete_item_set_n_levels (ShellBacklightDiscreteItem *self,
                            int                         n_levels)
{
  if (self->n_levels == n_levels)
    return;

  self->n_levels = n_levels;
  discrete_item_sync_levels (self);
  g_object_notify_by_pspec (G_OBJECT (self), discrete_item_props[DISCRETE_ITEM_PROP_N_LEVELS]);
}

static void
level_button_clicked_cb (StButton                   *button,
                         int                         clicked_button,
                         ShellBacklightDiscreteItem *self)
{
  Level level = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "level"));

  discrete_item_set_value (self, discrete_item_level_to_value (self, level));
}

/* _addLevelButton () */
static void
discrete_item_add_level_button (ShellBacklightDiscreteItem *self,
                                Level                       level,
                                const char                 *label_text,
                                const char                 *icon_name)
{
  ClutterActor *box;
  ClutterActor *label;
  StButton *button;

  box = g_object_new (ST_TYPE_BOX_LAYOUT,
                      "style-class", "keyboard-brightness-level",
                      "orientation", CLUTTER_ORIENTATION_VERTICAL,
                      "x-expand", TRUE,
                      NULL);

  label = g_object_new (ST_TYPE_LABEL,
                        "text", label_text,
                        "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);

  button = g_object_new (ST_TYPE_BUTTON,
                         "style-class", "icon-button",
                         "can-focus", TRUE,
                         "icon-name", icon_name,
                         NULL);
  st_widget_set_label_actor (ST_WIDGET (button), label);
  g_object_set_data (G_OBJECT (button), "level", GINT_TO_POINTER (level));
  g_signal_connect_object (button, "clicked",
                           G_CALLBACK (level_button_clicked_cb), self, 0);

  clutter_actor_add_child (box, CLUTTER_ACTOR (button));
  clutter_actor_add_child (box, label);
  clutter_actor_add_child (CLUTTER_ACTOR (self), box);

  g_set_weak_pointer (&self->level_boxes[level], box);
  g_set_weak_pointer (&self->level_buttons[level], button);
}

static void
shell_backlight_discrete_item_get_property (GObject    *object,
                                            guint       prop_id,
                                            GValue     *value,
                                            GParamSpec *pspec)
{
  ShellBacklightDiscreteItem *self = SHELL_BACKLIGHT_DISCRETE_ITEM (object);

  switch (prop_id)
    {
    case DISCRETE_ITEM_PROP_VALUE:
      g_value_set_int (value, self->value);
      break;

    case DISCRETE_ITEM_PROP_N_LEVELS:
      g_value_set_int (value, self->n_levels);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_backlight_discrete_item_set_property (GObject      *object,
                                            guint         prop_id,
                                            const GValue *value,
                                            GParamSpec   *pspec)
{
  ShellBacklightDiscreteItem *self = SHELL_BACKLIGHT_DISCRETE_ITEM (object);

  switch (prop_id)
    {
    case DISCRETE_ITEM_PROP_VALUE:
      discrete_item_set_value (self, g_value_get_int (value));
      break;

    case DISCRETE_ITEM_PROP_N_LEVELS:
      discrete_item_set_n_levels (self, g_value_get_int (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_backlight_discrete_item_constructed (GObject *object)
{
  ShellBacklightDiscreteItem *self = SHELL_BACKLIGHT_DISCRETE_ITEM (object);

  G_OBJECT_CLASS (shell_backlight_discrete_item_parent_class)->constructed (object);

  discrete_item_add_level_button (self, LEVEL_OFF, _("Off"),
                                  "keyboard-brightness-off-symbolic");
  discrete_item_add_level_button (self, LEVEL_LOW, _("Low"),
                                  "keyboard-brightness-medium-symbolic");
  discrete_item_add_level_button (self, LEVEL_HIGH, _("High"),
                                  "keyboard-brightness-high-symbolic");

  discrete_item_sync_levels (self);

  st_focus_manager_add_group (shell_global_get_focus_manager (shell_global_get ()),
                              ST_WIDGET (self));
}

static void
shell_backlight_discrete_item_dispose (GObject *object)
{
  ShellBacklightDiscreteItem *self = SHELL_BACKLIGHT_DISCRETE_ITEM (object);
  int i;

  for (i = 0; i < N_LEVELS; i++)
    {
      g_clear_weak_pointer (&self->level_boxes[i]);
      g_clear_weak_pointer (&self->level_buttons[i]);
    }

  G_OBJECT_CLASS (shell_backlight_discrete_item_parent_class)->dispose (object);
}

static void
shell_backlight_discrete_item_class_init (ShellBacklightDiscreteItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_backlight_discrete_item_constructed;
  object_class->dispose = shell_backlight_discrete_item_dispose;
  object_class->get_property = shell_backlight_discrete_item_get_property;
  object_class->set_property = shell_backlight_discrete_item_set_property;

  discrete_item_props[DISCRETE_ITEM_PROP_VALUE] =
    g_param_spec_int ("value", NULL, NULL,
                      0, 100, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  discrete_item_props[DISCRETE_ITEM_PROP_N_LEVELS] =
    g_param_spec_int ("n-levels", NULL, NULL,
                      1, 3, 1,
                      G_PARAM_READWRITE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_DISCRETE_ITEM_PROPS, discrete_item_props);
}

static void
shell_backlight_discrete_item_init (ShellBacklightDiscreteItem *self)
{
  self->value = 0;
  self->n_levels = 1;
}

static ShellBacklightDiscreteItem *
shell_backlight_discrete_item_new (void)
{
  return g_object_new (SHELL_TYPE_BACKLIGHT_DISCRETE_ITEM,
                       "style-class", "popup-menu-item",
                       "reactive", TRUE,
                       NULL);
}

/* ======================================================================== */
/* ShellKeyboardBrightnessToggle                                            */
/* ======================================================================== */

#define SHELL_TYPE_KEYBOARD_BRIGHTNESS_TOGGLE (shell_keyboard_brightness_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellKeyboardBrightnessToggle, shell_keyboard_brightness_toggle,
                      SHELL, KEYBOARD_BRIGHTNESS_TOGGLE, ShellQuickMenuToggle)

struct _ShellKeyboardBrightnessToggle
{
  ShellQuickMenuToggle parent;

  ShellSettingsDaemonPowerKeyboard *proxy;
  GCancellable *cancellable;

  ShellBacklightSliderItem *slider_item;        /* weak */
  ShellBacklightDiscreteItem *discrete_item;    /* weak */
  gulong slider_item_changed_id;
  gulong discrete_item_changed_id;
};

G_DEFINE_FINAL_TYPE (ShellKeyboardBrightnessToggle, shell_keyboard_brightness_toggle,
                     SHELL_TYPE_QUICK_MENU_TOGGLE)

static void
toggle_set_brightness (ShellKeyboardBrightnessToggle *self,
                       int                            brightness)
{
  if (self->proxy != NULL)
    shell_settings_daemon_power_keyboard_set_brightness (self->proxy, brightness);
}

static void
toggle_sync (ShellKeyboardBrightnessToggle *self)
{
  g_autoptr (GVariant) cached = NULL;
  int brightness;
  gboolean visible;
  gboolean use_slider;
  int steps;

  if (self->proxy != NULL)
    cached = g_dbus_proxy_get_cached_property (G_DBUS_PROXY (self->proxy), "Brightness");

  /* Number.isInteger (brightness) && brightness >= 0 */
  brightness = cached != NULL && g_variant_is_of_type (cached, G_VARIANT_TYPE_INT32)
    ? g_variant_get_int32 (cached) : -1;
  visible = brightness >= 0;
  shell_status_set_visible (self, visible);
  if (!visible || self->slider_item == NULL || self->discrete_item == NULL)
    return;

  st_button_set_checked (ST_BUTTON (self), brightness > 0);
  steps = shell_settings_daemon_power_keyboard_get_steps (self->proxy);
  use_slider = steps >= 4;

  g_signal_handler_block (self->slider_item, self->slider_item_changed_id);
  g_signal_handler_block (self->discrete_item, self->discrete_item_changed_id);

  shell_status_set_visible (self->slider_item, use_slider);
  g_object_set (self->slider_item, "value", brightness, NULL);

  if (!use_slider)
    g_object_set (self->discrete_item, "n-levels", CLAMP (steps, 1, 3), NULL);

  g_signal_handler_unblock (self->slider_item, self->slider_item_changed_id);
  g_signal_handler_unblock (self->discrete_item, self->discrete_item_changed_id);
}

static void
slider_item_value_changed_cb (ShellBacklightSliderItem     *item,
                              GParamSpec                   *pspec,
                              ShellKeyboardBrightnessToggle *self)
{
  if (clutter_actor_is_visible (CLUTTER_ACTOR (item)))
    toggle_set_brightness (self, slider_item_get_value (item));
}

static void
discrete_item_value_changed_cb (ShellBacklightDiscreteItem   *item,
                                GParamSpec                   *pspec,
                                ShellKeyboardBrightnessToggle *self)
{
  if (clutter_actor_is_visible (CLUTTER_ACTOR (item)))
    toggle_set_brightness (self, item->value);
}

static void
proxy_properties_changed_cb (GDBusProxy                    *proxy,
                             GVariant                      *changed_properties,
                             const char * const            *invalidated_properties,
                             ShellKeyboardBrightnessToggle *self)
{
  toggle_sync (self);
}

static void
proxy_ready_cb (GObject      *source,
                GAsyncResult *res,
                gpointer      user_data)
{
  g_autoptr (ShellKeyboardBrightnessToggle) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (ShellSettingsDaemonPowerKeyboard) proxy = NULL;

  proxy = shell_settings_daemon_power_keyboard_proxy_new_for_bus_finish (res, &error);

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

  toggle_sync (self);
}

static void
toggle_release (ShellKeyboardBrightnessToggle *self)
{
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->proxy);
}

static void
on_shutdown (ShellGlobal                   *global,
             ShellKeyboardBrightnessToggle *self)
{
  toggle_release (self);
}

static void
shell_keyboard_brightness_toggle_clicked (StButton *button,
                                          int       clicked_button)
{
  ShellKeyboardBrightnessToggle *self = SHELL_KEYBOARD_BRIGHTNESS_TOGGLE (button);

  toggle_set_brightness (self, st_button_get_checked (button) ? 0 : 100);
}

static void
shell_keyboard_brightness_toggle_constructed (GObject *object)
{
  ShellKeyboardBrightnessToggle *self = SHELL_KEYBOARD_BRIGHTNESS_TOGGLE (object);
  ShellPopupMenuBase *menu;
  ClutterActor *box;
  AtkObject *slider_accessible;
  ShellBacklightSliderItem *slider_item;
  ShellBacklightDiscreteItem *discrete_item;

  G_OBJECT_CLASS (shell_keyboard_brightness_toggle_parent_class)->constructed (object);

  menu = SHELL_POPUP_MENU_BASE (shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self)));

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  box = CLUTTER_ACTOR (shell_popup_menu_base_get_box (menu));

  slider_item = shell_backlight_slider_item_new ();
  g_set_weak_pointer (&self->slider_item, slider_item);
  clutter_actor_add_child (box, CLUTTER_ACTOR (slider_item));
  slider_accessible = clutter_actor_get_accessible (CLUTTER_ACTOR (slider_item->slider));
  atk_object_set_parent (slider_accessible, clutter_actor_get_accessible (box));
  clutter_actor_set_accessible (CLUTTER_ACTOR (slider_item), slider_accessible);

  discrete_item = shell_backlight_discrete_item_new ();
  g_set_weak_pointer (&self->discrete_item, discrete_item);
  clutter_actor_add_child (box, CLUTTER_ACTOR (discrete_item));

  g_object_bind_property (slider_item, "visible",
                          discrete_item, "visible",
                          G_BINDING_INVERT_BOOLEAN | G_BINDING_SYNC_CREATE);
  g_object_bind_property (slider_item, "value",
                          discrete_item, "value",
                          G_BINDING_SYNC_CREATE);

  self->slider_item_changed_id =
    g_signal_connect_object (slider_item, "notify::value",
                             G_CALLBACK (slider_item_value_changed_cb), self, 0);
  self->discrete_item_changed_id =
    g_signal_connect_object (discrete_item, "notify::value",
                             G_CALLBACK (discrete_item_value_changed_cb), self, 0);

  self->cancellable = g_cancellable_new ();
  shell_settings_daemon_power_keyboard_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                          G_DBUS_PROXY_FLAGS_NONE,
                                                          BUS_NAME, OBJECT_PATH,
                                                          self->cancellable,
                                                          proxy_ready_cb,
                                                          g_object_ref (self));
}

static void
shell_keyboard_brightness_toggle_dispose (GObject *object)
{
  ShellKeyboardBrightnessToggle *self = SHELL_KEYBOARD_BRIGHTNESS_TOGGLE (object);

  toggle_release (self);
  if (self->slider_item != NULL)
    g_signal_handlers_disconnect_by_data (self->slider_item, self);
  if (self->discrete_item != NULL)
    g_signal_handlers_disconnect_by_data (self->discrete_item, self);
  g_clear_weak_pointer (&self->slider_item);
  g_clear_weak_pointer (&self->discrete_item);

  G_OBJECT_CLASS (shell_keyboard_brightness_toggle_parent_class)->dispose (object);
}

static void
shell_keyboard_brightness_toggle_class_init (ShellKeyboardBrightnessToggleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->constructed = shell_keyboard_brightness_toggle_constructed;
  object_class->dispose = shell_keyboard_brightness_toggle_dispose;

  button_class->clicked = shell_keyboard_brightness_toggle_clicked;
}

static void
shell_keyboard_brightness_toggle_init (ShellKeyboardBrightnessToggle *self)
{
}

static ShellKeyboardBrightnessToggle *
shell_keyboard_brightness_toggle_new (void)
{
  return g_object_new (SHELL_TYPE_KEYBOARD_BRIGHTNESS_TOGGLE,
                       "title", _("Keyboard"),
                       "icon-name", "display-brightness-symbolic",
                       "menu-button-accessible-name", _("Open keyboard brightness menu"),
                       NULL);
}

/* ======================================================================== */
/* ShellBacklightIndicator                                                  */
/* ======================================================================== */

struct _ShellBacklightIndicator
{
  ShellSystemIndicator parent;
};

G_DEFINE_FINAL_TYPE (ShellBacklightIndicator, shell_backlight_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
shell_backlight_indicator_constructed (GObject *object)
{
  ShellSystemIndicator *indicator = SHELL_SYSTEM_INDICATOR (object);

  G_OBJECT_CLASS (shell_backlight_indicator_parent_class)->constructed (object);

  shell_system_indicator_add_quick_settings_item (indicator,
                                                  CLUTTER_ACTOR (shell_keyboard_brightness_toggle_new ()));
}

static void
shell_backlight_indicator_class_init (ShellBacklightIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_backlight_indicator_constructed;
}

static void
shell_backlight_indicator_init (ShellBacklightIndicator *self)
{
}

/**
 * shell_backlight_indicator_new:
 *
 * Creates the indicator with the "Keyboard" toggle that sets the brightness
 * of the keyboard backlight (`new Indicator ()`).
 *
 * Returns: (transfer floating): a new #ShellBacklightIndicator
 */
ShellBacklightIndicator *
shell_backlight_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_BACKLIGHT_INDICATOR, NULL);
}
