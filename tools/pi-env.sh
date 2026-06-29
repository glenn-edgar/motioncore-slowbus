# pi-env.sh — Pico build environment on the Pi (ssh host `robot`, 192.168.1.66).
#
# Source before any cmake invocation:   . tools/pi-env.sh
#
# Post-SD-crash layout (2026-06-23): all build software lives on the SSD at
# /mnt/ssd, with ~/pico -> /mnt/ssd/pico symlinked, so the $HOME/pico paths below
# resolve. The arm-none-eabi toolchain is now the SSD's arm-gnu-toolchain-14.2
# (the system apt arm-gcc was NOT reinstalled), so it MUST be put on PATH here.
# cmake 4.x is in ~/.local/bin (system cmake 3.18 needs the policy shim in
# pi-build.sh). Mirrors /mnt/ssd/pico/env.sh — keep the two in sync.
export PICO_SDK_PATH=/home/pi/pico/pico-sdk
export FREERTOS_KERNEL_PATH=/home/pi/pico/FreeRTOS-Kernel
export PICO_TOOLCHAIN_PATH=/home/pi/pico/arm-gnu-toolchain-14.2.rel1-aarch64-arm-none-eabi

# Toolchain bin + ~/.local/bin (cmake 4.x) ahead of the system dirs.
export PATH="$PICO_TOOLCHAIN_PATH/bin:$HOME/.local/bin:$PATH"
