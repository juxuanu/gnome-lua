/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * workspace-switcher-popup.h: The row of dots flashed at the bottom of the
 * monitor(s) when switching workspaces. Port of
 * js/ui/workspaceSwitcherPopup.js.
 */

#pragma once

#include <clutter/clutter.h>

G_BEGIN_DECLS

#define SHELL_TYPE_WORKSPACE_SWITCHER_POPUP (shell_workspace_switcher_popup_get_type ())
G_DECLARE_FINAL_TYPE (ShellWorkspaceSwitcherPopup, shell_workspace_switcher_popup,
                      SHELL, WORKSPACE_SWITCHER_POPUP, ClutterActor)

ShellWorkspaceSwitcherPopup *shell_workspace_switcher_popup_new     (ClutterActor                *ui_group);

void                         shell_workspace_switcher_popup_display (ShellWorkspaceSwitcherPopup *self,
                                                                     int                          active_workspace_index);

G_END_DECLS
