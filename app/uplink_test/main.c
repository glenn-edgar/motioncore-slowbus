// ============================================================================
// app/uplink_test/main.c — Pico bus controller: dongle host link + bus sweep.
//
// NORTHBOUND: USB-CDC libcomm SLIP+CRC frames. The Pi-side linux/bus_controller
// drives the four-layer sync ladder, sees identity=bus_controller, pushes a
// roster (CMD_BUS_*), and enables the autonomous poll sweep.
//
// SOUTHBOUND (this revision): the autonomous poll sweep + liveness. When polling
// is enabled the BC round-robins the ENABLED roster, POLLing each slave; a slave
// that answers is ALIVE, one that misses max_misses in a row is DEAD. State edges
// escalate to the Pi (OP_BUS_SLAVE_DOWN/UP) and the per-slave interlock summary
// bit escalates as OP_BUS_SLAVE_FLAGGED. Host commands to a slave are injected
// into a poll slot (one in flight): the slave's ISR ACK frees the bus
// (OP_BUS_CMD_ACK) and its reply rides a later routine poll (OP_SHELL_REPLY).
// When polling is disabled, a host command uses the reactive one-shot bridge
// (bench single-slave HIL).
//
// Cooperative engine: one bounded poll slot per main-loop pass, so USB + the
// host link stay serviced between slots. Mirrors the SAMD21 bus_poll_engine.
//
// Binary on USB: frames carry 0xC0/0xDB -> raw I/O only, CRLF compiled out.
// Wiring: Pico TX GP15 -> XIAO D7, Pico RX GP16 <- XIAO D6, GND. SAMD21 = slave.
// ============================================================================
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "host_link.h"
#include "bus_phy.h"
#include "bus_frame.h"
#include "bus_addr.h"
#include "board.h"
#include "opcodes.h"

#define CLASS_ID_BUS_CONTROLLER  0x5E589000u
#define BC_INSTANCE_ID           42u
#define BC_FW_VERSION            0x00000100u   // 0.1.0
#define BC_BUILD_DATE            20260607u
#define BC_SCHEMA_HASH           0x51B00001u
#define USB_VID                  0x2E8Au
#define USB_PID                  0x000Au

// BC-local shell command ids (must match linux/bus_controller controller.c).
#define CMD_ECHO                 0x0001u
#define CMD_BUS_REGISTER_SLAVE   0x0160u
#define CMD_BUS_LIST_SLAVES      0x0162u
#define CMD_BUS_SET_POLL         0x0163u
#define CMD_BUS_POLL_ENABLE      0x0164u
#define CMD_BUS_CLEAR_ROSTER     0x0165u
#define BUS_REG_OK               0u

#define SHELL_OK                 0u
#define SHELL_UNKNOWN_CMD        1u

// Roster flags + liveness states (match SAMD21 bus_roster).
#define FLAG_TCP                 0x01u
#define FLAG_ENABLED             0x02u
#define ST_UNKNOWN               0u
#define ST_ALIVE                 1u
#define ST_DEAD                  2u

// Per-slot timing.
#define POLL_SLOT_TIMEOUT_MS     20    // routine POLL reply window
#define CMD_ACK_TIMEOUT_MS       40    // injected-command ACK window

static host_link_t g_hl;

// --- southbound bus (BC side) -----------------------------------------------
static bus_asm_t g_bc;
static uint8_t   g_bus_seq;

static int bus_recv(bus_frame_t *out, int ms) {
    uint16_t w;
    for (int t = 0; t < ms * 20; t++) {
        if (bus_phy_rx_pop(&w)) { if (bus_asm_feed(&g_bc, w, out)) return 1; }
        else sleep_us(50);
    }
    return 0;
}
static void bus_send(uint8_t dest, uint8_t type, const uint8_t *p, uint8_t len) {
    bus_frame_t f; memset(&f, 0, sizeof f);
    f.dest = dest; f.src = BUS_ADDR_MASTER; f.type = type; f.seq = g_bus_seq++; f.len = len;
    if (len) memcpy(f.payload, p, len);
    uint16_t words[BUS_FRAME_WORDS_MAX];
    bus_phy_rx_flush();
    bus_phy_send_words(words, bus_frame_encode(words, &f));
}

// --- roster + liveness ------------------------------------------------------
#define ROSTER_MAX 16
typedef struct {
    uint8_t  addr;
    uint32_t class_id;
    uint8_t  flags;
    uint8_t  state;             // ST_*
    uint8_t  misses;
    uint32_t last_seen_ms;
    uint8_t  summary;           // last interlock summary byte
    uint8_t  announced_state;   // shadow for defer-never-drop edge emit
    uint8_t  announced_summary;
} slave_t;
static slave_t  g_roster[ROSTER_MAX];
static uint8_t  g_roster_n;
static uint16_t g_poll_period_ms  = 500;
static uint8_t  g_poll_max_misses = 3;
static uint8_t  g_poll_tcp_retries = 2;
static uint8_t  g_poll_enabled;

// Sweep cursor + cadence.
static uint8_t  g_cursor;
static uint32_t g_sweep_next_ms;

// One injected command in flight (host -> slave while sweeping).
static bool     g_cmd_pending;
static uint8_t  g_cmd_slave;
static uint16_t g_cmd_op;
static uint16_t g_cmd_req_id;
static uint8_t  g_cmd_body[BUS_PAYLOAD_MAX];
static uint8_t  g_cmd_len;

static slave_t *roster_find(uint8_t addr) {
    for (uint8_t i = 0; i < g_roster_n; i++) if (g_roster[i].addr == addr) return &g_roster[i];
    return NULL;
}

// --- reactive one-shot bridge (poll disabled): bench single-slave HIL --------
static void reactive_bridge(uint8_t dest, uint16_t opcode, const uint8_t *body, uint8_t len) {
    uint16_t req_id = 0;
    if (opcode == OP_BUS_EXEC && len >= 4) req_id = (uint16_t)body[2] | ((uint16_t)body[3] << 8);
    else if (len >= 2)                     req_id = (uint16_t)body[0] | ((uint16_t)body[1] << 8);

    uint8_t p[BUS_PAYLOAD_MAX];
    if (len > (uint8_t)(BUS_PAYLOAD_MAX - 2)) len = BUS_PAYLOAD_MAX - 2;
    p[0] = (uint8_t)(opcode & 0xFF); p[1] = (uint8_t)(opcode >> 8);
    memcpy(&p[2], body, len);
    bus_send(dest, BUS_FT_DATA, p, (uint8_t)(2 + len));

    bus_frame_t rf;
    if (bus_recv(&rf, 60) && (rf.type & BUS_FT_MASK) == BUS_FT_ACK) {
        uint8_t ack[3] = { dest, (uint8_t)req_id, (uint8_t)(req_id >> 8) };
        (void)host_link_s2m(&g_hl, dest, OP_BUS_CMD_ACK, ack, sizeof ack);
    }
    for (int k = 0; k < 12; k++) {
        bus_send(dest, BUS_FT_POLL, NULL, 0);
        if (bus_recv(&rf, 40) && (rf.type & BUS_FT_MASK) == BUS_FT_DATA && rf.len >= 2 &&
            rf.payload[0] == (uint8_t)(OP_SHELL_REPLY & 0xFF) &&
            rf.payload[1] == (uint8_t)(OP_SHELL_REPLY >> 8)) {
            (void)host_link_s2m(&g_hl, dest, OP_SHELL_REPLY, &rf.payload[2], (uint8_t)(rf.len - 2));
            return;
        }
        sleep_ms(3);
    }
}

// addr != 0: while sweeping, queue the command (one in flight) for a poll slot;
// otherwise bridge it reactively right now.
static void on_bus_msg(void *user, uint8_t dest, uint16_t opcode,
                       const uint8_t *body, uint8_t len) {
    (void)user;
    if (!g_poll_enabled) { reactive_bridge(dest, opcode, body, len); return; }
    if (g_cmd_pending) return;                 // one in flight; tracker retries
    if (len > sizeof g_cmd_body) len = sizeof g_cmd_body;
    g_cmd_slave  = dest;
    g_cmd_op     = opcode;
    g_cmd_len    = len;
    memcpy(g_cmd_body, body, len);
    g_cmd_req_id = (opcode == OP_BUS_EXEC && len >= 4)
                 ? (uint16_t)body[2] | ((uint16_t)body[3] << 8)
                 : (len >= 2 ? (uint16_t)body[0] | ((uint16_t)body[1] << 8) : 0);
    g_cmd_pending = true;
}

// --- liveness bookkeeping ----------------------------------------------------
static void mark_alive(slave_t *s, uint32_t now) {
    s->misses = 0; s->last_seen_ms = now; s->state = ST_ALIVE;
}
static void mark_miss(slave_t *s) {
    if (s->misses < 0xFF) s->misses++;
    if (s->state != ST_DEAD && s->misses >= g_poll_max_misses) s->state = ST_DEAD;
}

// Emit one pending liveness / flagged edge per call (defer-never-drop: a full TX
// ring just retries next loop; the announced-shadow is the pending flag).
static void emit_liveness_edges(void) {
    for (uint8_t i = 0; i < g_roster_n; i++) {
        slave_t *s = &g_roster[i];
        if (s->state != s->announced_state) {
            if (s->state == ST_DEAD) {
                uint8_t b[1] = { s->addr };
                if (host_link_s2m(&g_hl, s->addr, OP_BUS_SLAVE_DOWN, b, 1) == 0)
                    s->announced_state = ST_DEAD;
                return;
            } else if (s->state == ST_ALIVE && s->announced_state == ST_DEAD) {
                uint8_t b[5] = { s->addr, (uint8_t)s->class_id, (uint8_t)(s->class_id >> 8),
                                 (uint8_t)(s->class_id >> 16), (uint8_t)(s->class_id >> 24) };
                if (host_link_s2m(&g_hl, s->addr, OP_BUS_SLAVE_UP, b, 5) == 0)
                    s->announced_state = ST_ALIVE;
                return;
            } else {
                s->announced_state = s->state;   // UNKNOWN->ALIVE: silent
            }
        }
        if (s->summary != s->announced_summary) {
            uint8_t b[2] = { s->addr, s->summary };
            if (host_link_s2m(&g_hl, s->addr, OP_BUS_SLAVE_FLAGGED, b, 2) == 0)
                s->announced_summary = s->summary;
            return;
        }
    }
}

// Pick the next ENABLED slave starting at the cursor; advances cursor. NULL if
// none enabled.
static slave_t *next_enabled(void) {
    for (uint8_t n = 0; n < g_roster_n; n++) {
        slave_t *s = &g_roster[g_cursor];
        g_cursor = (uint8_t)((g_cursor + 1) % (g_roster_n ? g_roster_n : 1));
        if (s->flags & FLAG_ENABLED) return s;
    }
    return NULL;
}

// One poll slot per due tick: inject a queued command, else POLL the next slave.
static void sweep_step(uint32_t now) {
    if (!g_poll_enabled || g_roster_n == 0) return;
    if ((int32_t)(now - g_sweep_next_ms) < 0) return;
    g_sweep_next_ms = now + g_poll_period_ms;

    bus_frame_t rf;

    if (g_cmd_pending) {
        // Inject the command as DATA = [opcode:u16][body]; expect the ISR ACK/NAK.
        uint8_t p[BUS_PAYLOAD_MAX];
        uint8_t blen = g_cmd_len;
        if (blen > (uint8_t)(BUS_PAYLOAD_MAX - 2)) blen = BUS_PAYLOAD_MAX - 2;
        p[0] = (uint8_t)(g_cmd_op & 0xFF); p[1] = (uint8_t)(g_cmd_op >> 8);
        memcpy(&p[2], g_cmd_body, blen);
        bus_send(g_cmd_slave, BUS_FT_DATA, p, (uint8_t)(2 + blen));

        slave_t *s = roster_find(g_cmd_slave);
        if (bus_recv(&rf, CMD_ACK_TIMEOUT_MS) && rf.src == g_cmd_slave) {
            if (s) mark_alive(s, now);
            uint8_t cls = (uint8_t)(rf.type & BUS_FT_MASK);
            if (cls == BUS_FT_ACK || cls == BUS_FT_NAK) {
                uint8_t b[3] = { g_cmd_slave, (uint8_t)g_cmd_req_id, (uint8_t)(g_cmd_req_id >> 8) };
                (void)host_link_s2m(&g_hl, g_cmd_slave,
                                    cls == BUS_FT_NAK ? OP_BUS_CMD_NAK : OP_BUS_CMD_ACK, b, 3);
            }
        }
        g_cmd_pending = false;   // bus freed; the reply rides a later routine poll
        return;
    }

    slave_t *s = next_enabled();
    if (!s) return;
    bus_send(s->addr, BUS_FT_POLL, NULL, 0);
    if (bus_recv(&rf, POLL_SLOT_TIMEOUT_MS) && rf.src == s->addr) {
        mark_alive(s, now);
        uint8_t cls = (uint8_t)(rf.type & BUS_FT_MASK);
        if (cls == BUS_FT_DATA && rf.len >= 2 &&
            rf.payload[0] == (uint8_t)(OP_SHELL_REPLY & 0xFF) &&
            rf.payload[1] == (uint8_t)(OP_SHELL_REPLY >> 8)) {
            // Async command reply finished off-bus -> relay to the Pi.
            (void)host_link_s2m(&g_hl, s->addr, OP_SHELL_REPLY, &rf.payload[2], (uint8_t)(rf.len - 2));
        } else if (cls == BUS_FT_NO_MESSAGE) {
            s->summary = (rf.len >= 1) ? rf.payload[0] : 0;
        }
    } else {
        mark_miss(s);
    }
}

// ----------------------------------------------------------------------------
// addr == 0: BC-local command. ECHO + the CMD_BUS_* roster/sweep surface.
// ----------------------------------------------------------------------------
static void on_local_shell(void *user, uint16_t req_id, uint16_t cmd,
                           const uint8_t *args, uint8_t alen) {
    (void)user;
    switch (cmd) {
    case CMD_ECHO:
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, args, alen);
        break;

    case CMD_BUS_CLEAR_ROSTER:
        g_roster_n = 0; g_cursor = 0; g_cmd_pending = false;
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0);
        break;

    case CMD_BUS_REGISTER_SLAVE: {   // [addr][class_id u32][flags]
        uint8_t reason = BUS_REG_OK;
        if (alen >= 6 && g_roster_n < ROSTER_MAX) {
            slave_t *s = &g_roster[g_roster_n++];
            memset(s, 0, sizeof *s);
            s->addr     = args[0];
            s->class_id = (uint32_t)args[1] | ((uint32_t)args[2] << 8) |
                          ((uint32_t)args[3] << 16) | ((uint32_t)args[4] << 24);
            s->flags    = args[5];
            s->state = s->announced_state = ST_UNKNOWN;
        }
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, &reason, 1);
        break;
    }
    case CMD_BUS_SET_POLL:           // [period u16][max_misses][tcp_retries]
        if (alen >= 4) {
            g_poll_period_ms   = (uint16_t)args[0] | ((uint16_t)args[1] << 8);
            g_poll_max_misses  = args[2];
            g_poll_tcp_retries = args[3];
        }
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0);
        break;

    case CMD_BUS_LIST_SLAVES: {      // [total][shown] then 10-byte rows
        uint8_t r[2 + ROSTER_MAX * 10]; uint8_t n = 0;
        uint32_t now = to_ms_since_boot(get_absolute_time());
        r[n++] = g_roster_n; r[n++] = g_roster_n;
        for (uint8_t i = 0; i < g_roster_n; i++) {
            slave_t *s = &g_roster[i];
            uint32_t ago32 = (s->last_seen_ms == 0) ? 0xFFFFu : (now - s->last_seen_ms);
            uint16_t ago = (ago32 > 0xFFFFu) ? 0xFFFFu : (uint16_t)ago32;
            r[n++] = s->addr;
            r[n++] = (uint8_t)s->class_id;       r[n++] = (uint8_t)(s->class_id >> 8);
            r[n++] = (uint8_t)(s->class_id >> 16); r[n++] = (uint8_t)(s->class_id >> 24);
            r[n++] = s->flags; r[n++] = s->state; r[n++] = s->misses;
            r[n++] = (uint8_t)ago; r[n++] = (uint8_t)(ago >> 8);
        }
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, r, n);
        break;
    }
    case CMD_BUS_POLL_ENABLE:        // [on]
        if (alen >= 1) {
            g_poll_enabled = args[0];
            g_sweep_next_ms = to_ms_since_boot(get_absolute_time());  // start promptly
        }
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0);
        break;

    default:
        host_link_shell_reply(&g_hl, req_id, SHELL_UNKNOWN_CMD, NULL, 0);
        break;
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    bus_phy_init(BUS_DEFAULT_BAUD);
    bus_asm_init(&g_bc, BUS_ADDR_MASTER, true);

    host_link_cfg_t cfg = {
        .class_id            = CLASS_ID_BUS_CONTROLLER,
        .instance_id         = BC_INSTANCE_ID,
        .commissioning_state = 1,
        .vid = USB_VID, .pid = USB_PID,
        .fw_version = BC_FW_VERSION, .build_date = BC_BUILD_DATE,
        .schema_hash = BC_SCHEMA_HASH,
    };
    pico_unique_board_id_t uid;
    pico_get_unique_board_id(&uid);
    memcpy(cfg.chip_uid, uid.id, sizeof uid.id);
    host_link_init(&g_hl, &cfg);
    host_link_set_callbacks(&g_hl, on_bus_msg, on_local_shell, NULL);

    bool prev_conn = false;
    for (;;) {
        // Host (re)attach: re-arm to BOOT on the CDC disconnect edge (no DTR-reset
        // on RP2040) AND drop poll/roster state so the next controller re-provisions.
        bool conn = stdio_usb_connected();
        if (prev_conn && !conn) {
            host_link_reset_boot(&g_hl);
            g_poll_enabled = false; g_cmd_pending = false; g_roster_n = 0; g_cursor = 0;
        }
        prev_conn = conn;

        int c;
        while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
            host_link_feed(&g_hl, (uint8_t)c);

        uint32_t now = to_ms_since_boot(get_absolute_time());
        host_link_tick(&g_hl, now, conn);
        sweep_step(now);
        emit_liveness_edges();

        uint8_t out[64]; uint32_t n;
        while ((n = host_link_tx_drain(&g_hl, out, sizeof out)) > 0) {
            for (uint32_t i = 0; i < n; i++) putchar_raw(out[i]);
            stdio_flush();
        }
    }
}
