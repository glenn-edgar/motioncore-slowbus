// host_link.c — see host_link.h. The northbound dongle protocol state machine.

#include "host_link.h"
#include "opcodes.h"   // vendored libcomm OP_* catalogue

#include <string.h>

// Cadence. REGISTER is re-announced while not yet OPERATIONAL so a Pi controller
// that attaches late (no DTR-reset on the RP2040) still catches the announcement.
#define HL_REGISTER_PERIOD_MS   500u
#define HL_HEARTBEAT_PERIOD_MS  1000u

// REGISTER v2 wire layout (38 bytes) — must match linux/bus_controller identity.c.
#define HL_REGISTER_V2_LEN  38u
#define HL_REGISTER_V2_VER   2u

static void wr_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;        p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// Stage one s2m frame in the TX ring. Returns 0 on success, -1 if full.
static int emit_s2m(host_link_t *h, uint8_t addr, uint16_t opcode,
                    const uint8_t *payload, uint8_t len) {
    frame_meta_t m = {
        .addr = addr, .cmd = opcode, .seq = h->seq,
        .ack_seq = 0, .ack_status = 0, .payload_len = len,
    };
    if (frame_encode_s2m(&m, payload, &h->tx) != 0) return -1;  // ring rolls back
    h->seq++;
    return 0;
}

static void emit_register(host_link_t *h) {
    uint8_t p[HL_REGISTER_V2_LEN];
    p[0] = HL_REGISTER_V2_VER;
    wr_u32(&p[1],  h->cfg.class_id);
    wr_u32(&p[5],  h->cfg.instance_id);
    p[9] = h->cfg.commissioning_state;
    memcpy(&p[10], h->cfg.chip_uid, 16);
    wr_u16(&p[26], h->cfg.vid);
    wr_u16(&p[28], h->cfg.pid);
    wr_u32(&p[30], h->cfg.fw_version);
    wr_u32(&p[34], h->cfg.build_date);
    (void)emit_s2m(h, 1, OP_REGISTER, p, sizeof p);  // addr 1 = dongle->host
}

static void emit_manifest(host_link_t *h) {
    // schema_hash(u32) fw_version(u32) m2s_count(u8) [opcodes u16...]
    static const uint16_t m2s_ops[] = {
        OP_REGISTER_ACK, OP_PING, OP_GET_MANIFEST, OP_OPERATIONAL_BEGIN,
        OP_SHELL_EXEC, OP_POLL, OP_BUS_EXEC,
    };
    uint8_t n = (uint8_t)(sizeof m2s_ops / sizeof m2s_ops[0]);
    uint8_t p[9 + sizeof m2s_ops];
    wr_u32(&p[0], h->cfg.schema_hash);
    wr_u32(&p[4], h->cfg.fw_version);
    p[8] = n;
    for (uint8_t i = 0; i < n; i++) wr_u16(&p[9 + i * 2], m2s_ops[i]);
    (void)emit_s2m(h, 1, OP_MANIFEST_REPLY, p, (uint8_t)sizeof p);
}

void host_link_init(host_link_t *h, const host_link_cfg_t *cfg) {
    memset(h, 0, sizeof *h);
    h->cfg   = *cfg;
    h->state = HL_BOOT;
    frame_ring_init(&h->tx, h->tx_buf, HOST_LINK_TX_RING_SIZE);
    frame_decoder_init(&h->rx, FRAME_DIR_M2S);
    h->next_register_ms  = 0;   // announce immediately on first tick
    h->next_heartbeat_ms = 0;
}

void host_link_set_callbacks(host_link_t *h, host_link_bus_cb on_bus_msg,
                             host_link_shell_cb on_local_shell, void *user) {
    h->on_bus_msg     = on_bus_msg;
    h->on_local_shell = on_local_shell;
    h->user           = user;
}

// One completed m2s frame. addr 0 = us; addr != 0 = a bus node.
static void on_frame(host_link_t *h, const frame_meta_t *m, const uint8_t *payload) {
    if (m->addr != 0) {                       // route to the bus node
        if (h->on_bus_msg)
            h->on_bus_msg(h->user, m->addr, m->cmd, payload, m->payload_len);
        return;
    }

    switch (m->cmd) {
    case OP_REGISTER_ACK:
        if (h->state == HL_BOOT) h->state = HL_L1_ACKED;
        break;

    case OP_GET_MANIFEST:
        emit_manifest(h);
        if (h->state == HL_BOOT || h->state == HL_L1_ACKED) h->state = HL_MANIFEST;
        break;

    case OP_OPERATIONAL_BEGIN:
        h->state = HL_OPERATIONAL;
        h->next_heartbeat_ms = 0;             // heartbeat promptly
        break;

    case OP_PING:
        (void)emit_s2m(h, 1, OP_PONG, NULL, 0);
        break;

    case OP_SHELL_EXEC:
        // BC-local command: [req_id u16][cmd u16][args].
        if (m->payload_len >= 4 && h->on_local_shell) {
            uint16_t req_id = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            uint16_t cmd    = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
            h->on_local_shell(h->user, req_id, cmd,
                              &payload[4], (uint8_t)(m->payload_len - 4));
        }
        break;

    default:
        break;   // unknown m2s opcode — ignore
    }
}

void host_link_feed(host_link_t *h, uint8_t byte) {
    frame_meta_t meta;
    uint8_t payload[COMM_PAYLOAD_MAX];
    if (frame_decoder_feed(&h->rx, byte, &meta, payload) == FRAME_DECODE_FRAME_READY)
        on_frame(h, &meta, payload);
}

void host_link_tick(host_link_t *h, uint32_t now_ms) {
    if (h->state != HL_OPERATIONAL) {
        if ((int32_t)(now_ms - h->next_register_ms) >= 0) {
            emit_register(h);
            h->next_register_ms = now_ms + HL_REGISTER_PERIOD_MS;
        }
    } else {
        if ((int32_t)(now_ms - h->next_heartbeat_ms) >= 0) {
            (void)emit_s2m(h, 1, OP_HEARTBEAT, NULL, 0);
            h->next_heartbeat_ms = now_ms + HL_HEARTBEAT_PERIOD_MS;
        }
    }
}

void host_link_reset_boot(host_link_t *h) {
    h->state = HL_BOOT;
    h->next_register_ms  = 0;   // announce on the next tick
    h->next_heartbeat_ms = 0;
    frame_ring_init(&h->tx, h->tx_buf, HOST_LINK_TX_RING_SIZE);  // drop stale frames
    frame_decoder_init(&h->rx, FRAME_DIR_M2S);                   // resync the decoder
}

uint32_t host_link_tx_drain(host_link_t *h, uint8_t *dst, uint32_t max) {
    return frame_ring_read_drain(&h->tx, dst, max);
}

int host_link_s2m(host_link_t *h, uint8_t addr, uint16_t opcode,
                  const uint8_t *payload, uint8_t len) {
    return emit_s2m(h, addr, opcode, payload, len);
}

int host_link_shell_reply(host_link_t *h, uint16_t req_id, uint8_t status,
                          const uint8_t *result, uint8_t rlen) {
    uint8_t p[3 + COMM_PAYLOAD_MAX];
    if (rlen > COMM_PAYLOAD_MAX - 3) rlen = COMM_PAYLOAD_MAX - 3;
    wr_u16(&p[0], req_id);
    p[2] = status;
    if (rlen) memcpy(&p[3], result, rlen);
    return emit_s2m(h, 0, OP_SHELL_REPLY, p, (uint8_t)(3 + rlen));
}

host_link_state_t host_link_state(const host_link_t *h) { return h->state; }
