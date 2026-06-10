# ARM bare-metal toolchain (SAMD21 + RP2040)

Both firmwares are Cortex-M0+, so one `arm-none-eabi-gcc` builds both:
`app/samd21_client` (SAMD21, Makefile) and the Pico/RP2040 side.

**Pinned: xPack `arm-none-eabi-gcc` 15.2.1-1.1.** Install on every build host
(the Pi *and* any dev box) with:

```bash
bash tools/install-arm-toolchain.sh
```

It downloads the arch-correct tarball (arm64 on the Pi and the Snapdragon dev
box), extracts to `~/toolchains/`, and symlinks the tools into `~/.local/bin`.
No root, no apt. Idempotent.

## Why pinned, not apt
The Pi historically built with apt `gcc-arm-none-eabi` **8.3.1** — ancient and
per-machine, so the shipped `.uf2` drifted by host. Pinning xPack 15.2.1 makes
dev and the Pi emit comparable binaries and lets any clone reproduce the build.
The Makefile honours `CROSS ?= arm-none-eabi-`, so it just needs the pinned tool
first on `PATH`; the installer warns if an apt build in `/usr/bin` shadows it
(ensure `~/.local/bin` precedes `/usr/bin`).

## Note: build + flash run on the Pi
The Pi is the build, flash (it enumerates the USB dongles), and runtime host for
the robots. The dev box (`GE-Snapdragon`, WSL) is for editing and compile-checks
only — it has no dongles attached. Loop: edit → push → pull on the Pi → build +
flash + run there.
