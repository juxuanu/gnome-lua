/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * workspaces-view.h: The workspaces of the overview, on every monitor: the
 * row (or column) of window pickers that scrolls with the active workspace,
 * and the strip of thumbnails and workspaces of the secondary monitors. Port
 * of js/ui/workspacesView.js.
 *
 * #ShellWorkspacesDisplay (`WorkspacesDisplay`) is the only public type; it
 * is created by the controls of the overview (see overview-controls.h). The
 * other classes of the JS module are private to workspaces-view.c:
 * `WorkspacesViewBase`, `WorkspacesView`, `ExtraWorkspaceView` and
 * `SecondaryMonitorDisplay`.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>

#include "st.h"
#include "ui/layout.h"
#include "ui/overview-controls.h"
#include "ui/overview.h"
#include "ui/window-manager.h"

G_BEGIN_DECLS

/**
 * ShellFitMode:
 * @SHELL_FIT_MODE_SINGLE: one workspace fills the view
 * @SHELL_FIT_MODE_ALL: all the workspaces fit in the view
 *
 * How the workspaces are fit in the view; the value of the fit mode
 * adjustment lies between the two (`WorkspacesView.FitMode`).
 */
typedef enum
{
  SHELL_FIT_MODE_SINGLE = 0,
  SHELL_FIT_MODE_ALL    = 1,
} ShellFitMode;

#define SHELL_TYPE_WORKSPACES_DISPLAY (shell_workspaces_display_get_type ())
G_DECLARE_FINAL_TYPE (ShellWorkspacesDisplay, shell_workspaces_display,
                      SHELL, WORKSPACES_DISPLAY, StWidget)

ShellWorkspacesDisplay *shell_workspaces_display_new                    (ShellOverview           *overview,
                                                                         ShellControlsManager    *controls,
                                                                         StAdjustment            *scroll_adjustment,
                                                                         ShellOverviewAdjustment *overview_adjustment,
                                                                         ShellWindowManager      *window_manager,
                                                                         ShellLayoutManager      *layout_manager);

StAdjustment           *shell_workspaces_display_get_fit_mode_adjustment (ShellWorkspacesDisplay *self);
void                    shell_workspaces_display_set_primary_workspace_visible (ShellWorkspacesDisplay *self,
                                                                                gboolean                visible);
void                    shell_workspaces_display_prepare_to_enter_overview (ShellWorkspacesDisplay *self);
void                    shell_workspaces_display_prepare_to_leave_overview (ShellWorkspacesDisplay *self);
gboolean                shell_workspaces_display_active_workspace_has_maximized_windows (ShellWorkspacesDisplay *self);

G_END_DECLS
