// ============================================================================
// cfg_file.h — read-only config-file seam (SAMD21 boot-store format).
//
// The Pico's per-unit configuration lives in a small READ-ONLY region at the top
// of flash, flashed SEPARATELY from the firmware image (the two-step flash). The
// region holds SAMD21-boot-store-format entries (one 256-B row per file: magic,
// seq, 4-char name, len, CRC-8/AUTOSAR, <=240 B CBOR payload) — see cfg_file.c.
//
// We reuse that format's bytes + read scan, but NONE of its write machinery: the
// region is written by `picotool` while the app is stopped, so there are zero
// runtime flash writes and the RP2040/RP2350 XIP-vs-multicore hazard never
// arises. At runtime the region is XIP-mapped and `cfg_load` is pure pointer
// reads. Only this seam knows the format; the idnt/slvr readers above it don't.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define CFG_NAME_LEN  4
#define CFG_FILE_MAX  256   // caller buffer cap; on-flash payload max is 240 B

// Copy the named file into buf. Returns 0 and sets *out_len on success; <0 if
// absent (-1) / larger than cap (-2). A read-only XIP scan — never writes flash.
int cfg_load(const char name[CFG_NAME_LEN], uint8_t *buf, uint32_t cap, uint32_t *out_len);

// Boot-time guard: the firmware image must not extend into the config region.
// Pass &__flash_binary_end (SDK linker symbol). Returns true if the layout is
// sane (binary ends at or below the region base).
bool cfg_layout_ok(const void *flash_binary_end);
