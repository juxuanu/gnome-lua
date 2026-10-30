/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * login-dialog.c: The login screen: the list of users, the prompt, the
 * banner, the logo and the buttons of the bottom. Port of
 * js/gdm/loginDialog.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - `UserListItem`, `UserList` and `A11yMenuButton` are private types.
 *  - A failure to talk to the greeter, to the login manager or to read the
 *    banner is a warning or a message, the JS logged the exception.
 *  - The actions that restart the timed login on user interaction are kept
 *    and removed from the stage when the dialog is disposed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <act/act.h>
#include <atk/atk.h>
#include <gdm/gdm-client.h>
#include <gdm/gdm-sessions.h>
#include <glib/gi18n-lib.h>
#include <math.h>
#include <pango/pango.h>
#include <clutter/clutter-pango.h>

#include "gdm/auth-menu-button.h"
#include "gdm/auth-prompt.h"
#include "gdm/batch.h"
#include "gdm/conflicting-session-dialog.h"
#include "gdm/login-dialog.h"
#include "gdm/realmd.h"
#include "gdm/settings.h"
#include "gdm/util.h"
#include "misc/login-manager.h"
#include "shell-action-modes.h"
#include "shell-global.h"
#include "ui/ctrl-alt-tab.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/message-tray.h"
#include "ui/popup-menu.h"
#include "ui/quick-settings.h"
#include "ui/status/accessibility.h"
#include "ui/user-widget.h"
#include "ui/window-manager.h"

#define FADE_ANIMATION_TIME 250
#define SCROLL_ANIMATION_TIME 500
#define AUTH_PROMPT_VERTICAL_CENTER_OFFSET 50
#define TIMED_LOGIN_IDLE_THRESHOLD 5.0
#define CONFLICTING_SESSION_DIALOG_TIMEOUT 60

#define N_A11Y_MENU_COLUMNS 2

/* The names of the sections of the menu are translated, so they cannot be
 * constants */
static const char *
primary_login_method_section_name (void)
{
  return _("Login Options");
}

static const char *
session_type_section_name (void)
{
  return _("Session Type");
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static ShellEaseParams
ease_params (int                  duration,
             int                  delay,
             ClutterAnimationMode mode)
{
  ShellEaseParams params = {
    .duration = duration,
    .delay = delay,
    .mode = mode,
    .repeat_count = 0,
    .auto_reverse = FALSE,
    .animation_required = FALSE,
  };

  return params;
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

static ClutterActor *
get_ui_group (void)
{
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());

  if (layout_manager == NULL)
    return NULL;

  return CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager));
}

/* `{sectionName: name}`, a floating reference */
static GVariant *
section_criteria (const char *section_name)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "sectionName", g_variant_new_string (section_name));
  return g_variant_builder_end (&builder);
}

/* `{sectionName, ...mechanism}`, a floating reference */
static GVariant *
mechanism_item (const char *section_name,
                GVariant   *mechanism)
{
  GVariantBuilder builder;
  GVariantIter iter;
  const char *key;
  GVariant *value;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "sectionName", g_variant_new_string (section_name));

  g_variant_iter_init (&iter, mechanism);
  while (g_variant_iter_next (&iter, "{&sv}", &key, &value))
    {
      g_autoptr (GVariant) owned = value;

      if (g_str_equal (key, "sectionName"))
        continue;

      g_variant_builder_add (&builder, "{sv}", key, owned);
    }

  return g_variant_builder_end (&builder);
}

/* ------------------------------------------------------------------------ */
/* ShellUserListItem                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_USER_LIST_ITEM (shell_user_list_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellUserListItem, shell_user_list_item,
                      SHELL, USER_LIST_ITEM, StButton)

/* `UserListItem` */
struct _ShellUserListItem
{
  StButton parent;

  ActUser *user;
  ClutterActor *timed_login_indicator;
  guint timed_login_timeout_id;
};

G_DEFINE_FINAL_TYPE (ShellUserListItem, shell_user_list_item, ST_TYPE_BUTTON)

enum
{
  ITEM_ACTIVATE,
  N_ITEM_SIGNALS
};

static guint item_signals[N_ITEM_SIGNALS] = { 0 };

static void
user_list_item_set_selected (ShellUserListItem *self,
                             gboolean           selected)
{
  if (selected)
    {
      st_widget_add_style_pseudo_class (ST_WIDGET (self), "selected");
      clutter_actor_grab_key_focus (CLUTTER_ACTOR (self));
    }
  else
    {
      st_widget_remove_style_pseudo_class (ST_WIDGET (self), "selected");
    }
}

static void
user_list_item_hover_cb (GObject           *object,
                         GParamSpec        *pspec,
                         ShellUserListItem *self)
{
  user_list_item_set_selected (self, st_widget_get_hover (ST_WIDGET (self)));
}

/* `_updateLoggedIn ()` */
static void
user_list_item_user_changed_cb (ActUser           *user,
                                ShellUserListItem *self)
{
  if (act_user_is_logged_in (user))
    st_widget_add_style_pseudo_class (ST_WIDGET (self), "logged-in");
  else
    st_widget_remove_style_pseudo_class (ST_WIDGET (self), "logged-in");
}

static void
shell_user_list_item_key_focus_in (ClutterActor *actor)
{
  CLUTTER_ACTOR_CLASS (shell_user_list_item_parent_class)->key_focus_in (actor);

  user_list_item_set_selected (SHELL_USER_LIST_ITEM (actor), TRUE);
}

static void
shell_user_list_item_key_focus_out (ClutterActor *actor)
{
  CLUTTER_ACTOR_CLASS (shell_user_list_item_parent_class)->key_focus_out (actor);

  user_list_item_set_selected (SHELL_USER_LIST_ITEM (actor), FALSE);
}

static void
shell_user_list_item_clicked (StButton *button,
                              int       clicked_button)
{
  g_signal_emit (button, item_signals[ITEM_ACTIVATE], 0);
}

static void
shell_user_list_item_dispose (GObject *object)
{
  ShellUserListItem *self = SHELL_USER_LIST_ITEM (object);

  if (self->timed_login_timeout_id != 0)
    {
      g_source_remove (self->timed_login_timeout_id);
      self->timed_login_timeout_id = 0;
    }

  self->timed_login_indicator = NULL;
  g_clear_object (&self->user);

  G_OBJECT_CLASS (shell_user_list_item_parent_class)->dispose (object);
}

static void
shell_user_list_item_class_init (ShellUserListItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->dispose = shell_user_list_item_dispose;

  actor_class->key_focus_in = shell_user_list_item_key_focus_in;
  actor_class->key_focus_out = shell_user_list_item_key_focus_out;

  button_class->clicked = shell_user_list_item_clicked;

  /**
   * ShellUserListItem::activate:
   * @self: the #ShellUserListItem
   *
   * Emitted when the user clicks the item.
   */
  item_signals[ITEM_ACTIVATE] =
    g_signal_new ("activate",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_user_list_item_init (ShellUserListItem *self)
{
}

/* `new UserListItem (user)` */
static ShellUserListItem *
shell_user_list_item_new (ActUser *user)
{
  ShellUserListItem *self;
  ClutterActor *layout;
  ShellUserWidget *user_widget;

  layout = g_object_new (ST_TYPE_BOX_LAYOUT,
                         "orientation", CLUTTER_ORIENTATION_VERTICAL,
                         "x-expand", TRUE,
                         NULL);
  self = g_object_new (SHELL_TYPE_USER_LIST_ITEM,
                       "style-class", "login-dialog-user-list-item",
                       "button-mask", ST_BUTTON_PRIMARY | ST_BUTTON_SECONDARY,
                       "can-focus", TRUE,
                       "x-expand", TRUE,
                       "child", layout,
                       "reactive", TRUE,
                       NULL);

  self->user = g_object_ref (user);
  g_signal_connect_object (user, "changed",
                           G_CALLBACK (user_list_item_user_changed_cb), self, 0);

  g_signal_connect (self, "notify::hover", G_CALLBACK (user_list_item_hover_cb), self);

  user_widget = shell_user_widget_new (user, CLUTTER_ORIENTATION_HORIZONTAL);
  clutter_actor_add_child (layout, CLUTTER_ACTOR (user_widget));

  g_object_bind_property (user_widget, "label-actor",
                          self, "label-actor",
                          G_BINDING_SYNC_CREATE);

  self->timed_login_indicator = g_object_new (ST_TYPE_BIN,
                                              "style-class", "login-dialog-timed-login-indicator",
                                              "scale-x", 0.0,
                                              "visible", FALSE,
                                              NULL);
  clutter_actor_add_child (layout, self->timed_login_indicator);

  user_list_item_user_changed_cb (user, self);

  return self;
}

typedef struct
{
  ShellBatchHold *hold;
  gint64 start_time;
  double time;
  ShellUserListItem *item;     /* weak */
} ItemTimedLoginData;

static void
item_timed_login_data_free (ItemTimedLoginData *data)
{
  g_clear_object (&data->hold);
  g_clear_weak_pointer (&data->item);
  g_free (data);
}

static gboolean
item_timed_login_timeout_cb (gpointer user_data)
{
  ItemTimedLoginData *data = user_data;
  ShellUserListItem *self = data->item;
  double elapsed_time = (g_get_monotonic_time () - data->start_time) / (double) G_USEC_PER_SEC;

  if (self == NULL)
    return G_SOURCE_REMOVE;

  clutter_actor_set_scale (self->timed_login_indicator, elapsed_time / data->time, 1.0);

  if (elapsed_time >= data->time)
    {
      g_autoptr (ShellBatchHold) hold = g_object_ref (data->hold);

      self->timed_login_timeout_id = 0;
      shell_batch_hold_release (hold);
      return G_SOURCE_REMOVE;
    }

  return G_SOURCE_CONTINUE;
}

/* `hideTimedLoginIndicator ()` */
static void
shell_user_list_item_hide_timed_login_indicator (ShellUserListItem *self)
{
  if (self->timed_login_timeout_id != 0)
    {
      g_source_remove (self->timed_login_timeout_id);
      self->timed_login_timeout_id = 0;
    }

  if (self->timed_login_indicator == NULL)
    return;

  clutter_actor_hide (self->timed_login_indicator);
  clutter_actor_set_scale (self->timed_login_indicator, 0.0, 1.0);
}

/* `showTimedLoginIndicator (time)`; the hold is kept alive until the
 * indicator is full or hidden */
static ShellBatchHold *
shell_user_list_item_show_timed_login_indicator (ShellUserListItem *self,
                                                 double             time)
{
  ItemTimedLoginData *data;
  ShellBatchHold *hold = shell_batch_hold_new ();

  shell_user_list_item_hide_timed_login_indicator (self);

  clutter_actor_show (self->timed_login_indicator);

  data = g_new0 (ItemTimedLoginData, 1);
  data->hold = hold;
  data->start_time = g_get_monotonic_time ();
  data->time = time;
  g_set_weak_pointer (&data->item, self);

  self->timed_login_timeout_id = g_timeout_add_full (G_PRIORITY_DEFAULT, 33,
                                                     item_timed_login_timeout_cb, data,
                                                     (GDestroyNotify) item_timed_login_data_free);
  g_source_set_name_by_id (self->timed_login_timeout_id,
                           "[gnome-shell] this._timedLoginTimeoutId");

  return hold;
}

/* ------------------------------------------------------------------------ */
/* ShellUserList                                                             */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_USER_LIST (shell_user_list_get_type ())
G_DECLARE_FINAL_TYPE (ShellUserList, shell_user_list,
                      SHELL, USER_LIST, StScrollView)

/* `UserList` */
struct _ShellUserList
{
  StScrollView parent;

  StBoxLayout *box;
  GHashTable *items;     /* user name -> ShellUserListItem (owned) */

  MetaLaters *laters;    /* weak, where move_focus_later was added */
  guint move_focus_later;
};

G_DEFINE_FINAL_TYPE (ShellUserList, shell_user_list, ST_TYPE_SCROLL_VIEW)

enum
{
  LIST_ACTIVATE,
  LIST_ITEM_ADDED,
  N_LIST_SIGNALS
};

static guint list_signals[N_LIST_SIGNALS] = { 0 };

static void user_list_move_focus_to_items (ShellUserList *self);

static gboolean
user_list_move_focus_later_cb (gpointer user_data)
{
  ShellUserList *self = SHELL_USER_LIST (user_data);

  self->move_focus_later = 0;
  user_list_move_focus_to_items (self);

  return G_SOURCE_REMOVE;
}

/* `_moveFocusToItems ()` */
static void
user_list_move_focus_to_items (ShellUserList *self)
{
  ShellGlobal *global = shell_global_get ();
  MetaLaters *laters;

  if (g_hash_table_size (self->items) == 0)
    return;

  if (clutter_stage_get_key_focus (shell_global_get_stage (global)) != CLUTTER_ACTOR (self))
    return;

  if (st_widget_navigate_focus (ST_WIDGET (self), NULL, ST_DIR_TAB_FORWARD, FALSE))
    return;

  if (self->move_focus_later != 0)
    return;

  laters = meta_compositor_get_laters (shell_global_get_compositor (global));
  g_set_weak_pointer (&self->laters, laters);
  self->move_focus_later = meta_laters_add (laters, META_LATER_BEFORE_REDRAW,
                                            user_list_move_focus_later_cb, self, NULL);
}

static void
shell_user_list_key_focus_in (ClutterActor *actor)
{
  CLUTTER_ACTOR_CLASS (shell_user_list_parent_class)->key_focus_in (actor);

  user_list_move_focus_to_items (SHELL_USER_LIST (actor));
}

/* Eases the adjustment of the list to the middle of @item */
static double
user_list_item_target (ShellUserList *self,
                       ClutterActor  *item)
{
  StAdjustment *adjustment = st_scroll_view_get_vadjustment (ST_SCROLL_VIEW (self));
  ClutterActorBox box;
  double step_increment, page_size;

  clutter_actor_get_allocation_box (item, &box);
  st_adjustment_get_values (adjustment, NULL, NULL, NULL, &step_increment, NULL, &page_size);

  return (box.y1 + step_increment / 2.0) - (page_size / 2.0);
}

/* `scrollToItem ()` */
static void
shell_user_list_scroll_to_item (ShellUserList *self,
                                ClutterActor  *item)
{
  StAdjustment *adjustment = st_scroll_view_get_vadjustment (ST_SCROLL_VIEW (self));
  ShellEaseParams params = ease_params (SCROLL_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);
  g_auto (GValue) target = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;

  g_value_init (&target, G_TYPE_DOUBLE);
  g_value_set_double (&target, user_list_item_target (self, item));

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (adjustment), "value",
                                       &target, &params, NULL, NULL, NULL, &error))
    g_debug ("%s: %s", G_STRFUNC, error->message);
}

/* `jumpToItem ()` */
static void
shell_user_list_jump_to_item (ShellUserList *self,
                              ClutterActor  *item)
{
  StAdjustment *adjustment = st_scroll_view_get_vadjustment (ST_SCROLL_VIEW (self));

  st_adjustment_set_value (adjustment, user_list_item_target (self, item));
}

static void
user_list_item_activate_cb (ShellUserListItem *item,
                            ShellUserList     *self)
{
  g_signal_emit (self, list_signals[LIST_ACTIVATE], 0, item);
}

static void
user_list_item_key_focus_in_cb (ClutterActor  *item,
                                ShellUserList *self)
{
  /* Try to keep the focused item front-and-center */
  shell_user_list_scroll_to_item (self, item);
}

/* `getItemFromUserName ()` */
static ShellUserListItem *
shell_user_list_get_item_from_user_name (ShellUserList *self,
                                         const char    *user_name)
{
  return g_hash_table_lookup (self->items, user_name);
}

/* `containsUser ()` */
static gboolean
shell_user_list_contains_user (ShellUserList *self,
                               ActUser       *user)
{
  const char *user_name = act_user_get_user_name (user);

  return user_name != NULL && g_hash_table_contains (self->items, user_name);
}

/* `removeUser ()` */
static void
shell_user_list_remove_user (ShellUserList *self,
                             ActUser       *user)
{
  const char *user_name;
  ShellUserListItem *item;

  if (!act_user_is_loaded (user))
    return;

  user_name = act_user_get_user_name (user);
  if (user_name == NULL)
    return;

  item = g_hash_table_lookup (self->items, user_name);
  if (item == NULL)
    return;

  g_object_ref (item);
  g_hash_table_remove (self->items, user_name);
  clutter_actor_destroy (CLUTTER_ACTOR (item));
  g_object_unref (item);
}

/* `addUser ()` */
static void
shell_user_list_add_user (ShellUserList *self,
                          ActUser       *user)
{
  const char *user_name;
  ShellUserListItem *item;

  if (!act_user_is_loaded (user))
    return;

  if (act_user_is_system_account (user))
    return;

  if (act_user_get_locked (user))
    return;

  user_name = act_user_get_user_name (user);
  if (user_name == NULL || user_name[0] == '\0')
    return;

  shell_user_list_remove_user (self, user);

  item = shell_user_list_item_new (user);
  clutter_actor_add_child (CLUTTER_ACTOR (self->box), CLUTTER_ACTOR (item));

  g_hash_table_insert (self->items, g_strdup (user_name), g_object_ref (item));

  g_signal_connect (item, "activate", G_CALLBACK (user_list_item_activate_cb), self);
  g_signal_connect (item, "key-focus-in", G_CALLBACK (user_list_item_key_focus_in_cb), self);

  user_list_move_focus_to_items (self);

  g_signal_emit (self, list_signals[LIST_ITEM_ADDED], 0, item);
}

/* `updateStyle ()` */
static void
shell_user_list_update_style (ShellUserList *self,
                              gboolean       is_expanded)
{
  if (is_expanded)
    st_widget_add_style_pseudo_class (ST_WIDGET (self->box), "expanded");
  else
    st_widget_remove_style_pseudo_class (ST_WIDGET (self->box), "expanded");
}

static void
shell_user_list_constructed (GObject *object)
{
  ShellUserList *self = SHELL_USER_LIST (object);

  G_OBJECT_CLASS (shell_user_list_parent_class)->constructed (object);

  self->box = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "orientation", CLUTTER_ORIENTATION_VERTICAL,
                            "style-class", "login-dialog-user-list",
                            "pseudo-class", "expanded",
                            NULL);
  st_scroll_view_set_child (ST_SCROLL_VIEW (self), ST_SCROLLABLE (self->box));
}

static void
shell_user_list_dispose (GObject *object)
{
  ShellUserList *self = SHELL_USER_LIST (object);

  if (self->move_focus_later != 0 && self->laters != NULL)
    meta_laters_remove (self->laters, self->move_focus_later);
  self->move_focus_later = 0;
  g_clear_weak_pointer (&self->laters);

  if (self->items != NULL)
    g_hash_table_remove_all (self->items);
  self->box = NULL;

  G_OBJECT_CLASS (shell_user_list_parent_class)->dispose (object);
}

static void
shell_user_list_finalize (GObject *object)
{
  ShellUserList *self = SHELL_USER_LIST (object);

  g_clear_pointer (&self->items, g_hash_table_unref);

  G_OBJECT_CLASS (shell_user_list_parent_class)->finalize (object);
}

static void
shell_user_list_class_init (ShellUserListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_user_list_constructed;
  object_class->dispose = shell_user_list_dispose;
  object_class->finalize = shell_user_list_finalize;

  actor_class->key_focus_in = shell_user_list_key_focus_in;

  /**
   * ShellUserList::activate:
   * @self: the #ShellUserList
   * @item: the item the user activated
   *
   * Emitted when the user activates an item.
   */
  list_signals[LIST_ACTIVATE] =
    g_signal_new ("activate",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_USER_LIST_ITEM);

  /**
   * ShellUserList::item-added:
   * @self: the #ShellUserList
   * @item: the new item
   *
   * Emitted when an item was added.
   */
  list_signals[LIST_ITEM_ADDED] =
    g_signal_new ("item-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  SHELL_TYPE_USER_LIST_ITEM);
}

static void
shell_user_list_init (ShellUserList *self)
{
  self->items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
}

static ShellUserList *
shell_user_list_new (void)
{
  return g_object_new (SHELL_TYPE_USER_LIST,
                       "style-class", "login-dialog-user-list-view",
                       "x-expand", TRUE,
                       "y-expand", TRUE,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellA11yMenuButton                                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_A11Y_MENU_BUTTON (shell_a11y_menu_button_get_type ())
G_DECLARE_FINAL_TYPE (ShellA11yMenuButton, shell_a11y_menu_button,
                      SHELL, A11Y_MENU_BUTTON, StButton)

/* `A11yMenuButton`: the accessibility menu of the login screen */
struct _ShellA11yMenuButton
{
  StButton parent;

  ShellQuickSettingsMenu *menu;
  ShellPopupMenuManager *manager;
};

G_DEFINE_FINAL_TYPE (ShellA11yMenuButton, shell_a11y_menu_button, ST_TYPE_BUTTON)

static void
a11y_menu_open_state_changed_cb (ShellPopupMenuBase *menu,
                                 gboolean            is_open,
                                 ShellA11yMenuButton *self)
{
  if (is_open)
    st_widget_add_style_pseudo_class (ST_WIDGET (self), "active");
  else
    st_widget_remove_style_pseudo_class (ST_WIDGET (self), "active");
}

static void
a11y_menu_button_clicked_cb (StButton            *button,
                             int                  clicked_button,
                             ShellA11yMenuButton *self)
{
  shell_popup_menu_base_toggle (SHELL_POPUP_MENU_BASE (self->menu));
}

static void
a11y_menu_button_destroy_cb (ClutterActor        *actor,
                             ShellA11yMenuButton *self)
{
  if (self->menu != NULL)
    shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (self->menu));
}

static void
shell_a11y_menu_button_dispose (GObject *object)
{
  ShellA11yMenuButton *self = SHELL_A11Y_MENU_BUTTON (object);

  G_OBJECT_CLASS (shell_a11y_menu_button_parent_class)->dispose (object);

  /* Clear after the chain-up: ClutterActor's dispose emits ::destroy, whose
   * handler (a11y_menu_button_destroy_cb ()) destroys the menu */
  g_clear_object (&self->manager);
  g_clear_object (&self->menu);
}

static void
shell_a11y_menu_button_class_init (ShellA11yMenuButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_a11y_menu_button_dispose;
}

static void
shell_a11y_menu_button_init (ShellA11yMenuButton *self)
{
}

static ClutterActor *
shell_a11y_menu_button_new (void)
{
  ShellA11yMenuButton *self;
  ShellPopupMenuBase *menu;
  ClutterActor *menu_actor;
  ClutterActor *ui_group = get_ui_group ();

  self = g_object_new (SHELL_TYPE_A11Y_MENU_BUTTON,
                       "style-class", "login-dialog-button a11y-button",
                       "icon-name", "org.gnome.Settings-accessibility-symbolic",
                       "accessible-name", _("Accessibility"),
                       "accessible-role", ATK_ROLE_MENU,
                       "button-mask", ST_BUTTON_PRIMARY | ST_BUTTON_SECONDARY,
                       "can-focus", TRUE,
                       NULL);

  self->menu = shell_quick_settings_menu_new (CLUTTER_ACTOR (self), N_A11Y_MENU_COLUMNS);
  menu = SHELL_POPUP_MENU_BASE (self->menu);

  shell_quick_settings_menu_add_item (self->menu, CLUTTER_ACTOR (shell_high_contrast_toggle_new ()), 1);
  shell_quick_settings_menu_add_item (self->menu, CLUTTER_ACTOR (shell_magnifier_toggle_new ()), 1);
  shell_quick_settings_menu_add_item (self->menu, CLUTTER_ACTOR (shell_large_text_toggle_new ()), 1);
  shell_quick_settings_menu_add_item (self->menu, CLUTTER_ACTOR (shell_screen_reader_toggle_new ()), 1);
  shell_quick_settings_menu_add_item (self->menu, CLUTTER_ACTOR (shell_screen_keyboard_toggle_new ()), 1);
  shell_quick_settings_menu_add_item (self->menu, CLUTTER_ACTOR (shell_visual_bell_toggle_new ()), 1);
  shell_quick_settings_menu_add_item (self->menu, CLUTTER_ACTOR (shell_sticky_keys_toggle_new ()), 1);
  shell_quick_settings_menu_add_item (self->menu, CLUTTER_ACTOR (shell_slow_keys_toggle_new ()), 1);
  shell_quick_settings_menu_add_item (self->menu, CLUTTER_ACTOR (shell_bounce_keys_toggle_new ()), 1);
  shell_quick_settings_menu_add_item (self->menu, CLUTTER_ACTOR (shell_mouse_keys_toggle_new ()), 1);

  menu_actor = shell_popup_menu_base_get_actor (menu);
  if (ui_group != NULL)
    clutter_actor_add_child (ui_group, menu_actor);
  clutter_actor_hide (menu_actor);

  g_signal_connect (menu, "open-state-changed",
                    G_CALLBACK (a11y_menu_open_state_changed_cb), self);
  st_focus_manager_add_group (shell_global_get_focus_manager (shell_global_get ()),
                              ST_WIDGET (menu_actor));

  self->manager = shell_popup_menu_manager_new (SHELL_ACTION_MODE_NONE);
  shell_popup_menu_manager_add_menu (self->manager, G_OBJECT (self->menu));

  g_signal_connect (self, "clicked", G_CALLBACK (a11y_menu_button_clicked_cb), self);
  g_signal_connect (self, "destroy", G_CALLBACK (a11y_menu_button_destroy_cb), self);

  return CLUTTER_ACTOR (self);
}

/* ------------------------------------------------------------------------ */
/* ShellLoginDialog                                                          */
/* ------------------------------------------------------------------------ */

/**
 * ShellLoginDialog:
 *
 * The login screen (`LoginDialog`).
 */
struct _ShellLoginDialog
{
  StWidget parent;

  ActUserManager *user_manager;
  GdmClient *gdm_client;
  GSettings *settings;
  StTextureCache *texture_cache;

  ClutterActor *user_selection_box;
  ShellUserList *user_list;
  ShellAuthPrompt *auth_prompt;
  ClutterActor *not_listed_button;
  ClutterActor *banner_view;
  StLabel *banner_label;
  ClutterActor *bottom_button_group;
  ShellAuthMenuButton *auth_menu_button;
  ClutterActor *a11y_menu_button;
  ClutterActor *logo_bin;

  GFile *logo_file;
  GFile *banner_message_file;
  GFileMonitor *banner_message_monitor;

  ShellRealmdManager *realm_manager;

  GdmGreeter *greeter;
  gulong greeter_connection_closed_id;
  ShellLogin1Session *greeter_session_proxy;
  GVariant *selected_auth_mechanism;

  char *user_name;
  gulong next_signal_id;
  int disable_user_list;          /* -1 until the first update */
  gboolean user_list_loaded;

  GPtrArray *bindings;            /* GBinding, of _bindOpacity () */

  ShellNotification *conflicting_session_notification;   /* weak */

  ShellBatch *timed_login_batch;
  ShellBatchHold *timed_login_user_list_hold;
  guint timed_login_idle_timeout_id;
  GPtrArray *timed_login_actions;  /* ClutterAction, added to the stage */

  ClutterGrab *grab;
  GCancellable *cancellable;
};

G_DEFINE_FINAL_TYPE (ShellLoginDialog, shell_login_dialog, ST_TYPE_WIDGET)

enum
{
  FAILED,
  WAKE_UP_SCREEN,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

static void update_banner (ShellLoginDialog *self);
static void start_session (ShellLoginDialog *self,
                           const char       *service_name);
static void hide_user_list_ask_for_username_and_begin_verification (ShellLoginDialog *self);

/* ---- greeter calls ---- */

#define GREETER_DONE_CB(call)                                                  \
  static void                                                                  \
  greeter_##call##_done_cb (GObject      *source,                              \
                            GAsyncResult *res,                                 \
                            gpointer      user_data)                           \
  {                                                                            \
    g_autoptr (GError) error = NULL;                                           \
                                                                               \
    if (!gdm_greeter_call_##call##_finish (GDM_GREETER (source), res, &error) && \
        !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))            \
      g_warning ("%s: %s", #call, error->message);                             \
  }

GREETER_DONE_CB (begin_auto_login)
GREETER_DONE_CB (select_session)
GREETER_DONE_CB (start_session_when_ready)
GREETER_DONE_CB (stop_conflicting_session)

/* ---- the visibility of the buttons ---- */

/* `_showBottomButtonGroup ()` */
static void
show_bottom_button_group (ShellLoginDialog *self)
{
  ShellEaseParams params = ease_params (FADE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);

  clutter_actor_set_reactive (self->bottom_button_group, TRUE);
  shell_actor_ease (self->bottom_button_group, &params, NULL, NULL, NULL,
                    "opacity", 255u,
                    NULL);
}

/* `_hideBottomButtonGroup ()` */
static void
hide_bottom_button_group (ShellLoginDialog *self)
{
  ShellEaseParams params = ease_params (FADE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);

  clutter_actor_set_reactive (self->bottom_button_group, FALSE);
  shell_actor_ease (self->bottom_button_group, &params, NULL, NULL, NULL,
                    "opacity", 0u,
                    NULL);
}

/* ---- the opacity of the rest of the shell ---- */

/* `_bindOpacity ()` */
static void
bind_opacity (ShellLoginDialog *self)
{
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  ClutterActor *ui_group = get_ui_group ();
  ClutterActor *screen_shield_group;
  ClutterActor *child;

  if (ui_group == NULL || layout_manager == NULL)
    return;

  screen_shield_group = CLUTTER_ACTOR (shell_layout_manager_get_screen_shield_group (layout_manager));

  for (child = clutter_actor_get_first_child (ui_group);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    {
      GBinding *binding;

      if (child == screen_shield_group)
        continue;

      binding = g_object_bind_property (self, "opacity", child, "opacity", G_BINDING_DEFAULT);
      g_ptr_array_add (self->bindings, g_object_ref (binding));
    }
}

/* `_unbindOpacity ()` */
static void
unbind_opacity (ShellLoginDialog *self)
{
  guint i;

  for (i = 0; i < self->bindings->len; i++)
    g_binding_unbind (g_ptr_array_index (self->bindings, i));

  g_ptr_array_set_size (self->bindings, 0);
}

static void
unbind_opacity_done_cb (gboolean finished,
                        gpointer user_data)
{
  unbind_opacity (SHELL_LOGIN_DIALOG (user_data));
}

/* ---- sessions and mechanisms ---- */

static gint
compare_session_names (gconstpointer a,
                       gconstpointer b)
{
  GVariant *first = (GVariant *) a;
  GVariant *second = (GVariant *) b;
  const char *first_name = NULL;
  const char *second_name = NULL;

  g_variant_get_child (first, 0, "&s", &first_name);
  g_variant_get_child (second, 0, "&s", &second_name);

  return g_utf8_collate (first_name, second_name);
}

/* `get _user ()` of the JS: the user is looked up by name, it may not exist (yet) */
static ActUser *
login_dialog_get_user (ShellLoginDialog *self)
{
  if (self->user_name == NULL || self->user_manager == NULL)
    return NULL;

  return act_user_manager_get_user (self->user_manager, self->user_name);
}

/* `_updateSessions ()` */
static void
update_sessions (ShellLoginDialog *self)
{
  ActUser *user = login_dialog_get_user (self);
  g_autoptr (GVariant) criteria = g_variant_ref_sink (section_criteria (session_type_section_name ()));
  g_auto (GStrv) ids = NULL;
  g_autoptr (GPtrArray) sessions = NULL;
  guint i;

  shell_auth_menu_button_clear_items (self->auth_menu_button, criteria);

  /* User is already logged in, it can't choose a different session */
  if (user != NULL && act_user_is_loaded (user) &&
      act_user_is_logged_in (user))
    return;

  ids = gdm_get_session_ids ();
  if (ids == NULL || g_strv_length (ids) == 1)
    return;

  /* The (name, id) pairs, sorted by name */
  sessions = g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
  for (i = 0; ids[i] != NULL; i++)
    {
      g_autofree char *description = NULL;
      g_autofree char *session_name = gdm_get_session_name_and_description (ids[i], &description);

      g_ptr_array_add (sessions,
                       g_variant_ref_sink (g_variant_new ("(ss)",
                                                          session_name != NULL ? session_name : "",
                                                          ids[i])));
    }
  g_ptr_array_sort_values (sessions, (GCompareFunc) compare_session_names);

  for (i = 0; i < sessions->len; i++)
    {
      GVariant *session = g_ptr_array_index (sessions, i);
      const char *session_name = NULL;
      const char *id = NULL;
      GVariantBuilder builder;

      g_variant_get (session, "(&s&s)", &session_name, &id);

      g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&builder, "{sv}", "sectionName",
                             g_variant_new_string (session_type_section_name ()));
      g_variant_builder_add (&builder, "{sv}", "name", g_variant_new_string (session_name));
      g_variant_builder_add (&builder, "{sv}", "id", g_variant_new_string (id));

      shell_auth_menu_button_add_item (self->auth_menu_button, g_variant_builder_end (&builder));
    }
}

/* `_selectAuthMechanism ()` */
static void
select_auth_mechanism (ShellLoginDialog *self,
                       GVariant         *auth_mechanism)
{
  g_autoptr (GVariant) old_mechanism = self->selected_auth_mechanism;

  self->selected_auth_mechanism = NULL;

  if (!shell_auth_prompt_select_mechanism (self->auth_prompt, auth_mechanism))
    {
      if (old_mechanism != NULL)
        shell_auth_menu_button_set_active_item (self->auth_menu_button, old_mechanism);
      self->selected_auth_mechanism = g_steal_pointer (&old_mechanism);
      return;
    }

  self->selected_auth_mechanism = g_variant_ref (auth_mechanism);
}

static void
auth_menu_active_item_changed_cb (ShellAuthMenuButton *button,
                                  const char          *section_name,
                                  ShellLoginDialog    *self)
{
  g_autoptr (GVariant) criteria = NULL;
  g_autoptr (GVariant) item = NULL;

  if (section_name == NULL)
    return;

  criteria = g_variant_ref_sink (section_criteria (section_name));
  item = shell_auth_menu_button_get_active_item (button, criteria);
  if (item == NULL)
    return;

  if (g_strcmp0 (section_name, primary_login_method_section_name ()) == 0)
    {
      select_auth_mechanism (self, item);
    }
  else if (g_strcmp0 (section_name, session_type_section_name ()) == 0)
    {
      g_autofree char *id = NULL;

      g_variant_lookup (item, "id", "s", &id);
      if (self->greeter != NULL && id != NULL)
        gdm_greeter_call_select_session (self->greeter, id, self->cancellable,
                                         greeter_select_session_done_cb, NULL);
    }

  shell_auth_menu_button_close_menu (button);
}

/* `_createAuthMenuButton ()` */
static void
create_auth_menu_button (ShellLoginDialog *self)
{
  const char *section_order[] = {
    primary_login_method_section_name (),
    session_type_section_name (),
    NULL
  };

  self->auth_menu_button = shell_auth_menu_button_new (section_order, TRUE);
  g_object_set (self->auth_menu_button,
                "accessible-name", _("Login Options"),
                "visible", FALSE,
                NULL);

  g_signal_connect (self->auth_menu_button, "active-item-changed",
                    G_CALLBACK (auth_menu_active_item_changed_cb), self);
  clutter_actor_add_child (self->bottom_button_group, CLUTTER_ACTOR (self->auth_menu_button));
}

/* ---- the user list ---- */

/* `_updateCancelButton ()` */
static void
update_cancel_button (ShellLoginDialog *self)
{
  gboolean cancel_visible;

  /* Hide the cancel button if the user list is disabled and we're asking for
   * a username */
  if (shell_auth_prompt_get_verification_status (self->auth_prompt) ==
      SHELL_AUTH_PROMPT_STATUS_NOT_VERIFYING &&
      self->disable_user_list == 1)
    cancel_visible = FALSE;
  else
    cancel_visible = TRUE;

  set_actor_visible (CLUTTER_ACTOR (shell_auth_prompt_get_cancel_button (self->auth_prompt)),
                     cancel_visible);
}

/* `_updateDisableUserList ()` */
static void
update_disable_user_list (ShellLoginDialog *self)
{
  gboolean disable_user_list = g_settings_get_boolean (self->settings,
                                                       SHELL_GDM_DISABLE_USER_LIST_KEY);

  /* Disable user list when there are no users. */
  if (self->user_list_loaded && g_hash_table_size (self->user_list->items) == 0)
    disable_user_list = TRUE;

  if (self->disable_user_list != disable_user_list)
    {
      self->disable_user_list = disable_user_list;

      if (shell_auth_prompt_get_verification_status (self->auth_prompt) ==
          SHELL_AUTH_PROMPT_STATUS_NOT_VERIFYING)
        shell_auth_prompt_reset (self->auth_prompt, FALSE, FALSE);

      if (self->disable_user_list && self->timed_login_user_list_hold != NULL)
        shell_batch_hold_release (self->timed_login_user_list_hold);
    }
}

static void
disable_user_list_changed_cb (GSettings        *settings,
                              const char       *key,
                              ShellLoginDialog *self)
{
  update_disable_user_list (self);
}

static void
startup_complete_cb (ShellLayoutManager *layout_manager,
                     ShellLoginDialog   *self)
{
  update_disable_user_list (self);
}

/* `_loadUserList ()` */
static void
user_manager_user_added_cb (ActUserManager   *manager,
                            ActUser          *user,
                            ShellLoginDialog *self)
{
  shell_user_list_add_user (self->user_list, user);
  update_disable_user_list (self);
}

static void
user_manager_user_removed_cb (ActUserManager   *manager,
                              ActUser          *user,
                              ShellLoginDialog *self)
{
  shell_user_list_remove_user (self->user_list, user);
  update_disable_user_list (self);
}

static void
user_manager_user_changed_cb (ActUserManager   *manager,
                              ActUser          *user,
                              ShellLoginDialog *self)
{
  if (shell_user_list_contains_user (self->user_list, user) && act_user_get_locked (user))
    shell_user_list_remove_user (self->user_list, user);
  else if (!shell_user_list_contains_user (self->user_list, user) && !act_user_get_locked (user))
    shell_user_list_add_user (self->user_list, user);

  update_disable_user_list (self);
}

static void
load_user_list (ShellLoginDialog *self)
{
  g_autoptr (GSList) users = NULL;
  GSList *l;

  if (self->user_list_loaded)
    return;

  self->user_list_loaded = TRUE;

  users = act_user_manager_list_users (self->user_manager);
  for (l = users; l != NULL; l = l->next)
    shell_user_list_add_user (self->user_list, l->data);

  update_disable_user_list (self);

  g_signal_connect_object (self->user_manager, "user-added",
                           G_CALLBACK (user_manager_user_added_cb), self, 0);
  g_signal_connect_object (self->user_manager, "user-removed",
                           G_CALLBACK (user_manager_user_removed_cb), self, 0);
  g_signal_connect_object (self->user_manager, "user-changed",
                           G_CALLBACK (user_manager_user_changed_cb), self, 0);
}

static void
user_manager_loaded_cb (GObject          *object,
                        GParamSpec       *pspec,
                        ShellLoginDialog *self)
{
  gboolean is_loaded;

  g_object_get (self->user_manager, "is-loaded", &is_loaded, NULL);
  if (!is_loaded)
    return;

  g_signal_handlers_disconnect_by_func (self->user_manager, user_manager_loaded_cb, self);
  load_user_list (self);
}

static gboolean
load_user_list_idle_cb (gpointer user_data)
{
  ShellLoginDialog *self = SHELL_LOGIN_DIALOG (user_data);

  load_user_list (self);
  return G_SOURCE_REMOVE;
}

/* `_ensureUserListLoaded ()` */
static void
ensure_user_list_loaded (ShellLoginDialog *self)
{
  gboolean is_loaded;
  guint id;

  g_object_get (self->user_manager, "is-loaded", &is_loaded, NULL);
  if (!is_loaded)
    {
      g_signal_connect (self->user_manager, "notify::is-loaded",
                        G_CALLBACK (user_manager_loaded_cb), self);
      return;
    }

  id = g_idle_add_full (G_PRIORITY_DEFAULT, load_user_list_idle_cb,
                        g_object_ref (self), g_object_unref);
  g_source_set_name_by_id (id, "[gnome-shell] _loadUserList");
}

/* `_setUserListExpanded ()` */
static void
set_user_list_expanded (ShellLoginDialog *self,
                        gboolean          expanded)
{
  shell_user_list_update_style (self->user_list, expanded);
  set_actor_visible (self->user_selection_box, expanded);
}

/* `_hideUserList ()` */
static void
hide_user_list (ShellLoginDialog *self)
{
  set_user_list_expanded (self, FALSE);
  if (clutter_actor_is_visible (self->user_selection_box))
    g_object_unref (shell_gdm_clone_and_fade_out_actor (self->user_selection_box));
}

/* `_hideBannerView ()` */
static void
hide_banner_view (ShellLoginDialog *self)
{
  clutter_actor_remove_all_transitions (self->banner_view);
  clutter_actor_set_opacity (self->banner_view, 0);
  clutter_actor_hide (self->banner_view);
}

/* `_fadeInBannerView ()` */
static void
fade_in_banner_view (ShellLoginDialog *self)
{
  ShellEaseParams params = ease_params (FADE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);

  clutter_actor_show (self->banner_view);
  shell_actor_ease (self->banner_view, &params, NULL, NULL, NULL,
                    "opacity", 255u,
                    NULL);
}

/* `_showUserList ()` */
static void
show_user_list (ShellLoginDialog *self)
{
  ensure_user_list_loaded (self);
  clutter_actor_hide (CLUTTER_ACTOR (self->auth_prompt));
  hide_banner_view (self);
  shell_auth_menu_button_update_visibility (self->auth_menu_button, FALSE);
  set_user_list_expanded (self, TRUE);
  clutter_actor_show (self->not_listed_button);
  clutter_actor_grab_key_focus (CLUTTER_ACTOR (self->user_list));
}

/* `_showPrompt ()` */
static void
show_prompt (ShellLoginDialog *self)
{
  ShellEaseParams params = ease_params (FADE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);

  if (clutter_actor_is_visible (CLUTTER_ACTOR (self->auth_prompt)))
    return;

  clutter_actor_set_opacity (CLUTTER_ACTOR (self->auth_prompt), 0);
  clutter_actor_show (CLUTTER_ACTOR (self->auth_prompt));
  shell_actor_ease (CLUTTER_ACTOR (self->auth_prompt), &params, NULL, NULL, NULL,
                    "opacity", 255u,
                    NULL);
  fade_in_banner_view (self);
}

/* `_showRealmLoginHint ()` */
static void
show_realm_login_hint (ShellLoginDialog *self,
                       const char       *hint)
{
  g_autoptr (GString) formatted = NULL;
  g_autofree char *message = NULL;
  const char *p;

  if (hint == NULL || hint[0] == '\0')
    return;

  /* hint.replace (/%U/g, 'user').replace (/%D/g, 'DOMAIN').replace (/%[^UD]/g, '') */
  formatted = g_string_new (NULL);
  for (p = hint; *p != '\0';)
    {
      if (*p == '%' && p[1] != '\0')
        {
          if (p[1] == 'U')
            g_string_append (formatted, "user");
          else if (p[1] == 'D')
            g_string_append (formatted, "DOMAIN");

          p = g_utf8_next_char (p + 1);
        }
      else
        {
          g_string_append_c (formatted, *p);
          p++;
        }
    }

  /* Translators: this message is shown below the username entry field to clue the user in on how to login to the local network realm */
  message = g_strdup_printf (_("(e.g., user or %s)"), formatted->str);
  shell_auth_prompt_set_message (self->auth_prompt, message, SHELL_GDM_MESSAGE_TYPE_HINT);
}

static void
realm_login_format_changed_cb (ShellRealmdManager *manager,
                               const char         *hint,
                               ShellLoginDialog   *self)
{
  show_realm_login_hint (self, hint);
}

static void
next_signal_cb (ShellAuthPrompt   *prompt,
                ShellLoginDialog  *self)
{
  g_autofree char *answer = NULL;

  g_signal_handler_disconnect (prompt, self->next_signal_id);
  self->next_signal_id = 0;

  shell_auth_prompt_update_sensitivity (prompt, FALSE);
  answer = shell_auth_prompt_get_answer (prompt);

  g_free (self->user_name);
  self->user_name = g_strdup (answer);

  update_sessions (self);
  shell_auth_prompt_clear (prompt, FALSE);
  shell_auth_prompt_begin (prompt, answer, NULL);
}

/* `_askForUsernameAndBeginVerification ()` */
static void
ask_for_username_and_begin_verification (ShellLoginDialog *self)
{
  shell_auth_prompt_set_user (self->auth_prompt, NULL);
  shell_auth_prompt_update_entry (self->auth_prompt, FALSE);
  shell_auth_prompt_set_question (self->auth_prompt, _("Username"));

  show_realm_login_hint (self, shell_realmd_manager_get_login_format (self->realm_manager));

  if (self->next_signal_id != 0)
    g_signal_handler_disconnect (self->auth_prompt, self->next_signal_id);
  self->next_signal_id = g_signal_connect (self->auth_prompt, "next",
                                           G_CALLBACK (next_signal_cb), self);

  shell_auth_menu_button_update_visibility (self->auth_menu_button, FALSE);
  show_prompt (self);
}

/* `_hideUserListAskForUsernameAndBeginVerification ()` */
static void
hide_user_list_ask_for_username_and_begin_verification (ShellLoginDialog *self)
{
  hide_user_list (self);
  ask_for_username_and_begin_verification (self);
}

static void
not_listed_clicked_cb (StButton         *button,
                       int               clicked_button,
                       ShellLoginDialog *self)
{
  hide_user_list_ask_for_username_and_begin_verification (self);
}

/* `_hideUserListAndBeginVerification ()` */
static void
hide_user_list_and_begin_verification (ShellLoginDialog *self)
{
  hide_user_list (self);
  shell_auth_prompt_set_user (self->auth_prompt, NULL);
  shell_auth_prompt_begin (self->auth_prompt, NULL, NULL);
}

/* `_beginVerificationForItem ()` */
static ShellBatchHold *
begin_verification_for_item (ShellLoginDialog *self,
                             ShellUserListItem *item)
{
  ShellBatchHold *hold = shell_batch_hold_new ();

  shell_auth_prompt_set_user (self->auth_prompt, item->user);
  shell_auth_prompt_begin (self->auth_prompt, act_user_get_user_name (item->user), hold);

  return hold;
}

/* `_onUserListActivated ()` */
static void
user_list_activate_cb (ShellUserList     *list,
                       ShellUserListItem *item,
                       ShellLoginDialog  *self)
{
  g_autoptr (ShellBatchHold) fade_hold = NULL;
  g_autoptr (ShellBatchHold) begin_hold = NULL;
  g_autoptr (ShellConcurrentBatch) batch = NULL;
  ShellBatchTask *tasks[2];

  g_free (self->user_name);
  self->user_name = g_strdup (act_user_get_user_name (item->user));

  update_sessions (self);

  update_cancel_button (self);

  if (self->conflicting_session_notification != NULL)
    shell_notification_destroy (self->conflicting_session_notification,
                                SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED);

  fade_hold = shell_gdm_clone_and_fade_out_actor (self->user_selection_box);
  begin_hold = begin_verification_for_item (self, item);

  tasks[0] = SHELL_BATCH_TASK (fade_hold);
  tasks[1] = SHELL_BATCH_TASK (begin_hold);
  batch = shell_concurrent_batch_new (tasks, 2);
  shell_batch_run (SHELL_BATCH (batch));
}

/* ---- the auth prompt ---- */

static void
auth_prompt_prompted_cb (ShellAuthPrompt  *prompt,
                         ShellLoginDialog *self)
{
  shell_auth_menu_button_update_visibility (self->auth_menu_button, TRUE);

  show_prompt (self);
}

/* `_ensureGreeterProxy ()` */
static void ensure_greeter_proxy (ShellLoginDialog *self);

static void
default_session_name_changed_cb (GdmGreeter       *greeter,
                                 const char       *session_id,
                                 ShellLoginDialog *self)
{
  g_autoptr (GVariantBuilder) builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);

  g_variant_builder_add (builder, "{sv}", "sectionName",
                         g_variant_new_string (session_type_section_name ()));
  g_variant_builder_add (builder, "{sv}", "id", g_variant_new_string (session_id));

  shell_auth_menu_button_set_active_item (self->auth_menu_button,
                                          g_variant_builder_end (builder));
}

/* ---- conflicting sessions ---- */

/* `_notifyConflictingSessionDialogClosed ()` */
static void
notify_conflicting_session_dialog_closed (ShellLoginDialog *self)
{
  ShellMessageTray *message_tray = shell_main_get_message_tray (shell_main_get_default ());
  ShellSource *source;
  g_autoptr (ShellNotification) notification = NULL;

  if (message_tray == NULL)
    return;

  source = shell_message_tray_get_system_source (message_tray);

  notification = shell_notification_new (source,
                                         _("Login Attempt Timed Out"),
                                         _("Login took too long, please try again"));
  shell_notification_set_urgency (notification, SHELL_NOTIFICATION_URGENCY_CRITICAL);
  shell_notification_set_is_transient (notification, TRUE);

  g_set_weak_pointer (&self->conflicting_session_notification, notification);

  shell_source_add_notification (source, notification);
}

/* What the conflicting session dialog needs while it is open */
typedef struct
{
  ShellLoginDialog *login_dialog;                 /* weak */
  ShellConflictingSessionDialog *dialog;          /* weak */
  char *service_name;
  char *conflicting_session_id;
  guint close_dialog_timeout_id;
  gulong session_removed_id;
  ShellLoginManager *login_manager;               /* weak */
} ConflictingSessionData;

static void
conflicting_session_data_cleanup (ConflictingSessionData *data)
{
  if (data->close_dialog_timeout_id != 0)
    {
      g_source_remove (data->close_dialog_timeout_id);
      data->close_dialog_timeout_id = 0;
    }

  if (data->login_manager != NULL && data->session_removed_id != 0)
    g_signal_handler_disconnect (data->login_manager, data->session_removed_id);
  data->session_removed_id = 0;
}

static void
conflicting_session_data_free (ConflictingSessionData *data)
{
  conflicting_session_data_cleanup (data);

  g_clear_weak_pointer (&data->login_dialog);
  g_clear_weak_pointer (&data->dialog);
  g_clear_weak_pointer (&data->login_manager);
  g_free (data->service_name);
  g_free (data->conflicting_session_id);
  g_free (data);
}

static gboolean
conflicting_session_timeout_source_cb (gpointer user_data)
{
  ConflictingSessionData *data = user_data;

  data->close_dialog_timeout_id = 0;

  if (data->login_dialog != NULL)
    {
      notify_conflicting_session_dialog_closed (data->login_dialog);
      if (data->dialog != NULL)
        shell_modal_dialog_close (SHELL_MODAL_DIALOG (data->dialog));
      shell_auth_prompt_reset (data->login_dialog->auth_prompt, FALSE, FALSE);
    }
  else if (data->dialog != NULL)
    {
      shell_modal_dialog_close (SHELL_MODAL_DIALOG (data->dialog));
    }

  return G_SOURCE_REMOVE;
}

typedef struct
{
  ShellLoginDialog *login_dialog;   /* strong */
  char *service_name;
} StartSessionData;

static void
start_session_data_free (StartSessionData *data)
{
  g_clear_object (&data->login_dialog);
  g_free (data->service_name);
  g_free (data);
}

static void
finish_start_session_cb (gpointer user_data)
{
  StartSessionData *data = user_data;

  start_session (data->login_dialog, data->service_name);
}

static void
conflicting_session_removed_cb (ShellLoginManager      *login_manager,
                                const char             *session_id,
                                ConflictingSessionData *data)
{
  StartSessionData *start_data;

  if (g_strcmp0 (session_id, data->conflicting_session_id) != 0)
    return;

  if (data->dialog != NULL)
    shell_modal_dialog_close (SHELL_MODAL_DIALOG (data->dialog));

  if (data->login_dialog == NULL)
    return;

  start_data = g_new0 (StartSessionData, 1);
  start_data->login_dialog = g_object_ref (data->login_dialog);
  start_data->service_name = g_strdup (data->service_name);
  shell_auth_prompt_finish (data->login_dialog->auth_prompt,
                            finish_start_session_cb, start_data,
                            (GDestroyNotify) start_session_data_free);
}

static void
conflicting_dialog_cancel_cb (ShellConflictingSessionDialog *dialog,
                              ConflictingSessionData        *data)
{
  shell_modal_dialog_close (SHELL_MODAL_DIALOG (dialog));
  if (data->login_dialog != NULL)
    shell_auth_prompt_reset (data->login_dialog->auth_prompt, FALSE, FALSE);
}

static void
conflicting_dialog_force_stop_cb (ShellConflictingSessionDialog *dialog,
                                  ConflictingSessionData        *data)
{
  ShellLoginDialog *self = data->login_dialog;

  if (self != NULL && self->greeter != NULL)
    gdm_greeter_call_stop_conflicting_session (self->greeter, self->cancellable,
                                               greeter_stop_conflicting_session_done_cb, NULL);
}

static void
conflicting_dialog_closed_cb (ShellModalDialog        *dialog,
                              ConflictingSessionData *data)
{
  conflicting_session_data_cleanup (data);
  g_signal_handlers_disconnect_by_data (dialog, data);
}

/* `_showConflictingSessionDialog ()` */
static void
show_conflicting_session_dialog (ShellLoginDialog  *self,
                                 const char        *service_name,
                                 ShellLogin1Session *conflicting_session)
{
  ShellLoginManager *login_manager = shell_login_manager_get_default ();
  ShellConflictingSessionDialog *dialog;
  ConflictingSessionData *data;

  if (self->greeter_session_proxy == NULL)
    {
      g_warning ("%s: no session for the login screen", G_STRFUNC);
      shell_auth_prompt_reset (self->auth_prompt, FALSE, FALSE);
      return;
    }

  dialog = shell_conflicting_session_dialog_new (conflicting_session, self->greeter_session_proxy);

  data = g_new0 (ConflictingSessionData, 1);
  g_set_weak_pointer (&data->login_dialog, self);
  g_set_weak_pointer (&data->dialog, dialog);
  g_set_weak_pointer (&data->login_manager, login_manager);
  data->service_name = g_strdup (service_name);
  data->conflicting_session_id = g_strdup (shell_login1_session_get_id (conflicting_session));
  g_object_set_data_full (G_OBJECT (dialog), "shell-conflicting-session-data", data,
                          (GDestroyNotify) conflicting_session_data_free);

  data->close_dialog_timeout_id = g_timeout_add_seconds (CONFLICTING_SESSION_DIALOG_TIMEOUT,
                                                         conflicting_session_timeout_source_cb,
                                                         data);

  data->session_removed_id = g_signal_connect (login_manager, "session-removed",
                                               G_CALLBACK (conflicting_session_removed_cb), data);

  g_signal_connect (dialog, "cancel", G_CALLBACK (conflicting_dialog_cancel_cb), data);
  g_signal_connect (dialog, "force-stop", G_CALLBACK (conflicting_dialog_force_stop_cb), data);
  g_signal_connect (dialog, "closed", G_CALLBACK (conflicting_dialog_closed_cb), data);

  shell_modal_dialog_open (SHELL_MODAL_DIALOG (dialog));
}

/* What `_findConflictingSession ()` is looking for */
typedef struct
{
  GWeakRef login_dialog;
  char *service_name;
  char *session_id;
} SessionOpenedData;

static void
session_opened_data_free (SessionOpenedData *data)
{
  g_weak_ref_clear (&data->login_dialog);
  g_free (data->service_name);
  g_free (data->session_id);
  g_free (data);
}

/* The proxy of a session of the user of @starting_session_id which is
 * running, or %NULL */
static ShellLogin1Session *
find_conflicting_session (GVariant   *sessions,
                          const char *starting_session_id)
{
  ShellLoginManager *login_manager = shell_login_manager_get_default ();
  g_autofree char *starting_session_owner = NULL;
  GVariantIter iter;
  const char *id, *user, *path;

  g_variant_iter_init (&iter, sessions);
  while (g_variant_iter_next (&iter, "(&su&s&s&o)", &id, NULL, &user, NULL, &path))
    {
      if (g_str_equal (id, starting_session_id))
        {
          starting_session_owner = g_strdup (user);
          break;
        }
    }

  if (starting_session_owner == NULL)
    return NULL;

  g_variant_iter_init (&iter, sessions);
  while (g_variant_iter_next (&iter, "(&su&s&s&o)", &id, NULL, &user, NULL, &path))
    {
      g_autoptr (GError) error = NULL;
      g_autoptr (ShellLogin1Session) session_proxy = NULL;
      const char *type, *state;

      if (g_str_equal (starting_session_id, id))
        continue;

      if (g_strcmp0 (starting_session_owner, user) != 0)
        continue;

      session_proxy = shell_login_manager_get_session (login_manager, path, &error);
      if (session_proxy == NULL)
        {
          if (error != NULL)
            g_warning ("%s: %s", G_STRFUNC, error->message);
          continue;
        }

      type = shell_login1_session_get_type_ (session_proxy);
      if (g_strcmp0 (type, "wayland") != 0 && g_strcmp0 (type, "x11") != 0)
        continue;

      state = shell_login1_session_get_state (session_proxy);
      if (g_strcmp0 (state, "active") != 0 && g_strcmp0 (state, "online") != 0)
        continue;

      return g_steal_pointer (&session_proxy);
    }

  return NULL;
}

static void
sessions_listed_cb (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  SessionOpenedData *data = user_data;
  g_autoptr (ShellLoginDialog) self = g_weak_ref_get (&data->login_dialog);
  g_autoptr (GVariant) sessions = NULL;
  g_autoptr (ShellLogin1Session) conflicting_session = NULL;
  StartSessionData *start_data;

  if (self == NULL || self->auth_prompt == NULL)
    {
      session_opened_data_free (data);
      return;
    }

  sessions = shell_login_manager_list_sessions_finish (SHELL_LOGIN_MANAGER (source), res);
  conflicting_session = find_conflicting_session (sessions, data->session_id);

  if (conflicting_session != NULL)
    {
      show_conflicting_session_dialog (self, data->service_name, conflicting_session);
      session_opened_data_free (data);
      return;
    }

  start_data = g_new0 (StartSessionData, 1);
  start_data->login_dialog = g_object_ref (self);
  start_data->service_name = g_strdup (data->service_name);
  shell_auth_prompt_finish (self->auth_prompt, finish_start_session_cb, start_data,
                            (GDestroyNotify) start_session_data_free);

  session_opened_data_free (data);
}

/* `_onSessionOpened ()` */
static void
greeter_session_opened_cb (GdmGreeter       *greeter,
                           const char       *service_name,
                           const char       *session_id,
                           ShellLoginDialog *self)
{
  StartSessionData *start_data;

  if (session_id != NULL && session_id[0] != '\0')
    {
      SessionOpenedData *data = g_new0 (SessionOpenedData, 1);

      g_weak_ref_init (&data->login_dialog, self);
      data->service_name = g_strdup (service_name);
      data->session_id = g_strdup (session_id);

      shell_login_manager_list_sessions_async (shell_login_manager_get_default (),
                                               self->cancellable,
                                               sessions_listed_cb, data);
      return;
    }

  start_data = g_new0 (StartSessionData, 1);
  start_data->login_dialog = g_object_ref (self);
  start_data->service_name = g_strdup (service_name);
  shell_auth_prompt_finish (self->auth_prompt, finish_start_session_cb, start_data,
                            (GDestroyNotify) start_session_data_free);
}

/* `_startSession ()` */
static void
start_session_ease_done_cb (gboolean finished,
                            gpointer user_data)
{
  StartSessionData *data = user_data;
  ShellLoginDialog *self = data->login_dialog;

  if (finished && self->greeter != NULL)
    gdm_greeter_call_start_session_when_ready (self->greeter, data->service_name, TRUE,
                                               self->cancellable,
                                               greeter_start_session_when_ready_done_cb, NULL);
  unbind_opacity (self);
}

static void
start_session (ShellLoginDialog *self,
               const char       *service_name)
{
  ShellEaseParams params = ease_params (FADE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);
  StartSessionData *data = g_new0 (StartSessionData, 1);

  data->login_dialog = g_object_ref (self);
  data->service_name = g_strdup (service_name);

  bind_opacity (self);
  shell_actor_ease (CLUTTER_ACTOR (self), &params,
                    start_session_ease_done_cb, data,
                    (GDestroyNotify) start_session_data_free,
                    "opacity", 0u,
                    NULL);
}

/* ---- timed login ---- */

/* The variables the tasks of `_startTimedLogin ()` share */
typedef struct
{
  int ref_count;
  ShellLoginDialog *dialog;        /* weak */
  char *user_name;
  int delay;
  gboolean first_run;
  ClutterActor *login_item;        /* weak */
  double animation_time;
} TimedLoginState;

static TimedLoginState *
timed_login_state_ref (TimedLoginState *state)
{
  state->ref_count++;
  return state;
}

static void
timed_login_state_unref (TimedLoginState *state)
{
  if (--state->ref_count > 0)
    return;

  g_clear_weak_pointer (&state->dialog);
  g_clear_weak_pointer (&state->login_item);
  g_free (state->user_name);
  g_free (state);
}

/* What `_waitForItemForUser ()` keeps while it waits */
typedef struct
{
  ShellUserList *user_list;      /* weak */
  char *user_name;
  gulong item_added_id;
} WaitForItemData;

static void
wait_for_item_data_free (WaitForItemData *data)
{
  if (data->user_list != NULL && data->item_added_id != 0)
    g_signal_handler_disconnect (data->user_list, data->item_added_id);

  g_clear_weak_pointer (&data->user_list);
  g_free (data->user_name);
  g_free (data);
}

static void
wait_for_item_added_cb (ShellUserList     *list,
                        ShellUserListItem *added,
                        ShellBatchHold    *hold)
{
  WaitForItemData *data = g_object_get_data (G_OBJECT (hold), "shell-wait-for-item");

  if (data != NULL &&
      shell_user_list_get_item_from_user_name (list, data->user_name) != NULL)
    shell_batch_hold_release (hold);
}

static void
wait_for_item_hold_released_cb (ShellBatchHold *hold,
                                gpointer        user_data)
{
  WaitForItemData *data = g_object_get_data (G_OBJECT (hold), "shell-wait-for-item");

  if (data != NULL && data->user_list != NULL && data->item_added_id != 0)
    {
      g_signal_handler_disconnect (data->user_list, data->item_added_id);
      data->item_added_id = 0;
    }
}

/* `_waitForItemForUser ()`; the hold is owned by the caller */
static ShellBatchHold *
wait_for_item_for_user (ShellLoginDialog *self,
                        const char       *user_name)
{
  ShellBatchHold *hold;
  WaitForItemData *data;

  if (shell_user_list_get_item_from_user_name (self->user_list, user_name) != NULL)
    return NULL;

  hold = shell_batch_hold_new ();

  data = g_new0 (WaitForItemData, 1);
  g_set_weak_pointer (&data->user_list, self->user_list);
  data->user_name = g_strdup (user_name);
  g_object_set_data_full (G_OBJECT (hold), "shell-wait-for-item", data,
                          (GDestroyNotify) wait_for_item_data_free);

  data->item_added_id = g_signal_connect (self->user_list, "item-added",
                                          G_CALLBACK (wait_for_item_added_cb), hold);
  g_signal_connect (hold, "release",
                    G_CALLBACK (wait_for_item_hold_released_cb), NULL);

  return hold;
}

typedef struct
{
  ShellBatchHold *hold;
  ShellLoginDialog *dialog;       /* weak */
} IdleBlockData;

static void
idle_block_data_free (IdleBlockData *data)
{
  g_clear_object (&data->hold);
  g_clear_weak_pointer (&data->dialog);
  g_free (data);
}

static gboolean
timed_login_idle_timeout_cb (gpointer user_data)
{
  IdleBlockData *data = user_data;
  g_autoptr (ShellBatchHold) hold = g_object_ref (data->hold);

  if (data->dialog != NULL)
    data->dialog->timed_login_idle_timeout_id = 0;

  shell_batch_hold_release (hold);
  return G_SOURCE_REMOVE;
}

/* `_blockTimedLoginUntilIdle ()`; the hold is kept alive by the timeout */
static ShellBatchHold *
block_timed_login_until_idle (ShellLoginDialog *self)
{
  IdleBlockData *data = g_new0 (IdleBlockData, 1);

  data->hold = shell_batch_hold_new ();
  g_set_weak_pointer (&data->dialog, self);

  self->timed_login_idle_timeout_id =
    g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, (guint) TIMED_LOGIN_IDLE_THRESHOLD,
                                timed_login_idle_timeout_cb, data,
                                (GDestroyNotify) idle_block_data_free);
  g_source_set_name_by_id (self->timed_login_idle_timeout_id,
                           "[gnome-shell] this._timedLoginIdleTimeOutId");

  return data->hold;
}

static ShellBatchHold *
timed_login_task_wait_for_item (gpointer user_data)
{
  TimedLoginState *state = user_data;
  ShellLoginDialog *self = state->dialog;

  if (self == NULL || self->disable_user_list == 1)
    return NULL;

  g_clear_object (&self->timed_login_user_list_hold);
  self->timed_login_user_list_hold = wait_for_item_for_user (self, state->user_name);
  return self->timed_login_user_list_hold;
}

static ShellBatchHold *
timed_login_task_find_item (gpointer user_data)
{
  TimedLoginState *state = user_data;
  ShellLoginDialog *self = state->dialog;
  ClutterActor *login_item;

  if (self == NULL)
    return NULL;

  g_clear_object (&self->timed_login_user_list_hold);

  if (self->disable_user_list == 1)
    login_item = CLUTTER_ACTOR (self->auth_prompt);
  else
    login_item = CLUTTER_ACTOR (shell_user_list_get_item_from_user_name (self->user_list,
                                                                          state->user_name));

  g_clear_weak_pointer (&state->login_item);
  if (login_item == NULL)
    {
      g_debug ("%s: no item for the timed login of %s", G_STRFUNC, state->user_name);
      return NULL;
    }
  g_set_weak_pointer (&state->login_item, login_item);

  /* If there is an animation running on the item, reset it. */
  if (SHELL_IS_AUTH_PROMPT (login_item))
    shell_auth_prompt_hide_timed_login_indicator (SHELL_AUTH_PROMPT (login_item));
  else
    shell_user_list_item_hide_timed_login_indicator (SHELL_USER_LIST_ITEM (login_item));

  return NULL;
}

static ShellBatchHold *
timed_login_task_jump_to_item (gpointer user_data)
{
  TimedLoginState *state = user_data;
  ShellLoginDialog *self = state->dialog;
  gboolean is_loaded;

  if (self == NULL || self->disable_user_list == 1 || state->login_item == NULL)
    return NULL;

  /* If we're just starting out, start on the right item. */
  g_object_get (self->user_manager, "is-loaded", &is_loaded, NULL);
  if (!is_loaded)
    shell_user_list_jump_to_item (self->user_list, state->login_item);

  return NULL;
}

static ShellBatchHold *
timed_login_task_block_until_idle (gpointer user_data)
{
  TimedLoginState *state = user_data;

  /* This blocks the timed login animation until a few
   * seconds after the user stops interacting with the
   * login screen.
   *
   * We skip this step if the timed login delay is very short. */
  if (state->delay > TIMED_LOGIN_IDLE_THRESHOLD)
    {
      state->animation_time = state->delay - TIMED_LOGIN_IDLE_THRESHOLD;
      return state->dialog != NULL ? block_timed_login_until_idle (state->dialog) : NULL;
    }

  state->animation_time = state->delay;
  return NULL;
}

static ShellBatchHold *
timed_login_task_show_item (gpointer user_data)
{
  TimedLoginState *state = user_data;
  ShellLoginDialog *self = state->dialog;

  if (self == NULL || self->disable_user_list == 1 || state->login_item == NULL)
    return NULL;

  /* If idle timeout is done, make sure the timed login indicator is shown */
  if (state->delay > TIMED_LOGIN_IDLE_THRESHOLD &&
      clutter_actor_is_visible (CLUTTER_ACTOR (self->auth_prompt)))
    shell_auth_prompt_cancel (self->auth_prompt);

  if (state->delay > TIMED_LOGIN_IDLE_THRESHOLD || state->first_run)
    {
      shell_user_list_scroll_to_item (self->user_list, state->login_item);
      clutter_actor_grab_key_focus (state->login_item);
    }

  return NULL;
}

static ShellBatchHold *
timed_login_task_show_indicator (gpointer user_data)
{
  TimedLoginState *state = user_data;

  if (state->login_item == NULL)
    return NULL;

  if (SHELL_IS_AUTH_PROMPT (state->login_item))
    return shell_auth_prompt_show_timed_login_indicator (SHELL_AUTH_PROMPT (state->login_item),
                                                         state->animation_time);

  return shell_user_list_item_show_timed_login_indicator (SHELL_USER_LIST_ITEM (state->login_item),
                                                          state->animation_time);
}

static ShellBatchHold *
timed_login_task_begin_auto_login (gpointer user_data)
{
  TimedLoginState *state = user_data;
  ShellLoginDialog *self = state->dialog;

  if (self == NULL)
    return NULL;

  g_clear_object (&self->timed_login_batch);

  if (self->greeter != NULL)
    gdm_greeter_call_begin_auto_login (self->greeter, state->user_name, self->cancellable,
                                       greeter_begin_auto_login_done_cb, NULL);

  return NULL;
}

/* `_startTimedLogin ()` */
static void
start_timed_login (ShellLoginDialog *self,
                   const char       *user_name,
                   int               delay)
{
  static const ShellBatchTaskFunc task_funcs[] = {
    timed_login_task_wait_for_item,
    timed_login_task_find_item,
    timed_login_task_jump_to_item,
    timed_login_task_block_until_idle,
    timed_login_task_show_item,
    timed_login_task_show_indicator,
    timed_login_task_begin_auto_login,
  };
  TimedLoginState *state;
  ShellBatchTask *tasks[G_N_ELEMENTS (task_funcs)];
  gboolean first_run = TRUE;
  guint i;

  /* Cancel execution of old batch */
  if (self->timed_login_batch != NULL)
    {
      shell_batch_cancel (SHELL_BATCH (self->timed_login_batch));
      g_clear_object (&self->timed_login_batch);
      first_run = FALSE;
    }

  /* Reset previous idle-timeout */
  if (self->timed_login_idle_timeout_id != 0)
    {
      g_source_remove (self->timed_login_idle_timeout_id);
      self->timed_login_idle_timeout_id = 0;
    }

  state = g_new0 (TimedLoginState, 1);
  state->ref_count = 1;
  g_set_weak_pointer (&state->dialog, self);
  state->user_name = g_strdup (user_name);
  state->delay = delay;
  state->first_run = first_run;

  /* The tasks are functions that return a hold to wait for, or nothing */
  for (i = 0; i < G_N_ELEMENTS (task_funcs); i++)
    tasks[i] = shell_batch_task_new (task_funcs[i],
                                     timed_login_state_ref (state),
                                     (GDestroyNotify) timed_login_state_unref);
  timed_login_state_unref (state);

  self->timed_login_batch = SHELL_BATCH (shell_consecutive_batch_new (tasks, G_N_ELEMENTS (tasks)));
  for (i = 0; i < G_N_ELEMENTS (tasks); i++)
    g_object_unref (tasks[i]);

  shell_batch_run (self->timed_login_batch);
}

/* What the actions which restart the timed login know */
typedef struct
{
  ShellLoginDialog *dialog;     /* weak */
  char *user_name;
  int seconds;
} TimedLoginRequest;

static void
timed_login_request_free (TimedLoginRequest *request)
{
  g_clear_weak_pointer (&request->dialog);
  g_free (request->user_name);
  g_free (request);
}

static gboolean
timed_login_click_may_recognize_cb (ClutterGesture    *gesture,
                                    TimedLoginRequest *request)
{
  if (request->dialog != NULL)
    start_timed_login (request->dialog, request->user_name, request->seconds);

  return FALSE;
}

static gboolean
timed_login_key_press_cb (ClutterKeyController *controller,
                          TimedLoginRequest    *request)
{
  if (request->dialog != NULL)
    start_timed_login (request->dialog, request->user_name, request->seconds);

  return CLUTTER_EVENT_PROPAGATE;
}

/* `_onTimedLoginRequested ()` */
static void
greeter_timed_login_requested_cb (GdmGreeter       *greeter,
                                  const char       *user_name,
                                  int               seconds,
                                  ShellLoginDialog *self)
{
  ClutterActor *stage = CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ()));
  ClutterAction *click_gesture;
  ClutterAction *key_controller;
  TimedLoginRequest *request;

  if (self->timed_login_batch != NULL)
    return;

  start_timed_login (self, user_name, seconds);

  /* Restart timed login on user interaction */
  request = g_new0 (TimedLoginRequest, 1);
  g_set_weak_pointer (&request->dialog, self);
  request->user_name = g_strdup (user_name);
  request->seconds = seconds;

  click_gesture = clutter_click_gesture_new ();
  g_object_ref_sink (click_gesture);
  clutter_click_gesture_set_recognize_on_press (CLUTTER_CLICK_GESTURE (click_gesture), TRUE);
  g_signal_connect_data (click_gesture, "may-recognize",
                         G_CALLBACK (timed_login_click_may_recognize_cb), request,
                         (GClosureNotify) timed_login_request_free, 0);
  clutter_actor_add_action_full (stage, "timed-login-click-capture",
                                 CLUTTER_PHASE_CAPTURE, click_gesture);
  g_ptr_array_add (self->timed_login_actions, click_gesture);

  request = g_new0 (TimedLoginRequest, 1);
  g_set_weak_pointer (&request->dialog, self);
  request->user_name = g_strdup (user_name);
  request->seconds = seconds;

  key_controller = clutter_key_controller_new (NULL);
  g_object_ref_sink (key_controller);
  g_signal_connect_data (key_controller, "key-press",
                         G_CALLBACK (timed_login_key_press_cb), request,
                         (GClosureNotify) timed_login_request_free, 0);
  clutter_actor_add_action_full (stage, "timed-login-key-capture",
                                 CLUTTER_PHASE_CAPTURE, key_controller);
  g_ptr_array_add (self->timed_login_actions, key_controller);
}

/* ---- the greeter ---- */

static void
greeter_connection_closed_cb (GDBusConnection  *connection,
                              gboolean          remote_peer_vanished,
                              GError           *error,
                              ShellLoginDialog *self)
{
  if (self->greeter_connection_closed_id != 0)
    g_signal_handler_disconnect (connection, self->greeter_connection_closed_id);
  self->greeter_connection_closed_id = 0;

  if (self->greeter != NULL)
    g_signal_handlers_disconnect_by_data (self->greeter, self);
  g_clear_object (&self->greeter);

  ensure_greeter_proxy (self);
}

static void
ensure_greeter_proxy (ShellLoginDialog *self)
{
  g_autoptr (GError) error = NULL;
  GDBusConnection *connection;

  if (g_strcmp0 (g_getenv ("GDM_GREETER_TEST"), "1") == 0)
    return;

  if (self->greeter != NULL)
    return;

  self->greeter = gdm_client_get_greeter_sync (self->gdm_client, NULL, &error);
  if (self->greeter == NULL)
    {
      g_warning ("%s: Could not get the greeter: %s", G_STRFUNC, error->message);
      return;
    }

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (self->greeter));
  self->greeter_connection_closed_id = g_signal_connect (connection, "closed",
                                                         G_CALLBACK (greeter_connection_closed_cb),
                                                         self);

  g_signal_connect (self->greeter, "default-session-name-changed",
                    G_CALLBACK (default_session_name_changed_cb), self);
  g_signal_connect (self->greeter, "session-opened",
                    G_CALLBACK (greeter_session_opened_cb), self);
  g_signal_connect (self->greeter, "timed-login-requested",
                    G_CALLBACK (greeter_timed_login_requested_cb), self);
}

/* `_onReset ()` */
static void
auth_prompt_reset_cb (ShellAuthPrompt  *prompt,
                      guint             reset_type,
                      ShellLoginDialog *self)
{
  g_autofree char *previous_user_name = NULL;

  ensure_greeter_proxy (self);
  show_bottom_button_group (self);

  previous_user_name = g_steal_pointer (&self->user_name);

  if (self->next_signal_id != 0)
    {
      g_signal_handler_disconnect (self->auth_prompt, self->next_signal_id);
      self->next_signal_id = 0;
    }

  if (previous_user_name != NULL && reset_type == SHELL_AUTH_PROMPT_RESET_REUSE_USERNAME)
    {
      self->user_name = g_strdup (previous_user_name);
      shell_auth_prompt_set_user (self->auth_prompt, login_dialog_get_user (self));
      shell_auth_prompt_begin (self->auth_prompt, self->user_name, NULL);
    }
  else if (reset_type == SHELL_AUTH_PROMPT_RESET_PROVIDE_USERNAME)
    {
      if (self->disable_user_list != 1)
        show_user_list (self);
      else
        hide_user_list_ask_for_username_and_begin_verification (self);
    }
  else
    {
      hide_user_list_and_begin_verification (self);
    }
}

static void
auth_prompt_verification_complete_cb (ShellAuthPrompt  *prompt,
                                      ShellLoginDialog *self)
{
  hide_bottom_button_group (self);
}

static void
auth_prompt_loading_cb (ShellAuthPrompt  *prompt,
                        gboolean          is_loading,
                        ShellLoginDialog *self)
{
  clutter_actor_set_reactive (CLUTTER_ACTOR (self->auth_menu_button), !is_loading);
}

static void
auth_prompt_mechanisms_changed_cb (ShellAuthPrompt  *prompt,
                                   GVariant         *mechanisms,
                                   GVariant         *selected_mechanism,
                                   ShellLoginDialog *self)
{
  g_autoptr (GVariant) criteria = g_variant_ref_sink (section_criteria (primary_login_method_section_name ()));
  gsize n_mechanisms = g_variant_n_children (mechanisms);
  gsize i;

  shell_auth_menu_button_clear_items (self->auth_menu_button, criteria);

  if (n_mechanisms == 0)
    return;

  for (i = 0; i < n_mechanisms; i++)
    {
      g_autoptr (GVariant) mechanism = g_variant_get_child_value (mechanisms, i);
      gboolean selectable = FALSE;

      g_variant_lookup (mechanism, "selectable", "b", &selectable);
      if (selectable)
        shell_auth_menu_button_add_item (self->auth_menu_button,
                                         mechanism_item (primary_login_method_section_name (),
                                                         mechanism));
    }

  if (g_variant_n_children (selected_mechanism) > 0)
    shell_auth_menu_button_set_active_item (self->auth_menu_button, selected_mechanism);
}

static void
auth_prompt_status_notify_cb (GObject          *object,
                              GParamSpec       *pspec,
                              ShellLoginDialog *self)
{
  update_cancel_button (self);
}

/* `_loginScreenSessionActivated ()` */
static void
login_screen_session_activated (ShellLoginDialog *self)
{
  ShellEaseParams params = ease_params (FADE_ANIMATION_TIME, -1, CLUTTER_EASE_OUT_QUAD);
  ShellAuthPromptStatus status = shell_auth_prompt_get_verification_status (self->auth_prompt);

  if (clutter_actor_get_opacity (CLUTTER_ACTOR (self)) == 255 &&
      status == SHELL_AUTH_PROMPT_STATUS_NOT_VERIFYING)
    return;

  if (status != SHELL_AUTH_PROMPT_STATUS_NOT_VERIFYING)
    shell_auth_prompt_reset (self->auth_prompt, FALSE, FALSE);

  bind_opacity (self);
  shell_actor_ease (CLUTTER_ACTOR (self), &params,
                    unbind_opacity_done_cb, g_object_ref (self), g_object_unref,
                    "opacity", 255u,
                    NULL);
}

static void
greeter_session_properties_changed_cb (GDBusProxy       *proxy,
                                       GVariant         *changed_properties,
                                       GStrv             invalidated_properties,
                                       ShellLoginDialog *self)
{
  g_autoptr (GVariant) active = g_variant_lookup_value (changed_properties, "Active", NULL);

  if (active != NULL && shell_login1_session_get_active (SHELL_LOGIN1_SESSION (proxy)))
    login_screen_session_activated (self);
}

static void
greeter_session_ready_cb (GObject      *source,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  g_autoptr (ShellLoginDialog) self = user_data;
  g_autoptr (GError) error = NULL;
  ShellLogin1Session *session;

  session = shell_login_manager_get_current_session_finish (SHELL_LOGIN_MANAGER (source), res, &error);
  if (session == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Could not get a proxy for the greeter session: %s", error->message);
      return;
    }

  g_clear_object (&self->greeter_session_proxy);
  self->greeter_session_proxy = session;
  g_signal_connect_object (session, "g-properties-changed",
                           G_CALLBACK (greeter_session_properties_changed_cb), self, 0);
}

/* ---- the banner and the logo ---- */

static void
set_banner_text (ShellLoginDialog *self,
                 const char       *text)
{
  if (self->banner_label == NULL)
    return;

  if (text != NULL && text[0] != '\0')
    {
      st_label_set_text (self->banner_label, text);
      clutter_actor_show (CLUTTER_ACTOR (self->banner_label));
    }
  else
    {
      clutter_actor_hide (CLUTTER_ACTOR (self->banner_label));
    }
}

static void
banner_contents_loaded_cb (GObject      *source,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  g_autoptr (ShellLoginDialog) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *contents = NULL;
  gsize length = 0;

  if (!g_file_load_contents_finish (G_FILE (source), res, &contents, &length, NULL, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_autofree char *path = g_file_get_path (G_FILE (source));

          g_message ("Failed to read banner from %s: %s", path, error->message);
          set_banner_text (self, NULL);
        }
      return;
    }

  /* TextDecoder replaces what is not valid UTF-8 */
  {
    g_autofree char *text = g_utf8_make_valid (contents, length);

    set_banner_text (self, text);
  }
}

/* `_updateBanner ()`, with `_getBannerText ()` */
static void
update_banner (ShellLoginDialog *self)
{
  gboolean enabled = g_settings_get_boolean (self->settings, SHELL_GDM_BANNER_MESSAGE_KEY);

  if (!enabled)
    {
      set_banner_text (self, NULL);
      return;
    }

  if (self->banner_message_file != NULL)
    {
      g_file_load_contents_async (self->banner_message_file, self->cancellable,
                                  banner_contents_loaded_cb, g_object_ref (self));
      return;
    }

  {
    g_autofree char *text = g_settings_get_string (self->settings,
                                                   SHELL_GDM_BANNER_MESSAGE_TEXT_KEY);

    set_banner_text (self, text);
  }
}

static void
banner_message_file_changed_cb (GFileMonitor      *monitor,
                                GFile             *file,
                                GFile             *other_file,
                                GFileMonitorEvent  event_type,
                                ShellLoginDialog  *self)
{
  update_banner (self);
}

/* `_updateBannerMessageFile ()`; returns whether the file changed */
static gboolean
update_banner_message_file (ShellLoginDialog *self)
{
  g_autofree char *source = g_settings_get_string (self->settings,
                                                   SHELL_GDM_BANNER_MESSAGE_SOURCE_KEY);
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;

  if (g_strcmp0 (source, "file") == 0)
    path = g_settings_get_string (self->settings, SHELL_GDM_BANNER_MESSAGE_PATH_KEY);

  if (path != NULL && path[0] != '\0')
    file = g_file_new_for_path (path);

  if (file == NULL && self->banner_message_file == NULL)
    return FALSE;

  if (file != NULL && self->banner_message_file != NULL &&
      g_file_equal (self->banner_message_file, file))
    return FALSE;

  if (self->banner_message_monitor != NULL)
    g_signal_handlers_disconnect_by_data (self->banner_message_monitor, self);
  g_clear_object (&self->banner_message_monitor);

  g_clear_object (&self->banner_message_file);
  self->banner_message_file = g_steal_pointer (&file);

  if (self->banner_message_file != NULL)
    {
      g_autoptr (GError) error = NULL;

      self->banner_message_monitor = g_file_monitor_file (self->banner_message_file,
                                                          G_FILE_MONITOR_NONE, NULL, &error);
      if (self->banner_message_monitor != NULL)
        g_signal_connect (self->banner_message_monitor, "changed",
                          G_CALLBACK (banner_message_file_changed_cb), self);
      else
        g_message ("Failed to monitor the banner file: %s", error->message);
    }

  return TRUE;
}

static void
banner_setting_changed_cb (GSettings        *settings,
                           const char       *key,
                           ShellLoginDialog *self)
{
  update_banner (self);
}

static void
banner_file_setting_changed_cb (GSettings        *settings,
                                const char       *key,
                                ShellLoginDialog *self)
{
  if (update_banner_message_file (self))
    update_banner (self);
}

/* `_updateLogoTexture ()` */
static void
update_logo_texture (ShellLoginDialog *self,
                     GFile            *file)
{
  float resource_scale;

  if (self->logo_file != NULL &&
      (file == NULL || !g_file_equal (self->logo_file, file)))
    return;

  clutter_actor_destroy_all_children (self->logo_bin);
  resource_scale = clutter_actor_get_resource_scale (self->logo_bin);

  if (self->logo_file != NULL)
    {
      ClutterStage *stage = shell_global_get_stage (shell_global_get ());
      int scale_factor = st_theme_context_get_scale_factor (st_theme_context_get_for_stage (stage));
      ClutterActor *texture;

      texture = st_texture_cache_load_file_async (self->texture_cache, self->logo_file,
                                                  -1, -1, scale_factor, resource_scale);
      clutter_actor_add_child (self->logo_bin, texture);
    }
}

static void
texture_file_changed_cb (StTextureCache   *cache,
                         GFile            *file,
                         ShellLoginDialog *self)
{
  update_logo_texture (self, file);
}

static void
logo_resource_scale_changed_cb (ClutterActor     *actor,
                                ShellLoginDialog *self)
{
  update_logo_texture (self, self->logo_file);
}

/* `_updateLogo ()` */
static void
update_logo (ShellLoginDialog *self)
{
  g_autofree char *path = g_settings_get_string (self->settings, SHELL_GDM_LOGO_KEY);

  g_clear_object (&self->logo_file);
  if (path != NULL && path[0] != '\0')
    self->logo_file = g_file_new_for_path (path);

  update_logo_texture (self, self->logo_file);
}

static void
logo_setting_changed_cb (GSettings        *settings,
                         const char       *key,
                         ShellLoginDialog *self)
{
  update_logo (self);
}

/* ---- the allocation ---- */

/* `_getBannerAllocation ()` */
static void
get_banner_allocation (ShellLoginDialog      *self,
                       const ClutterActorBox *dialog_box,
                       ClutterActorBox       *actor_box)
{
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  float nat_width, nat_height;
  float center_x = dialog_box->x1 + (dialog_box->x2 - dialog_box->x1) / 2;
  float panel_height = 0;

  clutter_actor_get_preferred_size (self->banner_view, NULL, NULL, &nat_width, &nat_height);

  if (layout_manager != NULL)
    panel_height = clutter_actor_get_height (CLUTTER_ACTOR (shell_layout_manager_get_panel_box (layout_manager)));

  actor_box->x1 = floorf (center_x - nat_width / 2);
  actor_box->y1 = dialog_box->y1 + panel_height;
  actor_box->x2 = actor_box->x1 + nat_width;
  actor_box->y2 = actor_box->y1 + nat_height;
}

/* `_getLogoBinAllocation ()` */
static void
get_logo_bin_allocation (ShellLoginDialog      *self,
                         const ClutterActorBox *dialog_box,
                         ClutterActorBox       *actor_box)
{
  float nat_width, nat_height;
  float center_x = dialog_box->x1 + (dialog_box->x2 - dialog_box->x1) / 2;

  clutter_actor_get_preferred_size (self->logo_bin, NULL, NULL, &nat_width, &nat_height);

  actor_box->x1 = floorf (center_x - nat_width / 2);
  actor_box->y1 = dialog_box->y2 - nat_height;
  actor_box->x2 = actor_box->x1 + nat_width;
  actor_box->y2 = actor_box->y1 + nat_height;
}

/* `_getBottomButtonGroupAllocation ()` */
static void
get_bottom_button_group_allocation (ShellLoginDialog      *self,
                                    const ClutterActorBox *dialog_box,
                                    ClutterActorBox       *actor_box)
{
  float nat_width, nat_height;

  clutter_actor_get_preferred_size (self->bottom_button_group, NULL, NULL, &nat_width, &nat_height);

  if (clutter_actor_get_text_direction (CLUTTER_ACTOR (self)) == CLUTTER_TEXT_DIRECTION_RTL)
    actor_box->x1 = dialog_box->x1;
  else
    actor_box->x1 = dialog_box->x2 - nat_width;

  actor_box->y1 = dialog_box->y2 - nat_height;
  actor_box->x2 = actor_box->x1 + nat_width;
  actor_box->y2 = actor_box->y1 + nat_height;
}

/* `_getCenterActorAllocation ()` */
static void
get_center_actor_allocation (const ClutterActorBox *dialog_box,
                             ClutterActor          *actor,
                             float                  offset_y,
                             ClutterActorBox       *actor_box)
{
  float nat_width, nat_height;
  float center_x = dialog_box->x1 + (dialog_box->x2 - dialog_box->x1) / 2;
  float center_y = dialog_box->y1 + (dialog_box->y2 - dialog_box->y1) / 2;

  clutter_actor_get_preferred_size (actor, NULL, NULL, &nat_width, &nat_height);

  nat_width = MIN (nat_width, dialog_box->x2 - dialog_box->x1);
  nat_height = MIN (nat_height, dialog_box->y2 - dialog_box->y1);

  actor_box->x1 = floorf (center_x - nat_width / 2);
  actor_box->y1 = floorf (center_y - nat_height / 2) + offset_y;
  actor_box->x2 = actor_box->x1 + nat_width;
  actor_box->y2 = actor_box->y1 + nat_height;
}

static void
shell_login_dialog_allocate (ClutterActor          *actor,
                             const ClutterActorBox *box)
{
  ShellLoginDialog *self = SHELL_LOGIN_DIALOG (actor);
  StThemeNode *theme_node;
  ClutterActorBox dialog_box;
  ClutterActorBox banner_allocation = { 0, };
  ClutterActorBox auth_prompt_allocation = { 0, };
  ClutterActorBox user_selection_allocation = { 0, };
  ClutterActorBox logo_allocation = { 0, };
  ClutterActorBox bottom_button_group_allocation = { 0, };
  gboolean have_banner = FALSE;
  gboolean have_auth_prompt = FALSE;
  gboolean have_user_selection = FALSE;
  gboolean have_logo = FALSE;
  gboolean have_bottom_button_group = FALSE;
  float dialog_width, dialog_height;
  float banner_height = 0;
  float auth_prompt_width = 0;
  float user_selection_height = 0;
  float logo_height = 0;

  clutter_actor_set_allocation (actor, box);

  theme_node = st_widget_get_theme_node (ST_WIDGET (actor));
  st_theme_node_get_content_box (theme_node, box, &dialog_box);

  dialog_width = dialog_box.x2 - dialog_box.x1;
  dialog_height = dialog_box.y2 - dialog_box.y1;

  /* First find out what space the children require */
  if (clutter_actor_is_visible (self->banner_view))
    {
      have_banner = TRUE;
      get_banner_allocation (self, &dialog_box, &banner_allocation);
      banner_height = banner_allocation.y2 - banner_allocation.y1;
    }

  if (clutter_actor_is_visible (CLUTTER_ACTOR (self->auth_prompt)))
    {
      have_auth_prompt = TRUE;
      get_center_actor_allocation (&dialog_box, CLUTTER_ACTOR (self->auth_prompt),
                                   AUTH_PROMPT_VERTICAL_CENTER_OFFSET, &auth_prompt_allocation);
      auth_prompt_width = auth_prompt_allocation.x2 - auth_prompt_allocation.x1;
    }

  if (clutter_actor_is_visible (self->user_selection_box))
    {
      have_user_selection = TRUE;
      get_center_actor_allocation (&dialog_box, self->user_selection_box, 0,
                                   &user_selection_allocation);
      user_selection_height = user_selection_allocation.y2 - user_selection_allocation.y1;
    }

  if (clutter_actor_is_visible (self->logo_bin))
    {
      have_logo = TRUE;
      get_logo_bin_allocation (self, &dialog_box, &logo_allocation);
      logo_height = logo_allocation.y2 - logo_allocation.y1;
    }

  if (clutter_actor_is_visible (self->bottom_button_group))
    {
      have_bottom_button_group = TRUE;
      get_bottom_button_group_allocation (self, &dialog_box, &bottom_button_group_allocation);
    }

  /* Then figure out if we're overly constrained and need to
   * try a different layout, or if we have what extra space we
   * can hand out */
  if (have_banner)
    {
      float banner_space;
      float left_over_y_space;

      if (have_auth_prompt)
        banner_space = auth_prompt_allocation.y1 - banner_allocation.y1;
      else
        banner_space = 0;

      left_over_y_space = banner_space - banner_height;

      if (left_over_y_space > 0)
        {
          /* First figure out how much left over space is up top */
          float left_over_top_space = left_over_y_space / 2;

          /* Then, shift the banner into the middle of that extra space */
          float y_shift = floorf (left_over_top_space / 2);

          banner_allocation.y1 += y_shift;
          banner_allocation.y2 += y_shift;
        }
      else
        {
          /* Then figure out how much space there would be if we switched to a
           * wide layout with banner on one side and authprompt on the other. */
          float left_over_x_space = dialog_width - auth_prompt_width;

          /* In a wide view, half of the available space goes to the banner,
           * and the other half goes to the margins. */
          float wide_banner_width = left_over_x_space / 2;
          float wide_spacing = left_over_x_space - wide_banner_width;

          /* If we do go with a wide layout, we need there to be at least enough
           * space for the banner and the auth prompt to be the same width,
           * so it doesn't look unbalanced. */
          if (auth_prompt_width > 0 && wide_banner_width > auth_prompt_width)
            {
              float center_x = dialog_box.x1 + dialog_width / 2;
              float center_y = dialog_box.y1 + dialog_height / 2;
              float center_gap, wide_banner_height, max_wide_height;

              /* A small portion of the spacing goes down the center of the
               * screen to help delimit the two columns of the wide view */
              center_gap = wide_spacing / 8;

              /* place the banner along the left edge of the center margin */
              banner_allocation.x2 = floorf (center_x - center_gap / 2);
              banner_allocation.x1 = floorf (banner_allocation.x2 - wide_banner_width);

              /* figure out how tall it would like to be and try to accommodate
               * but don't let it get too close to the logo */
              clutter_actor_get_preferred_height (self->banner_view, wide_banner_width,
                                                  NULL, &wide_banner_height);

              max_wide_height = dialog_height - 3 * logo_height;
              wide_banner_height = MIN (max_wide_height, wide_banner_height);
              banner_allocation.y1 = floorf (center_y - wide_banner_height / 2);
              banner_allocation.y2 = banner_allocation.y1 + wide_banner_height;

              /* place the auth prompt along the right edge of the center margin */
              auth_prompt_allocation.x1 = floorf (center_x + center_gap / 2);
              auth_prompt_allocation.x2 = auth_prompt_allocation.x1 + auth_prompt_width;
            }
          else
            {
              /* If we aren't going to do a wide view, then we need to limit
               * the height of the banner so it will present scrollbars

               * Give 70% of the banner space to the banner itself, and
               * split the remaining 30% evenly above and below it */
              float banner_spacing = floorf (0.3 * banner_space);

              banner_allocation.y1 += floorf (banner_spacing / 2);
              banner_allocation.y2 = banner_allocation.y1 + (banner_space - banner_spacing);
            }
        }
    }
  else if (have_user_selection)
    {
      /* Grow the user list to fill the space */
      float left_over_y_space = dialog_height - user_selection_height - logo_height;

      if (left_over_y_space > 0)
        {
          float top_expansion = floorf (left_over_y_space / 2);
          float bottom_expansion = top_expansion;

          user_selection_allocation.y1 -= top_expansion;
          user_selection_allocation.y2 += bottom_expansion;
        }
    }

  /* Finally hand out the allocations */
  if (have_banner)
    clutter_actor_allocate (self->banner_view, &banner_allocation);

  if (have_auth_prompt)
    clutter_actor_allocate (CLUTTER_ACTOR (self->auth_prompt), &auth_prompt_allocation);

  if (have_user_selection)
    clutter_actor_allocate (self->user_selection_box, &user_selection_allocation);

  if (have_logo)
    clutter_actor_allocate (self->logo_bin, &logo_allocation);

  if (have_bottom_button_group)
    clutter_actor_allocate (self->bottom_button_group, &bottom_button_group_allocation);
}

/* ---- the actor ---- */

static void
shell_login_dialog_constructed (GObject *object)
{
  ShellLoginDialog *self = SHELL_LOGIN_DIALOG (object);
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  ShellMonitorConstraint *constraint;
  g_autoptr (GError) error = NULL;
  const char *extensions[3];
  ClutterActor *banner_box;
  ClutterActor *not_listed_label;

  G_OBJECT_CLASS (shell_login_dialog_parent_class)->constructed (object);

  self->disable_user_list = -1;
  self->bindings = g_ptr_array_new_with_free_func (g_object_unref);
  self->timed_login_actions = g_ptr_array_new_with_free_func (g_object_unref);
  self->cancellable = g_cancellable_new ();

  clutter_actor_set_accessible_role (CLUTTER_ACTOR (self), ATK_ROLE_WINDOW);

  constraint = shell_monitor_constraint_new ();
  shell_monitor_constraint_set_primary (constraint, TRUE);
  clutter_actor_add_constraint (CLUTTER_ACTOR (self), CLUTTER_CONSTRAINT (constraint));

  self->user_manager = g_object_ref (act_user_manager_get_default ());
  self->gdm_client = gdm_client_new ();

  extensions[0] = gdm_user_verifier_choice_list_interface_info ()->name;
  extensions[1] = gdm_user_verifier_custom_json_interface_info ()->name;
  extensions[2] = NULL;
  gdm_client_set_enabled_extensions (self->gdm_client, extensions);

  self->settings = g_settings_new (SHELL_GDM_LOGIN_SCREEN_SCHEMA);

  g_signal_connect (self->settings, "changed::" SHELL_GDM_BANNER_MESSAGE_KEY,
                    G_CALLBACK (banner_setting_changed_cb), self);
  g_signal_connect (self->settings, "changed::" SHELL_GDM_BANNER_MESSAGE_TEXT_KEY,
                    G_CALLBACK (banner_setting_changed_cb), self);
  g_signal_connect (self->settings, "changed::" SHELL_GDM_BANNER_MESSAGE_SOURCE_KEY,
                    G_CALLBACK (banner_file_setting_changed_cb), self);
  g_signal_connect (self->settings, "changed::" SHELL_GDM_BANNER_MESSAGE_PATH_KEY,
                    G_CALLBACK (banner_file_setting_changed_cb), self);
  g_signal_connect (self->settings, "changed::" SHELL_GDM_DISABLE_USER_LIST_KEY,
                    G_CALLBACK (disable_user_list_changed_cb), self);
  g_signal_connect (self->settings, "changed::" SHELL_GDM_LOGO_KEY,
                    G_CALLBACK (logo_setting_changed_cb), self);

  self->texture_cache = st_texture_cache_get_default ();
  g_signal_connect_object (self->texture_cache, "texture-file-changed",
                           G_CALLBACK (texture_file_changed_cb), self, 0);

  self->user_selection_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                           "style-class", "login-dialog-user-selection-box",
                                           "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                           "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                           "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                           "visible", FALSE,
                                           NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->user_selection_box);

  self->user_list = shell_user_list_new ();
  clutter_actor_add_child (self->user_selection_box, CLUTTER_ACTOR (self->user_list));

  self->auth_prompt = shell_auth_prompt_new (self->gdm_client,
                                             SHELL_AUTH_PROMPT_MODE_UNLOCK_OR_LOG_IN);
  g_signal_connect (self->auth_prompt, "prompted",
                    G_CALLBACK (auth_prompt_prompted_cb), self);
  g_signal_connect (self->auth_prompt, "reset",
                    G_CALLBACK (auth_prompt_reset_cb), self);
  g_signal_connect (self->auth_prompt, "verification-complete",
                    G_CALLBACK (auth_prompt_verification_complete_cb), self);
  g_signal_connect (self->auth_prompt, "loading",
                    G_CALLBACK (auth_prompt_loading_cb), self);
  g_signal_connect (self->auth_prompt, "mechanisms-changed",
                    G_CALLBACK (auth_prompt_mechanisms_changed_cb), self);
  g_signal_connect (self->auth_prompt, "notify::verification-status",
                    G_CALLBACK (auth_prompt_status_notify_cb), self);
  clutter_actor_hide (CLUTTER_ACTOR (self->auth_prompt));
  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (self->auth_prompt));

  /* translators: this message is shown below the user list on the
   * login screen. It can be activated to reveal an entry for
   * manually entering the username. */
  not_listed_label = g_object_new (ST_TYPE_LABEL,
                                   "text", _("Not listed?"),
                                   "style-class", "login-dialog-not-listed-label",
                                   NULL);
  self->not_listed_button = g_object_new (ST_TYPE_BUTTON,
                                          "style-class", "login-dialog-not-listed-button",
                                          "button-mask", ST_BUTTON_PRIMARY | ST_BUTTON_SECONDARY,
                                          "can-focus", TRUE,
                                          "child", not_listed_label,
                                          "reactive", TRUE,
                                          "x-align", CLUTTER_ACTOR_ALIGN_START,
                                          "label-actor", not_listed_label,
                                          NULL);
  g_signal_connect (self->not_listed_button, "clicked",
                    G_CALLBACK (not_listed_clicked_cb), self);
  clutter_actor_hide (self->not_listed_button);
  clutter_actor_add_child (self->user_selection_box, self->not_listed_button);

  banner_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                             "orientation", CLUTTER_ORIENTATION_VERTICAL,
                             NULL);

  self->banner_view = g_object_new (ST_TYPE_SCROLL_VIEW,
                                    "style-class", "login-dialog-banner-view",
                                    "opacity", 0,
                                    "child", banner_box,
                                    NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->banner_view);

  self->banner_label = g_object_new (ST_TYPE_LABEL,
                                     "style-class", "login-dialog-banner",
                                     "text", "",
                                     NULL);
  clutter_text_set_line_wrap (st_label_get_clutter_text (self->banner_label), TRUE);
  clutter_text_set_ellipsize (st_label_get_clutter_text (self->banner_label), PANGO_ELLIPSIZE_NONE);
  clutter_actor_add_child (banner_box, CLUTTER_ACTOR (self->banner_label));

  update_banner_message_file (self);
  update_banner (self);

  self->bottom_button_group = g_object_new (ST_TYPE_BOX_LAYOUT,
                                            "style-class", "login-dialog-bottom-button-group",
                                            NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->bottom_button_group);

  create_auth_menu_button (self);

  self->a11y_menu_button = shell_a11y_menu_button_new ();
  clutter_actor_add_child (self->bottom_button_group, self->a11y_menu_button);

  self->logo_bin = g_object_new (ST_TYPE_WIDGET,
                                 "style-class", "login-dialog-logo-bin",
                                 "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                 "y-align", CLUTTER_ACTOR_ALIGN_END,
                                 NULL);
  g_signal_connect (self->logo_bin, "resource-scale-changed",
                    G_CALLBACK (logo_resource_scale_changed_cb), self);
  clutter_actor_add_child (CLUTTER_ACTOR (self), self->logo_bin);
  update_logo (self);

  g_signal_connect (self->user_list, "activate",
                    G_CALLBACK (user_list_activate_cb), self);

  self->realm_manager = shell_realmd_manager_new ();
  g_signal_connect (self->realm_manager, "login-format-changed",
                    G_CALLBACK (realm_login_format_changed_cb), self);

  shell_login_manager_get_current_session_async (shell_login_manager_get_default (),
                                                 self->cancellable,
                                                 greeter_session_ready_cb,
                                                 g_object_ref (self));

  /* If the user list is enabled, it should take key focus; make sure the
   * screen shield is initialized first to prevent it from stealing the
   * focus later */
  if (layout_manager != NULL)
    g_signal_connect_object (layout_manager, "startup-complete",
                             G_CALLBACK (startup_complete_cb), self, 0);

}

static void
shell_login_dialog_dispose (GObject *object)
{
  ShellLoginDialog *self = SHELL_LOGIN_DIALOG (object);
  guint i;

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  if (self->timed_login_batch != NULL)
    shell_batch_cancel (SHELL_BATCH (self->timed_login_batch));
  g_clear_object (&self->timed_login_batch);
  g_clear_object (&self->timed_login_user_list_hold);

  if (self->timed_login_idle_timeout_id != 0)
    {
      g_source_remove (self->timed_login_idle_timeout_id);
      self->timed_login_idle_timeout_id = 0;
    }

  if (self->timed_login_actions != NULL)
    {
      for (i = 0; i < self->timed_login_actions->len; i++)
        {
          ClutterActorMeta *meta = g_ptr_array_index (self->timed_login_actions, i);
          ClutterActor *attached = clutter_actor_meta_get_actor (meta);

          /* A destroyed actor keeps the address, not the action */
          if (attached != NULL)
            clutter_actor_remove_action (attached, CLUTTER_ACTION (meta));
        }
      g_ptr_array_set_size (self->timed_login_actions, 0);
    }

  if (self->bindings != NULL)
    unbind_opacity (self);

  if (self->settings != NULL)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  g_clear_object (&self->settings);

  if (self->banner_message_monitor != NULL)
    g_signal_handlers_disconnect_by_data (self->banner_message_monitor, self);
  g_clear_object (&self->banner_message_monitor);
  g_clear_object (&self->banner_message_file);

  if (self->greeter != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->greeter, self);
      if (self->greeter_connection_closed_id != 0)
        g_signal_handler_disconnect (g_dbus_proxy_get_connection (G_DBUS_PROXY (self->greeter)),
                                     self->greeter_connection_closed_id);
      self->greeter_connection_closed_id = 0;
    }
  g_clear_object (&self->greeter);
  g_clear_object (&self->greeter_session_proxy);

  if (self->realm_manager != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->realm_manager, self);
      shell_realmd_manager_release (self->realm_manager);
    }
  g_clear_object (&self->realm_manager);

  if (self->user_manager != NULL)
    g_signal_handlers_disconnect_by_data (self->user_manager, self);
  g_clear_object (&self->user_manager);

  g_clear_object (&self->grab);
  g_clear_pointer (&self->user_name, g_free);
  g_clear_object (&self->gdm_client);
  g_clear_object (&self->logo_file);
  g_clear_pointer (&self->selected_auth_mechanism, g_variant_unref);
  g_clear_weak_pointer (&self->conflicting_session_notification);

  self->texture_cache = NULL;
  self->user_selection_box = NULL;
  self->user_list = NULL;
  self->auth_prompt = NULL;
  self->not_listed_button = NULL;
  self->banner_view = NULL;
  self->banner_label = NULL;
  self->bottom_button_group = NULL;
  self->auth_menu_button = NULL;
  self->a11y_menu_button = NULL;
  self->logo_bin = NULL;

  G_OBJECT_CLASS (shell_login_dialog_parent_class)->dispose (object);
}

static void
shell_login_dialog_finalize (GObject *object)
{
  ShellLoginDialog *self = SHELL_LOGIN_DIALOG (object);

  g_clear_pointer (&self->bindings, g_ptr_array_unref);
  g_clear_pointer (&self->timed_login_actions, g_ptr_array_unref);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (shell_login_dialog_parent_class)->finalize (object);
}

static void
shell_login_dialog_class_init (ShellLoginDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_login_dialog_constructed;
  object_class->dispose = shell_login_dialog_dispose;
  object_class->finalize = shell_login_dialog_finalize;

  actor_class->allocate = shell_login_dialog_allocate;

  /**
   * ShellLoginDialog::failed:
   * @self: the #ShellLoginDialog
   *
   * Emitted when the login failed. The login dialog never emits it, the
   * screen shield connects to the signal of both kinds of dialogs.
   */
  signals[FAILED] =
    g_signal_new ("failed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);

  /**
   * ShellLoginDialog::wake-up-screen:
   * @self: the #ShellLoginDialog
   *
   * Emitted when the screen should be woken up. The login dialog never emits
   * it either.
   */
  signals[WAKE_UP_SCREEN] =
    g_signal_new ("wake-up-screen",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_login_dialog_init (ShellLoginDialog *self)
{
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */

/**
 * shell_login_dialog_new:
 * @parent_actor: the actor the dialog is added to
 *
 * Creates the login screen (`new LoginDialog (parentActor)`).
 *
 * Returns: (transfer none) (nullable): the new #ShellLoginDialog, which
 *   belongs to @parent_actor, or %NULL if the schema of the login screen is
 *   not installed
 */
ShellLoginDialog *
shell_login_dialog_new (ClutterActor *parent_actor)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  g_autoptr (GSettingsSchema) schema = NULL;
  ShellLoginDialog *self;

  g_return_val_if_fail (CLUTTER_IS_ACTOR (parent_actor), NULL);

  if (source != NULL)
    schema = g_settings_schema_source_lookup (source, SHELL_GDM_LOGIN_SCREEN_SCHEMA, TRUE);
  if (schema == NULL)
    {
      g_message ("The schema %s is not installed, there is no login screen",
                 SHELL_GDM_LOGIN_SCREEN_SCHEMA);
      return NULL;
    }

  self = g_object_new (SHELL_TYPE_LOGIN_DIALOG,
                       "style-class", "login-dialog",
                       "visible", FALSE,
                       "reactive", TRUE,
                       NULL);
  clutter_actor_add_child (parent_actor, CLUTTER_ACTOR (self));

  return self;
}

/**
 * shell_login_dialog_activate:
 * @self: a #ShellLoginDialog
 *
 * Shows the dialog with the user list focused (`activate ()`).
 */
void
shell_login_dialog_activate (ShellLoginDialog *self)
{
  g_return_if_fail (SHELL_IS_LOGIN_DIALOG (self));

  clutter_actor_grab_key_focus (CLUTTER_ACTOR (self->user_list));
  clutter_actor_show (CLUTTER_ACTOR (self));
}

/**
 * shell_login_dialog_open:
 * @self: a #ShellLoginDialog
 *
 * Shows the dialog, makes it modal and fades it in (`open ()`).
 *
 * Returns: %TRUE
 */
gboolean
shell_login_dialog_open (ShellLoginDialog *self)
{
  ShellMain *main_obj = shell_main_get_default ();
  ShellWindowManager *window_manager = shell_main_get_window_manager (main_obj);
  ShellEaseParams params = ease_params (1000, -1, CLUTTER_EASE_IN_QUAD);

  g_return_val_if_fail (SHELL_IS_LOGIN_DIALOG (self), FALSE);

  if (window_manager != NULL)
    {
      ShellCtrlAltTabManager *ctrl_alt_tab = shell_window_manager_get_ctrl_alt_tab_manager (window_manager);

      if (ctrl_alt_tab != NULL)
        shell_ctrl_alt_tab_manager_add_group (ctrl_alt_tab, CLUTTER_ACTOR (self),
                                              _("Login Window"),
                                              "dialog-password-symbolic");
    }

  shell_login_dialog_activate (self);

  clutter_actor_set_opacity (CLUTTER_ACTOR (self), 0);

  g_clear_object (&self->grab);
  self->grab = shell_main_push_modal (main_obj,
                                      CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ())),
                                      SHELL_ACTION_MODE_LOGIN_SCREEN);
  if (self->grab != NULL)
    g_object_ref (self->grab);

  shell_actor_ease (CLUTTER_ACTOR (self), &params, NULL, NULL, NULL,
                    "opacity", 255u,
                    NULL);

  return TRUE;
}

/**
 * shell_login_dialog_close:
 * @self: a #ShellLoginDialog
 *
 * Drops the modal status of the dialog (`close ()`).
 */
void
shell_login_dialog_close (ShellLoginDialog *self)
{
  ShellMain *main_obj = shell_main_get_default ();
  ShellWindowManager *window_manager = shell_main_get_window_manager (main_obj);

  g_return_if_fail (SHELL_IS_LOGIN_DIALOG (self));

  if (self->grab != NULL)
    {
      g_autoptr (GError) error = NULL;

      shell_main_pop_modal (main_obj, self->grab, &error);
      g_clear_object (&self->grab);
    }

  if (window_manager != NULL)
    {
      ShellCtrlAltTabManager *ctrl_alt_tab = shell_window_manager_get_ctrl_alt_tab_manager (window_manager);

      if (ctrl_alt_tab != NULL)
        shell_ctrl_alt_tab_manager_remove_group (ctrl_alt_tab, CLUTTER_ACTOR (self));
    }
}

/**
 * shell_login_dialog_cancel:
 * @self: a #ShellLoginDialog
 *
 * Cancels the verification in progress (`cancel ()`).
 */
void
shell_login_dialog_cancel (ShellLoginDialog *self)
{
  g_return_if_fail (SHELL_IS_LOGIN_DIALOG (self));

  shell_auth_prompt_cancel (self->auth_prompt);
}

/**
 * shell_login_dialog_finish:
 * @self: a #ShellLoginDialog
 * @func: called when the dialog is done with the messages of the verifier
 * @user_data: (closure func): data for @func
 * @destroy: (nullable): called on @user_data after @func
 *
 * Waits for the messages of the verifier to be shown and calls @func
 * (`finish (onComplete)`).
 */
void
shell_login_dialog_finish (ShellLoginDialog          *self,
                           ShellAuthPromptFinishFunc  func,
                           gpointer                   user_data,
                           GDestroyNotify             destroy)
{
  g_return_if_fail (SHELL_IS_LOGIN_DIALOG (self));

  shell_auth_prompt_finish (self->auth_prompt, func, user_data, destroy);
}
