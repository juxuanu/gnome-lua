-- This script tests that the shell handles connecting monitors after startup
-- is properly handled.

-- The start of the script runs before the startup of the shell is complete,
-- which waits for a monitor: it connects one after two seconds.

local display = global.display
local test_monitor = nil

assert (display:get_n_monitors () == 0, "Not running headless")

global:connect ("shutdown", function ()
  if test_monitor then
    test_monitor:destroy ()
    test_monitor = nil
  end
end)

test.timeout (2000, function ()
  test.log ("Connecting 1280x720 test monitor")
  test_monitor = test.new_monitor (1280, 720, 60.0)
end)

test.define_event ("monitorsChanged", "Monitors changed")

local monitor_manager = global.backend:get_monitor_manager ()
local monitors_changed_id
monitors_changed_id = monitor_manager:connect ("monitors-changed", function ()
  assert (display:get_n_monitors () == 1, "Expected a single monitor")
  test.event ("monitorsChanged")
  monitor_manager:disconnect (monitors_changed_id)
end)

local monitors_changed = false
local overview_hidden = false
local overview_shown = false

test.on_script_event ("monitorsChanged", function ()
  monitors_changed = true
end)

test.on_script_event ("overviewHideDone", function ()
  overview_hidden = true
end)

test.on_script_event ("overviewShowDone", function ()
  overview_shown = true
end)

test.finish (function ()
  assert (monitors_changed, "Monitors never changed")
  assert (overview_hidden, "Failed to hide overview")
  assert (overview_shown, "Failed to show overview")
end)

test.wait_startup ()

test.define_event ("overviewShowDone", "Overview finished showing")
test.define_event ("overviewHideDone", "Overview finished hiding")

main.overview:connect ("shown", function ()
  test.event ("overviewShowDone")
end)
main.overview:connect ("hidden", function ()
  test.event ("overviewHideDone")
end)
main.overview:hide ()
test.wait_leisure ()

main.overview:show ()
test.wait_leisure ()
