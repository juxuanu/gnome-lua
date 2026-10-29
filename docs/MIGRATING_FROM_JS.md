# Migrating a GNOME Shell extension from JavaScript to Lua

For authors who know the GJS extension API (ESM `Extension` subclasses,
`Main.panel.addToStatusArea`, `PanelMenu.Button`, `QuickSettings`, `Gio.DBus`,
`GLib.timeout_add`, `St` widgets, `global.get_window_actors()`, ...) and move
to this shell. This guide is a map; the normative text is in `docs/api/`
(start with `docs/api/README.md`: calling conventions, handles, ownership,
capabilities, rules 1-8) and the extensions in
`subprojects/shell-lua/examples/` are the runnable reference. Where this guide
and `docs/api/` differ, `docs/api/` wins (`docs/PLAN.md` §7 is the one-line
summary of this file; the code in `docs/PLAN.md` §5 is illustrative only).

This guide describes API 1.1 (`shell.api_version = {major = 1, minor = 1}`).
Rows, functions and hooks added after 1.0 are marked "(1.1)"; an extension
that uses them declares `"shell-api": "1.1"`.

Contents: 1 What changed and why, 2 The skeleton, 3 Translation tables,
4 Ownership and teardown, 5 What has no equivalent, 6 Worked example,
7 Looking Glass, 8 Preferences.

## 1. What changed and why

There is no JavaScript in this shell. `gnome-shell` is C with Lua 5.5 embedded
for extensions only. JS extensions are not loaded and there is no
compatibility shim (PLAN D13): a shim would need GObject Introspection, which
is exactly what was removed.

| JS shell | This shell | Why |
|---|---|---|
| GJS, `imports`/`gi://` | A sandboxed Lua 5.5 state per extension. The only module is `shell` (`require("shell")` and the global `shell` are the same table). | Isolation: one extension's error, leak or runaway loop cannot touch another (D5). |
| GObject Introspection: every `St`, `Clutter`, `Meta`, `Gio` object | No GI. Handles (userdata) with exactly the methods and properties `docs/api/` lists. | Decouples extensions from shell internals, which is what makes the API stable (D2). |
| Monkey patching, `InjectionManager`, prototype overrides | Hooks: `shell.hooks.on` (after) and `shell.hooks.before` (cancel or amend). Patching internals is impossible by design. | Patches are what break every six months. |
| `shell-version` whitelist in `metadata.json` | `"shell-api": "1"` (or `"1.N"`). `shell.api_version = {major, minor}`. Within a major, minors only add; a removal needs two releases of runtime warning, then a major bump. | One metadata value instead of one per GNOME release (D10). |
| Unrestricted code | No `os`, `io`, `debug`, `load`, `package`; `require` only reaches files inside your extension directory; D-Bus, files outside your own directories, processes and the network behind `capabilities` shown to the user (D6). | Extensions today can do anything the user can. |
| A frozen shell if your callback loops | Watchdog: 500 ms wall clock per callback, 64 MiB per Lua state (`subprojects/shell-lua/docs/SANDBOX.md`). | D7 |
| Leaked signal handlers and actors | Everything created through `shell.*` is owned by the extension and destroyed at disable (section 4). | D8 |
| Uncaught exceptions are logged and ignored | An error in any callback is logged with a traceback and counted; more than 5 disable the extension (state ERROR). The shell never crashes because of Lua. | D9, README rule 8 |
| Synchronous D-Bus, file, process and HTTP calls | Nothing blocks the compositor: D-Bus, `shell.fs` (1.1), `shell.spawn` (1.1) and `shell.network` (1.1) are callback-only. The only synchronous file calls are `fs.exists` and `fs.info` (one `stat`). | The watchdog would kill you first. |
| `prefs.js` with Gtk/Adw widgets | `prefs.lua`: a data table rendered by the shell (section 8). | D12 |

Unchanged: the directory layout (`<uuid>/` with `metadata.json`, `stylesheet.css`,
`schemas/`, `locale/`), GSettings schemas (same XML), gettext, the
`enabled-extensions` key, the `gnome-extensions` tool (`create`, `pack`,
`install`, `enable`, `prefs`, ...) and the `org.gnome.Shell.Extensions` D-Bus
interface (D11). Session modes work as before: `"session-modes"` in
`metadata.json`, default `["user"]`.

Failure kinds (README, calling convention 5), so you know what to `pcall`:

| Kind | What you get |
|---|---|
| Misuse: wrong type, unknown or missing field, dead handle, missing capability | A Lua error, e.g. `handle to <type> is no longer valid`, `<function> requires the "<cap>" capability`, `unknown hook`. An unknown field in a constructor is an error naming the field. |
| Environmental failure of a synchronous call | `nil, message` (e.g. `shell.apps.launch`). |
| Asynchronous failure | the `on_error` callback (D-Bus, `shell.fs`, `shell.network`; for `shell.spawn` the outcome, failures included, is the `result` of `on_exit`). |

## 2. The skeleton

### metadata.json

```json
{
  "uuid": "break-reminder@example.org",
  "name": "Break Reminder",
  "description": "Reminds you to take a break.",
  "shell-version": ["46", "47", "48"],
  "settings-schema": "org.example.break-reminder",
  "gettext-domain": "break-reminder@example.org",
  "version": 3,
  "url": "https://example.org/break-reminder"
}
```

```json
{
  "uuid": "break-reminder@example.org",
  "name": "Break Reminder",
  "description": "Reminds you to take a break.",
  "shell-api": "1",
  "settings-schema": "org.example.break-reminder",
  "gettext-domain": "break-reminder@example.org",
  "capabilities": [],
  "session-modes": ["user"],
  "version": 3,
  "url": "https://example.org/break-reminder"
}
```

| Key | Now |
|---|---|
| `uuid`, `name`, `shell-api` | required; the directory name MUST equal `uuid` |
| `shell-api` | `"1"` or `"1.N"`: the loader checks the major, and with `"1.N"` marks the extension OUT_OF_DATE when the running minor is below `N`. Malformed values (`"1."`, `"1.x"`, `"1.0.1"`) are OUT_OF_DATE too. Use `"1.N"` only when you need something added in minor `N` (the "Since" columns of `docs/api/`): everything marked 1.1 below needs `"shell-api": "1.1"`, and a 1.0 shell marks such an extension OUT_OF_DATE |
| `shell-version` | gone, replaced by `shell-api` |
| `settings-schema` | as before; opened with `shell.settings.extension()` |
| `gettext-domain` | as before (default: the uuid); `locale/` as before |
| `capabilities` | new, shown to the user: `settings` (`shell.settings.open` for any schema), `dbus` (all of `shell.dbus`), `search` (hook `search.query` and `shell.search.add_provider`), and since 1.1 `fs` (`shell.fs` outside your own directories), `spawn` (`shell.spawn`: any program, the broadest capability) and `network` (`shell.network.request`). An unknown string rejects the extension. Everything else, `shell.time`, `shell.keybindings` and `shell.overview` included, needs none |
| `session-modes` | as before |

`gnome-extensions create --template plain|indicator|quick-settings [--prefs]
[--settings-schema ID] [--gettext-domain D]` scaffolds a Lua extension.

### extension.js vs extension.lua

```js
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

export default class Hello extends Extension {
    enable() {
        this._indicator = new MyIndicator();
        Main.panel.addToStatusArea(this.uuid, this._indicator);
    }

    disable() {
        this._indicator.destroy();
        this._indicator = null;
    }
}
```

```lua
local shell = require("shell")

local ext = {}

function ext.enable()
  shell.panel.add_indicator{ accessible_name = _("Hello"), icon_name = "face-smile-symbolic" }
end

return ext   -- no disable(): the shell destroys what enable() created
```

| JS | Lua |
|---|---|
| `export default class X extends Extension` | `extension.lua` returns a table; `enable` and `disable` are optional fields of it. A chunk that does not return a table fails to enable (`extension.lua must return a table`) |
| `constructor(metadata)` | none: the file body runs once per enable, right before `enable()`. Create shell objects in `enable()` |
| `enable()`, `disable()` | `ext.enable()`, `ext.disable()`: no `this`, no arguments, synchronous (no `async`). Every enable runs in a fresh Lua state, so nothing survives a disable. `disable()` runs first and is optional (section 4) |
| `this.uuid` | `shell.extension.uuid` |
| `this.path`, `this.dir` (`Gio.File`) | `shell.extension.dir` (absolute path string) |
| `this.metadata` | `shell.extension.metadata` (read-only copy of `metadata.json`) |
| a directory for your own files (`GLib.get_user_data_dir()` + a name you invent) | `shell.extension.data_dir` (1.1): `$XDG_DATA_HOME/gnome-lua/<uuid>`, created on first read (mode 0700), kept across disable, restart and upgrade; readable and writable through `shell.fs` without any capability |
| (no equivalent) | `shell.extension.capabilities` (sequence of strings) |
| `this.getSettings()` | `shell.settings.extension()` |
| `this.gettext`, `_` imported from `extension.js` | the globals `_`, `ngettext`, `pgettext` (also `shell.gettext`, `shell.ngettext`, `shell.pgettext`) |
| `import * as Util from './util.js'` | `local util = require("util")`: loads `util.lua` or `util/init.lua` from your extension directory; names match `^[A-Za-z0-9_][A-Za-z0-9_.]*$` (`a.b` is `a/b.lua`) and never leave the directory |
| `Extension.lookupByUUID`, `Main.extensionManager` | none: extensions must not depend on each other |

**The context object.** There is none to hold. Your "context" is the extension
instance the shell keeps behind the `shell` table: it owns everything you
create (D8). The only part you can see is `shell.extension` (`uuid`, `dir`,
`metadata`, `capabilities`, and since 1.1 `data_dir`); it is `nil` in the
Looking Glass evaluator.
Dropping the last Lua reference to a widget, indicator or notification never
destroys it, and a reference to a destroyed object is just a dead handle.

The Lua sandbox has `string`, `table`, `math`, `utf8`, `coroutine`, the basic
functions (`pairs`, `pcall`, `select`, `setmetatable`, `unpack`, `print`, ...)
and `shell`. A parameter named `_` shadows the gettext function inside that
callback: write `function(_item, on)` where you also call `_("...")`.

## 3. Translation tables

Conventions that apply to every row (README rules 1-7): constructors take
**one options table**; names are `snake_case`; indices and counts are
**1-based**; enumerations are kebab-case strings (`"center"`); lengths are
logical pixels, durations integer milliseconds, opacity `0..255`, fractions
`0.0..1.0`. Events are `on_<event>` fields, and any handle also has
`h:on_<event>(fn)` returning a `shell.Connection` (`:disconnect()`). Callbacks
of things on screen receive the emitting handle first (`function(self, ...)`);
data sources (timers, settings, hooks, D-Bus signals) receive only data.
Assigning a property from Lua (`toggle.checked = true`, `slider.value = 0.5`)
**never calls the callback**, so synchronising from external state cannot loop.

### Core

| Task | JS | Lua |
|---|---|---|
| One-shot timeout | `GLib.timeout_add(GLib.PRIORITY_DEFAULT, 500, () => {...; return GLib.SOURCE_REMOVE;})` | `shell.timer.after{ ms = 500, callback = function() ... end }` returns a `shell.Timer` |
| Repeating timeout | `GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 60, () => {...; return GLib.SOURCE_CONTINUE;})` | `shell.timer.every{ ms = 60000, callback = function() ... end }`; it repeats until the callback returns `false` (a callback that raises does not repeat). Positional `shell.timer.every(60000, f)` also works |
| Idle | `GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, fn)` | `shell.timer.idle{ callback = fn }` |
| Cancel | `GLib.source_remove(id)` | `timer:cancel()`, `timer.active`. Not needed at disable: timers are owned |
| Wall clock | `GLib.get_real_time()`, `Date.now()`, `new Date()` | `shell.time.now()` (1.1): seconds since the epoch, with a fraction. There is still no `os.time` or `os.date` in the sandbox |
| Monotonic clock | `GLib.get_monotonic_time()` | `shell.time.monotonic()` (1.1): seconds, for durations |
| Calendar fields | `GLib.DateTime.new_now_local()` (`get_year()`, `get_month()`, `get_hour()`, `get_day_of_week()`, ...), `date.getHours()` | `shell.time.date{ at = t, utc = false }` (1.1) returns `{year, month, day, hour, minute, second, microsecond, weekday, yearday, timezone, offset, dst}`: months 1..12, `weekday` ISO 1 = Monday .. 7 = Sunday; `at` defaults to now |
| Build a time | `GLib.DateTime.new_local(y, m, d, h, mi, s).to_unix()` | `shell.time.timestamp{ year = 2026, month = 10, day = 29, hour = 8 }` (1.1), the inverse of `date` (`utc = true` for UTC) |
| Format | `dt.format('%H:%M')` | `shell.time.format("%H:%M", { at = t, utc = false })` (1.1): GDateTime format strings (`%A %x` is the weekday and the locale's date); raises on a bad format |
| Time zone | `GLib.TimeZone.new_local().get_identifier()`; watching for a change by hand | `shell.time.timezone()` (1.1); hook `time.timezone-changed` (payload `timezone`) |
| Settings object | `this.getSettings()` | `shell.settings.extension()` (errors without `settings-schema`) |
| Any other schema | `new Gio.Settings({schema_id})` | `shell.settings.open(schema_id)`, needs capability `settings` |
| Read, write | `s.get_boolean('k')`, `s.set_int('k', 3)`, `s.get_value`/`set_value` | `s:get_boolean("k")`, `s:set_int("k", 3)`; also `get_double`, `get_string`, `get_strv` and the `set_*` twins, generic `s:get("k")`/`s:set("k", v)` (`docs/api/variant.md`), `s:reset("k")`, `s:keys()` |
| Changes | `s.connect('changed::k', (s, key) => ...)` | `s:on_changed("k", function(key, value) ... end)`; `nil` instead of `"k"` listens to every key. Returns a `shell.Connection`. Fires for changes made elsewhere (dconf, prefs window) |
| Binding to a widget | `s.bind('k', obj, 'prop', Gio.SettingsBindFlags.DEFAULT)` | no equivalent: write the widget from `on_changed` and the setting from the widget's callback (the worked example does) |
| Translate | `_('Text')`, `ngettext(a, b, n)`, `pgettext(ctx, s)`, `_('%d min').format(n)` | `_("Text")`, `ngettext("%d minute", "%d minutes", n)`, `pgettext("ctx", "Text")`, `_("%d min"):format(n)` (`string.format`). Strings come from `locale/` with `gettext-domain`; translation also works in `prefs.lua` |
| Log | `console.log()`, `log()`, `logError(e)`, `this.getLogger()` | `shell.log.debug(fmt, ...)`, `.info`, `.warning`, `.error`, and since 1.1 `.fatal` (logs like `error`, then raises a Lua error with the message) (`string.format` semantics: `shell.log.info("%s", text)`); `shell.log(fmt, ...)` is `info`. Domain `shell-lua/<uuid>`. `print` goes to `g_message`. Since 1.1 `warning` and `error` never abort the shell, even under `G_DEBUG=fatal-warnings` |
| Stylesheet | `stylesheet.css`, `stylesheet-dark.css`, ... | same files, same loading and unloading. The shell adds the class `lua-ext-<id>` to every widget you create (uuid, lower-cased, every char outside `[a-z0-9-]` replaced by `-`): scope selectors as `.lua-ext-<id> .mine` |
| Bundled icon | `Gio.icon_new_for_string(this.path + '/a.svg')` | `icon_file = "a.svg"` (path relative to the extension directory, `.svg` or `.png`; absolute paths, `..` and missing files raise) |

### Widgets (`shell.ui`, `docs/api/ui.md`)

| JS | Lua |
|---|---|
| `new St.BoxLayout({vertical: true})` | `ui.box{ orientation = "vertical", children = { a, b } }`; `b:add(child [, index])`, `b:children()`, `b:clear()` |
| `new St.Label({text, y_align: Clutter.ActorAlign.CENTER})` | `ui.label{ text = "x", y_align = "center" }`; `l:set_markup(str)`; `ellipsize` |
| `new St.Icon({icon_name, icon_size, style_class: 'system-status-icon'})` | `ui.icon{ icon_name = "...", icon_size = 16, style_class = "system-status-icon" }` |
| `new St.Button({label}); b.connect('clicked', ...)` | `ui.button{ label = "x", on_clicked = function(self, button) end }`; `toggle_mode`, `checked`; an icon-only button needs `accessible_name` |
| `new St.Entry`, `PopupSwitchMenuItem`'s switch, `Slider.Slider`, `St.ScrollView` | `ui.entry{...}` (`on_changed(self, text)`, `on_activate(self, text)`), `ui.switch{...}`, `ui.slider{...}` (`value` 0.0..1.0), `ui.scroll{ child = box }` |
| `actor.add_style_class_name()`, `set_style()`, `visible`, `opacity`, `reactive`, `x_expand`, `x_align` | `w:add_style_class(name)`, `w.style` (inline declarations only, 1024 bytes), `w.visible`, `w.opacity`, `w.reactive`, `w.x_expand`, `w.x_align = "start"` |
| `actor.get_parent().remove_child(actor)`; `actor.destroy()` | `w:remove()` (stays alive, can be added again); `w:destroy()` |
| `ease()`, `set_position()`, `allocate()`, padding in code | none: sizes, margins, spacing and fonts are CSS; layout is `x_expand`/`x_align` |

### Panel indicator and menu (`docs/api/panel.md`, `docs/api/menu.md`)

| JS | Lua |
|---|---|
| `new PanelMenu.Button(0.0, name)` + `add_child(icon)` + `Main.panel.addToStatusArea(role, btn, position, box)` | `shell.panel.add_indicator{ accessible_name = "...", icon_name = "x-symbolic" }` (also `icon_file`, `text`, or `child = widget`; give at most one) |
| `role` | `name = "role"` (default: the uuid; unique, `^[A-Za-z0-9_.@-]{1,128}$`; a second indicator of yours needs its own `name`) |
| `box` `'left'`/`'center'`/`'right'` | `position = "left"\|"center"\|"right"` (default `"right"`) |
| `position` index `0` (left end of the box, how most extensions call it) | `edge = "start"` (default) or `"end"`; `index` is relative to *your* items in that slot. Built-ins never interleave with extensions |
| `new PanelMenu.Button(0.0, name, true)` (`dontCreateMenu`) + own click handling | `on_click = function(self, button) end`, or `menu = false`; mutually exclusive with `menu = true` |
| `btn.menu` | `ind.menu` (a `shell.Menu`; nil without a menu) |
| `btn.visible`, `setSensitive()`, `btn.menu.isOpen` | `ind.visible`, `ind.sensitive`, `ind.menu.is_open` (`ind.active` while open) |
| replace the button's child | `ind.child = widget` (the previous child is destroyed) |
| `new PopupMenuItem(t)`; `i.connect('activate', ...)`; `menu.addMenuItem(i)` | `menu:add_item{ label = t, on_activate = function(item) end }`; the whole menu closes afterwards |
| `PopupImageMenuItem` | `menu:add_image_item{ label, icon_name \| icon_file, on_activate }` |
| `PopupSwitchMenuItem`; `connect('toggled', (i, state) => ...)` | `menu:add_switch{ label, state = false, on_toggled = function(item, state) end }`; `item.state`; `status` text replaces the switch |
| `PopupSubMenuMenuItem`; `item.menu.addMenuItem()` | `local sub = menu:add_submenu{ label, icon_name }`; `sub.menu:add_item{...}` (one level only) |
| `PopupSeparatorMenuItem(t)` | `menu:add_separator{ label = t }` |
| `item.setOrnament(PopupMenu.Ornament.DOT)` | `item.ornament = "dot"` (`"hidden"`, `"none"`, `"check"`, `"dot"`, `"no-dot"`) |
| an item holding an arbitrary actor (`PopupBaseMenuItem` + `add_child`) | `menu:add_widget{ child = widget }` |
| `menu.removeAll()`, `item.destroy()` | `menu:clear()`, `item:destroy()` |
| `menu.open()`, `close()`, `toggle()`, `connect('open-state-changed', (m, open) => ...)` | `menu:open([{animate = false}])`, `:close()`, `:toggle()`, `menu:on_open_state_changed(function(menu, open) end)` |
| `item.label.text`, `item.setSensitive()` | `item.label`, `item.sensitive`, `item.visible` |

```lua
local ind = shell.panel.add_indicator{
  name = "caffeine", position = "right", edge = "start",
  icon_name = "my-caffeine-symbolic", accessible_name = _("Caffeine"),
}
ind.menu:add_switch{ label = _("Keep awake"), state = false,
  on_toggled = function(_item, on)
    ind.child.icon_name = on and "my-caffeine-on-symbolic" or "my-caffeine-symbolic"
  end }
ind.menu:add_separator{}
ind.menu:add_item{ label = _("Settings"),
  on_activate = function() shell.apps.launch("org.gnome.Settings.desktop") end }
```

### Quick settings (`docs/api/quick-settings.md`)

There is no `SystemIndicator` class that couples an icon with its items: icon
and items are independent objects; link them yourself (`dot.visible = on` in
`on_toggle`). The grid has 2 columns; extension items come after the built-in
toggles.

| JS | Lua |
|---|---|
| `class extends QuickSettings.QuickToggle` (`title`, `subtitle`, `iconName`, `toggleMode`, `checked`; `connect('clicked')`) | `qs.add_toggle{ title, subtitle, icon_name \| icon_file, checked, on_toggle = function(self, checked) end }` with `local qs = shell.quick_settings`. `checked` is already flipped when `on_toggle` runs; veto with `self.checked = false` (silent) |
| a toggle that only acts (no state) | `on_click = function(self) end` instead of `on_toggle` (exclusive) |
| `QuickSettings.QuickMenuToggle`, `menu.setHeader(icon, title, subtitle)`, `menu.addMenuItem(...)` | `qs.add_menu_toggle{...}` (all `add_toggle` fields plus `menu_enabled`, `menu_button_accessible_name`); `t.menu:set_header{ icon_name, title, subtitle }` (`title` required); `t.menu:add_item{...}`, `t.menu:add_widget{...}` (`t.menu` is a `shell.QuickMenu`, a `shell.Menu`) |
| `QuickSettings.QuickSlider` (`iconName`, `slider.value`, `slider.connect('notify::value')`) | `qs.add_slider{ accessible_name = "...", icon_name, value = 0.5, on_changed = function(self, value) end }`; `icon_reactive` + `on_icon_click(self)`; `colspan` defaults to 2 |
| `SystemIndicator._addIndicator()` + `iconName`, `quickSettingsItems.push(item)`, `statusArea.quickSettings.addExternalIndicator()` | `qs.add_indicator{ icon_name \| icon_file, visible, accessible_name }`; items and icon are added separately |
| `item.visible`, `item.destroy()` | `item.visible`, `item:destroy()` (no re-adding: create a new one, or toggle `visible`) |
| grid span | `colspan = 1\|2`; `index` among your own items |

```lua
local qs = shell.quick_settings
local dot = qs.add_indicator{ icon_name = "format-text-larger-symbolic", visible = false }
local toggle = qs.add_menu_toggle{
  title = _("Large text"), subtitle = _("Off"), icon_name = "format-text-larger-symbolic",
  on_toggle = function(self, on)
    self.subtitle, dot.visible = on and _("On") or _("Off"), on
  end,
}
toggle.menu:set_header{ icon_name = "format-text-larger-symbolic", title = _("Large text") }
qs.add_slider{ accessible_name = _("Text scale"), icon_name = "preferences-desktop-font-symbolic",
  value = 0.5, on_changed = function(_self, v) shell.log.debug("scale %.2f", v) end }
```

### Dash and applications (`docs/api/dash.md`, `docs/api/window.md`)

| JS | Lua |
|---|---|
| `Main.overview.dash._box.add_child(btn)` or patching `Dash._redisplay` (no official API; breaks the favorites indexing) | `shell.dash.add_button{ label, icon_name \| icon_file, on_click = function(self, button) end, position = "end", hide_overview = true, checked, visible, sensitive }` |
| insert at a position | `shell.dash.insert_at{ index = 1, position = "start", ... }` (index among *your* buttons in that group) |
| dash button state | `btn.checked`, `btn.label`, `btn.icon_name`, `btn:destroy()` (fades out) |
| `Shell.AppSystem.get_default().lookup_app(id).activate()` | `shell.apps.launch("org.gnome.Console.desktop")` returns `true` or `nil, message` |
| app lookups | `shell.apps.info(id)` returns `{id, name, icon_name, description}` or nil; `shell.apps.running()`, `shell.apps.favorites()` (sequences of app ids) |

At most 8 buttons per extension (raises beyond). A button is an icon and a
label; it is not an app icon (no menu, no drag and drop, not a favorite).

### Notifications (`docs/api/notify.md`)

| JS | Lua |
|---|---|
| `Main.notify(title, body)` | `shell.notify{ title = "T", body = "B" }` posts to your default source (titled with metadata `name`) |
| `new MessageTray.Source(...)` + `Main.messageTray.add(src)` | `local src = shell.notify.source{ title = "Mail", icon_name = "mail-unread-symbolic" }` (registered with the tray on the first `notify`) |
| `new MessageTray.Notification(...)` + `src.addNotification(n)` | `src:notify{ title, body, icon_name, urgency = "low"\|"normal"\|"high", resident, transient, markup }` returns a `shell.Notification` |
| `n.addAction(label, cb)` | `actions = { { label = "Open", on_activate = function(notification) end } }` (up to 3, each needs both fields) |
| `n.connect('activated', ...)` | `on_activate = function(self) end` |
| `n.connect('destroy', (n, reason) => ...)` | `on_dismissed = function(self, reason) end`, `reason` is `"expired"`, `"dismissed"` or `"source-closed"`; not called for your own `destroy()` |
| `n.update()`, `n.acknowledged`, `n.destroy()` | assign `n.title`, `n.body`, ...; `n.acknowledged`; `n:show()`; `n:destroy()`; `src:clear()`; `src:destroy()` |
| `Urgency.CRITICAL` | not in v1 (raises) |

### D-Bus (`docs/api/dbus.md`; capability `"dbus"` in `metadata.json`)

Everything is asynchronous. A proxy is only a `(name, path, interface)`
triple: no introspection, no property cache.

| JS | Lua |
|---|---|
| `Gio.DBus.session` / `Gio.DBus.system` | `shell.dbus.session()` / `shell.dbus.system()` (`shell.Bus`) |
| `Gio.DBusProxy.makeProxyWrapper(xml)` + `new Proxy(bus, name, path, cb)` | `bus:proxy{ name = "...", path = "...", interface = "..." }` (no XML) |
| `proxy.FooRemote(a, b, (res, err) => ...)`; `new GLib.Variant('(su)', [..])` | `proxy:call{ method = "Foo", signature = "su", args = { a, b }, on_reply = function(...) end, on_error = function(err) end, timeout_ms = 5000 }`. `signature` is the body signature without outer parentheses; `on_reply` receives the reply body spread; `err = {name, message, remote}`. Without callbacks the call is fire-and-forget |
| `proxy.FooSync(...)` | none |
| `proxy.Percentage` (cached property) | `proxy:get_property{ name = "Percentage", on_reply = function(value) end }`, `proxy:get_all{ on_reply = function(props) end }`, `proxy:set_property{ name, signature, value, on_done, on_error }` |
| `proxy.connectSignal('Sig', (p, sender, args) => ...)` | `proxy:on_signal("Sig", function(signal_name, ...) end)` (`nil` = every signal), returns `shell.Connection` |
| `proxy.connect('g-properties-changed', (p, changed, invalidated) => ...)` | `proxy:on_properties_changed(function(changed, invalidated) end)` (`changed` keyed by name, `invalidated` a sequence) |
| `Gio.bus_watch_name(...)` | `bus:watch_name{ name, on_appeared = function(owner) end, on_vanished = function() end }` |
| `Gio.bus_own_name(...)` | `bus:own_name{ name, allow_replacement, replace, on_acquired, on_lost }` returns `shell.BusName` (`acquired`, `release()`) |
| `Gio.DBusExportedObject.wrapJSObject(xml, impl)` + `.export(bus, path)` | `bus:export{ path = "/org/example/Obj", interface_xml = xml, methods = { Name = function(...) return out1, out2 end }, properties = {...} }`. Fail with `return nil, "org.example.Error.X", "message"` |
| `exported.emit_signal('S', variant)`, `emit_property_changed` | `obj:emit_signal{ name = "S", args = { ... } }`, `obj:set_property(name, value)` |
| `GLib.Variant('u', 5)` inside `a{sv}` | `shell.dbus.variant("u", 5)` where a `v` is expected; other values are typed by the default rules (`docs/api/variant.md`) |

Limits to know: one interface per exported object, no writable properties,
no asynchronous replies from your handlers, no caller identity, no file
descriptors; `proxy` refuses `org.gnome.Shell` and `org.gnome.Shell.*`;
`own_name` and `export` refuse names under `org.gnome.` and `org.freedesktop.`.
`dbus` reaches whatever the user can reach; treat it as roughly `spawn`.

### Windows, workspaces, monitors (`docs/api/window.md`)

All indices are 1-based (Meta's 0-based values never appear). A window `id` is
an opaque integer; key your own tables on it.

| JS | Lua |
|---|---|
| `global.get_window_actors().map(a => a.meta_window)`, `display.get_tab_list(...)` | `shell.windows.list()`, most recently focused first; includes minimized windows, excludes `skip_taskbar` windows |
| `global.display.focus_window` | `shell.windows.focused()` (or nil) |
| lookup by `Meta.Window.get_id()` | `shell.windows.get(id)` (or nil) |
| `w.get_title()`, `get_wm_class()`, `get_pid()`, `minimized`, `fullscreen`, `has_focus()` | `w.title`, `w.wm_class`, `w.pid`, `w.minimized`, `w.fullscreen`, `w.focused` |
| `Shell.WindowTracker.get_default().get_window_app(w).get_id()` | `w.app_id` (desktop file id, or nil) |
| `w.get_workspace().index()` (0-based), `is_on_all_workspaces()` | `w.workspace` (1-based; nil = all workspaces) |
| `w.get_monitor()` (0-based) | `w.monitor` (1-based) |
| `w.maximized_horizontally && w.maximized_vertically` | `w.maximized` (both axes) |
| `w.get_frame_rect()` | `w.frame` (1.1): a new table `{x, y, width, height}` on every read, global logical pixels, the frame rectangle (decorations included, client shadow not) |
| `w.is_above()`, `w.is_on_all_workspaces()`, `w.allows_resize()`, `w.allows_move()` | `w.above`, `w.on_all_workspaces`, `w.resizable`, `w.movable` (1.1) |
| `w.activate(time)` / `minimize()` / `delete(time)` | `w:activate()` / `w:minimize()` / `w:close()` |
| `w.move_frame(true, x, y)` | `w:move{ x = 10, y = 20 }` (1.1): frame coordinates, global logical pixels, not a user operation |
| `w.move_resize_frame(true, x, y, w, h)` | `w:move_resize{ x, y, width, height }` (1.1), or `w:resize{ width, height }` to keep the corner; every field required |
| `w.maximize(Meta.MaximizeFlags.BOTH)` / `unmaximize(...)` | `w:maximize()` / `w:unmaximize()` (1.1): both axes only |
| `w.make_fullscreen()` / `unmake_fullscreen()` | `w:set_fullscreen(true\|false)` (1.1) |
| `w.make_above()` / `unmake_above()`, `w.stick()` / `unstick()` | `w:set_above(on)`, `w:set_on_all_workspaces(on)` (1.1) |
| `w.unminimize()` | `w:unminimize()` (1.1): does not focus; `w:activate()` does |
| `w.change_workspace_by_index(i, false)` / `w.move_to_monitor(i)` | `w:move_to_workspace(n)` / `w:move_to_monitor(n)` (raise when out of range) |
| a stored `Meta.Window` after `unmanaged` | `w.alive` is false once the window is destroyed; the handle then raises like any dead handle |
| `workspace_manager.get_n_workspaces()`, `get_active_workspace()`, `get_workspace_by_index()` | `shell.workspaces.count()`, `.active()`, `.get(n)`, `.list()` |
| `ws.index()`, `ws.list_windows()`, `ws.activate(time)` | `ws.index`, `ws:windows()`, `ws:activate()`; also `ws.active`, `ws.n_windows` |
| `Main.layoutManager.monitors`, `.primaryMonitor`, `getWorkAreaForMonitor(i)` | `shell.monitors.list()`, `.primary()`, `.get(n)`: plain snapshot tables `{index, x, y, width, height, scale, primary, work_area = {x, y, width, height}}`; call again after `monitors.changed` |

Actions run on the next frame, and from a `window.*` or `workspace.*` hook
listener they are deferred to idle, so properties may still show the old value
right after the call. The 1.1 geometry and state actions are requests: Mutter
may ignore or adjust them without an error (a fixed-size window keeps its size,
a maximized window keeps its geometry); read `w.frame` afterwards or wait for
`window.geometry-changed` / `window.state-changed`.

```lua
for i, w in ipairs(shell.windows.list()) do
  if w.app_id == "org.gnome.Console.desktop" and not w.minimized then w:minimize() end
end

-- 1.1: put a window in the top-left corner of the primary monitor
local mon = shell.monitors.primary()
local w = shell.windows.focused()
if w and w.resizable then w:move_resize{ x = mon.x, y = mon.y, width = mon.width // 2, height = mon.height } end
```

### Keyboard shortcuts (`docs/api/keybindings.md`, 1.1; no capability)

| JS | Lua |
|---|---|
| `Main.wm.addKeybinding('my-key', settings, Meta.KeyBindingFlags.NONE, Shell.ActionMode.NORMAL \| Shell.ActionMode.OVERVIEW, handler)` | `shell.keybindings.add{ name = "my-key", on_pressed = function(binding) end }`: `name` is a key of type `as` in your schema (the `settings` field defaults to your own; pass a `shell.Settings` for another). The `keybinding` row of `prefs.lua` edits that key and a rebinding takes effect at once |
| `Meta.Display.grab_accelerator('<Super>x', Meta.KeyBindingFlags.NONE)` plus the `accelerator-activated` signal | `shell.keybindings.add{ accelerators = { "<Super>x" }, on_pressed = ... }` (1 to 8 fixed combinations, GTK syntax; mutually exclusive with `name`/`settings`) |
| the third argument, `Shell.ActionMode` flags | `modes = { "normal", "overview" }` (the default; only these two) |
| `Main.wm.removeKeybinding('my-key')`, `display.ungrab_accelerator(id)` | `binding:destroy()` (also done at disable) |
| `settings.get_strv('my-key')` | `binding.accelerators`, `binding.name` |

`add` returns the `shell.Keybinding` or `nil, message` when the combination is
already taken (literal) or the name is already used (settings-backed; names are
one namespace for the session, so prefix yours). Key names of settings-backed
shortcuts must be specific to the extension. At most 16 per extension. The
callback gets the binding, no key and no window; it never runs on the lock
screen or in system modal dialogs; autorepeat does not fire it again.

```lua
local ok, err = shell.keybindings.add{
  accelerators = { "<Super><Alt>o" },
  on_pressed = function() shell.overview.toggle() end,
}
if not ok then shell.log.warning("no shortcut: %s", err) end
```

### Overview and search (`docs/api/overview.md`, `docs/api/search.md`, 1.1)

| JS | Lua |
|---|---|
| `Main.overview.visible` | `shell.overview.visible` (true from the start of the showing animation to the end of the hiding one) |
| (no property) | `shell.overview.target`: `"hidden"`, `"window-picker"` or `"app-grid"`; `shell.overview.animating` (`Main.overview.animationInProgress`) |
| `Main.overview.show()` / `showApps()` | `shell.overview.show{ target = "window-picker" \| "app-grid" }` |
| `Main.overview.hide()` / `toggle()` | `shell.overview.hide()` / `shell.overview.toggle()` |
| `Main.overview.searchController.addProvider(provider)` with `getInitialResultSet`, `getSubsearchResultSet`, `getResultMetas`, `activateResult`, `launchSearch` | `shell.search.add_provider{ id, name, icon_name \| icon_file, on_search = function(provider, terms, respond) end, on_activate = function(provider, result_id, terms) end, on_launch = function(provider, terms) end }`; needs capability `search` |
| `getResultMetas` returning `{id, name, description, createIcon, clipboardText}` | `respond{ { id, name, description, icon_name \| icon_file, clipboard_text } }`, at most 32 results kept, 5 shown |
| `provider.removeProvider()` / `searchController.removeProvider` | `provider:destroy()` (also done at disable) |

The calls return at once; a request made from inside an `overview.*` hook is
deferred to idle, and a listener may still cancel a showing. `show` on an
overview that is shown does nothing. The overview hooks report the extension
that asked as `reason = "extension"` with `owner` = its uuid. A search provider
is asked once per search with the full terms (no sub-search protocol), answers
through `respond` (from any later callback; the answer to a search that was
replaced by a newer one is dropped), and has no widgets of its own.

```lua
shell.search.add_provider{
  id = "notes", name = _("Notes"), icon_name = "accessories-text-editor-symbolic",
  on_search = function(_provider, terms, respond)
    respond{ { id = "n1", name = _("Shopping list"), description = terms[1] } }
  end,
  on_activate = function(_provider, id) shell.log.info("open %s", id) end,
}
```

### Files, processes, HTTP (`docs/api/fs.md`, `spawn.md`, `network.md`, 1.1)

All asynchronous: the options table carries `on_done` / `on_error` (or `on_exit`), nothing runs inside the call, and
callbacks never run after disable.

| JS | Lua |
|---|---|
| `Gio.File.new_for_path(p).query_exists(null)` | `shell.fs.exists(path)` (absolute paths only; no `~`, no relative paths) |
| `file.query_info('standard::type,standard::size,time::modified', ...)` | `shell.fs.info(path)` returns `{type = "file"\|"directory"\|"symlink"\|"special", size, modified}`, `nil` when absent, or `nil, message` |
| `file.load_contents_async(...)`, `GLib.file_get_contents` | `shell.fs.read{ path, max_bytes = 16777216, on_done = function(contents) end, on_error = function(err) end }`; `err = {message, code}` with code `"not-found"`, `"permission-denied"`, `"too-large"`, `"cancelled"` or `"other"`; returns a `shell.Task` (`cancel()`, `alive`) |
| `file.replace_contents_async(...)`, `GLib.file_set_contents` | `shell.fs.write{ path, contents, append = false, on_done, on_error }` (atomic replace; parent directories are created inside `data_dir` only) |
| `file.enumerate_children_async(...)` | `shell.fs.list{ path, on_done = function(entries) end }`, entries `{name, type}` sorted by name |
| `file.delete(null)` | `shell.fs.remove{ path, on_done, on_error }` (files and empty directories) |
| `file.monitor(Gio.FileMonitorFlags.NONE, null)` + `changed` | `shell.fs.watch{ path, on_changed = function(event, path, other_path) end }`, events `"created"`, `"changed"`, `"deleted"`, `"renamed"`; returns a `shell.FileWatch` (`disconnect()`/`destroy()`) |
| `new Gio.Subprocess({argv, flags})` + `communicate_utf8_async`, `GLib.spawn_async`, `GLib.spawn_command_line_sync` | `shell.spawn{ argv = { "cmd", "arg" }, cwd, env = { NAME = "v" }, stdin = "text", timeout_ms, max_output = 1048576, on_exit = function(result) end }` returns a `shell.Process` (`pid`, `alive`, `kill()`, `destroy()`) or `nil, message` when it cannot start; `result = {status, signal, stdout, stderr, timed_out, truncated}` (`status` is nil when a signal killed it). Needs capability `spawn`. No shell is involved: for pipes run `{ "/bin/sh", "-c", script }` |
| `proc.force_exit()`, `proc.send_signal(SIGTERM)` | `process:kill()` (SIGTERM), `process:destroy()` (SIGTERM, SIGKILL after 2 s, no `on_exit`); running processes are killed at disable |
| `new Soup.Session()` + `send_and_read_async(Soup.Message.new('GET', url), ...)` | `shell.network.request{ method = "GET", url, headers = { Name = "v" }, body, content_type, timeout_ms = 30000, max_bytes = 8388608, on_done = function(response) end, on_error = function(err) end }` returns a `shell.Request` (`cancel()`, `destroy()`, `alive`). `response = {status, reason, headers (lower-case names), body, url}`; `err.code` is `"cancelled"`, `"timeout"`, `"too-large"`, `"tls"`, `"resolve"`, `"connection"` or `"other"`. An HTTP 404 or 500 is a response, not an error. Needs capability `network` |

Capabilities, as shown to the user: **`fs`** gates `shell.fs` outside your own
directories. Without it you may read `shell.extension.dir` and read and write
`shell.extension.data_dir`; every other path raises `shell.fs.<function>()
requires the "fs" capability` (paths are canonicalised and symbolic links are
followed before the check). With it, any path the shell can reach. **`spawn`**
gates all of `shell.spawn`; the program runs as the user with the shell's
environment, so treat it like `dbus`. **`network`** gates all of
`shell.network.request`; it reaches `localhost` and the local network as well.
Keep your own state in `shell.extension.data_dir` (settings are better for
anything the user edits).

```lua
-- no capability needed: the data directory is yours
local file = shell.extension.data_dir .. "/seen.txt"
shell.fs.read{ path = file, on_done = function(text) shell.log.info("seen: %s", text) end,
               on_error = function(err) if err.code ~= "not-found" then shell.log.warning("%s", err.message) end end }
shell.fs.write{ path = file, contents = "yes\n" }

-- capability "spawn"
shell.spawn{ argv = { "uptime", "-p" }, timeout_ms = 3000,
             on_exit = function(r) if r.status == 0 then shell.log.info("%s", r.stdout) end end }

-- capability "network"
shell.network.request{ url = "https://example.org/status", timeout_ms = 10000,
  on_done = function(res) shell.log.info("HTTP %d", res.status) end,
  on_error = function(err) shell.log.debug("%s (%s)", err.message, err.code) end }
```

### Signals to hooks (`docs/api/hooks.md`)

`shell.hooks.on(name, fn)` listens after the fact; `shell.hooks.before(name,
fn)` is for cancellable hooks (those named with a present participle) and may
return `shell.hooks.CANCEL` or a table of amendments. `fn(payload)` gets a
table with the **hyphenated keys exactly as written below** (the one place the
API is not `snake_case`: `ev["window-id"]`). Connecting to a hook the shell does
not declare raises `unknown hook`; feature-detect with `shell.hooks.list()`
(entries `{name, cancellable, doc, schema}`), as `examples/hello@gnome-lua.example`
does. `W` below is the window snapshot: `window-id`, `title`, `wm-class`,
`app-id` (omitted when unknown), `workspace` (omitted for all workspaces),
`monitor`, `pid` (omitted when unknown).

| JS signal | Hook | Payload keys |
|---|---|---|
| `Main.overview` `showing` (and patching `show()`) | `overview.showing` (cancellable; amend `target`) | `reason`, `target`, and `owner` when `reason` is `"extension"` (1.1) |
| `Main.overview` `shown` | `overview.shown` | `target` |
| `Main.overview` `hiding` | `overview.hiding` (cancellable) | `reason`, and `owner` when `reason` is `"extension"` (1.1) |
| `Main.overview` `hidden` | `overview.hidden` | none |
| `display` `notify::focus-window` | `window.focused` | W + `previous-window-id` |
| `display` `window-created`, actor `map` | `window.mapped` | W |
| `window` `unmanaged`, actor `destroy` | `window.unmapped` | W (the window is gone) |
| `global.window_manager` `minimize` / `unminimize` | `window.minimized` / `window.unminimized` | W |
| `window` `notify::title` | `window.title-changed` | W + `old-title` |
| `window` `workspace-changed` | `window.workspace-changed` | W + `from`, `to` |
| `display` `window-entered-monitor` | `window.monitor-changed` | W + `from`, `to` |
| `notify::maximized-*`, `notify::fullscreen`, `notify::above`, `notify::on-all-workspaces` | `window.state-changed` | W + `maximized`, `fullscreen`, `changed`, and since 1.1 `above`, `on-all-workspaces` |
| `window` `position-changed`, `size-changed` | `window.geometry-changed` (1.1; at most once per frame per window) | W + `x`, `y`, `width`, `height` (the frame) |
| `workspace_manager` `workspace-switched` | `workspace.switched` | `from`, `to` |
| `workspace-added` / `workspace-removed` | `workspace.added` / `workspace.removed` | `index` |
| `Main.layoutManager` `monitors-changed` | `monitors.changed` | `count`, `primary` |
| `Main.layoutManager` `startup-complete` | `shell.started` | none |
| shell shutdown | `shell.shutting-down` | none |
| `Main.sessionMode` `updated` | `session.mode-changed` | `mode` |
| a `Gio.FileMonitor` on `/etc/localtime` | `time.timezone-changed` (1.1) | `timezone` (the new identifier) |
| `Main.messageTray` banner requests | `notification.showing` (cancellable: no banner, no sound), `notification.shown`, `notification.activated` | `notification-id`, `source`, `urgency`, `resident`, `transient`, `owner`, `app-id`; never title or body |
| `Panel._onMenuSet`, `statusArea` additions | `panel.menu-opened`, `panel.indicator-added` | `name`, `box` (+ `owner` for `indicator-added`, empty for the shell's own) |
| quick settings menu `open-state-changed` | `quick-settings.opened` | none |
| dash `_redisplay` | `dash.item-added` | `kind`, `owner`, `index` (+ `app-id` for apps) |
| `Main.screenshotUI`, `Main.keyboard` | `screenshot.taken`, `osk.shown`, `osk.hidden` | `kind`, `clipboard`, `file`; `monitor` |
| what the user types into the search entry | `search.query`, observe only; capability `search` (to provide results use `shell.search.add_provider`) | `query`, `terms` |

Notes: since 1.1 `window.mapped` is replayed to a new listener: the first
thing it gets, from an idle after `shell.hooks.on` returned, is one event per
existing window in `shell.windows.list()` order, so one listener sees "every
window, now and later" (on a 1.0 shell read them with `shell.windows.list()` in
`enable()`); an after hook runs once
the new state is visible through the API (`shell.windows.focused()` already
returns the new window inside `window.focused`); `reason` and `target` are
open enumerations, treat an unknown value as `"other"`; a cancel of
`overview.hiding` is honoured only for `hot-corner`, `activities-button`,
`overlay-key`, `keybinding` and `gesture` (never for `extension`: a hiding an
extension asked for cannot be cancelled).

```lua
-- Replaces a patch of the hot corner
shell.hooks.before("overview.showing", function(ev)
  if ev.reason == "hot-corner" then return shell.hooks.CANCEL end
end)

shell.hooks.on("window.focused", function(ev)
  local w = shell.windows.get(ev["window-id"])
  if w then shell.log.info("focus: %s (%s)", w.title, w.app_id or "?") end
end)
```

## 4. Ownership and teardown in practice

Everything you create through `shell.*` (widgets, indicators, menu items,
quick-settings items, dash buttons, notifications, timers, settings and hook
connections, D-Bus subscriptions, names and exported objects, and since 1.1
keybindings, search providers, file watches, pending file tasks, running
processes and HTTP requests) belongs to the
extension, not to the Lua value. At disable the shell, in this order: runs
your `disable()` if there is one, destroys your objects in reverse creation
order, disconnects callbacks and cancels timers (no callback runs after
disable begins; pending D-Bus calls, file tasks and HTTP requests are cancelled
and running processes are killed, all without callbacks), then closes the Lua
state.
Nothing you created is visible afterwards.

What you stop writing:

| In JS you wrote | Now |
|---|---|
| `GLib.source_remove(this._id)` | nothing |
| `settings.disconnect(id)`, `proxy.disconnectSignal(id)`, `Main.overview.disconnect(id)` | nothing (`conn:disconnect()` exists for cutting one early) |
| `this._indicator.destroy(); this._indicator = null` | nothing |
| `Gio.bus_unown_name(id)`, `exported.unexport()` | nothing |
| `source.destroy()` for notifications | nothing |
| `injectionManager.clear()` | not applicable: no injection |
| `disable()` | optional; keep it for state *outside* the shell, e.g. restoring a key of another schema you changed through `shell.settings.open(...)` (see `examples/hooks-log@gnome-lua.example` for where it goes) |

Rules in practice:

- **Dropping a reference does not destroy.** `local l = ui.label{}` then `l = nil`
  leaves the label owned. A widget that was never placed is invisible but still
  counts against the per-extension cap of live objects (reference value 4096; the
  runtime raises beyond it). Destroy what you no longer need.
- **Destroying a parent destroys its children**: their handles die. `ind:destroy()`
  destroys its child and menu; destroying a quick-settings slider or menu
  toggle destroys its menu; `box:clear()` and `menu:clear()` destroy all
  children; `ind.child = other` destroys the previous child. `w:remove()`
  detaches without destroying and the widget may be added again.
- **`destroy()` is idempotent and frees the slot at once** (an indicator `name`,
  a dash button's place). A destroyed quick-settings item or dash button cannot
  be re-added: create a new one, or use `visible`.
- **`alive`**: every handle has `alive` (boolean, read-only, never raises). It is
  false after `destroy()`, after a parent's destroy, at disable, and (for a
  `shell.Window`) once the window is destroyed. Using a dead handle raises
  `handle to <type> is no longer valid`; only `alive`, `destroy()`, `tostring` and
  `==` are safe. Test before using a handle you stored; for windows store
  `w.id` and call `shell.windows.get(id)`.
- **Deferred destroy from inside a hook or callback** (README rule 8): a call that
  would destroy or reparent the object currently emitting is deferred until the
  callback returns, and the handle stays valid until then. You may destroy an
  item inside its own `on_activate`, an indicator inside its own `on_click`, a
  notification inside `on_activate`; `on_dismissed` runs with a valid handle
  and the handle is dead afterwards. Window and workspace actions (`w:close()`,
  `w:activate()`, `ws:activate()`, ...) called from a `window.*` or
  `workspace.*` hook are deferred to idle. Do not rely on the effect being
  visible on the next line.
- **Callbacks given at construction live as long as the object.** Connections
  from `h:on_<event>(fn)`, `settings:on_changed`, `shell.hooks.on` are owned and
  die at disable; call `conn:disconnect()` only to stop early.
- **Session modes**: with `"session-modes"` listing more than `["user"]` the
  extension is torn down and enabled again on mode changes, in a fresh state.
- **Errors**: the callback's error is logged with a traceback and counted; more
  than 5 and the extension goes to ERROR. Use `pcall` yourself around an API
  call that may legitimately raise (e.g. `w:move_to_workspace(n)` out of range).

## 5. What has no equivalent, on purpose

| JS habit | Here | Instead |
|---|---|---|
| `Main.panel.add_child`, `Main.layoutManager.addChrome`, `uiGroup`, `global.stage`, any `St`/`Clutter` object | none: no handle to any GObject by any route | compose `shell.ui` widgets inside an indicator, menu or quick-settings menu (`docs/api/ui.md`); floating or overlay widgets do not exist |
| `GObject.registerClass` widgets, vfunc overrides, `allocate` | none | compose widgets; CSS for look; `icon_file` for bundled art |
| `actor.ease()`, transitions, `Clutter.Actor.set_position`, effects, `Cogl`, `St.DrawingArea` | none | CSS, `x_expand`/`x_align`; a fixed `transition` property may come later |
| `button-press-event`, `key-press-event` on arbitrary actors | none (raw input is a keylogging channel) | `on_clicked`, `on_activate`, `on_changed`; global shortcuts are `shell.keybindings` (1.1), which tells you that your shortcut fired, not which key was typed |
| `InjectionManager`, prototype patching, overriding `Main.*` methods | impossible | hooks: `shell.hooks.before` to cancel or amend, `shell.hooks.on` to react |
| Hiding, reordering or replacing built-in indicators, the date menu, the Activities button, built-in quick-settings items | none | add your own items; built-ins never move relative to extension groups |
| Key release, chords (`<Super>x, y`), "any key" grabs, `Main.wm.setCustomKeybindingHandler`, overriding or taking over a shortcut of the shell or of Mutter | none: `shell.keybindings.add` reports a taken combination (`nil, message`), never takes it over; it never runs on the lock screen, in system modal dialogs or while a shell menu is open | pick another combination; let the user rebind a settings-backed one in `prefs.lua` |
| Typing into the search entry, focusing it, selecting an app or workspace in the overview, changing the layout or content of the window picker or the app grid, `Main.overview.dash` internals | none: `shell.overview` shows, hides and reports the state | `shell.overview.show/hide/toggle`, the `overview.*` hooks, `hide_overview` on a dash button |
| Search providers with their own widgets, grid results, context menus, results that change after `respond`, `getSubsearchResultSet` | none: one asynchronous `respond` of plain tables, at most 32 results kept and 5 shown | `shell.search.add_provider`; `search.query` to observe what is typed (both need `search`) |
| `import ... from 'gi://...'`: `Gio`, `GLib`, `Soup`, `Gtk`, `Clipboard`, `GObject` | no GI | `shell.fs` (exists, info, read, write, list, remove, watch: absolute paths, whole files, no directories outside `data_dir`, no rename, copy or recursion), `shell.spawn`, `shell.network.request` (no cookies, WebSockets, streaming or custom certificate handling), `shell.time`, `shell.dbus`, `shell.apps.launch`; no clipboard API (a search result can set `clipboard_text`) |
| `os.time`, `os.date`, `os.clock`, `io.open`, `os.execute` | the sandbox has no `os` and no `io` | `shell.time`, `shell.fs`, `shell.spawn` (1.1) |
| Synchronous calls of any kind: `Sync` proxy calls, `load_contents`, `communicate_utf8`, a nested main loop to wait for a reply | none; the watchdog would kill you first | callbacks (`on_reply`, `on_done`, `on_error`, `on_exit`); start the second step from the first one's callback |
| Detached processes that outlive the extension, interactive standard input, streaming output | none: a process running at disable is killed, `stdin` is a string written once, output arrives whole in `on_exit` | `systemd-run --user` through `shell.spawn`, `shell.apps.launch` |
| Tiling and half-maximizing, maximizing on one axis, interactive moves and resizes, window actors, window contents, screenshots | none: window management stays in the shell | geometry and state actions (`move`, `resize`, `move_resize`, `maximize`, `unmaximize`, `set_fullscreen`, `set_above`, `set_on_all_workspaces`, `unminimize`, 1.1) plus the older five, and the hooks; Mutter may adjust or ignore a request |
| Creating, removing, reordering or renaming workspaces (`workspace_manager.append_new_workspace`, `reorder_workspace`) | none: dynamic workspaces are Mutter's | `w:move_to_workspace(n)` within the existing ones, `ws:activate()` |
| Vetoing a window being closed, focused or moved (`before` hooks on windows) | none: window hooks are informational | react afterwards (`window.focused`, `window.geometry-changed`) and call an action |
| Other extensions' indicators, `Extension.lookupByUUID` | none | extensions must not depend on each other |
| Reading other applications' notifications, `Urgency.CRITICAL`, custom banners, sounds | none; hooks never carry title or body | `shell.notify` for your own |
| Dash drag-and-drop, dash context menus, per-monitor panels, panel height | none | |
| `prefs.js` with Gtk widgets, buttons, about pages, file choosers | none | declarative `prefs.lua` (section 8) |

**Asking for an API addition.** The surface grows by minor versions
(`shell-api` `"1.N"`), additively, and an addition is proposed as a change to
the API documents in `docs/api/` (a new row or section first, then the
implementation that matches it). A good proposal contains:

1. The use case: what the user sees and why no existing `shell.*` call or hook
   can do it.
2. The JS you would have written (the shell objects it touches).
3. The shape you propose, following `docs/api/README.md`: for state change
   a hook named `<subject>.<event>` (present participle for a cancellable
   `before`, past tense for informational), lower-case, hyphens inside a part,
   payload keys of basic types referring to objects by id, never by pointer
   (`docs/CONVENTIONS.md`, "Hooks"); for an action a function taking one options
   table, `snake_case`, 1-based indices.
4. Whether it needs a capability.

Every accepted hook gets a row in `docs/api/hooks.md` in the same change that
declares it.

## 6. Worked example: Break Reminder

A top-bar indicator with a minute counter and a menu (a switch and an
action), a timer, a setting for the interval, and a notification. This is
the typical upstream shape (`gnome-extensions create --template indicator`,
extended).

### JS (GNOME 45+ ESM)

`extension.js`:

```js
import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Clutter from 'gi://Clutter';
import St from 'gi://St';

import {Extension, gettext as _, ngettext} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';

const Indicator = GObject.registerClass(
class Indicator extends PanelMenu.Button {
    _init(settings) {
        super._init(0.0, _('Break Reminder'));
        this._settings = settings;
        this._minutes = 0;

        const box = new St.BoxLayout();
        box.add_child(new St.Icon({
            icon_name: 'alarm-symbolic',
            style_class: 'system-status-icon',
        }));
        this._label = new St.Label({text: '0', y_align: Clutter.ActorAlign.CENTER});
        box.add_child(this._label);
        this.add_child(box);

        const remind = new PopupMenu.PopupSwitchMenuItem(_('Remind me'), false);
        this._settings.bind('remind', remind, 'state', Gio.SettingsBindFlags.DEFAULT);
        this.menu.addMenuItem(remind);

        const took = new PopupMenu.PopupMenuItem(_('I took a break'));
        took.connect('activate', () => this._reset());
        this.menu.addMenuItem(took);

        this._timeoutId = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 60, () => {
            this._tick();
            return GLib.SOURCE_CONTINUE;
        });
    }

    _reset() {
        this._minutes = 0;
        this._label.text = '0';
    }

    _tick() {
        this._minutes++;
        this._label.text = `${this._minutes}`;

        const interval = this._settings.get_int('interval');
        if (this._settings.get_boolean('remind') && this._minutes % interval === 0) {
            Main.notify(_('Time for a break'),
                ngettext('You have been working for %d minute',
                    'You have been working for %d minutes', this._minutes).format(this._minutes));
        }
    }

    destroy() {
        if (this._timeoutId) {
            GLib.source_remove(this._timeoutId);
            this._timeoutId = null;
        }
        super.destroy();
    }
});

export default class BreakReminderExtension extends Extension {
    enable() {
        this._indicator = new Indicator(this.getSettings());
        Main.panel.addToStatusArea(this.uuid, this._indicator);
    }

    disable() {
        this._indicator?.destroy();
        this._indicator = null;
    }
}
```

`metadata.json` carries `"shell-version": ["46", "47", "48"]`; the schema
`schemas/org.example.break-reminder.gschema.xml` has the keys `remind` (`b`)
and `interval` (`i`, range 1..240); `prefs.js` is a Gtk/Adw window (not shown).

### Lua

`metadata.json`:

```json
{
  "uuid": "break-reminder@example.org",
  "name": "Break Reminder",
  "description": "Reminds you to take a break.",
  "shell-api": "1",
  "settings-schema": "org.example.break-reminder",
  "gettext-domain": "break-reminder@example.org",
  "version": 4
}
```

`schemas/org.example.break-reminder.gschema.xml` (unchanged from the JS
extension; compile with `glib-compile-schemas schemas/`):

```xml
<schemalist>
  <schema id="org.example.break-reminder" path="/org/example/break-reminder/">
    <key name="remind" type="b"><default>false</default></key>
    <key name="interval" type="i"><range min="1" max="240"/><default>45</default></key>
  </schema>
</schemalist>
```

`extension.lua`:

```lua
local shell = require("shell")
local ui = shell.ui

local ext = {}

function ext.enable()
  local settings = shell.settings.extension()
  local minutes = 0

  local label = ui.label{ text = "0", y_align = "center" }
  local indicator = shell.panel.add_indicator{
    accessible_name = _("Break Reminder"),
    child = ui.box{ children = {
      ui.icon{ icon_name = "alarm-symbolic", style_class = "system-status-icon" },
      label,
    } },
  }

  local function reset()
    minutes = 0
    label.text = "0"
  end

  -- settings.bind() has no equivalent: write the setting from the widget and
  -- the widget from the setting. Assignments are silent (README rule 7), so
  -- the two directions cannot loop.
  local remind = indicator.menu:add_switch{
    label = _("Remind me"),
    state = settings:get_boolean("remind"),
    on_toggled = function(_item, on) settings:set_boolean("remind", on) end,
  }
  settings:on_changed("remind", function(_key, on) remind.state = on end)

  indicator.menu:add_item{ label = _("I took a break"), on_activate = reset }

  shell.timer.every{ ms = 60 * 1000, callback = function()
    minutes = minutes + 1
    label.text = tostring(minutes)

    if settings:get_boolean("remind") and minutes % settings:get_int("interval") == 0 then
      shell.notify{
        title = _("Time for a break"),
        body = ngettext("You have been working for %d minute",
                        "You have been working for %d minutes", minutes):format(minutes),
      }
    end
  end }
end

return ext
```

`prefs.lua` (replaces `prefs.js`, section 8):

```lua
return {
  groups = {
    { title = _("Reminder"), rows = {
      { type = "switch", key = "remind", title = _("Remind me") },
      { type = "spin", key = "interval", title = _("Interval"),
        subtitle = _("Minutes between reminders"), min = 5, max = 240, step = 5,
        depends_on = "remind" },
    } },
  },
}
```

What went away: the `GObject.registerClass` subclass, `_init`/`super._init`,
`destroy()` with `GLib.source_remove`, `disable()` with the null assignments,
`Main.panel.addToStatusArea`, `Gio.SettingsBindFlags`, the `.format()` call
on a translated string (now `string.format` through `:format`), and the whole
`prefs.js`. What stayed: the schema, the strings, the structure of the menu.
The timer callback does not return a value to continue (only `false` stops a
`shell.timer.every`); the counter is reset by a closure upvalue instead of a
class field.

Check before shipping: `gnome-extensions prefs <uuid>` validates `prefs.lua`
against the compiled schema (the window shows the problems); `pack` does not.

## 7. Looking Glass

Alt+F2, `lg`, Enter (Esc in the Evaluator leaves). Full text:
`docs/looking-glass.md`. The Evaluator pane is the Lua prompt (there is no
JS evaluator and no separate "Lua" page any more); it runs in the
*inspector state*, which is **not sandboxed** (`os`, `io`, `debug`, the whole
standard library) and has the same `shell` table, so you can try any call of
this guide live.

| Task | In the Evaluator |
|---|---|
| Evaluate | input is first tried as an expression (`return <input>`), otherwise run as a statement; several return values are several results |
| Results | `r(n)` (counting from 1) and `it` (the last); click a result to inspect it (*Back*, *Insert*) |
| Try the API | `shell.windows.list()`, `shell.windows.focused().title`, `shell.workspaces.count()`, `shell.monitors.primary()`, `shell.apps.running()`, `shell.hooks.list()` |
| See which hooks exist | `shell.hooks.list()` (entries `{name, cancellable, doc, schema}`) |
| Fake an event to your listeners | `shell.hooks.emit_after("quick-settings.opened", {})` (and `emit_before`): inspector only, for testing; sandboxed states get an error |
| Complete | Tab completes globals, table fields (`shell.hooks.`) and object properties and methods; pressing it twice lists the options |
| Traceback | `print(debug.traceback("here"))` |
| Find a CSS class or the actor behind a widget | the Picker (select an object on screen); `it.style_class`, `it:gtype()`, `it:properties()`; only the classes listed in `docs/api/ui.md` are stable API |
| Globals | `global`, `stage`, `st_settings`, `main`, `window_actors()`, `inspect(x, y)` |
| Slow animations | `st_settings.slow_down_factor = 5` |
| Handles | a handle exposes the properties of its type as fields (dashes become underscores) and no calls to arbitrary functions; it turns dead (`<gobject.StButton (dead)>`) when the object goes |

The Evaluator is for inspection: the extension API itself is still the only
way an extension reaches the shell, and `shell.extension` is `nil` there.

Other panes: **Extensions** lists each Lua extension with its state, its Lua
heap and number of errors, *View Source* and *Show Errors* (the last
traceback, usually the quickest answer to "why was my extension disabled
after 5 errors"); **Windows**, **Actors** (the whole actor tree), **Flags**
(Mutter and Clutter debug flags). The trash can button collects the garbage
of the inspector state and of every enabled extension's state.

Outside Looking Glass:

```sh
# log lines of your extension (domain shell-lua/<uuid>)
journalctl -f -o cat _COMM=gnome-shell | grep 'shell-lua/break-reminder@example.org'

# run a nested shell that reads extensions straight from a directory that
# contains break-reminder@example.org/
SHELL_LUA_EXTENSION_PATH=$PWD dbus-run-session gnome-shell --wayland --devkit

# stacks (with the Lua traceback of the running extension) on every warning or critical
SHELL_DEBUG=backtrace-warnings dbus-run-session gnome-shell --wayland --devkit
```

`SHELL_LUA_EXTENSION_PATH` is a colon-separated list of directories that
replaces the default search path; the directory name must equal the uuid
(`subprojects/shell-lua/examples/README.md`). A syntax error in
`extension.lua`, or an error in `enable()`, leaves the extension in state ERROR
and records the message (the Extensions pane shows the state).

## 8. Preferences: `prefs.lua`

`prefs.js` built an `Adw.PreferencesWindow` in `fillPreferencesWindow(window)`.
`prefs.lua` returns a **data table**; the shell renders it with libadwaita in
the extensions service. Your code does not run in the prefs process, there are
no handlers, and every row is bound to one key of `settings-schema`. React to
changes in `extension.lua` with `shell.settings.extension():on_changed(...)`.

| JS prefs | `prefs.lua` |
|---|---|
| `Adw.PreferencesPage` / `Adw.PreferencesGroup` | `pages = { { title, icon_name, groups = {...} } }`; one page without a title: top-level `groups = {...}` |
| `Adw.SwitchRow` + `settings.bind(key, row, 'active', ...)` | `{ type = "switch", key, title, subtitle? }` (key `b`) |
| `Adw.SpinRow` + `bind(..., 'value')` | `{ type = "spin", key, title, min, max, step, digits }` (numeric keys; bounds default to the schema `<range>`) |
| `Adw.ComboRow` with a model and manual mapping | `{ type = "combo", key, title, choices = { { value = "24h", label = _("24-hour") } } }` (key `s` or integer; with `<choices>`/`<enum>` every value must be one of them) |
| `Adw.EntryRow` + `bind(..., 'text')` | `{ type = "entry", key, title, max_length? }` (key `s`; written on Enter or focus-out) |
| `Gtk.ColorDialogButton` + string conversion | `{ type = "color", key, title, alpha? }` (key `s`; stores `#rrggbb` or `rgba(r, g, b, a)`) |
| a shortcut capture row | `{ type = "keybinding", key, title, max = 1..4, allow_empty }` (key `as`; it edits the key, it does not register the shortcut: register it in `extension.lua` with `shell.keybindings.add{ name = key, ... }`, 1.1) |
| `row.sensitive = ...` / `bind_property` on another key | `depends_on = "key"`, `"!key"` (booleans) or `{ key = "k", value = v }`; an inactive row is insensitive, not hidden |
| `window.set_title`, `window.add(page)` | top-level `title` (default: metadata `name`) |
| `ExtensionPreferences.gettext` | `_`, `ngettext`, `pgettext`, resolved at load time with `locale/` |
| action rows, buttons, about pages, lists, file choosers | none (rows without a settings key are not in v1) |

`prefs.lua` is loaded in its own sandbox that has only `shell.api_version`,
`shell.extension`, `shell.log`, `shell.gettext` and the globals `_`,
`ngettext`, `pgettext` (no `shell.ui`, `shell.settings`, `shell.hooks`), must
return a table synchronously within the 500 ms watchdog, and may contain only
strings, numbers, booleans and tables (a function, userdata or cycle is a
validation error).

Validation (by the prefs service when it opens the window, as
`gnome-extensions prefs <uuid>` does, with paths such as `pages[2].groups[1].rows[3].key`):
only documented fields; strings non-empty and at most 200 bytes (`subtitle`,
`description` 500); `settings-schema` set and compiled in `schemas/`; every
`key` in the schema, of a type allowed for the row, **bound by at most one
row**; `spin` bounds inside the schema `<range>` (required when there is
none), integer keys with integer bounds and step; at most 8 pages, 16 groups
per page, 64 rows per group. A failing file shows an error page (problems, a
Copy Error button, a link to the extension's `url`) and never disables the
extension. The window's menu has "Reset to Defaults". Rows bind two-way with
`Gio.Settings.bind`, so edits made in dconf show up live and each user edit is
written at once; a key that is not writable makes its row insensitive.

Full reference: `docs/api/prefs.md`; examples: `examples/hello@gnome-lua.example`
(three rows), `examples/prefs@gnome-lua.example` (every row type, two pages,
`depends_on` in all three forms).

## Reference extensions

All in `subprojects/shell-lua/examples/`; `tests/api/examples.lua` installs and
enables each of them in a headless shell.

| Example | Shows |
|---|---|
| `hello@gnome-lua.example` | minimal extension, timer, settings, feature-detecting a hook, prefs |
| `indicator@gnome-lua.example` | indicator with menu (action, switch, submenu with choice, separator, image item), widgets, stylesheet |
| `clock@gnome-lua.example` | 1.1: `shell.time` clock refreshed on the minute and on `time.timezone-changed`, a keybinding that toggles `shell.overview` |
| `quick-toggle@gnome-lua.example` | menu toggle with header, slider and status icon kept in sync with settings both ways |
| `dash-launcher@gnome-lua.example` | two dash buttons, `shell.apps` |
| `hooks-log@gnome-lua.example` | only hooks (`on` and `before`), `disable()` |
| `prefs@gnome-lua.example` | every `prefs.lua` row type |
