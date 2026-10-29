/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The `time.timezone-changed` hook of shell.time (docs/api/core.md,
 * docs/api/hooks.md): the shell declares it and starts the watch once, from
 * shell_global_declare_hooks (). Not part of the Lua API of an extension.
 */
#pragma once

#include <glib.h>

#include "shell-hooks.h"

G_BEGIN_DECLS

/* Declares the hook "time.timezone-changed" (payload `timezone:s`) in
 * @registry and watches /etc/localtime and /etc/timezone with GFileMonitor:
 * whenever one of them changes and the identifier of the local zone
 * (shell.time.timezone ()) differs from the last one seen, the hook is emitted
 * with the new identifier. Idempotent while a watch is running. */
void shell_lua_time_watch       (ShellHookRegistry   *registry);

/* The same on other files (tests). @paths is NULL-terminated. */
void shell_lua_time_watch_paths (ShellHookRegistry   *registry,
                                 const char * const  *paths);

/* Stops the watch (the hook stays declared). */
void shell_lua_time_unwatch     (void);

G_END_DECLS
