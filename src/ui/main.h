/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * main.h: The modal stack of the shell (`Main.pushModal ()` and
 * `Main.popModal ()`) and the action mode derived from it. Port of the modal
 * part of js/ui/main.js; the rest of that file is still JS.
 */

#pragma once

#include <clutter/clutter.h>

#include "shell-action-modes.h"

G_BEGIN_DECLS

#define SHELL_TYPE_MAIN (shell_main_get_type ())
G_DECLARE_FINAL_TYPE (ShellMain, shell_main, SHELL, MAIN, GObject)

ShellMain       *shell_main_get_default          (void);

ClutterGrab     *shell_main_push_modal           (ShellMain       *self,
                                                  ClutterActor    *actor,
                                                  ShellActionMode  action_mode);
gboolean         shell_main_pop_modal            (ShellMain       *self,
                                                  ClutterGrab     *grab,
                                                  GError         **error);

guint            shell_main_get_modal_count      (ShellMain       *self);

ShellActionMode  shell_main_get_action_mode      (ShellMain       *self);
void             shell_main_set_action_mode      (ShellMain       *self,
                                                  ShellActionMode  action_mode);

G_END_DECLS
