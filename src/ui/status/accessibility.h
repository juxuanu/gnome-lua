/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * accessibility.h: The accessibility menu of the top bar and the quick
 * settings toggles for the accessibility settings. Port of
 * js/ui/status/accessibility.js.
 *
 * #ShellATIndicator (`ATIndicator`) is the menu button of the top bar. The
 * toggles (`HighContrastToggle`, ..., `MouseKeysToggle`) are public because
 * the menu of the login screen (src/gdm/login-dialog.c) puts them in its own
 * menu; they all are final #ShellQuickToggle subclasses which bind their
 * `checked` property to a GSettings key.
 */

#pragma once

#include "ui/panel-menu.h"
#include "ui/quick-settings.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellATIndicator                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_AT_INDICATOR (shell_at_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellATIndicator, shell_at_indicator,
                      SHELL, AT_INDICATOR, ShellPanelMenuButton)

ShellATIndicator *shell_at_indicator_new (void);

/* ------------------------------------------------------------------------ */
/* ShellHighContrastToggle                                                  */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_HIGH_CONTRAST_TOGGLE (shell_high_contrast_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellHighContrastToggle, shell_high_contrast_toggle,
                      SHELL, HIGH_CONTRAST_TOGGLE, ShellQuickToggle)

ShellHighContrastToggle *shell_high_contrast_toggle_new (void);

/* ------------------------------------------------------------------------ */
/* ShellMagnifierToggle                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MAGNIFIER_TOGGLE (shell_magnifier_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellMagnifierToggle, shell_magnifier_toggle,
                      SHELL, MAGNIFIER_TOGGLE, ShellQuickToggle)

ShellMagnifierToggle *shell_magnifier_toggle_new (void);

/* ------------------------------------------------------------------------ */
/* ShellLargeTextToggle                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LARGE_TEXT_TOGGLE (shell_large_text_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellLargeTextToggle, shell_large_text_toggle,
                      SHELL, LARGE_TEXT_TOGGLE, ShellQuickToggle)

ShellLargeTextToggle *shell_large_text_toggle_new (void);

/* ------------------------------------------------------------------------ */
/* ShellScreenReaderToggle                                                  */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREEN_READER_TOGGLE (shell_screen_reader_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenReaderToggle, shell_screen_reader_toggle,
                      SHELL, SCREEN_READER_TOGGLE, ShellQuickToggle)

ShellScreenReaderToggle *shell_screen_reader_toggle_new (void);

/* ------------------------------------------------------------------------ */
/* ShellScreenKeyboardToggle                                                */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREEN_KEYBOARD_TOGGLE (shell_screen_keyboard_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenKeyboardToggle, shell_screen_keyboard_toggle,
                      SHELL, SCREEN_KEYBOARD_TOGGLE, ShellQuickToggle)

ShellScreenKeyboardToggle *shell_screen_keyboard_toggle_new (void);

/* ------------------------------------------------------------------------ */
/* ShellVisualBellToggle                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_VISUAL_BELL_TOGGLE (shell_visual_bell_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellVisualBellToggle, shell_visual_bell_toggle,
                      SHELL, VISUAL_BELL_TOGGLE, ShellQuickToggle)

ShellVisualBellToggle *shell_visual_bell_toggle_new (void);

/* ------------------------------------------------------------------------ */
/* ShellStickyKeysToggle                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_STICKY_KEYS_TOGGLE (shell_sticky_keys_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellStickyKeysToggle, shell_sticky_keys_toggle,
                      SHELL, STICKY_KEYS_TOGGLE, ShellQuickToggle)

ShellStickyKeysToggle *shell_sticky_keys_toggle_new (void);

/* ------------------------------------------------------------------------ */
/* ShellSlowKeysToggle                                                      */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SLOW_KEYS_TOGGLE (shell_slow_keys_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellSlowKeysToggle, shell_slow_keys_toggle,
                      SHELL, SLOW_KEYS_TOGGLE, ShellQuickToggle)

ShellSlowKeysToggle *shell_slow_keys_toggle_new (void);

/* ------------------------------------------------------------------------ */
/* ShellBounceKeysToggle                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_BOUNCE_KEYS_TOGGLE (shell_bounce_keys_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellBounceKeysToggle, shell_bounce_keys_toggle,
                      SHELL, BOUNCE_KEYS_TOGGLE, ShellQuickToggle)

ShellBounceKeysToggle *shell_bounce_keys_toggle_new (void);

/* ------------------------------------------------------------------------ */
/* ShellMouseKeysToggle                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MOUSE_KEYS_TOGGLE (shell_mouse_keys_toggle_get_type ())
G_DECLARE_FINAL_TYPE (ShellMouseKeysToggle, shell_mouse_keys_toggle,
                      SHELL, MOUSE_KEYS_TOGGLE, ShellQuickToggle)

ShellMouseKeysToggle *shell_mouse_keys_toggle_new (void);

G_END_DECLS
