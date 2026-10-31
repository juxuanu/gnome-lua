-- Fixture of tests/api/panel.lua: every function, field, property and error
-- kind of docs/api/panel.md. enable () runs the checks that are synchronous;
-- the script of the test asks for those that need the shell to be driven
-- (placement, opening the menu) with steps (tests/api/README.md).
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

local UUID = "panel@test"
local BUILTIN_ROLES = {
  "activities", "quickSettings", "dateMenu", "a11y", "keyboard", "dwellClick",
  "screenRecording", "screenSharing",
}

local ext = {}

-- Everything the steps keep between calls
local kept = {}

function ext.enable()
  -- -------------------------------------------------------------------
  -- The table
  -- -------------------------------------------------------------------
  case("shell.panel", function()
    eq(type(shell.panel), "table", "shell.panel")
    eq(type(shell.panel.add_indicator), "function", "shell.panel.add_indicator")
    eq(shell.panel.no_such_function, nil, "an absent function is nil, not a stub")
  end)

  -- -------------------------------------------------------------------
  -- add_indicator: misuse. Nothing is created by a call that raises, so the
  -- status area is untouched whatever fails.
  -- -------------------------------------------------------------------
  case("add_indicator: required and unknown fields", function()
    local add = shell.panel.add_indicator

    raises(function() add() end, nil, "a call without a table")
    raises(function() add("Clock") end, nil, "a string instead of a table")
    raises(function() add{} end, "accessible_name", "a table without accessible_name")
    raises(function() add{ icon_name = "alarm-symbolic" } end, "accessible_name", "no accessible_name")
    raises(function() add{ accessible_name = "", icon_name = "alarm-symbolic" } end, "accessible_name",
           "an empty accessible_name")
    raises(function() add{ accessible_name = 5 } end, "accessible_name", "a number as accessible_name")
    raises(function() add{ accessible_name = "x", bogus = 1 } end, "bogus", "an unknown field")
    raises(function() add{ accessible_name = "x", menu = "yes" } end, "menu", "a string as menu")
    raises(function() add{ accessible_name = "x", visible = 1 } end, "visible", "a number as visible")
    raises(function() add{ accessible_name = "x", icon_name = 5 } end, "icon_name", "a number as icon_name")
  end)

  case("add_indicator: name", function()
    local add = shell.panel.add_indicator

    raises(function() add{ accessible_name = "x", name = "" } end, "name", "an empty name")
    raises(function() add{ accessible_name = "x", name = "has space" } end, "name", "a space in the name")
    raises(function() add{ accessible_name = "x", name = "slash/y" } end, "name", "a slash in the name")
    raises(function() add{ accessible_name = "x", name = ("n"):rep(129) } end, "name", "a name of 129 characters")
    raises(function() add{ accessible_name = "x", name = 5 } end, "name", "a number as name")
  end)

  case("add_indicator: names of the shell are taken", function()
    for _, role in ipairs(BUILTIN_ROLES) do
      raises(function()
        shell.panel.add_indicator{ accessible_name = "x", name = role, icon_name = "alarm-symbolic" }
      end, nil, "the name " .. role)
    end
  end)

  case("add_indicator: position and edge", function()
    local add = shell.panel.add_indicator

    raises(function() add{ accessible_name = "x", position = "top" } end, "position", "an unknown position")
    raises(function() add{ accessible_name = "x", position = 1 } end, "position", "a number as position")
    raises(function() add{ accessible_name = "x", edge = "middle" } end, "edge", "an unknown edge")
    raises(function() add{ accessible_name = "x", edge = false } end, "edge", "a boolean as edge")
  end)

  case("add_indicator: index", function()
    local add = shell.panel.add_indicator

    raises(function() add{ accessible_name = "x", icon_name = "a-symbolic", index = 0 } end, "index", "index 0")
    raises(function() add{ accessible_name = "x", icon_name = "a-symbolic", index = -1 } end, "index", "index -1")
    raises(function() add{ accessible_name = "x", icon_name = "a-symbolic", index = 1.5 } end, "index", "index 1.5")
    raises(function() add{ accessible_name = "x", icon_name = "a-symbolic", index = "1" } end, "index", "a string as index")
  end)

  case("add_indicator: one content field at most", function()
    local add = shell.panel.add_indicator

    raises(function() add{ accessible_name = "x", icon_name = "a-symbolic", text = "t" } end, nil,
           "icon_name and text")
    raises(function() add{ accessible_name = "x", icon_name = "a-symbolic", icon_file = "x.svg" } end, nil,
           "icon_name and icon_file")
    raises(function() add{ accessible_name = "x", icon_file = "x.svg", text = "t" } end, nil,
           "icon_file and text")
  end)

  case("add_indicator: icon_file stays inside the extension directory", function()
    local add = shell.panel.add_indicator

    raises(function() add{ accessible_name = "x", icon_file = "/etc/hostname" } end, "icon_file", "an absolute path")
    raises(function() add{ accessible_name = "x", icon_file = "../x.svg" } end, "icon_file", "a path with ..")
    raises(function() add{ accessible_name = "x", icon_file = "a/../../x.svg" } end, "icon_file", "a path that climbs")
    raises(function() add{ accessible_name = "x", icon_file = "missing.svg" } end, "icon_file", "a missing file")
    raises(function() add{ accessible_name = "x", icon_file = "extension.lua" } end, "icon_file", "a wrong suffix")
  end)

  case("add_indicator: on_click and menu", function()
    local add = shell.panel.add_indicator

    raises(function() add{ accessible_name = "x", icon_name = "a-symbolic", on_click = function() end, menu = true } end,
           nil, "on_click with menu = true")
    raises(function() add{ accessible_name = "x", icon_name = "a-symbolic", on_click = 5 } end, "on_click",
           "a number as on_click")
  end)

  case("add_indicator: a call that raised left no name taken", function()
    -- the name of the extension is the default one and none of the calls above took it
    local ind = shell.panel.add_indicator{ accessible_name = "Probe", icon_name = "alarm-symbolic" }

    eq(ind.name, UUID, "the default name")
    ind:destroy()
  end)

  -- -------------------------------------------------------------------
  -- The indicator with a menu, the defaults
  -- -------------------------------------------------------------------
  local added = {}
  local listener = shell.hooks.on("panel.indicator-added", function(p)
    added[#added + 1] = p
  end)

  local ind = shell.panel.add_indicator{ accessible_name = "Panel fixture", icon_name = "alarm-symbolic" }
  kept.ind = ind

  case("indicator: defaults", function()
    eq(type(ind), "userdata", "an indicator is a handle")
    eq(ind.alive, true, "alive")
    eq(ind.name, UUID, "name defaults to the uuid of the extension")
    eq(ind.position, "right", "position defaults to right")
    eq(ind.accessible_name, "Panel fixture", "accessible_name")
    eq(ind.visible, true, "visible")
    eq(ind.sensitive, true, "sensitive")
    eq(ind.active, false, "active")
    eq(ind.no_such_property, nil, "an unknown property reads as nil")
    truthy(ind == ind, "the same handle")
  end)

  case("indicator: the menu", function()
    local menu = ind.menu

    eq(type(menu), "userdata", "menu is a handle")
    truthy(menu == ind.menu, "the same object is the same handle")
    eq(menu.alive, true, "menu.alive")
    eq(menu.is_open, false, "menu.is_open")
    eq(menu.n_items, 0, "menu.n_items")
  end)

  case("indicator: the child made by icon_name", function()
    local child = ind.child

    eq(type(child), "userdata", "child is a handle")
    truthy(child == ind.child, "the same object is the same handle")
    if shell.ui then
      eq(child.icon_name, "alarm-symbolic", "icon_name of the child")
    end
  end)

  case("hook panel.indicator-added is emitted for the indicator", function()
    local ours
    for _, p in ipairs(added) do
      if p.name == UUID then
        ours = p
      end
    end

    truthy(ours, "a payload for " .. UUID)
    eq(ours.box, "right", "box")
    eq(ours.owner, UUID, "owner is the uuid of the extension")
  end)

  case("add_indicator: a name that is taken", function()
    local add = shell.panel.add_indicator

    raises(function() add{ accessible_name = "again", icon_name = "alarm-symbolic" } end, nil,
           "the default name, taken by the indicator above")
    raises(function() add{ accessible_name = "again", name = UUID, icon_name = "alarm-symbolic" } end, nil,
           "the same name explicitly")
    local n = 0
    for _, p in ipairs(added) do
      if p.name == UUID then
        n = n + 1
      end
    end
    eq(n, 1, "payloads of panel.indicator-added for the name")
  end)

  case("indicator: read-only properties", function()
    raises(function() ind.name = "other" end, nil, "assigning name")
    raises(function() ind.position = "left" end, nil, "assigning position")
    raises(function() ind.menu = nil end, nil, "assigning menu")
    raises(function() ind.active = true end, nil, "assigning active")
    raises(function() ind.alive = false end, nil, "assigning alive")
    raises(function() ind.no_such_property = 1 end, nil, "assigning an unknown property")
    eq(ind.name, UUID, "name after the writes")
    eq(ind.position, "right", "position after the writes")
  end)

  case("indicator: accessible_name, visible, sensitive", function()
    ind.accessible_name = "Renamed"
    eq(ind.accessible_name, "Renamed", "accessible_name after the write")
    ind.accessible_name = "Panel fixture"
    ind.visible = false
    eq(ind.visible, false, "visible after hide")
    ind.visible = true
    eq(ind.visible, true, "visible after show")
    ind.sensitive = false
    eq(ind.sensitive, false, "sensitive after the write")
    ind.sensitive = true
    eq(ind.sensitive, true, "sensitive after the write")
    raises(function() ind.visible = "yes" end, nil, "a string as visible")
    raises(function() ind.sensitive = 1 end, nil, "a number as sensitive")
    raises(function() ind.accessible_name = 5 end, nil, "a number as accessible_name")
  end)

  case("indicator: on_click is only for indicators without a menu", function()
    raises(function() ind:on_click(function() end) end, nil, "on_click of an indicator with a menu")
  end)

  case("indicator: assignments that do not close an open menu (README rule 7)", function()
    local menu = ind.menu
    local calls = 0

    menu:on_open_state_changed(function() calls = calls + 1 end)
    ind.visible = false
    ind.visible = true
    ind.sensitive = false
    ind.sensitive = true
    eq(calls, 0, "the menu was closed: no open state changed")
  end)

  -- -------------------------------------------------------------------
  -- Other contents and kinds of indicator
  -- -------------------------------------------------------------------
  case("indicator: text shorthand", function()
    local t = shell.panel.add_indicator{ accessible_name = "Text", name = "text-ind", text = "T1" }

    eq(t.name, "text-ind", "name")
    eq(type(t.child), "userdata", "child")
    if shell.ui then
      eq(t.child.text, "T1", "text of the child")
    end
    t:destroy()
  end)

  case("indicator: no content", function()
    local n = shell.panel.add_indicator{ accessible_name = "Empty", name = "empty-ind" }

    eq(n.child, nil, "child of an indicator without content")
    n:destroy()
  end)

  case("indicator: icon_file shorthand", function()
    local f = shell.panel.add_indicator{ accessible_name = "File", name = "file-ind", icon_file = "icon.svg" }

    eq(type(f.child), "userdata", "child")
    if shell.ui then
      eq(f.child.icon_file, "icon.svg", "icon_file of the child")
    end
    f:destroy()
  end)

  case("indicator: menu = false and on_click", function()
    local clicks = 0
    local plain = shell.panel.add_indicator{
      accessible_name = "Plain", name = "plain-ind", icon_name = "alarm-symbolic",
      on_click = function(self, button)
        clicks = clicks + 1
        kept.click_self, kept.click_button = self, button
      end,
    }

    eq(plain.menu, nil, "an on_click indicator has no menu")
    eq(clicks, 0, "on_click is not called at creation")
    local conn = plain:on_click(function() end)
    eq(type(conn), "userdata", "on_click returns a connection")
    eq(type(conn.disconnect), "function", "Connection:disconnect")
    conn:disconnect()
    kept.plain = plain

    local nomenu = shell.panel.add_indicator{
      accessible_name = "No menu", name = "nomenu-ind", icon_name = "alarm-symbolic", menu = false,
    }
    eq(nomenu.menu, nil, "menu = false has no menu")
    nomenu:destroy()
  end)

  case("indicator: child given as a widget", function()
    if not shell.ui then
      return
    end

    local label = shell.ui.label{ text = "W" }
    local w = shell.panel.add_indicator{ accessible_name = "Widget", name = "widget-ind", child = label }

    truthy(w.child == label, "child is the widget that was given")
    -- a widget with a parent cannot be given again
    raises(function()
      shell.panel.add_indicator{ accessible_name = "Twice", name = "twice-ind", child = label }
    end, nil, "a widget that already has a parent")
    raises(function() w.child = label end, nil, "assigning the child that is already there")

    -- assigning a child replaces and destroys the previous one
    local other = shell.ui.label{ text = "W2" }
    w.child = other
    eq(label.alive, false, "the previous child is destroyed")
    truthy(w.child == other, "the new child")
    w.child = nil
    eq(other.alive, false, "assigning nil destroys the child")
    eq(w.child, nil, "no child")
    w:destroy()
  end)

  -- -------------------------------------------------------------------
  -- destroy ()
  -- -------------------------------------------------------------------
  case("destroy: frees the name, kills the handles, is idempotent", function()
    local d = shell.panel.add_indicator{ accessible_name = "Doomed", name = "doomed-ind", icon_name = "alarm-symbolic" }
    local child, menu = d.child, d.menu

    d:destroy()
    eq(d.alive, false, "alive after destroy")
    eq(child.alive, false, "the child dies with the indicator")
    eq(menu.alive, false, "the menu dies with the indicator")
    raises(function() return d.name end, "no longer valid", "reading a property of a dead handle")
    raises(function() d.visible = false end, "no longer valid", "writing a property of a dead handle")
    raises(function() d:on_click(function() end) end, "no longer valid", "a method of a dead handle")
    d:destroy()
    d:destroy()
    eq(tostring(d):find("dead", 1, true) ~= nil, true, "tostring of a dead handle")

    -- the name is free again
    local again = shell.panel.add_indicator{ accessible_name = "Reborn", name = "doomed-ind", icon_name = "alarm-symbolic" }
    truthy(again ~= d, "a new handle")
    eq(again.alive, true, "alive")
    again:destroy()
  end)

  -- -------------------------------------------------------------------
  -- The steps of the script
  -- -------------------------------------------------------------------
  local made = {}

  local function make(name, position, edge, index)
    local ind2 = shell.panel.add_indicator{
      accessible_name = name, name = name, icon_name = "alarm-symbolic",
      position = position, edge = edge, index = index,
    }
    made[#made + 1] = ind2
    return ind2
  end

  steps {
    -- Indicators in every slot: the script reads where they are
    placement = function()
      -- right box, edge start: asked out of order
      make("r-start-2", "right", "start")
      make("r-start-3", "right", "start")
      make("r-start-1", "right", "start", 1)
      make("r-start-clamped", "right", "start", 99)
      -- right box, edge end
      make("r-end-1", "right", "end")
      make("r-end-2", "right", "end")
      -- the other boxes
      make("l-start-1", "left", "start")
      make("l-start-2", "left", "start")
      make("l-end-1", "left", "end")
      make("c-start-1", "center", "start")
      make("c-end-1", "center", "end")
      make("c-end-0", "center", "end", 1)

      eq(made[1].position, "right", "position of r-start-2")
      eq(made[7].position, "left", "position of l-start-1")
      eq(made[10].position, "center", "position of c-start-1")
    end,

    ["placement-destroy"] = function()
      -- destroying an indicator frees its place, the others keep their order
      for _, m in ipairs(made) do
        if m.name == "r-start-3" then
          m:destroy()
        end
      end
    end,

    -- the menu of the indicator, opened as the user would
    ["menu-items"] = function()
      local m = ind.menu

      m:add_item{ label = "One" }
      m:add_separator{}
      m:add_item{ label = "Two" }
      eq(m.n_items, 3, "n_items")
      kept.opened = {}
      kept.panel_menu_opened = {}
      kept.opened_conn = m:on_open_state_changed(function(menu, open)
        kept.opened[#kept.opened + 1] = open
      end)
      kept.hook_conn = shell.hooks.on("panel.menu-opened", function(p)
        kept.panel_menu_opened[#kept.panel_menu_opened + 1] = p
      end)
    end,

    ["menu-open"] = function()
      eq(ind.menu:open({ animate = false }), true, "open () of a menu with items")
      eq(ind.menu.is_open, true, "is_open")
      eq(ind.active, true, "active while the menu is open")
      eq(kept.opened[#kept.opened], true, "on_open_state_changed (true)")
      local last = kept.panel_menu_opened[#kept.panel_menu_opened]
      truthy(last, "panel.menu-opened was emitted")
      eq(last.name, UUID, "panel.menu-opened, name")
      eq(last.box, "right", "panel.menu-opened, box")
    end,

    ["menu-hide"] = function()
      -- hiding an indicator closes its menu, which calls back (README rule 7)
      ind.visible = false
      eq(ind.menu.is_open, false, "the menu is closed when the indicator is hidden")
      eq(ind.active, false, "active after the menu closed")
      eq(kept.opened[#kept.opened], false, "on_open_state_changed (false)")
      ind.visible = true
    end,

    ["menu-clear"] = function()
      ind.menu:clear()
      eq(ind.menu.n_items, 0, "n_items after clear")
      eq(ind.menu:open({ animate = false }), false, "open () of an empty menu")
      eq(ind.active, false, "active")
    end,
  }

  listener:disconnect()
  T.done()
end

-- Defining disable () is optional (README): this one only checks what it can
function ext.disable()
  case("disable () runs before the objects are destroyed", function()
    eq(kept.ind.alive, true, "the indicator is alive in disable ()")
  end)
end

return ext
