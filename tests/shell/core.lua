-- This performance script measure the most important (core) performance
-- metrics for the shell. By looking at the output metrics of this script
-- someone should be able to get an idea of how well the shell is performing
-- on a particular system.

local overview = main.overview

local METRICS = test.metrics {
  overviewLatencyFirst = {
    description = "Time to first frame after triggering overview, first time",
    units = "us",
  },
  overviewFpsFirst = {
    description = "Frame rate when going to the overview, first time",
    units = "frames / s",
  },
  overviewLatencySubsequent = {
    description = "Time to first frame after triggering overview, second time",
    units = "us",
  },
  overviewFpsSubsequent = {
    description = "Frames rate when going to the overview, second time",
    units = "frames / s",
  },
  overviewFps5Windows = {
    description = "Frames rate when going to the overview, 5 windows open",
    units = "frames / s",
  },
  overviewFps10Windows = {
    description = "Frames rate when going to the overview, 10 windows open",
    units = "frames / s",
  },
  overviewFps5Maximized = {
    description = "Frames rate when going to the overview, 5 maximized windows open",
    units = "frames / s",
  },
  overviewFps10Maximized = {
    description = "Frames rate when going to the overview, 10 maximized windows open",
    units = "frames / s",
  },
  overviewFps5Alpha = {
    description = "Frames rate when going to the overview, 5 alpha-transparent windows open",
    units = "frames / s",
  },
  overviewFps10Alpha = {
    description = "Frames rate when going to the overview, 10 alpha-transparent windows open",
    units = "frames / s",
  },
  usedAfterOverview = {
    description = "Malloc'ed bytes after the overview is shown once",
    units = "B",
  },
  leakedAfterOverview = {
    description = "Additional malloc'ed bytes the second time the overview is shown",
    units = "B",
  },
  applicationsShowTimeFirst = {
    description = "Time to switch to applications view, first time",
    units = "us",
  },
  applicationsShowTimeSubsequent = {
    description = "Time to switch to applications view, second time",
    units = "us",
  },
}

local WINDOW_CONFIGS = {
  { width = 640, height = 480, alpha = false, maximized = false, count = 1,  metric = "overviewFpsSubsequent" },
  { width = 640, height = 480, alpha = false, maximized = false, count = 5,  metric = "overviewFps5Windows" },
  { width = 640, height = 480, alpha = false, maximized = false, count = 10, metric = "overviewFps10Windows" },
  { width = 640, height = 480, alpha = false, maximized = true,  count = 5,  metric = "overviewFps5Maximized" },
  { width = 640, height = 480, alpha = false, maximized = true,  count = 10, metric = "overviewFps10Maximized" },
  { width = 640, height = 480, alpha = true,  maximized = false, count = 5,  metric = "overviewFps5Alpha" },
  { width = 640, height = 480, alpha = true,  maximized = false, count = 10, metric = "overviewFps10Alpha" },
}

local showing_overview = false
local finished_showing_overview = false
local overview_show_start = 0
local overview_frames = 0
local overview_latency = 0
local malloc_used_size = 0
local overview_show_count = 0
local have_swap_complete = false
local applications_show_start = 0
local applications_show_count = 0

test.on_script_event ("overviewShowStart", function (time)
  showing_overview = true
  finished_showing_overview = false
  overview_show_start = time
  overview_frames = 0
end)

test.on_script_event ("overviewShowDone", function (_time)
  -- We've set up the state at the end of the zoom out, but we
  -- need to wait for one more frame to paint before we count
  -- ourselves as done.
  finished_showing_overview = true
end)

test.on_script_event ("applicationsShowStart", function (time)
  applications_show_start = time
end)

test.on_script_event ("applicationsShowDone", function (time)
  applications_show_count = applications_show_count + 1
  if applications_show_count == 1 then
    METRICS.applicationsShowTimeFirst.value = time - applications_show_start
  else
    METRICS.applicationsShowTimeSubsequent.value = time - applications_show_start
  end
end)

test.on_script_event ("afterShowHide", function (_time)
  if overview_show_count == 1 then
    METRICS.usedAfterOverview.value = malloc_used_size
  else
    METRICS.leakedAfterOverview.value = malloc_used_size - METRICS.usedAfterOverview.value
  end
end)

test.on_event ("malloc.usedSize", function (_time, bytes)
  malloc_used_size = bytes
end)

local function frame_done (time)
  if showing_overview then
    if overview_frames == 0 then
      overview_latency = time - overview_show_start
    end

    overview_frames = overview_frames + 1
  end

  if finished_showing_overview then
    showing_overview = false
    finished_showing_overview = false
    overview_show_count = overview_show_count + 1

    local dt = (time - (overview_show_start + overview_latency)) / 1000000

    -- If we see a start frame and an end frame, that would
    -- be 1 frame for a FPS computation, hence the '- 1'
    local fps = (overview_frames - 1) / dt

    if overview_show_count == 1 then
      METRICS.overviewLatencyFirst.value = overview_latency
      METRICS.overviewFpsFirst.value = fps
    elseif overview_show_count == 2 then
      METRICS.overviewLatencySubsequent.value = overview_latency
    end

    -- Other than overviewFpsFirst, we collect FPS metrics the second
    -- we show each window configuration. overview_show_count is 1,2,3...
    if overview_show_count % 2 == 0 then
      local config = WINDOW_CONFIGS[overview_show_count // 2]
      METRICS[config.metric].value = fps
    end
  end
end

test.on_event ("clutter.stagePaintDone", function (time)
  -- If we aren't receiving GLXBufferSwapComplete events, then we approximate
  -- the time the user sees a frame with the time we finished doing drawing
  -- commands for the frame. This doesn't take into account the time for
  -- the GPU to finish painting, and the time for waiting for the buffer
  -- swap, but if this are uniform - every frame takes the same time to draw -
  -- then it won't upset our FPS calculation, though the latency value
  -- will be slightly too low.

  if not have_swap_complete then
    frame_done (time)
  end
end)

test.wait_startup ()

test.define_event ("overviewShowStart", "Starting to show the overview")
test.define_event ("overviewShowDone", "Overview finished showing")
test.define_event ("afterShowHide", "After a show/hide cycle for the overview")
test.define_event ("applicationsShowStart", "Starting to switch to applications view")
test.define_event ("applicationsShowDone", "Done switching to applications view")

-- Enable recording of timestamps for different points in the frame cycle
global.frame_timestamps = true

overview:connect ("shown", function ()
  test.event ("overviewShowDone")
end)

test.sleep (1000)

for i = 0, 2 * #WINDOW_CONFIGS - 1 do
  -- We go to the overview twice for each configuration; the first time
  -- to calculate the mipmaps for the windows, the second time to get
  -- a clean set of numbers.
  if i % 2 == 0 then
    local config = WINDOW_CONFIGS[i // 2 + 1]
    test.destroy_windows ()

    for _ = 1, config.count do
      test.create_window {
        width = config.width,
        height = config.height,
        alpha = config.alpha,
        maximized = config.maximized,
      }
    end

    test.wait_windows ()
    test.sleep (1000)
    test.wait_leisure ()
  end

  test.event ("overviewShowStart")
  overview:show ()

  test.wait_leisure ()
  overview:hide ()
  test.wait_leisure ()

  collectgarbage ()
  test.sleep (1000)
  test.collect_statistics ()
  test.event ("afterShowHide")
end

test.destroy_windows ()
test.sleep (1000)

overview:show ()
test.wait_leisure ()

for _ = 1, 2 do
  test.event ("applicationsShowStart")
  overview.dash.show_apps_button.checked = true
  test.wait_leisure ()
  test.event ("applicationsShowDone")
  overview.dash.show_apps_button.checked = false
  test.wait_leisure ()
end
