// ============================================================================
// app/uplink_test/main.c — connect the Pico bus controller to the Pi dongle host.
//
// Brings up the NORTHBOUND host link (USB-CDC libcomm SLIP+CRC frames) so the
// Pi-side linux/bus_controller opens this Pico, drives the four-layer sync
// ladder, and recognises it as a bus_controller (identity = class_id
// CLASS_ID_BUS_CONTROLLER, instance 42). Then it bridges SOUTHBOUND: any host
// frame addressed to a bus node (addr != 0) is forwarded over the proven 9-bit
// bus to that slave and the reply is relayed back, correlated by request_id.
//
// This is the bare-superloop bring-up (like phy_test / bus2_test / bus_api_test);
// folding host_link into the FreeRTOS app/bus_controller + the autonomous poll
// sweep is the next step.
//
// Binary on USB: libcomm frames contain 0xC0/0xDB — raw I/O only (putchar_raw /
// getchar_timeout_us), CRLF translation compiled out. No printf to USB: human
// diagnostics would corrupt the frame stream (route them as OP_DBG_LOG later).
//
// Wiring: Pico TX GP15 -> XIAO D7, Pico RX GP16 <- XIAO D6, GND. SAMD21 = slave 3.
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

// Identity announced to the Pi. class_id MUST map to ROLE_BUS_CONTROLLER in
// linux/bus_controller/identity.h (interim: reuse the SAMD21 BC class until a
// Pico-specific class_id lands in the kb_build catalog).
#define CLASS_ID_BUS_CONTROLLER  0x5E589000u
#define BC_INSTANCE_ID           42u
#define BC_FW_VERSION            0x00000100u   // 0.1.0
#define BC_BUILD_DATE            20260607u
#define BC_SCHEMA_HASH           0x51B00001u   // opaque to the Pi; placeholder
#define USB_VID                  0x2E8Au        // Raspberry Pi
#define USB_PID                  0x000Au        // Pico SDK CDC

// BC-local shell command ids (must match linux/bus_controller controller.c).
#define CMD_ECHO                 0x0001u
#define CMD_BUS_REGISTER_SLAVE   0x0160u
#define CMD_BUS_LIST_SLAVES      0x0162u
#define CMD_BUS_SET_POLL         0x0163u
#define CMD_BUS_POLL_ENABLE      0x0164u
#define CMD_BUS_CLEAR_ROSTER     0x0165u
#define BUS_REG_OK               0u

// OP_SHELL_REPLY status codes (opcodes.h shell_status_t mirror).
#define SHELL_OK                 0u
#define SHELL_UNKNOWN_CMD        1u

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

// --- in-RAM roster stub (just enough for provisioning to reach DONE) --------
#define ROSTER_MAX 16
static struct { uint8_t addr; uint32_t class_id; uint8_t flags; } g_roster[ROSTER_MAX];
static uint8_t  g_roster_n;
static uint16_t g_poll_period_ms = 50;
static uint8_t  g_poll_max_misses = 3;
static uint8_t  g_poll_tcp_retries = 2;
static uint8_t  g_poll_enabled;

// ----------------------------------------------------------------------------
// addr != 0: bridge the command over the bus to that slave, relay the reply.
// Reactive (one in flight), reusing the bus_api_test DATA/ACK/POLL handshake.
// ----------------------------------------------------------------------------
static void on_bus_msg(void *user, uint8_t dest, uint16_t opcode,
                       const uint8_t *body, uint8_t len) {
    (void)user;
    // request_id lives at a different offset by opcode (for the ACK relay).
    uint16_t req_id = 0;
    if (opcode == OP_BUS_EXEC && len >= 4) req_id = (uint16_t)body[2] | ((uint16_t)body[3] << 8);
    else if (len >= 2)                     req_id = (uint16_t)body[0] | ((uint16_t)body[1] << 8);

    // Bus DATA payload = [opcode:u16][body].
    uint8_t p[BUS_PAYLOAD_MAX];
    if (len > (uint8_t)(BUS_PAYLOAD_MAX - 2)) len = BUS_PAYLOAD_MAX - 2;
    p[0] = (uint8_t)(opcode & 0xFF); p[1] = (uint8_t)(opcode >> 8);
    memcpy(&p[2], body, len);
    bus_send(dest, BUS_FT_DATA, p, (uint8_t)(2 + len));

    // Slave ISR ACKs (echoing req_id); relay it as OP_BUS_CMD_ACK [addr][req_id].
    bus_frame_t rf;
    if (bus_recv(&rf, 60) && (rf.type & BUS_FT_MASK) == BUS_FT_ACK) {
        uint8_t ack[3] = { dest, (uint8_t)req_id, (uint8_t)(req_id >> 8) };
        (void)host_link_s2m(&g_hl, dest, OP_BUS_CMD_ACK, ack, sizeof ack);
    }

    // Poll for the async reply DATA [OP_SHELL_REPLY:u16][req_id][status][result].
    for (int k = 0; k < 12; k++) {
        bus_send(dest, BUS_FT_POLL, NULL, 0);
        if (bus_recv(&rf, 40) && (rf.type & BUS_FT_MASK) == BUS_FT_DATA && rf.len >= 2 &&
            rf.payload[0] == (uint8_t)(OP_SHELL_REPLY & 0xFF) &&
            rf.payload[1] == (uint8_t)(OP_SHELL_REPLY >> 8)) {
            // Relay payload after the bus opcode as s2m OP_SHELL_REPLY tagged
            // with the source slave; the Pi demux correlates by request_id.
            (void)host_link_s2m(&g_hl, dest, OP_SHELL_REPLY,
                                &rf.payload[2], (uint8_t)(rf.len - 2));
            return;
        }
        sleep_ms(3);
    }
    // No reply: the Pi's command tracker times out / resends.
}

// ----------------------------------------------------------------------------
// addr == 0: BC-local command. ECHO + the CMD_BUS_* roster surface so the Pi's
// provisioning ladder (CLEAR -> REGISTER xN -> SET_POLL -> LIST) reaches DONE.
// ----------------------------------------------------------------------------
static void on_local_shell(void *user, uint16_t req_id, uint16_t cmd,
                           const uint8_t *args, uint8_t alen) {
    (void)user;
    switch (cmd) {
    case CMD_ECHO:
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, args, alen);
        break;

    case CMD_BUS_CLEAR_ROSTER:
        g_roster_n = 0;
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0);
        break;

    case CMD_BUS_REGISTER_SLAVE: {   // [addr][class_id u32][flags]
        uint8_t reason = BUS_REG_OK;
        if (alen >= 6 && g_roster_n < ROSTER_MAX) {
            g_roster[g_roster_n].addr     = args[0];
            g_roster[g_roster_n].class_id = (uint32_t)args[1] | ((uint32_t)args[2] << 8) |
                                            ((uint32_t)args[3] << 16) | ((uint32_t)args[4] << 24);
            g_roster[g_roster_n].flags    = args[5];
            g_roster_n++;
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

    case CMD_BUS_LIST_SLAVES: {      // reply: [total u8][shown u8][addr...]
        uint8_t r[2 + ROSTER_MAX];
        r[0] = g_roster_n; r[1] = g_roster_n;
        for (uint8_t i = 0; i < g_roster_n; i++) r[2 + i] = g_roster[i].addr;
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, r, (uint8_t)(2 + g_roster_n));
        break;
    }
    case CMD_BUS_POLL_ENABLE:        // [on] — autonomous sweep is the next step;
        if (alen >= 1) g_poll_enabled = args[0];   // commands still bridge reactively.
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

    // Bus (southbound).
    bus_phy_init(BUS_DEFAULT_BAUD);
    bus_asm_init(&g_bc, BUS_ADDR_MASTER, true);

    // Host link (northbound).
    host_link_cfg_t cfg = {
        .class_id            = CLASS_ID_BUS_CONTROLLER,
        .instance_id         = BC_INSTANCE_ID,
        .commissioning_state = 1,
        .vid = USB_VID, .pid = USB_PID,
        .fw_version = BC_FW_VERSION, .build_date = BC_BUILD_DATE,
        .schema_hash = BC_SCHEMA_HASH,
    };
    pico_unique_board_id_t uid;
    pico_get_unique_board_id(&uid);             // 8 bytes; zero-pad to 16
    memcpy(cfg.chip_uid, uid.id, sizeof uid.id);
    host_link_init(&g_hl, &cfg);
    host_link_set_callbacks(&g_hl, on_bus_msg, on_local_shell, NULL);

    bool prev_conn = false;
    for (;;) {
        // Host (re)attach: the RP2040 has no DTR-reset, so when a controller
        // closes the port (DTR drop) we re-arm to BOOT — the next controller
        // then gets a fresh REGISTER + sync ladder instead of stale heartbeats.
        bool conn = stdio_usb_connected();
        if (prev_conn && !conn) host_link_reset_boot(&g_hl);
        prev_conn = conn;

        // Drain inbound USB bytes into the protocol decoder.
        int c;
        while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
            host_link_feed(&g_hl, (uint8_t)c);

        // Timed REGISTER (boot) / HEARTBEAT (operational).
        host_link_tick(&g_hl, to_ms_since_boot(get_absolute_time()));

        // Drain staged TX frames to USB (raw — frames contain 0xC0/0xDB).
        uint8_t out[64];
        uint32_t n;
        while ((n = host_link_tx_drain(&g_hl, out, sizeof out)) > 0) {
            for (uint32_t i = 0; i < n; i++) putchar_raw(out[i]);
            stdio_flush();
        }
    }
}
