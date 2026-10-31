-- disk-space@gnome-lua.example: running a program (docs/api/spawn.md,
-- capability "spawn").
--
-- shell.spawn runs a program directly, without a shell, and calls on_exit
-- with its status and output once it has ended; the shell never waits for
-- it. "spawn" is the broadest capability: the program runs as the user. This
-- one runs `df`, in the POSIX format and the C locale so that its output is
-- the same everywhere, and shows the free space of / and of the home file
-- system.
local shell = require("shell")
local ui = shell.ui

local EVERY_MS = 10 * 60 * 1000

local function human(kib)
  local units = { "K", "M", "G", "T", "P" }
  local i = 1

  while kib >= 1024 and i < #units do
    kib = kib / 1024
    i = i + 1
  end
  return (kib >= 10 or i == 1) and ("%.0f%s"):format(kib, units[i]) or ("%.1f%s"):format(kib, units[i])
end

-- The lines of `df -Pk` after its header: file system, size, used,
-- available (KiB), capacity, mount point
local function parse(output)
  local rows = {}

  for line in output:gmatch("[^\n]+") do
    local avail, capacity, mount = line:match("^%S+%s+%d+%s+%d+%s+(%d+)%s+(%d+)%%%s+(.+)$")

    if avail then
      rows[#rows + 1] = { avail = tonumber(avail), capacity = tonumber(capacity), mount = mount }
    end
  end
  return rows
end

local ext = {}

function ext.enable()
  local label = ui.label{ text = "…", y_align = "center" }
  local indicator = shell.panel.add_indicator{
    name = "disk-space-example",
    accessible_name = _("Free disk space"),
    child = ui.box{ children = {
      ui.icon{ icon_name = "drive-harddisk-symbolic", style_class = "system-status-icon" },
      label,
    } },
  }
  local menu = indicator.menu
  local lines = {}
  local refresh

  local function show(rows)
    for _, item in ipairs(lines) do
      item:destroy()
    end
    lines = {}
    for i, row in ipairs(rows) do
      lines[i] = menu:add_item{
        label = _("%s: %s free (%d%% used)"):format(row.mount, human(row.avail), row.capacity),
        sensitive = false,
        index = i,
      }
    end
    label.text = rows[1] and human(rows[1].avail) or "?"
  end

  refresh = function()
    local process, err = shell.spawn{
      -- the data directory is in the home directory, so df reports the
      -- file system of the user's files with it (twice the same one when
      -- home is not a file system of its own)
      argv = { "df", "-Pk", "/", shell.extension.data_dir },
      env = { LC_ALL = "C" },
      timeout_ms = 10000,
      on_exit = function(result)
        if result.status ~= 0 and result.stdout == "" then
          label.text = "?"
          shell.log.warning("df failed: %s", result.stderr)
          return
        end
        show(parse(result.stdout))
      end,
    }

    if not process then                     -- df is not installed, or not in PATH
      label.text = "?"
      shell.log.warning("cannot run df: %s", err)
    end
  end

  menu:add_separator{}
  menu:add_item{ label = _("Refresh"), on_activate = function() refresh() end }

  refresh()
  shell.timer.every{ ms = EVERY_MS, callback = refresh }
end

return ext
