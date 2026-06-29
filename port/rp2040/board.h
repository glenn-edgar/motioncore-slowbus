// ============================================================================
// board.h — RP2040 / Pico W board glue + pin map shared by the port layer.
//
// One bus, one tree. Bus speed and BC-master/slave role come from the flashed
// `idnt` config (idnt.sp / idnt.vr), read at boot — so the old GP0/GP1 boot
// straps are RETIRED and GP0/GP1 are now free (spare/expansion).
//
// "pico1" role: GPIO interface + I2C manager. Reduced HIL set vs. the old map —
// NO SPI, TWO I2C (polled + async), ONE HIL UART, plus an 8-pin contiguous GPIO
// block and the 3-channel ADC analog spine. PWM + quadrature were dropped to the
// Pico2 (RP2350). Header exposes GP0-GP22 + GP26/27/28; GP23/24/25/29 are
// CYW43-internal. See docs/README.md for the physical two-side layout.
// ============================================================================
#pragma once

#include <stdint.h>

// --- RS-485 bus (PIO 9-bit, no DE) ------------------------------------------
// GP15/GP16 are physically adjacent (header pins 20/21, bottom) — a clean
// transceiver pigtail. PIO maps to any GPIO, so the bus is free to sit here.
#define BUS_PIN_DI         15u       // RS-485 TX = GP15 (header pin 20, bottom-LEFT)
#define BUS_PIN_RO         16u       // RS-485 RX = GP16 (header pin 21, bottom-RIGHT)
// (no BUS_PIN_DE: auto-direction transceivers / bare TTL)

#define BUS_DEFAULT_BAUD   115200u   // fall-through default; idnt.sp selects the speed

// GP0/GP1 (header pins 1/2): freed when role/speed moved to the idnt config.
// GP0 = the interlock HARD VETO output (reserved — Thread 2 drives it; the ilcf
// DSL / hwio must not assign it elsewhere). GP1 stays spare/expansion.
#define INTERLOCK_VETO_PIN 0u        // GP0 (pin 1): interlock hard veto (GPIO, fail-safe)

// --- HIL GPIO block (8 pins, contiguous GP2..GP9 = header pins 4..12) -------
// The flexible HIL block — today GPIO / servo-bank / pulse-count, and MAY gain
// ADDITIONAL MODES later. The hwio role map (frozen config) fixes each pin's mode
// per deployment; keep that role enum append-only / extensible.
#define HIL_GPIO_BASE      2u        // first GPIO = GP2 (pin 4)
#define HIL_GPIO_COUNT     8u        // GP2..GP9
#define HIL_PIN_GPIO0      2u
#define HIL_PIN_GPIO1      3u
#define HIL_PIN_GPIO2      4u
#define HIL_PIN_GPIO3      5u
#define HIL_PIN_GPIO4      6u
#define HIL_PIN_GPIO5      7u
#define HIL_PIN_GPIO6      8u
#define HIL_PIN_GPIO7      9u

// --- I2C — TWO buses (split for safety; see docs/three-thread-design.md) -----
// ASYNC bus (i2c1, GP10/11): bench + chain-tree one-off read/write requests.
// (HIL_* names kept — the HIL I2C commands already target this bus.)
#define HIL_PIN_I2C_SDA    10u       // i2c1 SDA, GP10 (pin 14)   [ASYNC bus]
#define HIL_PIN_I2C_SCL    11u       // i2c1 SCL, GP11 (pin 15)
#define HIL_I2C_INST       i2c1
// POLLED bus (i2c0, GP20/21): periodically-sampled devices (INA219, …) → the
// shared mirror that feeds the interlock. Isolated from async traffic so a sample
// is never delayed past its freshness deadline. Device inventory = frozen config.
// (Supersedes the opt-in I2C_SELFTEST fixture that used i2c0 on GP20/21.)
#define I2C_POLLED_SDA     20u       // i2c0 SDA, GP20 (pin 26)   [POLLED bus]
#define I2C_POLLED_SCL     21u       // i2c0 SCL, GP21 (pin 27)
#define I2C_POLLED_INST    i2c0

// --- UART — single HIL serial, uart0 (separate from the PIO RS-485 bus) ------
#define HIL_PIN_UART_TX    12u       // uart0 TX, GP12 (pin 16)
#define HIL_PIN_UART_RX    13u       // uart0 RX, GP13 (pin 17)
#define HIL_UART_INST      uart0

// PWM (GP14) and the quadrature decoder (GP17/18) were DROPPED from the RP2040
// (2026-06-23) — those functions move to the Pico2 (RP2350). GP14/GP17/GP18 are
// now free (spare/expansion).

// ADC (analog inputs; one SAR, round-robin muxed, not simultaneous)
#define HIL_PIN_ADC0       26u
#define HIL_PIN_ADC1       27u
#define HIL_PIN_ADC2       28u

// Spare/expansion GPIO: GP1, GP14, GP17, GP18, GP19, GP22 (header pins 2/19/22/24/25/29).
// (GP0 = interlock veto; GP20/21 = polled-I²C bus.)

uint32_t board_millis(void);   // ms since boot (used by the scheduler)
void     board_init(void);
