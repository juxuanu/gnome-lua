# `prefs.lua`: declarative preferences (version 1, draft)

Scope: how an extension describes its settings window. `prefs.lua` returns a
plain data table (pages, groups, rows); every row is bound to one key of the
extension's GSettings schema (`metadata["settings-schema"]`, compiled from
`schemas/`). The extensions D-Bus service renders it with libadwaita; the
extension runs no code in the prefs process (PLAN D12). This replaces
`prefs.js` with `ExtensionPreferences.fillPreferencesWindow(window)` (today
`js/extensions/prefs.js`, hosted by `ExtensionPrefsDialog`, an
`Adw.PreferencesWindow`, in `js/dbusServices/extensions/extensionPrefsDialog.js`).

Not in v1:
- Custom GTK/Adw widgets, signals, handlers or any callback: the file is data.
  Behaviour that reacts to a setting lives in `extension.lua` via
  `shell.settings.extension():on_changed(...)`.
- Rows without a settings key (buttons, "open folder", about pages, static
  labels, links), progress, lists the user can add to/reorder, file choosers,
  sub-pages and dialogs.
- Settings of schemas other than `settings-schema`, and relocatable schemas.
- Per-row validation callbacks; validation is the schema's type and range.
- Registering the keybinding (the `keybinding` row only edits the `as` key;
  the global shortcut registration API is not in 1.0, a future
  `shell.keybindings`).
- Live preview in the shell, window size, window title other than `title`,
  `Adw.PreferencesWindow` search configuration, custom styling.

## Loading

`prefs.lua` is loaded in a sandboxed state (SANDBOX.md) holding only
`shell.api_version`, `shell.extension`, `shell.log`, `shell.gettext` and the
globals `_`, `ngettext`, `pgettext` (strings are translated at load time with
the extension's `locale/` and the user's language). `shell.ui`, `shell.timer`,
`shell.settings`, `shell.hooks` and all shell-surface tables are absent. It must
return a table synchronously under the 500 ms watchdog. The result is converted
to a data tree: strings, numbers, booleans, tables; a function, userdata or
cycle anywhere is a validation error.

## Schema

```
prefs   = { title?, pages = {page+}  |  groups = {group+} }   -- `groups` = one untitled page
page    = { title (required with >1 page), icon_name?, groups = {group+} }
group   = { title?, description?, rows = {row+} }
row     = { type, key, title, subtitle?, depends_on?, ...type fields }
```

`title` (prefs) is the window title, default metadata `name`. A page `icon_name`
is a themed symbolic icon shown in the window switcher. `depends_on` is either a
key name (boolean key, row active while it is `true`; `"!key"` while `false`) or
`{ key = <string>, value = <any> }` (active while that key equals `value`). An
inactive row is insensitive, not hidden. The referenced key need not have a row.

| `type` | Widget | Key type | Fields |
|---|---|---|---|
| `switch` | `Adw.SwitchRow` | `b` | |
| `spin` | `Adw.SpinRow` | `n q i u x t y d` | `min`, `max` (default: the schema `<range>`), `step` (default 1; 0.1 for `d`), `digits` (0; 2 for `d`) |
| `combo` | `Adw.ComboRow` | `s`, or an integer type | `choices` = `{ {value = <key type>, label = string}, ... }` |
| `entry` | `Adw.EntryRow` | `s` | `max_length` (integer, optional) |
| `color` | `Adw.ActionRow` + `Gtk.ColorDialogButton` | `s` | `alpha` (boolean, default false) |
| `keybinding` | `Adw.ActionRow` + shortcut label | `as` | `max` (1..4, default 1), `allow_empty` (default true) |

Behaviour of the renderer:
- Rows bind two-way with `Gio.Settings.bind` (DEFAULT flags): changes made
  elsewhere (dconf, another window) update the row live; each user edit is
  written immediately. `entry` writes on Enter or focus-out. A key that is not
  writable (admin lockdown, `Gio.Settings.is_writable`) makes the row
  insensitive.
- `color` stores `#rrggbb`, or `rgba(r, g, b, a)` when `alpha` is true; any CSS
  colour parseable by `Gdk.RGBA` is accepted when reading, an unparseable value
  shows as unset and is not rewritten.
- `combo` with a current value outside `choices` shows no selection and does
  not write until the user picks.
- `keybinding` shows the accelerators (GTK syntax, `<Super>k`) and a capture
  dialog: a key press sets it, Escape cancels, Backspace clears (if
  `allow_empty`). It warns, but does not block, when the accelerator is already
  used by `org.gnome.desktop.wm.keybindings`, `org.gnome.shell.keybindings`,
  `org.gnome.mutter.keybindings` or `org.gnome.settings-daemon.plugins.media-keys`.
- The window's menu offers "Reset to defaults", resetting every bound key.

## Validation

Performed identically by the prefs service, `gnome-extensions pack` (as errors)
and `gnome-extensions prefs`; all problems are collected and reported with a
path such as `pages[2].groups[1].rows[3].key`. A failing file shows the
existing error page (`ExtensionPrefsErrorPage`); it never disables the
extension.

1. The file returns a table; only the fields above exist (unknown field =
   error); required fields present; strings are non-empty and at most 200 bytes
   (`subtitle`, `description` 500).
2. `metadata.json` has `settings-schema` and `schemas/` contains the compiled
   schema with that id.
3. `key` exists in the schema; its type is listed for the row type; **a key is
   bound by at most one row**.
4. `spin`: `min <= max`; `step > 0`; bounds lie inside the schema `<range>`
   when there is one; `min` and `max` are required when there is none; integer
   key types require integer bounds.
5. `combo`: at least one choice; `value`s are unique and match the key type; for
   a key with `<choices>`/`<enum>` every `value` MUST be one of them.
6. `keybinding`: `max` in 1..4.
7. `depends_on.key` exists; `value` converts to that key's type (variant.md).
8. Limits: at most 8 pages, 16 groups per page, 64 rows per group.

## Example

`metadata.json` has `"settings-schema": "org.example.clock"`; `schemas/org.example.clock.gschema.xml`:

```xml
<schemalist><schema id="org.example.clock" path="/org/example/clock/">
  <key name="show-seconds" type="b"><default>false</default></key>
  <key name="format" type="s"><choices><choice value="24h"/><choice value="12h"/></choices><default>"24h"</default></key>
  <key name="update-interval" type="i"><range min="1" max="60"/><default>1</default></key>
  <key name="label" type="s"><default>""</default></key>
  <key name="accent" type="s"><default>"#3584e4"</default></key>
  <key name="toggle-shortcut" type="as"><default>["&lt;Super&gt;c"]</default></key>
</schema></schemalist>
```

```lua
-- prefs.lua
return {
  pages = {{
    title = _("General"), icon_name = "preferences-system-symbolic",
    groups = {
      { title = _("Display"), rows = {
        { type = "switch", key = "show-seconds", title = _("Show seconds") },
        { type = "combo", key = "format", title = _("Time format"),
          choices = { { value = "24h", label = _("24-hour") }, { value = "12h", label = _("12-hour") } } },
        { type = "entry", key = "label", title = _("Prefix"), max_length = 16 },
        { type = "color", key = "accent", title = _("Accent colour") },
      } },
      { title = _("Behaviour"), description = _("How often the clock refreshes"), rows = {
        { type = "spin", key = "update-interval", title = _("Update interval"),
          subtitle = _("Seconds"), min = 1, max = 60, depends_on = "show-seconds" },
        { type = "keybinding", key = "toggle-shortcut", title = _("Toggle shortcut") },
      } },
    },
  }},
}
```
