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
#include "hardware/timer.h"         // hardware_alarm_* (liveness-slot idle-gap alarm)
#include "hardware/pwm.h"           // GP22 PWM test source (CMD_PWM_TEST)
#include "pico/critical_section.h"   // hw spinlock + IRQ-save (TX buffer pool, §17)
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
#include "bus_node.h"        // bus_node_queue — slave engine reply pump (C3)
#include "bus_frame.h"
#include "bus_addr.h"
#include "board.h"
#include "opcodes.h"
#include "commission.h"
#include "variants.h"        // shared product/variant enum + role derivation
#include "node_role.h"       // slave/node role entry (single image, role from config)
#include "boot_identity.h"   // read+validate the unit identity ('idnt' config file)
#include "boot_hwio.h"       // read the frozen HIL pin-role map ('hwio' config file)
#include "boot_netcfg.h"     // read WiFi creds + zenoh-agent endpoint ('neti' config file)
#include <errno.h>
#include "pico/cyw43_arch.h" // §dual-transport: CYW43/lwIP ALWAYS linked; transport chosen at runtime
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
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
#define CMD_BUS_POLL_STATS       0x0166u // arbiter step 2: -> [total u32][alive u32][miss u32][meas_ta_us u16]
#define CMD_BUS_MEASURE_TA       0x0167u // arbiter step 2: latch the measured POLL->reply turnaround
#define CMD_BUS_CYCLE_MODE       0x0168u // §17 step3: [0|1] per-slot rotation vs the cyclic engine
#define CMD_BUS_MSG_INJECT       0x0169u // §17 step3 test: [addr][payload...] -> bus_msg_send (producer)
#define CMD_BUS_SET_DEADLINE     0x016Au // §17 step4: [deadline_us u32] real-time cycle deadline
#define CMD_BUS_SET_PACE         0x016Bu // §17 step7: [pace_us u32][slowpoll_div u32 opt] fixed-rate + dead-probe rate
#define CMD_BUS_UPLINK_FORCE     0x016Cu // §dual-transport: [mode u8 (0=standby 1=usb 2=wifi, 0xFF=auto)][dur_ms u32] force-pin the uplink mode
#define BUS_REG_OK               0u
#define SHELL_OK                 0u
#define SHELL_UNKNOWN_CMD        1u

// ---- core1 application engine (KB0 monitor PoC) -----------------------------
#define BUS_ADDR_APPCORE         0xFBu   // core1 virtual-slave address
#define CMD_INTERLOCK_CLEAR      0x0210u // Thread 2: request a global clear of all latched trips
#define CMD_INTERLOCK_STATUS     0x0211u // Thread 2: -> [gveto][n] + per non-empty slot [slot][state][tf][latched]
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
#define CMD_APP_ECHO             0x0300u // Thread 3 (kbapp): echo payload via the chain-tree engine
#define CMD_APP_ECHO_TO          0x0301u // Thread 3 (kbapp): [addr u8][payload] — engine originates a
                                         // bus echo to node <addr>, master correlates the reply
#define CMD_APP_IL_CLEAR         0x0302u // Thread 3 (kbapp): engine-driven interlock global clear (Thread 2)
#define CMD_APP_PP_START         0x0303u // ping-pong bench: [addr] start free-running master<->slave chain-flow loop
#define CMD_APP_PP_STOP          0x0304u // ping-pong bench: stop the loop
#define CMD_APP_PP_READ          0x0305u // ping-pong bench: -> [master_cnt u32][slave_cnt u32] (read out-of-band)
#define KBAPP_VER                1u

// ---- GPIO + pulse-count HIL (KB1 api; pin map per board.h) ------------------
#define CMD_GPIO_CONFIG          0x0100u // [port u8][pin u8][mode u8][mode-args...]
#define CMD_GPIO_WRITE           0x0101u // [port u8][pin u8][level u8]
#define CMD_GPIO_READ            0x0102u // [port u8][pin u8] -> [level u8]
#define CMD_GPIO_ROLES           0x0103u // [] -> [base u8][count u8][role u8 x count] effective hwio roles
#define CMD_PULSE_READ           0x0107u // [] -> [count u32 LE] x8 (GP2..GP9 running totals)
#define CMD_PULSE_CLEAR          0x0108u // [mask u8] bit i -> clear GP(HIL_GPIO_BASE+i); 0xFF=all
#define CMD_PWM_TEST             0x0109u // GP22 test source: [freq_hz u32][duty_pct u8]; 0 freq/duty = off (hi-Z)
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
static int g_neti_rc; // boot_read_netcfg() result (NETI_OK / _MISSING benign; other = present-but-bad)
static netcfg_t g_netcfg; // WiFi creds + zenoh-agent endpoint (present=0 unless a valid 'neti' loaded)
// §dual-transport step 2: ONE uplink supervisor picks the transport at runtime — USB > WiFi >
// standalone, dynamic (USB PREEMPTS WiFi at any time). The RS-485 bus + engine + interlock run in
// ALL modes; only host_link's transport changes. g_uplink_active=false (standalone) tells the §17
// cycle to sink its emits (step 3 gates at the source; step 2 has a pump-side sink).
enum { UPL_STANDALONE = 0, UPL_USB = 1, UPL_WIFI = 2 };
static volatile uint8_t g_uplink_mode;    // current transport (POLL_STATS / observability)
static volatile bool    g_uplink_active;  // true in USB/WiFi, false in standalone
// Force-override the selector for a bounded window (testing/ops): pin a mode (e.g. standalone) so
// the dynamic selector is overridden, with auto-revert so a forced-standalone can't strand the node.
static volatile uint8_t  g_uplink_force = 0xFFu;   // 0xFF = auto selector; else force this UPL_* mode
static volatile uint32_t g_uplink_force_until;     // ms deadline for the forced mode (auto-revert to auto)
static int g_ilcf_rc = -2; // Thread-2 interlock bring-up for the banner: -2 pending,
                           // else the count of slots armed from ilc0..ilc9 (0..10)
static uint32_t g_il_armfail = 0; // DIAG: first arm failure [slot:8][status:8][err0:8][err1:8]
static volatile bool g_identity_refused;   // mismatch (or missing+IDENT_REQUIRE_PRESENT) -> bus arbiter quarantined
static uint32_t g_bus_baud;   // resolved RS-485 baud (config 'sp', else BUS_DEFAULT_BAUD); shown in the banner
static uint8_t g_cfg_roster_n;   // slaves loaded from the 'slvr' config file at boot (0 if none)
static uint16_t g_poll_period_ms = 500;
static uint8_t  g_poll_max_misses = 3, g_poll_tcp_retries = 2, g_poll_enabled;
// Arbiter step 2 instrumentation: back-to-back poll counters + turnaround probe.
static volatile uint32_t g_poll_total, g_poll_alive, g_poll_miss;
static volatile uint32_t g_bus_msgs;   // perf: RS-485 frames the master TX'd (POLL+DATA) -- every bus_send
static volatile uint16_t g_last_ta_us;    // bus_poll_slot writes the slot's POLL->first-word turnaround
static volatile uint16_t g_meas_ta_us;    // latched turnaround (now measured every slot by the RX ISR)
static TaskHandle_t      g_bus_task;      // arbiter task handle (woken by the RX ISR hook)

// Slot timing (460800; make bit-time-relative later). Defined here because the RX
// ISR hook below uses T_GAP_US.
#define T_RESP_US        3000u   // first-word deadline -> dead-node (HW turnaround ~210-480us)
#define T_GAP_US          300u   // inter-frame idle -> end of the node's burst
#define T_WINDOW_MAX_US 12000u   // per-node burst ceiling (anti-babble)

// --- RX-ISR dispatch (per role) + the master's liveness-slot idle-gap alarm ----
// The PHY RX ISR calls bus_phy_rx_isr_hook after draining words; behaviour is set
// per role at task start. SLAVE: notify node_task to respond (us latency). MASTER:
// during a liveness slot it just pushes the idle-gap alarm out by T_GAP -- the bus
// task stays BLOCKED for the whole slot and wakes ONCE (alarm at last-word+T_GAP, or
// at T_RESP if no reply), so core0 isn't burned busy-waiting. The slot SM + roster
// stay in the task (nothing mutex-guarded is touched from an ISR).
static void          (*g_rx_hook)(void);     // per-role RX-ISR action
static uint            g_gap_alarm;          // master: claimed hardware alarm
static volatile bool   g_slot_active;        // master: a liveness slot awaits the bus
static volatile uint32_t g_slot_poll_us;     // master: time the POLL went out
static volatile bool   g_slot_first;         // master: first reply word seen this slot
// Miss diagnostics (POLL_STATS b93-100): split SLOT_NO_RESPONSE into SILENT (no word at
// all -> the node is absent/dead) vs PARTIAL (a reply started but no complete frame from
// addr -> a mid-burst stall the grace-redrain couldn't recover within the window, or CRC).
// Useful for low-baud / long-bus operation, where PARTIAL is the signal that reply bursts
// are stalling (see bus_poll_slot's grace-redrain).
static volatile uint32_t g_miss_silent, g_miss_partial;

static void rot_alarm_cb(uint n) {           // idle-gap / T_RESP fired -> wake the bus task once
    (void)n;
    if (g_bus_task) { BaseType_t h = pdFALSE; vTaskNotifyGiveFromISR(g_bus_task, &h); portYIELD_FROM_ISR(h); }
}
static void master_rx_hook(void) {           // RX words arrived during a slot: defer the idle deadline
    if (!g_slot_active) return;
    if (!g_slot_first) {
        g_slot_first = true;
        uint32_t ta = time_us_32() - g_slot_poll_us;
        g_last_ta_us = (ta > 0xFFFFu) ? 0xFFFFu : (uint16_t)ta;
    }
    hardware_alarm_set_target(g_gap_alarm, make_timeout_time_us(T_GAP_US));
}
static void slave_notify_hook(void) {        // slave: wake node_task to answer the POLL
    if (g_bus_task) { BaseType_t h = pdFALSE; vTaskNotifyGiveFromISR(g_bus_task, &h); portYIELD_FROM_ISR(h); }
}
void bus_phy_rx_isr_hook(void) { if (g_rx_hook) g_rx_hook(); }

// Slave registers here (node_role.c): node_task is woken per RX so it answers at us latency.
void bus_rx_notify_set(TaskHandle_t t) { g_bus_task = t; g_rx_hook = slave_notify_hook; }

// Boot banner as an OP_DBG_LOG frame. Emitted once at boot AND re-emitted on every
// host-attach edge (uplink_task): the cold-boot emit happens before any host can
// attach, and the SDK drops CDC output while disconnected, so the attach-time
// re-emit is what actually makes `ident` observable over USB. Caller serializes
// host_link access (boot path is pre-scheduler; uplink holds g_lock).
// §17 step 1: result scalars used by the banner; full pool/table defs are below
// (after ROSTER_MAX / g_perm). Tentative defs here, completed in that block.
static uint16_t g_pool_n, g_pool_selftest;
static volatile uint16_t g_pool_avail;     // current free count (banner/POLL_STATS read it)
static volatile uint16_t g_pool_low;       // low-water (min avail = peak in-flight)
static volatile uint32_t g_pool_exhausted; // §17 step5: grab-NULL events (producer out-ran the bus)
static volatile bool g_pool_ready;         // set by bus_pool_init (core1); bus task waits on it
static critical_section_t g_pool_cs;       // hw spinlock: pool free-list + node lists + roster rebuild + corr (defined early; the roster handlers use it)
static uint8_t  g_nodes_n, g_pp_selftest;
static volatile uint8_t  g_cycle_mode = 1u;       // §17: DEFAULT 1=cyclic engine (full superset); 0=per-slot rotation fallback (CMD_BUS_CYCLE_MODE)
static volatile uint32_t g_cycle_last, g_cycle_min, g_cycle_max;  // full-cycle time (us)
// §17 step4 real-time watchdog: deadline + overrun/slack/slowest-node
static volatile uint32_t g_cycle_deadline_us = 5000;   // 200 Hz default; CMD_BUS_SET_DEADLINE
static volatile uint32_t g_cycle_overruns;             // cycles exceeding the deadline
static volatile int32_t  g_cycle_minslack = 0x7FFFFFFF; // min (deadline - cycle); <0 once overran
static volatile uint8_t  g_cycle_slow_addr;            // node addr that took longest in a cycle
static volatile uint32_t g_cycle_slow_us;              // that node's slot time
// §17 step7: fixed-rate pacing + dead-node slow-poll/re-enable
static volatile uint32_t g_cycle_pace_us;              // 0=free-run; else hold each cycle to this period (jitter-free)
static volatile uint32_t g_dead_slowpoll_div = 200;    // probe one dead node every N cycles (200 @200Hz ~= 1Hz)
static volatile uint32_t g_dead_revives;               // dead->alive resurrections (stat)
static volatile uint8_t  g_nodes_dead;                  // current dead-node count (cycle-maintained; POLL_STATS reads it)
// #1: dead-node -> veto. The cycle mirrors g_nodes_dead here; interlock.c reads it via
// IL_VIRT_NODES_DEAD so a lost node trips the (wired-OR) GP0 veto. Non-static (extern'd by
// interlock.c). Master-only (a slave never polls -> stays 0).
volatile uint8_t         g_il_nodes_dead = 0;
static volatile bool     g_nodes_dirty;                 // roster changed at runtime -> cycle rebuilds g_nodes
static volatile uint32_t g_nodes_rebuilds;              // count of runtime node-table rebuilds (stat)
static uint8_t  g_dead_cursor;                          // round-robin index over dead nodes
// §17 step8: per-cycle feedback batch -- handle_slot_frame appends here while a cycle
// collects (instead of a per-frame host_link_s2m); bus_run_cycle emits ONE OP_BUS_FEEDBACK.
// Touched only by the bus task (core0) under g_lock -> no cross-core concern.
static volatile bool g_cycle_collecting;               // true while bus_run_cycle gathers feedback
static uint8_t  g_fb_buf[BUS_PAYLOAD_MAX];             // [n_rec] then [addr][len][bytes...]*
static uint8_t  g_fb_n;                                 // bytes used (incl the leading count slot)
static uint8_t  g_fb_rec;                               // record count this cycle
static volatile uint32_t g_fb_frames;                  // OP_BUS_FEEDBACK frames emitted (stat)
static volatile uint32_t g_fb_drops;                   // feedback records dropped (batch full)
static uint32_t g_dead_tick;                            // cycles since the last dead-node probe
static void bus_nodetable_build(void);
static void bus_pp_selftest(void);
static void bus_run_cycle(void);
static bool bus_msg_send(uint8_t dest_addr, const uint8_t *payload, uint8_t len, uint32_t tag);
// §17 fold: outstanding host/engine command -> slave-reply correlation (cycle mode).
static volatile uint32_t g_corr_relayed, g_corr_full;   // stats (POLL_STATS + handle_slot_frame use them)
static bool corr_add(uint16_t req, uint8_t addr, bool is_orig, uint16_t host_req);
static bool corr_take(uint16_t req, uint8_t addr, bool *is_orig, uint16_t *host_req);

static void bc_emit_boot_banner(void) {
    char b[200];
    int n = snprintf(b, sizeof b, "[boot] bus_controller boot#%u rst=%s ident=%d%s hwio=%d il=%d ilfail=0x%08X slvr=%u baud=%u neti=%d pool=%u/%u nodes=%u",
                     (unsigned)g_crash.boot_count, RST_NAME[g_crash.last_cause], g_id_rc,
                     g_identity_refused ? " REFUSED" : "", g_hwio_rc, g_ilcf_rc, (unsigned)g_il_armfail,
                     (unsigned)g_cfg_roster_n, (unsigned)g_bus_baud, g_neti_rc,
                     (unsigned)g_pool_selftest, (unsigned)g_pool_n, (unsigned)g_nodes_n);
    // WiFi config: log AP count + first SSID + shared agent endpoint; passphrase redacted to length.
    if (g_neti_rc == NETI_OK && n > 0 && n < (int)sizeof b)
        n += snprintf(b + n, (size_t)((int)sizeof b - n), " aps=%u ssid0=%s %u.%u.%u.%u:%u pwlen=%u",
                      (unsigned)g_netcfg.n_ap, g_netcfg.ap[0].ssid,
                      g_netcfg.ip[0], g_netcfg.ip[1], g_netcfg.ip[2], g_netcfg.ip[3],
                      (unsigned)g_netcfg.port, (unsigned)strlen(g_netcfg.ap[0].pass));
    if (n > (int)sizeof b) n = (int)sizeof b;
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
// C2 correlation: when the in-flight command was ENGINE-originated (not host-relayed),
// the slave's reply is re-tagged from the master's wire req_id back to the host's.
static bool     g_cmd_is_orig; static uint16_t g_cmd_host_req;
static uint16_t g_orig_seq;    // master-owned wire req_id for originated commands

// ---- inter-core seam (core0 <-> core1 app engine), FreeRTOS queues ----------
// down: a host shell-exec addressed to 0xFB; up: a frame for the host. Cross-core
// safe (xQueue is SMP-safe), single in/out per direction. host_connected is a
// core0-published condition core1 gates on (the "USB connected throughout" rule).
#define APPCORE_ARGS_MAX  64
#define APPCORE_PAY_MAX   96
// Tagged event (C3): an engine request carries WHERE its reply goes, so one
// origin-agnostic handler serves both roles. ROUTE_USB -> master uplink (host);
// ROUTE_BUS -> slave bus window (bus_src = the node that asked).
#define ROUTE_USB  0u
#define ROUTE_BUS  1u
typedef struct { uint16_t req_id, cmd; uint8_t alen; uint8_t route; uint8_t bus_src; uint8_t args[APPCORE_ARGS_MAX]; } appcore_cmd_t;
typedef struct { uint8_t dest; uint16_t opcode; uint8_t len; uint8_t payload[APPCORE_PAY_MAX]; } appcore_rep_t;
static QueueHandle_t   g_down_q;   // core0 -> core1
static QueueHandle_t   g_up_q;     // core1 -> core0
static volatile bool   g_host_connected;

// Thread-3 C2: a bus message the master's ENGINE originates to a slave node. The
// engine (core1) can't block on a bus round-trip, so kbapp enqueues this and the
// bus thread (core0) does the transaction + correlates the reply back to the host.
#define ORIG_PAY_MAX 64
typedef struct { uint8_t addr; uint16_t host_req_id; uint8_t len; uint8_t bytes[ORIG_PAY_MAX]; } orig_req_t;
static QueueHandle_t   g_orig_q;   // core1 (engine) -> core0 (bus thread)

// Ping-pong bench: a free-running chain-flow loop, master kbapp <-> slave kbapp over
// RS-485. Each end's ENGINE sets a counter (g_pp_master in kbapp_on_echo_to on the
// originating master; g_pp_slave in kbapp_on_echo on the answering slave). The slave
// piggybacks its count on every bus echo reply; the master's COLLECT captures it into
// g_pp_slave_seen, so ONE local CMD_APP_PP_READ returns both. The loop self-sustains:
// each collected reply re-injects CMD_APP_ECHO_TO (pp_kick) so the master originates
// the next round. The host/USB/WiFi is used ONLY to start/stop/read — NEVER in the loop,
// so the measured rate is the true bus chain-flow rate (no uplink latency).
static volatile uint32_t g_pp_master;      // master kbapp originations (kbapp_on_echo_to)
static volatile uint32_t g_pp_slave;       // this node's slave-side handles (kbapp_on_echo, bus route)
static volatile uint32_t g_pp_slave_seen;  // master's view of the slave count, piggybacked on each reply
static volatile uint8_t  g_pp_run;         // loop enabled
static volatile uint8_t  g_pp_addr = 9;    // slave addr to ping
static volatile uint32_t g_pp_kicks;       // §17 step6: originations ARMED (vs g_pp_master = consumed by core1)
static void pp_kick(void) {                // re-arm one round (master kbapp originates)
    if (!g_down_q) return;
    appcore_cmd_t c; memset(&c, 0, sizeof c);
    c.cmd = CMD_APP_ECHO_TO; c.route = ROUTE_USB;
    c.args[0] = g_pp_addr; c.args[1] = 'p'; c.args[2] = 'p'; c.alen = 3;
    if (xQueueSend(g_down_q, &c, 0) == pdTRUE) g_pp_kicks++;   // count only if queued (self-correcting on drops)
}

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
    g_bus_msgs++;   // perf stat: one RS-485 frame on the bus
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
        c.route  = ROUTE_USB; c.bus_src = 0;              // host-sourced -> reply to USB
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
    g_cmd_is_orig = false;   // host-relayed (not engine-originated)
    g_cmd_pending = true;
}
static void on_local_shell(void *u, uint16_t req_id, uint16_t cmd, const uint8_t *args, uint8_t alen) {
    (void)u;
    switch (cmd) {
    case CMD_ECHO:
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, args, alen); break;
    case CMD_BUS_CLEAR_ROSTER:
        // §17 fold: the cycle rebuilds g_nodes from g_roster on g_nodes_dirty. The roster
        // write is serialized with the rebuild's read on g_pool_cs (we already hold g_lock;
        // g_lock->g_pool_cs is the established order). g_pool_cs exists only after the pool
        // is carved; before that no cycle/rebuild runs, so the plain write is safe.
        if (g_pool_ready) critical_section_enter_blocking(&g_pool_cs);
        g_roster_n = 0; g_cursor = 0; g_nodes_dirty = true;
        if (g_pool_ready) critical_section_exit(&g_pool_cs);
        g_cmd_pending = false;
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0); break;
    case CMD_BUS_REGISTER_SLAVE: {
        uint8_t reason = BUS_REG_OK;
        if (alen >= 6 && g_roster_n < ROSTER_MAX) {
            if (g_pool_ready) critical_section_enter_blocking(&g_pool_cs);
            slave_t *s = &g_roster[g_roster_n]; memset(s, 0, sizeof *s);
            s->addr = args[0];
            s->class_id = (uint32_t)args[1] | ((uint32_t)args[2] << 8) |
                          ((uint32_t)args[3] << 16) | ((uint32_t)args[4] << 24);
            s->flags = args[5];
            g_roster_n++; g_nodes_dirty = true;   // publish the new entry, then flag rebuild
            if (g_pool_ready) critical_section_exit(&g_pool_cs);
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
    case CMD_BUS_POLL_STATS: {   // step 2 + §17 step1: poll counters + turnaround + pool/table
        uint32_t t = g_poll_total, a = g_poll_alive, m = g_poll_miss; uint16_t ta = g_meas_ta_us;
        uint32_t cl = g_cycle_last, cmn = g_cycle_min, cmx = g_cycle_max;
        uint32_t ov = g_cycle_overruns; int32_t sk = g_cycle_minslack; uint32_t su = g_cycle_slow_us;
        uint32_t ex = g_pool_exhausted; uint16_t lw = g_pool_low;
        uint32_t dv = g_dead_revives, pc = g_cycle_pace_us;
        uint32_t fbf = g_fb_frames, fbd = g_fb_drops;
        uint32_t crl = g_corr_relayed, crf = g_corr_full, nrb = g_nodes_rebuilds;
        uint32_t bm = g_bus_msgs, utx = g_hl.tx_msgs, urx = g_hl.rx_msgs;  // perf counters
        uint8_t ndead = g_nodes_dead;  // §17 step7 (cycle-maintained; g_nodes is defined later)
        uint32_t msil = g_miss_silent, mpar = g_miss_partial;  // miss split (b93-100)
        uint8_t r[100] = { (uint8_t)t,(uint8_t)(t>>8),(uint8_t)(t>>16),(uint8_t)(t>>24),
                          (uint8_t)a,(uint8_t)(a>>8),(uint8_t)(a>>16),(uint8_t)(a>>24),
                          (uint8_t)m,(uint8_t)(m>>8),(uint8_t)(m>>16),(uint8_t)(m>>24),
                          (uint8_t)ta,(uint8_t)(ta>>8),
                          (uint8_t)g_pool_selftest, (uint8_t)g_pool_n, g_nodes_n,  // §17 step1 [b15,16,17]
                          (uint8_t)g_pool_avail, (uint8_t)(g_pool_avail>>8),       // free count [b18,19]
                          g_pp_selftest,                                           // §17 step2 [b20]
                          (uint8_t)cl,(uint8_t)(cl>>8),(uint8_t)(cl>>16),(uint8_t)(cl>>24),    // cycle_last us [b21-24]
                          (uint8_t)cmn,(uint8_t)(cmn>>8),(uint8_t)(cmn>>16),(uint8_t)(cmn>>24),// cycle_min  [b25-28]
                          (uint8_t)cmx,(uint8_t)(cmx>>8),(uint8_t)(cmx>>16),(uint8_t)(cmx>>24), // cycle_max [b29-32]
                          (uint8_t)ov,(uint8_t)(ov>>8),(uint8_t)(ov>>16),(uint8_t)(ov>>24),     // overruns [b33-36]
                          (uint8_t)sk,(uint8_t)(sk>>8),(uint8_t)(sk>>16),(uint8_t)(sk>>24),     // min slack (signed) [b37-40]
                          g_cycle_slow_addr, (uint8_t)su,(uint8_t)(su>>8),                      // slowest node addr + us [b41-43]
                          (uint8_t)lw,(uint8_t)(lw>>8),                                          // §17 step5 pool low-water [b44-45]
                          (uint8_t)ex,(uint8_t)(ex>>8),(uint8_t)(ex>>16),(uint8_t)(ex>>24),    // pool exhausted count [b46-49]
                          (uint8_t)dv,(uint8_t)(dv>>8),(uint8_t)(dv>>16),(uint8_t)(dv>>24),    // §17 step7 dead revives [b50-53]
                          ndead,                                                               // current dead-node count [b54]
                          (uint8_t)pc,(uint8_t)(pc>>8),(uint8_t)(pc>>16),(uint8_t)(pc>>24),    // cycle pace us [b55-58]
                          (uint8_t)fbf,(uint8_t)(fbf>>8),(uint8_t)(fbf>>16),(uint8_t)(fbf>>24),// §17 step8 feedback frames [b59-62]
                          (uint8_t)fbd,(uint8_t)(fbd>>8),(uint8_t)(fbd>>16),(uint8_t)(fbd>>24),// feedback drops [b63-66]
                          (uint8_t)crl,(uint8_t)(crl>>8),(uint8_t)(crl>>16),(uint8_t)(crl>>24),// §17 fold cmd replies relayed [b67-70]
                          (uint8_t)crf,(uint8_t)(crf>>8),(uint8_t)(crf>>16),(uint8_t)(crf>>24),// corr-table-full drops [b71-74]
                          (uint8_t)nrb,(uint8_t)(nrb>>8),(uint8_t)(nrb>>16),(uint8_t)(nrb>>24),// §17 fold node-table rebuilds [b75-78]
                          g_uplink_mode,    // §dual-transport: 0=standalone 1=usb 2=wifi [b79]
                          g_netcfg.n_ap,    // §step5: # WiFi credentials loaded [b80]
                          (uint8_t)bm,(uint8_t)(bm>>8),(uint8_t)(bm>>16),(uint8_t)(bm>>24),    // perf: intra-bus msgs [b81-84]
                          (uint8_t)utx,(uint8_t)(utx>>8),(uint8_t)(utx>>16),(uint8_t)(utx>>24),// perf: uplink TX msgs (USB/UDP) [b85-88]
                          (uint8_t)urx,(uint8_t)(urx>>8),(uint8_t)(urx>>16),(uint8_t)(urx>>24),// perf: uplink RX msgs (USB/UDP) [b89-92]
                          (uint8_t)msil,(uint8_t)(msil>>8),(uint8_t)(msil>>16),(uint8_t)(msil>>24),// miss SILENT (node absent) [b93-96]
                          (uint8_t)mpar,(uint8_t)(mpar>>8),(uint8_t)(mpar>>16),(uint8_t)(mpar>>24)};// miss PARTIAL (reply stalled) [b97-100]
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, r, 100); break;
    }
    case CMD_BUS_MEASURE_TA:     // turnaround is now measured every slot (RX ISR); just latch it
        g_meas_ta_us = g_last_ta_us;
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0); break;
    case CMD_BUS_CYCLE_MODE:     // §17 step3: [0|1] select per-slot rotation vs the cyclic engine
        if (alen >= 1) { g_cycle_mode = args[0] ? 1u : 0u; g_cycle_min = 0; g_cycle_max = 0; }
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0); break;
    case CMD_BUS_MSG_INJECT: {   // [addr][count][payload...] -> inject `count` copies (tests exhaustion)
        uint8_t inj = 0;
        if (alen >= 2) {
            uint8_t addr = args[0], cnt = args[1];
            const uint8_t *pl = (alen > 2) ? &args[2] : NULL; uint8_t pn = (alen > 2) ? (uint8_t)(alen - 2) : 0;
            for (uint8_t i = 0; i < cnt; i++) { if (bus_msg_send(addr, pl, pn, 0xDEAD0000u | i)) inj++; else break; }
        }
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, &inj, 1); break;
    }
    case CMD_BUS_SET_DEADLINE:   // §17 step4: set the real-time cycle deadline + reset watchdog stats
        if (alen >= 4) g_cycle_deadline_us = (uint32_t)args[0]|((uint32_t)args[1]<<8)|((uint32_t)args[2]<<16)|((uint32_t)args[3]<<24);
        g_cycle_overruns = 0; g_cycle_minslack = 0x7FFFFFFF;
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0); break;
    case CMD_BUS_SET_PACE:   // §17 step7: [pace_us u32] (0=free-run) [+ slowpoll_div u32 opt]
        if (alen >= 4) g_cycle_pace_us = (uint32_t)args[0]|((uint32_t)args[1]<<8)|((uint32_t)args[2]<<16)|((uint32_t)args[3]<<24);
        if (alen >= 8) g_dead_slowpoll_div = (uint32_t)args[4]|((uint32_t)args[5]<<8)|((uint32_t)args[6]<<16)|((uint32_t)args[7]<<24);
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0); break;
    case CMD_BUS_UPLINK_FORCE: {   // §dual-transport: [mode u8][dur_ms u32] pin the uplink mode (auto-revert)
        uint8_t fm = (alen >= 1) ? args[0] : 0xFFu;
        uint32_t dur = (alen >= 5) ? ((uint32_t)args[1]|((uint32_t)args[2]<<8)|((uint32_t)args[3]<<16)|((uint32_t)args[4]<<24)) : 0u;
        g_uplink_force_until = to_ms_since_boot(get_absolute_time()) + dur;
        g_uplink_force = fm;   // 0xFF = release to the auto selector immediately
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0); break;
    }
    case CMD_APP_PP_START:   // [addr] -> start the free-running chain-flow ping-pong loop
        if (alen >= 1) g_pp_addr = args[0];
        g_pp_master = 0; g_pp_slave_seen = 0; g_pp_kicks = 0;
        g_pp_run = 1;
        pp_kick();           // fire the first round; per-slot COLLECT (or §17 cycle) re-arms each subsequent round
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0); break;
    case CMD_APP_PP_STOP:
        g_pp_run = 0;
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, NULL, 0); break;
    case CMD_APP_PP_READ: {  // -> [master_cnt u32][slave_seen u32][this_node_slave_handles u32]
        // master_cnt/slave_seen are the MASTER's view (slave_seen via per-slot COLLECT, 0 in cycle
        // mode). this_node_slave_handles = g_pp_slave (§17 step6: query THIS node over USB to prove
        // engine-produced echoes were delivered to+handled by the slave's own engine, fire-and-forget).
        uint32_t mc = g_pp_master, sc = g_pp_slave_seen, ps = g_pp_slave;
        uint8_t r[12] = { (uint8_t)mc, (uint8_t)(mc>>8), (uint8_t)(mc>>16), (uint8_t)(mc>>24),
                          (uint8_t)sc, (uint8_t)(sc>>8), (uint8_t)(sc>>16), (uint8_t)(sc>>24),
                          (uint8_t)ps, (uint8_t)(ps>>8), (uint8_t)(ps>>16), (uint8_t)(ps>>24) };
        host_link_shell_reply(&g_hl, req_id, SHELL_OK, r, 12); break;
    }
    default: {
        // Chain to the shared node dispatcher (echo / GPIO / interlock status+clear), so
        // the MASTER exposes the same operate commands over its USB shell that a slave
        // answers over the bus — e.g. CMD_INTERLOCK_STATUS for its own Thread-2 interlock.
        uint8_t out[64]; uint8_t outlen = 0;
        uint8_t st = node_cmd_dispatch(cmd, args, alen, out, sizeof out, &outlen);
        if (st != CMD_NOT_MINE) host_link_shell_reply(&g_hl, req_id, st, out, outlen);
        else                    host_link_shell_reply(&g_hl, req_id, SHELL_UNKNOWN_CMD, NULL, 0);
        break;
    }
    }
}

// ---- bus arbiter step 1: idle-gap slot collector (see docs/bus-arbiter-spec.md) -
// Replaces the blocking single-frame bus_recv on the SWEEP liveness path with an
// idle-gap delimited burst collect: poll <addr>, gather every frame it sends until
// the bus is idle for T_GAP_US (end of burst), T_RESP_US elapses with no reply
// (dead node), or T_WINDOW_MAX_US (anti-babble). Still task-driven (step 1): it
// YIELDS while waiting for the first word (ms-scale T_RESP, no core0 starvation) and
// busy-spins only during the short burst for us-precision idle detection. The ISR
// migration (steps 2-3) removes the spin entirely. Values are for 460800; make them
// bit-time-relative later (TODO in the spec).
typedef enum { SLOT_NO_RESPONSE = 0, SLOT_OK, SLOT_BABBLE } slot_result_t;

// Deliver one assembled frame from the granted node (relay reply / record summary).
static void handle_slot_frame(uint8_t addr, const bus_frame_t *rf) {
    uint8_t cls = (uint8_t)(rf->type & BUS_FT_MASK);
    LOCK();
    if (cls == BUS_FT_DATA && rf->len >= 2 &&
        rf->payload[0] == (uint8_t)(OP_SHELL_REPLY & 0xFF) &&
        rf->payload[1] == (uint8_t)(OP_SHELL_REPLY >> 8)) {
        uint8_t plen = (uint8_t)(rf->len - 2);
        // §17 fold: is this the reply to an outstanding host/engine command? The reply
        // carries the wire req_id at payload[2..3]. If correlated, RELAY it to the
        // requester (host RPC / re-tagged engine origination) instead of batching it.
        uint16_t rrq = (plen >= 2) ? ((uint16_t)rf->payload[2] | ((uint16_t)rf->payload[3] << 8)) : 0;
        bool is_orig = false; uint16_t host_req = 0;
        if (plen >= 2 && corr_take(rrq, addr, &is_orig, &host_req)) {
            g_corr_relayed++;
            if (is_orig) {   // engine origination (chain-flow / pp) -- mirror the per-slot COLLECT
                uint8_t rb[BUS_PAYLOAD_MAX]; uint8_t rn = plen;
                memcpy(rb, &rf->payload[2], rn);
                if (rn >= 7) {   // the bus echo ALWAYS piggybacks the slave's u32 count -> capture+strip
                    g_pp_slave_seen = (uint32_t)rb[rn-4] | ((uint32_t)rb[rn-3] << 8) |
                                      ((uint32_t)rb[rn-2] << 16) | ((uint32_t)rb[rn-1] << 24);
                    rn -= 4;
                }
                if (!g_pp_run) {   // re-tag the wire req_id back to the host's, relay to APPCORE
                    if (rn >= 2) { rb[0] = (uint8_t)host_req; rb[1] = (uint8_t)(host_req >> 8); }
                    (void)host_link_s2m(&g_hl, BUS_ADDR_APPCORE, OP_SHELL_REPLY, rb, rn);
                }
            } else {           // host direct command -- the req_id is already the host's; relay as-is
                (void)host_link_s2m(&g_hl, addr, OP_SHELL_REPLY, &rf->payload[2], plen);
            }
        } else if (g_cycle_collecting) {   // §17 step8: unsolicited -> batch as feedback (one frame/cycle)
            if ((int)g_fb_n + 2 + (int)plen <= (int)sizeof g_fb_buf) {
                g_fb_buf[g_fb_n++] = addr; g_fb_buf[g_fb_n++] = plen;
                for (uint8_t i = 0; i < plen; i++) g_fb_buf[g_fb_n++] = rf->payload[2 + i];
                g_fb_rec++;
            } else { g_fb_drops++; }
        } else {
            (void)host_link_s2m(&g_hl, addr, OP_SHELL_REPLY, &rf->payload[2], plen);
        }
    } else if (cls == BUS_FT_NO_MESSAGE) {
        slave_t *s = roster_find(addr); if (s && rf->len >= 1) s->summary = rf->payload[0];
    }
    UNLOCK();
}

// Poll `addr` and collect its whole reply burst. The bus task BLOCKS for the entire
// slot: arm the idle-gap alarm to the T_RESP first-word deadline, then sleep. The RX
// ISR (master_rx_hook) pushes the alarm out by T_GAP on each word, so the alarm fires
// ONCE -- at last-word+T_GAP (burst complete) or at T_RESP (no reply). core0 is free
// the entire wait (no busy-spin); one wake per slot. The burst is drained + assembled
// here afterwards (idle has elapsed, so it is complete).
static slot_result_t bus_poll_slot(uint8_t addr, bus_frame_t *rf) {
    g_slot_first = false;
    (void)ulTaskNotifyTake(pdTRUE, 0);                 // clear stale notify
    bus_send(addr, BUS_FT_POLL, NULL, 0);              // flushes the RX ring, then TX
    g_slot_poll_us = time_us_32();
    hardware_alarm_set_target(g_gap_alarm, make_timeout_time_us(T_RESP_US));
    g_slot_active = true;                               // RX ISR now defers the alarm per word
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(T_WINDOW_MAX_US / 1000u + 4u));  // wake on alarm (or safety)
    g_slot_active = false;
    hardware_alarm_cancel(g_gap_alarm);
    bool from_addr = false;
    uint16_t w;
    // Drain + grace-extend. A complete frame from `addr` ends the slot. But the fast
    // T_GAP idle alarm can fire WHILE the slave is still transmitting: under load the
    // slave's reply burst stalls (its TX FIFO underruns while it runs an injected
    // command), opening a >T_GAP gap mid-burst. The PHY drops no words here (FIFO not
    // overrun) -- the rest simply hasn't been sent yet. So once a reply has STARTED
    // (g_slot_first: at least one word heard, even just the leading preamble, which
    // leaves the assembler IDLE), don't miss on a premature close: hold the slot open
    // and re-drain until the frame completes or the T_WINDOW_MAX budget is spent. A
    // slot that drew no word at all (true silent miss / dead node) breaks immediately.
    // The no-stall case assembles on the first pass -> zero throughput cost.
    for (;;) {
        while (bus_phy_rx_pop(&w)) {
            if (bus_asm_feed(&g_bc, w, rf) && rf->src == addr) { from_addr = true; handle_slot_frame(addr, rf); }
        }
        if (from_addr || !g_slot_first) break;                                   // complete, or no reply started
        if ((uint32_t)(time_us_32() - g_slot_poll_us) >= T_WINDOW_MAX_US) break; // anti-babble budget spent
        hardware_alarm_set_target(g_gap_alarm, make_timeout_time_us(T_RESP_US)); // wait for the burst to resume
        g_slot_active = true;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(T_WINDOW_MAX_US / 1000u + 4u));
        g_slot_active = false;
        hardware_alarm_cancel(g_gap_alarm);
    }
    bool babble = (uint32_t)(time_us_32() - g_slot_poll_us) >= T_WINDOW_MAX_US;
    if (!from_addr) {
        if (g_slot_first) g_miss_partial++;   // a reply started but never completed (stall beyond the window / CRC)
        else              g_miss_silent++;    // no word at all -> node absent/dead
        return SLOT_NO_RESPONSE;
    }
    return babble ? SLOT_BABBLE : SLOT_OK;
}

// ---- bus thread: per-iteration state machine, one bus transaction per pass --
enum { BS_SWEEP = 0, BS_CMD_INJECT, BS_CMD_COLLECT };

static void bus_control_task(void *arg) {
    (void)arg;
    g_bus_task = xTaskGetCurrentTaskHandle();   // step 3: RX ISR / alarm wake this task
    g_gap_alarm = (uint)hardware_alarm_claim_unused(true);
    hardware_alarm_set_callback(g_gap_alarm, rot_alarm_cb);
    g_rx_hook = master_rx_hook;                 // RX ISR defers the idle-gap alarm (no busy-wait)
    while (!g_pool_ready) vTaskDelay(1);         // wait for the pool carve (engine task, core1)
    bus_nodetable_build();                      // §17 step 1: node table ordered by bus address
    bus_pp_selftest();                          // §17 step 2: exercise attach->swap->drain
    uint8_t  state = BS_SWEEP, collect_slave = 0, collect_tries = 0;
    bool     collect_is_orig = false; uint16_t collect_host = 0;
    for (;;) {
        g_hb[HB_BUS]++; g_hb_us[HB_BUS] = time_us_32();
        // Identity refused -> quarantine: never drive the bus. Heartbeat keeps
        // ticking (above) so the watchdog is satisfied; the monitor/uplink stay
        // alive so the host can attach and read the refusal. (No bus_send here.)
        if (g_identity_refused) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        // §17 step3: the cyclic engine runs in place of the per-slot rotation when enabled.
        if (g_cycle_mode) { bus_run_cycle(); LOCK(); emit_liveness_edges(); UNLOCK(); continue; }
        uint32_t now = to_ms_since_boot(get_absolute_time());
        bus_frame_t rf;

        // C2: when idle, promote one engine-originated request into the in-flight slot.
        // The engine (core1) handed us {addr, host_req_id, payload}; we mint a master
        // wire req_id and build an OP_SHELL_EXEC for NODE_CMD_ECHO, then let the normal
        // INJECT/COLLECT path run — COLLECT re-tags the reply back to host_req_id.
        if (state == BS_SWEEP) {
            orig_req_t o;
            LOCK();
            // Hold the lock across check+set so a host command (on_bus_msg, same lock)
            // can't claim the in-flight slot between our test and our write.
            if (!g_cmd_pending && xQueueReceive(g_orig_q, &o, 0) == pdTRUE) {
                uint16_t sr = (uint16_t)(++g_orig_seq); if (sr == 0) sr = (uint16_t)(++g_orig_seq);
                uint8_t bn = 0;
                g_cmd_body[bn++] = (uint8_t)sr;                    g_cmd_body[bn++] = (uint8_t)(sr >> 8);
                g_cmd_body[bn++] = (uint8_t)(CMD_APP_ECHO & 0xFF); g_cmd_body[bn++] = (uint8_t)(CMD_APP_ECHO >> 8); // slave's engine (kbapp) handles it
                uint8_t m = o.len; if (m > (uint8_t)(BUS_PAYLOAD_MAX - bn)) m = (uint8_t)(BUS_PAYLOAD_MAX - bn);
                for (uint8_t i = 0; i < m; i++) g_cmd_body[bn++] = o.bytes[i];
                g_cmd_slave = o.addr; g_cmd_op = OP_SHELL_EXEC; g_cmd_len = bn;
                g_cmd_req_id = sr; g_cmd_is_orig = true; g_cmd_host_req = o.host_req_id;
                g_cmd_pending = true;
            }
            UNLOCK();
        }

        // A queued command pre-empts the routine sweep.
        bool start_cmd = false; uint8_t cslave = 0, clen = 0; uint16_t cop = 0;
        bool c_is_orig = false; uint16_t c_host = 0;
        uint8_t cbuf[BUS_PAYLOAD_MAX];
        LOCK();
        if (state == BS_SWEEP && g_cmd_pending) {
            start_cmd = true; cslave = g_cmd_slave; cop = g_cmd_op; clen = g_cmd_len;
            memcpy(cbuf, g_cmd_body, clen);
            c_is_orig = g_cmd_is_orig; c_host = g_cmd_host_req;
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
                if (!c_is_orig && (cls == BUS_FT_ACK || cls == BUS_FT_NAK)) {
                    // host-relay only: the host tracks the two-phase ACK/NAK. An
                    // engine-originated command has no host ACK waiter — skip it.
                    uint8_t b[3] = { cslave, (uint8_t)g_cmd_req_id, (uint8_t)(g_cmd_req_id >> 8) };
                    (void)host_link_s2m(&g_hl, cslave, cls == BUS_FT_NAK ? OP_BUS_CMD_NAK : OP_BUS_CMD_ACK, b, 3);
                }
            }
            g_cmd_pending = false;            // bus freed; reply rides a later poll
            UNLOCK();
            collect_slave = cslave; collect_tries = CMD_COLLECT_TRIES; state = BS_CMD_COLLECT;
            collect_is_orig = c_is_orig; collect_host = c_host;
        } else if (state == BS_CMD_COLLECT) {
            bool pp_round = (collect_is_orig && g_pp_run);   // ping-pong: re-arm when this round ends
            bus_send(collect_slave, BUS_FT_POLL, NULL, 0);
            int got = bus_recv(&rf, CMD_COLLECT_TIMEOUT_MS);
            if (got && rf.src == collect_slave) {
                LOCK(); mark_alive(collect_slave, now);
                uint8_t cls = (uint8_t)(rf.type & BUS_FT_MASK);
                if (cls == BUS_FT_DATA && rf.len >= 2 &&
                    rf.payload[0] == (uint8_t)(OP_SHELL_REPLY & 0xFF) &&
                    rf.payload[1] == (uint8_t)(OP_SHELL_REPLY >> 8)) {
                    if (collect_is_orig) {
                        // C2 correlation: re-tag the slave's reply [wire_req][status][echo]
                        // to the host's req_id. Ping-pong piggybacks a u32 slave count as the
                        // last 4 bytes -> capture+strip it; during a pp bench (g_pp_run) the
                        // round is consumed here, not surfaced to the host.
                        uint8_t rb[BUS_PAYLOAD_MAX]; uint8_t rn = (uint8_t)(rf.len - 2);
                        memcpy(rb, &rf.payload[2], rn);
                        if (rn >= 7) {   // [req2][status1][..echo..][cnt4]
                            g_pp_slave_seen = (uint32_t)rb[rn-4] | ((uint32_t)rb[rn-3] << 8) |
                                              ((uint32_t)rb[rn-2] << 16) | ((uint32_t)rb[rn-1] << 24);
                            rn -= 4;
                        }
                        if (!g_pp_run) {
                            if (rn >= 2) { rb[0] = (uint8_t)collect_host; rb[1] = (uint8_t)(collect_host >> 8); }
                            (void)host_link_s2m(&g_hl, BUS_ADDR_APPCORE, OP_SHELL_REPLY, rb, rn);
                        }
                    } else {
                        (void)host_link_s2m(&g_hl, collect_slave, OP_SHELL_REPLY, &rf.payload[2], (uint8_t)(rf.len - 2));
                    }
                    collect_is_orig = false;
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
            if (pp_round && state == BS_SWEEP) pp_kick();   // free-running: originate the next round
        } else {  // BS_SWEEP
            // Step 2/3: back-to-back rotation — no g_poll_period_ms gate; poll the next
            // enabled node every pass. bus_poll_slot blocks on the RX-ISR notify while
            // awaiting the reply (us latency, yields core0/USB), so no 1 ms tick wait.
            uint8_t addr = 0xFF;
            if (g_poll_enabled) { LOCK(); addr = next_enabled_addr(); UNLOCK(); }
            if (addr != 0xFF) {
                slot_result_t sr = bus_poll_slot(addr, &rf);   // blocks the whole slot (core0 free)
                uint32_t seen_ms = to_ms_since_boot(get_absolute_time());
                LOCK();
                g_poll_total++;
                if (sr == SLOT_NO_RESPONSE) { g_poll_miss++; mark_miss(addr); }
                else { g_poll_alive++; mark_alive(addr, seen_ms); g_meas_ta_us = g_last_ta_us; }  // SLOT_OK or SLOT_BABBLE
                UNLOCK();
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));   // nothing enabled to poll -> idle
            }
        }

        LOCK(); emit_liveness_edges(); UNLOCK();
        // Step 3: no unconditional inter-slot delay — each path already yields
        // (bus_poll_slot blocks on the RX-ISR notify; bus_recv yields; idle SWEEP
        // vTaskDelay(1)). This is what lets the rotation run at the bus/turnaround limit.
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

// ---- per-slice uplink PUMPS (non-blocking; the supervisor calls one per iteration) ---------
// host_link is transport-agnostic: each pump does the SAME drain-TX / feed-RX work over its
// transport, so USB can preempt WiFi mid-stream just by switching which pump the supervisor runs.

// USB-CDC: feed host->us getchar, tick, relay core1 replies, drain us->host putchar.
static void usb_pump(void) {
    uint8_t out[64]; uint32_t n; appcore_rep_t up;
    LOCK();
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
        host_link_feed(&g_hl, (uint8_t)c);     // may invoke on_bus_msg/on_local_shell
    host_link_tick(&g_hl, to_ms_since_boot(get_absolute_time()), true);
    while (xQueueReceive(g_up_q, &up, 0) == pdTRUE)   // relay core1 replies/reports
        (void)host_link_s2m(&g_hl, up.dest, up.opcode, up.payload, up.len);
    n = host_link_tx_drain(&g_hl, out, sizeof out);
    UNLOCK();
    if (n) { for (uint32_t i = 0; i < n; i++) putchar_raw(out[i]); stdio_flush(); }
}

// Standalone (NO uplink): SINK so nothing fills while there is no host -- discard core1 replies
// and drain+drop any host_link TX. (Step 3 gates the §17 cycle's emits at the source via
// g_uplink_active; this pump-side sink is the safety net so a buffer can never wedge the cycle.)
static void standalone_pump(void) {
    uint8_t out[64]; appcore_rep_t up;
    LOCK();
    host_link_tick(&g_hl, to_ms_since_boot(get_absolute_time()), false);
    while (xQueueReceive(g_up_q, &up, 0) == pdTRUE) { /* discard: no uplink */ }
    (void)host_link_tx_drain(&g_hl, out, sizeof out);   // drain + discard
    UNLOCK();
}

// Entering an uplink mode (USB/WiFi): fresh host_link boot + restore the commissioned roster +
// re-announce identity. Entering standalone: just publish "no host".
static void uplink_enter(bool active) {
    LOCK();
    host_link_reset_boot(&g_hl);
    g_cmd_pending = false;
    bc_load_cfg_roster();   // drop host-registered slaves; restore the commissioned roster
    UNLOCK();
    g_host_connected = active;   // core0-published condition for core1 (stream gating etc.)
    g_uplink_active  = active;
    if (active) bc_emit_boot_banner();   // announce identity to the freshly-bound host/agent
}

// ---- WiFi uplink (§dual-transport: always compiled): same host_link frame stream over UDP to the -----
// Linux zenoh-agent instead of USB-CDC. host_link is transport-agnostic, so this is
// the USB uplink_task with getchar/putchar swapped for lwip recv/send. EVERYTHING is
// non-blocking/polled: the HW watchdog (4 s) gates on HB_UPLINK going stale >500 ms,
// so the WiFi join (async + poll) and the agent connect (non-blocking + select) must
// never block this task. USB-CDC stays enabled for flashing + the BOOTSEL reset path.
#define WIFI_HB() do { g_hb[HB_UPLINK]++; g_hb_us[HB_UPLINK] = time_us_32(); } while (0)

// Link is healthy only if associated AND we hold an IP. Checking the IP alone misses
// the "silently disassociated but the netif IP lingers" case (the power-save drop), so
// query the actual WiFi link state too — that's what makes the supervisor rejoin.
static bool wifi_link_up(struct netif *nif) {
    return cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) >= CYW43_LINK_JOIN
           && !ip4_addr_isany(netif_ip4_addr(nif));
}
// Disable CYW43 power-save: default PM silently disassociates while link status still
// reads "connected" (pico-sdk #2153). NO_POWERSAVE (CYW43_DEFAULT_PM & ~0xf) stops it.
static void wifi_no_powersave(void) { cyw43_wifi_pm(&cyw43_state, CYW43_DEFAULT_PM & ~0xfu); }

// UDP transport (the only uplink transport): connectionless. connect() just sets the default peer on
// lwIP (no handshake), so the shared serve loop's send/recv work unchanged AND there's no
// dial/re-dial to fail or wedge. fd>=0, or -1. SLIP framing tolerates datagram boundaries.
static int wifi_udp_open(const netcfg_t *nc) {
    int fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = lwip_htons(nc->port);
    a.sin_addr.s_addr = (uint32_t)nc->ip[0] | ((uint32_t)nc->ip[1] << 8) |
                        ((uint32_t)nc->ip[2] << 16) | ((uint32_t)nc->ip[3] << 24);
    if (lwip_connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { lwip_close(fd); return -1; }
    return fd;
}

// WiFi/UDP pump: ONE non-blocking slice of host_link over the agent socket. Returns false if the
// link/socket died (-> supervisor closes fd + re-evaluates -> standalone/rejoin). MSG_DONTWAIT:
// this lwIP doesn't honor SO_RCVTIMEO on the UDP recvmbox, so a blocking recv would hang the task
// >4 s and the HW watchdog would reboot us (the old UDP reboot loop).
static bool wifi_pump(int fd, struct netif *nif) {
    if (!wifi_link_up(nif)) return false;            // WiFi dropped under us
    uint8_t rx[128], out[256];
    LOCK();
    host_link_tick(&g_hl, to_ms_since_boot(get_absolute_time()), true);
    appcore_rep_t up;
    while (xQueueReceive(g_up_q, &up, 0) == pdTRUE)
        (void)host_link_s2m(&g_hl, up.dest, up.opcode, up.payload, up.len);
    uint32_t m = host_link_tx_drain(&g_hl, out, sizeof out);
    UNLOCK();
    if (m && lwip_write(fd, out, m) < 0) return false;   // dead link
    int n = lwip_recv(fd, rx, sizeof rx, MSG_DONTWAIT);
    if (n > 0) { LOCK(); for (int i = 0; i < n; i++) host_link_feed(&g_hl, rx[i]); UNLOCK(); }
    return true;
}

// §dual-transport step 2: the ONE uplink SUPERVISOR. cyw43 init'd once; WiFi (re)joins in the
// BACKGROUND (sliced async, never blocking) so it is warm the instant USB drops. Each iteration:
// pick the desired transport (USB > WiFi > standalone), debounce + commit the switch, then pump one
// slice of the active mode. USB PREEMPTS WiFi at any time. The bus/engine/interlock run on other
// tasks regardless of mode; standalone just sinks (uplink inactive).
#define UPLINK_DEBOUNCE 10u      // stable iterations (~30-40 ms) before committing a mode change
static void uplink_supervisor_task(void *arg) {
    (void)arg;
    WIFI_HB();
    bool wifi_ok = (cyw43_arch_init() == 0);
    struct netif *nif = NULL;
    if (wifi_ok) { cyw43_arch_enable_sta_mode(); wifi_no_powersave(); nif = &cyw43_state.netif[CYW43_ITF_STA]; }
    g_uplink_mode = UPL_STANDALONE; g_host_connected = false; g_uplink_active = false;

    int fd = -1;
    bool join_pending = false; uint32_t join_deadline = 0; uint16_t join_fails = 0;
    uint8_t ap_idx = 0;   // §step5: which credential we're currently trying (try-each by priority)
    uint8_t want = UPL_STANDALONE, want_n = 0;

    for (;;) {
        WIFI_HB();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        bool usb = stdio_usb_connected();

        // ---- background WiFi join management (sliced; skipped while USB is preferred) ----
        // §step5: MULTIPLE credentials -- try ap[ap_idx]; on fail/timeout advance to the next
        // (round-robin by priority) so the node joins whichever known AP is in range. The agent
        // endpoint (ip:port) is shared, so wifi_udp_open(&g_netcfg) is credential-independent.
        if (wifi_ok && g_netcfg.present && !usb) {
            if (!wifi_link_up(nif)) {
                if (fd >= 0) { lwip_close(fd); fd = -1; }
                if (ap_idx >= g_netcfg.n_ap) ap_idx = 0;
                if (!join_pending) {
                    cyw43_arch_wifi_connect_async(g_netcfg.ap[ap_idx].ssid, g_netcfg.ap[ap_idx].pass,
                                                  CYW43_AUTH_WPA2_AES_PSK);
                    join_pending = true; join_deadline = now + 10000u;    // ~10s/cred (present APs join in <6s)
                } else if (cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) < 0 ||
                           (int32_t)(now - join_deadline) > 0) {          // this credential failed / timed out
                    join_pending = false;
                    if (g_netcfg.n_ap > 1) ap_idx = (uint8_t)((ap_idx + 1u) % g_netcfg.n_ap);  // next credential
                    // Reset the radio only if NO credential connects after several full passes
                    // (a wedged CYW43, not merely out-of-range APs).
                    if (++join_fails >= (uint16_t)(5u * g_netcfg.n_ap)) {
                        cyw43_arch_deinit(); vTaskDelay(pdMS_TO_TICKS(150));
                        if (cyw43_arch_init() == 0) { cyw43_arch_enable_sta_mode(); wifi_no_powersave(); }
                        join_fails = 0;
                    }
                }
            } else {                                                      // associated + IP
                join_pending = false; join_fails = 0;
                if (fd < 0) fd = wifi_udp_open(&g_netcfg);
            }
        }

        // ---- desired transport (USB > WiFi > standalone) ----
        uint8_t desired = usb ? UPL_USB
                        : ((wifi_ok && wifi_link_up(nif) && fd >= 0) ? UPL_WIFI : UPL_STANDALONE);
        // force-override window (testing/ops): pin a mode, auto-revert to the selector when it expires.
        if (g_uplink_force != 0xFFu) {
            if ((int32_t)(now - g_uplink_force_until) < 0) desired = g_uplink_force;
            else g_uplink_force = 0xFFu;   // expired -> back to auto
        }

        // ---- debounce, then commit the switch (USB preempts; a brief DTR/link blip won't thrash) ----
        if (desired != g_uplink_mode) {
            if (desired == want) want_n++; else { want = desired; want_n = 1; }
            if (want_n >= UPLINK_DEBOUNCE) {
                if (g_uplink_mode == UPL_WIFI && fd >= 0) { lwip_close(fd); fd = -1; }  // leaving WiFi
                g_uplink_mode = desired; want_n = 0;
                uplink_enter(desired != UPL_STANDALONE);
            }
        } else { want = desired; want_n = 0; }

        // ---- pump one slice of the active mode ----
        if (g_uplink_mode == UPL_USB)        usb_pump();
        else if (g_uplink_mode == UPL_WIFI) {
            if (!wifi_pump(fd, nif)) {        // link/socket died -> drop to standalone; rejoin next loops
                if (fd >= 0) { lwip_close(fd); fd = -1; }
                g_uplink_mode = UPL_STANDALONE; g_host_connected = false; g_uplink_active = false;
                want = UPL_STANDALONE; want_n = 0;
            }
        } else                               standalone_pump();

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

// ==== Bus arbiter rewrite (spec §17) step 1: TX buffer pool + node table ======
// Buffers are carved ONCE from cfl_perm (a bump allocator -- no free) at init, then
// grabbed/recovered via a free-list guarded by a HW spinlock (critical_section_t:
// ISR + cross-core safe, unlike a FreeRTOS mutex). cfl_perm is untouched after init.
#define BUS_POOL_N   24u
typedef struct tx_buf { struct tx_buf *next; uint8_t dest_node; uint8_t len;
                        uint8_t payload[BUS_PAYLOAD_MAX]; uint32_t origin_tag; } tx_buf_t;
// g_pool_cs defined earlier (the roster handlers use it before this block)
static tx_buf_t          *g_pool_free;                 // free-list head
// g_pool_avail, g_pool_low, g_pool_n, g_pool_selftest declared earlier (banner/POLL_STATS use them)

static tx_buf_t *pool_grab(void) {
    critical_section_enter_blocking(&g_pool_cs);
    tx_buf_t *b = g_pool_free;
    if (b) { g_pool_free = b->next; if (--g_pool_avail < g_pool_low) g_pool_low = g_pool_avail; }
    critical_section_exit(&g_pool_cs);
    return b;   // NULL = exhausted (caller raises the exception + zenoh notify -- later step)
}
static void pool_recover(tx_buf_t *b) {
    if (!b) return;
    critical_section_enter_blocking(&g_pool_cs);
    b->next = g_pool_free; g_pool_free = b; g_pool_avail++;
    critical_section_exit(&g_pool_cs);
}

// §17 fold: host/engine command -> slave-reply correlation for cycle mode. The cycle
// fires commands fire-and-forget (producer path); the reply rides a later NO_MSG poll,
// so we can't block-and-collect like the per-slot path. Instead we record the wire
// req_id here and, when handle_slot_frame later sees a SHELL_REPLY carrying it, relay it
// to the requester instead of batching it as feedback. Guarded by g_pool_cs because a
// core1 producer (kbapp_on_echo_to) and the core0 cycle/handle_slot_frame all touch it.
#define CMDCORR_MAX 8u
typedef struct { uint16_t req; uint16_t host_req; uint8_t addr; uint8_t is_orig; uint8_t used; } cmdcorr_t;
static cmdcorr_t g_corr[CMDCORR_MAX];   // g_corr_relayed/g_corr_full declared earlier (POLL_STATS uses them)
static bool corr_add(uint16_t req, uint8_t addr, bool is_orig, uint16_t host_req) {
    bool ok = false;
    critical_section_enter_blocking(&g_pool_cs);
    for (uint8_t i = 0; i < CMDCORR_MAX; i++) if (!g_corr[i].used) {
        g_corr[i].req = req; g_corr[i].addr = addr; g_corr[i].is_orig = is_orig ? 1u : 0u;
        g_corr[i].host_req = host_req; g_corr[i].used = 1u; ok = true; break;
    }
    critical_section_exit(&g_pool_cs);
    if (!ok) g_corr_full++;
    return ok;
}
static bool corr_take(uint16_t req, uint8_t addr, bool *is_orig, uint16_t *host_req) {
    bool hit = false;
    critical_section_enter_blocking(&g_pool_cs);
    for (uint8_t i = 0; i < CMDCORR_MAX; i++) if (g_corr[i].used && g_corr[i].req == req && g_corr[i].addr == addr) {
        *is_orig = g_corr[i].is_orig != 0; *host_req = g_corr[i].host_req; g_corr[i].used = 0u; hit = true; break;
    }
    critical_section_exit(&g_pool_cs);
    return hit;
}
static void bus_pool_init(void) {            // call once, after cfl_runtime_create (g_perm ready)
    if (g_pool_n) return;
    critical_section_init(&g_pool_cs);
    g_pool_free = NULL; g_pool_avail = 0;
    for (uint16_t i = 0; i < BUS_POOL_N; i++) {
        tx_buf_t *b = (tx_buf_t *)cfl_perm_alloc_pointer_aligned(&g_perm, (uint16_t)sizeof(tx_buf_t), 4);
        if (!b) break;                       // cfl_perm exhausted
        pool_recover(b);
    }
    g_pool_n = g_pool_avail; g_pool_low = g_pool_avail;
    // self-test: grab all (pool then empty -> NULL), recover all; avail must round-trip.
    tx_buf_t *got[BUS_POOL_N]; uint16_t k = 0; tx_buf_t *b;
    while (k < g_pool_n && (b = pool_grab()) != NULL) got[k++] = b;
    bool empty_ok = (pool_grab() == NULL);
    for (uint16_t i = 0; i < k; i++) pool_recover(got[i]);
    g_pool_selftest = (k == g_pool_n && empty_ok && g_pool_avail == g_pool_n) ? g_pool_n : 0;
    g_pool_ready = true;   // pool fully carved + self-tested -> safe for the bus task to use
}

// Node table, ordered by bus address: the cycle sweep index + per-node dead-flags +
// the per-node ping-pong TX list heads (list[fill] for producers, list[active] for the cycle).
typedef struct { uint8_t addr; uint8_t enabled; uint8_t dead; uint8_t miss; tx_buf_t *list[2]; } bus_node_t;
static bus_node_t g_nodes[ROSTER_MAX];        // g_nodes_n declared earlier (banner uses it)
static void bus_nodetable_build(void) {       // from the commissioned roster, sorted by addr
    g_nodes_n = 0;
    for (uint8_t i = 0; i < g_roster_n && g_nodes_n < ROSTER_MAX; i++) {
        bus_node_t e = { .addr = g_roster[i].addr,
                         .enabled = (g_roster[i].flags & FLAG_ENABLED) ? 1u : 0u, .dead = 0, .miss = 0 };
        uint8_t j = g_nodes_n;               // insertion-sort by address
        while (j > 0 && g_nodes[j - 1].addr > e.addr) { g_nodes[j] = g_nodes[j - 1]; j--; }
        g_nodes[j] = e; g_nodes_n++;
    }
}

// --- §17 step 2: ping-pong list heads + producer attach + swap ----------------
// Producers attach to list[fill]; the cycle (step 3) drains list[active]; the cycle
// swaps the index. Head ops + swap use the pool hw spinlock (brief); the active-set
// drain is lock-free (no producer touches it after the swap).
static volatile uint8_t g_active_idx;   // 0/1; fill = active ^ 1

static int node_index_of(uint8_t addr) {
    for (uint8_t i = 0; i < g_nodes_n; i++) if (g_nodes[i].addr == addr) return (int)i;
    return -1;
}
// Producer API (host/zenoh, chain-tree): grab a buffer, fill it, attach to dest's fill list.
// false = unknown node or pool exhausted (the exhaustion exception/notify lands in step 5).
static bool bus_msg_send(uint8_t dest_addr, const uint8_t *payload, uint8_t len, uint32_t tag) {
    if (len > BUS_PAYLOAD_MAX) len = BUS_PAYLOAD_MAX;
    tx_buf_t *b = pool_grab();
    if (!b) {   // §17 step5: exhaustion is an exception -> COUNT ONLY (no silent drop).
        // The zenoh notify is DEFERRED to bus_run_cycle: a producer can be inside the
        // uplink thread's g_lock (on_local_shell -> MSG_INJECT), so taking g_lock here
        // would re-enter a non-recursive mutex and deadlock -> watchdog reboot.
        g_pool_exhausted++;
        return false;
    }
    b->len = len; b->origin_tag = tag;
    for (uint8_t i = 0; i < len; i++) b->payload[i] = payload[i];
    // node_index_of + the attach MUST be under the spinlock: a runtime roster rebuild
    // (bus_run_cycle, same g_pool_cs) can renumber g_nodes, so an index taken outside the
    // lock could be stale. If the node vanished (rebuild removed it), recover the buffer.
    critical_section_enter_blocking(&g_pool_cs);
    int idx = node_index_of(dest_addr);
    bool ok = (idx >= 0);
    if (ok) {
        b->dest_node = (uint8_t)idx;
        uint8_t f = g_active_idx ^ 1u;
        b->next = g_nodes[idx].list[f]; g_nodes[idx].list[f] = b;   // push onto the fill list
    }
    critical_section_exit(&g_pool_cs);
    if (!ok) { pool_recover(b); return false; }
    return true;
}
static void bus_pingpong_swap(void) {    // cycle boundary: flip active/fill
    critical_section_enter_blocking(&g_pool_cs);
    g_active_idx ^= 1u;
    critical_section_exit(&g_pool_cs);
}
// Step-2 self-test: attach K msgs/node to the fill set, swap, drain the active set,
// verify counts + that the pool round-trips (exercises grab->fill->attach->swap->drain).
static void bus_pp_selftest(void) {
    uint16_t before = g_pool_avail;
    const uint8_t K = 2;
    uint16_t attached = 0, drained = 0;
    for (uint8_t i = 0; i < g_nodes_n; i++)
        for (uint8_t k = 0; k < K; k++) {
            uint8_t pl[2] = { i, k };
            if (bus_msg_send(g_nodes[i].addr, pl, 2, 0xABCD0000u | ((uint32_t)i << 8) | k)) attached++;
        }
    bus_pingpong_swap();
    uint8_t a = g_active_idx;
    for (uint8_t i = 0; i < g_nodes_n; i++) {
        tx_buf_t *b = g_nodes[i].list[a]; g_nodes[i].list[a] = NULL;
        while (b) { tx_buf_t *nx = b->next; pool_recover(b); drained++; b = nx; }
    }
    g_pp_selftest = (attached == (uint16_t)(g_nodes_n * K) && drained == attached && g_pool_avail == before) ? 1u : 0u;
}

// --- §17 step 3: the cyclic engine -------------------------------------------
// One cycle: swap; MESSAGE pass (fire-and-forget the queued buffers, mark+recover);
// NO_MSG sweep (poll every unmarked live node, alarm-blocked feedback wait, §16);
// cycle-time stat. Runs in place of the per-slot rotation when g_cycle_mode is set.
static void bus_run_cycle(void) {
    // (-1) §17 fold: rebuild the node table if the roster changed at runtime
    // (CMD_BUS_REGISTER_SLAVE / CLEAR_ROSTER set g_nodes_dirty). Done here on the bus task
    // under g_pool_cs so it serializes with producers (bus_msg_send's node_index_of+attach)
    // and the roster write. Detach any queued buffers first (recover them -> no leak); old
    // correlations to removed nodes won't be answered, so drop them. Rare/administrative.
    if (g_nodes_dirty) {
        tx_buf_t *orphans = NULL;
        critical_section_enter_blocking(&g_pool_cs);
        g_nodes_dirty = false;
        for (uint8_t i = 0; i < g_nodes_n; i++)
            for (uint8_t bl = 0; bl < 2; bl++) {
                tx_buf_t *p = g_nodes[i].list[bl]; g_nodes[i].list[bl] = NULL;
                while (p) { tx_buf_t *nx = p->next; p->next = orphans; orphans = p; p = nx; }
            }
        bus_nodetable_build();                       // fresh g_nodes from g_roster (list[2]=NULL, dead=0)
        for (uint8_t i = 0; i < CMDCORR_MAX; i++) g_corr[i].used = 0u;   // drop stale correlations
        critical_section_exit(&g_pool_cs);
        while (orphans) { tx_buf_t *nx = orphans->next; pool_recover(orphans); orphans = nx; }
        g_nodes_rebuilds++;
    }
    // (0) §17 fold: drain a pending HOST->slave command into the producer path. on_bus_msg
    // (uplink, under g_lock) staged it in g_cmd_*; we fire it fire-and-forget like any
    // producer and record correlation so its reply is relayed to the host (handle_slot_frame),
    // not batched. This makes cycle mode serve host RPC -- the per-slot BS_CMD path is bypassed.
    LOCK();
    if (g_cmd_pending) {
        uint8_t fr[BUS_PAYLOAD_MAX]; uint8_t fn = 0;
        fr[fn++] = (uint8_t)(g_cmd_op & 0xFF); fr[fn++] = (uint8_t)(g_cmd_op >> 8);
        uint8_t m = g_cmd_len; if (m > (uint8_t)(BUS_PAYLOAD_MAX - fn)) m = (uint8_t)(BUS_PAYLOAD_MAX - fn);
        for (uint8_t i = 0; i < m; i++) fr[fn++] = g_cmd_body[i];
        uint8_t  cs = g_cmd_slave; uint16_t crq = g_cmd_req_id;
        g_cmd_pending = false;          // free the slot for the next host command (reply correlates by req_id)
        UNLOCK();
        corr_add(crq, cs, false, crq);
        (void)bus_msg_send(cs, fr, fn, 0xC1000000u | crq);
    } else {
        UNLOCK();
    }
    bus_pingpong_swap();
    uint32_t t0 = time_us_32();
    uint8_t a = g_active_idx;
    bool marked[ROSTER_MAX];
    for (uint8_t i = 0; i < g_nodes_n; i++) marked[i] = false;
    bus_frame_t rf;
    g_fb_n = 1; g_fb_rec = 0; g_cycle_collecting = true;   // §17 step8: gather this cycle's feedback
    // (1) message pass -- fire-and-forget the queued buffers to their nodes.
    // Grab each node's list head UNDER THE SPINLOCK (a producer can still be attaching
    // to this set in the swap window -- both run time-sliced on core0); once grabbed +
    // NULLed, the chain is private and processed lock-free.
    for (uint8_t i = 0; i < g_nodes_n; i++) {
        critical_section_enter_blocking(&g_pool_cs);
        tx_buf_t *b = g_nodes[i].list[a]; g_nodes[i].list[a] = NULL;
        critical_section_exit(&g_pool_cs);
        while (b) {
            tx_buf_t *nx = b->next;
            if (!g_nodes[i].dead) { bus_send(g_nodes[i].addr, BUS_FT_DATA, b->payload, b->len); marked[i] = true; }
            pool_recover(b); b = nx;          // recover regardless (no reply -- §17 fire-and-forget)
        }
    }
    // (2) NO_MSG sweep -- poll each unmarked live node for feedback + liveness
    uint32_t slow_us = 0; uint8_t slow_addr = 0;
    for (uint8_t i = 0; i < g_nodes_n; i++) {
        if (g_nodes[i].dead || marked[i]) continue;
        uint32_t st = time_us_32();
        slot_result_t sr = bus_poll_slot(g_nodes[i].addr, &rf);   // alarm-blocked feedback wait
        uint32_t sl = time_us_32() - st;
        if (sl > slow_us) { slow_us = sl; slow_addr = g_nodes[i].addr; }
        uint32_t now = to_ms_since_boot(get_absolute_time());
        LOCK();
        g_poll_total++;
        if (sr == SLOT_NO_RESPONSE) {
            g_poll_miss++; mark_miss(g_nodes[i].addr);
            if (g_nodes[i].miss < 0xFFu && ++g_nodes[i].miss >= g_poll_max_misses) g_nodes[i].dead = 1;
        } else {
            g_poll_alive++; mark_alive(g_nodes[i].addr, now); g_nodes[i].miss = 0; g_meas_ta_us = g_last_ta_us;
        }
        UNLOCK();
    }
    // (2a) §17 step7: DEAD-NODE slow-poll. Dead nodes are skipped above (never sent
    // messages, not swept) so the live cycle stays fast. Re-probe ONE dead node every
    // g_dead_slowpoll_div cycles (round-robin) so a recovered node rejoins on its own.
    // A no-response probe costs T_RESP (~3ms) -> that ONE cycle overruns (expected for a
    // slow-poll; the overrun stat flags it); the other div-1 cycles are unaffected.
    if (g_dead_slowpoll_div && ++g_dead_tick >= g_dead_slowpoll_div) {
        g_dead_tick = 0;
        for (uint8_t k = 0; k < g_nodes_n; k++) {
            uint8_t i = (uint8_t)((g_dead_cursor + k) % g_nodes_n);
            if (!g_nodes[i].dead) continue;
            g_dead_cursor = (uint8_t)((i + 1) % g_nodes_n);
            slot_result_t sr = bus_poll_slot(g_nodes[i].addr, &rf);
            uint32_t now = to_ms_since_boot(get_absolute_time());
            LOCK();
            g_poll_total++;
            if (sr != SLOT_NO_RESPONSE) {   // resurrected -> back into the live rotation
                g_nodes[i].dead = 0; g_nodes[i].miss = 0;
                g_poll_alive++; mark_alive(g_nodes[i].addr, now); g_dead_revives++;
            } else { g_poll_miss++; }
            UNLOCK();
            break;                          // at most one dead probe per interval
        }
    }
    { uint8_t nd = 0; for (uint8_t i = 0; i < g_nodes_n; i++) if (g_nodes[i].dead) nd++; g_nodes_dead = nd; g_il_nodes_dead = nd; }  // #1: feed the dead-node->veto virtual
    // (2b) deferred pool-exhaustion notify (§17 step5): producers only bump the
    // counter -- emit the rate-limited zenoh notify HERE, where no outer g_lock is
    // held, so we never re-enter the uplink thread's mutex (see bus_msg_send).
    {
        static uint32_t s_seen_ex, s_last_ex_ms;
        uint32_t ex = g_pool_exhausted;
        if (g_uplink_active && ex != s_seen_ex) {   // §dual-transport: no uplink -> don't emit (counter still tracks)
            uint32_t nms = to_ms_since_boot(get_absolute_time());
            if ((uint32_t)(nms - s_last_ex_ms) >= 100u) {
                s_last_ex_ms = nms; s_seen_ex = ex;
                char m[48];
                int mn = snprintf(m, sizeof m, "[pool-exhausted] n=%u low=%u", (unsigned)ex, (unsigned)g_pool_low);
                if (mn > 0) { if (mn >= (int)sizeof m) mn = (int)sizeof m - 1;
                              LOCK(); (void)host_link_s2m(&g_hl, 1, OP_DBG_LOG, (const uint8_t *)m, (uint8_t)mn); UNLOCK(); }
            }
        }
    }
    // (3) cycle-time stat + real-time watchdog (overrun / slack / slowest node)
    uint32_t ct = time_us_32() - t0;
    g_cycle_last = ct;
    if (ct > g_cycle_max) g_cycle_max = ct;
    if (g_cycle_min == 0 || ct < g_cycle_min) g_cycle_min = ct;
    g_cycle_slow_addr = slow_addr; g_cycle_slow_us = slow_us;
    int32_t slack = (int32_t)g_cycle_deadline_us - (int32_t)ct;
    if (slack < g_cycle_minslack) g_cycle_minslack = slack;
    if (ct > g_cycle_deadline_us) {
        g_cycle_overruns++;                         // stat counts even in standalone; only the notify is gated
        static uint32_t s_last_notify_ms;          // rate-limit the push to <=10/s
        uint32_t nms = to_ms_since_boot(get_absolute_time());
        if (g_uplink_active && (uint32_t)(nms - s_last_notify_ms) >= 100u) {   // §dual-transport: no uplink -> skip
            s_last_notify_ms = nms;
            char m[64];
            int mn = snprintf(m, sizeof m, "[overrun] cycle=%uus deadline=%uus slow=0x%02X/%uus",
                              (unsigned)ct, (unsigned)g_cycle_deadline_us, slow_addr, (unsigned)slow_us);
            if (mn > 0) {
                if (mn >= (int)sizeof m) mn = (int)sizeof m - 1;
                LOCK(); (void)host_link_s2m(&g_hl, 1, OP_DBG_LOG, (const uint8_t *)m, (uint8_t)mn); UNLOCK();
            }
        }
    }
    // (4) §17 step6: drive the chain-tree ENGINE producer in cycle mode. The per-slot
    // COLLECT re-arm doesn't run here, so re-arm pp from the cycle -- but keep exactly
    // ONE origination outstanding (g_pp_kicks==g_pp_master => core1 has consumed the
    // last) so the rate is engine-tick paced (~100/s) and the slave's reply queue can't
    // pile up. Self-correcting if a kick is dropped (g_pp_kicks only counts queued ones).
    if (g_pp_run && g_pp_kicks == g_pp_master) pp_kick();

    // (4b) §17 step8: emit this cycle's BATCHED feedback as ONE OP_BUS_FEEDBACK frame
    // (the agent PUBLISHes it to the feedback key). Only when there is feedback -> no
    // empty-frame spam; rate is naturally cycle-paced. handle_slot_frame appended the
    // records while g_cycle_collecting was set; close the window before emitting.
    g_cycle_collecting = false;
    // §dual-transport: emit only when an uplink is active (USB/WiFi). In standalone the batch is
    // collected (so handle_slot_frame keeps batching, not relaying) but DROPPED here -> no traffic
    // generated for an absent host. (standalone_pump is the pump-side safety-net sink.)
    if (g_uplink_active && g_fb_rec > 0) {
        g_fb_buf[0] = g_fb_rec;
        LOCK(); (void)host_link_s2m(&g_hl, 1, OP_BUS_FEEDBACK, g_fb_buf, g_fb_n); UNLOCK();
        g_fb_frames++;
    }

    // (5) §17 step7: FIXED-RATE PACING. Hold each cycle to g_cycle_pace_us (0=free-run)
    // measured from this cycle's start (t0) so the control loop sees a steady period with
    // low jitter. If work finished early, sleep the remainder on the SAME idle alarm the
    // slot uses (rot_alarm_cb wakes us) -- core0 stays free, no busy-wait. No slot is
    // active here so the RX ISR won't touch the alarm. Overran (work>=pace) -> no wait
    // (already counted as an overrun above); the period naturally re-anchors next cycle.
    if (g_cycle_pace_us) {
        uint32_t elapsed = time_us_32() - t0;
        if (elapsed < g_cycle_pace_us) {
            uint32_t rem = g_cycle_pace_us - elapsed;
            (void)ulTaskNotifyTake(pdTRUE, 0);                 // clear any stale notify
            hardware_alarm_set_target(g_gap_alarm, make_timeout_time_us(rem));
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(rem / 1000u + 4u));   // wake on alarm (+safety)
            hardware_alarm_cancel(g_gap_alarm);
        }
    }
}

static uint16_t              g_kb0_node;   // KB0 start node (event injection target)
static uint16_t              g_kb1_node;   // KB1 (api) start node
static uint16_t              g_kbapp_node; // kbapp (Thread 3 application) start node

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
    // Drain whatever the FIFO holds FIRST, so the decimation tiers keep advancing
    // even while recovering from an overrun. (The old code early-returned on OVER
    // WITHOUT draining and did an adc_run stop/start: on the busier slave core the
    // FIFO can outrun the ISR, and once OVER latched, every IRQ just resynced and
    // bailed — the FIFO never drained and the ADC froze forever. Drain-then-clear
    // recovers in place with no stop/start churn and can never freeze.)
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
    // Overrun/underrun are sticky (W1C). If the ISR ran late the FIFO overflowed:
    // samples were dropped so the round-robin phase has slipped. Clear the flags and
    // realign to channel 0 AFTER draining, so the decimator already advanced this IRQ.
    if (adc_hw->fcs & (ADC_FCS_OVER_BITS | ADC_FCS_UNDER_BITS)) {
        hw_set_bits(&adc_hw->fcs, ADC_FCS_OVER_BITS | ADC_FCS_UNDER_BITS);
        for (uint8_t i = 0; i < ADC_NCH; i++) { adc_acc[i] = 0; adc_acc_n[i] = 0; }
        adc_phase = 0; adc_select_input(0); g_adc_resyncs++;
    }
}

// ADC config (peripheral + FIFO + handler registration). Global state — safe from
// any core, pre- or post-scheduler. Does NOT enable the NVIC line or start
// conversions: that is adc_service_start_on_core(), which MUST run on the core that
// will service the ISR (irq_set_enabled is per-core). Splitting the two lets the
// slave configure pre-scheduler on core0 but service the ISR on core1 (the master
// already runs both on core1). Running conversions on the busier core0 let the
// 8-deep FIFO outrun the ISR -> stuck OVER flag -> frozen ADC.
static void adc_service_config(void) {
    adc_init();
    for (uint8_t i = 0; i < ADC_NCH; i++) adc_gpio_init(ADC0_GPIO + i);
    adc_set_round_robin((1u << ADC_NCH) - 1);          // channels 0..ADC_NCH-1
    adc_select_input(0);                               // phase 0 == channel 0
    adc_phase = 0;
    adc_fifo_setup(true, false, ADC_FIFO_THRESH, false, false); // en, no DREQ, IRQ@thresh
    adc_set_clkdiv(ADC_CLKDIV);
    irq_set_exclusive_handler(ADC_IRQ_FIFO, adc_fifo_isr);
    adc_irq_set_enabled(true);
}

// Enable the ADC FIFO IRQ on THIS core's NVIC and start free-running conversions.
// Call from the core that will own the ISR (core1). adc_run last so the FIFO can't
// overflow before the ISR is live.
static void adc_service_start_on_core(void) {
    irq_set_enabled(ADC_IRQ_FIFO, true);
    adc_run(true);
}

// Master: config + start both on core1 (app_engine_task). Kept as one call.
static void adc_service_init(void) {
    adc_service_config();
    adc_service_start_on_core();
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

// kbapp (Thread 3 application): CMD_APP_ECHO. The engine event is a variant (int or
// pointer); an app command's payload rides as a pointer to one of these slots. We own
// the storage (a small ring, malloc_flag=false) — ChainTree's engine-frees-the-message
// path is the Linux build's, not wired here. pre_tick fills a slot and injects a
// LOW-priority pointer event the kbapp WAIT leaf is parked on; the SAME engine cycle
// drains it on single-threaded core1, so a few slots cover several queued commands.
#define APP_REQ_SLOTS 4
#define APP_REQ_MAX   APPCORE_ARGS_MAX
typedef struct { uint16_t req_id; uint8_t addr; uint8_t route; uint8_t bus_src; uint8_t len; uint8_t bytes[APP_REQ_MAX]; } app_req_t;
static app_req_t g_app_req[APP_REQ_SLOTS];
static uint8_t   g_app_req_head;

// Route a kbapp reply by the request's tag: the reply always rides g_up_q, and the
// role-specific drain delivers it (master uplink -> USB; slave pump -> bus window).
// `body` is the OP_SHELL_REPLY payload (typically [req][status]...). dest = appcore on
// the USB route, or the asking node on the bus route.
static void kbapp_reply(const app_req_t *q, const uint8_t *body, uint8_t blen) {
    appcore_rep_t r;
    r.dest = (q->route == ROUTE_BUS) ? q->bus_src : BUS_ADDR_APPCORE;
    r.opcode = OP_SHELL_REPLY;
    if (blen > APPCORE_PAY_MAX) blen = APPCORE_PAY_MAX;
    memcpy(r.payload, body, blen); r.len = blen;
    (void)xQueueSend(g_up_q, &r, 0);
}

// Echo, routed by the request's tag.
//   USB route: OP_SHELL_REPLY [req][status][ver][echo] (C1 format).
//   BUS route: OP_SHELL_REPLY [req][status][echo] (the on-wire echo contract the
//              master correlates in COLLECT — no ver byte).
void kbapp_on_echo(void *handle, unsigned node_index) {
    (void)node_index;
    cfl_runtime_handle_t *rt = (cfl_runtime_handle_t *)handle;
    const app_req_t *q = (const app_req_t *)rt->event_data_ptr->data.ptr;
    if (!q) return;
    bool to_bus = (q->route == ROUTE_BUS);
    uint8_t p[APPCORE_PAY_MAX]; uint8_t n = 0;
    p[n++] = (uint8_t)q->req_id; p[n++] = (uint8_t)(q->req_id >> 8);
    p[n++] = SHELL_OK;
    if (!to_bus) p[n++] = KBAPP_VER;          // ver only on the local (USB) echo
    uint8_t m = q->len;
    uint8_t cap = to_bus ? (uint8_t)(APPCORE_PAY_MAX - 4) : APPCORE_PAY_MAX; // leave room for the pp count
    if (m > (uint8_t)(cap - n)) m = (uint8_t)(cap - n);
    for (uint8_t i = 0; i < m; i++) p[n++] = q->bytes[i];
    if (to_bus) {
        // Ping-pong: this node is the answering SLAVE — its kbapp engine counts the
        // round and piggybacks the count (u32 LE) so the master captures it in COLLECT.
        uint32_t c = ++g_pp_slave;
        p[n++] = (uint8_t)c; p[n++] = (uint8_t)(c >> 8);
        p[n++] = (uint8_t)(c >> 16); p[n++] = (uint8_t)(c >> 24);
    }
    kbapp_reply(q, p, n);
}

// Thread 3 -> Thread 2: the engine clears the interlock's latched trips. Fail-safe —
// a still-violated slot re-latches on the next il tick. Reply: [req][status][1=cleared].
void kbapp_on_il_clear(void *handle, unsigned node_index) {
    (void)node_index;
    cfl_runtime_handle_t *rt = (cfl_runtime_handle_t *)handle;
    const app_req_t *q = (const app_req_t *)rt->event_data_ptr->data.ptr;
    if (!q) return;
    interlock_request_global_clear();
    uint8_t p[4]; uint8_t n = 0;
    p[n++] = (uint8_t)q->req_id; p[n++] = (uint8_t)(q->req_id >> 8);
    p[n++] = SHELL_OK; p[n++] = 1u;           // 1 = global clear requested
    kbapp_reply(q, p, n);
}

// kbapp (C2): CMD_APP_ECHO_TO — the engine ORIGINATES a bus echo to a slave node.
// The bus round-trip can't block the engine tick, so just hand the request to the
// bus thread (core0) and return; that thread does the transaction and correlates the
// reply back to this host req_id. No reply is pushed here — it rides the round-trip.
void kbapp_on_echo_to(void *handle, unsigned node_index) {
    (void)node_index;
    cfl_runtime_handle_t *rt = (cfl_runtime_handle_t *)handle;
    const app_req_t *q = (const app_req_t *)rt->event_data_ptr->data.ptr;
    if (!q) return;
    g_pp_master++;   // ping-pong: master kbapp engine counts each round it originates
    uint8_t m = q->len; if (m > ORIG_PAY_MAX) m = ORIG_PAY_MAX;

    // §17 step6: in CYCLE MODE the engine (core1) is a DIRECT producer into the
    // ping-pong lists -- build the full OP_SHELL_EXEC frame and bus_msg_send() it
    // (grab->fill->attach under the hw spinlock, the SAME path the host/uplink
    // producer on core0 uses). This is the genuine cross-core concurrent attach the
    // hw spinlock exists for (a FreeRTOS mutex couldn't guard a core1 producer vs the
    // core0 cycle drain). The §17 FOLD adds reply correlation: record {sr -> host_req}
    // so handle_slot_frame relays the slave's reply back to this host req_id (re-tagged)
    // instead of batching it -- chain-flow over the cycle now round-trips like per-slot.
    // Falls back to the per-slot g_orig_q path when not in cycle mode.
    if (g_cycle_mode) {
        uint16_t sr = (uint16_t)(++g_orig_seq); if (sr == 0) sr = (uint16_t)(++g_orig_seq);
        uint8_t pb[BUS_PAYLOAD_MAX]; uint8_t pn = 0;
        pb[pn++] = (uint8_t)(OP_SHELL_EXEC & 0xFF); pb[pn++] = (uint8_t)(OP_SHELL_EXEC >> 8);
        pb[pn++] = (uint8_t)sr;                     pb[pn++] = (uint8_t)(sr >> 8);
        pb[pn++] = (uint8_t)(CMD_APP_ECHO & 0xFF);  pb[pn++] = (uint8_t)(CMD_APP_ECHO >> 8);
        if (m > (uint8_t)(BUS_PAYLOAD_MAX - pn)) m = (uint8_t)(BUS_PAYLOAD_MAX - pn);
        for (uint8_t i = 0; i < m; i++) pb[pn++] = q->bytes[i];
        corr_add(sr, q->addr, true, q->req_id);                  // correlate the reply (relay/pp-piggyback)
        (void)bus_msg_send(q->addr, pb, pn, 0xC2000000u | sr);   // core1 producer
        return;
    }

    orig_req_t o; o.addr = q->addr; o.host_req_id = q->req_id;
    o.len = m;
    for (uint8_t i = 0; i < m; i++) o.bytes[i] = q->bytes[i];
    (void)xQueueSend(g_orig_q, &o, 0);   // non-blocking; drop if the bus is backed up
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
    // CMD_GPIO_WRITE / CMD_GPIO_READ are handled earlier by the unified
    // node_cmd_dispatch (shared with the slave) — not here.
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
        } else if (c.cmd == CMD_APP_ECHO) {      // Thread 3 (kbapp): route to the app KB
            // The event carries a variant (int OR pointer). Echo needs the payload
            // bytes, so stash them in a slot and inject a pointer event. App traffic
            // doesn't need to jump FIFO ordering -> LOW priority. malloc_flag=false:
            // we own the storage (this build doesn't free pointer events post-dispatch).
            app_req_t *q = &g_app_req[g_app_req_head];
            g_app_req_head = (uint8_t)((g_app_req_head + 1u) % APP_REQ_SLOTS);
            q->req_id = c.req_id; q->addr = 0; q->route = c.route; q->bus_src = c.bus_src;
            uint8_t m = c.alen; if (m > APP_REQ_MAX) m = APP_REQ_MAX;
            q->len = m;
            for (uint8_t i = 0; i < m; i++) q->bytes[i] = c.args[i];
            cfl_send_data_event(g_rt->event_queue, CFL_EVENT_PRIORITY_LOW,
                                g_kbapp_node, false, EVENT_CMD_APP_ECHO, q);
        } else if (c.cmd == CMD_APP_ECHO_TO) {   // C2: engine originates a bus echo to [addr]
            // args = [addr u8][payload...]; same slot+pointer-event mechanism as echo,
            // but the kbapp handler hands it to the bus thread instead of replying.
            app_req_t *q = &g_app_req[g_app_req_head];
            g_app_req_head = (uint8_t)((g_app_req_head + 1u) % APP_REQ_SLOTS);
            q->req_id = c.req_id; q->route = c.route; q->bus_src = c.bus_src;
            q->addr = (c.alen >= 1) ? c.args[0] : 0;
            uint8_t m = (c.alen >= 1) ? (uint8_t)(c.alen - 1) : 0;
            if (m > APP_REQ_MAX) m = APP_REQ_MAX;
            q->len = m;
            for (uint8_t i = 0; i < m; i++) q->bytes[i] = c.args[i + 1];
            cfl_send_data_event(g_rt->event_queue, CFL_EVENT_PRIORITY_LOW,
                                g_kbapp_node, false, EVENT_CMD_APP_ECHO_TO, q);
        } else if (c.cmd == CMD_APP_IL_CLEAR) {  // Thread 3 -> Thread 2: engine-driven interlock clear
            app_req_t *q = &g_app_req[g_app_req_head];
            g_app_req_head = (uint8_t)((g_app_req_head + 1u) % APP_REQ_SLOTS);
            q->req_id = c.req_id; q->route = c.route; q->bus_src = c.bus_src; q->addr = 0; q->len = 0;
            cfl_send_data_event(g_rt->event_queue, CFL_EVENT_PRIORITY_LOW,
                                g_kbapp_node, false, EVENT_CMD_APP_IL_CLEAR, q);
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
        } else {
            // Thread-1 unified operate dispatch (echo / GPIO / interlock), shared
            // with the slave responder. Then the master's own extras (servo / pulse
            // / I2C via hil_gpio_dispatch), else UNKNOWN.
            uint8_t out[APPCORE_PAY_MAX], outlen;
            uint8_t st = node_cmd_dispatch(c.cmd, c.args, c.alen, out, sizeof out, &outlen);
            if (st != CMD_NOT_MINE) {
                hil_reply(c.req_id, st, out, outlen);
            } else if (hil_gpio_dispatch(&c)) {
                // servo / pulse / I2C / (frozen) config handled inline (reply sent within)
            } else {   // unknown command -> direct UNKNOWN_CMD reply (no chain handler)
                appcore_rep_t r; r.dest = BUS_ADDR_APPCORE; r.opcode = OP_SHELL_REPLY;
                r.payload[0] = (uint8_t)c.req_id; r.payload[1] = (uint8_t)(c.req_id >> 8);
                r.payload[2] = SHELL_UNKNOWN_CMD; r.len = 3;
                (void)xQueueSend(g_up_q, &r, 0);
            }
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
// NOTE: (a) the il task ticks every ~2 ms at priority 4 (above the engine), reading
// the 1 kHz-fresh g_adc_latest — ~2 ms veto response with the FULL DNF/latch/clear
// eval. A sub-ms ISR-embedded fast-veto can be added on top later if a specific
// hard requirement needs it (assert-only in the ISR, tick still owns clear).
// (b) ideal core affinity isolates interlock + ADC ISR on their own core (engine
// moved off) — deferred to the thread-review.

// Platform externs the framework's VIRTUAL inputs read. board_millis() is in
// board.c. These two back _stack_hwm / _t_since_m2s; fed minimally for now (only
// matter when a DSL references those virtuals — wire real sources when used).
volatile uint16_t g_stack_hwm_bytes = 0;
volatile uint32_t g_last_m2s_rx_ms  = 0;

// ---- il_hal platform seam — bind the HAL to the frozen hwio roles + shared ADC.
// Pins the interlock may bind: GP0 (veto output), GP1 (dedicated interlock input),
// GP2..GP9 (HIL block, per hwio role), GP26/27/28 (ADC). Everything else is
// fixed-function → reserved.
bool il_plat_pin_reserved(uint8_t gpio) {
    if (gpio == INTERLOCK_VETO_PIN) return false;                            // GP0 veto
    if (gpio == INTERLOCK_IN_PIN) return false;                              // GP1 interlock input
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
    // input modes: the dedicated GP1 interlock input, or an hwio input-roled HIL pin
    // (the interlock reads them; it never reconfigures an hwio-owned pin).
    if (gpio == INTERLOCK_IN_PIN) return true;                               // GP1 dedicated interlock input
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
        vTaskDelay(pdMS_TO_TICKS(2));    // fast tick: ~2 ms veto response (reads the
                                         // 1 kHz-fresh g_adc_latest); full DNF/latch/clear eval
    }
}

// FNV-1a over the flashed ilc0..ilc9 config (each present file, position-tagged) —
// the fingerprint used to detect a config change across a warm reset.
static uint32_t interlock_cfg_fingerprint(void) {
    uint32_t fp = 2166136261u;
    // Slot 0 is the built-in GP1 safety input (not config-derived); config = ilc1..ilc9.
    for (uint8_t slot = 1; slot < INTERLOCK_MAX_SLOTS; slot++) {
        const char name[CFG_NAME_LEN] = { 'i', 'l', 'c', (char)('0' + slot) };
        uint8_t buf[CFG_FILE_MAX]; uint32_t len;
        if (cfg_load(name, buf, sizeof buf, &len) != 0 || len == 0) continue;
        fp = (fp ^ (uint32_t)(slot + 1)) * 16777619u;                 // position tag
        for (uint32_t i = 0; i < len; i++) fp = (fp ^ buf[i]) * 16777619u;
    }
    return fp;
}

// Run from app_engine_task / node_role_run AFTER hwio_apply (peripherals up).
// Warm-restores the persisted armed set, then RE-ARMS from the flashed ilc0..ilc9
// config when that config changed (fingerprint mismatch) or on a cold boot — so a
// reflashed interlock config takes effect WITHOUT a power cycle, while an unchanged
// config + warm reset preserves the armed/latched safety state. Spawns the tick task.
static void interlock_thread2_start(void) {
    // GP1 dedicated interlock input HW: input + internal pull-up (safe = HIGH). The
    // il HAL also claims it when slot 0 arms (cfg gp1:in,up); this is belt-and-suspenders
    // so the line is held high the instant peripherals come up, before the first tick.
    gpio_init(INTERLOCK_IN_PIN);
    gpio_set_dir(INTERLOCK_IN_PIN, GPIO_IN);
    gpio_pull_up(INTERLOCK_IN_PIN);

    interlock_warm_restore();
    uint32_t fp = interlock_cfg_fingerprint();
    if (fp != g_interlock_persist.cfg_fingerprint || interlock_armed_count() == 0) {
        for (uint8_t slot = 1; slot < INTERLOCK_MAX_SLOTS; slot++)
            interlock_disarm_slot(slot);                     // drop stale CONFIG slots; keep slot 0 (built-in)
        uint8_t armed = 0;
        for (uint8_t slot = 1; slot < INTERLOCK_MAX_SLOTS; slot++) {   // slot 0 = built-in; config = ilc1..ilc9
            const char name[CFG_NAME_LEN] = { 'i', 'l', 'c', (char)('0' + slot) };
            uint8_t buf[CFG_FILE_MAX]; uint32_t len;
            if (cfg_load(name, buf, sizeof buf, &len) != 0 || len == 0) continue;  // absent → empty
            uint8_t err[3];
            uint8_t st = interlock_set_slot_dsl(slot, (const char *)buf, (uint16_t)len, err);
            if (st == SHELL_OK) armed++;
            else if (!g_il_armfail)                          // DIAG: capture first arm failure
                g_il_armfail = ((uint32_t)slot << 24) | ((uint32_t)st << 16) |
                               ((uint32_t)err[0] << 8) | err[1];
        }
        g_interlock_persist.cfg_fingerprint = fp;
        g_ilcf_rc = armed;                                   // 0..9 config slots armed (slot 0 = built-in, below)
    } else {
        g_ilcf_rc = (int)interlock_armed_count();            // warm, same config → preserved
    }
    // Built-in GP1 safety interlock — slot 0, via the COMMON engine. The pull-up holds
    // gp1 HIGH = OK; a device pulling gp1 LOW fails the watch -> trip. GP0 is an OPEN-DRAIN
    // (oc) wired-OR veto: OK -> released (hi-Z, an external pull-up holds the shared line
    // HIGH), trip -> driven LOW. Many nodes share the line; any one trips -> line low.
    // (Needs an external pull-up on the GP0 line; GP1 has the internal one.)
    //
    // Arm it ONLY if a warm restore didn't already preserve the SAME built-in DSL, so a
    // latched trip SURVIVES a warm (watchdog/glitch) reset. A cold boot (slots wiped) or a
    // changed built-in DSL re-arms fresh (which intentionally drops the old latch).
    {
        // watch[gp1:1] AND watch[_nodesdead:0]: OK requires GP1 high AND zero dead bus
        // nodes (#1). Either a GP1 trip OR a node going dead drives the GP0 wired-OR veto.
        // On a slave _nodesdead is always 0, so the clause is a no-op there.
        static const char gp1_il[] =
            "gp1il;cfg[(gp1):in,up,debounce_4,(gp0):oc,up];watch[gp1:1];watch[_nodesdead:0];out_ok[gp0:1];out_err[gp0:0]";
        const uint16_t gl = (uint16_t)(sizeof gp1_il - 1u);
        bool preserved = (g_interlock_persist.slots[0].state == INTERLOCK_SLOT_ARMED) &&
                         (g_interlock_persist.dsl_len[0] == gl) &&
                         (memcmp(g_interlock_persist.dsl_text[0], gp1_il, gl) == 0);
        if (!preserved) {
            interlock_disarm_slot(0);    // a warm_restore may hold the OLD built-in here; drop it so the
                                         // re-arm to the current DSL takes (set_slot_dsl rejects an ARMED slot)
            uint8_t err[3];
            uint8_t st = interlock_set_slot_dsl(0, gp1_il, gl, err);
            if (st != SHELL_OK && !g_il_armfail)             // DIAG: capture a slot-0 arm failure
                g_il_armfail = ((uint32_t)st << 16) | ((uint32_t)err[0] << 8) | err[1];
        }
    }
    // Wired-OR boot grace: the shared GP0 veto line needs a moment to settle HIGH on
    // the pull-ups as all nodes come up; suppress new latches during it so a startup
    // transient doesn't false-latch the chain. (Warm-restored real latches still drive.)
    interlock_begin_grace();
    // Priority 4 — ABOVE the chain-tree engine (prio 3): safety preempts the
    // application, so engine load can't delay the veto. core1, with the ADC ISR.
    xTaskCreate(il_tick_task, "il", configMINIMAL_STACK_SIZE * 4, NULL, 4, &t_il);
    vTaskCoreAffinitySet(t_il, 1u << 1);
}

// ---- Role-agnostic Thread-2 entry points (used by the SLAVE node responder) ----
// The slave path (node_role_run) is a stub responder today; these let it run the
// SAME interlock + a minimal HIL surface so the trip/latch/clear behaviour can be
// exercised on the slave over the bus. Master keeps using app_engine_task's calls.

// Bring up Thread 2 on the slave: validate persist, start the ADC service, apply
// the frozen hwio roles, arm ilc0..ilc9, and start the tick task. Runs pre-scheduler
// from node_role_run (core0), so the ADC FIFO ISR lives on core0 here (vs core1 on
// the master); the interlock reads g_adc_latest lock-free cross-core, so ADC-watching
// interlocks now work on the slave too.
void node_thread2_start(void) {
    interlock_boot_decide();
    adc_service_config();       // config only (core0, pre-scheduler); the ISR is
                                // enabled + conversions started on core1 in
                                // node_engine_task, so the FIFO can't outrun it.
    hwio_apply(&g_hwio);
    interlock_thread2_start();
}

// Pack interlock status into out (<= cap bytes). Wire format v1:
//   [ver=1][gveto] [n] then n x [slot][state][tf(live)][latched]
// Only non-empty slots are reported (mostly-empty model).
#define IL_STATUS_WIRE_VER 1u
static uint8_t il_status_pack(uint8_t *out, uint8_t cap) {
    if (cap < 3) return 0;
    uint8_t gveto = 0;
    for (uint8_t s = 0; s < INTERLOCK_MAX_SLOTS; s++)
        if (g_interlock_persist.slots[s].state == INTERLOCK_SLOT_ARMED &&
            g_interlock_persist.slots[s].latched) gveto = 1;
    uint8_t n = 0;
    out[n++] = IL_STATUS_WIRE_VER;
    out[n++] = gveto;
    uint8_t cnt_at = n++, cnt = 0;
    for (uint8_t s = 0; s < INTERLOCK_MAX_SLOTS && (uint8_t)(n + 4) <= cap; s++) {
        if (g_interlock_persist.slots[s].state == INTERLOCK_SLOT_EMPTY) continue;
        out[n++] = s;
        out[n++] = g_interlock_persist.slots[s].state;
        out[n++] = g_interlock_persist.inst[s].tf_state;   // live boolean
        out[n++] = g_interlock_persist.slots[s].latched;   // sticky trip
        cnt++;
    }
    out[cnt_at] = cnt;
    if ((uint8_t)(n + 2) <= cap) {                                  // DIAG: actual pin levels
        out[n++] = (uint8_t)gpio_get(INTERLOCK_VETO_PIN);          // GP0 (veto out) level
        out[n++] = (uint8_t)gpio_get(INTERLOCK_IN_PIN);           // GP1 (shared line) level
    }
    return n;
}

// ---- Thread 1: unified operate-command dispatch (B1) ------------------------
// ONE origin-agnostic handler for the synchronous operate commands common to the
// master and the slave: echo, role-validated GPIO read/write, interlock clear/
// status. Fills out (<= cap) + *outlen with reply data and returns a SHELL_* status,
// or CMD_NOT_MINE if cmd isn't one of these — the caller then handles its own extras
// (engine-routed MON/ADC + async servo/I2C on the master, UNKNOWN on the slave). The
// caller owns the reply transport (USB up-queue vs bus window), so the same dispatch
// serves both roles. Replaces the duplicated node_hil_gpio + per-side handlers.
#define NODE_CMD_ECHO  0x0001u   // host/peer echo (now handled on both roles)

// GP22 PWM test source. A settable-frequency/duty square wave for bench testing —
// loop it into an ADC pin (raw for AC/known-freq, or via an RC for a DC level) or a
// counter/edge input. freq_hz==0 or duty==0 releases the pin to hi-Z (off). The clk
// divider is chosen so TOP fits 16 bits across the chip's sys clock (125/150 MHz).
static void pwm_test_set(uint32_t freq_hz, uint8_t duty_pct) {
    const uint slice = pwm_gpio_to_slice_num(PWM_TEST_PIN);
    const uint chan  = pwm_gpio_to_channel(PWM_TEST_PIN);
    if (freq_hz == 0u || duty_pct == 0u) {                 // OFF -> hi-Z (no drive)
        pwm_set_enabled(slice, false);
        gpio_set_function(PWM_TEST_PIN, GPIO_FUNC_SIO);
        gpio_set_dir(PWM_TEST_PIN, false);
        return;
    }
    if (duty_pct > 100u) duty_pct = 100u;
    uint32_t fsys = clock_get_hz(clk_sys);
    uint32_t div  = (fsys / (freq_hz * 65536u)) + 1u;      // integer clkdiv so TOP <= 65535
    if (div > 255u) div = 255u;
    uint32_t top  = fsys / (div * freq_hz);                // counts per period
    if (top < 2u)     top = 2u;
    if (top > 65536u) top = 65536u;
    uint16_t wrap = (uint16_t)(top - 1u);
    uint16_t level = (uint16_t)(((uint32_t)top * duty_pct) / 100u);
    gpio_set_function(PWM_TEST_PIN, GPIO_FUNC_PWM);
    pwm_set_clkdiv_int_frac(slice, (uint8_t)div, 0);
    pwm_set_wrap(slice, wrap);
    pwm_set_chan_level(slice, chan, level);
    pwm_set_enabled(slice, true);
}

// (CMD_NOT_MINE sentinel is defined in node_role.h — shared with the slave.)
uint8_t node_cmd_dispatch(uint16_t cmd, const uint8_t *args, uint8_t alen,
                          uint8_t *out, uint8_t cap, uint8_t *outlen) {
    *outlen = 0;
    switch (cmd) {
    case NODE_CMD_ECHO: {
        uint8_t k = (alen > cap) ? cap : alen;
        memcpy(out, args, k); *outlen = k; return SHELL_OK;
    }
    case CMD_GPIO_WRITE: {
        if (alen < 3) return SHELL_BAD_ARGS;
        uint8_t port = args[0], pin = args[1], level = args[2];
        if (port != 0 || level > 1 || hil_role_of(pin) != HWIO_ROLE_OUTPUT) return SHELL_BAD_ARGS;
        gpio_put(pin, level); return SHELL_OK;
    }
    case CMD_GPIO_READ: {
        if (alen < 2 || cap < 1) return SHELL_BAD_ARGS;
        uint8_t port = args[0], pin = args[1], role = hil_role_of(pin);
        bool readable = (role == HWIO_ROLE_INPUT || role == HWIO_ROLE_INPUT_PULLUP ||
                         role == HWIO_ROLE_INPUT_PULLDOWN || role == HWIO_ROLE_OUTPUT);
        if (port != 0 || !readable) return SHELL_BAD_ARGS;
        out[0] = gpio_get(pin) ? 1u : 0u; *outlen = 1; return SHELL_OK;
    }
    case CMD_GPIO_ROLES: {         // discovery: effective (post-demotion) hwio role of GP2..GP9
        if (cap < (uint8_t)(2u + HIL_GPIO_COUNT)) return SHELL_BAD_ARGS;
        uint8_t n = 0;
        out[n++] = HIL_GPIO_BASE;
        out[n++] = HIL_GPIO_COUNT;
        for (uint8_t i = 0; i < HIL_GPIO_COUNT; i++) out[n++] = g_hwio_role[i];
        *outlen = n; return SHELL_OK;
    }
    case CMD_INTERLOCK_CLEAR:
        interlock_request_global_clear(); return SHELL_OK;
    case CMD_INTERLOCK_STATUS:
        *outlen = il_status_pack(out, cap); return SHELL_OK;
    case CMD_PWM_TEST: {           // GP22 test source: [freq_hz u32][duty_pct u8]
        if (alen < 5) return SHELL_BAD_ARGS;
        uint32_t f = (uint32_t)args[0] | ((uint32_t)args[1] << 8) |
                     ((uint32_t)args[2] << 16) | ((uint32_t)args[3] << 24);
        pwm_test_set(f, args[4]);
        if (cap >= 4) {            // readback: pin / function / instantaneous level / slice
            out[0] = PWM_TEST_PIN;                              // 22
            out[1] = (uint8_t)gpio_get_function(PWM_TEST_PIN);  // GPIO_FUNC_PWM = 4
            out[2] = (uint8_t)gpio_get(PWM_TEST_PIN);           // instantaneous pin level
            out[3] = (uint8_t)pwm_gpio_to_slice_num(PWM_TEST_PIN);
            *outlen = 4;
        }
        return SHELL_OK;
    }
    case CMD_ADC_READ: {           // bench: 3-ch decimated ADC snapshot -> [ch u16]*ADC_NCH
        if (cap < (uint8_t)(2u * ADC_NCH)) return SHELL_BAD_ARGS;
        uint8_t n = 0;
        for (uint8_t i = 0; i < ADC_NCH; i++) {
            uint16_t v = g_adc_latest[i];
            out[n++] = (uint8_t)v; out[n++] = (uint8_t)(v >> 8);
        }
        if (cap >= (uint8_t)(2u * ADC_NCH + 8u)) {   // liveness: decimated-set count + FIFO resyncs
            uint32_t dc = g_adc_decim_count, rs = g_adc_resyncs;
            out[n++]=(uint8_t)dc; out[n++]=(uint8_t)(dc>>8); out[n++]=(uint8_t)(dc>>16); out[n++]=(uint8_t)(dc>>24);
            out[n++]=(uint8_t)rs; out[n++]=(uint8_t)(rs>>8); out[n++]=(uint8_t)(rs>>16); out[n++]=(uint8_t)(rs>>24);
        }
        *outlen = n; return SHELL_OK;
    }
    default:
        return CMD_NOT_MINE;
    }
}

// Role-agnostic engine bring-up: create the runtime, activate every real KB (capture
// start nodes), bind the ADC blackboard slots. Peripherals (ADC/I2C/hwio/interlock)
// must already be up — the master does that in app_engine_task; the slave in
// node_thread2_start. Leaves g_rt ready for cfl_runtime_run().
static void engine_runtime_bringup(void) {
    const chaintree_handle_t *h = &g_chaintree_handle;
    chaintree_handle_bb_init_hashes();   // blackboard field name-hashes are static-init'd to 0
    cfl_runtime_create_params_t p; memset(&p, 0, sizeof p);
    p.perm = &g_perm; p.perm_buffer = g_perm_buf; p.perm_buffer_size = (uint16_t)sizeof g_perm_buf;
    p.heap_size = 4096; p.max_allocator_count = cfl_calculate_arrena_number(h);  // ~250 B in use; 4 KB = generous margin for KB2/3/4
    p.total_node_count = h->node_count; p.allocator_0_size = 256;
    p.event_queue_high_priority_size = 8; p.event_queue_low_priority_size = 64; p.delta_time = 0.01;

    g_rt = cfl_runtime_create(&g_perm, &p, h);
    if (!g_rt) chassis_panic(RST_PANIC, 3);
    cfl_runtime_reset(g_rt);
    bus_pool_init();   // §17 step 1: carve the TX buffer pool from cfl_perm (g_perm now ready)
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
        if (strcmp(nm, "kb0") == 0)         g_kb0_node = node;
        else if (strcmp(nm, "kb1") == 0)    g_kb1_node = node;
        else if (strcmp(nm, "kbapp") == 0)  g_kbapp_node = node;
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
}

static void app_engine_task(void *arg) {
    (void)arg;
    interlock_boot_decide();   // Thread 2: validate persist + bootloop guard (before peripherals)
    // Central ADC service: free-running round-robin + 1 kHz FIFO ISR on core1.
    // (Started here so ADC_IRQ_FIFO is owned by core1.)
    adc_service_init();
    i2c_init_hw();      // GP10/11 I2C manager (master, 100 kHz) — inited here; serviced on core1 task
    hwio_apply(&g_hwio); // FROZEN HIL pin roles from 'hwio': configure GP2..GP9 + arm the servo bank
#ifdef I2C_SELFTEST
    i2c_loopback_init(); // opt-in self-test: i2c0 slave 0x42 on GP20/21
#endif
    interlock_thread2_start(); // Thread 2: warm-restore / arm from 'ilcf', start the tick task (core1)

    engine_runtime_bringup();
    g_hb[HB_APP]++; g_hb_us[HB_APP] = time_us_32();
    cfl_runtime_run(g_rt);                           // forever loop (the thread body)
    for (;;) vTaskDelay(pdMS_TO_TICKS(100));         // not reached
}

// ---- C3: the chain-tree engine on the SLAVE (engine<->engine node-to-node) ------
// The slave already brought up Thread 2 peripherals (node_thread2_start: interlock +
// ADC + hwio). These add the engine + the bus<->engine plumbing so the slave's own
// kbapp answers app messages. node_engine_start() is called from node_role_run().

// Create the inter-core queues (shared by both roles' engine paths).
void appcore_queues_init(void) {
    g_down_q = xQueueCreate(8, sizeof(appcore_cmd_t));
    g_up_q   = xQueueCreate(16, sizeof(appcore_rep_t));   // headroom for a snapshot burst (7 frames)
    g_i2c_req_q = xQueueCreate(8, sizeof(i2c_req_t));     // engine -> i2c service
    g_orig_q = xQueueCreate(4, sizeof(orig_req_t));       // engine -> bus (C2 originate)
    if (!g_down_q || !g_up_q || !g_i2c_req_q || !g_orig_q) chassis_panic(RST_PANIC, 2);
}

// Slave: route an inbound bus app command into the engine (async). Returns true if it
// claimed the command (caller skips its synchronous dispatch). Non-app cmds -> false,
// so echo/GPIO/interlock keep their existing in-window node_cmd_dispatch path.
bool node_engine_try_route(uint8_t src, uint16_t req, uint16_t cmd,
                           const uint8_t *args, uint8_t alen) {
    if (cmd != CMD_APP_ECHO && cmd != CMD_APP_IL_CLEAR) return false;  // only engine-app opcodes
    if (!g_down_q) return false;               // engine not up yet -> let caller sync-dispatch
    appcore_cmd_t c; c.req_id = req; c.cmd = cmd; c.route = ROUTE_BUS; c.bus_src = src;
    c.alen = (alen > APPCORE_ARGS_MAX) ? APPCORE_ARGS_MAX : alen;
    memcpy(c.args, args, c.alen);
    (void)xQueueSend(g_down_q, &c, 0);         // reply ships later via the pump -> bus window
    return true;
}

// Slave core0 pump: drain engine replies and stage them in the bus node's TX queue
// (keeps bus_node_queue single-core). The reply rides the next POLL grant.
static void node_reply_pump_task(void *arg) {
    (void)arg;
    appcore_rep_t up;
    for (;;) {
        while (xQueueReceive(g_up_q, &up, 0) == pdTRUE) {
            uint8_t b[BUS_PAYLOAD_MAX]; uint8_t n = 0;
            b[n++] = (uint8_t)(up.opcode & 0xFF); b[n++] = (uint8_t)(up.opcode >> 8);
            uint8_t m = up.len; if (m > (uint8_t)(BUS_PAYLOAD_MAX - n)) m = (uint8_t)(BUS_PAYLOAD_MAX - n);
            for (uint8_t i = 0; i < m; i++) b[n++] = up.payload[i];
            (void)bus_node_queue(up.dest, BUS_FT_DATA, b, n);
        }
        vTaskDelay(1);
    }
}

static void node_engine_task(void *arg) {
    (void)arg;
    adc_service_start_on_core();  // own the ADC FIFO ISR on core1 (config done in
                                  // node_thread2_start) — matches the master, keeps
                                  // the ISR off the contended core0.
    engine_runtime_bringup();   // peripherals already up via node_thread2_start
    g_hb[HB_APP]++; g_hb_us[HB_APP] = time_us_32();
    cfl_runtime_run(g_rt);
    for (;;) vTaskDelay(pdMS_TO_TICKS(100));   // not reached
}

// Called from node_role_run() (slave) to bring up the engine + bus<->engine plumbing.
void node_engine_start(void) {
    appcore_queues_init();
    TaskHandle_t t_eng, t_pump;
    // engine on core1 (prio 3, below the interlock tick prio 4 — safety preempts the app)
    xTaskCreate(node_engine_task, "app", configMINIMAL_STACK_SIZE * 8, NULL, 3, &t_eng);
    vTaskCoreAffinitySet(t_eng, 1u << 1);
    // reply pump on core0 (prio 2), alongside the node responder
    xTaskCreate(node_reply_pump_task, "epump", configMINIMAL_STACK_SIZE * 2, NULL, 2, &t_pump);
    vTaskCoreAffinitySet(t_pump, 1u << 0);
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
    g_neti_rc = boot_read_netcfg(&g_netcfg);   // WiFi creds + agent endpoint ('neti'); MISSING is benign

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
    appcore_queues_init();

    uint32_t t0 = time_us_32();
    for (int i = 0; i < HB_COUNT; i++) g_hb_us[i] = t0;

    // 8 KB stacks for the working threads (+4 KB over prior *4) — app/engine ran
    // the tightest at ~360 B headroom and the chain_tree walker deepens with each
    // KB; FreeRTOS heap has the room. Watchdog stays at 2 KB (trivial loop).
    xTaskCreate(bus_control_task, "bus",    configMINIMAL_STACK_SIZE * 8, NULL, 2, &t_bus);
    // §dual-transport step 2: ONE supervisor selects USB/WiFi/standalone at runtime (USB preempts).
    // *12 stack = the WiFi path's depth (lwIP/cyw43); USB/standalone use far less.
    xTaskCreate(uplink_supervisor_task, "uplink", configMINIMAL_STACK_SIZE * 12, NULL, 2, &t_up);
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
