-- This script tests the most important (basic) functionality of the shell.

local overview = main.overview
local panel = main.panel

local top_bar_nav = false
local notification_shown = false
local notification_closed = false
local window_picker_shown = false
local app_picker_shown = false

test.on_script_event ("topBarNavDone", function ()
  top_bar_nav = true
end)

test.on_script_event ("notificationShowDone", function ()
  notification_shown = true
end)

test.on_script_event ("notificationCloseDone", function ()
  notification_closed = true
end)

test.on_script_event ("overviewShowDone", function ()
  window_picker_shown = true
end)

test.on_script_event ("applicationsShowDone", function ()
  app_picker_shown = true
end)

test.finish (function ()
  assert (top_bar_nav, "Failed to navigate top bar")
  assert (notification_shown, "Failed to show notification")
  assert (notification_closed, "Failed to close notification")
  assert (window_picker_shown, "Failed to show window picker")
  assert (app_picker_shown, "Failed to show app picker")
end)

test.wait_startup ()

test.log ("Running basic perf test")

test.define_event ("topBarNavStart", "Starting to navigate the top bar")
test.define_event ("topBarNavDone", "Done navigating the top bar")
test.define_event ("notificationShowStart", "Showing a notification")
test.define_event ("notificationShowDone", "Done showing a notification")
test.define_event ("notificationCloseStart", "Closing a notification")
test.define_event ("notificationCloseDone", "Done closing a notification")
test.define_event ("overviewShowStart", "Starting to show the overview")
test.define_event ("overviewShowDone", "Overview finished showing")
test.define_event ("applicationsShowStart", "Starting to switch to applications view")
test.define_event ("applicationsShowDone", "Done switching to applications view")

overview:connect ("shown", function ()
  test.event ("overviewShowDone")
end)

test.sleep (1000)

-- navigate through top bar

test.log ("Navigate through top bar")
test.event ("topBarNavStart")
panel:get_status_area_indicator ("quickSettings").menu:open ()
test.sleep (400)

local menu_manager = panel.menu_manager
while menu_manager.active_menu and
      panel:navigate_focus (menu_manager.active_menu.source_actor,
                            "tab-backward", false) do
  test.sleep (400)
end
test.event ("topBarNavDone")

test.sleep (1000)

-- notification
test.log ("Show notification message")
local source = main.message_tray:get_system_source ()

test.event ("notificationShowStart")
source:connect ("notification-request-banner", function ()
  test.event ("notificationShowDone")
end)

local notification = test.new ("ShellNotification", {
  source = source,
  title = "A test notification",
})
source:add_notification (notification)
test.sleep (400)

test.log ("Show date menu")
panel:get_status_area_indicator ("dateMenu").menu:open ()
test.sleep (400)

test.event ("notificationCloseStart")
notification:connect ("destroy", function ()
  test.event ("notificationCloseDone")
end)

test.log ("Destroy notification message")
notification:destroy ()
test.sleep (400)

test.log ("Close date menu")
panel:get_status_area_indicator ("dateMenu").menu:close ()
test.wait_leisure ()

test.sleep (1000)

-- overview (window picker)
test.event ("overviewShowStart")
overview:show ()
test.wait_leisure ()
overview:hide ()
test.wait_leisure ()

test.sleep (1000)

-- overview (app picker)
test.log ("Showing overview")
overview:show ()
test.wait_leisure ()

test.event ("applicationsShowStart")
test.log ("Showing applications")
overview.dash.show_apps_button.checked = true
test.wait_leisure ()
test.event ("applicationsShowDone")
test.log ("Hiding applications")
overview.dash.show_apps_button.checked = false
test.wait_leisure ()

test.log ("Hiding overview")
overview:hide ()
test.wait_leisure ()

test.log ("Finished basic perf test")
