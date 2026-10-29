# `shell.spawn`: running programs (version 1.1; capability `spawn`)

Scope: starting a program without a shell, feeding it standard input, and
collecting its exit status and output, without ever blocking the shell.
Replaces `Gio.Subprocess`/`GLib.spawn_*` use in extensions. Everything in this
document is new in 1.1: `shell.spawn`, every option and result field below,
and the type `shell.Process`.

`shell.spawn` requires `"spawn"` in `metadata.json` `capabilities`; without it
the call raises `shell.spawn() requires the "spawn" capability`, before the
options are even looked at and before anything runs. **`spawn` is the
broadest capability of the API**: the program runs as the user, with the
shell's environment, and can do everything the user can do. The extensions
app shows it as such; it is not a narrow permission.

Status: implemented by `subprojects/shell-lua/src/shell-lua-api-spawn.c` (GLib
only); unit tests `subprojects/shell-lua/tests/test-api-spawn.c`, conformance
test `tests/api/spawn.lua` with the fixtures `tests/api/fixtures/spawn@test`
and `spawn-nocap@test`.

Not in v1.1:
- Waiting for a process (every outcome comes through `on_exit`), streaming
  output while the process runs, interactive standard input, pseudo
  terminals, process groups and sessions, resource limits, and changing the
  user.
- A shell: `argv` is executed directly. To use pipes, redirections or
  expansion run `{ "/bin/sh", "-c", script }` and quote what you put in it.
- Detaching: a process that is still running when the extension is disabled
  is killed. Use `systemd-run --user` (or `gtk-launch`, `shell.apps.launch`)
  for something that must outlive the shell.

## `shell.spawn{...}`

| Field | Type | Notes |
|---|---|---|
| `argv` | sequence of strings, required | the program and its arguments; at least one element; the program is looked up in `PATH` when it has no `/`. No element may contain a NUL byte |
| `cwd` | string | working directory; default: the shell's |
| `env` | table `NAME = "value"` | added to (replacing the same names in) the environment of the shell; `NAME` is non-empty and has no `=`; values are strings |
| `stdin` | string | written to the process's standard input (at most 16 MiB), which is then closed. Without it standard input is `/dev/null` |
| `timeout_ms` | integer 1..86400000 | when the process runs longer it is sent `SIGTERM`, and `SIGKILL` 2 seconds later when it is still there; `result.timed_out` is `true` |
| `max_output` | integer 0..67108864, default 1048576 | bytes kept of each of standard output and standard error; the rest is read and thrown away and `result.truncated` is `true` |
| `on_exit` | function(result) | called once when the process has ended and its output is complete. Without it the output is discarded (not even read into memory) |

Returns a `shell.Process`. The program is started before the call returns, so
a failure to start (no such program, not executable, bad `cwd`) is
**environmental** and the call returns `nil, message` (README, rule 5)
without calling `on_exit`. A process that starts and fails is not an error of
this function: its status is in the result.

**`result`**:

| Field | Type | Notes |
|---|---|---|
| `status` | integer or `nil` | the exit code; `nil` when the process was killed by a signal |
| `signal` | string or `nil` | the name of that signal (`"SIGTERM"`, `"SIGKILL"`, `"SIGSEGV"`, ...; `"SIG<n>"` for an unnamed one); `nil` when it exited |
| `stdout`, `stderr` | string | the raw bytes (possibly empty, possibly not UTF-8) |
| `timed_out` | boolean | `timeout_ms` expired |
| `truncated` | boolean | `max_output` was exceeded on either stream |

A process that closes its output but keeps running is not ended until it
exits. When the process has exited but a descendant it started still holds its
output open, the result is delivered 500 ms after the exit, with whatever was
read until then.

`on_exit` runs on the main loop, under the watchdog and the usual error
policy, never after the extension is disabled. It is the only callback.

## `shell.Process`

| Member | Notes |
|---|---|
| `pid` | integer, read-only: the process id (raises when the process has ended, like any property of a dead handle) |
| `alive` | `true` while the process runs; `false` from the moment its result is ready (also inside `on_exit`) |
| `kill()` | sends `SIGTERM` (the process may ignore it); does nothing when it has ended. The result still comes through `on_exit` |
| `destroy()` | `SIGTERM`, `SIGKILL` 2 s later if needed, and forget: `on_exit` is not called afterwards. Idempotent |

The handle is owned by the extension and counts against the cap of owned
objects only while the process runs. At disable every running process is
killed in the way of `destroy()` and no callback runs.

## Example

```lua
local shell = require("shell")   -- metadata: "capabilities": ["spawn"]

local label = shell.ui.label{ text = "…", y_align = "center" }
shell.panel.add_indicator{ child = label, accessible_name = "Uptime" }

local function refresh()
  local proc, err = shell.spawn{
    argv = { "uptime", "-p" },
    timeout_ms = 3000,
    on_exit = function(r)
      if r.status == 0 then
        label.text = (r.stdout:gsub("%s+$", ""))
      else
        shell.log.warning("uptime: %s", r.timed_out and "timed out" or r.stderr)
      end
    end,
  }
  if not proc then shell.log.warning("cannot run uptime: %s", err) end
end

refresh()
shell.timer.every{ ms = 60000, callback = refresh }

-- feeding standard input, the long way round
shell.spawn{ argv = { "/bin/sh", "-c", "tr a-z A-Z" }, stdin = "hello",
             on_exit = function(r) shell.log.info("%s", r.stdout) end }  -- HELLO
```
