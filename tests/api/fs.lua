-- Conformance test of docs/api/fs.md: see tests/api/README.md. The assertions
-- of the API are in the fixtures tests/api/fixtures/fs@test (with the "fs"
-- capability: every function, the data directory, the paths outside it) and
-- fs-nocap@test (the same API without the capability: the extension's own
-- directories, and the capability error everywhere else), run by the extension
-- manager of the shell; this script checks from the outside what the
-- extension left when it was disabled.

local harness = dofile (((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

test.wait_startup ()

-- The directories and the capability error of an extension without "fs"
harness.run { uuid = "fs-nocap@test" }

-- This script is a privileged state: shell.fs is open to it, anywhere. Runs
-- one asynchronous function and waits for it.
local function run (c, kind, opts)
  local result

  opts.on_done = function (value)
    result = { ok = true, value = value }
  end
  opts.on_error = function (err)
    result = { ok = false, err = err }
  end
  shell.fs[kind] (opts)
  c.wait_until (function () return result ~= nil end, "shell.fs." .. kind)
  return result
end

-- The data directory the fixture reported
local function data_dir (uuid)
  for _, check in ipairs (test.extension_report (uuid).checks) do
    if check.name == "data-dir" then
      return check.message
    end
  end
  fail ("%s did not report its data directory", uuid)
end

harness.run {
  uuid = "fs@test",
  timeout = 30000,

  after_disable = function (c)
    local dir = data_dir ("fs@test")

    -- the data directory outlives the extension
    local read = run (c, "read", { path = dir .. "/persist.txt" })
    if not read.ok or read.value ~= "kept" then
      fail ("the file the extension left in its data directory is not there: %s",
            read.ok and tostring (read.value) or read.err.message)
    end

    -- its watch is gone: a change in the watched directory calls nothing, and
    -- the shell is still standing
    local n = #test.extension_report ("fs@test").checks
    local poke = run (c, "write", { path = dir .. "/keep/poke", contents = "x", append = true })
    if not poke.ok then
      fail ("cannot write into the data directory of the extension: %s", poke.err.message)
    end
    test.sleep (400)
    if #test.extension_report ("fs@test").checks ~= n then
      fail ("a callback of the extension ran after it was disabled (the watch)")
    end
  end,
}
