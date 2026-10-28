-- Conformance test of docs/api/core.md (and README.md where it concerns the
-- core): see tests/api/README.md. The assertions are in the fixtures
-- tests/api/fixtures/core*@test, run by the extension manager of the shell;
-- this script drives them and checks what only the shell can see.

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

test.wait_startup ()

-- The inspector state of this script is not an extension
if shell.extension ~= nil then
  fail ("shell.extension should be nil outside an extension")
end
if pcall (shell.settings.extension) then
  fail ("shell.settings.extension () should raise outside an extension")
end
if type (shell.api_version) ~= "table" or shell.api_version.major ~= 1 then
  fail ("shell.api_version should be {major = 1, minor = n}")
end

-- What the loader does with the metadata (README.md: Versioning, Capabilities)
harness.expect_state ("core-v2@test", "out-of-date")
harness.expect_state ("core-future@test", "out-of-date")
harness.expect_idle ("core-modes@test")
harness.run { uuid = "core-minor@test", cycles = 1 }

-- An extension without capability and without settings
harness.run { uuid = "core-bare@test" }

-- Errors in callbacks: logged with a traceback, counted, the sixth disables
do
  local uuid = "core-errors@test"
  local c = harness.context (uuid, 1)

  test.extension_enable (uuid)
  harness.wait_state (uuid, { "active" })
  c.wait_done ("main")

  local log = c.capture_log (function ()
    c.step ("raise", { n = 5 })
    c.wait_until (function ()
      return test.extension_state (uuid).errors == 5
    end, "five errors in the callbacks of the extension")
    test.sleep (300)
    test.wait_leisure ()
    local st = test.extension_state (uuid)
    if st.state ~= "active" then
      fail ("%s is %s after five errors: %s", uuid, st.state, st.error or "")
    end

    c.step ("raise", { n = 1 })
    harness.wait_state (uuid, { "error" })
  end)

  local booms = 0
  for _, entry in ipairs (log) do
    if entry.message:find ("boom", 1, true) then
      booms = booms + 1
      if entry.level ~= "warning" then
        fail ("an error of a callback is logged as %s, expected a warning", entry.level)
      end
      if not entry.message:find ("stack traceback", 1, true) then
        fail ("an error of a callback is logged with a traceback: %s", entry.message)
      end
    end
  end
  if booms ~= 6 then
    fail ("%d errors of callbacks were logged, expected 6", booms)
  end

  local st = test.extension_state (uuid)
  if not (st.error or ""):find ("boom", 1, true) then
    fail ("the error of %s should say why: %s", uuid, st.error or "no error")
  end

  test.extension_disable (uuid)
  test.print (string.format ("%s: disabled with state error after 6 errors", uuid))
end

-- The extension with the whole core API: the steps of the fixture
local EXPECTED_LOG = {
  { "debug", "d 1 2.5" },
  { "info", "i x" },
  { "warning", "w y" },
  { "critical", 'e "z"' },
  { "info", "alias a" },
  { "info", "100% sure" },
  { "info", "no format arguments" },
  { "debug", "last" },
}

harness.run {
  uuid = "core@test",
  drive = function (c)
    -- shell.log: the domain is shell-lua/<uuid>, the levels are those of GLib
    -- (shell.log.error is a critical: an error level is fatal in GLib)
    local log = c.capture_log (function ()
      c.step ("log")
    end)
    if #log ~= #EXPECTED_LOG then
      fail ("the extension logged %d messages, expected %d", #log, #EXPECTED_LOG)
    end
    for i, want in ipairs (EXPECTED_LOG) do
      local got = log[i]
      local level_ok = got.level == want[1] or (want[1] == "critical" and got.level == "error")
      if not level_ok or got.message ~= want[2] then
        fail ("log message %d is %s %q, expected %s %q", i, got.level, got.message, want[1], want[2])
      end
    end

    -- shell.timer: callbacks, active, cancel, every, idle
    c.step ("timers")

    -- shell.Settings:on_changed: changes from outside the extension
    local schema, key = "org.gnome.desktop.interface", "show-battery-percentage"
    c.step ("watch-arm")
    test.set_boolean (schema, key, true)
    test.sleep (200)
    c.step ("watch-check-1", { value = true })
    test.set_boolean (schema, key, false)
    test.sleep (200)
    c.step ("watch-check-2", { value = false })
  end,
}
