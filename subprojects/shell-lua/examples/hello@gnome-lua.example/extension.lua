-- hello@gnome-lua.example: the canonical minimal gnome-lua extension.
--
-- extension.lua runs once when the extension is enabled, in a sandbox: only
-- string, table, math, utf8, coroutine, the basic functions and `shell` exist
-- (no os, io, debug, load or arbitrary require). It must return a table; the
-- optional `enable` (and `disable`) fields of that table are called by the
-- shell.
--
-- `require("shell")` and the global `shell` are the same table.
local shell = require("shell")

local ext = {}

-- Hook we would like to follow, if this shell version declares it. No hooks
-- are declared in M1, so this stays dormant until they appear; connecting to
-- an unknown hook name raises an error, hence the check against list().
local WANTED_HOOK = "window.focused"

local function hook_exists(name)
  for _, hook in ipairs(shell.hooks.list()) do
    if hook.name == name then
      return true
    end
  end
  return false
end

function ext.enable()
  shell.log.info("hello from Lua")

  -- A repeating timer. Timers belong to the extension: they are cancelled
  -- automatically when it is disabled, so there is nothing to clean up.
  local counter = 0
  shell.timer.every{
    ms = 5000,
    callback = function()
      counter = counter + 1
      shell.log.info("hello counter: %d", counter)
      return true -- returning false would stop the timer
    end,
  }

  if hook_exists(WANTED_HOOK) then
    shell.hooks.on(WANTED_HOOK, function(payload)
      shell.log.info("%s fired", WANTED_HOOK)
    end)
  else
    shell.log.debug("hook %s is not available, skipping", WANTED_HOOK)
  end

  -- Settings are only available when metadata.json names a "settings-schema"
  -- (shipped in schemas/). This extension has none, so this is skipped.
  if shell.extension.metadata["settings-schema"] then
    local settings = shell.settings.extension()
    settings:on_changed(nil, function(key)
      shell.log.info("setting %s changed", key)
    end)
  end
end

-- There is deliberately no disable(): the timer and the hook connection above
-- are owned by the extension context and are torn down by the shell when the
-- extension is disabled. Define disable() only for state outside the shell.

return ext
