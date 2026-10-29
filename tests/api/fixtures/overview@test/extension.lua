-- Fixture of tests/api/overview.lua: every member, field and error kind of
-- docs/api/overview.md. enable () runs the checks that do not depend on the
-- state of the overview; the script of the test asks for the others with
-- steps (tests/api/README.md): each one starts from the hidden overview.
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

local UUID = "overview@test"

local ext = {}

-- The after hooks seen since the last step, and the one thing a step waits for
local kept = { events = {}, waiting = nil }

local function reset()
  kept.events = {}
  kept.waiting = nil
end

-- await (name, fn): fn (payload) runs once, when the after hook `name` comes
local function await(name, fn)
  kept.waiting = { name = name, fn = fn }
end

local function names()
  local out = {}
  for i, e in ipairs(kept.events) do
    out[i] = e.name
  end
  return table.concat(out, ",")
end

-- The first event named `name`
local function event(name)
  for _, e in ipairs(kept.events) do
    if e.name == name then
      return e.payload
    end
  end
end

-- A step handler finishes, with a failing check if fn raised, when `hook`
-- came: fn () runs from the hook
local function finish_on(name, step, done, fn)
  await(name, function(payload)
    local ok, err = pcall(fn, payload)
    T.report("step " .. step .. ": after " .. name, ok, not ok and tostring(err) or nil)
    done()
  end)
end

function ext.enable()
  -- the after hooks the checks of the steps are made from
  for _, name in ipairs({ "overview.showing", "overview.shown", "overview.hiding", "overview.hidden" }) do
    shell.hooks.on(name, function(p)
      kept.events[#kept.events + 1] = { name = name, payload = p }
      local w = kept.waiting
      if w and w.name == name then
        kept.waiting = nil
        w.fn(p)
      end
    end)
  end

  -- -------------------------------------------------------------------
  -- The table
  -- -------------------------------------------------------------------
  case("shell.overview", function()
    eq(type(shell.overview), "table", "shell.overview")
    eq(type(shell.overview.show), "function", "shell.overview.show")
    eq(type(shell.overview.hide), "function", "shell.overview.hide")
    eq(type(shell.overview.toggle), "function", "shell.overview.toggle")
    eq(shell.overview.no_such_function, nil, "an absent function is nil, not a stub")
    eq(shell.api_version.minor >= 1, true, "the API is 1.1 or later")
  end)

  case("properties have their types", function()
    eq(type(shell.overview.visible), "boolean", "visible")
    eq(type(shell.overview.animating), "boolean", "animating")
    local target = shell.overview.target
    eq(target == "hidden" or target == "window-picker" or target == "app-grid", true, "target is " .. tostring(target))
    -- hidden exactly when the overview is not on its way in or shown
    eq(shell.overview.visible or target == "hidden", true, "a hidden target needs a visible overview or none")
  end)

  case("properties are read-only", function()
    raises(function() shell.overview.visible = true end, "visible", "assigning visible")
    raises(function() shell.overview.target = "app-grid" end, "target", "assigning target")
    raises(function() shell.overview.animating = false end, "animating", "assigning animating")
    raises(function() shell.overview.no_such_property = 1 end, "no_such_property", "assigning an unknown member")
    raises(function() shell.overview.show = nil end, "show", "replacing a function")
    eq(type(shell.overview.show), "function", "show is still there")
  end)

  case("pairs lists the members", function()
    local seen = {}
    for k in pairs(shell.overview) do
      seen[k] = true
    end
    for _, k in ipairs({ "visible", "target", "animating", "show", "hide", "toggle" }) do
      truthy(seen[k], "pairs lists " .. k)
    end
  end)

  case("show: arguments", function()
    raises(function() shell.overview.show("app-grid") end, nil, "a string instead of a table")
    raises(function() shell.overview.show(5) end, nil, "a number instead of a table")
    raises(function() shell.overview.show{ bogus = 1 } end, "bogus", "an unknown field")
    raises(function() shell.overview.show{ target = "nowhere" } end, "target", "an unknown target")
    raises(function() shell.overview.show{ target = "hidden" } end, "target", "hidden is not a page to show")
    raises(function() shell.overview.show{ target = 5 } end, "target", "a number as target")
    raises(function() shell.overview.show{ target = true } end, "target", "a boolean as target")
    -- nothing happened: no hook came
    eq(#kept.events, 0, "hooks emitted by the calls that raised")
  end)

  -- -------------------------------------------------------------------
  -- The steps: each starts and ends with the hidden overview. A call made
  -- from inside a hook of the overview is deferred (README rule 8), so what
  -- the steps check right after a call runs from timers.
  -- -------------------------------------------------------------------
  local function later(fn)
    shell.timer.after{ ms = 50, callback = fn }
  end

  -- Runs fn; the step reports its failure under `what` and is done
  local function guarded(what, done, fn)
    local ok, err = pcall(fn)
    T.report(what, ok, not ok and tostring(err) or nil)
    if not ok then
      done()
    end
    return ok
  end

  steps {
    show = function(p, done)
      reset()
      eq(shell.overview.visible, false, "visible before")
      eq(shell.overview.target, "hidden", "target before")
      eq(shell.overview.animating, false, "animating before")
      eq(shell.overview.show(), nil, "show returns nothing")
      eq(shell.overview.visible, true, "visible right after show")
      eq(shell.overview.target, "window-picker", "target right after show")
      eq(shell.overview.animating, true, "animating right after show")
      finish_on("overview.shown", "show", done, function(shown)
        eq(shell.overview.visible, true, "visible when shown")
        eq(shell.overview.target, "window-picker", "target when shown")
        eq(shell.overview.animating, false, "animating when shown")
        eq(names(), "overview.showing,overview.shown", "hooks")
        local showing = event("overview.showing")
        eq(showing.reason, "extension", "reason of overview.showing")
        eq(showing.owner, UUID, "owner of overview.showing")
        eq(showing.target, "window-picker", "target of overview.showing")
        eq(shown.target, "window-picker", "target of overview.shown")
      end)
      return "later"
    end,

    -- hide () of the shown overview (the script left it shown by "show")
    hide = function(p, done)
      reset()
      eq(shell.overview.visible, true, "visible before")
      shell.overview.hide()
      eq(shell.overview.visible, true, "visible while it hides")
      eq(shell.overview.target, "hidden", "target as soon as the hide is under way")
      eq(shell.overview.animating, true, "animating while it hides")
      await("overview.hidden", function()
        guarded("step hide: after overview.hidden", done, function()
          eq(shell.overview.visible, false, "visible when hidden")
          eq(shell.overview.target, "hidden", "target when hidden")
          eq(shell.overview.animating, false, "animating when hidden")
          eq(names(), "overview.hiding,overview.hidden", "hooks")
          local hiding = event("overview.hiding")
          eq(hiding.reason, "extension", "reason of overview.hiding")
          eq(hiding.owner, UUID, "owner of overview.hiding")
        end)
        done()
      end)
      return "later"
    end,

    -- hide () of the hidden overview emits nothing
    noop = function(p, done)
      reset()
      eq(shell.overview.visible, false, "visible before")
      shell.overview.hide()
      eq(shell.overview.visible, false, "hide of the hidden overview")
      eq(#kept.events, 0, "hooks for hide of the hidden overview")
    end,

    toggle = function(p, done)
      reset()
      eq(shell.overview.visible, false, "visible before")
      shell.overview.toggle()
      eq(shell.overview.visible, true, "toggle shows")
      eq(shell.overview.target, "window-picker", "toggle shows the window picker")
      await("overview.shown", function()
        local ok = guarded("step toggle: after overview.shown", done, function()
          eq(event("overview.showing").reason, "extension", "reason of overview.showing")
          eq(event("overview.showing").owner, UUID, "owner of overview.showing")
        end)
        if not ok then
          return
        end
        later(function()
          reset()
          shell.overview.toggle()
          local started = guarded("step toggle: the second toggle", done, function()
            eq(shell.overview.target, "hidden", "toggle hides")
            eq(shell.overview.visible, true, "visible while the toggle hides")
          end)
          if started then
            await("overview.hidden", function()
              guarded("step toggle: after the second toggle", done, function()
                eq(shell.overview.visible, false, "visible after the second toggle")
                eq(event("overview.hiding").reason, "extension", "reason of overview.hiding")
                eq(event("overview.hiding").owner, UUID, "owner of overview.hiding")
              end)
              done()
            end)
          end
        end)
      end)
      return "later"
    end,

    -- show { target = "app-grid" }, then show again while it is shown
    ["app-grid"] = function(p, done)
      reset()
      shell.overview.show{ target = "app-grid" }
      eq(shell.overview.target, "app-grid", "target right after show")
      await("overview.shown", function(shown)
        local ok = guarded("step app-grid: after overview.shown", done, function()
          eq(shell.overview.target, "app-grid", "target when shown")
          eq(event("overview.showing").target, "app-grid", "target of overview.showing")
          eq(shown.target, "app-grid", "target of overview.shown")
        end)
        if not ok then
          return
        end
        later(function()
          -- already shown: nothing, also for the other page
          guarded("step app-grid: show of the shown overview", done, function()
            local n = #kept.events
            shell.overview.show{ target = "window-picker" }
            shell.overview.show()
            eq(#kept.events, n, "hooks for show of the shown overview")
            eq(shell.overview.target, "app-grid", "target after show of the shown overview")
          end)
          done()
        end)
      end)
      return "later"
    end,

    -- a listener may cancel a showing; a hiding by an extension cannot be
    cancel = function(p, done)
      reset()
      local cancelled = 0
      local seen
      local before = shell.hooks.before("overview.showing", function(payload)
        cancelled = cancelled + 1
        seen = payload
        return shell.hooks.CANCEL
      end)
      shell.overview.show()
      before:disconnect()
      eq(cancelled, 1, "calls of the before listener")
      eq(seen.reason, "extension", "reason in the before phase")
      eq(seen.owner, UUID, "owner in the before phase")
      eq(seen.target, "window-picker", "target in the before phase")
      eq(shell.overview.visible, false, "a cancelled show leaves the overview hidden")
      eq(shell.overview.target, "hidden", "target after the cancelled show")
      eq(#kept.events, 0, "after hooks of a cancelled show")

      -- now really show it, and try to cancel the hiding
      await("overview.shown", function()
        later(function()
          reset()
          local hiding_calls = 0
          local trap = shell.hooks.before("overview.hiding", function()
            hiding_calls = hiding_calls + 1
            return shell.hooks.CANCEL
          end)
          shell.overview.hide()
          trap:disconnect()
          guarded("step cancel: the hiding is not cancellable", done, function()
            eq(hiding_calls, 1, "calls of the before listener of overview.hiding")
            eq(shell.overview.target, "hidden", "the hiding of an extension is not cancellable")
          end)
          await("overview.hidden", function()
            T.report("step cancel: the hiding went through", true)
            done()
          end)
        end)
      end)
      shell.overview.show()
      return "later"
    end,

    -- called from the hooks of the overview, the actions wait for the hook
    deferred = function(p, done)
      reset()
      local calls = 0
      local before = shell.hooks.before("overview.showing", function()
        calls = calls + 1
        if calls == 1 then
          -- the same show from inside its own before phase: deferred, not recursive
          shell.overview.show{ target = "app-grid" }
          eq(shell.overview.visible, false, "visible inside the before phase")
        end
      end)
      shell.overview.show()
      before:disconnect()
      eq(calls, 1, "calls of the before listener (a re-entrant show is deferred)")

      await("overview.shown", function()
        local ok = guarded("step deferred: the re-entrant show", done, function()
          eq(shell.overview.target, "window-picker", "the deferred show did not change the target")
          eq(names(), "overview.showing,overview.shown", "hooks")
        end)
        if not ok then
          return
        end

        -- show () from the hook that ends a hiding is deferred to idle
        reset()
        local fired, inside = false, nil
        local reshow = shell.hooks.on("overview.hidden", function()
          if fired then
            return
          end
          fired = true
          shell.overview.show()
          inside = shell.overview.visible
        end)
        await("overview.shown", function()
          reshow:disconnect()
          guarded("step deferred: show from overview.hidden", done, function()
            eq(fired, true, "the listener of overview.hidden ran")
            eq(inside, false, "visible right after show from overview.hidden")
            eq(shell.overview.visible, true, "the deferred show happened")
          end)
          -- and the hide from this hook is deferred too
          reset()
          await("overview.hidden", function()
            T.report("step deferred: hide from overview.shown", shell.overview.visible == false)
            done()
          end)
          shell.overview.hide()
          T.report("step deferred: the hide from overview.shown waits",
                   shell.overview.visible == true and shell.overview.target ~= "hidden")
        end)
        shell.overview.hide()
      end)
      return "later"
    end,
  }

  T.done()
end

return ext
