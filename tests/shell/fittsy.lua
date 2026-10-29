-- Checks that the dash and the top bar extend to the edges of the screen, so
-- that they are easy to hit with the pointer (Fitts's law).

local EPSILON = 0.1

local results = {
  fittsyDash = false,
  fittsyTopBarVert = false,
  fittsyTopBarHorzLTR = false,
  fittsyTopBarHorzRTL = false,
}

-- The events carry a number: whether the actor under the pointer was the
-- actor that is meant to reach the edge
local function define_script_event (name, description)
  test.define_event (name, description, "i")
end

local function script_event (name, value)
  test.event_i (name, value and 1 or 0)
end

local function check_fittsiness (event_name, actor, orientation, edge)
  local x, y = actor:get_transformed_position ()
  local pick_x, pick_y

  if orientation == "horizontal" then
    pick_x, pick_y = edge, y + math.floor (actor.height / 2)
  else
    pick_x, pick_y = x + math.floor (actor.width / 2), edge
  end

  local picked_actor = stage:get_actor_at_pos ("reactive", pick_x, pick_y)

  script_event (event_name, picked_actor == actor)
end

for name in pairs (results) do
  test.on_script_event (name, function (time, num)
    results[name] = num ~= 0
  end)
end

test.finish (function ()
  assert (results.fittsyDash, "Dash does not extend to screen edge")
  assert (results.fittsyTopBarVert, "Top bar does not extend to top screen edge")
  assert (results.fittsyTopBarHorzLTR, "Top bar does not extend to left screen edge")
  assert (results.fittsyTopBarHorzRTL, "Top bar does not extend to right screen edge")
end)

test.wait_startup ()

define_script_event ("fittsyDash", "Dash extends to screen edge")
define_script_event ("fittsyTopBarVert", "Top bar extends to top screen edge")
define_script_event ("fittsyTopBarHorzLTR", "Top bar extends to left screen edge")
define_script_event ("fittsyTopBarHorzRTL", "Top bar extends to right screen edge")

local primary_monitor = main.layout_manager:get_primary_monitor ()
local top_edge = primary_monitor.y + EPSILON
local bottom_edge = primary_monitor.y + primary_monitor.height - EPSILON
local left_edge = primary_monitor.x + EPSILON
local right_edge = primary_monitor.x + primary_monitor.width - EPSILON

test.wait_leisure ()

local show_apps = main.overview.dash.show_apps_button
test.log ("Checking that dash extends to bottom screen edge")
check_fittsiness ("fittsyDash", show_apps, "vertical", bottom_edge)

local activities_button = main.panel:get_status_area_indicator ("activities")
test.log ("Checking that top bar extends to top screen edge")
check_fittsiness ("fittsyTopBarVert", activities_button, "vertical", top_edge)

main.panel.text_direction = "ltr"
test.sleep (50)
test.wait_leisure ()

test.log ("Checking that top bar extends to left screen edge for LTR")
check_fittsiness ("fittsyTopBarHorzLTR", activities_button, "horizontal", left_edge)

main.panel.text_direction = "rtl"
test.sleep (50)
test.wait_leisure ()

test.log ("Checking that top bar extends to right screen edge for RTL")
check_fittsiness ("fittsyTopBarHorzRTL", activities_button, "horizontal", right_edge)
