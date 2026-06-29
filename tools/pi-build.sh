#!/usr/bin/env bash
# pi-build.sh — configure (if needed) + build a slow_bus target ON THE PI.
#   usage: tools/pi-build.sh [target]      (default: bus_controller)
#
# Run on the robot host. Sources pi-env.sh, does a one-time configure on a fresh
# build/ (with the cmake-policy shim the old-ish SDK needs), then builds. Adding
# sources to CMakeLists triggers an automatic reconfigure on the next build.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$here/tools/pi-env.sh"
target="${1:-bus_controller}"

if [ ! -f "$here/build/CMakeCache.txt" ]; then
    cmake -S "$here" -B "$here/build" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DPICO_BOARD=pico_w
fi
cmake --build "$here/build" --target "$target"

uf2="$here/build/$target.uf2"
echo "built: $uf2"
ls -l "$uf2"
