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
  -- The icon in the top bar, shown while the toggle is on
  local indicator = shell.quick_settings.add_indicator{
    icon_name = "face-smile-symbolic",
    accessible_name = _("Smile"),
    visible = false,
  }

  -- The toggle in the quick settings menu. There is nothing to clean up in
  -- disable(): the shell destroys the items with the extension.
  shell.quick_settings.add_toggle{
    title = _("Smile"),
    icon_name = "face-smile-symbolic",
    on_toggle = function(toggle, checked)
      indicator.visible = checked
    end,
  }
end

return ext
