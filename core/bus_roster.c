// ============================================================================
// bus_roster.c — RAM-only node roster. See bus_roster.h.
// Adapted from reference/samd21/bus_roster.c (128-node space, peer flag added).
// ============================================================================
#include "bus_roster.h"

static bus_slave_t     g_roster[BUS_ROSTER_MAX];
static uint8_t         g_count;
static bus_sched_cfg_t g_cfg;

void bus_roster_init(void) {
    bus_roster_clear();
    g_cfg.grant_period_ms = 0;       // 0 = poll as fast as the bus allows
    g_cfg.window_us       = 2000;    // per-node window budget
    g_cfg.max_misses      = 3;
    g_cfg.tcp_retries     = 3;
    g_cfg.enabled         = 0;       // host enables once the roster is loaded
}

void bus_roster_clear(void) {
    for (uint8_t i = 0; i < BUS_ROSTER_MAX; ++i) g_roster[i].addr = 0;
    g_count = 0;
}

static bool addr_valid(uint8_t a) {
    return a >= BUS_ADDR_SLAVE_MIN && a <= BUS_ADDR_SLAVE_MAX;
}

uint8_t bus_roster_register(uint8_t addr, uint32_t class_id, uint8_t flags,
                            uint8_t *out_count) {
    if (out_count) *out_count = g_count;
    if (!addr_valid(addr))            return BUS_REG_BADADDR;
    if (bus_roster_find(addr))        return BUS_REG_DUP;
    if (g_count >= BUS_ROSTER_MAX)    return BUS_REG_FULL;

    for (uint8_t i = 0; i < BUS_ROSTER_MAX; ++i) {
        if (g_roster[i].addr == 0) {
            g_roster[i] = (bus_slave_t){
                .addr = addr, .class_id = class_id, .flags = flags,
                .state = BUS_STATE_UNKNOWN, .consecutive_misses = 0,
                .last_seen_ms = 0,
            };
            g_count++;
            if (out_count) *out_count = g_count;
            return BUS_REG_OK;
        }
    }
    return BUS_REG_FULL;   // unreachable if g_count is consistent
}

bool bus_roster_unregister(uint8_t addr, uint8_t *out_count) {
    bus_slave_t *s = bus_roster_find(addr);
    if (s) { s->addr = 0; g_count--; }
    if (out_count) *out_count = g_count;
    return s != 0;
}

uint8_t bus_roster_count(void) { return g_count; }

const bus_slave_t *bus_roster_at(uint8_t index) {
    uint8_t seen = 0;
    for (uint8_t i = 0; i < BUS_ROSTER_MAX; ++i) {
        if (g_roster[i].addr != 0) {
            if (seen == index) return &g_roster[i];
            seen++;
        }
    }
    return 0;
}

bus_slave_t *bus_roster_find(uint8_t addr) {
    if (addr == 0) return 0;
    for (uint8_t i = 0; i < BUS_ROSTER_MAX; ++i) {
        if (g_roster[i].addr == addr) return &g_roster[i];
    }
    return 0;
}

const bus_slave_t *bus_roster_next_enabled(uint8_t *cursor) {
    for (uint8_t step = 0; step < BUS_ROSTER_MAX; ++step) {
        uint8_t i = (uint8_t)((*cursor + step) % BUS_ROSTER_MAX);
        const bus_slave_t *s = &g_roster[i];
        if (s->addr != 0 && (s->flags & BUS_FLAG_ENABLED)) {
            *cursor = (uint8_t)((i + 1u) % BUS_ROSTER_MAX);
            return s;
        }
    }
    return 0;
}

bus_sched_cfg_t *bus_sched_cfg(void) { return &g_cfg; }
