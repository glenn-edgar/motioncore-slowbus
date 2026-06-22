// ============================================================================
// boot_identity.c — read & validate 'idnt' at boot.
//
// One generic image per CHIP, serving every variant it implements (the boot
// role — master vs slave — is chosen from this file's 'vr'). This file
// personalizes the unit (addr) and DOUBLE-CHECKS the config landed on the right
// silicon / a variant this image implements / the right physical board. chip /
// variant / uuid / addr mismatch is a MIS-FLASH — the caller hard-refuses (a
// unit must not run wearing the wrong identity). The refuse-vs-warn policy lives
// in the caller; this only reports a code.
//
// idnt shape:  { "v":1, "ch":<chip>, "vr":<variant>, "ad":<addr>, "id":h'..8..' }
// ============================================================================
#include "boot_identity.h"
#include "cfg_file.h"
#include "cbor_min.h"
#include "variants.h"
#include "bus_addr.h"

#define IDENT_SCHEMA_VER 1u

#if PICO_RP2350
  #define BUILD_CHIP CHIP_PICO2
#else
  #define BUILD_CHIP CHIP_PICO
#endif

int boot_read_identity(identity_t *out) {
    uint8_t buf[CFG_FILE_MAX]; uint32_t len;
    if (cfg_load("idnt", buf, sizeof buf, &len) < 0) return IDENT_ERR_MISSING;

    cbor_t c; cbor_init(&c, buf, len);
    uint64_t np; if (!cbor_open(&c, CBOR_MAP, &np)) return IDENT_ERR_FORMAT;

    uint64_t v = 0, ch = ~0ull, vr = ~0ull, ad = ~0ull;
    const uint8_t *uid = 0; uint32_t uidn = 0;
    enum { S_V = 1, S_CH = 2, S_VR = 4, S_AD = 8, S_ID = 16 }; uint8_t seen = 0;

    for (uint64_t i = 0; i < np; i++) {
        const uint8_t *k; uint32_t kn;
        if (!cbor_str(&c, CBOR_TEXT, &k, &kn)) return IDENT_ERR_FORMAT;
        if      (CBOR_KEY(k, kn, "v"))  { if (!cbor_uint(&c, &v))  return IDENT_ERR_FORMAT; seen |= S_V; }
        else if (CBOR_KEY(k, kn, "ch")) { if (!cbor_uint(&c, &ch)) return IDENT_ERR_FORMAT; seen |= S_CH; }
        else if (CBOR_KEY(k, kn, "vr")) { if (!cbor_uint(&c, &vr)) return IDENT_ERR_FORMAT; seen |= S_VR; }
        else if (CBOR_KEY(k, kn, "ad")) { if (!cbor_uint(&c, &ad)) return IDENT_ERR_FORMAT; seen |= S_AD; }
        else if (CBOR_KEY(k, kn, "id")) { if (!cbor_str(&c, CBOR_BYTES, &uid, &uidn)) return IDENT_ERR_FORMAT; seen |= S_ID; }
        else if (!cbor_skip(&c)) return IDENT_ERR_FORMAT;   // unknown key -> fwd-compat
    }
    if (seen != (S_V | S_CH | S_VR | S_AD | S_ID)) return IDENT_ERR_FORMAT;

    if (v  != IDENT_SCHEMA_VER) return IDENT_ERR_SCHEMA;    // contract guard
    if (ch != BUILD_CHIP)       return IDENT_ERR_CHIP;      // silicon family
    // ONE image serves multiple variants (role chosen at boot from 'vr'), so the
    // variant must be one THIS image implements — not a single BUILD_VARIANT.
    if (!variant_supported((uint8_t)vr)) return IDENT_ERR_VARIANT;

    pico_unique_board_id_t board; pico_get_unique_board_id(&board);   // this board
    if (uidn != PICO_UNIQUE_BOARD_ID_SIZE_BYTES || memcmp(uid, board.id, uidn))
        return IDENT_ERR_UUID;

    if (variant_is_master((uint8_t)vr)) {                  // role implied by variant
        if (ad != BUS_ADDR_MASTER) return IDENT_ERR_ADDR;
    } else if (ad < BUS_ADDR_SLAVE_MIN || ad > BUS_ADDR_SLAVE_MAX) {
        return IDENT_ERR_ADDR;
    }

    out->chip = (uint8_t)ch; out->variant = (uint8_t)vr; out->addr = (uint8_t)ad;
    memcpy(out->uuid, uid, uidn);
    return IDENT_OK;
}
