/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * camera.h: The privacy indicator which shows while a camera is in use.
 * Port of js/ui/status/camera.js.
 */

#pragma once

#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_CAMERA_INDICATOR (shell_camera_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellCameraIndicator, shell_camera_indicator,
                      SHELL, CAMERA_INDICATOR, ShellSystemIndicator)

ShellCameraIndicator *shell_camera_indicator_new (void);

G_END_DECLS
