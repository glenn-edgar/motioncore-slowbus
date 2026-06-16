// PCF8563 RTC driver (C). Mirror of `pcf8563.lua` -- identical register logic,
// only the transport (i2c_bus_t) differs from the Lua Bus object.
#include "pcf8563.h"

enum {
    REG_CTRL1   = 0x00,
    REG_CTRL2   = 0x01,
    REG_SECONDS = 0x02,   // bit7 = VL
};

#define VL_BIT  0x80u

static uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10u + (b & 0x0Fu)); }
static uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10u) << 4) | (d % 10u)); }

bool pcf8563_init(const i2c_bus_t *bus, uint8_t addr) {
    uint8_t w[3] = { REG_CTRL1, 0x00u, 0x00u };   // CTRL1=0, CTRL2=0 (auto-advances)
    return i2c_bus_write(bus, addr, w, sizeof w) == 0;
}

bool pcf8563_lost_power(const i2c_bus_t *bus, uint8_t addr, bool *vl) {
    uint8_t reg = REG_SECONDS, sec = 0;
    if (i2c_bus_write_read(bus, addr, &reg, 1, &sec, 1) != 0) return false;
    *vl = (sec & VL_BIT) != 0;
    return true;
}

bool pcf8563_get(const i2c_bus_t *bus, uint8_t addr, pcf8563_time_t *t) {
    uint8_t reg = REG_SECONDS, r[7];
    if (i2c_bus_write_read(bus, addr, &reg, 1, r, sizeof r) != 0) return false;
    t->sec   = bcd2dec(r[0] & 0x7Fu);   // mask VL
    t->min   = bcd2dec(r[1] & 0x7Fu);
    t->hour  = bcd2dec(r[2] & 0x3Fu);
    t->mday  = bcd2dec(r[3] & 0x3Fu);
    t->wday  = (uint8_t)(r[4] & 0x07u);
    t->month = bcd2dec(r[5] & 0x1Fu);
    t->year  = (uint16_t)(2000u + bcd2dec(r[6]));
    return true;
}

bool pcf8563_set(const i2c_bus_t *bus, uint8_t addr, const pcf8563_time_t *t) {
    uint8_t w[8] = {
        REG_SECONDS,                       // writing seconds clears VL
        dec2bcd(t->sec),
        dec2bcd(t->min),
        dec2bcd(t->hour),
        dec2bcd(t->mday),
        (uint8_t)(t->wday & 0x07u),
        dec2bcd(t->month),
        dec2bcd((uint8_t)(t->year % 100u)),
    };
    return i2c_bus_write(bus, addr, w, sizeof w) == 0;
}
