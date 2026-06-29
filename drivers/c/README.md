# slow_bus C I2C device-driver library

Portable, transport-agnostic C drivers for I2C peripherals. Each driver is the
**C twin** of a LuaJIT driver in [`../lua`](../lua) — identical register/draw
logic, kept line-for-line in sync; only the transport differs. The Lua twins run
on the Pi (over a USB→I2C bridge) for bring-up and as the reference
implementation; these C twins run **on-chip**.

> **slow_bus uses this library on the Pico.** The `bus_controller`
> (RP2040 / RP2350) binds an `i2c_bus_t` to its `i2c1` peripheral and links the
> drivers directly — see [On slow_bus](#on-slow_bus-pico) below.

Pure C11 — depends only on `<stdint.h>`/`<stddef.h>`/`<string.h>`, **no MCU SDK,
no peripheral registers**. It compiles with stock `gcc` (the test harness proves
it). That is what makes it portable to any ARM MCU.

## Layout

```
c/
├─ i2c/      i2c_bus.h          the transport contract (this is the whole port surface)
├─ pcf8563/  pcf8563.{h,c}      PCF8563 RTC
├─ ssd1306/  font5x7.h          shared 5x7 ASCII glyphs
│            ssd1306.{h,c}      SSD1306 mono OLED
└─ test/     test_c_drivers.c   mock-bus equivalence harness (no hardware)
```

## The contract: `i2c_bus_t`

Every driver touches hardware **only** through two function pointers:

```c
typedef struct i2c_bus {
    void *ctx;                                                    // your transport context
    int (*write)(void *ctx, uint8_t addr, const uint8_t *buf, size_t len, bool nostop);
    int (*read )(void *ctx, uint8_t addr, uint8_t *buf, size_t len);
} i2c_bus_t;
```

`write` returns `0` on success, `<0` on NACK/timeout. `nostop=true` holds the bus
for a repeated-START. `i2c_bus.h` also provides the `i2c_bus_write_read()` inline
(write reg pointer with `nostop`, then read) — the register-read idiom every
driver uses.

**To port the whole library to a new target you implement exactly these two
functions.** The drivers are untouched.

## Porting: provide one `i2c_bus_t`

The backend behind `write`/`read` can be anything. Three styles, increasing
indirection:

### 1. Direct HAL binding (the normal port)
Wrap the target's I2C HAL. Driver code is byte-identical across MCUs.

```c
// STM32 HAL example
static int w(void *ctx, uint8_t a, const uint8_t *b, size_t n, bool nostop) {
    uint32_t mode = nostop ? I2C_FIRST_FRAME : I2C_FIRST_AND_LAST_FRAME;
    return HAL_I2C_Master_Seq_Transmit_IT(ctx, a << 1, (uint8_t*)b, n, mode) == HAL_OK ? 0 : -1;
}
```

### 2. Blob / interpreter binding (async engine owns the bus)
`write`/`read` enqueue a transaction *descriptor* into a ring that a separate
engine drains and executes — the driver becomes pure transaction-*description*.
**slow_bus already ships such an interpreter:** `app/bus_controller/main.c`'s
`i2c_req_t` queue + `i2c_service_task` + `i2c_exec()`. Binding to it is just
"enqueue, block until it ran":

```c
static int w(void *ctx, uint8_t a, const uint8_t *b, size_t n, bool nostop) {
    i2c_req_t q = { .op = I2C_OP_WRITE, .addr = a, .wlen = n /* , .nostop = nostop */ };
    memcpy(q.data, b, n);                 // copy: caller's buffer may be a stack temp
    xQueueSend(engine_q, &q, portMAX_DELAY);
    return wait_result(q.req_id);         // block on a semaphore until the engine ran it
}
```
The engine can equally be a bytecode VM, a coprocessor, or the chain_tree/CFL
runtime — the driver never knows.

### 3. Remote / proxy binding
`write`/`read` serialize to the `CMD_I2C_*` wire protocol and ship to *another*
MCU that owns the bus — exactly what the Lua twins do over USB today. A driver on
MCU-A can drive a chip on MCU-B's bus.

## Honor the contract (the real porting work)

- **Repeated-START** — `write_read` does `write(nostop=true)` then `read` with
  **no STOP between**. The backend must support it, or every register read breaks.
- **Blocking semantics** — the API is synchronous: `write`/`read` return *after*
  the transfer. An async engine binding must block the caller until the op
  completes (queue round-trip + semaphore). Fine for these register-style drivers.
- **Buffer copy** — if the backend is async, copy `buf` before returning; the
  `const` buffer may be a caller stack temporary.
- **Large transfers** — `ssd1306_show()` issues one ~1025-byte write (`0x40` +
  1024 framebuffer bytes), assuming a chip-native master with no cap. If your
  backend has a payload cap, the **binding** must chunk it (same as the Lua
  `show()` chunks to `bus.max_write`). A direct HAL has no cap; the blob engine's
  cap is `HIL_I2C_MAX_LEN`.
- **Return codes** — NACK/timeout → negative; drivers check `== 0`.

## On slow_bus (Pico)

`bus_controller` binds the library to its `i2c1` HIL bus (GP10 SDA / GP11 SCL):

```c
#include "i2c_bus.h"
static int bc_w(void *ctx, uint8_t a, const uint8_t *b, size_t n, bool nostop) {
    int r = i2c_write_timeout_us((i2c_inst_t*)ctx, a, b, n, nostop, i2c_to_us(n));
    return r == (int)n ? 0 : -1;
}
static int bc_r(void *ctx, uint8_t a, uint8_t *b, size_t n) {
    int r = i2c_read_timeout_us((i2c_inst_t*)ctx, a, b, n, false, i2c_to_us(n));
    return r == (int)n ? 0 : -1;
}
i2c_bus_t bus = { .ctx = HIL_I2C_INST, .write = bc_w, .read = bc_r };

ssd1306_t oled;
ssd1306_init(&oled, &bus, 0, 0, 0);   // 0x3C, 128x64
ssd1306_text(&oled, 0, 0, "slow_bus");
ssd1306_show(&oled);
```

This direct-HAL path is simplest. To run the drivers off the centralized async
service instead, use the blob binding (style 2) against `i2c_req_t`. Either way
the driver source is unchanged.

## Build & test

The harness binds `i2c_bus_t` to a **mock** bus that records every byte and
serves canned reads, then asserts the drivers emit the same wire as the
hardware-verified Lua twins — **no hardware needed**:

```sh
cd drivers/c
cc -std=c11 -Wall -Wextra -Werror -Ii2c -Ipcf8563 -Issd1306 \
   test/test_c_drivers.c pcf8563/pcf8563.c ssd1306/ssd1306.c -o /tmp/tcd && /tmp/tcd
# -> 12/12 checks passed
```

Run it after any driver edit — it's the C side's anti-drift regression test.

## Adding a driver

1. `c/<chip>/<chip>.{h,c}` taking `const i2c_bus_t *` + `addr`; use only
   `i2c_bus_write` / `i2c_bus_read` / `i2c_bus_write_read`.
2. Mirror it in `lua/<chip>/<chip>.lua` (same regs, same logic).
3. Add equivalence checks to `test/test_c_drivers.c`.
