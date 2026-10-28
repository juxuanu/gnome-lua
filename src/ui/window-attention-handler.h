/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * window-attention-handler.h: Shows a notification when a window asks for
 * attention. Port of js/ui/windowAttentionHandler.js.
 *
 * The source of the notification (`WindowAttentionSource`) is private to
 * window-attention-handler.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

#include "ui/message-tray.h"

G_BEGIN_DECLS

#define SHELL_TYPE_WINDOW_ATTENTION_HANDLER (shell_window_attention_handler_get_type ())
G_DECLARE_FINAL_TYPE (ShellWindowAttentionHandler, shell_window_attention_handler,
                      SHELL, WINDOW_ATTENTION_HANDLER, GObject)

ShellWindowAttentionHandler *shell_window_attention_handler_new (ShellMessageTray *message_tray);

G_END_DECLS
