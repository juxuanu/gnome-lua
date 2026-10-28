/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * extension-system.h: The extension manager of the shell: which extensions
 * are enabled, when they run, how they are installed, updated and removed.
 * Port of js/ui/extensionSystem.js (and of the policy of
 * js/ui/extensionDownloader.js, which is extension-downloader.c).
 *
 * The extensions are Lua extensions: the runtime, the states and the error
 * policy are those of #ShellLuaExtensionManager (subprojects/shell-lua), which
 * this object drives. What it adds is what depends on the shell:
 *
 *  - the org.gnome.shell settings `enabled-extensions`, `disabled-extensions`,
 *    `disable-user-extensions` and `allow-extension-installation`, and the
 *    extensions that the session mode enables on its own;
 *  - the session modes an extension declares in its metadata (`session-modes`);
 *  - the stylesheet of an extension, loaded into the theme while it runs;
 *  - the installation, update and removal of extensions, and the
 *    notification that updates are available;
 *  - the answers of the org.gnome.Shell.Extensions D-Bus interface, which
 *    #ShellDBusExtensionsService (shell-dbus.c) takes from here.
 *
 * js/ui/main.js creates the only instance (`Main.extensionManager`).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "shell-lua-extension.h"
#include "ui/message-tray.h"
#include "ui/session-mode.h"

G_BEGIN_DECLS

/**
 * ShellExtensionError:
 * @SHELL_EXTENSION_ERROR_INFO_DOWNLOAD_FAILED: the description of the extension
 *   could not be downloaded
 * @SHELL_EXTENSION_ERROR_DOWNLOAD_FAILED: the archive of the extension could
 *   not be downloaded
 * @SHELL_EXTENSION_ERROR_EXTRACT_FAILED: the archive could not be extracted
 * @SHELL_EXTENSION_ERROR_ENABLE_FAILED: the extension was installed but could
 *   not be enabled
 * @SHELL_EXTENSION_ERROR_NOT_ALLOWED: installing extensions is not allowed
 *
 * The errors of installing an extension. Same values and D-Bus names
 * (`org.gnome.Shell.Extensions.Error.*`) as the former `ExtensionError` of
 * js/misc/dbusErrors.js.
 */
typedef enum
{
  SHELL_EXTENSION_ERROR_INFO_DOWNLOAD_FAILED = 0,
  SHELL_EXTENSION_ERROR_DOWNLOAD_FAILED = 1,
  SHELL_EXTENSION_ERROR_EXTRACT_FAILED = 2,
  SHELL_EXTENSION_ERROR_ENABLE_FAILED = 3,
  SHELL_EXTENSION_ERROR_NOT_ALLOWED = 4,
} ShellExtensionError;

#define SHELL_EXTENSION_ERROR (shell_extension_error_quark ())
GQuark shell_extension_error_quark (void);

#define SHELL_TYPE_EXTENSION_MANAGER (shell_extension_manager_get_type ())
G_DECLARE_FINAL_TYPE (ShellExtensionManager, shell_extension_manager,
                      SHELL, EXTENSION_MANAGER, GObject)

ShellExtensionManager *shell_extension_manager_new (ShellSessionMode *session_mode,
                                                    ShellMessageTray *message_tray);

void       shell_extension_manager_start                (ShellExtensionManager *self);

gboolean   shell_extension_manager_get_updates_supported (ShellExtensionManager *self);

ShellLuaExtension *shell_extension_manager_lookup       (ShellExtensionManager *self,
                                                         const char            *uuid);
char     **shell_extension_manager_get_uuids            (ShellExtensionManager *self);

GVariant  *shell_extension_manager_list_extensions      (ShellExtensionManager *self);
GVariant  *shell_extension_manager_get_extension_info   (ShellExtensionManager *self,
                                                         const char            *uuid);
GVariant  *shell_extension_manager_get_extension_errors (ShellExtensionManager *self,
                                                         const char            *uuid);
GVariant  *shell_extension_manager_serialize_extension  (ShellExtensionManager *self,
                                                         ShellLuaExtension     *extension);

gboolean   shell_extension_manager_enable_extension     (ShellExtensionManager *self,
                                                         const char            *uuid);
gboolean   shell_extension_manager_disable_extension    (ShellExtensionManager *self,
                                                         const char            *uuid);
gboolean   shell_extension_manager_reload_extension     (ShellExtensionManager  *self,
                                                         const char             *uuid,
                                                         GError                **error);

gboolean   shell_extension_manager_open_extension_prefs (ShellExtensionManager *self,
                                                         const char            *uuid,
                                                         const char            *parent_window,
                                                         GVariant              *options);

void       shell_extension_manager_install_extension_async  (ShellExtensionManager *self,
                                                             const char            *uuid,
                                                             GCancellable          *cancellable,
                                                             GAsyncReadyCallback    callback,
                                                             gpointer               user_data);
char      *shell_extension_manager_install_extension_finish (ShellExtensionManager  *self,
                                                             GAsyncResult           *result,
                                                             GError                **error);
gboolean   shell_extension_manager_uninstall_extension  (ShellExtensionManager *self,
                                                         const char            *uuid);

void       shell_extension_manager_check_for_updates_async  (ShellExtensionManager *self,
                                                             GCancellable          *cancellable,
                                                             GAsyncReadyCallback    callback,
                                                             gpointer               user_data);
gboolean   shell_extension_manager_check_for_updates_finish (ShellExtensionManager  *self,
                                                             GAsyncResult           *result,
                                                             GError                **error);

void       shell_extension_manager_log_extension_error  (ShellExtensionManager *self,
                                                         const char            *uuid,
                                                         const char            *message);

G_END_DECLS
