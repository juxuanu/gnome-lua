-- Conformance test of docs/api/notify.md (and of the notification.* hooks of
-- docs/api/hooks.md): see tests/api/README.md. The assertions are in the
-- fixture tests/api/fixtures/notify@test, run by the extension manager of the
-- shell; this script drives it and does what only the shell can do: it ends
-- notifications the way the shell does (they expire, the user closes them,
-- the shell closes their source) and activates them.
--
-- The notifications are found by watching the message tray from before the
-- fixture posts them: the generic handles of the test runner give the
-- ShellNotification of every new source.

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

test.wait_startup ()

-- title -> the notification; a source is watched when the tray adds it
local seen = {}
local tray = main.message_tray

local function watch_source (source)
  source:connect ("notification-added", function (_, notification)
    seen[notification.title] = notification
  end)
end

local tray_id = tray:connect ("source-added", function (_, source)
  watch_source (source)
end)

local function find (title)
  local notification = seen[title]
  if notification == nil then
    fail ("the notification '%s' did not reach the message tray", title)
  end
  return notification
end

harness.run {
  uuid = "notify@test",
  -- the banners and the message list make actors of their own while they exist
  actors = false,
  -- the steps that wait for a banner give up after 20 s themselves, with
  -- the name of what did not happen
  timeout = 25000,

  drive = function (c)
    c.wait_done ("main")
    seen = {}

    -- Notifications ended the way the shell ends them. The destroy () of the
    -- test runner takes the reason of the shell: expired, dismissed (the
    -- default), source-closed
    c.step ("post-for-shell")
    find ("Shell1"):destroy ("expired")
    find ("Shell2"):destroy ("dismissed")
    find ("Shell3"):destroy ("source-closed")
    c.step ("check-shell-reasons")

    -- Notifications the user activates
    c.step ("post-for-activation")
    local activate = find ("Activate me")
    if activate.activate ~= nil then
      activate:activate ()
      find ("Resident"):activate ()
      c.step ("check-activation")
      c.step ("check-activation-hook")
    else
      test.print ("notify: the test runner cannot activate a notification, the activation is not tested")
      find ("Activate me"):destroy ("dismissed")
      find ("Resident"):destroy ("dismissed")
    end

    -- What needs a banner: the shell shows them, expires them, and the hooks
    -- see them (these steps wait for the banners for a while)
    c.step ("low-urgency")
    c.step ("cancel-banner")
    c.step ("banner")
    c.step ("default-source")
    c.step ("transient")
  end,
}

tray:disconnect (tray_id)
