-- Conformance test of README calling convention 1 (docs/api/README.md):
-- option tables, and the tables in them that the API reads, are plain
-- tables read raw, and text is valid UTF-8. The checks are in the fixture
-- tests/api/fixtures/arguments@test, which runs them all when it is
-- enabled; see tests/api/README.md. Then utf8-error@test fails in enable ()
-- with a message that is not UTF-8, which the shell must still be able to
-- send over D-Bus.

local harness = dofile (((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local TIMEOUT = 10000

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

local function gdbus (...)
  local result
  local proc, err = shell.spawn {
    argv = { "gdbus", "call", "--session", "--timeout", "10",
             "--dest", "org.gnome.Shell", "--object-path", "/org/gnome/Shell", "--method", ... },
    on_exit = function (r) result = r end,
  }
  assert (proc, err)
  test.wait_for (function () return result ~= nil end, "gdbus", TIMEOUT)
  return result
end

harness.run {
  uuid = "arguments@test",
  timeout = 30000,

  drive = function (c)
    c.wait_done ("main")
  end,
}

-- The error is logged as a warning, which the test shell would take as fatal
test.log_capture ("utf8-error@test", true)
test.extension_enable ("utf8-error@test")
test.wait_for (function ()
  local state = test.extension_state ("utf8-error@test")
  return state ~= nil and state.state == "error"
end, "utf8-error@test to fail", TIMEOUT)

for _, method in ipairs { "org.gnome.Shell.Extensions.ListExtensions",
                          "org.gnome.Shell.Extensions.GetExtensionInfo",
                          "org.gnome.Shell.Extensions.GetExtensionErrors" } do
  local r = method:find ("ListExtensions") and gdbus (method) or gdbus (method, "utf8-error@test")
  if r.status ~= 0 then
    fail ("%s failed: %s", method, r.stderr)
  end
  if not r.stdout:find ("bad \u{FFFD} byte", 1, true) then
    fail ("%s does not carry the repaired message: %s", method, r.stdout)
  end
end
test.extension_disable ("utf8-error@test")
