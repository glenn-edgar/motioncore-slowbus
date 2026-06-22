// ============================================================================
// boot_roster.h — read the master's slave roster from the config-FS ('slvr').
//
// Master-only. The BC loads which slaves to poll (and the poll schedule) from a
// read-only config file instead of waiting for the host to register them, so a
// commissioned controller comes up polling its bus on its own. Absent 'slvr' is
// fine: the BC just starts with an empty roster (host can still register over USB).
//
// slvr CBOR:  { v:1, p:grant_period_ms, w:window_us, m:max_misses,
//               r:tcp_retries, s:[ [addr, variant, flags], ... ] }
// Positional 3-tuples + a 1-byte variant code (NOT the 32-bit class_id) so 32
// slaves fit one 240-B entry. flags bit1 (0x02) = ENABLED (matches FLAG_ENABLED).
// ============================================================================
#pragma once

#include <stdint.h>

#define ROSTER_FILE_MAX 32   // slvr cap (the BC's live g_roster may be smaller)

typedef struct { uint8_t addr, variant, flags; } roster_slave_t;

typedef struct {
    uint16_t       grant_period_ms;
    uint16_t       window_us;
    uint8_t        max_misses;
    uint8_t        tcp_retries;
    uint8_t        n;                       // slaves parsed (<= ROSTER_FILE_MAX)
    roster_slave_t s[ROSTER_FILE_MAX];
} roster_cfg_t;

enum {
    ROSTER_OK          =  0,
    ROSTER_ERR_MISSING = -1,   // no 'slvr' file
    ROSTER_ERR_FORMAT  = -2,   // CBOR malformed / required field absent
    ROSTER_ERR_SCHEMA  = -3,   // schema_ver mismatch
};

// Parse 'slvr' into *out (zeroed first). Returns ROSTER_OK or a negative code.
int boot_read_roster(roster_cfg_t *out);
