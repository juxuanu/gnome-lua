/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * keyboard.h: The on-screen keyboard. Port of js/ui/keyboard.js.
 *
 * #ShellOskManager is the former `Keyboard.KeyboardManager`, created by
 * main.js (`new Shell.OskManager ({input_method: inputMethod})`) and kept in
 * `Main.keyboard`. It shows or hides the keyboard actor depending on the
 * accessibility setting, the touch mode and the last input device. The
 * actor and its helpers (keys, the emoji panel, the focus tracker and the
 * controller which sends the key events) are private to keyboard.c.
 *
 * The keyboard reads the input sources straight from the input source
 * manager (ui/status/keyboard.h): the layout of the current input source, the
 * input sources of its language menu, and whether the current one is an IBus
 * engine. The manager is created when the keyboard is first created.
 *
 * Hooks declared here: "osk.shown" and "osk.hidden" (docs/api/hooks.md).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>

#include "misc/input-method.h"

G_BEGIN_DECLS

/**
 * ShellOskSuggestionFunc:
 * @user_data: (closure): the user data given along with the callback
 *
 * Called when the user clicks a word added with
 * shell_osk_manager_add_suggestion ().
 */
typedef void (* ShellOskSuggestionFunc) (gpointer user_data);

#define SHELL_TYPE_OSK_MANAGER (shell_osk_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellOskManager, shell_osk_manager,
                      SHELL, OSK_MANAGER, GObject)

ShellOskManager *shell_osk_manager_new                   (ShellInputMethod       *input_method);

ClutterActor    *shell_osk_manager_get_keyboard_actor    (ShellOskManager        *self);
gboolean         shell_osk_manager_get_visible           (ShellOskManager        *self);

void             shell_osk_manager_open                  (ShellOskManager        *self,
                                                          int                     monitor);
void             shell_osk_manager_close                 (ShellOskManager        *self);

void             shell_osk_manager_add_suggestion        (ShellOskManager        *self,
                                                          const char             *text,
                                                          ShellOskSuggestionFunc  callback,
                                                          gpointer                user_data,
                                                          GDestroyNotify          destroy);
void             shell_osk_manager_reset_suggestions     (ShellOskManager        *self);
void             shell_osk_manager_set_suggestions_visible (ShellOskManager      *self,
                                                            gboolean              visible);

G_END_DECLS
