-- Fixture of tests/api/menu.lua: every method, field, property, event and
-- error kind of docs/api/menu.md. enable () runs the checks that need nothing
-- but the extension; the script of the test asks for those that need the
-- shell to be driven (opening the menu as the user would) with steps
-- (tests/api/README.md).
--
-- Not covered here: activation by the user (a click, Enter, Space), which
-- needs input; what is checked is what an assignment calls back (README
-- rule 7: the callbacks of the state it changes, never those of a user
-- action) and that the on_<event> methods connect.
local shell = require("shell")
local T = shell.test
local ui = shell.ui

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
local kept = {}

local function labels(menu)
  local out = {}
  for i, item in ipairs(menu:items()) do
    out[i] = item.label
  end
  return table.concat(out, ",")
end

function ext.enable()
  local ind = shell.panel.add_indicator{ accessible_name = "Menu test", icon_name = "alarm-symbolic" }
  local m = ind.menu
  kept.ind, kept.menu = ind, m

  -- A failed check must not leave its items behind for the cases after it:
  -- every case of this menu ends with an empty menu.
  local plain_case = case
  local function case(name, fn, ...)
    return plain_case(name, function(...)
      local ok, err = pcall(fn, ...)
      m:clear()
      if not ok then
        error(err, 0)
      end
    end, ...)
  end

  -- -------------------------------------------------------------------
  -- The menu
  -- -------------------------------------------------------------------
  case("shell.Menu: properties", function()
    truthy(tostring(m):find("shell.Menu", 1, true), "tostring")
    eq(m.alive, true, "alive")
    eq(m.is_open, false, "is_open")
    eq(m.n_items, 0, "n_items of a new menu")
    eq(m.sensitive, true, "sensitive")
    eq(m.nonsense, nil, "an unknown property reads as nil")
    eq(ind.menu, m, "the same handle every time")
    eq(#m:items(), 0, "items () of an empty menu")
    raises(function() m.is_open = true end, "is_open", "is_open is read-only")
    raises(function() m.n_items = 3 end, "n_items", "n_items is read-only")
    raises(function() m.alive = false end, "alive", "alive is read-only")
    raises(function() m.nonsense = 1 end, "nonsense", "an unknown property")
    raises(function() m.sensitive = "no" end, "sensitive", "a string as sensitive")
    eq(m:open({ animate = false }), false, "open () of an empty menu")
    eq(m.is_open, false, "still closed")
    eq(m:close({ animate = false }), false, "close () of a closed menu")
  end)

  -- -------------------------------------------------------------------
  -- add_item
  -- -------------------------------------------------------------------
  case("add_item: misuse", function()
    raises(function() m:add_item() end, nil, "no table")
    raises(function() m:add_item("x") end, nil, "a string")
    raises(function() m:add_item{} end, "label", "no label")
    raises(function() m:add_item{ label = 5 } end, "label", "a number as label")
    raises(function() m:add_item{ label = "x", bogus = 1 } end, "bogus", "an unknown field")
    raises(function() m:add_item{ label = "x", icon_name = "alarm-symbolic" } end, "icon_name", "icon_name on a plain item")
    raises(function() m:add_item{ label = "x", on_toggled = function() end } end, "on_toggled", "on_toggled on a plain item")
    raises(function() m:add_item{ label = "x", sensitive = 1 } end, "sensitive", "a number as sensitive")
    raises(function() m:add_item{ label = "x", visible = "yes" } end, "visible", "a string as visible")
    raises(function() m:add_item{ label = "x", ornament = "star" } end, "ornament", "an unknown ornament")
    raises(function() m:add_item{ label = "x", on_activate = 5 } end, "on_activate", "a number as on_activate")
    raises(function() m:add_item{ label = "x", index = 0 } end, "index", "index 0")
    raises(function() m:add_item{ label = "x", index = -2 } end, "index", "index -2")
    raises(function() m:add_item{ label = "x", index = "first" } end, "index", "a string as index")
    raises(function() m:add_item{ label = "x", index = 1.5 } end, "index", "a fraction as index")
    eq(m.n_items, 0, "a call that raised added nothing")
  end)

  case("add_item", function()
    local calls = 0
    local item = m:add_item{ label = "One", on_activate = function() calls = calls + 1 end }

    eq(m.n_items, 1, "n_items")
    truthy(tostring(item):find("shell.MenuActionItem", 1, true), "tostring")
    eq(item.kind, "item", "kind")
    eq(item.label, "One", "label")
    eq(item.visible, true, "visible")
    eq(item.sensitive, true, "sensitive")
    eq(item.ornament, "hidden", "default ornament")
    eq(item.index, 1, "index")
    eq(item.icon_name, nil, "icon_name of a plain item")
    eq(item.icon_file, nil, "icon_file of a plain item")
    eq(item.alive, true, "alive")

    item.label = "Uno"
    eq(item.label, "Uno", "label after the assignment")
    item.sensitive = false
    eq(item.sensitive, false, "sensitive = false")
    item.sensitive = true
    item.visible = false
    eq(item.visible, false, "visible = false")
    item.visible = true
    for _, o in ipairs{ "hidden", "none", "check", "dot", "no-dot" } do
      item.ornament = o
      eq(item.ornament, o, "ornament")
    end
    raises(function() item.ornament = "star" end, "ornament", "an unknown ornament")
    raises(function() item.label = 5 end, "label", "a number as label")
    raises(function() item.sensitive = "no" end, "sensitive", "a string as sensitive")
    raises(function() item.kind = "x" end, "kind", "kind is read-only")
    raises(function() item.index = 3 end, "index", "index is read-only")
    raises(function() item.icon_name = "alarm-symbolic" end, "icon_name", "icon_name on a plain item")
    raises(function() item.icon_file = "icon.svg" end, "icon_file", "icon_file on a plain item")

    -- rule 7: an assignment is not an activation
    item.label = "Again"
    item.sensitive = false
    item.sensitive = true
    eq(calls, 0, "on_activate is not called by an assignment")

    local conn = item:on_activate(function() calls = calls + 1 end)
    eq(conn:is_connected(), true, "on_activate returns a connection")
    conn:disconnect()
    eq(conn:is_connected(), false, "disconnect")
    raises(function() item:on_activate() end, nil, "on_activate without a function")
    raises(function() item:on_toggled(function() end) end, nil, "on_toggled on a plain item")

    -- construction with every field
    local full = m:add_item{ label = "Full", sensitive = false, visible = false, ornament = "check" }
    eq(full.sensitive, false, "constructed insensitive")
    eq(full.visible, false, "constructed hidden")
    eq(full.ornament, "check", "constructed with an ornament")

    -- destroy: idempotent, the handle dies, the slot is freed
    eq(m.n_items, 2, "two items")
    full:destroy()
    eq(full.alive, false, "alive after destroy")
    full:destroy()
    eq(m.n_items, 1, "n_items after destroy")
    raises(function() return full.label end, "no longer valid", "a dead item")
    raises(function() full.label = "x" end, "no longer valid", "assigning a dead item")
    eq(full.alive, false, "alive never raises")
    item:destroy()
    eq(m.n_items, 0, "n_items")
  end)

  -- -------------------------------------------------------------------
  -- add_image_item
  -- -------------------------------------------------------------------
  case("add_image_item", function()
    raises(function() m:add_image_item{ label = "x" } end, "icon", "no icon")
    raises(function() m:add_image_item{ icon_name = "alarm-symbolic" } end, "label", "no label")
    raises(function() m:add_image_item{ label = "x", icon_name = "alarm-symbolic", icon_file = "icon.svg" } end,
           "mutually exclusive", "both icons")
    raises(function() m:add_image_item{ label = "x", icon_file = "/etc/hosts.svg" } end, "icon_file", "an absolute path")
    raises(function() m:add_image_item{ label = "x", icon_file = "../icon.svg" } end, "icon_file", "..")
    raises(function() m:add_image_item{ label = "x", icon_file = "missing.svg" } end, "icon_file", "a missing file")
    raises(function() m:add_image_item{ label = "x", icon_file = "metadata.json" } end, "icon_file", "a wrong suffix")
    eq(m.n_items, 0, "nothing was added by the calls that raised")

    local named = m:add_image_item{ label = "Named", icon_name = "alarm-symbolic" }
    eq(named.kind, "image", "kind")
    truthy(tostring(named):find("shell.MenuActionItem", 1, true), "an image item is an action item")
    eq(named.icon_name, "alarm-symbolic", "icon_name")
    eq(named.icon_file, nil, "icon_file")

    local filed = m:add_image_item{ label = "Filed", icon_file = "icon.svg" }
    eq(filed.icon_file, "icon.svg", "icon_file")
    eq(filed.icon_name, nil, "icon_name")

    named.icon_file = "icon.svg"
    eq(named.icon_file, "icon.svg", "icon_file after the assignment")
    eq(named.icon_name, nil, "setting icon_file clears icon_name")
    named.icon_name = "alarm-symbolic"
    eq(named.icon_file, nil, "setting icon_name clears icon_file")
    raises(function() named.icon_file = "missing.svg" end, "icon_file", "a missing file")
    raises(function() named.icon_name = 5 end, "icon_name", "a number as icon_name")

    m:clear()
    eq(m.n_items, 0, "clear")
  end)

  -- -------------------------------------------------------------------
  -- add_switch
  -- -------------------------------------------------------------------
  case("add_switch", function()
    raises(function() m:add_switch{} end, "label", "no label")
    raises(function() m:add_switch{ label = "x", state = 1 } end, "state", "a number as state")
    raises(function() m:add_switch{ label = "x", status = 5 } end, "status", "a number as status")
    raises(function() m:add_switch{ label = "x", ornament = "check" } end, "ornament", "ornament is not a switch field")

    local toggles, activations = {}, 0
    local sw = m:add_switch{
      label = "Wi-Fi",
      on_toggled = function(self, state) toggles[#toggles + 1] = tostring(state) end,
      on_activate = function() activations = activations + 1 end,
    }
    truthy(tostring(sw):find("shell.MenuSwitchItem", 1, true), "tostring")
    eq(sw.kind, "switch", "kind")
    eq(sw.label, "Wi-Fi", "label")
    eq(sw.state, false, "default state")
    eq(sw.status, nil, "default status")
    eq(sw.sensitive, true, "sensitive")

    -- rule 7: each change of state calls on_toggled; it is not an activation
    sw.state = true
    eq(sw.state, true, "state after the assignment")
    sw.state = true
    sw.state = false
    eq(table.concat(toggles, ","), "true,false", "on_toggled for each change")
    eq(activations, 0, "on_activate is not called by an assignment")
    raises(function() sw.state = "on" end, "state", "a string as state")

    sw.status = "Not available"
    eq(sw.status, "Not available", "status")
    sw.status = nil
    eq(sw.status, nil, "status = nil restores the switch")
    raises(function() sw.status = 5 end, "status", "a number as status")

    local on = m:add_switch{ label = "On", state = true, status = "Busy" }
    eq(on.state, true, "constructed with a state")
    eq(on.status, "Busy", "constructed with a status")
    sw:on_toggled(function() end):disconnect()
    sw:on_activate(function() end):disconnect()
    m:clear()
  end)

  -- -------------------------------------------------------------------
  -- add_submenu
  -- -------------------------------------------------------------------
  case("add_submenu", function()
    raises(function() m:add_submenu{} end, "label", "no label")
    raises(function() m:add_submenu{ label = "x", on_activate = function() end } end, "on_activate",
           "a submenu item never activates")
    raises(function() m:add_submenu{ label = "x", icon_name = "a", icon_file = "icon.svg" } end,
           "mutually exclusive", "both icons")

    local sub = m:add_submenu{ label = "Profiles", icon_name = "preferences-system-symbolic" }
    truthy(tostring(sub):find("shell.MenuSubmenuItem", 1, true), "tostring")
    eq(sub.kind, "submenu", "kind")
    eq(sub.label, "Profiles", "label")
    eq(sub.icon_name, "preferences-system-symbolic", "icon_name")
    truthy(tostring(sub.menu):find("shell.Menu", 1, true), "item.menu is a menu")
    eq(sub.menu, sub.menu, "the same handle every time")
    eq(sub.menu.is_open, false, "the submenu is closed")
    eq(sub.menu.n_items, 0, "the submenu is empty")

    local a = sub.menu:add_item{ label = "home", ornament = "dot" }
    local b = sub.menu:add_item{ label = "work", ornament = "no-dot" }
    eq(sub.menu.n_items, 2, "n_items of the submenu")
    eq(a.index, 1, "index in the submenu")
    eq(b.index, 2, "index in the submenu")
    eq(labels(sub.menu), "home,work", "items of the submenu")
    eq(m.n_items, 1, "the items of a submenu are not items of the menu")
    raises(function() sub.menu:add_submenu{ label = "deeper" } end, "submenu", "a submenu in a submenu")

    local plain = m:add_submenu{ label = "No icon" }
    eq(plain.icon_name, nil, "icon_name of a submenu item without icon")
    raises(function() plain.icon_name = "alarm-symbolic" end, "icon", "an icon where there is none")
    sub.icon_name = "alarm-symbolic"
    eq(sub.icon_name, "alarm-symbolic", "icon_name after the assignment")
    sub.icon_file = "icon.svg"
    eq(sub.icon_file, "icon.svg", "icon_file after the assignment")
    raises(function() sub.menu.alive = false end, "alive", "alive is read-only")

    local seen = 0
    local conn = sub.menu:on_open_state_changed(function() seen = seen + 1 end)
    conn:disconnect()

    -- destroying the item destroys its menu and the items of it
    sub:destroy()
    eq(sub.alive, false, "the submenu item is dead")
    eq(a.alive, false, "the items of its menu are dead")
    m:clear()
  end)

  -- -------------------------------------------------------------------
  -- add_separator
  -- -------------------------------------------------------------------
  case("add_separator", function()
    raises(function() m:add_separator{ label = 5 } end, "label", "a number as label")
    raises(function() m:add_separator{ bogus = 1 } end, "bogus", "an unknown field")
    raises(function() m:add_separator{ on_activate = function() end } end, "on_activate", "no events")

    m:add_item{ label = "above" }
    local plain = m:add_separator{}
    local headed = m:add_separator{ label = "Section" }
    m:add_item{ label = "below" }
    truthy(tostring(plain):find("shell.MenuSeparator", 1, true), "tostring")
    eq(plain.kind, "separator", "kind")
    eq(plain.label, "", "a plain separator has an empty label")
    eq(headed.label, "Section", "label")
    headed.label = "Other"
    eq(headed.label, "Other", "label after the assignment")
    headed.label = ""
    eq(headed.label, "", "label cleared")
    eq(plain.sensitive, nil, "a separator has no sensitive")
    eq(plain.ornament, nil, "a separator has no ornament")
    eq(m.n_items, 4, "n_items counts separators")
    m:clear()
  end)

  -- -------------------------------------------------------------------
  -- add_widget
  -- -------------------------------------------------------------------
  case("add_widget", function()
    raises(function() m:add_widget{} end, "child", "no child")
    raises(function() m:add_widget{ child = "x" } end, nil, "a string as child")
    raises(function() m:add_widget{ child = ui.label{}, label = "x" } end, "label", "label is not a widget field")

    local slider = ui.slider{ value = 0.5, accessible_name = "Level" }
    local item = m:add_widget{ child = slider }
    truthy(tostring(item):find("shell.MenuWidgetItem", 1, true), "tostring")
    eq(item.kind, "widget", "kind")
    eq(item.child, slider, "child")
    eq(item.label, nil, "a widget item has no label")
    eq(item.sensitive, nil, "a widget item has no sensitive")
    eq(slider.alive, true, "the child is alive")

    -- one parent only
    raises(function() m:add_widget{ child = slider } end, "already has a parent", "a child that is in use")
    raises(function() ui.box{ children = { slider } } end, "already has a parent", "a child that is in a menu")
    slider:remove()
    m:add_widget{ child = slider }   -- a removed widget can be placed again

    -- destroying the item destroys the child
    local label = ui.label{ text = "in a menu" }
    local holder = m:add_widget{ child = label }
    holder:destroy()
    eq(label.alive, false, "destroying the item destroys the child")
    m:clear()
    eq(slider.alive, false, "clear () destroys the child of a widget item")
  end)

  -- -------------------------------------------------------------------
  -- Order, index, items (), clear ()
  -- -------------------------------------------------------------------
  case("index and items ()", function()
    local a = m:add_item{ label = "a" }
    local b = m:add_item{ label = "b" }
    local c = m:add_item{ label = "c", index = 1 }
    local d = m:add_item{ label = "d", index = 2 }
    local e = m:add_item{ label = "e", index = 100 }

    eq(labels(m), "c,d,a,b,e", "inserted before the item at that position, past the end appends")
    eq(a.index, 3, "index of a")
    eq(c.index, 1, "index of c")
    eq(e.index, 5, "index of e")
    local items = m:items()
    eq(#items, 5, "items ()")
    eq(items[1], c, "the handles are the ones that were returned")
    eq(items[5], e, "the last one")
    b:destroy()
    eq(labels(m), "c,d,a,e", "destroy removes the item")
    eq(e.index, 4, "index follows")

    local s = m:add_switch{ label = "s", index = 1 }
    local sep = m:add_separator{ index = 2 }
    local img = m:add_image_item{ label = "i", icon_name = "alarm-symbolic", index = 3 }
    eq(s.index, 1, "switch index")
    eq(sep.index, 2, "separator index")
    eq(img.index, 3, "image item index")
    eq(m.n_items, 7, "n_items")

    m:clear()
    eq(m.n_items, 0, "clear")
    eq(#m:items(), 0, "items () after clear")
    for _, item in ipairs{ a, c, d, e, s, sep, img } do
      eq(item.alive, false, "an item cleared")
    end
    m:clear()   -- harmless on an empty menu
  end)

  case("sensitive menu", function()
    local item = m:add_item{ label = "x" }
    local sw = m:add_switch{ label = "y" }

    m.sensitive = false
    eq(m.sensitive, false, "menu sensitive = false")
    eq(item.sensitive, false, "items are insensitive while the menu is")
    eq(sw.sensitive, false, "so are switches")
    m.sensitive = true
    eq(item.sensitive, true, "sensitive again")
    item.sensitive = false
    m.sensitive = false
    m.sensitive = true
    eq(item.sensitive, false, "an item that was insensitive stays so")
    m:clear()
  end)

  case("assignments that neither open nor close the menu", function()
    local opened = {}
    local conn = m:on_open_state_changed(function(menu, open)
      opened[#opened + 1] = open
    end)

    m:add_item{ label = "x" }
    m.sensitive = false
    m.sensitive = true
    eq(#opened, 0, "no open state changed")
    eq(conn:is_connected(), true, "the connection")
    conn:disconnect()
    raises(function() m:on_open_state_changed() end, nil, "no function")
    raises(function() m:on_open_state_changed(5) end, nil, "a number as function")
    m:clear()
  end)

  case("open and close: options", function()
    m:add_item{ label = "x" }
    raises(function() m:open(5) end, nil, "a number as options")
    raises(function() m:open({ animate = "no" }) end, "animate", "a string as animate")
    raises(function() m:open({ bogus = true }) end, "bogus", "an unknown option")
    raises(function() m:close({ bogus = true }) end, "bogus", "an unknown option")
    m:clear()
  end)

  -- -------------------------------------------------------------------
  -- Steps: the menu opened as the user would
  -- -------------------------------------------------------------------
  kept.opened = {}
  steps {
    ["menu-items"] = function()
      m:add_item{ label = "One" }
      m:add_separator{}
      kept.sw = m:add_switch{ label = "Two" }
      kept.sub = m:add_submenu{ label = "Three" }
      kept.sub.menu:add_item{ label = "Inner" }
      kept.conn = m:on_open_state_changed(function(menu, open)
        kept.opened[#kept.opened + 1] = open
        eq(menu, m, "the first argument is the menu")
      end)
      kept.sub_opened = {}
      kept.sub_conn = kept.sub.menu:on_open_state_changed(function(menu, open)
        kept.sub_opened[#kept.sub_opened + 1] = open
      end)
    end,

    ["menu-open"] = function()
      eq(m:open({ animate = false }), true, "open () changes the state")
      eq(m.is_open, true, "is_open")
      eq(kept.opened[#kept.opened], true, "on_open_state_changed (true)")
      eq(m:open({ animate = false }), false, "open () of an open menu")
      kept.sub.menu:open({ animate = false })
      eq(kept.sub.menu.is_open, true, "the submenu opens")
      eq(kept.sub_opened[#kept.sub_opened], true, "on_open_state_changed of the submenu (true)")
      kept.sub.menu:close({ animate = false })
      eq(kept.sub_opened[#kept.sub_opened], false, "on_open_state_changed of the submenu (false)")
    end,

    ["menu-close"] = function()
      eq(m:close({ animate = false }), true, "close () changes the state")
      eq(m.is_open, false, "is_open after close")
      eq(kept.opened[#kept.opened], false, "on_open_state_changed (false)")
      m:toggle()
      eq(m.is_open, true, "toggle () opens")
      m:toggle()
      eq(m.is_open, false, "toggle () closes")
    end,

    ["menu-destroy-open"] = function()
      -- destroying the items of an open menu is fine
      m:open({ animate = false })
      kept.sw:destroy()
      eq(kept.sw.alive, false, "an item destroyed while the menu is open")
      m:clear()
      eq(m.n_items, 0, "clear () while the menu is open")
      m:close({ animate = false })
      eq(m.is_open, false, "closed again")
    end,
  }

  T.done()
end

-- Defining disable () is optional (README): this one only checks what it can
function ext.disable()
  case("disable () runs before the objects are destroyed", function()
    eq(kept.menu.alive, true, "the menu is alive in disable ()")
    eq(kept.ind.alive, true, "the indicator is alive in disable ()")
  end)
end

return ext
