// ============================================================================
// node_role.h — the SLAVE/node role of the single unified image.
//
// One firmware binary serves both roles; main() reads the config identity and,
// for a slave variant, calls node_role_run(). The master role stays in
// app/bus_controller/main.c. (Glenn 2026-06-22: same image, config picks role.)
// ============================================================================
#pragma once

#include <stdint.h>

// Run the node/slave role: board + PHY init, the bus_node responder task, then
// the FreeRTOS scheduler. NEVER RETURNS. `addr` = this node's RS-485 address and
// `baud` = the configured bus speed (both from the config-FS identity; baud 0 ->
// BUS_DEFAULT_BAUD). The caller (main) has already done stdio_init_all() and
// read+validated the identity.
void node_role_run(uint8_t addr, uint32_t baud);

// Role-agnostic Thread-2 hooks, defined in app/bus_controller/main.c, used by the
// slave responder so the interlock runs on the slave too and its trip/latch/clear
// can be driven over the bus (the master uses its own app_engine_task path).
void    node_thread2_start(void);   // boot_decide + hwio_apply + arm ilcN + tick task

// Thread-1 unified operate-command dispatch (B1), shared by the master appcore drain
// and the slave bus responder. Handles echo / GPIO read-write / interlock clear-
// status into out (<= cap) + *outlen, returns a SHELL_* status, or CMD_NOT_MINE
// (0xFF) if cmd isn't one of these. The caller owns the reply transport.
#define CMD_NOT_MINE 0xFFu
uint8_t node_cmd_dispatch(uint16_t cmd, const uint8_t *args, uint8_t alen,
                          uint8_t *out, uint8_t cap, uint8_t *outlen);
