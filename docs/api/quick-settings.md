# `shell.quick_settings`: toggles, sliders and status icons (version 1, draft)

Scope: adding entries to the system menu (the quick-settings panel opened from
the top-right) and status icons to the system area of the top bar. Models
`QuickToggle`, `QuickMenuToggle`, `QuickSlider` and `SystemIndicator`
(`js/ui/quickSettings.js`) and `QuickSettings.addExternalIndicator`
(`js/ui/panel.js`). Unlike the JS API there is no `SystemIndicator` class that
couples an icon with its items: an icon (`add_indicator`) and items
(`add_toggle`, ...) are independent, so each can be created, hidden and
destroyed on its own.

Not in v1:
- Built-in items: reordering, hiding, moving, or adding to their menus
  (network, bluetooth, power profiles, ...). Their set depends on hardware and
  build options (`Config.HAVE_NETWORKMANAGER`, ...).
- Items wider than the grid, grids of more than two columns, items placed
  outside the grid (header, footer, background-apps section).
- Arbitrary content as a grid item (an item is always a toggle, menu toggle or
  slider); custom content belongs in the item's menu via `menu:add_widget`.
- Linking an icon's visibility to a toggle automatically (do it in
  `on_toggle`): the coupling is policy, not mechanism.
- `QuickSettingsMenu.addItem/insertItemBefore` with a foreign sibling; the
  slot rules (README) replace sibling arguments.
- Menu header suffix actors (`addHeaderSuffix`), custom slider widgets
  (`QuickSlider.slider` is not exposed; use `value`).

## Status

**M7, implemented.** `src/lua-api/shell-lua-api-quick-settings.c` (the module
and the handle types) over `ShellQuickToggle`, `ShellQuickMenuToggle`,
`ShellQuickSlider` and `ShellQuickToggleMenu` (`src/ui/quick-settings.c`), and
`shell_panel_items_add_extension_item ()` / `_add_extension_indicator ()` in
`src/ui/panel-items.c`, with the ordering helpers of
`src/ui/extension-order.c`. The hook `quick-settings.opened` is declared by
`shell_quick_settings_menu_class_init ()`. Conformance test:
`tests/api/quick-settings.lua` with the fixture
`tests/api/fixtures/quick-settings@test`. Differences from the text below are
marked **(as built)** where they occur.

## Placement

The grid has **2 columns** (`N_QUICK_SETTINGS_COLUMNS`, `js/ui/panel.js`). Items
flow left to right; an item with `colspan = 2`, or a 1-wide item that does not
fit the remaining cell, starts a new row (`QuickSettingsLayout`).
Built-in order is: sliders (system, volume, brightness; span 2), built-in
toggles, **extension items**, then the background-apps section
(`QuickSettings._setupIndicators`, `addExternalIndicator` inserts "before
background apps"). So an extension slider appears below the built-in toggles,
not next to the volume slider. Within the extension block, items follow the
slot rules in README.md (extension order, then `index`).

Status icons (`add_indicator`) go into the top-bar system area after the
privacy indicators (remote access, camera, microphone, location) and before the
first other built-in icon (`insert_child_below(indicator, this._brightness)`),
ordered by the same slot rules. **(as built)** The items and the icons are two
slots, each with its own `index`. The order follows `enabled-extensions` while
it changes, too. In a session mode whose top bar has no quick settings button
(the constructors raise `The top bar has no quick settings menu in this
session mode`) nothing can be added.

## Constructors

All return a handle and take one table. Common optional fields: `visible`
(true), `colspan` (integer 1 or 2; toggles default 1, sliders 2), `index`
(integer >= 1), `sensitive` (true).

**`add_toggle{...}` -> `shell.QuickToggle`**

| Field | Type | Notes |
|---|---|---|
| `title` | string, required | label and accessible name |
| `subtitle` | string | hidden when nil or empty |
| `icon_name` / `icon_file` | string | at most one. **(as built)** `icon_file` is a path relative to the extension directory, `.svg` or `.png`, as in ui.md; an absolute path, `..` or a missing file raises |
| `checked` | boolean | initial state, default false |
| `on_toggle` | function(self, checked) | the user toggled: `checked` is the new value, already applied. Makes the item a toggle (`toggle_mode`) |
| `on_click` | function(self) | action button: no state, `checked` stays false. Exclusive with `on_toggle` (raises); with neither, the item is display-only |

When `on_toggle` runs the shell has already flipped `checked`; to veto, assign
`self.checked = false` in the callback (an assignment is not a click: it does
not call `on_toggle` again, README rule 7). **(as built)**
`on_toggle` and `on_click` are the user clicking the item (the `clicked` signal
of the button, by pointer or keyboard); `h:on_toggle(fn)` and `h:on_click(fn)`
raise on an item that has the other one.

**`add_menu_toggle{...}` -> `shell.QuickMenuToggle`**: all of `add_toggle`'s
fields, plus `menu_enabled` (true) and `menu_button_accessible_name` (default
gettext "Open menu"). The right-hand arrow opens `item.menu`; the main area
toggles.

**`add_slider{...}` -> `shell.QuickSlider`**

| Field | Type | Notes |
|---|---|---|
| `icon_name` / `icon_file` | string | |
| `value` | number 0.0..1.0 | default 0 |
| `on_changed` | function(self, value) | every change of `value`: the user's (drag, keys) and assignments, README rule 7 |
| `icon_reactive` | boolean | the icon is a button, default false |
| `on_icon_click` | function(self) | requires `icon_reactive = true` |
| `icon_accessible_name` | string | name of the icon button |
| `accessible_name` | string, required | name of the slider |
| `menu_enabled` | boolean | show the menu arrow, default false |
| `menu_button_accessible_name` | string | |

**(as built)** `value` outside `0.0..1.0` and `on_icon_click` without
`icon_reactive = true` raise; `sensitive = false` makes the slider, its icon
button and its menu button insensitive.

**`add_indicator{...}` -> `shell.QuickIndicator`**: `icon_name` or `icon_file`
(one required), `visible`, `accessible_name`, `index`. The icon gets the
`system-status-icon` class.

## Handles

`shell.QuickItem` (base of toggle, menu toggle, slider): `visible` (rw),
`sensitive` (rw), `colspan` (rw integer 1..2), `index` (ro), `alive`;
`destroy()`.

| Type | Properties (rw unless noted) | Events |
|---|---|---|
| `QuickToggle` | `title`, `subtitle` (nil clears), `icon_name`, `icon_file`, `checked` | `on_toggle`, `on_click` |
| `QuickMenuToggle` (extends `QuickToggle`) | `menu` (ro `shell.QuickMenu`), `menu_enabled`, `menu_button_accessible_name` | as above |
| `QuickSlider` | `value`, `icon_name`, `icon_file`, `icon_reactive`, `icon_accessible_name`, `accessible_name`, `menu` (ro, always present), `menu_enabled`, `menu_button_accessible_name` | `on_changed`, `on_icon_click` |
| `QuickIndicator` | `icon_name`, `icon_file`, `visible`, `accessible_name`, `index` (ro); `destroy()` | |

`shell.QuickMenu` extends `shell.Menu` (menu.md) and adds
`menu:set_header{ icon_name|icon_file, title, subtitle }` (shows the header
row, `QuickToggleMenu.setHeader`; the icon is highlighted while the owning item
is checked). A menu opens inline, dims the grid and closes with the panel; at
most one is open at a time. `menu:open()` returns `false` unless the
quick-settings panel itself is open (hook `quick-settings.opened`).
**(as built)** `title` is required by `set_header`. The rule of `open()` is
that the item of the menu is on screen (`ShellQuickToggleMenu` refuses to open
while its item is not mapped): it holds for a hidden item, too, and for the
menus of the built-in items.

There is no separate `remove`: `item:destroy()` removes an item (and
`indicator:destroy()` an icon), and `visible = false` hides one temporarily.
Destroying an item reflows the grid; destroying a slider or menu
toggle destroys its menu. A destroyed item cannot be re-added; create a new
one (or toggle `visible`).

## Example

```lua
local shell = require("shell")
local settings = shell.settings.open("org.gnome.desktop.interface")  -- needs "settings"
local ICON = "format-text-larger-symbolic"

local function apply(on)
  settings:set_double("text-scaling-factor", on and 1.25 or 1.0)
end

local dot = shell.quick_settings.add_indicator{
  icon_name = ICON, visible = settings:get_double("text-scaling-factor") > 1.0 }

local toggle = shell.quick_settings.add_menu_toggle{
  title = "Large text", subtitle = "Off", icon_name = ICON,
  checked = dot.visible,
  on_toggle = function(self, on)
    apply(on)
    self.subtitle, dot.visible = on and "On" or "Off", on
  end,
}
toggle.menu:set_header{ icon_name = ICON, title = "Large text" }
toggle.menu:add_item{ label = "Reset", on_activate = function()
  apply(false)   -- assigning checked is not a click: on_toggle does not run
  toggle.checked, toggle.subtitle, dot.visible = false, "Off", false
end }

shell.quick_settings.add_slider{
  icon_name = "preferences-desktop-font-symbolic", accessible_name = "Text scale",
  value = 0.5, on_changed = function(_, v) settings:set_double("text-scaling-factor", 0.5 + v) end,
}
```
