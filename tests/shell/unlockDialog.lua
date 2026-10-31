-- This script tests the unlock dialog (src/ui/unlock-dialog.c) under fatal
-- warnings: once the screen is locked, the indicator of the authentication
-- methods (src/gdm/auth-menu-button.c) must hold what it shows, the icons
-- and the description of its method. The indicator used to be made with an
-- icon-name, which GObject sets after constructed (): St replaced the box
-- with the icon, the indicator kept pointers to the box's children, and the
-- lock screen wrote into a freed label when the methods changed (a crash on
-- waking up from suspend).

local TIMEOUT = 10000

local shield = main.screen_shield

local results = {
  locked = false,
  indicator_found = false,
  indicator_built = false,
}

test.finish (function ()
  assert (results.locked, "The screen did not lock")
  assert (results.indicator_found, "The unlock dialog has no indicator of the authentication methods")
  assert (results.indicator_built, "The indicator of the authentication methods lost its icons and description")
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

test.log ("Running unlock dialog test")

test.log ("Locking the screen")
shield:lock (false)
test.wait_for (function () return shield.locked end, "the screen to lock", TIMEOUT)
results.locked = true
test.wait_leisure ()

local dialog = find_all (stage, "ShellUnlockDialog")[1]
assert (dialog ~= nil, "no unlock dialog once locked")
local indicator = find_all (dialog, "ShellAuthMenuButtonIndicator")[1]
results.indicator_found = indicator ~= nil
assert (results.indicator_found, "no indicator in the unlock dialog")

-- The child of the button is the box of _createChild (): the icons, then
-- the description
local box = indicator.child
local parts = box ~= nil and box:gtype () == "StBoxLayout" and box:get_children () or {}
results.indicator_built = #parts == 2
  and parts[1]:gtype () == "StBoxLayout"
  and parts[2]:gtype () == "StLabel"
assert (results.indicator_built,
        "the child of the indicator is " .. (box ~= nil and box:gtype () or "nil"))

test.log ("Finished unlock dialog test")
