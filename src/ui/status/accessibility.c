/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * accessibility.c: The accessibility menu of the top bar and the quick
 * settings toggles for the accessibility settings. Port of
 * js/ui/status/accessibility.js.
 *
 * The toggles that only bind `checked` to a boolean key (all of them except
 * the large text one) are defined by the SETTINGS_TOGGLE () macro, one final
 * #ShellQuickToggle subclass each, so that they are distinct GTypes for the
 * login screen.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - A GSettings schema that is not installed leaves the item or the toggle
 *    without its setting (it stays off and does nothing) instead of
 *    throwing.
 *  - After ShellGlobal::shutdown the queued visibility update is dropped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include "shell-global.h"
#include "ui/popup-menu.h"
#include "ui/status/accessibility.h"
#include "ui/status/status-private.h"

#define A11Y_SCHEMA                   "org.gnome.desktop.a11y"
#define KEY_ALWAYS_SHOW               "always-show-universal-access-status"

#define A11Y_KEYBOARD_SCHEMA          "org.gnome.desktop.a11y.keyboard"
#define KEY_STICKY_KEYS_ENABLED       "stickykeys-enable"
#define KEY_BOUNCE_KEYS_ENABLED       "bouncekeys-enable"
#define KEY_SLOW_KEYS_ENABLED         "slowkeys-enable"
#define KEY_MOUSE_KEYS_ENABLED        "mousekeys-enable"

#define APPLICATIONS_SCHEMA           "org.gnome.desktop.a11y.applications"
#define KEY_SCREEN_MAGNIFIER          "screen-magnifier-enabled"
#define KEY_SCREEN_READER             "screen-reader-enabled"
#define KEY_SCREEN_KEYBOARD           "screen-keyboard-enabled"

#define DPI_FACTOR_LARGE              1.25

#define WM_SCHEMA                     "org.gnome.desktop.wm.preferences"
#define KEY_VISUAL_BELL               "visual-bell"

#define DESKTOP_INTERFACE_SCHEMA      "org.gnome.desktop.interface"
#define KEY_TEXT_SCALING_FACTOR       "text-scaling-factor"

#define A11Y_INTERFACE_SCHEMA         "org.gnome.desktop.a11y.interface"
#define KEY_HIGH_CONTRAST             "high-contrast"

/* ======================================================================== */
/* ShellATIndicator                                                         */
/* ======================================================================== */

struct _ShellATIndicator
{
  ShellPanelMenuButton parent;

  GSettings *a11y_settings;
  GSettings *interface_settings;
  ShellPopupSwitchMenuItem *font_item;      /* weak */
  guint sync_menu_visibility_idle;
};

G_DEFINE_FINAL_TYPE (ShellATIndicator, shell_at_indicator, SHELL_TYPE_PANEL_MENU_BUTTON)

/* _syncMenuVisibility () */
static void
at_indicator_sync_menu_visibility (ShellATIndicator *self)
{
  ShellPopupMenuBase *menu = SHELL_POPUP_MENU_BASE (shell_panel_menu_button_get_menu (SHELL_PANEL_MENU_BUTTON (self)));
  g_autoptr (GList) items = NULL;
  gboolean always_show = FALSE;
  gboolean any_state = FALSE;
  GList *l;

  self->sync_menu_visibility_idle = 0;

  if (self->a11y_settings != NULL)
    always_show = g_settings_get_boolean (self->a11y_settings, KEY_ALWAYS_SHOW);

  items = shell_popup_menu_base_get_menu_items (menu);
  for (l = items; l != NULL && !any_state; l = l->next)
    {
      /* The separator and the settings action have no `state` */
      if (SHELL_IS_POPUP_SWITCH_MENU_ITEM (l->data))
        any_state = shell_popup_switch_menu_item_get_state (SHELL_POPUP_SWITCH_MENU_ITEM (l->data));
    }

  shell_status_set_visible (self, always_show || any_state);
}

static void
at_indicator_sync_menu_visibility_idle_cb (gpointer user_data)
{
  at_indicator_sync_menu_visibility (user_data);
}

/* _queueSyncMenuVisibility () */
static void
at_indicator_queue_sync_menu_visibility (ShellATIndicator *self)
{
  if (self->sync_menu_visibility_idle != 0)
    return;

  self->sync_menu_visibility_idle =
    g_idle_add_once (at_indicator_sync_menu_visibility_idle_cb, self);
  g_source_set_name_by_id (self->sync_menu_visibility_idle,
                           "[gnome-shell] this._syncMenuVisibility");
}

/* _buildItem () */
static ShellPopupSwitchMenuItem *
at_indicator_build_item (ShellATIndicator *self,
                         const char       *string,
                         const char       *schema,
                         const char       *key)
{
  g_autoptr (GSettings) settings = shell_status_settings_new (schema);
  ShellPopupSwitchMenuItem *widget;

  widget = g_object_new (SHELL_TYPE_POPUP_SWITCH_MENU_ITEM,
                         "text", string,
                         "state", FALSE,
                         NULL);
  /* The binding is dropped with the item */
  if (settings != NULL)
    g_settings_bind (settings, key, widget, "state", G_SETTINGS_BIND_DEFAULT);

  g_signal_connect_object (widget, "toggled",
                           G_CALLBACK (at_indicator_queue_sync_menu_visibility),
                           self, G_CONNECT_SWAPPED);

  return widget;
}

static void
font_item_toggled_cb (ShellPopupSwitchMenuItem *item,
                      gboolean                  state,
                      ShellATIndicator         *self)
{
  if (self->interface_settings == NULL)
    return;

  if (state)
    g_settings_set_double (self->interface_settings, KEY_TEXT_SCALING_FACTOR, DPI_FACTOR_LARGE);
  else
    g_settings_reset (self->interface_settings, KEY_TEXT_SCALING_FACTOR);
}

static void
font_settings_changed_cb (GSettings        *settings,
                          const char       *key,
                          ShellATIndicator *self)
{
  double factor = g_settings_get_double (settings, KEY_TEXT_SCALING_FACTOR);
  gboolean active = factor > 1.0;

  if (self->font_item != NULL)
    {
      g_signal_handlers_block_by_func (self->font_item, font_item_toggled_cb, self);
      shell_popup_switch_menu_item_set_toggle_state (self->font_item, active);
      g_signal_handlers_unblock_by_func (self->font_item, font_item_toggled_cb, self);
    }

  at_indicator_queue_sync_menu_visibility (self);
}

/* _buildFontItem () */
static ShellPopupSwitchMenuItem *
at_indicator_build_font_item (ShellATIndicator *self)
{
  ShellPopupSwitchMenuItem *widget;
  gboolean state = FALSE;

  self->interface_settings = shell_status_settings_new (DESKTOP_INTERFACE_SCHEMA);
  if (self->interface_settings != NULL)
    state = g_settings_get_double (self->interface_settings, KEY_TEXT_SCALING_FACTOR) > 1.0;

  widget = g_object_new (SHELL_TYPE_POPUP_SWITCH_MENU_ITEM,
                         "text", _("Large Text"),
                         "state", state,
                         NULL);

  g_set_weak_pointer (&self->font_item, widget);
  g_signal_connect_object (widget, "toggled",
                           G_CALLBACK (font_item_toggled_cb), self, 0);

  if (self->interface_settings != NULL)
    {
      g_signal_connect_object (self->interface_settings,
                               "changed::" KEY_TEXT_SCALING_FACTOR,
                               G_CALLBACK (font_settings_changed_cb), self, 0);
      g_settings_bind_writable (self->interface_settings, KEY_TEXT_SCALING_FACTOR,
                                widget, "sensitive", FALSE);
    }

  return widget;
}

static void
on_shutdown (ShellGlobal      *global,
             ShellATIndicator *self)
{
  g_clear_handle_id (&self->sync_menu_visibility_idle, g_source_remove);
}

static GObject *
shell_at_indicator_constructor (GType                  type,
                                guint                  n_construct_properties,
                                GObjectConstructParam *construct_properties)
{
  guint i;

  /* `super._init ({menu_alignment: 0.5, ...})`: JS creates the button with
   * no property, so the alignment is forced here, before the menu is made */
  for (i = 0; i < n_construct_properties; i++)
    {
      if (g_strcmp0 (g_param_spec_get_name (construct_properties[i].pspec),
                     "menu-alignment") == 0)
        g_value_set_double (construct_properties[i].value, 0.5);
    }

  return G_OBJECT_CLASS (shell_at_indicator_parent_class)->constructor (type,
                                                                        n_construct_properties,
                                                                        construct_properties);
}

static void
shell_at_indicator_constructed (GObject *object)
{
  ShellATIndicator *self = SHELL_AT_INDICATOR (object);
  ShellPopupMenuBase *menu;
  g_autoptr (ShellPopupSeparatorMenuItem) separator = NULL;

  G_OBJECT_CLASS (shell_at_indicator_parent_class)->constructed (object);

  clutter_actor_set_accessible_name (CLUTTER_ACTOR (self), _("Accessibility"));

  clutter_actor_add_child (CLUTTER_ACTOR (self),
                           g_object_new (ST_TYPE_ICON,
                                         "style-class", "system-status-icon",
                                         "icon-name", "accessibility-menu-symbolic",
                                         NULL));

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  self->a11y_settings = shell_status_settings_new (A11Y_SCHEMA);
  if (self->a11y_settings != NULL)
    g_signal_connect_object (self->a11y_settings, "changed::" KEY_ALWAYS_SHOW,
                             G_CALLBACK (at_indicator_queue_sync_menu_visibility),
                             self, G_CONNECT_SWAPPED);

  menu = SHELL_POPUP_MENU_BASE (shell_panel_menu_button_get_menu (SHELL_PANEL_MENU_BUTTON (self)));

#define ADD_ITEM(item) \
  shell_popup_menu_base_add_menu_item (menu, G_OBJECT (item))

  ADD_ITEM (at_indicator_build_item (self, _("High Contrast"),
                                     A11Y_INTERFACE_SCHEMA, KEY_HIGH_CONTRAST));
  ADD_ITEM (at_indicator_build_item (self, _("Zoom"),
                                     APPLICATIONS_SCHEMA, KEY_SCREEN_MAGNIFIER));
  ADD_ITEM (at_indicator_build_font_item (self));
  ADD_ITEM (at_indicator_build_item (self, _("Screen Reader"),
                                     APPLICATIONS_SCHEMA, KEY_SCREEN_READER));
  ADD_ITEM (at_indicator_build_item (self, _("Screen Keyboard"),
                                     APPLICATIONS_SCHEMA, KEY_SCREEN_KEYBOARD));
  ADD_ITEM (at_indicator_build_item (self, _("Visual Alerts"),
                                     WM_SCHEMA, KEY_VISUAL_BELL));
  ADD_ITEM (at_indicator_build_item (self, _("Sticky Keys"),
                                     A11Y_KEYBOARD_SCHEMA, KEY_STICKY_KEYS_ENABLED));
  ADD_ITEM (at_indicator_build_item (self, _("Slow Keys"),
                                     A11Y_KEYBOARD_SCHEMA, KEY_SLOW_KEYS_ENABLED));
  ADD_ITEM (at_indicator_build_item (self, _("Bounce Keys"),
                                     A11Y_KEYBOARD_SCHEMA, KEY_BOUNCE_KEYS_ENABLED));
  ADD_ITEM (at_indicator_build_item (self, _("Mouse Keys"),
                                     A11Y_KEYBOARD_SCHEMA, KEY_MOUSE_KEYS_ENABLED));

#undef ADD_ITEM

  separator = g_object_ref_sink (g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL));
  shell_popup_menu_base_add_menu_item (menu, G_OBJECT (separator));
  shell_popup_menu_base_add_settings_action (menu, _("Accessibility Settings"),
                                             "gnome-universal-access-panel.desktop");

  at_indicator_sync_menu_visibility (self);
}

static void
shell_at_indicator_dispose (GObject *object)
{
  ShellATIndicator *self = SHELL_AT_INDICATOR (object);

  g_clear_handle_id (&self->sync_menu_visibility_idle, g_source_remove);
  g_clear_weak_pointer (&self->font_item);
  g_clear_object (&self->a11y_settings);
  g_clear_object (&self->interface_settings);

  G_OBJECT_CLASS (shell_at_indicator_parent_class)->dispose (object);
}

static void
shell_at_indicator_class_init (ShellATIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructor = shell_at_indicator_constructor;
  object_class->constructed = shell_at_indicator_constructed;
  object_class->dispose = shell_at_indicator_dispose;
}

static void
shell_at_indicator_init (ShellATIndicator *self)
{
}

/**
 * shell_at_indicator_new:
 *
 * Creates the accessibility menu button of the top bar
 * (`new ATIndicator ()`).
 *
 * Returns: (transfer floating): a new #ShellATIndicator
 */
ShellATIndicator *
shell_at_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_AT_INDICATOR, NULL);
}

/* ======================================================================== */
/* The toggles                                                              */
/* ======================================================================== */

/* settings.bind (key, toggle, 'checked', Gio.SettingsBindFlags.DEFAULT) */
static void
bind_settings_toggle (ShellQuickToggle *toggle,
                      const char       *schema_id,
                      const char       *key)
{
  g_autoptr (GSettings) settings = shell_status_settings_new (schema_id);

  /* The binding is dropped with the toggle */
  if (settings != NULL)
    g_settings_bind (settings, key, toggle, "checked", G_SETTINGS_BIND_DEFAULT);
}

/*
 * Defines a final toggle with a title and an icon whose `checked` property
 * is bound to a boolean GSettings key. The login screen (and anyone else)
 * creates the toggles with `new Shell.HighContrastToggle ()`, without any
 * property, so the title and the icon are set when the toggle is constructed.
 * Callers write the documentation of the shell_*_toggle_new () function
 * above the use of the macro.
 */
#define SETTINGS_TOGGLE(TypeName, type_name, title, icon_name, schema_id, key)  \
struct _##TypeName                                                              \
{                                                                               \
  ShellQuickToggle parent;                                                      \
};                                                                              \
                                                                                \
G_DEFINE_FINAL_TYPE (TypeName, type_name, SHELL_TYPE_QUICK_TOGGLE)              \
                                                                                \
static void                                                                     \
type_name##_constructed (GObject *object)                                       \
{                                                                               \
  ShellQuickToggle *toggle = SHELL_QUICK_TOGGLE (object);                       \
                                                                                \
  G_OBJECT_CLASS (type_name##_parent_class)->constructed (object);              \
                                                                                \
  /* JS creates the toggles without any property */                            \
  g_object_set (toggle,                                                         \
                "title", title,                                                 \
                "icon-name", icon_name,                                         \
                "toggle-mode", TRUE,                                            \
                NULL);                                                          \
  bind_settings_toggle (toggle, schema_id, key);                                \
}                                                                               \
                                                                                \
static void                                                                     \
type_name##_class_init (TypeName##Class *klass)                                 \
{                                                                               \
  G_OBJECT_CLASS (klass)->constructed = type_name##_constructed;                \
}                                                                               \
                                                                                \
static void                                                                     \
type_name##_init (TypeName *self)                                               \
{                                                                               \
}                                                                               \
                                                                                \
TypeName *                                                                      \
type_name##_new (void)                                                          \
{                                                                               \
  return g_object_new (type_name##_get_type (), NULL);                          \
}

/**
 * shell_high_contrast_toggle_new:
 *
 * Creates the "High Contrast" toggle (`new HighContrastToggle ()`).
 *
 * Returns: (transfer floating): a new #ShellHighContrastToggle
 */
SETTINGS_TOGGLE (ShellHighContrastToggle, shell_high_contrast_toggle,
                 _("High Contrast"), "accessibility-high-contrast-symbolic",
                 A11Y_INTERFACE_SCHEMA, KEY_HIGH_CONTRAST)

/**
 * shell_magnifier_toggle_new:
 *
 * Creates the "Zoom" toggle (`new MagnifierToggle ()`).
 *
 * Returns: (transfer floating): a new #ShellMagnifierToggle
 */
SETTINGS_TOGGLE (ShellMagnifierToggle, shell_magnifier_toggle,
                 _("Zoom"), "accessibility-zoom-symbolic",
                 APPLICATIONS_SCHEMA, KEY_SCREEN_MAGNIFIER)

/**
 * shell_screen_reader_toggle_new:
 *
 * Creates the "Screen Reader" toggle (`new ScreenReaderToggle ()`).
 *
 * Returns: (transfer floating): a new #ShellScreenReaderToggle
 */
SETTINGS_TOGGLE (ShellScreenReaderToggle, shell_screen_reader_toggle,
                 _("Screen Reader"), "accessibility-screen-reader-symbolic",
                 APPLICATIONS_SCHEMA, KEY_SCREEN_READER)

/**
 * shell_screen_keyboard_toggle_new:
 *
 * Creates the "Screen Keyboard" toggle (`new ScreenKeyboardToggle ()`).
 *
 * Returns: (transfer floating): a new #ShellScreenKeyboardToggle
 */
SETTINGS_TOGGLE (ShellScreenKeyboardToggle, shell_screen_keyboard_toggle,
                 _("Screen Keyboard"), "input-keyboard-symbolic",
                 APPLICATIONS_SCHEMA, KEY_SCREEN_KEYBOARD)

/**
 * shell_visual_bell_toggle_new:
 *
 * Creates the "Visual Alerts" toggle (`new VisualBellToggle ()`).
 *
 * Returns: (transfer floating): a new #ShellVisualBellToggle
 */
SETTINGS_TOGGLE (ShellVisualBellToggle, shell_visual_bell_toggle,
                 _("Visual Alerts"), "accessibility-visual-alerts-symbolic",
                 WM_SCHEMA, KEY_VISUAL_BELL)

/**
 * shell_sticky_keys_toggle_new:
 *
 * Creates the "Sticky Keys" toggle (`new StickyKeysToggle ()`).
 *
 * Returns: (transfer floating): a new #ShellStickyKeysToggle
 */
SETTINGS_TOGGLE (ShellStickyKeysToggle, shell_sticky_keys_toggle,
                 _("Sticky Keys"), "accessibility-sticky-keys-symbolic",
                 A11Y_KEYBOARD_SCHEMA, KEY_STICKY_KEYS_ENABLED)

/**
 * shell_slow_keys_toggle_new:
 *
 * Creates the "Slow Keys" toggle (`new SlowKeysToggle ()`).
 *
 * Returns: (transfer floating): a new #ShellSlowKeysToggle
 */
SETTINGS_TOGGLE (ShellSlowKeysToggle, shell_slow_keys_toggle,
                 _("Slow Keys"), "accessibility-slow-keys-symbolic",
                 A11Y_KEYBOARD_SCHEMA, KEY_SLOW_KEYS_ENABLED)

/**
 * shell_bounce_keys_toggle_new:
 *
 * Creates the "Bounce Keys" toggle (`new BounceKeysToggle ()`).
 *
 * Returns: (transfer floating): a new #ShellBounceKeysToggle
 */
SETTINGS_TOGGLE (ShellBounceKeysToggle, shell_bounce_keys_toggle,
                 _("Bounce Keys"), "accessibility-bounce-keys-symbolic",
                 A11Y_KEYBOARD_SCHEMA, KEY_BOUNCE_KEYS_ENABLED)

/**
 * shell_mouse_keys_toggle_new:
 *
 * Creates the "Mouse Keys" toggle (`new MouseKeysToggle ()`).
 *
 * Returns: (transfer floating): a new #ShellMouseKeysToggle
 */
SETTINGS_TOGGLE (ShellMouseKeysToggle, shell_mouse_keys_toggle,
                 _("Mouse Keys"), "accessibility-mouse-keys-symbolic",
                 A11Y_KEYBOARD_SCHEMA, KEY_MOUSE_KEYS_ENABLED)

/* ------------------------------------------------------------------------ */
/* ShellLargeTextToggle                                                     */
/* ------------------------------------------------------------------------ */

struct _ShellLargeTextToggle
{
  ShellQuickToggle parent;

  GSettings *settings;
  gulong toggled_id;
};

G_DEFINE_FINAL_TYPE (ShellLargeTextToggle, shell_large_text_toggle, SHELL_TYPE_QUICK_TOGGLE)

/* _updateChecked () */
static void
large_text_toggle_update_checked (ShellLargeTextToggle *self)
{
  gboolean checked = FALSE;

  if (self->settings != NULL)
    checked = g_settings_get_double (self->settings, KEY_TEXT_SCALING_FACTOR) > 1.0;

  st_button_set_checked (ST_BUTTON (self), checked);
}

static void
large_text_toggle_notify_checked_cb (GObject              *object,
                                     GParamSpec           *pspec,
                                     ShellLargeTextToggle *self)
{
  if (self->settings == NULL)
    return;

  if (st_button_get_checked (ST_BUTTON (self)))
    g_settings_set_double (self->settings, KEY_TEXT_SCALING_FACTOR, DPI_FACTOR_LARGE);
  else
    g_settings_reset (self->settings, KEY_TEXT_SCALING_FACTOR);
}

static void
large_text_toggle_settings_changed_cb (GSettings            *settings,
                                       const char           *key,
                                       ShellLargeTextToggle *self)
{
  g_signal_handler_block (self, self->toggled_id);
  large_text_toggle_update_checked (self);
  g_signal_handler_unblock (self, self->toggled_id);
}

static void
shell_large_text_toggle_constructed (GObject *object)
{
  G_OBJECT_CLASS (shell_large_text_toggle_parent_class)->constructed (object);

  /* JS creates the toggles without any property */
  g_object_set (object,
                "title", _("Large Text"),
                "icon-name", "accessibility-large-text-symbolic",
                "toggle-mode", TRUE,
                NULL);
}

static void
shell_large_text_toggle_dispose (GObject *object)
{
  ShellLargeTextToggle *self = SHELL_LARGE_TEXT_TOGGLE (object);

  g_clear_object (&self->settings);

  G_OBJECT_CLASS (shell_large_text_toggle_parent_class)->dispose (object);
}

static void
shell_large_text_toggle_class_init (ShellLargeTextToggleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_large_text_toggle_constructed;
  object_class->dispose = shell_large_text_toggle_dispose;
}

static void
shell_large_text_toggle_init (ShellLargeTextToggle *self)
{
  self->settings = shell_status_settings_new (DESKTOP_INTERFACE_SCHEMA);
  large_text_toggle_update_checked (self);

  self->toggled_id = g_signal_connect (self, "notify::checked",
                                       G_CALLBACK (large_text_toggle_notify_checked_cb), self);

  if (self->settings != NULL)
    {
      g_signal_connect_object (self->settings, "changed::" KEY_TEXT_SCALING_FACTOR,
                               G_CALLBACK (large_text_toggle_settings_changed_cb), self, 0);
      g_settings_bind_writable (self->settings, KEY_TEXT_SCALING_FACTOR,
                                self, "reactive", FALSE);
    }
}

/**
 * shell_large_text_toggle_new:
 *
 * Creates the "Large Text" toggle (`new LargeTextToggle ()`).
 *
 * Returns: (transfer floating): a new #ShellLargeTextToggle
 */
ShellLargeTextToggle *
shell_large_text_toggle_new (void)
{
  return g_object_new (SHELL_TYPE_LARGE_TEXT_TOGGLE, NULL);
}
