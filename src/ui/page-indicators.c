/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * page-indicators.c: A row (or column) of dots showing which page of a
 * paged view is visible. Port of js/ui/pageIndicators.js.
 */

#include "config.h"

#include <math.h>

#include "ui/page-indicators.h"

#define INDICATOR_INACTIVE_OPACITY 128
#define INDICATOR_INACTIVE_OPACITY_HOVER 255
#define INDICATOR_INACTIVE_SCALE (2.0 / 3.0)
#define INDICATOR_INACTIVE_SCALE_PRESSED 0.5

enum
{
  PROP_0,
  PROP_N_PAGES,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  PAGE_ACTIVATED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellPageIndicators
{
  StBoxLayout parent;

  int n_pages;
  double current_position;
  gboolean indicators_reactive;
};

G_DEFINE_FINAL_TYPE (ShellPageIndicators, shell_page_indicators, ST_TYPE_BOX_LAYOUT)

static void
update_indicator (ShellPageIndicators *self,
                  StButton            *indicator,
                  int                  page_index)
{
  ClutterActor *icon = st_bin_get_child (ST_BIN (indicator));
  double progress, inactive_scale, scale, inactive_opacity, opacity;

  if (icon == NULL)
    return;

  progress = MAX (1 - fabs (self->current_position - page_index), 0);

  inactive_scale = st_button_get_pressed (indicator)
    ? INDICATOR_INACTIVE_SCALE_PRESSED : INDICATOR_INACTIVE_SCALE;
  inactive_opacity = st_widget_get_hover (ST_WIDGET (indicator))
    ? INDICATOR_INACTIVE_OPACITY_HOVER : INDICATOR_INACTIVE_OPACITY;

  scale = inactive_scale + (1 - inactive_scale) * progress;
  opacity = inactive_opacity + (255 - inactive_opacity) * progress;

  clutter_actor_set_scale (icon, scale, scale);
  clutter_actor_set_opacity (icon, (uint8_t) CLAMP (opacity, 0, 255));
}

static int
indicator_page_index (StButton *indicator)
{
  return GPOINTER_TO_INT (g_object_get_data (G_OBJECT (indicator),
                                             "shell-page-index"));
}

static void
indicator_clicked_cb (StButton            *indicator,
                      ShellPageIndicators *self)
{
  g_signal_emit (self, signals[PAGE_ACTIVATED], 0,
                 indicator_page_index (indicator));
}

static void
indicator_state_changed_cb (StButton            *indicator,
                            GParamSpec          *pspec,
                            ShellPageIndicators *self)
{
  update_indicator (self, indicator, indicator_page_index (indicator));
}

static void
add_indicator (ShellPageIndicators *self,
               int                  page_index)
{
  ClutterActor *icon;
  ClutterActor *indicator;

  icon = g_object_new (ST_TYPE_WIDGET,
                       "style-class", "page-indicator-icon",
                       NULL);
  clutter_actor_set_pivot_point (icon, 0.5, 0.5);

  indicator = g_object_new (ST_TYPE_BUTTON,
                            "style-class", "page-indicator",
                            "button-mask", ST_BUTTON_PRIMARY |
                                           ST_BUTTON_MIDDLE |
                                           ST_BUTTON_SECONDARY,
                            "reactive", self->indicators_reactive,
                            "child", icon,
                            NULL);
  g_object_set_data (G_OBJECT (indicator), "shell-page-index",
                     GINT_TO_POINTER (page_index));

  g_signal_connect_object (indicator, "clicked",
                           G_CALLBACK (indicator_clicked_cb), self, 0);
  g_signal_connect_object (indicator, "notify::hover",
                           G_CALLBACK (indicator_state_changed_cb), self, 0);
  g_signal_connect_object (indicator, "notify::pressed",
                           G_CALLBACK (indicator_state_changed_cb), self, 0);

  update_indicator (self, ST_BUTTON (indicator), page_index);
  clutter_actor_add_child (CLUTTER_ACTOR (self), indicator);
}

/**
 * shell_page_indicators_new:
 * @orientation: whether the dots are laid out in a row or in a column
 *
 * The dots are aligned to the end of the cross axis and centered along the
 * main axis, as in the former `new PageIndicators(orientation)`.
 *
 * Returns: (transfer floating): a new #ShellPageIndicators
 */
ShellPageIndicators *
shell_page_indicators_new (ClutterOrientation orientation)
{
  return g_object_new (SHELL_TYPE_PAGE_INDICATORS,
                       "orientation", orientation,
                       NULL);
}

/**
 * shell_page_indicators_set_indicators_reactive:
 * @self: a #ShellPageIndicators
 * @reactive: whether the individual dots react to input
 *
 * Makes the dots (but not the container itself) reactive or not. This is the
 * former `setReactive()`; it has another name because `set_reactive()` is
 * already ClutterActor's.
 */
void
shell_page_indicators_set_indicators_reactive (ShellPageIndicators *self,
                                               gboolean             reactive)
{
  ClutterActor *child;

  g_return_if_fail (SHELL_IS_PAGE_INDICATORS (self));

  for (child = clutter_actor_get_first_child (CLUTTER_ACTOR (self));
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    clutter_actor_set_reactive (child, reactive);

  self->indicators_reactive = !!reactive;
}

/**
 * shell_page_indicators_set_n_pages:
 * @self: a #ShellPageIndicators
 * @n_pages: the number of pages, hence of dots
 *
 * Adds or removes dots at the end. The widget is only visible when there is
 * more than one page.
 */
void
shell_page_indicators_set_n_pages (ShellPageIndicators *self,
                                   int                  n_pages)
{
  int diff;

  g_return_if_fail (SHELL_IS_PAGE_INDICATORS (self));
  g_return_if_fail (n_pages >= 0);

  if (self->n_pages == n_pages)
    return;

  diff = n_pages - self->n_pages;
  if (diff > 0)
    {
      int i;

      for (i = 0; i < diff; i++)
        add_indicator (self, self->n_pages + i);
    }
  else
    {
      int i;

      for (i = 0; i < -diff; i++)
        {
          ClutterActor *last =
            clutter_actor_get_last_child (CLUTTER_ACTOR (self));

          if (last == NULL)
            break;

          clutter_actor_destroy (last);
        }
    }

  self->n_pages = n_pages;
  if (self->n_pages > 1)
    clutter_actor_show (CLUTTER_ACTOR (self));
  else
    clutter_actor_hide (CLUTTER_ACTOR (self));

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_N_PAGES]);
}

/**
 * shell_page_indicators_get_n_pages:
 * @self: a #ShellPageIndicators
 *
 * Returns: the number of pages
 */
int
shell_page_indicators_get_n_pages (ShellPageIndicators *self)
{
  g_return_val_if_fail (SHELL_IS_PAGE_INDICATORS (self), 0);

  return self->n_pages;
}

/**
 * shell_page_indicators_set_current_position:
 * @self: a #ShellPageIndicators
 * @position: the position in pages, fractional while a scroll is in progress
 *
 * Highlights the dots according to their distance from @position: dot `i` is
 * fully active when @position is `i`, and fades to inactive one page away.
 */
void
shell_page_indicators_set_current_position (ShellPageIndicators *self,
                                            double               position)
{
  ClutterActor *child;
  int i = 0;

  g_return_if_fail (SHELL_IS_PAGE_INDICATORS (self));

  self->current_position = position;

  for (child = clutter_actor_get_first_child (CLUTTER_ACTOR (self));
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    update_indicator (self, ST_BUTTON (child), i++);
}

static void
shell_page_indicators_get_preferred_height (ClutterActor *actor,
                                            float         for_width,
                                            float        *min_height_p,
                                            float        *natural_height_p)
{
  float natural_height;

  /* We want to request the natural height of all our children as our
   * natural height, so we chain up to StBoxLayout, but we only request 0
   * as minimum height, since it's not that important if some indicators
   * are not shown
   */
  CLUTTER_ACTOR_CLASS (shell_page_indicators_parent_class)->get_preferred_height
    (actor, for_width, NULL, &natural_height);

  if (min_height_p)
    *min_height_p = 0;
  if (natural_height_p)
    *natural_height_p = natural_height;
}

static void
update_alignment (ShellPageIndicators *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  gboolean vertical =
    st_box_layout_get_orientation (ST_BOX_LAYOUT (self)) == CLUTTER_ORIENTATION_VERTICAL;

  clutter_actor_set_x_align (actor, vertical ? CLUTTER_ACTOR_ALIGN_END
                                             : CLUTTER_ACTOR_ALIGN_CENTER);
  clutter_actor_set_y_align (actor, vertical ? CLUTTER_ACTOR_ALIGN_CENTER
                                             : CLUTTER_ACTOR_ALIGN_END);
}

static void
orientation_changed_cb (GObject    *object,
                        GParamSpec *pspec,
                        gpointer    user_data)
{
  update_alignment (SHELL_PAGE_INDICATORS (object));
}

static void
shell_page_indicators_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  ShellPageIndicators *self = SHELL_PAGE_INDICATORS (object);

  switch (prop_id)
    {
    case PROP_N_PAGES:
      g_value_set_int (value, self->n_pages);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void shell_page_indicators_constructed (GObject *object);

static void
shell_page_indicators_class_init (ShellPageIndicatorsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_page_indicators_constructed;
  object_class->get_property = shell_page_indicators_get_property;

  actor_class->get_preferred_height = shell_page_indicators_get_preferred_height;

  /**
   * ShellPageIndicators:n-pages:
   *
   * The number of pages, that is of dots. Read-only, use
   * shell_page_indicators_set_n_pages().
   */
  props[PROP_N_PAGES] =
    g_param_spec_int ("n-pages", NULL, NULL,
                      0, G_MAXINT, 0,
                      G_PARAM_READABLE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellPageIndicators::page-activated:
   * @self: the #ShellPageIndicators
   * @page: the index of the page whose dot was clicked
   */
  signals[PAGE_ACTIVATED] =
    g_signal_new ("page-activated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT);
}

static void
shell_page_indicators_init (ShellPageIndicators *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  self->indicators_reactive = TRUE;

  st_widget_set_style_class_name (ST_WIDGET (self), "page-indicators");
  clutter_actor_set_x_expand (actor, TRUE);
  clutter_actor_set_y_expand (actor, TRUE);
  clutter_actor_set_reactive (actor, TRUE);
  clutter_actor_set_clip_to_allocation (actor, TRUE);
}

/* StBoxLayout's ClutterBoxLayout is created by ClutterActor's constructed(),
 * so the orientation can only be touched from here on, never from init(). */
static void
shell_page_indicators_constructed (GObject *object)
{
  ShellPageIndicators *self = SHELL_PAGE_INDICATORS (object);

  G_OBJECT_CLASS (shell_page_indicators_parent_class)->constructed (object);

  /* The JS constructor defaulted to a vertical layout. "orientation" is
   * not a construct property, so a value passed to g_object_new() is
   * applied after this and re-runs update_alignment() through notify. */
  st_box_layout_set_orientation (ST_BOX_LAYOUT (self),
                                 CLUTTER_ORIENTATION_VERTICAL);
  update_alignment (self);

  /* The alignment follows the orientation given at construction (and any
   * later change); callers that want something else set it afterwards. */
  g_signal_connect (self, "notify::orientation",
                    G_CALLBACK (orientation_changed_cb), NULL);
}
