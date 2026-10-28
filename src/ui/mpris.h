/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * mpris.h: The media players of the session bus (MPRIS) and the message that
 * controls one of them. Port of js/ui/mpris.js.
 *
 * The types, with the name they had in the JS module:
 *
 *  - #ShellMprisPlayer: `MprisPlayer`. The getters of the JS class are
 *    shell_mpris_player_get_status (), shell_mpris_player_get_track_artists (),
 *    shell_mpris_player_get_track_title (),
 *    shell_mpris_player_get_track_cover_url (), shell_mpris_player_get_app (),
 *    shell_mpris_player_get_can_go_next () and
 *    shell_mpris_player_get_can_go_previous (); `playPause ()`, `next ()`,
 *    `previous ()` and `raise ()` are shell_mpris_player_play_pause (),
 *    shell_mpris_player_next (), shell_mpris_player_previous () and
 *    shell_mpris_player_raise (). The `source` member is
 *    shell_mpris_player_get_source (), the #ShellMessageSource whose title
 *    and icon are those of the application of the player.
 *  - #ShellMprisSource: `MprisSource`, which lists the players that can play
 *    (shell_mpris_source_get_players ()) and emits #ShellMprisSource::player-added
 *    and #ShellMprisSource::player-removed.
 *  - #ShellMediaMessage: `MediaMessage` (it was in js/ui/messageList.js), the
 *    #ShellMessage with the controls of one player. A JS caller adds it to a
 *    #ShellMessageView with shell_message_view_add_media_message () or to its
 *    own container.
 */

#pragma once

#include <gio/gio.h>

#include "shell-app.h"
#include "ui/message-list.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellMprisPlayer                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MPRIS_PLAYER (shell_mpris_player_get_type ())
G_DECLARE_FINAL_TYPE (ShellMprisPlayer, shell_mpris_player,
                      SHELL, MPRIS_PLAYER, GObject)

ShellMprisPlayer *shell_mpris_player_new (const char *bus_name);

const char         *shell_mpris_player_get_bus_name           (ShellMprisPlayer *self);
ShellMessageSource *shell_mpris_player_get_source             (ShellMprisPlayer *self);
gboolean            shell_mpris_player_get_can_play           (ShellMprisPlayer *self);
const char         *shell_mpris_player_get_status             (ShellMprisPlayer *self);
const char * const *shell_mpris_player_get_track_artists      (ShellMprisPlayer *self);
const char         *shell_mpris_player_get_track_title        (ShellMprisPlayer *self);
const char         *shell_mpris_player_get_track_cover_url    (ShellMprisPlayer *self);
ShellApp           *shell_mpris_player_get_app                (ShellMprisPlayer *self);
gboolean            shell_mpris_player_get_can_go_next        (ShellMprisPlayer *self);
gboolean            shell_mpris_player_get_can_go_previous    (ShellMprisPlayer *self);

void                shell_mpris_player_play_pause             (ShellMprisPlayer *self);
void                shell_mpris_player_next                   (ShellMprisPlayer *self);
void                shell_mpris_player_previous               (ShellMprisPlayer *self);
void                shell_mpris_player_raise                  (ShellMprisPlayer *self);

/* ------------------------------------------------------------------------ */
/* ShellMprisSource                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MPRIS_SOURCE (shell_mpris_source_get_type ())
G_DECLARE_FINAL_TYPE (ShellMprisSource, shell_mpris_source,
                      SHELL, MPRIS_SOURCE, GObject)

ShellMprisSource *shell_mpris_source_new         (void);

GList            *shell_mpris_source_get_players (ShellMprisSource *self);

/* ------------------------------------------------------------------------ */
/* ShellMediaMessage                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MEDIA_MESSAGE (shell_media_message_get_type ())
G_DECLARE_FINAL_TYPE (ShellMediaMessage, shell_media_message,
                      SHELL, MEDIA_MESSAGE, ShellMessage)

ShellMediaMessage *shell_media_message_new        (ShellMprisPlayer *player);

ShellMprisPlayer  *shell_media_message_get_player (ShellMediaMessage *self);

G_END_DECLS
