-- This script tests the box pointer (src/ui/box-pointer.c), the popup of
-- menus that points at a source actor, under fatal warnings. The window menu
-- shows its source, a widget of the UI group moved to the click, right
-- before the menu. The UI group is a no-layout actor: Clutter allocates each
-- of its children as a root of its own, the last queued first, so when
-- nothing relays out between the two the menu is allocated before its source,
-- which has never been allocated yet (an infinite box). The box pointer must
-- then wait for the source, not take a NaN position from it.

local ui_group = main.layout_manager.ui_group

local results = {
  positioned = false,
}

test.finish (function ()
  assert (results.positioned, "The box pointer was not placed below its source")
end)

test.wait_startup ()

test.log ("Running box pointer test")

local source = test.new ("StWidget", { visible = false, reactive = true })
ui_group:add_child (source)
local box_pointer = test.new ("ShellBoxPointer", { visible = false })
ui_group:add_child (box_pointer)
test.wait_and_draw (200)

-- As ShellWindowMenuManager does, in one turn of the main loop
test.log ("Showing the source and the box pointer")
source.x = 300
source.y = 200
source.width = 1
source.height = 1
source.visible = true
box_pointer:set_position (source, 0)
box_pointer.visible = true

test.wait_and_draw (500)

-- The arrow is on the top side: the box is below the source
local box = box_pointer:get_allocation_box ()
test.log (string.format ("Box pointer at %g,%g", box.x, box.y))
results.positioned = box.x == box.x and box.y >= 201 and box.y < 300
