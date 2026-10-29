-- Fixture of tests/api/fs.lua: without "fs" in the capabilities an extension
-- reads its own directory and reads and writes its data directory, and every
-- other path raises the capability error (docs/api/fs.md, README.md
-- "Capabilities").
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

local DIR = shell.extension.dir
local DATA = shell.extension.data_dir

local CAP = 'requires the "fs" capability'

-- fn () must raise the capability error of shell.fs.<name> ()
local function denied(name, fn)
  raises(fn, { "shell.fs." .. name .. "()", CAP }, "shell.fs." .. name .. " ()")
end

function ext.enable()
  case("no capability is declared", function()
    eq(#shell.extension.capabilities, 0, "capabilities")
    eq(type(shell.fs), "table", "shell.fs")
  end)

  case("the data directory", function()
    eq(type(DATA), "string", "data_dir")
    truthy(DATA:find("/gnome-lua/fs-nocap@test", 1, true), "data_dir ends with the uuid of the extension")
    eq(shell.fs.exists(DATA), true, "data_dir exists once it was read")
    eq(shell.fs.info(DATA).type, "directory", "info (data_dir)")
    eq(shell.fs.exists(DATA .. "/nothing-here"), false, "exists (a missing file in data_dir)")
    T.report("data-dir", true, DATA)
  end)

  case("the extension's own directory is readable", function()
    eq(shell.fs.exists(DIR), true, "exists (dir)")
    eq(shell.fs.exists(DIR .. "/extension.lua"), true, "exists (extension.lua)")
    eq(shell.fs.info(DIR .. "/metadata.json").type, "file", "info (metadata.json)")
    -- `..` that stays inside is fine
    eq(shell.fs.exists(DIR .. "/../" .. shell.extension.uuid .. "/extension.lua"), true, "exists (..)")
  end)

  case("everything else raises the capability error, before any I/O", function()
    denied("exists", function() return shell.fs.exists("/etc/passwd") end)
    denied("exists", function() return shell.fs.exists("/") end)
    denied("info", function() return shell.fs.info("/tmp") end)
    denied("exists", function() return shell.fs.exists(DIR .. "/..") end)
    denied("exists", function() return shell.fs.exists(DIR .. "/../../../../../etc/passwd") end)
    denied("exists", function() return shell.fs.exists(DATA .. "/../x") end)
    denied("exists", function() return shell.fs.exists(DATA .. "2") end)
    denied("read", function() shell.fs.read{ path = "/etc/passwd", on_done = function() end } end)
    denied("list", function() shell.fs.list{ path = "/", on_done = function() end } end)
    denied("watch", function() shell.fs.watch{ path = "/tmp", on_changed = function() end } end)
    -- the extension's own directory is read only
    denied("write", function() shell.fs.write{ path = DIR .. "/new.txt", contents = "x" } end)
    denied("write", function() shell.fs.write{ path = DIR .. "/extension.lua", contents = "" } end)
    denied("remove", function() shell.fs.remove{ path = DIR .. "/extension.lua" } end)
    -- nothing outside the data directory is writable
    denied("write", function() shell.fs.write{ path = "/tmp/gnome-lua-fs-api-test", contents = "x" } end)
    denied("write", function() shell.fs.write{ path = DATA .. "/../x", contents = "x" } end)
    denied("remove", function() shell.fs.remove{ path = DATA .. "/../x" } end)
  end)

  case("misuse raises also for the allowed directories", function()
    raises(function() shell.fs.exists("relative") end, "absolute path", "exists (relative)")
    raises(function() shell.fs.exists("~/x") end, "absolute path", "exists (~)")
    raises(function() shell.fs.read{ path = "x", on_done = function() end } end, "absolute path", "read (relative)")
    raises(function() shell.fs.exists() end, "must be a string", "exists ()")
  end)

  case("watching the data directory is allowed", function()
    local w = shell.fs.watch{ path = DATA, on_changed = function() end }

    eq(w.alive, true, "watch.alive")
    eq(w.path, DATA, "watch.path")
    w:destroy()
    eq(w.alive, false, "watch.alive after destroy ()")
  end)

  async_case("the data directory is read and write", function(finish)
    local file = DATA .. "/sub/dir/one.txt"

    chain(finish, {
      function(go)
        shell.fs.write{ path = file, contents = "inside",
          on_done = guard(finish, function() go() end),
          on_error = function(err) finish(false, "write: " .. err.code .. ": " .. err.message) end }
      end,
      function(go)
        shell.fs.read{ path = file,
          on_done = guard(finish, function(c) eq(c, "inside", "read back"); go() end),
          on_error = function(err) finish(false, "read: " .. err.code .. ": " .. err.message) end }
      end,
      function(go)
        shell.fs.list{ path = DATA .. "/sub",
          on_done = guard(finish, function(entries)
            eq(#entries, 1, "entries")
            eq(entries[1].name, "dir", "entry")
            eq(entries[1].type, "directory", "type")
            go()
          end),
          on_error = function(err) finish(false, "list: " .. err.code .. ": " .. err.message) end }
      end,
      function(go)
        shell.fs.remove{ path = file,
          on_done = guard(finish, function() eq(shell.fs.exists(file), false, "removed"); go() end),
          on_error = function(err) finish(false, "remove: " .. err.code .. ": " .. err.message) end }
      end,
    })
  end)

  async_case("the extension's own directory is listed and read", function(finish)
    shell.fs.list{ path = DIR,
      on_done = guard(finish, function(entries)
        local names = {}
        for _, e in ipairs(entries) do names[e.name] = e.type end
        eq(names["extension.lua"], "file", "extension.lua")
        eq(names["metadata.json"], "file", "metadata.json")
        shell.fs.read{ path = DIR .. "/metadata.json",
          on_done = guard(finish, function(c)
            truthy(c:find("fs-nocap@test", 1, true), "metadata.json mentions the uuid")
            finish(true)
          end),
          on_error = function(err) finish(false, "read: " .. err.message) end }
      end),
      on_error = function(err) finish(false, "list: " .. err.message) end }
  end)

  sync_over = true
  maybe_done()
end

return ext
