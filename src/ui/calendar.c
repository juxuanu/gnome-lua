/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * calendar.c: The month grid of the date menu, the sources of the events it
 * marks, and the list of messages next to it. Port of js/ui/calendar.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - JS `Date` objects are seconds since the epoch (gint64) when they are
 *    stored, and #GDateTime in the cached local time zone of
 *    shell_date_time_new_from_unix_msecs () when their fields are needed, so
 *    that the grid and the labels formatted by the date utilities agree after
 *    a time zone change (the caller clears the cache before
 *    shell_calendar_update_time_zone ()). The milliseconds of a JS `Date` are
 *    dropped.
 *  - The calendar server proxy is a gdbus-codegen proxy. When the proxy cannot
 *    be created because the call timed out the source watches the bus name
 *    and creates the proxy when the service appears (the JS kept the
 *    uninitialised proxy and waited for `notify::g-name-owner`).
 *  - The source cancels its pending calls and drops its proxy on
 *    ShellGlobal::shutdown and when it is destroyed.
 *  - The message list creates its own #ShellMprisSource and adds a
 *    #ShellMediaMessage to the view for each player (`_setupMpris ()`), as the
 *    JS list did before the media messages moved to the owner.
 *  - `Placeholder` is a plain vertical box.
 *  - The events of the cache are sorted by time and then by the order in which
 *    they were first received, as the stable sort of the JS `Map` iteration
 *    did.
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <gdesktop-enums.h>

#include "misc/date-utils.h"
#include "shell-global.h"
#include "shell-util.h"
#include "ui/calendar.h"
#include "ui/environment.h"
#include "ui/mpris.h"

#include "calendar-server-dbus.h"

#define SHOW_WEEKDATE_KEY "show-weekdate"
#define WEEK_START_DAY_KEY "week-start-day"

#define CALENDAR_SERVER_BUS_NAME "org.gnome.Shell.CalendarServer"
#define CALENDAR_SERVER_OBJECT_PATH "/org/gnome/Shell/CalendarServer"

/* The JS `NC_ ()` joined the context and the message for
 * Shell.util_translate_time_string (); xgettext still sees the macro. */
#undef NC_
#define NC_(context, str) context "\004" str

/* ------------------------------------------------------------------------ */
/* Dates                                                                     */
/* ------------------------------------------------------------------------ */

static gint64
now_secs (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

/* new Date (secs * 1000) in the cached local time zone */
static GDateTime *
date_from_secs (gint64 secs)
{
  GDateTime *date = shell_date_time_new_from_unix_msecs (secs * 1000);

  if (date == NULL)
    date = shell_date_time_new_from_unix_msecs (0);

  return date;
}

/* Date.getDay (): 0 is Sunday */
static int
get_js_day (GDateTime *date)
{
  return g_date_time_get_day_of_week (date) % 7;
}

static gboolean
same_year (GDateTime *a,
           GDateTime *b)
{
  return g_date_time_get_year (a) == g_date_time_get_year (b);
}

static gboolean
same_month (GDateTime *a,
            GDateTime *b)
{
  return same_year (a, b) &&
         g_date_time_get_month (a) == g_date_time_get_month (b);
}

static gboolean
same_day (GDateTime *a,
          GDateTime *b)
{
  return same_month (a, b) &&
         g_date_time_get_day_of_month (a) == g_date_time_get_day_of_month (b);
}

static GDateTime *
get_beginning_of_day (GDateTime *date)
{
  return g_date_time_new (g_date_time_get_timezone (date),
                          g_date_time_get_year (date),
                          g_date_time_get_month (date),
                          g_date_time_get_day_of_month (date),
                          0, 0, 0);
}

/* The month after or before @date, with the day clamped to the length of the
 * month. A clamped date is at the beginning of the day, as in the JS. */
static GDateTime *
shift_month (GDateTime *date,
             int        delta)
{
  int year = g_date_time_get_year (date);
  int month = g_date_time_get_month (date) - 1 + delta;
  int day = g_date_time_get_day_of_month (date);
  int days_in_month;

  if (month < 0)
    {
      month = 11;
      year--;
    }
  else if (month > 11)
    {
      month = 0;
      year++;
    }

  days_in_month = g_date_get_days_in_month (month + 1, year);
  if (day > days_in_month)
    return g_date_time_new (g_date_time_get_timezone (date),
                            year, month + 1, days_in_month, 0, 0, 0);

  return g_date_time_new (g_date_time_get_timezone (date),
                          year, month + 1, day,
                          g_date_time_get_hour (date),
                          g_date_time_get_minute (date),
                          g_date_time_get_seconds (date));
}

/* _isWorkDay () */
static gboolean
is_work_day (GDateTime *date)
{
  /* Translators: Enter 0-6 (Sunday-Saturday) for non-work days. Examples: "0" (Sunday) "6" (Saturday) "06" (Sunday and Saturday). */
  const char *days = C_("calendar-no-work", "06");
  char day[2] = { '0' + get_js_day (date), 0 };

  return strstr (days, day) == NULL;
}

/* _getCalendarDayAbbreviation () */
static char *
get_calendar_day_abbreviation (int day_number)
{
  static const char * const abbreviations[] = {
    /* Translators: Calendar grid abbreviation for Sunday.
     *
     * NOTE: These grid abbreviations are always shown together
     * and in order, e.g. "S M T W T F S".
     */
    NC_ ("grid sunday", "S"),
    /* Translators: Calendar grid abbreviation for Monday */
    NC_ ("grid monday", "M"),
    /* Translators: Calendar grid abbreviation for Tuesday */
    NC_ ("grid tuesday", "T"),
    /* Translators: Calendar grid abbreviation for Wednesday */
    NC_ ("grid wednesday", "W"),
    /* Translators: Calendar grid abbreviation for Thursday */
    NC_ ("grid thursday", "T"),
    /* Translators: Calendar grid abbreviation for Friday */
    NC_ ("grid friday", "F"),
    /* Translators: Calendar grid abbreviation for Saturday */
    NC_ ("grid saturday", "S"),
  };

  return g_strdup (shell_util_translate_time_string (abbreviations[day_number % 7]));
}

/* ------------------------------------------------------------------------ */
/* ShellCalendarEvent                                                        */
/* ------------------------------------------------------------------------ */

struct _ShellCalendarEvent
{
  grefcount ref_count;

  char *id;
  gint64 start;
  gint64 end;
  char *summary;

  guint64 seq; /* order of arrival in the cache of a source */
};

G_DEFINE_BOXED_TYPE (ShellCalendarEvent, shell_calendar_event,
                     shell_calendar_event_ref, shell_calendar_event_unref)

/**
 * shell_calendar_event_new:
 * @id: the unique identifier of the event
 * @start: the beginning of the event in seconds since the epoch
 * @end: the end of the event in seconds since the epoch
 * @summary: the title of the event
 *
 * Creates an event (`new CalendarEvent (id, date, end, summary)`).
 *
 * Returns: (transfer full): a new #ShellCalendarEvent
 */
ShellCalendarEvent *
shell_calendar_event_new (const char *id,
                          gint64      start,
                          gint64      end,
                          const char *summary)
{
  ShellCalendarEvent *self;

  g_return_val_if_fail (id != NULL, NULL);
  g_return_val_if_fail (summary != NULL, NULL);

  self = g_new0 (ShellCalendarEvent, 1);
  g_ref_count_init (&self->ref_count);
  self->id = g_strdup (id);
  self->start = start;
  self->end = end;
  self->summary = g_strdup (summary);

  return self;
}

/**
 * shell_calendar_event_ref:
 * @self: a #ShellCalendarEvent
 *
 * Returns: (transfer full): @self
 */
ShellCalendarEvent *
shell_calendar_event_ref (ShellCalendarEvent *self)
{
  g_return_val_if_fail (self != NULL, NULL);

  g_ref_count_inc (&self->ref_count);

  return self;
}

/**
 * shell_calendar_event_unref:
 * @self: (transfer full): a #ShellCalendarEvent
 *
 * Drops a reference on @self.
 */
void
shell_calendar_event_unref (ShellCalendarEvent *self)
{
  g_return_if_fail (self != NULL);

  if (!g_ref_count_dec (&self->ref_count))
    return;

  g_free (self->id);
  g_free (self->summary);
  g_free (self);
}

/**
 * shell_calendar_event_get_id:
 * @self: a #ShellCalendarEvent
 *
 * Returns: the identifier of the event (`id`)
 */
const char *
shell_calendar_event_get_id (ShellCalendarEvent *self)
{
  g_return_val_if_fail (self != NULL, NULL);

  return self->id;
}

/**
 * shell_calendar_event_get_start:
 * @self: a #ShellCalendarEvent
 *
 * Returns: the beginning of the event in seconds since the epoch (`date`)
 */
gint64
shell_calendar_event_get_start (ShellCalendarEvent *self)
{
  g_return_val_if_fail (self != NULL, 0);

  return self->start;
}

/**
 * shell_calendar_event_get_end:
 * @self: a #ShellCalendarEvent
 *
 * Returns: the end of the event in seconds since the epoch (`end`)
 */
gint64
shell_calendar_event_get_end (ShellCalendarEvent *self)
{
  g_return_val_if_fail (self != NULL, 0);

  return self->end;
}

/**
 * shell_calendar_event_get_summary:
 * @self: a #ShellCalendarEvent
 *
 * Returns: the title of the event (`summary`)
 */
const char *
shell_calendar_event_get_summary (ShellCalendarEvent *self)
{
  g_return_val_if_fail (self != NULL, NULL);

  return self->summary;
}

/* ------------------------------------------------------------------------ */
/* ShellCalendarEventSource                                                  */
/* ------------------------------------------------------------------------ */

enum
{
  SOURCE_PROP_0,

  SOURCE_PROP_HAS_CALENDARS,
  SOURCE_PROP_IS_LOADING,

  SOURCE_N_PROPS
};

enum
{
  SOURCE_CHANGED,

  SOURCE_N_SIGNALS
};

static GParamSpec *source_props[SOURCE_N_PROPS] = { NULL, };
static guint source_signals[SOURCE_N_SIGNALS] = { 0, };

G_DEFINE_ABSTRACT_TYPE (ShellCalendarEventSource, shell_calendar_event_source,
                        G_TYPE_OBJECT)

/**
 * shell_calendar_event_source_get_is_loading:
 * @self: a #ShellCalendarEventSource
 *
 * Returns: whether the source is loading events (`isLoading`)
 */
gboolean
shell_calendar_event_source_get_is_loading (ShellCalendarEventSource *self)
{
  ShellCalendarEventSourceClass *klass;

  g_return_val_if_fail (SHELL_IS_CALENDAR_EVENT_SOURCE (self), FALSE);

  klass = SHELL_CALENDAR_EVENT_SOURCE_GET_CLASS (self);
  return klass->get_is_loading != NULL ? klass->get_is_loading (self) : FALSE;
}

/**
 * shell_calendar_event_source_get_has_calendars:
 * @self: a #ShellCalendarEventSource
 *
 * Returns: whether there is a calendar to show events from (`hasCalendars`)
 */
gboolean
shell_calendar_event_source_get_has_calendars (ShellCalendarEventSource *self)
{
  ShellCalendarEventSourceClass *klass;

  g_return_val_if_fail (SHELL_IS_CALENDAR_EVENT_SOURCE (self), FALSE);

  klass = SHELL_CALENDAR_EVENT_SOURCE_GET_CLASS (self);
  return klass->get_has_calendars != NULL ? klass->get_has_calendars (self) : FALSE;
}

/**
 * shell_calendar_event_source_destroy:
 * @self: a #ShellCalendarEventSource
 *
 * Releases what the source holds on the outside world (`destroy ()`). The
 * source reports no calendar and no event afterwards.
 */
void
shell_calendar_event_source_destroy (ShellCalendarEventSource *self)
{
  ShellCalendarEventSourceClass *klass;

  g_return_if_fail (SHELL_IS_CALENDAR_EVENT_SOURCE (self));

  klass = SHELL_CALENDAR_EVENT_SOURCE_GET_CLASS (self);
  if (klass->destroy != NULL)
    klass->destroy (self);
}

/**
 * shell_calendar_event_source_request_range:
 * @self: a #ShellCalendarEventSource
 * @begin: the beginning of the interval the caller is interested in, in
 *   seconds since the epoch
 * @end: the end of the interval, in seconds since the epoch
 *
 * Tells the source that only the events of the interval are of interest
 * (`requestRange ()`).
 */
void
shell_calendar_event_source_request_range (ShellCalendarEventSource *self,
                                           gint64                    begin,
                                           gint64                    end)
{
  ShellCalendarEventSourceClass *klass;

  g_return_if_fail (SHELL_IS_CALENDAR_EVENT_SOURCE (self));

  klass = SHELL_CALENDAR_EVENT_SOURCE_GET_CLASS (self);
  if (klass->request_range != NULL)
    klass->request_range (self, begin, end);
}

/**
 * shell_calendar_event_source_get_events:
 * @self: a #ShellCalendarEventSource
 * @begin: the beginning of the interval, in seconds since the epoch
 * @end: the end of the interval, in seconds since the epoch
 *
 * Gets the events that overlap the interval, sorted by time (`getEvents ()`).
 *
 * Returns: (transfer full) (element-type ShellCalendarEvent): the events
 */
GPtrArray *
shell_calendar_event_source_get_events (ShellCalendarEventSource *self,
                                        gint64                    begin,
                                        gint64                    end)
{
  ShellCalendarEventSourceClass *klass;
  GPtrArray *events = NULL;

  g_return_val_if_fail (SHELL_IS_CALENDAR_EVENT_SOURCE (self), NULL);

  klass = SHELL_CALENDAR_EVENT_SOURCE_GET_CLASS (self);
  if (klass->get_events != NULL)
    events = klass->get_events (self, begin, end);

  if (events == NULL)
    events = g_ptr_array_new_with_free_func ((GDestroyNotify) shell_calendar_event_unref);

  return events;
}

/**
 * shell_calendar_event_source_has_events:
 * @self: a #ShellCalendarEventSource
 * @day: a day, in the local time zone
 *
 * Returns: whether there is an event on @day (`hasEvents ()`)
 */
gboolean
shell_calendar_event_source_has_events (ShellCalendarEventSource *self,
                                        GDateTime                *day)
{
  ShellCalendarEventSourceClass *klass;

  g_return_val_if_fail (SHELL_IS_CALENDAR_EVENT_SOURCE (self), FALSE);
  g_return_val_if_fail (day != NULL, FALSE);

  klass = SHELL_CALENDAR_EVENT_SOURCE_GET_CLASS (self);
  return klass->has_events != NULL ? klass->has_events (self, day) : FALSE;
}

static void
shell_calendar_event_source_get_property (GObject    *object,
                                          guint       prop_id,
                                          GValue     *value,
                                          GParamSpec *pspec)
{
  ShellCalendarEventSource *self = SHELL_CALENDAR_EVENT_SOURCE (object);

  switch (prop_id)
    {
    case SOURCE_PROP_HAS_CALENDARS:
      g_value_set_boolean (value, shell_calendar_event_source_get_has_calendars (self));
      break;

    case SOURCE_PROP_IS_LOADING:
      g_value_set_boolean (value, shell_calendar_event_source_get_is_loading (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_calendar_event_source_class_init (ShellCalendarEventSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_calendar_event_source_get_property;

  /**
   * ShellCalendarEventSource:has-calendars:
   *
   * Whether there is a calendar to show events from (`hasCalendars`).
   */
  source_props[SOURCE_PROP_HAS_CALENDARS] =
    g_param_spec_boolean ("has-calendars", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  /**
   * ShellCalendarEventSource:is-loading:
   *
   * Whether the source is loading events (`isLoading`).
   */
  source_props[SOURCE_PROP_IS_LOADING] =
    g_param_spec_boolean ("is-loading", NULL, NULL,
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, SOURCE_N_PROPS, source_props);

  /**
   * ShellCalendarEventSource::changed:
   * @self: the source
   *
   * Emitted when the events of the source changed.
   */
  source_signals[SOURCE_CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_calendar_event_source_init (ShellCalendarEventSource *self)
{
}

static void
source_emit_changed (ShellCalendarEventSource *self)
{
  g_signal_emit (self, source_signals[SOURCE_CHANGED], 0);
}

/* ------------------------------------------------------------------------ */
/* ShellCalendarEmptyEventSource                                             */
/* ------------------------------------------------------------------------ */

struct _ShellCalendarEmptyEventSource
{
  ShellCalendarEventSource parent;
};

G_DEFINE_FINAL_TYPE (ShellCalendarEmptyEventSource, shell_calendar_empty_event_source,
                     SHELL_TYPE_CALENDAR_EVENT_SOURCE)

static void
shell_calendar_empty_event_source_class_init (ShellCalendarEmptyEventSourceClass *klass)
{
  /* The virtual functions of the base class report nothing */
}

static void
shell_calendar_empty_event_source_init (ShellCalendarEmptyEventSource *self)
{
}

/**
 * shell_calendar_empty_event_source_new:
 *
 * Creates a source with no calendar and no event (`new EmptyEventSource ()`).
 *
 * Returns: (transfer full): a new #ShellCalendarEmptyEventSource
 */
ShellCalendarEmptyEventSource *
shell_calendar_empty_event_source_new (void)
{
  return g_object_new (SHELL_TYPE_CALENDAR_EMPTY_EVENT_SOURCE, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellCalendarDBusEventSource                                              */
/* ------------------------------------------------------------------------ */

struct _ShellCalendarDBusEventSource
{
  ShellCalendarEventSource parent;

  ShellCalendarServer *proxy;
  GCancellable *cancellable;
  guint watch_id;

  GHashTable *events;   /* char * id -> ShellCalendarEvent * */
  guint64 next_seq;

  gboolean initialized;
  gboolean destroyed;

  gboolean has_last_request;
  gint64 last_request_begin;
  gint64 last_request_end;
  gboolean has_cur_request;
  gint64 cur_request_begin;
  gint64 cur_request_end;
};

G_DEFINE_FINAL_TYPE (ShellCalendarDBusEventSource, shell_calendar_dbus_event_source,
                     SHELL_TYPE_CALENDAR_EVENT_SOURCE)

static void dbus_source_create_proxy (ShellCalendarDBusEventSource *self);

/*
 * checks whether an event overlaps a given interval
 *
 * e0 and e1 are the beginning and the end of the event, i0 and i1 the ones
 * of the interval.
 */
static gboolean
event_overlaps_interval (gint64 e0,
                         gint64 e1,
                         gint64 i0,
                         gint64 i1)
{
  /* This also ensures zero-length events are included */
  if (e0 >= i0 && e1 < i1)
    return TRUE;

  if (e1 <= i0)
    return FALSE;
  if (i1 <= e0)
    return FALSE;

  return TRUE;
}

/* _resetCache () */
static void
dbus_source_reset_cache (ShellCalendarDBusEventSource *self)
{
  g_hash_table_remove_all (self->events);
  self->has_last_request = FALSE;
}

/* _removeMatching (): whether an event was removed */
static gboolean
dbus_source_remove_matching (ShellCalendarDBusEventSource *self,
                             const char                   *uid_prefix)
{
  GHashTableIter iter;
  gpointer key;
  gboolean changed = FALSE;

  g_hash_table_iter_init (&iter, self->events);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      if (g_str_has_prefix (key, uid_prefix))
        {
          g_hash_table_iter_remove (&iter);
          changed = TRUE;
        }
    }

  return changed;
}

static void
set_time_range_cb (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  g_autoptr (ShellCalendarDBusEventSource) self = user_data;
  g_autoptr (GError) error = NULL;

  if (!shell_calendar_server_call_set_time_range_finish (SHELL_CALENDAR_SERVER (source),
                                                         res, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
          g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
          g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CLOSED) ||
          g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_DISCONNECTED))
        return;

      g_warning ("Failed to set the time range of the calendar server: %s",
                 error->message);
    }
}

/* _loadEvents () */
static void
dbus_source_load_events (ShellCalendarDBusEventSource *self,
                         gboolean                      force_reload)
{
  /* Ignore while loading */
  if (!self->initialized || self->proxy == NULL)
    return;

  if (self->has_cur_request)
    {
      if (force_reload)
        {
          g_hash_table_remove_all (self->events);
          source_emit_changed (SHELL_CALENDAR_EVENT_SOURCE (self));
        }

      /* The emission above may have destroyed the source */
      if (self->proxy == NULL)
        return;

      shell_calendar_server_call_set_time_range (self->proxy,
                                                 self->cur_request_begin,
                                                 self->cur_request_end,
                                                 force_reload,
                                                 self->cancellable,
                                                 set_time_range_cb,
                                                 g_object_ref (self));
    }
}

/* _onNameAppeared () */
static void
dbus_source_name_appeared (ShellCalendarDBusEventSource *self)
{
  self->initialized = TRUE;
  dbus_source_reset_cache (self);
  dbus_source_load_events (self, TRUE);
}

/* _onNameVanished () */
static void
dbus_source_name_vanished (ShellCalendarDBusEventSource *self)
{
  dbus_source_reset_cache (self);
  source_emit_changed (SHELL_CALENDAR_EVENT_SOURCE (self));
}

static void
proxy_name_owner_changed_cb (GObject                      *proxy,
                             GParamSpec                   *pspec,
                             ShellCalendarDBusEventSource *self)
{
  g_autofree char *owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (proxy));

  if (owner != NULL)
    dbus_source_name_appeared (self);
  else
    dbus_source_name_vanished (self);
}

static void
proxy_properties_changed_cb (GDBusProxy                   *proxy,
                             GVariant                     *changed,
                             const char * const           *invalidated,
                             ShellCalendarDBusEventSource *self)
{
  g_object_notify (G_OBJECT (self), "has-calendars");
}

/* _onEventsAddedOrUpdated () */
static void
proxy_events_added_or_updated_cb (ShellCalendarServer           *proxy,
                                  GVariant                      *appointments,
                                  ShellCalendarDBusEventSource  *self)
{
  g_autoptr (GHashTable) handled_removals = NULL;
  gboolean changed = FALSE;
  gsize n_appointments, i;

  if (!g_variant_is_of_type (appointments, G_VARIANT_TYPE ("a(ssxxa{sv})")))
    return;

  handled_removals = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  n_appointments = g_variant_n_children (appointments);

  for (i = 0; i < n_appointments; i++)
    {
      g_autoptr (GVariant) appointment = g_variant_get_child_value (appointments, i);
      g_autoptr (GVariant) id_value = g_variant_get_child_value (appointment, 0);
      g_autoptr (GVariant) summary_value = g_variant_get_child_value (appointment, 1);
      g_autoptr (GVariant) start_value = g_variant_get_child_value (appointment, 2);
      g_autoptr (GVariant) end_value = g_variant_get_child_value (appointment, 3);
      const char *id = g_variant_get_string (id_value, NULL);
      ShellCalendarEvent *event;
      ShellCalendarEvent *previous;

      event = shell_calendar_event_new (id,
                                        g_variant_get_int64 (start_value),
                                        g_variant_get_int64 (end_value),
                                        g_variant_get_string (summary_value, NULL));

      /* It's a recurring event */
      if (!g_str_has_suffix (id, "\n"))
        {
          const char *last_newline = strrchr (id, '\n');
          g_autofree char *parent_id =
            last_newline != NULL ? g_strndup (id, last_newline - id + 1) : g_strdup ("");

          if (!g_hash_table_contains (handled_removals, parent_id))
            {
              dbus_source_remove_matching (self, parent_id);
              g_hash_table_add (handled_removals, g_steal_pointer (&parent_id));
            }
        }

      /* Map.set () keeps the position of an existing key */
      previous = g_hash_table_lookup (self->events, id);
      event->seq = previous != NULL ? previous->seq : self->next_seq++;
      g_hash_table_replace (self->events, g_strdup (id), event);

      changed = TRUE;
    }

  if (changed)
    source_emit_changed (SHELL_CALENDAR_EVENT_SOURCE (self));
}

/* _onEventsRemoved () */
static void
proxy_events_removed_cb (ShellCalendarServer          *proxy,
                         const char * const           *ids,
                         ShellCalendarDBusEventSource *self)
{
  gboolean changed = FALSE;
  int i;

  for (i = 0; ids != NULL && ids[i] != NULL; i++)
    changed = dbus_source_remove_matching (self, ids[i]) || changed;

  if (changed)
    source_emit_changed (SHELL_CALENDAR_EVENT_SOURCE (self));
}

/* _onClientDisappeared () */
static void
proxy_client_disappeared_cb (ShellCalendarServer          *proxy,
                             const char                   *source_uid,
                             ShellCalendarDBusEventSource *self)
{
  g_autofree char *prefix = g_strconcat (source_uid != NULL ? source_uid : "", "\n", NULL);

  if (dbus_source_remove_matching (self, prefix))
    source_emit_changed (SHELL_CALENDAR_EVENT_SOURCE (self));
}

static void
dbus_source_stop_watching (ShellCalendarDBusEventSource *self)
{
  if (self->watch_id != 0)
    {
      g_bus_unwatch_name (self->watch_id);
      self->watch_id = 0;
    }
}

static void
dbus_source_name_watch_appeared_cb (GDBusConnection *connection,
                                    const char      *name,
                                    const char      *name_owner,
                                    gpointer         user_data)
{
  ShellCalendarDBusEventSource *self = user_data;

  /* The service showed up after the proxy gave up waiting for it */
  dbus_source_stop_watching (self);

  if (!self->destroyed && self->proxy == NULL)
    dbus_source_create_proxy (self);
}

static void
dbus_source_proxy_ready_cb (GObject      *source,
                            GAsyncResult *res,
                            gpointer      user_data)
{
  g_autoptr (ShellCalendarDBusEventSource) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (ShellCalendarServer) proxy = NULL;

  proxy = shell_calendar_server_proxy_new_for_bus_finish (res, &error);

  if (self->destroyed || g_cancellable_is_cancelled (self->cancellable))
    return;

  if (proxy == NULL)
    {
      /* Ignore timeouts, because with high probability the service will
       * appear later on, and we will then be able to create the proxy.
       * Other errors (such as the service not being installed) end the
       * loading, as they did in the JS. */
      if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_TIMED_OUT) ||
          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
        {
          if (self->watch_id == 0)
            self->watch_id = g_bus_watch_name (G_BUS_TYPE_SESSION,
                                               CALENDAR_SERVER_BUS_NAME,
                                               G_BUS_NAME_WATCHER_FLAGS_NONE,
                                               dbus_source_name_watch_appeared_cb,
                                               NULL,
                                               self, NULL);
        }
      else
        {
          g_message ("Error loading calendars: %s", error->message);
        }
      return;
    }

  self->proxy = g_steal_pointer (&proxy);

  g_signal_connect_object (self->proxy, "events-added-or-updated",
                           G_CALLBACK (proxy_events_added_or_updated_cb), self, 0);
  g_signal_connect_object (self->proxy, "events-removed",
                           G_CALLBACK (proxy_events_removed_cb), self, 0);
  g_signal_connect_object (self->proxy, "client-disappeared",
                           G_CALLBACK (proxy_client_disappeared_cb), self, 0);
  g_signal_connect_object (self->proxy, "notify::g-name-owner",
                           G_CALLBACK (proxy_name_owner_changed_cb), self, 0);
  g_signal_connect_object (self->proxy, "g-properties-changed",
                           G_CALLBACK (proxy_properties_changed_cb), self, 0);

  self->initialized = TRUE;
  g_object_notify (G_OBJECT (self), "has-calendars");
  dbus_source_name_appeared (self);
}

static void
dbus_source_create_proxy (ShellCalendarDBusEventSource *self)
{
  shell_calendar_server_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                           G_DBUS_PROXY_FLAGS_NONE,
                                           CALENDAR_SERVER_BUS_NAME,
                                           CALENDAR_SERVER_OBJECT_PATH,
                                           self->cancellable,
                                           dbus_source_proxy_ready_cb,
                                           g_object_ref (self));
}

/* destroy (): the equivalent of run_dispose () on the proxy */
static void
dbus_source_release (ShellCalendarDBusEventSource *self)
{
  self->destroyed = TRUE;
  self->initialized = FALSE;

  if (self->cancellable != NULL)
    g_cancellable_cancel (self->cancellable);

  dbus_source_stop_watching (self);

  if (self->proxy != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->proxy, self);
      g_clear_object (&self->proxy);
    }

  g_hash_table_remove_all (self->events);
}

static gboolean
dbus_source_get_has_calendars (ShellCalendarEventSource *source)
{
  ShellCalendarDBusEventSource *self = SHELL_CALENDAR_DBUS_EVENT_SOURCE (source);

  if (self->initialized && self->proxy != NULL)
    return shell_calendar_server_get_has_calendars (self->proxy);

  return FALSE;
}

static gboolean
dbus_source_get_is_loading (ShellCalendarEventSource *source)
{
  /* _isLoading was never set */
  return FALSE;
}

static void
dbus_source_destroy (ShellCalendarEventSource *source)
{
  dbus_source_release (SHELL_CALENDAR_DBUS_EVENT_SOURCE (source));
}

static void
dbus_source_request_range (ShellCalendarEventSource *source,
                           gint64                    begin,
                           gint64                    end)
{
  ShellCalendarDBusEventSource *self = SHELL_CALENDAR_DBUS_EVENT_SOURCE (source);

  if (self->has_last_request &&
      self->last_request_begin == begin &&
      self->last_request_end == end)
    return;

  self->has_last_request = TRUE;
  self->last_request_begin = begin;
  self->last_request_end = end;
  self->has_cur_request = TRUE;
  self->cur_request_begin = begin;
  self->cur_request_end = end;
  dbus_source_load_events (self, TRUE);
}

typedef struct
{
  gint64 begin;
  gint64 end;
} SortRange;

static int
compare_events (gconstpointer a,
                gconstpointer b,
                gpointer      user_data)
{
  ShellCalendarEvent *event1 = *(ShellCalendarEvent **) a;
  ShellCalendarEvent *event2 = *(ShellCalendarEvent **) b;
  const SortRange *range = user_data;
  gint64 d1, d2;

  /* sort events by end time on ending day */
  d1 = event1->start < range->begin && event1->end <= range->end ? event1->end : event1->start;
  d2 = event2->start < range->begin && event2->end <= range->end ? event2->end : event2->start;

  if (d1 != d2)
    return d1 < d2 ? -1 : 1;

  /* the stable sort of the JS keeps the order of the Map */
  if (event1->seq != event2->seq)
    return event1->seq < event2->seq ? -1 : 1;

  return 0;
}

static GPtrArray *
dbus_source_get_events (ShellCalendarEventSource *source,
                        gint64                    begin,
                        gint64                    end)
{
  ShellCalendarDBusEventSource *self = SHELL_CALENDAR_DBUS_EVENT_SOURCE (source);
  GPtrArray *result = g_ptr_array_new_with_free_func ((GDestroyNotify) shell_calendar_event_unref);
  SortRange range = { begin, end };
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, self->events);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      ShellCalendarEvent *event = value;

      if (event_overlaps_interval (event->start, event->end, begin, end))
        g_ptr_array_add (result, shell_calendar_event_ref (event));
    }

  g_ptr_array_sort_with_data (result, compare_events, &range);

  return result;
}

static gboolean
dbus_source_has_events (ShellCalendarEventSource *source,
                        GDateTime                *day)
{
  ShellCalendarDBusEventSource *self = SHELL_CALENDAR_DBUS_EVENT_SOURCE (source);
  g_autoptr (GDateTime) day_begin = get_beginning_of_day (day);
  g_autoptr (GDateTime) day_end = g_date_time_add_days (day_begin, 1);
  gint64 begin = g_date_time_to_unix (day_begin);
  gint64 end = g_date_time_to_unix (day_end);
  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init (&iter, self->events);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      ShellCalendarEvent *event = value;

      if (event_overlaps_interval (event->start, event->end, begin, end))
        return TRUE;
    }

  return FALSE;
}

static void
on_shutdown (ShellGlobal                  *global,
             ShellCalendarDBusEventSource *self)
{
  /* Nothing reaches the date menu once the shell is going away */
  dbus_source_release (self);
}

static void
shell_calendar_dbus_event_source_constructed (GObject *object)
{
  ShellCalendarDBusEventSource *self = SHELL_CALENDAR_DBUS_EVENT_SOURCE (object);

  G_OBJECT_CLASS (shell_calendar_dbus_event_source_parent_class)->constructed (object);

  g_signal_connect_object (shell_global_get (), "shutdown",
                           G_CALLBACK (on_shutdown), self, 0);

  dbus_source_create_proxy (self);
}

static void
shell_calendar_dbus_event_source_dispose (GObject *object)
{
  ShellCalendarDBusEventSource *self = SHELL_CALENDAR_DBUS_EVENT_SOURCE (object);

  dbus_source_release (self);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (shell_calendar_dbus_event_source_parent_class)->dispose (object);
}

static void
shell_calendar_dbus_event_source_finalize (GObject *object)
{
  ShellCalendarDBusEventSource *self = SHELL_CALENDAR_DBUS_EVENT_SOURCE (object);

  g_clear_pointer (&self->events, g_hash_table_unref);

  G_OBJECT_CLASS (shell_calendar_dbus_event_source_parent_class)->finalize (object);
}

static void
shell_calendar_dbus_event_source_class_init (ShellCalendarDBusEventSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellCalendarEventSourceClass *source_class = SHELL_CALENDAR_EVENT_SOURCE_CLASS (klass);

  object_class->constructed = shell_calendar_dbus_event_source_constructed;
  object_class->dispose = shell_calendar_dbus_event_source_dispose;
  object_class->finalize = shell_calendar_dbus_event_source_finalize;

  source_class->get_is_loading = dbus_source_get_is_loading;
  source_class->get_has_calendars = dbus_source_get_has_calendars;
  source_class->destroy = dbus_source_destroy;
  source_class->request_range = dbus_source_request_range;
  source_class->get_events = dbus_source_get_events;
  source_class->has_events = dbus_source_has_events;
}

static void
shell_calendar_dbus_event_source_init (ShellCalendarDBusEventSource *self)
{
  self->events = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify) shell_calendar_event_unref);
  self->cancellable = g_cancellable_new ();
}

/**
 * shell_calendar_dbus_event_source_new:
 *
 * Creates a source that reads the events from the calendar server on the
 * session bus (`new DBusEventSource ()`). A missing service is not an error:
 * the source has no calendar.
 *
 * Returns: (transfer full): a new #ShellCalendarDBusEventSource
 */
ShellCalendarDBusEventSource *
shell_calendar_dbus_event_source_new (void)
{
  return g_object_new (SHELL_TYPE_CALENDAR_DBUS_EVENT_SOURCE, NULL);
}

/* ------------------------------------------------------------------------ */
/* ShellCalendar                                                             */
/* ------------------------------------------------------------------------ */

enum
{
  CALENDAR_SELECTED_DATE_CHANGED,

  CALENDAR_N_SIGNALS
};

static guint calendar_signals[CALENDAR_N_SIGNALS] = { 0, };

struct _ShellCalendar
{
  StWidget parent;

  GSettings *settings;
  int week_start;
  gboolean use_weekdate;

  char *header_format_without_year;
  char *header_format;

  gint64 selected_date;          /* seconds since the epoch */
  gboolean should_date_grab_focus;

  ShellCalendarEventSource *event_source;
  gulong event_source_changed_id;

  ClutterActor *top_box;         /* weak */
  ClutterActor *back_button;     /* weak */
  ClutterActor *month_label;     /* weak */
  ClutterActor *forward_button;  /* weak */
  int first_day_index;

  GPtrArray *buttons;            /* of StButton, not owned */
  gboolean has_calendar_begin;
  gint64 calendar_begin;
  gint64 marked_as_today;
};

G_DEFINE_FINAL_TYPE (ShellCalendar, shell_calendar, ST_TYPE_WIDGET)

#define DAY_DATE_KEY "shell-calendar-day-date"

static void calendar_rebuild (ShellCalendar *self);
static void calendar_update (ShellCalendar *self);

/* _getWeekStartDay () */
static int
calendar_get_week_start_day (ShellCalendar *self)
{
  int week_start_day = g_settings_get_enum (self->settings, WEEK_START_DAY_KEY);

  if (week_start_day == G_DESKTOP_WEEKDAY_DEFAULT)
    return shell_util_get_week_start ();

  return week_start_day % 7;
}

/* column of the weekday @day in the grid */
static int
calendar_get_column (ShellCalendar *self,
                     int            day,
                     gboolean       rtl)
{
  int offset_cols = self->use_weekdate ? 1 : 0;

  if (rtl)
    return 6 - (7 + day - self->week_start) % 7;

  return offset_cols + (7 + day - self->week_start) % 7;
}

static gboolean
calendar_is_rtl (ClutterActor *actor)
{
  return clutter_actor_get_text_direction (actor) == CLUTTER_TEXT_DIRECTION_RTL;
}

static void
calendar_set_date_secs (ShellCalendar *self,
                        gint64         secs)
{
  g_autoptr (GDateTime) selected = date_from_secs (self->selected_date);
  g_autoptr (GDateTime) date = date_from_secs (secs);
  g_autoptr (GDateTime) datetime = NULL;

  if (same_day (date, selected))
    return;

  self->selected_date = secs;

  datetime = date_from_secs (self->selected_date);
  g_signal_emit (self, calendar_signals[CALENDAR_SELECTED_DATE_CHANGED], 0, datetime);

  calendar_update (self);
}

static void
calendar_on_prev_month_clicked (ShellCalendar *self)
{
  g_autoptr (GDateTime) selected = date_from_secs (self->selected_date);
  g_autoptr (GDateTime) new_date = shift_month (selected, -1);

  if (self->back_button != NULL)
    clutter_actor_grab_key_focus (self->back_button);

  calendar_set_date_secs (self, g_date_time_to_unix (new_date));
}

static void
calendar_on_next_month_clicked (ShellCalendar *self)
{
  g_autoptr (GDateTime) selected = date_from_secs (self->selected_date);
  g_autoptr (GDateTime) new_date = shift_month (selected, 1);

  if (self->forward_button != NULL)
    clutter_actor_grab_key_focus (self->forward_button);

  calendar_set_date_secs (self, g_date_time_to_unix (new_date));
}

static void
calendar_on_scroll (ClutterScrollController *controller,
                    ClutterSprite           *sprite,
                    ClutterScrollSource      source,
                    double                   dx,
                    double                   dy,
                    ShellCalendar           *self)
{
  if (dx < 0 || dy < 0)
    calendar_on_prev_month_clicked (self);
  else
    calendar_on_next_month_clicked (self);
}

static void
calendar_on_back_clicked (StButton      *button,
                          ShellCalendar *self)
{
  calendar_on_prev_month_clicked (self);
}

static void
calendar_on_forward_clicked (StButton      *button,
                             ShellCalendar *self)
{
  calendar_on_next_month_clicked (self);
}

/* _buildHeader () */
static void
calendar_build_header (ShellCalendar *self)
{
  ClutterLayoutManager *layout =
    clutter_actor_get_layout_manager (CLUTTER_ACTOR (self));
  int offset_cols = self->use_weekdate ? 1 : 0;
  g_autoptr (GDateTime) selected = date_from_secs (self->selected_date);
  g_autoptr (GDateTime) iter = NULL;
  ClutterActor *top_box, *back_button, *forward_button, *month_label;
  int i;

  g_ptr_array_set_size (self->buttons, 0);
  self->has_calendar_begin = FALSE;
  clutter_actor_destroy_all_children (CLUTTER_ACTOR (self));

  /* Top line of the calendar '<| September 2009 |>' */
  top_box = g_object_new (ST_TYPE_BOX_LAYOUT,
                          "style-class", "calendar-month-header",
                          NULL);
  g_set_weak_pointer (&self->top_box, top_box);
  clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), top_box,
                              0, 0, offset_cols + 7, 1);

  back_button = g_object_new (ST_TYPE_BUTTON,
                              "style-class", "calendar-change-month-back pager-button",
                              "icon-name", "pan-start-symbolic",
                              "accessible-name", _("Previous month"),
                              "can-focus", TRUE,
                              NULL);
  g_set_weak_pointer (&self->back_button, back_button);
  clutter_actor_add_child (top_box, back_button);
  g_signal_connect_object (back_button, "clicked",
                           G_CALLBACK (calendar_on_back_clicked), self, 0);

  month_label = g_object_new (ST_TYPE_LABEL,
                              "style-class", "calendar-month-label",
                              "can-focus", TRUE,
                              "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                              "x-expand", TRUE,
                              "y-align", CLUTTER_ACTOR_ALIGN_CENTER,
                              NULL);
  g_set_weak_pointer (&self->month_label, month_label);
  clutter_actor_add_child (top_box, month_label);

  forward_button = g_object_new (ST_TYPE_BUTTON,
                                 "style-class", "calendar-change-month-forward pager-button",
                                 "icon-name", "pan-end-symbolic",
                                 "accessible-name", _("Next month"),
                                 "can-focus", TRUE,
                                 NULL);
  g_set_weak_pointer (&self->forward_button, forward_button);
  clutter_actor_add_child (top_box, forward_button);
  g_signal_connect_object (forward_button, "clicked",
                           G_CALLBACK (calendar_on_forward_clicked), self, 0);

  /* Add weekday labels...
   *
   * We need to figure out the abbreviated localized names for the days of the week;
   * we do this by just getting the next 7 days starting from right now and then putting
   * them in the right cell in the table. It doesn't matter if we add them in order
   */
  iter = g_date_time_new (g_date_time_get_timezone (selected),
                          g_date_time_get_year (selected),
                          g_date_time_get_month (selected),
                          g_date_time_get_day_of_month (selected),
                          12, g_date_time_get_minute (selected), 0);
  for (i = 0; i < 7; i++)
    {
      /* Could use Shell.date_format_unix_msecs(iter.getTime(), '%a') but that normally gives three characters
       * and we want, ideally, a single character for e.g. S M T W T F S */
      g_autofree char *custom_day_abbrev = get_calendar_day_abbreviation (get_js_day (iter));
      g_autofree char *accessible_name = NULL;
      GDateTime *next;
      ClutterActor *label;
      int col;

      label = g_object_new (ST_TYPE_LABEL,
                            "style-class", "calendar-day-heading",
                            "text", custom_day_abbrev,
                            "can-focus", TRUE,
                            NULL);
      accessible_name = shell_date_format_with_c_format_string (iter, "%A");
      clutter_actor_set_accessible_name (label, accessible_name);

      col = calendar_get_column (self, get_js_day (iter),
                                 clutter_actor_get_text_direction (CLUTTER_ACTOR (self)) ==
                                 CLUTTER_TEXT_DIRECTION_RTL);
      clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), label, col, 1, 1, 1);

      next = g_date_time_add_days (iter, 1);
      g_date_time_unref (iter);
      iter = next;
    }

  /* All the children after this are days, and get removed when we update the calendar */
  self->first_day_index = clutter_actor_get_n_children (CLUTTER_ACTOR (self));
}

static void
calendar_on_day_clicked (StButton      *button,
                         ShellCalendar *self)
{
  gint64 *date = g_object_get_data (G_OBJECT (button), DAY_DATE_KEY);

  if (date == NULL)
    return;

  self->should_date_grab_focus = TRUE;
  calendar_set_date_secs (self, *date);
  self->should_date_grab_focus = FALSE;
}

/* _rebuildCalendar () */
static void
calendar_rebuild (ShellCalendar *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterLayoutManager *layout = clutter_actor_get_layout_manager (actor);
  gint64 now = now_secs ();
  g_autoptr (GDateTime) now_date = date_from_secs (now);
  g_autoptr (GDateTime) selected = date_from_secs (self->selected_date);
  g_autoptr (GDateTime) begin_date = NULL;
  g_autoptr (GDateTime) iter = NULL;
  g_autoptr (GDateTime) first = NULL;
  g_autoptr (GList) children = NULL;
  GList *l;
  int days_to_week_start;
  gboolean starts_on_week_start;
  int week_padding;
  int row;
  /* nRows here means 6 weeks + one header + one navbar */
  const int n_rows = 8;

  if (self->event_source == NULL)
    return;

  /* Remove everything but the topBox and the weekday labels */
  children = clutter_actor_get_children (actor);
  for (l = g_list_nth (children, self->first_day_index); l != NULL; l = l->next)
    clutter_actor_destroy (l->data);

  g_ptr_array_set_size (self->buttons, 0);

  /* Start at the beginning of the week before the start of the month
   *
   * We want to show always 6 weeks (to keep the calendar menu at the same
   * height if there are no events), so we pad it according to the following
   * policy:
   *
   * 1 - If a month has 6 weeks, we place no padding (example: Dec 2012)
   * 2 - If a month has 5 weeks and it starts on week start, we pad one week
   *     before it (example: Apr 2012)
   * 3 - If a month has 5 weeks and it starts on any other day, we pad one week
   *     after it (example: Nov 2012)
   * 4 - If a month has 4 weeks, we pad one week before and one after it
   *     (example: Feb 2010)
   *
   * Actually computing the number of weeks is complex, but we know that the
   * problematic categories (2 and 4) always start on week start, and that
   * all months at the end have 6 weeks.
   */
  first = g_date_time_new (g_date_time_get_timezone (selected),
                           g_date_time_get_year (selected),
                           g_date_time_get_month (selected),
                           1, 0, 0, 0);

  self->has_calendar_begin = TRUE;
  self->calendar_begin = g_date_time_to_unix (first);
  self->marked_as_today = now;

  days_to_week_start = (7 + get_js_day (first) - self->week_start) % 7;
  starts_on_week_start = days_to_week_start == 0;
  week_padding = starts_on_week_start ? 7 : 0;

  begin_date = g_date_time_add_days (first, -(week_padding + days_to_week_start));
  iter = g_date_time_ref (begin_date);
  row = 2;

  while (row < n_rows)
    {
      g_autofree char *label_text = NULL;
      g_autoptr (GString) style_class = NULL;
      GDateTime *next;
      ClutterActor *button;
      gint64 *date;
      gboolean rtl;
      gboolean has_events;
      gboolean left_most;
      int col;

      /* xgettext:no-c-format */
      label_text = shell_date_format_with_c_format_string (iter, C_("date day number format", "%d"));
      button = g_object_new (ST_TYPE_BUTTON,
                             "label", label_text,
                             "can-focus", TRUE,
                             NULL);
      rtl = calendar_is_rtl (button);

      if (SHELL_IS_CALENDAR_EMPTY_EVENT_SOURCE (self->event_source))
        clutter_actor_set_reactive (button, FALSE);

      date = g_new (gint64, 1);
      *date = g_date_time_to_unix (iter);
      g_object_set_data_full (G_OBJECT (button), DAY_DATE_KEY, date, g_free);
      g_signal_connect_object (button, "clicked",
                               G_CALLBACK (calendar_on_day_clicked), self, 0);

      has_events = shell_calendar_event_source_has_events (self->event_source, iter);
      style_class = g_string_new ("calendar-day");

      if (is_work_day (iter))
        g_string_append (style_class, " calendar-weekday");
      else
        g_string_append (style_class, " calendar-weekend");

      /* Hack used in lieu of border-collapse - see gnome-shell.css */
      if (row == 2)
        g_string_prepend (style_class, "calendar-day-top ");

      left_most = rtl
        ? get_js_day (iter) == (self->week_start + 6) % 7
        : get_js_day (iter) == self->week_start;
      if (left_most)
        g_string_prepend (style_class, "calendar-day-left ");

      if (same_day (now_date, iter))
        g_string_append (style_class, " calendar-today");
      else if (g_date_time_get_month (iter) != g_date_time_get_month (selected))
        g_string_append (style_class, " calendar-other-month");

      if (has_events)
        g_string_append (style_class, " calendar-day-with-events");

      st_widget_set_style_class_name (ST_WIDGET (button), style_class->str);

      col = calendar_get_column (self, get_js_day (iter), rtl);
      clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), button, col, row, 1, 1);

      g_ptr_array_add (self->buttons, button);

      if (self->use_weekdate && get_js_day (iter) == 4)
        {
          g_autofree char *week_number = shell_date_format_with_c_format_string (iter, "%V");
          const char *week_format = shell_util_translate_time_string (N_("Week %V"));
          g_autofree char *accessible_name = shell_date_format_with_c_format_string (iter, week_format);
          ClutterActor *label;

          label = g_object_new (ST_TYPE_LABEL,
                                "text", week_number,
                                "style-class", "calendar-week-number",
                                "can-focus", TRUE,
                                NULL);
          clutter_actor_set_y_align (CLUTTER_ACTOR (st_label_get_clutter_text (ST_LABEL (label))),
                                     CLUTTER_ACTOR_ALIGN_CENTER);
          clutter_actor_set_accessible_name (label, accessible_name);
          clutter_grid_layout_attach (CLUTTER_GRID_LAYOUT (layout), label,
                                      rtl ? 7 : 0, row, 1, 1);
        }

      next = g_date_time_add_days (iter, 1);
      g_date_time_unref (iter);
      iter = next;

      if (get_js_day (iter) == self->week_start)
        row++;
    }

  /* Signal to the event source that we are interested in events
   * only from this date range */
  shell_calendar_event_source_request_range (self->event_source,
                                             g_date_time_to_unix (begin_date),
                                             g_date_time_to_unix (iter));
}

/* _update () */
static void
calendar_update (ShellCalendar *self)
{
  g_autoptr (GDateTime) now = date_from_secs (now_secs ());
  g_autoptr (GDateTime) selected = date_from_secs (self->selected_date);
  g_autofree char *text = NULL;
  gboolean needs_rebuild;
  guint i;

  if (same_year (selected, now))
    text = shell_date_format_with_c_format_string (selected, self->header_format_without_year);
  else
    text = shell_date_format_with_c_format_string (selected, self->header_format);

  if (self->month_label != NULL)
    st_label_set_text (ST_LABEL (self->month_label), text);

  needs_rebuild = !self->has_calendar_begin;
  if (!needs_rebuild)
    {
      g_autoptr (GDateTime) begin = date_from_secs (self->calendar_begin);
      g_autoptr (GDateTime) marked = date_from_secs (self->marked_as_today);

      needs_rebuild = !same_month (selected, begin) || !same_day (now, marked);
    }

  if (needs_rebuild)
    calendar_rebuild (self);

  for (i = 0; i < self->buttons->len; i++)
    {
      ClutterActor *button = g_ptr_array_index (self->buttons, i);
      gint64 *date = g_object_get_data (G_OBJECT (button), DAY_DATE_KEY);
      g_autoptr (GDateTime) button_date = date_from_secs (date != NULL ? *date : 0);

      if (same_day (button_date, selected))
        {
          st_widget_add_style_pseudo_class (ST_WIDGET (button), "selected");
          if (self->should_date_grab_focus)
            clutter_actor_grab_key_focus (button);
        }
      else
        {
          st_widget_remove_style_pseudo_class (ST_WIDGET (button), "selected");
        }
    }
}

static void
calendar_on_settings_change (ShellCalendar *self)
{
  self->week_start = calendar_get_week_start_day (self);
  self->use_weekdate = g_settings_get_boolean (self->settings, SHOW_WEEKDATE_KEY);
  calendar_build_header (self);
  calendar_rebuild (self);
  calendar_update (self);
}

static void
calendar_on_event_source_changed (ShellCalendarEventSource *source,
                                  ShellCalendar            *self)
{
  calendar_rebuild (self);
  calendar_update (self);
}

static void
calendar_disconnect_event_source (ShellCalendar *self)
{
  if (self->event_source != NULL && self->event_source_changed_id != 0)
    g_signal_handler_disconnect (self->event_source, self->event_source_changed_id);

  self->event_source_changed_id = 0;
}

/**
 * shell_calendar_set_event_source:
 * @self: a #ShellCalendar
 * @event_source: the source of the events the grid marks
 *
 * Sets the source of the events (`setEventSource ()`). The grid is rebuilt
 * whenever the source changes. The calendar does not destroy the previous
 * source.
 */
void
shell_calendar_set_event_source (ShellCalendar            *self,
                                 ShellCalendarEventSource *event_source)
{
  g_return_if_fail (SHELL_IS_CALENDAR (self));
  g_return_if_fail (SHELL_IS_CALENDAR_EVENT_SOURCE (event_source));

  calendar_disconnect_event_source (self);

  g_set_object (&self->event_source, event_source);
  self->event_source_changed_id =
    g_signal_connect_object (self->event_source, "changed",
                             G_CALLBACK (calendar_on_event_source_changed), self, 0);

  calendar_rebuild (self);
  calendar_update (self);
}

/**
 * shell_calendar_get_selected_date:
 * @self: a #ShellCalendar
 *
 * Returns: (transfer full): the selected date, in the local time zone
 */
GDateTime *
shell_calendar_get_selected_date (ShellCalendar *self)
{
  g_return_val_if_fail (SHELL_IS_CALENDAR (self), NULL);

  return date_from_secs (self->selected_date);
}

/**
 * shell_calendar_set_date:
 * @self: a #ShellCalendar
 * @date: the date to show
 *
 * Sets the calendar to show a specific date (`setDate ()`). Emits
 * #ShellCalendar::selected-date-changed if the day changed.
 */
void
shell_calendar_set_date (ShellCalendar *self,
                         GDateTime     *date)
{
  g_return_if_fail (SHELL_IS_CALENDAR (self));
  g_return_if_fail (date != NULL);

  calendar_set_date_secs (self, g_date_time_to_unix (date));
}

/**
 * shell_calendar_update_time_zone:
 * @self: a #ShellCalendar
 *
 * Rebuilds the grid after the time zone changed, as the date might have
 * changed (`updateTimeZone ()`). The caller clears the cached local time zone
 * with shell_date_clear_cached_local_time_zone () first.
 */
void
shell_calendar_update_time_zone (ShellCalendar *self)
{
  g_return_if_fail (SHELL_IS_CALENDAR (self));

  calendar_rebuild (self);
  calendar_update (self);
}

/**
 * shell_calendar_new:
 *
 * Creates a calendar that shows the current date.
 *
 * Returns: (transfer floating): a new #ShellCalendar
 */
ShellCalendar *
shell_calendar_new (void)
{
  return g_object_new (SHELL_TYPE_CALENDAR, NULL);
}

static void
shell_calendar_constructed (GObject *object)
{
  ShellCalendar *self = SHELL_CALENDAR (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterAction *scroll_controller;

  G_OBJECT_CLASS (shell_calendar_parent_class)->constructed (object);

  clutter_actor_set_layout_manager (actor, clutter_grid_layout_new ());

  calendar_build_header (self);

  scroll_controller =
    clutter_scroll_controller_new (CLUTTER_SCROLL_CONTROLLER_FLAG_DISCRETE |
                                   CLUTTER_SCROLL_CONTROLLER_FLAG_SCROLL_HORIZONTAL |
                                   CLUTTER_SCROLL_CONTROLLER_FLAG_SCROLL_VERTICAL);
  g_signal_connect_object (scroll_controller, "scroll",
                           G_CALLBACK (calendar_on_scroll), self, 0);
  clutter_actor_add_action (actor, scroll_controller);
}

static void
shell_calendar_destroy (ClutterActor *actor)
{
  ShellCalendar *self = SHELL_CALENDAR (actor);

  calendar_disconnect_event_source (self);
  g_ptr_array_set_size (self->buttons, 0);

  CLUTTER_ACTOR_CLASS (shell_calendar_parent_class)->destroy (actor);
}

static void
shell_calendar_dispose (GObject *object)
{
  ShellCalendar *self = SHELL_CALENDAR (object);

  calendar_disconnect_event_source (self);
  g_clear_object (&self->event_source);
  g_clear_object (&self->settings);

  g_clear_weak_pointer (&self->top_box);
  g_clear_weak_pointer (&self->back_button);
  g_clear_weak_pointer (&self->month_label);
  g_clear_weak_pointer (&self->forward_button);

  G_OBJECT_CLASS (shell_calendar_parent_class)->dispose (object);
}

static void
shell_calendar_finalize (GObject *object)
{
  ShellCalendar *self = SHELL_CALENDAR (object);

  g_clear_pointer (&self->header_format_without_year, g_free);
  g_clear_pointer (&self->header_format, g_free);
  g_clear_pointer (&self->buttons, g_ptr_array_unref);

  G_OBJECT_CLASS (shell_calendar_parent_class)->finalize (object);
}

static void
shell_calendar_class_init (ShellCalendarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  object_class->constructed = shell_calendar_constructed;
  object_class->dispose = shell_calendar_dispose;
  object_class->finalize = shell_calendar_finalize;

  actor_class->destroy = shell_calendar_destroy;

  /**
   * ShellCalendar::selected-date-changed:
   * @self: the calendar
   * @date: the new selected date, in the local time zone
   *
   * Emitted when the user or the owner selected another day.
   */
  calendar_signals[CALENDAR_SELECTED_DATE_CHANGED] =
    g_signal_new ("selected-date-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_DATE_TIME);
}

static void
shell_calendar_init (ShellCalendar *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  self->buttons = g_ptr_array_new ();

  st_widget_set_style_class_name (ST_WIDGET (self), "calendar");
  clutter_actor_set_reactive (actor, TRUE);

  self->settings = g_settings_new ("org.gnome.desktop.calendar");

  g_signal_connect_object (self->settings, "changed::" WEEK_START_DAY_KEY,
                           G_CALLBACK (calendar_on_settings_change), self,
                           G_CONNECT_SWAPPED);
  self->week_start = calendar_get_week_start_day (self);

  g_signal_connect_object (self->settings, "changed::" SHOW_WEEKDATE_KEY,
                           G_CALLBACK (calendar_on_settings_change), self,
                           G_CONNECT_SWAPPED);
  self->use_weekdate = g_settings_get_boolean (self->settings, SHOW_WEEKDATE_KEY);

  /* Translators: The header displaying just the month name
   * standalone, when this is a month of the current year.
   * "%OB" is the new format specifier introduced in glibc 2.27,
   * in most cases you should not change it.
   */
  /* xgettext:no-c-format */
  self->header_format_without_year = g_strdup (_("%OB"));
  /* Translators: The header displaying the month name and the year
   * number, when this is a month of a different year.  You can
   * reorder the format specifiers or add other modifications
   * according to the requirements of your language.
   * "%OB" is the new format specifier introduced in glibc 2.27,
   * in most cases you should not use the old "%B" here unless you
   * absolutely know what you are doing.
   */
  /* xgettext:no-c-format */
  self->header_format = g_strdup (_("%OB %Y"));

  /* Start off with the current date */
  self->selected_date = now_secs ();
  self->should_date_grab_focus = FALSE;
}

/* ------------------------------------------------------------------------ */
/* ShellCalendarMessageList                                                  */
/* ------------------------------------------------------------------------ */

enum
{
  LIST_PROP_0,

  LIST_PROP_MESSAGE_TRAY,

  LIST_N_PROPS
};

static GParamSpec *list_props[LIST_N_PROPS] = { NULL, };

struct _ShellCalendarMessageList
{
  StWidget parent;

  ShellMessageTray *message_tray;      /* strong, construct only */

  ClutterActor *placeholder;           /* weak */
  ClutterActor *scroll_view;           /* weak */
  ClutterActor *clear_button;          /* weak */
  ShellMessageView *message_view;      /* weak */

  ClutterClickGesture *click_gesture;  /* strong */
  ClutterKeyController *key_controller; /* strong */
  ClutterActor *capture_container;     /* weak */

  ShellMprisSource *mpris_source;      /* strong */
  GHashTable *players;                 /* ShellMprisPlayer -> ShellMediaMessage, strong */
};

G_DEFINE_FINAL_TYPE (ShellCalendarMessageList, shell_calendar_message_list,
                     ST_TYPE_WIDGET)

static void
list_add_player (ShellCalendarMessageList *self,
                 ShellMprisPlayer         *player)
{
  ShellMediaMessage *message;

  if (self->message_view == NULL || g_hash_table_contains (self->players, player))
    return;

  message = shell_media_message_new (player);
  g_object_ref_sink (message);
  g_hash_table_insert (self->players, g_object_ref (player), message);
  shell_message_view_add_media_message (self->message_view, SHELL_MESSAGE (message));
}

static void
list_remove_player (ShellCalendarMessageList *self,
                    ShellMprisPlayer         *player)
{
  ShellMediaMessage *message = g_hash_table_lookup (self->players, player);

  if (message == NULL)
    return;

  if (self->message_view != NULL)
    shell_message_view_remove_media_message (self->message_view,
                                             SHELL_MESSAGE (message));
  g_hash_table_remove (self->players, player);
}

static void
list_on_player_added (ShellMprisSource          *source,
                      ShellMprisPlayer          *player,
                      ShellCalendarMessageList  *self)
{
  list_add_player (self, player);
}

static void
list_on_player_removed (ShellMprisSource          *source,
                        ShellMprisPlayer          *player,
                        ShellCalendarMessageList  *self)
{
  list_remove_player (self, player);
}

/* `_setupMpris ()` */
static void
list_setup_mpris (ShellCalendarMessageList *self)
{
  g_autoptr (GList) players = NULL;
  GList *l;

  self->mpris_source = shell_mpris_source_new ();
  g_signal_connect_object (self->mpris_source, "player-added",
                           G_CALLBACK (list_on_player_added), self, 0);
  g_signal_connect_object (self->mpris_source, "player-removed",
                           G_CALLBACK (list_on_player_removed), self, 0);

  players = shell_mpris_source_get_players (self->mpris_source);
  for (l = players; l != NULL; l = l->next)
    list_add_player (self, l->data);
}

static void
list_on_clear_clicked (StButton                 *button,
                       ShellCalendarMessageList *self)
{
  if (self->message_view != NULL)
    shell_message_view_clear (self->message_view);
}

static void
list_on_message_focused (ShellMessageView         *view,
                         ShellMessage             *message,
                         ShellCalendarMessageList *self)
{
  if (self->scroll_view != NULL && message != NULL)
    shell_animation_ensure_actor_visible_in_scroll_view (ST_SCROLL_VIEW (self->scroll_view),
                                                         CLUTTER_ACTOR (message));
}

static gboolean
list_click_may_recognize_cb (ClutterGesture           *gesture,
                             ShellCalendarMessageList *self)
{
  const ClutterEvent *event = clutter_gesture_get_point_event (gesture, 0);
  ClutterActor *stage;
  ClutterActor *target_actor;
  ClutterActor *expanded_group;
  gboolean on_scrollbar;

  if (event == NULL || self->message_view == NULL || self->scroll_view == NULL)
    return FALSE;

  stage = clutter_actor_get_stage (CLUTTER_ACTOR (self));
  if (stage == NULL)
    return FALSE;

  target_actor = clutter_stage_get_event_actor (CLUTTER_STAGE (stage), event);
  if (target_actor == NULL)
    return TRUE;

  on_scrollbar = clutter_actor_contains (self->scroll_view, target_actor) &&
                 !clutter_actor_contains (CLUTTER_ACTOR (self->message_view), target_actor);

  /* Collapse expanded group when the user clicks outside */
  expanded_group = shell_message_view_get_expanded_group (self->message_view);
  return !(expanded_group != NULL && clutter_actor_contains (expanded_group, target_actor)) &&
         !on_scrollbar;
}

static void
list_click_recognize_cb (ClutterGesture           *gesture,
                         ShellCalendarMessageList *self)
{
  if (self->message_view != NULL)
    shell_message_view_collapse (self->message_view);
}

static gboolean
list_key_press_cb (ClutterKeyController      *controller,
                   ShellCalendarMessageList  *self)
{
  uint32_t symbol = 0;

  clutter_key_controller_get_key (controller, &symbol, NULL, NULL);
  if (symbol == CLUTTER_KEY_Escape)
    {
      if (self->message_view != NULL)
        shell_message_view_collapse (self->message_view);
      return CLUTTER_EVENT_STOP;
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static gboolean
list_expanded_group_to_enabled (GBinding     *binding,
                                const GValue *from_value,
                                GValue       *to_value,
                                gpointer      user_data)
{
  g_value_set_boolean (to_value, g_value_get_object (from_value) != NULL);
  return TRUE;
}

/* Removes the capture actions from the container they were added to. The
 * container is a weak pointer that is only cleared when the actor is
 * finalized: a destroyed container has already dropped its actions, and
 * removing them again warns, so check what the actions are attached to. */
static void
message_list_detach_capture_actions (ShellCalendarMessageList *self)
{
  ClutterActorMeta *metas[2] = {
    CLUTTER_ACTOR_META (self->click_gesture),
    CLUTTER_ACTOR_META (self->key_controller),
  };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (metas); i++)
    {
      if (metas[i] != NULL &&
          self->capture_container != NULL &&
          clutter_actor_meta_get_actor (metas[i]) == self->capture_container)
        clutter_actor_remove_action (self->capture_container,
                                     CLUTTER_ACTION (metas[i]));
    }

  g_clear_weak_pointer (&self->capture_container);
}

/**
 * shell_calendar_message_list_set_capture_container:
 * @self: a #ShellCalendarMessageList
 * @container: (nullable): the actor whose clicks and key presses collapse the
 *   expanded group of notifications
 *
 * Makes @container collapse the expanded group when the user clicks outside
 * of it or presses Escape (`setCaptureContainer ()`).
 */
void
shell_calendar_message_list_set_capture_container (ShellCalendarMessageList *self,
                                                   ClutterActor             *container)
{
  g_return_if_fail (SHELL_IS_CALENDAR_MESSAGE_LIST (self));
  g_return_if_fail (container == NULL || CLUTTER_IS_ACTOR (container));

  if (container == self->capture_container)
    return;

  message_list_detach_capture_actions (self);

  if (container != NULL)
    {
      g_set_weak_pointer (&self->capture_container, container);

      clutter_actor_add_action_full (container,
                                     "calendar-close-expanded-group-click",
                                     CLUTTER_PHASE_CAPTURE,
                                     CLUTTER_ACTION (self->click_gesture));
      clutter_actor_add_action_full (container,
                                     "calendar-close-expanded-group-key",
                                     CLUTTER_PHASE_CAPTURE,
                                     CLUTTER_ACTION (self->key_controller));
    }
}

/**
 * shell_calendar_message_list_get_message_view:
 * @self: a #ShellCalendarMessageList
 *
 * Gets the view of the messages, to which the list adds the media messages
 * of the MPRIS players.
 *
 * Returns: (transfer none): the #ShellMessageView
 */
ShellMessageView *
shell_calendar_message_list_get_message_view (ShellCalendarMessageList *self)
{
  g_return_val_if_fail (SHELL_IS_CALENDAR_MESSAGE_LIST (self), NULL);

  return self->message_view;
}

/**
 * shell_calendar_message_list_new:
 * @message_tray: the tray whose notifications the list shows
 *
 * Creates the list of notifications of the date menu.
 *
 * Returns: (transfer floating): a new #ShellCalendarMessageList
 */
ShellCalendarMessageList *
shell_calendar_message_list_new (ShellMessageTray *message_tray)
{
  g_return_val_if_fail (SHELL_IS_MESSAGE_TRAY (message_tray), NULL);

  return g_object_new (SHELL_TYPE_CALENDAR_MESSAGE_LIST,
                       "message-tray", message_tray,
                       NULL);
}

static void
shell_calendar_message_list_constructed (GObject *object)
{
  ShellCalendarMessageList *self = SHELL_CALENDAR_MESSAGE_LIST (object);
  ClutterActor *actor = CLUTTER_ACTOR (self);
  ClutterActor *placeholder, *icon, *label, *box, *scroll_view, *hbox, *clear_button;
  ShellMessageView *message_view;

  G_OBJECT_CLASS (shell_calendar_message_list_parent_class)->constructed (object);

  clutter_actor_set_layout_manager (actor, clutter_bin_layout_new ());

  placeholder = g_object_new (ST_TYPE_BOX_LAYOUT,
                              "style-class", "message-list-placeholder",
                              NULL);
  st_box_layout_set_orientation (ST_BOX_LAYOUT (placeholder), CLUTTER_ORIENTATION_VERTICAL);
  g_set_weak_pointer (&self->placeholder, placeholder);
  clutter_actor_add_child (actor, placeholder);

  icon = g_object_new (ST_TYPE_ICON,
                       "icon-name", "no-notifications-symbolic",
                       NULL);
  clutter_actor_add_child (placeholder, icon);

  label = g_object_new (ST_TYPE_LABEL,
                        "text", _("No Notifications"),
                        "x-align", CLUTTER_ACTOR_ALIGN_CENTER,
                        NULL);
  clutter_actor_add_child (placeholder, label);

  box = g_object_new (ST_TYPE_BOX_LAYOUT,
                      "x-expand", TRUE,
                      "y-expand", TRUE,
                      NULL);
  st_box_layout_set_orientation (ST_BOX_LAYOUT (box), CLUTTER_ORIENTATION_VERTICAL);
  clutter_actor_add_child (actor, box);

  message_view = shell_message_view_new (self->message_tray);
  g_set_weak_pointer (&self->message_view, message_view);

  scroll_view = g_object_new (ST_TYPE_SCROLL_VIEW,
                              "overlay-scrollbars", TRUE,
                              "x-expand", TRUE,
                              "y-expand", TRUE,
                              NULL);
  st_scroll_view_set_child (ST_SCROLL_VIEW (scroll_view), ST_SCROLLABLE (message_view));
  g_set_weak_pointer (&self->scroll_view, scroll_view);
  clutter_actor_add_child (box, scroll_view);

  hbox = g_object_new (ST_TYPE_BOX_LAYOUT,
                       "style-class", "message-list-controls",
                       NULL);
  clutter_actor_add_child (box, hbox);

  clear_button = g_object_new (ST_TYPE_BUTTON,
                               "style-class", "message-list-clear-button button",
                               "label", _("Clear"),
                               "can-focus", TRUE,
                               "x-expand", TRUE,
                               "x-align", CLUTTER_ACTOR_ALIGN_START,
                               "accessible-name", C_("action", "Clear all notifications"),
                               NULL);
  g_set_weak_pointer (&self->clear_button, clear_button);
  g_signal_connect_object (clear_button, "clicked",
                           G_CALLBACK (list_on_clear_clicked), self, 0);
  clutter_actor_add_child (hbox, clear_button);

  g_object_bind_property (placeholder, "visible",
                          clear_button, "visible",
                          G_BINDING_INVERT_BOOLEAN);

  g_signal_connect_object (message_view, "message-focused",
                           G_CALLBACK (list_on_message_focused), self, 0);

  g_object_bind_property (message_view, "empty",
                          placeholder, "visible",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (message_view, "can-clear",
                          clear_button, "reactive",
                          G_BINDING_SYNC_CREATE);

  g_signal_connect_object (self->click_gesture, "may-recognize",
                           G_CALLBACK (list_click_may_recognize_cb), self, 0);
  g_signal_connect_object (self->click_gesture, "recognize",
                           G_CALLBACK (list_click_recognize_cb), self, 0);
  g_object_bind_property_full (message_view, "expanded-group",
                               self->click_gesture, "enabled",
                               G_BINDING_SYNC_CREATE,
                               list_expanded_group_to_enabled, NULL,
                               NULL, NULL);

  g_signal_connect_object (self->key_controller, "key-press",
                           G_CALLBACK (list_key_press_cb), self, 0);
  g_object_bind_property_full (message_view, "expanded-group",
                               self->key_controller, "enabled",
                               G_BINDING_SYNC_CREATE,
                               list_expanded_group_to_enabled, NULL,
                               NULL, NULL);

  list_setup_mpris (self);
}

static void
shell_calendar_message_list_get_property (GObject    *object,
                                          guint       prop_id,
                                          GValue     *value,
                                          GParamSpec *pspec)
{
  ShellCalendarMessageList *self = SHELL_CALENDAR_MESSAGE_LIST (object);

  switch (prop_id)
    {
    case LIST_PROP_MESSAGE_TRAY:
      g_value_set_object (value, self->message_tray);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_calendar_message_list_set_property (GObject      *object,
                                          guint         prop_id,
                                          const GValue *value,
                                          GParamSpec   *pspec)
{
  ShellCalendarMessageList *self = SHELL_CALENDAR_MESSAGE_LIST (object);

  switch (prop_id)
    {
    case LIST_PROP_MESSAGE_TRAY:
      g_set_object (&self->message_tray, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
shell_calendar_message_list_dispose (GObject *object)
{
  ShellCalendarMessageList *self = SHELL_CALENDAR_MESSAGE_LIST (object);

  message_list_detach_capture_actions (self);

  g_clear_weak_pointer (&self->placeholder);
  g_clear_weak_pointer (&self->scroll_view);
  g_clear_weak_pointer (&self->clear_button);
  g_clear_weak_pointer (&self->message_view);

  if (self->mpris_source != NULL)
    g_signal_handlers_disconnect_by_data (self->mpris_source, self);
  g_clear_object (&self->mpris_source);
  g_clear_pointer (&self->players, g_hash_table_unref);

  g_clear_object (&self->click_gesture);
  g_clear_object (&self->key_controller);
  g_clear_object (&self->message_tray);

  G_OBJECT_CLASS (shell_calendar_message_list_parent_class)->dispose (object);
}

static void
shell_calendar_message_list_class_init (ShellCalendarMessageListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = shell_calendar_message_list_constructed;
  object_class->dispose = shell_calendar_message_list_dispose;
  object_class->get_property = shell_calendar_message_list_get_property;
  object_class->set_property = shell_calendar_message_list_set_property;

  /**
   * ShellCalendarMessageList:message-tray:
   *
   * The tray whose notifications the list shows.
   */
  list_props[LIST_PROP_MESSAGE_TRAY] =
    g_param_spec_object ("message-tray", NULL, NULL,
                         SHELL_TYPE_MESSAGE_TRAY,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LIST_N_PROPS, list_props);
}

static void
shell_calendar_message_list_init (ShellCalendarMessageList *self)
{
  ClutterActor *actor = CLUTTER_ACTOR (self);

  self->players = g_hash_table_new_full (NULL, NULL,
                                          g_object_unref, g_object_unref);

  st_widget_set_style_class_name (ST_WIDGET (self), "message-list");
  clutter_actor_set_x_expand (actor, TRUE);
  clutter_actor_set_y_expand (actor, TRUE);

  /* Gestures kept in fields are sunk, as the actor they are added to sinks
   * (and drops) the floating reference */
  self->click_gesture =
    CLUTTER_CLICK_GESTURE (g_object_ref_sink (clutter_click_gesture_new ()));
  self->key_controller =
    CLUTTER_KEY_CONTROLLER (g_object_ref_sink (clutter_key_controller_new (NULL)));
}
