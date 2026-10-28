-- Checks that the ui group and the overview group fill the stage after
-- monitors were added and removed.

local layout_manager = main.layout_manager

local function check_allocation (actor)
  local box = actor:get_allocation_box ()

  if box.width ~= stage.width or box.height ~= stage.height then
    error (string.format ("%s allocation is %sx%s, expected %sx%s",
                          actor.name, box.width, box.height,
                          stage.width, stage.height))
  end
end

local function change_monitors (change)
  main.overview:hide ()
  test.wait_leisure ()

  local manager = global.backend:get_monitor_manager ()
  local monitors_changed = test.watch (manager, "monitors-changed")
  change ()
  test.wait (monitors_changed)

  test.wait_leisure ()
  main.overview:show ()
  test.wait_leisure ()
end

test.wait_startup ()

main.overview:show ()
test.wait_leisure ()
check_allocation (layout_manager.ui_group)
check_allocation (layout_manager.overview_group)

local monitor = nil
local ok, err = pcall (function ()
  change_monitors (function ()
    monitor = test.new_monitor (1920, 1080, 60.0)
  end)
  check_allocation (layout_manager.ui_group)
  check_allocation (layout_manager.overview_group)

  change_monitors (function ()
    monitor:destroy ()
    monitor = nil
  end)
  check_allocation (layout_manager.ui_group)
  check_allocation (layout_manager.overview_group)
end)

if monitor then
  monitor:destroy ()
end

if not ok then
  error (err, 0)
end
