-- Conformance test of docs/api/worker.md: see tests/api/README.md. The
-- assertions of the API are in the fixtures tests/api/fixtures/worker@test
-- (with the "worker" capability: messages, limits, stopping, errors) and
-- worker-nocap@test (nothing runs without the capability), run by the
-- extension manager of the shell; this script drives the step that causes
-- errors under a captured log, checks that the log of the worker reached it,
-- and checks from the outside that the thread of the worker the extension
-- left looping is gone after the disable.

local harness = dofile (((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

test.wait_startup ()

if test.live_workers () ~= 0 then
  fail ("%d worker threads before any extension ran", test.live_workers ())
end

-- Without "worker" in the capabilities shell.worker.start raises
harness.run { uuid = "worker-nocap@test" }

if test.live_workers () ~= 0 then
  fail ("the refused starts left %d worker threads", test.live_workers ())
end

harness.run {
  uuid = "worker@test",
  timeout = 60000,
  -- the step "errors": a handler that raises, a module that is missing, one
  -- that is not a worker, one that runs out of memory
  expect_errors = 4,

  drive = function (c)
    c.wait_done ("main")

    local log = c.capture_log (function () c.step ("errors") end)
    local seen = {}

    for _, entry in ipairs (log) do
      for _, needle in ipairs { "hello from the worker 42", "printed by the worker", "worker-", "not enough memory" } do
        if entry.message:find (needle, 1, true) then
          seen[needle] = true
        end
      end
    end
    for _, needle in ipairs { "hello from the worker 42", "printed by the worker", "worker-", "not enough memory" } do
      if not seen[needle] then
        local lines = {}
        for _, entry in ipairs (log) do
          lines[#lines + 1] = entry.level .. ": " .. entry.message
        end
        fail ("the captured log lacks %q; it has:\n%s", needle, table.concat (lines, "\n"))
      end
    end
  end,

  after_disable = function (c)
    -- the worker left looping at disable is stopped, and its thread exits on
    -- its own (the shell never waits for it)
    c.wait_until (function () return test.live_workers () == 0 end,
                  function () return string.format ("the worker threads to exit (%d left)", test.live_workers ()) end)
  end,
}
