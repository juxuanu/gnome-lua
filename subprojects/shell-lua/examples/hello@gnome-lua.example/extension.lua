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

-- Settings come from the schema named by "settings-schema" in metadata.json
-- (schemas/, compiled). The same keys are what prefs.lua shows in the
-- preferences window.
local function log_level_function(level)
  if level == "debug" then
    return shell.log.debug
  end
  return shell.log.info
end

function ext.enable()
  local settings = shell.settings.extension()

  shell.log.info("%s", settings:get_string("greeting"))

  -- A repeating timer. Timers belong to the extension: they are cancelled
  -- automatically when it is disabled, so there is nothing to clean up.
  local counter = 0
  local timer

  local function start_timer()
    if timer then
      timer:cancel()
    end
    timer = shell.timer.every{
      ms = settings:get_int("interval") * 1000,
      callback = function()
        counter = counter + 1
        if settings:get_boolean("log-ticks") then
          log_level_function(settings:get_string("level"))("hello counter: %d", counter)
        end
        return true -- returning false would stop the timer
      end,
    }
  end
  start_timer()

  -- The interval is read when the timer starts, so a change restarts it
  settings:on_changed("interval", start_timer)

  if hook_exists(WANTED_HOOK) then
    shell.hooks.on(WANTED_HOOK, function(payload)
      shell.log.info("%s fired", WANTED_HOOK)
    end)
  else
    shell.log.debug("hook %s is not available, skipping", WANTED_HOOK)
  end
end

-- There is deliberately no disable(): the timer, the settings connection and the
-- hook connection above are owned by the extension context and are torn down
-- by the shell when the extension is disabled. Define disable() only for state
-- outside the shell.

return ext
