-- Conformance test of docs/api/panel.md (and README.md where it concerns the
-- top bar): see tests/api/README.md. The assertions of the API are in the
-- fixture tests/api/fixtures/panel@test, run by the extension manager of the
-- shell; this script drives it and checks what only the shell can see: where
-- the indicators are on the top bar ("Placement" of panel.md and "Placement
-- and ordering" of README.md).

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

local function x_of (name)
  local indicator = main.panel:get_status_area_indicator (name)

  if indicator == nil then
    fail ("the status area has no indicator %q", name)
  end

  return (indicator:get_transformed_position ())
end

-- The indicators of the list are in this order from the start of the bar
local function expect_order (names, what)
  local previous

  for _, name in ipairs (names) do
    local x = x_of (name)

    if previous and not (previous.x < x) then
      fail ("%s: %s (x = %.0f) should be right of %s (x = %.0f); the order is %s", what,
            name, x, previous.name, previous.x, table.concat (names, ", "))
    end
    previous = { name = name, x = x }
  end
end

harness.run {
  uuid = "panel@test",

  drive = function (c)
    local uuid = "panel@test"

    -- [extensions at "start"] [built-ins] [extensions at "end"], in every box
    c.step ("placement")
    test.wait_leisure ()

    expect_order ({ "r-start-1", uuid, "plain-ind", "r-start-2", "r-start-3", "r-start-clamped",
                    "quickSettings", "r-end-1", "r-end-2" }, "right box")
    expect_order ({ "l-start-1", "l-start-2", "activities", "l-end-1" }, "left box")
    expect_order ({ "c-start-1", "dateMenu", "c-end-0", "c-end-1" }, "center box")

    -- the place of a destroyed indicator is free, the others keep their order
    c.step ("placement-destroy")
    test.wait_leisure ()
    if main.panel:get_status_area_indicator ("r-start-3") ~= nil then
      fail ("r-start-3 is still in the status area after it was destroyed")
    end
    expect_order ({ "r-start-1", uuid, "plain-ind", "r-start-2", "r-start-clamped",
                    "quickSettings", "r-end-1", "r-end-2" }, "right box after a destroy")

    -- the menu of the indicator
    c.step ("menu-items")
    test.wait_leisure ()
    c.step ("menu-open")
    test.wait_leisure ()
    c.step ("menu-hide")
    test.wait_leisure ()
    c.step ("menu-clear")
  end,
}
