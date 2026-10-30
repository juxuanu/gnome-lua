#!/bin/sh
# Build gnome-lua in the CI container image with podman, from the project root:
#
#   tools/podman/build.sh                 # image (once) + meson setup + compile
#   tools/podman/build.sh meson test -C build   # any command inside the image
#
# The source tree is mounted at /src; the build directory is build/ in the
# tree, as on the host. The image is rebuilt only when it is missing. Image
# builds, compiles and tests run at the lowest CPU and I/O priority, so the
# desktop stays responsive; GNOME_LUA_LOW_PRIORITY=0 leaves the priority as is.
set -eu

IMAGE=${GNOME_LUA_IMAGE:-gnome-lua-dev}
# -t only when there is a terminal (podman warns otherwise)
TTY="-i"; [ -t 0 ] && TTY="-it"
SRC=$(cd "$(dirname "$0")/../.." && pwd)
# Niceness 19 whatever the starting one (the kernel caps it), idle I/O class
LOW="nice -n 39 ionice -c 3"
[ "${GNOME_LUA_LOW_PRIORITY:-1}" = 0 ] && LOW=""

if ! podman image exists "$IMAGE"; then
  $LOW podman build -t "$IMAGE" -f "$SRC/tools/podman/Containerfile" "$SRC/tools/podman"
fi

run() {
  podman run --rm $TTY \
    --userns=keep-id --security-opt label=disable \
    --pids-limit=-1 \
    -v "$SRC:/src" -w /src \
    -e HOME=/work -e NO_AT_BRIDGE=1 \
    "$IMAGE" $LOW "$@"
}

if [ $# -eq 0 ]; then
  run sh -c 'test -f build/build.ninja || meson setup build -Dbuildtype=debugoptimized -Dman=false -Dgtk_doc=false -Dintrospection=false; meson compile -C build'
else
  run "$@"
fi
