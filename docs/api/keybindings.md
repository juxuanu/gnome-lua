# `shell.keybindings`: global shortcuts (version 1.1)

Scope: running a function of the extension when the user presses a key
combination, anywhere in the session, in the way the shell's own shortcuts
work. A shortcut is either **settings-backed** (the combination lives in an
`as` key of the extension's settings schema, so the `keybinding` row of
`prefs.lua` edits it and a change takes effect at once) or **literal** (the
combinations are fixed in the code). Models `Main.wm.addKeybinding ()` /
`removeKeybinding ()` of the JS shell (`ShellWindowManager`,
`src/ui/window-manager.c`) and `Meta.Display.grab_accelerator ()` (the
accelerators that gnome-settings-daemon and the screenshot UI use over D-Bus).

Since: **1.1**. Every row below is new in 1.1; an extension that uses it
declares `"shell-api": "1.1"`.

Not in v1.1:
- Key *release*, chords (`<Super>x, y`), sequences, gestures, mouse buttons,
  and "any key" grabs. A shortcut fires once per press; holding the keys down
  (autorepeat) does not fire it again.
- Learning which key combination the user pressed when a binding has several
  (`on_pressed` receives the handle only).
- Modes other than the two below: a shortcut never runs on the lock screen,
  the unlock dialog, the login screen, in system modal dialogs (authentication,
  session dialogs), in Looking Glass or while a shell menu (popup) is open.
- Overriding a shortcut of the shell or of Mutter: a combination another
  binding already owns is reported, not taken over (see *Conflicts*).
- Reading or changing the key (use `shell.settings`), and the conflict warning
  of the preferences window (prefs.md).

No capability: a shortcut is visible to the user in the schema he or she
edits, runs only the extension's own code, and does not receive what is typed
(the callback gets no key, no text and no window). The number of live
shortcuts per extension is bounded (16) for the same reason the other
UI objects are.

## Status

**1.1, implemented.** `src/lua-api/shell-lua-api-keybindings.c` over
`shell_window_manager_add_keybinding ()` (settings-backed) and
`meta_display_grab_accelerator ()` with the display's `accelerator-activated`
signal (literal). Conformance test: `tests/api/keybindings.lua` with the
fixture `tests/api/fixtures/keybindings@test`, which presses the combinations
with `test.press_accelerator ()` of the test runner (a virtual keyboard).

## `shell.keybindings.add{...}` -> `shell.Keybinding` | `nil, message`

| Field | Type | Default | Notes |
|---|---|---|---|
| `name` | string | one of `name`, `accelerators` | settings-backed: the key (type `as`) that holds the combinations, in GTK syntax (`{"<Super>x"}`). An empty list disables the shortcut until the key is set |
| `settings` | `shell.Settings` | the extension's own (`shell.settings.extension()`) | only with `name`; any `shell.Settings` the extension holds (an extension without `settings-schema` must pass one) |
| `accelerators` | sequence of strings (1 to 8) | | literal: the combinations, GTK syntax. Mutually exclusive with `name` and `settings` |
| `modes` | sequence of `"normal"`, `"overview"` | both | where the shortcut runs: `"normal"` = in the desktop, `"overview"` = while the overview is shown (the shell's `ShellActionMode` NORMAL and OVERVIEW). Not empty |
| `on_pressed` | function(binding) | required | called once per press, with the `shell.Keybinding` first (README.md rule 6: a shortcut is a user action, not a data source, so a callback can refer to its own binding without an upvalue) |

Accelerator syntax is the one of GTK and of `prefs.md`: zero or more
`<Modifier>` (`Shift`, `Control`, `Alt`, `Super`, `Hyper`, `Meta`, case does
not matter) followed by a key name (`x`, `F9`, `Print`, `XF86AudioPlay`).

Raises (misuse): an unknown field; both or neither of `name` and
`accelerators`; `settings` with `accelerators`; a non-string `name` or
accelerator; an accelerator that is not of the syntax above (empty, an
unbalanced `<`, no key name); more than 8 accelerators; `modes` that is empty,
holds a value other than the two above or is not a sequence of strings;
`on_pressed` missing or not a function; `settings` that is not a live
`shell.Settings`; `name` that is not a key of the schema or whose type is not
`as`; no `settings` and no `settings-schema` in the metadata; more than 16
live shortcuts (`too many key bindings (limit 16)`).

Returns `nil, message` (environmental, README.md rule 5) when the shell cannot
give the combination to the extension:
- literal: an accelerator is already held by another literal shortcut (of this
  or another extension), by a D-Bus client (`GrabAccelerator`) or by Mutter,
  or Mutter cannot translate its key name. Nothing stays grabbed: the call is
  all or nothing;
- settings-backed: the name `name` is already used by a key binding of the shell
  or of Mutter (a built-in shortcut, `toggle-overview`...), of another
  extension, or by a live `shell.Keybinding` of this one. Key names of
  settings-backed shortcuts are one namespace for the whole session, so give
  the key a name that is specific to the extension (`myext-toggle`), not
  `toggle`.

## `shell.Keybinding`

| Member | Type | Access |
|---|---|---|
| `name` | string \| nil | ro: the settings key, nil for a literal shortcut |
| `accelerators` | sequence of strings | ro: the combinations now. Settings-backed: the current value of the key (read at each access, so a rebinding shows at once); literal: as given |
| `alive` | boolean | ro |

Methods:
- `destroy()` removes the shortcut: the combinations are released at once and
  `on_pressed` never runs again. Idempotent; the handle is dead afterwards.
  Called from inside `on_pressed` of the same binding, the removal happens when
  the callback returns (README.md rule 8); a settings-backed name is given
  back to Mutter a moment later, from an idle (Mutter is still running the
  binding), so an `add` with the same `name` inside the same callback returns
  `nil, message`.
- `on_pressed(fn)` -> `shell.Connection`: one more callback, called after the
  one given to `add`, in connection order (rule 6). `disconnect()` removes it.

Ownership: the extension context owns the shortcut. It is removed at disable,
in reverse creation order with the other objects (README.md, *Ownership and
teardown*), and its combinations are free for the next `add` (the same
extension enabled again gets them). Dropping the handle does not remove it.

## Behaviour

- **Rebinding.** A settings-backed shortcut follows its key: when the user (the
  `keybinding` row of the preferences window, `dconf`) or the extension
  (`settings:set_strv`) writes the key, Mutter regrabs at once. There is no
  callback for the change; use `settings:on_changed`.
- **Conflicts.** Several settings-backed shortcuts, or a settings-backed one
  and a built-in, may be given the same combination by the user; Mutter then
  decides which one runs (the same as for the shell's own shortcuts). `add`
  only fails for a *name* that exists and for a *literal* combination that is
  already grabbed, as above.
- **Modes.** The shell filters by its action mode when the key is pressed. The
  overview is the mode `"overview"`; a shortcut with `modes = {"overview"}`
  does not run in the desktop and one with `{"normal"}` does not run while the
  overview is shown. The default (both) is what the shell's own shortcuts use.
- **Callbacks** run under the extension's pcall, watchdog and error policy
  (README.md rule 8). They run from the key event: do not wait in them.
  A callback MAY call any API, including `destroy()` of its own binding.
- **Session modes.** Because the shortcut never runs on the lock screen or the
  login screen, an extension that also loads in those session modes
  (`session-modes` of the metadata) still cannot see keys typed there.

## Example

`metadata.json`: `"shell-api": "1.1"`, `"settings-schema": "org.example.clock"`;
the schema has

```xml
<key name="clock-toggle-shortcut" type="as"><default>["&lt;Super&gt;c"]</default></key>
```

and `prefs.lua` a `{ type = "keybinding", key = "clock-toggle-shortcut", ... }`
row (prefs.md).

```lua
local shell = require("shell")

shell.keybindings.add{
  name = "clock-toggle-shortcut",           -- the user changes it in the prefs
  on_pressed = function(binding) shell.log("pressed %s", table.concat(binding.accelerators, " ")) end,
}

local help, err = shell.keybindings.add{
  accelerators = { "<Super><Alt>h" },       -- fixed
  modes = { "normal" },
  on_pressed = function() shell.notify{ title = "Help" } end,
}
if not help then shell.log.warning("no help shortcut: %s", err) end
```
