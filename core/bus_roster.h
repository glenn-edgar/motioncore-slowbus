// ============================================================================
// bus_roster.h — RAM-only node roster held by the bus controller.
//
// Adapted from the SAMD21 bus_controller starting point (reference/samd21/).
// Chip-independent. The roster is the set of nodes the BC schedules. It is
// RAM-only by decision: the host is the single source of truth and re-pushes it
// after any BC cold boot or watchdog recovery — no flash wear, no stale roster
// after rewiring.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "bus_addr.h"

#define BUS_ROSTER_MAX   32u   // bounded; a real bus is small (addr space is 128)

// Liveness state, updated by the scheduler's poll loop.
#define BUS_STATE_UNKNOWN 0u
#define BUS_STATE_ALIVE   1u
#define BUS_STATE_DEAD    2u

// Per-node flags.
#define BUS_FLAG_TCP      0x01u   // command this node reliably (else fire-and-forget)
#define BUS_FLAG_ENABLED  0x02u   // included in the poll/grant rotation
#define BUS_FLAG_PEERING  0x04u   // permitted to send peer (node->node) frames in-window

typedef struct {
    uint8_t  addr;                 // 1..126; 0 marks an empty slot
    uint32_t class_id;             // host bookkeeping + optional sanity check
    uint8_t  flags;                // BUS_FLAG_*
    uint8_t  state;                // BUS_STATE_*
    uint8_t  consecutive_misses;
    uint32_t last_seen_ms;         // 0 = never heard
} bus_slave_t;

typedef struct {
    uint16_t grant_period_ms;      // round-robin pacing
    uint16_t window_us;            // per-node window budget (reply + peer tail)
    uint8_t  max_misses;           // misses before a node is marked DEAD
    uint8_t  tcp_retries;          // reliable-DATA retransmits before DEAD
    uint8_t  enabled;              // master scheduling enable
} bus_sched_cfg_t;

// register result codes
#define BUS_REG_OK      0u
#define BUS_REG_FULL    1u
#define BUS_REG_DUP     2u
#define BUS_REG_BADADDR 3u

void bus_roster_init(void);

uint8_t bus_roster_register(uint8_t addr, uint32_t class_id, uint8_t flags,
                            uint8_t *out_count);
bool    bus_roster_unregister(uint8_t addr, uint8_t *out_count);
void    bus_roster_clear(void);

uint8_t            bus_roster_count(void);
const bus_slave_t *bus_roster_at(uint8_t index);     // 0..count-1, occupied slots
bus_slave_t       *bus_roster_find(uint8_t addr);    // NULL if absent

// Round-robin iterator over ENABLED nodes. *cursor is an opaque slot index the
// caller persists; returns the next enabled node and advances past it (wrapping).
// NULL if none enabled.
const bus_slave_t *bus_roster_next_enabled(uint8_t *cursor);

bus_sched_cfg_t   *bus_sched_cfg(void);
