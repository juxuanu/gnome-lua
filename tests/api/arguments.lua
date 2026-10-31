-- Conformance test of README calling convention 1 (docs/api/README.md):
-- option tables, and the tables in them that the API reads, are plain
-- tables read raw. The checks are in the fixture tests/api/fixtures/
-- arguments@test, which runs them all when it is enabled; see
-- tests/api/README.md.

local harness = dofile (((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

harness.run {
  uuid = "arguments@test",
  timeout = 30000,

  drive = function (c)
    c.wait_done ("main")
  end,
}
