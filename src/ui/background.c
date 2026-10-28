/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * background.c: The desktop background of a monitor. Port of
 * js/ui/background.js.
 *
 * Background handling is a maze of objects, both objects in this file, and
 * also objects inside Mutter. They all have a role.
 *
 * ShellBackgroundManager
 *   The only object that other parts of GNOME Shell deal with; a
 *   manager creates background actors and adds them to the specified
 *   container. When the background is changed by the user it will fade out
 *   the old actor and fade in the new actor. (This is separate from the
 *   fading for an animated background, since using two actors is quite
 *   inefficient.)
 *
 * texture cache (in ShellBackgroundCache)
 *   Shell-side cache from filename to CoglTexture. Handles image loading
 *   using glycin and creates the textures.
 *
 * BackgroundSource
 *   An object that is created for each GSettings schema (separate settings
 *   schemas are used for the lock screen and main background), and holds a
 *   reference to shared ShellBackground objects.
 *
 * MetaBackground
 *   Holds the specification of a background - a background color or
 *   gradient and one or two textures blended together.
 *
 * ShellBackground
 *   Delegate object that connects a MetaBackground to the GSettings schema
 *   for the background. Loads images via the texture cache and provides
 *   textures to the MetaBackground. (MetaBackground is a final type, so
 *   unlike the JS class this one owns the MetaBackground instead of
 *   deriving from it.)
 *
 * Animation
 *   A helper that handles loading a XML-based animation; it is a wrapper for
 *   GnomeBGSlideShow.
 *
 * MetaBackgroundActor
 *   An actor that draws the background for a single monitor.
 *
 * ShellBackgroundCache
 *   A cache of Settings schema => BackgroundSource and of a single Animation
 *   per schema. Also used to share file monitors.
 *
 * A static image, background color or gradient is relatively
 * straightforward. The calling code creates a separate manager for each
 * monitor. Since they are created for the same GSettings schema, they will
 * use the same BackgroundSource object, which provides a single
 * ShellBackground and correspondingly a single MetaBackground object.
 *
 * The animated case is trickier because the animation XML file can specify
 * different files for different monitor resolutions and aspect ratios. For
 * this reason, the BackgroundSource provides different ShellBackground
 * objects that share a single Animation object, which tracks the animation,
 * but use different MetaBackground objects. In the common case, the
 * different MetaBackground objects will be created for the same filename and
 * look up the *same* CoglTexture object, so there is little wasted memory.
 */

#include "config.h"

#define GNOME_DESKTOP_USE_UNSTABLE_API

#include <math.h>

#include <gdesktop-enums.h>
#include <glycin.h>
#include <gnome-bg/gnome-bg-slide-show.h>
#include <libgnome-desktop/gnome-wall-clock.h>
#include <meta/display.h>
#include <meta/meta-backend.h>
#include <meta/meta-background-actor.h>
#include <meta/meta-background-content.h>
#include <meta/meta-background.h>
#include <meta/meta-monitor-manager.h>

#include "misc/login-manager.h"
#include "shell-global.h"
#include "ui/background.h"
#include "ui/environment.h"

#define BACKGROUND_SCHEMA "org.gnome.desktop.background"
#define PRIMARY_COLOR_KEY "primary-color"
#define SECONDARY_COLOR_KEY "secondary-color"
#define COLOR_SHADING_TYPE_KEY "color-shading-type"
#define BACKGROUND_STYLE_KEY "picture-options"
#define PICTURE_URI_KEY "picture-uri"
#define PICTURE_URI_DARK_KEY "picture-uri-dark"

#define INTERFACE_SCHEMA "org.gnome.desktop.interface"
#define COLOR_SCHEME_KEY "color-scheme"

#define FADE_ANIMATION_TIME 1000

/* These parameters affect how often we redraw.
 * The first is how different (percent crossfaded) the slide show
 * has to look before redrawing and the second is the minimum
 * frequency (in seconds) we're willing to wake up */
#define ANIMATION_OPACITY_STEP_INCREMENT 4.0
#define ANIMATION_MIN_WAKEUP_INTERVAL 1.0

#define CACHE_DATA_KEY "shell-background-cache"

#define SHELL_TYPE_BACKGROUND_CACHE (shell_background_cache_get_type ())
G_DECLARE_FINAL_TYPE (ShellBackgroundCache, shell_background_cache,
                      SHELL, BACKGROUND_CACHE, GObject)

#define SHELL_TYPE_BACKGROUND (shell_background_get_type ())
G_DECLARE_FINAL_TYPE (ShellBackground, shell_background,
                      SHELL, BACKGROUND, GObject)

typedef struct _BackgroundSource BackgroundSource;

static ShellBackgroundCache *get_background_cache (void);

static ShellBackground *shell_background_new                (int               monitor_index,
                                                             GSettings        *settings,
                                                             GFile            *file,
                                                             GDesktopBackgroundStyle style);
static void             shell_background_destroy            (ShellBackground  *self);
static void             shell_background_update_resolution  (ShellBackground  *self);
static gboolean         shell_background_is_loaded          (ShellBackground  *self);
static MetaBackground  *shell_background_get_meta_background (ShellBackground *self);

static BackgroundSource *background_source_new     (const char       *schema);
static void              background_source_destroy (BackgroundSource *source);
static ShellBackground  *background_source_get_background (BackgroundSource *source,
                                                           int               monitor_index);

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

/* GLib.idle_add_once(GLib.PRIORITY_DEFAULT, ...): g_idle_add_once() would
 * run at G_PRIORITY_DEFAULT_IDLE instead. */
typedef struct
{
  GSourceOnceFunc func;
  gpointer data;
} IdleOnce;

static gboolean
idle_once_cb (gpointer user_data)
{
  IdleOnce *once = user_data;

  once->func (once->data);

  return G_SOURCE_REMOVE;
}

static guint
idle_add_default (GSourceOnceFunc func,
                  gpointer        data)
{
  IdleOnce *once = g_new (IdleOnce, 1);

  once->func = func;
  once->data = data;

  return g_idle_add_full (G_PRIORITY_DEFAULT, idle_once_cb, once, g_free);
}

/* js/ui/background.js read the geometry from Main.layoutManager.monitors[],
 * which is a copy of the display's monitor geometry. Like the other ports
 * the monitor index is clamped to the existing monitors. */
static void
get_monitor_geometry (int           monitor_index,
                      MtkRectangle *geometry)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  int n_monitors = meta_display_get_n_monitors (display);

  *geometry = (MtkRectangle) { 0, };

  if (n_monitors > 0)
    {
      meta_display_get_monitor_geometry (display,
                                         MIN (monitor_index, n_monitors - 1),
                                         geometry);
    }
}

static gboolean
file_equal0 (GFile *file1,
             GFile *file2)
{
  if (file1 == file2)
    return TRUE;

  if (file1 == NULL || file2 == NULL)
    return FALSE;

  return g_file_equal (file1, file2);
}

/* ------------------------------------------------------------------------ */
/* Animation: a GnomeBGSlideShow with the state the background needs         */
/* ------------------------------------------------------------------------ */

typedef struct
{
  GnomeBGSlideShow *slide_show;
  GFile *file;
  GPtrArray *key_frame_files; /* (element-type GFile) */
  double transition_progress;
  double transition_duration;
  gboolean loaded;
} Animation;

typedef void (* AnimationLoadedFunc) (Animation *animation,
                                      gpointer   user_data);

static void
animation_clear (gpointer data)
{
  Animation *animation = data;

  g_clear_object (&animation->slide_show);
  g_clear_object (&animation->file);
  g_clear_pointer (&animation->key_frame_files, g_ptr_array_unref);
}

static Animation *
animation_new (GFile *file)
{
  Animation *animation = g_rc_box_new0 (Animation);

  animation->file = g_object_ref (file);
  animation->slide_show = g_object_new (GNOME_BG_TYPE_SLIDE_SHOW,
                                        "file", file,
                                        NULL);
  animation->key_frame_files = g_ptr_array_new_with_free_func (g_object_unref);

  return animation;
}

static Animation *
animation_ref (Animation *animation)
{
  return g_rc_box_acquire (animation);
}

static void
animation_unref (Animation *animation)
{
  g_rc_box_release_full (animation, animation_clear);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Animation, animation_unref)

static void
animation_update (Animation          *animation,
                  const MtkRectangle *monitor)
{
  double progress, duration;
  gboolean is_fixed;
  const char *filename1 = NULL;
  const char *filename2 = NULL;

  /* A new array every time: callers keep the one they got */
  g_clear_pointer (&animation->key_frame_files, g_ptr_array_unref);
  animation->key_frame_files = g_ptr_array_new_with_free_func (g_object_unref);

  if (gnome_bg_slide_show_get_num_slides (animation->slide_show) < 1)
    return;

  gnome_bg_slide_show_get_current_slide (animation->slide_show,
                                         monitor->width, monitor->height,
                                         &progress, &duration, &is_fixed,
                                         &filename1, &filename2);

  animation->transition_duration = duration;
  animation->transition_progress = progress;

  if (filename1 != NULL && *filename1 != '\0')
    g_ptr_array_add (animation->key_frame_files, g_file_new_for_path (filename1));

  if (filename2 != NULL && *filename2 != '\0')
    g_ptr_array_add (animation->key_frame_files, g_file_new_for_path (filename2));
}

/* ------------------------------------------------------------------------ */
/* Texture cache entries                                                     */
/* ------------------------------------------------------------------------ */

typedef struct
{
  CoglTexture *texture;
  ClutterColorState *color_state;
} TextureEntry;

static void
texture_entry_clear (gpointer data)
{
  TextureEntry *entry = data;

  g_clear_object (&entry->texture);
  g_clear_object (&entry->color_state);
}

static TextureEntry *
texture_entry_ref (TextureEntry *entry)
{
  return g_rc_box_acquire (entry);
}

static void
texture_entry_unref (TextureEntry *entry)
{
  g_rc_box_release_full (entry, texture_entry_clear);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (TextureEntry, texture_entry_unref)

static gboolean
glycin_format_to_cogl (GlyMemoryFormat  format,
                       CoglPixelFormat *cogl_format)
{
  switch (format)
    {
    case GLY_MEMORY_B8G8R8A8_PREMULTIPLIED:
      *cogl_format = COGL_PIXEL_FORMAT_BGRA_8888_PRE;
      return TRUE;
    case GLY_MEMORY_A8R8G8B8_PREMULTIPLIED:
      *cogl_format = COGL_PIXEL_FORMAT_ARGB_8888_PRE;
      return TRUE;
    case GLY_MEMORY_R8G8B8A8_PREMULTIPLIED:
      *cogl_format = COGL_PIXEL_FORMAT_RGBA_8888_PRE;
      return TRUE;
    case GLY_MEMORY_B8G8R8A8:
      *cogl_format = COGL_PIXEL_FORMAT_BGRA_8888;
      return TRUE;
    case GLY_MEMORY_A8R8G8B8:
      *cogl_format = COGL_PIXEL_FORMAT_ARGB_8888;
      return TRUE;
    case GLY_MEMORY_R8G8B8A8:
      *cogl_format = COGL_PIXEL_FORMAT_RGBA_8888;
      return TRUE;
    case GLY_MEMORY_A8B8G8R8:
      *cogl_format = COGL_PIXEL_FORMAT_ABGR_8888;
      return TRUE;
    case GLY_MEMORY_R8G8B8:
      *cogl_format = COGL_PIXEL_FORMAT_RGB_888;
      return TRUE;
    case GLY_MEMORY_B8G8R8:
      *cogl_format = COGL_PIXEL_FORMAT_BGR_888;
      return TRUE;
    case GLY_MEMORY_R16G16B16A16_PREMULTIPLIED:
      *cogl_format = COGL_PIXEL_FORMAT_RGBA_16161616_PRE;
      return TRUE;
    case GLY_MEMORY_R16G16B16A16:
      *cogl_format = COGL_PIXEL_FORMAT_RGBA_16161616;
      return TRUE;
    case GLY_MEMORY_R16G16B16A16_FLOAT:
      *cogl_format = COGL_PIXEL_FORMAT_RGBA_FP_16161616;
      return TRUE;
    case GLY_MEMORY_R32G32B32A32_FLOAT_PREMULTIPLIED:
      *cogl_format = COGL_PIXEL_FORMAT_RGBA_FP_32323232_PRE;
      return TRUE;
    case GLY_MEMORY_R32G32B32A32_FLOAT:
      *cogl_format = COGL_PIXEL_FORMAT_RGBA_FP_32323232;
      return TRUE;
    default:
      return FALSE;
    }
}

static CoglTexture *
create_texture (GlyFrame  *frame,
                GError   **error)
{
  GlyMemoryFormat format = gly_frame_get_memory_format (frame);
  ClutterActor *stage = CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ()));
  ClutterContext *clutter_context = clutter_actor_get_context (stage);
  ClutterBackend *clutter_backend = clutter_context_get_backend (clutter_context);
  CoglContext *cogl_context = clutter_backend_get_cogl_context (clutter_backend);
  g_autoptr (CoglTexture) texture = NULL;
  g_autoptr (GError) allocate_error = NULL;
  CoglTextureComponents components;
  CoglPixelFormat cogl_format;
  uint32_t width = gly_frame_get_width (frame);
  uint32_t height = gly_frame_get_height (frame);
  uint32_t stride = gly_frame_get_stride (frame);
  GBytes *bytes = gly_frame_get_buf_bytes (frame);
  const uint8_t *data = g_bytes_get_data (bytes, NULL);

  if (!glycin_format_to_cogl (format, &cogl_format))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unsupported glycin memory format: %d", format);
      return NULL;
    }

  components = gly_memory_format_has_alpha (format)
    ? COGL_TEXTURE_COMPONENTS_RGBA
    : COGL_TEXTURE_COMPONENTS_RGB;

  texture = cogl_texture_2d_new_with_size (cogl_context, width, height);
  cogl_texture_set_components (texture, components);

  /* Try to allocate, if it fails (texture too large), use sliced */
  if (!cogl_texture_allocate (texture, &allocate_error))
    {
      g_clear_object (&texture);

      texture = cogl_texture_2d_sliced_new_with_size (cogl_context,
                                                      width, height,
                                                      COGL_TEXTURE_MAX_WASTE);
      cogl_texture_set_components (texture, components);
    }

  if (!cogl_texture_set_data (texture, cogl_format, stride, data, 0, error))
    {
      if (error != NULL && *error == NULL)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Failed to set texture data");
      return NULL;
    }

  return g_steal_pointer (&texture);
}

static ClutterColorState *
create_color_state (GlyFrame *frame)
{
  g_autoptr (GlyCicp) cicp = gly_frame_get_color_cicp (frame);
  ClutterActor *stage = CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ()));
  g_autoptr (GError) error = NULL;
  ClutterColorState *color_state;
  ClutterCicp clutter_cicp;

  if (cicp == NULL)
    return NULL;

  clutter_cicp = (ClutterCicp) {
    .primaries = (ClutterCicpPrimaries) cicp->color_primaries,
    .transfer = (ClutterCicpTransfer) cicp->transfer_characteristics,
    .matrix_coefficients = cicp->matrix_coefficients,
    .video_full_range_flag = cicp->video_full_range_flag,
  };

  color_state =
    clutter_color_state_params_new_from_cicp (clutter_actor_get_context (stage),
                                              &clutter_cicp, &error);
  if (color_state == NULL)
    g_warning ("Failed to create color state from CICP: %s", error->message);

  return color_state;
}

/* ------------------------------------------------------------------------ */
/* ShellBackgroundCache                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Private. Replaces the BackgroundCache and BackgroundTextureCache of the JS
 * file.
 */

enum
{
  CACHE_FILE_CHANGED,
  N_CACHE_SIGNALS
};

static guint cache_signals[N_CACHE_SIGNALS] = { 0 };

struct _ShellBackgroundCache
{
  GObject parent;

  GHashTable *file_monitors;      /* guint (GFile hash) -> GFileMonitor */
  GHashTable *background_sources; /* char * schema -> BackgroundSource */
  GHashTable *animations;         /* char * schema -> Animation */
  GHashTable *textures;           /* char * uri -> TextureEntry */

  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellBackgroundCache, shell_background_cache, G_TYPE_OBJECT)

static void
on_file_monitor_changed (GFileMonitor      *monitor,
                         GFile             *file,
                         GFile             *other_file,
                         GFileMonitorEvent  event_type,
                         gpointer           user_data)
{
  ShellBackgroundCache *self = user_data;
  GFile *watched_file = g_object_get_data (G_OBJECT (monitor), "shell-background-file");

  /* Ignore CHANGED and CREATED events, since in both cases
   * we'll get a CHANGES_DONE_HINT event when done. */
  if (event_type != G_FILE_MONITOR_EVENT_CHANGED &&
      event_type != G_FILE_MONITOR_EVENT_CREATED)
    g_signal_emit (self, cache_signals[CACHE_FILE_CHANGED], 0, watched_file);
}

static void
cache_monitor_file (ShellBackgroundCache *self,
                    GFile                *file)
{
  g_autoptr (GFileMonitor) monitor = NULL;
  g_autoptr (GError) error = NULL;
  gpointer key = GUINT_TO_POINTER (g_file_hash (file));

  if (g_hash_table_contains (self->file_monitors, key))
    return;

  monitor = g_file_monitor (file, G_FILE_MONITOR_NONE, NULL, &error);
  if (monitor == NULL)
    {
      g_warning ("Failed to monitor background file: %s", error->message);
      return;
    }

  /* The monitored file, which is what 'file-changed' is emitted with */
  g_object_set_data_full (G_OBJECT (monitor), "shell-background-file",
                          g_object_ref (file), g_object_unref);
  g_signal_connect (monitor, "changed",
                    G_CALLBACK (on_file_monitor_changed), self);

  g_hash_table_insert (self->file_monitors, key, g_steal_pointer (&monitor));
}

/* getAnimation() */

typedef struct
{
  ShellBackgroundCache *cache;
  char *schema;
  AnimationLoadedFunc on_loaded;
  gpointer user_data;
  GDestroyNotify destroy;
} AnimationRequest;

static void
animation_request_free (AnimationRequest *request)
{
  if (request->destroy != NULL)
    request->destroy (request->user_data);

  g_clear_object (&request->cache);
  g_free (request->schema);
  g_free (request);
}

static void
animation_loaded_idle_cb (gpointer data)
{
  AnimationRequest *request = data;

  if (!request->cache->shutting_down)
    {
      /* The animation of the schema at this time, which can be a newer one
       * than the one this request loaded */
      Animation *animation = g_hash_table_lookup (request->cache->animations,
                                                  request->schema);

      request->on_loaded (animation, request->user_data);
    }

  animation_request_free (request);
}

static void
queue_animation_loaded (AnimationRequest *request)
{
  guint id;

  if (request->on_loaded == NULL)
    {
      animation_request_free (request);
      return;
    }

  id = idle_add_default (animation_loaded_idle_cb, request);
  g_source_set_name_by_id (id, "[gnome-shell] params.onLoaded");
}

typedef struct
{
  AnimationRequest *request;
  Animation *animation;
} AnimationLoad;

static void
on_slide_show_loaded (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  AnimationLoad *load = user_data;
  AnimationRequest *request = load->request;
  ShellBackgroundCache *cache = request->cache;
  g_autoptr (GError) error = NULL;

  load->animation->loaded = TRUE;

  /* The JS ignored the outcome: a slide show that failed to load has no
   * slides, so it just does not produce key frames */
  if (!gnome_bg_slide_show_load_finish (GNOME_BG_SLIDE_SHOW (source),
                                        result, &error))
    g_debug ("Failed to load background animation: %s", error->message);

  if (cache->shutting_down)
    {
      animation_request_free (request);
    }
  else
    {
      g_hash_table_replace (cache->animations,
                            g_strdup (request->schema),
                            animation_ref (load->animation));

      queue_animation_loaded (request);
    }

  animation_unref (load->animation);
  g_free (load);
}

static void
cache_get_animation (ShellBackgroundCache *self,
                     GFile                *file,
                     const char           *schema,
                     AnimationLoadedFunc   on_loaded,
                     gpointer              user_data,
                     GDestroyNotify        destroy)
{
  AnimationRequest *request;
  AnimationLoad *load;
  Animation *animation;

  request = g_new0 (AnimationRequest, 1);
  request->cache = g_object_ref (self);
  request->schema = g_strdup (schema);
  request->on_loaded = on_loaded;
  request->user_data = user_data;
  request->destroy = destroy;

  animation = g_hash_table_lookup (self->animations, schema);
  if (animation != NULL && file_equal0 (animation->file, file))
    {
      queue_animation_loaded (request);
      return;
    }

  load = g_new0 (AnimationLoad, 1);
  load->request = request;
  load->animation = animation_new (file);

  gnome_bg_slide_show_load_async (load->animation->slide_show, NULL,
                                  on_slide_show_loaded, load);
}

/* getBackgroundSource() / releaseBackgroundSource() */

struct _BackgroundSource
{
  char *schema;
  GSettings *settings;
  GSettings *interface_settings;
  char *override_image;
  GHashTable *backgrounds; /* int monitor -> BackgroundEntry */
  gulong monitors_changed_id;
  int use_count;
};

static BackgroundSource *
cache_get_background_source (ShellBackgroundCache *self,
                             const char           *schema)
{
  BackgroundSource *source = g_hash_table_lookup (self->background_sources, schema);

  if (source == NULL)
    {
      source = background_source_new (schema);
      source->use_count = 1;
      g_hash_table_insert (self->background_sources, g_strdup (schema), source);
    }
  else
    {
      source->use_count++;
    }

  return source;
}

static void
cache_release_background_source (ShellBackgroundCache *self,
                                 const char           *schema)
{
  BackgroundSource *source = g_hash_table_lookup (self->background_sources, schema);

  if (source == NULL)
    return;

  source->use_count--;
  if (source->use_count == 0)
    {
      /* Removes it from the table and destroys it */
      g_hash_table_remove (self->background_sources, schema);
    }
}

/* BackgroundTextureCache */

typedef struct
{
  GFile *file;
  GlyLoader *loader;
  GlyImage *image;
} TextureLoad;

static void
texture_load_free (gpointer data)
{
  TextureLoad *load = data;

  g_clear_object (&load->file);
  g_clear_object (&load->loader);
  g_clear_object (&load->image);
  g_free (load);
}

static void
on_texture_next_frame (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  ShellBackgroundCache *cache = g_task_get_source_object (task);
  TextureLoad *load = g_task_get_task_data (task);
  g_autoptr (GlyFrame) frame = NULL;
  g_autoptr (CoglTexture) texture = NULL;
  g_autoptr (TextureEntry) entry = NULL;
  g_autoptr (GError) error = NULL;

  frame = gly_image_next_frame_finish (GLY_IMAGE (source), result, &error);
  if (frame == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  texture = create_texture (frame, &error);
  if (texture == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  entry = g_rc_box_new0 (TextureEntry);
  entry->texture = g_steal_pointer (&texture);
  entry->color_state = create_color_state (frame);

  g_hash_table_replace (cache->textures,
                        g_file_get_uri (load->file),
                        texture_entry_ref (entry));

  g_task_return_pointer (task, g_steal_pointer (&entry),
                         (GDestroyNotify) texture_entry_unref);
}

static void
on_texture_loader_load (GObject      *source,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  GTask *task = user_data;
  TextureLoad *load = g_task_get_task_data (task);
  g_autoptr (GError) error = NULL;

  load->image = gly_loader_load_finish (GLY_LOADER (source), result, &error);
  if (load->image == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (task);
      return;
    }

  gly_image_next_frame_async (load->image, g_task_get_cancellable (task),
                              on_texture_next_frame, task);
}

static void
on_texture_file_read (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  GTask *task = user_data;
  TextureLoad *load = g_task_get_task_data (task);
  g_autoptr (GFileInputStream) stream = NULL;
  g_autoptr (GError) error = NULL;

  stream = g_file_read_finish (G_FILE (source), result, &error);
  if (stream == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      g_object_unref (task);
      return;
    }

  load->loader = gly_loader_new_for_stream (G_INPUT_STREAM (stream));

  gly_loader_set_accepted_memory_formats (load->loader,
    (GlyMemoryFormatSelection) (GLY_MEMORY_SELECTION_B8G8R8A8_PREMULTIPLIED |
                                GLY_MEMORY_SELECTION_A8R8G8B8_PREMULTIPLIED |
                                GLY_MEMORY_SELECTION_R8G8B8A8_PREMULTIPLIED |
                                GLY_MEMORY_SELECTION_B8G8R8A8 |
                                GLY_MEMORY_SELECTION_A8R8G8B8 |
                                GLY_MEMORY_SELECTION_R8G8B8A8 |
                                GLY_MEMORY_SELECTION_A8B8G8R8 |
                                GLY_MEMORY_SELECTION_R8G8B8 |
                                GLY_MEMORY_SELECTION_B8G8R8 |
                                GLY_MEMORY_SELECTION_R16G16B16A16_PREMULTIPLIED |
                                GLY_MEMORY_SELECTION_R16G16B16A16 |
                                GLY_MEMORY_SELECTION_R16G16B16A16_FLOAT |
                                GLY_MEMORY_SELECTION_R32G32B32A32_FLOAT_PREMULTIPLIED |
                                GLY_MEMORY_SELECTION_R32G32B32A32_FLOAT));

  gly_loader_load_async (load->loader, g_task_get_cancellable (task),
                         on_texture_loader_load, task);
}

static void
texture_cache_load_async (ShellBackgroundCache *self,
                          GFile                *file,
                          GCancellable         *cancellable,
                          GAsyncReadyCallback   callback,
                          gpointer              user_data)
{
  g_autoptr (GTask) task = g_task_new (self, cancellable, callback, user_data);
  g_autofree char *uri = g_file_get_uri (file);
  TextureEntry *entry;
  TextureLoad *load;

  g_task_set_source_tag (task, texture_cache_load_async);
  /* The JS applied the result of a load even when it was cancelled meanwhile */
  g_task_set_check_cancellable (task, FALSE);

  entry = g_hash_table_lookup (self->textures, uri);
  if (entry != NULL)
    {
      g_task_return_pointer (task, texture_entry_ref (entry),
                             (GDestroyNotify) texture_entry_unref);
      return;
    }

  load = g_new0 (TextureLoad, 1);
  load->file = g_object_ref (file);
  g_task_set_task_data (task, load, texture_load_free);

  g_file_read_async (file, G_PRIORITY_DEFAULT, cancellable,
                     on_texture_file_read, g_steal_pointer (&task));
}

/* Returns: (transfer full): a reference to the cached entry */
static TextureEntry *
texture_cache_load_finish (ShellBackgroundCache  *self,
                           GAsyncResult          *result,
                           GError               **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
texture_cache_purge (ShellBackgroundCache *self,
                     GFile                *file)
{
  g_autofree char *uri = g_file_get_uri (file);

  g_hash_table_remove (self->textures, uri);
}

static void
on_global_shutdown (ShellGlobal          *global,
                    ShellBackgroundCache *self)
{
  /* Nothing may complete into the (about to be destroyed) UI from now on */
  self->shutting_down = TRUE;
}

static void
shell_background_cache_constructed (GObject *object)
{
  ShellBackgroundCache *self = SHELL_BACKGROUND_CACHE (object);

  G_OBJECT_CLASS (shell_background_cache_parent_class)->constructed (object);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_global_shutdown), self, 0);
}

static void
shell_background_cache_dispose (GObject *object)
{
  ShellBackgroundCache *self = SHELL_BACKGROUND_CACHE (object);

  g_clear_pointer (&self->background_sources, g_hash_table_unref);
  g_clear_pointer (&self->file_monitors, g_hash_table_unref);
  g_clear_pointer (&self->animations, g_hash_table_unref);
  g_clear_pointer (&self->textures, g_hash_table_unref);

  G_OBJECT_CLASS (shell_background_cache_parent_class)->dispose (object);
}

static void
shell_background_cache_class_init (ShellBackgroundCacheClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_background_cache_constructed;
  object_class->dispose = shell_background_cache_dispose;

  cache_signals[CACHE_FILE_CHANGED] =
    g_signal_new ("file-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1, G_TYPE_FILE);
}

static void
shell_background_cache_init (ShellBackgroundCache *self)
{
  self->file_monitors = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                               NULL, g_object_unref);
  self->background_sources = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free,
                                                    (GDestroyNotify) background_source_destroy);
  self->animations = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free,
                                            (GDestroyNotify) animation_unref);
  self->textures = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free,
                                          (GDestroyNotify) texture_entry_unref);
}

/* The module-level _backgroundCache of the JS; tied to the lifetime of
 * ShellGlobal instead of being a static. */
static ShellBackgroundCache *
get_background_cache (void)
{
  GObject *global = G_OBJECT (shell_global_get ());
  ShellBackgroundCache *cache = g_object_get_data (global, CACHE_DATA_KEY);

  if (cache == NULL)
    {
      cache = g_object_new (SHELL_TYPE_BACKGROUND_CACHE, NULL);
      g_object_set_data_full (global, CACHE_DATA_KEY, cache, g_object_unref);
    }

  return cache;
}

/* ------------------------------------------------------------------------ */
/* ShellBackground                                                           */
/* ------------------------------------------------------------------------ */

/*
 * Private. The JS Background class derived from Meta.Background, which is a
 * final type in C, so this one owns a MetaBackground (see
 * shell_background_get_meta_background()).
 *
 * Signals:
 *  - loaded: the background finished loading (emitted from an idle)
 *  - bg-changed: the settings or the file changed (emitted from an idle)
 */

enum
{
  BACKGROUND_LOADED,
  BACKGROUND_CHANGED,
  N_BACKGROUND_SIGNALS
};

static guint background_signals[N_BACKGROUND_SIGNALS] = { 0 };

struct _ShellBackground
{
  GObject parent;

  MetaBackground *meta_background;

  GSettings *settings;
  GSettings *interface_settings;
  GFile *file;
  GDesktopBackgroundStyle style;
  int monitor_index;

  ShellBackgroundCache *cache;
  GCancellable *cancellable;
  GnomeWallClock *clock;
  Animation *animation;
  GHashTable *file_watches; /* guint (GFile hash) -> handler id on the cache */

  gulong clock_handler_id;
  gulong sleep_handler_id;
  gulong settings_handler_id;
  gulong interface_handler_id;
  gulong shutdown_handler_id;

  guint update_animation_timeout_id;
  guint changed_idle_id;
  guint loaded_idle_id;

  gboolean is_loaded;
  gboolean destroyed;
};

G_DEFINE_FINAL_TYPE (ShellBackground, shell_background, G_TYPE_OBJECT)

static void update_animation (ShellBackground *self);

static void
changed_idle_cb (gpointer data)
{
  g_autoptr (ShellBackground) self = g_object_ref (data);

  self->changed_idle_id = 0;
  g_signal_emit (self, background_signals[BACKGROUND_CHANGED], 0);
}

static void
emit_changed_signal (ShellBackground *self)
{
  if (self->changed_idle_id != 0)
    return;

  self->changed_idle_id = idle_add_default (changed_idle_cb, self);
  g_source_set_name_by_id (self->changed_idle_id,
                           "[gnome-shell] Background._emitChangedSignal");
}

static void
background_loaded_idle_cb (gpointer data)
{
  g_autoptr (ShellBackground) self = g_object_ref (data);

  self->loaded_idle_id = 0;
  g_signal_emit (self, background_signals[BACKGROUND_LOADED], 0);
}

static void
set_loaded (ShellBackground *self)
{
  if (self->is_loaded)
    return;

  self->is_loaded = TRUE;
  if (g_cancellable_is_cancelled (self->cancellable))
    return;

  self->loaded_idle_id = idle_add_default (background_loaded_idle_cb, self);
  g_source_set_name_by_id (self->loaded_idle_id,
                           "[gnome-shell] Background._setLoaded Idle");
}

static void
remove_animation_timeout (ShellBackground *self)
{
  g_clear_handle_id (&self->update_animation_timeout_id, g_source_remove);
}

static void
refresh_animation (ShellBackground *self)
{
  if (self->animation == NULL)
    return;

  remove_animation_timeout (self);
  update_animation (self);
}

static void
load_pattern (ShellBackground *self)
{
  g_autofree char *color_string = NULL;
  g_autofree char *second_color_string = NULL;
  CoglColor color = { 0, };
  CoglColor second_color = { 0, };
  GDesktopBackgroundShading shading_type;

  color_string = g_settings_get_string (self->settings, PRIMARY_COLOR_KEY);
  cogl_color_from_string (&color, color_string);
  second_color_string = g_settings_get_string (self->settings, SECONDARY_COLOR_KEY);
  cogl_color_from_string (&second_color, second_color_string);

  shading_type = g_settings_get_enum (self->settings, COLOR_SHADING_TYPE_KEY);

  if (shading_type == G_DESKTOP_BACKGROUND_SHADING_SOLID)
    meta_background_set_color (self->meta_background, &color);
  else
    meta_background_set_gradient (self->meta_background, shading_type,
                                  &color, &second_color);
}

/* _watchFile() */

typedef struct
{
  ShellBackground *background;
  GFile *file;
} FileWatch;

static void
file_watch_free (FileWatch *watch,
                 GClosure  *closure)
{
  g_clear_object (&watch->file);
  g_free (watch);
}

static void
on_cache_file_changed (ShellBackgroundCache *cache,
                       GFile                *changed_file,
                       FileWatch            *watch)
{
  if (g_file_equal (changed_file, watch->file))
    {
      texture_cache_purge (cache, changed_file);
      emit_changed_signal (watch->background);
    }
}

static void
watch_file (ShellBackground *self,
            GFile           *file)
{
  FileWatch *watch;
  gpointer key;
  gulong signal_id;

  /* The JS dropped _fileWatches in destroy() (and threw from here) */
  if (self->destroyed)
    return;

  key = GUINT_TO_POINTER (g_file_hash (file));
  if (g_hash_table_contains (self->file_watches, key))
    return;

  cache_monitor_file (self->cache, file);

  watch = g_new0 (FileWatch, 1);
  watch->background = self;
  watch->file = g_object_ref (file);
  signal_id = g_signal_connect_data (self->cache, "file-changed",
                                     G_CALLBACK (on_cache_file_changed),
                                     watch,
                                     (GClosureNotify) file_watch_free, 0);

  g_hash_table_insert (self->file_watches, key, GSIZE_TO_POINTER (signal_id));
}

/* _queueUpdateAnimation() */

static void
update_animation_timeout_cb (gpointer data)
{
  ShellBackground *self = data;

  self->update_animation_timeout_id = 0;
  update_animation (self);
}

static void
queue_update_animation (ShellBackground *self)
{
  double n_steps, time_per_step, interval;

  if (self->update_animation_timeout_id != 0)
    return;

  if (self->cancellable == NULL || g_cancellable_is_cancelled (self->cancellable))
    return;

  if (self->animation->transition_duration == 0.0 ||
      isnan (self->animation->transition_duration))
    return;

  n_steps = 255 / ANIMATION_OPACITY_STEP_INCREMENT;
  time_per_step = (self->animation->transition_duration * 1000) / n_steps;

  interval = MAX (ANIMATION_MIN_WAKEUP_INTERVAL * 1000, time_per_step);

  if (interval > G_MAXUINT32)
    return;

  self->update_animation_timeout_id =
    g_timeout_add_once ((guint) interval, update_animation_timeout_cb, self);
  g_source_set_name_by_id (self->update_animation_timeout_id,
                           "[gnome-shell] this._updateAnimation");
}

/* _updateAnimation(): the Promise.all() over the key frame textures */

typedef struct
{
  ShellBackground *background;
  TextureEntry **entries;
  guint n_entries;
  guint n_pending;
  gboolean done;
} AnimationUpdate;

static void
animation_update_clear (gpointer data)
{
  AnimationUpdate *update = data;
  guint i;

  for (i = 0; i < update->n_entries; i++)
    {
      if (update->entries[i] != NULL)
        texture_entry_unref (update->entries[i]);
    }

  g_free (update->entries);
  g_clear_object (&update->background);
}

typedef struct
{
  AnimationUpdate *update;
  guint index;
} AnimationUpdateRequest;

static void
on_animation_texture_loaded (GObject      *source,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  AnimationUpdateRequest *request = user_data;
  AnimationUpdate *update = request->update;
  ShellBackground *self = update->background;
  g_autoptr (TextureEntry) entry = NULL;
  g_autoptr (GError) error = NULL;

  entry = texture_cache_load_finish (SHELL_BACKGROUND_CACHE (source), result, &error);

  if (!self->cache->shutting_down && !update->done)
    {
      if (entry == NULL)
        {
          update->done = TRUE;

          if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("Failed to load animation: %s", error->message);
          set_loaded (self);
        }
      else
        {
          update->entries[request->index] = g_steal_pointer (&entry);
          update->n_pending--;

          if (update->n_pending == 0)
            {
              CoglTexture *texture1 = update->entries[0]->texture;
              ClutterColorState *color_state = update->entries[0]->color_state;

              update->done = TRUE;

              if (update->n_entries > 1)
                {
                  meta_background_set_blend_textures (self->meta_background,
                                                      texture1,
                                                      update->entries[1]->texture,
                                                      self->animation->transition_progress,
                                                      self->style,
                                                      color_state);
                }
              else
                {
                  meta_background_set_texture (self->meta_background,
                                               texture1, self->style,
                                               color_state);
                }

              set_loaded (self);
              queue_update_animation (self);
            }
        }
    }

  g_rc_box_release_full (update, animation_update_clear);
  g_free (request);
}

static void
update_animation (ShellBackground *self)
{
  g_autoptr (GPtrArray) files = NULL;
  AnimationUpdate *update;
  MtkRectangle monitor;
  guint i;

  self->update_animation_timeout_id = 0;

  get_monitor_geometry (self->monitor_index, &monitor);
  animation_update (self->animation, &monitor);
  files = g_ptr_array_ref (self->animation->key_frame_files);

  if (files->len == 0)
    {
      /* The JS called Meta.Background.set_file(null, style) here, a method
       * that does not exist anymore (so it threw and neither marked the
       * background as loaded nor requeued the update). Fall back to the
       * color or gradient. */
      load_pattern (self);
      set_loaded (self);
      queue_update_animation (self);
      return;
    }

  update = g_rc_box_new0 (AnimationUpdate);
  update->background = g_object_ref (self);
  update->n_entries = files->len;
  update->n_pending = files->len;
  update->entries = g_new0 (TextureEntry *, files->len);

  for (i = 0; i < files->len; i++)
    {
      GFile *file = g_ptr_array_index (files, i);
      AnimationUpdateRequest *request = g_new0 (AnimationUpdateRequest, 1);

      watch_file (self, file);

      request->update = g_rc_box_acquire (update);
      request->index = i;
      texture_cache_load_async (self->cache, file, self->cancellable,
                                on_animation_texture_loaded, request);
    }

  g_rc_box_release_full (update, animation_update_clear);
}

/* _loadAnimation() */

static void
on_animation_ready (Animation *animation,
                    gpointer   user_data)
{
  ShellBackground *self = user_data;

  g_clear_pointer (&self->animation, animation_unref);
  if (animation != NULL)
    self->animation = animation_ref (animation);

  if (self->animation == NULL || g_cancellable_is_cancelled (self->cancellable))
    {
      set_loaded (self);
      return;
    }

  update_animation (self);
  watch_file (self, self->file);
}

static void
load_animation (ShellBackground *self,
                GFile           *file)
{
  g_autofree char *schema_id = NULL;

  g_object_get (self->settings, "schema-id", &schema_id, NULL);

  cache_get_animation (self->cache, file, schema_id,
                       on_animation_ready,
                       g_object_ref (self), g_object_unref);
}

/* _loadImage() */

static void
on_image_loaded (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  g_autoptr (ShellBackground) self = user_data;
  g_autoptr (TextureEntry) entry = NULL;
  g_autoptr (GError) error = NULL;

  entry = texture_cache_load_finish (SHELL_BACKGROUND_CACHE (source), result, &error);

  if (self->cache->shutting_down)
    return;

  if (entry == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to load background: %s", error->message);
      set_loaded (self);
      return;
    }

  meta_background_set_texture (self->meta_background,
                               entry->texture, self->style,
                               entry->color_state);
  set_loaded (self);
}

static void
load_image (ShellBackground *self,
            GFile           *file)
{
  watch_file (self, file);

  texture_cache_load_async (self->cache, file, self->cancellable,
                            on_image_loaded, g_object_ref (self));
}

/* _loadFile() */

static void
on_content_type_queried (GObject      *source,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  g_autoptr (ShellBackground) self = user_data;
  g_autoptr (GFileInfo) info = NULL;
  const char *content_type;

  info = g_file_query_info_finish (G_FILE (source), result, NULL);

  if (self->cache->shutting_down)
    return;

  if (info == NULL)
    {
      set_loaded (self);
      return;
    }

  content_type = g_file_info_get_content_type (info);
  if (g_strcmp0 (content_type, "application/xml") == 0)
    load_animation (self, G_FILE (source));
  else
    load_image (self, G_FILE (source));
}

static void
load_file (ShellBackground *self,
           GFile           *file)
{
  g_file_query_info_async (file,
                           G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                           G_FILE_QUERY_INFO_NONE,
                           0,
                           self->cancellable,
                           on_content_type_queried,
                           g_object_ref (self));
}

/* _load() */

static void
load_background (ShellBackground *self)
{
  load_pattern (self);

  if (self->file == NULL)
    {
      set_loaded (self);
      return;
    }

  load_file (self, self->file);
}

static void
on_settings_changed (GSettings       *settings,
                     const char      *key,
                     ShellBackground *self)
{
  emit_changed_signal (self);
}

static void
on_timezone_changed (GnomeWallClock  *clock,
                     GParamSpec      *pspec,
                     ShellBackground *self)
{
  if (self->animation != NULL)
    {
      g_autoptr (GFile) file = g_object_ref (self->animation->file);

      load_animation (self, file);
    }
}

static void
on_prepare_for_sleep (ShellLoginManager *login_manager,
                      gboolean           about_to_suspend,
                      ShellBackground   *self)
{
  if (about_to_suspend)
    return;

  refresh_animation (self);
}

static void
stop_loaded_idle (ShellBackground *self)
{
  g_clear_handle_id (&self->loaded_idle_id, g_source_remove);
}

static void
on_global_shutdown_background (ShellGlobal     *global,
                               ShellBackground *self)
{
  /* Nothing may be emitted (into JS) while the shell is going away */
  shell_background_destroy (self);
  stop_loaded_idle (self);
}

/*
 * shell_background_destroy:
 *
 * Stops everything the background does and disconnects it from the settings,
 * the clock and the cache. The object itself (and the MetaBackground, with
 * whatever it shows) stays valid for the actors that still use it, as the
 * JS destroy() did.
 */
static void
shell_background_destroy (ShellBackground *self)
{
  GHashTableIter iter;
  gpointer value;

  if (self->destroyed)
    return;

  self->destroyed = TRUE;

  g_cancellable_cancel (self->cancellable);
  remove_animation_timeout (self);

  g_hash_table_iter_init (&iter, self->file_watches);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    g_signal_handler_disconnect (self->cache, GPOINTER_TO_SIZE (value));
  g_hash_table_remove_all (self->file_watches);

  if (self->clock != NULL)
    {
      g_clear_signal_handler (&self->clock_handler_id, self->clock);
      g_clear_object (&self->clock);
    }

  g_clear_signal_handler (&self->sleep_handler_id, shell_login_manager_get_default ());
  g_clear_signal_handler (&self->settings_handler_id, self->settings);
  g_clear_signal_handler (&self->interface_handler_id, self->interface_settings);
  g_clear_signal_handler (&self->shutdown_handler_id, shell_global_get ());

  g_clear_handle_id (&self->changed_idle_id, g_source_remove);
}

static void
shell_background_update_resolution (ShellBackground *self)
{
  if (self->animation != NULL)
    refresh_animation (self);
}

static gboolean
shell_background_is_loaded (ShellBackground *self)
{
  return self->is_loaded;
}

/* Returns: (transfer none) */
static MetaBackground *
shell_background_get_meta_background (ShellBackground *self)
{
  return self->meta_background;
}

static void
shell_background_dispose (GObject *object)
{
  ShellBackground *self = SHELL_BACKGROUND (object);

  shell_background_destroy (self);
  stop_loaded_idle (self);

  g_clear_pointer (&self->animation, animation_unref);
  g_clear_object (&self->meta_background);
  g_clear_object (&self->settings);
  g_clear_object (&self->interface_settings);
  g_clear_object (&self->file);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->cache);

  G_OBJECT_CLASS (shell_background_parent_class)->dispose (object);
}

static void
shell_background_finalize (GObject *object)
{
  ShellBackground *self = SHELL_BACKGROUND (object);

  g_clear_pointer (&self->file_watches, g_hash_table_unref);

  G_OBJECT_CLASS (shell_background_parent_class)->finalize (object);
}

static void
shell_background_class_init (ShellBackgroundClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_background_dispose;
  object_class->finalize = shell_background_finalize;

  background_signals[BACKGROUND_LOADED] =
    g_signal_new ("loaded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  background_signals[BACKGROUND_CHANGED] =
    g_signal_new ("bg-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_background_init (ShellBackground *self)
{
  self->file_watches = g_hash_table_new (g_direct_hash, g_direct_equal);
}

static ShellBackground *
shell_background_new (int                      monitor_index,
                      GSettings               *settings,
                      GFile                   *file,
                      GDesktopBackgroundStyle  style)
{
  ShellBackground *self = g_object_new (SHELL_TYPE_BACKGROUND, NULL);
  MetaDisplay *display = shell_global_get_display (shell_global_get ());

  self->meta_background = meta_background_new (display);

  self->settings = g_object_ref (settings);
  self->file = (file != NULL) ? g_object_ref (file) : NULL;
  self->style = style;
  self->monitor_index = monitor_index;
  self->cancellable = g_cancellable_new ();
  self->is_loaded = FALSE;
  self->cache = g_object_ref (get_background_cache ());

  self->interface_settings = g_settings_new (INTERFACE_SCHEMA);

  self->clock = gnome_wall_clock_new ();
  self->clock_handler_id = g_signal_connect (self->clock, "notify::timezone",
                                             G_CALLBACK (on_timezone_changed), self);

  self->sleep_handler_id = g_signal_connect (shell_login_manager_get_default (),
                                             "prepare-for-sleep",
                                             G_CALLBACK (on_prepare_for_sleep), self);

  self->settings_handler_id = g_signal_connect (self->settings, "changed",
                                                G_CALLBACK (on_settings_changed), self);

  self->interface_handler_id =
    g_signal_connect (self->interface_settings, "changed::" COLOR_SCHEME_KEY,
                      G_CALLBACK (on_settings_changed), self);

  self->shutdown_handler_id = g_signal_connect (shell_global_get (), "shutdown",
                                                G_CALLBACK (on_global_shutdown_background),
                                                self);

  load_background (self);

  return self;
}

/* ------------------------------------------------------------------------ */
/* BackgroundSource                                                          */
/* ------------------------------------------------------------------------ */

typedef struct
{
  BackgroundSource *source;
  int monitor_index;
  ShellBackground *background;
  gulong changed_id;
} BackgroundEntry;

static void
background_entry_free (BackgroundEntry *entry)
{
  g_clear_signal_handler (&entry->changed_id, entry->background);
  shell_background_destroy (entry->background);
  g_clear_object (&entry->background);
  g_free (entry);
}

static void
on_background_changed (ShellBackground *background,
                       BackgroundEntry *entry)
{
  BackgroundSource *source = entry->source;

  /* Disconnects the handler, destroys the background and forgets it */
  g_hash_table_remove (source->backgrounds, GINT_TO_POINTER (entry->monitor_index));
}

static void
on_monitors_changed (MetaMonitorManager *monitor_manager,
                     BackgroundSource   *source)
{
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  int n_monitors = meta_display_get_n_monitors (display);
  g_autoptr (GList) keys = g_hash_table_get_keys (source->backgrounds);
  GList *l;

  for (l = keys; l != NULL; l = l->next)
    {
      int monitor_index = GPOINTER_TO_INT (l->data);
      BackgroundEntry *entry = g_hash_table_lookup (source->backgrounds, l->data);

      if (entry == NULL)
        continue;

      if (monitor_index < n_monitors)
        shell_background_update_resolution (entry->background);
      else
        g_hash_table_remove (source->backgrounds, l->data);
    }
}

static BackgroundSource *
background_source_new (const char *schema)
{
  BackgroundSource *source = g_new0 (BackgroundSource, 1);
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (shell_global_get_backend (shell_global_get ()));

  source->schema = g_strdup (schema);

  /* Allow override the background image setting for performance testing */
  source->override_image = g_strdup (g_getenv ("SHELL_BACKGROUND_IMAGE"));
  source->settings = g_settings_new (schema);
  source->backgrounds = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                               NULL,
                                               (GDestroyNotify) background_entry_free);

  source->monitors_changed_id =
    g_signal_connect (monitor_manager, "monitors-changed",
                      G_CALLBACK (on_monitors_changed), source);

  source->interface_settings = g_settings_new (INTERFACE_SCHEMA);

  return source;
}

/* Returns: (transfer none): owned by @source, or by whatever reffed it */
static ShellBackground *
background_source_get_background (BackgroundSource *source,
                                  int               monitor_index)
{
  g_autoptr (GFile) file = NULL;
  GDesktopBackgroundStyle style;
  BackgroundEntry *entry;

  /* We don't watch changes to settings here,
   * instead we rely on ShellBackground to watch those
   * and emit 'bg-changed' at the right time */

  if (source->override_image != NULL)
    {
      file = g_file_new_for_path (source->override_image);
      style = G_DESKTOP_BACKGROUND_STYLE_ZOOM; /* Hardcode */
    }
  else
    {
      style = g_settings_get_enum (source->settings, BACKGROUND_STYLE_KEY);
      if (style != G_DESKTOP_BACKGROUND_STYLE_NONE)
        {
          GDesktopColorScheme color_scheme =
            g_settings_get_enum (source->interface_settings, COLOR_SCHEME_KEY);
          g_autofree char *uri =
            g_settings_get_string (source->settings,
                                   color_scheme == G_DESKTOP_COLOR_SCHEME_PREFER_DARK
                                   ? PICTURE_URI_DARK_KEY
                                   : PICTURE_URI_KEY);

          file = g_file_new_for_commandline_arg (uri);
        }
    }

  /* Animated backgrounds are (potentially) per-monitor, since
   * they can have variants that depend on the aspect ratio and
   * size of the monitor; for other backgrounds we can use the
   * same background object for all monitors. */
  if (file == NULL)
    {
      monitor_index = 0;
    }
  else
    {
      g_autofree char *basename = g_file_get_basename (file);

      if (basename == NULL || !g_str_has_suffix (basename, ".xml"))
        monitor_index = 0;
    }

  entry = g_hash_table_lookup (source->backgrounds, GINT_TO_POINTER (monitor_index));
  if (entry == NULL)
    {
      entry = g_new0 (BackgroundEntry, 1);
      entry->source = source;
      entry->monitor_index = monitor_index;
      entry->background = shell_background_new (monitor_index,
                                                source->settings,
                                                file, style);
      entry->changed_id = g_signal_connect (entry->background, "bg-changed",
                                            G_CALLBACK (on_background_changed),
                                            entry);

      g_hash_table_insert (source->backgrounds, GINT_TO_POINTER (monitor_index), entry);
    }

  return entry->background;
}

static void
background_source_destroy (BackgroundSource *source)
{
  MetaMonitorManager *monitor_manager =
    meta_backend_get_monitor_manager (shell_global_get_backend (shell_global_get ()));

  g_signal_handler_disconnect (monitor_manager, source->monitors_changed_id);

  g_clear_pointer (&source->backgrounds, g_hash_table_unref);
  g_clear_object (&source->settings);
  g_clear_object (&source->interface_settings);
  g_free (source->override_image);
  g_free (source->schema);
  g_free (source);
}

/* ------------------------------------------------------------------------ */
/* ShellSystemBackground                                                     */
/* ------------------------------------------------------------------------ */

/**
 * ShellSystemBackground:
 *
 * A dark background covering monitor 0 (or whatever size it is given), shown
 * while the real background is loading. Meta.BackgroundActor, which the JS
 * class derived from, is a final type in C, so this actor wraps one.
 */

enum
{
  SYSTEM_BACKGROUND_LOADED,
  N_SYSTEM_BACKGROUND_SIGNALS
};

static guint system_background_signals[N_SYSTEM_BACKGROUND_SIGNALS] = { 0 };

struct _ShellSystemBackground
{
  ClutterActor parent;

  MetaBackground *background;
  guint loaded_idle_id;
};

G_DEFINE_FINAL_TYPE (ShellSystemBackground, shell_system_background, CLUTTER_TYPE_ACTOR)

/* The module-level _systemBackground of the JS: shared by all instances,
 * and dropped with the last of them. */
static MetaBackground *system_background = NULL;

static MetaBackground *
ref_system_background (void)
{
  if (system_background == NULL)
    {
      MetaDisplay *display = shell_global_get_display (shell_global_get ());
      CoglColor default_color = { .red = 40, .green = 40, .blue = 40, .alpha = 255 };

      system_background = meta_background_new (display);
      g_object_add_weak_pointer (G_OBJECT (system_background),
                                 (gpointer *) &system_background);
      meta_background_set_color (system_background, &default_color);

      return system_background;
    }

  return g_object_ref (system_background);
}

static void
system_background_loaded_idle_cb (gpointer data)
{
  ShellSystemBackground *self = data;

  self->loaded_idle_id = 0;
  g_signal_emit (self, system_background_signals[SYSTEM_BACKGROUND_LOADED], 0);
}

static void
on_global_shutdown_system_background (ShellGlobal           *global,
                                      ShellSystemBackground *self)
{
  g_clear_handle_id (&self->loaded_idle_id, g_source_remove);
}

static void
shell_system_background_constructed (GObject *object)
{
  ShellSystemBackground *self = SHELL_SYSTEM_BACKGROUND (object);
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  ClutterActor *actor;

  G_OBJECT_CLASS (shell_system_background_parent_class)->constructed (object);

  self->background = ref_system_background ();

  actor = meta_background_actor_new (display, 0);
  meta_background_content_set_background (META_BACKGROUND_CONTENT (clutter_actor_get_content (actor)),
                                          self->background);

  /* The wrapped actor always covers the whole wrapper */
  clutter_actor_set_layout_manager (CLUTTER_ACTOR (self),
                                    clutter_bin_layout_new ());
  clutter_actor_add_child (CLUTTER_ACTOR (self), actor);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_global_shutdown_system_background),
                           self, 0);

  self->loaded_idle_id = idle_add_default (system_background_loaded_idle_cb, self);
  g_source_set_name_by_id (self->loaded_idle_id,
                           "[gnome-shell] SystemBackground.loaded");
}

static void
shell_system_background_dispose (GObject *object)
{
  ShellSystemBackground *self = SHELL_SYSTEM_BACKGROUND (object);

  g_clear_handle_id (&self->loaded_idle_id, g_source_remove);
  g_clear_object (&self->background);

  G_OBJECT_CLASS (shell_system_background_parent_class)->dispose (object);
}

static void
shell_system_background_class_init (ShellSystemBackgroundClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_system_background_constructed;
  object_class->dispose = shell_system_background_dispose;

  /**
   * ShellSystemBackground::loaded:
   * @self: the #ShellSystemBackground
   *
   * Emitted (from an idle) after the actor was created.
   */
  system_background_signals[SYSTEM_BACKGROUND_LOADED] =
    g_signal_new ("loaded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_system_background_init (ShellSystemBackground *self)
{
}

/**
 * shell_system_background_new:
 *
 * Creates a dark background actor for monitor 0.
 *
 * Returns: (transfer full): a new #ShellSystemBackground
 */
ShellSystemBackground *
shell_system_background_new (void)
{
  return g_object_new (SHELL_TYPE_SYSTEM_BACKGROUND, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellBackgroundManager                                                    */
/* ------------------------------------------------------------------------ */

/**
 * ShellBackgroundManager:
 *
 * Creates the background actor of a monitor and keeps it up to date. See
 * #ShellBackgroundManager:background-actor.
 */

enum
{
  PROP_0,

  PROP_CONTAINER,
  PROP_MONITOR_INDEX,
  PROP_VIGNETTE,
  PROP_CONTROL_POSITION,
  PROP_SETTINGS_SCHEMA,
  PROP_USE_CONTENT_SIZE,
  PROP_BACKGROUND_ACTOR,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  CHANGED,
  LOADED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

typedef struct
{
  ShellBackgroundManager *manager;
  guint id;
} LoadedIdle;

struct _ShellBackgroundManager
{
  GObject parent;

  ClutterActor *container;
  int monitor_index;
  gboolean vignette;
  gboolean control_position;
  gboolean use_content_size;
  char *settings_schema;

  BackgroundSource *source;
  ClutterActor *background_actor;
  ClutterActor *new_background_actor;

  GSList *loaded_idles; /* LoadedIdle */
};

G_DEFINE_FINAL_TYPE (ShellBackgroundManager, shell_background_manager, G_TYPE_OBJECT)

/* Main.layoutManager.screen_transition, set by the layout manager */
static ClutterActor *screen_transition = NULL;

/*
 * Per background actor: what the JS kept in closures over the actor in
 * _createBackgroundActor() (and its loadedSignalId expando).
 */
typedef struct
{
  ShellBackgroundManager *manager; /* weak */
  ShellBackground *background;
  gulong changed_id;
  gulong loaded_id;
  gulong swap_loaded_id;
} ActorLink;

static GQuark
actor_link_quark (void)
{
  return g_quark_from_static_string ("shell-background-actor-link");
}

static void
actor_link_disconnect (ActorLink *link)
{
  g_clear_signal_handler (&link->changed_id, link->background);
  g_clear_signal_handler (&link->loaded_id, link->background);
  g_clear_signal_handler (&link->swap_loaded_id, link->background);
}

static void
actor_link_free (ActorLink *link)
{
  actor_link_disconnect (link);
  g_clear_weak_pointer (&link->manager);
  g_clear_object (&link->background);
  g_free (link);
}

static ActorLink *
get_actor_link (ClutterActor *actor)
{
  return g_object_get_qdata (G_OBJECT (actor), actor_link_quark ());
}

static void
manager_loaded_idle_cb (gpointer data)
{
  LoadedIdle *idle = data;
  g_autoptr (ShellBackgroundManager) self = g_object_ref (idle->manager);

  self->loaded_idles = g_slist_remove (self->loaded_idles, idle);
  g_free (idle);

  g_signal_emit (self, signals[LOADED], 0);
}

static void
queue_loaded_emission (ShellBackgroundManager *self)
{
  LoadedIdle *idle = g_new0 (LoadedIdle, 1);

  idle->manager = self;
  idle->id = idle_add_default (manager_loaded_idle_cb, idle);

  self->loaded_idles = g_slist_prepend (self->loaded_idles, idle);
}

static void
clear_loaded_idles (ShellBackgroundManager *self)
{
  GSList *l;

  for (l = self->loaded_idles; l != NULL; l = l->next)
    {
      LoadedIdle *idle = l->data;

      g_source_remove (idle->id);
      g_free (idle);
    }

  g_clear_pointer (&self->loaded_idles, g_slist_free);
}

static void
fade_out_complete_cb (gboolean finished,
                      gpointer user_data)
{
  /* onComplete: only when the animation ran to its end */
  if (finished)
    clutter_actor_destroy (CLUTTER_ACTOR (user_data));
}

static void
swap_background_actor (ShellBackgroundManager *self)
{
  g_autoptr (ShellBackgroundManager) keep_alive = g_object_ref (self);
  ClutterActor *old_background_actor;
  ShellEaseParams params = {
    .duration = FADE_ANIMATION_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  if (self->new_background_actor == NULL)
    return;

  old_background_actor = self->background_actor;
  self->background_actor = self->new_background_actor;
  self->new_background_actor = NULL;

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BACKGROUND_ACTOR]);
  g_signal_emit (self, signals[CHANGED], 0);

  if (old_background_actor == NULL)
    return;

  if (screen_transition != NULL && clutter_actor_is_visible (screen_transition))
    {
      clutter_actor_destroy (old_background_actor);
      g_object_unref (old_background_actor);
      return;
    }

  /* The reference of the manager now belongs to the animation */
  shell_actor_ease (old_background_actor, &params,
                    fade_out_complete_cb, old_background_actor,
                    g_object_unref,
                    "opacity", 0u,
                    NULL);
}

static void
on_background_loaded_for_swap (ShellBackground *background,
                               ActorLink       *link)
{
  g_clear_signal_handler (&link->swap_loaded_id, background);

  if (link->manager != NULL)
    swap_background_actor (link->manager);
}

static void
copy_content_property (ClutterContent *from,
                       ClutterContent *to,
                       const char     *name)
{
  GParamSpec *pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (from), name);
  g_auto (GValue) value = G_VALUE_INIT;

  if (pspec == NULL)
    return;

  g_value_init (&value, G_PARAM_SPEC_VALUE_TYPE (pspec));
  g_object_get_property (G_OBJECT (from), name, &value);
  g_object_set_property (G_OBJECT (to), name, &value);
}

static ClutterActor *create_background_actor (ShellBackgroundManager *self);

static void
update_background_actor (ShellBackgroundManager *self)
{
  ClutterActor *new_background_actor;
  ClutterContent *old_content, *new_content;
  ActorLink *link;

  if (self->new_background_actor != NULL)
    {
      /* Skip displaying existing background queued for load */
      clutter_actor_destroy (self->new_background_actor);
      g_clear_object (&self->new_background_actor);
    }

  if (self->background_actor == NULL)
    return;

  new_background_actor = create_background_actor (self);

  old_content = clutter_actor_get_content (self->background_actor);
  new_content = clutter_actor_get_content (new_background_actor);

  copy_content_property (old_content, new_content, "vignette-sharpness");
  copy_content_property (old_content, new_content, "brightness");

  if (clutter_actor_is_visible (self->background_actor))
    clutter_actor_show (new_background_actor);
  else
    clutter_actor_hide (new_background_actor);

  self->new_background_actor = g_object_ref (new_background_actor);

  link = get_actor_link (new_background_actor);

  if (shell_background_is_loaded (link->background))
    {
      swap_background_actor (self);
    }
  else
    {
      link->swap_loaded_id =
        g_signal_connect (link->background, "loaded",
                          G_CALLBACK (on_background_loaded_for_swap), link);
    }
}

static void
on_background_changed_for_actor (ShellBackground *background,
                                 ActorLink       *link)
{
  g_autoptr (ShellBackgroundManager) manager = NULL;

  g_clear_signal_handler (&link->changed_id, background);

  if (link->manager == NULL)
    return;

  manager = g_object_ref (link->manager);
  update_background_actor (manager);
}

static void
on_background_loaded_for_manager (ShellBackground *background,
                                  ActorLink       *link)
{
  g_autoptr (ShellBackgroundManager) manager = NULL;

  g_clear_signal_handler (&link->loaded_id, background);

  if (link->manager == NULL)
    return;

  manager = g_object_ref (link->manager);
  g_signal_emit (manager, signals[LOADED], 0);
}

static void
on_background_actor_destroy (ClutterActor *actor,
                             ActorLink    *link)
{
  actor_link_disconnect (link);
}

static ClutterActor *
create_background_actor (ShellBackgroundManager *self)
{
  ShellBackground *background =
    background_source_get_background (self->source, self->monitor_index);
  MetaDisplay *display = shell_global_get_display (shell_global_get ());
  ClutterActor *background_actor;
  MetaBackgroundContent *content;
  ActorLink *link;

  background_actor = meta_background_actor_new (display, self->monitor_index);
  clutter_actor_set_request_mode (background_actor,
                                  self->use_content_size
                                  ? CLUTTER_REQUEST_CONTENT_SIZE
                                  : CLUTTER_REQUEST_HEIGHT_FOR_WIDTH);
  clutter_actor_set_x_expand (background_actor, !self->use_content_size);
  clutter_actor_set_y_expand (background_actor, !self->use_content_size);

  content = META_BACKGROUND_CONTENT (clutter_actor_get_content (background_actor));
  meta_background_content_set_background (content,
                                          shell_background_get_meta_background (background));
  meta_background_content_set_vignette (content, self->vignette, 0.5, 0.5);

  clutter_actor_add_child (self->container, background_actor);

  if (self->control_position)
    {
      MtkRectangle monitor;

      get_monitor_geometry (self->monitor_index, &monitor);
      clutter_actor_set_position (background_actor, monitor.x, monitor.y);
      clutter_actor_set_child_below_sibling (self->container, background_actor, NULL);
    }

  link = g_new0 (ActorLink, 1);
  g_set_weak_pointer (&link->manager, self);
  link->background = g_object_ref (background);
  g_object_set_qdata_full (G_OBJECT (background_actor), actor_link_quark (),
                           link, (GDestroyNotify) actor_link_free);

  link->changed_id = g_signal_connect (background, "bg-changed",
                                       G_CALLBACK (on_background_changed_for_actor),
                                       link);

  if (shell_background_is_loaded (background))
    {
      queue_loaded_emission (self);
    }
  else
    {
      link->loaded_id = g_signal_connect (background, "loaded",
                                          G_CALLBACK (on_background_loaded_for_manager),
                                          link);
    }

  g_signal_connect (background_actor, "destroy",
                    G_CALLBACK (on_background_actor_destroy), link);

  return background_actor;
}

static void
on_global_shutdown_manager (ShellGlobal            *global,
                            ShellBackgroundManager *self)
{
  /* Nothing may be emitted (into JS) while the shell is going away */
  clear_loaded_idles (self);
}

static void
shell_background_manager_constructed (GObject *object)
{
  ShellBackgroundManager *self = SHELL_BACKGROUND_MANAGER (object);

  G_OBJECT_CLASS (shell_background_manager_parent_class)->constructed (object);

  if (self->container == NULL)
    {
      g_warning ("ShellBackgroundManager needs a container");
      return;
    }

  self->source = cache_get_background_source (get_background_cache (),
                                              self->settings_schema);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_global_shutdown_manager), self, 0);

  self->background_actor = g_object_ref (create_background_actor (self));
  self->new_background_actor = NULL;
}

static void
shell_background_manager_dispose (GObject *object)
{
  ShellBackgroundManager *self = SHELL_BACKGROUND_MANAGER (object);

  clear_loaded_idles (self);

  if (self->source != NULL)
    {
      cache_release_background_source (get_background_cache (), self->settings_schema);
      self->source = NULL;
    }

  if (self->new_background_actor != NULL)
    {
      clutter_actor_destroy (self->new_background_actor);
      g_clear_object (&self->new_background_actor);
    }

  if (self->background_actor != NULL)
    {
      clutter_actor_destroy (self->background_actor);
      g_clear_object (&self->background_actor);
      g_object_notify_by_pspec (object, props[PROP_BACKGROUND_ACTOR]);
    }

  g_clear_object (&self->container);

  G_OBJECT_CLASS (shell_background_manager_parent_class)->dispose (object);
}

static void
shell_background_manager_finalize (GObject *object)
{
  ShellBackgroundManager *self = SHELL_BACKGROUND_MANAGER (object);

  g_free (self->settings_schema);

  G_OBJECT_CLASS (shell_background_manager_parent_class)->finalize (object);
}

static void
shell_background_manager_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellBackgroundManager *self = SHELL_BACKGROUND_MANAGER (object);

  switch (prop_id)
    {
    case PROP_CONTAINER:
      g_value_set_object (value, self->container);
      break;
    case PROP_MONITOR_INDEX:
      g_value_set_int (value, self->monitor_index);
      break;
    case PROP_VIGNETTE:
      g_value_set_boolean (value, self->vignette);
      break;
    case PROP_CONTROL_POSITION:
      g_value_set_boolean (value, self->control_position);
      break;
    case PROP_SETTINGS_SCHEMA:
      g_value_set_string (value, self->settings_schema);
      break;
    case PROP_USE_CONTENT_SIZE:
      g_value_set_boolean (value, self->use_content_size);
      break;
    case PROP_BACKGROUND_ACTOR:
      g_value_set_object (value, self->background_actor);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_background_manager_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellBackgroundManager *self = SHELL_BACKGROUND_MANAGER (object);

  switch (prop_id)
    {
    case PROP_CONTAINER:
      self->container = g_value_dup_object (value);
      break;
    case PROP_MONITOR_INDEX:
      self->monitor_index = g_value_get_int (value);
      break;
    case PROP_VIGNETTE:
      self->vignette = g_value_get_boolean (value);
      break;
    case PROP_CONTROL_POSITION:
      self->control_position = g_value_get_boolean (value);
      break;
    case PROP_SETTINGS_SCHEMA:
      g_free (self->settings_schema);
      self->settings_schema = g_value_dup_string (value);
      if (self->settings_schema == NULL)
        self->settings_schema = g_strdup (BACKGROUND_SCHEMA);
      break;
    case PROP_USE_CONTENT_SIZE:
      self->use_content_size = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_background_manager_class_init (ShellBackgroundManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_background_manager_constructed;
  object_class->dispose = shell_background_manager_dispose;
  object_class->finalize = shell_background_manager_finalize;
  object_class->get_property = shell_background_manager_get_property;
  object_class->set_property = shell_background_manager_set_property;

  /**
   * ShellBackgroundManager:container:
   *
   * The actor the background actors are added to.
   */
  props[PROP_CONTAINER] =
    g_param_spec_object ("container", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellBackgroundManager:monitor-index:
   *
   * The monitor the background is for. The geometry of the monitor comes
   * straight from the #MetaDisplay.
   */
  props[PROP_MONITOR_INDEX] =
    g_param_spec_int ("monitor-index", NULL, NULL,
                      0, G_MAXINT, 0,
                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellBackgroundManager:vignette:
   *
   * Whether the background actors have a vignette.
   */
  props[PROP_VIGNETTE] =
    g_param_spec_boolean ("vignette", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellBackgroundManager:control-position:
   *
   * Whether the manager moves the background actors to the position of the
   * monitor and keeps them below their siblings.
   */
  props[PROP_CONTROL_POSITION] =
    g_param_spec_boolean ("control-position", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellBackgroundManager:settings-schema:
   *
   * The GSettings schema the background is read from.
   */
  props[PROP_SETTINGS_SCHEMA] =
    g_param_spec_string ("settings-schema", NULL, NULL,
                         BACKGROUND_SCHEMA,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellBackgroundManager:use-content-size:
   *
   * Whether the background actors request the size of their content
   * (otherwise they expand to fill the container).
   */
  props[PROP_USE_CONTENT_SIZE] =
    g_param_spec_boolean ("use-content-size", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellBackgroundManager:background-actor:
   *
   * The current background actor, a #MetaBackgroundActor. It is replaced
   * (see #ShellBackgroundManager::changed) when the background changes and
   * is %NULL after shell_background_manager_destroy().
   */
  props[PROP_BACKGROUND_ACTOR] =
    g_param_spec_object ("background-actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellBackgroundManager::changed:
   * @self: the #ShellBackgroundManager
   *
   * Emitted when the background actor was replaced by a new one, which is
   * now #ShellBackgroundManager:background-actor. The old one fades out.
   */
  signals[CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellBackgroundManager::loaded:
   * @self: the #ShellBackgroundManager
   *
   * Emitted (from an idle) when the background finished loading.
   */
  signals[LOADED] =
    g_signal_new ("loaded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_background_manager_init (ShellBackgroundManager *self)
{
  self->control_position = TRUE;
  self->use_content_size = TRUE;
  self->settings_schema = g_strdup (BACKGROUND_SCHEMA);
}

/**
 * shell_background_manager_new:
 * @container: the actor to add the background actors to
 * @monitor_index: the monitor to show the background of
 *
 * Creates a manager for the main background (#ShellBackgroundManager:settings-schema),
 * positioned at the monitor and with the size of its content. Use g_object_new()
 * to set any other property.
 *
 * Returns: (transfer full): a new #ShellBackgroundManager
 */
ShellBackgroundManager *
shell_background_manager_new (ClutterActor *container,
                              int           monitor_index)
{
  g_return_val_if_fail (CLUTTER_IS_ACTOR (container), NULL);

  return g_object_new (SHELL_TYPE_BACKGROUND_MANAGER,
                       "container", container,
                       "monitor-index", monitor_index,
                       NULL);
}

/**
 * shell_background_manager_get_background_actor:
 * @self: a #ShellBackgroundManager
 *
 * Returns: (transfer none) (nullable): the current background actor
 */
ClutterActor *
shell_background_manager_get_background_actor (ShellBackgroundManager *self)
{
  g_return_val_if_fail (SHELL_IS_BACKGROUND_MANAGER (self), NULL);

  return self->background_actor;
}

/**
 * shell_background_manager_destroy:
 * @self: a #ShellBackgroundManager
 *
 * Destroys the background actors and releases the background. The manager
 * does nothing afterwards. It is safe to call this more than once.
 */
void
shell_background_manager_destroy (ShellBackgroundManager *self)
{
  g_return_if_fail (SHELL_IS_BACKGROUND_MANAGER (self));

  g_object_run_dispose (G_OBJECT (self));
}

/**
 * shell_background_manager_set_screen_transition:
 * @screen_transition_actor: (nullable): the actor of the screen transition
 *
 * Tells the backgrounds about the screen transition of the layout manager
 * (Main.layoutManager.screen_transition). While it is visible a replaced
 * background actor is destroyed immediately instead of fading out.
 */
void
shell_background_manager_set_screen_transition (ClutterActor *screen_transition_actor)
{
  g_return_if_fail (screen_transition_actor == NULL ||
                    CLUTTER_IS_ACTOR (screen_transition_actor));

  g_set_weak_pointer (&screen_transition, screen_transition_actor);
}
