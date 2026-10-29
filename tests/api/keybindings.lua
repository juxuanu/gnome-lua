-- Conformance test of docs/api/keybindings.md (API 1.1): see
-- tests/api/README.md. The assertions of the API are in the fixture
-- tests/api/fixtures/keybindings@test, run by the extension manager of the
-- shell; this script types the key combinations on a virtual keyboard
-- (test.press_accelerator) and asks the fixture, with steps, whether its
-- bindings ran: a literal accelerator, a settings-backed binding and its
-- rebinding through the key, the two modes, a binding that removes itself
-- from its own callback.
--
-- The presses are real key events through Mutter, so the test covers the
-- whole path: grab, filter by action mode, dispatch, callback.

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local WAIT_MS = 8000

harness.run {
  uuid = "keybindings@test",

  drive = function (c)
    local overview = main.overview

    c.wait_done ("main")

    -- the session may start with the overview shown: the first half of the
    -- test runs in the desktop (the action mode normal)
    if overview.visible then
      local hidden = test.watch (overview, "hidden")
      overview:hide ()
      test.wait (hidden, WAIT_MS)
    end
    test.wait_leisure ()

    c.step ("arm")
    test.wait_leisure ()

    -- the fixture reports a step as done once: each use has a name of its own
    local waits, stays = 0, 0

    -- presses `accelerator`, then waits until counts[name] reaches n
    local function press_and_wait (accelerator, name, n)
      waits = waits + 1
      test.press_accelerator (accelerator)
      c.step ("wait-" .. waits, { name = name, n = n, ms = WAIT_MS })
    end

    -- presses `accelerator` and checks that counts[name] is still n later
    local function press_and_stay (accelerator, name, n)
      stays = stays + 1
      test.press_accelerator (accelerator)
      c.step ("stay-" .. stays, { name = name, n = n, ms = 500 })
    end

    -- literal accelerator, two callbacks
    press_and_wait ("<Super><Alt>y", "lit", 1)
    c.step ("same")

    -- settings-backed (default <Super><Alt>j)
    press_and_wait ("<Super><Alt>j", "sb", 1)

    -- rebinding through the key: the new combination runs, the old one does not
    c.step ("rebind")
    test.wait_leisure ()
    press_and_wait ("<Super><Alt>k", "sb", 2)
    press_and_stay ("<Super><Alt>j", "sb", 2)

    -- an empty key disables the binding
    c.step ("unbind")
    test.wait_leisure ()
    press_and_stay ("<Super><Alt>k", "sb", 2)
    c.step ("restore")
    test.wait_leisure ()
    press_and_wait ("<Super><Alt>j", "sb", 3)

    -- modes: in the desktop the normal binding runs, the overview one does not
    press_and_wait ("<Super><Alt>n", "nm", 1)
    press_and_stay ("<Super><Alt>o", "ov", 0)

    -- a binding that destroys itself from its callback
    press_and_wait ("<Super><Alt>i", "once", 1)
    test.wait_leisure ()
    c.step ("once-dead")
    press_and_stay ("<Super><Alt>i", "once", 1)

    -- in the overview it is the other way round
    local shown = test.watch (overview, "shown")
    overview:show ()
    test.wait (shown, WAIT_MS)
    test.wait_leisure ()
    test.sleep (300)

    press_and_wait ("<Super><Alt>o", "ov", 1)
    press_and_stay ("<Super><Alt>n", "nm", 1)

    local hidden = test.watch (overview, "hidden")
    overview:hide ()
    test.wait (hidden, WAIT_MS)
    test.wait_leisure ()
    test.sleep (300)

    -- back in the desktop; destroyed bindings do not run any more
    c.step ("destroy-lit")
    test.wait_leisure ()
    press_and_stay ("<Super><Alt>y", "lit", 1)
    press_and_stay ("<Super><Alt>j", "sb", 3)
  end,

  -- the overview and its dash have actors of their own that are not there
  -- before the first time the overview is shown
  actors = false,
}
