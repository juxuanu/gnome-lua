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

-- The extension API is documented in docs/api of gnome-shell (README.md first).
local shell = require("shell")

local ext = {}

function ext.enable()
  -- Everything created through the shell table (widgets, indicators, timers,
  -- hook connections, ...) belongs to the extension and is destroyed by the
  -- shell when the extension is disabled
  shell.log.info("%s is enabled", shell.extension.metadata.name)
end

-- disable() is optional: define it only for what the shell does not know of
function ext.disable()
end

return ext
