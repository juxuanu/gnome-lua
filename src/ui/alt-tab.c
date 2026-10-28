/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * alt-tab.c: The popups of the window switching shortcuts. Port of
 * js/ui/altTab.js.
 *
 * Differences from the JS classes, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `Main.activateWindow ()` is done here (switching to the workspace of the
 *    window and activating it). What it does on top of that, hiding the
 *    overview and closing the calendar, is up to the handler window-manager.c
 *    connects to ::window-activated, as for #ShellCtrlAltTabManager.
 *  - The window cyclers (#ShellCyclerPopup) need two more things of
 *    `Main.wm`, which is still JS: ::skip-next-effect is emitted before a
 *    minimized window is unminimized (`Main.wm.skipNextEffect ()`) and
 *    ::move-window when the window is on another workspace
 *    (`Main.wm.actionMoveWindow ()`).
 *  - The items, lists and the highlight of the cyclers are private types
 *    (#ShellAltTabAppIcon, #ShellAltTabWindowIcon, #ShellAltTabAppSwitcher,
 *    #ShellAltTabThumbnailList, #ShellAltTabWindowSwitcher,
 *    #ShellCyclerHighlight and #ShellCyclerList) and not exported.
 *  - `_selectWindow (app, window = null, forceAppFocus = false)` takes -1 for
 *    "no window".
 *  - A thumbnail list that is fading out when a new one is created no longer
 *    clears the reference to the new list when it is destroyed (the JS popup
 *    forgot the new thumbnails then).
 *  - Windows or applications that are gone while a handler runs (an
 *    out-of-range index, a window without a compositor actor) are skipped
 *    where the JS code threw a TypeError.
 *  - The timeouts of the popup and of the application list are also removed
 *    on ::shutdown.
 */

#include "config.h"

#include <math.h>

#include <atk/atk.h>
#include <clutter/clutter.h>
#include <gio/gio.h>
#include <meta/display.h>
#include <meta/meta-window-actor.h>
#include <meta/meta-workspace-manager.h>
#include <meta/prefs.h>
#include <meta/window.h>
#include <meta/workspace.h>

#include "shell-app.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "shell-window-tracker.h"
#include "ui/alt-tab.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/switcher-popup.h"

#define APP_ICON_HOVER_TIMEOUT 200 /* milliseconds */

#define THUMBNAIL_DEFAULT_SIZE 256
#define THUMBNAIL_POPUP_TIME 500 /* milliseconds */
#define THUMBNAIL_FADE_TIME 100 /* milliseconds */

#define WINDOW_PREVIEW_SIZE 128
#define APP_ICON_SIZE 96
#define APP_ICON_SIZE_SMALL 48

/* The index of no window in _selectWindow (), `null` in the JS code */
#define WINDOW_NONE (-1)

static const int base_icon_sizes[] = { 96, 64, 48, 32, 22 };

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static int
get_scale_factor (void)
{
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());

  return st_theme_context_get_scale_factor (st_theme_context_get_for_stage (stage));
}

/* _createWindowClone () */
static ClutterActor *
create_window_clone (ClutterActor *window_actor,
                     float         size)
{
  float width, height, scale;

  clutter_actor_get_size (window_actor, &width, &height);
  scale = MIN (1.0f, MIN (size / width, size / height));

  return g_object_new (CLUTTER_TYPE_CLONE,
                       "source", window_actor,
                       "width", width * scale,
                       "height", height * scale,
                       "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       /* usual hack for the usual bug in ClutterBinLayout... */
                       "x-expand", TRUE,
                       "y-expand", TRUE,
                       NULL);
}

/**
 * shell_alt_tab_get_windows:
 * @workspace: (nullable): the workspace whose windows to list, all
 *   workspaces when %NULL
 *
 * Lists the windows the switchers show, most recently used first
 * (`getWindows ()`): the windows that are not skipped in the taskbar, with an
 * attached dialog replaced by its parent window and without duplicates.
 *
 * Returns: (transfer container) (element-type MetaWindow): the windows
 */
GList *
shell_alt_tab_get_windows (MetaWorkspace *workspace)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  g_autoptr (GList) tab_list = NULL;
  GList *windows = NULL;
  GList *l;

  /* We ignore skip-taskbar windows in switchers, but if they are attached
   * to their parent, their position in the MRU list may be more appropriate
   * than the parent; so start with the complete list ... */
  tab_list = meta_display_get_tab_list (display, META_TAB_LIST_NORMAL_ALL, workspace);

  for (l = tab_list; l != NULL; l = l->next)
    {
      MetaWindow *window = l->data;

      /* ... map windows to their parent where appropriate ... */
      if (meta_window_is_attached_dialog (window))
        window = meta_window_get_transient_for (window);

      /* ... and filter out skip-taskbar windows and duplicates */
      if (window == NULL ||
          meta_window_is_skip_taskbar (window) ||
          g_list_find (windows, window) != NULL)
        continue;

      windows = g_list_prepend (windows, window);
    }

  return g_list_reverse (windows);
}

static GPtrArray *
windows_array_new (GList *windows)
{
  GPtrArray *array = g_ptr_array_new_with_free_func (g_object_unref);
  GList *l;

  for (l = windows; l != NULL; l = l->next)
    g_ptr_array_add (array, g_object_ref (l->data));

  return array;
}

static MetaWorkspace *
get_active_workspace (void)
{
  MetaWorkspaceManager *workspace_manager =
    shell_global_get_workspace_manager (shell_global_get ());

  return meta_workspace_manager_get_active_workspace (workspace_manager);
}

/* Main.activateWindow (), without hiding the overview and closing the
 * calendar. Returns the time used. */
static uint32_t
activate_window (MetaWindow *window,
                 uint32_t    time)
{
  ShellGlobal *global = shell_global_get ();
  MetaWorkspace *workspace = meta_window_get_workspace (window);

  if (time == 0)
    time = shell_global_get_current_time (global);

  if (workspace != NULL && workspace != get_active_workspace ())
    meta_workspace_activate_with_focus (workspace, window, time);
  else
    meta_window_activate (window, time);

  return time;
}

static guint
window_activated_signal_new (GType type)
{
  return g_signal_new ("window-activated",
                       type,
                       G_SIGNAL_RUN_LAST,
                       0, NULL, NULL, NULL,
                       G_TYPE_NONE, 2,
                       META_TYPE_WINDOW,
                       G_TYPE_UINT);
}

static void
arrow_repaint_cb (StDrawingArea *area,
                  gpointer       user_data)
{
  shell_switcher_draw_arrow (area, ST_SIDE_BOTTOM);
}

/* ------------------------------------------------------------------------ */
/* Types                                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_ALT_TAB_APP_ICON (shell_alt_tab_app_icon_get_type ())
G_DECLARE_FINAL_TYPE (ShellAltTabAppIcon, shell_alt_tab_app_icon,
                      SHELL, ALT_TAB_APP_ICON, StBoxLayout)

#define SHELL_TYPE_ALT_TAB_APP_SWITCHER (shell_alt_tab_app_switcher_get_type ())
G_DECLARE_FINAL_TYPE (ShellAltTabAppSwitcher, shell_alt_tab_app_switcher,
                      SHELL, ALT_TAB_APP_SWITCHER, ShellSwitcherList)

#define SHELL_TYPE_ALT_TAB_THUMBNAIL_LIST (shell_alt_tab_thumbnail_list_get_type ())
G_DECLARE_FINAL_TYPE (ShellAltTabThumbnailList, shell_alt_tab_thumbnail_list,
                      SHELL, ALT_TAB_THUMBNAIL_LIST, ShellSwitcherList)

#define SHELL_TYPE_ALT_TAB_WINDOW_ICON (shell_alt_tab_window_icon_get_type ())
G_DECLARE_FINAL_TYPE (ShellAltTabWindowIcon, shell_alt_tab_window_icon,
                      SHELL, ALT_TAB_WINDOW_ICON, StBoxLayout)

#define SHELL_TYPE_ALT_TAB_WINDOW_SWITCHER (shell_alt_tab_window_switcher_get_type ())
G_DECLARE_FINAL_TYPE (ShellAltTabWindowSwitcher, shell_alt_tab_window_switcher,
                      SHELL, ALT_TAB_WINDOW_SWITCHER, ShellSwitcherList)

#define SHELL_TYPE_CYCLER_HIGHLIGHT (shell_cycler_highlight_get_type ())
G_DECLARE_FINAL_TYPE (ShellCyclerHighlight, shell_cycler_highlight,
                      SHELL, CYCLER_HIGHLIGHT, StWidget)

#define SHELL_TYPE_CYCLER_LIST (shell_cycler_list_get_type ())
G_DECLARE_FINAL_TYPE (ShellCyclerList, shell_cycler_list,
                      SHELL, CYCLER_LIST, StWidget)

/* AppIcon */
struct _ShellAltTabAppIcon
{
  StBoxLayout parent;

  ShellApp *app;
  ClutterActor *icon;          /* weak, NULL until set_icon_size () */
  StBin *icon_bin;             /* weak */
  StLabel *label;              /* weak */
  GPtrArray *cached_windows;   /* MetaWindow. The windows of the application
                                * when the list was created. Shared with the
                                * thumbnail list, which removes the windows
                                * that are closed */
};

/* AppSwitcher */
struct _ShellAltTabAppSwitcher
{
  ShellSwitcherList parent;

  GPtrArray *icons;            /* ShellAltTabAppIcon */
  GPtrArray *apps;             /* ShellApp, one for each icon. The icons
                                * forget theirs when they are destroyed */
  GPtrArray *arrows;           /* StDrawingArea, one for each icon */
  ShellAppSwitcherPopup *popup; /* weak */
  int icon_size;
  int delayed_highlighted;
  guint mouse_timeout_id;
};

/* ThumbnailSwitcher: the windows of an application */
struct _ShellAltTabThumbnailList
{
  ShellSwitcherList parent;

  GPtrArray *labels;           /* StLabel, the title of each window */
  GPtrArray *bins;             /* StBin, until the clones are added */
  GPtrArray *clones;           /* ThumbnailClone */
  GPtrArray *windows;          /* MetaWindow, see ShellAltTabAppIcon */
};

/* AppSwitcherPopup */
struct _ShellAppSwitcherPopup
{
  ShellSwitcherPopup parent;

  ShellAltTabThumbnailList *thumbnails; /* not owned, NULL when destroyed */
  guint thumbnail_timeout_id;
  int current_window;
  gboolean thumbnails_visible;
  gboolean thumbnails_focused;
};

/* WindowIcon */
struct _ShellAltTabWindowIcon
{
  StBoxLayout parent;

  MetaWindow *window;
  ShellApp *app;               /* nullable */
  StLabel *label;              /* strong: it is not a child of the icon */
};

/* WindowSwitcher */
struct _ShellAltTabWindowSwitcher
{
  ShellSwitcherList parent;

  StLabel *label;              /* weak */
  GPtrArray *icons;            /* ShellAltTabWindowIcon */
  GPtrArray *windows;          /* MetaWindow, one for each icon. The icons
                                * forget theirs when they are destroyed */
};

/* WindowSwitcherPopup */
struct _ShellWindowSwitcherPopup
{
  ShellSwitcherPopup parent;
};

static ShellAltTabThumbnailList *thumbnail_list_new (GPtrArray *windows);
static void thumbnail_list_add_clones (ShellAltTabThumbnailList *self,
                                       double                    avail_height);

/* ------------------------------------------------------------------------ */
/* ShellAltTabAppIcon                                                        */
/* ------------------------------------------------------------------------ */

G_DEFINE_FINAL_TYPE (ShellAltTabAppIcon, shell_alt_tab_app_icon, ST_TYPE_BOX_LAYOUT)

static void
shell_alt_tab_app_icon_dispose (GObject *object)
{
  ShellAltTabAppIcon *self = SHELL_ALT_TAB_APP_ICON (object);

  g_clear_weak_pointer (&self->icon);
  g_clear_weak_pointer (&self->icon_bin);
  g_clear_weak_pointer (&self->label);
  g_clear_object (&self->app);
  g_clear_pointer (&self->cached_windows, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_alt_tab_app_icon_parent_class)->dispose (object);
}

static void
shell_alt_tab_app_icon_class_init (ShellAltTabAppIconClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_alt_tab_app_icon_dispose;
}

static void
shell_alt_tab_app_icon_init (ShellAltTabAppIcon *self)
{
}

static ShellAltTabAppIcon *
app_icon_new (ShellApp   *app,
              GPtrArray  *cached_windows)
{
  ShellAltTabAppIcon *self;
  ClutterActor *icon_bin, *label;

  self = g_object_new (SHELL_TYPE_ALT_TAB_APP_ICON,
                       "style-class", "alt-tab-app",
                       "orientation", CLUTTER_ORIENTATION_VERTICAL,
                       NULL);
  self->app = g_object_ref (app);
  self->cached_windows = g_ptr_array_ref (cached_windows);

  icon_bin = g_object_new (ST_TYPE_BIN, NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), icon_bin);
  g_set_weak_pointer (&self->icon_bin, ST_BIN (icon_bin));

  label = g_object_new (ST_TYPE_LABEL,
                        "text", shell_app_get_name (app),
                        "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), label);
  g_set_weak_pointer (&self->label, ST_LABEL (label));

  return self;
}

/* set_size () of the JS AppIcon */
static void
app_icon_set_icon_size (ShellAltTabAppIcon *self,
                        int                 size)
{
  ClutterActor *icon;

  if (self->icon_bin == NULL)
    return;

  icon = shell_app_create_icon_texture (self->app, size);
  g_clear_weak_pointer (&self->icon);
  st_bin_set_child (self->icon_bin, icon);
  g_set_weak_pointer (&self->icon, icon);
}

/* ------------------------------------------------------------------------ */
/* ShellAltTabThumbnailList: the windows of the selected application         */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellAltTabThumbnailList *list; /* not owned: the handler is disconnected
                                   * when the list is disposed */
  ClutterActor *clone;         /* strong */
  ClutterActor *source;        /* weak, the window actor */
  gulong destroy_id;
} ThumbnailClone;

static void
thumbnail_clone_free (ThumbnailClone *thumbnail)
{
  if (thumbnail->source != NULL && thumbnail->destroy_id != 0 &&
      g_signal_handler_is_connected (thumbnail->source, thumbnail->destroy_id))
    g_signal_handler_disconnect (thumbnail->source, thumbnail->destroy_id);

  g_clear_weak_pointer (&thumbnail->source);
  g_clear_object (&thumbnail->clone);
  g_free (thumbnail);
}

G_DEFINE_FINAL_TYPE (ShellAltTabThumbnailList, shell_alt_tab_thumbnail_list,
                     SHELL_TYPE_SWITCHER_LIST)

static void
shell_alt_tab_thumbnail_list_dispose (GObject *object)
{
  ShellAltTabThumbnailList *self = SHELL_ALT_TAB_THUMBNAIL_LIST (object);

  /* Disconnects from the window actors */
  if (self->clones != NULL)
    g_ptr_array_set_size (self->clones, 0);
  if (self->labels != NULL)
    g_ptr_array_set_size (self->labels, 0);
  if (self->bins != NULL)
    g_ptr_array_set_size (self->bins, 0);
  g_clear_pointer (&self->windows, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_alt_tab_thumbnail_list_parent_class)->dispose (object);
}

static void
shell_alt_tab_thumbnail_list_finalize (GObject *object)
{
  ShellAltTabThumbnailList *self = SHELL_ALT_TAB_THUMBNAIL_LIST (object);

  g_clear_pointer (&self->clones, g_ptr_array_unref);
  g_clear_pointer (&self->labels, g_ptr_array_unref);
  g_clear_pointer (&self->bins, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_alt_tab_thumbnail_list_parent_class)->finalize (object);
}

static void
shell_alt_tab_thumbnail_list_class_init (ShellAltTabThumbnailListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_alt_tab_thumbnail_list_dispose;
  object_class->finalize = shell_alt_tab_thumbnail_list_finalize;
}

static void
shell_alt_tab_thumbnail_list_init (ShellAltTabThumbnailList *self)
{
  self->labels = g_ptr_array_new_with_free_func (g_object_unref);
  self->bins = g_ptr_array_new_with_free_func (g_object_unref);
  self->clones = g_ptr_array_new_with_free_func ((GDestroyNotify) thumbnail_clone_free);
}

static ShellAltTabThumbnailList *
thumbnail_list_new (GPtrArray *windows)
{
  ShellAltTabThumbnailList *self;
  guint i;

  self = g_object_new (SHELL_TYPE_ALT_TAB_THUMBNAIL_LIST,
                       "square-items", FALSE,
                       NULL);
  self->windows = g_ptr_array_ref (windows);

  for (i = 0; i < windows->len; i++)
    {
      MetaWindow *window = g_ptr_array_index (windows, i);
      const char *title = meta_window_get_title (window);
      ClutterActor *box, *bin, *name;

      box = g_object_new (ST_TYPE_BOX_LAYOUT,
                          "style-class", "thumbnail-box",
                          "orientation", CLUTTER_ORIENTATION_VERTICAL,
                          NULL);

      bin = g_object_new (ST_TYPE_BIN,
                          "style-class", "thumbnail",
                          NULL);
      clutter_actor_add_child (box, bin);
      g_ptr_array_add (self->bins, g_object_ref (bin));

      name = g_object_new (ST_TYPE_LABEL,
                           "text", title != NULL ? title : "",
                           /* St.Label doesn't support text-align */
                           "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                           NULL);
      g_ptr_array_add (self->labels, g_object_ref (name));
      clutter_actor_add_child (box, name);

      shell_switcher_list_add_item (SHELL_SWITCHER_LIST (self), box, name);
    }

  return self;
}

static void
thumbnail_list_remove_thumbnail (ShellAltTabThumbnailList *self,
                                 ThumbnailClone           *thumbnail)
{
  g_autoptr (ShellAltTabThumbnailList) keep_alive = g_object_ref (self);
  ShellSwitcherList *list = SHELL_SWITCHER_LIST (self);
  guint index;

  if (!g_ptr_array_find (self->clones, thumbnail, &index))
    return;

  g_ptr_array_remove_index (self->clones, index);
  if (self->windows != NULL && index < self->windows->len)
    g_ptr_array_remove_index (self->windows, index);
  if (index < self->labels->len)
    g_ptr_array_remove_index (self->labels, index);
  if ((int) index < shell_switcher_list_get_n_items (list))
    shell_switcher_list_remove_item (list, index);

  if (self->clones->len > 0)
    shell_switcher_list_highlight (list,
                                   shell_switcher_popup_mod (index, self->clones->len),
                                   FALSE);
  else
    clutter_actor_destroy (CLUTTER_ACTOR (self));
}

static void
window_actor_destroyed_cb (ClutterActor   *source,
                           ThumbnailClone *thumbnail)
{
  thumbnail_list_remove_thumbnail (thumbnail->list, thumbnail);
}

/* addClones () */
static void
thumbnail_list_add_clones (ShellAltTabThumbnailList *self,
                           double                    avail_height)
{
  ShellSwitcherList *list = SHELL_SWITCHER_LIST (self);
  StButton *first_item;
  StThemeNode *first_node, *list_node, *child_node;
  ClutterActor *first_child;
  double total_padding, spacing, bin_height;
  float label_natural_height;
  int thumbnail_size;
  guint i;

  if (self->bins->len == 0 || self->windows == NULL)
    return;

  first_item = shell_switcher_list_get_item (list, 0);
  if (first_item == NULL || self->labels->len == 0)
    return;

  first_node = st_widget_get_theme_node (ST_WIDGET (first_item));
  list_node = st_widget_get_theme_node (ST_WIDGET (self));
  total_padding = st_theme_node_get_horizontal_padding (first_node) +
                  st_theme_node_get_vertical_padding (first_node);
  total_padding += st_theme_node_get_horizontal_padding (list_node) +
                   st_theme_node_get_vertical_padding (list_node);

  clutter_actor_get_preferred_height (g_ptr_array_index (self->labels, 0), -1,
                                      NULL, &label_natural_height);

  first_child = st_bin_get_child (ST_BIN (first_item));
  child_node = ST_IS_WIDGET (first_child) ?
    st_widget_get_theme_node (ST_WIDGET (first_child)) : first_node;
  spacing = st_theme_node_get_length (child_node, "spacing");

  thumbnail_size = THUMBNAIL_DEFAULT_SIZE * get_scale_factor ();

  avail_height = MIN (avail_height - label_natural_height - total_padding - spacing,
                      thumbnail_size);
  bin_height = avail_height + st_theme_node_get_vertical_padding (first_node) +
               st_theme_node_get_vertical_padding (list_node) - spacing;
  bin_height = MIN (thumbnail_size, bin_height);

  for (i = 0; i < self->bins->len && i < self->windows->len; i++)
    {
      MetaWindow *window = g_ptr_array_index (self->windows, i);
      GObject *window_actor = meta_window_get_compositor_private (window);
      StBin *bin = g_ptr_array_index (self->bins, i);
      ThumbnailClone *thumbnail;
      ClutterActor *clone;

      if (window_actor == NULL)
        continue;

      clone = create_window_clone (CLUTTER_ACTOR (window_actor), thumbnail_size);
      clutter_actor_set_height (CLUTTER_ACTOR (bin), bin_height);
      st_bin_set_child (bin, clone);

      thumbnail = g_new0 (ThumbnailClone, 1);
      thumbnail->list = self;
      thumbnail->clone = g_object_ref (clone);
      g_set_weak_pointer (&thumbnail->source, CLUTTER_ACTOR (window_actor));
      thumbnail->destroy_id = g_signal_connect (window_actor, "destroy",
                                                G_CALLBACK (window_actor_destroyed_cb),
                                                thumbnail);
      g_ptr_array_add (self->clones, thumbnail);
    }

  /* Make sure we only do this once */
  g_ptr_array_set_size (self->bins, 0);
}

/* ------------------------------------------------------------------------ */
/* ShellAltTabAppSwitcher: the list of applications                          */
/* ------------------------------------------------------------------------ */

G_DEFINE_FINAL_TYPE (ShellAltTabAppSwitcher, shell_alt_tab_app_switcher,
                     SHELL_TYPE_SWITCHER_LIST)

static ShellAltTabAppIcon *
app_switcher_get_icon (ShellAltTabAppSwitcher *self,
                       int                     index)
{
  if (self == NULL || self->icons == NULL || index < 0 || (guint) index >= self->icons->len)
    return NULL;

  return g_ptr_array_index (self->icons, index);
}

static void
app_switcher_set_icon_size (ShellAltTabAppSwitcher *self)
{
  ShellSwitcherList *list = SHELL_SWITCHER_LIST (self);
  StBoxLayout *box = shell_switcher_list_get_list (list);
  StButton *first_item = shell_switcher_list_get_item (list, 0);
  int n_items = shell_switcher_list_get_n_items (list);
  ShellLayoutManager *layout_manager =
    shell_main_get_layout_manager (shell_main_get_default ());
  ShellMonitor *primary;
  StThemeNode *theme_node;
  ShellAltTabAppIcon *first_icon;
  ClutterActor *parent;
  double icon_padding, icon_border, icon_spacing, spacing, total_spacing;
  double parent_padding = 0, avail_width;
  float label_natural_height;
  int scale_factor, icon_size;
  guint i;

  if (first_item == NULL || self->icons->len == 0 || layout_manager == NULL)
    return;

  /* The JS code looked for the first item with the style class `item-box`,
   * which is the first one: the lists only hold such items. */
  theme_node = st_widget_get_theme_node (ST_WIDGET (first_item));
  st_widget_ensure_style (ST_WIDGET (box));

  icon_padding = st_theme_node_get_horizontal_padding (theme_node);
  icon_border = st_theme_node_get_border_width (theme_node, ST_SIDE_LEFT) +
                st_theme_node_get_border_width (theme_node, ST_SIDE_RIGHT);
  first_icon = g_ptr_array_index (self->icons, 0);
  clutter_actor_get_preferred_height (CLUTTER_ACTOR (first_icon->label), -1,
                                      NULL, &label_natural_height);
  icon_spacing = label_natural_height + icon_padding + icon_border;
  spacing = st_theme_node_get_length (st_widget_get_theme_node (ST_WIDGET (box)),
                                      "spacing");
  total_spacing = spacing * (n_items - 1);

  /* We just assume the whole screen here due to weirdness happening with
   * the passed width */
  primary = shell_layout_manager_get_primary_monitor (layout_manager);
  if (primary == NULL)
    return;

  parent = clutter_actor_get_parent (CLUTTER_ACTOR (self));
  if (ST_IS_WIDGET (parent))
    parent_padding = st_theme_node_get_horizontal_padding (st_widget_get_theme_node (ST_WIDGET (parent)));
  avail_width = primary->width - parent_padding -
                st_theme_node_get_horizontal_padding (st_widget_get_theme_node (ST_WIDGET (self)));
  shell_monitor_free (primary);

  scale_factor = get_scale_factor ();
  icon_size = base_icon_sizes[0];

  if (n_items > 1)
    {
      for (i = 0; i < G_N_ELEMENTS (base_icon_sizes); i++)
        {
          double height, width;

          icon_size = base_icon_sizes[i];
          height = base_icon_sizes[i] * scale_factor + icon_spacing;
          width = height * n_items + total_spacing;
          if (width <= avail_width)
            break;
        }
    }

  self->icon_size = icon_size;

  for (i = 0; i < self->icons->len; i++)
    {
      ShellAltTabAppIcon *icon = g_ptr_array_index (self->icons, i);

      if (icon->icon != NULL)
        break;
      app_icon_set_icon_size (icon, icon_size);
    }
}

static void
app_switcher_get_preferred_height (ClutterActor *actor,
                                   float         for_width,
                                   float        *min_height_p,
                                   float        *natural_height_p)
{
  ShellAltTabAppSwitcher *self = SHELL_ALT_TAB_APP_SWITCHER (actor);

  if (self->icon_size == 0)
    app_switcher_set_icon_size (self);

  CLUTTER_ACTOR_CLASS (shell_alt_tab_app_switcher_parent_class)->get_preferred_height (actor,
                                                                                       for_width,
                                                                                       min_height_p,
                                                                                       natural_height_p);
}

static void
app_switcher_allocate (ClutterActor          *actor,
                       const ClutterActorBox *box)
{
  ShellAltTabAppSwitcher *self = SHELL_ALT_TAB_APP_SWITCHER (actor);
  ShellSwitcherList *list = SHELL_SWITCHER_LIST (actor);
  StThemeNode *theme_node;
  ClutterActorBox content_box, child_box;
  float arrow_height, arrow_width;
  int i, n_items;

  /* Allocate the main list items */
  CLUTTER_ACTOR_CLASS (shell_alt_tab_app_switcher_parent_class)->allocate (actor, box);

  theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  st_theme_node_get_content_box (theme_node, box, &content_box);

  arrow_height = floorf (st_theme_node_get_padding (theme_node, ST_SIDE_BOTTOM) / 3);
  arrow_width = arrow_height * 2;

  /* Now allocate each arrow underneath its item */
  n_items = shell_switcher_list_get_n_items (list);
  for (i = 0; i < n_items && (guint) i < self->arrows->len; i++)
    {
      ClutterActorBox item_box;

      clutter_actor_get_allocation_box (CLUTTER_ACTOR (shell_switcher_list_get_item (list, i)),
                                        &item_box);
      child_box.x1 = content_box.x1 +
                     floorf (item_box.x1 + (item_box.x2 - item_box.x1 - arrow_width) / 2);
      child_box.x2 = child_box.x1 + arrow_width;
      child_box.y1 = content_box.y1 + item_box.y2 + arrow_height;
      child_box.y2 = child_box.y1 + arrow_height;
      clutter_actor_allocate (g_ptr_array_index (self->arrows, i), &child_box);
    }
}

static void
app_switcher_enter_item (ShellAltTabAppSwitcher *self,
                         int                     index)
{
  ShellGlobal *global = shell_global_get ();
  ShellSwitcherList *list = SHELL_SWITCHER_LIST (self);
  ClutterActor *picked_actor;
  StButton *item;
  ClutterModifierType mods;
  int x, y;

  shell_global_get_pointer (global, &x, &y, &mods);
  picked_actor = clutter_stage_get_actor_at_pos (shell_global_get_stage (global),
                                                 CLUTTER_PICK_ALL, x, y);
  item = shell_switcher_list_get_item (list, index);

  if (item != NULL && picked_actor != NULL &&
      clutter_actor_contains (CLUTTER_ACTOR (item), picked_actor))
    shell_switcher_list_item_entered (list, index);
}

static void
mouse_timeout_cb (gpointer user_data)
{
  g_autoptr (ShellAltTabAppSwitcher) self = g_object_ref (user_data);
  int index = self->delayed_highlighted;

  self->delayed_highlighted = -1;
  self->mouse_timeout_id = 0;

  app_switcher_enter_item (self, index);
}

/* We override the item_motion vfunc of the list to delay activation when
 * the thumbnail list is open */
static void
app_switcher_item_motion (ShellSwitcherList *list,
                          StButton          *item)
{
  ShellAltTabAppSwitcher *self = SHELL_ALT_TAB_APP_SWITCHER (list);
  int index;

  if (item == shell_switcher_list_get_item (list, shell_switcher_list_get_highlighted (list)) ||
      item == shell_switcher_list_get_item (list, self->delayed_highlighted))
    return;

  index = shell_switcher_list_get_item_index (list, item);

  if (self->mouse_timeout_id != 0)
    {
      g_clear_handle_id (&self->mouse_timeout_id, g_source_remove);
      self->delayed_highlighted = -1;
    }

  if (self->popup != NULL && self->popup->thumbnails_visible)
    {
      self->delayed_highlighted = index;
      self->mouse_timeout_id = g_timeout_add_once (APP_ICON_HOVER_TIMEOUT,
                                                   mouse_timeout_cb, self);
      g_source_set_name_by_id (self->mouse_timeout_id,
                               "[gnome-shell] app_switcher_enter_item");
    }
  else
    {
      shell_switcher_list_item_entered (list, index);
    }
}

/* We override the highlight vfunc of the list to also deal with the
 * AppSwitcher->ThumbnailSwitcher arrows. Apps with only 1 window will hide
 * their arrows by default, but show them when their thumbnails are visible
 * (ie, when the app icon is supposed to be in just_outline mode). Apps with
 * multiple windows will normally show a dim arrow, but show a bright arrow
 * when they are highlighted. */
static void
app_switcher_highlight (ShellSwitcherList *list,
                        int                index,
                        gboolean           just_outline)
{
  ShellAltTabAppSwitcher *self = SHELL_ALT_TAB_APP_SWITCHER (list);
  int highlighted = shell_switcher_list_get_highlighted (list);
  ShellAltTabAppIcon *icon = app_switcher_get_icon (self, highlighted);

  if (icon != NULL && (guint) highlighted < self->arrows->len)
    {
      ClutterActor *arrow = g_ptr_array_index (self->arrows, highlighted);

      if (icon->cached_windows->len == 1)
        clutter_actor_hide (arrow);
      else
        st_widget_remove_style_pseudo_class (ST_WIDGET (arrow), "highlighted");
    }

  SHELL_SWITCHER_LIST_CLASS (shell_alt_tab_app_switcher_parent_class)->highlight (list,
                                                                                  index,
                                                                                  just_outline);

  highlighted = shell_switcher_list_get_highlighted (list);
  icon = app_switcher_get_icon (self, highlighted);
  if (icon != NULL && (guint) highlighted < self->arrows->len)
    {
      ClutterActor *arrow = g_ptr_array_index (self->arrows, highlighted);

      if (just_outline && icon->cached_windows->len == 1)
        clutter_actor_show (arrow);
      else
        st_widget_add_style_pseudo_class (ST_WIDGET (arrow), "highlighted");
    }
}

static void
app_switcher_remove_icon (ShellAltTabAppSwitcher *self,
                          ShellApp               *app)
{
  guint i;

  if (!g_ptr_array_find (self->apps, app, &i))
    return;

  g_signal_handlers_disconnect_by_data (app, self);

  clutter_actor_destroy (g_ptr_array_index (self->arrows, i));
  g_ptr_array_remove_index (self->arrows, i);

  g_ptr_array_remove_index (self->icons, i);
  g_ptr_array_remove_index (self->apps, i);
  shell_switcher_list_remove_item (SHELL_SWITCHER_LIST (self), i);
}

static void
app_state_changed_cb (ShellApp               *app,
                      GParamSpec             *pspec,
                      ShellAltTabAppSwitcher *self)
{
  if (shell_app_get_state (app) != SHELL_APP_STATE_RUNNING)
    app_switcher_remove_icon (self, app);
}

static void
app_switcher_add_icon (ShellAltTabAppSwitcher *self,
                       ShellAltTabAppIcon     *app_icon)
{
  StButton *item;
  ClutterActor *arrow;

  g_ptr_array_add (self->icons, g_object_ref (app_icon));
  g_ptr_array_add (self->apps, g_object_ref (app_icon->app));
  item = shell_switcher_list_add_item (SHELL_SWITCHER_LIST (self),
                                       CLUTTER_ACTOR (app_icon),
                                       CLUTTER_ACTOR (app_icon->label));

  g_signal_connect_object (app_icon->app, "notify::state",
                           G_CALLBACK (app_state_changed_cb), self, 0);

  arrow = g_object_new (ST_TYPE_DRAWING_AREA,
                        "style-class", "switcher-arrow",
                        NULL);
  g_signal_connect (arrow, "repaint", G_CALLBACK (arrow_repaint_cb), NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), arrow);
  g_ptr_array_add (self->arrows, g_object_ref (arrow));

  if (app_icon->cached_windows->len == 1)
    clutter_actor_hide (arrow);
  else
    clutter_actor_add_accessible_state (CLUTTER_ACTOR (item), ATK_STATE_EXPANDABLE);
}

static void
shutdown_clear_mouse_timeout_cb (ShellGlobal            *global,
                                 ShellAltTabAppSwitcher *self)
{
  g_clear_handle_id (&self->mouse_timeout_id, g_source_remove);
}

static void
shell_alt_tab_app_switcher_dispose (GObject *object)
{
  ShellAltTabAppSwitcher *self = SHELL_ALT_TAB_APP_SWITCHER (object);
  guint i;

  g_clear_handle_id (&self->mouse_timeout_id, g_source_remove);

  if (self->apps != NULL)
    {
      for (i = 0; i < self->apps->len; i++)
        g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->apps, i), self);
      g_ptr_array_set_size (self->apps, 0);
    }
  if (self->icons != NULL)
    g_ptr_array_set_size (self->icons, 0);
  if (self->arrows != NULL)
    g_ptr_array_set_size (self->arrows, 0);

  g_clear_weak_pointer (&self->popup);

  G_OBJECT_CLASS (shell_alt_tab_app_switcher_parent_class)->dispose (object);
}

static void
shell_alt_tab_app_switcher_finalize (GObject *object)
{
  ShellAltTabAppSwitcher *self = SHELL_ALT_TAB_APP_SWITCHER (object);

  g_clear_pointer (&self->icons, g_ptr_array_unref);
  g_clear_pointer (&self->apps, g_ptr_array_unref);
  g_clear_pointer (&self->arrows, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_alt_tab_app_switcher_parent_class)->finalize (object);
}

static void
shell_alt_tab_app_switcher_class_init (ShellAltTabAppSwitcherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  ShellSwitcherListClass *list_class = SHELL_SWITCHER_LIST_CLASS (klass);

  object_class->dispose = shell_alt_tab_app_switcher_dispose;
  object_class->finalize = shell_alt_tab_app_switcher_finalize;

  actor_class->get_preferred_height = app_switcher_get_preferred_height;
  actor_class->allocate = app_switcher_allocate;

  list_class->highlight = app_switcher_highlight;
  list_class->item_motion = app_switcher_item_motion;
}

static void
shell_alt_tab_app_switcher_init (ShellAltTabAppSwitcher *self)
{
  self->icons = g_ptr_array_new_with_free_func (g_object_unref);
  self->apps = g_ptr_array_new_with_free_func (g_object_unref);
  self->arrows = g_ptr_array_new_with_free_func (g_object_unref);
  self->delayed_highlighted = -1;
}

static ShellAltTabAppSwitcher *
app_switcher_new (GSList                *apps,
                  ShellAppSwitcherPopup *popup)
{
  ShellWindowTracker *tracker = shell_window_tracker_get_default ();
  g_autoptr (GSettings) settings = g_settings_new ("org.gnome.shell.app-switcher");
  g_autoptr (GList) all_windows = NULL;
  MetaWorkspace *workspace = NULL;
  ShellAltTabAppSwitcher *self;
  GSList *a;

  self = g_object_new (SHELL_TYPE_ALT_TAB_APP_SWITCHER,
                       "square-items", TRUE,
                       NULL);
  g_set_weak_pointer (&self->popup, popup);
  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (shutdown_clear_mouse_timeout_cb), self, 0);

  if (g_settings_get_boolean (settings, "current-workspace-only"))
    workspace = get_active_workspace ();

  all_windows = shell_alt_tab_get_windows (workspace);

  /* Construct the AppIcons, add to the popup */
  for (a = apps; a != NULL; a = a->next)
    {
      ShellApp *app = a->data;
      g_autoptr (GPtrArray) cached_windows = g_ptr_array_new_with_free_func (g_object_unref);
      GList *w;

      /* Cache the window list now; we don't handle dynamic changes here,
       * and we don't want to be continually retrieving it */
      for (w = all_windows; w != NULL; w = w->next)
        {
          g_autoptr (ShellApp) window_app = shell_window_tracker_get_window_app (tracker, w->data);

          if (window_app == app)
            g_ptr_array_add (cached_windows, g_object_ref (w->data));
        }

      if (cached_windows->len > 0)
        app_switcher_add_icon (self, app_icon_new (app, cached_windows));
    }

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellAppSwitcherPopup                                                     */
/* ------------------------------------------------------------------------ */

enum
{
  APP_POPUP_WINDOW_ACTIVATED,

  APP_POPUP_LAST_SIGNAL
};

static guint app_popup_signals[APP_POPUP_LAST_SIGNAL] = { 0, };

G_DEFINE_FINAL_TYPE (ShellAppSwitcherPopup, shell_app_switcher_popup,
                     SHELL_TYPE_SWITCHER_POPUP)

static ShellAltTabAppSwitcher *
app_popup_get_list (ShellAppSwitcherPopup *self)
{
  StWidget *list = shell_switcher_popup_get_switcher_list (SHELL_SWITCHER_POPUP (self));

  return SHELL_IS_ALT_TAB_APP_SWITCHER (list) ? SHELL_ALT_TAB_APP_SWITCHER (list) : NULL;
}

/* this._items[index] */
static ShellAltTabAppIcon *
app_popup_get_icon (ShellAppSwitcherPopup *self,
                    int                    index)
{
  return app_switcher_get_icon (app_popup_get_list (self), index);
}

static int
app_popup_get_selected (ShellAppSwitcherPopup *self)
{
  return shell_switcher_popup_get_selected_index (SHELL_SWITCHER_POPUP (self));
}

/* Main.activateWindow () */
static void
app_popup_activate_window (ShellAppSwitcherPopup *self,
                           MetaWindow            *window,
                           uint32_t               time)
{
  time = activate_window (window, time);
  g_signal_emit (self, app_popup_signals[APP_POPUP_WINDOW_ACTIVATED], 0, window, time);
}

static void select_window (ShellAppSwitcherPopup *self,
                           int                    app,
                           int                    window,
                           gboolean               force_app_focus);

typedef struct
{
  GWeakRef popup;
  ClutterActor *actor;         /* destroyed when the fade ends, nullable */
  gboolean visible;            /* the value of thumbnails-visible afterwards */
} ThumbnailFade;

static void
thumbnail_fade_free (gpointer user_data)
{
  ThumbnailFade *fade = user_data;

  g_weak_ref_clear (&fade->popup);
  g_clear_object (&fade->actor);
  g_free (fade);
}

static void
thumbnail_fade_done_cb (gboolean  finished,
                        gpointer  user_data)
{
  ThumbnailFade *fade = user_data;
  g_autoptr (ShellAppSwitcherPopup) self = NULL;

  /* onComplete: only when the animation ran to its end */
  if (!finished)
    return;

  if (fade->actor != NULL)
    clutter_actor_destroy (fade->actor);

  self = g_weak_ref_get (&fade->popup);
  if (self != NULL)
    self->thumbnails_visible = fade->visible;
}

static void
thumbnail_fade (ShellAppSwitcherPopup *self,
                ClutterActor          *thumbnails,
                guint8                 opacity,
                gboolean               destroy_when_done)
{
  ThumbnailFade *fade = g_new0 (ThumbnailFade, 1);
  ShellEaseParams params = {
    .duration = THUMBNAIL_FADE_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  g_weak_ref_init (&fade->popup, self);
  if (destroy_when_done)
    fade->actor = g_object_ref (thumbnails);
  fade->visible = !destroy_when_done;

  shell_actor_ease (thumbnails, &params,
                    thumbnail_fade_done_cb, fade, thumbnail_fade_free,
                    "opacity", (guint) opacity,
                    NULL);
}

static void
destroy_thumbnails (ShellAppSwitcherPopup *self)
{
  ShellAltTabAppSwitcher *list = app_popup_get_list (self);
  ClutterActor *thumbnails = CLUTTER_ACTOR (self->thumbnails);
  int selected = app_popup_get_selected (self);

  self->thumbnails = NULL;
  thumbnail_fade (self, thumbnails, 0, TRUE);

  if (list != NULL && shell_switcher_list_get_item (SHELL_SWITCHER_LIST (list), selected) != NULL)
    shell_switcher_list_remove_accessible_state (SHELL_SWITCHER_LIST (list), selected,
                                                 ATK_STATE_EXPANDED);
}

static void
window_activated_cb (ShellSwitcherList      *thumbnail_list,
                     int                     n,
                     ShellAppSwitcherPopup  *self)
{
  ShellAltTabAppIcon *icon = app_popup_get_icon (self, app_popup_get_selected (self));

  if (icon != NULL && n >= 0 && (guint) n < icon->cached_windows->len)
    app_popup_activate_window (self, g_ptr_array_index (icon->cached_windows, n), 0);

  shell_switcher_popup_fade_and_destroy (SHELL_SWITCHER_POPUP (self));
}

static void
window_entered_cb (ShellSwitcherList      *thumbnail_list,
                   int                     n,
                   ShellAppSwitcherPopup  *self)
{
  if (!shell_switcher_popup_get_mouse_active (SHELL_SWITCHER_POPUP (self)))
    return;

  select_window (self, app_popup_get_selected (self), n, FALSE);
}

static void
window_removed_cb (ShellSwitcherList      *thumbnail_list,
                   int                     n,
                   ShellAppSwitcherPopup  *self)
{
  ShellAltTabAppIcon *icon = app_popup_get_icon (self, app_popup_get_selected (self));

  if (icon == NULL)
    return;

  if (icon->cached_windows->len > 0)
    {
      int new_index = MIN (n, (int) icon->cached_windows->len - 1);

      select_window (self, app_popup_get_selected (self), new_index, FALSE);
    }
}

static void
thumbnails_destroyed_cb (ClutterActor          *thumbnails,
                         ShellAppSwitcherPopup *self)
{
  /* Not the thumbnails that were replaced and are fading out */
  if (self->thumbnails != (ShellAltTabThumbnailList *) thumbnails)
    return;

  self->thumbnails = NULL;
  self->thumbnails_focused = FALSE;
}

static void
create_thumbnails (ShellAppSwitcherPopup *self)
{
  ShellAltTabAppSwitcher *list = app_popup_get_list (self);
  int selected = app_popup_get_selected (self);
  ShellAltTabAppIcon *icon = app_popup_get_icon (self, selected);
  ShellAltTabThumbnailList *thumbnails;
  ClutterActor *actor;
  ClutterActorBox box;

  if (icon == NULL)
    return;

  thumbnails = thumbnail_list_new (icon->cached_windows);
  actor = CLUTTER_ACTOR (thumbnails);
  self->thumbnails = thumbnails;

  g_signal_connect_object (thumbnails, "item-activated",
                           G_CALLBACK (window_activated_cb), self, 0);
  g_signal_connect_object (thumbnails, "item-entered",
                           G_CALLBACK (window_entered_cb), self, 0);
  g_signal_connect_object (thumbnails, "item-removed",
                           G_CALLBACK (window_removed_cb), self, 0);
  g_signal_connect_object (thumbnails, "destroy",
                           G_CALLBACK (thumbnails_destroyed_cb), self, 0);

  clutter_actor_add_child (CLUTTER_ACTOR (self), actor);

  /* Need to force an allocation so we can figure out whether we
   * need to scroll when selecting */
  clutter_actor_get_allocation_box (actor, &box);

  clutter_actor_set_opacity (actor, 0);
  thumbnail_fade (self, actor, 255, FALSE);

  if (list != NULL &&
      shell_switcher_list_get_item (SHELL_SWITCHER_LIST (list), selected) != NULL)
    shell_switcher_list_add_accessible_state (SHELL_SWITCHER_LIST (list), selected,
                                              ATK_STATE_EXPANDED);
}

static void
timeout_popup_thumbnails (gpointer user_data)
{
  ShellAppSwitcherPopup *self = user_data;

  self->thumbnail_timeout_id = 0;

  if (self->thumbnails == NULL)
    create_thumbnails (self);
  self->thumbnails_focused = FALSE;
}

/*
 * select_window:
 * @app: index of the app to select
 * @window: index of which of the windows of @app to select, or %WINDOW_NONE
 * @force_app_focus: see below
 *
 * Selects the indicated @app, and optional @window, and sets
 * thumbnails_focused appropriately to indicate whether the arrow keys should
 * act on the app list or the thumbnail list (`_selectWindow ()`).
 *
 * If @app is specified and @window is %WINDOW_NONE, then the app is
 * highlighted (ie, given a light background), and the current thumbnail list,
 * if any, is destroyed. If @app has multiple windows, and @force_app_focus is
 * not %TRUE, then a timeout is started to open a thumbnail list.
 *
 * If @app and @window are specified (and @force_app_focus is not), then @app
 * will be outlined, a thumbnail list will be created and focused (if it
 * hasn't been already), and the @window'th window in it will be highlighted.
 *
 * If @app and @window are specified and @force_app_focus is %TRUE, then @app
 * will be highlighted, and @window outlined, and the app list will have the
 * keyboard focus.
 */
static void
select_window (ShellAppSwitcherPopup *self,
               int                    app,
               int                    window,
               gboolean               force_app_focus)
{
  ShellSwitcherPopup *popup = SHELL_SWITCHER_POPUP (self);
  ShellAltTabAppSwitcher *list = app_popup_get_list (self);
  ShellAltTabAppIcon *icon;

  if (app != app_popup_get_selected (self) || window == WINDOW_NONE)
    {
      if (self->thumbnails != NULL)
        destroy_thumbnails (self);
    }

  g_clear_handle_id (&self->thumbnail_timeout_id, g_source_remove);

  self->thumbnails_focused = window != WINDOW_NONE && !force_app_focus;

  shell_switcher_popup_set_selected_index (popup, app);
  self->current_window = window > 0 ? window : -1;
  if (list != NULL)
    shell_switcher_list_highlight (SHELL_SWITCHER_LIST (list), app, self->thumbnails_focused);

  if (window != WINDOW_NONE)
    {
      if (self->thumbnails == NULL)
        create_thumbnails (self);
      self->current_window = window;
      if (self->thumbnails != NULL)
        shell_switcher_list_highlight (SHELL_SWITCHER_LIST (self->thumbnails), window,
                                       force_app_focus);
    }
  else if ((icon = app_popup_get_icon (self, app)) != NULL &&
           icon->cached_windows->len > 1 &&
           !force_app_focus)
    {
      self->thumbnail_timeout_id = g_timeout_add_once (THUMBNAIL_POPUP_TIME,
                                                       timeout_popup_thumbnails, self);
      g_source_set_name_by_id (self->thumbnail_timeout_id,
                               "[gnome-shell] timeout_popup_thumbnails");
    }
}

static int
app_popup_get_n_items (ShellSwitcherPopup *popup)
{
  ShellAltTabAppSwitcher *list = app_popup_get_list (SHELL_APP_SWITCHER_POPUP (popup));

  return list != NULL ? list->icons->len : 0;
}

static void
app_popup_allocate (ClutterActor          *actor,
                    const ClutterActorBox *box)
{
  ShellAppSwitcherPopup *self = SHELL_APP_SWITCHER_POPUP (actor);
  ShellSwitcherPopup *popup = SHELL_SWITCHER_POPUP (actor);
  ShellLayoutManager *layout_manager =
    shell_main_get_layout_manager (shell_main_get_default ());
  StWidget *switcher_list = shell_switcher_popup_get_switcher_list (popup);
  ClutterActor *thumbnails = CLUTTER_ACTOR (self->thumbnails);
  ShellAltTabAppIcon *icon;
  ShellMonitor *primary;
  StThemeNode *theme_node;
  ClutterActorBox child_box, list_box;
  float left_padding, right_padding, bottom_padding, h_padding, spacing;
  float pos_x, thumbnail_center;
  float child_natural_width, child_natural_height;

  CLUTTER_ACTOR_CLASS (shell_app_switcher_popup_parent_class)->allocate (actor, box);

  /* Allocate the thumbnails
   * We try to avoid overflowing the screen so we base the resulting size on
   * those calculations */
  if (self->thumbnails == NULL || switcher_list == NULL || layout_manager == NULL)
    return;

  icon = app_popup_get_icon (self, app_popup_get_selected (self));
  if (icon == NULL)
    return;

  primary = shell_layout_manager_get_primary_monitor (layout_manager);
  if (primary == NULL)
    return;

  theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  left_padding = st_theme_node_get_padding (theme_node, ST_SIDE_LEFT);
  right_padding = st_theme_node_get_padding (theme_node, ST_SIDE_RIGHT);
  bottom_padding = st_theme_node_get_padding (theme_node, ST_SIDE_BOTTOM);
  h_padding = left_padding + right_padding;

  clutter_actor_get_transformed_position (CLUTTER_ACTOR (icon), &pos_x, NULL);
  thumbnail_center = pos_x + clutter_actor_get_width (CLUTTER_ACTOR (icon)) / 2;
  clutter_actor_get_preferred_width (thumbnails, -1, NULL, &child_natural_width);
  child_box.x1 = MAX (primary->x + left_padding,
                      floorf (thumbnail_center - child_natural_width / 2));
  if (child_box.x1 + child_natural_width > primary->x + primary->width - h_padding)
    {
      float offset = child_box.x1 + child_natural_width - primary->width + h_padding;

      child_box.x1 = MAX (primary->x + left_padding, child_box.x1 - offset - h_padding);
    }

  spacing = st_theme_node_get_length (theme_node, "spacing");

  child_box.x2 = child_box.x1 + child_natural_width;
  if (child_box.x2 > primary->x + primary->width - right_padding)
    child_box.x2 = primary->x + primary->width - right_padding;
  clutter_actor_get_allocation_box (CLUTTER_ACTOR (switcher_list), &list_box);
  child_box.y1 = list_box.y2 + spacing;
  thumbnail_list_add_clones (self->thumbnails,
                             primary->y + primary->height - bottom_padding - child_box.y1);
  clutter_actor_get_preferred_height (thumbnails, -1, NULL, &child_natural_height);
  child_box.y2 = child_box.y1 + child_natural_height;
  clutter_actor_allocate (thumbnails, &child_box);

  shell_monitor_free (primary);
}

static void
app_popup_initial_selection (ShellSwitcherPopup *popup,
                             gboolean            backward,
                             const char         *binding)
{
  ShellAppSwitcherPopup *self = SHELL_APP_SWITCHER_POPUP (popup);
  ShellAltTabAppIcon *first = app_popup_get_icon (self, 0);
  int n_items = shell_switcher_popup_get_n_items (popup);
  int n_windows = first != NULL ? (int) first->cached_windows->len : 0;

  if (g_strcmp0 (binding, "switch-group") == 0)
    {
      if (backward)
        select_window (self, 0, n_windows - 1, FALSE);
      else if (n_windows > 1)
        select_window (self, 0, 1, FALSE);
      else
        select_window (self, 0, 0, FALSE);
    }
  else if (g_strcmp0 (binding, "switch-group-backward") == 0)
    {
      select_window (self, 0, n_windows - 1, FALSE);
    }
  else if (g_strcmp0 (binding, "switch-applications-backward") == 0)
    {
      select_window (self, n_items - 1, WINDOW_NONE, FALSE);
    }
  else if (n_items == 1)
    {
      select_window (self, 0, WINDOW_NONE, FALSE);
    }
  else if (backward)
    {
      select_window (self, n_items - 1, WINDOW_NONE, FALSE);
    }
  else
    {
      select_window (self, 1, WINDOW_NONE, FALSE);
    }
}

static void
app_popup_select (ShellSwitcherPopup *popup,
                  int                 index)
{
  select_window (SHELL_APP_SWITCHER_POPUP (popup), index, WINDOW_NONE, FALSE);
}

static int
app_popup_next_window (ShellAppSwitcherPopup *self)
{
  ShellAltTabAppIcon *icon = app_popup_get_icon (self, app_popup_get_selected (self));

  /* We actually want the second window if we're in the unset state */
  if (self->current_window == -1)
    self->current_window = 0;

  return shell_switcher_popup_mod (self->current_window + 1,
                                   icon != NULL ? icon->cached_windows->len : 0);
}

static int
app_popup_previous_window (ShellAppSwitcherPopup *self)
{
  ShellAltTabAppIcon *icon = app_popup_get_icon (self, app_popup_get_selected (self));

  /* Also assume second window here */
  if (self->current_window == -1)
    self->current_window = 1;

  return shell_switcher_popup_mod (self->current_window - 1,
                                   icon != NULL ? icon->cached_windows->len : 0);
}

static void
app_popup_close_app_window (ShellAppSwitcherPopup *self,
                            int                    app_index,
                            int                    window_index)
{
  ShellAltTabAppIcon *icon = app_popup_get_icon (self, app_index);

  if (icon == NULL || window_index < 0 || (guint) window_index >= icon->cached_windows->len)
    return;

  meta_window_delete (g_ptr_array_index (icon->cached_windows, window_index),
                      shell_global_get_current_time (shell_global_get ()));
}

static void
app_popup_quit_application (ShellAppSwitcherPopup *self,
                            int                    app_index)
{
  ShellAltTabAppIcon *icon = app_popup_get_icon (self, app_index);

  if (icon == NULL)
    return;

  shell_app_request_quit (icon->app);
}

static gboolean
app_popup_key_press_handler (ShellSwitcherPopup *popup,
                             uint32_t            keysym,
                             uint32_t            action)
{
  ShellAppSwitcherPopup *self = SHELL_APP_SWITCHER_POPUP (popup);
  gboolean rtl = clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL;
  int selected = shell_switcher_popup_get_selected_index (popup);

  if (action == META_KEYBINDING_ACTION_SWITCH_GROUP)
    {
      if (!self->thumbnails_focused)
        select_window (self, selected, 0, FALSE);
      else
        select_window (self, selected, app_popup_next_window (self), FALSE);
    }
  else if (action == META_KEYBINDING_ACTION_SWITCH_GROUP_BACKWARD)
    {
      select_window (self, selected, app_popup_previous_window (self), FALSE);
    }
  else if (action == META_KEYBINDING_ACTION_SWITCH_APPLICATIONS)
    {
      select_window (self, shell_switcher_popup_get_next_index (popup), WINDOW_NONE, FALSE);
    }
  else if (action == META_KEYBINDING_ACTION_SWITCH_APPLICATIONS_BACKWARD)
    {
      select_window (self, shell_switcher_popup_get_previous_index (popup), WINDOW_NONE, FALSE);
    }
  else if (keysym == CLUTTER_KEY_q || keysym == CLUTTER_KEY_Q)
    {
      app_popup_quit_application (self, selected);
    }
  else if (self->thumbnails_focused)
    {
      if (keysym == CLUTTER_KEY_Left)
        select_window (self, selected,
                       rtl ? app_popup_next_window (self) : app_popup_previous_window (self),
                       FALSE);
      else if (keysym == CLUTTER_KEY_Right)
        select_window (self, selected,
                       rtl ? app_popup_previous_window (self) : app_popup_next_window (self),
                       FALSE);
      else if (keysym == CLUTTER_KEY_Up)
        select_window (self, selected, WINDOW_NONE, TRUE);
      else if (keysym == CLUTTER_KEY_w || keysym == CLUTTER_KEY_W || keysym == CLUTTER_KEY_F4)
        app_popup_close_app_window (self, selected, self->current_window);
      else
        return CLUTTER_EVENT_PROPAGATE;
    }
  else if (keysym == CLUTTER_KEY_Left)
    {
      select_window (self,
                     rtl ? shell_switcher_popup_get_next_index (popup) :
                           shell_switcher_popup_get_previous_index (popup),
                     WINDOW_NONE, FALSE);
    }
  else if (keysym == CLUTTER_KEY_Right)
    {
      select_window (self,
                     rtl ? shell_switcher_popup_get_previous_index (popup) :
                           shell_switcher_popup_get_next_index (popup),
                     WINDOW_NONE, FALSE);
    }
  else if (keysym == CLUTTER_KEY_Down)
    {
      select_window (self, selected, 0, FALSE);
    }
  else
    {
      return CLUTTER_EVENT_PROPAGATE;
    }

  return CLUTTER_EVENT_STOP;
}

static void
app_popup_scroll_handler (ShellSwitcherPopup    *popup,
                          ClutterScrollDirection direction)
{
  ShellAppSwitcherPopup *self = SHELL_APP_SWITCHER_POPUP (popup);
  int selected = shell_switcher_popup_get_selected_index (popup);
  ShellAltTabAppIcon *icon = app_popup_get_icon (self, selected);
  int n_windows = icon != NULL ? (int) icon->cached_windows->len : 0;

  if (direction == CLUTTER_SCROLL_UP)
    {
      if (self->thumbnails_focused)
        {
          if (self->current_window == 0 || self->current_window == -1)
            select_window (self, shell_switcher_popup_get_previous_index (popup),
                           WINDOW_NONE, FALSE);
          else
            select_window (self, selected, app_popup_previous_window (self), FALSE);
        }
      else
        {
          if (n_windows > 1)
            select_window (self, selected, n_windows - 1, FALSE);
          else
            select_window (self, shell_switcher_popup_get_previous_index (popup),
                           WINDOW_NONE, FALSE);
        }
    }
  else if (direction == CLUTTER_SCROLL_DOWN)
    {
      if (self->thumbnails_focused)
        {
          if (self->current_window == n_windows - 1)
            select_window (self, shell_switcher_popup_get_next_index (popup),
                           WINDOW_NONE, FALSE);
          else
            select_window (self, selected, app_popup_next_window (self), FALSE);
        }
      else
        {
          if (n_windows > 1)
            select_window (self, selected, 0, FALSE);
          else
            select_window (self, shell_switcher_popup_get_next_index (popup),
                           WINDOW_NONE, FALSE);
        }
    }
}

static void
app_popup_item_activated_handler (ShellSwitcherPopup *popup,
                                  int                 index)
{
  ShellAppSwitcherPopup *self = SHELL_APP_SWITCHER_POPUP (popup);

  /* If the user clicks on the selected app, activate the
   * selected window; otherwise (eg, they click on an app while
   * !mouse_active) activate the clicked-on app. */
  if (index == shell_switcher_popup_get_selected_index (popup) && self->current_window >= 0)
    select_window (self, index, self->current_window, FALSE);
  else
    select_window (self, index, WINDOW_NONE, FALSE);
}

static void
app_popup_finish (ShellSwitcherPopup *popup,
                  uint32_t            timestamp)
{
  ShellAppSwitcherPopup *self = SHELL_APP_SWITCHER_POPUP (popup);
  ShellAltTabAppIcon *icon = app_popup_get_icon (self, shell_switcher_popup_get_selected_index (popup));

  if (icon != NULL)
    {
      if (self->current_window < 0)
        {
          if (icon->cached_windows->len > 0)
            shell_app_activate_window (icon->app,
                                       g_ptr_array_index (icon->cached_windows, 0),
                                       timestamp);
        }
      else if ((guint) self->current_window < icon->cached_windows->len)
        {
          app_popup_activate_window (self,
                                     g_ptr_array_index (icon->cached_windows,
                                                        self->current_window),
                                     timestamp);
        }
    }

  SHELL_SWITCHER_POPUP_CLASS (shell_app_switcher_popup_parent_class)->finish (popup, timestamp);
}

static gboolean
app_popup_is_actor_outside (ShellSwitcherPopup *popup,
                            ClutterActor       *actor)
{
  ShellAppSwitcherPopup *self = SHELL_APP_SWITCHER_POPUP (popup);

  return SHELL_SWITCHER_POPUP_CLASS (shell_app_switcher_popup_parent_class)->is_actor_outside (popup, actor) &&
         !(self->thumbnails != NULL && actor != NULL &&
           clutter_actor_contains (CLUTTER_ACTOR (self->thumbnails), actor));
}

static void
app_popup_shutdown_cb (ShellGlobal           *global,
                       ShellAppSwitcherPopup *self)
{
  g_clear_handle_id (&self->thumbnail_timeout_id, g_source_remove);
}

static void
shell_app_switcher_popup_constructed (GObject *object)
{
  ShellAppSwitcherPopup *self = SHELL_APP_SWITCHER_POPUP (object);
  g_autoptr (GSList) apps = NULL;
  ShellAltTabAppSwitcher *list;

  G_OBJECT_CLASS (shell_app_switcher_popup_parent_class)->constructed (object);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (app_popup_shutdown_cb), self, 0);

  apps = shell_app_system_get_running (shell_app_system_get_default ());
  list = app_switcher_new (apps, self);
  shell_switcher_popup_set_switcher_list (SHELL_SWITCHER_POPUP (self), ST_WIDGET (list));
}

static void
shell_app_switcher_popup_dispose (GObject *object)
{
  ShellAppSwitcherPopup *self = SHELL_APP_SWITCHER_POPUP (object);

  g_clear_handle_id (&self->thumbnail_timeout_id, g_source_remove);
  self->thumbnails = NULL;

  G_OBJECT_CLASS (shell_app_switcher_popup_parent_class)->dispose (object);
}

static void
shell_app_switcher_popup_class_init (ShellAppSwitcherPopupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  ShellSwitcherPopupClass *popup_class = SHELL_SWITCHER_POPUP_CLASS (klass);

  object_class->constructed = shell_app_switcher_popup_constructed;
  object_class->dispose = shell_app_switcher_popup_dispose;

  actor_class->allocate = app_popup_allocate;

  popup_class->get_n_items = app_popup_get_n_items;
  popup_class->initial_selection = app_popup_initial_selection;
  popup_class->key_press_handler = app_popup_key_press_handler;
  popup_class->scroll_handler = app_popup_scroll_handler;
  popup_class->item_activated_handler = app_popup_item_activated_handler;
  popup_class->is_actor_outside = app_popup_is_actor_outside;
  popup_class->select = app_popup_select;
  popup_class->finish = app_popup_finish;

  /**
   * ShellAppSwitcherPopup::window-activated:
   * @self: the popup
   * @window: the window that was activated
   * @timestamp: the time of the event that selected the window
   *
   * Emitted after the popup activated a window (switching to its workspace
   * if needed), to do the rest of `Main.activateWindow ()`: hide the overview
   * and close the calendar.
   */
  app_popup_signals[APP_POPUP_WINDOW_ACTIVATED] =
    window_activated_signal_new (G_TYPE_FROM_CLASS (klass));
}

static void
shell_app_switcher_popup_init (ShellAppSwitcherPopup *self)
{
  self->current_window = -1;
}

/**
 * shell_app_switcher_popup_new:
 *
 * Creates the popup with the running applications that have windows.
 * Show it with shell_switcher_popup_show_switcher ().
 *
 * Returns: (transfer none): a new #ShellAppSwitcherPopup, owned by the ui
 *   group
 */
ShellAppSwitcherPopup *
shell_app_switcher_popup_new (void)
{
  return g_object_new (SHELL_TYPE_APP_SWITCHER_POPUP, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellAltTabWindowIcon                                                     */
/* ------------------------------------------------------------------------ */

G_DEFINE_FINAL_TYPE (ShellAltTabWindowIcon, shell_alt_tab_window_icon, ST_TYPE_BOX_LAYOUT)

static void
shell_alt_tab_window_icon_dispose (GObject *object)
{
  ShellAltTabWindowIcon *self = SHELL_ALT_TAB_WINDOW_ICON (object);

  g_clear_object (&self->label);
  g_clear_object (&self->app);
  g_clear_object (&self->window);

  G_OBJECT_CLASS (shell_alt_tab_window_icon_parent_class)->dispose (object);
}

static void
shell_alt_tab_window_icon_class_init (ShellAltTabWindowIconClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_alt_tab_window_icon_dispose;
}

static void
shell_alt_tab_window_icon_init (ShellAltTabWindowIcon *self)
{
}

static ClutterActor *
window_icon_create_app_icon (ShellApp *app,
                             int       size)
{
  ClutterActor *app_icon;

  if (app != NULL)
    {
      app_icon = shell_app_create_icon_texture (app, size);
    }
  else
    {
      app_icon = g_object_new (ST_TYPE_ICON,
                               "icon-name", "application-x-executable",
                               "icon-size", size,
                               NULL);
    }

  g_object_set (app_icon,
                "x-expand", TRUE,
                "y-expand", TRUE,
                "x-align", CLUTTER_ACTOR_ALIGN_END,
                "y-align", CLUTTER_ACTOR_ALIGN_END,
                NULL);

  return app_icon;
}

static ShellAltTabWindowIcon *
window_icon_new (MetaWindow             *window,
                 ShellAltTabAppIconMode  mode)
{
  ShellAltTabWindowIcon *self;
  ShellWindowTracker *tracker = shell_window_tracker_get_default ();
  const char *title = meta_window_get_title (window);
  GObject *window_actor = meta_window_get_compositor_private (window);
  ClutterActor *icon;
  int scale_factor, size;

  self = g_object_new (SHELL_TYPE_ALT_TAB_WINDOW_ICON,
                       "style-class", "alt-tab-app",
                       "orientation", CLUTTER_ORIENTATION_VERTICAL,
                       NULL);
  self->window = g_object_ref (window);

  icon = g_object_new (ST_TYPE_WIDGET, NULL);
  clutter_actor_set_layout_manager (icon, clutter_bin_layout_new ());
  clutter_actor_add_child (CLUTTER_ACTOR (self), icon);

  /* Not a child of the icon: it labels the item and its text is shown by
   * the list */
  self->label = g_object_ref_sink (g_object_new (ST_TYPE_LABEL,
                                                 "text", title != NULL ? title : "",
                                                 NULL));

  self->app = shell_window_tracker_get_window_app (tracker, window);

  scale_factor = get_scale_factor ();

  switch (mode)
    {
    case SHELL_ALT_TAB_APP_ICON_MODE_BOTH:
      size = WINDOW_PREVIEW_SIZE;
      if (window_actor != NULL)
        clutter_actor_add_child (icon,
                                 create_window_clone (CLUTTER_ACTOR (window_actor),
                                                      size * scale_factor));

      if (self->app != NULL)
        clutter_actor_add_child (icon,
                                 window_icon_create_app_icon (self->app, APP_ICON_SIZE_SMALL));
      break;

    case SHELL_ALT_TAB_APP_ICON_MODE_APP_ICON_ONLY:
      size = APP_ICON_SIZE;
      clutter_actor_add_child (icon, window_icon_create_app_icon (self->app, size));
      break;

    case SHELL_ALT_TAB_APP_ICON_MODE_THUMBNAIL_ONLY:
    default:
      size = WINDOW_PREVIEW_SIZE;
      if (window_actor != NULL)
        clutter_actor_add_child (icon,
                                 create_window_clone (CLUTTER_ACTOR (window_actor),
                                                      size * scale_factor));
      break;
    }

  clutter_actor_set_size (icon, size * scale_factor, size * scale_factor);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellAltTabWindowSwitcher: the list of windows                            */
/* ------------------------------------------------------------------------ */

G_DEFINE_FINAL_TYPE (ShellAltTabWindowSwitcher, shell_alt_tab_window_switcher,
                     SHELL_TYPE_SWITCHER_LIST)

static ShellAltTabWindowIcon *
window_switcher_get_icon (ShellAltTabWindowSwitcher *self,
                          int                        index)
{
  if (self == NULL || self->icons == NULL || index < 0 || (guint) index >= self->icons->len)
    return NULL;

  return g_ptr_array_index (self->icons, index);
}

static void
window_switcher_remove_window (ShellAltTabWindowSwitcher *self,
                               MetaWindow                *window)
{
  guint i;

  if (!g_ptr_array_find (self->windows, window, &i))
    return;

  g_signal_handlers_disconnect_by_data (window, self);

  g_ptr_array_remove_index (self->icons, i);
  g_ptr_array_remove_index (self->windows, i);
  shell_switcher_list_remove_item (SHELL_SWITCHER_LIST (self), i);
}

static void
window_unmanaged_cb (MetaWindow                *window,
                     ShellAltTabWindowSwitcher *self)
{
  window_switcher_remove_window (self, window);
}

static void
window_switcher_get_preferred_height (ClutterActor *actor,
                                      float         for_width,
                                      float        *min_height_p,
                                      float        *natural_height_p)
{
  ShellAltTabWindowSwitcher *self = SHELL_ALT_TAB_WINDOW_SWITCHER (actor);
  float min_height = 0, natural_height = 0;
  float label_min = 0, label_natural = 0;
  double spacing;

  CLUTTER_ACTOR_CLASS (shell_alt_tab_window_switcher_parent_class)->get_preferred_height (actor,
                                                                                          for_width,
                                                                                          &min_height,
                                                                                          &natural_height);

  spacing = st_theme_node_get_padding (st_widget_get_theme_node (ST_WIDGET (actor)),
                                       ST_SIDE_BOTTOM);
  if (self->label != NULL)
    clutter_actor_get_preferred_height (CLUTTER_ACTOR (self->label), -1,
                                        &label_min, &label_natural);

  min_height += label_min + spacing;
  natural_height += label_natural + spacing;

  if (min_height_p)
    *min_height_p = min_height;
  if (natural_height_p)
    *natural_height_p = natural_height;
}

static void
window_switcher_allocate (ClutterActor          *actor,
                          const ClutterActorBox *box)
{
  ShellAltTabWindowSwitcher *self = SHELL_ALT_TAB_WINDOW_SWITCHER (actor);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  ClutterActorBox content_box, child_box, list_box;
  float label_height = 0, total_label_height;

  st_theme_node_get_content_box (theme_node, box, &content_box);
  if (self->label != NULL)
    label_height = clutter_actor_get_height (CLUTTER_ACTOR (self->label));
  total_label_height = label_height + st_theme_node_get_padding (theme_node, ST_SIDE_BOTTOM);

  list_box = *box;
  list_box.y2 -= total_label_height;
  CLUTTER_ACTOR_CLASS (shell_alt_tab_window_switcher_parent_class)->allocate (actor, &list_box);

  /* Hooking up the parent vfunc will call set_allocation () with the height
   * without the label height, so call it again with the correct size here. */
  clutter_actor_set_allocation (actor, box);

  child_box.x1 = content_box.x1;
  child_box.x2 = content_box.x2;
  child_box.y2 = content_box.y2;
  child_box.y1 = child_box.y2 - label_height;
  if (self->label != NULL)
    clutter_actor_allocate (CLUTTER_ACTOR (self->label), &child_box);
}

static void
window_switcher_highlight (ShellSwitcherList *list,
                           int                index,
                           gboolean           just_outline)
{
  ShellAltTabWindowSwitcher *self = SHELL_ALT_TAB_WINDOW_SWITCHER (list);
  ShellAltTabWindowIcon *icon;

  SHELL_SWITCHER_LIST_CLASS (shell_alt_tab_window_switcher_parent_class)->highlight (list,
                                                                                     index,
                                                                                     just_outline);

  if (self->label == NULL)
    return;

  icon = index == -1 ? NULL : window_switcher_get_icon (self, index);
  st_label_set_text (self->label,
                     icon != NULL ? st_label_get_text (icon->label) : "");
}

static void
shell_alt_tab_window_switcher_dispose (GObject *object)
{
  ShellAltTabWindowSwitcher *self = SHELL_ALT_TAB_WINDOW_SWITCHER (object);
  guint i;

  if (self->windows != NULL)
    {
      for (i = 0; i < self->windows->len; i++)
        g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->windows, i), self);
      g_ptr_array_set_size (self->windows, 0);
    }
  if (self->icons != NULL)
    g_ptr_array_set_size (self->icons, 0);

  g_clear_weak_pointer (&self->label);

  G_OBJECT_CLASS (shell_alt_tab_window_switcher_parent_class)->dispose (object);
}

static void
shell_alt_tab_window_switcher_finalize (GObject *object)
{
  ShellAltTabWindowSwitcher *self = SHELL_ALT_TAB_WINDOW_SWITCHER (object);

  g_clear_pointer (&self->icons, g_ptr_array_unref);
  g_clear_pointer (&self->windows, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_alt_tab_window_switcher_parent_class)->finalize (object);
}

static void
shell_alt_tab_window_switcher_class_init (ShellAltTabWindowSwitcherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  ShellSwitcherListClass *list_class = SHELL_SWITCHER_LIST_CLASS (klass);

  object_class->dispose = shell_alt_tab_window_switcher_dispose;
  object_class->finalize = shell_alt_tab_window_switcher_finalize;

  actor_class->get_preferred_height = window_switcher_get_preferred_height;
  actor_class->allocate = window_switcher_allocate;

  list_class->highlight = window_switcher_highlight;
}

static void
shell_alt_tab_window_switcher_init (ShellAltTabWindowSwitcher *self)
{
  self->icons = g_ptr_array_new_with_free_func (g_object_unref);
  self->windows = g_ptr_array_new_with_free_func (g_object_unref);
}

static ShellAltTabWindowSwitcher *
window_switcher_new (GList                  *windows,
                     ShellAltTabAppIconMode  mode)
{
  ShellAltTabWindowSwitcher *self;
  ClutterActor *label;
  GList *l;

  self = g_object_new (SHELL_TYPE_ALT_TAB_WINDOW_SWITCHER,
                       "square-items", TRUE,
                       NULL);

  label = g_object_new (ST_TYPE_LABEL,
                        "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), label);
  g_set_weak_pointer (&self->label, ST_LABEL (label));

  for (l = windows; l != NULL; l = l->next)
    {
      MetaWindow *window = l->data;
      ShellAltTabWindowIcon *icon = window_icon_new (window, mode);

      shell_switcher_list_add_item (SHELL_SWITCHER_LIST (self),
                                    CLUTTER_ACTOR (icon),
                                    CLUTTER_ACTOR (icon->label));
      g_ptr_array_add (self->icons, g_object_ref (icon));
      g_ptr_array_add (self->windows, g_object_ref (window));

      g_signal_connect_object (window, "unmanaged",
                               G_CALLBACK (window_unmanaged_cb), self, 0);
    }

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellWindowSwitcherPopup                                                  */
/* ------------------------------------------------------------------------ */

enum
{
  WINDOW_POPUP_WINDOW_ACTIVATED,

  WINDOW_POPUP_LAST_SIGNAL
};

static guint window_popup_signals[WINDOW_POPUP_LAST_SIGNAL] = { 0, };

G_DEFINE_FINAL_TYPE (ShellWindowSwitcherPopup, shell_window_switcher_popup,
                     SHELL_TYPE_SWITCHER_POPUP)

static ShellAltTabWindowSwitcher *
window_popup_get_list (ShellWindowSwitcherPopup *self)
{
  StWidget *list = shell_switcher_popup_get_switcher_list (SHELL_SWITCHER_POPUP (self));

  return SHELL_IS_ALT_TAB_WINDOW_SWITCHER (list) ? SHELL_ALT_TAB_WINDOW_SWITCHER (list) : NULL;
}

static int
window_popup_get_n_items (ShellSwitcherPopup *popup)
{
  ShellAltTabWindowSwitcher *list = window_popup_get_list (SHELL_WINDOW_SWITCHER_POPUP (popup));

  return list != NULL ? list->icons->len : 0;
}

static gboolean
window_popup_key_press_handler (ShellSwitcherPopup *popup,
                                uint32_t            keysym,
                                uint32_t            action)
{
  ShellWindowSwitcherPopup *self = SHELL_WINDOW_SWITCHER_POPUP (popup);
  gboolean rtl = clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL;

  if (action == META_KEYBINDING_ACTION_SWITCH_WINDOWS)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_next_index (popup));
  else if (action == META_KEYBINDING_ACTION_SWITCH_WINDOWS_BACKWARD)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_previous_index (popup));
  else if (keysym == CLUTTER_KEY_Left)
    shell_switcher_popup_select (popup,
                                 rtl ? shell_switcher_popup_get_next_index (popup) :
                                       shell_switcher_popup_get_previous_index (popup));
  else if (keysym == CLUTTER_KEY_Right)
    shell_switcher_popup_select (popup,
                                 rtl ? shell_switcher_popup_get_previous_index (popup) :
                                       shell_switcher_popup_get_next_index (popup));
  else if (keysym == CLUTTER_KEY_w || keysym == CLUTTER_KEY_W || keysym == CLUTTER_KEY_F4)
    {
      ShellAltTabWindowIcon *icon =
        window_switcher_get_icon (window_popup_get_list (self),
                                  shell_switcher_popup_get_selected_index (popup));

      if (icon != NULL)
        meta_window_delete (icon->window, shell_global_get_current_time (shell_global_get ()));
    }
  else
    return CLUTTER_EVENT_PROPAGATE;

  return CLUTTER_EVENT_STOP;
}

static void
window_popup_finish (ShellSwitcherPopup *popup,
                     uint32_t            timestamp)
{
  ShellWindowSwitcherPopup *self = SHELL_WINDOW_SWITCHER_POPUP (popup);
  ShellAltTabWindowIcon *icon =
    window_switcher_get_icon (window_popup_get_list (self),
                              shell_switcher_popup_get_selected_index (popup));

  if (icon != NULL)
    {
      uint32_t time = activate_window (icon->window, 0);

      g_signal_emit (self, window_popup_signals[WINDOW_POPUP_WINDOW_ACTIVATED], 0,
                     icon->window, time);
    }

  SHELL_SWITCHER_POPUP_CLASS (shell_window_switcher_popup_parent_class)->finish (popup, timestamp);
}

static void
shell_window_switcher_popup_constructed (GObject *object)
{
  ShellWindowSwitcherPopup *self = SHELL_WINDOW_SWITCHER_POPUP (object);
  g_autoptr (GSettings) settings = g_settings_new ("org.gnome.shell.window-switcher");
  g_autoptr (GList) windows = NULL;
  MetaWorkspace *workspace = NULL;
  ShellAltTabWindowSwitcher *list;
  ShellAltTabAppIconMode mode;

  G_OBJECT_CLASS (shell_window_switcher_popup_parent_class)->constructed (object);

  if (g_settings_get_boolean (settings, "current-workspace-only"))
    workspace = get_active_workspace ();
  windows = shell_alt_tab_get_windows (workspace);

  mode = g_settings_get_enum (settings, "app-icon-mode");
  list = window_switcher_new (windows, mode);
  shell_switcher_popup_set_switcher_list (SHELL_SWITCHER_POPUP (self), ST_WIDGET (list));
}

static void
shell_window_switcher_popup_class_init (ShellWindowSwitcherPopupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellSwitcherPopupClass *popup_class = SHELL_SWITCHER_POPUP_CLASS (klass);

  object_class->constructed = shell_window_switcher_popup_constructed;

  popup_class->get_n_items = window_popup_get_n_items;
  popup_class->key_press_handler = window_popup_key_press_handler;
  popup_class->finish = window_popup_finish;

  /**
   * ShellWindowSwitcherPopup::window-activated:
   * @self: the popup
   * @window: the window that was activated
   * @timestamp: the time of the event that selected the window
   *
   * Emitted after the popup activated a window (switching to its workspace
   * if needed), to do the rest of `Main.activateWindow ()`: hide the overview
   * and close the calendar.
   */
  window_popup_signals[WINDOW_POPUP_WINDOW_ACTIVATED] =
    window_activated_signal_new (G_TYPE_FROM_CLASS (klass));
}

static void
shell_window_switcher_popup_init (ShellWindowSwitcherPopup *self)
{
}

/**
 * shell_window_switcher_popup_new:
 *
 * Creates the popup with the windows of the workspace, or of all
 * workspaces. Show it with shell_switcher_popup_show_switcher ().
 *
 * Returns: (transfer none): a new #ShellWindowSwitcherPopup, owned by the ui
 *   group
 */
ShellWindowSwitcherPopup *
shell_window_switcher_popup_new (void)
{
  return g_object_new (SHELL_TYPE_WINDOW_SWITCHER_POPUP, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellCyclerHighlight                                                      */
/* ------------------------------------------------------------------------ */

struct _ShellCyclerHighlight
{
  StWidget parent;

  MetaWindow *window;          /* strong, nullable */
  ClutterActor *clone;         /* weak */
  ClutterActor *highlight;     /* weak */
};

G_DEFINE_FINAL_TYPE (ShellCyclerHighlight, shell_cycler_highlight, ST_TYPE_WIDGET)

static void
cycler_highlight_update_size (ShellCyclerHighlight *self)
{
  MtkRectangle buffer_rect, rect;

  if (self->window == NULL || self->highlight == NULL)
    return;

  meta_window_get_buffer_rect (self->window, &buffer_rect);
  meta_window_get_frame_rect (self->window, &rect);
  clutter_actor_set_size (self->highlight, rect.width, rect.height);
  clutter_actor_set_position (self->highlight,
                              rect.x - buffer_rect.x,
                              rect.y - buffer_rect.y);
  clutter_actor_show (self->highlight);
}

static void
cycler_highlight_size_changed_cb (MetaWindow           *window,
                                  ShellCyclerHighlight *self)
{
  cycler_highlight_update_size (self);
}

/* The setter of the JS `window` property */
static void
cycler_highlight_set_window (ShellCyclerHighlight *self,
                             MetaWindow           *window)
{
  ClutterActor *source = NULL;
  GObject *window_actor = NULL;

  if (self->window == window)
    return;

  if (self->window != NULL)
    g_signal_handlers_disconnect_by_data (self->window, self);

  g_set_object (&self->window, window);

  if (self->clone != NULL)
    source = clutter_clone_get_source (CLUTTER_CLONE (self->clone));
  if (META_IS_WINDOW_ACTOR (source))
    meta_window_actor_sync_visibility (META_WINDOW_ACTOR (source));

  if (self->window != NULL)
    window_actor = meta_window_get_compositor_private (self->window);

  if (window_actor != NULL)
    clutter_actor_hide (CLUTTER_ACTOR (window_actor));

  if (self->clone != NULL)
    clutter_clone_set_source (CLUTTER_CLONE (self->clone), CLUTTER_ACTOR (window_actor));

  if (self->window != NULL)
    {
      cycler_highlight_update_size (self);
      g_signal_connect (self->window, "size-changed",
                        G_CALLBACK (cycler_highlight_size_changed_cb), self);
    }
  else if (self->highlight != NULL)
    {
      clutter_actor_set_size (self->highlight, 0, 0);
      clutter_actor_hide (self->highlight);
    }
}

static void
cycler_highlight_destroy_cb (ClutterActor *actor,
                             gpointer      user_data)
{
  cycler_highlight_set_window (SHELL_CYCLER_HIGHLIGHT (actor), NULL);
}

static void
shell_cycler_highlight_constructed (GObject *object)
{
  ShellCyclerHighlight *self = SHELL_CYCLER_HIGHLIGHT (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *clone, *highlight;
  ClutterConstraint *constraint;

  G_OBJECT_CLASS (shell_cycler_highlight_parent_class)->constructed (object);

  clutter_actor_set_layout_manager (actor, clutter_bin_layout_new ());

  clone = clutter_clone_new (NULL);
  clutter_actor_add_child (actor, clone);
  g_set_weak_pointer (&self->clone, clone);

  highlight = g_object_new (ST_TYPE_WIDGET,
                            "style-class", "cycler-highlight",
                            NULL);
  clutter_actor_add_child (actor, highlight);
  g_set_weak_pointer (&self->highlight, highlight);

  constraint = clutter_bind_constraint_new (NULL, CLUTTER_BIND_ALL, 0);
  g_object_bind_property (clone, "source", constraint, "source", G_BINDING_DEFAULT);
  clutter_actor_add_constraint (actor, constraint);

  g_signal_connect (self, "destroy", G_CALLBACK (cycler_highlight_destroy_cb), NULL);
}

static void
shell_cycler_highlight_dispose (GObject *object)
{
  ShellCyclerHighlight *self = SHELL_CYCLER_HIGHLIGHT (object);

  if (self->window != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->window, self);
      g_clear_object (&self->window);
    }
  g_clear_weak_pointer (&self->clone);
  g_clear_weak_pointer (&self->highlight);

  G_OBJECT_CLASS (shell_cycler_highlight_parent_class)->dispose (object);
}

static void
shell_cycler_highlight_class_init (ShellCyclerHighlightClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_cycler_highlight_constructed;
  object_class->dispose = shell_cycler_highlight_dispose;
}

static void
shell_cycler_highlight_init (ShellCyclerHighlight *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellCyclerList                                                           */
/* ------------------------------------------------------------------------ */

/* We don't show an actual popup, so just provide what ShellSwitcherPopup
 * expects instead of inheriting from ShellSwitcherList */

struct _ShellCyclerList
{
  StWidget parent;
};

G_DEFINE_FINAL_TYPE (ShellCyclerList, shell_cycler_list, ST_TYPE_WIDGET)

static void
shell_cycler_list_class_init (ShellCyclerListClass *klass)
{
  const char *names[] = { "item-activated", "item-entered", "item-removed" };
  guint i;

  /* Never emitted */
  for (i = 0; i < G_N_ELEMENTS (names); i++)
    g_signal_new (names[i],
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT);
}

static void
shell_cycler_list_init (ShellCyclerList *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellCyclerPopup                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  CYCLER_WINDOW_ACTIVATED,
  CYCLER_SKIP_NEXT_EFFECT,
  CYCLER_MOVE_WINDOW,

  CYCLER_LAST_SIGNAL
};

static guint cycler_signals[CYCLER_LAST_SIGNAL] = { 0, };

typedef struct
{
  GPtrArray *windows;          /* MetaWindow, strong */
  ClutterActor *highlight;     /* weak, ShellCyclerHighlight */
} ShellCyclerPopupPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (ShellCyclerPopup, shell_cycler_popup,
                                     SHELL_TYPE_SWITCHER_POPUP)

/* this._items = this._getWindows () */
static void
cycler_popup_set_windows (ShellCyclerPopup *self,
                          GList            *windows)
{
  ShellCyclerPopupPrivate *priv = shell_cycler_popup_get_instance_private (self);

  g_clear_pointer (&priv->windows, g_ptr_array_unref);
  priv->windows = windows_array_new (windows);
}

static int
cycler_popup_get_n_items (ShellSwitcherPopup *popup)
{
  ShellCyclerPopupPrivate *priv =
    shell_cycler_popup_get_instance_private (SHELL_CYCLER_POPUP (popup));

  return priv->windows != NULL ? priv->windows->len : 0;
}

static MetaWindow *
cycler_popup_get_window (ShellCyclerPopup *self,
                         int               index)
{
  ShellCyclerPopupPrivate *priv = shell_cycler_popup_get_instance_private (self);

  if (priv->windows == NULL || index < 0 || (guint) index >= priv->windows->len)
    return NULL;

  return g_ptr_array_index (priv->windows, index);
}

/* There is no list to highlight the item in: highlight its window */
static void
cycler_popup_select (ShellSwitcherPopup *popup,
                     int                 index)
{
  ShellCyclerPopup *self = SHELL_CYCLER_POPUP (popup);
  ShellCyclerPopupPrivate *priv = shell_cycler_popup_get_instance_private (self);

  shell_switcher_popup_set_selected_index (popup, index);

  /* _highlightItem () */
  if (priv->highlight != NULL)
    {
      cycler_highlight_set_window (SHELL_CYCLER_HIGHLIGHT (priv->highlight),
                                   cycler_popup_get_window (self, index));
      clutter_actor_set_child_above_sibling (shell_global_get_window_group (shell_global_get ()),
                                             priv->highlight, NULL);
    }
}

static void
cycler_popup_finish (ShellSwitcherPopup *popup,
                     uint32_t            timestamp)
{
  ShellCyclerPopup *self = SHELL_CYCLER_POPUP (popup);
  MetaWindow *window = cycler_popup_get_window (self,
                                                shell_switcher_popup_get_selected_index (popup));

  if (window != NULL)
    {
      ShellGlobal *global = shell_global_get ();
      MetaWorkspace *workspace = meta_window_get_workspace (window);
      MetaWorkspace *active_workspace = get_active_workspace ();
      gboolean minimized = FALSE;

      g_object_get (window, "minimized", &minimized, NULL);
      if (minimized)
        {
          GObject *window_actor = meta_window_get_compositor_private (window);

          if (window_actor != NULL)
            g_signal_emit (self, cycler_signals[CYCLER_SKIP_NEXT_EFFECT], 0, window_actor);
          meta_window_unminimize (window);
        }

      if (active_workspace == workspace)
        {
          uint32_t time = activate_window (window, 0);

          g_signal_emit (self, cycler_signals[CYCLER_WINDOW_ACTIVATED], 0, window, time);
        }
      else
        {
          /* If the selected window is on a different workspace, we don't
           * want it to disappear, then slide in with the workspace; instead,
           * always activate it on the active workspace ... */
          meta_workspace_activate_with_focus (active_workspace, window,
                                              shell_global_get_current_time (global));

          /* ... then slide it over to the original workspace if necessary */
          if (workspace != NULL)
            g_signal_emit (self, cycler_signals[CYCLER_MOVE_WINDOW], 0, window, workspace);
        }
    }

  SHELL_SWITCHER_POPUP_CLASS (shell_cycler_popup_parent_class)->finish (popup, timestamp);
}

static void
cycler_popup_destroy_cb (ClutterActor *actor,
                         gpointer      user_data)
{
  ShellCyclerPopupPrivate *priv =
    shell_cycler_popup_get_instance_private (SHELL_CYCLER_POPUP (actor));

  if (priv->highlight != NULL)
    clutter_actor_destroy (priv->highlight);
}

static void
shell_cycler_popup_constructed (GObject *object)
{
  ShellCyclerPopup *self = SHELL_CYCLER_POPUP (object);
  ShellCyclerPopupPrivate *priv = shell_cycler_popup_get_instance_private (self);
  ClutterActor *highlight;

  G_OBJECT_CLASS (shell_cycler_popup_parent_class)->constructed (object);

  highlight = g_object_new (SHELL_TYPE_CYCLER_HIGHLIGHT, NULL);
  clutter_actor_add_child (shell_global_get_window_group (shell_global_get ()), highlight);
  g_set_weak_pointer (&priv->highlight, highlight);

  shell_switcher_popup_set_switcher_list (SHELL_SWITCHER_POPUP (self),
                                          g_object_new (SHELL_TYPE_CYCLER_LIST, NULL));

  g_signal_connect (self, "destroy", G_CALLBACK (cycler_popup_destroy_cb), NULL);
}

static void
shell_cycler_popup_dispose (GObject *object)
{
  ShellCyclerPopupPrivate *priv =
    shell_cycler_popup_get_instance_private (SHELL_CYCLER_POPUP (object));

  g_clear_weak_pointer (&priv->highlight);

  G_OBJECT_CLASS (shell_cycler_popup_parent_class)->dispose (object);
}

static void
shell_cycler_popup_finalize (GObject *object)
{
  ShellCyclerPopupPrivate *priv =
    shell_cycler_popup_get_instance_private (SHELL_CYCLER_POPUP (object));

  g_clear_pointer (&priv->windows, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_cycler_popup_parent_class)->finalize (object);
}

static void
shell_cycler_popup_class_init (ShellCyclerPopupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellSwitcherPopupClass *popup_class = SHELL_SWITCHER_POPUP_CLASS (klass);

  object_class->constructed = shell_cycler_popup_constructed;
  object_class->dispose = shell_cycler_popup_dispose;
  object_class->finalize = shell_cycler_popup_finalize;

  popup_class->get_n_items = cycler_popup_get_n_items;
  popup_class->select = cycler_popup_select;
  popup_class->finish = cycler_popup_finish;

  /**
   * ShellCyclerPopup::window-activated:
   * @self: the popup
   * @window: the window that was activated
   * @timestamp: the time of the event that selected the window
   *
   * Emitted after the popup activated a window (switching to its workspace
   * if needed), to do the rest of `Main.activateWindow ()`: hide the overview
   * and close the calendar.
   */
  cycler_signals[CYCLER_WINDOW_ACTIVATED] =
    window_activated_signal_new (G_TYPE_FROM_CLASS (klass));

  /**
   * ShellCyclerPopup::skip-next-effect:
   * @self: the popup
   * @actor: the actor of the window that is about to be unminimized
   *
   * Emitted before the selected window is unminimized, so that no animation
   * runs (`Main.wm.skipNextEffect ()`).
   */
  cycler_signals[CYCLER_SKIP_NEXT_EFFECT] =
    g_signal_new ("skip-next-effect",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  CLUTTER_TYPE_ACTOR);

  /**
   * ShellCyclerPopup::move-window:
   * @self: the popup
   * @window: the window that was activated on the active workspace
   * @workspace: the workspace the window is to be moved back to
   *
   * Emitted when the selected window is on another workspace, after it was
   * activated on the active workspace, to slide it over to its own
   * workspace (`Main.wm.actionMoveWindow ()`).
   */
  cycler_signals[CYCLER_MOVE_WINDOW] =
    g_signal_new ("move-window",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  META_TYPE_WINDOW,
                  META_TYPE_WORKSPACE);
}

static void
shell_cycler_popup_init (ShellCyclerPopup *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellGroupCyclerPopup                                                     */
/* ------------------------------------------------------------------------ */

struct _ShellGroupCyclerPopup
{
  ShellCyclerPopup parent;
};

G_DEFINE_FINAL_TYPE (ShellGroupCyclerPopup, shell_group_cycler_popup,
                     SHELL_TYPE_CYCLER_POPUP)

static gboolean
group_cycler_key_press_handler (ShellSwitcherPopup *popup,
                                uint32_t            keysym,
                                uint32_t            action)
{
  if (action == META_KEYBINDING_ACTION_CYCLE_GROUP)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_next_index (popup));
  else if (action == META_KEYBINDING_ACTION_CYCLE_GROUP_BACKWARD)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_previous_index (popup));
  else
    return CLUTTER_EVENT_PROPAGATE;

  return CLUTTER_EVENT_STOP;
}

static void
shell_group_cycler_popup_constructed (GObject *object)
{
  ShellGroupCyclerPopup *self = SHELL_GROUP_CYCLER_POPUP (object);
  g_autoptr (GSettings) settings = g_settings_new ("org.gnome.shell.app-switcher");
  g_autoptr (GList) windows = NULL;
  ShellApp *app;

  G_OBJECT_CLASS (shell_group_cycler_popup_parent_class)->constructed (object);

  app = shell_window_tracker_get_focus_app (shell_window_tracker_get_default ());
  if (app != NULL)
    {
      g_autoptr (GSList) app_windows = shell_app_get_windows (app);
      gboolean current_workspace_only =
        g_settings_get_boolean (settings, "current-workspace-only");
      MetaWorkspace *workspace = current_workspace_only ? get_active_workspace () : NULL;
      GSList *l;

      for (l = app_windows; l != NULL; l = l->next)
        {
          if (!current_workspace_only || meta_window_located_on_workspace (l->data, workspace))
            windows = g_list_prepend (windows, l->data);
        }
      windows = g_list_reverse (windows);
    }

  cycler_popup_set_windows (SHELL_CYCLER_POPUP (self), windows);
}

static void
shell_group_cycler_popup_class_init (ShellGroupCyclerPopupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellSwitcherPopupClass *popup_class = SHELL_SWITCHER_POPUP_CLASS (klass);

  object_class->constructed = shell_group_cycler_popup_constructed;

  popup_class->key_press_handler = group_cycler_key_press_handler;
}

static void
shell_group_cycler_popup_init (ShellGroupCyclerPopup *self)
{
}

/**
 * shell_group_cycler_popup_new:
 *
 * Creates the popup that cycles through the windows of the focused
 * application. Show it with shell_switcher_popup_show_switcher ().
 *
 * Returns: (transfer none): a new #ShellGroupCyclerPopup, owned by the ui
 *   group
 */
ShellGroupCyclerPopup *
shell_group_cycler_popup_new (void)
{
  return g_object_new (SHELL_TYPE_GROUP_CYCLER_POPUP, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellWindowCyclerPopup                                                    */
/* ------------------------------------------------------------------------ */

struct _ShellWindowCyclerPopup
{
  ShellCyclerPopup parent;
};

G_DEFINE_FINAL_TYPE (ShellWindowCyclerPopup, shell_window_cycler_popup,
                     SHELL_TYPE_CYCLER_POPUP)

static gboolean
window_cycler_key_press_handler (ShellSwitcherPopup *popup,
                                 uint32_t            keysym,
                                 uint32_t            action)
{
  if (action == META_KEYBINDING_ACTION_CYCLE_WINDOWS)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_next_index (popup));
  else if (action == META_KEYBINDING_ACTION_CYCLE_WINDOWS_BACKWARD)
    shell_switcher_popup_select (popup, shell_switcher_popup_get_previous_index (popup));
  else
    return CLUTTER_EVENT_PROPAGATE;

  return CLUTTER_EVENT_STOP;
}

static void
shell_window_cycler_popup_constructed (GObject *object)
{
  ShellWindowCyclerPopup *self = SHELL_WINDOW_CYCLER_POPUP (object);
  g_autoptr (GSettings) settings = g_settings_new ("org.gnome.shell.window-switcher");
  g_autoptr (GList) windows = NULL;
  MetaWorkspace *workspace = NULL;

  G_OBJECT_CLASS (shell_window_cycler_popup_parent_class)->constructed (object);

  if (g_settings_get_boolean (settings, "current-workspace-only"))
    workspace = get_active_workspace ();

  windows = shell_alt_tab_get_windows (workspace);
  cycler_popup_set_windows (SHELL_CYCLER_POPUP (self), windows);
}

static void
shell_window_cycler_popup_class_init (ShellWindowCyclerPopupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellSwitcherPopupClass *popup_class = SHELL_SWITCHER_POPUP_CLASS (klass);

  object_class->constructed = shell_window_cycler_popup_constructed;

  popup_class->key_press_handler = window_cycler_key_press_handler;
}

static void
shell_window_cycler_popup_init (ShellWindowCyclerPopup *self)
{
}

/**
 * shell_window_cycler_popup_new:
 *
 * Creates the popup that cycles through the windows of the workspace, or of
 * all workspaces. Show it with shell_switcher_popup_show_switcher ().
 *
 * Returns: (transfer none): a new #ShellWindowCyclerPopup, owned by the ui
 *   group
 */
ShellWindowCyclerPopup *
shell_window_cycler_popup_new (void)
{
  return g_object_new (SHELL_TYPE_WINDOW_CYCLER_POPUP, NULL);
}
