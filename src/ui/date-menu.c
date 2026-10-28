/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * date-menu.c: The clock of the top bar and the menu that opens from it.
 * Port of js/ui/dateMenu.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - A JS `Date` is seconds since the epoch while it is stored and a
 *    #GDateTime in the cached local time zone (shell_date_time_new_from_unix_msecs ())
 *    while its fields are read, as in calendar.c. System.clearDateCaches (),
 *    which only the JS engine needs, is not called.
 *  - `Main.overview.hide ()` is #ShellDateMenuButton::overview-hide-requested,
 *    `Main.panel`, `Main.sessionMode` and `Main.messageTray` are read from
 *    #ShellMain, and the media section is added by the owner of the button
 *    (see calendar.h).
 *  - The proxy of GNOME Clocks is a gdbus-codegen proxy.
 *  - `bin._delegate = this` ("some minimal compatibility with
 *    PopupMenuItem") is gone: the menu code of C does not look for it.
 *  - The branch of `_formatEventTime ()` that compared two `Date` objects with
 *    `===` could never be taken (the objects are never the same), a zero length
 *    event is shown as a range as it was; the sub-minute part of the offset
 *    of a world clock time zone is dropped.
 *  - Everything that is connected to an external object is disconnected when
 *    the actor is destroyed, and the event source is cancelled on
 *    ShellGlobal::shutdown: nothing reaches destroyed actors while the shell
 *    shuts down.
 */

#include "config.h"

#define GNOME_DESKTOP_USE_UNSTABLE_API

#include <math.h>

#include <glib/gi18n-lib.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <clutter/clutter-pango.h>
#include <libgnome-desktop/gnome-wall-clock.h>
#include <libgweather/gweather.h>

#include "misc/date-utils.h"
#include "misc/weather.h"
#include "shell-app.h"
#include "shell-app-system.h"
#include "shell-global.h"
#include "shell-util.h"
#include "ui/date-menu.h"
#include "ui/main.h"
#include "ui/panel.h"
#include "ui/popup-menu.h"
#include "ui/session-mode.h"

#include "clocks-integration-dbus.h"

#define MAX_FORECASTS 5
#define EN_CHAR "–"

#define CLOCKS_BUS_NAME "org.gnome.clocks"
#define CLOCKS_OBJECT_PATH "/org/gnome/clocks"
#define CLOCKS_APP_ID "org.gnome.clocks.desktop"

/* The JS `NC_ ()` joined the context and the message for
 * Shell.util_translate_time_string (); xgettext still sees the macro. */
#undef NC_
#define NC_(context, str) context "\004" str

#define T_(str) shell_util_translate_time_string (str)

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

enum
{
  OVERVIEW_HIDE_REQUESTED,

  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0, };

static void
set_visible (ClutterActor *actor,
             gboolean      visible)
{
  if (visible)
    clutter_actor_show (actor);
  else
    clutter_actor_hide (actor);
}

static gint64
now_secs (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

static GDateTime *
date_from_secs (gint64 secs)
{
  GDateTime *date = shell_date_time_new_from_unix_msecs (secs * 1000);

  if (date == NULL)
    date = shell_date_time_new_from_unix_msecs (0);

  return date;
}

static char *
format_secs (gint64      secs,
             const char *format)
{
  g_autoptr (GDateTime) date = date_from_secs (secs);

  return shell_date_format_with_c_format_string (date, format);
}

/* _isToday () */
static gboolean
is_today_secs (gint64 secs)
{
  g_autoptr (GDateTime) now = date_from_secs (now_secs ());
  g_autoptr (GDateTime) date = date_from_secs (secs);

  return g_date_time_get_year (now) == g_date_time_get_year (date) &&
         g_date_time_get_month (now) == g_date_time_get_month (date) &&
         g_date_time_get_day_of_month (now) == g_date_time_get_day_of_month (date);
}

/* Closes the menu for an application that is launched: the former
 * `Main.overview.hide (); Main.panel.close_calendar ();` */
static void
dismiss_for_launch (ShellDateMenuButton *owner)
{
  ShellPanel *panel;

  if (owner != NULL)
    g_signal_emit (owner, signals[OVERVIEW_HIDE_REQUESTED], 0);

  panel = shell_main_get_panel (shell_main_get_default ());
  if (panel != NULL)
    shell_panel_close_calendar (panel);
}

/* The `hookup_style ()` that environment.js added to the layout managers:
 * the spacing of the layout comes from the theme node of the container */
typedef struct
{
  ClutterLayoutManager *layout;
  gboolean is_grid;
} HookupData;

static void
hookup_style_cb (StWidget   *container,
                 HookupData *data)
{
  StThemeNode *node = st_widget_get_theme_node (container);
  double length;

  if (data->is_grid)
    {
      ClutterGridLayout *grid = CLUTTER_GRID_LAYOUT (data->layout);

      if (st_theme_node_lookup_length (node, "spacing-rows", FALSE, &length))
        clutter_grid_layout_set_row_spacing (grid, (guint) length);
      if (st_theme_node_lookup_length (node, "spacing-columns", FALSE, &length))
        clutter_grid_layout_set_column_spacing (grid, (guint) length);
    }
  else
    {
      if (st_theme_node_lookup_length (node, "spacing", FALSE, &length))
        clutter_box_layout_set_spacing (CLUTTER_BOX_LAYOUT (data->layout), (guint) length);
    }
}

static void
hookup_data_free (HookupData *data)
{
  g_object_unref (data->layout);
  g_free (data);
}

static void
hookup_style (StWidget             *container,
              ClutterLayoutManager *layout,
              gboolean              is_grid)
{
  HookupData *data = g_new0 (HookupData, 1);

  data->layout = g_object_ref (layout);
  data->is_grid = is_grid;
  g_signal_connect_data (container, "style-changed",
                         G_CALLBACK (hookup_style_cb), data,
                         (GClosureNotify) hookup_data_free, 0);
}

static ClutterActor *
new_vertical_box (const char *style_class,
                  gboolean    x_expand)
{
  ClutterActor *box = g_object_new (ST_TYPE_BOX_LAYOUT,
                                    "style-class", style_class,
                                    "x-expand", x_expand,
                                    NULL);

  st_box_layout_set_orientation (ST_BOX_LAYOUT (box), CLUTTER_ORIENTATION_VERTICAL);
  return box;
}

static ClutterActor *
new_grid_widget (const char *style_class,
                 gboolean    x_expand)
{
  ClutterActor *grid = g_object_new (ST_TYPE_WIDGET,
                                     "style-class", style_class,
                                     "x-expand", x_expand,
                                     NULL);
  ClutterLayoutManager *layout = clutter_grid_layout_new ();

  clutter_grid_layout_set_orientation (CLUTTER_GRID_LAYOUT (layout),
                                       CLUTTER_ORIENTATION_VERTICAL);
  clutter_actor_set_layout_manager (grid, layout);
  hookup_style (ST_WIDGET (grid), layout, TRUE);

  return grid;
}

/* ------------------------------------------------------------------------ */
/* ShellTodayButton                                                          */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_TODAY_BUTTON (shell_today_button_get_type ())
G_DECLARE_FINAL_TYPE (ShellTodayButton, shell_today_button,
                      SHELL, TODAY_BUTTON, StButton)

struct _ShellTodayButton
{
  StButton parent;

  ClutterActor *day_label;     /* weak */
  ClutterActor *date_label;    /* weak */
  ShellCalendar *calendar;     /* weak */
};

G_DEFINE_FINAL_TYPE (ShellTodayButton, shell_today_button, ST_TYPE_BUTTON)

static void
today_button_selected_date_changed_cb (ShellCalendar    *calendar,
                                       GDateTime        *datetime,
                                       ShellTodayButton *self)
{
  /* Make the button reactive only if the selected date is not the
   * current date. */
  clutter_actor_set_reactive (CLUTTER_ACTOR (self),
                              !is_today_secs (g_date_time_to_unix (datetime)));
}

static void
today_button_set_calendar (ShellTodayButton *self,
                           ShellCalendar    *calendar)
{
  g_set_weak_pointer (&self->calendar, calendar);
  g_signal_connect_object (calendar, "selected-date-changed",
                           G_CALLBACK (today_button_selected_date_changed_cb),
                           self, 0);
}

static void
today_button_set_date (ShellTodayButton *self,
                       gint64            secs)
{
  g_autofree char *day = format_secs (secs, "%A");
  g_autofree char *date_format = NULL;
  g_autofree char *date_text = NULL;
  g_autofree char *accessible_format = NULL;
  g_autofree char *accessible_name = NULL;

  st_label_set_text (ST_LABEL (self->day_label), day);

  /* Translators: This is the date format to use when the calendar popup is
   * shown - it is shown just below the time in the top bar (e.g.,
   * "Tue 9:29 AM").  The string itself should become a full date, e.g.,
   * "February 17 2015".
   */
  /* xgettext:no-c-format */
  date_format = g_strdup (T_(N_("%B %-d %Y")));
  date_text = format_secs (secs, date_format);
  st_label_set_text (ST_LABEL (self->date_label), date_text);

  /* Translators: This is the accessible name of the date button shown
   * below the time in the shell; it should combine the weekday and the
   * date, e.g. "Tuesday February 17 2015".
   */
  /* xgettext:no-c-format */
  accessible_format = g_strdup (T_(N_("%A %B %e %Y")));
  accessible_name = format_secs (secs, accessible_format);
  clutter_actor_set_accessible_name (CLUTTER_ACTOR (self), accessible_name);
}

static void
shell_today_button_clicked (StButton *button,
                            int       clicked_button)
{
  ShellTodayButton *self = SHELL_TODAY_BUTTON (button);
  g_autoptr (GDateTime) now = NULL;

  if (self->calendar == NULL)
    return;

  now = date_from_secs (now_secs ());
  shell_calendar_set_date (self->calendar, now);
}

static void
shell_today_button_dispose (GObject *object)
{
  ShellTodayButton *self = SHELL_TODAY_BUTTON (object);

  if (self->calendar != NULL)
    g_signal_handlers_disconnect_by_data (self->calendar, self);

  g_clear_weak_pointer (&self->calendar);
  g_clear_weak_pointer (&self->day_label);
  g_clear_weak_pointer (&self->date_label);

  G_OBJECT_CLASS (shell_today_button_parent_class)->dispose (object);
}

static void
shell_today_button_class_init (ShellTodayButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->dispose = shell_today_button_dispose;

  button_class->clicked = shell_today_button_clicked;
}

static void
shell_today_button_init (ShellTodayButton *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *hbox, *day_label, *date_label;

  /* Having the ability to go to the current date if the user is already
   * on the current date can be confusing. So don't make the button reactive
   * until the selected date changes. */
  st_widget_set_style_class_name (ST_WIDGET (self), "datemenu-today-button");
  clutter_actor_set_x_expand (actor, TRUE);
  st_widget_set_can_focus (ST_WIDGET (self), TRUE);
  clutter_actor_set_reactive (actor, FALSE);

  hbox = new_vertical_box (NULL, TRUE);
  st_bin_set_child (ST_BIN (self), hbox);

  day_label = g_object_new (ST_TYPE_LABEL,
                            "style-class", "day-label",
                            "x-align", CLUTTER_ACTOR_ALIGN_START,
                            NULL);
  g_set_weak_pointer (&self->day_label, day_label);
  clutter_actor_add_child (hbox, day_label);

  date_label = g_object_new (ST_TYPE_LABEL,
                             "style-class", "date-label",
                             NULL);
  g_set_weak_pointer (&self->date_label, date_label);
  clutter_actor_add_child (hbox, date_label);
}

static ShellTodayButton *
today_button_new (ShellCalendar *calendar)
{
  ShellTodayButton *self = g_object_new (SHELL_TYPE_TODAY_BUTTON, NULL);

  today_button_set_calendar (self, calendar);
  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellEventsSection                                                        */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_EVENTS_SECTION (shell_events_section_get_type ())
G_DECLARE_FINAL_TYPE (ShellEventsSection, shell_events_section,
                      SHELL, EVENTS_SECTION, StButton)

struct _ShellEventsSection
{
  StButton parent;

  ShellDateMenuButton *owner;  /* weak */

  gboolean has_range;
  gint64 start_date;           /* seconds since the epoch */
  gint64 end_date;

  ShellCalendarEventSource *event_source;
  GAppInfo *calendar_app;

  ClutterActor *title;         /* weak */
  ClutterActor *events_list;   /* weak */
  ShellAppSystem *app_sys;     /* weak */
  gboolean reloading;
};

G_DEFINE_FINAL_TYPE (ShellEventsSection, shell_events_section, ST_TYPE_BUTTON)

/* _sync () */
static void
events_section_sync (ShellEventsSection *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  set_visible (actor,
                             self->event_source != NULL &&
                             shell_calendar_event_source_get_has_calendars (self->event_source));
  clutter_actor_set_reactive (actor, self->calendar_app != NULL);
}

/* _updateTitle () */
static void
events_section_update_title (ShellEventsSection *self)
{
  /* Translators: Shown on calendar heading when selected day occurs on current year */
  /* xgettext:no-c-format */
  g_autofree char *same_year_format = g_strdup (T_(NC_("calendar heading", "%B %-d")));

  /* Translators: Shown on calendar heading when selected day occurs on different year */
  /* xgettext:no-c-format */
  g_autofree char *other_year_format = g_strdup (T_(NC_("calendar heading", "%B %-d %Y")));

  const gint64 time_span_day = G_TIME_SPAN_DAY / 1000;
  gint64 now = g_get_real_time () / 1000;
  gint64 start = self->start_date * 1000;
  gint64 end = self->end_date * 1000;
  g_autoptr (GDateTime) start_date = date_from_secs (self->start_date);
  g_autoptr (GDateTime) now_date = date_from_secs (now / 1000);
  g_autofree char *text = NULL;

  if (start <= now && now < end)
    text = g_strdup (_("Today"));
  else if (end <= now && now - end < time_span_day)
    text = g_strdup (_("Yesterday"));
  else if (start > now && start - now <= time_span_day)
    text = g_strdup (_("Tomorrow"));
  else if (g_date_time_get_year (start_date) == g_date_time_get_year (now_date))
    text = shell_date_format_with_c_format_string (start_date, same_year_format);
  else
    text = shell_date_format_with_c_format_string (start_date, other_year_format);

  st_label_set_text (ST_LABEL (self->title), text);
}

static gboolean
is_at_midnight (gint64 secs)
{
  g_autoptr (GDateTime) date = date_from_secs (secs);

  return g_date_time_get_hour (date) == 0 &&
         g_date_time_get_minute (date) == 0 &&
         (int) g_date_time_get_seconds (date) == 0;
}

/* _formatEventTime () */
static char *
events_section_format_event_time (ShellEventsSection *self,
                                  ShellCalendarEvent *event)
{
  gint64 event_start = shell_calendar_event_get_start (event);
  gint64 event_end = shell_calendar_event_get_end (event);
  gboolean all_day = event_start == self->start_date && event_end == self->end_date;
  gboolean starts_before_today = event_start < self->start_date;
  gboolean ends_after_today = event_end > self->end_date;
  g_autofree char *start_time_only = shell_date_format_time_unix_msecs (event_start * 1000,
                                                                       SHELL_DATE_FORMAT_FLAGS_TIME_ONLY);
  g_autofree char *end_time_only = shell_date_format_time_unix_msecs (event_end * 1000,
                                                                     SHELL_DATE_FORMAT_FLAGS_TIME_ONLY);
  gboolean rtl = clutter_get_default_text_direction () == CLUTTER_TEXT_DIRECTION_RTL;

  if (all_day)
    {
      /* Translators: Shown in calendar event list for all day events
       * Keep it short, best if you can use less then 10 characters
       */
      return g_strdup (C_("event list time", "All Day"));
    }
  else if (starts_before_today || ends_after_today)
    {
      g_autoptr (GDateTime) now = date_from_secs (now_secs ());
      g_autoptr (GDateTime) start = date_from_secs (event_start);
      g_autoptr (GDateTime) end = date_from_secs (event_end);
      g_autofree char *format = NULL;
      g_autofree char *start_date_only = NULL;
      g_autofree char *end_date_only = NULL;
      int this_year = g_date_time_get_year (now);
      int start_year = g_date_time_get_year (start);
      int end_year;
      gboolean starts_at_midnight = is_at_midnight (event_start);
      gboolean ends_at_midnight = is_at_midnight (event_end);

      if (ends_at_midnight)
        {
          GDateTime *previous_day = g_date_time_add_days (end, -1);

          g_date_time_unref (end);
          end = previous_day;
        }

      end_year = g_date_time_get_year (end);

      if (start_year == this_year && this_year == end_year)
        {
          /* Translators: Shown in calendar event list as the start/end of events
           * that only show day and month
           */
          /* xgettext:no-c-format */
          format = g_strdup (T_(N_("%m/%d")));
        }
      else
        {
          format = g_strdup ("%x");
        }

      start_date_only = shell_date_format_with_c_format_string (start, format);
      end_date_only = shell_date_format_with_c_format_string (end, format);

      if (starts_at_midnight && ends_at_midnight)
        return g_strdup_printf ("%s %s %s",
                                rtl ? end_date_only : start_date_only,
                                EN_CHAR,
                                rtl ? start_date_only : end_date_only);
      else if (rtl)
        return g_strdup_printf ("%s %s %s %s %s",
                                end_time_only, end_date_only, EN_CHAR,
                                start_time_only, start_date_only);
      else
        return g_strdup_printf ("%s %s %s %s %s",
                                start_date_only, start_time_only, EN_CHAR,
                                end_date_only, end_time_only);
    }

  return g_strdup_printf ("%s %s %s",
                          rtl ? end_time_only : start_time_only,
                          EN_CHAR,
                          rtl ? start_time_only : end_time_only);
}

/* _reloadEvents () */
static void
events_section_reload_events (ShellEventsSection *self)
{
  g_autoptr (GPtrArray) events = NULL;
  guint i;

  if (self->event_source == NULL || self->events_list == NULL)
    return;

  if (shell_calendar_event_source_get_is_loading (self->event_source) || self->reloading)
    return;

  self->reloading = TRUE;

  clutter_actor_destroy_all_children (self->events_list);

  if (self->has_range)
    events = shell_calendar_event_source_get_events (self->event_source,
                                                     self->start_date,
                                                     self->end_date);
  else
    events = g_ptr_array_new ();

  for (i = 0; i < events->len; i++)
    {
      ShellCalendarEvent *event = g_ptr_array_index (events, i);
      g_autofree char *time_text = events_section_format_event_time (self, event);
      ClutterActor *box = new_vertical_box ("event-box", FALSE);
      ClutterActor *summary, *time_label;

      summary = g_object_new (ST_TYPE_LABEL,
                              "text", shell_calendar_event_get_summary (event),
                              "style-class", "event-summary",
                              NULL);
      clutter_actor_add_child (box, summary);

      time_label = g_object_new (ST_TYPE_LABEL,
                                 "text", time_text,
                                 "style-class", "event-time",
                                 NULL);
      clutter_actor_add_child (box, time_label);

      clutter_actor_add_child (self->events_list, box);
    }

  if (clutter_actor_get_n_children (self->events_list) == 0)
    {
      ClutterActor *placeholder = g_object_new (ST_TYPE_LABEL,
                                                "text", _("No Events"),
                                                "style-class", "event-placeholder",
                                                NULL);

      clutter_actor_add_child (self->events_list, placeholder);
    }

  self->reloading = FALSE;
  events_section_sync (self);
}

static void
events_section_set_date (ShellEventsSection *self,
                         GDateTime          *date)
{
  g_autoptr (GDateTime) start = g_date_time_new (g_date_time_get_timezone (date),
                                                 g_date_time_get_year (date),
                                                 g_date_time_get_month (date),
                                                 g_date_time_get_day_of_month (date),
                                                 0, 0, 0);
  g_autoptr (GDateTime) end = g_date_time_add_days (start, 1);

  self->has_range = TRUE;
  self->start_date = g_date_time_to_unix (start);
  self->end_date = g_date_time_to_unix (end);

  events_section_update_title (self);
  events_section_reload_events (self);
}

static void
events_section_source_changed_cb (ShellCalendarEventSource *source,
                                  ShellEventsSection       *self)
{
  events_section_reload_events (self);
}

static void
events_section_source_has_calendars_cb (ShellCalendarEventSource *source,
                                        GParamSpec               *pspec,
                                        ShellEventsSection       *self)
{
  events_section_sync (self);
}

static void
events_section_set_event_source (ShellEventsSection       *self,
                                 ShellCalendarEventSource *event_source)
{
  if (self->event_source != NULL)
    g_signal_handlers_disconnect_by_data (self->event_source, self);

  g_set_object (&self->event_source, event_source);

  g_signal_connect_object (self->event_source, "changed",
                           G_CALLBACK (events_section_source_changed_cb), self, 0);
  g_signal_connect_object (self->event_source, "notify::has-calendars",
                           G_CALLBACK (events_section_source_has_calendars_cb), self, 0);
  events_section_sync (self);
}

/* _appInstalledChanged () */
static void
events_section_app_installed_changed (ShellEventsSection *self)
{
  g_autolist (GAppInfo) apps = g_app_info_get_recommended_for_type ("text/calendar");

  g_clear_object (&self->calendar_app);

  if (apps != NULL)
    {
      g_autoptr (GAppInfo) app = g_app_info_get_default_for_type ("text/calendar", FALSE);
      gboolean default_in_recommended = FALSE;
      GList *l;

      for (l = apps; l != NULL && app != NULL; l = l->next)
        {
          if (g_app_info_equal (l->data, app))
            {
              default_in_recommended = TRUE;
              break;
            }
        }

      self->calendar_app = g_object_ref (default_in_recommended ? app : apps->data);
    }

  events_section_sync (self);
}

static void
shell_events_section_clicked (StButton *button,
                              int       clicked_button)
{
  ShellEventsSection *self = SHELL_EVENTS_SECTION (button);
  g_autoptr (GAppLaunchContext) context = NULL;
  g_autoptr (GError) error = NULL;
  gboolean launched;

  dismiss_for_launch (self->owner);

  if (self->calendar_app == NULL)
    return;

  context = shell_global_create_app_launch_context (shell_global_get (), 0, -1);
  if (g_strcmp0 (g_app_info_get_id (self->calendar_app), "org.gnome.Evolution.desktop") == 0)
    {
      if (G_IS_DESKTOP_APP_INFO (self->calendar_app))
        g_desktop_app_info_launch_action (G_DESKTOP_APP_INFO (self->calendar_app),
                                          "calendar", context);
      launched = TRUE;
    }
  else
    {
      launched = g_app_info_launch (self->calendar_app, NULL, context, &error);
    }

  if (!launched)
    g_warning ("Failed to launch the calendar application: %s", error->message);
}

static void
shell_events_section_destroy (ClutterActor *actor)
{
  ShellEventsSection *self = SHELL_EVENTS_SECTION (actor);

  if (self->event_source != NULL)
    g_signal_handlers_disconnect_by_data (self->event_source, self);
  if (self->app_sys != NULL)
    g_signal_handlers_disconnect_by_data (self->app_sys, self);

  CLUTTER_ACTOR_CLASS (shell_events_section_parent_class)->destroy (actor);
}

static void
shell_events_section_dispose (GObject *object)
{
  ShellEventsSection *self = SHELL_EVENTS_SECTION (object);

  if (self->event_source != NULL)
    g_signal_handlers_disconnect_by_data (self->event_source, self);
  if (self->app_sys != NULL)
    g_signal_handlers_disconnect_by_data (self->app_sys, self);

  g_clear_object (&self->event_source);
  g_clear_object (&self->calendar_app);
  g_clear_weak_pointer (&self->owner);
  g_clear_weak_pointer (&self->title);
  g_clear_weak_pointer (&self->events_list);
  g_clear_weak_pointer (&self->app_sys);

  G_OBJECT_CLASS (shell_events_section_parent_class)->dispose (object);
}

static void
shell_events_section_class_init (ShellEventsSectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->dispose = shell_events_section_dispose;

  actor_class->destroy = shell_events_section_destroy;

  button_class->clicked = shell_events_section_clicked;
}

static void
shell_events_section_init (ShellEventsSection *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *box, *title, *events_list;
  ShellAppSystem *app_sys;

  st_widget_set_style_class_name (ST_WIDGET (self), "events-button");
  st_widget_set_can_focus (ST_WIDGET (self), TRUE);
  clutter_actor_set_x_expand (actor, TRUE);

  box = new_vertical_box ("events-box", TRUE);
  st_bin_set_child (ST_BIN (self), box);

  title = g_object_new (ST_TYPE_LABEL,
                        "style-class", "events-title",
                        NULL);
  g_set_weak_pointer (&self->title, title);
  clutter_actor_add_child (box, title);
  st_widget_set_label_actor (ST_WIDGET (self), title);

  events_list = new_vertical_box ("events-list", TRUE);
  g_set_weak_pointer (&self->events_list, events_list);
  clutter_actor_add_child (box, events_list);

  app_sys = shell_app_system_get_default ();
  g_set_weak_pointer (&self->app_sys, app_sys);
  g_signal_connect_object (app_sys, "installed-changed",
                           G_CALLBACK (events_section_app_installed_changed),
                           self, G_CONNECT_SWAPPED);
  events_section_app_installed_changed (self);
}

static ShellEventsSection *
events_section_new (ShellDateMenuButton *owner)
{
  ShellEventsSection *self = g_object_new (SHELL_TYPE_EVENTS_SECTION, NULL);

  g_set_weak_pointer (&self->owner, owner);
  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellWorldClocksSection                                                   */
/* ------------------------------------------------------------------------ */

typedef struct
{
  GWeatherLocation *location;
  ClutterActor *time_label;   /* weak */
  ClutterActor *tz_label;     /* weak */
} ClockLocation;

static void
clock_location_free (ClockLocation *l)
{
  g_clear_weak_pointer (&l->time_label);
  g_clear_weak_pointer (&l->tz_label);
  g_clear_object (&l->location);
  g_free (l);
}

#define SHELL_TYPE_WORLD_CLOCKS_SECTION (shell_world_clocks_section_get_type ())
G_DECLARE_FINAL_TYPE (ShellWorldClocksSection, shell_world_clocks_section,
                      SHELL, WORLD_CLOCKS_SECTION, StButton)

struct _ShellWorldClocksSection
{
  StButton parent;

  ShellDateMenuButton *owner;      /* weak */

  GnomeWallClock *clock;
  gulong clock_notify_id;
  gulong tz_notify_id;

  GPtrArray *locations;            /* of ClockLocation */
  ClutterActor *grid;              /* weak */

  ShellApp *clocks_app;            /* strong */
  ShellClocksIntegration *clocks_proxy;
  GCancellable *cancellable;
  GSettings *settings;
  ShellAppSystem *app_system;      /* weak */
};

G_DEFINE_FINAL_TYPE (ShellWorldClocksSection, shell_world_clocks_section, ST_TYPE_BUTTON)

/* _getTimezoneOffsetAtLocation () */
static char *
world_clocks_get_timezone_offset_at_location (GWeatherLocation *location)
{
  GTimeZone *tz = gweather_location_get_timezone (location);
  g_autoptr (GDateTime) local_now = g_date_time_new_now_local ();
  g_autoptr (GDateTime) tz_now = g_date_time_new_now (tz);
  gint64 offset_current_tz = g_date_time_get_utc_offset (tz_now) -
                             g_date_time_get_utc_offset (local_now);
  gint64 absolute = offset_current_tz < 0 ? -offset_current_tz : offset_current_tz;
  gint64 offset_hours = absolute / G_TIME_SPAN_HOUR;
  gint64 offset_minutes = (absolute % G_TIME_SPAN_HOUR) / G_TIME_SPAN_MINUTE;
  const char *prefix = offset_current_tz >= 0 ? "+" : "-";

  if (offset_minutes == 0)
    return g_strdup_printf ("%s%" G_GINT64_FORMAT, prefix, offset_hours);

  return g_strdup_printf ("%s%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
                          prefix, offset_hours, offset_minutes);
}

/* _updateTimeLabels () */
static void
world_clocks_update_time_labels (ShellWorldClocksSection *self)
{
  guint i;

  for (i = 0; i < self->locations->len; i++)
    {
      ClockLocation *l = g_ptr_array_index (self->locations, i);
      g_autoptr (GDateTime) now = g_date_time_new_now (gweather_location_get_timezone (l->location));
      g_autofree char *text = shell_date_format_time (now, SHELL_DATE_FORMAT_FLAGS_TIME_ONLY);

      if (l->time_label != NULL)
        st_label_set_text (ST_LABEL (l->time_label), text);
    }
}

/* _updateTimezoneLabels () */
static void
world_clocks_update_timezone_labels (ShellWorldClocksSection *self)
{
  guint i;

  for (i = 0; i < self->locations->len; i++)
    {
      ClockLocation *l = g_ptr_array_index (self->locations, i);
      g_autofree char *text = world_clocks_get_timezone_offset_at_location (l->location);

      if (l->tz_label != NULL)
        st_label_set_text (ST_LABEL (l->tz_label), text);
    }
}

static int
compare_clock_locations (gconstpointer a,
                         gconstpointer b,
                         gpointer      user_data)
{
  const ClockLocation *loc_a = *(ClockLocation * const *) a;
  const ClockLocation *loc_b = *(ClockLocation * const *) b;
  gint64 unixtime = *(gint64 *) user_data;
  GTimeZone *tz_a = gweather_location_get_timezone (loc_a->location);
  GTimeZone *tz_b = gweather_location_get_timezone (loc_b->location);
  int int_a = g_time_zone_find_interval (tz_a, G_TIME_TYPE_STANDARD, unixtime);
  int int_b = g_time_zone_find_interval (tz_b, G_TIME_TYPE_STANDARD, unixtime);
  gint32 offset_a = g_time_zone_get_offset (tz_a, int_a);
  gint32 offset_b = g_time_zone_get_offset (tz_b, int_b);

  return offset_a < offset_b ? -1 : (offset_a > offset_b ? 1 : 0);
}

/* _clocksChanged () */
static void
world_clocks_clocks_changed (ShellWorldClocksSection *self)
{
  g_autoptr (GWeatherLocation) world = gweather_location_get_world ();
  g_autoptr (GVariant) clocks = NULL;
  ClutterLayoutManager *layout;
  ClutterActor *header;
  const char *title;
  gboolean rtl;
  gint64 unixtime;
  gsize n_clocks, i;
  guint j;

  if (self->grid == NULL)
    return;

  clutter_actor_destroy_all_children (self->grid);
  g_ptr_array_set_size (self->locations, 0);

  clocks = g_settings_get_value (self->settings, "locations");
  n_clocks = g_variant_n_children (clocks);
  for (i = 0; world != NULL && i < n_clocks; i++)
    {
      g_autoptr (GVariant) boxed = g_variant_get_child_value (clocks, i);
      g_autoptr (GVariant) serialized = g_variant_get_variant (boxed);
      GWeatherLocation *l = gweather_location_deserialize (world, serialized);

      if (l != NULL && gweather_location_get_timezone (l) != NULL)
        {
          ClockLocation *loc = g_new0 (ClockLocation, 1);

          loc->location = l;
          g_ptr_array_add (self->locations, loc);
        }
      else if (l != NULL)
        {
          g_object_unref (l);
        }
    }

  unixtime = now_secs ();
  g_ptr_array_sort_with_data (self->locations, compare_clock_locations, &unixtime);

  layout = clutter_actor_get_layout_manager (self->grid);
  title = self->locations->len == 0 ? _("Add World Clocks…") : _("World Clocks");
  header = g_object_new (ST_TYPE_LABEL,
                         "style-class", "world-clocks-header",
                         "x-align", CLUTTER_ACTOR_ALIGN_START,
                         "text", title,
                         NULL);

  if (self->locations->len == 0)
    st_widget_add_style_class_name (ST_WIDGET (header), "no-world-clocks");
  else
    st_widget_remove_style_class_name (ST_WIDGET (header), "no-world-clocks");

  rtl = clutter_actor_get_text_direction (self->grid) == CLUTTER_TEXT_DIRECTION_RTL;
  if (rtl)
    clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), header, 2, 0, 1, 1);
  else
    clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), header, 0, 0, 2, 1);
  st_widget_set_label_actor (ST_WIDGET (self), header);

  for (j = 0; j < self->locations->len; j++)
    {
      ClockLocation *loc = g_ptr_array_index (self->locations, j);
      g_autofree char *city_name = gweather_location_get_city_name (loc->location);
      const char *name = (city_name != NULL && *city_name != '\0') ?
                         city_name : gweather_location_get_name (loc->location);
      ClutterActor *label, *time_label, *tz_label;

      label = g_object_new (ST_TYPE_LABEL,
                            "style-class", "world-clocks-city",
                            "text", name,
                            "x-align", CLUTTER_ACTOR_ALIGN_START,
                            "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                            "x-expand", TRUE,
                            NULL);

      time_label = g_object_new (ST_TYPE_LABEL,
                                 "style-class", "world-clocks-time",
                                 NULL);

      tz_label = g_object_new (ST_TYPE_LABEL,
                               "style-class", "world-clocks-timezone",
                               "x-align", CLUTTER_ACTOR_ALIGN_END,
                               "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                               NULL);

      clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (time_label)),
                                  PANGO_ELLIPSIZE_NONE);
      clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (tz_label)),
                                  PANGO_ELLIPSIZE_NONE);

      if (rtl)
        {
          clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), tz_label, 0, j + 1, 1, 1);
          clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), time_label, 1, j + 1, 1, 1);
          clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), label, 2, j + 1, 1, 1);
        }
      else
        {
          clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), label, 0, j + 1, 1, 1);
          clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), time_label, 1, j + 1, 1, 1);
          clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), tz_label, 2, j + 1, 1, 1);
        }

      g_set_weak_pointer (&loc->time_label, time_label);
      g_set_weak_pointer (&loc->tz_label, tz_label);
    }

  if (clutter_actor_get_n_children (self->grid) > 1)
    {
      if (self->clock_notify_id == 0)
        self->clock_notify_id =
          g_signal_connect_object (self->clock, "notify::clock",
                                   G_CALLBACK (world_clocks_update_time_labels),
                                   self, G_CONNECT_SWAPPED);
      if (self->tz_notify_id == 0)
        self->tz_notify_id =
          g_signal_connect_object (self->clock, "notify::timezone",
                                   G_CALLBACK (world_clocks_update_timezone_labels),
                                   self, G_CONNECT_SWAPPED);

      world_clocks_update_time_labels (self);
      world_clocks_update_timezone_labels (self);
    }
  else
    {
      if (self->clock_notify_id != 0)
        g_signal_handler_disconnect (self->clock, self->clock_notify_id);
      self->clock_notify_id = 0;

      if (self->tz_notify_id != 0)
        g_signal_handler_disconnect (self->clock, self->tz_notify_id);
      self->tz_notify_id = 0;
    }
}

/* _sync () */
static void
world_clocks_sync (ShellWorldClocksSection *self)
{
  g_set_object (&self->clocks_app,
                shell_app_system_lookup_app (self->app_system, CLOCKS_APP_ID));
  set_visible (CLUTTER_ACTOR (self), self->clocks_app != NULL);
}

/* _onClocksPropertiesChanged () */
static void
world_clocks_clocks_properties_changed (ShellWorldClocksSection *self)
{
  g_autofree char *owner = NULL;
  GVariant *locations;

  if (self->clocks_proxy == NULL)
    return;

  owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (self->clocks_proxy));
  if (owner == NULL)
    return;

  locations = shell_clocks_integration_get_locations (self->clocks_proxy);
  if (locations != NULL)
    g_settings_set_value (self->settings, "locations", locations);
}

/* _onProxyReady () */
static void
world_clocks_proxy_ready_cb (GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  g_autoptr (ShellWorldClocksSection) self = user_data;
  g_autoptr (GError) error = NULL;
  ShellClocksIntegration *proxy;

  proxy = shell_clocks_integration_proxy_new_for_bus_finish (res, &error);

  if (proxy == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_message ("Failed to create GNOME Clocks proxy: %s", error->message);
      return;
    }

  /* The shell is shutting down, or the section was destroyed */
  if (self->cancellable == NULL || g_cancellable_is_cancelled (self->cancellable))
    {
      g_object_unref (proxy);
      return;
    }

  self->clocks_proxy = proxy;
  g_signal_connect_object (proxy, "g-properties-changed",
                           G_CALLBACK (world_clocks_clocks_properties_changed),
                           self, G_CONNECT_SWAPPED);
  world_clocks_clocks_properties_changed (self);
}

static void
world_clocks_release (ShellWorldClocksSection *self)
{
  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  if (self->clocks_proxy != NULL)
    g_signal_handlers_disconnect_by_data (self->clocks_proxy, self);
  if (self->settings != NULL)
    g_signal_handlers_disconnect_by_data (self->settings, self);
  if (self->app_system != NULL)
    g_signal_handlers_disconnect_by_data (self->app_system, self);
  if (self->clock != NULL)
    g_signal_handlers_disconnect_by_data (self->clock, self);
  self->clock_notify_id = 0;
  self->tz_notify_id = 0;
}

static void
shell_world_clocks_section_clicked (StButton *button,
                                    int       clicked_button)
{
  ShellWorldClocksSection *self = SHELL_WORLD_CLOCKS_SECTION (button);

  if (self->clocks_app != NULL)
    shell_app_activate (self->clocks_app);

  dismiss_for_launch (self->owner);
}

static void
shell_world_clocks_section_destroy (ClutterActor *actor)
{
  world_clocks_release (SHELL_WORLD_CLOCKS_SECTION (actor));

  CLUTTER_ACTOR_CLASS (shell_world_clocks_section_parent_class)->destroy (actor);
}

static void
shell_world_clocks_section_dispose (GObject *object)
{
  ShellWorldClocksSection *self = SHELL_WORLD_CLOCKS_SECTION (object);

  world_clocks_release (self);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->clocks_proxy);
  g_clear_object (&self->settings);
  g_clear_object (&self->clock);
  g_clear_object (&self->clocks_app);
  g_clear_weak_pointer (&self->owner);
  g_clear_weak_pointer (&self->grid);
  g_clear_weak_pointer (&self->app_system);

  G_OBJECT_CLASS (shell_world_clocks_section_parent_class)->dispose (object);
}

static void
shell_world_clocks_section_finalize (GObject *object)
{
  ShellWorldClocksSection *self = SHELL_WORLD_CLOCKS_SECTION (object);

  g_clear_pointer (&self->locations, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_world_clocks_section_parent_class)->finalize (object);
}

static void
shell_world_clocks_section_class_init (ShellWorldClocksSectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->dispose = shell_world_clocks_section_dispose;
  object_class->finalize = shell_world_clocks_section_finalize;

  actor_class->destroy = shell_world_clocks_section_destroy;

  button_class->clicked = shell_world_clocks_section_clicked;
}

static void
shell_world_clocks_section_init (ShellWorldClocksSection *self)
{
  ClutterActor *grid;
  ShellAppSystem *app_system;

  st_widget_set_style_class_name (ST_WIDGET (self), "world-clocks-button");
  st_widget_set_can_focus (ST_WIDGET (self), TRUE);
  clutter_actor_set_x_expand (CLUTTER_ACTOR (self), TRUE);

  self->clock = gnome_wall_clock_new ();
  self->locations = g_ptr_array_new_with_free_func ((GDestroyNotify) clock_location_free);
  self->cancellable = g_cancellable_new ();

  grid = new_grid_widget ("world-clocks-grid", TRUE);
  g_set_weak_pointer (&self->grid, grid);
  st_bin_set_child (ST_BIN (self), grid);

  shell_clocks_integration_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                              G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START |
                                              G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES,
                                              CLOCKS_BUS_NAME,
                                              CLOCKS_OBJECT_PATH,
                                              self->cancellable,
                                              world_clocks_proxy_ready_cb,
                                              g_object_ref (self));

  self->settings = g_settings_new ("org.gnome.shell.world-clocks");
  g_signal_connect_object (self->settings, "changed",
                           G_CALLBACK (world_clocks_clocks_changed),
                           self, G_CONNECT_SWAPPED);
  world_clocks_clocks_changed (self);

  app_system = shell_app_system_get_default ();
  g_set_weak_pointer (&self->app_system, app_system);
  g_signal_connect_object (app_system, "installed-changed",
                           G_CALLBACK (world_clocks_sync),
                           self, G_CONNECT_SWAPPED);
  world_clocks_sync (self);

  /* The proxy and the settings stop reaching the section once the shell is
   * going away */
  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (world_clocks_release),
                           self, G_CONNECT_SWAPPED);
}

static ShellWorldClocksSection *
world_clocks_section_new (ShellDateMenuButton *owner)
{
  ShellWorldClocksSection *self = g_object_new (SHELL_TYPE_WORLD_CLOCKS_SECTION, NULL);

  g_set_weak_pointer (&self->owner, owner);
  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellWeatherSection                                                       */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_WEATHER_SECTION (shell_weather_section_get_type ())
G_DECLARE_FINAL_TYPE (ShellWeatherSection, shell_weather_section,
                      SHELL, WEATHER_SECTION, StButton)

struct _ShellWeatherSection
{
  StButton parent;

  ShellDateMenuButton *owner;      /* weak */

  ShellWeatherClient *weather_client;
  ClutterActor *title_label;       /* weak */
  ClutterActor *title_location;    /* weak */
  ClutterActor *forecast_grid;     /* weak */
};

G_DEFINE_FINAL_TYPE (ShellWeatherSection, shell_weather_section, ST_TYPE_BUTTON)

/* _getInfos (): the forecasts to show, at most MAX_FORECASTS of them */
static GPtrArray *
weather_section_get_infos (ShellWeatherSection *self)
{
  GWeatherInfo *info = shell_weather_client_get_info (self->weather_client);
  GPtrArray *infos = g_ptr_array_new ();
  gint64 now_us = g_get_real_time ();
  gint64 current_us = 0;
  GSList *l;

  for (l = gweather_info_get_forecast_list (info); l != NULL; l = l->next)
    {
      GWeatherInfo *forecast = l->data;
      time_t timestamp = 0;
      gint64 datetime_us;

      if (!gweather_info_get_value_update (forecast, &timestamp) || timestamp == 0)
        continue;  /* 0 means 'never updated' */

      datetime_us = (gint64) timestamp * G_USEC_PER_SEC;
      if (now_us - datetime_us > 0)
        continue; /* Ignore earlier forecasts */

      if (datetime_us - current_us < G_TIME_SPAN_HOUR)
        continue; /* Enforce a minimum interval of 1h */

      g_ptr_array_add (infos, forecast);
      if (infos->len == MAX_FORECASTS)
        break; /* Use a maximum of five forecasts */

      current_us = datetime_us;
    }

  return infos;
}

/* _addForecasts () */
static void
weather_section_add_forecasts (ShellWeatherSection *self)
{
  ClutterLayoutManager *layout = clutter_actor_get_layout_manager (self->forecast_grid);
  g_autoptr (GPtrArray) infos = weather_section_get_infos (self);
  guint col = 0;
  guint i;

  if (clutter_actor_get_text_direction (self->forecast_grid) == CLUTTER_TEXT_DIRECTION_RTL)
    {
      guint n = infos->len;

      for (i = 0; i < n / 2; i++)
        {
          gpointer tmp = g_ptr_array_index (infos, i);

          infos->pdata[i] = infos->pdata[n - 1 - i];
          infos->pdata[n - 1 - i] = tmp;
        }
    }

  for (i = 0; i < infos->len; i++)
    {
      GWeatherInfo *fc = g_ptr_array_index (infos, i);
      time_t timestamp = 0;
      double temp_value = 0.0;
      g_autofree char *time_str = NULL;
      g_autofree char *temp_text = NULL;
      int rounded;
      ClutterActor *time_label, *icon, *temp;

      gweather_info_get_value_update (fc, &timestamp);
      time_str = shell_date_format_time_unix_msecs ((gint64) timestamp * 1000,
                                                    SHELL_DATE_FORMAT_FLAGS_TIME_ONLY |
                                                    SHELL_DATE_FORMAT_FLAGS_NO_AMPM);
      gweather_info_get_value_temp (fc, GWEATHER_TEMP_UNIT_DEFAULT, &temp_value);

      /* Math.round () rounds halves up */
      rounded = (int) floor (temp_value + 0.5);
      temp_text = g_strdup_printf ("%s%d°", rounded >= 0 ? " " : "", rounded);

      time_label = g_object_new (ST_TYPE_LABEL,
                                 "style-class", "weather-forecast-time",
                                 "text", time_str,
                                 "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                                 NULL);
      icon = g_object_new (ST_TYPE_ICON,
                           "style-class", "weather-forecast-icon",
                           "icon-name", gweather_info_get_symbolic_icon_name (fc),
                           "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                           "x-expand", TRUE,
                           NULL);
      temp = g_object_new (ST_TYPE_LABEL,
                           "style-class", "weather-forecast-temp",
                           "text", temp_text,
                           "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                           NULL);

      clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (temp)),
                                  PANGO_ELLIPSIZE_NONE);
      clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (time_label)),
                                  PANGO_ELLIPSIZE_NONE);

      clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), time_label, col, 0, 1, 1);
      clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), icon, col, 1, 1, 1);
      clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), temp, col, 2, 1, 1);
      col++;
    }
}

/* _setStatusLabel () */
static void
weather_section_set_status_label (ShellWeatherSection *self,
                                  const char          *text)
{
  ClutterLayoutManager *layout = clutter_actor_get_layout_manager (self->forecast_grid);
  ClutterActor *label = g_object_new (ST_TYPE_LABEL, "text", text, NULL);

  clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), label, 0, 0, 1, 1);
}

/* _findBestLocationName () */
static char *
weather_section_find_best_location_name (GWeatherLocation *loc)
{
  const char *loc_name = gweather_location_get_name (loc);
  g_autoptr (GWeatherLocation) world = NULL;
  g_autoptr (GWeatherLocation) city = NULL;
  double latitude, longitude;
  const char *city_name;

  if (gweather_location_get_level (loc) == GWEATHER_LOCATION_CITY ||
      !gweather_location_has_coords (loc))
    return g_strdup (loc_name);

  world = gweather_location_get_world ();
  gweather_location_get_coords (loc, &latitude, &longitude);
  city = world != NULL ? gweather_location_find_nearest_city (world, latitude, longitude) : NULL;
  if (city == NULL)
    return g_strdup (loc_name);

  city_name = gweather_location_get_name (city);

  return g_strdup (strstr (loc_name, city_name) != NULL ? city_name : loc_name);
}

/* _updateForecasts () */
static void
weather_section_update_forecasts (ShellWeatherSection *self)
{
  GWeatherInfo *info;
  g_autofree char *best_name = NULL;

  clutter_actor_destroy_all_children (self->forecast_grid);

  if (!shell_weather_client_get_has_location (self->weather_client))
    return;

  info = shell_weather_client_get_info (self->weather_client);
  if (gweather_info_get_location (info) != NULL)
    {
      best_name = weather_section_find_best_location_name ((GWeatherLocation *) gweather_info_get_location (info));
      st_label_set_text (ST_LABEL (self->title_location), best_name);
    }

  if (shell_weather_client_get_loading (self->weather_client))
    {
      weather_section_set_status_label (self, _("Loading…"));
      return;
    }

  if (gweather_info_is_valid (info))
    {
      weather_section_add_forecasts (self);
      return;
    }

  if (gweather_info_network_error (info))
    weather_section_set_status_label (self, _("Go Online for Weather Information"));
  else
    weather_section_set_status_label (self, _("Weather Information Unavailable"));
}

/* _sync () */
static void
weather_section_sync (ShellWeatherSection *self)
{
  gboolean has_location;

  if (self->title_label == NULL)
    return;

  set_visible (CLUTTER_ACTOR (self),
                             shell_weather_client_get_available (self->weather_client));

  if (!clutter_actor_is_visible (CLUTTER_ACTOR (self)))
    return;

  has_location = shell_weather_client_get_has_location (self->weather_client);

  if (has_location)
    st_label_set_text (ST_LABEL (self->title_label), _("Weather"));
  else
    st_label_set_text (ST_LABEL (self->title_label), _("Select Weather Location…"));

  if (has_location)
    st_widget_remove_style_class_name (ST_WIDGET (self->title_label), "no-location");
  else
    st_widget_add_style_class_name (ST_WIDGET (self->title_label), "no-location");

  set_visible (self->forecast_grid, has_location);
  set_visible (self->title_location, has_location);

  weather_section_update_forecasts (self);
}

static void
shell_weather_section_map (ClutterActor *actor)
{
  ShellWeatherSection *self = SHELL_WEATHER_SECTION (actor);

  shell_weather_client_update (self->weather_client);

  CLUTTER_ACTOR_CLASS (shell_weather_section_parent_class)->map (actor);
}

static void
shell_weather_section_clicked (StButton *button,
                               int       clicked_button)
{
  ShellWeatherSection *self = SHELL_WEATHER_SECTION (button);

  shell_weather_client_activate_app (self->weather_client);

  dismiss_for_launch (self->owner);
}

static void
shell_weather_section_destroy (ClutterActor *actor)
{
  ShellWeatherSection *self = SHELL_WEATHER_SECTION (actor);

  if (self->weather_client != NULL)
    g_signal_handlers_disconnect_by_data (self->weather_client, self);

  CLUTTER_ACTOR_CLASS (shell_weather_section_parent_class)->destroy (actor);
}

static void
shell_weather_section_dispose (GObject *object)
{
  ShellWeatherSection *self = SHELL_WEATHER_SECTION (object);

  if (self->weather_client != NULL)
    g_signal_handlers_disconnect_by_data (self->weather_client, self);

  g_clear_object (&self->weather_client);
  g_clear_weak_pointer (&self->owner);
  g_clear_weak_pointer (&self->title_label);
  g_clear_weak_pointer (&self->title_location);
  g_clear_weak_pointer (&self->forecast_grid);

  G_OBJECT_CLASS (shell_weather_section_parent_class)->dispose (object);
}

static void
shell_weather_section_class_init (ShellWeatherSectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);
  StButtonClass *button_class = ST_BUTTON_CLASS (klass);

  object_class->dispose = shell_weather_section_dispose;

  actor_class->map = shell_weather_section_map;
  actor_class->destroy = shell_weather_section_destroy;

  button_class->clicked = shell_weather_section_clicked;
}

static void
shell_weather_section_init (ShellWeatherSection *self)
{
  ClutterActor *box, *title_box, *title_label, *title_location, *forecast_grid;

  st_widget_set_style_class_name (ST_WIDGET (self), "weather-button");
  st_widget_set_can_focus (ST_WIDGET (self), TRUE);
  clutter_actor_set_x_expand (CLUTTER_ACTOR (self), TRUE);

  self->weather_client = g_object_new (SHELL_TYPE_WEATHER_CLIENT, NULL);

  box = new_vertical_box ("weather-box", TRUE);
  st_bin_set_child (ST_BIN (self), box);

  title_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                            "style-class", "weather-header-box",
                            NULL);
  title_label = g_object_new (ST_TYPE_LABEL,
                              "style-class", "weather-header",
                              "x-align", CLUTTER_ACTOR_ALIGN_START,
                              "x-expand", TRUE,
                              "y-align", CLUTTER_ACTOR_ALIGN_END,
                              NULL);
  g_set_weak_pointer (&self->title_label, title_label);
  clutter_actor_add_child (title_box, title_label);
  clutter_actor_add_child (box, title_box);
  st_widget_set_label_actor (ST_WIDGET (self), title_label);

  title_location = g_object_new (ST_TYPE_LABEL,
                                 "style-class", "weather-header location",
                                 "x-align", CLUTTER_ACTOR_ALIGN_END,
                                 "y-align", CLUTTER_ACTOR_ALIGN_END,
                                 NULL);
  g_set_weak_pointer (&self->title_location, title_location);
  clutter_actor_add_child (title_box, title_location);

  forecast_grid = new_grid_widget ("weather-grid", FALSE);
  g_set_weak_pointer (&self->forecast_grid, forecast_grid);
  clutter_actor_add_child (box, forecast_grid);

  g_signal_connect_object (self->weather_client, "changed",
                           G_CALLBACK (weather_section_sync),
                           self, G_CONNECT_SWAPPED);
  weather_section_sync (self);
}

static ShellWeatherSection *
weather_section_new (ShellDateMenuButton *owner)
{
  ShellWeatherSection *self = g_object_new (SHELL_TYPE_WEATHER_SECTION, NULL);

  g_set_weak_pointer (&self->owner, owner);
  return self;
}

/* ------------------------------------------------------------------------ */
/* ShellMessagesIndicator                                                    */
/* ------------------------------------------------------------------------ */

enum
{
  INDICATOR_PROP_0,

  INDICATOR_PROP_MESSAGE_TRAY,

  INDICATOR_N_PROPS
};

static GParamSpec *indicator_props[INDICATOR_N_PROPS] = { NULL, };

#define SHELL_TYPE_MESSAGES_INDICATOR (shell_messages_indicator_get_type ())
G_DECLARE_FINAL_TYPE (ShellMessagesIndicator, shell_messages_indicator,
                      SHELL, MESSAGES_INDICATOR, StIcon)

struct _ShellMessagesIndicator
{
  StIcon parent;

  ShellMessageTray *message_tray;  /* weak, construct only */
  GPtrArray *sources;              /* of ShellSource, strong */
  int count;
  GSettings *settings;
};

G_DEFINE_FINAL_TYPE (ShellMessagesIndicator, shell_messages_indicator, ST_TYPE_ICON)

/* _sync () */
static void
messages_indicator_sync (ShellMessagesIndicator *self)
{
  gboolean do_not_disturb = !g_settings_get_boolean (self->settings, "show-banners");

  set_visible (CLUTTER_ACTOR (self), !do_not_disturb && self->count > 0);
}

/* _updateCount () */
static void
messages_indicator_update_count (ShellMessagesIndicator *self)
{
  int count = 0;
  guint i;

  for (i = 0; i < self->sources->len; i++)
    count += shell_source_get_unseen_count (g_ptr_array_index (self->sources, i));

  self->count = count;
  if (self->message_tray != NULL)
    self->count -= (int) shell_message_tray_get_queue_count (self->message_tray);

  messages_indicator_sync (self);
}

/* _onSourceAdded () */
static void
messages_indicator_source_added (ShellMessageTray       *tray,
                                 ShellSource            *source,
                                 ShellMessagesIndicator *self)
{
  g_signal_connect_object (source, "notify::count",
                           G_CALLBACK (messages_indicator_update_count),
                           self, G_CONNECT_SWAPPED);
  g_ptr_array_add (self->sources, g_object_ref (source));
  messages_indicator_update_count (self);
}

/* _onSourceRemoved () */
static void
messages_indicator_source_removed (ShellMessageTray       *tray,
                                   ShellSource            *source,
                                   ShellMessagesIndicator *self)
{
  guint index;

  if (g_ptr_array_find (self->sources, source, &index))
    {
      g_signal_handlers_disconnect_by_data (source, self);
      g_ptr_array_remove_index (self->sources, index);
    }

  messages_indicator_update_count (self);
}

static void
messages_indicator_release (ShellMessagesIndicator *self)
{
  guint i;

  if (self->message_tray != NULL)
    g_signal_handlers_disconnect_by_data (self->message_tray, self);

  for (i = 0; self->sources != NULL && i < self->sources->len; i++)
    g_signal_handlers_disconnect_by_data (g_ptr_array_index (self->sources, i), self);

  if (self->settings != NULL)
    g_signal_handlers_disconnect_by_data (self->settings, self);
}

static void
shell_messages_indicator_constructed (GObject *object)
{
  ShellMessagesIndicator *self = SHELL_MESSAGES_INDICATOR (object);

  G_OBJECT_CLASS (shell_messages_indicator_parent_class)->constructed (object);

  self->settings = g_settings_new ("org.gnome.desktop.notifications");
  g_signal_connect_object (self->settings, "changed::show-banners",
                           G_CALLBACK (messages_indicator_sync),
                           self, G_CONNECT_SWAPPED);

  if (self->message_tray != NULL)
    {
      g_autoptr (GList) sources = shell_message_tray_get_sources (self->message_tray);
      GList *l;

      g_signal_connect_object (self->message_tray, "source-added",
                               G_CALLBACK (messages_indicator_source_added), self, 0);
      g_signal_connect_object (self->message_tray, "source-removed",
                               G_CALLBACK (messages_indicator_source_removed), self, 0);
      g_signal_connect_object (self->message_tray, "queue-changed",
                               G_CALLBACK (messages_indicator_update_count),
                               self, G_CONNECT_SWAPPED);

      for (l = sources; l != NULL; l = l->next)
        messages_indicator_source_added (NULL, l->data, self);
    }

  messages_indicator_sync (self);
}

static void
shell_messages_indicator_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ShellMessagesIndicator *self = SHELL_MESSAGES_INDICATOR (object);

  switch (prop_id)
    {
    case INDICATOR_PROP_MESSAGE_TRAY:
      g_value_set_object (value, self->message_tray);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_messages_indicator_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  ShellMessagesIndicator *self = SHELL_MESSAGES_INDICATOR (object);

  switch (prop_id)
    {
    case INDICATOR_PROP_MESSAGE_TRAY:
      if (g_value_get_object (value) != NULL)
        g_set_weak_pointer (&self->message_tray, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_messages_indicator_destroy (ClutterActor *actor)
{
  messages_indicator_release (SHELL_MESSAGES_INDICATOR (actor));

  CLUTTER_ACTOR_CLASS (shell_messages_indicator_parent_class)->destroy (actor);
}

static void
shell_messages_indicator_dispose (GObject *object)
{
  ShellMessagesIndicator *self = SHELL_MESSAGES_INDICATOR (object);

  messages_indicator_release (self);

  g_clear_weak_pointer (&self->message_tray);
  g_clear_pointer (&self->sources, g_ptr_array_unref);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (shell_messages_indicator_parent_class)->dispose (object);
}

static void
shell_messages_indicator_class_init (ShellMessagesIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_messages_indicator_constructed;
  object_class->dispose = shell_messages_indicator_dispose;
  object_class->get_property = shell_messages_indicator_get_property;
  object_class->set_property = shell_messages_indicator_set_property;

  actor_class->destroy = shell_messages_indicator_destroy;

  indicator_props[INDICATOR_PROP_MESSAGE_TRAY] =
    g_param_spec_object ("message-tray", NULL, NULL,
                         SHELL_TYPE_MESSAGE_TRAY,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, INDICATOR_N_PROPS, indicator_props);
}

static void
shell_messages_indicator_init (ShellMessagesIndicator *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  self->sources = g_ptr_array_new_with_free_func (g_object_unref);

  st_widget_set_style_class_name (ST_WIDGET (self), "messages-indicator");
  st_icon_set_icon_name (ST_ICON (self), "message-indicator-symbolic");
  set_visible (actor, FALSE);
  clutter_actor_set_y_expand (actor, TRUE);
  clutter_actor_set_y_align (actor, CLUTTER_ACTOR_ALIGN_CENTER);
}

/* ------------------------------------------------------------------------ */
/* ShellFreezableBinLayout                                                   */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_FREEZABLE_BIN_LAYOUT (shell_freezable_bin_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellFreezableBinLayout, shell_freezable_bin_layout,
                      SHELL, FREEZABLE_BIN_LAYOUT, ClutterBinLayout)

struct _ShellFreezableBinLayout
{
  ClutterBinLayout parent;

  gboolean frozen;
  float saved_width[2];
  float saved_height[2];
};

G_DEFINE_FINAL_TYPE (ShellFreezableBinLayout, shell_freezable_bin_layout,
                     CLUTTER_TYPE_BIN_LAYOUT)

static void
freezable_bin_layout_set_frozen (ShellFreezableBinLayout *self,
                                 gboolean                 frozen)
{
  if (self->frozen == frozen)
    return;

  self->frozen = frozen;
  if (!self->frozen)
    clutter_layout_manager_layout_changed (CLUTTER_LAYOUT_MANAGER (self));
}

static void
shell_freezable_bin_layout_get_preferred_width (ClutterLayoutManager *manager,
                                                ClutterActor         *container,
                                                float                 for_height,
                                                float                *min_width_p,
                                                float                *nat_width_p)
{
  ShellFreezableBinLayout *self = SHELL_FREEZABLE_BIN_LAYOUT (manager);

  if (!self->frozen || isnan (self->saved_width[0]) || isnan (self->saved_width[1]))
    {
      CLUTTER_LAYOUT_MANAGER_CLASS (shell_freezable_bin_layout_parent_class)->
        get_preferred_width (manager, container, for_height, min_width_p, nat_width_p);
      return;
    }

  if (min_width_p)
    *min_width_p = self->saved_width[0];
  if (nat_width_p)
    *nat_width_p = self->saved_width[1];
}

static void
shell_freezable_bin_layout_get_preferred_height (ClutterLayoutManager *manager,
                                                 ClutterActor         *container,
                                                 float                 for_width,
                                                 float                *min_height_p,
                                                 float                *nat_height_p)
{
  ShellFreezableBinLayout *self = SHELL_FREEZABLE_BIN_LAYOUT (manager);

  if (!self->frozen || isnan (self->saved_height[0]) || isnan (self->saved_height[1]))
    {
      CLUTTER_LAYOUT_MANAGER_CLASS (shell_freezable_bin_layout_parent_class)->
        get_preferred_height (manager, container, for_width, min_height_p, nat_height_p);
      return;
    }

  if (min_height_p)
    *min_height_p = self->saved_height[0];
  if (nat_height_p)
    *nat_height_p = self->saved_height[1];
}

static void
shell_freezable_bin_layout_allocate (ClutterLayoutManager   *manager,
                                     ClutterActor           *container,
                                     const ClutterActorBox  *allocation)
{
  ShellFreezableBinLayout *self = SHELL_FREEZABLE_BIN_LAYOUT (manager);
  float width, height;

  CLUTTER_LAYOUT_MANAGER_CLASS (shell_freezable_bin_layout_parent_class)->
    allocate (manager, container, allocation);

  clutter_actor_box_get_size (allocation, &width, &height);
  self->saved_width[0] = self->saved_width[1] = width;
  self->saved_height[0] = self->saved_height[1] = height;
}

static void
shell_freezable_bin_layout_class_init (ShellFreezableBinLayoutClass *klass)
{
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  layout_class->get_preferred_width = shell_freezable_bin_layout_get_preferred_width;
  layout_class->get_preferred_height = shell_freezable_bin_layout_get_preferred_height;
  layout_class->allocate = shell_freezable_bin_layout_allocate;
}

static void
shell_freezable_bin_layout_init (ShellFreezableBinLayout *self)
{
  self->frozen = FALSE;
  self->saved_width[0] = self->saved_width[1] = NAN;
  self->saved_height[0] = self->saved_height[1] = NAN;
}

/* ------------------------------------------------------------------------ */
/* ShellCalendarColumnLayout                                                 */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_CALENDAR_COLUMN_LAYOUT (shell_calendar_column_layout_get_type ())
G_DECLARE_FINAL_TYPE (ShellCalendarColumnLayout, shell_calendar_column_layout,
                      SHELL, CALENDAR_COLUMN_LAYOUT, ClutterBoxLayout)

struct _ShellCalendarColumnLayout
{
  ClutterBoxLayout parent;

  ClutterActor *col_actors[2];  /* weak */
};

G_DEFINE_FINAL_TYPE (ShellCalendarColumnLayout, shell_calendar_column_layout,
                     CLUTTER_TYPE_BOX_LAYOUT)

static void
shell_calendar_column_layout_get_preferred_width (ClutterLayoutManager *manager,
                                                  ClutterActor         *container,
                                                  float                 for_height,
                                                  float                *min_width_p,
                                                  float                *nat_width_p)
{
  ShellCalendarColumnLayout *self = SHELL_CALENDAR_COLUMN_LAYOUT (manager);
  float min_acc = 0, nat_acc = 0;
  gboolean found = FALSE;
  int i;

  for (i = 0; i < 2; i++)
    {
      ClutterActor *child = self->col_actors[i];
      float min, nat;

      if (child == NULL || clutter_actor_get_parent (child) != container)
        continue;

      clutter_actor_get_preferred_width (child, for_height, &min, &nat);
      min_acc = MAX (min_acc, min);
      nat_acc = MAX (nat_acc, nat);
      found = TRUE;
    }

  if (!found)
    {
      CLUTTER_LAYOUT_MANAGER_CLASS (shell_calendar_column_layout_parent_class)->
        get_preferred_width (manager, container, for_height, min_width_p, nat_width_p);
      return;
    }

  if (min_width_p)
    *min_width_p = min_acc;
  if (nat_width_p)
    *nat_width_p = nat_acc;
}

static void
shell_calendar_column_layout_finalize (GObject *object)
{
  ShellCalendarColumnLayout *self = SHELL_CALENDAR_COLUMN_LAYOUT (object);

  g_clear_weak_pointer (&self->col_actors[0]);
  g_clear_weak_pointer (&self->col_actors[1]);

  G_OBJECT_CLASS (shell_calendar_column_layout_parent_class)->finalize (object);
}

static void
shell_calendar_column_layout_class_init (ShellCalendarColumnLayoutClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterLayoutManagerClass *layout_class = CLUTTER_LAYOUT_MANAGER_CLASS (klass);

  object_class->finalize = shell_calendar_column_layout_finalize;

  layout_class->get_preferred_width = shell_calendar_column_layout_get_preferred_width;
}

static void
shell_calendar_column_layout_init (ShellCalendarColumnLayout *self)
{
  clutter_box_layout_set_orientation (CLUTTER_BOX_LAYOUT (self),
                                      CLUTTER_ORIENTATION_VERTICAL);
}

static ClutterLayoutManager *
calendar_column_layout_new (ClutterActor *first,
                            ClutterActor *second)
{
  ShellCalendarColumnLayout *self = g_object_new (SHELL_TYPE_CALENDAR_COLUMN_LAYOUT, NULL);

  g_set_weak_pointer (&self->col_actors[0], first);
  g_set_weak_pointer (&self->col_actors[1], second);

  return CLUTTER_LAYOUT_MANAGER (self);
}

/* ------------------------------------------------------------------------ */
/* ShellDateMenuButton                                                       */
/* ------------------------------------------------------------------------ */

struct _ShellDateMenuButton
{
  ShellPanelMenuButton parent;

  ClutterActor *clock_display;               /* weak */
  ClutterActor *indicator;                   /* weak */
  ShellFreezableBinLayout *bin_layout;       /* weak */
  ShellCalendar *calendar;                   /* weak */
  ShellTodayButton *date;                    /* weak */
  ShellCalendarMessageList *message_list;    /* weak */
  ClutterActor *displays_section;            /* weak */
  ShellEventsSection *events_item;           /* weak */
  ShellWorldClocksSection *clocks_item;      /* weak */
  ShellWeatherSection *weather_item;         /* weak */

  GnomeWallClock *clock;
  ShellCalendarEventSource *event_source;
  gboolean shutting_down;
};

G_DEFINE_FINAL_TYPE (ShellDateMenuButton, shell_date_menu_button,
                     SHELL_TYPE_PANEL_MENU_BUTTON)

static void
date_menu_calendar_selected_date_changed_cb (ShellCalendar       *calendar,
                                             GDateTime           *datetime,
                                             ShellDateMenuButton *self)
{
  gboolean today = is_today_secs (g_date_time_to_unix (datetime));

  if (self->bin_layout != NULL)
    freezable_bin_layout_set_frozen (self->bin_layout, !today);
  if (self->events_item != NULL)
    events_section_set_date (self->events_item, datetime);
}

static void
date_menu_open_state_changed_cb (GObject             *menu,
                                 gboolean             is_open,
                                 ShellDateMenuButton *self)
{
  /* Whenever the menu is opened, select today */
  if (is_open)
    {
      g_autoptr (GDateTime) now = date_from_secs (now_secs ());

      if (self->calendar != NULL)
        shell_calendar_set_date (self->calendar, now);
      if (self->date != NULL)
        today_button_set_date (self->date, g_date_time_to_unix (now));
      if (self->events_item != NULL)
        events_section_set_date (self->events_item, now);
    }
}

/* _setEventSource () */
static void
date_menu_set_event_source (ShellDateMenuButton      *self,
                            ShellCalendarEventSource *event_source)
{
  g_autoptr (ShellCalendarEventSource) previous = g_steal_pointer (&self->event_source);

  if (previous != NULL)
    shell_calendar_event_source_destroy (previous);

  self->event_source = g_object_ref (event_source);

  if (self->calendar != NULL)
    shell_calendar_set_event_source (self->calendar, event_source);
  if (self->events_item != NULL)
    events_section_set_event_source (self->events_item, event_source);
}

/* _updateTimeZone () */
static void
date_menu_update_time_zone (ShellDateMenuButton *self)
{
  /* The cached local time zone must be dropped before the calendar is rebuilt */
  shell_date_clear_cached_local_time_zone ();

  if (self->calendar != NULL)
    shell_calendar_update_time_zone (self->calendar);
}

/* _sessionUpdated () */
static void
date_menu_session_updated (ShellDateMenuButton *self)
{
  ShellSessionMode *session_mode = shell_main_get_session_mode (shell_main_get_default ());
  g_autoptr (ShellCalendarEventSource) event_source = NULL;
  gboolean show_events = FALSE;
  gboolean allow_settings = FALSE;
  gboolean has_notifications = FALSE;

  if (self->shutting_down)
    return;

  if (session_mode != NULL)
    g_object_get (session_mode,
                  "show-calendar-events", &show_events,
                  "allow-settings", &allow_settings,
                  "has-notifications", &has_notifications,
                  NULL);

  if (show_events)
    event_source = SHELL_CALENDAR_EVENT_SOURCE (shell_calendar_dbus_event_source_new ());
  else
    event_source = SHELL_CALENDAR_EVENT_SOURCE (shell_calendar_empty_event_source_new ());

  date_menu_set_event_source (self, event_source);

  /* Displays are not actually expected to launch Settings when activated
   * but the corresponding app (clocks, weather); however we can consider
   * that display-specific settings, so re-use "allowSettings" here ... */
  if (self->displays_section != NULL)
    set_visible (self->displays_section, allow_settings);

  if (self->message_list != NULL)
    set_visible (CLUTTER_ACTOR (self->message_list),
                               has_notifications &&
                               (session_mode == NULL ||
                                !shell_session_mode_is_greeter (session_mode)));
}

static void
date_menu_shutdown_cb (ShellGlobal         *global,
                       ShellDateMenuButton *self)
{
  /* Pending calls of the event source are cancelled, and it does not
   * report anything once the shell is going away */
  self->shutting_down = TRUE;

  if (self->event_source != NULL)
    shell_calendar_event_source_destroy (self->event_source);
}

static GObject *
shell_date_menu_button_constructor (GType                  type,
                                    guint                  n_construct_properties,
                                    GObjectConstructParam *construct_properties)
{
  guint i;

  /* `super._init ({menu_alignment: 0.5})`: JS creates the button with no
   * property, so the alignment is forced here, before the menu is made */
  for (i = 0; i < n_construct_properties; i++)
    {
      if (g_strcmp0 (g_param_spec_get_name (construct_properties[i].pspec),
                     "menu-alignment") == 0)
        g_value_set_double (construct_properties[i].value, 0.5);
    }

  return G_OBJECT_CLASS (shell_date_menu_button_parent_class)->constructor (type,
                                                                          n_construct_properties,
                                                                          construct_properties);
}

static void
shell_date_menu_button_constructed (GObject *object)
{
  ShellDateMenuButton *self = SHELL_DATE_MENU_BUTTON (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ShellMain *shell_main = shell_main_get_default ();
  ShellMessageTray *message_tray = shell_main_get_message_tray (shell_main);
  ShellSessionMode *session_mode = shell_main_get_session_mode (shell_main);
  ShellPopupMenuBase *menu;
  ClutterActor *clock_display, *indicator, *indicator_pad, *box;
  ClutterActor *bin, *hbox, *vbox, *displays_box, *displays_section;
  ClutterLayoutManager *bin_layout, *column_layout;
  ShellCalendar *calendar;
  ShellTodayButton *date;

  G_OBJECT_CLASS (shell_date_menu_button_parent_class)->constructed (object);

  menu = SHELL_POPUP_MENU_BASE (shell_panel_menu_button_get_menu (SHELL_PANEL_MENU_BUTTON (self)));

  clock_display = g_object_new (ST_TYPE_LABEL,
                                "style-class", "clock",
                                NULL);
  clutter_actor_set_y_align (CLUTTER_ACTOR (st_label_get_clutter_text (ST_LABEL (clock_display))),
                             CLUTTER_ACTOR_ALIGN_CENTER);
  clutter_text_set_ellipsize (st_label_get_clutter_text (ST_LABEL (clock_display)),
                              PANGO_ELLIPSIZE_NONE);
  g_set_weak_pointer (&self->clock_display, clock_display);

  indicator = g_object_new (SHELL_TYPE_MESSAGES_INDICATOR,
                            "message-tray", message_tray,
                            NULL);
  g_set_weak_pointer (&self->indicator, indicator);

  indicator_pad = g_object_new (ST_TYPE_WIDGET, NULL);
  g_object_bind_property (indicator, "visible",
                          indicator_pad, "visible",
                          G_BINDING_SYNC_CREATE);
  clutter_actor_add_constraint (indicator_pad,
                                clutter_bind_constraint_new (indicator,
                                                             CLUTTER_BIND_SIZE, 0));

  box = g_object_new (ST_TYPE_BOX_LAYOUT,
                      "style-class", "clock-display-box",
                      NULL);
  clutter_actor_add_child (box, indicator_pad);
  clutter_actor_add_child (box, clock_display);
  clutter_actor_add_child (box, indicator);

  st_widget_set_label_actor (ST_WIDGET (self), clock_display);
  clutter_actor_add_child (actor, box);
  st_widget_add_style_class_name (ST_WIDGET (self), "clock-display");

  bin_layout = g_object_new (SHELL_TYPE_FREEZABLE_BIN_LAYOUT, NULL);
  g_set_weak_pointer (&self->bin_layout, SHELL_FREEZABLE_BIN_LAYOUT (bin_layout));
  bin = g_object_new (ST_TYPE_WIDGET, NULL);
  clutter_actor_set_layout_manager (bin, bin_layout);
  clutter_actor_add_child (CLUTTER_ACTOR (shell_popup_menu_base_get_box (menu)), bin);
  st_widget_add_style_class_name (ST_WIDGET (shell_popup_menu_base_get_box (menu)),
                                  "datemenu-popover");
  /* Default vertical wraparound keynav behavior is unwanted here */
  st_widget_set_keynav_flags (ST_WIDGET (shell_popup_menu_base_get_actor (menu)),
                              ST_KEYNAV_FLAG_NONE);

  hbox = g_object_new (ST_TYPE_BOX_LAYOUT,
                       "name", "calendarArea",
                       NULL);
  clutter_actor_add_child (bin, hbox);

  calendar = shell_calendar_new ();
  g_set_weak_pointer (&self->calendar, calendar);
  g_signal_connect_object (calendar, "selected-date-changed",
                           G_CALLBACK (date_menu_calendar_selected_date_changed_cb),
                           self, 0);
  date = today_button_new (calendar);
  g_set_weak_pointer (&self->date, date);

  g_signal_connect_object (menu, "open-state-changed",
                           G_CALLBACK (date_menu_open_state_changed_cb), self, 0);

  /* Fill up the first column */
  if (message_tray != NULL)
    {
      ShellCalendarMessageList *message_list = shell_calendar_message_list_new (message_tray);

      g_set_weak_pointer (&self->message_list, message_list);
      shell_calendar_message_list_set_capture_container (message_list,
                                                         shell_popup_menu_base_get_actor (menu));
      clutter_actor_add_child (hbox, CLUTTER_ACTOR (message_list));
    }

  /* Fill up the second column */
  column_layout = calendar_column_layout_new (CLUTTER_ACTOR (calendar),
                                              CLUTTER_ACTOR (date));
  vbox = g_object_new (ST_TYPE_WIDGET,
                       "style-class", "datemenu-calendar-column",
                       NULL);
  clutter_actor_set_layout_manager (vbox, column_layout);
  hookup_style (ST_WIDGET (vbox), column_layout, FALSE);
  clutter_actor_add_child (hbox, vbox);

  clutter_actor_add_child (vbox, CLUTTER_ACTOR (date));
  clutter_actor_add_child (vbox, CLUTTER_ACTOR (calendar));

  displays_box = new_vertical_box ("datemenu-displays-box", TRUE);

  displays_section = g_object_new (ST_TYPE_SCROLL_VIEW,
                                   "style-class", "datemenu-displays-section vfade",
                                   "x-expand", TRUE,
                                   "overlay-scrollbars", TRUE,
                                   "vscrollbar-policy", ST_POLICY_EXTERNAL,
                                   NULL);
  st_scroll_view_set_child (ST_SCROLL_VIEW (displays_section), ST_SCROLLABLE (displays_box));
  g_set_weak_pointer (&self->displays_section, displays_section);
  clutter_actor_add_child (vbox, displays_section);

  self->events_item = events_section_new (self);
  g_object_add_weak_pointer (G_OBJECT (self->events_item), (gpointer *) &self->events_item);
  clutter_actor_add_child (displays_box, CLUTTER_ACTOR (self->events_item));

  self->clocks_item = world_clocks_section_new (self);
  g_object_add_weak_pointer (G_OBJECT (self->clocks_item), (gpointer *) &self->clocks_item);
  clutter_actor_add_child (displays_box, CLUTTER_ACTOR (self->clocks_item));

  self->weather_item = weather_section_new (self);
  g_object_add_weak_pointer (G_OBJECT (self->weather_item), (gpointer *) &self->weather_item);
  clutter_actor_add_child (displays_box, CLUTTER_ACTOR (self->weather_item));

  /* Done with hbox for calendar and event list */

  self->clock = gnome_wall_clock_new ();
  g_object_bind_property (self->clock, "clock", clock_display, "text",
                          G_BINDING_SYNC_CREATE);
  g_signal_connect_object (self->clock, "notify::timezone",
                           G_CALLBACK (date_menu_update_time_zone),
                           self, G_CONNECT_SWAPPED);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (date_menu_shutdown_cb), self, 0);

  if (session_mode != NULL)
    g_signal_connect_object (session_mode, "updated",
                             G_CALLBACK (date_menu_session_updated),
                             self, G_CONNECT_SWAPPED);
  date_menu_session_updated (self);
}

static void
shell_date_menu_button_dispose (GObject *object)
{
  ShellDateMenuButton *self = SHELL_DATE_MENU_BUTTON (object);

  self->shutting_down = TRUE;

  if (self->event_source != NULL)
    shell_calendar_event_source_destroy (self->event_source);
  g_clear_object (&self->event_source);
  g_clear_object (&self->clock);

  g_clear_weak_pointer (&self->clock_display);
  g_clear_weak_pointer (&self->indicator);
  g_clear_weak_pointer (&self->bin_layout);
  g_clear_weak_pointer (&self->calendar);
  g_clear_weak_pointer (&self->date);
  g_clear_weak_pointer (&self->message_list);
  g_clear_weak_pointer (&self->displays_section);
  g_clear_weak_pointer (&self->events_item);
  g_clear_weak_pointer (&self->clocks_item);
  g_clear_weak_pointer (&self->weather_item);

  G_OBJECT_CLASS (shell_date_menu_button_parent_class)->dispose (object);
}

static void
shell_date_menu_button_class_init (ShellDateMenuButtonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructor = shell_date_menu_button_constructor;
  object_class->constructed = shell_date_menu_button_constructed;
  object_class->dispose = shell_date_menu_button_dispose;

  /**
   * ShellDateMenuButton::overview-hide-requested:
   * @self: the #ShellDateMenuButton
   *
   * Emitted before the menu closes for the application that a section of the
   * menu launched (clocks, weather or calendar), so that the owner of the
   * button hides the overview (`Main.overview.hide ()`).
   */
  signals[OVERVIEW_HIDE_REQUESTED] =
    g_signal_new ("overview-hide-requested",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_date_menu_button_init (ShellDateMenuButton *self)
{
}

/**
 * shell_date_menu_button_new:
 *
 * Creates the clock of the top bar with its menu (`new DateMenuButton ()`).
 * The message tray must have been given to #ShellMain:message-tray.
 *
 * Returns: (transfer floating): a new #ShellDateMenuButton
 */
ShellDateMenuButton *
shell_date_menu_button_new (void)
{
  return g_object_new (SHELL_TYPE_DATE_MENU_BUTTON, NULL);
}

/**
 * shell_date_menu_button_get_message_list:
 * @self: a #ShellDateMenuButton
 *
 * Gets the list of notifications of the menu (`_messageList`).
 *
 * Returns: (transfer none) (nullable): the list, %NULL if the shell has no
 *   message tray
 */
ShellCalendarMessageList *
shell_date_menu_button_get_message_list (ShellDateMenuButton *self)
{
  g_return_val_if_fail (SHELL_IS_DATE_MENU_BUTTON (self), NULL);

  return self->message_list;
}

/**
 * shell_date_menu_button_get_message_view:
 * @self: a #ShellDateMenuButton
 *
 * Gets the view of the messages in the menu, to which the media messages of
 * the MPRIS players are added.
 *
 * Returns: (transfer none) (nullable): the view
 */
ShellMessageView *
shell_date_menu_button_get_message_view (ShellDateMenuButton *self)
{
  g_return_val_if_fail (SHELL_IS_DATE_MENU_BUTTON (self), NULL);

  if (self->message_list == NULL)
    return NULL;

  return shell_calendar_message_list_get_message_view (self->message_list);
}
