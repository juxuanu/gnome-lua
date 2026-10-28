/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * remote-search.h: The search providers of installed applications, which
 * talk to the application over D-Bus (`org.gnome.Shell.SearchProvider` and
 * `org.gnome.Shell.SearchProvider2`). Port of js/ui/remoteSearch.js.
 *
 * #ShellRemoteSearchProvider implements #ShellSearchProvider for version 1
 * of the interface, #ShellRemoteSearchProvider2 for version 2 (which adds
 * the search terms to the activation and `LaunchSearch`).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "ui/search.h"

G_BEGIN_DECLS

#define SHELL_TYPE_REMOTE_SEARCH_PROVIDER (shell_remote_search_provider_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellRemoteSearchProvider, shell_remote_search_provider,
                          SHELL, REMOTE_SEARCH_PROVIDER, GObject)

/**
 * ShellRemoteSearchProviderClass:
 * @parent_class: the parent class
 */
struct _ShellRemoteSearchProviderClass
{
  GObjectClass parent_class;

  /*< private >*/
  gpointer padding[8];
};

#define SHELL_TYPE_REMOTE_SEARCH_PROVIDER2 (shell_remote_search_provider2_get_type ())
G_DECLARE_FINAL_TYPE (ShellRemoteSearchProvider2, shell_remote_search_provider2,
                      SHELL, REMOTE_SEARCH_PROVIDER2, ShellRemoteSearchProvider)

ShellRemoteSearchProvider  *shell_remote_search_provider_new  (GAppInfo   *app_info,
                                                               const char *bus_name,
                                                               const char *object_path,
                                                               gboolean    auto_start);
ShellRemoteSearchProvider2 *shell_remote_search_provider2_new (GAppInfo   *app_info,
                                                               const char *bus_name,
                                                               const char *object_path,
                                                               gboolean    auto_start);

gboolean shell_remote_search_provider_get_default_enabled (ShellRemoteSearchProvider *self);
void     shell_remote_search_provider_set_default_enabled (ShellRemoteSearchProvider *self,
                                                          gboolean                   default_enabled);

GList   *shell_remote_search_load_providers               (GSettings *search_settings);

G_END_DECLS
