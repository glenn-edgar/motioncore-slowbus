/* RP2040 port of the chain_tree timer (cfl_timer_system.h) — replaces the host
 * Linux/calendar timer. Provides the subset cfl_runtime uses: create / wait /
 * get_timestamp. wait() is also the per-tick seam: after the FreeRTOS delay it
 * calls cfl_embed_pre_tick() so the firmware can inject inter-core events into the
 * runtime's event queue before the engine processes them this tick. */
#include "cfl_timer_system.h"
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* Firmware override: drain the core0->core1 down-queue and cfl_send_event() the
 * resulting commands into the runtime. Default no-op (e.g. host link tests). */
__attribute__((weak)) void cfl_embed_pre_tick(void) {}

struct cfl_timer_context { double wait_seconds; int32_t last_second; };
static struct cfl_timer_context g_ctx;

static double now_seconds(void) {
    return (double)to_us_since_boot(get_absolute_time()) / 1e6;
}

cfl_timer_handle_t cfl_timer_create(double wait_seconds, cfl_perm_t *perm) {
    (void)perm;
    g_ctx.wait_seconds = wait_seconds;
    g_ctx.last_second  = -1;
    return &g_ctx;
}

cfl_timer_error_t cfl_timer_wait(cfl_timer_handle_t handle, double wait_seconds,
                                 cfl_tick_result_t *result) {
    if (wait_seconds > 0.0) {
        uint32_t ms = (uint32_t)(wait_seconds * 1000.0 + 0.5);
        if (ms == 0) ms = 1;
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    cfl_embed_pre_tick();                 /* inject this tick's inter-core events */
    double t = now_seconds();
    if (result) {
        memset(result, 0, sizeof *result);
        result->all_values.timestamp = t;
        int32_t sec = (int32_t)t;
        if (handle && sec != handle->last_second) {
            result->changed_mask = CFL_CHANGED_SECOND;   /* drive per-second events/timeouts */
            handle->last_second = sec;
        }
    }
    return CFL_TIMER_SUCCESS;
}

double cfl_timer_get_timestamp(cfl_timer_handle_t handle) {
    (void)handle;
    return now_seconds();
}

cfl_timer_error_t cfl_timer_set_wait(cfl_timer_handle_t handle, double wait_seconds) {
    if (!handle) return CFL_TIMER_ERROR_INVALID_HANDLE;
    handle->wait_seconds = wait_seconds;
    return CFL_TIMER_SUCCESS;
}
double cfl_timer_get_wait(cfl_timer_handle_t handle) {
    return handle ? handle->wait_seconds : -1.0;
}
