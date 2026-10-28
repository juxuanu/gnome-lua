/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auth-list.c: The list of choices an authentication service asks the user
 * to pick from. Port of js/gdm/authList.js.
 *
 * Copyright 2017 Red Hat, Inc
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The popup of the icon of an item (`ItemIconPopup`) is not a class of its
 *    own: the icon (a private #StButton) owns the menu and its manager.
 *  - The content of an item is the `a{sv}` the user verifier emits with the
 *    choice list, instead of a JS object.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <clutter/clutter-pango.h>
#include <graphene.h>
#include <pango/pango.h>

#include "gdm/auth-list.h"
#include "shell-action-modes.h"
#include "shell-global.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/popup-menu.h"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

/* The string value @key of an `a{sv}`, or %NULL */
static const char *
dict_get_string (GVariant   *dict,
                 const char *key)
{
  g_autoptr (GVariant) value = NULL;

  if (dict == NULL)
    return NULL;

  value = g_variant_lookup_value (dict, key, G_VARIANT_TYPE_STRING);
  if (value == NULL)
    return NULL;

  /* The string is owned by the dictionary, which outlives the value */
  return g_variant_get_string (value, NULL);
}

static void
add_ui_group_child (ClutterActor *actor)
{
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());

  if (layout_manager != NULL)
    clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager)),
                             actor);
}

/* ------------------------------------------------------------------------ */
/* ShellAuthListItemIcon                                                     */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_AUTH_LIST_ITEM_ICON (shell_auth_list_item_icon_get_type ())
G_DECLARE_FINAL_TYPE (ShellAuthListItemIcon, shell_auth_list_item_icon,
                      SHELL, AUTH_LIST_ITEM_ICON, StButton)

/* `ItemIcon`, with the `ItemIconPopup` it owns */
struct _ShellAuthListItemIcon
{
  StButton parent;

  ShellPopupMenu *menu;
  ShellPopupMenuManager *manager;
};

G_DEFINE_FINAL_TYPE (ShellAuthListItemIcon, shell_auth_list_item_icon, ST_TYPE_BUTTON)

static void
item_icon_clicked_cb (StButton              *button,
                      int                    clicked_button,
                      ShellAuthListItemIcon *self)
{
  if (self->menu != NULL)
    shell_popup_menu_base_toggle (SHELL_POPUP_MENU_BASE (self->menu));
}

static void
item_icon_destroy_cb (ClutterActor          *actor,
                      ShellAuthListItemIcon *self)
{
  if (self->menu != NULL)
    shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (self->menu));
}

static void
shell_auth_list_item_icon_dispose (GObject *object)
{
  ShellAuthListItemIcon *self = SHELL_AUTH_LIST_ITEM_ICON (object);

  g_clear_object (&self->manager);
  g_clear_object (&self->menu);

  G_OBJECT_CLASS (shell_auth_list_item_icon_parent_class)->dispose (object);
}

static void
shell_auth_list_item_icon_class_init (ShellAuthListItemIconClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_auth_list_item_icon_dispose;
}

static void
shell_auth_list_item_icon_init (ShellAuthListItemIcon *self)
{
}

/* `new ItemIcon (iconName, iconTitle, iconSubtitle)` */
static ClutterActor *
shell_auth_list_item_icon_new (const char *icon_name,
                               const char *title,
                               const char *subtitle)
{
  ShellAuthListItemIcon *self;
  ShellPopupMenuBase *menu_base;
  ClutterActor *labels;
  ClutterActor *menu_item;

  self = g_object_new (SHELL_TYPE_AUTH_LIST_ITEM_ICON,
                       "style-class", "login-dialog-item-icon",
                       "button-mask", ST_BUTTON_PRIMARY | ST_BUTTON_SECONDARY,
                       "icon-name", icon_name,
                       NULL);

  /* ItemIconPopup */
  self->menu = shell_popup_menu_new (CLUTTER_ACTOR (self), 0.5, ST_SIDE_TOP);
  menu_base = SHELL_POPUP_MENU_BASE (self->menu);

  st_widget_add_style_class_name (ST_WIDGET (shell_popup_menu_base_get_box (menu_base)),
                                  "login-dialog-item-icon-popup-box");
  st_widget_add_style_class_name (ST_WIDGET (shell_popup_menu_base_get_actor (menu_base)),
                                  "login-dialog-item-icon-popup");

  labels = g_object_new (ST_TYPE_BOX_LAYOUT,
                         "orientation", CLUTTER_ORIENTATION_VERTICAL,
                         "style-class", "login-dialog-item-icon-popup-labels",
                         NULL);
  clutter_actor_add_child (labels, g_object_new (ST_TYPE_LABEL, "text", title, NULL));
  clutter_actor_add_child (labels, g_object_new (ST_TYPE_LABEL, "text", subtitle, NULL));

  menu_item = g_object_new (SHELL_TYPE_POPUP_BASE_MENU_ITEM,
                            "reactive", FALSE,
                            "can-focus", FALSE,
                            "activatable", FALSE,
                            "active-on-hover", FALSE,
                            NULL);
  clutter_actor_add_child (menu_item, labels);
  shell_popup_menu_base_add_menu_item (menu_base, G_OBJECT (menu_item));

  g_signal_connect (self, "clicked", G_CALLBACK (item_icon_clicked_cb), self);
  g_signal_connect (self, "destroy", G_CALLBACK (item_icon_destroy_cb), self);

  clutter_actor_hide (CLUTTER_ACTOR (shell_popup_menu_base_get_actor (menu_base)));

  self->manager = shell_popup_menu_manager_new (SHELL_ACTION_MODE_NONE);
  shell_popup_menu_manager_add_menu (self->manager, G_OBJECT (self->menu));

  add_ui_group_child (shell_popup_menu_base_get_actor (menu_base));

  return CLUTTER_ACTOR (self);
}

/* ------------------------------------------------------------------------ */
/* ShellAuthListItem                                                         */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_AUTH_LIST_ITEM (shell_auth_list_item_get_type ())
G_DECLARE_FINAL_TYPE (ShellAuthListItem, shell_auth_list_item,
                      SHELL, AUTH_LIST_ITEM, StButton)

/* `AuthListItem` */
struct _ShellAuthListItem
{
  StButton parent;

  char *key;
  gboolean compact;
};

G_DEFINE_FINAL_TYPE (ShellAuthListItem, shell_auth_list_item, ST_TYPE_BUTTON)

enum
{
  ITEM_ACTIVATE,
  N_ITEM_SIGNALS
};

static guint item_signals[N_ITEM_SIGNALS] = { 0 };

static void
item_set_selected (ShellAuthListItem *self,
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
item_hover_cb (GObject           *object,
               GParamSpec        *pspec,
               ShellAuthListItem *self)
{
  item_set_selected (self, st_widget_get_hover (ST_WIDGET (self)));
}

static void
item_clicked_cb (StButton          *button,
                 int                clicked_button,
                 ShellAuthListItem *self)
{
  g_signal_emit (self, item_signals[ITEM_ACTIVATE], 0);
}

static void
shell_auth_list_item_key_focus_in (ClutterActor *actor)
{
  CLUTTER_ACTOR_CLASS (shell_auth_list_item_parent_class)->key_focus_in (actor);

  item_set_selected (SHELL_AUTH_LIST_ITEM (actor), TRUE);
}

static void
shell_auth_list_item_key_focus_out (ClutterActor *actor)
{
  CLUTTER_ACTOR_CLASS (shell_auth_list_item_parent_class)->key_focus_out (actor);

  item_set_selected (SHELL_AUTH_LIST_ITEM (actor), FALSE);
}

static void
shell_auth_list_item_finalize (GObject *object)
{
  ShellAuthListItem *self = SHELL_AUTH_LIST_ITEM (object);

  g_clear_pointer (&self->key, g_free);

  G_OBJECT_CLASS (shell_auth_list_item_parent_class)->finalize (object);
}

static void
shell_auth_list_item_class_init (ShellAuthListItemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->finalize = shell_auth_list_item_finalize;

  actor_class->key_focus_in = shell_auth_list_item_key_focus_in;
  actor_class->key_focus_out = shell_auth_list_item_key_focus_out;

  /**
   * ShellAuthListItem::activate:
   * @self: the #ShellAuthListItem
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
shell_auth_list_item_init (ShellAuthListItem *self)
{
}

static ClutterActor *
make_item_label (const char *text,
                 const char *style_class)
{
  ClutterActor *label = g_object_new (ST_TYPE_LABEL,
                                      "text", text,
                                      "style-class", style_class,
                                      "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                      "x-expand", TRUE,
                                      NULL);

  clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (label)),
                              PANGO_ELLIPSIZE_END);
  return label;
}

/* `new AuthListItem (key, content)` */
static ClutterActor *
shell_auth_list_item_new (const char *key,
                          GVariant   *content)
{
  ShellAuthListItem *self;
  const char *title = dict_get_string (content, "title");
  const char *subtitle = dict_get_string (content, "subtitle");
  const char *icon_name = dict_get_string (content, "iconName");
  const char *icon_title = dict_get_string (content, "iconTitle");
  const char *icon_subtitle = dict_get_string (content, "iconSubtitle");
  g_autoptr (GPtrArray) name_parts = g_ptr_array_new ();
  g_autofree char *accessible_name = NULL;
  ClutterActor *container;
  ClutterActor *label_box;

  if (title != NULL && title[0] != '\0')
    g_ptr_array_add (name_parts, (gpointer) title);
  if (subtitle != NULL && subtitle[0] != '\0')
    g_ptr_array_add (name_parts, (gpointer) subtitle);
  if (icon_title != NULL && icon_title[0] != '\0')
    g_ptr_array_add (name_parts, (gpointer) icon_title);
  if (icon_subtitle != NULL && icon_subtitle[0] != '\0')
    g_ptr_array_add (name_parts, (gpointer) icon_subtitle);
  g_ptr_array_add (name_parts, NULL);
  accessible_name = g_strjoinv (", ", (char **) name_parts->pdata);

  self = g_object_new (SHELL_TYPE_AUTH_LIST_ITEM,
                       "style-class", "login-dialog-auth-list-item",
                       "button-mask", ST_BUTTON_PRIMARY | ST_BUTTON_SECONDARY,
                       "can-focus", TRUE,
                       "reactive", TRUE,
                       "accessible-name", accessible_name,
                       NULL);
  self->key = g_strdup (key);

  container = g_object_new (ST_TYPE_WIDGET,
                            "layout-manager", clutter_bin_layout_new (),
                            "x-expand", TRUE,
                            NULL);
  label_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "orientation", CLUTTER_ORIENTATION_VERTICAL,
                            "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                            "x-expand", TRUE,
                            NULL);
  clutter_actor_add_child (container, label_box);

  if (title != NULL && title[0] != '\0')
    clutter_actor_add_child (label_box,
                             make_item_label (title, "login-dialog-auth-list-item-title"));

  if (subtitle != NULL && subtitle[0] != '\0')
    clutter_actor_add_child (label_box,
                             make_item_label (subtitle, "login-dialog-auth-list-item-subtitle"));

  if (icon_name != NULL && icon_name[0] != '\0' &&
      icon_title != NULL && icon_title[0] != '\0' &&
      icon_subtitle != NULL && icon_subtitle[0] != '\0')
    {
      ClutterActor *icon = shell_auth_list_item_icon_new (icon_name, icon_title, icon_subtitle);
      ClutterConstraint *constraint;
      graphene_point_t pivot = GRAPHENE_POINT_INIT (0.0f, 0.35f);

      constraint = clutter_align_constraint_new (container, CLUTTER_ALIGN_X_AXIS, 0.5f);
      clutter_actor_add_constraint (icon, constraint);

      constraint = clutter_align_constraint_new (container, CLUTTER_ALIGN_Y_AXIS, 0.0f);
      clutter_align_constraint_set_pivot_point (CLUTTER_ALIGN_CONSTRAINT (constraint), &pivot);
      clutter_actor_add_constraint (icon, constraint);

      clutter_actor_add_child (container, icon);
    }

  self->compact = (subtitle == NULL || subtitle[0] == '\0') &&
                  (icon_name == NULL || icon_name[0] == '\0');

  st_bin_set_child (ST_BIN (self), container);

  g_signal_connect (self, "notify::hover", G_CALLBACK (item_hover_cb), self);
  g_signal_connect (self, "clicked", G_CALLBACK (item_clicked_cb), self);

  return CLUTTER_ACTOR (self);
}

/* ------------------------------------------------------------------------ */
/* ShellAuthList                                                             */
/* ------------------------------------------------------------------------ */

/**
 * ShellAuthList:
 *
 * The list of choices of an authentication service (`AuthList`). Activating
 * an item emits #ShellAuthList::activate with its key.
 */
struct _ShellAuthList
{
  StBoxLayout parent;

  StBoxLayout *box;
  StScrollView *scroll_view;

  GHashTable *items;     /* key (owned) -> ShellAuthListItem (weak) */

  MetaLaters *laters;    /* weak, where move_focus_later was added */
  guint move_focus_later;
};

G_DEFINE_FINAL_TYPE (ShellAuthList, shell_auth_list, ST_TYPE_BOX_LAYOUT)

enum
{
  ACTIVATE,
  ITEM_ADDED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

static void move_focus_to_items (ShellAuthList *self);

static gboolean
move_focus_later_cb (gpointer user_data)
{
  ShellAuthList *self = SHELL_AUTH_LIST (user_data);

  /* The later is being removed by returning G_SOURCE_REMOVE */
  self->move_focus_later = 0;
  move_focus_to_items (self);

  return G_SOURCE_REMOVE;
}

/* `_moveFocusToItems ()` */
static void
move_focus_to_items (ShellAuthList *self)
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
                                            move_focus_later_cb, self, NULL);
}

static void
update_items_layout (ShellAuthList *self)
{
  g_autoptr (GList) children =
    clutter_actor_get_children (CLUTTER_ACTOR (self->box));
  gboolean compact = TRUE;
  GList *l;

  for (l = children; l != NULL; l = l->next)
    {
      if (!SHELL_IS_AUTH_LIST_ITEM (l->data) ||
          !SHELL_AUTH_LIST_ITEM (l->data)->compact)
        compact = FALSE;
    }

  for (l = children; l != NULL; l = l->next)
    {
      if (compact)
        st_widget_add_style_class_name (ST_WIDGET (l->data), "compact");
      else
        st_widget_remove_style_class_name (ST_WIDGET (l->data), "compact");
    }
}

static void
box_children_changed_cb (ClutterActor  *box,
                         ClutterActor  *child,
                         ShellAuthList *self)
{
  update_items_layout (self);
}

static void
item_activate_cb (ShellAuthListItem *item,
                  ShellAuthList     *self)
{
  g_signal_emit (self, signals[ACTIVATE], 0, item->key);
}

static void
item_key_focus_in_cb (ClutterActor  *item,
                      ShellAuthList *self)
{
  /* Try to keep the focused item front-and-center */
  shell_auth_list_scroll_to_item (self, item);
}

static void
item_destroyed_cb (gpointer  data,
                   GObject  *where_the_object_was)
{
  /* shell_auth_list_remove_item () and clear () forget the item first; this
   * is only for an item destroyed behind our back */
  ShellAuthList *self = SHELL_AUTH_LIST (data);
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, self->items);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      if (value == (gpointer) where_the_object_was)
        {
          g_hash_table_iter_remove (&iter);
          break;
        }
    }
}

static void
forget_item (ShellAuthList *self,
             gpointer       item)
{
  g_object_weak_unref (G_OBJECT (item), item_destroyed_cb, self);
}

static void
shell_auth_list_key_focus_in (ClutterActor *actor)
{
  CLUTTER_ACTOR_CLASS (shell_auth_list_parent_class)->key_focus_in (actor);

  move_focus_to_items (SHELL_AUTH_LIST (actor));
}

static void
shell_auth_list_constructed (GObject *object)
{
  ShellAuthList *self = SHELL_AUTH_LIST (object);

  G_OBJECT_CLASS (shell_auth_list_parent_class)->constructed (object);

  self->box = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "orientation", CLUTTER_ORIENTATION_VERTICAL,
                            "style-class", "login-dialog-auth-list",
                            "pseudo-class", "expanded",
                            NULL);

  self->scroll_view = g_object_new (ST_TYPE_SCROLL_VIEW,
                                    "style-class", "login-dialog-auth-list-view",
                                    "y-expand", TRUE,
                                    NULL);
  st_bin_set_child (ST_BIN (self->scroll_view), CLUTTER_ACTOR (self->box));
  clutter_actor_add_child (CLUTTER_ACTOR (self), CLUTTER_ACTOR (self->scroll_view));

  g_signal_connect (self->box, "child-added",
                    G_CALLBACK (box_children_changed_cb), self);
  g_signal_connect (self->box, "child-removed",
                    G_CALLBACK (box_children_changed_cb), self);
}

static void
shell_auth_list_dispose (GObject *object)
{
  ShellAuthList *self = SHELL_AUTH_LIST (object);
  GHashTableIter iter;
  gpointer value;

  if (self->move_focus_later != 0 && self->laters != NULL)
    meta_laters_remove (self->laters, self->move_focus_later);
  self->move_focus_later = 0;
  g_clear_weak_pointer (&self->laters);

  if (self->box != NULL)
    g_signal_handlers_disconnect_by_data (self->box, self);

  if (self->items != NULL)
    {
      g_hash_table_iter_init (&iter, self->items);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        forget_item (self, value);
      g_hash_table_remove_all (self->items);
    }

  self->box = NULL;
  self->scroll_view = NULL;

  G_OBJECT_CLASS (shell_auth_list_parent_class)->dispose (object);
}

static void
shell_auth_list_finalize (GObject *object)
{
  ShellAuthList *self = SHELL_AUTH_LIST (object);

  g_clear_pointer (&self->items, g_hash_table_unref);

  G_OBJECT_CLASS (shell_auth_list_parent_class)->finalize (object);
}

static void
shell_auth_list_class_init (ShellAuthListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_auth_list_constructed;
  object_class->dispose = shell_auth_list_dispose;
  object_class->finalize = shell_auth_list_finalize;

  actor_class->key_focus_in = shell_auth_list_key_focus_in;

  /**
   * ShellAuthList::activate:
   * @self: the #ShellAuthList
   * @key: the key of the item the user activated
   *
   * Emitted when the user activates an item.
   */
  signals[ACTIVATE] =
    g_signal_new ("activate",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);

  /**
   * ShellAuthList::item-added:
   * @self: the #ShellAuthList
   * @item: the new item
   *
   * Emitted when an item was added.
   */
  signals[ITEM_ADDED] =
    g_signal_new ("item-added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  CLUTTER_TYPE_ACTOR);
}

static void
shell_auth_list_init (ShellAuthList *self)
{
  self->items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

/**
 * shell_auth_list_new:
 *
 * Creates an empty list (`new AuthList ()`).
 *
 * Returns: (transfer floating): a new #ShellAuthList
 */
ShellAuthList *
shell_auth_list_new (void)
{
  return g_object_new (SHELL_TYPE_AUTH_LIST,
                       "orientation", CLUTTER_ORIENTATION_VERTICAL,
                       "style-class", "login-dialog-auth-list-layout",
                       "x-expand", TRUE,
                       NULL);
}

/**
 * shell_auth_list_scroll_to_item:
 * @self: a #ShellAuthList
 * @item: an item of the list
 *
 * Scrolls the list so that @item is in the middle (`scrollToItem ()`).
 */
void
shell_auth_list_scroll_to_item (ShellAuthList *self,
                                ClutterActor  *item)
{
  StAdjustment *adjustment;
  ClutterActorBox box;
  double step_increment, page_size, value;
  ShellEaseParams params;
  g_auto (GValue) target = G_VALUE_INIT;
  g_autoptr (GError) error = NULL;

  g_return_if_fail (SHELL_IS_AUTH_LIST (self));
  g_return_if_fail (CLUTTER_IS_ACTOR (item));

  if (self->scroll_view == NULL)
    return;

  clutter_actor_get_allocation_box (item, &box);

  adjustment = st_scroll_view_get_vadjustment (self->scroll_view);
  st_adjustment_get_values (adjustment, NULL, NULL, NULL, &step_increment, NULL, &page_size);

  value = (box.y1 + step_increment / 2.0) - (page_size / 2.0);

  params.duration = SHELL_AUTH_LIST_SCROLL_ANIMATION_TIME;
  params.delay = -1;
  params.mode = CLUTTER_EASE_OUT_QUAD;
  params.repeat_count = 0;
  params.auto_reverse = FALSE;
  params.animation_required = FALSE;

  g_value_init (&target, G_TYPE_DOUBLE);
  g_value_set_double (&target, value);

  if (!shell_animatable_ease_property (CLUTTER_ANIMATABLE (adjustment), "value",
                                       &target, &params, NULL, NULL, NULL, &error))
    g_debug ("%s: %s", G_STRFUNC, error->message);
}

/**
 * shell_auth_list_remove_item:
 * @self: a #ShellAuthList
 * @key: the key of an item
 *
 * Removes the item with the key @key, if any (`removeItem ()`).
 */
void
shell_auth_list_remove_item (ShellAuthList *self,
                             const char    *key)
{
  gpointer item;

  g_return_if_fail (SHELL_IS_AUTH_LIST (self));
  g_return_if_fail (key != NULL);

  item = g_hash_table_lookup (self->items, key);
  if (item == NULL)
    return;

  forget_item (self, item);
  g_hash_table_remove (self->items, key);

  clutter_actor_destroy (CLUTTER_ACTOR (item));
}

/**
 * shell_auth_list_add_item:
 * @self: a #ShellAuthList
 * @key: the key of the item, that #ShellAuthList::activate gives back
 * @content: (nullable): an `a{sv}` with the optional strings `title`,
 *   `subtitle`, `iconName`, `iconTitle` and `iconSubtitle`
 *
 * Adds an item, replacing the one with the same key (`addItem ()`).
 */
void
shell_auth_list_add_item (ShellAuthList *self,
                          const char    *key,
                          GVariant      *content)
{
  ClutterActor *item;

  g_return_if_fail (SHELL_IS_AUTH_LIST (self));
  g_return_if_fail (key != NULL);
  g_return_if_fail (content == NULL || g_variant_is_of_type (content, G_VARIANT_TYPE_VARDICT));

  if (self->box == NULL)
    return;

  shell_auth_list_remove_item (self, key);

  item = shell_auth_list_item_new (key, content);
  clutter_actor_add_child (CLUTTER_ACTOR (self->box), item);

  g_hash_table_insert (self->items, g_strdup (key), item);
  g_object_weak_ref (G_OBJECT (item), item_destroyed_cb, self);

  g_signal_connect (item, "activate", G_CALLBACK (item_activate_cb), self);
  g_signal_connect (item, "key-focus-in", G_CALLBACK (item_key_focus_in_cb), self);

  move_focus_to_items (self);

  g_signal_emit (self, signals[ITEM_ADDED], 0, item);
}

/**
 * shell_auth_list_get_n_items:
 * @self: a #ShellAuthList
 *
 * Returns: the number of items (`numItems`)
 */
guint
shell_auth_list_get_n_items (ShellAuthList *self)
{
  g_return_val_if_fail (SHELL_IS_AUTH_LIST (self), 0);

  return g_hash_table_size (self->items);
}

/**
 * shell_auth_list_clear:
 * @self: a #ShellAuthList
 *
 * Removes all the items (`clear ()`).
 */
void
shell_auth_list_clear (ShellAuthList *self)
{
  GHashTableIter iter;
  gpointer value;

  g_return_if_fail (SHELL_IS_AUTH_LIST (self));

  g_hash_table_iter_init (&iter, self->items);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    forget_item (self, value);

  g_hash_table_remove_all (self->items);

  if (self->box != NULL)
    clutter_actor_destroy_all_children (CLUTTER_ACTOR (self->box));
}
