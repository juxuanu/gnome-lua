/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * extension-downloader.h: Installs extensions from extensions.gnome.org and
 * looks for their updates. Port of js/ui/extensionDownloader.js.
 *
 * #ShellExtensionManager creates the only instance and exposes its
 * operations (shell_extension_manager_install_extension_async () and
 * shell_extension_manager_check_for_updates_async ()), whose tasks have the
 * manager as their source object. Not in the GIR.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "ui/extension-system.h"

G_BEGIN_DECLS

#define SHELL_TYPE_EXTENSION_DOWNLOADER (shell_extension_downloader_get_type ())
G_DECLARE_FINAL_TYPE (ShellExtensionDownloader, shell_extension_downloader,
                      SHELL, EXTENSION_DOWNLOADER, GObject)

ShellExtensionDownloader *shell_extension_downloader_new (ShellExtensionManager *manager);

void      shell_extension_downloader_install_async  (ShellExtensionDownloader *self,
                                                     const char               *uuid,
                                                     GCancellable             *cancellable,
                                                     GAsyncReadyCallback       callback,
                                                     gpointer                  user_data);
char     *shell_extension_downloader_install_finish (ShellExtensionDownloader  *self,
                                                     GAsyncResult              *result,
                                                     GError                   **error);

void      shell_extension_downloader_check_for_updates_async  (ShellExtensionDownloader *self,
                                                               GCancellable             *cancellable,
                                                               GAsyncReadyCallback       callback,
                                                               gpointer                  user_data);
gboolean  shell_extension_downloader_check_for_updates_finish (ShellExtensionDownloader  *self,
                                                               GAsyncResult              *result,
                                                               GError                   **error);

G_END_DECLS
