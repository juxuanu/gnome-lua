/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * slider.h: A draggable, focusable bar level with a handle. Port of
 * js/ui/slider.js.
 */

#pragma once

#include "ui/bar-level.h"

G_BEGIN_DECLS

#define SHELL_TYPE_SLIDER (shell_slider_get_type ())
G_DECLARE_FINAL_TYPE (ShellSlider, shell_slider, SHELL, SLIDER, ShellBarLevel)

ShellSlider *shell_slider_new         (double        value);

void         shell_slider_add_mark    (ShellSlider  *self,
                                       double        value);
void         shell_slider_clear_marks (ShellSlider  *self);

gboolean     shell_slider_step        (ShellSlider  *self,
                                       double        n_steps);

G_END_DECLS
