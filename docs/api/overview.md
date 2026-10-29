# `shell.overview`: showing and hiding the overview (version 1.1)

Scope: reading the state of the overview (the window picker, the application
grid, the dash and the search all live inside it) and asking the shell to show
or hide it, as the Activities button, the hot corner and the overlay key do.
Models `Main.overview` of the JS shell (`ShellOverview`, `src/ui/overview.c`,
was `js/ui/overview.js`): `visible`, `animationInProgress`, `show ()`,
`showApps ()`, `hide ()` and `toggle ()`. What happens *during* the overview is
observed with the `overview.*` hooks (hooks.md).

Since: **1.1**. Every row below is new in 1.1; an extension that uses it
declares `"shell-api": "1.1"`.

Not in v1.1:
- Putting the key focus in the search entry, typing into it, or selecting an
  application or a workspace in the overview (use `shell.search` for results
  and `shell.windows`/`shell.workspaces` for windows and workspaces).
- Waiting for the animation to end from a function (nothing blocks): listen to
  `overview.shown` and `overview.hidden`.
- Changing the animation, the layout, the order or the content of the window
  picker or of the application grid.
- Cancelling the hiding that the user asks for: `overview.hiding` listeners
  cannot trap the user in the overview (hooks.md, "Cancel and amend").

No capability: the overview is visible to the user, and showing it does not
give the extension any data.

## Status

**1.1, implemented.** `src/lua-api/shell-lua-api-overview.c` over
`shell_overview_show_for_extension ()`, `shell_overview_hide_for_extension ()`
and `shell_overview_toggle_for_extension ()` in `src/ui/overview.c`. Conformance
test: `tests/api/overview.lua` with the fixture `tests/api/fixtures/overview@test`.

## `shell.overview`

| Member | Type | Since | Notes |
|---|---|---|---|
| `visible` | boolean (read-only) | 1.1 | true from the moment the overview starts to show until it is completely hidden, so also during both animations (`Overview.visible`). True while the session starts in the overview |
| `target` | `"hidden"`, `"window-picker"`, `"app-grid"` (read-only) | 1.1 | where the overview is going or resting. `"hidden"` as soon as a hide is under way (even while the hide animation still runs) and while it is not visible; otherwise the page the overview is going to or showing, also after the user switched pages with the Show Applications button. A read in the middle of a gesture or of a drag gives the page the gesture is nearest to |
| `animating` | boolean (read-only) | 1.1 | an animation of the overview runs (`Overview.animationInProgress`): showing or hiding, not the switch between the two pages |
| `show{...}` | function | 1.1 | shows the overview, below |
| `hide()` | function | 1.1 | hides it, below |
| `toggle()` | function | 1.1 | `hide()` when `visible` is true, else `show()` with the window picker (`Overview.toggle ()`) |

**`shell.overview.show{ target = "window-picker" | "app-grid" }`**

| Field | Type | Default | Notes |
|---|---|---|---|
| `target` | `"window-picker"`, `"app-grid"` | `"window-picker"` | the page to show. `shell.overview.show()` without a table is the same as `show{}`; any other value of `target` raises, naming the field |

The calls return nothing. They start the animation and return at once:
`visible` is already true when `show` returns (unless the showing was
cancelled, below), `target` is the new target, and `animating` is true until
the animation ends; `overview.shown` / `overview.hidden` tell when that is.

Rules:
- `show` when the overview is **already shown or showing** does nothing, also
  when `target` names the other page (as `Overview.show ()` returns when the
  overview is `_shown`); `hide` when it is hidden or already hiding does
  nothing. A `show` during the hide animation takes effect when that
  animation ends, and shows the window picker (as the JS overview did).
- Both emit the before and the after hooks like every other cause does:
  `overview.showing` / `overview.hiding` with `reason = "extension"` and
  `owner` = the uuid of the extension (hooks.md). A listener (of this or of
  another extension) may cancel a showing, so `visible` can still be false after
  `show` returned: the call is a request. A hiding asked for by an extension is
  **not** cancellable (the reason `extension` is not in the list of cancellable
  reasons of hooks.md).
- In a session without an overview (the login and lock screens) the calls do
  nothing and `visible` is false; extensions normally only run in the `user`
  mode (`session-modes`, README.md).
- Like for the user's own request, the overview needs the grab of the pointer
  and the keyboard: when another grab is active (a window is being moved, a
  system dialog is open) the overview hides again at once.
- Called from inside a listener of an `overview.*` hook, the action is deferred
  until that hook returns and runs from an idle (README.md rule 8): `visible`
  and `target` still have their old values right after the call. A deferred
  action whose extension was disabled meanwhile is dropped.

## Example

```lua
local shell = require("shell")

-- A shortcut-less way to the application grid: a dash button
shell.dash.add_button{
  label = "All apps", icon_name = "view-app-grid-symbolic", hide_overview = false,
  on_click = function()
    if shell.overview.target == "app-grid" then
      shell.overview.hide()
    else
      shell.overview.show{ target = "app-grid" }
    end
  end,
}

-- Leave the overview when a notification of ours is activated
shell.hooks.on("notification.activated", function(p)
  if p.owner == shell.extension.uuid and shell.overview.visible then
    shell.overview.hide()
  end
end)
```
