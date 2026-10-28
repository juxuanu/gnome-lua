# API conformance tests

One headless shell test per document of `docs/api/`: `tests/api/<area>.lua`
runs a **fixture extension**, `tests/api/fixtures/<area>@test/`, in the real
shell and fails when the API does not do what the document says. The tests run
against every release (`meson test --suite api`); a document and its test
change together.

| Test | Document | Fixtures |
|---|---|---|
| `core.lua` | core.md (and README.md for what concerns the core) | `core@test`, `core-bare@test`, `core-errors@test`, `core-minor@test`, `core-future@test`, `core-v2@test`, `core-modes@test` |
| `hooks.lua` | hooks.md, `shell.hooks` of core.md | `hooks@test`, `hooks-search@test` |
| `examples.lua` | the reference extensions of `subprojects/shell-lua/examples` | (the examples themselves) |
| `ui`, `menu`, `panel`, `dash`, `quick-settings`, `notify`, `window`, `dbus` | the other documents | `<area>@test` |

## How a test runs

1. `tests/meson.build` lists the test in `api_tests` and starts
   `gnome-shell-test-tool --headless --install-extension <fixture dir> ...
   tests/api/<area>.lua`. The tool copies each fixture directory to the
   per-user extension directory of the test home (compiling `schemas/`), so
   the extension is installed and **not** enabled.
2. The script is a script of the Lua test runner (`src/shell-test-runner.c`,
   the same as `tests/shell/*.lua`). It calls the harness:

   ```lua
   local harness = dofile(((os.getenv("SHELL_TEST_SCRIPT") or ""):match("^(.*)/[^/]*$") or ".") .. "/harness.lua")
   harness.run { uuid = "ui@test" }
   ```

3. `harness.run` enables the fixture through the extension manager of the shell
   (`test.extension_enable`: the uuid goes to `enabled-extensions`), waits for
   the state `active`, calls the optional `drive (c)` of the test, waits until
   the fixture reports that it is done, fails on any failed check, any Lua
   error of the extension (`test.extension_state (uuid).errors`) or a state
   other than `active`; then it disables the extension and checks that
   nothing is left: no listener on any hook, no new actor on the stage (the
   `test.fingerprint ()` before the first enable against the one after each
   disable), and that no callback runs any more. It does this twice (a second
   enable shows what the first one left behind). Options and the context `c`
   of `drive` are documented at the head of `harness.lua`.

An error anywhere fails the shell test: the runner exits with an error.

## Writing a fixture

`metadata.json` names the uuid (`<area>@test`, the same as the directory) and
the `capabilities` and `settings-schema` the area needs. `extension.lua` starts
with the preamble below (copy it as it is: a fixture can only `require` files
of its own directory), runs the checks that are synchronous in `enable ()`, and
ends `enable ()` with `T.done ()`.

```lua
local shell = require("shell")
local T = shell.test     -- exists only when a test script runs (src/shell-test-runner.c)
-- case (name, fn, ...)      runs fn in pcall and reports one check
-- eq (got, want, what)      raises unless got == want
-- truthy (value, what)      raises unless value
-- raises (fn, patterns, what)   fn () must raise; patterns: a string or a list,
--                           each must appear in the message (nil: any error)
-- steps { name = function (payload, done) ... end }
--                           handlers of the steps the script asks for
```

The full text of these helpers is at the head of
`tests/api/fixtures/core@test/extension.lua`.

`shell.test` (sandbox, test runs only) is the whole channel from the extension
to the script:

- `T.report (name, ok [, message])` records one check (the name appears in the
  failure message; the failure is also printed at once, so a run that aborts
  says what failed);
- `T.done ([label])` says that the main part (`"main"`, what `harness.run`
  waits for) or the step `label` is finished.

**Steps** carry what the extension cannot do alone (the script drives the
shell, waits, changes settings from outside, opens the overview, ...):
the script calls `c.step ("name", {payload})`, which emits the hook `test.step`
with `{step = "name", ...}` and waits for `T.done ("name")`; the fixture
handles it with `steps {}`. A handler is finished when it returns, unless it
returns `"later"`: then it calls `done ()` when its timers or callbacks have
run. A failing check in a step fails the step at once.

What the script can do besides the harness (all in `src/shell-test-runner.c`,
documented at its head): `test.sleep`, `test.wait_leisure`, `main.*` (the
objects of the shell, with `overview:show_for_reason (reason [, state])` and
`hide_for_reason (reason)`), `test.set_boolean (schema, key, value)`,
`test.log_capture (uuid, on)` (`c.capture_log (fn)`: the log of the extension,
and the errors of its callbacks, as `{level, message}`; warnings are not
fatal inside), `test.extension_report (uuid)`.

## Rules

- The shell tests run with `G_DEBUG=fatal-warnings`. An error in a callback,
  `shell.log.warning` and `shell.log.error` are warnings in the runtime: run
  them inside `c.capture_log`, and declare the errors you cause on purpose with
  `expect_errors`. A metadata that the loader refuses (an unknown capability)
  is a warning at startup, before the script runs: it cannot be a fixture
  (`subprojects/shell-lua/tests/test-extension.c` covers it).
- A step handler or a callback cannot wait (`test.sleep` yields the script, not
  the extension): use timers in the fixture and `"later"`, or let the script
  wait on what it can see.
- Hook payloads of a real emission can only be checked on the hooks the script
  can trigger; for the others use `shell.hooks.emit_after/emit_before` from the
  script (a privileged state), which tests the bridge, not the emitter.
- The comparison of the stage after disable (`actors`) counts the actors of
  each type; a test that makes actors of its own on purpose (the overview, a
  notification banner) passes `actors = false` or `ignore_actors`.
- Everything the test needs to wait for has a timeout (15 s by default); a test
  that hangs fails with the name of what it waited for.

## Adding an area

1. Write `tests/api/fixtures/<area>@test/{metadata.json,extension.lua}` and
   `tests/api/<area>.lua` (one `harness.run` for a simple area).
2. Add `{ 'name': '<area>' }` to `api_tests` in `tests/meson.build`
   (`'fixtures'` lists more than one fixture, `'extension_dirs'` other
   extensions to install).
3. Run it: `meson test -C build --suite api` (or one test:
   `meson test -C build api-<area>`), or by hand:

       gnome-shell-test-tool --headless --install-extension tests/api/fixtures/<area>@test tests/api/<area>.lua
