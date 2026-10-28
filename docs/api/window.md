# `shell.windows`, `shell.workspaces`, `shell.monitors`, `shell.apps` (version 1, draft)

Scope: a read-mostly view of the desktop (which windows, workspaces and
monitors exist, which window is focused) with five narrow actions on windows,
and the small `shell.apps` helper the other documents use to launch
applications. State changes arrive through hooks (hooks.md), whose payloads
carry the same ids and indices as these handles. Window management itself
(placement, tiling, keybinding-driven moves, window animations) stays in the
shell.

Not in v1:
- Geometry: reading `frame_rect`, moving, resizing, maximizing, fullscreening,
  pinning, always-on-top, shading, `stick`: an extension that arranges windows
  is a window manager; use hooks to observe and the five actions to nudge.
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
cannot map.

## `shell.windows`

| Function | Returns |
|---|---|
| `shell.windows.list()` | sequence of `shell.Window`, most recently focused first (the Alt-Tab order). Includes minimized windows and windows on any workspace/monitor; excludes `skip_taskbar` windows (docks, desktop, the shell's own chrome), as the overview does (`workspace.js` `_isOverviewWindow`) |
| `shell.windows.focused()` | the focused `shell.Window`, or nil |
| `shell.windows.get(id)` | the `shell.Window` with that id, or nil (also nil for windows `list()` would exclude) |

## `shell.Window`

All properties are read-only snapshots of the live window; they change when the
window does.

| Property | Type | Notes |
|---|---|---|
| `id` | integer | stable key; use it for your own tables |
| `title` | string | may be empty |
| `wm_class` | string | `WM_CLASS` class (X11) or the app id (Wayland), may be empty |
| `app_id` | string or nil | desktop file id |
| `workspace` | integer or nil | 1-based; nil when the window is on all workspaces |
| `monitor` | integer | 1-based, the monitor holding most of the window |
| `minimized` | boolean | |
| `maximized` | boolean | maximized on both axes |
| `fullscreen` | boolean | |
| `focused` | boolean | |
| `pid` | integer or nil | nil when unknown (many Wayland clients) |
| `alive` | boolean | false once the window is destroyed; the handle then raises like any dead handle |

Actions (methods; each returns nothing, raises on misuse, and takes effect on
the next frame, so properties may still show the old value right after the
call):

| Method | Effect |
|---|---|
| `w:activate()` | `Main.activateWindow`: switches to the window's workspace if needed, unminimizes, raises, focuses; also hides the overview and closes the calendar |
| `w:minimize()` | minimize (no-op if already) |
| `w:close()` | polite close request (`Meta.Window.delete`); the app may refuse or ask; an unresponsive app gets the usual "Not responding" dialog |
| `w:move_to_workspace(n)` | `n` an integer in `1..#shell.workspaces.list()`; does not create workspaces (`change_workspace_by_index(n, append = false)`); raises when out of range |
| `w:move_to_monitor(n)` | `n` in `1..#shell.monitors.list()`; as in `moveWindowToMonitorAndWorkspace`, entering the primary monitor can change the window's workspace |

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
pixels, global coordinates, from `Monitor` in `js/ui/layout.js`), `scale`
(`geometry_scale`, integer or fractional), `primary` (boolean),
`work_area` (`{x, y, width, height}`, the area not covered by the panel and
struts, `LayoutManager.getWorkAreaForMonitor`).

## `shell.apps`

| Function | Returns |
|---|---|
| `shell.apps.launch(app_id)` | `true`, or `nil, message` when the id is unknown or launching failed (environmental failure, README rule 5). Activates a running app's window like clicking its dash icon; does not hide the overview by itself |
| `shell.apps.running()` | sequence of app ids, most recently used first |
| `shell.apps.favorites()` | sequence of app ids in dash order (`favorite-apps`) |
| `shell.apps.info(app_id)` | `{id = , name = , icon_name = , description = }` or nil |

## Which hook carries which id

| Hook | Key | Matches |
|---|---|---|
| `window.mapped`, `.unmapped`, `.focused`, `.minimized`, `.unminimized`, `.title-changed`, `.workspace-changed`, `.monitor-changed`, `.state-changed` | `window-id:t` | `Window.id`; resolve with `windows.get(id)` (nil for `unmapped`: the window is gone) |
| `window.focused` | `previous-window-id:t` | `Window.id` of the previously focused window, absent if none |
| `window.workspace-changed` | `from:i`, `to:i` | workspace numbers (absent = all workspaces) |
| `window.monitor-changed` | `from:i`, `to:i` | monitor numbers |
| `workspace.switched`, `workspace.added`, `workspace.removed` | `from:i` / `to:i` / `index:i` | `Workspace.index` |
| `monitors.changed` | `count:i`, `primary:i` | `monitors.list()` |
| `dash.item-added`, `notification.*`, `panel.*` | `id`, `name` | see their documents |

Every `window.*` payload also carries the snapshot keys `title`, `wm-class`,
`app-id`, `workspace`, `monitor`, `pid` so a listener rarely needs a lookup.

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
