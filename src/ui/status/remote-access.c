/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * remote-access.c: The indicators of the top bar for screen recording and
 * screen sharing. Port of js/ui/status/remoteAccess.js.
 *
 * The handles are the #MetaRemoteAccessHandle objects of the
 * #MetaRemoteAccessController of the backend.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `Main.screenshotUI` is not reachable from C: the
 *    `screencast-in-progress` properties are set by panelItems.js, and the
 *    click on the recording indicator is the signal
 *    #ShellScreenRecordingIndicator::stop-screencast-requested. As in the
 *    JS, the applet only looks at the property when the handles change.
 *  - The sharing indicator is hidden when it is created, where the JS left
 *    it visible until its first synchronization hid it again (a timeout
 *    of NaN milliseconds).
 *  - The timeouts are dropped on ShellGlobal::shutdown.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>

#include <meta/meta-backend.h>
#include <meta/meta-remote-access-controller.h>

#include "shell-global.h"
#include "ui/status/remote-access.h"
#include "ui/status/status-private.h"

/* Minimum amount of time the shared indicator is visible (in micro seconds) */
#define MIN_SHARED_INDICATOR_VISIBLE_TIME_US (5 * G_TIME_SPAN_SECOND)

/* handle.is_recording: only the handles of the screen cast sessions have it */
static gboolean
handle_is_recording (MetaRemoteAccessHandle *handle)
{
  gboolean is_recording = FALSE;

  if (g_object_class_find_property (G_OBJECT_GET_CLASS (handle), "is-recording") == NULL)
    return FALSE;

  g_object_get (handle, "is-recording", &is_recording, NULL);
  return is_recording;
}

static MetaRemoteAccessController *
get_remote_access_controller (void)
{
  return meta_backend_get_remote_access_controller (shell_global_get_backend (shell_global_get ()));
}

static GHashTable *
handle_set_new (void)
{
  return g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);
}

static void
handle_set_add (GHashTable             *set,
                MetaRemoteAccessHandle *handle)
{
  g_hash_table_add (set, g_object_ref (handle));
}

/* ======================================================================== */
/* ShellRemoteAccessApplet                                                  */
/* ======================================================================== */

enum
{
  APPLET_PROP_0,

  APPLET_PROP_SCREENCAST_IN_PROGRESS,

  N_APPLET_PROPS
};

static GParamSpec *applet_props[N_APPLET_PROPS] = { NULL, };

struct _ShellRemoteAccessApplet
{
  ShellSystemIndicator parent;

  StIcon *icon;                 /* weak */
  GHashTable *handles;          /* MetaRemoteAccessHandle */
  gboolean screencast_in_progress;
};

G_DEFINE_FINAL_TYPE (ShellRemoteAccessApplet, shell_remote_access_applet,
                     SHELL_TYPE_SYSTEM_INDICATOR)

static void
applet_sync (ShellRemoteAccessApplet *self)
{
  guint n_handles = g_hash_table_size (self->handles);
  gboolean recording;

  if (self->icon == NULL)
    return;

  /* Screenshot UI screencasts have their own panel, so don't show this
   * indicator if there's only a screenshot UI screencast. */
  if (self->screencast_in_progress)
    recording = n_handles > 1;
  else
    recording = n_handles > 0;

  shell_status_set_visible (self->icon, recording);
}

static void
applet_handle_stopped_cb (MetaRemoteAccessHandle *handle,
                          ShellRemoteAccessApplet *self)
{
  g_hash_table_remove (self->handles, handle);
  applet_sync (self);
}

static void
applet_new_handle_cb (MetaRemoteAccessController *controller,
                      MetaRemoteAccessHandle     *handle,
                      ShellRemoteAccessApplet    *self)
{
  if (!handle_is_recording (handle))
    return;

  handle_set_add (self->handles, handle);
  g_signal_connect_object (handle, "stopped",
                           G_CALLBACK (applet_handle_stopped_cb), self, 0);

  applet_sync (self);
}

static void
shell_remote_access_applet_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  ShellRemoteAccessApplet *self = SHELL_REMOTE_ACCESS_APPLET (object);

  switch (prop_id)
    {
    case APPLET_PROP_SCREENCAST_IN_PROGRESS:
      g_value_set_boolean (value, self->screencast_in_progress);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_remote_access_applet_set_property (GObject      *object,
                                         guint         prop_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
  ShellRemoteAccessApplet *self = SHELL_REMOTE_ACCESS_APPLET (object);

  switch (prop_id)
    {
    case APPLET_PROP_SCREENCAST_IN_PROGRESS:
      if (self->screencast_in_progress != g_value_get_boolean (value))
        {
          self->screencast_in_progress = g_value_get_boolean (value);
          g_object_notify_by_pspec (object, pspec);
        }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_remote_access_applet_constructed (GObject *object)
{
  ShellRemoteAccessApplet *self = SHELL_REMOTE_ACCESS_APPLET (object);
  MetaRemoteAccessController *controller;
  StIcon *icon;

  G_OBJECT_CLASS (shell_remote_access_applet_parent_class)->constructed (object);

  controller = get_remote_access_controller ();
  if (controller == NULL)
    return;

  icon = shell_system_indicator_add_indicator (SHELL_SYSTEM_INDICATOR (self));
  g_set_weak_pointer (&self->icon, icon);
  st_icon_set_icon_name (icon, "media-record-symbolic");
  st_widget_add_style_class_name (ST_WIDGET (icon), "privacy-indicator");

  g_signal_connect_object (controller, "new-handle",
                           G_CALLBACK (applet_new_handle_cb), self, 0);
  applet_sync (self);
}

static void
shell_remote_access_applet_dispose (GObject *object)
{
  ShellRemoteAccessApplet *self = SHELL_REMOTE_ACCESS_APPLET (object);

  g_clear_weak_pointer (&self->icon);
  g_hash_table_remove_all (self->handles);

  G_OBJECT_CLASS (shell_remote_access_applet_parent_class)->dispose (object);
}

static void
shell_remote_access_applet_finalize (GObject *object)
{
  ShellRemoteAccessApplet *self = SHELL_REMOTE_ACCESS_APPLET (object);

  g_clear_pointer (&self->handles, g_hash_table_unref);

  G_OBJECT_CLASS (shell_remote_access_applet_parent_class)->finalize (object);
}

static void
shell_remote_access_applet_class_init (ShellRemoteAccessAppletClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_remote_access_applet_constructed;
  object_class->dispose = shell_remote_access_applet_dispose;
  object_class->finalize = shell_remote_access_applet_finalize;
  object_class->get_property = shell_remote_access_applet_get_property;
  object_class->set_property = shell_remote_access_applet_set_property;

  /**
   * ShellRemoteAccessApplet:screencast-in-progress:
   *
   * Whether the screenshot UI is recording a screencast
   * (`Main.screenshotUI.screencast_in_progress`). The indicator does not
   * show for a recording that is the one of the screenshot UI alone.
   */
  applet_props[APPLET_PROP_SCREENCAST_IN_PROGRESS] =
    g_param_spec_boolean ("screencast-in-progress", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_APPLET_PROPS, applet_props);
}

static void
shell_remote_access_applet_init (ShellRemoteAccessApplet *self)
{
  self->handles = handle_set_new ();
}

/**
 * shell_remote_access_applet_new:
 *
 * Creates the privacy indicator which shows while a screen is being
 * recorded (`new RemoteAccessApplet ()`). Without a remote access
 * controller it stays empty.
 *
 * Returns: (transfer floating): a new #ShellRemoteAccessApplet
 */
ShellRemoteAccessApplet *
shell_remote_access_applet_new (void)
{
  return g_object_new (SHELL_TYPE_REMOTE_ACCESS_APPLET, NULL);
}

/* ======================================================================== */
/* ShellScreenRecordingIndicator                                            */
/* ======================================================================== */

enum
{
  RECORDING_PROP_0,

  RECORDING_PROP_SCREENCAST_IN_PROGRESS,

  N_RECORDING_PROPS
};

static GParamSpec *recording_props[N_RECORDING_PROPS] = { NULL, };

enum
{
  STOP_SCREENCAST_REQUESTED,
  N_RECORDING_SIGNALS
};

static guint recording_signals[N_RECORDING_SIGNALS] = { 0, };

struct _ShellScreenRecordingIndicator
{
  ShellPanelButtonBox parent;

  StLabel *label;               /* weak */
  gboolean screencast_in_progress;
  guint timeout_id;
  int seconds_passed;
};

G_DEFINE_FINAL_TYPE (ShellScreenRecordingIndicator, shell_screen_recording_indicator,
                     SHELL_TYPE_PANEL_BUTTON_BOX)

static void
recording_update_label (ShellScreenRecordingIndicator *self)
{
  g_autofree char *text = NULL;

  if (self->label == NULL)
    return;

  text = g_strdup_printf ("%d:%02d", self->seconds_passed / 60, self->seconds_passed % 60);
  st_label_set_text (self->label, text);
}

static gboolean
recording_tick_cb (gpointer user_data)
{
  ShellScreenRecordingIndicator *self = user_data;

  self->seconds_passed += 1;
  recording_update_label (self);
  return G_SOURCE_CONTINUE;
}

static void
recording_stop_timeout (ShellScreenRecordingIndicator *self)
{
  g_clear_handle_id (&self->timeout_id, g_source_remove);
}

/* _onScreencastInProgressChanged () */
static void
recording_sync (ShellScreenRecordingIndicator *self)
{
  if (self->screencast_in_progress)
    {
      clutter_actor_show (CLUTTER_ACTOR (self));

      self->seconds_passed = 0;
      recording_update_label (self);

      recording_stop_timeout (self);
      self->timeout_id = g_timeout_add (1000, recording_tick_cb, self);
      g_source_set_name_by_id (self->timeout_id,
                               "[gnome-shell] screen recording indicator tick");
    }
  else
    {
      clutter_actor_hide (CLUTTER_ACTOR (self));

      recording_stop_timeout (self);
      self->seconds_passed = 0;
    }
}

static void
recording_clicked_cb (ShellScreenRecordingIndicator *self)
{
  g_signal_emit (self, recording_signals[STOP_SCREENCAST_REQUESTED], 0);
}

static void
recording_on_shutdown (ShellGlobal                   *global,
                       ShellScreenRecordingIndicator *self)
{
  recording_stop_timeout (self);
}

static void
shell_screen_recording_indicator_get_property (GObject    *object,
                                               guint       prop_id,
                                               GValue     *value,
                                               GParamSpec *pspec)
{
  ShellScreenRecordingIndicator *self = SHELL_SCREEN_RECORDING_INDICATOR (object);

  switch (prop_id)
    {
    case RECORDING_PROP_SCREENCAST_IN_PROGRESS:
      g_value_set_boolean (value, self->screencast_in_progress);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_screen_recording_indicator_set_property (GObject      *object,
                                               guint         prop_id,
                                               const GValue *value,
                                               GParamSpec   *pspec)
{
  ShellScreenRecordingIndicator *self = SHELL_SCREEN_RECORDING_INDICATOR (object);

  switch (prop_id)
    {
    case RECORDING_PROP_SCREENCAST_IN_PROGRESS:
      if (self->screencast_in_progress != g_value_get_boolean (value))
        {
          self->screencast_in_progress = g_value_get_boolean (value);
          recording_sync (self);
          g_object_notify_by_pspec (object, pspec);
        }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_screen_recording_indicator_constructed (GObject *object)
{
  ShellScreenRecordingIndicator *self = SHELL_SCREEN_RECORDING_INDICATOR (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *box;
  ClutterActor *label;
  ClutterActor *icon;
  ClutterAction *click_gesture;

  G_OBJECT_CLASS (shell_screen_recording_indicator_parent_class)->constructed (object);

  /* JS creates the indicators without any property */
  clutter_actor_set_reactive (actor, TRUE);
  st_widget_set_can_focus (ST_WIDGET (self), TRUE);
  st_widget_set_track_hover (ST_WIDGET (self), TRUE);
  clutter_actor_set_accessible_name (actor, _("Stop Screencast"));
  clutter_actor_set_accessible_role (actor, ATK_ROLE_PUSH_BUTTON);
  st_widget_add_style_class_name (ST_WIDGET (self), "screen-recording-indicator");

  box = CLUTTER_ACTOR (st_box_layout_new ());
  clutter_actor_add_child (actor, box);

  label = g_object_new (ST_TYPE_LABEL,
                        "text", "0:00",
                        "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  clutter_actor_add_child (box, label);
  g_set_weak_pointer (&self->label, ST_LABEL (label));

  icon = g_object_new (ST_TYPE_ICON, "icon-name", "screencast-stop-symbolic", NULL);
  clutter_actor_add_child (box, icon);

  clutter_actor_hide (actor);

  /* The actor owns the gesture, nothing else needs it */
  click_gesture = clutter_click_gesture_new ();
  clutter_click_gesture_set_recognize_on_press (CLUTTER_CLICK_GESTURE (click_gesture), TRUE);
  g_signal_connect_object (click_gesture, "recognize",
                           G_CALLBACK (recording_clicked_cb), self, G_CONNECT_SWAPPED);
  clutter_actor_add_action (actor, click_gesture);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (recording_on_shutdown), self, 0);
}

static void
shell_screen_recording_indicator_dispose (GObject *object)
{
  ShellScreenRecordingIndicator *self = SHELL_SCREEN_RECORDING_INDICATOR (object);

  recording_stop_timeout (self);
  g_clear_weak_pointer (&self->label);

  G_OBJECT_CLASS (shell_screen_recording_indicator_parent_class)->dispose (object);
}

static void
shell_screen_recording_indicator_class_init (ShellScreenRecordingIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_screen_recording_indicator_constructed;
  object_class->dispose = shell_screen_recording_indicator_dispose;
  object_class->get_property = shell_screen_recording_indicator_get_property;
  object_class->set_property = shell_screen_recording_indicator_set_property;

  /**
   * ShellScreenRecordingIndicator:screencast-in-progress:
   *
   * Whether the screenshot UI is recording a screencast
   * (`Main.screenshotUI.screencast_in_progress`). The indicator shows, with
   * the time passed, while it is %TRUE.
   */
  recording_props[RECORDING_PROP_SCREENCAST_IN_PROGRESS] =
    g_param_spec_boolean ("screencast-in-progress", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_RECORDING_PROPS, recording_props);

  /**
   * ShellScreenRecordingIndicator::stop-screencast-requested:
   * @self: the #ShellScreenRecordingIndicator
   *
   * Emitted when the user clicks the indicator. The handler stops the
   * screencast of the screenshot UI (`Main.screenshotUI.stopScreencast ()`).
   */
  recording_signals[STOP_SCREENCAST_REQUESTED] =
    g_signal_new ("stop-screencast-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_screen_recording_indicator_init (ShellScreenRecordingIndicator *self)
{
}

/**
 * shell_screen_recording_indicator_new:
 *
 * Creates the button which shows the time of the screencast of the screenshot
 * UI and stops it when it is clicked (`new ScreenRecordingIndicator ()`).
 *
 * Returns: (transfer floating): a new #ShellScreenRecordingIndicator
 */
ShellScreenRecordingIndicator *
shell_screen_recording_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_SCREEN_RECORDING_INDICATOR, NULL);
}

/* ======================================================================== */
/* ShellScreenSharingIndicator                                              */
/* ======================================================================== */

struct _ShellScreenSharingIndicator
{
  ShellPanelButtonBox parent;

  GHashTable *handles;          /* MetaRemoteAccessHandle */
  guint hide_indicator_id;
  gint64 visible_time_us;
};

G_DEFINE_FINAL_TYPE (ShellScreenSharingIndicator, shell_screen_sharing_indicator,
                     SHELL_TYPE_PANEL_BUTTON_BOX)

static void
sharing_stop_hide_timeout (ShellScreenSharingIndicator *self)
{
  g_clear_handle_id (&self->hide_indicator_id, g_source_remove);
}

static void
sharing_hide_indicator_cb (gpointer user_data)
{
  ShellScreenSharingIndicator *self = user_data;

  self->hide_indicator_id = 0;
  clutter_actor_hide (CLUTTER_ACTOR (self));
}

static void
sharing_sync (ShellScreenSharingIndicator *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  sharing_stop_hide_timeout (self);

  if (g_hash_table_size (self->handles) > 0)
    {
      if (!clutter_actor_is_visible (actor))
        self->visible_time_us = g_get_monotonic_time ();
      clutter_actor_show (actor);
    }
  else if (clutter_actor_is_visible (actor))
    {
      gint64 current_time_us = g_get_monotonic_time ();
      gint64 time_since_visible_us = current_time_us - self->visible_time_us;

      if (time_since_visible_us >= MIN_SHARED_INDICATOR_VISIBLE_TIME_US)
        {
          clutter_actor_hide (actor);
        }
      else
        {
          gint64 time_until_hide_us =
            MIN_SHARED_INDICATOR_VISIBLE_TIME_US - time_since_visible_us;

          self->hide_indicator_id =
            g_timeout_add_once (time_until_hide_us / G_TIME_SPAN_MILLISECOND,
                                sharing_hide_indicator_cb, self);
        }
    }
}

static void
sharing_handle_stopped_cb (MetaRemoteAccessHandle *handle,
                           ShellScreenSharingIndicator *self)
{
  g_hash_table_remove (self->handles, handle);
  sharing_sync (self);
}

static void
sharing_new_handle_cb (MetaRemoteAccessController *controller,
                       MetaRemoteAccessHandle     *handle,
                       ShellScreenSharingIndicator *self)
{
  if (handle_is_recording (handle))
    return;

  handle_set_add (self->handles, handle);
  g_signal_connect_object (handle, "stopped",
                           G_CALLBACK (sharing_handle_stopped_cb), self, 0);
  sharing_sync (self);
}

/* _stopSharing () */
static void
sharing_clicked_cb (ShellScreenSharingIndicator *self)
{
  GList *handles = g_hash_table_get_keys (self->handles);
  GList *l;

  /* Stopping a handle takes it out of the set */
  g_list_foreach (handles, (GFunc) g_object_ref, NULL);
  for (l = handles; l != NULL; l = l->next)
    meta_remote_access_handle_stop (META_REMOTE_ACCESS_HANDLE (l->data));
  g_list_free_full (handles, g_object_unref);
}

static void
sharing_on_shutdown (ShellGlobal                 *global,
                     ShellScreenSharingIndicator *self)
{
  sharing_stop_hide_timeout (self);
}

static void
shell_screen_sharing_indicator_constructed (GObject *object)
{
  ShellScreenSharingIndicator *self = SHELL_SCREEN_SHARING_INDICATOR (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  MetaRemoteAccessController *controller;
  ClutterActor *box;
  ClutterAction *click_gesture;

  G_OBJECT_CLASS (shell_screen_sharing_indicator_parent_class)->constructed (object);

  /* JS creates the indicators without any property */
  clutter_actor_set_reactive (actor, TRUE);
  st_widget_set_can_focus (ST_WIDGET (self), TRUE);
  st_widget_set_track_hover (ST_WIDGET (self), TRUE);
  clutter_actor_set_accessible_name (actor, _("Stop Screen Sharing"));
  clutter_actor_set_accessible_role (actor, ATK_ROLE_PUSH_BUTTON);
  st_widget_add_style_class_name (ST_WIDGET (self), "screen-sharing-indicator");

  box = CLUTTER_ACTOR (st_box_layout_new ());
  clutter_actor_add_child (actor, box);
  clutter_actor_add_child (box, g_object_new (ST_TYPE_ICON,
                                              "icon-name", "screen-shared-symbolic",
                                              NULL));
  clutter_actor_add_child (box, g_object_new (ST_TYPE_ICON,
                                              "icon-name", "screencast-stop-symbolic",
                                              NULL));

  controller = get_remote_access_controller ();
  if (controller != NULL)
    g_signal_connect_object (controller, "new-handle",
                             G_CALLBACK (sharing_new_handle_cb), self, 0);

  clutter_actor_hide (actor);

  /* The actor owns the gesture, nothing else needs it */
  click_gesture = clutter_click_gesture_new ();
  clutter_click_gesture_set_recognize_on_press (CLUTTER_CLICK_GESTURE (click_gesture), TRUE);
  g_signal_connect_object (click_gesture, "recognize",
                           G_CALLBACK (sharing_clicked_cb), self, G_CONNECT_SWAPPED);
  clutter_actor_add_action (actor, click_gesture);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (sharing_on_shutdown), self, 0);
}

static void
shell_screen_sharing_indicator_dispose (GObject *object)
{
  ShellScreenSharingIndicator *self = SHELL_SCREEN_SHARING_INDICATOR (object);

  sharing_stop_hide_timeout (self);
  g_hash_table_remove_all (self->handles);

  G_OBJECT_CLASS (shell_screen_sharing_indicator_parent_class)->dispose (object);
}

static void
shell_screen_sharing_indicator_finalize (GObject *object)
{
  ShellScreenSharingIndicator *self = SHELL_SCREEN_SHARING_INDICATOR (object);

  g_clear_pointer (&self->handles, g_hash_table_unref);

  G_OBJECT_CLASS (shell_screen_sharing_indicator_parent_class)->finalize (object);
}

static void
shell_screen_sharing_indicator_class_init (ShellScreenSharingIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_screen_sharing_indicator_constructed;
  object_class->dispose = shell_screen_sharing_indicator_dispose;
  object_class->finalize = shell_screen_sharing_indicator_finalize;
}

static void
shell_screen_sharing_indicator_init (ShellScreenSharingIndicator *self)
{
  self->handles = handle_set_new ();
}

/**
 * shell_screen_sharing_indicator_new:
 *
 * Creates the button which shows that the screen is shared and stops the
 * sharing when it is clicked (`new ScreenSharingIndicator ()`).
 *
 * Returns: (transfer floating): a new #ShellScreenSharingIndicator
 */
ShellScreenSharingIndicator *
shell_screen_sharing_indicator_new (void)
{
  return g_object_new (SHELL_TYPE_SCREEN_SHARING_INDICATOR, NULL);
}
