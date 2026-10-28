/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * date-utils.c: Date and time formatting helpers.
 * Port of js/misc/dateUtils.js.
 *
 * The JS module accepted either a JS Date or a GLib.DateTime. A GDateTime is
 * the only type here; JS callers with a Date convert it with
 * shell_date_time_new_from_unix_msecs () or use the *_unix_msecs variants,
 * passing Date.getTime ().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "misc/date-utils.h"
#include "shell-util.h"

#define DESKTOP_INTERFACE_SCHEMA "org.gnome.desktop.interface"

static GSettings *desktop_settings = NULL;
static GTimeZone *local_time_zone = NULL;

static GTimeZone *
get_local_time_zone (void)
{
  if (local_time_zone == NULL)
    local_time_zone = g_time_zone_new_local ();

  return local_time_zone;
}

/**
 * shell_date_time_new_from_unix_msecs:
 * @msecs: milliseconds since the epoch, as returned by `Date.getTime ()`
 *
 * Converts a JS `Date` to a #GDateTime in the cached local time zone (see
 * shell_date_clear_cached_local_time_zone ()). Like the JS conversion this
 * drops the milliseconds.
 *
 * Returns: (transfer full) (nullable): a new #GDateTime, or %NULL if @msecs is
 *   out of the range #GDateTime supports
 */
GDateTime *
shell_date_time_new_from_unix_msecs (gint64 msecs)
{
  g_autoptr (GDateTime) utc = NULL;
  gint64 secs = msecs / 1000;

  if (msecs % 1000 < 0)
    secs--;

  utc = g_date_time_new_from_unix_utc (secs);
  if (utc == NULL)
    return NULL;

  return g_date_time_to_timezone (utc, get_local_time_zone ());
}

/**
 * shell_date_format_with_c_format_string:
 * @date: (nullable): a #GDateTime
 * @format: a C `strftime ()`-style format string, see g_date_time_format ()
 *
 * Formats @date according to @format.
 *
 * Returns: (transfer full): the formatted string, empty if @date is %NULL or
 *   @format is invalid
 */
char *
shell_date_format_with_c_format_string (GDateTime  *date,
                                        const char *format)
{
  char *formatted;

  g_return_val_if_fail (format != NULL, NULL);

  if (date == NULL)
    return g_strdup ("");

  formatted = g_date_time_format (date, format);

  return formatted != NULL ? formatted : g_strdup ("");
}

/**
 * shell_date_format_unix_msecs:
 * @msecs: milliseconds since the epoch, as returned by `Date.getTime ()`
 * @format: a C `strftime ()`-style format string
 *
 * Formats the JS `Date` @msecs represents according to @format, using the
 * cached local time zone. This is `formatDateWithCFormatString ()`.
 *
 * Returns: (transfer full): the formatted string
 */
char *
shell_date_format_unix_msecs (gint64      msecs,
                              const char *format)
{
  g_autoptr (GDateTime) date = shell_date_time_new_from_unix_msecs (msecs);

  return shell_date_format_with_c_format_string (date, format);
}

/**
 * shell_date_format_time_span:
 * @date: the start of the time span
 *
 * Formats a time span string representing the time between @date and the
 * current time, such as "Just now" or "3 hours ago".
 *
 * Returns: (transfer full): the formatted string
 */
char *
shell_date_format_time_span (GDateTime *date)
{
  g_autoptr (GDateTime) now = NULL;
  double minutes_ago, hours_ago, days_ago, weeks_ago, months_ago, years_ago;
  gint64 timespan;

  g_return_val_if_fail (date != NULL, NULL);

  now = g_date_time_new_now (get_local_time_zone ());
  timespan = g_date_time_difference (now, date);

  minutes_ago = (double) timespan / G_TIME_SPAN_MINUTE;
  hours_ago = (double) timespan / G_TIME_SPAN_HOUR;
  days_ago = (double) timespan / G_TIME_SPAN_DAY;
  weeks_ago = days_ago / 7;
  months_ago = days_ago / 30;
  years_ago = weeks_ago / 52;

  if (minutes_ago < 5)
    return g_strdup (_("Just now"));
  if (hours_ago < 1)
    {
      return g_strdup_printf (ngettext ("%d minute ago",
                                        "%d minutes ago",
                                        (gulong) minutes_ago),
                              (int) minutes_ago);
    }
  if (days_ago < 1)
    {
      return g_strdup_printf (ngettext ("%d hour ago",
                                        "%d hours ago",
                                        (gulong) hours_ago),
                              (int) hours_ago);
    }
  if (days_ago < 2)
    return g_strdup (_("Yesterday"));
  if (days_ago < 15)
    {
      return g_strdup_printf (ngettext ("%d day ago",
                                        "%d days ago",
                                        (gulong) days_ago),
                              (int) days_ago);
    }
  if (weeks_ago < 8)
    {
      return g_strdup_printf (ngettext ("%d week ago",
                                        "%d weeks ago",
                                        (gulong) weeks_ago),
                              (int) weeks_ago);
    }
  if (years_ago < 1)
    {
      return g_strdup_printf (ngettext ("%d month ago",
                                        "%d months ago",
                                        (gulong) months_ago),
                              (int) months_ago);
    }

  return g_strdup_printf (ngettext ("%d year ago",
                                    "%d years ago",
                                    (gulong) years_ago),
                          (int) years_ago);
}

/* format.replace (/\s*%p/g, '') */
static char *
strip_am_pm (const char *format)
{
  GString *stripped = g_string_new (NULL);
  const char *p = format;

  while (*p != '\0')
    {
      if (p[0] == '%' && p[1] == 'p')
        {
          while (stripped->len > 0 &&
                 g_ascii_isspace (stripped->str[stripped->len - 1]))
            g_string_truncate (stripped, stripped->len - 1);

          p += 2;
          continue;
        }

      g_string_append_c (stripped, *p);
      p++;
    }

  return g_string_free (stripped, FALSE);
}

/**
 * shell_date_format_time:
 * @datetime: (nullable): the time to format
 * @flags: style of the string
 *
 * Formats @datetime according to the clock format of the desktop (24h or 12h).
 * It is shown alone if it is today or @flags has
 * %SHELL_DATE_FORMAT_FLAGS_TIME_ONLY, otherwise with "Yesterday", the week
 * day or the date, depending on how long ago it was.
 *
 * Returns: (transfer full): the formatted string, empty if @datetime is %NULL
 */
char *
shell_date_format_time (GDateTime            *datetime,
                        ShellDateFormatFlags  flags)
{
  g_autoptr (GDateTime) now = NULL;
  g_autofree char *clock_format = NULL;
  g_autofree char *stripped = NULL;
  g_autofree char *formatted = NULL;
  gboolean time_only = (flags & SHELL_DATE_FORMAT_FLAGS_TIME_ONLY) != 0;
  gboolean ampm = (flags & SHELL_DATE_FORMAT_FLAGS_NO_AMPM) == 0;
  const char *format;
  double days_ago;

  if (datetime == NULL)
    return g_strdup ("");

  now = g_date_time_new_now (get_local_time_zone ());
  days_ago = (double) g_date_time_difference (now, datetime) /
             (24 * 60 * 60 * G_GINT64_CONSTANT (1000000));

  if (desktop_settings == NULL)
    desktop_settings = g_settings_new (DESKTOP_INTERFACE_SCHEMA);
  clock_format = g_settings_get_string (desktop_settings, "clock-format");

  if (g_strcmp0 (clock_format, "24h") == 0)
    {
      /* Show only the time if date is on today */
      if (days_ago < 1 || time_only)
        /* Translators: Time in 24h format */
        format = N_("%H:%M");
      /* Show the word "Yesterday" and time if date is on yesterday */
      else if (days_ago < 2)
        /* Translators: this is the word "Yesterday" followed by a
         time string in 24h format. i.e. "Yesterday, 14:30" */
        /* xgettext:no-c-format */
        format = N_("Yesterday, %H:%M");
      /* Show a week day and time if date is in the last week */
      else if (days_ago < 7)
        /* Translators: this is the week day name followed by a time
         string in 24h format. i.e. "Monday, 14:30" */
        /* xgettext:no-c-format */
        format = N_("%A, %H:%M");
      else if (g_date_time_get_year (datetime) == g_date_time_get_year (now))
        /* Translators: this is the month name and day number
         followed by a time string in 24h format.
         i.e. "May 25, 14:30" */
        /* xgettext:no-c-format */
        format = N_("%B %-d, %H:%M");
      else
        /* Translators: this is the month name, day number, year
         number followed by a time string in 24h format.
         i.e. "May 25 2012, 14:30" */
        /* xgettext:no-c-format */
        format = N_("%B %-d %Y, %H:%M");
    }
  else
    {
      /* Show only the time if date is on today */
      if (days_ago < 1 || time_only)
        /* Translators: Time in 12h format */
        format = N_("%l:%M %p");
      /* Show the word "Yesterday" and time if date is on yesterday */
      else if (days_ago < 2)
        /* Translators: this is the word "Yesterday" followed by a
         time string in 12h format. i.e. "Yesterday, 2:30 pm" */
        /* xgettext:no-c-format */
        format = N_("Yesterday, %l:%M %p");
      /* Show a week day and time if date is in the last week */
      else if (days_ago < 7)
        /* Translators: this is the week day name followed by a time
         string in 12h format. i.e. "Monday, 2:30 pm" */
        /* xgettext:no-c-format */
        format = N_("%A, %l:%M %p");
      else if (g_date_time_get_year (datetime) == g_date_time_get_year (now))
        /* Translators: this is the month name and day number
         followed by a time string in 12h format.
         i.e. "May 25, 2:30 pm" */
        /* xgettext:no-c-format */
        format = N_("%B %-d, %l:%M %p");
      else
        /* Translators: this is the month name, day number, year
         number followed by a time string in 12h format.
         i.e. "May 25 2012, 2:30 pm"*/
        /* xgettext:no-c-format */
        format = N_("%B %-d %Y, %l:%M %p");
    }

  /* Time in short 12h format, without the equivalent of "AM" or "PM"; used
   * when it is clear from the context */
  if (!ampm)
    {
      stripped = strip_am_pm (format);
      format = stripped;
    }

  formatted = g_date_time_format (datetime, shell_util_translate_time_string (format));

  return formatted != NULL ? g_steal_pointer (&formatted) : g_strdup ("");
}

/**
 * shell_date_format_time_unix_msecs:
 * @msecs: milliseconds since the epoch, as returned by `Date.getTime ()`
 * @flags: style of the string
 *
 * Like shell_date_format_time (), for a JS `Date`.
 *
 * Returns: (transfer full): the formatted string
 */
char *
shell_date_format_time_unix_msecs (gint64               msecs,
                                   ShellDateFormatFlags flags)
{
  g_autoptr (GDateTime) datetime = shell_date_time_new_from_unix_msecs (msecs);

  return shell_date_format_time (datetime, flags);
}

/**
 * shell_date_clear_cached_local_time_zone:
 *
 * Updates the local time zone these functions use, after it changed. JS also
 * has to clear the date caches of the engine (`System.clearDateCaches ()`),
 * which is not something C can do.
 */
void
shell_date_clear_cached_local_time_zone (void)
{
  g_clear_pointer (&local_time_zone, g_time_zone_unref);
  local_time_zone = g_time_zone_new_local ();
}
