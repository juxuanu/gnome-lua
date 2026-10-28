-- Fixture of tests/api/window.lua: every function, field, property and error
-- kind of docs/api/window.md, and the window.*, workspace.* and monitors.changed
-- hooks of docs/api/hooks.md as the shell emits them. enable () runs the checks
-- that are synchronous and connects a listener to every hook; the script of
-- the test opens a window, changes the monitors and asks for the verdicts with
-- steps (tests/api/README.md).
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

-- ---------------------------------------------------------------------
-- What this fixture needs besides
-- ---------------------------------------------------------------------
local function is_int(v)
  return math.type(v) == "integer"
end

-- Polls pred () every 50 ms: on_ok () when it holds, a failed check and done ()
-- after `ms`. Returns "later" for a step handler.
local function wait_for(pred, what, on_ok, done, ms)
  local waited = 0

  shell.timer.every{ ms = 50, callback = function()
    waited = waited + 50
    local ok, result = pcall(pred)
    if not ok then
      T.report("waiting for " .. what, false, tostring(result))
      done()
      return false
    end
    if result then
      local ok2, err = pcall(on_ok)
      if not ok2 then
        T.report("after waiting for " .. what, false, tostring(err))
      end
      done()
      return false
    end
    if waited >= (ms or 8000) then
      T.report("waiting for " .. what, false, "timed out")
      done()
      return false
    end
  end }
  return "later"
end

local WINDOW_HOOKS = {
  "window.mapped", "window.unmapped", "window.focused", "window.minimized", "window.unminimized",
  "window.title-changed", "window.workspace-changed", "window.monitor-changed", "window.state-changed",
}
local WORKSPACE_HOOKS = { "workspace.switched", "workspace.added", "workspace.removed" }

local ext = {}

-- everything the hooks said since the extension was enabled
local kept = { events = {}, conns = {} }

-- the first event after index `from` of that hook (and window)
local function find_event(hook, id, from)
  for i = (from or 0) + 1, #kept.events do
    local ev = kept.events[i]

    if ev.hook == hook and (id == nil or ev.p["window-id"] == id) then
      return ev, i
    end
  end
end

local function check_w(p)
  eq(is_int(p["window-id"]), true, "window-id is an integer")
  eq(type(p.title), "string", "title")
  eq(type(p["wm-class"]), "string", "wm-class")
  if p["app-id"] ~= nil then
    eq(type(p["app-id"]), "string", "app-id")
  end
  if p.workspace ~= nil then
    eq(is_int(p.workspace), true, "workspace")
    truthy(p.workspace >= 1, "workspace is 1-based")
  end
  eq(is_int(p.monitor), true, "monitor")
  truthy(p.monitor >= 1, "monitor is 1-based")
  if p.pid ~= nil then
    eq(is_int(p.pid), true, "pid")
  end
end

-- What the shell says when a hook has just been emitted: the new state is
-- visible through the API (hooks.md, "Rules for emitters")
local function check_event(name, p)
  if name:find("^window%.") then
    check_w(p)
    local id = p["window-id"]
    local w = shell.windows.get(id)

    if name == "window.unmapped" then
      eq(w, nil, "windows.get (id) in window.unmapped: the window is gone")
      return
    end
    w = truthy(w, "windows.get (window-id) in " .. name)
    eq(w.id, id, "id")
    if name == "window.mapped" then
      eq(w.title, p.title, "title")
      eq(w.wm_class, p["wm-class"], "wm-class")
      eq(w.app_id, p["app-id"], "app-id")
      eq(w.workspace, p.workspace, "workspace")
      eq(w.monitor, p.monitor, "monitor")
      eq(w.pid, p.pid, "pid")
    elseif name == "window.focused" then
      local focused = shell.windows.focused()
      truthy(focused ~= nil and focused.id == id, "windows.focused () is the window of window.focused")
      eq(w.focused, true, "focused")
      if p["previous-window-id"] ~= nil then
        eq(is_int(p["previous-window-id"]), true, "previous-window-id")
        truthy(p["previous-window-id"] ~= id, "previous-window-id is another window")
      end
    elseif name == "window.minimized" then
      eq(w.minimized, true, "minimized in window.minimized")
    elseif name == "window.unminimized" then
      eq(w.minimized, false, "minimized in window.unminimized")
    elseif name == "window.title-changed" then
      eq(type(p["old-title"]), "string", "old-title")
      eq(w.title, p.title, "title")
      truthy(p["old-title"] ~= p.title, "the title did change")
    elseif name == "window.workspace-changed" then
      for _, key in ipairs({ "from", "to" }) do
        if p[key] ~= nil then
          eq(is_int(p[key]), true, key)
        end
      end
      eq(w.workspace, p.to, "workspace in window.workspace-changed")
      eq(p.workspace, p.to, "workspace of the snapshot is the new one")
    elseif name == "window.monitor-changed" then
      for _, key in ipairs({ "from", "to" }) do
        if p[key] ~= nil then
          eq(is_int(p[key]), true, key)
        end
      end
      eq(w.monitor, p.to, "monitor in window.monitor-changed")
    elseif name == "window.state-changed" then
      eq(type(p.maximized), "boolean", "maximized")
      eq(type(p.fullscreen), "boolean", "fullscreen")
      eq(type(p.changed), "table", "changed")
      truthy(#p.changed >= 1, "changed lists what changed")
      eq(w.maximized, p.maximized, "maximized in window.state-changed")
      eq(w.fullscreen, p.fullscreen, "fullscreen in window.state-changed")
      for _, what in ipairs(p.changed) do
        truthy(what == "maximized" or what == "fullscreen", "changed: " .. tostring(what))
      end
    end
  elseif name == "workspace.switched" then
    eq(is_int(p.from), true, "from")
    eq(is_int(p.to), true, "to")
    eq(shell.workspaces.active().index, p.to, "the active workspace in workspace.switched")
  elseif name == "workspace.added" then
    eq(is_int(p.index), true, "index")
    truthy(shell.workspaces.get(p.index) ~= nil, "the new workspace exists in workspace.added")
  elseif name == "workspace.removed" then
    eq(is_int(p.index), true, "index")
    truthy(p.index >= 1, "index is 1-based")
  elseif name == "monitors.changed" then
    eq(is_int(p.count), true, "count")
    eq(is_int(p.primary), true, "primary")
    eq(#shell.monitors.list(), p.count, "count is #monitors.list ()")
    if p.count > 0 then
      eq(shell.monitors.primary().index, p.primary, "primary is monitors.primary ().index")
    else
      eq(p.primary, 0, "primary is 0 without monitors")
    end
  end
end

local function record(name, p)
  local ok, err = pcall(check_event, name, p)

  kept.events[#kept.events + 1] = { hook = name, p = p, ok = ok }
  T.report(string.format("%s #%d", name, #kept.events), ok, not ok and tostring(err) or nil)
end

function ext.enable()
  -- -------------------------------------------------------------------
  -- Surface
  -- -------------------------------------------------------------------
  case("the tables of window.md and their functions", function()
    local functions = {
      windows = { "list", "focused", "get" },
      workspaces = { "list", "count", "active", "get" },
      monitors = { "list", "primary", "get" },
      apps = { "launch", "running", "favorites", "info" },
    }

    for module, names in pairs(functions) do
      eq(type(shell[module]), "table", "shell." .. module)
      for _, name in ipairs(names) do
        eq(type(shell[module][name]), "function", "shell." .. module .. "." .. name)
      end
      eq(shell[module].no_such_function, nil, "shell." .. module .. ".no_such_function")
    end
  end)

  case("the hooks of window.md are declared, none is cancellable", function()
    local declared = {}

    for _, entry in ipairs(shell.hooks.list()) do
      declared[entry.name] = entry
    end
    local names = { "monitors.changed" }
    for _, list in ipairs({ WINDOW_HOOKS, WORKSPACE_HOOKS }) do
      for _, name in ipairs(list) do
        names[#names + 1] = name
      end
    end
    for _, name in ipairs(names) do
      truthy(declared[name], name .. " is declared")
      eq(declared[name].cancellable, false, name .. " cancellable")
    end
  end)

  -- -------------------------------------------------------------------
  -- shell.workspaces
  -- -------------------------------------------------------------------
  case("workspaces: list, count, active, get", function()
    local list = shell.workspaces.list()
    local n = shell.workspaces.count()
    local active = 0

    eq(is_int(n), true, "count () is an integer")
    truthy(n >= 1, "there is a workspace")
    eq(#list, n, "#list () == count ()")
    for i, ws in ipairs(list) do
      eq(type(ws), "userdata", "a Workspace is a handle")
      eq(is_int(ws.index), true, "index is an integer")
      eq(ws.index, i, "index of the workspace " .. i .. " (1-based)")
      eq(type(ws.active), "boolean", "active")
      if ws.active then
        active = active + 1
      end
      eq(is_int(ws.n_windows), true, "n_windows is an integer")
      eq(ws.alive, true, "alive")
      local windows = ws:windows()
      eq(type(windows), "table", "windows ()")
      eq(#windows, ws.n_windows, "n_windows is the number of windows ()")
      truthy(rawequal(shell.workspaces.get(i), ws), "get (i) is the handle that list () has")
    end
    eq(active, 1, "exactly one workspace is active")
    local current = shell.workspaces.active()
    eq(current.active, true, "active () is active")
    truthy(rawequal(list[current.index], current), "active () is in list ()")
    eq(shell.workspaces.get(0), nil, "get (0)")
    eq(shell.workspaces.get(-1), nil, "get (-1)")
    eq(shell.workspaces.get(n + 1), nil, "get (count () + 1)")
    raises(function() shell.workspaces.get("x") end, nil, "get ('x')")
    raises(function() shell.workspaces.get() end, nil, "get ()")
    raises(function() shell.workspaces.get(1.5) end, nil, "get (1.5)")
  end)

  case("a Workspace is a read-only handle", function()
    local ws = shell.workspaces.get(1)

    eq(tostring(ws):find("shell.Workspace", 1, true) ~= nil, true, "tostring")
    eq(ws.no_such_property, nil, "unknown properties are nil")
    raises(function() ws.index = 2 end, nil, "assigning index")
    raises(function() ws.active = true end, nil, "assigning active")
    raises(function() ws.n_windows = 1 end, nil, "assigning n_windows")
    raises(function() ws.no_such_property = 1 end, nil, "assigning an unknown property")
    raises(function() ws:no_such_method() end, nil, "calling an unknown method")
    eq(ws.alive, true, "alive")
  end)

  -- -------------------------------------------------------------------
  -- shell.monitors
  -- -------------------------------------------------------------------
  case("monitors: list, primary, get", function()
    local list = shell.monitors.list()
    local primary = 0
    local primary_index

    truthy(#list >= 1, "there is a monitor")
    for i, m in ipairs(list) do
      eq(type(m), "table", "a monitor is a plain table, not a handle")
      eq(m.index, i, "index (1-based, in order)")
      for _, key in ipairs({ "x", "y", "width", "height" }) do
        eq(is_int(m[key]), true, "monitor " .. i .. ": " .. key .. " is an integer")
      end
      truthy(m.width > 0 and m.height > 0, "the size of the monitor " .. i)
      eq(type(m.scale), "number", "scale")
      truthy(m.scale > 0, "scale is positive")
      eq(type(m.primary), "boolean", "primary")
      if m.primary then
        primary = primary + 1
        primary_index = m.index
      end
      local wa = m.work_area
      eq(type(wa), "table", "work_area")
      for _, key in ipairs({ "x", "y", "width", "height" }) do
        eq(is_int(wa[key]), true, "work_area." .. key .. " is an integer")
      end
      truthy(wa.width > 0 and wa.height > 0, "the size of the work area")
      truthy(wa.x >= m.x and wa.y >= m.y and wa.x + wa.width <= m.x + m.width
             and wa.y + wa.height <= m.y + m.height, "the work area of monitor " .. i .. " is inside it")

      -- a snapshot: changing it changes nothing
      m.width = -1
      m.work_area.width = -1
      local again = shell.monitors.get(i)
      truthy(again.width > 0 and again.work_area.width > 0, "monitors.get () is a fresh table")
    end
    eq(primary, 1, "exactly one primary monitor")
    local p = shell.monitors.primary()
    eq(p.primary, true, "primary () is primary")
    eq(p.index, primary_index, "primary () is the primary monitor of list ()")
    eq(shell.monitors.get(0), nil, "get (0)")
    eq(shell.monitors.get(#list + 1), nil, "get (count + 1)")
    raises(function() shell.monitors.get("x") end, nil, "get ('x')")
    raises(function() shell.monitors.get() end, nil, "get ()")
    truthy(shell.monitors.list() ~= shell.monitors.list(), "list () makes new tables")
  end)

  -- -------------------------------------------------------------------
  -- shell.windows (the window the script opens is looked at by steps)
  -- -------------------------------------------------------------------
  case("windows: list, focused, get", function()
    local list = shell.windows.list()
    local focused = shell.windows.focused()

    eq(type(list), "table", "list ()")
    for i, w in ipairs(list) do
      eq(type(w), "userdata", "a Window is a handle")
      eq(is_int(w.id), true, "id")
      truthy(rawequal(shell.windows.get(w.id), w), "get (id) is the handle that list () has")
    end
    truthy(focused == nil or type(focused) == "userdata", "focused () is a window or nil")
    eq(shell.windows.get(-1), nil, "get (-1)")
    eq(shell.windows.get(0), nil, "get (0)")
    eq(shell.windows.get(1 << 50), nil, "get (a big id)")
    raises(function() shell.windows.get("x") end, nil, "get ('x')")
    raises(function() shell.windows.get() end, nil, "get ()")
    raises(function() shell.windows.get(1.5) end, nil, "get (1.5)")
  end)

  -- -------------------------------------------------------------------
  -- shell.apps
  -- -------------------------------------------------------------------
  case("apps: favorites, running, info", function()
    local function check_info(id)
      local info = truthy(shell.apps.info(id), "info (" .. id .. ")")

      eq(type(info), "table", "info")
      eq(info.id, id, "id")
      eq(type(info.name), "string", "name")
      truthy(info.icon_name == nil or type(info.icon_name) == "string", "icon_name")
      truthy(info.description == nil or type(info.description) == "string", "description")
    end

    local favorites = shell.apps.favorites()
    eq(type(favorites), "table", "favorites ()")
    for _, id in ipairs(favorites) do
      eq(type(id), "string", "a favorite")
      check_info(id)
    end
    local running = shell.apps.running()
    eq(type(running), "table", "running ()")
    for _, id in ipairs(running) do
      eq(type(id), "string", "a running app")
      check_info(id)
    end

    eq(shell.apps.info("org.example.NoSuchApp.desktop"), nil, "info of an unknown id")
    local launched = table.pack(shell.apps.launch("org.example.NoSuchApp.desktop"))
    eq(launched.n, 2, "launch () of an unknown id returns nil and a message")
    eq(launched[1], nil, "the first value")
    eq(type(launched[2]), "string", "the message")
    raises(function() shell.apps.launch() end, nil, "launch ()")
    raises(function() shell.apps.launch({}) end, nil, "launch ({})")
    raises(function() shell.apps.info() end, nil, "info ()")
  end)

  -- -------------------------------------------------------------------
  -- The hooks: a listener on each, checked as the events come
  -- -------------------------------------------------------------------
  for _, list in ipairs({ WINDOW_HOOKS, WORKSPACE_HOOKS, { "monitors.changed" } }) do
    for _, name in ipairs(list) do
      kept.conns[#kept.conns + 1] = shell.hooks.on(name, function(p)
        record(name, p)
      end)
    end
  end

  -- -------------------------------------------------------------------
  -- Steps of the script
  -- -------------------------------------------------------------------
  steps{
    -- the window the script opened: its properties, its hooks, the handle
    ["opened"] = function()
      local mapped = find_event("window.mapped")
      local id, w, ws

      truthy(mapped, "window.mapped was emitted for the new window")
      id = mapped.p["window-id"]
      w = truthy(shell.windows.get(id), "windows.get (id)")
      kept.id, kept.w = id, w

      case("a Window has the properties of window.md", function()
        eq(type(w), "userdata", "a handle")
        eq(tostring(w):find("shell.Window", 1, true) ~= nil, true, "tostring")
        eq(w.id, id, "id")
        eq(is_int(w.id), true, "id is an integer")
        eq(type(w.title), "string", "title")
        eq(type(w.wm_class), "string", "wm_class")
        truthy(w.app_id == nil or type(w.app_id) == "string", "app_id is a string or nil")
        truthy(w.workspace == nil or is_int(w.workspace), "workspace is an integer or nil")
        if w.workspace ~= nil then
          truthy(w.workspace >= 1 and w.workspace <= shell.workspaces.count(), "workspace is in 1..count ()")
        end
        eq(is_int(w.monitor), true, "monitor is an integer")
        truthy(w.monitor >= 1 and w.monitor <= #shell.monitors.list(), "monitor is in 1..#monitors.list ()")
        eq(w.minimized, false, "minimized")
        eq(w.maximized, false, "maximized")
        eq(w.fullscreen, false, "fullscreen")
        eq(type(w.focused), "boolean", "focused")
        truthy(w.pid == nil or is_int(w.pid), "pid is an integer or nil")
        eq(w.alive, true, "alive")
        eq(w.no_such_property, nil, "unknown properties are nil")
      end)

      case("a Window is a read-only handle, the same one every time", function()
        for _, key in ipairs({ "id", "title", "wm_class", "app_id", "workspace", "monitor", "minimized",
                               "maximized", "fullscreen", "focused", "pid", "alive" }) do
          raises(function() w[key] = 1 end, nil, "assigning " .. key)
        end
        raises(function() w.no_such_property = 1 end, nil, "assigning an unknown property")
        truthy(rawequal(shell.windows.get(id), w), "windows.get (id) twice")
        local found = false
        for _, x in ipairs(shell.windows.list()) do
          found = found or rawequal(x, w)
        end
        truthy(found, "windows.list () has the window")
        truthy(w == shell.windows.get(id), "==")
      end)

      case("the actions check their arguments", function()
        raises(function() w:move_to_workspace(0) end, nil, "move_to_workspace (0)")
        raises(function() w:move_to_workspace(shell.workspaces.count() + 1) end, nil, "move_to_workspace (count + 1)")
        raises(function() w:move_to_workspace("x") end, nil, "move_to_workspace ('x')")
        raises(function() w:move_to_workspace(1.5) end, nil, "move_to_workspace (1.5)")
        raises(function() w:move_to_workspace() end, nil, "move_to_workspace ()")
        raises(function() w:move_to_monitor(0) end, nil, "move_to_monitor (0)")
        raises(function() w:move_to_monitor(#shell.monitors.list() + 1) end, nil, "move_to_monitor (count + 1)")
        raises(function() w:move_to_monitor("x") end, nil, "move_to_monitor ('x')")
        raises(function() w:no_such_method() end, nil, "an unknown method")
      end)

      case("the window is in the list of its workspace", function()
        if w.workspace ~= nil then
          ws = shell.workspaces.get(w.workspace)
          local found = false
          for _, x in ipairs(ws:windows()) do
            found = found or rawequal(x, w)
          end
          truthy(found, "workspace:windows () has the window")
          truthy(ws.n_windows >= 1, "n_windows")
        end
      end)

      case("window.mapped is the first hook of the window, once", function()
        local first, mapped_count = nil, 0

        for _, ev in ipairs(kept.events) do
          if ev.hook:find("^window%.") and ev.p["window-id"] == id then
            first = first or ev
            if ev.hook == "window.mapped" then
              mapped_count = mapped_count + 1
            end
          end
        end
        eq(first and first.hook, "window.mapped", "the first hook of the window")
        eq(mapped_count, 1, "window.mapped is emitted once")
      end)
    end,

    -- minimize () and the hook, and a minimized window stays listed
    ["minimize"] = function(p, done)
      local w, id = kept.w, kept.id
      local from = #kept.events
      local returned = table.pack(w:minimize())

      case("minimize () returns nothing", function()
        eq(returned.n, 0, "values returned")
      end)
      return wait_for(function() return w.minimized end, "the window to be minimized", function()
        case("window.minimized, and a minimized window is still listed", function()
          truthy(find_event("window.minimized", id, from), "window.minimized was emitted")
          local found = false
          for _, x in ipairs(shell.windows.list()) do
            found = found or rawequal(x, w)
          end
          truthy(found, "a minimized window is in windows.list ()")
          eq(shell.windows.get(id) ~= nil, true, "windows.get () finds it")
          w:minimize()                          -- a no-op when already minimized
        end)
      end, done)
    end,

    -- activate () unminimizes, focuses
    ["activate"] = function(p, done)
      local w, id = kept.w, kept.id
      local from = #kept.events
      local returned = table.pack(w:activate())

      case("activate () returns nothing", function()
        eq(returned.n, 0, "values returned")
      end)
      return wait_for(function() return not w.minimized and w.focused end, "the window to be active", function()
        case("window.unminimized, the focus", function()
          truthy(find_event("window.unminimized", id, from), "window.unminimized was emitted")
          eq(shell.windows.focused() ~= nil and shell.windows.focused().id == id, true, "windows.focused ()")
          eq(w.focused, true, "focused")
        end)
      end, done)
    end,

    -- workspace:activate () and workspace.switched
    ["switch"] = function(p, done)
      local before = shell.workspaces.active()
      local other

      for _, ws in ipairs(shell.workspaces.list()) do
        if not ws.active then
          other = ws
        end
      end
      truthy(other, "a second workspace to switch to")
      local from_index, to_index = before.index, other.index
      local from = #kept.events

      local returned = table.pack(other:activate())
      local second_part = false

      case("activate () of a workspace returns nothing", function()
        eq(returned.n, 0, "values returned")
      end)
      return wait_for(function() return other.active end, "the workspace to be active", function()
        case("workspace.switched", function()
          local ev = truthy(find_event("workspace.switched", nil, from), "workspace.switched was emitted")
          eq(ev.p.from, from_index, "from")
          eq(ev.p.to, to_index, "to")
          eq(before.active, false, "the first workspace is not active any more")
          eq(shell.workspaces.active().index, to_index, "workspaces.active ()")
        end)
        -- and back
        local back = #kept.events
        second_part = true
        before:activate()
        wait_for(function() return before.active end, "the first workspace to be active again", function()
          case("workspace.switched, back to the first", function()
            local ev = truthy(find_event("workspace.switched", nil, back), "workspace.switched was emitted")
            eq(ev.p.from, to_index, "from")
            eq(ev.p.to, from_index, "to")
          end)
        end, done)
      end, function()
        -- the first wait ended: the second one takes over when it started
        if not second_part then
          done()
        end
      end)
    end,

    -- move_to_workspace () and window.workspace-changed (a dynamic workspace
    -- may go away afterwards and shift the numbers: only the event is checked)
    ["move"] = function(p, done)
      local w, id = kept.w, kept.id
      local count = shell.workspaces.count()
      local from_ws = w.workspace

      truthy(count >= 2, "a second workspace to move to")
      truthy(from_ws, "the window is on one workspace")
      local to_ws = from_ws == 1 and 2 or 1
      local from = #kept.events

      local returned = table.pack(w:move_to_workspace(to_ws))
      case("move_to_workspace () returns nothing", function()
        eq(returned.n, 0, "values returned")
      end)
      case("move_to_monitor () to the monitor it is on", function()
        local r = table.pack(w:move_to_monitor(w.monitor))
        eq(r.n, 0, "values returned")
      end)
      return wait_for(function() return find_event("window.workspace-changed", id, from) end,
                      "window.workspace-changed", function()
        case("window.workspace-changed", function()
          local ev = find_event("window.workspace-changed", id, from)

          eq(ev.p.from, from_ws, "from")
          eq(ev.p.to, to_ws, "to")
          truthy(ev.ok, "the payload was consistent with the API")
        end)
      end, done)
    end,

    -- a monitor was added or removed by the script
    ["monitors-changed"] = function(p, done)
      return wait_for(function() return #shell.monitors.list() == p.count end,
                      "the number of monitors to be " .. tostring(p.count), function()
        case("monitors.changed and monitors.list ()", function()
          local list = shell.monitors.list()
          local ev

          for _, e in ipairs(kept.events) do
            if e.hook == "monitors.changed" and e.p.count == p.count then
              ev = e
            end
          end
          truthy(ev, "monitors.changed with count " .. tostring(p.count))
          eq(#list, p.count, "#monitors.list ()")
          eq(shell.monitors.get(p.count) ~= nil, true, "monitors.get (count)")
          eq(shell.monitors.get(p.count + 1), nil, "monitors.get (count + 1)")
          if p.width then
            local found = false

            for _, m in ipairs(list) do
              found = found or (m.width == p.width and m.height == p.height)
            end
            truthy(found, "a monitor of " .. tostring(p.width) .. "x" .. tostring(p.height))
          end
          local primary = 0
          for _, m in ipairs(list) do
            if m.primary then
              primary = primary + 1
            end
          end
          eq(primary, 1, "one primary monitor")
        end)
      end, done)
    end,

    -- close (): polite, then the handle is dead and window.unmapped is the last hook
    ["close"] = function(p, done)
      local w, id = kept.w, kept.id
      local from = #kept.events
      local returned = table.pack(w:close())

      case("close () returns nothing", function()
        eq(returned.n, 0, "values returned")
      end)
      return wait_for(function() return not w.alive end, "the window to be destroyed", function()
        case("window.unmapped is the last hook of the window, the handle is dead", function()
          local last
          local ev, index = find_event("window.unmapped", id, from)

          truthy(ev, "window.unmapped was emitted")
          for i, e in ipairs(kept.events) do
            if e.hook:find("^window%.") and e.p["window-id"] == id then
              last = i
            end
          end
          eq(last, index, "window.unmapped is the last hook of the window")
          eq(shell.windows.get(id), nil, "windows.get (id) of a closed window")
          for _, x in ipairs(shell.windows.list()) do
            truthy(x.id ~= id, "windows.list () does not have the closed window")
          end
          eq(w.alive, false, "alive is false, and reading it does not raise")
          raises(function() return w.title end, "no longer valid", "reading a property of a dead window")
          raises(function() w:minimize() end, "no longer valid", "calling a method of a dead window")
          raises(function() w:close() end, "no longer valid", "close () on a dead window")
          eq(tostring(w):find("shell.Window", 1, true) ~= nil, true, "tostring of a dead handle")
          truthy(w == w, "== on a dead handle")
        end)
      end, done)
    end,
  }

  T.done()
end

-- the listeners are connections owned by the extension; the context disconnects them
function ext.disable()
  case("disable () runs before the objects are destroyed", function()
    eq(#kept.conns > 0 and kept.conns[1]:is_connected(), true, "a listener is connected in disable ()")
  end)
end

return ext
