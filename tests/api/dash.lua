-- Conformance test of docs/api/dash.md (and README.md where it concerns the
-- dash): see tests/api/README.md. The assertions of the API are in the
-- fixture tests/api/fixtures/dash@test, run by the extension manager of the
-- shell; this script drives it while the overview shows the dash: buttons
-- that are added, edited and destroyed while they are on the screen.

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

harness.run {
  uuid = "dash@test",

  -- the dash is part of the overview: the buttons made by the fixture add
  -- their actors to it, which the fingerprint of the test compares
  drive = function (c)
    local overview = main.overview

    c.wait_done ("main")

    -- with the dash on the screen
    overview:show ()
    test.wait_leisure ()
    test.sleep (300)

    c.step ("overview-add")
    test.wait_leisure ()
    c.step ("overview-edit")
    test.wait_leisure ()
    c.step ("overview-destroy")
    test.wait_leisure ()
    test.sleep (400)   -- the buttons fade out
    test.wait_leisure ()

    overview:hide ()
    test.wait_leisure ()
    test.sleep (300)
    test.wait_leisure ()
  end,

  -- the overview and the dash have actors of their own that are not there
  -- before the first time the overview is shown
  actors = false,
}
