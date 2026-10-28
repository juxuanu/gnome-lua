/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * camera.c: The privacy indicator which shows while a camera is in use.
 * Port of js/ui/status/camera.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "shell-camera-monitor.h"
#include "ui/status/camera.h"

struct _ShellCameraIndicator
{
  ShellSystemIndicator parent;

  ShellCameraMonitor *camera_monitor;
};

G_DEFINE_FINAL_TYPE (ShellCameraIndicator, shell_camera_indicator,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
shell_camera_indicator_constructed (GObject *object)
{
  ShellCameraIndicator *self = SHELL_CAMERA_INDICATOR (object);
  StIcon *icon;

  G_OBJECT_CLASS (shell_camera_indicator_parent_class)->constructed (object);

  icon = shell_system_indicator_add_indicator (SHELL_SYSTEM_INDICATOR (self));
  st_icon_set_icon_name (icon, "camera-web-symbolic");
  st_widget_add_style_class_name (ST_WIDGET (icon), "privacy-indicator");

  self->camera_monitor = g_object_new (SHELL_TYPE_CAMERA_MONITOR, NULL);
  g_object_bind_property (self->camera_monitor, "cameras-in-use",
                          icon, "visible",
                          G_BINDING_SYNC_CREATE);
}

static void
shell_camera_indicator_dispose (GObject *object)
{
  ShellCameraIndicator *self = SHELL_CAMERA_INDICATOR (object);

  g_clear_object (&self->camera_monitor);

  G_OBJECT_CLASS (shell_camera_indicator_parent_class)->dispose (object);
}

static void
shell_camera_indicator_class_init (ShellCameraIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_camera_indicator_constructed;
  object_class->dispose = shell_camera_indicator_dispose;
}

static void
shell_camera_indicator_init (ShellCameraIndicator *self)
{
}

/**
 * shell_camera_indicator_new:
 *
 * Creates the indicator which shows a camera icon while a camera is in use
 * (`new Indicator ()`).
 *
 * Returns: (transfer floating): a new #ShellCameraIndicator
 */
ShellCameraIndicator *
shell_camera_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_CAMERA_INDICATOR, NULL);
}
