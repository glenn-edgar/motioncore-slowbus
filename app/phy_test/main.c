// app/phy_test/main.c — PIO 9-bit PHY internal-loopback self-test (gating task).
//
// Built with -DPHY_RX_PIN=20 so the RX SM samples the TX pin (GP20): the PHY
// loops back on-chip, no external wiring. Sends 9-bit words (data + address-marked)
// and verifies bus_phy_rx_pop() returns them bit-exact, marker included.
#include <stdio.h>
#include "pico/stdlib.h"
#include "bus_phy.h"
#include "board.h"

int main(void) {
    stdio_init_all();
    sleep_ms(2000);                       // let USB-CDC enumerate
    printf("\n[phy_test] 9-bit PIO PHY internal loopback (RX samples GP%u) @ %u baud\n",
           (unsigned)BUS_PIN_DI, (unsigned)BUS_DEFAULT_BAUD);
    bus_phy_init(BUS_DEFAULT_BAUD);

    const uint16_t tx[] = { 0x055, 0x0AA, 0x1FF, 0x100, 0x003, 0x10A, 0x000, 0x0FF };
    const int N = (int)(sizeof(tx) / sizeof(tx[0]));

    for (uint32_t round = 0; ; round++) {
        int pass = 0;
        uint16_t first_rx = 0xFFFF; int first_ok = 0;
        for (int i = 0; i < N; i++) {
            bus_phy_rx_flush();
            uint16_t w = tx[i];
            bus_phy_send_words(&w, 1);
            uint16_t rx = 0xFFFF;
            int ok = 0;
            for (int t = 0; t < 2000 && !ok; t++) { if (bus_phy_rx_pop(&rx)) ok = 1; sleep_us(100); }
            if (i == 0) { first_ok = ok; first_rx = rx; }
            if (ok && rx == w) pass++;
        }
        printf("[phy_test] round %u: %d/%d OK  (first tx=0x055 rx=0x%03X seen=%d)  words=%u ovr=%u\n",
               (unsigned)round, pass, N, first_ok ? first_rx : 0xFFFF, first_ok,
               (unsigned)bus_phy_rx_words(), (unsigned)bus_phy_rx_overrun());
        sleep_ms(2000);
    }
}
