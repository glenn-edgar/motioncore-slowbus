#ifndef SLOW_BUS_PCF8563_H
#define SLOW_BUS_PCF8563_H
// PCF8563 real-time clock driver (C). Mirror of host-side `pcf8563.lua`.
//
// The PCF8563 (XIAO expansion board RTC, fixed address 0x51) is a BCD clock:
//   0x00 CTRL1   0x01 CTRL2
//   0x02 SECONDS (bit7 VL = clock-integrity-lost since power-up)
//   0x03 MINUTES 0x04 HOURS 0x05 DAYS 0x06 WEEKDAYS
//   0x07 CENTURY/MONTHS (bit7 century)  0x08 YEARS
// Time is read in one burst (write reg ptr 0x02, read 7) and set in one write
// (reg 0x02 + 7 BCD bytes), so the registers stay coherent.
#include "i2c_bus.h"

#define PCF8563_ADDR  0x51u

typedef struct {
    uint8_t  sec;     // 0..59
    uint8_t  min;     // 0..59
    uint8_t  hour;    // 0..23
    uint8_t  mday;    // 1..31
    uint8_t  wday;    // 0..6 (application-defined; the chip just stores it)
    uint8_t  month;   // 1..12
    uint16_t year;    // full year, e.g. 2026
} pcf8563_time_t;

// CTRL1=0 (run, normal mode), CTRL2=0 (no alarm/timer interrupts). Returns true
// on bus success.
bool pcf8563_init(const i2c_bus_t *bus, uint8_t addr);

// Read the VL (voltage-low) flag: true => the clock lost integrity since last
// power-up and the time is not trustworthy until set.
bool pcf8563_lost_power(const i2c_bus_t *bus, uint8_t addr, bool *vl);

// Read the current time. Returns true on bus success.
bool pcf8563_get(const i2c_bus_t *bus, uint8_t addr, pcf8563_time_t *t);

// Set the time. Writing the seconds register clears VL. Returns true on success.
bool pcf8563_set(const i2c_bus_t *bus, uint8_t addr, const pcf8563_time_t *t);

#endif // SLOW_BUS_PCF8563_H
