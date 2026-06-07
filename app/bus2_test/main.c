// app/bus2_test/main.c — 2-node bring-up: BC polls the SAMD21 slave at addr 3.
//
// Minimal BC poll loop on the proven bus_frame codec + PIO PHY (RX on the REAL
// pin GP21, not loopback). Sends a POLL to slave 3 and decodes the reply
// (NO_MESSAGE when the slave has nothing queued). Bare-TTL cross-wire:
//   Pico GP20 (TX) -> SAMD21 D7 (RX),  Pico GP21 (RX) <- SAMD21 D6 (TX),  GND-GND.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "bus_phy.h"
#include "bus_frame.h"
#include "bus_addr.h"
#include "board.h"

#define SLAVE_ADDR 3u

int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("\n[bus2] BC -> slave %u  bare-TTL (TX GP%u / RX GP%u) @ %u baud\n",
           SLAVE_ADDR, (unsigned)BUS_PIN_DI, (unsigned)BUS_PIN_RO, (unsigned)BUS_DEFAULT_BAUD);
    bus_phy_init(BUS_DEFAULT_BAUD);

    bus_asm_t bc;
    bus_asm_init(&bc, BUS_ADDR_MASTER, true);   // promiscuous: capture any reply

    uint8_t seq = 0;
    for (uint32_t round = 0; ; round++) {
        bus_frame_t pf;
        memset(&pf, 0, sizeof pf);
        pf.dest = SLAVE_ADDR; pf.src = BUS_ADDR_MASTER;
        pf.type = BUS_FT_POLL; pf.seq = seq++; pf.len = 0;

        uint16_t words[BUS_FRAME_WORDS_MAX];
        uint16_t n = bus_frame_encode(words, &pf);
        bus_phy_rx_flush();
        bus_phy_send_words(words, n);

        // wait ~50 ms for the slave's reply
        bus_frame_t rf;
        int got = 0;
        uint16_t w;
        uint16_t raw[16]; int nr = 0;
        for (int t = 0; t < 1000 && !got; t++) {
            if (bus_phy_rx_pop(&w)) {
                if (nr < 16) raw[nr++] = w;
                if (bus_asm_feed(&bc, w, &rf)) got = 1;
            } else sleep_us(50);
        }

        if (got) {
            const char *tn = (rf.type & BUS_FT_MASK) == BUS_FT_NO_MESSAGE ? "NO_MESSAGE" :
                             (rf.type & BUS_FT_MASK) == BUS_FT_DATA       ? "DATA" :
                             (rf.type & BUS_FT_MASK) == BUS_FT_ACK        ? "ACK" : "?";
            printf("[bus2] round %u: REPLY src=%u dest=%u type=0x%02X(%s) len=%u  ok=%u crcfail=%u\n",
                   (unsigned)round, rf.src, rf.dest, rf.type, tn, rf.len,
                   (unsigned)bc.frames_ok, (unsigned)bc.crc_fail);
        } else {
            printf("[bus2] round %u: NO REPLY rx_words=%u crcfail=%u ovr=%u raw:",
                   (unsigned)round, (unsigned)bus_phy_rx_words(),
                   (unsigned)bc.crc_fail, (unsigned)bus_phy_rx_overrun());
            for (int i = 0; i < nr; i++) printf(" %03X%s", raw[i], (raw[i] & 0x100) ? "*" : "");
            printf("\n");
        }
        sleep_ms(1000);
    }
}
