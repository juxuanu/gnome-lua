/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auto-rotate.c: The "Auto Rotate" quick settings toggle. Port of
 * js/ui/status/autoRotate.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - A missing org.gnome.settings-daemon.peripherals.touchscreen schema
 *    leaves the toggle unbound (it stays unchecked) instead of throwing.
 *  - A failing orientation lock is logged as a warning (the JS handler
 *    threw an exception which nobody caught).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include "misc/system-actions.h"
#include "ui/quick-settings.h"
#include "ui/status/auto-rotate.h"
#include "ui/status/status-private.h"

#define TOUCHSCREEN_SCHEMA "org.gnome.settings-daemon.peripherals.touchscreen"
#define KEY_ORIENTATION_LOCK "orientation-lock"

/* ------------------------------------------------------------------------ */
/* ShellRotationToggle                                                      */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_ROTATION_TOGGLE (shell_rotation_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellRotationToggle, shell_rotation_toggle,
                      SHELL, ROTATION_TOGGLE, ShellQuickToggle)

struct _ShellRotationToggle
{
  ShellQuickToggle parent;

  ShellSystemActions *system_actions;
};

G_DEFINE_FINAL_TYPE (ShellRotationToggle, shell_rotation_toggle, SHELL_TYPE_QUICK_TOGGLE)

static void
shell_rotation_toggle_clicked (StButton *button,
                               int       clicked_button)
{
  ShellRotationToggle *self = SHELL_ROTATION_TOGGLE (button);
  g_autoptr (GError) error = NULL;

  if (!shell_system_actions_activate_lock_orientation (self->system_actions, &error))
    g_warning ("Failed to lock the orientation: %s", error->message);
}

static void
shell_rotation_toggle_class_init (ShellRotationToggleClass *klass)
{
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  button_class->clicked = shell_rotation_toggle_clicked;
}

static void
shell_rotation_toggle_init (ShellRotationToggle *self)
{
  g_autoptr (GSettings) settings = NULL;

  self->system_actions = shell_system_actions_get_default ();

  g_object_bind_property (self->system_actions, "can-lock-orientation",
                          self, "visible",
                          G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->system_actions, "orientation-lock-icon",
                          self, "icon-name",
                          G_BINDING_DEFAULT | G_BINDING_SYNC_CREATE);

  /* The binding is dropped with the toggle */
  settings = shell_status_settings_new (TOUCHSCREEN_SCHEMA);
  if (settings != NULL)
    g_settings_bind (settings, KEY_ORIENTATION_LOCK,
                     self, "checked",
                     G_SETTINGS_BIND_DEFAULT | G_SETTINGS_BIND_INVERT_BOOLEAN);
}

static ShellRotationToggle *
shell_rotation_toggle_new (void)
{
  return g_object_new (SHELL_TYPE_ROTATION_TOGGLE,
                       "title", _("Auto Rotate"),
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellAutoRotateIndicator                                                 */
/* ------------------------------------------------------------------------ */

struct _ShellAutoRotateIndicator
{
  ShellSystemIndicator parent;
};

G_DEFINE_FINAL_TYPE (ShellAutoRotateIndicator, shell_auto_rotate_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
shell_auto_rotate_indicator_constructed (GObject *object)
{
  ShellSystemIndicator *indicator = SHELL_SYSTEM_INDICATOR (object);

  G_OBJECT_CLASS (shell_auto_rotate_indicator_parent_class)->constructed (object);

  shell_system_indicator_add_quick_settings_item (indicator,
                                                  CLUTTER_ACTOR (shell_rotation_toggle_new ()));
}

static void
shell_auto_rotate_indicator_class_init (ShellAutoRotateIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_auto_rotate_indicator_constructed;
}

static void
shell_auto_rotate_indicator_init (ShellAutoRotateIndicator *self)
{
}

/**
 * shell_auto_rotate_indicator_new:
 *
 * Creates the indicator with the "Auto Rotate" toggle (`new Indicator ()`).
 *
 * Returns: (transfer floating): a new #ShellAutoRotateIndicator
 */
ShellAutoRotateIndicator *
shell_auto_rotate_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_AUTO_ROTATE_INDICATOR, NULL);
}
