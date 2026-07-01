// ============================================================================
// boot_hwio.c — read & validate 'hwio' at boot (the frozen HIL pin-role map +
// ADC channel annotation). Pure parse/validate: no hardware calls, so it stays
// as testable as boot_identity.c. The CALLER (main.c) applies the roles to the
// pins using the port's gpio/servo/pulse helpers — see hwio_apply().
//
// MISSING is benign: *out is pre-filled with the all-UNUSED / unlabeled fail-safe
// before the file is consulted, so a unit with no 'hwio' boots every HIL pin as a
// hi-Z input. A PRESENT-but-malformed file returns a negative code; policy (warn
// vs refuse) lives in the caller.
// ============================================================================
#include "boot_hwio.h"
#include "cfg_file.h"
#include "cbor_min.h"
#include <string.h>

#define HWIO_SCHEMA_VER 2u

// Copy a CBOR text slice into a fixed NUL-terminated field, truncating to fit.
static void copy_text(char *dst, uint32_t cap, const uint8_t *s, uint32_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

// Parse one {"l":..,"u":..,"n":..,"d":..} ADC entry. Every field optional.
static int parse_adc_entry(cbor_t *c, hwio_adc_t *a) {
    uint64_t np;
    if (!cbor_open(c, CBOR_MAP, &np)) return HWIO_ERR_FORMAT;
    for (uint64_t i = 0; i < np; i++) {
        const uint8_t *k; uint32_t kn;
        if (!cbor_str(c, CBOR_TEXT, &k, &kn)) return HWIO_ERR_FORMAT;
        if (CBOR_KEY(k, kn, "l")) {
            const uint8_t *s; uint32_t sn;
            if (!cbor_str(c, CBOR_TEXT, &s, &sn)) return HWIO_ERR_FORMAT;
            copy_text(a->label, sizeof a->label, s, sn);
        } else if (CBOR_KEY(k, kn, "u")) {
            const uint8_t *s; uint32_t sn;
            if (!cbor_str(c, CBOR_TEXT, &s, &sn)) return HWIO_ERR_FORMAT;
            copy_text(a->unit, sizeof a->unit, s, sn);
        } else if (CBOR_KEY(k, kn, "n")) {
            uint64_t v; if (!cbor_uint(c, &v)) return HWIO_ERR_FORMAT; a->scale_num = (uint32_t)v;
        } else if (CBOR_KEY(k, kn, "d")) {
            uint64_t v; if (!cbor_uint(c, &v)) return HWIO_ERR_FORMAT; a->scale_den = (uint32_t)v;
        } else if (!cbor_skip(c)) return HWIO_ERR_FORMAT;   // unknown -> fwd-compat
    }
    return HWIO_OK;
}

int boot_read_hwio(hwio_t *out) {
    // Fail-safe defaults first: GPIO mode, every HIL pin UNUSED (hi-Z), every ADC
    // channel unlabeled. A missing/all-zero map -> a GPIO node with inert pins.
    memset(out, 0, sizeof *out);   // pin[]=UNUSED; empty labels; 0/0 scale
    out->io_mode = HWIO_MODE_GPIO;

    uint8_t buf[CFG_FILE_MAX]; uint32_t len;
    if (cfg_load("hwio", buf, sizeof buf, &len) < 0) return HWIO_ERR_MISSING;

    // Parse into a temp; only commit to *out on full success, so a present-but-bad
    // or stale-schema (v1) file leaves *out at the safe GPIO/all-UNUSED default.
    hwio_t tmp; memset(&tmp, 0, sizeof tmp); tmp.io_mode = HWIO_MODE_GPIO;

    cbor_t c; cbor_init(&c, buf, len);
    uint64_t np; if (!cbor_open(&c, CBOR_MAP, &np)) return HWIO_ERR_FORMAT;

    uint64_t v = 0; bool seen_v = false;
    for (uint64_t i = 0; i < np; i++) {
        const uint8_t *k; uint32_t kn;
        if (!cbor_str(&c, CBOR_TEXT, &k, &kn)) return HWIO_ERR_FORMAT;
        if (CBOR_KEY(k, kn, "v")) {
            if (!cbor_uint(&c, &v)) return HWIO_ERR_FORMAT;
            seen_v = true;
        } else if (CBOR_KEY(k, kn, "m")) {
            uint64_t m; if (!cbor_uint(&c, &m)) return HWIO_ERR_FORMAT;
            if (m >= HWIO_MODE__MAX) return HWIO_ERR_ROLE;
            tmp.io_mode = (uint8_t)m;
        } else if (CBOR_KEY(k, kn, "io")) {
            uint64_t n; if (!cbor_open(&c, CBOR_ARRAY, &n)) return HWIO_ERR_FORMAT;
            if (n > HIL_GPIO_COUNT) return HWIO_ERR_ROLE;     // too many pins for the block
            for (uint64_t j = 0; j < n; j++) {                // raw sub-config byte; meaning per io_mode
                uint64_t b; if (!cbor_uint(&c, &b)) return HWIO_ERR_FORMAT;
                if (b > 0xFFu) return HWIO_ERR_ROLE;
                tmp.pin[j] = (uint8_t)b;
            }
        } else if (CBOR_KEY(k, kn, "ad")) {
            uint64_t n; if (!cbor_open(&c, CBOR_ARRAY, &n)) return HWIO_ERR_FORMAT;
            for (uint64_t j = 0; j < n; j++) {
                if (j < HWIO_ADC_NCH) {                        // store the first 3...
                    int rc = parse_adc_entry(&c, &tmp.adc[j]);
                    if (rc != HWIO_OK) return rc;
                } else if (!cbor_skip(&c)) {                   // ...consume any extras
                    return HWIO_ERR_FORMAT;
                }
            }
        } else if (!cbor_skip(&c)) {
            return HWIO_ERR_FORMAT;                           // unknown key -> fwd-compat
        }
    }

    if (!seen_v)              return HWIO_ERR_FORMAT;
    if (v != HWIO_SCHEMA_VER) return HWIO_ERR_SCHEMA;   // e.g. a stale v1 map -> reject, keep default
    *out = tmp;                                         // commit only on full success
    return HWIO_OK;
}
