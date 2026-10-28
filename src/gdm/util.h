/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * util.h: Helpers of the login screen. Port of js/gdm/util.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>

#include "gdm/batch.h"

G_BEGIN_DECLS

/**
 * SHELL_GDM_CLONE_FADE_ANIMATION_TIME:
 *
 * Duration in milliseconds of the fade of shell_gdm_clone_and_fade_out_actor ()
 * (`Shell.GDM_CLONE_FADE_ANIMATION_TIME`).
 */
#define SHELL_GDM_CLONE_FADE_ANIMATION_TIME 250

ShellBatchHold *shell_gdm_clone_and_fade_out_actor (ClutterActor *actor);

G_END_DECLS
