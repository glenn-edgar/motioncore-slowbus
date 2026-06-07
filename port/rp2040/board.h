// ============================================================================
// board.h — RP2040 / Pico W board glue + pin map shared by the port layer.
//
// The slow bus runs at a FIXED 115200 baud (it must match the SAMD21 slaves,
// which never change). Auto-direction RS-485 transceivers are used, so there is
// NO DE pin — the sacrificial 0xFF preamble covers the TX turnaround/settle.
//
// Full HIL pin map (the Pico W is both BC and slave, single tree, so it carries
// the whole peripheral set). Header exposes GP0-GP22 + GP26/27/28; GP23/24/25/29
// are CYW43-internal. See docs/README.md for the physical two-side layout.
// ============================================================================
#pragma once

#include <stdint.h>

// --- RS-485 bus (PIO 9-bit, fixed 115200, no DE) ----------------------------
// GP20/GP21 are physically adjacent (header pins 26/27) — a clean transceiver
// pigtail. PIO maps to any GPIO, so the bus is free to sit here.
// Bottom corner pins, mirroring the XIAO's D6(left)/D7(right) bottom layout for
// a clean across-the-bench cross-wire (PIO maps to any GPIO).
#define BUS_PIN_DI         15u       // RS-485 TX = GP15 (header pin 20, bottom-LEFT)
#define BUS_PIN_RO         16u       // RS-485 RX = GP16 (header pin 21, bottom-RIGHT)
// (no BUS_PIN_DE: auto-direction transceivers / bare TTL)

#define BUS_DEFAULT_BAUD   115200u   // fixed forever for the slow bus

// --- HIL peripherals --------------------------------------------------------
// I2C0
#define HIL_PIN_I2C_SDA    4u
#define HIL_PIN_I2C_SCL    5u

// SPI0 — one contiguous block GP14..GP19 (3 signals + 3 chip-selects).
// Signal pins are SPI0-mux-locked (MISO=GP16, SCK=GP18, MOSI=GP19); the three
// CS lines are plain software-driven GPIO (3 devices share the bus), filling the
// gaps so all six SPI pins sit together.
#define HIL_PIN_SPI_CS0    14u
#define HIL_PIN_SPI_CS1    15u
#define HIL_PIN_SPI_MISO   16u
#define HIL_PIN_SPI_CS2    17u
#define HIL_PIN_SPI_SCK    18u
#define HIL_PIN_SPI_MOSI   19u

// Quadrature decoders (PIO — RP2040 has no hardware QEI)
#define HIL_PIN_QUAD0_A    10u
#define HIL_PIN_QUAD0_B    11u
#define HIL_PIN_QUAD1_A    12u
#define HIL_PIN_QUAD1_B    13u

// PWM: both run at 20 kHz, armed at boot at 0% duty (pin held low) until driven.
// 11-bit resolution (2048 steps) to match the SAMD21/RA4M1 HIL command surface;
// divider chosen so a 2048-count period lands ~20 kHz. GP0/GP1 = PWM slice 0,
// channels A/B — same slice (both 20 kHz), independent duty.
#define HIL_PIN_PWM0       0u         // slice 0A
#define HIL_PIN_PWM1       1u         // slice 0B
#define HIL_PWM_FREQ_HZ    20000u
#define HIL_PWM_BITS       11u        // 2048 steps
#define HIL_PWM_TOP        2047u

// ADC (analog inputs; one SAR, round-robin muxed, not simultaneous)
#define HIL_PIN_ADC0       26u
#define HIL_PIN_ADC1       27u
#define HIL_PIN_ADC2       28u

// Free GPIO pool: GP2, GP3, GP6, GP7, GP8, GP9, GP22 (7 pins).

uint32_t board_millis(void);   // ms since boot (used by the scheduler)
void     board_init(void);
