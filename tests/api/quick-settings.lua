-- Conformance test of docs/api/quick-settings.md: see tests/api/README.md. The
-- assertions are in the fixture tests/api/fixtures/quick-settings@test, run
-- by the extension manager of the shell; this script drives it and does what
-- only the shell can do: it opens and closes the system menu, which the
-- fixture sees through the hook quick-settings.opened and through the
-- menus of its items (menu:open () is false unless the system menu is open).
--
-- Not covered here, for lack of a way to give the script the actors of the
-- fixture: the events the user causes (on_toggle, on_click, on_changed,
-- on_icon_click) and the position of the items in the grid. The fixture
-- checks what it can: that assignments call nothing back (README rule 7),
-- the errors of the callbacks, the index of every item.

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

test.wait_startup ()

harness.run {
  uuid = "quick-settings@test",

  drive = function (c)
    c.wait_done ("main")

    -- Whatever the assignments of the fixture caused has been processed
    test.sleep (300)
    test.wait_leisure ()
    c.step ("assignments-later")

    local button = main.panel:get_status_area_indicator ("quickSettings")
    if button == nil then
      fail ("the top bar has no quick settings button")
    end
    local menu = button.menu

    -- The system menu opens: the hook runs, the menus of the items can open
    menu:open ()
    c.wait_until (function ()
      return menu.is_open
    end, "the system menu to open")
    test.sleep (500)
    test.wait_leisure ()
    c.step ("opened")

    -- It closes: the menus of the items cannot open
    menu:close ()
    c.wait_until (function ()
      return not menu.is_open
    end, "the system menu to close")
    test.sleep (800)
    test.wait_leisure ()
    c.step ("closed")

    -- And opens again: the hook runs every time
    menu:open ()
    c.wait_until (function ()
      return menu.is_open
    end, "the system menu to open again")
    test.sleep (300)
    c.step ("opened-again")
    menu:close ()
    c.wait_until (function ()
      return not menu.is_open
    end, "the system menu to close again")
    test.sleep (500)
  end,
}
