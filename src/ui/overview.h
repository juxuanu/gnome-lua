/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * overview.h: The overview: the window picker, the application grid and the
 * search, and the state machine that shows and hides them (animations, the
 * modal grab, the gestures and the drags that hover windows). Port of
 * js/ui/overview.js; see overview-controls.h and workspaces-view.h for the
 * other two modules of the overview.
 *
 * #ShellOverview is created by ShellMain, after the window manager
 * (`Main.overview = new Shell.Overview ({window_manager, layout_manager,
 * xdnd_handler, ctrl_alt_tab_manager, session_mode})`) and is kept in
 * `Main.overview`. It connects itself to the objects it is created with, and
 * keeps #ShellWindowManager:overview-visible, #ShellWindowManager:overview-closing
 * and #ShellWindowManager:overview-animating, and #ShellPanel:overview-visible
 * in sync with its state.
 *
 * Hooks declared here: "overview.showing", "overview.shown", "overview.hiding"
 * and "overview.hidden" (docs/api/hooks.md).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>
#include <meta/window.h>

#include "st.h"
#include "ui/ctrl-alt-tab.h"
#include "ui/dash.h"
#include "ui/layout.h"
#include "ui/search-controller.h"
#include "ui/session-mode.h"
#include "ui/window-manager.h"
#include "ui/xdnd-handler.h"

G_BEGIN_DECLS

/**
 * SHELL_OVERVIEW_ANIMATION_TIME:
 *
 * Time in milliseconds of the animation going into the overview
 * (`Overview.ANIMATION_TIME`).
 */
#define SHELL_OVERVIEW_ANIMATION_TIME 250

/**
 * ShellControlsState:
 * @SHELL_CONTROLS_STATE_HIDDEN: the overview is hidden
 * @SHELL_CONTROLS_STATE_WINDOW_PICKER: the window picker is shown
 * @SHELL_CONTROLS_STATE_APP_GRID: the application grid is shown
 *
 * The state of the controls of the overview, which is also the value of the
 * adjustment that animates the overview (`OverviewControls.ControlsState`).
 */
typedef enum
{
  SHELL_CONTROLS_STATE_HIDDEN        = 0,
  SHELL_CONTROLS_STATE_WINDOW_PICKER = 1,
  SHELL_CONTROLS_STATE_APP_GRID      = 2,
} ShellControlsState;

#define SHELL_TYPE_OVERVIEW (shell_overview_get_type ())
G_DECLARE_FINAL_TYPE (ShellOverview, shell_overview, SHELL, OVERVIEW, GObject)

ShellOverview *shell_overview_new                   (ShellWindowManager     *window_manager,
                                                     ShellLayoutManager     *layout_manager,
                                                     ShellXdndHandler       *xdnd_handler,
                                                     ShellCtrlAltTabManager *ctrl_alt_tab_manager,
                                                     ShellSessionMode       *session_mode);

/* The state */
gboolean       shell_overview_get_visible           (ShellOverview *self);
gboolean       shell_overview_get_visible_target    (ShellOverview *self);
gboolean       shell_overview_get_animation_in_progress (ShellOverview *self);
gboolean       shell_overview_get_closing           (ShellOverview *self);
ShellControlsState shell_overview_get_target_state  (ShellOverview *self);
gboolean       shell_overview_hooks_are_emitting    (void);
gboolean       shell_overview_get_is_dummy          (ShellOverview *self);

/* The parts of the overview */
ShellDash             *shell_overview_get_dash              (ShellOverview *self);
ShellSearchController *shell_overview_get_search_controller (ShellOverview *self);
StEntry               *shell_overview_get_search_entry      (ShellOverview *self);

/* Showing and hiding */
void           shell_overview_show                  (ShellOverview      *self);
void           shell_overview_show_apps             (ShellOverview      *self);
void           shell_overview_show_for_reason       (ShellOverview      *self,
                                                     ShellControlsState  state,
                                                     const char         *reason);
void           shell_overview_hide                  (ShellOverview      *self);
void           shell_overview_hide_for_reason       (ShellOverview      *self,
                                                     const char         *reason);
void           shell_overview_toggle                (ShellOverview      *self);
void           shell_overview_toggle_for_reason     (ShellOverview      *self,
                                                     const char         *reason);

/* The same for `shell.overview` (docs/api/overview.md): the reason of the
 * hooks is "extension" and their `owner` key the uuid of the extension */
void           shell_overview_show_for_extension    (ShellOverview      *self,
                                                     ShellControlsState  state,
                                                     const char         *owner);
void           shell_overview_hide_for_extension    (ShellOverview      *self,
                                                     const char         *owner);
void           shell_overview_toggle_for_extension  (ShellOverview      *self,
                                                     const char         *owner);

void           shell_overview_focus_search          (ShellOverview      *self);
void           shell_overview_select_app            (ShellOverview      *self,
                                                     const char         *id);
gboolean       shell_overview_should_toggle_by_corner_or_button (ShellOverview *self);

/* The drags of items and windows */
void           shell_overview_begin_item_drag       (ShellOverview *self,
                                                     GObject       *source);
void           shell_overview_cancelled_item_drag   (ShellOverview *self,
                                                     GObject       *source);
void           shell_overview_end_item_drag         (ShellOverview *self,
                                                     GObject       *source);
void           shell_overview_begin_window_drag     (ShellOverview *self,
                                                     MetaWindow    *window);
void           shell_overview_cancelled_window_drag (ShellOverview *self,
                                                     MetaWindow    *window);
void           shell_overview_end_window_drag       (ShellOverview *self,
                                                     MetaWindow    *window);

G_END_DECLS
