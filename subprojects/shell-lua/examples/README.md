# Example extensions

Each subdirectory is a complete extension in the packaging format described
in `docs/PLAN.md` (D13): a directory named after its uuid containing
`metadata.json` and `extension.lua`, plus optional `prefs.lua`,
`stylesheet.css`, `schemas/` and `locale/`. The examples are written against
the documents of `docs/api/`; `tests/api/examples.lua` installs and enables
each of them in a headless shell.

| Example | Shows | Documents |
|---|---|---|
| `hello@gnome-lua.example` | the canonical minimal extension: logs, runs a timer, connects to a hook only when the running shell declares it, has settings and a preferences window | core.md, hooks.md, prefs.md |
| `indicator@gnome-lua.example` | a top-bar indicator with a menu (action, switch, submenu with a choice, separator, image item), widgets as its content, a stylesheet | panel.md, menu.md, ui.md |
| `clock@gnome-lua.example` | API 1.1: a top-bar clock (`shell.time.format`) re-rendered by a timer aligned to the minute and on `time.timezone-changed`, a menu with the full date and a 24-hour switch, a literal keybinding (Super+Alt+O) that toggles the overview | core.md (`shell.time`), keybindings.md, overview.md, hooks.md |
| `quick-toggle@gnome-lua.example` | a quick-settings menu toggle with a header and a slider, and a status icon, kept in sync with the extension's settings in both directions | quick-settings.md, core.md |
| `dash-launcher@gnome-lua.example` | two buttons in the dash: one launches an application, one toggles a mark | dash.md, window.md (`shell.apps`) |
| `hooks-log@gnome-lua.example` | an extension that only listens to hooks and logs the lifecycle events of the shell, skipping the hooks the shell does not declare | hooks.md |
| `prefs@gnome-lua.example` | a tour of `prefs.lua`: every kind of row, two pages, rows that depend on other keys | prefs.md |

One example per capability (README.md of `docs/api`, "Capabilities"); each
declares only the capability it shows:

| Example | Capability | Shows | Documents |
|---|---|---|---|
| `power-profile@gnome-lua.example` | `dbus` | the power profile of power-profiles-daemon in the top bar, set from its menu: a proxy on the system bus, `watch_name` to follow the service, `get_property`, `set_property` and `on_properties_changed`; hidden while the daemon is not there | dbus.md, panel.md, menu.md |
| `text-scale@gnome-lua.example` | `settings` | a quick-settings slider for `text-scaling-factor` of `org.gnome.desktop.interface`, a schema that is not the extension's own, kept in sync both ways without a loop (callbacks run for assignments too) | core.md (`shell.settings`), quick-settings.md |
| `emoji-search@gnome-lua.example` | `search` | a search provider of the overview that answers with emoji; activating one copies it (`clipboard_text`) | search.md |
| `load-average@gnome-lua.example` | `fs` | the load average in the top bar, read from `/proc/loadavg`, outside the extension's own directories, by a timer | fs.md, core.md (`shell.timer`) |
| `disk-space@gnome-lua.example` | `spawn` | the free space of `/` and of the home file system from `df -Pk` (C locale, a timeout, the exit status and the output), every ten minutes and from the menu | spawn.md |
| `weather@gnome-lua.example` | `network` | the weather from the one-line format of wttr.in: an HTTPS request with a timeout, the status and body of the response, the network errors; the first request waits for the network to come up | network.md |
| `pi-digits@gnome-lua.example` | `worker` | 3000 digits of pi computed in a worker (`workers/pi.lua`), off the main thread: the progress posted while it runs, the answer as the return value, a notification, `terminate ()` from the menu | worker.md, notify.md |

`hello@gnome-lua.example` has no `disable()` on purpose, to show that
everything created through `shell.*` is torn down automatically; the others
have none either, except `hooks-log`, which has one to show where it would go.

The preferences of an example with a `prefs.lua` open with `gnome-extensions
prefs <uuid>`. Its `schemas/` holds the source of the schema and the
`gschemas.compiled` that `glib-compile-schemas schemas/` makes of it; compile
again after editing the XML.

To install one for your user, copy the directory (the directory name must equal
the uuid in `metadata.json`) into the per-user extension directory and enable
it in the `org.gnome.shell` settings:

    mkdir -p ~/.local/share/gnome-shell/extensions
    cp -r hello@gnome-lua.example ~/.local/share/gnome-shell/extensions/
    gsettings set org.gnome.shell enabled-extensions "['hello@gnome-lua.example']"

(Adjust the `gsettings` call if you already have other enabled extensions.)

To try an extension straight from a source checkout, without installing it,
point `SHELL_LUA_EXTENSION_PATH` at the directory that contains it. The
variable is a colon-separated list of directories, searched in order, and
replaces the default search path:

    SHELL_LUA_EXTENSION_PATH=$PWD/subprojects/shell-lua/examples dbus-run-session gnome-shell --wayland --devkit

Directories listed there are treated like per-user extensions, and the
directory name must still equal the uuid, which is why the example lives in
`examples/hello@gnome-lua.example/`. Log output appears in
the journal with the domain `shell-lua/hello@gnome-lua.example`.
