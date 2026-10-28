/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * ctrl-alt-tab.c: The keyboard navigation between the focus groups of the
 * shell and the windows of the workspace. Port of js/ui/ctrlAltTab.js.
 *
 * Differences from the JS classes, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `addGroup (root, name, icon, {sortGroup, proxy, focusCallback})` is
 *    shell_ctrl_alt_tab_manager_add_group () for the groups with the default
 *    parameters and shell_ctrl_alt_tab_manager_add_group_full () for the
 *    others.
 *  - `popup ()` takes `Main.overview.visible` as an argument, as the overview
 *    is not reachable from C yet. `Main.sessionMode` is read from
 *    #ShellMain:session-mode.
 *  - `Main.activateWindow ()` is done here (switching to the workspace of the
 *    window and activating it); what it does on top of that, hiding the
 *    overview and closing the calendar, is up to the handler main.js connects
 *    to #ShellCtrlAltTabManager::window-activated.
 *  - A window of the workspace without an application gets the generic
 *    application icon; the JS code threw an error.
 *  - The 'destroy' handler that forgets the popup (`_popup`) is connected
 *    before the popup is shown. The JS code connected it after, which missed
 *    a popup that show () destroyed at once and left Ctrl+Alt+Tab dead.
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include <clutter/clutter.h>
#include <meta/display.h>
#include <meta/meta-workspace-manager.h>
#include <meta/prefs.h>
#include <meta/window.h>
#include <meta/workspace.h>

#include "shell-app.h"
#include "shell-global.h"
#include "shell-window-tracker.h"
#include "ui/ctrl-alt-tab.h"
#include "ui/main.h"
#include "ui/session-mode.h"
#include "ui/switcher-popup.h"

#define POPUP_APPICON_SIZE 96

/* ------------------------------------------------------------------------ */
/* CtrlAltTabItem: one entry of the popup                                    */
/* ------------------------------------------------------------------------ */

typedef struct
{
  grefcount ref_count;

  ClutterActor *root;          /* weak, nullable (a window has none) */
  gulong root_destroy_id;
  ClutterActor *proxy;         /* weak, the actor that tells whether the
                                * group is shown and where */
  char *name;
  char *icon_name;
  ClutterActor *icon_actor;    /* strong, nullable */
  ShellCtrlAltTabSortGroup sort_group;

  ShellCtrlAltTabFocusCallback focus_callback;
  gpointer user_data;
  GDestroyNotify destroy;

  MetaWindow *window;          /* weak, set for the windows of the workspace */
} CtrlAltTabItem;

static CtrlAltTabItem *
item_new (const char               *name,
          const char               *icon_name,
          ShellCtrlAltTabSortGroup  sort_group)
{
  CtrlAltTabItem *item = g_new0 (CtrlAltTabItem, 1);

  g_ref_count_init (&item->ref_count);
  item->name = g_strdup (name);
  item->icon_name = g_strdup (icon_name);
  item->sort_group = sort_group;

  return item;
}

static CtrlAltTabItem *
item_ref (CtrlAltTabItem *item)
{
  g_ref_count_inc (&item->ref_count);
  return item;
}

static void
item_unref (CtrlAltTabItem *item)
{
  if (!g_ref_count_dec (&item->ref_count))
    return;

  g_clear_weak_pointer (&item->root);
  g_clear_weak_pointer (&item->proxy);
  g_clear_weak_pointer (&item->window);
  g_clear_object (&item->icon_actor);
  g_clear_pointer (&item->name, g_free);
  g_clear_pointer (&item->icon_name, g_free);

  if (item->destroy != NULL)
    item->destroy (item->user_data);

  g_free (item);
}

static int
sort_items (gconstpointer a,
            gconstpointer b)
{
  const CtrlAltTabItem *item_a = a;
  const CtrlAltTabItem *item_b = b;
  float ax = 0, bx = 0;

  if (item_a->sort_group != item_b->sort_group)
    return (int) item_a->sort_group - (int) item_b->sort_group;

  if (item_a->proxy != NULL)
    clutter_actor_get_transformed_position (item_a->proxy, &ax, NULL);
  if (item_b->proxy != NULL)
    clutter_actor_get_transformed_position (item_b->proxy, &bx, NULL);

  if (ax < bx)
    return -1;
  else if (ax > bx)
    return 1;

  return 0;
}

/* ------------------------------------------------------------------------ */
/* ShellCtrlAltTabManager                                                    */
/* ------------------------------------------------------------------------ */

enum
{
  WINDOW_ACTIVATED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellCtrlAltTabManager
{
  GObject parent;

  GPtrArray *items;            /* CtrlAltTabItem */
  StFocusManager *focus_manager; /* weak */
  ClutterActor *popup;         /* weak */
};

G_DEFINE_FINAL_TYPE (ShellCtrlAltTabManager, shell_ctrl_alt_tab_manager, G_TYPE_OBJECT)

static void
focus_windows_cb (uint32_t  timestamp,
                  gpointer  user_data)
{
  meta_display_focus_default_window (shell_global_get_display (shell_global_get ()),
                                     timestamp);
}

static void
root_destroyed_cb (ClutterActor           *root,
                   ShellCtrlAltTabManager *self)
{
  shell_ctrl_alt_tab_manager_remove_group (self, root);
}

/**
 * shell_ctrl_alt_tab_manager_add_group_full:
 * @self: a #ShellCtrlAltTabManager
 * @root: the actor that is the root of the group. The group is removed when
 *   it is destroyed. If it is a #StWidget it becomes a focus group of the
 *   shell
 * @name: the name of the group in the popup
 * @icon_name: the name of the icon of the group in the popup
 * @sort_group: where the group is shown in the popup
 * @proxy: (nullable): the actor that tells whether the group is mapped and
 *   where it is on screen. @root when %NULL
 * @focus_callback: (nullable) (scope notified) (closure user_data) (destroy destroy):
 *   called to give the key focus to the group. When %NULL the focus
 *   navigates into @root
 * @user_data: data for @focus_callback
 * @destroy: called on @user_data when the group is removed
 *
 * Adds a group to the popup (`addGroup (root, name, icon, {sortGroup, proxy,
 * focusCallback})`).
 */
void
shell_ctrl_alt_tab_manager_add_group_full (ShellCtrlAltTabManager       *self,
                                           ClutterActor                 *root,
                                           const char                   *name,
                                           const char                   *icon_name,
                                           ShellCtrlAltTabSortGroup      sort_group,
                                           ClutterActor                 *proxy,
                                           ShellCtrlAltTabFocusCallback  focus_callback,
                                           gpointer                      user_data,
                                           GDestroyNotify                destroy)
{
  CtrlAltTabItem *item;

  g_return_if_fail (SHELL_IS_CTRL_ALT_TAB_MANAGER (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (root));
  g_return_if_fail (proxy == NULL || CLUTTER_IS_ACTOR (proxy));

  item = item_new (name, icon_name, sort_group);
  g_set_weak_pointer (&item->root, root);
  g_set_weak_pointer (&item->proxy, proxy != NULL ? proxy : root);
  item->focus_callback = focus_callback;
  item->user_data = user_data;
  item->destroy = destroy;

  g_ptr_array_add (self->items, item);
  item->root_destroy_id = g_signal_connect (root, "destroy",
                                            G_CALLBACK (root_destroyed_cb), self);

  if (ST_IS_WIDGET (root) && self->focus_manager != NULL)
    st_focus_manager_add_group (self->focus_manager, ST_WIDGET (root));
}

/**
 * shell_ctrl_alt_tab_manager_add_group:
 * @self: a #ShellCtrlAltTabManager
 * @root: the actor that is the root of the group
 * @name: the name of the group in the popup
 * @icon_name: the name of the icon of the group in the popup
 *
 * Adds a group in the middle of the popup, which gets the focus by
 * navigating into @root. See shell_ctrl_alt_tab_manager_add_group_full ().
 */
void
shell_ctrl_alt_tab_manager_add_group (ShellCtrlAltTabManager *self,
                                      ClutterActor           *root,
                                      const char             *name,
                                      const char             *icon_name)
{
  shell_ctrl_alt_tab_manager_add_group_full (self, root, name, icon_name,
                                             SHELL_CTRL_ALT_TAB_SORT_GROUP_MIDDLE,
                                             NULL, NULL, NULL, NULL);
}

/**
 * shell_ctrl_alt_tab_manager_remove_group:
 * @self: a #ShellCtrlAltTabManager
 * @root: the root actor given to shell_ctrl_alt_tab_manager_add_group ()
 *
 * Removes the first group with @root (`removeGroup ()`).
 */
void
shell_ctrl_alt_tab_manager_remove_group (ShellCtrlAltTabManager *self,
                                         ClutterActor           *root)
{
  guint i;

  g_return_if_fail (SHELL_IS_CTRL_ALT_TAB_MANAGER (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (root));

  if (ST_IS_WIDGET (root) && self->focus_manager != NULL)
    st_focus_manager_remove_group (self->focus_manager, ST_WIDGET (root));

  for (i = 0; i < self->items->len; i++)
    {
      CtrlAltTabItem *item = g_ptr_array_index (self->items, i);

      if (item->root == root)
        {
          if (item->root_destroy_id != 0)
            g_signal_handler_disconnect (root, item->root_destroy_id);
          item->root_destroy_id = 0;
          g_ptr_array_remove_index (self->items, i);
          return;
        }
    }
}

static void
activate_window (ShellCtrlAltTabManager *self,
                 MetaWindow             *window,
                 uint32_t                time)
{
  ShellGlobal *global = shell_global_get ();
  MetaWorkspaceManager *workspace_manager = shell_global_get_workspace_manager (global);
  MetaWorkspace *workspace = meta_window_get_workspace (window);

  if (time == 0)
    time = shell_global_get_current_time (global);

  if (workspace != NULL &&
      workspace != meta_workspace_manager_get_active_workspace (workspace_manager))
    meta_workspace_activate_with_focus (workspace, window, time);
  else
    meta_window_activate (window, time);

  g_signal_emit (self, signals[WINDOW_ACTIVATED], 0, window, time);
}

static void
focus_group (ShellCtrlAltTabManager *self,
             CtrlAltTabItem         *item,
             uint32_t                timestamp)
{
  if (item->focus_callback != NULL)
    item->focus_callback (timestamp, item->user_data);
  else if (item->window != NULL)
    activate_window (self, item->window, timestamp);
  else if (item->root != NULL && ST_IS_WIDGET (item->root))
    st_widget_navigate_focus (ST_WIDGET (item->root), NULL, ST_DIR_TAB_FORWARD, FALSE);
}

/* ------------------------------------------------------------------------ */
/* ShellCtrlAltTabSwitcher: the list of the popup                            */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_CTRL_ALT_TAB_SWITCHER (shell_ctrl_alt_tab_switcher_get_type ())
G_DECLARE_FINAL_TYPE (ShellCtrlAltTabSwitcher, shell_ctrl_alt_tab_switcher,
                      SHELL, CTRL_ALT_TAB_SWITCHER, ShellSwitcherList)

struct _ShellCtrlAltTabSwitcher
{
  ShellSwitcherList parent;
};

G_DEFINE_FINAL_TYPE (ShellCtrlAltTabSwitcher, shell_ctrl_alt_tab_switcher,
                     SHELL_TYPE_SWITCHER_LIST)

static void
switcher_add_icon (ShellCtrlAltTabSwitcher *self,
                   CtrlAltTabItem          *item)
{
  ClutterActor *box, *icon, *text;

  box = g_object_new (ST_TYPE_BOX_LAYOUT,
                      "style-class", "alt-tab-app",
                      "orientation", CLUTTER_ORIENTATION_VERTICAL,
                      NULL);

  icon = item->icon_actor;
  if (icon == NULL)
    {
      icon = g_object_new (ST_TYPE_ICON,
                           "icon-name", item->icon_name,
                           "icon-size", POPUP_APPICON_SIZE,
                           NULL);
    }
  clutter_actor_add_child (box, icon);

  text = g_object_new (ST_TYPE_LABEL,
                       "text", item->name,
                       "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       NULL);
  clutter_actor_add_child (box, text);

  shell_switcher_list_add_item (SHELL_SWITCHER_LIST (self), box, text);
}

static void
shell_ctrl_alt_tab_switcher_class_init (ShellCtrlAltTabSwitcherClass *klass)
{
}

static void
shell_ctrl_alt_tab_switcher_init (ShellCtrlAltTabSwitcher *self)
{
}

static ShellCtrlAltTabSwitcher *
switcher_new (GPtrArray *items)
{
  ShellCtrlAltTabSwitcher *self;
  guint i;

  self = g_object_new (SHELL_TYPE_CTRL_ALT_TAB_SWITCHER,
                       "square-items", TRUE,
                       NULL);

  for (i = 0; i < items->len; i++)
    switcher_add_icon (self, g_ptr_array_index (items, i));

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellCtrlAltTabPopup: the popup                                           */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_CTRL_ALT_TAB_POPUP (shell_ctrl_alt_tab_popup_get_type ())
G_DECLARE_FINAL_TYPE (ShellCtrlAltTabPopup, shell_ctrl_alt_tab_popup,
                      SHELL, CTRL_ALT_TAB_POPUP, ShellSwitcherPopup)

struct _ShellCtrlAltTabPopup
{
  ShellSwitcherPopup parent;

  GPtrArray *items;            /* CtrlAltTabItem */
  ShellCtrlAltTabManager *manager; /* weak */
};

G_DEFINE_FINAL_TYPE (ShellCtrlAltTabPopup, shell_ctrl_alt_tab_popup,
                     SHELL_TYPE_SWITCHER_POPUP)

static int
popup_get_n_items (ShellSwitcherPopup *popup)
{
  ShellCtrlAltTabPopup *self = SHELL_CTRL_ALT_TAB_POPUP (popup);

  return self->items != NULL ? self->items->len : 0;
}

static gboolean
popup_key_press_handler (ShellSwitcherPopup *popup,
                         uint32_t            keysym,
                         uint32_t            action)
{
  if (action == META_KEYBINDING_ACTION_SWITCH_PANELS)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_next_index (popup));
  else if (action == META_KEYBINDING_ACTION_SWITCH_PANELS_BACKWARD)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_previous_index (popup));
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
  ShellCtrlAltTabPopup *self = SHELL_CTRL_ALT_TAB_POPUP (popup);
  int selected_index = shell_switcher_popup_get_selected_index (popup);
  g_autoptr (ShellCtrlAltTabManager) manager = NULL;
  CtrlAltTabItem *item = NULL;

  /* Chaining up destroys the popup */
  if (self->items != NULL && selected_index >= 0 &&
      (guint) selected_index < self->items->len)
    item = item_ref (g_ptr_array_index (self->items, selected_index));
  if (self->manager != NULL)
    manager = g_object_ref (self->manager);

  SHELL_SWITCHER_POPUP_CLASS (shell_ctrl_alt_tab_popup_parent_class)->finish (popup, timestamp);

  if (item != NULL)
    {
      if (manager != NULL)
        focus_group (manager, item, timestamp);
      item_unref (item);
    }
}

static void
shell_ctrl_alt_tab_popup_dispose (GObject *object)
{
  ShellCtrlAltTabPopup *self = SHELL_CTRL_ALT_TAB_POPUP (object);

  g_clear_weak_pointer (&self->manager);

  G_OBJECT_CLASS (shell_ctrl_alt_tab_popup_parent_class)->dispose (object);
}

static void
shell_ctrl_alt_tab_popup_finalize (GObject *object)
{
  ShellCtrlAltTabPopup *self = SHELL_CTRL_ALT_TAB_POPUP (object);

  g_clear_pointer (&self->items, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_ctrl_alt_tab_popup_parent_class)->finalize (object);
}

static void
shell_ctrl_alt_tab_popup_class_init (ShellCtrlAltTabPopupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellSwitcherPopupClass *popup_class = SHELL_SWITCHER_POPUP_CLASS (klass);

  object_class->dispose = shell_ctrl_alt_tab_popup_dispose;
  object_class->finalize = shell_ctrl_alt_tab_popup_finalize;

  popup_class->get_n_items = popup_get_n_items;
  popup_class->key_press_handler = popup_key_press_handler;
  popup_class->finish = popup_finish;
}

static void
shell_ctrl_alt_tab_popup_init (ShellCtrlAltTabPopup *self)
{
}

static ShellCtrlAltTabPopup *
popup_new (ShellCtrlAltTabManager *manager,
           GPtrArray              *items)
{
  ShellCtrlAltTabPopup *self;
  ShellCtrlAltTabSwitcher *switcher;

  self = g_object_new (SHELL_TYPE_CTRL_ALT_TAB_POPUP, NULL);
  self->items = g_ptr_array_ref (items);
  g_set_weak_pointer (&self->manager, manager);

  switcher = switcher_new (items);
  shell_switcher_popup_set_switcher_list (SHELL_SWITCHER_POPUP (self),
                                          ST_WIDGET (switcher));

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellCtrlAltTabManager, continued                                         */
/* ------------------------------------------------------------------------ */

static void
popup_destroyed_cb (ClutterActor           *popup,
                    ShellCtrlAltTabManager *self)
{
  if (self->popup == popup)
    g_clear_weak_pointer (&self->popup);
}

static void
add_window_items (ShellCtrlAltTabManager *self,
                  GPtrArray              *items)
{
  ShellGlobal *global = shell_global_get ();
  MetaDisplay *display = shell_global_get_display (global);
  MetaWorkspaceManager *workspace_manager = shell_global_get_workspace_manager (global);
  MetaWorkspace *active_workspace = meta_workspace_manager_get_active_workspace (workspace_manager);
  ShellWindowTracker *window_tracker = shell_window_tracker_get_default ();
  g_autoptr (GList) windows = NULL;
  GList *l;

  /* And add the windows metacity would show in its Ctrl-Alt-Tab list */
  windows = meta_display_get_tab_list (display, META_TAB_LIST_DOCKS, active_workspace);
  for (l = windows; l != NULL; l = l->next)
    {
      MetaWindow *window = l->data;
      CtrlAltTabItem *item;
      GObject *window_actor;
      const char *icon_name = NULL;
      ClutterActor *icon = NULL;

      if (meta_window_get_window_type (window) == META_WINDOW_DESKTOP)
        {
          icon_name = "shell-focus-desktop-symbolic";
        }
      else
        {
          g_autoptr (ShellApp) app = shell_window_tracker_get_window_app (window_tracker, window);

          if (app != NULL)
            icon = shell_app_create_icon_texture (app, POPUP_APPICON_SIZE);
          else
            icon_name = "application-x-executable";
        }

      item = item_new (meta_window_get_title (window) != NULL ?
                       meta_window_get_title (window) : "",
                       icon_name,
                       SHELL_CTRL_ALT_TAB_SORT_GROUP_MIDDLE);
      window_actor = meta_window_get_compositor_private (window);
      if (window_actor != NULL)
        g_set_weak_pointer (&item->proxy, CLUTTER_ACTOR (window_actor));
      g_set_weak_pointer (&item->window, window);
      if (icon != NULL)
        item->icon_actor = g_object_ref_sink (icon);

      g_ptr_array_add (items, item);
    }
}

/**
 * shell_ctrl_alt_tab_manager_popup:
 * @self: a #ShellCtrlAltTabManager
 * @backward: whether the shortcut goes backwards
 * @binding: (nullable): the name of the key binding that started the popup
 * @mask: the modifier mask of the key binding
 * @overview_visible: whether the overview is visible (`Main.overview.visible`).
 *   The windows of the workspace are not in the popup then
 *
 * Shows the popup with the groups that are mapped and the windows of the
 * workspace, unless it is shown already (`popup ()`).
 */
void
shell_ctrl_alt_tab_manager_popup (ShellCtrlAltTabManager *self,
                                  gboolean                backward,
                                  const char             *binding,
                                  uint32_t                mask,
                                  gboolean                overview_visible)
{
  g_autoptr (GPtrArray) items = NULL;
  ShellSessionMode *session_mode;
  ShellCtrlAltTabPopup *popup;
  guint i;

  g_return_if_fail (SHELL_IS_CTRL_ALT_TAB_MANAGER (self));

  items = g_ptr_array_new_with_free_func ((GDestroyNotify) item_unref);

  /* Start with the set of focus groups that are currently mapped */
  for (i = 0; i < self->items->len; i++)
    {
      CtrlAltTabItem *item = g_ptr_array_index (self->items, i);

      if (item->proxy != NULL && clutter_actor_is_mapped (item->proxy))
        g_ptr_array_add (items, item_ref (item));
    }

  session_mode = shell_main_get_session_mode (shell_main_get_default ());
  if (session_mode != NULL && !overview_visible)
    {
      gboolean has_windows = FALSE;

      g_object_get (session_mode, "has-windows", &has_windows, NULL);
      if (has_windows)
        add_window_items (self, items);
    }

  if (items->len == 0)
    return;

  g_ptr_array_sort_values (items, sort_items);

  if (self->popup != NULL)
    return;

  popup = popup_new (self, items);
  g_set_weak_pointer (&self->popup, CLUTTER_ACTOR (popup));
  g_signal_connect (popup, "destroy", G_CALLBACK (popup_destroyed_cb), self);

  if (!shell_switcher_popup_show_switcher (SHELL_SWITCHER_POPUP (popup),
                                           backward, binding, mask))
    clutter_actor_destroy (CLUTTER_ACTOR (popup));
}

static void
shell_ctrl_alt_tab_manager_constructed (GObject *object)
{
  ShellCtrlAltTabManager *self = SHELL_CTRL_ALT_TAB_MANAGER (object);
  ShellGlobal *global = shell_global_get ();

  G_OBJECT_CLASS (shell_ctrl_alt_tab_manager_parent_class)->constructed (object);

  g_set_weak_pointer (&self->focus_manager, shell_global_get_focus_manager (global));

  shell_ctrl_alt_tab_manager_add_group_full (self,
                                             shell_global_get_window_group (global),
                                             _("Windows"),
                                             "shell-focus-windows-symbolic",
                                             SHELL_CTRL_ALT_TAB_SORT_GROUP_TOP,
                                             NULL,
                                             focus_windows_cb, NULL, NULL);
}

static void
shell_ctrl_alt_tab_manager_dispose (GObject *object)
{
  ShellCtrlAltTabManager *self = SHELL_CTRL_ALT_TAB_MANAGER (object);
  guint i;

  if (self->popup != NULL)
    g_signal_handlers_disconnect_by_data (self->popup, self);
  g_clear_weak_pointer (&self->popup);

  for (i = 0; i < self->items->len; i++)
    {
      CtrlAltTabItem *item = g_ptr_array_index (self->items, i);

      if (item->root != NULL && item->root_destroy_id != 0)
        g_signal_handler_disconnect (item->root, item->root_destroy_id);
      item->root_destroy_id = 0;
    }
  g_ptr_array_set_size (self->items, 0);

  g_clear_weak_pointer (&self->focus_manager);

  G_OBJECT_CLASS (shell_ctrl_alt_tab_manager_parent_class)->dispose (object);
}

static void
shell_ctrl_alt_tab_manager_finalize (GObject *object)
{
  ShellCtrlAltTabManager *self = SHELL_CTRL_ALT_TAB_MANAGER (object);

  g_clear_pointer (&self->items, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_ctrl_alt_tab_manager_parent_class)->finalize (object);
}

static void
shell_ctrl_alt_tab_manager_class_init (ShellCtrlAltTabManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_ctrl_alt_tab_manager_constructed;
  object_class->dispose = shell_ctrl_alt_tab_manager_dispose;
  object_class->finalize = shell_ctrl_alt_tab_manager_finalize;

  /**
   * ShellCtrlAltTabManager::window-activated:
   * @self: the #ShellCtrlAltTabManager
   * @window: the window that was activated
   * @timestamp: the time of the event that selected the window
   *
   * Emitted after the popup activated a window of the workspace, to do the
   * rest of `Main.activateWindow ()`: hide the overview and close the
   * calendar.
   */
  signals[WINDOW_ACTIVATED] =
    g_signal_new ("window-activated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  META_TYPE_WINDOW,
                  G_TYPE_UINT);
}

static void
shell_ctrl_alt_tab_manager_init (ShellCtrlAltTabManager *self)
{
  self->items = g_ptr_array_new_with_free_func ((GDestroyNotify) item_unref);
}

/**
 * shell_ctrl_alt_tab_manager_new:
 *
 * Creates the manager, with the windows of the workspace as its first
 * group.
 *
 * Returns: (transfer full): a new #ShellCtrlAltTabManager
 */
ShellCtrlAltTabManager *
shell_ctrl_alt_tab_manager_new (void)
{
  return g_object_new (SHELL_TYPE_CTRL_ALT_TAB_MANAGER, NULL);
}
