# shell-lua tests

GTest programs, one process per `test-<area>.c`, test paths `/shell-lua/<area>/...`.

    meson setup build -Dintrospection=disabled
    meson test -C build --print-errorlogs          # everything (add a name: `... state hooks`)
    ./build/tests/test-state -k -p /shell-lua/state/watchdog   # one path, keep going (see below)

Run the binaries through `meson test`, which sets `GSETTINGS_BACKEND=memory`, `GSETTINGS_SCHEMA_DIR`
(schemas compiled from `tests/schemas/`), `G_TEST_SRCDIR` and `G_DEBUG=fatal-warnings`; set the same by
hand to run one directly. Sanitizers: `meson setup build-asan -Db_sanitize=address,undefined ...`.
Helpers live in `test-utils.h`; to add a program create `test-<name>.c` and list `<name>` in `meson.build`.

`test-fuzz.c` fuzzes the Lua API boundary (docs/PLAN.md M8): malformed handles, re-entrancy from hooks,
disabling an extension from inside its own callbacks, memory/stack/watchdog limits, and random sequences
of API calls whose registry counts are checked. Everything is deterministic: the random choices come from
one seeded `GRand`. Run it under the sanitizers, they are what catch the use-after-free and leak classes it
targets. Knobs: `SHELL_LUA_FUZZ_SEED=<n>` (the seed in use is printed as a test message),
`SHELL_LUA_FUZZ_ITERATIONS=<n>` (random operations per random test, default 1500) and
`SHELL_LUA_FUZZ_VERBOSE=1` (every random operation is traced to stderr before it runs, so the last line
names the operation a crash happened in). To reproduce a failure, rerun with the printed seed.
