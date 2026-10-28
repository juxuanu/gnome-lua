/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * search-controller.h: The search entry of the overview and the search
 * results shown while the user types. Port of js/ui/searchController.js.
 *
 * The controller listens to the entry, shows the #ShellSearchResultsView
 * while there are search terms and moves the key focus between the entry and
 * the results. It does not know the overview (still JS): the overview tells
 * it with shell_search_controller_prepare_to_enter_overview (),
 * shell_search_controller_prepare_to_leave_overview (),
 * shell_search_controller_overview_showing () and
 * shell_search_controller_overview_hiding (), and it asks the overview to
 * hide or toggle with the ::overview-hide-requested and
 * ::overview-toggle-requested signals.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "st.h"

#include "ui/ctrl-alt-tab.h"
#include "ui/search.h"

G_BEGIN_DECLS

#define SHELL_TYPE_SEARCH_CONTROLLER (shell_search_controller_get_type ())
G_DECLARE_FINAL_TYPE (ShellSearchController, shell_search_controller,
                      SHELL, SEARCH_CONTROLLER, StWidget)

ShellSearchController  *shell_search_controller_new                    (StEntry                *search_entry,
                                                                        StButton               *show_apps_button,
                                                                        ShellCtrlAltTabManager *ctrl_alt_tab_manager,
                                                                        ShellSearchProvider    *application_provider);

gboolean                shell_search_controller_get_search_active      (ShellSearchController *self);
ShellSearchResultsView *shell_search_controller_get_search_results     (ShellSearchController *self);

void                    shell_search_controller_add_provider           (ShellSearchController *self,
                                                                        ShellSearchProvider   *provider);
void                    shell_search_controller_remove_provider        (ShellSearchController *self,
                                                                        ShellSearchProvider   *provider);

void                    shell_search_controller_reset                  (ShellSearchController *self);
void                    shell_search_controller_prepare_to_enter_overview (ShellSearchController *self);
void                    shell_search_controller_prepare_to_leave_overview (ShellSearchController *self);
void                    shell_search_controller_overview_showing       (ShellSearchController *self);
void                    shell_search_controller_overview_hiding        (ShellSearchController *self);

G_END_DECLS
