/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * ibus-manager.h: The connection of the shell to the IBus daemon: spawning
 * it, the list of engines, the panel service and the active engine. Port of
 * js/misc/ibusManager.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>
#include <ibus.h>

G_BEGIN_DECLS

#define SHELL_TYPE_IBUS_MANAGER (shell_ibus_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellIBusManager, shell_ibus_manager,
                      SHELL, IBUS_MANAGER, GObject)

ShellIBusManager *shell_ibus_manager_get_default (void);

IBusPanelService *shell_ibus_manager_get_panel_service (ShellIBusManager *self);
gboolean          shell_ibus_manager_get_ready         (ShellIBusManager *self);

void              shell_ibus_manager_restart_daemon (ShellIBusManager  *self,
                                                     const char * const *extra_args);

void              shell_ibus_manager_activate_property (ShellIBusManager *self,
                                                        const char       *key,
                                                        unsigned int      state);

IBusEngineDesc   *shell_ibus_manager_get_engine_desc (ShellIBusManager *self,
                                                      const char       *id);

void              shell_ibus_manager_set_engine      (ShellIBusManager *self,
                                                      const char       *id);

void              shell_ibus_manager_preload_engines (ShellIBusManager  *self,
                                                      const char * const *ids);

void              shell_ibus_manager_set_completion_enabled_async  (ShellIBusManager    *self,
                                                                    gboolean             enabled,
                                                                    GCancellable        *cancellable,
                                                                    GAsyncReadyCallback  callback,
                                                                    gpointer             user_data);
gboolean          shell_ibus_manager_set_completion_enabled_finish (ShellIBusManager    *self,
                                                                    GAsyncResult        *res);

G_END_DECLS
