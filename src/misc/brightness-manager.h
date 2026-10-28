/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * brightness-manager.h: The screen brightness of the monitors that have a
 * backlight, as one global scale plus one scale per monitor. Port of
 * js/misc/brightnessManager.js.
 *
 * The OSD is not shown by the manager: js/ui/main.js connects
 * #ShellBrightnessManager::osd-requested to the OSD window manager, and calls
 * Main.wm.allowKeybinding () for the names of
 * shell_brightness_manager_get_keybinding_names ().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define SHELL_TYPE_BRIGHTNESS_SCALE (shell_brightness_scale_get_type ())
G_DECLARE_FINAL_TYPE (ShellBrightnessScale, shell_brightness_scale,
                      SHELL, BRIGHTNESS_SCALE, GObject)

const char *shell_brightness_scale_get_name     (ShellBrightnessScale *self);
double      shell_brightness_scale_get_value    (ShellBrightnessScale *self);
void        shell_brightness_scale_set_value    (ShellBrightnessScale *self,
                                                 double                value);
int         shell_brightness_scale_get_n_steps  (ShellBrightnessScale *self);

void        shell_brightness_scale_step_up      (ShellBrightnessScale *self);
void        shell_brightness_scale_step_down    (ShellBrightnessScale *self);
void        shell_brightness_scale_cycle_up     (ShellBrightnessScale *self);
void        shell_brightness_scale_destroy      (ShellBrightnessScale *self);


#define SHELL_TYPE_BRIGHTNESS_MANAGER (shell_brightness_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellBrightnessManager, shell_brightness_manager,
                      SHELL, BRIGHTNESS_MANAGER, GObject)

ShellBrightnessManager *shell_brightness_manager_new                (void);

const char * const     *shell_brightness_manager_get_keybinding_names (void);

gboolean                shell_brightness_manager_get_dimming        (ShellBrightnessManager *self);
void                    shell_brightness_manager_set_dimming        (ShellBrightnessManager *self,
                                                                     gboolean                dimming);
double                  shell_brightness_manager_get_auto_brightness_target (ShellBrightnessManager *self);
void                    shell_brightness_manager_set_auto_brightness_target (ShellBrightnessManager *self,
                                                                             double                  target);
ShellBrightnessScale   *shell_brightness_manager_get_global_scale   (ShellBrightnessManager *self);
GList                  *shell_brightness_manager_get_scales         (ShellBrightnessManager *self);

G_END_DECLS
