// ============================================================================
// board.h — RP2350 / Pico 2 W board glue + pin map shared by the port layer.
//
// Same RS-485 bus + one-tree model as the RP2040 node ([[port/rp2040/board.h]]):
// bus speed and BC-master/slave role come from the flashed `idnt` config, read at
// boot. The RS-485 bus pins MATCH the Pico (GP15/16) so a Pico-W harness drops in.
//
// "pico2" role: motion/vibration node. Versus the RP2040 "pico1" map: NO
// configurable HIL role block (the GPIO are plain digital), NO second UART. ADDS a
// SPI master (1 CS), a quadrature ENCODER (A/B/Z, PIO), and a 20 kHz PWM. The
// 3-channel ADC is NOT generic — all 3 inputs are owned by the 20 kHz center-capture
// subsystem (sampled at the PWM-period center to dodge switching noise).
//
// Header exposes GP0-GP22 + GP26/27/28; GP23/24/25/29 are CYW43-internal (WiFi).
// (header pin numbers in comments are the 40-pin Pico 2 W layout.)
// ============================================================================
#pragma once

#include <stdint.h>

// --- RS-485 bus (PIO 9-bit, no DE) — SAME PINS AS THE PICO W -----------------
#define BUS_PIN_DI         15u       // RS-485 TX = GP15 (header pin 20)
#define BUS_PIN_RO         16u       // RS-485 RX = GP16 (header pin 21)
// (no BUS_PIN_DE: auto-direction transceivers / bare TTL)

#define BUS_DEFAULT_BAUD   115200u   // fall-through default; idnt.sp selects the speed

// --- interlock hard veto (GPIO, fail-safe) — Thread 2 owns it ---------------
#define INTERLOCK_VETO_PIN 0u        // GP0 (header pin 1)

// --- interlock INPUT (GP1, internal PULL-UP, ACTIVE-LOW) --------------------
// GP1 is the dedicated interlock input with the INTERNAL PULL-UP enabled, so the
// SAFE state is HIGH (the pull-up holds it high when idle); a device that pulls
// GP1 LOW trips the interlock -> veto (GP0). DSL: cfg (gp1):in,up; watch[gp1:1].
#define INTERLOCK_IN_PIN   1u        // GP1 (header pin 2)

// --- plain GPIO block (3 regular digital I/O, GP2..GP4 = header pins 4/5/6) --
// Plain GPIO — no configurable role/mode system (that was the RP2040 HIL block).
#define GPIO_BASE          2u
#define GPIO_COUNT         3u
#define PIN_GPIO0          2u        // GP2  (pin 4)
#define PIN_GPIO1          3u        // GP3  (pin 5)
#define PIN_GPIO2          4u        // GP4  (pin 6)

// --- quadrature encoder (PIO; A/B + index Z) --------------------------------
#define ENC_PIN_A          6u        // GP6  (pin 9)
#define ENC_PIN_B          7u        // GP7  (pin 10)
#define ENC_PIN_Z          8u        // GP8  (pin 11)  index/home pulse

// --- 20 kHz PWM (drives the ADC center-capture timing) — RELOCATED off GP10 --
// GP10/11 are the BASE async-I²C bus (see port/rp2040/board.h); leave them free so
// a node carries the base bench I²C. The PWM slice/channel are derived from the pin.
#define PWM_20K_PIN        21u       // GP21 (pin 27)  [was GP10]
#define PWM_20K_HZ         20000u

// --- I2C — base layout (the variant keeps the base's two-bus map) ------------
// i2c0 GP12/13 = the base POLLED bus; the base ASYNC bus is i2c1 on GP10/11 (now
// free here). vib_node uses neither today; defined to match the base pin map.
#define I2C_PIN_SDA        12u       // i2c0 SDA, GP12 (pin 16)  [base POLLED bus]
#define I2C_PIN_SCL        13u       // i2c0 SCL, GP13 (pin 17)
#define I2C_INST           i2c0

// --- SPI master, 1 CS (spi0, GP17..GP20 = header pins 22/24/25/26) ----------
#define SPI_PIN_CS         17u       // spi0 CSn,  GP17 (pin 22)
#define SPI_PIN_SCK        18u       // spi0 SCK,  GP18 (pin 24)
#define SPI_PIN_MOSI       19u       // spi0 TX,   GP19 (pin 25)
#define SPI_PIN_MISO       20u       // spi0 RX,   GP20 (pin 26)
#define SPI_INST           spi0

// --- ADC — all 3 channels OWNED BY THE 20 kHz CENTER-CAPTURE SUBSYSTEM -------
// Not generic ADC reads: sampled synchronously at the PWM-period center. The
// interlock/hwio generic-ADC HAL must NOT claim these.
#define ADC_PIN_CH0        26u       // ADC0, GP26 (header pin 31)
#define ADC_PIN_CH1        27u       // ADC1, GP27 (header pin 32)
#define ADC_PIN_CH2        28u       // ADC2, GP28 (header pin 34)
#define ADC_CH_COUNT       3u

// Spare/expansion GPIO: GP5, GP9, GP14, GP22 (header pins 7/12/19/29).
// (GP10/11 = base async-I²C bus; GP12/13 = base polled-I²C; GP21 = 20 kHz PWM.)
// Base alignment: GP0/GP1 interlock, GP15/16 RS-485, GP22 reserved as the base PWM-test pin.

uint32_t board_millis(void);   // ms since boot (used by the scheduler)
void     board_init(void);
