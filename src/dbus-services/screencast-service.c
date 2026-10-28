/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * screencast-service.c: The screen recorder behind org.gnome.Shell.Screencast.
 * Port of js/dbusServices/screencast/screencastService.js and main.js.
 *
 * The process owns org.gnome.Shell.Screencast and exports
 * /org/gnome/Shell/Screencast. A client (the screenshot UI of the shell, which
 * drives it through the ShellDBusScreencast proxy) asks it to record the whole
 * screen or an area. The service opens a screen cast session in mutter
 * (org.gnome.Mutter.ScreenCast), waits for the PipeWire node of the stream and
 * runs a GStreamer pipeline that encodes it to a file. There is one recording
 * per client; the recording stops when the client calls StopScreencast, or
 * when it leaves the bus.
 *
 * The pipelines are tried in the order of the pipelines table (hardware
 * encoders first, the software VP8 encoder as the fallback), unless the client
 * passed its own `pipeline` option. A pipeline is put on a blocklist, a file
 * in the runtime directory, while it runs; if the service crashes with it, it
 * is skipped the next time. The blocklist is cleared when the recording ends
 * well or fails for a reason that has nothing to do with the pipeline.
 *
 * The service exits when it has been idle for a while and there is no
 * recording; it exits at once if the pipeline elements it needs are missing.
 *
 * Differences from the JS version:
 *
 *  - The errors of the sync calls to mutter (CreateSession) and the failure to
 *    read the size of the screen are returned to the client. The JS async
 *    function threw and never replied. The size of the screen is read before
 *    the session is created, so that no session is left behind.
 *  - With a custom `pipeline` option the file name was the template and the
 *    extension `undefined`; the template is used as it is.
 *  - A recorder that is dropped while it is still running (StopScreencast
 *    while it is still starting) is shut down: the pipeline is stopped and
 *    the session is closed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <stdint.h>
#include <string.h>

#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

#include "screencast-service-dbus.h"
#include "introspect-service-dbus.h"
#include "mutter-screencast-dbus.h"
#include "shell-dbus-service.h"

#define SCREENCAST_BUS_NAME "org.gnome.Shell.Screencast"
#define SCREENCAST_OBJECT_PATH "/org/gnome/Shell/Screencast"

#define DEFAULT_FRAMERATE 30
#define DEFAULT_DRAW_CURSOR TRUE

#define PIPELINE_BLOCKLIST_FILENAME "gnome-shell-screencast-pipeline-blocklist"

/* Errors */

typedef enum
{
  SCREENCAST_ERROR_ALL_PIPELINES_FAILED,
  SCREENCAST_ERROR_PIPELINE_ERROR,
  SCREENCAST_ERROR_SAVE_TO_DISK_DISABLED,
  SCREENCAST_ERROR_ALREADY_RECORDING,
  SCREENCAST_ERROR_RECORDER_ERROR,
  SCREENCAST_ERROR_SERVICE_CRASH,
  SCREENCAST_ERROR_OUT_OF_DISK_SPACE,
} ScreencastError;

static const GDBusErrorEntry screencast_error_entries[] = {
  { SCREENCAST_ERROR_ALL_PIPELINES_FAILED,
    "org.gnome.Shell.Screencast.Error.AllPipelinesFailed" },
  { SCREENCAST_ERROR_PIPELINE_ERROR,
    "org.gnome.Shell.Screencast.Error.PipelineError" },
  { SCREENCAST_ERROR_SAVE_TO_DISK_DISABLED,
    "org.gnome.Shell.Screencast.Error.SaveToDiskDisabled" },
  { SCREENCAST_ERROR_ALREADY_RECORDING,
    "org.gnome.Shell.Screencast.Error.AlreadyRecording" },
  { SCREENCAST_ERROR_RECORDER_ERROR,
    "org.gnome.Shell.Screencast.Error.RecorderError" },
  { SCREENCAST_ERROR_SERVICE_CRASH,
    "org.gnome.Shell.Screencast.Error.ServiceCrash" },
  { SCREENCAST_ERROR_OUT_OF_DISK_SPACE,
    "org.gnome.Shell.Screencast.Error.OutOfDiskSpace" },
};

static GQuark
screencast_error_quark (void)
{
  static size_t quark_volatile = 0;

  g_dbus_error_register_error_domain ("shell-screencast-error",
                                      &quark_volatile,
                                      screencast_error_entries,
                                      G_N_ELEMENTS (screencast_error_entries));
  return (GQuark) quark_volatile;
}

#define SCREENCAST_ERROR (screencast_error_quark ())

/* Pipelines */

typedef struct
{
  const char *id;
  const char *file_extension;
  const char *pipeline_string;
} PipelineConfig;

/* %T is replaced by the number of threads and %F by the frame rate */
static const PipelineConfig pipelines[] = {
  {
    "hwenc-dmabuf-h264-vaapi-lp",
    "mp4",
    "capsfilter caps=video/x-raw(memory:DMABuf),format=DMA_DRM,max-framerate=%F/1 ! "
    "vapostproc ! "
    "vah264lpenc rate-control=vbr bitrate=2048 target-percentage=50 ! "
    "queue ! "
    "h264parse ! "
    "mp4mux fragment-duration=500 fragment-mode=first-moov-then-finalise",
  },
  {
    "hwenc-dmabuf-h264-vaapi",
    "mp4",
    "capsfilter caps=video/x-raw(memory:DMABuf),format=DMA_DRM,max-framerate=%F/1 ! "
    "vapostproc ! "
    "vah264enc rate-control=vbr bitrate=2048 target-percentage=50 ! "
    "queue ! "
    "h264parse ! "
    "mp4mux fragment-duration=500 fragment-mode=first-moov-then-finalise",
  },
  {
    "swenc-dmabuf-h264-openh264",
    "mp4",
    "capsfilter caps=video/x-raw(memory:DMABuf),max-framerate=%F/1 ! "
    "glupload ! glcolorconvert ! gldownload ! "
    "queue ! "
    "openh264enc deblocking=off background-detection=false complexity=low adaptive-quantization=false qp-max=26 qp-min=26 multi-thread=%T slice-mode=auto ! "
    "queue ! "
    "h264parse ! "
    "mp4mux fragment-duration=500 fragment-mode=first-moov-then-finalise",
  },
  {
    "swenc-memfd-h264-openh264",
    "mp4",
    "capsfilter caps=video/x-raw,max-framerate=%F/1 ! "
    "videoconvert chroma-mode=none dither=none matrix-mode=output-only n-threads=%T ! "
    "queue ! "
    "openh264enc deblocking=off background-detection=false complexity=low adaptive-quantization=false qp-max=26 qp-min=26 multi-thread=%T slice-mode=auto ! "
    "queue ! "
    "h264parse ! "
    "mp4mux fragment-duration=500 fragment-mode=first-moov-then-finalise",
  },
  {
    "swenc-dmabuf-vp8-vp8enc",
    "webm",
    "capsfilter caps=video/x-raw(memory:DMABuf),max-framerate=%F/1 ! "
    "glupload ! glcolorconvert ! gldownload ! "
    "queue ! "
    "vp8enc cpu-used=16 max-quantizer=17 deadline=1 keyframe-mode=disabled threads=%T static-threshold=1000 buffer-size=20000 ! "
    "queue ! "
    "webmmux",
  },
  {
    "swenc-memfd-vp8-vp8enc",
    "webm",
    "capsfilter caps=video/x-raw,max-framerate=%F/1 ! "
    "videoconvert chroma-mode=none dither=none matrix-mode=output-only n-threads=%T ! "
    "queue ! "
    "vp8enc cpu-used=16 max-quantizer=17 deadline=1 keyframe-mode=disabled threads=%T static-threshold=1000 buffer-size=20000 ! "
    "queue ! "
    "webmmux",
  },
};

typedef enum
{
  PIPELINE_STATE_INIT,
  PIPELINE_STATE_STARTING,
  PIPELINE_STATE_PLAYING,
  PIPELINE_STATE_FLUSHING,
  PIPELINE_STATE_STOPPED,
  PIPELINE_STATE_ERROR,
} PipelineState;

static const char *
pipeline_state_name (PipelineState state)
{
  switch (state)
    {
    case PIPELINE_STATE_INIT:
      return "INIT";
    case PIPELINE_STATE_STARTING:
      return "STARTING";
    case PIPELINE_STATE_PLAYING:
      return "PLAYING";
    case PIPELINE_STATE_FLUSHING:
      return "FLUSHING";
    case PIPELINE_STATE_STOPPED:
      return "STOPPED";
    case PIPELINE_STATE_ERROR:
      return "ERROR";
    default:
      g_assert_not_reached ();
    }
}

typedef enum
{
  SESSION_STATE_INIT,
  SESSION_STATE_ACTIVE,
  SESSION_STATE_STOPPED,
} SessionState;

static gboolean
element_exists (const char *name)
{
  GstElementFactory *factory = gst_element_factory_find (name);

  if (factory == NULL)
    return FALSE;

  gst_object_unref (factory);
  return TRUE;
}

static gboolean
can_screencast (void)
{
  static const char *const elements[] = { "pipewiresrc", "filesink" };
  const PipelineConfig *fallback = &pipelines[G_N_ELEMENTS (pipelines) - 1];
  g_auto (GStrv) parts = NULL;
  size_t i;

  if (!gst_init_check (NULL, NULL, NULL))
    return FALSE;

  for (i = 0; i < G_N_ELEMENTS (elements); i++)
    {
      if (!element_exists (elements[i]))
        return FALSE;
    }

  /* The fallback pipeline must be available, the other ones are not
   * guaranteed to work because they depend on hw encoders. */
  parts = g_strsplit (fallback->pipeline_string, "!", -1);
  for (i = 0; parts[i] != NULL; i++)
    {
      char *space;

      g_strstrip (parts[i]);
      space = strchr (parts[i], ' ');
      if (space != NULL)
        *space = '\0';

      if (!element_exists (parts[i]))
        return FALSE;
    }

  return TRUE;
}

/* Replaces every @needle in @string by @replacement */
static char *
replace_all (const char *string,
             const char *needle,
             const char *replacement)
{
  g_auto (GStrv) parts = g_strsplit (string, needle, -1);

  return g_strjoinv (replacement, parts);
}

static char *
substitute_variables (const char *pipeline_descr,
                      int32_t     framerate)
{
  int num_processors = (int) g_get_num_processors ();
  int num_threads = CLAMP (num_processors, 1, 64);
  g_autofree char *threads = g_strdup_printf ("%d", num_threads);
  g_autofree char *rate = g_strdup_printf ("%d", framerate);
  g_autofree char *with_threads = replace_all (pipeline_descr, "%T", threads);

  return replace_all (with_threads, "%F", rate);
}

/* The recorder of a client */

#define SCREENCAST_TYPE_RECORDER (screencast_recorder_get_type ())
G_DECLARE_FINAL_TYPE (ScreencastRecorder, screencast_recorder,
                      SCREENCAST, RECORDER, GObject)

enum
{
  RECORDER_ERROR_SIGNAL,
  N_RECORDER_SIGNALS
};

static unsigned int recorder_signals[N_RECORDER_SIGNALS] = { 0 };

struct _ScreencastRecorder
{
  GObject parent;

  GDBusConnection *connection;
  char *sender;

  int x;
  int y;
  int width;
  int height;
  char *file_path_stem;
  char *file_path;

  char *pipeline_string;
  int32_t framerate;
  gboolean draw_cursor;
  GPtrArray *blocklist_from_previous_crashes; /* char *, can hold NULL */
  GFile *pipeline_blocklist_file;

  PipelineState pipeline_state;
  GstElement *pipeline;
  unsigned int bus_watch_id;
  unsigned int name_watch_id;

  SessionState session_state;
  ShellMutterScreenCastSession *session_proxy;
  ShellMutterScreenCastStream *stream_proxy;

  GTask *start_request;
  GTask *stop_request;

  uint32_t node_id;
  char *custom_pipeline;
  size_t next_config;
  size_t n_configs;
};

G_DEFINE_FINAL_TYPE (ScreencastRecorder, screencast_recorder, G_TYPE_OBJECT)

static void try_next_pipeline (ScreencastRecorder *self);

static void
add_recent_item (ScreencastRecorder *self)
{
  g_autoptr (GFile) file = NULL;
  g_autofree char *uri = NULL;

  if (self->file_path == NULL)
    return;

  file = g_file_new_for_path (self->file_path);
  uri = g_file_get_uri (file);
  gtk_recent_manager_add_item (gtk_recent_manager_get_default (), uri);
}

static void
unwatch_sender (ScreencastRecorder *self)
{
  g_clear_handle_id (&self->name_watch_id, g_bus_unwatch_name);
}

static void
teardown_pipeline (ScreencastRecorder *self)
{
  if (self->pipeline == NULL)
    return;

  if (gst_element_set_state (self->pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_SUCCESS)
    g_warning ("Failed to set pipeline state to NULL");

  self->pipeline_state = PIPELINE_STATE_STOPPED;
  g_clear_handle_id (&self->bus_watch_id, g_source_remove);
  g_clear_pointer (&self->pipeline, gst_object_unref);
}

static void
stop_session (ScreencastRecorder *self)
{
  g_autoptr (GError) error = NULL;

  if (self->session_state != SESSION_STATE_ACTIVE)
    return;

  self->session_state = SESSION_STATE_STOPPED;
  if (!shell_mutter_screen_cast_session_call_stop_sync (self->session_proxy,
                                                        NULL, &error))
    g_debug ("Failed to stop the screen cast session: %s", error->message);
}

/* Rewrites the blocklist file: the list of the pipelines of the previous
 * crashes, plus @id if @append_id is set (a custom pipeline has no id, which
 * is a null entry in the file). The file is deleted if the list is empty. */
static void
update_service_crash_blocklist (ScreencastRecorder *self,
                                gboolean            append_id,
                                const char         *id)
{
  g_autoptr (GError) error = NULL;
  size_t n_entries = self->blocklist_from_previous_crashes->len + (append_id ? 1 : 0);

  if (n_entries == 0)
    {
      if (!g_file_delete (self->pipeline_blocklist_file, NULL, &error) &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_message ("Failed to update pipeline-blocklist file: %s", error->message);
    }
  else
    {
      g_autoptr (JsonGenerator) generator = json_generator_new ();
      g_autoptr (JsonNode) root = json_node_new (JSON_NODE_ARRAY);
      g_autoptr (JsonArray) array = json_array_new ();
      g_autofree char *contents = NULL;
      size_t length = 0;
      unsigned int i;

      for (i = 0; i < self->blocklist_from_previous_crashes->len; i++)
        {
          const char *entry = g_ptr_array_index (self->blocklist_from_previous_crashes, i);

          if (entry != NULL)
            json_array_add_string_element (array, entry);
          else
            json_array_add_null_element (array);
        }

      if (append_id)
        {
          if (id != NULL)
            json_array_add_string_element (array, id);
          else
            json_array_add_null_element (array);
        }

      json_node_set_array (root, array);
      json_generator_set_root (generator, root);
      contents = json_generator_to_data (generator, &length);

      if (!g_file_replace_contents (self->pipeline_blocklist_file,
                                    contents, length, NULL, FALSE,
                                    G_FILE_CREATE_NONE, NULL, NULL, &error))
        g_message ("Failed to update pipeline-blocklist file: %s", error->message);
    }
}

static gboolean
blocklist_contains (ScreencastRecorder *self,
                    const char         *id)
{
  unsigned int i;

  if (id == NULL)
    return FALSE;

  for (i = 0; i < self->blocklist_from_previous_crashes->len; i++)
    {
      const char *entry = g_ptr_array_index (self->blocklist_from_previous_crashes, i);

      if (g_strcmp0 (entry, id) == 0)
        return TRUE;
    }

  return FALSE;
}

static void
bail_out_on_error (ScreencastRecorder *self,
                   GQuark              domain,
                   int                 code,
                   const char         *message)
{
  g_autoptr (ScreencastRecorder) keep_alive = g_object_ref (self);
  g_autoptr (GError) error = g_error_new_literal (domain, code, message);

  g_debug ("Bailing out with error %s", error->message);

  /* If it's a PIPELINE_ERROR, we want to leave the failing pipeline on the
   * blocklist for the next time. Other errors are pipeline-independent, so
   * reset the blocklist to allow the pipeline to be tried again next time. */
  if (!g_error_matches (error, SCREENCAST_ERROR, SCREENCAST_ERROR_PIPELINE_ERROR))
    update_service_crash_blocklist (self, FALSE, NULL);

  teardown_pipeline (self);
  unwatch_sender (self);
  stop_session (self);

  if (self->start_request != NULL)
    {
      GTask *task = g_steal_pointer (&self->start_request);

      g_task_return_error (task, g_error_copy (error));
      g_object_unref (task);
    }

  if (self->stop_request != NULL)
    {
      GTask *task = g_steal_pointer (&self->stop_request);

      g_task_return_error (task, g_error_copy (error));
      g_object_unref (task);
    }

  g_signal_emit (self, recorder_signals[RECORDER_ERROR_SIGNAL], 0, error);
}

static void
handle_fatal_pipeline_error (ScreencastRecorder *self,
                             const char         *message,
                             GQuark              domain,
                             int                 code)
{
  self->pipeline_state = PIPELINE_STATE_ERROR;
  bail_out_on_error (self, domain, code, message);
}

static void
sender_vanished_cb (GDBusConnection *connection,
                    const char      *name,
                    gpointer         user_data)
{
  ScreencastRecorder *self = user_data;

  bail_out_on_error (self, SCREENCAST_ERROR, SCREENCAST_ERROR_RECORDER_ERROR,
                     "Sender has vanished");
}

static void
session_closed_cb (ShellMutterScreenCastSession *proxy,
                   ScreencastRecorder           *self)
{
  if (self->session_state == SESSION_STATE_STOPPED)
    return; /* We closed the session ourselves */

  self->session_state = SESSION_STATE_STOPPED;
  bail_out_on_error (self, SCREENCAST_ERROR, SCREENCAST_ERROR_RECORDER_ERROR,
                     "Session closed unexpectedly");
}

static void
resolve_start_request (ScreencastRecorder *self)
{
  GTask *task = g_steal_pointer (&self->start_request);

  if (task == NULL)
    return;

  g_task_return_pointer (task, g_strdup (self->file_path), g_free);
  g_object_unref (task);
}

static gboolean
bus_message_cb (GstBus     *bus,
                GstMessage *message,
                gpointer    user_data)
{
  g_autoptr (ScreencastRecorder) self = g_object_ref (user_data);

  switch (GST_MESSAGE_TYPE (message))
    {
    case GST_MESSAGE_STATE_CHANGED:
      {
        GstState new_state;

        gst_message_parse_state_changed (message, NULL, &new_state, NULL);

        if (self->pipeline_state == PIPELINE_STATE_STARTING &&
            self->pipeline != NULL &&
            GST_MESSAGE_SRC (message) == GST_OBJECT (self->pipeline) &&
            new_state == GST_STATE_PLAYING)
          {
            self->pipeline_state = PIPELINE_STATE_PLAYING;
            resolve_start_request (self);
          }

        break;
      }

    case GST_MESSAGE_EOS:
      switch (self->pipeline_state)
        {
        case PIPELINE_STATE_INIT:
        case PIPELINE_STATE_STOPPED:
        case PIPELINE_STATE_ERROR:
          /* In these cases there should be no pipeline, so should never
           * happen */
          break;

        case PIPELINE_STATE_STARTING:
          /* This is something we can handle, try to switch to the next
           * pipeline */
          g_debug ("Received EOS message while trying to start pipeline");
          try_next_pipeline (self);
          break;

        case PIPELINE_STATE_PLAYING:
          add_recent_item (self);
          handle_fatal_pipeline_error (self, "Unexpected EOS message",
                                       SCREENCAST_ERROR,
                                       SCREENCAST_ERROR_PIPELINE_ERROR);
          break;

        case PIPELINE_STATE_FLUSHING:
          {
            GTask *task;

            /* The pipeline ran successfully and we didn't crash; we can
             * remove it from the blocklist again now. */
            update_service_crash_blocklist (self, FALSE, NULL);

            add_recent_item (self);

            teardown_pipeline (self);
            unwatch_sender (self);
            stop_session (self);

            task = g_steal_pointer (&self->stop_request);
            if (task != NULL)
              {
                g_task_return_boolean (task, TRUE);
                g_object_unref (task);
              }
            break;
          }

        default:
          break;
        }

      break;

    case GST_MESSAGE_ERROR:
      {
        g_autoptr (GError) error = NULL;
        g_autofree char *debug = NULL;

        gst_message_parse_error (message, &error, &debug);

        switch (self->pipeline_state)
          {
          case PIPELINE_STATE_INIT:
          case PIPELINE_STATE_STOPPED:
          case PIPELINE_STATE_ERROR:
            /* In these cases there should be no pipeline, so should never
             * happen */
            break;

          case PIPELINE_STATE_STARTING:
            /* This is something we can handle, try to switch to the next
             * pipeline */
            g_debug ("Received ERROR message while trying to start pipeline: %s",
                     error->message);
            try_next_pipeline (self);
            break;

          case PIPELINE_STATE_PLAYING:
          case PIPELINE_STATE_FLUSHING:
            if (g_error_matches (error, GST_RESOURCE_ERROR,
                                 GST_RESOURCE_ERROR_NO_SPACE_LEFT))
              {
                handle_fatal_pipeline_error (self, "Out of disk space",
                                             SCREENCAST_ERROR,
                                             SCREENCAST_ERROR_OUT_OF_DISK_SPACE);
              }
            else
              {
                g_autofree char *text =
                  g_strdup_printf ("GStreamer error while in state %s: %s",
                                   pipeline_state_name (self->pipeline_state),
                                   error->message);

                handle_fatal_pipeline_error (self, text, SCREENCAST_ERROR,
                                             SCREENCAST_ERROR_PIPELINE_ERROR);
              }
            break;

          default:
            break;
          }

        break;
      }

    default:
      break;
    }

  return G_SOURCE_CONTINUE;
}

static GstElement *
create_pipeline (ScreencastRecorder *self,
                 const PipelineConfig *config,
                 const char          *pipeline_string,
                 GError             **error)
{
  g_autofree char *final_pipeline_string = NULL;
  g_autofree char *full_pipeline = NULL;

  final_pipeline_string = substitute_variables (pipeline_string, self->framerate);

  g_free (self->file_path);
  if (config->file_extension != NULL)
    self->file_path = g_strdup_printf ("%s.%s", self->file_path_stem,
                                       config->file_extension);
  else
    self->file_path = g_strdup (self->file_path_stem);

  full_pipeline = g_strdup_printf ("pipewiresrc path=%u "
                                   "do-timestamp=true "
                                   "keepalive-time=1000 "
                                   "resend-last=true ! "
                                   "%s ! "
                                   "filesink location=\"%s\"",
                                   self->node_id,
                                   final_pipeline_string,
                                   self->file_path);

  return gst_parse_launch_full (full_pipeline, NULL,
                                GST_PARSE_FLAG_FATAL_ERRORS, error);
}

/* The pipelines to try: the custom one, or all of them if the PipeWire source
 * can fall back to a format that the software pipelines can use, or only the
 * last (the software one that always works) */
static gboolean
get_next_config (ScreencastRecorder *self,
                 PipelineConfig     *config,
                 const char        **pipeline_string)
{
  if (self->next_config >= self->n_configs)
    return FALSE;

  if (self->custom_pipeline != NULL)
    {
      config->id = NULL;
      config->file_extension = NULL;
      config->pipeline_string = NULL;
      *pipeline_string = self->custom_pipeline;
    }
  else
    {
      size_t first = G_N_ELEMENTS (pipelines) - self->n_configs;

      *config = pipelines[first + self->next_config];
      *pipeline_string = config->pipeline_string;
    }

  self->next_config++;
  return TRUE;
}

static void
try_next_pipeline (ScreencastRecorder *self)
{
  for (;;)
    {
      PipelineConfig config;
      const char *pipeline_string;
      g_autoptr (GError) error = NULL;
      GstStateChangeReturn retval;
      GstBus *bus;

      if (self->file_path != NULL)
        {
          g_unlink (self->file_path);
          g_clear_pointer (&self->file_path, g_free);
        }

      if (!get_next_config (self, &config, &pipeline_string))
        {
          handle_fatal_pipeline_error (self, "All pipelines failed to start",
                                       SCREENCAST_ERROR,
                                       SCREENCAST_ERROR_ALL_PIPELINES_FAILED);
          return;
        }

      if (blocklist_contains (self, config.id))
        {
          g_info ("Skipping pipeline '%s' due to pipeline blocklist", config.id);
          continue;
        }

      if (self->pipeline != NULL)
        {
          if (gst_element_set_state (self->pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_SUCCESS)
            g_warning ("Failed to set pipeline state to NULL");

          g_clear_handle_id (&self->bus_watch_id, g_source_remove);
          g_clear_pointer (&self->pipeline, gst_object_unref);
        }

      g_debug ("Creating pipeline for config %s",
               config.id != NULL ? config.id : "(custom)");
      self->pipeline = create_pipeline (self, &config, pipeline_string, &error);
      if (self->pipeline == NULL)
        {
          g_debug ("Failed to create pipeline: %s",
                   error != NULL ? error->message : "unknown error");
          continue;
        }

      g_debug ("Pipeline created successfully");

      /* Add the current pipeline to the blocklist, so it is skipped next
       * time in case we crash; we'll remove it again on success or on
       * non-pipeline-related failures. */
      update_service_crash_blocklist (self, TRUE, config.id);

      bus = gst_element_get_bus (self->pipeline);
      self->bus_watch_id = gst_bus_add_watch (bus, bus_message_cb, self);
      gst_object_unref (bus);

      retval = gst_element_set_state (self->pipeline, GST_STATE_PLAYING);

      if (retval == GST_STATE_CHANGE_SUCCESS ||
          retval == GST_STATE_CHANGE_ASYNC)
        {
          /* We'll wait for the state change message to PLAYING on the bus */
          return;
        }

      g_debug ("Setting PLAYING state on pipeline returned %s",
               gst_element_state_change_return_get_name (retval));
    }
}

static void
pipe_wire_stream_added_cb (ShellMutterScreenCastStream *proxy,
                           uint32_t                     node_id,
                           ScreencastRecorder          *self)
{
  gboolean fallback_supported;

  self->node_id = node_id;

  self->pipeline_state = PIPELINE_STATE_STARTING;

  if (self->custom_pipeline != NULL)
    {
      self->n_configs = 1;
    }
  else
    {
      fallback_supported =
        gst_registry_check_feature_version (gst_registry_get (),
                                            "pipewiresrc", 0, 3, 67);
      self->n_configs = fallback_supported ? G_N_ELEMENTS (pipelines) : 1;
    }
  self->next_config = 0;

  try_next_pipeline (self);
}

static void
screencast_recorder_start_async (ScreencastRecorder  *self,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (self, NULL, callback, user_data);
  g_autoptr (GError) error = NULL;
  g_autofree char *stream_path = NULL;
  GVariantBuilder builder;

  g_debug ("Start recording");

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "is-recording",
                         g_variant_new_boolean (TRUE));
  g_variant_builder_add (&builder, "{sv}", "cursor-mode",
                         g_variant_new_uint32 (self->draw_cursor ? 1 : 0));

  if (!shell_mutter_screen_cast_session_call_record_area_sync (self->session_proxy,
                                                               self->x, self->y,
                                                               self->width, self->height,
                                                               g_variant_builder_end (&builder),
                                                               &stream_path,
                                                               NULL, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  self->stream_proxy =
    shell_mutter_screen_cast_stream_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                            "org.gnome.Mutter.ScreenCast",
                                                            stream_path,
                                                            NULL, &error);
  if (self->stream_proxy == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_signal_connect (self->stream_proxy, "pipe-wire-stream-added",
                    G_CALLBACK (pipe_wire_stream_added_cb), self);

  if (!shell_mutter_screen_cast_session_call_start_sync (self->session_proxy,
                                                         NULL, &error))
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  self->session_state = SESSION_STATE_ACTIVE;
  self->start_request = g_steal_pointer (&task);
}

/* Returns: (transfer full): the path of the file that is being recorded */
static char *
screencast_recorder_start_finish (ScreencastRecorder  *self,
                                  GAsyncResult        *result,
                                  GError             **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
screencast_recorder_stop_async (ScreencastRecorder  *self,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  g_autoptr (GTask) task = g_task_new (self, NULL, callback, user_data);

  if (self->start_request != NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Unable to stop recorder while still starting");
      return;
    }

  if (self->pipeline == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "The recorder has no pipeline");
      return;
    }

  g_debug ("Stop recording");

  self->stop_request = g_steal_pointer (&task);
  self->pipeline_state = PIPELINE_STATE_FLUSHING;
  gst_element_send_event (self->pipeline, gst_event_new_eos ());
}

static gboolean
screencast_recorder_stop_finish (ScreencastRecorder  *self,
                                 GAsyncResult        *result,
                                 GError             **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static const char *
screencast_recorder_get_sender (ScreencastRecorder *self)
{
  return self->sender;
}

static gboolean
load_blocklist (ScreencastRecorder  *self,
                GError             **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autofree char *contents = NULL;
  JsonNode *root;
  JsonArray *array;
  size_t length;
  unsigned int i;

  if (!g_file_load_contents (self->pipeline_blocklist_file, NULL,
                             &contents, &length, NULL, error))
    return FALSE;

  if (!json_parser_load_from_data (parser, contents, length, error))
    return FALSE;

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "The pipeline blocklist is not an array");
      return FALSE;
    }

  array = json_node_get_array (root);
  for (i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *element = json_array_get_element (array, i);

      if (JSON_NODE_HOLDS_NULL (element))
        g_ptr_array_add (self->blocklist_from_previous_crashes, NULL);
      else if (json_node_get_value_type (element) == G_TYPE_STRING)
        g_ptr_array_add (self->blocklist_from_previous_crashes,
                         json_node_dup_string (element));
    }

  return TRUE;
}

static void
apply_options (ScreencastRecorder *self,
               GVariant           *options)
{
  const char *pipeline;
  int32_t framerate;
  gboolean draw_cursor;

  if (g_variant_lookup (options, "pipeline", "&s", &pipeline))
    {
      /* An empty string means that there is no custom pipeline */
      if (pipeline[0] != '\0')
        {
          self->custom_pipeline =
            g_strdup_printf ("capsfilter caps=video/x-raw,max-framerate=%%F/1 ! %s",
                             pipeline);
        }
    }

  if (g_variant_lookup (options, "framerate", "i", &framerate))
    self->framerate = framerate;

  if (g_variant_lookup (options, "draw-cursor", "b", &draw_cursor))
    self->draw_cursor = draw_cursor;
}

static void
screencast_recorder_dispose (GObject *object)
{
  ScreencastRecorder *self = SCREENCAST_RECORDER (object);

  /* A recorder that is dropped while it is running shuts down */
  teardown_pipeline (self);
  unwatch_sender (self);
  stop_session (self);

  if (self->start_request != NULL)
    {
      GTask *task = g_steal_pointer (&self->start_request);

      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "The recorder was destroyed");
      g_object_unref (task);
    }

  if (self->stop_request != NULL)
    {
      GTask *task = g_steal_pointer (&self->stop_request);

      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "The recorder was destroyed");
      g_object_unref (task);
    }

  if (self->session_proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->session_proxy, self);
  if (self->stream_proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->stream_proxy, self);

  g_clear_object (&self->session_proxy);
  g_clear_object (&self->stream_proxy);
  g_clear_object (&self->pipeline_blocklist_file);
  g_clear_object (&self->connection);

  G_OBJECT_CLASS (screencast_recorder_parent_class)->dispose (object);
}

static void
screencast_recorder_finalize (GObject *object)
{
  ScreencastRecorder *self = SCREENCAST_RECORDER (object);

  g_clear_pointer (&self->blocklist_from_previous_crashes, g_ptr_array_unref);
  g_free (self->sender);
  g_free (self->file_path_stem);
  g_free (self->file_path);
  g_free (self->custom_pipeline);

  G_OBJECT_CLASS (screencast_recorder_parent_class)->finalize (object);
}

static void
screencast_recorder_class_init (ScreencastRecorderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = screencast_recorder_dispose;
  object_class->finalize = screencast_recorder_finalize;

  /* ScreencastRecorder::error:
   * @error: the error
   *
   * The recording failed after it had started. */
  recorder_signals[RECORDER_ERROR_SIGNAL] =
    g_signal_new ("error",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_ERROR);
}

static void
screencast_recorder_init (ScreencastRecorder *self)
{
  self->framerate = DEFAULT_FRAMERATE;
  self->draw_cursor = DEFAULT_DRAW_CURSOR;
  self->blocklist_from_previous_crashes = g_ptr_array_new_with_free_func (g_free);
  self->pipeline_state = PIPELINE_STATE_INIT;
  self->session_state = SESSION_STATE_INIT;
}

static ScreencastRecorder *
screencast_recorder_new (const char             *session_path,
                         int                     x,
                         int                     y,
                         int                     width,
                         int                     height,
                         const char             *file_path_stem,
                         GVariant               *options,
                         GDBusMethodInvocation  *invocation,
                         GError                **error)
{
  g_autoptr (ScreencastRecorder) self = g_object_new (SCREENCAST_TYPE_RECORDER, NULL);
  g_autoptr (GFile) dir = NULL;
  g_autoptr (GError) local_error = NULL;
  g_autofree char *blocklist_path = NULL;

  self->connection = g_object_ref (g_dbus_method_invocation_get_connection (invocation));
  self->sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));

  self->x = x;
  self->y = y;
  self->width = width;
  self->height = height;
  self->file_path_stem = g_strdup (file_path_stem);

  {
    g_autoptr (GFile) file = g_file_new_for_path (file_path_stem);

    dir = g_file_get_parent (file);
  }

  if (dir != NULL &&
      !g_file_make_directory_with_parents (dir, NULL, &local_error) &&
      !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }
  g_clear_error (&local_error);

  blocklist_path = g_build_filename (g_get_user_runtime_dir (),
                                     PIPELINE_BLOCKLIST_FILENAME, NULL);
  self->pipeline_blocklist_file = g_file_new_for_path (blocklist_path);

  if (!load_blocklist (self, &local_error))
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return NULL;
        }
      g_clear_error (&local_error);
    }

  apply_options (self, options);

  self->name_watch_id =
    g_bus_watch_name_on_connection (self->connection, self->sender,
                                    G_BUS_NAME_WATCHER_FLAGS_NONE,
                                    NULL, sender_vanished_cb, self, NULL);

  self->session_proxy =
    shell_mutter_screen_cast_session_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                             G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                             "org.gnome.Mutter.ScreenCast",
                                                             session_path,
                                                             NULL, error);
  if (self->session_proxy == NULL)
    return NULL;

  g_signal_connect (self->session_proxy, "closed",
                    G_CALLBACK (session_closed_cb), self);

  return g_steal_pointer (&self);
}

/* The service */

#define SCREENCAST_TYPE_SERVICE (screencast_service_get_type ())
G_DECLARE_FINAL_TYPE (ScreencastService, screencast_service,
                      SCREENCAST, SERVICE, ShellDBusService)

struct _ScreencastService
{
  ShellDBusService parent;

  ShellDBusScreencast *skeleton;
  GHashTable *recorders; /* char * sender -> ScreencastRecorder */
  GSettings *lockdown_settings;
  ShellMutterScreenCast *proxy;
  ShellIntrospect *introspect_proxy;
};

G_DEFINE_FINAL_TYPE (ScreencastService, screencast_service, SHELL_TYPE_DBUS_SERVICE)

static void
remove_recorder (ScreencastService *self,
                 const char        *sender)
{
  g_autofree char *sender_copy = g_strdup (sender);

  if (!g_hash_table_remove (self->recorders, sender_copy))
    return;

  if (g_hash_table_size (self->recorders) == 0)
    shell_dbus_service_release (SHELL_DBUS_SERVICE (self));
}

static void
add_recorder (ScreencastService   *self,
              const char          *sender,
              ScreencastRecorder  *recorder)
{
  g_hash_table_insert (self->recorders, g_strdup (sender),
                       g_object_ref (recorder));

  if (g_hash_table_size (self->recorders) == 1)
    shell_dbus_service_hold (SHELL_DBUS_SERVICE (self));
}

static char *
get_absolute_path (const char *filename)
{
  const char *video_dir;

  if (g_path_is_absolute (filename))
    return g_strdup (filename);

  video_dir = g_get_user_special_dir (G_USER_DIRECTORY_VIDEOS);
  if (video_dir == NULL || video_dir[0] == '\0')
    video_dir = g_get_home_dir ();

  return g_build_filename (video_dir, filename, NULL);
}

static char *
generate_file_path (const char *template)
{
  g_autoptr (GString) filename = g_string_new (NULL);
  g_autofree char *stripped = NULL;
  gboolean escape = FALSE;
  const char *p;

  /* FIXME: temporarily detect and strip .webm prefix to avoid breaking
   * external consumers of our API, remove this again */
  if (g_str_has_suffix (template, ".webm"))
    {
      g_message ("'file_template' for screencast includes '.webm' file-extension. Passing the file-extension as part of the filename has been deprecated, pass the 'file_template' without a file-extension instead.");
      stripped = g_strndup (template, strlen (template) - strlen (".webm"));
      template = stripped;
    }

  for (p = template; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (escape)
        {
          switch (c)
            {
            case '%':
              g_string_append_c (filename, '%');
              break;

            case 'd':
              {
                g_autoptr (GDateTime) datetime = g_date_time_new_now_local ();
                g_autofree char *datestr = g_date_time_format (datetime, "%Y-%m-%d");

                g_string_append (filename, datestr);
                break;
              }

            case 't':
              {
                g_autoptr (GDateTime) datetime = g_date_time_new_now_local ();
                g_autofree char *datestr = g_date_time_format (datetime, "%H-%M-%S");

                g_string_append (filename, datestr);
                break;
              }

            default:
              {
                char buf[7] = { 0, };

                g_unichar_to_utf8 (c, buf);
                g_warning ("Unknown escape %s", buf);
              }
            }

          escape = FALSE;
        }
      else if (c == '%')
        {
          escape = TRUE;
        }
      else
        {
          g_string_append_unichar (filename, c);
        }
    }

  if (escape)
    g_string_append_c (filename, '%');

  return get_absolute_path (filename->str);
}

static void
return_screencast_error (ScreencastService      *self,
                         GDBusMethodInvocation  *invocation,
                         ScreencastError         code,
                         const char             *message)
{
  shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (self), invocation);
  g_dbus_method_invocation_return_error_literal (invocation, SCREENCAST_ERROR,
                                                 code, message);
}

static gboolean
get_screen_size (ScreencastService  *self,
                 int                *width,
                 int                *height,
                 GError            **error)
{
  GVariant *size;

  if (self->introspect_proxy == NULL)
    {
      self->introspect_proxy =
        shell_introspect_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 "org.gnome.Shell.Introspect",
                                                 "/org/gnome/Shell/Introspect",
                                                 NULL, error);
      if (self->introspect_proxy == NULL)
        return FALSE;
    }

  size = shell_introspect_get_screen_size (self->introspect_proxy);
  if (size == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "The size of the screen is not known");
      return FALSE;
    }

  g_variant_get (size, "(ii)", width, height);
  return TRUE;
}

static gboolean
lockdown_disables_saving (ScreencastService *self)
{
  return self->lockdown_settings != NULL &&
         g_settings_get_boolean (self->lockdown_settings,
                                 "disable-save-to-disk");
}

typedef struct
{
  ScreencastService *service;
  GDBusMethodInvocation *invocation;
  ScreencastRecorder *recorder;
  char *sender;
  gboolean area;
} StartData;

static void
start_data_free (StartData *data)
{
  g_object_unref (data->service);
  g_object_unref (data->recorder);
  g_free (data->sender);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (StartData, start_data_free)

static void
recorder_error_cb (ScreencastRecorder *recorder,
                   const GError       *error,
                   ScreencastService  *self)
{
  g_autofree char *sender = g_strdup (screencast_recorder_get_sender (recorder));
  g_autofree char *name = g_dbus_error_encode_gerror (error);

  g_warning ("Fatal error while recording: %s", error->message);
  remove_recorder (self, sender);
  shell_dbus_screencast_emit_error (self->skeleton, name, error->message);
}

static void
recorder_started_cb (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  g_autoptr (StartData) data = user_data;
  ScreencastService *self = data->service;
  g_autoptr (GError) error = NULL;
  g_autofree char *path_with_extension = NULL;

  path_with_extension = screencast_recorder_start_finish (data->recorder,
                                                          result, &error);
  if (path_with_extension == NULL)
    {
      g_warning ("Failed to start recorder: %s", error->message);
      remove_recorder (self, data->sender);

      /* The errors of the sync calls to mutter are returned as they are */
      shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (self),
                                           data->invocation);
      g_dbus_method_invocation_return_gerror (data->invocation, error);
      return;
    }

  shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (self),
                                       data->invocation);
  if (data->area)
    shell_dbus_screencast_complete_screencast_area (self->skeleton,
                                                    data->invocation, TRUE,
                                                    path_with_extension);
  else
    shell_dbus_screencast_complete_screencast (self->skeleton,
                                               data->invocation, TRUE,
                                               path_with_extension);

  g_signal_connect_object (data->recorder, "error",
                           G_CALLBACK (recorder_error_cb), self, 0);
}

static void
start_screencast (ScreencastService     *self,
                  GDBusMethodInvocation *invocation,
                  gboolean               area,
                  int                    x,
                  int                    y,
                  int                    width,
                  int                    height,
                  const char            *file_template,
                  GVariant              *options)
{
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr (ScreencastRecorder) recorder = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *session_path = NULL;
  g_autofree char *file_path_stem = NULL;
  StartData *data;

  if (lockdown_disables_saving (self))
    {
      return_screencast_error (self, invocation,
                               SCREENCAST_ERROR_SAVE_TO_DISK_DISABLED,
                               "Saving to disk is disabled");
      return;
    }

  if (g_hash_table_lookup (self->recorders, sender) != NULL)
    {
      return_screencast_error (self, invocation,
                               SCREENCAST_ERROR_ALREADY_RECORDING,
                               "Service is already recording");
      return;
    }

  if (!area && !get_screen_size (self, &width, &height, &error))
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self), invocation,
                                       error);
      return;
    }

  if (self->proxy == NULL)
    {
      self->proxy =
        shell_mutter_screen_cast_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                         G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                         "org.gnome.Mutter.ScreenCast",
                                                         "/org/gnome/Mutter/ScreenCast",
                                                         NULL, &error);
      if (self->proxy == NULL)
        {
          shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self),
                                           invocation, error);
          return;
        }
    }

  if (!shell_mutter_screen_cast_call_create_session_sync (self->proxy,
                                                          g_variant_new ("a{sv}", NULL),
                                                          &session_path,
                                                          NULL, &error))
    {
      shell_dbus_service_return_error (SHELL_DBUS_SERVICE (self), invocation,
                                       error);
      return;
    }

  file_path_stem = generate_file_path (file_template);

  recorder = screencast_recorder_new (session_path, x, y, width, height,
                                      file_path_stem, options, invocation,
                                      &error);
  if (recorder == NULL)
    {
      g_warning ("Failed to create recorder: %s", error->message);
      return_screencast_error (self, invocation,
                               SCREENCAST_ERROR_RECORDER_ERROR,
                               error->message);
      return;
    }

  add_recorder (self, sender, recorder);

  data = g_new0 (StartData, 1);
  data->service = g_object_ref (self);
  data->invocation = invocation;
  data->recorder = g_object_ref (recorder);
  data->sender = g_strdup (sender);
  data->area = area;

  screencast_recorder_start_async (recorder, recorder_started_cb, data);
}

static gboolean
handle_screencast (ShellDBusScreencast    *skeleton,
                   GDBusMethodInvocation  *invocation,
                   const char             *file_template,
                   GVariant               *options,
                   ScreencastService      *self)
{
  start_screencast (self, invocation, FALSE, 0, 0, 0, 0, file_template, options);
  return TRUE;
}

static gboolean
handle_screencast_area (ShellDBusScreencast    *skeleton,
                        GDBusMethodInvocation  *invocation,
                        int                     x,
                        int                     y,
                        int                     width,
                        int                     height,
                        const char             *file_template,
                        GVariant               *options,
                        ScreencastService      *self)
{
  start_screencast (self, invocation, TRUE, x, y, width, height,
                    file_template, options);
  return TRUE;
}

typedef struct
{
  ScreencastService *service;
  GDBusMethodInvocation *invocation;
  ScreencastRecorder *recorder;
  char *sender;
} StopData;

static void
stop_data_free (StopData *data)
{
  g_object_unref (data->service);
  g_object_unref (data->recorder);
  g_free (data->sender);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (StopData, stop_data_free)

static void
recorder_stopped_cb (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  g_autoptr (StopData) data = user_data;
  ScreencastService *self = data->service;
  g_autoptr (GError) error = NULL;

  if (!screencast_recorder_stop_finish (data->recorder, result, &error))
    g_warning ("%s: Error while stopping recorder: %s", data->sender,
               error->message);

  remove_recorder (self, data->sender);

  shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (self),
                                       data->invocation);
  shell_dbus_screencast_complete_stop_screencast (self->skeleton,
                                                  data->invocation, TRUE);
}

static gboolean
handle_stop_screencast (ShellDBusScreencast    *skeleton,
                        GDBusMethodInvocation  *invocation,
                        ScreencastService      *self)
{
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  ScreencastRecorder *recorder = g_hash_table_lookup (self->recorders, sender);
  StopData *data;

  if (recorder == NULL)
    {
      shell_dbus_service_track_invocation (SHELL_DBUS_SERVICE (self),
                                           invocation);
      shell_dbus_screencast_complete_stop_screencast (skeleton, invocation,
                                                      FALSE);
      return TRUE;
    }

  data = g_new0 (StopData, 1);
  data->service = g_object_ref (self);
  data->invocation = invocation;
  data->recorder = g_object_ref (recorder);
  data->sender = g_strdup (sender);

  screencast_recorder_stop_async (recorder, recorder_stopped_cb, data);
  return TRUE;
}

static void
screencast_service_constructed (GObject *object)
{
  ScreencastService *self = SCREENCAST_SERVICE (object);
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = NULL;

  G_OBJECT_CLASS (screencast_service_parent_class)->constructed (object);

  g_set_object (&self->skeleton,
                SHELL_DBUS_SCREENCAST (shell_dbus_service_get_skeleton (SHELL_DBUS_SERVICE (self))));

  shell_dbus_service_hold (SHELL_DBUS_SERVICE (self)); /* gstreamer initializing can take a bit */
  shell_dbus_screencast_set_screencast_supported (self->skeleton, can_screencast ());

  gst_init (NULL, NULL);
  if (!gtk_init_check ())
    g_message ("Failed to initialize GTK, the recordings will not be added to the recent files");

  shell_dbus_service_release (SHELL_DBUS_SERVICE (self));

  if (source != NULL)
    schema = g_settings_schema_source_lookup (source,
                                              "org.gnome.desktop.lockdown",
                                              TRUE);
  if (schema != NULL)
    self->lockdown_settings = g_settings_new_full (schema, NULL, NULL);
  else
    g_message ("The schema org.gnome.desktop.lockdown is not installed");

  g_signal_connect (self->skeleton, "handle-screencast",
                    G_CALLBACK (handle_screencast), self);
  g_signal_connect (self->skeleton, "handle-screencast-area",
                    G_CALLBACK (handle_screencast_area), self);
  g_signal_connect (self->skeleton, "handle-stop-screencast",
                    G_CALLBACK (handle_stop_screencast), self);
}

static void
screencast_service_dispose (GObject *object)
{
  ScreencastService *self = SCREENCAST_SERVICE (object);

  g_clear_pointer (&self->recorders, g_hash_table_unref);
  g_clear_object (&self->lockdown_settings);
  g_clear_object (&self->proxy);
  g_clear_object (&self->introspect_proxy);
  g_clear_object (&self->skeleton);

  G_OBJECT_CLASS (screencast_service_parent_class)->dispose (object);
}

static void
screencast_service_class_init (ScreencastServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = screencast_service_constructed;
  object_class->dispose = screencast_service_dispose;
}

static void
screencast_service_init (ScreencastService *self)
{
  self->recorders = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free, g_object_unref);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (ScreencastService) service = NULL;
  g_autoptr (ShellDBusScreencast) skeleton = NULL;

  if (!can_screencast ())
    return 0;

  skeleton = shell_dbus_screencast_skeleton_new ();
  service = g_object_new (SCREENCAST_TYPE_SERVICE,
                          "skeleton", skeleton,
                          "object-path", SCREENCAST_OBJECT_PATH,
                          NULL);

  return shell_dbus_service_run (SHELL_DBUS_SERVICE (service),
                                 SCREENCAST_BUS_NAME, argc, argv);
}
