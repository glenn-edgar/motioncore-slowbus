// ============================================================================
// frame_selftest.c — host unit test for the chip-independent codec.
//
// Builds and runs on the dev host (no Pico SDK), proving core/ is genuinely
// portable. Exercises the CRC reference vector and an encode -> assemble
// round-trip including dest filtering and a CRC-corruption drop.
//
//   make -C tools
//   (or: cc -I../core frame_selftest.c ../core/bus_crc8.c ../core/bus_frame.c -o st)
// ============================================================================
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "bus_crc8.h"
#include "bus_frame.h"

static int feed_all(bus_asm_t *a, const uint16_t *w, uint16_t n, bus_frame_t *out) {
    int got = 0;
    for (uint16_t i = 0; i < n; ++i) if (bus_asm_feed(a, w[i], out)) got++;
    return got;
}

int main(void) {
    // 1. CRC-8/AUTOSAR reference vector.
    assert(bus_crc8((const uint8_t *)"123456789", 9) == 0xDF);

    // 2. Encode -> assemble round-trip for a node addressed at 0x05.
    bus_frame_t tx = { .dest = 0x05, .src = BUS_ADDR_MASTER, .type = BUS_FT_DATA,
                       .seq = 7, .len = 4 };
    memcpy(tx.payload, "ping", 4);

    uint16_t words[BUS_FRAME_WORDS_MAX];
    uint16_t n = bus_frame_encode(words, &tx);
    assert(n == 1 + BUS_HEADER_LEN + 4 + 1);
    assert(words[0] == BUS_PREAMBLE_WORD);
    assert(BUS_WORD_IS_ADDR(words[1]) && (words[1] & 0x7F) == 0x05);

    bus_asm_t a; bus_frame_t rx;
    bus_asm_init(&a, 0x05, false);
    assert(feed_all(&a, words, n, &rx) == 1);
    assert(rx.dest == 0x05 && rx.src == 0 && rx.type == BUS_FT_DATA &&
           rx.seq == 7 && rx.len == 4 && memcmp(rx.payload, "ping", 4) == 0);

    // 3. A different node must NOT accept the same frame.
    bus_asm_t b; bus_asm_init(&b, 0x06, false);
    assert(feed_all(&b, words, n, &rx) == 0);

    // 4. Broadcast is accepted by anyone.
    bus_frame_t bc = { .dest = BUS_ADDR_BROADCAST, .src = BUS_ADDR_MASTER,
                       .type = BUS_FT_POLL, .seq = 0, .len = 0 };
    n = bus_frame_encode(words, &bc);
    bus_asm_t c; bus_asm_init(&c, 0x42, false);
    assert(feed_all(&c, words, n, &rx) == 1 && rx.type == BUS_FT_POLL);

    // 5. Corrupted CRC is dropped and counted.
    n = bus_frame_encode(words, &tx);
    words[n - 1] ^= 0xFF;              // flip the CRC byte (data, bit8 stays 0)
    bus_asm_t d; bus_asm_init(&d, 0x05, false);
    assert(feed_all(&d, words, n, &rx) == 0 && d.crc_fail == 1);

    printf("frame_selftest: all checks passed\n");
    return 0;
}
