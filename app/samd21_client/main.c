// ============================================================================
// register_dongle - Seeeduino Xiao SAMD21
//
// I2C-config-chip role: the s_engine event pipeline that used to drive the
// USB-CDC dongle protocol has been removed (it lagged shell replies by ~6
// commands). The USB command path is now a simple SYNCHRONOUS dispatch loop:
//   tud_cdc_read -> frame_decoder_feed -> on FRAME_READY, if OP_SHELL_EXEC,
//   call shell_dispatch_payload IN-LINE and stage the OP_SHELL_REPLY in the
//   shared TX ring. No event queue, no state machine, no handshake.
//
// One USB-CDC port. s2m frames staged in the shared TX ring; main loop drains
// to CDC each iteration.
// ============================================================================

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"
#include "samd21.h"     // CMSIS: NVIC_SystemReset (inline)

#include "frame.h"
#include "opcodes.h"
#include "flash_storage.h"
#include "shell_commands.h"  // firmware_sysinfo_t
#include "samd21_hal.h"      // hal_wdt_init/pet, hal_capture_reset_cause
#include "samd21_hal_pin.h"  // hal_pin_check_consistency
#include "samd21_interlocks.h"

// Implemented in user_functions.c.
extern void     register_dongle_load_commissioning(void);
extern void     register_dongle_chip_uid(uint8_t out[16]);
extern uint16_t shell_dispatch_payload(const uint8_t* exec, uint16_t exec_len, uint8_t* reply);

// OP_SHELL_EXEC body is [request_id u16][command_id u16][args...]; the header
// is 4 bytes. Mirrors SHELL_EXEC_HEADER_LEN in user_functions.c.
#define SHELL_EXEC_HEADER_LEN 4u

// Deferred-reboot plumbing: firmware_request_reboot sets the reboot time; the
// main loop waits for TX to drain, then triggers NVIC_SystemReset so any
// in-flight bytes actually reach the host before USB renegotiates. No internal
// caller remains, but kept for the shell-side reboot command.
static volatile uint32_t g_reboot_at_ms = 0;   // 0 = no reboot pending

void firmware_request_reboot(uint32_t delay_ms) {
    g_reboot_at_ms = board_millis() + delay_ms;
    if (g_reboot_at_ms == 0) g_reboot_at_ms = 1;   // 0 reserved as sentinel
}

// ----------------------------------------------------------------------------
// firmware_get_sysinfo — SAMD21G18A implementation. Called by CMD_SYSINFO
// shell handler. Linker symbols are provided by seeeduino_xiao.ld.
//
// Flash layout: app starts at 0x2000 (8 KB bootloader reserved). _etext is
// end of code+rodata. The .data initializer follows _etext in flash and is
// copied to _srelocate..._erelocate in RAM at startup.
// ----------------------------------------------------------------------------
extern char _etext[];        // end of text+rodata in flash
extern char _srelocate[];    // start of .data in RAM
extern char _erelocate[];    // end   of .data in RAM
extern char _sbss[];         // start of .bss in RAM
extern char _ebss[];         // end   of .bss in RAM
extern char _sstack[];       // start of stack region in RAM
extern char _estack[];       // top of stack

#define SAMD21G18A_FLASH_TOTAL_KB  256u
#define SAMD21G18A_RAM_TOTAL_KB    32u
#define SAMD21_APP_FLASH_ORIGIN    0x2000u

void firmware_get_sysinfo(firmware_sysinfo_t* out) {
    out->flash_total_kb   = SAMD21G18A_FLASH_TOTAL_KB;
    out->flash_text_b     = (uint32_t)((uintptr_t)_etext - SAMD21_APP_FLASH_ORIGIN);
    out->flash_data_b     = (uint32_t)((uintptr_t)_erelocate - (uintptr_t)_srelocate);
    out->ram_total_kb     = SAMD21G18A_RAM_TOTAL_KB;
    out->ram_bss_b        = (uint32_t)((uintptr_t)_ebss - (uintptr_t)_sbss);
    out->ram_stack_b      = (uint32_t)((uintptr_t)_estack - (uintptr_t)_sstack);
    out->bump_capacity_b  = 0u;   // bump allocator removed with the s_engine
    out->bump_peak_b      = 0u;
    out->uptime_ms        = (uint32_t)board_millis();
    out->cpu_clock_hz     = SystemCoreClock;
}

// ----------------------------------------------------------------------------
// Shared TX ring. user_functions.c references this via `extern`.
// 256 B sized to comfortably fit the largest expected frame (24 B register
// payload + 7 B s2m header + worst-case SLIP escapes).
// ----------------------------------------------------------------------------

// 1024 B sized to hold (a) the largest possible SHELL_REPLY (128 B payload +
// 8 B framing + SLIP-worst-case ≈ 280 B) plus (b) ~700 B of backlogged DBG_LOG
// / heartbeat / OP_EVENT bytes that may be queued during a long shell-handler
// run (cmd_adc_capture at 60 samples × 5 ms holds the loop for ~300 ms during
// which nothing drains). 256 B previously was tight enough that adc_capture
// replies were silently dropped — see samd21_adc_capture_bug_2026-05-28.
#define TX_RING_SIZE 1024u
static uint8_t       g_tx_ring_buf[TX_RING_SIZE];
frame_ring_t         g_tx_ring;

// ----------------------------------------------------------------------------
// Stack hardening (slice-4): paint .stack at boot with STACK_PAINT_VALUE,
// reserve the lowest 4 bytes at _sstack as a canary, then check the canary
// + scan for high-water mark after every chain pump tick. Overflow triggers
// a deferred reset so the diagnostic log line drains via TX ring first.
//
// Linker symbols (placed by seeeduino_xiao_noinit.ld):
//   _sstack — lowest address of the .stack region
//   _estack — highest address; initial SP value loaded by the vector table
//
// .noinit now sits ABOVE .stack (after slice-4 reorder), so SP dropping
// below _sstack writes into .bss/heap instead of corrupting g_interlock_persist.
// ----------------------------------------------------------------------------
// _sstack/_estack declared as `char[]` higher up in this file (for sysinfo).
#define STACK_PAINT_VALUE   0xDEADBEEFu
#define STACK_CANARY_VALUE  0xCAFEBABEu

volatile uint16_t g_stack_hwm_bytes      = 0;     // peak observed depth (bytes)
volatile uint16_t g_stack_size_bytes     = 0;     // total stack region size
volatile uint8_t  g_stack_canary_tripped = 0;     // 1 = overflow detected

// Slice 6 — feeds the IL_VIRT_T_SINCE_M2S virtual input. Updated every time
// rx_drain decodes a complete host frame; reset to board_millis() at boot so
// virtuals don't trip on the time-since-epoch right after a cold start.
volatile uint32_t g_last_m2s_rx_ms       = 0;

static void stack_paint_and_canary(void) {
    // Paint from _sstack up to (current SP - safety). Don't write above SP —
    // that would clobber our own stack frame.
    uint32_t sp;
    __asm__ volatile ("mov %0, sp" : "=r"(sp));
    uint32_t* p   = (uint32_t*)(uintptr_t)_sstack;
    uint32_t* end = (uint32_t*)(sp - 64);     // 64 B safety from current SP
    while (p < end) *p++ = STACK_PAINT_VALUE;
    // Canary at the LOWEST word — first thing overwritten by stack overflow.
    *(volatile uint32_t*)(uintptr_t)_sstack = STACK_CANARY_VALUE;
    g_stack_size_bytes = (uint16_t)((uintptr_t)_estack - (uintptr_t)_sstack);
}

// ----------------------------------------------------------------------------
// system_self_check (slice 5, defensive amendment D).
//
// Called at end of init, immediately before the for(;;) main loop. Validates
// every invariant the main loop assumes; any failure routes through panic()
// so the next boot prints [BOOT_IL] ... pn=N ... with the discriminator.
//
// Six invariants per slice-5 prep memory Q6 ratification:
//   1. Stack canary intact at _sstack
//   2. interlock_persist magic == INTERLOCK_MAGIC
//   3. interlock_persist version == INTERLOCK_PERSIST_VERSION
//   4. interlock_persist self_size == sizeof(interlock_persist_t)
//   5. HAL pin claim table has no duplicate non-shared owners
//   6. Crash record self-consistency (if last_pc != 0 then last_crashed_slot
//      must be 0xFF (no-slot) OR < INTERLOCK_MAX_SLOTS)
// ----------------------------------------------------------------------------

extern interlock_persist_t g_interlock_persist;   // declared in samd21_interlocks.h

static void system_self_check(void) {
    // 1. Stack canary
    if (*(volatile uint32_t*)(uintptr_t)_sstack != STACK_CANARY_VALUE) {
        panic(PANIC_INIT_CANARY_BAD, 0);
    }
    // 2. Persist magic
    if (g_interlock_persist.magic != INTERLOCK_MAGIC) {
        panic(PANIC_PERSIST_MAGIC_BAD, g_interlock_persist.magic);
    }
    // 3. Persist version
    if (g_interlock_persist.version != INTERLOCK_PERSIST_VERSION) {
        panic(PANIC_PERSIST_VERSION_BAD, g_interlock_persist.version);
    }
    // 4. Persist self_size
    if (g_interlock_persist.self_size != (uint16_t)sizeof(interlock_persist_t)) {
        panic(PANIC_PERSIST_SIZE_BAD, g_interlock_persist.self_size);
    }
    // 5. HAL pin claim table
    if (!hal_pin_check_consistency()) {
        panic(PANIC_HAL_PIN_DUPLICATE, 0);
    }
    // 6. Crash record consistency — if there's a recorded crash (last_pc != 0
    //    or panic_code != 0), the slot field must be either NONE (0xFF) or a
    //    valid slot index.
    {
        const interlock_crash_record_t* cr = &g_interlock_persist.crash;
        bool has_crash = (cr->last_pc != 0) || (cr->panic_code != PANIC_NONE);
        bool slot_ok   = (cr->last_crashed_slot == INTERLOCK_CRASHED_SLOT_NONE)
                      || (cr->last_crashed_slot < INTERLOCK_MAX_SLOTS);
        if (has_crash && !slot_ok) {
            panic(PANIC_CRASH_RECORD_BAD, cr->last_crashed_slot);
        }
    }
}

// ----------------------------------------------------------------------------
// RX frame decoder. Direction = M2S (5-byte header). Persistent state — the
// in_escape flag does NOT reset between calls. Frames feed in one byte at a
// time from tud_cdc_read; on FRAME_READY we push the cmd to the engine.
// ----------------------------------------------------------------------------

static frame_decoder_t g_rx_decoder;

// ----------------------------------------------------------------------------
// dbg_log — stage a text line as an OP_DBG_LOG s2m frame in the TX ring. Used
// for the boot banner ([BOOT] rstsr / interlock boot line) now that the
// s_engine debug_fn callback is gone. Same drain path as shell replies.
// ----------------------------------------------------------------------------

static uint8_t g_dbg_seq = 0;

static void dbg_log(const char* msg) {
    if (!msg) return;
    size_t len = strlen(msg);
    if (len > COMM_PAYLOAD_MAX) len = COMM_PAYLOAD_MAX;
    frame_meta_t meta = {
        .addr        = 1,
        .cmd         = OP_DBG_LOG,
        .seq         = g_dbg_seq++,
        .ack_seq     = 0,
        .ack_status  = 0,
        .payload_len = (uint8_t)len,
    };
    (void)frame_encode_s2m(&meta, (const uint8_t*)msg, &g_tx_ring);
}

// ----------------------------------------------------------------------------
// usb_cmd_poll — synchronous USB command dispatch. Reads any available CDC
// bytes, feeds them through the frame decoder, and on each complete
// OP_SHELL_EXEC frame calls shell_dispatch_payload IN-LINE (same loop
// iteration) and stages the OP_SHELL_REPLY in the TX ring. No event queue, no
// state gating, no handshake — this is the fix for the ~6-command s_engine lag.
// All other opcodes are ignored (no dongle protocol any more).
// ----------------------------------------------------------------------------

static uint8_t g_rx_buf[64];
static uint8_t g_rx_payload[COMM_PAYLOAD_MAX];
static uint8_t g_reply_seq;

static void usb_cmd_poll(void) {
    if (!tud_cdc_connected() || !tud_cdc_available()) return;
    uint32_t n = tud_cdc_read(g_rx_buf, sizeof g_rx_buf);
    for (uint32_t i = 0; i < n; i++) {
        frame_meta_t meta;
        if (frame_decoder_feed(&g_rx_decoder, g_rx_buf[i], &meta, g_rx_payload)
            != FRAME_DECODE_FRAME_READY) continue;
        // Slice 6 — feed the IL_VIRT_T_SINCE_M2S virtual.
        g_last_m2s_rx_ms = board_millis();
        if (meta.cmd == OP_SHELL_EXEC && meta.payload_len >= SHELL_EXEC_HEADER_LEN) {
            uint8_t reply[COMM_PAYLOAD_MAX];
            uint16_t rlen = shell_dispatch_payload(g_rx_payload, meta.payload_len, reply);
            frame_meta_t rm = { .addr = 1, .cmd = OP_SHELL_REPLY,
                .seq = g_reply_seq++, .ack_seq = 0, .ack_status = 0,
                .payload_len = (uint8_t)rlen };
            (void)frame_encode_s2m(&rm, reply, &g_tx_ring);
        }
        // all other opcodes: ignored (no dongle protocol any more)
    }
}

// 1200-baud "touch" -> reboot into the UF2 bootloader (Arduino/Adafruit-SAMD
// convention), so the host can reflash over USB without the physical double-tap.
// Opening the CDC port at 1200 baud sets DBL_TAP_MAGIC at the top of RAM and
// resets; the SAMD21 UF2 bootloader sees the magic and stays in the bootloader.
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding) {
    (void)itf;
    if (coding->bit_rate == 1200u) {
        *((volatile uint32_t *)0x20007FFCu) = 0xF01669EFu;  // top of 32 KB RAM - 4
        NVIC_SystemReset();
    }
}

// ----------------------------------------------------------------------------
// Entry
// ----------------------------------------------------------------------------

int main(void) {
    // Snapshot PM->RCAUSE BEFORE anything else can clobber it. Distinguishes
    // WDT bite (the layer-2 chain-stuck signal) from power-on / external
    // reset / NVIC_SystemReset.
    hal_capture_reset_cause();

    // Paint stack + install canary. Must run AFTER reset cause capture
    // (which uses minimal stack) and BEFORE any deeper call chain that
    // would consume more stack than our 64 B paint-safety margin allows.
    stack_paint_and_canary();

    // Interlock framework boot decision. Reads .noinit persistence, applies
    // bootloop guard, marks slots POISONED if they've crashed too many times
    // in a row. Must run before board_init / engine setup so the framework
    // can also report attribution if the very next thing crashes.
    interlock_boot_decide();

    board_init();

    // Arm the WDT immediately after board_init so any subsequent hang in
    // tusb_init / bring-up triggers recovery within ~4 s. The pet site is now
    // the C main loop (hal_wdt_pet() called every pass), not a chain pump.
    hal_wdt_init();

    // Bring up always-on peripherals (DAC, ADC, and future I2C/RS-485) once.
    // Pins they own are reserved from GPIO commands by pin_is_reserved() in
    // samd21_commands.c.
    samd21_peripherals_init();

    // Re-parse + re-claim pins for any DSL-defined interlock slots that
    // survived the warm boot. Slots that fail re-parse / re-claim are
    // marked POISONED. Idempotent on cold boot (no ARMED slots).
    interlock_warm_restore();

    tusb_rhport_init_t const rhport_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(BOARD_TUD_RHPORT, &rhport_init);

    frame_ring_init(&g_tx_ring, g_tx_ring_buf, TX_RING_SIZE);
    frame_decoder_init(&g_rx_decoder, FRAME_DIR_M2S);

    // Slice 6: seed the m2s timestamp at "now" so _t_since_m2s starts at 0,
    // not at uptime. Host hasn't sent anything yet but we don't want the
    // virtual to trip an arming-immediately watch before the first frame.
    g_last_m2s_rx_ms = board_millis();

    // Read commissioning blob from flash. Factory-fresh chips read nothing;
    // defaults to UNCOMMISSIONED.
    register_dongle_load_commissioning();

    // Emit captured reset cause as the first dbg_log frame. Sits in the TX ring
    // until USB CDC enumerates; host sees it on first attach. Grep-able literal
    // "[BOOT] rstsr=0xNN" — only the RCAUSE low byte is meaningful.
    {
        char buf[24];
        uint8_t rc = hal_get_reset_cause();
        static const char hex[] = "0123456789abcdef";
        buf[0]='['; buf[1]='B'; buf[2]='O'; buf[3]='O'; buf[4]='T'; buf[5]=']';
        buf[6]=' '; buf[7]='r'; buf[8]='s'; buf[9]='t'; buf[10]='s'; buf[11]='r';
        buf[12]='='; buf[13]='0'; buf[14]='x';
        buf[15] = hex[(rc >> 4) & 0xF];
        buf[16] = hex[(rc >> 0) & 0xF];
        buf[17] = '\0';
        dbg_log(buf);

        // Companion line summarising interlock persistence + crash record.
        // Always emitted, even on cold boot (slots EMPTY, crash zeroed).
        char il_buf[96];
        (void)interlock_format_boot_line(il_buf, sizeof(il_buf));
        dbg_log(il_buf);
    }

    // Interlock eval cadence: ~1 kHz, gated on board_millis().
    uint32_t next_il_ms = 1;

#ifdef I2C_CLIENT
    // Tracks the CDC connection state across loop passes so a connected->
    // disconnected edge can trigger the OFFLINE->ONLINE reboot. Seeded to the
    // current state so the first pass never sees a spurious edge.
    bool prev_conn = tud_cdc_connected();
#endif

    // Defensive amendment D — gate entry to the main loop on invariants.
    // Any failure panics with a discriminated code; next boot prints it.
    system_self_check();

    for (;;) {
        tud_task();

        // Synchronous USB command dispatch: each complete OP_SHELL_EXEC frame
        // is handled in-line and its reply staged this same iteration.
        usb_cmd_poll();

#ifdef I2C_CLIENT
        i2c_store_service();   // M2b: do any pending config-store flash commit (~ms, off the I2C ISR)
#endif

        // Pet the WDT every pass. Previously petted inside the s_engine chain
        // pump; with the chain gone this is the sole pet site, so it must run
        // unconditionally each loop or the chip resets in ~4 s.
        hal_wdt_pet();

        uint32_t now = board_millis();

        // Interlock eval on a fast ~1 kHz cadence — a bounded safety response.
        // Single-core: runs sequentially with usb_cmd_poll, so a
        // CMD_INTERLOCK_SET dispatched synchronously can't race the eval.
        if ((int32_t)(now - next_il_ms) >= 0) {
            next_il_ms += 1;
            interlock_tick_all();
        }

        // Amendment A — pre-overflow SP check, run every loop. Catches a
        // near-overflow BEFORE any deeper call chain can write below _sstack.
        // 256 B margin sized to cover the deepest IRQ chain (~500 B for TinyUSB
        // CDC) with half the margin reserved for IRQ overhead between this
        // check and the overflow point.
        {
            uint32_t sp;
            __asm__ volatile ("mov %0, sp" : "=r"(sp));
            if (sp < ((uintptr_t)_sstack + 256u)) {
                panic(PANIC_STACK_NEAR_OVERFLOW, sp);
            }
        }

        // Stack hardening: verify canary + update HWM every loop. Canary trip
        // → panic (immediate reset) so the post-mortem code surfaces in the
        // next boot's [BOOT_IL] line via panic_code = INIT_CANARY_BAD.
        if (*(volatile uint32_t*)(uintptr_t)_sstack != STACK_CANARY_VALUE) {
            g_stack_canary_tripped = 1;
            dbg_log("[STACK_CANARY_TRIPPED] panicking");
            panic(PANIC_INIT_CANARY_BAD, 0);
        } else {
            uint32_t* p   = (uint32_t*)((uintptr_t)_sstack + 4);
            uint32_t* end = (uint32_t*)(uintptr_t)_estack;
            while (p < end && *p == STACK_PAINT_VALUE) p++;
            uint16_t used = (uint16_t)((uintptr_t)end - (uintptr_t)p);
            if (used > g_stack_hwm_bytes) g_stack_hwm_bytes = used;
        }

        // Drain the TX ring to CDC every loop, but ONLY as many bytes as the CDC
        // FIFO will accept this instant. Draining a fixed 64 B and ignoring
        // tud_cdc_write()'s return silently drops the overflow (the bytes are
        // already gone from g_tx_ring) — which shreds a large frame that spans a
        // partly-full FIFO while small frames fit. Pulling exactly write_available
        // bytes keeps every frame intact; the rest stays queued for next loop.
        if (tud_cdc_connected()) {
            uint32_t avail = tud_cdc_write_available();
            if (avail > 0) {
                uint8_t buf[64];
                if (avail > sizeof(buf)) avail = sizeof(buf);
                uint32_t n = frame_ring_read_drain(&g_tx_ring, buf, avail);
                if (n > 0) {
                    tud_cdc_write(buf, n);   // n <= avail, fully accepted
                    tud_cdc_write_flush();
                }
            }
        }

        // Commissioning OFFLINE -> ONLINE transition. A host that entered the
        // OFFLINE state (CMD_OFFLINE), wrote config files, then closed the CDC
        // port wants the unit to reboot into ONLINE with the new config applied
        // (i2c_slave_init/B4 re-reads + enters the commissioned mode). Detect the
        // connected->disconnected edge while offline, and reboot — but only once
        // any pending config-store flash commit has finished (so files persist).
        // An ONLINE disconnect (not g_offline) does nothing.
#ifdef I2C_CLIENT
        {
            static bool reboot_armed = false;
            bool conn = tud_cdc_connected();
            // Latch the reboot intent on the offline disconnect edge (the edge
            // fires once); then fire once the pending config-store flash commit
            // has drained, so the just-written files persist before reset.
            if (prev_conn && !conn && samd21_is_offline()) reboot_armed = true;
            prev_conn = conn;
            if (reboot_armed && !samd21_store_commit_pending()) {
                NVIC_SystemReset();
                // not reached
            }
        }
#endif

        // Deferred reboot. Wait until the requested time has passed so any
        // in-flight reply / log bytes leave the chip before USB renegotiates.
        if (g_reboot_at_ms != 0 && (int32_t)(board_millis() - g_reboot_at_ms) >= 0) {
            tud_cdc_write_flush();
            NVIC_SystemReset();
            // not reached
        }
    }
}
