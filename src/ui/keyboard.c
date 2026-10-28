/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * keyboard.c: The on-screen keyboard. Port of js/ui/keyboard.js.
 *
 * Only #ShellOskManager (the former `Keyboard.KeyboardManager`) is public.
 * The other classes of the JS are private types of this file: AspectContainer
 * (ShellOskAspectContainer), NoGrabPopup (ShellOskNoGrabPopup),
 * KeyContainer (ShellOskKeyContainer), Suggestions (ShellOskSuggestions),
 * Key (ShellOskKey), FocusTracker (ShellOskFocusTracker), EmojiPager
 * (ShellOskEmojiPager), EmojiSelection (ShellOskEmojiSelection), Keyboard
 * (ShellOsk, the actor) and KeyboardController (ShellOskController).
 * LanguageSelectionPopup is a function creating a ShellOskNoGrabPopup,
 * KeyboardModel is a JSON parse of `resource:///org/gnome/shell/osk-layouts/`.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - js/ui/status/keyboard.js is still JS and its InputSourceManager is a
 *    plain EventEmitter. It pushes the list of input sources and the current
 *    input source with shell_osk_manager_set_input_sources () and
 *    shell_osk_manager_set_current_input_source (), which the controller
 *    reads where it read `InputSourceManager.currentSource`, and the language
 *    menu asks for a switch with #ShellOskManager::activate-input-source.
 *    With no current input source the layout group is "us" (the JS threw).
 *  - `Keyboard._shouldShowEmoji ()` read `this._purpose`, which was never
 *    assigned, so the emoji key was never shown. The purpose of the
 *    controller is used, as the code intends.
 *  - The page-changed signal of the emoji pager declared an integer for the
 *    label of the section it emits, so no section button was ever latched.
 *    It is a string here.
 *  - `Keyboard.open ()` was called with the focus monitor index as its
 *    `immediate` argument. That is kept: the keyboard opens at once when the
 *    focused monitor is not the first one.
 *  - `_getKeyvalsFromString ()` compared the keysym to a string, which was
 *    never equal, so its check for unknown characters is not there.
 *  - `key.level === 1` of the level switch keys compared a string to a
 *    number and so was always false: switching level never latches.
 *  - The handlers which update the level from the surrounding text are
 *    disconnected when the keyboard is destroyed.
 *  - `Main.inputMethod` is the #ShellOskManager:input-method property and
 *    `Main.layoutManager` is read from #ShellMain.
 *
 * Hooks: osk.shown and osk.hidden (docs/api/hooks.md).
 */

#include "config.h"

#include <math.h>
#include <string.h>

#include <clutter/clutter-pango.h>
#include <glib/gi18n-lib.h>
#include <json-glib/json-glib.h>
#include <meta/compositor.h>
#include <meta/display.h>
#include <meta/meta-backend.h>
#include <meta/window.h>

#include "shell-edge-drag-gesture.h"
#include "shell-global.h"
#include "shell-hooks.h"
#include "misc/ibus-manager.h"
#include "ui/environment.h"
#include "ui/keyboard.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/page-indicators.h"
#include "ui/popup-menu.h"
#include "ui/swipe-tracker.h"

#define KEYBOARD_ANIMATION_TIME 150
#define KEYBOARD_REST_TIME (KEYBOARD_ANIMATION_TIME * 2)

#define A11Y_APPLICATIONS_SCHEMA "org.gnome.desktop.a11y.applications"
#define SHOW_KEYBOARD "screen-keyboard-enabled"
#define EMOJI_PAGE_SEPARATION 32

/* KeyContainer puts keys in a grid where a 1:1 key takes this size */
#define KEY_SIZE 2

#define KEY_RELEASE_TIMEOUT 50
#define BACKSPACE_WORD_DELETE_THRESHOLD 50

#define OSK_LAYOUTS_URI "resource:///org/gnome/shell/osk-layouts/"

/* Object data of the buttons of a ShellOskKey */
#define COMMIT_STRING_DATA "shell-osk-commit-string"
#define EXTENDED_KEY_DATA "shell-osk-extended-key"
/* Object data of a ShellOskKey made from a key of the layout */
#define KEY_SPEC_DATA "shell-osk-key-spec"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static GWeakRef *
weak_ref_new (gpointer object)
{
  GWeakRef *ref = g_new0 (GWeakRef, 1);

  g_weak_ref_init (ref, object);
  return ref;
}

static void
weak_ref_free (GWeakRef *ref)
{
  g_weak_ref_clear (ref);
  g_free (ref);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ShellMonitor, shell_monitor_free)

static void
actor_set_visible (ClutterActor *actor,
                   gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

static ShellEaseParams
make_ease_params (int                  duration,
                  ClutterAnimationMode mode)
{
  ShellEaseParams params = {
    .duration = duration,
    .delay = -1,
    .mode = mode,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  return params;
}

static ClutterActor *
new_actor_with_layout (ClutterLayoutManager *layout)
{
  return g_object_new (CLUTTER_TYPE_ACTOR,
                       "layout-manager", layout,
                       NULL);
}

static ClutterGridLayout *
new_grid_layout (void)
{
  ClutterGridLayout *grid = CLUTTER_GRID_LAYOUT (clutter_grid_layout_new ());

  clutter_grid_layout_set_orientation (grid, CLUTTER_ORIENTATION_HORIZONTAL);
  clutter_grid_layout_set_column_homogeneous (grid, TRUE);
  clutter_grid_layout_set_row_homogeneous (grid, TRUE);

  return grid;
}

/* ------------------------------------------------------------------------ */
/* ShellOskAspectContainer                                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_OSK_ASPECT_CONTAINER (shell_osk_aspect_container_get_type ())
G_DECLARE_FINAL_TYPE (ShellOskAspectContainer, shell_osk_aspect_container,
                      SHELL, OSK_ASPECT_CONTAINER, StWidget)

struct _ShellOskAspectContainer
{
  StWidget parent;

  float ratio;
};

G_DEFINE_FINAL_TYPE (ShellOskAspectContainer, shell_osk_aspect_container, ST_TYPE_WIDGET)

static void
aspect_container_set_ratio (ShellOskAspectContainer *self,
                            double                   rel_width,
                            double                   rel_height)
{
  self->ratio = rel_height != 0 ? rel_width / rel_height : 1;
  clutter_actor_queue_relayout (CLUTTER_ACTOR (self));
}

static void
aspect_container_get_preferred_width (ClutterActor *actor,
                                      float         for_height,
                                      float        *min_width_p,
                                      float        *natural_width_p)
{
  ShellOskAspectContainer *self = SHELL_OSK_ASPECT_CONTAINER (actor);
  float min_width, natural_width;

  CLUTTER_ACTOR_CLASS (shell_osk_aspect_container_parent_class)->get_preferred_width (actor,
                                                                                      for_height,
                                                                                      &min_width,
                                                                                      &natural_width);

  if (for_height > 0)
    natural_width = for_height * self->ratio;

  if (min_width_p != NULL)
    *min_width_p = min_width;
  if (natural_width_p != NULL)
    *natural_width_p = natural_width;
}

static void
aspect_container_get_preferred_height (ClutterActor *actor,
                                       float         for_width,
                                       float        *min_height_p,
                                       float        *natural_height_p)
{
  ShellOskAspectContainer *self = SHELL_OSK_ASPECT_CONTAINER (actor);
  float min_height, natural_height;

  CLUTTER_ACTOR_CLASS (shell_osk_aspect_container_parent_class)->get_preferred_height (actor,
                                                                                       for_width,
                                                                                       &min_height,
                                                                                       &natural_height);

  if (for_width > 0)
    natural_height = for_width / self->ratio;

  if (min_height_p != NULL)
    *min_height_p = min_height;
  if (natural_height_p != NULL)
    *natural_height_p = natural_height;
}

static void
aspect_container_allocate (ClutterActor          *actor,
                           const ClutterActorBox *box)
{
  ShellOskAspectContainer *self = SHELL_OSK_ASPECT_CONTAINER (actor);
  ClutterActorBox allocation = *box;
  float width = clutter_actor_box_get_width (box);
  float height = clutter_actor_box_get_height (box);

  if (width > 0 && height > 0)
    {
      float size_ratio = width / height;

      if (size_ratio >= self->ratio)
        {
          /* Restrict horizontally */
          float restricted_width = height * self->ratio;
          float diff = width - restricted_width;

          allocation.x1 += floorf (diff / 2);
          allocation.x2 -= ceilf (diff / 2);
        }
    }

  CLUTTER_ACTOR_CLASS (shell_osk_aspect_container_parent_class)->allocate (actor, &allocation);
}

static void
shell_osk_aspect_container_class_init (ShellOskAspectContainerClass *klass)
{
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  actor_class->get_preferred_width = aspect_container_get_preferred_width;
  actor_class->get_preferred_height = aspect_container_get_preferred_height;
  actor_class->allocate = aspect_container_allocate;
}

static void
shell_osk_aspect_container_init (ShellOskAspectContainer *self)
{
  self->ratio = 1;
}

static ShellOskAspectContainer *
aspect_container_new (gboolean x_expand)
{
  return g_object_new (SHELL_TYPE_OSK_ASPECT_CONTAINER,
                       "layout-manager", clutter_bin_layout_new (),
                       "x-expand", x_expand,
                       "y-expand", TRUE,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellOskNoGrabPopup                                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_OSK_NO_GRAB_POPUP (shell_osk_no_grab_popup_get_type ())
G_DECLARE_FINAL_TYPE (ShellOskNoGrabPopup, shell_osk_no_grab_popup,
                      SHELL, OSK_NO_GRAB_POPUP, ShellPopupMenu)

struct _ShellOskNoGrabPopup
{
  ShellPopupMenu parent;

  ClutterAction *click_gesture;
};

G_DEFINE_FINAL_TYPE (ShellOskNoGrabPopup, shell_osk_no_grab_popup, SHELL_TYPE_POPUP_MENU)

static void
no_grab_popup_remove_gesture (ShellOskNoGrabPopup *self)
{
  ClutterActor *attached;

  if (self->click_gesture == NULL)
    return;

  /* The gesture is only on the stage while the popup is open */
  attached = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->click_gesture));
  if (attached != NULL)
    clutter_actor_remove_action (attached, self->click_gesture);
}

static void
no_grab_popup_source_destroy_cb (ClutterActor        *source,
                                 ShellOskNoGrabPopup *self)
{
  shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (self), SHELL_POPUP_ANIMATION_FULL);
}

static void
no_grab_popup_source_mapped_cb (ClutterActor        *source,
                                GParamSpec          *pspec,
                                ShellOskNoGrabPopup *self)
{
  if (!clutter_actor_is_mapped (source))
    shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (self), SHELL_POPUP_ANIMATION_FULL);
}

static gboolean
no_grab_popup_may_recognize_cb (ClutterGesture      *gesture,
                                ShellOskNoGrabPopup *self)
{
  ClutterActor *popup_actor = shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (self));
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());
  ClutterActor *target;
  graphene_point_t coords;

  clutter_press_gesture_get_coords_abs (CLUTTER_PRESS_GESTURE (gesture), &coords);
  target = clutter_stage_get_actor_at_pos (stage, CLUTTER_PICK_ALL, coords.x, coords.y);

  if (target == NULL || popup_actor == NULL)
    return TRUE;

  return target != popup_actor && !clutter_actor_contains (popup_actor, target);
}

static void
no_grab_popup_recognize_cb (ClutterGesture      *gesture,
                            ShellOskNoGrabPopup *self)
{
  shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (self), SHELL_POPUP_ANIMATION_FULL);
}

static gboolean
no_grab_popup_open (ShellPopupMenuBase  *base,
                    ShellPopupAnimation  animate)
{
  ShellOskNoGrabPopup *self = SHELL_OSK_NO_GRAB_POPUP (base);
  ShellPopupMenuBaseClass *parent_class =
    SHELL_POPUP_MENU_BASE_CLASS (shell_osk_no_grab_popup_parent_class);
  ClutterStage *stage;

  if (!parent_class->open (base, animate))
    return FALSE;

  stage = shell_global_get_stage (shell_global_get ());
  clutter_actor_add_action_full (CLUTTER_ACTOR (stage),
                                 "close-popup-gesture",
                                 CLUTTER_PHASE_CAPTURE,
                                 self->click_gesture);
  return TRUE;
}

static gboolean
no_grab_popup_close (ShellPopupMenuBase  *base,
                     ShellPopupAnimation  animate)
{
  ShellOskNoGrabPopup *self = SHELL_OSK_NO_GRAB_POPUP (base);
  ShellPopupMenuBaseClass *parent_class =
    SHELL_POPUP_MENU_BASE_CLASS (shell_osk_no_grab_popup_parent_class);

  if (!parent_class->close (base, animate))
    return FALSE;

  no_grab_popup_remove_gesture (self);
  return TRUE;
}

static void
no_grab_popup_destroy (ShellPopupMenuBase *base)
{
  ShellOskNoGrabPopup *self = SHELL_OSK_NO_GRAB_POPUP (base);
  ShellPopupMenuBaseClass *parent_class =
    SHELL_POPUP_MENU_BASE_CLASS (shell_osk_no_grab_popup_parent_class);
  ClutterActor *source_actor = shell_popup_menu_base_get_source_actor (base);

  no_grab_popup_remove_gesture (self);
  if (source_actor != NULL)
    g_signal_handlers_disconnect_by_data (source_actor, self);

  parent_class->destroy (base);
}

static void
no_grab_popup_constructed (GObject *object)
{
  ShellOskNoGrabPopup *self = SHELL_OSK_NO_GRAB_POPUP (object);
  ClutterActor *source_actor;

  G_OBJECT_CLASS (shell_osk_no_grab_popup_parent_class)->constructed (object);

  source_actor = shell_popup_menu_base_get_source_actor (SHELL_POPUP_MENU_BASE (self));
  if (source_actor != NULL)
    {
      g_signal_connect_object (source_actor, "destroy",
                               G_CALLBACK (no_grab_popup_source_destroy_cb), self, 0);
      g_signal_connect_object (source_actor, "notify::mapped",
                               G_CALLBACK (no_grab_popup_source_mapped_cb), self, 0);
    }
}

static void
no_grab_popup_dispose (GObject *object)
{
  ShellOskNoGrabPopup *self = SHELL_OSK_NO_GRAB_POPUP (object);

  no_grab_popup_remove_gesture (self);
  g_clear_object (&self->click_gesture);

  G_OBJECT_CLASS (shell_osk_no_grab_popup_parent_class)->dispose (object);
}

static void
shell_osk_no_grab_popup_class_init (ShellOskNoGrabPopupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellPopupMenuBaseClass *base_class = SHELL_POPUP_MENU_BASE_CLASS (klass);

  object_class->constructed = no_grab_popup_constructed;
  object_class->dispose = no_grab_popup_dispose;

  base_class->open = no_grab_popup_open;
  base_class->close = no_grab_popup_close;
  base_class->destroy = no_grab_popup_destroy;
}

static void
shell_osk_no_grab_popup_init (ShellOskNoGrabPopup *self)
{
  self->click_gesture = g_object_ref_sink (clutter_click_gesture_new ());
  g_signal_connect (self->click_gesture, "may-recognize",
                    G_CALLBACK (no_grab_popup_may_recognize_cb), self);
  g_signal_connect (self->click_gesture, "recognize",
                    G_CALLBACK (no_grab_popup_recognize_cb), self);
}

static ShellOskNoGrabPopup *
no_grab_popup_new (ClutterActor *actor,
                   StSide        arrow_side)
{
  return g_object_new (SHELL_TYPE_OSK_NO_GRAB_POPUP,
                       "source-actor", actor,
                       "arrow-alignment", 0.5,
                       "arrow-side", arrow_side,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellOskKeyContainer                                                      */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_OSK_KEY_CONTAINER (shell_osk_key_container_get_type ())
G_DECLARE_FINAL_TYPE (ShellOskKeyContainer, shell_osk_key_container,
                      SHELL, OSK_KEY_CONTAINER, StWidget)

struct _ShellOskKeyContainer
{
  StWidget parent;

  ClutterGridLayout *grid_layout; /* weak, owned by the actor */
  int n_rows;
  double current_col;
  double max_cols;

  /* The keys which show the shift or caps lock icon, strong refs */
  GPtrArray *shift_keys;
};

G_DEFINE_FINAL_TYPE (ShellOskKeyContainer, shell_osk_key_container, ST_TYPE_WIDGET)

static void
key_container_append_row (ShellOskKeyContainer *self)
{
  self->n_rows++;
  self->current_col = 0;
}

static void
key_container_append_key (ShellOskKeyContainer *self,
                          ClutterActor         *key,
                          double                width,
                          double                height,
                          double                left_offset)
{
  double left = self->current_col + left_offset;
  double top = self->n_rows;

  clutter_grid_layout_attach (self->grid_layout, key,
                              (int) (left * KEY_SIZE), (int) (top * KEY_SIZE),
                              (int) (width * KEY_SIZE), (int) (height * KEY_SIZE));

  self->current_col += left_offset + width;
  self->max_cols = MAX (self->current_col, self->max_cols);
}

static void
key_container_get_ratio (ShellOskKeyContainer *self,
                         double               *n_cols,
                         double               *n_rows)
{
  *n_cols = self->max_cols;
  *n_rows = self->n_rows;
}

static void
key_container_constructed (GObject *object)
{
  ShellOskKeyContainer *self = SHELL_OSK_KEY_CONTAINER (object);
  ClutterGridLayout *grid;

  G_OBJECT_CLASS (shell_osk_key_container_parent_class)->constructed (object);

  grid = new_grid_layout ();
  clutter_actor_set_layout_manager (CLUTTER_ACTOR (self), CLUTTER_LAYOUT_MANAGER (grid));
  self->grid_layout = grid;
}

static void
key_container_dispose (GObject *object)
{
  ShellOskKeyContainer *self = SHELL_OSK_KEY_CONTAINER (object);

  g_clear_pointer (&self->shift_keys, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_osk_key_container_parent_class)->dispose (object);
}

static void
shell_osk_key_container_class_init (ShellOskKeyContainerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = key_container_constructed;
  object_class->dispose = key_container_dispose;
}

static void
shell_osk_key_container_init (ShellOskKeyContainer *self)
{
  self->shift_keys = g_ptr_array_new_with_free_func (g_object_unref);
}

static ShellOskKeyContainer *
key_container_new (void)
{
  return g_object_new (SHELL_TYPE_OSK_KEY_CONTAINER,
                       "x-expand", TRUE,
                       "y-expand", TRUE,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellOskSuggestions                                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_OSK_SUGGESTIONS (shell_osk_suggestions_get_type ())
G_DECLARE_FINAL_TYPE (ShellOskSuggestions, shell_osk_suggestions,
                      SHELL, OSK_SUGGESTIONS, StBoxLayout)

struct _ShellOskSuggestions
{
  StBoxLayout parent;
};

G_DEFINE_FINAL_TYPE (ShellOskSuggestions, shell_osk_suggestions, ST_TYPE_BOX_LAYOUT)

typedef struct
{
  ShellOskSuggestionFunc callback;
  gpointer user_data;
  GDestroyNotify destroy;
} SuggestionData;

static void
suggestion_data_free (SuggestionData *data)
{
  if (data->destroy != NULL)
    data->destroy (data->user_data);
  g_free (data);
}

static void
suggestion_clicked_cb (StButton       *button,
                       int             clicked_button,
                       SuggestionData *data)
{
  if (data->callback != NULL)
    data->callback (data->user_data);
}

static void
suggestions_add (ShellOskSuggestions   *self,
                 const char            *word,
                 ShellOskSuggestionFunc callback,
                 gpointer               user_data,
                 GDestroyNotify         destroy)
{
  SuggestionData *data = g_new0 (SuggestionData, 1);
  ClutterActor *button;

  data->callback = callback;
  data->user_data = user_data;
  data->destroy = destroy;

  button = g_object_new (ST_TYPE_BUTTON,
                         "label", word,
                         NULL);
  g_signal_connect_data (button, "clicked",
                         G_CALLBACK (suggestion_clicked_cb),
                         data, (GClosureNotify) suggestion_data_free, 0);
  clutter_actor_add_child (CLUTTER_ACTOR (self), button);
}

static void
suggestions_clear (ShellOskSuggestions *self)
{
  clutter_actor_remove_all_children (CLUTTER_ACTOR (self));
}

static void
suggestions_set_visible (ShellOskSuggestions *self,
                         gboolean             visible)
{
  ClutterActor *child;

  for (child = clutter_actor_get_first_child (CLUTTER_ACTOR (self));
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      actor_set_visible (child, visible);
    }
}

static void
suggestions_constructed (GObject *object)
{
  ShellOskSuggestions *self = SHELL_OSK_SUGGESTIONS (object);

  G_OBJECT_CLASS (shell_osk_suggestions_parent_class)->constructed (object);

  st_box_layout_set_orientation (ST_BOX_LAYOUT (self), CLUTTER_ORIENTATION_HORIZONTAL);
  st_widget_set_style_class_name (ST_WIDGET (self), "word-suggestions");
  clutter_actor_set_x_align (CLUTTER_ACTOR (self), CLUTTER_ACTOR_ALIGN_CENTER);
  clutter_actor_show (CLUTTER_ACTOR (self));
}

static void
shell_osk_suggestions_class_init (ShellOskSuggestionsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = suggestions_constructed;
}

static void
shell_osk_suggestions_init (ShellOskSuggestions *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellOskKey                                                               */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_OSK_KEY (shell_osk_key_get_type ())
G_DECLARE_FINAL_TYPE (ShellOskKey, shell_osk_key, SHELL, OSK_KEY, StBoxLayout)

enum
{
  KEY_LONG_PRESS,
  KEY_RELEASED,
  KEY_KEYVAL,
  KEY_COMMIT,
  KEY_LAST_SIGNAL
};

static guint key_signals[KEY_LAST_SIGNAL] = { 0, };

struct _ShellOskKey
{
  StBoxLayout parent;

  guint keyval;
  StButton *key_button;       /* weak, a child */
  StIcon *icon;               /* weak, the child of key_button */
  GStrv extended_keys;
  ShellOskNoGrabPopup *menu;  /* owned */
  gboolean has_action;
};

G_DEFINE_FINAL_TYPE (ShellOskKey, shell_osk_key, ST_TYPE_BOX_LAYOUT)



static void osk_key_long_press_recognize_cb (ClutterGesture *gesture,
                                             ShellOskKey    *self);

static void
osk_key_hide_subkeys (ShellOskKey *self)
{
  if (self->menu != NULL)
    shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (self->menu),
                                 SHELL_POPUP_ANIMATION_FULL);
}

static void
osk_key_show_subkeys (ShellOskKey *self)
{
  if (self->menu != NULL)
    shell_popup_menu_base_open (SHELL_POPUP_MENU_BASE (self->menu),
                                SHELL_POPUP_ANIMATION_FULL);
}

static void
osk_key_button_clicked_cb (StButton    *button,
                           int          clicked_button,
                           ShellOskKey *self)
{
  g_autoptr (ShellOskKey) keep_alive = g_object_ref (self);
  const char *commit_string = g_object_get_data (G_OBJECT (button), COMMIT_STRING_DATA);

  if (self->keyval != 0 && button == self->key_button)
    g_signal_emit (self, key_signals[KEY_KEYVAL], 0, self->keyval);
  else if (commit_string != NULL)
    g_signal_emit (self, key_signals[KEY_COMMIT], 0, commit_string);
  else if (!self->has_action)
    g_warning ("Need keyval, commitString or an action");

  g_signal_emit (self, key_signals[KEY_RELEASED], 0);
  osk_key_hide_subkeys (self);
}

static StButton *
osk_key_make_button (ShellOskKey *self,
                     const char  *commit_string,
                     const char  *label,
                     const char  *icon_name)
{
  StButton *button = g_object_new (ST_TYPE_BUTTON,
                                   "style-class", "keyboard-key",
                                   "x-expand", TRUE,
                                   NULL);
  ClutterAction *long_press_gesture;

  if (icon_name != NULL && *icon_name != '\0')
    {
      StIcon *icon = g_object_new (ST_TYPE_ICON,
                                   "icon-name", icon_name,
                                   NULL);

      st_bin_set_child (ST_BIN (button), CLUTTER_ACTOR (icon));
      g_set_weak_pointer (&self->icon, icon);
    }
  else if (label != NULL && *label != '\0')
    {
      st_button_set_label (button, label);
    }
  else if (commit_string != NULL && *commit_string != '\0')
    {
      st_button_set_label (button, commit_string);
    }

  if (commit_string != NULL && *commit_string != '\0')
    {
      g_object_set_data_full (G_OBJECT (button), COMMIT_STRING_DATA,
                              g_strdup (commit_string), g_free);
    }

  long_press_gesture = clutter_long_press_gesture_new ();
  g_signal_connect_object (long_press_gesture, "recognize",
                           G_CALLBACK (osk_key_long_press_recognize_cb), self, 0);
  clutter_actor_add_action (CLUTTER_ACTOR (button), long_press_gesture);

  g_signal_connect_object (button, "clicked",
                           G_CALLBACK (osk_key_button_clicked_cb), self, 0);

  return button;
}

static void
osk_key_ensure_extended_keys_popup (ShellOskKey *self)
{
  ShellPopupMenuBase *base;
  ShellLayoutManager *layout_manager;
  StBoxLayout *box;
  ClutterActorBox allocation;
  int i;

  if (self->extended_keys == NULL || self->extended_keys[0] == NULL)
    return;

  if (self->menu != NULL)
    return;

  self->menu = no_grab_popup_new (CLUTTER_ACTOR (self->key_button), ST_SIDE_BOTTOM);
  base = SHELL_POPUP_MENU_BASE (self->menu);

  st_widget_add_style_class_name (ST_WIDGET (shell_popup_menu_base_get_actor (base)),
                                  "keyboard-subkeys-boxpointer");
  box = shell_popup_menu_base_get_box (base);
  st_box_layout_set_orientation (box, CLUTTER_ORIENTATION_HORIZONTAL);
  st_widget_set_style_class_name (ST_WIDGET (box), "key-container");

  clutter_actor_get_allocation_box (CLUTTER_ACTOR (self->key_button), &allocation);

  for (i = 0; self->extended_keys[i] != NULL; i++)
    {
      const char *extended_key = self->extended_keys[i];
      StButton *button = osk_key_make_button (self, extended_key, NULL, NULL);

      g_object_set_data_full (G_OBJECT (button), EXTENDED_KEY_DATA,
                              g_strdup (extended_key), g_free);
      clutter_actor_add_child (CLUTTER_ACTOR (box), CLUTTER_ACTOR (button));
      clutter_actor_set_size (CLUTTER_ACTOR (button),
                              clutter_actor_box_get_width (&allocation),
                              clutter_actor_box_get_height (&allocation));
    }

  layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  if (layout_manager != NULL)
    shell_layout_manager_add_top_chrome (layout_manager,
                                         shell_popup_menu_base_get_actor (base),
                                         SHELL_CHROME_FLAGS_NONE);
}

static void
osk_key_long_press_recognize_cb (ClutterGesture *gesture,
                                 ShellOskKey    *self)
{
  g_autoptr (ShellOskKey) keep_alive = g_object_ref (self);

  g_signal_emit (self, key_signals[KEY_LONG_PRESS], 0);

  if (self->extended_keys != NULL && self->extended_keys[0] != NULL)
    {
      osk_key_ensure_extended_keys_popup (self);
      osk_key_show_subkeys (self);
    }
}

static void
osk_key_teardown (ShellOskKey *self)
{
  if (self->menu != NULL)
    {
      shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (self->menu));
      g_clear_object (&self->menu);
    }
}

static void
osk_key_destroy (ClutterActor *actor)
{
  osk_key_teardown (SHELL_OSK_KEY (actor));

  CLUTTER_ACTOR_CLASS (shell_osk_key_parent_class)->destroy (actor);
}

static void
osk_key_dispose (GObject *object)
{
  ShellOskKey *self = SHELL_OSK_KEY (object);

  osk_key_teardown (self);
  g_clear_weak_pointer (&self->icon);
  g_clear_pointer (&self->extended_keys, g_strfreev);

  G_OBJECT_CLASS (shell_osk_key_parent_class)->dispose (object);
}

static void
shell_osk_key_class_init (ShellOskKeyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->dispose = osk_key_dispose;
  actor_class->destroy = osk_key_destroy;

  key_signals[KEY_LONG_PRESS] =
    g_signal_new ("long-press",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  key_signals[KEY_RELEASED] =
    g_signal_new ("released",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  key_signals[KEY_KEYVAL] =
    g_signal_new ("keyval",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_UINT);
  key_signals[KEY_COMMIT] =
    g_signal_new ("commit",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
}

static void
shell_osk_key_init (ShellOskKey *self)
{
}

/* The key is a container, so keys can be padded without losing logical
 * proportions between those. */
static ShellOskKey *
osk_key_new (const char         *commit_string,
             const char         *label,
             const char         *icon_name,
             guint               keyval,
             gboolean            has_action,
             const char * const *extended_keys)
{
  ShellOskKey *self = g_object_new (SHELL_TYPE_OSK_KEY,
                                    "style-class", "key-container",
                                    NULL);

  self->keyval = keyval;
  self->has_action = has_action;
  self->extended_keys = g_strdupv ((char **) extended_keys);
  self->key_button = osk_key_make_button (self, commit_string, label, icon_name);

  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (self->key_button));

  return self;
}

static StButton *
osk_key_get_key_button (ShellOskKey *self)
{
  return self->key_button;
}

static void
osk_key_set_latched (ShellOskKey *self,
                     gboolean     latched)
{
  if (self->key_button == NULL)
    return;

  if (latched)
    st_widget_add_style_pseudo_class (ST_WIDGET (self->key_button), "latched");
  else
    st_widget_remove_style_pseudo_class (ST_WIDGET (self->key_button), "latched");
}

static void
osk_key_set_icon_name (ShellOskKey *self,
                       const char  *icon_name)
{
  if (self->icon != NULL)
    st_icon_set_icon_name (self->icon, icon_name);
}

/* ------------------------------------------------------------------------ */
/* KeyboardModel                                                             */
/* ------------------------------------------------------------------------ */

/* Loads the layout of a group of the `osk-layouts` resource. Returns the
 * parser which owns the model, or %NULL if there is no such layout or it does
 * not parse. */
static JsonParser *
load_keyboard_model (const char *group_name)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) error = NULL;
  g_autofree char *uri = g_strdup_printf (OSK_LAYOUTS_URI "%s.json", group_name);
  g_autoptr (GFile) file = g_file_new_for_uri (uri);
  g_autofree char *contents = NULL;
  gsize length;
  JsonNode *root;

  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, &error))
    return NULL;

  if (!json_parser_load_from_data (parser, contents, length, &error))
    return NULL;

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return NULL;

  return g_steal_pointer (&parser);
}

static JsonArray *
json_object_peek_array (JsonObject *object,
                        const char *name)
{
  JsonNode *node;

  if (object == NULL || !json_object_has_member (object, name))
    return NULL;

  node = json_object_get_member (object, name);
  return JSON_NODE_HOLDS_ARRAY (node) ? json_node_get_array (node) : NULL;
}

static const char *
json_object_peek_string (JsonObject *object,
                         const char *name)
{
  JsonNode *node;

  if (object == NULL || !json_object_has_member (object, name))
    return NULL;

  node = json_object_get_member (object, name);
  if (!JSON_NODE_HOLDS_VALUE (node) || json_node_get_value_type (node) != G_TYPE_STRING)
    return NULL;

  return json_node_get_string (node);
}

static double
json_object_peek_double (JsonObject *object,
                         const char *name,
                         double      default_value)
{
  JsonNode *node;

  if (object == NULL || !json_object_has_member (object, name))
    return default_value;

  node = json_object_get_member (object, name);
  if (!JSON_NODE_HOLDS_VALUE (node))
    return default_value;

  switch (json_node_get_value_type (node))
    {
    case G_TYPE_DOUBLE:
    case G_TYPE_INT64:
      return json_node_get_double (node);

    default:
      return default_value;
    }
}

static JsonObject *
json_array_peek_object (JsonArray *array,
                        guint      index)
{
  JsonNode *node = json_array_get_element (array, index);

  return JSON_NODE_HOLDS_OBJECT (node) ? json_node_get_object (node) : NULL;
}

/* ------------------------------------------------------------------------ */
/* ShellOskFocusTracker                                                      */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_OSK_FOCUS_TRACKER (shell_osk_focus_tracker_get_type ())
G_DECLARE_FINAL_TYPE (ShellOskFocusTracker, shell_osk_focus_tracker,
                      SHELL, OSK_FOCUS_TRACKER, GObject)

enum
{
  TRACKER_POSITION_CHANGED,
  TRACKER_WINDOW_GRABBED,
  TRACKER_WINDOW_MOVED,
  TRACKER_LAST_SIGNAL
};

static guint tracker_signals[TRACKER_LAST_SIGNAL] = { 0, };

struct _ShellOskFocusTracker
{
  GObject parent;

  MetaDisplay *display;               /* weak */
  ShellInputMethod *input_method;     /* weak */
  ShellIBusManager *ibus_manager;     /* weak */
  MetaWindow *current_window;         /* weak */

  graphene_rect_t rect;
  gboolean has_rect;
};

G_DEFINE_FINAL_TYPE (ShellOskFocusTracker, shell_osk_focus_tracker, G_TYPE_OBJECT)

static void
focus_tracker_window_position_changed_cb (MetaWindow           *window,
                                          ShellOskFocusTracker *self)
{
  g_signal_emit (self, tracker_signals[TRACKER_WINDOW_MOVED], 0);
}

static void
focus_tracker_set_current_window (ShellOskFocusTracker *self,
                                  MetaWindow           *window)
{
  if (self->current_window != NULL)
    g_signal_handlers_disconnect_by_data (self->current_window, self);

  g_set_weak_pointer (&self->current_window, window);

  if (self->current_window != NULL)
    {
      g_signal_connect_object (self->current_window, "position-changed",
                               G_CALLBACK (focus_tracker_window_position_changed_cb),
                               self, 0);
    }
}

static void
focus_tracker_set_current_rect (ShellOskFocusTracker *self,
                                const graphene_rect_t *rect_in)
{
  graphene_rect_t rect = *rect_in;

  /* Some clients give us 0-sized rects, in that case set size to 1 */
  if (rect.size.width <= 0)
    rect.size.width = 1;
  if (rect.size.height <= 0)
    rect.size.height = 1;

  if (self->current_window != NULL)
    {
      MtkRectangle frame_rect;
      graphene_rect_t graphene_frame_rect;

      meta_window_get_frame_rect (self->current_window, &frame_rect);
      graphene_rect_init (&graphene_frame_rect,
                          frame_rect.x, frame_rect.y,
                          frame_rect.width, frame_rect.height);

      if (!graphene_rect_intersection (&graphene_frame_rect, &rect, NULL))
        return;
    }

  if (self->has_rect && graphene_rect_equal (&self->rect, &rect))
    return;

  self->rect = rect;
  self->has_rect = TRUE;
  g_signal_emit (self, tracker_signals[TRACKER_POSITION_CHANGED], 0);
}

static void
focus_tracker_get_current_rect (ShellOskFocusTracker *self,
                                double               *x,
                                double               *y,
                                double               *width,
                                double               *height)
{
  *x = self->rect.origin.x;
  *y = self->rect.origin.y;
  *width = self->rect.size.width;
  *height = self->rect.size.height;
}

static void
focus_tracker_focus_window_cb (MetaDisplay          *display,
                               GParamSpec           *pspec,
                               ShellOskFocusTracker *self)
{
  focus_tracker_set_current_window (self, meta_display_get_focus_window (display));
}

static void
focus_tracker_grab_op_begin_cb (MetaDisplay          *display,
                                MetaWindow           *window,
                                MetaGrabOp            op,
                                ClutterSprite        *sprite,
                                ShellOskFocusTracker *self)
{
  if (window == self->current_window &&
      (op == META_GRAB_OP_MOVING || op == META_GRAB_OP_KEYBOARD_MOVING))
    g_signal_emit (self, tracker_signals[TRACKER_WINDOW_GRABBED], 0);
}

static void
focus_tracker_cursor_location_changed_cb (ClutterInputMethod    *input_method,
                                          const graphene_rect_t *rect,
                                          ShellOskFocusTracker  *self)
{
  /* Valid for wayland clients */
  focus_tracker_set_current_rect (self, rect);
}

static void
focus_tracker_set_cursor_location_cb (ShellIBusManager    *manager,
                                      int                   x,
                                      int                   y,
                                      int                   width,
                                      int                   height,
                                      ShellOskFocusTracker *self)
{
  graphene_rect_t rect;

  /* Valid for X11 clients only */
  if (self->input_method == NULL ||
      shell_input_method_get_current_focus (self->input_method) != NULL)
    return;

  graphene_rect_init (&rect, x, y, width, height);
  focus_tracker_set_current_rect (self, &rect);
}

static void
focus_tracker_dispose (GObject *object)
{
  ShellOskFocusTracker *self = SHELL_OSK_FOCUS_TRACKER (object);

  if (self->current_window != NULL)
    g_signal_handlers_disconnect_by_data (self->current_window, self);
  g_clear_weak_pointer (&self->current_window);

  if (self->display != NULL)
    g_signal_handlers_disconnect_by_data (self->display, self);
  g_clear_weak_pointer (&self->display);

  if (self->input_method != NULL)
    g_signal_handlers_disconnect_by_data (self->input_method, self);
  g_clear_weak_pointer (&self->input_method);

  if (self->ibus_manager != NULL)
    g_signal_handlers_disconnect_by_data (self->ibus_manager, self);
  g_clear_weak_pointer (&self->ibus_manager);

  G_OBJECT_CLASS (shell_osk_focus_tracker_parent_class)->dispose (object);
}

static void
shell_osk_focus_tracker_class_init (ShellOskFocusTrackerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = focus_tracker_dispose;

  tracker_signals[TRACKER_POSITION_CHANGED] =
    g_signal_new ("position-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  tracker_signals[TRACKER_WINDOW_GRABBED] =
    g_signal_new ("window-grabbed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  tracker_signals[TRACKER_WINDOW_MOVED] =
    g_signal_new ("window-moved",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_osk_focus_tracker_init (ShellOskFocusTracker *self)
{
}

static ShellOskFocusTracker *
focus_tracker_new (MetaDisplay      *display,
                   ShellInputMethod *input_method)
{
  ShellOskFocusTracker *self = g_object_new (SHELL_TYPE_OSK_FOCUS_TRACKER, NULL);

  g_set_weak_pointer (&self->display, display);
  g_set_weak_pointer (&self->input_method, input_method);
  g_set_weak_pointer (&self->ibus_manager, shell_ibus_manager_get_default ());

  g_signal_connect_object (display, "notify::focus-window",
                           G_CALLBACK (focus_tracker_focus_window_cb), self, 0);
  g_signal_connect_object (display, "grab-op-begin",
                           G_CALLBACK (focus_tracker_grab_op_begin_cb), self, 0);

  focus_tracker_set_current_window (self, meta_display_get_focus_window (display));

  g_signal_connect_object (input_method, "cursor-location-changed",
                           G_CALLBACK (focus_tracker_cursor_location_changed_cb), self, 0);

  g_signal_connect_object (self->ibus_manager, "set-cursor-location",
                           G_CALLBACK (focus_tracker_set_cursor_location_cb), self, 0);

  return self;
}

/* ------------------------------------------------------------------------ */
/* Emoji model                                                               */
/* ------------------------------------------------------------------------ */

typedef struct
{
  char *label;
  GStrv variants;
} EmojiKey;

typedef struct
{
  const char *first;
  const char *label;
  GPtrArray *keys;        /* EmojiKey, %NULL until the section is found */
  ShellOskKey *button;    /* strong */
} EmojiSection;

typedef struct
{
  GPtrArray *page_keys;   /* EmojiKey, not owned */
  int n_pages;
  int page;
  EmojiSection *section;
} EmojiPage;

static void
emoji_key_free (EmojiKey *key)
{
  g_free (key->label);
  g_strfreev (key->variants);
  g_free (key);
}

static void
emoji_section_free (EmojiSection *section)
{
  g_clear_pointer (&section->keys, g_ptr_array_unref);
  g_clear_object (&section->button);
  g_free (section);
}

static void
emoji_page_clear (EmojiPage *page)
{
  g_clear_pointer (&page->page_keys, g_ptr_array_unref);
}

static GPtrArray *
emoji_sections_new (void)
{
  static const struct
  {
    const char *first;
    const char *label;
  } sections[] = {
    { "grinning face", "\U0001F642️" },
    { "selfie", "\U0001F44D️" },
    { "monkey face", "\U0001F337️" },
    { "grapes", "\U0001F374️" },
    { "globe showing Europe-Africa", "✈️" },
    { "jack-o-lantern", "\U0001F3C3️" },
    { "muted speaker", "\U0001F514️" },
    { "ATM sign", "❤️" },
    { "chequered flag", "\U0001F6A9️" },
  };
  GPtrArray *array = g_ptr_array_new_with_free_func ((GDestroyNotify) emoji_section_free);
  guint i;

  for (i = 0; i < G_N_ELEMENTS (sections); i++)
    {
      EmojiSection *section = g_new0 (EmojiSection, 1);

      section->first = sections[i].first;
      section->label = sections[i].label;
      g_ptr_array_add (array, section);
    }

  return array;
}

static EmojiSection *
emoji_sections_find (GPtrArray  *sections,
                     const char *emoji)
{
  guint i;

  for (i = 0; i < sections->len; i++)
    {
      EmojiSection *section = g_ptr_array_index (sections, i);

      if (g_strcmp0 (section->first, emoji) == 0)
        return section;
    }

  return NULL;
}

static void
emoji_sections_populate (GPtrArray *sections)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GFile) file = g_file_new_for_uri (OSK_LAYOUTS_URI "emoji.json");
  g_autofree char *contents = NULL;
  g_autoptr (GStrvBuilder) variants = g_strv_builder_new ();
  EmojiSection *current_section = NULL;
  JsonArray *emoji;
  gsize length;
  guint current_key = 0;
  guint i, n_emoji;

  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, NULL) ||
      !json_parser_load_from_data (parser, contents, length, NULL) ||
      !JSON_NODE_HOLDS_ARRAY (json_parser_get_root (parser)))
    return;

  emoji = json_node_get_array (json_parser_get_root (parser));
  n_emoji = json_array_get_length (emoji);

  for (i = 0; i < n_emoji; i++)
    {
      JsonObject *item = json_array_peek_object (emoji, i);
      JsonObject *key_item = json_array_peek_object (emoji, current_key);
      const char *name = json_object_peek_string (item, "name");
      const char *key_name = json_object_peek_string (key_item, "name");
      EmojiSection *new_section;
      EmojiKey *key;
      const char *key_char;

      if (name == NULL || key_name == NULL)
        continue;

      /* Group variants of a same emoji so they appear on the key popover */
      if (g_str_has_prefix (name, key_name))
        {
          const char *item_char = json_object_peek_string (item, "char");

          g_strv_builder_add (variants, item_char != NULL ? item_char : "");
          if (i < n_emoji - 1)
            continue;
        }

      new_section = emoji_sections_find (sections, key_name);
      if (new_section != NULL)
        {
          current_section = new_section;
          g_clear_pointer (&current_section->keys, g_ptr_array_unref);
          current_section->keys = g_ptr_array_new_with_free_func ((GDestroyNotify) emoji_key_free);
        }

      /* Create the key */
      key_char = json_object_peek_string (key_item, "char");
      if (current_section != NULL && current_section->keys != NULL && key_char != NULL)
        {
          key = g_new0 (EmojiKey, 1);
          key->label = g_strconcat (key_char, "️", NULL);
          key->variants = g_strv_builder_end (variants);
          g_ptr_array_add (current_section->keys, key);
        }
      else
        {
          g_strfreev (g_strv_builder_end (variants));
        }

      current_key = i;
    }
}

/* ------------------------------------------------------------------------ */
/* ShellOskEmojiPager                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_OSK_EMOJI_PAGER (shell_osk_emoji_pager_get_type ())
G_DECLARE_FINAL_TYPE (ShellOskEmojiPager, shell_osk_emoji_pager,
                      SHELL, OSK_EMOJI_PAGER, StWidget)

enum
{
  PAGER_PROP_0,
  PAGER_PROP_DELTA,
  PAGER_N_PROPS
};

static GParamSpec *pager_props[PAGER_N_PROPS] = { NULL, };

enum
{
  PAGER_EMOJI,
  PAGER_PAGE_CHANGED,
  PAGER_LAST_SIGNAL
};

static guint pager_signals[PAGER_LAST_SIGNAL] = { 0, };

struct _ShellOskEmojiPager
{
  StWidget parent;

  GPtrArray *sections;          /* EmojiSection, shared with the selection */
  GArray *pages;                /* EmojiPage */

  ClutterActor *panel;          /* weak, a child */
  int cur_page;                 /* -1 for none */
  int following_page;           /* -1 for none */
  ClutterActor *following_panel; /* weak, a child */
  int delta;
  float width;
  int n_cols;
  int n_rows;

  ShellSwipeTracker *swipe_tracker;
  GBinding *swipe_binding;
};

G_DEFINE_FINAL_TYPE (ShellOskEmojiPager, shell_osk_emoji_pager, ST_TYPE_WIDGET)

static int
emoji_pager_prev_page (ShellOskEmojiPager *self,
                       int                 n_page)
{
  return (n_page + (int) self->pages->len - 1) % (int) self->pages->len;
}

static int
emoji_pager_next_page (ShellOskEmojiPager *self,
                       int                 n_page)
{
  return (n_page + 1) % (int) self->pages->len;
}

static int
emoji_pager_get_following_page (ShellOskEmojiPager *self)
{
  if (self->delta == 0 || self->pages->len == 0 || self->cur_page < 0)
    return -1;

  if (self->delta < 0)
    return emoji_pager_next_page (self, self->cur_page);
  else
    return emoji_pager_prev_page (self, self->cur_page);
}

static void
emoji_pager_key_commit_cb (ShellOskKey        *key,
                           const char         *str,
                           ShellOskEmojiPager *self)
{
  g_signal_emit (self, pager_signals[PAGER_EMOJI], 0, str);
}

static ClutterActor *
emoji_pager_generate_panel (ShellOskEmojiPager *self,
                            int                 n_page)
{
  ClutterGridLayout *grid_layout = new_grid_layout ();
  ClutterActor *panel;
  ClutterActor *expander;
  EmojiPage *page;
  int col = 0;
  int row = 0;
  guint i;

  panel = g_object_new (ST_TYPE_WIDGET,
                        "layout-manager", grid_layout,
                        "style-class", "emoji-page",
                        "x-expand", TRUE,
                        "y-expand", TRUE,
                        NULL);

  /* Set an expander actor so all proportions are right despite the panel
   * not having all rows/cols filled in.
   */
  expander = clutter_actor_new ();
  clutter_grid_layout_attach (grid_layout, expander, 0, 0, self->n_cols, self->n_rows);

  page = &g_array_index (self->pages, EmojiPage, n_page);

  for (i = 0; i < page->page_keys->len; i++)
    {
      EmojiKey *model_key = g_ptr_array_index (page->page_keys, i);
      ShellOskKey *key = osk_key_new (model_key->label, NULL, NULL, 0, FALSE,
                                      (const char * const *) model_key->variants);

      g_signal_connect_object (key, "commit",
                               G_CALLBACK (emoji_pager_key_commit_cb), self, 0);

      clutter_grid_layout_attach (grid_layout, CLUTTER_ACTOR (key), col, row, 1, 1);

      col++;
      if (col >= self->n_cols)
        {
          col = 0;
          row++;
        }
    }

  return panel;
}

static void
emoji_pager_set_delta (ShellOskEmojiPager *self,
                       int                 value)
{
  int following_page;
  float multiplier;

  if (self->delta == value)
    return;

  self->delta = value;
  g_object_notify_by_pspec (G_OBJECT (self), pager_props[PAGER_PROP_DELTA]);

  following_page = emoji_pager_get_following_page (self);

  if (self->following_page != following_page)
    {
      if (self->following_panel != NULL)
        {
          clutter_actor_destroy (self->following_panel);
          self->following_panel = NULL;
        }

      if (following_page != -1)
        {
          self->following_panel = emoji_pager_generate_panel (self, following_page);
          clutter_actor_add_child (CLUTTER_ACTOR (self), self->following_panel);
        }

      self->following_page = following_page;
    }

  multiplier = clutter_actor_get_text_direction (CLUTTER_ACTOR (self)) == CLUTTER_TEXT_DIRECTION_RTL
    ? -1 : 1;

  if (self->panel != NULL)
    g_object_set (self->panel, "translation-x", value * multiplier, NULL);

  if (self->following_panel != NULL)
    {
      float translation = value < 0
        ? self->width + EMOJI_PAGE_SEPARATION
        : -self->width - EMOJI_PAGE_SEPARATION;

      g_object_set (self->following_panel,
                    "translation-x", (value * multiplier) + (translation * multiplier),
                    NULL);
    }
}

static void
emoji_pager_set_current_page (ShellOskEmojiPager *self,
                              int                 n_page)
{
  EmojiPage *page;

  if (self->cur_page == n_page)
    return;

  if (n_page < 0 || n_page >= (int) self->pages->len)
    return;

  self->cur_page = n_page;

  if (self->panel != NULL)
    {
      clutter_actor_destroy (self->panel);
      self->panel = NULL;
    }

  /* Reuse followingPage if possible */
  if (n_page == self->following_page)
    {
      self->panel = self->following_panel;
      self->following_panel = NULL;
    }

  if (self->following_panel != NULL)
    clutter_actor_destroy (self->following_panel);

  self->following_panel = NULL;
  self->following_page = -1;
  self->delta = 0;

  if (self->panel == NULL)
    {
      self->panel = emoji_pager_generate_panel (self, n_page);
      clutter_actor_add_child (CLUTTER_ACTOR (self), self->panel);
    }

  page = &g_array_index (self->pages, EmojiPage, n_page);
  g_signal_emit (self, pager_signals[PAGER_PAGE_CHANGED], 0,
                 page->section->label, page->page, page->n_pages);
}

static void
emoji_pager_set_current_section (ShellOskEmojiPager *self,
                                 EmojiSection       *section,
                                 int                 n_page)
{
  guint i;

  for (i = 0; i < self->pages->len; i++)
    {
      EmojiPage *page = &g_array_index (self->pages, EmojiPage, i);

      if (page->section == section && page->page == n_page)
        {
          emoji_pager_set_current_page (self, i);
          break;
        }
    }
}

static void
emoji_pager_init_paging_info (ShellOskEmojiPager *self)
{
  int items_per_page = self->n_cols * self->n_rows;
  guint i, j;

  g_array_set_size (self->pages, 0);

  if (items_per_page <= 0)
    return;

  for (i = 0; i < self->sections->len; i++)
    {
      EmojiSection *section = g_ptr_array_index (self->sections, i);
      guint n_keys = section->keys != NULL ? section->keys->len : 0;
      int n_pages = (n_keys + items_per_page - 1) / items_per_page;
      int page = -1;
      GPtrArray *page_keys = NULL;

      for (j = 0; j < n_keys; j++)
        {
          if (j % items_per_page == 0)
            {
              EmojiPage new_page;

              page++;
              page_keys = g_ptr_array_new ();
              new_page.page_keys = page_keys;
              new_page.n_pages = n_pages;
              new_page.page = page;
              new_page.section = section;
              g_array_append_val (self->pages, new_page);
            }

          g_ptr_array_add (page_keys, g_ptr_array_index (section->keys, j));
        }
    }
}

static void
emoji_pager_set_ratio (ShellOskEmojiPager *self,
                       int                 n_cols,
                       int                 n_rows)
{
  self->n_cols = n_cols;
  self->n_rows = n_rows;
  emoji_pager_init_paging_info (self);
}

static void
emoji_pager_swipe_update_cb (ShellSwipeTracker *tracker,
                             double             progress,
                             ShellOskEmojiPager *self)
{
  emoji_pager_set_delta (self, (int) (-progress * self->width));
}

static void
emoji_pager_swipe_begin_cb (ShellSwipeTracker  *tracker,
                            guint               monitor,
                            ShellOskEmojiPager *self)
{
  const double points[] = { -1, 0, 1 };

  self->width = clutter_actor_get_width (CLUTTER_ACTOR (self));
  shell_swipe_tracker_confirm_swipe (tracker, self->width, points, G_N_ELEMENTS (points), 0, 0);
}

static void
emoji_pager_ease_done_cb (gboolean  finished,
                          gpointer  user_data)
{
  g_autoptr (ShellOskEmojiPager) self = g_weak_ref_get (user_data);

  if (!finished || self == NULL)
    return;

  emoji_pager_set_current_page (self, emoji_pager_get_following_page (self));
}

static void
emoji_pager_ease_delta (ShellOskEmojiPager *self,
                        int                 target,
                        int                 duration,
                        gboolean            set_page_when_done)
{
  ShellEaseParams params = make_ease_params (duration, 0);
  g_auto (GValue) value = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;

  g_value_init (&value, G_TYPE_INT);
  g_value_set_int (&value, target);

  if (set_page_when_done)
    {
      shell_animatable_ease_property (CLUTTER_ANIMATABLE (self), "delta", &value, &params,
                                      emoji_pager_ease_done_cb, weak_ref_new (self),
                                      (GDestroyNotify) weak_ref_free, &error);
    }
  else
    {
      shell_animatable_ease_property (CLUTTER_ANIMATABLE (self), "delta", &value, &params,
                                      NULL, NULL, NULL, &error);
    }

  if (error != NULL)
    g_warning ("%s: %s", G_STRFUNC, error->message);
}

static void
emoji_pager_swipe_end_cb (ShellSwipeTracker  *tracker,
                          guint64             duration,
                          double              end_progress,
                          ShellOskEmojiPager *self)
{
  clutter_actor_remove_all_transitions (CLUTTER_ACTOR (self));

  if (end_progress == 0)
    {
      emoji_pager_ease_delta (self, 0, (int) duration, FALSE);
    }
  else
    {
      int value = end_progress < 0
        ? (int) (self->width + EMOJI_PAGE_SEPARATION)
        : (int) (-self->width - EMOJI_PAGE_SEPARATION);

      emoji_pager_ease_delta (self, value, (int) duration, TRUE);
    }
}

static void
emoji_pager_teardown (ShellOskEmojiPager *self)
{
  if (self->swipe_binding != NULL)
    {
      g_binding_unbind (self->swipe_binding);
      self->swipe_binding = NULL;
    }

  if (self->swipe_tracker != NULL)
    {
      shell_swipe_tracker_destroy (self->swipe_tracker);
      g_clear_object (&self->swipe_tracker);
    }
}

static void
emoji_pager_destroy (ClutterActor *actor)
{
  ShellOskEmojiPager *self = SHELL_OSK_EMOJI_PAGER (actor);

  emoji_pager_teardown (self);
  self->panel = NULL;
  self->following_panel = NULL;

  CLUTTER_ACTOR_CLASS (shell_osk_emoji_pager_parent_class)->destroy (actor);
}

static void
emoji_pager_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  ShellOskEmojiPager *self = SHELL_OSK_EMOJI_PAGER (object);

  switch (prop_id)
    {
    case PAGER_PROP_DELTA:
      g_value_set_int (value, self->delta);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
emoji_pager_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  ShellOskEmojiPager *self = SHELL_OSK_EMOJI_PAGER (object);

  switch (prop_id)
    {
    case PAGER_PROP_DELTA:
      emoji_pager_set_delta (self, g_value_get_int (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
emoji_pager_constructed (GObject *object)
{
  ShellOskEmojiPager *self = SHELL_OSK_EMOJI_PAGER (object);

  G_OBJECT_CLASS (shell_osk_emoji_pager_parent_class)->constructed (object);

  self->swipe_tracker = g_object_new (SHELL_TYPE_SWIPE_TRACKER,
                                      "actor", self,
                                      "orientation", CLUTTER_ORIENTATION_HORIZONTAL,
                                      "allowed-modes", SHELL_ACTION_MODE_NORMAL | SHELL_ACTION_MODE_OVERVIEW,
                                      "allow-drag", TRUE,
                                      "allow-scroll", TRUE,
                                      "name", "EmojiPager swipe tracker",
                                      NULL);
  g_signal_connect (self->swipe_tracker, "begin",
                    G_CALLBACK (emoji_pager_swipe_begin_cb), self);
  g_signal_connect (self->swipe_tracker, "update",
                    G_CALLBACK (emoji_pager_swipe_update_cb), self);
  g_signal_connect (self->swipe_tracker, "end",
                    G_CALLBACK (emoji_pager_swipe_end_cb), self);

  self->swipe_binding = g_object_bind_property (self, "visible",
                                                self->swipe_tracker, "enabled",
                                                G_BINDING_DEFAULT);
}

static void
emoji_pager_dispose (GObject *object)
{
  ShellOskEmojiPager *self = SHELL_OSK_EMOJI_PAGER (object);

  emoji_pager_teardown (self);
  self->panel = NULL;
  self->following_panel = NULL;
  g_clear_pointer (&self->pages, g_array_unref);
  g_clear_pointer (&self->sections, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_osk_emoji_pager_parent_class)->dispose (object);
}

static void
shell_osk_emoji_pager_class_init (ShellOskEmojiPagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = emoji_pager_constructed;
  object_class->dispose = emoji_pager_dispose;
  object_class->get_property = emoji_pager_get_property;
  object_class->set_property = emoji_pager_set_property;

  actor_class->destroy = emoji_pager_destroy;

  pager_props[PAGER_PROP_DELTA] =
    g_param_spec_int ("delta", NULL, NULL,
                      G_MININT32, G_MAXINT32, 0,
                      G_PARAM_READWRITE |
                      G_PARAM_EXPLICIT_NOTIFY |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, PAGER_N_PROPS, pager_props);

  pager_signals[PAGER_EMOJI] =
    g_signal_new ("emoji",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
  pager_signals[PAGER_PAGE_CHANGED] =
    g_signal_new ("page-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT);
}

static void
shell_osk_emoji_pager_init (ShellOskEmojiPager *self)
{
  self->pages = g_array_new (FALSE, TRUE, sizeof (EmojiPage));
  g_array_set_clear_func (self->pages, (GDestroyNotify) emoji_page_clear);
  self->cur_page = -1;
  self->following_page = -1;
}

static ShellOskEmojiPager *
emoji_pager_new (GPtrArray *sections)
{
  ShellOskEmojiPager *self = g_object_new (SHELL_TYPE_OSK_EMOJI_PAGER,
                                           "layout-manager", clutter_bin_layout_new (),
                                           "reactive", TRUE,
                                           "clip-to-allocation", TRUE,
                                           "y-expand", TRUE,
                                           NULL);

  self->sections = g_ptr_array_ref (sections);

  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellOskEmojiSelection                                                    */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_OSK_EMOJI_SELECTION (shell_osk_emoji_selection_get_type ())
G_DECLARE_FINAL_TYPE (ShellOskEmojiSelection, shell_osk_emoji_selection,
                      SHELL, OSK_EMOJI_SELECTION, StWidget)

enum
{
  SELECTION_EMOJI_SELECTED,
  SELECTION_CLOSE_REQUEST,
  SELECTION_TOGGLE,
  SELECTION_LAST_SIGNAL
};

static guint selection_signals[SELECTION_LAST_SIGNAL] = { 0, };

struct _ShellOskEmojiSelection
{
  StWidget parent;

  GPtrArray *sections;                  /* EmojiSection */
  ClutterGridLayout *grid_layout;       /* weak, owned by the actor */
  ClutterActor *pager_box;              /* strong */
  ShellOskEmojiPager *emoji_pager;      /* weak, in pager_box */
  ShellPageIndicators *page_indicator;  /* weak, in pager_box */
  ClutterActor *bottom_row;             /* strong */
  int cur_page;
};

G_DEFINE_FINAL_TYPE (ShellOskEmojiSelection, shell_osk_emoji_selection, ST_TYPE_WIDGET)

static void
emoji_selection_update_indicator_position (ShellOskEmojiSelection *self)
{
  float width = clutter_actor_get_width (CLUTTER_ACTOR (self->emoji_pager));
  int delta;

  g_object_get (self->emoji_pager, "delta", &delta, NULL);

  shell_page_indicators_set_current_position (self->page_indicator,
                                              self->cur_page - (width > 0 ? delta / width : 0));
}

static void
emoji_selection_pager_page_changed_cb (ShellOskEmojiPager     *pager,
                                       const char             *section_label,
                                       int                     page,
                                       int                     n_pages,
                                       ShellOskEmojiSelection *self)
{
  guint i;

  self->cur_page = page;
  shell_page_indicators_set_n_pages (self->page_indicator, n_pages);
  emoji_selection_update_indicator_position (self);

  for (i = 0; i < self->sections->len; i++)
    {
      EmojiSection *section = g_ptr_array_index (self->sections, i);

      if (section->button != NULL)
        osk_key_set_latched (section->button, g_strcmp0 (section_label, section->label) == 0);
    }
}

static void
emoji_selection_pager_emoji_cb (ShellOskEmojiPager     *pager,
                                const char             *str,
                                ShellOskEmojiSelection *self)
{
  g_signal_emit (self, selection_signals[SELECTION_EMOJI_SELECTED], 0, str);
}

static void
emoji_selection_pager_delta_cb (ShellOskEmojiPager     *pager,
                                GParamSpec             *pspec,
                                ShellOskEmojiSelection *self)
{
  emoji_selection_update_indicator_position (self);
}

static void
emoji_selection_abc_released_cb (ShellOskKey            *key,
                                 ShellOskEmojiSelection *self)
{
  g_signal_emit (self, selection_signals[SELECTION_TOGGLE], 0);
}

static void
emoji_selection_section_released_cb (ShellOskKey            *key,
                                     ShellOskEmojiSelection *self)
{
  EmojiSection *section = g_object_get_data (G_OBJECT (key), "shell-osk-emoji-section");

  emoji_pager_set_current_section (self->emoji_pager, section, 0);
}

static void
emoji_selection_hide_released_cb (ShellOskKey            *key,
                                  ShellOskEmojiSelection *self)
{
  g_signal_emit (self, selection_signals[SELECTION_CLOSE_REQUEST], 0);
}

static ClutterActor *
emoji_selection_create_bottom_row (ShellOskEmojiSelection *self)
{
  ShellOskKeyContainer *row = key_container_new ();
  ShellOskAspectContainer *actor;
  ShellOskKey *key;
  guint i;

  key_container_append_row (row);

  key = osk_key_new (NULL, "ABC", NULL, 0, TRUE, NULL);
  st_widget_add_style_class_name (ST_WIDGET (osk_key_get_key_button (key)), "default-key");
  g_signal_connect_object (key, "released",
                           G_CALLBACK (emoji_selection_abc_released_cb), self, 0);
  key_container_append_key (row, CLUTTER_ACTOR (key), 1.5, 1, 0);

  for (i = 0; i < self->sections->len; i++)
    {
      EmojiSection *section = g_ptr_array_index (self->sections, i);

      /* The JS did not set hasAction here, which logged an error at every click */
      key = osk_key_new (NULL, section->label, NULL, 0, TRUE, NULL);
      g_object_set_data (G_OBJECT (key), "shell-osk-emoji-section", section);
      g_signal_connect_object (key, "released",
                               G_CALLBACK (emoji_selection_section_released_cb), self, 0);
      key_container_append_key (row, CLUTTER_ACTOR (key), 1, 1, 0);

      section->button = g_object_ref (key);
    }

  key = osk_key_new (NULL, NULL, "osk-hide-symbolic", 0, TRUE, NULL);
  st_widget_add_style_class_name (ST_WIDGET (osk_key_get_key_button (key)), "default-key");
  st_widget_add_style_class_name (ST_WIDGET (osk_key_get_key_button (key)), "hide-key");
  g_signal_connect_object (key, "released",
                           G_CALLBACK (emoji_selection_hide_released_cb), self, 0);
  key_container_append_key (row, CLUTTER_ACTOR (key), 1, 1, 0);

  actor = aspect_container_new (TRUE);
  clutter_actor_add_child (CLUTTER_ACTOR (actor), CLUTTER_ACTOR (row));

  return CLUTTER_ACTOR (actor);
}

static void
emoji_selection_set_ratio (ShellOskEmojiSelection *self,
                           double                  n_cols,
                           double                  n_rows)
{
  int rows = (int) floor (n_rows);

  emoji_pager_set_ratio (self->emoji_pager, (int) floor (n_cols), rows - 1);
  aspect_container_set_ratio (SHELL_OSK_ASPECT_CONTAINER (self->bottom_row), n_cols, 1);

  /* (Re)attach actors so the emoji panel fits the ratio and
   * the bottom row is ensured to take 1 row high.
   */
  if (clutter_actor_get_parent (self->pager_box) != NULL)
    clutter_actor_remove_child (CLUTTER_ACTOR (self), self->pager_box);
  if (clutter_actor_get_parent (self->bottom_row) != NULL)
    clutter_actor_remove_child (CLUTTER_ACTOR (self), self->bottom_row);

  clutter_grid_layout_attach (self->grid_layout, self->pager_box, 0, 0, 1, rows - 1);
  clutter_grid_layout_attach (self->grid_layout, self->bottom_row, 0, rows - 1, 1, 1);
}

static void
emoji_selection_map (ClutterActor *actor)
{
  ShellOskEmojiSelection *self = SHELL_OSK_EMOJI_SELECTION (actor);

  emoji_pager_set_current_page (self->emoji_pager, 0);

  CLUTTER_ACTOR_CLASS (shell_osk_emoji_selection_parent_class)->map (actor);
}

static void
emoji_selection_constructed (GObject *object)
{
  ShellOskEmojiSelection *self = SHELL_OSK_EMOJI_SELECTION (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterBoxLayout *box_layout;
  ClutterStage *stage;

  G_OBJECT_CLASS (shell_osk_emoji_selection_parent_class)->constructed (object);

  self->grid_layout = new_grid_layout ();
  clutter_actor_set_layout_manager (actor, CLUTTER_LAYOUT_MANAGER (self->grid_layout));
  st_widget_set_style_class_name (ST_WIDGET (self), "emoji-panel");
  clutter_actor_set_x_expand (actor, TRUE);
  clutter_actor_set_y_expand (actor, TRUE);

  stage = shell_global_get_stage (shell_global_get ());
  clutter_actor_set_text_direction (actor, clutter_actor_get_text_direction (CLUTTER_ACTOR (stage)));

  self->sections = emoji_sections_new ();
  emoji_sections_populate (self->sections);

  box_layout = CLUTTER_BOX_LAYOUT (clutter_box_layout_new ());
  clutter_box_layout_set_orientation (box_layout, CLUTTER_ORIENTATION_VERTICAL);
  self->pager_box = g_object_ref_sink (new_actor_with_layout (CLUTTER_LAYOUT_MANAGER (box_layout)));

  self->emoji_pager = emoji_pager_new (self->sections);
  g_signal_connect_object (self->emoji_pager, "page-changed",
                           G_CALLBACK (emoji_selection_pager_page_changed_cb), self, 0);
  g_signal_connect_object (self->emoji_pager, "emoji",
                           G_CALLBACK (emoji_selection_pager_emoji_cb), self, 0);
  clutter_actor_add_child (self->pager_box, CLUTTER_ACTOR (self->emoji_pager));

  self->page_indicator = shell_page_indicators_new (CLUTTER_ORIENTATION_HORIZONTAL);
  clutter_actor_set_y_expand (CLUTTER_ACTOR (self->page_indicator), FALSE);
  clutter_actor_set_y_align (CLUTTER_ACTOR (self->page_indicator), CLUTTER_ACTOR_ALIGN_START);
  clutter_actor_add_child (self->pager_box, CLUTTER_ACTOR (self->page_indicator));
  shell_page_indicators_set_indicators_reactive (self->page_indicator, FALSE);

  g_signal_connect_object (self->emoji_pager, "notify::delta",
                           G_CALLBACK (emoji_selection_pager_delta_cb), self, 0);

  self->bottom_row = g_object_ref_sink (emoji_selection_create_bottom_row (self));

  self->cur_page = 0;
}

static void
emoji_selection_dispose (GObject *object)
{
  ShellOskEmojiSelection *self = SHELL_OSK_EMOJI_SELECTION (object);

  g_clear_object (&self->pager_box);
  g_clear_object (&self->bottom_row);
  g_clear_pointer (&self->sections, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_osk_emoji_selection_parent_class)->dispose (object);
}

static void
shell_osk_emoji_selection_class_init (ShellOskEmojiSelectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = emoji_selection_constructed;
  object_class->dispose = emoji_selection_dispose;

  actor_class->map = emoji_selection_map;

  selection_signals[SELECTION_EMOJI_SELECTED] =
    g_signal_new ("emoji-selected",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
  selection_signals[SELECTION_CLOSE_REQUEST] =
    g_signal_new ("close-request",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  selection_signals[SELECTION_TOGGLE] =
    g_signal_new ("toggle",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_osk_emoji_selection_init (ShellOskEmojiSelection *self)
{
}


/* ------------------------------------------------------------------------ */
/* ShellOskManager (the instance struct, the class is at the end)            */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_OSK (shell_osk_get_type ())
G_DECLARE_FINAL_TYPE (ShellOsk, shell_osk, SHELL, OSK, StBoxLayout)

#define SHELL_TYPE_OSK_CONTROLLER (shell_osk_controller_get_type ())
G_DECLARE_FINAL_TYPE (ShellOskController, shell_osk_controller, SHELL, OSK_CONTROLLER, GObject)

enum
{
  MANAGER_PROP_0,
  MANAGER_PROP_INPUT_METHOD,
  MANAGER_PROP_KEYBOARD_ACTOR,
  MANAGER_PROP_VISIBLE,
  MANAGER_N_PROPS
};

static GParamSpec *manager_props[MANAGER_N_PROPS] = { NULL, };

enum
{
  MANAGER_VISIBILITY_CHANGED,
  MANAGER_INPUT_SOURCES_REQUESTED,
  MANAGER_ACTIVATE_INPUT_SOURCE,
  MANAGER_LAST_SIGNAL
};

static guint manager_signals[MANAGER_LAST_SIGNAL] = { 0, };

struct _ShellOskManager
{
  GObject parent;

  ShellInputMethod *input_method;
  GSettings *a11y_applications_settings;
  ClutterStage *stage;                /* weak */
  ClutterSeat *seat;                  /* weak */
  MetaBackend *backend;               /* weak */
  ShellLayoutManager *layout_manager; /* weak */
  ClutterInputDevice *last_device;

  ShellEdgeDragGesture *bottom_drag_gesture;
  ShellOsk *keyboard;
  gboolean shutting_down;

  /* What the input source manager pushed */
  GStrv source_names;
  int current_source_index;
  char *current_source_id;
  char *current_source_xkb_id;
  gboolean current_source_is_ibus;
  IBusPropList *current_source_properties;
};

G_DEFINE_FINAL_TYPE (ShellOskManager, shell_osk_manager, G_TYPE_OBJECT)

/* ------------------------------------------------------------------------ */
/* ShellOskController                                                        */
/* ------------------------------------------------------------------------ */

enum
{
  CONTROLLER_GROUP_CHANGED,
  CONTROLLER_PANEL_STATE,
  CONTROLLER_PURPOSE_CHANGED,
  CONTROLLER_CONTENT_HINTS_CHANGED,
  CONTROLLER_LAST_SIGNAL
};

static guint controller_signals[CONTROLLER_LAST_SIGNAL] = { 0, };

struct _ShellOskController
{
  GObject parent;

  ShellOskManager *manager;           /* weak */
  ShellInputMethod *input_method;     /* weak */
  ClutterVirtualInputDevice *virtual_device;

  ClutterInputContentPurpose purpose;
  int osk_completion_enabled;         /* -1 until set_completion_enabled () returned */
  int delete_enabled;                 /* -1 until toggled */
  gboolean backspace_pressed;
  int times_deleted;
  gulong surrounding_update_id;
  guint surrounding_update_timeout_id;
};

G_DEFINE_FINAL_TYPE (ShellOskController, shell_osk_controller, G_TYPE_OBJECT)

static void
controller_notify_group_changed (ShellOskController *self)
{
  g_signal_emit (self, controller_signals[CONTROLLER_GROUP_CHANGED], 0);
}

static const char *
controller_get_current_group (ShellOskController *self)
{
  ShellOskManager *manager = self->manager;

  if (manager == NULL || manager->current_source_xkb_id == NULL)
    return "us";

  /* Special case for Korean, if Hangul mode is disabled, use the 'us' keymap */
  if (g_strcmp0 (manager->current_source_id, "hangul") == 0 &&
      manager->current_source_properties != NULL)
    {
      IBusProperty *prop;
      guint i;

      for (i = 0;
           (prop = ibus_prop_list_get (manager->current_source_properties, i)) != NULL;
           ++i)
        {
          if (g_strcmp0 (ibus_property_get_key (prop), "InputMode") == 0 &&
              ibus_property_get_prop_type (prop) == PROP_TYPE_TOGGLE &&
              ibus_property_get_state (prop) != PROP_STATE_CHECKED)
            return "us";
        }
    }

  return manager->current_source_xkb_id;
}

static void
controller_keyval_press (ShellOskController *self,
                         guint               keyval)
{
  if (self->virtual_device == NULL)
    return;

  clutter_virtual_input_device_notify_keyval (self->virtual_device,
                                              (uint64_t) clutter_get_current_event_time () * 1000,
                                              keyval, CLUTTER_KEY_STATE_PRESSED);
}

static void
controller_keyval_release (ShellOskController *self,
                           guint               keyval)
{
  if (self->virtual_device == NULL)
    return;

  clutter_virtual_input_device_notify_keyval (self->virtual_device,
                                              (uint64_t) clutter_get_current_event_time () * 1000,
                                              keyval, CLUTTER_KEY_STATE_RELEASED);
}

static void
controller_forward_modifiers (ShellOskController *self,
                              GArray             *modifiers,
                              gboolean            press)
{
  guint i;

  for (i = 0; i < modifiers->len; i++)
    {
      guint keyval = g_array_index (modifiers, guint, i);

      if (press)
        controller_keyval_press (self, keyval);
      else
        controller_keyval_release (self, keyval);
    }
}

static GArray *
controller_get_keyvals_from_string (const char *string)
{
  GArray *keyvals = g_array_new (FALSE, FALSE, sizeof (guint));
  const char *p;

  for (p = string; *p != '\0'; p = g_utf8_next_char (p))
    {
      guint keyval = clutter_unicode_to_keysym (g_utf8_get_char (p));

      g_array_append_val (keyvals, keyval);
    }

  return keyvals;
}

typedef void (* CommitDoneFunc) (gpointer user_data);

typedef struct
{
  ShellOskController *controller;   /* strong */
  GArray *keyvals;
  guint index;
  CommitDoneFunc done;
  gpointer done_data;
  GDestroyNotify done_destroy;
} CommitOp;

static void
commit_op_finish (CommitOp *op)
{
  if (op->done != NULL)
    op->done (op->done_data);

  if (op->done_destroy != NULL)
    op->done_destroy (op->done_data);

  g_clear_pointer (&op->keyvals, g_array_unref);
  g_clear_object (&op->controller);
  g_free (op);
}

static void commit_op_next (CommitOp *op);

static void
commit_op_key_done_cb (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  CommitOp *op = user_data;
  guint keyval = g_array_index (op->keyvals, guint, op->index);

  if (!shell_input_method_handle_virtual_key_finish (SHELL_INPUT_METHOD (source), res))
    {
      controller_keyval_press (op->controller, keyval);
      controller_keyval_release (op->controller, keyval);
    }

  op->index++;
  commit_op_next (op);
}

static void
commit_op_next (CommitOp *op)
{
  ShellInputMethod *input_method = op->controller->input_method;

  if (input_method == NULL || op->index >= op->keyvals->len)
    {
      commit_op_finish (op);
      return;
    }

  shell_input_method_handle_virtual_key_async (input_method,
                                               g_array_index (op->keyvals, guint, op->index),
                                               NULL,
                                               commit_op_key_done_cb, op);
}

/* Sends @str to the focus. @done is called once it is sent. @modifiers is the
 * ordered set of keyvals of the modifier keys which are on, or %NULL. */
static void
controller_commit (ShellOskController *self,
                   const char         *str,
                   GArray             *modifiers,
                   CommitDoneFunc      done,
                   gpointer            done_data,
                   GDestroyNotify      done_destroy)
{
  g_autoptr (GArray) keyvals = controller_get_keyvals_from_string (str);
  gboolean has_modifiers = modifiers != NULL && modifiers->len > 0;
  ShellInputMethod *input_method = self->input_method;
  gboolean ibus_source = self->manager != NULL && self->manager->current_source_is_ibus;
  CommitOp *op;
  guint i;

  if (input_method == NULL)
    {
      if (done_destroy != NULL)
        done_destroy (done_data);
      return;
    }

  /* If there is no IM focus (e.g. with X11 clients), or modifiers
   * are in use, send raw key events.
   */
  if (shell_input_method_get_current_focus (input_method) == NULL || has_modifiers)
    {
      if (has_modifiers)
        controller_forward_modifiers (self, modifiers, TRUE);

      for (i = 0; i < keyvals->len; i++)
        {
          guint keyval = g_array_index (keyvals, guint, i);

          controller_keyval_press (self, keyval);
          controller_keyval_release (self, keyval);
        }

      if (has_modifiers)
        controller_forward_modifiers (self, modifiers, FALSE);

      if (done != NULL)
        done (done_data);
      if (done_destroy != NULL)
        done_destroy (done_data);
      return;
    }

  /* If OSK completion is enabled, or there is an active source requiring
   * IBus to receive input, prefer to feed the events directly to the IM
   */
  if (self->osk_completion_enabled > 0 || ibus_source)
    {
      op = g_new0 (CommitOp, 1);
      op->controller = g_object_ref (self);
      op->keyvals = g_steal_pointer (&keyvals);
      op->done = done;
      op->done_data = done_data;
      op->done_destroy = done_destroy;
      commit_op_next (op);
      return;
    }

  clutter_input_method_commit (CLUTTER_INPUT_METHOD (input_method), str);

  if (done != NULL)
    done (done_data);
  if (done_destroy != NULL)
    done_destroy (done_data);
}

static void
controller_set_osk_completion_done_cb (GObject      *source,
                                       GAsyncResult *res,
                                       gpointer      user_data)
{
  g_autoptr (ShellOskController) self = user_data;
  gboolean enabled = shell_ibus_manager_set_completion_enabled_finish (SHELL_IBUS_MANAGER (source), res);

  if (self->manager != NULL && self->manager->shutting_down)
    return;

  self->osk_completion_enabled = enabled;

  if (self->input_method != NULL)
    shell_input_method_update (self->input_method);
}

static void
controller_set_osk_completion (ShellOskController *self,
                               gboolean            enabled)
{
  if (self->osk_completion_enabled == (int) enabled)
    return;

  if (self->manager != NULL && self->manager->shutting_down)
    return;

  shell_ibus_manager_set_completion_enabled_async (shell_ibus_manager_get_default (),
                                                   enabled, NULL,
                                                   controller_set_osk_completion_done_cb,
                                                   g_object_ref (self));
}

/* The position, in characters, of the start of the whitespace which precedes
 * the last word before @cursor (`/\s+\S+\s*$/`), or 0 if there is none. */
static guint
controller_previous_word_position (const char *text,
                                   guint       cursor)
{
  glong n_chars = g_utf8_strlen (text, -1);
  const char *p = g_utf8_offset_to_pointer (text, MIN ((glong) cursor, n_chars));
  const char *word_end, *ws_end;

  /* Trailing whitespace */
  while (p > text)
    {
      const char *prev = g_utf8_prev_char (p);

      if (!g_unichar_isspace (g_utf8_get_char (prev)))
        break;
      p = prev;
    }

  /* The last word */
  word_end = p;
  while (p > text)
    {
      const char *prev = g_utf8_prev_char (p);

      if (g_unichar_isspace (g_utf8_get_char (prev)))
        break;
      p = prev;
    }

  if (p == word_end)
    return 0;

  /* The whitespace before it */
  ws_end = p;
  while (p > text)
    {
      const char *prev = g_utf8_prev_char (p);

      if (!g_unichar_isspace (g_utf8_get_char (prev)))
        break;
      p = prev;
    }

  if (p == ws_end)
    return 0;

  return g_utf8_pointer_to_offset (text, p);
}

static void
controller_delete_surrounding (ShellOskController *self,
                               const char         *text,
                               guint               cursor,
                               guint               anchor)
{
  int offset;
  guint len;

  if (cursor == 0 && anchor == 0)
    return;

  if (cursor > anchor)
    {
      offset = (int) anchor - (int) cursor;
      len = -offset;
    }
  else if (cursor < anchor)
    {
      offset = 0;
      len = anchor - cursor;
    }
  else if (self->times_deleted < BACKSPACE_WORD_DELETE_THRESHOLD)
    {
      offset = -1;
      len = 1;
    }
  else
    {
      guint word_length = cursor - controller_previous_word_position (text != NULL ? text : "",
                                                                      cursor);

      offset = -(int) word_length;
      len = word_length;
    }

  self->times_deleted++;
  if (self->input_method != NULL)
    clutter_input_method_delete_surrounding (CLUTTER_INPUT_METHOD (self->input_method),
                                             offset, len);
}

typedef struct
{
  GWeakRef controller;
  char *text;
  guint cursor;
} DeleteTimeoutData;

static void
delete_timeout_data_free (DeleteTimeoutData *data)
{
  g_weak_ref_clear (&data->controller);
  g_free (data->text);
  g_free (data);
}

static gboolean
controller_surrounding_update_timeout_cb (gpointer user_data)
{
  DeleteTimeoutData *data = user_data;
  g_autoptr (ShellOskController) self = g_weak_ref_get (&data->controller);

  if (self != NULL)
    {
      self->surrounding_update_timeout_id = 0;
      controller_delete_surrounding (self, data->text, data->cursor, data->cursor);
    }

  return G_SOURCE_REMOVE;
}

static void
controller_surrounding_text_set_cb (ShellInputMethod   *input_method,
                                    ShellOskController *self)
{
  char *surrounding;
  guint cursor, anchor;

  shell_input_method_get_surrounding_text (input_method, &surrounding, &cursor, &anchor);

  if (self->times_deleted == 0)
    {
      controller_delete_surrounding (self, surrounding, cursor, anchor);
    }
  else
    {
      DeleteTimeoutData *data = g_new0 (DeleteTimeoutData, 1);

      g_clear_handle_id (&self->surrounding_update_timeout_id, g_source_remove);

      g_weak_ref_init (&data->controller, self);
      data->text = g_strdup (surrounding);
      data->cursor = cursor;
      self->surrounding_update_timeout_id =
        g_timeout_add_full (G_PRIORITY_DEFAULT, KEY_RELEASE_TIMEOUT,
                            controller_surrounding_update_timeout_cb, data,
                            (GDestroyNotify) delete_timeout_data_free);
    }
}

static void
controller_toggle_delete (ShellOskController *self,
                          gboolean            enabled)
{
  ShellInputMethod *input_method = self->input_method;

  if (input_method == NULL || self->delete_enabled == (int) enabled)
    return;

  self->delete_enabled = enabled;
  self->times_deleted = 0;

  /* If there is no IM focus or are in the middle of preedit, fallback to
   * keypresses */
  if (enabled &&
      (shell_input_method_get_current_focus (input_method) == NULL ||
       shell_input_method_has_preedit (input_method) ||
       self->purpose == CLUTTER_INPUT_CONTENT_PURPOSE_TERMINAL))
    {
      controller_keyval_press (self, CLUTTER_KEY_BackSpace);
      self->backspace_pressed = TRUE;
      return;
    }

  if (!enabled && self->backspace_pressed)
    {
      controller_keyval_release (self, CLUTTER_KEY_BackSpace);
      self->backspace_pressed = FALSE;
      return;
    }

  if (enabled)
    {
      char *text;
      guint cursor, anchor;

      self->surrounding_update_id =
        g_signal_connect_object (input_method, "surrounding-text-set",
                                 G_CALLBACK (controller_surrounding_text_set_cb), self, 0);

      shell_input_method_get_surrounding_text (input_method, &text, &cursor, &anchor);
      if (text != NULL && *text != '\0')
        controller_delete_surrounding (self, text, cursor, anchor);
      else
        clutter_input_method_request_surrounding (CLUTTER_INPUT_METHOD (input_method));
    }
  else
    {
      if (self->surrounding_update_id > 0)
        {
          g_signal_handler_disconnect (input_method, self->surrounding_update_id);
          self->surrounding_update_id = 0;
        }

      g_clear_handle_id (&self->surrounding_update_timeout_id, g_source_remove);
    }
}

static void
controller_purpose_notify_cb (GObject            *object,
                              GParamSpec         *pspec,
                              ShellOskController *self)
{
  ClutterInputContentPurpose purpose;

  g_object_get (object, "content-purpose", &purpose, NULL);
  self->purpose = purpose;
  g_signal_emit (self, controller_signals[CONTROLLER_PURPOSE_CHANGED], 0, purpose);
}

static void
controller_hints_notify_cb (GObject            *object,
                            GParamSpec         *pspec,
                            ShellOskController *self)
{
  ClutterInputContentHintFlags hints;

  g_object_get (object, "content-hints", &hints, NULL);
  g_signal_emit (self, controller_signals[CONTROLLER_CONTENT_HINTS_CHANGED], 0, hints);
}

static void
controller_panel_state_cb (ClutterInputMethod     *input_method,
                           ClutterInputPanelState  state,
                           ShellOskController     *self)
{
  g_signal_emit (self, controller_signals[CONTROLLER_PANEL_STATE], 0, state);
}

static void
controller_destroy (ShellOskController *self)
{
  if (self->input_method != NULL)
    g_signal_handlers_disconnect_by_data (self->input_method, self);
  self->surrounding_update_id = 0;

  g_clear_handle_id (&self->surrounding_update_timeout_id, g_source_remove);

  /* Make sure any buttons pressed by the virtual device are released
   * immediately instead of waiting for the next GC cycle */
  g_clear_object (&self->virtual_device);
}

static void
controller_dispose (GObject *object)
{
  ShellOskController *self = SHELL_OSK_CONTROLLER (object);

  controller_destroy (self);
  g_clear_weak_pointer (&self->input_method);
  g_clear_weak_pointer (&self->manager);

  G_OBJECT_CLASS (shell_osk_controller_parent_class)->dispose (object);
}

static void
shell_osk_controller_class_init (ShellOskControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = controller_dispose;

  controller_signals[CONTROLLER_GROUP_CHANGED] =
    g_signal_new ("group-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
  controller_signals[CONTROLLER_PANEL_STATE] =
    g_signal_new ("panel-state",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  CLUTTER_TYPE_INPUT_PANEL_STATE);
  controller_signals[CONTROLLER_PURPOSE_CHANGED] =
    g_signal_new ("purpose-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  CLUTTER_TYPE_INPUT_CONTENT_PURPOSE);
  controller_signals[CONTROLLER_CONTENT_HINTS_CHANGED] =
    g_signal_new ("content-hints-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  CLUTTER_TYPE_INPUT_CONTENT_HINT_FLAGS);
}

static void
shell_osk_controller_init (ShellOskController *self)
{
  self->osk_completion_enabled = -1;
  self->delete_enabled = -1;
}

static ShellOskController *
controller_new (ShellOskManager *manager)
{
  ShellOskController *self = g_object_new (SHELL_TYPE_OSK_CONTROLLER, NULL);
  ClutterBackend *backend;
  ClutterSeat *seat;

  g_set_weak_pointer (&self->manager, manager);
  g_set_weak_pointer (&self->input_method, manager->input_method);

  backend = clutter_context_get_backend (clutter_actor_get_context (CLUTTER_ACTOR (manager->stage)));
  seat = clutter_backend_get_default_seat (backend);
  self->virtual_device = clutter_seat_create_virtual_device (seat, CLUTTER_KEYBOARD_DEVICE);

  g_object_get (manager->input_method, "content-purpose", &self->purpose, NULL);

  g_signal_connect_object (manager->input_method, "notify::content-purpose",
                           G_CALLBACK (controller_purpose_notify_cb), self, 0);
  g_signal_connect_object (manager->input_method, "notify::content-hints",
                           G_CALLBACK (controller_hints_notify_cb), self, 0);
  g_signal_connect_object (manager->input_method, "input-panel-state",
                           G_CALLBACK (controller_panel_state_cb), self, 0);

  return self;
}


/* ------------------------------------------------------------------------ */
/* ShellOsk                                                                  */
/* ------------------------------------------------------------------------ */

enum
{
  OSK_PROP_0,
  OSK_PROP_MANAGER,
  OSK_N_PROPS
};

static GParamSpec *osk_props[OSK_N_PROPS] = { NULL, };

enum
{
  OSK_VISIBILITY_CHANGED,
  OSK_LAST_SIGNAL
};

static guint osk_signals[OSK_LAST_SIGNAL] = { 0, };

struct _ShellOsk
{
  StBoxLayout parent;

  ShellOskManager *manager;           /* weak */
  ShellLayoutManager *layout_manager; /* weak */
  ClutterStage *stage;                /* weak */
  MetaCompositor *compositor;         /* weak */
  ShellInputMethod *input_method;     /* weak */

  gboolean torn_down;

  gboolean focus_in_extended_keys;
  gboolean emoji_active;
  gboolean emoji_visible;
  ClutterInputContentPurpose purpose;
  ClutterInputContentHintFlags content_hints;

  ShellOskNoGrabPopup *language_popup;
  MetaWindow *focus_window;           /* weak */
  double focus_window_start_y;

  gboolean latched;                   /* current level is latched */
  gboolean long_pressed;
  GArray *modifiers;                  /* the keyvals of the modifiers which are on */
  GHashTable *modifier_keys;          /* keyval -> GPtrArray of ShellOskKey */

  ShellOskSuggestions *suggestions;   /* weak, a child */
  ShellOskAspectContainer *aspect_container; /* weak, a child */
  ShellOskEmojiSelection *emoji_selection;   /* weak, a child */
  ClutterActor *current_layout;       /* weak, a child of aspect_container */
  GHashTable *layers;                 /* level -> ShellOskKeyContainer */
  ClutterActor *current_page;         /* one of layers, %NULL when destroyed */
  gulong current_page_destroy_id;

  ShellOskFocusTracker *focus_tracker;
  gulong window_moved_id;
  gulong surrounding_text_id;

  guint show_idle_id;

  gboolean keyboard_visible;
  gboolean keyboard_requested;
  guint keyboard_resting_id;
  gulong keyboard_height_notify_id;
  gboolean gesture_in_progress;

  ShellOskController *controller;
};

G_DEFINE_FINAL_TYPE (ShellOsk, shell_osk, ST_TYPE_BOX_LAYOUT)

static void osk_manager_request_activate_input_source (ShellOskManager *manager,
                                                       int              position);

static void osk_set_cursor_location (ShellOsk   *self,
                                     MetaWindow *window,
                                     double      x,
                                     double      y,
                                     double      w,
                                     double      h);

static void osk_open  (ShellOsk *self,
                       gboolean  immediate);
static void osk_close (ShellOsk *self,
                       gboolean  immediate);

typedef enum
{
  KEY_ACTION_UNKNOWN,
  KEY_ACTION_HIDE,
  KEY_ACTION_LANGUAGE_MENU,
  KEY_ACTION_EMOJI,
  KEY_ACTION_MODIFIER,
  KEY_ACTION_DELETE,
  KEY_ACTION_LEVEL_SWITCH,
} KeyActionType;

typedef struct
{
  KeyActionType action;
  char *level;
  guint keyval;
} KeySpec;

static void
key_spec_free (KeySpec *spec)
{
  g_free (spec->level);
  g_free (spec);
}

static KeyActionType
key_action_from_string (const char *action)
{
  if (g_strcmp0 (action, "hide") == 0)
    return KEY_ACTION_HIDE;
  if (g_strcmp0 (action, "languageMenu") == 0)
    return KEY_ACTION_LANGUAGE_MENU;
  if (g_strcmp0 (action, "emoji") == 0)
    return KEY_ACTION_EMOJI;
  if (g_strcmp0 (action, "modifier") == 0)
    return KEY_ACTION_MODIFIER;
  if (g_strcmp0 (action, "delete") == 0)
    return KEY_ACTION_DELETE;
  if (g_strcmp0 (action, "levelSwitch") == 0)
    return KEY_ACTION_LEVEL_SWITCH;

  return KEY_ACTION_UNKNOWN;
}

/* `Keyboard.visible`: the keyboard is shown and its actor is visible */
static gboolean
osk_is_visible (ShellOsk *self)
{
  return self->keyboard_visible && clutter_actor_is_visible (CLUTTER_ACTOR (self));
}

static StBoxLayout *
osk_get_keyboard_box (ShellOsk *self)
{
  if (self->layout_manager == NULL)
    return NULL;

  return shell_layout_manager_get_keyboard_box (self->layout_manager);
}

static gboolean
osk_should_show_emoji (ShellOsk *self)
{
  if ((self->content_hints & CLUTTER_INPUT_CONTENT_HINT_NO_EMOJI) != 0)
    return FALSE;

  return self->purpose == CLUTTER_INPUT_CONTENT_PURPOSE_NORMAL ||
         self->purpose == CLUTTER_INPUT_CONTENT_PURPOSE_ALPHA ||
         self->purpose == CLUTTER_INPUT_CONTENT_PURPOSE_PASSWORD ||
         self->purpose == CLUTTER_INPUT_CONTENT_PURPOSE_TERMINAL;
}

/* ---- levels and modifiers -------------------------------------------- */

static void
osk_set_current_level_latched (ShellOsk     *self,
                               ClutterActor *layout,
                               gboolean      latched)
{
  ShellOskKeyContainer *container;
  guint i;

  if (layout == NULL)
    return;

  container = SHELL_OSK_KEY_CONTAINER (layout);
  for (i = 0; i < container->shift_keys->len; i++)
    {
      ShellOskKey *key = g_ptr_array_index (container->shift_keys, i);

      osk_key_set_latched (key, latched);
      osk_key_set_icon_name (key, latched ? "osk-caps-lock-symbolic" : "osk-shift-symbolic");
    }
}

static void
osk_set_latched (ShellOsk *self,
                 gboolean  latched)
{
  self->latched = latched;
  osk_set_current_level_latched (self, self->current_page, self->latched);
}

static int
osk_find_modifier (ShellOsk *self,
                   guint     keyval)
{
  guint i;

  for (i = 0; i < self->modifiers->len; i++)
    {
      if (g_array_index (self->modifiers, guint, i) == keyval)
        return i;
    }

  return -1;
}

static void
osk_set_modifier_enabled (ShellOsk *self,
                          guint     keyval,
                          gboolean  enabled)
{
  GPtrArray *keys;
  int index = osk_find_modifier (self, keyval);
  guint i;

  if (enabled && index < 0)
    g_array_append_val (self->modifiers, keyval);
  else if (!enabled && index >= 0)
    g_array_remove_index (self->modifiers, index);

  keys = g_hash_table_lookup (self->modifier_keys, GUINT_TO_POINTER (keyval));
  if (keys == NULL)
    return;

  for (i = 0; i < keys->len; i++)
    osk_key_set_latched (g_ptr_array_index (keys, i), enabled);
}

static void
osk_toggle_modifier (ShellOsk *self,
                     guint     keyval)
{
  osk_set_modifier_enabled (self, keyval, osk_find_modifier (self, keyval) < 0);
}

static void
osk_disable_all_modifiers (ShellOsk *self)
{
  g_autoptr (GArray) modifiers = g_array_copy (self->modifiers);
  guint i;

  for (i = 0; i < modifiers->len; i++)
    osk_set_modifier_enabled (self, g_array_index (modifiers, guint, i), FALSE);
}

static void
osk_update_current_page_visible (ShellOsk *self)
{
  if (self->current_page != NULL)
    actor_set_visible (self->current_page, !self->emoji_active);
}

static void
osk_set_emoji_active (ShellOsk *self,
                      gboolean  active)
{
  self->emoji_active = active;
  actor_set_visible (CLUTTER_ACTOR (self->emoji_selection), self->emoji_active);
  osk_update_current_page_visible (self);
}

static void
osk_toggle_emoji (ShellOsk *self)
{
  osk_set_emoji_active (self, !self->emoji_active);
}

static void
osk_current_page_destroy_cb (ClutterActor *page,
                             ShellOsk     *self)
{
  self->current_page = NULL;
  self->current_page_destroy_id = 0;
}

static void
osk_set_active_level (ShellOsk   *self,
                      const char *active_level)
{
  ClutterActor *current_page;
  double n_cols, n_rows;

  if (self->layers == NULL || active_level == NULL ||
      self->aspect_container == NULL || self->emoji_selection == NULL)
    return;

  current_page = g_hash_table_lookup (self->layers, active_level);
  if (current_page == NULL)
    return;

  if (self->current_page == current_page)
    {
      osk_update_current_page_visible (self);
      return;
    }

  if (self->current_page != NULL)
    {
      osk_set_current_level_latched (self, self->current_page, FALSE);
      g_clear_signal_handler (&self->current_page_destroy_id, self->current_page);
      clutter_actor_hide (self->current_page);
    }

  osk_disable_all_modifiers (self);
  self->current_page = current_page;
  self->current_page_destroy_id = g_signal_connect (current_page, "destroy",
                                                    G_CALLBACK (osk_current_page_destroy_cb), self);
  osk_update_current_page_visible (self);

  key_container_get_ratio (SHELL_OSK_KEY_CONTAINER (current_page), &n_cols, &n_rows);
  aspect_container_set_ratio (self->aspect_container, n_cols, n_rows);
  emoji_selection_set_ratio (self->emoji_selection, n_cols, n_rows);
}

static void
osk_surrounding_text_set_cb (ShellInputMethod *input_method,
                             ShellOsk         *self)
{
  char *text;
  guint cursor, anchor;
  glong n_chars;

  shell_input_method_get_surrounding_text (input_method, &text, &cursor, &anchor);

  if (text == NULL || *text == '\0' || cursor == 0)
    {
      /* First character in the buffer */
      osk_set_active_level (self, "shift");
      return;
    }

  n_chars = g_utf8_strlen (text, -1);

  if ((self->content_hints & CLUTTER_INPUT_CONTENT_HINT_TITLECASE) != 0)
    {
      const char *end = g_utf8_offset_to_pointer (text, MIN ((glong) cursor, n_chars));

      if (end > text && g_utf8_get_char (g_utf8_prev_char (end)) == ' ')
        osk_set_active_level (self, "shift");
      else
        osk_set_active_level (self, "default");
    }
  else if ((self->content_hints & CLUTTER_INPUT_CONTENT_HINT_AUTO_CAPITALIZATION) != 0)
    {
      const char *end = g_utf8_offset_to_pointer (text, MIN ((glong) cursor, n_chars));

      while (end > text && g_unichar_isspace (g_utf8_get_char (g_utf8_prev_char (end))))
        end = g_utf8_prev_char (end);

      if (end > text && g_utf8_get_char (g_utf8_prev_char (end)) == '.')
        osk_set_active_level (self, "shift");
      else
        osk_set_active_level (self, "default");
    }

  g_clear_signal_handler (&self->surrounding_text_id, input_method);
}

static void
osk_update_level_from_hints (ShellOsk *self,
                             gboolean  user_input_happened)
{
  /* If the latch is enabled, avoid level changes */
  if (self->latched)
    return;

  if ((self->content_hints & CLUTTER_INPUT_CONTENT_HINT_LOWERCASE) != 0)
    {
      osk_set_active_level (self, "default");
      return;
    }

  if (self->layers == NULL || !g_hash_table_contains (self->layers, "shift"))
    return;

  if ((self->content_hints & CLUTTER_INPUT_CONTENT_HINT_UPPERCASE) != 0)
    {
      osk_set_active_level (self, "shift");
      return;
    }

  if ((self->content_hints &
       (CLUTTER_INPUT_CONTENT_HINT_AUTO_CAPITALIZATION |
        CLUTTER_INPUT_CONTENT_HINT_TITLECASE)) != 0)
    {
      if (self->surrounding_text_id != 0 || self->input_method == NULL)
        return;

      self->surrounding_text_id =
        g_signal_connect_object (self->input_method, "surrounding-text-set",
                                 G_CALLBACK (osk_surrounding_text_set_cb), self, 0);
      clutter_input_method_request_surrounding (CLUTTER_INPUT_METHOD (self->input_method));
      return;
    }

  if (user_input_happened &&
      self->current_page == g_hash_table_lookup (self->layers, "shift"))
    osk_set_active_level (self, "default");
}

/* ---- language menu --------------------------------------------------- */

typedef struct
{
  GWeakRef manager;
  int position;
} LanguageItemData;

static void
language_item_data_free (LanguageItemData *data)
{
  g_weak_ref_clear (&data->manager);
  g_free (data);
}

static void
language_item_activate_cb (const ClutterEvent *event,
                           gpointer            user_data)
{
  LanguageItemData *data = user_data;
  g_autoptr (ShellOskManager) manager = g_weak_ref_get (&data->manager);

  if (manager != NULL)
    osk_manager_request_activate_input_source (manager, data->position);
}

static void
osk_popup_language_menu (ShellOsk     *self,
                         ClutterActor *key_actor)
{
  ShellPopupMenuBase *base;
  ShellPopupBaseMenuItem *item;
  ShellLayoutManager *layout_manager;
  ShellOskManager *manager = self->manager;
  int i;

  if (self->language_popup != NULL)
    {
      shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (self->language_popup));
      g_clear_object (&self->language_popup);
    }

  self->language_popup = no_grab_popup_new (key_actor, ST_SIDE_BOTTOM);
  base = SHELL_POPUP_MENU_BASE (self->language_popup);

  for (i = 0; manager != NULL && manager->source_names != NULL && manager->source_names[i] != NULL; i++)
    {
      LanguageItemData *data = g_new0 (LanguageItemData, 1);

      g_weak_ref_init (&data->manager, manager);
      data->position = i;

      item = shell_popup_menu_base_add_action (base, manager->source_names[i],
                                               language_item_activate_cb,
                                               data,
                                               (GDestroyNotify) language_item_data_free);
      st_widget_set_can_focus (ST_WIDGET (item), FALSE);
      shell_popup_base_menu_item_set_ornament (item,
                                               i == manager->current_source_index
                                               ? SHELL_ORNAMENT_DOT
                                               : SHELL_ORNAMENT_NO_DOT);
    }

  shell_popup_menu_base_add_menu_item (base,
                                       g_object_new (SHELL_TYPE_POPUP_SEPARATOR_MENU_ITEM,
                                                     NULL));
  item = shell_popup_menu_base_add_settings_action (base, _("Keyboard Settings"),
                                                    "gnome-keyboard-panel.desktop");
  st_widget_set_can_focus (ST_WIDGET (item), FALSE);

  layout_manager = self->layout_manager;
  if (layout_manager != NULL)
    shell_layout_manager_add_top_chrome (layout_manager,
                                         shell_popup_menu_base_get_actor (base),
                                         SHELL_CHROME_FLAGS_NONE);
  shell_popup_menu_base_open (base, SHELL_POPUP_ANIMATION_FULL);
}

/* ---- the keys of a layout -------------------------------------------- */

static void
osk_action_key_released_cb (ShellOskKey *key,
                            ShellOsk    *self)
{
  KeySpec *spec = g_object_get_data (G_OBJECT (key), KEY_SPEC_DATA);

  if (spec == NULL)
    return;

  switch (spec->action)
    {
    case KEY_ACTION_HIDE:
      osk_close (self, TRUE);
      osk_update_level_from_hints (self, TRUE);
      break;

    case KEY_ACTION_LANGUAGE_MENU:
      osk_popup_language_menu (self, CLUTTER_ACTOR (key));
      break;

    case KEY_ACTION_EMOJI:
      osk_toggle_emoji (self);
      break;

    case KEY_ACTION_MODIFIER:
      osk_toggle_modifier (self, spec->keyval);
      break;

    case KEY_ACTION_DELETE:
      controller_toggle_delete (self->controller, TRUE);
      controller_toggle_delete (self->controller, FALSE);
      osk_update_level_from_hints (self, TRUE);
      break;

    case KEY_ACTION_LEVEL_SWITCH:
      if (!self->long_pressed)
        {
          osk_set_active_level (self, spec->level);
          /* The JS passed `key.level === 1 && ...`, but levels are strings */
          osk_set_latched (self, FALSE);
        }
      break;

    case KEY_ACTION_UNKNOWN:
    default:
      break;
    }

  self->long_pressed = FALSE;
}

static void
osk_keyval_key_cb (ShellOskKey *key,
                   guint        keyval,
                   ShellOsk    *self)
{
  controller_keyval_press (self->controller, keyval);
  controller_keyval_release (self->controller, keyval);
  osk_update_level_from_hints (self, TRUE);
}

static void
osk_commit_done_cb (gpointer user_data)
{
  g_autoptr (ShellOsk) self = g_weak_ref_get (user_data);

  if (self == NULL)
    return;

  osk_disable_all_modifiers (self);
  osk_update_level_from_hints (self, TRUE);
}

static void
osk_commit_key_cb (ShellOskKey *key,
                   const char  *str,
                   ShellOsk    *self)
{
  controller_commit (self->controller, str, self->modifiers,
                     osk_commit_done_cb, weak_ref_new (self),
                     (GDestroyNotify) weak_ref_free);
}

static void
osk_shift_long_press_cb (ShellOskKey *key,
                         ShellOsk    *self)
{
  KeySpec *spec = g_object_get_data (G_OBJECT (key), KEY_SPEC_DATA);

  if (spec == NULL)
    return;

  osk_set_active_level (self, spec->level);
  osk_set_latched (self, TRUE);
  self->long_pressed = TRUE;
}

static void
osk_delete_long_press_cb (ShellOskKey *key,
                          ShellOsk    *self)
{
  controller_toggle_delete (self->controller, TRUE);
}

static void
osk_add_row_keys (ShellOsk             *self,
                  JsonArray            *keys,
                  ShellOskKeyContainer *layout,
                  gboolean              emoji_visible)
{
  double accumulated_width = 0;
  guint i, n_keys = json_array_get_length (keys);

  for (i = 0; i < n_keys; ++i)
    {
      JsonObject *key = json_array_peek_object (keys, i);
      JsonArray *strings;
      g_autoptr (GStrvBuilder) extended_keys = g_strv_builder_new ();
      g_auto (GStrv) extended = NULL;
      const char *commit_string = NULL;
      const char *action, *label, *icon_name, *keyval_string, *level;
      double width, height, left_offset;
      gboolean has_action, has_keyval;
      KeyActionType action_type;
      ShellOskKey *button;
      KeySpec *spec;
      guint keyval;

      if (key == NULL)
        continue;

      strings = json_object_peek_array (key, "strings");
      if (strings != NULL && json_array_get_length (strings) > 0)
        {
          guint j;

          for (j = 0; j < json_array_get_length (strings); j++)
            {
              JsonNode *node = json_array_get_element (strings, j);
              const char *string = JSON_NODE_HOLDS_VALUE (node) ? json_node_get_string (node) : NULL;

              if (string == NULL)
                string = "";

              if (j == 0)
                commit_string = string;
              else
                g_strv_builder_add (extended_keys, string);
            }
        }
      extended = g_strv_builder_end (extended_keys);

      action = json_object_peek_string (key, "action");
      width = json_object_peek_double (key, "width", 1);
      height = json_object_peek_double (key, "height", 1);
      left_offset = json_object_peek_double (key, "leftOffset", 0);

      if (g_strcmp0 (action, "emoji") == 0 && !emoji_visible)
        {
          accumulated_width = width;
          continue;
        }

      if (accumulated_width > 0)
        {
          /* Pass accumulated width onto the next key */
          width += accumulated_width;
          accumulated_width = 0;
        }

      label = json_object_peek_string (key, "label");
      icon_name = json_object_peek_string (key, "iconName");
      keyval_string = json_object_peek_string (key, "keyval");
      level = json_object_peek_string (key, "level");
      has_action = action != NULL && *action != '\0';
      has_keyval = keyval_string != NULL && *keyval_string != '\0';
      keyval = has_keyval ? g_ascii_strtoull (keyval_string, NULL, 16) : 0;
      action_type = has_action ? key_action_from_string (action) : KEY_ACTION_UNKNOWN;

      button = osk_key_new (commit_string, label, icon_name, keyval, has_action,
                            (const char * const *) extended);

      spec = g_new0 (KeySpec, 1);
      spec->action = action_type;
      spec->level = g_strdup (level);
      spec->keyval = keyval;
      g_object_set_data_full (G_OBJECT (button), KEY_SPEC_DATA, spec,
                              (GDestroyNotify) key_spec_free);

      if (has_action)
        {
          g_signal_connect_object (button, "released",
                                   G_CALLBACK (osk_action_key_released_cb), self, 0);
        }
      else if (has_keyval)
        {
          g_signal_connect_object (button, "keyval",
                                   G_CALLBACK (osk_keyval_key_cb), self, 0);
        }
      else
        {
          g_signal_connect_object (button, "commit",
                                   G_CALLBACK (osk_commit_key_cb), self, 0);
        }

      if (action_type == KEY_ACTION_LEVEL_SWITCH &&
          g_strcmp0 (icon_name, "osk-shift-symbolic") == 0)
        {
          g_ptr_array_add (layout->shift_keys, g_object_ref (button));

          if (g_strcmp0 (level, "shift") == 0)
            {
              g_signal_connect_object (button, "long-press",
                                       G_CALLBACK (osk_shift_long_press_cb), self, 0);
            }
        }

      if (action_type == KEY_ACTION_DELETE)
        {
          g_signal_connect_object (button, "long-press",
                                   G_CALLBACK (osk_delete_long_press_cb), self, 0);
        }

      if (action_type == KEY_ACTION_MODIFIER)
        {
          GPtrArray *modifier_keys = g_hash_table_lookup (self->modifier_keys,
                                                          GUINT_TO_POINTER (keyval));

          if (modifier_keys == NULL)
            {
              modifier_keys = g_ptr_array_new_with_free_func (g_object_unref);
              g_hash_table_insert (self->modifier_keys, GUINT_TO_POINTER (keyval), modifier_keys);
            }

          g_ptr_array_add (modifier_keys, g_object_ref (button));
        }

      if (has_action || has_keyval)
        st_widget_add_style_class_name (ST_WIDGET (osk_key_get_key_button (button)), "default-key");

      key_container_append_key (layout, CLUTTER_ACTOR (button), width, height, left_offset);
    }
}

/* ---- the layout ------------------------------------------------------ */

static void
osk_update_layout (ShellOsk                   *self,
                   const char                 *group_name,
                   ClutterInputContentPurpose  purpose)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GHashTable) layers = NULL;
  JsonObject *model;
  JsonArray *levels;
  ClutterActor *layout;
  guint i;

  switch (purpose)
    {
    case CLUTTER_INPUT_CONTENT_PURPOSE_DIGITS:
      parser = load_keyboard_model ("digits");
      break;

    case CLUTTER_INPUT_CONTENT_PURPOSE_NUMBER:
      parser = load_keyboard_model ("number");
      break;

    case CLUTTER_INPUT_CONTENT_PURPOSE_PHONE:
      parser = load_keyboard_model ("phone");
      break;

    case CLUTTER_INPUT_CONTENT_PURPOSE_EMAIL:
      parser = load_keyboard_model ("email");
      break;

    case CLUTTER_INPUT_CONTENT_PURPOSE_URL:
      parser = load_keyboard_model ("url");
      break;

    default:
      {
        g_autoptr (GPtrArray) groups = g_ptr_array_new_with_free_func (g_free);
        const char *plus = strchr (group_name, '+');

        g_ptr_array_add (groups, g_strdup (group_name));
        if (plus != NULL)
          g_ptr_array_add (groups, g_strndup (group_name, plus - group_name));
        g_ptr_array_add (groups, g_strdup ("us"));

        for (i = 0; i < groups->len; i++)
          {
            g_autofree char *group = NULL;

            if (purpose == CLUTTER_INPUT_CONTENT_PURPOSE_TERMINAL)
              group = g_strdup_printf ("%s-extended", (const char *) g_ptr_array_index (groups, i));
            else
              group = g_strdup (g_ptr_array_index (groups, i));

            /* Ignore the error and fall back to the next model */
            parser = load_keyboard_model (group);
            if (parser != NULL)
              break;
          }
      }
      break;
    }

  if (parser == NULL)
    return;

  model = json_node_get_object (json_parser_get_root (parser));
  levels = json_object_peek_array (model, "levels");

  self->emoji_visible = osk_should_show_emoji (self);
  g_array_set_size (self->modifiers, 0);
  g_hash_table_remove_all (self->modifier_keys);

  layout = new_actor_with_layout (clutter_bin_layout_new ());
  clutter_actor_set_x_expand (layout, TRUE);
  clutter_actor_set_y_expand (layout, TRUE);

  layers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  for (i = 0; levels != NULL && i < json_array_get_length (levels); i++)
    {
      JsonObject *current_level = json_array_peek_object (levels, i);
      ShellOskKeyContainer *level_layout;
      const char *level_name;
      JsonArray *rows;
      guint j;

      if (current_level == NULL)
        continue;

      level_layout = key_container_new ();

      rows = json_object_peek_array (current_level, "rows");
      for (j = 0; rows != NULL && j < json_array_get_length (rows); j++)
        {
          JsonNode *row = json_array_get_element (rows, j);

          key_container_append_row (level_layout);
          if (JSON_NODE_HOLDS_ARRAY (row))
            osk_add_row_keys (self, json_node_get_array (row), level_layout, self->emoji_visible);
        }

      level_name = json_object_peek_string (current_level, "level");
      if (level_name != NULL)
        g_hash_table_replace (layers, g_strdup (level_name), g_object_ref (level_layout));

      clutter_actor_add_child (layout, CLUTTER_ACTOR (level_layout));
      clutter_actor_hide (CLUTTER_ACTOR (level_layout));
    }

  clutter_actor_add_child (CLUTTER_ACTOR (self->aspect_container), layout);
  if (self->current_layout != NULL)
    clutter_actor_destroy (self->current_layout);
  g_set_weak_pointer (&self->current_layout, layout);

  g_clear_pointer (&self->layers, g_hash_table_unref);
  self->layers = g_steal_pointer (&layers);
}

static void
osk_update_keys (ShellOsk *self)
{
  const char *group = controller_get_current_group (self->controller);

  self->purpose = self->controller->purpose;
  osk_update_layout (self, group, self->purpose);
  osk_set_active_level (self, "default");
}

static void
osk_relayout (ShellOsk *self)
{
  g_autoptr (ShellMonitor) monitor = NULL;
  float min_height;
  double height;

  if (self->layout_manager == NULL)
    return;

  monitor = shell_layout_manager_get_keyboard_monitor (self->layout_manager);
  clutter_actor_get_preferred_height (CLUTTER_ACTOR (self), -1, &min_height, NULL);

  if (monitor == NULL)
    return;

  clutter_actor_set_width (CLUTTER_ACTOR (self), monitor->width);

  if (monitor->width > monitor->height)
    height = monitor->height / 3.0;
  else
    height = monitor->height / 4.0;

  /* Math.clamp (height, minHeight, monitor.height / 2) */
  height = MIN (MAX (height, min_height), monitor->height / 2.0);
  clutter_actor_set_height (CLUTTER_ACTOR (self), height);
}

/* ---- showing and hiding ---------------------------------------------- */

static void
osk_clear_show_idle (ShellOsk *self)
{
  g_clear_handle_id (&self->show_idle_id, g_source_remove);
}

static void
osk_clear_keyboard_rest_timer (ShellOsk *self)
{
  g_clear_handle_id (&self->keyboard_resting_id, g_source_remove);
}

static void
osk_emit_hook (const char *name,
               int         monitor)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  GVariantBuilder builder;

  if (!shell_hook_registry_is_declared (registry, name))
    return;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (monitor >= 0)
    g_variant_builder_add (&builder, "{sv}", "monitor", g_variant_new_int32 (monitor));

  shell_hook_registry_emit_after (registry, name, g_variant_builder_end (&builder));
}

/* The focus tracker saw the focus window move: let go of the window and
 * hide the keyboard */
static void
osk_on_focus_window_moving (ShellOsk             *self,
                            ShellOskFocusTracker *tracker)
{
  if (tracker->current_window == self->focus_window)
    {
      /* Don't use osk_set_focus_window () here because that would move the
       * window while the user has grabbed it. Instead we simply "let go"
       * of the window.
       */
      g_clear_weak_pointer (&self->focus_window);
      self->focus_window_start_y = 0;
    }

  osk_close (self, TRUE);
}

typedef struct
{
  MetaWindow *window;             /* strong */
  ClutterActor *window_actor;     /* strong */
  GWeakRef osk;
  double final_y;
  gboolean unmanaged;
  gulong unmanaged_id;
} WindowAnimation;

static void
window_animation_free (WindowAnimation *animation)
{
  if (animation->unmanaged_id != 0)
    g_signal_handler_disconnect (animation->window, animation->unmanaged_id);

  g_clear_object (&animation->window);
  g_clear_object (&animation->window_actor);
  g_weak_ref_clear (&animation->osk);
  g_free (animation);
}

static void
window_animation_unmanaged_cb (MetaWindow      *window,
                               WindowAnimation *animation)
{
  animation->unmanaged = TRUE;
}

static void
osk_window_slide_animation_complete (ShellOsk  *self,
                                     MetaWindow *window,
                                     double      final_y)
{
  MtkRectangle frame_rect, buffer_rect;

  /* Synchronize window positions again. */
  meta_window_get_frame_rect (window, &frame_rect);
  meta_window_get_buffer_rect (window, &buffer_rect);

  final_y += frame_rect.y - buffer_rect.y;

  frame_rect.y = (int) final_y;

  if (self->focus_tracker == NULL)
    {
      meta_window_move_frame (window, TRUE, frame_rect.x, frame_rect.y);
      return;
    }

  if (self->window_moved_id != 0)
    g_signal_handler_disconnect (self->focus_tracker, self->window_moved_id);
  meta_window_move_frame (window, TRUE, frame_rect.x, frame_rect.y);
  self->window_moved_id = g_signal_connect_swapped (self->focus_tracker, "window-moved",
                                                    G_CALLBACK (osk_on_focus_window_moving),
                                                    self);
}


static void
window_animation_done_cb (gboolean finished,
                          gpointer user_data)
{
  WindowAnimation *animation = user_data;
  g_autoptr (ShellOsk) self = g_weak_ref_get (&animation->osk);

  if (animation->unmanaged_id != 0)
    {
      g_signal_handler_disconnect (animation->window, animation->unmanaged_id);
      animation->unmanaged_id = 0;
    }

  if (animation->unmanaged)
    return;

  clutter_actor_set_y (animation->window_actor, animation->final_y);
  if (self != NULL)
    osk_window_slide_animation_complete (self, animation->window, animation->final_y);
}

static void
osk_animate_window (ShellOsk   *self,
                    MetaWindow *window,
                    gboolean    show)
{
  GObject *window_actor = meta_window_get_compositor_private (window);
  StBoxLayout *keyboard_box = osk_get_keyboard_box (self);
  WindowAnimation *animation;
  ShellEaseParams params;
  double final_y;

  if (window_actor == NULL)
    return;

  final_y = show
    ? self->focus_window_start_y - (keyboard_box != NULL
                                    ? clutter_actor_get_height (CLUTTER_ACTOR (keyboard_box))
                                    : 0)
    : self->focus_window_start_y;

  animation = g_new0 (WindowAnimation, 1);
  animation->window = g_object_ref (window);
  animation->window_actor = g_object_ref (CLUTTER_ACTOR (window_actor));
  g_weak_ref_init (&animation->osk, self);
  animation->final_y = final_y;
  animation->unmanaged_id = g_signal_connect (window, "unmanaged",
                                              G_CALLBACK (window_animation_unmanaged_cb),
                                              animation);

  params = make_ease_params (KEYBOARD_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (CLUTTER_ACTOR (window_actor), &params,
                    window_animation_done_cb, animation,
                    (GDestroyNotify) window_animation_free,
                    "y", final_y,
                    NULL);
}

static void
osk_keyboard_box_height_notify_cb (GObject    *keyboard_box,
                                   GParamSpec *pspec,
                                   ShellOsk   *self)
{
  g_object_set (self,
                "translation-y", -clutter_actor_get_height (CLUTTER_ACTOR (self)),
                NULL);
}

static void
osk_animate_show_complete_cb (gboolean finished,
                              gpointer user_data)
{
  g_autoptr (ShellOsk) self = g_weak_ref_get (user_data);
  StBoxLayout *keyboard_box;

  if (!finished || self == NULL)
    return;

  keyboard_box = osk_get_keyboard_box (self);
  if (keyboard_box == NULL)
    return;

  if (self->keyboard_height_notify_id != 0)
    g_signal_handler_disconnect (keyboard_box, self->keyboard_height_notify_id);
  self->keyboard_height_notify_id =
    g_signal_connect_object (keyboard_box, "notify::height",
                             G_CALLBACK (osk_keyboard_box_height_notify_cb), self, 0);
}

static void
osk_animate_show (ShellOsk *self)
{
  StBoxLayout *keyboard_box = osk_get_keyboard_box (self);
  ShellEaseParams params;

  if (self->compositor != NULL)
    meta_compositor_disable_unredirect (self->compositor);

  if (self->focus_window != NULL)
    osk_animate_window (self, self->focus_window, TRUE);

  if (keyboard_box != NULL)
    clutter_actor_show (CLUTTER_ACTOR (keyboard_box));

  params = make_ease_params (KEYBOARD_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);
  shell_actor_ease (CLUTTER_ACTOR (self), &params,
                    osk_animate_show_complete_cb, weak_ref_new (self),
                    (GDestroyNotify) weak_ref_free,
                    "translation-y", (double) -clutter_actor_get_height (CLUTTER_ACTOR (self)),
                    "opacity", 255u,
                    NULL);

  self->keyboard_visible = TRUE;
  g_signal_emit (self, osk_signals[OSK_VISIBILITY_CHANGED], 0);

  if (self->layout_manager != NULL)
    osk_emit_hook ("osk.shown",
                   shell_layout_manager_get_keyboard_index (self->layout_manager) + 1);
}

static void
osk_animate_hide_complete_cb (gboolean finished,
                              gpointer user_data)
{
  g_autoptr (ShellOsk) self = g_weak_ref_get (user_data);
  StBoxLayout *keyboard_box;

  if (!finished || self == NULL)
    return;

  keyboard_box = osk_get_keyboard_box (self);
  if (keyboard_box != NULL)
    clutter_actor_hide (CLUTTER_ACTOR (keyboard_box));

  if (self->compositor != NULL)
    meta_compositor_enable_unredirect (self->compositor);
}

static void
osk_animate_hide (ShellOsk *self)
{
  gboolean was_visible = self->keyboard_visible;
  ShellEaseParams params;

  if (self->focus_window != NULL)
    osk_animate_window (self, self->focus_window, FALSE);

  if (self->keyboard_height_notify_id != 0)
    {
      StBoxLayout *keyboard_box = osk_get_keyboard_box (self);

      if (keyboard_box != NULL)
        g_signal_handler_disconnect (keyboard_box, self->keyboard_height_notify_id);
      self->keyboard_height_notify_id = 0;
    }

  params = make_ease_params (KEYBOARD_ANIMATION_TIME, CLUTTER_EASE_IN_QUAD);
  shell_actor_ease (CLUTTER_ACTOR (self), &params,
                    osk_animate_hide_complete_cb, weak_ref_new (self),
                    (GDestroyNotify) weak_ref_free,
                    "translation-y", (double) 0,
                    "opacity", 0u,
                    NULL);

  self->keyboard_visible = FALSE;
  g_signal_emit (self, osk_signals[OSK_VISIBILITY_CHANGED], 0);

  if (was_visible)
    osk_emit_hook ("osk.hidden", -1);
}

static void
osk_gesture_progress (ShellOsk *self,
                      double    delta)
{
  StBoxLayout *keyboard_box = osk_get_keyboard_box (self);
  float height = clutter_actor_get_height (CLUTTER_ACTOR (self));
  double progress = height > 0 ? MIN (delta, height) / height : 0;
  GObject *window_actor = NULL;

  if (self->torn_down)
    return;

  self->gesture_in_progress = TRUE;
  if (keyboard_box != NULL)
    clutter_actor_show (CLUTTER_ACTOR (keyboard_box));

  g_object_set (self, "translation-y", (float) (-height * progress), NULL);
  clutter_actor_set_opacity (CLUTTER_ACTOR (self), (guint8) CLAMP (255 * progress, 0, 255));

  if (self->focus_window != NULL)
    window_actor = meta_window_get_compositor_private (self->focus_window);
  if (window_actor != NULL)
    clutter_actor_set_y (CLUTTER_ACTOR (window_actor),
                         self->focus_window_start_y - (height * progress));
}

static void
osk_gesture_activate (ShellOsk *self)
{
  osk_open (self, TRUE);
  self->gesture_in_progress = FALSE;
}

static void
osk_gesture_cancel (ShellOsk *self)
{
  if (self->torn_down)
    return;

  if (self->gesture_in_progress)
    osk_animate_hide (self);
  self->gesture_in_progress = FALSE;
}

static void
osk_rest_open_cb (gpointer user_data)
{
  ShellOsk *self = user_data;

  self->keyboard_resting_id = 0;

  /* _open () */
  if (!self->keyboard_requested)
    return;

  osk_relayout (self);
  osk_animate_show (self);

  osk_set_emoji_active (self, FALSE);
}

static void
osk_rest_close_cb (gpointer user_data)
{
  ShellOsk *self = user_data;

  self->keyboard_resting_id = 0;

  /* _close () */
  if (self->keyboard_requested)
    return;

  osk_animate_hide (self);
  /* setCursorLocation (null) */
  osk_set_cursor_location (self, NULL, 0, 0, 0, 0);
  osk_disable_all_modifiers (self);
}

static void
osk_open (ShellOsk *self,
          gboolean  immediate)
{
  if (self->torn_down)
    return;

  osk_clear_show_idle (self);
  self->keyboard_requested = TRUE;

  if (self->keyboard_visible)
    {
      osk_relayout (self);
      return;
    }

  controller_set_osk_completion (self->controller, TRUE);
  osk_clear_keyboard_rest_timer (self);

  if (immediate)
    {
      osk_rest_open_cb (self);
      return;
    }

  self->keyboard_resting_id = g_timeout_add_once (KEYBOARD_REST_TIME, osk_rest_open_cb, self);
  g_source_set_name_by_id (self->keyboard_resting_id,
                           "[gnome-shell] this._clearKeyboardRestTimer");
}

static void
osk_close (ShellOsk *self,
           gboolean  immediate)
{
  if (self->torn_down)
    return;

  osk_clear_show_idle (self);
  self->keyboard_requested = FALSE;

  if (!self->keyboard_visible)
    return;

  controller_set_osk_completion (self->controller, FALSE);
  osk_clear_keyboard_rest_timer (self);

  if (immediate)
    {
      osk_rest_close_cb (self);
      return;
    }

  self->keyboard_resting_id = g_timeout_add_once (KEYBOARD_REST_TIME, osk_rest_close_cb, self);
  g_source_set_name_by_id (self->keyboard_resting_id,
                           "[gnome-shell] this._clearKeyboardRestTimer");
}

/* ---- the focus window and the cursor --------------------------------- */

static void
osk_set_focus_window (ShellOsk   *self,
                      MetaWindow *window)
{
  GObject *window_actor;

  if (self->focus_window == window)
    return;

  if (self->keyboard_visible && self->focus_window != NULL)
    osk_animate_window (self, self->focus_window, FALSE);

  window_actor = window != NULL ? meta_window_get_compositor_private (window) : NULL;
  if (window_actor != NULL)
    clutter_actor_remove_transition (CLUTTER_ACTOR (window_actor), "y");
  self->focus_window_start_y = window_actor != NULL
    ? clutter_actor_get_y (CLUTTER_ACTOR (window_actor))
    : 0;

  if (self->keyboard_visible && window != NULL)
    osk_animate_window (self, window, TRUE);

  g_set_weak_pointer (&self->focus_window, window);
}

static void
osk_set_cursor_location (ShellOsk   *self,
                         MetaWindow *window,
                         double      x,
                         double      y,
                         double      w,
                         double      h)
{
  g_autoptr (ShellMonitor) monitor = NULL;

  if (self->layout_manager != NULL)
    monitor = shell_layout_manager_get_keyboard_monitor (self->layout_manager);

  if (window != NULL && monitor != NULL)
    {
      StBoxLayout *keyboard_box = osk_get_keyboard_box (self);
      double keyboard_height = keyboard_box != NULL
        ? clutter_actor_get_height (CLUTTER_ACTOR (keyboard_box))
        : 0;
      double keyboard_y1 = (monitor->y + monitor->height) - keyboard_height;

      if (self->focus_window == window)
        {
          if (y + h + keyboard_height < keyboard_y1)
            osk_set_focus_window (self, NULL);

          return;
        }

      if (y + h >= keyboard_y1)
        osk_set_focus_window (self, window);
      else
        osk_set_focus_window (self, NULL);
    }
  else
    {
      osk_set_focus_window (self, NULL);
    }
}

/* ---- signal handlers ------------------------------------------------- */

static void
osk_focus_position_changed_cb (ShellOskFocusTracker *tracker,
                               ShellOsk             *self)
{
  double x, y, width, height;

  focus_tracker_get_current_rect (tracker, &x, &y, &width, &height);
  osk_set_cursor_location (self, tracker->current_window, x, y, width, height);
  osk_update_level_from_hints (self, TRUE);
}

static void
osk_window_grabbed_cb (ShellOskFocusTracker *tracker,
                       ShellOsk             *self)
{
  osk_on_focus_window_moving (self, tracker);
}

static void
osk_monitors_changed_cb (ShellLayoutManager *layout_manager,
                         ShellOsk           *self)
{
  osk_relayout (self);
}

static void
osk_show_idle_cb (gpointer user_data)
{
  ShellOsk *self = user_data;

  self->show_idle_id = 0;

  /* The JS gave the focus monitor index as the `immediate` argument */
  osk_open (self, self->layout_manager != NULL &&
                  shell_layout_manager_get_focus_index (self->layout_manager) != 0);
}

static void
osk_key_focus_changed_cb (ClutterStage *stage,
                          GParamSpec   *pspec,
                          ShellOsk     *self)
{
  ClutterActor *focus = clutter_stage_get_key_focus (stage);

  /* Showing an extended key popup and clicking a key from the extended keys
   * will grab focus, but ignore that
   */
  gboolean extended_keys_were_focused = self->focus_in_extended_keys;

  self->focus_in_extended_keys =
    focus != NULL && g_object_get_data (G_OBJECT (focus), EXTENDED_KEY_DATA) != NULL;
  if (self->focus_in_extended_keys || extended_keys_were_focused)
    return;

  if (!CLUTTER_IS_TEXT (focus))
    {
      osk_close (self, FALSE);
      return;
    }

  if (self->show_idle_id == 0)
    {
      self->show_idle_id = g_idle_add_once (osk_show_idle_cb, self);
      g_source_set_name_by_id (self->show_idle_id, "[gnome-shell] this.open");
    }
}

static void
osk_group_changed_cb (ShellOskController *controller,
                      ShellOsk           *self)
{
  osk_update_keys (self);
}

static void
osk_purpose_changed_cb (ShellOskController        *controller,
                        ClutterInputContentPurpose purpose,
                        ShellOsk                  *self)
{
  osk_update_keys (self);
}

static void
osk_content_hints_changed_cb (ShellOskController            *controller,
                              ClutterInputContentHintFlags   content_hints,
                              ShellOsk                      *self)
{
  self->content_hints = content_hints;

  if (osk_is_visible (self) &&
      (content_hints & CLUTTER_INPUT_CONTENT_HINT_INHIBIT_OSK) != 0)
    {
      osk_close (self, FALSE);
    }
  else
    {
      gboolean emoji_visible = osk_should_show_emoji (self);

      if (emoji_visible != self->emoji_visible)
        osk_update_keys (self);
      else
        osk_update_level_from_hints (self, FALSE);
    }
}

static void
osk_panel_state_cb (ShellOskController     *controller,
                    ClutterInputPanelState  state,
                    ShellOsk               *self)
{
  gboolean enabled;

  if (state == CLUTTER_INPUT_PANEL_STATE_OFF)
    enabled = FALSE;
  else if (state == CLUTTER_INPUT_PANEL_STATE_ON)
    enabled = TRUE;
  else if (state == CLUTTER_INPUT_PANEL_STATE_TOGGLE)
    enabled = !self->keyboard_visible;
  else
    return;

  if ((self->content_hints & CLUTTER_INPUT_CONTENT_HINT_INHIBIT_OSK) != 0)
    enabled = FALSE;

  if (enabled)
    {
      /* The JS gave the focus monitor index as the `immediate` argument */
      osk_open (self, self->layout_manager != NULL &&
                      shell_layout_manager_get_focus_index (self->layout_manager) != 0);
    }
  else
    {
      osk_close (self, TRUE);
    }
}

static void
osk_emoji_selection_toggle_cb (ShellOskEmojiSelection *selection,
                               ShellOsk               *self)
{
  osk_toggle_emoji (self);
}

static void
osk_emoji_selection_close_request_cb (ShellOskEmojiSelection *selection,
                                      ShellOsk               *self)
{
  osk_close (self, TRUE);
}

static void
osk_emoji_selection_emoji_selected_cb (ShellOskEmojiSelection *selection,
                                       const char             *emoji,
                                       ShellOsk               *self)
{
  controller_commit (self->controller, emoji, NULL, NULL, NULL, NULL);
}

/* ---- suggestions ----------------------------------------------------- */

static void
osk_reset_suggestions (ShellOsk *self)
{
  if (self->suggestions != NULL)
    suggestions_clear (self->suggestions);
}

static void
osk_set_suggestions_visible (ShellOsk *self,
                             gboolean  visible)
{
  if (self->suggestions != NULL)
    suggestions_set_visible (self->suggestions, visible);
}

static void
osk_add_suggestion (ShellOsk               *self,
                    const char             *text,
                    ShellOskSuggestionFunc  callback,
                    gpointer                user_data,
                    GDestroyNotify          destroy)
{
  if (self->suggestions == NULL)
    {
      if (destroy != NULL)
        destroy (user_data);
      return;
    }

  suggestions_add (self->suggestions, text, callback, user_data, destroy);
  clutter_actor_show (CLUTTER_ACTOR (self->suggestions));
}

/* ---- construction and destruction ------------------------------------ */

static void
osk_shutdown (ShellOsk *self)
{
  osk_clear_show_idle (self);
  osk_clear_keyboard_rest_timer (self);
}

static void
osk_teardown (ShellOsk *self)
{
  StBoxLayout *keyboard_box;

  if (self->torn_down)
    return;

  self->torn_down = TRUE;

  if (self->focus_tracker != NULL)
    {
      if (self->window_moved_id != 0)
        g_signal_handler_disconnect (self->focus_tracker, self->window_moved_id);
      self->window_moved_id = 0;

      g_signal_handlers_disconnect_by_data (self->focus_tracker, self);
      g_clear_object (&self->focus_tracker);
    }

  osk_clear_show_idle (self);
  osk_clear_keyboard_rest_timer (self);

  if (self->surrounding_text_id != 0 && self->input_method != NULL)
    g_signal_handler_disconnect (self->input_method, self->surrounding_text_id);
  self->surrounding_text_id = 0;

  if (self->controller != NULL)
    {
      controller_set_osk_completion (self->controller, FALSE);
      g_signal_handlers_disconnect_by_data (self->controller, self);
      controller_destroy (self->controller);
      g_clear_object (&self->controller);
    }

  /* The layout manager may be tearing down its boxes */
  keyboard_box = osk_get_keyboard_box (self);
  if (keyboard_box != NULL)
    {
      shell_layout_manager_untrack_chrome (self->layout_manager, CLUTTER_ACTOR (self));
      if (clutter_actor_get_parent (CLUTTER_ACTOR (self)) == CLUTTER_ACTOR (keyboard_box))
        clutter_actor_remove_child (CLUTTER_ACTOR (keyboard_box), CLUTTER_ACTOR (self));
      clutter_actor_hide (CLUTTER_ACTOR (keyboard_box));
    }

  if (self->language_popup != NULL)
    {
      shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (self->language_popup));
      g_clear_object (&self->language_popup);
    }

  if (self->stage != NULL)
    g_signal_handlers_disconnect_by_data (self->stage, self);
  if (self->layout_manager != NULL)
    g_signal_handlers_disconnect_by_data (self->layout_manager, self);

  if (self->current_page != NULL && self->current_page_destroy_id != 0)
    g_signal_handler_disconnect (self->current_page, self->current_page_destroy_id);
  self->current_page_destroy_id = 0;
  self->current_page = NULL;
}

static void
osk_destroy (ClutterActor *actor)
{
  osk_teardown (SHELL_OSK (actor));

  CLUTTER_ACTOR_CLASS (shell_osk_parent_class)->destroy (actor);
}

static void
osk_constructed (GObject *object)
{
  ShellOsk *self = SHELL_OSK (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellOskManager *manager = self->manager;
  ShellGlobal *global = shell_global_get ();
  StBoxLayout *keyboard_box;
  ClutterActor *emoji_selection;

  G_OBJECT_CLASS (shell_osk_parent_class)->constructed (object);

  g_return_if_fail (SHELL_IS_OSK_MANAGER (manager));

  st_box_layout_set_orientation (ST_BOX_LAYOUT (self), CLUTTER_ORIENTATION_VERTICAL);
  clutter_actor_set_name (actor, "keyboard");
  clutter_actor_set_reactive (actor, TRUE);
  /* Keyboard models are defined in LTR, we must override
   * the locale setting in order to avoid flipping the
   * keyboard on RTL locales.
   */
  clutter_actor_set_text_direction (actor, CLUTTER_TEXT_DIRECTION_LTR);

  g_set_weak_pointer (&self->layout_manager, manager->layout_manager);
  g_set_weak_pointer (&self->stage, manager->stage);
  g_set_weak_pointer (&self->input_method, manager->input_method);
  g_set_weak_pointer (&self->compositor, shell_global_get_compositor (global));

  self->modifiers = g_array_new (FALSE, FALSE, sizeof (guint));
  self->modifier_keys = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                               (GDestroyNotify) g_ptr_array_unref);

  self->focus_tracker = focus_tracker_new (shell_global_get_display (global),
                                           manager->input_method);
  g_signal_connect_object (self->focus_tracker, "position-changed",
                           G_CALLBACK (osk_focus_position_changed_cb), self, 0);
  g_signal_connect_object (self->focus_tracker, "window-grabbed",
                           G_CALLBACK (osk_window_grabbed_cb), self, 0);
  self->window_moved_id = g_signal_connect_swapped (self->focus_tracker, "window-moved",
                                                    G_CALLBACK (osk_on_focus_window_moving),
                                                    self);

  if (self->layout_manager != NULL)
    g_signal_connect_object (self->layout_manager, "monitors-changed",
                             G_CALLBACK (osk_monitors_changed_cb), self, 0);

  /* _setupKeyboard () */
  keyboard_box = osk_get_keyboard_box (self);
  if (keyboard_box != NULL)
    {
      clutter_actor_add_child (CLUTTER_ACTOR (keyboard_box), actor);
      shell_layout_manager_track_chrome (self->layout_manager, actor);
    }

  self->controller = controller_new (manager);

  self->suggestions = g_object_new (SHELL_TYPE_OSK_SUGGESTIONS, NULL);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (self->suggestions));
  g_object_add_weak_pointer (G_OBJECT (self->suggestions), (gpointer *) &self->suggestions);

  self->aspect_container = aspect_container_new (FALSE);
  clutter_actor_add_child (actor, CLUTTER_ACTOR (self->aspect_container));
  g_object_add_weak_pointer (G_OBJECT (self->aspect_container),
                             (gpointer *) &self->aspect_container);

  emoji_selection = g_object_new (SHELL_TYPE_OSK_EMOJI_SELECTION, NULL);
  self->emoji_selection = SHELL_OSK_EMOJI_SELECTION (emoji_selection);
  g_signal_connect_object (emoji_selection, "toggle",
                           G_CALLBACK (osk_emoji_selection_toggle_cb), self, 0);
  g_signal_connect_object (emoji_selection, "close-request",
                           G_CALLBACK (osk_emoji_selection_close_request_cb), self, 0);
  g_signal_connect_object (emoji_selection, "emoji-selected",
                           G_CALLBACK (osk_emoji_selection_emoji_selected_cb), self, 0);
  clutter_actor_hide (emoji_selection);
  clutter_actor_add_child (CLUTTER_ACTOR (self->aspect_container), emoji_selection);
  g_object_add_weak_pointer (G_OBJECT (emoji_selection), (gpointer *) &self->emoji_selection);

  osk_update_keys (self);

  g_signal_connect_object (self->controller, "group-changed",
                           G_CALLBACK (osk_group_changed_cb), self, 0);
  g_signal_connect_object (self->controller, "panel-state",
                           G_CALLBACK (osk_panel_state_cb), self, 0);
  g_signal_connect_object (self->controller, "purpose-changed",
                           G_CALLBACK (osk_purpose_changed_cb), self, 0);
  g_signal_connect_object (self->controller, "content-hints-changed",
                           G_CALLBACK (osk_content_hints_changed_cb), self, 0);
  g_signal_connect_object (self->stage, "notify::key-focus",
                           G_CALLBACK (osk_key_focus_changed_cb), self, 0);

  osk_relayout (self);
}

static void
osk_dispose (GObject *object)
{
  ShellOsk *self = SHELL_OSK (object);

  osk_teardown (self);

  if (self->suggestions != NULL)
    g_object_remove_weak_pointer (G_OBJECT (self->suggestions), (gpointer *) &self->suggestions);
  if (self->aspect_container != NULL)
    g_object_remove_weak_pointer (G_OBJECT (self->aspect_container),
                                  (gpointer *) &self->aspect_container);
  if (self->emoji_selection != NULL)
    g_object_remove_weak_pointer (G_OBJECT (self->emoji_selection),
                                  (gpointer *) &self->emoji_selection);

  g_clear_weak_pointer (&self->current_layout);
  g_clear_weak_pointer (&self->focus_window);
  g_clear_pointer (&self->layers, g_hash_table_unref);
  g_clear_pointer (&self->modifier_keys, g_hash_table_unref);
  g_clear_pointer (&self->modifiers, g_array_unref);

  g_clear_weak_pointer (&self->layout_manager);
  g_clear_weak_pointer (&self->stage);
  g_clear_weak_pointer (&self->compositor);
  g_clear_weak_pointer (&self->input_method);
  g_clear_weak_pointer (&self->manager);

  G_OBJECT_CLASS (shell_osk_parent_class)->dispose (object);
}

static void
osk_get_property (GObject    *object,
                  guint       prop_id,
                  GValue     *value,
                  GParamSpec *pspec)
{
  ShellOsk *self = SHELL_OSK (object);

  switch (prop_id)
    {
    case OSK_PROP_MANAGER:
      g_value_set_object (value, self->manager);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
osk_set_property (GObject      *object,
                  guint         prop_id,
                  const GValue *value,
                  GParamSpec   *pspec)
{
  ShellOsk *self = SHELL_OSK (object);

  switch (prop_id)
    {
    case OSK_PROP_MANAGER:
      g_set_weak_pointer (&self->manager, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_osk_class_init (ShellOskClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = osk_constructed;
  object_class->dispose = osk_dispose;
  object_class->get_property = osk_get_property;
  object_class->set_property = osk_set_property;

  actor_class->destroy = osk_destroy;

  osk_props[OSK_PROP_MANAGER] =
    g_param_spec_object ("manager", NULL, NULL,
                         SHELL_TYPE_OSK_MANAGER,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, OSK_N_PROPS, osk_props);

  osk_signals[OSK_VISIBILITY_CHANGED] =
    g_signal_new ("visibility-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_osk_init (ShellOsk *self)
{
}

static ShellOsk *
osk_new (ShellOskManager *manager)
{
  return g_object_new (SHELL_TYPE_OSK,
                       "manager", manager,
                       NULL);
}


/* ------------------------------------------------------------------------ */
/* ShellOskManager                                                           */
/* ------------------------------------------------------------------------ */

static void
osk_manager_request_activate_input_source (ShellOskManager *manager,
                                           int              position)
{
  g_signal_emit (manager, manager_signals[MANAGER_ACTIVATE_INPUT_SOURCE], 0, position);
}

static gboolean
manager_last_device_is_touchscreen (ShellOskManager *self)
{
  if (self->last_device == NULL)
    return FALSE;

  return clutter_input_device_get_device_type (self->last_device) ==
         CLUTTER_TOUCHSCREEN_DEVICE;
}

static void
manager_osk_visibility_changed_cb (ShellOsk        *osk,
                                   ShellOskManager *self)
{
  gboolean visible = shell_osk_manager_get_visible (self);

  shell_input_method_set_osk_visible (self->input_method, visible);
  g_object_notify_by_pspec (G_OBJECT (self), manager_props[MANAGER_PROP_VISIBLE]);
  g_signal_emit (self, manager_signals[MANAGER_VISIBILITY_CHANGED], 0);
  clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->bottom_drag_gesture),
                                  !osk_is_visible (osk));
}

static void
manager_sync_enabled (ShellOskManager *self)
{
  gboolean enable_keyboard =
    g_settings_get_boolean (self->a11y_applications_settings, SHOW_KEYBOARD);
  gboolean auto_enabled = self->seat != NULL &&
                          clutter_seat_get_touch_mode (self->seat) &&
                          manager_last_device_is_touchscreen (self);
  gboolean enabled = enable_keyboard || auto_enabled;

  if (!enabled && self->keyboard == NULL)
    return;

  if (enabled && self->keyboard == NULL)
    {
      /* The input source manager pushes its state in response */
      g_signal_emit (self, manager_signals[MANAGER_INPUT_SOURCES_REQUESTED], 0);

      self->keyboard = g_object_ref_sink (osk_new (self));
      g_signal_connect_object (self->keyboard, "visibility-changed",
                               G_CALLBACK (manager_osk_visibility_changed_cb), self, 0);
      g_object_notify_by_pspec (G_OBJECT (self), manager_props[MANAGER_PROP_KEYBOARD_ACTOR]);
    }
  else if (!enabled && self->keyboard != NULL)
    {
      osk_set_cursor_location (self->keyboard, NULL, 0, 0, 0, 0);
      clutter_actor_destroy (CLUTTER_ACTOR (self->keyboard));
      g_clear_object (&self->keyboard);
      shell_input_method_set_osk_visible (self->input_method, FALSE);
      clutter_actor_meta_set_enabled (CLUTTER_ACTOR_META (self->bottom_drag_gesture), TRUE);
      g_object_notify_by_pspec (G_OBJECT (self), manager_props[MANAGER_PROP_KEYBOARD_ACTOR]);
      g_object_notify_by_pspec (G_OBJECT (self), manager_props[MANAGER_PROP_VISIBLE]);
    }
}

static void
manager_a11y_changed_cb (GSettings       *settings,
                         const char      *key,
                         ShellOskManager *self)
{
  manager_sync_enabled (self);
}

static void
manager_touch_mode_changed_cb (ClutterSeat     *seat,
                               GParamSpec      *pspec,
                               ShellOskManager *self)
{
  manager_sync_enabled (self);
}

static void
manager_last_device_changed_cb (MetaBackend        *backend,
                                ClutterInputDevice *device,
                                ShellOskManager    *self)
{
  if (clutter_input_device_get_device_type (device) == CLUTTER_KEYBOARD_DEVICE)
    return;

  g_set_object (&self->last_device, device);
  manager_sync_enabled (self);
}

static gboolean
manager_bottom_drag_may_recognize_cb (ClutterGesture *gesture,
                                      gpointer        user_data)
{
  guint allowed_modes = SHELL_ACTION_MODE_ALL & ~SHELL_ACTION_MODE_LOCK_SCREEN;

  return (allowed_modes & shell_main_get_action_mode (shell_main_get_default ())) != 0;
}

static void
manager_bottom_drag_progress_cb (ShellEdgeDragGesture *gesture,
                                 float                 progress,
                                 ShellOskManager      *self)
{
  if (self->keyboard != NULL)
    osk_gesture_progress (self->keyboard, progress);
}

static void
manager_bottom_drag_end_cb (ClutterGesture  *gesture,
                            ShellOskManager *self)
{
  if (self->keyboard != NULL)
    osk_gesture_activate (self->keyboard);
}

static void
manager_bottom_drag_cancel_cb (ClutterGesture  *gesture,
                               ShellOskManager *self)
{
  if (self->keyboard != NULL)
    osk_gesture_cancel (self->keyboard);
}

static void
manager_global_shutdown_cb (ShellGlobal     *global,
                            ShellOskManager *self)
{
  self->shutting_down = TRUE;

  if (self->keyboard != NULL)
    osk_shutdown (self->keyboard);
}

static void
manager_constructed (GObject *object)
{
  ShellOskManager *self = SHELL_OSK_MANAGER (object);
  ShellGlobal *global = shell_global_get ();
  ClutterBackend *clutter_backend;
  ClutterStage *stage;

  G_OBJECT_CLASS (shell_osk_manager_parent_class)->constructed (object);

  g_return_if_fail (SHELL_IS_INPUT_METHOD (self->input_method));

  stage = shell_global_get_stage (global);
  g_set_weak_pointer (&self->stage, stage);
  g_set_weak_pointer (&self->backend, shell_global_get_backend (global));
  g_set_weak_pointer (&self->layout_manager,
                      shell_main_get_layout_manager (shell_main_get_default ()));

  self->a11y_applications_settings = g_settings_new (A11Y_APPLICATIONS_SCHEMA);
  g_signal_connect_object (self->a11y_applications_settings, "changed",
                           G_CALLBACK (manager_a11y_changed_cb), self, 0);

  clutter_backend = clutter_context_get_backend (clutter_actor_get_context (CLUTTER_ACTOR (stage)));
  g_set_weak_pointer (&self->seat, clutter_backend_get_default_seat (clutter_backend));
  g_signal_connect_object (self->seat, "notify::touch-mode",
                           G_CALLBACK (manager_touch_mode_changed_cb), self, 0);

  g_signal_connect_object (self->backend, "last-device-changed",
                           G_CALLBACK (manager_last_device_changed_cb), self, 0);

  self->bottom_drag_gesture = g_object_ref_sink (g_object_new (SHELL_TYPE_EDGE_DRAG_GESTURE,
                                                               NULL));
  clutter_actor_meta_set_name (CLUTTER_ACTOR_META (self->bottom_drag_gesture),
                               "OSK show bottom drag");
  shell_edge_drag_gesture_set_side (self->bottom_drag_gesture, ST_SIDE_BOTTOM);
  g_signal_connect (self->bottom_drag_gesture, "may-recognize",
                    G_CALLBACK (manager_bottom_drag_may_recognize_cb), NULL);
  g_signal_connect_object (self->bottom_drag_gesture, "progress",
                           G_CALLBACK (manager_bottom_drag_progress_cb), self, 0);
  g_signal_connect_object (self->bottom_drag_gesture, "end",
                           G_CALLBACK (manager_bottom_drag_end_cb), self, 0);
  g_signal_connect_object (self->bottom_drag_gesture, "cancel",
                           G_CALLBACK (manager_bottom_drag_cancel_cb), self, 0);
  clutter_actor_add_action (CLUTTER_ACTOR (stage), CLUTTER_ACTION (self->bottom_drag_gesture));

  g_signal_connect_object (global, "shutdown",
                           G_CALLBACK (manager_global_shutdown_cb), self, 0);

  manager_sync_enabled (self);
}

static void
manager_dispose (GObject *object)
{
  ShellOskManager *self = SHELL_OSK_MANAGER (object);

  if (self->bottom_drag_gesture != NULL)
    {
      ClutterActor *attached =
        clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self->bottom_drag_gesture));

      if (attached != NULL)
        clutter_actor_remove_action (attached, CLUTTER_ACTION (self->bottom_drag_gesture));
    }
  g_clear_object (&self->bottom_drag_gesture);

  if (self->keyboard != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->keyboard, self);
      clutter_actor_destroy (CLUTTER_ACTOR (self->keyboard));
    }
  g_clear_object (&self->keyboard);

  g_clear_object (&self->a11y_applications_settings);
  g_clear_object (&self->last_device);
  g_clear_weak_pointer (&self->stage);
  g_clear_weak_pointer (&self->seat);
  g_clear_weak_pointer (&self->backend);
  g_clear_weak_pointer (&self->layout_manager);
  g_clear_object (&self->input_method);
  g_clear_object (&self->current_source_properties);

  G_OBJECT_CLASS (shell_osk_manager_parent_class)->dispose (object);
}

static void
manager_finalize (GObject *object)
{
  ShellOskManager *self = SHELL_OSK_MANAGER (object);

  g_clear_pointer (&self->source_names, g_strfreev);
  g_clear_pointer (&self->current_source_id, g_free);
  g_clear_pointer (&self->current_source_xkb_id, g_free);

  G_OBJECT_CLASS (shell_osk_manager_parent_class)->finalize (object);
}

static void
manager_get_property (GObject    *object,
                      guint       prop_id,
                      GValue     *value,
                      GParamSpec *pspec)
{
  ShellOskManager *self = SHELL_OSK_MANAGER (object);

  switch (prop_id)
    {
    case MANAGER_PROP_INPUT_METHOD:
      g_value_set_object (value, self->input_method);
      break;

    case MANAGER_PROP_KEYBOARD_ACTOR:
      g_value_set_object (value, shell_osk_manager_get_keyboard_actor (self));
      break;

    case MANAGER_PROP_VISIBLE:
      g_value_set_boolean (value, shell_osk_manager_get_visible (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
manager_set_property (GObject      *object,
                      guint         prop_id,
                      const GValue *value,
                      GParamSpec   *pspec)
{
  ShellOskManager *self = SHELL_OSK_MANAGER (object);

  switch (prop_id)
    {
    case MANAGER_PROP_INPUT_METHOD:
      g_set_object (&self->input_method, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_osk_manager_class_init (ShellOskManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellHookRegistry *registry;

  object_class->constructed = manager_constructed;
  object_class->dispose = manager_dispose;
  object_class->finalize = manager_finalize;
  object_class->get_property = manager_get_property;
  object_class->set_property = manager_set_property;

  /**
   * ShellOskManager:input-method:
   *
   * The input method of the compositor (`Main.inputMethod`).
   */
  manager_props[MANAGER_PROP_INPUT_METHOD] =
    g_param_spec_object ("input-method", NULL, NULL,
                         SHELL_TYPE_INPUT_METHOD,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellOskManager:keyboard-actor:
   *
   * The actor of the on-screen keyboard, or %NULL while it is disabled
   * (`keyboardActor`).
   */
  manager_props[MANAGER_PROP_KEYBOARD_ACTOR] =
    g_param_spec_object ("keyboard-actor", NULL, NULL,
                         CLUTTER_TYPE_ACTOR,
                         G_PARAM_READABLE |
                         G_PARAM_EXPLICIT_NOTIFY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellOskManager:visible:
   *
   * Whether the on-screen keyboard is enabled and shown (`visible`).
   */
  manager_props[MANAGER_PROP_VISIBLE] =
    g_param_spec_boolean ("visible", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, MANAGER_N_PROPS, manager_props);

  /**
   * ShellOskManager::visibility-changed:
   * @self: the #ShellOskManager
   *
   * Emitted when the keyboard is shown or hidden.
   */
  manager_signals[MANAGER_VISIBILITY_CHANGED] =
    g_signal_new ("visibility-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellOskManager::input-sources-requested:
   * @self: the #ShellOskManager
   *
   * Emitted before the keyboard is created. The handler has to give the
   * input sources with shell_osk_manager_set_input_sources () and
   * shell_osk_manager_set_current_input_source () if it has any.
   */
  manager_signals[MANAGER_INPUT_SOURCES_REQUESTED] =
    g_signal_new ("input-sources-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellOskManager::activate-input-source:
   * @self: the #ShellOskManager
   * @position: the position of the input source in the list given to
   *   shell_osk_manager_set_input_sources ()
   *
   * Emitted when the user chooses an input source in the language menu of
   * the keyboard. The handler has to activate the input source
   * interactively.
   */
  manager_signals[MANAGER_ACTIVATE_INPUT_SOURCE] =
    g_signal_new ("activate-input-source",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_INT);

  /* Lua extensions (gnome-lua): the hook points of the on-screen keyboard */
  registry = shell_hook_registry_get_default ();
  shell_hook_registry_declare (registry, "osk.shown",
                               "monitor:i", FALSE,
                               "The on-screen keyboard was shown");
  shell_hook_registry_declare (registry, "osk.hidden",
                               "", FALSE,
                               "The on-screen keyboard was hidden");
}

static void
shell_osk_manager_init (ShellOskManager *self)
{
  self->current_source_index = -1;
}

/**
 * shell_osk_manager_new:
 * @input_method: the input method of the compositor
 *
 * Creates the manager of the on-screen keyboard (`new Keyboard.KeyboardManager ()`).
 * It creates the keyboard at once if the accessibility setting enables it.
 *
 * Returns: (transfer full): a new #ShellOskManager
 */
ShellOskManager *
shell_osk_manager_new (ShellInputMethod *input_method)
{
  g_return_val_if_fail (SHELL_IS_INPUT_METHOD (input_method), NULL);

  return g_object_new (SHELL_TYPE_OSK_MANAGER,
                       "input-method", input_method,
                       NULL);
}

/**
 * shell_osk_manager_get_keyboard_actor:
 * @self: a #ShellOskManager
 *
 * Returns: (transfer none) (nullable): #ShellOskManager:keyboard-actor
 */
ClutterActor *
shell_osk_manager_get_keyboard_actor (ShellOskManager *self)
{
  g_return_val_if_fail (SHELL_IS_OSK_MANAGER (self), NULL);

  return self->keyboard != NULL ? CLUTTER_ACTOR (self->keyboard) : NULL;
}

/**
 * shell_osk_manager_get_visible:
 * @self: a #ShellOskManager
 *
 * Returns: #ShellOskManager:visible
 */
gboolean
shell_osk_manager_get_visible (ShellOskManager *self)
{
  g_return_val_if_fail (SHELL_IS_OSK_MANAGER (self), FALSE);

  return self->keyboard != NULL && osk_is_visible (self->keyboard);
}

/**
 * shell_osk_manager_open:
 * @self: a #ShellOskManager
 * @monitor: the index of the monitor to show the keyboard on
 *
 * Shows the keyboard, if it is enabled, after the time it takes to rest.
 */
void
shell_osk_manager_open (ShellOskManager *self,
                        int              monitor)
{
  g_return_if_fail (SHELL_IS_OSK_MANAGER (self));

  if (self->layout_manager != NULL)
    shell_layout_manager_set_keyboard_index (self->layout_manager, monitor);

  if (self->keyboard != NULL)
    osk_open (self->keyboard, FALSE);
}

/**
 * shell_osk_manager_close:
 * @self: a #ShellOskManager
 *
 * Hides the keyboard, if it is enabled, after the time it takes to rest.
 */
void
shell_osk_manager_close (ShellOskManager *self)
{
  g_return_if_fail (SHELL_IS_OSK_MANAGER (self));

  if (self->keyboard != NULL)
    osk_close (self->keyboard, FALSE);
}

/**
 * shell_osk_manager_add_suggestion:
 * @self: a #ShellOskManager
 * @text: the suggested word
 * @callback: (scope notified) (closure user_data) (destroy destroy): called
 *   when the word is clicked
 * @user_data: data for @callback
 * @destroy: called on @user_data when the suggestion is removed
 *
 * Adds a word to the suggestions above the keys. Nothing is added, and
 * @destroy is called at once, if the keyboard is not enabled.
 */
void
shell_osk_manager_add_suggestion (ShellOskManager        *self,
                                  const char             *text,
                                  ShellOskSuggestionFunc  callback,
                                  gpointer                user_data,
                                  GDestroyNotify          destroy)
{
  g_return_if_fail (SHELL_IS_OSK_MANAGER (self));
  g_return_if_fail (text != NULL);

  if (self->keyboard != NULL)
    {
      osk_add_suggestion (self->keyboard, text, callback, user_data, destroy);
      return;
    }

  if (destroy != NULL)
    destroy (user_data);
}

/**
 * shell_osk_manager_reset_suggestions:
 * @self: a #ShellOskManager
 *
 * Removes all the suggestions.
 */
void
shell_osk_manager_reset_suggestions (ShellOskManager *self)
{
  g_return_if_fail (SHELL_IS_OSK_MANAGER (self));

  if (self->keyboard != NULL)
    osk_reset_suggestions (self->keyboard);
}

/**
 * shell_osk_manager_set_suggestions_visible:
 * @self: a #ShellOskManager
 * @visible: whether the suggestions are shown
 *
 * Shows or hides the suggestions.
 */
void
shell_osk_manager_set_suggestions_visible (ShellOskManager *self,
                                           gboolean         visible)
{
  g_return_if_fail (SHELL_IS_OSK_MANAGER (self));

  if (self->keyboard != NULL)
    osk_set_suggestions_visible (self->keyboard, visible);
}

/**
 * shell_osk_manager_set_input_sources:
 * @self: a #ShellOskManager
 * @display_names: (array zero-terminated=1) (element-type utf8): the names of
 *   the input sources, in the order they are listed in the language menu
 * @current_index: the position in @display_names of the current input
 *   source, or -1 if it is none of them
 *
 * Tells the keyboard which input sources there are (`sources-changed` of the
 * input source manager). The layout is built again.
 */
void
shell_osk_manager_set_input_sources (ShellOskManager    *self,
                                     const char * const *display_names,
                                     int                 current_index)
{
  g_return_if_fail (SHELL_IS_OSK_MANAGER (self));

  g_strfreev (self->source_names);
  self->source_names = g_strdupv ((char **) display_names);
  self->current_source_index = current_index;

  if (self->keyboard != NULL && self->keyboard->controller != NULL)
    controller_notify_group_changed (self->keyboard->controller);
}

/**
 * shell_osk_manager_set_current_input_source:
 * @self: a #ShellOskManager
 * @current_index: the position of the input source in the list given to
 *   shell_osk_manager_set_input_sources (), or -1
 * @id: (nullable): the id of the input source, %NULL if there is none
 * @xkb_id: (nullable): the id of the XKB layout of the input source
 * @is_ibus: whether the input source is an IBus engine
 * @properties: (nullable): the properties of the IBus engine, if it has
 *   registered any
 *
 * Tells the keyboard which input source is current
 * (`current-source-changed` of the input source manager). The layout is
 * built again.
 */
void
shell_osk_manager_set_current_input_source (ShellOskManager *self,
                                            int              current_index,
                                            const char      *id,
                                            const char      *xkb_id,
                                            gboolean         is_ibus,
                                            IBusPropList    *properties)
{
  g_return_if_fail (SHELL_IS_OSK_MANAGER (self));
  g_return_if_fail (properties == NULL || IBUS_IS_PROP_LIST (properties));

  self->current_source_index = current_index;
  g_free (self->current_source_id);
  self->current_source_id = g_strdup (id);
  g_free (self->current_source_xkb_id);
  self->current_source_xkb_id = g_strdup (xkb_id);
  self->current_source_is_ibus = is_ibus;
  g_set_object (&self->current_source_properties, properties);

  if (self->keyboard != NULL && self->keyboard->controller != NULL)
    controller_notify_group_changed (self->keyboard->controller);
}
