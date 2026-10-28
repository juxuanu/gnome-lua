/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * weather.h: The weather location and forecast shown in the date menu.
 * Port of js/misc/weather.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>
#include <libgweather/gweather.h>

G_BEGIN_DECLS


#define SHELL_TYPE_WEATHER_CLIENT (shell_weather_client_get_type ())
G_DECLARE_FINAL_TYPE (ShellWeatherClient, shell_weather_client,
                      SHELL, WEATHER_CLIENT, GObject)

gboolean      shell_weather_client_get_available    (ShellWeatherClient *client);
gboolean      shell_weather_client_get_loading      (ShellWeatherClient *client);
gboolean      shell_weather_client_get_has_location (ShellWeatherClient *client);
GWeatherInfo *shell_weather_client_get_info         (ShellWeatherClient *client);

void          shell_weather_client_activate_app     (ShellWeatherClient *client);
void          shell_weather_client_update           (ShellWeatherClient *client);

G_END_DECLS
