/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * switch-monitor.c: The popup of the display switch key. Port of
 * js/ui/switchMonitor.js.
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include <clutter/clutter.h>
#include <meta/meta-backend.h>
#include <meta/meta-monitor-manager.h>
#include <meta/prefs.h>

#include "shell-global.h"
#include "ui/switch-monitor.h"

#define APP_ICON_SIZE 96

typedef struct
{
  const char *icon;
  char *label;
  MetaMonitorSwitchConfigType config_type;
} SwitchMonitorItem;

static void
item_free (SwitchMonitorItem *item)
{
  g_free (item->label);
  g_free (item);
}

static MetaMonitorManager *
get_monitor_manager (void)
{
  return meta_backend_get_monitor_manager (shell_global_get_backend (shell_global_get ()));
}

/* ------------------------------------------------------------------------ */
/* ShellSwitchMonitorSwitcher: the list of the popup                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SWITCH_MONITOR_SWITCHER (shell_switch_monitor_switcher_get_type ())
G_DECLARE_FINAL_TYPE (ShellSwitchMonitorSwitcher, shell_switch_monitor_switcher,
                      SHELL, SWITCH_MONITOR_SWITCHER, ShellSwitcherList)

struct _ShellSwitchMonitorSwitcher
{
  ShellSwitcherList parent;
};

G_DEFINE_FINAL_TYPE (ShellSwitchMonitorSwitcher, shell_switch_monitor_switcher,
                     SHELL_TYPE_SWITCHER_LIST)

static void
switcher_add_icon (ShellSwitchMonitorSwitcher *self,
                   SwitchMonitorItem          *item)
{
  ClutterActor *box, *icon, *text;

  box = g_object_new (ST_TYPE_BOX_LAYOUT,
                      "style-class", "alt-tab-app",
                      "orientation", CLUTTER_ORIENTATION_VERTICAL,
                      NULL);

  icon = g_object_new (ST_TYPE_ICON,
                       "icon-name", item->icon,
                       "icon-size", APP_ICON_SIZE,
                       NULL);
  clutter_actor_add_child (box, icon);

  text = g_object_new (ST_TYPE_LABEL,
                       "text", item->label,
                       "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       NULL);
  clutter_actor_add_child (box, text);

  shell_switcher_list_add_item (SHELL_SWITCHER_LIST (self), box, text);
}

static void
shell_switch_monitor_switcher_class_init (ShellSwitchMonitorSwitcherClass *klass)
{
}

static void
shell_switch_monitor_switcher_init (ShellSwitchMonitorSwitcher *self)
{
}

static ShellSwitchMonitorSwitcher *
switcher_new (GPtrArray *items)
{
  ShellSwitchMonitorSwitcher *self;
  guint i;

  self = g_object_new (SHELL_TYPE_SWITCH_MONITOR_SWITCHER,
                       "square-items", TRUE,
                       NULL);

  for (i = 0; i < items->len; i++)
    switcher_add_icon (self, g_ptr_array_index (items, i));

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellSwitchMonitorPopup                                                   */
/* ------------------------------------------------------------------------ */

struct _ShellSwitchMonitorPopup
{
  ShellSwitcherPopup parent;

  GPtrArray *items;            /* SwitchMonitorItem */
};

G_DEFINE_FINAL_TYPE (ShellSwitchMonitorPopup, shell_switch_monitor_popup,
                     SHELL_TYPE_SWITCHER_POPUP)

static gboolean
popup_show_switcher (ShellSwitcherPopup *popup,
                     gboolean            backward,
                     const char         *binding,
                     uint32_t            mask)
{
  if (!meta_monitor_manager_can_switch_config (get_monitor_manager ()))
    return FALSE;

  return SHELL_SWITCHER_POPUP_CLASS (shell_switch_monitor_popup_parent_class)->show_switcher (popup,
                                                                                               backward,
                                                                                               binding,
                                                                                               mask);
}

static int
popup_get_n_items (ShellSwitcherPopup *popup)
{
  ShellSwitchMonitorPopup *self = SHELL_SWITCH_MONITOR_POPUP (popup);

  return self->items != NULL ? self->items->len : 0;
}

static void
popup_initial_selection (ShellSwitcherPopup *popup,
                         gboolean            backward,
                         const char         *binding)
{
  MetaMonitorSwitchConfigType current_config =
    meta_monitor_manager_get_switch_config (get_monitor_manager ());
  int n_items = shell_switcher_popup_get_n_items (popup);

  shell_switcher_popup_select (popup, n_items > 0 ? ((int) current_config + 1) % n_items : 0);
}

static gboolean
popup_key_press_handler (ShellSwitcherPopup *popup,
                         uint32_t            keysym,
                         uint32_t            action)
{
  if (action == META_KEYBINDING_ACTION_SWITCH_MONITOR)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_next_index (popup));
  else if (keysym == CLUTTER_KEY_Left)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_previous_index (popup));
  else if (keysym == CLUTTER_KEY_Right)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_next_index (popup));
  else
    return CLUTTER_EVENT_PROPAGATE;

  return CLUTTER_EVENT_STOP;
}

static void
popup_finish (ShellSwitcherPopup *popup,
              uint32_t            timestamp)
{
  ShellSwitchMonitorPopup *self = SHELL_SWITCH_MONITOR_POPUP (popup);
  int selected_index = shell_switcher_popup_get_selected_index (popup);
  MetaMonitorSwitchConfigType config_type;

  SHELL_SWITCHER_POPUP_CLASS (shell_switch_monitor_popup_parent_class)->finish (popup, timestamp);

  if (selected_index < 0 || (guint) selected_index >= self->items->len)
    return;

  config_type = ((SwitchMonitorItem *) g_ptr_array_index (self->items, selected_index))->config_type;
  meta_monitor_manager_switch_config (get_monitor_manager (), config_type);
}

static void
shell_switch_monitor_popup_constructed (GObject *object)
{
  ShellSwitchMonitorPopup *self = SHELL_SWITCH_MONITOR_POPUP (object);
  gboolean has_builtin_panel = FALSE;
  SwitchMonitorItem *item;

  G_OBJECT_CLASS (shell_switch_monitor_popup_parent_class)->constructed (object);

  item = g_new0 (SwitchMonitorItem, 1);
  item->icon = "shell-display-mirror-symbolic";
  /* Translators: this is for display mirroring i.e. cloning.
   * Try to keep it under around 15 characters.
   */
  item->label = g_strdup (_("Mirror"));
  item->config_type = META_MONITOR_SWITCH_CONFIG_ALL_MIRROR;
  g_ptr_array_add (self->items, item);

  item = g_new0 (SwitchMonitorItem, 1);
  item->icon = "shell-display-extend-all-symbolic";
  /* Translators: this is for the desktop spanning displays.
   * Try to keep it under around 15 characters.
   */
  item->label = g_strdup (_("Join Displays"));
  item->config_type = META_MONITOR_SWITCH_CONFIG_ALL_LINEAR;
  g_ptr_array_add (self->items, item);

  g_object_get (get_monitor_manager (), "has-builtin-panel", &has_builtin_panel, NULL);
  if (has_builtin_panel)
    {
      item = g_new0 (SwitchMonitorItem, 1);
      item->icon = "shell-display-external-only-symbolic";
      /* Translators: this is for using only external displays.
       * Try to keep it under around 15 characters.
       */
      item->label = g_strdup (_("External Only"));
      item->config_type = META_MONITOR_SWITCH_CONFIG_EXTERNAL;
      g_ptr_array_add (self->items, item);

      item = g_new0 (SwitchMonitorItem, 1);
      item->icon = "shell-display-built-in-only-symbolic";
      /* Translators: this is for using only the laptop display.
       * Try to keep it under around 15 characters.
       */
      item->label = g_strdup (_("Built-in Only"));
      item->config_type = META_MONITOR_SWITCH_CONFIG_BUILTIN;
      g_ptr_array_add (self->items, item);
    }

  shell_switcher_popup_set_switcher_list (SHELL_SWITCHER_POPUP (self),
                                          ST_WIDGET (switcher_new (self->items)));
}

static void
shell_switch_monitor_popup_finalize (GObject *object)
{
  ShellSwitchMonitorPopup *self = SHELL_SWITCH_MONITOR_POPUP (object);

  g_clear_pointer (&self->items, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_switch_monitor_popup_parent_class)->finalize (object);
}

static void
shell_switch_monitor_popup_class_init (ShellSwitchMonitorPopupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellSwitcherPopupClass *popup_class = SHELL_SWITCHER_POPUP_CLASS (klass);

  object_class->constructed = shell_switch_monitor_popup_constructed;
  object_class->finalize = shell_switch_monitor_popup_finalize;

  popup_class->show_switcher = popup_show_switcher;
  popup_class->get_n_items = popup_get_n_items;
  popup_class->initial_selection = popup_initial_selection;
  popup_class->key_press_handler = popup_key_press_handler;
  popup_class->finish = popup_finish;
}

static void
shell_switch_monitor_popup_init (ShellSwitchMonitorPopup *self)
{
  self->items = g_ptr_array_new_with_free_func ((GDestroyNotify) item_free);
}

/**
 * shell_switch_monitor_popup_new:
 *
 * Creates the popup with the display configurations that can be switched
 * to. Show it with shell_switcher_popup_show_switcher (), which returns
 * %FALSE if the configuration cannot be switched.
 *
 * Returns: (transfer none): a new #ShellSwitchMonitorPopup, owned by the ui
 *   group
 */
ShellSwitchMonitorPopup *
shell_switch_monitor_popup_new (void)
{
  return g_object_new (SHELL_TYPE_SWITCH_MONITOR_POPUP, NULL);
}
