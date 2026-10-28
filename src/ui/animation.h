/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * animation.h: A spinner that fades in and out. Port of js/ui/animation.js.
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

#define SHELL_SPINNER_ANIMATION_TIME 300

#define SHELL_TYPE_SPINNER (shell_spinner_get_type ())
G_DECLARE_FINAL_TYPE (ShellSpinner, shell_spinner, SHELL, SPINNER, StWidget)

ShellSpinner *shell_spinner_new  (int           size,
                                  gboolean      animate,
                                  gboolean      hide_on_stop);

void          shell_spinner_play (ShellSpinner *self);
void          shell_spinner_stop (ShellSpinner *self);

G_END_DECLS
