// ============================================================================
// bus_crc8.h — CRC-8/AUTOSAR (poly 0x2F, init 0xFF, final XOR 0xFF).
//
// Chip-independent. Identical algorithm to the motioncore libcomm
// crc8_autosar so frames remain bit-compatible across the two projects.
// Reference vector: bus_crc8("123456789", 9) == 0xDF.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

uint8_t bus_crc8_update(uint8_t crc, uint8_t byte);   // incremental
uint8_t bus_crc8(const uint8_t *data, size_t len);    // one-shot (incl. final XOR)
