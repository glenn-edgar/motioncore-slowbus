// ============================================================================
// boot_netcfg.c — read & validate 'neti' at boot (WiFi creds + agent endpoint).
// §dual-transport: MULTIPLE credentials (a list), ONE shared agent endpoint. The
// pure parse (boot_parse_netcfg) is separated from the flash read so it is unit-
// testable on the host. v1 (single flat AP) parses into a 1-entry list.
// ============================================================================
#include "boot_netcfg.h"
#include "cfg_file.h"
#include "cbor_min.h"
#include <string.h>

#define NETI_SCHEMA_MIN 1u
#define NETI_SCHEMA_MAX 2u

// Copy a CBOR text slice into a fixed NUL-terminated field, truncating to fit.
static void copy_text(char *dst, uint32_t cap, const uint8_t *s, uint32_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

// Parse one {ss,pw} credential map into *ap. False on malformed CBOR.
static bool parse_ap(cbor_t *c, netap_t *ap) {
    uint64_t np;
    if (!cbor_open(c, CBOR_MAP, &np)) return false;
    for (uint64_t i = 0; i < np; i++) {
        const uint8_t *k; uint32_t kn;
        if (!cbor_str(c, CBOR_TEXT, &k, &kn)) return false;
        if (CBOR_KEY(k, kn, "ss")) {
            const uint8_t *s; uint32_t sn;
            if (!cbor_str(c, CBOR_TEXT, &s, &sn)) return false;
            copy_text(ap->ssid, sizeof ap->ssid, s, sn);
        } else if (CBOR_KEY(k, kn, "pw")) {
            const uint8_t *s; uint32_t sn;
            if (!cbor_str(c, CBOR_TEXT, &s, &sn)) return false;
            copy_text(ap->pass, sizeof ap->pass, s, sn);
        } else if (!cbor_skip(c)) return false;
    }
    return true;
}

int boot_parse_netcfg(const uint8_t *buf, uint32_t len, netcfg_t *out) {
    memset(out, 0, sizeof *out);   // fail-safe: present=0, no APs, ip 0.0.0.0, port 0

    cbor_t c; cbor_init(&c, buf, len);
    uint64_t np; if (!cbor_open(&c, CBOR_MAP, &np)) return NETI_ERR_FORMAT;

    uint64_t v = 0; bool seen_v = false;
    for (uint64_t i = 0; i < np; i++) {
        const uint8_t *k; uint32_t kn;
        if (!cbor_str(&c, CBOR_TEXT, &k, &kn)) return NETI_ERR_FORMAT;
        if (CBOR_KEY(k, kn, "v")) {
            if (!cbor_uint(&c, &v)) return NETI_ERR_FORMAT;
            seen_v = true;
        } else if (CBOR_KEY(k, kn, "ip")) {                 // shared agent IPv4
            const uint8_t *s; uint32_t sn;
            if (!cbor_str(&c, CBOR_BYTES, &s, &sn)) return NETI_ERR_FORMAT;
            if (sn != 4) return NETI_ERR_FORMAT;
            memcpy(out->ip, s, 4);
        } else if (CBOR_KEY(k, kn, "pt")) {                 // shared agent port
            uint64_t p; if (!cbor_uint(&c, &p)) return NETI_ERR_FORMAT;
            out->port = (uint16_t)p;
        } else if (CBOR_KEY(k, kn, "ss")) {                 // v1 flat ssid -> ap[0]
            const uint8_t *s; uint32_t sn;
            if (!cbor_str(&c, CBOR_TEXT, &s, &sn)) return NETI_ERR_FORMAT;
            copy_text(out->ap[0].ssid, sizeof out->ap[0].ssid, s, sn);
            if (out->n_ap < 1) out->n_ap = 1;
        } else if (CBOR_KEY(k, kn, "pw")) {                 // v1 flat pass -> ap[0]
            const uint8_t *s; uint32_t sn;
            if (!cbor_str(&c, CBOR_TEXT, &s, &sn)) return NETI_ERR_FORMAT;
            copy_text(out->ap[0].pass, sizeof out->ap[0].pass, s, sn);
            if (out->n_ap < 1) out->n_ap = 1;
        } else if (CBOR_KEY(k, kn, "aps")) {                // v2 credential list
            uint64_t na; if (!cbor_open(&c, CBOR_ARRAY, &na)) return NETI_ERR_FORMAT;
            for (uint64_t j = 0; j < na; j++) {
                if (j < NETI_AP_MAX) { if (!parse_ap(&c, &out->ap[j])) return NETI_ERR_FORMAT; }
                else                 { if (!cbor_skip(&c))            return NETI_ERR_FORMAT; }  // overflow
            }
            out->n_ap = (na > NETI_AP_MAX) ? NETI_AP_MAX : (uint8_t)na;
        } else if (!cbor_skip(&c)) {                        // unknown key -> fwd-compat
            return NETI_ERR_FORMAT;
        }
    }

    if (!seen_v)                                    return NETI_ERR_FORMAT;
    if (v < NETI_SCHEMA_MIN || v > NETI_SCHEMA_MAX) return NETI_ERR_SCHEMA;
    if (out->n_ap == 0 || out->ap[0].ssid[0] == '\0') return NETI_ERR_FORMAT;  // need >=1 SSID
    out->present = 1;
    return NETI_OK;
}

int boot_read_netcfg(netcfg_t *out) {
    memset(out, 0, sizeof *out);
    uint8_t buf[CFG_FILE_MAX]; uint32_t len;
    if (cfg_load("neti", buf, sizeof buf, &len) < 0) return NETI_ERR_MISSING;
    return boot_parse_netcfg(buf, len, out);
}
