/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * workspace-animation.h: The animation of switching workspaces, by a
 * keybinding or by a touchpad/touchscreen swipe: clones of the windows of
 * every workspace on a strip which slides, one per monitor. Port of
 * js/ui/workspaceAnimation.js.
 *
 * #ShellWorkspaceAnimationController is owned by #ShellWindowManager, which
 * starts the animation of a keybinding with
 * shell_workspace_animation_controller_animate_switch () and asks it whether
 * a swipe is in progress. The groups of actors of the JS file
 * (BaseWorkspaceGroup, WorkspaceGroup, WorkspaceBackground and MonitorGroup)
 * are private to workspace-animation.c.
 */

#pragma once

#include <clutter/clutter.h>
#include <meta/window.h>
#include <meta/workspace.h>

#include "ui/window-manager.h"

G_BEGIN_DECLS

/**
 * SHELL_WORKSPACE_ANIMATION_WINDOW_ANIMATION_TIME:
 *
 * Duration in milliseconds of the animation of switching workspaces with a
 * keybinding (`WorkspaceAnimation.WINDOW_ANIMATION_TIME`).
 */
#define SHELL_WORKSPACE_ANIMATION_WINDOW_ANIMATION_TIME 250

/**
 * SHELL_WORKSPACE_ANIMATION_WORKSPACE_SPACING:
 *
 * The gap in pixels (before applying the scale factor) between two
 * workspaces on the strip (`WorkspaceAnimation.WORKSPACE_SPACING`).
 */
#define SHELL_WORKSPACE_ANIMATION_WORKSPACE_SPACING 100

/**
 * ShellWorkspaceAnimationCallback:
 * @user_data: (closure): the user data given along with the callback
 *
 * Called when the animation of a workspace switch has finished.
 */
typedef void (* ShellWorkspaceAnimationCallback) (gpointer user_data);

#define SHELL_TYPE_WORKSPACE_ANIMATION_CONTROLLER (shell_workspace_animation_controller_get_type ())
G_DECLARE_FINAL_TYPE (ShellWorkspaceAnimationController,
                      shell_workspace_animation_controller,
                      SHELL, WORKSPACE_ANIMATION_CONTROLLER, GObject)

ShellWorkspaceAnimationController *shell_workspace_animation_controller_new (ShellWindowManager *window_manager);

void       shell_workspace_animation_controller_animate_switch       (ShellWorkspaceAnimationController *self,
                                                                      int                                from,
                                                                      int                                to,
                                                                      MetaMotionDirection                direction,
                                                                      ShellWorkspaceAnimationCallback    on_complete,
                                                                      gpointer                           user_data,
                                                                      GDestroyNotify                     destroy);
gboolean   shell_workspace_animation_controller_can_handle_scroll_event (ShellWorkspaceAnimationController *self,
                                                                        const ClutterEvent                 *event);
void       shell_workspace_animation_controller_cancel_switch_animation (ShellWorkspaceAnimationController *self);
gboolean   shell_workspace_animation_controller_get_gesture_active   (ShellWorkspaceAnimationController *self);
MetaWindow *shell_workspace_animation_controller_get_moving_window   (ShellWorkspaceAnimationController *self);
void       shell_workspace_animation_controller_set_moving_window    (ShellWorkspaceAnimationController *self,
                                                                      MetaWindow                        *moving_window);

G_END_DECLS
