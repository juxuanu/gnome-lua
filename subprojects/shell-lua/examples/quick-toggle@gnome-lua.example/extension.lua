-- quick-toggle@gnome-lua.example: a quick-settings toggle with a menu and a
-- slider, and a status icon in the top bar (docs/api/quick-settings.md).
--
-- The state lives in the extension's settings (schemas/), so it survives a
-- restart and can be changed from outside (dconf, the preferences). The
-- widgets are updated from settings:on_changed(); assigning to a widget
-- property never calls the widget's own callback (docs/api/README.md, rule 7),
-- so this synchronisation cannot loop.
local shell = require("shell")
local qs = shell.quick_settings

local ICON = "weather-clear-night-symbolic"

local ext = {}

function ext.enable()
  local settings = shell.settings.extension()

  local function subtitle(on)
    return on and _("On") or _("Off")
  end

  -- The icon in the system area of the top bar, shown while the toggle is on
  local status = qs.add_indicator{
    icon_name = ICON,
    accessible_name = _("Soft light is on"),
    visible = settings:get_boolean("enabled"),
  }

  -- The toggle: the main area toggles, the arrow opens the menu
  local toggle = qs.add_menu_toggle{
    title = _("Soft light"),
    subtitle = subtitle(settings:get_boolean("enabled")),
    icon_name = ICON,
    checked = settings:get_boolean("enabled"),
    -- the user toggled it: `on` is the new value, already applied
    on_toggle = function(_, on)
      settings:set_boolean("enabled", on)
    end,
  }
  toggle.menu:set_header{ icon_name = ICON, title = _("Soft light"), subtitle = _("Dims the colours") }

  -- The slider: its row is a grid item of its own
  local slider = qs.add_slider{
    icon_name = "display-brightness-symbolic",
    accessible_name = _("Soft light level"),
    value = settings:get_double("level"),
    sensitive = settings:get_boolean("enabled"),
    on_changed = function(_, value)
      settings:set_double("level", value)
    end,
  }

  toggle.menu:add_item{
    label = _("Reset the level"),
    on_activate = function()
      settings:reset("level")
    end,
  }

  -- Changes made elsewhere reach the widgets; the assignments are silent
  settings:on_changed("enabled", function(_, on)
    toggle.checked = on
    toggle.subtitle = subtitle(on)
    status.visible = on
    slider.sensitive = on
  end)
  settings:on_changed("level", function(_, level)
    if math.abs(slider.value - level) > 0.001 then
      slider.value = level
    end
  end)
end

return ext
