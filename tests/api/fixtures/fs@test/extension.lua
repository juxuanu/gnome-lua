-- Fixture of tests/api/fs.lua: every function, field, property and error
-- kind of docs/api/fs.md, with the "fs" capability (fs-nocap@test covers the
-- extension without it). enable () runs the synchronous checks and starts the
-- asynchronous ones, and reports T.done () when the last of them has
-- finished. The script of the test checks from the outside what the
-- extension left (tests/api/README.md).
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

local DATA = shell.extension.data_dir
-- a file in the directory of all the data directories: outside this
-- extension's own directories, so it needs the capability
local OUTSIDE = DATA .. "/../fs-api-outside-" .. tostring(math.random(100000, 999999)) .. ".txt"

local disabled = false
local kept = {}

-- Runs shell.fs[kind] with opts; ok (...) gets the results, a failure fails
-- the case
local function run(finish, kind, opts, ok)
  opts.on_done = guard(finish, ok)
  opts.on_error = function(err)
    finish(false, kind .. " failed: " .. tostring(err.code) .. ": " .. tostring(err.message))
  end
  return shell.fs[kind](opts)
end

-- Runs shell.fs[kind] with opts, which must fail with `code`; then next ()
local function run_err(finish, kind, opts, code, next)
  opts.on_done = function()
    finish(false, kind .. " should have failed with " .. code)
  end
  opts.on_error = guard(finish, function(err)
    eq(type(err), "table", "the error")
    eq(err.code, code, kind .. " error code (" .. tostring(err.message) .. ")")
    eq(type(err.message), "string", "the error message")
    next()
  end)
  return shell.fs[kind](opts)
end

local function has_event(events, event, path)
  for _, e in ipairs(events) do
    if e[1] == event and e[2] == path then
      return true
    end
  end
  return false
end

function ext.enable()
  case("shell.fs and the data directory", function()
    eq(type(shell.fs), "table", "shell.fs")
    for _, name in ipairs({ "exists", "info", "read", "write", "list", "remove", "watch" }) do
      eq(type(shell.fs[name]), "function", "shell.fs." .. name)
    end
    eq(shell.fs.nothing, nil, "an unknown function")
    eq(shell.extension.capabilities[1], "fs", "the capability of the fixture")

    eq(type(DATA), "string", "data_dir")
    eq(DATA:sub(1, 1), "/", "data_dir is absolute")
    truthy(DATA:find("/gnome-lua/fs@test", 1, true), "data_dir ends with the uuid of the extension")
    eq(shell.extension.data_dir, DATA, "data_dir is stable")
    eq(shell.fs.info(DATA).type, "directory", "data_dir exists once it was read")
    raises(function() shell.extension.data_dir = "/tmp" end, "read-only", "assigning data_dir")
    eq(shell.extension.data_dir, DATA, "data_dir after the assignment")
    T.report("data-dir", true, DATA)
  end)

  case("exists and info", function()
    local dir = shell.extension.dir

    eq(shell.fs.exists(dir .. "/metadata.json"), true, "exists (file)")
    eq(shell.fs.exists(dir), true, "exists (directory)")
    eq(shell.fs.exists(dir .. "/nothing"), false, "exists (missing)")
    eq(shell.fs.exists("/"), true, "exists (anywhere, with the capability)")

    local info = shell.fs.info(dir .. "/metadata.json")
    eq(info.type, "file", "info.type")
    eq(math.type(info.size), "integer", "info.size")
    truthy(info.size > 0, "info.size is positive")
    eq(type(info.modified), "number", "info.modified")
    truthy(info.modified > 1600000000, "info.modified is seconds since the epoch")
    eq(shell.fs.info(dir).type, "directory", "info of a directory")
    eq(shell.fs.info("/dev/null").type, "special", "info of a device")
    eq(shell.fs.info(dir .. "/nothing"), nil, "info of a missing file")
    eq(select("#", shell.fs.info(dir .. "/nothing")), 1, "info of a missing file returns one value")
  end)

  case("misuse raises", function()
    raises(function() shell.fs.exists("relative") end, "absolute path", "exists (relative)")
    raises(function() shell.fs.exists("") end, "absolute path", "exists (empty)")
    raises(function() shell.fs.exists() end, "must be a string", "exists ()")
    raises(function() shell.fs.info(5) end, "must be a string", "info (5)")
    raises(function() shell.fs.read{ path = "x", on_done = function() end } end, "absolute path", "read (relative)")
    raises(function() shell.fs.read{ path = DATA } end, "on_done", "read without on_done")
    raises(function() shell.fs.read{ on_done = function() end } end, "'path'", "read without path")
    raises(function() shell.fs.read{ path = DATA, on_done = function() end, bogus = 1 } end, "bogus", "read, unknown field")
    raises(function() shell.fs.read{ path = DATA, on_done = function() end, max_bytes = 0 } end, "max_bytes", "read, max_bytes = 0")
    raises(function() shell.fs.read{ path = DATA, on_done = 5 } end, "on_done", "read, on_done = 5")
    raises(function() shell.fs.write{ path = DATA } end, "contents", "write without contents")
    raises(function() shell.fs.write{ path = DATA, contents = 5 } end, "contents", "write, contents = 5")
    raises(function() shell.fs.write{ path = DATA, contents = "", append = "yes" } end, "append", "write, append = 'yes'")
    raises(function() shell.fs.list{ path = DATA } end, "on_done", "list without on_done")
    raises(function() shell.fs.watch{ path = DATA } end, "on_changed", "watch without on_changed")
    raises(function() shell.fs.watch{ on_changed = function() end } end, "'path'", "watch without path")
    raises(function() shell.fs.remove("x") end, "options table", "remove ('x')")
  end)

  case("a task is a handle, and cancelling it ends it at once", function()
    local t = shell.fs.read{ path = DATA, on_done = function()
      T.report("a cancelled read must not call on_done", false)
    end }

    eq(t.alive, true, "task.alive")
    truthy(tostring(t):find("shell.Task", 1, true), "tostring (task)")
    t:cancel()
    eq(t.alive, false, "task.alive after cancel ()")
    t:cancel()   -- a second time does nothing
  end)

  -- ---------------------------------------------------------------
  -- Asynchronous cases (they run side by side: one directory each)
  -- ---------------------------------------------------------------
  async_case("write, read, append, list and remove in the data directory", function(finish)
    local D = DATA .. "/t1"
    local A, B = D .. "/a.txt", D .. "/b.bin"
    local binary = "x\0y\255\n"

    chain(finish, {
      -- the parents of the data directory are made by the write
      function(go) run(finish, "write", { path = A, contents = "one" }, function() go() end) end,
      function(go) run(finish, "read", { path = A }, function(c) eq(c, "one", "read back"); go() end) end,
      function(go) run(finish, "write", { path = A, contents = "two", append = true }, function() go() end) end,
      function(go) run(finish, "read", { path = A }, function(c) eq(c, "onetwo", "appended"); go() end) end,
      function(go) run(finish, "write", { path = A, contents = "new" }, function() go() end) end,
      function(go) run(finish, "read", { path = A }, function(c) eq(c, "new", "replaced"); go() end) end,
      function(go) run(finish, "write", { path = A, contents = "" }, function() go() end) end,
      function(go) run(finish, "read", { path = A }, function(c) eq(c, "", "emptied"); go() end) end,
      function(go) run(finish, "write", { path = A, contents = "onetwo" }, function() go() end) end,
      function(go) run(finish, "write", { path = B, contents = binary }, function() go() end) end,
      function(go) run(finish, "read", { path = B }, function(c) eq(c, binary, "raw bytes round trip"); go() end) end,
      function(go)
        run(finish, "list", { path = D }, function(entries)
          eq(#entries, 2, "number of entries")
          eq(entries[1].name, "a.txt", "first entry (sorted)")
          eq(entries[1].type, "file", "type of a.txt")
          eq(entries[2].name, "b.bin", "second entry")
          go()
        end)
      end,
      function(go)
        eq(shell.fs.info(A).size, 6, "size of a.txt")
        eq(shell.fs.exists(B), true, "b.bin exists")
        go()
      end,
      function(go) run_err(finish, "read", { path = A, max_bytes = 5 }, "too-large", go) end,
      function(go) run(finish, "read", { path = A, max_bytes = 6 }, function(c) eq(c, "onetwo", "max_bytes = size"); go() end) end,
      function(go) run_err(finish, "read", { path = D .. "/missing" }, "not-found", go) end,
      function(go) run_err(finish, "read", { path = D }, "other", go) end,
      function(go) run_err(finish, "list", { path = D .. "/missing" }, "not-found", go) end,
      function(go) run_err(finish, "list", { path = A }, "other", go) end,
      function(go) run(finish, "remove", { path = A }, function() eq(shell.fs.exists(A), false, "a.txt removed"); go() end) end,
      function(go) run_err(finish, "remove", { path = A }, "not-found", go) end,
      function(go) run(finish, "remove", { path = B }, function() go() end) end,
      function(go) run(finish, "list", { path = D }, function(entries) eq(#entries, 0, "empty directory"); go() end) end,
      function(go) run(finish, "remove", { path = D }, function() eq(shell.fs.exists(D), false, "directory removed"); go() end) end,
    })
  end)

  async_case("a path outside the data directory", function(finish)
    chain(finish, {
      function(go) run(finish, "write", { path = OUTSIDE, contents = "outside" }, function() go() end) end,
      function(go) run(finish, "read", { path = OUTSIDE }, function(c) eq(c, "outside", "read back"); go() end) end,
      function(go)
        eq(shell.fs.info(OUTSIDE).type, "file", "info")
        go()
      end,
      -- a missing parent is made only inside the data directory
      function(go) run_err(finish, "write", { path = DATA .. "/../fs-api-no-such-dir/x", contents = "x" }, "not-found", go) end,
      function(go) run(finish, "remove", { path = OUTSIDE }, function() eq(shell.fs.exists(OUTSIDE), false); go() end) end,
    })
  end)

  async_case("the extension's own directory", function(finish)
    local returned = false

    -- callbacks never run inside the call
    shell.fs.list{ path = shell.extension.dir,
      on_done = guard(finish, function(entries)
        eq(returned, true, "on_done ran inside the call")
        local types = {}
        for _, e in ipairs(entries) do types[e.name] = e.type end
        eq(types["metadata.json"], "file", "metadata.json")
        eq(types["extension.lua"], "file", "extension.lua")
        run(finish, "read", { path = shell.extension.dir .. "/metadata.json" }, function(c)
          truthy(c:find("fs@test", 1, true), "metadata.json mentions the uuid")
          finish(true)
        end)
      end),
      on_error = function(err) finish(false, "list: " .. err.message) end }
    returned = true
  end)

  async_case("errors are never synchronous either", function(finish)
    local returned = false

    shell.fs.read{ path = DATA .. "/t2-missing", on_done = function() finish(false, "read of a missing file succeeded") end,
      on_error = guard(finish, function(err)
        eq(returned, true, "on_error ran inside the call")
        eq(err.code, "not-found", "code")
        truthy(err.message:find("t2-missing", 1, true), "the message names the file")
        finish(true)
      end) }
    returned = true
  end)

  async_case("cancel", function(finish)
    local big = DATA .. "/t3/big"
    local called = false

    run(finish, "write", { path = big, contents = ("x"):rep(4 * 1024 * 1024) }, function()
      local t = shell.fs.read{ path = big,
        on_done = function() called = true end,
        on_error = guard(finish, function(err)
          eq(err.code, "cancelled", "code of a cancelled task")
          eq(type(err.message), "string", "message")
          eq(called, false, "on_done")
          run(finish, "remove", { path = big }, function() finish(true) end)
        end) }

      t:cancel()
      eq(t.alive, false, "alive after cancel ()")
      eq(called, false, "no callback inside cancel ()")
    end)
  end)

  async_case("watch", function(finish)
    local W = DATA .. "/t4"
    local file = W .. "/seen.txt"
    local events = {}

    -- the directory comes from a write
    run(finish, "write", { path = W .. "/first.txt", contents = "1" }, function()
      local w = shell.fs.watch{ path = W, on_changed = function(event, path, other)
        events[#events + 1] = { event, path, other }
      end }

      eq(w.alive, true, "watch.alive")
      eq(w.path, W, "watch.path")
      truthy(tostring(w):find("shell.FileWatch", 1, true), "tostring (watch)")

      run(finish, "write", { path = file, contents = "x", append = true }, function()
        poll(finish, function() return has_event(events, "created", file) end, 5000,
             "a 'created' event for " .. file, function()
          run(finish, "remove", { path = file }, function()
            poll(finish, function() return has_event(events, "deleted", file) end, 5000,
                 "a 'deleted' event for " .. file, function()
              w:disconnect()
              eq(w.alive, false, "watch.alive after disconnect ()")
              w:disconnect()
              w:destroy()

              local n = #events
              run(finish, "write", { path = W .. "/after.txt", contents = "z" }, function()
                shell.timer.after{ ms = 300, callback = guard(finish, function()
                  eq(#events, n, "events after disconnect ()")
                  finish(true)
                end) }
              end)
            end)
          end)
        end)
      end)
    end)
  end)

  -- what stays when the extension is disabled: a file of the data directory
  -- (the script reads it back), and a watch and a pending read that must not
  -- call anything any more (the script writes into the watched directory)
  async_case("what is left for the disable", function(finish)
    run(finish, "write", { path = DATA .. "/persist.txt", contents = "kept" }, function()
      run(finish, "write", { path = DATA .. "/keep/seed", contents = "s" }, function()
        kept.watch = shell.fs.watch{ path = DATA .. "/keep", on_changed = function()
          if disabled then
            T.report("the watch must not call back after disable", false)
          end
        end }
        kept.task = shell.fs.read{ path = DATA .. "/keep/seed",
          on_done = function() end, on_error = function() end }
        finish(true)
      end)
    end)
  end)

  sync_over = true
  maybe_done()
end

-- disable () is optional; this one checks that the objects are still usable in it
function ext.disable()
  disabled = true
  case("disable () runs before the objects are destroyed", function()
    eq(kept.watch.alive, true, "the watch is alive in disable ()")
  end)
end

return ext
