-- This script tests the screenshot UI (src/ui/screenshot-ui.c) under fatal
-- warnings: Print opens it over a window, and Return captures and saves in
-- ~/Screenshots, in its three modes: the default selection (the middle
-- quarter of the screen), the screen (c) and the window (w). Each file must
-- hold what the screen shows: the same pixels as the screenshot that
-- org.gnome.Shell.Screenshot takes of the same thing (ScreenshotArea,
-- Screenshot, ScreenshotWindow), compared by tests/compare-png.py.

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

assert (os.execute (string.format ("mkdir -p '%s'", work_dir)))

-- Something in the middle of the screen, smaller than the selection: its
-- edges and the background around them show a capture of the wrong place
test.create_window { width = 200, height = 100 }
test.wait_windows ()
test.wait_leisure ()

-- Waits until the screen stops changing: two screenshots half a second
-- apart are the same. A new window has no content for a moment, then plays
-- the map effect (slow under ASan in CI): the UI, which captures when it
-- opens, and the reference, which comes later, must see the same screen.
local stable_count = 0
local function wait_stable_screen ()
  local previous = nil

  for _ = 1, 20 do
    stable_count = stable_count + 1
    local file = work_dir .. "/stable-" .. stable_count .. ".png"

    global.context.unsafe_mode = true
    local shot = run { "gdbus", "call", "--session", "--dest", "org.gnome.Shell",
                       "--object-path", "/org/gnome/Shell/Screenshot",
                       "--method", "org.gnome.Shell.Screenshot.Screenshot", "false", "false", file }
    global.context.unsafe_mode = false
    assert (shot.status == 0, "Screenshot failed: " .. shot.stderr)
    if previous ~= nil and run { "python3", compare, previous, file }.status == 0 then
      return
    end
    previous = file
    test.sleep (500)
    test.wait_leisure ()
  end
  error ("the screen does not settle")
end

-- Opens the UI, picks a mode with its key (nil: the default selection),
-- captures with Return; returns the file saved
local function capture_with_ui (mode_key)
  local before = #saved_screenshots ()

  wait_stable_screen ()
  test.press_accelerator ("Print")
  test.wait_for (function () return ui.visible end, "the screenshot UI", TIMEOUT)
  test.sleep (1000)
  test.wait_leisure ()
  results.opened = true
  if mode_key ~= nil then
    test.press_accelerator (mode_key)
    test.sleep (300)
    test.wait_leisure ()
  end
  test.press_accelerator ("Return")
  test.wait_for (function () return #saved_screenshots () == before + 1 end, "the screenshot to be saved", TIMEOUT)
  test.wait_for (function () return not ui.visible end, "the screenshot UI to close", TIMEOUT)
  test.wait_leisure ()
  results.saved = true
  return saved_screenshots ()[before + 1]
end

-- A screenshot through org.gnome.Shell.Screenshot (allowed in unsafe mode
-- only): @method and its arguments before the file name
local function capture_with_dbus (method, args, name)
  local file = work_dir .. "/" .. name .. ".png"
  local argv = { "gdbus", "call", "--session", "--dest", "org.gnome.Shell",
                 "--object-path", "/org/gnome/Shell/Screenshot",
                 "--method", "org.gnome.Shell.Screenshot." .. method }

  for _, arg in ipairs (args) do
    argv[#argv + 1] = arg
  end
  argv[#argv + 1] = file
  global.context.unsafe_mode = true
  local shot = run (argv)
  global.context.unsafe_mode = false
  assert (shot.status == 0 and shot.stdout:find ("true", 1, true), method .. " failed: " .. shot.stdout .. shot.stderr)
  return file
end

-- @ignore: nil or {x, y, width, height}, left out of the comparison
local function same (what, a, b, ignore)
  local argv = { "python3", compare, a, b }

  for _, v in ipairs (ignore or {}) do
    argv[#argv + 1] = tostring (math.floor (v))
  end
  local verdict = run (argv)

  test.log (what .. ": " .. verdict.stdout)
  assert (verdict.status == 0, what .. ": the screenshot and the screen differ: " .. verdict.stdout .. verdict.stderr)
end

local width, height = stage.width, stage.height

test.log ("Capturing the default selection")
same ("selection", capture_with_ui (nil),
      capture_with_dbus ("ScreenshotArea", { tostring (math.floor (width * 3 / 8)), tostring (math.floor (height * 3 / 8)),
                                            tostring (math.floor (width / 4)), tostring (math.floor (height / 4)), "false" },
                         "area"))

-- The top bar is not compared: between the two, the notification of the
-- previous screenshot puts a dot next to the clock, and the unsafe mode
-- that the D-Bus call needs shows its indicator
test.log ("Capturing the screen")
local panel_x, panel_y = main.panel:get_transformed_position ()
local panel = main.panel:get_allocation_box ()
same ("screen", capture_with_ui ("c"), capture_with_dbus ("Screenshot", { "false", "false" }, "screen"),
      { panel_x, panel_y, panel.width, panel.height })

test.log ("Capturing the window")
same ("window", capture_with_ui ("w"), capture_with_dbus ("ScreenshotWindow", { "true", "false", "false" }, "window"))
results.same_as_screen = true

test.destroy_windows ()

test.log ("Finished screenshot UI test")
