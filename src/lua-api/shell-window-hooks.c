/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell-window-hooks.c: the window.*, workspace.* hook points
 * (docs/api/hooks.md) and the pieces of the view of windows that they share
 * with shell.windows (docs/api/window.md).
 *
 * Which hook is emitted from where:
 *
 *  - window.mapped, .minimized, .unminimized and workspace.switched come
 *    from #ShellWindowManager (its handlers of the "map", "minimize",
 *    "unminimize" and "switch-workspace" signals of #ShellWM), at the start
 *    of the handler, before the animation; window.unmapped from
 *    #MetaWindow::unmanaging (and from the "destroy" handler, which comes
 *    later for a window the compositor was showing);
 *  - window.focused from notify::focus-window of #MetaDisplay;
 *  - window.title-changed, .state-changed, .workspace-changed and
 *    .geometry-changed from the notify::title, notify::maximized-*,
 *    notify::fullscreen, notify::above, notify::on-all-workspaces,
 *    workspace-changed, position-changed and size-changed signals of the
 *    #MetaWindow, which are connected when the display creates the window (or
 *    here at the start for the windows that exist already); the geometry
 *    signals only mark the window, one idle per window sends the hook with
 *    the frame rectangle of that moment;
 *  - window.monitor-changed from window-entered-monitor of #MetaDisplay;
 *  - workspace.added and .removed from the #MetaWorkspaceManager.
 *
 * Ordering (hooks.md): window.mapped precedes every other window.* hook of
 * a window and window.unmapped is the last. A window is "announced" the first
 * time one of its hooks is due after the shell saw it map, and until then
 * nothing is emitted for it; so a change of the title or the monitor while a
 * window is being set up is folded into the window.mapped payload, and a
 * window that is not listed (skip-taskbar: docks, the shell's own chrome)
 * never shows up.
 *
 * window.mapped has a replay (shell_hook_registry_set_replay ()): a new
 * listener gets one event per announced window, from an idle.
 */

#include "config.h"

#include <string.h>

#include <meta/meta-enum-types.h>

#include "lua-api/shell-window-hooks.h"
#include "shell-app.h"
#include "shell-global.h"
#include "shell-window-tracker.h"
#include "shell-hooks.h"
#include "shell-lua-handle.h"

/* "W" of hooks.md */
#define W_SCHEMA "window-id:t title:s wm-class:s app-id:s workspace:i monitor:i pid:u"

/* ------------------------------------------------------------------------ */
/* What is known of a window                                                */
/* ------------------------------------------------------------------------ */

typedef struct
{
  char     *title;
  int       workspace;   /* 1-based, 0: all workspaces (or none) */
  int       monitor;     /* 1-based, 0: none yet */
  gboolean  maximized;
  gboolean  fullscreen;
  gboolean  above;
  gboolean  on_all_workspaces;
  int       x, y, width, height;   /* the frame rectangle last reported (or seen while not announced) */
  guint     geometry_idle;         /* source id of the pending window.geometry-changed, 0 */
  gboolean  mapped;      /* the shell saw the window map (or it predates us) */
  gboolean  announced;   /* window.mapped was emitted */
} WindowInfo;

static GQuark
window_info_quark (void)
{
  static GQuark quark = 0;

  if (G_UNLIKELY (quark == 0))
    quark = g_quark_from_static_string ("shell-window-hooks-info");
  return quark;
}

static void
window_info_free (gpointer data)
{
  WindowInfo *info = data;

  g_clear_handle_id (&info->geometry_idle, g_source_remove);
  g_free (info->title);
  g_free (info);
}

static WindowInfo *
window_info_get (MetaWindow *window)
{
  return g_object_get_qdata (G_OBJECT (window), window_info_quark ());
}

/* ------------------------------------------------------------------------ */
/* Shared helpers                                                           */
/* ------------------------------------------------------------------------ */

static int emitting_depth = 0;

/* The one ShellWindowHooks of the shell's window manager (weak): the replay of
 * window.mapped and the idle of window.geometry-changed run without an
 * instance at hand. */
static ShellWindowHooks *current_hooks = NULL;

gboolean
shell_window_hooks_is_emitting (void)
{
  return emitting_depth > 0;
}

gboolean
shell_window_hooks_is_listed (MetaWindow *window)
{
  return window != NULL && !meta_window_is_skip_taskbar (window);
}

int
shell_window_hooks_get_workspace_number (MetaWindow *window)
{
  MetaWorkspace *workspace;

  if (meta_window_is_on_all_workspaces (window))
    return 0;
  workspace = meta_window_get_workspace (window);
  return workspace != NULL ? meta_workspace_index (workspace) + 1 : 0;
}

int
shell_window_hooks_get_monitor_number (MetaWindow *window)
{
  int monitor = meta_window_get_monitor (window);

  if (monitor < 0)
    {
      MetaDisplay *display = meta_window_get_display (window);

      monitor = display != NULL ? meta_display_get_primary_monitor (display) : 0;
    }
  return monitor + 1;
}

char *
shell_window_hooks_dup_app_id (MetaWindow *window)
{
  ShellWindowTracker *tracker = shell_window_tracker_get_default ();
  g_autoptr (ShellApp) app = NULL;
  char *id = NULL;

  if (tracker == NULL)
    return NULL;
  app = shell_window_tracker_get_window_app (tracker, window);
  if (app != NULL && !shell_app_is_window_backed (app))
    id = g_strdup (shell_app_get_id (app));
  return id;
}

GList *
shell_window_hooks_list_windows (MetaDisplay *display)
{
  MetaWorkspaceManager *workspace_manager = meta_display_get_workspace_manager (display);
  MetaWorkspace *active = meta_workspace_manager_get_active_workspace (workspace_manager);
  GList *tab_list = meta_display_get_tab_list (display, META_TAB_LIST_NORMAL_ALL, active);
  GList *result = NULL, *l;

  for (l = tab_list; l != NULL; l = l->next)
    {
      if (shell_window_hooks_is_listed (l->data))
        result = g_list_prepend (result, l->data);
    }
  g_list_free (tab_list);
  return g_list_reverse (result);
}

MetaWindow *
shell_window_hooks_lookup_window (MetaDisplay *display,
                                  guint64      id)
{
  g_autoptr (GList) windows = meta_display_list_all_windows (display);
  GList *l;

  for (l = windows; l != NULL; l = l->next)
    {
      MetaWindow *window = l->data;

      if (meta_window_get_id (window) == id)
        return shell_window_hooks_is_listed (window) ? window : NULL;
    }
  return NULL;
}

MetaWindow *
shell_window_hooks_get_focused_window (MetaDisplay *display)
{
  MetaWindow *window = meta_display_get_focus_window (display);

  return shell_window_hooks_is_listed (window) ? window : NULL;
}

/* ------------------------------------------------------------------------ */
/* Emission                                                                 */
/* ------------------------------------------------------------------------ */

static GPtrArray *replay_window_mapped (ShellHookRegistry *registry,
                                        const char        *name,
                                        gpointer           user_data);

void
shell_window_hooks_declare (void)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();

  shell_hook_registry_declare (registry, "window.mapped", W_SCHEMA, FALSE,
                               "A window was mapped");
  shell_hook_registry_declare (registry, "window.unmapped", W_SCHEMA, FALSE,
                               "A window was destroyed");
  shell_hook_registry_declare (registry, "window.focused",
                               W_SCHEMA " previous-window-id:t", FALSE,
                               "A window got the focus");
  shell_hook_registry_declare (registry, "window.minimized", W_SCHEMA, FALSE,
                               "A window was minimized");
  shell_hook_registry_declare (registry, "window.unminimized", W_SCHEMA, FALSE,
                               "A window was unminimized");
  shell_hook_registry_declare (registry, "window.title-changed", W_SCHEMA " old-title:s", FALSE,
                               "The title of a window changed");
  shell_hook_registry_declare (registry, "window.workspace-changed", W_SCHEMA " from:i to:i", FALSE,
                               "A window moved to another workspace");
  shell_hook_registry_declare (registry, "window.monitor-changed", W_SCHEMA " from:i to:i", FALSE,
                               "A window moved to another monitor");
  shell_hook_registry_declare (registry, "window.state-changed",
                               W_SCHEMA " maximized:b fullscreen:b above:b on-all-workspaces:b changed:as", FALSE,
                               "A window was maximized, unmaximized, made fullscreen, left fullscreen, "
                               "made always-on-top or not, or put on all workspaces or not");
  shell_hook_registry_declare (registry, "window.geometry-changed",
                               W_SCHEMA " x:i y:i width:i height:i", FALSE,
                               "A window moved or was resized");
  shell_hook_registry_declare (registry, "workspace.switched", "from:i to:i", FALSE,
                               "The active workspace changed");
  shell_hook_registry_declare (registry, "workspace.added", "index:i", FALSE,
                               "A workspace was added");
  shell_hook_registry_declare (registry, "workspace.removed", "index:i", FALSE,
                               "A workspace was removed");

  /* A new listener of window.mapped is told about the windows that exist */
  shell_hook_registry_set_replay (registry, "window.mapped", replay_window_mapped, NULL);
}

static void
emit_hook (const char     *name,
           GVariantBuilder *builder)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  GVariant *payload = g_variant_builder_end (builder);

  if (!shell_hook_registry_is_declared (registry, name))
    {
      g_variant_unref (g_variant_ref_sink (payload));
      return;
    }

  emitting_depth++;
  shell_hook_registry_emit_after (registry, name, payload);
  emitting_depth--;
}

/* "W": the keys every window.* payload carries */
static void
window_payload_init (GVariantBuilder *builder,
                     MetaWindow      *window)
{
  g_autofree char *app_id = shell_window_hooks_dup_app_id (window);
  const char *title = meta_window_get_title (window);
  const char *wm_class = meta_window_get_wm_class (window);
  int workspace = shell_window_hooks_get_workspace_number (window);
  pid_t pid = meta_window_get_pid (window);

  g_variant_builder_init (builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (builder, "{sv}", "window-id",
                         g_variant_new_uint64 (meta_window_get_id (window)));
  g_variant_builder_add (builder, "{sv}", "title",
                         g_variant_new_string (title != NULL ? title : ""));
  g_variant_builder_add (builder, "{sv}", "wm-class",
                         g_variant_new_string (wm_class != NULL ? wm_class : ""));
  if (app_id != NULL)
    g_variant_builder_add (builder, "{sv}", "app-id", g_variant_new_string (app_id));
  if (workspace > 0)
    g_variant_builder_add (builder, "{sv}", "workspace", g_variant_new_int32 (workspace));
  g_variant_builder_add (builder, "{sv}", "monitor",
                         g_variant_new_int32 (shell_window_hooks_get_monitor_number (window)));
  if (pid > 0)
    g_variant_builder_add (builder, "{sv}", "pid", g_variant_new_uint32 ((guint32) pid));
}

/* ------------------------------------------------------------------------ */
/* ShellWindowHooks                                                         */
/* ------------------------------------------------------------------------ */

struct _ShellWindowHooks
{
  GObject parent_instance;

  MetaDisplay          *display;            /* weak */
  MetaWorkspaceManager *workspace_manager;  /* weak */
  guint64               focused_id;         /* the listed window that has the focus, 0 */
  GPtrArray            *workspaces;         /* MetaWorkspace, strong: to kill the handles of removed ones */
};

G_DEFINE_FINAL_TYPE (ShellWindowHooks, shell_window_hooks, G_TYPE_OBJECT)

static WindowInfo *track_window (ShellWindowHooks *self, MetaWindow *window);

/* The window is shown to extensions: it has mapped and is listed. The first
 * time, window.mapped goes out. */
static gboolean
window_ready (ShellWindowHooks *self,
              MetaWindow       *window,
              WindowInfo       *info)
{
  if (!info->mapped || !shell_window_hooks_is_listed (window))
    return FALSE;

  if (!info->announced)
    {
      GVariantBuilder builder;

      info->announced = TRUE;
      window_payload_init (&builder, window);
      emit_hook ("window.mapped", &builder);
    }
  return TRUE;
}

static void
window_refresh_frame (WindowInfo *info,
                      MetaWindow *window)
{
  MtkRectangle frame;

  meta_window_get_frame_rect (window, &frame);
  info->x = frame.x;
  info->y = frame.y;
  info->width = frame.width;
  info->height = frame.height;
}

static void
window_refresh_info (WindowInfo *info,
                     MetaWindow *window)
{
  const char *title = meta_window_get_title (window);

  g_free (info->title);
  info->title = g_strdup (title != NULL ? title : "");
  info->workspace = shell_window_hooks_get_workspace_number (window);
  info->monitor = meta_window_get_monitor (window) + 1;
  if (info->monitor < 0)
    info->monitor = 0;
  info->maximized = meta_window_is_maximized (window);
  info->fullscreen = meta_window_is_fullscreen (window);
  info->above = meta_window_is_above (window);
  info->on_all_workspaces = meta_window_is_on_all_workspaces (window);
  window_refresh_frame (info, window);
}

static void
window_title_cb (MetaWindow       *window,
                 GParamSpec       *pspec,
                 ShellWindowHooks *self)
{
  WindowInfo *info = window_info_get (window);
  const char *title = meta_window_get_title (window);
  g_autofree char *old_title = NULL;

  if (info == NULL)
    return;
  if (title == NULL)
    title = "";
  if (g_str_equal (info->title, title))
    return;

  old_title = g_steal_pointer (&info->title);
  info->title = g_strdup (title);

  if (window_ready (self, window, info))
    {
      GVariantBuilder builder;

      window_payload_init (&builder, window);
      g_variant_builder_add (&builder, "{sv}", "old-title", g_variant_new_string (old_title));
      emit_hook ("window.title-changed", &builder);
    }
}

static void
window_state_cb (MetaWindow       *window,
                 GParamSpec       *pspec,
                 ShellWindowHooks *self)
{
  WindowInfo *info = window_info_get (window);
  gboolean maximized = meta_window_is_maximized (window);
  gboolean fullscreen = meta_window_is_fullscreen (window);
  gboolean above = meta_window_is_above (window);
  gboolean on_all_workspaces = meta_window_is_on_all_workspaces (window);
  GVariantBuilder changed;
  gboolean any = FALSE;

  if (info == NULL)
    return;

  g_variant_builder_init (&changed, G_VARIANT_TYPE ("as"));
  if (maximized != info->maximized)
    {
      g_variant_builder_add (&changed, "s", "maximized");
      any = TRUE;
    }
  if (fullscreen != info->fullscreen)
    {
      g_variant_builder_add (&changed, "s", "fullscreen");
      any = TRUE;
    }
  if (above != info->above)
    {
      g_variant_builder_add (&changed, "s", "above");
      any = TRUE;
    }
  if (on_all_workspaces != info->on_all_workspaces)
    {
      g_variant_builder_add (&changed, "s", "on-all-workspaces");
      any = TRUE;
    }
  info->maximized = maximized;
  info->fullscreen = fullscreen;
  info->above = above;
  info->on_all_workspaces = on_all_workspaces;

  if (any && window_ready (self, window, info))
    {
      GVariantBuilder builder;

      window_payload_init (&builder, window);
      g_variant_builder_add (&builder, "{sv}", "maximized", g_variant_new_boolean (maximized));
      g_variant_builder_add (&builder, "{sv}", "fullscreen", g_variant_new_boolean (fullscreen));
      g_variant_builder_add (&builder, "{sv}", "above", g_variant_new_boolean (above));
      g_variant_builder_add (&builder, "{sv}", "on-all-workspaces",
                             g_variant_new_boolean (on_all_workspaces));
      g_variant_builder_add (&builder, "{sv}", "changed", g_variant_builder_end (&changed));
      emit_hook ("window.state-changed", &builder);
    }
  else
    {
      g_variant_builder_clear (&changed);
    }
}

/* window.geometry-changed: position-changed and size-changed come in bursts
 * (every configure of a resize), so they only mark the window; one idle per
 * window reports the frame rectangle it has by then, if it differs from the
 * last one reported. */
static gboolean
geometry_idle_cb (gpointer data)
{
  MetaWindow *window = data;
  WindowInfo *info = window_info_get (window);
  MtkRectangle frame;

  if (info == NULL)
    return G_SOURCE_REMOVE;
  info->geometry_idle = 0;

  meta_window_get_frame_rect (window, &frame);
  if (frame.x == info->x && frame.y == info->y &&
      frame.width == info->width && frame.height == info->height)
    return G_SOURCE_REMOVE;

  info->x = frame.x;
  info->y = frame.y;
  info->width = frame.width;
  info->height = frame.height;

  if (current_hooks != NULL && window_ready (current_hooks, window, info))
    {
      GVariantBuilder builder;

      window_payload_init (&builder, window);
      g_variant_builder_add (&builder, "{sv}", "x", g_variant_new_int32 (frame.x));
      g_variant_builder_add (&builder, "{sv}", "y", g_variant_new_int32 (frame.y));
      g_variant_builder_add (&builder, "{sv}", "width", g_variant_new_int32 (frame.width));
      g_variant_builder_add (&builder, "{sv}", "height", g_variant_new_int32 (frame.height));
      emit_hook ("window.geometry-changed", &builder);
    }
  return G_SOURCE_REMOVE;
}

static void
window_geometry_cb (MetaWindow       *window,
                    ShellWindowHooks *self)
{
  WindowInfo *info = window_info_get (window);

  if (info != NULL && info->geometry_idle == 0)
    info->geometry_idle = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, geometry_idle_cb, window, NULL);
}

static void
window_workspace_cb (MetaWindow       *window,
                     ShellWindowHooks *self)
{
  WindowInfo *info = window_info_get (window);
  int workspace = shell_window_hooks_get_workspace_number (window);
  int old;

  if (info == NULL || workspace == info->workspace)
    return;

  old = info->workspace;
  info->workspace = workspace;

  if (window_ready (self, window, info))
    {
      GVariantBuilder builder;

      window_payload_init (&builder, window);
      if (old > 0)
        g_variant_builder_add (&builder, "{sv}", "from", g_variant_new_int32 (old));
      if (workspace > 0)
        g_variant_builder_add (&builder, "{sv}", "to", g_variant_new_int32 (workspace));
      emit_hook ("window.workspace-changed", &builder);
    }
}

static void
window_unmanaging_cb (MetaWindow       *window,
                      ShellWindowHooks *self)
{
  /* The first thing of meta_window_unmanage (): the window still has its
   * workspace and title for the payload, but it is already out of the lists
   * of the display, so windows.get () no longer finds it. The destroy effect
   * (on_destroy () of the window manager) reaches here too, but only for a
   * window the compositor was showing: one closed on another workspace has
   * no effect, and the workspace removal that follows must not be reported
   * after window.unmapped. */
  shell_window_hooks_window_unmapped (self, window);
}

static void
window_unmanaged_cb (MetaWindow       *window,
                     ShellWindowHooks *self)
{
  /* The handles of extensions die with the window */
  shell_lua_handle_invalidate (NULL, G_OBJECT (window));
}

static WindowInfo *
track_window (ShellWindowHooks *self,
              MetaWindow       *window)
{
  WindowInfo *info = window_info_get (window);

  if (info != NULL)
    return info;

  info = g_new0 (WindowInfo, 1);
  window_refresh_info (info, window);
  g_object_set_qdata_full (G_OBJECT (window), window_info_quark (), info, window_info_free);

  g_signal_connect_object (window, "notify::title",
                           G_CALLBACK (window_title_cb), self, 0);
  g_signal_connect_object (window, "notify::maximized-horizontally",
                           G_CALLBACK (window_state_cb), self, 0);
  g_signal_connect_object (window, "notify::maximized-vertically",
                           G_CALLBACK (window_state_cb), self, 0);
  g_signal_connect_object (window, "notify::fullscreen",
                           G_CALLBACK (window_state_cb), self, 0);
  g_signal_connect_object (window, "notify::above",
                           G_CALLBACK (window_state_cb), self, 0);
  g_signal_connect_object (window, "notify::on-all-workspaces",
                           G_CALLBACK (window_state_cb), self, 0);
  g_signal_connect_object (window, "position-changed",
                           G_CALLBACK (window_geometry_cb), self, 0);
  g_signal_connect_object (window, "size-changed",
                           G_CALLBACK (window_geometry_cb), self, 0);
  g_signal_connect_object (window, "workspace-changed",
                           G_CALLBACK (window_workspace_cb), self, 0);
  g_signal_connect_object (window, "unmanaging",
                           G_CALLBACK (window_unmanaging_cb), self, 0);
  g_signal_connect_object (window, "unmanaged",
                           G_CALLBACK (window_unmanaged_cb), self, 0);
  return info;
}

/* ---- the display ------------------------------------------------------- */

static void
display_window_created_cb (MetaDisplay      *display,
                           MetaWindow       *window,
                           ShellWindowHooks *self)
{
  track_window (self, window);
}

static void
display_window_entered_monitor_cb (MetaDisplay      *display,
                                   int               monitor,
                                   MetaWindow       *window,
                                   ShellWindowHooks *self)
{
  WindowInfo *info = track_window (self, window);
  int old = info->monitor;

  info->monitor = monitor + 1;
  if (old == info->monitor)
    return;

  if (window_ready (self, window, info))
    {
      GVariantBuilder builder;

      window_payload_init (&builder, window);
      if (old > 0)
        g_variant_builder_add (&builder, "{sv}", "from", g_variant_new_int32 (old));
      g_variant_builder_add (&builder, "{sv}", "to", g_variant_new_int32 (info->monitor));
      emit_hook ("window.monitor-changed", &builder);
    }
}

/* The focus moved to a listed window the shell has seen map: window.focused */
static void
update_focus (ShellWindowHooks *self)
{
  MetaWindow *window;
  WindowInfo *info;
  guint64 previous = self->focused_id;

  if (self->display == NULL)
    return;

  window = shell_window_hooks_get_focused_window (self->display);
  if (window == NULL)
    {
      self->focused_id = 0;
      return;
    }

  info = track_window (self, window);
  if (!info->mapped)
    return;                /* window.mapped will pick the focus up */
  if (meta_window_get_id (window) == previous)
    return;

  self->focused_id = meta_window_get_id (window);
  if (window_ready (self, window, info))
    {
      GVariantBuilder builder;

      window_payload_init (&builder, window);
      if (previous != 0)
        g_variant_builder_add (&builder, "{sv}", "previous-window-id", g_variant_new_uint64 (previous));
      emit_hook ("window.focused", &builder);
    }
}

static void
display_focus_window_cb (MetaDisplay      *display,
                         GParamSpec       *pspec,
                         ShellWindowHooks *self)
{
  update_focus (self);
}

/* ---- the workspaces ---------------------------------------------------- */

/* A removed or reordered workspace shifts the numbers of the others: the
 * workspace a window is on is the same one, so no window.workspace-changed
 * follows, but the number we compare the next change against must. */
static void
refresh_window_workspaces (ShellWindowHooks *self)
{
  g_autoptr (GList) windows = NULL;
  GList *l;

  if (self->display == NULL)
    return;

  windows = meta_display_list_all_windows (self->display);
  for (l = windows; l != NULL; l = l->next)
    {
      WindowInfo *info = window_info_get (l->data);

      if (info != NULL)
        info->workspace = shell_window_hooks_get_workspace_number (l->data);
    }
}

/* Kills the handles of the workspaces that are gone and remembers the rest */
static void
sync_workspaces (ShellWindowHooks *self)
{
  GPtrArray *current = g_ptr_array_new_with_free_func (g_object_unref);
  int n, i;
  guint j;

  if (self->workspace_manager == NULL)
    {
      g_ptr_array_unref (current);
      return;
    }

  n = meta_workspace_manager_get_n_workspaces (self->workspace_manager);
  for (i = 0; i < n; i++)
    g_ptr_array_add (current,
                     g_object_ref (meta_workspace_manager_get_workspace_by_index (self->workspace_manager, i)));

  for (j = 0; j < self->workspaces->len; j++)
    {
      GObject *old = g_ptr_array_index (self->workspaces, j);
      guint k;
      gboolean still_there = FALSE;

      for (k = 0; k < current->len && !still_there; k++)
        still_there = g_ptr_array_index (current, k) == old;
      if (!still_there)
        shell_lua_handle_invalidate (NULL, old);
    }

  g_ptr_array_unref (self->workspaces);
  self->workspaces = current;

  refresh_window_workspaces (self);
}

static void
emit_workspace_index (const char *name,
                      int         index)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "index", g_variant_new_int32 (index + 1));
  emit_hook (name, &builder);
}

static void
workspace_added_cb (MetaWorkspaceManager *manager,
                    int                   index,
                    ShellWindowHooks     *self)
{
  sync_workspaces (self);
  emit_workspace_index ("workspace.added", index);
}

static void
workspace_removed_cb (MetaWorkspaceManager *manager,
                      int                   index,
                      ShellWindowHooks     *self)
{
  sync_workspaces (self);
  emit_workspace_index ("workspace.removed", index);
}

static void
workspaces_reordered_cb (MetaWorkspaceManager *manager,
                         ShellWindowHooks     *self)
{
  sync_workspaces (self);
}

/* ---- events of ShellWindowManager --------------------------------------- */

void
shell_window_hooks_window_mapped (ShellWindowHooks *self,
                                  MetaWindow       *window)
{
  WindowInfo *info;

  g_return_if_fail (SHELL_IS_WINDOW_HOOKS (self));
  g_return_if_fail (META_IS_WINDOW (window));

  info = track_window (self, window);
  if (!info->mapped)
    {
      /* What changed while the window was being set up is part of the
       * window.mapped payload */
      window_refresh_info (info, window);
      info->mapped = TRUE;
    }
  window_ready (self, window, info);

  /* A window that got the focus before the shell saw it map */
  if (self->display != NULL && meta_display_get_focus_window (self->display) == window)
    update_focus (self);
}

void
shell_window_hooks_window_unmapped (ShellWindowHooks *self,
                                    MetaWindow       *window)
{
  WindowInfo *info;

  g_return_if_fail (SHELL_IS_WINDOW_HOOKS (self));
  g_return_if_fail (META_IS_WINDOW (window));

  info = window_info_get (window);
  if (info == NULL)
    return;

  if (info->announced)
    {
      GVariantBuilder builder;

      info->announced = FALSE;
      if (self->focused_id == meta_window_get_id (window))
        self->focused_id = 0;
      window_payload_init (&builder, window);
      emit_hook ("window.unmapped", &builder);
    }
  info->mapped = FALSE;
  g_clear_handle_id (&info->geometry_idle, g_source_remove);
}

void
shell_window_hooks_window_minimized (ShellWindowHooks *self,
                                     MetaWindow       *window,
                                     gboolean          minimized)
{
  WindowInfo *info;

  g_return_if_fail (SHELL_IS_WINDOW_HOOKS (self));
  g_return_if_fail (META_IS_WINDOW (window));

  info = track_window (self, window);
  if (window_ready (self, window, info))
    {
      GVariantBuilder builder;

      window_payload_init (&builder, window);
      emit_hook (minimized ? "window.minimized" : "window.unminimized", &builder);
    }
}

void
shell_window_hooks_workspace_switched (ShellWindowHooks *self,
                                       int               from,
                                       int               to)
{
  GVariantBuilder builder;

  g_return_if_fail (SHELL_IS_WINDOW_HOOKS (self));

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "from", g_variant_new_int32 (from + 1));
  g_variant_builder_add (&builder, "{sv}", "to", g_variant_new_int32 (to + 1));
  emit_hook ("workspace.switched", &builder);
}

/* ---- the replay of window.mapped ---------------------------------------- */

static GPtrArray *
replay_window_mapped (ShellHookRegistry *registry,
                      const char        *name,
                      gpointer           user_data)
{
  GPtrArray *payloads = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
  g_autoptr (GList) windows = NULL;
  GList *l;

  if (current_hooks == NULL || current_hooks->display == NULL)
    return payloads;

  /* The windows window.mapped was emitted for, in the order of windows.list () */
  windows = shell_window_hooks_list_windows (current_hooks->display);
  for (l = windows; l != NULL; l = l->next)
    {
      WindowInfo *info = window_info_get (l->data);
      GVariantBuilder builder;

      if (info == NULL || !info->announced)
        continue;

      window_payload_init (&builder, l->data);
      g_ptr_array_add (payloads, g_variant_ref_sink (g_variant_builder_end (&builder)));
    }
  return payloads;
}

/* ---- life cycle ---------------------------------------------------------- */

static void
shell_window_hooks_dispose (GObject *object)
{
  ShellWindowHooks *self = SHELL_WINDOW_HOOKS (object);

  if (current_hooks == self)
    g_clear_weak_pointer (&current_hooks);
  g_clear_weak_pointer (&self->display);
  g_clear_weak_pointer (&self->workspace_manager);
  g_clear_pointer (&self->workspaces, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_window_hooks_parent_class)->dispose (object);
}

static void
shell_window_hooks_class_init (ShellWindowHooksClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = shell_window_hooks_dispose;
}

static void
shell_window_hooks_init (ShellWindowHooks *self)
{
  self->workspaces = g_ptr_array_new_with_free_func (g_object_unref);
}

/**
 * shell_window_hooks_new: (skip)
 * @display: the display
 * @workspace_manager: its workspace manager
 *
 * Starts emitting the window.* and workspace.* hooks for the windows of
 * @display. The windows that exist already count as announced.
 *
 * Returns: (transfer full): a new #ShellWindowHooks
 */
ShellWindowHooks *
shell_window_hooks_new (MetaDisplay          *display,
                        MetaWorkspaceManager *workspace_manager)
{
  ShellWindowHooks *self = g_object_new (SHELL_TYPE_WINDOW_HOOKS, NULL);
  g_autoptr (GList) windows = meta_display_list_all_windows (display);
  MetaWindow *focus;
  GList *l;

  g_set_weak_pointer (&self->display, display);
  g_set_weak_pointer (&self->workspace_manager, workspace_manager);
  g_clear_weak_pointer (&current_hooks);
  g_set_weak_pointer (&current_hooks, self);

  shell_window_hooks_declare ();

  for (l = windows; l != NULL; l = l->next)
    {
      MetaWindow *window = l->data;
      WindowInfo *info = track_window (self, window);

      if (meta_window_get_compositor_private (window) != NULL)
        {
          info->mapped = TRUE;
          info->announced = shell_window_hooks_is_listed (window);
        }
    }
  focus = shell_window_hooks_get_focused_window (display);
  if (focus != NULL && window_info_get (focus)->announced)
    self->focused_id = meta_window_get_id (focus);

  sync_workspaces (self);

  g_signal_connect_object (display, "window-created",
                           G_CALLBACK (display_window_created_cb), self, 0);
  g_signal_connect_object (display, "window-entered-monitor",
                           G_CALLBACK (display_window_entered_monitor_cb), self, 0);
  g_signal_connect_object (display, "notify::focus-window",
                           G_CALLBACK (display_focus_window_cb), self, 0);
  g_signal_connect_object (workspace_manager, "workspace-added",
                           G_CALLBACK (workspace_added_cb), self, 0);
  g_signal_connect_object (workspace_manager, "workspace-removed",
                           G_CALLBACK (workspace_removed_cb), self, 0);
  g_signal_connect_object (workspace_manager, "workspaces-reordered",
                           G_CALLBACK (workspaces_reordered_cb), self, 0);
  return self;
}
