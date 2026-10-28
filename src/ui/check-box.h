/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * check-box.h: A toggle button with a check mark and a wrapping label. Port
 * of js/ui/checkBox.js.
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

#define SHELL_TYPE_CHECK_BOX (shell_check_box_get_type ())
G_DECLARE_FINAL_TYPE (ShellCheckBox, shell_check_box, SHELL, CHECK_BOX, StButton)

ShellCheckBox *shell_check_box_new             (const char    *label);

void           shell_check_box_set_label       (ShellCheckBox *self,
                                                const char    *label);
ClutterActor  *shell_check_box_get_label_actor (ShellCheckBox *self);

G_END_DECLS
