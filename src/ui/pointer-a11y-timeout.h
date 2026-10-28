/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * pointer-a11y-timeout.h: Visual feedback (a "pie timer" drawn under the
 * pointer) for the pointer accessibility timeouts (simulated secondary
 * click, dwell click and dwell gesture). Port of js/ui/pointerA11yTimeout.js.
 */

#pragma once

#include <clutter/clutter.h>

G_BEGIN_DECLS

#define SHELL_TYPE_POINTER_A11Y_TIMEOUT (shell_pointer_a11y_timeout_get_type ())
G_DECLARE_FINAL_TYPE (ShellPointerA11yTimeout, shell_pointer_a11y_timeout,
                      SHELL, POINTER_A11Y_TIMEOUT, GObject)

ShellPointerA11yTimeout *shell_pointer_a11y_timeout_new (ClutterActor *ui_group);

G_END_DECLS
