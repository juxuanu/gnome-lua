# `shell.notify`: notifications (version 1, draft)

Scope: showing notifications through the shell's own message tray, with
actions and a dismissal callback, either one-off (`shell.notify{...}`) or
grouped under a persistent source (`shell.notify.source{...}`). Models
`ShellNotification`, `ShellSource` and `ShellNotificationAction`
(`src/ui/message-tray.c`, ported from `js/ui/messageTray.js` and the `Source`
base of `js/ui/messageList.js`) and `Main.notify`
(`js/ui/main.js`). Observing *other* applications' notifications is hooks only
(`notification.*`, hooks.md) and never carries title or body.

Not in v1:
- `urgency = "critical"`: critical notifications bypass do-not-disturb and are
  never auto-hidden (`tray_on_notification_request_banner ()` in `src/ui/message-tray.c`); needs a
  capability, so not before a minor release.
- Custom banners/widgets inside a notification, images beyond an icon, sounds
  (`Notification.sound`), `privacy_scope` (fixed to the user scope, so the lock
  screen follows the user's policy), `datetime` overrides, `forFeedback`.
- Replacing/updating by id across restarts; D-Bus `org.freedesktop.Notifications`
  semantics (that is the daemon's job; use `shell.dbus` to talk to it).
- Reading, listing, dismissing or activating notifications of other sources.
- Source-level click handlers (`Source.open`) and per-source policy objects:
  extension sources use the generic policy (`NotificationGenericPolicy`), so
  the user can silence them only globally.
- More than 3 actions (raises); markup in titles.

## Status

**M7, implemented.** `src/lua-api/shell-lua-api-notify.c` (the module and the
handle types) over `ShellNotification` and `ShellSource`
(`src/ui/message-tray.c`); `shell_source_get_owner ()` / `_set_owner ()` there
give the notification hooks their `owner`. The hooks `notification.showing`,
`.shown` and `.activated` were declared with the port of the message tray.
Conformance test: `tests/api/notify.lua` with the fixture
`tests/api/fixtures/notify@test`. Differences from the text below are marked
**(as built)** where they occur.

## `shell.notify{...}` -> `shell.Notification`

`shell.notify` is a callable table (like `shell.log`). It posts to the
extension's **default source**: title = metadata `name`, icon
`application-x-addon-symbolic`; the shell creates it on first use and drops it
when it holds no notification, as `Source` does when its last notification is
destroyed (`source_notification_destroy_cb ()` in `src/ui/message-tray.c`).

| Field | Type | Default | Notes |
|---|---|---|---|
| `title` | string | required | |
| `body` | string | nil | |
| `markup` | boolean | false | interpret `body` as Pango markup (`use-body-markup`); invalid markup is shown as plain text |
| `icon_name` / `icon_file` | string | source icon | at most one. **(as built)** `icon_file` is a path relative to the extension directory, `.svg` or `.png`, as in ui.md |
| `urgency` | `"low"`, `"normal"`, `"high"` | `"normal"` | `low` never shows a banner, only the list entry; DND (`show-banners` off) suppresses banners of the others |
| `resident` | boolean | false | stays after activation or an action |
| `transient` | boolean | false | removed after its banner expires instead of staying in the list (`isTransient`, as `Main.notify`) |
| `actions` | sequence of `{label = string, on_activate = function(notification)}` | none | up to 3, shown as banner buttons in order |
| `on_activate` | function(self) | nil | user clicked the notification itself |
| `on_dismissed` | function(self, reason) | nil | see below |

Argument errors raise; a failed post never returns nil. **(as built)** Every
entry of `actions` needs a `label` and an `on_activate`; `urgency = "critical"`
raises, for the constructors and for the property. The default source is
named after the metadata `name` (the uuid if there is none). Where there is no
message tray (no shell) the call raises `the message tray does not exist`.

## `shell.Notification`

| Property | Type | Access | Notes |
|---|---|---|---|
| `title`, `body` | string | rw | updating a shown notification updates it in place |
| `icon_name` | string | rw | |
| `urgency` | enum above | rw | |
| `resident`, `transient` | boolean | rw | |
| `acknowledged` | boolean | rw | true = seen; assigning false re-requests a banner |
| `alive` | boolean | ro | |

Methods: `n:show()` (request the banner again; no-op for `low`; **(as built)**
it assigns `acknowledged = true` and then `false`, which is what asks the tray),
`n:destroy()` (withdraw; suppresses `on_dismissed`), `n:on_activate(fn)`,
`n:on_dismissed(fn)`.

Semantics, from `src/ui/message-tray.c`:
- **Activation** (`shell_notification_real_activate ()`): `on_activate` runs,
  then a non-resident notification is destroyed.
- **Actions** (`shell_notification_action_activate ()`): the action's `on_activate(notification)`
  runs, then a non-resident notification is destroyed.
- **`on_dismissed(self, reason)`** runs once, when the notification goes away
  for a reason the extension did not cause by `destroy()`, source `destroy()`
  or disable. `reason` is `"expired"` (timed out, or pushed out by the 10th
  newer one: `MAX_NOTIFICATIONS_PER_SOURCE`), `"dismissed"` (user closed it, or
  it was destroyed after activation/an action), `"source-closed"` (the shell
  closed the source). It therefore also follows `on_activate` for non-resident
  notifications. **(as built)** The handle is valid while `on_dismissed` runs
  and dead afterwards; a notification the shell replaces (a reason only the
  notification daemon uses) is `"dismissed"`.
- Banners queue at most 3 at once; extra ones appear only in the list
  (`MAX_NOTIFICATIONS_IN_QUEUE`).

## `shell.notify.source{...}` -> `shell.NotificationSource`

Groups notifications under a title and icon in the message list, so an
extension with many notifications does not mix them with "System".

| Field | Type | Notes |
|---|---|---|
| `title` | string, required | |
| `icon_name` / `icon_file` | string | one at most |

`shell.NotificationSource`: `title` (rw), `icon_name` (rw), `count` (ro
integer: live notifications), `alive`.
Methods: `src:notify{...}` (the same fields as `shell.notify` and the same
result), `src:clear()` (destroys non-resident notifications,
`destroyNonResidentNotifications`; suppresses their `on_dismissed`),
`src:destroy()` (destroys all its notifications and the source).

"Persistent" means the **handle** stays valid while the source has zero
notifications: the shell registers the source with the tray on the first
`notify` and unregisters it when empty (upstream behaviour) and registers it
again on the next one. Nothing survives a shell restart or disable.
**(as built)** A `ShellSource` cannot be used again once it destroyed itself, so
the handle wraps a private object that holds the title and the icon and
creates a new `ShellSource` for the next `notify`; `count` is 0 while there is
none. `title` and `icon_name` written to the handle reach the live source at
once. A handle is dead after `src:destroy()`.

## Example

```lua
local shell = require("shell")
local src = shell.notify.source{ title = "Mail checker", icon_name = "mail-unread-symbolic" }
local function mark_read(id) shell.log.info("marking %s read", id) end

local function new_mail(id, from, subject)
  return src:notify{
    title = from, body = subject,
    actions = {
      { label = "Open", on_activate = function() shell.apps.launch("org.gnome.Evolution.desktop") end },
      { label = "Mark read", on_activate = function() mark_read(id) end },
    },
    on_dismissed = function(_, reason) shell.log.debug("mail notice %s: %s", id, reason) end,
  }
end

new_mail("m1", "Ada", "Lunch?")
shell.notify{ title = "Mail checker started", urgency = "low", transient = true }
```
