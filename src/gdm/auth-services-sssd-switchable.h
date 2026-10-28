/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auth-services-sssd-switchable.h: The switchable authentication of SSSD
 * (password, smartcard, passkey and web login in one PAM service). Port of
 * js/gdm/authServicesSSSDSwitchable.js.
 *
 * Private to the gdm module of libshell, like auth-services.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "gdm/auth-services.h"

G_BEGIN_DECLS

#define SHELL_TYPE_GDM_AUTH_SERVICES_SSSD_SWITCHABLE (shell_gdm_auth_services_sssd_switchable_get_type ())
G_DECLARE_FINAL_TYPE (ShellGdmAuthServicesSSSDSwitchable, shell_gdm_auth_services_sssd_switchable,
                      SHELL, GDM_AUTH_SERVICES_SSSD_SWITCHABLE, ShellGdmAuthServices)

gboolean shell_gdm_auth_services_sssd_switchable_is_enabled  (GSettings  *settings);
gboolean shell_gdm_auth_services_sssd_switchable_has_service (const char *service_name);

G_END_DECLS
