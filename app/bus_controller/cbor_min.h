// ============================================================================
// cbor_min.h — minimal read-only CBOR decoder for the boot config files.
//
// Supports only the subset the DSL emits: unsigned int, byte string, text
// string, array, map. No floats, tags, negatives, or indefinite lengths.
// Cursor-based and fully bounds-checked — a truncated/garbage file fails the
// decode rather than reading out of bounds.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef struct { const uint8_t *p, *end; } cbor_t;
enum { CBOR_UINT = 0, CBOR_BYTES = 2, CBOR_TEXT = 3, CBOR_ARRAY = 4, CBOR_MAP = 5 };

static inline void cbor_init(cbor_t *c, const uint8_t *b, uint32_t n) { c->p = b; c->end = b + n; }

// Read one item header: major type + argument (length / value / count). Advances
// past the header only. False on truncation or an unsupported encoding.
static inline bool cbor_hdr(cbor_t *c, int *major, uint64_t *arg) {
    if (c->p >= c->end) return false;
    uint8_t b = *c->p++; *major = b >> 5; uint8_t n = b & 0x1F;
    if (n >= 28) return false;                       // reserved / indefinite
    // n in 0..23 is the value itself; n in 24..27 means the value is the next
    // 1/2/4/8 bytes (n is only the length indicator, NOT part of the value).
    uint64_t v = (n < 24) ? n : 0;
    int nb = (n == 24) ? 1 : (n == 25) ? 2 : (n == 26) ? 4 : (n == 27) ? 8 : 0;
    for (int i = 0; i < nb; i++) { if (c->p >= c->end) return false; v = (v << 8) | *c->p++; }
    *arg = v; return true;
}
static inline bool cbor_uint(cbor_t *c, uint64_t *out) {
    int mj; uint64_t a; const uint8_t *s = c->p;
    if (!cbor_hdr(c, &mj, &a) || mj != CBOR_UINT) { c->p = s; return false; }
    *out = a; return true;
}
// Byte/text string -> pointer + length into the buffer (no copy).
static inline bool cbor_str(cbor_t *c, int want, const uint8_t **str, uint32_t *n) {
    int mj; uint64_t a; const uint8_t *s = c->p;
    if (!cbor_hdr(c, &mj, &a) || mj != want || c->p + a > c->end) { c->p = s; return false; }
    *str = c->p; *n = (uint32_t)a; c->p += a; return true;
}
// Open an array/map; returns element/pair count via *count.
static inline bool cbor_open(cbor_t *c, int want, uint64_t *count) {
    int mj; uint64_t a; const uint8_t *s = c->p;
    if (!cbor_hdr(c, &mj, &a) || mj != want) { c->p = s; return false; }
    *count = a; return true;
}
// Skip one complete item (recursively) — used to ignore unknown map keys.
static inline bool cbor_skip(cbor_t *c) {
    int mj; uint64_t a; if (!cbor_hdr(c, &mj, &a)) return false;
    switch (mj) {
        case CBOR_UINT: return true;
        case CBOR_BYTES: case CBOR_TEXT:
            if (c->p + a > c->end) return false;
            c->p += a; return true;
        case CBOR_ARRAY: for (uint64_t i = 0; i < a; i++)     if (!cbor_skip(c)) return false; return true;
        case CBOR_MAP:   for (uint64_t i = 0; i < 2 * a; i++) if (!cbor_skip(c)) return false; return true;
        default: return false;
    }
}
#define CBOR_KEY(k, kn, s) ((kn) == sizeof(s) - 1 && !memcmp((k), (s), (kn)))
