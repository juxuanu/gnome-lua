-- This script tests the application switcher (src/ui/alt-tab.c,
-- src/ui/switcher-popup.c) under fatal warnings: with two windows of one
-- application, Alt+Tab opens it, which allocates and so repaints the arrows
-- it draws (the scroll arrows of the list, the arrow below an application
-- with several windows).

local TIMEOUT = 10000

local overview = main.overview

local results = {
  switcher_shown = false,
}

test.finish (function ()
  assert (results.switcher_shown, "Alt+Tab did not show the application switcher")
end)

test.wait_startup ()

test.log ("Running switcher test")

-- The session starts with the overview shown (the startup animation leaves
-- it visible): hide it first
if overview.visible then
  local hidden = test.watch (overview, "hidden")
  overview:hide ()
  test.wait (hidden, TIMEOUT)
end

test.create_window { width = 640, height = 480 }
test.create_window { width = 640, height = 480 }
test.wait_windows ()
test.wait_leisure ()

local shown = {}
main.layout_manager.ui_group:connect ("child-added", function (_, child)
  shown[child:gtype ()] = true
end)

test.log ("Pressing Alt+Tab")
test.press_accelerator ("<Alt>Tab")
test.wait_for (function ()
  return shown.ShellAppSwitcherPopup
end, "the application switcher", TIMEOUT)
results.switcher_shown = true
test.sleep (500)
test.wait_leisure ()

test.destroy_windows ()
