/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * workspace.h: The window picker of one workspace on one monitor in the
 * overview: its background, the previews of its windows and the layout that
 * arranges them. Also the layout of the window selector of the screenshot
 * UI, which shares the algorithm. Port of js/ui/workspace.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>
#include <meta/meta-workspace-manager.h>
#include <meta/window.h>
#include <mtk/mtk.h>

#include "st.h"
#include "ui/window-manager.h"
#include "ui/window-preview.h"

G_BEGIN_DECLS

#define SHELL_TYPE_WORKSPACE (shell_workspace_get_type ())
G_DECLARE_FINAL_TYPE (ShellWorkspace, shell_workspace, SHELL, WORKSPACE, StWidget)

ShellWorkspace *shell_workspace_new                    (MetaWorkspace      *meta_workspace,
                                                        int                 monitor_index,
                                                        StAdjustment       *overview_adjustment,
                                                        ShellWindowManager *window_manager);

MetaWorkspace  *shell_workspace_get_meta_workspace     (ShellWorkspace     *self);
int             shell_workspace_get_monitor_index      (ShellWorkspace     *self);
StAdjustment   *shell_workspace_get_state_adjustment   (ShellWorkspace     *self);

gboolean        shell_workspace_contains_meta_window   (ShellWorkspace     *self,
                                                        MetaWindow         *meta_window);
gboolean        shell_workspace_is_empty               (ShellWorkspace     *self);
gboolean        shell_workspace_has_maximized_windows  (ShellWorkspace     *self);

void            shell_workspace_sync_stacking          (ShellWorkspace     *self,
                                                        const guint32      *stable_sequences,
                                                        gsize               n_stable_sequences);
void            shell_workspace_prepare_to_leave_overview (ShellWorkspace  *self);

/* ------------------------------------------------------------------------ */
/* ShellWindowSlotLayout                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WINDOW_SLOT_LAYOUT (shell_window_slot_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellWindowSlotLayout, shell_window_slot_layout,
                      SHELL, WINDOW_SLOT_LAYOUT, ClutterLayoutManager)

ShellWindowSlotLayout *shell_window_slot_layout_new         (int                    monitor_index);

void                   shell_window_slot_layout_add_window  (ShellWindowSlotLayout *self,
                                                             ClutterActor          *window,
                                                             const MtkRectangle    *bounding_box);
void                   shell_window_slot_layout_reset       (ShellWindowSlotLayout *self);
GList                 *shell_window_slot_layout_get_windows (ShellWindowSlotLayout *self);

G_END_DECLS
