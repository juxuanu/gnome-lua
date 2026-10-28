/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * dwell-click.c: The menu of the top bar which chooses the kind of click the
 * dwell click accessibility feature performs. Port of
 * js/ui/status/dwellClick.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - A GSettings schema that is not installed leaves the indicator hidden
 *    instead of throwing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include "ui/popup-menu.h"
#include "ui/status/dwell-click.h"
#include "ui/status/status-private.h"

#define MOUSE_A11Y_SCHEMA       "org.gnome.desktop.a11y.mouse"
#define KEY_DWELL_CLICK_ENABLED "dwell-click-enabled"
#define KEY_DWELL_MODE          "dwell-mode"
#define DWELL_MODE_WINDOW       "window"

typedef struct
{
  const char *name;
  const char *icon;
  ClutterPointerA11yDwellClickType type;
} DwellClickMode;

/* DWELL_CLICK_MODES; the names are translated when the menu is built */
static const DwellClickMode dwell_click_modes[] = {
  { N_("Single Click"),    "pointer-primary-click-symbolic",   CLUTTER_A11Y_DWELL_CLICK_TYPE_PRIMARY },
  { N_("Double Click"),    "pointer-double-click-symbolic",    CLUTTER_A11Y_DWELL_CLICK_TYPE_DOUBLE },
  { N_("Drag"),           "pointer-drag-symbolic",            CLUTTER_A11Y_DWELL_CLICK_TYPE_DRAG },
  { N_("Secondary Click"), "pointer-secondary-click-symbolic", CLUTTER_A11Y_DWELL_CLICK_TYPE_SECONDARY },
};

struct _ShellDwellClickIndicator
{
  ShellPanelMenuButton parent;

  StIcon *icon;
  GSettings *a11y_settings;
  ClutterSeat *seat;            /* weak */
};

G_DEFINE_FINAL_TYPE (ShellDwellClickIndicator, shell_dwell_click_indicator,
                     SHELL_TYPE_PANEL_MENU_BUTTON)

/* _syncMenuVisibility () */
static void
dwell_click_sync_menu_visibility (ShellDwellClickIndicator *self)
{
  gboolean visible = FALSE;

  if (self->a11y_settings != NULL)
    {
      g_autofree char *mode = g_settings_get_string (self->a11y_settings, KEY_DWELL_MODE);

      visible = g_settings_get_boolean (self->a11y_settings, KEY_DWELL_CLICK_ENABLED) &&
                g_strcmp0 (mode, DWELL_MODE_WINDOW) == 0;
    }

  shell_status_set_visible (self, visible);
}

/* _updateClickType () */
static void
dwell_click_type_changed_cb (ClutterSeat                      *seat,
                             ClutterPointerA11yDwellClickType  click_type,
                             ShellDwellClickIndicator         *self)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (dwell_click_modes); i++)
    {
      if (dwell_click_modes[i].type == click_type)
        st_icon_set_icon_name (self->icon, dwell_click_modes[i].icon);
    }
}

/* _setClickType () */
static void
dwell_click_set_click_type (ShellDwellClickIndicator *self,
                            const DwellClickMode     *mode)
{
  if (self->seat != NULL)
    clutter_seat_set_pointer_a11y_dwell_click_type (self->seat, mode->type);
  st_icon_set_icon_name (self->icon, mode->icon);
}

typedef struct
{
  GWeakRef indicator;
  const DwellClickMode *mode;
} ActionData;

static void
action_data_free (ActionData *data)
{
  g_weak_ref_clear (&data->indicator);
  g_free (data);
}

static void
mode_action_cb (const ClutterEvent *event,
                gpointer            user_data)
{
  ActionData *data = user_data;
  g_autoptr (ShellDwellClickIndicator) self = g_weak_ref_get (&data->indicator);

  if (self != NULL)
    dwell_click_set_click_type (self, data->mode);
}

/* _addDwellAction () */
static void
dwell_click_add_action (ShellDwellClickIndicator *self,
                        const DwellClickMode     *mode)
{
  ShellPopupMenuBase *menu = SHELL_POPUP_MENU_BASE (shell_panel_menu_button_get_menu (SHELL_PANEL_MENU_BUTTON (self)));
  ActionData *data = g_new0 (ActionData, 1);

  g_weak_ref_init (&data->indicator, self);
  data->mode = mode;

  shell_popup_menu_base_add_action_with_icon (menu, _(mode->name), NULL, mode->icon,
                                              mode_action_cb, data,
                                              (GDestroyNotify) action_data_free);
}

static GObject *
shell_dwell_click_indicator_constructor (GType                  type,
                                         guint                  n_construct_properties,
                                         GObjectConstructParam *construct_properties)
{
  guint i;

  /* `super._init ({menu_alignment: 0.5, ...})`: the alignment is forced here,
   * before the button creates its menu */
  for (i = 0; i < n_construct_properties; i++)
    {
      if (g_strcmp0 (g_param_spec_get_name (construct_properties[i].pspec),
                     "menu-alignment") == 0)
        g_value_set_double (construct_properties[i].value, 0.5);
    }

  return G_OBJECT_CLASS (shell_dwell_click_indicator_parent_class)->constructor (type,
                                                                                 n_construct_properties,
                                                                                 construct_properties);
}

static void
shell_dwell_click_indicator_constructed (GObject *object)
{
  ShellDwellClickIndicator *self = SHELL_DWELL_CLICK_INDICATOR (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterContext *context;
  ClutterBackend *backend;
  guint i;

  G_OBJECT_CLASS (shell_dwell_click_indicator_parent_class)->constructed (object);

  clutter_actor_set_accessible_name (actor, _("Dwell Click"));

  self->icon = g_object_new (ST_TYPE_ICON,
                             "style-class", "system-status-icon",
                             "icon-name", "pointer-primary-click-symbolic",
                             NULL);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (self->icon));

  self->a11y_settings = shell_status_settings_new (MOUSE_A11Y_SCHEMA);
  if (self->a11y_settings != NULL)
    {
      g_signal_connect_object (self->a11y_settings, "changed::" KEY_DWELL_CLICK_ENABLED,
                               G_CALLBACK (dwell_click_sync_menu_visibility),
                               self, G_CONNECT_SWAPPED);
      g_signal_connect_object (self->a11y_settings, "changed::" KEY_DWELL_MODE,
                               G_CALLBACK (dwell_click_sync_menu_visibility),
                               self, G_CONNECT_SWAPPED);
    }

  context = clutter_actor_get_context (actor);
  backend = clutter_context_get_backend (context);
  g_set_weak_pointer (&self->seat, clutter_backend_get_default_seat (backend));
  g_signal_connect_object (self->seat, "ptr-a11y-dwell-click-type-changed",
                           G_CALLBACK (dwell_click_type_changed_cb), self, 0);

  for (i = 0; i < G_N_ELEMENTS (dwell_click_modes); i++)
    dwell_click_add_action (self, &dwell_click_modes[i]);

  dwell_click_set_click_type (self, &dwell_click_modes[0]);
  dwell_click_sync_menu_visibility (self);
}

static void
shell_dwell_click_indicator_dispose (GObject *object)
{
  ShellDwellClickIndicator *self = SHELL_DWELL_CLICK_INDICATOR (object);

  g_clear_weak_pointer (&self->seat);
  g_clear_object (&self->a11y_settings);

  G_OBJECT_CLASS (shell_dwell_click_indicator_parent_class)->dispose (object);
}

static void
shell_dwell_click_indicator_class_init (ShellDwellClickIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructor = shell_dwell_click_indicator_constructor;
  object_class->constructed = shell_dwell_click_indicator_constructed;
  object_class->dispose = shell_dwell_click_indicator_dispose;
}

static void
shell_dwell_click_indicator_init (ShellDwellClickIndicator *self)
{
}

/**
 * shell_dwell_click_indicator_new:
 *
 * Creates the dwell click menu button of the top bar
 * (`new DwellClickIndicator ()`).
 *
 * Returns: (transfer floating): a new #ShellDwellClickIndicator
 */
ShellDwellClickIndicator *
shell_dwell_click_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_DWELL_CLICK_INDICATOR, NULL);
}
