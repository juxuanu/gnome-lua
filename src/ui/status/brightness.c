/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * brightness.c: The brightness slider of the quick settings, with a menu
 * that has one slider per monitor. Port of js/ui/status/brightness.js.
 *
 * `BrightnessSliderMenu` (a #ShellPopupMenuSection subclass whose only
 * method was addSlider ()) is brightness_item_add_slider (). The two-way
 * link between a slider and a brightness scale, which the JS wrote with
 * connectObject () and a re-entrancy flag, is a bidirectional #GBinding.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `Main.brightnessManager` is the construct-only property
 *    #ShellBrightnessIndicator:brightness-manager.
 *  - The scales do not need to be registered with `registerDestroyableType ()`:
 *    the bindings follow the lifetime of the scales and sliders.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include "ui/popup-menu.h"
#include "ui/quick-settings.h"
#include "ui/slider.h"
#include "ui/status/brightness.h"
#include "ui/status/status-private.h"

/* ======================================================================== */
/* ShellBrightnessItem                                                      */
/* ======================================================================== */

enum
{
  ITEM_PROP_0,

  ITEM_PROP_BRIGHTNESS_MANAGER,

  N_ITEM_PROPS
};

static GParamSpec *item_props[N_ITEM_PROPS] = { NULL, };

#define SHELL_TYPE_BRIGHTNESS_ITEM (shell_brightness_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellBrightnessItem, shell_brightness_item,
                      SHELL, BRIGHTNESS_ITEM, ShellQuickSlider)

struct _ShellBrightnessItem
{
  ShellQuickSlider parent;

  ShellBrightnessManager *manager;
  ShellPopupMenuSection *monitor_section;   /* weak */
  GBinding *global_binding;                 /* weak */
};

G_DEFINE_FINAL_TYPE (ShellBrightnessItem, shell_brightness_item, SHELL_TYPE_QUICK_SLIDER)

/* BrightnessSliderMenu.addSlider () */
static ShellSlider *
brightness_item_add_slider (ShellBrightnessItem  *self,
                            ShellBrightnessScale *scale)
{
  ShellPopupMenuBase *section = SHELL_POPUP_MENU_BASE (self->monitor_section);
  const char *name = shell_brightness_scale_get_name (scale);
  ShellSlider *slider;
  ClutterActor *slider_bin;
  ShellPopupBaseMenuItem *slider_item;
  ShellPopupMenuItem *text;

  text = g_object_new (SHELL_TYPE_POPUP_MENU_ITEM,
                       "text", name,
                       "reactive", FALSE,
                       "activatable", FALSE,
                       "active-on-hover", FALSE,
                       NULL);
  shell_popup_menu_base_add_menu_item (section, G_OBJECT (text));

  slider = shell_slider_new (0);
  clutter_actor_set_accessible_name (CLUTTER_ACTOR (slider), name);

  slider_bin = g_object_new (ST_TYPE_BIN,
                             "style-class", "slider-bin",
                             "child", slider,
                             "reactive", TRUE,
                             "can-focus", TRUE,
                             "x-expand", TRUE,
                             "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                             NULL);

  slider_item = g_object_new (SHELL_TYPE_POPUP_BASE_MENU_ITEM,
                              "reactive", FALSE,
                              "activatable", FALSE,
                              "active-on-hover", FALSE,
                              NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (slider_item), slider_bin);
  shell_popup_menu_base_add_menu_item (section, G_OBJECT (slider_item));

  return slider;
}

/* _connectSlider (): the slider follows the scale and the scale follows the
 * slider. The binding starts with the value of the scale. */
static GBinding *
brightness_item_connect_slider (ShellSlider          *slider,
                                ShellBrightnessScale *scale)
{
  return g_object_bind_property (scale, "value", slider, "value",
                                 G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
}

static void
brightness_item_sync (ShellBrightnessItem *self)
{
  ShellBrightnessScale *global_scale = NULL;
  g_autoptr (GList) scales = NULL;
  GList *l;
  gboolean visible;

  if (self->manager != NULL)
    {
      global_scale = shell_brightness_manager_get_global_scale (self->manager);
      scales = shell_brightness_manager_get_scales (self->manager);
    }

  visible = global_scale != NULL;
  shell_status_set_visible (self, visible);
  shell_quick_slider_set_menu_enabled (SHELL_QUICK_SLIDER (self),
                                       g_list_length (scales) > 1);

  if (!visible || self->monitor_section == NULL)
    return;

  shell_popup_menu_base_remove_all (SHELL_POPUP_MENU_BASE (self->monitor_section));

  /* slider.disconnectObject (scale) of the previous global scale */
  if (self->global_binding != NULL)
    g_binding_unbind (self->global_binding);
  g_set_weak_pointer (&self->global_binding,
                      brightness_item_connect_slider (shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (self)),
                                                      global_scale));

  for (l = scales; l != NULL; l = l->next)
    {
      ShellBrightnessScale *scale = l->data;
      ShellSlider *slider = brightness_item_add_slider (self, scale);

      brightness_item_connect_slider (slider, scale);
    }
}

static void
shell_brightness_item_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  ShellBrightnessItem *self = SHELL_BRIGHTNESS_ITEM (object);

  switch (prop_id)
    {
    case ITEM_PROP_BRIGHTNESS_MANAGER:
      g_value_set_object (value, self->manager);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_brightness_item_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  ShellBrightnessItem *self = SHELL_BRIGHTNESS_ITEM (object);

  switch (prop_id)
    {
    case ITEM_PROP_BRIGHTNESS_MANAGER:
      g_set_object (&self->manager, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_brightness_item_constructed (GObject *object)
{
  ShellBrightnessItem *self = SHELL_BRIGHTNESS_ITEM (object);
  ShellQuickSlider *quick_slider = SHELL_QUICK_SLIDER (object);
  ShellPopupMenuBase *menu;
  g_autoptr (ShellPopupMenuSection) section = NULL;

  G_OBJECT_CLASS (shell_brightness_item_parent_class)->constructed (object);

  menu = SHELL_POPUP_MENU_BASE (shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self)));

  clutter_actor_set_accessible_name (CLUTTER_ACTOR (shell_quick_slider_get_slider (quick_slider)),
                                     _("Brightness"));

  shell_quick_toggle_menu_set_header (SHELL_QUICK_TOGGLE_MENU (menu),
                                      "display-brightness-symbolic",
                                      _("Brightness"), NULL);

  section = shell_popup_menu_section_new ();
  shell_popup_menu_base_add_menu_item (menu, G_OBJECT (section));
  g_set_weak_pointer (&self->monitor_section, section);

  if (self->manager != NULL)
    g_signal_connect_object (self->manager, "changed",
                             G_CALLBACK (brightness_item_sync), self,
                             G_CONNECT_SWAPPED);
  brightness_item_sync (self);
}

static void
shell_brightness_item_dispose (GObject *object)
{
  ShellBrightnessItem *self = SHELL_BRIGHTNESS_ITEM (object);

  if (self->manager != NULL)
    g_signal_handlers_disconnect_by_data (self->manager, self);
  g_clear_weak_pointer (&self->monitor_section);
  g_clear_weak_pointer (&self->global_binding);
  g_clear_object (&self->manager);

  G_OBJECT_CLASS (shell_brightness_item_parent_class)->dispose (object);
}

static void
shell_brightness_item_class_init (ShellBrightnessItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_brightness_item_constructed;
  object_class->dispose = shell_brightness_item_dispose;
  object_class->get_property = shell_brightness_item_get_property;
  object_class->set_property = shell_brightness_item_set_property;

  item_props[ITEM_PROP_BRIGHTNESS_MANAGER] =
    g_param_spec_object ("brightness-manager", NULL, NULL,
                         SHELL_TYPE_BRIGHTNESS_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_ITEM_PROPS, item_props);
}

static void
shell_brightness_item_init (ShellBrightnessItem *self)
{
}

static ShellBrightnessItem *
shell_brightness_item_new (ShellBrightnessManager *manager)
{
  return g_object_new (SHELL_TYPE_BRIGHTNESS_ITEM,
                       "icon-name", "display-brightness-symbolic",
                       "menu-button-accessible-name", _("Open brightness menu"),
                       "brightness-manager", manager,
                       NULL);
}

/* ======================================================================== */
/* ShellBrightnessIndicator                                                 */
/* ======================================================================== */

enum
{
  PROP_0,

  PROP_BRIGHTNESS_MANAGER,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

struct _ShellBrightnessIndicator
{
  ShellSystemIndicator parent;

  ShellBrightnessManager *manager;
};

G_DEFINE_FINAL_TYPE (ShellBrightnessIndicator, shell_brightness_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
shell_brightness_indicator_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  ShellBrightnessIndicator *self = SHELL_BRIGHTNESS_INDICATOR (object);

  switch (prop_id)
    {
    case PROP_BRIGHTNESS_MANAGER:
      g_value_set_object (value, self->manager);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_brightness_indicator_set_property (GObject      *object,
                                         guint         prop_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
  ShellBrightnessIndicator *self = SHELL_BRIGHTNESS_INDICATOR (object);

  switch (prop_id)
    {
    case PROP_BRIGHTNESS_MANAGER:
      g_set_object (&self->manager, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_brightness_indicator_constructed (GObject *object)
{
  ShellBrightnessIndicator *self = SHELL_BRIGHTNESS_INDICATOR (object);

  G_OBJECT_CLASS (shell_brightness_indicator_parent_class)->constructed (object);

  shell_system_indicator_add_quick_settings_item (SHELL_SYSTEM_INDICATOR (self),
                                                  CLUTTER_ACTOR (shell_brightness_item_new (self->manager)));
}

static void
shell_brightness_indicator_dispose (GObject *object)
{
  ShellBrightnessIndicator *self = SHELL_BRIGHTNESS_INDICATOR (object);

  g_clear_object (&self->manager);

  G_OBJECT_CLASS (shell_brightness_indicator_parent_class)->dispose (object);
}

static void
shell_brightness_indicator_class_init (ShellBrightnessIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_brightness_indicator_constructed;
  object_class->dispose = shell_brightness_indicator_dispose;
  object_class->get_property = shell_brightness_indicator_get_property;
  object_class->set_property = shell_brightness_indicator_set_property;

  /**
   * ShellBrightnessIndicator:brightness-manager:
   *
   * The brightness manager of the shell (`Main.brightnessManager`).
   */
  props[PROP_BRIGHTNESS_MANAGER] =
    g_param_spec_object ("brightness-manager", NULL, NULL,
                         SHELL_TYPE_BRIGHTNESS_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_brightness_indicator_init (ShellBrightnessIndicator *self)
{
}

/**
 * shell_brightness_indicator_new:
 * @brightness_manager: (nullable): the brightness manager of the shell
 *
 * Creates the indicator with the brightness slider (`new Indicator ()`).
 * Without a brightness manager the slider stays hidden.
 *
 * Returns: (transfer floating): a new #ShellBrightnessIndicator
 */
ShellBrightnessIndicator *
shell_brightness_indicator_new (ShellBrightnessManager *brightness_manager)
{
  g_return_val_if_fail (brightness_manager == NULL ||
                        SHELL_IS_BRIGHTNESS_MANAGER (brightness_manager), NULL);

  return g_object_new (SHELL_TYPE_BRIGHTNESS_INDICATOR,
                       "brightness-manager", brightness_manager,
                       NULL);
}
