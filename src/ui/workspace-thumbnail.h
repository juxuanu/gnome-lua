/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * workspace-thumbnail.h: The strip of workspace thumbnails of the overview
 * (the windows of a workspace as clones, the indicator of the active
 * workspace, and the drag and drop that moves windows between workspaces
 * and creates new ones). Port of js/ui/workspaceThumbnail.js.
 *
 * #ShellThumbnailsBox is created by overview-controls.c (the primary monitor)
 * and workspaces-view.c (the other monitors). The parts of the shell it needs
 * and that are still JS are reached through the properties and signals
 * documented on the type, which the consumers keep in sync.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>
#include <meta/meta-window-actor.h>
#include <meta/window.h>
#include <meta/workspace.h>

#include "st.h"
#include "ui/window-manager.h"

G_BEGIN_DECLS

/**
 * SHELL_MAX_THUMBNAIL_SCALE:
 *
 * The maximum size of a thumbnail: 5% of the width and height of the monitor
 * (`MAX_THUMBNAIL_SCALE`).
 */
#define SHELL_MAX_THUMBNAIL_SCALE 0.05

/**
 * ShellThumbnailState:
 * @SHELL_THUMBNAIL_STATE_NEW: just added, collapsed and slid out
 * @SHELL_THUMBNAIL_STATE_EXPANDING: expanding to its width
 * @SHELL_THUMBNAIL_STATE_EXPANDED: expanded, waiting to slide in
 * @SHELL_THUMBNAIL_STATE_ANIMATING_IN: sliding in
 * @SHELL_THUMBNAIL_STATE_NORMAL: shown, the thumbnail of an existing workspace
 * @SHELL_THUMBNAIL_STATE_REMOVING: its workspace is gone, waiting to slide out
 * @SHELL_THUMBNAIL_STATE_ANIMATING_OUT: sliding out
 * @SHELL_THUMBNAIL_STATE_ANIMATED_OUT: slid out, waiting to collapse
 * @SHELL_THUMBNAIL_STATE_COLLAPSING: collapsing
 * @SHELL_THUMBNAIL_STATE_DESTROYED: destroyed
 *
 * Where a #ShellWorkspaceThumbnail is in its life, which is in the order of
 * this enumeration (`ThumbnailState`). A thumbnail with a state after
 * %SHELL_THUMBNAIL_STATE_NORMAL does not accept drops.
 */
typedef enum
{
  SHELL_THUMBNAIL_STATE_NEW           = 0,
  SHELL_THUMBNAIL_STATE_EXPANDING     = 1,
  SHELL_THUMBNAIL_STATE_EXPANDED      = 2,
  SHELL_THUMBNAIL_STATE_ANIMATING_IN  = 3,
  SHELL_THUMBNAIL_STATE_NORMAL        = 4,
  SHELL_THUMBNAIL_STATE_REMOVING      = 5,
  SHELL_THUMBNAIL_STATE_ANIMATING_OUT = 6,
  SHELL_THUMBNAIL_STATE_ANIMATED_OUT  = 7,
  SHELL_THUMBNAIL_STATE_COLLAPSING    = 8,
  SHELL_THUMBNAIL_STATE_DESTROYED     = 9,
} ShellThumbnailState;

/* ------------------------------------------------------------------------ */
/* ShellWindowClone                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WINDOW_CLONE (shell_window_clone_get_type ())
G_DECLARE_FINAL_TYPE (ShellWindowClone, shell_window_clone,
                      SHELL, WINDOW_CLONE, ClutterActor)

ShellWindowClone *shell_window_clone_new                   (MetaWindowActor  *window_actor);

MetaWindowActor  *shell_window_clone_get_window_actor      (ShellWindowClone *self);
MetaWindow       *shell_window_clone_get_meta_window       (ShellWindowClone *self);
gboolean          shell_window_clone_get_in_drag           (ShellWindowClone *self);

ShellWindowClone *shell_window_clone_get_stack_above       (ShellWindowClone *self);
void              shell_window_clone_set_stack_above       (ShellWindowClone *self,
                                                            ShellWindowClone *stack_above);
ShellWindowClone *shell_window_clone_get_actual_stack_above (ShellWindowClone *self);

void              shell_window_clone_add_attached_dialog   (ShellWindowClone *self,
                                                            MetaWindow       *dialog);

/* ------------------------------------------------------------------------ */
/* ShellWorkspaceThumbnail                                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WORKSPACE_THUMBNAIL (shell_workspace_thumbnail_get_type ())
G_DECLARE_FINAL_TYPE (ShellWorkspaceThumbnail, shell_workspace_thumbnail,
                      SHELL, WORKSPACE_THUMBNAIL, StWidget)

ShellWorkspaceThumbnail *shell_workspace_thumbnail_new            (MetaWorkspace           *meta_workspace,
                                                                   int                      monitor_index,
                                                                   ShellWindowManager      *window_manager);

MetaWorkspace           *shell_workspace_thumbnail_get_meta_workspace (ShellWorkspaceThumbnail *self);
int                      shell_workspace_thumbnail_get_monitor_index  (ShellWorkspaceThumbnail *self);

ShellThumbnailState      shell_workspace_thumbnail_get_state      (ShellWorkspaceThumbnail *self);
void                     shell_workspace_thumbnail_set_state      (ShellWorkspaceThumbnail *self,
                                                                   ShellThumbnailState      state);
double                   shell_workspace_thumbnail_get_slide_position (ShellWorkspaceThumbnail *self);
void                     shell_workspace_thumbnail_set_slide_position (ShellWorkspaceThumbnail *self,
                                                                       double                   slide_position);
double                   shell_workspace_thumbnail_get_collapse_fraction (ShellWorkspaceThumbnail *self);
void                     shell_workspace_thumbnail_set_collapse_fraction (ShellWorkspaceThumbnail *self,
                                                                          double                   collapse_fraction);

void                     shell_workspace_thumbnail_set_porthole   (ShellWorkspaceThumbnail *self,
                                                                   float                    x,
                                                                   float                    y,
                                                                   float                    width,
                                                                   float                    height);
void                     shell_workspace_thumbnail_set_scale      (ShellWorkspaceThumbnail *self,
                                                                   double                   scale_x,
                                                                   double                   scale_y);
void                     shell_workspace_thumbnail_activate       (ShellWorkspaceThumbnail *self,
                                                                   guint32                  time);
void                     shell_workspace_thumbnail_workspace_removed (ShellWorkspaceThumbnail *self);

/* ------------------------------------------------------------------------ */
/* ShellThumbnailsBox                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_THUMBNAILS_BOX (shell_thumbnails_box_get_type ())
G_DECLARE_FINAL_TYPE (ShellThumbnailsBox, shell_thumbnails_box,
                      SHELL, THUMBNAILS_BOX, StWidget)

ShellThumbnailsBox *shell_thumbnails_box_new                  (StAdjustment       *scroll_adjustment,
                                                               int                 monitor_index,
                                                               ShellWindowManager *window_manager);

int                 shell_thumbnails_box_get_monitor_index    (ShellThumbnailsBox *self);
void                shell_thumbnails_box_set_monitor_index    (ShellThumbnailsBox *self,
                                                               int                 monitor_index);
double              shell_thumbnails_box_get_expand_fraction  (ShellThumbnailsBox *self);
void                shell_thumbnails_box_set_expand_fraction  (ShellThumbnailsBox *self,
                                                               double              expand_fraction);
double              shell_thumbnails_box_get_scale            (ShellThumbnailsBox *self);
void                shell_thumbnails_box_set_scale            (ShellThumbnailsBox *self,
                                                               double              scale);
gboolean            shell_thumbnails_box_get_should_show      (ShellThumbnailsBox *self);
double              shell_thumbnails_box_get_max_thumbnail_scale (ShellThumbnailsBox *self);

void                shell_thumbnails_box_add_thumbnails       (ShellThumbnailsBox *self,
                                                               int                 start,
                                                               int                 count);
void                shell_thumbnails_box_remove_thumbnails    (ShellThumbnailsBox *self,
                                                               int                 start,
                                                               int                 count);

void                shell_thumbnails_box_drag_begin           (ShellThumbnailsBox *self);
void                shell_thumbnails_box_drag_end             (ShellThumbnailsBox *self);
void                shell_thumbnails_box_drag_cancelled       (ShellThumbnailsBox *self);

G_END_DECLS
