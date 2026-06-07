// ============================================================================
// commission.c — A/B + CRC device-identity block in the top two flash sectors.
// ============================================================================
#include "commission.h"
#include <string.h>
#include <stddef.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#define COMMISSION_MAGIC    0x5B0BC011u
#define COMMISSION_VERSION  1u

// Top two sectors of flash (away from the program image, which grows from 0).
#define OFF_A  (PICO_FLASH_SIZE_BYTES - 2u * FLASH_SECTOR_SIZE)
#define OFF_B  (PICO_FLASH_SIZE_BYTES - 1u * FLASH_SECTOR_SIZE)

static uint32_t crc32_buf(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int)(c & 1u)));
    }
    return ~c;
}

static const commission_t *at(uint32_t off) {
    return (const commission_t *)(XIP_BASE + off);
}

static bool valid(const commission_t *c) {
    return c->magic == COMMISSION_MAGIC &&
           c->version == COMMISSION_VERSION &&
           c->crc32 == crc32_buf(c, offsetof(commission_t, crc32));
}

bool commission_load(commission_t *out) {
    const commission_t *a = at(OFF_A), *b = at(OFF_B);
    bool va = valid(a), vb = valid(b);
    const commission_t *best = NULL;
    if (va && vb)      best = (a->seq >= b->seq) ? a : b;
    else if (va)       best = a;
    else if (vb)       best = b;
    if (!best) return false;
    if (out) *out = *best;
    return true;
}

// Erase+program one sector. Safe pre-scheduler (single core). At runtime wrap the
// body in flash_safe_execute() so the other core is locked out of XIP.
bool commission_write(uint16_t class_id, uint16_t instance_id) {
    commission_t cur;
    uint32_t seq = 0;
    uint32_t target = OFF_A;

    if (commission_load(&cur)) {
        seq = cur.seq + 1u;
        const commission_t *a = at(OFF_A);
        // write to the sector that does NOT hold the current newest copy
        target = (valid(a) && a->seq == cur.seq) ? OFF_B : OFF_A;
    }

    commission_t nb;
    memset(&nb, 0, sizeof nb);
    nb.magic = COMMISSION_MAGIC;
    nb.version = COMMISSION_VERSION;
    nb.len = (uint16_t)sizeof nb;
    nb.seq = seq;
    nb.class_id = class_id;
    nb.instance_id = instance_id;
    nb.crc32 = crc32_buf(&nb, offsetof(commission_t, crc32));

    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof page);
    memcpy(page, &nb, sizeof nb);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(target, FLASH_SECTOR_SIZE);
    flash_range_program(target, page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    return true;
}

uint8_t commission_bus_addr(void) {
    commission_t c;
    if (commission_load(&c)) return (uint8_t)(c.instance_id & 0xFFu);
    return COMMISSION_ADDR_NONE;
}
