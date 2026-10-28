/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-lua-api-ui.h: `shell.ui` (docs/api/ui.md), the widget handle types
 * and the helpers the other modules of src/lua-api use to adopt a widget an
 * extension created (an indicator's child, a menu's widget item, a quick
 * settings header, a dash button's content).
 *
 * Private to libshell: not installed, not scanned for GIR.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <lua.h>
#include <lauxlib.h>

#include "shell-lua-api-private.h"
#include "st.h"

G_BEGIN_DECLS

/* The handle type names: "shell.Widget" is the base of "shell.Box",
 * "shell.Label", "shell.Icon", "shell.Button", "shell.Entry", "shell.Switch",
 * "shell.Slider", "shell.Spacer" and "shell.Scroll". The types are registered
 * in every state by the module (shell_lua_api_register ("ui", ...)). */

/* Registers the widget handle types in @L (idempotent). Other modules call it
 * before pushing a handle of a type that has "shell.Widget" as its parent. */
void          shell_lua_api_ui_register_types (lua_State *L);

/* The ClutterActor behind the shell.Widget handle at @idx; raises if the value
 * is not a widget handle or the handle is dead. */
ClutterActor *shell_lua_api_ui_check_widget      (lua_State *L,
                                                  int        idx);

/* Same, and the widget must not have a parent yet: raises
 * `widget already has a parent; call remove() first`. Use it for every
 * `child = widget` field before adding the actor to a container of yours. */
ClutterActor *shell_lua_api_ui_check_free_widget (lua_State *L,
                                                  int        idx);

/* Pushes the handle of a widget created through shell.ui (with its own type:
 * shell.Label, shell.Spacer, ...), or nil for an actor the extension does not
 * own. */
void          shell_lua_api_ui_push_widget       (lua_State    *L,
                                                  ClutterActor *actor);

/* How an extension-owned ClutterActor is destroyed (clutter_actor_destroy)
 * and how the slot learns about a destruction from elsewhere ("destroy").
 * Pass it to shell_lua_own_object () for every actor you create. */
const ShellLuaOwnOps *shell_lua_api_ui_actor_ops (void);

/* Marks an actor created for the extension of @L: adds the CSS class
 * `lua-ext-<id>` (ui.md "Common properties"). Does nothing for actors that
 * are not St widgets or for states without an extension. */
void          shell_lua_api_ui_decorate          (lua_State   *L,
                                                  ClutterActor *actor);

/* `lua-ext-<id>` for the extension of @L, "" for states without one.
 * (transfer full) */
char         *shell_lua_api_ui_extension_class   (lua_State *L);

G_END_DECLS
