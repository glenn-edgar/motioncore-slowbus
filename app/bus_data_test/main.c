// app/bus_data_test/main.c — DATA path: BC sends CMD_ECHO to slave 3, gets the reply.
//
// Sequence (6b async slave): BC sends DATA [OP_SHELL_EXEC][req_id][cmd][args] ->
// slave ACKs (echoes req_id) -> BC POLLs -> slave ships
// [OP_SHELL_REPLY][req_id][status][result]. CMD_ECHO returns the args.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "bus_phy.h"
#include "bus_frame.h"
#include "bus_addr.h"
#include "board.h"

#define SLAVE          3u
#define OP_SHELL_EXEC  0x0109u
#define OP_SHELL_REPLY 0x0011u
#define CMD_ECHO       0x0001u

// Receive one CRC-valid frame within ~ms milliseconds. Returns 1 on success.
static int recv_frame(bus_asm_t *a, bus_frame_t *out, int ms) {
    uint16_t w;
    for (int t = 0; t < ms * 20; t++) {
        if (bus_phy_rx_pop(&w)) { if (bus_asm_feed(a, w, out)) return 1; }
        else sleep_us(50);
    }
    return 0;
}

static void send_frame(uint8_t dest, uint8_t type, uint8_t seq,
                       const uint8_t *payload, uint8_t len) {
    bus_frame_t f;
    memset(&f, 0, sizeof f);
    f.dest = dest; f.src = BUS_ADDR_MASTER; f.type = type; f.seq = seq; f.len = len;
    if (len) memcpy(f.payload, payload, len);
    uint16_t words[BUS_FRAME_WORDS_MAX];
    uint16_t n = bus_frame_encode(words, &f);
    bus_phy_rx_flush();
    bus_phy_send_words(words, n);
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("\n[busd] DATA path: BC -> slave %u CMD_ECHO  (TX GP%u / RX GP%u)\n",
           SLAVE, (unsigned)BUS_PIN_DI, (unsigned)BUS_PIN_RO);
    bus_phy_init(BUS_DEFAULT_BAUD);
    bus_asm_t bc;
    bus_asm_init(&bc, BUS_ADDR_MASTER, true);

    uint8_t seq = 0;
    uint16_t rid = 0;
    for (uint32_t round = 0; ; round++) {
        rid++;
        // DATA: [OP_SHELL_EXEC][req_id][CMD_ECHO][args="HI!"]
        uint8_t p[16]; uint8_t n = 0;
        p[n++] = OP_SHELL_EXEC & 0xFF; p[n++] = OP_SHELL_EXEC >> 8;
        p[n++] = rid & 0xFF;           p[n++] = rid >> 8;
        p[n++] = CMD_ECHO & 0xFF;      p[n++] = CMD_ECHO >> 8;
        p[n++] = 3; p[n++] = 0;        // echo args: len:u16 = 3
        p[n++] = 'H'; p[n++] = 'I'; p[n++] = '!';
        send_frame(SLAVE, BUS_FT_DATA, seq++, p, n);

        bus_frame_t rf;
        int gotack = recv_frame(&bc, &rf, 50) && ((rf.type & BUS_FT_MASK) == BUS_FT_ACK);

        // Poll a few times for the armed reply (slave executes in its main loop).
        int gotrep = 0; bus_frame_t pr;
        for (int k = 0; k < 6 && !gotrep; k++) {
            send_frame(SLAVE, BUS_FT_POLL, seq++, NULL, 0);
            if (recv_frame(&bc, &pr, 30) &&
                (pr.type & BUS_FT_MASK) == BUS_FT_DATA && pr.len >= 2 &&
                pr.payload[0] == (OP_SHELL_REPLY & 0xFF) &&
                pr.payload[1] == (OP_SHELL_REPLY >> 8)) {
                gotrep = 1;
            } else sleep_ms(3);
        }

        if (gotrep) {
            printf("[busd] round %u: ACK=%d REPLY len=%u  hex:", (unsigned)round, gotack, pr.len);
            for (int i = 0; i < pr.len; i++) printf(" %02X", pr.payload[i]);
            printf("  ascii:");
            for (int i = 0; i < pr.len; i++) printf("%c", (pr.payload[i] >= 32 && pr.payload[i] < 127) ? pr.payload[i] : '.');
            printf("\n");
        } else {
            printf("[busd] round %u: ACK=%d  no reply\n", (unsigned)round, gotack);
        }
        sleep_ms(1000);
    }
}
