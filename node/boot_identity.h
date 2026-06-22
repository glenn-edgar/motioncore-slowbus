// ============================================================================
// boot_identity.h — read & validate the per-unit 'idnt' config file at boot.
// ============================================================================
#pragma once

#include <stdint.h>
#include "pico/unique_id.h"

enum { CHIP_PICO = 0, CHIP_PICO2 = 1 };

enum {
    IDENT_OK          =  0,
    IDENT_ERR_MISSING = -1,   // no 'idnt' file (no config FS / not flashed)
    IDENT_ERR_FORMAT  = -2,   // CBOR malformed / required field absent
    IDENT_ERR_SCHEMA  = -3,   // schema_ver mismatch (contract)
    IDENT_ERR_CHIP    = -4,   // wrong silicon family (pico vs pico2)
    IDENT_ERR_VARIANT = -5,   // wrong product / hw layout
    IDENT_ERR_UUID    = -6,   // config built for a different physical board
    IDENT_ERR_ADDR    = -7,   // addr inconsistent with the role
};

typedef struct {
    uint8_t chip;
    uint8_t variant;
    uint8_t addr;             // own RS-485 address (master = 0x00)
    uint8_t uuid[PICO_UNIQUE_BOARD_ID_SIZE_BYTES];
} identity_t;

// Load 'idnt', validate it against this image's baked identity + this board's
// unique id, and fill *out on success. Returns IDENT_OK or a negative code.
int boot_read_identity(identity_t *out);
