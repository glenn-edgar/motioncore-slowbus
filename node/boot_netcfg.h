// ============================================================================
// boot_netcfg.h — read the 'neti' config file at boot: WiFi credentials + the
// zenoh-agent endpoint the WiFi uplink dials. Sibling to 'idnt'/'hwio' in the
// read-only config-FS (one 256-B store row, CBOR body). Credentials live ONLY in
// the separately-flashed config UF2 (the "secondary flash"), never in the image
// or in git — built by tools/commission/lua/cfg_image.lua --ssid/--pass/...
//
// Pure parse/validate (no radio calls) — as testable as boot_identity.c. The
// caller (the WiFi uplink, UPLINK=wifi) joins the AP and dials ip:port.
//
// MISSING is benign for a USB-uplink build (no WiFi); a WiFi build with no 'neti'
// has nothing to join -> the uplink stays down (caller policy).
//
// neti shape:
//   { "v":1, "ss":<ssid text>, "pw":<pass text>, "ip":h'..4..', "pt":<port u16> }
//   ip = agent IPv4 as a 4-byte string (a.b.c.d); pt = agent TCP listen port.
// ============================================================================
#pragma once

#include <stdint.h>

#define NETI_SSID_MAX 33   // 32 chars + NUL
#define NETI_PASS_MAX 64   // 63 chars (WPA2 max) + NUL

enum {
    NETI_OK          =  0,
    NETI_ERR_MISSING = -1,   // no 'neti' file
    NETI_ERR_FORMAT  = -2,   // CBOR malformed / required field absent (ssid)
    NETI_ERR_SCHEMA  = -3,   // schema_ver mismatch
};

typedef struct {
    char     ssid[NETI_SSID_MAX];   // AP SSID (required)
    char     pass[NETI_PASS_MAX];   // AP passphrase ("" = open)
    uint8_t  ip[4];                 // zenoh-agent IPv4 (a.b.c.d); 0.0.0.0 if absent
    uint16_t port;                  // zenoh-agent TCP port; 0 if absent
    uint8_t  present;               // 1 if a valid 'neti' was loaded
} netcfg_t;

// Read+validate 'neti' into *out (zeroed first). NETI_OK on success (present=1).
int boot_read_netcfg(netcfg_t *out);
