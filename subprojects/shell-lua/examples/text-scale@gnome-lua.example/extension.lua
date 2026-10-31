-- text-scale@gnome-lua.example: a setting of the desktop (docs/api/core.md,
-- shell.settings.open, capability "settings").
--
-- Every extension may open its own schema (shell.settings.extension ());
-- opening any other installed schema needs the "settings" capability. This
-- one changes text-scaling-factor of org.gnome.desktop.interface, the size
-- of the text everywhere, from a slider in the quick settings, and follows
-- the key when something else changes it (Settings, gsettings).
local shell = require("shell")

-- The slider goes from 0 to 1; the text from normal to twice as large
local SMALLEST, LARGEST = 1.0, 2.0

local function to_slider(factor)
  return math.max(0, math.min(1, (factor - SMALLEST) / (LARGEST - SMALLEST)))
end

local function to_factor(value)
  -- steps of 5%: the slider moves in small steps, the setting need not
  return math.floor((SMALLEST + value * (LARGEST - SMALLEST)) * 20 + 0.5) / 20
end

local ext = {}

function ext.enable()
  local interface = shell.settings.open("org.gnome.desktop.interface")

  -- Callbacks run for every change, assignments included (README rule 7):
  -- each direction writes only what differs, so the two cannot loop
  local slider = shell.quick_settings.add_slider{
    icon_name = "preferences-desktop-font-symbolic",
    accessible_name = _("Text size"),
    value = to_slider(interface:get_double("text-scaling-factor")),
    on_changed = function(_, value)
      local factor = to_factor(value)

      if factor ~= interface:get_double("text-scaling-factor") then
        interface:set_double("text-scaling-factor", factor)
      end
    end,
  }

  interface:on_changed("text-scaling-factor", function(_, factor)
    if to_factor(slider.value) ~= factor then
      slider.value = to_slider(factor)
    end
  end)
end

return ext
