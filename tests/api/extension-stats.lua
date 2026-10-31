-- The tooling that finds the extensions that cost frames: the time of the
-- calls into an extension, measured by the runtime (ShellLuaStats), as
-- gnome-extensions shows it from the extension info of
-- org.gnome.Shell.Extensions (`info`, and the table of `stats`; the script
-- starts that service, which session activation starts outside tests). The fixture
-- tests/api/fixtures/extension-stats@test runs one timer callback of 40 ms
-- per enable: the figures count from the enable, so each cycle sees that
-- callback as the longest. See tests/api/README.md.

local harness = dofile (((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local uuid = "extension-stats@test"

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

test.wait_startup ()

-- Runs @argv; returns its result
local function run (argv, what)
  local result
  local proc, err = shell.spawn { argv = argv, env = { LANGUAGE = "C", LC_ALL = "C" },
                                  on_exit = function (r) result = r end }

  if not proc then
    fail ("%s: %s", what, err)
  end
  harness.wait_until (function () return result ~= nil end, what)
  return result
end

local function getenv (name)
  return os.getenv (name) or fail ("%s is not set", name)
end

-- The service the tool talks to; it forwards to the shell
local service = shell.spawn { argv = { getenv ("GNOME_SHELL_EXTENSIONS_SERVICE") } }

if not service then
  fail ("cannot start org.gnome.Shell.Extensions")
end

-- The output of gnome-extensions with @args. The service may not own its name
-- yet: the tool cannot connect until it does
local function gnome_extensions (args)
  local argv = { getenv ("GNOME_EXTENSIONS_TOOL") }
  local deadline = test.now () + 15000
  local result

  for _, arg in ipairs (args) do
    argv[#argv + 1] = arg
  end
  repeat
    result = run (argv, "gnome-extensions " .. args[1])
    if result.status == 0 then
      return result.stdout
    end
    test.sleep (100)
  until test.now () > deadline
  fail ("gnome-extensions %s exited with %s: %s", args[1], tostring (result.status), result.stderr)
end

local function field (out, name)
  return out:match ("\n  " .. name .. ": ([^\n]*)")
end

harness.run {
  uuid = uuid,
  timeout = 30000,

  drive = function (c)
    c.wait_done ("main")
    c.wait_done ("slow")

    -- enable (), the callback, and nothing longer than the callback
    local info = gnome_extensions { "info", uuid }
    local calls, total = (field (info, "Callbacks") or ""):match ("^(%d+), ([%d.]+) ms$")
    local longest, where = (field (info, "Longest callback") or ""):match ("^([%d.]+) ms %((.*)%)$")
    local slow = field (info, "Callbacks longer than a frame")

    if not calls or tonumber (calls) < 2 then
      fail ("no callbacks in:\n%s", info)
    end
    if not longest or tonumber (longest) < 40 or tonumber (longest) > 1000 then
      fail ("the longest callback is not the one of 40 ms:\n%s", info)
    end
    if tonumber (total) < tonumber (longest) then
      fail ("the total is less than the longest:\n%s", info)
    end
    if not where:find ("^extension%.lua:%d+$") then
      fail ("the longest callback is at %q", where)
    end
    if not slow or tonumber (slow) < 1 then
      fail ("no callback longer than a frame:\n%s", info)
    end
    if not field (info, "Lua memory") then
      fail ("no memory in:\n%s", info)
    end

    -- the same figures in the table, with a header
    local out = gnome_extensions { "stats" }
    local line

    if not out:find ("^EXTENSION%s+CALLBACKS%s+TOTAL%s+LONGEST%s+SLOW%s+MEMORY%s+WHERE\n") then
      fail ("no header in:\n%s", out)
    end
    for l in out:gmatch ("[^\n]+") do
      if l:find (uuid, 1, true) == 1 then
        line = l
      end
    end
    if not line then
      fail ("%s is not in:\n%s", uuid, out)
    end
    if not line:find (where, 1, true) then
      fail ("the table does not say %s: %q", where, line)
    end
  end,
}

-- A disabled extension has no figures: they are those of a running state
local info = gnome_extensions { "info", uuid }

if field (info, "Callbacks") then
  fail ("a disabled extension has figures:\n%s", info)
end
