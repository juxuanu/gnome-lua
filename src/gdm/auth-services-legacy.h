/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auth-services-legacy.h: The password, smartcard and fingerprint PAM
 * services of GDM. Port of js/gdm/authServicesLegacy.js.
 *
 * Private to the gdm module of libshell, like auth-services.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "gdm/auth-services.h"

G_BEGIN_DECLS

#define SHELL_TYPE_GDM_AUTH_SERVICES_LEGACY (shell_gdm_auth_services_legacy_get_type ())
G_DECLARE_FINAL_TYPE (ShellGdmAuthServicesLegacy, shell_gdm_auth_services_legacy,
                      SHELL, GDM_AUTH_SERVICES_LEGACY, ShellGdmAuthServices)

gboolean shell_gdm_auth_services_legacy_is_enabled  (GSettings  *settings);
gboolean shell_gdm_auth_services_legacy_has_service (const char *service_name);

G_END_DECLS
