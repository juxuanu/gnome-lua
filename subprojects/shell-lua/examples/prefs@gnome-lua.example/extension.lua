-- prefs@gnome-lua.example: the extension behind the preferences window of
-- prefs.lua. It logs a banner and then a counter; the settings that the
-- window edits control it. settings:on_changed() delivers the change as soon
-- as the user makes it, in the window or with dconf.
local shell = require("shell")

local ext = {}

function ext.enable()
  local settings = shell.settings.extension()
  local counter = 0
  local timer

  local function tick()
    counter = counter + 1
    if settings:get_string("format") == "long" then
      shell.log.info("tick %d of %s (%d columns, opacity %.2f, accent %s)", counter,
                     settings:get_string("greeting"), settings:get_int("columns"),
                     settings:get_double("opacity"), settings:get_string("accent"))
    else
      shell.log.info("tick %d", counter)
    end
    return true
  end

  -- The interval is read when the timer starts, so a change starts it again
  local function restart()
    if timer then
      timer:cancel()
    end
    timer = shell.timer.every{ ms = settings:get_int("interval") * 1000, callback = tick }
  end

  if settings:get_boolean("show-banner") then
    shell.log.info("%s from %s", settings:get_string("greeting"), shell.extension.metadata.name)
  end

  restart()
  settings:on_changed("interval", restart)
  settings:on_changed("show-banner", function(_, on)
    shell.log.debug("banner %s", on and "on" or "off")
  end)
end

return ext
