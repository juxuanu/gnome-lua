/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * looking-glass.h: Looking Glass, the integrated debugger and inspector of the
 * shell (Alt+F2, `lg`). Port of js/ui/lookingGlass.js.
 *
 * The Evaluator is a Lua prompt on the privileged state of
 * #ShellLuaInspector (docs/looking-glass.md); the rest (windows, Lua
 * extensions, the actor tree, debug flags, the picker and the object
 * inspector) is private to looking-glass.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

/**
 * SHELL_LOOKING_GLASS_ANIMATION_TIME:
 *
 * Duration in milliseconds of the slide of Looking Glass
 * (`LookingGlass.LG_ANIMATION_TIME`).
 */
#define SHELL_LOOKING_GLASS_ANIMATION_TIME 500

#define SHELL_TYPE_LOOKING_GLASS (shell_looking_glass_get_type ())
G_DECLARE_FINAL_TYPE (ShellLookingGlass, shell_looking_glass,
                      SHELL, LOOKING_GLASS, StBoxLayout)

ShellLookingGlass *shell_looking_glass_new     (void);

void               shell_looking_glass_open    (ShellLookingGlass *self);
void               shell_looking_glass_close   (ShellLookingGlass *self);
void               shell_looking_glass_toggle  (ShellLookingGlass *self);

gboolean           shell_looking_glass_get_is_open (ShellLookingGlass *self);

G_END_DECLS
