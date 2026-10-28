-- dash-launcher@gnome-lua.example: buttons in the dash (docs/api/dash.md).
--
-- A dash button is an icon with a label and one action; it is not an
-- application icon and does not count as a favorite. shell.apps (docs/api/
-- window.md) looks applications up and launches them.
local shell = require("shell")

-- The first of these that is installed
local CANDIDATES = {
  "org.gnome.Console.desktop",
  "org.gnome.Terminal.desktop",
  "org.gnome.Nautilus.desktop",
  "org.gnome.Settings.desktop",
}

local ext = {}

function ext.enable()
  local app_id, info

  for _, id in ipairs(CANDIDATES) do
    info = shell.apps.info(id)
    if info then
      app_id = id
      break
    end
  end

  -- Launches the application, as a click on its dash icon would. The result
  -- is `true`, or `nil, message` when launching failed (an environmental
  -- failure is a return value, not an error).
  shell.dash.add_button{
    label = info and info.name or _("Launcher"),
    icon_name = info and info.icon_name or "application-x-executable-symbolic",
    position = "end",               -- the group after the application icons
    on_click = function()
      if not app_id then
        shell.log.info("none of the applications is installed")
        return
      end
      local ok, message = shell.apps.launch(app_id)
      if not ok then
        shell.log.info("cannot launch %s: %s", app_id, message)
      end
    end,
  }

  -- A second button, in the group before the application icons. It toggles a
  -- mark (the :checked pseudo-class) and keeps the overview open.
  shell.dash.insert_at{
    index = 1,
    position = "start",
    label = _("Mark"),
    icon_name = "starred-symbolic",
    hide_overview = false,
    on_click = function(self)
      self.checked = not self.checked
    end,
  }

  -- dash.item-added also fires for the buttons (hooks.md)
  for _, hook in ipairs(shell.hooks.list()) do
    if hook.name == "dash.item-added" then
      shell.hooks.on("dash.item-added", function(payload)
        if payload.kind == "button" then
          shell.log.debug("a dash button was added by '%s' at position %d", payload.owner, payload.index)
        end
      end)
    end
  end
end

return ext
