-- Conformance test of docs/api/overview.md (and the `extension` reason and
-- the `owner` key of the overview.* hooks of docs/api/hooks.md): see
-- tests/api/README.md. The assertions of the API are in the fixture
-- tests/api/fixtures/overview@test, run by the extension manager of the
-- shell; this script starts every step from the hidden overview (the session
-- starts in the overview, tests/shell/overview.lua) and compares what the
-- extension sees with the state of the overview itself.

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local TIMEOUT = 10000

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

local function settle ()
  test.wait_leisure ()
  test.sleep (300)
  test.wait_leisure ()
end

harness.run {
  uuid = "overview@test",
  timeout = 30000,

  -- the overview has actors of its own that are not there before the first
  -- time it is shown
  actors = false,

  drive = function (c)
    local overview = main.overview

    c.wait_done ("main")

    -- Every step starts from the hidden overview
    local function hide_overview ()
      if overview.visible then
        local hidden = test.watch (overview, "hidden")
        overview:hide ()
        test.wait (hidden, TIMEOUT)
      end
      settle ()
      if overview.visible then
        fail ("the overview is still visible after hide ()")
      end
    end

    hide_overview ()

    -- show (): the extension's view and the overview's own agree
    c.step ("show")
    settle ()
    if not (overview.visible and not overview.animation_in_progress) then
      fail ("the overview is not shown after the step 'show'")
    end
    if not overview.visible_target then
      fail ("the overview does not have a visible target after the step 'show'")
    end

    -- hide (), and hide () of the hidden overview
    c.step ("hide")
    settle ()
    if overview.visible then
      fail ("the overview is visible after the step 'hide'")
    end
    c.step ("noop")

    -- toggle () twice: shows, then hides
    c.step ("toggle")
    settle ()
    if overview.visible then
      fail ("the overview is visible after the step 'toggle'")
    end

    -- show { target = "app-grid" }: stays shown, the script hides it
    c.step ("app-grid")
    settle ()
    if not overview.visible then
      fail ("the overview is not shown after the step 'app-grid'")
    end
    hide_overview ()

    -- a cancelled show, a hiding that cannot be cancelled
    c.step ("cancel")
    hide_overview ()

    -- calls made from the hooks of the overview
    c.step ("deferred")
    hide_overview ()
  end,
}
