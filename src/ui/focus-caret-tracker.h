/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * focus-caret-tracker.h: Reports the focus and text caret changes of the
 * accessible applications, through AT-SPI. Port of js/ui/focusCaretTracker.js.
 *
 * Copyright 2012 Inclusive Design Research Centre, OCAD University.
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SHELL_TYPE_FOCUS_CARET_TRACKER (shell_focus_caret_tracker_get_type ())
G_DECLARE_FINAL_TYPE (ShellFocusCaretTracker, shell_focus_caret_tracker,
                      SHELL, FOCUS_CARET_TRACKER, GObject)

ShellFocusCaretTracker *shell_focus_caret_tracker_new                    (void);

void                    shell_focus_caret_tracker_register_focus_listener   (ShellFocusCaretTracker *self);
void                    shell_focus_caret_tracker_deregister_focus_listener (ShellFocusCaretTracker *self);
void                    shell_focus_caret_tracker_register_caret_listener   (ShellFocusCaretTracker *self);
void                    shell_focus_caret_tracker_deregister_caret_listener (ShellFocusCaretTracker *self);

G_END_DECLS
