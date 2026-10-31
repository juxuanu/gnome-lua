# `shell.worker`: work off the main thread (version 1.2; capability `worker`)

Scope: running expensive Lua code (parsing a large JSON or XML string,
searching, computing) outside the thread that composites the desktop. A
worker is a separate Lua state in a thread of its own, with no access to
shell objects, that exchanges **messages** (plain values) with the extension,
in the manner of a Web Worker. Everything in this document is **Since 1.2**: a
shell that reports `shell.api_version.minor < 2` has no `shell.worker`
(`shell.worker == nil`), and an extension that declares `"shell-api": "1.2"` in
its metadata can rely on it. It requires `"worker"` in `metadata.json`
`capabilities`; without it `shell.worker.start` raises
`shell.worker.start() requires the "worker" capability`, before the options
are looked at and before any thread is created.

Status: implemented by `subprojects/shell-lua/src/shell-lua-api-worker.c`
(GLib only; the worker's state is `SHELL_LUA_STATE_WORKER`, SANDBOX.md
"Worker states"); unit tests `subprojects/shell-lua/tests/test-api-worker.c`
and the `worker` group of `tests/test-state.c`; conformance test
`tests/api/worker.lua` with the fixtures `tests/api/fixtures/worker@test` and
`worker-nocap@test`; one more row and variant of the capability matrix
(`capabilities-worker@test`). The last section ("Notes for implementers") is
not part of the contract.

Why this shape, and not another: the main thread is the compositor, so any
Lua callback that runs long drops frames, and the watchdog stops it at 500 ms
anyway (SANDBOX.md). Running the whole engine in another thread was rejected:
the API wraps main-thread-only objects (Mutter, Clutter, St), every call would
round-trip, and the cancellable hooks need a synchronous verdict. A worker
has no such objects, so it needs no round trips and no verdicts; what it
needs is a way to get values in and out, and the GVariant mapping of
variant.md is already that.

Not in 1.2:
- Sharing anything: a worker has its own state, globals and memory. Values
  cross by copy, as GVariant (variant.md): functions, handles, coroutines,
  typed values (`shell.dbus.variant`) and `nil` cannot cross.
- A function as the worker's code: upvalues cannot cross states, and the
  sandbox has no `load`. A worker runs a **file** of the extension directory.
- Any `shell.*` module inside a worker other than `shell.log`, `shell.time`,
  `shell.extension` (a copy) and `shell.worker`: no `fs`, `network`, `spawn`,
  `dbus`, `settings`, `timer`, `hooks`, `ui`. They need a main loop, which a
  worker does not have; the extension does that work and posts the data. The
  other capabilities therefore mean nothing inside a worker today, and the
  `worker` capability grants nothing but threads.
- Waiting: the extension never waits for a worker, not at `post`, not at
  `terminate`, not at disable, not at shell exit. There is no `join`, no
  synchronous `call`, no promise.
- A time budget per message: a worker may run as long as it likes; what the
  runtime guarantees is that it can be **stopped**. A watchdog for workers
  would need a reason (there is no frame to protect).
- Transferring ownership (a buffer handed over without a copy), shared
  memory, workers starting workers, a pool that distributes messages among
  several workers, priorities between workers, a worker that outlives disable.
- A per-worker `require` root other than the extension directory, C modules,
  or loading code from a string.

## Model

```
extension state (main thread)                 worker state (its own thread)
 shell.worker.start{module=...}  ───────────▶  loads <dir>/<module>.lua
 worker:post(value)              ── inbox ──▶  on_message(value)
 on_message(value)               ◀── outbox ─  shell.worker.post(value) / return
 on_error(err)                   ◀───────────  error, end of the worker
 worker:terminate() / destroy()  ── stop ───▶  stop flag, checked by a count hook
```

Both queues carry GVariants. The extension side converts Lua → GVariant inside
`post` (so a value that cannot cross raises there, with the name of the
offending field) and GVariant → Lua just before `on_message` runs. The worker
side does the same in its thread. Nothing in a worker touches a GObject of the
shell, the GLib main context, or the Lua state of the extension.

A worker lives as long as the extension wants: it handles messages one at a
time, in order, and keeps its globals between them (so a worker that indexes a
large table once can answer many queries). A one-shot job is a worker that gets
one message and is destroyed from the `on_message` that brings its answer (see
the example).

## `shell.worker.start{...}`

Creates the thread, which loads and runs the module. Returns a `shell.Worker`
at once; no callback runs before `start` has returned, and none runs
synchronously inside `post`: every callback arrives from the main loop.

| Field | Type | Default | Since | Notes |
|---|---|---|---|---|
| `module` | string | required | 1.2 | the file that defines the worker, named as for `require` (SANDBOX.md: `"workers.parse"` is `<shell.extension.dir>/workers/parse.lua`, then `.../parse/init.lua`; same character rules, at most 255 bytes, so no `/` and no `..`). A name that breaks the rules raises `invalid module name '<name>'`; a file that does not exist is not an error of `start` but of the worker (`on_error`, code `"load"`) |
| `max_memory` | integer | 67108864 (64 MiB) | 1.2 | range 1048576..134217728 (1 MiB..128 MiB): bytes the worker's allocator may hand out. Beyond it the worker fails with `"memory"` |
| `on_message` | function(value) | nil | 1.2 | one value the worker posted. Data source: no handle argument (README rule 6). Without it the worker's messages are discarded |
| `on_error` | function(err) | nil | 1.2 | the worker ended: `err = { message = string, code = string }`, see "Errors". Runs at most once |

Argument errors raise before anything is created, naming the field (README
convention 1). Capacity limits:

- more than **4** live workers of one extension: `start` raises
  `too many workers (limit 4)` (misuse: a constraint of this document);
- more than the shell-wide limit of **max(8, 2 × CPUs)** live workers of all
  extensions together: `start` returns `nil, message` (environmental: it
  depends on other extensions, README rule 5). `shell.worker.limit` (below)
  tells how many a shell allows.

A worker is an owned object (README "Ownership and teardown") and counts
against the cap of owned objects while it is alive.

## `shell.Worker`

| Member | Type | Since | Notes |
|---|---|---|---|
| `worker:post(value)` | method | 1.2 | queues one value for the worker (see "Messages"). Exactly one argument; to send several values wrap them in a table. Raises when the value cannot cross, when it is too large, and when **64** messages are already queued: `too many queued messages (limit 64)`; the message is then not queued. Never waits |
| `worker:terminate()` | method | 1.2 | asks the worker to stop and forgets its queues: the handle is dead when `terminate` returns, messages the worker had not handled are dropped, messages the worker posted and the extension had not received are dropped, and `on_error` runs once, later, from the main loop, with `code = "terminated"` (never inside `terminate`). Nothing waits for the thread (see "Stopping a worker"). Does nothing on a dead handle |
| `worker:destroy()` | method | 1.2 | `terminate()` without the callback: **no** callback runs afterwards. Idempotent |
| `worker.alive` | boolean, ro | 1.2 | `true` from `start` until the worker ends; `false` inside `on_error`, after `terminate()` and `destroy()`, and at disable |
| `worker.queued` | integer, ro | 1.2 | messages posted with `post` that the worker has not started to handle. For backpressure: an extension that produces faster than its worker consumes looks at this before `post`, coalesces, or drops |

`shell.worker.limit` (integer, 1.2; no capability) is the shell-wide number
of workers (`max(8, 2 × CPUs)`), the same for every extension;
`shell.worker.start` is `nil` **inside** a worker and `shell.worker.post` is
`nil` in the extension (feature detection by presence, README).

## Inside a worker

The module file runs once, in the sandbox of SANDBOX.md ("What sandboxed code
can use": the base functions, `string`, `table`, `math`, `utf8`, `coroutine`,
`pcall`, `require` for the files of the extension directory, `print`), with the
same closed doors (no `os`, `io`, `load`, `__gc`, binary chunks). It must
return a table:

| Field | Type | Notes |
|---|---|---|
| `on_message` | function(value), required | runs once per message, in the order posted, never concurrently. A value it **returns** (other than `nil`) is posted to the extension as if by `shell.worker.post`, so a pure function is a complete worker. An error it raises ends the worker ("Errors") |

A file that does not exist, does not compile, raises while it runs or returns
something else ends the worker with `"load"`.

The `shell` table of a worker holds exactly:

| Member | Notes |
|---|---|
| `shell.api_version` | as in the extension |
| `shell.extension` | a read-only **copy** of `uuid`, `dir`, `metadata`, `capabilities` (plain data). No `data_dir` (nothing in a worker can use it) |
| `shell.log.debug/info/warning/error/fatal`, `shell.log(...)` | as core.md, same domain `shell-lua/<uuid>`. The line is written by the main thread when it gets to it, in order with the worker's messages (never ahead of a message posted before it, never behind one posted after it). `fatal` raises in the worker like anywhere else. `print` is relayed the same way |
| `shell.time.*` | all of it (core.md): clocks, calendar, formatting. No capability, no shell object |
| `shell.worker.post(value)` | queues one value for the extension's `on_message`. Allowed while the module file runs (a "ready" message) and inside `on_message`, any number of times (this is how progress is reported). When **64** messages are queued and not yet received, the call **waits** until there is room; it is the only call of the whole API that waits, and it waits in a worker, never on the main thread. A worker stopped while waiting is stopped there too |

A worker has the same `require` roots as the extension (its directory), so a
pure-Lua library (a JSON parser) is shared by putting it in the extension
directory and requiring it from both sides. Each worker loads its own copy.

What is **not** there: `shell.timer`, `shell.settings`, `shell.gettext` and
`_`, `shell.hooks`, `shell.ui`, `shell.panel`, everything that is a handle,
and every gated module. Reading them gives `nil` (absent functions are `nil`,
never stubs). `shell.test` is absent too.

## Messages

A message is one Lua value converted with the untyped mapping of variant.md
(boolean → `b`, integer → `x`, number → `d`, string → `s`, sequence → `av`,
other table → `a{sv}`), with one addition for workers: a **string that is not
valid UTF-8 crosses as `ay`** and arrives as the same bytes, so the raw
contents of a file (`shell.fs.read`) can be posted as they are. On arrival
every value is a plain Lua value: strings are strings, integers stay integers,
floats stay floats, sequences are 1-based tables, string-keyed tables are
tables. A message is delivered **at most once**, and in order with the others
of the same direction and the same worker; nothing is promised between two
workers.

What cannot cross raises in the sender (`post` in the extension, raising in
the worker's handler there), naming the field, as in variant.md:

| Value | Why |
|---|---|
| `nil` (also as the whole message) | ambiguous in GVariant; send `false` or an empty table |
| function, coroutine, userdata, handle, typed value (`shell.dbus.variant`) | not data |
| table with both integer and string keys, or keys of another type | neither `av` nor `a{sv}` (`mixes integer and string keys`; for messages only: the D-Bus conversion of variant.md keeps the string keys) |
| sparse sequence (`{1, nil, 3}`) | `field ... is a sparse array` |
| nesting deeper than 64 | `variant is nested too deeply` |
| cycles | they are deep nesting |
| a `shell.*` handle inside a table | not data |

Limits of one message, checked by the sender before it is queued:

| Limit | Value | On breach |
|---|---|---|
| serialised size | 16777216 bytes (16 MiB) | `message is too large (limit 16777216 bytes)` |
| values (every scalar and every table counts one) | 1000000 | `too many values (limit 1000000)`, raised as soon as the conversion passes the limit |

**Converting costs main-thread time.** The worker's result becomes Lua values
in the extension's state on the main thread, right before `on_message` runs;
that conversion is C and cannot be interrupted. A 16 MiB string is a copy (a
few milliseconds); a table of a million small values is a million table
stores (tens of milliseconds, a dropped frame). Hence the two limits, and
hence the advice: let the worker **digest** and post the small answer (counts,
the ten matches, the parsed subtree the extension shows), not the whole parse
tree. The runtime delivers at most **one message per main-loop dispatch and
worker** (at default priority), so a burst of results is spread over frames
rather than converted in one go, and each `on_message` runs under the usual
watchdog.

## Errors

`on_error(err)` receives `{ message = string, code = string }`. `message` is
for logs and is not promised (README: versioning); `code` is one of:

| `code` | When | Counts (SANDBOX.md error policy) |
|---|---|---|
| `"load"` | the module file was not found, did not compile, raised while it ran, or did not return a table with `on_message`. No message was handled | yes |
| `"error"` | `on_message` raised (the message carries the traceback), or `shell.log.fatal` was called, or a value it posted or returned cannot cross or is too large | yes |
| `"memory"` | the worker's allocator refused a request beyond `max_memory` | yes |
| `"terminated"` | `terminate()` was called | no |

**Any error ends the worker.** Its handle is dead inside `on_error`, the
messages it had not handled are dropped, and the extension decides whether to
start another one. Reasons: a handler that raised half-way may have left the
worker's globals inconsistent; the stopped-while-running case must end the
worker anyway, so one path serves all; and a worker that wants to survive its
own mistakes has `pcall`, which is cheaper than a policy in the runtime.

A worker's error is the extension's code failing: the runtime logs it with its
traceback as a warning of the `ShellLua` domain, as `[<uuid>] worker-<n>:
<message>` (`n` counts the workers the extension started since its enable;
the worker's own state is `<uuid>/worker-<n>` in tracebacks), and it
**counts against the extension's error policy** (more than 5 errors disable
the extension), whether or not `on_error` is set, as the error of any
callback does. `"terminated"` is neither logged nor counted. Without
`on_error` the end of a worker is otherwise silent.

Misuse on the extension side (a `post` with a value that cannot cross, on a
dead handle, or on a full queue; unknown fields of `start`) raises in the
calling callback like any misuse, and the worker is unaffected.

## Stopping a worker

`terminate()`, `destroy()` and disable set a **stop flag** that the worker's
state checks with a count hook every 10000 VM instructions (the same mechanism
as the watchdog, SANDBOX.md). When the flag is seen the worker raises
`worker: stopped` and, as the watchdog does, keeps raising at every instruction
until the thread is unwound, so a `pcall`, `xpcall` or coroutine in the worker
cannot keep it running. A worker waiting in `shell.worker.post` is woken and
stopped the same way. The thread then closes its state and exits on its own.

The main thread **never waits** for this: `terminate()` returns at once, the
handle is dead, and the thread finishes when it finishes. Two consequences an
extension can observe:

- `shell.worker.limit` and the per-extension limit of 4 count a worker until
  its thread has actually exited, so a `start` right after a `terminate()` of
  the fourth worker may raise `too many workers` for a moment. The
  extension re-tries from a timer (or keeps its workers).
- A single long C call (SANDBOX.md "Not interruptible": `string.rep` of a
  huge string, `table.sort`, a pathological `string.find`) is not stopped
  before it returns; the memory limit still bounds it. Nothing of this is
  visible to the desktop: the thread runs at reduced priority (below).

A worker thread runs at `nice` +10 (`setpriority` on the thread) with the
name `lua-worker`, so a busy worker yields to the compositor and to the user's
applications. At shell exit the threads are not joined: the process ends, and
with it the workers.

## Ownership and teardown

Workers are owned objects of the extension (README "Ownership and teardown").
At disable, after `disable()` has run and before the Lua state closes, every
worker of the extension is ended as by `destroy()`: stop flags set, queues
dropped, handles dead, no callback. `disable()` itself may still call
`terminate()` and `destroy()` (and `post`, which is pointless: nothing comes
back, since no callback runs once disable has begun). Nothing of a worker
survives disable except the thread that is still unwinding its last C call,
which no longer has a way to reach the extension.

Because delivery goes through the main loop and the callback machinery of the
runtime (`shell_lua_callback_emit_data`), the rules of README hold without
special cases: no callback runs after disable begins, `on_message` runs under
the watchdog and the error policy, and a callback that destroys its own
worker (`worker:destroy()` inside `on_message`) is fine: the handle is dead
when it returns, and the messages still queued are dropped.

## Time accounting

A separate change gives every extension counters of its main-thread
callbacks (how many ran, total time, the longest, with a log line for slow
ones). A worker's CPU time is **reported next to those, never added to
them**: it does not cost frames, which is the point. The thread measures its
own time with `CLOCK_THREAD_CPUTIME_ID` around each `on_message` and around
the module load, and the runtime keeps, per extension: messages handled,
total worker CPU time, the longest handler, and the Lua memory of its live
workers (each worker's allocator keeps an atomic counter of the extension up
to date, so a worker that grows in the middle of a message shows at once).
Looking Glass and `gnome-extensions info` show them after the callback
figures; the memory column of `gnome-extensions stats` counts the workers'
memory with the extension's. Nothing is logged for a slow handler: there is
no budget to exceed.

What a worker **does** cost the main thread is accounted where it is spent:
the conversion of an outgoing message inside `post` (a call made by a
callback that is already being timed), and the conversion of an incoming one,
which the instrumentation starts timing **before** the GVariant → Lua
conversion, so that `on_message` is charged for the message it receives.

## Example

A pure-Lua JSON library (`json.lua`) ships in the extension directory. The
worker parses whatever it is given and answers with what the panel shows:

```lua
-- workers/parse.lua: runs in the worker; nothing of the shell exists here
local json = require("json")

return {
  on_message = function(text)
    local t0 = shell.time.monotonic()
    local data = json.decode(text)             -- may take seconds: no frame is dropped
    local names = {}
    for i, item in ipairs(data.items) do
      if i > 10 then break end
      names[i] = item.name
    end
    shell.log.debug("parsed %d items in %.0f ms", #data.items,
                    (shell.time.monotonic() - t0) * 1000)
    return { count = #data.items, first = names }    -- small: cheap to convert
  end,
}
```

The extension reads the file and hands it to a one-shot worker:

```lua
-- extension.lua   metadata: "shell-api": "1.2", "capabilities": ["worker"]
local shell = require("shell")

local label = shell.ui.label{ text = "…", y_align = "center" }
shell.panel.add_indicator{ child = label, accessible_name = "Items" }

local worker

shell.fs.read{ path = shell.extension.data_dir .. "/catalog.json",
  on_done = function(contents)              -- raw bytes: they cross as they are
    if worker and worker.alive then worker:destroy() end
    worker = shell.worker.start{
      module = "workers.parse",
      on_message = function(result)
        label.text = ("%d items"):format(result.count)
        worker:destroy()                    -- the job is over; no on_error follows
      end,
      on_error = function(e)
        label.text = "error"
        shell.log.warning("parse worker: %s (%s)", e.message, e.code)
      end,
    }
    if not worker then                      -- the shell has no thread left for us
      label.text = "busy"
      return
    end
    worker:post(contents)                   -- 10 MiB: one copy, no parse on the main thread
  end,
  on_error = function(e) shell.log.warning("catalog: %s", e.message) end }
```

`terminate()` or `destroy()` right after `post` would drop the message: the
worker has not handled it yet. A one-shot worker is destroyed from the
`on_message` that brings its answer, as above.

A long-lived worker is the same code without the `destroy()`: post a
`{ op = "index", items = ... }` once, then `{ op = "query", text = ... }` as
the user types, and let `on_message` keep the module-level index between
calls.

## Notes for implementers (not part of the contract)

- The thread owns the worker's `lua_State` from creation to `lua_close ()`;
  the main thread reaches it through the two queues (under the worker's
  lock) and the stop flag only. Both sides reference-count the worker
  object; nothing joins the thread, which hands the slot of the limits back
  when it exits (`shell_lua_worker_get_live_count ()` for tests).
- The outbox is drained by a `GSource` of the default context, one item per
  dispatch; the thread wakes it with `g_source_set_ready_time ()` under the
  same lock that the dispatch takes, so a wakeup is never lost. Log lines
  (`shell.log`, `print`) travel through the same queue as the messages,
  which is what orders them.
- The callbacks are registry references kept by the module, as `shell.spawn`
  does, not callbacks bound to the owned object: `terminate ()` destroys the
  object at once and still runs `on_error` later.
- A conversion failure of a result inside the worker is an error of the
  handler; the conversion of an incoming message on the main thread runs
  inside the protected call of `on_message`, so the callback is charged for
  it and a failure of it is an error of the callback.
- `ShellLuaState` was made thread-aware for this: the stack of running
  states is thread-local, a `WORKER` state neither logs nor emits, its
  warnings buffer is per state, and the registry of API modules is used
  under a lock with a kinds mask (`shell_lua_api_register_for_kinds`), so the
  modules of the main thread never run in a worker.
