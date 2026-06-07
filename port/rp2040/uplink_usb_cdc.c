// ============================================================================
// uplink_usb_cdc.c — RP2350 USB-CDC implementation of bus_uplink.h (BC host link).
//
// STATUS: SKELETON. Implements the bus_uplink.h contract over the Pico SDK
// USB-CDC stdio. The host wire format (how a host->bus message names its dest
// node, and how bus->host frames are tagged with the source) is the BC-1b
// libcomm bridge format from the motioncore project; port it here once the bus
// core moves bytes. For now this is a no-traffic stub that links and runs.
// ============================================================================
#include "bus_uplink.h"
#include "pico/stdlib.h"

void bus_uplink_init(void) {
    // stdio_usb is initialised by the app (stdio_init_all). Nothing else yet.
}

int bus_uplink_poll(uint8_t *dest, uint8_t *buf, int max) {
    (void)dest; (void)buf; (void)max;
    // TODO(bring-up): read a framed host->bus message (SLIP+CRC libcomm frame),
    //   parse [dest][opcode|body], return its length. 0 = nothing pending.
    return 0;
}

void bus_uplink_send(uint8_t src, const uint8_t *payload, int len) {
    (void)src; (void)payload; (void)len;
    // TODO(bring-up): frame [src][payload] as a libcomm s2m frame and write it
    //   to the USB-CDC endpoint.
}

void bus_uplink_task(void) {
    // TinyUSB/stdio_usb is serviced by the SDK; no per-loop work needed yet.
}
