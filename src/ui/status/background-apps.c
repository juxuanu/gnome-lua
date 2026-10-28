/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * background-apps.c: The quick settings entry which lists the applications
 * that run without a window, and lets the user quit them. Port of
 * js/ui/status/backgroundApps.js.
 *
 * The org.freedesktop.background.Monitor proxy is a gdbus-codegen proxy
 * (`background-monitor-dbus`, type ShellBackgroundMonitor), created with
 * DO_NOT_AUTO_START. Without the portal the toggle stays hidden, quietly.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `Main.overview.hide ()` is the signal
 *    #ShellBackgroundAppsIndicator::overview-hide-requested (panelItems.js
 *    connects it); `Main.panel`, `Main.sessionMode` are read from
 *    #ShellMain.
 *  - The applications are sorted with g_utf8_collate () where the JS used
 *    String.localeCompare ().
 *  - Util.trySpawn () (js/misc/util.js is still JS) is a local copy, as in
 *    ui/run-dialog.c; a spawn error is logged as a warning.
 *  - A request to quit an application that is still pending when the
 *    shell shuts down, or when its item is destroyed, is dropped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-systemd.h>

#include "shell-app-system.h"
#include "shell-app.h"
#include "shell-global.h"
#include "shell-util.h"
#include "ui/animation.h"
#include "ui/panel.h"
#include "ui/popup-menu.h"
#include "ui/quick-settings.h"
#include "ui/status/background-apps.h"
#include "ui/status/status-private.h"

#include "background-monitor-dbus.h"

#define DBUS_NAME "org.freedesktop.background.Monitor"
#define DBUS_OBJECT_PATH "/org/freedesktop/background/monitor"

#define SPINNER_TIMEOUT 5 /* seconds */

/* ======================================================================== */
/* ShellBackgroundAppMenuItem                                               */
/* ======================================================================== */

enum
{
  ITEM_PROP_0,

  ITEM_PROP_APP,
  ITEM_PROP_MESSAGE,

  N_ITEM_PROPS
};

static GParamSpec *item_props[N_ITEM_PROPS] = { NULL, };

#define SHELL_TYPE_BACKGROUND_APP_MENU_ITEM (shell_background_app_menu_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellBackgroundAppMenuItem, shell_background_app_menu_item,
                      SHELL, BACKGROUND_APP_MENU_ITEM, ShellPopupImageMenuItem)

struct _ShellBackgroundAppMenuItem
{
  ShellPopupImageMenuItem parent;

  ShellApp *app;
  char *message;

  StLabel *message_label;       /* weak */
  ShellSpinner *spinner;        /* weak */
  guint spinner_timeout_id;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (ShellBackgroundAppMenuItem, shell_background_app_menu_item,
                     SHELL_TYPE_POPUP_IMAGE_MENU_ITEM)

static void
child_watch_cb (GPid     pid,
                int      wait_status,
                gpointer user_data)
{
}

/* Util.trySpawn (): runs @argv in the background */
static gboolean
try_spawn (const char * const  *argv,
           GError             **error)
{
  g_autoptr (GAppLaunchContext) launch_context = NULL;
  g_auto (GStrv) envp = NULL;
  GPid pid;

  launch_context = shell_global_create_app_launch_context (shell_global_get (), 0, -1);
  envp = g_app_launch_context_get_environment (launch_context);

  pid = shell_util_spawn_async (NULL, argv, (const char * const *) envp,
                                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                error);
  if (pid == 0)
    return FALSE;

  /* Async call, we don't need the reply though */
  gnome_start_systemd_scope (argv[0], pid, NULL, NULL, NULL, NULL, NULL);

  /* Dummy child watch; we don't want to double-fork internally
   * because then we lose the parent-child relationship, which
   * can break polkit.  See https://bugzilla.redhat.com//show_bug.cgi?id=819275 */
  g_child_watch_add (pid, child_watch_cb, NULL);

  return TRUE;
}

static void
item_stop_spinner_timeout (ShellBackgroundAppMenuItem *self)
{
  g_clear_handle_id (&self->spinner_timeout_id, g_source_remove);
}

static void
item_spinner_timeout_cb (gpointer user_data)
{
  ShellBackgroundAppMenuItem *self = user_data;

  /* Assume the quit request has failed, stop the spinner */
  self->spinner_timeout_id = 0;
  if (self->spinner != NULL)
    shell_spinner_stop (self->spinner);
}

static void
item_kill_app (ShellBackgroundAppMenuItem *self)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *app_id = NULL;
  const char *argv[] = { "flatpak", "kill", NULL, NULL };

  /* The item was disposed while the request was pending */
  if (self->app == NULL)
    return;

  app_id = g_strdup (shell_app_get_id (self->app));
  if (g_str_has_suffix (app_id, ".desktop"))
    app_id[strlen (app_id) - strlen (".desktop")] = '\0';
  argv[2] = app_id;

  if (!try_spawn (argv, &error))
    g_warning ("Failed to kill application: %s", error->message);
}

static void
item_quit_cb (GObject      *source,
              GAsyncResult *res,
              gpointer      user_data)
{
  g_autoptr (ShellBackgroundAppMenuItem) self = user_data;
  g_autoptr (GError) error = NULL;

  if (!shell_app_activate_action_finish (SHELL_APP (source), res, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      item_kill_app (self);
    }
}

/* _quitApp () */
static void
item_quit_app (ShellBackgroundAppMenuItem *self)
{
  if (self->spinner == NULL)
    return;

  shell_spinner_play (self->spinner);

  item_stop_spinner_timeout (self);
  self->spinner_timeout_id =
    g_timeout_add_seconds_once (SPINNER_TIMEOUT, item_spinner_timeout_cb, self);

  if (self->cancellable == NULL)
    self->cancellable = g_cancellable_new ();

  shell_app_activate_action (self->app, "quit", NULL, 0, -1,
                             self->cancellable, item_quit_cb,
                             g_object_ref (self));
}

static void
item_close_button_clicked_cb (StButton                   *button,
                              int                         clicked_button,
                              ShellBackgroundAppMenuItem *self)
{
  item_quit_app (self);
}

static void
item_set_message (ShellBackgroundAppMenuItem *self,
                  const char                 *message)
{
  if (g_strcmp0 (self->message, message) == 0)
    return;

  g_free (self->message);
  self->message = g_strdup (message);

  if (self->message_label != NULL)
    {
      st_label_set_text (self->message_label, message != NULL ? message : "");
      shell_status_set_visible (self->message_label, message != NULL);
    }

  g_object_notify_by_pspec (G_OBJECT (self), item_props[ITEM_PROP_MESSAGE]);
}

static void
shell_background_app_menu_item_get_property (GObject    *object,
                                             guint       prop_id,
                                             GValue     *value,
                                             GParamSpec *pspec)
{
  ShellBackgroundAppMenuItem *self = SHELL_BACKGROUND_APP_MENU_ITEM (object);

  switch (prop_id)
    {
    case ITEM_PROP_APP:
      g_value_set_object (value, self->app);
      break;

    case ITEM_PROP_MESSAGE:
      g_value_set_string (value, self->message);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_background_app_menu_item_set_property (GObject      *object,
                                             guint         prop_id,
                                             const GValue *value,
                                             GParamSpec   *pspec)
{
  ShellBackgroundAppMenuItem *self = SHELL_BACKGROUND_APP_MENU_ITEM (object);

  switch (prop_id)
    {
    case ITEM_PROP_APP:
      if (g_set_object (&self->app, g_value_get_object (value)))
        g_object_notify_by_pspec (object, pspec);
      break;

    case ITEM_PROP_MESSAGE:
      item_set_message (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_background_app_menu_item_constructed (GObject *object)
{
  ShellBackgroundAppMenuItem *self = SHELL_BACKGROUND_APP_MENU_ITEM (object);
  ShellPopupImageMenuItem *image_item = SHELL_POPUP_IMAGE_MENU_ITEM (object);
  ClutterActor *actor = CLUTTER_ACTOR (object);
  StLabel *label;
  ClutterActor *box;
  ClutterActor *message_label;
  ClutterActor *spinner;
  ClutterActor *close_button;

  G_OBJECT_CLASS (shell_background_app_menu_item_parent_class)->constructed (object);

  st_widget_add_style_class_name (ST_WIDGET (self), "background-app-item");

  label = shell_popup_image_menu_item_get_label (image_item);
  st_widget_add_style_class_name (ST_WIDGET (label), "title");

  box = g_object_new (ST_TYPE_BOX_LAYOUT,
                      "orientation", CLUTTER_ORIENTATION_VERTICAL,
                      "x-expand", TRUE,
                      "x-align", CLUTTER_ACTOR_ALIGN_START,
                      "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                      NULL);
  clutter_actor_add_child (actor, box);

  g_object_ref (label);
  clutter_actor_remove_child (actor, CLUTTER_ACTOR (label));
  clutter_actor_add_child (box, CLUTTER_ACTOR (label));
  g_object_unref (label);

  message_label = g_object_new (ST_TYPE_LABEL,
                                "y-expand", TRUE,
                                "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                "style-class", "subtitle",
                                NULL);
  clutter_actor_add_child (box, message_label);
  g_set_weak_pointer (&self->message_label, ST_LABEL (message_label));
  st_label_set_text (self->message_label, self->message != NULL ? self->message : "");
  shell_status_set_visible (self->message_label, self->message != NULL);

  clutter_actor_set_child_above_sibling (actor,
                                         CLUTTER_ACTOR (shell_popup_base_menu_item_get_ornament_icon (SHELL_POPUP_BASE_MENU_ITEM (self))),
                                         NULL);

  spinner = CLUTTER_ACTOR (shell_spinner_new (16, FALSE, TRUE));
  st_widget_add_style_class_name (ST_WIDGET (spinner), "spinner");
  clutter_actor_add_child (actor, spinner);
  g_set_weak_pointer (&self->spinner, SHELL_SPINNER (spinner));

  close_button = g_object_new (ST_TYPE_BUTTON,
                               "icon-name", "window-close-symbolic",
                               "style-class", "icon-button",
                               "x-expand", TRUE,
                               "y-expand", FALSE,
                               "x-align", CLUTTER_ACTOR_ALIGN_END,
                               "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                               NULL);
  clutter_actor_add_child (actor, close_button);

  g_object_bind_property (spinner, "visible",
                          close_button, "visible",
                          G_BINDING_INVERT_BOOLEAN);

  g_signal_connect_object (close_button, "clicked",
                           G_CALLBACK (item_close_button_clicked_cb), self, 0);
}

static void
shell_background_app_menu_item_destroy (ClutterActor *actor)
{
  ShellBackgroundAppMenuItem *self = SHELL_BACKGROUND_APP_MENU_ITEM (actor);

  item_stop_spinner_timeout (self);
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  CLUTTER_ACTOR_CLASS (shell_background_app_menu_item_parent_class)->destroy (actor);
}

static void
shell_background_app_menu_item_dispose (GObject *object)
{
  ShellBackgroundAppMenuItem *self = SHELL_BACKGROUND_APP_MENU_ITEM (object);

  item_stop_spinner_timeout (self);
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_weak_pointer (&self->message_label);
  g_clear_weak_pointer (&self->spinner);
  g_clear_object (&self->app);

  G_OBJECT_CLASS (shell_background_app_menu_item_parent_class)->dispose (object);
}

static void
shell_background_app_menu_item_finalize (GObject *object)
{
  ShellBackgroundAppMenuItem *self = SHELL_BACKGROUND_APP_MENU_ITEM (object);

  g_clear_pointer (&self->message, g_free);

  G_OBJECT_CLASS (shell_background_app_menu_item_parent_class)->finalize (object);
}

static void
shell_background_app_menu_item_class_init (ShellBackgroundAppMenuItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_background_app_menu_item_constructed;
  object_class->dispose = shell_background_app_menu_item_dispose;
  object_class->finalize = shell_background_app_menu_item_finalize;
  object_class->get_property = shell_background_app_menu_item_get_property;
  object_class->set_property = shell_background_app_menu_item_set_property;

  actor_class->destroy = shell_background_app_menu_item_destroy;

  item_props[ITEM_PROP_APP] =
    g_param_spec_object ("app", NULL, NULL,
                         SHELL_TYPE_APP,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  item_props[ITEM_PROP_MESSAGE] =
    g_param_spec_string ("message", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_ITEM_PROPS, item_props);
}

static void
shell_background_app_menu_item_init (ShellBackgroundAppMenuItem *self)
{
}

static ShellBackgroundAppMenuItem *
shell_background_app_menu_item_new (ShellApp *app)
{
  return g_object_new (SHELL_TYPE_BACKGROUND_APP_MENU_ITEM,
                       "text", shell_app_get_name (app),
                       "gicon", shell_app_get_icon (app),
                       "app", app,
                       NULL);
}

/* ======================================================================== */
/* ShellBackgroundAppsToggle                                                */
/* ======================================================================== */

enum
{
  TOGGLE_OVERVIEW_HIDE_REQUESTED,
  N_TOGGLE_SIGNALS
};

static guint toggle_signals[N_TOGGLE_SIGNALS] = { 0, };

#define SHELL_TYPE_BACKGROUND_APPS_TOGGLE (shell_background_apps_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellBackgroundAppsToggle, shell_background_apps_toggle,
                      SHELL, BACKGROUND_APPS_TOGGLE, ShellQuickToggle)

struct _ShellBackgroundAppsToggle
{
  ShellQuickToggle parent;

  ShellAppSystem *app_system;
  ShellBackgroundMonitor *proxy;
  GCancellable *cancellable;

  ShellPopupMenuItem *list_title;           /* weak */
  ShellPopupMenuSection *apps_section;      /* weak */
};

G_DEFINE_FINAL_TYPE (ShellBackgroundAppsToggle, shell_background_apps_toggle,
                     SHELL_TYPE_QUICK_TOGGLE)

static guint
toggle_get_n_background_apps (ShellBackgroundAppsToggle *self)
{
  GVariant *apps;

  if (self->proxy == NULL)
    return 0;

  apps = shell_background_monitor_get_background_apps (self->proxy);
  return apps != NULL ? g_variant_n_children (apps) : 0;
}

/* _syncVisibility () */
static void
toggle_sync_visibility (ShellBackgroundAppsToggle *self)
{
  ShellSessionMode *session_mode = shell_status_get_session_mode ();
  ShellQuickToggleMenu *menu = shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self));
  gboolean is_locked = session_mode != NULL && shell_session_mode_is_locked (session_mode);
  gboolean is_open = menu != NULL && shell_popup_menu_base_get_is_open (SHELL_POPUP_MENU_BASE (menu));

  /* We cannot hide the quick toggle while the menu is open, otherwise
   * the menu position goes bogus. We can't show it in locked sessions
   * either */
  shell_status_set_visible (self,
                            !is_locked &&
                            (is_open || toggle_get_n_background_apps (self) > 0));
}

typedef struct
{
  ShellApp *app;        /* strong */
  char *message;
} BackgroundApp;

static void
background_app_free (BackgroundApp *background_app)
{
  g_clear_object (&background_app->app);
  g_free (background_app->message);
  g_free (background_app);
}

static int
background_app_compare (gconstpointer a,
                        gconstpointer b)
{
  const BackgroundApp *background_app_a = *(BackgroundApp * const *) a;
  const BackgroundApp *background_app_b = *(BackgroundApp * const *) b;

  return g_utf8_collate (shell_app_get_name (background_app_a->app),
                         shell_app_get_name (background_app_b->app));
}

static void
item_activate_cb (ShellPopupBaseMenuItem   *item,
                  const ClutterEvent       *event,
                  ShellBackgroundAppsToggle *self)
{
  ShellBackgroundAppMenuItem *app_item = SHELL_BACKGROUND_APP_MENU_ITEM (item);
  ShellPanel *panel = shell_main_get_panel (shell_main_get_default ());
  g_autoptr (ShellApp) app = app_item->app != NULL ? g_object_ref (app_item->app) : NULL;

  g_signal_emit (self, toggle_signals[TOGGLE_OVERVIEW_HIDE_REQUESTED], 0);
  if (panel != NULL)
    shell_panel_close_quick_settings (panel);
  if (app != NULL)
    shell_app_activate (app);
}

static void
toggle_sync (ShellBackgroundAppsToggle *self)
{
  g_autoptr (GPtrArray) background_apps = NULL;
  g_autoptr (GHashTable) items = NULL;
  GVariant *apps;
  GVariantIter iter;
  GVariant *dict;
  guint n_background_apps;
  guint i;

  toggle_sync_visibility (self);

  if (self->proxy == NULL || self->apps_section == NULL)
    return;

  apps = shell_background_monitor_get_background_apps (self->proxy);

  shell_popup_menu_base_remove_all (SHELL_POPUP_MENU_BASE (self->apps_section));

  background_apps = g_ptr_array_new_with_free_func ((GDestroyNotify) background_app_free);
  if (apps != NULL)
    {
      g_variant_iter_init (&iter, apps);
      while ((dict = g_variant_iter_next_value (&iter)) != NULL)
        {
          g_autoptr (GVariant) owned = dict;
          const char *app_id = NULL;
          const char *message = NULL;
          g_autofree char *desktop_id = NULL;
          ShellApp *app;
          BackgroundApp *background_app;

          if (!g_variant_lookup (owned, "app_id", "&s", &app_id))
            continue;
          g_variant_lookup (owned, "message", "&s", &message);

          desktop_id = g_strdup_printf ("%s.desktop", app_id);
          app = shell_app_system_lookup_app (self->app_system, desktop_id);
          if (app == NULL)
            continue;

          background_app = g_new0 (BackgroundApp, 1);
          background_app->app = g_object_ref (app);
          background_app->message = g_strdup (message);
          g_ptr_array_add (background_apps, background_app);
        }
    }

  g_ptr_array_sort (background_apps, background_app_compare);

  items = g_hash_table_new (NULL, NULL);
  for (i = 0; i < background_apps->len; i++)
    {
      BackgroundApp *background_app = g_ptr_array_index (background_apps, i);
      ShellBackgroundAppMenuItem *item = g_hash_table_lookup (items, background_app->app);

      if (item == NULL)
        {
          item = shell_background_app_menu_item_new (background_app->app);
          g_signal_connect_object (item, "activate",
                                   G_CALLBACK (item_activate_cb), self, 0);
          g_hash_table_insert (items, background_app->app, item);
          shell_popup_menu_base_add_menu_item (SHELL_POPUP_MENU_BASE (self->apps_section),
                                               G_OBJECT (item));
        }

      if (background_app->message != NULL && background_app->message[0] != '\0')
        item_set_message (item, background_app->message);
    }

  n_background_apps = g_hash_table_size (items);
  if (n_background_apps == 0)
    {
      shell_quick_toggle_set_title (SHELL_QUICK_TOGGLE (self), _("No Background Apps"));
    }
  else
    {
      g_autofree char *title =
        g_strdup_printf (ngettext ("%d Background App",
                                   "%d Background Apps",
                                   n_background_apps),
                         n_background_apps);

      shell_quick_toggle_set_title (SHELL_QUICK_TOGGLE (self), title);
    }

  if (self->list_title != NULL)
    shell_status_set_visible (self->list_title, n_background_apps > 0);
}

static void
proxy_properties_changed_cb (GDBusProxy                *proxy,
                             GVariant                  *changed_properties,
                             const char * const        *invalidated_properties,
                             ShellBackgroundAppsToggle *self)
{
  toggle_sync (self);
}

static void
proxy_ready_cb (GObject      *source,
                GAsyncResult *res,
                gpointer      user_data)
{
  g_autoptr (ShellBackgroundAppsToggle) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (ShellBackgroundMonitor) proxy = NULL;

  proxy = shell_background_monitor_proxy_new_for_bus_finish (res, &error);

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
toggle_release (ShellBackgroundAppsToggle *self)
{
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->proxy);
}

static void
on_shutdown (ShellGlobal                *global,
             ShellBackgroundAppsToggle *self)
{
  toggle_release (self);
}

static void
toggle_open_menu (ShellBackgroundAppsToggle *self)
{
  ShellQuickToggleMenu *menu = shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self));

  if (menu != NULL)
    shell_popup_menu_base_open (SHELL_POPUP_MENU_BASE (menu), SHELL_POPUP_ANIMATION_FULL);
}

static void
shell_background_apps_toggle_clicked (StButton *button,
                                      int       clicked_button)
{
  toggle_open_menu (SHELL_BACKGROUND_APPS_TOGGLE (button));
}

static void
toggle_popup_menu_cb (ShellBackgroundAppsToggle *self)
{
  toggle_open_menu (self);
}

static void
shell_background_apps_toggle_constructed (GObject *object)
{
  ShellBackgroundAppsToggle *self = SHELL_BACKGROUND_APPS_TOGGLE (object);
  ShellQuickToggleMenu *menu;
  ShellPopupMenuBase *menu_base;
  ShellSessionMode *session_mode = shell_status_get_session_mode ();
  g_autoptr (ShellPopupMenuSection) section = NULL;
  g_autoptr (ShellPopupSeparatorMenuItem) separator = NULL;
  ShellPopupMenuItem *list_title;

  G_OBJECT_CLASS (shell_background_apps_toggle_parent_class)->constructed (object);

  menu = shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self));
  menu_base = SHELL_POPUP_MENU_BASE (menu);

  st_widget_add_style_class_name (ST_WIDGET (self), "background-apps-quick-toggle");

  clutter_actor_set_child_above_sibling (CLUTTER_ACTOR (shell_quick_toggle_get_box (SHELL_QUICK_TOGGLE (self))),
                                         CLUTTER_ACTOR (shell_quick_toggle_get_icon (SHELL_QUICK_TOGGLE (self))),
                                         NULL);

  self->app_system = shell_app_system_get_default ();

  shell_quick_toggle_menu_set_header (menu, "background-app-ghost-symbolic",
                                      C_("title", "Background Apps"), NULL);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  self->cancellable = g_cancellable_new ();
  shell_background_monitor_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                              G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                              DBUS_NAME, DBUS_OBJECT_PATH,
                                              self->cancellable,
                                              proxy_ready_cb,
                                              g_object_ref (self));

  list_title = g_object_new (SHELL_TYPE_POPUP_MENU_ITEM,
                             "text", _("Apps known to be running without a window"),
                             "reactive", FALSE,
                             "activatable", FALSE,
                             "active-on-hover", FALSE,
                             NULL);
  g_object_set (st_label_get_clutter_text (shell_popup_menu_item_get_label (list_title)),
                "line-wrap", TRUE,
                NULL);
  shell_popup_menu_base_add_menu_item (menu_base, G_OBJECT (list_title));
  g_set_weak_pointer (&self->list_title, list_title);

  section = shell_popup_menu_section_new ();
  shell_popup_menu_base_add_menu_item (menu_base, G_OBJECT (section));
  g_set_weak_pointer (&self->apps_section, section);

  separator = g_object_ref_sink (g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL));
  shell_popup_menu_base_add_menu_item (menu_base, G_OBJECT (separator));
  shell_popup_menu_base_add_settings_action (menu_base, _("App Settings"),
                                             "gnome-applications-panel.desktop");

  g_signal_connect (self, "popup-menu", G_CALLBACK (toggle_popup_menu_cb), NULL);

  g_signal_connect_object (menu, "open-state-changed",
                           G_CALLBACK (toggle_sync_visibility), self, G_CONNECT_SWAPPED);
  if (session_mode != NULL)
    g_signal_connect_object (session_mode, "updated",
                             G_CALLBACK (toggle_sync_visibility), self, G_CONNECT_SWAPPED);
}

static void
shell_background_apps_toggle_dispose (GObject *object)
{
  ShellBackgroundAppsToggle *self = SHELL_BACKGROUND_APPS_TOGGLE (object);

  toggle_release (self);
  g_clear_weak_pointer (&self->list_title);
  g_clear_weak_pointer (&self->apps_section);

  G_OBJECT_CLASS (shell_background_apps_toggle_parent_class)->dispose (object);
}

static void
shell_background_apps_toggle_class_init (ShellBackgroundAppsToggleClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->constructed = shell_background_apps_toggle_constructed;
  object_class->dispose = shell_background_apps_toggle_dispose;

  button_class->clicked = shell_background_apps_toggle_clicked;

  toggle_signals[TOGGLE_OVERVIEW_HIDE_REQUESTED] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_background_apps_toggle_init (ShellBackgroundAppsToggle *self)
{
}

static ShellBackgroundAppsToggle *
shell_background_apps_toggle_new (void)
{
  return g_object_new (SHELL_TYPE_BACKGROUND_APPS_TOGGLE,
                       "visible", FALSE,
                       "has-menu", TRUE,
                       /* The background apps toggle looks like a flat menu, but doesn't
                        * have a separate menu button. Fake it with an arrow icon. */
                       "icon-name", "go-next-symbolic",
                       NULL);
}

/* ======================================================================== */
/* ShellBackgroundAppsIndicator                                             */
/* ======================================================================== */

enum
{
  OVERVIEW_HIDE_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0, };

struct _ShellBackgroundAppsIndicator
{
  ShellSystemIndicator parent;
};

G_DEFINE_FINAL_TYPE (ShellBackgroundAppsIndicator, shell_background_apps_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
toggle_overview_hide_requested_cb (ShellBackgroundAppsToggle    *toggle,
                                   ShellBackgroundAppsIndicator *self)
{
  g_signal_emit (self, signals[OVERVIEW_HIDE_REQUESTED], 0);
}

static void
shell_background_apps_indicator_constructed (GObject *object)
{
  ShellBackgroundAppsIndicator *self = SHELL_BACKGROUND_APPS_INDICATOR (object);
  ShellBackgroundAppsToggle *toggle;

  G_OBJECT_CLASS (shell_background_apps_indicator_parent_class)->constructed (object);

  toggle = shell_background_apps_toggle_new ();
  g_signal_connect_object (toggle, "overview-hide-requested",
                           G_CALLBACK (toggle_overview_hide_requested_cb), self, 0);
  shell_system_indicator_add_quick_settings_item (SHELL_SYSTEM_INDICATOR (self),
                                                  CLUTTER_ACTOR (toggle));
}

static void
shell_background_apps_indicator_class_init (ShellBackgroundAppsIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_background_apps_indicator_constructed;

  /**
   * ShellBackgroundAppsIndicator::overview-hide-requested:
   * @self: the #ShellBackgroundAppsIndicator
   *
   * Emitted when the user opens an application from the menu, before the
   * quick settings menu closes. The handler hides the overview
   * (`Main.overview.hide ()`).
   */
  signals[OVERVIEW_HIDE_REQUESTED] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_background_apps_indicator_init (ShellBackgroundAppsIndicator *self)
{
}

/**
 * shell_background_apps_indicator_new:
 *
 * Creates the indicator with the entry which lists the applications that run
 * in the background (`new Indicator ()`).
 *
 * Returns: (transfer floating): a new #ShellBackgroundAppsIndicator
 */
ShellBackgroundAppsIndicator *
shell_background_apps_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_BACKGROUND_APPS_INDICATOR, NULL);
}
