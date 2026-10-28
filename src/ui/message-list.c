/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * message-list.c: The messages of the message tray banners, of the date menu
 * and of the lock screen. Port of js/ui/messageList.js.
 *
 * What is private to this file, and was a JS class of its own: the URL
 * highlighter of the body of a message, the label of the time, the layout
 * managers (`ScaleLayout`, `LabelExpanderLayout`, `MessageGroupExpanderLayout`
 * and `MessageViewLayout`), the group of the notifications of a source
 * (`NotificationMessageGroup`) and the fade effect of the view
 * (`FadeEffect`). The header of a message (`MessageHeader`) is built by
 * #ShellMessage.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `MediaMessage` and the MPRIS players stay in JS (js/ui/mpris.js):
 *    #ShellMessageView:message-tray provides the notification sources, and
 *    the caller adds the media messages with
 *    shell_message_view_add_media_message ().
 *  - The messages are limited to a number of grapheme clusters as Pango finds
 *    them in the text (`Intl.Segmenter` is not available).
 *  - `async expand ()` and `async collapse ()` of the group, and the async
 *    `_setExpandedGroup ()` of the view, are chains of animation callbacks.
 */

#include "config.h"

#include <math.h>
#include <string.h>

#include <atk/atk.h>
#include <clutter/clutter.h>
#include <clutter/clutter-pango.h>
/* clutter.h does not include the motion controller (mutter 51) */
#define __CLUTTER_H_INSIDE__
#include <clutter/clutter-motion-controller.h>
#undef __CLUTTER_H_INSIDE__
#include <cogl/cogl.h>
#include <glib/gi18n-lib.h>
#include <graphene.h>
#include <pango/pango.h>

#include "misc/date-utils.h"
#include "misc/text-utils.h"
#include "shell-global.h"
#include "ui/environment.h"
#include "ui/message-list.h"

#define MAX_NOTIFICATION_BUTTONS 3
#define MESSAGE_ANIMATION_TIME 100

#define EXPANDED_GROUP_OVERSHOT_HEIGHT 50
#define DEFAULT_EXPAND_LINES 6

#define GROUP_EXPANSION_TIME 200
#define MAX_VISIBLE_STACKED_MESSAGES 3
#define ADDITIONAL_BOTTOM_MARGIN_EXPANDED_GROUP 15
#define WIDTH_OFFSET_STACKED 6
#define HEIGHT_OFFSET_STACKED 10
#define HEIGHT_OFFSET_REDUCTION_STACKED 1.4

#define MAX_TITLE_LENGTH 150
#define MAX_BODY_LENGTH 850

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static void
init_ease_params (ShellEaseParams     *params,
                  int                  duration,
                  int                  delay,
                  ClutterAnimationMode mode)
{
  params->duration = duration;
  params->delay = delay;
  params->mode = mode;
  params->repeat_count = 0;
  params->auto_reverse = FALSE;
  params->animation_required = FALSE;
}

/* Clutter.Actor.ease_property () */
static void
ease_actor_property (ClutterActor        *actor,
                     const char          *property,
                     double               target,
                     int                  duration,
                     ClutterAnimationMode mode,
                     ShellEaseCallback    callback,
                     gpointer             user_data,
                     GDestroyNotify       destroy)
{
  ShellEaseParams params;
  g_auto (GValue) value = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;

  init_ease_params (&params, duration, -1, mode);

  g_value_init (&value, G_TYPE_DOUBLE);
  g_value_set_double (&value, target);

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (actor), property,
                                       &value, &params,
                                       callback, user_data, destroy, &error))
    g_debug ("%s: %s", G_STRFUNC, error->message);
}

/* St.Adjustment.ease () */
static void
ease_adjustment_value (StAdjustment        *adjustment,
                       double               target,
                       int                  duration,
                       ClutterAnimationMode mode)
{
  ShellEaseParams params;
  g_auto (GValue) value = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;

  init_ease_params (&params, duration, -1, mode);

  g_value_init (&value, G_TYPE_DOUBLE);
  g_value_set_double (&value, target);

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (adjustment), "value",
                                       &value, &params, NULL, NULL, NULL, &error))
    g_debug ("%s: %s", G_STRFUNC, error->message);
}

/* Limits a string to a number of grapheme clusters, as Intl.Segmenter () does
 * for the JS module */
static char *
limit_string (const char *string,
              int         limit)
{
  glong n_chars = g_utf8_strlen (string, -1);
  g_autofree PangoLogAttr *attrs = NULL;
  int count = 0;
  glong i;

  if (n_chars <= limit)
    return g_strdup (string);

  attrs = g_new0 (PangoLogAttr, n_chars + 1);
  pango_get_log_attrs (string, -1, -1, pango_language_get_default (), attrs, n_chars + 1);

  for (i = 1; i <= n_chars; i++)
    {
      if (!attrs[i].is_cursor_position)
        continue;

      count++;
      if (count == limit)
        return g_strndup (string, g_utf8_offset_to_pointer (string, i) - string);
    }

  return g_strdup (string);
}

static char *
replace_newlines (const char *text)
{
  char *copy = g_strdup (text);

  return g_strdelimit (copy, "\n", ' ');
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

static GList *
get_children_list (ClutterActor *actor)
{
  GList *children = NULL;
  ClutterActor *child;

  for (child = clutter_actor_get_last_child (actor);
       child != NULL;
       child = clutter_actor_get_previous_sibling (child))
    children = g_list_prepend (children, child);

  return children;
}

/* ------------------------------------------------------------------------ */
/* ShellScaleLayout                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  SCALE_LAYOUT_PROP_0,

  SCALE_LAYOUT_PROP_SCALING_ENABLED,

  SCALE_LAYOUT_N_PROPS
};

static GParamSpec *scale_layout_props[SCALE_LAYOUT_N_PROPS] = { NULL, };

#define SHELL_TYPE_SCALE_LAYOUT (shell_scale_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellScaleLayout, shell_scale_layout,
                      SHELL, SCALE_LAYOUT, ClutterBinLayout)

struct _ShellScaleLayout
{
  ClutterBinLayout parent;

  ClutterActor *container; /* weak */
  gboolean scaling_enabled;
};

G_DEFINE_FINAL_TYPE (ShellScaleLayout, shell_scale_layout, CLUTTER_TYPE_BIN_LAYOUT)

static void
scale_layout_container_scale_changed_cb (ClutterActor     *container,
                                         GParamSpec       *pspec,
                                         ClutterLayoutManager *layout)
{
  clutter_layout_manager_layout_changed (layout);
}

static void
shell_scale_layout_set_container (ClutterLayoutManager *manager,
                                  ClutterActor         *container)
{
  ShellScaleLayout *self = SHELL_SCALE_LAYOUT (manager);

  if (self->container == container)
    return;

  if (self->container != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->container, self);
      g_clear_weak_pointer (&self->container);
    }

  g_set_weak_pointer (&self->container, container);

  if (self->container != NULL)
    {
      g_signal_connect (self->container, "notify::scale-x",
                        G_CALLBACK (scale_layout_container_scale_changed_cb), self);
      g_signal_connect (self->container, "notify::scale-y",
                        G_CALLBACK (scale_layout_container_scale_changed_cb), self);
    }
}

static void
shell_scale_layout_get_preferred_width (ClutterLayoutManager *manager,
                                        ClutterActor         *container,
                                        float                 for_height,
                                        float                *min_width_p,
                                        float                *nat_width_p)
{
  ShellScaleLayout *self = SHELL_SCALE_LAYOUT (manager);
  ClutterLayoutManagerClass *parent_class =
    CLUTTER_LAYOUT_MANAGER_CLASS (shell_scale_layout_parent_class);
  float min = 0, nat = 0;
  double scale_x;

  parent_class->get_preferred_width (manager, container, for_height, &min, &nat);

  if (self->scaling_enabled)
    {
      clutter_actor_get_scale (container, &scale_x, NULL);
      min = floorf (min * scale_x);
      nat = floorf (nat * scale_x);
    }

  if (min_width_p)
    *min_width_p = min;
  if (nat_width_p)
    *nat_width_p = nat;
}

static void
shell_scale_layout_get_preferred_height (ClutterLayoutManager *manager,
                                         ClutterActor         *container,
                                         float                 for_width,
                                         float                *min_height_p,
                                         float                *nat_height_p)
{
  ShellScaleLayout *self = SHELL_SCALE_LAYOUT (manager);
  ClutterLayoutManagerClass *parent_class =
    CLUTTER_LAYOUT_MANAGER_CLASS (shell_scale_layout_parent_class);
  float min = 0, nat = 0;
  double scale_y;

  parent_class->get_preferred_height (manager, container, for_width, &min, &nat);

  if (self->scaling_enabled)
    {
      clutter_actor_get_scale (container, NULL, &scale_y);
      min = floorf (min * scale_y);
      nat = floorf (nat * scale_y);
    }

  if (min_height_p)
    *min_height_p = min;
  if (nat_height_p)
    *nat_height_p = nat;
}

static void
shell_scale_layout_set_scaling_enabled (ShellScaleLayout *self,
                                        gboolean          scaling_enabled)
{
  scaling_enabled = !!scaling_enabled;
  if (self->scaling_enabled == scaling_enabled)
    return;

  self->scaling_enabled = scaling_enabled;
  g_object_notify_by_pspec (G_OBJECT (self), scale_layout_props[SCALE_LAYOUT_PROP_SCALING_ENABLED]);
  clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
}

static void
shell_scale_layout_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellScaleLayout *self = SHELL_SCALE_LAYOUT (object);

  switch (prop_id)
    {
    case SCALE_LAYOUT_PROP_SCALING_ENABLED:
      g_value_set_boolean (value, self->scaling_enabled);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_scale_layout_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellScaleLayout *self = SHELL_SCALE_LAYOUT (object);

  switch (prop_id)
    {
    case SCALE_LAYOUT_PROP_SCALING_ENABLED:
      shell_scale_layout_set_scaling_enabled (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_scale_layout_dispose (GObject *object)
{
  ShellScaleLayout *self = SHELL_SCALE_LAYOUT (object);

  if (self->container != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->container, self);
      g_clear_weak_pointer (&self->container);
    }

  G_OBJECT_CLASS (shell_scale_layout_parent_class)->dispose (object);
}

static void
shell_scale_layout_class_init (ShellScaleLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  object_class->get_property = shell_scale_layout_get_property;
  object_class->set_property = shell_scale_layout_set_property;
  object_class->dispose = shell_scale_layout_dispose;

  layout_class->set_container = shell_scale_layout_set_container;
  layout_class->get_preferred_width = shell_scale_layout_get_preferred_width;
  layout_class->get_preferred_height = shell_scale_layout_get_preferred_height;

  scale_layout_props[SCALE_LAYOUT_PROP_SCALING_ENABLED] =
    g_param_spec_boolean ("scaling-enabled", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, SCALE_LAYOUT_N_PROPS, scale_layout_props);
}

static void
shell_scale_layout_init (ShellScaleLayout *self)
{
  self->scaling_enabled = TRUE;
}

static ClutterLayoutManager *
shell_scale_layout_new (void)
{
  return g_object_new (SHELL_TYPE_SCALE_LAYOUT, NULL);
}

/* An St.Bin that scales its size with its scale factors, as the items of the
 * message list that zoom in and out. */
static ClutterActor *
scaling_item_new (ClutterActor *child)
{
  ClutterActor *item;

  item = g_object_new (ST_TYPE_BIN,
                       "can-focus", FALSE,
                       NULL);
  clutter_actor_set_layout_manager (item, shell_scale_layout_new ());
  clutter_actor_set_pivot_point (item, 0.5, 0.5);
  clutter_actor_set_scale (item, 0, 0);
  st_bin_set_child (ST_BIN (item), child);

  return item;
}

static void
scaling_item_set_scaling_enabled (ClutterActor *item,
                                  gboolean      enabled)
{
  ClutterLayoutManager *layout = clutter_actor_get_layout_manager (item);

  if (SHELL_IS_SCALE_LAYOUT (layout))
    shell_scale_layout_set_scaling_enabled (SHELL_SCALE_LAYOUT (layout), enabled);
}

/* ------------------------------------------------------------------------ */
/* ShellLabelExpanderLayout                                                  */
/* ------------------------------------------------------------------------ */

enum
{
  LABEL_EXPANDER_PROP_0,

  LABEL_EXPANDER_PROP_EXPANSION,

  LABEL_EXPANDER_N_PROPS
};

static GParamSpec *label_expander_props[LABEL_EXPANDER_N_PROPS] = { NULL, };

#define SHELL_TYPE_LABEL_EXPANDER_LAYOUT (shell_label_expander_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellLabelExpanderLayout, shell_label_expander_layout,
                      SHELL, LABEL_EXPANDER_LAYOUT, ClutterBinLayout)

struct _ShellLabelExpanderLayout
{
  ClutterBinLayout parent;

  double expansion;
  int expand_lines;
};

G_DEFINE_FINAL_TYPE (ShellLabelExpanderLayout, shell_label_expander_layout,
                     CLUTTER_TYPE_BIN_LAYOUT)

static void
shell_label_expander_layout_get_preferred_height (ClutterLayoutManager *manager,
                                                  ClutterActor         *container,
                                                  float                 for_width,
                                                  float                *min_height_p,
                                                  float                *nat_height_p)
{
  ShellLabelExpanderLayout *self = SHELL_LABEL_EXPANDER_LAYOUT (manager);
  ClutterActor *child = clutter_actor_get_first_child (container);
  float min = 0, nat = 0;

  if (child != NULL)
    {
      float nat2, exp_height;

      clutter_actor_get_preferred_height (child, -1, &min, &nat);
      clutter_actor_get_preferred_height (child, for_width, NULL, &nat2);

      exp_height = MIN (nat2, nat * self->expand_lines);
      min = min + self->expansion * (exp_height - min);
      nat = nat + self->expansion * (exp_height - nat);
    }

  if (min_height_p)
    *min_height_p = floorf (min);
  if (nat_height_p)
    *nat_height_p = floorf (nat);
}

static void
shell_label_expander_layout_set_expansion (ShellLabelExpanderLayout *self,
                                           double                    expansion)
{
  if (expansion == self->expansion)
    return;

  self->expansion = expansion;
  g_object_notify_by_pspec (G_OBJECT (self), label_expander_props[LABEL_EXPANDER_PROP_EXPANSION]);

  clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
}

static void
shell_label_expander_layout_get_property (GObject    *object,
                                          guint       prop_id,
                                          GValue     *value,
                                          GParamSpec *pspec)
{
  ShellLabelExpanderLayout *self = SHELL_LABEL_EXPANDER_LAYOUT (object);

  switch (prop_id)
    {
    case LABEL_EXPANDER_PROP_EXPANSION:
      g_value_set_double (value, self->expansion);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_label_expander_layout_set_property (GObject      *object,
                                          guint         prop_id,
                                          const GValue *value,
                                          GParamSpec   *pspec)
{
  ShellLabelExpanderLayout *self = SHELL_LABEL_EXPANDER_LAYOUT (object);

  switch (prop_id)
    {
    case LABEL_EXPANDER_PROP_EXPANSION:
      shell_label_expander_layout_set_expansion (self, g_value_get_double (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_label_expander_layout_class_init (ShellLabelExpanderLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  object_class->get_property = shell_label_expander_layout_get_property;
  object_class->set_property = shell_label_expander_layout_set_property;

  layout_class->get_preferred_height = shell_label_expander_layout_get_preferred_height;

  label_expander_props[LABEL_EXPANDER_PROP_EXPANSION] =
    g_param_spec_double ("expansion", NULL, NULL,
                         0, 1, 0,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LABEL_EXPANDER_N_PROPS, label_expander_props);
}

static void
shell_label_expander_layout_init (ShellLabelExpanderLayout *self)
{
  self->expansion = 0;
  self->expand_lines = DEFAULT_EXPAND_LINES;
}

/* ------------------------------------------------------------------------ */
/* ShellMessageTimeLabel                                                     */
/* ------------------------------------------------------------------------ */

enum
{
  TIME_LABEL_PROP_0,

  TIME_LABEL_PROP_DATETIME,

  TIME_LABEL_N_PROPS
};

static GParamSpec *time_label_props[TIME_LABEL_N_PROPS] = { NULL, };

#define SHELL_TYPE_MESSAGE_TIME_LABEL (shell_message_time_label_get_type ())
G_DECLARE_FINAL_TYPE (ShellMessageTimeLabel, shell_message_time_label,
                      SHELL, MESSAGE_TIME_LABEL, StLabel)

struct _ShellMessageTimeLabel
{
  StLabel parent;

  GDateTime *datetime;
};

G_DEFINE_FINAL_TYPE (ShellMessageTimeLabel, shell_message_time_label, ST_TYPE_LABEL)

static void
time_label_update_text (ShellMessageTimeLabel *self)
{
  if (self->datetime != NULL)
    {
      g_autofree char *text = shell_date_format_time_span (self->datetime);

      st_label_set_text (ST_LABEL (self), text != NULL ? text : "");
    }
}

static void
time_label_set_datetime (ShellMessageTimeLabel *self,
                         GDateTime             *datetime)
{
  if (self->datetime != NULL && datetime != NULL &&
      g_date_time_equal (self->datetime, datetime))
    return;

  g_clear_pointer (&self->datetime, g_date_time_unref);
  if (datetime != NULL)
    self->datetime = g_date_time_ref (datetime);

  set_actor_visible (CLUTTER_ACTOR (self), self->datetime != NULL);

  if (clutter_actor_is_mapped (CLUTTER_ACTOR (self)))
    time_label_update_text (self);

  g_object_notify_by_pspec (G_OBJECT (self), time_label_props[TIME_LABEL_PROP_DATETIME]);
}

static void
shell_message_time_label_map (ClutterActor *actor)
{
  ShellMessageTimeLabel *self = SHELL_MESSAGE_TIME_LABEL (actor);

  time_label_update_text (self);

  CLUTTER_ACTOR_CLASS (shell_message_time_label_parent_class)->map (actor);
}

static void
shell_message_time_label_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellMessageTimeLabel *self = SHELL_MESSAGE_TIME_LABEL (object);

  switch (prop_id)
    {
    case TIME_LABEL_PROP_DATETIME:
      g_value_set_boxed (value, self->datetime);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_message_time_label_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellMessageTimeLabel *self = SHELL_MESSAGE_TIME_LABEL (object);

  switch (prop_id)
    {
    case TIME_LABEL_PROP_DATETIME:
      time_label_set_datetime (self, g_value_get_boxed (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_message_time_label_finalize (GObject *object)
{
  ShellMessageTimeLabel *self = SHELL_MESSAGE_TIME_LABEL (object);

  g_clear_pointer (&self->datetime, g_date_time_unref);

  G_OBJECT_CLASS (shell_message_time_label_parent_class)->finalize (object);
}

static void
shell_message_time_label_class_init (ShellMessageTimeLabelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->get_property = shell_message_time_label_get_property;
  object_class->set_property = shell_message_time_label_set_property;
  object_class->finalize = shell_message_time_label_finalize;

  actor_class->map = shell_message_time_label_map;

  time_label_props[TIME_LABEL_PROP_DATETIME] =
    g_param_spec_boxed ("datetime", NULL, NULL,
                        G_TYPE_DATE_TIME,
                        G_PARAM_READWRITE |
                        G_PARAM_EXPLICIT_NOTIFY |
                        G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, TIME_LABEL_N_PROPS, time_label_props);
}

static void
shell_message_time_label_init (ShellMessageTimeLabel *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  st_widget_set_style_class_name (ST_WIDGET (self), "event-time");
  clutter_actor_set_x_expand (actor, TRUE);
  clutter_actor_set_y_expand (actor, TRUE);
  clutter_actor_set_x_align (actor, CLUTTER_ACTOR_ALIGN_START);
  clutter_actor_set_y_align (actor, CLUTTER_ACTOR_ALIGN_END);
  set_actor_visible (actor, FALSE);
}

/* ------------------------------------------------------------------------ */
/* ShellMessageUrlHighlighter                                                */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MESSAGE_URL_HIGHLIGHTER (shell_message_url_highlighter_get_type ())
G_DECLARE_FINAL_TYPE (ShellMessageUrlHighlighter, shell_message_url_highlighter,
                      SHELL, MESSAGE_URL_HIGHLIGHTER, StLabel)

struct _ShellMessageUrlHighlighter
{
  StLabel parent;

  char *link_color;
  char *text;           /* the text with markup */
  GList *urls;          /* of ShellUrlMatch, found in the text without markup */
  gboolean cursor_changed;
};

G_DEFINE_FINAL_TYPE (ShellMessageUrlHighlighter, shell_message_url_highlighter, ST_TYPE_LABEL)

static void
url_highlighter_highlight_urls (ShellMessageUrlHighlighter *self)
{
  ClutterText *clutter_text = st_label_get_clutter_text (ST_LABEL (self));
  const char *text = self->text != NULL ? self->text : "";
  GList *urls, *l;
  GString *markup;
  gsize pos = 0;

  /* text here contains markup */
  urls = shell_util_find_urls (text);
  markup = g_string_new (NULL);

  for (l = urls; l != NULL; l = l->next)
    {
      ShellUrlMatch *url = l->data;

      g_string_append_len (markup, text + pos, url->byte_pos - pos);
      g_string_append_printf (markup, "<span foreground=\"%s\"><u>%s</u></span>",
                              self->link_color, url->url);
      pos = url->byte_pos + strlen (url->url);
    }
  g_string_append (markup, text + pos);

  clutter_text_set_markup (clutter_text, markup->str);
  g_string_free (markup, TRUE);
  g_list_free_full (urls, (GDestroyNotify) shell_url_match_free);
}

static int
url_highlighter_find_url_at_pos (ShellMessageUrlHighlighter *self,
                                 float                       x,
                                 float                       y)
{
  ClutterText *clutter_text = st_label_get_clutter_text (ST_LABEL (self));
  const char *text = clutter_text_get_text (clutter_text);
  float local_x = x, local_y = y;
  int find_pos = -1;
  glong n_chars;
  glong i;

  clutter_actor_transform_stage_point (CLUTTER_ACTOR (self), x, y, &local_x, &local_y);

  n_chars = g_utf8_strlen (text, -1);
  for (i = 0; i < n_chars; i++)
    {
      float px, py, line_height;

      if (!clutter_text_position_to_coords (clutter_text, i, &px, &py, &line_height))
        continue;

      if (py > local_y || py + line_height < local_y || local_x < px)
        continue;

      find_pos = i;
    }

  if (find_pos != -1)
    {
      GList *l;
      int j;

      for (l = self->urls, j = 0; l != NULL; l = l->next, j++)
        {
          ShellUrlMatch *url = l->data;

          if (find_pos >= url->pos &&
              url->pos + (int) g_utf8_strlen (url->url, -1) > find_pos)
            return j;
        }
    }

  return -1;
}

static void
url_highlighter_set_markup (ShellMessageUrlHighlighter *self,
                            const char                 *text,
                            gboolean                    allow_markup)
{
  ClutterText *clutter_text = st_label_get_clutter_text (ST_LABEL (self));

  g_free (self->text);
  self->text = (text != NULL && *text != '\0') ?
    shell_util_fix_markup (text, allow_markup) : g_strdup ("");

  clutter_text_set_markup (clutter_text, self->text);

  /* clutter_text_get_text () contains the text without markup */
  g_list_free_full (g_steal_pointer (&self->urls), (GDestroyNotify) shell_url_match_free);
  self->urls = shell_util_find_urls (clutter_text_get_text (clutter_text));
  url_highlighter_highlight_urls (self);
}

static gboolean
url_highlighter_check_in_url_cb (ClutterGesture              *gesture,
                                 ShellMessageUrlHighlighter  *self)
{
  graphene_point_t coords;

  clutter_press_gesture_get_coords_abs (CLUTTER_PRESS_GESTURE (gesture), &coords);

  return url_highlighter_find_url_at_pos (self, coords.x, coords.y) != -1;
}

static void
url_highlighter_click_cb (ClutterGesture             *gesture,
                          ShellMessageUrlHighlighter *self)
{
  graphene_point_t coords;
  int url_id;

  clutter_press_gesture_get_coords_abs (CLUTTER_PRESS_GESTURE (gesture), &coords);
  url_id = url_highlighter_find_url_at_pos (self, coords.x, coords.y);

  if (url_id != -1)
    {
      ShellUrlMatch *match = g_list_nth_data (self->urls, url_id);
      g_autofree char *url = NULL;
      g_autoptr (GAppLaunchContext) context = NULL;
      g_autoptr (GError) error = NULL;

      if (strchr (match->url, ':') == NULL)
        url = g_strdup_printf ("http://%s", match->url);
      else
        url = g_strdup (match->url);

      context = shell_global_create_app_launch_context (shell_global_get (), 0, -1);
      if (!g_app_info_launch_default_for_uri (url, context, &error))
        g_warning ("Failed to open %s: %s", url, error->message);
    }
}

static void
url_highlighter_motion_cb (ClutterMotionController     *controller,
                           ClutterSprite               *sprite,
                           float                        x,
                           float                        y,
                           ShellMessageUrlHighlighter  *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  int url_id;

  if (!clutter_actor_is_visible (actor) || clutter_actor_get_paint_opacity (actor) == 0)
    return;

  url_id = url_highlighter_find_url_at_pos (self, x, y);
  if (url_id != -1 && !self->cursor_changed)
    {
      clutter_actor_set_cursor_type (actor, CLUTTER_CURSOR_POINTER);
      self->cursor_changed = TRUE;
    }
  else if (url_id == -1)
    {
      clutter_actor_set_cursor_type (actor, CLUTTER_CURSOR_DEFAULT);
      self->cursor_changed = FALSE;
    }
}

static void
url_highlighter_leave_cb (ClutterMotionController     *controller,
                          ClutterSprite               *sprite,
                          ShellMessageUrlHighlighter  *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  if (!clutter_actor_is_visible (actor) || clutter_actor_get_paint_opacity (actor) == 0)
    return;

  if (self->cursor_changed)
    {
      self->cursor_changed = FALSE;
      clutter_actor_set_cursor_type (actor, CLUTTER_CURSOR_DEFAULT);
    }
}

static void
shell_message_url_highlighter_style_changed (StWidget *widget)
{
  ShellMessageUrlHighlighter *self = SHELL_MESSAGE_URL_HIGHLIGHTER (widget);
  StThemeNode *theme_node = st_widget_get_theme_node (widget);
  CoglColor color;

  ST_WIDGET_CLASS (shell_message_url_highlighter_parent_class)->style_changed (widget);

  if (st_theme_node_lookup_color (theme_node, "link-color", FALSE, &color))
    {
      g_autofree char *color_string = cogl_color_to_string (&color);
      g_autofree char *link_color = g_strndup (color_string, 7);

      if (g_strcmp0 (link_color, self->link_color) != 0)
        {
          g_free (self->link_color);
          self->link_color = g_steal_pointer (&link_color);
          url_highlighter_highlight_urls (self);
        }
    }
}

static void
shell_message_url_highlighter_finalize (GObject *object)
{
  ShellMessageUrlHighlighter *self = SHELL_MESSAGE_URL_HIGHLIGHTER (object);

  g_clear_pointer (&self->link_color, g_free);
  g_clear_pointer (&self->text, g_free);
  g_list_free_full (g_steal_pointer (&self->urls), (GDestroyNotify) shell_url_match_free);

  G_OBJECT_CLASS (shell_message_url_highlighter_parent_class)->finalize (object);
}

static void
shell_message_url_highlighter_constructed (GObject *object)
{
  ShellMessageUrlHighlighter *self = SHELL_MESSAGE_URL_HIGHLIGHTER (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterAction *click_gesture;
  ClutterAction *motion_controller;

  G_OBJECT_CLASS (shell_message_url_highlighter_parent_class)->constructed (object);

  clutter_actor_set_reactive (actor, TRUE);
  st_widget_set_style_class_name (ST_WIDGET (self), "url-highlighter");
  clutter_actor_set_x_expand (actor, TRUE);
  clutter_actor_set_x_align (actor, CLUTTER_ACTOR_ALIGN_START);

  clutter_text_set_line_wrap_mode (st_label_get_clutter_text (ST_LABEL (self)),
                                   PANGO_WRAP_WORD_CHAR);

  click_gesture = clutter_click_gesture_new ();
  g_signal_connect (click_gesture, "recognize",
                    G_CALLBACK (url_highlighter_click_cb), self);
  g_signal_connect (click_gesture, "may-recognize",
                    G_CALLBACK (url_highlighter_check_in_url_cb), self);
  clutter_actor_add_action (actor, click_gesture);

  motion_controller = clutter_motion_controller_new ();
  g_signal_connect (motion_controller, "leave",
                    G_CALLBACK (url_highlighter_leave_cb), self);
  g_signal_connect (motion_controller, "motion",
                    G_CALLBACK (url_highlighter_motion_cb), self);
  clutter_actor_add_action (actor, motion_controller);
}

static void
shell_message_url_highlighter_class_init (ShellMessageUrlHighlighterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);

  object_class->constructed = shell_message_url_highlighter_constructed;
  object_class->finalize = shell_message_url_highlighter_finalize;

  widget_class->style_changed = shell_message_url_highlighter_style_changed;
}

static void
shell_message_url_highlighter_init (ShellMessageUrlHighlighter *self)
{
  self->link_color = g_strdup ("#ccccff");
}

static ClutterActor *
url_highlighter_new (gboolean line_wrap)
{
  ClutterActor *label = g_object_new (SHELL_TYPE_MESSAGE_URL_HIGHLIGHTER, NULL);

  clutter_text_set_line_wrap (st_label_get_clutter_text (ST_LABEL (label)), line_wrap);
  url_highlighter_set_markup (SHELL_MESSAGE_URL_HIGHLIGHTER (label), "", FALSE);

  return label;
}

/* ------------------------------------------------------------------------ */
/* ShellMessage                                                              */
/* ------------------------------------------------------------------------ */

enum
{
  MESSAGE_PROP_0,

  MESSAGE_PROP_SOURCE,
  MESSAGE_PROP_TITLE,
  MESSAGE_PROP_BODY,
  MESSAGE_PROP_USE_BODY_MARKUP,
  MESSAGE_PROP_ICON,
  MESSAGE_PROP_DATETIME,
  MESSAGE_PROP_EXPANDED,

  MESSAGE_N_PROPS
};

static GParamSpec *message_props[MESSAGE_N_PROPS] = { NULL, };

enum
{
  MESSAGE_CLOSE,
  MESSAGE_EXPANDED,
  MESSAGE_UNEXPANDED,

  MESSAGE_LAST_SIGNAL
};

static guint message_signals[MESSAGE_LAST_SIGNAL] = { 0, };

typedef struct
{
  ShellMessageSource *source;
  GBinding *title_binding;
  GBinding *icon_binding;

  gboolean expanded;
  gboolean use_body_markup;
  char *title_text;
  char *body_text;

  StIcon *source_icon;
  StLabel *source_title;
  ShellMessageTimeLabel *time_label;
  StButton *expand_button;
  StButton *close_button;

  StBin *action_bin;
  StIcon *icon;
  StBoxLayout *media_controls;
  StLabel *title_label;
  ClutterActor *body_label;
  StBin *body_bin;
} ShellMessagePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellMessage, shell_message, ST_TYPE_BUTTON)

/**
 * shell_message_can_close:
 * @self: a #ShellMessage
 *
 * Returns: whether the user can close the message (`canClose ()`)
 */
gboolean
shell_message_can_close (ShellMessage *self)
{
  ShellMessageClass *klass;

  g_return_val_if_fail (SHELL_IS_MESSAGE (self), FALSE);

  klass = SHELL_MESSAGE_GET_CLASS (self);
  return klass->can_close (self);
}

static gboolean
shell_message_real_can_close (ShellMessage *self)
{
  return FALSE;
}

/**
 * shell_message_close:
 * @self: a #ShellMessage
 *
 * Asks for the message to be closed: emits #ShellMessage::close.
 */
void
shell_message_close (ShellMessage *self)
{
  g_return_if_fail (SHELL_IS_MESSAGE (self));

  g_signal_emit (self, message_signals[MESSAGE_CLOSE], 0);
}

static void
message_close_if_allowed (ShellMessage *self)
{
  if (shell_message_can_close (self))
    shell_message_close (self);
}

static void
message_update_expand_button (ShellMessage *self)
{
  ShellMessagePrivate *priv = shell_message_get_instance_private (self);
  ClutterText *clutter_text;
  PangoLayout *layout;
  gboolean can_expand;

  if (!clutter_actor_has_allocation (priv->body_label))
    return;

  clutter_text = st_label_get_clutter_text (ST_LABEL (priv->body_label));
  layout = clutter_text_get_layout (clutter_text);
  can_expand = pango_layout_is_ellipsized (layout) ||
               priv->expanded ||
               st_bin_get_child (priv->action_bin) != NULL;

  /* Use opacity to not trigger a relayout */
  clutter_actor_set_opacity (CLUTTER_ACTOR (priv->expand_button), can_expand ? 255 : 0);
}

static void
body_label_allocation_changed_cb (ClutterActor *actor,
                                  GParamSpec   *pspec,
                                  ShellMessage *self)
{
  message_update_expand_button (self);
}

static void
message_source_title_transform (GBinding     *binding,
                                const GValue *from_value,
                                GValue       *to_value,
                                gpointer      user_data)
{
  const char *value = g_value_get_string (from_value);

  /* Translators: this is the string displayed in the header when a message
   * source doesn't have a name */
  g_value_set_string (to_value, (value == NULL || *value == '\0') ? _("Unknown App") : value);
}

static gboolean
message_source_title_transform_to (GBinding     *binding,
                                   const GValue *from_value,
                                   GValue       *to_value,
                                   gpointer      user_data)
{
  message_source_title_transform (binding, from_value, to_value, user_data);
  return TRUE;
}

/**
 * shell_message_get_source:
 * @self: a #ShellMessage
 *
 * Returns: (transfer none) (nullable): #ShellMessage:source
 */
ShellMessageSource *
shell_message_get_source (ShellMessage *self)
{
  ShellMessagePrivate *priv;

  g_return_val_if_fail (SHELL_IS_MESSAGE (self), NULL);

  priv = shell_message_get_instance_private (self);
  return priv->source;
}

static void
message_unbind_source (ShellMessage *self)
{
  ShellMessagePrivate *priv = shell_message_get_instance_private (self);

  if (priv->title_binding != NULL)
    {
      g_binding_unbind (priv->title_binding);
      g_clear_object (&priv->title_binding);
    }

  if (priv->icon_binding != NULL)
    {
      g_binding_unbind (priv->icon_binding);
      g_clear_object (&priv->icon_binding);
    }
}

/**
 * shell_message_set_source:
 * @self: a #ShellMessage
 * @source: (nullable): the source, whose title and icon the header shows
 *
 * Sets #ShellMessage:source.
 */
void
shell_message_set_source (ShellMessage       *self,
                          ShellMessageSource *source)
{
  ShellMessagePrivate *priv;

  g_return_if_fail (SHELL_IS_MESSAGE (self));
  g_return_if_fail (source == NULL || SHELL_IS_MESSAGE_SOURCE (source));

  priv = shell_message_get_instance_private (self);

  if (priv->source == source)
    return;

  message_unbind_source (self);
  g_set_object (&priv->source, source);

  if (source != NULL)
    {
      priv->title_binding =
        g_object_ref (g_object_bind_property_full (source, "title",
                                                  priv->source_title, "text",
                                                  G_BINDING_SYNC_CREATE,
                                                  message_source_title_transform_to,
                                                  NULL, NULL, NULL));
      priv->icon_binding =
        g_object_ref (g_object_bind_property (source, "icon",
                                              priv->source_icon, "gicon",
                                              G_BINDING_SYNC_CREATE));
    }

  g_object_notify_by_pspec (G_OBJECT (self), message_props[MESSAGE_PROP_SOURCE]);
}

/**
 * shell_message_get_icon:
 * @self: a #ShellMessage
 *
 * Returns: (transfer none) (nullable): #ShellMessage:icon
 */
GIcon *
shell_message_get_icon (ShellMessage *self)
{
  ShellMessagePrivate *priv;

  g_return_val_if_fail (SHELL_IS_MESSAGE (self), NULL);

  priv = shell_message_get_instance_private (self);
  return st_icon_get_gicon (priv->icon);
}

/**
 * shell_message_set_icon:
 * @self: a #ShellMessage
 * @icon: (nullable): the icon of the message
 *
 * Sets #ShellMessage:icon.
 */
void
shell_message_set_icon (ShellMessage *self,
                        GIcon        *icon)
{
  ShellMessagePrivate *priv;

  g_return_if_fail (SHELL_IS_MESSAGE (self));
  g_return_if_fail (icon == NULL || G_IS_ICON (icon));

  priv = shell_message_get_instance_private (self);

  st_icon_set_gicon (priv->icon, icon);
  set_actor_visible (CLUTTER_ACTOR (priv->icon), icon != NULL);
  g_object_notify_by_pspec (G_OBJECT (self), message_props[MESSAGE_PROP_ICON]);
}

/**
 * shell_message_get_datetime:
 * @self: a #ShellMessage
 *
 * Returns: (transfer none) (nullable): #ShellMessage:datetime
 */
GDateTime *
shell_message_get_datetime (ShellMessage *self)
{
  ShellMessagePrivate *priv;

  g_return_val_if_fail (SHELL_IS_MESSAGE (self), NULL);

  priv = shell_message_get_instance_private (self);
  return priv->time_label->datetime;
}

/**
 * shell_message_set_datetime:
 * @self: a #ShellMessage
 * @datetime: (nullable): the time shown in the header, as a time span
 *
 * Sets #ShellMessage:datetime.
 */
void
shell_message_set_datetime (ShellMessage *self,
                            GDateTime    *datetime)
{
  ShellMessagePrivate *priv;

  g_return_if_fail (SHELL_IS_MESSAGE (self));

  priv = shell_message_get_instance_private (self);

  time_label_set_datetime (priv->time_label, datetime);
  g_object_notify_by_pspec (G_OBJECT (self), message_props[MESSAGE_PROP_DATETIME]);
}

/**
 * shell_message_get_title:
 * @self: a #ShellMessage
 *
 * Returns: (nullable): #ShellMessage:title, which is limited to 150 characters
 */
const char *
shell_message_get_title (ShellMessage *self)
{
  ShellMessagePrivate *priv;

  g_return_val_if_fail (SHELL_IS_MESSAGE (self), NULL);

  priv = shell_message_get_instance_private (self);
  return priv->title_text;
}

/**
 * shell_message_set_title:
 * @self: a #ShellMessage
 * @title: (nullable): the title of the message
 *
 * Sets #ShellMessage:title.
 */
void
shell_message_set_title (ShellMessage *self,
                         const char   *title)
{
  ShellMessagePrivate *priv;
  g_autofree char *single_line = NULL;
  g_autofree char *markup = NULL;

  g_return_if_fail (SHELL_IS_MESSAGE (self));

  priv = shell_message_get_instance_private (self);

  g_free (priv->title_text);
  priv->title_text = (title != NULL && *title != '\0') ? limit_string (title, MAX_TITLE_LENGTH) : NULL;

  if (priv->title_text != NULL)
    {
      single_line = replace_newlines (priv->title_text);
      markup = shell_util_fix_markup (single_line, FALSE);
    }

  clutter_text_set_markup (st_label_get_clutter_text (priv->title_label),
                           markup != NULL ? markup : "");
  g_object_notify_by_pspec (G_OBJECT (self), message_props[MESSAGE_PROP_TITLE]);
}

/**
 * shell_message_get_body:
 * @self: a #ShellMessage
 *
 * Returns: (nullable): #ShellMessage:body, which is limited to 850 characters
 */
const char *
shell_message_get_body (ShellMessage *self)
{
  ShellMessagePrivate *priv;

  g_return_val_if_fail (SHELL_IS_MESSAGE (self), NULL);

  priv = shell_message_get_instance_private (self);
  return priv->body_text;
}

/**
 * shell_message_set_body:
 * @self: a #ShellMessage
 * @body: (nullable): the body of the message
 *
 * Sets #ShellMessage:body.
 */
void
shell_message_set_body (ShellMessage *self,
                        const char   *body)
{
  ShellMessagePrivate *priv;
  g_autofree char *single_line = NULL;

  g_return_if_fail (SHELL_IS_MESSAGE (self));

  priv = shell_message_get_instance_private (self);

  g_free (priv->body_text);
  priv->body_text = (body != NULL && *body != '\0') ? limit_string (body, MAX_BODY_LENGTH) : NULL;

  if (priv->body_text != NULL)
    single_line = replace_newlines (priv->body_text);

  url_highlighter_set_markup (SHELL_MESSAGE_URL_HIGHLIGHTER (priv->body_label),
                              single_line != NULL ? single_line : "",
                              priv->use_body_markup);
  g_object_notify_by_pspec (G_OBJECT (self), message_props[MESSAGE_PROP_BODY]);
}

/**
 * shell_message_get_use_body_markup:
 * @self: a #ShellMessage
 *
 * Returns: #ShellMessage:use-body-markup
 */
gboolean
shell_message_get_use_body_markup (ShellMessage *self)
{
  ShellMessagePrivate *priv;

  g_return_val_if_fail (SHELL_IS_MESSAGE (self), FALSE);

  priv = shell_message_get_instance_private (self);
  return priv->use_body_markup;
}

/**
 * shell_message_set_use_body_markup:
 * @self: a #ShellMessage
 * @use_body_markup: whether the body contains markup
 *
 * Sets #ShellMessage:use-body-markup.
 */
void
shell_message_set_use_body_markup (ShellMessage *self,
                                   gboolean      use_body_markup)
{
  ShellMessagePrivate *priv;
  g_autofree char *body = NULL;

  g_return_if_fail (SHELL_IS_MESSAGE (self));

  priv = shell_message_get_instance_private (self);

  use_body_markup = !!use_body_markup;
  if (priv->use_body_markup == use_body_markup)
    return;

  priv->use_body_markup = use_body_markup;

  body = g_strdup (priv->body_text);
  shell_message_set_body (self, body);

  g_object_notify_by_pspec (G_OBJECT (self), message_props[MESSAGE_PROP_USE_BODY_MARKUP]);
}

/**
 * shell_message_get_expanded:
 * @self: a #ShellMessage
 *
 * Returns: #ShellMessage:expanded
 */
gboolean
shell_message_get_expanded (ShellMessage *self)
{
  ShellMessagePrivate *priv;

  g_return_val_if_fail (SHELL_IS_MESSAGE (self), FALSE);

  priv = shell_message_get_instance_private (self);
  return priv->expanded;
}

static void
message_set_expanded (ShellMessage *self,
                      gboolean      expanded)
{
  ShellMessagePrivate *priv = shell_message_get_instance_private (self);

  if (priv->expanded == expanded)
    return;

  priv->expanded = expanded;
  g_object_notify_by_pspec (G_OBJECT (self), message_props[MESSAGE_PROP_EXPANDED]);
}

/**
 * shell_message_set_action_area:
 * @self: a #ShellMessage
 * @actor: (nullable): the actor shown below the message when it is expanded
 *
 * Sets the action area of the message (`setActionArea ()`).
 */
void
shell_message_set_action_area (ShellMessage *self,
                               ClutterActor *actor)
{
  ShellMessagePrivate *priv;

  g_return_if_fail (SHELL_IS_MESSAGE (self));
  g_return_if_fail (actor == NULL || CLUTTER_IS_ACTOR (actor));

  priv = shell_message_get_instance_private (self);

  st_bin_set_child (priv->action_bin, actor);
  set_actor_visible (CLUTTER_ACTOR (priv->action_bin),
                             actor != NULL && priv->expanded);
  message_update_expand_button (self);
}

/**
 * shell_message_add_media_control:
 * @self: a #ShellMessage
 * @icon_name: the name of the icon of the button
 *
 * Adds a button to the media controls of the message
 * (`addMediaControl ()`). Connect to its `clicked` signal to react.
 *
 * Returns: (transfer none): the new button
 */
StButton *
shell_message_add_media_control (ShellMessage *self,
                                 const char   *icon_name)
{
  ShellMessagePrivate *priv;
  ClutterActor *button;

  g_return_val_if_fail (SHELL_IS_MESSAGE (self), NULL);

  priv = shell_message_get_instance_private (self);

  button = g_object_new (ST_TYPE_BUTTON,
                         "style-class", "message-media-control",
                         "icon-name", icon_name,
                         NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (priv->media_controls), button);

  return ST_BUTTON (button);
}

/**
 * shell_message_set_expand_button_visible:
 * @self: a #ShellMessage
 * @visible: whether the header shows the button that expands the message
 *
 * Shows or hides the expand button of the header. The banners hide it.
 */
void
shell_message_set_expand_button_visible (ShellMessage *self,
                                         gboolean      visible)
{
  ShellMessagePrivate *priv;

  g_return_if_fail (SHELL_IS_MESSAGE (self));

  priv = shell_message_get_instance_private (self);

  set_actor_visible (CLUTTER_ACTOR (priv->expand_button), visible);
}

static void
unexpand_complete_cb (gboolean finished,
                      gpointer user_data)
{
  ShellMessage *self = user_data;
  ShellMessagePrivate *priv = shell_message_get_instance_private (self);

  /* onComplete */
  if (!finished)
    return;

  clutter_actor_hide (CLUTTER_ACTOR (priv->action_bin));
  message_set_expanded (self, FALSE);
}

/**
 * shell_message_expand:
 * @self: a #ShellMessage
 * @animate: whether to animate the change
 *
 * Expands the message: the body shows more lines and the action area shows.
 */
void
shell_message_expand (ShellMessage *self,
                      gboolean      animate)
{
  ShellMessagePrivate *priv;
  ShellEaseParams params;
  int duration = animate ? SHELL_MESSAGE_TRAY_ANIMATION_TIME : 0;

  g_return_if_fail (SHELL_IS_MESSAGE (self));

  priv = shell_message_get_instance_private (self);

  if (priv->expanded)
    return;

  message_set_expanded (self, TRUE);

  set_actor_visible (CLUTTER_ACTOR (priv->action_bin),
                             st_bin_get_child (priv->action_bin) != NULL);

  ease_actor_property (CLUTTER_ACTOR (priv->body_bin), "@layout.expansion", 1.0,
                       duration, CLUTTER_EASE_OUT_QUAD, NULL, NULL, NULL);

  g_object_set (priv->action_bin, "scale-y", 0.0, NULL);
  init_ease_params (&params, duration, -1, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (CLUTTER_ACTOR (priv->action_bin), &params,
                    NULL, NULL, NULL,
                    "scale-y", 1.0,
                    NULL);

  init_ease_params (&params, duration, -1, 0);
  shell_actor_ease (CLUTTER_ACTOR (priv->expand_button), &params,
                    NULL, NULL, NULL,
                    "rotation-angle-z", 180.0,
                    NULL);

  g_signal_emit (self, message_signals[MESSAGE_EXPANDED], 0);
}

/**
 * shell_message_unexpand:
 * @self: a #ShellMessage
 * @animate: whether to animate the change
 *
 * Undoes shell_message_expand ().
 */
void
shell_message_unexpand (ShellMessage *self,
                        gboolean      animate)
{
  ShellMessagePrivate *priv;
  ShellEaseParams params;
  int duration = animate ? SHELL_MESSAGE_TRAY_ANIMATION_TIME : 0;

  g_return_if_fail (SHELL_IS_MESSAGE (self));

  priv = shell_message_get_instance_private (self);

  ease_actor_property (CLUTTER_ACTOR (priv->body_bin), "@layout.expansion", 0.0,
                       duration, CLUTTER_EASE_OUT_QUAD, NULL, NULL, NULL);

  init_ease_params (&params, duration, -1, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (CLUTTER_ACTOR (priv->action_bin), &params,
                    unexpand_complete_cb, g_object_ref (self), g_object_unref,
                    "scale-y", 0.0,
                    NULL);

  init_ease_params (&params, duration, -1, 0);
  shell_actor_ease (CLUTTER_ACTOR (priv->expand_button), &params,
                    NULL, NULL, NULL,
                    "rotation-angle-z", 0.0,
                    NULL);

  g_signal_emit (self, message_signals[MESSAGE_UNEXPANDED], 0);
}

static void
close_button_clicked_cb (StButton     *button,
                         int           clicked_button,
                         ShellMessage *self)
{
  shell_message_close (self);
}

static void
expand_button_clicked_cb (StButton     *button,
                          int           clicked_button,
                          ShellMessage *self)
{
  ShellMessagePrivate *priv = shell_message_get_instance_private (self);

  if (priv->expanded)
    shell_message_unexpand (self, TRUE);
  else
    shell_message_expand (self, TRUE);
}

static void
icon_is_symbolic_changed_cb (StIcon       *icon,
                             GParamSpec   *pspec,
                             ShellMessage *self)
{
  if (st_icon_get_is_symbolic (icon))
    st_widget_add_style_class_name (ST_WIDGET (icon), "message-themed-icon");
  else
    st_widget_remove_style_class_name (ST_WIDGET (icon), "message-themed-icon");
}

static void
source_icon_style_changed_cb (StWidget            *icon,
                              ClutterEffect       *effect)
{
  StThemeNode *theme_node = st_widget_get_theme_node (icon);

  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (effect),
                                  st_theme_node_get_icon_style (theme_node) == ST_ICON_STYLE_SYMBOLIC);
}

static gboolean
close_binding_cb (GObject             *object,
                  const char          *action_name,
                  guint                key_val,
                  ClutterModifierType  modifiers,
                  gpointer             user_data)
{
  message_close_if_allowed (SHELL_MESSAGE (object));

  return CLUTTER_EVENT_STOP;
}

static void
shell_message_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  ShellMessage *self = SHELL_MESSAGE (object);

  switch (prop_id)
    {
    case MESSAGE_PROP_SOURCE:
      g_value_set_object (value, shell_message_get_source (self));
      break;

    case MESSAGE_PROP_TITLE:
      g_value_set_string (value, shell_message_get_title (self));
      break;

    case MESSAGE_PROP_BODY:
      g_value_set_string (value, shell_message_get_body (self));
      break;

    case MESSAGE_PROP_USE_BODY_MARKUP:
      g_value_set_boolean (value, shell_message_get_use_body_markup (self));
      break;

    case MESSAGE_PROP_ICON:
      g_value_set_object (value, shell_message_get_icon (self));
      break;

    case MESSAGE_PROP_DATETIME:
      g_value_set_boxed (value, shell_message_get_datetime (self));
      break;

    case MESSAGE_PROP_EXPANDED:
      g_value_set_boolean (value, shell_message_get_expanded (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_message_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  ShellMessage *self = SHELL_MESSAGE (object);

  switch (prop_id)
    {
    case MESSAGE_PROP_SOURCE:
      shell_message_set_source (self, g_value_get_object (value));
      break;

    case MESSAGE_PROP_TITLE:
      shell_message_set_title (self, g_value_get_string (value));
      break;

    case MESSAGE_PROP_BODY:
      shell_message_set_body (self, g_value_get_string (value));
      break;

    case MESSAGE_PROP_USE_BODY_MARKUP:
      shell_message_set_use_body_markup (self, g_value_get_boolean (value));
      break;

    case MESSAGE_PROP_ICON:
      shell_message_set_icon (self, g_value_get_object (value));
      break;

    case MESSAGE_PROP_DATETIME:
      shell_message_set_datetime (self, g_value_get_boxed (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_message_constructed (GObject *object)
{
  ShellMessage *self = SHELL_MESSAGE (object);
  ShellMessagePrivate *priv = shell_message_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *vbox, *header, *hbox, *header_content, *content_box;
  ClutterEffect *source_icon_effect;

  G_OBJECT_CLASS (shell_message_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (self), "message");
  clutter_actor_set_accessible_role (actor, ATK_ROLE_NOTIFICATION);
  st_widget_set_can_focus (ST_WIDGET (self), TRUE);
  clutter_actor_set_x_expand (actor, TRUE);
  clutter_actor_set_y_expand (actor, FALSE);

  priv->expanded = FALSE;
  priv->use_body_markup = FALSE;

  vbox = g_object_new (ST_TYPE_BOX_LAYOUT,
                       "orientation", CLUTTER_ORIENTATION_VERTICAL,
                       "x-expand", TRUE,
                       NULL);
  st_bin_set_child (ST_BIN (self), vbox);

  /* The header: the icon and title of the source, the time, and the buttons */
  header = g_object_new (ST_TYPE_BOX_LAYOUT,
                         "style-class", "message-header",
                         "x-expand", TRUE,
                         NULL);
  clutter_actor_add_child (vbox, header);

  source_icon_effect = clutter_desaturate_effect_new (1.0);
  priv->source_icon = g_object_new (ST_TYPE_ICON,
                                    "style-class", "message-source-icon",
                                    "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                    "fallback-icon-name", "application-x-executable-symbolic",
                                    NULL);
  clutter_actor_add_effect (CLUTTER_ACTOR (priv->source_icon), source_icon_effect);
  clutter_actor_add_child (header, CLUTTER_ACTOR (priv->source_icon));

  g_signal_connect (priv->source_icon, "style-changed",
                    G_CALLBACK (source_icon_style_changed_cb), source_icon_effect);

  header_content = g_object_new (ST_TYPE_BOX_LAYOUT,
                                 "style-class", "message-header-content",
                                 "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                 "x-expand", TRUE,
                                 NULL);
  clutter_actor_add_child (header, header_content);

  priv->expand_button = g_object_new (ST_TYPE_BUTTON,
                                      "style-class", "message-expand-button",
                                      "icon-name", "notification-expand-symbolic",
                                      "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                      NULL);
  clutter_actor_set_pivot_point (CLUTTER_ACTOR (priv->expand_button), 0.5, 0.5);
  clutter_actor_add_child (header, CLUTTER_ACTOR (priv->expand_button));

  priv->close_button = g_object_new (ST_TYPE_BUTTON,
                                     "style-class", "message-close-button",
                                     "icon-name", "window-close-symbolic",
                                     "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                     NULL);
  clutter_actor_add_child (header, CLUTTER_ACTOR (priv->close_button));

  priv->source_title = g_object_new (ST_TYPE_LABEL,
                                     "style-class", "message-source-title",
                                     "y-align", CLUTTER_ACTOR_ALIGN_END,
                                     NULL);
  clutter_actor_add_child (header_content, CLUTTER_ACTOR (priv->source_title));

  priv->time_label = g_object_new (SHELL_TYPE_MESSAGE_TIME_LABEL, NULL);
  clutter_actor_add_child (header_content, CLUTTER_ACTOR (priv->time_label));

  /* The message itself */
  hbox = g_object_new (ST_TYPE_BOX_LAYOUT,
                       "style-class", "message-box",
                       NULL);
  clutter_actor_add_child (vbox, hbox);

  priv->action_bin = g_object_new (ST_TYPE_BIN,
                                   "style-class", "message-action-bin",
                                   "visible", FALSE,
                                   NULL);
  clutter_actor_set_layout_manager (CLUTTER_ACTOR (priv->action_bin), shell_scale_layout_new ());
  clutter_actor_add_child (vbox, CLUTTER_ACTOR (priv->action_bin));

  priv->icon = g_object_new (ST_TYPE_ICON,
                             "style-class", "message-icon",
                             "y-expand", TRUE,
                             "y-align", CLUTTER_ACTOR_ALIGN_START,
                             "visible", FALSE,
                             NULL);
  clutter_actor_add_child (hbox, CLUTTER_ACTOR (priv->icon));

  g_signal_connect (priv->icon, "notify::is-symbolic",
                    G_CALLBACK (icon_is_symbolic_changed_cb), self);

  content_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                              "style-class", "message-content",
                              "orientation", CLUTTER_ORIENTATION_VERTICAL,
                              "x-expand", TRUE,
                              NULL);
  clutter_actor_add_child (hbox, content_box);

  priv->media_controls = g_object_new (ST_TYPE_BOX_LAYOUT, NULL);
  clutter_actor_add_child (hbox, CLUTTER_ACTOR (priv->media_controls));

  priv->title_label = g_object_new (ST_TYPE_LABEL,
                                    "style-class", "message-title",
                                    "y-align", CLUTTER_ACTOR_ALIGN_END,
                                    NULL);
  clutter_actor_add_child (content_box, CLUTTER_ACTOR (priv->title_label));

  priv->body_label = url_highlighter_new (TRUE);
  st_widget_add_style_class_name (ST_WIDGET (priv->body_label), "message-body");
  priv->body_bin = g_object_new (ST_TYPE_BIN,
                                 "x-expand", TRUE,
                                 NULL);
  clutter_actor_set_layout_manager (CLUTTER_ACTOR (priv->body_bin),
                                    g_object_new (SHELL_TYPE_LABEL_EXPANDER_LAYOUT, NULL));
  st_bin_set_child (priv->body_bin, priv->body_label);
  clutter_actor_add_child (content_box, CLUTTER_ACTOR (priv->body_bin));

  g_signal_connect (priv->close_button, "clicked",
                    G_CALLBACK (close_button_clicked_cb), self);
  set_actor_visible (CLUTTER_ACTOR (priv->close_button),
                             shell_message_can_close (self));

  g_signal_connect (priv->expand_button, "clicked",
                    G_CALLBACK (expand_button_clicked_cb), self);
  g_signal_connect (priv->body_label, "notify::allocation",
                    G_CALLBACK (body_label_allocation_changed_cb), self);
  message_update_expand_button (self);
}

static void
shell_message_dispose (GObject *object)
{
  ShellMessage *self = SHELL_MESSAGE (object);
  ShellMessagePrivate *priv = shell_message_get_instance_private (self);

  message_unbind_source (self);
  g_clear_object (&priv->source);

  G_OBJECT_CLASS (shell_message_parent_class)->dispose (object);
}

static void
shell_message_finalize (GObject *object)
{
  ShellMessage *self = SHELL_MESSAGE (object);
  ShellMessagePrivate *priv = shell_message_get_instance_private (self);

  g_clear_pointer (&priv->title_text, g_free);
  g_clear_pointer (&priv->body_text, g_free);

  G_OBJECT_CLASS (shell_message_parent_class)->finalize (object);
}

static void
shell_message_class_init (ShellMessageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterBindingPool *binding_pool;
  const GParamFlags rw_flags = G_PARAM_READWRITE |
                               G_PARAM_EXPLICIT_NOTIFY |
                               G_PARAM_STATIC_STRINGS;

  object_class->get_property = shell_message_get_property;
  object_class->set_property = shell_message_set_property;
  object_class->constructed = shell_message_constructed;
  object_class->dispose = shell_message_dispose;
  object_class->finalize = shell_message_finalize;

  klass->can_close = shell_message_real_can_close;

  /**
   * ShellMessage:source:
   *
   * The source whose title and icon the header shows.
   */
  message_props[MESSAGE_PROP_SOURCE] =
    g_param_spec_object ("source", NULL, NULL,
                         SHELL_TYPE_MESSAGE_SOURCE,
                         rw_flags);

  /**
   * ShellMessage:title:
   *
   * The title of the message.
   */
  message_props[MESSAGE_PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL, NULL, rw_flags);

  /**
   * ShellMessage:body:
   *
   * The body of the message.
   */
  message_props[MESSAGE_PROP_BODY] =
    g_param_spec_string ("body", NULL, NULL, NULL, rw_flags);

  /**
   * ShellMessage:use-body-markup:
   *
   * Whether the body contains markup.
   */
  message_props[MESSAGE_PROP_USE_BODY_MARKUP] =
    g_param_spec_boolean ("use-body-markup", NULL, NULL, FALSE, rw_flags);

  /**
   * ShellMessage:icon:
   *
   * The icon of the message.
   */
  message_props[MESSAGE_PROP_ICON] =
    g_param_spec_object ("icon", NULL, NULL, G_TYPE_ICON, rw_flags);

  /**
   * ShellMessage:datetime:
   *
   * The time of the message.
   */
  message_props[MESSAGE_PROP_DATETIME] =
    g_param_spec_boxed ("datetime", NULL, NULL, G_TYPE_DATE_TIME, rw_flags);

  /**
   * ShellMessage:expanded:
   *
   * Whether the message is expanded.
   */
  message_props[MESSAGE_PROP_EXPANDED] =
    g_param_spec_boolean ("expanded", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, MESSAGE_N_PROPS, message_props);

  /**
   * ShellMessage::close:
   * @self: the message
   *
   * The message wants to be closed.
   */
  message_signals[MESSAGE_CLOSE] =
    g_signal_new ("close",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (ShellMessageClass, close),
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellMessage::expanded:
   * @self: the message
   *
   * The message was expanded.
   */
  message_signals[MESSAGE_EXPANDED] =
    g_signal_new ("expanded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellMessage::unexpanded:
   * @self: the message
   *
   * The message was unexpanded.
   */
  message_signals[MESSAGE_UNEXPANDED] =
    g_signal_new ("unexpanded",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  binding_pool = clutter_binding_pool_get_for_class (klass);
  clutter_binding_pool_install_action (binding_pool, "close",
                                       CLUTTER_KEY_BackSpace, 0,
                                       G_CALLBACK (close_binding_cb), NULL, NULL);
  clutter_binding_pool_install_action (binding_pool, "close",
                                       CLUTTER_KEY_Delete, 0,
                                       G_CALLBACK (close_binding_cb), NULL, NULL);
}

static void
shell_message_init (ShellMessage *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellNotificationMessage                                                  */
/* ------------------------------------------------------------------------ */

enum
{
  NOTIFICATION_MESSAGE_PROP_0,

  NOTIFICATION_MESSAGE_PROP_NOTIFICATION,

  NOTIFICATION_MESSAGE_N_PROPS
};

static GParamSpec *notification_message_props[NOTIFICATION_MESSAGE_N_PROPS] = { NULL, };

struct _ShellNotificationMessage
{
  ShellMessage parent;

  ShellNotification *notification;
  GHashTable *actions;  /* ShellNotificationAction -> StButton (weak) */
  StBoxLayout *button_box;
  gboolean closed;
};

G_DEFINE_FINAL_TYPE (ShellNotificationMessage, shell_notification_message, SHELL_TYPE_MESSAGE)

/**
 * shell_notification_message_new:
 * @notification: the notification to show
 *
 * Creates the message of @notification. It follows the notification and is
 * closed when the notification is destroyed.
 *
 * Returns: (transfer floating): a new #ShellNotificationMessage
 */
ShellNotificationMessage *
shell_notification_message_new (ShellNotification *notification)
{
  g_return_val_if_fail (SHELL_IS_NOTIFICATION (notification), NULL);

  return g_object_new (SHELL_TYPE_NOTIFICATION_MESSAGE,
                       "notification", notification,
                       NULL);
}

/**
 * shell_notification_message_get_notification:
 * @self: a #ShellNotificationMessage
 *
 * Returns: (transfer none) (nullable): the notification of the message, or
 *   %NULL once it was destroyed
 */
ShellNotification *
shell_notification_message_get_notification (ShellNotificationMessage *self)
{
  g_return_val_if_fail (SHELL_IS_NOTIFICATION_MESSAGE (self), NULL);

  return self->notification;
}

static void
action_button_clicked_cb (StButton                *button,
                          int                      clicked_button,
                          ShellNotificationAction *action)
{
  shell_notification_action_activate (action);
}

static void
action_button_destroy_cb (ClutterActor             *button,
                          ShellNotificationMessage *self)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, self->actions);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      if (value == button)
        {
          g_hash_table_iter_remove (&iter);
          return;
        }
    }
}

static void
notification_message_add_action (ShellNotificationMessage *self,
                                 ShellNotificationAction  *action)
{
  ClutterActor *button;

  if (self->button_box == NULL)
    {
      self->button_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                       "x-expand", TRUE,
                                       "style-class", "notification-buttons-bin",
                                       NULL);
      shell_message_set_action_area (SHELL_MESSAGE (self), CLUTTER_ACTOR (self->button_box));
      st_focus_manager_add_group (shell_global_get_focus_manager (shell_global_get ()),
                                  ST_WIDGET (self->button_box));
    }

  if (clutter_actor_get_n_children (CLUTTER_ACTOR (self->button_box)) >= MAX_NOTIFICATION_BUTTONS)
    return;

  button = g_object_new (ST_TYPE_BUTTON,
                         "style-class", "notification-button",
                         "x-expand", TRUE,
                         "label", shell_notification_action_get_label (action),
                         NULL);

  g_signal_connect_data (button, "clicked",
                         G_CALLBACK (action_button_clicked_cb),
                         g_object_ref (action),
                         (GClosureNotify) g_object_unref, 0);
  g_signal_connect (button, "destroy",
                    G_CALLBACK (action_button_destroy_cb), self);

  g_hash_table_insert (self->actions, g_object_ref (action), button);
  clutter_actor_add_child (CLUTTER_ACTOR (self->button_box), button);
}

static void
notification_message_remove_action (ShellNotificationMessage *self,
                                    ShellNotificationAction  *action)
{
  ClutterActor *button = g_hash_table_lookup (self->actions, action);

  if (button != NULL)
    {
      g_signal_handlers_disconnect_by_func (button, action_button_destroy_cb, self);
      g_hash_table_remove (self->actions, action);
      clutter_actor_destroy (button);
    }
}

static void
notification_action_added_cb (ShellNotification         *notification,
                              ShellNotificationAction   *action,
                              ShellNotificationMessage  *self)
{
  notification_message_add_action (self, action);
}

static void
notification_action_removed_cb (ShellNotification         *notification,
                                ShellNotificationAction   *action,
                                ShellNotificationMessage  *self)
{
  notification_message_remove_action (self, action);
}

static void
notification_destroy_cb (ShellNotification                *notification,
                         ShellNotificationDestroyedReason  reason,
                         ShellNotificationMessage         *self)
{
  g_signal_handlers_disconnect_by_data (notification, self);
  g_clear_object (&self->notification);

  if (!self->closed)
    shell_message_close (SHELL_MESSAGE (self));
}

static void
shell_notification_message_close (ShellMessage *message)
{
  ShellNotificationMessage *self = SHELL_NOTIFICATION_MESSAGE (message);
  g_autoptr (ShellNotification) notification = NULL;

  self->closed = TRUE;

  if (self->notification != NULL)
    notification = g_object_ref (self->notification);

  if (notification != NULL)
    shell_notification_destroy (notification, SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);
}

static gboolean
shell_notification_message_can_close (ShellMessage *message)
{
  return TRUE;
}

static void
shell_notification_message_clicked (StButton *button,
                                    int       clicked_button)
{
  ShellNotificationMessage *self = SHELL_NOTIFICATION_MESSAGE (button);
  g_autoptr (ShellNotification) notification = NULL;

  if (self->notification == NULL)
    return;

  notification = g_object_ref (self->notification);
  shell_notification_activate (notification);
}

static void
shell_notification_message_constructed (GObject *object)
{
  ShellNotificationMessage *self = SHELL_NOTIFICATION_MESSAGE (object);
  g_autoptr (GList) actions = NULL;
  GList *l;

  G_OBJECT_CLASS (shell_notification_message_parent_class)->constructed (object);

  if (self->notification == NULL)
    return;

  shell_message_set_source (SHELL_MESSAGE (self),
                            SHELL_MESSAGE_SOURCE (shell_notification_get_source (self->notification)));

  g_signal_connect (self->notification, "action-added",
                    G_CALLBACK (notification_action_added_cb), self);
  g_signal_connect (self->notification, "action-removed",
                    G_CALLBACK (notification_action_removed_cb), self);
  g_signal_connect (self->notification, "destroy",
                    G_CALLBACK (notification_destroy_cb), self);

  g_object_bind_property (self->notification, "title", self, "title", G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->notification, "body", self, "body", G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->notification, "use-body-markup",
                          self, "use-body-markup", G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->notification, "datetime",
                          self, "datetime", G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->notification, "gicon", self, "icon", G_BINDING_SYNC_CREATE);

  actions = shell_notification_get_actions (self->notification);
  for (l = actions; l != NULL; l = l->next)
    notification_message_add_action (self, l->data);
}

static void
shell_notification_message_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  ShellNotificationMessage *self = SHELL_NOTIFICATION_MESSAGE (object);

  switch (prop_id)
    {
    case NOTIFICATION_MESSAGE_PROP_NOTIFICATION:
      g_value_set_object (value, self->notification);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_notification_message_set_property (GObject      *object,
                                         guint         prop_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
  ShellNotificationMessage *self = SHELL_NOTIFICATION_MESSAGE (object);

  switch (prop_id)
    {
    case NOTIFICATION_MESSAGE_PROP_NOTIFICATION:
      g_set_object (&self->notification, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_notification_message_dispose (GObject *object)
{
  ShellNotificationMessage *self = SHELL_NOTIFICATION_MESSAGE (object);

  if (self->notification != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->notification, self);
      g_clear_object (&self->notification);
    }

  g_hash_table_remove_all (self->actions);

  G_OBJECT_CLASS (shell_notification_message_parent_class)->dispose (object);
}

static void
shell_notification_message_finalize (GObject *object)
{
  ShellNotificationMessage *self = SHELL_NOTIFICATION_MESSAGE (object);

  g_clear_pointer (&self->actions, g_hash_table_unref);

  G_OBJECT_CLASS (shell_notification_message_parent_class)->finalize (object);
}

static void
shell_notification_message_class_init (ShellNotificationMessageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);
  ShellMessageClass *message_class = SHELL_MESSAGE_CLASS (klass);

  object_class->get_property = shell_notification_message_get_property;
  object_class->set_property = shell_notification_message_set_property;
  object_class->constructed = shell_notification_message_constructed;
  object_class->dispose = shell_notification_message_dispose;
  object_class->finalize = shell_notification_message_finalize;

  button_class->clicked = shell_notification_message_clicked;

  message_class->can_close = shell_notification_message_can_close;
  message_class->close = shell_notification_message_close;

  /**
   * ShellNotificationMessage:notification:
   *
   * The notification the message shows.
   */
  notification_message_props[NOTIFICATION_MESSAGE_PROP_NOTIFICATION] =
    g_param_spec_object ("notification", NULL, NULL,
                         SHELL_TYPE_NOTIFICATION,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NOTIFICATION_MESSAGE_N_PROPS,
                                     notification_message_props);
}

static void
shell_notification_message_init (ShellNotificationMessage *self)
{
  self->actions = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                         g_object_unref, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellGroupExpanderLayout                                                  */
/* ------------------------------------------------------------------------ */

enum
{
  GROUP_LAYOUT_PROP_0,

  GROUP_LAYOUT_PROP_EXPANSION,

  GROUP_LAYOUT_N_PROPS
};

static GParamSpec *group_layout_props[GROUP_LAYOUT_N_PROPS] = { NULL, };

#define SHELL_TYPE_GROUP_EXPANDER_LAYOUT (shell_group_expander_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellGroupExpanderLayout, shell_group_expander_layout,
                      SHELL, GROUP_EXPANDER_LAYOUT, ClutterLayoutManager)

struct _ShellGroupExpanderLayout
{
  ClutterLayoutManager parent;

  ClutterActor *cover;  /* weak */
  ClutterActor *header; /* weak */
  double expansion;
};

G_DEFINE_FINAL_TYPE (ShellGroupExpanderLayout, shell_group_expander_layout,
                     CLUTTER_TYPE_LAYOUT_MANAGER)

static void
group_layout_get_expanded_height (ShellGroupExpanderLayout *self,
                                  ClutterActor             *container,
                                  float                     for_width,
                                  float                    *min_expanded_p,
                                  float                    *nat_expanded_p)
{
  float min_expanded = 0, nat_expanded = 0;
  ClutterActor *child;

  for (child = clutter_actor_get_first_child (container);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      float min_child, nat_child;

      /* We don't need to measure the cover */
      if (child == self->cover)
        continue;

      clutter_actor_get_preferred_height (child, for_width, &min_child, &nat_child);
      min_expanded += min_child;
      nat_expanded += nat_child;
    }

  /* Add additional spacing after an expanded group */
  min_expanded += ADDITIONAL_BOTTOM_MARGIN_EXPANDED_GROUP;
  nat_expanded += ADDITIONAL_BOTTOM_MARGIN_EXPANDED_GROUP;

  if (min_expanded_p)
    *min_expanded_p = min_expanded;
  if (nat_expanded_p)
    *nat_expanded_p = nat_expanded;
}

static void
shell_group_expander_layout_get_preferred_width (ClutterLayoutManager *manager,
                                                 ClutterActor         *container,
                                                 float                 for_height,
                                                 float                *min_width_p,
                                                 float                *nat_width_p)
{
  ShellGroupExpanderLayout *self = SHELL_GROUP_EXPANDER_LAYOUT (manager);
  float min = 0, nat = 0;
  ClutterActor *child;

  for (child = clutter_actor_get_first_child (container);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      float min_child, nat_child;

      /* We don't need to measure the cover (and the JS reduction started over
       * at it) */
      if (child == self->cover)
        {
          min = 0;
          nat = 0;
          continue;
        }

      if (!clutter_actor_is_visible (child))
        continue;

      clutter_actor_get_preferred_width (child, for_height, &min_child, &nat_child);
      min = MAX (min_child, min);
      nat = MAX (nat_child, nat);
    }

  if (min_width_p)
    *min_width_p = min;
  if (nat_width_p)
    *nat_width_p = nat;
}

static void
shell_group_expander_layout_get_preferred_height (ClutterLayoutManager *manager,
                                                  ClutterActor         *container,
                                                  float                 for_width,
                                                  float                *min_height_p,
                                                  float                *nat_height_p)
{
  ShellGroupExpanderLayout *self = SHELL_GROUP_EXPANDER_LAYOUT (manager);
  double offset = HEIGHT_OFFSET_STACKED;
  float min = 0, nat = 0;
  int visible_count = MAX_VISIBLE_STACKED_MESSAGES;
  float min_expanded, nat_expanded;
  ClutterActor *child;

  for (child = clutter_actor_get_first_child (container);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      /* We don't need to measure the cover and the header is behind the
       * stacked messages */
      if (child == self->cover || child == self->header)
        continue;

      if (!clutter_actor_is_visible (child))
        continue;

      /* The first message is always fully shown */
      if (min == 0 || nat == 0)
        {
          clutter_actor_get_preferred_height (child, for_width, &min, &nat);
        }
      else
        {
          min += offset;
          nat += offset;
          offset /= HEIGHT_OFFSET_REDUCTION_STACKED;
        }

      visible_count--;

      if (visible_count == 0)
        break;
    }

  group_layout_get_expanded_height (self, container, for_width, &min_expanded, &nat_expanded);

  min = min + self->expansion * (min_expanded - min);
  nat = nat + self->expansion * (nat_expanded - nat);

  if (min_height_p)
    *min_height_p = min;
  if (nat_height_p)
    *nat_height_p = nat;
}

static void
shell_group_expander_layout_allocate (ClutterLayoutManager *manager,
                                      ClutterActor         *container,
                                      const ClutterActorBox *allocation)
{
  ShellGroupExpanderLayout *self = SHELL_GROUP_EXPANDER_LAYOUT (manager);
  g_autoptr (GList) children = NULL;
  ClutterActorBox box = *allocation;
  float child_width = box.x2 - box.x1;
  float full_y2 = box.y2;
  double height_offset = HEIGHT_OFFSET_STACKED;
  ClutterActor *top, *cover, *header;
  GList *stack, *l;
  int n_children;

  if (self->cover != NULL && clutter_actor_is_visible (self->cover))
    clutter_actor_allocate (self->cover, &box);

  if (self->header != NULL && clutter_actor_is_visible (self->header))
    {
      float min;

      clutter_actor_get_preferred_height (self->header, child_width, &min, NULL);
      box.y2 = box.y1 + min;
      clutter_actor_allocate (self->header, &box);
      box.y1 += self->expansion * (box.y2 - box.y1);
    }

  /* The group doesn't have any messages */
  n_children = clutter_actor_get_n_children (container);
  if (n_children < 3)
    return;

  children = get_children_list (container);
  top = children->data;
  cover = children->next->data;
  stack = children->next->next;
  header = g_list_last (children)->data;

  if (cover != self->cover || header != self->header)
    return;

  if (top != NULL)
    {
      float min;

      clutter_actor_get_preferred_height (top, child_width, &min, NULL);
      /* The first message is always fully shown */
      box.y2 = box.y1 + min;
      clutter_actor_allocate (top, &box);
    }

  for (l = stack; l != NULL && l->data != header; l = l->next)
    {
      ClutterActor *child = l->data;
      float min;
      float width_offset;

      clutter_actor_get_preferred_height (child, child_width, &min, NULL);

      /* Reduce width of children when collapsed */
      width_offset = (1.0 - self->expansion) * WIDTH_OFFSET_STACKED;
      box.x1 += width_offset;
      box.x2 -= width_offset;

      /* Stack children with a small reveal when collapsed */
      box.y2 += height_offset + self->expansion * (min - height_offset);
      /* Ensure messages are not placed outside the widget */
      if (box.y2 > full_y2)
        box.y2 = full_y2;
      else
        height_offset /= HEIGHT_OFFSET_REDUCTION_STACKED;
      box.y1 = box.y2 - min;

      clutter_actor_allocate (child, &box);
    }
}

static void
shell_group_expander_layout_set_expansion (ShellGroupExpanderLayout *self,
                                           double                    expansion)
{
  expansion = CLAMP (expansion, 0, 1);

  if (expansion == self->expansion)
    return;

  self->expansion = expansion;
  g_object_notify_by_pspec (G_OBJECT (self), group_layout_props[GROUP_LAYOUT_PROP_EXPANSION]);

  clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
}

static void
shell_group_expander_layout_get_property (GObject    *object,
                                          guint       prop_id,
                                          GValue     *value,
                                          GParamSpec *pspec)
{
  ShellGroupExpanderLayout *self = SHELL_GROUP_EXPANDER_LAYOUT (object);

  switch (prop_id)
    {
    case GROUP_LAYOUT_PROP_EXPANSION:
      g_value_set_double (value, self->expansion);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_group_expander_layout_set_property (GObject      *object,
                                          guint         prop_id,
                                          const GValue *value,
                                          GParamSpec   *pspec)
{
  ShellGroupExpanderLayout *self = SHELL_GROUP_EXPANDER_LAYOUT (object);

  switch (prop_id)
    {
    case GROUP_LAYOUT_PROP_EXPANSION:
      shell_group_expander_layout_set_expansion (self, g_value_get_double (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_group_expander_layout_dispose (GObject *object)
{
  ShellGroupExpanderLayout *self = SHELL_GROUP_EXPANDER_LAYOUT (object);

  g_clear_weak_pointer (&self->cover);
  g_clear_weak_pointer (&self->header);

  G_OBJECT_CLASS (shell_group_expander_layout_parent_class)->dispose (object);
}

static void
shell_group_expander_layout_class_init (ShellGroupExpanderLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  object_class->get_property = shell_group_expander_layout_get_property;
  object_class->set_property = shell_group_expander_layout_set_property;
  object_class->dispose = shell_group_expander_layout_dispose;

  layout_class->get_preferred_width = shell_group_expander_layout_get_preferred_width;
  layout_class->get_preferred_height = shell_group_expander_layout_get_preferred_height;
  layout_class->allocate = shell_group_expander_layout_allocate;

  group_layout_props[GROUP_LAYOUT_PROP_EXPANSION] =
    g_param_spec_double ("expansion", NULL, NULL,
                         0, 1, 0,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, GROUP_LAYOUT_N_PROPS, group_layout_props);
}

static void
shell_group_expander_layout_init (ShellGroupExpanderLayout *self)
{
}

static ClutterLayoutManager *
shell_group_expander_layout_new (ClutterActor *cover,
                                 ClutterActor *header)
{
  ShellGroupExpanderLayout *self = g_object_new (SHELL_TYPE_GROUP_EXPANDER_LAYOUT, NULL);

  g_set_weak_pointer (&self->cover, cover);
  g_set_weak_pointer (&self->header, header);

  return CLUTTER_LAYOUT_MANAGER (self);
}

/* ------------------------------------------------------------------------ */
/* ShellNotificationMessageGroup                                             */
/* ------------------------------------------------------------------------ */

enum
{
  GROUP_PROP_0,

  GROUP_PROP_SOURCE,
  GROUP_PROP_EXPANDED,
  GROUP_PROP_HAS_URGENT,
  GROUP_PROP_FOCUS_CHILD,

  GROUP_N_PROPS
};

static GParamSpec *group_props[GROUP_N_PROPS] = { NULL, };

enum
{
  GROUP_NOTIFICATION_ADDED,
  GROUP_EXPAND_TOGGLE_REQUESTED,

  GROUP_LAST_SIGNAL
};

static guint group_signals[GROUP_LAST_SIGNAL] = { 0, };

#define SHELL_TYPE_NOTIFICATION_MESSAGE_GROUP (shell_notification_message_group_get_type ())
G_DECLARE_FINAL_TYPE (ShellNotificationMessageGroup, shell_notification_message_group,
                      SHELL, NOTIFICATION_MESSAGE_GROUP, StWidget)

struct _ShellNotificationMessageGroup
{
  StWidget parent;

  ShellSource *source;

  /* The cover is always the second child to prevent interaction with stacked
   * messages when collapsed. The header will always be the last child. */
  ClutterActor *cover;
  ClutterActor *header_box;
  ShellGroupExpanderLayout *layout; /* owned by the actor */
  StButton *unexpand_button;

  gboolean expanded;
  GHashTable *notification_to_message; /* ShellNotification -> ShellNotificationMessage (borrowed) */
  int n_urgent;
  ClutterActor *focus_child; /* weak */
};

G_DEFINE_FINAL_TYPE (ShellNotificationMessageGroup, shell_notification_message_group, ST_TYPE_WIDGET)

static void group_move_message (ShellNotificationMessageGroup *self,
                                ClutterActor                  *message,
                                int                            index);

static gboolean
group_get_expanded (ShellNotificationMessageGroup *self)
{
  /* Consider this group to be expanded when it has only one message */
  return self->expanded ||
         g_hash_table_size (self->notification_to_message) == 1;
}

static gboolean
group_get_has_urgent (ShellNotificationMessageGroup *self)
{
  return self->n_urgent > 0;
}

static void
group_notify_expanded_change (ShellNotificationMessageGroup *self,
                              gboolean                       was_expanded)
{
  if (was_expanded != group_get_expanded (self))
    g_object_notify_by_pspec (G_OBJECT (self), group_props[GROUP_PROP_EXPANDED]);
}

/* Ensure that the cover is still below the top most message */
static void
group_ensure_cover_position (ShellNotificationMessageGroup *self)
{
  /* If the group doesn't have any messages, don't move the cover before the
   * header_box */
  if (clutter_actor_get_n_children (CLUTTER_ACTOR (self)) > 2)
    clutter_actor_set_child_at_index (CLUTTER_ACTOR (self), self->cover, 1);
}

static void
group_update_stacked_messages_fade (ShellNotificationMessageGroup *self)
{
  static const char * const pseudo_classes[] = { "second-in-stack", "lower-in-stack" };
  g_autoptr (GList) children = NULL;
  GList *stack_start, *l;
  ClutterActor *header;
  guint i;

  /* The group doesn't have any messages */
  if (clutter_actor_get_n_children (CLUTTER_ACTOR (self)) < 3)
    return;

  children = get_children_list (CLUTTER_ACTOR (self));
  header = g_list_last (children)->data;
  if (children->next->data != self->cover || header != self->header_box)
    return;

  /* A message may have moved so we need to remove the classes from all
   * messages. messages = [top, ...stack] */
  for (l = children; l != NULL && l->data != header; l = l->next)
    {
      ClutterActor *message;

      if (l->data == self->cover)
        continue;

      message = st_bin_get_child (ST_BIN (l->data));
      if (message == NULL)
        continue;

      for (i = 0; i < G_N_ELEMENTS (pseudo_classes); i++)
        st_widget_remove_style_pseudo_class (ST_WIDGET (message), pseudo_classes[i]);
    }

  /* stack = the children after the top and the cover, without the header */
  stack_start = children->next->next;
  if (!group_get_expanded (self) && stack_start != NULL && stack_start->data != header)
    {
      ClutterActor *stack_top = st_bin_get_child (ST_BIN (stack_start->data));

      if (stack_top != NULL)
        st_widget_add_style_pseudo_class (ST_WIDGET (stack_top), pseudo_classes[0]);

      /* Use the same class for the third message and all messages after that,
       * since they won't be visible anyways */
      for (l = stack_start->next; l != NULL && l->data != header; l = l->next)
        {
          ClutterActor *message = st_bin_get_child (ST_BIN (l->data));

          if (message != NULL)
            st_widget_add_style_pseudo_class (ST_WIDGET (message), pseudo_classes[1]);
        }
    }
}

static void
message_key_focus_in_cb (ClutterActor                  *actor,
                         ShellNotificationMessageGroup *self)
{
  if (self->focus_child == actor)
    return;

  g_set_weak_pointer (&self->focus_child, actor);
  g_object_notify_by_pspec (G_OBJECT (self), group_props[GROUP_PROP_FOCUS_CHILD]);
}

static void
message_expanded_cb (ShellMessage                  *message,
                     ShellNotificationMessageGroup *self)
{
  if (!group_get_expanded (self))
    g_signal_emit (self, group_signals[GROUP_EXPAND_TOGGLE_REQUESTED], 0);
}

static void group_close (ShellNotificationMessageGroup *self);

static void
message_close_cb (ShellMessage                  *message,
                  ShellNotificationMessageGroup *self)
{
  /* If the group is collapsed and one notification is closed, close the
   * entire group */
  if (!group_get_expanded (self))
    {
      g_signal_stop_emission_by_name (message, "close");
      group_close (self);
    }
}

static void
message_clicked_cb (StButton                      *button,
                    int                            clicked_button,
                    ShellNotificationMessageGroup *self)
{
  if (!group_get_expanded (self))
    {
      g_signal_stop_emission_by_name (button, "clicked");
      g_signal_emit (self, group_signals[GROUP_EXPAND_TOGGLE_REQUESTED], 0);
    }
}

/* Whether the message was counted as urgent, so that the count does not drift
 * when the urgency changes between levels that are not critical. */
#define URGENT_COUNTED_KEY "shell-message-urgent-counted"

static void
notification_urgency_changed_cb (ShellNotification *notification,
                                 GParamSpec        *pspec,
                                 ClutterActor      *message)
{
  ClutterActor *item = clutter_actor_get_parent (message);
  ShellNotificationMessageGroup *self;
  gboolean is_urgent, old_has_urgent, was_counted;
  int index;

  if (item == NULL || clutter_actor_get_parent (item) == NULL)
    return;

  self = SHELL_NOTIFICATION_MESSAGE_GROUP (clutter_actor_get_parent (item));
  is_urgent = shell_notification_get_urgency (notification) == SHELL_NOTIFICATION_URGENCY_CRITICAL;
  was_counted = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (message), URGENT_COUNTED_KEY));

  if (is_urgent == was_counted)
    return;

  old_has_urgent = group_get_has_urgent (self);

  if (is_urgent)
    self->n_urgent++;
  else
    self->n_urgent--;
  g_object_set_data (G_OBJECT (message), URGENT_COUNTED_KEY, GINT_TO_POINTER (is_urgent));

  index = is_urgent ? 0 : self->n_urgent;
  group_move_message (self, message, index);
  if (old_has_urgent != group_get_has_urgent (self))
    g_object_notify_by_pspec (G_OBJECT (self), group_props[GROUP_PROP_HAS_URGENT]);
}

static void
group_add_notification (ShellNotificationMessageGroup *self,
                        ShellNotification             *notification)
{
  ClutterActor *group = CLUTTER_ACTOR (self);
  ShellNotificationMessage *message;
  ClutterActor *item;
  gboolean is_urgent, old_has_urgent, was_expanded;
  int index;

  message = shell_notification_message_new (notification);
  g_object_ref_sink (message);

  g_hash_table_insert (self->notification_to_message,
                       g_object_ref (notification), message);

  is_urgent = shell_notification_get_urgency (notification) == SHELL_NOTIFICATION_URGENCY_CRITICAL;
  old_has_urgent = group_get_has_urgent (self);

  g_object_set_data (G_OBJECT (message), URGENT_COUNTED_KEY, GINT_TO_POINTER (is_urgent));
  g_signal_connect_object (notification, "notify::urgency",
                           G_CALLBACK (notification_urgency_changed_cb), message, 0);

  if (is_urgent)
    self->n_urgent++;

  was_expanded = group_get_expanded (self);
  item = scaling_item_new (CLUTTER_ACTOR (message));
  g_object_unref (message);

  g_signal_connect_object (message, "key-focus-in",
                           G_CALLBACK (message_key_focus_in_cb), self, 0);
  g_signal_connect_object (message, "expanded",
                           G_CALLBACK (message_expanded_cb), self, 0);
  g_signal_connect_object (message, "close",
                           G_CALLBACK (message_close_cb), self, 0);
  g_signal_connect_object (message, "clicked",
                           G_CALLBACK (message_clicked_cb), self, 0);

  index = is_urgent ? 0 : self->n_urgent;
  /* If we add a child below the top child we need to adjust index to skip the
   * cover child */
  if (index > 0)
    index += 1;

  clutter_actor_insert_child_at_index (group, item, index);
  group_ensure_cover_position (self);
  group_update_stacked_messages_fade (self);

  scaling_item_set_scaling_enabled (item, self->expanded);

  /* The first message doesn't need to be animated since the entire group is
   * animated */
  if (g_hash_table_size (self->notification_to_message) > 1)
    {
      ShellEaseParams params;

      init_ease_params (&params, MESSAGE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);
      shell_actor_ease (item, &params, NULL, NULL, NULL,
                        "scale-x", 1.0,
                        "scale-y", 1.0,
                        NULL);
    }
  else
    {
      clutter_actor_set_scale (item, 1.0, 1.0);
    }

  group_notify_expanded_change (self, was_expanded);

  if (old_has_urgent != group_get_has_urgent (self))
    g_object_notify_by_pspec (G_OBJECT (self), group_props[GROUP_PROP_HAS_URGENT]);

  g_signal_emit (self, group_signals[GROUP_NOTIFICATION_ADDED], 0);
}

typedef struct
{
  ShellNotificationMessageGroup *group;
  ClutterActor *item;
  ShellNotification *notification;
} RemoveNotificationData;

static void
remove_notification_data_free (RemoveNotificationData *data)
{
  g_object_unref (data->group);
  g_object_unref (data->item);
  g_object_unref (data->notification);
  g_free (data);
}

static void
remove_notification_complete_cb (gboolean finished,
                                 gpointer user_data)
{
  RemoveNotificationData *data = user_data;
  ShellNotificationMessageGroup *self = data->group;

  /* onComplete */
  if (!finished)
    return;

  clutter_actor_destroy (data->item);
  g_hash_table_remove (self->notification_to_message, data->notification);
  group_ensure_cover_position (self);
  group_update_stacked_messages_fade (self);

  if (g_hash_table_size (self->notification_to_message) == 1)
    g_signal_emit (self, group_signals[GROUP_EXPAND_TOGGLE_REQUESTED], 0);
}

static void
group_remove_notification (ShellNotificationMessageGroup *self,
                           ShellNotification             *notification)
{
  ClutterActor *message = g_hash_table_lookup (self->notification_to_message, notification);
  RemoveNotificationData *data;
  ShellEaseParams params;
  ClutterActor *item;

  if (message == NULL)
    return;

  item = clutter_actor_get_parent (message);

  if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (message), URGENT_COUNTED_KEY)))
    self->n_urgent--;

  g_signal_handlers_disconnect_by_data (message, self);

  scaling_item_set_scaling_enabled (item, self->expanded);

  data = g_new0 (RemoveNotificationData, 1);
  data->group = g_object_ref (self);
  data->item = g_object_ref (item);
  data->notification = g_object_ref (notification);

  init_ease_params (&params, MESSAGE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (item, &params,
                    remove_notification_complete_cb, data,
                    (GDestroyNotify) remove_notification_data_free,
                    "scale-x", 0.0,
                    "scale-y", 0.0,
                    NULL);
}

static void
source_notification_added_cb (ShellSource                   *source,
                              ShellNotification             *notification,
                              ShellNotificationMessageGroup *self)
{
  group_add_notification (self, notification);
}

static void
source_notification_removed_cb (ShellSource                   *source,
                                ShellNotification             *notification,
                                ShellNotificationMessageGroup *self)
{
  group_remove_notification (self, notification);
}

typedef struct
{
  ShellNotificationMessageGroup *group;
  ClutterActor *item;
  int index;
} MoveMessageData;

static void
move_message_data_free (MoveMessageData *data)
{
  g_object_unref (data->group);
  g_object_unref (data->item);
  g_free (data);
}

static void
move_message_complete_cb (gboolean finished,
                          gpointer user_data)
{
  MoveMessageData *data = user_data;
  ShellNotificationMessageGroup *self = data->group;
  int index = data->index;
  int n_children;
  ShellEaseParams params;

  /* onComplete */
  if (!finished)
    return;

  /* If we add a child below the top child we need to adjust index to skip the
   * cover child */
  if (index > 0)
    index += 1;

  n_children = clutter_actor_get_n_children (CLUTTER_ACTOR (self));
  clutter_actor_set_child_at_index (CLUTTER_ACTOR (self), data->item,
                                    CLAMP (index, 0, n_children));
  group_ensure_cover_position (self);
  group_update_stacked_messages_fade (self);

  init_ease_params (&params, MESSAGE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (data->item, &params, NULL, NULL, NULL,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    NULL);
}

static void
group_move_message (ShellNotificationMessageGroup *self,
                    ClutterActor                  *message,
                    int                            index)
{
  ClutterActor *group = CLUTTER_ACTOR (self);
  ClutterActor *item;
  MoveMessageData *data;
  ShellEaseParams params;

  if (index >= 0 && index < clutter_actor_get_n_children (group) &&
      clutter_actor_get_child_at_index (group, index) == message)
    return;

  item = clutter_actor_get_parent (message);

  data = g_new0 (MoveMessageData, 1);
  data->group = g_object_ref (self);
  data->item = g_object_ref (item);
  data->index = index;

  init_ease_params (&params, MESSAGE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (item, &params,
                    move_message_complete_cb, data,
                    (GDestroyNotify) move_message_data_free,
                    "scale-x", 0.0,
                    "scale-y", 0.0,
                    NULL);
}

typedef struct
{
  ShellNotificationMessageGroup *group;
  void (* done) (gpointer user_data);
  gpointer user_data;
  GDestroyNotify destroy;
} ExpansionData;

static void
expansion_data_free (ExpansionData *data)
{
  if (data->destroy != NULL)
    data->destroy (data->user_data);
  g_object_unref (data->group);
  g_free (data);
}

static void
collapse_complete_cb (gboolean finished,
                      gpointer user_data)
{
  ExpansionData *data = user_data;

  /* The JS awaited the animation and went on whether or not it ran to its
   * end */
  clutter_actor_hide (data->group->header_box);

  if (data->done != NULL)
    data->done (data->user_data);
}

static void
expand_complete_cb (gboolean finished,
                    gpointer user_data)
{
  ExpansionData *data = user_data;

  if (data->done != NULL)
    data->done (data->user_data);
}

static void
group_expand (ShellNotificationMessageGroup *self)
{
  ExpansionData *data;

  if (self->expanded)
    return;

  clutter_actor_show (self->header_box);
  self->expanded = TRUE;
  group_update_stacked_messages_fade (self);
  g_object_notify_by_pspec (G_OBJECT (self), group_props[GROUP_PROP_EXPANDED]);
  clutter_actor_hide (self->cover);

  data = g_new0 (ExpansionData, 1);
  data->group = g_object_ref (self);
  ease_actor_property (CLUTTER_ACTOR (self), "@layout.expansion", 1.0,
                       GROUP_EXPANSION_TIME, CLUTTER_EASE_OUT_QUAD,
                       expand_complete_cb, data,
                       (GDestroyNotify) expansion_data_free);
}

/* Collapses the group and calls @done, if given, once the animation is over;
 * at once if the group is not expanded */
static void
group_collapse (ShellNotificationMessageGroup *self,
                void                         (*done) (gpointer user_data),
                gpointer                       user_data,
                GDestroyNotify                 destroy)
{
  g_autoptr (GList) messages = NULL;
  ExpansionData *data;
  GList *l;

  if (!self->expanded)
    {
      if (done != NULL)
        done (user_data);
      if (destroy != NULL)
        destroy (user_data);
      return;
    }

  messages = g_hash_table_get_values (self->notification_to_message);
  for (l = messages; l != NULL; l = l->next)
    shell_message_unexpand (l->data, TRUE);

  /* Give focus to the fully visible message */
  if (self->focus_child != NULL && clutter_actor_has_key_focus (self->focus_child))
    {
      ClutterActor *first = clutter_actor_get_first_child (CLUTTER_ACTOR (self));

      if (first != NULL && st_bin_get_child (ST_BIN (first)) != NULL)
        clutter_actor_grab_key_focus (st_bin_get_child (ST_BIN (first)));
    }

  self->expanded = FALSE;
  g_object_notify_by_pspec (G_OBJECT (self), group_props[GROUP_PROP_EXPANDED]);
  clutter_actor_show (self->cover);
  group_update_stacked_messages_fade (self);

  data = g_new0 (ExpansionData, 1);
  data->group = g_object_ref (self);
  data->done = done;
  data->user_data = user_data;
  data->destroy = destroy;
  ease_actor_property (CLUTTER_ACTOR (self), "@layout.expansion", 0.0,
                       GROUP_EXPANSION_TIME, CLUTTER_EASE_OUT_QUAD,
                       collapse_complete_cb, data,
                       (GDestroyNotify) expansion_data_free);
}

static float
group_get_expanded_height (ShellNotificationMessageGroup *self)
{
  float min = 0;

  group_layout_get_expanded_height (self->layout, CLUTTER_ACTOR (self), -1, &min, NULL);

  return min;
}

static void
group_close (ShellNotificationMessageGroup *self)
{
  g_autoptr (GList) messages = NULL;
  GList *l;

  /* If the group is closed, close all messages in this group */
  messages = g_hash_table_get_values (self->notification_to_message);
  g_list_foreach (messages, (GFunc) g_object_ref, NULL);

  for (l = messages; l != NULL; l = l->next)
    {
      g_signal_handlers_disconnect_by_data (l->data, self);
      shell_message_close (l->data);
    }

  g_list_free_full (g_steal_pointer (&messages), g_object_unref);
}

static void
shell_notification_message_group_paint (ClutterActor        *actor,
                                        ClutterPaintContext *paint_context)
{
  ClutterActor *child;

  /* Invert the paint order, so that messages are collapsed with the newest
   * message (the first child) on top of the stack */
  for (child = clutter_actor_get_last_child (actor);
       child != NULL;
       child = clutter_actor_get_previous_sibling (child))
    clutter_actor_paint (child, paint_context);
}

static void
shell_notification_message_group_pick (ClutterActor       *actor,
                                       ClutterPickContext *pick_context)
{
  ClutterActor *child;

  /* Invert the pick order, so that messages are collapsed with the newest
   * message (the first child) on top of the stack */
  for (child = clutter_actor_get_last_child (actor);
       child != NULL;
       child = clutter_actor_get_previous_sibling (child))
    clutter_actor_pick (child, pick_context);
}

static void
shell_notification_message_group_map (ClutterActor *actor)
{
  ShellNotificationMessageGroup *self = SHELL_NOTIFICATION_MESSAGE_GROUP (actor);
  g_autoptr (GList) notifications = NULL;
  GList *l;

  /* Acknowledge all notifications once they are mapped */
  notifications = g_hash_table_get_keys (self->notification_to_message);
  g_list_foreach (notifications, (GFunc) g_object_ref, NULL);
  for (l = notifications; l != NULL; l = l->next)
    shell_notification_set_acknowledged (l->data, TRUE);
  g_list_free_full (g_steal_pointer (&notifications), g_object_unref);

  CLUTTER_ACTOR_CLASS (shell_notification_message_group_parent_class)->map (actor);
}

static GList *
shell_notification_message_group_get_focus_chain (StWidget *widget)
{
  ShellNotificationMessageGroup *self = SHELL_NOTIFICATION_MESSAGE_GROUP (widget);

  if (group_get_expanded (self))
    return get_children_list (CLUTTER_ACTOR (self));
  else
    return g_list_append (NULL, clutter_actor_get_first_child (CLUTTER_ACTOR (self)));
}

static void
unexpand_button_clicked_cb (StButton                      *button,
                            int                            clicked_button,
                            ShellNotificationMessageGroup *self)
{
  g_signal_emit (self, group_signals[GROUP_EXPAND_TOGGLE_REQUESTED], 0);
}

static void
group_click_recognize_cb (ClutterGesture                *gesture,
                          ShellNotificationMessageGroup *self)
{
  g_signal_emit (self, group_signals[GROUP_EXPAND_TOGGLE_REQUESTED], 0);
}

static void
shell_notification_message_group_get_property (GObject    *object,
                                               guint       prop_id,
                                               GValue     *value,
                                               GParamSpec *pspec)
{
  ShellNotificationMessageGroup *self = SHELL_NOTIFICATION_MESSAGE_GROUP (object);

  switch (prop_id)
    {
    case GROUP_PROP_SOURCE:
      g_value_set_object (value, self->source);
      break;

    case GROUP_PROP_EXPANDED:
      g_value_set_boolean (value, group_get_expanded (self));
      break;

    case GROUP_PROP_HAS_URGENT:
      g_value_set_boolean (value, group_get_has_urgent (self));
      break;

    case GROUP_PROP_FOCUS_CHILD:
      g_value_set_object (value, self->focus_child);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_notification_message_group_set_property (GObject      *object,
                                               guint         prop_id,
                                               const GValue *value,
                                               GParamSpec   *pspec)
{
  ShellNotificationMessageGroup *self = SHELL_NOTIFICATION_MESSAGE_GROUP (object);

  switch (prop_id)
    {
    case GROUP_PROP_SOURCE:
      g_set_object (&self->source, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_notification_message_group_constructed (GObject *object)
{
  ShellNotificationMessageGroup *self = SHELL_NOTIFICATION_MESSAGE_GROUP (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterAction *action;
  ClutterActor *title_label;
  ClutterLayoutManager *layout;
  g_autoptr (GList) notifications = NULL;
  GList *l;

  G_OBJECT_CLASS (shell_notification_message_group_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (self), "message-notification-group");
  clutter_actor_set_x_expand (actor, TRUE);
  clutter_actor_set_reactive (actor, TRUE);

  /* A widget that covers stacked messages so that they don't receive events */
  self->cover = g_object_new (ST_TYPE_WIDGET,
                              "name", "cover",
                              "reactive", TRUE,
                              NULL);

  self->header_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                   "style-class", "message-group-header",
                                   "x-expand", TRUE,
                                   "visible", FALSE,
                                   NULL);

  layout = shell_group_expander_layout_new (self->cover, self->header_box);
  clutter_actor_set_layout_manager (actor, layout);
  self->layout = SHELL_GROUP_EXPANDER_LAYOUT (layout);

  action = clutter_click_gesture_new ();
  g_signal_connect (action, "recognize",
                    G_CALLBACK (group_click_recognize_cb), self);
  clutter_actor_add_action (actor, action);

  title_label = g_object_new (ST_TYPE_LABEL,
                              "style-class", "message-group-title",
                              "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                              NULL);
  g_object_bind_property (self->source, "title", title_label, "text", G_BINDING_SYNC_CREATE);
  clutter_actor_add_child (self->header_box, title_label);

  self->unexpand_button = g_object_new (ST_TYPE_BUTTON,
                                        "style-class", "message-collapse-button",
                                        "icon-name", "group-collapse-symbolic",
                                        "x-align", CLUTTER_ACTOR_ALIGN_END,
                                        "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                        "x-expand", TRUE,
                                        "y-expand", TRUE,
                                        NULL);
  g_signal_connect (self->unexpand_button, "clicked",
                    G_CALLBACK (unexpand_button_clicked_cb), self);
  clutter_actor_add_child (self->header_box, CLUTTER_ACTOR (self->unexpand_button));

  clutter_actor_add_child (actor, self->header_box);
  clutter_actor_add_child (actor, self->cover);

  g_signal_connect_object (self->source, "notification-added",
                           G_CALLBACK (source_notification_added_cb), self, 0);
  g_signal_connect_object (self->source, "notification-removed",
                           G_CALLBACK (source_notification_removed_cb), self, 0);

  notifications = shell_source_get_notifications (self->source);
  for (l = notifications; l != NULL; l = l->next)
    group_add_notification (self, l->data);
}

static void
shell_notification_message_group_dispose (GObject *object)
{
  ShellNotificationMessageGroup *self = SHELL_NOTIFICATION_MESSAGE_GROUP (object);

  if (self->source != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->source, self);
      g_clear_object (&self->source);
    }

  g_hash_table_remove_all (self->notification_to_message);
  g_clear_weak_pointer (&self->focus_child);

  G_OBJECT_CLASS (shell_notification_message_group_parent_class)->dispose (object);
}

static void
shell_notification_message_group_finalize (GObject *object)
{
  ShellNotificationMessageGroup *self = SHELL_NOTIFICATION_MESSAGE_GROUP (object);

  g_clear_pointer (&self->notification_to_message, g_hash_table_unref);

  G_OBJECT_CLASS (shell_notification_message_group_parent_class)->finalize (object);
}

static void
shell_notification_message_group_class_init (ShellNotificationMessageGroupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);

  object_class->get_property = shell_notification_message_group_get_property;
  object_class->set_property = shell_notification_message_group_set_property;
  object_class->constructed = shell_notification_message_group_constructed;
  object_class->dispose = shell_notification_message_group_dispose;
  object_class->finalize = shell_notification_message_group_finalize;

  actor_class->paint = shell_notification_message_group_paint;
  actor_class->pick = shell_notification_message_group_pick;
  actor_class->map = shell_notification_message_group_map;

  widget_class->get_focus_chain = shell_notification_message_group_get_focus_chain;

  group_props[GROUP_PROP_SOURCE] =
    g_param_spec_object ("source", NULL, NULL,
                         SHELL_TYPE_SOURCE,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  group_props[GROUP_PROP_EXPANDED] =
    g_param_spec_boolean ("expanded", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  group_props[GROUP_PROP_HAS_URGENT] =
    g_param_spec_boolean ("has-urgent", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  group_props[GROUP_PROP_FOCUS_CHILD] =
    g_param_spec_object ("focus-child", NULL, NULL,
                         SHELL_TYPE_MESSAGE,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, GROUP_N_PROPS, group_props);

  group_signals[GROUP_NOTIFICATION_ADDED] =
    g_signal_new ("notification-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  group_signals[GROUP_EXPAND_TOGGLE_REQUESTED] =
    g_signal_new ("expand-toggle-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_notification_message_group_init (ShellNotificationMessageGroup *self)
{
  self->notification_to_message =
    g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, NULL);
}

static ClutterActor *
notification_message_group_new (ShellSource *source)
{
  return g_object_new (SHELL_TYPE_NOTIFICATION_MESSAGE_GROUP,
                       "source", source,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellMessageViewFade                                                      */
/* ------------------------------------------------------------------------ */

enum
{
  FADE_PROP_0,

  FADE_PROP_FADE_MARGIN,
  FADE_PROP_OPACITY,
  FADE_PROP_HIGHLIGHT_ACTOR,

  FADE_N_PROPS
};

static GParamSpec *fade_props[FADE_N_PROPS] = { NULL, };

#define SHELL_TYPE_MESSAGE_VIEW_FADE (shell_message_view_fade_get_type ())
G_DECLARE_FINAL_TYPE (ShellMessageViewFade, shell_message_view_fade,
                      SHELL, MESSAGE_VIEW_FADE, ClutterShaderEffect)

struct _ShellMessageViewFade
{
  ClutterShaderEffect parent;

  float fade_margin;
  float opacity;
  ClutterActor *highlight_actor; /* weak */
  StAdjustment *vadjustment;     /* weak; the view's, connected */
  gulong vadjustment_notify_id;  /* on the actor of the meta */
};

G_DEFINE_FINAL_TYPE (ShellMessageViewFade, shell_message_view_fade, CLUTTER_TYPE_SHADER_EFFECT)

static const char fade_declarations[] =
  "uniform sampler2D tex;\n"
  "uniform float height;\n"
  "uniform float opacity;\n"
  "uniform float top_edge_fade;\n"
  "uniform float bottom_edge_fade;\n"
  "uniform float top_fade_position;\n"
  "uniform float bottom_fade_position;\n";

static const char fade_replace[] =
  "cogl_color_out = cogl_color_in * texture2D (tex, vec2 (cogl_tex_coord_in[0].xy));\n"
  "float fade_base = 300;\n"
  "float fade_height = 400;\n"
  "float y = height * cogl_tex_coord_in[0].y;\n"
  "float ratio = 1.0;\n"
  "\n"
  "if (y < top_fade_position && top_fade_position > 0.0) {\n"
  "    float edge1 = top_fade_position - fade_height;\n"
  "    float edge2 = top_fade_position;\n"
  "    ratio = (smoothstep (edge1, edge2 + fade_base, y) - 1.0) * opacity + 1.0;\n"
  "} else if (y > bottom_fade_position && bottom_fade_position < height) {\n"
  "    float edge1 = bottom_fade_position + fade_height;\n"
  "    float edge2 = bottom_fade_position;\n"
  "    ratio = (smoothstep (edge1, edge2 - fade_base, y) - 1.0) * opacity + 1.0;\n"
  "}\n"
  "\n"
  "if (top_edge_fade > 0.0)\n"
  "    ratio *= smoothstep (0.0, top_edge_fade, y);\n"
  "if (bottom_edge_fade > 0.0 && bottom_edge_fade < height)\n"
  "    ratio *= smoothstep (height, bottom_edge_fade, y);\n"
  "\n"
  "cogl_color_out *= ratio;\n";

static void
fade_update_enabled (ShellMessageViewFade *self)
{
  ClutterActorMeta *meta = CLUTTER_ACTOR_META (self);
  double upper, page_size;

  if (self->vadjustment == NULL)
    {
      clutter_actor_meta_set_enabled (meta, FALSE);
      return;
    }

  st_adjustment_get_values (self->vadjustment, NULL, NULL, &upper, NULL, NULL, &page_size);

  clutter_actor_meta_set_enabled (meta,
                                  (upper > page_size && self->fade_margin > 0.0) ||
                                  self->highlight_actor != NULL);
}

static void
fade_vadjustment_changed_cb (StAdjustment         *adjustment,
                             ShellMessageViewFade *self)
{
  fade_update_enabled (self);
}

static void
fade_disconnect_vadjustment (ShellMessageViewFade *self)
{
  /* The adjustment can be finalized before the view it belongs to is
   * destroyed, hence the weak pointer */
  if (self->vadjustment != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->vadjustment,
                                            fade_vadjustment_changed_cb, self);
      g_clear_weak_pointer (&self->vadjustment);
    }
}

static void
fade_actor_vadjustment_changed (ShellMessageViewFade *self)
{
  ClutterActor *actor = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self));
  StAdjustment *new_adjustment = NULL;

  if (actor != NULL)
    {
      g_object_get (actor, "vadjustment", &new_adjustment, NULL);
      if (new_adjustment != NULL)
        g_object_unref (new_adjustment);
    }

  if (self->vadjustment == new_adjustment)
    return;

  fade_disconnect_vadjustment (self);
  g_set_weak_pointer (&self->vadjustment, new_adjustment);
  if (self->vadjustment != NULL)
    {
      g_signal_connect (self->vadjustment, "changed",
                        G_CALLBACK (fade_vadjustment_changed_cb), self);
    }
  fade_update_enabled (self);
}

static void
fade_actor_vadjustment_notify_cb (ClutterActor         *actor,
                                  GParamSpec           *pspec,
                                  ShellMessageViewFade *self)
{
  fade_actor_vadjustment_changed (self);
}

static void
shell_message_view_fade_set_actor (ClutterActorMeta *meta,
                                   ClutterActor     *actor)
{
  ShellMessageViewFade *self = SHELL_MESSAGE_VIEW_FADE (meta);
  ClutterActor *old_actor = clutter_actor_meta_get_actor (meta);

  if (old_actor == actor)
    return;

  /* Only our own handler: ClutterActorMeta connects to the actor's destroy
   * signal with the meta as data too, and disconnects it by id afterwards */
  if (old_actor != NULL)
    g_clear_signal_handler (&self->vadjustment_notify_id, old_actor);

  if (actor != NULL)
    {
      self->vadjustment_notify_id =
        g_signal_connect (actor, "notify::vadjustment",
                          G_CALLBACK (fade_actor_vadjustment_notify_cb), self);
    }

  CLUTTER_ACTOR_META_CLASS (shell_message_view_fade_parent_class)->set_actor (meta, actor);
  fade_actor_vadjustment_changed (self);
}

static void
shell_message_view_fade_paint_target (ClutterOffscreenEffect *effect,
                                      ClutterPaintNode       *node,
                                      ClutterPaintContext    *paint_context)
{
  ShellMessageViewFade *self = SHELL_MESSAGE_VIEW_FADE (effect);
  ClutterShaderEffect *shader = CLUTTER_SHADER_EFFECT (effect);
  ClutterActor *actor = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (effect));
  double page_size, upper, value;

  if (self->vadjustment != NULL)
    {
      st_adjustment_get_values (self->vadjustment, &value, NULL, &upper, NULL, NULL, &page_size);
    }
  else
    {
      page_size = upper = clutter_actor_get_height (actor);
      value = 0;
    }

  if (self->highlight_actor != NULL)
    {
      graphene_point3d_t origin = GRAPHENE_POINT3D_INIT (0, 0, 0);
      graphene_point3d_t position;

      clutter_actor_apply_relative_transform_to_point (self->highlight_actor, actor,
                                                       &origin, &position);

      clutter_shader_effect_set_uniform (shader, "top_fade_position", G_TYPE_FLOAT, 1,
                                         (float) (position.y - value));
      clutter_shader_effect_set_uniform (shader, "bottom_fade_position", G_TYPE_FLOAT, 1,
                                         (float) ((position.y + clutter_actor_get_height (self->highlight_actor) - 1) - value));
    }
  else
    {
      clutter_shader_effect_set_uniform (shader, "top_fade_position", G_TYPE_FLOAT, 1, 0.0f);
      clutter_shader_effect_set_uniform (shader, "bottom_fade_position", G_TYPE_FLOAT, 1, 0.0f);
    }

  clutter_shader_effect_set_uniform (shader, "tex", G_TYPE_INT, 1, 0);
  clutter_shader_effect_set_uniform (shader, "opacity", G_TYPE_FLOAT, 1, self->opacity);
  clutter_shader_effect_set_uniform (shader, "height", G_TYPE_FLOAT, 1, (float) page_size);

  clutter_shader_effect_set_uniform (shader, "top_edge_fade", G_TYPE_FLOAT, 1,
                                     (float) MIN (value, self->fade_margin));
  clutter_shader_effect_set_uniform (shader, "bottom_edge_fade", G_TYPE_FLOAT, 1,
                                     (float) (page_size - MIN (upper - page_size - value,
                                                               self->fade_margin)));

  CLUTTER_OFFSCREEN_EFFECT_CLASS (shell_message_view_fade_parent_class)->paint_target (effect, node, paint_context);
}

static CoglSnippet *
shell_message_view_fade_get_static_snippet (ClutterShaderEffect *effect)
{
  CoglSnippet *snippet;

  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT, fade_declarations, NULL);
  cogl_snippet_set_replace (snippet, fade_replace);

  return snippet;
}

static void
fade_set_highlight_actor (ShellMessageViewFade *self,
                          ClutterActor         *actor)
{
  if (self->highlight_actor == actor)
    return;

  g_set_weak_pointer (&self->highlight_actor, actor);
  clutter_effect_queue_repaint (CLUTTER_EFFECT (self));

  fade_update_enabled (self);
  g_object_notify_by_pspec (G_OBJECT (self), fade_props[FADE_PROP_HIGHLIGHT_ACTOR]);
}

static void
fade_set_opacity (ShellMessageViewFade *self,
                  float                 opacity)
{
  if (self->opacity == opacity)
    return;

  self->opacity = opacity;
  clutter_effect_queue_repaint (CLUTTER_EFFECT (self));

  g_object_notify_by_pspec (G_OBJECT (self), fade_props[FADE_PROP_OPACITY]);
}

static void
fade_set_fade_margin (ShellMessageViewFade *self,
                      float                 fade_margin)
{
  if (self->fade_margin == fade_margin)
    return;

  self->fade_margin = fade_margin;
  clutter_effect_queue_repaint (CLUTTER_EFFECT (self));

  g_object_notify_by_pspec (G_OBJECT (self), fade_props[FADE_PROP_FADE_MARGIN]);
}

static void
shell_message_view_fade_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  ShellMessageViewFade *self = SHELL_MESSAGE_VIEW_FADE (object);

  switch (prop_id)
    {
    case FADE_PROP_FADE_MARGIN:
      g_value_set_float (value, self->fade_margin);
      break;

    case FADE_PROP_OPACITY:
      g_value_set_float (value, self->opacity);
      break;

    case FADE_PROP_HIGHLIGHT_ACTOR:
      g_value_set_object (value, self->highlight_actor);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_message_view_fade_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  ShellMessageViewFade *self = SHELL_MESSAGE_VIEW_FADE (object);

  switch (prop_id)
    {
    case FADE_PROP_FADE_MARGIN:
      fade_set_fade_margin (self, g_value_get_float (value));
      break;

    case FADE_PROP_OPACITY:
      fade_set_opacity (self, g_value_get_float (value));
      break;

    case FADE_PROP_HIGHLIGHT_ACTOR:
      fade_set_highlight_actor (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_message_view_fade_dispose (GObject *object)
{
  ShellMessageViewFade *self = SHELL_MESSAGE_VIEW_FADE (object);

  fade_disconnect_vadjustment (self);
  g_clear_weak_pointer (&self->highlight_actor);

  G_OBJECT_CLASS (shell_message_view_fade_parent_class)->dispose (object);
}

static void
shell_message_view_fade_class_init (ShellMessageViewFadeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorMetaClass *meta_class = CLUTTER_ACTOR_META_CLASS (klass);
  ClutterOffscreenEffectClass *offscreen_class = CLUTTER_OFFSCREEN_EFFECT_CLASS (klass);
  ClutterShaderEffectClass *shader_class = CLUTTER_SHADER_EFFECT_CLASS (klass);

  object_class->get_property = shell_message_view_fade_get_property;
  object_class->set_property = shell_message_view_fade_set_property;
  object_class->dispose = shell_message_view_fade_dispose;

  meta_class->set_actor = shell_message_view_fade_set_actor;
  offscreen_class->paint_target = shell_message_view_fade_paint_target;
  shader_class->get_static_snippet = shell_message_view_fade_get_static_snippet;

  fade_props[FADE_PROP_FADE_MARGIN] =
    g_param_spec_float ("fade-margin", NULL, NULL,
                        0, G_MAXFLOAT, 0,
                        G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  fade_props[FADE_PROP_OPACITY] =
    g_param_spec_float ("opacity", NULL, NULL,
                        0, 1, 0,
                        G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  fade_props[FADE_PROP_HIGHLIGHT_ACTOR] =
    g_param_spec_object ("highlight-actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, FADE_N_PROPS, fade_props);
}

static void
shell_message_view_fade_init (ShellMessageViewFade *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellMessageView                                                          */
/* ------------------------------------------------------------------------ */

enum
{
  VIEW_PROP_0,

  VIEW_PROP_MESSAGE_TRAY,
  VIEW_PROP_CAN_CLEAR,
  VIEW_PROP_EMPTY,
  VIEW_PROP_EXPANDED_GROUP,

  VIEW_N_PROPS
};

static GParamSpec *view_props[VIEW_N_PROPS] = { NULL, };

enum
{
  VIEW_MESSAGE_FOCUSED,

  VIEW_LAST_SIGNAL
};

static guint view_signals[VIEW_LAST_SIGNAL] = { 0, };

struct _ShellMessageView
{
  StViewport parent;

  ShellMessageTray *message_tray;
  GPtrArray *messages;              /* of ClutterActor, borrowed */
  GHashTable *source_to_group;      /* ShellSource -> group, borrowed */
  int n_media;
  int n_urgent;

  ClutterActor *overlay;            /* owned by the actor */
  ClutterActor *expanded_group;     /* weak */
  ShellMessageViewFade *fade;       /* owned by the actor */

  /* When a group is expanded the user isn't allowed to scroll outside the
   * expanded group, therefore the adjustment used by the view needs to be
   * different then the external adjustment used by the scrollbar and
   * scrollview. */
  StAdjustment *scroll_view_adjustment;
  double adj_value_offset;
  GBindingGroup *adj_binding;
  gboolean adj_binding_active;
  GBinding *value_binding;
};

static void shell_message_view_scrollable_init (StScrollableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (ShellMessageView, shell_message_view, ST_TYPE_VIEWPORT,
                               G_IMPLEMENT_INTERFACE (ST_TYPE_SCROLLABLE,
                                                      shell_message_view_scrollable_init))

static StScrollableInterface *parent_scrollable_iface = NULL;

/* ------------------------------------------------------------------------ */
/* ShellMessageViewLayout                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_MESSAGE_VIEW_LAYOUT (shell_message_view_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellMessageViewLayout, shell_message_view_layout,
                      SHELL, MESSAGE_VIEW_LAYOUT, ClutterLayoutManager)

struct _ShellMessageViewLayout
{
  ClutterLayoutManager parent;

  ClutterActor *overlay; /* weak */
};

G_DEFINE_FINAL_TYPE (ShellMessageViewLayout, shell_message_view_layout,
                     CLUTTER_TYPE_LAYOUT_MANAGER)

static void
shell_message_view_layout_get_preferred_width (ClutterLayoutManager *manager,
                                               ClutterActor         *container,
                                               float                 for_height,
                                               float                *min_width_p,
                                               float                *nat_width_p)
{
  float min = 0, nat = 0;
  ClutterActor *child;

  for (child = clutter_actor_get_first_child (container);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      float min_child, nat_child;

      clutter_actor_get_preferred_width (child, for_height, &min_child, &nat_child);
      min = MAX (min_child, min);
      nat = MAX (nat_child, nat);
    }

  if (min_width_p)
    *min_width_p = min;
  if (nat_width_p)
    *nat_width_p = nat;
}

static void
shell_message_view_layout_get_preferred_height (ClutterLayoutManager *manager,
                                                ClutterActor         *container,
                                                float                 for_width,
                                                float                *min_height_p,
                                                float                *nat_height_p)
{
  float min = 0, nat = 0;
  ClutterActor *child;

  for (child = clutter_actor_get_first_child (container);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      float min_child, nat_child;

      clutter_actor_get_preferred_height (child, for_width, &min_child, &nat_child);
      min += min_child;
      nat += nat_child;
    }

  if (min_height_p)
    *min_height_p = min;
  if (nat_height_p)
    *nat_height_p = nat;
}

static void
shell_message_view_layout_allocate (ClutterLayoutManager  *manager,
                                    ClutterActor          *container,
                                    const ClutterActorBox *allocation)
{
  ShellMessageViewLayout *self = SHELL_MESSAGE_VIEW_LAYOUT (manager);
  ShellMessageView *view = SHELL_MESSAGE_VIEW (container);
  ClutterActorBox box = *allocation;
  float width = box.x2 - box.x1;
  guint i;

  if (self->overlay != NULL && clutter_actor_is_visible (self->overlay))
    clutter_actor_allocate (self->overlay, &box);

  /* We need to use the order in messages since the children order is the
   * render order and the expanded group needs to be the top most child and the
   * overlay the child below it. */
  for (i = 0; i < view->messages->len; i++)
    {
      ClutterActor *message = g_ptr_array_index (view->messages, i);
      ClutterActor *child = clutter_actor_get_parent (message);
      float min;

      clutter_actor_get_preferred_height (child, width, &min, NULL);
      box.y2 = box.y1 + min;
      clutter_actor_allocate (child, &box);
      box.y1 = box.y2;
    }
}

static void
shell_message_view_layout_dispose (GObject *object)
{
  ShellMessageViewLayout *self = SHELL_MESSAGE_VIEW_LAYOUT (object);

  g_clear_weak_pointer (&self->overlay);

  G_OBJECT_CLASS (shell_message_view_layout_parent_class)->dispose (object);
}

static void
shell_message_view_layout_class_init (ShellMessageViewLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  object_class->dispose = shell_message_view_layout_dispose;

  layout_class->get_preferred_width = shell_message_view_layout_get_preferred_width;
  layout_class->get_preferred_height = shell_message_view_layout_get_preferred_height;
  layout_class->allocate = shell_message_view_layout_allocate;
}

static void
shell_message_view_layout_init (ShellMessageViewLayout *self)
{
}

/* ------------------------------------------------------------------------ */
/* Messages                                                                  */
/* ------------------------------------------------------------------------ */

static gboolean
item_can_close (ClutterActor *message)
{
  if (SHELL_IS_MESSAGE (message))
    return shell_message_can_close (SHELL_MESSAGE (message));

  return SHELL_IS_NOTIFICATION_MESSAGE_GROUP (message);
}

static void
item_close (ClutterActor *message)
{
  if (SHELL_IS_MESSAGE (message))
    shell_message_close (SHELL_MESSAGE (message));
  else if (SHELL_IS_NOTIFICATION_MESSAGE_GROUP (message))
    group_close (SHELL_NOTIFICATION_MESSAGE_GROUP (message));
}

/**
 * shell_message_view_get_empty:
 * @self: a #ShellMessageView
 *
 * Returns: #ShellMessageView:empty
 */
gboolean
shell_message_view_get_empty (ShellMessageView *self)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_VIEW (self), TRUE);

  return self->messages->len == 0;
}

/**
 * shell_message_view_get_can_clear:
 * @self: a #ShellMessageView
 *
 * Returns: #ShellMessageView:can-clear
 */
gboolean
shell_message_view_get_can_clear (ShellMessageView *self)
{
  guint i;

  g_return_val_if_fail (SHELL_IS_MESSAGE_VIEW (self), FALSE);

  for (i = 0; i < self->messages->len; i++)
    {
      if (item_can_close (g_ptr_array_index (self->messages, i)))
        return TRUE;
    }

  return FALSE;
}

/**
 * shell_message_view_get_expanded_group:
 * @self: a #ShellMessageView
 *
 * Returns: (transfer none) (nullable): #ShellMessageView:expanded-group
 */
ClutterActor *
shell_message_view_get_expanded_group (ShellMessageView *self)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_VIEW (self), NULL);

  return self->expanded_group;
}

/**
 * shell_message_view_get_message_tray:
 * @self: a #ShellMessageView
 *
 * Returns: (transfer none) (nullable): #ShellMessageView:message-tray
 */
ShellMessageTray *
shell_message_view_get_message_tray (ShellMessageView *self)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_VIEW (self), NULL);

  return self->message_tray;
}

static void
view_key_focus_in_cb (ClutterActor     *actor,
                      ShellMessageView *self)
{
  /* The groups report the focus of their messages */
  if (SHELL_IS_MESSAGE (actor))
    g_signal_emit (self, view_signals[VIEW_MESSAGE_FOCUSED], 0, actor);
}

static void
view_message_destroy_cb (ClutterActor     *message,
                         ShellMessageView *self)
{
  /* Make sure that the messages array is updated even when
   * view_remove_message () isn't called. */
  g_ptr_array_remove (self->messages, message);
}

static void
view_notify_empty_and_can_clear (ShellMessageView *self,
                                 gboolean          was_empty,
                                 gboolean          could_clear)
{
  if (was_empty != shell_message_view_get_empty (self))
    g_object_notify_by_pspec (G_OBJECT (self), view_props[VIEW_PROP_EMPTY]);

  if (could_clear != shell_message_view_get_can_clear (self))
    g_object_notify_by_pspec (G_OBJECT (self), view_props[VIEW_PROP_CAN_CLEAR]);
}

static void
view_add_message_at_index (ShellMessageView *self,
                           ClutterActor     *message,
                           int               index)
{
  gboolean was_empty = shell_message_view_get_empty (self);
  gboolean could_clear = shell_message_view_get_can_clear (self);
  ShellEaseParams params;
  ClutterActor *item;

  if (g_ptr_array_find (self->messages, message, NULL))
    return;

  item = scaling_item_new (message);

  g_signal_connect_object (message, "key-focus-in",
                           G_CALLBACK (view_key_focus_in_cb), self, 0);
  /* Make sure that the messages array is updated even when
   * view_remove_message () isn't called. */
  g_signal_connect_object (message, "destroy",
                           G_CALLBACK (view_message_destroy_cb), self, 0);

  clutter_actor_add_child (CLUTTER_ACTOR (self), item);
  g_ptr_array_insert (self->messages, CLAMP (index, 0, (int) self->messages->len), message);

  view_notify_empty_and_can_clear (self, was_empty, could_clear);

  init_ease_params (&params, MESSAGE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (item, &params, NULL, NULL, NULL,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    NULL);
}

typedef struct
{
  ShellMessageView *view;
  ClutterActor *message;
  ClutterActor *item;
  int index;
} ViewMessageData;

static ViewMessageData *
view_message_data_new (ShellMessageView *view,
                       ClutterActor     *message,
                       ClutterActor     *item,
                       int               index)
{
  ViewMessageData *data = g_new0 (ViewMessageData, 1);

  data->view = g_object_ref (view);
  data->message = g_object_ref (message);
  data->item = g_object_ref (item);
  data->index = index;

  return data;
}

static void
view_message_data_free (ViewMessageData *data)
{
  g_object_unref (data->view);
  g_object_unref (data->message);
  g_object_unref (data->item);
  g_free (data);
}

static void
view_move_message_complete_cb (gboolean finished,
                               gpointer user_data)
{
  ViewMessageData *data = user_data;
  ShellMessageView *self = data->view;
  ShellEaseParams params;
  guint old_index;

  /* onComplete */
  if (!finished)
    return;

  if (g_ptr_array_find (self->messages, data->message, &old_index))
    {
      g_ptr_array_remove_index (self->messages, old_index);
      g_ptr_array_insert (self->messages,
                          CLAMP (data->index, 0, (int) self->messages->len),
                          data->message);
    }
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));

  init_ease_params (&params, MESSAGE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (data->item, &params, NULL, NULL, NULL,
                    "scale-x", 1.0,
                    "scale-y", 1.0,
                    NULL);
}

static void
view_move_message (ShellMessageView *self,
                   ClutterActor     *message,
                   int               index)
{
  ShellEaseParams params;
  ClutterActor *item;

  if (!g_ptr_array_find (self->messages, message, NULL))
    return;

  if (index >= 0 && index < (int) self->messages->len &&
      g_ptr_array_index (self->messages, index) == message)
    return;

  item = clutter_actor_get_parent (message);

  init_ease_params (&params, MESSAGE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (item, &params,
                    view_move_message_complete_cb,
                    view_message_data_new (self, message, item, index),
                    (GDestroyNotify) view_message_data_free,
                    "scale-x", 0.0,
                    "scale-y", 0.0,
                    NULL);
}

static void
view_remove_message_complete_cb (gboolean finished,
                                 gpointer user_data)
{
  ViewMessageData *data = user_data;
  ShellMessageView *self = data->view;
  gboolean was_empty, could_clear;
  guint index;

  /* onComplete */
  if (!finished)
    return;

  was_empty = shell_message_view_get_empty (self);
  could_clear = shell_message_view_get_can_clear (self);

  if (g_ptr_array_find (self->messages, data->message, &index) &&
      clutter_actor_has_key_focus (data->message))
    {
      ClutterActor *next_message = NULL;

      if (index + 1 < self->messages->len)
        next_message = g_ptr_array_index (self->messages, index + 1);
      else if (index > 0)
        next_message = g_ptr_array_index (self->messages, index - 1);
      else
        next_message = CLUTTER_ACTOR (self);

      clutter_actor_grab_key_focus (next_message);
    }

  /* The message is removed from the messages array in the destroy signal
   * handler */
  clutter_actor_destroy (data->item);

  view_notify_empty_and_can_clear (self, was_empty, could_clear);
}

static void
view_remove_message (ShellMessageView *self,
                     ClutterActor     *message)
{
  ShellEaseParams params;
  ClutterActor *item;

  if (!g_ptr_array_find (self->messages, message, NULL))
    return;

  item = clutter_actor_get_parent (message);

  init_ease_params (&params, MESSAGE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (item, &params,
                    view_remove_message_complete_cb,
                    view_message_data_new (self, message, item, 0),
                    (GDestroyNotify) view_message_data_free,
                    "scale-x", 0.0,
                    "scale-y", 0.0,
                    NULL);
}

/**
 * shell_message_view_add_media_message:
 * @self: a #ShellMessageView
 * @message: the message of a media player
 *
 * Adds the message of a media player above all the notifications.
 */
void
shell_message_view_add_media_message (ShellMessageView *self,
                                      ShellMessage     *message)
{
  g_return_if_fail (SHELL_IS_MESSAGE_VIEW (self));
  g_return_if_fail (SHELL_IS_MESSAGE (message));

  self->n_media++;
  view_add_message_at_index (self, CLUTTER_ACTOR (message), 0);
}

/**
 * shell_message_view_remove_media_message:
 * @self: a #ShellMessageView
 * @message: a message added with shell_message_view_add_media_message ()
 *
 * Removes the message of a media player.
 */
void
shell_message_view_remove_media_message (ShellMessageView *self,
                                         ShellMessage     *message)
{
  g_return_if_fail (SHELL_IS_MESSAGE_VIEW (self));
  g_return_if_fail (SHELL_IS_MESSAGE (message));

  if (!g_ptr_array_find (self->messages, message, NULL))
    return;

  self->n_media--;
  view_remove_message (self, CLUTTER_ACTOR (message));
}

static void
view_clear_complete_cb (gboolean finished,
                        gpointer user_data)
{
  ClutterActor *message = user_data;

  /* onComplete */
  if (!finished)
    return;

  item_close (message);
}

/**
 * shell_message_view_clear:
 * @self: a #ShellMessageView
 *
 * Closes all the messages that can be closed.
 */
void
shell_message_view_clear (ShellMessageView *self)
{
  g_autoptr (GPtrArray) messages = NULL;
  guint i;

  g_return_if_fail (SHELL_IS_MESSAGE_VIEW (self));

  messages = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = 0; i < self->messages->len; i++)
    {
      ClutterActor *message = g_ptr_array_index (self->messages, i);

      if (item_can_close (message))
        g_ptr_array_add (messages, g_object_ref (message));
    }

  /* If there are few messages, letting them all zoom out looks OK */
  if (messages->len < 2)
    {
      for (i = 0; i < messages->len; i++)
        item_close (g_ptr_array_index (messages, i));
    }
  else
    {
      /* Otherwise we slide them out one by one, and then zoom them out
       * "off-screen" in the end to smoothly shrink the parent */
      double delay = MESSAGE_ANIMATION_TIME / (double) MAX (messages->len, 5);

      for (i = 0; i < messages->len; i++)
        {
          ClutterActor *message = g_ptr_array_index (messages, i);
          ShellEaseParams params;

          init_ease_params (&params, MESSAGE_ANIMATION_TIME, (int) (i * delay),
                            CLUTTER_EASE_OUT_QUAD);
          shell_actor_ease (clutter_actor_get_parent (message), &params,
                            view_clear_complete_cb, g_object_ref (message), g_object_unref,
                            "translation-x", (double) clutter_actor_get_width (CLUTTER_ACTOR (self)),
                            "opacity", 0u,
                            NULL);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Expanded group                                                            */
/* ------------------------------------------------------------------------ */

static StAdjustment *
view_get_vadjustment (ShellMessageView *self)
{
  StAdjustment *vadjustment = NULL;

  st_scrollable_get_adjustments (ST_SCROLLABLE (self), NULL, &vadjustment);

  return vadjustment;
}

/* Try to center the expanded group in the available space */
static void
view_scroll_to_expanded_group (ShellMessageView *self)
{
  StAdjustment *vadjustment = view_get_vadjustment (self);
  graphene_point3d_t origin = GRAPHENE_POINT3D_INIT (0, 0, 0);
  graphene_point3d_t position;
  double page_size, page_center, group_expanded_height, group_center, value;

  if (self->expanded_group == NULL || vadjustment == NULL)
    return;

  group_expanded_height =
    group_get_expanded_height (SHELL_NOTIFICATION_MESSAGE_GROUP (self->expanded_group));
  clutter_actor_apply_relative_transform_to_point (self->expanded_group,
                                                   CLUTTER_ACTOR (self),
                                                   &origin, &position);
  group_center = position.y + (group_expanded_height / 2);
  st_adjustment_get_values (vadjustment, NULL, NULL, NULL, NULL, NULL, &page_size);
  page_center = page_size / 2;
  value = MIN (position.y, group_center - page_center);

  ease_adjustment_value (vadjustment, value, GROUP_EXPANSION_TIME, CLUTTER_EASE_OUT_QUAD);
}

static void
view_highlight_group (ShellMessageView *self,
                      ClutterActor     *group)
{
  g_object_set (self->fade,
                "opacity", 0.0f,
                "highlight-actor", group,
                NULL);
  ease_actor_property (CLUTTER_ACTOR (self), "@effects.highlight.opacity", 1.0,
                       MESSAGE_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD,
                       NULL, NULL, NULL);
}

static void
unhighlight_complete_cb (gboolean finished,
                         gpointer user_data)
{
  ShellMessageView *self = user_data;

  /* onStopped */
  if (self->fade != NULL)
    g_object_set (self->fade, "highlight-actor", NULL, NULL);
}

static void
view_unhighlight_group (ShellMessageView *self)
{
  ease_actor_property (CLUTTER_ACTOR (self), "@effects.highlight.opacity", 0.0,
                       MESSAGE_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD,
                       unhighlight_complete_cb, g_object_ref (self), g_object_unref);
}

typedef struct
{
  ShellMessageView *view;
  GWeakRef group;
} ExpandedGroupData;

static void
expanded_group_data_free (ExpandedGroupData *data)
{
  g_object_unref (data->view);
  g_weak_ref_clear (&data->group);
  g_free (data);
}

/* What _setExpandedGroup () did after it awaited the collapse of the group
 * that was expanded before */
static void
view_continue_set_expanded_group (gpointer user_data)
{
  ExpandedGroupData *data = user_data;
  ShellMessageView *self = data->view;
  g_autoptr (ClutterActor) group = g_weak_ref_get (&data->group);

  if (group != NULL)
    {
      ClutterActor *item = clutter_actor_get_parent (group);

      if (item == NULL)
        return;

      /* Make sure that the overlay is the child below the expanded group */
      clutter_actor_set_child_above_sibling (CLUTTER_ACTOR (self), item, NULL);
      clutter_actor_set_child_below_sibling (CLUTTER_ACTOR (self), self->overlay, item);
      clutter_actor_show (self->overlay);
      view_scroll_to_expanded_group (self);

      view_highlight_group (self, group);
      group_expand (SHELL_NOTIFICATION_MESSAGE_GROUP (group));
    }
  else
    {
      clutter_actor_hide (self->overlay);
    }
}

static void
view_set_expanded_group (ShellMessageView *self,
                         ClutterActor     *group)
{
  ClutterActor *prev_group = self->expanded_group;
  ExpandedGroupData *data;

  if (prev_group == group)
    return;

  g_object_ref (self);

  if (prev_group != NULL)
    g_object_ref (prev_group);

  g_set_weak_pointer (&self->expanded_group, group);
  g_object_notify_by_pspec (G_OBJECT (self), view_props[VIEW_PROP_EXPANDED_GROUP]);

  data = g_new0 (ExpandedGroupData, 1);
  data->view = g_object_ref (self);
  g_weak_ref_init (&data->group, group);

  /* Collapse the previously expanded group */
  if (prev_group != NULL)
    {
      view_unhighlight_group (self);
      group_collapse (SHELL_NOTIFICATION_MESSAGE_GROUP (prev_group),
                      view_continue_set_expanded_group, data,
                      (GDestroyNotify) expanded_group_data_free);
      g_object_unref (prev_group);
    }
  else
    {
      view_continue_set_expanded_group (data);
      expanded_group_data_free (data);
    }

  g_object_unref (self);
}

/**
 * shell_message_view_collapse:
 * @self: a #ShellMessageView
 *
 * Collapses the notification group that is expanded.
 */
void
shell_message_view_collapse (ShellMessageView *self)
{
  g_return_if_fail (SHELL_IS_MESSAGE_VIEW (self));

  view_set_expanded_group (self, NULL);
}

/* ------------------------------------------------------------------------ */
/* Notification sources                                                      */
/* ------------------------------------------------------------------------ */

static int
view_group_index (ShellMessageView                *self,
                  ShellNotificationMessageGroup *group)
{
  return self->n_media + (group_get_has_urgent (group) ? 0 : self->n_urgent);
}

static void
group_focus_child_changed_cb (ShellNotificationMessageGroup *group,
                              GParamSpec                    *pspec,
                              ShellMessageView              *self)
{
  ClutterActor *focus_child = group->focus_child;

  if (focus_child != NULL)
    view_key_focus_in_cb (focus_child, self);
}

static void
group_expand_toggle_requested_cb (ShellNotificationMessageGroup *group,
                                  ShellMessageView              *self)
{
  if (group_get_expanded (group))
    view_set_expanded_group (self, NULL);
  else
    view_set_expanded_group (self, CLUTTER_ACTOR (group));
}

static void
group_has_urgent_changed_cb (ShellNotificationMessageGroup *group,
                             GParamSpec                    *pspec,
                             ShellMessageView              *self)
{
  if (group_get_has_urgent (group))
    self->n_urgent++;
  else
    self->n_urgent--;

  view_move_message (self, CLUTTER_ACTOR (group), view_group_index (self, group));
}

static void
group_notification_added_cb (ShellNotificationMessageGroup *group,
                             ShellMessageView              *self)
{
  view_move_message (self, CLUTTER_ACTOR (group), view_group_index (self, group));
}

static void
view_add_notification_source (ShellMessageView *self,
                              ShellSource      *source)
{
  ClutterActor *group = notification_message_group_new (source);
  ShellNotificationMessageGroup *group_self = SHELL_NOTIFICATION_MESSAGE_GROUP (group);

  g_object_ref_sink (group);
  g_hash_table_insert (self->source_to_group, g_object_ref (source), group);

  g_signal_connect_object (group, "notify::focus-child",
                           G_CALLBACK (group_focus_child_changed_cb), self, 0);
  g_signal_connect_object (group, "expand-toggle-requested",
                           G_CALLBACK (group_expand_toggle_requested_cb), self, 0);
  g_signal_connect_object (group, "notify::has-urgent",
                           G_CALLBACK (group_has_urgent_changed_cb), self, 0);
  g_signal_connect_object (group, "notification-added",
                           G_CALLBACK (group_notification_added_cb), self, 0);

  if (group_get_has_urgent (group_self))
    self->n_urgent++;

  view_add_message_at_index (self, group, view_group_index (self, group_self));
  g_object_unref (group);
}

static void
view_remove_notification_source (ShellMessageView *self,
                                 ShellSource      *source)
{
  ClutterActor *group = g_hash_table_lookup (self->source_to_group, source);

  if (group == NULL)
    return;

  view_remove_message (self, group);

  if (group_get_has_urgent (SHELL_NOTIFICATION_MESSAGE_GROUP (group)))
    self->n_urgent--;

  g_hash_table_remove (self->source_to_group, source);
}

static void
tray_source_added_cb (ShellMessageTray *tray,
                      ShellSource      *source,
                      ShellMessageView *self)
{
  view_add_notification_source (self, source);
}

static void
tray_source_removed_cb (ShellMessageTray *tray,
                        ShellSource      *source,
                        ShellMessageView *self)
{
  view_remove_notification_source (self, source);
}

/* ------------------------------------------------------------------------ */
/* Scrolling                                                                 */
/* ------------------------------------------------------------------------ */

static void
view_clear_adjustment_bindings (ShellMessageView *self)
{
  if (self->value_binding != NULL)
    {
      g_binding_unbind (self->value_binding);
      g_clear_object (&self->value_binding);
    }

  if (self->adj_binding != NULL)
    {
      g_binding_group_set_source (self->adj_binding, NULL);
      g_clear_object (&self->adj_binding);
    }
  self->adj_binding_active = FALSE;

  g_clear_object (&self->scroll_view_adjustment);
}

static gboolean
adjustment_value_to_scroll_view (GBinding     *binding,
                                 const GValue *from_value,
                                 GValue       *to_value,
                                 gpointer      user_data)
{
  ShellMessageView *self = user_data;

  g_value_set_double (to_value, g_value_get_double (from_value) - self->adj_value_offset);
  return TRUE;
}

static gboolean
adjustment_value_from_scroll_view (GBinding     *binding,
                                   const GValue *from_value,
                                   GValue       *to_value,
                                   gpointer      user_data)
{
  ShellMessageView *self = user_data;

  g_value_set_double (to_value, g_value_get_double (from_value) + self->adj_value_offset);
  return TRUE;
}

/*
 * When a group is expanded the user isn't allowed to scroll outside the
 * expanded group, therefore the adjustment used by the view needs to be
 * different then the external adjustment used by the scrollbar and
 * scrollview.
 */
static void
shell_message_view_set_adjustments (StScrollable *scrollable,
                                    StAdjustment *hadjustment,
                                    StAdjustment *vadjustment)
{
  ShellMessageView *self = SHELL_MESSAGE_VIEW (scrollable);
  g_autoptr (StAdjustment) internal_adjustment = NULL;
  ClutterActor *actor = NULL;

  view_clear_adjustment_bindings (self);
  self->adj_value_offset = 0;

  if (vadjustment == NULL)
    {
      parent_scrollable_iface->set_adjustments (scrollable, hadjustment, NULL);
      return;
    }

  g_object_get (vadjustment, "actor", &actor, NULL);
  internal_adjustment = g_object_new (ST_TYPE_ADJUSTMENT, "actor", actor, NULL);
  g_clear_object (&actor);

  self->scroll_view_adjustment = g_object_ref (vadjustment);

  self->adj_binding = g_binding_group_new ();
  g_binding_group_bind (self->adj_binding, "lower",
                        self->scroll_view_adjustment, "lower", G_BINDING_SYNC_CREATE);
  g_binding_group_bind (self->adj_binding, "upper",
                        self->scroll_view_adjustment, "upper", G_BINDING_SYNC_CREATE);
  g_binding_group_bind (self->adj_binding, "step-increment",
                        self->scroll_view_adjustment, "step-increment", G_BINDING_SYNC_CREATE);
  g_binding_group_bind (self->adj_binding, "page-increment",
                        self->scroll_view_adjustment, "page-increment", G_BINDING_SYNC_CREATE);
  g_binding_group_bind (self->adj_binding, "page-size",
                        self->scroll_view_adjustment, "page-size", G_BINDING_SYNC_CREATE);

  self->value_binding =
    g_object_ref (g_object_bind_property_full (internal_adjustment, "value",
                                               self->scroll_view_adjustment, "value",
                                               G_BINDING_BIDIRECTIONAL,
                                               adjustment_value_to_scroll_view,
                                               adjustment_value_from_scroll_view,
                                               self, NULL));

  parent_scrollable_iface->set_adjustments (scrollable, hadjustment, internal_adjustment);
}

static void
shell_message_view_scrollable_init (StScrollableInterface *iface)
{
  parent_scrollable_iface = g_type_interface_peek_parent (iface);

  iface->set_adjustments = shell_message_view_set_adjustments;
  iface->get_adjustments = parent_scrollable_iface->get_adjustments;
}

static void
shell_message_view_allocate (ClutterActor          *actor,
                             const ClutterActorBox *box)
{
  ShellMessageView *self = SHELL_MESSAGE_VIEW (actor);
  ClutterActor *group = self->expanded_group;
  StAdjustment *vadjustment = view_get_vadjustment (self);
  double prev_upper = 0;
  double prev_adj_value_offset = self->adj_value_offset;

  if (vadjustment == NULL)
    {
      CLUTTER_ACTOR_CLASS (shell_message_view_parent_class)->allocate (actor, box);
      return;
    }

  g_object_freeze_notify (G_OBJECT (vadjustment));

  st_adjustment_get_values (vadjustment, NULL, NULL, &prev_upper, NULL, NULL, NULL);

  CLUTTER_ACTOR_CLASS (shell_message_view_parent_class)->allocate (actor, box);

  if (group != NULL && self->adj_binding != NULL)
    {
      graphene_point3d_t origin = GRAPHENE_POINT3D_INIT (0, 0, 0);
      graphene_point3d_t position;
      float group_height;
      double page_height, upper;

      /* Decouple the internal adjustment from the external when there is an
       * expanded group */
      g_binding_group_set_source (self->adj_binding, NULL);
      self->adj_binding_active = FALSE;

      st_adjustment_get_values (vadjustment, NULL, NULL, NULL, NULL, NULL, &page_height);
      clutter_actor_apply_relative_transform_to_point (group, actor, &origin, &position);
      clutter_actor_get_preferred_height (group, box->x2 - box->x1, &group_height, NULL);

      self->adj_value_offset = MAX (0, position.y - EXPANDED_GROUP_OVERSHOT_HEIGHT);

      /* Limit the area the user can scroll to the expanded group with some
       * extra space */
      g_object_freeze_notify (G_OBJECT (self->scroll_view_adjustment));
      upper = MAX (group_height + EXPANDED_GROUP_OVERSHOT_HEIGHT * 2, page_height);
      g_object_set (self->scroll_view_adjustment,
                    "upper", upper,
                    "step-increment", page_height / 6,
                    "page-increment", page_height - page_height / 6,
                    "page-size", page_height,
                    NULL);
      g_object_thaw_notify (G_OBJECT (self->scroll_view_adjustment));

      /* Adjust the value when new messages are added before the expanded
       * group */
      if (self->adj_value_offset > prev_adj_value_offset)
        {
          double offset;

          st_adjustment_get_values (vadjustment, NULL, NULL, &upper, NULL, NULL, NULL);
          offset = upper - prev_upper;
          if (offset > 0)
            st_adjustment_set_value (vadjustment, st_adjustment_get_value (vadjustment) + offset);
        }
    }
  else if (group == NULL && self->adj_binding != NULL && !self->adj_binding_active)
    {
      self->adj_value_offset = 0;
      g_binding_group_set_source (self->adj_binding, vadjustment);
      self->adj_binding_active = TRUE;
      /* We need to notify the 'value' property since it indirectly changed */
      g_object_notify (G_OBJECT (vadjustment), "value");
    }

  g_object_thaw_notify (G_OBJECT (vadjustment));
}

static void
shell_message_view_style_changed (StWidget *widget)
{
  ShellMessageView *self = SHELL_MESSAGE_VIEW (widget);
  StThemeNode *theme_node = st_widget_get_theme_node (widget);
  double fade_offset;

  /* This widget doesn't use the normal st scroll view fade effect because
   * highlighting groups needs more control over the fade. */
  fade_offset = st_theme_node_get_length (theme_node, "-st-vfade-offset");
  if (self->fade != NULL)
    g_object_set (self->fade, "fade-margin", (float) fade_offset, NULL);

  ST_WIDGET_CLASS (shell_message_view_parent_class)->style_changed (widget);
}

static GList *
shell_message_view_get_focus_chain (StWidget *widget)
{
  ShellMessageView *self = SHELL_MESSAGE_VIEW (widget);
  GList *chain = NULL;
  guint i;

  if (self->expanded_group != NULL)
    return g_list_append (NULL, self->expanded_group);

  for (i = 0; i < self->messages->len; i++)
    {
      ClutterActor *message = g_ptr_array_index (self->messages, i);

      if (clutter_actor_is_visible (message))
        chain = g_list_prepend (chain, clutter_actor_get_parent (message));
    }

  return g_list_reverse (chain);
}

/* ------------------------------------------------------------------------ */
/* Object                                                                    */
/* ------------------------------------------------------------------------ */

/**
 * shell_message_view_new:
 * @message_tray: the tray whose sources the view shows
 *
 * Creates a view with a group for each source of @message_tray.
 *
 * Returns: (transfer floating): a new #ShellMessageView
 */
ShellMessageView *
shell_message_view_new (ShellMessageTray *message_tray)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_TRAY (message_tray), NULL);

  return g_object_new (SHELL_TYPE_MESSAGE_VIEW,
                       "message-tray", message_tray,
                       NULL);
}

static void
shell_message_view_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  ShellMessageView *self = SHELL_MESSAGE_VIEW (object);

  switch (prop_id)
    {
    case VIEW_PROP_MESSAGE_TRAY:
      g_value_set_object (value, self->message_tray);
      break;

    case VIEW_PROP_CAN_CLEAR:
      g_value_set_boolean (value, shell_message_view_get_can_clear (self));
      break;

    case VIEW_PROP_EMPTY:
      g_value_set_boolean (value, shell_message_view_get_empty (self));
      break;

    case VIEW_PROP_EXPANDED_GROUP:
      g_value_set_object (value, self->expanded_group);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_message_view_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  ShellMessageView *self = SHELL_MESSAGE_VIEW (object);

  switch (prop_id)
    {
    case VIEW_PROP_MESSAGE_TRAY:
      g_set_object (&self->message_tray, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_message_view_constructed (GObject *object)
{
  ShellMessageView *self = SHELL_MESSAGE_VIEW (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellMessageViewLayout *layout;
  g_autoptr (GList) sources = NULL;
  GList *l;

  G_OBJECT_CLASS (shell_message_view_parent_class)->constructed (object);

  /* Add an overlay that will be placed below the expanded group message to
   * block interaction with other messages. Unfortunately there isn't a much
   * better way to block interaction with widgets for this use-case. */
  self->overlay = g_object_new (CLUTTER_TYPE_ACTOR,
                                "reactive", TRUE,
                                "name", "overlay",
                                "visible", FALSE,
                                NULL);

  layout = g_object_new (SHELL_TYPE_MESSAGE_VIEW_LAYOUT, NULL);
  g_set_weak_pointer (&layout->overlay, self->overlay);
  clutter_actor_set_layout_manager (actor, CLUTTER_LAYOUT_MANAGER (layout));

  st_widget_set_style_class_name (ST_WIDGET (self), "message-view");
  clutter_actor_set_x_expand (actor, TRUE);
  clutter_actor_set_y_expand (actor, TRUE);

  self->fade = g_object_new (SHELL_TYPE_MESSAGE_VIEW_FADE, NULL);
  clutter_actor_add_effect_with_name (actor, "highlight", CLUTTER_EFFECT (self->fade));

  clutter_actor_add_child (actor, self->overlay);

  if (self->message_tray != NULL)
    {
      g_signal_connect_object (self->message_tray, "source-added",
                               G_CALLBACK (tray_source_added_cb), self, 0);
      g_signal_connect_object (self->message_tray, "source-removed",
                               G_CALLBACK (tray_source_removed_cb), self, 0);

      sources = shell_message_tray_get_sources (self->message_tray);
      for (l = sources; l != NULL; l = l->next)
        view_add_notification_source (self, l->data);
    }
}

static void
shell_message_view_dispose (GObject *object)
{
  ShellMessageView *self = SHELL_MESSAGE_VIEW (object);
  guint i;

  view_clear_adjustment_bindings (self);

  if (self->message_tray != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->message_tray, self);
      g_clear_object (&self->message_tray);
    }

  for (i = 0; i < self->messages->len; i++)
    g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->messages, i), self);
  g_ptr_array_set_size (self->messages, 0);
  g_hash_table_remove_all (self->source_to_group);

  g_clear_weak_pointer (&self->expanded_group);
  self->fade = NULL;

  G_OBJECT_CLASS (shell_message_view_parent_class)->dispose (object);
}

static void
shell_message_view_finalize (GObject *object)
{
  ShellMessageView *self = SHELL_MESSAGE_VIEW (object);

  g_clear_pointer (&self->messages, g_ptr_array_unref);
  g_clear_pointer (&self->source_to_group, g_hash_table_unref);

  G_OBJECT_CLASS (shell_message_view_parent_class)->finalize (object);
}

static void
shell_message_view_class_init (ShellMessageViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);

  object_class->get_property = shell_message_view_get_property;
  object_class->set_property = shell_message_view_set_property;
  object_class->constructed = shell_message_view_constructed;
  object_class->dispose = shell_message_view_dispose;
  object_class->finalize = shell_message_view_finalize;

  actor_class->allocate = shell_message_view_allocate;

  widget_class->style_changed = shell_message_view_style_changed;
  widget_class->get_focus_chain = shell_message_view_get_focus_chain;

  /**
   * ShellMessageView:message-tray:
   *
   * The tray whose sources the view shows.
   */
  view_props[VIEW_PROP_MESSAGE_TRAY] =
    g_param_spec_object ("message-tray", NULL, NULL,
                         SHELL_TYPE_MESSAGE_TRAY,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellMessageView:can-clear:
   *
   * Whether there is a message that can be closed.
   */
  view_props[VIEW_PROP_CAN_CLEAR] =
    g_param_spec_boolean ("can-clear", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellMessageView:empty:
   *
   * Whether the view has no message.
   */
  view_props[VIEW_PROP_EMPTY] =
    g_param_spec_boolean ("empty", NULL, NULL,
                          TRUE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellMessageView:expanded-group:
   *
   * The group of notifications that is expanded, if any.
   */
  view_props[VIEW_PROP_EXPANDED_GROUP] =
    g_param_spec_object ("expanded-group", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, VIEW_N_PROPS, view_props);

  /**
   * ShellMessageView::message-focused:
   * @self: the view
   * @message: the message that received the key focus
   */
  view_signals[VIEW_MESSAGE_FOCUSED] =
    g_signal_new ("message-focused",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_MESSAGE);
}

static void
shell_message_view_init (ShellMessageView *self)
{
  self->messages = g_ptr_array_new ();
  self->source_to_group = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                 g_object_unref, NULL);
}
