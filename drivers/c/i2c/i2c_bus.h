#ifndef SLOW_BUS_I2C_BUS_H
#define SLOW_BUS_I2C_BUS_H
// Transport-agnostic I2C bus handle shared by every C device driver.
//
// This is the C half of the driver-library mirror: it exposes the SAME three
// primitives the host-side Lua `i2c_host.lua` Bus object does (write / read /
// write_read), so a chip driver's register logic is identical in both languages
// and ONLY the transport differs. A driver depends only on this handle -- never
// on the Pico SDK -- so the same .c compiles into the bus controller, a slave
// node, or a host unit test.
//
// An app binds the function pointers to its transport. On the bus controller:
//   static int bc_w(void *ctx, uint8_t a, const uint8_t *b, size_t n, bool nostop) {
//       int r = i2c_write_timeout_us((i2c_inst_t*)ctx, a, b, n, nostop, i2c_to_us(n));
//       return r == (int)n ? 0 : -1; }
//   static int bc_r(void *ctx, uint8_t a, uint8_t *b, size_t n) {
//       int r = i2c_read_timeout_us((i2c_inst_t*)ctx, a, b, n, false, i2c_to_us(n));
//       return r == (int)n ? 0 : -1; }
//   i2c_bus_t bus = { .ctx = HIL_I2C_INST, .write = bc_w, .read = bc_r };
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct i2c_bus {
    void *ctx;   // transport context (e.g. an i2c_inst_t *)
    // Write `len` bytes to 7-bit `addr`. nostop=true holds the bus for a
    // repeated-START (the register-read idiom). Returns 0 on success, <0 on
    // NACK / timeout.
    int (*write)(void *ctx, uint8_t addr, const uint8_t *buf, size_t len, bool nostop);
    // Read `len` bytes from 7-bit `addr`. Returns 0 on success, <0 on error.
    int (*read)(void *ctx, uint8_t addr, uint8_t *buf, size_t len);
} i2c_bus_t;

static inline int i2c_bus_write(const i2c_bus_t *bus, uint8_t addr,
                                const uint8_t *buf, size_t len) {
    return bus->write(bus->ctx, addr, buf, len, false);
}
static inline int i2c_bus_read(const i2c_bus_t *bus, uint8_t addr,
                               uint8_t *buf, size_t len) {
    return bus->read(bus->ctx, addr, buf, len);
}
// Write `wbuf` (no STOP) then repeated-START read `rbuf` -- write the register
// pointer, read N. The mirror of Lua `bus:write_read(addr, wdata, n)`.
static inline int i2c_bus_write_read(const i2c_bus_t *bus, uint8_t addr,
                                     const uint8_t *wbuf, size_t wlen,
                                     uint8_t *rbuf, size_t rlen) {
    int r = bus->write(bus->ctx, addr, wbuf, wlen, true);
    if (r < 0) return r;
    return bus->read(bus->ctx, addr, rbuf, rlen);
}

#endif // SLOW_BUS_I2C_BUS_H
