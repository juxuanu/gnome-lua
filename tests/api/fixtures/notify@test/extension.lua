-- Fixture of tests/api/notify.lua: every function, field, property and error
-- kind of docs/api/notify.md that an extension can check by itself, and the
-- notification.* hooks of docs/api/hooks.md. enable () runs the synchronous
-- checks; the script of the test asks for the others with steps
-- (tests/api/README.md): the ones the shell has to cause (a notification that
-- expires, that the user closes, a banner that is shown).
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

local function is_type(h, name)
  return tostring(h):find("shell." .. name, 1, true) ~= nil
end

-- One of the hooks of the notifications, as shell.hooks.list () shows it
local function find_hook(name)
  for _, hook in ipairs(shell.hooks.list()) do
    if hook.name == name then
      return hook
    end
  end
end

local UUID = "notify@test"
local NAME = "API conformance: notify"

local ext = {}

-- What lives from enable () to the steps and to disable ()
local keep = {}
-- The hooks seen: the payloads of notification.showing / shown / activated
local seen = { showing = {}, shown = {}, activated = {} }
-- Titles of the sources the hook handler should cancel the banner of
local CANCEL_SOURCE = "Notify test: cancel the banner"

function ext.enable()
  -- The hooks first: the banners of the checks below are seen, and the ones of
  -- the source CANCEL_SOURCE never shown
  shell.hooks.before("notification.showing", function(payload)
    seen.showing[#seen.showing + 1] = payload
    if payload.source == CANCEL_SOURCE then
      return shell.hooks.CANCEL
    end
  end)
  shell.hooks.on("notification.shown", function(payload)
    seen.shown[#seen.shown + 1] = payload
  end)
  shell.hooks.on("notification.activated", function(payload)
    seen.activated[#seen.activated + 1] = payload
  end)

  -- Normal and high urgency would show a banner: this source has none
  local quiet = shell.notify.source { title = CANCEL_SOURCE }
  keep.quiet = quiet

  -- -------------------------------------------------------------------
  -- The module
  -- -------------------------------------------------------------------
  case("shell.notify is a callable table with source ()", function()
    eq(type(shell.notify), "table", "type")
    eq(type(shell.notify.source), "function", "shell.notify.source")
    -- "(like shell.log)": a table with a __call that nobody can change
    eq(getmetatable(shell.notify), false, "getmetatable ()")
  end)

  -- -------------------------------------------------------------------
  -- shell.notify { ... }
  -- -------------------------------------------------------------------
  case("shell.notify with a title only", function()
    local n = shell.notify { title = "Plain", urgency = "low" }

    truthy(is_type(n, "Notification"), "tostring: " .. tostring(n))
    eq(n.alive, true, "alive")
    eq(n.title, "Plain", "title")
    eq(n.body, nil, "body")
    eq(n.urgency, "low", "urgency")
    eq(n.resident, false, "resident")
    eq(n.transient, false, "transient")
    eq(type(n.acknowledged), "boolean", "acknowledged")
    eq(n.nonsense, nil, "an unknown property reads as nil")
    truthy(n == n, "equality")
    keep.plain = n
  end)

  case("the default urgency is normal", function()
    -- (through the source whose banners are cancelled: normal shows a banner)
    local n = quiet:notify { title = "Default urgency" }
    eq(n.urgency, "normal", "urgency")
    local h = quiet:notify { title = "High urgency", urgency = "high" }
    eq(h.urgency, "high", "urgency high")
    n:destroy()
    h:destroy()
  end)

  case("shell.notify with every field", function()
    local activated, dismissed = 0, 0
    local n = shell.notify {
      title = "Full", body = "<b>Body</b>", markup = true, icon_name = "mail-unread-symbolic",
      urgency = "low", resident = true, transient = false,
      actions = {
        { label = "One", on_activate = function() end },
        { label = "Two", on_activate = function() end },
        { label = "Three", on_activate = function() end },
      },
      on_activate = function() activated = activated + 1 end,
      on_dismissed = function() dismissed = dismissed + 1 end,
    }

    eq(n.title, "Full", "title")
    eq(n.body, "<b>Body</b>", "body")
    eq(n.icon_name, "mail-unread-symbolic", "icon_name")
    eq(n.urgency, "low", "urgency")
    eq(n.resident, true, "resident")
    eq(n.transient, false, "transient")
    eq(activated, 0, "on_activate is not called by the post")
    eq(dismissed, 0, "on_dismissed is not called by the post")
    keep.full = n
  end)

  case("shell.notify accepts up to three actions and no action", function()
    local n0 = shell.notify { title = "No actions", urgency = "low", actions = {} }
    local n1 = shell.notify { title = "One action", urgency = "low", actions = { { label = "A", on_activate = function() end } } }
    n0:destroy()
    n1:destroy()
  end)

  case("writing the properties of a notification", function()
    local n = shell.notify { title = "W", urgency = "low" }

    n.title = "W2"
    eq(n.title, "W2", "title")
    n.body = "text"
    eq(n.body, "text", "body")
    n.icon_name = "mail-unread-symbolic"
    eq(n.icon_name, "mail-unread-symbolic", "icon_name")
    for _, urgency in ipairs { "high", "normal", "low" } do
      n.urgency = urgency
      eq(n.urgency, urgency, "urgency")
    end
    n.resident = true
    eq(n.resident, true, "resident")
    n.transient = true
    eq(n.transient, true, "transient")
    n.acknowledged = true
    eq(n.acknowledged, true, "acknowledged")
    n.acknowledged = false
    eq(n.acknowledged, false, "acknowledged back")

    raises(function() n.urgency = "critical" end, "urgency", "urgency = critical")
    raises(function() n.urgency = "urgent" end, "urgency", "an unknown urgency")
    raises(function() n.urgency = 2 end, "urgency", "a number as urgency")
    raises(function() n.alive = false end, "alive", "assigning alive")
    raises(function() n.nonsense = 1 end, "nonsense", "assigning an unknown property")
    raises(function() n.title = 5 end, "title", "a number as title")
    raises(function() n.resident = "yes" end, "resident", "a string as resident")
    eq(n.urgency, "low", "urgency unchanged by the failed writes")
    n:destroy()
  end)

  case("n:show () asks for the banner again and is harmless", function()
    local n = shell.notify { title = "Show", urgency = "low" }
    n:show()
    n:show()
    eq(n.alive, true, "alive after show ()")
    n:destroy()
  end)

  case("shell.notify raises on a misuse, naming the field", function()
    raises(function() shell.notify { title = "x", bogus = 1 } end, "bogus", "an unknown field")
    raises(function() shell.notify {} end, "title", "no title")
    raises(function() shell.notify { title = 5 } end, "title", "a number as title")
    raises(function() shell.notify { title = "x", body = 5 } end, "body", "a number as body")
    raises(function() shell.notify { title = "x", markup = "yes" } end, "markup", "a string as markup")
    raises(function() shell.notify { title = "x", icon_name = "a", icon_file = "b.svg" } end,
      { "icon_name", "icon_file" }, "two icons")
    raises(function() shell.notify { title = "x", icon_file = "missing.svg" } end, "icon_file", "a missing icon file")
    raises(function() shell.notify { title = "x", icon_file = "/usr/share/pixmaps/x.svg" } end, "icon_file",
      "an absolute icon file")
    raises(function() shell.notify { title = "x", urgency = "critical" } end, "urgency", "urgency critical")
    raises(function() shell.notify { title = "x", urgency = "urgent" } end, "urgency", "an unknown urgency")
    raises(function() shell.notify { title = "x", urgency = 1 } end, "urgency", "a number as urgency")
    raises(function() shell.notify { title = "x", resident = "yes" } end, "resident", "a string as resident")
    raises(function() shell.notify { title = "x", transient = 1 } end, "transient", "a number as transient")
    raises(function() shell.notify { title = "x", on_activate = "f" } end, "on_activate", "a string as on_activate")
    raises(function() shell.notify { title = "x", on_dismissed = 5 } end, "on_dismissed", "a number as on_dismissed")
    raises(function() shell.notify { title = "x", actions = "a" } end, "actions", "a string as actions")
    raises(function() shell.notify { title = "x", actions = { "a" } } end, "actions", "a string as an action")
    raises(function() shell.notify { title = "x", actions = { { on_activate = function() end } } } end, "label",
      "an action without label")
    raises(function() shell.notify { title = "x", actions = { { label = "a" } } } end, "on_activate",
      "an action without on_activate")
    raises(function() shell.notify { title = "x", actions = { { label = 1, on_activate = function() end } } } end,
      "label", "a number as label")
    raises(function() shell.notify { title = "x", actions = { { label = "a", on_activate = 1 } } } end,
      "on_activate", "a number as the callback of an action")
    raises(function() shell.notify { title = "x", actions = { { label = "a", on_activate = function() end, bogus = 1 } } } end,
      "bogus", "an unknown field of an action")
    -- "More than 3 actions (raises)"
    local four = {}
    for i = 1, 4 do
      four[i] = { label = "A" .. i, on_activate = function() end }
    end
    raises(function() shell.notify { title = "x", actions = four } end, "actions", "four actions")
    raises(function() shell.notify("x") end, nil, "a string instead of the table")
    raises(function() shell.notify() end, nil, "no argument")
    -- the fields of the sources are not the ones of the notifications
    raises(function() shell.notify { title = "x", count = 1 } end, "count", "count on a notification")
  end)

  -- -------------------------------------------------------------------
  -- Events of a notification: on_activate and on_dismissed as methods
  -- -------------------------------------------------------------------
  case("on_activate and on_dismissed as methods return connections", function()
    local n = shell.notify { title = "Methods", urgency = "low" }
    local a = n:on_activate(function() end)
    local d = n:on_dismissed(function() end)

    eq(type(a), "userdata", "a connection is a handle")
    eq(type(a.disconnect), "function", "Connection:disconnect")
    eq(type(d.disconnect), "function", "Connection:disconnect")
    raises(function() n:on_activate(5) end, nil, "a number as callback")
    raises(function() n:on_dismissed("f") end, nil, "a string as callback")
    a:disconnect()
    d:disconnect()
    n:destroy()
  end)

  -- -------------------------------------------------------------------
  -- destroy (README: Handles; notify.md: n:destroy () suppresses on_dismissed)
  -- -------------------------------------------------------------------
  case("n:destroy () withdraws the notification without on_dismissed", function()
    local calls = 0
    local n = shell.notify { title = "Withdrawn", urgency = "low", on_dismissed = function() calls = calls + 1 end }
    local m = shell.notify { title = "Withdrawn 2", urgency = "low" }
    m:on_dismissed(function() calls = calls + 1 end)

    n:destroy()
    m:destroy()
    eq(n.alive, false, "alive after destroy")
    eq(m.alive, false, "alive after destroy, callback set by a method")
    eq(calls, 0, "on_dismissed called by destroy ()")
    n:destroy()
    n:destroy()
    raises(function() return n.title end, "no longer valid", "reading a dead handle")
    raises(function() n.title = "x" end, "no longer valid", "writing a dead handle")
    raises(function() n:show() end, "no longer valid", "show () of a dead handle")
    raises(function() n:on_dismissed(function() end) end, "no longer valid", "a method of a dead handle")
    truthy(tostring(n):find("dead", 1, true), "tostring of a dead handle: " .. tostring(n))
    truthy(n == n, "a dead handle equals itself")
  end)

  -- -------------------------------------------------------------------
  -- shell.notify.source { ... }
  -- -------------------------------------------------------------------
  case("shell.notify.source", function()
    local src = shell.notify.source { title = "Mail checker", icon_name = "mail-unread-symbolic" }

    truthy(is_type(src, "NotificationSource"), "tostring: " .. tostring(src))
    eq(src.alive, true, "alive")
    eq(src.title, "Mail checker", "title")
    eq(src.icon_name, "mail-unread-symbolic", "icon_name")
    eq(src.count, 0, "count")
    eq(type(src.notify), "function", "notify")
    eq(type(src.clear), "function", "clear")
    eq(type(src.destroy), "function", "destroy")

    src.title = "Mail checker 2"
    eq(src.title, "Mail checker 2", "title written")
    src.icon_name = "dialog-information-symbolic"
    eq(src.icon_name, "dialog-information-symbolic", "icon_name written")
    raises(function() src.count = 1 end, "count", "assigning count")
    raises(function() src.alive = false end, "alive", "assigning alive")
    raises(function() src.nonsense = 1 end, "nonsense", "assigning an unknown property")
    raises(function() src.title = 5 end, "title", "a number as title")
    keep.source = src
  end)

  case("shell.notify.source raises on a misuse", function()
    raises(function() shell.notify.source {} end, "title", "no title")
    raises(function() shell.notify.source { title = 5 } end, "title", "a number as title")
    raises(function() shell.notify.source { title = "x", bogus = 1 } end, "bogus", "an unknown field")
    raises(function() shell.notify.source { title = "x", icon_name = "a", icon_file = "b.svg" } end,
      { "icon_name", "icon_file" }, "two icons")
    raises(function() shell.notify.source { title = "x", icon_file = "missing.svg" } end, "icon_file",
      "a missing icon file")
    raises(function() shell.notify.source { title = "x", body = "b" } end, "body", "a field of the notifications")
    raises(function() shell.notify.source("x") end, nil, "a string instead of the table")
  end)

  case("src:notify { ... } and the count of the source", function()
    local src = shell.notify.source { title = "Counting" }
    local a = src:notify { title = "a", urgency = "low" }
    eq(src.count, 1, "count after one notification")
    local b = src:notify { title = "b", urgency = "low" }
    eq(src.count, 2, "count after two")
    truthy(is_type(a, "Notification"), "the result is a shell.Notification: " .. tostring(a))

    a:destroy()
    eq(src.count, 1, "count after destroy ()")
    eq(a.alive, false, "alive")
    b:destroy()
    eq(src.count, 0, "count when the source is empty")

    -- "Persistent": the handle stays valid while the source has no notification
    eq(src.alive, true, "the source stays valid with zero notifications")
    local c = src:notify { title = "c", urgency = "low" }
    eq(src.count, 1, "the source is registered again by the next notify")
    eq(c.alive, true, "the new notification")
    src:destroy()
    eq(src.alive, false, "the source after destroy ()")
    eq(c.alive, false, "its notification dies with it")

    raises(function() src:notify { title = "dead", urgency = "low" } end, "no longer valid", "notify () of a dead source")
    raises(function() return src.count end, "no longer valid", "reading a dead source")
    src:destroy()
  end)

  case("src:notify raises on a misuse, like shell.notify", function()
    local src = shell.notify.source { title = "Misuse" }

    raises(function() src:notify { title = "x", bogus = 1 } end, "bogus", "an unknown field")
    raises(function() src:notify {} end, "title", "no title")
    raises(function() src:notify { title = "x", urgency = "critical" } end, "urgency", "urgency critical")
    raises(function() src:notify("x") end, nil, "a string instead of the table")
    eq(src.count, 0, "nothing was posted by the failed calls")
    src:destroy()
  end)

  case("src:clear () destroys the non-resident notifications without on_dismissed", function()
    local src = shell.notify.source { title = "Clearing" }
    local calls = 0
    local function count()
      calls = calls + 1
    end
    local a = src:notify { title = "a", urgency = "low", on_dismissed = count }
    local b = src:notify { title = "b", urgency = "low", on_dismissed = count }
    local r = src:notify { title = "resident", urgency = "low", resident = true, on_dismissed = count }

    eq(src.count, 3, "count")
    src:clear()
    eq(a.alive, false, "a non-resident notification")
    eq(b.alive, false, "another one")
    eq(r.alive, true, "a resident notification stays")
    eq(src.count, 1, "count after clear ()")
    eq(calls, 0, "on_dismissed called by clear ()")

    -- clear () of an empty source is fine
    r:destroy()
    src:clear()
    eq(src.alive, true, "the source after clear ()")
    src:destroy()
  end)

  case("src:destroy () destroys the notifications without on_dismissed", function()
    local src = shell.notify.source { title = "Destroying" }
    local calls = 0
    local a = src:notify { title = "a", urgency = "low", on_dismissed = function() calls = calls + 1 end }
    local r = src:notify { title = "resident", urgency = "low", resident = true,
      on_dismissed = function() calls = calls + 1 end }

    src:destroy()
    eq(src.alive, false, "the source")
    eq(a.alive, false, "a notification")
    eq(r.alive, false, "a resident notification")
    eq(calls, 0, "on_dismissed called by destroy ()")
  end)

  case("a source keeps at most ten notifications: the oldest expires", function()
    local src = shell.notify.source { title = "Overflow" }
    local reasons = {}
    local first

    for i = 1, 11 do
      local n = src:notify {
        title = "n" .. i, urgency = "low",
        on_dismissed = function(self, reason)
          reasons[#reasons + 1] = { i, reason, self.alive }
        end,
      }
      if i == 1 then
        first = n
      end
    end

    eq(src.count, 10, "count after eleven notifications")
    eq(first.alive, false, "the oldest")
    eq(#reasons, 1, "calls of on_dismissed")
    eq(reasons[1][1], 1, "the notification that expired")
    eq(reasons[1][2], "expired", "reason")
    eq(reasons[1][3], true, "the handle is valid while on_dismissed runs")
    src:destroy()
    eq(#reasons, 1, "destroy () of the source called on_dismissed")
  end)

  case("a callback may destroy its own notification (README rule 8)", function()
    local src = shell.notify.source { title = "Rule 8" }
    local seen_alive
    local n
    n = src:notify {
      title = "r8", urgency = "low",
      on_dismissed = function(self)
        self:destroy()
        seen_alive = self.alive
      end,
    }
    -- it is dismissed by a push-out of ten newer ones
    for i = 1, 10 do
      src:notify { title = "newer" .. i, urgency = "low" }
    end
    eq(n.alive, false, "the notification")
    eq(seen_alive, true, "the handle stays valid until the callback returns")
    src:destroy()
  end)

  -- -------------------------------------------------------------------
  -- Hooks (hooks.md)
  -- -------------------------------------------------------------------
  case("the hooks notification.showing, shown and activated are declared", function()
    local showing = find_hook("notification.showing")
    local shown = find_hook("notification.shown")
    local activated = find_hook("notification.activated")

    truthy(showing, "notification.showing in shell.hooks.list ()")
    truthy(shown, "notification.shown in shell.hooks.list ()")
    truthy(activated, "notification.activated in shell.hooks.list ()")
    eq(showing.cancellable, true, "notification.showing is cancellable")
    eq(shown.cancellable, false, "notification.shown is not cancellable")
    eq(activated.cancellable, false, "notification.activated is not cancellable")
    raises(function() shell.hooks.before("notification.shown", function() end) end, nil,
      "before () on a hook that cannot be cancelled")
  end)

  -- -------------------------------------------------------------------
  -- Steps of the script
  -- -------------------------------------------------------------------
  local posted = {}
  local reasons = {}

  steps {
    -- Notifications the script then destroys the way the shell does: one that
    -- expired, one that the user closed, one the shell closed with its source
    ["post-for-shell"] = function()
      local src = shell.notify.source { title = "Closed by the shell" }

      for _, name in ipairs { "Shell1", "Shell2", "Shell3" } do
        posted[name] = src:notify {
          title = name, urgency = "low",
          on_dismissed = function(self, reason)
            reasons[name] = reasons[name] or {}
            table.insert(reasons[name], { reason = reason, alive = self.alive })
          end,
        }
      end
      keep.shell_source = src
      eq(src.count, 3, "count")
    end,

    ["check-shell-reasons"] = function(p)
      local want = { Shell1 = "expired", Shell2 = "dismissed", Shell3 = "source-closed" }

      for name, reason in pairs(want) do
        local got = reasons[name]
        truthy(got, name .. " reported its dismissal")
        eq(#got, 1, name .. ": calls of on_dismissed")
        eq(got[1].reason, reason, name .. ": reason")
        eq(got[1].alive, true, name .. ": the handle is valid while on_dismissed runs")
        eq(posted[name].alive, false, name .. ": the handle after the dismissal")
      end
      eq(keep.shell_source.count, 0, "count after the dismissals")
      eq(keep.shell_source.alive, true, "the source stays valid when the shell dropped it")

      -- and it can be used again
      local n = keep.shell_source:notify { title = "Again", urgency = "low" }
      eq(keep.shell_source.count, 1, "count after the next notify")
      n:destroy()
    end,

    -- A notification the script activates as the user would
    ["post-for-activation"] = function()
      local calls = { activate = 0, dismissed = {}, action = 0 }

      keep.activation = calls
      keep.activated = shell.notify {
        title = "Activate me", urgency = "low",
        actions = { { label = "Act", on_activate = function(self)
          calls.action = calls.action + 1
          calls.action_arg = self
        end } },
        on_activate = function(self)
          calls.activate = calls.activate + 1
          calls.activate_arg = self
        end,
        on_dismissed = function(_, reason)
          table.insert(calls.dismissed, reason)
        end,
      }
      keep.resident = shell.notify {
        title = "Resident", urgency = "low", resident = true,
        on_activate = function()
          calls.resident_activate = (calls.resident_activate or 0) + 1
        end,
        on_dismissed = function(_, reason)
          calls.resident_dismissed = reason
        end,
      }
    end,

    ["check-activation"] = function()
      local calls = keep.activation

      eq(calls.activate, 1, "on_activate called once")
      truthy(calls.activate_arg == keep.activated, "on_activate gets the notification first")
      -- "It therefore also follows on_activate for non-resident notifications"
      eq(#calls.dismissed, 1, "on_dismissed after on_activate")
      eq(calls.dismissed[1], "dismissed", "reason after on_activate")
      eq(keep.activated.alive, false, "a non-resident notification is destroyed after activation")

      eq(calls.resident_activate, 1, "on_activate of a resident notification")
      eq(calls.resident_dismissed, nil, "a resident notification stays after activation")
      eq(keep.resident.alive, true, "the resident notification is alive")
      keep.resident:destroy()
    end,

    ["check-activation-hook"] = function()
      truthy(#seen.activated >= 1, "notification.activated ran")
      local p = seen.activated[#seen.activated]
      eq(math.type(p["notification-id"]), "integer", "notification-id")
      eq(p.owner, UUID, "owner")
      eq(p.title, nil, "the payload never carries the title")
      eq(p.body, nil, "the payload never carries the body")
    end,

    -- A transient notification with a banner expires by itself
    ["transient"] = function(p, done)
      local reason, n = nil, nil
      local ticks = 0
      local shown_before = #seen.shown

      n = shell.notify {
        title = "Transient", transient = true,
        on_dismissed = function(self, r)
          reason = r
        end,
      }

      shell.timer.every { ms = 250, callback = function()
        ticks = ticks + 1
        if reason ~= nil then
          case("a transient notification expires after its banner", function()
            eq(reason, "expired", "reason")
            eq(n.alive, false, "the handle")
            truthy(#seen.shown > shown_before, "its banner was shown (notification.shown)")
          end)
          done()
          return false
        end
        if ticks > 80 then
          T.report("a transient notification expires after its banner", false,
                   "no on_dismissed in 20 s; banners shown meanwhile: " .. (#seen.shown - shown_before))
          done()
          return false
        end
        return true
      end }

      return "later"
    end,

    -- A banner is shown: payloads of the hooks
    ["banner"] = function(p, done)
      local ticks = 0
      local shown_before = #seen.shown
      local showing_before = #seen.showing
      local src = shell.notify.source { title = "Banner source" }
      local n = src:notify {
        title = "Banner", body = "Never in a hook", urgency = "high", transient = true, resident = false,
      }

      keep.banner_source = src
      shell.timer.every { ms = 250, callback = function()
        ticks = ticks + 1
        local mine
        for i = shown_before + 1, #seen.shown do
          if seen.shown[i].source == "Banner source" then
            mine = seen.shown[i]
          end
        end

        if mine ~= nil then
          case("the payload of notification.shown", function()
            eq(math.type(mine["notification-id"]), "integer", "notification-id")
            eq(mine.source, "Banner source", "source")
            eq(mine.urgency, "high", "urgency")
            eq(mine.resident, false, "resident")
            eq(mine.transient, true, "transient")
            eq(mine.owner, UUID, "owner")
            eq(mine["app-id"], nil, "app-id is omitted for an extension")
            eq(mine.title, nil, "the payload never carries the title")
            eq(mine.body, nil, "the payload never carries the body")
          end)
          case("the payload of notification.showing", function()
            local showing
            for i = showing_before + 1, #seen.showing do
              if seen.showing[i].source == "Banner source" then
                showing = seen.showing[i]
              end
            end
            truthy(showing, "notification.showing ran before the banner")
            eq(showing["notification-id"], mine["notification-id"], "the same notification")
            eq(showing.owner, UUID, "owner")
            eq(showing.title, nil, "no title")
          end)
          src:destroy()
          done()
          return false
        end

        if ticks > 80 then
          T.report("a banner is shown for a notification of normal urgency", false,
                   "notification.shown did not run in 20 s")
          src:destroy()
          done()
          return false
        end
        return true
      end }

      return "later"
    end,

    -- Cancelling notification.showing: no banner, the entry stays in the list
    ["cancel-banner"] = function(p, done)
      local shown_before = #seen.shown
      local showing_before = #seen.showing
      local src = shell.notify.source { title = CANCEL_SOURCE }
      local n = src:notify { title = "Cancelled", urgency = "high" }

      shell.timer.after { ms = 1500, callback = function()
        case("cancelling notification.showing shows no banner", function()
          local asked = false
          for i = showing_before + 1, #seen.showing do
            if seen.showing[i].source == CANCEL_SOURCE then
              asked = true
            end
          end
          truthy(asked, "notification.showing ran for the notification")
          for i = shown_before + 1, #seen.shown do
            if seen.shown[i].source == CANCEL_SOURCE then
              error("notification.shown ran for a notification whose banner was cancelled")
            end
          end
          eq(n.alive, true, "the notification stays in the list")
          eq(src.count, 1, "count")
        end)
        src:destroy()
        done()
      end }

      return "later"
    end,

    -- Low urgency: no banner at all, no notification.showing either
    ["low-urgency"] = function(p, done)
      local shown_before = #seen.shown
      local showing_before = #seen.showing
      local src = shell.notify.source { title = "Low urgency source" }
      src:notify { title = "Quiet", urgency = "low" }

      shell.timer.after { ms = 1500, callback = function()
        case("a low urgency notification never shows a banner", function()
          for i = showing_before + 1, #seen.showing do
            if seen.showing[i].source == "Low urgency source" then
              error("notification.showing ran for a low urgency notification")
            end
          end
          for i = shown_before + 1, #seen.shown do
            if seen.shown[i].source == "Low urgency source" then
              error("notification.shown ran for a low urgency notification")
            end
          end
          eq(src.count, 1, "the entry is in the list")
        end)
        src:destroy()
        done()
      end }

      return "later"
    end,

    -- The default source: named after the extension, owner in the hooks
    ["default-source"] = function(p, done)
      local shown_before = #seen.shown
      local n = shell.notify { title = "Default source", urgency = "high", transient = true }

      local ticks = 0
      shell.timer.every { ms = 250, callback = function()
        ticks = ticks + 1
        for i = shown_before + 1, #seen.shown do
          local shown = seen.shown[i]
          if shown.source == NAME then
            case("the default source is named after the extension", function()
              eq(shown.owner, UUID, "owner")
              eq(shown.source, NAME, "source")
            end)
            n:destroy()
            done()
            return false
          end
        end
        if ticks > 80 then
          T.report("the default source is named after the extension", false,
                   "no banner of the default source in 20 s")
          n:destroy()
          done()
          return false
        end
        return true
      end }

      return "later"
    end,
  }

  T.done()
end

-- Defining disable () is optional (README): this one only checks what it can
function ext.disable()
  case("disable () runs before the objects are destroyed", function()
    eq(keep.plain.alive, true, "notification")
    eq(keep.full.alive, true, "notification with every field")
    eq(keep.source.alive, true, "source")
    eq(keep.plain.title, "Plain", "a handle can be read in disable ()")
  end)
end

return ext
