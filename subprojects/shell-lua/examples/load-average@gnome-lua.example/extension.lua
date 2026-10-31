-- load-average@gnome-lua.example: reading a file of the system
-- (docs/api/fs.md, capability "fs").
--
-- Without a capability an extension reads its own directory and reads and
-- writes its data directory (shell.extension.data_dir). Anything else, here
-- /proc/loadavg, needs "fs". Reading is asynchronous: the contents arrive in
-- on_done, from the main loop, and the shell never waits for the disk.
local shell = require("shell")
local ui = shell.ui

local FILE = "/proc/loadavg"
local EVERY_MS = 5000

local ext = {}

function ext.enable()
  local label = ui.label{ text = "…", y_align = "center" }
  local indicator = shell.panel.add_indicator{
    name = "load-average-example",
    accessible_name = _("Load average"),
    child = label,
  }
  local details = indicator.menu:add_item{ label = "…", sensitive = false }
  local processes = indicator.menu:add_item{ label = "…", sensitive = false }

  local function update()
    shell.fs.read{
      path = FILE,
      -- "0.42 0.36 0.30 2/1234 5678": the averages over 1, 5 and 15
      -- minutes, the runnable and total tasks, the last pid
      on_done = function(contents)
        local one, five, fifteen, running, total = contents:match("^(%S+) (%S+) (%S+) (%d+)/(%d+)")

        if not one then
          return
        end
        label.text = one
        details.label = _("1, 5 and 15 minutes: %s, %s, %s"):format(one, five, fifteen)
        processes.label = _("%s of %s tasks runnable"):format(running, total)
      end,
      on_error = function(err)
        label.text = "?"
        shell.log.warning("cannot read %s: %s", FILE, err.message)
      end,
    }
  end

  update()
  shell.timer.every{ ms = EVERY_MS, callback = update }
end

return ext
