-- extension.lua
--
-- This program is free software: you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation, either version 2 of the License, or
-- (at your option) any later version.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program.  If not, see <http://www.gnu.org/licenses/>.
--
-- SPDX-License-Identifier: GPL-2.0-or-later

local shell = require("shell")

local ext = {}

function ext.enable()
  -- An indicator in the top bar with a popup menu. There is nothing to clean
  -- up in disable(): the shell destroys the indicator with the extension.
  local indicator = shell.panel.add_indicator{
    accessible_name = _("My Shiny Indicator"),
    icon_name = "face-smile-symbolic",
  }

  indicator.menu:add_item{
    label = _("Show Notification"),
    on_activate = function()
      shell.notify{ title = _("Whatʼs up, folks?") }
    end,
  }
end

return ext
