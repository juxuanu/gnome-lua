-- indicator@gnome-lua.example: a top-bar indicator with a popup menu.
--
-- shell.panel.add_indicator{} (docs/api/panel.md) puts a button in the top
-- bar; its content is built from shell.ui widgets (docs/api/ui.md) and its
-- popup is a shell.Menu (docs/api/menu.md). Everything created through
-- shell.* belongs to the extension: there is no disable() because the shell
-- destroys the indicator, the menu and the widgets when the extension is
-- disabled.
local shell = require("shell")
local ui = shell.ui

local ext = {}

function ext.enable()
  local clicks = 0

  -- The content of the button: an icon and a counter. A widget is never
  -- shown on its own, it becomes visible by being placed.
  local icon = ui.icon{ icon_name = "face-smile-symbolic", style_class = "system-status-icon" }
  local count = ui.label{ text = "0", y_align = "center", style_class = "indicator-example-count" }

  local indicator = shell.panel.add_indicator{
    name = "indicator-example",            -- the status area role, unique in the shell
    accessible_name = _("Indicator example"),
    position = "right",                    -- left, center or right box of the panel
    edge = "start",                        -- left of the built-in system area
    child = ui.box{ children = { icon, count } },
  }
  local menu = indicator.menu

  -- An action: the whole menu closes after on_activate
  menu:add_item{
    label = _("Say hello"),
    on_activate = function()
      clicks = clicks + 1
      count.text = tostring(clicks)
      shell.log.info("hello number %d", clicks)
    end,
  }

  -- A switch: on_toggled runs when its state changes (docs/api/README.md,
  -- rule 7); this script never assigns it, so here it is the user
  menu:add_switch{
    label = _("Happy"),
    state = true,
    on_toggled = function(_, happy)
      icon.icon_name = happy and "face-smile-symbolic" or "face-sad-symbolic"
    end,
  }

  -- A submenu with a choice, drawn with the ornament of a radio button
  local size = menu:add_submenu{ label = _("Size"), icon_name = "view-fullscreen-symbolic" }
  local sizes = {}
  local function choose(chosen)
    for _, item in ipairs(sizes) do
      item.ornament = item == chosen and "dot" or "no-dot"
    end
    shell.log.info("size is now %s", chosen.label)
  end
  for _, label in ipairs({ _("Small"), _("Medium"), _("Large") }) do
    sizes[#sizes + 1] = size.menu:add_item{ label = label, ornament = "no-dot", on_activate = choose }
  end
  sizes[2].ornament = "dot"

  menu:add_separator{}

  menu:add_image_item{
    label = _("Reset the counter"),
    icon_name = "edit-undo-symbolic",
    on_activate = function()
      clicks = 0
      count.text = "0"
    end,
  }

  -- Events of the menu itself
  menu:on_open_state_changed(function(_, open)
    shell.log.debug("the menu is %s", open and "open" or "closed")
  end)
end

return ext
