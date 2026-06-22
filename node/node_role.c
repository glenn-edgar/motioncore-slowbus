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
// the host as OP_SHELL_REPLY. Handles CMD_ECHO (returns the args); any other
// cmd -> SHELL_UNKNOWN_CMD.
#define OP_SHELL_EXEC       0x0109u
#define OP_SHELL_REPLY      0x0011u
#define NODE_CMD_ECHO       0x0001u
#define NODE_SHELL_OK       0u
#define NODE_SHELL_UNKNOWN  1u

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
    } else {
        r[n++] = NODE_SHELL_UNKNOWN;
    }
    bus_node_queue(src, BUS_FT_DATA, r, n);
}

static void node_task(void *arg) {
    (void)arg;
    for (;;) {
        bus_node_task();
        taskYIELD();
    }
}

void node_role_run(uint8_t addr) {
    // stdio_init_all() + the identity read already ran in main(); do the
    // node-specific init in the same order the standalone slave used.
    board_init();
    bus_phy_init(BUS_DEFAULT_BAUD);   // TODO: config-driven bus speed (next step)
    bus_node_init(addr);

    TaskHandle_t t;
    xTaskCreate(node_task, "node", configMINIMAL_STACK_SIZE * 4, NULL, tskIDLE_PRIORITY + 2, &t);
    vTaskCoreAffinitySet(t, 1u << 0);

    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
