// ============================================================================
// cfg_file.c — read-only config store (SAMD21 boot-store format, Step 2a).
//
// The config region is the top 64 KB of flash (256 rows x 256 B), flashed as a
// separate `picotool` load. Each used row is one cfg_entry_t (byte-identical to
// the SAMD21 store_entry_t): magic + monotonic seq + 4-char name + len + CRC-8/
// AUTOSAR + <=240 B CBOR data. cfg_load() scans the rows, validates magic/CRC,
// and returns the highest-seq entry matching the name (latest-seq-wins, kept for
// exact format parity even though the host builder writes one entry per name).
//
// READ-ONLY: pure XIP pointer reads, no erase/program — so the dual-core flash
// hazard does not apply. Erased rows read 0xFF (magic mismatch) and are skipped,
// so an absent file / empty region degrades to "not found" (graceful fallback).
// ============================================================================
#include "cfg_file.h"
#include <string.h>
#include "pico.h"                       // PICO_FLASH_SIZE_BYTES (board header)
#include "hardware/regs/addressmap.h"   // XIP_BASE
#include "bus_crc8.h"                   // bus_crc8_update: CRC-8/AUTOSAR (core; shared BC+slave)

#define CFG_STORE_MAGIC     0x10C0FFEEu
#define CFG_STORE_ROW_SZ    256u
#define CFG_STORE_DATA_MAX  240u
#define CFG_REGION_LEN      (64u * 1024u)
#define CFG_REGION_BASE     (XIP_BASE + PICO_FLASH_SIZE_BYTES - CFG_REGION_LEN)
#define CFG_REGION_ROWS     (CFG_REGION_LEN / CFG_STORE_ROW_SZ)

typedef struct {
    uint32_t magic;                  // CFG_STORE_MAGIC when valid, 0xFFFFFFFF when erased
    uint32_t seq;                    // monotonic; highest per name wins
    uint8_t  name[CFG_NAME_LEN];     // 4-char key (space/zero padded)
    uint8_t  len;                    // payload length, <= CFG_STORE_DATA_MAX
    uint8_t  crc;                    // entry_crc(name, len, data)
    uint8_t  pad[2];
    uint8_t  data[CFG_STORE_DATA_MAX];
} cfg_entry_t;                       // exactly one 256-B flash row
_Static_assert(sizeof(cfg_entry_t) == CFG_STORE_ROW_SZ, "cfg_entry_t must be one 256-B row");

// CRC-8/AUTOSAR over [name[4], len, data[len]] — byte-identical to the SAMD21
// store's store_crc(), so the same host tooling builds both images.
static uint8_t entry_crc(const uint8_t name[CFG_NAME_LEN], uint8_t len, const uint8_t *data) {
    uint8_t c = 0xFFu;
    for (uint8_t i = 0; i < CFG_NAME_LEN; i++) c = bus_crc8_update(c, name[i]);
    c = bus_crc8_update(c, len);
    for (uint8_t i = 0; i < len; i++)         c = bus_crc8_update(c, data[i]);
    return (uint8_t)(c ^ 0xFFu);
}

static bool entry_valid(const cfg_entry_t *e) {
    return e->magic == CFG_STORE_MAGIC && e->name[0] != 0xFFu
        && e->len <= CFG_STORE_DATA_MAX && entry_crc(e->name, e->len, e->data) == e->crc;
}

int cfg_load(const char name[CFG_NAME_LEN], uint8_t *buf, uint32_t cap, uint32_t *out_len) {
    const cfg_entry_t *region = (const cfg_entry_t *)(uintptr_t)CFG_REGION_BASE;
    const cfg_entry_t *best = 0;
    for (uint32_t r = 0; r < CFG_REGION_ROWS; r++) {
        const cfg_entry_t *e = &region[r];
        if (!entry_valid(e)) continue;
        if (memcmp(e->name, name, CFG_NAME_LEN) != 0) continue;
        if (!best || e->seq > best->seq) best = e;   // latest-seq-wins (format parity)
    }
    if (!best) return -1;                 // absent / empty region
    if (best->len > cap) return -2;       // caller buffer too small
    memcpy(buf, best->data, best->len);
    if (out_len) *out_len = best->len;
    return 0;
}

bool cfg_layout_ok(const void *flash_binary_end) {
    return (uintptr_t)flash_binary_end <= (uintptr_t)CFG_REGION_BASE;
}
