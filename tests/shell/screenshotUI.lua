-- This script tests the screenshot UI (src/ui/screenshot-ui.c) under fatal
-- warnings: Print opens it over a window, Return captures the default
-- selection (the middle quarter of the screen) and saves it in
-- ~/Screenshots. The file must hold what the screen shows there: the same
-- pixels as a screenshot of that area taken through org.gnome.Shell.Screenshot
-- (ScreenshotArea), compared by tests/compare-png.py.

local TIMEOUT = 10000

local overview = main.overview
local ui = main.screenshot_ui
local work_dir = os.getenv ("XDG_CACHE_HOME") .. "/screenshot-ui"
local compare = ((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/shell/[^/]*$") or ".") .. "/compare-png.py"

local results = {
  opened = false,
  saved = false,
  same_as_screen = false,
}

test.finish (function ()
  assert (results.opened, "Print did not open the screenshot UI")
  assert (results.saved, "Return did not save a screenshot")
  assert (results.same_as_screen, "The screenshot does not show what the screen shows")
end)

local function run (argv)
  local result
  local proc, err = shell.spawn { argv = argv, on_exit = function (r) result = r end }
  assert (proc, err)
  test.wait_for (function () return result ~= nil end, argv[1], TIMEOUT)
  return result
end

local function saved_screenshots ()
  local found = {}
  local ls = io.popen (string.format ("ls '%s/Screenshots' 2>/dev/null", os.getenv ("HOME")))

  for name in ls:lines () do
    if name:match ("%.png$") then
      found[#found + 1] = os.getenv ("HOME") .. "/Screenshots/" .. name
    end
  end
  ls:close ()
  return found
end

test.wait_startup ()

test.log ("Running screenshot UI test")

-- The session starts with the overview shown: hide it first
if overview.visible then
  local hidden = test.watch (overview, "hidden")
  overview:hide ()
  test.wait (hidden, TIMEOUT)
end

-- Something in the middle of the screen, smaller than the selection: its
-- edges and the background around them show a capture of the wrong place
test.create_window { width = 200, height = 100 }
test.wait_windows ()
test.wait_leisure ()

test.log ("Opening the screenshot UI")
test.press_accelerator ("Print")
test.wait_for (function () return ui.visible end, "the screenshot UI", TIMEOUT)
test.sleep (1000)
test.wait_leisure ()
results.opened = true

test.log ("Capturing the default selection")
test.press_accelerator ("Return")
test.wait_for (function () return #saved_screenshots () == 1 end, "the screenshot to be saved", TIMEOUT)
test.wait_for (function () return not ui.visible end, "the screenshot UI to close", TIMEOUT)
test.wait_leisure ()
results.saved = true

-- The same area through D-Bus (the API allows it in unsafe mode only)
assert (os.execute (string.format ("mkdir -p '%s'", work_dir)))
local width, height = stage.width, stage.height
local reference = work_dir .. "/reference.png"
global.context.unsafe_mode = true
local shot = run { "gdbus", "call", "--session", "--dest", "org.gnome.Shell",
                   "--object-path", "/org/gnome/Shell/Screenshot",
                   "--method", "org.gnome.Shell.Screenshot.ScreenshotArea",
                   tostring (math.floor (width * 3 / 8)), tostring (math.floor (height * 3 / 8)),
                   tostring (math.floor (width / 4)), tostring (math.floor (height / 4)),
                   "false", reference }
global.context.unsafe_mode = false
assert (shot.status == 0 and shot.stdout:find ("true", 1, true), "ScreenshotArea failed: " .. shot.stdout .. shot.stderr)

local verdict = run { "python3", compare, saved_screenshots ()[1], reference }
test.log ("Comparison: " .. verdict.stdout)
results.same_as_screen = verdict.status == 0
assert (results.same_as_screen, "the screenshot and the screen differ: " .. verdict.stdout .. verdict.stderr)

test.destroy_windows ()

test.log ("Finished screenshot UI test")
