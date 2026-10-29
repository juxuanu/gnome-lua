/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.time (docs/api/core.md, Since 1.1) and the time.timezone-changed hook.
 *
 * The local zone is set with TZ to POSIX rules, which GLib parses without a
 * time zone database: "CET-1CEST,M3.5.0,M10.5.0/3" is Central European Time,
 * summer time from the last Sunday of March 02:00 to the last Sunday of
 * October 03:00 (local), and the repeated hour of the clock change is
 * 02:00-03:00 on 2024-10-27.
 */

#include "test-utils.h"

#include <stdlib.h>

#include "shell-hooks.h"
#include "shell-lua-time.h"

#define CET "CET-1CEST,M3.5.0,M10.5.0/3"

static ShellLuaState *
new_state (void)
{
  return tu_state_new ("time", SHELL_LUA_STATE_PRIVILEGED);
}

/* ------------------------------------------------------------------------ */
/* Clocks                                                                    */
/* ------------------------------------------------------------------------ */

static void
test_clocks (void)
{
  ShellLuaState *s = new_state ();
  gint64 before = g_get_real_time ();
  g_autofree char *now = NULL;
  double value;

  now = tu_eval_str (s, "shell.time.now()");
  value = g_ascii_strtod (now, NULL);
  g_assert_cmpfloat (value, >=, (double) before / 1e6 - 0.001);
  g_assert_cmpfloat (value, <=, (double) g_get_real_time () / 1e6 + 0.001);
  tu_assert_eval_str (s, "math.type(shell.time.now())", "float");
  tu_assert_eval_str (s, "math.type(shell.time.monotonic())", "float");

  tu_do (s,
         "local a = shell.time.monotonic()\n"
         "local b = shell.time.monotonic()\n"
         "assert(b >= a, 'monotonic goes back')\n"
         "local n1 = shell.time.now()\n"
         "assert(shell.time.now() >= n1)\n");

  /* no argument is taken */
  tu_do (s, "shell.time.now(1, 2); shell.time.monotonic('x')");

  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* date                                                                      */
/* ------------------------------------------------------------------------ */

static void
test_date_utc (void)
{
  ShellLuaState *s = new_state ();

  /* the epoch: a Thursday */
  tu_do (s,
         "local d = shell.time.date{ at = 0, utc = true }\n"
         "assert(d.year == 1970 and d.month == 1 and d.day == 1, 'epoch date')\n"
         "assert(d.hour == 0 and d.minute == 0 and d.second == 0 and d.microsecond == 0)\n"
         "assert(d.weekday == 4, 'weekday ' .. d.weekday)\n"
         "assert(d.yearday == 1)\n"
         "assert(d.timezone == 'UTC' and d.offset == 0 and d.dst == false)\n"
         "for _, k in ipairs{'year','month','day','hour','minute','second','microsecond',"
         "'weekday','yearday','offset'} do\n"
         "  assert(math.type(d[k]) == 'integer', k)\n"
         "end\n");

  /* 2023-11-14 22:13:20 UTC, a Tuesday, day 318 */
  tu_do (s,
         "local d = shell.time.date{ at = 1700000000, utc = true }\n"
         "assert(d.year == 2023 and d.month == 11 and d.day == 14)\n"
         "assert(d.hour == 22 and d.minute == 13 and d.second == 20)\n"
         "assert(d.weekday == 2 and d.yearday == 318)\n");

  /* ISO weekdays: Sunday is 7, Monday is 1 */
  tu_do (s,
         "assert(shell.time.date{ at = 1700432000, utc = true }.weekday == 7)\n"
         "assert(shell.time.date{ at = 1700432000 + 86400, utc = true }.weekday == 1)\n");

  /* leap day, end of the year */
  tu_do (s,
         "local d = shell.time.date{ at = 951782400, utc = true }\n"
         "assert(d.year == 2000 and d.month == 2 and d.day == 29 and d.yearday == 60)\n"
         "d = shell.time.date{ at = 1704067199, utc = true }\n"
         "assert(d.year == 2023 and d.month == 12 and d.day == 31 and d.yearday == 365 and d.second == 59)\n");

  /* fractions are microseconds, before the epoch too */
  tu_do (s,
         "local d = shell.time.date{ at = 1700000000.25, utc = true }\n"
         "assert(d.second == 20 and d.microsecond == 250000, d.microsecond)\n"
         "d = shell.time.date{ at = -1.5, utc = true }\n"
         "assert(d.year == 1969 and d.second == 58 and d.microsecond == 500000)\n"
         "d = shell.time.date{ at = 0.9999996, utc = true }\n"
         "assert(d.second == 1 and d.microsecond == 0, 'rounds up to the next second')\n");

  /* the limits of GDateTime */
  tu_do (s,
         "local d = shell.time.date{ at = -62135596800, utc = true }\n"
         "assert(d.year == 1 and d.month == 1 and d.day == 1)\n"
         "d = shell.time.date{ at = 253402300799, utc = true }\n"
         "assert(d.year == 9999 and d.month == 12 and d.day == 31 and d.second == 59)\n");

  /* no argument, nil, an empty table: now, local */
  tu_do (s,
         "local a = shell.time.date()\n"
         "local b = shell.time.date(nil)\n"
         "local c = shell.time.date{}\n"
         "assert(a.year >= 2024 and b.year >= 2024 and c.year >= 2024)\n"
         "assert(shell.time.date{ utc = true }.timezone == 'UTC')\n");

  g_object_unref (s);
}

static void
test_date_local (void)
{
  ShellLuaState *s = new_state ();

  g_setenv ("TZ", CET, TRUE);
  tu_assert_eval_str (s, "shell.time.timezone()", CET);

  /* winter: 2023-11-14 22:13:20 UTC is 23:13:20 CET */
  tu_do (s,
         "local d = shell.time.date{ at = 1700000000 }\n"
         "assert(d.day == 14 and d.hour == 23 and d.minute == 13, d.hour)\n"
         "assert(d.timezone == 'CET' and d.offset == 3600 and d.dst == false)\n"
         "assert(d.weekday == 2 and d.yearday == 318)\n");

  /* summer: 2024-07-03 09:46:40 UTC is 11:46:40 CEST */
  tu_do (s,
         "local d = shell.time.date{ at = 1720000000 }\n"
         "assert(d.month == 7 and d.day == 3 and d.hour == 11 and d.minute == 46, d.hour)\n"
         "assert(d.timezone == 'CEST' and d.offset == 7200 and d.dst == true)\n"
         "local u = shell.time.date{ at = 1720000000, utc = true }\n"
         "assert(u.hour == 9 and u.offset == 0 and u.timezone == 'UTC' and u.dst == false)\n");

  /* the local day can be the next one */
  tu_do (s,
         "local d = shell.time.date{ at = 1700000000 + 3600 }\n"
         "assert(d.day == 15 and d.hour == 0, d.day)\n");

  g_unsetenv ("TZ");
  g_object_unref (s);
}

static void
test_date_errors (void)
{
  ShellLuaState *s = new_state ();

  tu_assert_fails (s, "shell.time.date{ at = 'now' }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'at' must be a number, got string");
  tu_assert_fails (s, "shell.time.date{ at = true }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'at' must be a number, got boolean");
  tu_assert_fails (s, "shell.time.date{ utc = 1 }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'utc' must be a boolean, got number");
  tu_assert_fails (s, "shell.time.date{ tz = 'UTC' }", SHELL_LUA_ERROR_RUNTIME,
                   "unknown field 'tz'");
  tu_assert_fails (s, "shell.time.date(5)", SHELL_LUA_ERROR_RUNTIME,
                   "options table expected, got number");
  tu_assert_fails (s, "shell.time.date('x')", SHELL_LUA_ERROR_RUNTIME,
                   "options table expected, got string");
  tu_assert_fails (s, "shell.time.date({}, {})", SHELL_LUA_ERROR_RUNTIME,
                   "takes one options table");
  tu_assert_fails (s, "shell.time.date{ at = 1e20 }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'at' is out of range");
  tu_assert_fails (s, "shell.time.date{ at = -1e20 }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'at' is out of range");
  tu_assert_fails (s, "shell.time.date{ at = 0/0 }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'at' is out of range");
  tu_assert_fails (s, "shell.time.date{ at = math.huge }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'at' is out of range");
  tu_assert_fails (s, "shell.time.date{ at = 253402300800, utc = true }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'at' is out of range");
  tu_assert_fails (s, "shell.time.date{ at = -62135596801, utc = true }", SHELL_LUA_ERROR_RUNTIME,
                   "field 'at' is out of range");

  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* timestamp                                                                 */
/* ------------------------------------------------------------------------ */

static void
test_timestamp_utc (void)
{
  ShellLuaState *s = new_state ();

  tu_assert_eval_str (s, "shell.time.timestamp{ year = 1970, month = 1, day = 1, utc = true }", "0");
  tu_assert_eval_str (s, "shell.time.timestamp{ year = 2023, month = 11, day = 14, "
                         "hour = 22, minute = 13, second = 20, utc = true }", "1700000000");
  tu_assert_eval_str (s, "math.type(shell.time.timestamp{ year = 2023, month = 11, day = 14, "
                         "utc = true })", "integer");
  tu_assert_eval_str (s, "shell.time.timestamp{ year = 1969, month = 12, day = 31, hour = 23, "
                         "minute = 59, second = 59, utc = true }", "-1");
  tu_assert_eval_str (s, "shell.time.timestamp{ year = 9999, month = 12, day = 31, hour = 23, "
                         "minute = 59, second = 59, utc = true }", "253402300799");
  /* the microseconds make it a float */
  tu_do (s,
         "local t = shell.time.timestamp{ year = 1970, month = 1, day = 1, second = 1, "
         "microsecond = 500000, utc = true }\n"
         "assert(math.type(t) == 'float' and t == 1.5, t)\n");

  /* round trips: the fields of date () give the instant back */
  tu_do (s,
         "for _, t in ipairs{ 0, 1, -1, 59, 86399, 86400, 951782400, 1700000000, 1720000000,\n"
         "                    1704067199, -62135596800, 253402300799, -2208988800 } do\n"
         "  local d = shell.time.date{ at = t, utc = true }\n"
         "  assert(shell.time.timestamp{ year = d.year, month = d.month, day = d.day, hour = d.hour,\n"
         "         minute = d.minute, second = d.second, utc = true } == t, 'utc ' .. t)\n"
         "  d.utc = true\n"
         "  assert(shell.time.timestamp(d) == t, 'date result + utc ' .. t)\n"
         "end\n"
         "for t = -400000, 400000, 3671 do\n"
         "  local d = shell.time.date{ at = t, utc = true }; d.utc = true\n"
         "  assert(shell.time.timestamp(d) == t, 'utc ' .. t)\n"
         "end\n"
         "local t = 1700000000.123456\n"
         "local d = shell.time.date{ at = t, utc = true }; d.utc = true\n"
         "assert(math.abs(shell.time.timestamp(d) - t) < 1e-6, 'fraction')\n"
         "assert(math.floor(shell.time.timestamp(d)) == math.floor(t))\n");

  g_object_unref (s);
}

static void
test_timestamp_local (void)
{
  ShellLuaState *s = new_state ();

  g_setenv ("TZ", CET, TRUE);

  /* 23:13:20 CET is 22:13:20 UTC */
  tu_assert_eval_str (s, "shell.time.timestamp{ year = 2023, month = 11, day = 14, "
                         "hour = 23, minute = 13, second = 20 }", "1700000000");
  /* 11:46:40 CEST is 09:46:40 UTC */
  tu_assert_eval_str (s, "shell.time.timestamp{ year = 2024, month = 7, day = 3, "
                         "hour = 11, minute = 46, second = 40 }", "1720000000");

  /* round trip of date (): every hour for two years, local */
  tu_do (s,
         "for t = 1672531200, 1735689600, 3600 + 1 do\n"
         "  local d = shell.time.date{ at = t }\n"
         "  assert(shell.time.timestamp(d) == t, 'local ' .. t .. ' ' .. d.hour .. ' ' .. d.timezone)\n"
         "end\n");

  /* the hour that the clock goes through twice: 2024-10-27 02:30 CEST, then CET.
   * The offset of the date () result says which one it was. */
  tu_do (s,
         "local first = shell.time.timestamp{ year = 2024, month = 10, day = 27, hour = 0, minute = 30, utc = true }\n"
         "local second = first + 3600\n"
         "local a = shell.time.date{ at = first }\n"
         "local b = shell.time.date{ at = second }\n"
         "assert(a.hour == 2 and a.minute == 30 and a.offset == 7200 and a.dst, 'first ' .. a.hour)\n"
         "assert(b.hour == 2 and b.minute == 30 and b.offset == 3600 and not b.dst, 'second ' .. b.hour)\n"
         "assert(shell.time.timestamp(a) == first, 'first instant')\n"
         "assert(shell.time.timestamp(b) == second, 'second instant')\n"
         "local plain = shell.time.timestamp{ year = 2024, month = 10, day = 27, hour = 2, minute = 30 }\n"
         "assert(plain == first or plain == second, 'without an offset one of them')\n"
         "-- an offset that the zone does not have at that time is ignored\n"
         "local c = shell.time.timestamp{ year = 2024, month = 7, day = 3, hour = 11, minute = 46, second = 40, offset = 3600 }\n"
         "assert(c == 1720000000, 'stale offset ' .. c)\n"
         "local e = shell.time.timestamp{ year = 2024, month = 7, day = 3, hour = 11, minute = 46, second = 40, offset = 7200 }\n"
         "assert(e == 1720000000)\n");

  /* the hour that does not exist: 2024-03-31 02:30 gives the end of the gap, as GLib does */
  tu_do (s,
         "local t = shell.time.timestamp{ year = 2024, month = 3, day = 31, hour = 2, minute = 30 }\n"
         "local d = shell.time.date{ at = t }\n"
         "assert(d.hour == 3 and d.dst, 'gap: ' .. d.hour .. ':' .. d.minute)\n");

  g_unsetenv ("TZ");
  g_object_unref (s);
}

static void
test_timestamp_errors (void)
{
  ShellLuaState *s = new_state ();

  tu_assert_fails (s, "shell.time.timestamp()", SHELL_LUA_ERROR_RUNTIME, "takes one table");
  tu_assert_fails (s, "shell.time.timestamp(1700000000)", SHELL_LUA_ERROR_RUNTIME,
                   "options table expected, got number");
  tu_assert_fails (s, "shell.time.timestamp{}", SHELL_LUA_ERROR_RUNTIME,
                   "missing required field 'year'");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020 }", SHELL_LUA_ERROR_RUNTIME,
                   "missing required field 'month'");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020, month = 1 }", SHELL_LUA_ERROR_RUNTIME,
                   "missing required field 'day'");
  tu_assert_fails (s, "shell.time.timestamp{ year = '2020', month = 1, day = 1 }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'year' must be an integer, got string");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020.5, month = 1, day = 1 }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'year' must be an integer");
  tu_assert_fails (s, "shell.time.timestamp{ year = 0, month = 1, day = 1 }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'year' is out of range");
  tu_assert_fails (s, "shell.time.timestamp{ year = 10000, month = 1, day = 1 }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'year' is out of range");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020, month = 13, day = 1 }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'month' is out of range");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020, month = 0, day = 1 }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'month' is out of range");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020, month = 1, day = 32 }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'day' is out of range");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2023, month = 2, day = 29, utc = true }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'day' is out of range for the month");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020, month = 4, day = 31 }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'day' is out of range for the month");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020, month = 1, day = 1, hour = 24 }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'hour' is out of range");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020, month = 1, day = 1, minute = 60 }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'minute' is out of range");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020, month = 1, day = 1, second = 60 }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'second' is out of range");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020, month = 1, day = 1, microsecond = 1000000 }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'microsecond' is out of range");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020, month = 1, day = 1, utc = 'yes' }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'utc' must be a boolean");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020, month = 1, day = 1, offset = 'x' }",
                   SHELL_LUA_ERROR_RUNTIME, "field 'offset' must be an integer");
  tu_assert_fails (s, "shell.time.timestamp{ year = 2020, month = 1, day = 1, week = 3 }",
                   SHELL_LUA_ERROR_RUNTIME, "unknown field 'week'");

  g_object_unref (s);
}

/* ------------------------------------------------------------------------ */
/* format, timezone                                                          */
/* ------------------------------------------------------------------------ */

static void
test_format (void)
{
  ShellLuaState *s = new_state ();

  tu_assert_eval_str (s, "shell.time.format('%Y-%m-%d %H:%M:%S', { at = 1700000000, utc = true })",
                      "2023-11-14 22:13:20");
  tu_assert_eval_str (s, "shell.time.format('%H:%M', { at = 3600 * 5 + 60 * 7, utc = true })", "05:07");
  tu_assert_eval_str (s, "shell.time.format('100%% %Z', { at = 0, utc = true })", "100% UTC");
  tu_assert_eval_str (s, "shell.time.format('no specifiers')", "no specifiers");
  tu_assert_eval_str (s, "shell.time.format('', { at = 0 })", "");
  tu_assert_eval_str (s, "shell.time.format('%f', { at = 1700000000.25, utc = true })", "250000");
  tu_assert_eval_str (s, "shell.time.format('é %Y', { at = 0, utc = true })", "é 1970");

  /* the same instant as date () */
  tu_do (s,
         "assert(shell.time.format('%Y') == tostring(shell.time.date().year))\n"
         "local d = shell.time.date()\n"
         "assert(shell.time.format('%m') == string.format('%02d', shell.time.date().month) or d.day == 1)\n");

  g_setenv ("TZ", CET, TRUE);
  tu_assert_eval_str (s, "shell.time.format('%H:%M %Z', { at = 1700000000 })", "23:13 CET");
  tu_assert_eval_str (s, "shell.time.format('%H:%M %Z', { at = 1700000000, utc = true })", "22:13 UTC");
  tu_assert_eval_str (s, "shell.time.format('%H %Z', { at = 1720000000 })", "11 CEST");
  g_unsetenv ("TZ");

  g_object_unref (s);
}

static void
test_format_errors (void)
{
  ShellLuaState *s = new_state ();

  tu_assert_fails (s, "shell.time.format()", SHELL_LUA_ERROR_RUNTIME, "string expected, got no value");
  tu_assert_fails (s, "shell.time.format(1)", SHELL_LUA_ERROR_RUNTIME, "string expected, got number");
  tu_assert_fails (s, "shell.time.format({})", SHELL_LUA_ERROR_RUNTIME, "string expected, got table");
  tu_assert_fails (s, "shell.time.format('%Q')", SHELL_LUA_ERROR_RUNTIME, "invalid format string '%Q'");
  tu_assert_fails (s, "shell.time.format('%Y %J')", SHELL_LUA_ERROR_RUNTIME, "invalid format string");
  tu_assert_fails (s, "shell.time.format('%-')", SHELL_LUA_ERROR_RUNTIME, "invalid format string");
  tu_assert_fails (s, "shell.time.format('%Y\\0%m')", SHELL_LUA_ERROR_RUNTIME, "embedded NUL");
  tu_assert_fails (s, "shell.time.format(string.rep('x', 1025))", SHELL_LUA_ERROR_RUNTIME,
                   "too long (limit 1024");
  /* GLib asserts on text that is not UTF-8: no critical may come from Lua */
  tu_assert_fails (s, "shell.time.format('\\xff\\xfe')", SHELL_LUA_ERROR_RUNTIME,
                   "not valid UTF-8");
  tu_assert_fails (s, "shell.time.format('%Y \\xc3')", SHELL_LUA_ERROR_RUNTIME,
                   "not valid UTF-8");
  tu_assert_fails (s, "shell.time.format('%Y', 5)", SHELL_LUA_ERROR_RUNTIME,
                   "options table expected, got number");
  tu_assert_fails (s, "shell.time.format('%Y', { at = 'x' })", SHELL_LUA_ERROR_RUNTIME,
                   "field 'at' must be a number");
  tu_assert_fails (s, "shell.time.format('%Y', { when = 1 })", SHELL_LUA_ERROR_RUNTIME,
                   "unknown field 'when'");
  tu_assert_fails (s, "shell.time.format('%Y', { at = 1e30 })", SHELL_LUA_ERROR_RUNTIME,
                   "field 'at' is out of range");
  tu_assert_fails (s, "shell.time.format('%Y', {}, 1)", SHELL_LUA_ERROR_RUNTIME,
                   "takes a format string");

  /* the longest format string still works */
  tu_do (s, "assert(#shell.time.format(string.rep('x', 1024)) == 1024)");

  g_object_unref (s);
}

/* Whatever is passed, a function of shell.time returns or raises: no crash, no
 * critical of GLib (this program runs with fatal warnings), nothing leaked
 * (AddressSanitizer in CI). */
static void
test_hostile_arguments (void)
{
  ShellLuaState *s = new_state ();

  tu_do (s,
         "local values = { 0, -1, 1.5, 2^53, -2^63, 2^63, 1e300, -1e300, 0/0, math.huge, -math.huge,\n"
         "  math.maxinteger, math.mininteger, '', 'x', '%Y', '%', '%%%%%%%%', string.rep('%Y', 600),\n"
         "  '\\xff', '\\0', true, false, {}, { at = 0/0 }, { at = 1e300, utc = true },\n"
         "  { at = math.maxinteger }, { at = math.mininteger, utc = true }, { utc = {} },\n"
         "  { year = math.maxinteger, month = 1, day = 1 }, { year = 1, month = 1, day = 1 },\n"
         "  { year = 9999, month = 12, day = 31, hour = 23, minute = 59, second = 59 },\n"
         "  { year = 1, month = 1, day = 1, offset = math.mininteger },\n"
         "  { year = 2020, month = 2, day = 30 }, { year = 2020, month = 1, day = 1, offset = 1e300 },\n"
         "  setmetatable({}, { __index = function() error('index') end }),\n"
         "  function() end, coroutine.create(function() end), shell.time }\n"
         "local n = 0\n"
         "for name, fn in pairs(shell.time) do\n"
         "  for _, a in ipairs(values) do\n"
         "    pcall(fn, a); n = n + 1\n"
         "    for _, b in ipairs(values) do pcall(fn, a, b); n = n + 1 end\n"
         "  end\n"
         "  pcall(fn); pcall(fn, nil, nil, nil)\n"
         "end\n"
         "assert(n > 1000)\n");

  g_object_unref (s);
}

static void
test_timezone (void)
{
  ShellLuaState *s = new_state ();

  g_setenv ("TZ", "Europe/Paris", TRUE);
  tu_assert_eval_str (s, "shell.time.timezone()", "Europe/Paris");
  g_setenv ("TZ", CET, TRUE);
  tu_assert_eval_str (s, "shell.time.timezone()", CET);
  g_unsetenv ("TZ");
  tu_assert_eval_true (s, "type(shell.time.timezone()) == 'string' and #shell.time.timezone() > 0");

  g_object_unref (s);
}

static void
test_module_shape (void)
{
  ShellLuaState *privileged = new_state ();
  ShellLuaState *sandboxed = tu_state_new ("shape@test", SHELL_LUA_STATE_SANDBOXED);
  ShellLuaState *states[] = { privileged, sandboxed };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (states); i++)
    {
      tu_assert_eval_str (states[i], "type(shell.time)", "table");
      tu_do (states[i],
             "for _, name in ipairs{ 'now', 'monotonic', 'date', 'timestamp', 'format', 'timezone' } do\n"
             "  assert(type(shell.time[name]) == 'function', name)\n"
             "end\n"
             "assert(shell.time.no_such_function == nil)\n");
    }

  g_object_unref (privileged);
  g_object_unref (sandboxed);
}

/* ------------------------------------------------------------------------ */
/* time.timezone-changed                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
  guint  calls;
  char  *last;
} Seen;

static ShellHookVerdict
seen_listener (const char     *name,
               ShellHookPhase  phase,
               GVariant       *payload,
               GVariantDict   *amend,
               gpointer        user_data)
{
  Seen *seen = user_data;
  const char *timezone = NULL;

  g_assert_cmpstr (name, ==, "time.timezone-changed");
  g_assert_true (g_variant_lookup (payload, "timezone", "&s", &timezone));
  g_free (seen->last);
  seen->last = g_strdup (timezone);
  seen->calls++;
  return SHELL_HOOK_VERDICT_CONTINUE;
}

static gboolean
seen_at_least_one (gpointer data)
{
  return ((Seen *) data)->calls >= 1;
}

static gboolean
seen_at_least_two (gpointer data)
{
  return ((Seen *) data)->calls >= 2;
}

static void
test_timezone_changed_hook (void)
{
  g_autoptr (ShellHookRegistry) registry = g_object_new (SHELL_TYPE_HOOK_REGISTRY, NULL);
  g_autofree char *dir = tu_tmpdir_new ();
  g_autofree char *file = g_build_filename (dir, "localtime", NULL);
  const char * const paths[] = { file, NULL };
  Seen seen = { 0, NULL };
  guint id;

  tu_write_file (dir, "localtime", "zone 1");
  g_setenv ("TZ", "UTC", TRUE);

  shell_lua_time_watch_paths (registry, paths);
  g_assert_true (shell_hook_registry_is_declared (registry, "time.timezone-changed"));
  g_assert_false (shell_hook_registry_is_cancellable (registry, "time.timezone-changed"));
  g_assert_cmpstr (shell_hook_registry_get_schema (registry, "time.timezone-changed"), ==, "timezone:s");

  id = shell_hook_registry_connect (registry, "time.timezone-changed", SHELL_HOOK_PHASE_AFTER,
                                    seen_listener, &seen, NULL);
  g_assert_cmpuint (id, !=, 0);

  /* the file changes but the zone is the same: no emission */
  tu_write_file (dir, "localtime", "zone 1b");
  tu_spin (300);
  g_assert_cmpuint (seen.calls, ==, 0);

  /* the zone changes and the file with it */
  g_setenv ("TZ", CET, TRUE);
  tu_write_file (dir, "localtime", "zone 2");
  g_assert_true (tu_wait_for (seen_at_least_one, &seen, 5000));
  g_assert_cmpuint (seen.calls, ==, 1);
  g_assert_cmpstr (seen.last, ==, CET);

  /* nothing new: nothing emitted, however often the file is touched */
  tu_write_file (dir, "localtime", "zone 2b");
  tu_spin (300);
  g_assert_cmpuint (seen.calls, ==, 1);

  /* and back */
  g_setenv ("TZ", "UTC", TRUE);
  tu_write_file (dir, "localtime", "zone 3");
  g_assert_true (tu_wait_for (seen_at_least_two, &seen, 5000));
  g_assert_cmpstr (seen.last, ==, "UTC");

  /* stopped: no more emissions */
  shell_lua_time_unwatch ();
  g_setenv ("TZ", CET, TRUE);
  tu_write_file (dir, "localtime", "zone 4");
  tu_spin (300);
  g_assert_cmpuint (seen.calls, ==, 2);

  /* watching again works, and declaring again is harmless */
  shell_lua_time_watch_paths (registry, paths);
  shell_lua_time_watch_paths (registry, paths);
  shell_lua_time_unwatch ();
  shell_lua_time_unwatch ();

  shell_hook_registry_disconnect (registry, id);
  g_unsetenv ("TZ");
  g_free (seen.last);
  tu_rm_rf (dir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/shell-lua/api-time/module", test_module_shape);
  g_test_add_func ("/shell-lua/api-time/clocks", test_clocks);
  g_test_add_func ("/shell-lua/api-time/date/utc", test_date_utc);
  g_test_add_func ("/shell-lua/api-time/date/local", test_date_local);
  g_test_add_func ("/shell-lua/api-time/date/errors", test_date_errors);
  g_test_add_func ("/shell-lua/api-time/timestamp/utc", test_timestamp_utc);
  g_test_add_func ("/shell-lua/api-time/timestamp/local", test_timestamp_local);
  g_test_add_func ("/shell-lua/api-time/timestamp/errors", test_timestamp_errors);
  g_test_add_func ("/shell-lua/api-time/format", test_format);
  g_test_add_func ("/shell-lua/api-time/format/errors", test_format_errors);
  g_test_add_func ("/shell-lua/api-time/timezone", test_timezone);
  g_test_add_func ("/shell-lua/api-time/hostile-arguments", test_hostile_arguments);
  g_test_add_func ("/shell-lua/api-time/hook/timezone-changed", test_timezone_changed_hook);

  return g_test_run ();
}
