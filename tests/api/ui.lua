-- Conformance test of docs/api/ui.md (and README.md where it concerns
-- handles, ownership and teardown): see tests/api/README.md. The assertions
-- of the API are in the fixture tests/api/fixtures/ui@test, run by the
-- extension manager of the shell; this script drives the one that is heavy
-- (the cap on the objects an extension owns) and, through the harness,
-- checks that disabling the extension leaves no actor on the stage.

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

harness.run {
  uuid = "ui@test",

  drive = function (c)
    c.wait_done ("main")
    c.step ("cap")
    test.wait_leisure ()
  end,
}
