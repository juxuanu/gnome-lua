/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * do-not-disturb.c: The "Do Not Disturb" quick settings toggle and its
 * status icon. Port of js/ui/status/doNotDisturb.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - A missing org.gnome.desktop.notifications schema leaves the toggle
 *    unbound (it stays unchecked) instead of throwing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include "ui/quick-settings.h"
#include "ui/status/do-not-disturb.h"
#include "ui/status/status-private.h"

#define NOTIFICATIONS_SCHEMA "org.gnome.desktop.notifications"
#define KEY_SHOW_BANNERS "show-banners"

/* ------------------------------------------------------------------------ */
/* ShellDoNotDisturbToggle                                                  */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_DO_NOT_DISTURB_TOGGLE (shell_do_not_disturb_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellDoNotDisturbToggle, shell_do_not_disturb_toggle,
                      SHELL, DO_NOT_DISTURB_TOGGLE, ShellQuickToggle)

struct _ShellDoNotDisturbToggle
{
  ShellQuickToggle parent;
};

G_DEFINE_FINAL_TYPE (ShellDoNotDisturbToggle, shell_do_not_disturb_toggle,
                     SHELL_TYPE_QUICK_TOGGLE)

static void
shell_do_not_disturb_toggle_class_init (ShellDoNotDisturbToggleClass *klass)
{
}

static void
shell_do_not_disturb_toggle_init (ShellDoNotDisturbToggle *self)
{
  g_autoptr (GSettings) settings = shell_status_settings_new (NOTIFICATIONS_SCHEMA);

  /* The binding is dropped with the toggle */
  if (settings != NULL)
    g_settings_bind (settings, KEY_SHOW_BANNERS,
                     self, "checked",
                     G_SETTINGS_BIND_DEFAULT | G_SETTINGS_BIND_INVERT_BOOLEAN);
}

static ShellDoNotDisturbToggle *
shell_do_not_disturb_toggle_new (void)
{
  return g_object_new (SHELL_TYPE_DO_NOT_DISTURB_TOGGLE,
                       "title", _("Do Not Disturb"),
                       "icon-name", "notifications-disabled-symbolic",
                       "toggle-mode", TRUE,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellDoNotDisturbIndicator                                               */
/* ------------------------------------------------------------------------ */

struct _ShellDoNotDisturbIndicator
{
  ShellSystemIndicator parent;
};

G_DEFINE_FINAL_TYPE (ShellDoNotDisturbIndicator, shell_do_not_disturb_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
shell_do_not_disturb_indicator_constructed (GObject *object)
{
  ShellSystemIndicator *indicator = SHELL_SYSTEM_INDICATOR (object);
  StIcon *icon;
  ShellDoNotDisturbToggle *toggle;

  G_OBJECT_CLASS (shell_do_not_disturb_indicator_parent_class)->constructed (object);

  icon = shell_system_indicator_add_indicator (indicator);
  st_icon_set_icon_name (icon, "notifications-disabled-symbolic");

  toggle = shell_do_not_disturb_toggle_new ();
  g_object_bind_property (toggle, "checked", icon, "visible", G_BINDING_SYNC_CREATE);

  shell_system_indicator_add_quick_settings_item (indicator, CLUTTER_ACTOR (toggle));
}

static void
shell_do_not_disturb_indicator_class_init (ShellDoNotDisturbIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_do_not_disturb_indicator_constructed;
}

static void
shell_do_not_disturb_indicator_init (ShellDoNotDisturbIndicator *self)
{
}

/**
 * shell_do_not_disturb_indicator_new:
 *
 * Creates the indicator with the "Do Not Disturb" toggle and the icon which
 * shows while it is on (`new Indicator ()`).
 *
 * Returns: (transfer floating): a new #ShellDoNotDisturbIndicator
 */
ShellDoNotDisturbIndicator *
shell_do_not_disturb_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_DO_NOT_DISTURB_INDICATOR, NULL);
}
