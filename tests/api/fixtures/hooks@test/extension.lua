-- Fixture of tests/api/hooks.lua: every rule of docs/api/hooks.md and of the
-- shell.hooks section of docs/api/core.md. The listeners record what the shell
-- (or the script, with shell.hooks.emit_*) emits and check each payload
-- against the catalogue (catalogue.lua is docs/api/hooks.md as data); the
-- script drives the shell and asks for the verdicts with steps
-- (tests/api/README.md).
local shell = require("shell")
local T = shell.test
local catalogue = require("catalogue")

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


-- `value` as the Lua value that the GVariant type `gtype` becomes (variant.md)
local function check_type(key, value, gtype)
  if gtype == "b" then
    eq(type(value), "boolean", key)
  elseif gtype:match("^[ynqiuxth]$") then
    eq(math.type(value), "integer", key)
  elseif gtype == "d" then
    eq(type(value), "number", key)
  elseif gtype == "s" or gtype == "o" or gtype == "g" then
    eq(type(value), "string", key)
  elseif gtype == "as" then
    eq(type(value), "table", key)
    for i, item in ipairs(value) do
      eq(type(item), "string", key .. "[" .. i .. "]")
    end
  else
    error("the catalogue has the type " .. gtype .. " for " .. key)
  end
end

local function validate(hook, payload)
  eq(type(payload), "table", "the payload of " .. hook.name)
  for _, spec in ipairs(hook.keys) do
    local key, gtype, optional = spec[1], spec[2], spec[3]
    if payload[key] == nil then
      if not optional then
        error(hook.name .. ": the key " .. key .. " is missing")
      end
    else
      check_type(hook.name .. "." .. key, payload[key], gtype)
    end
  end
end

local function same(a, b)
  if type(a) ~= "table" or type(b) ~= "table" then
    return a == b
  end
  for key, value in pairs(a) do
    if not same(value, b[key]) then
      return false
    end
  end
  for key in pairs(b) do
    if a[key] == nil then
      return false
    end
  end
  return true
end

local ext = {}

local events = {}        -- {key = "hook:phase", payload = {...}} since the last reset
local total = {}         -- key -> count, never reset: the labels of the events
local last = {}          -- key -> the last payload
local calls = {}         -- hook -> {A = n, B = n} since the last reset
local armed = {}         -- hook -> {mode = ..., amend = ..., value = ...}
local before_a = {}      -- hook -> the connection of the first before listener
local connected = {}     -- the catalogue hooks this extension listens to
local pings = 0

local function record(hook, phase, payload)
  local key = hook.name .. ":" .. phase

  case("payload of " .. key, validate, hook, payload)
  events[#events + 1] = { key = key, payload = payload }
  total[key] = (total[key] or 0) + 1
  last[key] = payload
  T.done("event:" .. key .. ":" .. total[key])
end

-- What the listener A of a cancellable hook does: the script arms it
local function behave(name)
  local a = armed[name]

  if a == nil then
    return nil
  elseif a.mode == "cancel" then
    return shell.hooks.CANCEL
  elseif a.mode == "amend" then
    return a.amend
  elseif a.mode == "error" then
    error("a listener failed on purpose")
  elseif a.mode == "junk" then
    return a.value
  end
end

function ext.enable()
  -- -------------------------------------------------------------------
  -- shell.hooks.list ()
  -- -------------------------------------------------------------------
  local listed = {}
  case("shell.hooks.list", function()
    local list = shell.hooks.list()
    local names = {}

    eq(type(list), "table", "list ()")
    truthy(#list > 0, "the shell declares hooks")
    for i, entry in ipairs(list) do
      eq(type(entry), "table", "entry " .. i)
      eq(type(entry.name), "string", "name of entry " .. i)
      eq(type(entry.cancellable), "boolean", "cancellable of " .. entry.name)
      if entry.doc ~= nil then
        eq(type(entry.doc), "string", "doc of " .. entry.name)
      end
      if entry.schema ~= nil then
        eq(type(entry.schema), "string", "schema of " .. entry.name)
      end
      truthy(not names[entry.name], "the hook " .. entry.name .. " is listed once")
      names[entry.name] = true
      listed[entry.name] = entry
    end
    truthy(listed["test.step"], "the hook of the test runner is listed")
  end)

  -- -------------------------------------------------------------------
  -- The catalogue against what the shell declares
  -- -------------------------------------------------------------------
  for _, hook in ipairs(catalogue) do
    local entry = listed[hook.name]

    if hook.declared then
      case("the shell declares " .. hook.name, function()
        truthy(entry, hook.name .. " is not in shell.hooks.list ()")
      end)
    end

    if entry then
      case("catalogue: " .. hook.name, function()
        eq(entry.cancellable, hook.cancellable, "cancellable")
        eq(type(entry.schema), "string", "schema")
        for _, spec in ipairs(hook.keys) do
          local token = spec[1] .. ":" .. spec[2]
          local found = false

          for word in entry.schema:gmatch("%S+") do
            if word == token then
              found = true
            end
          end
          truthy(found, "the schema (" .. entry.schema .. ") lacks " .. token)
        end
        eq(entry.capability, hook.gated, "capability")
      end)
    else
      case("a reserved hook is unknown: " .. hook.name, function()
        raises(function() shell.hooks.on(hook.name, function() end) end, "unknown hook", "on")
      end)
    end
  end

  -- -------------------------------------------------------------------
  -- Connecting
  -- -------------------------------------------------------------------
  case("shell.hooks.on and before: errors", function()
    local function f() end

    raises(function() shell.hooks.on("no.such-hook", f) end, "unknown hook", "on, unknown hook")
    raises(function() shell.hooks.before("no.such-hook", f) end, "unknown hook", "before, unknown hook")
    raises(function() shell.hooks.on("nodot", f) end, "unknown hook", "on, name without a dot")
    raises(function() shell.hooks.on("", f) end, "unknown hook", "on, empty name")
    raises(function() shell.hooks.on() end, nil, "on()")
    raises(function() shell.hooks.on(5, f) end, nil, "on(5, f)")
    raises(function() shell.hooks.on("shell.started") end, nil, "on without a function")
    raises(function() shell.hooks.on("shell.started", "f") end, nil, "on with a string")
    raises(function() shell.hooks.before("shell.started") end, nil, "before without a function")
    -- before is for the cancellable hooks only
    raises(function() shell.hooks.before("shell.started", f) end, "not cancellable", "before on shell.started")
    raises(function() shell.hooks.before("overview.shown", f) end, "not cancellable", "before on overview.shown")
    raises(function() shell.hooks.before("session.mode-changed", f) end, "not cancellable", "before on a non-cancellable hook")
  end)

  case("shell.hooks.CANCEL", function()
    eq(type(shell.hooks.CANCEL), "userdata", "type")
    truthy(rawequal(shell.hooks.CANCEL, shell.hooks.CANCEL), "unique")
    truthy(shell.hooks.CANCEL ~= nil, "not nil")
    raises(function() shell.hooks.CANCEL.x = 1 end, nil, "indexing the sentinel for writing")
  end)

  case("shell.hooks.emit_* are for the tests", function()
    raises(function() shell.hooks.emit_after("overview.hidden", {}) end, nil, "emit_after in an extension")
    raises(function() shell.hooks.emit_before("overview.showing", {}) end, nil, "emit_before in an extension")
    raises(function() shell.hooks.emit_after("no.such-hook", {}) end, nil, "emit_after of an unknown hook")
  end)

  case("a gated hook needs its capability", function()
    local entry = listed["search.query"]

    truthy(entry, "search.query is declared")
    eq(entry.capability, "search", "capability of search.query in the list")
    raises(function() shell.hooks.on("search.query", function() end) end,
           "hook 'search.query' requires the \"search\" capability", "on('search.query')")
    -- the check comes before the one for the phase
    raises(function() shell.hooks.before("search.query", function() end) end, nil, "before('search.query')")
  end)

  -- -------------------------------------------------------------------
  -- Listeners on everything the catalogue says exists
  -- -------------------------------------------------------------------
  for _, hook in ipairs(catalogue) do
    if listed[hook.name] and not hook.gated then
      connected[#connected + 1] = hook
      local ok, conn = pcall(shell.hooks.on, hook.name, function(payload)
        record(hook, "after", payload)
      end)
      T.report("on(" .. hook.name .. ")", ok, not ok and tostring(conn) or nil)
      if ok then
        case("on returns a connection: " .. hook.name, function()
          eq(type(conn), "userdata", "type")
          eq(type(conn.disconnect), "function", "disconnect")
        end)
      end

      if hook.cancellable then
        calls[hook.name] = { A = 0, B = 0 }
        local okA, connA = pcall(shell.hooks.before, hook.name, function(payload)
          calls[hook.name].A = calls[hook.name].A + 1
          record(hook, "before", payload)
          return behave(hook.name)
        end)
        T.report("before(" .. hook.name .. ")", okA, not okA and tostring(connA) or nil)
        before_a[hook.name] = connA
        -- registered after A: it is not called when A cancels
        shell.hooks.before(hook.name, function()
          calls[hook.name].B = calls[hook.name].B + 1
        end)
      else
        case("before is refused: " .. hook.name, function()
          raises(function() shell.hooks.before(hook.name, function() end) end, "not cancellable", "before")
        end)
      end
    end
  end

  -- a connection that is disconnected stops the delivery
  local ping_conn = shell.hooks.on("test.step", function(p)
    if p.step == "ping" then
      pings = pings + 1
    end
  end)

  steps {
    ["payload-types"] = function(p)
      eq(math.type(p.n), "integer", "an integer")
      eq(p.n, 5, "the integer")
      eq(math.type(p.big), "integer", "a large integer")
      eq(p.big, 1 << 40, "the large integer")
      eq(math.type(p.neg), "integer", "a negative integer")
      eq(math.type(p.f), "float", "a number with a fraction")
      eq(p.f, 1.5, "the fraction")
      eq(p.b, true, "a boolean")
      eq(p.s, "str\u{e9}", "a string")
      eq(#p.list, 2, "the length of a sequence")
      eq(p.list[1], "a", "a sequence is 1-based")
      eq(p.list[2], "b", "the second item")
      eq(p.map.x, 1, "a table keyed by strings")
      eq(p.map.y, "z", "a value of a table")
      eq(p.step, "payload-types", "the step itself")
      eq(next(p.empty), nil, "an empty table")
    end,

    reset = function()
      events = {}
      for _, c in pairs(calls) do
        c.A, c.B = 0, 0
      end
    end,

    arm = function(p)
      if p.mode == "none" then
        armed[p.hook] = nil
      else
        armed[p.hook] = { mode = p.mode, amend = p.amend, value = p.value }
      end
    end,

    ["expect-events"] = function(p)
      local got = {}

      for _, event in ipairs(events) do
        if event.key:sub(1, #p.prefix) == p.prefix then
          got[#got + 1] = event.key
        end
      end
      eq(table.concat(got, " "), table.concat(p.list, " "), "the events " .. p.prefix .. "*")
    end,

    ["expect-last"] = function(p)
      local payload = last[p.event]

      truthy(payload, "no event " .. p.event)
      eq(payload[p.key], p.value, p.event .. "." .. p.key)
    end,

    ["expect-calls"] = function(p)
      eq(calls[p.hook].A, p.a, "calls of the first before listener of " .. p.hook)
      eq(calls[p.hook].B, p.b, "calls of the second before listener of " .. p.hook)
    end,

    ["expect-count"] = function(p)
      eq(total[p.event] or 0, p.n, "number of events " .. p.event .. " since the start")
    end,

    -- every declared hook of the catalogue got its sample from the script
    samples = function()
      local n = 0

      for _, hook in ipairs(connected) do
        local after = last[hook.name .. ":after"]

        truthy(after, hook.name .. ": no after event")
        truthy(same(after, hook.sample),
               hook.name .. ": the payload is not the sample that was emitted")
        n = n + 1
        if hook.cancellable then
          truthy(last[hook.name .. ":before"], hook.name .. ": no before event")
          truthy(same(last[hook.name .. ":before"], hook.sample), hook.name .. ": the before payload differs")
        end
      end
      truthy(n > 0, "no hook was connected")
    end,

    ["expect-pings"] = function(p)
      eq(pings, p.n, "calls of the listener")
    end,

    disconnect = function()
      ping_conn:disconnect()
    end,

    ["disconnect-before"] = function(p)
      before_a[p.hook]:disconnect()
    end,
  }

  T.done()
end

return ext
