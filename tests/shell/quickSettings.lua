-- This script tests the quick settings of the system (src/ui/quick-settings.c,
-- src/ui/status/*.c) under fatal warnings: the toggles that bind their icon
-- in their instance init, before ShellQuickToggle has made its StIcon (Auto
-- Rotate, bound to ShellSystemActions:orientation-lock-icon), show it. The
-- toggle used to drop the icon set that early, so Auto Rotate had none.

local results = {
  rotation_icon = false,
}

test.finish (function ()
  assert (results.rotation_icon, "The Auto Rotate toggle has no icon")
end)

local function find_all (actor, gtype, found)
  found = found or {}
  for _, child in ipairs (actor:get_children ()) do
    if child:gtype () == gtype then
      found[#found + 1] = child
    end
    find_all (child, gtype, found)
  end
  return found
end

test.wait_startup ()

test.log ("Running quick settings test")

-- Hidden without an orientation sensor, but there
local toggle = find_all (stage, "ShellRotationToggle")[1]
assert (toggle ~= nil, "no Auto Rotate toggle in the quick settings")
local function rotation_icon (t)
  local icon = t.icon_name
  return icon == "rotation-allowed-symbolic" or icon == "rotation-locked-symbolic", icon
end

local ok, icon = rotation_icon (toggle)
assert (ok, "the icon of Auto Rotate is " .. tostring (icon))

-- A new one, now that the system actions have settled: only the binding
-- made in the instance init gives it its icon. With the lock off, the new
-- toggle writes the setting back when it binds checked to it, and the
-- in-memory settings of the test report a change, after which the system
-- actions notify the icon again: that hid the bug. With the lock on,
-- checked keeps its default and nothing is written.
test.set_boolean ("org.gnome.settings-daemon.peripherals.touchscreen", "orientation-lock", true)
test.wait_leisure ()
local fresh = test.new ("ShellRotationToggle", {})
ok, icon = rotation_icon (fresh)
test.log ("Auto Rotate icon of a new toggle: " .. tostring (icon))
assert (ok, "the icon of a new Auto Rotate toggle is " .. tostring (icon))
results.rotation_icon = true

test.log ("Finished quick settings test")
