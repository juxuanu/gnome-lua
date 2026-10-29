# `shell.windows`, `shell.workspaces`, `shell.monitors`, `shell.apps` (version 1)

Scope: a read-mostly view of the desktop (which windows, workspaces and
monitors exist, which window is focused) with narrow actions on windows (five
in 1.0; in 1.1 also geometry, maximizing, fullscreen, always-on-top, sticking
and unminimizing), and the small `shell.apps` helper the other documents use to
launch applications. State changes arrive through hooks (hooks.md), whose
payloads carry the same ids and indices as these handles. Window management
itself (placement policy, tiling, keybinding-driven moves, window animations)
stays in the shell.

Items marked **Since 1.1** do not exist in a shell that reports
`shell.api_version.minor == 0`; an extension that declares `"shell-api": "1"`
in its metadata and wants them tests for them (`w.frame ~= nil`,
`w.move ~= nil`).

Status: implemented (M7, extended in 1.1) by `src/lua-api/shell-lua-api-window.c`;
the hooks by `src/lua-api/shell-window-hooks.c` (hooks.md), the replay of
`window.mapped` by `ShellHookRegistry` (`subprojects/shell-lua/src/shell-hooks.c`);
conformance test `tests/api/window.lua` with the fixture
`tests/api/fixtures/window@test`.

Not in v1 (1.0 and 1.1):
- Edge tiling and half-maximizing, maximizing on one axis, shading, window
  decorations, the client area or buffer rectangles (`frame` is the frame
  rectangle only), interactive moves and resizes (the pointer drag), snapping
  and constraints of your own: an extension that arranges windows is a window
  manager; the 1.1 actions place a window, they do not make Mutter follow a
  layout policy.
- Window actors, previews, screenshots, window contents or `Meta.Window`
  properties beyond the table below.
- Creating, removing, reordering or renaming workspaces (dynamic workspaces
  are managed by Mutter); changing the number of monitors or their layout.
- Per-window `before` hooks that veto closing, focusing or moving.
- Search/launch by `.desktop` file contents, app categories, app actions,
  starting arbitrary commands (that is the reserved `spawn` capability).
- Titles are exposed to every extension without a capability; this is a
  known privacy cost (browser tab titles) accepted in v1 and the first
  candidate for a gate.

## Indices and ids

Workspace and monitor numbers are **1-based** (README rule 3). The values
`Meta` uses (0-based) never appear. A window `id` is an integer assigned by
Mutter (`Meta.Window.get_id()`), unique and stable for the window's lifetime
and never reused in a session. `app_id` is the desktop file id as the window
tracker reports it (`org.gnome.Console.desktop`), nil for windows the tracker
cannot map or maps to no desktop file (a window-backed app).

## `shell.windows`

| Function | Returns |
|---|---|
| `shell.windows.list()` | sequence of `shell.Window`, most recently focused first (the Alt-Tab order). Includes minimized windows and windows on any workspace/monitor; excludes `skip_taskbar` windows (docks, desktop, the shell's own chrome), as the overview does (`src/ui/workspace.c` `is_overview_window`) |
| `shell.windows.focused()` | the focused `shell.Window`, or nil |
| `shell.windows.get(id)` | the `shell.Window` with that id, or nil (also nil for windows `list()` would exclude) |

## `shell.Window`

All properties are read-only snapshots of the live window; they change when the
window does.

| Property | Type | Since | Notes |
|---|---|---|---|
| `id` | integer | 1.0 | stable key; use it for your own tables |
| `title` | string | 1.0 | may be empty |
| `wm_class` | string | 1.0 | `WM_CLASS` class (X11) or the app id (Wayland), may be empty |
| `app_id` | string or nil | 1.0 | desktop file id |
| `workspace` | integer or nil | 1.0 | 1-based; nil when the window is on all workspaces |
| `monitor` | integer | 1.0 | 1-based, the monitor holding most of the window (the primary one while it has none yet) |
| `minimized` | boolean | 1.0 | |
| `maximized` | boolean | 1.0 | maximized on both axes |
| `fullscreen` | boolean | 1.0 | |
| `focused` | boolean | 1.0 | |
| `pid` | integer or nil | 1.0 | nil when unknown (many Wayland clients) |
| `frame` | table | 1.1 | `{x = , y = , width = , height = }`, integers in logical pixels and global (stage) coordinates: the *frame rectangle* of Mutter (`meta_window_get_frame_rect`: the window with its server-side decorations, without the client-side shadow). A new table on every read, so changing it changes nothing; use `move`, `resize` and `move_resize` to change the window. For a minimized window, the rectangle it has when shown |
| `above` | boolean | 1.1 | always on top (`meta_window_is_above`); true after `set_above(true)`, but also when the user chose "Always on Top" in the window menu or the client asked for it |
| `on_all_workspaces` | boolean | 1.1 | the window is shown on every workspace; it is exactly the case where `workspace` is nil |
| `resizable` | boolean | 1.1 | Mutter lets the window be resized (`meta_window_allows_resize`): false for fixed-size windows, maximized and fullscreen windows |
| `movable` | boolean | 1.1 | Mutter lets the window be moved (`meta_window_allows_move`): false for fullscreen and some special windows |
| `alive` | boolean | 1.0 | false once the window is destroyed; the handle then raises like any dead handle |

Actions (methods; each returns nothing, raises on misuse, and takes effect on
the next frame, so properties may still show the old value right after the
call; called from a listener of a `window.*` or `workspace.*` hook, an action
(of `shell.Workspace` too) is deferred to idle, README rule 8):

| Method | Since | Effect |
|---|---|---|
| `w:activate()` | 1.0 | `Main.activateWindow`: switches to the window's workspace if needed, unminimizes, raises, focuses; also hides the overview and closes the calendar |
| `w:minimize()` | 1.0 | minimize (no-op if already) |
| `w:close()` | 1.0 | polite close request (`Meta.Window.delete`); the app may refuse or ask; an unresponsive app gets the usual "Not responding" dialog |
| `w:move_to_workspace(n)` | 1.0 | `n` an integer in `1..#shell.workspaces.list()`; does not create workspaces (`change_workspace_by_index(n, append = false)`); raises when out of range |
| `w:move_to_monitor(n)` | 1.0 | `n` in `1..#shell.monitors.list()`; as in `moveWindowToMonitorAndWorkspace`, entering the primary monitor can change the window's workspace; a window that is on all workspaces only moves |
| `w:move{ x = , y = }` | 1.1 | moves the window so that the top-left corner of its frame is at (`x`, `y`) in global logical pixels (`meta_window_move_frame`, not a user operation) |
| `w:resize{ width = , height = }` | 1.1 | resizes the frame, keeping its top-left corner (`meta_window_move_resize_frame` with the current position) |
| `w:move_resize{ x = , y = , width = , height = }` | 1.1 | both at once |
| `w:maximize()` | 1.1 | maximizes on both axes (`meta_window_maximize`); no-op if already maximized |
| `w:unmaximize()` | 1.1 | restores a maximized window (`meta_window_unmaximize`); no-op if it is not maximized |
| `w:set_fullscreen(on)` | 1.1 | `on` a boolean: `meta_window_make_fullscreen` / `meta_window_unmake_fullscreen` |
| `w:set_above(on)` | 1.1 | `on` a boolean: always on top (`meta_window_make_above` / `meta_window_unmake_above`) |
| `w:set_on_all_workspaces(on)` | 1.1 | `on` a boolean: `meta_window_stick` / `meta_window_unstick` |
| `w:unminimize()` | 1.1 | shows a minimized window again without activating it (`meta_window_unminimize`); no-op if it is not minimized. `activate()` also focuses and raises it |

Rules of the 1.1 actions:

- The fields of `move`, `resize` and `move_resize` are all required (a missing
  or mistyped field, or an unknown one, raises an error naming the field).
  `x` and `y` are integers in `-65536..65536`, `width` and `height` integers in
  `1..65536`; anything else raises. The rectangle is in the frame coordinates
  of `frame`, not the monitor-relative ones of `monitors`: add the `x` and `y`
  of the monitor.
- The booleans must be booleans (`nil`, `0` or `"yes"` raise).
- **Mutter may ignore or adjust any of them**, without an error: a window
  that is not `resizable` keeps its size, a maximized or fullscreen window
  keeps its geometry, `move` is held back by the usual constraints (a window
  cannot be moved entirely off the screen), a client may pick another size than
  asked (minimum and maximum size, size increments), an application that does
  not allow fullscreen stays as it is. The actions never raise because Mutter
  refused; read `frame`, `maximized`, ... afterwards, or wait for
  `window.geometry-changed` and `window.state-changed`, to see what happened.
- Geometry actions do not change the maximized or fullscreen state of the
  window; maximize or fullscreen state set by the user stays until it is
  undone.
- Like the 1.0 actions they are deferred to idle when called from a listener
  of a `window.*` or `workspace.*` hook.

## `shell.workspaces`

| Function | Returns |
|---|---|
| `shell.workspaces.list()` | sequence of `shell.Workspace` in order |
| `shell.workspaces.count()` | `#list()` without building handles |
| `shell.workspaces.active()` | the active `shell.Workspace` |
| `shell.workspaces.get(n)` | handle or nil |

`shell.Workspace`: `index` (ro integer, 1-based; shifts when an earlier
workspace is removed), `active` (ro boolean), `n_windows` (ro integer),
`alive`. Methods: `ws:windows()` (sequence of `shell.Window`, same order and
filter as `windows.list()`), `ws:activate()` (switch to it).
With dynamic workspaces the last workspace is always empty
(`Meta.prefs_get_dynamic_workspaces()`): `count()` includes it.

## `shell.monitors`

Plain snapshot tables, not handles; call again after `monitors.changed`.

| Function | Returns |
|---|---|
| `shell.monitors.list()` | sequence ordered by monitor number |
| `shell.monitors.primary()` | the primary monitor table, or nil without monitors |
| `shell.monitors.get(n)` | table or nil |

A monitor table: `index` (1-based), `x`, `y`, `width`, `height` (logical
pixels, global coordinates, from `ShellMonitor` in `src/ui/layout.h`), `scale`
(`geometry_scale`, integer or fractional), `primary` (boolean),
`work_area` (`{x, y, width, height}`, the area not covered by the panel and
struts, `LayoutManager.getWorkAreaForMonitor`).

## `shell.apps`

| Function | Returns |
|---|---|
| `shell.apps.launch(app_id)` | `true`, or `nil, message` when the id is unknown or launching failed (environmental failure, README rule 5). Activates a running app's window like clicking its dash icon (an app that is still starting is left alone: `true`); does not hide the overview by itself |
| `shell.apps.running()` | sequence of app ids, most recently used first (windows that no desktop file explains have no id and are left out, as they are in `window.app_id`) |
| `shell.apps.favorites()` | sequence of app ids in dash order (`favorite-apps`) |
| `shell.apps.info(app_id)` | `{id = , name = , icon_name = , description = }` or nil; `icon_name` is the first name of a themed icon and absent for an icon that is a file, `description` is absent when the desktop file has none |

## Which hook carries which id

| Hook | Key | Matches |
|---|---|---|
| `window.mapped`, `.unmapped`, `.focused`, `.minimized`, `.unminimized`, `.title-changed`, `.workspace-changed`, `.monitor-changed`, `.state-changed`, `.geometry-changed` (1.1) | `window-id:t` | `Window.id`; resolve with `windows.get(id)` (nil for `unmapped`: the window is gone) |
| `window.focused` | `previous-window-id:t` | `Window.id` of the previously focused window, absent if none |
| `window.workspace-changed` | `from:i`, `to:i` | workspace numbers (absent = all workspaces) |
| `window.monitor-changed` | `from:i`, `to:i` | monitor numbers |
| `window.state-changed` | `maximized:b`, `fullscreen:b`, `changed:as`; since 1.1 also `above:b`, `on-all-workspaces:b` | `Window.maximized`, `.fullscreen`, `.above`, `.on_all_workspaces` after the change; `changed` lists `maximized`, `fullscreen`, and since 1.1 `above`, `on-all-workspaces` |
| `window.geometry-changed` (1.1) | `x:i`, `y:i`, `width:i`, `height:i` | `Window.frame` after the change |
| `workspace.switched`, `workspace.added`, `workspace.removed` | `from:i` / `to:i` / `index:i` | `Workspace.index` |
| `monitors.changed` | `count:i`, `primary:i` | `monitors.list()` |
| `dash.item-added`, `notification.*`, `panel.*` | `id`, `name` | see their documents |

Every `window.*` payload also carries the snapshot keys `title`, `wm-class`,
`app-id`, `workspace`, `monitor`, `pid` so a listener rarely needs a lookup.

### The windows that exist when a listener connects (1.1)

Windows that were already mapped when the shell's window manager started get
no `window.mapped`, and neither does a window that was mapped before an
extension was enabled. Since 1.1 the hook is **replayed to a new listener**:
when `shell.hooks.on("window.mapped", fn)` connects, `fn` is called once for
every window `shell.windows.list()` returns that has been mapped (a window
that is still being set up gets the live `window.mapped` when it is ready), in
the order of that list (most recently focused first), with the payload `window.mapped` would have carried (the
current values of the snapshot keys). So the first thing a new listener of
`window.mapped` gets is one event per existing window, and an extension that
wants "every window, now and later" needs a single listener and no
`shell.windows.list()` loop.

- The replay is never synchronous: the events are delivered from an idle after
  `shell.hooks.on` returned, to that listener only (other listeners of the hook
  see nothing). The windows are listed when the idle runs, not when the
  listener connected, so a window that closed in between is not replayed.
- A window that maps after `on` returned but before the idle runs is
  delivered once (by the replay), not twice.
- A listener that disconnects before the idle runs gets nothing.
- The replayed events are not emitted from inside a window transition, so an
  action called from such a listener (`w:close()`, ...) runs at once instead
  of being deferred to idle (README rule 8 does not apply: nothing is
  being animated).
- Only `window.mapped`, only the `after` phase of the connection made with
  `shell.hooks.on`. The other hooks (a `window.focused` for the window that
  has the focus now, ...) are not replayed: resolve the replayed event with
  `windows.get(id)` and read the state from the handle (`focused`,
  `minimized`, ...).
- The replay is a short delay after the connection, so a hook of another kind
  (a title change, say) can reach a listener of *that* hook before the
  replayed `window.mapped` reaches the listener of `window.mapped`; the
  ordering rule of hooks.md ("`window.mapped` precedes all other `window.*`
  hooks of a window") holds for the live emissions, not for what one extension
  sees across several connections made at once. Write the handlers to cope
  with a window id they have not seen (`windows.get(id)`).

## Example

```lua
local shell = require("shell")

local label = shell.ui.label{ text = "", y_align = "center" }
shell.panel.add_indicator{ child = label, accessible_name = "Focused window", position = "center", edge = "end" }

shell.hooks.on("window.focused", function(ev)
  label.text = ev.title ~= "" and ev.title or ev["wm-class"]
end)

shell.hooks.on("window.minimized", function(ev)
  local w = shell.windows.get(ev["window-id"])
  if w then shell.log.info("%s minimized on workspace %d", w.title, w.workspace or 0) end
end)
```

Since 1.1, a minimal "keep new windows on the left half" without being a
window manager (the `window.mapped` listener is also called for the windows
that exist when it connects):

```lua
shell.hooks.on("window.mapped", function(ev)
  local w = shell.windows.get(ev["window-id"])
  if not w or not w.resizable or w.maximized or w.fullscreen then return end
  local area = shell.monitors.get(w.monitor).work_area
  w:move_resize{ x = area.x, y = area.y, width = area.width // 2, height = area.height }
end)

shell.hooks.on("window.geometry-changed", function(ev)
  shell.log.debug("window %d is now %dx%d at %d,%d", ev["window-id"], ev.width, ev.height, ev.x, ev.y)
end)
```
