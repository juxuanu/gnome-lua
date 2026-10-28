/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * app-favorites.h: The applications pinned to the dash, kept in the
 * `favorite-apps` key of the shell settings. Port of js/ui/appFavorites.js.
 */

#pragma once

#include <glib-object.h>

#include "shell-app.h"

G_BEGIN_DECLS

#define SHELL_TYPE_APP_FAVORITES (shell_app_favorites_get_type ())
G_DECLARE_FINAL_TYPE (ShellAppFavorites, shell_app_favorites,
                      SHELL, APP_FAVORITES, GObject)

ShellAppFavorites *shell_app_favorites_get_default            (void);

void               shell_app_favorites_reload                 (ShellAppFavorites *self);

GList             *shell_app_favorites_get_favorites          (ShellAppFavorites *self);
gboolean           shell_app_favorites_is_favorite            (ShellAppFavorites *self,
                                                               const char        *app_id);

void               shell_app_favorites_add_favorite           (ShellAppFavorites *self,
                                                               const char        *app_id);
void               shell_app_favorites_add_favorite_at_pos    (ShellAppFavorites *self,
                                                               const char        *app_id,
                                                               int                pos);
void               shell_app_favorites_move_favorite_to_pos   (ShellAppFavorites *self,
                                                               const char        *app_id,
                                                               int                pos);
void               shell_app_favorites_remove_favorite        (ShellAppFavorites *self,
                                                               const char        *app_id);

gboolean           shell_app_favorites_add_favorite_silently    (ShellAppFavorites *self,
                                                                 const char        *app_id,
                                                                 int                pos);
gboolean           shell_app_favorites_remove_favorite_silently (ShellAppFavorites *self,
                                                                 const char        *app_id);

G_END_DECLS
