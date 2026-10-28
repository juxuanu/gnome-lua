/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * main.h: The startup and the singletons of the shell, and the modal stack
 * (`Main.pushModal ()` and `Main.popModal ()`) with the action mode derived
 * from it. Port of src/ui/main.c and of the startup part of js/ui/init.js.
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>
#include <meta/window.h>

#include "shell-action-modes.h"
#include "misc/brightness-manager.h"
#include "misc/input-method.h"
#include "ui/ctrl-alt-tab.h"
#include "ui/extension-system.h"
#include "ui/keyboard.h"
#include "ui/layout.h"
#include "ui/looking-glass.h"
#include "ui/message-tray.h"
#include "ui/osd-window.h"
#include "ui/overview.h"
#include "ui/panel.h"
#include "ui/screen-shield.h"
#include "ui/screenshot-ui.h"
#include "ui/session-mode.h"
#include "ui/window-manager.h"
#include "ui/xdnd-handler.h"

G_BEGIN_DECLS

#define SHELL_TYPE_MAIN (shell_main_get_type ())
G_DECLARE_FINAL_TYPE (ShellMain, shell_main, SHELL, MAIN, GObject)

ShellMain       *shell_main_get_default          (void);

ClutterGrab     *shell_main_push_modal           (ShellMain       *self,
                                                  ClutterActor    *actor,
                                                  ShellActionMode  action_mode);
gboolean         shell_main_pop_modal            (ShellMain       *self,
                                                  ClutterGrab     *grab,
                                                  GError         **error);

guint            shell_main_get_modal_count      (ShellMain       *self);

ShellActionMode  shell_main_get_action_mode      (ShellMain       *self);
void             shell_main_set_action_mode      (ShellMain       *self,
                                                  ShellActionMode  action_mode);

ShellSessionMode *shell_main_get_session_mode    (ShellMain        *self);
void             shell_main_set_session_mode     (ShellMain        *self,
                                                  ShellSessionMode *session_mode);

ShellLayoutManager *shell_main_get_layout_manager (ShellMain          *self);
void             shell_main_set_layout_manager   (ShellMain          *self,
                                                  ShellLayoutManager *layout_manager);

ShellOsdWindowManager *shell_main_get_osd_window_manager (ShellMain             *self);
void             shell_main_set_osd_window_manager (ShellMain             *self,
                                                    ShellOsdWindowManager *osd_window_manager);

ShellPanel      *shell_main_get_panel            (ShellMain       *self);
void             shell_main_set_panel            (ShellMain       *self,
                                                  ShellPanel      *panel);

ShellMessageTray *shell_main_get_message_tray    (ShellMain        *self);
void             shell_main_set_message_tray     (ShellMain        *self,
                                                  ShellMessageTray *message_tray);

ShellWindowManager *shell_main_get_window_manager (ShellMain          *self);
void             shell_main_set_window_manager   (ShellMain          *self,
                                                  ShellWindowManager *window_manager);

ShellInputMethod *shell_main_get_input_method    (ShellMain        *self);
void             shell_main_set_input_method     (ShellMain        *self,
                                                  ShellInputMethod *input_method);

ShellOskManager *shell_main_get_keyboard         (ShellMain       *self);
void             shell_main_set_keyboard         (ShellMain       *self,
                                                  ShellOskManager *keyboard);

ShellOverview   *shell_main_get_overview         (ShellMain       *self);
void             shell_main_set_overview         (ShellMain       *self,
                                                  ShellOverview   *overview);

ShellXdndHandler *shell_main_get_xdnd_handler    (ShellMain       *self);
ShellCtrlAltTabManager *shell_main_get_ctrl_alt_tab_manager (ShellMain *self);
ShellScreenshotUI *shell_main_get_screenshot_ui  (ShellMain       *self);
ShellBrightnessManager *shell_main_get_brightness_manager (ShellMain *self);
ShellScreenShield *shell_main_get_screen_shield  (ShellMain       *self);
ShellExtensionManager *shell_main_get_extension_manager (ShellMain *self);

void             shell_main_system_modal_opened  (ShellMain       *self);
void             shell_main_emit_settings_app_activating (ShellMain *self);

gboolean         shell_main_start                (ShellMain       *self,
                                                  GError         **error);
void             shell_main_shutdown             (ShellMain       *self);

void             shell_main_send_notification    (ShellMain       *self,
                                                  const char      *msg,
                                                  const char      *details);
void             shell_main_notify_error         (ShellMain       *self,
                                                  const char      *msg,
                                                  const char      *details);

ShellLookingGlass *shell_main_create_looking_glass (ShellMain     *self);
void             shell_main_open_run_dialog      (ShellMain       *self);
void             shell_main_open_welcome_dialog  (ShellMain       *self);

void             shell_main_activate_window      (ShellMain       *self,
                                                  MetaWindow      *window,
                                                  guint32          time,
                                                  int              workspace_index);
void             shell_main_move_window_to_monitor_and_workspace (ShellMain  *self,
                                                                  MetaWindow *window,
                                                                  int         monitor_index,
                                                                  int         workspace_index,
                                                                  gboolean    append);

gboolean         shell_main_load_theme           (ShellMain       *self,
                                                  GError         **error);
gboolean         shell_main_reload_theme_resource (ShellMain      *self,
                                                   GError        **error);
GFile           *shell_main_get_theme_stylesheet (ShellMain       *self);
void             shell_main_set_theme_stylesheet (ShellMain       *self,
                                                  const char      *css_stylesheet);
const char      *shell_main_get_style_variant    (ShellMain       *self);

G_END_DECLS
