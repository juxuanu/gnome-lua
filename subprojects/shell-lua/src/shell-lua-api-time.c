/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.time (docs/api/core.md, Since 1.1): the wall and monotonic clocks, the
 * calendar fields of an instant, their inverse, GDateTime formatting and the
 * local time zone; and the watch behind the `time.timezone-changed` hook
 * (shell-lua-time.h).
 *
 * Notes for maintainers:
 *  - Everything is GDateTime: the supported years are 1..9999, a value outside
 *    them (or a NaN) raises a Lua error naming the field.
 *  - `at` is split into whole seconds and a microsecond count before it
 *    reaches GLib; the microseconds are rounded, so that date{at = t}.microsecond
 *    is what t printed with six decimals says.
 */

#ifndef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ShellLua"
#endif

#include <math.h>
#include <string.h>

#include <gio/gio.h>

#include "shell-lua-api-private.h"
#include "shell-lua-time.h"

static void __attribute__ ((constructor))
shell_lua_api_time_init (void)
{
  shell_lua_api_register ("time", shell_lua_api_time_register);
}

/* GDateTime's range: 0001-01-01T00:00:00Z .. 9999-12-31T23:59:59Z */
#define MIN_UNIX_SECONDS (-62135596800LL)
#define MAX_UNIX_SECONDS (253402300799LL)

#define MAX_FORMAT_LENGTH 1024

/* ------------------------------------------------------------------------ */
/* Clocks                                                                    */
/* ------------------------------------------------------------------------ */

static int
l_now (lua_State *L)
{
  lua_pushnumber (L, (lua_Number) g_get_real_time () / 1e6);
  return 1;
}

static int
l_monotonic (lua_State *L)
{
  lua_pushnumber (L, (lua_Number) g_get_monotonic_time () / 1e6);
  return 1;
}

/* ------------------------------------------------------------------------ */
/* Instants                                                                  */
/* ------------------------------------------------------------------------ */

static const char *const instant_fields[] = { "at", "utc", NULL };

/* Reads the optional { at =, utc = } table at @idx (none or nil: now, local)
 * and returns the instant as a GDateTime including its microseconds
 * (transfer full). Raises on a bad field. */
static GDateTime *
read_instant (lua_State *L,
              int        idx)
{
  GDateTime *base, *dt;
  gboolean utc = FALSE;
  gint64 seconds, micros;

  idx = lua_absindex (L, idx);

  if (lua_isnoneornil (L, idx))
    {
      gint64 now = g_get_real_time ();

      seconds = now / G_USEC_PER_SEC;
      micros = now % G_USEC_PER_SEC;
    }
  else
    {
      shell_lua_check_fields (L, idx, instant_fields);
      utc = shell_lua_opt_boolean (L, idx, "utc", FALSE);

      if (lua_getfield (L, idx, "at") == LUA_TNIL)
        {
          gint64 now = g_get_real_time ();

          seconds = now / G_USEC_PER_SEC;
          micros = now % G_USEC_PER_SEC;
        }
      else
        {
          lua_Number at, whole;

          if (lua_type (L, -1) != LUA_TNUMBER)
            luaL_error (L, "field 'at' must be a number, got %s", luaL_typename (L, -1));

          at = lua_tonumber (L, -1);
          if (!(at >= (lua_Number) MIN_UNIX_SECONDS && at < (lua_Number) MAX_UNIX_SECONDS + 1.0))
            luaL_error (L, "field 'at' is out of range (years 1 to 9999)");

          whole = floor (at);
          seconds = (gint64) whole;
          micros = (gint64) llround ((at - whole) * 1e6);
          if (micros >= G_USEC_PER_SEC)
            {
              seconds++;
              micros -= G_USEC_PER_SEC;
            }
        }
      lua_pop (L, 1);
    }

  if (seconds < MIN_UNIX_SECONDS || seconds > MAX_UNIX_SECONDS)
    luaL_error (L, "field 'at' is out of range (years 1 to 9999)");

  /* Nothing owned may be live when luaL_error () jumps */
  base = utc ? g_date_time_new_from_unix_utc (seconds)
             : g_date_time_new_from_unix_local (seconds);
  if (base == NULL)
    luaL_error (L, "field 'at' is out of range (years 1 to 9999)");

  dt = g_date_time_add (base, micros);
  g_date_time_unref (base);
  if (dt == NULL)
    luaL_error (L, "field 'at' is out of range (years 1 to 9999)");

  return dt;
}

static void
set_integer_field (lua_State  *L,
                   const char *name,
                   lua_Integer value)
{
  lua_pushinteger (L, value);
  lua_setfield (L, -2, name);
}

static int
l_date (lua_State *L)
{
  GDateTime *dt;
  const char *abbreviation;
  lua_Integer year, month, day, hour, minute, second, micro, weekday, yearday, offset;
  gboolean dst;

  luaL_checkstack (L, 4, "shell.time.date");
  if (lua_gettop (L) > 1)
    return luaL_error (L, "date takes one options table");

  dt = read_instant (L, 1);
  year = g_date_time_get_year (dt);
  month = g_date_time_get_month (dt);
  day = g_date_time_get_day_of_month (dt);
  hour = g_date_time_get_hour (dt);
  minute = g_date_time_get_minute (dt);
  second = g_date_time_get_second (dt);
  micro = g_date_time_get_microsecond (dt);
  weekday = g_date_time_get_day_of_week (dt);
  yearday = g_date_time_get_day_of_year (dt);
  offset = g_date_time_get_utc_offset (dt) / G_USEC_PER_SEC;
  dst = g_date_time_is_daylight_savings (dt);
  /* interned: the Lua calls below may raise (memory limit), nothing may be
   * owned then */
  abbreviation = g_intern_string (g_date_time_get_timezone_abbreviation (dt));
  g_date_time_unref (dt);

  lua_createtable (L, 0, 12);
  set_integer_field (L, "year", year);
  set_integer_field (L, "month", month);
  set_integer_field (L, "day", day);
  set_integer_field (L, "hour", hour);
  set_integer_field (L, "minute", minute);
  set_integer_field (L, "second", second);
  set_integer_field (L, "microsecond", micro);
  set_integer_field (L, "weekday", weekday);
  set_integer_field (L, "yearday", yearday);
  lua_pushstring (L, abbreviation != NULL ? abbreviation : "");
  lua_setfield (L, -2, "timezone");
  set_integer_field (L, "offset", offset);
  lua_pushboolean (L, dst);
  lua_setfield (L, -2, "dst");

  return 1;
}

/* ------------------------------------------------------------------------ */
/* timestamp                                                                 */
/* ------------------------------------------------------------------------ */

static const char *const timestamp_fields[] = {
  "year", "month", "day", "hour", "minute", "second", "microsecond",
  /* the other fields of a date () result: accepted, derived */
  "weekday", "yearday", "timezone", "dst",
  /* disambiguates the repeated hour of a clock change */
  "offset",
  "utc",
  NULL
};

static lua_Integer
get_integer_field (lua_State  *L,
                   int         tbl,
                   const char *name,
                   lua_Integer min,
                   lua_Integer max,
                   lua_Integer def,
                   gboolean    required)
{
  lua_Integer value;
  int isnum;

  if (lua_getfield (L, tbl, name) == LUA_TNIL)
    {
      lua_pop (L, 1);
      if (required)
        luaL_error (L, "missing required field '%s'", name);
      return def;
    }

  if (lua_type (L, -1) != LUA_TNUMBER)
    luaL_error (L, "field '%s' must be an integer, got %s", name, luaL_typename (L, -1));

  value = lua_tointegerx (L, -1, &isnum);
  if (!isnum)
    luaL_error (L, "field '%s' must be an integer (number has no integer representation)", name);
  lua_pop (L, 1);

  if (value < min || value > max)
    luaL_error (L, "field '%s' is out of range (%I to %I)", name, min, max);

  return value;
}

/* Seconds since the epoch of the fields; FALSE when they name no instant.
 * No Lua API in here: the caller raises. */
static gboolean
fields_to_unix (gboolean     utc,
                gboolean     have_offset,
                lua_Integer  offset,
                lua_Integer  year,
                lua_Integer  month,
                lua_Integer  day,
                lua_Integer  hour,
                lua_Integer  minute,
                lua_Integer  second,
                gint64      *result,
                gboolean    *bad_day)
{
  g_autoptr(GDateTime) wall = NULL;

  *bad_day = FALSE;

  /* The fields read as UTC: fails for a day that the month does not have */
  wall = g_date_time_new_utc (year, month, day, hour, minute, second);
  if (wall == NULL)
    {
      *bad_day = TRUE;
      return FALSE;
    }

  if (utc)
    {
      *result = g_date_time_to_unix (wall);
      return TRUE;
    }
  else
    {
      g_autoptr(GTimeZone) tz = g_time_zone_new_local ();
      g_autoptr(GDateTime) local = NULL;

      /* A wall time that the clock goes through twice: the offset of the
       * `date` result says which instant it was, if it is one of them. */
      if (have_offset)
        {
          gint64 candidate = g_date_time_to_unix (wall) - offset;
          gint interval = g_time_zone_find_interval (tz, G_TIME_TYPE_UNIVERSAL, candidate);

          if (interval >= 0 && g_time_zone_get_offset (tz, interval) == offset)
            {
              *result = candidate;
              return TRUE;
            }
        }

      local = g_date_time_new (tz, year, month, day, hour, minute, second);
      if (local == NULL)
        return FALSE;
      *result = g_date_time_to_unix (local);
      return TRUE;
    }
}

static int
l_timestamp (lua_State *L)
{
  lua_Integer year, month, day, hour, minute, second, micro;
  gboolean utc, have_offset, bad_day;
  lua_Integer offset = 0;
  gint64 result = 0;

  luaL_checkstack (L, 4, "shell.time.timestamp");
  if (lua_gettop (L) != 1)
    return luaL_error (L, "timestamp takes one table");

  shell_lua_check_fields (L, 1, timestamp_fields);

  year = get_integer_field (L, 1, "year", 1, 9999, 0, TRUE);
  month = get_integer_field (L, 1, "month", 1, 12, 0, TRUE);
  day = get_integer_field (L, 1, "day", 1, 31, 0, TRUE);
  hour = get_integer_field (L, 1, "hour", 0, 23, 0, FALSE);
  minute = get_integer_field (L, 1, "minute", 0, 59, 0, FALSE);
  second = get_integer_field (L, 1, "second", 0, 59, 0, FALSE);
  micro = get_integer_field (L, 1, "microsecond", 0, 999999, 0, FALSE);
  utc = shell_lua_opt_boolean (L, 1, "utc", FALSE);

  have_offset = lua_getfield (L, 1, "offset") != LUA_TNIL;
  lua_pop (L, 1);
  if (have_offset)
    offset = get_integer_field (L, 1, "offset", G_MININT32, G_MAXINT32, 0, FALSE);

  if (!fields_to_unix (utc, have_offset, offset, year, month, day, hour, minute, second,
                       &result, &bad_day))
    {
      if (bad_day)
        return luaL_error (L, "field 'day' is out of range for the month (%I-%I-%I)",
                           year, month, day);
      return luaL_error (L, "the time is out of range (years 1 to 9999) in the local time zone");
    }

  if (micro == 0)
    lua_pushinteger (L, (lua_Integer) result);
  else
    lua_pushnumber (L, (lua_Number) result + (lua_Number) micro / 1e6);
  return 1;
}

/* ------------------------------------------------------------------------ */
/* format, timezone                                                          */
/* ------------------------------------------------------------------------ */

static void *
free_text (void   *ud,
           void   *ptr,
           size_t  osize,
           size_t  nsize)
{
  g_free (ptr);
  return NULL;
}

static int
l_format (lua_State *L)
{
  GDateTime *dt;
  char *text;
  const char *format;
  size_t length;

  luaL_checkstack (L, 4, "shell.time.format");
  if (lua_gettop (L) > 2)
    return luaL_error (L, "format takes a format string and an optional options table");

  if (lua_type (L, 1) != LUA_TSTRING)
    return luaL_error (L, "bad argument #1 to 'format' (string expected, got %s)",
                       luaL_typename (L, 1));

  format = lua_tolstring (L, 1, &length);
  if (length > MAX_FORMAT_LENGTH)
    return luaL_error (L, "format string is too long (limit %d bytes)", MAX_FORMAT_LENGTH);
  if (strlen (format) != length)
    return luaL_error (L, "invalid format string (embedded NUL)");
  /* g_date_time_format () asserts (a critical) on anything else */
  if (!g_utf8_validate (format, length, NULL))
    return luaL_error (L, "invalid format string (not valid UTF-8)");

  dt = read_instant (L, 2);
  text = g_date_time_format (dt, format);
  g_date_time_unref (dt);

  /* luaL_error () does not return: nothing owned may be live */
  if (text == NULL)
    return luaL_error (L, "invalid format string '%s'", format);

  /* Lua owns the text from here, also when it fails to make the string */
  lua_pushexternalstring (L, text, strlen (text), free_text, NULL);
  return 1;
}

static int
l_timezone (lua_State *L)
{
  /* interned: nothing owned while Lua may raise */
  const char *identifier;

  {
    g_autoptr(GTimeZone) tz = g_time_zone_new_local ();

    identifier = g_intern_string (g_time_zone_get_identifier (tz));
  }

  lua_pushstring (L, identifier != NULL ? identifier : "UTC");
  return 1;
}

static const luaL_Reg time_funcs[] = {
  { "now",       l_now },
  { "monotonic", l_monotonic },
  { "date",      l_date },
  { "timestamp", l_timestamp },
  { "format",    l_format },
  { "timezone",  l_timezone },
  { NULL, NULL }
};

void
shell_lua_api_time_register (ShellLuaState *state,
                             lua_State     *L)
{
  int shell = lua_absindex (L, -1);

  luaL_checkstack (L, 4, "shell time api");

  lua_newtable (L);
  luaL_setfuncs (L, time_funcs, 0);
  lua_setfield (L, shell, "time");
}

/* ------------------------------------------------------------------------ */
/* time.timezone-changed                                                     */
/* ------------------------------------------------------------------------ */

static ShellHookRegistry *watch_registry = NULL;
static GPtrArray *watch_monitors = NULL;
static char *watch_timezone = NULL;

static char *
current_timezone (void)
{
  g_autoptr(GTimeZone) tz = g_time_zone_new_local ();
  const char *identifier = g_time_zone_get_identifier (tz);

  return g_strdup (identifier != NULL ? identifier : "UTC");
}

static void
timezone_file_changed_cb (GFileMonitor      *monitor,
                          GFile             *file,
                          GFile             *other_file,
                          GFileMonitorEvent  event,
                          gpointer           user_data)
{
  g_autofree char *timezone = NULL;
  GVariantBuilder builder;

  if (watch_registry == NULL)
    return;

  timezone = current_timezone ();
  if (g_strcmp0 (timezone, watch_timezone) == 0)
    return;

  g_free (watch_timezone);
  watch_timezone = g_strdup (timezone);

  g_debug ("local time zone is now %s", timezone);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "timezone", g_variant_new_string (timezone));
  shell_hook_registry_emit_after (watch_registry, "time.timezone-changed",
                                  g_variant_builder_end (&builder));
}

void
shell_lua_time_watch_paths (ShellHookRegistry  *registry,
                            const char * const *paths)
{
  guint i;

  g_return_if_fail (SHELL_IS_HOOK_REGISTRY (registry));
  g_return_if_fail (paths != NULL);

  shell_hook_registry_declare (registry, "time.timezone-changed", "timezone:s", FALSE,
                               "The local time zone changed (/etc/localtime or /etc/timezone)");

  if (watch_registry != NULL)
    return;

  watch_registry = g_object_ref (registry);
  watch_timezone = current_timezone ();
  watch_monitors = g_ptr_array_new_with_free_func (g_object_unref);

  for (i = 0; paths[i] != NULL; i++)
    {
      g_autoptr(GFile) file = g_file_new_for_path (paths[i]);
      g_autoptr(GError) error = NULL;
      GFileMonitor *monitor;

      monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, &error);
      if (monitor == NULL)
        {
          g_debug ("not watching %s: %s", paths[i], error->message);
          continue;
        }

      g_signal_connect (monitor, "changed", G_CALLBACK (timezone_file_changed_cb), NULL);
      g_ptr_array_add (watch_monitors, monitor);
    }
}

void
shell_lua_time_watch (ShellHookRegistry *registry)
{
  static const char * const paths[] = { "/etc/localtime", "/etc/timezone", NULL };

  shell_lua_time_watch_paths (registry, paths);
}

void
shell_lua_time_unwatch (void)
{
  guint i;

  if (watch_monitors != NULL)
    {
      for (i = 0; i < watch_monitors->len; i++)
        {
          GFileMonitor *monitor = g_ptr_array_index (watch_monitors, i);

          g_signal_handlers_disconnect_by_func (monitor, timezone_file_changed_cb, NULL);
          g_file_monitor_cancel (monitor);
        }
    }

  g_clear_pointer (&watch_monitors, g_ptr_array_unref);
  g_clear_pointer (&watch_timezone, g_free);
  g_clear_object (&watch_registry);
}
