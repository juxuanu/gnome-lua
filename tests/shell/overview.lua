-- This script tests the overview (src/ui/overview.c): it shows the overview,
-- waits for it to be shown, switches to the application grid, runs a search
-- and hides the overview, under fatal warnings.

local TIMEOUT = 10000

local overview = main.overview

local results = {
  hidden_at_start = false,
  shown = false,
  visible_while_shown = false,
  app_grid_shown = false,
  search_started = false,
  search_ended = false,
  hidden = false,
}

test.finish (function ()
  assert (results.hidden_at_start, "The overview was visible at startup")
  assert (results.shown, "Failed to show the overview")
  assert (results.visible_while_shown, "The overview is not visible once shown")
  assert (results.app_grid_shown, "Failed to show the application grid")
  assert (results.search_started, "Failed to start the search")
  assert (results.search_ended, "Failed to end the search")
  assert (results.hidden, "Failed to hide the overview")
end)

test.wait_startup ()

test.log ("Running overview test")

test.sleep (1000)
test.wait_leisure ()

-- The session starts with the overview shown (the startup animation leaves
-- it visible): hide it first
if overview.visible then
  test.log ("Hiding the overview shown at startup")
  local hidden_at_start = test.watch (overview, "hidden")
  overview:hide ()
  test.wait (hidden_at_start, TIMEOUT)
  test.wait_leisure ()
end
results.hidden_at_start = not overview.visible

-- show the overview (window picker)
test.log ("Showing the overview")
local shown = test.watch (overview, "shown")
overview:show ()
test.wait (shown, TIMEOUT)
results.shown = true
results.visible_while_shown = overview.visible and not overview.animation_in_progress
test.wait_leisure ()

-- switch to the application grid
test.log ("Showing the application grid")
local show_apps = overview.dash.show_apps_button
show_apps.checked = true
test.sleep (600)
test.wait_leisure ()
results.app_grid_shown = show_apps.checked and overview.visible

-- run a search
test.log ("Searching")
local search_controller = overview.search_controller
local search_entry = overview.search_entry
search_entry.text = "a"
test.wait_for (function ()
  return search_controller.search_active
end, "the search to start", TIMEOUT)
results.search_started = true
test.sleep (800)
test.wait_leisure ()

search_entry.text = ""
test.wait_for (function ()
  return not search_controller.search_active
end, "the search to end", TIMEOUT)
results.search_ended = true
test.sleep (600)
test.wait_leisure ()

-- hide the overview
test.log ("Hiding the overview")
local hidden = test.watch (overview, "hidden")
overview:hide ()
test.wait (hidden, TIMEOUT)
results.hidden = not overview.visible
test.wait_leisure ()

test.log ("Finished overview test")
