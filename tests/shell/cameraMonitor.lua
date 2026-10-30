-- This script tests the camera monitor (src/shell-camera-monitor.c), behind
-- the camera privacy indicator, under fatal warnings. A fake camera is a
-- GStreamer test pattern that a PipeWire video source with the media role
-- Camera provides; a camera is in use while that node runs, which is while a
-- consumer reads it (WirePlumber links the two: it needs the media class of
-- both, and the media type of the consumer). The camera is used twice: the
-- monitor reports it in use, not in use once the delayed disable ran, and in
-- use again.

local TIMEOUT = 10000
local CAMERA = "shell-test-camera"

local results = {
  first_use = false,
  first_release = false,
  second_use = false,
  second_release = false,
}

local processes = {}

test.finish (function ()
  for _, process in ipairs (processes) do
    process:destroy ()
  end

  assert (results.first_use, "The camera was not reported in use")
  assert (results.first_release, "The camera was not reported released")
  assert (results.second_use, "The camera was not reported in use the second time")
  assert (results.second_release, "The camera was not reported released the second time")
end)

-- Starts a GStreamer pipeline; returns a function that stops it and waits
-- until it has exited
local function start_pipeline (description)
  local result
  local process, err = shell.spawn {
    argv = { "gst-launch-1.0", "-q", table.unpack (description) },
    on_exit = function (r) result = r end,
  }
  assert (process, err)
  table.insert (processes, process)

  return function ()
    process:kill ()
    test.wait_for (function () return result ~= nil end,
                   "gst-launch-1.0 to exit", TIMEOUT)
  end
end

local function wait_in_use (monitor, in_use, description)
  test.wait_for (function ()
    return monitor.cameras_in_use == in_use
  end, description, TIMEOUT)
  return true
end

test.wait_startup ()

test.log ("Running camera monitor test")

local monitor = test.new ("ShellCameraMonitor", {})
assert (not monitor.cameras_in_use, "A camera is in use before the test starts one")

test.log ("Providing the camera")
start_pipeline {
  "videotestsrc", "is-live=true", "!",
  "video/x-raw,width=64,height=48,framerate=10/1", "!",
  "pipewiresink", "mode=provide",
  "stream-properties=props,media.role=Camera,media.class=Video/Source,node.name=" .. CAMERA,
}

for round = 1, 2 do
  local suffix = round == 1 and "" or " again"

  test.log ("Using the camera" .. suffix)
  local stop_consumer = start_pipeline {
    "pipewiresrc", "target-object=" .. CAMERA,
    "stream-properties=props,media.class=Stream/Input/Video,media.type=Video",
    "!", "fakesink",
  }
  results[round == 1 and "first_use" or "second_use"] =
    wait_in_use (monitor, true, "the camera to be in use" .. suffix)

  test.log ("Releasing the camera" .. suffix)
  stop_consumer ()
  results[round == 1 and "first_release" or "second_release"] =
    wait_in_use (monitor, false, "the camera to be released" .. suffix)
end
