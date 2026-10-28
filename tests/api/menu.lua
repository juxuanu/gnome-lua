-- Conformance test of docs/api/menu.md: see tests/api/README.md. The
-- assertions of the API are in the fixture tests/api/fixtures/menu@test, run
-- by the extension manager of the shell; this script drives the part that
-- needs the menu of the indicator to be opened and closed as the shell does,
-- and, through the harness, checks that disabling the extension leaves no
-- actor on the stage.

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

harness.run {
  uuid = "menu@test",

  drive = function (c)
    c.wait_done ("main")

    c.step ("menu-items")
    test.wait_leisure ()
    c.step ("menu-open")
    test.wait_leisure ()
    c.step ("menu-close")
    test.wait_leisure ()
    c.step ("menu-destroy-open")
    test.wait_leisure ()
  end,

  -- the popup menu and its box pointer have actors of their own
  actors = false,
}
