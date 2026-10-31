-- Conformance test of the capabilities of docs/api/README.md ("Capabilities"):
-- see tests/api/README.md. Every gated function, with and without the
-- capability that unlocks it: the fixture tests/api/fixtures/capabilities is
-- installed nine times, as capabilities-<variant>@test with the same code and
-- the variant's capabilities in metadata.json (none, each one alone, all seven).
-- Each copy calls every gated function and checks that it works when its
-- capability is declared and raises the capability error when it is not, also
-- when other capabilities are. The per-area tests (dbus, fs, search, spawn,
-- network, core) test the functions themselves; this one tests the gates.
-- Afterwards the script checks from the outside that the refused calls did
-- nothing: the server got the requests of the two variants with "network"
-- only, and no file is left.

local harness = dofile (((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local VARIANTS = { "none", "dbus", "settings", "search", "fs", "spawn", "network", "worker", "all" }
local CYCLES = 2

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

test.wait_startup ()

local server = test.http_server {
  routes = {
    ["/matrix"] = { body = "matrix" },
  },
}

for _, variant in ipairs (VARIANTS) do
  harness.run {
    uuid = "capabilities-" .. variant .. "@test",
    cycles = CYCLES,
    timeout = 30000,

    drive = function (c)
      c.wait_done ("main")
      c.step ("matrix", { url = server.url })
    end,
  }
end

-- Only the variants with "network" sent requests, one per enable
local requests = server:requests ()
local wanted = 2 * CYCLES

if #requests ~= wanted then
  fail ("the server got %d requests, expected %d (from capabilities-network@test and capabilities-all@test)",
        #requests, wanted)
end
for _, request in ipairs (requests) do
  if request.method ~= "GET" or request.path ~= "/matrix" then
    fail ("unexpected request %s %s", request.method, request.path)
  end
end

-- Nothing written is left, outside or in the extension's own directory: the
-- variants with "fs" removed what they wrote, the others wrote nothing
local data_home = os.getenv ("XDG_DATA_HOME")

if not data_home then
  fail ("XDG_DATA_HOME is not set")
end
for _, variant in ipairs (VARIANTS) do
  local uuid = "capabilities-" .. variant .. "@test"

  for _, path in ipairs {
    data_home .. "/gnome-lua/" .. uuid .. ".outside",
    data_home .. "/gnome-shell/extensions/" .. uuid .. "/written.txt",
  } do
    if shell.fs.exists (path) then
      fail ("%s left %s", uuid, path)
    end
  end
end
