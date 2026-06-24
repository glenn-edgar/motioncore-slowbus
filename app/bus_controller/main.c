// ============================================================================
// app/bus_controller/main.c — slow_bus bus controller, FreeRTOS-SMP (core0).
//
// The bring-up logic proven in app/uplink_test (host_link dongle protocol +
// autonomous poll sweep + liveness + command injection) folded into the RTOS
// chassis. core0 runs two pinned threads; core1 is reserved for the application
// engine (chain_tree), wired in a later step.
//
//   core0 (pinned)  bus_control   sole bus owner: poll sweep + liveness +
//                                 command service (inject/ACK/collect). [prio 2]
//   core0 (pinned)  uplink        USB-CDC libcomm host link: feed/tick/drain. [prio 2]
//   float           watchdog      free-running-clock liveness gate -> pet HW WDT. [prio 1]
//
// Concurrency: ONE mutex (g_lock) guards the shared state — host_link (its TX
// ring + protocol), the roster, and the one-in-flight command. The bus thread
// owns the PHY alone and does its bus I/O WITHOUT the lock (so it can yield
// during a reply window); it takes the lock only to read the command/roster and
// to stage frames into host_link. Callbacks (on_bus_msg/on_local_shell) run
// inside host_link_feed and therefore inside the uplink thread's lock — they
// must NOT re-take it.
//
// Hardening (the SAMD21 lesson; see app/chassis): free-running-clock watchdog,
// .noinit crash slot across WDT/soft reset, recorded-panic hooks.
//
// USB carries BINARY libcomm frames (0xC0/0xDB) -> raw I/O only, no printf to
// USB; human diagnostics go up as OP_DBG_LOG frames the Pi controller logs.
// ============================================================================
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/watchdog.h"
#include "hardware/adc.h"
#include "hardware/irq.h"
#include "hardware/structs/adc.h"   // adc_hw, ADC_FCS_OVER_BITS (overflow resync)
#include "hardware/structs/sio.h"   // sio_hw->gpio_in (1 kHz pulse sampling)
#include "hardware/pio.h"           // servo bank PIO
#include "hardware/clocks.h"        // clock_get_hz (servo 1 us/tick)
#include "hardware/i2c.h"           // HIL I2C manager on GP10/11
#ifdef I2C_SELFTEST
#include "pico/i2c_slave.h"         // i2c0 loopback self-test fixture (opt-in)
#endif
#include "servo_bank.pio.h"         // generated: servo_bank_program
#include <string.h>   // strcmp for KB lookup by name
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"

#include "host_link.h"
#include "bus_phy.h"
#include "bus_frame.h"
#include "bus_addr.h"
#include "board.h"
#include "opcodes.h"
#include "commission.h"
#include "variants.h"        // shared product/variant enum + role derivation
#include "node_role.h"       // slave/node role entry (single image, role from config)
#include "boot_identity.h"   // read+validate the unit identity ('idnt' config file)
#include "boot_hwio.h"       // read the frozen HIL pin-role map ('hwio' config file)
#include "boot_roster.h"     // read the master's slave roster ('slvr' config file)
#include "interlock/interlock.h"  // Thread 2: ported SAMD21 interlock framework
#include "interlock/il_hal.h"     //          + its pin HAL (platform seam below)
#include "cfg_file.h"        // read-only config store (cfg_load / cfg_layout_ok)

// core1 chain_tree engine (KB0)
#include "cfl_runtime.h"
#include "cfl_event_queue.h"
#include "cfl_blackboard.h"            // 10 Hz ADC streams -> chain_tree blackboard
#include "chaintree_handle.h"
#include "chaintree_handle_events.h"   // EVENT_CMD_MON_PING
#include "chaintree_handle_blackboard.h" // chaintree_handle_bb_init_hashes()

// ---- identity announced in REGISTER (interim: SAMD21 BC class; see memory) --
#define CLASS_ID_BUS_CONTROLLER  0x5E589000u
#define BC_INSTANCE_ID           42u
#define BC_FW_VERSION            0x00000100u
#define BC_BUILD_DATE            20260607u
#define BC_SCHEMA_HASH           0x51B00001u
#define USB_VID                  0x2E8Au
#define USB_PID                  0x000Au

// ---- BC-local shell command ids (match linux/bus_controller controller.c) ---
#define CMD_ECHO                 0x0001u
#define CMD_BUS_REGISTER_SLAVE   0x0160u
#define CMD_BUS_LIST_SLAVES      0x0162u
#define CMD_BUS_SET_POLL         0x0163u
#define CMD_BUS_POLL_ENABLE      0x0164u
#define CMD_BUS_CLEAR_ROSTER     0x0165u
#define BUS_REG_OK               0u
#define SHELL_OK                 0u
#define SHELL_UNKNOWN_CMD        1u

// ---- core1 application engine (KB0 monitor PoC) -----------------------------
#define BUS_ADDR_APPCORE         0xFBu   // core1 virtual-slave address
#define CMD_INTERLOCK_CLEAR      0x0210u // Thread 2: request a global clear of all latched trips
#define CMD_MON_PING             0x0200u // KB0: liveness round-trip
#define CMD_MON_SNAPSHOT         0x0201u // KB0: one-shot system report set
#define CMD_MON_STREAM           0x0202u // KB0: [enable u8][period_ms u16][mask u16]
#define CMD_ADC_READ             0x0104u // KB1 (api/HIL): snapshot the 3 decimated ADC channels
#define ADC_NCH                  3       // ADC0..2 = GP26/27/28
#define ADC0_GPIO                26u     // ADC channel ch lives on GP(ADC0_GPIO+ch)
#define ADC_BOXCAR               8       // raw samples averaged per channel per 1 kHz output
#define ADC_CLKDIV               1999.0f // 24 kHz total -> 8 kHz/ch -> /8 boxcar = ~1 kHz/ch
#define ADC_FIFO_THRESH          4       // FIFO IRQ trigger level
#define ADC_WIN_SAMPLES          100     // 1 kHz samples per 10 Hz window (100 ms)
#define CMD_ADC_STATS            0x0105u // KB1/firmware: read the 10 Hz mean/max/rms streams

// ---- GPIO + pulse-count HIL (KB1 api; pin map per board.h) ------------------
#define CMD_GPIO_CONFIG          0x0100u // [port u8][pin u8][mode u8][mode-args...]
#define CMD_GPIO_WRITE           0x0101u // [port u8][pin u8][level u8]
#define CMD_GPIO_READ            0x0102u // [port u8][pin u8] -> [level u8]
#define CMD_PULSE_READ           0x0107u // [] -> [count u32 LE] x8 (GP2..GP9 running totals)
#define CMD_PULSE_CLEAR          0x0108u // [mask u8] bit i -> clear GP(HIL_GPIO_BASE+i); 0xFF=all
// 0x0109 (PWM) / 0x010A-0x010B (quad) RETIRED 2026-06-23 — PWM + quad moved to Pico2.
#define CMD_I2C_SCAN             0x010Cu // [] -> [addr u8]... (7-bit ACKing devices)
#define CMD_I2C_WRITE            0x010Du // [addr u8][data...] -> []
#define CMD_I2C_READ             0x010Eu // [addr u8][len u8] -> [data...]
#define CMD_I2C_WRITE_READ       0x010Fu // [addr u8][rlen u8][wdata...] -> [rdata...]
#define SHELL_BAD_ARGS           2u      // matches the SAMD21 BAD_ARGS status
#define SHELL_IO_ERROR           3u      // I2C NACK / bus timeout
#define HIL_I2C_BAUD             100000u // 100 kHz standard mode
#define HIL_I2C_MAX_LEN          64u     // cap per transfer (fits the appcore payload)
// (GPIO_MODE_* runtime config bytes retired — pin roles are frozen in 'hwio';
//  see HWIO_ROLE_* in boot_hwio.h and hwio_apply() below.)
// Fixed-function pins the HIL surface never operates (documented; enforcement is
// now per-pin via the frozen hwio role — only GP2..GP9 are HIL-operable):
//   veto GP0; spare GP1/14/17/18 (PWM/quad retired); I2C GP10/11 + GP20/21;
//   UART GP12/13; RS-485 GP15/16; ADC GP26/27/28.
#define KB0_VER                  1u
// KB0 report opcodes (s2m, src=0xFB). Common header: [batch u16][seq u8][total u8][ver u8].
#define OP_MON_SYS               0x0030u
#define OP_MON_TASKS             0x0031u
#define OP_MON_MEM               0x0032u
#define OP_MON_TICK              0x0034u
#define OP_MON_KB                0x0035u
#define OP_MON_END               0x003Fu
#define MON_VER                  1u

extern volatile uint32_t g_cfl_deadline_miss;    // from cfl_timer_rp2040.c
extern volatile uint32_t g_cfl_max_overrun_us;

// ---- roster flags + liveness states (match SAMD21 bus_roster) --------------
#define FLAG_ENABLED             0x02u
#define ST_UNKNOWN               0u
#define ST_ALIVE                 1u
#define ST_DEAD                  2u

#define POLL_SLOT_TIMEOUT_MS     20
#define CMD_ACK_TIMEOUT_MS       40
#define CMD_COLLECT_TIMEOUT_MS   40
#define CMD_COLLECT_TRIES        15

// ---- crash slot (.uninitialized_data survives WDT/soft reset) ---------------
enum { RST_POWER = 0, RST_WDT, RST_PANIC, RST_STACK, RST_MALLOC };
static const char *const RST_NAME[] = { "POWER", "WDT", "PANIC", "STACK", "MALLOC" };
#define CRASH_MAGIC 0x5B0BC0DEu
typedef struct { uint32_t magic, boot_count, last_cause, panic_code; } crash_slot_t;
static crash_slot_t g_crash __attribute__((section(".uninitialized_data")));
static void chassis_panic(uint32_t cause, uint32_t code);   // defined below

// ---- monitored-thread heartbeats -------------------------------------------
enum { HB_BUS = 0, HB_UPLINK, HB_APP, HB_COUNT };
static volatile uint32_t g_hb[HB_COUNT], g_hb_us[HB_COUNT];
static TaskHandle_t t_bus, t_up, t_app, t_wd, t_i2c, t_il;

#define WD_PERIOD_MS      100
#define WD_HW_TIMEOUT_MS 4000
#define HB_STALE_US   500000u   // 500 ms real-time => thread stalled

// ---- shared state (guarded by g_lock) --------------------------------------
static SemaphoreHandle_t g_lock;
#define LOCK()   xSemaphoreTake(g_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(g_lock)

static host_link_t g_hl;
static int g_id_rc;   // boot_read_identity() result; logged at boot + re-emitted on each host (re)attach
static int g_hwio_rc; // boot_read_hwio() result (HWIO_OK / _MISSING benign; other = present-but-bad)
static hwio_t g_hwio; // frozen HIL pin-role map + ADC annotation (always usable; defaults all-UNUSED)
static int g_ilcf_rc = -2; // Thread-2 'ilcf' bring-up for the banner: -2 pending, -1 absent,
                           // 0 armed/warm-restored, >0 = DSL parse-error category
static volatile bool g_identity_refused;   // mismatch (or missing+IDENT_REQUIRE_PRESENT) -> bus arbiter quarantined
static uint32_t g_bus_baud;   // resolved RS-485 baud (config 'sp', else BUS_DEFAULT_BAUD); shown in the banner
static uint8_t g_cfg_roster_n;   // slaves loaded from the 'slvr' config file at boot (0 if none)
static uint16_t g_poll_period_ms = 500;
static uint8_t  g_poll_max_misses = 3, g_poll_tcp_retries = 2, g_poll_enabled;

// Boot banner as an OP_DBG_LOG frame. Emitted once at boot AND re-emitted on every
// host-attach edge (uplink_task): the cold-boot emit happens before any host can
// attach, and the SDK drops CDC output while disconnected, so the attach-time
// re-emit is what actually makes `ident` observable over USB. Caller serializes
// host_link access (boot path is pre-scheduler; uplink holds g_lock).
static void bc_emit_boot_banner(void) {
    char b[112];
    int n = snprintf(b, sizeof b, "[boot] bus_controller boot#%u rst=%s ident=%d%s hwio=%d il=%d slvr=%u baud=%u",
                     (unsigned)g_crash.boot_count, RST_NAME[g_crash.last_cause], g_id_rc,
                     g_identity_refused ? " REFUSED" : "", g_hwio_rc, g_ilcf_rc, (unsigned)g_cfg_roster_n,
                     (unsigned)g_bus_baud);
    (void)host_link_s2m(&g_hl, 1, OP_DBG_LOG, (const uint8_t *)b, (uint8_t)n);
}

#define ROSTER_MAX 16
typedef struct {
    uint8_t addr; uint32_t class_id; uint8_t flags;
    uint8_t state, misses; uint32_t last_seen_ms;
    uint8_t summary, announced_state, announced_summary;
} slave_t;
static slave_t  g_roster[ROSTER_MAX];
static uint8_t  g_roster_n, g_cursor;
// one injected command in flight (host -> slave), set by on_bus_msg under lock.
static bool     g_cmd_pending;
static uint8_t  g_cmd_slave; static uint16_t g_cmd_op, g_cmd_req_id;
static uint8_t  g_cmd_body[BUS_PAYLOAD_MAX]; static uint8_t g_cmd_len;

// ---- inter-core seam (core0 <-> core1 app engine), FreeRTOS queues ----------
// down: a host shell-exec addressed to 0xFB; up: a frame for the host. Cross-core
// safe (xQueue is SMP-safe), single in/out per direction. host_connected is a
// core0-published condition core1 gates on (the "USB connected throughout" rule).
#define APPCORE_ARGS_MAX  64
#define APPCORE_PAY_MAX   96
typedef struct { uint16_t req_id, cmd; uint8_t alen; uint8_t args[APPCORE_ARGS_MAX]; } appcore_cmd_t;
typedef struct { uint8_t dest; uint16_t opcode; uint8_t len; uint8_t payload[APPCORE_PAY_MAX]; } appcore_rep_t;
static QueueHandle_t   g_down_q;   // core0 -> core1
static QueueHandle_t   g_up_q;     // core1 -> core0
static volatile bool   g_host_connected;

// ---- PHY-owned-by-bus-thread helpers (NO lock; PHY has a single owner) ------
static bus_asm_t g_bc; static uint8_t g_bus_seq;

static int bus_recv(bus_frame_t *out, int ms) {
    for (int t = 0; t < ms; t++) {
        uint16_t w;
        while (bus_phy_rx_pop(&w)) { if (bus_asm_feed(&g_bc, w, out)) return 1; }
        vTaskDelay(pdMS_TO_TICKS(1));   // RX is IRQ-buffered -> safe to yield
    }
    return 0;
}
static void bus_send(uint8_t dest, uint8_t type, const uint8_t *p, uint8_t len) {
    bus_frame_t f; memset(&f, 0, sizeof f);
    f.dest = dest; f.src = BUS_ADDR_MASTER; f.type = type; f.seq = g_bus_seq++; f.len = len;
    if (len) memcpy(f.payload, p, len);
    uint16_t words[BUS_FRAME_WORDS_MAX];
    bus_phy_rx_flush();
    bus_phy_send_words(words, bus_frame_encode(words, &f));
}

// ---- roster helpers (call under lock) --------------------------------------
static slave_t *roster_find(uint8_t addr) {
    for (uint8_t i = 0; i < g_roster_n; i++) if (g_roster[i].addr == addr) return &g_roster[i];
    return NULL;
}
static uint8_t next_enabled_addr(void) {     // 0xFF if none enabled
    for (uint8_t n = 0; n < g_roster_n; n++) {
        slave_t *s = &g_roster[g_cursor];
        g_cursor = (uint8_t)((g_cursor + 1) % (g_roster_n ? g_roster_n : 1));
        if (s->flags & FLAG_ENABLED) return s->addr;
    }
    return 0xFF;
}
static void mark_alive(uint8_t addr, uint32_t now) {
    slave_t *s = roster_find(addr); if (!s) return;
    s->misses = 0; s->last_seen_ms = now; s->state = ST_ALIVE;
}
static void mark_miss(uint8_t addr) {
    slave_t *s = roster_find(addr); if (!s) return;
    if (s->misses < 0xFF) s->misses++;
    if (s->state != ST_DEAD && s->misses >= g_poll_max_misses) s->state = ST_DEAD;
}
// One pending liveness/flagged edge per call (defer-never-drop via announced-shadow).
static void emit_liveness_edges(void) {
    for (uint8_t i = 0; i < g_roster_n; i++) {
        slave_t *s = &g_roster[i];
        if (s->state != s->announced_state) {
            if (s->state == ST_DEAD) {
                uint8_t b[1] = { s->addr };
                if (host_link_s2m(&g_hl, s->addr, OP_BUS_SLAVE_DOWN, b, 1) == 0) s->announced_state = ST_DEAD;
                return;
            } else if (s->state == ST_ALIVE && s->announced_state == ST_DEAD) {
                uint8_t b[5] = { s->addr, (uint8_t)s->class_id, (uint8_t)(s->class_id >> 8),
                                 (uint8_t)(s->class_id >> 16), (uint8_t)(s->class_id >> 24) };
                if (host_link_s2m(&g_hl, s->addr, OP_BUS_SLAVE_UP, b, 5) == 0) s->announced_state = ST_ALIVE;
                return;
            } else { s->announced_state = s->state; }   // UNKNOWN->ALIVE: silent
        }
        if (s->summary != s->announced_summary) {
            uint8_t b[2] = { s->addr, s->summary };
            if (host_link_s2m(&g_hl, s->addr, OP_BUS_SLAVE_FLAGGED, b, 2) == 0) s->announced_summary = s->summary;
            return;
        }
    }
}

// ---- host_link callbacks (run under the uplink thread's lock) ---------------
static void on_bus_msg(void *u, uint8_t dest, uint16_t opcode, const uint8_t *body, uint8_t len) {
    (void)u;
    // core1 app engine: route to the inter-core down-queue, not the bus.
    if (dest == BUS_ADDR_APPCORE) {
        if (opcode != OP_SHELL_EXEC || len < 4) return;   // [req_id u16][cmd u16][args]
        appcore_cmd_t c;
        c.req_id = (uint16_t)body[0] | ((uint16_t)body[1] << 8);
        c.cmd    = (uint16_t)body[2] | ((uint16_t)body[3] << 8);
        c.alen   = (uint8_t)(len - 4);
        if (c.alen > APPCORE_ARGS_MAX) c.alen = APPCORE_ARGS_MAX;
        memcpy(c.args, &body[4], c.alen);
        (void)xQueueSend(g_down_q, &c, 0);                // non-blocking; drop if full
        return;
    }
    if (g_cmd_pending) return;                 // one in flight; the Pi tracker retries
    if (len > sizeof g_cmd_body) len = sizeof g_cmd_body;
    g_cmd_slave = dest; g_cmd_op = opcode; g_cmd_len = len;
    memcpy(g_cmd_body, body, len);
    g_cmd_req_id = (opcode == OP_BUS_EXEC && len >= 4)
                 ? (uint16_t)body[2] | ((uint16_t)body[3] << 8)
                 : (len >= 2 ? (uint16_t)body[0] | ((uint16_t)body[1] << 8) : 0);
    g_cmd_pending = true;
}
static void on_local_shell(void *u, uint16_t req_id, uint16_t cmd, const uint8_t *args, uint8_t alen) {
    (void)u;
    switch (cmd) {
    case CMD_ECHO:
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, args, alen); break;
    case CMD_BUS_CLEAR_ROSTER:
        g_roster_n = 0; g_cursor = 0; g_cmd_pending = false;
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0); break;
    case CMD_BUS_REGISTER_SLAVE: {
        uint8_t reason = BUS_REG_OK;
        if (alen >= 6 && g_roster_n < ROSTER_MAX) {
            slave_t *s = &g_roster[g_roster_n++]; memset(s, 0, sizeof *s);
            s->addr = args[0];
            s->class_id = (uint32_t)args[1] | ((uint32_t)args[2] << 8) |
                          ((uint32_t)args[3] << 16) | ((uint32_t)args[4] << 24);
            s->flags = args[5];
        }
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, &reason, 1); break;
    }
    case CMD_BUS_SET_POLL:
        if (alen >= 4) { g_poll_period_ms = (uint16_t)args[0] | ((uint16_t)args[1] << 8);
                         g_poll_max_misses = args[2]; g_poll_tcp_retries = args[3]; }
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0); break;
    case CMD_BUS_LIST_SLAVES: {
        uint8_t r[2 + ROSTER_MAX * 10]; uint8_t n = 0;
        uint32_t now = to_ms_since_boot(get_absolute_time());
        r[n++] = g_roster_n; r[n++] = g_roster_n;
        for (uint8_t i = 0; i < g_roster_n; i++) {
            slave_t *s = &g_roster[i];
            uint32_t ago32 = (s->last_seen_ms == 0) ? 0xFFFFu : (now - s->last_seen_ms);
            uint16_t ago = (ago32 > 0xFFFFu) ? 0xFFFFu : (uint16_t)ago32;
            r[n++] = s->addr;
            r[n++] = (uint8_t)s->class_id;        r[n++] = (uint8_t)(s->class_id >> 8);
            r[n++] = (uint8_t)(s->class_id >> 16); r[n++] = (uint8_t)(s->class_id >> 24);
            r[n++] = s->flags; r[n++] = s->state; r[n++] = s->misses;
            r[n++] = (uint8_t)ago; r[n++] = (uint8_t)(ago >> 8);
        }
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, r, n); break;
    }
    case CMD_BUS_POLL_ENABLE:
        if (alen >= 1) g_poll_enabled = args[0];
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0); break;
    default:
        host_link_shell_reply(&g_hl, req_id, SHELL_UNKNOWN_CMD, NULL, 0); break;
    }
}

// ---- bus thread: per-iteration state machine, one bus transaction per pass --
enum { BS_SWEEP = 0, BS_CMD_INJECT, BS_CMD_COLLECT };

static void bus_control_task(void *arg) {
    (void)arg;
    uint8_t  state = BS_SWEEP, collect_slave = 0, collect_tries = 0;
    uint32_t sweep_next_ms = 0;
    for (;;) {
        g_hb[HB_BUS]++; g_hb_us[HB_BUS] = time_us_32();
        // Identity refused -> quarantine: never drive the bus. Heartbeat keeps
        // ticking (above) so the watchdog is satisfied; the monitor/uplink stay
        // alive so the host can attach and read the refusal. (No bus_send here.)
        if (g_identity_refused) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        uint32_t now = to_ms_since_boot(get_absolute_time());
        bus_frame_t rf;

        // A queued command pre-empts the routine sweep.
        bool start_cmd = false; uint8_t cslave = 0, clen = 0; uint16_t cop = 0;
        uint8_t cbuf[BUS_PAYLOAD_MAX];
        LOCK();
        if (state == BS_SWEEP && g_cmd_pending) {
            start_cmd = true; cslave = g_cmd_slave; cop = g_cmd_op; clen = g_cmd_len;
            memcpy(cbuf, g_cmd_body, clen);
        }
        UNLOCK();
        if (start_cmd) state = BS_CMD_INJECT;

        if (state == BS_CMD_INJECT) {
            uint8_t p[BUS_PAYLOAD_MAX];
            if (clen > (uint8_t)(BUS_PAYLOAD_MAX - 2)) clen = BUS_PAYLOAD_MAX - 2;
            p[0] = (uint8_t)(cop & 0xFF); p[1] = (uint8_t)(cop >> 8);
            memcpy(&p[2], cbuf, clen);
            bus_send(cslave, BUS_FT_DATA, p, (uint8_t)(2 + clen));
            int got = bus_recv(&rf, CMD_ACK_TIMEOUT_MS);
            LOCK();
            if (got && rf.src == cslave) {
                mark_alive(cslave, now);
                uint8_t cls = (uint8_t)(rf.type & BUS_FT_MASK);
                if (cls == BUS_FT_ACK || cls == BUS_FT_NAK) {
                    uint8_t b[3] = { cslave, (uint8_t)g_cmd_req_id, (uint8_t)(g_cmd_req_id >> 8) };
                    (void)host_link_s2m(&g_hl, cslave, cls == BUS_FT_NAK ? OP_BUS_CMD_NAK : OP_BUS_CMD_ACK, b, 3);
                }
            }
            g_cmd_pending = false;            // bus freed; reply rides a later poll
            UNLOCK();
            collect_slave = cslave; collect_tries = CMD_COLLECT_TRIES; state = BS_CMD_COLLECT;
        } else if (state == BS_CMD_COLLECT) {
            bus_send(collect_slave, BUS_FT_POLL, NULL, 0);
            int got = bus_recv(&rf, CMD_COLLECT_TIMEOUT_MS);
            if (got && rf.src == collect_slave) {
                LOCK(); mark_alive(collect_slave, now);
                uint8_t cls = (uint8_t)(rf.type & BUS_FT_MASK);
                if (cls == BUS_FT_DATA && rf.len >= 2 &&
                    rf.payload[0] == (uint8_t)(OP_SHELL_REPLY & 0xFF) &&
                    rf.payload[1] == (uint8_t)(OP_SHELL_REPLY >> 8)) {
                    (void)host_link_s2m(&g_hl, collect_slave, OP_SHELL_REPLY, &rf.payload[2], (uint8_t)(rf.len - 2));
                    UNLOCK(); state = BS_SWEEP;
                } else if (cls == BUS_FT_NO_MESSAGE) {
                    slave_t *s = roster_find(collect_slave);
                    if (s && rf.len >= 1) s->summary = rf.payload[0];
                    UNLOCK();
                    if (--collect_tries == 0) state = BS_SWEEP;
                } else { UNLOCK(); if (--collect_tries == 0) state = BS_SWEEP; }
            } else if (--collect_tries == 0) {
                state = BS_SWEEP;
            }
        } else {  // BS_SWEEP
            if (g_poll_enabled && (int32_t)(now - sweep_next_ms) >= 0) {
                sweep_next_ms = now + g_poll_period_ms;
                LOCK(); uint8_t addr = next_enabled_addr(); UNLOCK();
                if (addr != 0xFF) {
                    bus_send(addr, BUS_FT_POLL, NULL, 0);
                    int got = bus_recv(&rf, POLL_SLOT_TIMEOUT_MS);
                    LOCK();
                    if (got && rf.src == addr) {
                        mark_alive(addr, now);
                        uint8_t cls = (uint8_t)(rf.type & BUS_FT_MASK);
                        if (cls == BUS_FT_DATA && rf.len >= 2 &&
                            rf.payload[0] == (uint8_t)(OP_SHELL_REPLY & 0xFF) &&
                            rf.payload[1] == (uint8_t)(OP_SHELL_REPLY >> 8)) {
                            (void)host_link_s2m(&g_hl, addr, OP_SHELL_REPLY, &rf.payload[2], (uint8_t)(rf.len - 2));
                        } else if (cls == BUS_FT_NO_MESSAGE) {
                            slave_t *s = roster_find(addr); if (s && rf.len >= 1) s->summary = rf.payload[0];
                        }
                    } else { mark_miss(addr); }
                    UNLOCK();
                }
            }
        }

        LOCK(); emit_liveness_edges(); UNLOCK();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// ---- uplink thread: USB-CDC libcomm host link ------------------------------
// (Re)load the commissioned roster from the 'slvr' config file into g_roster and
// arm the poll schedule. Called at boot AND on every host-disconnect re-arm, so a
// commissioned BC keeps polling its bus autonomously (host registrations are
// transient session overrides; the config roster is the persistent baseline).
// Caller serializes g_roster access (boot is pre-scheduler; uplink holds g_lock).
static void bc_load_cfg_roster(void) {
    g_roster_n = 0; g_cursor = 0; g_cfg_roster_n = 0;
    if (g_identity_refused) { g_poll_enabled = 0; return; }   // quarantine: no polling
    roster_cfg_t rc;
    if (boot_read_roster(&rc) != ROSTER_OK) { g_poll_enabled = 0; return; }
    for (uint8_t i = 0; i < rc.n && g_roster_n < ROSTER_MAX; i++) {
        slave_t *s = &g_roster[g_roster_n++]; memset(s, 0, sizeof *s);
        s->addr = rc.s[i].addr; s->class_id = rc.s[i].variant; s->flags = rc.s[i].flags;
    }
    if (rc.grant_period_ms) g_poll_period_ms   = rc.grant_period_ms;
    if (rc.max_misses)      g_poll_max_misses  = rc.max_misses;
    if (rc.tcp_retries)     g_poll_tcp_retries = rc.tcp_retries;
    g_cfg_roster_n = g_roster_n;
    g_poll_enabled = (g_roster_n > 0);   // a config roster -> poll immediately
}

static void uplink_task(void *arg) {
    (void)arg;
    bool prev_conn = false;
    for (;;) {
        g_hb[HB_UPLINK]++; g_hb_us[HB_UPLINK] = time_us_32();

        bool conn = stdio_usb_connected();
        if (prev_conn && !conn) {              // host went away -> re-arm
            LOCK();
            host_link_reset_boot(&g_hl);
            g_cmd_pending = false;
            bc_load_cfg_roster();   // drop host-registered slaves; restore the commissioned roster
            UNLOCK();
        }
        bool attached = (!prev_conn && conn);  // host just attached -> banner becomes deliverable
        prev_conn = conn;
        g_host_connected = conn;                   // core0-published condition for core1

        uint8_t out[64]; uint32_t n; appcore_rep_t up;
        LOCK();
        int c;
        while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
            host_link_feed(&g_hl, (uint8_t)c);     // may invoke on_bus_msg/on_local_shell
        host_link_tick(&g_hl, to_ms_since_boot(get_absolute_time()), conn);
        if (attached) bc_emit_boot_banner();   // re-announce identity to the freshly-attached host
        while (xQueueReceive(g_up_q, &up, 0) == pdTRUE)   // relay core1 replies/reports
            (void)host_link_s2m(&g_hl, up.dest, up.opcode, up.payload, up.len);
        n = host_link_tx_drain(&g_hl, out, sizeof out);
        UNLOCK();

        if (n) { for (uint32_t i = 0; i < n; i++) putchar_raw(out[i]); stdio_flush(); }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// ---- core1 application engine: the chain_tree KB0 (static-link) -------------
// cfl_runtime_run is the forever loop = this thread's body. The RP2040 timer
// (cfl_timer_rp2040.c) yields each tick and calls cfl_embed_pre_tick(), where we
// drain the inter-core down-queue and inject the commands as chain_tree events.
// KB0's MON_PING_REPLY one-shot fires -> kb0_on_ping builds the OP_SHELL_REPLY and
// pushes the up-queue (core0 relays it to USB).
static cfl_runtime_handle_t *g_rt;
static cfl_perm_t            g_perm;
static char                  g_perm_buf[16 * 1024] __attribute__((aligned(8)));
static uint16_t              g_kb0_node;   // KB0 start node (event injection target)
static uint16_t              g_kb1_node;   // KB1 (api) start node

// Activated-KB registry (one row per real KB, in activation order = arena id).
// Drives the OP_MON_KB report generically as KBs are added (kb2/kb3/kb4 later).
#define MAX_APP_KBS 6
static struct { uint16_t node; uint8_t arena_id; uint8_t active; } g_appkb[MAX_APP_KBS];
static uint8_t               g_appkb_n;

static void kb0_push_ping_reply(uint16_t req_id) {
    appcore_rep_t r; r.dest = BUS_ADDR_APPCORE; r.opcode = OP_SHELL_REPLY;
    uint32_t up = to_ms_since_boot(get_absolute_time());
    uint16_t boot = (uint16_t)g_crash.boot_count;
    uint8_t *p = r.payload; uint8_t n = 0;
    p[n++] = (uint8_t)req_id; p[n++] = (uint8_t)(req_id >> 8);
    p[n++] = SHELL_OK;
    p[n++] = (uint8_t)up; p[n++] = (uint8_t)(up >> 8);
    p[n++] = (uint8_t)(up >> 16); p[n++] = (uint8_t)(up >> 24);
    p[n++] = (uint8_t)boot; p[n++] = (uint8_t)(boot >> 8);
    p[n++] = KB0_VER;
    r.len = n;
    (void)xQueueSend(g_up_q, &r, 0);
}

// chain_tree user-fn overrides (weak defaults in kb0/user_functions.c).
void kb0_on_ping(void *handle, unsigned node_index) {
    (void)node_index;
    cfl_runtime_handle_t *rt = (cfl_runtime_handle_t *)handle;
    uint16_t req_id = (uint16_t)rt->event_data_ptr->data.integer;   // injected req_id
    kb0_push_ping_reply(req_id);
}
void kb0_on_cmd_timeout(void *handle, unsigned node_index) { (void)handle; (void)node_index; }

// ---- KB0 SNAPSHOT report emit (ack + SYS/TASKS/MEM + END) -------------------
static void mon_w16(uint8_t *p, int *n, uint16_t v) { p[(*n)++]=(uint8_t)v; p[(*n)++]=(uint8_t)(v>>8); }
static void mon_w32(uint8_t *p, int *n, uint32_t v) { for (int i=0;i<4;i++) p[(*n)++]=(uint8_t)(v>>(8*i)); }
static void mon_w64(uint8_t *p, int *n, uint64_t v) { for (int i=0;i<8;i++) p[(*n)++]=(uint8_t)(v>>(8*i)); }
static void mon_push(uint16_t op, const uint8_t *body, uint8_t len) {
    appcore_rep_t r; r.dest = BUS_ADDR_APPCORE; r.opcode = op;
    if (len > APPCORE_PAY_MAX) len = APPCORE_PAY_MAX;
    memcpy(r.payload, body, len); r.len = len;
    (void)xQueueSend(g_up_q, &r, 0);
}

// Emit the report set (SYS/TASKS/MEM/TICK/KB + END) tagged with `batch`. Shared by
// the on-command SNAPSHOT and the periodic STREAM. Uses g_rt (the runtime handle).
static void kb0_emit_reports(uint16_t batch) {
    const uint8_t total = 5;
    uint8_t b[APPCORE_PAY_MAX]; int n;

    // OP_MON_SYS seq0
    n=0; mon_w16(b,&n,batch); b[n++]=0; b[n++]=total; b[n++]=MON_VER;
    mon_w32(b,&n,(uint32_t)to_ms_since_boot(get_absolute_time()));
    mon_w16(b,&n,(uint16_t)g_crash.boot_count);
    b[n++]=(uint8_t)g_crash.last_cause;
    b[n++]=0xFF;                                  // crashed_kb (n/a until KB-crash attribution)
    mon_w32(b,&n,g_crash.panic_code);
    mon_w64(b,&n,0);                              // status_word (none yet)
    mon_push(OP_MON_SYS, b, (uint8_t)n);

    // OP_MON_TASKS seq1 — {task_id, stack_hwm_words, load_permil(0xFFFF n/a)}
    n=0; mon_w16(b,&n,batch); b[n++]=1; b[n++]=total; b[n++]=MON_VER;
    b[n++]=4;
    TaskHandle_t th[4] = { t_bus, t_up, t_app, t_wd };
    for (int i=0;i<4;i++) { b[n++]=(uint8_t)i;
        mon_w16(b,&n,(uint16_t)uxTaskGetStackHighWaterMark(th[i]));
        mon_w16(b,&n,0xFFFF); }                   // load: runtime-stats follow-up
    mon_push(OP_MON_TASKS, b, (uint8_t)n);

    // OP_MON_MEM seq2 — FreeRTOS heap + chain_tree perm/heap
    n=0; mon_w16(b,&n,batch); b[n++]=2; b[n++]=total; b[n++]=MON_VER;
    mon_w32(b,&n,(uint32_t)xPortGetFreeHeapSize());
    mon_w32(b,&n,(uint32_t)xPortGetMinimumEverFreeHeapSize());
    mon_w32(b,&n,(uint32_t)configTOTAL_HEAP_SIZE);
    mon_w32(b,&n,(uint32_t)cfl_perm_used_bytes(g_rt->perm));   // perm USED / total
    mon_w32(b,&n,(uint32_t)sizeof g_perm_buf);
    mon_w32(b,&n,(uint32_t)cfl_heap_used_bytes(g_rt->heap));   // heap USED / total (was FREE — mislabeled)
    mon_w32(b,&n,(uint32_t)((uint32_t)cfl_heap_used_bytes(g_rt->heap)+cfl_heap_free_bytes(g_rt->heap)));
    mon_push(OP_MON_MEM, b, (uint8_t)n);

    // OP_MON_TICK seq3 — engine cadence + real-time loss + event-queue depths
    n=0; mon_w16(b,&n,batch); b[n++]=3; b[n++]=total; b[n++]=MON_VER;
    mon_w16(b,&n,1000);                              // tick_hz_x100 (10 Hz)
    mon_w16(b,&n,(uint16_t)g_cfl_deadline_miss);     // real-time loss (reset-on-read)
    mon_w16(b,&n,(uint16_t)g_cfl_max_overrun_us);
    g_cfl_deadline_miss = 0; g_cfl_max_overrun_us = 0;
    b[n++]=(uint8_t)cfl_high_priority_count(g_rt->event_queue); b[n++]=8;   // hi depth/cap
    b[n++]=(uint8_t)cfl_low_priority_count(g_rt->event_queue);  b[n++]=64;  // lo depth/cap
    mon_push(OP_MON_TICK, b, (uint8_t)n);

    // OP_MON_KB seq4 — per-KB {id, active, state, arena_used, arena_cap, crash_count}
    n=0; mon_w16(b,&n,batch); b[n++]=4; b[n++]=total; b[n++]=MON_VER;
    b[n++]=g_appkb_n;                                // n KBs reported (all activated)
    for (uint8_t k=0; k<g_appkb_n; k++) {
        uint16_t au = cfl_heap_arena_used_bytes(g_rt->arena_system, g_appkb[k].arena_id);
        uint16_t af = cfl_heap_arena_free_bytes(g_rt->arena_system, g_appkb[k].arena_id);
        b[n++]=k;                                    // kb_id
        b[n++]=g_appkb[k].active;                    // active
        b[n++]=0;                                    // state (n/a)
        mon_w16(b,&n,au); mon_w16(b,&n,(uint16_t)(au+af));
        b[n++]=0;                                    // crash_count
    }
    mon_push(OP_MON_KB, b, (uint8_t)n);

    // OP_MON_END [batch][count][status][ver]
    n=0; mon_w16(b,&n,batch); b[n++]=total; b[n++]=0; b[n++]=MON_VER;
    mon_push(OP_MON_END, b, (uint8_t)n);
}

// CMD_MON_SNAPSHOT: ack + one report set, batch = req_id (from the injected event).
void kb0_on_snapshot(void *handle, unsigned node_index) {
    (void)node_index;
    cfl_runtime_handle_t *rt = (cfl_runtime_handle_t *)handle;
    uint16_t req_id = (uint16_t)rt->event_data_ptr->data.integer;
    uint8_t ack[3]; int n=0; mon_w16(ack,&n,req_id); ack[n++]=SHELL_OK;
    mon_push(OP_SHELL_REPLY, ack, (uint8_t)n);
    kb0_emit_reports(req_id);
}

// CMD_MON_STREAM state (firmware-paced; periodic kb0_emit_reports gated on host_connected).
static volatile bool g_stream_on;
static uint16_t      g_stream_period_ms = 1000;
static uint32_t      g_stream_next_ms;
static uint16_t      g_stream_batch;

// ---- central ADC service ---------------------------------------------------
// One SAR ADC, free-running round-robin over ADC0..2 (GP26/27/28) -> FIFO. A
// single FIFO ISR (on core1) demuxes the round-robin stream into per-channel
// accumulators and boxcar-decimates to ~1 kHz/channel. SINGLE writer (ISR),
// lock-free readers — aligned 16-bit reads are atomic on M0+. This is the shared
// spine "ADC needed by many sources": KB1 reads the buffer here, and the KB2/KB3
// interlock callbacks will hang off this same 1 kHz decimation tick later.
static volatile uint16_t g_adc_latest[ADC_NCH];   // decimated mean per channel
static volatile uint32_t g_adc_decim_count;       // full decimated sets emitted (liveness)
static volatile uint32_t g_adc_resyncs;           // FIFO-overflow phase resyncs (should stay 0)
static uint32_t adc_acc[ADC_NCH];                 // ISR-private accumulators
static uint16_t adc_acc_n[ADC_NCH];
static uint8_t  adc_phase;                         // round-robin position 0..ADC_NCH-1

// --- 10 Hz window statistics (mean/max/rms over 100 ms of 1 kHz samples) -----
// Second decimation tier for chain_tree. RMS is the AC component (std-dev with the
// DC bias removed) — the meaningful figure for transformer-/CT-coupled AC current.
// Stats run over the 1 kHz Stage-A outputs (100/window): sum of squares fits u32
// (100 * 4095^2 < 2^32). Effective AC bandwidth ~440 Hz (Stage-A boxcar) — ample
// for 50/60 Hz line current; switch accumulation to raw samples + u64 if wider.
typedef struct { uint32_t sum, sumsq; uint16_t max, n; } adc_win_t;
static adc_win_t          g_win_acc[ADC_NCH];      // ISR-private: current window
static volatile adc_win_t g_win_done[ADC_NCH];     // ISR-published: last completed window
static volatile uint32_t  g_adc_win_count;         // completed windows (liveness)

// Integer sqrt (no FPU/libm in the ISR path) for the RMS finalize.
static uint16_t isqrt32(uint32_t v) {
    uint32_t op = v, res = 0, one = 1u << 30;
    while (one > op) one >>= 2;
    while (one != 0) {
        if (op >= res + one) { op -= res + one; res += one << 1; }
        res >>= 1; one >>= 2;
    }
    return (uint16_t)res;
}

// Cached blackboard slot pointers (chain_tree-facing 10 Hz streams), bound once.
static int32_t *g_bb_mean[ADC_NCH], *g_bb_max[ADC_NCH], *g_bb_rms[ADC_NCH];
// Mirror of the finalized stats for the CMD_ADC_STATS reply.
typedef struct { uint16_t mean, max, rms; } adc_stat_t;
static adc_stat_t g_adc_stats[ADC_NCH];

// ---- pulse-count service (1 kHz software edge counter, GP2..GP9, zero PIO) --
// Counts are running totals exposed as globals (like g_adc_latest), cleared only
// by CMD_PULSE_CLEAR. The 1 kHz decimation ISR is the SOLE writer. A u32 is NOT a
// single-cycle read on the M0+ (two 16-bit halves), so readers/clearers briefly
// mask ADC_IRQ for a coherent snapshot. Sampling at 1 kHz -> reliable to ~400 Hz.
static volatile uint32_t g_pulse_count[HIL_GPIO_COUNT]; // [i] = edges on GP(HIL_GPIO_BASE+i)
static uint8_t  g_pulse_mask;                           // bit i set -> GP(base+i) counts
static uint8_t  g_pulse_edge[HIL_GPIO_COUNT];           // 0=rising 1=falling 2=both
static uint8_t  g_pulse_last;                           // last sampled levels, bit i = GP(base+i)

// Called at the 1 kHz decimation anchor (ISR context). Bounded loop of 8.
static void pulse_sample_1khz(void) {
    if (!g_pulse_mask) return;
    uint32_t in = sio_hw->gpio_in;
    uint8_t now = 0;
    for (uint8_t i = 0; i < HIL_GPIO_COUNT; i++)
        if (in & (1u << (HIL_GPIO_BASE + i))) now |= (uint8_t)(1u << i);
    uint8_t changed = now ^ g_pulse_last;
    uint8_t rose = changed & now;            // 0 -> 1
    uint8_t fell = changed & (uint8_t)~now;  // 1 -> 0
    for (uint8_t i = 0; i < HIL_GPIO_COUNT; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (!(g_pulse_mask & bit)) continue;
        if ((g_pulse_edge[i] == 0 && (rose    & bit)) ||
            (g_pulse_edge[i] == 1 && (fell    & bit)) ||
            (g_pulse_edge[i] == 2 && (changed & bit)))
            g_pulse_count[i]++;
    }
    g_pulse_last = now;
}

// Per-KB 1 kHz fast-tick hooks (ISR context, bounded, pure C — NOT the chain
// engine, which runs at 10 Hz on core1). Weak no-ops by default; a KB overrides
// its own to ride the fast tier (KB2/KB3 interlock veto+latch will live here).
__attribute__((weak)) void kb0_on_fast_tick(void) {}
__attribute__((weak)) void kb1_on_fast_tick(void) {}
__attribute__((weak)) void kb2_on_fast_tick(void) {}

static void adc_fifo_isr(void) {
    // Overflow desyncs the round-robin phase (dropped samples). Rare in this
    // system (FIFO 8 deep, drained every IRQ), but if it ever happens, restart
    // cleanly so phase realigns to channel 0.
    if (adc_hw->fcs & ADC_FCS_OVER_BITS) {
        adc_run(false);
        adc_fifo_drain();
        hw_set_bits(&adc_hw->fcs, ADC_FCS_OVER_BITS | ADC_FCS_UNDER_BITS);
        for (uint8_t i = 0; i < ADC_NCH; i++) { adc_acc[i] = 0; adc_acc_n[i] = 0; }
        adc_phase = 0; adc_select_input(0); g_adc_resyncs++;
        adc_run(true);
        return;
    }
    while (!adc_fifo_is_empty()) {
        uint16_t raw = adc_fifo_get() & 0x0FFFu;       // 12-bit sample
        uint8_t ch = adc_phase;
        adc_acc[ch] += raw;
        if (++adc_acc_n[ch] >= ADC_BOXCAR) {
            uint16_t s = (uint16_t)(adc_acc[ch] / ADC_BOXCAR);
            g_adc_latest[ch] = s;                        // 1 kHz tier (interlock callbacks)
            adc_acc[ch] = 0; adc_acc_n[ch] = 0;
            if (ch == ADC_NCH - 1) {     // 1 kHz anchor: all channels fresh this cycle
                g_adc_decim_count++;
                pulse_sample_1khz();     // software pulse edge counting (zero PIO)
                kb0_on_fast_tick();      // per-KB 1 kHz fast tier (monitors / interlock veto)
                kb1_on_fast_tick();
                kb2_on_fast_tick();
            }

            // 10 Hz tier: accumulate this 1 kHz sample into the window stats.
            g_win_acc[ch].sum   += s;
            g_win_acc[ch].sumsq += (uint32_t)s * s;
            if (s > g_win_acc[ch].max) g_win_acc[ch].max = s;
            if (++g_win_acc[ch].n >= ADC_WIN_SAMPLES) {  // window complete -> publish
                g_win_done[ch].sum   = g_win_acc[ch].sum;
                g_win_done[ch].sumsq = g_win_acc[ch].sumsq;
                g_win_done[ch].max   = g_win_acc[ch].max;
                g_win_done[ch].n     = g_win_acc[ch].n;
                if (ch == ADC_NCH - 1) g_adc_win_count++;
                g_win_acc[ch].sum = 0; g_win_acc[ch].sumsq = 0;
                g_win_acc[ch].max = 0; g_win_acc[ch].n = 0;
            }
        }
        if (++adc_phase >= ADC_NCH) adc_phase = 0;
    }
}

// Bring up the ADC service. MUST be called from core1 so ADC_IRQ_FIFO is enabled
// on core1's NVIC — ISR, interlock callbacks, and consuming KBs all same-core.
static void adc_service_init(void) {
    adc_init();
    for (uint8_t i = 0; i < ADC_NCH; i++) adc_gpio_init(ADC0_GPIO + i);
    adc_set_round_robin((1u << ADC_NCH) - 1);          // channels 0..ADC_NCH-1
    adc_select_input(0);                               // phase 0 == channel 0
    adc_phase = 0;
    adc_fifo_setup(true, false, ADC_FIFO_THRESH, false, false); // en, no DREQ, IRQ@thresh
    adc_set_clkdiv(ADC_CLKDIV);
    irq_set_exclusive_handler(ADC_IRQ_FIFO, adc_fifo_isr);
    adc_irq_set_enabled(true);
    irq_set_enabled(ADC_IRQ_FIFO, true);
    adc_run(true);
}

// KB1 (api/HIL): CMD_ADC_READ -> snapshot the 3 decimated channels from the
// central ADC buffer (NOT a fresh blocking read) -> OP_SHELL_REPLY
// [req_id][status][ch0 u16][ch1 u16][ch2 u16].
void kb1_on_adc(void *handle, unsigned node_index) {
    (void)node_index;
    cfl_runtime_handle_t *rt = (cfl_runtime_handle_t *)handle;
    uint16_t req_id = (uint16_t)rt->event_data_ptr->data.integer;
    appcore_rep_t r; r.dest = BUS_ADDR_APPCORE; r.opcode = OP_SHELL_REPLY;
    uint8_t *p = r.payload; uint8_t n = 0;
    p[n++] = (uint8_t)req_id; p[n++] = (uint8_t)(req_id >> 8);
    p[n++] = SHELL_OK;
    for (uint8_t ch = 0; ch < ADC_NCH; ch++) {
        uint16_t v = g_adc_latest[ch];             // lock-free single 16-bit read
        p[n++] = (uint8_t)v; p[n++] = (uint8_t)(v >> 8);
    }
    r.len = n; (void)xQueueSend(g_up_q, &r, 0);
}

// per-tick hook (weak default in cfl_timer_rp2040.c): bump the core1 heartbeat and
// inject this tick's inter-core commands into the runtime's event queue.
// Finalize the last completed 10 Hz window per channel (mean/max/AC-rms) and mirror
// it to g_adc_stats + the chain_tree blackboard streams. Engine-thread context;
// masks ADC_IRQ briefly (same core) for a coherent copy of g_win_done.
static void adc_publish_10hz(void) {
    adc_win_t w[ADC_NCH];
    irq_set_enabled(ADC_IRQ_FIFO, false);
    for (int ch = 0; ch < ADC_NCH; ch++) {
        w[ch].sum = g_win_done[ch].sum; w[ch].sumsq = g_win_done[ch].sumsq;
        w[ch].max = g_win_done[ch].max; w[ch].n     = g_win_done[ch].n;
    }
    irq_set_enabled(ADC_IRQ_FIFO, true);
    for (int ch = 0; ch < ADC_NCH; ch++) {
        if (w[ch].n == 0) continue;                          // no window completed yet
        uint16_t mean = (uint16_t)(w[ch].sum / w[ch].n);
        uint32_t msq  = w[ch].sumsq / w[ch].n;
        uint32_t m2   = (uint32_t)mean * mean;
        uint16_t rms  = (msq > m2) ? isqrt32(msq - m2) : 0;   // AC RMS = std-dev (DC removed)
        g_adc_stats[ch].mean = mean; g_adc_stats[ch].max = w[ch].max; g_adc_stats[ch].rms = rms;
        if (g_bb_mean[ch]) *g_bb_mean[ch] = mean;            // publish chain_tree streams
        if (g_bb_max[ch])  *g_bb_max[ch]  = (int32_t)w[ch].max;
        if (g_bb_rms[ch])  *g_bb_rms[ch]  = rms;
    }
}

// ---- servo bank: one PIO SM drives up to 8 RC servos (GP2..GP9, 50 Hz) ------
// The SM consumes 32-bit words [delay:24][pin-levels:8]: set pins, hold delay
// (1 us/tick), repeat. The frame raises all active servos together at t0 and
// drops each at its width (common-rising-edge). One program serves any N.
#define CMD_SERVO_SET_ALL  0x0106u   // [width_us u16 LE] x up to 8 (GP2..GP9)
#define CMD_SERVO_STOP     0x0110u   // terminate servo mode -> GP2.. back to GPIO
#define SERVO_FRAME_US     20000u    // 50 Hz
#define SERVO_MIN_US       500u
#define SERVO_MAX_US       2500u
#define SERVO_MAX          HIL_GPIO_COUNT      // 8

static PIO      g_servo_pio = pio1;            // pio0 = RS-485 bus; pio1 = HIL
static uint     g_servo_sm;
static int      g_servo_off = -1;              // program offset; <0 = not loaded
static uint8_t  g_servo_n;                     // contiguous servos GP(base)..GP(base+n-1)
static uint16_t g_servo_us[SERVO_MAX] = { 1500,1500,1500,1500,1500,1500,1500,1500 };
static volatile uint32_t g_servo_frame[SERVO_MAX + 1];
static volatile uint8_t  g_servo_frame_len;
static volatile bool     g_servo_ready;

// Build the frame from g_servo_us[0..n) (common-rising-edge). TODO: a staggered
// variant bounds peak inrush (all N servos pulling at once) — same SM/words,
// different schedule; switch here if the supply can't take simultaneous starts.
static void servo_build(void) {
    uint8_t n = g_servo_n;
    if (n == 0) { g_servo_frame_len = 0; return; }
    uint8_t idx[SERVO_MAX];
    for (uint8_t i = 0; i < n; i++) idx[i] = i;
    for (uint8_t i = 1; i < n; i++)                 // insertion sort by width asc (n<=8)
        for (uint8_t j = i; j > 0 && g_servo_us[idx[j]] < g_servo_us[idx[j-1]]; j--) {
            uint8_t t = idx[j]; idx[j] = idx[j-1]; idx[j-1] = t;
        }
    uint32_t tmp[SERVO_MAX + 1];
    uint32_t mask = (1u << n) - 1u;                 // all active servos high at t0
    uint32_t prev = 0; uint8_t len = 0;
    for (uint8_t i = 0; i < n; i++) {
        uint32_t w = g_servo_us[idx[i]];
        if (w > prev) { tmp[len++] = (mask & 0xFFu) | ((w - prev) << 8); prev = w; }
        mask &= ~(1u << idx[i]);                     // this channel drops low
    }
    tmp[len++] = ((SERVO_FRAME_US - prev) << 8);     // tail: all low for the rest of the frame
    for (uint8_t i = 0; i < len; i++) g_servo_frame[i] = tmp[i];
    g_servo_frame_len = len;                          // publish length last
}

// (Re)configure the SM for n contiguous servos from GP(base). Rare (setup-time).
static void servo_set_n(uint8_t n) {
    if (n == 0 || n > SERVO_MAX) return;
    g_servo_ready = false;
    if (g_servo_off < 0) {                            // first servo: claim SM + load program
        g_servo_sm  = (uint)pio_claim_unused_sm(g_servo_pio, true);
        g_servo_off = (int)pio_add_program(g_servo_pio, &servo_bank_program);
    } else {
        pio_sm_set_enabled(g_servo_pio, g_servo_sm, false);
    }
    for (uint8_t i = 0; i < n; i++) pio_gpio_init(g_servo_pio, HIL_GPIO_BASE + i);
    pio_sm_set_consecutive_pindirs(g_servo_pio, g_servo_sm, HIL_GPIO_BASE, n, true);
    pio_sm_config cfg = servo_bank_program_get_default_config((uint)g_servo_off);
    sm_config_set_out_pins(&cfg, HIL_GPIO_BASE, n);
    sm_config_set_out_shift(&cfg, true, true, 32);    // shift right, autopull @ 32 bits
    sm_config_set_clkdiv(&cfg, (float)clock_get_hz(clk_sys) / 1000000.0f);  // 1 us/tick
    pio_sm_init(g_servo_pio, g_servo_sm, (uint)g_servo_off, &cfg);
    pio_sm_set_enabled(g_servo_pio, g_servo_sm, true);
    g_servo_n = n;
    servo_build();
    g_servo_ready = true;
}

// Terminate servo mode: release GP2..GP(base+n-1) back to normal GPIO (SIO input)
// and clear the bank so those pins are freely reconfigurable. The SM is left
// claimed + enabled (idle on an empty FIFO, output now disconnected from the
// pins) so a later servo_set_n re-enables cheaply. NOT disabling the SM avoids a
// race where the feeder is blocked in pio_sm_put_blocking on a full FIFO.
static void servo_stop_all(void) {
    if (g_servo_n == 0) return;
    uint8_t n = g_servo_n;
    g_servo_ready = false;                            // feeder stops feeding next cycle
    g_servo_n = 0;                                    // unblocks GPIO reconfig of GP2..
    g_servo_frame_len = 0;
    for (uint8_t i = 0; i < n; i++)
        gpio_init(HIL_GPIO_BASE + i);                 // PIO func -> SIO input (servo output gone)
}

// 20 ms feeder: push one frame per period. The blocking puts are paced by the SM
// (it plays the long all-low tail while we sleep), so the busy-wait is only
// ~max-width ms per 20 ms, and only once servos are configured. TODO: DMA
// self-restart would free the CPU entirely for production.
static void servo_feeder_task(void *arg) {
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(20));
        if (!g_servo_ready) continue;
        uint8_t len = g_servo_frame_len;
        for (uint8_t i = 0; i < len; i++)
            pio_sm_put_blocking(g_servo_pio, g_servo_sm, g_servo_frame[i]);
    }
}

// PWM (GP14) and the quadrature decoder (GP17/18) were removed 2026-06-23 — both
// move to the Pico2 (RP2350). The servo bank now owns pio1 outright.

// ---- I2C manager (master, i2c1, GP10 SDA / GP11 SCL, 100 kHz) ---------------
// Internal pull-ups only — fine for short bench wires / one device; a real bus
// wants external pull-ups. Transactions are timed-out so a stuck bus can't hang
// the engine / trip the watchdog.
static void i2c_init_hw(void) {
    i2c_init(HIL_I2C_INST, HIL_I2C_BAUD);
    gpio_set_function(HIL_PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(HIL_PIN_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(HIL_PIN_I2C_SDA);
    gpio_pull_up(HIL_PIN_I2C_SCL);
}
// per-transfer timeout: ~one byte at 100 kHz is ~90 us; pad generously.
static uint i2c_to_us(uint len) { return 2000u + len * 200u; }

// ---- centralized I2C service (Stage 1: async request queue) -----------------
// The chain_tree engine must NEVER block on I2C. All bus work runs on a dedicated
// service task (its own FreeRTOS task, so the engine just enqueues and continues):
// callers enqueue a request; the task does the transfer (blocking for now) and
// posts the OP_SHELL_REPLY. Stage 2 swaps the transport to DMA and adds the device
// registry + class drivers + blackboard above this same queue — the engine-facing
// interface (enqueue / read blackboard) is fixed.
typedef enum { I2C_OP_SCAN = 0, I2C_OP_WRITE, I2C_OP_READ, I2C_OP_WRITE_READ } i2c_op_t;
typedef struct {
    uint16_t req_id;                  // OP_SHELL_REPLY correlation
    uint8_t  op;                      // i2c_op_t
    uint8_t  addr;                    // 7-bit device address
    uint8_t  wlen, rlen;              // write / read byte counts
    uint8_t  data[HIL_I2C_MAX_LEN];   // write bytes (wlen)
} i2c_req_t;
static QueueHandle_t g_i2c_req_q;     // engine pre_tick -> i2c_service_task

#ifdef I2C_SELFTEST
// ---- I2C loopback self-test fixture (opt-in: -DI2C_SELFTEST) -----------------
// Stands up i2c0 as a slave (addr 0x42) on the spare pins GP20(SDA)/GP21(SCL)
// with a 16-byte register file preset to 0xA0|index. Jumper GP10->GP20 and
// GP11->GP21 and the i2c1 HIL master can scan/read it WITHOUT any external chip
// -> proves the master + bus are good, isolating firmware from MCP23017 wiring.
// The i2c0 ISR services the slave even while the master blocks (same core).
#define I2C0_SELFTEST_ADDR  0x42u
#define I2C0_SDA            20u
#define I2C0_SCL            21u
static struct { uint8_t mem[16]; uint8_t addr; bool addr_set; } g_i2c_slv;
static void i2c0_slave_handler(i2c_inst_t *i2c, i2c_slave_event_t event) {
    switch (event) {
    case I2C_SLAVE_RECEIVE:
        if (!g_i2c_slv.addr_set) { g_i2c_slv.addr = i2c_read_byte_raw(i2c) & 0x0Fu; g_i2c_slv.addr_set = true; }
        else { g_i2c_slv.mem[g_i2c_slv.addr++ & 0x0Fu] = i2c_read_byte_raw(i2c); }
        break;
    case I2C_SLAVE_REQUEST:
        i2c_write_byte_raw(i2c, g_i2c_slv.mem[g_i2c_slv.addr++ & 0x0Fu]);
        break;
    case I2C_SLAVE_FINISH:
        g_i2c_slv.addr_set = false;
        break;
    }
}
static void i2c_loopback_init(void) {
    for (uint8_t i = 0; i < 16; i++) g_i2c_slv.mem[i] = (uint8_t)(0xA0u | i);
    i2c_init(i2c0, HIL_I2C_BAUD);
    gpio_set_function(I2C0_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C0_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C0_SDA);
    gpio_pull_up(I2C0_SCL);
    i2c_slave_init(i2c0, I2C0_SELFTEST_ADDR, i2c0_slave_handler);
}
#endif // I2C_SELFTEST

// ---- Frozen HIL pin roles (from 'hwio'; applied once at boot) ----------------
// g_hwio_role[i] is the EFFECTIVE role of GP(HIL_GPIO_BASE+i) after hwio_apply
// resolved the servo bank. Operate commands validate against it; there is no
// runtime reconfiguration (CMD_GPIO_CONFIG is retired). Default all-UNUSED until
// hwio_apply runs, so a pre-config / missing-hwio unit is hi-Z and inert.
static uint8_t g_hwio_role[HIL_GPIO_COUNT];

// Configure one HIL block pin for a NON-servo role (servos = the contiguous bank).
static void apply_pin_gpio(uint8_t idx, uint8_t role) {
    uint8_t pin = (uint8_t)(HIL_GPIO_BASE + idx);
    switch (role) {
    case HWIO_ROLE_INPUT:
    case HWIO_ROLE_UNUSED:           // safe default: input, no pull
        gpio_init(pin); gpio_set_dir(pin, false); gpio_disable_pulls(pin); break;
    case HWIO_ROLE_INPUT_PULLUP:
        gpio_init(pin); gpio_set_dir(pin, false); gpio_pull_up(pin); break;
    case HWIO_ROLE_INPUT_PULLDOWN:
        gpio_init(pin); gpio_set_dir(pin, false); gpio_pull_down(pin); break;
    case HWIO_ROLE_OUTPUT:
        gpio_init(pin); gpio_put(pin, 0); gpio_set_dir(pin, true); break;   // driven low
    case HWIO_ROLE_PULSE_COUNT: {
        uint8_t bit = (uint8_t)(1u << idx);
        gpio_init(pin); gpio_set_dir(pin, false); gpio_disable_pulls(pin);
        irq_set_enabled(ADC_IRQ_FIFO, false);
        g_pulse_edge[idx] = 0;                                  // rising-edge default
        g_pulse_last = (uint8_t)((g_pulse_last & ~bit) | (gpio_get(pin) ? bit : 0));
        g_pulse_mask |= bit;
        irq_set_enabled(ADC_IRQ_FIFO, true);
        break;
    }
    default: break;   // SERVO is armed by the bank in hwio_apply, not here
    }
}

// Apply the frozen hwio pin-role map ONCE at boot. The PIO servo bank is
// contiguous by construction, so servos must be a run from GP2 (index 0); a
// SERVO role past that run is a config error and is demoted to UNUSED (hi-Z)
// rather than silently mis-armed.
static void hwio_apply(const hwio_t *hw) {
    uint8_t servo_n = 0;
    while (servo_n < HIL_GPIO_COUNT && hw->role[servo_n] == HWIO_ROLE_SERVO) servo_n++;
    for (uint8_t i = 0; i < HIL_GPIO_COUNT; i++) {
        if (i < servo_n) { g_hwio_role[i] = HWIO_ROLE_SERVO; continue; }   // bank owns it
        uint8_t role = (hw->role[i] == HWIO_ROLE_SERVO) ? HWIO_ROLE_UNUSED  // non-contiguous -> demote
                                                        : hw->role[i];
        g_hwio_role[i] = role;
        apply_pin_gpio(i, role);
    }
    if (servo_n) servo_set_n(servo_n);   // arm the bank (idles until driven)
}

// Effective frozen role of any pin; UNUSED for non-HIL-block pins.
static inline uint8_t hil_role_of(uint8_t pin) {
    if (pin >= HIL_GPIO_BASE && pin < HIL_GPIO_BASE + HIL_GPIO_COUNT)
        return g_hwio_role[pin - HIL_GPIO_BASE];
    return HWIO_ROLE_UNUSED;
}

// ---- GPIO + pulse-count command surface (handled inline on core1) -----------
static void hil_reply(uint16_t req_id, uint8_t status, const uint8_t *res, uint8_t rlen) {
    appcore_rep_t r; r.dest = BUS_ADDR_APPCORE; r.opcode = OP_SHELL_REPLY;
    uint8_t n = 0;
    r.payload[n++] = (uint8_t)req_id; r.payload[n++] = (uint8_t)(req_id >> 8);
    r.payload[n++] = status;
    for (uint8_t i = 0; i < rlen && n < APPCORE_PAY_MAX; i++) r.payload[n++] = res[i];
    r.len = n; (void)xQueueSend(g_up_q, &r, 0);
}

// Returns true if cmd was a GPIO/pulse command (reply already sent), false otherwise.
static bool hil_gpio_dispatch(const appcore_cmd_t *c) {
    switch (c->cmd) {
    case CMD_GPIO_CONFIG:
        // RETIRED: pin roles are FROZEN at config time (hwio), applied once at boot
        // by hwio_apply(). Runtime reconfiguration is no longer permitted — operate
        // the pins, don't reconfigure them. Always rejected.
        hil_reply(c->req_id, SHELL_BAD_ARGS, NULL, 0); return true;
    case CMD_GPIO_WRITE: {
        if (c->alen < 3) { hil_reply(c->req_id, SHELL_BAD_ARGS, NULL, 0); return true; }
        uint8_t port = c->args[0], pin = c->args[1], level = c->args[2];
        // Operate-only: writable iff the frozen role is OUTPUT.
        if (port != 0 || level > 1 || hil_role_of(pin) != HWIO_ROLE_OUTPUT) {
            hil_reply(c->req_id, SHELL_BAD_ARGS, NULL, 0); return true;
        }
        gpio_put(pin, level); hil_reply(c->req_id, SHELL_OK, NULL, 0); return true;
    }
    case CMD_GPIO_READ: {
        if (c->alen < 2) { hil_reply(c->req_id, SHELL_BAD_ARGS, NULL, 0); return true; }
        uint8_t port = c->args[0], pin = c->args[1], role = hil_role_of(pin);
        // Readable iff the pin is an input role or an output (read-back is harmless).
        bool readable = (role == HWIO_ROLE_INPUT || role == HWIO_ROLE_INPUT_PULLUP ||
                         role == HWIO_ROLE_INPUT_PULLDOWN || role == HWIO_ROLE_OUTPUT);
        if (port != 0 || !readable) { hil_reply(c->req_id, SHELL_BAD_ARGS, NULL, 0); return true; }
        uint8_t lvl = gpio_get(pin) ? 1u : 0u;
        hil_reply(c->req_id, SHELL_OK, &lvl, 1); return true;
    }
    case CMD_PULSE_READ: {
        uint8_t res[HIL_GPIO_COUNT * 4]; uint8_t n = 0;
        irq_set_enabled(ADC_IRQ_FIFO, false);                     // coherent 32-bit snapshot
        for (uint8_t i = 0; i < HIL_GPIO_COUNT; i++) {
            uint32_t v = g_pulse_count[i];
            res[n++] = (uint8_t)v;         res[n++] = (uint8_t)(v >> 8);
            res[n++] = (uint8_t)(v >> 16); res[n++] = (uint8_t)(v >> 24);
        }
        irq_set_enabled(ADC_IRQ_FIFO, true);
        hil_reply(c->req_id, SHELL_OK, res, n); return true;
    }
    case CMD_PULSE_CLEAR: {
        uint8_t mask = (c->alen >= 1) ? c->args[0] : 0xFFu;
        irq_set_enabled(ADC_IRQ_FIFO, false);
        for (uint8_t i = 0; i < HIL_GPIO_COUNT; i++)
            if (mask & (1u << i)) g_pulse_count[i] = 0;
        irq_set_enabled(ADC_IRQ_FIFO, true);
        hil_reply(c->req_id, SHELL_OK, NULL, 0); return true;
    }
    case CMD_SERVO_SET_ALL: {
        if (c->alen < 2) { hil_reply(c->req_id, SHELL_BAD_ARGS, NULL, 0); return true; }
        uint8_t cnt = (uint8_t)(c->alen / 2);
        if (cnt > g_servo_n) cnt = g_servo_n;            // operate only the hwio-armed servos
        for (uint8_t i = 0; i < cnt; i++) {              // clamp each to the RC servo range
            uint16_t us = (uint16_t)c->args[i*2] | ((uint16_t)c->args[i*2 + 1] << 8);
            if (us < SERVO_MIN_US) us = SERVO_MIN_US;
            if (us > SERVO_MAX_US) us = SERVO_MAX_US;
            g_servo_us[i] = us;
        }
        servo_build();                                   // feeder picks up the new frame next cycle
        hil_reply(c->req_id, SHELL_OK, NULL, 0); return true;
    }
    case CMD_SERVO_STOP: {
        servo_stop_all();
        hil_reply(c->req_id, SHELL_OK, NULL, 0); return true;
    }
    // CMD_PWM_SET / CMD_QUAD_READ / CMD_QUAD_CLEAR removed 2026-06-23 (moved to Pico2).
    case CMD_I2C_SCAN: {                              // -> i2c_service_task (async, off the engine)
        i2c_req_t q = { .req_id = c->req_id, .op = I2C_OP_SCAN };
        (void)xQueueSend(g_i2c_req_q, &q, 0); return true;
    }
    case CMD_I2C_WRITE: {
        if (c->alen < 1) { hil_reply(c->req_id, SHELL_BAD_ARGS, NULL, 0); return true; }
        i2c_req_t q = { .req_id = c->req_id, .op = I2C_OP_WRITE, .addr = c->args[0],
                        .wlen = (uint8_t)(c->alen - 1) };
        if (q.wlen > HIL_I2C_MAX_LEN) q.wlen = HIL_I2C_MAX_LEN;
        memcpy(q.data, &c->args[1], q.wlen);
        (void)xQueueSend(g_i2c_req_q, &q, 0); return true;
    }
    case CMD_I2C_READ: {
        if (c->alen < 2) { hil_reply(c->req_id, SHELL_BAD_ARGS, NULL, 0); return true; }
        i2c_req_t q = { .req_id = c->req_id, .op = I2C_OP_READ, .addr = c->args[0], .rlen = c->args[1] };
        if (q.rlen > HIL_I2C_MAX_LEN) q.rlen = HIL_I2C_MAX_LEN;
        (void)xQueueSend(g_i2c_req_q, &q, 0); return true;
    }
    case CMD_I2C_WRITE_READ: {
        if (c->alen < 2) { hil_reply(c->req_id, SHELL_BAD_ARGS, NULL, 0); return true; }
        i2c_req_t q = { .req_id = c->req_id, .op = I2C_OP_WRITE_READ, .addr = c->args[0],
                        .rlen = c->args[1], .wlen = (uint8_t)(c->alen - 2) };
        if (q.rlen > HIL_I2C_MAX_LEN) q.rlen = HIL_I2C_MAX_LEN;
        if (q.wlen > HIL_I2C_MAX_LEN) q.wlen = HIL_I2C_MAX_LEN;
        memcpy(q.data, &c->args[2], q.wlen);
        (void)xQueueSend(g_i2c_req_q, &q, 0); return true;
    }
    default: return false;
    }
}

// Execute one queued request on the bus (blocking) + post its reply. Runs on the
// i2c_service_task — the only context that touches i2c1, so no locking needed.
static void i2c_exec(const i2c_req_t *q) {
    uint8_t buf[HIL_I2C_MAX_LEN];
    switch (q->op) {
    case I2C_OP_SCAN: {
        uint8_t found[112]; uint8_t n = 0;
        for (uint8_t a = 0x08; a <= 0x77; a++) {
            uint8_t d;
            if (i2c_read_timeout_us(HIL_I2C_INST, a, &d, 1, false, i2c_to_us(1)) >= 0) found[n++] = a;
        }
        hil_reply(q->req_id, SHELL_OK, found, n); return;
    }
    case I2C_OP_WRITE: {
        int ret = i2c_write_timeout_us(HIL_I2C_INST, q->addr, q->data, q->wlen, false, i2c_to_us(q->wlen));
        hil_reply(q->req_id, (ret == (int)q->wlen || (q->wlen == 0 && ret == 0)) ? SHELL_OK : SHELL_IO_ERROR, NULL, 0);
        return;
    }
    case I2C_OP_READ: {
        int ret = i2c_read_timeout_us(HIL_I2C_INST, q->addr, buf, q->rlen, false, i2c_to_us(q->rlen));
        if (ret == (int)q->rlen) hil_reply(q->req_id, SHELL_OK, buf, q->rlen);
        else                     hil_reply(q->req_id, SHELL_IO_ERROR, NULL, 0);
        return;
    }
    case I2C_OP_WRITE_READ: {
        int w = i2c_write_timeout_us(HIL_I2C_INST, q->addr, q->data, q->wlen, true, i2c_to_us(q->wlen));
        if (w != (int)q->wlen) { hil_reply(q->req_id, SHELL_IO_ERROR, NULL, 0); return; }
        int r = i2c_read_timeout_us(HIL_I2C_INST, q->addr, buf, q->rlen, false, i2c_to_us(q->rlen));
        if (r == (int)q->rlen) hil_reply(q->req_id, SHELL_OK, buf, q->rlen);
        else                   hil_reply(q->req_id, SHELL_IO_ERROR, NULL, 0);
        return;
    }
    default: hil_reply(q->req_id, SHELL_BAD_ARGS, NULL, 0); return;
    }
}

static void i2c_service_task(void *arg) {
    (void)arg;
    i2c_req_t q;
    for (;;) {
        if (xQueueReceive(g_i2c_req_q, &q, portMAX_DELAY) == pdTRUE) i2c_exec(&q);
    }
}

void cfl_embed_pre_tick(void) {
    g_hb[HB_APP]++; g_hb_us[HB_APP] = time_us_32();
    appcore_cmd_t c;
    while (xQueueReceive(g_down_q, &c, 0) == pdTRUE) {
        if (c.cmd == CMD_MON_PING) {
            cfl_send_integer_event(g_rt->event_queue, CFL_EVENT_PRIORITY_HIGH,
                                   g_kb0_node, EVENT_CMD_MON_PING, (cfl_int_t)c.req_id);
        } else if (c.cmd == CMD_MON_SNAPSHOT) {
            cfl_send_integer_event(g_rt->event_queue, CFL_EVENT_PRIORITY_HIGH,
                                   g_kb0_node, EVENT_CMD_MON_SNAPSHOT, (cfl_int_t)c.req_id);
        } else if (c.cmd == CMD_ADC_READ) {      // KB1 (api): route to KB1's start node
            cfl_send_integer_event(g_rt->event_queue, CFL_EVENT_PRIORITY_HIGH,
                                   g_kb1_node, EVENT_CMD_ADC_READ, (cfl_int_t)c.req_id);
        } else if (c.cmd == CMD_ADC_STATS) {     // read the 10 Hz streams from the blackboard
            appcore_rep_t r; r.dest = BUS_ADDR_APPCORE; r.opcode = OP_SHELL_REPLY;
            uint8_t *p = r.payload; uint8_t n = 0;
            p[n++] = (uint8_t)c.req_id; p[n++] = (uint8_t)(c.req_id >> 8); p[n++] = SHELL_OK;
            for (int ch = 0; ch < ADC_NCH; ch++) {           // [mean][max][rms] per channel, from bb
                int32_t mn = g_bb_mean[ch] ? *g_bb_mean[ch] : 0;
                int32_t mx = g_bb_max[ch]  ? *g_bb_max[ch]  : 0;
                int32_t rm = g_bb_rms[ch]  ? *g_bb_rms[ch]  : 0;
                p[n++]=(uint8_t)mn; p[n++]=(uint8_t)(mn>>8);
                p[n++]=(uint8_t)mx; p[n++]=(uint8_t)(mx>>8);
                p[n++]=(uint8_t)rm; p[n++]=(uint8_t)(rm>>8);
            }
            r.len = n; (void)xQueueSend(g_up_q, &r, 0);
        } else if (c.cmd == CMD_MON_STREAM) {   // [enable u8][period_ms u16][mask u16]
            if (c.alen >= 3) {
                g_stream_on = c.args[0];
                g_stream_period_ms = (uint16_t)c.args[1] | ((uint16_t)c.args[2] << 8);
                if (g_stream_period_ms < 50) g_stream_period_ms = 50;
                g_stream_next_ms = to_ms_since_boot(get_absolute_time()) + g_stream_period_ms;
            }
            appcore_rep_t r; r.dest = BUS_ADDR_APPCORE; r.opcode = OP_SHELL_REPLY;
            r.payload[0] = (uint8_t)c.req_id; r.payload[1] = (uint8_t)(c.req_id >> 8);
            r.payload[2] = SHELL_OK; r.len = 3;
            (void)xQueueSend(g_up_q, &r, 0);
        } else if (c.cmd == CMD_INTERLOCK_CLEAR) {   // Thread 2: global clear of latched trips
            interlock_request_global_clear();         // serviced on the interlock's next tick
            appcore_rep_t r; r.dest = BUS_ADDR_APPCORE; r.opcode = OP_SHELL_REPLY;
            r.payload[0] = (uint8_t)c.req_id; r.payload[1] = (uint8_t)(c.req_id >> 8);
            r.payload[2] = SHELL_OK; r.len = 3;
            (void)xQueueSend(g_up_q, &r, 0);
        } else if (hil_gpio_dispatch(&c)) {
            // GPIO + pulse-count HIL handled inline (reply sent within)
        } else {   // unknown command -> direct UNKNOWN_CMD reply (no chain handler)
            appcore_rep_t r; r.dest = BUS_ADDR_APPCORE; r.opcode = OP_SHELL_REPLY;
            r.payload[0] = (uint8_t)c.req_id; r.payload[1] = (uint8_t)(c.req_id >> 8);
            r.payload[2] = SHELL_UNKNOWN_CMD; r.len = 3;
            (void)xQueueSend(g_up_q, &r, 0);
        }
    }

    adc_publish_10hz();   // finalize 10 Hz mean/max/rms -> blackboard streams

    // STREAM pacing: emit a report set every period while enabled + host connected.
    if (g_stream_on && g_host_connected) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((int32_t)(now - g_stream_next_ms) >= 0) {
            kb0_emit_reports(g_stream_batch++);
            g_stream_next_ms = now + g_stream_period_ms;
        }
    }
}

// ============================ Thread 2 — interlock ===========================
// The ported SAMD21 interlock (node/interlock/). It reads LOCAL I/O only and
// drives the hard veto on GP0; coupling to the rest of the system is SHARED
// STATUS ONLY (g_il_status_buffer) — no queue touches the safety path. Lives on
// core1 alongside the ADC ISR + its decimated data.
//
// NOTE (deferred to the thread-review / Stage 5): (a) the hard "1-clock" ADC fast
// veto path (driven from the ADC ISR) is not ported yet — the tick below is the
// SUPERVISORY rate; (b) ideal core affinity isolates interlock + ADC ISR on their
// own core (engine moved off); (c) Thread 2 runs on the master path only for now
// (the slave is still a stub) — it becomes role-agnostic with the Thread-1
// unification. (d) NOT yet HW-verified — trip/latch/reset proving needs the bench.

// Platform externs the framework's VIRTUAL inputs read. board_millis() is in
// board.c. These two back _stack_hwm / _t_since_m2s; fed minimally for now (only
// matter when a DSL references those virtuals — wire real sources when used).
volatile uint16_t g_stack_hwm_bytes = 0;
volatile uint32_t g_last_m2s_rx_ms  = 0;

// ---- il_hal platform seam — bind the HAL to the frozen hwio roles + shared ADC.
// Pins the interlock may bind: GP0 (veto output), GP2..GP9 (HIL block, per hwio
// role), GP26/27/28 (ADC). Everything else is fixed-function → reserved.
bool il_plat_pin_reserved(uint8_t gpio) {
    if (gpio == INTERLOCK_VETO_PIN) return false;                            // GP0 veto
    if (gpio >= HIL_GPIO_BASE && gpio < HIL_GPIO_BASE + HIL_GPIO_COUNT) return false; // GP2..9
    if (gpio >= ADC0_GPIO && gpio < ADC0_GPIO + ADC_NCH) return false;       // GP26..28
    return true;
}
bool il_plat_pin_cap(uint8_t gpio, hal_pin_mode_t mode) {
    if (mode == HAL_PIN_MODE_GPIO_OUT) {
        if (gpio == INTERLOCK_VETO_PIN) return true;                         // the veto
        if (gpio >= HIL_GPIO_BASE && gpio < HIL_GPIO_BASE + HIL_GPIO_COUNT)
            return g_hwio_role[gpio - HIL_GPIO_BASE] == HWIO_ROLE_OUTPUT;
        return false;
    }
    // input modes: must be an hwio input-roled HIL pin (interlock reads, never reconfigures)
    if (gpio >= HIL_GPIO_BASE && gpio < HIL_GPIO_BASE + HIL_GPIO_COUNT) {
        uint8_t r = g_hwio_role[gpio - HIL_GPIO_BASE];
        return r == HWIO_ROLE_INPUT || r == HWIO_ROLE_INPUT_PULLUP || r == HWIO_ROLE_INPUT_PULLDOWN;
    }
    return false;
}
uint8_t il_plat_adc_channel(uint8_t gpio) {
    if (gpio >= ADC0_GPIO && gpio < ADC0_GPIO + ADC_NCH) return (uint8_t)(gpio - ADC0_GPIO);
    return 0xFFu;
}
uint16_t il_plat_adc_latest(uint8_t ch) {
    return (ch < ADC_NCH) ? g_adc_latest[ch] : 0u;   // 1 kHz decimated mean (lock-free read)
}

static void il_tick_task(void *arg) {
    (void)arg;
    for (;;) {
        interlock_tick_all();
        vTaskDelay(pdMS_TO_TICKS(20));   // supervisory cadence (see NOTE above)
    }
}

// Run from app_engine_task AFTER adc_service_init + hwio_apply (peripherals up).
// Re-claims warm-restored slots; on a cold boot, arms slot 0 from the 'ilcf' DSL
// text. Then spawns the periodic tick task on core1.
static void interlock_thread2_start(void) {
    interlock_warm_restore();
    if (interlock_armed_count() == 0) {                  // cold boot → arm from config
        uint8_t buf[CFG_FILE_MAX]; uint32_t len;
        if (cfg_load("ilcf", buf, sizeof buf, &len) == 0 && len > 0) {
            uint8_t err[3];
            uint8_t st = interlock_set_slot_dsl(0, (const char *)buf, (uint16_t)len, err);
            g_ilcf_rc = (st == SHELL_OK) ? 0 : (int)err[0];   // 0 = ok, else parse category
        } else {
            g_ilcf_rc = -1;                                   // no ilcf → no interlock armed
        }
    } else {
        g_ilcf_rc = 0;                                        // warm-restored
    }
    xTaskCreate(il_tick_task, "il", configMINIMAL_STACK_SIZE * 4, NULL, 2, &t_il);
    vTaskCoreAffinitySet(t_il, 1u << 1);                      // core1, with the ADC ISR
}

static void app_engine_task(void *arg) {
    (void)arg;
    interlock_boot_decide();   // Thread 2: validate persist + bootloop guard (before peripherals)
    const chaintree_handle_t *h = &g_chaintree_handle;
    chaintree_handle_bb_init_hashes();   // blackboard field name-hashes are static-init'd to 0
    cfl_runtime_create_params_t p; memset(&p, 0, sizeof p);
    p.perm = &g_perm; p.perm_buffer = g_perm_buf; p.perm_buffer_size = (uint16_t)sizeof g_perm_buf;
    p.heap_size = 4096; p.max_allocator_count = cfl_calculate_arrena_number(h);  // ~250 B in use; 4 KB = generous margin for KB2/3/4
    p.total_node_count = h->node_count; p.allocator_0_size = 256;
    p.event_queue_high_priority_size = 8; p.event_queue_low_priority_size = 64; p.delta_time = 0.1;

    // Central ADC service: free-running round-robin + 1 kHz FIFO ISR on core1.
    // (Started here so ADC_IRQ_FIFO is owned by core1.)
    adc_service_init();
    i2c_init_hw();      // GP10/11 I2C manager (master, 100 kHz) — inited here; serviced on core1 task
    hwio_apply(&g_hwio); // FROZEN HIL pin roles from 'hwio': configure GP2..GP9 + arm the servo bank
#ifdef I2C_SELFTEST
    i2c_loopback_init(); // opt-in self-test: i2c0 slave 0x42 on GP20/21
#endif
    interlock_thread2_start(); // Thread 2: warm-restore / arm from 'ilcf', start the tick task (core1)

    g_rt = cfl_runtime_create(&g_perm, &p, h);
    if (!g_rt) chassis_panic(RST_PANIC, 3);
    cfl_runtime_reset(g_rt);
    // Activate every real KB and record its start node. The image also carries a per-KB
    // "<kb>_functions" metadata KB (names ending in "_functions") — skip those. Arenas are
    // created in activation order, so arena_id == g_appkb_n at the time of the add.
    for (uint16_t i = 0; i < h->kb_count && g_appkb_n < MAX_APP_KBS; i++) {
        const char *nm = h->kb_table[i].kb_name;
        if (strstr(nm, "_functions")) continue;
        uint16_t node = h->kb_table[i].start_index;
        if (cfl_add_test_by_index(g_rt, i)) {
            g_appkb[g_appkb_n].node = node;
            // Real arena id from the runtime — NOT activation order. Arena 0 is the
            // reserved system allocator; KB arenas are handed out starting at id 1.
            g_appkb[g_appkb_n].arena_id = g_rt->kb_allocator_ids[i];
            g_appkb[g_appkb_n].active = 1;
            g_appkb_n++;
        }
        if (strcmp(nm, "kb0") == 0)      g_kb0_node = node;
        else if (strcmp(nm, "kb1") == 0) g_kb1_node = node;
    }
    // Bind the chain_tree blackboard slots for the 10 Hz ADC streams (NULL if absent).
    static const char *const nm_mean[ADC_NCH] = { "adc0_mean", "adc1_mean", "adc2_mean" };
    static const char *const nm_max[ADC_NCH]  = { "adc0_max",  "adc1_max",  "adc2_max"  };
    static const char *const nm_rms[ADC_NCH]  = { "adc0_rms",  "adc1_rms",  "adc2_rms"  };
    for (int ch = 0; ch < ADC_NCH; ch++) {
        g_bb_mean[ch] = (int32_t *)cfl_bb_field_by_name(g_rt, nm_mean[ch]);
        g_bb_max[ch]  = (int32_t *)cfl_bb_field_by_name(g_rt, nm_max[ch]);
        g_bb_rms[ch]  = (int32_t *)cfl_bb_field_by_name(g_rt, nm_rms[ch]);
    }

    g_hb[HB_APP]++; g_hb_us[HB_APP] = time_us_32();
    cfl_runtime_run(g_rt);                           // forever loop (the thread body)
    for (;;) vTaskDelay(pdMS_TO_TICKS(100));         // not reached
}

// ---- watchdog: free-running-clock liveness gate -> pet HW WDT ---------------
static void watchdog_task(void *arg) {
    (void)arg;
    watchdog_enable(WD_HW_TIMEOUT_MS, false);    // false: must bite on RP2040
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        uint32_t now = time_us_32();
        bool all_alive = true;
        for (int i = 0; i < HB_COUNT; i++)
            if ((uint32_t)(now - g_hb_us[i]) > HB_STALE_US) all_alive = false;
        if (all_alive) watchdog_update();
        vTaskDelayUntil(&next, pdMS_TO_TICKS(WD_PERIOD_MS));
    }
}

static void chassis_panic(uint32_t cause, uint32_t code) {
    g_crash.last_cause = cause; g_crash.panic_code = code;
    watchdog_reboot(0, 0, 0);
    for (;;) { tight_loop_contents(); }
}
void chassis_assert(int line) { chassis_panic(RST_PANIC, (uint32_t)line); }

int main(void) {
    stdio_init_all();
    sleep_ms(200);

    bool cold = (g_crash.magic != CRASH_MAGIC);
    if (cold) { memset(&g_crash, 0, sizeof g_crash); g_crash.magic = CRASH_MAGIC; g_crash.last_cause = RST_POWER; }
    else if (watchdog_enable_caused_reboot()) g_crash.last_cause = RST_WDT;
    g_crash.boot_count++;

    // ---- per-unit identity from the config FS (two-step flash) --------------
    // Step 2a: cfg_load() now scans the read-only config region. Until an image is
    // flashed there (Step 2b) the region reads erased -> id_rc = IDENT_ERR_MISSING
    // and we fall back to baked defaults (no behavior change). Step 4 turns a
    // chip/variant/uuid/addr mismatch into a hard refuse; for now we only log it.
    // Guard first: refuse to run if the firmware image overlaps the config region.
    extern char __flash_binary_end;   // SDK linker symbol: end of the .uf2 image
    if (!cfg_layout_ok(&__flash_binary_end)) chassis_panic(RST_PANIC, 0x10);
    identity_t ident;
    g_id_rc = boot_read_identity(&ident);

    // Identity policy (Step 2c / Step 4):
    //   OK       -> operate, using the per-unit RS-485 address from idnt.
    //   MISSING  -> tolerate: fall back to baked defaults. -DIDENT_REQUIRE_PRESENT
    //               makes an absent config a refuse too (production lockdown).
    //   MISMATCH -> REFUSE. A unit must never operate wearing the wrong identity.
    //               We do NOT chassis_panic (that watchdog_reboots -> a persistent
    //               mismatch boot-loops, so the host can never read why); instead we
    //               boot far enough to stay diagnosable (banner + ping report the
    //               code) and quarantine the bus arbiter so it never drives the wire.
    if (g_id_rc == IDENT_OK) {
        /* operational; addr taken from ident below */
    } else if (g_id_rc == IDENT_ERR_MISSING) {
#ifdef IDENT_REQUIRE_PRESENT
        g_identity_refused = true;
#endif
    } else {
        g_identity_refused = true;   // FORMAT/SCHEMA/CHIP/VARIANT/UUID/ADDR mismatch
    }
    uint8_t self_addr = (g_id_rc == IDENT_OK) ? ident.addr : BUS_ADDR_MASTER;
    // Bus speed from config ('sp'); absent/uncommissioned -> BUS_DEFAULT_BAUD.
    // Must match across every node on the wire (set the same 'sp' in each idnt).
    g_bus_baud = (g_id_rc == IDENT_OK && ident.baud) ? ident.baud : BUS_DEFAULT_BAUD;

    // ---- frozen HIL pin-role map from the config FS ('hwio') ----------------
    // Role-agnostic (read before the role split). HWIO_OK / _MISSING both leave
    // g_hwio usable (MISSING -> all pins UNUSED / hi-Z). A present-but-malformed
    // file is logged via the banner; pins stay at the fail-safe defaults. Roles
    // are APPLIED to hardware later (hwio_apply, per role path).
    g_hwio_rc = boot_read_hwio(&g_hwio);

    // ROLE DISPATCH (one image; role chosen from the config 'vr'): a COMMISSIONED
    // SLAVE runs only the node responder (node_role_run never returns) and skips
    // ALL the master machinery below. Master variants -- and uncommissioned
    // (MISSING) or refused (MISMATCH) units -- fall through to the master path,
    // which self-quarantines its arbiter when g_identity_refused (a bad/absent
    // identity stays inert rather than guessing a slave address).
    if (g_id_rc == IDENT_OK && !variant_is_master(ident.variant)) {
        node_role_run(self_addr, g_bus_baud);   // PHY + node responder + scheduler; never returns
    }

    bus_phy_init(g_bus_baud);   // installs the RX IRQ on core0  (MASTER path)
    bus_asm_init(&g_bc, self_addr, true);

    host_link_cfg_t cfg = {
        .class_id = CLASS_ID_BUS_CONTROLLER, .instance_id = BC_INSTANCE_ID,
        .commissioning_state = 1, .vid = USB_VID, .pid = USB_PID,
        .fw_version = BC_FW_VERSION, .build_date = BC_BUILD_DATE, .schema_hash = BC_SCHEMA_HASH,
    };
    pico_unique_board_id_t uid; pico_get_unique_board_id(&uid);
    memcpy(cfg.chip_uid, uid.id, sizeof uid.id);
    host_link_init(&g_hl, &cfg);
    host_link_set_callbacks(&g_hl, on_bus_msg, on_local_shell, NULL);

    // Master roster from the config-FS ('slvr'): a commissioned BC comes up polling
    // its bus on its own, without waiting for the host to register slaves. Absent
    // 'slvr' -> empty roster (the host can still register over USB). Re-run on every
    // host-disconnect re-arm so the commissioned roster survives host churn.
    bc_load_cfg_roster();

    // Boot banner as an OP_DBG_LOG frame (the Pi controller logs it). Also
    // re-emitted on each host-attach edge in uplink_task (see bc_emit_boot_banner).
    bc_emit_boot_banner();

    g_lock = xSemaphoreCreateMutex();
    if (!g_lock) chassis_panic(RST_PANIC, 1);
    g_down_q = xQueueCreate(8, sizeof(appcore_cmd_t));
    g_up_q   = xQueueCreate(16, sizeof(appcore_rep_t));   // headroom for a snapshot burst (7 frames)
    g_i2c_req_q = xQueueCreate(8, sizeof(i2c_req_t));     // engine -> i2c service
    if (!g_down_q || !g_up_q || !g_i2c_req_q) chassis_panic(RST_PANIC, 2);

    uint32_t t0 = time_us_32();
    for (int i = 0; i < HB_COUNT; i++) g_hb_us[i] = t0;

    // 8 KB stacks for the working threads (+4 KB over prior *4) — app/engine ran
    // the tightest at ~360 B headroom and the chain_tree walker deepens with each
    // KB; FreeRTOS heap has the room. Watchdog stays at 2 KB (trivial loop).
    xTaskCreate(bus_control_task, "bus",    configMINIMAL_STACK_SIZE * 8, NULL, 2, &t_bus);
    xTaskCreate(uplink_task,      "uplink", configMINIMAL_STACK_SIZE * 8, NULL, 2, &t_up);
    xTaskCreate(app_engine_task,  "app",    configMINIMAL_STACK_SIZE * 8, NULL, 3, &t_app);
    xTaskCreate(watchdog_task,    "wd",     configMINIMAL_STACK_SIZE * 2, NULL, 1, &t_wd);
    xTaskCreate(servo_feeder_task,"servo",  configMINIMAL_STACK_SIZE * 2, NULL, 1, NULL); // float; idle until a servo is configured
    xTaskCreate(i2c_service_task, "i2c",    configMINIMAL_STACK_SIZE * 4, NULL, 1, &t_i2c); // async I2C, off the engine
    vTaskCoreAffinitySet(t_bus, 1u << 0);
    vTaskCoreAffinitySet(t_up,  1u << 0);
    vTaskCoreAffinitySet(t_app, 1u << 1);   // core1 = application engine
    vTaskCoreAffinitySet(t_i2c, 1u << 1);   // core1 with the engine task; engine (prio 3) preempts it, so it never delays a tick
    // watchdog floats.

    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}

// ---- FreeRTOS hooks --------------------------------------------------------
void vApplicationMallocFailedHook(void) { chassis_panic(RST_MALLOC, 0); }
void vApplicationStackOverflowHook(TaskHandle_t t, char *n) { (void)t; (void)n; chassis_panic(RST_STACK, 0); }

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
