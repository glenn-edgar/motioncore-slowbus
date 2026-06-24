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
#include "interlock/interlock.h"   // run the interlock on the slave too (role-agnostic Thread 2)

// The BC injects a host shell-exec to this node as DATA =
// [opcode u16][req_id u16][cmd u16][args]; we answer with DATA =
// [OP_SHELL_REPLY u16][req_id u16][status u8][result...], which the BC relays to
// the host as OP_SHELL_REPLY. Handles CMD_ECHO (returns the args); any other
// cmd -> SHELL_UNKNOWN_CMD.
#define OP_SHELL_EXEC       0x0109u
#define OP_SHELL_REPLY      0x0011u
#define NODE_CMD_ECHO       0x0001u
#define NODE_SHELL_OK       0u
#define NODE_SHELL_UNKNOWN  1u
// Test/interlock command opcodes — mirror app/bus_controller/main.c so the host can
// drive the SAME trip/latch/clear sequence on the slave over the bus.
#define CMD_GPIO_WRITE       0x0101u
#define CMD_GPIO_READ        0x0102u
#define CMD_INTERLOCK_CLEAR  0x0210u
#define CMD_INTERLOCK_STATUS 0x0211u
#define SHELL_OK             0u

void bus_node_on_data(uint8_t src, const uint8_t *payload, uint8_t len) {
    if (len < 6) return;
    uint16_t op = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    if (op != OP_SHELL_EXEC) return;
    uint16_t req = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
    uint16_t cmd = (uint16_t)payload[4] | ((uint16_t)payload[5] << 8);
    const uint8_t *args = &payload[6];
    uint8_t alen = (uint8_t)(len - 6);

    uint8_t r[BUS_PAYLOAD_MAX]; uint8_t n = 0;
    r[n++] = (uint8_t)(OP_SHELL_REPLY & 0xFF); r[n++] = (uint8_t)(OP_SHELL_REPLY >> 8);
    r[n++] = (uint8_t)(req & 0xFF);            r[n++] = (uint8_t)(req >> 8);
    if (cmd == NODE_CMD_ECHO) {
        r[n++] = NODE_SHELL_OK;
        if (alen > (uint8_t)(BUS_PAYLOAD_MAX - n)) alen = (uint8_t)(BUS_PAYLOAD_MAX - n);
        memcpy(&r[n], args, alen); n = (uint8_t)(n + alen);
    } else if (cmd == CMD_GPIO_WRITE || cmd == CMD_GPIO_READ) {
        uint8_t res[4], reslen = 0;
        r[n++] = node_hil_gpio(cmd, args, alen, res, &reslen);   // role-validated
        for (uint8_t i = 0; i < reslen && n < BUS_PAYLOAD_MAX; i++) r[n++] = res[i];
    } else if (cmd == CMD_INTERLOCK_CLEAR) {
        interlock_request_global_clear();
        r[n++] = SHELL_OK;
    } else if (cmd == CMD_INTERLOCK_STATUS) {
        r[n++] = SHELL_OK;
        uint8_t gveto = 0;                       // global veto = OR of armed+latched slots
        for (uint8_t s = 0; s < INTERLOCK_MAX_SLOTS; s++)
            if (g_interlock_persist.slots[s].state == INTERLOCK_SLOT_ARMED &&
                g_interlock_persist.slots[s].latched) gveto = 1;
        r[n++] = gveto;
        uint8_t cnt_at = n++, cnt = 0;
        for (uint8_t s = 0; s < INTERLOCK_MAX_SLOTS && (uint8_t)(n + 4) <= BUS_PAYLOAD_MAX; s++) {
            if (g_interlock_persist.slots[s].state == INTERLOCK_SLOT_EMPTY) continue;
            r[n++] = s;
            r[n++] = g_interlock_persist.slots[s].state;
            r[n++] = g_interlock_persist.inst[s].tf_state;   // live boolean
            r[n++] = g_interlock_persist.slots[s].latched;   // sticky trip
            cnt++;
        }
        r[cnt_at] = cnt;
    } else {
        r[n++] = NODE_SHELL_UNKNOWN;
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

    TaskHandle_t t;
    xTaskCreate(node_task, "node", configMINIMAL_STACK_SIZE * 4, NULL, tskIDLE_PRIORITY + 2, &t);
    vTaskCoreAffinitySet(t, 1u << 0);

    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
