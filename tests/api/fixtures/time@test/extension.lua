-- Fixture of tests/api/time.lua: every function, field and error of shell.time
-- and the hook time.timezone-changed (docs/api/core.md, docs/api/hooks.md,
-- Since 1.1). Only the UTC fields are compared with numbers: the local zone of
-- the CI session is whatever the container has, so the local results are
-- checked against each other and against the UTC ones.
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

local function is_integer(v)
  return math.type(v) == "integer"
end

function ext.enable()
  case("shell.time is a table of functions", function()
    eq(type(shell.time), "table", "shell.time")
    for _, name in ipairs({ "now", "monotonic", "date", "timestamp", "format", "timezone" }) do
      eq(type(shell.time[name]), "function", "shell.time." .. name)
    end
    eq(shell.time.no_such_function, nil, "shell.time.no_such_function")
  end)

  case("a metadata with shell-api 1.1 loads and the API is 1.1 or later", function()
    eq(shell.api_version.major, 1, "major")
    truthy(shell.api_version.minor >= 1, "minor")
    eq(shell.extension.metadata["shell-api"], "1.1", "shell-api")
  end)

  case("now and monotonic", function()
    local a, b = shell.time.now(), shell.time.now()
    eq(math.type(a), "float", "now is a float")
    truthy(b >= a, "now goes forward")
    truthy(a > 1.7e9, "now is a time since the epoch: " .. a)
    local m, n = shell.time.monotonic(), shell.time.monotonic()
    eq(math.type(m), "float", "monotonic is a float")
    truthy(n >= m, "monotonic goes forward")
    truthy(shell.time.date().year >= 2024, "the clock of the session")
  end)

  case("date{utc = true} fields", function()
    local d = shell.time.date{ at = 1700000000, utc = true }
    eq(d.year, 2023, "year"); eq(d.month, 11, "month"); eq(d.day, 14, "day")
    eq(d.hour, 22, "hour"); eq(d.minute, 13, "minute"); eq(d.second, 20, "second")
    eq(d.microsecond, 0, "microsecond")
    eq(d.weekday, 2, "weekday (ISO, Monday is 1)")
    eq(d.yearday, 318, "yearday")
    eq(d.timezone, "UTC", "timezone"); eq(d.offset, 0, "offset"); eq(d.dst, false, "dst")
    for _, key in ipairs({ "year", "month", "day", "hour", "minute", "second", "microsecond",
                           "weekday", "yearday", "offset" }) do
      truthy(is_integer(d[key]), key .. " is an integer")
    end
    eq(shell.time.date{ at = 0, utc = true }.weekday, 4, "the epoch was a Thursday")
    eq(shell.time.date{ at = 1700432000, utc = true }.weekday, 7, "Sunday is 7")
    eq(shell.time.date{ at = 1700432000 + 86400, utc = true }.weekday, 1, "then Monday is 1")
    eq(shell.time.date{ at = 951782400, utc = true }.yearday, 60, "leap day")
    eq(shell.time.date{ at = 1700000000.25, utc = true }.microsecond, 250000, "fraction")
    local before = shell.time.date{ at = -1.5, utc = true }
    eq(before.year, 1969, "before the epoch"); eq(before.second, 58, "second before the epoch")
    eq(before.microsecond, 500000, "microsecond before the epoch")
  end)

  case("date () fields of the local time are in range", function()
    local checked = 0
    for t = 1672531200, 1735689600, 86400 * 21 + 3601 do
      local d = shell.time.date{ at = t }
      truthy(d.year >= 2023 and d.year <= 2025, "year " .. d.year)
      truthy(d.month >= 1 and d.month <= 12, "month " .. d.month)
      truthy(d.day >= 1 and d.day <= 31, "day " .. d.day)
      truthy(d.hour >= 0 and d.hour <= 23, "hour " .. d.hour)
      truthy(d.minute >= 0 and d.minute <= 59, "minute " .. d.minute)
      truthy(d.second >= 0 and d.second <= 59, "second " .. d.second)
      truthy(d.microsecond >= 0 and d.microsecond <= 999999, "microsecond")
      truthy(d.weekday >= 1 and d.weekday <= 7, "weekday " .. d.weekday)
      truthy(d.yearday >= 1 and d.yearday <= 366, "yearday " .. d.yearday)
      eq(type(d.timezone), "string", "timezone")
      truthy(#d.timezone > 0, "timezone is not empty")
      truthy(is_integer(d.offset) and math.abs(d.offset) <= 14 * 3600, "offset " .. tostring(d.offset))
      eq(type(d.dst), "boolean", "dst")
      -- the local fields and the UTC ones describe one instant
      local u = shell.time.date{ at = t, utc = true }
      eq(shell.time.timestamp{ year = d.year, month = d.month, day = d.day, hour = d.hour,
                               minute = d.minute, second = d.second, utc = true } - d.offset,
         t, "local fields minus the offset")
      eq(u.offset, 0, "utc offset")
      checked = checked + 1
    end
    truthy(checked > 25, "instants checked: " .. checked)
  end)

  case("date () without at is now", function()
    local d, e = shell.time.date(), shell.time.date{}
    local t = shell.time.now()
    truthy(math.abs(shell.time.timestamp(d) - t) < 5, "date () is now")
    truthy(math.abs(shell.time.timestamp(e) - t) < 5, "date {} is now")
    eq(shell.time.date(nil).year, d.year, "date (nil)")
  end)

  case("timestamp is the inverse of date", function()
    for _, t in ipairs({ 0, 1, -1, 59, 86399, 951782400, 1700000000, 1720000000, 1704067199,
                         -62135596800, 253402300799, -2208988800 }) do
      local u = shell.time.date{ at = t, utc = true }
      u.utc = true
      eq(shell.time.timestamp(u), t, "utc " .. t)
    end
    -- the local fields, a few instants of the year (the long run is the step
    -- "roundtrip"; the repeated hour of a clock change is in the GTest)
    for _, t in ipairs({ 1672531200, 1680000000, 1690000000, 1700000000, 1720000000 }) do
      local d = shell.time.date{ at = t }
      eq(shell.time.timestamp(d), t, "local " .. t)
      eq(math.floor(shell.time.timestamp(d)), math.floor(t), "floor")
    end
    local t = 1700000000.123456
    local d = shell.time.date{ at = t }
    truthy(math.abs(shell.time.timestamp(d) - t) < 1e-6, "microseconds are kept")
    eq(math.floor(shell.time.timestamp(d)), math.floor(t), "floor of timestamp (date (t))")
    eq(math.type(shell.time.timestamp{ year = 2023, month = 11, day = 14, utc = true }), "integer",
       "integer without microseconds")
    eq(shell.time.timestamp{ year = 1970, month = 1, day = 1, utc = true }, 0, "the epoch")
    eq(shell.time.timestamp{ year = 1970, month = 1, day = 1, second = 1, microsecond = 500000, utc = true },
       1.5, "float with microseconds")
  end)

  case("format", function()
    eq(shell.time.format("%Y-%m-%d %H:%M:%S", { at = 1700000000, utc = true }), "2023-11-14 22:13:20", "full")
    eq(shell.time.format("%H:%M", { at = 3600 * 5 + 60 * 7, utc = true }), "05:07", "H:M")
    eq(shell.time.format("100%% %Z", { at = 0, utc = true }), "100% UTC", "percent and zone")
    eq(shell.time.format("text only"), "text only", "no specifier")
    eq(shell.time.format("%f", { at = 1700000000.25, utc = true }), "250000", "microseconds")
    eq(shell.time.format("%Y"), tostring(shell.time.date().year), "%Y is the year of date ()")
    local d = shell.time.date{ at = 1720000000 }
    eq(shell.time.format("%Y-%m-%d %H:%M:%S", { at = 1720000000 }),
       string.format("%04d-%02d-%02d %02d:%02d:%02d", d.year, d.month, d.day, d.hour, d.minute, d.second),
       "local format agrees with date")
    eq(type(shell.time.format("%x")), "string", "%x")
    eq(type(shell.time.format("%A %B")), "string", "%A %B")
  end)

  case("timezone", function()
    local tz = shell.time.timezone()
    eq(type(tz), "string", "timezone ()")
    truthy(#tz > 0, "timezone () is not empty")
    eq(shell.time.timezone(), tz, "timezone () is stable")
  end)

  case("errors name the field", function()
    raises(function() shell.time.date{ at = "now" } end, "field 'at' must be a number", "at is a string")
    raises(function() shell.time.date{ utc = 1 } end, "field 'utc' must be a boolean", "utc is a number")
    raises(function() shell.time.date{ zone = "UTC" } end, "unknown field 'zone'", "unknown field")
    raises(function() shell.time.date(5) end, "options table expected", "a number as the options")
    raises(function() shell.time.date{ at = 1e20 } end, "field 'at' is out of range", "at too large")
    raises(function() shell.time.date{ at = 0 / 0 } end, "field 'at' is out of range", "at is NaN")
    raises(function() shell.time.format("%Y", { when = 1 }) end, "unknown field 'when'", "format: unknown field")
    raises(function() shell.time.format("%Y", { at = {} }) end, "field 'at' must be a number", "format: at")
    raises(function() shell.time.timestamp{} end, "missing required field 'year'", "no year")
    raises(function() shell.time.timestamp{ year = 2020, month = 1 } end, "missing required field 'day'", "no day")
    raises(function() shell.time.timestamp{ year = 2020, month = 13, day = 1 } end,
           "field 'month' is out of range", "month 13")
    raises(function() shell.time.timestamp{ year = 2023, month = 2, day = 29, utc = true } end,
           "field 'day' is out of range", "29 February 2023")
    raises(function() shell.time.timestamp{ year = 2020, month = 1, day = 1, hour = 24 } end,
           "field 'hour' is out of range", "hour 24")
    raises(function() shell.time.timestamp{ year = "2020", month = 1, day = 1 } end,
           "field 'year' must be an integer", "year is a string")
    raises(function() shell.time.timestamp{ year = 2020.5, month = 1, day = 1 } end,
           "field 'year' must be an integer", "year is not an integer")
    raises(function() shell.time.timestamp{ year = 2020, month = 1, day = 1, utc = "yes" } end,
           "field 'utc' must be a boolean", "timestamp: utc")
    raises(function() shell.time.timestamp(1700000000) end, "options table expected", "timestamp of a number")
    raises(function() shell.time.format() end, "string expected", "format without a format")
    raises(function() shell.time.format(5) end, "string expected", "format of a number")
    raises(function() shell.time.format("%Q") end, "invalid format string", "an unknown specifier")
    raises(function() shell.time.format("%Y\0%m") end, "invalid format string", "a NUL in the format")
    raises(function() shell.time.format("\xff") end, "invalid format string", "a format that is not UTF-8")
    raises(function() shell.time.format(string.rep("x", 1025)) end, "too long", "a format of 1025 bytes")
  end)

  case("time.timezone-changed is declared", function()
    local found
    for _, hook in ipairs(shell.hooks.list()) do
      if hook.name == "time.timezone-changed" then
        found = hook
      end
    end
    truthy(found, "listed by shell.hooks.list ()")
    eq(found.cancellable, false, "cancellable")
    eq(found.schema, "timezone:s", "schema")
    truthy(found.doc and #found.doc > 0, "doc")
    raises(function() shell.hooks.before("time.timezone-changed", function() end) end, nil,
           "before on an informational hook")
  end)

  -- -------------------------------------------------------------------
  -- The steps of the script
  -- -------------------------------------------------------------------
  local zones = {}
  local conn

  steps {
    -- the clocks against a timer of the main loop
    clock = function(_, done)
      local wall, mono = shell.time.now(), shell.time.monotonic()

      shell.timer.after{ ms = 120, callback = function()
        case("the clocks advance with a timer", function()
          local dw, dm = shell.time.now() - wall, shell.time.monotonic() - mono
          truthy(dw >= 0.1 and dw < 5, "wall clock advanced by " .. dw)
          truthy(dm >= 0.1 and dm < 5, "monotonic clock advanced by " .. dm)
          truthy(math.abs(dw - dm) < 0.5, "both clocks advanced alike")
        end)
        done()
      end }
      return "later"
    end,

    -- every hour of two years, local: timestamp (date (t)) == t, the offset of the
    -- date () result picks the instant of a repeated hour (its own callback: a
    -- callback has a time budget)
    roundtrip = function()
      local n = 0
      for t = 1672531200, 1735689600, 11 * 3600 + 1 do
        local d = shell.time.date{ at = t }
        eq(shell.time.timestamp(d), t, "local " .. t)
        n = n + 1
      end
      truthy(n > 1000, "instants: " .. n)
    end,

    -- the script emits the hook (a privileged state: the bridge, not the watch of
    -- /etc/localtime, which the GTest of the subproject covers)
    ["tz-arm"] = function()
      conn = shell.hooks.on("time.timezone-changed", function(p)
        zones[#zones + 1] = p
      end)
    end,

    ["tz-check"] = function(p)
      eq(#zones, 1, "one emission")
      eq(zones[1].timezone, p.timezone, "payload timezone")
      eq(type(zones[1].timezone), "string", "type of the payload")
      conn:disconnect()
    end,
  }

  T.done()
end

return ext
