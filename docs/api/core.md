# `shell` core API (version 1, draft)

Normative for M1. Every `shell.*` constructor takes one table; unknown fields
raise an error naming the field. All callbacks run under the extension's
watchdog and error policy. Nothing here is available outside the `shell`
table; `require("shell")` returns the same table.

## `shell.api_version`
Table `{major = 1, minor = 0}`; read-only.

## `shell.extension`
Present only in extension states (nil in the inspector).
`uuid` (string), `dir` (string, absolute path), `metadata` (table, parsed
metadata.json, read-only copy), `capabilities` (sequence of strings).

## `shell.log`
`shell.log.debug(fmt, ...)`, `.info`, `.warning`, `.error`: `string.format`
semantics, routed to GLib logging with domain `shell-lua/<uuid>`. Also
`shell.log(fmt, ...)` as an alias of `info`.

## `shell.timer`
- `shell.timer.after{ms = n, callback = f}` and `shell.timer.every{ms = n, callback = f}`
  (also accept positional `(ms, f)`). Return a `shell.Timer` handle.
- `Timer:cancel()`, `Timer.active` (boolean, read-only).
- `every` stops when the callback returns `false` (same as GLib).
- Timers are owned by the extension: disabled on disable.
- Idle: `shell.timer.idle{callback = f}`.

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
