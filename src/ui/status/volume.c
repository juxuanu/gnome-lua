/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * volume.c: The sound output and input indicators of the top bar and their
 * quick settings sliders. Port of js/ui/status/volume.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The two indicators are the final types #ShellVolumeOutputIndicator
 *    (`OutputIndicator`) and #ShellVolumeInputIndicator (`InputIndicator`),
 *    which derive from the abstract #ShellVolumeIndicator; the sliders are
 *    #ShellOutputStreamSlider and #ShellInputStreamSlider on top of the
 *    abstract #ShellStreamSlider. The `_`-prefixed methods which subclasses
 *    override are virtual functions of #ShellStreamSliderClass.
 *  - getMixerControl () is shell_volume_get_mixer_control ().
 *  - showOSD () does nothing while the slider has no stream (the JS threw
 *    trying to make an icon without a name); the level of a missing stream is
 *    NaN instead of null.
 *  - The sound that gives feedback while the volume changes is not played
 *    when the stream is gone, and nothing is scheduled after
 *    ShellGlobal::shutdown.
 *  - A missing org.gnome.desktop.sound schema reads as "no amplification"
 *    instead of throwing (see status-private.h).
 *  - An output port without a port object (the JS dereferenced it) is not a
 *    headphone.
 *  - Everything that is connected to the mixer control (a process wide
 *    object) or to a stream is disconnected when the slider or the indicator
 *    is disposed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>
#include <string.h>

#include <glib/gi18n-lib.h>
#include <meta/display.h>
#include <meta/meta-sound-player.h>

#include "shell-global.h"
#include "ui/main.h"
#include "ui/osd-window.h"
#include "ui/popup-menu.h"
#include "ui/slider.h"
#include "ui/status/status-private.h"
#include "ui/status/volume.h"

#define SOUND_SCHEMA "org.gnome.desktop.sound"
#define ALLOW_AMPLIFIED_VOLUME_KEY "allow-volume-above-100-percent"
#define UNMUTE_DEFAULT_VOLUME 0.25
#define VOLUME_CHANGE_FEEDBACK_DELAY_MS 30

#define DEVICE_ID_DATA_KEY "shell-stream-slider-device-id"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

/* Each GvcMixerControl is a connection to PulseAudio, so it is a singleton */
static GvcMixerControl *mixer_control = NULL;

/**
 * shell_volume_get_mixer_control:
 *
 * Gets the connection to PulseAudio of the shell, which is opened the first
 * time this is called (`getMixerControl ()`).
 *
 * Returns: (transfer none): the `GvcMixerControl` singleton
 */
GvcMixerControl *
shell_volume_get_mixer_control (void)
{
  if (mixer_control == NULL)
    {
      mixer_control = gvc_mixer_control_new ("GNOME Shell Volume Control");
      gvc_mixer_control_open (mixer_control);
    }

  return mixer_control;
}

/* ======================================================================== */
/* ShellStreamSlider                                                        */
/* ======================================================================== */

enum
{
  STREAM_PROP_0,

  STREAM_PROP_CONTROL,
  STREAM_PROP_STREAM,

  N_STREAM_PROPS
};

static GParamSpec *stream_props[N_STREAM_PROPS] = { NULL, };

enum
{
  STREAM_UPDATED,

  N_STREAM_SIGNALS
};

static guint stream_signals[N_STREAM_SIGNALS] = { 0, };

typedef struct
{
  GvcMixerControl *control;
  GvcMixerStream *stream;
  gboolean in_drag;
  guint notify_volume_change_id;
  gulong slider_changed_id;

  GSettings *sound_settings;
  gboolean allow_amplified;

  /* guint id -> ShellPopupImageMenuItem, which the section owns */
  GHashTable *device_items;
  ShellPopupMenuSection *device_section;

  GCancellable *volume_cancellable;

  char **icons;
  guint n_icons;
} ShellStreamSliderPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (ShellStreamSlider, shell_stream_slider,
                                     SHELL_TYPE_QUICK_SLIDER)

static void stream_slider_update_volume (ShellStreamSlider *self);

/**
 * shell_stream_slider_get_control:
 * @self: a #ShellStreamSlider
 *
 * Returns: (transfer none): the mixer control which the slider controls
 */
GvcMixerControl *
shell_stream_slider_get_control (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv;

  g_return_val_if_fail (SHELL_IS_STREAM_SLIDER (self), NULL);

  priv = shell_stream_slider_get_instance_private (self);
  return priv->control;
}

/**
 * shell_stream_slider_get_stream:
 * @self: a #ShellStreamSlider
 *
 * Returns: (transfer none) (nullable): the stream which the slider controls
 */
GvcMixerStream *
shell_stream_slider_get_stream (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv;

  g_return_val_if_fail (SHELL_IS_STREAM_SLIDER (self), NULL);

  priv = shell_stream_slider_get_instance_private (self);
  return priv->stream;
}

/**
 * shell_stream_slider_set_icons:
 * @self: a #ShellStreamSlider
 * @icons: (array zero-terminated=1) (element-type utf8): the icon names, from
 *   the one for a muted stream to the one for the highest volume
 *
 * Sets the icon names which shell_stream_slider_get_icon () picks from
 * (`_icons`).
 */
void
shell_stream_slider_set_icons (ShellStreamSlider  *self,
                               const char * const *icons)
{
  ShellStreamSliderPrivate *priv;

  g_return_if_fail (SHELL_IS_STREAM_SLIDER (self));

  priv = shell_stream_slider_get_instance_private (self);

  g_strfreev (priv->icons);
  priv->icons = g_strdupv ((char **) icons);
  priv->n_icons = priv->icons != NULL ? g_strv_length (priv->icons) : 0;
}

/**
 * shell_stream_slider_get_icon:
 * @self: a #ShellStreamSlider
 *
 * Gets the name of the icon which goes with the volume of the stream
 * (`getIcon ()`).
 *
 * Returns: (nullable): an icon name, %NULL if the slider has no stream or no
 *   icons
 */
const char *
shell_stream_slider_get_icon (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv;
  guint32 volume;
  int n;

  g_return_val_if_fail (SHELL_IS_STREAM_SLIDER (self), NULL);

  priv = shell_stream_slider_get_instance_private (self);

  if (priv->stream == NULL || priv->control == NULL || priv->n_icons == 0)
    return NULL;

  volume = gvc_mixer_stream_get_volume (priv->stream);
  if (gvc_mixer_stream_get_is_muted (priv->stream) || volume == 0)
    {
      n = 0;
    }
  else
    {
      n = (int) ceil (3.0 * volume / gvc_mixer_control_get_vol_max_norm (priv->control));
      n = CLAMP (n, 1, (int) priv->n_icons - 1);
    }

  return priv->icons[n];
}

/**
 * shell_stream_slider_get_level:
 * @self: a #ShellStreamSlider
 *
 * Gets the volume of the stream, 1 being the normal maximum (`getLevel ()`).
 *
 * Returns: the level, NaN if the slider has no stream
 */
double
shell_stream_slider_get_level (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv;

  g_return_val_if_fail (SHELL_IS_STREAM_SLIDER (self), NAN);

  priv = shell_stream_slider_get_instance_private (self);

  if (priv->stream == NULL || priv->control == NULL)
    return NAN;

  return gvc_mixer_stream_get_volume (priv->stream) /
         gvc_mixer_control_get_vol_max_norm (priv->control);
}

/**
 * shell_stream_slider_get_max_level:
 * @self: a #ShellStreamSlider
 *
 * Gets the highest level of the slider: 1, or more if amplification is
 * allowed (`getMaxLevel ()`).
 *
 * Returns: the maximum level
 */
double
shell_stream_slider_get_max_level (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv;
  double max_volume;

  g_return_val_if_fail (SHELL_IS_STREAM_SLIDER (self), 1.0);

  priv = shell_stream_slider_get_instance_private (self);

  if (priv->control == NULL)
    return 1.0;

  max_volume = gvc_mixer_control_get_vol_max_norm (priv->control);
  if (priv->allow_amplified)
    max_volume = gvc_mixer_control_get_vol_max_amplified (priv->control);

  return max_volume / gvc_mixer_control_get_vol_max_norm (priv->control);
}

/**
 * shell_stream_slider_show_osd:
 * @self: a #ShellStreamSlider
 *
 * Shows the level of the stream on the on-screen display of every monitor
 * (`showOSD ()`).
 */
void
shell_stream_slider_show_osd (ShellStreamSlider *self)
{
  const char *icon_name;
  ShellOsdWindowManager *osd_manager;
  g_autoptr (GIcon) gicon = NULL;

  g_return_if_fail (SHELL_IS_STREAM_SLIDER (self));

  icon_name = shell_stream_slider_get_icon (self);
  if (icon_name == NULL)
    return;

  osd_manager = shell_main_get_osd_window_manager (shell_main_get_default ());
  if (osd_manager == NULL)
    return;

  gicon = g_themed_icon_new (icon_name);
  shell_osd_window_manager_show_all (osd_manager, gicon, NULL,
                                     shell_stream_slider_get_level (self),
                                     shell_stream_slider_get_max_level (self));
}

/* _sync () */
static void
stream_slider_sync (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);
  ShellStreamSliderClass *klass = SHELL_STREAM_SLIDER_GET_CLASS (self);

  shell_status_set_visible (CLUTTER_ACTOR (self), klass->should_be_visible (self));
  shell_quick_slider_set_menu_enabled (SHELL_QUICK_SLIDER (self),
                                       priv->device_items != NULL &&
                                       g_hash_table_size (priv->device_items) > 1);
}

static gboolean
stream_slider_real_should_be_visible (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);

  return priv->stream != NULL;
}

static void
stream_slider_real_update_icon (ShellStreamSlider *self)
{
  shell_quick_slider_set_icon_name (SHELL_QUICK_SLIDER (self),
                                    shell_stream_slider_get_icon (self));
}

static void
stream_slider_stream_notify_cb (GObject           *stream,
                                GParamSpec        *pspec,
                                ShellStreamSlider *self)
{
  stream_slider_update_volume (self);
}

static void
stream_slider_real_connect_stream (ShellStreamSlider *self,
                                   GvcMixerStream    *stream)
{
  g_signal_connect_object (stream, "notify::is-muted",
                           G_CALLBACK (stream_slider_stream_notify_cb), self, 0);
  g_signal_connect_object (stream, "notify::volume",
                           G_CALLBACK (stream_slider_stream_notify_cb), self, 0);
}

/**
 * shell_stream_slider_set_stream:
 * @self: a #ShellStreamSlider
 * @stream: (nullable): the stream to control
 *
 * Sets #ShellStreamSlider:stream.
 */
void
shell_stream_slider_set_stream (ShellStreamSlider *self,
                                GvcMixerStream    *stream)
{
  ShellStreamSliderPrivate *priv;
  ShellStreamSliderClass *klass;
  GvcMixerStream *old_stream;

  g_return_if_fail (SHELL_IS_STREAM_SLIDER (self));
  g_return_if_fail (stream == NULL || GVC_IS_MIXER_STREAM (stream));

  priv = shell_stream_slider_get_instance_private (self);
  klass = SHELL_STREAM_SLIDER_GET_CLASS (self);

  /* Reference the new stream first, it can be the current one */
  if (stream != NULL)
    g_object_ref (stream);

  old_stream = priv->stream;
  priv->stream = stream;
  if (old_stream != NULL)
    {
      g_signal_handlers_disconnect_by_data (old_stream, self);
      g_object_unref (old_stream);
    }

  if (priv->stream != NULL)
    {
      klass->connect_stream (self, priv->stream);
      stream_slider_update_volume (self);
    }
  else
    {
      g_signal_emit (self, stream_signals[STREAM_UPDATED], 0);
    }

  stream_slider_sync (self);
  g_object_notify_by_pspec (G_OBJECT (self), stream_props[STREAM_PROP_STREAM]);
}

/* _addDevice () */
static void
stream_slider_device_item_activate_cb (ShellPopupBaseMenuItem *item,
                                       const ClutterEvent     *event,
                                       ShellStreamSlider      *self)
{
  ShellStreamSliderClass *klass = SHELL_STREAM_SLIDER_GET_CLASS (self);
  guint id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (item), DEVICE_ID_DATA_KEY));
  GvcMixerUIDevice *device = klass->lookup_device != NULL ? klass->lookup_device (self, id) : NULL;

  if (device != NULL)
    klass->activate_device (self, device);
  else
    g_warning ("Trying to activate invalid device %u", id);
}

static void
stream_slider_device_item_destroy_cb (ClutterActor      *item,
                                      ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);
  guint id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (item), DEVICE_ID_DATA_KEY));

  if (priv->device_items != NULL)
    g_hash_table_remove (priv->device_items, GUINT_TO_POINTER (id));
}

static void
stream_slider_add_device (ShellStreamSlider *self,
                          guint              id)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);
  ShellStreamSliderClass *klass = SHELL_STREAM_SLIDER_GET_CLASS (self);
  GvcMixerUIDevice *device;
  const char *description;
  const char *origin;
  g_autofree char *name = NULL;
  ClutterActor *item;

  if (priv->device_items == NULL || priv->device_section == NULL ||
      g_hash_table_contains (priv->device_items, GUINT_TO_POINTER (id)))
    return;

  device = klass->lookup_device != NULL ? klass->lookup_device (self, id) : NULL;
  if (device == NULL)
    return;

  description = gvc_mixer_ui_device_get_description (device);
  origin = gvc_mixer_ui_device_get_origin (device);
  if (origin != NULL && origin[0] != '\0')
    name = g_strdup_printf ("%s – %s", description, origin);
  else
    name = g_strdup (description);

  item = g_object_new (SHELL_TYPE_POPUP_IMAGE_MENU_ITEM,
                       "text", name,
                       "gicon", gvc_mixer_ui_device_get_gicon (device),
                       NULL);
  g_object_set_data (G_OBJECT (item), DEVICE_ID_DATA_KEY, GUINT_TO_POINTER (id));
  g_signal_connect_object (item, "activate",
                           G_CALLBACK (stream_slider_device_item_activate_cb), self, 0);
  g_signal_connect_object (item, "destroy",
                           G_CALLBACK (stream_slider_device_item_destroy_cb), self, 0);

  shell_popup_menu_base_add_menu_item (SHELL_POPUP_MENU_BASE (priv->device_section),
                                       G_OBJECT (item));
  g_hash_table_insert (priv->device_items, GUINT_TO_POINTER (id), item);

  stream_slider_sync (self);
}

/* _removeDevice () */
static void
stream_slider_remove_device (ShellStreamSlider *self,
                             guint              id)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);
  ClutterActor *item;

  if (priv->device_items == NULL)
    return;

  item = g_hash_table_lookup (priv->device_items, GUINT_TO_POINTER (id));
  if (item == NULL)
    return;

  /* The destroy handler takes the item out of the table */
  clutter_actor_destroy (item);
  stream_slider_sync (self);
}

/* _setActiveDevice () */
static void
stream_slider_set_active_device (ShellStreamSlider *self,
                                 guint              active_id)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);
  GHashTableIter iter;
  gpointer key, value;

  if (priv->device_items == NULL)
    return;

  g_hash_table_iter_init (&iter, priv->device_items);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      shell_popup_base_menu_item_set_ornament (SHELL_POPUP_BASE_MENU_ITEM (value),
                                               GPOINTER_TO_UINT (key) == active_id
                                               ? SHELL_ORNAMENT_CHECK
                                               : SHELL_ORNAMENT_NONE);
    }
}

/* _notifyVolumeChange () */
static void
stream_slider_notify_volume_change (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);
  ShellGlobal *global = shell_global_get ();
  MetaDisplay *display;
  MetaSoundPlayer *player;
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;

  if (priv->volume_cancellable != NULL)
    g_cancellable_cancel (priv->volume_cancellable);
  g_clear_object (&priv->volume_cancellable);

  if (priv->stream == NULL)
    return;

  if (gvc_mixer_stream_get_state (priv->stream) == GVC_STREAM_STATE_RUNNING)
    return; /* feedback not necessary while playing */

  display = shell_global_get_display (global);
  if (display == NULL)
    return;

  player = meta_display_get_sound_player (display);
  if (player == NULL)
    return;

  path = g_build_filename (shell_global_get_datadir (global),
                           "sounds", "audio-volume-change.oga", NULL);
  file = g_file_new_for_path (path);

  priv->volume_cancellable = g_cancellable_new ();
  meta_sound_player_play_from_file (player, file, _("Volume changed"),
                                    priv->volume_cancellable);
}

static void
stream_slider_volume_change_timeout_cb (gpointer user_data)
{
  ShellStreamSlider *self = user_data;
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);

  priv->notify_volume_change_id = 0;
  stream_slider_notify_volume_change (self);
}

/* _sliderChanged () */
static void
stream_slider_slider_changed_cb (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);
  ShellSlider *slider = shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (self));
  double value;
  double volume;
  gboolean prev_muted;
  gboolean volume_changed;

  if (priv->stream == NULL || priv->control == NULL)
    return;

  g_object_get (slider, "value", &value, NULL);
  volume = value * gvc_mixer_control_get_vol_max_norm (priv->control);
  prev_muted = gvc_mixer_stream_get_is_muted (priv->stream);

  if (volume < 1)
    {
      volume_changed = gvc_mixer_stream_set_volume (priv->stream, 0);
      if (!prev_muted)
        gvc_mixer_stream_change_is_muted (priv->stream, TRUE);
    }
  else
    {
      volume_changed = gvc_mixer_stream_set_volume (priv->stream, (guint32) volume);
      if (prev_muted)
        gvc_mixer_stream_change_is_muted (priv->stream, FALSE);
    }

  if (volume_changed)
    gvc_mixer_stream_push_volume (priv->stream);

  if (volume_changed && priv->notify_volume_change_id == 0 && !priv->in_drag)
    {
      priv->notify_volume_change_id =
        g_timeout_add_once (VOLUME_CHANGE_FEEDBACK_DELAY_MS,
                            stream_slider_volume_change_timeout_cb, self);
      g_source_set_name_by_id (priv->notify_volume_change_id,
                               "[gnome-shell] priv->notify_volume_change_id");
    }
}

static void
stream_slider_drag_begin_cb (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);

  priv->in_drag = TRUE;
}

static void
stream_slider_drag_end_cb (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);

  priv->in_drag = FALSE;
  stream_slider_notify_volume_change (self);
}

static void
stream_slider_icon_clicked_cb (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);
  gboolean is_muted;

  if (priv->stream == NULL || priv->control == NULL)
    return;

  is_muted = gvc_mixer_stream_get_is_muted (priv->stream);
  if (is_muted && gvc_mixer_stream_get_volume (priv->stream) == 0)
    {
      gvc_mixer_stream_set_volume (priv->stream,
                                   (guint32) (UNMUTE_DEFAULT_VOLUME *
                                              gvc_mixer_control_get_vol_max_norm (priv->control)));
      gvc_mixer_stream_push_volume (priv->stream);
    }
  gvc_mixer_stream_change_is_muted (priv->stream, !is_muted);
}

/* _changeSlider () */
static void
stream_slider_change_slider (ShellStreamSlider *self,
                             double             value)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);
  ShellSlider *slider = shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (self));

  g_signal_handler_block (slider, priv->slider_changed_id);
  g_object_set (slider, "value", value, NULL);
  g_signal_handler_unblock (slider, priv->slider_changed_id);
}

/* _updateVolume () */
static void
stream_slider_update_volume (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);
  ShellStreamSliderClass *klass = SHELL_STREAM_SLIDER_GET_CLASS (self);
  gboolean muted;

  if (priv->stream == NULL || priv->control == NULL)
    return;

  muted = gvc_mixer_stream_get_is_muted (priv->stream);
  stream_slider_change_slider (self,
                               muted
                               ? 0
                               : gvc_mixer_stream_get_volume (priv->stream) /
                                 gvc_mixer_control_get_vol_max_norm (priv->control));
  shell_quick_slider_set_icon_label (SHELL_QUICK_SLIDER (self),
                                     muted ? _("Unmute") : _("Mute"));
  klass->update_icon (self);
  g_signal_emit (self, stream_signals[STREAM_UPDATED], 0);
}

/* _amplifySettingsChanged () */
static void
stream_slider_amplify_settings_changed (ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);
  ShellSlider *slider = shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (self));
  double max_level;

  priv->allow_amplified = priv->sound_settings != NULL &&
                          g_settings_get_boolean (priv->sound_settings,
                                                  ALLOW_AMPLIFIED_VOLUME_KEY);

  max_level = priv->allow_amplified ? shell_stream_slider_get_max_level (self) : 1;
  g_object_set (slider, "maximum-value", max_level, NULL);

  shell_slider_clear_marks (slider);
  if (priv->allow_amplified)
    shell_slider_add_mark (slider, 1);

  if (priv->stream != NULL)
    stream_slider_update_volume (self);
}

static void
stream_slider_shutdown_cb (ShellGlobal       *global,
                           ShellStreamSlider *self)
{
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);

  g_clear_handle_id (&priv->notify_volume_change_id, g_source_remove);
  if (priv->volume_cancellable != NULL)
    g_cancellable_cancel (priv->volume_cancellable);
  g_clear_object (&priv->volume_cancellable);
}

static void
shell_stream_slider_constructed (GObject *object)
{
  ShellStreamSlider *self = SHELL_STREAM_SLIDER (object);
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);
  ShellQuickSlider *quick_slider = SHELL_QUICK_SLIDER (self);
  ShellSlider *slider;
  ShellPopupMenuBase *menu;

  G_OBJECT_CLASS (shell_stream_slider_parent_class)->constructed (object);

  shell_quick_slider_set_icon_reactive (quick_slider, TRUE);

  priv->sound_settings = shell_status_settings_new (SOUND_SCHEMA);
  if (priv->sound_settings != NULL)
    g_signal_connect_object (priv->sound_settings,
                             "changed::" ALLOW_AMPLIFIED_VOLUME_KEY,
                             G_CALLBACK (stream_slider_amplify_settings_changed),
                             self, G_CONNECT_SWAPPED);
  stream_slider_amplify_settings_changed (self);

  slider = shell_quick_slider_get_slider (quick_slider);
  priv->slider_changed_id =
    g_signal_connect_object (slider, "notify::value",
                             G_CALLBACK (stream_slider_slider_changed_cb),
                             self, G_CONNECT_SWAPPED);
  g_signal_connect_object (slider, "drag-begin",
                           G_CALLBACK (stream_slider_drag_begin_cb),
                           self, G_CONNECT_SWAPPED);
  g_signal_connect_object (slider, "drag-end",
                           G_CALLBACK (stream_slider_drag_end_cb),
                           self, G_CONNECT_SWAPPED);

  g_signal_connect_object (self, "icon-clicked",
                           G_CALLBACK (stream_slider_icon_clicked_cb),
                           self, G_CONNECT_SWAPPED);

  priv->device_items = g_hash_table_new (g_direct_hash, g_direct_equal);

  menu = SHELL_POPUP_MENU_BASE (shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self)));

  priv->device_section = shell_popup_menu_section_new ();
  shell_popup_menu_base_add_menu_item (menu, G_OBJECT (priv->device_section));

  shell_popup_menu_base_add_menu_item (menu,
                                       g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM, NULL));
  shell_popup_menu_base_add_settings_action (menu, _("Sound Settings"),
                                             "gnome-sound-panel.desktop");

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (stream_slider_shutdown_cb), self, 0);

  stream_slider_sync (self);
}

static void
shell_stream_slider_dispose (GObject *object)
{
  ShellStreamSlider *self = SHELL_STREAM_SLIDER (object);
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);

  g_clear_handle_id (&priv->notify_volume_change_id, g_source_remove);
  if (priv->volume_cancellable != NULL)
    g_cancellable_cancel (priv->volume_cancellable);
  g_clear_object (&priv->volume_cancellable);

  if (priv->stream != NULL)
    g_signal_handlers_disconnect_by_data (priv->stream, self);
  g_clear_object (&priv->stream);

  g_clear_object (&priv->sound_settings);
  g_clear_pointer (&priv->device_items, g_hash_table_unref);
  g_clear_object (&priv->device_section);
  g_clear_object (&priv->control);

  G_OBJECT_CLASS (shell_stream_slider_parent_class)->dispose (object);
}

static void
shell_stream_slider_finalize (GObject *object)
{
  ShellStreamSlider *self = SHELL_STREAM_SLIDER (object);
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);

  g_strfreev (priv->icons);

  G_OBJECT_CLASS (shell_stream_slider_parent_class)->finalize (object);
}

static void
shell_stream_slider_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellStreamSlider *self = SHELL_STREAM_SLIDER (object);
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);

  switch (prop_id)
    {
    case STREAM_PROP_CONTROL:
      g_value_set_object (value, priv->control);
      break;

    case STREAM_PROP_STREAM:
      g_value_set_object (value, priv->stream);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_stream_slider_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  ShellStreamSlider *self = SHELL_STREAM_SLIDER (object);
  ShellStreamSliderPrivate *priv = shell_stream_slider_get_instance_private (self);

  switch (prop_id)
    {
    case STREAM_PROP_CONTROL:
      g_set_object (&priv->control, g_value_get_object (value));
      break;

    case STREAM_PROP_STREAM:
      shell_stream_slider_set_stream (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_stream_slider_class_init (ShellStreamSliderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_stream_slider_constructed;
  object_class->dispose = shell_stream_slider_dispose;
  object_class->finalize = shell_stream_slider_finalize;
  object_class->get_property = shell_stream_slider_get_property;
  object_class->set_property = shell_stream_slider_set_property;

  klass->connect_stream = stream_slider_real_connect_stream;
  klass->update_icon = stream_slider_real_update_icon;
  klass->should_be_visible = stream_slider_real_should_be_visible;

  /**
   * ShellStreamSlider:control:
   *
   * The `GvcMixerControl` which the slider controls.
   */
  stream_props[STREAM_PROP_CONTROL] =
    g_param_spec_object ("control", NULL, NULL,
                         GVC_TYPE_MIXER_CONTROL,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellStreamSlider:stream:
   *
   * The `GvcMixerStream` which the slider controls (`stream`).
   */
  stream_props[STREAM_PROP_STREAM] =
    g_param_spec_object ("stream", NULL, NULL,
                         GVC_TYPE_MIXER_STREAM,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_STREAM_PROPS, stream_props);

  /**
   * ShellStreamSlider::stream-updated:
   * @self: the #ShellStreamSlider
   *
   * Emitted when the stream of the slider changed or when its volume or mute
   * state did.
   */
  stream_signals[STREAM_UPDATED] =
    g_signal_new ("stream-updated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_stream_slider_init (ShellStreamSlider *self)
{
}

/* ======================================================================== */
/* ShellOutputStreamSlider                                                  */
/* ======================================================================== */

struct _ShellOutputStreamSlider
{
  ShellStreamSlider parent;

  int has_headphones; /* -1 until the first port is known */
};

G_DEFINE_FINAL_TYPE (ShellOutputStreamSlider, shell_output_stream_slider,
                     SHELL_TYPE_STREAM_SLIDER)

static const char * const output_icons[] = {
  "audio-volume-muted-symbolic",
  "audio-volume-low-symbolic",
  "audio-volume-medium-symbolic",
  "audio-volume-high-symbolic",
  "audio-volume-overamplified-symbolic",
  NULL
};

static void
output_slider_output_added_cb (GvcMixerControl   *control,
                               guint              id,
                               ShellStreamSlider *self)
{
  stream_slider_add_device (self, id);
}

static void
output_slider_output_removed_cb (GvcMixerControl   *control,
                                 guint              id,
                                 ShellStreamSlider *self)
{
  stream_slider_remove_device (self, id);
}

static void
output_slider_active_output_update_cb (GvcMixerControl   *control,
                                       guint              id,
                                       ShellStreamSlider *self)
{
  stream_slider_set_active_device (self, id);
}

/* _findHeadphones () */
static gboolean
output_slider_find_headphones (GvcMixerStream *sink)
{
  const char *form_factor = gvc_mixer_stream_get_form_factor (sink);

  /* This only works for external headphones (e.g. bluetooth) */
  if (g_strcmp0 (form_factor, "headset") == 0 ||
      g_strcmp0 (form_factor, "headphone") == 0)
    return TRUE;

  /* a bit hackish, but ALSA/PulseAudio have a number
   * of different identifiers for headphones, and I could
   * not find the complete list */
  if (gvc_mixer_stream_get_ports (sink) != NULL)
    {
      const GvcMixerStreamPort *port = gvc_mixer_stream_get_port (sink);
      g_autofree char *lower = NULL;

      if (port == NULL || port->port == NULL)
        return FALSE;

      lower = g_ascii_strdown (port->port, -1);
      return strstr (lower, "headphone") != NULL;
    }

  return FALSE;
}

/* _portChanged () */
static void
output_slider_port_changed (ShellOutputStreamSlider *self)
{
  ShellStreamSlider *slider = SHELL_STREAM_SLIDER (self);
  GvcMixerStream *stream = shell_stream_slider_get_stream (slider);
  int has_headphones;
  gboolean initializing;

  if (stream == NULL)
    return;

  has_headphones = output_slider_find_headphones (stream) ? 1 : 0;
  if (has_headphones == self->has_headphones)
    return;

  initializing = self->has_headphones < 0;
  self->has_headphones = has_headphones;
  SHELL_STREAM_SLIDER_GET_CLASS (slider)->update_icon (slider);
  if (!initializing)
    shell_stream_slider_show_osd (slider);
}

static void
output_slider_port_notify_cb (GObject                 *stream,
                              GParamSpec              *pspec,
                              ShellOutputStreamSlider *self)
{
  output_slider_port_changed (self);
}

static void
output_slider_connect_stream (ShellStreamSlider *slider,
                              GvcMixerStream    *stream)
{
  ShellOutputStreamSlider *self = SHELL_OUTPUT_STREAM_SLIDER (slider);

  SHELL_STREAM_SLIDER_CLASS (shell_output_stream_slider_parent_class)->connect_stream (slider, stream);

  g_signal_connect_object (stream, "notify::port",
                           G_CALLBACK (output_slider_port_notify_cb), self, 0);
  output_slider_port_changed (self);
}

static GvcMixerUIDevice *
output_slider_lookup_device (ShellStreamSlider *slider,
                             guint              id)
{
  return gvc_mixer_control_lookup_output_id (shell_stream_slider_get_control (slider), id);
}

static void
output_slider_activate_device (ShellStreamSlider *slider,
                               GvcMixerUIDevice  *device)
{
  gvc_mixer_control_change_output (shell_stream_slider_get_control (slider), device);
}

static void
output_slider_update_icon (ShellStreamSlider *slider)
{
  ShellOutputStreamSlider *self = SHELL_OUTPUT_STREAM_SLIDER (slider);

  shell_quick_slider_set_icon_name (SHELL_QUICK_SLIDER (slider),
                                    self->has_headphones == 1
                                    ? "audio-headphones-symbolic"
                                    : shell_stream_slider_get_icon (slider));
}

static void
shell_output_stream_slider_constructed (GObject *object)
{
  ShellOutputStreamSlider *self = SHELL_OUTPUT_STREAM_SLIDER (object);
  ShellStreamSlider *slider = SHELL_STREAM_SLIDER (self);
  GvcMixerControl *control;
  ShellQuickToggleMenu *menu;

  G_OBJECT_CLASS (shell_output_stream_slider_parent_class)->constructed (object);

  control = shell_stream_slider_get_control (slider);

  clutter_actor_set_accessible_name (CLUTTER_ACTOR (shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (self))),
                                     _("Volume"));

  g_signal_connect_object (control, "output-added",
                           G_CALLBACK (output_slider_output_added_cb), self, 0);
  g_signal_connect_object (control, "output-removed",
                           G_CALLBACK (output_slider_output_removed_cb), self, 0);
  g_signal_connect_object (control, "active-output-update",
                           G_CALLBACK (output_slider_active_output_update_cb), self, 0);

  shell_stream_slider_set_icons (slider, output_icons);

  menu = shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self));
  shell_quick_toggle_menu_set_header (menu, "audio-headphones-symbolic",
                                      _("Sound Output"), NULL);
  g_object_set (self,
                "menu-button-accessible-name", _("Open sound output menu"),
                NULL);
}

static void
shell_output_stream_slider_class_init (ShellOutputStreamSliderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellStreamSliderClass *slider_class = SHELL_STREAM_SLIDER_CLASS (klass);

  object_class->constructed = shell_output_stream_slider_constructed;

  slider_class->lookup_device = output_slider_lookup_device;
  slider_class->activate_device = output_slider_activate_device;
  slider_class->connect_stream = output_slider_connect_stream;
  slider_class->update_icon = output_slider_update_icon;
}

static void
shell_output_stream_slider_init (ShellOutputStreamSlider *self)
{
  self->has_headphones = -1;
}

/**
 * shell_output_stream_slider_new:
 * @control: the mixer control, see shell_volume_get_mixer_control ()
 *
 * Creates the slider of the volume of the default sound output
 * (`new OutputStreamSlider (control)`).
 *
 * Returns: (transfer floating): a new #ShellOutputStreamSlider
 */
ShellOutputStreamSlider *
shell_output_stream_slider_new (GvcMixerControl *control)
{
  g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), NULL);

  return g_object_new (SHELL_TYPE_OUTPUT_STREAM_SLIDER,
                       "control", control,
                       NULL);
}

/* ======================================================================== */
/* ShellInputStreamSlider                                                   */
/* ======================================================================== */

struct _ShellInputStreamSlider
{
  ShellStreamSlider parent;

  gboolean show_input;
};

G_DEFINE_FINAL_TYPE (ShellInputStreamSlider, shell_input_stream_slider,
                     SHELL_TYPE_STREAM_SLIDER)

static const char * const input_icons[] = {
  "microphone-sensitivity-muted-symbolic",
  "microphone-sensitivity-low-symbolic",
  "microphone-sensitivity-medium-symbolic",
  "microphone-sensitivity-high-symbolic",
  NULL
};

static void
input_slider_input_added_cb (GvcMixerControl   *control,
                             guint              id,
                             ShellStreamSlider *self)
{
  stream_slider_add_device (self, id);
}

static void
input_slider_input_removed_cb (GvcMixerControl   *control,
                               guint              id,
                               ShellStreamSlider *self)
{
  stream_slider_remove_device (self, id);
}

static void
input_slider_active_input_update_cb (GvcMixerControl   *control,
                                     guint              id,
                                     ShellStreamSlider *self)
{
  stream_slider_set_active_device (self, id);
}

/* _maybeShowInput () */
static void
input_slider_maybe_show_input (ShellInputStreamSlider *self)
{
  ShellStreamSlider *slider = SHELL_STREAM_SLIDER (self);
  GvcMixerControl *control = shell_stream_slider_get_control (slider);
  gboolean show_input = FALSE;

  /* only show input widgets if any application is recording audio */
  if (shell_stream_slider_get_stream (slider) != NULL)
    {
      /* skip gnome-volume-control and pavucontrol which appear
       * as recording because they show the input level */
      static const char * const skipped_apps[] = {
        "org.gnome.VolumeControl",
        "org.PulseAudio.pavucontrol",
        NULL
      };
      g_autoptr (GSList) sources = gvc_mixer_control_get_sources (control);
      g_autoptr (GSList) outputs = NULL;
      gboolean any_running = FALSE;
      GSList *l;

      for (l = sources; l != NULL; l = l->next)
        {
          if (gvc_mixer_stream_get_state (GVC_MIXER_STREAM (l->data)) == GVC_STREAM_STATE_RUNNING)
            {
              any_running = TRUE;
              break;
            }
        }

      if (any_running)
        {
          outputs = gvc_mixer_control_get_source_outputs (control);
          for (l = outputs; l != NULL; l = l->next)
            {
              const char *app_id =
                gvc_mixer_stream_get_application_id (GVC_MIXER_STREAM (l->data));

              if (app_id == NULL || !g_strv_contains (skipped_apps, app_id))
                {
                  show_input = TRUE;
                  break;
                }
            }
        }
    }

  self->show_input = show_input;
  stream_slider_sync (slider);
}

static void
input_slider_streams_changed_cb (GvcMixerControl        *control,
                                 guint                   id,
                                 ShellInputStreamSlider *self)
{
  input_slider_maybe_show_input (self);
}

static void
input_slider_connect_stream (ShellStreamSlider *slider,
                             GvcMixerStream    *stream)
{
  SHELL_STREAM_SLIDER_CLASS (shell_input_stream_slider_parent_class)->connect_stream (slider, stream);

  input_slider_maybe_show_input (SHELL_INPUT_STREAM_SLIDER (slider));
}

static GvcMixerUIDevice *
input_slider_lookup_device (ShellStreamSlider *slider,
                            guint              id)
{
  return gvc_mixer_control_lookup_input_id (shell_stream_slider_get_control (slider), id);
}

static void
input_slider_activate_device (ShellStreamSlider *slider,
                              GvcMixerUIDevice  *device)
{
  gvc_mixer_control_change_input (shell_stream_slider_get_control (slider), device);
}

static gboolean
input_slider_should_be_visible (ShellStreamSlider *slider)
{
  ShellInputStreamSlider *self = SHELL_INPUT_STREAM_SLIDER (slider);

  return SHELL_STREAM_SLIDER_CLASS (shell_input_stream_slider_parent_class)->should_be_visible (slider) &&
         self->show_input;
}

static void
shell_input_stream_slider_constructed (GObject *object)
{
  ShellInputStreamSlider *self = SHELL_INPUT_STREAM_SLIDER (object);
  ShellStreamSlider *slider = SHELL_STREAM_SLIDER (self);
  GvcMixerControl *control;
  ShellQuickToggleMenu *menu;

  G_OBJECT_CLASS (shell_input_stream_slider_parent_class)->constructed (object);

  control = shell_stream_slider_get_control (slider);

  clutter_actor_set_accessible_name (CLUTTER_ACTOR (shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (self))),
                                     _("Microphone"));

  g_signal_connect_object (control, "input-added",
                           G_CALLBACK (input_slider_input_added_cb), self, 0);
  g_signal_connect_object (control, "input-removed",
                           G_CALLBACK (input_slider_input_removed_cb), self, 0);
  g_signal_connect_object (control, "active-input-update",
                           G_CALLBACK (input_slider_active_input_update_cb), self, 0);
  g_signal_connect_object (control, "stream-added",
                           G_CALLBACK (input_slider_streams_changed_cb), self, 0);
  g_signal_connect_object (control, "stream-removed",
                           G_CALLBACK (input_slider_streams_changed_cb), self, 0);
  g_signal_connect_object (control, "stream-changed",
                           G_CALLBACK (input_slider_streams_changed_cb), self, 0);

  shell_quick_slider_set_icon_name (SHELL_QUICK_SLIDER (self),
                                    "audio-input-microphone-symbolic");
  shell_stream_slider_set_icons (slider, input_icons);

  menu = shell_quick_settings_item_get_menu (SHELL_QUICK_SETTINGS_ITEM (self));
  shell_quick_toggle_menu_set_header (menu, "audio-input-microphone-symbolic",
                                      _("Sound Input"), NULL);
  g_object_set (self,
                "menu-button-accessible-name", _("Open sound input menu"),
                NULL);
}

static void
shell_input_stream_slider_class_init (ShellInputStreamSliderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellStreamSliderClass *slider_class = SHELL_STREAM_SLIDER_CLASS (klass);

  object_class->constructed = shell_input_stream_slider_constructed;

  slider_class->lookup_device = input_slider_lookup_device;
  slider_class->activate_device = input_slider_activate_device;
  slider_class->connect_stream = input_slider_connect_stream;
  slider_class->should_be_visible = input_slider_should_be_visible;
}

static void
shell_input_stream_slider_init (ShellInputStreamSlider *self)
{
}

/**
 * shell_input_stream_slider_new:
 * @control: the mixer control, see shell_volume_get_mixer_control ()
 *
 * Creates the slider of the volume of the default sound input
 * (`new InputStreamSlider (control)`). It is only shown while an application
 * records audio.
 *
 * Returns: (transfer floating): a new #ShellInputStreamSlider
 */
ShellInputStreamSlider *
shell_input_stream_slider_new (GvcMixerControl *control)
{
  g_return_val_if_fail (GVC_IS_MIXER_CONTROL (control), NULL);

  return g_object_new (SHELL_TYPE_INPUT_STREAM_SLIDER,
                       "control", control,
                       NULL);
}

/* ======================================================================== */
/* ShellVolumeIndicator                                                     */
/* ======================================================================== */

typedef struct
{
  StIcon *indicator; /* weak */
} ShellVolumeIndicatorPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (ShellVolumeIndicator, shell_volume_indicator,
                                     SHELL_TYPE_SYSTEM_INDICATOR)

/**
 * shell_volume_indicator_get_indicator:
 * @self: a #ShellVolumeIndicator
 *
 * Gets the status icon of the indicator (`_indicator`).
 *
 * Returns: (transfer none): the icon
 */
StIcon *
shell_volume_indicator_get_indicator (ShellVolumeIndicator *self)
{
  ShellVolumeIndicatorPrivate *priv;

  g_return_val_if_fail (SHELL_IS_VOLUME_INDICATOR (self), NULL);

  priv = shell_volume_indicator_get_instance_private (self);
  return priv->indicator;
}

/**
 * shell_volume_indicator_handle_scroll:
 * @self: a #ShellVolumeIndicator
 * @item: the slider which the indicator scrolls
 * @delta: the vertical scroll delta
 *
 * Moves the slider by the scroll delta, unless the quick settings menu shows
 * it, and shows the new level on the on-screen display (`_handleScroll ()`).
 */
void
shell_volume_indicator_handle_scroll (ShellVolumeIndicator *self,
                                      ShellStreamSlider    *item,
                                      double                delta)
{
  double n_steps = -delta;

  g_return_if_fail (SHELL_IS_VOLUME_INDICATOR (self));
  g_return_if_fail (SHELL_IS_STREAM_SLIDER (item));

  if (clutter_actor_is_mapped (CLUTTER_ACTOR (item)) ||
      shell_slider_step (shell_quick_slider_get_slider (SHELL_QUICK_SLIDER (item)), n_steps))
    shell_stream_slider_show_osd (item);
}

static void
volume_indicator_scroll_cb (ClutterScrollController *controller,
                            ClutterSprite           *sprite,
                            ClutterScrollSource      source,
                            double                   dx,
                            double                   dy,
                            ShellVolumeIndicator    *self)
{
  ShellVolumeIndicatorClass *klass = SHELL_VOLUME_INDICATOR_GET_CLASS (self);

  if (klass->scroll != NULL)
    klass->scroll (self, source, dx, dy);
}

static void
shell_volume_indicator_constructed (GObject *object)
{
  ShellVolumeIndicator *self = SHELL_VOLUME_INDICATOR (object);
  ShellVolumeIndicatorPrivate *priv = shell_volume_indicator_get_instance_private (self);
  StIcon *indicator;
  ClutterAction *scroll_controller;

  G_OBJECT_CLASS (shell_volume_indicator_parent_class)->constructed (object);

  indicator = shell_system_indicator_add_indicator (SHELL_SYSTEM_INDICATOR (self));
  g_set_weak_pointer (&priv->indicator, indicator);
  clutter_actor_set_reactive (CLUTTER_ACTOR (indicator), TRUE);

  scroll_controller =
    clutter_scroll_controller_new (CLUTTER_SCROLL_CONTROLLER_FLAG_SCROLL_VERTICAL |
                                   CLUTTER_SCROLL_CONTROLLER_FLAG_PHYSICAL_DIRECTION);
  g_signal_connect_object (scroll_controller, "scroll",
                           G_CALLBACK (volume_indicator_scroll_cb), self, 0);
  clutter_actor_add_action (CLUTTER_ACTOR (indicator), scroll_controller);
}

static void
shell_volume_indicator_dispose (GObject *object)
{
  ShellVolumeIndicator *self = SHELL_VOLUME_INDICATOR (object);
  ShellVolumeIndicatorPrivate *priv = shell_volume_indicator_get_instance_private (self);

  g_clear_weak_pointer (&priv->indicator);

  G_OBJECT_CLASS (shell_volume_indicator_parent_class)->dispose (object);
}

static void
shell_volume_indicator_class_init (ShellVolumeIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_volume_indicator_constructed;
  object_class->dispose = shell_volume_indicator_dispose;
}

static void
shell_volume_indicator_init (ShellVolumeIndicator *self)
{
}

/* ======================================================================== */
/* ShellVolumeOutputIndicator                                               */
/* ======================================================================== */

struct _ShellVolumeOutputIndicator
{
  ShellVolumeIndicator parent;

  GvcMixerControl *control;
  ShellOutputStreamSlider *output; /* weak */
};

G_DEFINE_FINAL_TYPE (ShellVolumeOutputIndicator, shell_volume_output_indicator,
                     SHELL_TYPE_VOLUME_INDICATOR)

/* _readOutput () */
static void
output_indicator_read_output (ShellVolumeOutputIndicator *self)
{
  if (self->output == NULL || self->control == NULL)
    return;

  shell_stream_slider_set_stream (SHELL_STREAM_SLIDER (self->output),
                                  gvc_mixer_control_get_default_sink (self->control));
}

/* _onControlStateChanged () */
static void
output_indicator_control_state_changed (ShellVolumeOutputIndicator *self)
{
  StIcon *indicator = shell_volume_indicator_get_indicator (SHELL_VOLUME_INDICATOR (self));

  if (self->control == NULL)
    return;

  if (gvc_mixer_control_get_state (self->control) == GVC_STATE_READY)
    output_indicator_read_output (self);
  else if (indicator != NULL)
    clutter_actor_hide (CLUTTER_ACTOR (indicator));
}

static void
output_indicator_default_sink_changed_cb (GvcMixerControl            *control,
                                          guint                       id,
                                          ShellVolumeOutputIndicator *self)
{
  output_indicator_read_output (self);
}

static void
output_indicator_state_changed_cb (GvcMixerControl            *control,
                                   GvcMixerControlState        state,
                                   ShellVolumeOutputIndicator *self)
{
  output_indicator_control_state_changed (self);
}

static void
output_indicator_stream_updated_cb (ShellOutputStreamSlider    *output,
                                    ShellVolumeOutputIndicator *self)
{
  StIcon *indicator = shell_volume_indicator_get_indicator (SHELL_VOLUME_INDICATOR (self));
  const char *icon = shell_stream_slider_get_icon (SHELL_STREAM_SLIDER (output));

  if (indicator == NULL)
    return;

  if (icon != NULL)
    st_icon_set_icon_name (indicator, icon);
  shell_status_set_visible (CLUTTER_ACTOR (indicator), icon != NULL);
}

static void
output_indicator_scroll (ShellVolumeIndicator *indicator,
                         ClutterScrollSource   source,
                         double                dx,
                         double                dy)
{
  ShellVolumeOutputIndicator *self = SHELL_VOLUME_OUTPUT_INDICATOR (indicator);

  if (self->output != NULL)
    shell_volume_indicator_handle_scroll (indicator, SHELL_STREAM_SLIDER (self->output), dy);
}

static void
shell_volume_output_indicator_constructed (GObject *object)
{
  ShellVolumeOutputIndicator *self = SHELL_VOLUME_OUTPUT_INDICATOR (object);
  ShellOutputStreamSlider *output;

  G_OBJECT_CLASS (shell_volume_output_indicator_parent_class)->constructed (object);

  self->control = g_object_ref (shell_volume_get_mixer_control ());
  g_signal_connect_object (self->control, "state-changed",
                           G_CALLBACK (output_indicator_state_changed_cb), self, 0);
  g_signal_connect_object (self->control, "default-sink-changed",
                           G_CALLBACK (output_indicator_default_sink_changed_cb), self, 0);

  output = shell_output_stream_slider_new (self->control);
  g_set_weak_pointer (&self->output, output);
  g_signal_connect_object (output, "stream-updated",
                           G_CALLBACK (output_indicator_stream_updated_cb), self, 0);

  shell_system_indicator_add_quick_settings_item (SHELL_SYSTEM_INDICATOR (self),
                                                  CLUTTER_ACTOR (output));

  output_indicator_control_state_changed (self);
}

static void
shell_volume_output_indicator_dispose (GObject *object)
{
  ShellVolumeOutputIndicator *self = SHELL_VOLUME_OUTPUT_INDICATOR (object);

  g_clear_weak_pointer (&self->output);
  g_clear_object (&self->control);

  G_OBJECT_CLASS (shell_volume_output_indicator_parent_class)->dispose (object);
}

static void
shell_volume_output_indicator_class_init (ShellVolumeOutputIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellVolumeIndicatorClass *indicator_class = SHELL_VOLUME_INDICATOR_CLASS (klass);

  object_class->constructed = shell_volume_output_indicator_constructed;
  object_class->dispose = shell_volume_output_indicator_dispose;

  indicator_class->scroll = output_indicator_scroll;
}

static void
shell_volume_output_indicator_init (ShellVolumeOutputIndicator *self)
{
}

/**
 * shell_volume_output_indicator_new:
 *
 * Creates the indicator of the sound output: the volume icon of the top bar
 * and the output volume slider of the quick settings (`new OutputIndicator ()`).
 *
 * Returns: (transfer floating): a new #ShellVolumeOutputIndicator
 */
ShellVolumeOutputIndicator *
shell_volume_output_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_VOLUME_OUTPUT_INDICATOR, NULL);
}

/* ======================================================================== */
/* ShellVolumeInputIndicator                                                */
/* ======================================================================== */

struct _ShellVolumeInputIndicator
{
  ShellVolumeIndicator parent;

  GvcMixerControl *control;
  ShellInputStreamSlider *input; /* weak */
};

G_DEFINE_FINAL_TYPE (ShellVolumeInputIndicator, shell_volume_input_indicator,
                     SHELL_TYPE_VOLUME_INDICATOR)

/* _readInput () */
static void
input_indicator_read_input (ShellVolumeInputIndicator *self)
{
  if (self->input == NULL || self->control == NULL)
    return;

  shell_stream_slider_set_stream (SHELL_STREAM_SLIDER (self->input),
                                  gvc_mixer_control_get_default_source (self->control));
}

/* _onControlStateChanged () */
static void
input_indicator_control_state_changed (ShellVolumeInputIndicator *self)
{
  if (self->control != NULL &&
      gvc_mixer_control_get_state (self->control) == GVC_STATE_READY)
    input_indicator_read_input (self);
}

static void
input_indicator_default_source_changed_cb (GvcMixerControl           *control,
                                           guint                      id,
                                           ShellVolumeInputIndicator *self)
{
  input_indicator_read_input (self);
}

static void
input_indicator_state_changed_cb (GvcMixerControl           *control,
                                  GvcMixerControlState       state,
                                  ShellVolumeInputIndicator *self)
{
  input_indicator_control_state_changed (self);
}

/* _updatePrivacyIndicator () */
static void
input_indicator_update_privacy_indicator (ShellVolumeInputIndicator *self)
{
  StIcon *indicator = shell_volume_indicator_get_indicator (SHELL_VOLUME_INDICATOR (self));
  GvcMixerStream *stream;

  if (self->input == NULL || indicator == NULL)
    return;

  stream = shell_stream_slider_get_stream (SHELL_STREAM_SLIDER (self->input));
  if (stream == NULL)
    return;

  /* Muted microphone doesn't need privacy indicator (no privacy concern) */
  if (gvc_mixer_stream_get_is_muted (stream))
    st_widget_remove_style_class_name (ST_WIDGET (indicator), "privacy-indicator");
  else
    st_widget_add_style_class_name (ST_WIDGET (indicator), "privacy-indicator");
}

static void
input_indicator_stream_updated_cb (ShellInputStreamSlider   *input,
                                   ShellVolumeInputIndicator *self)
{
  StIcon *indicator = shell_volume_indicator_get_indicator (SHELL_VOLUME_INDICATOR (self));
  const char *icon = shell_stream_slider_get_icon (SHELL_STREAM_SLIDER (input));

  if (indicator != NULL && icon != NULL)
    st_icon_set_icon_name (indicator, icon);
  input_indicator_update_privacy_indicator (self);
}

static void
input_indicator_scroll (ShellVolumeIndicator *indicator,
                        ClutterScrollSource   source,
                        double                dx,
                        double                dy)
{
  ShellVolumeInputIndicator *self = SHELL_VOLUME_INPUT_INDICATOR (indicator);

  if (self->input != NULL)
    shell_volume_indicator_handle_scroll (indicator, SHELL_STREAM_SLIDER (self->input), dy);
}

static void
shell_volume_input_indicator_constructed (GObject *object)
{
  ShellVolumeInputIndicator *self = SHELL_VOLUME_INPUT_INDICATOR (object);
  StIcon *indicator;
  ShellInputStreamSlider *input;

  G_OBJECT_CLASS (shell_volume_input_indicator_parent_class)->constructed (object);

  indicator = shell_volume_indicator_get_indicator (SHELL_VOLUME_INDICATOR (self));

  self->control = g_object_ref (shell_volume_get_mixer_control ());
  g_signal_connect_object (self->control, "state-changed",
                           G_CALLBACK (input_indicator_state_changed_cb), self, 0);
  g_signal_connect_object (self->control, "default-source-changed",
                           G_CALLBACK (input_indicator_default_source_changed_cb), self, 0);

  input = shell_input_stream_slider_new (self->control);
  g_set_weak_pointer (&self->input, input);
  g_signal_connect_object (input, "stream-updated",
                           G_CALLBACK (input_indicator_stream_updated_cb), self, 0);

  g_object_bind_property (input, "visible",
                          indicator, "visible",
                          G_BINDING_SYNC_CREATE);

  shell_system_indicator_add_quick_settings_item (SHELL_SYSTEM_INDICATOR (self),
                                                  CLUTTER_ACTOR (input));

  input_indicator_control_state_changed (self);
}

static void
shell_volume_input_indicator_dispose (GObject *object)
{
  ShellVolumeInputIndicator *self = SHELL_VOLUME_INPUT_INDICATOR (object);

  g_clear_weak_pointer (&self->input);
  g_clear_object (&self->control);

  G_OBJECT_CLASS (shell_volume_input_indicator_parent_class)->dispose (object);
}

static void
shell_volume_input_indicator_class_init (ShellVolumeInputIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellVolumeIndicatorClass *indicator_class = SHELL_VOLUME_INDICATOR_CLASS (klass);

  object_class->constructed = shell_volume_input_indicator_constructed;
  object_class->dispose = shell_volume_input_indicator_dispose;

  indicator_class->scroll = input_indicator_scroll;
}

static void
shell_volume_input_indicator_init (ShellVolumeInputIndicator *self)
{
}

/**
 * shell_volume_input_indicator_new:
 *
 * Creates the indicator of the sound input: the microphone icon of the top
 * bar, which is only shown while an application records audio, and the input
 * volume slider of the quick settings (`new InputIndicator ()`).
 *
 * Returns: (transfer floating): a new #ShellVolumeInputIndicator
 */
ShellVolumeInputIndicator *
shell_volume_input_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_VOLUME_INPUT_INDICATOR, NULL);
}
