/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dark-mode.c: The "Dark Style" quick settings toggle. Port of
 * js/ui/status/darkMode.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - A missing org.gnome.desktop.interface schema leaves the toggle without
 *    a setting (it stays unchecked and does nothing) instead of throwing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include "ui/layout.h"
#include "ui/main.h"
#include "ui/quick-settings.h"
#include "ui/status/dark-mode.h"
#include "ui/status/status-private.h"

#define INTERFACE_SCHEMA "org.gnome.desktop.interface"
#define KEY_COLOR_SCHEME "color-scheme"

/* ------------------------------------------------------------------------ */
/* ShellDarkModeToggle                                                      */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_DARK_MODE_TOGGLE (shell_dark_mode_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellDarkModeToggle, shell_dark_mode_toggle,
                      SHELL, DARK_MODE_TOGGLE, ShellQuickToggle)

struct _ShellDarkModeToggle
{
  ShellQuickToggle parent;

  GSettings *settings;
};

G_DEFINE_FINAL_TYPE (ShellDarkModeToggle, shell_dark_mode_toggle, SHELL_TYPE_QUICK_TOGGLE)

static void
dark_mode_toggle_sync (ShellDarkModeToggle *self)
{
  g_autofree char *color_scheme = NULL;
  gboolean checked;

  if (self->settings == NULL)
    return;

  color_scheme = g_settings_get_string (self->settings, KEY_COLOR_SCHEME);
  checked = g_strcmp0 (color_scheme, "prefer-dark") == 0;

  if (st_button_get_checked (ST_BUTTON (self)) != checked)
    st_button_set_checked (ST_BUTTON (self), checked);
}

static void
shell_dark_mode_toggle_clicked (StButton *button,
                                int       clicked_button)
{
  ShellDarkModeToggle *self = SHELL_DARK_MODE_TOGGLE (button);
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());

  if (layout_manager != NULL)
    shell_screen_transition_run (shell_layout_manager_get_screen_transition (layout_manager));

  if (self->settings != NULL)
    g_settings_set_string (self->settings, KEY_COLOR_SCHEME,
                           st_button_get_checked (button) ? "default" : "prefer-dark");
}

static void
shell_dark_mode_toggle_dispose (GObject *object)
{
  ShellDarkModeToggle *self = SHELL_DARK_MODE_TOGGLE (object);

  /* connectObject ('destroy', () => this._settings.run_dispose ()) */
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (shell_dark_mode_toggle_parent_class)->dispose (object);
}

static void
shell_dark_mode_toggle_class_init (ShellDarkModeToggleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->dispose = shell_dark_mode_toggle_dispose;

  button_class->clicked = shell_dark_mode_toggle_clicked;
}

static void
shell_dark_mode_toggle_init (ShellDarkModeToggle *self)
{
  self->settings = shell_status_settings_new (INTERFACE_SCHEMA);

  if (self->settings != NULL)
    g_signal_connect_object (self->settings, "changed::" KEY_COLOR_SCHEME,
                             G_CALLBACK (dark_mode_toggle_sync), self,
                             G_CONNECT_SWAPPED);

  dark_mode_toggle_sync (self);
}

static ShellDarkModeToggle *
shell_dark_mode_toggle_new (void)
{
  return g_object_new (SHELL_TYPE_DARK_MODE_TOGGLE,
                       "title", _("Dark Style"),
                       "icon-name", "dark-mode-symbolic",
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellDarkModeIndicator                                                   */
/* ------------------------------------------------------------------------ */

struct _ShellDarkModeIndicator
{
  ShellSystemIndicator parent;
};

G_DEFINE_FINAL_TYPE (ShellDarkModeIndicator, shell_dark_mode_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
shell_dark_mode_indicator_constructed (GObject *object)
{
  ShellSystemIndicator *indicator = SHELL_SYSTEM_INDICATOR (object);

  G_OBJECT_CLASS (shell_dark_mode_indicator_parent_class)->constructed (object);

  shell_system_indicator_add_quick_settings_item (indicator,
                                                  CLUTTER_ACTOR (shell_dark_mode_toggle_new ()));
}

static void
shell_dark_mode_indicator_class_init (ShellDarkModeIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_dark_mode_indicator_constructed;
}

static void
shell_dark_mode_indicator_init (ShellDarkModeIndicator *self)
{
}

/**
 * shell_dark_mode_indicator_new:
 *
 * Creates the indicator with the "Dark Style" toggle (`new Indicator ()`).
 *
 * Returns: (transfer floating): a new #ShellDarkModeIndicator
 */
ShellDarkModeIndicator *
shell_dark_mode_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_DARK_MODE_INDICATOR, NULL);
}
