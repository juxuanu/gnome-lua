-- Fixture of tests/api/ui.lua: every constructor, field, property, method and
-- error kind of docs/api/ui.md. enable () runs the checks that need nothing
-- but the extension; the script of the test asks for the one that is heavy
-- (the cap on owned objects) with a step (tests/api/README.md).
--
-- Not covered here: events caused by the user (a click, a drag, typing),
-- which need input; what is checked is what an assignment calls back (README
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

local EXT_CLASS = "lua-ext-ui-test"   -- the uuid, "ui@test", as ui.md writes it
local CONSTRUCTORS = { "box", "label", "icon", "button", "entry", "switch", "slider", "spacer", "scroll" }

local ext = {}
local kept = {}

function ext.enable()
  -- -------------------------------------------------------------------
  -- The table
  -- -------------------------------------------------------------------
  case("shell.ui", function()
    eq(type(ui), "table", "shell.ui")
    for _, name in ipairs(CONSTRUCTORS) do
      eq(type(ui[name]), "function", "shell.ui." .. name)
    end
    eq(ui.no_such_widget, nil, "an absent function is nil, not a stub")
  end)

  case("constructors: misuse", function()
    for _, name in ipairs(CONSTRUCTORS) do
      raises(function() ui[name]() end, nil, name .. " without a table")
      raises(function() ui[name]("x") end, nil, name .. " with a string")
      raises(function() ui[name]{ bogus = 1 } end, "bogus", name .. " with an unknown field")
      raises(function() ui[name]{ visible = 1 } end, "visible", name .. " with a number as visible")
      raises(function() ui[name]{ opacity = 256 } end, "opacity", name .. " with opacity 256")
      raises(function() ui[name]{ opacity = -1 } end, "opacity", name .. " with opacity -1")
      raises(function() ui[name]{ opacity = 1.5 } end, "opacity", name .. " with opacity 1.5")
      raises(function() ui[name]{ x_align = "left" } end, "x_align", name .. " with an unknown alignment")
      raises(function() ui[name]{ style_class = 5 } end, "style_class", name .. " with a number as style_class")
    end
    -- fields of another widget are unknown ones
    raises(function() ui.label{ state = true } end, "state", "state on a label")
    raises(function() ui.box{ text = "x" } end, "text", "text on a box")
    raises(function() ui.switch{ on_clicked = function() end } end, "on_clicked", "on_clicked on a switch")
    -- a failed constructor leaves nothing behind (the harness compares the stage)
  end)

  -- -------------------------------------------------------------------
  -- Common properties
  -- -------------------------------------------------------------------
  case("defaults and the type of every widget", function()
    local types = {
      box = "shell.Box", label = "shell.Label", icon = "shell.Icon", button = "shell.Button",
      entry = "shell.Entry", switch = "shell.Switch", slider = "shell.Slider",
      spacer = "shell.Spacer", scroll = "shell.Scroll",
    }

    for name, type_name in pairs(types) do
      local w = ui[name]{}
      truthy(tostring(w):find(type_name, 1, true), "tostring of a " .. name)
      eq(w.alive, true, name .. ".alive")
      eq(w.visible, true, name .. ".visible")
      eq(w.opacity, 255, name .. ".opacity")
      eq(w.x_align, "fill", name .. ".x_align")
      -- the switch of the shell is centered vertically (ui.md)
      eq(w.y_align, name == "switch" and "center" or "fill", name .. ".y_align")
      eq(w.x_expand, name == "spacer", name .. ".x_expand")
      eq(w.y_expand, name == "spacer", name .. ".y_expand")
      eq(w.accessible_name, nil, name .. ".accessible_name")
      eq(w:has_style_class(EXT_CLASS), true, name .. " has the class of the extension")
    end
  end)

  case("common properties: round trip", function()
    local w = ui.label{ text = "x" }

    w.visible = false
    eq(w.visible, false, "visible")
    w.visible = true
    w.reactive = true
    eq(w.reactive, true, "reactive")
    w.x_expand = true
    w.y_expand = true
    eq(w.x_expand, true, "x_expand")
    eq(w.y_expand, true, "y_expand")
    for _, align in ipairs{ "fill", "start", "center", "end" } do
      w.x_align = align
      eq(w.x_align, align, "x_align")
      w.y_align = align
      eq(w.y_align, align, "y_align")
    end
    w.opacity = 0
    eq(w.opacity, 0, "opacity 0")
    w.opacity = 255
    eq(w.opacity, 255, "opacity 255")
    w.opacity = 100
    eq(w.opacity, 100, "opacity")
    w.accessible_name = "A label"
    eq(w.accessible_name, "A label", "accessible_name")

    raises(function() w.visible = "yes" end, "visible", "a string as visible")
    raises(function() w.x_align = "left" end, "x_align", "an unknown alignment")
    raises(function() w.opacity = 256 end, "opacity", "opacity 256")
    raises(function() w.accessible_name = 5 end, "accessible_name", "a number as accessible_name")
  end)

  case("assignments: read-only and unknown properties", function()
    local w = ui.label{}

    raises(function() w.alive = true end, "alive", "assigning alive")
    raises(function() w.nonsense = 1 end, "nonsense", "assigning an unknown property")
    eq(w.nonsense, nil, "an unknown property reads as nil")
    raises(function() w.sensitive = false end, "sensitive", "sensitive on a label")
    eq(w.sensitive, nil, "sensitive of a label")
    raises(function() ui.label{ sensitive = false } end, "sensitive", "sensitive in a label constructor")
  end)

  case("style_class", function()
    local w = ui.label{ style_class = "alpha beta" }

    eq(w.style_class, "alpha beta", "style_class")
    truthy(w:has_style_class("alpha") and w:has_style_class("beta"), "the classes")
    truthy(w:has_style_class(EXT_CLASS), "the class of the extension is always there")

    w.style_class = "gamma"
    eq(w.style_class, "gamma", "style_class after the assignment")
    eq(w:has_style_class("alpha"), false, "alpha is gone")
    truthy(w:has_style_class(EXT_CLASS), "the class of the extension survives an assignment")

    w:add_style_class("delta")
    truthy(w:has_style_class("delta"), "add_style_class")
    w:remove_style_class("delta")
    eq(w:has_style_class("delta"), false, "remove_style_class")
    w:remove_style_class("never-there")   -- harmless
    w:remove_style_class(EXT_CLASS)
    truthy(w:has_style_class(EXT_CLASS), "the class of the extension cannot be removed")
    raises(function() w:add_style_class("two words") end, nil, "a class with a space")
    raises(function() w:add_style_class("") end, nil, "an empty class")
    raises(function() w:has_style_class(5) end, nil, "a number as class")
  end)

  case("style", function()
    local w = ui.label{ style = "color: red; padding: 2px" }

    eq(w.style, "color: red; padding: 2px", "style")
    w.style = "color: blue"
    eq(w.style, "color: blue", "style after the assignment")
    raises(function() w.style = "label { color: red }" end, "style", "a selector")
    raises(function() w.style = "color: red }" end, "style", "a brace")
    raises(function() w.style = ("a"):rep(1025) end, "1024", "a style of 1025 bytes")
    w.style = ("a"):rep(1024)   -- the limit itself is accepted
    raises(function() w.style = 5 end, "style", "a number as style")
  end)

  case("sensitive", function()
    for _, name in ipairs{ "button", "entry", "switch", "slider" } do
      local w = ui[name]{}

      eq(w.sensitive, true, name .. " is sensitive by default")
      w.sensitive = false
      eq(w.sensitive, false, name .. " after sensitive = false")
      w.sensitive = true
      eq(w.sensitive, true, name .. " after sensitive = true")
      raises(function() w.sensitive = 1 end, "sensitive", name .. ": a number as sensitive")
      eq(ui[name]{ sensitive = false }.sensitive, false, name .. " constructed insensitive")
    end
  end)

  case("remove, focus and destroy", function()
    local w = ui.button{ label = "b" }

    w:remove()   -- no parent: a no-op
    eq(w.alive, true, "remove without a parent")
    eq(w:focus(), false, "focus of a widget that is not on the screen")
    w:destroy()
    eq(w.alive, false, "alive after destroy")
    w:destroy()   -- idempotent
    w:destroy()
    truthy(tostring(w):find("dead", 1, true), "tostring of a dead handle")
    eq(w == w, true, "== on a dead handle")
    raises(function() return w.label end, "no longer valid", "reading a dead handle")
    raises(function() w.label = "x" end, "no longer valid", "writing a dead handle")
    raises(function() w:remove() end, "no longer valid", "remove on a dead handle")
    raises(function() w:focus() end, "no longer valid", "focus on a dead handle")
    eq(w.alive, false, "alive on a dead handle never raises")
  end)

  case("the same object is the same handle", function()
    local a = ui.label{ text = "a" }
    local box = ui.box{ children = { a } }

    eq(box:children()[1], a, "children () returns the handle")
    eq(rawequal(box:children()[1], a), true, "the same userdata")
  end)

  -- -------------------------------------------------------------------
  -- Containers
  -- -------------------------------------------------------------------
  case("box", function()
    local a, b, c, d = ui.label{ text = "a" }, ui.label{ text = "b" }, ui.label{ text = "c" }, ui.label{ text = "d" }
    local box = ui.box{ orientation = "vertical", children = { a, b } }

    eq(box.orientation, "vertical", "orientation")
    eq(ui.box{}.orientation, "horizontal", "default orientation")
    box.orientation = "horizontal"
    eq(box.orientation, "horizontal", "orientation after the assignment")
    raises(function() box.orientation = "diagonal" end, "orientation", "an unknown orientation")

    local kids = box:children()
    eq(#kids, 2, "children ()")
    eq(kids[1], a, "first child")
    eq(kids[2], b, "second child")

    box:add(c)
    eq(box:children()[3], c, "add appends")
    box:add(d, 1)
    eq(box:children()[1], d, "add (child, 1) inserts first")
    eq(box:children()[2], a, "add (child, 1) shifts the others")
    eq(#box:children(), 4, "four children")

    local e = ui.label{ text = "e" }
    box:add(e, 100)
    eq(box:children()[5], e, "an index past the end appends")
    raises(function() box:add(ui.label{}, 0) end, "index", "index 0")
    raises(function() box:add(ui.label{}, -1) end, "index", "index -1")
    raises(function() box:add(ui.label{}, "x") end, nil, "a string as index")
    raises(function() box:add("x") end, nil, "a string as child")
    raises(function() box:add() end, nil, "no child")
    raises(function() box:add(box.children) end, nil, "a function as child")

    -- one parent only
    raises(function() box:add(a) end, "already has a parent", "adding a child twice")
    local other = ui.box{}
    raises(function() other:add(a) end, { "already has a parent", "remove()" }, "adding a child of another box")
    a:remove()
    eq(#box:children(), 4, "remove detaches")
    eq(a.alive, true, "a removed widget stays alive")
    other:add(a)
    eq(other:children()[1], a, "a removed widget can be added again")
    raises(function() ui.box{ children = { a } } end, "already has a parent", "a child that has a parent in a constructor")

    -- loops
    local inner = ui.box{}
    local outer = ui.box{ children = { inner } }
    raises(function() inner:add(outer) end, "descendant", "adding an ancestor")
    raises(function() outer:add(outer) end, nil, "adding a box to itself")

    -- a container only: the others have no add
    raises(function() ui.label{}:add(ui.label{}) end, nil, "add on a label")
    raises(function() ui.button{}:add(ui.label{}) end, nil, "add on a button")

    -- clear destroys the children
    box:clear()
    eq(#box:children(), 0, "clear")
    for _, w in ipairs{ b, c, d, e } do
      eq(w.alive, false, "a child destroyed by clear")
    end
    eq(box.alive, true, "clear keeps the box")
    box:clear()   -- on an empty box: harmless
  end)

  case("destroying a container destroys its children", function()
    local leaf = ui.label{}
    local middle = ui.box{ children = { leaf } }
    local top = ui.box{ children = { middle } }

    top:destroy()
    eq(top.alive, false, "top")
    eq(middle.alive, false, "middle")
    eq(leaf.alive, false, "leaf")

    -- destroying a child frees its place in the parent
    local a, b = ui.label{}, ui.label{}
    local box = ui.box{ children = { a, b } }
    a:destroy()
    eq(#box:children(), 1, "children () after a child was destroyed")
    eq(box:children()[1], b, "the other child")
  end)

  case("scroll", function()
    local content = ui.box{ orientation = "vertical" }
    local s = ui.scroll{ child = content }

    eq(s.child, content, "child")
    eq(s.hscrollbar_policy, "never", "default hscrollbar_policy")
    eq(s.vscrollbar_policy, "automatic", "default vscrollbar_policy")
    for _, policy in ipairs{ "automatic", "always", "never", "external" } do
      s.hscrollbar_policy = policy
      eq(s.hscrollbar_policy, policy, "hscrollbar_policy")
      s.vscrollbar_policy = policy
      eq(s.vscrollbar_policy, policy, "vscrollbar_policy")
    end
    raises(function() s.vscrollbar_policy = "sometimes" end, "vscrollbar_policy", "an unknown policy")
    raises(function() ui.scroll{ hscrollbar_policy = "sometimes" } end, "hscrollbar_policy", "an unknown policy in a constructor")

    eq(ui.scroll{}.child, nil, "a scroll without child")
    raises(function() ui.scroll{ child = ui.label{} } end, "shell.Box", "a label as child")
    raises(function() ui.scroll{ child = "x" } end, nil, "a string as child")
    raises(function() ui.scroll{ child = content } end, "already has a parent", "a child that is in use")

    local second = ui.box{}
    s.child = second
    eq(s.child, second, "child after the assignment")
    s.child = nil
    eq(s.child, nil, "child = nil")

    content:destroy()
    local t = ui.scroll{ child = ui.box{} }
    local kid = t.child
    t:destroy()
    eq(kid.alive, false, "destroying a scroll destroys its child")
  end)

  -- -------------------------------------------------------------------
  -- label, icon
  -- -------------------------------------------------------------------
  case("label", function()
    local l = ui.label{ text = "Hello", ellipsize = "end" }

    eq(l.text, "Hello", "text")
    eq(l.ellipsize, "end", "ellipsize")
    eq(ui.label{}.ellipsize, "none", "default ellipsize")
    eq(ui.label{}.text, "", "default text")
    l.text = "World"
    eq(l.text, "World", "text after the assignment")
    for _, mode in ipairs{ "none", "start", "middle", "end" } do
      l.ellipsize = mode
      eq(l.ellipsize, mode, "ellipsize")
    end
    raises(function() l.ellipsize = "never" end, "ellipsize", "an unknown ellipsize")
    raises(function() l.text = 5 end, "text", "a number as text")
    raises(function() ui.label{ text = nil, ellipsize = 1 } end, "ellipsize", "a number as ellipsize")

    l:set_markup("<b>bold</b> text")
    eq(l.text, "bold text", "the plain text after set_markup")
    raises(function() l:set_markup("<b>unclosed") end, "markup", "invalid markup")
    eq(l.text, "bold text", "invalid markup changes nothing")
    raises(function() l:set_markup(5) end, nil, "a number as markup")
    l.text = "<b>literal</b>"
    eq(l.text, "<b>literal</b>", "an assignment after markup is literal")
  end)

  case("icon", function()
    local i = ui.icon{ icon_name = "alarm-symbolic", icon_size = 24 }

    eq(i.icon_name, "alarm-symbolic", "icon_name")
    eq(i.icon_file, nil, "icon_file of a themed icon")
    eq(i.icon_size, 24, "icon_size")
    eq(ui.icon{}.icon_size, nil, "an unset icon_size")
    eq(ui.icon{}.icon_name, nil, "an unset icon_name")
    i.icon_size = nil
    eq(i.icon_size, nil, "icon_size = nil unsets it")
    raises(function() i.icon_size = 0 end, "icon_size", "icon_size 0")
    raises(function() i.icon_size = "big" end, "icon_size", "a string as icon_size")
    raises(function() i.icon_name = 5 end, "icon_name", "a number as icon_name")

    -- a bundled file; setting one clears the other
    i.icon_file = "icon.svg"
    eq(i.icon_file, "icon.svg", "icon_file")
    eq(i.icon_name, nil, "setting icon_file clears icon_name")
    i.icon_name = "alarm-symbolic"
    eq(i.icon_file, nil, "setting icon_name clears icon_file")
    eq(i.icon_name, "alarm-symbolic", "icon_name again")
    eq(ui.icon{ icon_file = "icon.svg" }.icon_file, "icon.svg", "icon_file in the constructor")

    raises(function() ui.icon{ icon_name = "a", icon_file = "icon.svg" } end, "mutually exclusive", "both")
    raises(function() i.icon_file = "/etc/passwd.svg" end, "icon_file", "an absolute path")
    raises(function() i.icon_file = "../icon.svg" end, "icon_file", "..")
    raises(function() i.icon_file = "a/../../icon.svg" end, "icon_file", ".. inside a path")
    raises(function() i.icon_file = "missing.svg" end, "icon_file", "a missing file")
    raises(function() i.icon_file = "metadata.json" end, "icon_file", "a wrong suffix")
    raises(function() i.icon_file = "" end, "icon_file", "an empty path")
    raises(function() i.icon_file = 5 end, "icon_file", "a number as icon_file")
    eq(i.icon_name, "alarm-symbolic", "a refused icon_file changes nothing")
  end)

  -- -------------------------------------------------------------------
  -- button, entry, switch, slider, spacer
  -- -------------------------------------------------------------------
  case("button", function()
    local b = ui.button{ label = "Press" }

    eq(b.label, "Press", "label")
    b.label = "Again"
    eq(b.label, "Again", "label after the assignment")
    eq(b.toggle_mode, false, "toggle_mode")
    raises(function() b.toggle_mode = true end, "toggle_mode", "toggle_mode is construct only")
    raises(function() b.label = 5 end, "label", "a number as label")

    local t = ui.button{ label = "T", toggle_mode = true, checked = true }
    eq(t.toggle_mode, true, "toggle_mode = true")
    eq(t.checked, true, "checked")
    t.checked = false
    eq(t.checked, false, "checked after the assignment")
    raises(function() ui.button{ toggle_mode = 1 } end, "toggle_mode", "a number as toggle_mode")

    -- icon-only buttons need a name for the screen reader
    raises(function() ui.button{ icon_name = "alarm-symbolic" } end, "accessible_name", "an icon button without a name")
    raises(function() ui.button{ icon_file = "icon.svg" } end, "accessible_name", "an icon file button without a name")
    raises(function() ui.button{ icon_name = "alarm-symbolic", accessible_name = "" } end, "accessible_name", "an empty name")
    local ib = ui.button{ icon_name = "alarm-symbolic", accessible_name = "Alarm" }
    eq(ib.icon_name, "alarm-symbolic", "icon_name")
    eq(ib.accessible_name, "Alarm", "accessible_name")
    local fb = ui.button{ icon_file = "icon.svg", accessible_name = "File" }
    eq(fb.icon_file, "icon.svg", "icon_file")
    raises(function() fb.icon_file = "missing.svg" end, "icon_file", "a missing file")

    -- label, icon and child exclude each other
    raises(function() ui.button{ label = "x", icon_name = "alarm-symbolic", accessible_name = "x" } end,
           "mutually exclusive", "label and icon_name")
    raises(function() ui.button{ label = "x", child = ui.label{} } end, "mutually exclusive", "label and child")
    raises(function() ui.button{ icon_name = "a", icon_file = "icon.svg", accessible_name = "x" } end,
           "mutually exclusive", "icon_name and icon_file")

    -- child
    local content = ui.label{ text = "inside" }
    local cb = ui.button{ child = content }
    eq(cb.child, content, "child")
    raises(function() ui.button{ child = content } end, "already has a parent", "a child that is in use")
    cb.child = nil
    eq(cb.child, nil, "child = nil")
    eq(content.alive, true, "the child outlives the button's use of it")

    -- events
    local calls = 0
    local conn = b:on_clicked(function() calls = calls + 1 end)
    eq(type(conn), "userdata", "on_clicked returns a connection")
    eq(conn:is_connected(), true, "is_connected")
    conn:disconnect()
    eq(conn:is_connected(), false, "disconnected")
    ui.button{ label = "c", on_clicked = function() calls = calls + 1 end }
    raises(function() ui.button{ on_clicked = 5 } end, "on_clicked", "a number as on_clicked")
    raises(function() b:on_clicked() end, nil, "on_clicked without a function")
    raises(function() b:on_activate(function() end) end, nil, "on_activate does not exist on a button")
    eq(calls, 0, "nothing was clicked")
  end)

  case("entry", function()
    local e = ui.entry{ text = "abc", hint_text = "Type here" }

    eq(e.text, "abc", "text")
    eq(e.hint_text, "Type here", "hint_text")
    eq(ui.entry{}.text, "", "default text")

    -- rule 7: an assignment calls on_changed, and is not an activation
    local changed, activated = {}, 0
    local watched = ui.entry{
      on_changed = function(self, text) changed[#changed + 1] = text end,
      on_activate = function() activated = activated + 1 end,
    }
    watched.text = "assigned"
    eq(watched.text, "assigned", "text after the assignment")
    watched.text = ""
    eq(table.concat(changed, "|"), "assigned|", "on_changed for each assignment")
    eq(activated, 0, "on_activate is not called by an assignment")

    local c1 = e:on_changed(function() end)
    local c2 = e:on_activate(function() end)
    c1:disconnect()
    c2:disconnect()
    raises(function() e.text = 5 end, "text", "a number as text")
    raises(function() e:on_clicked(function() end) end, nil, "on_clicked does not exist on an entry")
  end)

  case("switch", function()
    local s = ui.switch{ state = true }

    eq(s.state, true, "state")
    eq(ui.switch{}.state, false, "default state")
    s.state = false
    eq(s.state, false, "state after the assignment")
    raises(function() s.state = 1 end, "state", "a number as state")

    -- rule 7: each change of state calls on_changed, a repeated value does not
    local states = {}
    local watched = ui.switch{ on_changed = function(self, state) states[#states + 1] = tostring(state) end }
    watched.state = true
    watched.state = true
    watched.state = false
    eq(table.concat(states, ","), "true,false", "on_changed for each change")
    watched:on_changed(function() end):disconnect()
  end)

  case("slider", function()
    local s = ui.slider{ value = 0.25 }

    eq(s.value, 0.25, "value")
    eq(ui.slider{}.value, 0.0, "default value")
    s.value = 1
    eq(s.value, 1.0, "value 1")
    s.value = 0
    eq(s.value, 0.0, "value 0")
    raises(function() s.value = 1.5 end, "value", "1.5")
    raises(function() s.value = -0.1 end, "value", "-0.1")
    raises(function() s.value = "half" end, "value", "a string as value")
    raises(function() ui.slider{ value = 2 } end, "value", "2 in a constructor")

    local changes, drags = {}, 0
    local watched = ui.slider{
      on_changed = function(self, value) changes[#changes + 1] = value end,
      on_drag_begin = function() drags = drags + 1 end,
      on_drag_end = function() drags = drags + 1 end,
    }
    watched.value = 0.5
    watched.value = 0.75
    eq(watched.value, 0.75, "value")
    -- rule 7: each assignment calls on_changed; an assignment is not a drag
    eq(table.concat(changes, ","), "0.5,0.75", "on_changed for each assignment")
    eq(drags, 0, "no drag")
    watched:on_changed(function() end):disconnect()
    watched:on_drag_begin(function() end):disconnect()
    watched:on_drag_end(function() end):disconnect()
  end)

  case("spacer", function()
    local s = ui.spacer{}

    eq(s.width, -1, "default width")
    eq(s.height, -1, "default height")
    eq(s.x_expand, true, "x_expand defaults to true")
    eq(s.y_expand, true, "y_expand defaults to true")
    s.width = 12
    s.height = 8
    eq(s.width, 12, "width")
    eq(s.height, 8, "height")
    s.width = -1
    eq(s.width, -1, "width = -1")
    eq(ui.spacer{ width = 5, height = 6 }.width, 5, "width in the constructor")
    eq(ui.spacer{ x_expand = false }.x_expand, false, "x_expand = false")
    raises(function() s.width = -2 end, "width", "width -2")
    raises(function() s.height = "tall" end, "height", "a string as height")
    raises(function() s.height = 1.5 end, "height", "a fraction as height")
  end)

  case("CSS: the class of the extension", function()
    -- ui.md "Common properties": lua-ext-<id> is added to every widget
    for _, name in ipairs(CONSTRUCTORS) do
      truthy(ui[name]{ style_class = "mine" }:has_style_class(EXT_CLASS), name)
    end
  end)

  -- -------------------------------------------------------------------
  -- Things that stay alive until disable, and the cap
  -- -------------------------------------------------------------------
  kept.label = ui.label{ text = "kept" }
  kept.box = ui.box{ children = { ui.button{ label = "kept child" } } }

  steps {
    cap = function()
      -- A runtime SHOULD cap the owned objects (reference value 4096) and
      -- raise beyond it; destroy () frees the slot at once.
      local made = {}
      local raised

      for i = 1, 5000 do
        local ok, err = pcall(ui.spacer, {})
        if not ok then
          raised = tostring(err)
          break
        end
        made[#made + 1] = err
      end
      truthy(raised, "the cap was reached")
      truthy(raised:find("too many", 1, true), "the error of the cap: " .. raised)
      truthy(#made <= 4096 and #made > 3800, "the cap is about 4096 (made " .. #made .. ")")

      made[1]:destroy()
      truthy(pcall(ui.spacer, {}), "a slot was freed by destroy ()")
      raises(function() ui.label{} end, "too many", "the cap again")

      for _, w in ipairs(made) do
        w:destroy()
      end
      truthy(pcall(ui.label, {}), "widgets can be made again once the others are destroyed")
    end,
  }

  T.done()
end

-- Defining disable () is optional (README): this one only checks what it can
function ext.disable()
  case("disable () runs before the objects are destroyed", function()
    eq(kept.label.alive, true, "the label is alive in disable ()")
    eq(kept.box.alive, true, "the box is alive in disable ()")
  end)
end

return ext
