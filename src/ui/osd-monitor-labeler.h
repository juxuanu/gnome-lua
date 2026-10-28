/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * osd-monitor-labeler.h: Shows a large label on each monitor, so that the
 * user can tell which is which when configuring displays. Driven over D-Bus
 * by org.gnome.Shell.ShowMonitorLabels (). Port of js/ui/osdMonitorLabeler.js.
 */

#pragma once

#include <clutter/clutter.h>

G_BEGIN_DECLS

#define SHELL_TYPE_OSD_MONITOR_LABELER (shell_osd_monitor_labeler_get_type ())
G_DECLARE_FINAL_TYPE (ShellOsdMonitorLabeler, shell_osd_monitor_labeler,
                      SHELL, OSD_MONITOR_LABELER, GObject)

ShellOsdMonitorLabeler *shell_osd_monitor_labeler_new  (ClutterActor           *ui_group);

void                    shell_osd_monitor_labeler_show (ShellOsdMonitorLabeler *self,
                                                        const char             *client,
                                                        GVariant               *params);
void                    shell_osd_monitor_labeler_hide (ShellOsdMonitorLabeler *self,
                                                        const char             *client);

G_END_DECLS
