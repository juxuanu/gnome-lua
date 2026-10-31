-- clock@gnome-lua.example: a top-bar clock (API 1.1).
--
-- Shows shell.time (docs/api/core.md), a timer aligned to the minute, the hook
-- time.timezone-changed (docs/api/hooks.md), a global keybinding
-- (docs/api/keybindings.md) and shell.overview (docs/api/overview.md).
--
-- Keyboard shortcut: Super+Alt+O toggles the overview, in the desktop and in
-- the overview itself. The combination is literal (fixed in this file); an
-- extension whose shortcut the user should be able to change would keep it in
-- an `as` key of its settings schema and pass `name = "<key>"` instead (see
-- the example in docs/api/keybindings.md).
--
-- The extension has no capability: time, keybindings and the overview are not
-- gated. There is no disable(): the shell removes the indicator, the timer,
-- the hook connection and the shortcut when the extension is disabled.
local shell = require("shell")
local ui = shell.ui

local ext = {}

local SHORTCUT = "<Super><Alt>o"

function ext.enable()
  local use_24h = true

  local label = ui.label{ text = "", y_align = "center", style_class = "clock-example-time" }
  local indicator = shell.panel.add_indicator{
    name = "clock-example",
    accessible_name = _("Clock"),
    position = "center",
    child = label,
  }

  -- Not reactive: it only shows the date
  local date_item = indicator.menu:add_item{ label = "", sensitive = false }

  -- shell.time.format takes a GDateTime format string: %H:%M is 14:05,
  -- %I:%M %p is 02:05 PM. "%A %x" is the weekday and the locale's date.
  local function render()
    label.text = shell.time.format(use_24h and "%H:%M" or "%I:%M %p")
    date_item.label = shell.time.format("%A %x")
  end

  -- A timer aligned to the minute: sleep until the next minute starts, then
  -- again, so the clock does not drift or lag by up to 60 seconds. shell.time.date()
  -- tells how far into the minute we are.
  local function schedule()
    local now = shell.time.date()
    local ms = (60 - now.second) * 1000 - now.microsecond // 1000
    shell.timer.after{ ms = math.max(ms, 1), callback = function()
      render()
      schedule()
    end }
  end

  -- A switch: on_toggled runs when its state changes (docs/api/README.md,
  -- rule 7); this script never assigns it, so here it is the user. The state
  -- is not saved: an extension that wants to remember it would use a settings
  -- key.
  indicator.menu:add_switch{
    label = _("24-hour clock"),
    state = use_24h,
    on_toggled = function(_item, on)
      use_24h = on
      render()
    end,
  }

  -- The local zone changed (the user picked another one in Settings): the
  -- minute boundaries are the same, only the text changes
  shell.hooks.on("time.timezone-changed", function(payload)
    shell.log.info("time zone is now %s", payload.timezone)
    render()
  end)

  -- A taken combination is not an error: add() answers nil, message
  local binding, err = shell.keybindings.add{
    accelerators = { SHORTCUT },
    on_pressed = function() shell.overview.toggle() end,
  }
  if not binding then
    shell.log.warning("no %s shortcut: %s", SHORTCUT, err)
  end

  render()
  schedule()
end

return ext
