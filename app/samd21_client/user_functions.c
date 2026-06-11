// ============================================================================
// user_functions.c — register_dongle (SAMD21, I2C-config-chip role)
//
// The s_engine chain that used to drive the USB-CDC dongle protocol has been
// removed: the SAMD21 is now a relegated I2C config/HIL chip and the USB
// command path is a simple synchronous dispatch loop in main.c. What remains
// here is the transport-agnostic shell core (shell_dispatch_payload) plus the
// chip-identity helpers (UID, class_id/ROLE selection) that the shell uses.
//
// Frame work uses the vendored libcomm slice (SLIP + CRC-8/AUTOSAR).
// ============================================================================

#include <stdint.h>
#include <string.h>

#include "bsp/board_api.h"
#include "samd21.h"          // CMSIS: PORT register block + NVIC_SystemReset

#include "frame.h"
#include "opcodes.h"
#include "flash_storage.h"
#include "shell_commands.h"

// ----------------------------------------------------------------------------
// SAMD21 chip UID (datasheet §10.3.3): four 32-bit words at fixed addresses.
// Word 0 lives at 0x0080A00C; words 1..3 live at 0x0080A040 / +0x44 / +0x48.
// ----------------------------------------------------------------------------
#define SAMD21_UID_WORD0_ADDR  0x0080A00CU
#define SAMD21_UID_WORD1_ADDR  0x0080A040U
#define SAMD21_UID_WORD2_ADDR  0x0080A044U
#define SAMD21_UID_WORD3_ADDR  0x0080A048U

static void samd21_read_uid(uint8_t out[16]) {
    uint32_t w0 = *(volatile uint32_t*)SAMD21_UID_WORD0_ADDR;
    uint32_t w1 = *(volatile uint32_t*)SAMD21_UID_WORD1_ADDR;
    uint32_t w2 = *(volatile uint32_t*)SAMD21_UID_WORD2_ADDR;
    uint32_t w3 = *(volatile uint32_t*)SAMD21_UID_WORD3_ADDR;
    // Little-endian byte order in the payload (matches the rest of libcomm wire format).
    out[ 0] = (uint8_t)(w0 >>  0); out[ 1] = (uint8_t)(w0 >>  8);
    out[ 2] = (uint8_t)(w0 >> 16); out[ 3] = (uint8_t)(w0 >> 24);
    out[ 4] = (uint8_t)(w1 >>  0); out[ 5] = (uint8_t)(w1 >>  8);
    out[ 6] = (uint8_t)(w1 >> 16); out[ 7] = (uint8_t)(w1 >> 24);
    out[ 8] = (uint8_t)(w2 >>  0); out[ 9] = (uint8_t)(w2 >>  8);
    out[10] = (uint8_t)(w2 >> 16); out[11] = (uint8_t)(w2 >> 24);
    out[12] = (uint8_t)(w3 >>  0); out[13] = (uint8_t)(w3 >>  8);
    out[14] = (uint8_t)(w3 >> 16); out[15] = (uint8_t)(w3 >> 24);
}

// ----------------------------------------------------------------------------
// Role-specific class_id. ROLE_DONGLE / ROLE_BUS_CONTROLLER is set by the
// Makefile via -DROLE_xxx=1. Default (no ROLE on the make command line) is
// dongle, matching every previously-flashed bench unit. Retained so the shell
// identity command and any future register-style reply have a single source of
// truth.
// ----------------------------------------------------------------------------
#if defined(ROLE_BUS_CONTROLLER)
  #define REGISTER_CLASS_ID_STUB   0x5E589000U  // sequential to dongle; real FNV-1a "motioncore.bus_controller.samd21.v1" TBD
#elif defined(ROLE_SLAVE)
  #define REGISTER_CLASS_ID_STUB   0x5E589100U  // RS-485 slave; real FNV-1a "motioncore.slave.samd21.v1" TBD
#elif defined(ROLE_DONGLE)
  #define REGISTER_CLASS_ID_STUB   0x5E588873U  // FNV-1a "motioncore.dongle.register.samd21.v1"
#else
  #error "ROLE must be defined: pass ROLE=dongle, bus_controller or slave to make"
#endif

// Mutable identity. Kept for the shell identity command; no commissioning
// state machine remains on this chip.
static uint32_t g_class_id            = REGISTER_CLASS_ID_STUB;
static uint32_t g_instance_id         = 0;
static uint8_t  g_commissioning_state = COMMISSIONING_UNCOMMISSIONED;

// Called once from main() before the main loop. Reads flash; if no valid blob
// found (factory-fresh chip), leaves the globals at their UNCOMMISSIONED
// defaults.
void register_dongle_load_commissioning(void) {
    commission_blob_t blob;
    if (flash_storage_read(&blob)) {
        g_instance_id         = blob.instance_id;
        g_commissioning_state = blob.commissioning_state;
    }
}

// Public accessor for the 16-byte chip UID.
void register_dongle_chip_uid(uint8_t out[16]) {
    samd21_read_uid(out);
}

// Accessors for the chip identity. Kept so the class_id/instance_id state has
// real readers (single source of truth) now that the register/commission
// emitters are gone.
uint32_t register_dongle_class_id(void)    { return g_class_id; }
uint32_t register_dongle_instance_id(void) { return g_instance_id; }
uint8_t  register_dongle_commissioning_state(void) { return g_commissioning_state; }

// ----------------------------------------------------------------------------
// shell_dispatch_payload — transport-agnostic core of the app shell. Takes an
// OP_SHELL_EXEC body [request_id u16][command_id u16][args...] and writes the
// OP_SHELL_REPLY body [request_id u16][status u8][result...] into `reply`
// (must have capacity >= COMM_PAYLOAD_MAX). Returns the reply length.
//
// Called synchronously by main.c's usb_cmd_poll the moment a complete
// OP_SHELL_EXEC frame is decoded. Callers guarantee exec_len >=
// SHELL_EXEC_HEADER_LEN.
// ----------------------------------------------------------------------------

#define SHELL_REPLY_HEADER_LEN 3u
#define SHELL_EXEC_HEADER_LEN  4u
#define SHELL_RESULT_MAX       (COMM_PAYLOAD_MAX - SHELL_REPLY_HEADER_LEN)

uint16_t shell_dispatch_payload(const uint8_t* exec, uint16_t exec_len,
                                uint8_t* reply) {
    const uint16_t request_id =
        (uint16_t)exec[0] | ((uint16_t)exec[1] << 8);
    const uint16_t command_id =
        (uint16_t)exec[2] | ((uint16_t)exec[3] << 8);
    const uint8_t* args     = &exec[SHELL_EXEC_HEADER_LEN];
    const uint16_t args_len = (uint16_t)(exec_len - SHELL_EXEC_HEADER_LEN);

    uint8_t  result_buf[SHELL_RESULT_MAX];
    uint16_t result_len = 0;
    uint8_t  status     = SHELL_STATUS_OK;

    const shell_cmd_entry_t* cmd = shell_find_cmd(command_id);
    if (cmd == NULL) {
        status = SHELL_STATUS_UNKNOWN_CMD;
    } else {
        shell_reader_t r;
        shell_writer_t w;
        sr_init(&r, args, args_len);
        sw_init(&w, result_buf, sizeof(result_buf));
        status = cmd->fn(&r, &w);
        if (status == SHELL_STATUS_OK) {
            if (r.overflow)      status = SHELL_STATUS_BAD_ARGS;
            else if (w.overflow) status = SHELL_STATUS_RESULT_TOO_BIG;
        }
        // Capture any bytes the handler wrote regardless of status — failure
        // paths may attach a diagnostic payload (e.g., interlock-set parse-
        // error detail). Writer-overflow already promoted to RESULT_TOO_BIG.
        if (!w.overflow) result_len = sw_len(&w);
    }

    reply[0] = (uint8_t)(request_id & 0xFFu);
    reply[1] = (uint8_t)(request_id >> 8);
    reply[2] = status;
    if (result_len > 0) {
        memcpy(&reply[SHELL_REPLY_HEADER_LEN], result_buf, result_len);
    }
    return (uint16_t)(SHELL_REPLY_HEADER_LEN + result_len);
}
