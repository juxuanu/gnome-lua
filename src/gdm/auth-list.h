/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * auth-list.h: The list of choices an authentication service asks the user
 * to pick from. Port of js/gdm/authList.js.
 *
 * Copyright 2017 Red Hat, Inc
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "st.h"

G_BEGIN_DECLS

/**
 * SHELL_AUTH_LIST_SCROLL_ANIMATION_TIME:
 *
 * Duration in milliseconds of the scroll to the focused item
 * (`SCROLL_ANIMATION_TIME`).
 */
#define SHELL_AUTH_LIST_SCROLL_ANIMATION_TIME 500

#define SHELL_TYPE_AUTH_LIST (shell_auth_list_get_type ())
G_DECLARE_FINAL_TYPE (ShellAuthList, shell_auth_list, SHELL, AUTH_LIST, StBoxLayout)

ShellAuthList *shell_auth_list_new          (void);

void           shell_auth_list_add_item     (ShellAuthList *self,
                                             const char    *key,
                                             GVariant      *content);
void           shell_auth_list_remove_item  (ShellAuthList *self,
                                             const char    *key);
guint          shell_auth_list_get_n_items  (ShellAuthList *self);
void           shell_auth_list_clear        (ShellAuthList *self);
void           shell_auth_list_scroll_to_item (ShellAuthList *self,
                                               ClutterActor  *item);

G_END_DECLS
