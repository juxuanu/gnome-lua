# `shell.panel`: top-bar indicators (version 1, draft)

Scope: adding a button to the top bar, optionally with a popup menu. Replaces
`Main.panel.addToStatusArea(role, indicator, position, box)` with a
`PanelMenu.Button` (`js/ui/panel.js` `addToStatusArea`, `js/ui/panelMenu.js`
`Button`). The button's contents are `shell.ui` widgets (ui.md); the menu is a
`shell.Menu` (menu.md). Quick-settings entries are a different surface
(quick-settings.md).

Not in v1:
- Reordering, hiding or replacing built-in indicators (`activities`,
  `dateMenu`, `quickSettings`, ...): they are session-mode dependent
  (`js/ui/sessionMode.js` `panel.left/center/right`) and the layout is not API.
- Interleaving between built-ins: see Placement. `index` is relative to the
  extension's own items.
- Looking up other indicators (`Main.panel.statusArea[role]` of someone else's
  role): extensions must not depend on each other.
- Custom menu alignment (`menuAlignment`): the arrow follows the button centre.
- Drag-reordering by the user, per-monitor panels, panel style/height changes.
- A non-`St` panel child, or a button that is both a menu opener and an action
  button (pick one, see `on_click`).

## `shell.panel.add_indicator{...}` -> `shell.Indicator`

| Field | Type | Default | Notes |
|---|---|---|---|
| `accessible_name` | string | required | non-empty; `PanelMenu.Button(menuAlignment, nameText)` -> ATK role MENU |
| `name` | string | extension uuid | the status area role. `^[A-Za-z0-9_.@-]{1,128}$` |
| `position` | `"left"`, `"center"`, `"right"` | `"right"` | the panel box (`_leftBox`, `_centerBox`, `_rightBox`); unknown value raises |
| `edge` | `"start"`, `"end"` | `"start"` | which end of the box, see Placement |
| `index` | integer >= 1 | after own items | among this extension's items in that box and edge |
| `child` | widget | nil | exactly one of `child`, `icon_name`, `icon_file`, `text` may be given (or none) |
| `icon_name`, `icon_file` | string | | shorthand for `ui.icon{ style_class = "system-status-icon", ... }` |
| `text` | string | | shorthand for `ui.label{ text = ..., y_align = "center" }` |
| `menu` | boolean | `true` unless `on_click` | `true` creates the popup menu and the click gesture (`dontCreateMenu = false`) |
| `on_click` | function(self, button) | nil | makes the indicator a plain button; mutually exclusive with `menu = true` (raises). Primary button only (`button == 1`) |
| `visible` | boolean | `true` | |

Raises if `name` is taken, either by a built-in role (`activities`,
`quickSettings`, `dateMenu`, `a11y`, `keyboard`, `dwellClick`,
`screenRecording`, `screenSharing`: `PANEL_ITEM_IMPLEMENTATIONS`) or by an
earlier indicator, including one of the same extension (today's
`Extension point conflict: there is already a status indicator for role ...`).
An extension that needs several indicators MUST give each a distinct `name`.
The name is the identity used in hook payloads and Looking Glass; it is not a
lookup key for other extensions.

Clicking toggles the menu on press (`Clutter.ClickGesture` with
`recognize_on_press`); an empty menu does not open (`PopupMenuBase.open`
returns false when empty). Left/Right arrow keys move focus between
indicators and Escape closes, handled by the shell. While one panel menu is
open, hovering another indicator switches menus (`Panel._onMenuSet` ->
`menuManager.addMenu`); that membership is automatic.

## `shell.Indicator`

| Property | Type | Access | Notes |
|---|---|---|---|
| `name` | string | ro | status area role |
| `position` | `"left"\|"center"\|"right"` | ro | |
| `child` | widget or nil | rw | replaces and destroys the previous child; assigning a widget with a parent raises |
| `menu` | `shell.Menu` or nil | ro | nil when `menu = false`/`on_click`; created once, lives with the indicator |
| `accessible_name` | string | rw | |
| `visible` | boolean | rw | hiding closes the menu (`PanelMenu.Button.vfunc_hide`) |
| `sensitive` | boolean | rw | `setSensitive`: reactive, focusable, hover tracking |
| `active` | boolean | ro | menu currently open (`:active` pseudo-class) |
| `alive` | boolean | ro | |

Methods: `ind:destroy()` (destroys child and menu, frees the status area
name); `ind:on_click(fn)` (only for `on_click` indicators; otherwise raises).
Menu open/close events are on the menu (`ind.menu:on_open_state_changed(fn)`).

## Placement

Each box is conceptually `[extensions at "start"] [built-ins] [extensions at
"end"]`. Extension indicators are ordered inside a slot as defined in
README.md. Consequences:

- `position = "right", edge = "start"` (the default) lands left of the
  built-in system area, the place `addToStatusArea(..., 0, 'right')` gives today
  (`position ??= 0`).
- `position = "right", edge = "end"` is the far right, right of
  `quickSettings`; `position = "left", edge = "start"` is left of
  `activities`.
- The built-in group never moves relative to extension groups. Today
  `Panel._updateBox` re-appends built-ins on every session-mode update and an
  absolute index would shift; the slot model makes that invisible.
- On a session-mode change the shell re-applies extension placement from the
  recorded `(position, edge, index)` tuples.
- Each panel box is created even if empty (centre may have only `dateMenu`).

Hook `panel.indicator-added` fires for every indicator, built-in or not
(`owner` is the extension uuid, empty for the shell). `panel.menu-opened`
fires when any panel menu opens (hooks.md).

## Example

```lua
local shell = require("shell")

function enable()
  local ind = shell.panel.add_indicator{
    name = "caffeine", position = "right", edge = "start",
    icon_name = "my-caffeine-symbolic", accessible_name = "Caffeine",
  }
  local sw = ind.menu:add_switch{ label = "Keep awake", state = false,
    on_toggled = function(_, on) ind.child.icon_name = on and "my-caffeine-on-symbolic" or "my-caffeine-symbolic" end }
  ind.menu:add_separator{}
  ind.menu:add_item{ label = "Settings", on_activate = function() shell.apps.launch("org.gnome.Settings.desktop") end }
end
return { enable = enable }
```

(`ind.child` is the shorthand-created icon, a `shell.Icon`.)
