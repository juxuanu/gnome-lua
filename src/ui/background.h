/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * background.h: The desktop background of a monitor. Port of
 * js/ui/background.js.
 *
 * #ShellBackgroundManager is the only object other parts of the shell deal
 * with: it creates the background actor of one monitor, adds it to a
 * container and swaps it for a new one (with a fade) when the user changes the
 * background. #ShellSystemBackground is the plain dark background shown
 * before the real one is loaded.
 *
 * The other objects of the JS file (BackgroundCache, BackgroundSource,
 * Background, the glycin texture cache and the slide show Animation) are
 * private to background.c.
 */

#pragma once

#include <clutter/clutter.h>

G_BEGIN_DECLS

#define SHELL_TYPE_BACKGROUND_MANAGER (shell_background_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellBackgroundManager, shell_background_manager,
                      SHELL, BACKGROUND_MANAGER, GObject)

#define SHELL_TYPE_SYSTEM_BACKGROUND (shell_system_background_get_type ())
G_DECLARE_FINAL_TYPE (ShellSystemBackground, shell_system_background,
                      SHELL, SYSTEM_BACKGROUND, ClutterActor)

ShellBackgroundManager *shell_background_manager_new                   (ClutterActor           *container,
                                                                        int                     monitor_index);

ClutterActor           *shell_background_manager_get_background_actor  (ShellBackgroundManager *self);

void                    shell_background_manager_destroy               (ShellBackgroundManager *self);

void                    shell_background_manager_set_screen_transition (ClutterActor           *screen_transition_actor);

ShellSystemBackground  *shell_system_background_new                    (void);

G_END_DECLS
