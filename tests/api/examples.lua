-- Every reference extension of subprojects/shell-lua/examples enables without
-- an error, runs, and disables cleanly (twice, to show that a second enable
-- finds nothing left by the first). They do not report to the harness: a
-- Lua error in enable () or in a callback puts the extension in the error
-- state, which fails the test. They are written against the documents of
-- docs/api, so this test also tells when a module they use is missing.

local harness = dofile (((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local EXAMPLES = {
  "hello@gnome-lua.example",
  "indicator@gnome-lua.example",
  "quick-toggle@gnome-lua.example",
  "dash-launcher@gnome-lua.example",
  "hooks-log@gnome-lua.example",
  "prefs@gnome-lua.example",
}

for _, uuid in ipairs (EXAMPLES) do
  harness.run {
    uuid = uuid,
    fixture = false,
    actors = false,     -- what the area tests check, with their own fixtures
    drive = function (c)
      -- let the timers and the first callbacks run
      test.sleep (300)
      test.wait_leisure ()
    end,
  }
end
