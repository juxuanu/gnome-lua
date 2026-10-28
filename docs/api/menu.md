# `shell.Menu`: popup menus (version 1, draft)

Status: **impl** (`src/lua-api/shell-lua-api-menu.c`; conformance:
`tests/api/menu.lua`, fixture `tests/api/fixtures/menu@test`).

Scope: the menu attached to a panel indicator (`ind.menu`, panel.md), to a
quick-settings menu toggle or slider (`toggle.menu`, quick-settings.md), and
the one-level submenu of a submenu item. A menu is a vertical list of items;
items are created by `add_*` calls on the menu and are owned by it. Models
`PopupMenu`/`PopupSubMenu` and the items of `js/ui/popupMenu.js`
(`PopupMenuItem`, `PopupSwitchMenuItem`, `PopupSubMenuMenuItem`,
`PopupSeparatorMenuItem`, `PopupImageMenuItem`).

Not in v1:
- Standalone menus (`shell.menu.new{ source = widget }`) and context menus on
  arbitrary widgets: opening needs grab, focus and box-pointer placement that
  only the panel and quick-settings surfaces provide today.
- `PopupMenuSection`, nested submenus (a submenu's menu has no `add_submenu`;
  raises), `PopupMenuManager`, sliders and calendars as dedicated items: use
  `add_widget` with `shell.ui` widgets.
- Radio groups, accelerators/key hints, per-item tooltips.
- Intercepting key presses, reading the pointer event of an activation
  (`Clutter.Event`), `itemActivated` overrides, `closeOnActivate = false`
  (only Space on a switch row keeps the menu open, as today).
- Replacing built-in menus (date menu, system menu) or adding to them.
- Animation parameters (`open({animate = false})` is the only knob).

## Types

| Type | Extends | Created by |
|---|---|---|
| `shell.Menu` | | indicators, quick-settings menu items, submenu items |
| `shell.MenuItem` | | base of every item |
| `shell.MenuActionItem` | `MenuItem` | `add_item`, `add_image_item` |
| `shell.MenuSwitchItem` | `MenuItem` | `add_switch` |
| `shell.MenuSubmenuItem` | `MenuItem` | `add_submenu` |
| `shell.MenuSeparator` | `MenuItem` | `add_separator` |
| `shell.MenuWidgetItem` | `MenuItem` | `add_widget` |

## `shell.Menu`

| Property | Type | Access | Notes |
|---|---|---|---|
| `is_open` | boolean | ro | `isOpen` |
| `n_items` | integer | ro | `numMenuItems` |
| `sensitive` | boolean | rw | `PopupMenuBase.sensitive`: `false` makes every item insensitive |
| `alive` | boolean | ro | |

Methods (all `add_*` take one table, return the item handle; `index` is
optional, 1-based, inserts before the item currently at that position, values
past the end append):

| Method | Fields | Returns |
|---|---|---|
| `add_item{...}` | `label` (required), `on_activate`, `sensitive`, `visible`, `ornament`, `index` | `MenuActionItem` |
| `add_image_item{...}` | as `add_item` plus exactly one of `icon_name`, `icon_file` (required) | `MenuActionItem` |
| `add_switch{...}` | `label` (required), `state` (false), `status`, `on_toggled`, `on_activate`, `sensitive`, `visible`, `index` | `MenuSwitchItem` |
| `add_submenu{...}` | `label` (required), `icon_name` or `icon_file`, `sensitive`, `visible`, `index` | `MenuSubmenuItem` |
| `add_separator{...}` | `label` (a headed separator; default none), `visible`, `index` | `MenuSeparator` |
| `add_widget{...}` | `child` (a `shell.ui` widget, required), `visible`, `index` | `MenuWidgetItem` |
| `clear()` | | destroys every item (`removeAll`); returns nothing |
| `open([opts])` / `close([opts])` | `opts = {animate = true}` | boolean: whether the state changed. `open()` on an empty menu returns `false` |
| `toggle()` | | |
| `items()` | | sequence of item handles in order |
| `on_open_state_changed(fn)` | | `fn(menu, open)`; returns `shell.Connection` |

`add_widget` wraps the child in a non-activatable item
(`PopupBaseMenuItem({reactive = false})`, the way `status/brightness.js`
hosts a slider); the widget keeps keyboard focus handling of its own. The
quick-settings menu type `shell.QuickMenu` (quick-settings.md) extends `Menu`
with `set_header`.

Separator visibility is automatic: a separator without a label hides itself
while it would be first, last or adjacent to another separator
(`PopupMenuBase._updateSeparatorVisibility`), so `visible = true` is only a
request.

## Items

`MenuItem` (every item):

| Property | Type | Access | Notes |
|---|---|---|---|
| `kind` | `"item"\|"image"\|"switch"\|"submenu"\|"separator"\|"widget"` | ro | |
| `visible` | boolean | rw | |
| `index` | integer | ro | current 1-based position |
| `alive` | boolean | ro | |

Method `item:destroy()`. `MenuActionItem`, `MenuSwitchItem` and
`MenuSubmenuItem` add:

| Property | Type | Access | Notes |
|---|---|---|---|
| `label` | string | rw | |
| `sensitive` | boolean | rw | greyed and skipped by keynav; also false while the parent menu is insensitive |
| `ornament` | `"hidden"\|"none"\|"check"\|"dot"\|"no-dot"` | rw | `Ornament`; default `"hidden"`. `check`/`dot` also set the CHECKED accessible state |
| `icon_name`, `icon_file` | string | rw | image items and submenu items only (else nil; assigning raises) |

Event for these three: `on_activate(self)`, fired by click, Enter and Space
on a focused row, after which the **whole menu closes** (`itemActivated` ->
top menu `close()`), except for a switch toggled with Space and a submenu item
(which toggles its submenu and never fires `on_activate`).

`MenuSwitchItem`: `state` (rw boolean), `status` (rw string or nil: when set
the switch is replaced by that text and the row is not toggleable,
`setStatus`; nil restores the switch). `on_toggled(self, state)` fires for
user toggles only (README rule 7); `on_activate` fires after it for clicks.

`MenuSubmenuItem`: `menu` (ro `shell.Menu`, level 2), `open` is
`item.menu:open()`. `item.menu:on_open_state_changed` reports expansion.

`MenuSeparator`: `label` (rw string, "" = plain line). `MenuWidgetItem`:
`child` (ro widget handle); destroying the item destroys the child.

## Lifetime

Items die with their menu, the menu with its indicator or quick-settings item,
and everything at disable. Destroying an item inside its own `on_activate`
is allowed (deferred, README rule 8). The shell closes every menu on a
session-mode update (`PopupMenuBase._sessionUpdated`), which reports
`on_open_state_changed(false)`.

## Example

```lua
local shell = require("shell")
local function refresh() shell.log.debug("menu opened") end
local function set_vpn(on) shell.log("vpn %s", tostring(on)) end

local ind = shell.panel.add_indicator{ icon_name = "network-wired-symbolic", accessible_name = "VPN" }
local m = ind.menu
m:on_open_state_changed(function(_, open) if open then refresh() end end)
local vpn = m:add_switch{ label = "Home VPN", state = false,
  on_toggled = function(_, on) set_vpn(on) end }
local more = m:add_submenu{ label = "Profiles", icon_name = "preferences-system-symbolic" }
for i, name in ipairs{ "home", "work" } do
  local item = more.menu:add_item{ label = name, ornament = i == 1 and "dot" or "no-dot" }
  item:on_activate(function(self) shell.log("use %s", self.label) end)
end
m:add_separator{}
m:add_image_item{ label = "Open settings", icon_name = "emblem-system-symbolic",
  on_activate = function() shell.apps.launch("org.gnome.Settings.desktop") end }
```
