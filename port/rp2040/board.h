// ============================================================================
// board.h — THE BASE board / pin map (Pico W AND Pico 2 W).
//
// This is the COMMON base model: the `bus_controller` image uses this map on BOTH
// the RP2040 (Pico W) and the RP2350 (Pico 2 W) — they share the same physical
// pinout (see CMakeLists BC_INC). RP2350 *variations* (motion/vibration: encoder,
// SPI, 20 kHz center-capture ADC) live in port/rp2350/board.h and layer on top of
// the spare pins; derivative builds may remap freely.
//
// Bus speed and BC-master/slave role come from the flashed `idnt` config
// (idnt.sp / idnt.vr), read at boot. Header exposes GP0-GP22 + GP26/27/28;
// GP23/24/25/29 are CYW43-internal (WiFi). See docs/README.md for the layout.
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

// --- interlock (Thread 2 owns both pins; the ilcf DSL / hwio must not reassign) -
// GP0 = hard VETO output (fail-safe). GP1 = the dedicated interlock INPUT with the
// INTERNAL PULL-UP enabled, so the SAFE state is HIGH; a device that pulls GP1 LOW
// trips the interlock. GP0 is an OPEN-DRAIN wired-OR veto with the INTERNAL pull-up
// (oc:up): OK = released (hi-Z, the internal pull-up holds the shared line HIGH; many
// nodes' pull-ups parallel — no external resistor needed), trip = driven LOW. Many
// nodes share the GP0 line; any one tripping pulls it low. Built-in slot-0 DSL:
//   gp1il;cfg[(gp1):in,up,(gp0):oc,up];watch[gp1:1];watch[_nodesdead:0];out_ok[gp0:1];out_err[gp0:0]
#define INTERLOCK_VETO_PIN 0u        // GP0 (pin 1): interlock hard veto (GPIO, fail-safe)
#define INTERLOCK_IN_PIN   1u        // GP1 (pin 2): interlock input (int. pull-up, ACTIVE-LOW)

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
// ASYNC bus (i2c1, GP10/11 = pins 14/15): bench + chain-tree one-off read/write
// requests. (HIL_* names kept — the HIL I2C commands already target this bus.)
#define HIL_PIN_I2C_SDA    10u       // i2c1 SDA, GP10 (pin 14)   [ASYNC / bench bus]
#define HIL_PIN_I2C_SCL    11u       // i2c1 SCL, GP11 (pin 15)
#define HIL_I2C_INST       i2c1
// POLLED bus (i2c0, GP12/13 = pins 16/17): periodically-sampled devices (INA219, …)
// → the shared mirror that feeds the interlock. Isolated from async traffic so a
// sample is never delayed past its freshness deadline. Device inventory = frozen config.
#define I2C_POLLED_SDA     12u       // i2c0 SDA, GP12 (pin 16)   [POLLED bus]
#define I2C_POLLED_SCL     13u       // i2c0 SCL, GP13 (pin 17)
#define I2C_POLLED_INST    i2c0

// --- PWM test output (GP22 = pin 29) ----------------------------------------
// A settable-frequency/duty PWM, mainly a TEST stimulus: feed it (via an RC
// low-pass for a DC level, or raw for an AC/known-frequency source) into an ADC
// input, or loop it into a digital/counter input for a no-external-gear self-test.
#define PWM_TEST_PIN       22u       // GP22 (pin 29)
#define PWM_TEST_DEFAULT_HZ 1000u    // boot default; runtime-settable via bench command

// ADC (analog inputs; one SAR, round-robin muxed, not simultaneous)
#define HIL_PIN_ADC0       26u
#define HIL_PIN_ADC1       27u
#define HIL_PIN_ADC2       28u

// Spare/expansion GPIO: GP14, GP17, GP18, GP19, GP20, GP21 (header pins 19/22/24/25/26/27).
// (GP0/1 = interlock; GP10/11 = async-I²C; GP12/13 = polled-I²C; GP15/16 = bus; GP22 = PWM-test.)
// NOTE: the old HIL UART (GP12/13) is retired — those pins are the polled-I²C bus now.

uint32_t board_millis(void);   // ms since boot (used by the scheduler)
void     board_init(void);
