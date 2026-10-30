-- This script tests the overview (src/ui/overview.c): it shows the overview,
-- waits for it to be shown, switches to the application grid, runs a search
-- and hides the overview, under fatal warnings. The search matches two
-- applications the script installs, which the application search sorts by
-- usage. Then a D-Bus client shows and hides it through the OverviewActive
-- property of org.gnome.Shell.

local TIMEOUT = 10000

local overview = main.overview

local results = {
  hidden_at_start = false,
  shown = false,
  visible_while_shown = false,
  app_grid_shown = false,
  search_apps_installed = false,
  search_started = false,
  search_ended = false,
  hidden = false,
  dbus_shown = false,
  dbus_rewritten = false,
  dbus_hidden = false,
}

test.finish (function ()
  assert (results.hidden_at_start, "The overview was visible at startup")
  assert (results.shown, "Failed to show the overview")
  assert (results.visible_while_shown, "The overview is not visible once shown")
  assert (results.app_grid_shown, "Failed to show the application grid")
  assert (results.search_apps_installed, "The applications to search for were not found")
  assert (results.search_started, "Failed to start the search")
  assert (results.search_ended, "Failed to end the search")
  assert (results.hidden, "Failed to hide the overview")
  assert (results.dbus_shown, "Failed to show the overview over D-Bus")
  assert (results.dbus_rewritten, "Failed to write the value OverviewActive has over D-Bus")
  assert (results.dbus_hidden, "Failed to hide the overview over D-Bus")
end)

-- Writes OverviewActive as a client of the shell's D-Bus service does, and
-- waits for the reply: true when gdbus succeeded
local function set_overview_active (active)
  local result

  local proc, err = shell.spawn {
    argv = { "gdbus", "call", "--session", "--timeout", "5",
             "--dest", "org.gnome.Shell", "--object-path", "/org/gnome/Shell",
             "--method", "org.freedesktop.DBus.Properties.Set",
             "org.gnome.Shell", "OverviewActive", active and "<true>" or "<false>" },
    on_exit = function (r) result = r end,
  }
  assert (proc, err)
  test.wait_for (function () return result ~= nil end, "gdbus to exit", TIMEOUT)
  return result.status == 0
end

-- Installs the applications the search finds, in the test home
local function install_search_apps (names)
  local dir = os.getenv ("XDG_DATA_HOME") .. "/applications"
  local app_system = global:get_app_system ()

  assert (os.execute (string.format ("mkdir -p '%s'", dir)))
  for _, name in ipairs (names) do
    local file = assert (io.open (string.format ("%s/%s.desktop", dir, name), "w"))
    file:write ("[Desktop Entry]\nType=Application\nName=" .. name .. "\nExec=true\n")
    file:close ()
  end

  test.wait_for (function ()
    for _, name in ipairs (names) do
      if app_system:lookup_app (name .. ".desktop") == nil then
        return false
      end
    end
    return true
  end, "the applications to search for", TIMEOUT)
  return true
end

test.wait_startup ()

test.log ("Running overview test")

-- Two applications that one search matches equally: they make one group of
-- results, the one that sorting can get wrong
results.search_apps_installed = install_search_apps { "Luasearch-one", "Luasearch-two" }

test.sleep (1000)
test.wait_leisure ()

-- The session starts with the overview shown (the startup animation leaves
-- it visible): hide it first
if overview.visible then
  test.log ("Hiding the overview shown at startup")
  local hidden_at_start = test.watch (overview, "hidden")
  overview:hide ()
  test.wait (hidden_at_start, TIMEOUT)
  test.wait_leisure ()
end
results.hidden_at_start = not overview.visible

-- show the overview (window picker)
test.log ("Showing the overview")
local shown = test.watch (overview, "shown")
overview:show ()
test.wait (shown, TIMEOUT)
results.shown = true
results.visible_while_shown = overview.visible and not overview.animation_in_progress
test.wait_leisure ()

-- switch to the application grid
test.log ("Showing the application grid")
local show_apps = overview.dash.show_apps_button
show_apps.checked = true
test.sleep (600)
test.wait_leisure ()
results.app_grid_shown = show_apps.checked and overview.visible

-- run a search
test.log ("Searching")
local search_controller = overview.search_controller
local search_entry = overview.search_entry
search_entry.text = "luasearch"
test.wait_for (function ()
  return search_controller.search_active
end, "the search to start", TIMEOUT)
results.search_started = true
test.sleep (800)
test.wait_leisure ()

search_entry.text = ""
test.wait_for (function ()
  return not search_controller.search_active
end, "the search to end", TIMEOUT)
results.search_ended = true
test.sleep (600)
test.wait_leisure ()

-- hide the overview
test.log ("Hiding the overview")
local hidden = test.watch (overview, "hidden")
overview:hide ()
test.wait (hidden, TIMEOUT)
results.hidden = not overview.visible
test.wait_leisure ()

-- the same over D-Bus; writing the value the property already has used to
-- spin the shell in the notify handler of the property
test.log ("Showing the overview over D-Bus")
shown = test.watch (overview, "shown")
assert (set_overview_active (true), "Failed to write OverviewActive")
test.wait (shown, TIMEOUT)
results.dbus_shown = overview.visible
test.wait_leisure ()

results.dbus_rewritten = set_overview_active (true) and overview.visible
test.wait_leisure ()

test.log ("Hiding the overview over D-Bus")
hidden = test.watch (overview, "hidden")
assert (set_overview_active (false), "Failed to write OverviewActive")
test.wait (hidden, TIMEOUT)
results.dbus_hidden = not overview.visible
test.wait_leisure ()

test.log ("Finished overview test")
