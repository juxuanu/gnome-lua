/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * window-preview.h: The clone of a window in the window picker of the
 * overview, with its title, application icon and close button, which are
 * shown while the pointer or the keyboard focus is on it. Port of
 * js/ui/windowPreview.js; the class existed in C already (it used to be
 * subclassed in JS), it now holds the logic of the JS subclass.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <meta/window.h>

#include "st.h"
#include "ui/dnd.h"

G_BEGIN_DECLS

/**
 * SHELL_WINDOW_PREVIEW_DND_SIZE:
 *
 * The size in pixels the drag actor of a window preview is scaled down to
 * (`WINDOW_DND_SIZE` of windowPreview.js).
 */
#define SHELL_WINDOW_PREVIEW_DND_SIZE 256

/**
 * SHELL_WINDOW_PREVIEW_DRAGGING_OPACITY:
 *
 * The opacity of the drag actor of a window preview
 * (`DRAGGING_WINDOW_OPACITY` of windowPreview.js).
 */
#define SHELL_WINDOW_PREVIEW_DRAGGING_OPACITY 100

#define SHELL_TYPE_WINDOW_PREVIEW (shell_window_preview_get_type ())
G_DECLARE_FINAL_TYPE (ShellWindowPreview, shell_window_preview,
                      SHELL, WINDOW_PREVIEW, StWidget)

ShellWindowPreview *shell_window_preview_new (MetaWindow      *window,
                                              ShellDragTarget *workspace,
                                              StAdjustment    *overview_adjustment);

ClutterActor *shell_window_preview_get_window_container (ShellWindowPreview *preview);
MetaWindow   *shell_window_preview_get_meta_window      (ShellWindowPreview *preview);

gboolean      shell_window_preview_get_overlay_enabled  (ShellWindowPreview *preview);
void          shell_window_preview_set_overlay_enabled  (ShellWindowPreview *preview,
                                                         gboolean            enabled);

void          shell_window_preview_show_overlay         (ShellWindowPreview *preview,
                                                         gboolean            animate);
void          shell_window_preview_hide_overlay         (ShellWindowPreview *preview,
                                                         gboolean            animate);

void          shell_window_preview_overlap_heights      (ShellWindowPreview *preview,
                                                         double             *top,
                                                         double             *bottom);
void          shell_window_preview_chrome_heights       (ShellWindowPreview *preview,
                                                         double             *top,
                                                         double             *bottom);
void          shell_window_preview_chrome_widths        (ShellWindowPreview *preview,
                                                         double             *left,
                                                         double             *right);

void          shell_window_preview_add_dialog           (ShellWindowPreview *preview,
                                                         MetaWindow         *window);

const ClutterActorBox *shell_window_preview_get_bounding_box (ShellWindowPreview *preview);
void          shell_window_preview_get_window_center    (ShellWindowPreview *preview,
                                                         double             *x,
                                                         double             *y);

void          shell_window_preview_set_stack_above      (ShellWindowPreview *preview,
                                                         ShellWindowPreview *above);

void          shell_window_preview_get_state_transition_params (StAdjustment *adjustment,
                                                                double       *current_state,
                                                                double       *initial_state,
                                                                double       *final_state);

G_END_DECLS
