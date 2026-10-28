# Hook catalogue (version 1)

Hooks are declared by the shell with `shell_hook_registry_declare()` and
listened to from Lua with `shell.hooks.on(name, fn)` (after phase) or
`shell.hooks.before(name, fn)` (before phase, cancellable hooks only).
Payloads are `a{sv}` dictionaries delivered to Lua as tables.

Naming rules (see `docs/CONVENTIONS.md`): `<subject>.<event>`, lower case,
one dot by convention (the registry accepts more segments), hyphens inside a part. Hooks named with a present participle
(`overview.showing`) are cancellable; past-tense hooks (`window.focused`)
are informational. Payloads carry ids and names, never pointers.

Every new hook point gets a row here in the same commit that declares it.

| Hook | Cancellable | Payload | Emitted from | Since |
|---|---|---|---|---|
| `shell.started` | no | (empty) | `shell_global_notify_startup_complete()`, called by `main.js` when the layout manager reports startup complete | 1.0 |
| `shell.shutting-down` | no | (empty) | `_shell_global_notify_shutdown()` | 1.0 |
| `session.mode-changed` | no | `mode: s` | `ShellSessionMode` (`src/ui/session-mode.c`), after `::updated` on every mode change, including the initial mode | 1.0 |
| `panel.indicator-added` | no | `name:s box:s owner:s` | `add_to_panel_box ()` in `src/ui/panel.c`, once per indicator when it is first registered in the status area: the indicators of the shell when the session mode first lists them (`owner` empty), the others through `shell_panel_add_to_status_area_with_owner ()`. Not repeated when a session mode change moves an indicator | 1.0 (M2) |
| `panel.menu-opened` | no | `name:s box:s` | the `open-state-changed` handler (`menu_open_state_changed_cb ()`) that `src/ui/panel.c` installs on the menu of every indicator from `indicator_menu_set_cb ()` (the former `Panel._onMenuSet`), when the menu opens | 1.0 (M2) |
| `monitors.changed` | no | `count:i primary:i` | `ShellLayoutManager::monitors-changed` (`src/ui/layout.c`) | 1.0 (M2) |
| `overview.showing` | yes | `reason:s target:s` | `Overview.show()` (`js/ui/overview.js`) | 1.0 (M3) |
| `overview.shown` | no | `target:s` | `Overview._showDone()` | 1.0 (M3) |
| `overview.hiding` | yes | `reason:s` | `Overview.hide()` | 1.0 (M3) |
| `overview.hidden` | no | (empty) | `Overview._hideDone()` | 1.0 (M3) |
| `window.mapped` | no | W | `WindowManager._mapWindowDone` (`js/ui/window-manager.c`) | 1.0 (M3) |
| `window.unmapped` | no | W | `WindowManager._destroyWindow`: the window is gone (not emitted on minimize) | 1.0 (M3) |
| `window.focused` | no | W + `previous-window-id:t` | `notify::focus-window` of `Meta.Display` | 1.0 (M3) |
| `window.minimized` | no | W | `WindowManager._minimizeWindow` | 1.0 (M3) |
| `window.unminimized` | no | W | `WindowManager._unminimizeWindow` | 1.0 (M3) |
| `window.title-changed` | no | W + `old-title:s` | `notify::title` of `Meta.Window` | 1.0 (M3) |
| `window.workspace-changed` | no | W + `from:i to:i` | `Meta.Window::workspace-changed` | 1.0 (M3) |
| `window.monitor-changed` | no | W + `from:i to:i` | `window-entered-monitor` (`window-manager.c`) | 1.0 (M3) |
| `window.state-changed` | no | W + `maximized:b fullscreen:b changed:as` | `notify::maximized-*`, `notify::fullscreen` of `Meta.Window` | 1.0 (M3) |
| `workspace.switched` | no | `from:i to:i` | `WindowManager._switchWorkspace` | 1.0 (M3) |
| `workspace.added` | no | `index:i` | `WorkspaceManager::workspace-added` | 1.0 (M3) |
| `workspace.removed` | no | `index:i` | `WorkspaceManager::workspace-removed` | 1.0 (M3) |
| `search.query` | no | `query:s terms:as` | `view_do_search ()` in `src/ui/search.c` (`ShellSearchResultsView`), when the search starts after the debounce of 150 ms or is forced by activating the default result (`query` is the terms joined by spaces); needs capability `search` | 1.0 (M3) |
| `dash.item-added` | no | `kind:s owner:s index:i` + `app-id:s` for apps | `dash_redisplay ()` in `src/ui/dash.c` (`Dash._redisplay`), once for every application item added to the dash, after its `show ()`; `shell.dash` for buttons (M7) | 1.0 (M3) |
| `quick-settings.opened` | no | (empty) | `open-state-changed` of `QuickSettingsMenu` (`js/ui/quickSettings.js`) | 1.0 (M4) |
| `notification.showing` | yes | N | `tray_on_notification_request_banner ()` in `src/ui/message-tray.c` (`MessageTray._onNotificationRequestBanner`), after the checks that already skip the banner (acknowledged, low urgency, banners off) and before the notification is queued or its shown banner updated | 1.0 (M4) |
| `notification.shown` | no | N | `tray_show_notification ()` in `src/ui/message-tray.c` (`MessageTray._showNotification`), when the banner starts to slide in | 1.0 (M4) |
| `notification.activated` | no | N | `shell_notification_real_activate ()` in `src/ui/message-tray.c` (`Notification.activate`), before `::activated` is emitted | 1.0 (M4) |
| `screenshot.taken` | no | `kind:s clipboard:b` + `file:s` when saved | `store_screenshot ()` in `src/ui/screenshot-ui.c` (`_storeScreenshot`), after the screenshot was stored in the clipboard and (unless saving to disk is disabled) in a file, for the screenshot UI, the screenshot key bindings and "Take Screenshot" of the window menu. Not emitted for the screenshots taken through the `org.gnome.Shell.Screenshot` D-Bus interface | 1.0 (M4) |
| `osk.shown` | no | `monitor:i` | `osk_animate_show ()` in `src/ui/keyboard.c`, when the keyboard starts to slide in (`monitor` is the 1-based monitor it is on) | 1.0 (M4) |
| `osk.hidden` | no | (empty) | `osk_animate_hide ()` in `src/ui/keyboard.c`, when a shown keyboard starts to slide out | 1.0 (M4) |

The three `1.0` rows at the top are declared today
(`shell_global_declare_hooks()`), and the two `panel.*` rows by `shell_panel_class_init ()`
(`src/ui/panel.c`), and the two `osk.*` rows by `shell_osk_manager_class_init ()`
(`src/ui/keyboard.c`), and `search.query` by `shell_search_results_view_class_init ()`
(`src/ui/search.c`), and the three `notification.*` rows by `shell_notification_class_init ()` and
`shell_message_tray_class_init ()` (`src/ui/message-tray.c`), and `screenshot.taken` by `shell_screenshot_ui_class_init ()` (`src/ui/screenshot-ui.c`), and `dash.item-added` by `shell_dash_class_init ()` (`src/ui/dash.c`), so they exist once those classes are loaded, before any extension. Every other row is **reserved**: the name,
cancellability and payload are the contract, but the hook is declared by the
code that owns it when that module is ported (milestone in the last column).
`shell.hooks.list()` lists only declared hooks, so extensions feature-detect
(see `examples/hello@gnome-lua.example`); connecting to a reserved but
undeclared name raises `unknown hook`.

## Payload conventions

Payloads are `a{sv}`; the types below are the GVariant types of the values,
delivered to Lua by variant.md (every integer becomes a Lua integer). Keys are
delivered verbatim, so they stay hyphenated. Optional keys are **omitted**, never
sent as empty placeholders, unless the table says "may be empty".
Workspace and monitor numbers are 1-based, as in `window.md`.

- **W** (window snapshot): `window-id:t`, `title:s` (may be empty), `wm-class:s`
  (may be empty), `app-id:s` (omitted when the window tracker has no app),
  `workspace:i` (omitted when the window is on all workspaces), `monitor:i`,
  `pid:u` (omitted when unknown). Values are those at emission time; for
  `window.unmapped` the last known ones. `window-id` is the `Window.id` of
  window.md.
- **N** (notification summary): `notification-id:u` (assigned by the shell,
  unique for the session, not the D-Bus id), `source:s` (source title),
  `urgency:s` (`low`, `normal`, `high`, `critical`), `resident:b`, `transient:b`,
  `owner:s` (uuid of the extension that posted it through `shell.notify`, else
  empty) and `app-id:s` when the source belongs to an application. **Never
  title or body**: other applications' notification content is not an
  extension surface in v1.
- `from:i`/`to:i` in `window.workspace-changed` are omitted when the window was
  / is on all workspaces; in `workspace.switched` they are always present.
- `panel.*`: `name` is the status area name (panel.md), `box` is `left`,
  `center` or `right`, `owner` is an extension uuid or empty for the shell's
  own. `panel.indicator-added` is emitted for built-in indicators as well.
- `dash.item-added`: `kind` is `app` or `button`; `owner` is empty for apps;
  `index` is the 1-based position among all dash icons at that moment.
- `screenshot.taken`: `kind` is `screen`, `area` or `window`; `file` is an
  absolute path, omitted for clipboard-only captures.
- `workspace.removed`: `index` is the number the workspace had before removal.
- `monitors.changed`: `primary` is `0` when there are no monitors.

### Open enumerations

`reason` and `target` values may grow in minor versions; listeners MUST treat
an unknown value like `other`.

| Key | Values |
|---|---|
| `overview.showing` `reason` | `hot-corner` (`layout.c` hot corner, handled in `main.js`), `activities-button` (`panel.js`), `overlay-key`, `keybinding`, `gesture`, `drag`, `dbus` (`shell-dbus.c`, via `main.js`), `other` |
| `overview.hiding` `reason` | the values above, plus `escape`, `window-activated` (`Main.activateWindow`), `app-launched`, `search`, `session` (session mode change or lock) |
| `target` | `window-picker`, `app-grid` |

## Cancel and amend

A listener cancels with `shell.hooks.CANCEL` or amends by returning a table
(core.md); the first CANCEL stops the before phase. Unknown amendment keys are
ignored (debug log); an amendment of the wrong type is dropped whole.

| Hook | CANCEL means | Honoured when | Amendable keys |
|---|---|---|---|
| `overview.showing` | the overview stays hidden | every `reason` except `dbus` | `target` |
| `overview.hiding` | the overview stays shown | `reason` is `hot-corner`, `activities-button`, `overlay-key`, `keybinding` or `gesture`; ignored (and logged) for `escape`, `window-activated`, `app-launched`, `search`, `session`, `dbus`, `other`, so no extension can trap the user in the overview | none |
| `notification.showing` | no banner and no sound; the entry still goes to the message list | always | none |

## Rules for emitters

- Emit `before` from the state transition, the action runs unless CANCEL, then
  `after` (CONVENTIONS.md). `after` hooks run synchronously after the new state
  is visible through the API (`shell.windows.focused()` already returns the new
  window inside `window.focused`).
- Ordering guarantees for one object: `window.mapped` precedes all other
  `window.*` hooks for that id and `window.unmapped` is the last;
  `workspace.added` precedes any `window.workspace-changed` naming it;
  `overview.showing` -> `overview.shown` and `overview.hiding` ->
  `overview.hidden` always pair unless the first is cancelled.
- A listener that calls an API action on the emitter's object (closing the
  window it is told about, destroying the notification) is deferred to idle
  (README rule 8). Hooks are never emitted from `dispose`.
- `search.query` is emitted after the shell's own debounce, not per key press,
  and not at all for the lock screen. Gated hooks (`search.query`) appear in
  `shell.hooks.list()` with an additional `capability` key; connecting without
  the capability raises `hook 'search.query' requires the "search" capability`.
