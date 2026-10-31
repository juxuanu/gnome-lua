-- This script tests drag and drop (src/ui/dnd.c) with the pointer, under
-- fatal warnings: it drags an application of the grid onto another, which
-- makes a folder of the two. The drag destroys the actor it dragged when the
-- drop is accepted; the ui group, where that actor was the last child, still
-- holds it until its idle update of the :last-child style. That actor used to
-- lose a reference too many, which freed it under the ui group: meson runs
-- this script with glibc's malloc filling freed memory, which turns the
-- touch of the freed actor into a crash.

local TIMEOUT = 10000
local APPS = { "00-drag-source", "00-drag-target" }

local overview = main.overview
local ui_group = main.layout_manager.ui_group

local results = {
  apps_installed = false,
  icons_found = false,
  drag_started = false,
  folder_made = false,
}

test.finish (function ()
  assert (results.apps_installed, "The applications to drag were not found")
  assert (results.icons_found, "The icons of the applications are not shown in the grid")
  assert (results.drag_started, "Pressing and moving the pointer did not start a drag")
  assert (results.folder_made, "Dropping the application on the other did not make a folder")
end)

-- Installs the applications in the test home
local function install_apps (names)
  local dir = os.getenv ("XDG_DATA_HOME") .. "/applications"
  local app_system = global:get_app_system ()

  assert (os.execute (string.format ("mkdir -p '%s'", dir)))
  for _, name in ipairs (names) do
    local file = assert (io.open (string.format ("%s/%s.desktop", dir, name), "w"))
    file:write ("[Desktop Entry]\nType=Application\nName=" .. name .. "\nExec=true\n")
    file:close ()
  end

  test.wait_for (function ()
    for _, name in ipairs (names) do
      if app_system:lookup_app (name .. ".desktop") == nil then
        return false
      end
    end
    return true
  end, "the applications to drag", TIMEOUT)
  return true
end

-- The mapped icon of the grid for the application @id, or nil
local function find_icon (actor, id)
  for _, child in ipairs (actor:get_children ()) do
    if child:gtype () == "ShellAppIcon" and child.id == id and child.mapped then
      return child
    end

    local found = find_icon (child, id)
    if found ~= nil then
      return found
    end
  end
  return nil
end

local function center (actor)
  local x, y = actor:get_transformed_position ()
  local box = actor:get_allocation_box ()

  return x + box.width / 2, y + box.height / 2
end

-- Moves the pointer to (@x, @y) in @steps motions, one per frame or so
local function move_pointer (from_x, from_y, x, y, steps)
  for i = 1, steps do
    test.pointer_move (from_x + (x - from_x) * i / steps, from_y + (y - from_y) * i / steps)
    test.sleep (20)
  end
end

test.wait_startup ()

test.log ("Running drag and drop test")

results.apps_installed = install_apps (APPS)

-- The virtual pointer starts at 0, 0: in the hot corner, which toggles the
-- overview. Make it and move it away before the overview is shown.
test.pointer_move (stage.width / 2, stage.height / 2)
test.sleep (1000)
test.wait_leisure ()

-- The session starts with the overview shown (the startup animation leaves
-- it visible): hide it first
if overview.visible then
  local hidden_at_start = test.watch (overview, "hidden")
  overview:hide ()
  test.wait (hidden_at_start, TIMEOUT)
  test.wait_leisure ()
end

test.log ("Showing the application grid")
local shown = test.watch (overview, "shown")
overview:show_for_reason ("dnd-test", "app-grid")
test.wait (shown, TIMEOUT)
test.sleep (600)
test.wait_leisure ()

local source, target
test.wait_for (function ()
  source = find_icon (stage, APPS[1] .. ".desktop")
  target = find_icon (stage, APPS[2] .. ".desktop")
  return source ~= nil and target ~= nil
end, "the icons of the applications", TIMEOUT)

local source_x, source_y = center (source)
local target_x, target_y = center (target)
local stage_width, stage_height = stage.width, stage.height
for _, point in ipairs { { source_x, source_y }, { target_x, target_y } } do
  assert (point[1] > 0 and point[1] < stage_width and point[2] > 0 and point[2] < stage_height,
          "an icon is off screen (another page of the grid?)")
end
results.icons_found = true

-- The drag actor: a new actor that the drag adds to the ui group
ui_group:connect ("child-added", function (_, child)
  if child:gtype () == "StIcon" then
    results.drag_started = true
  end
end)

local source_destroyed = test.watch (source, "destroy")
local target_destroyed = test.watch (target, "destroy")

test.log (string.format ("Dragging %s onto %s", APPS[1], APPS[2]))
test.pointer_move (source_x, source_y)
test.sleep (100)
test.pointer_press ()
test.sleep (100)
move_pointer (source_x, source_y, target_x, target_y, 20)

-- Hover a little, as a hand would, before letting go
move_pointer (target_x, target_y, target_x + 2, target_y + 2, 10)
test.sleep (300)
test.pointer_release ()

-- The grid takes the two applications out to make the folder
test.wait (source_destroyed, TIMEOUT)
test.wait (target_destroyed, TIMEOUT)
results.folder_made = true

-- The idle update of the styles of the ui group's children runs
test.sleep (500)
test.wait_leisure ()

local hidden = test.watch (overview, "hidden")
overview:hide ()
test.wait (hidden, TIMEOUT)
test.wait_leisure ()

test.log ("Finished drag and drop test")
