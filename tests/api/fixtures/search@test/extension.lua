-- Fixture of tests/api/search.lua: every function, field, callback and error
-- kind of docs/api/search.md. enable () runs the checks that are synchronous
-- (arguments, handles, ownership); the script of the test types in the search
-- entry of the overview and asks for the checks of what the providers were
-- told with steps (tests/api/README.md). Every call of on_search reports a
-- check named "search: <terms>" (the script waits for it) and the providers
-- do what the first term says: "echo" answers at once with 7 results, "later"
-- and "stale" keep `respond` for a step, "bad" answers wrongly, "icons"
-- answers with the three kinds of icon, "invalid N" tries the Nth wrong
-- answer. The second provider has an on_launch.
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

local UUID = "search@test"

local ext = {}

-- What the steps check between calls
local kept = {
  searches = {},       -- { provider, terms, respond } of every call of on_search
  activated = nil,     -- the last on_activate: { provider, id, terms }
  launched = nil,      -- the last on_launch
  invalid = {},        -- N -> { ok, err }
  bad = nil,
  second_searches = 0,
}

local function many(terms, n)
  local q = table.concat(terms, " ")
  local out = {}

  for i = 1, n do
    out[i] = {
      id = "r" .. i,
      name = string.format("Result %d for %s", i, q),
      description = "description " .. i .. "\nsecond line",
      icon_name = "edit-find-symbolic",
      clipboard_text = i == 1 and "clip" or nil,
    }
  end
  return out
end

-- Each wrong answer of the section "Errors" of search.md: the error must name
-- the field. Returned by number so that every search tries one.
local invalid_answers = {
  { function() return "x" end, "sequence" },
  { function() return { "x" } end, "table" },
  { function() return { { name = "N" } } end, "id" },
  { function() return { { id = "i" } } end, "name" },
  { function() return { { id = "i", name = "N", bogus = 1 } } end, "bogus" },
  { function() return { { id = "i", name = "N", icon_name = "a-symbolic", icon_file = "icon.svg" } } end, "icon" },
  { function() return { { id = "i", name = "N", icon_file = "../x.svg" } } end, "icon_file" },
  { function() return { { id = "i", name = "N", icon_file = "/etc/hostname" } } end, "icon_file" },
  { function() return { id = "i", name = "N" } end, "sequence" },
  { function() return { { id = 5, name = "N" } } end, "id" },
}

local function on_search(provider, terms, respond)
  local mode = terms[1]

  T.report("search: " .. table.concat(terms, " "), true)
  kept.searches[#kept.searches + 1] = { provider = provider, terms = terms, respond = respond }

  if mode == "echo" then
    case("on_search arguments", function()
      eq(type(provider), "userdata", "the provider")
      eq(provider.id, "calc", "provider.id")
      eq(type(terms), "table", "terms")
      eq(#terms, 2, "number of terms")
      eq(terms[1], "echo", "terms[1]")
      -- the script searches "echo foo" and, later, "echo bar"
      truthy(terms[2] == "foo" or terms[2] == "bar", "terms[2]")
      eq(type(respond), "function", "respond")
    end)
    respond(many(terms, 7))
  elseif mode == "later" or mode == "stale" then
    -- answered by a step
  elseif mode == "bad" then
    -- a wrong answer raises in respond, and answers the search with no results
    local ok, err = pcall(respond, { { id = "x" } })
    kept.bad = { ok = ok, err = tostring(err), again = pcall(respond, many(terms, 2)) }
  elseif mode == "icons" then
    respond{
      { id = "a", name = "By name", icon_name = "edit-find-symbolic" },
      { id = "b", name = "By relative file", icon_file = "icon.svg" },
      { id = "c", name = "By absolute file", icon_file = shell.extension.dir .. "/icon.svg" },
      { id = "d", name = "No icon" },
      { id = "d", name = "Same id twice" },
    }
  elseif mode == "invalid" then
    local n = tonumber(terms[2])
    local answer, field = invalid_answers[n][1](), invalid_answers[n][2]
    local ok, err = pcall(respond, answer)

    kept.invalid[n] = { ok = ok, err = tostring(err), field = field }
  elseif mode == "none" then
    respond{}
  elseif mode == "error" then
    error("the extension fails on purpose")
  else
    respond(many(terms, 2))
  end
end

local function on_activate(provider, id, terms)
  kept.activated = { provider = provider, id = id, terms = terms }
  T.report("activate: " .. id .. " (" .. table.concat(terms, " ") .. ")", true)
end

function ext.enable()
  -- -------------------------------------------------------------------
  -- The table
  -- -------------------------------------------------------------------
  case("shell.search", function()
    eq(type(shell.search), "table", "shell.search")
    eq(type(shell.search.add_provider), "function", "shell.search.add_provider")
    eq(shell.search.no_such_function, nil, "an absent function is nil, not a stub")
  end)

  -- -------------------------------------------------------------------
  -- Misuse: nothing is created by a call that raises
  -- -------------------------------------------------------------------
  local function with(extra, without)
    local t = { id = "x", name = "X", on_search = function() end, on_activate = function() end }

    for k, v in pairs(extra or {}) do t[k] = v end
    if without then t[without] = nil end
    return t
  end

  case("add_provider: required and unknown fields", function()
    raises(function() shell.search.add_provider() end, nil, "a call without a table")
    raises(function() shell.search.add_provider("x") end, nil, "a string instead of a table")
    raises(function() shell.search.add_provider(with({}, "id")) end, "id", "no id")
    raises(function() shell.search.add_provider(with({ id = "" })) end, "id", "an empty id")
    raises(function() shell.search.add_provider(with({ id = 5 })) end, "id", "a number as id")
    raises(function() shell.search.add_provider(with({}, "name")) end, "name", "no name")
    raises(function() shell.search.add_provider(with({ name = "" })) end, "name", "an empty name")
    raises(function() shell.search.add_provider(with({}, "on_search")) end, "on_search", "no on_search")
    raises(function() shell.search.add_provider(with({}, "on_activate")) end, "on_activate", "no on_activate")
    raises(function() shell.search.add_provider(with({ on_search = 5 })) end, "on_search", "a number as on_search")
    raises(function() shell.search.add_provider(with({ on_activate = "f" })) end, "on_activate", "a string as on_activate")
    raises(function() shell.search.add_provider(with({ on_launch = 5 })) end, "on_launch", "a number as on_launch")
    raises(function() shell.search.add_provider(with({ bogus = 1 })) end, "bogus", "an unknown field")
  end)

  case("add_provider: icons", function()
    raises(function() shell.search.add_provider(with({ icon_name = 5 })) end, "icon_name", "a number as icon_name")
    raises(function() shell.search.add_provider(with({ icon_name = "" })) end, "icon_name", "an empty icon_name")
    raises(function() shell.search.add_provider(with({ icon_name = "a-symbolic", icon_file = "icon.svg" })) end,
           "icon", "both icons")
    raises(function() shell.search.add_provider(with({ icon_file = "/etc/hostname" })) end, "icon_file",
           "an absolute path")
    raises(function() shell.search.add_provider(with({ icon_file = "../icon.svg" })) end, "icon_file",
           "a path with ..")
    raises(function() shell.search.add_provider(with({ icon_file = "missing.svg" })) end, "icon_file",
           "a missing file")
    raises(function() shell.search.add_provider(with({ icon_file = "extension.lua" })) end, "icon_file",
           "a wrong suffix")
  end)

  -- -------------------------------------------------------------------
  -- The providers
  -- -------------------------------------------------------------------
  local calc = shell.search.add_provider{
    id = "calc", name = "Calculator", icon_name = "accessories-calculator-symbolic",
    on_search = on_search, on_activate = on_activate,
  }
  kept.calc = calc

  case("provider: the handle", function()
    eq(type(calc), "userdata", "a provider is a handle")
    eq(calc.alive, true, "alive")
    eq(calc.id, "calc", "id")
    eq(calc.no_such_property, nil, "an unknown property reads as nil")
    truthy(calc == calc, "the same handle")
    eq(type(calc.destroy), "function", "destroy")
    raises(function() calc.id = "other" end, nil, "assigning id")
    raises(function() calc.alive = false end, nil, "assigning alive")
    raises(function() calc.no_such_property = 1 end, nil, "assigning an unknown property")
    eq(calc.id, "calc", "id after the writes")
  end)

  case("provider: the first search is not made by creating it", function()
    eq(#kept.searches, 0, "calls of on_search at creation")
  end)

  local second = shell.search.add_provider{
    id = "second", name = "Second", icon_file = "icon.svg",
    on_search = function(provider, terms, respond)
      kept.second_searches = kept.second_searches + 1
      T.report("second: " .. table.concat(terms, " "), true)
      respond{ { id = "only", name = "The only result of " .. table.concat(terms, " ") } }
    end,
    on_activate = function(provider, id, terms)
      T.report("second activate: " .. id, true)
    end,
    on_launch = function(provider, terms)
      kept.launched = { provider = provider, terms = terms }
      T.report("launch: " .. table.concat(terms, " "), true)
    end,
  }
  kept.second = second

  case("add_provider: ids are unique among the live providers of the extension", function()
    raises(function() shell.search.add_provider(with({ id = "calc" })) end, "calc", "a second provider with the id")
    raises(function() shell.search.add_provider(with({ id = "second" })) end, "second", "a second provider with the id")
  end)

  case("add_provider: at most 8 providers per extension", function()
    local extra = {}

    for i = 1, 6 do
      extra[i] = shell.search.add_provider(with({ id = "extra" .. i }))
    end
    raises(function() shell.search.add_provider(with({ id = "extra7" })) end, { "8" }, "the ninth provider")
    -- destroying one frees its slot
    extra[1]:destroy()
    local again = shell.search.add_provider(with({ id = "extra1" }))
    eq(again.alive, true, "a provider after a slot was freed")
    for _, p in ipairs(extra) do p:destroy() end
    again:destroy()
  end)

  case("destroy: the handle dies, the id is free, destroy () is idempotent", function()
    local p = shell.search.add_provider(with({ id = "temp" }))

    eq(p.alive, true, "alive before")
    p:destroy()
    eq(p.alive, false, "alive after destroy")
    p:destroy()
    eq(p.alive, false, "alive after the second destroy")
    raises(function() return p.id end, "no longer valid", "reading id of a dead provider")
    local q = shell.search.add_provider(with({ id = "temp" }))
    eq(q.alive, true, "the same id again")
    truthy(q ~= p, "a new handle")
    q:destroy()
  end)

  -- -------------------------------------------------------------------
  -- The steps
  -- -------------------------------------------------------------------
  steps {
    -- the calls of on_search that the script waited for
    ["check-search"] = function(p)
      local last = kept.searches[#kept.searches]

      truthy(last, "a call of on_search")
      eq(last.provider, calc, "the provider of the last call is the handle that add_provider returned")
    end,

    -- the first result of "echo foo" was activated
    ["check-activate"] = function(p)
      local a = kept.activated

      truthy(a, "on_activate was called")
      eq(a.id, "r1", "id of the activated result")
      eq(a.provider, calc, "the provider")
      eq(#a.terms, 2, "number of terms")
      eq(a.terms[1], "echo", "terms[1]")
      eq(a.terms[2], "foo", "terms[2]")
    end,

    ["check-launch"] = function(p)
      local l = kept.launched

      truthy(l, "on_launch was called")
      eq(l.provider, second, "the provider")
      eq(l.terms[1], "echo", "terms[1]")
      eq(l.terms[2], "foo", "terms[2]")
    end,

    -- answers a "later" search from outside of on_search
    ["respond-later"] = function(p)
      local found

      for i = #kept.searches, 1, -1 do
        if kept.searches[i].terms[1] == "later" then
          found = kept.searches[i]
          break
        end
      end
      truthy(found, "a search for 'later'")
      found.respond(many(found.terms, 2))
      -- the second call of respond for the same search is ignored
      found.respond(many(found.terms, 4))
    end,

    -- the respond of the first of two searches does nothing; the second
    -- answers with a single result
    ["respond-stale"] = function(p)
      local first, current

      for _, s in ipairs(kept.searches) do
        if s.terms[1] == "stale" and s.terms[2] == "one" then first = s end
        if s.terms[1] == "stale" and s.terms[2] == "two" then current = s end
      end
      truthy(first, "the search 'stale one'")
      truthy(current, "the search 'stale two'")
      first.respond(many(first.terms, 3))   -- cancelled by the newer search: ignored, no error
      current.respond{ { id = "s2", name = "Stale two" } }
    end,

    ["check-bad"] = function(p)
      truthy(kept.bad, "the search 'bad' ran")
      eq(kept.bad.ok, false, "respond with a result without a name raises")
      truthy(kept.bad.err:find("name", 1, true), "the error names the field: " .. kept.bad.err)
      eq(kept.bad.again, true, "a respond after the search was answered does nothing, and does not raise")
    end,

    ["check-invalid"] = function(p)
      for n, answer in ipairs(invalid_answers) do
        local got = kept.invalid[n]

        truthy(got, "the search 'invalid " .. n .. "' ran")
        eq(got.ok, false, "invalid answer " .. n .. " raises")
        truthy(got.err:find(answer[2], 1, true),
               string.format("invalid answer %d: the error %q names %q", n, got.err, answer[2]))
      end
    end,

    -- destroy the second provider: its section goes, it is not asked again
    ["destroy-second"] = function(p)
      second:destroy()
      eq(second.alive, false, "alive after destroy")
      kept.second_searches = 0
    end,

    ["check-second-silent"] = function(p)
      eq(kept.second_searches, 0, "calls of the destroyed provider")
    end,

    -- the id of a destroyed provider is free
    ["re-add-second"] = function(p)
      kept.second = shell.search.add_provider{
        id = "second", name = "Second again",
        on_search = function(provider, terms, respond)
          T.report("second again: " .. table.concat(terms, " "), true)
          respond{ { id = "only", name = "Again" } }
        end,
        on_activate = function() end,
      }
      eq(kept.second.alive, true, "a provider with the id of a destroyed one")
    end,
  }

  T.done()
end

return ext
