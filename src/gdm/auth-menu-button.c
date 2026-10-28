/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auth-menu-button.c: A menu button that is used to manage the login options
 * (authentication methods, session). Port of js/gdm/authMenuButton.js.
 *
 * Copyright 2024 Red Hat, Inc
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - The items and the search criteria are `a{sv}` variants with the keys of
 *    the JS objects. Items are identified by their content, with the keys
 *    sorted (the JS used `JSON.stringify ()` of the object).
 *  - The items without a `sectionName` belong to a section of their own that
 *    can be searched, counted and cleared like the others (the JS never
 *    matched them with `{sectionName: undefined}`).
 *  - Adding an item without a name or twice, and finding several active
 *    items, are warnings instead of exceptions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <atk/atk.h>

#include "gdm/auth-menu-button.h"
#include "shell-action-modes.h"
#include "ui/environment.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/popup-menu.h"

#define VISIBILITY_ANIMATION_TIME 200

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static void
set_actor_visible (ClutterActor *actor,
                   gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

static ShellEaseParams
ease_params (int                  duration,
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

/* The same variant with the entries sorted by key, a floating reference */
static GVariant *
normalize_dict (GVariant *dict)
{
  g_autoptr (GPtrArray) keys = g_ptr_array_new ();
  GVariantBuilder builder;
  GVariantIter iter;
  const char *key;
  GVariant *value;
  guint i;

  g_variant_iter_init (&iter, dict);
  while (g_variant_iter_next (&iter, "{&sv}", &key, &value))
    {
      g_variant_unref (value);
      g_ptr_array_add (keys, (gpointer) key);
    }
  g_ptr_array_sort_values (keys, (GCompareFunc) g_strcmp0);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  for (i = 0; i < keys->len; i++)
    {
      g_autoptr (GVariant) entry = g_variant_lookup_value (dict, g_ptr_array_index (keys, i), NULL);

      g_variant_builder_add (&builder, "{sv}", g_ptr_array_index (keys, i), entry);
    }

  return g_variant_builder_end (&builder);
}

/* `item.name` and friends: the string @key of the dictionary, or %NULL */
static char *
dict_dup_string (GVariant   *dict,
                 const char *key)
{
  g_autoptr (GVariant) value = g_variant_lookup_value (dict, key, G_VARIANT_TYPE_STRING);

  if (value == NULL)
    return NULL;

  return g_variant_dup_string (value, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellAuthMenuButton                                                       */
/* ------------------------------------------------------------------------ */

/* An item of the button (a key of `_items`, with the menu item it maps to) */
typedef struct
{
  GVariant *item;                         /* normalized `a{sv}`, owned */
  char *section_name;                     /* nullable */
  ShellPopupBaseMenuItem *menu_item;      /* owned */
  gboolean active;                        /* is in `_activeItems` */
} AuthItem;

/* An entry of `_sections` */
typedef struct
{
  char *name;                             /* nullable */
  ShellPopupMenuSection *section;         /* owned */
} AuthSection;

typedef struct
{
  ShellPopupMenu *menu;
  ShellPopupMenuManager *manager;

  GPtrArray *items;                       /* AuthItem, in the order they were added */
  GPtrArray *sections;                    /* AuthSection */

  char **section_order;
  gboolean read_only;
  gboolean animate_visibility;
  int target_visibility;                  /* -1 until updateVisibility () ran */
} ShellAuthMenuButtonPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (ShellAuthMenuButton, shell_auth_menu_button, ST_TYPE_BUTTON)

enum
{
  PROP_0,
  PROP_READ_ONLY,
  PROP_SECTION_ORDER,
  PROP_ANIMATE_VISIBILITY,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL, };

enum
{
  ACTIVE_ITEM_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

static void
auth_item_free (AuthItem *item)
{
  g_clear_pointer (&item->item, g_variant_unref);
  g_clear_pointer (&item->section_name, g_free);
  g_clear_object (&item->menu_item);
  g_free (item);
}

static void
auth_section_free (AuthSection *section)
{
  g_clear_pointer (&section->name, g_free);
  g_clear_object (&section->section);
  g_free (section);
}

/* `_deepEquals ()` of the value @key of the criteria and of the item */
static gboolean
criteria_matches (GVariant *item,
                  GVariant *criteria)
{
  GVariantIter iter;
  const char *key;
  GVariant *value;

  if (criteria == NULL)
    return TRUE;

  g_variant_iter_init (&iter, criteria);
  while (g_variant_iter_next (&iter, "{&sv}", &key, &value))
    {
      g_autoptr (GVariant) criteria_value = value;
      g_autoptr (GVariant) item_value = g_variant_lookup_value (item, key, NULL);

      if (item_value == NULL || !g_variant_equal (item_value, criteria_value))
        return FALSE;
    }

  return TRUE;
}

/* `_findItems ()`; the entries are owned by the button */
static GPtrArray *
find_items (ShellAuthMenuButton *self,
            GVariant            *criteria)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);
  GPtrArray *found = g_ptr_array_new ();
  guint i;

  for (i = 0; i < priv->items->len; i++)
    {
      AuthItem *item = g_ptr_array_index (priv->items, i);

      if (criteria_matches (item->item, criteria))
        g_ptr_array_add (found, item);
    }

  return found;
}

static guint
section_item_count (ShellAuthMenuButton *self,
                    const char          *section_name)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);
  guint count = 0;
  guint i;

  for (i = 0; i < priv->items->len; i++)
    {
      AuthItem *item = g_ptr_array_index (priv->items, i);

      if (g_strcmp0 (item->section_name, section_name) == 0)
        count++;
    }

  return count;
}

static AuthSection *
lookup_section (ShellAuthMenuButton *self,
                const char          *section_name)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);
  guint i;

  for (i = 0; i < priv->sections->len; i++)
    {
      AuthSection *section = g_ptr_array_index (priv->sections, i);

      if (g_strcmp0 (section->name, section_name) == 0)
        return section;
    }

  return NULL;
}

/* `new AuthMenuItemSection (sectionName)` */
static ShellPopupMenuSection *
auth_menu_item_section_new (const char *section_name)
{
  ShellPopupMenuSection *section = shell_popup_menu_section_new ();

  st_widget_add_style_class_name (ST_WIDGET (shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (section))),
                                  "login-dialog-auth-menu-item-section");

  if (section_name != NULL)
    {
      ClutterActor *item_section = g_object_new (SHELL_TYPE_POPUP_MENU_ITEM,
                                                 "text", section_name,
                                                 "reactive", FALSE,
                                                 "can-focus", FALSE,
                                                 "activatable", FALSE,
                                                 "active-on-hover", FALSE,
                                                 NULL);
      StLabel *label = shell_popup_menu_item_get_label (SHELL_POPUP_MENU_ITEM (item_section));

      st_widget_set_style_class_name (ST_WIDGET (label),
                                      "login-dialog-auth-menu-item-section-label");
      clutter_actor_set_x_expand (CLUTTER_ACTOR (label), TRUE);
      clutter_actor_set_x_align (CLUTTER_ACTOR (label), CLUTTER_ACTOR_ALIGN_CENTER);
      shell_popup_menu_base_add_menu_item (SHELL_POPUP_MENU_BASE (section), G_OBJECT (item_section));
    }

  return section;
}

/* `_getSection ()` */
static AuthSection *
get_section (ShellAuthMenuButton *self,
             const char          *section_name)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);
  ShellPopupMenuBase *menu = SHELL_POPUP_MENU_BASE (priv->menu);
  AuthSection *section = lookup_section (self, section_name);
  guint n_menu_items;

  if (section != NULL)
    return section;

  section = g_new0 (AuthSection, 1);
  section->name = g_strdup (section_name);
  section->section = auth_menu_item_section_new (section_name);

  n_menu_items = shell_popup_menu_base_get_n_menu_items (menu);

  if (section_name == NULL)
    {
      /* Empty section is always last */
      shell_popup_menu_base_add_menu_item (menu, G_OBJECT (section->section));
    }
  else
    {
      int order_index = -1;
      guint i;

      for (i = 0; priv->section_order != NULL && priv->section_order[i] != NULL; i++)
        {
          if (g_strcmp0 (priv->section_order[i], section_name) == 0)
            {
              order_index = i;
              break;
            }
        }

      if (order_index != -1)
        {
          /* Section is in sectionOrder, add at its position */
          shell_popup_menu_base_insert_menu_item (menu, G_OBJECT (section->section),
                                                  MIN ((guint) order_index, n_menu_items));
        }
      else
        {
          /* Section not in sectionOrder, add before empty section if it exists */
          AuthSection *empty_section = lookup_section (self, NULL);

          if (empty_section != NULL)
            {
              g_autoptr (GList) menu_items = shell_popup_menu_base_get_menu_items (menu);
              int empty_index = g_list_index (menu_items, empty_section->section);

              if (empty_index < 0)
                shell_popup_menu_base_add_menu_item (menu, G_OBJECT (section->section));
              else
                shell_popup_menu_base_insert_menu_item (menu, G_OBJECT (section->section),
                                                        empty_index);
            }
          else
            {
              shell_popup_menu_base_add_menu_item (menu, G_OBJECT (section->section));
            }
        }
    }

  g_ptr_array_add (priv->sections, section);

  return section;
}

/* `_updateOrnament ()` */
static void
update_ornament (ShellAuthMenuButton *self)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);
  guint i;

  for (i = 0; i < priv->items->len; i++)
    {
      AuthItem *item = g_ptr_array_index (priv->items, i);

      shell_popup_base_menu_item_set_ornament (item->menu_item,
                                               item->active ? SHELL_ORNAMENT_DOT
                                                            : SHELL_ORNAMENT_NO_DOT);
    }
}

/* `_canBeVisible ()` */
static gboolean
shell_auth_menu_button_real_can_be_visible (ShellAuthMenuButton *self)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);
  gboolean any_visible = FALSE;
  guint i;

  for (i = 0; i < priv->sections->len; i++)
    {
      AuthSection *section = g_ptr_array_index (priv->sections, i);
      gboolean visible = section_item_count (self, section->name) > 1;

      set_actor_visible (shell_popup_menu_base_get_actor (SHELL_POPUP_MENU_BASE (section->section)),
                         visible);
      any_visible |= visible;
    }

  return any_visible;
}

static void
fade_out_done_cb (gboolean finished,
                  gpointer user_data)
{
  if (finished)
    set_actor_visible (CLUTTER_ACTOR (user_data), FALSE);
}

/**
 * shell_auth_menu_button_update_visibility:
 * @self: a #ShellAuthMenuButton
 * @visible: whether the button should be shown, if it has something to show
 *
 * Shows or hides the button (`updateVisibility ()`), with a fade if
 * #ShellAuthMenuButton:animate-visibility is set.
 */
void
shell_auth_menu_button_update_visibility (ShellAuthMenuButton *self,
                                          gboolean             visible)
{
  ShellAuthMenuButtonPrivate *priv;
  ShellAuthMenuButtonClass *klass;
  ClutterActor *actor;

  g_return_if_fail (SHELL_IS_AUTH_MENU_BUTTON (self));

  priv = shell_auth_menu_button_get_instance_private (self);
  klass = SHELL_AUTH_MENU_BUTTON_GET_CLASS (self);
  actor = CLUTTER_ACTOR (self);

  if (visible)
    visible = klass->can_be_visible (self);

  if (priv->target_visibility == visible)
    return;

  priv->target_visibility = visible;

  if (priv->animate_visibility)
    {
      ShellEaseParams params = ease_params (VISIBILITY_ANIMATION_TIME, CLUTTER_EASE_OUT_QUAD);

      if (visible)
        {
          clutter_actor_set_opacity (actor, 0);
          clutter_actor_show (actor);
          shell_actor_ease (actor, &params, NULL, NULL, NULL,
                            "opacity", 255u,
                            NULL);
        }
      else
        {
          shell_actor_ease (actor, &params,
                            fade_out_done_cb, g_object_ref (self), g_object_unref,
                            "opacity", 0u,
                            NULL);
        }
    }
  else
    {
      clutter_actor_set_opacity (actor, visible ? 255 : 0);
      set_actor_visible (actor, visible);
    }
}

static void
shell_auth_menu_button_real_activated (ShellAuthMenuButton *self)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);

  shell_popup_menu_base_toggle (SHELL_POPUP_MENU_BASE (priv->menu));
}

static void
button_clicked_cb (StButton            *button,
                   int                  clicked_button,
                   ShellAuthMenuButton *self)
{
  SHELL_AUTH_MENU_BUTTON_GET_CLASS (self)->activated (self);
}

static void
button_destroy_cb (ClutterActor        *actor,
                   ShellAuthMenuButton *self)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);

  if (priv->menu != NULL)
    shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (priv->menu));
}

static void
menu_item_activate_cb (ShellPopupBaseMenuItem *menu_item,
                       ClutterEvent           *event,
                       ShellAuthMenuButton    *self)
{
  GVariant *item = g_object_get_data (G_OBJECT (menu_item), "shell-auth-item");

  if (item != NULL)
    shell_auth_menu_button_set_active_item (self, item);
}

static void
shell_auth_menu_button_real_add_item (ShellAuthMenuButton *self,
                                      GVariant            *item)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);
  g_autoptr (GVariant) key = g_variant_ref_sink (normalize_dict (item));
  g_autofree char *key_string = NULL;
  g_autofree char *name = NULL;
  g_autofree char *description = NULL;
  g_autofree char *icon_name = NULL;
  g_autofree char *section_name = NULL;
  ShellPopupBaseMenuItem *menu_item;
  AuthItem *auth_item;
  AuthSection *section;
  guint i;

  key_string = g_variant_print (key, FALSE);

  for (i = 0; i < priv->items->len; i++)
    {
      AuthItem *existing = g_ptr_array_index (priv->items, i);

      if (g_variant_equal (existing->item, key))
        {
          g_warning ("%s: Duplicate item %s", G_STRFUNC, key_string);
          return;
        }
    }

  name = dict_dup_string (key, "name");
  if (name == NULL || name[0] == '\0')
    {
      g_warning ("%s: item %s lacks name", G_STRFUNC, key_string);
      return;
    }

  description = dict_dup_string (key, "description");
  icon_name = dict_dup_string (key, "iconName");
  section_name = dict_dup_string (key, "sectionName");

  /* `new AuthMenuItem (item, {reactive: !this.readOnly})` */
  menu_item = g_object_new (SHELL_TYPE_POPUP_BASE_MENU_ITEM,
                            "reactive", !priv->read_only,
                            "activatable", !priv->read_only,
                            "active-on-hover", !priv->read_only,
                            NULL);

  if (description != NULL && description[0] != '\0' &&
      icon_name != NULL && icon_name[0] != '\0')
    {
      ClutterActor *icon = g_object_new (ST_TYPE_ICON,
                                         "style-class", "login-dialog-auth-menu-item-icon",
                                         "icon-name", icon_name,
                                         "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                         NULL);
      ClutterActor *box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                        "orientation", CLUTTER_ORIENTATION_VERTICAL,
                                        "style-class", "login-dialog-auth-menu-item-box",
                                        NULL);
      ClutterActor *name_label = g_object_new (ST_TYPE_LABEL,
                                               "text", name,
                                               "style-class", "login-dialog-auth-menu-item-box-name",
                                               NULL);
      ClutterActor *description_label = g_object_new (ST_TYPE_LABEL,
                                                      "text", description,
                                                      "style-class", "login-dialog-auth-menu-item-box-description",
                                                      NULL);

      clutter_actor_add_child (CLUTTER_ACTOR (menu_item), icon);
      clutter_actor_add_child (box, name_label);
      clutter_actor_add_child (box, description_label);
      clutter_actor_add_child (CLUTTER_ACTOR (menu_item), box);

      st_widget_set_label_actor (ST_WIDGET (menu_item), name_label);
    }
  else
    {
      /* When no extra info is provided, use the default style */
      ClutterActor *name_label = g_object_new (ST_TYPE_LABEL,
                                               "text", name,
                                               "y-expand", TRUE,
                                               "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                               NULL);

      clutter_actor_add_child (CLUTTER_ACTOR (menu_item), name_label);
      st_widget_set_label_actor (ST_WIDGET (menu_item), name_label);
    }

  g_object_set_data_full (G_OBJECT (menu_item), "shell-auth-item",
                          g_variant_ref (key), (GDestroyNotify) g_variant_unref);
  g_signal_connect (menu_item, "activate", G_CALLBACK (menu_item_activate_cb), self);

  section = get_section (self, section_name);
  shell_popup_menu_base_add_menu_item (SHELL_POPUP_MENU_BASE (section->section),
                                       G_OBJECT (menu_item));

  auth_item = g_new0 (AuthItem, 1);
  auth_item->item = g_steal_pointer (&key);
  auth_item->section_name = g_steal_pointer (&section_name);
  auth_item->menu_item = g_object_ref (menu_item);
  g_ptr_array_add (priv->items, auth_item);

  shell_auth_menu_button_update_visibility (self, TRUE);
}

/**
 * shell_auth_menu_button_add_item:
 * @self: a #ShellAuthMenuButton
 * @item: an `a{sv}` with at least the string `name`
 *
 * Adds an item to the menu (`addItem ()`). Warns if @item has no name or was
 * added already.
 */
void
shell_auth_menu_button_add_item (ShellAuthMenuButton *self,
                                 GVariant            *item)
{
  g_autoptr (GVariant) sunk_item = NULL;

  g_return_if_fail (SHELL_IS_AUTH_MENU_BUTTON (self));
  g_return_if_fail (item != NULL && g_variant_is_of_type (item, G_VARIANT_TYPE_VARDICT));

  sunk_item = g_variant_ref_sink (item);
  SHELL_AUTH_MENU_BUTTON_GET_CLASS (self)->add_item (self, sunk_item);
}

static void
shell_auth_menu_button_real_clear_items (ShellAuthMenuButton *self,
                                         GVariant            *criteria)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);
  g_autoptr (GPtrArray) found = find_items (self, criteria);
  guint i;

  for (i = 0; i < found->len; i++)
    {
      AuthItem *item = g_ptr_array_index (found, i);

      clutter_actor_destroy (CLUTTER_ACTOR (item->menu_item));
      g_ptr_array_remove (priv->items, item);
    }

  for (i = 0; i < priv->sections->len;)
    {
      AuthSection *section = g_ptr_array_index (priv->sections, i);

      if (section_item_count (self, section->name) == 0)
        {
          shell_popup_menu_base_destroy (SHELL_POPUP_MENU_BASE (section->section));
          g_ptr_array_remove_index (priv->sections, i);
        }
      else
        {
          i++;
        }
    }

  update_ornament (self);
  shell_auth_menu_button_update_visibility (self, TRUE);
}

/**
 * shell_auth_menu_button_clear_items:
 * @self: a #ShellAuthMenuButton
 * @criteria: (nullable): an `a{sv}` the items must match, all the items if
 *   %NULL
 *
 * Removes the items which match @criteria, and the sections which end up
 * empty (`clearItems ()`).
 */
void
shell_auth_menu_button_clear_items (ShellAuthMenuButton *self,
                                    GVariant            *criteria)
{
  g_autoptr (GVariant) sunk_criteria = NULL;

  g_return_if_fail (SHELL_IS_AUTH_MENU_BUTTON (self));
  g_return_if_fail (criteria == NULL || g_variant_is_of_type (criteria, G_VARIANT_TYPE_VARDICT));

  if (criteria != NULL)
    sunk_criteria = g_variant_ref_sink (criteria);

  SHELL_AUTH_MENU_BUTTON_GET_CLASS (self)->clear_items (self, sunk_criteria);
}

/**
 * shell_auth_menu_button_get_items:
 * @self: a #ShellAuthMenuButton
 * @criteria: (nullable): an `a{sv}` the items must match, all the items if
 *   %NULL
 *
 * Returns: (transfer full): an `aa{sv}` with the items which match @criteria
 *   (`getItems ()`)
 */
GVariant *
shell_auth_menu_button_get_items (ShellAuthMenuButton *self,
                                  GVariant            *criteria)
{
  g_autoptr (GVariant) sunk_criteria = NULL;
  g_autoptr (GPtrArray) found = NULL;
  GVariantBuilder builder;
  guint i;

  g_return_val_if_fail (SHELL_IS_AUTH_MENU_BUTTON (self), NULL);
  g_return_val_if_fail (criteria == NULL || g_variant_is_of_type (criteria, G_VARIANT_TYPE_VARDICT), NULL);

  if (criteria != NULL)
    sunk_criteria = g_variant_ref_sink (criteria);

  found = find_items (self, sunk_criteria);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));
  for (i = 0; i < found->len; i++)
    g_variant_builder_add_value (&builder, ((AuthItem *) g_ptr_array_index (found, i))->item);

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

/* `_resolveItem ()` */
static AuthItem *
resolve_item (ShellAuthMenuButton *self,
              GVariant            *criteria)
{
  g_autoptr (GPtrArray) found = find_items (self, criteria);

  if (found->len != 1)
    {
      if (found->len > 1)
        {
          g_autofree char *printed = criteria != NULL ? g_variant_print (criteria, FALSE) : g_strdup ("{}");

          g_warning ("%s: Matched multiple items with criteria %s", G_STRFUNC, printed);
        }
      return NULL;
    }

  return g_ptr_array_index (found, 0);
}

/**
 * shell_auth_menu_button_set_active_item:
 * @self: a #ShellAuthMenuButton
 * @criteria: (nullable): an `a{sv}` which matches exactly one item
 *
 * Makes the item which matches @criteria the active one of its section and
 * emits #ShellAuthMenuButton::active-item-changed, unless it is active
 * already (`setActiveItem ()`). Does nothing if no item, or several, match.
 */
void
shell_auth_menu_button_set_active_item (ShellAuthMenuButton *self,
                                        GVariant            *criteria)
{
  ShellAuthMenuButtonPrivate *priv;
  g_autoptr (GVariant) sunk_criteria = NULL;
  g_autofree char *section_name = NULL;
  AuthItem *item;
  guint i;

  g_return_if_fail (SHELL_IS_AUTH_MENU_BUTTON (self));
  g_return_if_fail (criteria == NULL || g_variant_is_of_type (criteria, G_VARIANT_TYPE_VARDICT));

  priv = shell_auth_menu_button_get_instance_private (self);

  if (criteria != NULL)
    sunk_criteria = g_variant_ref_sink (criteria);

  item = resolve_item (self, sunk_criteria);
  if (item == NULL || item->active)
    return;

  section_name = g_strdup (item->section_name);

  for (i = 0; i < priv->items->len; i++)
    {
      AuthItem *other = g_ptr_array_index (priv->items, i);

      if (g_strcmp0 (other->section_name, section_name) == 0)
        other->active = FALSE;
    }

  item->active = TRUE;
  update_ornament (self);
  g_signal_emit (self, signals[ACTIVE_ITEM_CHANGED], 0, section_name);
}

/**
 * shell_auth_menu_button_get_active_item:
 * @self: a #ShellAuthMenuButton
 * @criteria: (nullable): an `a{sv}` the items must match, all if %NULL
 *
 * Returns: (transfer full) (nullable): the `a{sv}` of the active item which
 *   matches @criteria, or %NULL if none, or several, do (`getActiveItem ()`)
 */
GVariant *
shell_auth_menu_button_get_active_item (ShellAuthMenuButton *self,
                                        GVariant            *criteria)
{
  g_autoptr (GVariant) sunk_criteria = NULL;
  g_autoptr (GPtrArray) found = NULL;
  AuthItem *active = NULL;
  guint n_active = 0;
  guint i;

  g_return_val_if_fail (SHELL_IS_AUTH_MENU_BUTTON (self), NULL);
  g_return_val_if_fail (criteria == NULL || g_variant_is_of_type (criteria, G_VARIANT_TYPE_VARDICT), NULL);

  if (criteria != NULL)
    sunk_criteria = g_variant_ref_sink (criteria);

  found = find_items (self, sunk_criteria);
  for (i = 0; i < found->len; i++)
    {
      AuthItem *item = g_ptr_array_index (found, i);

      if (item->active)
        {
          active = item;
          n_active++;
        }
    }

  if (n_active == 0)
    return NULL;

  if (n_active > 1)
    {
      g_autofree char *printed = sunk_criteria != NULL ? g_variant_print (sunk_criteria, FALSE) : g_strdup ("{}");

      g_warning ("%s: Multiple active items found with criteria %s", G_STRFUNC, printed);
      return NULL;
    }

  return g_variant_ref (active->item);
}

/**
 * shell_auth_menu_button_close_menu:
 * @self: a #ShellAuthMenuButton
 *
 * Closes the menu (`closeMenu ()`).
 */
void
shell_auth_menu_button_close_menu (ShellAuthMenuButton *self)
{
  ShellAuthMenuButtonPrivate *priv;

  g_return_if_fail (SHELL_IS_AUTH_MENU_BUTTON (self));

  priv = shell_auth_menu_button_get_instance_private (self);
  shell_popup_menu_base_close (SHELL_POPUP_MENU_BASE (priv->menu), SHELL_POPUP_ANIMATION_FULL);
}

static void
shell_auth_menu_button_constructed (GObject *object)
{
  ShellAuthMenuButton *self = SHELL_AUTH_MENU_BUTTON (object);
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  ShellPopupMenuBase *menu;
  ClutterActor *menu_actor;

  G_OBJECT_CLASS (shell_auth_menu_button_parent_class)->constructed (object);

  g_object_bind_property (object, "reactive", object, "can-focus", G_BINDING_SYNC_CREATE);

  priv->menu = shell_popup_menu_new (CLUTTER_ACTOR (self), 0.0, ST_SIDE_BOTTOM);
  menu = SHELL_POPUP_MENU_BASE (priv->menu);
  menu_actor = shell_popup_menu_base_get_actor (menu);

  st_widget_add_style_class_name (ST_WIDGET (shell_popup_menu_base_get_box (menu)),
                                  "login-dialog-auth-menu-button-popup");
  if (layout_manager != NULL)
    clutter_actor_add_child (CLUTTER_ACTOR (shell_layout_manager_get_ui_group (layout_manager)),
                             menu_actor);
  clutter_actor_hide (menu_actor);

  priv->manager = shell_popup_menu_manager_new (SHELL_ACTION_MODE_NONE);
  shell_popup_menu_manager_add_menu (priv->manager, G_OBJECT (priv->menu));

  g_signal_connect (self, "clicked", G_CALLBACK (button_clicked_cb), self);
  g_signal_connect (self, "destroy", G_CALLBACK (button_destroy_cb), self);
}

static void
shell_auth_menu_button_set_property (GObject      *object,
                                     guint         prop_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  ShellAuthMenuButton *self = SHELL_AUTH_MENU_BUTTON (object);
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_READ_ONLY:
      priv->read_only = g_value_get_boolean (value);
      break;

    case PROP_SECTION_ORDER:
      g_strfreev (priv->section_order);
      priv->section_order = g_value_dup_boxed (value);
      break;

    case PROP_ANIMATE_VISIBILITY:
      if (priv->animate_visibility != g_value_get_boolean (value))
        {
          priv->animate_visibility = g_value_get_boolean (value);
          g_object_notify_by_pspec (object, props[PROP_ANIMATE_VISIBILITY]);
        }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_auth_menu_button_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  ShellAuthMenuButton *self = SHELL_AUTH_MENU_BUTTON (object);
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_READ_ONLY:
      g_value_set_boolean (value, priv->read_only);
      break;

    case PROP_SECTION_ORDER:
      g_value_set_boxed (value, priv->section_order);
      break;

    case PROP_ANIMATE_VISIBILITY:
      g_value_set_boolean (value, priv->animate_visibility);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
shell_auth_menu_button_dispose (GObject *object)
{
  ShellAuthMenuButton *self = SHELL_AUTH_MENU_BUTTON (object);
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);

  if (priv->items != NULL)
    g_ptr_array_set_size (priv->items, 0);
  if (priv->sections != NULL)
    g_ptr_array_set_size (priv->sections, 0);

  g_clear_object (&priv->manager);
  g_clear_object (&priv->menu);

  G_OBJECT_CLASS (shell_auth_menu_button_parent_class)->dispose (object);
}

static void
shell_auth_menu_button_finalize (GObject *object)
{
  ShellAuthMenuButton *self = SHELL_AUTH_MENU_BUTTON (object);
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);

  g_clear_pointer (&priv->items, g_ptr_array_unref);
  g_clear_pointer (&priv->sections, g_ptr_array_unref);
  g_clear_pointer (&priv->section_order, g_strfreev);

  G_OBJECT_CLASS (shell_auth_menu_button_parent_class)->finalize (object);
}

static void
shell_auth_menu_button_class_init (ShellAuthMenuButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_auth_menu_button_constructed;
  object_class->set_property = shell_auth_menu_button_set_property;
  object_class->get_property = shell_auth_menu_button_get_property;
  object_class->dispose = shell_auth_menu_button_dispose;
  object_class->finalize = shell_auth_menu_button_finalize;

  klass->activated = shell_auth_menu_button_real_activated;
  klass->can_be_visible = shell_auth_menu_button_real_can_be_visible;
  klass->add_item = shell_auth_menu_button_real_add_item;
  klass->clear_items = shell_auth_menu_button_real_clear_items;

  /**
   * ShellAuthMenuButton:read-only:
   *
   * Whether the items of the menu only show information: they cannot be
   * activated.
   */
  props[PROP_READ_ONLY] =
    g_param_spec_boolean ("read-only", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS);

  /**
   * ShellAuthMenuButton:section-order:
   *
   * The names of the sections, in the order they are shown in the menu.
   */
  props[PROP_SECTION_ORDER] =
    g_param_spec_boxed ("section-order", NULL, NULL,
                        G_TYPE_STRV,
                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                        G_PARAM_STATIC_STRINGS);

  /**
   * ShellAuthMenuButton:animate-visibility:
   *
   * Whether the button fades in and out when it is shown and hidden.
   */
  props[PROP_ANIMATE_VISIBILITY] =
    g_param_spec_boolean ("animate-visibility", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, props);

  /**
   * ShellAuthMenuButton::active-item-changed:
   * @self: the #ShellAuthMenuButton
   * @section_name: (nullable): the name of the section of the item
   *
   * Emitted when the active item of a section changed. Get it with
   * shell_auth_menu_button_get_active_item ().
   */
  signals[ACTIVE_ITEM_CHANGED] =
    g_signal_new ("active-item-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);
}

static void
shell_auth_menu_button_init (ShellAuthMenuButton *self)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);

  priv->items = g_ptr_array_new_with_free_func ((GDestroyNotify) auth_item_free);
  priv->sections = g_ptr_array_new_with_free_func ((GDestroyNotify) auth_section_free);
  priv->target_visibility = -1;
}

/**
 * shell_auth_menu_button_new:
 * @section_order: (nullable) (array zero-terminated=1): the names of the
 *   sections, in the order they are shown in the menu
 * @animate_visibility: whether the button fades in and out
 *
 * Creates a button with an empty menu (`new AuthMenuButton ()`). Callers set
 * the other properties of the actor (`accessible-name`, `visible`, ...).
 *
 * Returns: (transfer floating): a new #ShellAuthMenuButton
 */
ShellAuthMenuButton *
shell_auth_menu_button_new (const char * const *section_order,
                            gboolean            animate_visibility)
{
  static const char * const no_sections[] = { NULL };

  return g_object_new (SHELL_TYPE_AUTH_MENU_BUTTON,
                       "icon-name", "cog-wheel-symbolic",
                       "section-order", section_order != NULL ? section_order : no_sections,
                       "animate-visibility", animate_visibility,
                       "style-class", "login-dialog-button login-dialog-auth-menu-button",
                       "can-focus", TRUE,
                       "accessible-role", ATK_ROLE_MENU,
                       "button-mask", ST_BUTTON_PRIMARY | ST_BUTTON_SECONDARY,
                       NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellAuthMenuButtonIndicator                                              */
/* ------------------------------------------------------------------------ */

/**
 * ShellAuthMenuButtonIndicator:
 *
 * A read-only #ShellAuthMenuButton which shows the icons of its items, and
 * the description of its only item, in the button (`AuthMenuButtonIndicator`).
 */
struct _ShellAuthMenuButtonIndicator
{
  ShellAuthMenuButton parent;

  ClutterActor *icons_box;
  StLabel *description_label;
};

G_DEFINE_FINAL_TYPE (ShellAuthMenuButtonIndicator, shell_auth_menu_button_indicator,
                     SHELL_TYPE_AUTH_MENU_BUTTON)

static gboolean
text_to_visible_cb (GBinding     *binding,
                    const GValue *from_value,
                    GValue       *to_value,
                    gpointer      user_data)
{
  const char *text = g_value_get_string (from_value);

  g_value_set_boolean (to_value, text != NULL && text[0] != '\0');
  return TRUE;
}

static void
shell_auth_menu_button_indicator_constructed (GObject *object)
{
  ShellAuthMenuButtonIndicator *self = SHELL_AUTH_MENU_BUTTON_INDICATOR (object);
  ClutterActor *container;

  G_OBJECT_CLASS (shell_auth_menu_button_indicator_parent_class)->constructed (object);

  /* _createChild () */
  container = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "x-align", CLUTTER_ACTOR_ALIGN_START,
                            NULL);

  self->icons_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                  "style-class", "login-dialog-auth-menu-button-indicator-icons",
                                  "x-expand", TRUE,
                                  "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                  NULL);
  clutter_actor_add_child (container, self->icons_box);

  self->description_label = g_object_new (ST_TYPE_LABEL,
                                          "style-class", "login-dialog-auth-menu-button-indicator-description",
                                          "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                          NULL);
  g_object_bind_property_full (self->description_label, "text",
                               self->description_label, "visible",
                               G_BINDING_SYNC_CREATE,
                               text_to_visible_cb, NULL, NULL, NULL);
  clutter_actor_add_child (container, CLUTTER_ACTOR (self->description_label));

  st_bin_set_child (ST_BIN (self), container);

  st_widget_add_style_class_name (ST_WIDGET (self), "login-dialog-auth-menu-button-indicator");
}

static void
shell_auth_menu_button_indicator_dispose (GObject *object)
{
  ShellAuthMenuButtonIndicator *self = SHELL_AUTH_MENU_BUTTON_INDICATOR (object);

  self->icons_box = NULL;
  self->description_label = NULL;

  G_OBJECT_CLASS (shell_auth_menu_button_indicator_parent_class)->dispose (object);
}

/* `_getVisibleItemsCount ()` */
static guint
get_visible_items_count (ShellAuthMenuButton *self)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (self);
  guint count = 0;
  guint i;

  for (i = 0; i < priv->items->len; i++)
    {
      AuthItem *item = g_ptr_array_index (priv->items, i);

      if (clutter_actor_is_visible (CLUTTER_ACTOR (item->menu_item)))
        count++;
    }

  return count;
}

static void
shell_auth_menu_button_indicator_activated (ShellAuthMenuButton *button)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (button);

  /* When there's only one item, all the info (icon, description) is
   * already visible in the button, so no need to show the menu */
  if (get_visible_items_count (button) > 1)
    shell_popup_menu_base_toggle (SHELL_POPUP_MENU_BASE (priv->menu));
}

/* Override to force visibility even when there's only one item */
static gboolean
shell_auth_menu_button_indicator_can_be_visible (ShellAuthMenuButton *button)
{
  ShellAuthMenuButtonPrivate *priv = shell_auth_menu_button_get_instance_private (button);

  return priv->items->len > 0;
}

static void
shell_auth_menu_button_indicator_add_item (ShellAuthMenuButton *button,
                                           GVariant            *item)
{
  ShellAuthMenuButtonIndicator *self = SHELL_AUTH_MENU_BUTTON_INDICATOR (button);
  g_autofree char *icon_name = dict_dup_string (item, "iconName");

  if (icon_name != NULL && icon_name[0] != '\0' && self->icons_box != NULL)
    {
      ClutterActor *icon = g_object_new (ST_TYPE_ICON,
                                         "icon-name", icon_name,
                                         "style-class", "login-dialog-auth-menu-button-indicator-icon",
                                         NULL);

      clutter_actor_add_child (self->icons_box, icon);
    }

  SHELL_AUTH_MENU_BUTTON_CLASS (shell_auth_menu_button_indicator_parent_class)->add_item (button, item);
}

static void
shell_auth_menu_button_indicator_clear_items (ShellAuthMenuButton *button,
                                              GVariant            *criteria)
{
  ShellAuthMenuButtonIndicator *self = SHELL_AUTH_MENU_BUTTON_INDICATOR (button);
  g_autoptr (GPtrArray) found = find_items (button, criteria);
  guint i;

  for (i = 0; i < found->len; i++)
    {
      AuthItem *auth_item = g_ptr_array_index (found, i);
      g_autofree char *icon_name = dict_dup_string (auth_item->item, "iconName");
      g_autoptr (GList) icons = NULL;
      GList *l;

      if (icon_name == NULL || icon_name[0] == '\0' || self->icons_box == NULL)
        continue;

      icons = clutter_actor_get_children (self->icons_box);
      for (l = icons; l != NULL; l = l->next)
        {
          if (ST_IS_ICON (l->data) &&
              g_strcmp0 (st_icon_get_icon_name (ST_ICON (l->data)), icon_name) == 0)
            {
              clutter_actor_destroy (CLUTTER_ACTOR (l->data));
              break;
            }
        }
    }

  SHELL_AUTH_MENU_BUTTON_CLASS (shell_auth_menu_button_indicator_parent_class)->clear_items (button, criteria);
  shell_auth_menu_button_indicator_update_description_label (self);
}

static void
shell_auth_menu_button_indicator_class_init (ShellAuthMenuButtonIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellAuthMenuButtonClass *button_class = SHELL_AUTH_MENU_BUTTON_CLASS (klass);

  object_class->constructed = shell_auth_menu_button_indicator_constructed;
  object_class->dispose = shell_auth_menu_button_indicator_dispose;

  button_class->activated = shell_auth_menu_button_indicator_activated;
  button_class->can_be_visible = shell_auth_menu_button_indicator_can_be_visible;
  button_class->add_item = shell_auth_menu_button_indicator_add_item;
  button_class->clear_items = shell_auth_menu_button_indicator_clear_items;
}

static void
shell_auth_menu_button_indicator_init (ShellAuthMenuButtonIndicator *self)
{
}

/**
 * shell_auth_menu_button_indicator_new:
 * @animate_visibility: whether the button fades in and out
 *
 * Creates an indicator (`new AuthMenuButtonIndicator ()`). Callers set the
 * other properties of the actor (`accessible-name`, `visible`, ...).
 *
 * Returns: (transfer floating): a new #ShellAuthMenuButtonIndicator
 */
ShellAuthMenuButtonIndicator *
shell_auth_menu_button_indicator_new (gboolean animate_visibility)
{
  static const char * const no_sections[] = { NULL };

  return g_object_new (SHELL_TYPE_AUTH_MENU_BUTTON_INDICATOR,
                       "read-only", TRUE,
                       "icon-name", "cog-wheel-symbolic",
                       "section-order", no_sections,
                       "animate-visibility", animate_visibility,
                       "style-class", "login-dialog-button login-dialog-auth-menu-button",
                       "can-focus", TRUE,
                       "accessible-role", ATK_ROLE_MENU,
                       "button-mask", ST_BUTTON_PRIMARY | ST_BUTTON_SECONDARY,
                       NULL);
}

/**
 * shell_auth_menu_button_indicator_update_description_label:
 * @self: a #ShellAuthMenuButtonIndicator
 *
 * Shows the description of the only item of the button, if there is only
 * one, next to the icons (`updateDescriptionLabel ()`).
 */
void
shell_auth_menu_button_indicator_update_description_label (ShellAuthMenuButtonIndicator *self)
{
  ShellAuthMenuButtonPrivate *priv;
  g_autofree char *description = NULL;

  g_return_if_fail (SHELL_IS_AUTH_MENU_BUTTON_INDICATOR (self));

  if (self->description_label == NULL)
    return;

  priv = shell_auth_menu_button_get_instance_private (SHELL_AUTH_MENU_BUTTON (self));

  if (priv->items->len == 1)
    description = dict_dup_string (((AuthItem *) g_ptr_array_index (priv->items, 0))->item,
                                   "description");

  st_label_set_text (self->description_label, description != NULL ? description : "");
}
