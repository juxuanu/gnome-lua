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
 * The input source manager (js/ui/status/keyboard.js) is still JS and is not
 * a GObject, so it cannot be connected to from C. It pushes its state with
 * shell_osk_manager_set_input_sources () and
 * shell_osk_manager_set_current_input_source (), and the manager asks for it
 * with #ShellOskManager::input-sources-requested before it first creates the
 * keyboard. The language menu of the keyboard asks for a switch of input
 * source with #ShellOskManager::activate-input-source.
 *
 * Hooks declared here: "osk.shown" and "osk.hidden" (docs/api/hooks.md).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>
#include <ibus.h>

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

void             shell_osk_manager_set_input_sources     (ShellOskManager        *self,
                                                          const char * const     *display_names,
                                                          int                     current_index);
void             shell_osk_manager_set_current_input_source (ShellOskManager     *self,
                                                             int                  current_index,
                                                             const char          *id,
                                                             const char          *xkb_id,
                                                             gboolean             is_ibus,
                                                             IBusPropList        *properties);

G_END_DECLS
