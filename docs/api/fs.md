# `shell.fs`: files (version 1.1; capability `fs` outside the extension's own directories)

Scope: reading, writing, listing, removing and watching files without ever
blocking the shell, and a private data directory for every extension.
Replaces `Gio.File`/`GLib.file_get_contents` use in extensions. Everything in
this document is new in 1.1 (the "Since" column of the function table says it
once per entry point; the options, fields, events and error codes of an entry
point are new with it).

Status: implemented by `subprojects/shell-lua/src/shell-lua-api-fs.c` (GLib
only); unit tests `subprojects/shell-lua/tests/test-api-fs.c`, conformance test
`tests/api/fs.lua` with the fixtures `tests/api/fixtures/fs@test` and
`fs-nocap@test`.

Not in v1.1:
- Synchronous reads and writes (they would stall the compositor; the watchdog
  would kill the extension first): the data functions are callback-based.
  Only `exists` and `info` are synchronous (one `stat`).
- Relative paths, `~`, environment variables, URIs, globbing and recursive
  operations: every path is an absolute string of a local file.
- Creating directories outside the data directory, renaming, copying,
  permissions, links, mounts, and recursive watches.
- Streaming (reading in chunks, partial writes) and file descriptors. A file
  is read or written as a whole, bounded by `max_bytes`.

## Where an extension may go

| Directory | Without `fs` | With `fs` |
|---|---|---|
| `shell.extension.dir` (the extension's own files) | read | read and write |
| `shell.extension.data_dir` (private, see below) | read and write | read and write |
| any other path the shell process can reach | raises | read and write |

"Read" is `exists`, `info`, `read`, `list` and `watch`; "write" is `write` and
`remove`. A call that is not allowed raises
`shell.fs.<function>() requires the "fs" capability`. The check happens before
anything is read or written, and before the call can fail for any other
reason.

A path is **absolute**: a string starting with `/`, without a NUL byte. A
relative path, an empty string or a non-string raises (misuse), also with the
capability. Before the check the path is canonicalised: `.` and `..`
components and repeated slashes are resolved textually, then, for an
extension without `fs`, the longest existing prefix is resolved through the
file system (symbolic links followed) and the rest appended. So
`shell.extension.data_dir .. "/../../x"` and a link inside the data directory
that points elsewhere are both refused. The resolved path is the one used for
the operation, which also means that without `fs` `info` never reports
`"symlink"` for a path that goes through one. A directory that is replaced by
a link by another process between the check and the use is not defended
against.

### `shell.extension.data_dir`

A read-only string, available to every extension without a capability:
`$XDG_DATA_HOME/gnome-lua/<uuid>` (usually
`~/.local/share/gnome-lua/<uuid>`). The directory is created, mode `0700`,
the first time the field is read. Its content survives disable, shell restart
and upgrade of the extension; the runtime never deletes it. (The field is
resolved when it is read, so `pairs (shell.extension)` lists it only after
that.) If the directory cannot be created, the field is the same path and a
message is logged; operations on it then fail with `not-found` or
`permission-denied`.

## Functions

| Function | Returns | Since | Notes |
|---|---|---|---|
| `shell.fs.exists(path)` | boolean | 1.1 | follows links; `false` for anything that cannot be reached, errors included |
| `shell.fs.info(path)` | table, `nil`, or `nil, message` | 1.1 | see below |
| `shell.fs.read{...}` | `shell.Task` | 1.1 | |
| `shell.fs.write{...}` | `shell.Task` | 1.1 | |
| `shell.fs.list{...}` | `shell.Task` | 1.1 | |
| `shell.fs.remove{...}` | `shell.Task` | 1.1 | |
| `shell.fs.watch{...}` | `shell.FileWatch` | 1.1 | |
| `shell.extension.data_dir` | string (field) | 1.1 | no capability |

**`shell.fs.info(path)`** -> `{ type, size, modified }`: `type` is `"file"`,
`"directory"`, `"symlink"` (the link itself is described, not its target) or
`"special"` (devices, sockets, pipes); `size` an integer, bytes; `modified`
the modification time in seconds since the Unix epoch, with the fraction (a
number; compare with `shell.time.now ()`). Returns `nil`
when the path does not exist, and `nil, message` for any other failure (for
example a directory the shell may not enter).

### The async functions

Common to `read`, `write`, `list` and `remove`: all fields are in one options
table, `path` is required, and none of the work happens inside the call (the
callbacks run on a later main loop iteration, under the watchdog, with the
usual error policy). The returned `shell.Task` is a handle with
`cancel()` and `alive`: it is alive while the work is pending and dead once
the callback was scheduled to run, so a finished task does not count against
the cap of owned objects; `cancel()` on a finished task does nothing (it is
`DEAD_OK`). Pending tasks are cancelled when the extension is disabled and
their callbacks never run.

| Field | Type | Notes |
|---|---|---|
| `path` | string, required | absolute |
| `on_done` | function | arguments per function, see below; required for `read` and `list`, optional for `write` and `remove` |
| `on_error` | function(err) | `err = { message = string, code = string }`. Without `on_error` a failure is logged at message level and does **not** count against the extension's error policy |

`err.code` is one of `"not-found"`, `"permission-denied"`, `"too-large"`,
`"cancelled"`, `"other"` (new codes may be added in a minor version: treat
anything else as `"other"`). `cancel()` makes `on_error` run once, later, with
`"cancelled"` (never `on_done`), unless the task already finished.

| Function | Further fields | `on_done` receives | Behaviour |
|---|---|---|---|
| `read` | `max_bytes` (integer 1..67108864, default 16777216) | the contents, a string of raw bytes | `too-large` when the file has more than `max_bytes` bytes (nothing is returned, the memory is not kept). A directory is `other` |
| `write` | `contents` (string, required, at most 64 MiB), `append` (boolean, default `false`) | nothing | without `append` the file is replaced atomically (written to a temporary file and renamed over the target; the old content stays if anything fails). With `append` the bytes are added at the end, the file is created when absent. Missing parent directories are created (mode `0700`) when the path is inside `shell.extension.data_dir`, otherwise a missing parent is `not-found` |
| `list` | none | a sequence of `{ name = string, type = string }`, sorted by name (bytewise); `type` as in `info` (links are not followed). `.` and `..` are not listed | at most 100000 entries, more is `too-large` |
| `remove` | none | nothing | a file, a link (itself) or an **empty** directory; a directory with content is `other`; a missing path is `not-found` |

Operations on the same file are not ordered with respect to each other:
start the second one from the `on_done` of the first.

### `shell.fs.watch{...}`

| Field | Type | Notes |
|---|---|---|
| `path` | string, required | a file or a directory (not recursive); it does not have to exist yet |
| `on_changed` | function(event, path, other_path), required | data source: no handle argument (README rule 6) |

Returns a `shell.FileWatch`: `path` (ro), `alive`, `disconnect()` and
`destroy()` (the same, idempotent). The watch is owned by the extension and
removed when it is destroyed or the extension is disabled. `watch` returns
`nil, message` when the platform cannot watch the path.

`event` is one of:

| `event` | When | `path` |
|---|---|---|
| `"created"` | a file or directory appeared (also moved into a watched directory) | the new file |
| `"changed"` | the content of the file changed | the file |
| `"deleted"` | the file was removed (also moved out of a watched directory) | the file |
| `"renamed"` | the file was renamed inside the watched directory | the old path; `other_path` is the new one |

Changes of attributes (permissions, timestamps) and unmounts are not
reported. GLib merges bursts of `changed` events (rate limit 800 ms), so one
write can give one event that arrives a moment later, or two; a consumer
should read the file again rather than count events. A watch on a directory
reports the paths of the entries. Replacing a file atomically (as `write`
does) is reported as `created` or `renamed` for the entry, not `changed`.

## Example

```lua
local shell = require("shell")   -- no capability needed: the data directory is private

local file = shell.extension.data_dir .. "/state.json"
local state = { count = 0 }

shell.fs.read{ path = file,
  on_done = function(contents)
    local n = tonumber(contents:match('"count":(%d+)'))
    if n then state.count = n end
  end,
  on_error = function(err)
    if err.code ~= "not-found" then shell.log.warning("state: %s", err.message) end
  end }

local function save()
  shell.fs.write{ path = file, contents = ('{"count":%d}'):format(state.count),
                  on_error = function(err) shell.log.warning("save failed: %s", err.message) end }
end

shell.fs.watch{ path = shell.extension.data_dir, on_changed = function(event, path)
  shell.log.debug("%s %s", event, path)
end }
```
