/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * screenshot-ui.c: The screenshot and screen recording UI, the
 * org.gnome.Shell.Screenshot D-Bus service and the helpers they share. Port of
 * js/ui/screenshot.js.
 *
 * The types, with the name they had in the JS module:
 *
 *  - #ShellScreenshotUI: `ScreenshotUI` (final #StWidget). The private
 *    classes it is made of are `UIAreaIndicator`, `UIAreaSelector`,
 *    `UIWindowSelectorWindowContent`, `UIWindowSelectorWindow`,
 *    `UIWindowSelector` (laid out by #ShellWindowSlotLayout, was
 *    `UIWindowSelectorLayout`) and `Tooltip`; `IconLabelButton` is a function.
 *  - #ShellScreenshotService: `ScreenshotService` (final #GObject), the
 *    org.gnome.Shell.Screenshot skeleton generated from
 *    data/dbus-interfaces/org.gnome.Shell.Screenshot.xml.
 *  - #ShellSelectArea (`SelectArea`), #ShellPickPixel (`PickPixel`, with the
 *    private `RecolorEffect`) and #ShellFlashspot (`Flashspot`).
 *  - shell_screenshot_capture_screenshot_async () is `captureScreenshot ()`;
 *    `_storeScreenshot ()` and the screenshot notification source are private.
 *
 * Differences from the JS module, other than the renames above:
 *
 *  - `Main.screenshotUI` is created by main.js with
 *    `new Shell.ScreenshotUI ({window_manager: wm})`: the window manager is
 *    the one the four key bindings (`show-screenshot-ui`,
 *    `show-screen-recording-ui`, `screenshot-window` and `screenshot`) are
 *    added to, and `Main.overview.hide ()` is its
 *    ::overview-hide-requested signal. `new Screenshot.ScreenshotService ()` is
 *    `new Shell.ScreenshotService ({screenshot_ui: Main.screenshotUI})`.
 *  - `open ()` and `stopScreencast ()` were async functions nobody awaited
 *    (they still reported their errors themselves): they are now
 *    shell_screenshot_ui_open () and shell_screenshot_ui_stop_screencast (),
 *    plain void functions. `showScreenshotUI ()` and `showScreenRecordingUI ()`
 *    are `open (SCREENSHOT)` and `open (SCREENCAST)`. `UIMode` is
 *    #ShellScreenshotUIMode.
 *  - `captureScreenshot (texture, geometry, scale, cursor)` takes the
 *    cursor as four arguments and the new #ShellScreenshotKind, which the
 *    `screenshot.taken` hook reports; it returns the #GFile the screenshot
 *    was saved to (%NULL if saving to disk is disabled) in
 *    shell_screenshot_capture_screenshot_finish ().
 *  - The classes `Tooltip`, `SelectArea`, `PickPixel` and `Flashspot` were
 *    exported but used by nothing else. #ShellSelectArea and #ShellPickPixel
 *    are asynchronous functions (`selectAsync ()` and `pickAsync ()`) with a
 *    _finish () that fails with G_IO_ERROR_CANCELLED when the user dismissed
 *    the grab, where the JS resolved with null.
 *  - The Screencast proxy is generated (ShellDBusScreencast); the error
 *    domain of js/misc/dbusErrors.js is not needed, the D-Bus error names
 *    of the screencast service are compared instead.
 *  - The UI had a GSettings object for org.gnome.desktop.lockdown it never
 *    used: it is gone.
 *  - A screenshot saved to a file is written with g_output_stream_write_all ()
 *    and the file closed (the JS did a single write () and let the garbage
 *    collector close it). The screencast proxy and the stage screenshot are
 *    requested asynchronously; the open () continuation is
 *    stage_content_ready_cb ().
 *  - Errors that the JS handled with `logError ()` are g_warning ()s; the
 *    ones it handled with `log ()` are g_message ()s (a missing Screencast
 *    service must not warn at startup).
 *  - Asynchronous completions are dropped after ShellGlobal::shutdown (the
 *    service fails its pending D-Bus calls with G_IO_ERROR_CANCELLED first).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>
#include <string.h>

#include <clutter/clutter.h>
/* clutter.h does not include the motion controller (mutter 51) */
#define __CLUTTER_H_INSIDE__
#include <clutter/clutter-motion-controller.h>
#undef __CLUTTER_H_INSIDE__
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gi18n-lib.h>
#include <meta/display.h>
#include <meta/meta-backend.h>
#include <meta/meta-context.h>
#include <meta/meta-sound-player.h>
#include <meta/meta-window-actor.h>
#include <meta/prefs.h>
#include <meta/window.h>
#include <meta/workspace.h>

#include "misc/dbus-utils.h"
#include "shell-action-modes.h"
#include "shell-global.h"
#include "shell-hooks.h"
#include "shell-util.h"
#include "ui/environment.h"
#include "ui/grab-helper.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/message-tray.h"
#include "ui/panel.h"
#include "ui/screenshot-ui.h"
#include "ui/session-mode.h"
#include "ui/workspace.h"

#include "screenshot-dbus.h"

#define SCREENSHOT_OBJECT_PATH "/org/gnome/Shell/Screenshot"
#define SCREENSHOT_BUS_NAME "org.gnome.Shell.Screenshot"
#define SCREENCAST_OBJECT_PATH "/org/gnome/Shell/Screencast"
#define SCREENCAST_BUS_NAME "org.gnome.Shell.Screencast"

#define LOCKDOWN_SCHEMA "org.gnome.desktop.lockdown"
#define DISABLE_SAVE_TO_DISK_KEY "disable-save-to-disk"
#define KEYBINDINGS_SCHEMA "org.gnome.shell.keybindings"

#define CTRL_SELECTION_KEYBOARD_INCREMENT 1
#define SELECTION_KEYBOARD_INCREMENT 5

#define UI_ANIMATION_TIME 200 /* milliseconds, the literal of the JS */
#define FLASHSPOT_ANIMATION_OUT_TIME 500 /* milliseconds */

#define TOOLTIP_OPEN_DELAY 300 /* milliseconds */
#define TOOLTIP_OPEN_TIME 150 /* milliseconds */
#define TOOLTIP_CLOSE_TIME 100 /* milliseconds */

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

/* Set once ShellGlobal::shutdown was emitted: the completions of the
 * operations that were pending then must not touch the shell any more. */
static gboolean shutdown_seen = FALSE;

/* The window manager of the screenshot UI, whose ::overview-hide-requested
 * signal is `Main.overview.hide ()` for the notifications (weak) */
static ShellWindowManager *overview_window_manager = NULL;

static void
global_shutdown_cb (ShellGlobal *global,
                    gpointer     user_data)
{
  shutdown_seen = TRUE;
}

static void
ensure_shutdown_watch (void)
{
  static gsize watching = 0;

  if (g_once_init_enter (&watching))
    {
      g_signal_connect (shell_global_get (), "shutdown",
                        G_CALLBACK (global_shutdown_cb), NULL);
      g_once_init_leave (&watching, 1);
    }
}

static ShellLayoutManager *
get_layout_manager (void)
{
  return shell_main_get_layout_manager (shell_main_get_default ());
}

/* A boolean field of the session mode (`Main.sessionMode.hasWindows`) */
static gboolean
get_session_mode_flag (const char *name)
{
  ShellSessionMode *session_mode =
    shell_main_get_session_mode (shell_main_get_default ());
  g_autoptr (GVariant) value = NULL;

  if (session_mode == NULL)
    return FALSE;

  value = shell_session_mode_get_field (session_mode, name);

  return value != NULL &&
         g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN) &&
         g_variant_get_boolean (value);
}

static int
get_scale_factor (void)
{
  return st_theme_context_get_scale_factor (st_theme_context_get_for_stage (shell_global_get_stage (shell_global_get ())));
}

/* `Main.overview.hide (); Main.panel.close_calendar ();` */
static void
hide_overview_and_close_calendar (void)
{
  ShellPanel *panel = shell_main_get_panel (shell_main_get_default ());

  if (overview_window_manager != NULL)
    g_signal_emit_by_name (overview_window_manager, "overview-hide-requested");

  if (panel != NULL)
    shell_panel_close_calendar (panel);
}

static void
ease_opacity (ClutterActor     *actor,
              guint             opacity,
              int               duration,
              ShellEaseCallback callback,
              gpointer          user_data,
              GDestroyNotify    destroy)
{
  ShellEaseParams params = {
    .duration = duration,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  shell_actor_ease (actor, &params, callback, user_data, destroy,
                    "opacity", opacity,
                    NULL);
}

static void
set_actor_visible (ClutterActor *actor,
                   gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

static void
add_bind_constraint (ClutterActor        *actor,
                     ClutterActor        *source,
                     ClutterBindCoordinate coordinate)
{
  clutter_actor_add_constraint (actor,
                                clutter_bind_constraint_new (source, coordinate, 0));
}

static void
add_snap_constraint (ClutterActor    *actor,
                     ClutterActor    *source,
                     ClutterSnapEdge  from_edge,
                     ClutterSnapEdge  to_edge)
{
  clutter_actor_add_constraint (actor,
                                clutter_snap_constraint_new (source, from_edge, to_edge, 0));
}

/* Moves @actor to @new_parent at @index without dropping its last
 * reference in between */
static void
reparent_actor_at_index (ClutterActor *actor,
                         ClutterActor *new_parent,
                         int           index)
{
  ClutterActor *parent = clutter_actor_get_parent (actor);

  g_object_ref (actor);
  if (parent != NULL)
    clutter_actor_remove_child (parent, actor);
  clutter_actor_insert_child_at_index (new_parent, actor, index);
  g_object_unref (actor);
}

static int
find_checked_index (GPtrArray *buttons)
{
  guint i;

  for (i = 0; i < buttons->len; i++)
    {
      if (st_button_get_checked (g_ptr_array_index (buttons, i)))
        return (int) i;
    }

  return -1;
}

/* ------------------------------------------------------------------------ */
/* The screenshot notification source                                        */
/* ------------------------------------------------------------------------ */

/* `screenshotNotificationSource`, kept until it is destroyed */
static ShellSource *screenshot_notification_source = NULL;

static void
screenshot_source_destroy_cb (ShellSource                      *source,
                              ShellNotificationDestroyedReason  reason,
                              gpointer                          user_data)
{
  if (screenshot_notification_source == source)
    {
      g_signal_handlers_disconnect_by_func (source, screenshot_source_destroy_cb, NULL);
      g_clear_object (&screenshot_notification_source);
    }
}

static void
screenshot_source_shutdown_cb (ShellGlobal *global,
                               ShellSource *source)
{
  /* The message tray owns the source from now on */
  if (screenshot_notification_source == source)
    {
      g_signal_handlers_disconnect_by_func (source, screenshot_source_destroy_cb, NULL);
      g_clear_object (&screenshot_notification_source);
    }
}

/* Returns: (transfer none) (nullable): the source, %NULL if there is no
 *   message tray yet */
static ShellSource *
get_screenshot_notification_source (void)
{
  if (screenshot_notification_source == NULL)
    {
      ShellMessageTray *message_tray =
        shell_main_get_message_tray (shell_main_get_default ());
      ShellSource *source;

      if (message_tray == NULL)
        return NULL;

      source = g_object_new (SHELL_TYPE_SOURCE,
                             /* Translators: notification source name for screenshots and recordings. */
                             "title", _("Screen Capture"),
                             "icon-name", "screenshooter-symbolic",
                             NULL);
      g_signal_connect (source, "destroy",
                        G_CALLBACK (screenshot_source_destroy_cb), NULL);
      g_signal_connect_object (shell_global_get (), "shutdown",
                               G_CALLBACK (screenshot_source_shutdown_cb), source, 0);
      shell_message_tray_add (message_tray, source);

      screenshot_notification_source = source;
    }

  return screenshot_notification_source;
}

static void
play_screenshot_sound (void)
{
  ShellGlobal *global = shell_global_get ();
  MetaSoundPlayer *player = meta_display_get_sound_player (shell_global_get_display (global));
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;

  if (player == NULL)
    return;

  path = g_strdup_printf ("%s/sounds/screen-capture.oga",
                          shell_global_get_datadir (global));
  file = g_file_new_for_path (path);
  meta_sound_player_play_from_file (player, file, _("Screenshot taken"), NULL);
}

/* The "Show in Files" action of the notifications */
static void
show_in_files (GFile *file)
{
  g_autoptr (GAppInfo) app = g_app_info_get_default_for_type ("inode/directory", FALSE);
  g_autoptr (GAppLaunchContext) context = NULL;
  g_autoptr (GError) error = NULL;
  GList files = { file, NULL, NULL };

  if (app == NULL)
    {
      /* It may be null e.g. in a toolbox without nautilus. */
      g_message ("Error showing in files: no default app set for inode/directory");
      return;
    }

  context = shell_global_create_app_launch_context (shell_global_get (), 0, -1);
  if (!g_app_info_launch (app, &files, context, &error))
    g_warning ("Error showing in files: %s", error->message);
}

static void
show_in_files_action_cb (ShellNotification *notification,
                         gpointer           user_data)
{
  show_in_files (G_FILE (user_data));
}

/* Opens @file with the default application */
static void
open_file_default (GFile      *file,
                   const char *what)
{
  g_autoptr (GAppLaunchContext) context = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *uri = g_file_get_uri (file);

  context = shell_global_create_app_launch_context (shell_global_get (), 0, -1);
  if (!g_app_info_launch_default_for_uri (uri, context, &error))
    g_warning ("Error opening %s: %s", what, error->message);
}

static void
screenshot_notification_activated_cb (ShellNotification *notification,
                                      GFile             *file)
{
  open_file_default (file, "screenshot");

  hide_overview_and_close_calendar ();
}

/* ------------------------------------------------------------------------ */
/* _storeScreenshot (), captureScreenshot ()                                 */
/* ------------------------------------------------------------------------ */

/* Adds a record of a screenshot file in the recently used files list */
static void
save_recent_file (GFile *screenshot_file)
{
  g_autofree char *recent_file =
    g_build_filename (g_get_user_data_dir (), "recently-used.xbel", NULL);
  g_autofree char *uri = g_file_get_uri (screenshot_file);
  g_autoptr (GBookmarkFile) bookmarks = g_bookmark_file_new ();
  g_autoptr (GError) error = NULL;

  if (!g_bookmark_file_load_from_file (bookmarks, recent_file, &error))
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_message ("Could not open recent file %s: %s", uri, error->message);
          return;
        }

      g_clear_error (&error);
    }

  g_bookmark_file_add_application (bookmarks, uri, g_get_prgname (), "gio open %u");
  if (!g_bookmark_file_to_file (bookmarks, recent_file, &error))
    g_message ("Could not save recent file %s: %s", uri, error->message);
}

static const char *
screenshot_kind_to_string (ShellScreenshotKind kind)
{
  switch (kind)
    {
    case SHELL_SCREENSHOT_KIND_AREA:
      return "area";
    case SHELL_SCREENSHOT_KIND_WINDOW:
      return "window";
    case SHELL_SCREENSHOT_KIND_SCREEN:
    default:
      return "screen";
    }
}

static void
declare_hooks (void)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();

  shell_hook_registry_declare (registry, "screenshot.taken",
                               "kind:s clipboard:b file:s", FALSE,
                               "A screenshot was taken and stored in the "
                               "clipboard, and in a file unless saving to "
                               "disk is disabled");
}

static void
emit_screenshot_taken_hook (ShellScreenshotKind  kind,
                            GFile               *file)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  GVariantBuilder builder;

  if (!shell_hook_registry_is_declared (registry, "screenshot.taken"))
    return;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "kind",
                         g_variant_new_string (screenshot_kind_to_string (kind)));
  g_variant_builder_add (&builder, "{sv}", "clipboard", g_variant_new_boolean (TRUE));
  if (file != NULL)
    {
      g_autofree char *path = g_file_get_path (file);

      if (path != NULL)
        g_variant_builder_add (&builder, "{sv}", "file", g_variant_new_string (path));
    }

  shell_hook_registry_emit_after (registry, "screenshot.taken",
                                  g_variant_builder_end (&builder));
}

/*
 * Stores a PNG-encoded screenshot into the clipboard and a file, and shows a
 * notification.
 *
 * Returns: (transfer full) (nullable): the file the screenshot was saved to,
 *   %NULL if saving to disk is disabled or on error
 */
static GFile *
store_screenshot (GBytes               *bytes,
                  GdkPixbuf            *pixbuf,
                  ShellScreenshotKind   kind,
                  GError              **error)
{
  g_autoptr (GSettings) lockdown_settings = NULL;
  g_autoptr (GDateTime) time = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GBytes) pixels = NULL;
  g_autoptr (ClutterContent) content = NULL;
  g_autoptr (GError) content_error = NULL;
  g_autoptr (ShellNotification) notification = NULL;
  ClutterBackend *backend;
  ShellSource *source;
  gboolean disable_save_to_disk;
  int width, height;

  /* Store to the clipboard first in case storing to file fails. */
  st_clipboard_set_content (st_clipboard_get_default (), ST_CLIPBOARD_TYPE_CLIPBOARD,
                            "image/png", bytes);

  time = g_date_time_new_now_local ();

  lockdown_settings = g_settings_new (LOCKDOWN_SCHEMA);
  disable_save_to_disk = g_settings_get_boolean (lockdown_settings,
                                                 DISABLE_SAVE_TO_DISK_KEY);

  if (!disable_save_to_disk)
    {
      g_autofree char *pictures = NULL;
      g_autofree char *dir_path = NULL;
      g_autofree char *timestamp = NULL;
      g_autofree char *name = NULL;
      g_autoptr (GFile) dir = NULL;
      g_autoptr (GError) dir_error = NULL;
      const char *pictures_dir;
      int i;

      pictures_dir = g_get_user_special_dir (G_USER_DIRECTORY_PICTURES);
      if (pictures_dir == NULL || pictures_dir[0] == '\0')
        pictures_dir = g_get_home_dir ();

      dir_path = g_build_filename (pictures_dir,
                                   /* Translators: name of the folder under ~/Pictures for screenshots. */
                                   _("Screenshots"),
                                   NULL);
      dir = g_file_new_for_path (dir_path);

      if (!g_file_make_directory_with_parents (dir, NULL, &dir_error) &&
          !g_error_matches (dir_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
          g_propagate_error (error, g_steal_pointer (&dir_error));
          return NULL;
        }

      timestamp = g_date_time_format (time, "%Y-%m-%d %H-%M-%S");
      /* Translators: this is the name of the file that the screenshot is
       * saved to. The placeholder is a timestamp, e.g. "2017-05-21 12-24-03". */
      name = g_strdup_printf (_("Screenshot From %s"), timestamp);

      /* If the target file already exists, try appending a suffix with an
       * increasing number to it. */
      for (i = 0; ; i++)
        {
          g_autofree char *suffix = i == 0 ? g_strdup ("") : g_strdup_printf ("-%d", i);
          g_autofree char *basename = g_strdup_printf ("%s%s.png", name, suffix);
          g_autofree char *path = g_build_filename (dir_path, basename, NULL);
          g_autoptr (GFileOutputStream) stream = NULL;
          g_autoptr (GError) create_error = NULL;
          gsize written;

          g_clear_object (&file);
          file = g_file_new_for_path (path);

          stream = g_file_create (file, G_FILE_CREATE_NONE, NULL, &create_error);
          if (stream == NULL)
            {
              if (g_error_matches (create_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
                continue;

              g_propagate_error (error, g_steal_pointer (&create_error));
              return NULL;
            }

          if (!g_output_stream_write_all (G_OUTPUT_STREAM (stream),
                                          g_bytes_get_data (bytes, NULL),
                                          g_bytes_get_size (bytes),
                                          &written, NULL, error) ||
              !g_output_stream_close (G_OUTPUT_STREAM (stream), NULL, error))
            return NULL;

          break;
        }

      /* Add it to recent files. */
      save_recent_file (file);
    }

  /* Create a St.ImageContent icon for the notification. We want
   * St.ImageContent specifically because it preserves the aspect ratio when
   * shown in a notification. */
  backend = clutter_context_get_backend (clutter_actor_get_context (CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ()))));
  pixels = gdk_pixbuf_read_pixel_bytes (pixbuf);
  width = gdk_pixbuf_get_width (pixbuf);
  height = gdk_pixbuf_get_height (pixbuf);
  content = st_image_content_new_with_preferred_size (width, height);
  if (!st_image_content_set_bytes (ST_IMAGE_CONTENT (content),
                                   clutter_backend_get_cogl_context (backend),
                                   pixels,
                                   COGL_PIXEL_FORMAT_RGBA_8888,
                                   width, height,
                                   gdk_pixbuf_get_rowstride (pixbuf),
                                   &content_error))
    {
      g_message ("Error creating the icon of the screenshot notification: %s",
                 content_error->message);
      g_clear_object (&content);
    }

  /* Show a notification. */
  source = get_screenshot_notification_source ();
  if (source != NULL)
    {
      /* Translators: notification title. */
      notification = shell_notification_new (source, _("Screenshot Captured"),
                                             /* Translators: notification body when a screenshot was captured. */
                                             _("You can paste the image from the clipboard"));
      shell_notification_set_datetime (notification, time);
      if (content != NULL)
        shell_notification_set_gicon (notification, G_ICON (content));
      shell_notification_set_is_transient (notification, TRUE);

      if (!disable_save_to_disk)
        {
          /* Translators: button on the screenshot notification. */
          shell_notification_add_action (notification, _("Show in Files"),
                                         show_in_files_action_cb,
                                         g_object_ref (file), g_object_unref);
          g_signal_connect_data (notification, "activated",
                                 G_CALLBACK (screenshot_notification_activated_cb),
                                 g_object_ref (file), (GClosureNotify) g_object_unref, 0);
        }

      shell_source_add_notification (source, notification);
    }

  emit_screenshot_taken_hook (kind, file);

  return g_steal_pointer (&file);
}

typedef struct
{
  GOutputStream *stream;
  ShellScreenshotKind kind;
} CaptureData;

static void
capture_data_free (CaptureData *data)
{
  g_clear_object (&data->stream);
  g_free (data);
}

static void
composite_to_stream_done_cb (GObject      *source,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  CaptureData *data = g_task_get_task_data (task);
  g_autoptr (GdkPixbuf) pixbuf = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (GError) error = NULL;
  GFile *file;

  pixbuf = shell_screenshot_composite_to_stream_finish (result, &error);
  if (pixbuf == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  if (shutdown_seen)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "The shell is shutting down");
      return;
    }

  if (!g_output_stream_close (data->stream, NULL, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  bytes = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (data->stream));
  file = store_screenshot (bytes, pixbuf, data->kind, &error);
  if (file == NULL && error != NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_pointer (task, file, g_object_unref);
}

/**
 * shell_screenshot_capture_screenshot_async:
 * @texture: the texture to take the screenshot from
 * @geometry: (nullable): the region to use, or %NULL for the whole texture
 * @scale: the scale of @texture
 * @cursor_texture: (nullable): the texture of the cursor to include in the
 *   screenshot, if any
 * @cursor_x: the x coordinate of the cursor, relative to @texture, in pixels
 * @cursor_y: the y coordinate of the cursor, relative to @texture, in pixels
 * @cursor_scale: the scale of @cursor_texture
 * @kind: what the screenshot is a screenshot of
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async) (nullable): function to call when the screenshot
 *   was stored
 * @user_data: the data to pass to @callback
 *
 * Captures a screenshot from a texture: plays the sound of the screenshot,
 * composites @texture, @geometry and the cursor into a PNG, stores it in the
 * clipboard and a file, and shows a notification. This is
 * `Screenshot.captureScreenshot ()`.
 */
void
shell_screenshot_capture_screenshot_async (CoglTexture         *texture,
                                           const MtkRectangle  *geometry,
                                           float                scale,
                                           CoglTexture         *cursor_texture,
                                           int                  cursor_x,
                                           int                  cursor_y,
                                           float                cursor_scale,
                                           ShellScreenshotKind  kind,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;
  CaptureData *data;
  int x = 0, y = 0, width = -1, height = -1;

  g_return_if_fail (COGL_IS_TEXTURE (texture));
  g_return_if_fail (cursor_texture == NULL || COGL_IS_TEXTURE (cursor_texture));

  ensure_shutdown_watch ();

  task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, shell_screenshot_capture_screenshot_async);

  data = g_new0 (CaptureData, 1);
  data->stream = g_memory_output_stream_new_resizable ();
  data->kind = kind;
  g_task_set_task_data (task, data, (GDestroyNotify) capture_data_free);

  play_screenshot_sound ();

  if (geometry != NULL)
    {
      x = geometry->x;
      y = geometry->y;
      width = geometry->width;
      height = geometry->height;
    }

  if (cursor_texture == NULL)
    {
      cursor_x = 0;
      cursor_y = 0;
      cursor_scale = 1;
    }

  shell_screenshot_composite_to_stream (texture,
                                        x, y, width, height,
                                        scale,
                                        cursor_texture, cursor_x, cursor_y, cursor_scale,
                                        data->stream,
                                        composite_to_stream_done_cb,
                                        g_steal_pointer (&task));
}

/**
 * shell_screenshot_capture_screenshot_finish:
 * @result: the #GAsyncResult that was provided to the callback
 * @error: #GError for error reporting
 *
 * Finish the asynchronous operation started by
 * shell_screenshot_capture_screenshot_async ().
 *
 * Returns: (transfer full) (nullable): the file the screenshot was saved to,
 *   or %NULL with @error unset if saving to disk is disabled
 */
GFile *
shell_screenshot_capture_screenshot_finish (GAsyncResult  *result,
                                            GError       **error)
{
  g_return_val_if_fail (G_IS_TASK (result), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (result,
                                                  shell_screenshot_capture_screenshot_async),
                        NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ------------------------------------------------------------------------ */
/* IconLabelButton                                                           */
/* ------------------------------------------------------------------------ */

static StButton *
icon_label_button_new (const char *icon_name,
                       const char *label)
{
  StButton *button;
  ClutterActor *container;
  ClutterActor *label_actor;

  button = g_object_new (ST_TYPE_BUTTON,
                         "style-class", "screenshot-ui-type-button",
                         "x-expand", TRUE,
                         NULL);

  container = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "orientation", CLUTTER_ORIENTATION_VERTICAL,
                            "style-class", "icon-label-button-container",
                            NULL);
  st_bin_set_child (ST_BIN (button), container);

  clutter_actor_add_child (container,
                           g_object_new (ST_TYPE_ICON,
                                         "icon-name", icon_name,
                                         NULL));

  label_actor = CLUTTER_ACTOR (st_label_new (label));
  clutter_actor_set_x_align (label_actor, CLUTTER_ACTOR_ALIGN_CENTER);
  st_widget_set_label_actor (ST_WIDGET (button), label_actor);
  clutter_actor_add_child (container, label_actor);

  return button;
}

/* ------------------------------------------------------------------------ */
/* Tooltip                                                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREENSHOT_UI_TOOLTIP (shell_screenshot_ui_tooltip_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenshotUITooltip, shell_screenshot_ui_tooltip,
                      SHELL, SCREENSHOT_UI_TOOLTIP, StLabel)

struct _ShellScreenshotUITooltip
{
  StLabel parent;

  StWidget *widget; /* weak */
  guint timeout_id;
};

G_DEFINE_FINAL_TYPE (ShellScreenshotUITooltip, shell_screenshot_ui_tooltip,
                     ST_TYPE_LABEL)

static void
tooltip_hide_complete_cb (gboolean  finished,
                          gpointer  user_data)
{
  if (finished)
    clutter_actor_hide (CLUTTER_ACTOR (user_data));
}

static void
tooltip_open_timeout_cb (gpointer user_data)
{
  ShellScreenshotUITooltip *self = user_data;
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());
  StThemeNode *node;
  graphene_rect_t extents;
  float x_offset, x, y, y_offset;

  self->timeout_id = 0;

  if (self->widget == NULL)
    return;

  clutter_actor_set_opacity (actor, 0);
  clutter_actor_show (actor);

  clutter_actor_get_transformed_extents (CLUTTER_ACTOR (self->widget), &extents);

  x_offset = floorf ((extents.size.width - clutter_actor_get_width (actor)) / 2);
  x = CLAMP (extents.origin.x + x_offset, 0,
             clutter_actor_get_width (CLUTTER_ACTOR (stage)) - clutter_actor_get_width (actor));

  node = st_widget_get_theme_node (ST_WIDGET (self));
  y_offset = st_theme_node_get_length (node, "-y-offset");

  y = extents.origin.y - clutter_actor_get_height (actor) - y_offset;

  clutter_actor_set_position (actor, x, y);
  ease_opacity (actor, 255, TOOLTIP_OPEN_TIME, NULL, NULL, NULL);
}

static void
tooltip_open (ShellScreenshotUITooltip *self)
{
  if (self->timeout_id != 0)
    return;

  self->timeout_id = g_timeout_add_once (TOOLTIP_OPEN_DELAY,
                                         tooltip_open_timeout_cb, self);
  g_source_set_name_by_id (self->timeout_id, "[gnome-shell] tooltip.open");
}

static void
tooltip_close (ShellScreenshotUITooltip *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  if (self->timeout_id != 0)
    {
      g_clear_handle_id (&self->timeout_id, g_source_remove);
      return;
    }

  if (!clutter_actor_is_visible (actor))
    return;

  clutter_actor_remove_all_transitions (actor);
  ease_opacity (actor, 0, TOOLTIP_CLOSE_TIME,
                tooltip_hide_complete_cb, g_object_ref (self), g_object_unref);
}

static void
tooltip_hover_notify_cb (StWidget                 *widget,
                         GParamSpec               *pspec,
                         ShellScreenshotUITooltip *self)
{
  if (st_widget_get_hover (widget))
    tooltip_open (self);
  else
    tooltip_close (self);
}

static void
tooltip_shutdown_cb (ShellGlobal              *global,
                     ShellScreenshotUITooltip *self)
{
  g_clear_handle_id (&self->timeout_id, g_source_remove);
}

static void
shell_screenshot_ui_tooltip_dispose (GObject *object)
{
  ShellScreenshotUITooltip *self = SHELL_SCREENSHOT_UI_TOOLTIP (object);

  g_clear_handle_id (&self->timeout_id, g_source_remove);
  g_clear_weak_pointer (&self->widget);

  G_OBJECT_CLASS (shell_screenshot_ui_tooltip_parent_class)->dispose (object);
}

static void
shell_screenshot_ui_tooltip_class_init (ShellScreenshotUITooltipClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_screenshot_ui_tooltip_dispose;
}

static void
shell_screenshot_ui_tooltip_init (ShellScreenshotUITooltip *self)
{
}

/* Returns: (transfer floating): a tooltip shown while @widget is hovered */
static ClutterActor *
tooltip_new (StWidget   *widget,
             const char *text)
{
  ShellScreenshotUITooltip *self;

  self = g_object_new (SHELL_TYPE_SCREENSHOT_UI_TOOLTIP,
                       "text", text,
                       "style-class", "screenshot-ui-tooltip",
                       "visible", FALSE,
                       NULL);

  g_set_weak_pointer (&self->widget, widget);
  g_signal_connect_object (widget, "notify::hover",
                           G_CALLBACK (tooltip_hover_notify_cb), self, 0);
  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (tooltip_shutdown_cb), self, 0);

  if (st_widget_get_label_actor (widget) == NULL)
    st_widget_set_label_actor (widget, CLUTTER_ACTOR (self));

  return CLUTTER_ACTOR (self);
}

/* ------------------------------------------------------------------------ */
/* UIAreaIndicator                                                           */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREENSHOT_UI_AREA_INDICATOR (shell_screenshot_ui_area_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenshotUIAreaIndicator, shell_screenshot_ui_area_indicator,
                      SHELL, SCREENSHOT_UI_AREA_INDICATOR, StWidget)

struct _ShellScreenshotUIAreaIndicator
{
  StWidget parent;

  ClutterActor *top_rect;
  ClutterActor *bottom_rect;
  ClutterActor *left_rect;
  ClutterActor *right_rect;
  ClutterActor *selection_rect;
};

G_DEFINE_FINAL_TYPE (ShellScreenshotUIAreaIndicator, shell_screenshot_ui_area_indicator,
                     ST_TYPE_WIDGET)

static ClutterActor *
area_indicator_new_shade (ClutterActor *parent)
{
  ClutterActor *shade;

  shade = g_object_new (ST_TYPE_WIDGET,
                        "style-class", "screenshot-ui-area-indicator-shade",
                        NULL);
  add_bind_constraint (shade, parent, CLUTTER_BIND_WIDTH);

  return shade;
}

static void
shell_screenshot_ui_area_indicator_constructed (GObject *object)
{
  ShellScreenshotUIAreaIndicator *self = SHELL_SCREENSHOT_UI_AREA_INDICATOR (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);

  G_OBJECT_CLASS (shell_screenshot_ui_area_indicator_parent_class)->constructed (object);

  self->top_rect = area_indicator_new_shade (actor);
  add_snap_constraint (self->top_rect, actor, CLUTTER_SNAP_EDGE_TOP, CLUTTER_SNAP_EDGE_TOP);
  add_snap_constraint (self->top_rect, actor, CLUTTER_SNAP_EDGE_LEFT, CLUTTER_SNAP_EDGE_LEFT);
  clutter_actor_add_child (actor, self->top_rect);

  self->bottom_rect = area_indicator_new_shade (actor);
  add_snap_constraint (self->bottom_rect, actor, CLUTTER_SNAP_EDGE_BOTTOM, CLUTTER_SNAP_EDGE_BOTTOM);
  add_snap_constraint (self->bottom_rect, actor, CLUTTER_SNAP_EDGE_LEFT, CLUTTER_SNAP_EDGE_LEFT);
  clutter_actor_add_child (actor, self->bottom_rect);

  self->left_rect = g_object_new (ST_TYPE_WIDGET,
                                  "style-class", "screenshot-ui-area-indicator-shade",
                                  NULL);
  add_snap_constraint (self->left_rect, actor, CLUTTER_SNAP_EDGE_LEFT, CLUTTER_SNAP_EDGE_LEFT);
  add_snap_constraint (self->left_rect, self->top_rect, CLUTTER_SNAP_EDGE_TOP, CLUTTER_SNAP_EDGE_BOTTOM);
  add_snap_constraint (self->left_rect, self->bottom_rect, CLUTTER_SNAP_EDGE_BOTTOM, CLUTTER_SNAP_EDGE_TOP);
  clutter_actor_add_child (actor, self->left_rect);

  self->right_rect = g_object_new (ST_TYPE_WIDGET,
                                   "style-class", "screenshot-ui-area-indicator-shade",
                                   NULL);
  add_snap_constraint (self->right_rect, actor, CLUTTER_SNAP_EDGE_RIGHT, CLUTTER_SNAP_EDGE_RIGHT);
  add_snap_constraint (self->right_rect, self->top_rect, CLUTTER_SNAP_EDGE_TOP, CLUTTER_SNAP_EDGE_BOTTOM);
  add_snap_constraint (self->right_rect, self->bottom_rect, CLUTTER_SNAP_EDGE_BOTTOM, CLUTTER_SNAP_EDGE_TOP);
  clutter_actor_add_child (actor, self->right_rect);

  self->selection_rect = g_object_new (ST_TYPE_WIDGET,
                                       "style-class", "screenshot-ui-area-indicator-selection",
                                       NULL);
  clutter_actor_add_child (actor, self->selection_rect);

  add_snap_constraint (self->top_rect, self->selection_rect,
                       CLUTTER_SNAP_EDGE_BOTTOM, CLUTTER_SNAP_EDGE_TOP);
  add_snap_constraint (self->bottom_rect, self->selection_rect,
                       CLUTTER_SNAP_EDGE_TOP, CLUTTER_SNAP_EDGE_BOTTOM);
  add_snap_constraint (self->left_rect, self->selection_rect,
                       CLUTTER_SNAP_EDGE_RIGHT, CLUTTER_SNAP_EDGE_LEFT);
  add_snap_constraint (self->right_rect, self->selection_rect,
                       CLUTTER_SNAP_EDGE_LEFT, CLUTTER_SNAP_EDGE_RIGHT);
}

static void
shell_screenshot_ui_area_indicator_class_init (ShellScreenshotUIAreaIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_screenshot_ui_area_indicator_constructed;
}

static void
shell_screenshot_ui_area_indicator_init (ShellScreenshotUIAreaIndicator *self)
{
}

static void
area_indicator_set_selection_rect (ShellScreenshotUIAreaIndicator *self,
                                   float                           x,
                                   float                           y,
                                   float                           width,
                                   float                           height)
{
  clutter_actor_set_position (self->selection_rect, x, y);
  clutter_actor_set_size (self->selection_rect, width, height);
}

/* ------------------------------------------------------------------------ */
/* UIAreaSelector                                                            */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREENSHOT_UI_AREA_SELECTOR (shell_screenshot_ui_area_selector_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenshotUIAreaSelector, shell_screenshot_ui_area_selector,
                      SHELL, SCREENSHOT_UI_AREA_SELECTOR, StWidget)

enum
{
  SELECTOR_DRAG_STARTED,
  SELECTOR_DRAG_ENDED,
  SELECTOR_LAST_SIGNAL
};

static guint selector_signals[SELECTOR_LAST_SIGNAL] = { 0, };

struct _ShellScreenshotUIAreaSelector
{
  StWidget parent;

  ShellScreenshotUIAreaIndicator *area_indicator;
  ClutterActor *top_left_handle;
  ClutterActor *top_right_handle;
  ClutterActor *bottom_left_handle;
  ClutterActor *bottom_right_handle;

  ClutterPanGesture *pan_gesture;
  ClutterMotionController *motion_controller;

  float handle_size;

  double start_x;
  double start_y;
  double last_x;
  double last_y;

  StDirectionType current_side;
  StDirectionType last_resize_direction;

  ClutterGrab *drag_grab;
  ClutterCursorType drag_cursor;
  double drag_start_x;
  double drag_start_y;
};

G_DEFINE_FINAL_TYPE (ShellScreenshotUIAreaSelector, shell_screenshot_ui_area_selector,
                     ST_TYPE_WIDGET)

static void area_selector_reset (ShellScreenshotUIAreaSelector *self);
static void area_selector_reset_area (ShellScreenshotUIAreaSelector *self);
static void area_selector_stop_drag (ShellScreenshotUIAreaSelector *self);

static void
area_selector_get_geometry (ShellScreenshotUIAreaSelector *self,
                            double                        *x,
                            double                        *y,
                            double                        *width,
                            double                        *height)
{
  double left_x = MIN (self->start_x, self->last_x);
  double top_y = MIN (self->start_y, self->last_y);
  double right_x = MAX (self->start_x, self->last_x);
  double bottom_y = MAX (self->start_y, self->last_y);

  *x = left_x;
  *y = top_y;
  *width = right_x - left_x + 1;
  *height = bottom_y - top_y + 1;
}

static void
area_selector_update_selection_rect (ShellScreenshotUIAreaSelector *self)
{
  double x, y, w, h, offset;

  area_selector_get_geometry (self, &x, &y, &w, &h);
  area_indicator_set_selection_rect (self->area_indicator, x, y, w, h);

  offset = self->handle_size / 2;
  clutter_actor_set_position (self->top_left_handle, x - offset, y - offset);
  clutter_actor_set_position (self->top_right_handle, x + w - 1 - offset, y - offset);
  clutter_actor_set_position (self->bottom_left_handle, x - offset, y + h - 1 - offset);
  clutter_actor_set_position (self->bottom_right_handle, x + w - 1 - offset, y + h - 1 - offset);
}

static void
area_selector_warp_pointer (ShellScreenshotUIAreaSelector *self,
                            double                         x,
                            double                         y)
{
  ClutterBackend *backend =
    clutter_context_get_backend (clutter_actor_get_context (CLUTTER_ACTOR (self)));

  clutter_seat_warp_pointer (clutter_backend_get_default_seat (backend), (int) x, (int) y);
}

static ClutterCursorType
area_selector_compute_cursor_type (ShellScreenshotUIAreaSelector *self,
                                   double                         cursor_x,
                                   double                         cursor_y)
{
  double left_x, top_y, width, height, right_x, bottom_y;
  double x = cursor_x;
  double y = cursor_y;
  double limit, threshold;

  area_selector_get_geometry (self, &left_x, &top_y, &width, &height);
  right_x = left_x + width - 1;
  bottom_y = top_y + height - 1;

  /* Check if the cursor overlaps the handles first. */
  limit = pow (self->handle_size / 2, 2);
  if (pow (left_x - x, 2) + pow (top_y - y, 2) <= limit)
    return CLUTTER_CURSOR_NW_RESIZE;
  else if (pow (right_x - x, 2) + pow (top_y - y, 2) <= limit)
    return CLUTTER_CURSOR_NE_RESIZE;
  else if (pow (left_x - x, 2) + pow (bottom_y - y, 2) <= limit)
    return CLUTTER_CURSOR_SW_RESIZE;
  else if (pow (right_x - x, 2) + pow (bottom_y - y, 2) <= limit)
    return CLUTTER_CURSOR_SE_RESIZE;

  /* Now check the rest of the rectangle. */
  threshold = 10 * get_scale_factor ();

  if (left_x - x >= 0 && left_x - x <= threshold)
    {
      if (top_y - y >= 0 && top_y - y <= threshold)
        return CLUTTER_CURSOR_NW_RESIZE;
      else if (y - bottom_y >= 0 && y - bottom_y <= threshold)
        return CLUTTER_CURSOR_SW_RESIZE;
      else if (top_y - y < 0 && y - bottom_y < 0)
        return CLUTTER_CURSOR_W_RESIZE;
    }
  else if (x - right_x >= 0 && x - right_x <= threshold)
    {
      if (top_y - y >= 0 && top_y - y <= threshold)
        return CLUTTER_CURSOR_NE_RESIZE;
      else if (y - bottom_y >= 0 && y - bottom_y <= threshold)
        return CLUTTER_CURSOR_SE_RESIZE;
      else if (top_y - y < 0 && y - bottom_y < 0)
        return CLUTTER_CURSOR_E_RESIZE;
    }
  else if (left_x - x < 0 && x - right_x < 0)
    {
      if (top_y - y >= 0 && top_y - y <= threshold)
        return CLUTTER_CURSOR_N_RESIZE;
      else if (y - bottom_y >= 0 && y - bottom_y <= threshold)
        return CLUTTER_CURSOR_S_RESIZE;
      else if (top_y - y < 0 && y - bottom_y < 0)
        return CLUTTER_CURSOR_MOVE;
    }

  return CLUTTER_CURSOR_CROSSHAIR;
}

static void
area_selector_update_cursor (ShellScreenshotUIAreaSelector *self,
                             double                         x,
                             double                         y)
{
  clutter_actor_set_cursor_type (CLUTTER_ACTOR (self),
                                 area_selector_compute_cursor_type (self, x, y));
}

static void
area_selector_center_cursor (ShellScreenshotUIAreaSelector *self)
{
  double left_x, top_y, selection_width, selection_height, right_x, bottom_y;
  double cursor_x, cursor_y;

  area_selector_get_geometry (self, &left_x, &top_y, &selection_width, &selection_height);
  right_x = left_x + selection_width - 1;
  bottom_y = top_y + selection_height - 1;

  cursor_x = ((right_x - left_x) / 2) + left_x;
  cursor_y = ((top_y - bottom_y) / 2) + bottom_y;

  area_selector_warp_pointer (self, cursor_x, cursor_y);
  area_selector_update_cursor (self, cursor_x, cursor_y);
}

static gboolean
direction_is_vertical (StDirectionType direction)
{
  return direction == ST_DIR_UP || direction == ST_DIR_DOWN;
}

static gboolean
area_selector_maybe_change_resize_direction (ShellScreenshotUIAreaSelector *self,
                                             StDirectionType                direction)
{
  if (direction_is_vertical (direction) &&
      direction_is_vertical (self->last_resize_direction))
    return FALSE;

  if (!direction_is_vertical (direction) &&
      !direction_is_vertical (self->last_resize_direction))
    return FALSE;

  self->last_resize_direction = direction;
  self->current_side = direction;
  return TRUE;
}

static void
area_selector_move_cursor_to_side (ShellScreenshotUIAreaSelector *self,
                                   StDirectionType                direction)
{
  double left_x, top_y, width, height, right_x, bottom_y;
  double cursor_x = 0, cursor_y = 0;

  area_selector_get_geometry (self, &left_x, &top_y, &width, &height);
  right_x = left_x + width - 1;
  bottom_y = top_y + height - 1;

  switch (direction)
    {
    case ST_DIR_LEFT:
      cursor_x = self->start_x < self->last_x ? left_x : right_x;
      cursor_y = ((top_y - bottom_y) / 2) + bottom_y;
      break;
    case ST_DIR_RIGHT:
      cursor_x = self->start_x < self->last_x ? right_x : left_x;
      cursor_y = ((top_y - bottom_y) / 2) + bottom_y;
      break;
    case ST_DIR_UP:
      cursor_x = ((right_x - left_x) / 2) + left_x;
      cursor_y = self->start_y < self->last_y ? top_y : bottom_y;
      break;
    case ST_DIR_DOWN:
      cursor_x = ((right_x - left_x) / 2) + left_x;
      cursor_y = self->start_y < self->last_y ? bottom_y : top_y;
      break;
    default:
      break;
    }

  area_selector_warp_pointer (self, cursor_x, cursor_y);
  area_selector_update_cursor (self, cursor_x, cursor_y);
}

static void
area_selector_move_in_direction (ShellScreenshotUIAreaSelector *self,
                                 StDirectionType                direction,
                                 int                            increment)
{
  double x, y, selection_width, selection_height;
  double new_start_x = self->start_x;
  double new_start_y = self->start_y;
  double new_last_x = self->last_x;
  double new_last_y = self->last_y;
  float width = clutter_actor_get_width (CLUTTER_ACTOR (self));
  float height = clutter_actor_get_height (CLUTTER_ACTOR (self));

  area_selector_get_geometry (self, &x, &y, &selection_width, &selection_height);

  switch (direction)
    {
    case ST_DIR_LEFT:
      new_start_x -= increment;
      new_last_x -= increment;
      break;
    case ST_DIR_RIGHT:
      new_start_x += increment;
      new_last_x += increment;
      break;
    case ST_DIR_UP:
      new_start_y -= increment;
      new_last_y -= increment;
      break;
    case ST_DIR_DOWN:
      new_start_y += increment;
      new_last_y += increment;
      break;
    default:
      break;
    }

  /* Ensure area does not move off the stage edge. */
  if (new_start_x < 0 || new_last_x < 0)
    {
      new_start_x = 0;
      new_last_x = new_start_x + (selection_width - 1);
    }
  else if (new_last_x > width - 1)
    {
      new_last_x = width - 1;
      new_start_x = new_last_x - (selection_width - 1);
    }

  if (new_start_y < 0 || new_last_y < 0)
    {
      new_start_y = 0;
      new_last_y = new_start_y + (selection_height - 1);
    }
  else if (new_last_y > height - 1)
    {
      new_last_y = height - 1;
      new_start_y = new_last_y - (selection_height - 1);
    }

  /* Update selection rectangle props. */
  self->start_x = new_start_x;
  self->start_y = new_start_y;
  self->last_x = new_last_x;
  self->last_y = new_last_y;

  area_selector_update_selection_rect (self);

  /* Update cursor to center of the selection rectangle */
  area_selector_center_cursor (self);
}

static void
area_selector_resize_in_direction (ShellScreenshotUIAreaSelector *self,
                                   StDirectionType                direction,
                                   int                            increment)
{
  double new_start_x, new_start_y, new_last_x, new_last_y;
  float width = clutter_actor_get_width (CLUTTER_ACTOR (self));
  float height = clutter_actor_get_height (CLUTTER_ACTOR (self));

  /* Only move the current side of the area selected in the corresponding
   * direction to the key just pressed if the pressed key is on the same axis,
   * otherwise change cursor direction and exit early. */
  if (area_selector_maybe_change_resize_direction (self, direction))
    {
      area_selector_move_cursor_to_side (self, direction);
      return;
    }

  new_start_x = self->start_x;
  new_start_y = self->start_y;
  new_last_x = self->last_x;
  new_last_y = self->last_y;

  switch (direction)
    {
    case ST_DIR_LEFT:
      if (self->current_side == ST_DIR_LEFT)
        new_start_x -= increment;
      else if (self->current_side == ST_DIR_RIGHT)
        new_last_x -= increment;
      break;

    case ST_DIR_RIGHT:
      if (self->current_side == ST_DIR_LEFT)
        new_start_x += increment;
      else if (self->current_side == ST_DIR_RIGHT)
        new_last_x += increment;
      break;

    case ST_DIR_UP:
      if (self->current_side == ST_DIR_UP)
        new_start_y -= increment;
      else if (self->current_side == ST_DIR_DOWN)
        new_last_y -= increment;
      break;

    case ST_DIR_DOWN:
      if (self->current_side == ST_DIR_UP)
        new_start_y += increment;
      else if (self->current_side == ST_DIR_DOWN)
        new_last_y += increment;
      break;

    default:
      break;
    }

  /* Ensure new resized area does not go off the stage edge. */
  new_start_x = CLAMP (new_start_x, 0, width - 1);
  new_last_x = CLAMP (new_last_x, 0, width - 1);

  new_start_y = CLAMP (new_start_y, 0, height - 1);
  new_last_y = CLAMP (new_last_y, 0, height - 1);

  /* Update selection rectangle props. */
  self->start_x = new_start_x;
  self->start_y = new_start_y;
  self->last_x = new_last_x;
  self->last_y = new_last_y;
  self->last_resize_direction = direction;

  area_selector_update_selection_rect (self);

  area_selector_move_cursor_to_side (self, self->current_side);
}

static void
area_selector_reset_area (ShellScreenshotUIAreaSelector *self)
{
  ShellLayoutManager *layout_manager = get_layout_manager ();

  /* This can called when running headless without any monitors. */
  if (layout_manager != NULL &&
      shell_layout_manager_get_primary_index (layout_manager) != -1)
    {
      ShellMonitor *monitor =
        shell_layout_manager_get_monitor (layout_manager,
                                          shell_layout_manager_get_primary_index (layout_manager));

      if (monitor != NULL)
        {
          self->start_x = monitor->x + floor (monitor->width * 3 / 8);
          self->start_y = monitor->y + floor (monitor->height * 3 / 8);
          self->last_x = monitor->x + floor (monitor->width * 5 / 8) - 1;
          self->last_y = monitor->y + floor (monitor->height * 5 / 8) - 1;

          shell_monitor_free (monitor);
        }
    }

  area_selector_update_selection_rect (self);
}

static void
area_selector_reset (ShellScreenshotUIAreaSelector *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  double x, y, w, h;

  area_selector_stop_drag (self);
  clutter_actor_set_cursor_type (actor, CLUTTER_CURSOR_INHERIT);

  self->current_side = ST_DIR_LEFT;
  self->last_resize_direction = ST_DIR_LEFT;

  /* Preserve area selection if possible. If the area goes out of bounds,
   * the monitors might have changed, so reset the area. */
  area_selector_get_geometry (self, &x, &y, &w, &h);
  if (x < 0 || y < 0 ||
      x + w > clutter_actor_get_width (actor) ||
      y + h > clutter_actor_get_height (actor))
    {
      /* Initialize area to out of bounds so if there's no monitor,
       * the area will be reset once a monitor does appear. */
      self->start_x = -1;
      self->start_y = 0;
      self->last_x = 0;
      self->last_y = 0;

      area_selector_reset_area (self);
    }
}

static void
area_selector_stop_drag (ShellScreenshotUIAreaSelector *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  float width = clutter_actor_get_width (actor);
  float height = clutter_actor_get_height (actor);

  if (self->drag_grab != NULL)
    {
      clutter_grab_dismiss (self->drag_grab);
      g_clear_object (&self->drag_grab);
    }

  if (self->drag_cursor == CLUTTER_CURSOR_CROSSHAIR &&
      self->last_x == self->start_x && self->last_y == self->start_y)
    {
      /* The user clicked without dragging. Make up a larger selection
       * to reduce confusion. */
      double offset = 20 * get_scale_factor ();

      self->start_x -= offset;
      self->start_y -= offset;
      self->last_x += offset;
      self->last_y += offset;

      /* Keep the coordinates inside the stage. */
      if (self->start_x < 0)
        {
          self->last_x -= self->start_x;
          self->start_x = 0;
        }
      else if (self->last_x >= width)
        {
          self->start_x -= self->last_x - width + 1;
          self->last_x = width - 1;
        }

      if (self->start_y < 0)
        {
          self->last_y -= self->start_y;
          self->start_y = 0;
        }
      else if (self->last_y >= height)
        {
          self->start_y -= self->last_y - height + 1;
          self->last_y = height - 1;
        }

      area_selector_update_selection_rect (self);
    }

  g_signal_emit (self, selector_signals[SELECTOR_DRAG_ENDED], 0);
}

static void
area_selector_pan_begin_cb (ClutterGesture                 *gesture,
                            ShellScreenshotUIAreaSelector  *self)
{
  ClutterPanGesture *pan_gesture = CLUTTER_PAN_GESTURE (gesture);
  graphene_point_t centroid;
  ClutterCursorType cursor;

  clutter_pan_gesture_get_centroid_abs (pan_gesture, &centroid);
  cursor = area_selector_compute_cursor_type (self, centroid.x, centroid.y);

  /* Clicking outside of the selection, or using the right mouse button,
   * or with Ctrl results in dragging a new selection from scratch. */
  if (cursor == CLUTTER_CURSOR_CROSSHAIR ||
      clutter_pan_gesture_get_button (pan_gesture) == CLUTTER_BUTTON_SECONDARY ||
      (clutter_pan_gesture_get_state (pan_gesture) & CLUTTER_CONTROL_MASK) != 0)
    {
      self->drag_cursor = CLUTTER_CURSOR_CROSSHAIR;
      clutter_actor_set_cursor_type (CLUTTER_ACTOR (self), self->drag_cursor);

      self->start_x = floor (centroid.x);
      self->start_y = floor (centroid.y);
      self->last_x = self->start_x;
      self->last_y = self->start_y;

      area_selector_update_selection_rect (self);
    }
  else
    {
      /* This is a move or resize operation. */
      double left_x, top_y, width, height, right_x, bottom_y;
      double snap_x, snap_y;

      self->drag_cursor = cursor;
      self->drag_start_x = centroid.x;
      self->drag_start_y = centroid.y;

      area_selector_get_geometry (self, &left_x, &top_y, &width, &height);
      right_x = left_x + width - 1;
      bottom_y = top_y + height - 1;

      snap_x = self->drag_start_x;
      snap_y = self->drag_start_y;

      switch (cursor)
        {
        case CLUTTER_CURSOR_NW_RESIZE:
          snap_x = left_x;
          snap_y = top_y;
          break;
        case CLUTTER_CURSOR_NE_RESIZE:
          snap_x = right_x;
          snap_y = top_y;
          break;
        case CLUTTER_CURSOR_SW_RESIZE:
          snap_x = left_x;
          snap_y = bottom_y;
          break;
        case CLUTTER_CURSOR_SE_RESIZE:
          snap_x = right_x;
          snap_y = bottom_y;
          break;
        case CLUTTER_CURSOR_N_RESIZE:
          snap_y = top_y;
          break;
        case CLUTTER_CURSOR_S_RESIZE:
          snap_y = bottom_y;
          break;
        case CLUTTER_CURSOR_W_RESIZE:
          snap_x = left_x;
          break;
        case CLUTTER_CURSOR_E_RESIZE:
          snap_x = right_x;
          break;
        default:
          break;
        }

      area_selector_warp_pointer (self, snap_x, snap_y);
      self->drag_start_x = snap_x;
      self->drag_start_y = snap_y;

      /* For moving, start X and Y are the top left corner, while
       * last X and Y are the bottom right corner. */
      if (cursor == CLUTTER_CURSOR_MOVE)
        {
          self->start_x = left_x;
          self->start_y = top_y;
          self->last_x = right_x;
          self->last_y = bottom_y;
        }

      /* Start X and Y are set to the stationary sides, while last X
       * and Y are set to the moving sides. */
      if (cursor == CLUTTER_CURSOR_NW_RESIZE ||
          cursor == CLUTTER_CURSOR_W_RESIZE ||
          cursor == CLUTTER_CURSOR_SW_RESIZE)
        {
          self->start_x = right_x;
          self->last_x = left_x;
        }
      if (cursor == CLUTTER_CURSOR_NE_RESIZE ||
          cursor == CLUTTER_CURSOR_E_RESIZE ||
          cursor == CLUTTER_CURSOR_SE_RESIZE)
        {
          self->start_x = left_x;
          self->last_x = right_x;
        }
      if (cursor == CLUTTER_CURSOR_NW_RESIZE ||
          cursor == CLUTTER_CURSOR_N_RESIZE ||
          cursor == CLUTTER_CURSOR_NE_RESIZE)
        {
          self->start_y = bottom_y;
          self->last_y = top_y;
        }
      if (cursor == CLUTTER_CURSOR_SW_RESIZE ||
          cursor == CLUTTER_CURSOR_S_RESIZE ||
          cursor == CLUTTER_CURSOR_SE_RESIZE)
        {
          self->start_y = top_y;
          self->last_y = bottom_y;
        }
    }

  if (self->drag_grab != NULL)
    {
      clutter_grab_dismiss (self->drag_grab);
      g_clear_object (&self->drag_grab);
    }
  self->drag_grab = clutter_stage_grab (shell_global_get_stage (shell_global_get ()),
                                        CLUTTER_ACTOR (self));
  g_signal_emit (self, selector_signals[SELECTOR_DRAG_STARTED], 0);
}

static void
area_selector_pan_end_cb (ClutterGesture                *gesture,
                          ShellScreenshotUIAreaSelector *self)
{
  graphene_point_t centroid;

  area_selector_stop_drag (self);

  /* We might have finished creating a new selection, so we need to
   * update the cursor. */
  clutter_pan_gesture_get_centroid_abs (CLUTTER_PAN_GESTURE (gesture), &centroid);
  area_selector_update_cursor (self, centroid.x, centroid.y);
}

static void
area_selector_pan_update_cb (ClutterPanGesture              *gesture,
                             ShellScreenshotUIAreaSelector  *self)
{
  graphene_point_t centroid;
  float width = clutter_actor_get_width (CLUTTER_ACTOR (self));
  float height = clutter_actor_get_height (CLUTTER_ACTOR (self));

  clutter_pan_gesture_get_centroid_abs (gesture, &centroid);
  if (self->drag_cursor == CLUTTER_CURSOR_CROSSHAIR)
    {
      self->last_x = floor (centroid.x);
      self->last_y = floor (centroid.y);
    }
  else
    {
      double dx = round (centroid.x - self->drag_start_x);
      double dy = round (centroid.y - self->drag_start_y);

      if (self->drag_cursor == CLUTTER_CURSOR_MOVE)
        {
          double x, y, selection_width, selection_height;
          double new_start_x, new_start_y, new_last_x, new_last_y;
          double overshoot_x = 0;
          double overshoot_y = 0;

          area_selector_get_geometry (self, &x, &y, &selection_width, &selection_height);

          new_start_x = self->start_x + dx;
          new_start_y = self->start_y + dy;
          new_last_x = self->last_x + dx;
          new_last_y = self->last_y + dy;

          /* Keep the size intact if we bumped into the stage edge. */
          if (new_start_x < 0)
            {
              overshoot_x = 0 - new_start_x;
              new_start_x = 0;
              new_last_x = new_start_x + (selection_width - 1);
            }
          else if (new_last_x > width - 1)
            {
              overshoot_x = (width - 1) - new_last_x;
              new_last_x = width - 1;
              new_start_x = new_last_x - (selection_width - 1);
            }

          if (new_start_y < 0)
            {
              overshoot_y = 0 - new_start_y;
              new_start_y = 0;
              new_last_y = new_start_y + (selection_height - 1);
            }
          else if (new_last_y > height - 1)
            {
              overshoot_y = (height - 1) - new_last_y;
              new_last_y = height - 1;
              new_start_y = new_last_y - (selection_height - 1);
            }

          /* Add the overshoot to the delta to create a "rubberbanding"
           * behavior of the pointer when dragging. */
          dx += overshoot_x;
          dy += overshoot_y;

          self->start_x = new_start_x;
          self->start_y = new_start_y;
          self->last_x = new_last_x;
          self->last_y = new_last_y;
        }
      else
        {
          if (self->drag_cursor == CLUTTER_CURSOR_W_RESIZE ||
              self->drag_cursor == CLUTTER_CURSOR_E_RESIZE)
            dy = 0;
          if (self->drag_cursor == CLUTTER_CURSOR_N_RESIZE ||
              self->drag_cursor == CLUTTER_CURSOR_S_RESIZE)
            dx = 0;

          /* Make sure last X and Y are clamped between 0 and size - 1,
           * while always preserving the cursor dragging position relative
           * to the selection rectangle. */
          self->last_x += dx;
          if (self->last_x >= width)
            {
              dx -= self->last_x - width + 1;
              self->last_x = width - 1;
            }
          else if (self->last_x < 0)
            {
              dx -= self->last_x;
              self->last_x = 0;
            }

          self->last_y += dy;
          if (self->last_y >= height)
            {
              dy -= self->last_y - height + 1;
              self->last_y = height - 1;
            }
          else if (self->last_y < 0)
            {
              dy -= self->last_y;
              self->last_y = 0;
            }

          /* If we drag the handle past a selection side, update which
           * handles are which. */
          if (self->last_x > self->start_x)
            {
              if (self->drag_cursor == CLUTTER_CURSOR_NW_RESIZE)
                self->drag_cursor = CLUTTER_CURSOR_NE_RESIZE;
              else if (self->drag_cursor == CLUTTER_CURSOR_SW_RESIZE)
                self->drag_cursor = CLUTTER_CURSOR_SE_RESIZE;
              else if (self->drag_cursor == CLUTTER_CURSOR_W_RESIZE)
                self->drag_cursor = CLUTTER_CURSOR_E_RESIZE;
            }
          else
            {
              if (self->drag_cursor == CLUTTER_CURSOR_NE_RESIZE)
                self->drag_cursor = CLUTTER_CURSOR_NW_RESIZE;
              else if (self->drag_cursor == CLUTTER_CURSOR_SE_RESIZE)
                self->drag_cursor = CLUTTER_CURSOR_SW_RESIZE;
              else if (self->drag_cursor == CLUTTER_CURSOR_E_RESIZE)
                self->drag_cursor = CLUTTER_CURSOR_W_RESIZE;
            }

          if (self->last_y > self->start_y)
            {
              if (self->drag_cursor == CLUTTER_CURSOR_NW_RESIZE)
                self->drag_cursor = CLUTTER_CURSOR_SW_RESIZE;
              else if (self->drag_cursor == CLUTTER_CURSOR_NE_RESIZE)
                self->drag_cursor = CLUTTER_CURSOR_SE_RESIZE;
              else if (self->drag_cursor == CLUTTER_CURSOR_N_RESIZE)
                self->drag_cursor = CLUTTER_CURSOR_S_RESIZE;
            }
          else
            {
              if (self->drag_cursor == CLUTTER_CURSOR_SW_RESIZE)
                self->drag_cursor = CLUTTER_CURSOR_NW_RESIZE;
              else if (self->drag_cursor == CLUTTER_CURSOR_SE_RESIZE)
                self->drag_cursor = CLUTTER_CURSOR_NE_RESIZE;
              else if (self->drag_cursor == CLUTTER_CURSOR_S_RESIZE)
                self->drag_cursor = CLUTTER_CURSOR_N_RESIZE;
            }

          clutter_actor_set_cursor_type (CLUTTER_ACTOR (self), self->drag_cursor);
        }

      self->drag_start_x += dx;
      self->drag_start_y += dy;
    }

  area_selector_update_selection_rect (self);
}

/*
 * The JS compared `this._panGesture.get_state ()` with
 * `Clutter.GestureState.WAITING`. For a Clutter.PanGesture that is
 * ClutterPanGesture.get_state (), the modifier state, not the state of the
 * gesture, so the cursor is updated while no modifier key is held. That is
 * kept.
 */
static void
area_selector_motion_cb (ClutterMotionController        *controller,
                         ClutterSprite                  *sprite,
                         float                           x,
                         float                           y,
                         ShellScreenshotUIAreaSelector  *self)
{
  if (clutter_pan_gesture_get_state (self->pan_gesture) == 0)
    area_selector_update_cursor (self, x, y);
}

static void
area_selector_leave_cb (ClutterMotionController        *controller,
                        ClutterSprite                  *sprite,
                        ShellScreenshotUIAreaSelector  *self)
{
  clutter_actor_set_cursor_type (CLUTTER_ACTOR (self), CLUTTER_CURSOR_INHERIT);
}

static void
area_selector_handle_style_changed_cb (StWidget                      *widget,
                                       ShellScreenshotUIAreaSelector *self)
{
  self->handle_size = st_theme_node_get_width (st_widget_get_theme_node (widget));
  area_selector_update_selection_rect (self);
}

static void
area_selector_mapped_notify_cb (ShellScreenshotUIAreaSelector *self)
{
  int x, y;

  if (clutter_actor_is_mapped (CLUTTER_ACTOR (self)))
    {
      shell_global_get_pointer (shell_global_get (), &x, &y, NULL);
      area_selector_update_cursor (self, x, y);
    }
}

static ClutterActor *
area_selector_add_handle (ShellScreenshotUIAreaSelector *self)
{
  ClutterActor *handle;

  handle = g_object_new (ST_TYPE_WIDGET,
                         "style-class", "screenshot-ui-area-selector-handle",
                         NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), handle);

  return handle;
}

static void
shell_screenshot_ui_area_selector_constructed (GObject *object)
{
  ShellScreenshotUIAreaSelector *self = SHELL_SCREENSHOT_UI_AREA_SELECTOR (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);

  G_OBJECT_CLASS (shell_screenshot_ui_area_selector_parent_class)->constructed (object);

  self->area_indicator = g_object_new (SHELL_TYPE_SCREENSHOT_UI_AREA_INDICATOR, NULL);
  add_bind_constraint (CLUTTER_ACTOR (self->area_indicator), actor, CLUTTER_BIND_ALL);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (self->area_indicator));

  self->top_left_handle = area_selector_add_handle (self);
  self->top_right_handle = area_selector_add_handle (self);
  self->bottom_left_handle = area_selector_add_handle (self);
  self->bottom_right_handle = area_selector_add_handle (self);

  /* This will be updated before the first drawn frame. */
  self->handle_size = 0;
  g_signal_connect_object (self->top_left_handle, "style-changed",
                           G_CALLBACK (area_selector_handle_style_changed_cb), self, 0);

  g_signal_connect_object (self, "notify::mapped",
                           G_CALLBACK (area_selector_mapped_notify_cb), self,
                           G_CONNECT_SWAPPED);

  self->pan_gesture = g_object_ref_sink (CLUTTER_PAN_GESTURE (clutter_pan_gesture_new ()));
  clutter_pan_gesture_set_begin_threshold (self->pan_gesture, 0);
  clutter_pan_gesture_set_required_button (self->pan_gesture, 0);
  g_signal_connect_object (self->pan_gesture, "recognize",
                           G_CALLBACK (area_selector_pan_begin_cb), self, 0);
  g_signal_connect_object (self->pan_gesture, "pan-update",
                           G_CALLBACK (area_selector_pan_update_cb), self, 0);
  g_signal_connect_object (self->pan_gesture, "end",
                           G_CALLBACK (area_selector_pan_end_cb), self, 0);
  clutter_actor_add_action (actor, CLUTTER_ACTION (self->pan_gesture));

  self->motion_controller =
    g_object_ref_sink (CLUTTER_MOTION_CONTROLLER (clutter_motion_controller_new ()));
  g_signal_connect_object (self->motion_controller, "motion",
                           G_CALLBACK (area_selector_motion_cb), self, 0);
  g_signal_connect_object (self->motion_controller, "leave",
                           G_CALLBACK (area_selector_leave_cb), self, 0);
  clutter_actor_add_action (actor, CLUTTER_ACTION (self->motion_controller));

  /* Initialize area to out of bounds so reset() below resets it. */
  self->start_x = -1;
  self->start_y = 0;
  self->last_x = 0;
  self->last_y = 0;

  area_selector_reset (self);
}

static void
shell_screenshot_ui_area_selector_dispose (GObject *object)
{
  ShellScreenshotUIAreaSelector *self = SHELL_SCREENSHOT_UI_AREA_SELECTOR (object);

  if (self->drag_grab != NULL)
    {
      clutter_grab_dismiss (self->drag_grab);
      g_clear_object (&self->drag_grab);
    }

  g_clear_object (&self->pan_gesture);
  g_clear_object (&self->motion_controller);

  G_OBJECT_CLASS (shell_screenshot_ui_area_selector_parent_class)->dispose (object);
}

static void
shell_screenshot_ui_area_selector_class_init (ShellScreenshotUIAreaSelectorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_screenshot_ui_area_selector_constructed;
  object_class->dispose = shell_screenshot_ui_area_selector_dispose;

  selector_signals[SELECTOR_DRAG_STARTED] =
    g_signal_new ("drag-started",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  selector_signals[SELECTOR_DRAG_ENDED] =
    g_signal_new ("drag-ended",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_screenshot_ui_area_selector_init (ShellScreenshotUIAreaSelector *self)
{
  /* `_dragCursor` is undefined until the first drag */
  self->drag_cursor = CLUTTER_CURSOR_INHERIT;
}

/* ------------------------------------------------------------------------ */
/* UIWindowSelectorWindowContent                                             */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREENSHOT_UI_WINDOW_CONTENT (shell_screenshot_ui_window_content_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenshotUIWindowContent, shell_screenshot_ui_window_content,
                      SHELL, SCREENSHOT_UI_WINDOW_CONTENT, ClutterActor)

struct _ShellScreenshotUIWindowContent
{
  ClutterActor parent;

  MtkRectangle bounding_box;
  MtkRectangle buffer_rect;
  float buffer_scale;

  ClutterActor *window_actor; /* `_actor`, shows the contents of the window */
  ClutterActor *border;
  ClutterActor *cursor;

  graphene_point_t cursor_point;
  float cursor_scale;
  gboolean should_show_cursor;
  int border_size;
};

G_DEFINE_FINAL_TYPE (ShellScreenshotUIWindowContent, shell_screenshot_ui_window_content,
                     CLUTTER_TYPE_ACTOR)

static void
window_content_allocate (ClutterActor          *actor,
                         const ClutterActorBox *box)
{
  ShellScreenshotUIWindowContent *self = SHELL_SCREENSHOT_UI_WINDOW_CONTENT (actor);
  ClutterActorBox border_box, actor_box, cursor_box;
  ClutterContent *content;
  float x_scale, y_scale, window_w = 0, window_h = 0, w, h;

  clutter_actor_set_allocation (actor, box);

  if (self->window_actor == NULL || self->border == NULL)
    return;

  /* Border goes around the window. */
  clutter_actor_box_init_rect (&border_box, 0, 0,
                               clutter_actor_box_get_width (box),
                               clutter_actor_box_get_height (box));
  border_box.x1 -= self->border_size;
  border_box.y1 -= self->border_size;
  border_box.x2 += self->border_size;
  border_box.y2 += self->border_size;
  clutter_actor_allocate (self->border, &border_box);

  /* box should contain this._boundingBox worth of window. Compute
   * origin and size for the actor box to satisfy that. */
  x_scale = clutter_actor_box_get_width (box) / self->bounding_box.width;
  y_scale = clutter_actor_box_get_height (box) / self->bounding_box.height;

  content = clutter_actor_get_content (self->window_actor);
  if (content != NULL)
    clutter_content_get_preferred_size (content, &window_w, &window_h);

  clutter_actor_box_init_rect (&actor_box,
                               (self->buffer_rect.x - self->bounding_box.x) * x_scale,
                               (self->buffer_rect.y - self->bounding_box.y) * y_scale,
                               window_w * x_scale / self->buffer_scale,
                               window_h * y_scale / self->buffer_scale);
  clutter_actor_allocate (self->window_actor, &actor_box);

  /* Allocate the cursor if we have one. */
  if (self->cursor == NULL)
    return;

  clutter_actor_get_preferred_size (self->cursor, NULL, NULL, &w, &h);
  w *= self->cursor_scale;
  h *= self->cursor_scale;

  cursor_box.x1 = self->cursor_point.x;
  cursor_box.y1 = self->cursor_point.y;
  cursor_box.x2 = self->cursor_point.x + w;
  cursor_box.y2 = self->cursor_point.y + h;
  cursor_box.x1 *= x_scale;
  cursor_box.x2 *= x_scale;
  cursor_box.y1 *= y_scale;
  cursor_box.y2 *= y_scale;

  clutter_actor_allocate (self->cursor, &cursor_box);
}

static void
window_content_destroy (ClutterActor *actor)
{
  ShellScreenshotUIWindowContent *self = SHELL_SCREENSHOT_UI_WINDOW_CONTENT (actor);

  /* The children are destroyed along with the actor */
  self->window_actor = NULL;
  self->border = NULL;
  self->cursor = NULL;

  CLUTTER_ACTOR_CLASS (shell_screenshot_ui_window_content_parent_class)->destroy (actor);
}

static void
window_content_border_style_changed_cb (StWidget                       *border,
                                        ShellScreenshotUIWindowContent *self)
{
  self->border_size = st_theme_node_get_border_width (st_widget_get_theme_node (border),
                                                      ST_SIDE_TOP);
}

static void
shell_screenshot_ui_window_content_class_init (ShellScreenshotUIWindowContentClass *klass)
{
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  actor_class->allocate = window_content_allocate;
  actor_class->destroy = window_content_destroy;
}

static void
shell_screenshot_ui_window_content_init (ShellScreenshotUIWindowContent *self)
{
  self->buffer_scale = 1;
  self->cursor_scale = 1;
}

/* Returns: (transfer floating) (nullable): the content showing the window of
 *   @actor, %NULL if the window could not be painted */
static ClutterActor *
window_content_new (MetaWindowActor *actor)
{
  g_autoptr (ClutterContent) content = NULL;
  g_autoptr (GError) error = NULL;
  ShellScreenshotUIWindowContent *self;
  MetaWindow *window = meta_window_actor_get_meta_window (actor);
  ClutterActor *icon;

  content = meta_window_actor_paint_to_content (actor, NULL, &error);
  if (content == NULL)
    {
      g_message ("Error capturing window for the screenshot UI: %s",
                 error != NULL ? error->message : "no content");
      return NULL;
    }

  self = g_object_new (SHELL_TYPE_SCREENSHOT_UI_WINDOW_CONTENT,
                       "x-expand", TRUE,
                       "y-expand", TRUE,
                       NULL);

  meta_window_get_frame_rect (window, &self->bounding_box);
  meta_window_get_buffer_rect (window, &self->buffer_rect);
  self->buffer_scale = clutter_actor_get_resource_scale (CLUTTER_ACTOR (actor));

  self->window_actor = g_object_new (CLUTTER_TYPE_ACTOR,
                                     "content", content,
                                     NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->window_actor);

  icon = g_object_new (ST_TYPE_ICON,
                       "icon-name", "object-select-symbolic",
                       "style-class", "screenshot-ui-window-selector-check",
                       "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                       NULL);
  self->border = g_object_new (ST_TYPE_BIN,
                               "style-class", "screenshot-ui-window-selector-window-border",
                               "child", icon,
                               NULL);
  g_signal_connect_object (self->border, "style-changed",
                           G_CALLBACK (window_content_border_style_changed_cb), self, 0);
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->border);

  self->should_show_cursor = meta_window_has_pointer (window);

  return CLUTTER_ACTOR (self);
}

static void
window_content_get_cursor_point (ShellScreenshotUIWindowContent *self,
                                 float                          *x,
                                 float                          *y)
{
  *x = self->cursor_point.x + self->bounding_box.x - self->buffer_rect.x;
  *y = self->cursor_point.y + self->bounding_box.y - self->buffer_rect.y;
}

static void
window_content_add_cursor_texture (ShellScreenshotUIWindowContent *self,
                                   ClutterContent                 *content,
                                   const graphene_point_t         *point,
                                   float                           scale)
{
  if (!self->should_show_cursor || self->border == NULL)
    return;

  /* Add the cursor. */
  self->cursor = g_object_new (ST_TYPE_WIDGET,
                               "content", content,
                               "request-mode", CLUTTER_REQUEST_CONTENT_SIZE,
                               NULL);

  self->cursor_point.x = point->x - self->bounding_box.x;
  self->cursor_point.y = point->y - self->bounding_box.y;
  self->cursor_scale = scale;

  clutter_actor_insert_child_below (CLUTTER_ACTOR (self), self->cursor, self->border);
}

static void
window_content_set_cursor_visible (ShellScreenshotUIWindowContent *self,
                                   gboolean                        visible)
{
  if (self->cursor == NULL)
    return;

  if (visible)
    clutter_actor_show (self->cursor);
  else
    clutter_actor_hide (self->cursor);
}

/* ------------------------------------------------------------------------ */
/* UIWindowSelectorWindow                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREENSHOT_UI_WINDOW (shell_screenshot_ui_window_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenshotUIWindow, shell_screenshot_ui_window,
                      SHELL, SCREENSHOT_UI_WINDOW, StButton)

struct _ShellScreenshotUIWindow
{
  StButton parent;
};

G_DEFINE_FINAL_TYPE (ShellScreenshotUIWindow, shell_screenshot_ui_window,
                     ST_TYPE_BUTTON)

static void
shell_screenshot_ui_window_class_init (ShellScreenshotUIWindowClass *klass)
{
}

static void
shell_screenshot_ui_window_init (ShellScreenshotUIWindow *self)
{
}

static ShellScreenshotUIWindowContent *
window_get_content (ShellScreenshotUIWindow *self)
{
  ClutterActor *child = st_bin_get_child (ST_BIN (self));

  if (child == NULL)
    return NULL;

  return SHELL_SCREENSHOT_UI_WINDOW_CONTENT (child);
}

static ShellScreenshotUIWindow *
window_new (MetaWindowActor *actor)
{
  ClutterActor *content = window_content_new (actor);

  if (content == NULL)
    return NULL;

  return g_object_new (SHELL_TYPE_SCREENSHOT_UI_WINDOW,
                       "child", content,
                       "style-class", "screenshot-ui-window-selector-window",
                       "reactive", TRUE,
                       "can-focus", TRUE,
                       "toggle-mode", TRUE,
                       NULL);
}

static void
window_get_bounding_box (ShellScreenshotUIWindow *self,
                         MtkRectangle            *rect)
{
  ShellScreenshotUIWindowContent *content = window_get_content (self);

  if (content != NULL)
    *rect = content->bounding_box;
  else
    *rect = (MtkRectangle) { 0, };
}

static float
window_get_buffer_scale (ShellScreenshotUIWindow *self)
{
  ShellScreenshotUIWindowContent *content = window_get_content (self);

  return content != NULL ? content->buffer_scale : 1;
}

/* Returns: (transfer none) (nullable): the contents of the window */
static ClutterContent *
window_get_window_content (ShellScreenshotUIWindow *self)
{
  ShellScreenshotUIWindowContent *content = window_get_content (self);

  if (content == NULL || content->window_actor == NULL)
    return NULL;

  return clutter_actor_get_content (content->window_actor);
}

/* Returns: (transfer none) (nullable): the texture of the contents of the
 *   window */
static CoglTexture *
window_get_window_texture (ShellScreenshotUIWindow *self)
{
  ClutterContent *content = window_get_window_content (self);

  if (content == NULL || !CLUTTER_IS_TEXTURE_CONTENT (content))
    return NULL;

  return clutter_texture_content_get_texture (CLUTTER_TEXTURE_CONTENT (content));
}

/* Returns: (transfer none) (nullable): the texture of the cursor of the window */
static CoglTexture *
window_get_cursor_texture (ShellScreenshotUIWindow *self)
{
  ShellScreenshotUIWindowContent *content = window_get_content (self);
  ClutterContent *cursor_content;

  if (content == NULL || content->cursor == NULL)
    return NULL;

  cursor_content = clutter_actor_get_content (content->cursor);
  if (cursor_content == NULL || !CLUTTER_IS_TEXTURE_CONTENT (cursor_content))
    return NULL;

  return clutter_texture_content_get_texture (CLUTTER_TEXTURE_CONTENT (cursor_content));
}

static void
window_get_cursor_point (ShellScreenshotUIWindow *self,
                         float                   *x,
                         float                   *y)
{
  ShellScreenshotUIWindowContent *content = window_get_content (self);

  *x = 0;
  *y = 0;
  if (content != NULL)
    window_content_get_cursor_point (content, x, y);
}

static void
window_add_cursor_texture (ShellScreenshotUIWindow *self,
                           ClutterContent          *content,
                           const graphene_point_t  *point,
                           float                    scale)
{
  ShellScreenshotUIWindowContent *window_content = window_get_content (self);

  if (window_content != NULL)
    window_content_add_cursor_texture (window_content, content, point, scale);
}

static void
window_set_cursor_visible (ShellScreenshotUIWindow *self,
                           gboolean                 visible)
{
  ShellScreenshotUIWindowContent *content = window_get_content (self);

  if (content != NULL)
    window_content_set_cursor_visible (content, visible);
}

/* ------------------------------------------------------------------------ */
/* UIWindowSelector                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREENSHOT_UI_WINDOW_SELECTOR (shell_screenshot_ui_window_selector_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenshotUIWindowSelector, shell_screenshot_ui_window_selector,
                      SHELL, SCREENSHOT_UI_WINDOW_SELECTOR, StWidget)

struct _ShellScreenshotUIWindowSelector
{
  StWidget parent;

  int monitor_index;
  ClutterActor *ui; /* weak, the screenshot UI */
  ShellWindowSlotLayout *layout_manager; /* owned by the container */
};

G_DEFINE_FINAL_TYPE (ShellScreenshotUIWindowSelector, shell_screenshot_ui_window_selector,
                     ST_TYPE_WIDGET)

static void
shell_screenshot_ui_window_selector_dispose (GObject *object)
{
  ShellScreenshotUIWindowSelector *self = SHELL_SCREENSHOT_UI_WINDOW_SELECTOR (object);

  g_clear_weak_pointer (&self->ui);
  self->layout_manager = NULL;

  G_OBJECT_CLASS (shell_screenshot_ui_window_selector_parent_class)->dispose (object);
}

static void
shell_screenshot_ui_window_selector_class_init (ShellScreenshotUIWindowSelectorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_screenshot_ui_window_selector_dispose;
}

static void
shell_screenshot_ui_window_selector_init (ShellScreenshotUIWindowSelector *self)
{
}

/* Returns: (transfer floating): a window selector for a monitor, with @visible
 *   as its visibility */
static ShellScreenshotUIWindowSelector *
window_selector_new (int           monitor_index,
                     ClutterActor *ui,
                     gboolean      visible)
{
  ShellScreenshotUIWindowSelector *self;
  ClutterActor *container;

  self = g_object_new (SHELL_TYPE_SCREENSHOT_UI_WINDOW_SELECTOR,
                       "style-class", "screenshot-ui-window-selector",
                       "x-expand", TRUE,
                       "y-expand", TRUE,
                       "visible", visible,
                       NULL);
  clutter_actor_set_layout_manager (CLUTTER_ACTOR (self), clutter_bin_layout_new ());

  self->monitor_index = monitor_index;
  g_set_weak_pointer (&self->ui, ui);

  /* Window screenshots */
  container = g_object_new (ST_TYPE_WIDGET,
                            "style-class", "screenshot-ui-window-selector-window-container",
                            "x-expand", TRUE,
                            "y-expand", TRUE,
                            NULL);
  self->layout_manager = shell_window_slot_layout_new (monitor_index);
  clutter_actor_set_layout_manager (container, CLUTTER_LAYOUT_MANAGER (self->layout_manager));
  clutter_actor_add_child (CLUTTER_ACTOR (self), container);

  return self;
}

static void
window_key_focus_in_cb (ClutterActor *window,
                        ClutterActor *ui)
{
  clutter_actor_grab_key_focus (ui);
  st_button_set_checked (ST_BUTTON (window), TRUE);
}

static void
window_selector_capture (ShellScreenshotUIWindowSelector *self)
{
  ShellGlobal *global = shell_global_get ();
  MetaWorkspaceManager *workspace_manager =
    meta_display_get_workspace_manager (shell_global_get_display (global));
  MetaWorkspace *active_workspace =
    meta_workspace_manager_get_active_workspace (workspace_manager);
  g_autoptr (GList) actors = shell_global_get_window_actors (global);
  GList *l;

  if (self->layout_manager == NULL)
    return;

  for (l = actors; l != NULL; l = l->next)
    {
      MetaWindowActor *actor = l->data;
      MetaWindow *window = meta_window_actor_get_meta_window (actor);
      ShellScreenshotUIWindow *widget;
      MtkRectangle bounding_box;

      if (meta_window_is_override_redirect (window) ||
          !meta_window_located_on_workspace (window, active_workspace) ||
          meta_window_get_monitor (window) != self->monitor_index)
        continue;

      widget = window_new (actor);
      if (widget == NULL)
        continue;

      if (self->ui != NULL)
        g_signal_connect_object (widget, "key-focus-in",
                                 G_CALLBACK (window_key_focus_in_cb), self->ui, 0);

      if (meta_window_has_focus (window))
        {
          st_button_set_checked (ST_BUTTON (widget), TRUE);
          st_button_set_toggle_mode (ST_BUTTON (widget), FALSE);
        }

      window_get_bounding_box (widget, &bounding_box);
      shell_window_slot_layout_add_window (self->layout_manager,
                                           CLUTTER_ACTOR (widget), &bounding_box);
    }
}

static void
window_selector_reset (ShellScreenshotUIWindowSelector *self)
{
  if (self->layout_manager != NULL)
    shell_window_slot_layout_reset (self->layout_manager);
}

/* Returns: (transfer container) (element-type ShellScreenshotUIWindow): the
 *   windows of the selector */
static GList *
window_selector_get_windows (ShellScreenshotUIWindowSelector *self)
{
  if (self->layout_manager == NULL)
    return NULL;

  return shell_window_slot_layout_get_windows (self->layout_manager);
}

/* ------------------------------------------------------------------------ */
/* ScreenshotUI                                                              */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_WINDOW_MANAGER,
  PROP_SCREENCAST_IN_PROGRESS,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  SCREENSHOT_TAKEN,
  CLOSED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

typedef enum
{
  SCREENCAST_PHASE_STARTUP,
  SCREENCAST_PHASE_RECORDING,
} ScreencastPhase;

/* The errors of the screencast service that have a notification of their own
 * (`ScreencastError` of js/misc/dbusErrors.js) */
typedef enum
{
  SCREENCAST_ERROR_OTHER,
  SCREENCAST_ERROR_OUT_OF_DISK_SPACE,
  SCREENCAST_ERROR_SERVICE_CRASH,
} ScreencastErrorKind;

struct _ShellScreenshotUI
{
  StWidget parent;

  ShellWindowManager *window_manager; /* weak */

  gboolean disposed;

  ShellDBusScreencast *screencast_proxy;
  GCancellable *cancellable;
  gboolean screencast_in_progress;
  gboolean screencast_supported;
  gboolean screencast_starting;
  char *screencast_path;
  ShellScreenshotUIMode current_mode;

  /* The full-screen screenshot has a separate container so that we can show
   * it without the screenshot UI fade-in for a nicer animation. It is moved
   * between the screenshot UI group and this actor. */
  ClutterActor *stage_screenshot_container; /* weak */
  ClutterActor *stage_screenshot;
  ClutterActor *cursor;
  ClutterActor *screencast_area_indicator; /* weak */

  gboolean opening_in_progress;
  ShellGrabHelper *grab_helper;

  ShellScreenshotUIAreaSelector *area_selector;
  ClutterActor *primary_monitor_bin;
  ClutterActor *panel;
  ClutterActor *close_button;
  ClutterAlignConstraint *close_button_x_align_constraint;
  ClutterActor *type_button_container;
  StButton *selection_button;
  StButton *screen_button;
  StButton *window_button;
  ClutterActor *bottom_row_container;
  ClutterActor *shot_cast_container;
  StButton *shot_button;
  StButton *cast_button;
  StButton *capture_button;
  ClutterActor *show_pointer_button_container;
  StButton *show_pointer_button;

  GPtrArray *monitor_bins;      /* of ClutterActor */
  GPtrArray *window_selectors;  /* of ShellScreenshotUIWindowSelector */
  GPtrArray *screen_selectors;  /* of StButton */

  ShellScreenshot *shooter;
  float scale;
  float cursor_scale;
};

G_DEFINE_FINAL_TYPE (ShellScreenshotUI, shell_screenshot_ui, ST_TYPE_WIDGET)

static void start_screencast (ShellScreenshotUI *self,
                              int                n_retries);

/* ------------------------------------------------------------------------ */
/* Ease helpers                                                              */
/* ------------------------------------------------------------------------ */

static void
hide_actor_done_cb (gboolean  finished,
                    gpointer  user_data)
{
  if (finished)
    clutter_actor_hide (CLUTTER_ACTOR (user_data));
}

/* Fades @actor in, and shows it */
static void
fade_in (ClutterActor *actor)
{
  clutter_actor_show (actor);
  clutter_actor_remove_all_transitions (actor);
  ease_opacity (actor, 255, UI_ANIMATION_TIME, NULL, NULL, NULL);
}

/* Fades @actor out, and hides it */
static void
fade_out_and_hide (ClutterActor *actor)
{
  clutter_actor_remove_all_transitions (actor);
  ease_opacity (actor, 0, UI_ANIMATION_TIME,
                hide_actor_done_cb, g_object_ref (actor), g_object_unref);
}

/* ------------------------------------------------------------------------ */
/* Windows, monitors and the selected geometry                               */
/* ------------------------------------------------------------------------ */

/* Returns: (transfer container) (element-type ShellScreenshotUIWindow): the
 *   windows of all the window selectors */
static GList *
screenshot_ui_get_windows (ShellScreenshotUI *self)
{
  GList *windows = NULL;
  guint i;

  for (i = 0; i < self->window_selectors->len; i++)
    {
      GList *selector_windows =
        window_selector_get_windows (g_ptr_array_index (self->window_selectors, i));

      windows = g_list_concat (windows, selector_windows);
    }

  return windows;
}

static ShellScreenshotUIWindow *
screenshot_ui_find_checked_window (ShellScreenshotUI *self)
{
  g_autoptr (GList) windows = screenshot_ui_get_windows (self);
  GList *l;

  for (l = windows; l != NULL; l = l->next)
    {
      if (st_button_get_checked (ST_BUTTON (l->data)))
        return l->data;
    }

  return NULL;
}

/*
 * Gets the selected geometry: the area for the area selection, the monitor for
 * the screen selection. Returns %FALSE if there is none.
 */
static gboolean
screenshot_ui_get_selected_geometry (ShellScreenshotUI *self,
                                     gboolean           rescale,
                                     double            *x,
                                     double            *y,
                                     double            *w,
                                     double            *h)
{
  if (st_button_get_checked (self->selection_button))
    {
      area_selector_get_geometry (self->area_selector, x, y, w, h);
    }
  else if (st_button_get_checked (self->screen_button))
    {
      ShellLayoutManager *layout_manager = get_layout_manager ();
      int index = find_checked_index (self->screen_selectors);
      ShellMonitor *monitor;

      if (layout_manager == NULL || index < 0)
        return FALSE;

      monitor = shell_layout_manager_get_monitor (layout_manager, index);
      if (monitor == NULL)
        return FALSE;

      *x = monitor->x;
      *y = monitor->y;
      *w = monitor->width;
      *h = monitor->height;
      shell_monitor_free (monitor);
    }
  else
    {
      return FALSE;
    }

  if (rescale)
    {
      *x *= self->scale;
      *y *= self->scale;
      *w *= self->scale;
      *h *= self->scale;
    }

  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Button state                                                              */
/* ------------------------------------------------------------------------ */

static void
sync_cast_button (ShellScreenshotUI *self)
{
  gboolean visible = self->screencast_supported;
  gboolean reactive = visible &&
                      self->current_mode != SHELL_SCREENSHOT_UI_MODE_SCREENSHOT_ONLY &&
                      get_session_mode_flag ("allow-screencast");

  set_actor_visible (CLUTTER_ACTOR (self->cast_button), visible);
  clutter_actor_set_reactive (CLUTTER_ACTOR (self->cast_button), reactive);
}

static void
sync_window_button_sensitivity (ShellScreenshotUI *self)
{
  g_autoptr (GList) windows = screenshot_ui_get_windows (self);

  clutter_actor_set_reactive (CLUTTER_ACTOR (self->window_button),
                              get_session_mode_flag ("has-windows") &&
                              windows != NULL &&
                              !st_button_get_checked (self->cast_button));
}

static void
refresh_button_layout (ShellScreenshotUI *self)
{
  MetaButtonLayout button_layout;
  StWidget *close_button = ST_WIDGET (self->close_button);
  gboolean close_on_left = FALSE;
  int i;

  meta_prefs_get_button_layout (&button_layout);

  st_widget_remove_style_class_name (close_button, "left");
  st_widget_remove_style_class_name (close_button, "right");

  for (i = 0; i < MAX_BUTTONS_PER_CORNER; i++)
    {
      if (button_layout.left_buttons[i] == META_BUTTON_FUNCTION_CLOSE)
        {
          close_on_left = TRUE;
          break;
        }
    }

  if (close_on_left)
    {
      st_widget_add_style_class_name (close_button, "left");
      clutter_align_constraint_set_factor (self->close_button_x_align_constraint, 0);
    }
  else
    {
      st_widget_add_style_class_name (close_button, "right");
      clutter_align_constraint_set_factor (self->close_button_x_align_constraint, 1);
    }
}

static void
set_screencast_in_progress (ShellScreenshotUI *self,
                            gboolean           in_progress)
{
  if (self->screencast_in_progress == in_progress)
    return;

  self->screencast_in_progress = in_progress;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SCREENCAST_IN_PROGRESS]);
}

/* ------------------------------------------------------------------------ */
/* Notifications of the screencasts                                          */
/* ------------------------------------------------------------------------ */

static void
screencast_show_in_files_action_cb (ShellNotification *notification,
                                    gpointer           user_data)
{
  show_in_files (G_FILE (user_data));
}

static void
screencast_notification_activated_cb (ShellNotification *notification,
                                      GFile             *file)
{
  open_file_default (file, "screencast");
}

static void
screencast_show_notification (ShellScreenshotUI *self,
                              const char        *title)
{
  ShellSource *source = get_screenshot_notification_source ();
  g_autoptr (ShellNotification) notification = NULL;
  gboolean has_path = self->screencast_path != NULL && self->screencast_path[0] != '\0';

  if (source == NULL)
    return;

  notification = shell_notification_new (source, title,
                                         has_path
                                           /* Translators: notification body when a screencast was recorded. */
                                           ? _("Click here to view the video")
                                           : "");
  shell_notification_set_is_transient (notification, TRUE);

  if (has_path)
    {
      g_autoptr (GFile) file = g_file_new_for_path (self->screencast_path);

      /* Translators: button on the screencast notification. */
      shell_notification_add_action (notification, _("Show in Files"),
                                     screencast_show_in_files_action_cb,
                                     g_object_ref (file), g_object_unref);
      g_signal_connect_data (notification, "activated",
                             G_CALLBACK (screencast_notification_activated_cb),
                             g_object_ref (file), (GClosureNotify) g_object_unref, 0);

      hide_overview_and_close_calendar ();
    }

  shell_source_add_notification (source, notification);
}

static void
screencast_failed (ShellScreenshotUI    *self,
                   ScreencastPhase       phase,
                   ScreencastErrorKind   kind,
                   const char           *description)
{
  g_warning ("Screencast failed during phase %s: %s",
             phase == SCREENCAST_PHASE_STARTUP ? "STARTUP" : "RECORDING",
             description);

  set_screencast_in_progress (self, FALSE);

  switch (phase)
    {
    case SCREENCAST_PHASE_STARTUP:
      g_clear_pointer (&self->screencast_path, g_free);

      /* Translators: notification title. */
      screencast_show_notification (self, _("Screencast Failed to Start"));
      break;

    case SCREENCAST_PHASE_RECORDING:
      if (kind == SCREENCAST_ERROR_OUT_OF_DISK_SPACE)
        {
          /* Translators: notification title. */
          screencast_show_notification (self, _("Screencast Failed: Out of Disk Space"));
        }
      else if (kind == SCREENCAST_ERROR_SERVICE_CRASH)
        {
          /* We can encourage user to try again on service crashes since the
           * recorder will auto-blocklist the pipeline that crashed. */

          /* Translators: notification title. */
          screencast_show_notification (self, _("Screencast Failed, Please Try Again"));
        }
      else
        {
          /* Translators: notification title. */
          screencast_show_notification (self, _("Screencast Failed"));
        }

      break;

    default:
      g_assert_not_reached ();
    }
}

static ScreencastErrorKind
screencast_error_kind_from_name (const char *name)
{
  if (name == NULL)
    return SCREENCAST_ERROR_OTHER;

  if (g_str_equal (name, "org.gnome.Shell.Screencast.Error.OutOfDiskSpace"))
    return SCREENCAST_ERROR_OUT_OF_DISK_SPACE;
  else if (g_str_equal (name, "org.gnome.Shell.Screencast.Error.ServiceCrash"))
    return SCREENCAST_ERROR_SERVICE_CRASH;

  return SCREENCAST_ERROR_OTHER;
}

static void
screencast_error_signal_cb (ShellDBusScreencast *proxy,
                            const char          *name,
                            const char          *message,
                            ShellScreenshotUI   *self)
{
  g_autofree char *description = g_strdup_printf ("%s: %s", name, message);

  if (self->disposed || shutdown_seen)
    return;

  screencast_failed (self, SCREENCAST_PHASE_RECORDING,
                     screencast_error_kind_from_name (name), description);
}

static void
screencast_name_owner_notify_cb (GObject           *proxy,
                                 GParamSpec        *pspec,
                                 ShellScreenshotUI *self)
{
  g_autofree char *name_owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (proxy));

  if (name_owner != NULL || self->disposed || shutdown_seen)
    return;

  if (!self->screencast_in_progress)
    return;

  /* If the recorder crashed while we're starting it in start_screencast (),
   * let the callback there handle the error. */
  if (self->screencast_starting)
    return;

  screencast_failed (self, SCREENCAST_PHASE_RECORDING,
                     SCREENCAST_ERROR_SERVICE_CRASH, "Service crashed");
}

static void
screencast_proxy_ready_cb (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  g_autoptr (ShellScreenshotUI) self = user_data;
  g_autoptr (ShellDBusScreencast) proxy = NULL;
  g_autoptr (GError) error = NULL;

  proxy = shell_dbus_screencast_proxy_new_for_bus_finish (result, &error);
  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Error connecting to the screencast service");
      return;
    }

  if (self->disposed || shutdown_seen)
    return;

  self->screencast_proxy = g_steal_pointer (&proxy);

  g_signal_connect_object (self->screencast_proxy, "error",
                           G_CALLBACK (screencast_error_signal_cb), self, 0);
  g_signal_connect_object (self->screencast_proxy, "notify::g-name-owner",
                           G_CALLBACK (screencast_name_owner_notify_cb), self, 0);

  self->screencast_supported =
    shell_dbus_screencast_get_screencast_supported (self->screencast_proxy);
  sync_cast_button (self);
}

/* ------------------------------------------------------------------------ */
/* Starting and stopping a screencast                                        */
/* ------------------------------------------------------------------------ */

typedef struct
{
  ShellScreenshotUI *self;
  int n_retries;
} StartScreencastData;

static void
start_screencast_data_free (StartScreencastData *data)
{
  g_object_unref (data->self);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (StartScreencastData, start_screencast_data_free)

static void
screencast_started_cb (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr (StartScreencastData) data = user_data;
  ShellScreenshotUI *self = data->self;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = NULL;
  gboolean success;
  gboolean retry = FALSE;

  if (shell_dbus_screencast_call_screencast_area_finish (SHELL_DBUS_SCREENCAST (source),
                                                         &success, &path,
                                                         result, &error))
    {
      if (self->disposed)
        return;

      g_free (self->screencast_path);
      self->screencast_path = g_steal_pointer (&path);
    }
  else
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
          self->disposed || shutdown_seen)
        return;

      /* Recorder service disconnected without reply -> service crash
       * That should have blocklisted the pipeline that caused the crash,
       * so try again. */
      if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY) &&
          data->n_retries < 2)
        retry = TRUE;
      else
        screencast_failed (self, SCREENCAST_PHASE_STARTUP,
                           SCREENCAST_ERROR_OTHER, error->message);
    }

  self->screencast_starting = FALSE;

  if (retry)
    {
      g_message ("Screencast service crashed during startup, trying again");
      set_screencast_in_progress (self, FALSE);
      start_screencast (self, data->n_retries + 1);
    }
}

static void
start_screencast (ShellScreenshotUI *self,
                  int                n_retries)
{
  StartScreencastData *data;
  g_autofree char *file_template = NULL;
  GVariantBuilder builder;
  double x, y, w, h;
  gboolean draw_cursor;

  if (st_button_get_checked (self->window_button))
    return; /* TODO */

  if (!screenshot_ui_get_selected_geometry (self, FALSE, &x, &y, &w, &h))
    return;

  draw_cursor = clutter_actor_is_visible (self->cursor);

  /* Set up the screencast indicator rect. */
  if (self->screencast_area_indicator != NULL)
    area_indicator_set_selection_rect (SHELL_SCREENSHOT_UI_AREA_INDICATOR (self->screencast_area_indicator),
                                       x, y, w, h);

  /* Close instantly so the fade-out doesn't get recorded. */
  shell_screenshot_ui_close (self, TRUE);

  /* Set this before calling the method as the screen recording indicator
   * will check it before the success callback fires. */
  set_screencast_in_progress (self, TRUE);

  if (self->screencast_proxy == NULL)
    {
      screencast_failed (self, SCREENCAST_PHASE_STARTUP, SCREENCAST_ERROR_OTHER,
                         "The screencast service is not available");
      return;
    }

  file_template = g_build_filename (
    /* Translators: this is the folder where recorded
       screencasts are stored. */
    _("Screencasts"),
    /* Translators: this is a filename used for screencast
     * recording, where "%d" and "%t" date and time, e.g.
     * "Screencast From 07-17-2013 10:00:46 PM" */
    /* xgettext:no-c-format */
    _("Screencast From %d %t"),
    NULL);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "draw-cursor",
                         g_variant_new_boolean (draw_cursor));

  data = g_new0 (StartScreencastData, 1);
  data->self = g_object_ref (self);
  data->n_retries = n_retries;

  self->screencast_starting = TRUE;
  shell_dbus_screencast_call_screencast_area (self->screencast_proxy,
                                              (int) x, (int) y, (int) w, (int) h,
                                              file_template,
                                              g_variant_builder_end (&builder),
                                              self->cancellable,
                                              screencast_started_cb, data);
}

static void
screencast_stopped_cb (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr (ShellScreenshotUI) self = user_data;
  g_autoptr (GError) error = NULL;
  gboolean success = FALSE;

  if (!shell_dbus_screencast_call_stop_screencast_finish (SHELL_DBUS_SCREENCAST (source),
                                                          &success, result, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      if (error->message != NULL && error->message[0] != '\0')
        g_message ("Error stopping screencast: %s", error->message);
      else
        g_message ("Error stopping screencast");
      return;
    }

  if (!success)
    {
      g_message ("Error stopping screencast");
      return;
    }

  if (self->disposed || shutdown_seen)
    return;

  /* Translators: notification title. */
  screencast_show_notification (self, _("Screencast Recorded"));
}

/**
 * shell_screenshot_ui_stop_screencast:
 * @self: a #ShellScreenshotUI
 *
 * Stops the screencast that is being recorded, if any, and shows a
 * notification when the recording was stopped (`stopScreencast ()`).
 */
void
shell_screenshot_ui_stop_screencast (ShellScreenshotUI *self)
{
  g_return_if_fail (SHELL_IS_SCREENSHOT_UI (self));

  if (self->disposed || !self->screencast_in_progress)
    return;

  /* Set this before calling the method as the screen recording indicator
   * will check it before the success callback fires. */
  set_screencast_in_progress (self, FALSE);

  if (self->screencast_proxy == NULL)
    {
      g_message ("Error stopping screencast");
      return;
    }

  shell_dbus_screencast_call_stop_screencast (self->screencast_proxy,
                                              self->cancellable,
                                              screencast_stopped_cb,
                                              g_object_ref (self));
}

/* ------------------------------------------------------------------------ */
/* Taking the screenshot                                                     */
/* ------------------------------------------------------------------------ */

static void
screenshot_saved_cb (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  g_autoptr (ShellScreenshotUI) self = user_data;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GError) error = NULL;

  file = shell_screenshot_capture_screenshot_finish (result, &error);
  if (error != NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Error capturing screenshot: %s", error->message);
    }
  else if (file != NULL && !self->disposed)
    {
      g_signal_emit (self, signals[SCREENSHOT_TAKEN], 0, file);
    }

  if (!self->disposed && !shutdown_seen)
    shell_screenshot_ui_close (self, FALSE);
}

/*
 * Captures the screenshot the buttons ask for and closes the UI when that
 * is done (`_onCaptureButtonClicked ()` of a screenshot, with `_saveScreenshot ()`).
 */
static void
save_screenshot_and_close (ShellScreenshotUI *self)
{
  if (st_button_get_checked (self->selection_button) ||
      st_button_get_checked (self->screen_button))
    {
      ClutterContent *content = clutter_actor_get_content (self->stage_screenshot);
      ClutterContent *cursor_content;
      CoglTexture *cursor_texture = NULL;
      MtkRectangle geometry;
      double x, y, w, h;
      float cursor_x, cursor_y;

      if (content == NULL || !CLUTTER_IS_TEXTURE_CONTENT (content) ||
          !screenshot_ui_get_selected_geometry (self, TRUE, &x, &y, &w, &h))
        {
          /* Failed to capture the screenshot for some reason. */
          shell_screenshot_ui_close (self, FALSE);
          return;
        }

      geometry = (MtkRectangle) { (int) x, (int) y, (int) w, (int) h };

      cursor_content = clutter_actor_get_content (self->cursor);
      if (cursor_content != NULL && CLUTTER_IS_TEXTURE_CONTENT (cursor_content) &&
          clutter_actor_is_visible (self->cursor))
        cursor_texture = clutter_texture_content_get_texture (CLUTTER_TEXTURE_CONTENT (cursor_content));

      clutter_actor_get_position (self->cursor, &cursor_x, &cursor_y);

      shell_screenshot_capture_screenshot_async (clutter_texture_content_get_texture (CLUTTER_TEXTURE_CONTENT (content)),
                                                 &geometry, self->scale,
                                                 cursor_texture,
                                                 (int) (cursor_x * self->scale),
                                                 (int) (cursor_y * self->scale),
                                                 self->cursor_scale,
                                                 st_button_get_checked (self->selection_button)
                                                   ? SHELL_SCREENSHOT_KIND_AREA
                                                   : SHELL_SCREENSHOT_KIND_SCREEN,
                                                 self->cancellable,
                                                 screenshot_saved_cb,
                                                 g_object_ref (self));
    }
  else if (st_button_get_checked (self->window_button))
    {
      ShellScreenshotUIWindow *window = screenshot_ui_find_checked_window (self);
      CoglTexture *texture, *cursor_texture;
      float scale, cursor_x, cursor_y;

      if (window == NULL)
        {
          shell_screenshot_ui_close (self, FALSE);
          return;
        }

      texture = window_get_window_texture (window);
      if (texture == NULL)
        {
          shell_screenshot_ui_close (self, FALSE);
          return;
        }

      cursor_texture = window_get_cursor_texture (window);
      if (!clutter_actor_is_visible (self->cursor))
        cursor_texture = NULL;

      scale = window_get_buffer_scale (window);
      window_get_cursor_point (window, &cursor_x, &cursor_y);

      shell_screenshot_capture_screenshot_async (texture, NULL, scale,
                                                 cursor_texture,
                                                 (int) (cursor_x * scale),
                                                 (int) (cursor_y * scale),
                                                 self->cursor_scale,
                                                 SHELL_SCREENSHOT_KIND_WINDOW,
                                                 self->cancellable,
                                                 screenshot_saved_cb,
                                                 g_object_ref (self));
    }
  else
    {
      shell_screenshot_ui_close (self, FALSE);
    }
}

static void
on_capture_button_clicked (ShellScreenshotUI *self)
{
  if (st_button_get_checked (self->shot_button))
    {
      save_screenshot_and_close (self);
    }
  else
    {
      /* Screencast closes the UI on its own. */
      start_screencast (self, 0);
    }
}

/* ------------------------------------------------------------------------ */
/* Opening and closing                                                       */
/* ------------------------------------------------------------------------ */

static void
rebuild_monitor_bins (ShellScreenshotUI *self);

static void
screen_selector_notify_checked_cb (StButton          *screen_selector,
                                   GParamSpec        *pspec,
                                   ShellScreenshotUI *self)
{
  guint i;

  if (!st_button_get_checked (screen_selector))
    return;

  st_button_set_toggle_mode (screen_selector, FALSE);

  for (i = 0; i < self->screen_selectors->len; i++)
    {
      StButton *other_selector = g_ptr_array_index (self->screen_selectors, i);

      if (screen_selector == other_selector)
        continue;

      st_button_set_toggle_mode (other_selector, TRUE);
      st_button_set_checked (other_selector, FALSE);
    }
}

static void
screen_selector_key_focus_in_cb (ClutterActor      *screen_selector,
                                 ShellScreenshotUI *self)
{
  clutter_actor_grab_key_focus (CLUTTER_ACTOR (self));
  st_button_set_checked (ST_BUTTON (screen_selector), TRUE);
}

static void
rebuild_monitor_bins (ShellScreenshotUI *self)
{
  ShellLayoutManager *layout_manager = get_layout_manager ();
  int n_monitors, primary_index, i;
  guint j;

  for (j = 0; j < self->monitor_bins->len; j++)
    clutter_actor_destroy (g_ptr_array_index (self->monitor_bins, j));

  g_ptr_array_set_size (self->monitor_bins, 0);
  g_ptr_array_set_size (self->window_selectors, 0);
  g_ptr_array_set_size (self->screen_selectors, 0);

  if (layout_manager == NULL)
    return;

  n_monitors = shell_layout_manager_get_n_monitors (layout_manager);
  primary_index = shell_layout_manager_get_primary_index (layout_manager);

  for (i = 0; i < n_monitors; i++)
    {
      ShellMonitorConstraint *constraint;
      ShellScreenshotUIWindowSelector *window_selector;
      ClutterActor *bin;
      StButton *screen_selector;

      bin = g_object_new (ST_TYPE_WIDGET, NULL);
      clutter_actor_set_layout_manager (bin, clutter_bin_layout_new ());
      constraint = shell_monitor_constraint_new ();
      shell_monitor_constraint_set_index (constraint, i);
      clutter_actor_add_constraint (bin, CLUTTER_CONSTRAINT (constraint));
      clutter_actor_insert_child_below (CLUTTER_ACTOR (self), bin, self->primary_monitor_bin);
      g_ptr_array_add (self->monitor_bins, bin);

      window_selector = window_selector_new (i, CLUTTER_ACTOR (self),
                                             st_button_get_checked (self->window_button));
      if (i == primary_index)
        st_widget_add_style_pseudo_class (ST_WIDGET (window_selector), "primary-monitor");

      clutter_actor_add_child (bin, CLUTTER_ACTOR (window_selector));
      g_ptr_array_add (self->window_selectors, window_selector);

      screen_selector = g_object_new (ST_TYPE_BUTTON,
                                      "style-class", "screenshot-ui-screen-selector",
                                      "x-expand", TRUE,
                                      "y-expand", TRUE,
                                      "visible", st_button_get_checked (self->screen_button),
                                      "reactive", TRUE,
                                      "can-focus", TRUE,
                                      "toggle-mode", TRUE,
                                      NULL);
      g_signal_connect_object (screen_selector, "key-focus-in",
                               G_CALLBACK (screen_selector_key_focus_in_cb), self, 0);
      clutter_actor_add_child (bin, CLUTTER_ACTOR (screen_selector));
      g_ptr_array_add (self->screen_selectors, screen_selector);

      g_signal_connect_object (screen_selector, "notify::checked",
                               G_CALLBACK (screen_selector_notify_checked_cb), self, 0);
    }

  if (primary_index != -1 && primary_index < (int) self->screen_selectors->len)
    st_button_set_checked (g_ptr_array_index (self->screen_selectors, primary_index), TRUE);
}

static void
window_notify_checked_cb (StButton          *window,
                          GParamSpec        *pspec,
                          ShellScreenshotUI *self)
{
  g_autoptr (GList) windows = NULL;
  GList *l;

  if (!st_button_get_checked (window))
    return;

  st_button_set_toggle_mode (window, FALSE);

  windows = screenshot_ui_get_windows (self);
  for (l = windows; l != NULL; l = l->next)
    {
      StButton *other_window = l->data;

      if (window == other_window)
        continue;

      st_button_set_toggle_mode (other_window, TRUE);
      st_button_set_checked (other_window, FALSE);
    }
}

static void
open_ungrab_cb (gboolean  is_user,
                gpointer  user_data)
{
  ShellScreenshotUI *self = user_data;

  if (!self->disposed)
    shell_screenshot_ui_close (self, FALSE);
}

static void
open_ease_done_cb (gboolean  finished,
                   gpointer  user_data)
{
  ShellScreenshotUI *self = user_data;

  if (!finished || self->disposed || self->stage_screenshot_container == NULL)
    return;

  reparent_actor_at_index (self->stage_screenshot_container, CLUTTER_ACTOR (self), 0);
}

/* The second half of open (), after the screenshot of the stage was taken */
static void
open_finish (ShellScreenshotUI *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellLayoutManager *layout_manager = get_layout_manager ();
  ClutterActor *screenshot_ui_group;
  gboolean grabbed;

  /* Get rid of any popup menus.
   * We already have them captured on the screenshot anyway.
   *
   * This needs to happen before the grab below as closing menus will
   * pop their grabs. */
  shell_layout_manager_system_modal_opened (layout_manager);

  screenshot_ui_group = CLUTTER_ACTOR (shell_layout_manager_get_screenshot_ui_group (layout_manager));
  clutter_actor_set_child_above_sibling (clutter_actor_get_parent (screenshot_ui_group),
                                         screenshot_ui_group, NULL);

  grabbed = shell_grab_helper_grab (self->grab_helper, actor, NULL,
                                    open_ungrab_cb, self, NULL);
  if (!grabbed)
    {
      shell_screenshot_ui_close (self, TRUE);
      return;
    }

  refresh_button_layout (self);

  clutter_actor_remove_all_transitions (actor);
  clutter_actor_show (actor);
  ease_opacity (actor, 255, UI_ANIMATION_TIME,
                open_ease_done_cb, g_object_ref (self), g_object_unref);
}

static void
stage_content_ready_cb (GObject      *source,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  g_autoptr (ShellScreenshotUI) self = user_data;
  g_autoptr (ClutterContent) content = NULL;
  g_autoptr (ClutterContent) cursor_content = NULL;
  g_autoptr (GError) error = NULL;
  graphene_point_t cursor_point;
  float scale = 1, cursor_scale = 1;

  content = shell_screenshot_screenshot_stage_to_content_finish (SHELL_SCREENSHOT (source),
                                                                 result,
                                                                 &scale,
                                                                 &cursor_content,
                                                                 &cursor_point,
                                                                 &cursor_scale,
                                                                 &error);
  if (self->disposed || shutdown_seen)
    return;

  self->opening_in_progress = FALSE;

  if (content != NULL)
    {
      clutter_actor_set_content (self->stage_screenshot, content);
      self->scale = scale;

      if (cursor_content != NULL)
        {
          g_autoptr (GList) windows = screenshot_ui_get_windows (self);
          float w = 0, h = 0;
          GList *l;

          clutter_actor_set_content (self->cursor, cursor_content);
          clutter_actor_set_position (self->cursor, cursor_point.x, cursor_point.y);

          clutter_content_get_preferred_size (cursor_content, &w, &h);
          w *= cursor_scale;
          h *= cursor_scale;
          clutter_actor_set_size (self->cursor, w, h);

          self->cursor_scale = cursor_scale;

          for (l = windows; l != NULL; l = l->next)
            {
              window_add_cursor_texture (l->data, cursor_content, &cursor_point, cursor_scale);
              window_set_cursor_visible (l->data,
                                         st_button_get_checked (self->show_pointer_button));
            }
        }

      clutter_actor_show (self->stage_screenshot_container);
    }
  else
    {
      g_message ("Error capturing screenshot: %s",
                 error != NULL ? error->message : "no content");
    }

  open_finish (self);
}

/**
 * shell_screenshot_ui_open:
 * @self: a #ShellScreenshotUI
 * @mode: how to open the UI
 *
 * Opens the UI (`open (mode)`). Nothing happens if the UI is being opened or
 * a screencast is being recorded, or when @mode is
 * %SHELL_SCREENSHOT_UI_MODE_SCREENCAST and screen recording is not supported.
 */
void
shell_screenshot_ui_open (ShellScreenshotUI     *self,
                          ShellScreenshotUIMode  mode)
{
  g_return_if_fail (SHELL_IS_SCREENSHOT_UI (self));

  if (self->disposed || shutdown_seen)
    return;

  if (self->opening_in_progress)
    return;

  if (self->screencast_in_progress)
    return;

  if (mode == SHELL_SCREENSHOT_UI_MODE_SCREENCAST && !self->screencast_supported)
    return;

  self->current_mode = mode;
  st_button_set_checked (self->cast_button, mode == SHELL_SCREENSHOT_UI_MODE_SCREENCAST);
  sync_cast_button (self);

  if (!clutter_actor_is_visible (CLUTTER_ACTOR (self)))
    {
      /* Screenshot UI is opening from completely closed state
       * (rather than opening back from in process of closing). */
      g_autoptr (GList) windows = NULL;
      GList *l;
      guint i;

      for (i = 0; i < self->window_selectors->len; i++)
        window_selector_capture (g_ptr_array_index (self->window_selectors, i));

      windows = screenshot_ui_get_windows (self);
      for (l = windows; l != NULL; l = l->next)
        g_signal_connect_object (l->data, "notify::checked",
                                 G_CALLBACK (window_notify_checked_cb), self, 0);

      sync_window_button_sensitivity (self);
      if (!clutter_actor_get_reactive (CLUTTER_ACTOR (self->window_button)))
        st_button_set_checked (self->selection_button, TRUE);

      g_clear_object (&self->shooter);
      self->shooter = shell_screenshot_new ();

      self->opening_in_progress = TRUE;
      shell_screenshot_screenshot_stage_to_content (self->shooter,
                                                    stage_content_ready_cb,
                                                    g_object_ref (self));
      return;
    }

  open_finish (self);
}

static void
finish_closing (ShellScreenshotUI *self)
{
  ShellLayoutManager *layout_manager = get_layout_manager ();
  guint i;

  clutter_actor_hide (CLUTTER_ACTOR (self));

  g_clear_object (&self->shooter);

  /* Switch back to screenshot mode. */
  st_button_set_checked (self->shot_button, TRUE);

  if (self->stage_screenshot_container != NULL && layout_manager != NULL)
    {
      reparent_actor_at_index (self->stage_screenshot_container,
                               CLUTTER_ACTOR (shell_layout_manager_get_screenshot_ui_group (layout_manager)),
                               0);
      clutter_actor_hide (self->stage_screenshot_container);
    }

  clutter_actor_set_content (self->stage_screenshot, NULL);
  clutter_actor_set_content (self->cursor, NULL);

  area_selector_reset (self->area_selector);
  for (i = 0; i < self->window_selectors->len; i++)
    window_selector_reset (g_ptr_array_index (self->window_selectors, i));

  g_signal_emit (self, signals[CLOSED], 0);
}

static void
close_ease_done_cb (gboolean  finished,
                    gpointer  user_data)
{
  ShellScreenshotUI *self = user_data;

  if (finished && !self->disposed)
    finish_closing (self);
}

/**
 * shell_screenshot_ui_close:
 * @self: a #ShellScreenshotUI
 * @instantly: whether to skip the fade-out
 *
 * Closes the UI (`close (instantly)`).
 */
void
shell_screenshot_ui_close (ShellScreenshotUI *self,
                           gboolean           instantly)
{
  ClutterActor *actor;

  g_return_if_fail (SHELL_IS_SCREENSHOT_UI (self));

  if (self->disposed)
    return;

  actor = CLUTTER_ACTOR (self);

  /* This calls the ungrab callback, which closes the UI as well; the second
   * close below then replaces the animation of the first, as in the JS. */
  shell_grab_helper_ungrab (self->grab_helper, FALSE);

  if (instantly)
    {
      finish_closing (self);
      return;
    }

  clutter_actor_remove_all_transitions (actor);
  ease_opacity (actor, 0, UI_ANIMATION_TIME,
                close_ease_done_cb, g_object_ref (self), g_object_unref);
}

/* ------------------------------------------------------------------------ */
/* The buttons                                                               */
/* ------------------------------------------------------------------------ */

static void
on_selection_button_toggled (ShellScreenshotUI *self)
{
  ClutterActor *area_selector = CLUTTER_ACTOR (self->area_selector);

  if (st_button_get_checked (self->selection_button))
    {
      st_button_set_toggle_mode (self->selection_button, FALSE);
      st_button_set_checked (self->window_button, FALSE);
      st_button_set_checked (self->screen_button, FALSE);

      clutter_actor_show (area_selector);
      clutter_actor_remove_all_transitions (area_selector);
      clutter_actor_set_reactive (area_selector, TRUE);
      ease_opacity (area_selector, 255, UI_ANIMATION_TIME, NULL, NULL, NULL);
    }
  else
    {
      st_button_set_toggle_mode (self->selection_button, TRUE);

      area_selector_stop_drag (self->area_selector);
      clutter_actor_set_cursor_type (CLUTTER_ACTOR (self), CLUTTER_CURSOR_INHERIT);
      clutter_actor_remove_all_transitions (area_selector);
      clutter_actor_set_reactive (area_selector, FALSE);
      ease_opacity (area_selector, 0, UI_ANIMATION_TIME,
                    hide_actor_done_cb, g_object_ref (area_selector), g_object_unref);
    }
}

static void
on_screen_button_toggled (ShellScreenshotUI *self)
{
  guint i;

  if (st_button_get_checked (self->screen_button))
    {
      st_button_set_toggle_mode (self->screen_button, FALSE);
      st_button_set_checked (self->selection_button, FALSE);
      st_button_set_checked (self->window_button, FALSE);

      for (i = 0; i < self->screen_selectors->len; i++)
        fade_in (g_ptr_array_index (self->screen_selectors, i));
    }
  else
    {
      st_button_set_toggle_mode (self->screen_button, TRUE);

      for (i = 0; i < self->screen_selectors->len; i++)
        fade_out_and_hide (g_ptr_array_index (self->screen_selectors, i));
    }
}

static void
on_window_button_toggled (ShellScreenshotUI *self)
{
  guint i;

  if (st_button_get_checked (self->window_button))
    {
      st_button_set_toggle_mode (self->window_button, FALSE);
      st_button_set_checked (self->selection_button, FALSE);
      st_button_set_checked (self->screen_button, FALSE);

      for (i = 0; i < self->window_selectors->len; i++)
        fade_in (g_ptr_array_index (self->window_selectors, i));
    }
  else
    {
      st_button_set_toggle_mode (self->window_button, TRUE);

      for (i = 0; i < self->window_selectors->len; i++)
        fade_out_and_hide (g_ptr_array_index (self->window_selectors, i));
    }
}

static void
on_shot_button_toggled (ShellScreenshotUI *self)
{
  if (st_button_get_checked (self->shot_button))
    {
      st_button_set_toggle_mode (self->shot_button, FALSE);

      if (self->stage_screenshot_container != NULL)
        fade_in (self->stage_screenshot_container);
    }
  else
    {
      st_button_set_toggle_mode (self->shot_button, TRUE);
    }
}

static void
on_cast_button_toggled (ShellScreenshotUI *self)
{
  if (st_button_get_checked (self->cast_button))
    {
      st_button_set_toggle_mode (self->cast_button, FALSE);

      st_widget_add_style_pseudo_class (ST_WIDGET (self->capture_button), "cast");

      if (self->stage_screenshot_container != NULL)
        fade_out_and_hide (self->stage_screenshot_container);

      /* Screen recording doesn't support window selection yet. */
      if (st_button_get_checked (self->window_button))
        st_button_set_checked (self->selection_button, TRUE);

      clutter_actor_set_reactive (CLUTTER_ACTOR (self->window_button), FALSE);
    }
  else
    {
      st_button_set_toggle_mode (self->cast_button, TRUE);

      st_widget_remove_style_pseudo_class (ST_WIDGET (self->capture_button), "cast");

      sync_window_button_sensitivity (self);
    }
}

static void
on_show_pointer_button_toggled (ShellScreenshotUI *self)
{
  gboolean state = st_button_get_checked (self->show_pointer_button);
  g_autoptr (GList) windows = screenshot_ui_get_windows (self);
  GList *l;

  set_actor_visible (self->cursor, state);

  for (l = windows; l != NULL; l = l->next)
    window_set_cursor_visible (l->data, state);
}

static void
on_area_selector_drag_started (ShellScreenshotUI *self)
{
  ease_opacity (self->panel, 100, UI_ANIMATION_TIME, NULL, NULL, NULL);
  ease_opacity (self->close_button, 100, UI_ANIMATION_TIME, NULL, NULL, NULL);
}

static void
on_area_selector_drag_ended (ShellScreenshotUI *self)
{
  ease_opacity (self->panel, 255, UI_ANIMATION_TIME, NULL, NULL, NULL);
  ease_opacity (self->close_button, 255, UI_ANIMATION_TIME, NULL, NULL, NULL);
}

static void
on_close_button_clicked (ShellScreenshotUI *self)
{
  shell_screenshot_ui_close (self, FALSE);
}

/* ------------------------------------------------------------------------ */
/* Key bindings of the UI                                                    */
/* ------------------------------------------------------------------------ */

static int
get_increment (StDirectionType      direction,
               ClutterModifierType  modifier)
{
  if (modifier & CLUTTER_CONTROL_MASK)
    {
      return CTRL_SELECTION_KEYBOARD_INCREMENT;
    }
  else if (modifier & CLUTTER_SHIFT_MASK)
    {
      ShellLayoutManager *layout_manager = get_layout_manager ();
      ShellMonitor *monitor = NULL;
      int increment = SELECTION_KEYBOARD_INCREMENT;

      if (layout_manager != NULL)
        monitor = shell_layout_manager_get_primary_monitor (layout_manager);

      if (monitor != NULL)
        {
          if (direction == ST_DIR_LEFT || direction == ST_DIR_RIGHT)
            increment = monitor->width;
          else if (direction == ST_DIR_DOWN || direction == ST_DIR_UP)
            increment = monitor->height;

          shell_monitor_free (monitor);
        }

      return increment;
    }

  return SELECTION_KEYBOARD_INCREMENT;
}

static void
move_selection (ShellScreenshotUI    *self,
                StDirectionType       direction,
                ClutterModifierType   modifier)
{
  if (st_button_get_checked (self->selection_button))
    area_selector_move_in_direction (self->area_selector, direction,
                                     get_increment (direction, modifier));
}

static void
resize_selection (ShellScreenshotUI    *self,
                  StDirectionType       direction,
                  ClutterModifierType   modifier)
{
  if (st_button_get_checked (self->selection_button))
    area_selector_resize_in_direction (self->area_selector, direction,
                                       get_increment (direction, modifier));
}

static void
move_focus (ShellScreenshotUI *self,
            StDirectionType    direction)
{
  if (st_button_get_checked (self->window_button))
    {
      ShellScreenshotUIWindow *window = screenshot_ui_find_checked_window (self);

      st_widget_navigate_focus (ST_WIDGET (self), CLUTTER_ACTOR (window), direction, FALSE);
    }
  else if (st_button_get_checked (self->screen_button))
    {
      int index = find_checked_index (self->screen_selectors);
      ClutterActor *screen = NULL;

      if (index >= 0)
        screen = g_ptr_array_index (self->screen_selectors, index);

      st_widget_navigate_focus (ST_WIDGET (self), screen, direction, FALSE);
    }
  else
    {
      resize_selection (self, direction, 0);
    }
}

typedef enum
{
  BINDING_ACTIVATE,
  BINDING_SELECTION,
  BINDING_SCREEN,
  BINDING_WINDOW,
  BINDING_SCREENCAST,
  BINDING_POINTER,
  BINDING_MOVE_FOCUS,
  BINDING_RESIZE_SELECTION,
  BINDING_MOVE_SELECTION,
  BINDING_MOVE_SELECTION_AREA,
  BINDING_RESET,
} BindingAction;

typedef struct
{
  const char *name;
  guint key_val;
  ClutterModifierType modifiers;
  BindingAction action;
  StDirectionType direction;
} BindingEntry;

#define MOD1_CONTROL (CLUTTER_MOD1_MASK | CLUTTER_CONTROL_MASK)
#define MOD1_SHIFT (CLUTTER_MOD1_MASK | CLUTTER_SHIFT_MASK)

static const BindingEntry binding_entries[] = {
  { "activate", CLUTTER_KEY_Return, 0, BINDING_ACTIVATE, 0 },
  { "activate", CLUTTER_KEY_space, 0, BINDING_ACTIVATE, 0 },
  { "activate", CLUTTER_KEY_c, CLUTTER_CONTROL_MASK, BINDING_ACTIVATE, 0 },

  { "selection", CLUTTER_KEY_s, 0, BINDING_SELECTION, 0 },
  { "screen", CLUTTER_KEY_c, 0, BINDING_SCREEN, 0 },
  { "window", CLUTTER_KEY_w, 0, BINDING_WINDOW, 0 },
  { "screencast", CLUTTER_KEY_v, 0, BINDING_SCREENCAST, 0 },
  { "pointer", CLUTTER_KEY_p, 0, BINDING_POINTER, 0 },

  { "move-left", CLUTTER_KEY_Left, 0, BINDING_MOVE_FOCUS, ST_DIR_LEFT },
  { "move-right", CLUTTER_KEY_Right, 0, BINDING_MOVE_FOCUS, ST_DIR_RIGHT },
  { "move-up", CLUTTER_KEY_Up, 0, BINDING_MOVE_FOCUS, ST_DIR_UP },
  { "move-down", CLUTTER_KEY_Down, 0, BINDING_MOVE_FOCUS, ST_DIR_DOWN },

  { "resize-selection-left-control", CLUTTER_KEY_Left, CLUTTER_CONTROL_MASK, BINDING_RESIZE_SELECTION, ST_DIR_LEFT },
  { "resize-selection-left-shift", CLUTTER_KEY_Left, CLUTTER_SHIFT_MASK, BINDING_RESIZE_SELECTION, ST_DIR_LEFT },
  { "resize-selection-right-control", CLUTTER_KEY_Right, CLUTTER_CONTROL_MASK, BINDING_RESIZE_SELECTION, ST_DIR_RIGHT },
  { "resize-selection-right-shift", CLUTTER_KEY_Right, CLUTTER_SHIFT_MASK, BINDING_RESIZE_SELECTION, ST_DIR_RIGHT },
  { "resize-selection-up-control", CLUTTER_KEY_Up, CLUTTER_CONTROL_MASK, BINDING_RESIZE_SELECTION, ST_DIR_UP },
  { "resize-selection-up-shift", CLUTTER_KEY_Up, CLUTTER_SHIFT_MASK, BINDING_RESIZE_SELECTION, ST_DIR_UP },
  { "resize-selection-down-control", CLUTTER_KEY_Down, CLUTTER_CONTROL_MASK, BINDING_RESIZE_SELECTION, ST_DIR_DOWN },
  { "resize-selection-down-shift", CLUTTER_KEY_Down, CLUTTER_SHIFT_MASK, BINDING_RESIZE_SELECTION, ST_DIR_DOWN },

  { "move-selection-left", CLUTTER_KEY_Left, CLUTTER_MOD1_MASK, BINDING_MOVE_SELECTION, ST_DIR_LEFT },
  { "move-selection-right", CLUTTER_KEY_Right, CLUTTER_MOD1_MASK, BINDING_MOVE_SELECTION, ST_DIR_RIGHT },
  { "move-selection-up", CLUTTER_KEY_Up, CLUTTER_MOD1_MASK, BINDING_MOVE_SELECTION, ST_DIR_UP },
  { "move-selection-down", CLUTTER_KEY_Down, CLUTTER_MOD1_MASK, BINDING_MOVE_SELECTION, ST_DIR_DOWN },

  /* The JS moved the selection of this one even when the selection button
   * is not checked */
  { "move-selection-left-control", CLUTTER_KEY_Left, MOD1_CONTROL, BINDING_MOVE_SELECTION_AREA, ST_DIR_LEFT },
  { "move-selection-left-shift", CLUTTER_KEY_Left, MOD1_SHIFT, BINDING_MOVE_SELECTION, ST_DIR_LEFT },
  { "move-selection-right-control", CLUTTER_KEY_Right, MOD1_CONTROL, BINDING_MOVE_SELECTION, ST_DIR_RIGHT },
  { "move-selection-right-shift", CLUTTER_KEY_Right, MOD1_SHIFT, BINDING_MOVE_SELECTION, ST_DIR_RIGHT },
  { "move-selection-up-control", CLUTTER_KEY_Up, MOD1_CONTROL, BINDING_MOVE_SELECTION, ST_DIR_UP },
  { "move-selection-up-shift", CLUTTER_KEY_Up, MOD1_SHIFT, BINDING_MOVE_SELECTION, ST_DIR_UP },
  { "move-selection-down-control", CLUTTER_KEY_Down, MOD1_CONTROL, BINDING_MOVE_SELECTION, ST_DIR_DOWN },
  { "move-selection-down-shift", CLUTTER_KEY_Down, MOD1_SHIFT, BINDING_MOVE_SELECTION, ST_DIR_DOWN },

  { "reset", CLUTTER_KEY_r, 0, BINDING_RESET, 0 },
};

static gboolean
screenshot_ui_binding_cb (GObject             *object,
                          const char          *action_name,
                          guint                key_val,
                          ClutterModifierType  modifiers,
                          gpointer             user_data)
{
  ShellScreenshotUI *self = SHELL_SCREENSHOT_UI (object);
  const BindingEntry *entry = user_data;

  switch (entry->action)
    {
    case BINDING_ACTIVATE:
      on_capture_button_clicked (self);
      break;

    case BINDING_SELECTION:
      st_button_set_checked (self->selection_button, TRUE);
      break;

    case BINDING_SCREEN:
      st_button_set_checked (self->screen_button, TRUE);
      break;

    case BINDING_WINDOW:
      if (clutter_actor_get_reactive (CLUTTER_ACTOR (self->window_button)))
        st_button_set_checked (self->window_button, TRUE);
      break;

    case BINDING_SCREENCAST:
      if (clutter_actor_get_reactive (CLUTTER_ACTOR (self->cast_button)))
        st_button_set_checked (self->cast_button,
                               !st_button_get_checked (self->cast_button));
      break;

    case BINDING_POINTER:
      st_button_set_checked (self->show_pointer_button,
                             !st_button_get_checked (self->show_pointer_button));
      break;

    case BINDING_MOVE_FOCUS:
      move_focus (self, entry->direction);
      break;

    case BINDING_RESIZE_SELECTION:
      resize_selection (self, entry->direction, modifiers);
      break;

    case BINDING_MOVE_SELECTION:
      move_selection (self, entry->direction, modifiers);
      break;

    case BINDING_MOVE_SELECTION_AREA:
      area_selector_move_in_direction (self->area_selector, entry->direction,
                                       get_increment (entry->direction, modifiers));
      break;

    case BINDING_RESET:
      area_selector_reset_area (self->area_selector);
      break;

    default:
      g_assert_not_reached ();
    }

  return CLUTTER_EVENT_STOP;
}

/* ------------------------------------------------------------------------ */
/* Key bindings of the shell                                                 */
/* ------------------------------------------------------------------------ */

typedef struct
{
  GWeakRef ui;
} KeybindingData;

static KeybindingData *
keybinding_data_new (ShellScreenshotUI *ui)
{
  KeybindingData *data = g_new0 (KeybindingData, 1);

  g_weak_ref_init (&data->ui, ui);

  return data;
}

static void
keybinding_data_free (gpointer user_data)
{
  KeybindingData *data = user_data;

  g_weak_ref_clear (&data->ui);
  g_free (data);
}

static void
show_screenshot_ui_keybinding_cb (MetaDisplay        *display,
                                  MetaWindow         *window,
                                  const ClutterEvent *event,
                                  MetaKeyBinding     *binding,
                                  gpointer            user_data)
{
  KeybindingData *data = user_data;
  g_autoptr (ShellScreenshotUI) self = g_weak_ref_get (&data->ui);

  if (self != NULL)
    shell_screenshot_ui_open (self, SHELL_SCREENSHOT_UI_MODE_SCREENSHOT);
}

static void
show_screen_recording_ui_keybinding_cb (MetaDisplay        *display,
                                        MetaWindow         *window,
                                        const ClutterEvent *event,
                                        MetaKeyBinding     *binding,
                                        gpointer            user_data)
{
  KeybindingData *data = user_data;
  g_autoptr (ShellScreenshotUI) self = g_weak_ref_get (&data->ui);

  if (self == NULL)
    return;

  if (self->screencast_in_progress)
    shell_screenshot_ui_stop_screencast (self);
  else
    shell_screenshot_ui_open (self, SHELL_SCREENSHOT_UI_MODE_SCREENCAST);
}

static void
keybinding_capture_done_cb (GObject      *source,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  g_autoptr (GObject) content = user_data;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GError) error = NULL;

  file = shell_screenshot_capture_screenshot_finish (result, &error);
  if (error != NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("Error capturing screenshot: %s", error->message);
}

static void
screenshot_window_keybinding_cb (MetaDisplay        *display,
                                 MetaWindow         *window,
                                 const ClutterEvent *event,
                                 MetaKeyBinding     *binding,
                                 gpointer            user_data)
{
  g_autoptr (ClutterContent) content = NULL;
  g_autoptr (GError) error = NULL;
  GObject *actor;

  if (shutdown_seen || window == NULL)
    return;

  actor = meta_window_get_compositor_private (window);
  if (!META_IS_WINDOW_ACTOR (actor))
    {
      g_warning ("Error capturing screenshot: the window has no actor");
      return;
    }

  content = meta_window_actor_paint_to_content (META_WINDOW_ACTOR (actor), NULL, &error);
  if (content == NULL || !CLUTTER_IS_TEXTURE_CONTENT (content))
    {
      g_warning ("Error capturing screenshot: %s",
                 error != NULL ? error->message : "no texture");
      return;
    }

  shell_screenshot_capture_screenshot_async (clutter_texture_content_get_texture (CLUTTER_TEXTURE_CONTENT (content)),
                                             NULL, 1, NULL, 0, 0, 1,
                                             SHELL_SCREENSHOT_KIND_WINDOW,
                                             NULL,
                                             keybinding_capture_done_cb,
                                             g_steal_pointer (&content));
}

static void
screenshot_stage_content_cb (GObject      *source,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  g_autoptr (ShellScreenshot) shooter = user_data;
  g_autoptr (ClutterContent) content = NULL;
  g_autoptr (ClutterContent) cursor_content = NULL;
  g_autoptr (GError) error = NULL;

  content = shell_screenshot_screenshot_stage_to_content_finish (shooter, result,
                                                                 NULL, &cursor_content,
                                                                 NULL, NULL, &error);
  if (shutdown_seen)
    return;

  if (content == NULL || !CLUTTER_IS_TEXTURE_CONTENT (content))
    {
      g_warning ("Error capturing screenshot: %s",
                 error != NULL ? error->message : "no texture");
      return;
    }

  shell_screenshot_capture_screenshot_async (clutter_texture_content_get_texture (CLUTTER_TEXTURE_CONTENT (content)),
                                             NULL, 1, NULL, 0, 0, 1,
                                             SHELL_SCREENSHOT_KIND_SCREEN,
                                             NULL,
                                             keybinding_capture_done_cb,
                                             g_object_ref (content));
}

static void
screenshot_keybinding_cb (MetaDisplay        *display,
                          MetaWindow         *window,
                          const ClutterEvent *event,
                          MetaKeyBinding     *binding,
                          gpointer            user_data)
{
  ShellScreenshot *shooter;

  if (shutdown_seen)
    return;

  shooter = shell_screenshot_new ();
  shell_screenshot_screenshot_stage_to_content (shooter, screenshot_stage_content_cb, shooter);
}

static void
add_keybindings (ShellScreenshotUI *self)
{
  ShellActionMode ui_modes = SHELL_ACTION_MODE_ALL & ~SHELL_ACTION_MODE_LOGIN_SCREEN;
  ShellActionMode restricted_modes = ui_modes &
                                     ~(SHELL_ACTION_MODE_LOCK_SCREEN | SHELL_ACTION_MODE_UNLOCK_SCREEN);
  g_autoptr (GSettings) settings = NULL;

  if (self->window_manager == NULL)
    return;

  settings = g_settings_new (KEYBINDINGS_SCHEMA);

  shell_window_manager_add_keybinding (self->window_manager, "show-screenshot-ui",
                                       settings, META_KEY_BINDING_IGNORE_AUTOREPEAT,
                                       ui_modes,
                                       show_screenshot_ui_keybinding_cb,
                                       keybinding_data_new (self), keybinding_data_free);

  shell_window_manager_add_keybinding (self->window_manager, "show-screen-recording-ui",
                                       settings, META_KEY_BINDING_IGNORE_AUTOREPEAT,
                                       restricted_modes,
                                       show_screen_recording_ui_keybinding_cb,
                                       keybinding_data_new (self), keybinding_data_free);

  shell_window_manager_add_keybinding (self->window_manager, "screenshot-window",
                                       settings,
                                       META_KEY_BINDING_IGNORE_AUTOREPEAT |
                                       META_KEY_BINDING_PER_WINDOW,
                                       restricted_modes,
                                       screenshot_window_keybinding_cb,
                                       NULL, NULL);

  shell_window_manager_add_keybinding (self->window_manager, "screenshot",
                                       settings, META_KEY_BINDING_IGNORE_AUTOREPEAT,
                                       ui_modes,
                                       screenshot_keybinding_cb,
                                       NULL, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellScreenshotUI: construction                                           */
/* ------------------------------------------------------------------------ */

static void
on_monitors_changed (ShellScreenshotUI *self)
{
  if (self->disposed)
    return;

  /* Nope, not dealing with monitor changes. */
  shell_screenshot_ui_close (self, TRUE);
  rebuild_monitor_bins (self);
}

static void
on_session_updated (ShellScreenshotUI *self)
{
  shell_screenshot_ui_close (self, TRUE);
}

static void
on_global_shutdown (ShellGlobal       *global,
                    ShellScreenshotUI *self)
{
  g_cancellable_cancel (self->cancellable);
}

static void
add_tooltip (ShellScreenshotUI *self,
             StButton          *button,
             const char        *text)
{
  clutter_actor_add_child (CLUTTER_ACTOR (self),
                           tooltip_new (ST_WIDGET (button), text));
}

static void
shell_screenshot_ui_constructed (GObject *object)
{
  ShellScreenshotUI *self = SHELL_SCREENSHOT_UI (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *stage = CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ()));
  ShellLayoutManager *layout_manager = get_layout_manager ();
  ShellMain *shell_main = shell_main_get_default ();
  ShellSessionMode *session_mode = shell_main_get_session_mode (shell_main);
  ClutterActor *screenshot_ui_group;
  ClutterActor *stage_screenshot_container;
  ClutterActor *screencast_area_indicator;
  ShellMonitorConstraint *primary_constraint;
  graphene_point_t pivot;
  ClutterConstraint *constraint;

  G_OBJECT_CLASS (shell_screenshot_ui_parent_class)->constructed (object);

  g_return_if_fail (layout_manager != NULL);

  screenshot_ui_group = CLUTTER_ACTOR (shell_layout_manager_get_screenshot_ui_group (layout_manager));

  clutter_actor_set_name (actor, "screenshot-ui");
  add_bind_constraint (actor, stage, CLUTTER_BIND_ALL);
  clutter_actor_set_layout_manager (actor, clutter_bin_layout_new ());
  clutter_actor_set_opacity (actor, 0);
  clutter_actor_hide (actor);
  clutter_actor_set_reactive (actor, TRUE);

  if (self->window_manager != NULL)
    g_set_weak_pointer (&overview_window_manager, self->window_manager);

  self->current_mode = SHELL_SCREENSHOT_UI_MODE_SCREENSHOT;
  self->scale = 1;
  self->cursor_scale = 1;

  self->monitor_bins = g_ptr_array_new ();
  self->window_selectors = g_ptr_array_new ();
  self->screen_selectors = g_ptr_array_new ();

  self->cancellable = g_cancellable_new ();
  shell_dbus_screencast_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                           G_DBUS_PROXY_FLAGS_NONE,
                                           SCREENCAST_BUS_NAME,
                                           SCREENCAST_OBJECT_PATH,
                                           self->cancellable,
                                           screencast_proxy_ready_cb,
                                           g_object_ref (self));

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_global_shutdown), self, 0);

  /* The full-screen screenshot has a separate container so that we can
   * show it without the screenshot UI fade-in for a nicer animation. */
  stage_screenshot_container = g_object_new (ST_TYPE_WIDGET, "visible", FALSE, NULL);
  add_bind_constraint (stage_screenshot_container, stage, CLUTTER_BIND_ALL);
  clutter_actor_add_child (screenshot_ui_group, stage_screenshot_container);
  g_set_weak_pointer (&self->stage_screenshot_container, stage_screenshot_container);

  screencast_area_indicator = g_object_new (SHELL_TYPE_SCREENSHOT_UI_AREA_INDICATOR,
                                            "style-class", "screenshot-ui-screencast-area-indicator",
                                            "visible", FALSE,
                                            NULL);
  add_bind_constraint (screencast_area_indicator, stage, CLUTTER_BIND_ALL);
  g_object_bind_property (self, "screencast-in-progress",
                          screencast_area_indicator, "visible",
                          G_BINDING_DEFAULT);
  /* Add it directly to the stage so that it's above popup menus. */
  clutter_actor_add_child (stage, screencast_area_indicator);
  shell_util_set_hidden_from_pick (screencast_area_indicator, TRUE);
  g_set_weak_pointer (&self->screencast_area_indicator, screencast_area_indicator);

  clutter_actor_add_child (screenshot_ui_group, actor);

  self->stage_screenshot = g_object_new (ST_TYPE_WIDGET,
                                         "style-class", "screenshot-ui-screen-screenshot",
                                         NULL);
  add_bind_constraint (self->stage_screenshot, stage, CLUTTER_BIND_ALL);
  clutter_actor_add_child (stage_screenshot_container, self->stage_screenshot);

  self->cursor = g_object_new (ST_TYPE_WIDGET, NULL);
  clutter_actor_add_child (stage_screenshot_container, self->cursor);

  self->opening_in_progress = FALSE;
  self->grab_helper = shell_grab_helper_new (actor, SHELL_ACTION_MODE_POPUP);

  self->area_selector = g_object_new (SHELL_TYPE_SCREENSHOT_UI_AREA_SELECTOR,
                                      "style-class", "screenshot-ui-area-selector",
                                      "x-expand", TRUE,
                                      "y-expand", TRUE,
                                      "reactive", TRUE,
                                      NULL);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (self->area_selector));

  self->primary_monitor_bin = g_object_new (ST_TYPE_WIDGET, NULL);
  clutter_actor_set_layout_manager (self->primary_monitor_bin, clutter_bin_layout_new ());
  primary_constraint = shell_monitor_constraint_new ();
  shell_monitor_constraint_set_primary (primary_constraint, TRUE);
  clutter_actor_add_constraint (self->primary_monitor_bin, CLUTTER_CONSTRAINT (primary_constraint));
  clutter_actor_add_child (actor, self->primary_monitor_bin);

  self->panel = g_object_new (ST_TYPE_BOX_LAYOUT,
                              "style-class", "screenshot-ui-panel",
                              "y-align", CLUTTER_ACTOR_ALIGN_END,
                              "y-expand", TRUE,
                              "orientation", CLUTTER_ORIENTATION_VERTICAL,
                              "offscreen-redirect", CLUTTER_OFFSCREEN_REDIRECT_AUTOMATIC_FOR_OPACITY,
                              NULL);
  clutter_actor_add_child (self->primary_monitor_bin, self->panel);

  self->close_button = g_object_new (ST_TYPE_BUTTON,
                                     "style-class", "screenshot-ui-close-button",
                                     "icon-name", "preview-close-symbolic",
                                     NULL);
  add_bind_constraint (self->close_button, self->panel, CLUTTER_BIND_POSITION);
  constraint = clutter_align_constraint_new (self->panel, CLUTTER_ALIGN_Y_AXIS, 0);
  graphene_point_init (&pivot, -1, 0.5);
  clutter_align_constraint_set_pivot_point (CLUTTER_ALIGN_CONSTRAINT (constraint), &pivot);
  clutter_actor_add_constraint (self->close_button, constraint);
  constraint = clutter_align_constraint_new (self->panel, CLUTTER_ALIGN_X_AXIS, 0);
  graphene_point_init (&pivot, 0.5, -1);
  clutter_align_constraint_set_pivot_point (CLUTTER_ALIGN_CONSTRAINT (constraint), &pivot);
  self->close_button_x_align_constraint =
    CLUTTER_ALIGN_CONSTRAINT (g_object_ref_sink (constraint));
  clutter_actor_add_constraint (self->close_button, constraint);
  g_signal_connect_object (self->close_button, "clicked",
                           G_CALLBACK (on_close_button_clicked), self,
                           G_CONNECT_SWAPPED);
  clutter_actor_add_child (self->primary_monitor_bin, self->close_button);

  g_signal_connect_object (self->area_selector, "drag-started",
                           G_CALLBACK (on_area_selector_drag_started), self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->area_selector, "drag-ended",
                           G_CALLBACK (on_area_selector_drag_ended), self,
                           G_CONNECT_SWAPPED);

  self->type_button_container = g_object_new (ST_TYPE_WIDGET,
                                              "style-class", "screenshot-ui-type-button-container",
                                              NULL);
  {
    ClutterLayoutManager *box_layout = clutter_box_layout_new ();

    clutter_box_layout_set_spacing (CLUTTER_BOX_LAYOUT (box_layout), 12);
    clutter_box_layout_set_homogeneous (CLUTTER_BOX_LAYOUT (box_layout), TRUE);
    clutter_actor_set_layout_manager (self->type_button_container, box_layout);
  }
  clutter_actor_add_child (self->panel, self->type_button_container);

  self->selection_button = icon_label_button_new ("screenshot-ui-area-symbolic", _("Selection"));
  st_button_set_checked (self->selection_button, TRUE);
  g_signal_connect_object (self->selection_button, "notify::checked",
                           G_CALLBACK (on_selection_button_toggled), self,
                           G_CONNECT_SWAPPED);
  clutter_actor_add_child (self->type_button_container, CLUTTER_ACTOR (self->selection_button));

  add_tooltip (self, self->selection_button, _("Area Selection"));

  self->screen_button = icon_label_button_new ("screenshot-ui-display-symbolic", _("Screen"));
  st_button_set_toggle_mode (self->screen_button, TRUE);
  g_signal_connect_object (self->screen_button, "notify::checked",
                           G_CALLBACK (on_screen_button_toggled), self,
                           G_CONNECT_SWAPPED);
  clutter_actor_add_child (self->type_button_container, CLUTTER_ACTOR (self->screen_button));

  add_tooltip (self, self->screen_button, _("Screen Selection"));

  self->window_button = icon_label_button_new ("screenshot-ui-window-symbolic", _("Window"));
  st_button_set_toggle_mode (self->window_button, TRUE);
  g_signal_connect_object (self->window_button, "notify::checked",
                           G_CALLBACK (on_window_button_toggled), self,
                           G_CONNECT_SWAPPED);
  clutter_actor_add_child (self->type_button_container, CLUTTER_ACTOR (self->window_button));

  add_tooltip (self, self->window_button, _("Window Selection"));

  self->bottom_row_container = g_object_new (ST_TYPE_WIDGET, NULL);
  clutter_actor_set_layout_manager (self->bottom_row_container, clutter_bin_layout_new ());
  clutter_actor_add_child (self->panel, self->bottom_row_container);

  self->shot_cast_container = g_object_new (ST_TYPE_BOX_LAYOUT,
                                            "style-class", "screenshot-ui-shot-cast-container",
                                            "x-align", CLUTTER_ACTOR_ALIGN_START,
                                            "x-expand", TRUE,
                                            NULL);
  clutter_actor_add_child (self->bottom_row_container, self->shot_cast_container);

  self->shot_button = g_object_new (ST_TYPE_BUTTON,
                                    "style-class", "screenshot-ui-shot-cast-button",
                                    "icon-name", "camera-photo-symbolic",
                                    "checked", TRUE,
                                    NULL);
  g_signal_connect_object (self->shot_button, "notify::checked",
                           G_CALLBACK (on_shot_button_toggled), self,
                           G_CONNECT_SWAPPED);
  clutter_actor_add_child (self->shot_cast_container, CLUTTER_ACTOR (self->shot_button));

  add_tooltip (self, self->shot_button, _("Take Screenshot"));

  self->cast_button = g_object_new (ST_TYPE_BUTTON,
                                    "style-class", "screenshot-ui-shot-cast-button",
                                    "icon-name", "camera-web-symbolic",
                                    "toggle-mode", TRUE,
                                    "visible", FALSE,
                                    NULL);
  g_signal_connect_object (self->cast_button, "notify::checked",
                           G_CALLBACK (on_cast_button_toggled), self,
                           G_CONNECT_SWAPPED);
  clutter_actor_add_child (self->shot_cast_container, CLUTTER_ACTOR (self->cast_button));

  add_tooltip (self, self->cast_button, _("Record Screen"));

  g_object_bind_property (self->shot_button, "checked",
                          self->cast_button, "checked",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_INVERT_BOOLEAN);

  self->capture_button = g_object_new (ST_TYPE_BUTTON,
                                       "style-class", "screenshot-ui-capture-button",
                                       NULL);
  st_bin_set_child (ST_BIN (self->capture_button),
                    g_object_new (ST_TYPE_WIDGET,
                                  "style-class", "screenshot-ui-capture-button-circle",
                                  "x-expand", TRUE,
                                  "y-expand", TRUE,
                                  NULL));
  add_tooltip (self, self->capture_button,
               /* Translators: since this string refers to an action,
                  it needs to be phrased as a verb. */
               _("Capture"));
  g_signal_connect_object (self->capture_button, "clicked",
                           G_CALLBACK (on_capture_button_clicked), self,
                           G_CONNECT_SWAPPED);
  clutter_actor_add_child (self->bottom_row_container, CLUTTER_ACTOR (self->capture_button));

  self->show_pointer_button_container = g_object_new (ST_TYPE_BOX_LAYOUT,
                                                      "x-align", CLUTTER_ACTOR_ALIGN_END,
                                                      "x-expand", TRUE,
                                                      NULL);
  clutter_actor_add_child (self->bottom_row_container, self->show_pointer_button_container);

  self->show_pointer_button = g_object_new (ST_TYPE_BUTTON,
                                            "style-class", "screenshot-ui-show-pointer-button",
                                            "icon-name", "screenshot-ui-show-pointer-symbolic",
                                            "toggle-mode", TRUE,
                                            NULL);
  clutter_actor_add_child (self->show_pointer_button_container,
                          CLUTTER_ACTOR (self->show_pointer_button));

  add_tooltip (self, self->show_pointer_button, _("Show Pointer"));

  g_signal_connect_object (self->show_pointer_button, "notify::checked",
                           G_CALLBACK (on_show_pointer_button_toggled), self,
                           G_CONNECT_SWAPPED);
  clutter_actor_hide (self->cursor);

  rebuild_monitor_bins (self);

  g_signal_connect_object (layout_manager, "monitors-changed",
                           G_CALLBACK (on_monitors_changed), self,
                           G_CONNECT_SWAPPED);

  add_keybindings (self);

  if (session_mode != NULL)
    g_signal_connect_object (session_mode, "updated",
                             G_CALLBACK (on_session_updated), self,
                             G_CONNECT_SWAPPED);
  on_session_updated (self);
}

static void
shell_screenshot_ui_dispose (GObject *object)
{
  ShellScreenshotUI *self = SHELL_SCREENSHOT_UI (object);

  self->disposed = TRUE;

  if (self->cancellable != NULL)
    {
      g_cancellable_cancel (self->cancellable);
      g_clear_object (&self->cancellable);
    }

  g_clear_object (&self->close_button_x_align_constraint);
  g_clear_object (&self->screencast_proxy);
  g_clear_object (&self->shooter);
  g_clear_object (&self->grab_helper);

  /* The container is moved between the UI and the UI group, and the
   * indicator is a child of the stage */
  if (self->stage_screenshot_container != NULL)
    {
      ClutterActor *container = self->stage_screenshot_container;

      g_clear_weak_pointer (&self->stage_screenshot_container);
      clutter_actor_destroy (container);
    }

  if (self->screencast_area_indicator != NULL)
    {
      ClutterActor *indicator = self->screencast_area_indicator;

      g_clear_weak_pointer (&self->screencast_area_indicator);
      clutter_actor_destroy (indicator);
    }

  if (overview_window_manager == self->window_manager)
    g_clear_weak_pointer (&overview_window_manager);
  g_clear_weak_pointer (&self->window_manager);

  g_clear_pointer (&self->monitor_bins, g_ptr_array_unref);
  g_clear_pointer (&self->window_selectors, g_ptr_array_unref);
  g_clear_pointer (&self->screen_selectors, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_screenshot_ui_parent_class)->dispose (object);
}

static void
shell_screenshot_ui_finalize (GObject *object)
{
  ShellScreenshotUI *self = SHELL_SCREENSHOT_UI (object);

  g_clear_pointer (&self->screencast_path, g_free);

  G_OBJECT_CLASS (shell_screenshot_ui_parent_class)->finalize (object);
}

static void
shell_screenshot_ui_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellScreenshotUI *self = SHELL_SCREENSHOT_UI (object);

  switch (prop_id)
    {
    case PROP_WINDOW_MANAGER:
      g_value_set_object (value, self->window_manager);
      break;

    case PROP_SCREENCAST_IN_PROGRESS:
      g_value_set_boolean (value, self->screencast_in_progress);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_screenshot_ui_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  ShellScreenshotUI *self = SHELL_SCREENSHOT_UI (object);

  switch (prop_id)
    {
    case PROP_WINDOW_MANAGER:
      g_set_weak_pointer (&self->window_manager, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_screenshot_ui_class_init (ShellScreenshotUIClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterBindingPool *binding_pool;
  guint i;

  object_class->constructed = shell_screenshot_ui_constructed;
  object_class->dispose = shell_screenshot_ui_dispose;
  object_class->finalize = shell_screenshot_ui_finalize;
  object_class->get_property = shell_screenshot_ui_get_property;
  object_class->set_property = shell_screenshot_ui_set_property;

  /**
   * ShellScreenshotUI:window-manager:
   *
   * The window manager the key bindings of the UI are added to, and that
   * hides the overview (`Main.wm`).
   */
  props[PROP_WINDOW_MANAGER] =
    g_param_spec_object ("window-manager", NULL, NULL,
                         SHELL_TYPE_WINDOW_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellScreenshotUI:screencast-in-progress:
   *
   * Whether a screencast is being recorded (or starting).
   */
  props[PROP_SCREENCAST_IN_PROGRESS] =
    g_param_spec_boolean ("screencast-in-progress", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellScreenshotUI::screenshot-taken:
   * @self: the #ShellScreenshotUI
   * @file: the file the screenshot was saved to
   *
   * Emitted when a screenshot was taken, before the UI closes.
   */
  signals[SCREENSHOT_TAKEN] =
    g_signal_new ("screenshot-taken",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_FILE);

  /**
   * ShellScreenshotUI::closed:
   * @self: the #ShellScreenshotUI
   *
   * Emitted when the UI closed.
   */
  signals[CLOSED] =
    g_signal_new ("closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  binding_pool = clutter_binding_pool_get_for_class (klass);
  for (i = 0; i < G_N_ELEMENTS (binding_entries); i++)
    {
      const BindingEntry *entry = &binding_entries[i];

      clutter_binding_pool_install_action (binding_pool, entry->name,
                                           entry->key_val, entry->modifiers,
                                           G_CALLBACK (screenshot_ui_binding_cb),
                                           (gpointer) entry, NULL);
    }

  ensure_shutdown_watch ();
  declare_hooks ();
}

static void
shell_screenshot_ui_init (ShellScreenshotUI *self)
{
}

/**
 * shell_screenshot_ui_new:
 * @window_manager: the window manager the key bindings are added to
 *
 * Creates the screenshot UI (`new ScreenshotUI ()`), adds it to the screenshot
 * UI group of the layout manager and adds the key bindings `show-screenshot-ui`,
 * `show-screen-recording-ui`, `screenshot-window` and `screenshot` to
 * @window_manager.
 *
 * Returns: (transfer full): a new #ShellScreenshotUI
 */
ShellScreenshotUI *
shell_screenshot_ui_new (ShellWindowManager *window_manager)
{
  g_return_val_if_fail (window_manager == NULL ||
                        SHELL_IS_WINDOW_MANAGER (window_manager), NULL);

  return g_object_new (SHELL_TYPE_SCREENSHOT_UI,
                       "window-manager", window_manager,
                       NULL);
}

/**
 * shell_screenshot_ui_get_screencast_in_progress:
 * @self: a #ShellScreenshotUI
 *
 * Returns: the value of #ShellScreenshotUI:screencast-in-progress
 */
gboolean
shell_screenshot_ui_get_screencast_in_progress (ShellScreenshotUI *self)
{
  g_return_val_if_fail (SHELL_IS_SCREENSHOT_UI (self), FALSE);

  return self->screencast_in_progress;
}

/* ------------------------------------------------------------------------ */
/* Flashspot                                                                 */
/* ------------------------------------------------------------------------ */

struct _ShellFlashspot
{
  ShellLightbox parent;
};

G_DEFINE_FINAL_TYPE (ShellFlashspot, shell_flashspot, SHELL_TYPE_LIGHTBOX)

static void
shell_flashspot_constructed (GObject *object)
{
  G_OBJECT_CLASS (shell_flashspot_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (object), "flashspot");
}

static void
shell_flashspot_class_init (ShellFlashspotClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_flashspot_constructed;
}

static void
shell_flashspot_init (ShellFlashspot *self)
{
}

/**
 * shell_flashspot_new:
 * @area: the area of the stage to flash
 *
 * Creates a flashspot over @area, in the UI group (`new Flashspot (area)`).
 * It is hidden until shell_flashspot_fire () is called.
 *
 * Returns: (transfer full): a new #ShellFlashspot
 */
ShellFlashspot *
shell_flashspot_new (const MtkRectangle *area)
{
  ShellLayoutManager *layout_manager = get_layout_manager ();
  ShellFlashspot *self;

  g_return_val_if_fail (area != NULL, NULL);
  g_return_val_if_fail (layout_manager != NULL, NULL);

  self = g_object_new (SHELL_TYPE_FLASHSPOT,
                       "container", shell_layout_manager_get_ui_group (layout_manager),
                       "inhibit-events", TRUE,
                       "width", (float) area->width,
                       "height", (float) area->height,
                       NULL);
  clutter_actor_set_position (CLUTTER_ACTOR (self), area->x, area->y);

  /* The UI group owns the flashspot */
  return g_object_ref (self);
}

typedef struct
{
  ShellFlashspot *flashspot;
  ShellFlashspotDoneFunc done;
  gpointer user_data;
  GDestroyNotify destroy;
} FireData;

static void
fire_data_free (gpointer user_data)
{
  FireData *data = user_data;

  if (data->destroy != NULL)
    data->destroy (data->user_data);

  g_object_unref (data->flashspot);
  g_free (data);
}

static void
fire_done_cb (gboolean  finished,
              gpointer  user_data)
{
  FireData *data = user_data;

  if (!finished)
    return;

  if (data->done != NULL)
    data->done (data->user_data);

  clutter_actor_destroy (CLUTTER_ACTOR (data->flashspot));
}

/**
 * shell_flashspot_fire:
 * @self: a #ShellFlashspot
 * @done: (scope async) (nullable) (closure user_data) (destroy destroy):
 *   called when the flash is over, right before @self is destroyed
 * @user_data: the data to pass to @done
 * @destroy: (nullable): called on @user_data after @done, or when @done will
 *   not be called any more
 *
 * Flashes the area and destroys @self (`fire (doneCallback)`).
 */
void
shell_flashspot_fire (ShellFlashspot         *self,
                      ShellFlashspotDoneFunc  done,
                      gpointer                user_data,
                      GDestroyNotify          destroy)
{
  ClutterActor *actor;
  FireData *data;

  g_return_if_fail (SHELL_IS_FLASHSPOT (self));

  actor = CLUTTER_ACTOR (self);

  clutter_actor_show (actor);
  clutter_actor_set_opacity (actor, 255);

  data = g_new0 (FireData, 1);
  data->flashspot = g_object_ref (self);
  data->done = done;
  data->user_data = user_data;
  data->destroy = destroy;

  ease_opacity (actor, 0, FLASHSPOT_ANIMATION_OUT_TIME,
                fire_done_cb, data, fire_data_free);
}

/* ------------------------------------------------------------------------ */
/* Pending select/pick operations                                            */
/* ------------------------------------------------------------------------ */

static void
destroy_actor_idle_cb (gpointer user_data)
{
  ClutterActor *actor = user_data;

  clutter_actor_destroy (actor);
  g_object_unref (actor);
}

/* `GLib.idle_add_once (PRIORITY_DEFAULT, () => this.destroy ())` */
static void
destroy_actor_in_idle (ClutterActor *actor)
{
  g_idle_add_once (destroy_actor_idle_cb, g_object_ref (actor));
}

/* ------------------------------------------------------------------------ */
/* SelectArea                                                                */
/* ------------------------------------------------------------------------ */

struct _ShellSelectArea
{
  StWidget parent;

  ShellGrabHelper *grab_helper;
  ClutterActor *rubberband;

  int start_x;
  int start_y;
  int last_x;
  int last_y;

  MtkRectangle result;
  gboolean has_result;

  GTask *task; /* the pending select_async (), not owned */
};

G_DEFINE_FINAL_TYPE (ShellSelectArea, shell_select_area, ST_TYPE_WIDGET)

static MtkRectangle
select_area_get_geometry (ShellSelectArea *self)
{
  return (MtkRectangle) {
    MIN (self->start_x, self->last_x),
    MIN (self->start_y, self->last_y),
    abs (self->start_x - self->last_x) + 1,
    abs (self->start_y - self->last_y) + 1,
  };
}

static void
select_area_pan_update_cb (ClutterPanGesture *gesture,
                           ShellSelectArea   *self)
{
  graphene_point_t coords;
  MtkRectangle geometry;

  if (self->has_result)
    return;

  clutter_pan_gesture_get_centroid_abs (gesture, &coords);
  self->last_x = floor (coords.x);
  self->last_y = floor (coords.y);
  geometry = select_area_get_geometry (self);

  clutter_actor_set_position (self->rubberband, geometry.x, geometry.y);
  clutter_actor_set_size (self->rubberband, geometry.width, geometry.height);
  clutter_actor_show (self->rubberband);
}

static void
select_area_pan_begin_cb (ClutterPanGesture *gesture,
                          ShellSelectArea   *self)
{
  graphene_point_t coords;

  if (self->has_result)
    return;

  clutter_pan_gesture_get_centroid_abs (gesture, &coords);
  self->start_x = floor (coords.x);
  self->start_y = floor (coords.y);
  clutter_actor_set_position (self->rubberband, self->start_x, self->start_y);
}

static void
select_area_ungrab_cb (gboolean  finished,
                       gpointer  user_data)
{
  ShellSelectArea *self = user_data;

  if (finished && self->grab_helper != NULL)
    shell_grab_helper_ungrab (self->grab_helper, FALSE);
}

static void
select_area_pan_end_cb (ClutterPanGesture *gesture,
                        ShellSelectArea   *self)
{
  if (self->has_result)
    return;

  self->result = select_area_get_geometry (self);
  self->has_result = TRUE;

  ease_opacity (CLUTTER_ACTOR (self), 0, UI_ANIMATION_TIME,
                select_area_ungrab_cb, g_object_ref (self), g_object_unref);
}

static void
select_area_shutdown_cb (ShellGlobal    *global,
                         ShellSelectArea *self)
{
  GTask *task = self->task;

  if (task == NULL)
    return;

  self->task = NULL;
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                           "The shell is shutting down");
}

static void
shell_select_area_constructed (GObject *object)
{
  ShellSelectArea *self = SHELL_SELECT_AREA (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellLayoutManager *layout_manager = get_layout_manager ();
  ClutterPanGesture *pan_gesture;

  G_OBJECT_CLASS (shell_select_area_parent_class)->constructed (object);

  self->start_x = -1;
  self->start_y = -1;
  self->last_x = 0;
  self->last_y = 0;

  clutter_actor_hide (actor);
  clutter_actor_set_reactive (actor, TRUE);
  clutter_actor_set_position (actor, 0, 0);
  if (layout_manager != NULL)
    clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager)),
                             actor);

  self->grab_helper = shell_grab_helper_new (actor, SHELL_ACTION_MODE_NONE);

  add_bind_constraint (actor, CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ())),
                       CLUTTER_BIND_ALL);

  self->rubberband = g_object_new (ST_TYPE_WIDGET,
                                   "style-class", "select-area-rubberband",
                                   "visible", FALSE,
                                   NULL);
  clutter_actor_add_child (actor, self->rubberband);

  pan_gesture = CLUTTER_PAN_GESTURE (clutter_pan_gesture_new ());
  clutter_pan_gesture_set_begin_threshold (pan_gesture, 0);
  g_signal_connect_object (pan_gesture, "recognize",
                           G_CALLBACK (select_area_pan_begin_cb), self, 0);
  g_signal_connect_object (pan_gesture, "pan-update",
                           G_CALLBACK (select_area_pan_update_cb), self, 0);
  g_signal_connect_object (pan_gesture, "end",
                           G_CALLBACK (select_area_pan_end_cb), self, 0);
  clutter_actor_add_action (actor, CLUTTER_ACTION (pan_gesture));

  clutter_actor_set_cursor_type (actor, CLUTTER_CURSOR_CROSSHAIR);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (select_area_shutdown_cb), self, 0);
}

static void
shell_select_area_dispose (GObject *object)
{
  ShellSelectArea *self = SHELL_SELECT_AREA (object);

  g_clear_object (&self->grab_helper);

  G_OBJECT_CLASS (shell_select_area_parent_class)->dispose (object);
}

static void
shell_select_area_class_init (ShellSelectAreaClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_select_area_constructed;
  object_class->dispose = shell_select_area_dispose;

  ensure_shutdown_watch ();
}

static void
shell_select_area_init (ShellSelectArea *self)
{
}

static void
select_area_grab_done_cb (GObject      *source,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  ShellSelectArea *self = g_task_get_source_object (task);
  g_autoptr (GError) error = NULL;

  /* The shutdown handler returned the task already */
  if (self->task != task)
    return;

  self->task = NULL;

  shell_grab_helper_grab_finish (SHELL_GRAB_HELPER (source), result, &error);

  destroy_actor_in_idle (CLUTTER_ACTOR (self));

  if (error != NULL)
    g_task_return_error (task, g_steal_pointer (&error));
  else if (self->has_result)
    g_task_return_pointer (task, mtk_rectangle_copy (&self->result), (GDestroyNotify) mtk_rectangle_free);
  else
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                             "Operation was cancelled");
}

/**
 * shell_select_area_select_async:
 * @self: a #ShellSelectArea
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): function to call when the user selected an area
 *   or dismissed the selection
 * @user_data: the data to pass to @callback
 *
 * Lets the user select an area of the screen (`selectAsync ()`). @self is
 * destroyed when the selection is over, whatever the outcome.
 */
void
shell_select_area_select_async (ShellSelectArea     *self,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;
  ShellLayoutManager *layout_manager = get_layout_manager ();
  ClutterActor *ui_group;

  g_return_if_fail (SHELL_IS_SELECT_AREA (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, shell_select_area_select_async);

  if (self->task != NULL || shutdown_seen || layout_manager == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PENDING,
                               "Unable to select an area");
      return;
    }

  ui_group = CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager));
  clutter_actor_set_child_above_sibling (ui_group, CLUTTER_ACTOR (self), NULL);
  clutter_actor_show (CLUTTER_ACTOR (self));

  self->task = task;
  shell_grab_helper_grab_async (self->grab_helper, CLUTTER_ACTOR (self), NULL,
                                select_area_grab_done_cb, g_object_ref (task));
}

/**
 * shell_select_area_select_finish:
 * @self: a #ShellSelectArea
 * @result: the #GAsyncResult that was provided to the callback
 * @error: #GError for error reporting
 *
 * Finish the asynchronous operation started by shell_select_area_select_async ().
 *
 * Returns: (transfer full): the selected area, in stage coordinates, or %NULL
 *   with @error set (to %G_IO_ERROR_CANCELLED if the user dismissed the
 *   selection)
 */
MtkRectangle *
shell_select_area_select_finish (ShellSelectArea  *self,
                                 GAsyncResult     *result,
                                 GError          **error)
{
  g_return_val_if_fail (SHELL_IS_SELECT_AREA (self), NULL);
  g_return_val_if_fail (g_task_is_valid (result, self), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (result,
                                                  shell_select_area_select_async),
                        NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

/* ------------------------------------------------------------------------ */
/* RecolorEffect                                                             */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SCREENSHOT_UI_RECOLOR_EFFECT (shell_screenshot_ui_recolor_effect_get_type ())
G_DECLARE_FINAL_TYPE (ShellScreenshotUIRecolorEffect, shell_screenshot_ui_recolor_effect,
                      SHELL, SCREENSHOT_UI_RECOLOR_EFFECT, ClutterShaderEffect)

struct _ShellScreenshotUIRecolorEffect
{
  ClutterShaderEffect parent;

  CoglColor color;
  CoglColor chroma;
  float threshold;
  float smoothing;
};

G_DEFINE_FINAL_TYPE (ShellScreenshotUIRecolorEffect, shell_screenshot_ui_recolor_effect,
                     CLUTTER_TYPE_SHADER_EFFECT)

/* Conversion parameters from https://en.wikipedia.org/wiki/YCbCr */
static const char recolor_declarations[] =
  "vec3 rgb2yCrCb(vec3 c) {\n"
  "  float y = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;\n"
  "  float cr = 0.7133 * (c.r - y);\n"
  "  float cb = 0.5643 * (c.b - y);\n"
  "  return vec3(y, cr, cb);\n"
  "}\n"
  "\n"
  "uniform vec3 chroma_color;\n"
  "uniform vec3 recolor_color;\n"
  "uniform float threshold;\n"
  "uniform float smoothing;\n";

static const char recolor_code[] =
  "vec3 mask = rgb2yCrCb(chroma_color.rgb);\n"
  "vec3 yCrCb = rgb2yCrCb(cogl_color_out.rgb);\n"
  "float blend =\n"
  "  smoothstep(threshold,\n"
  "             threshold + smoothing,\n"
  "             distance(yCrCb.gb, mask.gb));\n"
  "cogl_color_out.rgb =\n"
  "  mix(recolor_color, cogl_color_out.rgb, blend);\n";

static CoglSnippet *
recolor_effect_get_static_snippet (ClutterShaderEffect *effect)
{
  return cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
                           recolor_declarations, recolor_code);
}

static void
recolor_effect_update_color_uniform (ShellScreenshotUIRecolorEffect *self,
                                     const char                     *name,
                                     const CoglColor                *color)
{
  float values[3] = { color->red / 255.f, color->green / 255.f, color->blue / 255.f };

  clutter_shader_effect_set_uniform_float (CLUTTER_SHADER_EFFECT (self),
                                           name, 3, 3, values);
  clutter_effect_queue_repaint (CLUTTER_EFFECT (self));
}

static void
recolor_effect_update_float_uniform (ShellScreenshotUIRecolorEffect *self,
                                     const char                     *name,
                                     float                           value)
{
  clutter_shader_effect_set_uniform_float (CLUTTER_SHADER_EFFECT (self),
                                           name, 1, 1, &value);
  clutter_effect_queue_repaint (CLUTTER_EFFECT (self));
}

static void
recolor_effect_set_color (ShellScreenshotUIRecolorEffect *self,
                          const CoglColor                *color)
{
  if (cogl_color_equal (&self->color, color))
    return;

  self->color = *color;
  recolor_effect_update_color_uniform (self, "recolor_color", &self->color);
}

static void
shell_screenshot_ui_recolor_effect_class_init (ShellScreenshotUIRecolorEffectClass *klass)
{
  ClutterShaderEffectClass *shader_class = CLUTTER_SHADER_EFFECT_CLASS (klass);

  shader_class->get_static_snippet = recolor_effect_get_static_snippet;
}

static void
shell_screenshot_ui_recolor_effect_init (ShellScreenshotUIRecolorEffect *self)
{
}

static ClutterEffect *
recolor_effect_new (void)
{
  ShellScreenshotUIRecolorEffect *self =
    g_object_new (SHELL_TYPE_SCREENSHOT_UI_RECOLOR_EFFECT, NULL);

  self->color = (CoglColor) COGL_COLOR_INIT (0, 0, 0, 0);
  self->chroma = (CoglColor) COGL_COLOR_INIT (80, 219, 181, 255);
  self->threshold = 0.04f;
  self->smoothing = 0.07f;

  recolor_effect_update_color_uniform (self, "recolor_color", &self->color);
  recolor_effect_update_color_uniform (self, "chroma_color", &self->chroma);
  recolor_effect_update_float_uniform (self, "threshold", self->threshold);
  recolor_effect_update_float_uniform (self, "smoothing", self->smoothing);

  return CLUTTER_EFFECT (self);
}

/* ------------------------------------------------------------------------ */
/* PickPixel                                                                 */
/* ------------------------------------------------------------------------ */

enum
{
  PICK_PROP_0,

  PICK_PROP_SCREENSHOT,

  PICK_N_PROPS
};

static GParamSpec *pick_props[PICK_N_PROPS] = { NULL, };

struct _ShellPickPixel
{
  StWidget parent;

  ShellScreenshot *screenshot;
  ShellGrabHelper *grab_helper;
  ClutterActor *preview_cursor;
  ClutterEffect *recolor_effect; /* owned by the preview cursor */

  CoglColor color;
  gboolean has_color;
  gboolean has_result;
  gboolean in_pick;

  GTask *task; /* the pending pick_async (), not owned */
};

G_DEFINE_FINAL_TYPE (ShellPickPixel, shell_pick_pixel, ST_TYPE_WIDGET)

typedef struct
{
  ShellPickPixel *self;
  int x;
  int y;
  gboolean finish_after;
} PickColorData;

static void
pick_color_data_free (PickColorData *data)
{
  g_object_unref (data->self);
  g_free (data);
}

static void
pick_pixel_finish_click (ShellPickPixel *self)
{
  self->has_result = self->has_color;
  if (self->grab_helper != NULL)
    shell_grab_helper_ungrab (self->grab_helper, FALSE);
}

static void
pick_color_done_cb (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  PickColorData *data = user_data;
  ShellPickPixel *self = data->self;
  g_autoptr (GError) error = NULL;
  CoglColor color;

  self->in_pick = FALSE;

  if (shell_screenshot_pick_color_finish (SHELL_SCREENSHOT (source), result, &color, &error))
    {
      self->color = color;
      self->has_color = TRUE;
    }
  else if (!shutdown_seen)
    {
      g_message ("Error picking the color: %s", error->message);
    }

  if (!shutdown_seen && self->preview_cursor != NULL && self->has_color)
    {
      recolor_effect_set_color (SHELL_SCREENSHOT_UI_RECOLOR_EFFECT (self->recolor_effect),
                                &self->color);
      clutter_actor_show (self->preview_cursor);
    }

  if (data->finish_after && !shutdown_seen)
    pick_pixel_finish_click (self);

  pick_color_data_free (data);
}

static void
pick_pixel_pick_color (ShellPickPixel *self,
                       int             x,
                       int             y,
                       gboolean        finish_after)
{
  PickColorData *data;

  if (self->in_pick)
    {
      if (finish_after)
        pick_pixel_finish_click (self);
      return;
    }

  self->in_pick = TRUE;
  clutter_actor_set_position (self->preview_cursor, x, y);

  data = g_new0 (PickColorData, 1);
  data->self = g_object_ref (self);
  data->x = x;
  data->y = y;
  data->finish_after = finish_after;
  shell_screenshot_pick_color (self->screenshot, x, y, pick_color_done_cb, data);
}

static void
pick_pixel_click_cb (ClutterClickGesture *gesture,
                     ShellPickPixel      *self)
{
  graphene_point_t coords;

  clutter_press_gesture_get_coords_abs (CLUTTER_PRESS_GESTURE (gesture), &coords);

  pick_pixel_pick_color (self, (int) coords.x, (int) coords.y, TRUE);
}

static void
pick_pixel_motion_cb (ClutterMotionController *controller,
                      ClutterSprite           *sprite,
                      float                    x,
                      float                    y,
                      ShellPickPixel          *self)
{
  pick_pixel_pick_color (self, (int) x, (int) y, FALSE);
}

static void
pick_pixel_shutdown_cb (ShellGlobal   *global,
                        ShellPickPixel *self)
{
  GTask *task = self->task;

  if (task == NULL)
    return;

  self->task = NULL;
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                           "The shell is shutting down");
}

static void
shell_pick_pixel_constructed (GObject *object)
{
  ShellPickPixel *self = SHELL_PICK_PIXEL (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellLayoutManager *layout_manager = get_layout_manager ();
  ClutterActor *ui_group, *preview_cursor;
  ClutterAction *action;

  G_OBJECT_CLASS (shell_pick_pixel_parent_class)->constructed (object);

  g_return_if_fail (layout_manager != NULL);
  ui_group = CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager));

  clutter_actor_hide (actor);
  clutter_actor_set_reactive (actor, TRUE);
  clutter_actor_add_child (ui_group, actor);

  self->grab_helper = shell_grab_helper_new (actor, SHELL_ACTION_MODE_NONE);

  add_bind_constraint (actor, CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ())),
                       CLUTTER_BIND_ALL);

  action = clutter_click_gesture_new ();
  g_signal_connect_object (action, "recognize",
                           G_CALLBACK (pick_pixel_click_cb), self, 0);
  clutter_actor_add_action (actor, action);

  action = clutter_motion_controller_new ();
  g_signal_connect_object (action, "motion",
                           G_CALLBACK (pick_pixel_motion_cb), self, 0);
  clutter_actor_add_action (actor, action);

  preview_cursor = g_object_new (ST_TYPE_ICON,
                                 "icon-name", "color-pick",
                                 "icon-size", meta_prefs_get_cursor_size (),
                                 "visible", FALSE,
                                 NULL);
  self->recolor_effect = recolor_effect_new ();
  clutter_actor_add_effect (preview_cursor, self->recolor_effect);
  /* The UI group owns the preview cursor */
  clutter_actor_add_child (ui_group, preview_cursor);
  g_set_weak_pointer (&self->preview_cursor, preview_cursor);

  clutter_actor_set_cursor_type (actor, CLUTTER_CURSOR_NONE);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (pick_pixel_shutdown_cb), self, 0);
}

static void
shell_pick_pixel_dispose (GObject *object)
{
  ShellPickPixel *self = SHELL_PICK_PIXEL (object);

  g_clear_object (&self->grab_helper);
  g_clear_object (&self->screenshot);

  if (self->preview_cursor != NULL)
    {
      ClutterActor *preview_cursor = self->preview_cursor;

      g_clear_weak_pointer (&self->preview_cursor);
      self->recolor_effect = NULL;
      clutter_actor_destroy (preview_cursor);
    }

  G_OBJECT_CLASS (shell_pick_pixel_parent_class)->dispose (object);
}

static void
shell_pick_pixel_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  ShellPickPixel *self = SHELL_PICK_PIXEL (object);

  switch (prop_id)
    {
    case PICK_PROP_SCREENSHOT:
      g_value_set_object (value, self->screenshot);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_pick_pixel_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  ShellPickPixel *self = SHELL_PICK_PIXEL (object);

  switch (prop_id)
    {
    case PICK_PROP_SCREENSHOT:
      g_set_object (&self->screenshot, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_pick_pixel_class_init (ShellPickPixelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_pick_pixel_constructed;
  object_class->dispose = shell_pick_pixel_dispose;
  object_class->get_property = shell_pick_pixel_get_property;
  object_class->set_property = shell_pick_pixel_set_property;

  /**
   * ShellPickPixel:screenshot:
   *
   * The #ShellScreenshot the pixels are read with.
   */
  pick_props[PICK_PROP_SCREENSHOT] =
    g_param_spec_object ("screenshot", NULL, NULL,
                         SHELL_TYPE_SCREENSHOT,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PICK_N_PROPS, pick_props);

  ensure_shutdown_watch ();
}

static void
shell_pick_pixel_init (ShellPickPixel *self)
{
}

static void
pick_pixel_grab_done_cb (GObject      *source,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  ShellPickPixel *self = g_task_get_source_object (task);
  g_autoptr (GError) error = NULL;

  /* The shutdown handler returned the task already */
  if (self->task != task)
    return;

  self->task = NULL;

  shell_grab_helper_grab_finish (SHELL_GRAB_HELPER (source), result, &error);

  if (self->preview_cursor != NULL)
    {
      ClutterActor *preview_cursor = self->preview_cursor;

      g_clear_weak_pointer (&self->preview_cursor);
      self->recolor_effect = NULL;
      clutter_actor_destroy (preview_cursor);
    }

  destroy_actor_in_idle (CLUTTER_ACTOR (self));

  if (error != NULL)
    g_task_return_error (task, g_steal_pointer (&error));
  else if (self->has_result)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                             "Operation was cancelled");
}

/**
 * shell_pick_pixel_pick_async:
 * @self: a #ShellPickPixel
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): function to call when the user picked a pixel or
 *   dismissed the picker
 * @user_data: the data to pass to @callback
 *
 * Lets the user pick the color of a pixel of the screen (`pickAsync ()`).
 * @self is destroyed when the picking is over, whatever the outcome.
 */
void
shell_pick_pixel_pick_async (ShellPickPixel      *self,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;
  ShellLayoutManager *layout_manager = get_layout_manager ();
  ClutterActor *ui_group;
  int x, y;

  g_return_if_fail (SHELL_IS_PICK_PIXEL (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, shell_pick_pixel_pick_async);

  if (self->task != NULL || shutdown_seen || layout_manager == NULL ||
      self->preview_cursor == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PENDING,
                               "Unable to pick a color");
      return;
    }

  ui_group = CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager));
  clutter_actor_set_child_above_sibling (ui_group, CLUTTER_ACTOR (self), NULL);
  clutter_actor_show (CLUTTER_ACTOR (self));

  shell_global_get_pointer (shell_global_get (), &x, &y, NULL);
  pick_pixel_pick_color (self, x, y, FALSE);

  self->task = task;
  shell_grab_helper_grab_async (self->grab_helper, CLUTTER_ACTOR (self), NULL,
                                pick_pixel_grab_done_cb, g_object_ref (task));
}

/**
 * shell_pick_pixel_pick_finish:
 * @self: a #ShellPickPixel
 * @result: the #GAsyncResult that was provided to the callback
 * @color: (out caller-allocates): the picked color
 * @error: #GError for error reporting
 *
 * Finish the asynchronous operation started by shell_pick_pixel_pick_async ().
 *
 * Returns: %TRUE if a color was picked, %FALSE with @error set (to
 *   %G_IO_ERROR_CANCELLED if the user dismissed the picker) otherwise
 */
gboolean
shell_pick_pixel_pick_finish (ShellPickPixel  *self,
                              GAsyncResult    *result,
                              CoglColor       *color,
                              GError         **error)
{
  g_return_val_if_fail (SHELL_IS_PICK_PIXEL (self), FALSE);
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  g_return_val_if_fail (color != NULL, FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result,
                                                  shell_pick_pixel_pick_async),
                        FALSE);

  if (!g_task_propagate_boolean (G_TASK (result), error))
    return FALSE;

  *color = self->color;
  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* ScreenshotService                                                         */
/* ------------------------------------------------------------------------ */

enum
{
  SERVICE_PROP_0,

  SERVICE_PROP_SCREENSHOT_UI,

  SERVICE_N_PROPS
};

static GParamSpec *service_props[SERVICE_N_PROPS] = { NULL, };

static const char * const SENDER_ALLOWLIST[] = {
  "org.gnome.SettingsDaemon.MediaKeys",
  "org.freedesktop.impl.portal.desktop.gnome",
  NULL
};

typedef struct
{
  ShellScreenshot *shooter;
  guint watch_id;
} ShooterEntry;

struct _ShellScreenshotService
{
  GObject parent;

  ShellDBusScreenshot *skeleton;
  ShellDBusSenderChecker *sender_checker;
  GSettings *lockdown_settings;
  guint name_id;

  /* sender (char *) -> ShooterEntry: the senders with an ongoing operation */
  GHashTable *shooters;
  /* of Request: the calls that did not get their reply yet */
  GList *pending;

  ShellScreenshotUI *screenshot_ui; /* weak */

  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellScreenshotService, shell_screenshot_service, G_TYPE_OBJECT)

typedef enum
{
  REQUEST_SCREENSHOT,
  REQUEST_SCREENSHOT_WINDOW,
  REQUEST_SCREENSHOT_AREA,
  REQUEST_PICK_COLOR,
  REQUEST_SELECT_AREA,
  REQUEST_FLASH_AREA,
  REQUEST_INTERACTIVE,
} RequestKind;

/* A call of the service that waits for something. The first reference is the
 * one of the code that handles the call; the asynchronous operations hold
 * their own. Replying (to the call or with an error) removes the request from
 * the pending list of the service. */
typedef struct _Request Request;

typedef void (* RequestContinueFunc) (Request *req);

struct _Request
{
  int ref_count;
  RequestKind kind;

  ShellScreenshotService *service; /* weak */
  GDBusMethodInvocation *invocation;
  gboolean replied;
  RequestContinueFunc next; /* what to do once the sender was checked */

  /* Screenshot, ScreenshotWindow and ScreenshotArea, FlashArea */
  int x;
  int y;
  int width;
  int height;
  gboolean include_frame;
  gboolean include_cursor;
  gboolean flash;
  char *filename;
  ShellScreenshot *shooter;
  GOutputStream *stream;
  GFile *file;
  gulong taken_id;
  int n_pending;
  gboolean failed;

  /* PickColor, SelectArea */
  GObject *picker;

  /* InteractiveScreenshot */
  ShellScreenshotUI *screenshot_ui; /* weak */
  gulong ui_taken_id;
  gulong ui_closed_id;
  gboolean ui_ref_held; /* the signal handlers hold a reference */
};

static void request_remove_shooter (Request *req);

static Request *
request_ref (Request *req)
{
  req->ref_count++;

  return req;
}

static void
request_unref (Request *req)
{
  if (--req->ref_count > 0)
    return;

  if (req->taken_id != 0 && req->shooter != NULL)
    g_signal_handler_disconnect (req->shooter, req->taken_id);
  if (req->screenshot_ui != NULL)
    {
      g_clear_signal_handler (&req->ui_taken_id, req->screenshot_ui);
      g_clear_signal_handler (&req->ui_closed_id, req->screenshot_ui);
    }

  if (req->service != NULL)
    {
      req->service->pending = g_list_remove (req->service->pending, req);
      g_clear_weak_pointer (&req->service);
    }
  g_clear_weak_pointer (&req->screenshot_ui);

  g_clear_object (&req->invocation);
  g_clear_object (&req->shooter);
  g_clear_object (&req->stream);
  g_clear_object (&req->file);
  g_clear_object (&req->picker);
  g_clear_pointer (&req->filename, g_free);
  g_free (req);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Request, request_unref)

static Request *
request_new (ShellScreenshotService *service,
             RequestKind             kind,
             GDBusMethodInvocation  *invocation)
{
  Request *req = g_new0 (Request, 1);

  req->ref_count = 1;
  req->kind = kind;
  req->invocation = g_object_ref (invocation);
  g_set_weak_pointer (&req->service, service);
  service->pending = g_list_prepend (service->pending, req);

  return req;
}

/* Marks the call as replied; %FALSE if it was replied already */
static gboolean
request_begin_reply (Request *req)
{
  if (req->replied)
    return FALSE;

  req->replied = TRUE;
  if (req->service != NULL)
    req->service->pending = g_list_remove (req->service->pending, req);

  if (req->screenshot_ui != NULL)
    {
      g_clear_signal_handler (&req->ui_taken_id, req->screenshot_ui);
      g_clear_signal_handler (&req->ui_closed_id, req->screenshot_ui);
    }

  /* The caller holds a reference of its own, so this does not free @req */
  if (req->ui_ref_held)
    {
      req->ui_ref_held = FALSE;
      request_unref (req);
    }

  return TRUE;
}

static void
request_return_error (Request    *req,
                      GQuark      domain,
                      int         code,
                      const char *message)
{
  if (!request_begin_reply (req))
    return;

  g_dbus_method_invocation_return_error_literal (req->invocation, domain, code, message);
}

static void
request_return_gerror (Request *req,
                       GError  *error)
{
  if (!request_begin_reply (req))
    return;

  g_dbus_method_invocation_return_gerror (req->invocation, error);
}

static void
request_return_cancelled (Request *req)
{
  request_return_error (req, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "Operation was cancelled");
}

/* Replies (success, filename) to Screenshot, ScreenshotWindow, ScreenshotArea
 * and InteractiveScreenshot */
static void
request_return_result (Request    *req,
                       gboolean    success,
                       const char *filename)
{
  ShellScreenshotService *service = req->service;

  if (!request_begin_reply (req))
    return;

  if (service == NULL)
    {
      g_dbus_method_invocation_return_error_literal (req->invocation, G_IO_ERROR,
                                                     G_IO_ERROR_CANCELLED,
                                                     "The shell is shutting down");
      return;
    }

  switch (req->kind)
    {
    case REQUEST_SCREENSHOT:
      shell_dbus_screenshot_complete_screenshot (service->skeleton, req->invocation,
                                                 success, filename);
      break;
    case REQUEST_SCREENSHOT_WINDOW:
      shell_dbus_screenshot_complete_screenshot_window (service->skeleton, req->invocation,
                                                        success, filename);
      break;
    case REQUEST_SCREENSHOT_AREA:
      shell_dbus_screenshot_complete_screenshot_area (service->skeleton, req->invocation,
                                                      success, filename);
      break;
    case REQUEST_INTERACTIVE:
      shell_dbus_screenshot_complete_interactive_screenshot (service->skeleton, req->invocation,
                                                             success, filename);
      break;
    default:
      g_assert_not_reached ();
    }
}

/* ------------------------------------------------------------------------ */
/* Per-sender screenshot objects                                             */
/* ------------------------------------------------------------------------ */

static void
shooter_entry_free (gpointer data)
{
  ShooterEntry *entry = data;

  g_bus_unwatch_name (entry->watch_id);
  g_object_unref (entry->shooter);
  g_free (entry);
}

static void
service_remove_shooter_for_sender (ShellScreenshotService *self,
                                   const char             *sender)
{
  if (self->shooters != NULL)
    g_hash_table_remove (self->shooters, sender);
}

static void
request_remove_shooter (Request *req)
{
  if (req->service == NULL)
    return;

  service_remove_shooter_for_sender (req->service,
                                     g_dbus_method_invocation_get_sender (req->invocation));
}

static void
shooter_name_vanished_cb (GDBusConnection *connection,
                          const char      *name,
                          gpointer         user_data)
{
  ShellScreenshotService *self = user_data;

  service_remove_shooter_for_sender (self, name);
}

/* Creates the screenshot object of the sender of the call; the request is
 * dropped with an error reply if that fails */
static gboolean
request_register_shooter (Request *req)
{
  ShellScreenshotService *self = req->service;
  const char *sender = g_dbus_method_invocation_get_sender (req->invocation);
  ShooterEntry *entry;

  if (self == NULL || self->shooters == NULL || self->shutting_down)
    {
      request_return_error (req, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "The shell is shutting down");
      return FALSE;
    }

  if (g_hash_table_contains (self->shooters, sender))
    {
      request_return_error (req, G_IO_ERROR, G_IO_ERROR_BUSY,
                            "There is an ongoing operation for this sender");
      return FALSE;
    }

  entry = g_new0 (ShooterEntry, 1);
  entry->shooter = shell_screenshot_new ();
  entry->watch_id = g_bus_watch_name (G_BUS_TYPE_SESSION, sender,
                                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                                      NULL, shooter_name_vanished_cb, self, NULL);
  g_hash_table_insert (self->shooters, g_strdup (sender), entry);

  req->shooter = g_object_ref (entry->shooter);

  return TRUE;
}

typedef struct
{
  Request *req;
  RequestContinueFunc continue_func;
} SenderCheckData;

static void
sender_checked_cb (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  SenderCheckData *data = user_data;
  g_autoptr (Request) req = data->req;
  RequestContinueFunc continue_func = data->continue_func;
  g_autoptr (GError) error = NULL;

  g_free (data);

  if (!shell_dbus_sender_checker_check_invocation_finish (SHELL_DBUS_SENDER_CHECKER (source),
                                                          result, &error))
    {
      request_return_gerror (req, error);
      return;
    }

  if (req->replied)
    return;

  continue_func (g_steal_pointer (&req));
}

/*
 * Checks that the sender of the call is allowed to make it, then calls
 * @continue_func, which takes the reference of @req. Takes the reference of
 * @req.
 */
static void
request_check_sender (Request             *req,
                      RequestContinueFunc  continue_func)
{
  ShellScreenshotService *self = req->service;
  SenderCheckData *data;

  if (self == NULL || self->shutting_down)
    {
      request_return_error (req, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "The shell is shutting down");
      request_unref (req);
      return;
    }

  data = g_new0 (SenderCheckData, 1);
  data->req = req;
  data->continue_func = continue_func;
  shell_dbus_sender_checker_check_invocation_async (self->sender_checker,
                                                    req->invocation, NULL,
                                                    sender_checked_cb, data);
}

static void
create_screenshot_checked (Request             *req,
                           RequestContinueFunc  continue_func)
{
  if (!request_register_shooter (req))
    {
      request_unref (req);
      return;
    }

  continue_func (req);
}

static void
create_screenshot_sender_checked (Request *req)
{
  RequestContinueFunc continue_func = req->next;

  req->next = NULL;
  create_screenshot_checked (req, continue_func);
}

/*
 * `_createScreenshot ()`: checks that the sender has no ongoing operation,
 * that saving to disk is not locked down (if @needs_disk) and that the sender
 * is allowed to take screenshots (if @restrict_callers), then creates the
 * #ShellScreenshot of the sender and calls @continue_func with it set in
 * @req->shooter. @continue_func takes the reference of @req. Takes the
 * reference of @req; replies with the error if the checks fail.
 */
static void
request_create_screenshot (Request             *req,
                           gboolean             needs_disk,
                           gboolean             restrict_callers,
                           RequestContinueFunc  continue_func)
{
  ShellScreenshotService *self = req->service;
  const char *sender = g_dbus_method_invocation_get_sender (req->invocation);
  gboolean locked_down = FALSE;

  if (self == NULL || self->shutting_down)
    {
      request_return_error (req, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "The shell is shutting down");
      request_unref (req);
      return;
    }

  if (needs_disk)
    locked_down = g_settings_get_boolean (self->lockdown_settings,
                                          DISABLE_SAVE_TO_DISK_KEY);

  if (g_hash_table_contains (self->shooters, sender))
    {
      request_return_error (req, G_IO_ERROR, G_IO_ERROR_BUSY,
                            "There is an ongoing operation for this sender");
      request_unref (req);
    }
  else if (locked_down)
    {
      request_return_error (req, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Saving to disk is disabled");
      request_unref (req);
    }
  else if (restrict_callers)
    {
      req->next = continue_func;
      request_check_sender (req, create_screenshot_sender_checked);
    }
  else
    {
      create_screenshot_checked (req, continue_func);
    }
}

/* ------------------------------------------------------------------------ */
/* Areas                                                                     */
/* ------------------------------------------------------------------------ */

static gboolean
check_area (int x,
            int y,
            int width,
            int height)
{
  ShellGlobal *global = shell_global_get ();

  return x >= 0 && y >= 0 &&
         width > 0 && height > 0 &&
         x + width <= shell_global_get_screen_width (global) &&
         y + height <= shell_global_get_screen_height (global);
}

static void
scale_area (int *x,
            int *y,
            int *width,
            int *height)
{
  int scale_factor = get_scale_factor ();

  *x *= scale_factor;
  *y *= scale_factor;
  *width *= scale_factor;
  *height *= scale_factor;
}

static void
unscale_area (int *x,
              int *y,
              int *width,
              int *height)
{
  int scale_factor = get_scale_factor ();

  *x /= scale_factor;
  *y /= scale_factor;
  *width /= scale_factor;
  *height /= scale_factor;
}

/* ------------------------------------------------------------------------ */
/* Streams                                                                   */
/* ------------------------------------------------------------------------ */

/* Creates the file of a screenshot saved under a relative name: in the
 * pictures directory, or in the home directory, with a suffix if the file
 * exists already (`_resolveRelativeFilename ()` and `_createStream ()`) */
static GFileOutputStream *
create_relative_stream (const char  *filename,
                        GFile      **file_out,
                        GError     **error)
{
  g_autofree char *basename = NULL;
  const char *pictures_dir = g_get_user_special_dir (G_USER_DIRECTORY_PICTURES);
  const char *path = NULL;
  const char *candidates[] = { pictures_dir, g_get_home_dir () };
  gsize i;
  int idx;

  basename = g_str_has_suffix (filename, ".png")
    ? g_strndup (filename, strlen (filename) - strlen (".png"))
    : g_strdup (filename);

  for (i = 0; i < G_N_ELEMENTS (candidates); i++)
    {
      if (candidates[i] != NULL && candidates[i][0] != '\0' &&
          g_file_test (candidates[i], G_FILE_TEST_EXISTS))
        {
          path = candidates[i];
          break;
        }
    }

  if (path == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "No directory to save the screenshot to");
      return NULL;
    }

  for (idx = 0; ; idx++)
    {
      g_autofree char *name = idx == 0
        ? g_strdup_printf ("%s.png", basename)
        : g_strdup_printf ("%s-%d.png", basename, idx);
      g_autofree char *file_path = g_build_filename (path, name, NULL);
      g_autoptr (GFile) file = g_file_new_for_path (file_path);
      g_autoptr (GError) create_error = NULL;
      GFileOutputStream *stream;

      stream = g_file_create (file, G_FILE_CREATE_NONE, NULL, &create_error);
      if (stream != NULL)
        {
          *file_out = g_steal_pointer (&file);
          return stream;
        }

      if (!g_error_matches (create_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
          g_propagate_error (error, g_steal_pointer (&create_error));
          return NULL;
        }
    }
}

/* Creates the stream the screenshot is written to; replies with the error
 * and removes the shooter of the sender if that fails */
static gboolean
request_create_stream (Request *req)
{
  g_autoptr (GError) error = NULL;

  if (req->filename[0] == '\0')
    {
      req->stream = g_memory_output_stream_new_resizable ();
      return TRUE;
    }

  if (g_path_is_absolute (req->filename))
    {
      g_autoptr (GFile) file = g_file_new_for_path (req->filename);
      GFileOutputStream *stream;

      stream = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &error);
      if (stream != NULL)
        {
          req->stream = G_OUTPUT_STREAM (stream);
          req->file = g_steal_pointer (&file);
          return TRUE;
        }
    }
  else
    {
      GFileOutputStream *stream;

      stream = create_relative_stream (req->filename, &req->file, &error);
      if (stream != NULL)
        {
          req->stream = G_OUTPUT_STREAM (stream);
          return TRUE;
        }
    }

  request_return_gerror (req, error);
  request_remove_shooter (req);
  return FALSE;
}

/* ------------------------------------------------------------------------ */
/* Screenshot, ScreenshotWindow, ScreenshotArea                              */
/* ------------------------------------------------------------------------ */

/* `_onScreenshotComplete ()`; replies and removes the shooter of the sender */
static void
request_screenshot_complete (Request *req)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *filename_used = NULL;

  if (!g_output_stream_close (req->stream, NULL, &error))
    {
      request_return_result (req, FALSE, "");
      request_remove_shooter (req);
      return;
    }

  if (req->file != NULL)
    {
      filename_used = g_file_get_path (req->file);
    }
  else
    {
      g_autoptr (GBytes) bytes =
        g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (req->stream));

      st_clipboard_set_content (st_clipboard_get_default (), ST_CLIPBOARD_TYPE_CLIPBOARD,
                                "image/png", bytes);
    }

  request_return_result (req, TRUE, filename_used != NULL ? filename_used : "");
  request_remove_shooter (req);
}

static void
request_screenshot_maybe_complete (Request *req)
{
  if (!req->failed && !req->replied && req->n_pending == 0)
    request_screenshot_complete (req);
}

static void
request_screenshot_fail (Request *req)
{
  req->failed = TRUE;

  request_return_result (req, FALSE, "");
  request_remove_shooter (req);
}

static void
flash_done_cb (gpointer user_data)
{
  Request *req = user_data;

  req->n_pending--;
  request_screenshot_maybe_complete (req);
}

static void
shooter_screenshot_taken_cb (ShellScreenshot *shooter,
                             MtkRectangle    *area,
                             Request         *req)
{
  g_autoptr (ShellFlashspot) flashspot = NULL;

  if (shutdown_seen || req->replied)
    return;

  flashspot = shell_flashspot_new (area);
  shell_flashspot_fire (flashspot, flash_done_cb,
                        request_ref (req), (GDestroyNotify) request_unref);

  play_screenshot_sound ();
}

static void
screenshot_done_cb (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr (Request) req = user_data;
  ShellScreenshot *shooter = SHELL_SCREENSHOT (source);
  g_autoptr (GError) error = NULL;
  MtkRectangle *area = NULL;
  gboolean success;

  switch (req->kind)
    {
    case REQUEST_SCREENSHOT_AREA:
      success = shell_screenshot_screenshot_area_finish (shooter, result, &area, &error);
      break;
    case REQUEST_SCREENSHOT_WINDOW:
      success = shell_screenshot_screenshot_window_finish (shooter, result, &area, &error);
      break;
    case REQUEST_SCREENSHOT:
      success = shell_screenshot_screenshot_finish (shooter, result, &area, &error);
      break;
    default:
      g_assert_not_reached ();
    }

  if (req->replied)
    return;

  if (!success)
    {
      request_screenshot_fail (req);
      return;
    }

  req->n_pending--;
  request_screenshot_maybe_complete (req);
}

/* The stream is created and the shooter registered: take the screenshot */
static void
screenshot_request_start (Request *req)
{
  g_autoptr (Request) owned = req;

  if (!request_create_stream (req))
    return;

  req->n_pending = 1;
  if (req->flash)
    {
      req->n_pending++;
      req->taken_id = g_signal_connect (req->shooter, "screenshot-taken",
                                        G_CALLBACK (shooter_screenshot_taken_cb), req);
    }

  switch (req->kind)
    {
    case REQUEST_SCREENSHOT_AREA:
      shell_screenshot_screenshot_area (req->shooter,
                                        req->x, req->y, req->width, req->height,
                                        req->stream,
                                        screenshot_done_cb, request_ref (req));
      break;
    case REQUEST_SCREENSHOT_WINDOW:
      shell_screenshot_screenshot_window (req->shooter,
                                          req->include_frame, req->include_cursor,
                                          req->stream,
                                          screenshot_done_cb, request_ref (req));
      break;
    case REQUEST_SCREENSHOT:
      shell_screenshot_screenshot (req->shooter, req->include_cursor,
                                   req->stream,
                                   screenshot_done_cb, request_ref (req));
      break;
    default:
      g_assert_not_reached ();
    }
}

static gboolean
on_handle_screenshot_area (ShellDBusScreenshot    *skeleton,
                           GDBusMethodInvocation  *invocation,
                           int                     x,
                           int                     y,
                           int                     width,
                           int                     height,
                           gboolean                flash,
                           const char             *filename,
                           ShellScreenshotService *self)
{
  Request *req;

  scale_area (&x, &y, &width, &height);
  if (!check_area (x, y, width, height))
    {
      g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR,
                                                     G_IO_ERROR_CANCELLED,
                                                     "Invalid params");
      return TRUE;
    }

  req = request_new (self, REQUEST_SCREENSHOT_AREA, invocation);
  req->x = x;
  req->y = y;
  req->width = width;
  req->height = height;
  req->flash = flash;
  req->filename = g_strdup (filename);
  request_create_screenshot (req, TRUE, TRUE, screenshot_request_start);

  return TRUE;
}

static gboolean
on_handle_screenshot_window (ShellDBusScreenshot    *skeleton,
                             GDBusMethodInvocation  *invocation,
                             gboolean                include_frame,
                             gboolean                include_cursor,
                             gboolean                flash,
                             const char             *filename,
                             ShellScreenshotService *self)
{
  Request *req = request_new (self, REQUEST_SCREENSHOT_WINDOW, invocation);

  req->include_frame = include_frame;
  req->include_cursor = include_cursor;
  req->flash = flash;
  req->filename = g_strdup (filename);
  request_create_screenshot (req, TRUE, TRUE, screenshot_request_start);

  return TRUE;
}

static gboolean
on_handle_screenshot (ShellDBusScreenshot    *skeleton,
                      GDBusMethodInvocation  *invocation,
                      gboolean                include_cursor,
                      gboolean                flash,
                      const char             *filename,
                      ShellScreenshotService *self)
{
  Request *req = request_new (self, REQUEST_SCREENSHOT, invocation);

  req->include_cursor = include_cursor;
  req->flash = flash;
  req->filename = g_strdup (filename);
  request_create_screenshot (req, TRUE, TRUE, screenshot_request_start);

  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* InteractiveScreenshot                                                     */
/* ------------------------------------------------------------------------ */

static void
interactive_screenshot_taken_cb (ShellScreenshotUI *ui,
                                 GFile             *file,
                                 Request           *req)
{
  g_autofree char *uri = g_file_get_uri (file);
  g_autoptr (Request) owned = request_ref (req);

  request_return_result (req, TRUE, uri);
}

static void
interactive_closed_cb (ShellScreenshotUI *ui,
                       Request           *req)
{
  g_autoptr (Request) owned = request_ref (req);

  request_return_result (req, FALSE, "");
}

static void
interactive_request_start (Request *req)
{
  ShellScreenshotService *self = req->service;

  if (self == NULL || self->screenshot_ui == NULL)
    {
      request_return_result (req, FALSE, "");
      request_unref (req);
      return;
    }

  /* The signal handlers keep the reference until the call is replied */
  req->ui_ref_held = TRUE;
  g_set_weak_pointer (&req->screenshot_ui, self->screenshot_ui);
  req->ui_taken_id = g_signal_connect (self->screenshot_ui, "screenshot-taken",
                                       G_CALLBACK (interactive_screenshot_taken_cb), req);
  req->ui_closed_id = g_signal_connect (self->screenshot_ui, "closed",
                                        G_CALLBACK (interactive_closed_cb), req);

  shell_screenshot_ui_open (self->screenshot_ui, SHELL_SCREENSHOT_UI_MODE_SCREENSHOT_ONLY);
}

static gboolean
on_handle_interactive_screenshot (ShellDBusScreenshot    *skeleton,
                                  GDBusMethodInvocation  *invocation,
                                  ShellScreenshotService *self)
{
  Request *req = request_new (self, REQUEST_INTERACTIVE, invocation);

  request_check_sender (req, interactive_request_start);

  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* SelectArea                                                                */
/* ------------------------------------------------------------------------ */

static void
select_area_done_cb (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  g_autoptr (Request) req = user_data;
  ShellScreenshotService *self = req->service;
  g_autoptr (MtkRectangle) area = NULL;

  area = shell_select_area_select_finish (SHELL_SELECT_AREA (source), result, NULL);
  if (req->replied)
    return;

  if (area == NULL || self == NULL)
    {
      request_return_cancelled (req);
      return;
    }

  if (request_begin_reply (req))
    {
      int x = area->x, y = area->y, width = area->width, height = area->height;

      unscale_area (&x, &y, &width, &height);
      shell_dbus_screenshot_complete_select_area (self->skeleton, req->invocation,
                                                  x, y, width, height);
    }
}

static void
select_area_request_start (Request *req)
{
  g_autoptr (Request) owned = req;

  req->picker = g_object_ref_sink (g_object_new (SHELL_TYPE_SELECT_AREA, NULL));
  shell_select_area_select_async (SHELL_SELECT_AREA (req->picker), NULL,
                                  select_area_done_cb, request_ref (req));
}

static gboolean
on_handle_select_area (ShellDBusScreenshot    *skeleton,
                       GDBusMethodInvocation  *invocation,
                       ShellScreenshotService *self)
{
  Request *req = request_new (self, REQUEST_SELECT_AREA, invocation);

  request_check_sender (req, select_area_request_start);

  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* FlashArea                                                                 */
/* ------------------------------------------------------------------------ */

static void
flash_area_request_start (Request *req)
{
  g_autoptr (Request) owned = req;
  g_autoptr (ShellFlashspot) flashspot = NULL;
  ShellScreenshotService *self = req->service;
  MtkRectangle area;

  if (self == NULL)
    {
      request_return_cancelled (req);
      return;
    }

  /* The area is checked here, as it was after the sender check */
  scale_area (&req->x, &req->y, &req->width, &req->height);
  if (!check_area (req->x, req->y, req->width, req->height))
    {
      request_return_error (req, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Invalid params");
      return;
    }

  area = (MtkRectangle) { req->x, req->y, req->width, req->height };
  flashspot = shell_flashspot_new (&area);
  shell_flashspot_fire (flashspot, NULL, NULL, NULL);

  if (request_begin_reply (req))
    shell_dbus_screenshot_complete_flash_area (self->skeleton, req->invocation);
}

static gboolean
on_handle_flash_area (ShellDBusScreenshot    *skeleton,
                      GDBusMethodInvocation  *invocation,
                      int                     x,
                      int                     y,
                      int                     width,
                      int                     height,
                      ShellScreenshotService *self)
{
  Request *req = request_new (self, REQUEST_FLASH_AREA, invocation);

  req->x = x;
  req->y = y;
  req->width = width;
  req->height = height;
  request_check_sender (req, flash_area_request_start);

  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* PickColor                                                                 */
/* ------------------------------------------------------------------------ */

static void
pick_color_done_request_cb (GObject      *source,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  g_autoptr (Request) req = user_data;
  ShellScreenshotService *self = req->service;
  CoglColor color;

  if (shell_pick_pixel_pick_finish (SHELL_PICK_PIXEL (source), result, &color, NULL) &&
      self != NULL && request_begin_reply (req))
    {
      GVariantBuilder builder;

      g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&builder, "{sv}", "color",
                             g_variant_new ("(ddd)",
                                            color.red / 255.0,
                                            color.green / 255.0,
                                            color.blue / 255.0));
      shell_dbus_screenshot_complete_pick_color (self->skeleton, req->invocation,
                                                 g_variant_builder_end (&builder));
    }
  else
    {
      request_return_cancelled (req);
    }

  request_remove_shooter (req);
}

static void
pick_color_request_start (Request *req)
{
  g_autoptr (Request) owned = req;

  req->picker = g_object_ref_sink (g_object_new (SHELL_TYPE_PICK_PIXEL,
                                                 "screenshot", req->shooter,
                                                 NULL));
  shell_pick_pixel_pick_async (SHELL_PICK_PIXEL (req->picker), NULL,
                               pick_color_done_request_cb, request_ref (req));
}

static gboolean
on_handle_pick_color (ShellDBusScreenshot    *skeleton,
                      GDBusMethodInvocation  *invocation,
                      ShellScreenshotService *self)
{
  Request *req = request_new (self, REQUEST_PICK_COLOR, invocation);

  request_create_screenshot (req, FALSE, FALSE, pick_color_request_start);

  return TRUE;
}

/* ------------------------------------------------------------------------ */
/* ShellScreenshotService: construction                                      */
/* ------------------------------------------------------------------------ */

/* Fails the calls that wait for something */
static void
service_fail_pending (ShellScreenshotService *self)
{
  g_autoptr (GList) pending = g_list_copy (self->pending);
  GList *l;

  for (l = pending; l != NULL; l = l->next)
    {
      Request *req = request_ref (l->data);

      request_return_error (req, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "The shell is shutting down");
      request_unref (req);
    }
}

static void
service_shutdown_cb (ShellGlobal            *global,
                     ShellScreenshotService *self)
{
  self->shutting_down = TRUE;

  service_fail_pending (self);
  if (self->shooters != NULL)
    g_hash_table_remove_all (self->shooters);
}

static void
shell_screenshot_service_constructed (GObject *object)
{
  ShellScreenshotService *self = SHELL_SCREENSHOT_SERVICE (object);
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GError) error = NULL;

  G_OBJECT_CLASS (shell_screenshot_service_parent_class)->constructed (object);

  self->shooters = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, shooter_entry_free);
  self->sender_checker = shell_dbus_sender_checker_new (SENDER_ALLOWLIST);
  self->lockdown_settings = g_settings_new (LOCKDOWN_SCHEMA);

  self->skeleton = shell_dbus_screenshot_skeleton_new ();
  g_signal_connect (self->skeleton, "handle-interactive-screenshot",
                    G_CALLBACK (on_handle_interactive_screenshot), self);
  g_signal_connect (self->skeleton, "handle-screenshot",
                    G_CALLBACK (on_handle_screenshot), self);
  g_signal_connect (self->skeleton, "handle-screenshot-window",
                    G_CALLBACK (on_handle_screenshot_window), self);
  g_signal_connect (self->skeleton, "handle-screenshot-area",
                    G_CALLBACK (on_handle_screenshot_area), self);
  g_signal_connect (self->skeleton, "handle-pick-color",
                    G_CALLBACK (on_handle_pick_color), self);
  g_signal_connect (self->skeleton, "handle-flash-area",
                    G_CALLBACK (on_handle_flash_area), self);
  g_signal_connect (self->skeleton, "handle-select-area",
                    G_CALLBACK (on_handle_select_area), self);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (service_shutdown_cb), self, 0);

  connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (connection == NULL)
    {
      g_warning ("%s: %s", G_STRFUNC, error->message);
      return;
    }

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                         connection, SCREENSHOT_OBJECT_PATH, &error))
    {
      g_warning ("%s: %s", G_STRFUNC, error->message);
      return;
    }

  self->name_id = g_bus_own_name_on_connection (connection, SCREENSHOT_BUS_NAME,
                                                G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                                NULL, NULL, NULL, NULL);
}

static void
shell_screenshot_service_dispose (GObject *object)
{
  ShellScreenshotService *self = SHELL_SCREENSHOT_SERVICE (object);

  self->shutting_down = TRUE;

  service_fail_pending (self);

  if (self->name_id != 0)
    {
      g_bus_unown_name (self->name_id);
      self->name_id = 0;
    }

  if (self->skeleton != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->skeleton, self);
      if (g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (self->skeleton)) != NULL)
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->skeleton));
      g_clear_object (&self->skeleton);
    }

  g_clear_pointer (&self->shooters, g_hash_table_unref);
  g_clear_object (&self->sender_checker);
  g_clear_object (&self->lockdown_settings);
  g_clear_weak_pointer (&self->screenshot_ui);

  G_OBJECT_CLASS (shell_screenshot_service_parent_class)->dispose (object);
}

static void
shell_screenshot_service_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellScreenshotService *self = SHELL_SCREENSHOT_SERVICE (object);

  switch (prop_id)
    {
    case SERVICE_PROP_SCREENSHOT_UI:
      g_value_set_object (value, self->screenshot_ui);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_screenshot_service_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellScreenshotService *self = SHELL_SCREENSHOT_SERVICE (object);

  switch (prop_id)
    {
    case SERVICE_PROP_SCREENSHOT_UI:
      g_set_weak_pointer (&self->screenshot_ui, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_screenshot_service_class_init (ShellScreenshotServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_screenshot_service_constructed;
  object_class->dispose = shell_screenshot_service_dispose;
  object_class->get_property = shell_screenshot_service_get_property;
  object_class->set_property = shell_screenshot_service_set_property;

  /**
   * ShellScreenshotService:screenshot-ui:
   *
   * The screenshot UI InteractiveScreenshot () opens (`Main.screenshotUI`).
   */
  service_props[SERVICE_PROP_SCREENSHOT_UI] =
    g_param_spec_object ("screenshot-ui", NULL, NULL,
                         SHELL_TYPE_SCREENSHOT_UI,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, SERVICE_N_PROPS, service_props);

  ensure_shutdown_watch ();
}

static void
shell_screenshot_service_init (ShellScreenshotService *self)
{
}

/**
 * shell_screenshot_service_new:
 * @screenshot_ui: (nullable): the screenshot UI InteractiveScreenshot () opens
 *
 * Creates the org.gnome.Shell.Screenshot service, exports it at
 * /org/gnome/Shell/Screenshot and owns the bus name
 * (`new ScreenshotService ()`).
 *
 * Returns: (transfer full): a new #ShellScreenshotService
 */
ShellScreenshotService *
shell_screenshot_service_new (ShellScreenshotUI *screenshot_ui)
{
  g_return_val_if_fail (screenshot_ui == NULL || SHELL_IS_SCREENSHOT_UI (screenshot_ui), NULL);

  return g_object_new (SHELL_TYPE_SCREENSHOT_SERVICE,
                       "screenshot-ui", screenshot_ui,
                       NULL);
}
