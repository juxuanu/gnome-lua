/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * ibus-candidate-popup.c: The popup which shows the preedit text, the
 * auxiliary text and the candidates of the current IBus engine next to the
 * text cursor. Port of js/ui/ibusCandidatePopup.js.
 *
 * `CandidateArea` is the private ShellIBusCandidateArea. `Main.keyboard` is
 * read from #ShellMain:keyboard.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The handlers on the panel service are removed when the panel service
 *    is replaced (the JS connected the new one and left the old one
 *    connected).
 *  - The `set-cursor-location-relative` signal is connected without a
 *    try/catch: ibus 1.5.33 is a build-time requirement.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>

#include <clutter/clutter.h>
#include <clutter/clutter-pango.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-window-actor.h>
#include <meta/window.h>

#include "shell-global.h"
#include "ui/ibus-candidate-popup.h"
#include "ui/keyboard.h"
#include "ui/main.h"

#define MAX_CANDIDATES_PER_PAGE 16

/* actor.visible = visible */
static void
set_actor_visible (ClutterActor *actor,
                   gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

static const char * const default_index_labels[MAX_CANDIDATES_PER_PAGE] = {
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
  "a", "b", "c", "d", "e", "f",
};

/* ======================================================================== */
/* ShellIBusCandidateArea                                                   */
/* ======================================================================== */

#define SHELL_TYPE_IBUS_CANDIDATE_AREA (shell_ibus_candidate_area_get_type ())
G_DECLARE_FINAL_TYPE (ShellIBusCandidateArea, shell_ibus_candidate_area,
                      SHELL, IBUS_CANDIDATE_AREA, StBoxLayout)

enum
{
  AREA_CANDIDATE_CLICKED,
  AREA_CURSOR_DOWN,
  AREA_CURSOR_UP,
  AREA_NEXT_PAGE,
  AREA_PREVIOUS_PAGE,
  AREA_LAST_SIGNAL
};

static guint area_signals[AREA_LAST_SIGNAL] = { 0, };

typedef struct
{
  ShellIBusCandidateArea *area;   /* the owner */
  guint index;
  StBoxLayout *box;               /* weak */
  StLabel *index_label;           /* weak */
  StLabel *candidate_label;       /* weak */
} CandidateBox;

struct _ShellIBusCandidateArea
{
  StBoxLayout parent;

  CandidateBox candidate_boxes[MAX_CANDIDATES_PER_PAGE];
  StBoxLayout *button_box;        /* weak */
  StButton *previous_button;      /* weak */
  StButton *next_button;          /* weak */

  int orientation;
  guint cursor_position;
};

G_DEFINE_FINAL_TYPE (ShellIBusCandidateArea, shell_ibus_candidate_area, ST_TYPE_BOX_LAYOUT)

static void
candidate_clicked_cb (ClutterPressGesture *gesture,
                      CandidateBox        *candidate)
{
  g_signal_emit (candidate->area, area_signals[AREA_CANDIDATE_CLICKED], 0,
                 candidate->index,
                 clutter_press_gesture_get_button (gesture),
                 clutter_press_gesture_get_state (gesture));
}

static void
previous_button_clicked_cb (StButton               *button,
                            int                     clicked_button,
                            ShellIBusCandidateArea *self)
{
  g_signal_emit (self, area_signals[AREA_PREVIOUS_PAGE], 0);
}

static void
next_button_clicked_cb (StButton               *button,
                        int                     clicked_button,
                        ShellIBusCandidateArea *self)
{
  g_signal_emit (self, area_signals[AREA_NEXT_PAGE], 0);
}

static void
area_scroll_cb (ClutterScrollController *controller,
                ClutterSprite           *sprite,
                ClutterScrollSource      source,
                double                   dx,
                double                   dy,
                ShellIBusCandidateArea  *self)
{
  if (dy < 0)
    g_signal_emit (self, area_signals[AREA_CURSOR_UP], 0);
  else if (dy > 0)
    g_signal_emit (self, area_signals[AREA_CURSOR_DOWN], 0);
}

static void
shell_ibus_candidate_area_constructed (GObject *object)
{
  ShellIBusCandidateArea *self = SHELL_IBUS_CANDIDATE_AREA (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterAction *scroll_controller;
  guint i;

  G_OBJECT_CLASS (shell_ibus_candidate_area_parent_class)->constructed (object);

  for (i = 0; i < MAX_CANDIDATES_PER_PAGE; ++i)
    {
      CandidateBox *candidate = &self->candidate_boxes[i];
      ClutterAction *click_gesture;
      ClutterActor *box;

      candidate->area = self;
      candidate->index = i;

      box = g_object_new (ST_TYPE_BOX_LAYOUT,
                          "style-class", "candidate-box",
                          "reactive", TRUE,
                          "track-hover", TRUE,
                          NULL);
      g_set_weak_pointer (&candidate->box, ST_BOX_LAYOUT (box));

      candidate->index_label = NULL;
      g_set_weak_pointer (&candidate->index_label,
                          g_object_new (ST_TYPE_LABEL,
                                        "style-class", "candidate-index",
                                        NULL));
      g_set_weak_pointer (&candidate->candidate_label,
                          g_object_new (ST_TYPE_LABEL,
                                        "style-class", "candidate-label",
                                        NULL));
      clutter_actor_add_child (box, CLUTTER_ACTOR (candidate->index_label));
      clutter_actor_add_child (box, CLUTTER_ACTOR (candidate->candidate_label));
      clutter_actor_add_child (actor, box);

      click_gesture = clutter_click_gesture_new ();
      g_signal_connect (click_gesture, "recognize",
                        G_CALLBACK (candidate_clicked_cb), candidate);
      clutter_actor_add_action (box, click_gesture);
    }

  self->button_box = NULL;
  g_set_weak_pointer (&self->button_box,
                      g_object_new (ST_TYPE_BOX_LAYOUT,
                                    "style-class", "candidate-page-button-box",
                                    NULL));

  g_set_weak_pointer (&self->previous_button,
                      g_object_new (ST_TYPE_BUTTON,
                                    "style-class", "candidate-page-button candidate-page-button-previous button",
                                    "x-expand", TRUE,
                                    NULL));
  clutter_actor_add_child (CLUTTER_ACTOR (self->button_box), CLUTTER_ACTOR (self->previous_button));

  g_set_weak_pointer (&self->next_button,
                      g_object_new (ST_TYPE_BUTTON,
                                    "style-class", "candidate-page-button candidate-page-button-next button",
                                    "x-expand", TRUE,
                                    NULL));
  clutter_actor_add_child (CLUTTER_ACTOR (self->button_box), CLUTTER_ACTOR (self->next_button));

  clutter_actor_add_child (actor, CLUTTER_ACTOR (self->button_box));

  g_signal_connect_object (self->previous_button, "clicked",
                           G_CALLBACK (previous_button_clicked_cb), self, 0);
  g_signal_connect_object (self->next_button, "clicked",
                           G_CALLBACK (next_button_clicked_cb), self, 0);

  scroll_controller = clutter_scroll_controller_new (CLUTTER_SCROLL_CONTROLLER_FLAG_DISCRETE |
                                                     CLUTTER_SCROLL_CONTROLLER_FLAG_SCROLL_VERTICAL);
  g_signal_connect (scroll_controller, "scroll", G_CALLBACK (area_scroll_cb), self);
  clutter_actor_add_action (actor, scroll_controller);
}

static void
shell_ibus_candidate_area_dispose (GObject *object)
{
  ShellIBusCandidateArea *self = SHELL_IBUS_CANDIDATE_AREA (object);
  guint i;

  for (i = 0; i < MAX_CANDIDATES_PER_PAGE; ++i)
    {
      g_clear_weak_pointer (&self->candidate_boxes[i].box);
      g_clear_weak_pointer (&self->candidate_boxes[i].index_label);
      g_clear_weak_pointer (&self->candidate_boxes[i].candidate_label);
    }
  g_clear_weak_pointer (&self->button_box);
  g_clear_weak_pointer (&self->previous_button);
  g_clear_weak_pointer (&self->next_button);

  G_OBJECT_CLASS (shell_ibus_candidate_area_parent_class)->dispose (object);
}

static void
shell_ibus_candidate_area_class_init (ShellIBusCandidateAreaClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_ibus_candidate_area_constructed;
  object_class->dispose = shell_ibus_candidate_area_dispose;

  area_signals[AREA_CANDIDATE_CLICKED] =
    g_signal_new ("candidate-clicked",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_UINT, G_TYPE_UINT, CLUTTER_TYPE_MODIFIER_TYPE);
  area_signals[AREA_CURSOR_DOWN] =
    g_signal_new ("cursor-down", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  area_signals[AREA_CURSOR_UP] =
    g_signal_new ("cursor-up", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  area_signals[AREA_NEXT_PAGE] =
    g_signal_new ("next-page", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  area_signals[AREA_PREVIOUS_PAGE] =
    g_signal_new ("previous-page", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
shell_ibus_candidate_area_init (ShellIBusCandidateArea *self)
{
  self->orientation = -1;
  self->cursor_position = 0;
}

static ShellIBusCandidateArea *
candidate_area_new (void)
{
  return g_object_new (SHELL_TYPE_IBUS_CANDIDATE_AREA,
                       "orientation", CLUTTER_ORIENTATION_VERTICAL,
                       "reactive", TRUE,
                       "visible", FALSE,
                       NULL);
}

/* setOrientation () */
static void
candidate_area_set_orientation (ShellIBusCandidateArea *self,
                                int                     orientation)
{

  if (self->orientation == orientation)
    return;

  self->orientation = orientation;

  if (self->orientation == IBUS_ORIENTATION_HORIZONTAL)
    {
      st_box_layout_set_orientation (ST_BOX_LAYOUT (self), CLUTTER_ORIENTATION_HORIZONTAL);
      st_widget_remove_style_class_name (ST_WIDGET (self), "vertical");
      st_widget_add_style_class_name (ST_WIDGET (self), "horizontal");
      st_button_set_icon_name (self->previous_button, "go-previous-symbolic");
      st_button_set_icon_name (self->next_button, "go-next-symbolic");
    }
  else
    {
      /* VERTICAL || SYSTEM */
      st_box_layout_set_orientation (ST_BOX_LAYOUT (self), CLUTTER_ORIENTATION_VERTICAL);
      st_widget_add_style_class_name (ST_WIDGET (self), "vertical");
      st_widget_remove_style_class_name (ST_WIDGET (self), "horizontal");
      st_button_set_icon_name (self->previous_button, "go-up-symbolic");
      st_button_set_icon_name (self->next_button, "go-down-symbolic");
    }
}

/* setCandidates () */
static void
candidate_area_set_candidates (ShellIBusCandidateArea *self,
                               GPtrArray              *indexes,
                               GPtrArray              *candidates,
                               guint                   cursor_position,
                               gboolean                cursor_visible)
{
  guint i;

  for (i = 0; i < MAX_CANDIDATES_PER_PAGE; ++i)
    {
      CandidateBox *candidate = &self->candidate_boxes[i];
      gboolean visible = i < candidates->len;
      const char *index_text;

      set_actor_visible (CLUTTER_ACTOR (candidate->box), visible);

      if (!visible)
        continue;

      index_text = i < indexes->len ? g_ptr_array_index (indexes, i) : NULL;
      st_label_set_text (candidate->index_label,
                         index_text != NULL && index_text[0] != '\0'
                         ? index_text : default_index_labels[i]);
      st_label_set_text (candidate->candidate_label, g_ptr_array_index (candidates, i));
    }

  st_widget_remove_style_pseudo_class (ST_WIDGET (self->candidate_boxes[self->cursor_position].box),
                                       "selected");
  self->cursor_position = MIN (cursor_position, MAX_CANDIDATES_PER_PAGE - 1);
  if (cursor_visible)
    st_widget_add_style_pseudo_class (ST_WIDGET (self->candidate_boxes[self->cursor_position].box),
                                      "selected");
}

/* updateButtons () */
static void
candidate_area_update_buttons (ShellIBusCandidateArea *self,
                               gboolean                wraps_around,
                               guint                   page,
                               guint                   n_pages)
{
  if (n_pages < 2)
    {
      clutter_actor_hide (CLUTTER_ACTOR (self->button_box));
      return;
    }

  clutter_actor_show (CLUTTER_ACTOR (self->button_box));
  clutter_actor_set_reactive (CLUTTER_ACTOR (self->previous_button), wraps_around || page > 0);
  clutter_actor_set_reactive (CLUTTER_ACTOR (self->next_button), wraps_around || page + 1 < n_pages);
}

/* ======================================================================== */
/* ShellIBusCandidatePopup                                                  */
/* ======================================================================== */

struct _ShellIBusCandidatePopup
{
  ShellBoxPointer parent;

  ClutterActor *dummy_cursor;           /* weak, owned by the input panel group */
  StLabel *preedit_text;                /* weak */
  StLabel *aux_text;                    /* weak */
  ShellIBusCandidateArea *candidate_area; /* weak */

  IBusPanelService *panel_service;
};

G_DEFINE_FINAL_TYPE (ShellIBusCandidatePopup, shell_ibus_candidate_popup,
                     SHELL_TYPE_BOX_POINTER)

static ShellOskManager *
popup_get_keyboard (void)
{
  return shell_main_get_keyboard (shell_main_get_default ());
}

/* _updateVisibility () */
static void
popup_update_visibility (ShellIBusCandidatePopup *self)
{
  ShellOskManager *keyboard = popup_get_keyboard ();
  gboolean is_visible = (keyboard == NULL || !shell_osk_manager_get_visible (keyboard)) &&
                        (clutter_actor_is_visible (CLUTTER_ACTOR (self->preedit_text)) ||
                         clutter_actor_is_visible (CLUTTER_ACTOR (self->aux_text)) ||
                         clutter_actor_is_visible (CLUTTER_ACTOR (self->candidate_area)));

  if (is_visible)
    {
      shell_box_pointer_set_position (SHELL_BOX_POINTER (self), self->dummy_cursor, 0);
      shell_box_pointer_open (SHELL_BOX_POINTER (self), SHELL_POPUP_ANIMATION_NONE,
                              NULL, NULL, NULL);
    }
  else
    {
      shell_box_pointer_close (SHELL_BOX_POINTER (self), SHELL_POPUP_ANIMATION_NONE,
                               NULL, NULL, NULL);
    }
}

/* _setDummyCursorGeometry () */
static void
popup_set_dummy_cursor_geometry (ShellIBusCandidatePopup *self,
                                 double                   x,
                                 double                   y,
                                 double                   w,
                                 double                   h)
{
  clutter_actor_set_position (self->dummy_cursor, round (x), round (y));
  clutter_actor_set_size (self->dummy_cursor, round (w), round (h));

  if (clutter_actor_is_visible (CLUTTER_ACTOR (self)))
    shell_box_pointer_set_position (SHELL_BOX_POINTER (self), self->dummy_cursor, 0);
}

/* _setTextAttributes () */
static void
popup_set_text_attributes (StLabel      *label,
                           IBusAttrList *attrs)
{
  ClutterText *clutter_text = CLUTTER_TEXT (st_label_get_clutter_text (label));
  IBusAttribute *attr;
  guint i;

  for (i = 0; (attr = ibus_attr_list_get (attrs, i)) != NULL; ++i)
    {
      if (ibus_attribute_get_attr_type (attr) == IBUS_ATTR_TYPE_BACKGROUND)
        clutter_text_set_selection (clutter_text,
                                    ibus_attribute_get_start_index (attr),
                                    ibus_attribute_get_end_index (attr));
    }
}

static void
ps_set_cursor_location_cb (IBusPanelService        *panel_service,
                           int                      x,
                           int                      y,
                           int                      w,
                           int                      h,
                           ShellIBusCandidatePopup *self)
{
  ShellGlobal *global = shell_global_get ();
  MetaWindow *focus_window = meta_display_get_focus_window (shell_global_get_display (global));
  MtkRectangle rect = { x, y, w, h };

  if (clutter_stage_get_key_focus (shell_global_get_stage (global)) == NULL &&
      focus_window != NULL)
    {
      MtkRectangle stage_rect;

      meta_window_protocol_to_stage_rect (focus_window, &rect, &stage_rect);
      rect = stage_rect;
    }

  popup_set_dummy_cursor_geometry (self, rect.x, rect.y, rect.width, rect.height);
}

static void
ps_set_cursor_location_relative_cb (IBusPanelService        *panel_service,
                                    int                      x,
                                    int                      y,
                                    int                      w,
                                    int                      h,
                                    ShellIBusCandidatePopup *self)
{
  MetaWindow *focus_window = meta_display_get_focus_window (shell_global_get_display (shell_global_get ()));
  MtkRectangle rect = { x, y, w, h };
  MtkRectangle stage_rect;
  MetaWindowActor *window_actor;
  float scale;

  if (focus_window == NULL)
    return;

  meta_window_protocol_to_stage_rect (focus_window, &rect, &stage_rect);
  window_actor = META_WINDOW_ACTOR (meta_window_get_compositor_private (focus_window));
  if (window_actor == NULL)
    return;

  /* IBus GtkIMModule cannot get the shell scale. */
  scale = clutter_actor_get_resource_scale (CLUTTER_ACTOR (window_actor));
  popup_set_dummy_cursor_geometry (self,
                                   clutter_actor_get_x (CLUTTER_ACTOR (window_actor)) + stage_rect.x / scale,
                                   clutter_actor_get_y (CLUTTER_ACTOR (window_actor)) + stage_rect.y / scale,
                                   stage_rect.width / scale,
                                   stage_rect.height / scale);
}

static void
ps_update_preedit_text_cb (IBusPanelService        *panel_service,
                           IBusText                *text,
                           guint                    cursor_position,
                           gboolean                 visible,
                           ShellIBusCandidatePopup *self)
{
  IBusAttrList *attrs;

  set_actor_visible (CLUTTER_ACTOR (self->preedit_text), visible);
  popup_update_visibility (self);

  st_label_set_text (self->preedit_text, ibus_text_get_text (text));

  attrs = ibus_text_get_attributes (text);
  if (attrs != NULL)
    popup_set_text_attributes (self->preedit_text, attrs);
}

static void
ps_show_preedit_text_cb (IBusPanelService        *panel_service,
                         ShellIBusCandidatePopup *self)
{
  clutter_actor_show (CLUTTER_ACTOR (self->preedit_text));
  popup_update_visibility (self);
}

static void
ps_hide_preedit_text_cb (IBusPanelService        *panel_service,
                         ShellIBusCandidatePopup *self)
{
  clutter_actor_hide (CLUTTER_ACTOR (self->preedit_text));
  popup_update_visibility (self);
}

static void
ps_update_auxiliary_text_cb (IBusPanelService        *panel_service,
                             IBusText                *text,
                             gboolean                 visible,
                             ShellIBusCandidatePopup *self)
{
  set_actor_visible (CLUTTER_ACTOR (self->aux_text), visible);
  popup_update_visibility (self);

  st_label_set_text (self->aux_text, ibus_text_get_text (text));
}

static void
ps_show_auxiliary_text_cb (IBusPanelService        *panel_service,
                           ShellIBusCandidatePopup *self)
{
  clutter_actor_show (CLUTTER_ACTOR (self->aux_text));
  popup_update_visibility (self);
}

static void
ps_hide_auxiliary_text_cb (IBusPanelService        *panel_service,
                           ShellIBusCandidatePopup *self)
{
  clutter_actor_hide (CLUTTER_ACTOR (self->aux_text));
  popup_update_visibility (self);
}

/* What the suggestion of the keyboard does when it is clicked */
typedef struct
{
  GWeakRef popup;
  guint index;
} SuggestionData;

static void
suggestion_data_free (SuggestionData *data)
{
  g_weak_ref_clear (&data->popup);
  g_free (data);
}

static void
suggestion_clicked_cb (gpointer user_data)
{
  SuggestionData *data = user_data;
  g_autoptr (ShellIBusCandidatePopup) self = g_weak_ref_get (&data->popup);

  if (self != NULL && self->panel_service != NULL)
    ibus_panel_service_candidate_clicked (self->panel_service, data->index, 1, 0);
}

static void
ps_update_lookup_table_cb (IBusPanelService        *panel_service,
                           IBusLookupTable         *lookup_table,
                           gboolean                 visible,
                           ShellIBusCandidatePopup *self)
{
  ShellOskManager *keyboard = popup_get_keyboard ();
  g_autoptr (GPtrArray) indexes = g_ptr_array_new_with_free_func (NULL);
  g_autoptr (GPtrArray) candidates = g_ptr_array_new_with_free_func (NULL);
  guint n_candidates, cursor_pos, page_size, n_pages, page, start_index, end_index, i;
  IBusText *index_label;

  set_actor_visible (CLUTTER_ACTOR (self->candidate_area), visible);
  popup_update_visibility (self);

  n_candidates = ibus_lookup_table_get_number_of_candidates (lookup_table);
  cursor_pos = ibus_lookup_table_get_cursor_pos (lookup_table);
  page_size = MAX (ibus_lookup_table_get_page_size (lookup_table), 1);
  n_pages = (n_candidates + page_size - 1) / page_size;
  page = cursor_pos == 0 ? 0 : cursor_pos / page_size;
  start_index = page * page_size;
  end_index = MIN ((page + 1) * page_size, n_candidates);

  for (i = 0; (index_label = ibus_lookup_table_get_label (lookup_table, i)) != NULL; ++i)
    g_ptr_array_add (indexes, (gpointer) ibus_text_get_text (index_label));

  if (keyboard != NULL)
    {
      shell_osk_manager_reset_suggestions (keyboard);
      shell_osk_manager_set_suggestions_visible (keyboard, visible);
    }

  for (i = start_index; i < end_index; ++i)
    {
      const char *text = ibus_text_get_text (ibus_lookup_table_get_candidate (lookup_table, i));

      if (candidates->len < MAX_CANDIDATES_PER_PAGE)
        g_ptr_array_add (candidates, (gpointer) text);

      if (keyboard != NULL)
        {
          SuggestionData *data = g_new0 (SuggestionData, 1);

          g_weak_ref_init (&data->popup, self);
          data->index = i;
          shell_osk_manager_add_suggestion (keyboard, text, suggestion_clicked_cb,
                                            data, (GDestroyNotify) suggestion_data_free);
        }
    }

  candidate_area_set_candidates (self->candidate_area, indexes, candidates,
                                 cursor_pos % page_size,
                                 ibus_lookup_table_is_cursor_visible (lookup_table));
  candidate_area_set_orientation (self->candidate_area,
                                  ibus_lookup_table_get_orientation (lookup_table));
  candidate_area_update_buttons (self->candidate_area,
                                 ibus_lookup_table_is_round (lookup_table),
                                 page, n_pages);
}

static void
ps_show_lookup_table_cb (IBusPanelService        *panel_service,
                         ShellIBusCandidatePopup *self)
{
  ShellOskManager *keyboard = popup_get_keyboard ();

  if (keyboard != NULL)
    shell_osk_manager_set_suggestions_visible (keyboard, TRUE);
  clutter_actor_show (CLUTTER_ACTOR (self->candidate_area));
  popup_update_visibility (self);
}

static void
ps_hide_lookup_table_cb (IBusPanelService        *panel_service,
                         ShellIBusCandidatePopup *self)
{
  ShellOskManager *keyboard = popup_get_keyboard ();

  if (keyboard != NULL)
    shell_osk_manager_set_suggestions_visible (keyboard, FALSE);
  clutter_actor_hide (CLUTTER_ACTOR (self->candidate_area));
  popup_update_visibility (self);
}

static void
ps_focus_out_cb (IBusPanelService        *panel_service,
                 const char              *input_context_path,
                 ShellIBusCandidatePopup *self)
{
  ShellOskManager *keyboard = popup_get_keyboard ();

  shell_box_pointer_close (SHELL_BOX_POINTER (self), SHELL_POPUP_ANIMATION_NONE,
                           NULL, NULL, NULL);
  if (keyboard != NULL)
    shell_osk_manager_reset_suggestions (keyboard);
}

static void
area_previous_page_cb (ShellIBusCandidateArea  *area,
                       ShellIBusCandidatePopup *self)
{
  if (self->panel_service != NULL)
    ibus_panel_service_page_up (self->panel_service);
}

static void
area_next_page_cb (ShellIBusCandidateArea  *area,
                   ShellIBusCandidatePopup *self)
{
  if (self->panel_service != NULL)
    ibus_panel_service_page_down (self->panel_service);
}

static void
area_cursor_up_cb (ShellIBusCandidateArea  *area,
                   ShellIBusCandidatePopup *self)
{
  if (self->panel_service != NULL)
    ibus_panel_service_cursor_up (self->panel_service);
}

static void
area_cursor_down_cb (ShellIBusCandidateArea  *area,
                     ShellIBusCandidatePopup *self)
{
  if (self->panel_service != NULL)
    ibus_panel_service_cursor_down (self->panel_service);
}

static void
area_candidate_clicked_cb (ShellIBusCandidateArea  *area,
                           guint                    index,
                           guint                    button,
                           ClutterModifierType      state,
                           ShellIBusCandidatePopup *self)
{
  if (self->panel_service != NULL)
    ibus_panel_service_candidate_clicked (self->panel_service, index, button, state);
}

static void
shell_ibus_candidate_popup_constructed (GObject *object)
{
  ShellIBusCandidatePopup *self = SHELL_IBUS_CANDIDATE_POPUP (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellGlobal *global;
  ClutterActor *input_panel_group;
  ClutterActor *box;
  ShellIBusCandidateArea *area;

  G_OBJECT_CLASS (shell_ibus_candidate_popup_parent_class)->constructed (object);

  global = shell_global_get ();

  clutter_actor_hide (actor);
  st_widget_set_style_class_name (ST_WIDGET (self), "candidate-popup-boxpointer");

  input_panel_group = meta_compositor_get_input_panel_group (shell_global_get_compositor (global));

  g_set_weak_pointer (&self->dummy_cursor,
                      g_object_new (CLUTTER_TYPE_ACTOR, "opacity", 0, NULL));
  clutter_actor_add_child (input_panel_group, self->dummy_cursor);
  clutter_actor_add_child (input_panel_group, actor);

  box = g_object_new (ST_TYPE_BOX_LAYOUT,
                      "style-class", "candidate-popup-content",
                      "orientation", CLUTTER_ORIENTATION_VERTICAL,
                      NULL);
  st_bin_set_child (shell_box_pointer_get_bin (SHELL_BOX_POINTER (self)), box);

  g_set_weak_pointer (&self->preedit_text,
                      g_object_new (ST_TYPE_LABEL,
                                    "style-class", "candidate-popup-text",
                                    "visible", FALSE,
                                    NULL));
  clutter_actor_add_child (box, CLUTTER_ACTOR (self->preedit_text));

  g_set_weak_pointer (&self->aux_text,
                      g_object_new (ST_TYPE_LABEL,
                                    "style-class", "candidate-popup-text",
                                    "visible", FALSE,
                                    NULL));
  clutter_actor_add_child (box, CLUTTER_ACTOR (self->aux_text));

  area = candidate_area_new ();
  g_set_weak_pointer (&self->candidate_area, area);
  clutter_actor_add_child (box, CLUTTER_ACTOR (area));

  g_signal_connect_object (area, "previous-page",
                           G_CALLBACK (area_previous_page_cb), self, 0);
  g_signal_connect_object (area, "next-page",
                           G_CALLBACK (area_next_page_cb), self, 0);
  g_signal_connect_object (area, "cursor-up",
                           G_CALLBACK (area_cursor_up_cb), self, 0);
  g_signal_connect_object (area, "cursor-down",
                           G_CALLBACK (area_cursor_down_cb), self, 0);
  g_signal_connect_object (area, "candidate-clicked",
                           G_CALLBACK (area_candidate_clicked_cb), self, 0);
}

static void
popup_disconnect_panel_service (ShellIBusCandidatePopup *self)
{
  if (self->panel_service != NULL)
    g_signal_handlers_disconnect_by_data (self->panel_service, self);
  g_clear_object (&self->panel_service);
}

static void
shell_ibus_candidate_popup_dispose (GObject *object)
{
  ShellIBusCandidatePopup *self = SHELL_IBUS_CANDIDATE_POPUP (object);

  popup_disconnect_panel_service (self);

  if (self->dummy_cursor != NULL)
    clutter_actor_destroy (self->dummy_cursor);
  g_clear_weak_pointer (&self->dummy_cursor);
  g_clear_weak_pointer (&self->preedit_text);
  g_clear_weak_pointer (&self->aux_text);
  g_clear_weak_pointer (&self->candidate_area);

  G_OBJECT_CLASS (shell_ibus_candidate_popup_parent_class)->dispose (object);
}

static void
shell_ibus_candidate_popup_class_init (ShellIBusCandidatePopupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_ibus_candidate_popup_constructed;
  object_class->dispose = shell_ibus_candidate_popup_dispose;
}

static void
shell_ibus_candidate_popup_init (ShellIBusCandidatePopup *self)
{
}

/**
 * shell_ibus_candidate_popup_new:
 *
 * Creates the candidate popup (`new CandidatePopup ()`). It is added to the
 * input panel group of the compositor, which owns it.
 *
 * Returns: (transfer none): a new #ShellIBusCandidatePopup
 */
ShellIBusCandidatePopup *
shell_ibus_candidate_popup_new (void)
{
  return g_object_new (SHELL_TYPE_IBUS_CANDIDATE_POPUP,
                       "arrow-side", ST_SIDE_TOP,
                       NULL);
}

/**
 * shell_ibus_candidate_popup_set_panel_service:
 * @self: a #ShellIBusCandidatePopup
 * @panel_service: (nullable): the panel service of IBus
 *
 * Follows the signals of @panel_service to show the text and the candidates
 * of the engine (`setPanelService ()`). The previous panel service is
 * dropped.
 */
void
shell_ibus_candidate_popup_set_panel_service (ShellIBusCandidatePopup *self,
                                              IBusPanelService        *panel_service)
{
  g_return_if_fail (SHELL_IS_IBUS_CANDIDATE_POPUP (self));
  g_return_if_fail (panel_service == NULL || IBUS_IS_PANEL_SERVICE (panel_service));

  popup_disconnect_panel_service (self);

  if (panel_service == NULL)
    return;

  self->panel_service = g_object_ref (panel_service);

  g_signal_connect (panel_service, "set-cursor-location",
                    G_CALLBACK (ps_set_cursor_location_cb), self);
  g_signal_connect (panel_service, "set-cursor-location-relative",
                    G_CALLBACK (ps_set_cursor_location_relative_cb), self);
  g_signal_connect (panel_service, "update-preedit-text",
                    G_CALLBACK (ps_update_preedit_text_cb), self);
  g_signal_connect (panel_service, "show-preedit-text",
                    G_CALLBACK (ps_show_preedit_text_cb), self);
  g_signal_connect (panel_service, "hide-preedit-text",
                    G_CALLBACK (ps_hide_preedit_text_cb), self);
  g_signal_connect (panel_service, "update-auxiliary-text",
                    G_CALLBACK (ps_update_auxiliary_text_cb), self);
  g_signal_connect (panel_service, "show-auxiliary-text",
                    G_CALLBACK (ps_show_auxiliary_text_cb), self);
  g_signal_connect (panel_service, "hide-auxiliary-text",
                    G_CALLBACK (ps_hide_auxiliary_text_cb), self);
  g_signal_connect (panel_service, "update-lookup-table",
                    G_CALLBACK (ps_update_lookup_table_cb), self);
  g_signal_connect (panel_service, "show-lookup-table",
                    G_CALLBACK (ps_show_lookup_table_cb), self);
  g_signal_connect (panel_service, "hide-lookup-table",
                    G_CALLBACK (ps_hide_lookup_table_cb), self);
  g_signal_connect (panel_service, "focus-out",
                    G_CALLBACK (ps_focus_out_cb), self);
}
