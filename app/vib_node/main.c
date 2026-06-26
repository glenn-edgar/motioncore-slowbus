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
#include <math.h>
#include "spectral.h"        // CMSIS-DSP rfft + cepstrum (vendored, M33 DSP ext)
#include "adc_capture.h"     // 20kHz x3ch PWM-locked ADC front end
#include "pipeline.h"        // FIR decimation cascade + FFT/cepstrum taps

// SHELL_* status (mirrors app/bus_controller/main.c; only the ones we use here).
#define SHELL_OK         0u
#define NODE_CMD_ECHO    0x0001u

// ---- role hooks node_role.c calls (STUBS for 3b.1) -------------------------
// 4b: the measurement core-1 task. Owns the 20 kHz PWM-locked 3-channel ADC
// capture (the wrap ISR enables on THIS core) and, for now, reports the capture
// rate + per-channel sample range so we can verify the front end on HW. The DSP
// (decimation pipeline + FFT/cepstrum -> interlock scalars) wires in here next.
static pipeline_t *g_pipe[ADC_CAP_CH];   // one full pipeline per ADC channel
static void pipeline_selftest(pipeline_t *p);   // defined below (self-test section)

static void measure_task(void *arg) {
    (void)arg;
    for (int ch = 0; ch < (int)ADC_CAP_CH; ch++) g_pipe[ch] = pipeline_create(20000.0f);
    pipeline_selftest(g_pipe[0]);       // 4c: synthetic decimation check (before live)
    adc_capture_init();                 // start the live capture ISR on core1 (this task)
    TickType_t next = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    for (;;) {
        // Every ADC channel runs its own full pipeline (decimation + coarse/fine
        // FFT + cepstrum + band stats). core1 feeds the instances sequentially.
        uint16_t s;
        for (int ch = 0; ch < (int)ADC_CAP_CH; ch++)
            while (adc_capture_pop(ch, &s)) pipeline_feed(g_pipe[ch], s);
        if (xTaskGetTickCount() >= next) {
            for (int ch = 0; ch < (int)ADC_CAP_CH; ch++) {
                const spec_result_t *hi = pipeline_spectral(g_pipe[ch], 0);
                int hi_hz = (int)((float)hi->peak_bin * hi->fs / (float)SPEC_N + 0.5f);
                printf("[vib_node] ch%d: b0 dc=%u rms=%u | b3 rms=%u | coarse peak=%dHz mag=%d | ovr=%u\n",
                       ch, pipeline_tap(g_pipe[ch], 0, TAP_DC), pipeline_tap(g_pipe[ch], 0, TAP_RMS),
                       pipeline_tap(g_pipe[ch], 3, TAP_RMS), hi_hz, (int)hi->peak_mag,
                       (unsigned)adc_capture_overrun(ch));
            }
            next += pdMS_TO_TICKS(2000);
        }
        vTaskDelay(1);
    }
}

void node_thread2_start(void) {
    // 4b: start the measurement task pinned to core1 (the DSP core). It calls
    // adc_capture_init() itself so the PWM-wrap ISR + ADC reads run on core1,
    // off the core0 bus responder. (The COMMON interlock engine wires in here in
    // step 5, reading the DSP measurement scalars.)
    TaskHandle_t t;
    xTaskCreate(measure_task, "meas", configMINIMAL_STACK_SIZE * 4, NULL, 2, &t);
    vTaskCoreAffinitySet(t, 1u << 1);   // core1
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

// ---- DSP self-test (4a): prove the vendored CMSIS-DSP FFT+cepstrum on RP2350 -
// Feed a synthetic 2 kHz sine at 20 kHz through spectral.c; the peak bin AND the
// cepstrum quefrency should both report ~2 kHz. (spectral_t is ~14 KB at N=1024
// -> static, not on the stack.)
static spectral_t g_dsp_test;
static void dsp_selftest(void) {
    // Harmonic test: fundamental 1 kHz + 2nd/3rd. The FFT peak should land on the
    // (strongest) 1 kHz line; the cepstrum quefrency should report the 1 kHz
    // harmonic spacing -> both ~1000 Hz. Verifies rfft AND cepstrum.
    const float fs = 20000.0f, f0 = 1000.0f, w = 6.28318530718f / fs;
    spectral_init(&g_dsp_test, fs);
    for (uint32_t n = 0; n < SPEC_N * (SPEC_AVG + 1u); n++) {
        float t = (float)n;
        float v = 2048.0f + 1000.0f * sinf(w * f0 * t)
                          +  500.0f * sinf(w * 2.0f * f0 * t)
                          +  250.0f * sinf(w * 3.0f * f0 * t);
        spectral_feed(&g_dsp_test, (uint16_t)(v + 0.5f));
    }
    const spec_result_t *r = spectral_get(&g_dsp_test);
    int peak_hz = (int)((float)r->peak_bin * fs / (float)SPEC_N + 0.5f);
    int q_hz    = r->q_bin ? (int)(fs / (float)r->q_bin + 0.5f) : 0;
    printf("[vib_node] DSP selftest: N=%u fs=20k  f0=1000Hz+harmonics -> "
           "peak_bin=%d peak_hz=%d  q_bin=%d q_hz=%d  gen=%u  (both ~1000 expected)\n",
           (unsigned)SPEC_N, r->peak_bin, peak_hz, r->q_bin, q_hz, (unsigned)r->gen);
}

// ---- pipeline self-test (4c): prove the 20 kHz FIR decimation cascade --------
// Feed a 200 Hz tone (below band-1's 500 Hz Nyquist, so it survives the /20
// decimation). Expect the coarse FFT (band 0, 20 kHz) AND the fine FFT (band 1,
// 1 kHz) to both peak at ~200 Hz, and the band rates to be 20k/1k/100/10.
static void pipeline_selftest(pipeline_t *p) {
    const float fs = 20000.0f, f = 200.0f, w = 6.28318530718f / fs;
    for (uint32_t n = 0; n < 90000u; n++) {   // enough for one fine-FFT publish (band1 @1kHz)
        float v = 2048.0f + 1200.0f * sinf(w * f * (float)n);
        pipeline_feed(p, (uint16_t)(v + 0.5f));
    }
    const spec_result_t *hi = pipeline_spectral(p, 0), *lo = pipeline_spectral(p, 1);
    int hi_hz = (int)((float)hi->peak_bin * hi->fs / (float)SPEC_N + 0.5f);
    int lo_hz = (int)((float)lo->peak_bin * lo->fs / (float)SPEC_N + 0.5f);
    printf("[vib_node] pipeline selftest: 200Hz -> coarse(b0) peak=%dHz  fine(b1) peak=%dHz "
           "(both ~200 expected)\n", hi_hz, lo_hz);
    for (int b = 0; b < PIPE_BANDS; b++) {
        const band_stat_t *s = pipeline_band(p, b);
        printf("[vib_node]   band%d rate=%dHz dc=%u rms=%u gen=%u\n",
               b, (int)s->rate, s->avg, s->rms, (unsigned)s->gen);
    }
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

    dsp_selftest();        // 4a: prove the CMSIS-DSP FFT/cepstrum on RP2350
    // 4c pipeline self-test runs inside measure_task (on a real instance, core1).

    // 3b.1: run the shared node responder (PHY + bus_node + scheduler). The
    // heartbeat task is created first so it runs once node_role_run starts the
    // scheduler. node_role_run() never returns.
    xTaskCreate(heartbeat_task, "hb", configMINIMAL_STACK_SIZE * 2, NULL, 1, NULL);
    node_role_run(addr, baud);
    for (;;) tight_loop_contents();
}
