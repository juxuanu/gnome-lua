# `shell` core API (version 1)

Normative. Every `shell.*` constructor takes one table; unknown fields
raise an error naming the field. All callbacks run under the extension's
watchdog and error policy. Nothing here is available outside the `shell`
table; `require("shell")` returns the same table.

## `shell.api_version`
Table `{major = 1, minor = 1}`; read-only. The minor is 1 since API 1.1
(see "Since" below); `metadata.json` may declare `"shell-api": "1.1"`.

## `shell.extension`
Present only in extension states (nil in the inspector).
`uuid` (string), `dir` (string, absolute path), `metadata` (table, parsed
metadata.json, read-only copy), `capabilities` (sequence of strings),
`data_dir` (string, since 1.1: the private data directory
`$XDG_DATA_HOME/gnome-lua/<uuid>`, created on first read; see fs.md).

## `shell.log`
`shell.log.debug(fmt, ...)`, `.info`, `.warning`, `.error`, `.fatal`:
`string.format` semantics, routed to the GLib log with domain
`shell-lua/<uuid>` (the journal entry has `GLIB_DOMAIN=shell-lua/<uuid>`).
Also `shell.log(fmt, ...)` as an alias of `info`. A call without a string
message, or with a format that does not fit its arguments, raises.

| Function | GLib level |
|---|---|
| `shell.log.debug` | `G_LOG_LEVEL_DEBUG` (shown only when `G_MESSAGES_DEBUG` names the domain or is `all`) |
| `shell.log.info` | `G_LOG_LEVEL_INFO` |
| `shell.log.warning` | `G_LOG_LEVEL_WARNING` |
| `shell.log.error` | `G_LOG_LEVEL_CRITICAL` (`G_LOG_LEVEL_ERROR` aborts the process in GLib, so `error` is a critical) |
| `shell.log.fatal` (Since 1.1) | `G_LOG_LEVEL_CRITICAL`, then raises (below) |

**`warning`, `error` and `fatal` never abort the shell** (changed in 1.1: until
then they were fatal under the settings below). The shell can run with
`G_DEBUG=fatal-warnings` (CI, developers), which makes GLib abort on any
warning or critical in any domain. What an extension logs on purpose is exempt:
the runtime lowers GLib's fatal mask for the duration of the call. Warnings and
criticals of the shell itself stay fatal, and so do the messages of the runtime
about an extension (the traceback of an error in a callback is a warning of the
`ShellLua` domain).

`shell.log.fatal(fmt, ...)` (Since 1.1): logs like `error`, then raises a Lua
error whose value is the formatted message, as is. It stops the current
callback after saying why: the error counts against the error policy (see
"Errors") like any other, and the runtime logs it with its traceback. A
`pcall` around it catches it.

## `shell.timer`
- `shell.timer.after{ms = n, callback = f}` and `shell.timer.every{ms = n, callback = f}`
  (also accept positional `(ms, f)`). Return a `shell.Timer` handle.
- `Timer:cancel()`, `Timer.active` (boolean, read-only).
- `every` stops when the callback returns `false` (same as GLib).
- Timers are owned by the extension: disabled on disable.
- Idle: `shell.timer.idle{callback = f}`.

## `shell.time` (Since 1.1)
Wall-clock and calendar helpers (extensions have no `os` or `io`). No
capability. Times are numbers of seconds since the Unix epoch (UTC), with a
fraction; the local time zone is the shell's (`TZ`, else `/etc/localtime`).

- `shell.time.now()` returns the wall clock, `g_get_real_time () / 1e6`.
- `shell.time.monotonic()` returns a clock that never goes back and is
  unaffected by changes of the system time, `g_get_monotonic_time () / 1e6`; its
  origin is unspecified, use it for durations.
- `shell.time.date(opts?)` returns a table describing an instant in the local
  zone (or in UTC). `opts` is nil or a table `{ at = seconds, utc = boolean }`:
  `at` defaults to now, `utc` to `false`. Fields of the result:

  | Field | Type | Meaning |
  |---|---|---|
  | `year` | integer | 1..9999 |
  | `month` | integer | 1..12 |
  | `day` | integer | 1..31 |
  | `hour` | integer | 0..23 |
  | `minute` | integer | 0..59 |
  | `second` | integer | 0..59 |
  | `microsecond` | integer | 0..999999 (the fraction of `at`, rounded to the microsecond) |
  | `weekday` | integer | ISO: 1 = Monday .. 7 = Sunday |
  | `yearday` | integer | 1..366 |
  | `timezone` | string | abbreviation in effect, such as `"CET"`, `"UTC"` |
  | `offset` | integer | seconds east of UTC in effect |
  | `dst` | boolean | daylight saving time in effect |

  `at` outside the years 1..9999 raises `field 'at' is out of range`.
- `shell.time.timestamp(t)` is the inverse of `date`: `t` is a table with
  `year`, `month`, `day` (required) and `hour`, `minute`, `second`,
  `microsecond` (default 0), read as local time, or as UTC when `t.utc` is
  `true`. The other fields of a `date` result (`weekday`, `yearday`,
  `timezone`, `dst`) are accepted and ignored; `offset` selects between the
  two instants of the hour that a clock change repeats (it is used only when
  it is the offset that zone has at the instant it names). Returns the
  seconds as an integer when `microsecond` is 0, a float otherwise, so
  `timestamp(date{ at = t })` equals `t` to the microsecond. A missing required
  field raises `missing required field '<name>'`; a field that is out of range
  raises `field '<name>' is out of range (<min> to <max>)`, and a day that
  the month does not have `field 'day' is out of range for the month`. A wall time that a clock change skips
  (02:30 on the night the clocks go forward from 02:00 to 03:00) gives the
  instant at the end of the skipped interval, as GLib's `g_date_time_new`
  does (03:00).
- `shell.time.format(fmt, opts?)` returns `fmt` formatted with GLib's
  `g_date_time_format` (`%H:%M`, `%Y-%m-%d`, `%x`, `%A`, `%B`...; locale
  dependent specifiers use the shell's locale). `opts` is `{ at =, utc = }` as
  for `date`. Raises `invalid format string` when GLib refuses it, and when
  `fmt` is longer than 1024 bytes, contains a NUL or is not valid UTF-8.
- `shell.time.timezone()` returns the identifier of the local zone as GLib
  names it (`"Europe/Paris"`, `"Etc/UTC"`).

Option tables follow the conventions of README.md: an unknown field raises
`unknown field '<name>'`, a field of the wrong type raises an error naming it.

Hook: `time.timezone-changed` (hooks.md) reports a change of the local zone;
`shell.time.timezone()` and `date` give the new zone from then on.

## `shell.settings`
- `shell.settings.extension()` returns the `shell.Settings` for
  `metadata["settings-schema"]` (error if absent).
- `shell.settings.open(schema_id)` is capability-gated (`"settings"`): any
  installed schema.
- `shell.Settings` methods: `get(key)` / `set(key, value)` (GVariant mapping,
  see `docs/api/variant.md`), `get_boolean/get_int/get_double/get_string/
  get_strv` and the `set_*` twins, `reset(key)`, `keys()`,
  `on_changed(key_or_nil, fn)` returning a `shell.Connection` with `:disconnect()`.
  Connections are owned by the extension.

## `shell.gettext`
`shell.gettext(msgid)`, `shell.ngettext(msgid, plural, n)`, `shell.pgettext(ctx, msgid)`
bound to the extension's `locale/` dir with domain `metadata["gettext-domain"]`
(default uuid). Also installed as `_`, `ngettext`, `pgettext` globals in the
sandbox.

## `shell.hooks`
- `shell.hooks.on(name, fn)` connects to the **after** phase; returns `shell.Connection`.
- `shell.hooks.before(name, fn)` connects to the **before** phase of a
  cancellable hook; `fn(payload)` may return `shell.hooks.CANCEL` or a table of
  amendments. Error if the hook is not cancellable.
- `fn` receives the payload as a table (GVariant a{sv} mapping).
- `shell.hooks.list()` → sequence of `{name=, cancellable=, doc=, schema=}`.
- `shell.hooks.emit_after(name, payload)` / `emit_before(name, payload)`:
  inspector only (sandboxed states get an error), for testing.
- Unknown hook name: error at connect time.

## Errors
A Lua error in any callback is logged with the traceback, the extension's
error count increments, and above the manager's limit the extension is
disabled with state ERROR. The shell never crashes because of Lua.
