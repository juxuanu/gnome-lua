-- Fixture of tests/api/spawn.lua: every field and result of docs/api/spawn.md
-- that /bin/sh and coreutils can show, with the "spawn" capability
-- (spawn-nocap@test covers the extension without it). enable () starts the
-- processes and reports T.done () when the last of them has ended; a process
-- is left running, and the script checks that it is gone after the disable
-- (tests/api/README.md).
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

local disabled = false
local kept = {}

local function sh(script)
  return { "/bin/sh", "-c", script }
end

-- Runs shell.spawn with opts; check (result) runs when it ends. A failure to
-- start fails the case.
local function run(finish, opts, check)
  opts.on_exit = guard(finish, check)
  local proc, err = shell.spawn(opts)
  if not proc then
    finish(false, "shell.spawn did not start: " .. tostring(err))
  end
  return proc
end

function ext.enable()
  case("shell.spawn exists and the capability is declared", function()
    eq(type(shell.spawn), "function", "shell.spawn")
    eq(shell.extension.capabilities[1], "spawn", "the capability of the fixture")
  end)

  case("misuse raises, a program that does not start returns nil and a message", function()
    raises(function() shell.spawn() end, "options table", "spawn ()")
    raises(function() shell.spawn{} end, "argv", "spawn{} (argv)")
    raises(function() shell.spawn{ argv = {} } end, "argv", "spawn{ argv = {} }")
    raises(function() shell.spawn{ argv = "true" } end, "argv", "spawn{ argv = 'true' }")
    raises(function() shell.spawn{ argv = { "true", 5 } } end, "argv", "spawn{ argv = { 'true', 5 } }")
    raises(function() shell.spawn{ argv = { "true" }, bogus = 1 } end, "bogus", "spawn, unknown field")
    raises(function() shell.spawn{ argv = { "true" }, cwd = 5 } end, "cwd", "spawn, cwd = 5")
    raises(function() shell.spawn{ argv = { "true" }, env = { ["A=B"] = "x" } } end, "env", "spawn, env name")
    raises(function() shell.spawn{ argv = { "true" }, env = { A = 1 } } end, "env", "spawn, env value")
    raises(function() shell.spawn{ argv = { "true" }, stdin = 5 } end, "stdin", "spawn, stdin = 5")
    raises(function() shell.spawn{ argv = { "true" }, timeout_ms = 0 } end, "timeout_ms", "spawn, timeout_ms = 0")
    raises(function() shell.spawn{ argv = { "true" }, max_output = -1 } end, "max_output", "spawn, max_output = -1")
    raises(function() shell.spawn{ argv = { "true" }, on_exit = "x" } end, "on_exit", "spawn, on_exit = 'x'")

    local proc, err = shell.spawn{ argv = { "/nonexistent/gnome-lua-program" }, on_exit = function()
      T.report("on_exit of a program that did not start", false)
    end }
    eq(proc, nil, "the process of a program that does not start")
    eq(type(err), "string", "the message")
    local proc2, err2 = shell.spawn{ argv = { "/bin/true" }, cwd = "/nonexistent/gnome-lua-dir" }
    eq(proc2, nil, "the process with a bad cwd")
    eq(type(err2), "string", "the message for a bad cwd")
  end)

  async_case("the result of a process", function(finish)
    local returned = false
    local proc

    proc = run(finish, { argv = sh("echo out; echo err >&2; exit 3") }, function(r)
      eq(returned, true, "on_exit ran inside the call")
      eq(r.status, 3, "status")
      eq(math.type(r.status), "integer", "status type")
      eq(r.signal, nil, "signal")
      eq(r.stdout, "out\n", "stdout")
      eq(r.stderr, "err\n", "stderr")
      eq(r.timed_out, false, "timed_out")
      eq(r.truncated, false, "truncated")
      eq(proc.alive, false, "the handle is dead in on_exit")
      finish(true)
    end)
    returned = true
    eq(proc.alive, true, "alive while running")
    eq(math.type(proc.pid), "integer", "pid")
    truthy(proc.pid > 1, "pid is a process id")
    truthy(tostring(proc):find("shell.Process", 1, true), "tostring (process)")
  end)

  async_case("killed by a signal", function(finish)
    run(finish, { argv = sh("kill -TERM $$") }, function(r)
      eq(r.status, nil, "status")
      eq(r.signal, "SIGTERM", "signal")
      eq(r.timed_out, false, "timed_out")
      finish(true)
    end)
  end)

  async_case("standard input, environment and working directory", function(finish)
    chain(finish, {
      function(go)
        run(finish, { argv = { "/bin/cat" }, stdin = "hello" }, function(r)
          eq(r.stdout, "hello", "cat")
          eq(r.status, 0, "status")
          go()
        end)
      end,
      function(go)
        -- more than a pipe holds, both ways
        local text = ("0123456789abcdef"):rep(16384)
        run(finish, { argv = { "/bin/cat" }, stdin = text }, function(r)
          eq(#r.stdout, #text, "size of the echo")
          eq(r.stdout == text, true, "content of the echo")
          go()
        end)
      end,
      function(go)
        run(finish, { argv = sh('echo "$A-$B-$EMPTY."'), env = { A = "a", B = "b c", EMPTY = "" } }, function(r)
          eq(r.stdout, "a-b c-.\n", "env")
          go()
        end)
      end,
      function(go)
        run(finish, { argv = { "/bin/pwd" }, cwd = "/" }, function(r)
          eq(r.stdout, "/\n", "cwd")
          go()
        end)
      end,
      function(go)
        -- without stdin the process sees /dev/null
        run(finish, { argv = { "/bin/cat" } }, function(r)
          eq(r.stdout, "", "cat without stdin")
          eq(r.status, 0, "status")
          go()
        end)
      end,
    })
  end)

  async_case("timeout", function(finish)
    run(finish, { argv = { "/bin/sleep", "30" }, timeout_ms = 200 }, function(r)
      eq(r.timed_out, true, "timed_out")
      eq(r.status, nil, "status")
      eq(r.signal, "SIGTERM", "signal")
      finish(true)
    end)
  end)

  async_case("max_output", function(finish)
    chain(finish, {
      function(go)
        run(finish, { argv = sh("head -c 5000 /dev/zero; echo e >&2"), max_output = 1000 }, function(r)
          eq(#r.stdout, 1000, "stdout is cut")
          eq(r.stderr, "e\n", "stderr is within the limit")
          eq(r.truncated, true, "truncated")
          eq(r.status, 0, "status")
          go()
        end)
      end,
      function(go)
        run(finish, { argv = sh("head -c 1000 /dev/zero"), max_output = 1000 }, function(r)
          eq(#r.stdout, 1000, "exactly the limit")
          eq(r.truncated, false, "not truncated")
          go()
        end)
      end,
      function(go)
        run(finish, { argv = sh("printf 0123456789"), max_output = 4 }, function(r)
          eq(r.stdout, "0123", "the head is kept")
          eq(r.truncated, true, "truncated")
          go()
        end)
      end,
    })
  end)

  async_case("kill ()", function(finish)
    local proc
    proc = run(finish, { argv = { "/bin/sleep", "30" } }, function(r)
      eq(r.signal, "SIGTERM", "signal")
      eq(r.status, nil, "status")
      eq(r.timed_out, false, "timed_out")
      eq(proc.alive, false, "alive")
      proc:kill()   -- harmless on an ended process
      finish(true)
    end)
    eq(proc.alive, true, "alive")
    proc:kill()
  end)

  async_case("destroy () forgets the process", function(finish)
    local proc = shell.spawn{ argv = { "/bin/sleep", "30" }, on_exit = function()
      finish(false, "on_exit ran after destroy ()")
    end }

    eq(proc.alive, true, "alive")
    proc:destroy()
    eq(proc.alive, false, "alive after destroy ()")
    proc:destroy()
    proc:kill()
    shell.timer.after{ ms = 400, callback = function() finish(true) end }
  end)

  async_case("without on_exit", function(finish)
    local proc = shell.spawn{ argv = sh("head -c 2000000 /dev/zero; exit 0") }

    eq(proc.alive, true, "alive")
    poll(finish, function() return not proc.alive end, 8000, "the process to end", function() finish(true) end)
  end)

  -- a process that runs when the extension is disabled: the script checks
  -- from the outside that it was killed
  async_case("a process left running", function(finish)
    kept.proc = shell.spawn{ argv = { "/bin/sleep", "60" }, on_exit = function()
      if disabled then
        T.report("on_exit must not run after disable", false)
      end
    end }
    T.report("sleeper-pid", true, tostring(kept.proc.pid))
    finish(true)
  end)

  sync_over = true
  maybe_done()
end

function ext.disable()
  disabled = true
  case("disable () runs before the objects are destroyed", function()
    eq(kept.proc.alive, true, "the process runs in disable ()")
  end)
end

return ext
