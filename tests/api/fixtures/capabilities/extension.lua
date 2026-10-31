-- Fixture of tests/api/capabilities.lua: the capabilities of docs/api/README.md
-- ("Capabilities") as a matrix. Every capabilities-<variant>@test directory
-- links to this file and differs only in its metadata.json: "none" declares
-- none, "all" declares all six, any other variant declares that capability
-- alone. The fixture reads its variant from its uuid, not from
-- shell.extension.capabilities (which is under test), and calls every gated
-- function with valid arguments: with the capability the call must work, and
-- without it it must raise `<function> requires the "<capability>" capability`
-- before anything runs. So each gated function is tried with its capability
-- declared alone, among all the others, and with every other capability but not
-- its own. The script runs the matrix as the step "matrix" with the URL of its
-- HTTP server.
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

-- a callback that fails the case instead of raising
local function guard(finish, fn)
  return function(...)
    local ok, err = pcall(fn, ...)
    if not ok then
      finish(false, err)
    end
  end
end

-- ---------------------------------------------------------------------
-- The variant of this copy
-- ---------------------------------------------------------------------
local CAPABILITIES = { "dbus", "settings", "search", "fs", "spawn", "network" }

local UUID = shell.extension.uuid
local VARIANT = UUID:match("^capabilities%-(.-)@test$")
local DIR = shell.extension.dir
local DATA = shell.extension.data_dir
local OWN_SCHEMA = "org.gnome-lua.test.capabilities"

-- What metadata.json declares, by the name of the variant
local declared = {}
if VARIANT == "all" then
  for _, capability in ipairs(CAPABILITIES) do
    declared[capability] = true
  end
elseif VARIANT ~= "none" then
  declared[VARIANT] = true
end

-- Paths outside the extension's own directories: next to the data directory,
-- in $XDG_DATA_HOME/gnome-lua of the test home
local OUTSIDE = DATA .. "/../" .. UUID .. ".outside"
local OUTSIDE_MISSING = DATA .. "/../" .. UUID .. ".missing"

local function fs_error(finish, what)
  return function(err)
    finish(false, what .. ": " .. err.code .. ": " .. err.message)
  end
end

-- GetId of the bus daemon. The system bus of the test may not exist: then the
-- call fails with a local error of a standard name, which shows that it ran too
local function bus_answers(bus, finish, may_be_absent)
  local daemon = bus:proxy{
    name = "org.freedesktop.DBus", path = "/org/freedesktop/DBus", interface = "org.freedesktop.DBus",
  }

  daemon:call{
    method = "GetId",
    timeout_ms = 3000,
    on_reply = guard(finish, function(id)
      eq(type(id), "string", "GetId ()")
      finish(true)
    end),
    on_error = guard(finish, function(err)
      truthy(may_be_absent and err.name:find("^org%.freedesktop%.DBus%.Error%."), "GetId (): " .. err.name)
      finish(true)
    end),
  }
end

-- ---------------------------------------------------------------------
-- The gated functions
-- ---------------------------------------------------------------------
-- One entry per function that a capability unlocks (README.md, the per-area
-- documents and the checks of the runtime). `fn` is the name the error gives,
-- `call (ctx, finish)` makes the call as an extension with the capability
-- would, with valid arguments, and returns what the function returns;
-- `check (finish, ...)` gets those values and checks that the call worked,
-- calling finish (true) now or from a callback.
local GATES = {
  -- dbus.md
  {
    capability = "dbus", fn = "shell.dbus.session()",
    call = function() return shell.dbus.session() end,
    check = function(finish, bus) bus_answers(bus, finish, false) end,
  },
  {
    capability = "dbus", fn = "shell.dbus.system()",
    call = function() return shell.dbus.system() end,
    check = function(finish, bus) bus_answers(bus, finish, true) end,
  },
  {
    capability = "dbus", fn = "shell.dbus.variant()",
    call = function() return shell.dbus.variant("u", 5) end,
    check = function(finish, value)
      eq(tostring(value), "<variant 'u'>", "the variant")
      finish(true)
    end,
  },

  -- core.md, shell.settings: a schema that is not the extension's own
  {
    capability = "settings", fn = "shell.settings.open()",
    call = function() return shell.settings.open("org.gnome.desktop.interface") end,
    check = function(finish, settings)
      eq(type(settings:get_boolean("enable-animations")), "boolean", "get_boolean ('enable-animations')")
      finish(true)
    end,
  },

  -- search.md, and the hook of hooks.md that carries what the user types
  {
    capability = "search", fn = "shell.search.add_provider",
    call = function()
      return shell.search.add_provider{
        id = "matrix", name = "Matrix",
        on_search = function(_, _, respond) respond({}) end,
        on_activate = function() end,
      }
    end,
    check = function(finish, provider, message)
      truthy(provider, "the provider (" .. tostring(message) .. ")")
      eq(provider.alive, true, "provider.alive")
      eq(provider.id, "matrix", "provider.id")
      provider:destroy()
      eq(provider.alive, false, "provider.alive after destroy ()")
      finish(true)
    end,
  },
  {
    capability = "search", fn = "hook 'search.query'",
    call = function() return shell.hooks.on("search.query", function() end) end,
    check = function(finish, connection)
      truthy(connection, "the connection")
      connection:disconnect()
      finish(true)
    end,
  },

  -- fs.md: any path outside the extension's own directories
  {
    capability = "fs", fn = "shell.fs.exists()",
    call = function() return shell.fs.exists("/") end,
    check = function(finish, exists)
      eq(exists, true, "exists ('/')")
      finish(true)
    end,
  },
  {
    capability = "fs", fn = "shell.fs.info()",
    call = function() return shell.fs.info("/") end,
    check = function(finish, info, message)
      truthy(info, "info ('/') (" .. tostring(message) .. ")")
      eq(info.type, "directory", "info ('/').type")
      finish(true)
    end,
  },
  {
    capability = "fs", fn = "shell.fs.read()",
    call = function(_, finish)
      return shell.fs.read{ path = "/etc/passwd",
        on_done = guard(finish, function(contents)
          eq(type(contents), "string", "the contents of /etc/passwd")
          finish(true)
        end),
        on_error = fs_error(finish, "read") }
    end,
    check = function(_, task) truthy(task, "the task") end,
  },
  {
    capability = "fs", fn = "shell.fs.list()",
    call = function(_, finish)
      return shell.fs.list{ path = "/",
        on_done = guard(finish, function(entries)
          truthy(#entries > 0, "entries of /")
          finish(true)
        end),
        on_error = fs_error(finish, "list") }
    end,
    check = function(_, task) truthy(task, "the task") end,
  },
  {
    -- writes the file and removes it again
    capability = "fs", fn = "shell.fs.write()",
    call = function(_, finish)
      return shell.fs.write{ path = OUTSIDE, contents = "matrix",
        on_done = guard(finish, function()
          eq(shell.fs.exists(OUTSIDE), true, "the file outside was written")
          shell.fs.remove{ path = OUTSIDE,
            on_done = guard(finish, function()
              eq(shell.fs.exists(OUTSIDE), false, "the file outside was removed")
              finish(true)
            end),
            on_error = fs_error(finish, "remove") }
        end),
        on_error = fs_error(finish, "write") }
    end,
    check = function(_, task) truthy(task, "the task") end,
  },
  {
    -- a path that does not exist: the removal gets to the file system
    capability = "fs", fn = "shell.fs.remove()",
    call = function(_, finish)
      return shell.fs.remove{ path = OUTSIDE_MISSING,
        on_done = guard(finish, function() error("removed a path that does not exist") end),
        on_error = guard(finish, function(err)
          eq(err.code, "not-found", "the error of the removal")
          finish(true)
        end) }
    end,
    check = function(_, task) truthy(task, "the task") end,
  },
  {
    capability = "fs", fn = "shell.fs.watch()",
    call = function() return shell.fs.watch{ path = OUTSIDE, on_changed = function() end } end,
    check = function(finish, watch)
      eq(watch.alive, true, "watch.alive")
      watch:destroy()
      eq(watch.alive, false, "watch.alive after destroy ()")
      finish(true)
    end,
  },

  -- fs.md: writing the extension's own directory (reading it needs nothing)
  {
    capability = "fs", fn = "shell.fs.write()", where = "the extension's own directory",
    call = function(_, finish)
      local file = DIR .. "/written.txt"

      return shell.fs.write{ path = file, contents = "matrix",
        on_done = guard(finish, function()
          shell.fs.remove{ path = file,
            on_done = guard(finish, function()
              eq(shell.fs.exists(file), false, "the file was removed")
              finish(true)
            end),
            on_error = fs_error(finish, "remove") }
        end),
        on_error = fs_error(finish, "write") }
    end,
    check = function(_, task) truthy(task, "the task") end,
  },
  {
    capability = "fs", fn = "shell.fs.remove()", where = "the extension's own directory",
    call = function(_, finish)
      return shell.fs.remove{ path = DIR .. "/missing.txt",
        on_done = guard(finish, function() error("removed a path that does not exist") end),
        on_error = guard(finish, function(err)
          eq(err.code, "not-found", "the error of the removal")
          finish(true)
        end) }
    end,
    check = function(_, task) truthy(task, "the task") end,
  },

  -- spawn.md
  {
    capability = "spawn", fn = "shell.spawn()",
    call = function(_, finish)
      return shell.spawn{ argv = { "/bin/sh", "-c", "exit 3" },
        on_exit = guard(finish, function(result)
          eq(result.status, 3, "the exit status")
          finish(true)
        end) }
    end,
    check = function(_, process, message) truthy(process, "the process (" .. tostring(message) .. ")") end,
  },

  -- network.md
  {
    capability = "network", fn = "shell.network.request()",
    call = function(ctx, finish)
      return shell.network.request{ url = ctx.url .. "/matrix",
        on_done = guard(finish, function(response)
          eq(response.status, 200, "the status")
          eq(response.body, "matrix", "the body")
          finish(true)
        end),
        on_error = function(err) finish(false, "request: " .. tostring(err.code) .. ": " .. tostring(err.message)) end }
    end,
    check = function(_, request) truthy(request, "the request") end,
  },
}

-- What no capability unlocks, and what needs none
local OTHERS = {
  {
    name = "shell.hooks.emit_after: privileged states only, whatever the capabilities",
    run = function(finish)
      raises(function() shell.hooks.emit_after("test.step", { step = "nothing" }) end,
             "shell.hooks.emit_after is only available in privileged states", "emit_after")
      finish(true)
    end,
  },
  {
    name = "shell.hooks.emit_before: privileged states only, whatever the capabilities",
    run = function(finish)
      raises(function() shell.hooks.emit_before("test.step", { step = "nothing" }) end,
             "shell.hooks.emit_before is only available in privileged states", "emit_before")
      finish(true)
    end,
  },
  {
    name = "shell.settings.open() of the extension's own schema needs no capability",
    run = function(finish)
      local settings = shell.settings.open(OWN_SCHEMA)

      eq(settings:get_boolean("flag"), false, "get_boolean ('flag')")
      truthy(settings == shell.settings.extension(), "the same handle as shell.settings.extension ()")
      finish(true)
    end,
  },
}

local function refused(gate)
  return string.format('%s requires the "%s" capability', gate.fn, gate.capability)
end

-- The rows of the matrix for this variant: { name, run (finish) }
local function rows(ctx)
  local list = {}

  for _, gate in ipairs(GATES) do
    local label = gate.fn .. (gate.where and " in " .. gate.where or "")

    if declared[gate.capability] then
      list[#list + 1] = {
        name = string.format('%s works with the "%s" capability', label, gate.capability),
        run = function(finish)
          gate.check(finish, gate.call(ctx, finish))
        end,
      }
    else
      local name = string.format('%s raises without the "%s" capability', label, gate.capability)
      local function ran()
        T.report(name .. ": no callback", false, "a callback of a refused call ran")
      end

      list[#list + 1] = {
        name = name,
        run = function(finish)
          raises(function() gate.call(ctx, ran) end, refused(gate), label)
          finish(true)
        end,
      }
    end
  end
  for _, other in ipairs(OTHERS) do
    list[#list + 1] = other
  end
  return list
end

-- Runs every row to its end, at most 10 s each, then done ()
local function run_rows(list, done)
  local pending = #list

  for _, row in ipairs(list) do
    local over = false
    local watchdog

    local function finish(ok, message)
      if over then
        return
      end
      over = true
      if watchdog then
        watchdog:cancel()
      end
      T.report(row.name, ok, not ok and tostring(message) or nil)
      pending = pending - 1
      if pending == 0 then
        done()
      end
    end

    local ok, err = pcall(row.run, finish)
    if not ok then
      finish(false, err)
    end
    if not over then
      watchdog = shell.timer.after{ ms = 10000, callback = function()
        finish(false, "timed out")
      end }
    end
  end
end

local ext = {}

function ext.enable()
  case("the variant", function()
    truthy(VARIANT, "a variant in the uuid " .. UUID)
    local known = VARIANT == "none" or VARIANT == "all"

    for _, capability in ipairs(CAPABILITIES) do
      known = known or VARIANT == capability
    end
    truthy(known, "a known variant: " .. VARIANT)
  end)

  case("shell.extension.capabilities lists what metadata.json declares", function()
    local listed = shell.extension.capabilities
    local seen = {}

    for _, capability in ipairs(listed) do
      truthy(declared[capability], "listed but not declared: " .. capability)
      truthy(not seen[capability], "listed twice: " .. capability)
      seen[capability] = true
    end
    for _, capability in ipairs(CAPABILITIES) do
      eq(seen[capability] or false, declared[capability] or false, "capability " .. capability)
    end
    eq(#shell.extension.metadata.capabilities, #listed, "#metadata.capabilities")
  end)

  -- The capabilities come from metadata.json as the loader read it: changing
  -- the tables an extension sees, if they let it, grants nothing (the matrix
  -- runs afterwards)
  case("the tables of the capabilities grant nothing", function()
    for _, capability in ipairs(CAPABILITIES) do
      pcall(function() shell.extension.capabilities[#shell.extension.capabilities + 1] = capability end)
      pcall(function() table.insert(shell.extension.metadata.capabilities, capability) end)
    end
  end)

  steps{
    matrix = function(p, done)
      run_rows(rows({ url = p.url }), done)
      return "later"
    end,
  }

  T.done()
end

return ext
