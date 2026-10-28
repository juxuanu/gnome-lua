/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * keyboard.h: The input sources (keyboard layouts and IBus engines) of the
 * session, the manager which switches between them, and the menu of the top
 * bar which lists them. Port of js/ui/status/keyboard.js.
 *
 * #ShellInputSourceManager is the former `InputSourceManager` (and
 * `getInputSourceManager ()` is shell_input_source_manager_get_default ()).
 * It talks to the IBus manager (misc/ibus-manager.h), to the keyboard manager
 * which applies the XKB layouts (misc/keyboard-manager.h) and to the
 * on-screen keyboard (ui/keyboard.h), which reads the input sources straight
 * from it. #ShellInputSourceIndicator is the menu button of the top bar.
 *
 * The switcher popup (`InputSourcePopup` and `InputSourceSwitcher`), the
 * settings classes (`InputSourceSettings` and its two subclasses), the
 * `LayoutMenuItem` and the container of the indicator are private to the C
 * file.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <ibus.h>

#include "misc/keyboard-manager.h"
#include "ui/panel-menu.h"

G_BEGIN_DECLS

/**
 * SHELL_INPUT_SOURCE_TYPE_XKB:
 *
 * The type of an input source which is an XKB layout.
 */
#define SHELL_INPUT_SOURCE_TYPE_XKB "xkb"

/**
 * SHELL_INPUT_SOURCE_TYPE_IBUS:
 *
 * The type of an input source which is an IBus engine.
 */
#define SHELL_INPUT_SOURCE_TYPE_IBUS "ibus"

/**
 * SHELL_INPUT_SOURCE_TYPE_EXTERNAL:
 *
 * The type of the input source the indicator makes up for an external
 * keyboard, which cannot be switched.
 */
#define SHELL_INPUT_SOURCE_TYPE_EXTERNAL "external"

/* ------------------------------------------------------------------------ */
/* ShellInputSource                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_INPUT_SOURCE (shell_input_source_get_type ())
G_DECLARE_FINAL_TYPE (ShellInputSource, shell_input_source,
                      SHELL, INPUT_SOURCE, GObject)

ShellInputSource *shell_input_source_new              (const char       *source_type,
                                                       const char       *id,
                                                       const char       *display_name,
                                                       const char       *short_name,
                                                       int               index);

const char       *shell_input_source_get_source_type  (ShellInputSource  *self);
const char       *shell_input_source_get_id           (ShellInputSource  *self);
const char       *shell_input_source_get_display_name (ShellInputSource  *self);
const char       *shell_input_source_get_short_name   (ShellInputSource  *self);
void              shell_input_source_set_short_name   (ShellInputSource  *self,
                                                       const char       *short_name);
int               shell_input_source_get_index        (ShellInputSource  *self);
const char       *shell_input_source_get_xkb_id       (ShellInputSource  *self);
IBusPropList     *shell_input_source_get_properties   (ShellInputSource  *self);
void              shell_input_source_set_properties   (ShellInputSource  *self,
                                                       IBusPropList     *properties);
gboolean          shell_input_source_get_locked       (ShellInputSource  *self);
void              shell_input_source_set_locked       (ShellInputSource  *self,
                                                       gboolean          locked);

void              shell_input_source_activate         (ShellInputSource  *self,
                                                       gboolean          interactive);

/* ------------------------------------------------------------------------ */
/* ShellInputSourceManager                                                  */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_INPUT_SOURCE_MANAGER (shell_input_source_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellInputSourceManager, shell_input_source_manager,
                      SHELL, INPUT_SOURCE_MANAGER, GObject)

ShellInputSourceManager *shell_input_source_manager_get_default (void);

void                     shell_input_source_manager_reload                (ShellInputSourceManager *self);
void                     shell_input_source_manager_activate_input_source (ShellInputSourceManager *self,
                                                                           ShellInputSource        *source,
                                                                           gboolean                 interactive);

ShellInputSource        *shell_input_source_manager_get_current_source    (ShellInputSourceManager *self);
GPtrArray               *shell_input_source_manager_get_input_sources     (ShellInputSourceManager *self);
ShellKeyboardManager    *shell_input_source_manager_get_keyboard_manager  (ShellInputSourceManager *self);

/* ------------------------------------------------------------------------ */
/* ShellInputSourceIndicator                                                */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_INPUT_SOURCE_INDICATOR (shell_input_source_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellInputSourceIndicator, shell_input_source_indicator,
                      SHELL, INPUT_SOURCE_INDICATOR, ShellPanelMenuButton)

ShellInputSourceIndicator *shell_input_source_indicator_new (void);

G_END_DECLS
