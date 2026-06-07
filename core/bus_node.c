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
            // TODO(bring-up): if (f.type & BUS_TF_TCP) queue an ACK/NAK(seq).
            bus_node_on_data(f.src, f.payload, f.len);
        }

        // A grant addressed to us (POLL, or DATA from the BC) opens our window.
        bool granted = (f.dest == g_my_addr) &&
                       (cls == BUS_FT_POLL || (cls == BUS_FT_DATA && f.src == BUS_ADDR_MASTER));
        if (granted) {
            node_emit_window();
        }
    }
}

__attribute__((weak)) void bus_node_on_data(uint8_t src, const uint8_t *payload, uint8_t len) {
    (void)src; (void)payload; (void)len;
}
