// ============================================================================
// il_pin_table.h — RP2040 pin table for the interlock DSL (Thread 2).
//
// Replaces the SAMD21 samd21_pin_table.h. The DSL parser resolves a pin NAME in
// the flashed `ilcf` text to a phys_id (== the RP2040 GPIO number, flat) and, for
// ADC-stream watches, to an ADC channel. Naming for `ilcf`:
//   - "gp0".."gp29"  -> GPIO N           (digital in/out; veto = gp0)
//   - "adc0".."adc2" / "ain0".."ain2" -> GP26/27/28, ADC channel 0/1/2
// (case-insensitive prefix). Capability + reserved enforcement lives in the HAL
// claim (il_plat_pin_cap / il_plat_pin_reserved against the frozen hwio roles),
// so this table only does name -> {gpio, adc_channel} resolution.
// ============================================================================
#pragma once

#include <stdint.h>

#define BOARD_PIN_ADC_NONE   0xFFu

typedef struct {
    uint8_t gpio;          // RP2040 GPIO number (== phys_id)
    uint8_t adc_channel;   // 0..2 for GP26/27/28, else BOARD_PIN_ADC_NONE
} board_pin_t;

// phys_id is just the GPIO number on the (flat) RP2040.
static inline uint8_t board_pin_phys_id(const board_pin_t* bp) { return bp->gpio; }

// Resolve a DSL pin label (not NUL-terminated; `len` bytes) to a pin record.
// Returns NULL if the name doesn't match a known pin. The returned pointer is
// stable per-GPIO static storage (safe for the single-threaded parser).
const board_pin_t* board_pin_lookup(const char* label, uint8_t len);
