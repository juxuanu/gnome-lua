-- Fixture of tests/api/quick-settings.lua: every function, field, property
-- and error kind of docs/api/quick-settings.md that an extension can check
-- by itself. enable () runs the synchronous checks; the script of the test
-- asks for the others with steps (tests/api/README.md): the ones that need
-- the system menu to be open.
local shell = require("shell")
local T = shell.test
local qs = shell.quick_settings

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

local function near(got, want, what)
  if type(got) ~= "number" or math.abs(got - want) > 1e-6 then
    error(string.format("%s: expected %s, got %s", what or "value", tostring(want), tostring(got)), 2)
  end
end

local function is_type(h, name)
  return tostring(h):find("shell." .. name, 1, true) ~= nil
end

local ext = {}

-- The handles that live from enable () to the steps and to disable ()
local keep = {}
local opened = { n = 0 }

function ext.enable()
  -- -------------------------------------------------------------------
  -- The module (README: absent functions are nil, never stubs)
  -- -------------------------------------------------------------------
  case("shell.quick_settings has the constructors of the document", function()
    eq(type(qs), "table", "type")
    for _, name in ipairs { "add_toggle", "add_menu_toggle", "add_slider", "add_indicator" } do
      eq(type(qs[name]), "function", name)
    end
    -- "There is no separate remove": items are destroyed
    eq(qs.remove, nil, "remove")
    eq(qs.remove_toggle, nil, "remove_toggle")
  end)

  -- -------------------------------------------------------------------
  -- Placement: the index of an item among the items of the extension (the
  -- slot rules of README.md). This runs first: nothing else exists yet
  -- -------------------------------------------------------------------
  case("index orders the items of the extension, in default and explicit positions", function()
    local a = qs.add_toggle { title = "a" }
    local b = qs.add_toggle { title = "b" }
    local c = qs.add_slider { accessible_name = "c" }
    eq(a.index, 1, "first item")
    eq(b.index, 2, "second item")
    eq(c.index, 3, "a slider is in the same slot as the toggles")

    local d = qs.add_toggle { title = "d", index = 1 }
    eq(d.index, 1, "index = 1 goes first")
    eq(a.index, 2, "a moved")
    eq(b.index, 3, "b moved")
    eq(c.index, 4, "c moved")

    local e = qs.add_menu_toggle { title = "e", index = 3 }
    eq(e.index, 3, "index = 3 goes third")
    eq(b.index, 4, "b moved again")

    local f = qs.add_toggle { title = "f", index = 99 }
    eq(f.index, 6, "values past the end clamp")

    local g = qs.add_toggle { title = "g" }
    eq(g.index, 7, "the default is after the existing ones")

    d:destroy()
    eq(a.index, 1, "an item takes the freed slot immediately")
    eq(f.index, 5, "the items after the destroyed one move up")
    eq(g.index, 6, "the last one too")

    -- an item hidden is still in its slot
    e.visible = false
    eq(e.index, 2, "index of a hidden item")
    e.visible = true

    for _, item in ipairs { a, b, c, e, f, g } do
      item:destroy()
    end
  end)

  case("index orders the status icons of the extension", function()
    local a = qs.add_indicator { icon_name = "alarm-symbolic" }
    local b = qs.add_indicator { icon_name = "alarm-symbolic" }
    eq(a.index, 1, "first icon")
    eq(b.index, 2, "second icon")

    local c = qs.add_indicator { icon_name = "alarm-symbolic", index = 1 }
    eq(c.index, 1, "index = 1 goes first")
    eq(a.index, 2, "a moved")
    eq(b.index, 3, "b moved")

    local d = qs.add_indicator { icon_name = "alarm-symbolic", index = 50 }
    eq(d.index, 4, "values past the end clamp")

    -- icons and items are independent: they do not share indices
    local t = qs.add_toggle { title = "t" }
    eq(t.index, 1, "the first item, with icons around")
    t:destroy()

    c:destroy()
    eq(a.index, 1, "a moved up")
    eq(d.index, 3, "d moved up")
    for _, icon in ipairs { a, b, d } do
      icon:destroy()
    end
  end)

  -- -------------------------------------------------------------------
  -- add_toggle
  -- -------------------------------------------------------------------
  case("add_toggle with a title only", function()
    local t = qs.add_toggle { title = "Plain" }

    truthy(is_type(t, "QuickToggle"), "tostring: " .. tostring(t))
    eq(t.alive, true, "alive")
    eq(t.title, "Plain", "title")
    eq(t.subtitle, nil, "subtitle")
    eq(t.checked, false, "checked")
    eq(t.visible, true, "visible")
    eq(t.sensitive, true, "sensitive")
    eq(t.colspan, 1, "colspan of a toggle")
    eq(math.type(t.index), "integer", "index")
    eq(t.menu, nil, "a toggle without menu has no menu")
    eq(t.nonsense, nil, "an unknown property reads as nil")
    truthy(t == t, "equality")
    truthy(rawequal(t, t), "identity")
    keep.plain = t
  end)

  case("add_toggle with every field", function()
    local t = qs.add_toggle {
      title = "Full", subtitle = "Sub", icon_name = "alarm-symbolic", checked = true,
      visible = false, sensitive = false, colspan = 2, index = 1,
      on_toggle = function() end,
    }

    eq(t.title, "Full", "title")
    eq(t.subtitle, "Sub", "subtitle")
    eq(t.icon_name, "alarm-symbolic", "icon_name")
    eq(t.checked, true, "checked")
    eq(t.visible, false, "visible")
    eq(t.sensitive, false, "sensitive")
    eq(t.colspan, 2, "colspan")
    eq(t.index, 1, "index")
    keep.full = t
  end)

  case("a toggle can have an empty subtitle: it reads as nil", function()
    local t = qs.add_toggle { title = "E", subtitle = "" }
    eq(t.subtitle, nil, "subtitle")
    t.subtitle = "now"
    eq(t.subtitle, "now", "subtitle set")
    t.subtitle = ""
    eq(t.subtitle, nil, "an empty subtitle hides it")
    t.subtitle = "again"
    t.subtitle = nil
    eq(t.subtitle, nil, "nil clears it")
    t:destroy()
  end)

  case("a toggle with an icon file", function()
    -- the files of the extension only (docs/api/ui.md): none is shipped here
    raises(function() qs.add_toggle { title = "F", icon_file = "missing.svg" } end, "icon_file", "a missing icon file")
    raises(function() qs.add_toggle { title = "F", icon_file = "/usr/share/pixmaps/x.svg" } end, "icon_file",
      "an absolute icon file")
    raises(function() qs.add_toggle { title = "F", icon_file = "../x.svg" } end, "icon_file", "an icon file with ..")
    local t = qs.add_toggle { title = "F" }
    raises(function() t.icon_file = "/etc/passwd" end, "icon_file", "assigning an absolute icon file")
    eq(t.icon_file, nil, "no icon file")
    t:destroy()
  end)

  case("writing the properties of a toggle", function()
    local t = qs.add_toggle { title = "W" }

    t.title = "W2"
    eq(t.title, "W2", "title")
    t.icon_name = "alarm-symbolic"
    eq(t.icon_name, "alarm-symbolic", "icon_name")
    t.checked = true
    eq(t.checked, true, "checked")
    t.checked = false
    eq(t.checked, false, "checked back")
    t.visible = false
    eq(t.visible, false, "visible")
    t.visible = true
    t.sensitive = false
    eq(t.sensitive, false, "sensitive")
    t.sensitive = true
    t.colspan = 2
    eq(t.colspan, 2, "colspan")
    t.colspan = 1
    eq(t.colspan, 1, "colspan back")

    raises(function() t.colspan = 3 end, "colspan", "colspan = 3")
    raises(function() t.colspan = 0 end, "colspan", "colspan = 0")
    raises(function() t.colspan = 1.5 end, "colspan", "colspan = 1.5")
    raises(function() t.colspan = "2" end, "colspan", "colspan = '2'")
    raises(function() t.index = 1 end, "index", "assigning index")
    raises(function() t.alive = false end, "alive", "assigning alive")
    raises(function() t.nonsense = 1 end, "nonsense", "assigning an unknown property")
    raises(function() t.title = 5 end, "title", "a number as title")
    raises(function() t.checked = "yes" end, "checked", "a string as checked")
    eq(t.colspan, 1, "colspan unchanged by the failed writes")
    t:destroy()
  end)

  case("add_toggle raises on a misuse, naming the field", function()
    raises(function() qs.add_toggle { title = "x", bogus = 1 } end, "bogus", "an unknown field")
    raises(function() qs.add_toggle {} end, "title", "no title")
    raises(function() qs.add_toggle { title = 5 } end, "title", "a number as title")
    raises(function() qs.add_toggle { title = "x", subtitle = 5 } end, "subtitle", "a number as subtitle")
    raises(function() qs.add_toggle { title = "x", checked = "yes" } end, "checked", "a string as checked")
    raises(function() qs.add_toggle { title = "x", visible = 1 } end, "visible", "a number as visible")
    raises(function() qs.add_toggle { title = "x", sensitive = "no" } end, "sensitive", "a string as sensitive")
    raises(function() qs.add_toggle { title = "x", colspan = 3 } end, "colspan", "colspan = 3")
    raises(function() qs.add_toggle { title = "x", colspan = 0 } end, "colspan", "colspan = 0")
    raises(function() qs.add_toggle { title = "x", index = 0 } end, "index", "index = 0")
    raises(function() qs.add_toggle { title = "x", index = -2 } end, "index", "a negative index")
    raises(function() qs.add_toggle { title = "x", index = "1" } end, "index", "a string as index")
    raises(function() qs.add_toggle { title = "x", icon_name = "a", icon_file = "b.svg" } end,
      { "icon_name", "icon_file" }, "two icons")
    raises(function() qs.add_toggle { title = "x", on_toggle = function() end, on_click = function() end } end,
      "on_toggle", "on_toggle and on_click")
    raises(function() qs.add_toggle { title = "x", on_toggle = "f" } end, "on_toggle", "a string as on_toggle")
    raises(function() qs.add_toggle { title = "x", on_click = 5 } end, "on_click", "a number as on_click")
    raises(function() qs.add_toggle("x") end, nil, "a string instead of the table")
    raises(function() qs.add_toggle() end, nil, "no argument")
    -- a toggle takes no menu field: they are the fields of add_menu_toggle
    raises(function() qs.add_toggle { title = "x", menu_enabled = true } end, "menu_enabled", "menu_enabled on a toggle")
    raises(function() qs.add_toggle { title = "x", value = 0.5 } end, "value", "value on a toggle")
  end)

  case("a failed constructor leaves nothing behind", function()
    local before = qs.add_toggle { title = "before" }
    pcall(qs.add_toggle, { title = "bad", colspan = 7 })
    pcall(qs.add_toggle, { title = "bad", on_toggle = "f" })
    pcall(qs.add_toggle, { title = "bad", bogus = true })
    local after = qs.add_toggle { title = "after" }
    eq(after.index, before.index + 1, "no slot was taken by the failed calls")
    before:destroy()
    after:destroy()
  end)

  -- -------------------------------------------------------------------
  -- Events: on_toggle, on_click and README rule 7
  -- -------------------------------------------------------------------
  -- An assignment is not a click: on_toggle, on_click and on_icon_click stay
  -- quiet. It changes the value of a slider: on_changed reports it.
  case("what assignments call back (README rule 7)", function()
    local calls = 0
    local function count()
      calls = calls + 1
    end
    local values = {}

    local t = qs.add_toggle { title = "R7", on_toggle = count }
    t.checked = true
    t.checked = false
    t.title = "R7b"
    t.subtitle = "x"
    t.visible = false
    t.visible = true
    t.sensitive = false
    t.sensitive = true

    local c = qs.add_toggle { title = "R7c", on_click = count }
    c.checked = true
    c.checked = false
    c.title = "R7d"

    local m = qs.add_menu_toggle { title = "R7m", on_toggle = count }
    m.checked = true
    m.checked = false
    m.menu_enabled = false
    m.menu_enabled = true

    local s = qs.add_slider {
      accessible_name = "R7s", icon_reactive = true, on_icon_click = count,
      on_changed = function(_, value) values[#values + 1] = string.format("%g", value) end,
    }
    s.value = 0.5
    s.value = 0
    s.value = 1
    s.value = 1
    s.icon_name = "alarm-symbolic"
    s.sensitive = false
    s.sensitive = true

    eq(calls, 0, "click callbacks called by assignments")
    eq(table.concat(values, ","), "0.5,0,1", "on_changed for each change of value")
    -- ... and nothing more later, when the shell has processed what the
    -- assignments caused: the step "assignments-later" of the script looks
    -- again
    keep.r7 = {
      calls = function() return calls end,
      values = function() return #values end,
      items = { t, c, m, s },
    }
  end)

  case("on_toggle and on_click as methods return connections", function()
    local t = qs.add_toggle { title = "M" }
    local conn = t:on_toggle(function() end)

    eq(type(conn), "userdata", "a connection is a handle")
    eq(type(conn.disconnect), "function", "Connection:disconnect")
    raises(function() t:on_click(function() end) end, "on_click", "on_click on an item with on_toggle")
    raises(function() t:on_toggle(5) end, nil, "a number as callback")
    conn:disconnect()

    local d = qs.add_toggle { title = "D" }
    local conn2 = d:on_click(function() end)
    eq(type(conn2.disconnect), "function", "Connection:disconnect")
    raises(function() d:on_toggle(function() end) end, "on_toggle", "on_toggle on an item with on_click")
    conn2:disconnect()

    -- a display-only item takes either, once
    local e = qs.add_toggle { title = "E" }
    e:on_click(function() end)
    t:destroy()
    d:destroy()
    e:destroy()
  end)

  -- -------------------------------------------------------------------
  -- add_menu_toggle and shell.QuickMenu
  -- -------------------------------------------------------------------
  case("add_menu_toggle", function()
    local m = qs.add_menu_toggle { title = "Menu", subtitle = "S", icon_name = "alarm-symbolic" }

    truthy(is_type(m, "QuickMenuToggle"), "tostring: " .. tostring(m))
    eq(m.title, "Menu", "title")
    eq(m.subtitle, "S", "subtitle")
    eq(m.checked, false, "checked")
    eq(m.colspan, 1, "colspan")
    eq(m.menu_enabled, true, "menu_enabled default")
    eq(type(m.menu_button_accessible_name), "string", "the default name of the menu button")
    truthy(#m.menu_button_accessible_name > 0, "the default name of the menu button is not empty")

    m.menu_enabled = false
    eq(m.menu_enabled, false, "menu_enabled")
    m.menu_button_accessible_name = "More"
    eq(m.menu_button_accessible_name, "More", "menu_button_accessible_name")
    raises(function() m.menu = m.menu end, "menu", "assigning menu")
    raises(function() m.menu_enabled = "yes" end, "menu_enabled", "a string as menu_enabled")

    local n = qs.add_menu_toggle { title = "N", menu_enabled = false, menu_button_accessible_name = "Open N" }
    eq(n.menu_enabled, false, "menu_enabled given")
    eq(n.menu_button_accessible_name, "Open N", "menu_button_accessible_name given")

    -- the properties of the toggle are inherited
    m.checked = true
    eq(m.checked, true, "checked")
    m.title = "Menu 2"
    eq(m.title, "Menu 2", "title written")
    eq(type(m.on_toggle), "function", "on_toggle is inherited")
    eq(type(m.on_click), "function", "on_click is inherited")
    n:destroy()
    keep.menu_toggle = m
  end)

  case("the menu of a menu toggle", function()
    local m = qs.add_menu_toggle { title = "MM" }
    local menu = m.menu

    eq(menu.alive, true, "alive")
    truthy(is_type(menu, "QuickMenu"), "tostring: " .. tostring(menu))
    truthy(rawequal(menu, m.menu), "the same object gives the same handle")
    eq(menu.is_open, false, "is_open")
    eq(type(menu.set_header), "function", "set_header")
    eq(type(menu.add_item), "function", "a QuickMenu is a Menu: add_item")

    menu:set_header { icon_name = "alarm-symbolic", title = "Header", subtitle = "Sub" }
    menu:set_header { title = "Only a title" }
    raises(function() menu:set_header { icon_name = "alarm-symbolic" } end, "title", "set_header without title")
    raises(function() menu:set_header { title = "t", bogus = 1 } end, "bogus", "set_header with an unknown field")
    raises(function() menu:set_header { title = "t", icon_name = "a", icon_file = "b.svg" } end,
      { "icon_name", "icon_file" }, "set_header with two icons")
    raises(function() menu:set_header("x") end, nil, "set_header with a string")

    -- the menu takes items like any other (menu.md)
    local item = menu:add_item { label = "An item" }
    eq(item.label, "An item", "label of an item of the menu")
    eq(menu.n_items, 1, "n_items")
    keep.menu = menu
    keep.menu_owner = m
  end)

  case("a plain toggle has no menu, a slider always has one", function()
    local t = qs.add_toggle { title = "NoMenu" }
    eq(t.menu, nil, "toggle.menu")
    t:destroy()

    local s = qs.add_slider { accessible_name = "WithMenu" }
    truthy(is_type(s.menu, "QuickMenu"), "slider.menu: " .. tostring(s.menu))
    eq(s.menu_enabled, false, "the arrow of a slider is hidden by default")
    s.menu:set_header { title = "Slider menu" }
    s:destroy()
  end)

  case("add_menu_toggle raises on a misuse", function()
    raises(function() qs.add_menu_toggle { title = "x", bogus = 1 } end, "bogus", "an unknown field")
    raises(function() qs.add_menu_toggle { subtitle = "x" } end, "title", "no title")
    raises(function() qs.add_menu_toggle { title = "x", menu_enabled = 1 } end, "menu_enabled", "a number")
    raises(function() qs.add_menu_toggle { title = "x", menu_button_accessible_name = 1 } end,
      "menu_button_accessible_name", "a number as the name")
    raises(function() qs.add_menu_toggle { title = "x", on_toggle = function() end, on_click = function() end } end,
      "on_toggle", "on_toggle and on_click")
    raises(function() qs.add_menu_toggle { title = "x", accessible_name = "a" } end, "accessible_name",
      "a field of the slider")
  end)

  -- -------------------------------------------------------------------
  -- add_slider
  -- -------------------------------------------------------------------
  case("add_slider defaults", function()
    local s = qs.add_slider { accessible_name = "Volume" }

    truthy(is_type(s, "QuickSlider"), "tostring: " .. tostring(s))
    eq(s.accessible_name, "Volume", "accessible_name")
    near(s.value, 0, "value")
    eq(s.colspan, 2, "colspan of a slider")
    eq(s.icon_reactive, false, "icon_reactive")
    eq(s.menu_enabled, false, "menu_enabled")
    eq(s.visible, true, "visible")
    eq(s.sensitive, true, "sensitive")
    eq(s.title, nil, "a slider has no title")
    eq(s.checked, nil, "a slider is not a toggle")
    eq(type(s.on_changed), "function", "on_changed")
    eq(type(s.on_icon_click), "function", "on_icon_click")
    eq(s.on_toggle, nil, "no on_toggle on a slider")
    keep.slider = s
  end)

  case("add_slider with every field", function()
    local s = qs.add_slider {
      accessible_name = "Brightness", icon_name = "alarm-symbolic", value = 0.25,
      on_changed = function() end, icon_reactive = true, on_icon_click = function() end,
      icon_accessible_name = "Mute", menu_enabled = true, menu_button_accessible_name = "Options",
      visible = false, colspan = 1, sensitive = false,
    }

    near(s.value, 0.25, "value")
    eq(s.icon_name, "alarm-symbolic", "icon_name")
    eq(s.icon_reactive, true, "icon_reactive")
    eq(s.icon_accessible_name, "Mute", "icon_accessible_name")
    eq(s.menu_enabled, true, "menu_enabled")
    eq(s.menu_button_accessible_name, "Options", "menu_button_accessible_name")
    eq(s.visible, false, "visible")
    eq(s.colspan, 1, "colspan")
    eq(s.sensitive, false, "sensitive")
    s:destroy()
  end)

  case("writing the properties of a slider", function()
    local s = qs.add_slider { accessible_name = "W" }

    s.value = 0.75
    near(s.value, 0.75, "value")
    s.value = 1
    near(s.value, 1, "value at the top")
    s.value = 0
    near(s.value, 0, "value at the bottom")
    raises(function() s.value = 1.5 end, "value", "value above 1")
    raises(function() s.value = -0.1 end, "value", "a negative value")
    raises(function() s.value = "half" end, "value", "a string as value")
    near(s.value, 0, "value unchanged by the failed writes")

    s.icon_name = "alarm-symbolic"
    eq(s.icon_name, "alarm-symbolic", "icon_name")
    s.icon_reactive = true
    eq(s.icon_reactive, true, "icon_reactive")
    s.icon_accessible_name = "Icon"
    eq(s.icon_accessible_name, "Icon", "icon_accessible_name")
    s.accessible_name = "Renamed"
    eq(s.accessible_name, "Renamed", "accessible_name")
    s.menu_enabled = true
    eq(s.menu_enabled, true, "menu_enabled")
    s.menu_button_accessible_name = "Arrow"
    eq(s.menu_button_accessible_name, "Arrow", "menu_button_accessible_name")
    s.sensitive = false
    eq(s.sensitive, false, "sensitive")
    s.sensitive = true
    s.colspan = 1
    eq(s.colspan, 1, "colspan")
    raises(function() s.menu = nil end, "menu", "assigning menu")
    raises(function() s.title = "x" end, "title", "a slider has no title")
    raises(function() s.checked = true end, "checked", "a slider has no checked")
    s:destroy()
  end)

  case("add_slider raises on a misuse, naming the field", function()
    raises(function() qs.add_slider {} end, "accessible_name", "no accessible_name")
    raises(function() qs.add_slider { accessible_name = 1 } end, "accessible_name", "a number as accessible_name")
    raises(function() qs.add_slider { accessible_name = "x", bogus = 1 } end, "bogus", "an unknown field")
    raises(function() qs.add_slider { accessible_name = "x", value = 2 } end, "value", "value 2")
    raises(function() qs.add_slider { accessible_name = "x", value = -1 } end, "value", "value -1")
    raises(function() qs.add_slider { accessible_name = "x", value = "a" } end, "value", "a string as value")
    raises(function() qs.add_slider { accessible_name = "x", colspan = 3 } end, "colspan", "colspan 3")
    raises(function() qs.add_slider { accessible_name = "x", index = 0 } end, "index", "index 0")
    raises(function() qs.add_slider { accessible_name = "x", icon_name = "a", icon_file = "b.svg" } end,
      { "icon_name", "icon_file" }, "two icons")
    raises(function() qs.add_slider { accessible_name = "x", on_icon_click = function() end } end,
      "on_icon_click", "on_icon_click without icon_reactive")
    raises(function()
      qs.add_slider { accessible_name = "x", icon_reactive = false, on_icon_click = function() end }
    end, "on_icon_click", "on_icon_click with icon_reactive = false")
    raises(function() qs.add_slider { accessible_name = "x", on_changed = 1 } end, "on_changed", "a number as on_changed")
    raises(function() qs.add_slider { accessible_name = "x", title = "t" } end, "title", "a field of the toggles")
    raises(function() qs.add_slider { accessible_name = "x", on_toggle = function() end } end, "on_toggle",
      "on_toggle on a slider")
  end)

  case("the events of a slider as methods", function()
    local s = qs.add_slider { accessible_name = "E" }
    local conn = s:on_changed(function() end)

    eq(type(conn.disconnect), "function", "Connection:disconnect")
    conn:disconnect()
    raises(function() s:on_icon_click(function() end) end, "icon_reactive", "on_icon_click without icon_reactive")
    s.icon_reactive = true
    local conn2 = s:on_icon_click(function() end)
    eq(type(conn2.disconnect), "function", "Connection:disconnect")
    raises(function() s:on_changed("f") end, nil, "a string as callback")
    s:destroy()
  end)

  -- -------------------------------------------------------------------
  -- add_indicator
  -- -------------------------------------------------------------------
  case("add_indicator", function()
    local i = qs.add_indicator { icon_name = "alarm-symbolic", accessible_name = "Alarm", visible = false }

    truthy(is_type(i, "QuickIndicator"), "tostring: " .. tostring(i))
    eq(i.alive, true, "alive")
    eq(i.icon_name, "alarm-symbolic", "icon_name")
    eq(i.accessible_name, "Alarm", "accessible_name")
    eq(i.visible, false, "visible")
    eq(math.type(i.index), "integer", "index")

    i.visible = true
    eq(i.visible, true, "visible written")
    i.icon_name = "dialog-information-symbolic"
    eq(i.icon_name, "dialog-information-symbolic", "icon_name written")
    i.accessible_name = "Info"
    eq(i.accessible_name, "Info", "accessible_name written")

    raises(function() i.index = 1 end, "index", "assigning index")
    raises(function() i.colspan = 1 end, "colspan", "an indicator has no colspan")
    eq(i.colspan, nil, "colspan reads as nil")
    eq(i.title, nil, "an indicator has no title")
    keep.indicator = i
  end)

  case("add_indicator raises on a misuse", function()
    raises(function() qs.add_indicator {} end, "icon", "no icon")
    raises(function() qs.add_indicator { icon_name = "a", icon_file = "b.svg" } end, { "icon_name", "icon_file" },
      "two icons")
    raises(function() qs.add_indicator { icon_name = "a", bogus = 1 } end, "bogus", "an unknown field")
    raises(function() qs.add_indicator { icon_name = 1 } end, "icon_name", "a number as icon_name")
    raises(function() qs.add_indicator { icon_name = "a", visible = "yes" } end, "visible", "a string as visible")
    raises(function() qs.add_indicator { icon_name = "a", index = 0 } end, "index", "index 0")
    raises(function() qs.add_indicator { icon_name = "a", colspan = 1 } end, "colspan", "colspan on an indicator")
    raises(function() qs.add_indicator { icon_name = "a", sensitive = true } end, "sensitive",
      "sensitive on an indicator")
    raises(function() qs.add_indicator { icon_file = "missing.svg" } end, "icon_file", "a missing icon file")
  end)

  -- -------------------------------------------------------------------
  -- destroy (README: Handles, Ownership and teardown)
  -- -------------------------------------------------------------------
  case("destroy kills the handle and is idempotent", function()
    local t = qs.add_toggle { title = "D" }
    local index = t.index

    t:destroy()
    eq(t.alive, false, "alive after destroy")
    t:destroy()
    t:destroy()
    raises(function() return t.title end, "no longer valid", "reading a dead handle")
    raises(function() t.title = "x" end, "no longer valid", "writing a dead handle")
    raises(function() return t.index end, "no longer valid", "index of a dead handle")
    raises(function() t:on_toggle(function() end) end, "no longer valid", "a method of a dead handle")
    truthy(tostring(t):find("dead", 1, true), "tostring of a dead handle: " .. tostring(t))
    truthy(t == t, "a dead handle equals itself")

    -- its slot is free immediately
    local u = qs.add_toggle { title = "after D" }
    eq(u.index, index, "the slot of the destroyed item")
    u:destroy()
  end)

  case("destroying an item with a menu destroys the menu", function()
    local m = qs.add_menu_toggle { title = "DM" }
    local menu = m.menu
    local item = menu:add_item { label = "i" }

    m:destroy()
    eq(m.alive, false, "the item")
    eq(menu.alive, false, "its menu")
    eq(item.alive, false, "the items of its menu")

    local s = qs.add_slider { accessible_name = "DS" }
    local smenu = s.menu
    s:destroy()
    eq(s.alive, false, "the slider")
    eq(smenu.alive, false, "its menu")
  end)

  case("destroying a status icon", function()
    local i = qs.add_indicator { icon_name = "alarm-symbolic" }
    local index = i.index

    i:destroy()
    eq(i.alive, false, "alive after destroy")
    i:destroy()
    raises(function() return i.icon_name end, "no longer valid", "reading a dead handle")

    local j = qs.add_indicator { icon_name = "alarm-symbolic" }
    eq(j.index, index, "the slot of the destroyed icon")
    j:destroy()
  end)

  case("items and icons are independent", function()
    local i = qs.add_indicator { icon_name = "alarm-symbolic" }
    local t = qs.add_toggle { title = "X" }

    t:destroy()
    eq(i.alive, true, "the icon survives the destruction of an item")
    local t2 = qs.add_toggle { title = "Y" }
    i:destroy()
    eq(t2.alive, true, "the item survives the destruction of an icon")
    t2:destroy()
  end)

  -- -------------------------------------------------------------------
  -- Hooks (hooks.md): quick-settings.opened
  -- -------------------------------------------------------------------
  case("the hook quick-settings.opened is declared", function()
    local found

    for _, hook in ipairs(shell.hooks.list()) do
      if hook.name == "quick-settings.opened" then
        found = hook
      end
    end
    truthy(found, "quick-settings.opened in shell.hooks.list ()")
    eq(found.cancellable, false, "cancellable")
    raises(function()
      shell.hooks.before("quick-settings.opened", function() end)
    end, nil, "before () on a hook that cannot be cancelled")

    keep.opened_conn = shell.hooks.on("quick-settings.opened", function(payload)
      opened.n = opened.n + 1
      opened.payload = payload
    end)
    eq(type(keep.opened_conn), "userdata", "a connection")
  end)

  -- -------------------------------------------------------------------
  -- Steps of the script: the system menu is opened and closed
  -- -------------------------------------------------------------------
  steps {
    -- The script waited for the shell to be idle
    ["assignments-later"] = function()
      eq(keep.r7.calls(), 0, "click callbacks called after the assignments")
      eq(keep.r7.values(), 3, "on_changed of the slider after the assignments")
      for _, item in ipairs(keep.r7.items) do
        item:destroy()
      end
    end,

    -- The script opened the system menu
    ["opened"] = function()
      truthy(opened.n >= 1, "the hook ran when the menu opened")
      eq(type(opened.payload), "table", "payload")
      eq(next(opened.payload), nil, "the payload is empty")

      local menu, owner = keep.menu, keep.menu_owner
      eq(menu.alive, true, "the menu is alive")
      eq(menu.is_open, false, "closed")
      -- the item is on screen: its menu can open
      eq(menu:open { animate = false }, true, "open () while the system menu is open")
      eq(menu.is_open, true, "open")
      eq(menu:open { animate = false }, false, "open () of an open menu changes nothing")
      eq(menu:close { animate = false }, true, "close ()")
      eq(menu.is_open, false, "closed again")

      -- an item that is not visible has no menu to open
      owner.visible = false
      eq(menu:open { animate = false }, false, "open () of the menu of a hidden item")
      owner.visible = true
    end,

    -- The script closed the system menu
    ["closed"] = function()
      local menu = keep.menu
      eq(menu.is_open, false, "closed with the system menu")
      eq(menu:open { animate = false }, false, "open () while the system menu is closed")
      eq(menu.is_open, false, "still closed")
    end,

    -- The script opened it again: the hook runs every time
    ["opened-again"] = function()
      truthy(opened.n >= 2, "the hook ran again: " .. tostring(opened.n))
    end,
  }

  T.done()
end

-- Defining disable () is optional (README): this one only checks what it can
function ext.disable()
  case("disable () runs before the objects are destroyed", function()
    eq(keep.plain.alive, true, "toggle")
    eq(keep.full.alive, true, "toggle with every field")
    eq(keep.menu_toggle.alive, true, "menu toggle")
    eq(keep.menu.alive, true, "menu")
    eq(keep.slider.alive, true, "slider")
    eq(keep.indicator.alive, true, "status icon")
    eq(keep.plain.title, "Plain", "a handle can be read in disable ()")
  end)
end

return ext
