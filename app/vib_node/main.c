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
#include "interlock.h"       // Thread-2 common interlock engine + DSL
#include "il_hal.h"          // pin HAL contract (il_plat_* seam below)
#include "cfg_file.h"        // cfg_load (ilcN interlock config)

// ---- symbols the shared interlock framework expects from the app ------------
volatile uint16_t g_stack_hwm_bytes;   // stack telemetry (unused detail here)
volatile uint32_t g_last_m2s_rx_ms;    // last master->slave rx ms (comms-loss source)
extern interlock_persist_t g_interlock_persist;   // defined in interlock.c (.uninitialized_data)

// SHELL_* status (mirrors app/bus_controller/main.c; only the ones we use here).
#define SHELL_OK         0u
#define NODE_CMD_ECHO    0x0001u

// ---- role hooks node_role.c calls (STUBS for 3b.1) -------------------------
// 4b: the measurement core-1 task. Owns the 20 kHz PWM-locked 3-channel ADC
// capture (the wrap ISR enables on THIS core) and, for now, reports the capture
// rate + per-channel sample range so we can verify the front end on HW. The DSP
// (decimation pipeline + FFT/cepstrum -> interlock scalars) wires in here next.
static pipeline_t *g_pipe[ADC_CAP_CH];   // one full pipeline per channel (made in node_thread2_start)

static void measure_task(void *arg) {
    (void)arg;
    adc_capture_init();                 // start the live capture ISR on core1 (g_pipe already created)
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
            printf("[vib_node] interlock: armed=%u summary=0x%02x  veto(gp0)=%d  gp1_in=%d\n",
                   interlock_armed_count(), interlock_summary_flags(),
                   gpio_get(INTERLOCK_VETO_PIN), gpio_get(INTERLOCK_IN_PIN));
            next += pdMS_TO_TICKS(2000);
        }
        vTaskDelay(1);
    }
}

// ---- il_plat seam: the interlock's measurement + pin-capability surface -----
// The Pico 2 W maps the 3 "ADC channels" to their pipeline BROADBAND AC-RMS
// (band 0), so the EXISTING ADC-stream interlock conditions threshold vibration
// level with no engine/DSL change (common engine, different measurement). Only
// the plain GPIO (GP1..4) + the GP0 veto are bindable; bus/SPI/I2C/encoder/PWM/
// ADC pins are reserved.
bool il_plat_pin_reserved(uint8_t gpio) {
    if (gpio == INTERLOCK_VETO_PIN) return false;                         // GP0 veto out
    if (gpio == INTERLOCK_IN_PIN) return false;                           // GP1 interlock in
    if (gpio >= GPIO_BASE && gpio < GPIO_BASE + GPIO_COUNT) return false; // GP2..4
    if (gpio >= ADC_PIN_CH0 && gpio <= ADC_PIN_CH2) return false;         // GP26..28
    return true;
}
bool il_plat_pin_cap(uint8_t gpio, hal_pin_mode_t mode) {
    if (mode == HAL_PIN_MODE_GPIO_OUT)
        return gpio == INTERLOCK_VETO_PIN ||
               (gpio >= GPIO_BASE && gpio < GPIO_BASE + GPIO_COUNT);
    // input modes: the GP1 interlock input (high-Z) + the plain GPIO GP2..4
    return gpio == INTERLOCK_IN_PIN ||
           (gpio >= GPIO_BASE && gpio < GPIO_BASE + GPIO_COUNT);
}
uint8_t il_plat_adc_channel(uint8_t gpio) {
    if (gpio == ADC_PIN_CH0) return 0;
    if (gpio == ADC_PIN_CH1) return 1;
    if (gpio == ADC_PIN_CH2) return 2;
    return 0xFFu;
}
uint16_t il_plat_adc_latest(uint8_t ch) {   // "decimated ADC" = channel broadband AC-rms
    return (ch < ADC_CAP_CH && g_pipe[ch]) ? pipeline_tap(g_pipe[ch], 0, TAP_RMS) : 0u;
}

// FNV-1a over the flashed ilc0..ilc9 config (re-arm when the config changes).
static uint32_t interlock_cfg_fingerprint(void) {
    uint32_t fp = 2166136261u;
    for (uint8_t slot = 0; slot < INTERLOCK_MAX_SLOTS; slot++) {
        const char name[CFG_NAME_LEN] = { 'i', 'l', 'c', (char)('0' + slot) };
        uint8_t buf[CFG_FILE_MAX]; uint32_t len;
        if (cfg_load(name, buf, sizeof buf, &len) != 0 || len == 0) continue;
        fp = (fp ^ (uint32_t)(slot + 1)) * 16777619u;
        for (uint32_t i = 0; i < len; i++) fp = (fp ^ buf[i]) * 16777619u;
    }
    return fp;
}

static void il_tick_task(void *arg) {
    (void)arg;
    for (;;) { interlock_tick_all(); vTaskDelay(pdMS_TO_TICKS(2)); }  // ~2 ms veto response
}

void node_thread2_start(void) {
    // Create the per-channel pipelines BEFORE the interlock can read them
    // (il_plat_adc_latest -> g_pipe). Empty pipelines read rms=0 until fed.
    for (int ch = 0; ch < (int)ADC_CAP_CH; ch++) g_pipe[ch] = pipeline_create(20000.0f);

    // Configure the GP1 interlock input HW: input + internal pull-up. The il HAL
    // only records ownership (RP2040 relied on hwio_apply for pin direction/pull);
    // vib_node has no hwio, so set the pull here. Safe = HIGH; pulled LOW -> trip.
    gpio_init(INTERLOCK_IN_PIN);
    gpio_set_dir(INTERLOCK_IN_PIN, GPIO_IN);
    gpio_pull_up(INTERLOCK_IN_PIN);

    // Common interlock engine: warm-restore the armed set, re-arm from the flashed
    // ilc0..ilc9 config when it changed / on cold boot (same path as the RP2040 node).
    interlock_boot_decide();
    interlock_warm_restore();
    uint32_t fp = interlock_cfg_fingerprint();
    if (fp != g_interlock_persist.cfg_fingerprint || interlock_armed_count() == 0) {
        for (uint8_t slot = 0; slot < INTERLOCK_MAX_SLOTS; slot++) interlock_disarm_slot(slot);
        // Slot 0 is reserved for the built-in GP1 safety input (below); config = ilc1..ilc9.
        for (uint8_t slot = 1; slot < INTERLOCK_MAX_SLOTS; slot++) {
            const char name[CFG_NAME_LEN] = { 'i', 'l', 'c', (char)('0' + slot) };
            uint8_t buf[CFG_FILE_MAX]; uint32_t len; uint8_t err[3];
            if (cfg_load(name, buf, sizeof buf, &len) != 0 || len == 0) continue;
            (void)interlock_set_slot_dsl(slot, (const char *)buf, (uint16_t)len, err);
        }
        g_interlock_persist.cfg_fingerprint = fp;
    }
    // Built-in GP1 interlock input (internal PULL-UP, active-low), slot 0, ALWAYS
    // armed via the COMMON engine: the pull-up holds gp1 HIGH = OK; a device pulling
    // gp1 LOW fails the watch -> trips -> veto (gp0 high). Hardwired safety input.
    {
        static const char gp1_il[] =
            "gp1il;cfg[(gp1):in,up,(gp0):out];watch[gp1:1];out_ok[gp0:0];out_err[gp0:1]";
        uint8_t err[3];
        interlock_disarm_slot(0);
        uint8_t st = interlock_set_slot_dsl(0, gp1_il, (uint16_t)(sizeof gp1_il - 1u), err);
        printf("[vib_node] GP1 interlock (slot0) arm st=%u err=%u,%u\n", st, err[0], err[1]);
    }
    printf("[vib_node] interlock: armed=%u slots (GP1 safety in + ADC ch -> band0 rms)\n",
           interlock_armed_count());

    TaskHandle_t tm, ti;
    xTaskCreate(measure_task, "meas", configMINIMAL_STACK_SIZE * 4, NULL, 2, &tm);
    vTaskCoreAffinitySet(tm, 1u << 1);            // core1: the DSP
    xTaskCreate(il_tick_task, "il",   configMINIMAL_STACK_SIZE * 4, NULL, 4, &ti);
    vTaskCoreAffinitySet(ti, 1u << 1);            // core1, prio4 > engine: safety preempts app
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

// (pipeline decimation self-test lived here through 4c; removed now that the
// pipeline is fed live by measure_task and read by the interlock.)

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
