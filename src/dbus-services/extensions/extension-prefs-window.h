/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * extension-prefs-window.h: The window of the preferences of a Lua extension.
 * It renders the declarative preferences of `prefs.lua` (docs/api/prefs.md)
 * with libadwaita rows bound to the GSettings of the extension, or an error
 * page when the file is missing or invalid. Replaces
 * js/dbusServices/extensions/extensionPrefsDialog.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "extension-prefs-schema.h"

G_BEGIN_DECLS

#define EXT_TYPE_PREFS_WINDOW (ext_prefs_window_get_type ())
G_DECLARE_FINAL_TYPE (ExtPrefsWindow, ext_prefs_window, EXT, PREFS_WINDOW, AdwWindow)

ExtPrefsWindow *ext_prefs_window_new (ShellLuaExtension *extension);

G_END_DECLS
