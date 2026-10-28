/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * extension-system-private.h: What extension-system.c and
 * extension-downloader.c share. Not installed, not in the GIR.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "ui/extension-system.h"

G_BEGIN_DECLS

GSettings *shell_extension_manager_get_settings   (ShellExtensionManager *self);

/* The directory of the per-user data of the shell (`global.userdatadir`) */
char      *shell_extension_manager_dup_userdatadir (ShellExtensionManager *self);

gboolean   shell_extension_manager_has_update     (ShellExtensionManager *self,
                                                   const char            *uuid);

/* An update of @uuid was downloaded: tell the user (`notifyExtensionUpdate ()`) */
void       shell_extension_manager_notify_extension_update (ShellExtensionManager *self,
                                                            const char            *uuid);

/* The archive of @uuid was extracted into the per-user extensions directory:
 * load it (or load it again if it was installed already) and enable it. */
gboolean   shell_extension_manager_add_installed  (ShellExtensionManager  *self,
                                                   const char             *uuid,
                                                   GError                **error);

/* Remove the files of an installed extension and forget it */
gboolean   shell_extension_manager_remove_installed (ShellExtensionManager *self,
                                                     const char            *uuid);

/* js/misc/fileUtils.js recursivelyDeleteDir () and recursivelyMoveDir () */
gboolean   shell_extension_recursively_delete_dir (GFile   *dir,
                                                   gboolean delete_parent,
                                                   GError **error);
gboolean   shell_extension_recursively_move_dir   (GFile   *source,
                                                   GFile   *destination,
                                                   GError **error);

G_END_DECLS
