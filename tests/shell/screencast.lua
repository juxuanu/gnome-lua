-- This script tests screen recording: the screencast service
-- (src/dbus-services/screencast-service.c, org.gnome.Shell.Screencast)
-- records the screen through Mutter's ScreenCast API, PipeWire and
-- GStreamer. The test bus cannot activate the service, so the script starts
-- the one of the build; a client keeps one connection to it (the service
-- stops when its caller goes) to start a recording, wait and stop it. The
-- file must hold the video. A service that missed the stream of Mutter (its
-- PipeWireStreamAdded signal) used to time out while starting.

local TIMEOUT = 40000

local video = os.getenv ("XDG_CACHE_HOME") .. "/screencast-test.webm"

local results = {
  service_up = false,
  recorded = false,
  file_written = false,
}

test.finish (function ()
  assert (results.service_up, "The screencast service did not start")
  assert (results.recorded, "The recording did not start and stop")
  assert (results.file_written, "The recording left no video")
end)

local function run (argv)
  local result
  local proc, err = shell.spawn { argv = argv, on_exit = function (r) result = r end }
  assert (proc, err)
  return proc, function () return result end
end

test.wait_startup ()

test.log ("Running screencast test")

local service = run { os.getenv ("GNOME_SHELL_BUILDDIR") .. "/dbus-services/org.gnome.Shell.Screencast" }
local _, up = run { "gdbus", "wait", "--session", "--timeout", "20", "org.gnome.Shell.Screencast" }
test.wait_for (up, "the screencast service to own its name", TIMEOUT)
assert (up ().status == 0, "the screencast service did not own its name: " .. up ().stderr)
results.service_up = true

test.log ("Recording")
local _, client = run { "python3", "-c", [[
import sys, time, dbus
bus = dbus.SessionBus()
screencast = dbus.Interface(bus.get_object('org.gnome.Shell.Screencast', '/org/gnome/Shell/Screencast'),
                            'org.gnome.Shell.Screencast')
success, name = screencast.Screencast(sys.argv[1], dbus.Dictionary({}, signature='sv'), timeout=30)
print('started', bool(success), name, flush=True)
time.sleep(2)
print('stopped', bool(screencast.StopScreencast(timeout=30)), flush=True)
]], video }

-- Frames for the stream while the client records
local deadline = test.now () + TIMEOUT
while client () == nil and test.now () < deadline do
  test.wait_and_draw (100)
end
assert (client () ~= nil, "the recording client did not finish")
assert (client ().status == 0, "the recording client failed: " .. client ().stderr)
results.recorded = client ().stdout:find ("started True", 1, true) ~= nil
  and client ().stdout:find ("stopped True", 1, true) ~= nil
assert (results.recorded, "the recording client said: " .. client ().stdout)

local file = io.open (video, "rb")
if file ~= nil then
  results.file_written = file:seek ("end") > 0
  file:close ()
end

service:destroy ()

test.log ("Finished screencast test")
