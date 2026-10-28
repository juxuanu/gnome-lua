-- Fixture of tests/api/hooks.lua: the hook search.query is gated by the
-- capability "search" (docs/api/hooks.md, "Rules for emitters"; README.md,
-- "Capabilities"). This extension declares it.
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

local queries = {}

function ext.enable()
  case("shell.extension.capabilities", function()
    eq(#shell.extension.capabilities, 1, "number of capabilities")
    eq(shell.extension.capabilities[1], "search", "capability")
  end)

  case("search.query is listed with its capability", function()
    local found

    for _, entry in ipairs(shell.hooks.list()) do
      if entry.name == "search.query" then
        found = entry
      end
    end
    truthy(found, "search.query is declared")
    eq(found.capability, "search", "capability")
    eq(found.cancellable, false, "cancellable")
    truthy(found.schema:find("query:s", 1, true), "query:s in " .. found.schema)
    truthy(found.schema:find("terms:as", 1, true), "terms:as in " .. found.schema)
  end)

  case("search.query can be connected with the capability", function()
    local conn = shell.hooks.on("search.query", function(payload)
      queries[#queries + 1] = payload
    end)

    eq(type(conn), "userdata", "a connection")
    -- it is not cancellable
    raises(function() shell.hooks.before("search.query", function() end) end, "not cancellable", "before")
    -- the other hooks are not gated
    shell.hooks.on("overview.hidden", function() end)
  end)

  steps {
    ["expect-query"] = function(p)
      eq(#queries, p.n, "number of queries")
      local q = queries[#queries]

      eq(q.query, p.query, "query")
      eq(#q.terms, #p.terms, "number of terms")
      for i, term in ipairs(p.terms) do
        eq(q.terms[i], term, "term " .. i)
      end
      eq(math.type(#q.terms), "integer", "terms is a sequence")
    end,
  }

  T.done()
end

return ext
