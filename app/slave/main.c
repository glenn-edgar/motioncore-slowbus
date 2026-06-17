// ============================================================================
// app/slave/main.c — ROLE=slave entry point (Pico 2 W node).
//
// A node listens for its grant and, in-window, answers the BC and/or sends peer
// frames to other nodes. The application lives in bus_node_on_data(): dispatch
// the inner opcode and bus_node_queue() replies or peer messages.
//
// STATUS: SKELETON. The bus_node window logic is in core/bus_node.c; this wires
// it to the PIO PHY and a single FreeRTOS task. NODE_ADDR is a build-time stand-in
// until commissioning/identity is ported.
// ============================================================================
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "bus_phy.h"
#include "bus_node.h"
#include "bus_addr.h"
#include "boot_identity.h"   // per-unit address from the config-FS (two-step flash)
#include "cfg_file.h"

#ifndef NODE_ADDR
#define NODE_ADDR 0x01u   // fallback when no idnt is flashed (uncommissioned)
#endif

// Minimal node command responder. The BC injects a host shell-exec to this node
// as DATA = [opcode u16][req_id u16][cmd u16][args]; we answer with
// DATA = [OP_SHELL_REPLY u16][req_id u16][status u8][result...], which the BC
// relays to the host as OP_SHELL_REPLY. Handles CMD_ECHO (returns the args); any
// other cmd -> SHELL_UNKNOWN_CMD. (Skeleton app -- grows into the real node logic.)
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

int main(void) {
    stdio_init_all();
    board_init();
    bus_phy_init(BUS_DEFAULT_BAUD);

    // Per-unit RS-485 address from the config-FS identity (idnt), validated against
    // this image's chip/variant and the board UUID. Policy mirrors the BC:
    //   OK       -> use ident.addr (commissioned).
    //   MISSING  -> fall back to the baked NODE_ADDR (uncommissioned bring-up).
    //   MISMATCH -> REFUSE: don't init the node, so it stays silent on the bus
    //               (the BC ages it out as DEAD). A slave has no USB diagnostics
    //               yet, so silence is the safe refusal. TODO: layout guard + a
    //               status pin/LED once the node app grows past the skeleton.
    identity_t ident;
    int rc = boot_read_identity(&ident);
    uint8_t addr = NODE_ADDR;
    bool refused = false;
    if (rc == IDENT_OK)             addr = ident.addr;
    else if (rc != IDENT_ERR_MISSING) refused = true;

    if (!refused) {
        bus_node_init(addr);
        TaskHandle_t t;
        xTaskCreate(node_task, "node", configMINIMAL_STACK_SIZE * 4, NULL, tskIDLE_PRIORITY + 2, &t);
        vTaskCoreAffinitySet(t, 1u << 0);
    }

    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}

// --- FreeRTOS static-allocation hooks (SMP) ---------------------------------
// configASSERT() -> chassis_assert() (FreeRTOSConfig.h). The skeleton has no
// crash slot / panic plumbing yet, so reboot to recover (matches the BC's
// watchdog_reboot policy) rather than wedging the node on the bus.
void chassis_assert(int line) { (void)line; watchdog_reboot(0, 0, 0); for (;;) { tight_loop_contents(); } }
void vApplicationMallocFailedHook(void) { for (;;); }
void vApplicationStackOverflowHook(TaskHandle_t t, char *n) { (void)t; (void)n; for (;;); }

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stk, uint32_t *sz) {
    static StaticTask_t t; static StackType_t s[configMINIMAL_STACK_SIZE];
    *tcb = &t; *stk = s; *sz = configMINIMAL_STACK_SIZE;
}
void vApplicationGetPassiveIdleTaskMemory(StaticTask_t **tcb, StackType_t **stk,
                                          uint32_t *sz, BaseType_t core) {
    static StaticTask_t t[configNUMBER_OF_CORES - 1];
    static StackType_t  s[configNUMBER_OF_CORES - 1][configMINIMAL_STACK_SIZE];
    *tcb = &t[core]; *stk = s[core]; *sz = configMINIMAL_STACK_SIZE;
}
void vApplicationGetTimerTaskMemory(StaticTask_t **tcb, StackType_t **stk, uint32_t *sz) {
    static StaticTask_t t; static StackType_t s[configTIMER_TASK_STACK_DEPTH];
    *tcb = &t; *stk = s; *sz = configTIMER_TASK_STACK_DEPTH;
}
