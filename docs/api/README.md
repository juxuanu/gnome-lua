# gnome-lua extension API, version 1 (1.1)

Scope: the contract every `shell.*` module obeys. Per-area documents
(`core.md`, `ui.md`, `panel.md`, `menu.md`, `quick-settings.md`, `dash.md`,
`notify.md`, `window.md`, `prefs.md`, `dbus.md`, `hooks.md`, `variant.md`, and
since 1.1 `keybindings.md`, `overview.md`, `search.md`, `fs.md`, `spawn.md`,
`network.md`) only add what is specific to them; where they are silent, this
file applies. Status
column conventions used in all documents: **impl** = exists in
`subprojects/shell-lua` today (M1), **M7** = specified here, implemented in M7.
See also: [`../MIGRATING_FROM_JS.md`](../MIGRATING_FROM_JS.md) (guide for authors of JS extensions).

Not in v1: access to GObject/Clutter/Cogl/Meta/Gio objects by any route; custom
widget classes or virtual-method overrides; raw allocation, paint or Clutter
effects; patching or replacing shell internals; synchronous waiting of any
kind; JS extensions or a compatibility shim (PLAN D13); loading extension code
from outside the extension directory.

## Modules

| Table | Document | Gate |
|---|---|---|
| `shell.api_version`, `.extension`, `.log`, `.timer`, `.settings`, `.gettext`, `.hooks` | core.md | impl; `settings.open` needs `settings` |
| `shell.time` (1.1) | core.md | none |
| `shell.ui` | ui.md | none |
| `shell.panel`, type `shell.Indicator` | panel.md | none |
| type `shell.Menu` and its items | menu.md | none |
| `shell.quick_settings` | quick-settings.md | none |
| `shell.dash` | dash.md | none |
| `shell.notify` (callable) | notify.md | none |
| `shell.windows`, `.workspaces`, `.monitors`, `.apps` | window.md | none |
| `shell.keybindings`, type `shell.Keybinding` (1.1) | keybindings.md | none |
| `shell.overview` (1.1) | overview.md | none |
| `shell.search`, type `shell.SearchProvider` (1.1) | search.md | `search` |
| `shell.dbus` | dbus.md | `dbus` |
| `shell.fs`, `shell.extension.data_dir`, types `shell.Task`, `shell.FileWatch` (1.1) | fs.md | `fs` outside the extension's own directories |
| `shell.spawn`, type `shell.Process` (1.1) | spawn.md | `spawn` |
| `shell.network`, type `shell.Request` (1.1) | network.md | `network` |
| `prefs.lua` (a file, not a table) | prefs.md | none |

## Versioning and stability

- `shell.api_version = {major, minor}` (read-only). `metadata.json` declares
  `"shell-api": "1"` or `"1.N"`. The loader checks the major, and with `"1.N"`
  marks the extension OUT_OF_DATE when the running minor is below `N`
  (**impl**; a malformed version such as `"1."`, `"1.x"` or `"1.0.1"` is
  OUT_OF_DATE too).
- Within a major, minors are additive: new functions, new optional fields, new
  properties, new hooks, new payload keys, new enum strings. Nothing documented
  is removed or changes meaning. A removal needs two releases of runtime
  warning (Looking Glass and the extensions app), then a major bump.
- Promised: names, fields, enum strings, argument and return shapes, callback
  signatures, hook names and payload keys/types, error *kinds*, placement and
  ordering rules, teardown rules. Not promised: error message text, CSS class
  names of built-in widgets (except those a document lists as stable), timing
  and animation, pixel sizes, behaviour of undocumented fields.
- Feature detection: unknown handle properties read as `nil` (**impl**), absent
  functions are `nil` (never stubs), and `shell.hooks.list()` enumerates hooks.
  Passing an unknown *field* to a constructor is an error naming the field.
- `tests/api/` runs one Lua file per document against every release.

## Calling conventions

1. Constructors and anything that creates or changes shell state take **one
   options table**. Unknown or mistyped fields, and missing required ones,
   raise an error naming the field (**impl**: `check_fields`). Positional forms
   exist only where core.md defines them (`hooks.on(name, fn)`,
   `timer.after(ms, fn)`, `settings:on_changed(key, fn)`) and for one-argument
   getters/setters such as `windows.get(id)`.
2. All names are `snake_case`; the one exception is hook names and hook
   payload keys, which are hyphenated (`panel.indicator-added`, `window-id`)
   because `docs/CONVENTIONS.md` fixes them for the C emitters and the bridge
   delivers keys verbatim (**impl**). Enumerations are lower-case kebab strings (GLib
   nicks: `"center"`, `"source-closed"`); flags are `{nick = true}` sets
   (**impl**: `shell_lua_push_gvalue`). Booleans are booleans, never 0/1.
3. Every index, position and count the API exposes is **1-based** (panel and
   menu indices, workspace and monitor numbers). Hook payloads follow the same
   rule. Identifiers (`window id`) are opaque integers, never indices.
4. Lengths are logical pixels, durations are integer milliseconds, opacity is
   `0..255`, fractions (slider values) are `0.0..1.0`. Colours are CSS strings.
5. Failures: **misuse** (wrong type, unknown field, dead handle, missing
   capability, constraint in the document) raises a Lua error. **Environmental**
   failure of a synchronous call returns `nil, message` (e.g.
   `shell.apps.launch`). Asynchronous work reports through `on_error`.
6. Events are `on_<event>` callbacks. A constructor takes them as fields; any
   handle also offers `h:on_<event>(fn)` returning a `shell.Connection`
   (**impl** precedent: `Settings:on_changed`). There is no generic
   `connect(name, fn)`. Callbacks set at construction live as long as the
   object. Argument shape depends on the source:
   - handles that are something on screen (widgets, indicators, menus and
     items, quick-settings items, dash buttons, notifications) pass the
     emitting handle first, then the event's arguments, so a callback can refer
     to its own object without an upvalue;
   - data sources (timers, settings, hooks, D-Bus signals and name watches) pass
     only data (**impl**: `fn(key, value)`, `fn(payload)`);
   - one-shot completion callbacks (`on_reply`, `on_error`, `on_done`) pass only
     their results.
   Two naming families exist by design: `shell.ui` widgets mirror the St signal
   (`on_clicked`, `on_changed`), shell items name the user action (`on_click`,
   `on_toggle`, `on_activate`).
7. **No callback is invoked because of an assignment made by Lua code.** Writing
   `toggle.checked = true` or `slider.value = 0.5` is silent; only user (or
   shell) originated changes call back. This makes "sync from external state"
   loops safe.
8. Callbacks run under the extension's pcall with the watchdog (500 ms) and the
   error policy of SANDBOX.md: an error is logged with a traceback and counted;
   more than 5 errors disable the extension. A callback MAY call any API. A call
   that would destroy or reparent the object currently emitting is deferred
   until the callback returns; the handle stays valid until then.

## Handles

A handle is userdata that holds a weak reference to a shell object and exposes
exactly the methods and properties its document lists (**impl**:
`shell-lua-handle.h`). Methods use `:`; properties are plain fields, read-only
unless the table says `rw`; assigning a read-only or unknown property raises.
The same object yields the same userdata within an extension (`==` works).
Handle types inherit (`shell.QuickMenuToggle` is a `shell.QuickToggle`).
Snapshots (`shell.monitors`) are ordinary tables, never handles.

A handle goes **dead** when its object is destroyed (by `destroy()`, by its
parent, by the shell, or at disable). Any use raises
`handle to <type> is no longer valid` (**impl**) except: `tostring(h)`, `==`,
and the two below (**impl**: `SHELL_LUA_METHOD_DEAD_OK`).

- every handle has `alive` (boolean, read-only, never raises);
- every handle that owns something has `destroy()`, idempotent.

## Ownership and teardown

Everything an extension creates through `shell.*` is owned by its extension
context (PLAN D8), not by the Lua value: dropping the last Lua reference never
destroys a widget, indicator, item or notification, and never leaks it past
disable. On disable the context destroys its objects in reverse creation
order, disconnects callbacks and cancels timers, then closes the Lua state;
`disable()` runs first and is optional. Consequences MUST hold: a destroyed
parent destroys its children (their handles die); `destroy()` on a child frees
its slot immediately; callbacks never run after disable begins; nothing the
extension created is visible after disable completes. A runtime SHOULD cap live
owned objects per extension (reference value 4096) and raise beyond it.
Extensions are enabled and torn down on session-mode changes according to
`metadata["session-modes"]` (default `["user"]`, `shell-lua-extension.c`;
`src/ui/extension-system.c`, `extension_supports_session_mode ()`).

## Placement and ordering

Panel indicators, quick-settings items and dash buttons are placed into **slots**
(a panel box plus edge, the quick-settings grid, a dash edge). Extension items
in a slot are ordered by (1) the extension's position in `org.gnome.shell
enabled-extensions`, then (2) the item's `index` among that extension's items
in the slot (1-based, default: after the extension's existing ones, values
past the end clamp). Built-in items are never interleaved. Order is a pure
function of this state, never of enable timing. An extension cannot position
itself relative to another extension.

## Capabilities

Declared in `metadata.json` `"capabilities"`; unknown strings reject the
extension (**impl**: `known_capabilities`); shown to the user in the extensions
app. Calling a gated function without the capability raises
`<function> requires the "<cap>" capability` (**impl** for `settings`).

| Capability | Unlocks | Status |
|---|---|---|
| `settings` | `shell.settings.open(schema_id)` for any installed schema | impl |
| `dbus` | all of `shell.dbus`. It reaches every service the user can; treat it as roughly `spawn`-equivalent (e.g. systemd user manager), not as a narrow permission | impl |
| `search` | connecting to hook `search.query` (carries what the user types), and `shell.search.add_provider` (1.1: providing results, search.md) | impl (accepted in `metadata.json`); `add_provider` 1.1 |
| `network` | `shell.network.request` (1.1): HTTP and HTTPS requests to any host the user's machine can reach, including `localhost` and the local network (network.md) | 1.1 |
| `fs` | `shell.fs` outside the extension's own directories (1.1): without it an extension reads `shell.extension.dir` and reads and writes `shell.extension.data_dir`; with it, any path the shell can reach (fs.md) | 1.1 |
| `spawn` | `shell.spawn` (1.1): running any program as the user, with the environment of the shell. The broadest capability (spawn.md) | 1.1 |

## Required changes to the M1 runtime

Done (`subprojects/shell-lua`, tests in `tests/test-objects.c`): `alive` on
every handle and idempotent `destroy()` on dead handles (per-method
`SHELL_LUA_METHOD_DEAD_OK`); the owned-object registry (`shell_lua_own_object`,
reverse-order destruction at disable, the 4096 cap, `destroy()` frees the slot
at once, callbacks blocked once disable begins); the `"1.N"` check; `search`
in `known_capabilities`; virtual handle properties (per-type getter/setter
tables); the assignment guard (rule 7: assignments are silent, unknown and
read-only properties raise); the deferral of rule 8. Still to do: `shell.dbus.variant` (dbus.md).

## Example

```lua
local shell = require("shell")
return { enable = function()
  local ind = shell.panel.add_indicator{ icon_name = "alarm-symbolic", accessible_name = "Clock" }
  ind.menu:add_item{ label = "Hello", on_activate = function(item) shell.log("clicked %s", item.label) end }
end }  -- no disable(): the context destroys the indicator
```
