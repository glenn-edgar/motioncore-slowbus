// ============================================================================
// board.h — RP2040 / Pico W board glue + pin map shared by the port layer.
//
// One bus, one tree. Bus speed and BC-master/slave role come from the flashed
// `idnt` config (idnt.sp / idnt.vr), read at boot — so the old GP0/GP1 boot
// straps are RETIRED and GP0/GP1 are now free (spare/expansion).
//
// "pico1" role: GPIO interface + I2C manager. Reduced HIL set vs. the old map —
// NO SPI, ONE I2C, ONE HIL UART, ONE PWM, ONE quadrature decoder, plus an 8-pin
// contiguous GPIO block and the 3-channel ADC analog spine. Header exposes
// GP0-GP22 + GP26/27/28; GP23/24/25/29 are CYW43-internal. See docs/README.md
// for the physical two-side layout.
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

// GP0/GP1 (header pins 1/2) are FREE — role/speed moved to the idnt config
// (idnt.vr / idnt.sp), so the old GP0/GP1 boot straps are retired. Available as
// spare/expansion GPIO (a future hwio role map may assign them).

// --- HIL GPIO block (8 pins, contiguous GP2..GP9 = header pins 4..12) -------
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

// --- I2C — single bus, i2c1 (the "I2C manager" interface) -------------------
#define HIL_PIN_I2C_SDA    10u       // i2c1 SDA, GP10 (pin 14)
#define HIL_PIN_I2C_SCL    11u       // i2c1 SCL, GP11 (pin 15)
#define HIL_I2C_INST       i2c1

// --- UART — single HIL serial, uart0 (separate from the PIO RS-485 bus) ------
#define HIL_PIN_UART_TX    12u       // uart0 TX, GP12 (pin 16)
#define HIL_PIN_UART_RX    13u       // uart0 RX, GP13 (pin 17)
#define HIL_UART_INST      uart0

// --- PWM — single channel ---------------------------------------------------
// Armed at boot at 0% duty (pin held low) until driven. 11-bit (2048 steps) to
// match the SAMD21/RA4M1 HIL command surface; divider lands a 2048-count period
// near 20 kHz. GP14 = PWM slice 7, channel A.
#define HIL_PIN_PWM0       14u       // GP14 (pin 19), slice 7A
#define HIL_PWM_FREQ_HZ    20000u
#define HIL_PWM_BITS       11u       // 2048 steps
#define HIL_PWM_TOP        2047u

// --- Quadrature decoder — single, PIO (RP2040 has no hardware QEI) -----------
#define HIL_PIN_QUAD0_A    17u       // GP17 (pin 22)
#define HIL_PIN_QUAD0_B    18u       // GP18 (pin 24)

// ADC (analog inputs; one SAR, round-robin muxed, not simultaneous)
#define HIL_PIN_ADC0       26u
#define HIL_PIN_ADC1       27u
#define HIL_PIN_ADC2       28u

// Spare/expansion GPIO: GP0, GP1, GP19, GP20, GP21, GP22 (header pins 1/2/25/26/27/29).

uint32_t board_millis(void);   // ms since boot (used by the scheduler)
void     board_init(void);
