-- prefs.lua
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

-- The preferences window is made from this table (docs/api/prefs.md of
-- gnome-shell): every row is bound to a key of the settings schema of the
-- extension (schemas/). The extension reacts to a change with
-- shell.settings.extension():on_changed().
return {
  groups = {
    {
      title = _("Section"),
      rows = {
        { type = "switch", key = "enabled", title = _("Title"), subtitle = _("Subtitle") },
      },
    },
  },
}
