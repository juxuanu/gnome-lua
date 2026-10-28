/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * app-menu.c: The context menu of an application icon of the app grid and
 * the dash. Port of js/ui/appMenu.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The overview, the icon and `Main.activateWindow ()` are not reachable
 *    from C. The menu emits signals where the JS called them, which the
 *    owner of the menu (app-display.c) connects:
 *    #ShellAppMenu::overview-hide-requested (`Main.overview.hide ()`),
 *    #ShellAppMenu::animate-launch (`sourceActor.animateLaunch ()`, which was
 *    only called if the source actor had that method) and
 *    #ShellAppMenu::window-activate-requested (`Main.activateWindow ()`).
 *  - `new AppMenu (sourceActor, side, {favoritesSection, showSingleWindows})`
 *    is `Shell.AppMenu.new (sourceActor, side, favoritesSection,
 *    showSingleWindows)`. The arrow side is swapped for right-to-left
 *    locales like the JS constructor did, and the arrow alignment is 0.5.
 *    Constructing with properties (`new Shell.AppMenu ({source_actor,
 *    arrow_side, favorites_section, show_single_windows})`) also swaps the
 *    side, but the alignment is the default 0 unless `arrow_alignment` is
 *    given.
 *  - "App Details" calls org.gnome.Software with the session bus connection
 *    that is already open instead of waiting for it.
 *  - The JS `connectObject (..., this.actor)` handlers die with the menu
 *    here: they are disconnected when the menu is destroyed.
 *  - Errors from launching the application are logged with g_warning ()
 *    instead of being thrown into the signal handler.
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-later.h>
#include <meta/window.h>

#include "misc/parental-controls-manager.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "ui/app-favorites.h"
#include "ui/app-menu.h"

enum
{
  PROP_0,

  PROP_APP,
  PROP_FAVORITES_SECTION,
  PROP_SHOW_SINGLE_WINDOWS,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  ANIMATE_LAUNCH,
  OVERVIEW_HIDE_REQUESTED,
  WINDOW_ACTIVATE_REQUESTED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellAppMenu
{
  ShellPopupMenu parent;

  ShellApp *app;
  gboolean favorites_section;
  gboolean show_single_windows;

  /* Foreign objects the menu listens to, weak: they can go away during
   * shutdown before the menu does */
  ShellAppSystem *app_system;
  ShellParentalControlsManager *parental_controls_manager;
  ShellAppFavorites *app_favorites;
  GSettings *settings;
  ShellGlobal *global;

  MetaLaters *laters; /* weak, where update_windows_later was added */
  unsigned int update_windows_later;

  ShellPopupSeparatorMenuItem *open_windows_header; /* weak */
  ShellPopupMenuSection *window_section; /* weak */
  ShellPopupMenuItem *new_window_item; /* weak */
  ShellPopupMenuSection *action_section; /* weak */
  ShellPopupMenuItem *on_gpu_item; /* weak */
  ShellPopupMenuItem *toggle_favorite_item; /* weak */
  ShellPopupMenuItem *details_item; /* weak */
  ShellPopupMenuItem *quit_item; /* weak */

  gboolean destroyed;
};

G_DEFINE_FINAL_TYPE (ShellAppMenu, shell_app_menu, SHELL_TYPE_POPUP_MENU)

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

/* What the closure of a menu item captured in the JS */
typedef struct
{
  ShellAppMenu *menu; /* weak */
  char *action;       /* nullable, the desktop action */
  MetaWindow *window; /* nullable, weak */
} ItemData;

static ItemData *
item_data_new (ShellAppMenu *menu)
{
  ItemData *data = g_new0 (ItemData, 1);

  g_set_weak_pointer (&data->menu, menu);

  return data;
}

static void
item_data_free (gpointer user_data)
{
  ItemData *data = user_data;

  g_clear_weak_pointer (&data->menu);
  g_clear_weak_pointer (&data->window);
  g_free (data->action);
  g_free (data);
}

static uint32_t
get_event_time (const ClutterEvent *event)
{
  if (event != NULL)
    return clutter_event_get_time (event);

  return CLUTTER_CURRENT_TIME;
}

static void
set_item_visible (gpointer item,
                  gboolean visible)
{
  if (item == NULL)
    return;

  if (visible)
    clutter_actor_show (CLUTTER_ACTOR (item));
  else
    clutter_actor_hide (CLUTTER_ACTOR (item));
}

static void
animate_launch (ShellAppMenu *self)
{
  g_signal_emit (self, signals[ANIMATE_LAUNCH], 0);
}

static void
hide_overview (ShellAppMenu *self)
{
  g_signal_emit (self, signals[OVERVIEW_HIDE_REQUESTED], 0);
}

static ShellAppLaunchGpu
get_non_default_launch_gpu (ShellAppMenu *self)
{
  GDesktopAppInfo *app_info = self->app != NULL ? shell_app_get_app_info (self->app) : NULL;

  if (app_info != NULL &&
      g_desktop_app_info_get_boolean (app_info, "PrefersNonDefaultGPU"))
    return SHELL_APP_LAUNCH_GPU_DEFAULT;

  return SHELL_APP_LAUNCH_GPU_DISCRETE;
}

/* ------------------------------------------------------------------------ */
/* Item callbacks                                                           */
/* ------------------------------------------------------------------------ */

static void
new_window_cb (const ClutterEvent *event,
               gpointer            user_data)
{
  ItemData *data = user_data;
  ShellAppMenu *self = data->menu;

  if (self == NULL || self->app == NULL)
    return;

  animate_launch (self);
  shell_app_open_new_window (self->app, -1);
  hide_overview (self);
}

static void
launch_on_gpu_cb (const ClutterEvent *event,
                  gpointer            user_data)
{
  ItemData *data = user_data;
  ShellAppMenu *self = data->menu;
  g_autoptr (GError) error = NULL;

  if (self == NULL || self->app == NULL)
    return;

  animate_launch (self);
  if (!shell_app_launch (self->app, 0, -1, get_non_default_launch_gpu (self), &error))
    g_warning ("Failed to launch %s: %s", shell_app_get_id (self->app), error->message);
  hide_overview (self);
}

static void
toggle_favorite_cb (const ClutterEvent *event,
                    gpointer            user_data)
{
  ItemData *data = user_data;
  ShellAppMenu *self = data->menu;
  const char *app_id;

  if (self == NULL || self->app == NULL || self->app_favorites == NULL)
    return;

  app_id = shell_app_get_id (self->app);
  if (shell_app_favorites_is_favorite (self->app_favorites, app_id))
    shell_app_favorites_remove_favorite (self->app_favorites, app_id);
  else
    shell_app_favorites_add_favorite (self->app_favorites, app_id);
}

static void
details_cb (const ClutterEvent *event,
            gpointer            user_data)
{
  ItemData *data = user_data;
  ShellAppMenu *self = data->menu;
  g_autoptr (GDBusConnection) bus = NULL;
  g_autoptr (GError) error = NULL;
  GVariantBuilder args, platform_data;

  if (self == NULL || self->app == NULL)
    return;

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (bus == NULL)
    {
      g_warning ("Failed to open details of %s: %s",
                 shell_app_get_id (self->app), error->message);
      return;
    }

  g_variant_builder_init (&args, G_VARIANT_TYPE ("av"));
  g_variant_builder_add (&args, "v",
                         g_variant_new ("(ss)", shell_app_get_id (self->app), ""));
  g_variant_builder_init (&platform_data, G_VARIANT_TYPE ("a{sv}"));

  g_dbus_connection_call (bus,
                          "org.gnome.Software",
                          "/org/gnome/Software",
                          "org.gtk.Actions", "Activate",
                          g_variant_new ("(sava{sv})", "details", &args, &platform_data),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  hide_overview (self);
}

static void
quit_cb (const ClutterEvent *event,
         gpointer            user_data)
{
  ItemData *data = user_data;
  ShellAppMenu *self = data->menu;

  if (self != NULL && self->app != NULL)
    shell_app_request_quit (self->app);
}

static void
desktop_action_cb (const ClutterEvent *event,
                   gpointer            user_data)
{
  ItemData *data = user_data;
  ShellAppMenu *self = data->menu;

  if (self == NULL || self->app == NULL)
    return;

  if (g_strcmp0 (data->action, "new-window") == 0)
    animate_launch (self);

  shell_app_launch_action (self->app, data->action, get_event_time (event), -1);
  hide_overview (self);
}

static void
activate_window_cb (const ClutterEvent *event,
                    gpointer            user_data)
{
  ItemData *data = user_data;

  if (data->menu == NULL || data->window == NULL)
    return;

  g_signal_emit (data->menu, signals[WINDOW_ACTIVATE_REQUESTED], 0,
                 data->window, get_event_time (event));
}

static void
window_title_changed_cb (MetaWindow *window,
                         GParamSpec *pspec,
                         gpointer    user_data)
{
  ShellPopupMenuItem *item = user_data;
  const char *title = meta_window_get_title (window);

  if (title == NULL || *title == '\0')
    title = g_object_get_data (G_OBJECT (item), "shell-app-menu-app-name");

  shell_popup_menu_item_set_text (item, title);
}

/* ------------------------------------------------------------------------ */
/* Updating the items                                                       */
/* ------------------------------------------------------------------------ */

static void
update_quit_item (ShellAppMenu *self)
{
  set_item_visible (self->quit_item,
                    self->app != NULL &&
                    shell_app_get_state (self->app) == SHELL_APP_STATE_RUNNING);
}

static void
update_new_window_item (ShellAppMenu *self)
{
  GDesktopAppInfo *app_info = self->app != NULL ? shell_app_get_app_info (self->app) : NULL;
  const char * const *actions = NULL;
  gboolean has_new_window_action;

  if (app_info != NULL)
    actions = g_desktop_app_info_list_actions (app_info);
  has_new_window_action = actions != NULL && g_strv_contains (actions, "new-window");

  set_item_visible (self->new_window_item,
                    self->app != NULL &&
                    shell_app_can_open_new_window (self->app) &&
                    !has_new_window_action);
}

static void
update_favorite_item (ShellAppMenu *self)
{
  GDesktopAppInfo *app_info = self->app != NULL ? shell_app_get_app_info (self->app) : NULL;
  gboolean can_favorite;

  if (self->toggle_favorite_item == NULL)
    return;

  can_favorite = app_info != NULL &&
                 self->favorites_section &&
                 self->settings != NULL &&
                 g_settings_is_writable (self->settings, "favorite-apps") &&
                 self->parental_controls_manager != NULL &&
                 shell_parental_controls_manager_should_show_app (self->parental_controls_manager,
                                                                  G_APP_INFO (app_info));

  set_item_visible (self->toggle_favorite_item, can_favorite);

  if (!can_favorite || self->app_favorites == NULL)
    return;

  shell_popup_menu_item_set_text (self->toggle_favorite_item,
                                  shell_app_favorites_is_favorite (self->app_favorites,
                                                                   shell_app_get_id (self->app))
                                  ? _("Unpin")
                                  : _("Pin to Dash"));
}

static void
update_gpu_item (ShellAppMenu *self)
{
  GDBusProxy *proxy = self->global != NULL ? shell_global_get_switcheroo_control (self->global) : NULL;
  g_autoptr (GVariant) has_dual_gpu_variant = NULL;
  gboolean has_dual_gpu = FALSE;
  gboolean show_item;

  if (self->on_gpu_item == NULL)
    return;

  if (proxy != NULL)
    has_dual_gpu_variant = g_dbus_proxy_get_cached_property (proxy, "HasDualGpu");
  if (has_dual_gpu_variant != NULL &&
      g_variant_is_of_type (has_dual_gpu_variant, G_VARIANT_TYPE_BOOLEAN))
    has_dual_gpu = g_variant_get_boolean (has_dual_gpu_variant);

  show_item = self->app != NULL &&
              shell_app_get_state (self->app) == SHELL_APP_STATE_STOPPED &&
              has_dual_gpu;

  set_item_visible (self->on_gpu_item, show_item);

  if (!show_item)
    return;

  shell_popup_menu_item_set_text (self->on_gpu_item,
                                  get_non_default_launch_gpu (self) == SHELL_APP_LAUNCH_GPU_DEFAULT
                                  ? _("Launch Using Integrated Graphics Card")
                                  : _("Launch Using Discrete Graphics Card"));
}

static void
update_details_visibility (ShellAppMenu *self)
{
  ShellApp *software;

  if (self->app_system == NULL)
    return;

  software = shell_app_system_lookup_app (self->app_system, "org.gnome.Software.desktop");
  set_item_visible (self->details_item, software != NULL);
}

static void
clear_update_windows_later (ShellAppMenu *self)
{
  if (self->update_windows_later != 0 && self->laters != NULL)
    meta_laters_remove (self->laters, self->update_windows_later);
  self->update_windows_later = 0;
}

/**
 * shell_app_menu_update_windows_section:
 * @self: a #ShellAppMenu
 *
 * Rebuilds the list of the open windows of the application. The menu does
 * this itself when the application's windows change.
 */
void
shell_app_menu_update_windows_section (ShellAppMenu *self)
{
  g_autoptr (GSList) windows = NULL;
  unsigned int min_windows, n_windows = 0;
  GSList *l;

  g_return_if_fail (SHELL_IS_APP_MENU (self));

  clear_update_windows_later (self);

  if (self->window_section == NULL)
    return;

  shell_popup_menu_base_remove_all (SHELL_POPUP_MENU_BASE (self->window_section));
  set_item_visible (self->open_windows_header, FALSE);

  if (self->app == NULL)
    return;

  min_windows = self->show_single_windows ? 1 : 2;
  for (l = windows = shell_app_get_windows (self->app); l != NULL; l = l->next)
    {
      if (!meta_window_is_skip_taskbar (l->data))
        n_windows++;
    }

  if (n_windows < min_windows)
    return;

  set_item_visible (self->open_windows_header, TRUE);

  for (l = windows; l != NULL; l = l->next)
    {
      MetaWindow *window = l->data;
      const char *title;
      ItemData *data;
      ShellPopupBaseMenuItem *item;

      if (meta_window_is_skip_taskbar (window))
        continue;

      title = meta_window_get_title (window);
      if (title == NULL || *title == '\0')
        title = shell_app_get_name (self->app);

      data = item_data_new (self);
      g_set_weak_pointer (&data->window, window);
      item = shell_popup_menu_base_add_action (SHELL_POPUP_MENU_BASE (self->window_section),
                                               title, activate_window_cb,
                                               data, item_data_free);
      g_object_set_data_full (G_OBJECT (item), "shell-app-menu-app-name",
                              g_strdup (shell_app_get_name (self->app)), g_free);
      g_signal_connect_object (window, "notify::title",
                               G_CALLBACK (window_title_changed_cb), item, 0);
    }
}

static gboolean
update_windows_later_cb (gpointer user_data)
{
  ShellAppMenu *self = user_data;

  /* The later is being removed by returning G_SOURCE_REMOVE */
  self->update_windows_later = 0;
  shell_app_menu_update_windows_section (self);

  return G_SOURCE_REMOVE;
}

static void
queue_update_windows_section (ShellAppMenu *self)
{
  MetaLaters *laters;

  if (self->update_windows_later != 0 || self->destroyed)
    return;

  laters = meta_compositor_get_laters (shell_global_get_compositor (shell_global_get ()));
  g_set_weak_pointer (&self->laters, laters);
  self->update_windows_later = meta_laters_add (laters, META_LATER_BEFORE_REDRAW,
                                                update_windows_later_cb, self, NULL);
}

static void
windows_changed_cb (ShellApp     *app,
                    ShellAppMenu *self)
{
  queue_update_windows_section (self);
}

static void
update_favorite_item_cb (gpointer instance,
                         gpointer user_data)
{
  update_favorite_item (SHELL_APP_MENU (user_data));
}

static void
update_favorite_item_writable_cb (GSettings    *settings,
                                  const char   *key,
                                  ShellAppMenu *self)
{
  update_favorite_item (self);
}

static void
installed_changed_cb (ShellAppSystem *system,
                      ShellAppMenu   *self)
{
  update_details_visibility (self);
}

static void
app_state_changed_cb (ShellAppSystem *system,
                      ShellApp       *app,
                      ShellAppMenu   *self)
{
  if (self->app != app)
    return;

  update_quit_item (self);
  update_new_window_item (self);
  update_gpu_item (self);
}

static void
switcheroo_control_changed_cb (GObject      *object,
                               GParamSpec   *pspec,
                               ShellAppMenu *self)
{
  update_gpu_item (self);
}

/* ------------------------------------------------------------------------ */
/* Application                                                              */
/* ------------------------------------------------------------------------ */

/**
 * shell_app_menu_get_app:
 * @self: a #ShellAppMenu
 *
 * Returns: (transfer none) (nullable): the application the menu represents
 */
ShellApp *
shell_app_menu_get_app (ShellAppMenu *self)
{
  g_return_val_if_fail (SHELL_IS_APP_MENU (self), NULL);

  return self->app;
}

/**
 * shell_app_menu_set_app:
 * @self: a #ShellAppMenu
 * @app: (nullable): the application the menu represents (`setApp ()`)
 *
 * Fills the menu for @app: its open windows and its desktop actions. A menu
 * without an application is empty.
 */
void
shell_app_menu_set_app (ShellAppMenu *self,
                        ShellApp     *app)
{
  GDesktopAppInfo *app_info;
  const char * const *actions;
  guint i;

  g_return_if_fail (SHELL_IS_APP_MENU (self));
  g_return_if_fail (app == NULL || SHELL_IS_APP (app));

  if (self->app == app)
    return;

  if (self->app != NULL)
    g_signal_handlers_disconnect_by_data (self->app, self);

  g_set_object (&self->app, app);

  if (self->app != NULL)
    g_signal_connect_object (self->app, "windows-changed",
                             G_CALLBACK (windows_changed_cb), self, 0);

  shell_app_menu_update_windows_section (self);

  app_info = app != NULL ? shell_app_get_app_info (app) : NULL;
  actions = app_info != NULL ? g_desktop_app_info_list_actions (app_info) : NULL;

  if (self->action_section != NULL)
    {
      ShellPopupMenuBase *section = SHELL_POPUP_MENU_BASE (self->action_section);

      shell_popup_menu_base_remove_all (section);
      for (i = 0; actions != NULL && actions[i] != NULL; i++)
        {
          g_autofree char *label = g_desktop_app_info_get_action_name (app_info, actions[i]);
          ItemData *data = item_data_new (self);

          data->action = g_strdup (actions[i]);
          shell_popup_menu_base_add_action (section, label, desktop_action_cb,
                                            data, item_data_free);
        }
    }

  update_quit_item (self);
  update_new_window_item (self);
  update_favorite_item (self);
  update_gpu_item (self);

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_APP]);
}

/**
 * shell_app_menu_get_favorites_section:
 * @self: a #ShellAppMenu
 *
 * Returns: whether the menu has the item which pins the application to the dash
 */
gboolean
shell_app_menu_get_favorites_section (ShellAppMenu *self)
{
  g_return_val_if_fail (SHELL_IS_APP_MENU (self), FALSE);

  return self->favorites_section;
}

/**
 * shell_app_menu_get_show_single_windows:
 * @self: a #ShellAppMenu
 *
 * Returns: whether the list of open windows is shown for a single window
 */
gboolean
shell_app_menu_get_show_single_windows (ShellAppMenu *self)
{
  g_return_val_if_fail (SHELL_IS_APP_MENU (self), FALSE);

  return self->show_single_windows;
}

/* ------------------------------------------------------------------------ */
/* Class                                                                    */
/* ------------------------------------------------------------------------ */

static void
disconnect_sources (ShellAppMenu *self)
{
  if (self->app_system != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->app_system, self);
      g_clear_weak_pointer (&self->app_system);
    }
  if (self->parental_controls_manager != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->parental_controls_manager, self);
      g_clear_weak_pointer (&self->parental_controls_manager);
    }
  if (self->app_favorites != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->app_favorites, self);
      g_clear_weak_pointer (&self->app_favorites);
    }
  if (self->settings != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->settings, self);
      g_clear_weak_pointer (&self->settings);
    }
  if (self->global != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->global, self);
      g_clear_weak_pointer (&self->global);
    }
}

static void
shell_app_menu_real_destroy (ShellPopupMenuBase *base)
{
  ShellAppMenu *self = SHELL_APP_MENU (base);

  shell_app_menu_set_app (self, NULL);
  self->destroyed = TRUE;
  disconnect_sources (self);

  SHELL_POPUP_MENU_BASE_CLASS (shell_app_menu_parent_class)->destroy (base);
}

static gboolean
shell_app_menu_real_is_empty (ShellPopupMenuBase *base)
{
  ShellAppMenu *self = SHELL_APP_MENU (base);

  if (self->app == NULL)
    return TRUE;

  return SHELL_POPUP_MENU_BASE_CLASS (shell_app_menu_parent_class)->is_empty (base);
}

static void
shell_app_menu_constructed (GObject *object)
{
  ShellAppMenu *self = SHELL_APP_MENU (object);
  ShellPopupMenuBase *base = SHELL_POPUP_MENU_BASE (self);
  ShellPopupMenu *popup_menu = SHELL_POPUP_MENU (self);
  ShellPopupMenuItem *item;
  g_autoptr (ShellPopupMenuSection) window_section = NULL;
  g_autoptr (ShellPopupMenuSection) action_section = NULL;
  ShellPopupSeparatorMenuItem *header;
  const char *open_windows_text;
  StSide side;

  G_OBJECT_CLASS (shell_app_menu_parent_class)->constructed (object);

  /* Right-to-left locales mirror the side of the arrow */
  side = shell_popup_menu_get_arrow_side (popup_menu);
  if (clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL)
    {
      if (side == ST_SIDE_LEFT)
        shell_box_pointer_update_arrow_side (shell_popup_menu_get_box_pointer (popup_menu),
                                             ST_SIDE_RIGHT);
      else if (side == ST_SIDE_RIGHT)
        shell_box_pointer_update_arrow_side (shell_popup_menu_get_box_pointer (popup_menu),
                                             ST_SIDE_LEFT);
    }

  st_widget_add_style_class_name (ST_WIDGET (shell_popup_menu_base_get_actor (base)),
                                  "app-menu");

  self->app_system = shell_app_system_get_default ();
  g_object_add_weak_pointer (G_OBJECT (self->app_system), (gpointer *) &self->app_system);
  self->parental_controls_manager = shell_parental_controls_manager_get_default ();
  g_object_add_weak_pointer (G_OBJECT (self->parental_controls_manager),
                             (gpointer *) &self->parental_controls_manager);
  self->app_favorites = shell_app_favorites_get_default ();
  g_object_add_weak_pointer (G_OBJECT (self->app_favorites), (gpointer *) &self->app_favorites);
  self->global = shell_global_get ();
  g_object_add_weak_pointer (G_OBJECT (self->global), (gpointer *) &self->global);
  self->settings = shell_global_get_settings (self->global);
  g_object_add_weak_pointer (G_OBJECT (self->settings), (gpointer *) &self->settings);

  /* Translators: This is the heading of a list of open windows */
  open_windows_text = _("Open Windows");
  header = g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM,
                         "text", open_windows_text,
                         NULL);
  shell_popup_menu_base_add_menu_item (base, G_OBJECT (header));
  g_set_weak_pointer (&self->open_windows_header, header);

  window_section = shell_popup_menu_section_new ();
  g_set_weak_pointer (&self->window_section, window_section);
  shell_popup_menu_base_add_menu_item (base, G_OBJECT (window_section));

  shell_popup_menu_base_add_menu_item (base,
                                       g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL));

  item = SHELL_POPUP_MENU_ITEM (shell_popup_menu_base_add_action (base, _("New Window"),
                                                                  new_window_cb,
                                                                  item_data_new (self),
                                                                  item_data_free));
  g_set_weak_pointer (&self->new_window_item, item);

  action_section = shell_popup_menu_section_new ();
  g_set_weak_pointer (&self->action_section, action_section);
  shell_popup_menu_base_add_menu_item (base, G_OBJECT (action_section));

  item = SHELL_POPUP_MENU_ITEM (shell_popup_menu_base_add_action (base, "",
                                                                  launch_on_gpu_cb,
                                                                  item_data_new (self),
                                                                  item_data_free));
  g_set_weak_pointer (&self->on_gpu_item, item);

  shell_popup_menu_base_add_menu_item (base,
                                       g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL));

  item = SHELL_POPUP_MENU_ITEM (shell_popup_menu_base_add_action (base, "",
                                                                  toggle_favorite_cb,
                                                                  item_data_new (self),
                                                                  item_data_free));
  g_set_weak_pointer (&self->toggle_favorite_item, item);

  shell_popup_menu_base_add_menu_item (base,
                                       g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL));

  item = SHELL_POPUP_MENU_ITEM (shell_popup_menu_base_add_action (base, _("App Details"),
                                                                  details_cb,
                                                                  item_data_new (self),
                                                                  item_data_free));
  g_set_weak_pointer (&self->details_item, item);

  shell_popup_menu_base_add_menu_item (base,
                                       g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL));

  item = SHELL_POPUP_MENU_ITEM (shell_popup_menu_base_add_action (base, _("Quit"),
                                                                  quit_cb,
                                                                  item_data_new (self),
                                                                  item_data_free));
  g_set_weak_pointer (&self->quit_item, item);

  g_signal_connect_object (self->app_system, "installed-changed",
                           G_CALLBACK (installed_changed_cb), self, 0);
  g_signal_connect_object (self->app_system, "app-state-changed",
                           G_CALLBACK (app_state_changed_cb), self, 0);
  g_signal_connect_object (self->parental_controls_manager, "app-filter-changed",
                           G_CALLBACK (update_favorite_item_cb), self, 0);
  g_signal_connect_object (self->app_favorites, "changed",
                           G_CALLBACK (update_favorite_item_cb), self, 0);
  g_signal_connect_object (self->settings, "writable-changed::favorite-apps",
                           G_CALLBACK (update_favorite_item_writable_cb), self, 0);
  g_signal_connect_object (self->global, "notify::switcheroo-control",
                           G_CALLBACK (switcheroo_control_changed_cb), self, 0);

  update_quit_item (self);
  update_favorite_item (self);
  update_gpu_item (self);
  update_details_visibility (self);
  shell_app_menu_update_windows_section (self);
}

static void
shell_app_menu_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  ShellAppMenu *self = SHELL_APP_MENU (object);

  switch (prop_id)
    {
    case PROP_APP:
      g_value_set_object (value, self->app);
      break;

    case PROP_FAVORITES_SECTION:
      g_value_set_boolean (value, self->favorites_section);
      break;

    case PROP_SHOW_SINGLE_WINDOWS:
      g_value_set_boolean (value, self->show_single_windows);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_app_menu_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  ShellAppMenu *self = SHELL_APP_MENU (object);

  switch (prop_id)
    {
    case PROP_APP:
      shell_app_menu_set_app (self, g_value_get_object (value));
      break;

    case PROP_FAVORITES_SECTION:
      self->favorites_section = g_value_get_boolean (value);
      break;

    case PROP_SHOW_SINGLE_WINDOWS:
      self->show_single_windows = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_app_menu_dispose (GObject *object)
{
  ShellAppMenu *self = SHELL_APP_MENU (object);

  clear_update_windows_later (self);
  g_clear_weak_pointer (&self->laters);

  disconnect_sources (self);
  if (self->app != NULL)
    g_signal_handlers_disconnect_by_data (self->app, self);
  g_clear_object (&self->app);

  g_clear_weak_pointer (&self->open_windows_header);
  g_clear_weak_pointer (&self->window_section);
  g_clear_weak_pointer (&self->new_window_item);
  g_clear_weak_pointer (&self->action_section);
  g_clear_weak_pointer (&self->on_gpu_item);
  g_clear_weak_pointer (&self->toggle_favorite_item);
  g_clear_weak_pointer (&self->details_item);
  g_clear_weak_pointer (&self->quit_item);

  G_OBJECT_CLASS (shell_app_menu_parent_class)->dispose (object);
}

static void
shell_app_menu_class_init (ShellAppMenuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupMenuBaseClass *base_class = SHELL_POPUP_MENU_BASE_CLASS (klass);

  object_class->constructed = shell_app_menu_constructed;
  object_class->dispose = shell_app_menu_dispose;
  object_class->get_property = shell_app_menu_get_property;
  object_class->set_property = shell_app_menu_set_property;

  base_class->destroy = shell_app_menu_real_destroy;
  base_class->is_empty = shell_app_menu_real_is_empty;

  /**
   * ShellAppMenu:app:
   *
   * The application the menu represents (`setApp ()`). The menu is empty
   * without one.
   */
  props[PROP_APP] =
    g_param_spec_object ("app", NULL, NULL,
                         SHELL_TYPE_APP,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellAppMenu:favorites-section:
   *
   * Whether the menu has the item which pins the application to the dash
   * (`favoritesSection`)
   */
  props[PROP_FAVORITES_SECTION] =
    g_param_spec_boolean ("favorites-section", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellAppMenu:show-single-windows:
   *
   * Whether the list of the open windows is shown for a single window, and
   * not only for two or more (`showSingleWindows`)
   */
  props[PROP_SHOW_SINGLE_WINDOWS] =
    g_param_spec_boolean ("show-single-windows", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellAppMenu::animate-launch:
   * @self: the #ShellAppMenu
   *
   * Emitted before an item launches the application, so that the owner can
   * animate the icon the menu belongs to (`sourceActor.animateLaunch ()`).
   */
  signals[ANIMATE_LAUNCH] =
    g_signal_new ("animate-launch",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellAppMenu::overview-hide-requested:
   * @self: the #ShellAppMenu
   *
   * Emitted after an item launched or showed something, where the JS called
   * `Main.overview.hide ()`.
   */
  signals[OVERVIEW_HIDE_REQUESTED] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellAppMenu::window-activate-requested:
   * @self: the #ShellAppMenu
   * @window: the window of the item which was activated
   * @time: the time of the event which activated the item
   *
   * Emitted when an item of the list of windows is activated. The owner
   * calls `Main.activateWindow (window, time)`, which also leaves the
   * overview.
   */
  signals[WINDOW_ACTIVATE_REQUESTED] =
    g_signal_new ("window-activate-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2, META_TYPE_WINDOW, G_TYPE_UINT);
}

static void
shell_app_menu_init (ShellAppMenu *self)
{
}

/**
 * shell_app_menu_new:
 * @source_actor: the actor the menu is attached to
 * @side: the side of the menu the arrow points from; left and right are
 *   swapped in right-to-left locales
 * @favorites_section: whether to show the items to add and remove a favorite
 * @show_single_windows: whether to list the windows of an application which
 *   has a single one
 *
 * Creates the menu of an application, which is empty until
 * shell_app_menu_set_app () is called. The actor of the menu still has to be
 * added to the UI group (`new AppMenu (sourceActor, side, params)`).
 *
 * Returns: (transfer full): a new #ShellAppMenu
 */
ShellAppMenu *
shell_app_menu_new (ClutterActor *source_actor,
                    StSide        side,
                    gboolean      favorites_section,
                    gboolean      show_single_windows)
{
  g_return_val_if_fail (CLUTTER_IS_ACTOR (source_actor), NULL);

  return g_object_new (SHELL_TYPE_APP_MENU,
                       "source-actor", source_actor,
                       "arrow-alignment", 0.5,
                       "arrow-side", side,
                       "favorites-section", favorites_section,
                       "show-single-windows", show_single_windows,
                       NULL);
}
