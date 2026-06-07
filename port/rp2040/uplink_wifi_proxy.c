// ============================================================================
// uplink_wifi_proxy.c — RP2040 / Pico W WiFi implementation of bus_uplink.h.
//
// STATUS: SKELETON. Same northbound contract as uplink_usb_cdc.c, but the
// transport is a TCP socket to a Linux PROXY process instead of USB-CDC. The
// proxy "mangles the dongle" — it does exactly what the USB host did, and runs
// the zenoh side. zenoh-pico is deliberately NOT on the MCU: the chip only
// streams the SAME libcomm frames it would over USB, just over WiFi.
//
// So this and uplink_usb_cdc.c emit/consume an identical frame stream; the Linux
// side terminates either a serial port or a socket with one handler.
//
// Build: linked only into bus_controller_wifi (CMake WIFI=1 -> CYW43 + lwIP +
// FreeRTOS sys). The slave role never has an uplink (uplink_none.c). Bring this
// up LAST, behind the proven poll engine + USB uplink (bring-up order, README).
// ============================================================================
#include "bus_uplink.h"
#include "pico/stdlib.h"

void bus_uplink_init(void) {
    // TODO(bring-up): cyw43_arch_init(); join the configured AP (STA mode);
    //   open a TCP connection (or listen) to the Linux proxy. Hold the socket.
}

int bus_uplink_poll(uint8_t *dest, uint8_t *buf, int max) {
    (void)dest; (void)buf; (void)max;
    // TODO(bring-up): drain one framed proxy->bus libcomm message from the
    //   socket RX, parse [dest][opcode|body], return its length. 0 = nothing.
    return 0;
}

void bus_uplink_send(uint8_t src, const uint8_t *payload, int len) {
    (void)src; (void)payload; (void)len;
    // TODO(bring-up): frame [src][payload] as the libcomm s2m frame and write
    //   it to the proxy socket (same wire bytes as the USB-CDC path).
}

void bus_uplink_task(void) {
    // TODO(bring-up): service lwIP / poll the socket. cyw43 is FreeRTOS-driven.
}
