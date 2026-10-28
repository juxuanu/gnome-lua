/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * panel-items.h: The indicators of the shell itself in the top bar: the
 * activities button, the quick settings button, the date menu, the
 * accessibility, keyboard and dwell click indicators and the screen recording
 * and sharing indicators. Port of js/ui/panelItems.js.
 *
 * The panel (src/ui/panel.c) asks for an indicator through its
 * #ShellPanel::create-indicator signal when the session mode lists a role
 * that has no indicator yet; #ShellMain connects the signal to
 * shell_panel_item_create (). The classes of the activities button and of
 * the quick settings button are private to panel-items.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

#include "ui/panel-menu.h"
#include "ui/panel.h"

G_BEGIN_DECLS

ShellPanelButtonBox *shell_panel_item_create (const char *role);

gboolean             shell_panel_items_add_external_indicator (ShellPanel           *panel,
                                                               ShellSystemIndicator *indicator,
                                                               int                   col_span);

G_END_DECLS
