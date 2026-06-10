#!/usr/bin/env bash
# install-arm-toolchain.sh — pin the bare-metal ARM toolchain for this repo.
#
# Installs xPack arm-none-eabi-gcc PINNED at 15.2.1-1.1 into ~/toolchains and
# symlinks the tools into ~/.local/bin (no root, no apt). One Cortex-M0+
# toolchain serves both the SAMD21 (app/samd21_client) and RP2040 builds.
#
# Run on every build host (the Pi, and any dev box):
#     bash tools/install-arm-toolchain.sh
# Idempotent: re-running verifies/repairs the install.
#
# Why pinned, not apt: apt's gcc-arm-none-eabi is ancient (8.3.1 on the Pi) and
# per-machine, so the shipped binary drifts by host. Pinning makes dev and the
# Pi emit comparable output and lets any clone reproduce the build.
set -euo pipefail

VER="15.2.1-1.1"
PREFIX="$HOME/toolchains"
DEST="$PREFIX/xpack-arm-none-eabi-gcc-$VER"
BINLINK="$HOME/.local/bin"
BASEURL="https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v$VER"

case "$(uname -m)" in
  aarch64|arm64) ARCH="linux-arm64" ;;          # the Pi and the Snapdragon dev box
  x86_64|amd64)  ARCH="linux-x64"   ;;
  *) echo "unsupported arch: $(uname -m)" >&2; exit 1 ;;
esac
TARBALL="xpack-arm-none-eabi-gcc-$VER-$ARCH.tar.gz"

if [ ! -x "$DEST/bin/arm-none-eabi-gcc" ]; then
  echo ">> fetching $TARBALL"
  mkdir -p "$PREFIX"
  tmp="$(mktemp)"
  curl -fL "$BASEURL/$TARBALL" -o "$tmp"
  tar -xzf "$tmp" -C "$PREFIX"
  rm -f "$tmp"
else
  echo ">> $DEST already present"
fi

mkdir -p "$BINLINK"
ln -sfn "$DEST"/bin/arm-none-eabi-* "$BINLINK"/
echo ">> symlinked $(ls "$DEST"/bin/arm-none-eabi-* | wc -l) tools into $BINLINK"

# Verify the pinned version is the one that actually resolves on PATH.
resolved="$(command -v arm-none-eabi-gcc || true)"
if [ -z "$resolved" ]; then
  echo "!! arm-none-eabi-gcc not on PATH — add to your shell rc:"
  echo "     export PATH=\"\$HOME/.local/bin:\$PATH\""
  exit 1
fi
got="$("$resolved" -dumpversion 2>/dev/null || echo '?')"
echo ">> resolves: $resolved (gcc $got)"
if [ "$got" != "15.2.1" ]; then
  echo "!! a DIFFERENT arm-none-eabi-gcc shadows the pinned one (likely apt 8.3.1 in /usr/bin)."
  echo "   ensure \$HOME/.local/bin precedes /usr/bin on PATH so 15.2.1 wins."
  exit 1
fi
echo ">> OK — pinned arm-none-eabi-gcc 15.2.1 active."
