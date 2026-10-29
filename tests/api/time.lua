-- Conformance test of shell.time and of the hook time.timezone-changed
-- (docs/api/core.md, docs/api/hooks.md, Since 1.1): see tests/api/README.md.
-- The assertions are in the fixture tests/api/fixtures/time@test, run by the
-- extension manager of the shell (its metadata declares "shell-api": "1.1", so
-- loading it checks the version too); this script drives the steps and checks
-- what only the shell can see.

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

test.wait_startup ()

-- The API of the shell that runs the test is 1.1 or later
if shell.api_version.major ~= 1 or shell.api_version.minor < 1 then
  fail ("shell.api_version should be 1.1 or later, it is %d.%d",
        shell.api_version.major, shell.api_version.minor)
end

-- The module exists in the privileged state of the script too, and agrees with
-- itself
do
  local a, b = shell.time.monotonic (), shell.time.monotonic ()
  if b < a then
    fail ("shell.time.monotonic went back: %s then %s", a, b)
  end
  local t = shell.time.now ()
  if math.floor (shell.time.timestamp (shell.time.date { at = t })) ~= math.floor (t) then
    fail ("timestamp (date (t)) is not floor (t) for t = %s", t)
  end
  if shell.time.format ("%Y") ~= tostring (shell.time.date ().year) then
    fail ("format ('%%Y') is %s, date ().year is %s", shell.time.format ("%Y"), shell.time.date ().year)
  end
end

-- The hook is declared before any extension runs
do
  local found
  for _, hook in ipairs (shell.hooks.list ()) do
    if hook.name == "time.timezone-changed" then
      found = hook
    end
  end
  if not found then
    fail ("the hook time.timezone-changed is not declared")
  end
  if found.cancellable or found.schema ~= "timezone:s" then
    fail ("time.timezone-changed: cancellable %s, schema %s", tostring (found.cancellable), tostring (found.schema))
  end
end

harness.run {
  uuid = "time@test",
  drive = function (c)
    c.step ("clock")
    c.step ("roundtrip")

    -- The hook: the script plays the shell (the watch of /etc/localtime is
    -- tested by the GTest of the subproject)
    c.step ("tz-arm")
    shell.hooks.emit_after ("time.timezone-changed", { timezone = "Europe/Paris" })
    c.step ("tz-check", { timezone = "Europe/Paris" })
  end,
}
