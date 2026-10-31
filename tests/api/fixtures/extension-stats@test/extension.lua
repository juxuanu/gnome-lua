-- Fixture of tests/api/extension-stats.lua: the tooling that finds the
-- extensions that make the desktop stutter. A timer callback that runs for
-- 40 ms, longer than a frame; the script reads what the shell measured from
-- the extension info (D-Bus) and from gnome-extensions stats.
local shell = require("shell")
local T = shell.test

local function busy(ms)
  local start = shell.time.monotonic()
  while shell.time.monotonic() - start < ms / 1000 do end
end

local ext = {}

function ext.enable()
  shell.timer.after{ ms = 1, callback = function()
    busy(40)
    T.report("the slow callback ran", true)
    T.done("slow")
  end }
  T.done()
end

return ext
