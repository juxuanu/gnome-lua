/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * window-manager.h: The shell's side of the window manager: the animations of
 * the windows (map, destroy, minimize, unminimize, size change), the dimming
 * of windows with attached modal dialogs, the dynamic workspaces, the tile
 * preview and resize popup, the display change confirmation, the workspace
 * and window switching keybindings, and the table of the key bindings that
 * may run in a given #ShellActionMode. Port of js/ui/windowManager.js.
 *
 * #ShellWindowManager is created by main.js (`new Shell.WindowManager ()`)
 * and kept in `Main.wm`. It drives the #ShellWM of #ShellGlobal (the object
 * that mutter's plugin forwards the effects to) and owns a
 * #ShellWorkspaceAnimationController (see workspace-animation.h).
 *
 * The JS-only parts of the shell are reached through signals that main.js
 * answers, and through the #ShellWindowManager:overview-visible and
 * #ShellWindowManager:overview-closing properties that main.js keeps in sync
 * with the overview.
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>
#include <meta/display.h>
#include <meta/keybindings.h>
#include <meta/meta-window-actor.h>
#include <meta/meta-inhibit-shortcuts-dialog.h>
#include <meta/window.h>
#include <meta/workspace.h>

#include "shell-action-modes.h"
#include "st.h"
#include "ui/ctrl-alt-tab.h"
#include "ui/modal-dialog.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellWindowDimmer                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WINDOW_DIMMER (shell_window_dimmer_get_type ())
G_DECLARE_FINAL_TYPE (ShellWindowDimmer, shell_window_dimmer,
                      SHELL, WINDOW_DIMMER, ClutterBrightnessContrastEffect)

ShellWindowDimmer *shell_window_dimmer_new       (void);
ShellWindowDimmer *shell_window_dimmer_get_for_actor (ClutterActor      *actor);
void               shell_window_dimmer_set_dimmed (ShellWindowDimmer    *self,
                                                   gboolean              dimmed,
                                                   gboolean              animate);

/* ------------------------------------------------------------------------ */
/* ShellTilePreview                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_TILE_PREVIEW (shell_tile_preview_get_type ())
G_DECLARE_FINAL_TYPE (ShellTilePreview, shell_tile_preview,
                      SHELL, TILE_PREVIEW, StWidget)

ShellTilePreview *shell_tile_preview_new   (void);
void              shell_tile_preview_open  (ShellTilePreview   *self,
                                            MetaWindow         *window,
                                            const MtkRectangle *tile_rect,
                                            int                 monitor_index);
void              shell_tile_preview_close (ShellTilePreview   *self);

/* ------------------------------------------------------------------------ */
/* ShellResizePopup                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_RESIZE_POPUP (shell_resize_popup_get_type ())
G_DECLARE_FINAL_TYPE (ShellResizePopup, shell_resize_popup,
                      SHELL, RESIZE_POPUP, StWidget)

ShellResizePopup *shell_resize_popup_new (void);
void              shell_resize_popup_set (ShellResizePopup   *self,
                                          const MtkRectangle *rect,
                                          int                 display_w,
                                          int                 display_h);

/* ------------------------------------------------------------------------ */
/* ShellDisplayChangeDialog                                                 */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_DISPLAY_CHANGE_DIALOG (shell_display_change_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellDisplayChangeDialog, shell_display_change_dialog,
                      SHELL, DISPLAY_CHANGE_DIALOG, ShellModalDialog)

ShellDisplayChangeDialog *shell_display_change_dialog_new (void);

/* ------------------------------------------------------------------------ */
/* ShellWorkspaceTracker                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WORKSPACE_TRACKER (shell_workspace_tracker_get_type ())
G_DECLARE_FINAL_TYPE (ShellWorkspaceTracker, shell_workspace_tracker,
                      SHELL, WORKSPACE_TRACKER, GObject)

ShellWorkspaceTracker *shell_workspace_tracker_new                 (void);
void                   shell_workspace_tracker_block_updates       (ShellWorkspaceTracker *self);
void                   shell_workspace_tracker_unblock_updates     (ShellWorkspaceTracker *self);
void                   shell_workspace_tracker_keep_workspace_alive (ShellWorkspaceTracker *self,
                                                                     MetaWorkspace         *workspace,
                                                                     guint                  duration);

/* ------------------------------------------------------------------------ */
/* ShellWindowManager                                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WINDOW_MANAGER (shell_window_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellWindowManager, shell_window_manager,
                      SHELL, WINDOW_MANAGER, GObject)

ShellWindowManager *shell_window_manager_new                     (void);

/* Key bindings */
void     shell_window_manager_allow_keybinding            (ShellWindowManager *self,
                                                           const char         *name,
                                                           ShellActionMode     modes);
gboolean shell_window_manager_set_custom_keybinding_handler (ShellWindowManager *self,
                                                             const char         *name,
                                                             ShellActionMode     modes,
                                                             MetaKeyHandlerFunc  handler,
                                                             gpointer            user_data,
                                                             GDestroyNotify      destroy);
guint    shell_window_manager_add_keybinding              (ShellWindowManager *self,
                                                           const char         *name,
                                                           GSettings          *settings,
                                                           MetaKeyBindingFlags flags,
                                                           ShellActionMode     modes,
                                                           MetaKeyHandlerFunc  handler,
                                                           gpointer            user_data,
                                                           GDestroyNotify      destroy);
void     shell_window_manager_remove_keybinding           (ShellWindowManager *self,
                                                           const char         *name);

/* Workspaces */
void     shell_window_manager_insert_workspace            (ShellWindowManager *self,
                                                           int                 pos);
void     shell_window_manager_keep_workspace_alive        (ShellWindowManager *self,
                                                           MetaWorkspace      *workspace,
                                                           guint               duration);
void     shell_window_manager_block_workspace_updates     (ShellWindowManager *self);
void     shell_window_manager_unblock_workspace_updates   (ShellWindowManager *self);
void     shell_window_manager_action_move_workspace       (ShellWindowManager *self,
                                                           MetaWorkspace      *workspace);
void     shell_window_manager_action_move_window          (ShellWindowManager *self,
                                                           MetaWindow         *window,
                                                           MetaWorkspace      *workspace);
gboolean shell_window_manager_handle_workspace_scroll     (ShellWindowManager *self,
                                                           const ClutterEvent *event);
StAdjustment *shell_window_manager_create_workspaces_adjustment (ShellWindowManager *self,
                                                                 ClutterActor       *actor);

/* Effects */
void     shell_window_manager_skip_next_effect            (ShellWindowManager *self,
                                                           MetaWindowActor    *actor);

/* The state of the overview, kept in sync by main.js */
gboolean shell_window_manager_get_overview_visible        (ShellWindowManager *self);
void     shell_window_manager_set_overview_visible        (ShellWindowManager *self,
                                                           gboolean            visible);
gboolean shell_window_manager_get_overview_closing        (ShellWindowManager *self);
void     shell_window_manager_set_overview_closing        (ShellWindowManager *self,
                                                           gboolean            closing);
gboolean shell_window_manager_get_overview_animating      (ShellWindowManager *self);
void     shell_window_manager_set_overview_animating      (ShellWindowManager *self,
                                                           gboolean            animating);

ShellCtrlAltTabManager *shell_window_manager_get_ctrl_alt_tab_manager (ShellWindowManager *self);
void     shell_window_manager_set_ctrl_alt_tab_manager    (ShellWindowManager     *self,
                                                           ShellCtrlAltTabManager *manager);

G_END_DECLS
