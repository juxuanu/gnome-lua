/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-entry.h: The context menu of text entries (copy, paste and, for
 * password entries, show or hide the text) and the label which warns that
 * caps lock is on. Port of js/ui/shellEntry.js.
 */

#pragma once

#include "st.h"

#include "shell-action-modes.h"
#include "ui/popup-menu.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellEntryMenu                                                           */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_ENTRY_MENU (shell_entry_menu_get_type ())
G_DECLARE_FINAL_TYPE (ShellEntryMenu, shell_entry_menu,
                      SHELL, ENTRY_MENU, ShellPopupMenu)

ShellEntryMenu *shell_entry_menu_new             (StEntry        *entry,
                                                  gboolean        is_password);
gboolean        shell_entry_menu_get_is_password (ShellEntryMenu *self);

/* ------------------------------------------------------------------------ */
/* Context menus of entries                                                 */
/* ------------------------------------------------------------------------ */

void            shell_entry_add_context_menu     (StEntry        *entry,
                                                  ShellActionMode action_mode);
ShellEntryMenu *shell_entry_get_menu             (StEntry        *entry);

/* ------------------------------------------------------------------------ */
/* ShellCapsLockWarning                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_CAPS_LOCK_WARNING (shell_caps_lock_warning_get_type ())
G_DECLARE_FINAL_TYPE (ShellCapsLockWarning, shell_caps_lock_warning,
                      SHELL, CAPS_LOCK_WARNING, StLabel)

ShellCapsLockWarning *shell_caps_lock_warning_new (void);

G_END_DECLS
