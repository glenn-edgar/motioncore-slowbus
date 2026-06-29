// ============================================================================
// boot_netcfg.h — read the 'neti' config file at boot: WiFi credentials + the
// zenoh-agent endpoint the WiFi uplink dials. Sibling to 'idnt'/'hwio' in the
// read-only config-FS (one 256-B store row, CBOR body). Credentials live ONLY in
// the separately-flashed config UF2 (the "secondary flash"), never in the image
// or in git — built by tools/commission/lua/cfg_image.lua --ssid/--pass/...
//
// Pure parse/validate (no radio calls) — as testable as boot_identity.c. The
// caller (the dual-transport uplink supervisor) tries the credentials in list
// order, joins whichever AP is in range, then dials the (shared) agent ip:port.
//
// MISSING is benign (no WiFi config -> the node is USB-or-standalone only).
//
// neti shape — MULTIPLE credentials, ONE shared agent endpoint (§dual-transport):
//   v2: { "v":2, "ip":h'..4..', "pt":<u16>, "aps":[ {"ss":<text>,"pw":<text>}, ... ] }
//   v1 (legacy, single AP): { "v":1, "ss":<text>, "pw":<text>, "ip":h'..4..', "pt":<u16> }
//   ip = agent IPv4 (a.b.c.d); pt = agent UDP port. v1 parses into a 1-entry list.
// ============================================================================
#pragma once

#include <stdint.h>

#define NETI_SSID_MAX 33   // 32 chars + NUL
#define NETI_PASS_MAX 64   // 63 chars (WPA2 max) + NUL
#define NETI_AP_MAX    4   // max credentials in the list (the 240-B neti row bounds the real count)

enum {
    NETI_OK          =  0,
    NETI_ERR_MISSING = -1,   // no 'neti' file
    NETI_ERR_FORMAT  = -2,   // CBOR malformed / required field absent (ssid)
    NETI_ERR_SCHEMA  = -3,   // schema_ver mismatch
};

// One AP credential. The agent endpoint is shared (top-level), not per-AP.
typedef struct {
    char ssid[NETI_SSID_MAX];   // AP SSID (required)
    char pass[NETI_PASS_MAX];   // AP passphrase ("" = open)
} netap_t;

typedef struct {
    uint8_t  n_ap;                  // # credentials loaded (0 if none)
    uint8_t  present;               // 1 if >=1 valid AP credential
    uint8_t  ip[4];                 // shared zenoh-agent IPv4 (a.b.c.d); 0.0.0.0 if absent
    uint16_t port;                  // shared zenoh-agent UDP port; 0 if absent
    netap_t  ap[NETI_AP_MAX];       // credentials, in priority (list) order
} netcfg_t;

// Pure parse+validate of a 'neti' CBOR body (host-testable; no flash). NETI_OK on success.
int boot_parse_netcfg(const uint8_t *buf, uint32_t len, netcfg_t *out);
// Read 'neti' from the config-FS then parse. NETI_ERR_MISSING if absent.
int boot_read_netcfg(netcfg_t *out);
