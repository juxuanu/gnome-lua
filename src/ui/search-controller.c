/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * search-controller.c: The search entry of the overview and the search
 * results shown while the user types. Port of js/ui/searchController.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The controller is created with its parts as construct properties
 *    (`new SearchController (searchEntry, showAppsButton)` is
 *    `new Shell.SearchController ({search_entry, show_apps_button,
 *    ctrl_alt_tab_manager, application_provider})`): the ctrl-alt-tab
 *    manager and the applications provider were reached through `Main` and
 *    `AppDisplay`, which are still JS.
 *  - `Main.overview.connect ('showing' / 'hiding')` is
 *    shell_search_controller_overview_showing () and
 *    shell_search_controller_overview_hiding (), which overview-controls.c
 *    calls from the overview signals; `Main.overview.hide ()` and
 *    `Main.overview.toggle ()` are the ::overview-hide-requested and
 *    ::overview-toggle-requested signals.
 *  - The search string is split at the Unicode white space of GLib, where
 *    the JS split at /\s+/.
 *  - Nothing is reset when the controller is unmapped because it is being
 *    destroyed.
 */

#include "config.h"

#include <clutter/clutter-pango.h>
#include <glib/gi18n-lib.h>

#include "shell-global.h"
#include "ui/main.h"
#include "ui/search-controller.h"
#include "ui/shell-entry.h"

/* ------------------------------------------------------------------------ */
/* ShellSearchFocusTrap (private)                                            */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_SEARCH_FOCUS_TRAP (shell_search_focus_trap_get_type ())
G_DECLARE_FINAL_TYPE (ShellSearchFocusTrap, shell_search_focus_trap,
                      SHELL, SEARCH_FOCUS_TRAP, StWidget)

struct _ShellSearchFocusTrap
{
  StWidget parent;
};

G_DEFINE_FINAL_TYPE (ShellSearchFocusTrap, shell_search_focus_trap, ST_TYPE_WIDGET)

static gboolean
shell_search_focus_trap_navigate_focus (StWidget        *widget,
                                        ClutterActor    *from,
                                        StDirectionType  direction)
{
  if (direction == ST_DIR_TAB_FORWARD || direction == ST_DIR_TAB_BACKWARD)
    return ST_WIDGET_CLASS (shell_search_focus_trap_parent_class)->navigate_focus (widget, from,
                                                                                   direction);

  return FALSE;
}

static void
shell_search_focus_trap_class_init (ShellSearchFocusTrapClass *klass)
{
  StWidgetClass *widget_class = ST_WIDGET_CLASS (klass);

  widget_class->navigate_focus = shell_search_focus_trap_navigate_focus;
}

static void
shell_search_focus_trap_init (ShellSearchFocusTrap *self)
{
}

/* ------------------------------------------------------------------------ */
/* ShellSearchController                                                     */
/* ------------------------------------------------------------------------ */

enum
{
  PROP_0,

  PROP_SEARCH_ENTRY,
  PROP_SHOW_APPS_BUTTON,
  PROP_CTRL_ALT_TAB_MANAGER,
  PROP_APPLICATION_PROVIDER,
  PROP_SEARCH_ACTIVE,

  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  OVERVIEW_HIDE_REQUESTED,
  OVERVIEW_TOGGLE_REQUESTED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellSearchController
{
  StWidget parent;

  StButton *show_apps_button;
  StEntry *entry;
  ClutterText *text;
  ShellCtrlAltTabManager *ctrl_alt_tab_manager;
  ShellSearchProvider *application_provider;

  ShellSearchResultsView *search_results; /* weak, a child */
  ClutterActor *focus_trap;               /* weak, a child of the results */
  ClutterActor *clear_icon;

  ClutterKeyController *search_entry_key_controller;
  ClutterKeyController *stage_key_controller;
  ClutterClickGesture *click_gesture;
  ClutterStage *stage; /* weak */
  gboolean stage_key_controller_added;
  gboolean click_gesture_added;

  gulong icon_clicked_id;
  gboolean search_active;
  gboolean entry_destroyed;
  gboolean destroyed;
};

G_DEFINE_FINAL_TYPE (ShellSearchController, shell_search_controller, ST_TYPE_WIDGET)

/* The terms of a search string: the words between white space */
static char **
get_terms_for_search_string (const char *search_string)
{
  g_autoptr (GPtrArray) terms = g_ptr_array_new_with_free_func (g_free);
  const char *word_start = NULL;
  const char *p;

  for (p = search_string; *p != '\0'; p = g_utf8_next_char (p))
    {
      if (g_unichar_isspace (g_utf8_get_char (p)))
        {
          if (word_start != NULL)
            {
              g_ptr_array_add (terms, g_strndup (word_start, p - word_start));
              word_start = NULL;
            }
        }
      else if (word_start == NULL)
        {
          word_start = p;
        }
    }

  if (word_start != NULL)
    g_ptr_array_add (terms, g_strdup (word_start));

  g_ptr_array_add (terms, NULL);
  return (char **) g_ptr_array_free (g_steal_pointer (&terms), FALSE);
}

static guint
get_modal_count (void)
{
  return shell_main_get_modal_count (shell_main_get_default ());
}

static void
set_search_active (ShellSearchController *self,
                   gboolean               search_active)
{
  if (self->search_active == search_active)
    return;

  self->search_active = search_active;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SEARCH_ACTIVE]);
}


/* The stage is held weakly and keeps its address while destroyed but not yet
 * finalized, when its actions are already gone: only remove what is still
 * attached, removing a detached action is a fatal Clutter warning. */
static void
remove_stage_action (ShellSearchController *self,
                     ClutterAction         *action)
{
  if (self->stage != NULL && action != NULL &&
      clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (action)) == CLUTTER_ACTOR (self->stage))
    clutter_actor_remove_action (CLUTTER_ACTOR (self->stage), action);
}

static void
search_cancelled (ShellSearchController *self)
{
  set_search_active (self, FALSE);

  /* Leave the entry focused when it doesn't have any text;
   * when replacing a selected search term, Clutter emits
   * two 'text-changed' signals, one for deleting the previous
   * text and one for the new one - the second one is handled
   * incorrectly when we remove focus
   * (https://bugzilla.gnome.org/show_bug.cgi?id=636341) */
  if (*clutter_text_get_text (self->text) != '\0')
    shell_search_controller_reset (self);
}

/**
 * shell_search_controller_reset:
 * @self: a #ShellSearchController
 *
 * Clears the search entry and drops the key focus (`reset ()`).
 */
void
shell_search_controller_reset (ShellSearchController *self)
{
  g_return_if_fail (SHELL_IS_SEARCH_CONTROLLER (self));

  /* The entry goes away with the overview, and is not worth resetting then */
  if (self->destroyed || self->entry_destroyed)
    return;

  /* Don't drop the key focus on Clutter's side if anything but the
   * overview has pushed a modal (e.g. system modals when activated using
   * the overview). */
  if (get_modal_count () <= 1 && self->stage != NULL)
    clutter_stage_set_key_focus (self->stage, NULL);

  st_entry_set_text (self->entry, "");

  clutter_text_set_cursor_visible (self->text, TRUE);
  clutter_text_set_selection (self->text, 0, 0);
}

/**
 * shell_search_controller_prepare_to_enter_overview:
 * @self: a #ShellSearchController
 *
 * Resets the search before the overview is shown (`prepareToEnterOverview ()`).
 */
void
shell_search_controller_prepare_to_enter_overview (ShellSearchController *self)
{
  g_return_if_fail (SHELL_IS_SEARCH_CONTROLLER (self));

  shell_search_controller_reset (self);
  set_search_active (self, FALSE);
}

/**
 * shell_search_controller_prepare_to_leave_overview:
 * @self: a #ShellSearchController
 *
 * Ends the search before the overview is hidden (`prepareToLeaveOverview ()`).
 */
void
shell_search_controller_prepare_to_leave_overview (ShellSearchController *self)
{
  g_return_if_fail (SHELL_IS_SEARCH_CONTROLLER (self));

  set_search_active (self, FALSE);
}

/**
 * shell_search_controller_overview_showing:
 * @self: a #ShellSearchController
 *
 * Makes the stage send the typed text to the search entry, and handle
 * `Escape`, while the overview is shown. Call it when the overview emits
 * `showing`.
 */
void
shell_search_controller_overview_showing (ShellSearchController *self)
{
  g_return_if_fail (SHELL_IS_SEARCH_CONTROLLER (self));

  if (self->destroyed || self->stage == NULL)
    return;

  clutter_text_set_input_interceptor (self->text, CLUTTER_ACTOR (self->stage));

  if (!self->stage_key_controller_added)
    {
      clutter_actor_add_action (CLUTTER_ACTOR (self->stage),
                                CLUTTER_ACTION (self->stage_key_controller));
      self->stage_key_controller_added = TRUE;
    }
}

/**
 * shell_search_controller_overview_hiding:
 * @self: a #ShellSearchController
 *
 * Undoes shell_search_controller_overview_showing (). Call it when the
 * overview emits `hiding`.
 */
void
shell_search_controller_overview_hiding (ShellSearchController *self)
{
  g_return_if_fail (SHELL_IS_SEARCH_CONTROLLER (self));

  if (self->destroyed)
    return;

  clutter_text_set_input_interceptor (self->text, NULL);

  if (self->stage_key_controller_added)
    remove_stage_action (self, CLUTTER_ACTION (self->stage_key_controller));
  self->stage_key_controller_added = FALSE;
}

/**
 * shell_search_controller_add_provider:
 * @self: a #ShellSearchController
 * @provider: a search provider implementation
 *
 * Adds a search provider to the controller (`addProvider ()`).
 */
void
shell_search_controller_add_provider (ShellSearchController *self,
                                      ShellSearchProvider   *provider)
{
  g_return_if_fail (SHELL_IS_SEARCH_CONTROLLER (self));

  if (self->search_results != NULL)
    shell_search_results_view_register_provider (self->search_results, provider);
}

/**
 * shell_search_controller_remove_provider:
 * @self: a #ShellSearchController
 * @provider: a search provider implementation
 *
 * Removes a search provider from the controller (`removeProvider ()`).
 */
void
shell_search_controller_remove_provider (ShellSearchController *self,
                                         ShellSearchProvider   *provider)
{
  g_return_if_fail (SHELL_IS_SEARCH_CONTROLLER (self));

  if (self->search_results != NULL)
    shell_search_results_view_unregister_provider (self->search_results, provider);
}

/**
 * shell_search_controller_get_search_active:
 * @self: a #ShellSearchController
 *
 * Returns: whether the user is searching (`searchActive`)
 */
gboolean
shell_search_controller_get_search_active (ShellSearchController *self)
{
  g_return_val_if_fail (SHELL_IS_SEARCH_CONTROLLER (self), FALSE);

  return self->search_active;
}

/**
 * shell_search_controller_get_search_results:
 * @self: a #ShellSearchController
 *
 * Returns: (transfer none) (nullable): the view of the search results
 */
ShellSearchResultsView *
shell_search_controller_get_search_results (ShellSearchController *self)
{
  g_return_val_if_fail (SHELL_IS_SEARCH_CONTROLLER (self), NULL);

  return self->search_results;
}

static void
show_apps_button_toggled_cb (GObject               *object,
                             GParamSpec            *pspec,
                             ShellSearchController *self)
{
  set_search_active (self, FALSE);
}

static gboolean
stage_key_press_cb (ClutterKeyController  *controller,
                    ShellSearchController *self)
{
  uint32_t symbol = 0;

  /* Ignore events while anything but the overview has
   * pushed a modal (system modals, looking glass, ...) */
  if (get_modal_count () > 1)
    return CLUTTER_EVENT_PROPAGATE;

  clutter_key_controller_get_key (controller, &symbol, NULL, NULL);

  if (symbol == CLUTTER_KEY_Escape)
    {
      if (self->search_active)
        shell_search_controller_reset (self);
      else if (st_button_get_checked (self->show_apps_button))
        st_button_set_checked (self->show_apps_button, FALSE);
      else
        g_signal_emit (self, signals[OVERVIEW_HIDE_REQUESTED], 0);

      return CLUTTER_EVENT_STOP;
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static void
stage_key_focus_changed_cb (GObject               *object,
                            GParamSpec            *pspec,
                            ShellSearchController *self)
{
  ClutterActor *focus = clutter_stage_get_key_focus (CLUTTER_STAGE (object));
  gboolean appear_focused;

  if (self->destroyed || self->search_results == NULL)
    return;

  appear_focused = focus != NULL &&
    (clutter_actor_contains (CLUTTER_ACTOR (self->entry), focus) ||
     clutter_actor_contains (CLUTTER_ACTOR (self->search_results), focus));

  clutter_text_set_cursor_visible (self->text, appear_focused);

  if (appear_focused)
    st_widget_add_style_pseudo_class (ST_WIDGET (self->entry), "focus");
  else
    st_widget_remove_style_pseudo_class (ST_WIDGET (self->entry), "focus");
}

static void
entry_mapped_cb (GObject               *object,
                 GParamSpec            *pspec,
                 ShellSearchController *self)
{
  if (self->destroyed || self->stage == NULL || self->click_gesture == NULL)
    return;

  if (clutter_actor_is_mapped (CLUTTER_ACTOR (self->entry)))
    {
      /* Enable 'find-as-you-type' */
      clutter_text_set_cursor_visible (self->text, TRUE);
      clutter_text_set_selection (self->text, 0, 0);

      if (!self->click_gesture_added)
        {
          clutter_actor_add_action_full (CLUTTER_ACTOR (self->stage), "reset-search",
                                         CLUTTER_PHASE_CAPTURE,
                                         CLUTTER_ACTION (self->click_gesture));
          self->click_gesture_added = TRUE;
        }
    }
  else
    {
      /* Disable 'find-as-you-type' */
      if (self->click_gesture_added)
        remove_stage_action (self, CLUTTER_ACTION (self->click_gesture));
      self->click_gesture_added = FALSE;
    }
}

static void
entry_destroy_cb (ClutterActor          *entry,
                  ShellSearchController *self)
{
  self->entry_destroyed = TRUE;
}

static void
secondary_icon_clicked_cb (StEntry               *entry,
                           ShellSearchController *self)
{
  shell_search_controller_reset (self);
}

static void
text_changed_cb (ClutterText           *text,
                 ShellSearchController *self)
{
  g_auto (GStrv) terms = get_terms_for_search_string (st_entry_get_text (self->entry));
  gboolean search_active = terms[0] != NULL;

  if (self->destroyed || self->search_results == NULL)
    return;

  shell_search_results_view_set_terms (self->search_results, (const char * const *) terms);

  if (search_active)
    {
      set_search_active (self, TRUE);

      st_entry_set_secondary_icon (self->entry, self->clear_icon);

      if (self->icon_clicked_id == 0)
        self->icon_clicked_id = g_signal_connect_object (self->entry, "secondary-icon-clicked",
                                                         G_CALLBACK (secondary_icon_clicked_cb),
                                                         self, 0);
    }
  else
    {
      if (self->icon_clicked_id > 0)
        {
          g_signal_handler_disconnect (self->entry, self->icon_clicked_id);
          self->icon_clicked_id = 0;
        }

      st_entry_set_secondary_icon (self->entry, NULL);
      search_cancelled (self);
    }
}

static void
text_key_focus_in_cb (ClutterActor          *actor,
                      ShellSearchController *self)
{
  if (self->search_results != NULL)
    shell_search_results_view_highlight_default (self->search_results, TRUE);
}

static void
text_key_focus_out_cb (ClutterActor          *actor,
                       ShellSearchController *self)
{
  if (self->search_results != NULL)
    shell_search_results_view_highlight_default (self->search_results, FALSE);
}

static void
activate_default_cb (GObject               *object,
                     ShellSearchController *self)
{
  if (self->search_results != NULL)
    shell_search_results_view_activate_default (self->search_results);
}

static void
entry_popup_menu_cb (StWidget              *widget,
                     ShellSearchController *self)
{
  ShellEntryMenu *menu;

  if (!self->search_active || self->search_results == NULL)
    return;

  menu = shell_entry_get_menu (self->entry);
  if (menu != NULL)
    shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (menu), SHELL_POPUP_ANIMATION_FULL);

  shell_search_results_view_popup_menu_default (self->search_results);
}

/* the entry does not show the hint */
static gboolean
is_activated (ShellSearchController *self)
{
  return g_strcmp0 (clutter_text_get_text (self->text), st_entry_get_text (self->entry)) == 0;
}

static gboolean
entry_key_press_cb (ClutterKeyController  *controller,
                    ShellSearchController *self)
{
  uint32_t symbol = 0;

  if (self->destroyed || self->search_results == NULL)
    return CLUTTER_EVENT_PROPAGATE;

  clutter_key_controller_get_key (controller, &symbol, NULL, NULL);

  if (symbol == CLUTTER_KEY_Escape)
    {
      if (is_activated (self))
        {
          shell_search_controller_reset (self);
          return CLUTTER_EVENT_STOP;
        }
    }
  else if (self->search_active)
    {
      uint32_t arrow_next;
      StDirectionType next_direction;

      if (clutter_actor_get_text_direction (CLUTTER_ACTOR (self->entry)) == CLUTTER_TEXT_DIRECTION_RTL)
        {
          arrow_next = CLUTTER_KEY_Left;
          next_direction = ST_DIR_LEFT;
        }
      else
        {
          arrow_next = CLUTTER_KEY_Right;
          next_direction = ST_DIR_RIGHT;
        }

      if (symbol == CLUTTER_KEY_Tab)
        {
          shell_search_results_view_navigate_from_entry (self->search_results,
                                                         ST_DIR_TAB_FORWARD);
          return CLUTTER_EVENT_STOP;
        }
      else if (symbol == CLUTTER_KEY_ISO_Left_Tab)
        {
          st_widget_set_can_focus (ST_WIDGET (self->focus_trap), FALSE);
          shell_search_results_view_navigate_from_entry (self->search_results,
                                                         ST_DIR_TAB_BACKWARD);
          st_widget_set_can_focus (ST_WIDGET (self->focus_trap), TRUE);
          return CLUTTER_EVENT_STOP;
        }
      else if (symbol == CLUTTER_KEY_Down)
        {
          shell_search_results_view_navigate_from_entry (self->search_results, ST_DIR_DOWN);
          return CLUTTER_EVENT_STOP;
        }
      else if (symbol == arrow_next && clutter_text_get_cursor_position (self->text) == -1)
        {
          shell_search_results_view_navigate_from_entry (self->search_results, next_direction);
          return CLUTTER_EVENT_STOP;
        }
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static gboolean
click_may_recognize_cb (ClutterGesture        *gesture,
                        ShellSearchController *self)
{
  const ClutterEvent *event;
  ClutterActor *target_actor;
  ShellLayoutManager *layout_manager;
  StBoxLayout *keyboard_box;

  if (self->destroyed || self->stage == NULL)
    return FALSE;

  event = clutter_gesture_get_point_event (gesture, 0);
  target_actor = clutter_stage_get_event_actor (self->stage, event);

  layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  keyboard_box = layout_manager != NULL ? shell_layout_manager_get_keyboard_box (layout_manager)
                                        : NULL;

  if (target_actor != CLUTTER_ACTOR (self->text) &&
      clutter_actor_has_key_focus (CLUTTER_ACTOR (self->text)) &&
      *clutter_text_get_text (self->text) == '\0' &&
      !clutter_text_has_preedit (self->text) &&
      (keyboard_box == NULL || target_actor == NULL ||
       !clutter_actor_contains (CLUTTER_ACTOR (keyboard_box), target_actor)))
    {
      /* the user clicked outside after activating the entry, but
       * with no search term entered and no keyboard button pressed
       * - cancel the search */
      shell_search_controller_reset (self);
    }

  return FALSE;
}

static void
focus_trap_key_focus_in_cb (ClutterActor          *actor,
                            ShellSearchController *self)
{
  clutter_actor_grab_key_focus (CLUTTER_ACTOR (self->entry));
}

static void
overview_toggle_requested_cb (ShellSearchResultsView *view,
                              ShellSearchController  *self)
{
  g_signal_emit (self, signals[OVERVIEW_TOGGLE_REQUESTED], 0);
}

static void
shell_search_controller_unmap (ClutterActor *actor)
{
  ShellSearchController *self = SHELL_SEARCH_CONTROLLER (actor);

  shell_search_controller_reset (self);

  CLUTTER_ACTOR_CLASS (shell_search_controller_parent_class)->unmap (actor);
}

static void
shell_search_controller_set_property (GObject      *object,
                                      guint         prop_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
  ShellSearchController *self = SHELL_SEARCH_CONTROLLER (object);

  switch (prop_id)
    {
    case PROP_SEARCH_ENTRY:
      g_set_object (&self->entry, g_value_get_object (value));
      break;

    case PROP_SHOW_APPS_BUTTON:
      g_set_object (&self->show_apps_button, g_value_get_object (value));
      break;

    case PROP_CTRL_ALT_TAB_MANAGER:
      g_set_object (&self->ctrl_alt_tab_manager, g_value_get_object (value));
      break;

    case PROP_APPLICATION_PROVIDER:
      g_set_object (&self->application_provider, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_search_controller_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  ShellSearchController *self = SHELL_SEARCH_CONTROLLER (object);

  switch (prop_id)
    {
    case PROP_SEARCH_ENTRY:
      g_value_set_object (value, self->entry);
      break;

    case PROP_SHOW_APPS_BUTTON:
      g_value_set_object (value, self->show_apps_button);
      break;

    case PROP_CTRL_ALT_TAB_MANAGER:
      g_value_set_object (value, self->ctrl_alt_tab_manager);
      break;

    case PROP_APPLICATION_PROVIDER:
      g_value_set_object (value, self->application_provider);
      break;

    case PROP_SEARCH_ACTIVE:
      g_value_set_boolean (value, self->search_active);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_search_controller_constructed (GObject *object)
{
  ShellSearchController *self = SHELL_SEARCH_CONTROLLER (object);
  ShellGlobal *global = shell_global_get ();
  ClutterActor *primary_icon;
  ClutterActor *stage;
  ClutterActor *focus_trap;
  ShellSearchResultsView *results;

  G_OBJECT_CLASS (shell_search_controller_parent_class)->constructed (object);

  g_return_if_fail (ST_IS_ENTRY (self->entry));
  g_return_if_fail (ST_IS_BUTTON (self->show_apps_button));

  clutter_actor_set_name (CLUTTER_ACTOR (self), "searchController");
  clutter_actor_set_layout_manager (CLUTTER_ACTOR (self), clutter_bin_layout_new ());
  clutter_actor_set_x_expand (CLUTTER_ACTOR (self), TRUE);
  clutter_actor_set_y_expand (CLUTTER_ACTOR (self), TRUE);
  clutter_actor_hide (CLUTTER_ACTOR (self));

  g_signal_connect_object (self->show_apps_button, "notify::checked",
                           G_CALLBACK (show_apps_button_toggled_cb), self, 0);

  shell_entry_add_context_menu (self->entry, SHELL_ACTION_MODE_POPUP);

  self->text = g_object_ref (st_entry_get_clutter_text (self->entry));
  g_signal_connect_object (self->text, "text-changed",
                           G_CALLBACK (text_changed_cb), self, 0);
  g_signal_connect_object (self->text, "key-focus-in",
                           G_CALLBACK (text_key_focus_in_cb), self, 0);
  g_signal_connect_object (self->text, "key-focus-out",
                           G_CALLBACK (text_key_focus_out_cb), self, 0);
  g_signal_connect_object (self->text, "activate",
                           G_CALLBACK (activate_default_cb), self, 0);
  g_signal_connect_object (self->entry, "activate-new-instance",
                           G_CALLBACK (activate_default_cb), self, 0);
  g_signal_connect_object (self->entry, "popup-menu",
                           G_CALLBACK (entry_popup_menu_cb), self, 0);
  g_signal_connect_object (self->entry, "notify::mapped",
                           G_CALLBACK (entry_mapped_cb), self, 0);
  g_signal_connect_object (self->entry, "destroy",
                           G_CALLBACK (entry_destroy_cb), self, 0);

  stage = CLUTTER_ACTOR (shell_global_get_stage (global));
  g_set_weak_pointer (&self->stage, CLUTTER_STAGE (stage));
  g_signal_connect_object (stage, "notify::key-focus",
                           G_CALLBACK (stage_key_focus_changed_cb), self, 0);

  primary_icon = g_object_new (ST_TYPE_ICON,
                               "style-class", "search-entry-icon",
                               "icon-name", "edit-find-symbolic",
                               NULL);
  st_entry_set_primary_icon (self->entry, primary_icon);

  self->clear_icon = g_object_ref_sink (g_object_new (ST_TYPE_ICON,
                                                      "style-class", "search-entry-icon",
                                                      "icon-name", "edit-clear-symbolic",
                                                      NULL));

  self->search_entry_key_controller =
    CLUTTER_KEY_CONTROLLER (g_object_ref_sink (clutter_key_controller_new (NULL)));
  g_signal_connect_object (self->search_entry_key_controller, "key-press",
                           G_CALLBACK (entry_key_press_cb), self, 0);
  clutter_actor_add_action_full (CLUTTER_ACTOR (self->entry), "search-key-selection",
                                 CLUTTER_PHASE_CAPTURE,
                                 CLUTTER_ACTION (self->search_entry_key_controller));

  results = shell_search_results_view_new (self->application_provider);
  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (results));
  g_set_weak_pointer (&self->search_results, results);
  g_signal_connect_object (results, "overview-toggle-requested",
                           G_CALLBACK (overview_toggle_requested_cb), self, 0);

  if (self->ctrl_alt_tab_manager != NULL)
    shell_ctrl_alt_tab_manager_add_group (self->ctrl_alt_tab_manager,
                                          CLUTTER_ACTOR (self->entry),
                                          _("Search"),
                                          "shell-focus-search-symbolic");

  /* Since the entry isn't inside the results container we install this
   * dummy widget as the last results container child so that we can
   * include the entry in the keynav tab path */
  focus_trap = g_object_new (SHELL_TYPE_SEARCH_FOCUS_TRAP,
                             "can-focus", TRUE,
                             NULL);
  g_signal_connect_object (focus_trap, "key-focus-in",
                           G_CALLBACK (focus_trap_key_focus_in_cb), self, 0);
  clutter_actor_add_child (CLUTTER_ACTOR (self->search_results), focus_trap);
  g_set_weak_pointer (&self->focus_trap, focus_trap);

  st_focus_manager_add_group (shell_global_get_focus_manager (global),
                              ST_WIDGET (self->search_results));

  self->stage_key_controller =
    CLUTTER_KEY_CONTROLLER (g_object_ref_sink (clutter_key_controller_new (NULL)));
  g_signal_connect_object (self->stage_key_controller, "key-press",
                           G_CALLBACK (stage_key_press_cb), self, 0);

  /* Will be connected to the stage when active */
  self->click_gesture = CLUTTER_CLICK_GESTURE (g_object_ref_sink (clutter_click_gesture_new ()));
  clutter_click_gesture_set_recognize_on_press (self->click_gesture, TRUE);
  g_signal_connect_object (self->click_gesture, "may-recognize",
                           G_CALLBACK (click_may_recognize_cb), self, 0);
}

static void
shell_search_controller_dispose (GObject *object)
{
  ShellSearchController *self = SHELL_SEARCH_CONTROLLER (object);

  self->destroyed = TRUE;

  if (self->stage_key_controller_added)
    remove_stage_action (self, CLUTTER_ACTION (self->stage_key_controller));
  if (self->click_gesture_added)
    remove_stage_action (self, CLUTTER_ACTION (self->click_gesture));
  self->stage_key_controller_added = FALSE;
  self->click_gesture_added = FALSE;

  g_clear_weak_pointer (&self->stage);
  g_clear_weak_pointer (&self->search_results);
  g_clear_weak_pointer (&self->focus_trap);

  g_clear_object (&self->stage_key_controller);
  g_clear_object (&self->search_entry_key_controller);
  g_clear_object (&self->click_gesture);
  g_clear_object (&self->clear_icon);
  g_clear_object (&self->text);
  g_clear_object (&self->entry);
  g_clear_object (&self->show_apps_button);
  g_clear_object (&self->ctrl_alt_tab_manager);
  g_clear_object (&self->application_provider);

  G_OBJECT_CLASS (shell_search_controller_parent_class)->dispose (object);
}

static void
shell_search_controller_class_init (ShellSearchControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_search_controller_constructed;
  object_class->set_property = shell_search_controller_set_property;
  object_class->get_property = shell_search_controller_get_property;
  object_class->dispose = shell_search_controller_dispose;

  actor_class->unmap = shell_search_controller_unmap;

  /**
   * ShellSearchController:search-entry:
   *
   * The entry the user types the search in (`searchEntry`).
   */
  props[PROP_SEARCH_ENTRY] =
    g_param_spec_object ("search-entry", NULL, NULL,
                         ST_TYPE_ENTRY,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellSearchController:show-apps-button:
   *
   * The button of the dash which shows the app grid. Showing the grid ends
   * the search, and `Escape` unchecks the button before it hides the
   * overview (`showAppsButton`).
   */
  props[PROP_SHOW_APPS_BUTTON] =
    g_param_spec_object ("show-apps-button", NULL, NULL,
                         ST_TYPE_BUTTON,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellSearchController:ctrl-alt-tab-manager:
   *
   * The manager the search entry is added to as a keyboard navigation group
   * (`Main.ctrlAltTabManager`).
   */
  props[PROP_CTRL_ALT_TAB_MANAGER] =
    g_param_spec_object ("ctrl-alt-tab-manager", NULL, NULL,
                         SHELL_TYPE_CTRL_ALT_TAB_MANAGER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellSearchController:application-provider:
   *
   * The provider of the applications, which is searched first
   * (#ShellAppSearchProvider, `AppDisplay.AppSearchProvider`).
   */
  props[PROP_APPLICATION_PROVIDER] =
    g_param_spec_object ("application-provider", NULL, NULL,
                         SHELL_TYPE_SEARCH_PROVIDER,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  /**
   * ShellSearchController:search-active:
   *
   * Whether the user is searching (`search-active`).
   */
  props[PROP_SEARCH_ACTIVE] =
    g_param_spec_boolean ("search-active", NULL, NULL, FALSE,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellSearchController::overview-hide-requested:
   * @self: the controller
   *
   * Emitted when `Escape` is pressed while there is nothing to cancel, and
   * the overview should be hidden (`Main.overview.hide ()`).
   */
  signals[OVERVIEW_HIDE_REQUESTED] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellSearchController::overview-toggle-requested:
   * @self: the controller
   *
   * Emitted when a result was activated, or the search of a provider
   * launched, and the overview should be toggled (`Main.overview.toggle ()`).
   */
  signals[OVERVIEW_TOGGLE_REQUESTED] =
    g_signal_new ("overview-toggle-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_search_controller_init (ShellSearchController *self)
{
}

/**
 * shell_search_controller_new:
 * @search_entry: the entry the user types the search in
 * @show_apps_button: the button of the dash which shows the app grid
 * @ctrl_alt_tab_manager: (nullable): the manager to add the search entry to
 *   as a keyboard navigation group
 * @application_provider: (nullable): the provider of the applications
 *
 * Creates the controller of the search (`new SearchController (searchEntry,
 * showAppsButton)`). It is hidden until the overview shows it.
 *
 * Returns: (transfer full): a new #ShellSearchController
 */
ShellSearchController *
shell_search_controller_new (StEntry                *search_entry,
                             StButton               *show_apps_button,
                             ShellCtrlAltTabManager *ctrl_alt_tab_manager,
                             ShellSearchProvider    *application_provider)
{
  return g_object_new (SHELL_TYPE_SEARCH_CONTROLLER,
                       "search-entry", search_entry,
                       "show-apps-button", show_apps_button,
                       "ctrl-alt-tab-manager", ctrl_alt_tab_manager,
                       "application-provider", application_provider,
                       NULL);
}
