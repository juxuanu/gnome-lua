/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-lua-api-menu.h: the `shell.Menu` handle type and its items
 * (docs/api/menu.md) over ShellPopupMenuBase and the items of
 * ui/popup-menu.[ch].
 *
 * Private to libshell: not installed, not scanned for GIR.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <lua.h>
#include <lauxlib.h>

#include "shell-lua-api-private.h"
#include "ui/popup-menu.h"

G_BEGIN_DECLS

/* Handle type names: "shell.Menu" (any ShellPopupMenuBase), "shell.MenuItem"
 * and its subtypes "shell.MenuActionItem", "shell.MenuSwitchItem",
 * "shell.MenuSubmenuItem", "shell.MenuSeparator", "shell.MenuWidgetItem".
 * A module with a menu of its own (shell.QuickMenu) registers a type with the
 * parent "shell.Menu" and pushes it with its own name. */

/* Registers the menu and item handle types in @L (idempotent). */
void shell_lua_api_menu_register_types (lua_State *L);

/* Pushes the "shell.Menu" handle of @menu (or @type_name, a registered
 * subtype of it, if not NULL). The first time a menu is pushed the extension
 * context takes a reference to it (so callbacks can be bound to it and so
 * that its handle dies when the menu is destroyed by whoever owns it: the
 * indicator, the quick settings item, the submenu item) -- the context never
 * destroys a menu itself, the owner of the menu does. */
void shell_lua_api_menu_push (lua_State          *L,
                              const char         *type_name,
                              ShellPopupMenuBase *menu);

G_END_DECLS
