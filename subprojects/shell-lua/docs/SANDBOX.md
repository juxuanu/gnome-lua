# Sandbox, limits and error policy

Applies to `ShellLuaState` of kind `SHELL_LUA_STATE_SANDBOXED` (extensions).
`SHELL_LUA_STATE_PRIVILEGED` states (Looking Glass, tests) get the full
standard library plus `shell`; the differences are listed at the end.

## What sandboxed code can use

Globals (nothing else exists):

| Name | Notes |
|---|---|
| `assert error ipairs next pairs pcall select tonumber tostring type rawequal rawget rawset rawlen _VERSION` | stock |
| `unpack` | `table.unpack` |
| `xpcall` | the handler runs after unwinding, not at the raise point (so it cannot inspect the stack and cannot defeat the watchdog) |
| `setmetatable` | stock, but a metatable with a `__gc` field is rejected |
| `getmetatable` | returns `nil` for strings, userdata and light userdata; for tables honours `__metatable` like the stock function |
| `collectgarbage` | only `"count"` (KiB, float); any other option raises |
| `print` | `g_message("[<state id>] ...")` |
| `require` | see below |
| `string table math utf8 coroutine` | per-state copies; `string.dump` removed. The string metatable's `__index` is the copy. |
| `_G` | the sandbox environment itself |
| `shell` | the API table; `require("shell")` returns the same table |

`shell.api_version` is `{major=1, minor=2}` behind a read-only proxy.

`shell.log.debug/info/warning/error/fatal` write to the GLib log with the
domain `shell-lua/<state id>`; `error` and `fatal` are `G_LOG_LEVEL_CRITICAL`
(`G_LOG_LEVEL_ERROR` would abort the process). Warnings and criticals written
this way are never fatal: the runtime lowers GLib's fatal mask around the call,
so `G_DEBUG=fatal-warnings` (CI) does not let an extension abort the shell with
its own log. `shell.log.fatal` also raises the message as a Lua error, which
counts against the error policy like any other. `shell.time` (the wall and
monotonic clocks, calendar fields, `GDateTime` formatting) is how a sandboxed
extension reads the time, since `os` does not exist; it needs no capability and
reaches nothing else.
API modules may add further globals (`_`, `ngettext`, ...) with
`lua_pushglobaltable()`, which returns the sandbox environment in sandboxed
states.

## What it cannot do

- No `os`, `io`, `debug`, `package`; no `load`, `loadstring`, `loadfile`,
  `dofile`; no C modules (`package.cpath` does not exist).
- Binary chunks are never accepted (all loads use mode `"t"`), so
  `string.dump` bytecode cannot be smuggled in.
- No `__gc` metamethods on tables (finalizers run with debug hooks disabled
  and would escape the watchdog).
- `require` cannot reach outside the registered search roots.
- Reaching the real globals, the original library tables or the registry is
  not possible from Lua: the sandbox environment replaces the globals table,
  and the libraries it exposes are copies.
- Not interruptible: a single long-running C library call, e.g.
  `string.find` with a pathological pattern or `table.sort` on a huge table.
  The watchdog is a VM instruction-count hook. Memory growth in such calls is
  still bounded by the memory limit.

**Files and processes (API 1.1).** The Lua standard library stays closed:
there is still no `io`, `os` or `package`. What an extension can do with
files and programs goes through the API, which checks it:

- `shell.fs` (docs/api/fs.md) without the `fs` capability reaches only the
  extension's own directory (read only) and its private data directory
  `$XDG_DATA_HOME/gnome-lua/<uuid>` (read and write). Every path is absolute,
  canonicalised, and, for an extension without the capability, resolved
  through the file system (symbolic links followed) before it is compared with
  those two directories; a path outside them raises
  `shell.fs.<function>() requires the "fs" capability` before any I/O. With
  the capability any path the shell process can reach is open, which is the
  whole home of the user. All data I/O is asynchronous, bounded
  (`max_bytes`, at most 64 MiB; at most 100000 entries in a listing) and
  cancelled at disable.
- `shell.spawn` (docs/api/spawn.md) needs the `spawn` capability and then runs
  any program as the user, with the environment of the shell. It is not a
  sandbox boundary: an extension with `spawn` has the rights of the user.
  `argv` is executed without a shell; output is bounded (`max_output`); a
  process still running at disable is killed (SIGTERM, SIGKILL after two
  seconds) and its callback never runs.

Both capabilities are shown to the user by the extensions app; the Lua state
itself is unchanged (same watchdog, same memory limit: the callbacks of these
modules run under both).

**Worker states (API 1.2).** `shell.worker` (docs/api/worker.md, capability
`worker`) runs a file of the extension in a state of kind
`SHELL_LUA_STATE_WORKER` in a thread of its own. It is this sandbox (the same
globals and copies of the libraries, the same closed doors, `require` for the
files of the extension directory) with a `shell` table that holds only
`api_version`, `log`, `time`, a read-only copy of `extension` and
`shell.worker.post`: no timers, settings, hooks, handles or gated modules,
since nothing of the main thread exists there. It has no time budget
(`callback_budget_us` unlimited: there is no frame to protect) and the memory
limit of `max_memory` (64 MiB by default, at most 128 MiB). Instead of the
watchdog the count hook checks a **stop flag**
(`shell_lua_state_request_stop ()`, the one call that another thread may
make on a state): once set, the hook raises `worker: stopped` and escalates
like the watchdog, and the pcall reports `STOPPED`. A worker state never
logs, never emits `error` and never touches the main thread: a failed pcall
is returned to the thread, which relays it; `print` and `shell.log` go
through a message sink to the main thread, which writes them. The thread
creates and closes the state; the main thread never waits for it.

Every chunk loaded through `shell_lua_state_load_*()` or `require()` gets the
sandbox environment as its `_ENV` upvalue. Modules of one state share that
environment (a global written by one module is visible to the others).

## Limits and defaults

`ShellLuaLimits` fields set to 0 take the default for the state kind.

| Limit | SANDBOXED | PRIVILEGED |
|---|---|---|
| `hook_interval` (VM instructions between watchdog checks) | 10000 | 10000 |
| `callback_budget_us` (wall clock per `shell_lua_state_pcall`) | 500000 (500 ms) | `G_MAXINT64` (unlimited) |
| `memory_limit` (bytes handed out by the allocator) | 64 MiB | `G_MAXSIZE` (unlimited) |
| nested `shell_lua_state_pcall` depth | 50 | 50 |

A `WORKER` state has the limits of `SANDBOXED` except `callback_budget_us`,
which is unlimited; `shell.worker.start` sets its `memory_limit`.

`G_MAXINT64` / `G_MAXSIZE` may be passed explicitly to disable a limit.

**Watchdog.** Each `pcall` pushes a deadline (`now + budget`); the count hook
raises `"watchdog: callback exceeded its time budget (N ms)"` once any
deadline on the state's stack has passed, so an outer budget also bounds
nested calls. After expiry the hook fires on every instruction until the
frame is unwound, so a script that catches the error with `pcall`,
`xpcall`, `coroutine.wrap` or a coroutine cannot keep running. The state stays
usable afterwards; the manager decides whether to disable the extension.

**Memory.** The allocator refuses requests that would exceed the limit; Lua
runs an emergency GC, then raises a memory error. After a memory error the
state runs a full GC. Memory used is available from
`shell_lua_state_get_memory_usage()`; process totals from
`shell_lua_get_total_memory()` / `shell_lua_get_state_count()`.

## require resolution

`require(name)`:

1. `name` must match `^[A-Za-z0-9_][A-Za-z0-9_.]*$`, have no empty component
   (no `..`, no leading or trailing `.`) and be at most 255 bytes. Anything
   else fails with `sandbox: invalid module name '<name>'`
   (`SHELL_LUA_ERROR_SANDBOX`). Slashes and `..` can therefore never reach
   the file system.
2. The state's private cache is consulted (`loaded[name]`). A module being
   loaded, or one whose load failed, raises
   `loop or previous error loading module '<name>'`.
3. `"shell"` returns the API table.
4. For each search root, in the order added: `<root>/a/b.lua`, then
   `<root>/a/b/init.lua` for `a.b`. Roots are `GFile`s, so
   `resource:///org/gnome/shell/lua` works. Missing files are skipped; other
   I/O errors raise.
5. The chunk name is `@<path or URI>`; the chunk is called with
   `(name, path)`. The first return value (or `true`) is cached and returned,
   together with the path.
6. Not found: `module '<name>' not found:` plus the list of tried files.

Symbolic links inside a root are followed; roots must be trusted
directories. Privileged states try the roots first and then fall back to the
stock `require` (standard searchers), and `package.loaded.shell` is set.

## Error policy

`shell_lua_state_pcall()` is the single entry for running Lua. It installs a
message handler that appends `luaL_traceback`, converts the error object to a
string (strings as is; other values via `__tostring`, else
`(error object is a <type> value)`), and maps it to a `GError` in
`SHELL_LUA_ERROR`:

| Code | When |
|---|---|
| `SYNTAX` | chunk failed to compile (load functions only; not counted) |
| `RUNTIME` | any other Lua error, including nesting deeper than 50 |
| `WATCHDOG` | the watchdog hook fired during this call |
| `MEMORY` | allocator refused / `not enough memory` |
| `SANDBOX` | `sandbox: ...` errors (invalid module name) |
| `DEAD` | the state was already destroyed, or was destroyed during the call |
| `STOPPED` | a worker state was asked to stop (`shell_lua_state_request_stop ()`) during the call |

Every failed pcall increments the state's error counter, logs the message
(`g_warning` for sandboxed states, `g_debug` for privileged ones) and emits
`"error"`. Whoever owns the state (the extension manager) applies the
disable-after-N-errors policy. The function and its arguments are always
consumed from the stack, success or failure. A Lua error never reaches the
panic handler from the API provided here; if one does, it is a bug in C code
that called into Lua outside a protected call and aborts via `g_error`.

`shell_lua_state_destroy()` called from inside one of the state's own
callbacks is deferred until the outermost pcall returns; the pcall then
reports `DEAD`. Teardown callbacks run LIFO before `lua_close()`;
`shell_lua_state_remove_teardown()` unregisters without calling.

`coroutine.wrap` and `coroutine.close` are sandbox-specific replacements: a coroutine whose budget expired is never closed (its `__close` handlers would run with the watchdog disabled), and `close()` on it re-raises the watchdog error.
