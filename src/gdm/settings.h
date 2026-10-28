/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * settings.h: The keys of the org.gnome.login-screen schema. Port of
 * js/gdm/settings.js; the constants are exported to the remaining JS as
 * Shell.GDM_LOGIN_SCREEN_SCHEMA, Shell.GDM_BANNER_MESSAGE_KEY, ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#define SHELL_GDM_LOGIN_SCREEN_SCHEMA "org.gnome.login-screen"
#define SHELL_GDM_BANNER_MESSAGE_KEY "banner-message-enable"
#define SHELL_GDM_BANNER_MESSAGE_SOURCE_KEY "banner-message-source"
#define SHELL_GDM_BANNER_MESSAGE_TEXT_KEY "banner-message-text"
#define SHELL_GDM_BANNER_MESSAGE_PATH_KEY "banner-message-path"
#define SHELL_GDM_ALLOWED_FAILURES_KEY "allowed-failures"

#define SHELL_GDM_LOGO_KEY "logo"
#define SHELL_GDM_DISABLE_USER_LIST_KEY "disable-user-list"

#define SHELL_GDM_PASSWORD_AUTHENTICATION_KEY "enable-password-authentication"
#define SHELL_GDM_FINGERPRINT_AUTHENTICATION_KEY "enable-fingerprint-authentication"
#define SHELL_GDM_SMARTCARD_AUTHENTICATION_KEY "enable-smartcard-authentication"
#define SHELL_GDM_SWITCHABLE_AUTHENTICATION_KEY "enable-switchable-authentication"
