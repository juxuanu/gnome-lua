/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * input-method.h: The input method of the compositor, backed by an IBus input
 * context. Port of js/misc/inputMethod.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define SHELL_TYPE_INPUT_METHOD (shell_input_method_get_type ())
G_DECLARE_FINAL_TYPE (ShellInputMethod, shell_input_method,
                      SHELL, INPUT_METHOD, ClutterInputMethod)

ShellInputMethod  *shell_input_method_new (void);

ClutterInputFocus *shell_input_method_get_current_focus (ShellInputMethod *self);

gboolean           shell_input_method_get_has_input_source (ShellInputMethod *self);
void               shell_input_method_set_has_input_source (ShellInputMethod *self,
                                                            gboolean          has_input_source);

gboolean           shell_input_method_get_osk_visible (ShellInputMethod *self);
void               shell_input_method_set_osk_visible (ShellInputMethod *self,
                                                       gboolean          osk_visible);

void               shell_input_method_get_surrounding_text (ShellInputMethod  *self,
                                                            char             **text,
                                                            unsigned int      *cursor,
                                                            unsigned int      *anchor);

gboolean           shell_input_method_has_preedit (ShellInputMethod *self);

void               shell_input_method_update (ShellInputMethod *self);

void               shell_input_method_handle_virtual_key_async  (ShellInputMethod    *self,
                                                                 unsigned int         keyval,
                                                                 GCancellable        *cancellable,
                                                                 GAsyncReadyCallback  callback,
                                                                 gpointer             user_data);
gboolean           shell_input_method_handle_virtual_key_finish (ShellInputMethod    *self,
                                                                 GAsyncResult        *res);

G_END_DECLS
