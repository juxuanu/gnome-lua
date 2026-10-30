-- This script tests the notifications of the lock screen
-- (src/ui/unlock-dialog.c) under fatal warnings: with the screen locked, a
-- notification arrives and goes, and its source with it (a source destroys
-- itself with its last notification), as when an application closes its
-- notification while the screen is locked for a suspend.

local TIMEOUT = 10000

local shield = main.screen_shield
local tray = main.message_tray

local results = {
  locked = false,
  source_destroyed = false,
}

test.finish (function ()
  assert (results.locked, "The screen did not lock")
  assert (results.source_destroyed, "The source of the notification was not destroyed")
end)

test.wait_startup ()

test.log ("Running lock screen notifications test")

test.log ("Locking the screen")
shield:lock (false)
test.wait_for (function () return shield.locked end, "the screen to lock", TIMEOUT)
results.locked = true
test.wait_leisure ()

local source = tray:get_system_source ()
local source_destroyed = test.watch (source, "destroy")
local notification = test.new ("ShellNotification", {
  source = source,
  title = "While locked",
  body = "A notification that arrives while the screen is locked",
})
source:add_notification (notification)
test.wait_leisure ()

test.log ("Closing the notification")
notification:destroy ()
test.wait (source_destroyed, TIMEOUT)
results.source_destroyed = true
test.wait_leisure ()
