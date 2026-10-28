-- tests/api/harness.lua: the common part of the conformance tests of the
-- extension API (docs/api, tests/api/README.md).
--
-- A test of an area is a script of the Lua test runner (src/shell-test-runner.c)
-- that the test tool starts in a headless shell after it installed the
-- fixture extensions of the test (--install-extension). The script is
-- one line per fixture:
--
--   local harness = dofile((os.getenv("SHELL_TEST_SCRIPT"):match("^(.*)/") or ".") .. "/harness.lua")
--   harness.run { uuid = "ui@test" }
--
-- run () enables the fixture through the extension manager of the shell, waits
-- until it reports that it is done (shell.test.done () in the fixture), fails
-- on any failed check or Lua error of the extension, disables it and checks
-- that nothing it made is left: no hook listener, no new actor on the stage.
-- The whole cycle runs twice (a second enable shows what a first one left
-- behind).
--
-- Options of run ():
--   uuid           the fixture (required)
--   drive          function (c) called when the extension is enabled, before
--                  the wait for its main part: drives the shell and asks the
--                  extension to run steps (see the context below)
--   after_disable  function (c) called when the extension is disabled, before
--                  the tests for what is left: checks that the handles that
--                  the script keeps (c.expect_dead) are dead
--   cycles         number of enable/disable cycles (default 2)
--   fixture        false for an extension that does not report (an example):
--                  it only has to enable and disable without errors
--   actors         false to skip the comparison of the actors on the stage
--                  (a test that opens windows or the overview makes actors of
--                  its own); the hook listeners are always compared
--   ignore_actors  set of actor type names that may appear, {StBoxLayout = true}
--   expect_errors  number of Lua errors the fixture causes on purpose
--   timeout        ms to wait for the state and the report (default 15000)
--
-- The context c that drive () and after_disable () receive:
--   c.uuid, c.cycle
--   c.step (name [, payload])   asks the extension to run the step: emits the
--                               hook test.step with {step = name, ...} and
--                               waits until the extension reports
--                               shell.test.done (name); a failed check fails
--   c.wait_done (label)         waits for shell.test.done (label)
--   c.wait_until (pred, what)   polls pred () until it is true
--   c.capture_log (fn)          runs fn () with the log of the extension
--                               captured (and warnings not fatal); returns
--                               the list of {level, message}
--   c.expect_dead (handle, what)  fails if a handle of the script is alive
--   c.state ()                  test.extension_state (uuid)

local H = {}

H.dir = (os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or "."

local DEFAULT_TIMEOUT = 15000

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

-- Polls pred () until it is true; what () describes the wait for the error
function H.wait_until (pred, what, timeout)
  local deadline = test.now () + (timeout or DEFAULT_TIMEOUT)

  while not pred () do
    if test.now () > deadline then
      fail ("timeout waiting for %s", type (what) == "function" and what () or what)
    end
    test.sleep (50)
  end
end

function H.wait_state (uuid, wanted, timeout)
  local set = {}
  for _, state in ipairs (wanted) do
    set[state] = true
  end

  local function current ()
    local st = test.extension_state (uuid)
    return st, st and set[st.state]
  end

  H.wait_until (function ()
    local _, ok = current ()
    return ok
  end, function ()
    local st = current ()
    return string.format ("%s to be %s (it is %s)", uuid, table.concat (wanted, " or "),
                          st and st.state or "not installed")
  end, timeout)

  return (current ())
end

-- Fails with the failed checks of the report of the extension
local function check_failures (uuid, what)
  local report = test.extension_report (uuid)
  local failures = {}

  for _, check in ipairs (report.checks) do
    if not check.ok then
      failures[#failures + 1] = string.format ("  %s: %s", check.name, check.message or "failed")
    end
  end

  if #failures > 0 then
    fail ("%s: %d of %d checks failed:\n%s", what, #failures, #report.checks,
          table.concat (failures, "\n"))
  end

  return report
end

local function same_counts (a, b)
  for name, n in pairs (a) do
    if b[name] ~= n then
      return false
    end
  end
  for name in pairs (b) do
    if a[name] == nil then
      return false
    end
  end
  return true
end

local function same_fingerprint (a, b)
  return same_counts (a.actors, b.actors) and same_counts (a.mapped, b.mapped) and
         same_counts (a.listeners, b.listeners)
end

-- What is on the stage and connected to the hooks, once the shell is idle and
-- two fingerprints in a row agree (the shell finishes some of its work, and
-- the animations of what was destroyed, a little after it is idle)
function H.fingerprint ()
  local previous

  for _ = 1, 8 do
    test.wait_leisure ()
    test.sleep (250)
    test.wait_leisure ()

    local current = test.fingerprint ()
    if previous and same_fingerprint (previous, current) then
      return current
    end
    previous = current
  end

  return previous
end

-- What the second fingerprint has more of than the first
local function growth (before, after, ignore)
  local grown = {}

  for name, n in pairs (after) do
    local was = before[name] or 0
    if n > was and not (ignore and ignore[name]) then
      grown[#grown + 1] = string.format ("%s: %d, was %d", name, n, was)
    end
  end

  table.sort (grown)
  return grown
end

function H.compare (before, after, opts)
  local problems = {}
  local hooks = {}

  for name in pairs (before.listeners) do
    hooks[name] = true
  end
  for name in pairs (after.listeners) do
    hooks[name] = true
  end
  for name in pairs (hooks) do
    local was, now = before.listeners[name] or 0, after.listeners[name] or 0
    if was ~= now then
      problems[#problems + 1] = string.format ("hook %s has %d listeners, %d before the extension ran",
                                               name, now, was)
    end
  end
  table.sort (problems)

  if opts.actors ~= false then
    for _, line in ipairs (growth (before.mapped, after.mapped, opts.ignore_actors)) do
      problems[#problems + 1] = "visible actors left behind, " .. line
    end
    for _, line in ipairs (growth (before.actors, after.actors, opts.ignore_actors)) do
      problems[#problems + 1] = "actors left behind, " .. line
    end
  end

  return problems
end

-- An extension that the loader refuses is not installed at all
function H.expect_rejected (uuid)
  local st = test.extension_state (uuid)

  if st ~= nil then
    fail ("%s should be rejected by the loader, its state is %s", uuid, st.state)
  end
  test.print (string.format ("%s is rejected", uuid))
end

-- An installed extension that must stay in the state (it can never run)
function H.expect_state (uuid, state, error_pattern)
  local st = test.extension_state (uuid)

  if st == nil then
    fail ("%s is not installed", uuid)
  end
  if st.state ~= state then
    fail ("%s is %s (%s), expected %s", uuid, st.state, st.error or "no error", state)
  end
  if error_pattern and not (st.error or ""):find (error_pattern, 1, true) then
    fail ("%s: the error is %q, expected it to mention %q", uuid, st.error or "", error_pattern)
  end

  -- enabling it does not start it
  test.extension_enable (uuid)
  test.sleep (500)
  test.wait_leisure ()
  st = test.extension_state (uuid)
  if st.state ~= state then
    fail ("%s is %s after it was enabled, expected %s", uuid, st.state, state)
  end
  test.extension_disable (uuid)
  test.print (string.format ("%s stays %s", uuid, state))
end

-- An installed extension that the shell does not run in this session (its
-- session-modes do not include it): enabling it starts nothing
function H.expect_idle (uuid)
  test.extension_enable (uuid)
  test.sleep (500)
  test.wait_leisure ()

  local st = test.extension_state (uuid)
  if st == nil then
    fail ("%s is not installed", uuid)
  end
  if st.state == "active" or st.state == "activating" or st.state == "error" then
    fail ("%s is %s in this session, it should not run: %s", uuid, st.state, st.error or "")
  end
  if #test.extension_report (uuid).checks > 0 then
    fail ("%s ran in this session", uuid)
  end
  test.extension_disable (uuid)
  test.print (string.format ("%s does not run in this session (%s)", uuid, st.state))
end

-- The context c of run (), also for the tests that do not use run ()
function H.context (uuid, cycle, timeout)
  local c = { uuid = uuid, cycle = cycle }

  function c.wait_done (label)
    H.wait_until (function ()
      if test.extension_report (uuid).done[label] then
        return true
      end
      local st = test.extension_state (uuid)
      if st == nil or st.state ~= "active" then
        fail ("%s left the active state (%s) while the script waited for %s: %s",
              uuid, st and st.state or "uninstalled", label, st and st.error or "")
      end
      return false
    end, string.format ("%s to report '%s' as done", uuid, label), timeout)
  end

  function c.step (name, payload)
    local p = { step = name }

    for key, value in pairs (payload or {}) do
      p[key] = value
    end
    shell.hooks.emit_after ("test.step", p)
    c.wait_done (name)
    check_failures (uuid, string.format ("%s, step %s", uuid, name))
  end

  function c.wait_until (pred, what)
    H.wait_until (pred, what, timeout)
  end

  function c.capture_log (fn)
    test.log_capture (uuid, true)
    local ok, err = pcall (fn)
    local log = test.log_capture (uuid, false)

    if not ok then
      error (err, 0)
    end
    return log
  end

  function c.expect_dead (handle, what)
    local ok, alive = pcall (function ()
      return handle.alive
    end)

    if ok and alive == nil then
      ok, alive = pcall (function ()
        return handle:gtype () ~= nil
      end)
    end
    if ok and alive then
      fail ("%s is still alive after %s was disabled", what or tostring (handle), uuid)
    end
  end

  function c.state ()
    return test.extension_state (uuid)
  end

  return c
end

function H.run (opts)
  local uuid = assert (opts.uuid, "run (): uuid is required")
  local cycles = opts.cycles or 2
  local timeout = opts.timeout or DEFAULT_TIMEOUT
  local fixture = opts.fixture ~= false

  test.wait_startup ()
  local before = H.fingerprint ()

  for cycle = 1, cycles do
    local what = string.format ("%s (enable %d of %d)", uuid, cycle, cycles)
    local c = H.context (uuid, cycle, timeout)

    test.extension_enable (uuid)
    local st = H.wait_state (uuid, { "active", "error", "out-of-date" }, timeout)
    if st.state ~= "active" then
      fail ("%s: the extension is %s: %s", what, st.state, st.error or "no error")
    end

    if opts.drive then
      opts.drive (c)
    end

    local passed = 0
    if fixture then
      c.wait_done ("main")
      passed = check_failures (uuid, what).passed
      if passed == 0 then
        fail ("%s: the fixture reported no check", what)
      end
    end

    st = test.extension_state (uuid)
    if st.state ~= "active" then
      fail ("%s: the extension is %s after the test: %s", what, st.state, st.error or "no error")
    end
    if st.errors ~= (opts.expect_errors or 0) then
      fail ("%s: the extension raised %d Lua errors, expected %d (see the log)", what, st.errors,
            opts.expect_errors or 0)
    end

    test.extension_disable (uuid)
    H.wait_state (uuid, { "inactive", "error" }, timeout)
    st = test.extension_state (uuid)
    if st.state ~= "inactive" then
      fail ("%s: the extension is %s after it was disabled: %s", what, st.state, st.error or "no error")
    end

    if fixture then
      -- what its disable () checked
      check_failures (uuid, what .. ", disable")

      -- no callback runs any more
      local n = #test.extension_report (uuid).checks
      shell.hooks.emit_after ("test.step", { step = "after-disable" })
      test.sleep (100)
      local report = test.extension_report (uuid)
      if #report.checks ~= n or report.done["after-disable"] then
        fail ("%s: a callback of the extension ran after it was disabled", what)
      end
    end

    if opts.after_disable then
      opts.after_disable (c)
    end

    local problems = H.compare (before, H.fingerprint (), opts)
    if #problems > 0 then
      fail ("%s: the shell is not as it was before the extension ran:\n  %s", what,
            table.concat (problems, "\n  "))
    end

    test.print (string.format ("%s: %s%s", what, fixture and string.format ("%d checks passed, ", passed) or "",
                               "disabled cleanly"))
  end
end

return H
