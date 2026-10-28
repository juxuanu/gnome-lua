/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * window-menu.c: The menu which opens when the window menu of a window is
 * requested (right click on a titlebar, Alt+Space), and the manager which
 * shows it. Port of js/ui/windowMenu.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - "Take Screenshot" does not reach the screenshot module (screenshot-ui.c)
 *    itself. The window is painted here and the texture is handed to ShellMain
 *    through the #ShellWindowMenuManager::screenshot-requested signal, which
 *    window-manager.c forwards as #ShellWindowManager::screenshot-requested
 *    and ShellMain connects to shell_screenshot_capture_screenshot_async ().
 *  - `Main.layoutManager` and `Main.sessionMode` are read from #ShellMain.
 *  - A window menu type other than %META_WINDOW_MENU_WM logs a warning
 *    instead of throwing.
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <meta/display.h>
#include <meta/meta-window-actor.h>
#include <meta/prefs.h>
#include <meta/workspace.h>

#include "shell-global.h"
#include "ui/main.h"
#include "ui/window-menu.h"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static gboolean
session_mode_get_flag (const char *property)
{
  ShellSessionMode *session_mode = shell_main_get_session_mode (shell_main_get_default ());
  gboolean value = FALSE;

  if (session_mode == NULL)
    return FALSE;

  g_object_get (session_mode, property, &value, NULL);

  return value;
}

/* The pointer or touch sprite which the grab operation of a menu item has to
 * follow: the one of the event which activated the item, else the pointer's */
static ClutterSprite *
get_sprite_for_event (const ClutterEvent *event)
{
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());
  ClutterBackend *backend =
    clutter_context_get_backend (clutter_actor_get_context (CLUTTER_ACTOR (stage)));
  ClutterSprite *sprite = NULL;

  if (event != NULL)
    sprite = clutter_backend_get_sprite (backend, stage, event);
  if (sprite == NULL)
    sprite = clutter_backend_get_pointer_sprite (backend, stage);

  return sprite;
}

static uint32_t
get_event_time (const ClutterEvent *event)
{
  if (event != NULL)
    return clutter_event_get_time (event);

  return shell_global_get_current_time (shell_global_get ());
}

/* ------------------------------------------------------------------------ */
/* ShellWindowMenu                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_WINDOW,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  SCREENSHOT_REQUESTED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellWindowMenu
{
  ShellPopupMenu parent;

  MetaWindow *window;
};

G_DEFINE_FINAL_TYPE (ShellWindowMenu, shell_window_menu, SHELL_TYPE_POPUP_MENU)

/* What the closure of a menu item captured in the JS */
typedef struct
{
  MetaWindow *window;
  MetaWorkspace *workspace;     /* nullable */
  ShellWindowMenu *menu;        /* weak */
  int arg;                      /* direction or monitor index */
  gboolean flag;                /* isSticky */
} ItemData;

static ItemData *
item_data_new (ShellWindowMenu *menu,
               MetaWindow      *window)
{
  ItemData *data = g_new0 (ItemData, 1);

  data->window = g_object_ref (window);
  g_set_weak_pointer (&data->menu, menu);

  return data;
}

static void
item_data_free (gpointer user_data)
{
  ItemData *data = user_data;

  g_clear_object (&data->window);
  g_clear_object (&data->workspace);
  g_clear_weak_pointer (&data->menu);
  g_free (data);
}

static void
screenshot_cb (const ClutterEvent *event,
               gpointer            user_data)
{
  ItemData *data = user_data;
  g_autoptr (ClutterContent) content = NULL;
  g_autoptr (GError) error = NULL;
  GObject *actor;

  if (data->menu == NULL)
    return;

  actor = meta_window_get_compositor_private (data->window);
  if (!META_IS_WINDOW_ACTOR (actor))
    {
      g_warning ("Error capturing screenshot: the window has no actor");
      return;
    }

  content = meta_window_actor_paint_to_content (META_WINDOW_ACTOR (actor),
                                                NULL, &error);
  if (content == NULL || !CLUTTER_IS_TEXTURE_CONTENT (content))
    {
      g_warning ("Error capturing screenshot: %s",
                 error != NULL ? error->message : "no texture");
      return;
    }

  g_signal_emit (data->menu, signals[SCREENSHOT_REQUESTED], 0,
                 clutter_texture_content_get_texture (CLUTTER_TEXTURE_CONTENT (content)));
}

static void
minimize_cb (const ClutterEvent *event,
             gpointer            user_data)
{
  ItemData *data = user_data;

  meta_window_minimize (data->window);
}

static void
unmaximize_cb (const ClutterEvent *event,
               gpointer            user_data)
{
  ItemData *data = user_data;

  meta_window_unmaximize (data->window);
}

static void
maximize_cb (const ClutterEvent *event,
             gpointer            user_data)
{
  ItemData *data = user_data;

  meta_window_maximize (data->window);
}

static void
move_cb (const ClutterEvent *event,
         gpointer            user_data)
{
  ItemData *data = user_data;

  meta_window_begin_grab_op (data->window,
                             META_GRAB_OP_KEYBOARD_MOVING,
                             get_sprite_for_event (event),
                             get_event_time (event),
                             NULL);
}

static void
resize_cb (const ClutterEvent *event,
           gpointer            user_data)
{
  ItemData *data = user_data;

  meta_window_begin_grab_op (data->window,
                             META_GRAB_OP_KEYBOARD_RESIZING_UNKNOWN,
                             get_sprite_for_event (event),
                             get_event_time (event),
                             NULL);
}

static void
always_on_top_cb (const ClutterEvent *event,
                  gpointer            user_data)
{
  ItemData *data = user_data;

  if (meta_window_is_above (data->window))
    meta_window_unmake_above (data->window);
  else
    meta_window_make_above (data->window);
}

static void
sticky_cb (const ClutterEvent *event,
           gpointer            user_data)
{
  ItemData *data = user_data;

  /* The state which the label was built for, as the JS closure captured it */
  if (data->flag)
    meta_window_unstick (data->window);
  else
    meta_window_stick (data->window);
}

static void
change_workspace_cb (const ClutterEvent *event,
                     gpointer            user_data)
{
  ItemData *data = user_data;

  meta_window_change_workspace (data->window,
                                meta_workspace_get_neighbor (data->workspace,
                                                             data->arg));
}

static void
move_to_monitor_cb (const ClutterEvent *event,
                    gpointer            user_data)
{
  ItemData *data = user_data;

  meta_window_move_to_monitor (data->window, data->arg);
}

static void
close_cb (const ClutterEvent *event,
          gpointer            user_data)
{
  ItemData *data = user_data;

  meta_window_delete (data->window, get_event_time (event));
}

/* WindowMenu.add_action (): the base one, but without an ornament */
static ShellPopupBaseMenuItem *
add_action (ShellWindowMenu          *self,
            const char               *label,
            ShellPopupMenuActionFunc  callback,
            ItemData                 *data)
{
  ShellPopupBaseMenuItem *item =
    shell_popup_menu_base_add_action (SHELL_POPUP_MENU_BASE (self), label,
                                      callback, data, item_data_free);

  shell_popup_base_menu_item_set_ornament (item, SHELL_ORNAMENT_NONE);

  return item;
}

static void
add_workspace_action (ShellWindowMenu   *self,
                      const char        *label,
                      MetaWindow        *window,
                      MetaWorkspace     *workspace,
                      MetaMotionDirection direction)
{
  ItemData *data = item_data_new (self, window);

  data->workspace = g_object_ref (workspace);
  data->arg = direction;

  add_action (self, label, change_workspace_cb, data);
}

static void
add_monitor_action (ShellWindowMenu *self,
                    const char      *label,
                    MetaWindow      *window,
                    int              monitor_index)
{
  ItemData *data = item_data_new (self, window);

  data->arg = monitor_index;

  add_action (self, label, move_to_monitor_cb, data);
}

static void
build_menu (ShellWindowMenu *self,
            MetaWindow      *window)
{
  ShellPopupMenuBase *base = SHELL_POPUP_MENU_BASE (self);
  MetaWindowType type = meta_window_get_window_type (window);
  ShellPopupBaseMenuItem *item;
  MetaDisplay *display;
  int n_monitors, monitor_index;

  /* Translators: entry in the window right click menu. */
  add_action (self, C_("window-menu", "Take Screenshot"), screenshot_cb,
              item_data_new (self, window));

  item = add_action (self, _("Hide"), minimize_cb, item_data_new (self, window));
  if (!meta_window_can_minimize (window))
    shell_popup_base_menu_item_set_sensitive (item, FALSE);

  if (meta_window_is_maximized (window))
    item = add_action (self, _("Restore"), unmaximize_cb, item_data_new (self, window));
  else
    item = add_action (self, _("Maximize"), maximize_cb, item_data_new (self, window));
  if (!meta_window_can_maximize (window))
    shell_popup_base_menu_item_set_sensitive (item, FALSE);

  item = add_action (self, _("Move"), move_cb, item_data_new (self, window));
  if (!meta_window_allows_move (window))
    shell_popup_base_menu_item_set_sensitive (item, FALSE);

  item = add_action (self, _("Resize"), resize_cb, item_data_new (self, window));
  if (!meta_window_allows_resize (window))
    shell_popup_base_menu_item_set_sensitive (item, FALSE);

  item = add_action (self, _("Always on Top"), always_on_top_cb,
                     item_data_new (self, window));
  if (meta_window_is_above (window))
    shell_popup_base_menu_item_set_ornament (item, SHELL_ORNAMENT_CHECK);
  if (meta_window_is_maximized (window) ||
      type == META_WINDOW_DOCK ||
      type == META_WINDOW_DESKTOP ||
      type == META_WINDOW_SPLASHSCREEN)
    shell_popup_base_menu_item_set_sensitive (item, FALSE);

  if (session_mode_get_flag ("has-workspaces") &&
      (!meta_prefs_get_workspaces_only_on_primary () ||
       meta_window_is_on_primary_monitor (window)))
    {
      gboolean is_sticky = meta_window_is_on_all_workspaces (window);
      ItemData *data = item_data_new (self, window);

      data->flag = is_sticky;
      item = add_action (self, _("Always on Visible Workspace"), sticky_cb, data);
      if (is_sticky)
        shell_popup_base_menu_item_set_ornament (item, SHELL_ORNAMENT_CHECK);
      if (meta_window_is_always_on_all_workspaces (window))
        shell_popup_base_menu_item_set_sensitive (item, FALSE);

      if (!is_sticky)
        {
          MetaWorkspace *workspace = meta_window_get_workspace (window);

          if (workspace != NULL)
            {
              if (workspace != meta_workspace_get_neighbor (workspace, META_MOTION_LEFT))
                add_workspace_action (self, _("Move to Workspace Left"),
                                      window, workspace, META_MOTION_LEFT);
              if (workspace != meta_workspace_get_neighbor (workspace, META_MOTION_RIGHT))
                add_workspace_action (self, _("Move to Workspace Right"),
                                      window, workspace, META_MOTION_RIGHT);
              if (workspace != meta_workspace_get_neighbor (workspace, META_MOTION_UP))
                add_workspace_action (self, _("Move to Workspace Up"),
                                      window, workspace, META_MOTION_UP);
              if (workspace != meta_workspace_get_neighbor (workspace, META_MOTION_DOWN))
                add_workspace_action (self, _("Move to Workspace Down"),
                                      window, workspace, META_MOTION_DOWN);
            }
        }
    }

  display = meta_window_get_display (window);
  n_monitors = meta_display_get_n_monitors (display);
  monitor_index = meta_window_get_monitor (window);
  if (n_monitors > 1 && monitor_index >= 0)
    {
      int neighbor;

      shell_popup_menu_base_add_menu_item (base,
                                           g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM,
                                                         NULL));

      neighbor = meta_display_get_monitor_neighbor_index (display, monitor_index,
                                                          META_DISPLAY_UP);
      if (neighbor != -1)
        add_monitor_action (self, _("Move to Monitor Up"), window, neighbor);

      neighbor = meta_display_get_monitor_neighbor_index (display, monitor_index,
                                                          META_DISPLAY_DOWN);
      if (neighbor != -1)
        add_monitor_action (self, _("Move to Monitor Down"), window, neighbor);

      neighbor = meta_display_get_monitor_neighbor_index (display, monitor_index,
                                                          META_DISPLAY_LEFT);
      if (neighbor != -1)
        add_monitor_action (self, _("Move to Monitor Left"), window, neighbor);

      neighbor = meta_display_get_monitor_neighbor_index (display, monitor_index,
                                                          META_DISPLAY_RIGHT);
      if (neighbor != -1)
        add_monitor_action (self, _("Move to Monitor Right"), window, neighbor);
    }

  shell_popup_menu_base_add_menu_item (base,
                                       g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM,
                                                     NULL));

  item = add_action (self, _("Close"), close_cb, item_data_new (self, window));
  if (!meta_window_can_close (window))
    shell_popup_base_menu_item_set_sensitive (item, FALSE);
}

static void
shell_window_menu_constructed (GObject *object)
{
  ShellWindowMenu *self = SHELL_WINDOW_MENU (object);
  ShellLayoutManager *layout_manager;
  ClutterActor *actor;

  G_OBJECT_CLASS (shell_window_menu_parent_class)->constructed (object);

  actor = shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (self));
  st_widget_add_style_class_name (ST_WIDGET (actor), "window-menu");

  layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  if (layout_manager != NULL)
    clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager)),
                             actor);
  else
    g_warning ("%s: no layout manager to add the window menu to", G_STRFUNC);
  clutter_actor_hide (actor);

  if (META_IS_WINDOW (self->window))
    build_menu (self, self->window);
  else
    g_warning ("%s: no window", G_STRFUNC);
}

static void
shell_window_menu_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  ShellWindowMenu *self = SHELL_WINDOW_MENU (object);

  switch (prop_id)
    {
    case PROP_WINDOW:
      g_value_set_object (value, self->window);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_window_menu_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  ShellWindowMenu *self = SHELL_WINDOW_MENU (object);

  switch (prop_id)
    {
    case PROP_WINDOW:
      g_set_object (&self->window, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_window_menu_dispose (GObject *object)
{
  ShellWindowMenu *self = SHELL_WINDOW_MENU (object);

  g_clear_object (&self->window);

  G_OBJECT_CLASS (shell_window_menu_parent_class)->dispose (object);
}

static void
shell_window_menu_class_init (ShellWindowMenuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_window_menu_constructed;
  object_class->dispose = shell_window_menu_dispose;
  object_class->get_property = shell_window_menu_get_property;
  object_class->set_property = shell_window_menu_set_property;

  /**
   * ShellWindowMenu:window:
   *
   * The window the menu is built for. The items are made when the menu is
   * constructed and are not updated afterwards.
   */
  props[PROP_WINDOW] =
    g_param_spec_object ("window", NULL, NULL,
                         META_TYPE_WINDOW,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellWindowMenu::screenshot-requested:
   * @self: the #ShellWindowMenu
   * @texture: the texture with the contents of the window
   *
   * Emitted when "Take Screenshot" is activated, with the window already
   * painted. #ShellWindowMenuManager forwards it.
   */
  signals[SCREENSHOT_REQUESTED] =
    g_signal_new ("screenshot-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, COGL_TYPE_TEXTURE);
}

static void
shell_window_menu_init (ShellWindowMenu *self)
{
}

/**
 * shell_window_menu_new:
 * @window: the window to make the menu for
 * @source_actor: the actor the menu points at
 *
 * Creates the window menu of @window (`new WindowMenu (window, sourceActor)`)
 * and adds it to the UI group, hidden.
 *
 * Returns: (transfer full): a new #ShellWindowMenu
 */
ShellWindowMenu *
shell_window_menu_new (MetaWindow   *window,
                       ClutterActor *source_actor)
{
  g_return_val_if_fail (META_IS_WINDOW (window), NULL);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (source_actor), NULL);

  return g_object_new (SHELL_TYPE_WINDOW_MENU,
                       "window", window,
                       "source-actor", source_actor,
                       "arrow-alignment", 0.0,
                       "arrow-side", ST_SIDE_TOP,
                       NULL);
}

/**
 * shell_window_menu_get_window:
 * @self: a #ShellWindowMenu
 *
 * Returns: (transfer none): the window the menu was built for
 */
MetaWindow *
shell_window_menu_get_window (ShellWindowMenu *self)
{
  g_return_val_if_fail (SHELL_IS_WINDOW_MENU (self), NULL);

  return self->window;
}

/* ------------------------------------------------------------------------ */
/* ShellWindowMenuManager                                                   */
/* ------------------------------------------------------------------------ */

enum
{
  MANAGER_SCREENSHOT_REQUESTED,

  MANAGER_LAST_SIGNAL
};

static guint manager_signals[MANAGER_LAST_SIGNAL] = { 0, };

struct _ShellWindowMenuManager
{
  GObject parent;

  ShellPopupMenuManager *manager;
  ClutterActor *source_actor;
};

G_DEFINE_FINAL_TYPE (ShellWindowMenuManager, shell_window_menu_manager, G_TYPE_OBJECT)

static void
click_recognize_cb (ClutterClickGesture     *gesture,
                    ShellWindowMenuManager  *self)
{
  GObject *active_menu = shell_popup_menu_manager_get_active_menu (self->manager);

  if (SHELL_IS_POPUP_MENU_BASE (active_menu))
    shell_popup_menu_base_toggle (SHELL_POPUP_MENU_BASE (active_menu));
}

static void
shell_window_menu_manager_constructed (GObject *object)
{
  ShellWindowMenuManager *self = SHELL_WINDOW_MENU_MANAGER (object);
  ShellLayoutManager *layout_manager;
  ClutterAction *click_gesture;

  G_OBJECT_CLASS (shell_window_menu_manager_parent_class)->constructed (object);

  self->manager = shell_popup_menu_manager_new (SHELL_ACTION_MODE_POPUP);

  self->source_actor = g_object_ref_sink (g_object_new (ST_TYPE_WIDGET,
                                                        "reactive", TRUE,
                                                        "visible", FALSE,
                                                        NULL));

  click_gesture = clutter_click_gesture_new ();
  clutter_click_gesture_set_recognize_on_press (CLUTTER_CLICK_GESTURE (click_gesture), TRUE);
  g_signal_connect (click_gesture, "recognize",
                    G_CALLBACK (click_recognize_cb), self);
  clutter_actor_add_action (self->source_actor, click_gesture);

  layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  if (layout_manager != NULL)
    clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager)),
                             self->source_actor);
  else
    g_warning ("%s: no layout manager to add the window menu source to", G_STRFUNC);
}

static void
shell_window_menu_manager_dispose (GObject *object)
{
  ShellWindowMenuManager *self = SHELL_WINDOW_MENU_MANAGER (object);

  if (self->source_actor != NULL)
    {
      /* The click gesture dies with the actor, and with it the pointer to us */
      clutter_actor_destroy (self->source_actor);
      g_clear_object (&self->source_actor);
    }
  g_clear_object (&self->manager);

  G_OBJECT_CLASS (shell_window_menu_manager_parent_class)->dispose (object);
}

static void
shell_window_menu_manager_class_init (ShellWindowMenuManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_window_menu_manager_constructed;
  object_class->dispose = shell_window_menu_manager_dispose;

  /**
   * ShellWindowMenuManager::screenshot-requested:
   * @self: the #ShellWindowMenuManager
   * @texture: the texture with the contents of the window
   *
   * Emitted when "Take Screenshot" is activated in a window menu. window-manager.c
   * passes the texture to shell_screenshot_capture_screenshot_async ().
   */
  manager_signals[MANAGER_SCREENSHOT_REQUESTED] =
    g_signal_new ("screenshot-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, COGL_TYPE_TEXTURE);
}

static void
shell_window_menu_manager_init (ShellWindowMenuManager *self)
{
}

/**
 * shell_window_menu_manager_new:
 *
 * Creates the manager of the window menus (`new WindowMenuManager ()`). It
 * adds an invisible source actor to the UI group, which the menus point at.
 * The layout manager has to be set on #ShellMain already.
 *
 * Returns: (transfer full): a new #ShellWindowMenuManager
 */
ShellWindowMenuManager *
shell_window_menu_manager_new (void)
{
  return g_object_new (SHELL_TYPE_WINDOW_MENU_MANAGER, NULL);
}

static void
menu_activate_cb (ShellPopupMenuBase     *menu,
                  ShellPopupBaseMenuItem *item,
                  MetaWindow             *window)
{
  meta_window_check_alive (window, shell_global_get_current_time (shell_global_get ()));
}

static void
window_unmanaged_cb (MetaWindow         *window,
                     ShellPopupMenuBase *menu)
{
  shell_popup_menu_base_close (menu, SHELL_POPUP_ANIMATION_NONE);
}

static void
menu_screenshot_requested_cb (ShellWindowMenu        *menu,
                              CoglTexture            *texture,
                              ShellWindowMenuManager *self)
{
  g_signal_emit (self, manager_signals[MANAGER_SCREENSHOT_REQUESTED], 0, texture);
}

static void
menu_open_state_changed_cb (ShellPopupMenuBase     *menu,
                            gboolean                is_open,
                            ShellWindowMenuManager *self)
{
  MetaWindow *window;

  if (is_open)
    return;

  window = shell_window_menu_get_window (SHELL_WINDOW_MENU (menu));

  clutter_actor_hide (self->source_actor);

  /* The menu is kept alive by the emission it is in */
  if (window != NULL)
    g_signal_handlers_disconnect_by_func (window, window_unmanaged_cb, menu);
  shell_popup_menu_base_destroy (menu);
}

/**
 * shell_window_menu_manager_show_window_menu_for_window:
 * @self: a #ShellWindowMenuManager
 * @window: the window to show the menu of
 * @type: the type of window menu; only %META_WINDOW_MENU_WM is supported
 * @rect: where the menu points at, in stage coordinates
 *
 * Opens the window menu of @window, unless the session mode has no window
 * menus (`showWindowMenuForWindow ()`). The menu is destroyed when it
 * closes, or closes without animation when @window is unmanaged.
 */
void
shell_window_menu_manager_show_window_menu_for_window (ShellWindowMenuManager *self,
                                                       MetaWindow             *window,
                                                       MetaWindowMenuType      type,
                                                       const MtkRectangle     *rect)
{
  g_autoptr (ShellWindowMenu) menu = NULL;
  ShellPopupMenuBase *base;
  ClutterActor *menu_actor;

  g_return_if_fail (SHELL_IS_WINDOW_MENU_MANAGER (self));
  g_return_if_fail (META_IS_WINDOW (window));
  g_return_if_fail (rect != NULL);

  if (!session_mode_get_flag ("has-wm-menus"))
    return;

  if (type != META_WINDOW_MENU_WM)
    {
      g_warning ("Unsupported window menu type");
      return;
    }

  menu = shell_window_menu_new (window, self->source_actor);
  base = SHELL_POPUP_MENU_BASE (menu);

  shell_popup_menu_manager_add_menu (self->manager, G_OBJECT (menu));

  g_signal_connect_object (menu, "activate",
                           G_CALLBACK (menu_activate_cb), window, 0);
  g_signal_connect_object (menu, "screenshot-requested",
                           G_CALLBACK (menu_screenshot_requested_cb), self, 0);
  g_signal_connect_object (window, "unmanaged",
                           G_CALLBACK (window_unmanaged_cb), menu, 0);

  clutter_actor_set_size (self->source_actor,
                          MAX (1, rect->width), MAX (1, rect->height));
  clutter_actor_set_position (self->source_actor, rect->x, rect->y);
  clutter_actor_show (self->source_actor);

  shell_popup_menu_base_open (base, SHELL_POPUP_ANIMATION_FADE);
  menu_actor = shell_popup_menu_base_get_actor (base);
  st_widget_navigate_focus (ST_WIDGET (menu_actor), NULL,
                            ST_DIR_TAB_FORWARD, FALSE);
  g_signal_connect_object (menu, "open-state-changed",
                           G_CALLBACK (menu_open_state_changed_cb), self, 0);
}
