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

`shell.api_version` is `{major=1, minor=0}` behind a read-only proxy.
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
