-- power-profile@gnome-lua.example: a D-Bus client (docs/api/dbus.md,
-- capability "dbus").
--
-- power-profiles-daemon publishes the power profile on the system bus
-- (org.freedesktop.UPower.PowerProfiles). The indicator follows the service
-- as it comes and goes (watch_name), reads the ActiveProfile property and
-- listens for its changes, and the menu sets it. Every D-Bus call is
-- asynchronous: nothing here ever waits for the bus.
local shell = require("shell")
local ui = shell.ui

local SERVICE = "org.freedesktop.UPower.PowerProfiles"

-- The profiles the daemon knows, with their icons and names
local PROFILES = {
  { id = "power-saver", icon = "power-profile-power-saver-symbolic", label = _("Power Saver") },
  { id = "balanced", icon = "power-profile-balanced-symbolic", label = _("Balanced") },
  { id = "performance", icon = "power-profile-performance-symbolic", label = _("Performance") },
}

local ext = {}

function ext.enable()
  local bus = shell.dbus.system()
  local daemon = bus:proxy{
    name = SERVICE,
    path = "/org/freedesktop/UPower/PowerProfiles",
    interface = SERVICE,
  }

  local icon = ui.icon{ icon_name = PROFILES[2].icon, style_class = "system-status-icon" }
  local indicator = shell.panel.add_indicator{
    name = "power-profile-example",
    accessible_name = _("Power profile"),
    visible = false,                      -- until the daemon is there
    child = icon,
  }
  local items = {}

  -- Shows @id as the active profile
  local function show(id)
    for _, profile in ipairs(PROFILES) do
      items[profile.id].ornament = profile.id == id and "dot" or "no-dot"
      if profile.id == id then
        icon.icon_name = profile.icon
      end
    end
  end

  local function failed(what)
    return function(err)
      shell.log.warning("%s: %s", what, err.message)
    end
  end

  for _, profile in ipairs(PROFILES) do
    items[profile.id] = indicator.menu:add_item{
      label = profile.label,
      ornament = "no-dot",
      on_activate = function()
        -- The daemon answers with PropertiesChanged, which show () follows
        daemon:set_property{
          name = "ActiveProfile", signature = "s", value = profile.id,
          on_error = failed("cannot set the power profile"),
        }
      end,
    }
  end

  -- Changes made elsewhere (Settings, the quick settings, powerprofilesctl)
  daemon:on_properties_changed(function(changed)
    if changed.ActiveProfile then
      show(changed.ActiveProfile)
    end
  end)

  -- The daemon may start after the shell, restart, or not be installed
  bus:watch_name{
    name = SERVICE,
    on_appeared = function()
      daemon:get_property{
        name = "ActiveProfile",
        on_reply = function(id)
          show(id)
          indicator.visible = true
        end,
        on_error = failed("cannot read the power profile"),
      }
    end,
    on_vanished = function()
      indicator.visible = false
    end,
  }
end

return ext
