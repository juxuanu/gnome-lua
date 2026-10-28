-- Fixture of tests/api/dbus.lua: every function, field, property and error
-- kind of docs/api/dbus.md that can be tried against the session bus of the
-- test (the bus daemon, and this extension's own names and objects: the test
-- bus has no session services, so every peer other than the daemon is one the
-- extension exports itself). enable () runs the checks that are
-- synchronous and starts the asynchronous ones, and reports T.done () when the
-- last of them has finished; the script of the test asks for the part that
-- makes errors on purpose with a step (tests/api/README.md).
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

-- ---------------------------------------------------------------------
-- Asynchronous cases: async_case (name, function (finish) ... end) runs the
-- body at once; the case is over when the body (or a callback it set up)
-- calls finish (true) or finish (false, message). T.done () goes out when
-- enable () has run all its synchronous cases and every async case is over.
-- ---------------------------------------------------------------------
local pending = 0
local sync_over = false
local all_done = false

local function maybe_done()
  if sync_over and pending == 0 and not all_done then
    all_done = true
    T.done()
  end
end

local function async_case(name, body)
  local over = false
  local watchdog

  pending = pending + 1

  local function finish(ok, message)
    if over then
      return
    end
    over = true
    if watchdog then
      watchdog:cancel()
    end
    T.report(name, ok, not ok and tostring(message) or nil)
    pending = pending - 1
    maybe_done()
  end

  local ok, err = pcall(body, finish)
  if not ok then
    finish(false, err)
  end
  if not over then
    watchdog = shell.timer.after{ ms = 10000, callback = function()
      finish(false, "timed out")
    end }
  end
end

-- a callback that fails the case instead of raising
local function guard(finish, fn)
  return function(...)
    local ok, err = pcall(fn, ...)
    if not ok then
      finish(false, err)
    end
  end
end

-- calls on_ok () when pred () is true, fails the case after `ms`
local function poll(finish, pred, ms, what, on_ok)
  local waited = 0

  shell.timer.every{ ms = 20, callback = function()
    waited = waited + 20
    local ok, result = pcall(pred)
    if not ok then
      finish(false, result)
      return false
    end
    if result then
      guard(finish, on_ok)()
      return false
    end
    if waited >= ms then
      finish(false, "timed out waiting for " .. what)
      return false
    end
  end }
end

-- items: list of function (go); runs them one after the other
local function chain(finish, items)
  local i = 0

  local function step()
    i = i + 1
    local item = items[i]

    if not item then
      finish(true)
      return
    end
    local ok, err = pcall(item, step)
    if not ok then
      finish(false, err)
    end
  end

  step()
end

local ext = {}

-- What the extension keeps (the objects are owned by the extension anyway)
local kept = {}

local OWN_NAME = "org.example.ApiTest"
local OWN_PATH = "/org/example/ApiTest"
local OWN_IFACE = "org.example.ApiTest"

local XML = [[
<node>
  <interface name="org.example.ApiTest">
    <method name="Hello"><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
    <method name="Pair"><arg type="i" direction="out"/><arg type="s" direction="out"/></method>
    <method name="Sum"><arg type="ai" direction="in"/><arg type="x" direction="out"/></method>
    <method name="Dict"><arg type="a{sv}" direction="in"/><arg type="a{sv}" direction="out"/></method>
    <method name="Nothing"/>
    <method name="Refuse"><arg type="s" direction="out"/></method>
    <method name="Declared"/>
    <method name="Boom"/>
    <method name="TooMany"><arg type="s" direction="out"/></method>
    <signal name="Changed"><arg type="s" name="what"/><arg type="u" name="count"/></signal>
    <property name="Name" type="s" access="read"/>
    <property name="Count" type="u" access="read"/>
    <property name="Tags" type="as" access="read"/>
    <property name="Info" type="a{sv}" access="read"/>
  </interface>
</node>]]

-- A second, small object for the replies with several values and with a
-- sequence (a peer that exists on any bus: this extension itself)
local MULTI_NAME = "org.example.ApiTest.Multi"
local MULTI_PATH = "/org/example/ApiTest/Multi"
local MULTI_IFACE = "org.example.ApiTest.Multi"

local MULTI_XML = [[
<node>
  <interface name="org.example.ApiTest.Multi">
    <method name="Quad">
      <arg type="s" direction="out"/><arg type="s" direction="out"/>
      <arg type="s" direction="out"/><arg type="s" direction="out"/>
    </method>
    <method name="Words"><arg type="as" direction="out"/></method>
  </interface>
</node>]]

-- on_error of a call that must succeed: the case fails with the error
local function fails(finish)
  return function(err)
    finish(false, string.format("the call failed: %s: %s", tostring(err.name), tostring(err.message)))
  end
end

function ext.enable()
  local bus = shell.dbus.session()
  local daemon = bus:proxy{
    name = "org.freedesktop.DBus", path = "/org/freedesktop/DBus", interface = "org.freedesktop.DBus",
  }

  -- -------------------------------------------------------------------
  -- Entry points
  -- -------------------------------------------------------------------
  case("shell.dbus functions, the buses", function()
    eq(type(shell.dbus), "table", "shell.dbus")
    for _, name in ipairs({ "session", "system", "variant" }) do
      eq(type(shell.dbus[name]), "function", "shell.dbus." .. name)
    end
    eq(shell.dbus.no_such_function, nil, "shell.dbus.no_such_function")
    eq(type(bus), "userdata", "a Bus is a handle")
    eq(bus.alive, true, "alive")
    eq(tostring(bus):find("shell.Bus", 1, true) ~= nil, true, "tostring")
    truthy(rawequal(bus, shell.dbus.session()), "the session bus is one handle")
    local system = shell.dbus.system()
    eq(system.alive, true, "the system bus, alive")
    truthy(not rawequal(bus, system), "the two buses are two handles")
    eq(bus.no_such_property, nil, "unknown properties are nil")
    raises(function() bus.alive = false end, nil, "assigning alive")
  end)

  case("shell.dbus.variant", function()
    local v = shell.dbus.variant("u", 5)
    eq(type(v), "userdata", "a typed value")
    raises(function() shell.dbus.variant("ii", 1) end, nil, "two types")
    raises(function() shell.dbus.variant("mi", 1) end, nil, "a maybe type")
    raises(function() shell.dbus.variant("h", 1) end, nil, "a file descriptor")
    raises(function() shell.dbus.variant("y", 256) end, "range", "an out of range byte")
    raises(function() shell.dbus.variant("u", "x") end, nil, "a wrong value")
    raises(function() shell.dbus.variant("u") end, nil, "no value")
    raises(function() shell.dbus.variant(5, 5) end, nil, "no signature")
    local nested = shell.dbus.variant("a{sv}", { a = 1, b = shell.dbus.variant("y", 2) })
    eq(type(nested), "userdata", "a dictionary with a typed value in it")
    -- outside a `v` position a typed value is an error
    raises(function()
      daemon:call{ method = "NameHasOwner", signature = "s", args = { shell.dbus.variant("s", "x") } }
    end, nil, "a typed value for an 's'")
  end)

  -- -------------------------------------------------------------------
  -- Misuse raises before anything is sent
  -- -------------------------------------------------------------------
  case("proxy: fields, names, reserved names", function()
    local function p(fields)
      return function() return bus:proxy(fields) end
    end
    local ok = { name = "org.example.Service", path = "/org/example", interface = "org.example.Iface" }
    local function with(key, value)
      local t = {}
      for k, v in pairs(ok) do
        t[k] = v
      end
      t[key] = value
      return t
    end

    local proxy = bus:proxy(ok)
    eq(proxy.name, "org.example.Service", "name")
    eq(proxy.path, "/org/example", "path")
    eq(proxy.interface, "org.example.Iface", "interface")
    eq(proxy.bus, "session", "bus")
    eq(shell.dbus.system():proxy(ok).bus, "system", "bus of a proxy of the system bus")
    eq(proxy.alive, true, "alive")
    -- a unique name is a name too
    eq(bus:proxy(with("name", ":1.5")).name, ":1.5", "unique name")
    raises(p(with("name", nil)), "name", "no name")
    raises(p(with("path", nil)), "path", "no path")
    raises(p(with("interface", nil)), "interface", "no interface")
    raises(p(with("name", "nodots")), "name", "an invalid name")
    raises(p(with("path", "relative")), "path", "an invalid path")
    raises(p(with("interface", "nodots")), "interface", "an invalid interface")
    raises(p(with("name", 5)), "name", "a number for the name")
    raises(p(with("extra", true)), "extra", "an unknown field")
    -- the shell's own names are reached through shell.*
    raises(p(with("name", "org.gnome.Shell")), "reserved", "org.gnome.Shell")
    raises(p(with("name", "org.gnome.Shell.Extensions")), "reserved", "org.gnome.Shell.Extensions")
    raises(p(with("interface", "org.gnome.Shell.Extensions")), "reserved", "an org.gnome.Shell interface")
    eq(bus:proxy(with("name", "org.gnome.Mutter.DisplayConfig")).name, "org.gnome.Mutter.DisplayConfig",
       "other org.gnome names are allowed on a proxy")
  end)

  case("call: arguments are checked before sending", function()
    local proxy = bus:proxy{ name = "org.example.Service", path = "/org/example", interface = "org.example.Iface" }

    raises(function() proxy:call{} end, "method", "no method")
    raises(function() proxy:call{ method = "no good" } end, "method", "an invalid method name")
    raises(function() proxy:call{ method = 5 } end, "method", "a number for the method")
    raises(function() proxy:call{ method = "M", args = { 1 } } end, "signature", "args without a signature")
    raises(function() proxy:call{ method = "M", signature = "s", args = {} } end, nil, "too few args")
    raises(function() proxy:call{ method = "M", signature = "s", args = { "a", "b" } } end, nil, "too many args")
    raises(function() proxy:call{ method = "M", signature = "s" } end, nil, "no args for 's'")
    raises(function() proxy:call{ method = "M", signature = "", args = { 1 } } end, nil, "args with an empty signature")
    raises(function() proxy:call{ method = "M", signature = "ms", args = { "x" } } end, nil, "a maybe signature")
    raises(function() proxy:call{ method = "M", signature = "h", args = { 1 } } end, nil, "a file descriptor")
    raises(function() proxy:call{ method = "M", signature = "(s", args = { "x" } } end, nil, "a broken signature")
    raises(function() proxy:call{ method = "M", signature = "y", args = { 256 } } end, "range", "a byte out of range")
    raises(function() proxy:call{ method = "M", signature = "u", args = { -1 } } end, "range", "a negative 'u'")
    raises(function() proxy:call{ method = "M", signature = "i", args = { 1.5 } } end, nil, "a fraction for an 'i'")
    raises(function() proxy:call{ method = "M", signature = "s", args = { 5 } } end, nil, "a number for an 's'")
    raises(function() proxy:call{ method = "M", signature = "b", args = { 1 } } end, nil, "a number for a 'b'")
    raises(function() proxy:call{ method = "M", signature = "o", args = { "not a path" } } end, nil, "a bad path")
    raises(function() proxy:call{ method = "M", signature = "s", args = { nil } } end, nil, "nil as an argument")
    raises(function() proxy:call{ method = "M", signature = "v", args = { {} , 1 } } end, nil, "two args for a 'v'")
    raises(function() proxy:call{ method = "M", timeout_ms = 0 } end, "timeout_ms", "a timeout of 0")
    raises(function() proxy:call{ method = "M", timeout_ms = 60001 } end, "timeout_ms", "a timeout over a minute")
    raises(function() proxy:call{ method = "M", timeout_ms = 1.5 } end, "timeout_ms", "a fractional timeout")
    raises(function() proxy:call{ method = "M", on_reply = "x" } end, "on_reply", "a string for on_reply")
    raises(function() proxy:call{ method = "M", on_error = 5 } end, "on_error", "a number for on_error")
    raises(function() proxy:call{ method = "M", extra = true } end, "extra", "an unknown field")
    raises(function() proxy:call("M") end, nil, "a positional call")
  end)

  case("get_property, get_all, set_property: arguments", function()
    local proxy = bus:proxy{ name = "org.example.Service", path = "/org/example", interface = "org.example.Iface" }

    raises(function() proxy:get_property{} end, "name", "no name")
    raises(function() proxy:get_property{ name = "no good" } end, "name", "an invalid name")
    raises(function() proxy:get_property{ name = "P", extra = 1 } end, "extra", "an unknown field")
    raises(function() proxy:get_all{ name = "P" } end, "name", "a name for get_all")
    raises(function() proxy:set_property{ name = "P", value = 1 } end, "signature", "no signature")
    raises(function() proxy:set_property{ name = "P", signature = "u" } end, "value", "no value")
    raises(function() proxy:set_property{ name = "P", signature = "uu", value = 1 } end, nil, "two types")
    raises(function() proxy:set_property{ name = "P", signature = "mu", value = 1 } end, nil, "a maybe type")
    raises(function() proxy:set_property{ name = "P", signature = "u", value = "x" } end, nil, "a string for a 'u'")
    raises(function() proxy:set_property{ signature = "u", value = 1 } end, "name", "no name for set_property")
  end)

  case("on_signal, on_properties_changed: arguments", function()
    local proxy = bus:proxy{ name = "org.example.Service", path = "/org/example", interface = "org.example.Iface" }

    raises(function() proxy:on_signal("Sig") end, nil, "no function")
    raises(function() proxy:on_signal("no good", function() end) end, nil, "an invalid signal name")
    raises(function() proxy:on_signal(5, function() end) end, nil, "a number for the name")
    raises(function() proxy:on_properties_changed() end, nil, "no function")
    raises(function() proxy:on_properties_changed("x") end, nil, "a string for the function")
    local c = proxy:on_signal(nil, function() end)
    eq(type(c.disconnect), "function", "a Connection")
    eq(c:is_connected(), true, "connected")
    c:disconnect()
    eq(c:is_connected(), false, "disconnected")
    c:disconnect()
  end)

  case("watch_name, own_name: arguments", function()
    raises(function() bus:watch_name{ name = "org.example.X" } end, nil, "no callbacks")
    raises(function() bus:watch_name{ on_vanished = function() end } end, "name", "no name")
    raises(function() bus:watch_name{ name = "nodots", on_vanished = function() end } end, "name", "a bad name")
    raises(function() bus:watch_name{ name = "org.example.X", on_vanished = 5 } end, "on_vanished", "a number")
    raises(function() bus:watch_name{ name = "org.example.X", on_vanished = function() end, extra = 1 } end,
           "extra", "an unknown field")
    raises(function() bus:own_name{} end, "name", "no name")
    raises(function() bus:own_name{ name = "nodots" } end, "name", "a bad name")
    raises(function() bus:own_name{ name = ":1.5" } end, "name", "a unique name")
    raises(function() bus:own_name{ name = "org.example.X", replace = "yes" } end, "replace", "a string for replace")
    raises(function() bus:own_name{ name = "org.example.X", allow_replacement = 1 } end, "allow_replacement", "a number")
    raises(function() bus:own_name{ name = "org.example.X", on_lost = 5 } end, "on_lost", "a number")
    raises(function() bus:own_name{ name = "org.example.X", extra = 1 } end, "extra", "an unknown field")
    -- the namespaces of the system and of the shell
    raises(function() bus:own_name{ name = "org.gnome.Foo" } end, "reserved", "org.gnome.Foo")
    raises(function() bus:own_name{ name = "org.gnome.Shell" } end, "reserved", "org.gnome.Shell")
    raises(function() bus:own_name{ name = "org.freedesktop.Foo" } end, "reserved", "org.freedesktop.Foo")
  end)

  case("export: arguments", function()
    local function export(extra, xml)
      local fields = { path = "/org/example/Invalid", interface_xml = xml or XML }
      for k, v in pairs(extra or {}) do
        fields[k] = v
      end
      return function() return bus:export(fields) end
    end
    local function xml(body, name)
      return string.format('<node><interface name="%s">%s</interface></node>', name or "org.example.Invalid", body)
    end

    raises(function() bus:export{ interface_xml = XML } end, "path", "no path")
    raises(function() bus:export{ path = "/a" } end, "interface_xml", "no interface_xml")
    raises(export({ path = "relative" }), "path", "a bad path")
    raises(export({ extra = true }), "extra", "an unknown field")
    raises(export(nil, "<node"), "interface_xml", "broken XML")
    raises(export(nil, "<node/>"), "interface", "no interface")
    raises(export(nil, '<node><interface name="a.b"/><interface name="a.c"/></node>'), "interface",
           "two interfaces")
    raises(export(nil, '<node><interface name="a.b"/><node name="child"/></node>'), "node", "a child node")
    raises(export(nil, xml('<property name="W" type="u" access="readwrite"/>')), "read", "a readwrite property")
    raises(export(nil, xml('<property name="W" type="u" access="write"/>')), "read", "a write property")
    raises(export(nil, xml('<method name="M"><arg type="h" direction="in"/></method>')), nil,
           "a file descriptor argument")
    raises(export(nil, xml("", "org.gnome.Foo")), nil, "org.gnome.Foo")
    raises(export(nil, xml("", "org.gnome.Shell.Foo")), nil, "org.gnome.Shell.Foo")
    raises(export(nil, xml("", "org.freedesktop.Foo")), nil, "org.freedesktop.Foo")
    raises(export({ methods = { Nope = function() end } }), "Nope", "a function for a method that is not declared")
    raises(export({ methods = { Hello = 5 } }), "Hello", "a number for a method")
    raises(export({ methods = 5 }), "methods", "a number for methods")
    raises(export({ properties = { Nope = 1 } }), "Nope", "a property that is not declared")
    raises(export({ properties = { Count = "x" } }), nil, "a string for a 'u' property")
    raises(export({ properties = { Count = -1 } }), nil, "a negative 'u' property")
    raises(export({ properties = 5 }), "properties", "a number for properties")
  end)

  -- -------------------------------------------------------------------
  -- The services of the session bus of the test
  -- -------------------------------------------------------------------
  -- 1. the bus daemon: a reply comes from the main loop, as the arguments
  async_case("a reply arrives from the main loop, as arguments", function(finish)
    local returned = false
    local call

    call = daemon:call{
      method = "NameHasOwner", signature = "s", args = { "org.freedesktop.DBus" },
      on_error = fails(finish),
      on_reply = guard(finish, function(...)
        truthy(returned, "on_reply ran before call () returned")
        eq(select("#", ...), 1, "number of values in the reply")
        eq((...), true, "NameHasOwner")
        eq(call.pending, false, "pending in the reply")
        finish(true)
      end),
    }
    returned = true
    eq(type(call), "userdata", "a call is a handle")
    eq(call.pending, true, "pending after call ()")
    eq(tostring(call):find("shell.DBusCall", 1, true) ~= nil, true, "tostring")
    kept.first_call = call
  end)

  async_case("a reply with no values, with several, with a sequence", function(finish)
    local peer = bus:proxy{ name = "org.freedesktop.DBus", path = "/org/freedesktop/DBus",
                            interface = "org.freedesktop.DBus.Peer" }
    local multi = bus:proxy{ name = MULTI_NAME, path = MULTI_PATH, interface = MULTI_IFACE }

    -- the peer with several out arguments is an object of this extension:
    -- the test bus has no session services to ask
    kept.multi_obj = bus:export{
      path = MULTI_PATH, interface_xml = MULTI_XML,
      methods = {
        Quad = function() return "one", "two", "three", "four" end,
        Words = function() return { "alpha", "beta", "gamma" } end,
      },
    }
    kept.multi_name = bus:own_name{ name = MULTI_NAME, on_acquired = function()
      kept.multi_acquired = true
    end }

    chain(finish, {
      function(go)
        peer:call{ method = "Ping", on_error = fails(finish), on_reply = guard(finish, function(...)
          eq(select("#", ...), 0, "Ping () has no values")
          go()
        end) }
      end,
      function(go)
        daemon:call{ method = "ListNames", on_error = fails(finish), on_reply = guard(finish, function(...)
          eq(select("#", ...), 1, "ListNames () has one value")
          local names = ...
          eq(type(names), "table", "an 'as' is a sequence")
          local found = false
          for i, name in ipairs(names) do
            eq(type(name), "string", "name " .. i)
            found = found or name == "org.freedesktop.DBus"
          end
          truthy(found, "org.freedesktop.DBus is in ListNames ()")
          go()
        end) }
      end,
      -- the name has to be there before we call it
      function(go)
        poll(finish, function() return kept.multi_acquired end, 5000, "the name of the Multi object", go)
      end,
      function(go)
        multi:call{ method = "Quad", on_error = fails(finish), on_reply = guard(finish, function(...)
          eq(select("#", ...), 4, "Quad () returns its four strings as four values")
          local one, two, three, four = ...
          eq(one, "one", "value 1")
          eq(two, "two", "value 2")
          eq(three, "three", "value 3")
          eq(four, "four", "value 4")
          go()
        end) }
      end,
      function(go)
        multi:call{ method = "Words", on_error = fails(finish), on_reply = guard(finish, function(...)
          eq(select("#", ...), 1, "Words () has one value")
          local words = ...
          eq(type(words), "table", "an 'as' is a sequence")
          eq(#words, 3, "the number of words")
          eq(words[1], "alpha", "words[1]")
          eq(words[2], "beta", "words[2]")
          eq(words[3], "gamma", "words[3]")
          go()
        end) }
      end,
    })
  end)

  async_case("errors: name, message and remote", function(finish)
    local nobody = bus:proxy{ name = "org.example.Nobody", path = "/", interface = "org.example.Nobody" }
    local errors = {}

    local function expect(key, name, remote, go)
      return guard(finish, function(err)
        eq(type(err), "table", key .. ": the error")
        eq(err.name, name, key .. ": name")
        eq(type(err.message), "string", key .. ": message")
        eq(err.remote, remote, key .. ": remote")
        errors[key] = true
        go()
      end)
    end

    chain(finish, {
      function(go)
        nobody:call{ method = "Hello", on_reply = guard(finish, function() error("replied") end),
                     on_error = expect("service", "org.freedesktop.DBus.Error.ServiceUnknown", true, go) }
      end,
      function(go)
        daemon:call{ method = "GetNameOwner", signature = "s", args = { "org.example.NobodyOwnsThis" },
                     on_reply = guard(finish, function() error("replied") end),
                     on_error = expect("owner", "org.freedesktop.DBus.Error.NameHasNoOwner", true, go) }
      end,
      function(go)
        daemon:call{ method = "NoSuchMethod",
                     on_reply = guard(finish, function() error("replied") end),
                     on_error = expect("method", "org.freedesktop.DBus.Error.UnknownMethod", true, go) }
      end,
      function(go)
        daemon:call{ method = "NameHasOwner", signature = "u", args = { 5 },
                     on_reply = guard(finish, function() error("replied") end),
                     on_error = guard(finish, function(err)
                       truthy(err.remote, "the daemon refuses an 'u' where it wants an 's'")
                       go()
                     end) }
      end,
      -- the system bus of the test may not exist: a local error with a standard name
      function(go)
        local sys = shell.dbus.system():proxy{ name = "org.example.Nobody", path = "/", interface = "org.example.Nobody" }

        sys:call{ method = "Hello", timeout_ms = 3000, on_reply = guard(finish, function() error("replied") end),
                  on_error = guard(finish, function(err)
                    eq(type(err.name), "string", "system: name")
                    truthy(err.name:find("^org%.freedesktop%.DBus%.Error%."), "system: a standard name: " .. err.name)
                    go()
                  end) }
      end,
    })
  end)

  async_case("cancel, and a call without callbacks", function(finish)
    local ran = false
    local cancelled = daemon:call{ method = "ListNames", on_reply = function() ran = true end,
                                   on_error = function() ran = true end }

    eq(cancelled.pending, true, "pending")
    cancelled:cancel()
    eq(cancelled.pending, false, "pending after cancel ()")
    cancelled:cancel()

    local fire = daemon:call{ method = "ListNames" }
    eq(type(fire), "userdata", "a call without callbacks still returns its handle")
    fire:cancel()

    shell.timer.after{ ms = 400, callback = guard(finish, function()
      eq(ran, false, "a cancelled call calls nobody")
      finish(true)
    end) }
  end)

  async_case("properties of the bus daemon, signals of the bus daemon", function(finish)
    local seen = {}

    -- NameOwnerChanged, for any name of the daemon proxy
    kept.sig = daemon:on_signal("NameOwnerChanged", function(...)
      seen[#seen + 1] = table.pack(...)
    end)
    kept.sig_all = daemon:on_signal(nil, function(name)
      seen.all = (seen.all or 0) + 1
      seen.last = name
    end)

    chain(finish, {
      -- the subscriptions are in place when this reply is (same bus, in order)
      function(go)
        daemon:call{ method = "GetId", on_error = fails(finish), on_reply = guard(finish, function(id)
          eq(type(id), "string", "GetId ()")
          go()
        end) }
      end,
      function(go)
        kept.watch_events = {}
        kept.watch = bus:watch_name{
          name = OWN_NAME,
          on_appeared = guard(finish, function(owner)
            kept.watch_events[#kept.watch_events + 1] = "appeared"
            kept.watch_owner = owner
          end),
          on_vanished = guard(finish, function(...)
            eq(select("#", ...), 0, "on_vanished () has no arguments")
            kept.watch_events[#kept.watch_events + 1] = "vanished"
          end),
        }
        eq(kept.watch:is_connected(), true, "a name watch is a connection")
        -- nobody has the name yet: on_vanished at once, from the main loop
        poll(finish, function() return kept.watch_events[1] == "vanished" end, 3000,
             "on_vanished for a name without an owner", go)
      end,
      -- the name: on_acquired, acquired, NameOwnerChanged, on_appeared
      function(go)
        local acquired_events = 0

        kept.owner = bus:own_name{
          name = OWN_NAME, allow_replacement = true,
          on_acquired = guard(finish, function(...)
            eq(select("#", ...), 0, "on_acquired () has no arguments")
            acquired_events = acquired_events + 1
          end),
          on_lost = guard(finish, function()
            kept.lost = (kept.lost or 0) + 1
          end),
        }
        eq(kept.owner.name, OWN_NAME, "name")
        eq(kept.owner.acquired, false, "acquired before the main loop ran")
        eq(kept.owner.alive, true, "alive")
        eq(tostring(kept.owner):find("shell.BusName", 1, true) ~= nil, true, "tostring")
        poll(finish, function() return kept.owner.acquired end, 5000, "the name to be acquired", function()
          eq(acquired_events, 1, "on_acquired calls")
          poll(finish, function() return kept.watch_events[2] == "appeared" and seen.all end, 3000,
               "on_appeared and NameOwnerChanged", function()
            local own
            for _, args in ipairs(seen) do
              if args[2] == OWN_NAME then
                own = args
              end
            end
            truthy(own, "NameOwnerChanged for " .. OWN_NAME)
            eq(own[1], "NameOwnerChanged", "the signal name comes first")
            eq(own.n, 4, "NameOwnerChanged has the signal name and three arguments")
            eq(own[3], "", "old owner")
            truthy(own[4]:sub(1, 1) == ":", "new owner is a unique name")
            eq(kept.watch_owner, own[4], "on_appeared gets the unique name of the owner")
            eq(type(seen.last), "string", "the signal name of an on_signal (nil, fn) callback")
            go()
          end)
        end)
      end,
    })
  end)

  -- 2. this extension's own object, called through the bus
  async_case("export: methods, properties, signals", function(finish)
    local calls = {}

    kept.obj = bus:export{
      path = OWN_PATH, interface_xml = XML,
      methods = {
        Hello = function(who)
          calls.hello = who
          return "hello " .. who
        end,
        Pair = function() return 5, "five" end,
        Sum = function(list)
          local total = 0
          for _, n in ipairs(list) do
            total = total + n
          end
          return total
        end,
        Dict = function(d)
          local out = {}
          for k, v in pairs(d) do
            out[k] = v
          end
          out.seen = true
          return out
        end,
        Nothing = function() end,
        Refuse = function() return nil, "org.example.Error.NotReady", "try later" end,
        Boom = function() error("boom") end,
        TooMany = function() return "a", "b" end,
      },
      properties = { Name = "api", Count = 2, Tags = { "x", "y" } },
    }
    eq(kept.obj.path, OWN_PATH, "path")
    eq(kept.obj.interface, OWN_IFACE, "interface")
    eq(kept.obj.alive, true, "alive")
    eq(tostring(kept.obj):find("shell.DBusObject", 1, true) ~= nil, true, "tostring")

    local me = bus:proxy{ name = OWN_NAME, path = OWN_PATH, interface = OWN_IFACE }
    kept.me = me

    local function ask(key, method, signature, args, ok, bad)
      return function(go)
        me:call{
          method = method, signature = signature, args = args,
          on_reply = guard(finish, function(...)
            if not ok then
              error(key .. " should have failed")
            end
            ok(...)
            go()
          end),
          on_error = guard(finish, function(err)
            if not bad then
              error(key .. " failed: " .. err.name .. ": " .. err.message)
            end
            bad(err)
            go()
          end),
        }
      end
    end

    chain(finish, {
      -- the name has to be there before we call it
      function(go)
        poll(finish, function() return kept.owner and kept.owner.acquired end, 5000, "the name", go)
      end,
      ask("Hello", "Hello", "s", { "you" }, function(...)
        eq(select("#", ...), 1, "Hello: values")
        eq((...), "hello you", "Hello")
        eq(calls.hello, "you", "the method got its argument")
      end),
      ask("Pair", "Pair", nil, nil, function(...)
        eq(select("#", ...), 2, "Pair: one value per out argument")
        local a, b = ...
        eq(a, 5, "first out argument")
        eq(b, "five", "second out argument")
      end),
      ask("Sum", "Sum", "ai", { { 1, 2, 3 } }, function(total)
        eq(total, 6, "an array in, an int64 out")
      end),
      ask("Dict", "Dict", "a{sv}", { { n = 1, s = "x", u = shell.dbus.variant("u", 7), list = { 1, 2 } } }, function(d)
        eq(d.n, 1, "n")
        eq(d.s, "x", "s")
        eq(d.u, 7, "u")
        eq(#d.list, 2, "list")
        eq(d.seen, true, "the method's own key")
      end),
      ask("Nothing", "Nothing", nil, nil, function(...)
        eq(select("#", ...), 0, "Nothing: no values")
      end),
      ask("Refuse", "Refuse", nil, nil, nil, function(err)
        eq(err.name, "org.example.Error.NotReady", "the error name of `return nil, name, message`")
        eq(err.message, "try later", "its message")
        eq(err.remote, true, "an error from a peer is remote")
      end),
      ask("Declared", "Declared", nil, nil, nil, function(err)
        eq(err.name, "org.freedesktop.DBus.Error.UnknownMethod", "a method without a function")
      end),
      ask("Undeclared", "NoSuchMethod", nil, nil, nil, function(err)
        eq(err.name, "org.freedesktop.DBus.Error.UnknownMethod", "a method that is not in the XML")
      end),
      -- properties: the initial values, the defaults, the writes
      function(go)
        me:get_property{ name = "Name", on_reply = guard(finish, function(v)
          eq(v, "api", "Name")
          go()
        end) }
      end,
      function(go)
        me:get_all{ on_reply = guard(finish, function(props)
          eq(props.Name, "api", "Name")
          eq(props.Count, 2, "Count")
          eq(#props.Tags, 2, "Tags")
          eq(props.Tags[2], "y", "Tags[2]")
          eq(type(props.Info), "table", "Info (absent from `properties`: the default of a{sv})")
          eq(next(props.Info), nil, "the default of a{sv} is empty")
          go()
        end) }
      end,
      function(go)
        me:get_property{ name = "Nope", on_reply = guard(finish, function() error("replied") end),
                         on_error = guard(finish, function(err)
          truthy(err.remote, "an unknown property")
          go()
        end) }
      end,
      function(go)
        local changed, signal
        local pc = me:on_properties_changed(guard(finish, function(c, invalidated)
          changed = { c, invalidated }
        end))
        local sc = me:on_signal("Changed", guard(finish, function(name, what, count)
          signal = { name, what, count }
        end))

        -- let the subscriptions reach the daemon (a call after them is answered after them)
        daemon:call{ method = "GetId", on_error = fails(finish), on_reply = guard(finish, function()
          kept.obj:set_property("Count", 7)
          kept.obj:set_property("Name", "renamed")
          kept.obj:emit_signal{ name = "Changed", args = { "count", 7 } }

          poll(finish, function() return changed and signal end, 3000, "PropertiesChanged and Changed", function()
            eq(type(changed[1]), "table", "changed")
            eq(type(changed[2]), "table", "invalidated")
            truthy(changed[1].Count == 7 or changed[1].Name == "renamed", "a changed property")
            eq(signal[1], "Changed", "signal name")
            eq(signal[2], "count", "signal argument 1")
            eq(signal[3], 7, "signal argument 2")
            me:get_property{ name = "Count", on_reply = guard(finish, function(v)
              eq(v, 7, "Count after set_property")
              pc:disconnect()
              sc:disconnect()
              go()
            end) }
          end)
        end) }
      end,
      -- misuse of the object
      function(go)
        raises(function() kept.obj:set_property("Nope", 1) end, "Nope", "an unknown property")
        raises(function() kept.obj:set_property("Count", "x") end, nil, "a string for a 'u'")
        raises(function() kept.obj:set_property("Count", -1) end, nil, "a negative 'u'")
        raises(function() kept.obj:set_property("Count") end, nil, "no value")
        raises(function() kept.obj:emit_signal{ name = "Nope" } end, "Nope", "an unknown signal")
        raises(function() kept.obj:emit_signal{ name = "Changed", args = { "x" } } end, nil, "too few arguments")
        raises(function() kept.obj:emit_signal{ name = "Changed", args = { "x", "y" } } end, nil, "a string for a 'u'")
        raises(function() kept.obj:emit_signal{ args = {} } end, "name", "no name")
        raises(function() kept.obj:emit_signal{ name = "Changed", args = { "x", 1 }, extra = 1 } end, "extra",
               "an unknown field")
        go()
      end,
      -- destroy () unexports: the same call now fails, destroy () is idempotent
      function(go)
        kept.obj:destroy()
        kept.obj:destroy()
        eq(kept.obj.alive, false, "alive after destroy ()")
        raises(function() return kept.obj.path end, "no longer valid", "a destroyed object")
        raises(function() kept.obj:set_property("Count", 1) end, "no longer valid", "set_property on a destroyed object")
        me:call{ method = "Hello", signature = "s", args = { "x" },
                 on_reply = guard(finish, function() error("replied") end),
                 on_error = guard(finish, function(err)
                   eq(err.remote, true, "the call to an unexported object fails remotely")
                   go()
                 end) }
      end,
      -- release () gives the name back: the watch sees it go
      function(go)
        kept.owner:release()
        kept.owner:release()
        kept.owner:destroy()
        eq(kept.owner.alive, false, "alive after release ()")
        poll(finish, function() return kept.watch_events[3] == "vanished" end, 5000,
             "on_vanished after release ()", function()
          eq(kept.lost, nil, "on_lost is not called for a name we gave back")
          kept.watch:disconnect()
          eq(kept.watch:is_connected(), false, "disconnected")
          go()
        end)
      end,
    })
  end)

  -- -------------------------------------------------------------------
  -- Steps: errors on purpose. The script runs it inside c.capture_log, and
  -- expects the two errors it counts (expect_errors = 2).
  -- -------------------------------------------------------------------
  steps{
    -- a Lua error and a wrong return count in a method: the peer gets
    -- org.freedesktop.DBus.Error.Failed without a traceback, the extension an error each
    ["method-errors"] = function(p, done)
      local obj = bus:export{
        path = "/org/example/ApiTest/Errors", interface_xml = XML,
        methods = {
          Boom = function() error("secret-traceback-marker") end,
          TooMany = function() return "a", "b" end,
        },
      }
      local me = bus:proxy{ name = "org.example.ApiTest.Errors", path = "/org/example/ApiTest/Errors",
                            interface = OWN_IFACE }
      local replies = 0
      local owner

      local function reply(what)
        return function(err)
          local ok, e = pcall(function()
            eq(err.name, "org.freedesktop.DBus.Error.Failed", what .. ": name")
            truthy(not err.message:find("secret-traceback-marker", 1, true), what .. ": no Lua message for the peer")
            truthy(not err.message:find("traceback", 1, true), what .. ": no traceback for the peer")
          end)
          T.report("method error: " .. what, ok, not ok and tostring(e) or nil)
          replies = replies + 1
          if replies == 2 then
            obj:destroy()
            owner:release()
            done()
          end
        end
      end

      owner = bus:own_name{ name = "org.example.ApiTest.Errors", on_acquired = function()
        me:call{ method = "Boom", on_reply = function() T.report("method error: Boom", false, "replied") end,
                 on_error = reply("a Lua error") }
        me:call{ method = "TooMany", on_reply = function() T.report("method error: TooMany", false, "replied") end,
                 on_error = reply("the wrong number of values") }
      end }
      return "later"
    end,
  }

  sync_over = true
  maybe_done()
end

-- disable () is optional; this one checks that the objects are still usable in it
function ext.disable()
  case("disable () runs before the objects are destroyed", function()
    eq(kept.watch ~= nil, true, "the watch exists")
    eq(kept.sig:is_connected(), true, "a subscription is connected in disable ()")
  end)
end

return ext
