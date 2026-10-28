/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * main.c: The org.gnome.Shell.Extensions service. Port of
 * js/dbusServices/extensions/main.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <locale.h>

#include <glib/gi18n.h>

#include "extensions-service.h"

#define EXTENSIONS_BUS_NAME "org.gnome.Shell.Extensions"

int
main (int    argc,
      char **argv)
{
  g_autoptr (ExtService) service = NULL;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  g_set_prgname (EXTENSIONS_BUS_NAME);

  service = ext_service_new ();
  return shell_dbus_service_run (SHELL_DBUS_SERVICE (service), EXTENSIONS_BUS_NAME,
                                 argc, argv);
}
