-- Performance script for hardware testing: how long the shell takes to get to
-- the desktop, to show the overview and the application grid, to redraw its
-- views, and to start an application (gedit).

local overview = main.overview

local METRICS = test.metrics {
  timeToDesktop = {
    description = "Time from starting graphical.target to desktop showing",
    units = "us",
  },

  overviewShowTime = {
    description = "Time to switch to overview view, first time",
    units = "us",
  },

  applicationsShowTime = {
    description = "Time to switch to applications view, first time",
    units = "us",
  },

  mainViewRedrawTime = {
    description = "Time to redraw the main view, full screen",
    units = "us",
  },

  overviewRedrawTime = {
    description = "Time to redraw the overview, full screen, 5 windows",
    units = "us",
  },

  applicationRedrawTime = {
    description = "Time to redraw frame with a maximized application update",
    units = "us",
  },

  geditStartTime = {
    description = "Time from gedit launch to window drawn",
    units = "us",
  },
}

-- The monotonic time at which graphical.target was reached, from the journal
local function extract_boot_timestamp ()
  local pipe = assert (io.popen ("journalctl -b " ..
                                 "MESSAGE_ID=7d4958e842da4a758f6c1cdc7b36dcc5 " ..
                                 "UNIT=graphical.target -o json"))
  local result = nil

  for line in pipe:lines () do
    local timestamp = line:match ('"__MONOTONIC_TIMESTAMP"%s*:%s*"(%d+)"')
    if timestamp then
      result = tonumber (timestamp)
    end
  end
  pipe:close ()

  return result
end

local overview_show_start
local applications_show_start
local stage_paint_start
local redraw_timing
local redraw_times = {}
local gedit_launch_time

test.on_script_event ("desktopShown", function (time)
  local boot_timestamp = extract_boot_timestamp ()
  METRICS.timeToDesktop.value = time - boot_timestamp
end)

test.on_script_event ("overviewShowStart", function (time)
  overview_show_start = time
end)

test.on_script_event ("overviewShowDone", function (time)
  METRICS.overviewShowTime.value = time - overview_show_start
end)

test.on_script_event ("applicationsShowStart", function (time)
  applications_show_start = time
end)

test.on_script_event ("applicationsShowDone", function (time)
  METRICS.applicationsShowTime.value = time - applications_show_start
end)

test.on_script_event ("mainViewDrawStart", function (_time)
  redraw_timing = "mainView"
end)

test.on_script_event ("mainViewDrawDone", function (_time)
  redraw_timing = nil
end)

test.on_script_event ("overviewDrawStart", function (_time)
  redraw_timing = "overview"
end)

test.on_script_event ("overviewDrawDone", function (_time)
  redraw_timing = nil
end)

test.on_script_event ("redrawTestStart", function (_time)
  redraw_timing = "application"
end)

test.on_script_event ("redrawTestDone", function (_time)
  redraw_timing = nil
end)

test.on_script_event ("collectTimings", function (_time)
  for timing, times in pairs (redraw_times) do
    table.sort (times)

    local len = #times
    local median

    if len == 0 then
      median = -1
    elseif len % 2 == 1 then
      median = times[(len - 1) // 2 + 1]
    else
      median = math.floor ((times[len // 2] + times[len // 2 + 1]) / 2 + 0.5)
    end

    METRICS[timing .. "RedrawTime"].value = median
  end
end)

test.on_script_event ("geditLaunch", function (time)
  gedit_launch_time = time
end)

test.on_script_event ("geditFirstFrame", function (time)
  METRICS.geditStartTime.value = time - gedit_launch_time
end)

test.on_event ("clutter.stagePaintStart", function (time)
  stage_paint_start = time
end)

test.on_event ("clutter.paintCompletedTimestamp", function (time)
  if redraw_timing ~= nil and stage_paint_start ~= nil then
    redraw_times[redraw_timing] = redraw_times[redraw_timing] or {}
    table.insert (redraw_times[redraw_timing], time - stage_paint_start)
  end
  stage_paint_start = nil
end)

test.wait_startup ()

test.define_event ("desktopShown", "Finished initial animation")
test.define_event ("overviewShowStart", "Starting to show the overview")
test.define_event ("overviewShowDone", "Overview finished showing")
test.define_event ("applicationsShowStart", "Starting to switch to applications view")
test.define_event ("applicationsShowDone", "Done switching to applications view")
test.define_event ("mainViewDrawStart", "Drawing main view")
test.define_event ("mainViewDrawDone", "Ending timing main view drawing")
test.define_event ("overviewDrawStart", "Drawing overview")
test.define_event ("overviewDrawDone", "Ending timing overview drawing")
test.define_event ("redrawTestStart", "Drawing application window")
test.define_event ("redrawTestDone", "Ending timing application window drawing")
test.define_event ("collectTimings", "Accumulate frame timings from redraw tests")
test.define_event ("geditLaunch", "gedit application launch")
test.define_event ("geditFirstFrame", "first frame of gedit window drawn")

test.wait_leisure ()
test.event ("desktopShown")

test.set_boolean ("org.gnome.desktop.interface", "enable-animations", false)

test.event ("overviewShowStart")
overview:show ()
test.wait_leisure ()
test.event ("overviewShowDone")

test.sleep (1000)

test.event ("applicationsShowStart")
overview.dash.show_apps_button.checked = true

test.wait_leisure ()
test.event ("applicationsShowDone")

test.sleep (1000)

overview:hide ()
test.wait_leisure ()

-- --------------------- --
-- Tests of redraw speed --
-- --------------------- --

global.frame_timestamps = true
global.frame_finish_timestamp = true

for _ = 1, 5 do
  test.create_window { maximized = true }
end
test.wait_windows ()

test.sleep (1000)

test.event ("mainViewDrawStart")
test.wait_and_draw (1000)
test.event ("mainViewDrawDone")

overview:show ()
-- (the JS did not wait for the shell to be idle here either)

test.sleep (1500)

test.event ("overviewDrawStart")
test.wait_and_draw (1000)
test.event ("overviewDrawDone")

test.destroy_windows ()
overview:hide ()

test.create_window {
  maximized = true,
  redraws = true,
}
test.wait_windows ()

test.sleep (1000)

test.event ("redrawTestStart")
test.sleep (1000)
test.event ("redrawTestDone")

test.sleep (1000)
test.event ("collectTimings")

test.destroy_windows ()

global.frame_timestamps = false
global.frame_finish_timestamp = false

test.sleep (1000)

local app = global:get_app_system ():lookup_app ("org.gnome.gedit.desktop")

test.event ("geditLaunch")
app:activate ()

local windows = app:get_windows ()
assert (#windows == 0, "gedit was already running")

while #windows == 0 do
  test.wait_signal (global.display, "window-created")
  windows = app:get_windows ()
end

test.wait_signal (windows[1]:get_compositor_private (), "first-frame")
test.event ("geditFirstFrame")

test.sleep (1000)

windows[1]:delete (global:get_current_time ())

test.sleep (1000)

test.set_boolean ("org.gnome.desktop.interface", "enable-animations", true)
