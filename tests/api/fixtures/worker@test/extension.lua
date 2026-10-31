-- Fixture of tests/api/worker.lua: every function, option, limit and error of
-- docs/api/worker.md, with the "worker" capability (worker-nocap@test covers
-- the extension without it). enable () runs the cases that cause no error and
-- reports T.done (); the step "errors" runs those that do, under the captured
-- log of the script (they count against the error policy). A worker is left
-- looping at disable: the script checks that its thread is gone.
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


-- The cases of the step "errors": one after the other (the limit of four
-- workers, as for the sequence of enable ()), each with its own watchdog;
-- done () when the last is over. acase queues a case, run () starts them.
local function step_cases(done)
  local list = {}

  local function acase(name, body)
    list[#list + 1] = { name = name, body = body }
  end

  local function run(i)
    local item = list[i]
    local finished = false
    local watchdog

    if not item then
      done()
      return
    end

    local function finish(ok, message)
      if finished then
        return
      end
      finished = true
      if watchdog then
        watchdog:cancel()
      end
      T.report(item.name, ok, not ok and tostring(message) or nil)
      run(i + 1)
    end

    local ok, err = pcall(item.body, finish)
    if not ok then
      finish(false, err)
    elseif not finished then
      watchdog = shell.timer.after{ ms = 10000, callback = function() finish(false, "timed out") end }
    end
  end

  return acase, function() run(1) end
end

-- deep equality, with the integer/float distinction
local function deq(a, b)
  if type(a) ~= type(b) then
    return false
  end
  if type(a) ~= "table" then
    return a == b and math.type(a) == math.type(b)
  end
  for k, v in pairs(a) do
    if not deq(v, b[k]) then
      return false
    end
  end
  for k in pairs(b) do
    if a[k] == nil then
      return false
    end
  end
  return true
end

-- a worker of workers.echo whose replies and error land in `got`
local function echo_worker(finish, got, extra)
  local opts = {
    module = "workers.echo",
    on_message = guard(finish, function(v)
      got.n = got.n + 1
      got[got.n] = v
      if got.on_message then
        got.on_message(v)
      end
    end),
    on_error = guard(finish, function(e)
      got.error = e
      if got.on_error then
        got.on_error(e)
      end
    end),
  }
  for k, v in pairs(extra or {}) do
    opts[k] = v
  end
  got.n = 0
  local w, err = shell.worker.start(opts)
  truthy(w, "the worker (" .. tostring(err) .. ")")
  return w
end

-- The cases that start workers run one after the other (an extension has
-- four workers at most, and a stopped worker holds its slot until its
-- thread has exited): seq_case queues one, run_sequence runs them and then
-- lets enable () report
local sequence = {}

local function seq_case(name, body)
  sequence[#sequence + 1] = { name = name, body = body }
end

local function run_sequence(i)
  local item = sequence[i]

  if not item then
    sync_over = true
    maybe_done()
    return
  end
  async_case(item.name, function(finish)
    item.body(function(ok, message)
      finish(ok, message)
      run_sequence(i + 1)
    end)
  end)
end

local kept = {}
local disabled = false
local ext = {}

function ext.enable()
  case("shell.worker exists and the capability is declared", function()
    eq(type(shell.worker.start), "function", "shell.worker.start")
    eq(math.type(shell.worker.limit), "integer", "shell.worker.limit")
    truthy(shell.worker.limit >= 8, "shell.worker.limit >= 8")
    eq(shell.worker.post, nil, "shell.worker.post in the extension")
    eq(shell.extension.capabilities[1], "worker", "the capability of the fixture")
  end)

  case("misuse of start raises before anything is created", function()
    raises(function() shell.worker.start() end, "options table", "start ()")
    raises(function() shell.worker.start{} end, "module", "start{}")
    raises(function() shell.worker.start{ module = 7 } end, "module", "start{ module = 7 }")
    raises(function() shell.worker.start{ module = "workers/echo" } end, "invalid module name 'workers/echo'", "a slash")
    raises(function() shell.worker.start{ module = "..x" } end, "invalid module name", "dots")
    raises(function() shell.worker.start{ module = "workers.echo", bogus = 1 } end, "unknown field 'bogus'", "an unknown field")
    raises(function() shell.worker.start{ module = "workers.echo", max_memory = 1 } end, "max_memory", "max_memory too small")
    raises(function() shell.worker.start{ module = "workers.echo", max_memory = 2^40 } end, "max_memory", "max_memory too large")
    raises(function() shell.worker.start{ module = "workers.echo", on_message = 1 } end, "on_message", "on_message = 1")
    raises(function() shell.worker.start{ module = "workers.echo", on_error = "x" } end, "on_error", "on_error = 'x'")
  end)

  seq_case("echo: every kind of value crosses by value, in order", function(finish)
    local got = {}
    local values = {
      true, false, 0, -7, 2^53, 1.5, -0.25, "text", "", "ünïcödé", "a\0b\xff\xfe",
      { 1, 2, 3 }, { a = 1, b = "x" }, { nested = { deep = { 1, { 2, { 3 } } } }, list = { "a", { b = true } } }, {},
    }
    local w = echo_worker(finish, got)

    eq(w.alive, true, "alive after start")
    truthy(tostring(w):find("^<shell%.Worker "), "tostring (worker)")
    eq(w.queued, 0, "queued at start")
    for _, v in ipairs(values) do
      w:post(v)
    end
    eq(got.n, 0, "no delivery inside post")
    poll(finish, function() return got.n == #values end, 8000, "the echoes", function()
      for i, v in ipairs(values) do
        truthy(deq(got[i], v), "value " .. i .. " came back equal")
      end
      eq(math.type(got[3]), "integer", "integers stay integers")
      eq(math.type(got[6]), "float", "floats stay floats")
      eq(got[11], "a\0b\xff\xfe", "bytes cross as they are")
      eq(w.queued, 0, "queued once handled")
      w:destroy()
      eq(w.alive, false, "alive after destroy ()")
      finish(true)
    end)
  end)

  seq_case("the return value is posted, progress posts come first, globals persist, nil is not posted", function(finish)
    local got = {}
    local w = echo_worker(finish, got)

    w:post{ op = "progress" }
    w:post{ op = "nothing" }
    w:post{ op = "count" }
    w:post{ op = "count" }
    poll(finish, function() return got.n == 5 end, 8000, "the replies", function()
      eq(got[1], "p1", "progress 1")
      eq(got[2], "p2", "progress 2")
      eq(got[3], "done", "the return value")
      eq(got[4], 1, "count 1 (nothing was not posted)")
      eq(got[5], 2, "count 2: the worker kept its globals")
      w:destroy()
      finish(true)
    end)
  end)

  seq_case("inside the worker: the sandbox, shell.log/time/extension/worker.post, nothing else", function(finish)
    local got = {}
    local w = echo_worker(finish, got)

    w:post{ op = "env" }
    poll(finish, function() return got.n == 1 end, 8000, "the environment", function()
      local r = got[1]
      for _, name in ipairs{ "timer", "fs", "settings", "hooks", "dbus", "spawn", "network", "ui", "panel", "test", "start", "gettext", "os", "data_dir", "time", "ro", "gc" } do
        eq(r[name], true, name)
      end
      eq(r.post, "function", "shell.worker.post")
      eq(r.uuid, shell.extension.uuid, "shell.extension.uuid")
      eq(r.dir, shell.extension.dir, "shell.extension.dir")
      eq(r.name, shell.extension.metadata.name, "shell.extension.metadata.name")
      eq(r.caps, 1, "#capabilities")
      eq(r.cap1, "worker", "capabilities[1]")
      eq(r.lib, 42, "require of a shared library")
      eq(r.minor, shell.api_version.minor, "api_version")
      w:destroy()
      finish(true)
    end)
  end)

  seq_case("a worker may post while it loads", function(finish)
    local r
    local w = shell.worker.start{ module = "workers.ready", on_message = function(v) r = v end,
                                  on_error = function(e) finish(false, e.message) end }
    poll(finish, function() return r ~= nil end, 8000, "the ready message", function()
      eq(r, "ready", "the message posted at load")
      w:destroy()
      finish(true)
    end)
  end)

  seq_case("a worker without on_message drops its replies", function(finish)
    local w = shell.worker.start{ module = "workers.echo" }
    w:post("x")
    w:post{ op = "progress" }
    shell.timer.after{ ms = 300, callback = function()
      local ok, err = pcall(function()
        eq(w.alive, true, "alive")
        w:destroy()
      end)
      finish(ok, err)
    end }
  end)

  seq_case("post: what cannot cross raises in the caller and the worker is unaffected", function(finish)
    local got = {}
    local w = echo_worker(finish, got)

    raises(function() w:post(function() end) end, "cannot convert", "a function")
    raises(function() w:post(nil) end, "nil", "nil")
    raises(function() w:post() end, "exactly one value", "no value")
    raises(function() w:post(1, 2) end, "exactly one value", "two values")
    raises(function() w:post{ 1, x = 2 } end, "mixes integer and string keys", "mixed keys")
    raises(function() w:post{ 1, nil, 3 } end, "sparse", "a sparse sequence")
    raises(function() w:post{ [true] = 1 } end, "key", "a boolean key")
    raises(function() w:post(w) end, "cannot convert", "a handle")
    raises(function() w:post(coroutine.create(function() end)) end, "cannot convert", "a coroutine")
    raises(function() local t = {} t[1] = t w:post(t) end, "too deeply", "a cycle")
    raises(function() w:post(string.rep("x", 16 * 1024 * 1024 + 1)) end, "message is too large (limit 16777216 bytes)", "16 MiB + 1")
    w:post("still here")
    poll(finish, function() return got.n == 1 end, 8000, "the echo", function()
      eq(got[1], "still here", "the worker still works")
      w:destroy()
      finish(true)
    end)
  end)

  seq_case("post: the 65th queued message is refused, the 64 arrive", function(finish)
    local got = {}
    local w = echo_worker(finish, got)

    w:post{ op = "slow", s = 0.5 }
    poll(finish, function() return w.queued == 0 end, 8000, "the worker to take the slow message", function()
      for i = 1, 64 do
        w:post(i)
      end
      eq(w.queued, 64, "queued")
      raises(function() w:post(65) end, "too many queued messages (limit 64)", "the 65th")
      poll(finish, function() return got.n == 65 end, 8000, "the 65 replies", function()
        eq(got[1], "slow-done", "the slow one")
        eq(got[2], 1, "the first queued")
        eq(got[65], 64, "the last queued")
        w:destroy()
        raises(function() w:post("dead") end, "no longer valid", "post on a dead handle")
        raises(function() return w.queued end, "no longer valid", "queued on a dead handle")
        w:terminate()
        w:destroy()
        finish(true)
      end)
    end)
  end)

  seq_case("terminate: the handle dies at once, on_error 'terminated' comes later, nothing counts", function(finish)
    local got = {}
    local w
    w = shell.worker.start{ module = "workers.loop",
      on_message = function() finish(false, "a message of a looping worker") end,
      on_error = guard(finish, function(e) got.error = e; got.alive_in_error = w.alive end) }

    w:post("go")
    shell.timer.after{ ms = 200, callback = guard(finish, function()
      eq(w.alive, true, "alive while it loops")
      eq(got.error, nil, "no error while it loops")
      w:terminate()
      eq(w.alive, false, "dead when terminate () returns")
      eq(got.error, nil, "on_error did not run inside terminate ()")
      w:terminate()   -- idempotent, DEAD_OK
      poll(finish, function() return got.error ~= nil end, 8000, "on_error", function()
        eq(got.error.code, "terminated", "the code")
        eq(type(got.error.message), "string", "the message")
        eq(got.alive_in_error, false, "alive inside on_error")
        finish(true)
      end)
    end) }
  end)

  seq_case("terminate stops a worker that catches its errors", function(finish)
    local got = {}
    local w = shell.worker.start{ module = "workers.loop",
      on_error = guard(finish, function(e) got.error = e end) }

    w:post("catch")
    shell.timer.after{ ms = 100, callback = guard(finish, function()
      w:terminate()
      poll(finish, function() return got.error ~= nil end, 8000, "on_error", function()
        eq(got.error.code, "terminated", "the code")
        finish(true)
      end)
    end) }
  end)

  seq_case("destroy: no callback at all", function(finish)
    local ran = false
    local w = shell.worker.start{ module = "workers.loop",
      on_message = function() ran = true end, on_error = function() ran = true end }

    w:post("go")
    shell.timer.after{ ms = 100, callback = guard(finish, function()
      w:destroy()
      eq(w.alive, false, "dead")
      shell.timer.after{ ms = 500, callback = guard(finish, function()
        eq(ran, false, "no callback after destroy ()")
        finish(true)
      end) }
    end) }
  end)

  seq_case("terminate wakes a worker blocked in shell.worker.post", function(finish)
    local got = { n = 0 }
    local w
    w = shell.worker.start{ module = "workers.echo",
      on_message = guard(finish, function(v)
        got.n = got.n + 1
        if got.n == 1 then
          w:terminate()
        end
      end),
      on_error = guard(finish, function(e) got.error = e end) }

    w:post{ op = "flood", n = 100000 }
    poll(finish, function() return got.error ~= nil end, 8000, "on_error", function()
      eq(got.error.code, "terminated", "the code")
      eq(got.n, 1, "no message after terminate ()")
      shell.timer.after{ ms = 200, callback = guard(finish, function()
        eq(got.n, 1, "still no message")
        finish(true)
      end) }
    end)
  end)

  seq_case("a flood arrives whole and in order, one message per dispatch", function(finish)
    local got = {}
    local w = echo_worker(finish, got)

    w:post{ op = "flood", n = 300 }
    poll(finish, function() return got.n == 301 end, 8000, "the flood", function()
      eq(got[1], 1, "first")
      eq(got[150], 150, "middle")
      eq(got[300], 300, "last")
      eq(got[301], "flooded", "the return value after the posts")
      w:destroy()
      finish(true)
    end)
  end)

  seq_case("four workers per extension; the slot comes back when the thread exits", function(finish)
    -- the threads of the workers of earlier cases may still be exiting and
    -- hold their slots: start until the limit refuses
    local ws = {}
    local err
    repeat
      local ok, result = pcall(shell.worker.start, { module = "workers.echo" })
      if ok then
        ws[#ws + 1] = result
      else
        err = result
      end
    until not ok or #ws > 4
    eq(#ws <= 4, true, "at most four workers started (" .. #ws .. ")")
    truthy(tostring(err):find("too many workers (limit 4)", 1, true), "the fifth raised: " .. tostring(err))
    for _, w in ipairs(ws) do
      w:destroy()
    end
    -- the threads exit on their own; until then the slots are taken (worker.md)
    local again
    poll(finish, function()
      local ok, w = pcall(shell.worker.start, { module = "workers.echo" })
      again = ok and w or nil
      return again ~= nil
    end, 8000, "a slot to come back", function()
      truthy(again.alive, "a worker runs again")
      again:destroy()
      finish(true)
    end)
  end)

  -- a worker that loops when the extension is disabled: the script checks
  -- from the outside that its thread is gone
  seq_case("a worker left running", function(finish)
    kept.worker = shell.worker.start{ module = "workers.loop",
      on_message = function() T.report("a callback of the kept worker ran", false) end,
      on_error = function() T.report("on_error of the kept worker ran", false) end }
    kept.worker:post("go")
    finish(true)
  end)

  steps {
    -- the errors of workers, under the captured log of the script; each
    -- counts against the error policy (the script expects them)
    errors = function(_, done)
      local acase, run = step_cases(done)

      acase("a handler that raises: on_error 'error', the worker ends, the queue is dropped", function(finish)
        local got = {}
        local w
        w = shell.worker.start{ module = "workers.fail",
          on_message = function() finish(false, "a message of a failed worker") end,
          on_error = guard(finish, function(e) got.error = e; got.alive = w.alive end) }

        w:post("first")
        w:post("never")
        poll(finish, function() return got.error ~= nil end, 8000, "on_error", function()
          eq(got.error.code, "error", "the code")
          truthy(got.error.message:find("the worker failed on first", 1, true), "the message: " .. got.error.message)
          truthy(got.error.message:find("fail.lua", 1, true), "the traceback names the file")
          eq(got.alive, false, "alive inside on_error")
          eq(w.alive, false, "alive after")
          finish(true)
        end)
      end)

      acase("a module that does not exist: 'load'", function(finish)
        local got = {}
        local w = shell.worker.start{ module = "workers.missing",
          on_message = function() finish(false, "a message") end,
          on_error = guard(finish, function(e) got.error = e end) }
        w:post("x")
        poll(finish, function() return got.error ~= nil end, 8000, "on_error", function()
          eq(got.error.code, "load", "the code")
          truthy(got.error.message:find("not found", 1, true), "the message: " .. got.error.message)
          eq(w.alive, false, "alive after")
          finish(true)
        end)
      end)

      acase("a module that is not a worker: 'load'", function(finish)
        local got = {}
        shell.worker.start{ module = "workers.notable", on_error = guard(finish, function(e) got.error = e end) }
        poll(finish, function() return got.error ~= nil end, 8000, "on_error", function()
          eq(got.error.code, "load", "the code")
          truthy(got.error.message:find("on_message", 1, true), "the message names on_message")
          finish(true)
        end)
      end)

      acase("past max_memory: 'memory'", function(finish)
        local got = {}
        local w = shell.worker.start{ module = "workers.hog", max_memory = 1024 * 1024,
          on_error = guard(finish, function(e) got.error = e end) }
        w:post("go")
        poll(finish, function() return got.error ~= nil end, 8000, "on_error", function()
          eq(got.error.code, "memory", "the code")
          finish(true)
        end)
      end)

      acase("the log of the worker is relayed in order with its messages", function(finish)
        local got = {}
        local w = echo_worker(finish, got)
        w:post{ op = "log" }
        poll(finish, function() return got.n == 3 end, 8000, "the replies", function()
          eq(got[1], "before the log", "before")
          eq(got[2], "after the log", "after")
          eq(got[3], "logged", "the return value")
          w:destroy()
          finish(true)
        end)
      end)

      run()
      return "later"
    end,
  }

  run_sequence(1)
end

function ext.disable()
  disabled = true
  case("disable () runs before the workers are destroyed", function()
    eq(kept.worker.alive, true, "the kept worker runs in disable ()")
  end)
end

return ext
