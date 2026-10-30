# Building and Running

## Building gnome-shell

gnome-shell uses the [meson] build system, and can be compiled
with the following commands:

```sh
$ meson setup _build
$ meson compile -C _build
```

Unfortunately gnome-shell has a non-trivial number of dependencies
that cannot always be satisfied by distribution packages.

This is particular true for [mutter], which is developed in lock-step
with gnome-shell and always has to be built from source.

The shell no longer depends on GJS or SpiderMonkey: it is C plus an embedded
Lua 5.5 (vendored as a Meson subproject, see `docs/PLAN.md`). The
GObject Introspection data of libshell and libst is still generated, as the
API reference is built from it; `-Dintrospection=false` skips it (it cannot be
combined with `-Dgtk_doc=true`).

## Running the tests

```sh
$ meson test -C _build
```

The `unit` suite holds GTest programs that link libshell. The `shell` suite
starts the shell headless under `gnome-shell-test-tool` with a Lua script from
`tests/shell/` (the `SHELL_TEST_SCRIPT` runner of `src/shell-test-runner.c`).
Run them under `G_DEBUG=fatal-warnings` as CI does.

[meson]: https://mesonbuild.com/
[mutter]: https://mutter.gnome.org

## Toolbox

[Toolbox][toolbox] is a container tool for Linux, which allows the use of
interactive command line environments for development, without having to
install software on the host.

It is suitable for gnome-shell development, and we maintain a number of scripts
to make its use easier and more convenient.

You can set up a new container that satisfies all build- and runtime
dependencies with the following script:

```sh
$ tools/toolbox/create-toolbox.sh
```

The script will download the container image and build mutter, so it is
expected that it will take a while.

Once you have a working container, the following scripts can be
used to build and run gnome-shell:

```sh
$ tools/toolbox/meson-build.sh
$ tools/toolbox/run-gnome-shell.sh
```

If building or running fails with errors about `meta`, `clutter` or `cogl`,
there was probably an incompatible change in mutter. You can update the
dependency with the following command:

```sh
$ toolbox run --container gnome-shell-devel update-mutter
```

Refer to the [README][toolbox-tools] for further information on the scripts.

[toolbox]: https://containertoolbx.org/
[toolbox-tools]: ../tools/toolbox/README.md

## Building with podman (any distribution)

The dependencies are GNOME's: Mutter 51, gnome-desktop, libgdm, accountsservice
and a dozen libraries. On a non-GNOME distribution the container image CI
builds in avoids installing them on the host:

```sh
$ tools/podman/build.sh                      # builds the image once, then meson setup + compile
$ tools/podman/build.sh meson test -C build   # any command in the image
$ tools/podman/run-nested.sh                 # the built shell, nested in the current Wayland session
$ tools/podman/run-nested.sh --mode=gdm      # the login screen, nested
```

`build.sh` runs the image build, compiles and tests at the lowest CPU and I/O
priority (`GNOME_LUA_LOW_PRIORITY=0` leaves the priority as is).

`run-nested.sh` hands the container the session's Wayland socket, the GPU
render nodes and the source tree, read-only. It runs the shell uninstalled
from `build/`, as the current user, with its own session bus, an empty system
bus (`tools/podman/system-bus.conf`), an in-memory settings backend and a home
inside the container: whatever the shell writes goes away with the container.
`SHELL_LUA_EXTENSION_PATH` points at the example extensions and at
`tools/podman/extensions/`, where other extensions can be dropped (one
`<uuid>/` directory each). The system bus has no service on it, but it has to
exist: without one the parental controls never finish initializing, so no
application is shown and searches never end. Set `LIBGL_ALWAYS_SOFTWARE=1` if
the host GPU driver and the image's Mesa disagree. It works under KDE Plasma
as under GNOME: the devkit backend only needs a Wayland parent.

With `--mode=gdm` the system bus also gets stand-ins for GDM and
AccountsService (`tools/podman/greeter-mock.py`), and the container a
`/run/systemd/seats` directory: the login screen shows its dialog only where
the screen can be locked, which takes both. It lists the current user and
"guest"; the password of both is `gnome`, anything else is refused, and a
verified login fades the screen out where GDM would start the session.

Build options that trim dependencies for a development build:
`-Dcalendar_server=false` (no evolution-data-server; the date menu shows no
events), `-Dportal_helper=false` (no WebKitGTK), `-Dnetworkmanager=false`,
`-Dbluetooth=false`, `-Dmalcontent=false`, `-Dintrospection=false`, and
`-Dtests=false` where the distribution's Mutter ships without its test
library (`libmutter-test-51`: Arch's `mutter` package does not include it;
the podman image builds Mutter with it).

## Running a nested instance

It is possible to run gnome-shell as nested instance using Devkit.

The `run-gnome-shell` script will automatically do that when run from
a graphical session, or you can run the following command:

```sh
$ dbus-run-session gnome-shell --wayland --devkit
```

## Native

Sometimes it's necessary to run the "native backend", on real display hardware.

To do that, switch to a tty and either use the `run-gnome-shell` script
or run the following command:

```sh
$ dbus-run-session gnome-shell --wayland
```

Some functionality is not available when running gnome-shell outside a GNOME
session, including logout. To exit gnome-shell, bring up the run dialog with
<kbd>Alt</kbd> <kbd>F2</kbd> and enter `debugexit`.

## Running under valgrind with a full session

Sometimes it is necessary to run gnome-shell under valgrind within a full GNOME
session. This can be achieved by overriding the `ExecStart` command of the
systemd service file used to launch gnome-shell with a drop-in config file.
Starting gnome-shell under valgrind can also take some time which requires
adjusting the timeouts of the service as well. This command can be used to
create such a drop-in file for the current user:

```sh
$ systemctl --user edit org.gnome.Shell@user.service --drop-in valgrind
```

This opens an editor in which the following content has to be added:

```ini
[Service]
ExecStart=
ExecStart=/usr/bin/valgrind --log-file=/tmp/gs-valgrind.txt --enable-debuginfod=no --leak-check=full --show-leak-kinds=definite /usr/bin/gnome-shell
TimeoutStartSec=300
TimeoutStopSec=300
```

Then the next time when logging into a session as the current user, gnome-shell
will be running under valgrind and create a log file under
`/tmp/gs-valgrind.txt`.

After ending the valgrind session and obtaining the log file, the drop-in file
needs to be removed again before starting the next session. Otherwise the log
will get overwritten. This can be done using following command from a VT:

```sh
$ systemctl --user revert org.gnome.Shell@user.service
```
