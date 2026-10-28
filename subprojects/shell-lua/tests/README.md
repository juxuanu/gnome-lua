# shell-lua tests

GTest programs, one process per `test-<area>.c`, test paths `/shell-lua/<area>/...`.

    meson setup build -Dintrospection=disabled
    meson test -C build --print-errorlogs          # everything (add a name: `... state hooks`)
    ./build/tests/test-state -k -p /shell-lua/state/watchdog   # one path, keep going (see below)

Run the binaries through `meson test`, which sets `GSETTINGS_BACKEND=memory`, `GSETTINGS_SCHEMA_DIR`
(schemas compiled from `tests/schemas/`), `G_TEST_SRCDIR` and `G_DEBUG=fatal-warnings`; set the same by
hand to run one directly. Sanitizers: `meson setup build-asan -Db_sanitize=address,undefined ...`.
Helpers live in `test-utils.h`; to add a program create `test-<name>.c` and list `<name>` in `meson.build`.
