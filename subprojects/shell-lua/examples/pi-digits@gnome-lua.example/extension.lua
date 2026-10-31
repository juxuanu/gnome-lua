-- pi-digits@gnome-lua.example: expensive work in a worker (docs/api/worker.md,
-- capability "worker").
--
-- Computing 3000 digits of pi takes a second or more of pure Lua. Run from a
-- menu item on the main thread, it would freeze the desktop (the shell draws
-- the screen on that thread) until the watchdog stopped it at 500 ms. A
-- worker runs workers/pi.lua in a thread of its own instead: the extension
-- posts it the number of digits, the worker posts back its progress and then
-- the digits, and the top bar stays smooth all along.
local shell = require("shell")
local ui = shell.ui

local DIGITS = 3000

local ext = {}

function ext.enable()
  local label = ui.label{ text = "π", y_align = "center" }
  local indicator = shell.panel.add_indicator{
    name = "pi-digits-example",
    accessible_name = _("Digits of pi"),
    child = label,
  }
  local menu = indicator.menu
  local result = menu:add_item{ label = _("Not computed yet"), sensitive = false }
  local worker

  local function idle()
    label.text = "π"
    worker = nil
  end

  local function compute()
    if worker then
      return                              -- already at it
    end

    worker = shell.worker.start{
      module = "workers.pi",
      on_message = function(message)
        if message.progress then
          label.text = ("π %d%%"):format(message.progress)
          return
        end

        -- the answer: show the start of it, notify, end the worker
        result.label = message.digits:sub(1, 32) .. "…"
        shell.notify{
          title = _("%d digits of pi"):format(DIGITS),
          body = message.digits:sub(1, 200) .. "…",
        }
        worker:destroy()
        idle()
      end,
      -- the worker ended before its answer: an error, or Cancel
      on_error = function(err)
        if err.code ~= "terminated" then
          result.label = _("Failed: %s"):format(err.message)
        end
        idle()
      end,
    }

    if not worker then                    -- the shell runs as many workers as it allows
      result.label = _("No worker available now, try again later")
      return
    end
    label.text = "π 0%"
    worker:post(DIGITS)
  end

  menu:add_separator{}
  menu:add_item{ label = _("Compute %d digits"):format(DIGITS), on_activate = compute }
  menu:add_item{
    label = _("Cancel"),
    on_activate = function()
      if worker then
        worker:terminate()                  -- on_error follows, with "terminated"
      end
    end,
  }
end

return ext
