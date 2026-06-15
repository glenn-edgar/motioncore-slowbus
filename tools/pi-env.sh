# pi-env.sh — Pico build environment on the Pi (ssh host `robot`, 192.168.1.66).
#
# The Pico SDK + FreeRTOS kernel live under ~/pico and are NOT exported in the
# login shell, and the usable cmake (4.x) is in ~/.local/bin (system cmake is
# too old). Source this before any cmake invocation:   . tools/pi-env.sh
#
# Pi-specific paths — this file is for the robot host, not the dev box.
export PICO_SDK_PATH=/home/pi/pico/pico-sdk
export FREERTOS_KERNEL_PATH=/home/pi/pico/FreeRTOS-Kernel

# ~/.local/bin first: gets cmake 4.x (system cmake 3.18 is too old) and the
# pinned xPack arm-gcc if/when installed (else /usr/bin arm-gcc 8.3.1 is used).
export PATH="$HOME/.local/bin:$PATH"
