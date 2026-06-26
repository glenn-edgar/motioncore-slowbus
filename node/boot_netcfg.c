// ============================================================================
// boot_netcfg.c — read & validate 'neti' at boot (WiFi creds + agent endpoint).
// Pure parse/validate, mirroring boot_hwio.c / boot_identity.c. MISSING -> the
// all-zero fail-safe (present=0); a present-but-malformed file returns a negative
// code and policy (warn vs skip WiFi) lives in the caller.
// ============================================================================
#include "boot_netcfg.h"
#include "cfg_file.h"
#include "cbor_min.h"
#include <string.h>

#define NETI_SCHEMA_VER 1u

// Copy a CBOR text slice into a fixed NUL-terminated field, truncating to fit.
static void copy_text(char *dst, uint32_t cap, const uint8_t *s, uint32_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

int boot_read_netcfg(netcfg_t *out) {
    memset(out, 0, sizeof *out);   // fail-safe: present=0, empty ssid/pass, ip 0.0.0.0, port 0

    uint8_t buf[CFG_FILE_MAX]; uint32_t len;
    if (cfg_load("neti", buf, sizeof buf, &len) < 0) return NETI_ERR_MISSING;

    cbor_t c; cbor_init(&c, buf, len);
    uint64_t np; if (!cbor_open(&c, CBOR_MAP, &np)) return NETI_ERR_FORMAT;

    uint64_t v = 0; bool seen_v = false;
    for (uint64_t i = 0; i < np; i++) {
        const uint8_t *k; uint32_t kn;
        if (!cbor_str(&c, CBOR_TEXT, &k, &kn)) return NETI_ERR_FORMAT;
        if (CBOR_KEY(k, kn, "v")) {
            if (!cbor_uint(&c, &v)) return NETI_ERR_FORMAT;
            seen_v = true;
        } else if (CBOR_KEY(k, kn, "ss")) {
            const uint8_t *s; uint32_t sn;
            if (!cbor_str(&c, CBOR_TEXT, &s, &sn)) return NETI_ERR_FORMAT;
            copy_text(out->ssid, sizeof out->ssid, s, sn);
        } else if (CBOR_KEY(k, kn, "pw")) {
            const uint8_t *s; uint32_t sn;
            if (!cbor_str(&c, CBOR_TEXT, &s, &sn)) return NETI_ERR_FORMAT;
            copy_text(out->pass, sizeof out->pass, s, sn);
        } else if (CBOR_KEY(k, kn, "ip")) {
            const uint8_t *s; uint32_t sn;
            if (!cbor_str(&c, CBOR_BYTES, &s, &sn)) return NETI_ERR_FORMAT;
            if (sn != 4) return NETI_ERR_FORMAT;
            memcpy(out->ip, s, 4);
        } else if (CBOR_KEY(k, kn, "pt")) {
            uint64_t p; if (!cbor_uint(&c, &p)) return NETI_ERR_FORMAT;
            out->port = (uint16_t)p;
        } else if (CBOR_KEY(k, kn, "tp")) {
            uint64_t t; if (!cbor_uint(&c, &t)) return NETI_ERR_FORMAT;
            out->transport = (uint8_t)t;        // 0=UDP (default), 1=TCP
        } else if (!cbor_skip(&c)) {
            return NETI_ERR_FORMAT;                           // unknown key -> fwd-compat
        }
    }

    if (!seen_v)               return NETI_ERR_FORMAT;
    if (v != NETI_SCHEMA_VER)  return NETI_ERR_SCHEMA;
    if (out->ssid[0] == '\0')  return NETI_ERR_FORMAT;        // ssid is required
    out->present = 1;
    return NETI_OK;
}
