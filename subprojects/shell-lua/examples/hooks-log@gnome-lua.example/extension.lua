-- hooks-log@gnome-lua.example: an extension that only listens to hooks
-- (docs/api/hooks.md) and writes to the log (domain shell-lua/<uuid>).
--
-- A hook is a named event of the shell with a payload table; a past-tense
-- name (window.focused) is informational, a present participle
-- (overview.showing) comes first and can be cancelled. This extension never
-- cancels. Hooks that this shell version does not declare are skipped, so
-- the same file works on every release (docs/api/README.md, "Versioning").
local shell = require("shell")

-- Informational hooks (after the fact)
local INFO = {
  "shell.started",
  "shell.shutting-down",
  "session.mode-changed",
  "monitors.changed",
  "overview.shown",
  "overview.hidden",
  "window.mapped",
  "window.unmapped",
  "window.focused",
  "window.minimized",
  "window.unminimized",
  "window.title-changed",
  "window.workspace-changed",
  "window.state-changed",
  "workspace.switched",
  "workspace.added",
  "workspace.removed",
  "quick-settings.opened",
  "notification.shown",
  "notification.activated",
  "screenshot.taken",
}

-- Cancellable hooks: `before` runs ahead of the action; a listener that
-- returns nothing lets it happen
local ASKED = {
  "overview.showing",
  "overview.hiding",
  "notification.showing",
}

-- "key=value ..." with the keys in order; a sequence is {a,b}
local function describe(payload)
  local keys = {}

  for key in pairs(payload) do
    keys[#keys + 1] = key
  end
  table.sort(keys)

  local parts = {}
  for _, key in ipairs(keys) do
    local value = payload[key]
    if type(value) == "table" then
      local items = {}
      for _, item in ipairs(value) do
        items[#items + 1] = tostring(item)
      end
      value = "{" .. table.concat(items, ",") .. "}"
    end
    parts[#parts + 1] = key .. "=" .. tostring(value)
  end
  return table.concat(parts, " ")
end

local ext = {}

function ext.enable()
  local declared = {}

  for _, hook in ipairs(shell.hooks.list()) do
    declared[hook.name] = true
  end

  local listening = 0
  for _, name in ipairs(INFO) do
    if declared[name] then
      shell.hooks.on(name, function(payload)
        shell.log.info("%s %s", name, describe(payload))
      end)
      listening = listening + 1
    end
  end

  for _, name in ipairs(ASKED) do
    if declared[name] then
      shell.hooks.before(name, function(payload)
        shell.log.info("%s (about to happen) %s", name, describe(payload))
        -- no return value: do not cancel, do not amend
      end)
      listening = listening + 1
    end
  end

  shell.log.info("enabled, listening to %d hooks", listening)
end

-- disable() is optional: the connections above are owned by the extension and
-- are cut by the shell. It is the place for what the shell does not know of.
function ext.disable()
  shell.log.info("disabled")
end

return ext
