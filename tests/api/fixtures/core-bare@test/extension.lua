-- Fixture of tests/api/core.lua: an extension without capabilities and
-- without a settings schema (docs/api/core.md, README.md "Capabilities").
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

function ext.enable()
  case("shell.extension of an extension without capabilities", function()
    eq(shell.extension.uuid, "core-bare@test", "uuid")
    eq(#shell.extension.capabilities, 0, "no capability")
    eq(shell.extension.capabilities[1], nil, "capabilities[1]")
    eq(shell.extension.metadata["settings-schema"], nil, "no settings-schema")
    eq(shell.extension.metadata.capabilities, nil, "no capabilities in the metadata")
  end)

  case("shell.settings.extension needs a settings-schema", function()
    raises(function() shell.settings.extension() end, "settings-schema", "settings.extension()")
  end)

  case("shell.settings.open needs the settings capability", function()
    local err = raises(function() shell.settings.open("org.gnome.desktop.interface") end,
                       'requires the "settings" capability', "settings.open()")
    truthy(err:find("open", 1, true), "the error names the function: " .. err)
    raises(function() shell.settings.open("org.gnome-lua.test.no-such-schema") end,
           'requires the "settings" capability', "settings.open() of a schema that is not installed")
  end)

  case("gettext without a locale directory", function()
    eq(_("Hello"), "Hello", "_()")
    eq(ngettext("one", "many", 3), "many", "ngettext")
    eq(pgettext("ctx", "Hello"), "Hello", "pgettext")
  end)

  T.done()
end

return ext
