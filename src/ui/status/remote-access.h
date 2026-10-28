/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * remote-access.h: The indicators of the top bar for screen recording and
 * screen sharing. Port of js/ui/status/remoteAccess.js.
 *
 * #ShellRemoteAccessApplet (`RemoteAccessApplet`) is the privacy icon of the
 * quick settings, #ShellScreenRecordingIndicator and
 * #ShellScreenSharingIndicator are the buttons which stop a recording or a
 * sharing session. The screenshot UI (`Main.screenshotUI`) is not known to
 * the C code: panelItems.js sets the #ShellRemoteAccessApplet:screencast-in-progress
 * and #ShellScreenRecordingIndicator:screencast-in-progress properties from
 * it and connects #ShellScreenRecordingIndicator::stop-screencast-requested.
 */

#pragma once

#include "ui/panel-menu.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellRemoteAccessApplet                                                  */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_REMOTE_ACCESS_APPLET (shell_remote_access_applet_get_type ())
G_DECLARE_FINAL_TYPE (ShellRemoteAccessApplet, shell_remote_access_applet,
                      SHELL, REMOTE_ACCESS_APPLET, ShellSystemIndicator)

ShellRemoteAccessApplet *shell_remote_access_applet_new (void);

/* ------------------------------------------------------------------------ */
/* ShellScreenRecordingIndicator                                            */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREEN_RECORDING_INDICATOR (shell_screen_recording_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenRecordingIndicator, shell_screen_recording_indicator,
                      SHELL, SCREEN_RECORDING_INDICATOR, ShellPanelButtonBox)

ShellScreenRecordingIndicator *shell_screen_recording_indicator_new (void);

/* ------------------------------------------------------------------------ */
/* ShellScreenSharingIndicator                                              */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREEN_SHARING_INDICATOR (shell_screen_sharing_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenSharingIndicator, shell_screen_sharing_indicator,
                      SHELL, SCREEN_SHARING_INDICATOR, ShellPanelButtonBox)

ShellScreenSharingIndicator *shell_screen_sharing_indicator_new (void);

G_END_DECLS
