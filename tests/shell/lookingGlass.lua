-- This script tests the evaluator of Looking Glass (src/ui/looking-glass.c)
-- under fatal warnings: it types code in the entry and runs it with Return,
-- and the result is shown. The code can destroy Looking Glass itself
-- (main.looking_glass:destroy ()): the evaluator went on with its freed
-- self to show the result. meson runs this script with glibc's malloc
-- filling freed memory, which turns that into a crash.
--
-- It also calls org.gnome.Shell.Eval, the same evaluator over D-Bus: the
-- answer has what the code printed, and bytes that are not UTF-8 (which a
-- D-Bus string cannot hold, and the answer was lost) are replaced.

local TIMEOUT = 10000

local overview = main.overview

local results = {
  opened = false,
  evaluated = false,
  destroyed = false,
  dbus_eval = false,
}

test.finish (function ()
  assert (results.opened, "Looking Glass did not open")
  assert (results.evaluated, "Return did not evaluate the code")
  assert (results.destroyed, "The code did not destroy Looking Glass")
  assert (results.dbus_eval, "org.gnome.Shell.Eval did not answer as expected")
end)

-- org.gnome.Shell.Eval (allowed in unsafe mode only): the output of gdbus
local function dbus_eval (code)
  local result

  global.context.unsafe_mode = true
  local proc, err = shell.spawn {
    argv = { "gdbus", "call", "--session", "--dest", "org.gnome.Shell", "--object-path", "/org/gnome/Shell",
             "--method", "org.gnome.Shell.Eval", code },
    on_exit = function (r) result = r end,
  }
  assert (proc, err)
  test.wait_for (function () return result ~= nil end, "gdbus", TIMEOUT)
  global.context.unsafe_mode = false
  assert (result.status == 0, "Eval failed: " .. result.stderr)
  return result.stdout
end

test.wait_startup ()

test.log ("Running Looking Glass test")

if overview.visible then
  local hidden = test.watch (overview, "hidden")
  overview:hide ()
  test.wait (hidden, TIMEOUT)
end

-- Types @text in the entry that has the key focus, and Return
local function enter (text)
  test.wait_for (function () return stage.key_focus ~= nil end, "an entry to have the key focus", TIMEOUT)
  stage.key_focus.text = text
  test.press_accelerator ("Return")
  test.wait_leisure ()
end

-- Opens Looking Glass as one does: "lg" in the run dialog
local function open_looking_glass ()
  test.press_accelerator ("<Alt>F2")
  test.wait_for (function () return main.run_dialog ~= nil and main.run_dialog.visible end, "the run dialog", TIMEOUT)
  enter ("lg")
  test.wait_for (function () return main.looking_glass ~= nil and main.looking_glass.is_open end,
                 "Looking Glass to open", TIMEOUT)
  return main.looking_glass
end

local evaluate = enter

local lg = open_looking_glass ()
results.opened = true

evaluate ("evaluated = 6 * 7")
test.wait_for (function () return stage.key_focus ~= nil and stage.key_focus.text == "" end,
               "the entry to be emptied", TIMEOUT)
results.evaluated = true

local gone = test.watch (lg, "destroy")
evaluate ("main.looking_glass:destroy ()")
test.wait (gone, TIMEOUT)
test.wait_for (function () return main.looking_glass == nil end, "Looking Glass to be gone", TIMEOUT)
results.destroyed = true

-- Opening it again makes a new one, which works
lg = open_looking_glass ()
evaluate ("evaluated = 1")
test.wait_for (function () return stage.key_focus ~= nil and stage.key_focus.text == "" end,
               "the entry of the new Looking Glass to be emptied", TIMEOUT)
test.press_accelerator ("Escape")
test.wait_for (function () return not lg.is_open end, "the new Looking Glass to close", TIMEOUT)

test.log ("Calling org.gnome.Shell.Eval")
local answer = dbus_eval ("print ('printed') return 6 * 7")
test.log ("Eval: " .. answer)
assert (answer:find ("(true, 'printed\\n42')", 1, true), "Eval did not answer with the output and the value: " .. answer)
answer = dbus_eval ("return 'a' .. string.char (255) .. 'b'")
test.log ("Eval: " .. answer)
assert (answer:find ("(true, 'a\u{FFFD}b')", 1, true), "Eval did not replace the byte that is not UTF-8: " .. answer)
answer = dbus_eval ("error ('x' .. string.char (255) .. 'y')")
assert (answer:find ("(false, ", 1, true) and answer:find ("x\u{FFFD}y", 1, true), "Eval did not answer the error: " .. answer)
results.dbus_eval = true

test.log ("Finished Looking Glass test")
