-- Adds a monitor and checks that the shell (the display, the stage and the
-- layout manager) agrees on the layout of the monitors.

local display = global.display
local layout_manager = main.layout_manager

test.wait_startup ()

local monitors_changed = test.watch (layout_manager, "monitors-changed")
-- The monitor stays until the shell exits
test.new_monitor (1280, 720, 60.0)
test.wait (monitors_changed)

local expect_equal = test.expect_equal
local monitors = {}

expect_equal (display:get_n_monitors (), 2, "Configured monitors")

for i = 0, display:get_n_monitors () - 1 do
  monitors[#monitors + 1] = display:get_monitor_geometry (i)
end

local expected_screen_width = 0
local expected_screen_height = 0
for _, m in ipairs (monitors) do
  expected_screen_width = expected_screen_width + m.width
  expected_screen_height = math.max (expected_screen_height, m.height)
end
print (string.format ("Expecting screen to have size of: %dx%d",
                      expected_screen_width, expected_screen_height))

expect_equal (global.screen_width, expected_screen_width, "Screen Width")
expect_equal (global.screen_height, expected_screen_height, "Screen Height")

expect_equal (stage.width, global.screen_width, "Stage Width")
expect_equal (stage.height, global.screen_height, "Stage Height")

expect_equal (layout_manager.n_monitors, #monitors, "Monitors length")
expect_equal (layout_manager:get_primary_monitor ().index,
              layout_manager:get_monitor (0).index, "Primary Monitor")
expect_equal (layout_manager:get_n_background_managers (), #monitors,
              "Background Managers length")

for i, geometry in ipairs (monitors) do
  local m = layout_manager:get_monitor (i - 1)
  expect_equal (m.index, i - 1, "Monitor " .. (i - 1) .. " index")
  expect_equal (m.x, geometry.x, "Monitor " .. (i - 1) .. " x")
  expect_equal (m.y, geometry.y, "Monitor " .. (i - 1) .. " y")
  expect_equal (m.width, geometry.width, "Monitor " .. (i - 1) .. " width")
  expect_equal (m.height, geometry.height, "Monitor " .. (i - 1) .. " height")
end
