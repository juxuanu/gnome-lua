#!/bin/sh
# Run the shell built by tools/podman/build.sh nested in the current Wayland
# session (GNOME, KDE Plasma or any other Wayland compositor), from the
# project root:
#
#   tools/podman/run-nested.sh [gnome-shell options]
#   tools/podman/run-nested.sh --mode=gdm     # the login screen
#
# The container gets the session's Wayland socket, the GPU render nodes and
# the source tree, read-only. The shell runs uninstalled from build/, as the
# current user, with its own session bus, an empty system bus, an in-memory
# settings backend and a home inside the container: whatever it writes goes
# away with the container, and nothing of the host account is touched. The
# examples and the extensions placed under tools/podman/extensions/<uuid>/
# are visible to it (SHELL_LUA_EXTENSION_PATH).
#
# For the login screen the system bus gets stand-ins for GDM and
# AccountsService (tests/greeter-mock.py): it lists the current user and "guest",
# whose password is "gnome".
set -eu

IMAGE=${GNOME_LUA_IMAGE:-gnome-lua-dev}
# -t only when there is a terminal (podman warns otherwise)
TTY="-i"; [ -t 0 ] && TTY="-it"
SRC=$(cd "$(dirname "$0")/../.." && pwd)
USER_ID=$(id -u)
GROUP_ID=$(id -g)

if [ "${XDG_SESSION_TYPE:-}" != "wayland" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
  echo "run-nested.sh: a Wayland session is needed (XDG_SESSION_TYPE=$XDG_SESSION_TYPE)" >&2
  exit 1
fi
WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
RUNTIME=${XDG_RUNTIME_DIR:-/run/user/$USER_ID}
test -S "$RUNTIME/$WAYLAND_DISPLAY" || { echo "run-nested.sh: no socket $RUNTIME/$WAYLAND_DISPLAY" >&2; exit 1; }
test -f "$SRC/build/src/gnome-shell" || { echo "run-nested.sh: build first with tools/podman/build.sh" >&2; exit 1; }

mkdir -p "$SRC/tools/podman/extensions"

# The devkit backend shows the nested shell through a PipeWire stream: the
# viewer (mutter-devkit) needs a PipeWire daemon. Use the session's when it
# has one, otherwise start one inside the container.
PW=""
if [ -S "$RUNTIME/pipewire-0" ]; then
  PW="-v $RUNTIME/pipewire-0:/run/wayland/pipewire-0"
  START_PW=""
else
  START_PW="pipewire >/dev/null 2>&1 & sleep 0.5; wireplumber >/dev/null 2>&1 & sleep 0.5;"
fi

# The shell and its clients need a system bus: an empty one of their own
# (system-bus.conf), so the host's system services are out of reach.
SYSTEM_BUS='DBUS_SYSTEM_BUS_ADDRESS=$(dbus-daemon --config-file=/src/tools/podman/system-bus.conf --fork --print-address) && export DBUS_SYSTEM_BUS_ADDRESS;'

# The login screen shows its dialog only where the screen can be locked,
# which takes GDM on the system bus and systemd (its seats directory).
GREETER=""
prev=""
for arg in "$@"; do
  case "$prev $arg" in
    *" --mode=gdm" | *" --gdm-mode" | "--mode gdm") GREETER=1 ;;
  esac
  prev=$arg
done
SEATS=""
START_GREETER=""
if [ -n "$GREETER" ]; then
  SEATS="--tmpfs /run/systemd/seats"
  START_GREETER="python3 /src/tests/greeter-mock.py & gdbus wait --system --timeout 10 org.gnome.DisplayManager;"
fi

DRI=""
for d in /dev/dri/renderD* /dev/dri/card*; do
  [ -e "$d" ] && DRI="$DRI --device $d"
done

exec podman run --rm $TTY \
  --userns=keep-id --user "$USER_ID:$GROUP_ID" --security-opt label=disable \
  $DRI \
  -v "$SRC:/src:ro" -w /src \
  --tmpfs /run/wayland:rw,mode=0700,U \
  -v "$RUNTIME/$WAYLAND_DISPLAY:/run/wayland/$WAYLAND_DISPLAY" \
  $PW $SEATS \
  -e XDG_RUNTIME_DIR=/run/wayland -e WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
  -e XDG_SESSION_TYPE=wayland -e XDG_CURRENT_DESKTOP=GNOME \
  -e HOME=/work -e NO_AT_BRIDGE=1 \
  -e GSETTINGS_BACKEND=memory \
  -e GSETTINGS_SCHEMA_DIR=/src/build/data \
  -e GNOME_SHELL_DATADIR=/src/build/data \
  -e GNOME_SHELL_BUILDDIR=/src/build/src \
  -e SHELL_LUA_EXTENSION_PATH=/src/subprojects/shell-lua/examples:/src/tools/podman/extensions \
  -e LD_LIBRARY_PATH=/src/build/subprojects/shell-lua/src:/src/build/src/st:/src/build/subprojects/gvc:/src/build/src \
  ${LIBGL_ALWAYS_SOFTWARE:+-e LIBGL_ALWAYS_SOFTWARE=$LIBGL_ALWAYS_SOFTWARE} \
  "$IMAGE" \
  sh -c "$SYSTEM_BUS $START_GREETER $START_PW exec dbus-run-session build/src/gnome-shell --wayland --devkit \"\$@\"" -- "$@"
