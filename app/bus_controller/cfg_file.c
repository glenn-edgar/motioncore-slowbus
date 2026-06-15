// ============================================================================
// cfg_file.c — config-file seam, STUB body (Step 1).
//
// The LittleFS config region isn't built yet, so every file reads as ABSENT.
// Boot therefore degrades gracefully to the baked-in defaults until cfg_load()
// gets its real read-only LittleFS body (Step 2). Nothing above this seam — the
// idnt / slvr readers — changes when that lands.
// ============================================================================
#include "cfg_file.h"

int cfg_load(const char name[CFG_NAME_LEN], uint8_t *buf, uint32_t cap, uint32_t *out_len) {
    (void)name; (void)buf; (void)cap; (void)out_len;
    return -1;   // no config FS yet -> not found
}
