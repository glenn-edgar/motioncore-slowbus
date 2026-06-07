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

// core1 chain_tree engine (KB0)
#include "cfl_runtime.h"
#include "cfl_event_queue.h"
#include "chaintree_handle.h"
#include "chaintree_handle_events.h"   // EVENT_CMD_MON_PING

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
#define CMD_MON_PING             0x0200u // KB0: liveness round-trip
#define CMD_MON_SNAPSHOT         0x0201u // KB0: one-shot system report set
#define CMD_MON_STREAM           0x0202u // KB0: [enable u8][period_ms u16][mask u16]
#define CMD_ADC_READ             0x0104u // KB1 (api/HIL): read ADC0 (GP26)
#define ADC0_GPIO                26u
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
static TaskHandle_t t_bus, t_up, t_app, t_wd;

#define WD_PERIOD_MS      100
#define WD_HW_TIMEOUT_MS 4000
#define HB_STALE_US   500000u   // 500 ms real-time => thread stalled

// ---- shared state (guarded by g_lock) --------------------------------------
static SemaphoreHandle_t g_lock;
#define LOCK()   xSemaphoreTake(g_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(g_lock)

static host_link_t g_hl;

#define ROSTER_MAX 16
typedef struct {
    uint8_t addr; uint32_t class_id; uint8_t flags;
    uint8_t state, misses; uint32_t last_seen_ms;
    uint8_t summary, announced_state, announced_summary;
} slave_t;
static slave_t  g_roster[ROSTER_MAX];
static uint8_t  g_roster_n, g_cursor;
static uint16_t g_poll_period_ms = 500;
static uint8_t  g_poll_max_misses = 3, g_poll_tcp_retries = 2, g_poll_enabled;

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
static void uplink_task(void *arg) {
    (void)arg;
    bool prev_conn = false;
    for (;;) {
        g_hb[HB_UPLINK]++; g_hb_us[HB_UPLINK] = time_us_32();

        bool conn = stdio_usb_connected();
        if (prev_conn && !conn) {              // host went away -> re-arm
            LOCK();
            host_link_reset_boot(&g_hl);
            g_poll_enabled = 0; g_cmd_pending = false; g_roster_n = 0; g_cursor = 0;
            UNLOCK();
        }
        prev_conn = conn;
        g_host_connected = conn;                   // core0-published condition for core1

        uint8_t out[64]; uint32_t n; appcore_rep_t up;
        LOCK();
        int c;
        while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
            host_link_feed(&g_hl, (uint8_t)c);     // may invoke on_bus_msg/on_local_shell
        host_link_tick(&g_hl, to_ms_since_boot(get_absolute_time()), conn);
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
    mon_w32(b,&n,(uint32_t)cfl_perm_used_bytes(g_rt->perm));
    mon_w32(b,&n,(uint32_t)sizeof g_perm_buf);
    mon_w32(b,&n,(uint32_t)cfl_heap_free_bytes(g_rt->heap));
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

// KB1 (api/HIL): CMD_ADC_READ -> read ADC0 (GP26) -> OP_SHELL_REPLY [req_id][status][value u16].
// (Direct blocking read for now; switches to the central decimated ADC service with KB2/KB3.)
void kb1_on_adc(void *handle, unsigned node_index) {
    (void)node_index;
    cfl_runtime_handle_t *rt = (cfl_runtime_handle_t *)handle;
    uint16_t req_id = (uint16_t)rt->event_data_ptr->data.integer;
    adc_select_input(0);                 // ADC0 = GP26
    uint16_t v = adc_read();             // 12-bit
    appcore_rep_t r; r.dest = BUS_ADDR_APPCORE; r.opcode = OP_SHELL_REPLY;
    uint8_t *p = r.payload; uint8_t n = 0;
    p[n++] = (uint8_t)req_id; p[n++] = (uint8_t)(req_id >> 8);
    p[n++] = SHELL_OK;
    p[n++] = (uint8_t)v; p[n++] = (uint8_t)(v >> 8);
    r.len = n; (void)xQueueSend(g_up_q, &r, 0);
}

// per-tick hook (weak default in cfl_timer_rp2040.c): bump the core1 heartbeat and
// inject this tick's inter-core commands into the runtime's event queue.
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
        } else {   // unknown command -> direct UNKNOWN_CMD reply (no chain handler)
            appcore_rep_t r; r.dest = BUS_ADDR_APPCORE; r.opcode = OP_SHELL_REPLY;
            r.payload[0] = (uint8_t)c.req_id; r.payload[1] = (uint8_t)(c.req_id >> 8);
            r.payload[2] = SHELL_UNKNOWN_CMD; r.len = 3;
            (void)xQueueSend(g_up_q, &r, 0);
        }
    }

    // STREAM pacing: emit a report set every period while enabled + host connected.
    if (g_stream_on && g_host_connected) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((int32_t)(now - g_stream_next_ms) >= 0) {
            kb0_emit_reports(g_stream_batch++);
            g_stream_next_ms = now + g_stream_period_ms;
        }
    }
}

static void app_engine_task(void *arg) {
    (void)arg;
    const chaintree_handle_t *h = &g_chaintree_handle;
    cfl_runtime_create_params_t p; memset(&p, 0, sizeof p);
    p.perm = &g_perm; p.perm_buffer = g_perm_buf; p.perm_buffer_size = (uint16_t)sizeof g_perm_buf;
    p.heap_size = 8192; p.max_allocator_count = cfl_calculate_arrena_number(h);
    p.total_node_count = h->node_count; p.allocator_0_size = 256;
    p.event_queue_high_priority_size = 8; p.event_queue_low_priority_size = 64; p.delta_time = 0.1;

    // ADC0 (GP26) for KB1 CMD_ADC_READ. (Central decimated ADC service arrives with KB2/KB3.)
    adc_init();
    adc_gpio_init(ADC0_GPIO);

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
            g_appkb[g_appkb_n].arena_id = g_appkb_n;
            g_appkb[g_appkb_n].active = 1;
            g_appkb_n++;
        }
        if (strcmp(nm, "kb0") == 0)      g_kb0_node = node;
        else if (strcmp(nm, "kb1") == 0) g_kb1_node = node;
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

    bus_phy_init(BUS_DEFAULT_BAUD);   // installs the RX IRQ on core0
    bus_asm_init(&g_bc, BUS_ADDR_MASTER, true);

    host_link_cfg_t cfg = {
        .class_id = CLASS_ID_BUS_CONTROLLER, .instance_id = BC_INSTANCE_ID,
        .commissioning_state = 1, .vid = USB_VID, .pid = USB_PID,
        .fw_version = BC_FW_VERSION, .build_date = BC_BUILD_DATE, .schema_hash = BC_SCHEMA_HASH,
    };
    pico_unique_board_id_t uid; pico_get_unique_board_id(&uid);
    memcpy(cfg.chip_uid, uid.id, sizeof uid.id);
    host_link_init(&g_hl, &cfg);
    host_link_set_callbacks(&g_hl, on_bus_msg, on_local_shell, NULL);

    // Boot banner as an OP_DBG_LOG frame (the Pi controller logs it).
    { char b[48]; int n = snprintf(b, sizeof b, "[boot] bus_controller boot#%u rst=%s",
                                   (unsigned)g_crash.boot_count, RST_NAME[g_crash.last_cause]);
      (void)host_link_s2m(&g_hl, 1, OP_DBG_LOG, (const uint8_t *)b, (uint8_t)n); }

    g_lock = xSemaphoreCreateMutex();
    if (!g_lock) chassis_panic(RST_PANIC, 1);
    g_down_q = xQueueCreate(8, sizeof(appcore_cmd_t));
    g_up_q   = xQueueCreate(16, sizeof(appcore_rep_t));   // headroom for a snapshot burst (7 frames)
    if (!g_down_q || !g_up_q) chassis_panic(RST_PANIC, 2);

    uint32_t t0 = time_us_32();
    for (int i = 0; i < HB_COUNT; i++) g_hb_us[i] = t0;

    xTaskCreate(bus_control_task, "bus",    configMINIMAL_STACK_SIZE * 4, NULL, 2, &t_bus);
    xTaskCreate(uplink_task,      "uplink", configMINIMAL_STACK_SIZE * 4, NULL, 2, &t_up);
    xTaskCreate(app_engine_task,  "app",    configMINIMAL_STACK_SIZE * 4, NULL, 3, &t_app);
    xTaskCreate(watchdog_task,    "wd",     configMINIMAL_STACK_SIZE * 2, NULL, 1, &t_wd);
    vTaskCoreAffinitySet(t_bus, 1u << 0);
    vTaskCoreAffinitySet(t_up,  1u << 0);
    vTaskCoreAffinitySet(t_app, 1u << 1);   // core1 = application engine
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
