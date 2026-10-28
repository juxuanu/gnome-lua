/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * ripples.h: Three concentric ripples expanding outwards, used by the hot
 * corner and the pointer locator. Port of js/ui/ripples.js.
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

#define SHELL_TYPE_RIPPLES (shell_ripples_get_type ())
G_DECLARE_FINAL_TYPE (ShellRipples, shell_ripples, SHELL, RIPPLES, GObject)

ShellRipples *shell_ripples_new            (double        px,
                                            double        py,
                                            const char   *style_class,
                                            gboolean      animation_required);

void          shell_ripples_destroy        (ShellRipples *self);

void          shell_ripples_add_to         (ShellRipples *self,
                                            ClutterActor *stage);

void          shell_ripples_play_animation (ShellRipples *self,
                                            double        x,
                                            double        y);

G_END_DECLS
