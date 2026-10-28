/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * date-menu.h: The clock of the top bar and the menu that opens from it,
 * with the calendar, the notifications, the events of the day, the world
 * clocks and the weather. Port of js/ui/dateMenu.js.
 *
 * Only #ShellDateMenuButton (`DateMenuButton`) is public. The other classes
 * of the JS module (`TodayButton`, `EventsSection`, `WorldClocksSection`,
 * `WeatherSection`, `MessagesIndicator`, `FreezableBinLayout` and
 * `CalendarColumnLayout`) are private to date-menu.c.
 *
 * The date menu has no knowledge of the overview, which is still JS: it
 * emits #ShellDateMenuButton::overview-hide-requested before it closes its
 * menu for an application it launches. The MPRIS media messages are added to
 * the message view by the message list itself (ui/mpris.h).
 */

#pragma once

#include "ui/calendar.h"
#include "ui/message-list.h"
#include "ui/panel-menu.h"

G_BEGIN_DECLS

#define SHELL_TYPE_DATE_MENU_BUTTON (shell_date_menu_button_get_type ())
G_DECLARE_FINAL_TYPE (ShellDateMenuButton, shell_date_menu_button,
                      SHELL, DATE_MENU_BUTTON, ShellPanelMenuButton)

ShellDateMenuButton      *shell_date_menu_button_new              (void);

ShellCalendarMessageList *shell_date_menu_button_get_message_list (ShellDateMenuButton *self);
ShellMessageView         *shell_date_menu_button_get_message_view (ShellDateMenuButton *self);

G_END_DECLS
