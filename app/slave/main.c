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
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "bus_phy.h"
#include "bus_node.h"
#include "bus_addr.h"

#ifndef NODE_ADDR
#define NODE_ADDR 0x01u   // TODO: replace with commissioned identity
#endif

// Example application hook: echo any DATA back to its sender.
void bus_node_on_data(uint8_t src, const uint8_t *payload, uint8_t len) {
    bus_node_queue(src, BUS_FT_DATA, payload, len);
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
    bus_node_init(NODE_ADDR);

    TaskHandle_t t;
    xTaskCreate(node_task, "node", configMINIMAL_STACK_SIZE * 4, NULL, tskIDLE_PRIORITY + 2, &t);
    vTaskCoreAffinitySet(t, 1u << 0);

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
