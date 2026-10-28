/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * calendar.h: The month grid of the date menu, the sources of the events it
 * marks, and the list of messages next to it. Port of js/ui/calendar.js.
 *
 * The types, with the name they had in the JS module:
 *
 *  - #ShellCalendarEvent: the private `CalendarEvent`, a reference counted
 *    boxed type. Times are seconds since the epoch (the calendar server sends
 *    seconds), the JS `Date` objects of `date` and `end` are
 *    shell_calendar_event_get_start () and shell_calendar_event_get_end ().
 *  - #ShellCalendarEventSource: `EventSourceBase`, abstract. `isLoading`,
 *    `hasCalendars`, `destroy ()`, `requestRange ()`, `getEvents ()` and
 *    `hasEvents ()` are the functions of the same name in snake case. The
 *    virtual functions are private to calendar.c: nothing outside of it
 *    derives from the class. The intervals that were two JS `Date` objects
 *    are two times in seconds since the epoch, the day of `hasEvents ()` is a
 *    #GDateTime in the local time zone.
 *  - #ShellCalendarEmptyEventSource: `EmptyEventSource`.
 *  - #ShellCalendarDBusEventSource: `DBusEventSource`, which reads the events
 *    from the `org.gnome.Shell.CalendarServer` service of the session bus.
 *  - #ShellCalendar: `Calendar`, the month grid.
 *  - #ShellCalendarMessageList: `CalendarMessageList`, the notifications list
 *    with its "Clear" button. MPRIS stays in JS (js/ui/mpris.js), so the list
 *    does not create the media messages itself: the owner of the list adds
 *    them to shell_calendar_message_list_get_message_view () (see
 *    js/ui/panelItems.js).
 */

#pragma once

#include <clutter/clutter.h>
#include <gio/gio.h>

#include "st.h"
#include "ui/message-list.h"
#include "ui/message-tray.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------------ */
/* ShellCalendarEvent                                                        */
/* ------------------------------------------------------------------------ */

typedef struct _ShellCalendarEvent ShellCalendarEvent;

#define SHELL_TYPE_CALENDAR_EVENT (shell_calendar_event_get_type ())
GType               shell_calendar_event_get_type    (void) G_GNUC_CONST;

ShellCalendarEvent *shell_calendar_event_new         (const char *id,
                                                      gint64      start,
                                                      gint64      end,
                                                      const char *summary);
ShellCalendarEvent *shell_calendar_event_ref         (ShellCalendarEvent *self);
void                shell_calendar_event_unref       (ShellCalendarEvent *self);

const char         *shell_calendar_event_get_id      (ShellCalendarEvent *self);
gint64              shell_calendar_event_get_start   (ShellCalendarEvent *self);
gint64              shell_calendar_event_get_end     (ShellCalendarEvent *self);
const char         *shell_calendar_event_get_summary (ShellCalendarEvent *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ShellCalendarEvent, shell_calendar_event_unref)

/* ------------------------------------------------------------------------ */
/* ShellCalendarEventSource                                                  */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_CALENDAR_EVENT_SOURCE (shell_calendar_event_source_get_type ())
G_DECLARE_DERIVABLE_TYPE (ShellCalendarEventSource, shell_calendar_event_source,
                          SHELL, CALENDAR_EVENT_SOURCE, GObject)

/**
 * ShellCalendarEventSourceClass:
 * @parent_class: the parent class
 *
 * The class of the sources of calendar events. Its virtual functions are
 * private.
 */
struct _ShellCalendarEventSourceClass
{
  GObjectClass parent_class;

  /*< private >*/
  gboolean   (* get_is_loading)    (ShellCalendarEventSource *self);
  gboolean   (* get_has_calendars) (ShellCalendarEventSource *self);
  void       (* destroy)           (ShellCalendarEventSource *self);
  void       (* request_range)     (ShellCalendarEventSource *self,
                                    gint64                    begin,
                                    gint64                    end);
  GPtrArray *(* get_events)        (ShellCalendarEventSource *self,
                                    gint64                    begin,
                                    gint64                    end);
  gboolean   (* has_events)        (ShellCalendarEventSource *self,
                                    GDateTime                *day);

  gpointer padding[8];
};

gboolean   shell_calendar_event_source_get_is_loading    (ShellCalendarEventSource *self);
gboolean   shell_calendar_event_source_get_has_calendars (ShellCalendarEventSource *self);
void       shell_calendar_event_source_destroy           (ShellCalendarEventSource *self);
void       shell_calendar_event_source_request_range     (ShellCalendarEventSource *self,
                                                          gint64                    begin,
                                                          gint64                    end);
GPtrArray *shell_calendar_event_source_get_events        (ShellCalendarEventSource *self,
                                                          gint64                    begin,
                                                          gint64                    end);
gboolean   shell_calendar_event_source_has_events        (ShellCalendarEventSource *self,
                                                          GDateTime                *day);

#define SHELL_TYPE_CALENDAR_EMPTY_EVENT_SOURCE (shell_calendar_empty_event_source_get_type ())
G_DECLARE_FINAL_TYPE (ShellCalendarEmptyEventSource, shell_calendar_empty_event_source,
                      SHELL, CALENDAR_EMPTY_EVENT_SOURCE, ShellCalendarEventSource)

ShellCalendarEmptyEventSource *shell_calendar_empty_event_source_new (void);

#define SHELL_TYPE_CALENDAR_DBUS_EVENT_SOURCE (shell_calendar_dbus_event_source_get_type ())
G_DECLARE_FINAL_TYPE (ShellCalendarDBusEventSource, shell_calendar_dbus_event_source,
                      SHELL, CALENDAR_DBUS_EVENT_SOURCE, ShellCalendarEventSource)

ShellCalendarDBusEventSource *shell_calendar_dbus_event_source_new (void);

/* ------------------------------------------------------------------------ */
/* ShellCalendar                                                             */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_CALENDAR (shell_calendar_get_type ())
G_DECLARE_FINAL_TYPE (ShellCalendar, shell_calendar, SHELL, CALENDAR, StWidget)

ShellCalendar *shell_calendar_new               (void);

void           shell_calendar_set_event_source  (ShellCalendar            *self,
                                                 ShellCalendarEventSource *event_source);
GDateTime     *shell_calendar_get_selected_date (ShellCalendar *self);
void           shell_calendar_set_date          (ShellCalendar *self,
                                                 GDateTime     *date);
void           shell_calendar_update_time_zone  (ShellCalendar *self);

/* ------------------------------------------------------------------------ */
/* ShellCalendarMessageList                                                  */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_CALENDAR_MESSAGE_LIST (shell_calendar_message_list_get_type ())
G_DECLARE_FINAL_TYPE (ShellCalendarMessageList, shell_calendar_message_list,
                      SHELL, CALENDAR_MESSAGE_LIST, StWidget)

ShellCalendarMessageList *shell_calendar_message_list_new (ShellMessageTray *message_tray);

ShellMessageView *shell_calendar_message_list_get_message_view (ShellCalendarMessageList *self);
void              shell_calendar_message_list_set_capture_container (ShellCalendarMessageList *self,
                                                                     ClutterActor             *container);

G_END_DECLS
