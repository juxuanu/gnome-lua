/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * locate-pointer.h: Plays a ripple animation at the pointer position when
 * the "locate pointer" accessibility setting is on. Port of
 * js/ui/locatePointer.js.
 */

#pragma once

#include <clutter/clutter.h>

G_BEGIN_DECLS

#define SHELL_TYPE_LOCATE_POINTER (shell_locate_pointer_get_type ())
G_DECLARE_FINAL_TYPE (ShellLocatePointer, shell_locate_pointer,
                      SHELL, LOCATE_POINTER, GObject)

ShellLocatePointer *shell_locate_pointer_new  (ClutterActor       *ui_group);

void                shell_locate_pointer_show (ShellLocatePointer *self);

G_END_DECLS
