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
| `time.timezone-changed` | no | `timezone:s` | `timezone_file_changed_cb ()` in `subprojects/shell-lua/src/shell-lua-api-time.c`, declared and started by `shell_lua_time_watch ()` from `shell_global_declare_hooks ()`: a `GFileMonitor` on `/etc/localtime` and `/etc/timezone` reports a change and the identifier of the local zone (`shell.time.timezone ()`) differs from the last one seen; `timezone` is the new identifier. A change of the `TZ` environment variable of the shell is not watched | 1.1 |
| `panel.indicator-added` | no | `name:s box:s owner:s` | `add_to_panel_box ()` in `src/ui/panel.c`, once per indicator when it is first registered in the status area: the indicators of the shell when the session mode first lists them (`owner` empty), the others through `shell_panel_add_to_status_area_with_owner ()` and, for the indicators of `shell.panel`, `shell_panel_add_extension_indicator ()` (`register_indicator ()` emits the hook once the indicator is in its place). Not repeated when a session mode change moves an indicator | 1.0 (M2) |
| `panel.menu-opened` | no | `name:s box:s` | the `open-state-changed` handler (`menu_open_state_changed_cb ()`) that `src/ui/panel.c` installs on the menu of every indicator from `indicator_menu_set_cb ()` (the former `Panel._onMenuSet`), when the menu opens | 1.0 (M2) |
| `monitors.changed` | no | `count:i primary:i` | `monitors_changed ()` in `src/ui/layout.c`, after `ShellLayoutManager::monitors-changed` was emitted (the layout manager has updated itself) | 1.0 (M2) |
| `overview.showing` | yes | `reason:s target:s` + `owner:s` when `reason` is `extension` | `shell_overview_show_for_reason ()` in `src/ui/overview.c` (`Overview.show()`) for the "before" phase, and `change_shown_state ()` for the "after" phase, together with the `showing` signal; a swipe gesture asks in its `begin` handler; `shell_overview_show_for_extension ()` (`shell.overview.show`, overview.md) is the same with `reason = "extension"` | 1.0 (M3); `extension` and `owner` 1.1 |
| `overview.shown` | no | `target:s` | `change_shown_state ()` in `src/ui/overview.c` (`Overview._showDone()`), together with the `shown` signal | 1.0 (M3) |
| `overview.hiding` | yes | `reason:s` + `owner:s` when `reason` is `extension` | `shell_overview_hide_for_reason ()` in `src/ui/overview.c` (`Overview.hide()`) for the "before" phase, and `change_shown_state ()` for the "after" phase, together with the `hiding` signal. A gesture that ends by hiding the overview does not ask; `shell_overview_hide_for_extension ()` (`shell.overview.hide`) is the same with `reason = "extension"` | 1.0 (M3); `extension` and `owner` 1.1 |
| `overview.hidden` | no | (empty) | `change_shown_state ()` in `src/ui/overview.c` (`Overview._hideDone()`), together with the `hidden` signal | 1.0 (M3) |
| `window.mapped` | no | W | `shell_window_hooks_window_mapped ()` (`src/lua-api/shell-window-hooks.c`), called at the start of `on_map ()` in `src/ui/window-manager.c` (the `map` effect of `ShellWM`, before the map animation): the first hook of a window. **Replayed on connect (1.1)**: a new listener (`shell.hooks.on`) is called once for every window that was already mapped, from an idle after the connection, to that listener only (window.md, "The windows that exist when a listener connects"); `ShellHookRegistry` replay, `shell_hook_registry_set_replay ()` | 1.0 (M3); replay 1.1 |
| `window.unmapped` | no | W | `shell_window_hooks_window_unmapped ()`, called from `MetaWindow::unmanaging` (`window_unmanaging_cb ()` in `src/lua-api/shell-window-hooks.c`; also from the start of `on_destroy ()` in `src/ui/window-manager.c`, the `destroy` effect, which a window closed on another workspace never gets): the window is gone, `shell.windows.get ()` no longer finds it (not emitted on minimize) | 1.0 (M3) |
| `window.focused` | no | W + `previous-window-id:t` | `notify::focus-window` of `MetaDisplay` (`display_focus_window_cb ()` in `src/lua-api/shell-window-hooks.c`); not emitted when the focus goes to no window or to a window that is not listed; `previous-window-id` is absent when no listed window had the focus | 1.0 (M3) |
| `window.minimized` | no | W | `shell_window_hooks_window_minimized ()`, called at the start of `on_minimize ()` in `src/ui/window-manager.c` | 1.0 (M3) |
| `window.unminimized` | no | W | the same, from `on_unminimize ()` | 1.0 (M3) |
| `window.title-changed` | no | W + `old-title:s` | `notify::title` of `MetaWindow` (`window_title_cb ()` in `src/lua-api/shell-window-hooks.c`) | 1.0 (M3) |
| `window.workspace-changed` | no | W + `from:i to:i` | `MetaWindow::workspace-changed` (`window_workspace_cb ()`); not emitted when removing a workspace only renumbers the others | 1.0 (M3) |
| `window.monitor-changed` | no | W + `from:i to:i` | `MetaDisplay::window-entered-monitor` (`display_window_entered_monitor_cb ()`); `from` is omitted for the first monitor the window is on | 1.0 (M3) |
| `window.state-changed` | no | W + `maximized:b fullscreen:b above:b on-all-workspaces:b changed:as` | `notify::maximized-horizontally`, `notify::maximized-vertically`, `notify::fullscreen`, `notify::above`, `notify::on-all-workspaces` of `MetaWindow` (`window_state_cb ()`), once per change of `maximized` (both axes), `fullscreen`, `above` or `on-all-workspaces`; `changed` lists which of `maximized`, `fullscreen`, `above`, `on-all-workspaces` changed | 1.0 (M3); `above`, `on-all-workspaces` and their entries in `changed`: 1.1 |
| `window.geometry-changed` | no | W + `x:i y:i width:i height:i` | `MetaWindow::position-changed` and `::size-changed` (`window_geometry_cb ()` in `src/lua-api/shell-window-hooks.c`), coalesced: one emission per window per turn of the main loop (a burst of configure events during a resize is one emission with the last frame), and only when the frame rectangle differs from the one last reported. `x y width height` are the frame rectangle in global logical pixels, `Window.frame` of window.md. The placement the window has when `window.mapped` is emitted is not reported (W has no geometry: read `Window.frame`); the hook reports the changes after it | 1.1 |
| `workspace.switched` | no | `from:i to:i` | `shell_window_hooks_workspace_switched ()`, called at the start of `on_switch_workspace ()` in `src/ui/window-manager.c` (the `switch-workspace` effect, also when it is not animated) | 1.0 (M3) |
| `workspace.added` | no | `index:i` | `MetaWorkspaceManager::workspace-added` (`workspace_added_cb ()` in `src/lua-api/shell-window-hooks.c`) | 1.0 (M3) |
| `workspace.removed` | no | `index:i` | `MetaWorkspaceManager::workspace-removed` (`workspace_removed_cb ()`) | 1.0 (M3) |
| `search.query` | no | `query:s terms:as` | `view_do_search ()` in `src/ui/search.c` (`ShellSearchResultsView`), when the search starts after the debounce of 150 ms or is forced by activating the default result (`query` is the terms joined by spaces); needs capability `search` | 1.0 (M3) |
| `dash.item-added` | no | `kind:s owner:s index:i` + `app-id:s` for apps | `dash_redisplay ()` in `src/ui/dash.c` (`Dash._redisplay`), once for every application item added to the dash, after its `show ()`; `shell_dash_add_extension_button ()` in `src/ui/dash.c` for the buttons of `shell.dash`, once per button, after it is in place and shown | 1.0 (M3) |
| `quick-settings.opened` | no | (empty) | `quick_settings_menu_open_state_changed_cb ()` in `src/ui/quick-settings.c` (`open-state-changed` of `ShellQuickSettingsMenu`), when the menu has opened; not emitted when it closes | 1.0 (M4) |
| `notification.showing` | yes | N | `tray_on_notification_request_banner ()` in `src/ui/message-tray.c` (`MessageTray._onNotificationRequestBanner`), after the checks that already skip the banner (acknowledged, low urgency, banners off) and before the notification is queued or its shown banner updated | 1.0 (M4) |
| `notification.shown` | no | N | `tray_show_notification ()` in `src/ui/message-tray.c` (`MessageTray._showNotification`), when the banner starts to slide in | 1.0 (M4) |
| `notification.activated` | no | N | `shell_notification_real_activate ()` in `src/ui/message-tray.c` (`Notification.activate`), before `::activated` is emitted | 1.0 (M4) |
| `screenshot.taken` | no | `kind:s clipboard:b` + `file:s` when saved | `store_screenshot ()` in `src/ui/screenshot-ui.c` (`_storeScreenshot`), after the screenshot was stored in the clipboard and (unless saving to disk is disabled) in a file, for the screenshot UI, the screenshot key bindings and "Take Screenshot" of the window menu. Not emitted for the screenshots taken through the `org.gnome.Shell.Screenshot` D-Bus interface | 1.0 (M4) |
| `osk.shown` | no | `monitor:i` | `osk_animate_show ()` in `src/ui/keyboard.c`, when the keyboard starts to slide in (`monitor` is the 1-based monitor it is on) | 1.0 (M4) |
| `osk.hidden` | no | (empty) | `osk_animate_hide ()` in `src/ui/keyboard.c`, when a shown keyboard starts to slide out | 1.0 (M4) |

The three `1.0` rows at the top are declared today
(`shell_global_declare_hooks()`), and `time.timezone-changed` by `shell_lua_time_watch ()`
(`subprojects/shell-lua/src/shell-lua-api-time.c`, called from the same function), and the two `panel.*` rows by `shell_panel_class_init ()`
(`src/ui/panel.c`), and the two `osk.*` rows by `shell_osk_manager_class_init ()`
(`src/ui/keyboard.c`), and `search.query` by `shell_search_results_view_class_init ()`
(`src/ui/search.c`), and `quick-settings.opened` by `shell_quick_settings_menu_class_init ()`
(`src/ui/quick-settings.c`), and the three `notification.*` rows by `shell_notification_class_init ()` and
`shell_message_tray_class_init ()` (`src/ui/message-tray.c`; their `owner` is `shell_source_get_owner ()` of the
source of the notification, which `shell.notify` sets), and `screenshot.taken` by `shell_screenshot_ui_class_init ()` (`src/ui/screenshot-ui.c`), and `dash.item-added` by `shell_dash_class_init ()` (`src/ui/dash.c`), and the four `overview.*` rows by `shell_overview_class_init ()` (`src/ui/overview.c`), and the `window.*` and `workspace.*` rows by `shell_window_hooks_declare ()` (`src/lua-api/shell-window-hooks.c`, called from `shell_window_manager_class_init ()` in `src/ui/window-manager.c`), and `monitors.changed` by `shell_layout_manager_class_init ()` (`src/ui/layout.c`), so they exist once those classes are loaded, before any extension. Every other row is **reserved**: the name,
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
  window.md. Only the windows `shell.windows.list()` shows have hooks (not
  skip-taskbar windows: docks, the shell's own chrome), and a window's first hook
  is `window.mapped` even for one that was not listed when it mapped but is now;
  windows that were already mapped when the shell's window manager started get
  no live `window.mapped`, but (1.1) a new listener of `window.mapped` is
  called for each of them once (see below, "Replay").
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
| `overview.showing` `reason` | `hot-corner` (`layout.c` hot corner, handled in `overview.c`), `activities-button` (`panel-items.c`), `overlay-key`, `keybinding`, `gesture`, `drag`, `dbus` (`shell-dbus.c`, via `main.js`), `other`, `extension` (1.1: `shell.overview`, with `owner` = the uuid of the extension that asked) |
| `overview.hiding` `reason` | the values above, plus `escape`, `window-activated` (`Main.activateWindow`), `app-launched`, `search`, `session` (session mode change or lock) |
| `target` | `window-picker`, `app-grid` |

## Cancel and amend

A listener cancels with `shell.hooks.CANCEL` or amends by returning a table
(core.md); the first CANCEL stops the before phase. Unknown amendment keys are
ignored (debug log); an amendment of the wrong type is dropped whole.

| Hook | CANCEL means | Honoured when | Amendable keys |
|---|---|---|---|
| `overview.showing` | the overview stays hidden | every `reason` except `dbus` | `target` |
| `overview.hiding` | the overview stays shown | `reason` is `hot-corner`, `activities-button`, `overlay-key`, `keybinding` or `gesture`; ignored (and logged) for `escape`, `window-activated`, `app-launched`, `search`, `session`, `dbus`, `other`, `extension`, so no extension can trap the user in the overview | none |
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
- **Replay (1.1).** A hook may be declared with a replay: when a listener
  connects to its `after` phase, the shell delivers to that listener, and only
  to it, the events that describe what already exists, one per object, from an
  idle after the connect returned (never synchronously inside
  `shell.hooks.on`). The objects are collected when the idle runs; a listener
  that disconnected before then gets nothing; live emissions that happen
  between the connect and the idle are not delivered separately, the replay
  covers them (so nothing is delivered twice). The replay is not an emission:
  other listeners, `before` listeners and the tracing signal of the registry see
  nothing. Today only `window.mapped` has one. The order across hooks of
  one listener is not guaranteed during that short interval (window.md).
- A listener that calls an API action on the emitter's object (closing the
  window it is told about, destroying the notification) is deferred to idle
  (README rule 8; the actions of `shell.Window` and `shell.Workspace` do it for
  every `window.*` and `workspace.*` hook, whichever window they name). Hooks
  are never emitted from `dispose`.
- `search.query` is emitted after the shell's own debounce, not per key press,
  and not at all for the lock screen. Gated hooks (`search.query`) appear in
  `shell.hooks.list()` with an additional `capability` key; connecting without
  the capability raises `hook 'search.query' requires the "search" capability`.
