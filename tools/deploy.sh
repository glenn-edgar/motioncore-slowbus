#!/usr/bin/env bash
# deploy.sh — FROM THE DEV BOX: sync this tree to the Pi, build there, optionally
# flash. The Pi (ssh host `robot`) is the build+flash+run host; this box only
# edits.
#   usage: tools/deploy.sh [target] [--flash]
#          tools/deploy.sh bus_controller            # sync + build
#          tools/deploy.sh bus_controller --flash    # + picotool load (Pico in BOOTSEL)
#
# Non-destructive sync (no --delete); excludes .git and build/ so the Pi keeps
# its configured build tree. picotool is RP-only, so --flash never disturbs the
# SAMD21 (xiao_blocks) USB-CDC ports sharing the bus.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
target="bus_controller"; flash=0
for a in "$@"; do
    case "$a" in
        --flash) flash=1 ;;
        -*)      echo "unknown flag: $a" >&2; exit 2 ;;
        *)       target="$a" ;;
    esac
done

echo "[deploy] rsync $here/ -> robot:slow_bus/"
rsync -az --exclude '.git' --exclude 'build' "$here/" robot:slow_bus/

echo "[deploy] build '$target' on robot"
ssh robot "bash -lc 'slow_bus/tools/pi-build.sh $target'"

if [ "$flash" -eq 1 ]; then
    echo "[deploy] flash '$target' (Pico must be in BOOTSEL)"
    ssh robot "picotool load -x slow_bus/build/$target.uf2"
fi
echo "[deploy] done."
