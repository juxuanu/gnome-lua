# `shell.dash`: buttons in the dash (version 1, draft)

Scope: adding launcher-style icon buttons to the dash (the dock in the
overview). A button is an icon with a label and one action. It is not an
application icon: it has no `Shell.App`, no running state and no menu. Models
`DashItemContainer` (`src/ui/dash.c`, was `js/ui/dash.js`) hosting a `St.Button`, as `ShowAppsIcon`
does, but in an extension-owned group beside the app icons.

Not in v1:
- Drag-and-drop in any direction (button as drag source, as drop target,
  reordering by the user, dropping apps on a button): `DND` is not exposed.
- Context menu / right-click / middle-click actions, badges, progress, urgency
  highlights, notification counters.
- Reading, reordering, hiding or replacing application icons, the separator or
  the Show Apps button; changing favorites through the dash (use
  `shell.apps.favorites()` for reading; writing favorites is not in v1).
- Placement outside the dash (floating docks, other monitors, vertical docks),
  icon size control (`DASH_*`, `baseIconSizes` pick the size for everyone).
- Custom child widgets (`shell.ui` inside a button): an icon and a label only,
  so sizing and the hover tooltip match app icons.
- Animations other than the built-in fade/scale on add and remove.

## Layout facts that constrain the design

`Dash._dashContainer` holds `_box` (favorites, then a separator, then running
non-favorite apps; `Dash._redisplay` indexes it by *app position*) and the
`ShowAppsIcon`. Inserting foreign children into `_box` would corrupt that
indexing, so extension buttons live in two extension-owned groups next to it:

```
 [ start group ][ favorites | running apps ][ end group ][ Show Apps ]
```

Buttons never count as favorites or running apps, never change the separator
logic, and do not appear in `shell.apps.favorites()`/`running()`. They do take
part in icon-size fitting (`Dash._adjustIconSize` counts every icon child), so
adding buttons can shrink all icons, down to the 16 px minimum, and button
icons always render at the dash's current size. The dash is shown by the
overview; a button is only clickable while the overview shows it. An
extension may create at most 8 buttons (raises beyond).

## Constructors

**`shell.dash.add_button{...}` -> `shell.DashButton`**

| Field | Type | Default | Notes |
|---|---|---|---|
| `label` | string | required | hover tooltip (`dash-label`) and accessible name (`DashItemContainer.setLabelText`) |
| `icon_name` / `icon_file` | string | one required | at most one |
| `on_click` | function(self, button) | nil | primary button only (`button == 1`) |
| `position` | `"start"`, `"end"` | `"end"` | which group |
| `hide_overview` | boolean | `true` | hide the overview after `on_click` returns, as launching an app does (`Main.overview.hide()` in `AppIcon`); set false for buttons that toggle something |
| `checked` | boolean | false | `:checked` pseudo-class, e.g. for an "on" state; no automatic toggling |
| `visible`, `sensitive` | boolean | true | |

**`shell.dash.insert_at{...}` -> `shell.DashButton`**: the same fields plus
`index` (integer >= 1, required): the 1-based position among *this extension's*
buttons in the chosen group; values past the end append, and later buttons shift
right. `add_button` is `insert_at` with the index after the last one. Group
order across extensions follows README.md ("Placement and ordering").

## `shell.DashButton`

| Property | Type | Access |
|---|---|---|
| `label` | string | rw |
| `icon_name`, `icon_file` | string | rw (setting one clears the other) |
| `checked` | boolean | rw |
| `visible`, `sensitive` | boolean | rw |
| `hide_overview` | boolean | rw |
| `position` | `"start"\|"end"` | ro |
| `index` | integer | ro |
| `alive` | boolean | ro |

Methods: `destroy()` (fades out like an app icon being removed, then frees the
slot; the handle is dead immediately), `on_click(fn)`.

Behaviour: the dash label appears on hover after `DASH_ITEM_HOVER_TIMEOUT`
(300 ms) exactly as for app icons. Keyboard focus and Enter/Space activate.
Hook `dash.item-added` fires for buttons too (`kind = "button"`, hooks.md).

## Example

```lua
local shell = require("shell")

shell.dash.add_button{
  label = "Terminal", icon_name = "utilities-terminal-symbolic",
  on_click = function() shell.apps.launch("org.gnome.Console.desktop") end,
}

local night = shell.dash.insert_at{
  index = 1, position = "start", label = "Night mode",
  icon_name = "night-light-symbolic", hide_overview = false,
  on_click = function(self) self.checked = not self.checked end,
}
```
