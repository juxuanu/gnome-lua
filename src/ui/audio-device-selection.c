/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * audio-device-selection.c: The dialog which asks the user which audio device
 * was plugged in, and its D-Bus service. Port of js/ui/audioDeviceSelection.js.
 *
 * #ShellAudioDeviceSelectionDBus exports org.gnome.Shell.AudioDeviceSelection
 * at /org/gnome/Shell/AudioDeviceSelection of the session bus (a
 * gdbus-codegen skeleton, #ShellAudioDeviceSelection) and owns the name of the
 * same name. `Main.overview.hide ()` (the overview is still JS) is the
 * #ShellAudioDeviceSelectionDBus::overview-hide-requested signal, which
 * ShellMain connects.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - A call to Open () with fewer than two known devices is answered without
 *    creating the dialog (the JS created it and threw away the exception).
 *  - The missing settings panel is reported with g_message () instead of
 *    `log ()`.
 *  - The width of a device box follows its height from a single idle
 *    callback before the redraw, not one per change of the height.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-later.h>

#include "misc/dbus-utils.h"
#include "shell-app.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "ui/audio-device-selection.h"
#include "ui/dialog.h"
#include "ui/main.h"
#include "ui/modal-dialog.h"

#include "audio-device-selection-dbus.h"

#define AUDIO_DEVICE_SELECTION_BUS_NAME "org.gnome.Shell.AudioDeviceSelection"
#define AUDIO_DEVICE_SELECTION_OBJECT_PATH "/org/gnome/Shell/AudioDeviceSelection"

typedef enum
{
  AUDIO_DEVICE_HEADPHONES = 1 << 0,
  AUDIO_DEVICE_HEADSET = 1 << 1,
  AUDIO_DEVICE_MICROPHONE = 1 << 2,
} AudioDevice;

static const struct
{
  AudioDevice device;
  const char *name;
  const char *icon_name;
} DEVICES[] = {
  { AUDIO_DEVICE_HEADPHONES, "headphones", "audio-headphones-symbolic" },
  { AUDIO_DEVICE_HEADSET, "headset", "audio-headset-symbolic" },
  { AUDIO_DEVICE_MICROPHONE, "microphone", "audio-input-microphone-symbolic" },
};

static const char *
get_device_label (AudioDevice device)
{
  switch (device)
    {
    case AUDIO_DEVICE_HEADPHONES:
      return _("Headphones");
    case AUDIO_DEVICE_HEADSET:
      return _("Headset");
    case AUDIO_DEVICE_MICROPHONE:
      return _("Microphone");
    default:
      return NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* ShellAudioDeviceSelectionDialog                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_AUDIO_DEVICE_SELECTION_DIALOG (shell_audio_device_selection_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ShellAudioDeviceSelectionDialog, shell_audio_device_selection_dialog,
                      SHELL, AUDIO_DEVICE_SELECTION_DIALOG, ShellModalDialog)

enum
{
  DEVICE_SELECTED,
  OVERVIEW_HIDE_REQUESTED,

  LAST_DIALOG_SIGNAL
};

static guint dialog_signals[LAST_DIALOG_SIGNAL] = { 0, };

struct _ShellAudioDeviceSelectionDialog
{
  ShellModalDialog parent;

  StBoxLayout *selection_box;     /* weak */
};

G_DEFINE_FINAL_TYPE (ShellAudioDeviceSelectionDialog, shell_audio_device_selection_dialog,
                     SHELL_TYPE_MODAL_DIALOG)

/* The width of a device box follows its height */
typedef struct
{
  ClutterActor *box;    /* weak */
  guint later_id;
} SquareLater;

static MetaLaters *
get_laters (void)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  MetaCompositor *compositor;

  if (display == NULL)
    return NULL;

  compositor = meta_display_get_compositor (display);
  if (compositor == NULL)
    return NULL;

  return meta_compositor_get_laters (compositor);
}

static void
square_later_free (SquareLater *later)
{
  if (later->later_id != 0)
    {
      MetaLaters *laters = get_laters ();

      if (laters != NULL)
        meta_laters_remove (laters, later->later_id);
      later->later_id = 0;
    }

  g_clear_weak_pointer (&later->box);
  g_free (later);
}

static gboolean
square_later_cb (gpointer user_data)
{
  SquareLater *later = user_data;

  later->later_id = 0;
  if (later->box != NULL)
    clutter_actor_set_width (later->box, clutter_actor_get_height (later->box));

  return G_SOURCE_REMOVE;
}

static void
box_height_changed_cb (ClutterActor *box,
                       GParamSpec   *pspec,
                       SquareLater  *later)
{
  MetaLaters *laters;

  if (later->later_id != 0)
    return;

  laters = get_laters ();
  if (laters == NULL)
    return;

  later->later_id = meta_laters_add (laters, META_LATER_BEFORE_REDRAW,
                                     square_later_cb, later, NULL);
}

static void
device_button_clicked_cb (StButton                        *button,
                          int                              clicked_button,
                          ShellAudioDeviceSelectionDialog *self)
{
  AudioDevice device = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "device"));

  g_signal_emit (self, dialog_signals[DEVICE_SELECTED], 0, (guint) device);
  shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
  g_signal_emit (self, dialog_signals[OVERVIEW_HIDE_REQUESTED], 0);
}

static void
add_device (ShellAudioDeviceSelectionDialog *self,
            AudioDevice                      device,
            const char                      *icon_name)
{
  SquareLater *later;
  ClutterActor *box, *icon, *label, *button;

  box = g_object_new (ST_TYPE_BOX_LAYOUT,
                      "style-class", "audio-selection-device-box",
                      "orientation", CLUTTER_ORIENTATION_VERTICAL,
                      NULL);
  later = g_new0 (SquareLater, 1);
  g_set_weak_pointer (&later->box, box);
  g_signal_connect_data (box, "notify::height",
                         G_CALLBACK (box_height_changed_cb), later,
                         (GClosureNotify) square_later_free, 0);

  icon = g_object_new (ST_TYPE_ICON,
                       "style-class", "audio-selection-device-icon",
                       "icon-name", icon_name,
                       NULL);
  clutter_actor_add_child (box, icon);

  label = g_object_new (ST_TYPE_LABEL,
                        "style-class", "audio-selection-device-label",
                        "text", get_device_label (device),
                        "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  clutter_actor_add_child (box, label);

  button = g_object_new (ST_TYPE_BUTTON,
                         "style-class", "audio-selection-device",
                         "can-focus", TRUE,
                         "child", box,
                         NULL);
  g_object_set_data (G_OBJECT (button), "device", GUINT_TO_POINTER (device));
  clutter_actor_add_child (CLUTTER_ACTOR (self->selection_box), button);

  g_signal_connect_object (button, "clicked",
                           G_CALLBACK (device_button_clicked_cb), self, 0);
}

static void
close_button_cb (gpointer user_data)
{
  shell_modal_dialog_close (SHELL_MODAL_DIALOG (user_data));
}

static void
open_settings_cb (gpointer user_data)
{
  ShellAudioDeviceSelectionDialog *self = user_data;
  const char *desktop_file = "gnome-sound-panel.desktop";
  ShellApp *app;

  app = shell_app_system_lookup_app (shell_global_get_app_system (shell_global_get ()),
                                     desktop_file);
  if (app == NULL)
    {
      g_message ("Settings panel for desktop file %s could not be loaded!", desktop_file);
      return;
    }

  shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
  g_signal_emit (self, dialog_signals[OVERVIEW_HIDE_REQUESTED], 0);
  shell_app_activate (app);
}

static void
shell_audio_device_selection_dialog_constructed (GObject *object)
{
  ShellAudioDeviceSelectionDialog *self = SHELL_AUDIO_DEVICE_SELECTION_DIALOG (object);
  ShellModalDialog *dialog = SHELL_MODAL_DIALOG (self);
  ShellSessionMode *session_mode = shell_main_get_session_mode (shell_main_get_default ());
  gboolean allow_settings = FALSE;
  ShellMessageDialogContent *content;
  StBoxLayout *selection_box;

  G_OBJECT_CLASS (shell_audio_device_selection_dialog_parent_class)->constructed (object);

  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT,
                          "title", _("Select Audio Device"),
                          NULL);

  selection_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                "style-class", "audio-selection-box",
                                "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                "x-expand", TRUE,
                                NULL);
  g_set_weak_pointer (&self->selection_box, selection_box);
  clutter_actor_add_child (CLUTTER_ACTOR (content), CLUTTER_ACTOR (selection_box));

  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (dialog)),
                           CLUTTER_ACTOR (content));

  shell_modal_dialog_add_button (dialog, _("Cancel"), CLUTTER_KEY_Escape, FALSE,
                                 close_button_cb, self, NULL);

  if (session_mode != NULL)
    g_object_get (session_mode, "allow-settings", &allow_settings, NULL);
  if (allow_settings)
    shell_modal_dialog_add_button (dialog, _("Sound Settings"), 0, FALSE,
                                   open_settings_cb, self, NULL);
}

static void
shell_audio_device_selection_dialog_dispose (GObject *object)
{
  ShellAudioDeviceSelectionDialog *self = SHELL_AUDIO_DEVICE_SELECTION_DIALOG (object);

  g_clear_weak_pointer (&self->selection_box);

  G_OBJECT_CLASS (shell_audio_device_selection_dialog_parent_class)->dispose (object);
}

static void
shell_audio_device_selection_dialog_class_init (ShellAudioDeviceSelectionDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_audio_device_selection_dialog_constructed;
  object_class->dispose = shell_audio_device_selection_dialog_dispose;

  /* ShellAudioDeviceSelectionDialog::device-selected (device: uint) */
  dialog_signals[DEVICE_SELECTED] =
    g_signal_new ("device-selected",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_UINT);

  /* ShellAudioDeviceSelectionDialog::overview-hide-requested () */
  dialog_signals[OVERVIEW_HIDE_REQUESTED] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_audio_device_selection_dialog_init (ShellAudioDeviceSelectionDialog *self)
{
}

/* Returns %NULL if @devices lists fewer than two devices */
static ShellAudioDeviceSelectionDialog *
shell_audio_device_selection_dialog_new (guint devices)
{
  ShellAudioDeviceSelectionDialog *self;
  guint i, n_devices = 0;

  for (i = 0; i < G_N_ELEMENTS (DEVICES); i++)
    {
      if (devices & DEVICES[i].device)
        n_devices++;
    }

  if (n_devices < 2)
    return NULL;

  self = g_object_new (SHELL_TYPE_AUDIO_DEVICE_SELECTION_DIALOG,
                       "dialog-style-class", "audio-device-selection-dialog",
                       NULL);

  for (i = 0; i < G_N_ELEMENTS (DEVICES); i++)
    {
      if (devices & DEVICES[i].device)
        add_device (self, DEVICES[i].device, DEVICES[i].icon_name);
    }

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellAudioDeviceSelectionDBus                                            */
/* ------------------------------------------------------------------------ */

enum
{
  OVERVIEW_HIDE_REQUESTED_SIGNAL,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellAudioDeviceSelectionDBus
{
  GObject parent;

  ShellAudioDeviceSelection *skeleton;
  guint name_id;

  ShellAudioDeviceSelectionDialog *dialog;    /* weak */
  char *sender;                               /* of the call to Open () */
};

G_DEFINE_FINAL_TYPE (ShellAudioDeviceSelectionDBus, shell_audio_device_selection_dbus,
                     G_TYPE_OBJECT)

static void
dialog_closed_cb (ShellModalDialog                *dialog,
                  ShellAudioDeviceSelectionDBus   *self)
{
  g_clear_weak_pointer (&self->dialog);
}

static void
dialog_device_selected_cb (ShellAudioDeviceSelectionDialog *dialog,
                           guint                            device,
                           ShellAudioDeviceSelectionDBus   *self)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (DEVICES); i++)
    {
      if (DEVICES[i].device == device)
        {
          shell_dbus_emit_signal_to_destination (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                                 self->sender,
                                                 "DeviceSelected",
                                                 g_variant_new ("(s)", DEVICES[i].name));
          return;
        }
    }
}

static void
dialog_overview_hide_requested_cb (ShellAudioDeviceSelectionDialog *dialog,
                                   ShellAudioDeviceSelectionDBus   *self)
{
  g_signal_emit (self, signals[OVERVIEW_HIDE_REQUESTED_SIGNAL], 0);
}

static gboolean
on_handle_open (ShellAudioDeviceSelection   *skeleton,
                GDBusMethodInvocation       *invocation,
                const char * const          *device_names,
                ShellAudioDeviceSelectionDBus *self)
{
  ShellAudioDeviceSelectionDialog *dialog;
  guint devices = 0;
  guint i, j;

  if (self->dialog != NULL)
    {
      shell_audio_device_selection_complete_open (skeleton, invocation);
      return TRUE;
    }

  for (i = 0; device_names != NULL && device_names[i] != NULL; i++)
    {
      for (j = 0; j < G_N_ELEMENTS (DEVICES); j++)
        {
          if (g_ascii_strcasecmp (device_names[i], DEVICES[j].name) == 0)
            devices |= DEVICES[j].device;
        }
    }

  dialog = shell_audio_device_selection_dialog_new (devices);
  if (dialog == NULL)
    {
      shell_audio_device_selection_complete_open (skeleton, invocation);
      return TRUE;
    }

  g_free (self->sender);
  self->sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));

  g_set_weak_pointer (&self->dialog, dialog);
  g_signal_connect_object (dialog, "closed",
                           G_CALLBACK (dialog_closed_cb), self, 0);
  g_signal_connect_object (dialog, "device-selected",
                           G_CALLBACK (dialog_device_selected_cb), self, 0);
  g_signal_connect_object (dialog, "overview-hide-requested",
                           G_CALLBACK (dialog_overview_hide_requested_cb), self, 0);

  if (!shell_modal_dialog_open (SHELL_MODAL_DIALOG (dialog)))
    {
      /* The input cannot be grabbed: forget about the dialog, or the next
       * calls would find it */
      g_clear_weak_pointer (&self->dialog);
      clutter_actor_destroy (CLUTTER_ACTOR (dialog));
    }

  shell_audio_device_selection_complete_open (skeleton, invocation);
  return TRUE;
}

static gboolean
on_handle_close (ShellAudioDeviceSelection   *skeleton,
                 GDBusMethodInvocation       *invocation,
                 ShellAudioDeviceSelectionDBus *self)
{
  if (self->dialog != NULL &&
      g_strcmp0 (self->sender, g_dbus_method_invocation_get_sender (invocation)) == 0)
    shell_modal_dialog_close (SHELL_MODAL_DIALOG (self->dialog));

  shell_audio_device_selection_complete_close (skeleton, invocation);
  return TRUE;
}

static void
shell_audio_device_selection_dbus_constructed (GObject *object)
{
  ShellAudioDeviceSelectionDBus *self = SHELL_AUDIO_DEVICE_SELECTION_DBUS (object);
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GError) error = NULL;

  G_OBJECT_CLASS (shell_audio_device_selection_dbus_parent_class)->constructed (object);

  self->skeleton = shell_audio_device_selection_skeleton_new ();
  g_signal_connect (self->skeleton, "handle-open",
                    G_CALLBACK (on_handle_open), self);
  g_signal_connect (self->skeleton, "handle-close",
                    G_CALLBACK (on_handle_close), self);

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (connection == NULL ||
      !g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                         connection, AUDIO_DEVICE_SELECTION_OBJECT_PATH,
                                         &error))
    {
      g_warning ("%s: %s", G_STRFUNC, error->message);
      return;
    }

  self->name_id = g_bus_own_name_on_connection (connection,
                                                AUDIO_DEVICE_SELECTION_BUS_NAME,
                                                G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                                NULL, NULL, NULL, NULL);
}

static void
shell_audio_device_selection_dbus_dispose (GObject *object)
{
  ShellAudioDeviceSelectionDBus *self = SHELL_AUDIO_DEVICE_SELECTION_DBUS (object);

  g_clear_handle_id (&self->name_id, g_bus_unown_name);

  if (self->skeleton != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->skeleton, self);
      if (g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (self->skeleton)) != NULL)
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->skeleton));
      g_clear_object (&self->skeleton);
    }

  g_clear_weak_pointer (&self->dialog);

  G_OBJECT_CLASS (shell_audio_device_selection_dbus_parent_class)->dispose (object);
}

static void
shell_audio_device_selection_dbus_finalize (GObject *object)
{
  ShellAudioDeviceSelectionDBus *self = SHELL_AUDIO_DEVICE_SELECTION_DBUS (object);

  g_clear_pointer (&self->sender, g_free);

  G_OBJECT_CLASS (shell_audio_device_selection_dbus_parent_class)->finalize (object);
}

static void
shell_audio_device_selection_dbus_class_init (ShellAudioDeviceSelectionDBusClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_audio_device_selection_dbus_constructed;
  object_class->dispose = shell_audio_device_selection_dbus_dispose;
  object_class->finalize = shell_audio_device_selection_dbus_finalize;

  /**
   * ShellAudioDeviceSelectionDBus::overview-hide-requested:
   * @self: the #ShellAudioDeviceSelectionDBus
   *
   * Emitted when the user picked a device or opened the sound settings: the
   * overview, if it is showing, must be hidden (`Main.overview.hide ()`).
   */
  signals[OVERVIEW_HIDE_REQUESTED_SIGNAL] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_audio_device_selection_dbus_init (ShellAudioDeviceSelectionDBus *self)
{
}

/**
 * shell_audio_device_selection_dbus_new:
 *
 * Exports org.gnome.Shell.AudioDeviceSelection on the session bus and owns its
 * name (`new AudioDeviceSelectionDBus ()`).
 *
 * Returns: (transfer full): a new #ShellAudioDeviceSelectionDBus
 */
ShellAudioDeviceSelectionDBus *
shell_audio_device_selection_dbus_new (void)
{
  return g_object_new (SHELL_TYPE_AUDIO_DEVICE_SELECTION_DBUS, NULL);
}
