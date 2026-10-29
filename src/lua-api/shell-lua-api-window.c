/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.windows, shell.workspaces, shell.monitors and shell.apps
 * (docs/api/window.md).
 *
 * Windows and workspaces are handles to the MetaWindow and MetaWorkspace
 * themselves, with virtual properties that compute the documented values
 * (1-based numbers, nil for "all workspaces"). They are not owned by the
 * extension: the handle dies when the window is unmanaged or the workspace
 * removed (src/lua-api/shell-window-hooks.c kills them). Monitors are plain
 * snapshot tables. The numbers and the filter ("not skip-taskbar") are the
 * ones of the window.* hooks, whose emitter lives next to this file.
 *
 * An action asked for from inside a window.* or workspace.* hook is deferred
 * to idle (README rule 8, "a listener that closes the window it is told
 * about"); the others run at once.
 */

#include "config.h"

#include <math.h>
#include <string.h>

#include <meta/display.h>
#include <meta/meta-workspace-manager.h>
#include <meta/window.h>
#include <meta/workspace.h>

#include "lua-api/shell-window-hooks.h"
#include "shell-app.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "shell-lua-api-private.h"
#include "ui/app-favorites.h"
#include "ui/layout.h"
#include "ui/main.h"

static void shell_lua_api_window_register (ShellLuaState *state,
                                           lua_State     *L);

static void __attribute__ ((constructor))
shell_lua_api_window_init (void)
{
  shell_lua_api_register ("window", shell_lua_api_window_register);
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static MetaDisplay *
get_display (lua_State *L)
{
  ShellGlobal *global = shell_global_get ();
  MetaDisplay *display = global != NULL ? shell_global_get_display (global) : NULL;

  if (display == NULL)
    luaL_error (L, "the window manager is not available");
  return display;
}

static MetaWorkspaceManager *
get_workspace_manager (lua_State *L)
{
  return meta_display_get_workspace_manager (get_display (L));
}

static ShellLayoutManager *
get_layout_manager (lua_State *L)
{
  ShellMain *main = shell_main_get_default ();
  ShellLayoutManager *layout = main != NULL ? shell_main_get_layout_manager (main) : NULL;

  if (layout == NULL)
    luaL_error (L, "the monitors are not available");
  return layout;
}

static void
push_window (lua_State *L,
             MetaWindow *window)
{
  shell_lua_handle_push (L, "shell.Window", G_OBJECT (window));
}

static void
push_windows (lua_State *L,
              GList      *windows)
{
  GList *l;
  int i = 1;

  lua_newtable (L);
  for (l = windows; l != NULL; l = l->next)
    {
      push_window (L, l->data);
      lua_rawseti (L, -2, i++);
    }
}

/* ------------------------------------------------------------------------ */
/* Actions                                                                  */
/* ------------------------------------------------------------------------ */

typedef enum
{
  ACTION_ACTIVATE,
  ACTION_MINIMIZE,
  ACTION_CLOSE,
  ACTION_MOVE_TO_WORKSPACE,
  ACTION_MOVE_TO_MONITOR,
  ACTION_ACTIVATE_WORKSPACE,
  /* 1.1 */
  ACTION_MOVE,
  ACTION_RESIZE,
  ACTION_MOVE_RESIZE,
  ACTION_MAXIMIZE,
  ACTION_UNMAXIMIZE,
  ACTION_SET_FULLSCREEN,
  ACTION_SET_ABOVE,
  ACTION_SET_ON_ALL_WORKSPACES,
  ACTION_UNMINIMIZE,
} ActionKind;

/* x, y, width, height of the rectangle of a geometry action */
typedef struct
{
  int x;
  int y;
  int width;
  int height;
} ActionRect;

typedef struct
{
  GWeakRef   object;
  ActionKind kind;
  int        arg;      /* 0-based workspace or monitor; the boolean of set_* */
  ActionRect rect;
} Action;

/* The 1.1 actions call Mutter functions that g_return_if_fail () on an
 * override-redirect window, and on one that is being unmanaged (an action
 * that was deferred to idle can find the window gone: the handle died with
 * it, but the weak reference to the object holds until it is finalized). That
 * is a critical, so check first: Mutter "may ignore" the actions anyway. */
static gboolean
window_accepts_actions (MetaWindow *window)
{
  MetaDisplay *display = meta_window_get_display (window);
  g_autoptr (GList) all = NULL;

  if (display == NULL || meta_window_is_override_redirect (window))
    return FALSE;

  all = meta_display_list_all_windows (display);
  return g_list_find (all, window) != NULL;
}

static void
perform_action (GObject          *object,
                ActionKind        kind,
                int               arg,
                const ActionRect *rect)
{
  ShellGlobal *global = shell_global_get ();
  ShellMain *main = shell_main_get_default ();

  if (global == NULL)
    return;

  switch (kind)
    {
    case ACTION_ACTIVATE:
      if (main != NULL)
        shell_main_activate_window (main, META_WINDOW (object), 0, -1);
      break;

    case ACTION_MINIMIZE:
      {
        gboolean minimized = FALSE;

        g_object_get (object, "minimized", &minimized, NULL);
        if (!minimized)
          meta_window_minimize (META_WINDOW (object));
      }
      break;

    case ACTION_CLOSE:
      {
        /* An extension closes a window outside of any event (a timer, a
         * D-Bus reply): without a timestamp the liveness ping is refused */
        guint32 timestamp = shell_global_get_current_time (global);

        if (timestamp == 0)
          timestamp = meta_display_get_current_time_roundtrip (shell_global_get_display (global));
        meta_window_delete (META_WINDOW (object), timestamp);
      }
      break;

    case ACTION_MOVE_TO_WORKSPACE:
      meta_window_change_workspace_by_index (META_WINDOW (object), arg, FALSE);
      break;

    case ACTION_MOVE_TO_MONITOR:
      {
        MetaWindow *window = META_WINDOW (object);
        MetaWorkspace *workspace = meta_window_get_workspace (window);

        if (main != NULL && workspace != NULL && !meta_window_is_on_all_workspaces (window))
          shell_main_move_window_to_monitor_and_workspace (main, window, arg,
                                                           meta_workspace_index (workspace), FALSE);
        else
          meta_window_move_to_monitor (window, arg);
      }
      break;

    case ACTION_ACTIVATE_WORKSPACE:
      meta_workspace_activate (META_WORKSPACE (object), shell_global_get_current_time (global));
      break;

    case ACTION_MOVE:
      if (window_accepts_actions (META_WINDOW (object)))
        meta_window_move_frame (META_WINDOW (object), FALSE, rect->x, rect->y);
      break;

    case ACTION_RESIZE:
      if (window_accepts_actions (META_WINDOW (object)))
        {
          MtkRectangle frame;

          meta_window_get_frame_rect (META_WINDOW (object), &frame);
          meta_window_move_resize_frame (META_WINDOW (object), FALSE, frame.x, frame.y,
                                         rect->width, rect->height);
        }
      break;

    case ACTION_MOVE_RESIZE:
      if (window_accepts_actions (META_WINDOW (object)))
        meta_window_move_resize_frame (META_WINDOW (object), FALSE, rect->x, rect->y,
                                       rect->width, rect->height);
      break;

    case ACTION_MAXIMIZE:
      {
        MetaWindow *window = META_WINDOW (object);

        if (window_accepts_actions (window) && meta_window_can_maximize (window) &&
            !meta_window_is_maximized (window))
          meta_window_maximize (window);
      }
      break;

    case ACTION_UNMAXIMIZE:
      {
        MetaWindow *window = META_WINDOW (object);
        gboolean horizontal = FALSE, vertical = FALSE;

        /* also a window maximized on one axis only (by its client) */
        g_object_get (object, "maximized-horizontally", &horizontal,
                      "maximized-vertically", &vertical, NULL);
        if (window_accepts_actions (window) && (horizontal || vertical))
          meta_window_unmaximize (window);
      }
      break;

    case ACTION_SET_FULLSCREEN:
      if (window_accepts_actions (META_WINDOW (object)) &&
          arg != meta_window_is_fullscreen (META_WINDOW (object)))
        {
          if (arg)
            meta_window_make_fullscreen (META_WINDOW (object));
          else
            meta_window_unmake_fullscreen (META_WINDOW (object));
        }
      break;

    case ACTION_SET_ABOVE:
      if (window_accepts_actions (META_WINDOW (object)) &&
          arg != meta_window_is_above (META_WINDOW (object)))
        {
          if (arg)
            meta_window_make_above (META_WINDOW (object));
          else
            meta_window_unmake_above (META_WINDOW (object));
        }
      break;

    case ACTION_SET_ON_ALL_WORKSPACES:
      if (window_accepts_actions (META_WINDOW (object)) &&
          arg != meta_window_is_on_all_workspaces (META_WINDOW (object)))
        {
          if (arg)
            meta_window_stick (META_WINDOW (object));
          else
            meta_window_unstick (META_WINDOW (object));
        }
      break;

    case ACTION_UNMINIMIZE:
      {
        gboolean minimized = FALSE;

        g_object_get (object, "minimized", &minimized, NULL);
        if (minimized && window_accepts_actions (META_WINDOW (object)))
          meta_window_unminimize (META_WINDOW (object));
      }
      break;

    default:
      g_assert_not_reached ();
    }
}

static gboolean
action_idle_cb (gpointer data)
{
  Action *action = data;
  g_autoptr (GObject) object = g_weak_ref_get (&action->object);

  if (object != NULL)
    perform_action (object, action->kind, action->arg, &action->rect);
  return G_SOURCE_REMOVE;
}

static void
action_free (gpointer data)
{
  Action *action = data;

  g_weak_ref_clear (&action->object);
  g_free (action);
}

/* "Takes effect on the next frame": run now, unless a hook is being emitted */
static void
run_action_rect (GObject          *object,
                 ActionKind        kind,
                 int               arg,
                 const ActionRect *rect)
{
  static const ActionRect no_rect = { 0 };

  if (rect == NULL)
    rect = &no_rect;

  if (shell_window_hooks_is_emitting ())
    {
      Action *action = g_new0 (Action, 1);

      g_weak_ref_init (&action->object, object);
      action->kind = kind;
      action->arg = arg;
      action->rect = *rect;
      g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, action_idle_cb, action, action_free);
      return;
    }

  perform_action (object, kind, arg, rect);
}

static void
run_action (GObject   *object,
            ActionKind kind,
            int        arg)
{
  run_action_rect (object, kind, arg, NULL);
}

/* ------------------------------------------------------------------------ */
/* shell.Window                                                             */
/* ------------------------------------------------------------------------ */

static void
window_get_id (lua_State *L, GObject *object)
{
  lua_pushinteger (L, (lua_Integer) meta_window_get_id (META_WINDOW (object)));
}

static void
window_get_title (lua_State *L, GObject *object)
{
  const char *title = meta_window_get_title (META_WINDOW (object));

  lua_pushstring (L, title != NULL ? title : "");
}

static void
window_get_wm_class (lua_State *L, GObject *object)
{
  const char *wm_class = meta_window_get_wm_class (META_WINDOW (object));

  lua_pushstring (L, wm_class != NULL ? wm_class : "");
}

static void
window_get_app_id (lua_State *L, GObject *object)
{
  g_autofree char *app_id = shell_window_hooks_dup_app_id (META_WINDOW (object));

  if (app_id != NULL)
    lua_pushstring (L, app_id);
  else
    lua_pushnil (L);
}

static void
window_get_workspace (lua_State *L, GObject *object)
{
  int workspace = shell_window_hooks_get_workspace_number (META_WINDOW (object));

  if (workspace > 0)
    lua_pushinteger (L, workspace);
  else
    lua_pushnil (L);
}

static void
window_get_monitor (lua_State *L, GObject *object)
{
  lua_pushinteger (L, shell_window_hooks_get_monitor_number (META_WINDOW (object)));
}

static void
window_get_minimized (lua_State *L, GObject *object)
{
  gboolean minimized = FALSE;

  g_object_get (object, "minimized", &minimized, NULL);
  lua_pushboolean (L, minimized);
}

static void
window_get_maximized (lua_State *L, GObject *object)
{
  lua_pushboolean (L, meta_window_is_maximized (META_WINDOW (object)));
}

static void
window_get_fullscreen (lua_State *L, GObject *object)
{
  lua_pushboolean (L, meta_window_is_fullscreen (META_WINDOW (object)));
}

static void
window_get_focused (lua_State *L, GObject *object)
{
  lua_pushboolean (L, meta_window_has_focus (META_WINDOW (object)));
}

static void
window_get_pid (lua_State *L, GObject *object)
{
  pid_t pid = meta_window_get_pid (META_WINDOW (object));

  if (pid > 0)
    lua_pushinteger (L, pid);
  else
    lua_pushnil (L);
}

/* 1.1 */

static void
window_get_frame (lua_State *L, GObject *object)
{
  MtkRectangle frame;

  meta_window_get_frame_rect (META_WINDOW (object), &frame);
  lua_createtable (L, 0, 4);
  lua_pushinteger (L, frame.x);
  lua_setfield (L, -2, "x");
  lua_pushinteger (L, frame.y);
  lua_setfield (L, -2, "y");
  lua_pushinteger (L, frame.width);
  lua_setfield (L, -2, "width");
  lua_pushinteger (L, frame.height);
  lua_setfield (L, -2, "height");
}

static void
window_get_above (lua_State *L, GObject *object)
{
  lua_pushboolean (L, meta_window_is_above (META_WINDOW (object)));
}

static void
window_get_on_all_workspaces (lua_State *L, GObject *object)
{
  lua_pushboolean (L, meta_window_is_on_all_workspaces (META_WINDOW (object)));
}

static void
window_get_resizable (lua_State *L, GObject *object)
{
  lua_pushboolean (L, meta_window_allows_resize (META_WINDOW (object)));
}

static void
window_get_movable (lua_State *L, GObject *object)
{
  lua_pushboolean (L, meta_window_allows_move (META_WINDOW (object)));
}

static const ShellLuaHandleVirtual window_virtuals[] = {
  { "id",         window_get_id,         NULL },
  { "title",      window_get_title,      NULL },
  { "wm_class",   window_get_wm_class,   NULL },
  { "app_id",     window_get_app_id,     NULL },
  { "workspace",  window_get_workspace,  NULL },
  { "monitor",    window_get_monitor,    NULL },
  { "minimized",  window_get_minimized,  NULL },
  { "maximized",  window_get_maximized,  NULL },
  { "fullscreen", window_get_fullscreen, NULL },
  { "focused",    window_get_focused,    NULL },
  { "pid",        window_get_pid,        NULL },
  { "frame",             window_get_frame,             NULL },
  { "above",             window_get_above,             NULL },
  { "on_all_workspaces", window_get_on_all_workspaces, NULL },
  { "resizable",         window_get_resizable,         NULL },
  { "movable",           window_get_movable,           NULL },
  { NULL }
};

static GObject *
check_window (lua_State *L)
{
  return shell_lua_handle_check (L, 1, "shell.Window");
}

static int
l_window_activate (lua_State *L)
{
  run_action (check_window (L), ACTION_ACTIVATE, 0);
  return 0;
}

static int
l_window_minimize (lua_State *L)
{
  run_action (check_window (L), ACTION_MINIMIZE, 0);
  return 0;
}

static int
l_window_close (lua_State *L)
{
  run_action (check_window (L), ACTION_CLOSE, 0);
  return 0;
}

static int
l_window_move_to_workspace (lua_State *L)
{
  GObject *window = check_window (L);
  lua_Integer n = luaL_checkinteger (L, 2);
  int count = meta_workspace_manager_get_n_workspaces (get_workspace_manager (L));

  if (n < 1 || n > count)
    return luaL_error (L, "workspace %I is out of range (1..%d)", n, count);

  run_action (window, ACTION_MOVE_TO_WORKSPACE, (int) n - 1);
  return 0;
}

static int
l_window_move_to_monitor (lua_State *L)
{
  GObject *window = check_window (L);
  lua_Integer n = luaL_checkinteger (L, 2);
  int count = (int) shell_layout_manager_get_n_monitors (get_layout_manager (L));

  if (n < 1 || n > count)
    return luaL_error (L, "monitor %I is out of range (1..%d)", n, count);

  run_action (window, ACTION_MOVE_TO_MONITOR, (int) n - 1);
  return 0;
}

/* ---- 1.1: geometry and state ------------------------------------------- */

/* Coordinates and sizes stay far from the overflows of the constraint code of
 * Mutter */
#define COORDINATE_MAX 65536

/* Field @name of the options table at @tbl: a required integer in
 * [@min, @max] */
static int
check_rect_field (lua_State  *L,
                  int         tbl,
                  const char *name,
                  lua_Integer min)
{
  lua_Integer value;
  int isint;

  if (lua_getfield (L, tbl, name) == LUA_TNIL)
    luaL_error (L, "missing required field '%s' (an integer)", name);
  if (lua_type (L, -1) != LUA_TNUMBER)
    luaL_error (L, "field '%s' must be an integer, got %s", name, luaL_typename (L, -1));
  value = lua_tointegerx (L, -1, &isint);
  if (!isint)
    luaL_error (L, "field '%s' must be an integer (number has no integer representation)", name);
  if (value < min || value > COORDINATE_MAX)
    luaL_error (L, "field '%s' must be between %I and %I", name, min, (lua_Integer) COORDINATE_MAX);
  lua_pop (L, 1);
  return (int) value;
}

static int
window_rect_action (lua_State  *L,
                    ActionKind  kind,
                    gboolean    position,
                    gboolean    size)
{
  static const char * const move_fields[] = { "x", "y", NULL };
  static const char * const resize_fields[] = { "width", "height", NULL };
  static const char * const move_resize_fields[] = { "x", "y", "width", "height", NULL };
  GObject *window = check_window (L);
  ActionRect rect = { 0 };

  if (lua_gettop (L) != 2)
    luaL_error (L, "expected one options table");
  shell_lua_check_fields (L, 2, position && size ? move_resize_fields :
                                position ? move_fields : resize_fields);
  if (position)
    {
      rect.x = check_rect_field (L, 2, "x", -COORDINATE_MAX);
      rect.y = check_rect_field (L, 2, "y", -COORDINATE_MAX);
    }
  if (size)
    {
      rect.width = check_rect_field (L, 2, "width", 1);
      rect.height = check_rect_field (L, 2, "height", 1);
    }

  run_action_rect (window, kind, 0, &rect);
  return 0;
}

static int
l_window_move (lua_State *L)
{
  return window_rect_action (L, ACTION_MOVE, TRUE, FALSE);
}

static int
l_window_resize (lua_State *L)
{
  return window_rect_action (L, ACTION_RESIZE, FALSE, TRUE);
}

static int
l_window_move_resize (lua_State *L)
{
  return window_rect_action (L, ACTION_MOVE_RESIZE, TRUE, TRUE);
}

static int
l_window_maximize (lua_State *L)
{
  run_action (check_window (L), ACTION_MAXIMIZE, 0);
  return 0;
}

static int
l_window_unmaximize (lua_State *L)
{
  run_action (check_window (L), ACTION_UNMAXIMIZE, 0);
  return 0;
}

static int
l_window_unminimize (lua_State *L)
{
  run_action (check_window (L), ACTION_UNMINIMIZE, 0);
  return 0;
}

static int
window_boolean_action (lua_State *L,
                       ActionKind kind)
{
  GObject *window = check_window (L);

  luaL_checktype (L, 2, LUA_TBOOLEAN);
  run_action (window, kind, lua_toboolean (L, 2));
  return 0;
}

static int
l_window_set_fullscreen (lua_State *L)
{
  return window_boolean_action (L, ACTION_SET_FULLSCREEN);
}

static int
l_window_set_above (lua_State *L)
{
  return window_boolean_action (L, ACTION_SET_ABOVE);
}

static int
l_window_set_on_all_workspaces (lua_State *L)
{
  return window_boolean_action (L, ACTION_SET_ON_ALL_WORKSPACES);
}

static const luaL_Reg window_methods[] = {
  { "activate",          l_window_activate },
  { "minimize",          l_window_minimize },
  { "close",             l_window_close },
  { "move_to_workspace", l_window_move_to_workspace },
  { "move_to_monitor",   l_window_move_to_monitor },
  { "move",                 l_window_move },
  { "resize",               l_window_resize },
  { "move_resize",          l_window_move_resize },
  { "maximize",             l_window_maximize },
  { "unmaximize",           l_window_unmaximize },
  { "set_fullscreen",       l_window_set_fullscreen },
  { "set_above",            l_window_set_above },
  { "set_on_all_workspaces", l_window_set_on_all_workspaces },
  { "unminimize",           l_window_unminimize },
  { NULL, NULL }
};

static const ShellLuaHandleProperty window_properties[] = {
  { NULL }
};

static ShellLuaHandleType window_type = {
  .name = "shell.Window",
  .methods = window_methods,
  .properties = window_properties,
  .virtuals = window_virtuals,
};

/* ------------------------------------------------------------------------ */
/* shell.windows                                                            */
/* ------------------------------------------------------------------------ */

static int
l_windows_list (lua_State *L)
{
  g_autoptr (GList) windows = shell_window_hooks_list_windows (get_display (L));

  push_windows (L, windows);
  return 1;
}

static int
l_windows_focused (lua_State *L)
{
  MetaWindow *window = shell_window_hooks_get_focused_window (get_display (L));

  if (window != NULL)
    push_window (L, window);
  else
    lua_pushnil (L);
  return 1;
}

static int
l_windows_get (lua_State *L)
{
  lua_Integer id = luaL_checkinteger (L, 1);
  MetaWindow *window = id >= 0 ? shell_window_hooks_lookup_window (get_display (L), (guint64) id) : NULL;

  if (window != NULL)
    push_window (L, window);
  else
    lua_pushnil (L);
  return 1;
}

static const luaL_Reg windows_funcs[] = {
  { "list",    l_windows_list },
  { "focused", l_windows_focused },
  { "get",     l_windows_get },
  { NULL, NULL }
};

/* ------------------------------------------------------------------------ */
/* shell.Workspace, shell.workspaces                                        */
/* ------------------------------------------------------------------------ */

static void
workspace_get_index (lua_State *L, GObject *object)
{
  lua_pushinteger (L, meta_workspace_index (META_WORKSPACE (object)) + 1);
}

static void
workspace_get_active (lua_State *L, GObject *object)
{
  MetaWorkspaceManager *manager = get_workspace_manager (L);

  lua_pushboolean (L, meta_workspace_manager_get_active_workspace (manager) == META_WORKSPACE (object));
}

/* The listed windows of @workspace, in the order of shell.windows.list () */
static GList *
workspace_windows (lua_State     *L,
                   MetaWorkspace *workspace)
{
  GList *all = shell_window_hooks_list_windows (get_display (L));
  GList *result = NULL, *l;

  for (l = all; l != NULL; l = l->next)
    {
      if (meta_window_located_on_workspace (l->data, workspace))
        result = g_list_prepend (result, l->data);
    }
  g_list_free (all);
  return g_list_reverse (result);
}

static void
workspace_get_n_windows (lua_State *L, GObject *object)
{
  g_autoptr (GList) windows = workspace_windows (L, META_WORKSPACE (object));

  lua_pushinteger (L, g_list_length (windows));
}

static const ShellLuaHandleVirtual workspace_virtuals[] = {
  { "index",     workspace_get_index,     NULL },
  { "active",    workspace_get_active,    NULL },
  { "n_windows", workspace_get_n_windows, NULL },
  { NULL }
};

static GObject *
check_workspace (lua_State *L)
{
  return shell_lua_handle_check (L, 1, "shell.Workspace");
}

static int
l_workspace_windows (lua_State *L)
{
  GObject *workspace = check_workspace (L);
  g_autoptr (GList) windows = workspace_windows (L, META_WORKSPACE (workspace));

  push_windows (L, windows);
  return 1;
}

static int
l_workspace_activate (lua_State *L)
{
  run_action (check_workspace (L), ACTION_ACTIVATE_WORKSPACE, 0);
  return 0;
}

static const luaL_Reg workspace_methods[] = {
  { "windows",  l_workspace_windows },
  { "activate", l_workspace_activate },
  { NULL, NULL }
};

static const ShellLuaHandleProperty workspace_properties[] = {
  { NULL }
};

static ShellLuaHandleType workspace_type = {
  .name = "shell.Workspace",
  .methods = workspace_methods,
  .properties = workspace_properties,
  .virtuals = workspace_virtuals,
};

static void
push_workspace (lua_State     *L,
                MetaWorkspace *workspace)
{
  shell_lua_handle_push (L, "shell.Workspace", G_OBJECT (workspace));
}

static int
l_workspaces_list (lua_State *L)
{
  MetaWorkspaceManager *manager = get_workspace_manager (L);
  int i, n = meta_workspace_manager_get_n_workspaces (manager);

  lua_createtable (L, n, 0);
  for (i = 0; i < n; i++)
    {
      push_workspace (L, meta_workspace_manager_get_workspace_by_index (manager, i));
      lua_rawseti (L, -2, i + 1);
    }
  return 1;
}

static int
l_workspaces_count (lua_State *L)
{
  lua_pushinteger (L, meta_workspace_manager_get_n_workspaces (get_workspace_manager (L)));
  return 1;
}

static int
l_workspaces_active (lua_State *L)
{
  push_workspace (L, meta_workspace_manager_get_active_workspace (get_workspace_manager (L)));
  return 1;
}

static int
l_workspaces_get (lua_State *L)
{
  MetaWorkspaceManager *manager = get_workspace_manager (L);
  lua_Integer n = luaL_checkinteger (L, 1);

  if (n >= 1 && n <= meta_workspace_manager_get_n_workspaces (manager))
    push_workspace (L, meta_workspace_manager_get_workspace_by_index (manager, (int) n - 1));
  else
    lua_pushnil (L);
  return 1;
}

static const luaL_Reg workspaces_funcs[] = {
  { "list",   l_workspaces_list },
  { "count",  l_workspaces_count },
  { "active", l_workspaces_active },
  { "get",    l_workspaces_get },
  { NULL, NULL }
};

/* ------------------------------------------------------------------------ */
/* shell.monitors                                                           */
/* ------------------------------------------------------------------------ */

static void
set_integer_field (lua_State  *L,
                   const char *name,
                   lua_Integer value)
{
  lua_pushinteger (L, value);
  lua_setfield (L, -2, name);
}

/* Pushes the snapshot table of the monitor with 0-based @index, or nil */
static void
push_monitor (lua_State          *L,
              ShellLayoutManager *layout,
              int                 index)
{
  ShellMonitor *monitor = shell_layout_manager_get_monitor (layout, index);
  MtkRectangle work_area = { 0 };
  double scale;

  if (monitor == NULL)
    {
      lua_pushnil (L);
      return;
    }

  shell_layout_manager_get_work_area_for_monitor (layout, monitor->index, &work_area);
  scale = monitor->geometry_scale;

  lua_createtable (L, 0, 8);
  set_integer_field (L, "index", monitor->index + 1);
  set_integer_field (L, "x", monitor->x);
  set_integer_field (L, "y", monitor->y);
  set_integer_field (L, "width", monitor->width);
  set_integer_field (L, "height", monitor->height);
  if (scale == floor (scale))
    lua_pushinteger (L, (lua_Integer) scale);
  else
    lua_pushnumber (L, scale);
  lua_setfield (L, -2, "scale");
  lua_pushboolean (L, monitor->index == shell_layout_manager_get_primary_index (layout));
  lua_setfield (L, -2, "primary");

  lua_createtable (L, 0, 4);
  set_integer_field (L, "x", work_area.x);
  set_integer_field (L, "y", work_area.y);
  set_integer_field (L, "width", work_area.width);
  set_integer_field (L, "height", work_area.height);
  lua_setfield (L, -2, "work_area");

  shell_monitor_free (monitor);
}

static int
l_monitors_list (lua_State *L)
{
  ShellLayoutManager *layout = get_layout_manager (L);
  int i, n = shell_layout_manager_get_n_monitors (layout);

  lua_createtable (L, n, 0);
  for (i = 0; i < n; i++)
    {
      push_monitor (L, layout, i);
      lua_rawseti (L, -2, i + 1);
    }
  return 1;
}

static int
l_monitors_primary (lua_State *L)
{
  ShellLayoutManager *layout = get_layout_manager (L);

  if (shell_layout_manager_get_n_monitors (layout) == 0)
    lua_pushnil (L);
  else
    push_monitor (L, layout, shell_layout_manager_get_primary_index (layout));
  return 1;
}

static int
l_monitors_get (lua_State *L)
{
  ShellLayoutManager *layout = get_layout_manager (L);
  lua_Integer n = luaL_checkinteger (L, 1);

  if (n >= 1 && n <= shell_layout_manager_get_n_monitors (layout))
    push_monitor (L, layout, (int) n - 1);
  else
    lua_pushnil (L);
  return 1;
}

static const luaL_Reg monitors_funcs[] = {
  { "list",    l_monitors_list },
  { "primary", l_monitors_primary },
  { "get",     l_monitors_get },
  { NULL, NULL }
};

/* ------------------------------------------------------------------------ */
/* shell.apps                                                               */
/* ------------------------------------------------------------------------ */

static ShellApp *
lookup_app (const char *app_id)
{
  ShellAppSystem *system = shell_app_system_get_default ();

  return system != NULL ? shell_app_system_lookup_app (system, app_id) : NULL;
}

/* The apps with a desktop file; the ones backed by a window alone have no id
 * an extension could use. */
static void
push_app_ids (lua_State *L,
              GList     *apps)
{
  GList *l;
  int i = 1;

  lua_newtable (L);
  for (l = apps; l != NULL; l = l->next)
    {
      ShellApp *app = l->data;

      if (shell_app_is_window_backed (app))
        continue;
      lua_pushstring (L, shell_app_get_id (app));
      lua_rawseti (L, -2, i++);
    }
}

static int
l_apps_launch (lua_State *L)
{
  const char *app_id = luaL_checkstring (L, 1);
  ShellApp *app = lookup_app (app_id);
  ShellGlobal *global = shell_global_get ();
  g_autoptr (GError) error = NULL;

  if (app == NULL)
    {
      lua_pushnil (L);
      lua_pushfstring (L, "unknown application '%s'", app_id);
      return 2;
    }

  switch (shell_app_get_state (app))
    {
    case SHELL_APP_STATE_STOPPED:
      if (!shell_app_launch (app, shell_global_get_current_time (global), -1,
                             SHELL_APP_LAUNCH_GPU_APP_PREF, &error))
        {
          lua_pushnil (L);
          lua_pushstring (L, error->message);
          return 2;
        }
      break;

    case SHELL_APP_STATE_STARTING:
      break;

    case SHELL_APP_STATE_RUNNING:
      /* like clicking its dash icon */
      shell_app_activate (app);
      break;

    default:
      g_assert_not_reached ();
    }

  lua_pushboolean (L, TRUE);
  return 1;
}

static int
l_apps_running (lua_State *L)
{
  ShellAppSystem *system = shell_app_system_get_default ();
  GSList *running = system != NULL ? shell_app_system_get_running (system) : NULL;
  GList *apps = NULL;
  GSList *s;

  for (s = running; s != NULL; s = s->next)
    apps = g_list_prepend (apps, s->data);
  apps = g_list_reverse (apps);
  push_app_ids (L, apps);
  g_list_free (apps);
  g_slist_free (running);
  return 1;
}

static int
l_apps_favorites (lua_State *L)
{
  ShellAppFavorites *favorites = shell_app_favorites_get_default ();
  GList *apps = favorites != NULL ? shell_app_favorites_get_favorites (favorites) : NULL;

  push_app_ids (L, apps);
  g_list_free (apps);
  return 1;
}

static int
l_apps_info (lua_State *L)
{
  ShellApp *app = lookup_app (luaL_checkstring (L, 1));
  GIcon *icon;
  const char *description;

  if (app == NULL)
    {
      lua_pushnil (L);
      return 1;
    }

  lua_createtable (L, 0, 4);
  lua_pushstring (L, shell_app_get_id (app));
  lua_setfield (L, -2, "id");
  lua_pushstring (L, shell_app_get_name (app));
  lua_setfield (L, -2, "name");

  icon = shell_app_get_icon (app);
  if (G_IS_THEMED_ICON (icon))
    {
      const char * const *names = g_themed_icon_get_names (G_THEMED_ICON (icon));

      if (names != NULL && names[0] != NULL)
        {
          lua_pushstring (L, names[0]);
          lua_setfield (L, -2, "icon_name");
        }
    }

  description = shell_app_get_description (app);
  if (description != NULL)
    {
      lua_pushstring (L, description);
      lua_setfield (L, -2, "description");
    }
  return 1;
}

static const luaL_Reg apps_funcs[] = {
  { "launch",    l_apps_launch },
  { "running",   l_apps_running },
  { "favorites", l_apps_favorites },
  { "info",      l_apps_info },
  { NULL, NULL }
};

/* ------------------------------------------------------------------------ */
/* Registration                                                             */
/* ------------------------------------------------------------------------ */

static void
shell_lua_api_window_register (ShellLuaState *state,
                               lua_State     *L)
{
  int shell = lua_absindex (L, -1);

  luaL_checkstack (L, 6, "shell window api");

  window_type.gtype = META_TYPE_WINDOW;
  shell_lua_handle_register_type (L, &window_type);
  workspace_type.gtype = META_TYPE_WORKSPACE;
  shell_lua_handle_register_type (L, &workspace_type);

  lua_newtable (L);
  luaL_setfuncs (L, windows_funcs, 0);
  lua_setfield (L, shell, "windows");

  lua_newtable (L);
  luaL_setfuncs (L, workspaces_funcs, 0);
  lua_setfield (L, shell, "workspaces");

  lua_newtable (L);
  luaL_setfuncs (L, monitors_funcs, 0);
  lua_setfield (L, shell, "monitors");

  lua_newtable (L);
  luaL_setfuncs (L, apps_funcs, 0);
  lua_setfield (L, shell, "apps");
}
