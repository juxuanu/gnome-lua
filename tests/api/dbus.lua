-- Conformance test of docs/api/dbus.md and docs/api/variant.md: see
-- tests/api/README.md. The assertions of the API are in the fixtures
-- tests/api/fixtures/dbus@test (everything the session bus of the test can
-- answer, and the name and object of the extension itself) and dbus-nocap@test
-- (nothing works without the capability), run by the extension manager of the
-- shell; this script asks for the part that causes errors on purpose, and
-- checks from the outside what the extension left on the bus.

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

test.wait_startup ()

-- Without "dbus" in the capabilities every function of shell.dbus raises
harness.run { uuid = "dbus-nocap@test" }

-- This script is a privileged state: shell.dbus is open to it, which makes it
-- a client of the bus for the checks from the outside
local bus = shell.dbus.session ()
local daemon = bus:proxy {
  name = "org.freedesktop.DBus", path = "/org/freedesktop/DBus", interface = "org.freedesktop.DBus",
}

local function name_has_owner (c, name)
  local owned

  daemon:call {
    method = "NameHasOwner", signature = "s", args = { name },
    on_reply = function (has) owned = has end,
    on_error = function (err) fail ("NameHasOwner (%s): %s", name, err.name) end,
  }
  c.wait_until (function () return owned ~= nil end, "NameHasOwner (" .. name .. ")")
  return owned
end

local function wait_name_released (c, name)
  local deadline = test.now () + 10000

  while name_has_owner (c, name) do
    if test.now () > deadline then
      fail ("%s is still owned 10 s after the extension was disabled", name)
    end
    test.sleep (100)
  end
end

harness.run {
  uuid = "dbus@test",
  timeout = 30000,
  -- the extension causes these two on purpose, in the step below
  expect_errors = 2,

  drive = function (c)
    c.wait_done ("main")

    -- the name is held and the object answers while the extension is enabled
    if not name_has_owner (c, "org.example.ApiTest") then
      fail ("org.example.ApiTest has no owner while the extension is enabled")
    end

    -- a Lua error and a wrong return count in a method of an exported object:
    -- the peer gets org.freedesktop.DBus.Error.Failed (checked by the fixture),
    -- the log gets the errors with the traceback
    local log = c.capture_log (function ()
      c.step ("method-errors")
    end)

    local tracebacks, count_errors = 0, 0
    for _, entry in ipairs (log) do
      if entry.message:find ("secret-traceback-marker", 1, true) and entry.message:find ("traceback", 1, true) then
        tracebacks = tracebacks + 1
      end
      if entry.message:find ("returned 2 values", 1, true) then
        count_errors = count_errors + 1
      end
    end
    if tracebacks ~= 1 or count_errors ~= 1 then
      fail ("the log should have the two errors of the methods, once each (%d, %d)", tracebacks, count_errors)
    end
  end,

  after_disable = function (c)
    -- the name is released and the object is gone with the extension
    wait_name_released (c, "org.example.ApiTest")
    wait_name_released (c, "org.example.ApiTest.Errors")
  end,
}
