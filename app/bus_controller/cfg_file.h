// ============================================================================
// cfg_file.h — read-only config-file seam (LittleFS-backed; impl deferred).
//
// The Pico's per-unit configuration lives in a small read-only filesystem in a
// reserved flash region, flashed SEPARATELY from the firmware image (the
// two-step flash). Files are tiny (<=256 B), named by a 4-char tag, CBOR-
// encoded. This one-shot load mirrors the SAMD21 register FILE bank
// (open -> size -> stream -> close) so the same readers work either side.
//
// The boot readers depend ONLY on cfg_load(); when the FS is designed, only
// this function gets a real body — the readers above it never change.
// ============================================================================
#pragma once

#include <stdint.h>

#define CFG_NAME_LEN  4
#define CFG_FILE_MAX  256

// Copy the named file into buf. Returns 0 and sets *out_len on success; <0 if
// absent / larger than cap / IO error.
int cfg_load(const char name[CFG_NAME_LEN], uint8_t *buf, uint32_t cap, uint32_t *out_len);
