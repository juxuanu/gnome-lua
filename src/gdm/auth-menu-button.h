/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auth-menu-button.h: A menu button that is used to manage the login options
 * (authentication methods, session). Port of js/gdm/authMenuButton.js.
 *
 * Copyright 2024 Red Hat, Inc
 *
 * The items of the button are `a{sv}` variants instead of JS objects, with
 * the same keys. Certain keys have a special meaning:
 *
 *   - name: (required) the text of the item
 *   - iconName: the icon of the item, and optionally of the button
 *   - description: additional text shown below the name
 *   - sectionName: groups the items into labeled sections
 *
 * The button supports searching for items with partial criteria (an `a{sv}`
 * too): an item matches when it has every key of the criteria with the same
 * value. For example `{sectionName: 'foo'}` matches all the items of the
 * section 'foo', and `{name: 'bar', sectionName: 'foo'}` the item named 'bar'
 * of the section 'foo'.
 *
 * The items of the sections can be ordered with the :section-order property.
 * The sections which are not in it come at the end. Only one item per section
 * can be active at a time.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

#define SHELL_TYPE_AUTH_MENU_BUTTON (shell_auth_menu_button_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellAuthMenuButton, shell_auth_menu_button,
                          SHELL, AUTH_MENU_BUTTON, StButton)

/**
 * ShellAuthMenuButtonClass:
 * @parent_class: the parent class
 * @activated: the user clicked the button (`_onClicked ()`); the default
 *   toggles the menu
 * @can_be_visible: whether the button has something to show (`_canBeVisible ()`)
 * @add_item: adds an item (`addItem ()`)
 * @clear_items: removes the items which match the criteria (`clearItems ()`)
 */
struct _ShellAuthMenuButtonClass
{
  StButtonClass parent_class;

  void     (* activated)      (ShellAuthMenuButton *self);
  gboolean (* can_be_visible) (ShellAuthMenuButton *self);
  void     (* add_item)       (ShellAuthMenuButton *self,
                               GVariant            *item);
  void     (* clear_items)    (ShellAuthMenuButton *self,
                               GVariant            *criteria);

  /*< private >*/
  gpointer padding[8];
};

ShellAuthMenuButton *shell_auth_menu_button_new             (const char * const *section_order,
                                                             gboolean            animate_visibility);

void      shell_auth_menu_button_add_item         (ShellAuthMenuButton *self,
                                                   GVariant            *item);
void      shell_auth_menu_button_clear_items      (ShellAuthMenuButton *self,
                                                   GVariant            *criteria);
GVariant *shell_auth_menu_button_get_items        (ShellAuthMenuButton *self,
                                                   GVariant            *criteria);
void      shell_auth_menu_button_set_active_item  (ShellAuthMenuButton *self,
                                                   GVariant            *criteria);
GVariant *shell_auth_menu_button_get_active_item  (ShellAuthMenuButton *self,
                                                   GVariant            *criteria);
void      shell_auth_menu_button_update_visibility (ShellAuthMenuButton *self,
                                                    gboolean             visible);
void      shell_auth_menu_button_close_menu       (ShellAuthMenuButton *self);

#define SHELL_TYPE_AUTH_MENU_BUTTON_INDICATOR (shell_auth_menu_button_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellAuthMenuButtonIndicator, shell_auth_menu_button_indicator,
                      SHELL, AUTH_MENU_BUTTON_INDICATOR, ShellAuthMenuButton)

ShellAuthMenuButtonIndicator *shell_auth_menu_button_indicator_new (gboolean animate_visibility);

void      shell_auth_menu_button_indicator_update_description_label (ShellAuthMenuButtonIndicator *self);

G_END_DECLS
