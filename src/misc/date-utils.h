/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * date-utils.h: Date and time formatting helpers.
 * Port of js/misc/dateUtils.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * ShellDateFormatFlags:
 * @SHELL_DATE_FORMAT_FLAGS_NONE: format the time together with a day or date
 *   when the time is not today, and include "am" or "pm" in 12h mode
 * @SHELL_DATE_FORMAT_FLAGS_TIME_ONLY: the string only contains the time, never
 *   the day or the date (the `timeOnly` parameter of `formatTime ()`)
 * @SHELL_DATE_FORMAT_FLAGS_NO_AMPM: leave the "am" or "pm" out of the string
 *   (`ampm: false`)
 *
 * Style of the string made by shell_date_format_time (). The default of the
 * JS parameter object, `{timeOnly: false, ampm: true}`, is
 * %SHELL_DATE_FORMAT_FLAGS_NONE.
 */
typedef enum
{
  SHELL_DATE_FORMAT_FLAGS_NONE      = 0,
  SHELL_DATE_FORMAT_FLAGS_TIME_ONLY = 1 << 0,
  SHELL_DATE_FORMAT_FLAGS_NO_AMPM   = 1 << 1,
} ShellDateFormatFlags;

GDateTime *shell_date_time_new_from_unix_msecs (gint64 msecs);

char *shell_date_format_with_c_format_string (GDateTime  *date,
                                              const char *format);
char *shell_date_format_unix_msecs           (gint64      msecs,
                                              const char *format);

char *shell_date_format_time_span (GDateTime *date);

char *shell_date_format_time            (GDateTime            *datetime,
                                         ShellDateFormatFlags  flags);
char *shell_date_format_time_unix_msecs (gint64                msecs,
                                         ShellDateFormatFlags  flags);

void  shell_date_clear_cached_local_time_zone (void);

G_END_DECLS
