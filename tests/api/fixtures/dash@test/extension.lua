-- Fixture of tests/api/dash.lua: every function, field, property and error
-- kind of docs/api/dash.md. enable () runs the checks that are synchronous;
-- the script of the test asks for those that need the overview (buttons added
-- and destroyed while the dash is shown) with steps (tests/api/README.md).
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

local UUID = "dash@test"

local ext = {}

-- Everything the steps keep between calls
local kept = {}

function ext.enable()
  -- -------------------------------------------------------------------
  -- The table
  -- -------------------------------------------------------------------
  case("shell.dash", function()
    eq(type(shell.dash), "table", "shell.dash")
    eq(type(shell.dash.add_button), "function", "shell.dash.add_button")
    eq(type(shell.dash.insert_at), "function", "shell.dash.insert_at")
    eq(shell.dash.no_such_function, nil, "an absent function is nil, not a stub")
  end)

  -- -------------------------------------------------------------------
  -- Misuse. Nothing is created by a call that raises (the limit of 8 below
  -- would be hit otherwise).
  -- -------------------------------------------------------------------
  case("add_button: required and unknown fields", function()
    raises(function() shell.dash.add_button() end, nil, "a call without a table")
    raises(function() shell.dash.add_button("Terminal") end, nil, "a string instead of a table")
    raises(function() shell.dash.add_button{ icon_name = "alarm-symbolic" } end, "label", "no label")
    raises(function() shell.dash.add_button{ label = "", icon_name = "alarm-symbolic" } end, "label", "an empty label")
    raises(function() shell.dash.add_button{ label = 5, icon_name = "alarm-symbolic" } end, "label", "a number as label")
    raises(function() shell.dash.add_button{ label = "x" } end, "icon", "no icon")
    raises(function() shell.dash.add_button{ label = "x", icon_name = "a-symbolic", icon_file = "icon.svg" } end,
           "icon", "both icons")
    raises(function() shell.dash.add_button{ label = "x", icon_name = "a-symbolic", bogus = 1 } end, "bogus",
           "an unknown field")
    raises(function() shell.dash.add_button{ label = "x", icon_name = "a-symbolic", index = 1 } end, "index",
           "index is for insert_at")
    raises(function() shell.dash.add_button{ label = "x", icon_name = 5 } end, "icon_name", "a number as icon_name")
  end)

  case("add_button: position, flags and callback", function()
    local base = { label = "x", icon_name = "a-symbolic" }
    local function with(extra)
      local t = {}
      for k, v in pairs(base) do t[k] = v end
      for k, v in pairs(extra) do t[k] = v end
      return t
    end

    raises(function() shell.dash.add_button(with{ position = "middle" }) end, "position", "an unknown position")
    raises(function() shell.dash.add_button(with{ position = 1 }) end, "position", "a number as position")
    raises(function() shell.dash.add_button(with{ on_click = 5 }) end, "on_click", "a number as on_click")
    raises(function() shell.dash.add_button(with{ hide_overview = "no" }) end, "hide_overview", "a string as hide_overview")
    raises(function() shell.dash.add_button(with{ checked = 1 }) end, "checked", "a number as checked")
    raises(function() shell.dash.add_button(with{ visible = "yes" }) end, "visible", "a string as visible")
    raises(function() shell.dash.add_button(with{ sensitive = {} }) end, "sensitive", "a table as sensitive")
  end)

  case("add_button: icon_file stays inside the extension directory", function()
    raises(function() shell.dash.add_button{ label = "x", icon_file = "/etc/hostname" } end, "icon_file",
           "an absolute path")
    raises(function() shell.dash.add_button{ label = "x", icon_file = "../x.svg" } end, "icon_file", "a path with ..")
    raises(function() shell.dash.add_button{ label = "x", icon_file = "missing.svg" } end, "icon_file", "a missing file")
    raises(function() shell.dash.add_button{ label = "x", icon_file = "extension.lua" } end, "icon_file", "a wrong suffix")
  end)

  case("insert_at: index is required and at least 1", function()
    raises(function() shell.dash.insert_at{ label = "x", icon_name = "a-symbolic" } end, "index", "no index")
    raises(function() shell.dash.insert_at{ label = "x", icon_name = "a-symbolic", index = 0 } end, "index", "index 0")
    raises(function() shell.dash.insert_at{ label = "x", icon_name = "a-symbolic", index = -2 } end, "index", "index -2")
    raises(function() shell.dash.insert_at{ label = "x", icon_name = "a-symbolic", index = 1.5 } end, "index", "index 1.5")
    raises(function() shell.dash.insert_at{ label = "x", icon_name = "a-symbolic", index = "1" } end, "index", "a string as index")
  end)

  -- -------------------------------------------------------------------
  -- A button, the defaults
  -- -------------------------------------------------------------------
  local added = {}
  local listener = shell.hooks.on("dash.item-added", function(p)
    added[#added + 1] = p
  end)

  local clicks = 0
  local btn = shell.dash.add_button{
    label = "Terminal", icon_name = "utilities-terminal-symbolic",
    on_click = function(self, button)
      clicks = clicks + 1
    end,
  }
  kept.btn = btn

  case("button: defaults", function()
    eq(type(btn), "userdata", "a button is a handle")
    eq(btn.alive, true, "alive")
    eq(btn.label, "Terminal", "label")
    eq(btn.icon_name, "utilities-terminal-symbolic", "icon_name")
    eq(btn.icon_file, nil, "icon_file")
    eq(btn.checked, false, "checked")
    eq(btn.visible, true, "visible")
    eq(btn.sensitive, true, "sensitive")
    eq(btn.hide_overview, true, "hide_overview")
    eq(btn.position, "end", "position defaults to end")
    eq(math.type(btn.index), "integer", "index is an integer")
    eq(btn.index, 1, "index of the first button of the extension")
    eq(btn.no_such_property, nil, "an unknown property reads as nil")
    truthy(btn == btn, "the same handle")
    eq(clicks, 0, "on_click is not called at creation")
  end)

  case("hook dash.item-added is emitted for the button", function()
    local ours

    for _, p in ipairs(added) do
      if p.kind == "button" and p.owner == UUID then
        ours = p
      end
    end

    truthy(ours, "a payload of kind button")
    eq(ours.owner, UUID, "owner")
    eq(math.type(ours.index), "integer", "index")
    truthy(ours.index >= 1, "index is 1-based")
    eq(ours["app-id"], nil, "a button has no app-id")
  end)

  case("button: read-only properties", function()
    raises(function() btn.position = "start" end, nil, "assigning position")
    raises(function() btn.index = 3 end, nil, "assigning index")
    raises(function() btn.alive = false end, nil, "assigning alive")
    raises(function() btn.no_such_property = 1 end, nil, "assigning an unknown property")
    eq(btn.position, "end", "position after the writes")
    eq(btn.index, 1, "index after the writes")
  end)

  case("button: read-write properties", function()
    btn.label = "Console"
    eq(btn.label, "Console", "label after the write")
    btn.label = "Terminal"
    btn.checked = true
    eq(btn.checked, true, "checked after the write")
    btn.checked = false
    btn.visible = false
    eq(btn.visible, false, "visible after hide")
    btn.visible = true
    eq(btn.visible, true, "visible after show")
    btn.sensitive = false
    eq(btn.sensitive, false, "sensitive after the write")
    btn.sensitive = true
    btn.hide_overview = false
    eq(btn.hide_overview, false, "hide_overview after the write")
    btn.hide_overview = true
    raises(function() btn.label = 5 end, nil, "a number as label")
    raises(function() btn.checked = "yes" end, nil, "a string as checked")
    raises(function() btn.visible = 1 end, nil, "a number as visible")
    raises(function() btn.sensitive = 0 end, nil, "a number as sensitive")
    raises(function() btn.hide_overview = "no" end, nil, "a string as hide_overview")
  end)

  case("button: an assignment is not a click (README rule 7)", function()
    local before = clicks

    btn.checked = true
    btn.checked = false
    btn.sensitive = false
    btn.sensitive = true
    eq(clicks, before, "calls of on_click after assignments")
  end)

  case("button: setting one icon clears the other", function()
    btn.icon_file = "icon.svg"
    eq(btn.icon_file, "icon.svg", "icon_file after the write")
    eq(btn.icon_name, nil, "icon_name is cleared by icon_file")
    btn.icon_name = "utilities-terminal-symbolic"
    eq(btn.icon_name, "utilities-terminal-symbolic", "icon_name after the write")
    eq(btn.icon_file, nil, "icon_file is cleared by icon_name")
    raises(function() btn.icon_file = "/etc/hostname" end, "icon_file", "an absolute path")
    raises(function() btn.icon_file = "../icon.svg" end, "icon_file", "a path with ..")
    raises(function() btn.icon_file = "missing.svg" end, "icon_file", "a missing file")
    raises(function() btn.icon_file = "extension.lua" end, "icon_file", "a wrong suffix")
    raises(function() btn.icon_name = 5 end, nil, "a number as icon_name")
    eq(btn.icon_name, "utilities-terminal-symbolic", "icon_name after the failed writes")
  end)

  case("button: on_click", function()
    local conn = btn:on_click(function() end)

    eq(type(conn), "userdata", "on_click returns a connection")
    eq(type(conn.disconnect), "function", "Connection:disconnect")
    conn:disconnect()
    raises(function() btn:on_click() end, nil, "on_click without a function")
    raises(function() btn:on_click(5) end, nil, "on_click with a number")
  end)

  case("button: icon_file given at creation", function()
    local f = shell.dash.add_button{ label = "File", icon_file = "icon.svg" }

    eq(f.icon_file, "icon.svg", "icon_file")
    eq(f.icon_name, nil, "icon_name")
    f:destroy()
  end)

  -- -------------------------------------------------------------------
  -- Groups and indices
  -- -------------------------------------------------------------------
  case("start group: add_button and insert_at", function()
    local a = shell.dash.add_button{ label = "A", icon_name = "a-symbolic", position = "start" }
    local b = shell.dash.add_button{ label = "B", icon_name = "a-symbolic", position = "start" }

    eq(a.position, "start", "position of a")
    eq(a.index, 1, "index of a")
    eq(b.index, 2, "index of b")

    local c = shell.dash.insert_at{ label = "C", icon_name = "a-symbolic", position = "start", index = 1 }
    eq(c.index, 1, "index of c, inserted first")
    eq(a.index, 2, "index of a after c was inserted before it")
    eq(b.index, 3, "index of b after c was inserted before it")

    local d = shell.dash.insert_at{ label = "D", icon_name = "a-symbolic", position = "start", index = 99 }
    eq(d.index, 4, "an index past the end appends")

    local e = shell.dash.insert_at{ label = "E", icon_name = "a-symbolic", position = "start", index = 3 }
    eq(e.index, 3, "index of e")
    eq(b.index, 4, "index of b after e")
    eq(d.index, 5, "index of d after e")

    -- the button of the other group has its own indices
    eq(btn.index, 1, "index in the end group")

    b:destroy()
    eq(b.alive, false, "b is destroyed")
    eq(e.index, 3, "index of e after b was destroyed")
    eq(d.index, 4, "index of d after b was destroyed")

    kept.start = { a, c, d, e }
  end)

  case("hook dash.item-added reports the position among the icons of the dash", function()
    -- the start group comes first: the buttons of the group are 1, 2, ...
    local first
    for _, p in ipairs(added) do
      if p.kind == "button" and p.owner == UUID and p.index == 1 then
        first = p
      end
    end
    truthy(first, "a payload with index 1 (the first button of the start group)")
    local n = 0
    for _, p in ipairs(added) do
      if p.kind == "button" then
        n = n + 1
        eq(p.owner, UUID, "owner")
      end
    end
    truthy(n >= 6, "payloads of kind button: " .. n)
  end)

  case("at most 8 buttons per extension", function()
    local made = {}
    local total = 0

    -- btn, a, c, d, e are alive: five of the eight
    total = 5
    while total < 8 do
      made[#made + 1] = shell.dash.add_button{ label = "F" .. total, icon_name = "a-symbolic" }
      total = total + 1
    end
    raises(function() shell.dash.add_button{ label = "9", icon_name = "a-symbolic" } end, nil, "the ninth button")
    raises(function() shell.dash.insert_at{ label = "9", icon_name = "a-symbolic", index = 1 } end, nil,
           "the ninth button, insert_at")

    -- destroying one frees its slot at once
    made[1]:destroy()
    local again = shell.dash.add_button{ label = "again", icon_name = "a-symbolic" }
    eq(again.alive, true, "a button fits after one was destroyed")
    made[1] = again
    for _, m in ipairs(made) do
      m:destroy()
    end
  end)

  -- -------------------------------------------------------------------
  -- destroy ()
  -- -------------------------------------------------------------------
  case("destroy: kills the handle at once, is idempotent", function()
    local d = shell.dash.add_button{ label = "Doomed", icon_name = "a-symbolic" }

    d:destroy()
    eq(d.alive, false, "alive after destroy")
    raises(function() return d.label end, "no longer valid", "reading a property of a dead handle")
    raises(function() d.label = "x" end, "no longer valid", "writing a property of a dead handle")
    raises(function() d:on_click(function() end) end, "no longer valid", "a method of a dead handle")
    d:destroy()
    d:destroy()
    eq(tostring(d):find("dead", 1, true) ~= nil, true, "tostring of a dead handle")
  end)

  -- -------------------------------------------------------------------
  -- The steps of the script
  -- -------------------------------------------------------------------
  steps {
    -- the overview is shown: a button added now fades in, a button destroyed
    -- fades out
    ["overview-add"] = function()
      kept.shown = shell.dash.add_button{ label = "Shown", icon_name = "a-symbolic", position = "start" }
      eq(kept.shown.alive, true, "alive")
      eq(kept.shown.index, 1 + #kept.start, "index among the buttons of the group")
      kept.hidden = shell.dash.add_button{ label = "Hidden", icon_name = "a-symbolic", visible = false }
      eq(kept.hidden.visible, false, "visible = false at creation")
    end,

    ["overview-edit"] = function()
      kept.shown.label = "Shown, renamed"
      kept.shown.icon_name = "alarm-symbolic"
      kept.shown.checked = true
      kept.hidden.visible = true
      eq(kept.hidden.visible, true, "visible after show")
      kept.shown.visible = false
      kept.shown.visible = true
    end,

    ["overview-destroy"] = function()
      kept.shown:destroy()
      eq(kept.shown.alive, false, "the handle is dead at once")
      kept.hidden:destroy()
      eq(kept.hidden.alive, false, "the handle is dead at once")
    end,
  }

  listener:disconnect()
  T.done()
end

-- Defining disable () is optional (README): this one only checks what it can
function ext.disable()
  case("disable () runs before the objects are destroyed", function()
    eq(kept.btn.alive, true, "the button is alive in disable ()")
  end)
end

return ext
