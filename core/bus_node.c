// ============================================================================
// bus_node.c — slave-side bus logic. See bus_node.h.
//
// STATUS: SKELETON. Queue + window structure are laid out; the in-window TX
// sequencing and grant detection land during bench bring-up against the PIO
// PHY. Chip-independent (bus_phy_* / bus_frame_* only).
// ============================================================================
#include "bus_node.h"
#include "bus_addr.h"
#include "bus_phy.h"

#define NODE_TXQ_LEN  8u   // M33 nodes have memory; a deeper outbound queue is cheap

typedef struct {
    bus_frame_t frame;
    bool        in_use;
} node_txq_slot_t;

static node_txq_slot_t g_txq[NODE_TXQ_LEN];
static bus_asm_t       g_asm;
static uint8_t         g_my_addr;

void bus_node_init(uint8_t my_addr) {
    g_my_addr = my_addr;
    for (uint8_t i = 0; i < NODE_TXQ_LEN; ++i) g_txq[i].in_use = false;
    // Accept frames addressed to us or broadcast; not promiscuous.
    bus_asm_init(&g_asm, my_addr, false);
}

bool bus_node_queue(uint8_t dest, uint8_t type, const uint8_t *payload, uint8_t len) {
    if (len > BUS_PAYLOAD_MAX) return false;
    for (uint8_t i = 0; i < NODE_TXQ_LEN; ++i) {
        if (g_txq[i].in_use) continue;
        node_txq_slot_t *s = &g_txq[i];
        s->frame.dest = dest;
        s->frame.src  = g_my_addr;
        s->frame.type = type;
        s->frame.seq  = 0;
        s->frame.len  = len;
        for (uint8_t b = 0; b < len; ++b) s->frame.payload[b] = payload[b];
        s->in_use = true;
        return true;
    }
    return false;
}

// Acknowledge a BC command immediately (the BC's inject step waits ~40 ms for an
// ACK before it moves on to POLL for the reply). The reply itself is queued by
// the app (bus_node_on_data) and shipped on the next POLL grant -- the async
// two-phase model the BC arbiter is built around (DATA->ACK->POLL->reply).
static void node_emit_ack(void) {
    uint16_t words[BUS_FRAME_WORDS_MAX];
    bus_frame_t ack = { .dest = BUS_ADDR_MASTER, .src = g_my_addr,
                        .type = BUS_FT_ACK, .seq = 0, .len = 0 };
    uint16_t n = bus_frame_encode(words, &ack);
    bus_phy_send_words(words, n);
}

// Transmit the node's whole window: queued frames in order, then NO_MESSAGE.
static void node_emit_window(void) {
    uint16_t words[BUS_FRAME_WORDS_MAX];
    for (uint8_t i = 0; i < NODE_TXQ_LEN; ++i) {
        if (!g_txq[i].in_use) continue;
        uint16_t n = bus_frame_encode(words, &g_txq[i].frame);
        bus_phy_send_words(words, n);
        g_txq[i].in_use = false;
    }
    bus_frame_t term = { .dest = BUS_ADDR_MASTER, .src = g_my_addr,
                         .type = BUS_FT_NO_MESSAGE, .seq = 0, .len = 0 };
    uint16_t n = bus_frame_encode(words, &term);
    bus_phy_send_words(words, n);
}

void bus_node_task(void) {
    uint16_t word9;
    bus_frame_t f;
    while (bus_phy_rx_pop(&word9)) {
        if (!bus_asm_feed(&g_asm, word9, &f)) continue;

        uint8_t cls = f.type & BUS_FT_MASK;
        if (cls == BUS_FT_DATA) {
            bus_node_on_data(f.src, f.payload, f.len);   // app may queue a reply
            // A BC command is acknowledged now; its reply ships on the next POLL
            // (two-phase: the BC injects DATA, awaits the ACK, then POLLs to
            // collect). Peer DATA (src != BC) is fire-and-forget -- no ACK.
            if (f.src == BUS_ADDR_MASTER && f.dest == g_my_addr) node_emit_ack();
        } else if (cls == BUS_FT_POLL && f.dest == g_my_addr) {
            node_emit_window();                          // ship queued replies + NO_MESSAGE
        }
    }
}

__attribute__((weak)) void bus_node_on_data(uint8_t src, const uint8_t *payload, uint8_t len) {
    (void)src; (void)payload; (void)len;
}
