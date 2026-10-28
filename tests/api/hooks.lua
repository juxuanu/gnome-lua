-- Conformance test of docs/api/hooks.md and of shell.hooks in docs/api/core.md
-- (see tests/api/README.md). The fixture tests/api/fixtures/hooks@test listens
-- to every hook of the catalogue and checks each payload; this script makes
-- the shell emit them (the overview, the message tray and the top bar are
-- real, the other hooks are emitted by shell.hooks.emit_*) and asks the
-- fixture for its verdict with steps.

local script_dir = (os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or "."
local harness = dofile (script_dir .. "/harness.lua")
local catalogue = dofile (script_dir .. "/fixtures/hooks@test/catalogue.lua")

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

test.wait_startup ()

local declared = {}
for _, entry in ipairs (shell.hooks.list ()) do
  declared[entry.name] = entry
end

local overview = main.overview

local function settle (c, target)
  c.wait_until (function ()
    return overview.visible_target == target and not overview.animation_in_progress
  end, string.format ("the overview to be %s", target and "shown" or "hidden"))
  test.wait_leisure ()
end

local function quiet ()
  test.sleep (500)
  test.wait_leisure ()
end

local function expect_events (c, prefix, list)
  c.step ("expect-events", { prefix = prefix, list = list })
end

local function arm (c, hook, mode, extra)
  local p = { hook = hook, mode = mode }

  for key, value in pairs (extra or {}) do
    p[key] = value
  end
  c.step ("arm", p)
end

-- The overview: the "before" and "after" phases, cancel and amend
local function overview_scenarios (c)
  local SHOWING, HIDING = "overview.showing", "overview.hiding"
  local SHOW = { "overview.showing:before", "overview.showing:after", "overview.shown:after" }
  local HIDE = { "overview.hiding:before", "overview.hiding:after", "overview.hidden:after" }
  local BOTH = {}

  table.move (SHOW, 1, #SHOW, 1, BOTH)
  table.move (HIDE, 1, #HIDE, #BOTH + 1, BOTH)

  settle (c, false)

  -- shown and hidden: the phases in order, the payloads
  c.step ("reset")
  overview:show_for_reason ("keybinding", "window-picker")
  settle (c, true)
  expect_events (c, "overview.", SHOW)
  c.step ("expect-last", { event = "overview.showing:before", key = "reason", value = "keybinding" })
  c.step ("expect-last", { event = "overview.showing:before", key = "target", value = "window-picker" })
  c.step ("expect-last", { event = "overview.showing:after", key = "reason", value = "keybinding" })
  c.step ("expect-last", { event = "overview.shown:after", key = "target", value = "window-picker" })
  overview:hide_for_reason ("escape")
  settle (c, false)
  expect_events (c, "overview.", BOTH)
  c.step ("expect-last", { event = "overview.hiding:before", key = "reason", value = "escape" })
  c.step ("expect-last", { event = "overview.hiding:after", key = "reason", value = "escape" })

  -- the app grid
  c.step ("reset")
  overview:show_for_reason ("activities-button", "app-grid")
  settle (c, true)
  c.step ("expect-last", { event = "overview.shown:after", key = "target", value = "app-grid" })
  overview:hide_for_reason ("other")
  settle (c, false)

  -- CANCEL of overview.showing: the overview stays hidden, nothing follows
  arm (c, SHOWING, "cancel")
  c.step ("reset")
  overview:show_for_reason ("keybinding", "window-picker")
  quiet ()
  if overview.visible_target then
    fail ("a cancelled overview.showing did not keep the overview hidden")
  end
  expect_events (c, "overview.", { "overview.showing:before" })
  c.step ("expect-calls", { hook = SHOWING, a = 1, b = 0 })

  -- ... except for the reason dbus
  c.step ("reset")
  overview:show_for_reason ("dbus", "window-picker")
  settle (c, true)
  expect_events (c, "overview.", SHOW)
  overview:hide_for_reason ("escape")
  settle (c, false)
  arm (c, SHOWING, "none")

  -- an amendment of the target
  local function amended (amend, want, what)
    arm (c, SHOWING, "amend", { amend = amend })
    c.step ("reset")
    overview:show_for_reason ("hot-corner", "window-picker")
    settle (c, true)
    c.step ("expect-last", { event = "overview.shown:after", key = "target", value = want })
    overview:hide_for_reason ("escape")
    settle (c, false)
    test.print ("amendment " .. what .. ": ok")
  end

  amended ({ target = "app-grid" }, "app-grid", "of the target")
  amended ({ target = 5 }, "window-picker", "of the wrong type is dropped")
  amended ({ bogus = "x" }, "window-picker", "of an unknown key is ignored")
  amended ({ bogus = "x", target = "app-grid" }, "app-grid", "with an unknown key")
  arm (c, SHOWING, "none")

  -- CANCEL of overview.hiding is honoured for some reasons only
  for _, reason in ipairs ({ "hot-corner", "activities-button", "overlay-key", "keybinding", "gesture" }) do
    overview:show_for_reason ("other", "window-picker")
    settle (c, true)
    arm (c, HIDING, "cancel")
    c.step ("reset")
    overview:hide_for_reason (reason)
    quiet ()
    if not overview.visible_target then
      fail ("overview.hiding cancelled for the reason %s did not keep the overview shown", reason)
    end
    expect_events (c, "overview.", { "overview.hiding:before" })
    arm (c, HIDING, "none")
    overview:hide_for_reason ("other")
    settle (c, false)
  end

  for _, reason in ipairs ({ "escape", "window-activated", "app-launched", "search", "session", "dbus", "other" }) do
    overview:show_for_reason ("other", "window-picker")
    settle (c, true)
    arm (c, HIDING, "cancel")
    c.step ("reset")
    overview:hide_for_reason (reason)
    settle (c, false)
    expect_events (c, "overview.", HIDE)
    arm (c, HIDING, "none")
  end
end

-- notification.showing (cancellable), notification.shown
local function notification_scenarios (c)
  local source = main.message_tray:get_system_source ()

  c.step ("reset")
  local first = test.new ("ShellNotification", { source = source, title = "API test 1" })
  source:add_notification (first)
  -- the banner may not show in this session (the message tray can be busy):
  -- the hook notification.showing is certain, notification.shown follows the banner
  local banner = pcall (harness.wait_until, function ()
    return test.extension_report (c.uuid).done["event:notification.shown:after:1"]
  end, "the banner of the notification", 5000)
  if banner then
    expect_events (c, "notification.", { "notification.showing:before", "notification.shown:after" })
  else
    test.print ("the banner was not shown in this session")
    expect_events (c, "notification.", { "notification.showing:before" })
  end
  c.step ("expect-last", { event = "notification.showing:before", key = "urgency", value = "normal" })
  c.step ("expect-last", { event = "notification.showing:before", key = "owner", value = "" })
  first:destroy ()
  quiet ()

  -- CANCEL: no banner (the notification is not shown)
  arm (c, "notification.showing", "cancel")
  c.step ("reset")
  local second = test.new ("ShellNotification", { source = source, title = "API test 2" })
  source:add_notification (second)
  test.sleep (1500)
  test.wait_leisure ()
  expect_events (c, "notification.", { "notification.showing:before" })
  c.step ("expect-calls", { hook = "notification.showing", a = 1, b = 0 })
  arm (c, "notification.showing", "none")
  second:destroy ()
  quiet ()
end

-- panel.menu-opened
local function panel_scenarios (c)
  local indicator = main.panel:get_status_area_indicator ("quickSettings")

  c.step ("reset")
  indicator.menu:open ()
  test.sleep (300)
  test.wait_leisure ()
  expect_events (c, "panel.", { "panel.menu-opened:after" })
  c.step ("expect-last", { event = "panel.menu-opened:after", key = "name", value = "quickSettings" })
  c.step ("expect-last", { event = "panel.menu-opened:after", key = "box", value = "right" })
  indicator.menu:close ()
  test.sleep (300)
  test.wait_leisure ()
end

-- The bridge of every declared hook, and the cancel/amend protocol of the
-- before phase, with the hooks emitted by this script
local function synthetic_scenarios (c)
  local SHOWING = "overview.showing"
  local sample = { reason = "keybinding", target = "window-picker" }

  -- the types of a payload
  c.step ("payload-types", {
    n = 5, big = 1 << 40, neg = -3, f = 1.5, b = true, s = "str\u{e9}",
    list = { "a", "b" }, map = { x = 1, y = "z" }, empty = {},
  })

  -- every declared hook gets its sample, in both phases when it has two
  c.step ("reset")
  for _, hook in ipairs (catalogue) do
    if declared[hook.name] and not hook.gated then
      if hook.cancellable then
        local cancelled = shell.hooks.emit_before (hook.name, hook.sample)
        if cancelled then
          fail ("%s was cancelled although nothing cancels it", hook.name)
        end
      end
      shell.hooks.emit_after (hook.name, hook.sample)
    end
  end
  c.step ("samples")

  -- no amendment, no cancel
  local function emit ()
    return shell.hooks.emit_before (SHOWING, sample)
  end

  c.step ("reset")
  local cancelled, amend = emit ()
  if cancelled or (amend ~= nil and next (amend) ~= nil) then
    fail ("unarmed listeners cancelled or amended")
  end
  c.step ("expect-calls", { hook = SHOWING, a = 1, b = 1 })

  -- the first CANCEL stops the before phase
  arm (c, SHOWING, "cancel")
  c.step ("reset")
  cancelled = emit ()
  if cancelled ~= true then
    fail ("CANCEL did not cancel")
  end
  c.step ("expect-calls", { hook = SHOWING, a = 1, b = 0 })

  -- a table is an amendment
  arm (c, SHOWING, "amend", { amend = { target = "app-grid" } })
  c.step ("reset")
  cancelled, amend = emit ()
  if cancelled or amend == nil or amend.target ~= "app-grid" then
    fail ("the amendment {target = 'app-grid'} was not returned")
  end
  c.step ("expect-calls", { hook = SHOWING, a = 1, b = 1 })

  -- anything else a listener returns is ignored
  for _, value in ipairs ({ "text", 42, true, false }) do
    arm (c, SHOWING, "junk", { value = value })
    cancelled, amend = emit ()
    if cancelled or (amend ~= nil and next (amend) ~= nil) then
      fail ("a listener that returns %s cancelled or amended", tostring (value))
    end
  end

  -- a listener that raises does not cancel; the others still run
  arm (c, SHOWING, "error")
  c.step ("reset")
  local log = c.capture_log (function ()
    cancelled = emit ()
  end)
  if cancelled then
    fail ("a listener that raised cancelled")
  end
  if #log ~= 1 or log[1].level ~= "warning" or not log[1].message:find ("on purpose", 1, true) then
    fail ("the error of the listener should be logged once as a warning (%d messages)", #log)
  end
  c.step ("expect-calls", { hook = SHOWING, a = 1, b = 1 })
  arm (c, SHOWING, "none")

  -- a connection that is disconnected gets nothing more
  shell.hooks.emit_after ("test.step", { step = "ping" })
  c.step ("expect-pings", { n = 1 })
  c.step ("disconnect")
  shell.hooks.emit_after ("test.step", { step = "ping" })
  c.step ("expect-pings", { n = 1 })

  arm (c, SHOWING, "cancel")
  c.step ("reset")
  c.step ("disconnect-before", { hook = SHOWING })
  cancelled = emit ()
  if cancelled then
    fail ("a disconnected before listener cancelled")
  end
  c.step ("expect-calls", { hook = SHOWING, a = 0, b = 1 })
end

-- Everything that needs the real overview, message tray and top bar
local function real_scenarios (c)
  -- the real emitters first: the counts of the events are not disturbed
  if c.cycle == 1 then
    overview_scenarios (c)
  else
    -- once is enough for the long loops; the second enable must still work
    settle (c, false)
    c.step ("reset")
    overview:show_for_reason ("keybinding", "window-picker")
    settle (c, true)
    overview:hide_for_reason ("escape")
    settle (c, false)
    expect_events (c, "overview.", {
      "overview.showing:before", "overview.showing:after", "overview.shown:after",
      "overview.hiding:before", "overview.hiding:after", "overview.hidden:after",
    })
  end
  notification_scenarios (c)
  panel_scenarios (c)
end

harness.run {
  uuid = "hooks@test",
  actors = false,       -- the overview, a notification and a menu make actors
  expect_errors = 1,    -- the listener that raises on purpose
  drive = function (c)
    real_scenarios (c)
    synthetic_scenarios (c)
  end,
}

-- The capability "search" gates search.query
if not declared["search.query"] then
  fail ("search.query is not declared")
end

harness.run {
  uuid = "hooks-search@test",
  drive = function (c)
    shell.hooks.emit_after ("search.query", { query = "foo bar", terms = { "foo", "bar" } })
    c.step ("expect-query", { n = 1, query = "foo bar", terms = { "foo", "bar" } })
    shell.hooks.emit_after ("search.query", { query = "x", terms = { "x" } })
    c.step ("expect-query", { n = 2, query = "x", terms = { "x" } })
  end,
}
