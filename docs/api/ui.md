# `shell.ui`: widgets (version 1, draft)

Status: **impl** (`src/lua-api/shell-lua-api-ui.c`; conformance: `tests/api/ui.lua`,
fixture `tests/api/fixtures/ui@test`).

Scope: constructors for the nine St-level widgets an extension composes into a
panel indicator (panel.md), a menu (menu.md) or a quick-settings menu
(quick-settings.md), their common property set, containment, events and CSS. A
widget is never shown on its own: it becomes visible by being placed (as an
indicator `child`, inside a `menu:add_widget`, or inside another widget that
is). General rules are in README.md.

Not in v1 (each exclusion: why):
- Custom widget classes, `allocate`/`get_preferred_*` overrides: arbitrary C
  vfuncs run in the compositor's frame path.
- Raw allocation, `set_position`, transforms, `ease`/animations, Clutter
  actions, constraints, layout managers: layout is CSS plus expand/align, so it
  survives theme and toolkit changes. Animation may arrive as a fixed
  `transition` property later.
- Clutter effects (blur, brightness, shaders): none; expensive and
  unbounded. A fixed set could be added as a minor.
- Painting (`Cogl`, `St.DrawingArea`, canvases): icons and CSS are the drawing
  model (PLAN §7).
- `on_key`/key and pointer event hooks on arbitrary widgets: raw input is a
  keylogging channel and ties extensions to Clutter event shapes. Shortcuts
  are `shell.keybindings` (keybindings.md, 1.1); widgets expose `on_activate`
  instead.
- Images by pixel data or `Gio.Icon` objects: `icon_name` and `icon_file` cover
  themed and bundled icons without a GObject handle.
- Text selection/cursor control, password entries, tooltips, drag-and-drop,
  grid/flow layouts, popups outside menus.

| Constructor | Handle type | St class |
|---|---|---|
| `shell.ui.box{...}` | `shell.Box` | `St.BoxLayout` |
| `shell.ui.label{...}` | `shell.Label` | `St.Label` |
| `shell.ui.icon{...}` | `shell.Icon` | `St.Icon` |
| `shell.ui.button{...}` | `shell.Button` | `St.Button` |
| `shell.ui.entry{...}` | `shell.Entry` | `St.Entry` |
| `shell.ui.switch{...}` | `shell.Switch` | the switch of `PopupSwitchMenuItem` (`js/ui/popupMenu.js` `Switch`) |
| `shell.ui.slider{...}` | `shell.Slider` | `Shell.Slider` (`src/ui/slider.c`) |
| `shell.ui.spacer{...}` | `shell.Spacer` | `St.Widget` |
| `shell.ui.scroll{...}` | `shell.Scroll` | `St.ScrollView` |

All are `shell.Widget` subtypes. Every constructor accepts the common fields
below plus its own; an unknown field raises.

## Common properties (`shell.Widget`)

| Property | Type | Access | Notes |
|---|---|---|---|
| `style_class` | string | rw | space-separated CSS classes. The shell always adds `lua-ext-<id>` (uuid with every char outside `[a-z0-9-]` replaced by `-`, lower-cased) so a stylesheet can scope itself; reading returns only the classes the extension set (`has_style_class` sees `lua-ext-<id>`, `remove_style_class` cannot remove it) |
| `style` | string | rw | inline declarations only (`"color: red; padding: 2px"`), no selectors, at most 1024 bytes; for values computed at runtime (a prefs colour) |
| `visible` | boolean | rw | hidden widgets take no space; default `true` |
| `reactive` | boolean | rw | receives pointer input; default per widget |
| `x_expand`, `y_expand` | boolean | rw | default `false` (`spacer`: `true`) |
| `x_align`, `y_align` | `"fill"`, `"start"`, `"center"`, `"end"` | rw | `Clutter.ActorAlign`; default `"fill"` (a `switch` is `"center"` vertically, as everywhere in the shell) |
| `opacity` | integer 0..255 | rw | default 255 |
| `accessible_name` | string | rw | exposed to AT-SPI; REQUIRED for an icon-only `button` (construction raises without it); SHOULD be set on an `entry`, `slider` or `switch` that has no visible label beside it |
| `sensitive` | boolean | rw | only on `button`, `entry`, `switch`, `slider`; `false` = not clickable, not focusable, `:insensitive` pseudo-class |
| `alive` | boolean | ro | README |

Methods on every widget: `w:remove()` detaches from its parent (no-op without
one; the widget stays alive, owned by the extension, and may be added again);
`w:destroy()`; `w:add_style_class(name)`, `w:remove_style_class(name)`,
`w:has_style_class(name)`; `w:focus()` (give keyboard focus if mapped and
focusable; returns whether focus moved); `w:on_<event>(fn)` for its events.

Sizes, margins, padding, borders, fonts, colours, `spacing`, `min-width`,
`max-height`, `icon-size` and all other visual properties are CSS (below).

## Containers

`box` and `scroll` contain widgets; the others do not (adding a child to them
raises).

- `shell.ui.box{ orientation = "horizontal"|"vertical", children = {w1, w2} }`
  (default horizontal). Methods: `b:add(child [, index])` (appends, or inserts
  at the 1-based `index`), `b:children()` (sequence of handles), `b:clear()`
  (destroys all children).
- `shell.ui.scroll{ child = box, hscrollbar_policy = "never", vscrollbar_policy = "automatic" }`:
  `child` MUST be a `shell.Box`. Policies: `"automatic"`, `"always"`,
  `"never"`, `"external"` (`St.PolicyType`), defaults `"never"`/`"automatic"`.
  Bound the height with CSS `max-height` on the scroll's class, as
  `PanelMenu.Button._onOpenStateChanged` does for menus. Property `child` (rw).
- A widget has at most one parent: adding one that already has a parent raises
  `widget already has a parent; call remove() first`; adding an ancestor to its
  descendant raises. Destroying a container destroys its children.

## Per-widget fields

**label**: `text` (rw string), `ellipsize` (rw, `"none"|"start"|"middle"|"end"`,
default `"none"`); method `l:set_markup(str)` (Pango markup; invalid markup
raises, reading `text` afterwards returns the plain text).

**icon**: `icon_name` (rw, themed name), `icon_file` (rw, path *relative to the
extension directory*, `.svg`/`.png`; absolute paths, `..` and missing files
raise; a `-symbolic.svg` is recoloured by CSS like a themed symbolic icon),
`icon_size` (rw integer logical px; unset = CSS `icon-size`). Setting one of
`icon_name`/`icon_file` clears the other.

**button**: `label` (rw string) or `icon_name`/`icon_file` or `child` (a widget;
mutually exclusive with the other three), `toggle_mode` (boolean, construct
only), `checked` (rw, meaningful with `toggle_mode`), `sensitive`.
`on_clicked(self, button)`: `button` is 1 (primary), 2 or 3. In toggle mode
`checked` has already flipped when the callback runs (`st_button`
`handle_clicked`). `on_activate` is not provided: Enter/Space on a focused
button also emit `clicked`.

**entry**: `text` (rw), `hint_text` (rw), `sensitive`. `on_changed(self, text)`
(every change of `text`: user edits and assignments, rule 7),
`on_activate(self, text)` (Enter).

**switch**: `state` (rw boolean), `sensitive`. `on_changed(self, state)` (every
change of `state`, rule 7). Role CHECK_BOX; clicking, dragging the handle and
Space all toggle.

**slider**: `value` (rw number 0.0..1.0), `sensitive`. `on_changed(self, value)`
(every change of `value`: drags, arrow keys and assignments; rule 7),
`on_drag_begin(self)`, `on_drag_end(self)`
(`Shell.Slider` `drag-begin`/`drag-end`, for committing after a drag).

**spacer**: only `width`, `height` (rw integer px; `-1` = natural, the default);
`x_expand`/`y_expand` default `true`.

## CSS

`stylesheet.css` (and the variants `stylesheet-dark.css`, `stylesheet-light.css`,
`<session-mode>.css`, `<session-mode>-<variant>.css`; first match wins, loaded
with the extension and unloaded on disable) is added to the shell theme exactly
as before (`src/ui/extension-system.c`, `load_stylesheet ()`). It is global:
selectors reach built-in widgets and other extensions, as theme authors rely on
today. Only the following are stable API: the classes you set, `lua-ext-<id>`,
the class names of the theme's pseudo-classes (`:hover`, `:active`, `:checked`,
`:focus`, `:insensitive`), the CSS properties St documents, and the built-in
classes `panel-button`, `system-status-icon`, `popup-menu-item`, `quick-toggle`,
`quick-slider`, `dash-label`. Prefer `.lua-ext-<id> .mine`.

## Example

```lua
local shell = require("shell")
local ui = shell.ui

local value = ui.label{ text = "0", style_class = "counter-value", y_align = "center" }
local more  = ui.button{ label = "+", style_class = "counter-button",
  on_clicked = function() value.text = tostring(tonumber(value.text) + 1) end }
local row = ui.box{ style_class = "counter", children = { value, ui.spacer{}, more } }
shell.panel.add_indicator{ child = row, accessible_name = "Counter" }
-- stylesheet.css:  .lua-ext-counter-example .counter-value { font-weight: bold; }
```
