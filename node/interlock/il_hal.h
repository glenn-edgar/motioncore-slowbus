// ============================================================================
// il_hal.h — pin HAL contract for the ported interlock framework (Thread 2).
//
// Same names/enums as the SAMD21 samd21_hal_pin.h so the vendored framework +
// DSL compile against it UNCHANGED. The RP2040 implementation (il_hal_rp2040.c)
// keeps the SAMD21 ownership-table semantics (single-owner inputs, OR-of-vetoes
// shared outputs) and swaps only the platform layer:
//   - phys_id == the RP2040 GPIO number (0..29); the SoC is flat (no port<<5|pin).
//   - reserved / capability checks consult the FROZEN hwio roles via the platform
//     seam (il_plat_*), so the interlock can only bind pins hwio actually exposes.
//   - ADC inputs READ THE SHARED DECIMATED AREA the ADC ISR already produces
//     (design: "interlock and telemetry read the same decimated outputs"), so the
//     interlock never runs its own SAR and never contends the bus.
//   - the veto output is the board's INTERLOCK_VETO_PIN (GP0).
// ============================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define HAL_PIN_TABLE_SIZE   30u    // RP2040 GPIO 0..29 (flat phys_id == gpio)
#define HAL_PIN_UNCLAIMED    0xFFu

// Per-slot bitmask (bit i = slot i). uint16_t supports up to 16 interlock slots
// (INTERLOCK_MAX_SLOTS); widened from uint8_t to allow 10 slots.
typedef uint16_t il_slotmask_t;

typedef enum {
    HAL_PIN_MODE_UNCLAIMED  = 0,
    HAL_PIN_MODE_GPIO_IN    = 1,
    HAL_PIN_MODE_GPIO_IN_PU = 2,
    HAL_PIN_MODE_GPIO_IN_PD = 3,
    HAL_PIN_MODE_GPIO_OUT   = 4,
    HAL_PIN_MODE_ADC_SCAN   = 5,
} hal_pin_mode_t;

typedef enum {
    HAL_PIN_CLAIM_OK             = 0,
    HAL_PIN_CLAIM_NO_SUCH_PIN    = 1,
    HAL_PIN_CLAIM_RESERVED       = 2,    // fixed-function (bus/I2C/UART) or not hwio-exposed
    HAL_PIN_CLAIM_TAKEN          = 3,    // input single-owner; or output held by a different mode
    HAL_PIN_CLAIM_CAP_MISSING    = 4,    // pin can't do the requested mode under the frozen hwio role
    HAL_PIN_CLAIM_BAD_MODE       = 5,
    HAL_PIN_CLAIM_VALUE_MISMATCH = 6,    // shared output declared with different ok/err values
} hal_pin_claim_status_t;

// Input claim (single-owner; idempotent re-claim by the same slot).
hal_pin_claim_status_t hal_pin_claim(uint8_t phys_id, uint8_t slot, hal_pin_mode_t mode);

// ADC-stream claim: records the channel for this pin; reads come from the shared
// decimated area (oversample_exp/sh_cyc retained for wire-compat, not re-sampled).
hal_pin_claim_status_t hal_pin_claim_adc(uint8_t phys_id, uint8_t slot,
                                         uint8_t oversample_exp, uint8_t sh_cyc);
uint16_t               hal_pin_read_adc(uint8_t phys_id);

// Output claim — shareable across slots iff (ok,err,open_drain) match (OR-of-vetoes
// drive). open_drain: 0 = push-pull, 1 = oc (drive-low / hi-Z-release, external pull-up),
// 2 = oc:up (oc + internal pull-up). For oc, value 1 = RELEASE (hi-Z), value 0 = DRIVE LOW.
hal_pin_claim_status_t hal_pin_claim_output(uint8_t phys_id, uint8_t slot,
                                            uint8_t ok_value, uint8_t err_value,
                                            uint8_t open_drain);

void           hal_pin_release_slot(uint8_t slot);
il_slotmask_t  hal_pin_get_owners(uint8_t phys_id);
hal_pin_mode_t hal_pin_get_mode  (uint8_t phys_id);
bool           hal_pin_check_consistency(void);
uint8_t        hal_pin_read    (uint8_t phys_id);
void           hal_pin_drive_outputs(il_slotmask_t veto_mask, il_slotmask_t managed_mask);

// ---- Platform seam (provided by the firmware, e.g. main.c) -----------------
// Keeps the HAL free of board.h / hwio internals so it stays unit-testable.

// True if GPIO `gpio` is a fixed-function / not-hwio-exposed pin the interlock
// must never bind (RS-485, I2C, UART, ADC pins for GPIO modes, etc.).
bool     il_plat_pin_reserved(uint8_t gpio);

// True if `gpio` can serve `mode` under the frozen hwio role (e.g. an input mode
// needs an hwio input role; output needs the veto pin / an hwio OUTPUT pin).
bool     il_plat_pin_cap(uint8_t gpio, hal_pin_mode_t mode);

// ADC channel (0..2) backing `gpio` (GP26/27/28), or 0xFF if not an ADC pin.
uint8_t  il_plat_adc_channel(uint8_t gpio);

// Latest decimated reading (0..4095) for ADC channel `ch` from the shared area.
uint16_t il_plat_adc_latest(uint8_t ch);
