#!/bin/sh
# Run the shell built by tools/podman/build.sh nested in the current Wayland
# session (GNOME, KDE Plasma or any other Wayland compositor), from the
# project root:
#
#   tools/podman/run-nested.sh [gnome-shell options]
#
# The container gets the session's Wayland socket, the GPU render nodes and
# the source tree; the shell runs uninstalled from build/ with its own session
# bus, an in-memory settings backend and a throw-away data home, so nothing
# of the host account is touched. Extensions placed under
# tools/podman/home/gnome-shell/extensions/<uuid>/ are visible to it, as are
# the examples (SHELL_LUA_EXTENSION_PATH).
set -eu

IMAGE=${GNOME_LUA_IMAGE:-gnome-lua-dev}
# -t only when there is a terminal (podman warns otherwise)
TTY="-i"; [ -t 0 ] && TTY="-it"
SRC=$(cd "$(dirname "$0")/../.." && pwd)

if [ "${XDG_SESSION_TYPE:-}" != "wayland" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
  echo "run-nested.sh: a Wayland session is needed (XDG_SESSION_TYPE=$XDG_SESSION_TYPE)" >&2
  exit 1
fi
WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
RUNTIME=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
test -S "$RUNTIME/$WAYLAND_DISPLAY" || { echo "run-nested.sh: no socket $RUNTIME/$WAYLAND_DISPLAY" >&2; exit 1; }
test -f "$SRC/build/src/gnome-shell" || { echo "run-nested.sh: build first with tools/podman/build.sh" >&2; exit 1; }

mkdir -p "$SRC/tools/podman/home/gnome-shell/extensions"

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

DRI=""
for d in /dev/dri/renderD* /dev/dri/card*; do
  [ -e "$d" ] && DRI="$DRI --device $d"
done

exec podman run --rm $TTY \
  --userns=keep-id --security-opt label=disable \
  $DRI \
  -v "$SRC:/src" -w /src \
  -v "$RUNTIME/$WAYLAND_DISPLAY:/run/wayland/$WAYLAND_DISPLAY" \
  $PW \
  -e XDG_RUNTIME_DIR=/run/wayland -e WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
  -e XDG_SESSION_TYPE=wayland -e XDG_CURRENT_DESKTOP=GNOME \
  -e HOME=/work -e NO_AT_BRIDGE=1 \
  -e XDG_DATA_HOME=/src/tools/podman/home \
  -e GSETTINGS_BACKEND=memory \
  -e GSETTINGS_SCHEMA_DIR=/src/build/data \
  -e GNOME_SHELL_DATADIR=/src/build/data \
  -e GNOME_SHELL_BUILDDIR=/src/build/src \
  -e SHELL_LUA_EXTENSION_PATH=/src/subprojects/shell-lua/examples \
  -e LD_LIBRARY_PATH=/src/build/subprojects/shell-lua/src:/src/build/src/st:/src/build/subprojects/gvc:/src/build/src \
  ${LIBGL_ALWAYS_SOFTWARE:+-e LIBGL_ALWAYS_SOFTWARE=$LIBGL_ALWAYS_SOFTWARE} \
  "$IMAGE" \
  sh -c "chmod 700 /run/wayland 2>/dev/null; $START_PW exec dbus-run-session build/src/gnome-shell --wayland --devkit \"\$@\"" -- "$@"
