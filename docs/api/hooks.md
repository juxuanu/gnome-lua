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
| `session.mode-changed` | no | `mode: s` | `sessionMode.js` after `_sync()` | 1.0 |
| `panel.indicator-added` | no | `name:s box:s owner:s` | `Panel._addToPanelBox` (`js/ui/panel.js`), later `src/ui/panel.c` | 1.0 (M2) |
| `panel.menu-opened` | no | `name:s box:s` | the `open-state-changed` handler installed by `Panel._onMenuSet` | 1.0 (M2) |
| `monitors.changed` | no | `count:i primary:i` | `LayoutManager::monitors-changed` (`js/ui/layout.js`) | 1.0 (M2) |
| `overview.showing` | yes | `reason:s target:s` | `Overview.show()` (`js/ui/overview.js`) | 1.0 (M3) |
| `overview.shown` | no | `target:s` | `Overview._showDone()` | 1.0 (M3) |
| `overview.hiding` | yes | `reason:s` | `Overview.hide()` | 1.0 (M3) |
| `overview.hidden` | no | (empty) | `Overview._hideDone()` | 1.0 (M3) |
| `window.mapped` | no | W | `WindowManager._mapWindowDone` (`js/ui/windowManager.js`) | 1.0 (M3) |
| `window.unmapped` | no | W | `WindowManager._destroyWindow`: the window is gone (not emitted on minimize) | 1.0 (M3) |
| `window.focused` | no | W + `previous-window-id:t` | `notify::focus-window` of `Meta.Display` | 1.0 (M3) |
| `window.minimized` | no | W | `WindowManager._minimizeWindow` | 1.0 (M3) |
| `window.unminimized` | no | W | `WindowManager._unminimizeWindow` | 1.0 (M3) |
| `window.title-changed` | no | W + `old-title:s` | `notify::title` of `Meta.Window` | 1.0 (M3) |
| `window.workspace-changed` | no | W + `from:i to:i` | `Meta.Window::workspace-changed` | 1.0 (M3) |
| `window.monitor-changed` | no | W + `from:i to:i` | `window-entered-monitor` (`windowManager.js`) | 1.0 (M3) |
| `window.state-changed` | no | W + `maximized:b fullscreen:b changed:as` | `notify::maximized-*`, `notify::fullscreen` of `Meta.Window` | 1.0 (M3) |
| `workspace.switched` | no | `from:i to:i` | `WindowManager._switchWorkspace` | 1.0 (M3) |
| `workspace.added` | no | `index:i` | `WorkspaceManager::workspace-added` | 1.0 (M3) |
| `workspace.removed` | no | `index:i` | `WorkspaceManager::workspace-removed` | 1.0 (M3) |
| `search.query` | no | `query:s terms:as` | `SearchController._onTextChanged` (`js/ui/searchController.js`); needs capability `search` | 1.0 (M3) |
| `dash.item-added` | no | `kind:s owner:s index:i` + `app-id:s` for apps | `Dash._redisplay` and `shell.dash` | 1.0 (M3) |
| `quick-settings.opened` | no | (empty) | `open-state-changed` of `QuickSettingsMenu` (`js/ui/quickSettings.js`) | 1.0 (M4) |
| `notification.showing` | yes | N | `MessageTray._onNotificationRequestBanner` (`js/ui/messageTray.js`) | 1.0 (M4) |
| `notification.shown` | no | N | `MessageTray._showNotification` | 1.0 (M4) |
| `notification.activated` | no | N | `Notification.activate` | 1.0 (M4) |
| `screenshot.taken` | no | `kind:s clipboard:b` + `file:s` when saved | `_storeScreenshot` (`js/ui/screenshot.js`) | 1.0 (M4) |
| `osk.shown` | no | `monitor:i` | `Keyboard.open` (`js/ui/keyboard.js`) | 1.0 (M4) |
| `osk.hidden` | no | (empty) | `Keyboard.close` | 1.0 (M4) |

The three `1.0` rows at the top are declared today
(`shell_global_declare_hooks()`). Every other row is **reserved**: the name,
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
| `overview.showing` `reason` | `hot-corner` (`layout.js` HotCorner), `activities-button` (`panel.js`), `overlay-key`, `keybinding`, `gesture`, `drag`, `dbus` (`shellDBus.js`), `other` |
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
