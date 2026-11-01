-- This script tests the installation of an extension from the index
-- (src/ui/extension-downloader.c), against a test.http_server that stands in
-- for extensions.gnome.org: InstallRemoteExtension () downloads the archive
-- and asks, in a dialog that lists the capabilities the archive asks for.
-- Nothing is installed before the user agrees; Install installs and enables
-- it, Cancel leaves nothing behind, and an archive the manager would refuse
-- (an unknown capability) fails before any dialog, as one that holds a
-- symbolic link does (St and the loader would read through it), and one
-- that expands to more than the downloader takes (a zip bomb).
--
-- The check for updates offers the installed extensions to the index, but
-- not one whose directory is a link to a local copy: the update would
-- replace the files of the copy.

local TIMEOUT = 20000

local data_home = os.getenv ("XDG_DATA_HOME")
local extensions_dir = data_home .. "/gnome-shell/extensions"
local work_dir = os.getenv ("XDG_CACHE_HOME") .. "/extension-install"

local results = {
  dialog_listed_capabilities = false,
  not_installed_before_answer = false,
  installed = false,
  cancelled = false,
  refused = false,
  link_refused = false,
  bomb_refused = false,
  linked_not_updated = false,
}

test.finish (function ()
  assert (results.dialog_listed_capabilities, "The dialog did not list the capabilities of the extension")
  assert (results.not_installed_before_answer, "The extension was installed before the user agreed")
  assert (results.installed, "Install did not install and enable the extension")
  assert (results.cancelled, "Cancel did not leave the extension uninstalled")
  assert (results.refused, "An extension with an unknown capability was offered")
  assert (results.link_refused, "An archive with a symbolic link was offered")
  assert (results.bomb_refused, "An archive that expands to too much was offered")
  assert (results.linked_not_updated, "The check for updates offered a linked extension")
end)

local function exists (path)
  local file = io.open (path, "r")

  if file ~= nil then
    file:close ()
    return true
  end
  return false
end

-- Writes an extension to @dir: metadata.json and extension.lua
local function write_extension (dir, uuid, capabilities)
  local quoted = {}

  for i, capability in ipairs (capabilities) do
    quoted[i] = '"' .. capability .. '"'
  end

  assert (os.execute (string.format ("mkdir -p '%s'", dir)))
  local metadata = assert (io.open (dir .. "/metadata.json", "w"))
  metadata:write (string.format ('{"uuid": "%s", "name": "%s", "shell-api": "1.2", "capabilities": [%s]}',
                                 uuid, uuid, table.concat (quoted, ", ")))
  metadata:close ()
  local script = assert (io.open (dir .. "/extension.lua", "w"))
  script:write ("return {}\n")
  script:close ()
end

-- The archive of an extension, as the index serves it: metadata.json and
-- extension.lua at the top, @links ({name = target}) as symbolic links and
-- a file of @zeros MiB of zeros
local function make_archive (uuid, capabilities, links, zeros)
  local dir = work_dir .. "/" .. uuid
  local zip = work_dir .. "/" .. uuid .. ".zip"

  write_extension (dir, uuid, capabilities)
  assert (os.execute (string.format ("cd '%s' && python3 -m zipfile -c '%s' metadata.json extension.lua",
                                     dir, zip)))
  for name, target in pairs (links or {}) do
    -- zipfile has no symbolic links: an entry with their mode, which is
    -- what zip -y writes
    assert (os.execute (string.format ("python3 -c 'import sys, zipfile\n"
                                       .. "with zipfile.ZipFile (sys.argv[1], \"a\") as z:\n"
                                       .. "  i = zipfile.ZipInfo (sys.argv[2])\n"
                                       .. "  i.create_system = 3\n"
                                       .. "  i.external_attr = 0o120777 << 16\n"
                                       .. "  z.writestr (i, sys.argv[3])' '%s' '%s' '%s'",
                                       zip, name, target)))
  end
  if zeros ~= nil then
    assert (os.execute (string.format ("python3 -c 'import sys, zipfile\n"
                                       .. "with zipfile.ZipFile (sys.argv[1], \"a\", zipfile.ZIP_DEFLATED) as z:\n"
                                       .. "  with z.open (\"zeros\", \"w\") as f:\n"
                                       .. "    for _ in range (int (sys.argv[2])): f.write (bytes (1 << 20))' '%s' %d",
                                       zip, zeros)))
  end
  local file = assert (io.open (zip, "rb"))
  local bytes = file:read ("a")
  file:close ()
  return bytes
end

-- Calls InstallRemoteExtension () as the browser plugin does; returns a
-- function that gives the result of gdbus once it has exited
local function install (uuid)
  local result
  local proc, err = shell.spawn {
    argv = { "gdbus", "call", "--session", "--timeout", "60",
             "--dest", "org.gnome.Shell", "--object-path", "/org/gnome/Shell",
             "--method", "org.gnome.Shell.Extensions.InstallRemoteExtension", uuid },
    on_exit = function (r) result = r end,
  }
  assert (proc, err)
  return function () return result end
end

local function find_all (actor, gtype, found)
  found = found or {}
  for _, child in ipairs (actor:get_children ()) do
    if child:gtype () == gtype and child.mapped then
      found[#found + 1] = child
    end
    find_all (child, gtype, found)
  end
  return found
end

local function wait_for_dialog ()
  local dialog
  test.wait_for (function ()
    dialog = find_all (stage, "ShellModalDialog")[1]
    return dialog ~= nil
  end, "the install dialog", TIMEOUT)
  test.sleep (500)
  test.wait_leisure ()
  return dialog
end

local function wait_no_dialog ()
  test.wait_for (function ()
    return #find_all (stage, "ShellModalDialog") == 0
  end, "the install dialog to go", TIMEOUT)
end

-- Directories that an installation left in the extensions directory, other
-- than the extensions
local function leftovers ()
  local found = {}
  local ls = io.popen (string.format ("ls -A '%s' 2>/dev/null", extensions_dir))

  for name in ls:lines () do
    if name:sub (1, 1) == "." then
      found[#found + 1] = name
    end
  end
  ls:close ()
  return found
end

test.wait_startup ()

test.log ("Running extension install test")

local server = test.http_server {
  routes = {
    ["/extension-info/"] = { body = '{"name": "Test extension"}', content_type = "application/json" },
  },
}
for uuid, archive in pairs {
  ["install-caps@test"] = { { "worker", "network", "spawn" } },
  ["install-plain@test"] = { {} },
  ["install-unknown@test"] = { { "network", "teleport" } },
  ["install-link@test"] = { {}, { ["stylesheet.css"] = "/dev/zero" } },
  ["install-bomb@test"] = { {}, nil, 300 },
} do
  server:route ("/download-extension/" .. uuid .. ".shell-extension.zip",
                { body = make_archive (uuid, archive[1], archive[2], archive[3]), content_type = "application/zip" })
end
test.extension_index (server.url)

-- Capabilities, then Install
test.log ("Installing an extension with capabilities")
local done = install ("install-caps@test")
local dialog = wait_for_dialog ()
local titles = {}
for _, item in ipairs (find_all (dialog, "ShellListSectionItem")) do
  titles[#titles + 1] = item.title
end
-- Broadest first, whatever the order of metadata.json
local listed = table.concat (titles, ", ")
assert (listed == "Run programs, Use the network, Run work in the background",
        "the dialog lists: " .. listed)
results.dialog_listed_capabilities = true

results.not_installed_before_answer = not exists (extensions_dir .. "/install-caps@test/metadata.json")
  and test.extension_state ("install-caps@test") == nil

test.press_accelerator ("Return")
test.wait_for (done, "InstallRemoteExtension () to answer", TIMEOUT)
assert (done ().status == 0, "InstallRemoteExtension () failed: " .. done ().stderr)
assert (done ().stdout:find ("'successful'", 1, true), "InstallRemoteExtension () answered " .. done ().stdout)
test.wait_for (function ()
  local state = test.extension_state ("install-caps@test")
  return state ~= nil and state.state == "active"
end, "the installed extension to be active", TIMEOUT)
assert (#leftovers () == 0, "the installation left " .. table.concat (leftovers (), ", "))
results.installed = exists (extensions_dir .. "/install-caps@test/metadata.json")
wait_no_dialog ()

-- No capabilities, then Cancel
test.log ("Cancelling the installation of an extension without capabilities")
done = install ("install-plain@test")
dialog = wait_for_dialog ()
assert (#find_all (dialog, "ShellListSectionItem") == 0, "the dialog lists capabilities the extension does not ask for")
test.press_accelerator ("Escape")
test.wait_for (done, "InstallRemoteExtension () to answer", TIMEOUT)
assert (done ().stdout:find ("'cancelled'", 1, true), "InstallRemoteExtension () answered " .. done ().stdout)
wait_no_dialog ()
test.wait_for (function () return #leftovers () == 0 end, "the downloaded archive to be removed", TIMEOUT)
results.cancelled = not exists (extensions_dir .. "/install-plain@test")
  and test.extension_state ("install-plain@test") == nil

-- An archive that the manager would not load is not offered
test.log ("Installing an extension with an unknown capability")
local shown = false
local added = main.layout_manager.modal_dialog_group:connect ("child-added", function () shown = true end)
done = install ("install-unknown@test")
test.wait_for (done, "InstallRemoteExtension () to answer", TIMEOUT)
main.layout_manager.modal_dialog_group:disconnect (added)
assert (done ().status ~= 0, "InstallRemoteExtension () accepted an unknown capability")
assert (done ().stderr:find ("teleport", 1, true), "the error does not name the capability: " .. done ().stderr)
assert (not shown, "a dialog offered the extension")
test.wait_for (function () return #leftovers () == 0 end, "the downloaded archive to be removed", TIMEOUT)
results.refused = not exists (extensions_dir .. "/install-unknown@test")

test.log ("Installing an extension whose archive holds a symbolic link")
shown = false
added = main.layout_manager.modal_dialog_group:connect ("child-added", function () shown = true end)
done = install ("install-link@test")
test.wait_for (done, "InstallRemoteExtension () to answer", TIMEOUT)
main.layout_manager.modal_dialog_group:disconnect (added)
assert (done ().status ~= 0, "InstallRemoteExtension () accepted an archive with a symbolic link")
assert (done ().stderr:find ("stylesheet.css", 1, true), "the error does not name the link: " .. done ().stderr)
assert (not shown, "a dialog offered the extension")
test.wait_for (function () return #leftovers () == 0 end, "the downloaded archive to be removed", TIMEOUT)
results.link_refused = not exists (extensions_dir .. "/install-link@test")

test.log ("Installing an extension whose archive expands to 300 MiB")
shown = false
added = main.layout_manager.modal_dialog_group:connect ("child-added", function () shown = true end)
done = install ("install-bomb@test")
test.wait_for (done, "InstallRemoteExtension () to answer", TIMEOUT)
main.layout_manager.modal_dialog_group:disconnect (added)
assert (done ().status ~= 0, "InstallRemoteExtension () accepted an archive that expands to 300 MiB")
assert (done ().stderr:find ("MiB", 1, true), "the error does not say the archive is too big: " .. done ().stderr)
assert (not shown, "a dialog offered the extension")
test.wait_for (function () return #leftovers () == 0 end, "the extraction to be removed", TIMEOUT)
results.bomb_refused = not exists (extensions_dir .. "/install-bomb@test")

-- Updates are checked for when an application installs them
test.log ("Checking for updates with a linked extension")
local apps_dir = data_home .. "/applications"
assert (os.execute (string.format ("mkdir -p '%s'", apps_dir)))
local desktop = assert (io.open (apps_dir .. "/org.gnome.Extensions.desktop", "w"))
desktop:write ("[Desktop Entry]\nType=Application\nName=Extensions\nExec=true\n")
desktop:close ()
test.wait_for (function () return global:get_app_system ():lookup_app ("org.gnome.Extensions.desktop") ~= nil end,
               "the application that installs the updates", TIMEOUT)

write_extension (work_dir .. "/checkout", "linked@test", {})
assert (os.execute (string.format ("ln -s '%s/checkout' '%s/linked@test'", work_dir, extensions_dir)))
test.extension_enable ("linked@test")
test.wait_for (function ()
  local state = test.extension_state ("linked@test")
  return state ~= nil and state.state == "active"
end, "the linked extension to be active", TIMEOUT)

server:route ("/update-info/", { body = "{}", content_type = "application/json" })
local checked
local proc, err = shell.spawn {
  argv = { "gdbus", "call", "--session", "--dest", "org.gnome.Shell", "--object-path", "/org/gnome/Shell",
           "--method", "org.gnome.Shell.Extensions.CheckForUpdates" },
  on_exit = function (r) checked = r end,
}
assert (proc, err)
test.wait_for (function () return checked ~= nil end, "CheckForUpdates () to answer", TIMEOUT)
assert (checked.status == 0, "CheckForUpdates () failed: " .. checked.stderr)
local offered
test.wait_for (function ()
  for _, request in ipairs (server:requests ()) do
    if request.path:find ("^/update%-info/") then
      offered = request.body
      return true
    end
  end
  return false
end, "the index to be asked for updates", TIMEOUT)
test.log ("Offered for updates: " .. offered)
assert (offered:find ('"install-caps@test"', 1, true), "the installed extension was not offered: " .. offered)
results.linked_not_updated = not offered:find ('"linked@test"', 1, true)

server:stop ()

test.log ("Finished extension install test")
