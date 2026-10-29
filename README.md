# gnome-lua

GNOME Shell with the JavaScript engine removed. Lua 5.5 is embedded directly
in the shell and extensions target a small, stable, versioned `shell.*` API
instead of the shell's internals. The shell's own UI logic is written in C (a
port of upstream's `js/` into the existing C core); no GObject Introspection
binding is involved on the Lua side.

This is a fork of [GNOME Shell](https://gitlab.gnome.org/GNOME/gnome-shell) at
tag 51.1 (Mutter 51); the upstream README is kept at
[docs/UPSTREAM_README.md](docs/UPSTREAM_README.md).

## Status

* Every upstream JS module is ported to C and GJS is gone: the shell starts
  from `src/main.c`, nothing links `libgjs` or `libmozjs` (CI checks).
* The extension API is at **version 1.1** (`docs/api/`): top-bar indicators
  and menus, quick settings, dash buttons, notifications, windows and
  workspaces (query, actions, geometry), the overview, search providers,
  keybindings, settings and prefs windows, D-Bus, timers and a clock, files,
  processes and HTTP behind capabilities, and a catalogue of lifecycle hooks.
* CI runs 11 unit programs, 8 headless shell tests and 16 API conformance tests
  under AddressSanitizer with `G_DEBUG=fatal-warnings`; the Lua runtime has a
  fuzz suite of its own.
* Open: performance numbers against upstream 51.1 (`docs/PERF.md`, needs a
  real machine) and the soak time before a release (`docs/PLAN.md` M8).

## Documentation

| | |
|---|---|
| [docs/api/README.md](docs/api/README.md) | the extension API contract, then one document per area |
| [docs/MIGRATING_FROM_JS.md](docs/MIGRATING_FROM_JS.md) | guide for authors of JavaScript extensions |
| [subprojects/shell-lua/examples/](subprojects/shell-lua/examples/) | reference extensions (indicator, quick toggle, dash launcher, hooks log, prefs, clock) |
| [docs/PLAN.md](docs/PLAN.md) | strategy, decisions, milestones, risks |
| [docs/PORT_STATUS.md](docs/PORT_STATUS.md) | per-module JS → C tracking with rename tables |
| [docs/CONVENTIONS.md](docs/CONVENTIONS.md) | C conventions and the pitfalls CI found |
| [docs/building-and-running.md](docs/building-and-running.md) | building, running nested (`--devkit`) or headless, tests |
| [docs/looking-glass.md](docs/looking-glass.md) | the built-in inspector with its Lua evaluator |
| [subprojects/shell-lua/docs/SANDBOX.md](subprojects/shell-lua/docs/SANDBOX.md) | what Lua code can and cannot do |

## Writing an extension

```lua
-- ~/.local/share/gnome-shell/extensions/hello@example.org/extension.lua
-- metadata.json: { "uuid": "hello@example.org", "name": "Hello", "shell-api": "1.1" }
local shell = require("shell")

return {
  enable = function()
    local label = shell.ui.label{ text = shell.time.format("%H:%M") }
    shell.panel.add_indicator{ child = label, accessible_name = "Clock" }
    shell.timer.every{ ms = 60000, callback = function()
      label.text = shell.time.format("%H:%M")
    end }
  end,
}
-- no disable(): everything created through shell.* is torn down with the extension
```

`gnome-extensions create` scaffolds one; `gnome-extensions prefs <uuid>` opens
its preferences from `prefs.lua`; Looking Glass (Alt+F2, `lg`) evaluates Lua
against the running shell.

## Building

```sh
meson setup build
ninja -C build
meson test -C build            # unit, headless shell and API conformance tests
dbus-run-session build/src/gnome-shell --wayland --devkit   # nested in a session
```

See [docs/building-and-running.md](docs/building-and-running.md) for the
dependencies and the headless test tool.
