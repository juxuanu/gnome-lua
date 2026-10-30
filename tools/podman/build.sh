#!/bin/sh
# Build gnome-lua in the CI container image with podman, from the project root:
#
#   tools/podman/build.sh                 # image (once) + meson setup + compile
#   tools/podman/build.sh meson test -C build   # any command inside the image
#
# The source tree is mounted at /src; the build directory is build/ in the
# tree, as on the host. The image is rebuilt only when it is missing.
set -eu

IMAGE=${GNOME_LUA_IMAGE:-gnome-lua-dev}
# -t only when there is a terminal (podman warns otherwise)
TTY="-i"; [ -t 0 ] && TTY="-it"
SRC=$(cd "$(dirname "$0")/../.." && pwd)

if ! podman image exists "$IMAGE"; then
  podman build -t "$IMAGE" -f "$SRC/tools/podman/Containerfile" "$SRC/tools/podman"
fi

run() {
  podman run --rm $TTY \
    --userns=keep-id --security-opt label=disable \
    --pids-limit=-1 \
    -v "$SRC:/src" -w /src \
    -e HOME=/work -e NO_AT_BRIDGE=1 \
    "$IMAGE" "$@"
}

if [ $# -eq 0 ]; then
  run sh -c 'test -f build/build.ninja || meson setup build -Dbuildtype=debugoptimized -Dman=false -Dgtk_doc=false -Dintrospection=false; meson compile -C build'
else
  run "$@"
fi
