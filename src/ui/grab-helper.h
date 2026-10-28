/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * grab-helper.h: Keyboard and pointer grabs associated with a set of
 * actors, dropped when the user clicks outside or presses Escape. Port of
 * js/ui/grabHelper.js.
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>

#include "shell-action-modes.h"

G_BEGIN_DECLS

#define SHELL_TYPE_GRAB_HELPER (shell_grab_helper_get_type ())
G_DECLARE_FINAL_TYPE (ShellGrabHelper, shell_grab_helper,
                      SHELL, GRAB_HELPER, GObject)

/**
 * ShellGrabHelperUngrabFunc:
 * @is_user: %TRUE if the user dropped the grab by clicking outside of it or
 *   typing Escape, %FALSE if the grab was dropped by the code
 * @user_data: (closure): the data given to shell_grab_helper_grab ()
 *
 * Called when a grab is dropped.
 */
typedef void (* ShellGrabHelperUngrabFunc) (gboolean  is_user,
                                            gpointer  user_data);

ShellGrabHelper *shell_grab_helper_new               (ClutterActor    *owner,
                                                      ShellActionMode  action_mode);

gboolean         shell_grab_helper_grab              (ShellGrabHelper           *self,
                                                      ClutterActor              *actor,
                                                      ClutterActor              *focus,
                                                      ShellGrabHelperUngrabFunc  on_ungrab,
                                                      gpointer                   user_data,
                                                      GDestroyNotify             destroy);
void             shell_grab_helper_grab_async        (ShellGrabHelper     *self,
                                                      ClutterActor        *actor,
                                                      ClutterActor        *focus,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);
gboolean         shell_grab_helper_grab_finish       (ShellGrabHelper *self,
                                                      GAsyncResult    *result,
                                                      GError         **error);

void             shell_grab_helper_ungrab            (ShellGrabHelper *self,
                                                      gboolean         is_user);
void             shell_grab_helper_ungrab_actor      (ShellGrabHelper *self,
                                                      ClutterActor    *actor,
                                                      gboolean         is_user);

gboolean         shell_grab_helper_is_actor_grabbed  (ShellGrabHelper *self,
                                                      ClutterActor    *actor);
gboolean         shell_grab_helper_get_grabbed       (ShellGrabHelper *self);
ClutterActor    *shell_grab_helper_get_current_actor (ShellGrabHelper *self);

G_END_DECLS
