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

Planned for M2–M5 (names reserved, not yet declared): `panel.indicator-added`,
`panel.menu-opened`, `overview.showing` (cancellable), `overview.shown`,
`overview.hiding` (cancellable), `overview.hidden`, `window.mapped`,
`window.unmapped`, `window.focused`, `window.minimized`, `workspace.switched`,
`search.query`, `dash.item-added`, `quick-settings.opened`,
`notification.showing` (cancellable), `notification.shown`,
`notification.activated`, `screenshot.taken`, `osk.shown`, `osk.hidden`.
