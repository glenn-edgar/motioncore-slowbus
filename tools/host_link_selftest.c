// host_link_selftest.c — drive the northbound dongle protocol off-target.
//
// Simulates the Pi-side linux/bus_controller: encodes m2s frames into host_link,
// decodes the s2m frames it stages, and checks the sync ladder + routing:
//   1. boot announces OP_REGISTER (38-byte v2 identity, class=bus_controller)
//   2. GET_MANIFEST -> OP_MANIFEST_REPLY (schema_hash, fw, m2s opcode list)
//   3. OPERATIONAL_BEGIN -> state OPERATIONAL -> OP_HEARTBEAT on tick
//   4. PING -> OP_PONG
//   5. addr==0 OP_SHELL_EXEC -> on_local_shell fires, OP_SHELL_REPLY staged
//   6. addr!=0 frame -> on_bus_msg fires with the right dest/opcode/body
//
// Build: make hltest   (see tools/Makefile)
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "host_link.h"
#include "opcodes.h"

#define CLASS_ID_BUS_CONTROLLER 0x5E589000u
#define CMD_ECHO 0x0001u

static int g_fail;
#define CHECK(c, msg) do { if (c) printf("  [PASS] %s\n", msg); \
                           else { printf("  [FAIL] %s\n", msg); g_fail++; } } while (0)

// --- capture callbacks ------------------------------------------------------
static int      g_bus_called; static uint8_t g_bus_dest; static uint16_t g_bus_op;
static uint8_t  g_bus_body[64]; static uint8_t g_bus_len;
static int      g_shell_called; static uint16_t g_shell_rid, g_shell_cmd;

static host_link_t H;

static void on_bus(void *u, uint8_t dest, uint16_t op, const uint8_t *body, uint8_t len) {
    (void)u; g_bus_called++; g_bus_dest = dest; g_bus_op = op; g_bus_len = len;
    memcpy(g_bus_body, body, len);
}
static void on_shell(void *u, uint16_t rid, uint16_t cmd, const uint8_t *args, uint8_t alen) {
    (void)u; g_shell_called++; g_shell_rid = rid; g_shell_cmd = cmd;
    host_link_shell_reply(&H, rid, 0, args, alen);   // echo
}

// Encode an m2s frame and feed it byte-by-byte into the link.
static void feed_m2s(uint8_t addr, uint16_t cmd, const uint8_t *payload, uint8_t len) {
    uint8_t ring_buf[512]; frame_ring_t ring; frame_ring_init(&ring, ring_buf, 512);
    frame_meta_t m = { .addr = addr, .cmd = cmd, .seq = 0, .payload_len = len };
    assert(frame_encode_m2s(&m, payload, &ring) == 0);
    uint8_t b;
    while (frame_ring_read_byte(&ring, &b)) host_link_feed(&H, b);
}

// Drain all staged s2m frames; record the LAST one + a hit-count per opcode.
static int g_seen[0x40];
static frame_meta_t g_last; static uint8_t g_last_pl[256];
static void drain_s2m(void) {
    uint8_t buf[1024];
    uint32_t n = host_link_tx_drain(&H, buf, sizeof buf);
    frame_decoder_t d; frame_decoder_init(&d, FRAME_DIR_S2M);
    frame_meta_t m; uint8_t pl[256];
    for (uint32_t i = 0; i < n; i++) {
        if (frame_decoder_feed(&d, buf[i], &m, pl) == FRAME_DECODE_FRAME_READY) {
            if (m.cmd < 0x40) g_seen[m.cmd]++;
            g_last = m; memcpy(g_last_pl, pl, m.payload_len);
        }
    }
}

int main(void) {
    host_link_cfg_t cfg = {
        .class_id = CLASS_ID_BUS_CONTROLLER, .instance_id = 42, .commissioning_state = 1,
        .vid = 0x2E8A, .pid = 0x000A, .fw_version = 0x100, .build_date = 20260607,
        .schema_hash = 0x51B00001u,
    };
    for (int i = 0; i < 16; i++) cfg.chip_uid[i] = (uint8_t)(0xA0 + i);
    host_link_init(&H, &cfg);
    host_link_set_callbacks(&H, on_bus, on_shell, NULL);

    printf("== host_link sync ladder ==\n");

    // 1. boot REGISTER on first tick.
    host_link_tick(&H, 0, true);
    drain_s2m();
    CHECK(g_seen[OP_REGISTER] == 1, "boot announces OP_REGISTER");
    CHECK(g_last.payload_len == 38 && g_last_pl[0] == 2, "REGISTER is 38-byte v2");
    CHECK((uint32_t)(g_last_pl[1] | (g_last_pl[2]<<8) | (g_last_pl[3]<<16) |
          ((uint32_t)g_last_pl[4]<<24)) == CLASS_ID_BUS_CONTROLLER, "REGISTER class_id=bus_controller");

    // 2. REGISTER_ACK -> L1, then GET_MANIFEST -> MANIFEST_REPLY.
    feed_m2s(0, OP_REGISTER_ACK, NULL, 0);
    CHECK(host_link_state(&H) == HL_L1_ACKED, "REGISTER_ACK -> L1_ACKED");
    feed_m2s(0, OP_GET_MANIFEST, NULL, 0);
    drain_s2m();
    CHECK(g_seen[OP_MANIFEST_REPLY] == 1, "GET_MANIFEST -> MANIFEST_REPLY");
    CHECK(g_last.payload_len >= 9 && g_last_pl[8] == 7, "manifest lists 7 m2s opcodes");

    // 3. OPERATIONAL_BEGIN -> OPERATIONAL -> HEARTBEAT on tick.
    feed_m2s(0, OP_OPERATIONAL_BEGIN, NULL, 0);
    CHECK(host_link_state(&H) == HL_OPERATIONAL, "OPERATIONAL_BEGIN -> OPERATIONAL");
    host_link_tick(&H, 1000, true);
    drain_s2m();
    CHECK(g_seen[OP_HEARTBEAT] >= 1, "OPERATIONAL emits OP_HEARTBEAT");

    // 4. PING -> PONG.
    memset(g_seen, 0, sizeof g_seen);
    feed_m2s(0, OP_PING, NULL, 0);
    drain_s2m();
    CHECK(g_seen[OP_PONG] == 1, "PING -> PONG");

    // 5. addr==0 SHELL_EXEC -> on_local_shell + SHELL_REPLY echo.
    memset(g_seen, 0, sizeof g_seen);
    { uint8_t body[7] = {0x34,0x12, (uint8_t)CMD_ECHO,0x00, 'H','I','!'};
      feed_m2s(0, OP_SHELL_EXEC, body, sizeof body);
      drain_s2m();
      CHECK(g_shell_called == 1 && g_shell_rid == 0x1234 && g_shell_cmd == CMD_ECHO, "SHELL_EXEC -> on_local_shell");
      CHECK(g_seen[OP_SHELL_REPLY] == 1 && g_last_pl[0] == 0x34 && g_last_pl[1] == 0x12 &&
            g_last_pl[2] == 0 && g_last_pl[3]=='H', "SHELL_REPLY echoes req_id+status+result"); }

    // 6. addr!=0 -> on_bus_msg.
    { uint8_t body[6] = {0x02,0x00, 0x01,0x00, 0xAA,0xBB};
      feed_m2s(3, OP_BUS_EXEC, body, sizeof body);
      CHECK(g_bus_called == 1 && g_bus_dest == 3 && g_bus_op == OP_BUS_EXEC && g_bus_len == 6,
            "addr!=0 -> on_bus_msg(dest=3,OP_BUS_EXEC)"); }

    printf(g_fail ? "\n== %d FAILED ==\n" : "\n== all passed ==\n", g_fail);
    return g_fail ? 1 : 0;
}
