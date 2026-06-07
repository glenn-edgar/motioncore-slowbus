// ============================================================================
// uplink_none.c — null bus_uplink.h implementation for ROLE=slave.
//
// A node has no off-bus host link; the slave firmware links this so the core's
// uplink calls resolve to no-ops.
// ============================================================================
#include "bus_uplink.h"

void bus_uplink_init(void) {}
int  bus_uplink_poll(uint8_t *dest, uint8_t *buf, int max) { (void)dest; (void)buf; (void)max; return 0; }
void bus_uplink_send(uint8_t src, const uint8_t *payload, int len) { (void)src; (void)payload; (void)len; }
void bus_uplink_task(void) {}
