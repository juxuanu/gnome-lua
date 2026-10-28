/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * overview-controls.h: The controls of the overview: the search entry, the
 * dash, the application grid, the search results, the strip of workspace
 * thumbnails and the workspaces, and the layout that arranges them for the
 * state of the overview. Port of js/ui/overviewControls.js.
 *
 * The types, with the name they had in the JS module:
 *
 *  - #ShellOverviewAdjustment (`OverviewAdjustment`): the adjustment with the
 *    #ShellControlsState of the overview as its value.
 *  - #ShellControlsManager (`ControlsManager`): the actor with the controls.
 *  - `ControlsManagerLayout` is private to overview-controls.c.
 *
 * The controls are created and driven by #ShellOverview. They are not used
 * from JS any more: the functions that take callbacks are skipped for GI.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>

#include "st.h"
#include "ui/ctrl-alt-tab.h"
#include "ui/dash.h"
#include "ui/layout.h"
#include "ui/overview.h"
#include "ui/search-controller.h"
#include "ui/swipe-tracker.h"
#include "ui/workspace-thumbnail.h"
#include "ui/window-manager.h"

G_BEGIN_DECLS

/**
 * SHELL_SMALL_WORKSPACE_RATIO:
 *
 * The height of the workspaces in the application grid state, relative to
 * the height of the overview (`SMALL_WORKSPACE_RATIO`).
 */
#define SHELL_SMALL_WORKSPACE_RATIO 0.15

/**
 * SHELL_SIDE_CONTROLS_ANIMATION_TIME:
 *
 * Time in milliseconds of the animations of the controls around the
 * workspaces (`SIDE_CONTROLS_ANIMATION_TIME`).
 */
#define SHELL_SIDE_CONTROLS_ANIMATION_TIME 250

/**
 * ShellStateTransitionParams:
 * @transitioning: whether the state is changing, by an animation or a gesture
 * @current_state: the current value of the adjustment
 * @initial_state: the state the transition started from, or the current value
 * @final_state: the state the transition goes to, or the current value
 * @progress: how far the transition has come, between 0 and 1
 *
 * The result of shell_overview_adjustment_get_state_transition_params ().
 */
typedef struct
{
  gboolean transitioning;
  double   current_state;
  double   initial_state;
  double   final_state;
  double   progress;
} ShellStateTransitionParams;

/**
 * ShellControlsCallback: (skip)
 * @finished: whether the animation ran to its end (it was not stopped)
 * @user_data: the data given along with the callback
 *
 * Called when an animation of the controls ended.
 */
typedef void (* ShellControlsCallback) (gboolean finished,
                                        gpointer user_data);

/* ------------------------------------------------------------------------ */
/* ShellOverviewAdjustment                                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_OVERVIEW_ADJUSTMENT (shell_overview_adjustment_get_type ())
G_DECLARE_FINAL_TYPE (ShellOverviewAdjustment, shell_overview_adjustment,
                      SHELL, OVERVIEW_ADJUSTMENT, StAdjustment)

ShellOverviewAdjustment *shell_overview_adjustment_new                       (ClutterActor *actor);
gboolean                 shell_overview_adjustment_get_gesture_in_progress   (ShellOverviewAdjustment *self);
void                     shell_overview_adjustment_set_gesture_in_progress   (ShellOverviewAdjustment *self,
                                                                              gboolean                gesture_in_progress);
void                     shell_overview_adjustment_get_state_transition_params (ShellOverviewAdjustment    *self,
                                                                                ShellStateTransitionParams *params);

/* ------------------------------------------------------------------------ */
/* ShellControlsManager                                                      */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_CONTROLS_MANAGER (shell_controls_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellControlsManager, shell_controls_manager,
                      SHELL, CONTROLS_MANAGER, StWidget)

ShellControlsManager  *shell_controls_manager_new                  (ShellOverview          *overview,
                                                                    ShellWindowManager     *window_manager,
                                                                    ShellLayoutManager     *layout_manager,
                                                                    ShellCtrlAltTabManager *ctrl_alt_tab_manager);

ShellDash             *shell_controls_manager_get_dash             (ShellControlsManager *self);
ShellSearchController *shell_controls_manager_get_search_controller (ShellControlsManager *self);
StEntry               *shell_controls_manager_get_search_entry     (ShellControlsManager *self);
ClutterActor          *shell_controls_manager_get_app_display      (ShellControlsManager *self);
ShellOverviewAdjustment *shell_controls_manager_get_state_adjustment (ShellControlsManager *self);

void                   shell_controls_manager_prepare_to_enter_overview (ShellControlsManager *self);
void                   shell_controls_manager_prepare_to_leave_overview (ShellControlsManager *self);
void                   shell_controls_manager_animate_to_overview  (ShellControlsManager  *self,
                                                                    ShellControlsState     state,
                                                                    ShellControlsCallback  callback,
                                                                    gpointer               user_data,
                                                                    GDestroyNotify         destroy);
void                   shell_controls_manager_animate_from_overview (ShellControlsManager *self,
                                                                     ShellControlsCallback callback,
                                                                     gpointer              user_data,
                                                                     GDestroyNotify        destroy);
gboolean               shell_controls_manager_get_workspaces_box_for_state (ShellControlsManager *self,
                                                                            ShellControlsState    state,
                                                                            ClutterActorBox      *box);

void                   shell_controls_manager_gesture_begin        (ShellControlsManager *self,
                                                                    ShellSwipeTracker    *tracker);
void                   shell_controls_manager_gesture_progress     (ShellControlsManager *self,
                                                                    double                progress);
void                   shell_controls_manager_gesture_end          (ShellControlsManager  *self,
                                                                    double                 target,
                                                                    guint                  duration,
                                                                    ShellControlsCallback  callback,
                                                                    gpointer               user_data,
                                                                    GDestroyNotify         destroy);
void                   shell_controls_manager_run_startup_animation (ShellControlsManager  *self,
                                                                     ShellControlsCallback  callback,
                                                                     gpointer               user_data,
                                                                     GDestroyNotify         destroy);

void                   shell_controls_connect_thumbnails_box       (ShellOverview       *overview,
                                                                    ShellThumbnailsBox  *thumbnails_box);

G_END_DECLS
