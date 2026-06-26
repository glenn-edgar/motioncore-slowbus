// ============================================================================
// app/vib_node/main.c — Pico 2 W (RP2350) vibration / condition-monitoring node.
//
// Step 3b.1 SKELETON. Reuses the shared bus core + node_role responder; the
// RP2040 bus_controller image is left UNTOUCHED (Glenn: too different). This app
// provides the four role hooks node_role.c calls (node_thread2_start /
// node_engine_start / node_engine_try_route / node_cmd_dispatch) as STUBS:
//   - echo works (so the node answers the bus),
//   - interlock + chain-tree engine are no-ops for now.
//
// What grows here next (separate steps):
//   - measurement layer (il_hal_pico2): FIXED 20kHz center-capture ADC -> FFT /
//     cepstrum, a BUILD-CONFIG SPI device chain (vibration-analysis | 9DOF IMU),
//     and I2C sensor values -> measurement scalars,
//   - the COMMON interlock engine (interlock.c) reading those scalars,
//   - the chain-tree engine (kbapp).
// ============================================================================
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "bus_addr.h"
#include "boot_identity.h"   // identity_t, boot_read_identity, IDENT_*
#include "cfg_file.h"        // cfg_layout_ok
#include "variants.h"        // variant_is_master
#include "node_role.h"       // node_role_run + the four hooks below + CMD_NOT_MINE

// SHELL_* status (mirrors app/bus_controller/main.c; only the ones we use here).
#define SHELL_OK         0u
#define NODE_CMD_ECHO    0x0001u

// ---- role hooks node_role.c calls (STUBS for 3b.1) -------------------------
void node_thread2_start(void) {
    // TODO(step 5): start the measurement chains (ADC FFT/cepstrum, SPI device,
    // I2C) + the COMMON interlock engine on those scalars. No-op for now.
}
void node_engine_start(void) {
    // TODO(step 3b/later): bring up the chain-tree engine (kbapp). No-op for now.
}
bool node_engine_try_route(uint8_t src, uint16_t req, uint16_t cmd,
                           const uint8_t *args, uint8_t alen) {
    (void)src; (void)req; (void)cmd; (void)args; (void)alen;
    return false;            // no engine yet -> fall through to node_cmd_dispatch
}
uint8_t node_cmd_dispatch(uint16_t cmd, const uint8_t *args, uint8_t alen,
                          uint8_t *out, uint8_t cap, uint8_t *outlen) {
    *outlen = 0;
    if (cmd == NODE_CMD_ECHO) {                       // echo args back
        uint8_t k = (alen > cap) ? cap : alen;
        memcpy(out, args, k); *outlen = k; return SHELL_OK;
    }
    return CMD_NOT_MINE;                              // GPIO/interlock come later
}

// ---- liveness beacon (3b.1: confirm the node stays up on RP2350) -----------
static void heartbeat_task(void *arg) {
    (void)arg;
    for (uint32_t i = 0; ; i++) {
        printf("[vib_node] alive %u  up=%u ms  cores=%d\n",
               (unsigned)i, (unsigned)board_millis(), (int)configNUMBER_OF_CORES);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ---- FreeRTOS / chassis hooks (this image is its own entry point) ----------
void chassis_panic(uint32_t cause, uint32_t code) {
    (void)cause; (void)code;
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}
void chassis_assert(int line) { chassis_panic(2u, (uint32_t)line); }
void vApplicationMallocFailedHook(void) { for (;;) tight_loop_contents(); }
void vApplicationStackOverflowHook(TaskHandle_t t, char *n) { (void)t; (void)n; for (;;) tight_loop_contents(); }
void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stk, uint32_t *sz) {
    static StaticTask_t t; static StackType_t s[configMINIMAL_STACK_SIZE];
    *tcb = &t; *stk = s; *sz = configMINIMAL_STACK_SIZE;
}
void vApplicationGetPassiveIdleTaskMemory(StaticTask_t **tcb, StackType_t **stk, uint32_t *sz, BaseType_t core) {
    static StaticTask_t t[configNUMBER_OF_CORES - 1];
    static StackType_t  s[configNUMBER_OF_CORES - 1][configMINIMAL_STACK_SIZE];
    *tcb = &t[core]; *stk = s[core]; *sz = configMINIMAL_STACK_SIZE;
}
void vApplicationGetTimerTaskMemory(StaticTask_t **tcb, StackType_t **stk, uint32_t *sz) {
    static StaticTask_t t; static StackType_t s[configTIMER_TASK_STACK_DEPTH];
    *tcb = &t; *stk = s; *sz = configTIMER_TASK_STACK_DEPTH;
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);                       // let USB-CDC enumerate for the banner

    extern char __flash_binary_end;       // SDK linker symbol: end of the image
    if (!cfg_layout_ok(&__flash_binary_end)) chassis_panic(3u, 0x10);

    identity_t ident;
    int rc = boot_read_identity(&ident);
    uint8_t  addr = (rc == IDENT_OK) ? ident.addr : BUS_ADDR_MASTER;
    uint32_t baud = (rc == IDENT_OK && ident.baud) ? ident.baud : BUS_DEFAULT_BAUD;

    printf("\n[vib_node] Pico 2 W boot  idnt_rc=%d  addr=0x%02X  variant=%u  baud=%u  cores=%d\n",
           rc, addr, (rc == IDENT_OK) ? ident.variant : 0xFF, (unsigned)baud,
           (int)configNUMBER_OF_CORES);
    if (rc != IDENT_OK)
        printf("[vib_node] UNCOMMISSIONED (idnt_rc=%d) -- running responder for bring-up\n", rc);
    else if (variant_is_master(ident.variant))
        printf("[vib_node] master variant -- master path TBD; running node responder for now\n");

    // 3b.1: run the shared node responder (PHY + bus_node + scheduler). The
    // heartbeat task is created first so it runs once node_role_run starts the
    // scheduler. node_role_run() never returns.
    xTaskCreate(heartbeat_task, "hb", configMINIMAL_STACK_SIZE * 2, NULL, 1, NULL);
    node_role_run(addr, baud);
    for (;;) tight_loop_contents();
}
