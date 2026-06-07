// ============================================================================
// app/bus_controller/main.c — ROLE=bus_controller entry point (Pico 2 W).
//
// Core split (FreeRTOS-SMP):
//   core0 = bus task     : runs the arbiter (bus_sched_tick) — PIO/DMA servicing
//                          and the round-robin grant cycle. Time-critical.
//   core1 = uplink task  : USB-CDC host link + self-originated message logic;
//                          feeds frames into the scheduler's queue.
//
// STATUS: SKELETON wiring. The tasks call the real core APIs; the timing-
// critical bodies are stubbed where the PIO PHY is still TODO.
// ============================================================================
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "bus_phy.h"
#include "bus_uplink.h"
#include "bus_roster.h"
#include "bus_sched.h"

static void bus_task(void *arg) {
    (void)arg;
    for (;;) {
        uint8_t serviced = bus_sched_tick();   // one grant slot
        if (serviced == 0) vTaskDelay(pdMS_TO_TICKS(5));  // idle: no enabled node
        else               taskYIELD();
    }
}

static void uplink_task(void *arg) {
    (void)arg;
    uint8_t dest, buf[BUS_PAYLOAD_MAX];
    for (;;) {
        bus_uplink_task();
        int len = bus_uplink_poll(&dest, buf, sizeof(buf));
        if (len > 0) {
            // Host->bus message: queue it for the target node's next grant.
            bus_slave_t *s = bus_roster_find(dest);
            bool tcp = s && (s->flags & BUS_FLAG_TCP);
            bus_sched_queue_tx(dest, BUS_FT_DATA, buf, (uint8_t)len, tcp);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

int main(void) {
    stdio_init_all();
    board_init();
    bus_phy_init(BUS_DEFAULT_BAUD);
    bus_uplink_init();
    bus_roster_init();
    bus_sched_init();

    TaskHandle_t t_bus, t_up;
    xTaskCreate(bus_task,    "bus",    configMINIMAL_STACK_SIZE * 4, NULL, tskIDLE_PRIORITY + 2, &t_bus);
    xTaskCreate(uplink_task, "uplink", configMINIMAL_STACK_SIZE * 4, NULL, tskIDLE_PRIORITY + 1, &t_up);
    vTaskCoreAffinitySet(t_bus, 1u << 0);   // pin the bus loop to core0
    vTaskCoreAffinitySet(t_up,  1u << 1);   // host link on core1

    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}

// --- FreeRTOS static-allocation hooks (SMP) ---------------------------------
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
