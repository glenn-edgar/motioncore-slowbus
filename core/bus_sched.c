// ============================================================================
// bus_sched.c — bus controller arbiter. See bus_sched.h.
//
// STATUS: SKELETON. The control flow and data structures are laid out; the
// timing-critical inner loop (window listen, ack-table resolution, peer
// forwarding) is marked TODO and lands during RP2350 bench bring-up once the
// PIO PHY can move bytes. Everything here is chip-independent — it calls only
// bus_phy_* / bus_uplink_* / bus_roster_* and never touches a register.
// ============================================================================
#include "bus_sched.h"
#include "bus_addr.h"
#include "bus_roster.h"
#include "bus_phy.h"
#include "bus_uplink.h"

// Per-node outbound queue (host->node and self-originated frames awaiting a grant).
#define SCHED_TXQ_PER_NODE  4u

typedef struct {
    bus_frame_t frame;
    bool        tcp;
    bool        in_use;
} sched_txq_slot_t;

// Flat queue keyed by dest; small and bounded — a real bus is a handful of nodes.
static sched_txq_slot_t g_txq[BUS_ROSTER_MAX * SCHED_TXQ_PER_NODE];
static uint8_t          g_cursor;     // round-robin slot index into the roster
static uint8_t          g_next_seq;   // reliable-DATA sequence allocator

// board_millis() comes from the port; declared here to avoid a header cycle.
extern uint32_t board_millis(void);

void bus_sched_init(void) {
    for (unsigned i = 0; i < sizeof(g_txq) / sizeof(g_txq[0]); ++i)
        g_txq[i].in_use = false;
    g_cursor   = 0;
    g_next_seq = 1;
}

bool bus_sched_queue_tx(uint8_t dest, uint8_t type, const uint8_t *payload,
                        uint8_t len, bool tcp) {
    if (len > BUS_PAYLOAD_MAX) return false;
    for (unsigned i = 0; i < sizeof(g_txq) / sizeof(g_txq[0]); ++i) {
        if (g_txq[i].in_use) continue;
        sched_txq_slot_t *s = &g_txq[i];
        s->frame.dest = dest;
        s->frame.src  = BUS_ADDR_MASTER;
        s->frame.type = type;
        s->frame.seq  = tcp ? g_next_seq++ : 0u;
        s->frame.len  = len;
        for (uint8_t b = 0; b < len; ++b) s->frame.payload[b] = payload[b];
        s->tcp    = tcp;
        s->in_use = true;
        return true;
    }
    return false;   // queue full
}

// Pop the oldest queued frame for `dest`, if any.
static sched_txq_slot_t *txq_take(uint8_t dest) {
    for (unsigned i = 0; i < sizeof(g_txq) / sizeof(g_txq[0]); ++i) {
        if (g_txq[i].in_use && g_txq[i].frame.dest == dest) return &g_txq[i];
    }
    return 0;
}

uint8_t bus_sched_tick(void) {
    const bus_slave_t *node = bus_roster_next_enabled(&g_cursor);
    if (!node) return 0;

    uint16_t words[BUS_FRAME_WORDS_MAX];

    // 1. Grant: queued DATA (doubles as the grant) or a bare POLL.
    sched_txq_slot_t *q = txq_take(node->addr);
    if (q) {
        uint16_t n = bus_frame_encode(words, &q->frame);
        bus_phy_rx_flush();
        bus_phy_send_words(words, n);
        // TODO(bring-up): if q->tcp, record (addr,seq) in the ack-table and keep
        // the slot in_use until ACK; release on ACK, retransmit on NAK/timeout up
        // to tcp_retries, then mark the node DEAD. For now release immediately.
        q->in_use = false;
    } else {
        bus_frame_t poll = { .dest = node->addr, .src = BUS_ADDR_MASTER,
                             .type = BUS_FT_POLL, .seq = 0, .len = 0 };
        uint16_t n = bus_frame_encode(words, &poll);
        bus_phy_rx_flush();
        bus_phy_send_words(words, n);
    }

    // 2. Listen to the node's window (reply + peer tail) until NO_MESSAGE or the
    //    window budget elapses.
    // TODO(bring-up): drain bus_phy_rx_pop() -> bus_asm_feed(); dispatch by
    //   frame type and dest:
    //     dest==MASTER, type ACK/NAK  -> resolve ack-table
    //     dest==MASTER, type DATA     -> bus_sched_on_node_to_master(&f)
    //     dest==peer,   type DATA     -> bus_sched_on_peer_frame(&f)
    //     type NO_MESSAGE             -> window done, break
    //   Bound the wait by bus_sched_cfg()->window_us; pet the bus-progress WDT
    //   only when a byte actually arrives.
    (void)board_millis;

    // 3. Liveness bookkeeping (misses / ALIVE-DEAD edges) folds in with step 2.
    return node->addr;
}

// --- Weak default hooks (app overrides as needed) ---------------------------
__attribute__((weak)) void bus_sched_on_node_to_master(const bus_frame_t *f) {
    bus_uplink_send(f->src, f->payload, f->len);
}
__attribute__((weak)) void bus_sched_on_peer_frame(const bus_frame_t *f) {
    (void)f;   // BC does not relay peer frames; addressed node already has it
}
__attribute__((weak)) void bus_sched_on_node_state(uint8_t addr, uint8_t st,
                                                   uint32_t class_id) {
    (void)addr; (void)st; (void)class_id;
}
