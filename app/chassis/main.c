// ============================================================================
// app/chassis/main.c — slow_bus node RTOS chassis (FreeRTOS-SMP, dual core)
//                      + SAFERTOS/WITTENSTEIN-inspired defensive baseline.
//
// Threads (priority: app_engine 3 > bus_control = uplink 2 > watchdog 1):
//   core0 (pinned)  bus_control  10 ms tick   light: ISR/DMA owns bus timing  [stub]
//   core0 (pinned)  uplink       10 ms tick   USB-CDC / wifi-proxy north link [stub]
//   core1 (pinned)  app_engine   10 ms tick   chain_tree tick + interlocks    [stub]
//   float           watchdog    100 ms        arms WDT, checks heartbeats, pets
//
// Hardening (the SAMD21 lesson, applied day-1 — see [[defensive_baseline_recipe]]):
//   * Watchdog judges liveness on a FREE-RUNNING hardware clock (time_us_32),
//     independent of the FreeRTOS tick it is guarding.
//   * .noinit crash slot survives WDT/soft reset (RP2040 SRAM is not cleared by
//     reset) -> boot post-mortem: boot count, last reset cause, panic line/msg.
//   * Recorded panic path (configASSERT, stack-overflow, malloc-fail) -> crash
//     slot + immediate reset, reported on next boot.
//   * system_self_check() before the workload starts.
//   * Per-task stack high-water-mark in the status line.
//
// WDT: pause_on_debug MUST be false on RP2040 or it never fires (verified).
// ============================================================================
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "FreeRTOS.h"
#include "task.h"
#include "commission.h"

// ---- reset-cause codes (stored in the crash slot) --------------------------
enum { RST_POWER = 0, RST_WDT, RST_PANIC, RST_ASSERT, RST_STACK, RST_MALLOC };
static const char *const RST_NAME[] = { "POWER", "WDT", "PANIC", "ASSERT", "STACK", "MALLOC" };

// ---- crash slot: .uninitialized_data survives a watchdog/soft reset ---------
#define CRASH_MAGIC 0x5B0BC0DEu
typedef struct {
    uint32_t magic;
    uint32_t boot_count;
    uint32_t last_cause;
    uint32_t panic_code;     // assert line / panic code
    char     panic_msg[24];
} crash_slot_t;
static crash_slot_t g_crash __attribute__((section(".uninitialized_data")));
static volatile uint8_t g_bus_addr = COMMISSION_ADDR_NONE;  // from commission block

// ---- monitored-thread heartbeats (one writer per slot => SMP-safe) ----------
enum { HB_BUS = 0, HB_UPLINK, HB_APP, HB_COUNT };
static volatile uint32_t g_hb[HB_COUNT];      // liveness counter (display)
static volatile uint32_t g_hb_us[HB_COUNT];   // free-running stamp of last advance
static const char *const HB_NAME[HB_COUNT] = { "bus_control", "uplink", "app_engine" };

static TaskHandle_t t_bus, t_up, t_app, t_wd; // file-scope so the wd can query HWM

#define WORKER_TICK_MS     10        // worker cadence == future engine delta_time
#define WD_PERIOD_MS      100        // watchdog wake period
#define WD_HW_TIMEOUT_MS 4000        // HW WDT timeout (generous during bring-up)
#define HB_STALE_US   200000u        // 200 ms real-time => thread declared stalled

// ---- recorded panic: crash slot + immediate reset --------------------------
static void chassis_panic(uint32_t cause, uint32_t code, const char *msg) {
    g_crash.last_cause = cause;
    g_crash.panic_code = code;
    g_crash.panic_msg[0] = 0;
    if (msg) { strncpy(g_crash.panic_msg, msg, sizeof(g_crash.panic_msg) - 1);
               g_crash.panic_msg[sizeof(g_crash.panic_msg) - 1] = 0; }
    watchdog_reboot(0, 0, 0);        // immediate reset to normal boot
    for (;;) { tight_loop_contents(); }
}
// configASSERT hook (declared extern in FreeRTOSConfig.h)
void chassis_assert(int line) { chassis_panic(RST_ASSERT, (uint32_t)line, "configASSERT"); }

// ---- boot-time integrity check ---------------------------------------------
static bool system_self_check(void) {
    // RAM read/write integrity on a scratch word.
    static volatile uint32_t probe;
    probe = 0xA5A5A5A5u; if (probe != 0xA5A5A5A5u) return false;
    probe = 0x5A5A5A5Au; if (probe != 0x5A5A5A5Au) return false;
    // TODO(commission block): validate identity magic+version+CRC here.
    return true;
}

// ---- worker stubs: bump heartbeat (counter + free-running stamp), delay ------
static void bus_control_task(void *arg) {
    (void)arg; TickType_t next = xTaskGetTickCount();
    for (;;) {
        g_hb[HB_BUS]++; g_hb_us[HB_BUS] = time_us_32();
        // TODO: drain PHY RX frames + drive poll cadence.
        vTaskDelayUntil(&next, pdMS_TO_TICKS(WORKER_TICK_MS));
    }
}
static void uplink_task(void *arg) {
    (void)arg; TickType_t next = xTaskGetTickCount();
    for (;;) {
        g_hb[HB_UPLINK]++; g_hb_us[HB_UPLINK] = time_us_32();
        // TODO: service USB-CDC / wifi-proxy; bridge host <-> bus_control.
        vTaskDelayUntil(&next, pdMS_TO_TICKS(WORKER_TICK_MS));
    }
}
static void app_engine_task(void *arg) {
    (void)arg; TickType_t next = xTaskGetTickCount();
    uint32_t iter = 0;
    for (;;) {
#ifdef CHASSIS_STALL_TEST
        if (iter < (10000u / WORKER_TICK_MS)) { g_hb[HB_APP]++; g_hb_us[HB_APP] = time_us_32(); }
#else
        g_hb[HB_APP]++; g_hb_us[HB_APP] = time_us_32();
#endif
        iter++;
        // TODO: chain_tree runtime tick (delta = WORKER_TICK_MS/1000.0) + KBs.
        vTaskDelayUntil(&next, pdMS_TO_TICKS(WORKER_TICK_MS));
    }
}

// ---- watchdog: free-running-clock liveness gate -> pet HW WDT ---------------
static void watchdog_task(void *arg) {
    (void)arg;
    watchdog_enable(WD_HW_TIMEOUT_MS, false);   // false: must bite (RP2040)
    printf("[wd] HW watchdog armed @ %u ms\n", (unsigned)WD_HW_TIMEOUT_MS);

    uint32_t ticks = 0;
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        uint32_t now = time_us_32();            // independent of the FreeRTOS tick
        bool all_alive = true;
        for (int i = 0; i < HB_COUNT; i++) {
            if ((uint32_t)(now - g_hb_us[i]) > HB_STALE_US) {
                all_alive = false;
                printf("[wd] STALL: %s silent %u us -- withholding pet\n",
                       HB_NAME[i], (unsigned)(now - g_hb_us[i]));
            }
        }
        if (all_alive) {
            watchdog_update();                  // pet
            if ((ticks % 10u) == 0u)            // ~1 s status line
                printf("[wd] ok up=%u ms  hb b=%u u=%u a=%u  stk b=%u u=%u a=%u w=%u  caddr=0x%02X boot#%u rst=%s\n",
                       (unsigned)(ticks * WD_PERIOD_MS),
                       (unsigned)g_hb[HB_BUS], (unsigned)g_hb[HB_UPLINK], (unsigned)g_hb[HB_APP],
                       (unsigned)uxTaskGetStackHighWaterMark(t_bus),
                       (unsigned)uxTaskGetStackHighWaterMark(t_up),
                       (unsigned)uxTaskGetStackHighWaterMark(t_app),
                       (unsigned)uxTaskGetStackHighWaterMark(t_wd),
                       (unsigned)g_bus_addr,
                       (unsigned)g_crash.boot_count, RST_NAME[g_crash.last_cause]);
        }
        ticks++;
        vTaskDelayUntil(&next, pdMS_TO_TICKS(WD_PERIOD_MS));
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(200);   // let USB-CDC enumerate before the first prints

    // ---- crash slot: cold vs warm boot, determine this reset's cause -------
    bool cold = (g_crash.magic != CRASH_MAGIC);
    if (cold) {
        memset(&g_crash, 0, sizeof(g_crash));
        g_crash.magic = CRASH_MAGIC;
        g_crash.last_cause = RST_POWER;
    } else if (watchdog_enable_caused_reboot()) {
        g_crash.last_cause = RST_WDT;          // HW WDT timeout (panic paths set their own)
    }
    g_crash.boot_count++;

    printf("\n[boot] slow_bus chassis  boot#%u  cause=%s",
           (unsigned)g_crash.boot_count, RST_NAME[g_crash.last_cause]);
    if (g_crash.last_cause == RST_PANIC || g_crash.last_cause == RST_ASSERT ||
        g_crash.last_cause == RST_STACK || g_crash.last_cause == RST_MALLOC)
        printf(" code=%u msg=%s", (unsigned)g_crash.panic_code, g_crash.panic_msg);
    printf("  (cores=%d tick=%u ms)\n", (int)configNUMBER_OF_CORES, (unsigned)WORKER_TICK_MS);

    if (!system_self_check()) {
        printf("[boot] SELF-CHECK FAILED -- halting\n");
        chassis_panic(RST_PANIC, 0, "self_check");
    }

    // ---- device identity (protected commission block) ---------------------
    commission_t id;
    if (commission_load(&id)) {
        printf("[boot] commissioned: class=%u instance=%u  bus_addr=0x%02X  seq=%u\n",
               id.class_id, id.instance_id, commission_bus_addr(), (unsigned)id.seq);
    } else {
        printf("[boot] UNCOMMISSIONED (bus_addr=0x%02X) -- safe mode, will not join bus\n",
               commission_bus_addr());
#ifdef CHASSIS_COMMISSION_TEST
        // One-shot test write (pre-scheduler, single-core => flash-safe).
        printf("[boot] writing test commission block (class=2 instance=44)...\n");
        commission_write(2, 44);
        if (commission_load(&id))
            printf("[boot] now commissioned: class=%u instance=%u bus_addr=0x%02X seq=%u\n",
                   id.class_id, id.instance_id, commission_bus_addr(), (unsigned)id.seq);
    }
#else
    }
#endif
    g_bus_addr = commission_bus_addr();

    uint32_t t0 = time_us_32();
    for (int i = 0; i < HB_COUNT; i++) g_hb_us[i] = t0;   // seed so wd doesn't false-stall

    xTaskCreate(bus_control_task, "bus",    configMINIMAL_STACK_SIZE * 4, NULL, 2, &t_bus);
    xTaskCreate(uplink_task,      "uplink", configMINIMAL_STACK_SIZE * 4, NULL, 2, &t_up);
    xTaskCreate(app_engine_task,  "app",    configMINIMAL_STACK_SIZE * 6, NULL, 3, &t_app);
    xTaskCreate(watchdog_task,    "wd",     configMINIMAL_STACK_SIZE * 2, NULL, 1, &t_wd);
    vTaskCoreAffinitySet(t_bus, 1u << 0);
    vTaskCoreAffinitySet(t_up,  1u << 0);
    vTaskCoreAffinitySet(t_app, 1u << 1);
    // watchdog: no affinity -> floats to whichever core has slack.

    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}

// ---- FreeRTOS hooks -> recorded panic --------------------------------------
void vApplicationMallocFailedHook(void) { chassis_panic(RST_MALLOC, 0, "malloc"); }
void vApplicationStackOverflowHook(TaskHandle_t t, char *n) {
    (void)t; chassis_panic(RST_STACK, 0, n ? n : "?");
}

// Static allocation for the idle/timer tasks (SMP: one passive idle per core).
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
