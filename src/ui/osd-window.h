/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * osd-window.h: The on-screen display shown when the volume or brightness
 * changes, one per monitor. Port of js/ui/osdWindow.js.
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

#define SHELL_TYPE_OSD_WINDOW (shell_osd_window_get_type ())
G_DECLARE_FINAL_TYPE (ShellOsdWindow, shell_osd_window,
                      SHELL, OSD_WINDOW, StWidget)

#define SHELL_TYPE_OSD_WINDOW_MANAGER (shell_osd_window_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellOsdWindowManager, shell_osd_window_manager,
                      SHELL, OSD_WINDOW_MANAGER, GObject)

ShellOsdWindow *shell_osd_window_new               (int             monitor_index);

int             shell_osd_window_get_monitor_index (ShellOsdWindow *self);

void            shell_osd_window_set_icon          (ShellOsdWindow *self,
                                                    GIcon          *icon);
void            shell_osd_window_set_label         (ShellOsdWindow *self,
                                                    const char     *label);
void            shell_osd_window_set_level         (ShellOsdWindow *self,
                                                    double          level);
void            shell_osd_window_set_max_level     (ShellOsdWindow *self,
                                                    double          max_level);

void            shell_osd_window_show_osd          (ShellOsdWindow *self);
void            shell_osd_window_cancel            (ShellOsdWindow *self);


ShellOsdWindowManager *shell_osd_window_manager_new      (ClutterActor          *ui_group);

void                   shell_osd_window_manager_show     (ShellOsdWindowManager *self,
                                                          GIcon                 *icon,
                                                          const char            *label,
                                                          GVariant              *levels);
void                   shell_osd_window_manager_show_one (ShellOsdWindowManager *self,
                                                          int                    monitor_index,
                                                          GIcon                 *icon,
                                                          const char            *label,
                                                          double                 level,
                                                          double                 max_level);
void                   shell_osd_window_manager_show_all (ShellOsdWindowManager *self,
                                                          GIcon                 *icon,
                                                          const char            *label,
                                                          double                 level,
                                                          double                 max_level);
void                   shell_osd_window_manager_hide_all (ShellOsdWindowManager *self);

G_END_DECLS
