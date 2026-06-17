// ============================================================================
// cbor_selftest.c — host unit test for the minimal CBOR decoder (cbor_min.h).
//
// Header-only + pure C, so this builds and runs on the dev host (no Pico SDK).
// Focus: the multi-byte header decode (additional info n>=24), where a latent bug
// once made any value/length/count >= 24 decode wrong (200 -> 0x18C8 = 6344). idnt
// dodged it (all values < 24); slvr's poll period 200 surfaced it on the bench.
//
//   make -C tools cbortest
//   (or: cc -I../app/bus_controller cbor_selftest.c -o cbor && ./cbor)
// ============================================================================
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "cbor_min.h"

// Decode a single uint that must consume the whole buffer; return its value.
static uint64_t dec_uint(const uint8_t *b, uint32_t n) {
    cbor_t c; cbor_init(&c, b, n);
    uint64_t v = 0;
    assert(cbor_uint(&c, &v));
    assert(c.p == c.end);          // consumed exactly the header+payload
    return v;
}

#define UINT_IS(expect, ...) do {                          \
    static const uint8_t v_[] = { __VA_ARGS__ };           \
    assert(dec_uint(v_, (uint32_t)sizeof v_) == (expect)); \
} while (0)

int main(void) {
    // --- unsigned ints across every width class -----------------------------
    UINT_IS(0,          0x00);
    UINT_IS(23,         0x17);                       // last single-nibble value
    UINT_IS(24,         0x18, 0x18);                 // n=24 boundary (1 byte)
    UINT_IS(200,        0x18, 0xC8);                 // THE REGRESSION (was 6344)
    UINT_IS(255,        0x18, 0xFF);
    UINT_IS(256,        0x19, 0x01, 0x00);           // n=25 (2 bytes)
    UINT_IS(1000,       0x19, 0x03, 0xE8);
    UINT_IS(65535,      0x19, 0xFF, 0xFF);
    UINT_IS(0x12345678u,0x1A, 0x12, 0x34, 0x56, 0x78); // n=26 (4 bytes)
    UINT_IS(0x0100000000ull, 0x1B, 0,0,0,1, 0,0,0,0);  // n=27 (8 bytes)

    // --- byte/text string LENGTH also uses the header arg (n>=24 path) -------
    {
        uint8_t buf[2 + 24]; buf[0] = 0x58; buf[1] = 0x18;   // bstr, len=24
        for (int i = 0; i < 24; i++) buf[2 + i] = (uint8_t)i;
        cbor_t c; cbor_init(&c, buf, sizeof buf);
        const uint8_t *s; uint32_t n;
        assert(cbor_str(&c, CBOR_BYTES, &s, &n));
        assert(n == 24 && s == buf + 2 && c.p == c.end);
    }
    {
        const uint8_t t[] = { 0x65, 'h','e','l','l','o' };   // text "hello"
        cbor_t c; cbor_init(&c, t, sizeof t);
        const uint8_t *s; uint32_t n;
        assert(cbor_str(&c, CBOR_TEXT, &s, &n));
        assert(n == 5 && !memcmp(s, "hello", 5));
    }

    // --- array/map COUNT uses the header arg too (n>=24 path) ----------------
    { const uint8_t a[] = { 0x83 };       cbor_t c; cbor_init(&c, a, sizeof a); uint64_t k; assert(cbor_open(&c, CBOR_ARRAY, &k) && k == 3);  }
    { const uint8_t a[] = { 0x98, 0x1E }; cbor_t c; cbor_init(&c, a, sizeof a); uint64_t k; assert(cbor_open(&c, CBOR_ARRAY, &k) && k == 30); }
    { const uint8_t m[] = { 0xA2 };       cbor_t c; cbor_init(&c, m, sizeof m); uint64_t k; assert(cbor_open(&c, CBOR_MAP, &k)   && k == 2);  }

    // --- the exact shape that broke: { "p": 200, "v": 1 } -------------------
    {
        const uint8_t m[] = { 0xA2, 0x61,'p', 0x18,0xC8, 0x61,'v', 0x01 };
        cbor_t c; cbor_init(&c, m, sizeof m);
        uint64_t np; assert(cbor_open(&c, CBOR_MAP, &np) && np == 2);
        const uint8_t *k; uint32_t kn; uint64_t val;
        assert(cbor_str(&c, CBOR_TEXT, &k, &kn) && kn == 1 && k[0] == 'p');
        assert(cbor_uint(&c, &val) && val == 200);            // <-- not 6344
        assert(cbor_str(&c, CBOR_TEXT, &k, &kn) && kn == 1 && k[0] == 'v');
        assert(cbor_uint(&c, &val) && val == 1);
        assert(c.p == c.end);
    }

    // --- cbor_skip advances correctly across a multi-byte item --------------
    {
        const uint8_t s[] = { 0x19, 0x03, 0xE8, 0x07 };       // skip 1000, then read 7
        cbor_t c; cbor_init(&c, s, sizeof s);
        assert(cbor_skip(&c));
        uint64_t x; assert(cbor_uint(&c, &x) && x == 7 && c.p == c.end);
    }

    // --- negative cases: truncation and reserved additional-info ------------
    {
        const uint8_t t[] = { 0x19, 0x01 };                   // 2-byte uint, 1 byte short
        cbor_t c; cbor_init(&c, t, sizeof t);
        uint64_t v; assert(!cbor_uint(&c, &v));
    }
    {
        const uint8_t r[] = { 0x1C };                          // additional info 28 = reserved
        cbor_t c; cbor_init(&c, r, sizeof r);
        uint64_t v; assert(!cbor_uint(&c, &v));
    }
    {
        const uint8_t b[] = { 0x58, 0x05, 1, 2 };              // bstr len=5 but only 2 bytes present
        cbor_t c; cbor_init(&c, b, sizeof b);
        const uint8_t *s; uint32_t n; assert(!cbor_str(&c, CBOR_BYTES, &s, &n));
    }

    printf("PASS\n");
    return 0;
}
