/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * history.h: The history of a text entry, navigated with the Up and Down
 * keys and optionally stored in a GSettings key of the shell's settings.
 * Port of js/misc/history.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <clutter/clutter.h>
#include <clutter/clutter-pango.h>

G_BEGIN_DECLS

#define SHELL_TYPE_HISTORY_MANAGER (shell_history_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellHistoryManager, shell_history_manager,
                      SHELL, HISTORY_MANAGER, GObject)

ShellHistoryManager *shell_history_manager_new       (const char          *gsettings_key,
                                                      ClutterText         *entry);

char                *shell_history_manager_add_item  (ShellHistoryManager *self,
                                                      const char          *input);
const char          *shell_history_manager_last_item (ShellHistoryManager *self);

G_END_DECLS
