/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * keyboard-manager.h: Applies the XKB layouts of the input sources to the
 * compositor keymap. Port of js/misc/keyboardManager.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#ifndef GNOME_DESKTOP_USE_UNSTABLE_API
#define GNOME_DESKTOP_USE_UNSTABLE_API
#endif
#include <libgnome-desktop/gnome-xkb-info.h>

G_BEGIN_DECLS

/**
 * SHELL_KEYBOARD_MANAGER_DEFAULT_LOCALE:
 *
 * The locale used when the user locale does not name a country.
 */
#define SHELL_KEYBOARD_MANAGER_DEFAULT_LOCALE "en_US"

/**
 * SHELL_KEYBOARD_MANAGER_DEFAULT_LAYOUT:
 *
 * The XKB layout (and input source id) used when no input source is
 * configured.
 */
#define SHELL_KEYBOARD_MANAGER_DEFAULT_LAYOUT "us"

/**
 * SHELL_KEYBOARD_MANAGER_DEFAULT_VARIANT:
 *
 * The XKB variant of the default layout.
 */
#define SHELL_KEYBOARD_MANAGER_DEFAULT_VARIANT ""

/**
 * SHELL_KEYBOARD_MANAGER_MAX_LAYOUTS_PER_GROUP:
 *
 * The XKB protocol does not allow for more than 4 layouts in a keymap.
 * Wayland does not impose this limit and libxkbcommon can handle up to 32
 * layouts, but X clients have to be supported as well.
 */
#define SHELL_KEYBOARD_MANAGER_MAX_LAYOUTS_PER_GROUP 4

#define SHELL_TYPE_KEYBOARD_MANAGER (shell_keyboard_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellKeyboardManager, shell_keyboard_manager,
                      SHELL, KEYBOARD_MANAGER, GObject)

ShellKeyboardManager *shell_keyboard_manager_get_default (void);

GnomeXkbInfo         *shell_keyboard_manager_get_xkb_info (ShellKeyboardManager *self);

void                  shell_keyboard_manager_apply        (ShellKeyboardManager *self,
                                                           const char           *id);
void                  shell_keyboard_manager_reapply      (ShellKeyboardManager *self);

void                  shell_keyboard_manager_set_user_layouts (ShellKeyboardManager *self,
                                                               const char * const   *ids);

void                  shell_keyboard_manager_set_keyboard_options (ShellKeyboardManager *self,
                                                                   const char * const   *options);
void                  shell_keyboard_manager_set_keyboard_model   (ShellKeyboardManager *self,
                                                                   const char           *model);

const char           *shell_keyboard_manager_get_current_layout_id (ShellKeyboardManager *self);

const char           *shell_keyboard_manager_get_short_name   (ShellKeyboardManager *self);
const char           *shell_keyboard_manager_get_display_name (ShellKeyboardManager *self);

gboolean              shell_keyboard_manager_is_locked   (ShellKeyboardManager *self);
gboolean              shell_keyboard_manager_is_external (ShellKeyboardManager *self);

G_END_DECLS
