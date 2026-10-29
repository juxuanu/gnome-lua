-- Fixture of tests/api/keybindings.lua: every function, field and error of
-- docs/api/keybindings.md. enable () runs the checks that are synchronous;
-- the script of the test presses the keys (test.press_accelerator) and asks
-- for the verdicts with steps (tests/api/README.md).
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


local function noop() end

-- How often each binding of the steps ran
local counts = { lit = 0, lit2 = 0, sb = 0, ov = 0, nm = 0, once = 0 }
local kept = {}

local TOGGLE = "keybindings-test-toggle"
local OTHER = "keybindings-test-other"

local ext = {}

function ext.enable()
  local settings = shell.settings.extension()

  case("shell.keybindings is a module of 1.1", function()
    eq(type(shell.keybindings), "table", "type")
    eq(type(shell.keybindings.add), "function", "add")
    truthy(shell.api_version.minor >= 1, "minor " .. tostring(shell.api_version.minor))
  end)

  -- -------------------------------------------------------------------
  -- literal accelerators
  -- -------------------------------------------------------------------
  case("literal: handle fields, destroy, dead handle", function()
    local b = shell.keybindings.add{ accelerators = { "<Super><Alt>u" }, on_pressed = noop }

    truthy(b, "add returned a handle")
    eq(b.alive, true, "alive")
    eq(b.name, nil, "name of a literal binding")
    eq(type(b.accelerators), "table", "accelerators")
    eq(#b.accelerators, 1, "number of accelerators")
    eq(b.accelerators[1], "<Super><Alt>u", "accelerator")
    b:destroy()
    eq(b.alive, false, "alive after destroy")
    b:destroy()   -- idempotent
    raises(function() return b.accelerators end, "no longer valid", "reading a dead binding")
    raises(function() b:on_pressed(noop) end, "no longer valid", "on_pressed on a dead binding")
  end)

  case("literal: several accelerators are kept in order", function()
    local b = shell.keybindings.add{ accelerators = { "<Super><Alt>u", "<Control><Alt>u" }, on_pressed = noop }

    truthy(b, "add")
    eq(table.concat(b.accelerators, " "), "<Super><Alt>u <Control><Alt>u", "accelerators")
    b:destroy()
  end)

  case("literal: a taken accelerator is nil, message; destroy releases it", function()
    local first = shell.keybindings.add{ accelerators = { "<Super><Alt>u" }, on_pressed = noop }
    local second, err = shell.keybindings.add{ accelerators = { "<Super><Alt>u" }, on_pressed = noop }

    truthy(first, "first")
    eq(second, nil, "second binding of the same accelerator")
    eq(type(err), "string", "the message")
    truthy(#err > 0, "the message is not empty")
    first:destroy()

    local again = shell.keybindings.add{ accelerators = { "<Super><Alt>u" }, on_pressed = noop }
    truthy(again, "the accelerator is free after destroy")
    again:destroy()
  end)

  case("literal: all or nothing", function()
    local holder = shell.keybindings.add{ accelerators = { "<Super><Alt>u" }, on_pressed = noop }
    local both, err = shell.keybindings.add{ accelerators = { "<Super><Alt>i", "<Super><Alt>u" }, on_pressed = noop }

    truthy(holder, "holder")
    eq(both, nil, "the call fails when one accelerator is taken")
    eq(type(err), "string", "the message")

    -- the first accelerator of the failed call was released again
    local free = shell.keybindings.add{ accelerators = { "<Super><Alt>i" }, on_pressed = noop }
    truthy(free, "the accelerator of the failed call is free")
    free:destroy()
    holder:destroy()
  end)

  case("literal: the same accelerator twice in one call", function()
    local b, err = shell.keybindings.add{ accelerators = { "<Super><Alt>u", "<Super><Alt>u" }, on_pressed = noop }

    eq(b, nil, "duplicate")
    eq(type(err), "string", "the message")
  end)

  -- -------------------------------------------------------------------
  -- settings-backed
  -- -------------------------------------------------------------------
  case("settings-backed: handle fields, taken name, destroy, re-add", function()
    local b = shell.keybindings.add{ name = TOGGLE, on_pressed = noop }

    truthy(b, "add returned a handle")
    eq(b.name, TOGGLE, "name")
    eq(table.concat(b.accelerators, " "), "<Super><Alt>j", "accelerators from the schema default")

    local dup, err = shell.keybindings.add{ name = TOGGLE, on_pressed = noop }
    eq(dup, nil, "the same name twice")
    eq(type(err), "string", "the message")

    b:destroy()
    eq(b.alive, false, "dead after destroy")

    local again = shell.keybindings.add{ name = TOGGLE, settings = settings, on_pressed = noop }
    truthy(again, "the name is free after destroy; settings given explicitly")
    again:destroy()
  end)

  case("settings-backed: accelerators follows the key", function()
    local b = shell.keybindings.add{ name = OTHER, on_pressed = noop }

    truthy(b, "add")
    eq(table.concat(b.accelerators, " "), "<Super><Alt>m", "default")
    settings:set_strv(OTHER, { "<Super><Alt>w", "<Control><Alt>w" })
    eq(table.concat(b.accelerators, " "), "<Super><Alt>w <Control><Alt>w", "after set_strv")
    settings:set_strv(OTHER, {})
    eq(#b.accelerators, 0, "an empty key is an empty list")
    settings:reset(OTHER)
    eq(table.concat(b.accelerators, " "), "<Super><Alt>m", "after reset")
    b:destroy()
  end)

  case("settings-backed: a name of the shell is nil, message", function()
    -- `toggle-overview` is a key binding of the shell (org.gnome.shell.keybindings,
    -- added by src/ui/overview.c): the names of key bindings are one namespace
    local shell_settings = shell.settings.open("org.gnome.shell.keybindings")
    local b, err = shell.keybindings.add{ name = "toggle-overview", settings = shell_settings, on_pressed = noop }

    eq(b, nil, "a binding of the shell is not replaced")
    eq(type(err), "string", "the message")
    truthy(err:find("toggle-overview", 1, true), "the message names the key: " .. err)
  end)

  -- -------------------------------------------------------------------
  -- misuse
  -- -------------------------------------------------------------------
  case("misuse: fields", function()
    local add = shell.keybindings.add

    raises(function() add() end, nil, "no table")
    raises(function() add{ accelerators = { "<Super>x" }, on_pressed = noop, nope = 1 } end,
           "unknown field 'nope'", "an unknown field")
    raises(function() add{ on_pressed = noop } end, "'name' and 'accelerators'", "neither name nor accelerators")
    raises(function() add{ name = TOGGLE, accelerators = { "<Super>x" }, on_pressed = noop } end,
           "mutually exclusive", "both name and accelerators")
    raises(function() add{ accelerators = { "<Super>x" }, settings = settings, on_pressed = noop } end,
           "'settings'", "settings with accelerators")
    raises(function() add{ accelerators = { "<Super><Alt>u" } } end, "on_pressed", "no on_pressed")
    raises(function() add{ accelerators = { "<Super><Alt>u" }, on_pressed = 5 } end, "on_pressed", "on_pressed not a function")
    raises(function() add{ name = 5, on_pressed = noop } end, "name", "name not a string")
    raises(function() add{ name = "", on_pressed = noop } end, "name", "empty name")
  end)

  case("misuse: accelerators", function()
    local add = shell.keybindings.add
    local function accel(list)
      return function() add{ accelerators = list, on_pressed = noop } end
    end

    raises(accel("<Super>x"), "accelerators", "a string instead of a sequence")
    raises(accel({}), "accelerators", "an empty sequence")
    raises(accel({ 5 }), "accelerators", "a number")
    raises(accel({ "" }), "accelerators", "an empty accelerator")
    raises(accel({ "<Super>" }), "accelerators", "a modifier without a key")
    raises(accel({ "<Super x" }), "accelerators", "an unbalanced <")
    raises(accel({ "<>x" }), "accelerators", "an empty modifier")
    raises(accel({ "<Super>a b" }), "accelerators", "a space in the key name")
    raises(accel({ "a", "b", "c", "d", "e", "f", "g", "h", "i" }), "accelerators", "nine accelerators")
  end)

  case("misuse: modes", function()
    local add = shell.keybindings.add

    raises(function() add{ accelerators = { "<Super><Alt>u" }, modes = {}, on_pressed = noop } end,
           "modes", "empty modes")
    raises(function() add{ accelerators = { "<Super><Alt>u" }, modes = { "lock-screen" }, on_pressed = noop } end,
           { "modes", "lock-screen" }, "a mode that is not allowed")
    raises(function() add{ accelerators = { "<Super><Alt>u" }, modes = "normal", on_pressed = noop } end,
           "modes", "modes not a sequence")
    raises(function() add{ accelerators = { "<Super><Alt>u" }, modes = { 1 }, on_pressed = noop } end,
           "modes", "modes with a number")

    for _, modes in ipairs({ { "normal" }, { "overview" }, { "normal", "overview" } }) do
      local b = add{ accelerators = { "<Super><Alt>u" }, modes = modes, on_pressed = noop }
      truthy(b, "modes " .. table.concat(modes, ","))
      b:destroy()
    end
  end)

  case("misuse: settings and keys", function()
    local add = shell.keybindings.add

    raises(function() add{ name = "no-such-key", on_pressed = noop } end, "no key 'no-such-key'", "unknown key")
    raises(function() add{ name = "keybindings-test-title", on_pressed = noop } end,
           "must be of type 'as'", "a key of another type")
    raises(function() add{ name = TOGGLE, settings = 5, on_pressed = noop } end, nil, "settings not a handle")
    raises(function() add{ name = TOGGLE, settings = shell.log, on_pressed = noop } end, nil, "settings not a handle")
  end)

  case("on_pressed(fn) needs a function", function()
    local b = shell.keybindings.add{ accelerators = { "<Super><Alt>u" }, on_pressed = noop }

    truthy(b, "add")
    raises(function() b:on_pressed(5) end, nil, "a number")
    local conn = b:on_pressed(noop)
    truthy(conn, "a connection")
    conn:disconnect()
    b:destroy()
  end)

  case("the keybindings of the case are all released", function()
    -- everything above was destroyed: the combinations are free
    local b = shell.keybindings.add{ accelerators = { "<Super><Alt>u", "<Super><Alt>i" }, on_pressed = noop }
    truthy(b, "add")
    b:destroy()
  end)

  -- -------------------------------------------------------------------
  -- steps: the script presses the keys
  -- -------------------------------------------------------------------
  local handlers = {
    -- bindings that count their presses
    arm = function()
      kept.lit = shell.keybindings.add{
        accelerators = { "<Super><Alt>y" },
        on_pressed = function(binding)
          eq(binding, kept.lit, "on_pressed receives its binding")
          counts.lit = counts.lit + 1
        end,
      }
      truthy(kept.lit, "lit")
      -- a second callback, connected with the method, runs after the first
      kept.lit:on_pressed(function(binding)
        eq(binding, kept.lit, "the second callback receives the binding")
        counts.lit2 = counts.lit2 + 1
      end)

      kept.sb = shell.keybindings.add{
        name = TOGGLE,
        on_pressed = function(binding)
          eq(binding, kept.sb, "on_pressed receives its binding")
          counts.sb = counts.sb + 1
        end,
      }
      truthy(kept.sb, "sb")

      kept.ov = shell.keybindings.add{
        accelerators = { "<Super><Alt>o" },
        modes = { "overview" },
        on_pressed = function() counts.ov = counts.ov + 1 end,
      }
      truthy(kept.ov, "ov")

      kept.nm = shell.keybindings.add{
        accelerators = { "<Super><Alt>n" },
        modes = { "normal" },
        on_pressed = function() counts.nm = counts.nm + 1 end,
      }
      truthy(kept.nm, "nm")

      -- removes itself when pressed
      kept.once = shell.keybindings.add{
        accelerators = { "<Super><Alt>i" },
        on_pressed = function(binding)
          counts.once = counts.once + 1
          binding:destroy()
          -- the destruction is deferred until the callback returns (rule 8)
          eq(binding.alive, true, "alive until the callback returns")
        end,
      }
      truthy(kept.once, "once")
    end,

    -- wait-N {name, n, ms}: until counts[name] >= n
    wait = function(p, done)
      local ticks = 0
      local limit = (p.ms or 5000) // 25

      shell.timer.every{ ms = 25, callback = function()
        ticks = ticks + 1
        if counts[p.name] >= p.n then
          done()
          return false
        end
        if ticks > limit then
          T.report("wait " .. p.name, false, string.format("counts.%s is %d after %d ms, expected %d",
                   p.name, counts[p.name], limit * 25, p.n))
          done()
          return false
        end
      end }
      return "later"
    end,

    -- stay-N {name, n, ms}: after ms, counts[name] is exactly n
    stay = function(p, done)
      shell.timer.after{ ms = p.ms or 400, callback = function()
        case("stay " .. p.name, function() eq(counts[p.name], p.n, "counts." .. p.name) end)
        done()
      end }
      return "later"
    end,

    -- the second callback ran once for each press of the first
    same = function()
      eq(counts.lit2, counts.lit, "lit2 and lit")
      truthy(counts.lit >= 1, "lit ran")
    end,

    -- a rebinding of the key takes effect at once
    rebind = function()
      settings:set_strv(TOGGLE, { "<Super><Alt>k" })
      eq(table.concat(kept.sb.accelerators, " "), "<Super><Alt>k", "accelerators after the write")
    end,

    unbind = function()
      settings:set_strv(TOGGLE, {})
      eq(#kept.sb.accelerators, 0, "accelerators after clearing the key")
    end,

    restore = function()
      settings:reset(TOGGLE)
    end,

    -- the binding that removed itself is dead and its accelerator is free
    ["once-dead"] = function()
      eq(counts.once, 1, "once ran once")
      eq(kept.once.alive, false, "the handle is dead")
      local b = shell.keybindings.add{ accelerators = { "<Super><Alt>i" }, on_pressed = noop }
      truthy(b, "the accelerator is free again")
      b:destroy()
    end,

    ["destroy-lit"] = function()
      kept.lit:destroy()
      eq(kept.lit.alive, false, "dead")
      kept.sb:destroy()
      eq(kept.sb.alive, false, "dead")
    end,
  }

  -- The report keeps a set of finished labels, so the script gives each use
  -- of wait and stay a name of its own: wait-1, wait-2, ...
  for i = 1, 24 do
    handlers["wait-" .. i] = handlers.wait
    handlers["stay-" .. i] = handlers.stay
  end
  steps(handlers)

  T.done()
end

-- disable () is optional (README): this one checks the bindings are still
-- there while it runs and leaves the destruction to the context
function ext.disable()
  case("disable () runs before the bindings are destroyed", function()
    if kept.ov then
      eq(kept.ov.alive, true, "ov alive in disable ()")
    end
  end)
end

return ext
