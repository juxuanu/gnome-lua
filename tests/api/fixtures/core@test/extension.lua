-- Fixture of tests/api/core.lua: every function, field and error kind of
-- docs/api/core.md (and the rules of docs/api/README.md that concern it)
-- except shell.hooks, which tests/api/hooks.lua covers. enable () runs the
-- checks that are synchronous; the script of the test asks for the others with
-- steps (tests/api/README.md).
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

-- Timers still pending when the extension is disabled: nothing may run later
local pending = {}

function ext.enable()
  local settings = shell.settings.extension()

  -- -------------------------------------------------------------------
  -- shell.api_version, require("shell")
  -- -------------------------------------------------------------------
  case("shell.api_version", function()
    local v = shell.api_version

    eq(type(v), "table", "type")
    eq(math.type(v.major), "integer", "major")
    eq(v.major, 1, "major")
    eq(math.type(v.minor), "integer", "minor")
    eq(v.minor, 2, "minor (API 1.2)")
    raises(function() v.major = 2 end, nil, "assigning api_version.major")
    raises(function() v.minor = 5 end, nil, "assigning api_version.minor")
    raises(function() v.patch = 1 end, nil, "adding a field to api_version")
    eq(v.major, 1, "major after the writes")
    local keys = {}
    for key in pairs(v) do
      keys[#keys + 1] = key
    end
    table.sort(keys)
    eq(table.concat(keys, ","), "major,minor", "fields of api_version")
  end)

  case("require('shell') is the shell table", function()
    truthy(rawequal(require("shell"), shell), "require('shell') == shell")
  end)

  -- -------------------------------------------------------------------
  -- shell.extension
  -- -------------------------------------------------------------------
  case("shell.extension fields", function()
    local e = shell.extension

    eq(type(e), "table", "type")
    eq(e.uuid, "core@test", "uuid")
    eq(type(e.dir), "string", "dir")
    eq(e.dir:sub(1, 1), "/", "dir is absolute")
    truthy(e.dir:match("/core@test$"), "dir ends with the uuid: " .. e.dir)
    eq(type(e.metadata), "table", "metadata")
    eq(type(e.capabilities), "table", "capabilities")
  end)

  case("shell.extension.metadata is the parsed metadata.json", function()
    local m = shell.extension.metadata

    eq(m.uuid, "core@test", "uuid")
    eq(m.name, "API conformance: core", "name")
    eq(m["shell-api"], "1", "shell-api")
    eq(m["settings-schema"], "org.gnome-lua.test.core", "settings-schema")
    eq(math.type(m.version), "integer", "an integer is an integer")
    eq(m.version, 1, "version")
    eq(math.type(m.ratio), "float", "a fraction is a float")
    eq(m.ratio, 0.5, "ratio")
    eq(m.nothing, nil, "a null is nil")
    eq(m["custom-field"].text, "caf\u{e9}", "a string with an escape")
    eq(#m["custom-field"].nested, 3, "length of an array")
    eq(m["custom-field"].nested[2], 2, "an array is a 1-based sequence")
    eq(m["custom-field"].nested[3].deep, true, "a nested object")
    eq(m.capabilities[1], "settings", "an array of strings")
  end)

  case("shell.extension is read-only", function()
    local e = shell.extension

    raises(function() e.uuid = "other" end, nil, "assigning uuid")
    raises(function() e.extra = 1 end, nil, "adding a field")
    raises(function() e.metadata.name = "other" end, nil, "assigning a metadata field")
    raises(function() e.metadata.extra = 1 end, nil, "adding a metadata field")
    raises(function() e.metadata["custom-field"].nested[1] = 9 end, nil, "assigning in a nested array")
    raises(function() e.metadata["custom-field"].text = "x" end, nil, "assigning in a nested object")
    raises(function() e.capabilities[1] = "dbus" end, nil, "assigning a capability")
    raises(function() e.capabilities[2] = "dbus" end, nil, "adding a capability")
    eq(e.uuid, "core@test", "uuid after the writes")
    eq(e.metadata.name, "API conformance: core", "name after the writes")
  end)

  case("shell.extension.capabilities", function()
    local caps = shell.extension.capabilities
    local seen = {}

    eq(#caps, 1, "number of capabilities")
    eq(caps[1], "settings", "first capability")
    for i, cap in ipairs(caps) do
      seen[#seen + 1] = i .. "=" .. cap
    end
    eq(table.concat(seen, ","), "1=settings", "ipairs")
    seen = {}
    for key, cap in pairs(caps) do
      seen[#seen + 1] = key .. "=" .. cap
    end
    eq(table.concat(seen, ","), "1=settings", "pairs")
  end)

  -- -------------------------------------------------------------------
  -- shell.log
  -- -------------------------------------------------------------------
  case("shell.log functions", function()
    eq(type(shell.log), "table", "shell.log")
    for _, name in ipairs({ "debug", "info", "warning", "error", "fatal" }) do
      eq(type(shell.log[name]), "function", "shell.log." .. name)
    end
  end)

  case("shell.log.* raise on misuse, never on the format", function()
    raises(function() shell.log.info() end, nil, "a call without a message")
    raises(function() shell.log.info({}) end, nil, "a table as the message")
    raises(function() shell.log.info("%d", "x") end, nil, "a format that does not fit its argument")
    raises(function() shell.log.debug("%d") end, nil, "a missing argument")
    raises(function() shell.log.fatal() end, nil, "fatal without a message")
    raises(function() shell.log.fatal("%d", "x") end, nil, "fatal with a format that does not fit")
  end)

  -- -------------------------------------------------------------------
  -- shell.timer: errors (the behaviour is the step "timers")
  -- -------------------------------------------------------------------
  case("shell.timer misuse", function()
    local function f() end

    raises(function() shell.timer.after{ callback = f } end, "ms", "after without ms")
    raises(function() shell.timer.after{ ms = 10 } end, "callback", "after without callback")
    raises(function() shell.timer.after{ ms = 10, callback = f, bogus = 1 } end, "bogus", "after, unknown field")
    raises(function() shell.timer.after{ ms = "soon", callback = f } end, "ms", "after, ms is a string")
    raises(function() shell.timer.after{ ms = 1.5, callback = f } end, "ms", "after, ms is a fraction")
    raises(function() shell.timer.after{ ms = -1, callback = f } end, "ms", "after, negative ms")
    raises(function() shell.timer.after{ ms = 10, callback = 5 } end, "callback", "after, callback is a number")
    raises(function() shell.timer.after("soon", f) end, nil, "after(string, f)")
    raises(function() shell.timer.after(10) end, nil, "after(ms) without function")
    raises(function() shell.timer.after(-5, f) end, nil, "after(-5, f)")
    raises(function() shell.timer.every{ ms = 0, callback = f } end, "ms", "every{ms = 0}")
    raises(function() shell.timer.every{ callback = f } end, "ms", "every without ms")
    raises(function() shell.timer.every{ ms = 10, callback = f, extra = true } end, "extra", "every, unknown field")
    raises(function() shell.timer.every(0, f) end, nil, "every(0, f)")
    raises(function() shell.timer.idle{ } end, "callback", "idle without callback")
    raises(function() shell.timer.idle{ callback = f, ms = 5 } end, "ms", "idle takes no ms")
    raises(function() shell.timer.idle{ callback = f, bogus = 5 } end, "bogus", "idle, unknown field")
    raises(function() shell.timer.after() end, nil, "after()")
    raises(function() shell.timer.after(nil) end, nil, "after(nil)")
  end)

  -- -------------------------------------------------------------------
  -- shell.settings
  -- -------------------------------------------------------------------
  case("shell.settings.extension returns the settings of the schema", function()
    eq(type(settings), "userdata", "a handle")
    truthy(settings == shell.settings.extension(), "the same object is the same handle")
    eq(settings.alive, true, "alive")
  end)

  case("shell.Settings:keys", function()
    local keys = settings:keys()

    eq(type(keys), "table", "keys()")
    eq(table.concat(keys, ","), "big,count,dict,flag,mode,name,names,pair,ratio", "keys, sorted")
  end)

  case("shell.Settings:get and set", function()
    eq(settings:get("flag"), false, "default of a boolean")
    eq(settings:get("count"), 3, "default of an int")
    eq(math.type(settings:get("count")), "integer", "an int is an integer")
    eq(settings:get("big"), 0, "default of an int64")
    eq(settings:get("ratio"), 0.5, "default of a double")
    eq(settings:get("name"), "x", "default of a string")
    eq(#settings:get("names"), 0, "default of a string array")
    local pair = settings:get("pair")
    eq(pair[1], "x", "first of a tuple")
    eq(pair[2], 1, "second of a tuple")
    eq(next(settings:get("dict")), nil, "default of a dictionary")

    settings:set("flag", true)
    eq(settings:get("flag"), true, "flag after set")
    settings:set("count", 7)
    eq(settings:get("count"), 7, "count after set")
    settings:set("big", 1 << 40)
    eq(settings:get("big"), 1 << 40, "int64 after set")
    settings:set("ratio", 0.25)
    eq(settings:get("ratio"), 0.25, "ratio after set")
    settings:set("name", "caf\u{e9}")
    eq(settings:get("name"), "caf\u{e9}", "name after set")
    settings:set("mode", "b")
    eq(settings:get("mode"), "b", "mode after set")
    settings:set("names", { "a", "b" })
    local names = settings:get("names")
    eq(#names, 2, "length of the array after set")
    eq(names[2], "b", "second element after set")
    settings:set("pair", { "y", 2 })
    pair = settings:get("pair")
    eq(pair[1] .. pair[2], "y2", "tuple after set")
    settings:set("dict", { a = 1, b = "s" })
    local dict = settings:get("dict")
    eq(dict.a, 1, "dictionary, integer")
    eq(dict.b, "s", "dictionary, string")
  end)

  case("shell.Settings:reset", function()
    settings:set("count", 9)
    settings:reset("count")
    eq(settings:get("count"), 3, "count after reset")
    settings:reset("flag")
    settings:reset("big")
    settings:reset("ratio")
    settings:reset("name")
    settings:reset("mode")
    settings:reset("names")
    settings:reset("pair")
    settings:reset("dict")
    eq(settings:get("flag"), false, "flag after reset")
    eq(settings:get("mode"), "a", "mode after reset")
    eq(#settings:get("names"), 0, "names after reset")
  end)

  case("shell.Settings typed getters and setters", function()
    settings:set_boolean("flag", true)
    eq(settings:get_boolean("flag"), true, "get_boolean")
    settings:set_int("count", 4)
    eq(settings:get_int("count"), 4, "get_int")
    settings:set_int("big", 1 << 41)
    eq(settings:get_int("big"), 1 << 41, "get_int of an int64")
    settings:set_double("ratio", 1)
    eq(settings:get_double("ratio"), 1.0, "get_double (an integer is accepted)")
    eq(math.type(settings:get_double("ratio")), "float", "a double is a float")
    settings:set_double("ratio", 0.75)
    eq(settings:get_double("ratio"), 0.75, "get_double")
    settings:set_string("name", "typed")
    eq(settings:get_string("name"), "typed", "get_string")
    settings:set_string("mode", "c")
    eq(settings:get_string("mode"), "c", "get_string of a key with choices")
    settings:set_strv("names", { "p", "q", "r" })
    local names = settings:get_strv("names")
    eq(#names, 3, "length of get_strv")
    eq(names[3], "r", "get_strv")
    settings:set_strv("names", {})
    eq(#settings:get_strv("names"), 0, "an empty array")
    for _, key in ipairs({ "flag", "count", "big", "ratio", "name", "mode", "names" }) do
      settings:reset(key)
    end
  end)

  case("shell.Settings errors", function()
    raises(function() settings:get("nope") end, "nope", "get of an unknown key")
    raises(function() settings:set("nope", 1) end, "nope", "set of an unknown key")
    raises(function() settings:reset("nope") end, "nope", "reset of an unknown key")
    raises(function() settings:get_int("nope") end, "nope", "get_int of an unknown key")
    raises(function() settings:on_changed("nope", function() end) end, "nope", "on_changed of an unknown key")
    raises(function() settings:get() end, nil, "get without key")
    raises(function() settings:get_int("name") end, "name", "get_int of a string key")
    raises(function() settings:get_string("count") end, "count", "get_string of an int key")
    raises(function() settings:get_boolean("count") end, "count", "get_boolean of an int key")
    raises(function() settings:get_double("count") end, "count", "get_double of an int key")
    raises(function() settings:get_strv("name") end, "name", "get_strv of a string key")
    raises(function() settings:set_int("name", 1) end, "name", "set_int on a string key")
    raises(function() settings:set_boolean("flag", 1) end, nil, "set_boolean with a number")
    raises(function() settings:set_string("name", 5) end, nil, "set_string with a number")
    raises(function() settings:set_int("count", "5") end, nil, "set_int with a string")
    raises(function() settings:set_strv("names", "a") end, nil, "set_strv with a string")
    raises(function() settings:set_strv("names", { "a", 1 }) end, nil, "set_strv with a number in it")
    raises(function() settings:set_int("count", 4.5) end, nil, "set_int with a fraction")
    -- a value the schema refuses is an error, never a warning
    raises(function() settings:set_int("count", 99) end, "count", "set_int out of the range")
    raises(function() settings:set("count", -1) end, "count", "set out of the range")
    raises(function() settings:set_double("ratio", 2.0) end, "ratio", "set_double out of the range")
    raises(function() settings:set_string("mode", "zzz") end, "mode", "set_string outside the choices")
    raises(function() settings:set("pair", { "only-one" }) end, nil, "a tuple that is too short")
    eq(settings:get_int("count"), 3, "count is unchanged after the refused writes")
    eq(settings:get_string("mode"), "a", "mode is unchanged after the refused writes")
  end)

  case("shell.settings.open", function()
    local ds = shell.settings.open("org.gnome.desktop.interface")

    eq(type(ds), "userdata", "a handle")
    truthy(ds == shell.settings.open("org.gnome.desktop.interface"), "one handle per schema")
    local found = false
    for _, key in ipairs(ds:keys()) do
      if key == "show-battery-percentage" then
        found = true
      end
    end
    truthy(found, "keys() of another schema")
    eq(type(ds:get_boolean("show-battery-percentage")), "boolean", "a key of another schema")
    raises(function() shell.settings.open("org.gnome-lua.test.no-such-schema") end, "org.gnome-lua.test.no-such-schema",
           "open of a schema that is not installed")
    raises(function() shell.settings.open("org.gnome.desktop.app-folders.folder") end, nil,
           "open of a relocatable schema")
    raises(function() shell.settings.open() end, nil, "open without a schema")
    raises(function() shell.settings.open({}) end, nil, "open with a table")
  end)

  -- -------------------------------------------------------------------
  -- shell.gettext
  -- -------------------------------------------------------------------
  case("shell.gettext", function()
    eq(shell.gettext("Hello"), "Hello", "gettext of an untranslated string")
    eq(shell.gettext(""), "", "gettext of the empty string is the empty string")
    eq(shell.ngettext("one file", "many files", 1), "one file", "ngettext, n = 1")
    eq(shell.ngettext("one file", "many files", 2), "many files", "ngettext, n = 2")
    eq(shell.ngettext("one file", "many files", 0), "many files", "ngettext, n = 0")
    eq(shell.pgettext("menu", "Open"), "Open", "pgettext of an untranslated string")
    eq(shell.pgettext("menu", ""), "", "pgettext of the empty string")
    raises(function() shell.gettext() end, nil, "gettext without argument")
    raises(function() shell.gettext({}) end, nil, "gettext of a table")
    raises(function() shell.ngettext("a", "b") end, nil, "ngettext without n")
    raises(function() shell.ngettext("a", "b", "x") end, nil, "ngettext with a string for n")
    raises(function() shell.pgettext("ctx") end, nil, "pgettext without msgid")
  end)

  case("_, ngettext and pgettext are globals", function()
    truthy(rawequal(_, shell.gettext), "_ is shell.gettext")
    truthy(rawequal(ngettext, shell.ngettext), "ngettext is shell.ngettext")
    truthy(rawequal(pgettext, shell.pgettext), "pgettext is shell.pgettext")
    eq(_("Hello"), "Hello", "_()")
  end)

  -- -------------------------------------------------------------------
  -- Unknown names are nil, not errors (README: feature detection)
  -- -------------------------------------------------------------------
  case("absent functions are nil", function()
    eq(shell.no_such_module, nil, "shell.no_such_module")
    eq(shell.log.no_such_function, nil, "shell.log.no_such_function")
    eq(shell.timer.no_such_function, nil, "shell.timer.no_such_function")
  end)

  -- -------------------------------------------------------------------
  -- Timers that are still pending when the extension is disabled
  -- -------------------------------------------------------------------
  pending[#pending + 1] = shell.timer.after{ ms = 150, callback = function() end }
  pending[#pending + 1] = shell.timer.every{ ms = 10, callback = function() end }
  pending[#pending + 1] = shell.timer.idle{ callback = function() end }
  case("pending timers are active", function()
    eq(pending[1].active, true, "after")
    eq(pending[2].active, true, "every")
  end)

  -- -------------------------------------------------------------------
  -- The steps of the script
  -- -------------------------------------------------------------------
  local conns = {}
  local watch = { all = {}, one = {} }

  steps {
    -- shell.log: the script reads the log
    log = function()
      shell.log.debug("d %d %.1f", 1, 2.5)
      shell.log.info("i %s", "x")
      shell.log.warning("w %s", "y")
      shell.log.error("e %q", "z")
      shell.log("alias %s", "a")
      shell.log.info("100%% sure")
      shell.log.info("no format arguments")
      shell.log.debug("%s", "last")
    end,

    -- shell.log.fatal (1.1): logs like error, then raises the message as is
    ["log-fatal"] = function()
      local err = raises(function() shell.log.fatal("fatal %d", 7) end, "fatal 7", "shell.log.fatal")
      eq(err, "fatal 7", "the error is the formatted message, as is")
      local reached = false
      raises(function()
        shell.log.fatal("stops here")
        reached = true
      end, "stops here", "shell.log.fatal in a function")
      eq(reached, false, "the code after shell.log.fatal does not run")
      -- an error of a pcall can be caught and the extension goes on
      eq(select(2, pcall(shell.log.fatal, "caught")), "caught", "pcall (shell.log.fatal)")
    end,

    -- What an extension logs on purpose is never fatal. The script does NOT
    -- capture the log for this step: the shell runs with G_DEBUG=fatal-warnings,
    -- which aborts it on any warning or critical that is not exempt.
    ["log-uncaptured"] = function()
      shell.log.warning("on purpose: an uncaptured warning")
      shell.log.error("on purpose: an uncaptured error")
      pcall(shell.log.fatal, "on purpose: an uncaptured fatal")
      shell.log.warning("%s", "on purpose: and the shell is still here")
    end,

    -- shell.timer
    timers = function(_, done)
      local n = { once = 0, pos = 0, zero = 0, idle = 0, cancelled = 0, stop = 0, every = 0 }
      local t_once = shell.timer.after{ ms = 20, callback = function(...)
        n.once = n.once + 1
        n.args = select("#", ...)
      end }
      local t_pos = shell.timer.after(20, function() n.pos = n.pos + 1 end)
      local t_zero = shell.timer.after{ ms = 0, callback = function() n.zero = n.zero + 1 end }
      local t_idle = shell.timer.idle{ callback = function() n.idle = n.idle + 1 end }
      local t_cancel = shell.timer.after{ ms = 20, callback = function() n.cancelled = n.cancelled + 1 end }
      local t_stop = shell.timer.every{ ms = 10, callback = function()
        n.stop = n.stop + 1
        if n.stop == 3 then
          return false
        end
        return true
      end }
      local t_every = shell.timer.every(10, function() n.every = n.every + 1 end)

      eq(type(t_once), "userdata", "a timer is a handle")
      eq(t_once.active, true, "a pending timer is active")
      eq(t_every.active, true, "a pending repeating timer is active")
      t_cancel:cancel()
      eq(t_cancel.active, false, "a cancelled timer is not active")
      raises(function() t_once.active = false end, nil, "assigning Timer.active")

      shell.timer.after{ ms = 300, callback = function()
        local seen = 0

        case("timers fire", function()
          eq(n.once, 1, "after{} fires once")
          eq(n.args, 0, "a timer callback gets no argument")
          eq(n.pos, 1, "after(ms, f) fires once")
          eq(n.zero, 1, "after{ms = 0} fires once")
          eq(n.idle, 1, "idle fires once")
          eq(t_once.active, false, "a timer that fired is not active")
          eq(t_idle.active, false, "an idle callback that ran is not active")
          eq(t_zero.active, false, "a timer of 0 ms that fired is not active")
          eq(t_pos.active, false, "after(ms, f) that fired is not active")
        end)
        case("a cancelled timer does not fire", function()
          eq(n.cancelled, 0, "cancelled")
        end)
        case("every stops when the callback returns false", function()
          eq(n.stop, 3, "calls")
          eq(t_stop.active, false, "active")
        end)
        case("every repeats until cancelled", function()
          truthy(n.every >= 3, "calls: " .. n.every)
          eq(t_every.active, true, "active")
          t_every:cancel()
          eq(t_every.active, false, "active after cancel")
          seen = n.every
        end)

        shell.timer.after{ ms = 100, callback = function()
          case("a cancelled repeating timer does not fire again", function()
            eq(n.every, seen, "calls")
          end)
          done()
        end }
      end }

      return "later"
    end,

    -- shell.Settings:on_changed: the script writes to the schema
    ["watch-arm"] = function()
      local ds = shell.settings.open("org.gnome.desktop.interface")

      conns.all = ds:on_changed(nil, function(key, value)
        watch.all[#watch.all + 1] = { key, value }
      end)
      conns.one = ds:on_changed("show-battery-percentage", function(key, value)
        watch.one[#watch.one + 1] = { key, value }
      end)
      conns.other = ds:on_changed("clock-show-date", function(key)
        watch.other = key
      end)
      eq(type(conns.one), "userdata", "a connection is a handle")
      eq(type(conns.one.disconnect), "function", "Connection:disconnect")
    end,

    ["watch-check-1"] = function(p)
      eq(#watch.one, 1, "calls of the callback of the key")
      eq(watch.one[1][1], "show-battery-percentage", "key")
      eq(watch.one[1][2], p.value, "value")
      truthy(#watch.all >= 1, "calls of the callback of all the keys")
      eq(watch.all[#watch.all][1], "show-battery-percentage", "key, all keys")
      eq(watch.all[#watch.all][2], p.value, "value, all keys")
      eq(watch.other, nil, "the callback of another key is not called")
      conns.one:disconnect()
    end,

    ["watch-check-2"] = function(p)
      eq(#watch.one, 1, "no call after disconnect")
      truthy(#watch.all >= 2, "the other connection still gets the changes")
      eq(watch.all[#watch.all][2], p.value, "value, all keys, second change")
      conns.all:disconnect()
    end,
  }

  T.done()
end

-- Defining disable () is optional (README): this one only checks what it can
function ext.disable()
  case("disable () runs before the objects are destroyed", function()
    eq(pending[1].active or pending[1].active == false, true, "a timer can be read in disable")
  end)
end

return ext
