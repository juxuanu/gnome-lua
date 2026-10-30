/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * switcher-popup.c: The popup of Alt+Tab and its relatives. Port of
 * js/ui/switcherPopup.js.
 *
 * Differences from the JS classes, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The JS popup kept its items in the array `_items`, which the subclasses
 *    filled and mutated (`this._items = this._switcherList.icons`). The items
 *    are not known here: the popup only asks the subclass how many there are
 *    (the `get_n_items` virtual function). The subclasses keep their own
 *    `_items`.
 *  - `show ()`, which clashes with the `show` of #ClutterActor, is
 *    shell_switcher_popup_show_switcher ().
 *  - `_select ()` of the JS class is the `select` virtual function and
 *    `_selectedIndex` is the #ShellSwitcherPopup:selected-index property. The
 *    `_select (app, window, forceAppFocus)` of the application switcher is a
 *    function of alt-tab.c of its own.
 *  - `Main.layoutManager.connectObject ('system-modal-opened', ...)` is a
 *    handler of #ShellMain::system-modal-opened, `Main.osdWindowManager` is
 *    #ShellMain:osd-window-manager and `Main.layoutManager.primary_monitor`
 *    and `uiGroup` are read from #ShellMain:layout-manager.
 *  - The JS `SwitcherButton` is private and has no `_delegate`; the
 *    `spacing` of the item container, which the JS code stored as an expando
 *    property of the container when it changed, is read from its theme node
 *    by the application switcher.
 *  - The pending timeouts are removed on ::shutdown as well as when the popup
 *    is destroyed.
 */

#include "config.h"

#include <math.h>

#include <cairo.h>
#include <clutter/clutter.h>
/* clutter.h does not include the motion controller (mutter 51) */
#define __CLUTTER_H_INSIDE__
#include <clutter/clutter-motion-controller.h>
#undef __CLUTTER_H_INSIDE__
#include <meta/display.h>

#include "shell-action-modes.h"
#include "shell-global.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/osd-window.h"
#include "ui/switcher-popup.h"

#define POPUP_DELAY_TIMEOUT 150 /* milliseconds */

#define POPUP_SCROLL_TIME 100 /* milliseconds */
#define POPUP_FADE_OUT_TIME 100 /* milliseconds */

#define DISABLE_HOVER_TIMEOUT 500 /* milliseconds */
#define NO_MODS_TIMEOUT 1500 /* milliseconds */

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * shell_switcher_popup_mod:
 * @a: a number
 * @b: the modulus
 *
 * Returns: @a modulo @b, but never negative for @a > -@b (`mod ()` of
 *   switcherPopup.js). 0 if @b is 0
 */
int
shell_switcher_popup_mod (int a,
                          int b)
{
  if (b == 0)
    return 0;

  return (a + b) % b;
}

static uint32_t
primary_modifier (uint32_t mask)
{
  uint32_t primary;

  if (mask == 0)
    return 0;

  primary = 1;
  while (mask > 1)
    {
      mask >>= 1;
      primary <<= 1;
    }

  return primary;
}

static void
set_source_color (cairo_t         *cr,
                  const CoglColor *color)
{
  cairo_set_source_rgba (cr,
                         color->red / 255.,
                         color->green / 255.,
                         color->blue / 255.,
                         color->alpha / 255.);
}

/**
 * shell_switcher_draw_arrow:
 * @area: the drawing area to paint on
 * @side: the side the arrow points to
 *
 * Paints an arrow, outlined with the border color of @side and filled with
 * the foreground color of @area (`drawArrow ()`). Call this from the
 * ::repaint handler of @area.
 */
void
shell_switcher_draw_arrow (StDrawingArea *area,
                           StSide         side)
{
  StThemeNode *theme_node;
  CoglColor border_color, body_color;
  unsigned int surface_width, surface_height;
  double width, height;
  cairo_t *cr;

  g_return_if_fail (ST_IS_DRAWING_AREA (area));

  theme_node = st_widget_get_theme_node (ST_WIDGET (area));
  st_theme_node_get_border_color (theme_node, side, &border_color);
  st_theme_node_get_foreground_color (theme_node, &body_color);

  st_drawing_area_get_surface_size (area, &surface_width, &surface_height);
  width = surface_width;
  height = surface_height;
  cr = st_drawing_area_get_context (area);

  cairo_set_line_width (cr, 1.0);
  set_source_color (cr, &border_color);

  switch (side)
    {
    case ST_SIDE_TOP:
      cairo_move_to (cr, 0, height);
      cairo_line_to (cr, floor (width * 0.5), 0);
      cairo_line_to (cr, width, height);
      break;

    case ST_SIDE_BOTTOM:
      cairo_move_to (cr, width, 0);
      cairo_line_to (cr, floor (width * 0.5), height);
      cairo_line_to (cr, 0, 0);
      break;

    case ST_SIDE_LEFT:
      cairo_move_to (cr, width, height);
      cairo_line_to (cr, 0, floor (height * 0.5));
      cairo_line_to (cr, width, 0);
      break;

    case ST_SIDE_RIGHT:
      cairo_move_to (cr, 0, 0);
      cairo_line_to (cr, width, floor (height * 0.5));
      cairo_line_to (cr, 0, height);
      break;

    default:
      break;
    }

  cairo_stroke_preserve (cr);

  set_source_color (cr, &body_color);
  cairo_fill (cr);
  /* The context is the drawing area's: it destroys it after the repaint */
}

static void
arrow_repaint_cb (StDrawingArea *area,
                  gpointer       side)
{
  shell_switcher_draw_arrow (area, GPOINTER_TO_INT (side));
}

/* ------------------------------------------------------------------------ */
/* ShellSwitcherButton: the box around an item of a ShellSwitcherList        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SWITCHER_BUTTON (shell_switcher_button_get_type ())
G_DECLARE_FINAL_TYPE (ShellSwitcherButton, shell_switcher_button,
                      SHELL, SWITCHER_BUTTON, StButton)

struct _ShellSwitcherButton
{
  StButton parent;

  ShellSwitcherList *list; /* weak */
  gboolean square;
};

G_DEFINE_FINAL_TYPE (ShellSwitcherButton, shell_switcher_button, ST_TYPE_BUTTON)

static void
shell_switcher_button_get_preferred_width (ClutterActor *actor,
                                           float         for_height,
                                           float        *min_width_p,
                                           float        *natural_width_p)
{
  ShellSwitcherButton *self = SHELL_SWITCHER_BUTTON (actor);

  if (self->square)
    {
      clutter_actor_get_preferred_height (actor, -1, min_width_p, natural_width_p);
      return;
    }

  CLUTTER_ACTOR_CLASS (shell_switcher_button_parent_class)->get_preferred_width (actor,
                                                                                  for_height,
                                                                                  min_width_p,
                                                                                  natural_width_p);
}

static void
shell_switcher_button_dispose (GObject *object)
{
  ShellSwitcherButton *self = SHELL_SWITCHER_BUTTON (object);

  g_clear_weak_pointer (&self->list);

  G_OBJECT_CLASS (shell_switcher_button_parent_class)->dispose (object);
}

static void
shell_switcher_button_class_init (ShellSwitcherButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->dispose = shell_switcher_button_dispose;
  actor_class->get_preferred_width = shell_switcher_button_get_preferred_width;
}

static void
shell_switcher_button_init (ShellSwitcherButton *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellSwitcherList                                                         */
/* ------------------------------------------------------------------------ */

enum
{
  LIST_PROP_0,

  LIST_PROP_SQUARE_ITEMS,
  LIST_PROP_HIGHLIGHTED,

  LIST_N_PROPS
};

static GParamSpec *list_props[LIST_N_PROPS] = { NULL, };

enum
{
  ITEM_ACTIVATED,
  ITEM_ENTERED,
  ITEM_REMOVED,

  LIST_LAST_SIGNAL
};

static guint list_signals[LIST_LAST_SIGNAL] = { 0, };

typedef struct
{
  StBoxLayout *list;           /* weak */
  StScrollView *scroll_view;   /* weak */
  StDrawingArea *left_arrow;   /* weak */
  StDrawingArea *right_arrow;  /* weak */

  GPtrArray *items;            /* ShellSwitcherButton, not owned */
  int highlighted;
  gboolean square_items;
  gboolean scrollable_right;
  gboolean scrollable_left;
} ShellSwitcherListPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellSwitcherList, shell_switcher_list, ST_TYPE_WIDGET)

/**
 * shell_switcher_list_get_n_items:
 * @self: a #ShellSwitcherList
 *
 * Returns: the number of items of the list (`_items.length`)
 */
int
shell_switcher_list_get_n_items (ShellSwitcherList *self)
{
  ShellSwitcherListPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SWITCHER_LIST (self), 0);

  priv = shell_switcher_list_get_instance_private (self);
  return priv->items->len;
}

/**
 * shell_switcher_list_get_item:
 * @self: a #ShellSwitcherList
 * @index: the index of an item
 *
 * Returns: (transfer none) (nullable): the button around the item at @index
 *   (`_items[index]`), or %NULL if there is none
 */
StButton *
shell_switcher_list_get_item (ShellSwitcherList *self,
                              int                index)
{
  ShellSwitcherListPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SWITCHER_LIST (self), NULL);

  priv = shell_switcher_list_get_instance_private (self);
  if (index < 0 || (guint) index >= priv->items->len)
    return NULL;

  return g_ptr_array_index (priv->items, index);
}

/**
 * shell_switcher_list_get_item_index:
 * @self: a #ShellSwitcherList
 * @item: a button returned by shell_switcher_list_add_item ()
 *
 * Returns: the index of @item (`_items.indexOf (item)`), or -1 if it is not
 *   an item of the list
 */
int
shell_switcher_list_get_item_index (ShellSwitcherList *self,
                                    StButton          *item)
{
  ShellSwitcherListPrivate *priv;
  guint i;

  g_return_val_if_fail (SHELL_IS_SWITCHER_LIST (self), -1);

  priv = shell_switcher_list_get_instance_private (self);
  for (i = 0; i < priv->items->len; i++)
    {
      if (g_ptr_array_index (priv->items, i) == (gpointer) item)
        return i;
    }

  return -1;
}

/**
 * shell_switcher_list_get_highlighted:
 * @self: a #ShellSwitcherList
 *
 * Returns: the index of the highlighted item, -1 if there is none
 *   (`_highlighted`)
 */
int
shell_switcher_list_get_highlighted (ShellSwitcherList *self)
{
  ShellSwitcherListPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SWITCHER_LIST (self), -1);

  priv = shell_switcher_list_get_instance_private (self);
  return priv->highlighted;
}

/**
 * shell_switcher_list_get_list:
 * @self: a #ShellSwitcherList
 *
 * Returns: (transfer none): the box that holds the buttons (`_list`)
 */
StBoxLayout *
shell_switcher_list_get_list (ShellSwitcherList *self)
{
  ShellSwitcherListPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SWITCHER_LIST (self), NULL);

  priv = shell_switcher_list_get_instance_private (self);
  return priv->list;
}

/**
 * shell_switcher_list_item_activated:
 * @self: a #ShellSwitcherList
 * @index: the index of the item
 *
 * Emits #ShellSwitcherList::item-activated (`_itemActivated ()`).
 */
void
shell_switcher_list_item_activated (ShellSwitcherList *self,
                                    int                index)
{
  g_return_if_fail (SHELL_IS_SWITCHER_LIST (self));

  g_signal_emit (self, list_signals[ITEM_ACTIVATED], 0, index);
}

/**
 * shell_switcher_list_item_entered:
 * @self: a #ShellSwitcherList
 * @index: the index of the item
 *
 * Emits #ShellSwitcherList::item-entered (`_itemEntered ()`).
 */
void
shell_switcher_list_item_entered (ShellSwitcherList *self,
                                  int                index)
{
  g_return_if_fail (SHELL_IS_SWITCHER_LIST (self));

  g_signal_emit (self, list_signals[ITEM_ENTERED], 0, index);
}

static void
item_clicked_cb (ShellSwitcherButton *button)
{
  if (button->list == NULL)
    return;

  shell_switcher_list_item_activated (button->list,
                                      shell_switcher_list_get_item_index (button->list,
                                                                          ST_BUTTON (button)));
}

static void
item_motion_cb (ShellSwitcherButton *button)
{
  ShellSwitcherListClass *klass;

  if (button->list == NULL)
    return;

  klass = SHELL_SWITCHER_LIST_GET_CLASS (button->list);
  klass->item_motion (button->list, ST_BUTTON (button));
}

static void
item_destroy_cb (ClutterActor      *button,
                 ShellSwitcherList *self)
{
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);

  g_ptr_array_remove (priv->items, button);
}

/**
 * shell_switcher_list_add_item:
 * @self: a #ShellSwitcherList
 * @item: the actor to show in the list
 * @label: (nullable): the actor that labels @item, for the accessibility
 *   tree
 *
 * Appends @item to the list, in a button that activates it when clicked
 * (`addItem ()`).
 *
 * Returns: (transfer none): the button around @item
 */
StButton *
shell_switcher_list_add_item (ShellSwitcherList *self,
                              ClutterActor      *item,
                              ClutterActor      *label)
{
  ShellSwitcherListPrivate *priv;
  ShellSwitcherButton *button;
  ClutterAction *motion_controller;

  g_return_val_if_fail (SHELL_IS_SWITCHER_LIST (self), NULL);
  g_return_val_if_fail (CLUTTER_IS_ACTOR (item), NULL);
  g_return_val_if_fail (label == NULL || CLUTTER_IS_ACTOR (label), NULL);

  priv = shell_switcher_list_get_instance_private (self);

  button = g_object_new (SHELL_TYPE_SWITCHER_BUTTON,
                         "style-class", "item-box",
                         "reactive", TRUE,
                         NULL);
  button->square = priv->square_items;
  g_set_weak_pointer (&button->list, self);

  st_bin_set_child (ST_BIN (button), item);
  clutter_actor_add_child (CLUTTER_ACTOR (priv->list), CLUTTER_ACTOR (button));

  g_signal_connect_object (button, "clicked",
                           G_CALLBACK (item_clicked_cb), button,
                           G_CONNECT_SWAPPED);

  motion_controller = clutter_motion_controller_new ();
  /* Swapped, so that it does not matter which arguments the signal has */
  g_signal_connect_object (motion_controller, "motion",
                           G_CALLBACK (item_motion_cb), button,
                           G_CONNECT_SWAPPED);
  clutter_actor_add_action (CLUTTER_ACTOR (button), motion_controller);

  st_widget_set_label_actor (ST_WIDGET (button), label);

  g_ptr_array_add (priv->items, button);
  g_signal_connect_object (button, "destroy",
                           G_CALLBACK (item_destroy_cb), self, 0);

  return ST_BUTTON (button);
}

/**
 * shell_switcher_list_remove_item:
 * @self: a #ShellSwitcherList
 * @index: the index of the item to remove
 *
 * Destroys the item at @index and emits #ShellSwitcherList::item-removed
 * (`removeItem ()`).
 */
void
shell_switcher_list_remove_item (ShellSwitcherList *self,
                                 int                index)
{
  ShellSwitcherListPrivate *priv;
  ClutterActor *item;

  g_return_if_fail (SHELL_IS_SWITCHER_LIST (self));

  priv = shell_switcher_list_get_instance_private (self);
  g_return_if_fail (index >= 0 && (guint) index < priv->items->len);

  item = g_ptr_array_index (priv->items, index);
  g_ptr_array_remove_index (priv->items, index);
  clutter_actor_destroy (item);

  g_signal_emit (self, list_signals[ITEM_REMOVED], 0, index);
}

/**
 * shell_switcher_list_add_accessible_state:
 * @self: a #ShellSwitcherList
 * @index: the index of an item
 * @state: the state to add
 *
 * Adds an accessible state to the button of an item (`addAccessibleState ()`).
 */
void
shell_switcher_list_add_accessible_state (ShellSwitcherList *self,
                                          int                index,
                                          AtkStateType       state)
{
  StButton *item;

  g_return_if_fail (SHELL_IS_SWITCHER_LIST (self));

  item = shell_switcher_list_get_item (self, index);
  g_return_if_fail (item != NULL);

  clutter_actor_add_accessible_state (CLUTTER_ACTOR (item), state);
}

/**
 * shell_switcher_list_remove_accessible_state:
 * @self: a #ShellSwitcherList
 * @index: the index of an item
 * @state: the state to remove
 *
 * Removes an accessible state from the button of an item
 * (`removeAccessibleState ()`).
 */
void
shell_switcher_list_remove_accessible_state (ShellSwitcherList *self,
                                             int                index,
                                             AtkStateType       state)
{
  StButton *item;

  g_return_if_fail (SHELL_IS_SWITCHER_LIST (self));

  item = shell_switcher_list_get_item (self, index);
  g_return_if_fail (item != NULL);

  clutter_actor_remove_accessible_state (CLUTTER_ACTOR (item), state);
}

static void
real_item_motion (ShellSwitcherList *self,
                  StButton          *item)
{
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);

  /* Avoid reentrancy */
  if (item == shell_switcher_list_get_item (self, priv->highlighted))
    return;

  shell_switcher_list_item_entered (self,
                                    shell_switcher_list_get_item_index (self, item));
}

typedef struct
{
  GWeakRef list;
  int index;
  gboolean to_right;
} ScrollData;

static void
scroll_data_free (gpointer user_data)
{
  ScrollData *data = user_data;

  g_weak_ref_clear (&data->list);
  g_free (data);
}

static void
scroll_done_cb (gboolean finished,
                gpointer user_data)
{
  ScrollData *data = user_data;
  g_autoptr (ShellSwitcherList) self = NULL;
  ShellSwitcherListPrivate *priv;

  /* onComplete: only when the animation ran to its end */
  if (!finished)
    return;

  self = g_weak_ref_get (&data->list);
  if (self == NULL)
    return;

  priv = shell_switcher_list_get_instance_private (self);
  if (data->to_right)
    {
      if ((guint) data->index == priv->items->len - 1)
        priv->scrollable_right = FALSE;
    }
  else
    {
      if (data->index == 0)
        priv->scrollable_left = FALSE;
    }

  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

/* _scrollToLeft () and _scrollToRight () of the JS list only differ in the
 * arrow they enable and in the item whose visibility they check once done */
static void
scroll_to (ShellSwitcherList *self,
           int                index,
           gboolean           to_right)
{
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);
  StAdjustment *adjustment = st_scroll_view_get_hadjustment (priv->scroll_view);
  StButton *item = shell_switcher_list_get_item (self, index);
  ClutterActorBox item_box;
  double value, upper, page_size;
  g_auto (GValue) target = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;
  ScrollData *data;
  ShellEaseParams params = {
    .duration = POPUP_SCROLL_TIME,
    .delay = -1,
    .mode = CLUTTER_EASE_OUT_QUAD,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  if (item == NULL)
    return;

  st_adjustment_get_values (adjustment, &value, NULL, &upper, NULL, NULL, &page_size);
  clutter_actor_get_allocation_box (CLUTTER_ACTOR (item), &item_box);

  if (item_box.x1 < value)
    value = MAX (0, item_box.x1);
  else if (item_box.x2 > value + page_size)
    value = MIN (upper, item_box.x2 - page_size);

  if (to_right)
    priv->scrollable_left = TRUE;
  else
    priv->scrollable_right = TRUE;

  data = g_new0 (ScrollData, 1);
  g_weak_ref_init (&data->list, self);
  data->index = index;
  data->to_right = to_right;

  g_value_init (&target, G_TYPE_DOUBLE);
  g_value_set_double (&target, value);

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (adjustment), "value",
                                       &target, &params,
                                       scroll_done_cb, data, scroll_data_free,
                                       &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

static void
real_highlight (ShellSwitcherList *self,
                int                index,
                gboolean           just_outline)
{
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);
  StButton *old_item = shell_switcher_list_get_item (self, priv->highlighted);
  StButton *item = shell_switcher_list_get_item (self, index);
  StAdjustment *adjustment;
  ClutterActorBox item_box;
  float abs_item_x, pos_x, pos_y, container_width;
  double value;

  if (old_item != NULL)
    {
      st_widget_remove_style_pseudo_class (ST_WIDGET (old_item), "selected");
      st_widget_remove_style_pseudo_class (ST_WIDGET (old_item), "highlighted");
    }

  if (item != NULL)
    {
      if (just_outline)
        st_widget_add_style_pseudo_class (ST_WIDGET (item), "highlighted");
      else
        st_widget_add_style_pseudo_class (ST_WIDGET (item), "selected");
    }

  if (priv->highlighted != index)
    {
      priv->highlighted = index;
      g_object_notify_by_pspec (G_OBJECT (self), list_props[LIST_PROP_HIGHLIGHTED]);
    }

  /* The JS code threw here without an item */
  if (item == NULL)
    return;

  adjustment = st_scroll_view_get_hadjustment (priv->scroll_view);
  value = st_adjustment_get_value (adjustment);

  clutter_actor_get_transformed_position (CLUTTER_ACTOR (item), &abs_item_x, NULL);
  clutter_actor_transform_stage_point (CLUTTER_ACTOR (self), abs_item_x, 0,
                                       &pos_x, &pos_y);
  clutter_actor_get_transformed_size (CLUTTER_ACTOR (self), &container_width, NULL);
  clutter_actor_get_allocation_box (CLUTTER_ACTOR (item), &item_box);

  if (pos_x + clutter_actor_get_width (CLUTTER_ACTOR (item)) > container_width)
    scroll_to (self, index, TRUE);
  else if (item_box.x1 - value < 0)
    scroll_to (self, index, FALSE);
}

/**
 * shell_switcher_list_highlight:
 * @self: a #ShellSwitcherList
 * @index: the index of the item to highlight, -1 for none
 * @just_outline: whether to only outline the item (the thumbnails of the
 *   item are focused) instead of selecting it
 *
 * Highlights an item and scrolls it into view (`highlight ()`). This is the
 * `highlight` virtual function, which JS subclasses override with
 * `vfunc_highlight ()`.
 */
void
shell_switcher_list_highlight (ShellSwitcherList *self,
                               int                index,
                               gboolean           just_outline)
{
  g_return_if_fail (SHELL_IS_SWITCHER_LIST (self));

  SHELL_SWITCHER_LIST_GET_CLASS (self)->highlight (self, index, just_outline);
}

static void
max_child_width (ShellSwitcherList *self,
                 float              for_height,
                 float             *min_p,
                 float             *natural_p)
{
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);
  float max_child_min = 0;
  float max_child_nat = 0;
  guint i;

  for (i = 0; i < priv->items->len; i++)
    {
      ClutterActor *item = g_ptr_array_index (priv->items, i);
      float child_min, child_nat;

      clutter_actor_get_preferred_width (item, for_height, &child_min, &child_nat);
      max_child_min = MAX (child_min, max_child_min);
      max_child_nat = MAX (child_nat, max_child_nat);

      if (priv->square_items)
        {
          clutter_actor_get_preferred_height (item, -1, &child_min, &child_nat);
          max_child_min = MAX (child_min, max_child_min);
          max_child_nat = MAX (child_nat, max_child_nat);
        }
    }

  if (min_p)
    *min_p = max_child_min;
  if (natural_p)
    *natural_p = max_child_nat;
}

static void
shell_switcher_list_get_preferred_width (ClutterActor *actor,
                                         float         for_height,
                                         float        *min_width_p,
                                         float        *natural_width_p)
{
  ShellSwitcherList *self = SHELL_SWITCHER_LIST (actor);
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  float max_child_min = 0;
  float min_list_width = 0;

  max_child_width (self, for_height, &max_child_min, NULL);
  if (priv->list != NULL)
    clutter_actor_get_preferred_width (CLUTTER_ACTOR (priv->list), for_height,
                                       &min_list_width, NULL);

  st_theme_node_adjust_preferred_width (theme_node, &max_child_min, &min_list_width);

  if (min_width_p)
    *min_width_p = max_child_min;
  if (natural_width_p)
    *natural_width_p = min_list_width;
}

static void
shell_switcher_list_get_preferred_height (ClutterActor *actor,
                                          float         for_width,
                                          float        *min_height_p,
                                          float        *natural_height_p)
{
  ShellSwitcherList *self = SHELL_SWITCHER_LIST (actor);
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  float max_child_min = 0;
  float max_child_nat = 0;
  guint i;

  for (i = 0; i < priv->items->len; i++)
    {
      ClutterActor *item = g_ptr_array_index (priv->items, i);
      float child_min, child_nat;

      clutter_actor_get_preferred_height (item, -1, &child_min, &child_nat);
      max_child_min = MAX (child_min, max_child_min);
      max_child_nat = MAX (child_nat, max_child_nat);
    }

  if (priv->square_items)
    {
      float child_min;

      max_child_width (self, -1, &child_min, NULL);
      max_child_min = MAX (child_min, max_child_min);
      max_child_nat = max_child_min;
    }

  st_theme_node_adjust_preferred_height (theme_node, &max_child_min, &max_child_nat);

  if (min_height_p)
    *min_height_p = max_child_min;
  if (natural_height_p)
    *natural_height_p = max_child_nat;
}

static void
shell_switcher_list_allocate (ClutterActor          *actor,
                              const ClutterActorBox *box)
{
  ShellSwitcherList *self = SHELL_SWITCHER_LIST (actor);
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);
  StThemeNode *theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  ClutterActorBox content_box, child_box;
  float width, height, min_list_width;
  float left_padding, right_padding;
  float arrow_width, arrow_height;
  gboolean scrollable;

  clutter_actor_set_allocation (actor, box);

  st_theme_node_get_content_box (theme_node, box, &content_box);
  width = content_box.x2 - content_box.x1;
  height = content_box.y2 - content_box.y1;

  left_padding = st_theme_node_get_padding (theme_node, ST_SIDE_LEFT);
  right_padding = st_theme_node_get_padding (theme_node, ST_SIDE_RIGHT);

  clutter_actor_get_preferred_width (CLUTTER_ACTOR (priv->list), height,
                                     &min_list_width, NULL);
  scrollable = min_list_width > width;

  clutter_actor_allocate (CLUTTER_ACTOR (priv->scroll_view), &content_box);

  arrow_width = floorf (left_padding / 3);
  arrow_height = arrow_width * 2;
  child_box.x1 = left_padding / 2;
  child_box.y1 = clutter_actor_get_height (actor) / 2 - arrow_width;
  child_box.x2 = child_box.x1 + arrow_width;
  child_box.y2 = child_box.y1 + arrow_height;
  clutter_actor_allocate (CLUTTER_ACTOR (priv->left_arrow), &child_box);
  clutter_actor_set_opacity (CLUTTER_ACTOR (priv->left_arrow),
                             priv->scrollable_left && scrollable ? 255 : 0);

  arrow_width = floorf (right_padding / 3);
  arrow_height = arrow_width * 2;
  child_box.x1 = clutter_actor_get_width (actor) - arrow_width - right_padding / 2;
  child_box.y1 = clutter_actor_get_height (actor) / 2 - arrow_width;
  child_box.x2 = child_box.x1 + arrow_width;
  child_box.y2 = child_box.y1 + arrow_height;
  clutter_actor_allocate (CLUTTER_ACTOR (priv->right_arrow), &child_box);
  clutter_actor_set_opacity (CLUTTER_ACTOR (priv->right_arrow),
                             priv->scrollable_right && scrollable ? 255 : 0);
}

static void
shell_switcher_list_constructed (GObject *object)
{
  ShellSwitcherList *self = SHELL_SWITCHER_LIST (object);
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  StBoxLayout *list;
  StWidget *scroll_view;
  StDrawingArea *left_arrow, *right_arrow;

  G_OBJECT_CLASS (shell_switcher_list_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (self), "switcher-list");

  list = g_object_new (ST_TYPE_BOX_LAYOUT,
                       "style-class", "switcher-list-item-container",
                       "orientation", CLUTTER_ORIENTATION_HORIZONTAL,
                       "x-expand", TRUE,
                       "y-expand", TRUE,
                       NULL);
  g_set_weak_pointer (&priv->list, list);

  scroll_view = st_scroll_view_new ();
  st_widget_set_style_class_name (scroll_view, "hfade");
  st_scroll_view_set_mouse_scrolling (ST_SCROLL_VIEW (scroll_view), FALSE);
  st_scroll_view_set_touch_scrolling (ST_SCROLL_VIEW (scroll_view), FALSE);
  st_scroll_view_set_policy (ST_SCROLL_VIEW (scroll_view),
                             ST_POLICY_NEVER, ST_POLICY_NEVER);
  st_scroll_view_set_child (ST_SCROLL_VIEW (scroll_view), ST_SCROLLABLE (list));
  g_set_weak_pointer (&priv->scroll_view, ST_SCROLL_VIEW (scroll_view));

  clutter_actor_add_child (actor, CLUTTER_ACTOR (scroll_view));

  /* Those arrows indicate whether scrolling in one direction is possible */
  left_arrow = g_object_new (ST_TYPE_DRAWING_AREA,
                             "style-class", "switcher-arrow",
                             "pseudo-class", "highlighted",
                             NULL);
  g_signal_connect (left_arrow, "repaint",
                    G_CALLBACK (arrow_repaint_cb), GINT_TO_POINTER (ST_SIDE_LEFT));
  g_set_weak_pointer (&priv->left_arrow, left_arrow);

  right_arrow = g_object_new (ST_TYPE_DRAWING_AREA,
                              "style-class", "switcher-arrow",
                              "pseudo-class", "highlighted",
                              NULL);
  g_signal_connect (right_arrow, "repaint",
                    G_CALLBACK (arrow_repaint_cb), GINT_TO_POINTER (ST_SIDE_RIGHT));
  g_set_weak_pointer (&priv->right_arrow, right_arrow);

  clutter_actor_add_child (actor, CLUTTER_ACTOR (left_arrow));
  clutter_actor_add_child (actor, CLUTTER_ACTOR (right_arrow));

  clutter_box_layout_set_homogeneous (CLUTTER_BOX_LAYOUT (clutter_actor_get_layout_manager (CLUTTER_ACTOR (list))),
                                      priv->square_items);
}

static void
shell_switcher_list_dispose (GObject *object)
{
  ShellSwitcherList *self = SHELL_SWITCHER_LIST (object);
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);

  g_clear_weak_pointer (&priv->list);
  g_clear_weak_pointer (&priv->scroll_view);
  g_clear_weak_pointer (&priv->left_arrow);
  g_clear_weak_pointer (&priv->right_arrow);

  G_OBJECT_CLASS (shell_switcher_list_parent_class)->dispose (object);
}

static void
shell_switcher_list_finalize (GObject *object)
{
  ShellSwitcherList *self = SHELL_SWITCHER_LIST (object);
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);

  g_clear_pointer (&priv->items, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_switcher_list_parent_class)->finalize (object);
}

static void
shell_switcher_list_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  ShellSwitcherList *self = SHELL_SWITCHER_LIST (object);
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);

  switch (prop_id)
    {
    case LIST_PROP_SQUARE_ITEMS:
      g_value_set_boolean (value, priv->square_items);
      break;

    case LIST_PROP_HIGHLIGHTED:
      g_value_set_int (value, priv->highlighted);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_switcher_list_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  ShellSwitcherList *self = SHELL_SWITCHER_LIST (object);
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);

  switch (prop_id)
    {
    case LIST_PROP_SQUARE_ITEMS:
      priv->square_items = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_switcher_list_class_init (ShellSwitcherListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_switcher_list_constructed;
  object_class->dispose = shell_switcher_list_dispose;
  object_class->finalize = shell_switcher_list_finalize;
  object_class->get_property = shell_switcher_list_get_property;
  object_class->set_property = shell_switcher_list_set_property;

  actor_class->get_preferred_width = shell_switcher_list_get_preferred_width;
  actor_class->get_preferred_height = shell_switcher_list_get_preferred_height;
  actor_class->allocate = shell_switcher_list_allocate;

  klass->highlight = real_highlight;
  klass->item_motion = real_item_motion;

  /**
   * ShellSwitcherList:square-items:
   *
   * Whether the items are squares, as wide as they are high (the
   * `squareItems` argument of the JS constructor).
   */
  list_props[LIST_PROP_SQUARE_ITEMS] =
    g_param_spec_boolean ("square-items", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellSwitcherList:highlighted:
   *
   * The index of the highlighted item, -1 if there is none.
   */
  list_props[LIST_PROP_HIGHLIGHTED] =
    g_param_spec_int ("highlighted", NULL, NULL,
                      -1, G_MAXINT, -1,
                      G_PARAM_READABLE |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LIST_N_PROPS, list_props);

  /**
   * ShellSwitcherList::item-activated:
   * @self: the #ShellSwitcherList
   * @index: the index of the item
   *
   * Emitted when an item is clicked.
   */
  list_signals[ITEM_ACTIVATED] =
    g_signal_new ("item-activated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT);

  /**
   * ShellSwitcherList::item-entered:
   * @self: the #ShellSwitcherList
   * @index: the index of the item
   *
   * Emitted when the pointer moves over an item other than the highlighted
   * one.
   */
  list_signals[ITEM_ENTERED] =
    g_signal_new ("item-entered",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT);

  /**
   * ShellSwitcherList::item-removed:
   * @self: the #ShellSwitcherList
   * @index: the index the removed item had
   *
   * Emitted by shell_switcher_list_remove_item (), after the item is gone.
   */
  list_signals[ITEM_REMOVED] =
    g_signal_new ("item-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT);
}

static void
shell_switcher_list_init (ShellSwitcherList *self)
{
  ShellSwitcherListPrivate *priv = shell_switcher_list_get_instance_private (self);

  priv->items = g_ptr_array_new ();
  priv->highlighted = -1;
  priv->scrollable_right = TRUE;
  priv->scrollable_left = FALSE;
}

/* ------------------------------------------------------------------------ */
/* ShellSwitcherPopup                                                        */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_SWITCHER_LIST,
  PROP_SELECTED_INDEX,
  PROP_MOUSE_ACTIVE,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

typedef struct
{
  StWidget *switcher_list;     /* strong */
  int selected_index;

  gboolean mouse_active;

  gboolean have_modal;
  ClutterGrab *grab;           /* strong */
  uint32_t modifier_mask;

  guint motion_timeout_id;
  guint initial_delay_timeout_id;
  guint no_mods_timeout_id;
} ShellSwitcherPopupPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (ShellSwitcherPopup, shell_switcher_popup, ST_TYPE_WIDGET)

static MetaDisplay *
get_display (void)
{
  return shell_global_get_display (shell_global_get ());
}

/**
 * shell_switcher_popup_get_n_items:
 * @self: a #ShellSwitcherPopup
 *
 * Returns: the number of items of the popup (`_items.length`). This is the
 *   `get_n_items` virtual function, which subclasses must override
 */
int
shell_switcher_popup_get_n_items (ShellSwitcherPopup *self)
{
  g_return_val_if_fail (SHELL_IS_SWITCHER_POPUP (self), 0);

  return SHELL_SWITCHER_POPUP_GET_CLASS (self)->get_n_items (self);
}

/**
 * shell_switcher_popup_get_selected_index:
 * @self: a #ShellSwitcherPopup
 *
 * Returns: the index of the selected item (`_selectedIndex`)
 */
int
shell_switcher_popup_get_selected_index (ShellSwitcherPopup *self)
{
  ShellSwitcherPopupPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SWITCHER_POPUP (self), 0);

  priv = shell_switcher_popup_get_instance_private (self);
  return priv->selected_index;
}

/**
 * shell_switcher_popup_set_selected_index:
 * @self: a #ShellSwitcherPopup
 * @index: the index of the selected item
 *
 * Sets #ShellSwitcherPopup:selected-index without highlighting anything in
 * the list. Use shell_switcher_popup_select () to select an item.
 */
void
shell_switcher_popup_set_selected_index (ShellSwitcherPopup *self,
                                         int                 index)
{
  ShellSwitcherPopupPrivate *priv;

  g_return_if_fail (SHELL_IS_SWITCHER_POPUP (self));

  priv = shell_switcher_popup_get_instance_private (self);
  if (priv->selected_index == index)
    return;

  priv->selected_index = index;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SELECTED_INDEX]);
}

/**
 * shell_switcher_popup_get_switcher_list:
 * @self: a #ShellSwitcherPopup
 *
 * Returns: (transfer none) (nullable): the list of items the popup shows
 *   (`_switcherList`)
 */
StWidget *
shell_switcher_popup_get_switcher_list (ShellSwitcherPopup *self)
{
  ShellSwitcherPopupPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SWITCHER_POPUP (self), NULL);

  priv = shell_switcher_popup_get_instance_private (self);
  return priv->switcher_list;
}

/**
 * shell_switcher_popup_set_switcher_list:
 * @self: a #ShellSwitcherPopup
 * @switcher_list: (nullable): the list of items to show. It is usually a
 *   #ShellSwitcherList; other widgets must have the ::item-activated,
 *   ::item-entered and ::item-removed signals of one
 *
 * Sets #ShellSwitcherPopup:switcher-list. This is done once, before the
 * popup is shown. The popup keeps a reference and destroys the list with
 * itself.
 */
void
shell_switcher_popup_set_switcher_list (ShellSwitcherPopup *self,
                                        StWidget           *switcher_list)
{
  ShellSwitcherPopupPrivate *priv;

  g_return_if_fail (SHELL_IS_SWITCHER_POPUP (self));
  g_return_if_fail (switcher_list == NULL || ST_IS_WIDGET (switcher_list));

  priv = shell_switcher_popup_get_instance_private (self);
  if (priv->switcher_list == switcher_list)
    return;

  if (switcher_list != NULL)
    g_object_ref_sink (switcher_list);
  g_clear_object (&priv->switcher_list);
  priv->switcher_list = switcher_list;

  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SWITCHER_LIST]);
}

/**
 * shell_switcher_popup_get_mouse_active:
 * @self: a #ShellSwitcherPopup
 *
 * Returns: whether the pointer selects items (`mouseActive`). It does not
 *   shortly after the popup appeared or a key or the wheel was used
 */
gboolean
shell_switcher_popup_get_mouse_active (ShellSwitcherPopup *self)
{
  ShellSwitcherPopupPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SWITCHER_POPUP (self), FALSE);

  priv = shell_switcher_popup_get_instance_private (self);
  return priv->mouse_active;
}

/**
 * shell_switcher_popup_get_next_index:
 * @self: a #ShellSwitcherPopup
 *
 * Returns: the index of the item after the selected one, wrapping around
 *   (`_next ()`)
 */
int
shell_switcher_popup_get_next_index (ShellSwitcherPopup *self)
{
  ShellSwitcherPopupPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SWITCHER_POPUP (self), 0);

  priv = shell_switcher_popup_get_instance_private (self);
  return shell_switcher_popup_mod (priv->selected_index + 1,
                                   shell_switcher_popup_get_n_items (self));
}

/**
 * shell_switcher_popup_get_previous_index:
 * @self: a #ShellSwitcherPopup
 *
 * Returns: the index of the item before the selected one, wrapping around
 *   (`_previous ()`)
 */
int
shell_switcher_popup_get_previous_index (ShellSwitcherPopup *self)
{
  ShellSwitcherPopupPrivate *priv;

  g_return_val_if_fail (SHELL_IS_SWITCHER_POPUP (self), 0);

  priv = shell_switcher_popup_get_instance_private (self);
  return shell_switcher_popup_mod (priv->selected_index - 1,
                                   shell_switcher_popup_get_n_items (self));
}

/**
 * shell_switcher_popup_select:
 * @self: a #ShellSwitcherPopup
 * @index: the index of the item to select
 *
 * Selects an item and highlights it in the list (`_select ()`). This is the
 * `select` virtual function, which JS subclasses override with
 * `vfunc_select ()`.
 */
void
shell_switcher_popup_select (ShellSwitcherPopup *self,
                             int                 index)
{
  g_return_if_fail (SHELL_IS_SWITCHER_POPUP (self));

  SHELL_SWITCHER_POPUP_GET_CLASS (self)->select (self, index);
}

/**
 * shell_switcher_popup_finish:
 * @self: a #ShellSwitcherPopup
 * @timestamp: the time of the event that ended the popup
 *
 * Ends the popup with the selected item (`_finish ()`). This is the `finish`
 * virtual function, which JS subclasses override with `vfunc_finish ()`; the
 * default fades the popup out and destroys it.
 */
void
shell_switcher_popup_finish (ShellSwitcherPopup *self,
                             uint32_t            timestamp)
{
  g_autoptr (ShellSwitcherPopup) keep_alive = NULL;

  g_return_if_fail (SHELL_IS_SWITCHER_POPUP (self));

  /* The default destroys the popup, which can drop the last reference */
  keep_alive = g_object_ref (self);
  SHELL_SWITCHER_POPUP_GET_CLASS (self)->finish (self, timestamp);
}

/**
 * shell_switcher_popup_is_actor_outside:
 * @self: a #ShellSwitcherPopup
 * @actor: (nullable): the actor that was clicked
 *
 * Returns: whether a click on @actor is a click outside of the popup, which
 *   dismisses it (`_isActorOutside ()`). This is the `is_actor_outside`
 *   virtual function
 */
gboolean
shell_switcher_popup_is_actor_outside (ShellSwitcherPopup *self,
                                       ClutterActor       *actor)
{
  g_return_val_if_fail (SHELL_IS_SWITCHER_POPUP (self), FALSE);

  return SHELL_SWITCHER_POPUP_GET_CLASS (self)->is_actor_outside (self, actor);
}

static void
pop_modal (ShellSwitcherPopup *self)
{
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);
  g_autoptr (GError) error = NULL;

  if (!priv->have_modal)
    return;

  if (!shell_main_pop_modal (shell_main_get_default (), priv->grab, &error))
    g_warning ("%s: %s", G_STRFUNC, error->message);
  g_clear_object (&priv->grab);
  priv->have_modal = FALSE;
}

static void
clear_timeouts (ShellSwitcherPopup *self)
{
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  g_clear_handle_id (&priv->motion_timeout_id, g_source_remove);
  g_clear_handle_id (&priv->initial_delay_timeout_id, g_source_remove);
  g_clear_handle_id (&priv->no_mods_timeout_id, g_source_remove);
}

static void
set_mouse_active (ShellSwitcherPopup *self,
                  gboolean            mouse_active)
{
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  if (priv->mouse_active == mouse_active)
    return;

  priv->mouse_active = mouse_active;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_MOUSE_ACTIVE]);
}

static void
mouse_timed_out (gpointer user_data)
{
  ShellSwitcherPopup *self = user_data;
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  priv->motion_timeout_id = 0;
  set_mouse_active (self, TRUE);
}

static void
disable_hover (ShellSwitcherPopup *self)
{
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  set_mouse_active (self, FALSE);

  g_clear_handle_id (&priv->motion_timeout_id, g_source_remove);
  priv->motion_timeout_id = g_timeout_add_once (DISABLE_HOVER_TIMEOUT,
                                                mouse_timed_out, self);
  g_source_set_name_by_id (priv->motion_timeout_id,
                           "[gnome-shell] shell_switcher_popup mouse_timed_out");
}

static void
show_now (ShellSwitcherPopup *self)
{
  ShellOsdWindowManager *osd_window_manager =
    shell_main_get_osd_window_manager (shell_main_get_default ());

  if (osd_window_manager != NULL)
    shell_osd_window_manager_hide_all (osd_window_manager);
  clutter_actor_set_opacity (CLUTTER_ACTOR (self), 255);
}

static void
show_immediately (ShellSwitcherPopup *self)
{
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  if (priv->initial_delay_timeout_id == 0)
    return;

  g_clear_handle_id (&priv->initial_delay_timeout_id, g_source_remove);

  show_now (self);
}

static void
initial_delay_cb (gpointer user_data)
{
  ShellSwitcherPopup *self = user_data;
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  priv->initial_delay_timeout_id = 0;
  show_now (self);
}

static void
no_mods_timeout_cb (gpointer user_data)
{
  ShellSwitcherPopup *self = user_data;
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  priv->no_mods_timeout_id = 0;
  shell_switcher_popup_finish (self, meta_display_get_current_time_roundtrip (get_display ()));
}

static void
reset_no_mods_timeout (ShellSwitcherPopup *self)
{
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  g_clear_handle_id (&priv->no_mods_timeout_id, g_source_remove);
  priv->no_mods_timeout_id = g_timeout_add_once (NO_MODS_TIMEOUT,
                                                 no_mods_timeout_cb, self);
  g_source_set_name_by_id (priv->no_mods_timeout_id,
                           "[gnome-shell] shell_switcher_popup no_mods_timeout");
}

static void
fade_out_done_cb (gboolean finished,
                  gpointer user_data)
{
  /* onComplete: only when the animation ran to its end */
  if (!finished)
    return;

  clutter_actor_destroy (CLUTTER_ACTOR (user_data));
}

/**
 * shell_switcher_popup_fade_and_destroy:
 * @self: a #ShellSwitcherPopup
 *
 * Drops the modal grab and fades the popup out, then destroys it
 * (`fadeAndDestroy ()`).
 */
void
shell_switcher_popup_fade_and_destroy (ShellSwitcherPopup *self)
{
  ClutterActor *actor;

  g_return_if_fail (SHELL_IS_SWITCHER_POPUP (self));

  actor = CLUTTER_ACTOR (self);

  pop_modal (self);
  if (clutter_actor_get_opacity (actor) > 0)
    {
      ShellEaseParams params = {
        .duration = POPUP_FADE_OUT_TIME,
        .delay = -1,
        .mode = CLUTTER_EASE_OUT_QUAD,
        .repeat_count = 0,
        .auto_reverse = FALSE,
        .animation_required = FALSE,
      };

      shell_actor_ease (actor, &params,
                        fade_out_done_cb, g_object_ref (self), g_object_unref,
                        "opacity", 0u,
                        NULL);
    }
  else
    {
      clutter_actor_destroy (actor);
    }
}

static void
real_select (ShellSwitcherPopup *self,
             int                 index)
{
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  shell_switcher_popup_set_selected_index (self, index);

  if (SHELL_IS_SWITCHER_LIST (priv->switcher_list))
    shell_switcher_list_highlight (SHELL_SWITCHER_LIST (priv->switcher_list), index, FALSE);
}

static void
real_finish (ShellSwitcherPopup *self,
             uint32_t            timestamp)
{
  shell_switcher_popup_fade_and_destroy (self);
}

static int
real_get_n_items (ShellSwitcherPopup *self)
{
  return 0;
}

static void
real_initial_selection (ShellSwitcherPopup *self,
                        gboolean            backward,
                        const char         *binding)
{
  int n_items = shell_switcher_popup_get_n_items (self);

  if (backward)
    shell_switcher_popup_select (self, n_items - 1);
  else if (n_items == 1)
    shell_switcher_popup_select (self, 0);
  else
    shell_switcher_popup_select (self, 1);
}

static gboolean
real_key_press_handler (ShellSwitcherPopup *self,
                        uint32_t            keysym,
                        uint32_t            action)
{
  /* The JS class threw NotImplementedError: subclasses must override this */
  g_warning ("%s does not implement key_press_handler",
             G_OBJECT_TYPE_NAME (self));
  return CLUTTER_EVENT_PROPAGATE;
}

static void
real_scroll_handler (ShellSwitcherPopup    *self,
                     ClutterScrollDirection direction)
{
  if (direction == CLUTTER_SCROLL_UP)
    shell_switcher_popup_select (self, shell_switcher_popup_get_previous_index (self));
  else if (direction == CLUTTER_SCROLL_DOWN)
    shell_switcher_popup_select (self, shell_switcher_popup_get_next_index (self));
}

static void
real_item_activated_handler (ShellSwitcherPopup *self,
                             int                 index)
{
  shell_switcher_popup_select (self, index);
}

static void
real_item_entered_handler (ShellSwitcherPopup *self,
                           int                 index)
{
  shell_switcher_popup_select (self, index);
}

static void
real_item_removed_handler (ShellSwitcherPopup *self,
                           int                 index)
{
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);
  int n_items = shell_switcher_popup_get_n_items (self);
  int new_index;

  if (n_items > 0)
    {
      if (index < priv->selected_index)
        new_index = priv->selected_index - 1;
      else if (index == priv->selected_index)
        new_index = MIN (index, n_items - 1);
      else
        return; /* No need to select something new in this case */

      shell_switcher_popup_select (self, new_index);
    }
  else
    {
      shell_switcher_popup_fade_and_destroy (self);
    }
}

static gboolean
real_is_actor_outside (ShellSwitcherPopup *self,
                       ClutterActor       *actor)
{
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  return !(priv->switcher_list != NULL &&
           actor != NULL &&
           clutter_actor_contains (CLUTTER_ACTOR (priv->switcher_list), actor));
}

static void
list_item_activated_cb (ShellSwitcherList  *list,
                        int                 index,
                        ShellSwitcherPopup *self)
{
  g_autoptr (ShellSwitcherPopup) keep_alive = g_object_ref (self);

  SHELL_SWITCHER_POPUP_GET_CLASS (self)->item_activated_handler (self, index);
  shell_switcher_popup_finish (self, shell_global_get_current_time (shell_global_get ()));
}

static void
list_item_entered_cb (ShellSwitcherList  *list,
                      int                 index,
                      ShellSwitcherPopup *self)
{
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  if (!priv->mouse_active)
    return;

  SHELL_SWITCHER_POPUP_GET_CLASS (self)->item_entered_handler (self, index);
}

static void
list_item_removed_cb (ShellSwitcherList  *list,
                      int                 index,
                      ShellSwitcherPopup *self)
{
  SHELL_SWITCHER_POPUP_GET_CLASS (self)->item_removed_handler (self, index);
}

static gboolean
real_show_switcher (ShellSwitcherPopup *self,
                    gboolean            backward,
                    const char         *binding,
                    uint32_t            mask)
{
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);
  ShellSwitcherPopupClass *klass = SHELL_SWITCHER_POPUP_GET_CLASS (self);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  g_autoptr (ShellSwitcherPopup) keep_alive = NULL;
  ClutterGrab *grab;
  ClutterActorBox box;

  if (shell_switcher_popup_get_n_items (self) == 0)
    return FALSE;

  /* Finishing right away can destroy the popup */
  keep_alive = g_object_ref (self);

  grab = shell_main_push_modal (shell_main_get_default (), actor,
                                SHELL_ACTION_MODE_NONE);
  g_clear_object (&priv->grab);
  if (grab != NULL)
    priv->grab = g_object_ref (grab);
  priv->have_modal = TRUE;
  priv->modifier_mask = primary_modifier (mask);

  if (priv->switcher_list != NULL)
    {
      clutter_actor_add_child (actor, CLUTTER_ACTOR (priv->switcher_list));
      g_signal_connect_object (priv->switcher_list, "item-activated",
                               G_CALLBACK (list_item_activated_cb), self, 0);
      g_signal_connect_object (priv->switcher_list, "item-entered",
                               G_CALLBACK (list_item_entered_cb), self, 0);
      g_signal_connect_object (priv->switcher_list, "item-removed",
                               G_CALLBACK (list_item_removed_cb), self, 0);
    }

  /* Need to force an allocation so we can figure out whether we
   * need to scroll when selecting */
  clutter_actor_set_opacity (actor, 0);
  clutter_actor_show (actor);
  clutter_actor_get_allocation_box (actor, &box);

  klass->initial_selection (self, backward, binding);

  /* There's a race condition; if the user released Alt before
   * we got the grab, then we won't be notified. (See
   * https://bugzilla.gnome.org/show_bug.cgi?id=596695 for
   * details.) So we check now. (Have to do this after updating
   * selection.) */
  if (priv->modifier_mask != 0)
    {
      ClutterModifierType mods;

      shell_global_get_pointer (shell_global_get (), NULL, NULL, &mods);
      if (!(mods & priv->modifier_mask))
        {
          shell_switcher_popup_finish (self, shell_global_get_current_time (shell_global_get ()));
          return TRUE;
        }
    }
  else
    {
      reset_no_mods_timeout (self);
    }

  /* We delay showing the popup so that fast Alt+Tab users aren't
   * disturbed by the popup briefly flashing. */
  g_clear_handle_id (&priv->initial_delay_timeout_id, g_source_remove);
  priv->initial_delay_timeout_id = g_timeout_add_once (POPUP_DELAY_TIMEOUT,
                                                       initial_delay_cb, self);
  g_source_set_name_by_id (priv->initial_delay_timeout_id,
                           "[gnome-shell] shell_switcher_popup show_now");
  return TRUE;
}

/**
 * shell_switcher_popup_show_switcher:
 * @self: a #ShellSwitcherPopup
 * @backward: whether the shortcut that started the popup goes backwards
 * @binding: (nullable): the name of the key binding that started the popup,
 *   such as `switch-applications`
 * @mask: the modifier mask of the key binding. The popup ends when the
 *   primary modifier is released; without a modifier it ends after a
 *   moment without key events
 *
 * Grabs the input, selects the first item and shows the popup after a short
 * delay (`show ()`). This is the `show_switcher` virtual function, which JS
 * subclasses override with `vfunc_show_switcher ()`.
 *
 * Returns: %FALSE if there is nothing to show, in which case the popup is
 *   not shown and should be destroyed
 */
gboolean
shell_switcher_popup_show_switcher (ShellSwitcherPopup *self,
                                    gboolean            backward,
                                    const char         *binding,
                                    uint32_t            mask)
{
  g_return_val_if_fail (SHELL_IS_SWITCHER_POPUP (self), FALSE);

  return SHELL_SWITCHER_POPUP_GET_CLASS (self)->show_switcher (self, backward,
                                                               binding, mask);
}

static gboolean
key_press_cb (ClutterKeyController *controller,
              ShellSwitcherPopup   *self)
{
  uint32_t keysym, keycode;
  ClutterModifierType pressed, latched, locked;
  uint32_t action;

  clutter_key_controller_get_key (controller, &keysym, &keycode, NULL);
  clutter_key_controller_get_state (controller, &pressed, &latched, &locked);
  action = meta_display_get_keybinding_action (get_display (), keycode,
                                               pressed | latched | locked);

  disable_hover (self);

  if (SHELL_SWITCHER_POPUP_GET_CLASS (self)->key_press_handler (self, keysym, action) !=
      CLUTTER_EVENT_PROPAGATE)
    {
      show_immediately (self);
      return CLUTTER_EVENT_STOP;
    }

  /* Note: pressing one of the below keys will destroy the popup only if
   * that key is not used by the active popup's keyboard shortcut */
  if (keysym == CLUTTER_KEY_Escape || keysym == CLUTTER_KEY_Tab)
    shell_switcher_popup_fade_and_destroy (self);

  /* Allow to explicitly select the current item; this is particularly
   * useful for no-modifier popups */
  if (keysym == CLUTTER_KEY_space ||
      keysym == CLUTTER_KEY_Return ||
      keysym == CLUTTER_KEY_KP_Enter ||
      keysym == CLUTTER_KEY_ISO_Enter)
    shell_switcher_popup_finish (self, clutter_get_current_event_time ());

  return CLUTTER_EVENT_STOP;
}

static void
modifier_change_cb (ClutterKeyController *controller,
                    ShellSwitcherPopup   *self)
{
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  if (priv->modifier_mask != 0)
    {
      ClutterModifierType pressed, latched, locked;
      uint32_t state;

      clutter_key_controller_get_state (controller, &pressed, &latched, &locked);
      state = (pressed | latched | locked) & priv->modifier_mask;

      if (state == 0)
        shell_switcher_popup_finish (self, clutter_get_current_event_time ());
    }
  else
    {
      reset_no_mods_timeout (self);
    }
}

static void
scroll_cb (ClutterScrollController *controller,
           ClutterSprite           *sprite,
           ClutterScrollSource      source,
           double                   dx,
           double                   dy,
           ShellSwitcherPopup      *self)
{
  ClutterScrollDirection direction;

  disable_hover (self);

  direction = dy > 0 ? CLUTTER_SCROLL_DOWN : CLUTTER_SCROLL_UP;
  SHELL_SWITCHER_POPUP_GET_CLASS (self)->scroll_handler (self, direction);
}

static gboolean
long_press_may_recognize_cb (ClutterGesture     *gesture,
                             ShellSwitcherPopup *self)
{
  graphene_point_t coords;
  ClutterActor *actor_at_coords;

  clutter_press_gesture_get_coords_abs (CLUTTER_PRESS_GESTURE (gesture), &coords);
  actor_at_coords =
    clutter_stage_get_actor_at_pos (shell_global_get_stage (shell_global_get ()),
                                    CLUTTER_PICK_REACTIVE,
                                    coords.x, coords.y);

  return shell_switcher_popup_is_actor_outside (self, actor_at_coords);
}

static void
long_press_recognize_cb (ClutterGesture     *gesture,
                         ShellSwitcherPopup *self)
{
  /* User clicked outside */
  shell_switcher_popup_fade_and_destroy (self);
}

static void
system_modal_opened_cb (ShellMain          *shell_main,
                        ShellSwitcherPopup *self)
{
  clutter_actor_destroy (CLUTTER_ACTOR (self));
}

static void
shutdown_cb (ShellGlobal        *global,
             ShellSwitcherPopup *self)
{
  clear_timeouts (self);
}

static void
popup_destroy_cb (ClutterActor *actor)
{
  ShellSwitcherPopup *self = SHELL_SWITCHER_POPUP (actor);
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  pop_modal (self);
  clear_timeouts (self);

  /* Make sure the SwitcherList is always destroyed, it may not be
   * a child of the actor at this point. */
  if (priv->switcher_list != NULL)
    clutter_actor_destroy (CLUTTER_ACTOR (priv->switcher_list));
}

static void
shell_switcher_popup_allocate (ClutterActor          *actor,
                               const ClutterActorBox *box)
{
  ShellSwitcherPopup *self = SHELL_SWITCHER_POPUP (actor);
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);
  ShellLayoutManager *layout_manager =
    shell_main_get_layout_manager (shell_main_get_default ());
  StThemeNode *theme_node;
  ClutterActorBox child_box;
  ShellMonitor *primary;
  float left_padding, right_padding, h_padding;
  float child_natural_height, child_natural_width;

  clutter_actor_set_allocation (actor, box);

  if (priv->switcher_list == NULL || layout_manager == NULL)
    return;

  primary = shell_layout_manager_get_primary_monitor (layout_manager);
  if (primary == NULL)
    return;

  theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  left_padding = st_theme_node_get_padding (theme_node, ST_SIDE_LEFT);
  right_padding = st_theme_node_get_padding (theme_node, ST_SIDE_RIGHT);
  h_padding = left_padding + right_padding;

  /* Allocate the switcherList
   * We select a size based on an icon size that does not overflow the screen */
  clutter_actor_get_preferred_height (CLUTTER_ACTOR (priv->switcher_list),
                                      primary->width - h_padding,
                                      NULL, &child_natural_height);
  clutter_actor_get_preferred_width (CLUTTER_ACTOR (priv->switcher_list),
                                     child_natural_height,
                                     NULL, &child_natural_width);

  child_box.x1 = MAX (primary->x + left_padding,
                      primary->x + floorf ((primary->width - child_natural_width) / 2));
  child_box.x2 = MIN (primary->x + primary->width - right_padding,
                      child_box.x1 + child_natural_width);
  child_box.y1 = primary->y + floorf ((primary->height - child_natural_height) / 2);
  child_box.y2 = child_box.y1 + child_natural_height;
  clutter_actor_allocate (CLUTTER_ACTOR (priv->switcher_list), &child_box);

  shell_monitor_free (primary);
}

static void
shell_switcher_popup_constructed (GObject *object)
{
  ShellSwitcherPopup *self = SHELL_SWITCHER_POPUP (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellGlobal *global = shell_global_get ();
  ShellMain *shell_main = shell_main_get_default ();
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main);
  ClutterConstraint *constraint;
  ClutterAction *long_press_gesture;
  ClutterAction *key_controller;
  ClutterAction *scroll_controller;

  G_OBJECT_CLASS (shell_switcher_popup_parent_class)->constructed (object);

  st_widget_set_style_class_name (ST_WIDGET (self), "switcher-popup");
  clutter_actor_set_reactive (actor, TRUE);
  clutter_actor_hide (actor);

  if (layout_manager != NULL)
    clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager)),
                             actor);
  else
    g_critical ("ShellSwitcherPopup needs the layout manager: ShellMain:layout-manager is not set");

  g_signal_connect_object (shell_main, "system-modal-opened",
                           G_CALLBACK (system_modal_opened_cb), self, 0);
  g_signal_connect_object (global, "shutdown",
                           G_CALLBACK (shutdown_cb), self, 0);

  /* Connected before anything the subclasses connect, and before the modal
   * stack connects its own handler, so that popping the modal here does not
   * find it popped already */
  g_signal_connect (self, "destroy", G_CALLBACK (popup_destroy_cb), NULL);

  constraint = clutter_bind_constraint_new (CLUTTER_ACTOR (shell_global_get_stage (global)),
                                            CLUTTER_BIND_ALL, 0);
  clutter_actor_add_constraint (actor, constraint);

  long_press_gesture = clutter_long_press_gesture_new ();
  clutter_press_gesture_set_long_press_duration_ms (CLUTTER_PRESS_GESTURE (long_press_gesture), 0);
  g_signal_connect (long_press_gesture, "may-recognize",
                    G_CALLBACK (long_press_may_recognize_cb), self);
  g_signal_connect (long_press_gesture, "recognize",
                    G_CALLBACK (long_press_recognize_cb), self);
  clutter_actor_add_action (actor, long_press_gesture);

  key_controller = clutter_key_controller_new (NULL);
  g_signal_connect (key_controller, "key-press",
                    G_CALLBACK (key_press_cb), self);
  g_signal_connect (key_controller, "modifier-change",
                    G_CALLBACK (modifier_change_cb), self);
  clutter_actor_add_action (actor, key_controller);

  scroll_controller = clutter_scroll_controller_new (CLUTTER_SCROLL_CONTROLLER_FLAG_NONE);
  g_signal_connect (scroll_controller, "scroll",
                    G_CALLBACK (scroll_cb), self);
  clutter_actor_add_action (actor, scroll_controller);

  /* Initially disable hover so we ignore the enter-event if
   * the switcher appears underneath the current pointer location */
  disable_hover (self);
}

static void
shell_switcher_popup_dispose (GObject *object)
{
  ShellSwitcherPopup *self = SHELL_SWITCHER_POPUP (object);
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  clear_timeouts (self);

  G_OBJECT_CLASS (shell_switcher_popup_parent_class)->dispose (object);

  /* Clear after the chain-up: ClutterActor's dispose emits ::destroy, whose
   * handler (popup_destroy_cb ()) pops the modal with the grab and destroys
   * the switcher list */
  g_clear_object (&priv->grab);
  g_clear_object (&priv->switcher_list);
}

static void
shell_switcher_popup_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  ShellSwitcherPopup *self = SHELL_SWITCHER_POPUP (object);
  ShellSwitcherPopupPrivate *priv = shell_switcher_popup_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SWITCHER_LIST:
      g_value_set_object (value, priv->switcher_list);
      break;

    case PROP_SELECTED_INDEX:
      g_value_set_int (value, priv->selected_index);
      break;

    case PROP_MOUSE_ACTIVE:
      g_value_set_boolean (value, priv->mouse_active);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_switcher_popup_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  ShellSwitcherPopup *self = SHELL_SWITCHER_POPUP (object);

  switch (prop_id)
    {
    case PROP_SWITCHER_LIST:
      shell_switcher_popup_set_switcher_list (self, g_value_get_object (value));
      break;

    case PROP_SELECTED_INDEX:
      shell_switcher_popup_set_selected_index (self, g_value_get_int (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

/**
 * ShellSwitcherPopupClass::show_switcher:
 * @self: a #ShellSwitcherPopup
 * @backward: whether the shortcut goes backwards
 * @binding: (nullable): the name of the key binding that started the popup
 * @mask: the modifier mask of the key binding
 *
 * Returns: %FALSE if there is nothing to show
 */

/**
 * ShellSwitcherPopupClass::get_n_items:
 * @self: a #ShellSwitcherPopup
 *
 * Returns: the number of items of the popup
 */

/**
 * ShellSwitcherPopupClass::initial_selection:
 * @self: a #ShellSwitcherPopup
 * @backward: whether the shortcut goes backwards
 * @binding: (nullable): the name of the key binding that started the popup
 */

/**
 * ShellSwitcherPopupClass::key_press_handler:
 * @self: a #ShellSwitcherPopup
 * @keysym: the key symbol of the key that was pressed
 * @action: the #MetaKeyBindingAction of the key with the current modifiers.
 *   It is a number as the actions of the bindings added by extensions are
 *   not in the enumeration
 *
 * Returns: %CLUTTER_EVENT_STOP if the key was used, %CLUTTER_EVENT_PROPAGATE
 *   otherwise
 */

/**
 * ShellSwitcherPopupClass::is_actor_outside:
 * @self: a #ShellSwitcherPopup
 * @actor: (nullable): the actor under the pointer
 *
 * Returns: whether a click on @actor is a click outside of the popup
 */

static void
shell_switcher_popup_class_init (ShellSwitcherPopupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_switcher_popup_constructed;
  object_class->dispose = shell_switcher_popup_dispose;
  object_class->get_property = shell_switcher_popup_get_property;
  object_class->set_property = shell_switcher_popup_set_property;

  actor_class->allocate = shell_switcher_popup_allocate;

  klass->show_switcher = real_show_switcher;
  klass->get_n_items = real_get_n_items;
  klass->initial_selection = real_initial_selection;
  klass->key_press_handler = real_key_press_handler;
  klass->scroll_handler = real_scroll_handler;
  klass->item_activated_handler = real_item_activated_handler;
  klass->item_entered_handler = real_item_entered_handler;
  klass->item_removed_handler = real_item_removed_handler;
  klass->is_actor_outside = real_is_actor_outside;
  klass->select = real_select;
  klass->finish = real_finish;

  /**
   * ShellSwitcherPopup:switcher-list:
   *
   * The list of items the popup shows (`_switcherList`). Subclasses create it
   * and set it right after construction.
   */
  props[PROP_SWITCHER_LIST] =
    g_param_spec_object ("switcher-list", NULL, NULL,
                         ST_TYPE_WIDGET,
                         G_PARAM_READWRITE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellSwitcherPopup:selected-index:
   *
   * The index of the selected item (`_selectedIndex`).
   */
  props[PROP_SELECTED_INDEX] =
    g_param_spec_int ("selected-index", NULL, NULL,
                      G_MININT, G_MAXINT, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  /**
   * ShellSwitcherPopup:mouse-active:
   *
   * Whether the pointer selects items (`mouseActive`).
   */
  props[PROP_MOUSE_ACTIVE] =
    g_param_spec_boolean ("mouse-active", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
shell_switcher_popup_init (ShellSwitcherPopup *self)
{
}
