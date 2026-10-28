/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * audio-device-selection.h: The dialog which asks the user which audio device
 * was plugged in (headphones, headset or microphone) and the
 * org.gnome.Shell.AudioDeviceSelection D-Bus service which gnome-settings-daemon
 * opens it with. Port of js/ui/audioDeviceSelection.js.
 *
 * The dialog (`AudioDeviceSelectionDialog`) is private to
 * audio-device-selection.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SHELL_TYPE_AUDIO_DEVICE_SELECTION_DBUS (shell_audio_device_selection_dbus_get_type ())
G_DECLARE_FINAL_TYPE (ShellAudioDeviceSelectionDBus, shell_audio_device_selection_dbus,
                      SHELL, AUDIO_DEVICE_SELECTION_DBUS, GObject)

ShellAudioDeviceSelectionDBus *shell_audio_device_selection_dbus_new (void);

G_END_DECLS
