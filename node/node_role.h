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
// the FreeRTOS scheduler. NEVER RETURNS. `addr` = this node's RS-485 address
// (from the config-FS identity). The caller (main) has already done
// stdio_init_all() and read+validated the identity.
void node_role_run(uint8_t addr);
