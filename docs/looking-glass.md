# Looking Glass

Looking Glass is GNOME Shell's integrated debugger and inspector tool.

You currently run it by pressing <kbd>Alt</kbd> <kbd>F2</kbd>,
and typing `lg`<kbd>Return</kbd>.

You can leave Looking Glass by pressing <kbd>Esc</kbd> in its
Evaluator pane. While the object inspector (see below) is open, <kbd>Esc</kbd>
closes the inspector first.

Looking Glass has five major panes (Evaluator, Windows, Extensions,
Actors and Flags) and one tool (the Picker).

## Evaluator

This is an interactive Lua prompt. It runs your input in the *inspector
state*, a Lua 5.5 state that is not sandboxed: it has the whole standard
library (`os`, `io`, `debug`, ...) and the `shell` table of the extension API.
Everything you define stays defined until the shell restarts.
Try, in order:

```lua
1+1

window_actors()

for _, w in ipairs(it) do w.scale_x = 0.3; w.scale_y = 0.3 end

for _, w in ipairs(r(2)) do w.scale_x = 1; w.scale_y = 1 end

r(2)[1]

it.scale_x
```

As in the `lua` interpreter, the input is first tried as an expression
(`return <input>`), so its value is shown; otherwise it runs as a statement.
A command can return several values, each of them is a result.

### Special evaluator features

Each computed value is saved, and can be referred back to. Typing
`r(`*number*`)` gets you the result with that number (the numbers count from
1), and `it` is the last result. Click on a result to inspect it: the entries
of a table, or the properties and methods of an object, are listed, and each
of them can be inspected in turn (*Back* goes back, *Insert* makes the value
a new result). Results that scroll out of the list are forgotten.

What `print()` writes is shown above the results of the command. Errors are
shown with their traceback.

The evaluator also has a history feature; you can press <kbd>↑</kbd> and
<kbd>↓</kbd> to access previously used entries. The history is automatically
saved to the dconf key `/org/gnome/shell/looking-glass-history`, and loaded
when you restart the shell.

Press <kbd>Tab</kbd> to complete a global variable, a keyword, the field of
a table (`shell.hooks.`<kbd>Tab</kbd>) or the property or method of an object.
Pressing it twice quickly lists all the possibilities. Reading the values on the
way can run `__index` metamethods and the getters of properties, but nothing is
called.

### Globals

| Name | What it is |
|---|---|
| `global` | the `Shell.Global` object (`global.stage`, `global.display`, ...) |
| `stage` | the `Clutter.Stage` |
| `st_settings` | the `St.Settings` |
| `main` | the `Shell.Main` object, from which the panel, the message tray and the layout manager are reachable |
| `window_actors()` | the window actors, as a table |
| `inspect(x, y)` | the reactive actor at a position of the stage |
| `r(n)`, `it` | the results, see above |

### Objects

Objects of the shell (actors, windows, applications, ...) are *handles*. A
handle has the properties of its GObject type as fields, and no other access to
the C side: there are no calls to arbitrary functions.

```lua
stage.width
stage.name = "main stage"
global.display.focus_window.title
```

* The dashes in the name of a property are underscores (`scale_x`).
  Assigning to a property sets it, unless it is read-only.
* Properties that are objects give handles again, and the handle of the same
  object is the same value.
* Every handle has `h:gtype()` (the name of its type) and `h:properties()`
  (the sorted names of its properties).
* Actors also have `get_parent()`, `get_children()`, `get_next_sibling()`,
  `get_previous_sibling()`, `get_stage()`, `show()`, `hide()` and `destroy()`.
* A handle does not keep its object alive. When the object is gone, the handle
  turns dead (`<gobject.StButton (dead)>`) and errors when it is used.

Animations cannot be started from Looking Glass (there is no `ease()`), but the
properties can be set at once.

### Slowing Down Animations

You can use Looking Glass to access the global `St.Settings` object and set
its `slow_down_factor` property:

```lua
st_settings.slow_down_factor = 5
```

Any value greater than 1 makes all shell animations slower. This is
particularly useful when implementing a new animation behavior.

### Requiring modules

The inspector state finds modules in the resources of the shell
(`resource:///org/gnome/shell/lua`) and with the stock `require`.
`shell = require("shell")` is the same table as the global.

### The trash can

The button with the trash can collects the garbage of the inspector state and
of the state of every enabled extension.

## Windows

This is a list of all open windows and related information,
like the associated app. Their names are links which inspect the
window or the application.

## Extensions

This is a list of all installed Lua extensions, sorted by name, with their
description, their state, the memory (the Lua heap) and the number of errors of
their Lua state. You can use the View Source link to quickly access the
extension folder, Web Page to open the `url` of `metadata.json` (when it has
one), and Show Errors to see the last error of the extension (Hide Errors
closes it again). The list is made again whenever an extension changes its
state. The page does not enable, disable or reload extensions: use
`gnome-extensions` or the Extensions application.

## Actors

This pane gives access to a complete tree of the actor hierarchy.
It is useful when an actor cannot be selected with the picker or
accessed via code in the evaluator.

## Flags

This pane provides easy access to a list of Mutter and Clutter debug flags,
`unsafe-mode`, the export of the debug control of Mutter, and a slider for
`slow_down_factor` of `St.Settings` (see below). The switches are updated from
the shell each time the pane is shown.

## The Picker

The picker allows you to visually select any object from the shell's scene.
When you do, it's inserted as a result into the Evaluator pane.

During picking, you can scroll to move up and down the actor hierarchy.
