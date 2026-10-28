/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * mpris.c: The media players of the session bus (MPRIS) and the message that
 * controls one of them. Port of js/ui/mpris.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The proxies of the players are gdbus-codegen proxies (`mpris-dbus`), the
 *    bus itself is used through g_dbus_connection_call () and a signal
 *    subscription instead of an `org.freedesktop.DBus` proxy.
 *  - The source subscribes to NameOwnerChanged before it lists the names, so
 *    that a player that appears in between is not lost.
 *  - Everything is cancelled on ShellGlobal::shutdown and when the object is
 *    disposed; a player whose bus name lost its owner drops its proxies, and
 *    the proxies that are still being created are discarded.
 *  - The player state is updated once both proxies exist (the JS updated it
 *    from the player proxy alone and failed if the other proxy was late).
 *  - Faulty metadata is logged with g_message ().
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <string.h>

#include "shell-app-system.h"
#include "shell-global.h"
#include "ui/main.h"
#include "ui/message-tray.h"
#include "ui/mpris.h"
#include "ui/panel.h"
#include "ui/session-mode.h"

#include "mpris-dbus.h"

#define MPRIS_PLAYER_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"

/* ------------------------------------------------------------------------ */
/* ShellMprisPlayer                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  PLAYER_PROP_0,
  PLAYER_PROP_BUS_NAME,
  PLAYER_PROP_CAN_PLAY,
  PLAYER_PROP_SOURCE,
  PLAYER_N_PROPS
};

static GParamSpec *player_props[PLAYER_N_PROPS] = { NULL, };

enum
{
  PLAYER_CHANGED,
  PLAYER_LAST_SIGNAL
};

static guint player_signals[PLAYER_LAST_SIGNAL] = { 0 };

struct _ShellMprisPlayer
{
  GObject parent;

  char *bus_name;
  GCancellable *cancellable;
  gboolean closed;

  ShellMediaPlayer2 *mpris_proxy;
  ShellMediaPlayer2Player *player_proxy;
  gulong owner_handler_id;
  gulong properties_handler_id;

  ShellMessageSource *source;
  ShellApp *app;

  gboolean can_play;
  char **track_artists;
  char *track_title;
  char *track_cover_url;
};

G_DEFINE_FINAL_TYPE (ShellMprisPlayer, shell_mpris_player, G_TYPE_OBJECT)

static void
player_disconnect_proxies (ShellMprisPlayer *self)
{
  if (self->mpris_proxy != NULL)
    {
      g_clear_signal_handler (&self->owner_handler_id, self->mpris_proxy);
      g_clear_object (&self->mpris_proxy);
    }

  if (self->player_proxy != NULL)
    {
      g_clear_signal_handler (&self->properties_handler_id, self->player_proxy);
      g_clear_object (&self->player_proxy);
    }
}

/* `_close ()` */
static void
player_close (ShellMprisPlayer *self)
{
  self->closed = TRUE;
  player_disconnect_proxies (self);
}

static void
player_on_shutdown (ShellGlobal      *global,
                    ShellMprisPlayer *self)
{
  /* Nothing reaches the shell once it is going away */
  g_cancellable_cancel (self->cancellable);
  player_close (self);
}

static char *
describe_metadata_type (GVariant *value)
{
  return g_strdup (g_variant_get_type_string (value));
}

static void
player_update_state (ShellMprisPlayer *self)
{
  g_autoptr (GVariant) metadata = NULL;
  g_autoptr (GVariant) artists = NULL;
  g_autoptr (GVariant) title = NULL;
  g_autoptr (GVariant) cover_url = NULL;
  const char *desktop_entry = NULL;
  const char *identity = NULL;
  gboolean can_play;

  if (self->closed || self->player_proxy == NULL)
    return;

  metadata = shell_media_player2_player_dup_metadata (self->player_proxy);

  /* Validate according to the spec; some clients send buggy metadata:
   * https://www.freedesktop.org/wiki/Specifications/mpris-spec/metadata */
  g_clear_pointer (&self->track_artists, g_strfreev);
  if (metadata != NULL)
    artists = g_variant_lookup_value (metadata, "xesam:artist", NULL);
  if (artists != NULL && g_variant_is_of_type (artists, G_VARIANT_TYPE_STRING_ARRAY))
    {
      self->track_artists = g_variant_dup_strv (artists, NULL);
    }
  else
    {
      if (artists != NULL)
        {
          g_autofree char *type = describe_metadata_type (artists);

          g_message ("Received faulty track artist metadata from %s; "
                     "expected an array of strings, got a value of type '%s'",
                     self->bus_name, type);
        }

      self->track_artists = g_new0 (char *, 2);
      self->track_artists[0] = g_strdup (_("Unknown artist"));
    }

  g_clear_pointer (&self->track_title, g_free);
  if (metadata != NULL)
    title = g_variant_lookup_value (metadata, "xesam:title", NULL);
  if (title != NULL && g_variant_is_of_type (title, G_VARIANT_TYPE_STRING))
    {
      self->track_title = g_variant_dup_string (title, NULL);
    }
  else
    {
      if (title != NULL)
        {
          g_autofree char *type = describe_metadata_type (title);

          g_message ("Received faulty track title metadata from %s; "
                     "expected a string, got a value of type '%s'",
                     self->bus_name, type);
        }

      self->track_title = g_strdup (_("Unknown title"));
    }

  g_clear_pointer (&self->track_cover_url, g_free);
  if (metadata != NULL)
    cover_url = g_variant_lookup_value (metadata, "mpris:artUrl", NULL);
  if (cover_url != NULL && g_variant_is_of_type (cover_url, G_VARIANT_TYPE_STRING))
    {
      self->track_cover_url = g_variant_dup_string (cover_url, NULL);
    }
  else
    {
      if (cover_url != NULL)
        {
          g_autofree char *type = describe_metadata_type (cover_url);

          g_message ("Received faulty track cover art metadata from %s; "
                     "expected a string, got a value of type '%s'",
                     self->bus_name, type);
        }

      self->track_cover_url = g_strdup ("");
    }

  if (self->mpris_proxy != NULL)
    {
      desktop_entry = shell_media_player2_get_desktop_entry (self->mpris_proxy);
      identity = shell_media_player2_get_identity (self->mpris_proxy);
    }

  if (desktop_entry != NULL && *desktop_entry != '\0')
    {
      g_autofree char *desktop_id = g_strdup_printf ("%s.desktop", desktop_entry);
      ShellApp *app =
        shell_app_system_lookup_app (shell_app_system_get_default (), desktop_id);

      g_set_object (&self->app, app);
    }
  else
    {
      g_clear_object (&self->app);
    }

  shell_message_source_set_title (self->source,
                                  self->app != NULL ? shell_app_get_name (self->app)
                                                    : identity);
  shell_message_source_set_icon (self->source,
                                 self->app != NULL ? shell_app_get_icon (self->app)
                                                   : NULL);

  can_play = shell_media_player2_player_get_can_play (self->player_proxy);
  if (self->can_play != can_play)
    {
      self->can_play = can_play;
      g_object_notify_by_pspec (G_OBJECT (self), player_props[PLAYER_PROP_CAN_PLAY]);
    }

  g_signal_emit (self, player_signals[PLAYER_CHANGED], 0);
}

static void
player_on_properties_changed (ShellMprisPlayer *self)
{
  player_update_state (self);
}

static void
player_on_name_owner_changed (ShellMprisPlayer *self)
{
  if (self->mpris_proxy != NULL &&
      g_dbus_proxy_get_name_owner (G_DBUS_PROXY (self->mpris_proxy)) == NULL)
    player_close (self);
}

static void
player_mpris_proxy_ready_cb (GObject      *source_object,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  g_autoptr (ShellMprisPlayer) self = user_data;
  g_autoptr (GError) error = NULL;
  ShellMediaPlayer2 *proxy;

  proxy = shell_media_player2_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Could not create the MPRIS proxy of %s: %s",
                 self->bus_name, error->message);
      return;
    }

  if (self->closed)
    {
      g_object_unref (proxy);
      return;
    }

  self->mpris_proxy = proxy;
  self->owner_handler_id =
    g_signal_connect_swapped (proxy, "notify::g-name-owner",
                              G_CALLBACK (player_on_name_owner_changed), self);

  /* It is possible for the bus to disappear before the previous signal is
   * connected, so we must ensure that the bus still exists at this point. */
  if (g_dbus_proxy_get_name_owner (G_DBUS_PROXY (proxy)) == NULL)
    {
      player_close (self);
      return;
    }

  /* The identity and the desktop entry are known now */
  player_update_state (self);
}

static void
player_player_proxy_ready_cb (GObject      *source_object,
                              GAsyncResult *res,
                              gpointer      user_data)
{
  g_autoptr (ShellMprisPlayer) self = user_data;
  g_autoptr (GError) error = NULL;
  ShellMediaPlayer2Player *proxy;

  proxy = shell_media_player2_player_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Could not create the MPRIS player proxy of %s: %s",
                 self->bus_name, error->message);
      return;
    }

  if (self->closed)
    {
      g_object_unref (proxy);
      return;
    }

  self->player_proxy = proxy;
  self->properties_handler_id =
    g_signal_connect_swapped (proxy, "g-properties-changed",
                              G_CALLBACK (player_on_properties_changed), self);
  player_update_state (self);
}

static void
player_get_property (GObject    *object,
                     guint       prop_id,
                     GValue     *value,
                     GParamSpec *pspec)
{
  ShellMprisPlayer *self = SHELL_MPRIS_PLAYER (object);

  switch (prop_id)
    {
    case PLAYER_PROP_BUS_NAME:
      g_value_set_string (value, self->bus_name);
      break;

    case PLAYER_PROP_CAN_PLAY:
      g_value_set_boolean (value, self->can_play);
      break;

    case PLAYER_PROP_SOURCE:
      g_value_set_object (value, self->source);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
player_set_property (GObject      *object,
                     guint         prop_id,
                     const GValue *value,
                     GParamSpec   *pspec)
{
  ShellMprisPlayer *self = SHELL_MPRIS_PLAYER (object);

  switch (prop_id)
    {
    case PLAYER_PROP_BUS_NAME:
      g_free (self->bus_name);
      self->bus_name = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
player_constructed (GObject *object)
{
  ShellMprisPlayer *self = SHELL_MPRIS_PLAYER (object);

  G_OBJECT_CLASS (shell_mpris_player_parent_class)->constructed (object);

  g_return_if_fail (self->bus_name != NULL);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (player_on_shutdown), self, 0);

  shell_media_player2_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                         G_DBUS_PROXY_FLAGS_NONE,
                                         self->bus_name,
                                         MPRIS_OBJECT_PATH,
                                         self->cancellable,
                                         player_mpris_proxy_ready_cb,
                                         g_object_ref (self));
  shell_media_player2_player_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                self->bus_name,
                                                MPRIS_OBJECT_PATH,
                                                self->cancellable,
                                                player_player_proxy_ready_cb,
                                                g_object_ref (self));
}

static void
player_dispose (GObject *object)
{
  ShellMprisPlayer *self = SHELL_MPRIS_PLAYER (object);

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);
  player_close (self);

  g_clear_object (&self->app);
  g_clear_object (&self->source);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (shell_mpris_player_parent_class)->dispose (object);
}

static void
player_finalize (GObject *object)
{
  ShellMprisPlayer *self = SHELL_MPRIS_PLAYER (object);

  g_free (self->bus_name);
  g_strfreev (self->track_artists);
  g_free (self->track_title);
  g_free (self->track_cover_url);

  G_OBJECT_CLASS (shell_mpris_player_parent_class)->finalize (object);
}

static void
shell_mpris_player_class_init (ShellMprisPlayerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = player_constructed;
  object_class->dispose = player_dispose;
  object_class->finalize = player_finalize;
  object_class->get_property = player_get_property;
  object_class->set_property = player_set_property;

  /**
   * ShellMprisPlayer:bus-name:
   *
   * The name of the player on the session bus
   * (org.mpris.MediaPlayer2.<name>).
   */
  player_props[PLAYER_PROP_BUS_NAME] =
    g_param_spec_string ("bus-name", NULL, NULL,
                         NULL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMprisPlayer:can-play:
   *
   * Whether the player can play (`can-play`).
   */
  player_props[PLAYER_PROP_CAN_PLAY] =
    g_param_spec_boolean ("can-play", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellMprisPlayer:source:
   *
   * The source of the messages of the player, with the title and the icon of
   * its application (`source`).
   */
  player_props[PLAYER_PROP_SOURCE] =
    g_param_spec_object ("source", NULL, NULL,
                         SHELL_TYPE_MESSAGE_SOURCE,
                         G_PARAM_READABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PLAYER_N_PROPS, player_props);

  /**
   * ShellMprisPlayer::changed:
   * @self: the player
   *
   * Emitted when the state of the player (the track, the playback status, the
   * application) changed.
   */
  player_signals[PLAYER_CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_mpris_player_init (ShellMprisPlayer *self)
{
  self->cancellable = g_cancellable_new ();
  self->source = shell_message_source_new ();
  self->track_artists = g_new0 (char *, 1);
  self->track_title = g_strdup ("");
  self->track_cover_url = g_strdup ("");
}

/**
 * shell_mpris_player_new:
 * @bus_name: the name of the player on the session bus
 *
 * Creates the player of @bus_name (`new MprisPlayer (busName)`).
 *
 * Returns: (transfer full): a new #ShellMprisPlayer
 */
ShellMprisPlayer *
shell_mpris_player_new (const char *bus_name)
{
  g_return_val_if_fail (bus_name != NULL, NULL);

  return g_object_new (SHELL_TYPE_MPRIS_PLAYER,
                       "bus-name", bus_name,
                       NULL);
}

/**
 * shell_mpris_player_get_bus_name:
 * @self: a #ShellMprisPlayer
 *
 * Returns: (transfer none): #ShellMprisPlayer:bus-name
 */
const char *
shell_mpris_player_get_bus_name (ShellMprisPlayer *self)
{
  g_return_val_if_fail (SHELL_IS_MPRIS_PLAYER (self), NULL);

  return self->bus_name;
}

/**
 * shell_mpris_player_get_source:
 * @self: a #ShellMprisPlayer
 *
 * Returns: (transfer none): #ShellMprisPlayer:source
 */
ShellMessageSource *
shell_mpris_player_get_source (ShellMprisPlayer *self)
{
  g_return_val_if_fail (SHELL_IS_MPRIS_PLAYER (self), NULL);

  return self->source;
}

/**
 * shell_mpris_player_get_can_play:
 * @self: a #ShellMprisPlayer
 *
 * Returns: #ShellMprisPlayer:can-play
 */
gboolean
shell_mpris_player_get_can_play (ShellMprisPlayer *self)
{
  g_return_val_if_fail (SHELL_IS_MPRIS_PLAYER (self), FALSE);

  return self->can_play;
}

/**
 * shell_mpris_player_get_status:
 * @self: a #ShellMprisPlayer
 *
 * Gets the playback status, "Playing", "Paused" or "Stopped" (`status`).
 *
 * Returns: (transfer none) (nullable): the status, %NULL if the player is gone
 */
const char *
shell_mpris_player_get_status (ShellMprisPlayer *self)
{
  g_return_val_if_fail (SHELL_IS_MPRIS_PLAYER (self), NULL);

  if (self->player_proxy == NULL)
    return NULL;

  return shell_media_player2_player_get_playback_status (self->player_proxy);
}

/**
 * shell_mpris_player_get_track_artists:
 * @self: a #ShellMprisPlayer
 *
 * Gets the artists of the track (`trackArtists`); "Unknown artist" if the
 * player did not send any.
 *
 * Returns: (transfer none) (array zero-terminated=1): the artists
 */
const char * const *
shell_mpris_player_get_track_artists (ShellMprisPlayer *self)
{
  g_return_val_if_fail (SHELL_IS_MPRIS_PLAYER (self), NULL);

  return (const char * const *) self->track_artists;
}

/**
 * shell_mpris_player_get_track_title:
 * @self: a #ShellMprisPlayer
 *
 * Gets the title of the track (`trackTitle`); "Unknown title" if the player
 * did not send any.
 *
 * Returns: (transfer none): the title
 */
const char *
shell_mpris_player_get_track_title (ShellMprisPlayer *self)
{
  g_return_val_if_fail (SHELL_IS_MPRIS_PLAYER (self), NULL);

  return self->track_title;
}

/**
 * shell_mpris_player_get_track_cover_url:
 * @self: a #ShellMprisPlayer
 *
 * Gets the URL of the cover art of the track (`trackCoverUrl`).
 *
 * Returns: (transfer none): the URL, the empty string if there is none
 */
const char *
shell_mpris_player_get_track_cover_url (ShellMprisPlayer *self)
{
  g_return_val_if_fail (SHELL_IS_MPRIS_PLAYER (self), NULL);

  return self->track_cover_url;
}

/**
 * shell_mpris_player_get_app:
 * @self: a #ShellMprisPlayer
 *
 * Gets the application of the player (`app`).
 *
 * Returns: (transfer none) (nullable): the application, %NULL if the player
 *   has no desktop entry or it is not installed
 */
ShellApp *
shell_mpris_player_get_app (ShellMprisPlayer *self)
{
  g_return_val_if_fail (SHELL_IS_MPRIS_PLAYER (self), NULL);

  return self->app;
}

/**
 * shell_mpris_player_get_can_go_next:
 * @self: a #ShellMprisPlayer
 *
 * Returns: whether the player can skip to the next track (`canGoNext`)
 */
gboolean
shell_mpris_player_get_can_go_next (ShellMprisPlayer *self)
{
  g_return_val_if_fail (SHELL_IS_MPRIS_PLAYER (self), FALSE);

  if (self->player_proxy == NULL)
    return FALSE;

  return shell_media_player2_player_get_can_go_next (self->player_proxy);
}

/**
 * shell_mpris_player_get_can_go_previous:
 * @self: a #ShellMprisPlayer
 *
 * Returns: whether the player can skip to the previous track
 *   (`canGoPrevious`)
 */
gboolean
shell_mpris_player_get_can_go_previous (ShellMprisPlayer *self)
{
  g_return_val_if_fail (SHELL_IS_MPRIS_PLAYER (self), FALSE);

  if (self->player_proxy == NULL)
    return FALSE;

  return shell_media_player2_player_get_can_go_previous (self->player_proxy);
}

static void
player_call_done_cb (GObject      *source_object,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;

  reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &error);
  if (reply == NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("MPRIS call failed: %s", error->message);
}

/**
 * shell_mpris_player_play_pause:
 * @self: a #ShellMprisPlayer
 *
 * Toggles between playing and paused (`playPause ()`).
 */
void
shell_mpris_player_play_pause (ShellMprisPlayer *self)
{
  g_return_if_fail (SHELL_IS_MPRIS_PLAYER (self));

  if (self->player_proxy == NULL)
    return;

  shell_media_player2_player_call_play_pause (self->player_proxy,
                                              self->cancellable,
                                              player_call_done_cb, NULL);
}

/**
 * shell_mpris_player_next:
 * @self: a #ShellMprisPlayer
 *
 * Skips to the next track (`next ()`).
 */
void
shell_mpris_player_next (ShellMprisPlayer *self)
{
  g_return_if_fail (SHELL_IS_MPRIS_PLAYER (self));

  if (self->player_proxy == NULL)
    return;

  shell_media_player2_player_call_next (self->player_proxy,
                                        self->cancellable,
                                        player_call_done_cb, NULL);
}

/**
 * shell_mpris_player_previous:
 * @self: a #ShellMprisPlayer
 *
 * Skips to the previous track (`previous ()`).
 */
void
shell_mpris_player_previous (ShellMprisPlayer *self)
{
  g_return_if_fail (SHELL_IS_MPRIS_PLAYER (self));

  if (self->player_proxy == NULL)
    return;

  shell_media_player2_player_call_previous (self->player_proxy,
                                            self->cancellable,
                                            player_call_done_cb, NULL);
}

/**
 * shell_mpris_player_raise:
 * @self: a #ShellMprisPlayer
 *
 * Brings the window of the player to the front (`raise ()`).
 */
void
shell_mpris_player_raise (ShellMprisPlayer *self)
{
  g_return_if_fail (SHELL_IS_MPRIS_PLAYER (self));

  /* The remote Raise() method may run into focus stealing prevention, so
   * prefer activating the app via .desktop file if possible */
  if (self->app != NULL)
    shell_app_activate (self->app);
  else if (self->mpris_proxy != NULL &&
           shell_media_player2_get_can_raise (self->mpris_proxy))
    shell_media_player2_call_raise (self->mpris_proxy,
                                    self->cancellable,
                                    player_call_done_cb, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellMprisSource                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  SOURCE_PLAYER_ADDED,
  SOURCE_PLAYER_REMOVED,
  SOURCE_LAST_SIGNAL
};

static guint source_signals[SOURCE_LAST_SIGNAL] = { 0 };

struct _ShellMprisSource
{
  GObject parent;

  GPtrArray *players;
  GCancellable *cancellable;
  GDBusConnection *connection;
  guint subscription_id;
};

G_DEFINE_FINAL_TYPE (ShellMprisSource, shell_mpris_source, G_TYPE_OBJECT)

static ShellMprisPlayer *
source_find_player (ShellMprisSource *self,
                    const char       *bus_name)
{
  guint i;

  for (i = 0; i < self->players->len; i++)
    {
      ShellMprisPlayer *player = g_ptr_array_index (self->players, i);

      if (g_str_equal (shell_mpris_player_get_bus_name (player), bus_name))
        return player;
    }

  return NULL;
}

static void
source_on_player_can_play_changed (ShellMprisPlayer *player,
                                   GParamSpec       *pspec,
                                   ShellMprisSource *self)
{
  if (shell_mpris_player_get_can_play (player))
    g_signal_emit (self, source_signals[SOURCE_PLAYER_ADDED], 0, player);
  else
    g_signal_emit (self, source_signals[SOURCE_PLAYER_REMOVED], 0, player);
}

static void
source_add_player (ShellMprisSource *self,
                   const char       *bus_name)
{
  g_autoptr (ShellMprisPlayer) player = NULL;

  if (source_find_player (self, bus_name) != NULL)
    return;

  player = shell_mpris_player_new (bus_name);
  g_signal_connect_object (player, "notify::can-play",
                           G_CALLBACK (source_on_player_can_play_changed),
                           self, 0);
  g_ptr_array_add (self->players, g_steal_pointer (&player));
}

static void
source_on_name_owner_changed (GDBusConnection *connection,
                              const char      *sender_name,
                              const char      *object_path,
                              const char      *interface_name,
                              const char      *signal_name,
                              GVariant        *parameters,
                              gpointer         user_data)
{
  ShellMprisSource *self = user_data;
  const char *name, *old_owner, *new_owner;

  g_variant_get (parameters, "(&s&s&s)", &name, &old_owner, &new_owner);

  if (!g_str_has_prefix (name, MPRIS_PLAYER_PREFIX))
    return;

  if (*old_owner != '\0')
    {
      ShellMprisPlayer *found = source_find_player (self, name);

      if (found != NULL)
        {
          g_autoptr (ShellMprisPlayer) player = g_object_ref (found);

          g_ptr_array_remove (self->players, player);
          g_signal_handlers_disconnect_by_func (player,
                                                source_on_player_can_play_changed,
                                                self);

          /* if !can_play, the player has already been removed or was never
           * exposed in our public players list */
          if (shell_mpris_player_get_can_play (player))
            g_signal_emit (self, source_signals[SOURCE_PLAYER_REMOVED], 0, player);
        }
    }

  if (*new_owner != '\0')
    source_add_player (self, name);
}

static void
source_list_names_cb (GObject      *source_object,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  g_autoptr (ShellMprisSource) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;
  g_autoptr (GVariant) names = NULL;
  GVariantIter iter;
  const char *name;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source_object),
                                         res, &error);
  if (reply == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Could not list the names of the session bus: %s", error->message);
      return;
    }

  names = g_variant_get_child_value (reply, 0);
  g_variant_iter_init (&iter, names);
  while (g_variant_iter_next (&iter, "&s", &name))
    {
      if (g_str_has_prefix (name, MPRIS_PLAYER_PREFIX))
        source_add_player (self, name);
    }
}

static void
source_bus_ready_cb (GObject      *source_object,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  g_autoptr (ShellMprisSource) self = user_data;
  g_autoptr (GError) error = NULL;
  GDBusConnection *connection;

  connection = g_bus_get_finish (res, &error);
  if (connection == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("No session bus, no media players: %s", error->message);
      return;
    }

  if (g_cancellable_is_cancelled (self->cancellable))
    {
      g_object_unref (connection);
      return;
    }

  self->connection = connection;

  /* Subscribe first, so that a player that appears while the names are
   * listed is not missed; adding a player twice is a no-op. */
  self->subscription_id =
    g_dbus_connection_signal_subscribe (connection,
                                        "org.freedesktop.DBus",
                                        "org.freedesktop.DBus",
                                        "NameOwnerChanged",
                                        "/org/freedesktop/DBus",
                                        "org.mpris.MediaPlayer2",
                                        G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE,
                                        source_on_name_owner_changed,
                                        self, NULL);

  g_dbus_connection_call (connection,
                          "org.freedesktop.DBus",
                          "/org/freedesktop/DBus",
                          "org.freedesktop.DBus",
                          "ListNames",
                          NULL,
                          G_VARIANT_TYPE ("(as)"),
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          self->cancellable,
                          source_list_names_cb,
                          g_object_ref (self));
}

static void
source_release (ShellMprisSource *self)
{
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  if (self->connection != NULL && self->subscription_id != 0)
    g_dbus_connection_signal_unsubscribe (self->connection, self->subscription_id);
  self->subscription_id = 0;
  g_clear_object (&self->connection);
}

static void
source_on_shutdown (ShellGlobal      *global,
                    ShellMprisSource *self)
{
  /* Nothing reaches the date menu or the lock screen once the shell is going
   * away */
  source_release (self);
}

static void
shell_mpris_source_constructed (GObject *object)
{
  ShellMprisSource *self = SHELL_MPRIS_SOURCE (object);

  G_OBJECT_CLASS (shell_mpris_source_parent_class)->constructed (object);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (source_on_shutdown), self, 0);

  g_bus_get (G_BUS_TYPE_SESSION, self->cancellable,
             source_bus_ready_cb, g_object_ref (self));
}

static void
shell_mpris_source_dispose (GObject *object)
{
  ShellMprisSource *self = SHELL_MPRIS_SOURCE (object);

  source_release (self);

  if (self->players != NULL)
    {
      guint i;

      for (i = 0; i < self->players->len; i++)
        g_signal_handlers_disconnect_by_func (g_ptr_array_index (self->players, i),
                                              source_on_player_can_play_changed,
                                              self);
      g_clear_pointer (&self->players, g_ptr_array_unref);
    }

  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (shell_mpris_source_parent_class)->dispose (object);
}

static void
shell_mpris_source_class_init (ShellMprisSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_mpris_source_constructed;
  object_class->dispose = shell_mpris_source_dispose;

  /**
   * ShellMprisSource::player-added:
   * @self: the source
   * @player: the player that can play now
   *
   * Emitted when a player that can play appears.
   */
  source_signals[SOURCE_PLAYER_ADDED] =
    g_signal_new ("player-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_MPRIS_PLAYER);

  /**
   * ShellMprisSource::player-removed:
   * @self: the source
   * @player: the player that cannot play any more
   *
   * Emitted when a player that could play disappears or cannot play any more.
   */
  source_signals[SOURCE_PLAYER_REMOVED] =
    g_signal_new ("player-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_MPRIS_PLAYER);
}

static void
shell_mpris_source_init (ShellMprisSource *self)
{
  self->players = g_ptr_array_new_with_free_func (g_object_unref);
  self->cancellable = g_cancellable_new ();
}

/**
 * shell_mpris_source_new:
 *
 * Creates a source that watches the session bus for media players
 * (`new MprisSource ()`).
 *
 * Returns: (transfer full): a new #ShellMprisSource
 */
ShellMprisSource *
shell_mpris_source_new (void)
{
  return g_object_new (SHELL_TYPE_MPRIS_SOURCE, NULL);
}

/**
 * shell_mpris_source_get_players:
 * @self: a #ShellMprisSource
 *
 * Gets the players that can play (`players`), in the order they appeared.
 *
 * Returns: (transfer container) (element-type ShellMprisPlayer): the players
 */
GList *
shell_mpris_source_get_players (ShellMprisSource *self)
{
  GList *players = NULL;
  guint i;

  g_return_val_if_fail (SHELL_IS_MPRIS_SOURCE (self), NULL);

  if (self->players == NULL)
    return NULL;

  for (i = self->players->len; i > 0; i--)
    {
      ShellMprisPlayer *player = g_ptr_array_index (self->players, i - 1);

      if (shell_mpris_player_get_can_play (player))
        players = g_list_prepend (players, player);
    }

  return players;
}

/* ------------------------------------------------------------------------ */
/* ShellMediaMessage                                                         */
/* ------------------------------------------------------------------------ */

enum
{
  MESSAGE_PROP_0,
  MESSAGE_PROP_PLAYER,
  MESSAGE_N_PROPS
};

static GParamSpec *message_props[MESSAGE_N_PROPS] = { NULL, };

struct _ShellMediaMessage
{
  ShellMessage parent;

  ShellMprisPlayer *player;
  ShellNotificationPolicy *policy;
  ShellSessionMode *session_mode;

  StButton *prev_button;
  StButton *play_pause_button;
  StButton *next_button;
};

G_DEFINE_FINAL_TYPE (ShellMediaMessage, shell_media_message, SHELL_TYPE_MESSAGE)

static gboolean
message_session_is_locked (ShellMediaMessage *self)
{
  return self->session_mode != NULL &&
         shell_session_mode_is_locked (self->session_mode);
}

static void
message_apply_policy (ShellMediaMessage *self)
{
  gboolean visible;

  if (self->policy == NULL)
    return;

  visible = shell_notification_policy_get_enable (self->policy) &&
            (!message_session_is_locked (self) ||
             shell_notification_policy_get_show_in_lock_screen (self->policy));
  g_object_set (self, "visible", visible, NULL);
}

static void
message_update_policy (ShellMediaMessage *self)
{
  ShellApp *app = shell_mpris_player_get_app (self->player);
  g_autofree char *app_id = NULL;

  if (app != NULL)
    {
      app_id = g_strdup (shell_app_get_id (app));

      if (g_str_has_suffix (app_id, ".desktop"))
        app_id[strlen (app_id) - strlen (".desktop")] = '\0';
    }
  else
    {
      app_id = g_strdup ("generic");
    }

  if (self->policy != NULL &&
      g_strcmp0 (shell_notification_policy_get_id (self->policy), app_id) == 0)
    return;

  if (self->policy != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->policy, self);
      g_clear_object (&self->policy);
    }

  self->policy = shell_notification_policy_new_for_app (app);

  /* Register notification source */
  shell_notification_policy_store (self->policy);

  g_signal_connect_swapped (self->policy, "notify::enable",
                            G_CALLBACK (message_apply_policy), self);
  g_signal_connect_swapped (self->policy, "notify::show-in-lock-screen",
                            G_CALLBACK (message_apply_policy), self);
  message_apply_policy (self);
}

static void
message_update (ShellMediaMessage *self)
{
  g_autoptr (GIcon) icon = NULL;
  g_autofree char *body = NULL;
  const char *cover_url = shell_mpris_player_get_track_cover_url (self->player);
  const char * const *artists = shell_mpris_player_get_track_artists (self->player);
  const char *status = shell_mpris_player_get_status (self->player);

  if (cover_url != NULL && *cover_url != '\0')
    {
      g_autoptr (GFile) file = g_file_new_for_uri (cover_url);

      icon = G_ICON (g_file_icon_new (file));
    }
  else
    {
      icon = g_themed_icon_new ("audio-x-generic-symbolic");
    }

  body = artists != NULL ? g_strjoinv (", ", (char **) artists) : g_strdup ("");

  shell_message_set_title (SHELL_MESSAGE (self),
                           shell_mpris_player_get_track_title (self->player));
  shell_message_set_body (SHELL_MESSAGE (self), body);
  shell_message_set_icon (SHELL_MESSAGE (self), icon);

  st_button_set_icon_name (self->play_pause_button,
                           g_strcmp0 (status, "Playing") == 0
                             ? "media-playback-pause-symbolic"
                             : "media-playback-start-symbolic");

  clutter_actor_set_reactive (CLUTTER_ACTOR (self->prev_button),
                              shell_mpris_player_get_can_go_previous (self->player));
  clutter_actor_set_reactive (CLUTTER_ACTOR (self->next_button),
                              shell_mpris_player_get_can_go_next (self->player));

  message_update_policy (self);
}

static void
message_on_previous_clicked (StButton          *button,
                             int                clicked_button,
                             ShellMediaMessage *self)
{
  shell_mpris_player_previous (self->player);
}

static void
message_on_play_pause_clicked (StButton          *button,
                               int                clicked_button,
                               ShellMediaMessage *self)
{
  shell_mpris_player_play_pause (self->player);
}

static void
message_on_next_clicked (StButton          *button,
                         int                clicked_button,
                         ShellMediaMessage *self)
{
  shell_mpris_player_next (self->player);
}

static void
message_on_player_changed (ShellMediaMessage *self)
{
  message_update (self);
}

static void
message_on_session_updated (ShellMediaMessage *self)
{
  message_apply_policy (self);
}

static void
shell_media_message_clicked (StButton *button,
                             int       clicked_button)
{
  ShellMediaMessage *self = SHELL_MEDIA_MESSAGE (button);
  ShellPanel *panel;

  if (message_session_is_locked (self))
    return;

  shell_mpris_player_raise (self->player);

  panel = shell_main_get_panel (shell_main_get_default ());
  if (panel != NULL)
    shell_panel_close_calendar (panel);
}

static void
shell_media_message_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellMediaMessage *self = SHELL_MEDIA_MESSAGE (object);

  switch (prop_id)
    {
    case MESSAGE_PROP_PLAYER:
      g_value_set_object (value, self->player);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_media_message_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  ShellMediaMessage *self = SHELL_MEDIA_MESSAGE (object);

  switch (prop_id)
    {
    case MESSAGE_PROP_PLAYER:
      g_set_object (&self->player, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_media_message_constructed (GObject *object)
{
  ShellMediaMessage *self = SHELL_MEDIA_MESSAGE (object);
  ShellSessionMode *session_mode;

  G_OBJECT_CLASS (shell_media_message_parent_class)->constructed (object);

  g_return_if_fail (self->player != NULL);

  shell_message_set_source (SHELL_MESSAGE (self),
                            shell_mpris_player_get_source (self->player));
  st_widget_add_style_class_name (ST_WIDGET (self), "media-message");

  self->prev_button = shell_message_add_media_control (SHELL_MESSAGE (self),
                                                      "media-skip-backward-symbolic");
  g_signal_connect (self->prev_button, "clicked",
                    G_CALLBACK (message_on_previous_clicked), self);

  self->play_pause_button = shell_message_add_media_control (SHELL_MESSAGE (self), "");
  g_signal_connect (self->play_pause_button, "clicked",
                    G_CALLBACK (message_on_play_pause_clicked), self);

  self->next_button = shell_message_add_media_control (SHELL_MESSAGE (self),
                                                       "media-skip-forward-symbolic");
  g_signal_connect (self->next_button, "clicked",
                    G_CALLBACK (message_on_next_clicked), self);

  session_mode = shell_main_get_session_mode (shell_main_get_default ());
  if (session_mode != NULL)
    {
      g_set_weak_pointer (&self->session_mode, session_mode);
      g_signal_connect_object (session_mode, "updated",
                               G_CALLBACK (message_on_session_updated), self,
                               G_CONNECT_SWAPPED);
    }
  g_signal_connect_object (self->player, "changed",
                           G_CALLBACK (message_on_player_changed), self,
                           G_CONNECT_SWAPPED);
  message_update (self);
}

static void
shell_media_message_dispose (GObject *object)
{
  ShellMediaMessage *self = SHELL_MEDIA_MESSAGE (object);

  if (self->player != NULL)
    g_signal_handlers_disconnect_by_data (self->player, self);

  if (self->session_mode != NULL)
    g_signal_handlers_disconnect_by_data (self->session_mode, self);
  g_clear_weak_pointer (&self->session_mode);

  if (self->policy != NULL)
    g_signal_handlers_disconnect_by_data (self->policy, self);

  self->prev_button = NULL;
  self->play_pause_button = NULL;
  self->next_button = NULL;

  g_clear_object (&self->policy);
  g_clear_object (&self->player);

  G_OBJECT_CLASS (shell_media_message_parent_class)->dispose (object);
}

static void
shell_media_message_class_init (ShellMediaMessageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->constructed = shell_media_message_constructed;
  object_class->dispose = shell_media_message_dispose;
  object_class->get_property = shell_media_message_get_property;
  object_class->set_property = shell_media_message_set_property;

  button_class->clicked = shell_media_message_clicked;

  /**
   * ShellMediaMessage:player:
   *
   * The player the message controls.
   */
  message_props[MESSAGE_PROP_PLAYER] =
    g_param_spec_object ("player", NULL, NULL,
                         SHELL_TYPE_MPRIS_PLAYER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, MESSAGE_N_PROPS, message_props);
}

static void
shell_media_message_init (ShellMediaMessage *self)
{
}

/**
 * shell_media_message_new:
 * @player: the player to control
 *
 * Creates the message of @player (`new MediaMessage (player)`).
 *
 * Returns: (transfer floating): a new #ShellMediaMessage
 */
ShellMediaMessage *
shell_media_message_new (ShellMprisPlayer *player)
{
  g_return_val_if_fail (SHELL_IS_MPRIS_PLAYER (player), NULL);

  return g_object_new (SHELL_TYPE_MEDIA_MESSAGE,
                       "player", player,
                       NULL);
}

/**
 * shell_media_message_get_player:
 * @self: a #ShellMediaMessage
 *
 * Returns: (transfer none): #ShellMediaMessage:player
 */
ShellMprisPlayer *
shell_media_message_get_player (ShellMediaMessage *self)
{
  g_return_val_if_fail (SHELL_IS_MEDIA_MESSAGE (self), NULL);

  return self->player;
}
