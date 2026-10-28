/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * xdnd-handler.h: Tracks drags coming from X11 clients (XDND) and presents
 * them to the shell as a drag source. Port of js/ui/xdndHandler.js.
 */

#pragma once

#include <clutter/clutter.h>

G_BEGIN_DECLS

#define SHELL_TYPE_XDND_HANDLER (shell_xdnd_handler_get_type ())
G_DECLARE_FINAL_TYPE (ShellXdndHandler, shell_xdnd_handler,
                      SHELL, XDND_HANDLER, GObject)

ShellXdndHandler *shell_xdnd_handler_new               (ClutterActor     *ui_group);

gboolean          shell_xdnd_handler_get_session_locked (ShellXdndHandler *self);
void              shell_xdnd_handler_set_session_locked (ShellXdndHandler *self,
                                                         gboolean          locked);

G_END_DECLS
