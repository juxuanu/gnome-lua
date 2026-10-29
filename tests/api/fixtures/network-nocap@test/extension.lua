-- Fixture of tests/api/network.lua: without "network" in the capabilities of
-- metadata.json shell.network.request raises, before anything is sent
-- (docs/api/network.md, README.md "Capabilities").
local shell = require("shell")
local T = shell.test

-- ---------------------------------------------------------------------
-- Helpers of the fixtures (tests/api/README.md): copy them as they are
-- ---------------------------------------------------------------------
local function case(name, fn, ...)
  local ok, err = pcall(fn, ...)
  T.report(name, ok, not ok and tostring(err) or nil)
  return ok
end

local function eq(got, want, what)
  if got ~= want then
    error(string.format("%s: expected %s, got %s", what or "value", tostring(want), tostring(got)), 2)
  end
end

-- fn () must raise an error that contains every string of `patterns` (a
-- string or a list; nil: any error)
local function raises(fn, patterns, what)
  local ok, err = pcall(fn)
  if ok then
    error(string.format("%s did not raise", what or "the call"), 2)
  end
  if type(patterns) == "string" then
    patterns = { patterns }
  end
  for _, pattern in ipairs(patterns or {}) do
    if not tostring(err):find(pattern, 1, true) then
      error(string.format("%s raised %q, expected it to mention %q", what or "the call", tostring(err), pattern), 2)
    end
  end
  return tostring(err)
end

local ext = {}

function ext.enable()
  case("shell.network.request raises without the capability", function()
    eq(type(shell.network), "table", "shell.network")
    eq(type(shell.network.request), "function", "shell.network.request")
    raises(function() shell.network.request{ url = "http://127.0.0.1:9/" } end,
           { "shell.network.request()", "requires the \"network\" capability" }, "request")
    -- the capability is checked before the arguments
    raises(function() shell.network.request{} end, "requires the \"network\" capability", "request with no url")
    raises(function() shell.network.request() end, "requires the \"network\" capability", "request with no table")
  end)

  T.done()
end

return ext
