// ============================================================================
// boot_hwio.h — read the 'hwio' config file at boot: the FROZEN pin-role map
// for the flexible HIL block (GP2..GP9) + the ADC channel annotation (GP26/27/28).
//
// Hardware is frozen at config time (see docs/three-thread-design.md). 'hwio'
// fixes, per deployment, what each HIL pin IS — input / output / servo /
// pulse-count — and a label + integer scale for each ADC channel. Runtime
// commands then OPERATE these pins; they never reconfigure them. The old runtime
// CMD_GPIO_CONFIG role-switch is superseded by this file.
//
// Sibling to 'idnt' in the read-only config-FS (one 256-B store row, CBOR body).
// MISSING is not an error: absent 'hwio' -> every HIL pin defaults to the
// fail-safe UNUSED role (hi-Z input, no pull) and ADC channels are unlabeled.
//
// hwio shape (schema v2 — see docs/io-mode-model.md):
//   { "v":2,
//     "m":<io_mode>,                              # HWIO_MODE_* for the GP2..GP9 block
//     "io":[ b0,b1,b2,b3,b4,b5,b6,b7 ],          # per-pin sub-config byte, meaning per io_mode
//     "ad":[ {"l":<label>,"n":<num>,"d":<den>,"u":<unit>}, ... up to 3 ] }
// The whole block takes ONE io_mode (frozen, like the SAMD21 REG_MODE). Per-pin
// byte meaning:
//   GPIO    : (debounce_depth<<4) | role     role = HWIO_ROLE_* (0..6)
//   COUNTER : bit0 enable | bits1-2 pull | bits3-4 edge
//   SERVO   : bit0 enable (channel; contiguous from GP2)
// 'io' entries beyond the block fail the decode. 'ad' and every field inside it
// are optional (append-only forward-compat). MISSING file -> GPIO / all-UNUSED.
// ============================================================================
#pragma once

#include <stdint.h>
#include "board.h"   // HIL_GPIO_COUNT

// Block I/O mode (whole GP2..GP9 block, frozen at commission). Values mirror the
// SAMD21 REG_MODE so one host driver spans both platforms. The Pico uses GPIO /
// COUNTER / SERVO (+ ADC always on); ADC/MIXED/HIPERF are SAMD21-only here.
enum {
    HWIO_MODE_IDLE    = 0,
    HWIO_MODE_GPIO    = 1,   // GP2..9 as GPIO (+ per-pin debounce)
    HWIO_MODE_ADC     = 2,   // (SAMD21-only) — Pico ADC is always on separately
    HWIO_MODE_MIXED   = 3,   // (SAMD21-only)
    HWIO_MODE_SERVO   = 4,   // GP2..9 as the PIO servo bank
    HWIO_MODE_COUNTER = 5,   // GP2..9 as 1 kHz-sampled edge counters
    HWIO_MODE_HIPERF  = 6,   // (SAMD21-only)
    HWIO_MODE__MAX
};

// GPIO-mode pin roles (low nibble of the per-pin byte). APPEND-ONLY. UNUSED = 0 so
// an all-zero / missing map is fail-safe. OC/OC_PU are open-drain outputs (drive
// low on write, else released hi-Z + optional pull-up).
enum {
    HWIO_ROLE_UNUSED         = 0,   // hi-Z input, pulls disabled (safe default)
    HWIO_ROLE_INPUT          = 1,   // plain input, no pull
    HWIO_ROLE_INPUT_PULLUP   = 2,
    HWIO_ROLE_INPUT_PULLDOWN = 3,
    HWIO_ROLE_OUTPUT         = 4,   // push-pull, driven low at config time
    HWIO_ROLE_OC             = 5,   // open-drain, released (hi-Z) at config time
    HWIO_ROLE_OC_PU          = 6,   // open-drain + internal pull-up
    HWIO_ROLE__MAX                  // exclusive upper bound for range-checks
};

enum {
    HWIO_OK          =  0,
    HWIO_ERR_MISSING = -1,   // no 'hwio' file -> caller uses all-UNUSED defaults
    HWIO_ERR_FORMAT  = -2,   // CBOR malformed / required field absent
    HWIO_ERR_SCHEMA  = -3,   // schema_ver mismatch
    HWIO_ERR_ROLE    = -4,   // a pin role is out of range / too many io entries
};

#define HWIO_ADC_NCH      3        // GP26/27/28
#define HWIO_LABEL_MAX    16       // incl. NUL
#define HWIO_UNIT_MAX     8        // incl. NUL

typedef struct {
    char     label[HWIO_LABEL_MAX]; // ADC channel label (e.g. "vbus"); "" if absent
    char     unit[HWIO_UNIT_MAX];   // engineering unit (e.g. "mV");   "" if absent
    uint32_t scale_num;             // value = raw * num / den  (0/0 -> raw counts)
    uint32_t scale_den;
} hwio_adc_t;

typedef struct {
    uint8_t    io_mode;                // HWIO_MODE_* for the whole GP2..GP9 block
    uint8_t    pin[HIL_GPIO_COUNT];    // per-pin sub-config byte (meaning per io_mode)
    hwio_adc_t adc[HWIO_ADC_NCH];      // per-channel annotation
} hwio_t;

// Load + validate 'hwio'; fill *out. On HWIO_ERR_MISSING the caller should still
// use *out — it is pre-filled with the all-UNUSED / unlabeled fail-safe defaults.
// Any other negative code means a present-but-bad file (caller decides policy).
int boot_read_hwio(hwio_t *out);
