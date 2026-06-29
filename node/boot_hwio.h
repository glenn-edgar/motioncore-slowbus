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
// hwio shape:
//   { "v":1,
//     "io":[ r0,r1,r2,r3,r4,r5,r6,r7 ],          # role per GP2..GP9 (HWIO_ROLE_*)
//     "ad":[ {"l":<label>,"n":<num>,"d":<den>,"u":<unit>}, ... up to 3 ] }
// 'io' entries beyond the block, or a role out of range, fail the decode. 'ad'
// and every field inside it are optional (append-only forward-compat).
// ============================================================================
#pragma once

#include <stdint.h>
#include "board.h"   // HIL_GPIO_COUNT

// Pin roles for the HIL block. APPEND-ONLY — never renumber (a flashed config
// carries the integer). UNUSED = 0 so an all-zero / missing map is fail-safe.
enum {
    HWIO_ROLE_UNUSED         = 0,   // hi-Z input, pulls disabled (safe default)
    HWIO_ROLE_INPUT          = 1,   // plain input, no pull
    HWIO_ROLE_INPUT_PULLUP   = 2,
    HWIO_ROLE_INPUT_PULLDOWN = 3,
    HWIO_ROLE_OUTPUT         = 4,   // driven low at config time
    HWIO_ROLE_SERVO          = 5,   // 1 us-tick servo (PIO servo bank)
    HWIO_ROLE_PULSE_COUNT    = 6,   // 1 kHz-sampled edge counter
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
    uint8_t    role[HIL_GPIO_COUNT];   // HWIO_ROLE_* per GP(HIL_GPIO_BASE + i)
    hwio_adc_t adc[HWIO_ADC_NCH];      // per-channel annotation
} hwio_t;

// Load + validate 'hwio'; fill *out. On HWIO_ERR_MISSING the caller should still
// use *out — it is pre-filled with the all-UNUSED / unlabeled fail-safe defaults.
// Any other negative code means a present-but-bad file (caller decides policy).
int boot_read_hwio(hwio_t *out);
