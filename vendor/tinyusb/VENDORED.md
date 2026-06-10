# vendor/tinyusb — TinyUSB, trimmed to the SAMD21 surface

Upstream: https://github.com/hathach/tinyusb @ tag **0.18.0** (shallow clone).
SAMD21 MCU pack + CMSIS pulled with `python3 tools/get_deps.py samd21`.

Used only by `app/samd21_client` (Seeeduino Xiao SAMD21, Cortex-M0+). The Pico
side does NOT use this — it's pico-sdk based.

## Trimmed
The full fetch is ~332 MB; this is cut to ~20 MB. Removed: `lib/lwip`,
`lib/FreeRTOS-Kernel`, all of `lib/CMSIS_5` except `CMSIS/Core/Include`, every
`hw/mcu` vendor except `microchip/samd21`, every `hw/bsp/<family>` dir except
`samd21` (top-level `hw/bsp/*.h` kept), and `examples/ test/ docs/ .github/`.

## Regenerate from scratch
```
git clone --depth 1 --branch 0.18.0 https://github.com/hathach/tinyusb.git vendor/tinyusb
cd vendor/tinyusb && python3 tools/get_deps.py samd21 && rm -rf .git
# then re-apply the trim above
```
Build surface (Makefile `-I` / source lists): `src/`, `hw/bsp/samd21` (+ board
`seeeduino_xiao`), `hw/mcu/microchip/samd21`, `lib/CMSIS_5/CMSIS/Core/Include`,
`tools/uf2/utils/uf2conv.py`.
