-- Fixture of tests/api/search.lua: without the "search" capability
-- shell.search.add_provider raises before anything is created
-- (docs/api/search.md, "Capability").
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

local function truthy(value, what)
  if not value then
    error(string.format("%s: expected a true value, got %s", what or "value", tostring(value)), 2)
  end
  return value
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

-- handlers[name] (payload, done) runs when the script emits the step `name`;
-- it is finished when it returns, unless it returns "later": then it calls
-- done () itself
local function steps(handlers)
  shell.hooks.on("test.step", function(p)
    local handler = handlers[p.step]
    local finished = false

    if not handler then
      return
    end

    local function done()
      if not finished then
        finished = true
        T.done(p.step)
      end
    end

    local ok, result = pcall(handler, p, done)
    if not ok then
      T.report("step " .. p.step, false, tostring(result))
      done()
    elseif result ~= "later" then
      done()
    end
  end)
end

local ext = {}

function ext.enable()
  case("no capability declared", function()
    eq(#shell.extension.capabilities, 0, "capabilities")
  end)

  case("shell.search exists, the call is refused", function()
    eq(type(shell.search), "table", "shell.search")
    eq(type(shell.search.add_provider), "function", "shell.search.add_provider")
    raises(function()
      shell.search.add_provider{
        id = "x", name = "X",
        on_search = function() end, on_activate = function() end,
      }
    end, { "shell.search.add_provider", 'requires the "search" capability' }, "add_provider without the capability")
  end)

  case("the capability is checked before the arguments", function()
    raises(function() shell.search.add_provider() end, 'requires the "search" capability', "no table")
    raises(function() shell.search.add_provider{ bogus = 1 } end, 'requires the "search" capability', "bad fields")
  end)

  case("the hook search.query needs it too", function()
    raises(function() shell.hooks.on("search.query", function() end) end, 'requires the "search" capability',
           "search.query")
  end)

  T.done()
end

return ext
