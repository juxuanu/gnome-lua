-- docs/api/hooks.md as data, for the fixture extension (require ("catalogue"))
-- and for the script of the test (dofile). Plain Lua: no dependency on the
-- shell table.
--
-- Each hook: name, cancellable, declared (the shell declares it once the
-- class that owns it is loaded: the rows of the document that are not
-- "reserved"), gated (capability needed to listen), keys = {{key, gvariant
-- type [, optional]}}, sample = a payload with every key, the way the shell
-- sends it.

local function keys(...)
  local result = {}
  for _, list in ipairs({ ... }) do
    for _, key in ipairs(list) do
      result[#result + 1] = key
    end
  end
  return result
end

-- W: window snapshot, N: notification summary (hooks.md, "Payload conventions")
local W = {
  { "window-id", "t" }, { "title", "s" }, { "wm-class", "s" }, { "app-id", "s", true },
  { "workspace", "i", true }, { "monitor", "i" }, { "pid", "u", true },
}
local N = {
  { "notification-id", "u" }, { "source", "s" }, { "urgency", "s" }, { "resident", "b" },
  { "transient", "b" }, { "owner", "s" }, { "app-id", "s", true },
}

local w_sample = {
  ["window-id"] = 4294967297, title = "A window", ["wm-class"] = "Test", ["app-id"] = "test.desktop",
  workspace = 1, monitor = 1, pid = 1234,
}
local n_sample = {
  ["notification-id"] = 7, source = "Test source", urgency = "normal", resident = false,
  transient = true, owner = "", ["app-id"] = "test.desktop",
}

local function with(base, extra)
  local result = {}
  for key, value in pairs(base) do
    result[key] = value
  end
  for key, value in pairs(extra) do
    result[key] = value
  end
  return result
end

return {
  { name = "shell.started", cancellable = false, declared = true, keys = {}, sample = {} },
  { name = "shell.shutting-down", cancellable = false, declared = true, keys = {}, sample = {} },
  { name = "session.mode-changed", cancellable = false, declared = true,
    keys = { { "mode", "s" } }, sample = { mode = "user" } },
  { name = "panel.indicator-added", cancellable = false, declared = true,
    keys = { { "name", "s" }, { "box", "s" }, { "owner", "s" } },
    sample = { name = "test", box = "right", owner = "" } },
  { name = "panel.menu-opened", cancellable = false, declared = true,
    keys = { { "name", "s" }, { "box", "s" } }, sample = { name = "test", box = "right" } },
  { name = "monitors.changed", cancellable = false, declared = true,
    keys = { { "count", "i" }, { "primary", "i" } }, sample = { count = 2, primary = 1 } },
  { name = "overview.showing", cancellable = true, declared = true,
    keys = { { "reason", "s" }, { "target", "s" } }, sample = { reason = "keybinding", target = "window-picker" } },
  { name = "overview.shown", cancellable = false, declared = true,
    keys = { { "target", "s" } }, sample = { target = "app-grid" } },
  { name = "overview.hiding", cancellable = true, declared = true,
    keys = { { "reason", "s" } }, sample = { reason = "escape" } },
  { name = "overview.hidden", cancellable = false, declared = true, keys = {}, sample = {} },
  { name = "window.mapped", cancellable = false, declared = true, keys = keys(W), sample = w_sample },
  { name = "window.unmapped", cancellable = false, declared = true, keys = keys(W), sample = w_sample },
  { name = "window.focused", cancellable = false, declared = true,
    keys = keys(W, { { "previous-window-id", "t", true } }),
    sample = with(w_sample, { ["previous-window-id"] = 4294967298 }) },
  { name = "window.minimized", cancellable = false, declared = true, keys = keys(W), sample = w_sample },
  { name = "window.unminimized", cancellable = false, declared = true, keys = keys(W), sample = w_sample },
  { name = "window.title-changed", cancellable = false, declared = true,
    keys = keys(W, { { "old-title", "s" } }), sample = with(w_sample, { ["old-title"] = "Old" }) },
  { name = "window.workspace-changed", cancellable = false, declared = true,
    keys = keys(W, { { "from", "i", true }, { "to", "i", true } }),
    sample = with(w_sample, { from = 1, to = 2 }) },
  { name = "window.monitor-changed", cancellable = false, declared = true,
    keys = keys(W, { { "from", "i", true }, { "to", "i", true } }),
    sample = with(w_sample, { from = 1, to = 2 }) },
  { name = "window.state-changed", cancellable = false, declared = true,
    keys = keys(W, { { "maximized", "b" }, { "fullscreen", "b" }, { "changed", "as" } }),
    sample = with(w_sample, { maximized = true, fullscreen = false, changed = { "maximized" } }) },
  { name = "workspace.switched", cancellable = false, declared = true,
    keys = { { "from", "i" }, { "to", "i" } }, sample = { from = 1, to = 2 } },
  { name = "workspace.added", cancellable = false, declared = true,
    keys = { { "index", "i" } }, sample = { index = 2 } },
  { name = "workspace.removed", cancellable = false, declared = true,
    keys = { { "index", "i" } }, sample = { index = 2 } },
  { name = "search.query", cancellable = false, declared = true, gated = "search",
    keys = { { "query", "s" }, { "terms", "as" } }, sample = { query = "foo bar", terms = { "foo", "bar" } } },
  { name = "dash.item-added", cancellable = false, declared = true,
    keys = { { "kind", "s" }, { "owner", "s" }, { "index", "i" }, { "app-id", "s", true } },
    sample = { kind = "app", owner = "", index = 3, ["app-id"] = "test.desktop" } },
  { name = "quick-settings.opened", cancellable = false, declared = true, keys = {}, sample = {} },
  { name = "notification.showing", cancellable = true, declared = true, keys = keys(N), sample = n_sample },
  { name = "notification.shown", cancellable = false, declared = true, keys = keys(N), sample = n_sample },
  { name = "notification.activated", cancellable = false, declared = true, keys = keys(N), sample = n_sample },
  { name = "screenshot.taken", cancellable = false, declared = true,
    keys = { { "kind", "s" }, { "clipboard", "b" }, { "file", "s", true } },
    sample = { kind = "area", clipboard = true, file = "/tmp/Screenshot.png" } },
  { name = "osk.shown", cancellable = false, declared = true,
    keys = { { "monitor", "i" } }, sample = { monitor = 1 } },
  { name = "osk.hidden", cancellable = false, declared = true, keys = {}, sample = {} },
}
