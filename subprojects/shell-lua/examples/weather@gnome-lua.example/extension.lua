-- weather@gnome-lua.example: an HTTP client (docs/api/network.md,
-- capability "network").
--
-- shell.network.request sends a request and returns at once; exactly one of
-- on_done (there was an HTTP response, whatever its status) and on_error (no
-- response: offline, no such host, a timeout) runs later, from the main
-- loop. This one asks wttr.in for the weather in its one-line format,
-- "Barcelona: ☀️ +18°C".
local shell = require("shell")
local ui = shell.ui

-- A place, "Barcelona" or "Saint+Malo" (spaces as +); empty: wttr.in guesses
-- it from the address the request comes from
local LOCATION = ""
local URL = "https://wttr.in/" .. LOCATION .. "?format=3"

local FIRST_MS = 5000                       -- the network may not be up at login
local EVERY_MS = 30 * 60 * 1000             -- wttr.in updates about as often

local ext = {}

function ext.enable()
  local label = ui.label{ text = "…", y_align = "center" }
  local indicator = shell.panel.add_indicator{
    name = "weather-example",
    accessible_name = _("Weather"),
    child = label,
  }
  local place = indicator.menu:add_item{ label = _("Not updated yet"), sensitive = false }
  local pending

  local function update()
    if pending and pending.alive then
      return                                -- the previous request is still out
    end

    pending = shell.network.request{
      url = URL,
      timeout_ms = 20000,
      on_done = function(response)
        local where, weather = response.body:match("^%s*(.-):%s*(.-)%s*$")

        if response.status ~= 200 or not weather then
          label.text = "?"
          place.label = _("The weather service answered %d"):format(response.status)
          return
        end
        label.text = weather
        place.label = where
      end,
      on_error = function(err)
        -- err.code: "resolve", "connection", "timeout", "tls", ... (network.md)
        label.text = "—"
        place.label = _("No weather: %s"):format(err.message)
      end,
    }
  end

  indicator.menu:add_separator{}
  indicator.menu:add_item{ label = _("Refresh"), on_activate = update }

  shell.timer.after{ ms = FIRST_MS, callback = update }
  shell.timer.every{ ms = EVERY_MS, callback = update }
end

return ext
