-- Fixture of tests/api/arguments.lua: README calling convention 1, the
-- options table and the tables in it that the API reads are plain tables.
-- Every module refuses a table with a metatable, so no code of the extension
-- runs while the API checks and uses its arguments: the metamethods used to
-- destroy a widget between its check and its use, or to answer a second read
-- with something the first one did not see. The shorthand fields of the panel
-- use the constructors of shell.ui, not the (writable) shell table.
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

local PLAIN = "plain table"

-- A table with a metatable whose metamethods would run code of ours
local function hostile(t, on_touch)
  return setmetatable(t, {
    __index = function(_, k) if on_touch then on_touch(k) end return nil end,
    __len = function() if on_touch then on_touch("#") end return 1 end,
  })
end

local ext = {}

function ext.enable()
  local touched = 0
  local function touch() touched = touched + 1 end
  local function nop() end

  case("an options table with a metatable is refused", function()
    raises(function() shell.ui.label(hostile({ text = "x" }, touch)) end, PLAIN, "shell.ui.label")
    raises(function() shell.panel.add_indicator(hostile({ name = "args-a", accessible_name = "A" }, touch)) end, PLAIN, "add_indicator")
    raises(function() shell.notify(hostile({ title = "t" }, touch)) end, PLAIN, "shell.notify")
    raises(function() shell.keybindings.add(hostile({ accelerators = { "<Super>F12" }, on_pressed = nop }, touch)) end,
           PLAIN, "keybindings.add")
    raises(function() shell.network.request(hostile({ url = "http://127.0.0.1:9/" }, touch)) end, PLAIN, "network.request")
    raises(function() shell.search.add_provider(hostile({ id = "a", name = "A", on_search = nop, on_activate = nop }, touch)) end,
           PLAIN, "search.add_provider")
    raises(function() shell.timer.after(hostile({ ms = 1, callback = nop }, touch)) end, PLAIN, "timer.after")
    eq(touched, 0, "metamethods run by the API")
  end)

  case("a table inside the options with a metatable is refused", function()
    raises(function() shell.keybindings.add{ accelerators = hostile({ "<Super>F12" }, touch), on_pressed = nop } end,
           { PLAIN, "accelerators" }, "accelerators")
    raises(function() shell.keybindings.add{ accelerators = { "<Super>F12" }, modes = hostile({ "normal" }, touch), on_pressed = nop } end,
           { PLAIN, "modes" }, "modes")
    raises(function() shell.notify{ title = "t", actions = hostile({ { label = "a", on_activate = nop } }, touch) } end,
           { PLAIN, "actions" }, "actions")
    raises(function() shell.notify{ title = "t", actions = { hostile({ label = "a", on_activate = nop }, touch) } } end,
           PLAIN, "an action")
    raises(function() shell.network.request{ url = "http://127.0.0.1:9/", headers = hostile({ Accept = "x" }, touch) } end,
           { PLAIN, "headers" }, "headers")
    raises(function() shell.ui.box{ children = hostile({}, touch) } end, { PLAIN, "children" }, "children")
    eq(touched, 0, "metamethods run by the API")
  end)

  -- The attacks of the audit: a metamethod that destroys the child between
  -- its check and its use, a second read that sees something else
  case("a metamethod cannot destroy a child between its check and its use", function()
    local child = shell.ui.label{ text = "child" }
    raises(function()
      shell.panel.add_indicator(setmetatable({ name = "args-b", accessible_name = "B", child = child },
                                             { __index = function() child:destroy() end }))
    end, PLAIN, "add_indicator")
    eq(child.alive, true, "the child")
    local ind = shell.panel.add_indicator{ name = "args-c", accessible_name = "C" }
    raises(function()
      ind.menu:add_widget(setmetatable({ child = child }, { __index = function() child:destroy() end }))
    end, PLAIN, "add_widget")
    eq(child.alive, true, "the child")
    local item = ind.menu:add_widget{ child = child }
    eq(item.alive, true, "a plain table still works")
    ind:destroy()
  end)

  case("the shorthand fields of an indicator use the constructors of shell.ui", function()
    local label, icon = shell.ui.label, shell.ui.icon
    local hijacked = 0
    shell.ui.label = function() hijacked = hijacked + 1 error("not this one") end
    shell.ui.icon = function() hijacked = hijacked + 1 error("not this one") end
    local ok, a = pcall(shell.panel.add_indicator, { name = "args-text", accessible_name = "Text", text = "hi" })
    local ok2, b = pcall(shell.panel.add_indicator, { name = "args-icon", accessible_name = "Icon", icon_name = "edit-find-symbolic" })
    shell.ui.label, shell.ui.icon = label, icon
    truthy(ok, "add_indicator with text: " .. tostring(a))
    truthy(ok2, "add_indicator with icon_name: " .. tostring(b))
    eq(hijacked, 0, "replaced constructors called")
    eq(a.child.text, "hi", "the label")
    a:destroy()
    b:destroy()
  end)

  case("pairs of shell.overview ends on a key it does not know", function()
    local next_member = pairs(shell.overview)
    eq(next_member(shell.overview, "bogus"), nil, "the key after an unknown one")
    eq(next_member(shell.overview, "toggle"), nil, "the key after the last one")
  end)

  T.done("main")
end

function ext.disable()
end

return ext
