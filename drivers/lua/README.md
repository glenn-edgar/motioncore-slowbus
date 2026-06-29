# slow_bus LuaJIT I2C device-driver library

Host-side (Raspberry Pi) LuaJIT drivers for I2C peripherals. Each driver is the
**Lua twin** of a C driver in [`../c`](../c) — identical register/draw logic,
kept line-for-line in sync; only the transport differs. These twins are the
**bring-up and reference implementation**: they run on the Pi over a USB→I2C
front-end, get hardware-verified there, and the C twins are checked to emit the
same wire bytes (see [`../c/README.md`](../c/README.md) and the equivalence
harness `../c/test/test_c_drivers.c`).

> Porting, the `i2c_bus_t` contract, and how the C drivers run on-chip
> (including the slow_bus Pico binding) are documented in
> **[`../c/README.md`](../c/README.md)**.

Runs under stock `luajit` on the Pi; the only dependency is `libcomm.lua`
(vendored under `tools/commission/lua`), resolved automatically by each script.

## Layout

```
lua/
├─ i2c/      i2c_host.lua       the transport: I2C bus over a USB->I2C front-end
├─ pcf8563/  pcf8563.lua        PCF8563 RTC
├─ ssd1306/  font5x7.lua        shared 5x7 ASCII glyphs (byte-identical to ../c)
│            ssd1306.lua        SSD1306 mono OLED
└─ demo/     demo.lua           live RTC -> OLED clock (composes both drivers)
```

## The transport: `i2c_host.lua`

The Lua mirror of the C `i2c_bus_t` handle — the same three primitives
(`bus:write`, `bus:read`, `bus:write_read`), but carried over USB-CDC by
libcomm's `OP_SHELL_EXEC` to a controller that owns the actual I2C bus. Two
USB→I2C front-ends sit behind one interface, selected at `open`:

| `kind`   | controller                          | opcodes        | `write_read` framing      | caps (w/r) |
|----------|-------------------------------------|----------------|---------------------------|------------|
| `pico`   | `app/bus_controller` (RP2040)       | `0x010C–010F`  | `[addr, rlen, wdata…]`    | 63 / 64    |
| `samd21` | `app/samd21_client` USB↔I2C bridge  | `0x0130–0133`  | `[addr, wlen, rlen, wdata…]` | 32 / 60 |

Only the opcodes, `write_read` framing, and transfer caps differ — the device
drivers above never see it. `bus.max_write` is exposed so a driver can chunk
large transfers (e.g. the OLED framebuffer) to whatever the front-end allows.

```lua
local i2c = require("i2c_host")
local bus = i2c.open("/dev/ttyACM0", "samd21")   -- or "pico" (default)
for _, a in ipairs(bus:scan()) do print(string.format("0x%02X", a)) end
```

## Running (on the Pi)

Chips live on the Pi (`ssh robot`, on `/dev/ttyACM0`). The XIAO expansion board
carries the RTC (`0x51`) and OLED (`0x3C`). Each driver is a module *and* a
self-test CLI:

```sh
cd ~/slow_bus/drivers
luajit lua/i2c/i2c_host.lua     --port /dev/ttyACM0 --kind samd21   # bus scan
luajit lua/pcf8563/pcf8563.lua  --port /dev/ttyACM0 --kind samd21   # RTC API self-test (9/9)
luajit lua/ssd1306/ssd1306.lua  --port /dev/ttyACM0 --kind samd21   # OLED visual self-test
luajit lua/demo/demo.lua        --port /dev/ttyACM0 --kind samd21   # live RTC -> OLED clock
```

`demo.lua` extras: `--set` re-syncs the RTC from the host clock (auto on lost
power), `--secs N` runs N seconds then clears and exits.

## Mirror discipline

- `font5x7.lua` and `../c/ssd1306/font5x7.h` carry **byte-identical** data
  (parity-checked in the C harness).
- Keep method/field names and the per-register logic matched to the C twin; the
  only intended divergence is `ssd1306.lua:show()`, which **chunks** the
  framebuffer to `bus.max_write` (a USB-shell framing limit), where the C twin
  sends it in one chip-native write. The limit lives where the constraint is.

## Adding a driver

1. `lua/<chip>/<chip>.lua` taking an `i2c_host` `bus` + `addr`; use only
   `bus:write` / `bus:read` / `bus:write_read`.
2. Mirror it in `../c/<chip>/<chip>.{h,c}` (same regs, same logic).
3. Add equivalence checks to `../c/test/test_c_drivers.c`.
