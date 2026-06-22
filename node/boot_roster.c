// ============================================================================
// boot_roster.c — parse the 'slvr' roster config file. See boot_roster.h.
// ============================================================================
#include "boot_roster.h"
#include "cfg_file.h"
#include "cbor_min.h"

#define ROSTER_SCHEMA_VER 1u

int boot_read_roster(roster_cfg_t *out) {
    memset(out, 0, sizeof *out);

    uint8_t buf[CFG_FILE_MAX]; uint32_t len;
    if (cfg_load("slvr", buf, sizeof buf, &len) < 0) return ROSTER_ERR_MISSING;

    cbor_t c; cbor_init(&c, buf, len);
    uint64_t np; if (!cbor_open(&c, CBOR_MAP, &np)) return ROSTER_ERR_FORMAT;

    uint64_t v = 0, p = 0, w = 0, m = 0, r = 0; bool got_v = false;
    for (uint64_t i = 0; i < np; i++) {
        const uint8_t *k; uint32_t kn;
        if (!cbor_str(&c, CBOR_TEXT, &k, &kn)) return ROSTER_ERR_FORMAT;
        if      (CBOR_KEY(k, kn, "v")) { if (!cbor_uint(&c, &v)) return ROSTER_ERR_FORMAT; got_v = true; }
        else if (CBOR_KEY(k, kn, "p")) { if (!cbor_uint(&c, &p)) return ROSTER_ERR_FORMAT; }
        else if (CBOR_KEY(k, kn, "w")) { if (!cbor_uint(&c, &w)) return ROSTER_ERR_FORMAT; }
        else if (CBOR_KEY(k, kn, "m")) { if (!cbor_uint(&c, &m)) return ROSTER_ERR_FORMAT; }
        else if (CBOR_KEY(k, kn, "r")) { if (!cbor_uint(&c, &r)) return ROSTER_ERR_FORMAT; }
        else if (CBOR_KEY(k, kn, "s")) {
            uint64_t ns; if (!cbor_open(&c, CBOR_ARRAY, &ns)) return ROSTER_ERR_FORMAT;
            for (uint64_t j = 0; j < ns; j++) {
                uint64_t nf; if (!cbor_open(&c, CBOR_ARRAY, &nf) || nf < 3) return ROSTER_ERR_FORMAT;
                uint64_t addr, variant, flags;
                if (!cbor_uint(&c, &addr) || !cbor_uint(&c, &variant) || !cbor_uint(&c, &flags))
                    return ROSTER_ERR_FORMAT;
                for (uint64_t x = 3; x < nf; x++) if (!cbor_skip(&c)) return ROSTER_ERR_FORMAT;  // fwd-compat
                if (out->n < ROSTER_FILE_MAX) {
                    out->s[out->n].addr    = (uint8_t)addr;
                    out->s[out->n].variant = (uint8_t)variant;
                    out->s[out->n].flags   = (uint8_t)flags;
                    out->n++;
                }
            }
        }
        else if (!cbor_skip(&c)) return ROSTER_ERR_FORMAT;   // unknown key -> fwd-compat
    }
    if (!got_v)                  return ROSTER_ERR_FORMAT;
    if (v != ROSTER_SCHEMA_VER)  return ROSTER_ERR_SCHEMA;

    out->grant_period_ms = (uint16_t)p;
    out->window_us       = (uint16_t)w;
    out->max_misses      = (uint8_t)m;
    out->tcp_retries     = (uint8_t)r;
    return ROSTER_OK;
}
