/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell-window-hooks.h: the window.*, workspace.* hook points of
 * docs/api/hooks.md and the view of windows that shell.windows (docs/api/
 * window.md) shares with them: which windows are listed, their number
 * conversions (1-based workspaces and monitors) and their payload snapshot.
 *
 * Private to libshell: ShellWindowManager owns one #ShellWindowHooks, feeds
 * it the events that only it sees (map, destroy, minimize, unminimize, the
 * workspace switch) and the rest is connected here, to #MetaDisplay,
 * #MetaWorkspaceManager and the windows.
 */

#pragma once

#include <glib-object.h>
#include <meta/display.h>
#include <meta/meta-workspace-manager.h>
#include <meta/window.h>
#include <meta/workspace.h>

G_BEGIN_DECLS

#define SHELL_TYPE_WINDOW_HOOKS (shell_window_hooks_get_type ())
G_DECLARE_FINAL_TYPE (ShellWindowHooks, shell_window_hooks, SHELL, WINDOW_HOOKS, GObject)

void              shell_window_hooks_declare            (void);
ShellWindowHooks *shell_window_hooks_new                (MetaDisplay          *display,
                                                         MetaWorkspaceManager *workspace_manager);

/* Events of ShellWM, forwarded by ShellWindowManager */
void              shell_window_hooks_window_mapped      (ShellWindowHooks *self,
                                                         MetaWindow       *window);
void              shell_window_hooks_window_unmapped    (ShellWindowHooks *self,
                                                         MetaWindow       *window);
void              shell_window_hooks_window_minimized   (ShellWindowHooks *self,
                                                         MetaWindow       *window,
                                                         gboolean          minimized);
void              shell_window_hooks_workspace_switched (ShellWindowHooks *self,
                                                         int               from,
                                                         int               to);

/* Shared with src/lua-api/shell-lua-api-window.c */

/* Whether shell.windows and the window.* hooks show @window: everything the
 * overview shows, i.e. not skip-taskbar. */
gboolean          shell_window_hooks_is_listed          (MetaWindow *window);
/* 1-based workspace of @window, 0 when it is on all workspaces or has none */
int               shell_window_hooks_get_workspace_number (MetaWindow *window);
/* 1-based monitor holding most of @window (the primary one if it has none) */
int               shell_window_hooks_get_monitor_number (MetaWindow *window);
/* The desktop file id of the application of @window, or %NULL */
char             *shell_window_hooks_dup_app_id         (MetaWindow *window);
/* The windows shell.windows.list () returns, most recently focused first */
GList            *shell_window_hooks_list_windows       (MetaDisplay *display);
/* The listed window with @id, or %NULL */
MetaWindow       *shell_window_hooks_lookup_window      (MetaDisplay *display,
                                                         guint64      id);
/* The listed focused window, or %NULL */
MetaWindow       *shell_window_hooks_get_focused_window (MetaDisplay *display);
/* TRUE while a window.* or workspace.* hook is being emitted: an action that
 * an extension asks for from inside one is deferred to idle (README rule 8) */
gboolean          shell_window_hooks_is_emitting        (void);

G_END_DECLS
