/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
#pragma once

#include "shell-global.h"

#include "shell-app-cache-private.h"

void _shell_global_init            (const char *first_property_name,
                                    ...);
void _shell_global_set_plugin      (ShellGlobal  *global,
                                    MetaPlugin   *plugin);

ShellAppCache * shell_global_get_app_cache (ShellGlobal *global);

void _shell_global_locate_pointer (ShellGlobal  *global);

void _shell_global_notify_shutdown (ShellGlobal *global);

/* Lua extensions (gnome-lua): disable every Lua extension and drop the
 * inspector. Idempotent; must run before the UI is destroyed. */
void _shell_global_shutdown_lua (ShellGlobal *global);
