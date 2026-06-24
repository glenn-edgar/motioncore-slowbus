// ============================================================================
// node_role.c — SLAVE/node role for the single unified image.
//
// Lifted from the old app/slave/main.c (now deleted): the bus_node responder
// plus the role entry point node_role_run(). It carries NO main() and NO
// FreeRTOS app hooks — those come from the master's app/bus_controller/main.c,
// which is the one image's entry point. main() dispatches here when the config
// identity is a non-master variant.
//
// A node listens for its grant and, in-window, answers the BC. The application
// lives in bus_node_on_data(): dispatch the inner opcode and bus_node_queue()
// the reply. STATUS: skeleton responder (CMD_ECHO) — grows into real node logic.
// ============================================================================
#include <string.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "bus_phy.h"
#include "bus_node.h"
#include "bus_addr.h"
#include "node_role.h"
// The BC injects a host shell-exec to this node as DATA =
// [opcode u16][req_id u16][cmd u16][args]; we answer with DATA =
// [OP_SHELL_REPLY u16][req_id u16][status u8][result...], which the BC relays to
// the host as OP_SHELL_REPLY. The inner cmd is handled by the shared Thread-1
// node_cmd_dispatch (echo / GPIO / interlock); anything else -> UNKNOWN.
#define OP_SHELL_EXEC       0x0109u
#define OP_SHELL_REPLY      0x0011u
#define NODE_SHELL_UNKNOWN  1u
void bus_node_on_data(uint8_t src, const uint8_t *payload, uint8_t len) {
    if (len < 6) return;
    uint16_t op = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    if (op != OP_SHELL_EXEC) return;
    uint16_t req = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
    uint16_t cmd = (uint16_t)payload[4] | ((uint16_t)payload[5] << 8);
    const uint8_t *args = &payload[6];
    uint8_t alen = (uint8_t)(len - 6);

    // C3: app opcodes go to the slave's own chain-tree engine (kbapp), which replies
    // asynchronously on a later POLL via the reply pump. Everything else is handled
    // synchronously below by the shared Thread-1 dispatch.
    if (node_engine_try_route(src, req, cmd, args, alen)) return;

    uint8_t r[BUS_PAYLOAD_MAX]; uint8_t n = 0;
    r[n++] = (uint8_t)(OP_SHELL_REPLY & 0xFF); r[n++] = (uint8_t)(OP_SHELL_REPLY >> 8);
    r[n++] = (uint8_t)(req & 0xFF);            r[n++] = (uint8_t)(req >> 8);

    // Thread-1 unified operate dispatch (shared with the master). The slave has no
    // engine-routed / async extras, so anything not handled here is UNKNOWN.
    uint8_t out[BUS_PAYLOAD_MAX]; uint8_t outlen = 0;
    uint8_t st = node_cmd_dispatch(cmd, args, alen, out, (uint8_t)(BUS_PAYLOAD_MAX - n), &outlen);
    if (st == CMD_NOT_MINE) {
        r[n++] = NODE_SHELL_UNKNOWN;
    } else {
        r[n++] = st;
        for (uint8_t i = 0; i < outlen && n < BUS_PAYLOAD_MAX; i++) r[n++] = out[i];
    }
    bus_node_queue(src, BUS_FT_DATA, r, n);
}

static void node_task(void *arg) {
    (void)arg;
    for (;;) {
        bus_node_task();        // drain the IRQ-fed RX ring; ship queued replies
        // Sleep one tick (1 ms) rather than taskYIELD(): taskYIELD only yields to
        // EQUAL-or-higher priority, which starved the lower-priority USB worker so
        // the picotool reset interface went dead (a slave couldn't be reflashed
        // without the BOOTSEL button). A real sleep lets the USB worker (and idle)
        // run. RX is IRQ-buffered so nothing is missed in the gap, and the 1 ms wake
        // latency sits well inside the ~2 ms per-node POLL window (window_us=2000).
        vTaskDelay(1);
    }
}

void node_role_run(uint8_t addr, uint32_t baud) {
    // stdio_init_all() + the identity read already ran in main(); do the
    // node-specific init in the same order the standalone slave used.
    board_init();
    bus_phy_init(baud ? baud : BUS_DEFAULT_BAUD);   // config 'sp', else default
    bus_node_init(addr);

    // Role-agnostic Thread 2: the slave runs the interlock on its own local I/O
    // (hwio roles + ilc0..ilc9), exposed over the bus via bus_node_on_data above.
    node_thread2_start();

    // C3: the slave runs the chain-tree engine too, so its own kbapp answers app
    // messages (engine<->engine). Peripherals are already up (node_thread2_start).
    node_engine_start();

    TaskHandle_t t;
    xTaskCreate(node_task, "node", configMINIMAL_STACK_SIZE * 4, NULL, tskIDLE_PRIORITY + 2, &t);
    vTaskCoreAffinitySet(t, 1u << 0);

    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
