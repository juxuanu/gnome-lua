/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * pad-osd.h: The on-screen display which shows what the buttons, rings,
 * strips and dials of a tablet pad do, lets the user change it, and the
 * D-Bus service which asks for it. Port of js/ui/padOsd.js.
 *
 * #ShellPadOsd is created by the handler of the MetaDisplay::show-pad-osd
 * signal, which returns it to mutter. The widgets it is made of (the diagram,
 * the pad chooser, the action editor) are private to pad-osd.c.
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>

#include "st.h"

G_BEGIN_DECLS

#define SHELL_TYPE_PAD_OSD (shell_pad_osd_get_type ())
G_DECLARE_FINAL_TYPE (ShellPadOsd, shell_pad_osd, SHELL, PAD_OSD, StBoxLayout)

ShellPadOsd        *shell_pad_osd_new               (ClutterInputDevice *pad,
                                                     GSettings          *settings,
                                                     const char         *image_path,
                                                     gboolean            edition_mode,
                                                     int                 monitor_index);

ClutterInputDevice *shell_pad_osd_get_pad_device    (ShellPadOsd        *self);

gboolean            shell_pad_osd_get_edition_mode  (ShellPadOsd        *self);
void                shell_pad_osd_set_edition_mode  (ShellPadOsd        *self,
                                                     gboolean            edition_mode);

#define SHELL_TYPE_PAD_OSD_SERVICE (shell_pad_osd_service_get_type ())
G_DECLARE_FINAL_TYPE (ShellPadOsdService, shell_pad_osd_service,
                      SHELL, PAD_OSD_SERVICE, GObject)

ShellPadOsdService *shell_pad_osd_service_new       (void);

G_END_DECLS
