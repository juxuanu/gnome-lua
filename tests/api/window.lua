-- Conformance test of docs/api/window.md (1.0 and 1.1) and of the window.*,
-- workspace.* and monitors.changed hooks of docs/api/hooks.md: see
-- tests/api/README.md. The
-- assertions of the API are in the fixture tests/api/fixtures/window@test,
-- run by the extension manager of the shell; this script opens a window,
-- changes the monitors and asks the fixture for its verdicts with steps.

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local uuid = "window@test"

harness.run {
  uuid = uuid,
  timeout = 30000,
  -- the test opens windows: the stage has actors of its own while it runs
  actors = false,

  drive = function (c)
    c.wait_done ("main")

    -- a window of the perf helper: mapped, drawn, listed. The helper reports
    -- its window shown before the compositor maps it; the fixture reports
    -- window.mapped as it comes.
    test.create_window {}
    test.wait_windows ()
    c.wait_until (function ()
      for _, check in ipairs (test.extension_report (uuid).checks) do
        if check.name:find ("^window%.mapped #") then
          return true
        end
      end
      return false
    end, "the fixture to see window.mapped")
    test.wait_leisure ()
    c.step ("opened")

    -- minimize () and activate ()
    c.step ("minimize")
    test.wait_leisure ()
    c.step ("activate")
    test.wait_leisure ()

    -- 1.1: geometry (move, resize, move_resize, frame, window.geometry-changed),
    -- maximize/unmaximize, fullscreen, above, on_all_workspaces, unminimize
    c.step ("geometry")
    test.wait_leisure ()
    c.step ("maximize")
    test.wait_leisure ()
    c.step ("fullscreen")
    test.wait_leisure ()
    c.step ("above")
    test.wait_leisure ()
    c.step ("sticky")
    test.wait_leisure ()
    c.step ("unminimize")
    test.wait_leisure ()

    -- 1.1: a listener that connects now is told about the window (replay)
    c.step ("replay")
    test.wait_leisure ()

    -- workspace:activate (), window:move_to_workspace ()
    c.step ("switch")
    test.wait_leisure ()
    c.step ("move")
    test.wait_leisure ()

    -- a second monitor appears and goes away
    local n = main.layout_manager.n_monitors
    local added = test.watch (main.layout_manager, "monitors-changed")
    local monitor = test.new_monitor (1280, 720, 60.0)
    test.wait (added, 15000)
    test.wait_leisure ()
    c.step ("monitors-changed", { count = n + 1, width = 1280, height = 720 })

    local removed = test.watch (main.layout_manager, "monitors-changed")
    monitor:destroy ()
    test.wait (removed, 15000)
    test.wait_leisure ()
    c.step ("monitors-changed", { count = n })

    -- close (): the window goes, its handle dies
    c.step ("close")
    test.destroy_windows ()
  end,

  after_disable = function (c)
    -- the window of the helper, if a step failed before it was closed
    test.destroy_windows ()
  end,
}
