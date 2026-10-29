-- Conformance test of docs/api/spawn.md: see tests/api/README.md. The
-- assertions of the API are in the fixtures tests/api/fixtures/spawn@test
-- (everything /bin/sh and coreutils can show) and spawn-nocap@test (nothing
-- runs without the capability), run by the extension manager of the shell;
-- this script checks from the outside that the process the extension left
-- running is gone when it is disabled.

local harness = dofile (((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

test.wait_startup ()

-- Without "spawn" in the capabilities shell.spawn raises
harness.run { uuid = "spawn-nocap@test" }

-- This script is a privileged state: shell.spawn is open to it. Is the
-- process with this pid still there? (kill -0 also succeeds for a zombie, which
-- is what an unreaped child would be.)
local function process_exists (c, pid)
  local result

  local proc, err = shell.spawn {
    argv = { "/bin/sh", "-c", "kill -0 " .. pid .. " 2>/dev/null && ! grep -q '^State:.*Z' /proc/" .. pid .. "/status 2>/dev/null" },
    on_exit = function (r) result = r end,
  }
  if not proc then
    fail ("cannot run /bin/sh: %s", err)
  end
  c.wait_until (function () return result ~= nil end, "kill -0 " .. pid)
  return result.status == 0
end

local function sleeper_pid (uuid)
  for _, check in ipairs (test.extension_report (uuid).checks) do
    if check.name == "sleeper-pid" then
      return tonumber (check.message)
    end
  end
  fail ("%s did not report the pid of its sleeper", uuid)
end

harness.run {
  uuid = "spawn@test",
  timeout = 30000,

  after_disable = function (c)
    local pid = sleeper_pid ("spawn@test")
    local deadline = test.now () + 10000

    -- killed when the extension was disabled (SIGTERM, SIGKILL after 2 s)
    while process_exists (c, pid) do
      if test.now () > deadline then
        fail ("the process the extension left running (%d) is still there 10 s after it was disabled", pid)
      end
      test.sleep (100)
    end
  end,
}
