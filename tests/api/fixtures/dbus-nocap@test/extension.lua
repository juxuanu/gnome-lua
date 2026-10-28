-- Fixture of tests/api/dbus.lua: without "dbus" in the capabilities of
-- metadata.json every function of shell.dbus raises (docs/api/dbus.md, README.md
-- "Capabilities"), and there is nothing to get at in any other way.
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

local ext = {}

function ext.enable()
  case("shell.dbus exists, its functions raise", function()
    eq(type(shell.dbus), "table", "shell.dbus")
    for _, name in ipairs({ "session", "system", "variant" }) do
      eq(type(shell.dbus[name]), "function", "shell.dbus." .. name)
    end
    raises(function() shell.dbus.session() end,
           { "shell.dbus.session()", "requires the \"dbus\" capability" }, "shell.dbus.session ()")
    raises(function() shell.dbus.system() end,
           { "shell.dbus.system()", "requires the \"dbus\" capability" }, "shell.dbus.system ()")
    raises(function() shell.dbus.variant("u", 5) end,
           { "shell.dbus.variant()", "requires the \"dbus\" capability" }, "shell.dbus.variant ()")
  end)

  case("the capabilities the extension declares", function()
    eq(#shell.extension.capabilities, 0, "capabilities")
  end)

  T.done()
end

return ext
