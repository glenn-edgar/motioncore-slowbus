// ============================================================================
// il_hal_rp2040.c — RP2040 implementation of the interlock pin HAL.
//
// Ports the SAMD21 samd21_hal_pin.c ownership-table semantics verbatim (single-
// owner inputs, OR-of-vetoes shared outputs, idempotent re-claim) and swaps the
// platform layer for the RP2040 + the FROZEN hwio model:
//   - phys_id == GPIO number (0..29); the table is indexed by it directly.
//   - INPUT / ADC pins are already configured by hwio_apply() at boot — the
//     interlock only READS them, so an input claim does NOT touch pin config
//     (it validates the cap + records single-owner ownership). This keeps the
//     hardware frozen: the interlock can never reconfigure an hwio pin.
//   - OUTPUT pins (the veto GP0) ARE interlock-owned: claim configures them as
//     a driven output; release returns them to the OK level.
//   - ADC reads come from the shared decimated area (il_plat_adc_latest).
// ============================================================================
#include "il_hal.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include <string.h>

typedef struct {
    il_slotmask_t slot_mask;  // bit i = slot i shares this pin; 0 = unclaimed
    uint8_t mode;             // hal_pin_mode_t (uniform across sharers)
    uint8_t ok_value;         // output mode only
    uint8_t err_value;        // output mode only
    uint8_t adc_channel;      // ADC mode only (0..2), else 0xFF
    uint8_t open_drain;       // output mode: 0=push-pull, 1=oc, 2=oc:up
    uint8_t reserved[2];
} hal_pin_claim_record_t;

// Apply a logical value to a claimed GPIO output. Push-pull drives the level
// directly. Open-drain (oc / oc:up) realizes value 1 = RELEASE (hi-Z; an external —
// or internal, for oc:up — pull-up takes the line high) and value 0 = DRIVE LOW.
// This is what makes a wired-OR veto line work: many nodes share it, any one drives low.
static void hal_pin_apply_output(uint8_t id, uint8_t value);

static hal_pin_claim_record_t g_claims[HAL_PIN_TABLE_SIZE];
static bool                   g_claims_initialised = false;

static void ensure_initialised(void) {
    if (g_claims_initialised) return;
    for (uint8_t i = 0; i < HAL_PIN_TABLE_SIZE; i++) {
        g_claims[i].slot_mask   = 0;
        g_claims[i].mode        = HAL_PIN_MODE_UNCLAIMED;
        g_claims[i].ok_value    = 0;
        g_claims[i].err_value   = 0;
        g_claims[i].open_drain  = 0;
        g_claims[i].adc_channel = 0xFFu;
    }
    g_claims_initialised = true;
}

// ---- public API ----------------------------------------------------------

hal_pin_claim_status_t hal_pin_claim(uint8_t phys_id, uint8_t slot, hal_pin_mode_t mode) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return HAL_PIN_CLAIM_NO_SUCH_PIN;
    // Inputs use this API; outputs/ADC have dedicated claim calls.
    if (mode != HAL_PIN_MODE_GPIO_IN && mode != HAL_PIN_MODE_GPIO_IN_PU &&
        mode != HAL_PIN_MODE_GPIO_IN_PD)
        return HAL_PIN_CLAIM_BAD_MODE;

    if (il_plat_pin_reserved(phys_id))     return HAL_PIN_CLAIM_RESERVED;
    if (!il_plat_pin_cap(phys_id, mode))   return HAL_PIN_CLAIM_CAP_MISSING;

    const il_slotmask_t slot_bit = (il_slotmask_t)(1u << slot);
    // Inputs are single-owner; allow idempotent re-claim by the same slot.
    if (g_claims[phys_id].slot_mask != 0 && g_claims[phys_id].slot_mask != slot_bit)
        return HAL_PIN_CLAIM_TAKEN;

    // NB: no pin reconfiguration — hwio_apply() already set this input's
    // direction + pull at boot, and hardware is frozen at config time.
    g_claims[phys_id].slot_mask = slot_bit;
    g_claims[phys_id].mode      = (uint8_t)mode;
    g_claims[phys_id].ok_value  = 0;
    g_claims[phys_id].err_value = 0;
    return HAL_PIN_CLAIM_OK;
}

hal_pin_claim_status_t hal_pin_claim_adc(uint8_t phys_id, uint8_t slot,
                                         uint8_t oversample_exp, uint8_t sh_cyc) {
    (void)oversample_exp; (void)sh_cyc;   // the shared ADC ISR owns sampling cadence
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return HAL_PIN_CLAIM_NO_SUCH_PIN;

    uint8_t ch = il_plat_adc_channel(phys_id);
    if (ch == 0xFFu) return HAL_PIN_CLAIM_CAP_MISSING;
    if (il_plat_pin_reserved(phys_id)) return HAL_PIN_CLAIM_RESERVED;

    // ADC is a SHARED READ resource — any number of slots (and the bench /
    // telemetry) may watch the same channel. Reads come from the shared decimated
    // area, so there is no contention and no single-owner rule: just ADD this slot
    // to the sharer mask. (ADC pins are never GPIO-claimed — il_plat_pin_cap
    // rejects GPIO modes on GP26/27/28 — so there is no cross-mode conflict.)
    const il_slotmask_t slot_bit = (il_slotmask_t)(1u << slot);
    g_claims[phys_id].slot_mask  |= slot_bit;
    g_claims[phys_id].mode        = (uint8_t)HAL_PIN_MODE_ADC_SCAN;
    g_claims[phys_id].ok_value    = 0;
    g_claims[phys_id].err_value   = 0;
    g_claims[phys_id].adc_channel = ch;
    return HAL_PIN_CLAIM_OK;
}

uint16_t hal_pin_read_adc(uint8_t phys_id) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return 0;
    const hal_pin_claim_record_t* c = &g_claims[phys_id];
    if (c->mode != (uint8_t)HAL_PIN_MODE_ADC_SCAN || c->adc_channel == 0xFFu) return 0;
    return il_plat_adc_latest(c->adc_channel);   // shared decimated area; no private SAR
}

hal_pin_claim_status_t hal_pin_claim_output(uint8_t phys_id, uint8_t slot,
                                            uint8_t ok_value, uint8_t err_value,
                                            uint8_t open_drain) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return HAL_PIN_CLAIM_NO_SUCH_PIN;
    if (ok_value > 1u || err_value > 1u || open_drain > 2u) return HAL_PIN_CLAIM_BAD_MODE;
    if (ok_value == err_value)           return HAL_PIN_CLAIM_VALUE_MISMATCH;

    if (il_plat_pin_reserved(phys_id)) return HAL_PIN_CLAIM_RESERVED;
    if (!il_plat_pin_cap(phys_id, HAL_PIN_MODE_GPIO_OUT)) return HAL_PIN_CLAIM_CAP_MISSING;

    const il_slotmask_t slot_bit = (il_slotmask_t)(1u << slot);

    if (g_claims[phys_id].slot_mask != 0) {
        if (g_claims[phys_id].mode != HAL_PIN_MODE_GPIO_OUT) return HAL_PIN_CLAIM_TAKEN;
        if (g_claims[phys_id].ok_value   != ok_value ||
            g_claims[phys_id].err_value  != err_value ||
            g_claims[phys_id].open_drain != open_drain) return HAL_PIN_CLAIM_VALUE_MISMATCH;
        g_claims[phys_id].slot_mask |= slot_bit;   // share; idempotent (wired-OR drive)
        return HAL_PIN_CLAIM_OK;
    }

    // First claim — the interlock OWNS its outputs (the veto): configure + drive OK.
    gpio_init(phys_id);
    g_claims[phys_id].open_drain = open_drain;      // set before apply (apply reads it)
    g_claims[phys_id].ok_value   = ok_value;
    g_claims[phys_id].err_value  = err_value;
    hal_pin_apply_output(phys_id, ok_value);        // initial state = OK (push-pull level, or oc release/drive)
    g_claims[phys_id].slot_mask = slot_bit;
    g_claims[phys_id].mode      = (uint8_t)HAL_PIN_MODE_GPIO_OUT;
    return HAL_PIN_CLAIM_OK;
}

static void hal_pin_apply_output(uint8_t id, uint8_t value) {
    const hal_pin_claim_record_t *c = &g_claims[id];
    if (c->open_drain) {
        if (value) {                                // RELEASE -> hi-Z, pull-up takes the line high
            gpio_set_dir(id, false);
            if (c->open_drain == 2u) gpio_pull_up(id);       // oc:up: internal pull-up
            else                     gpio_disable_pulls(id); // oc:    external pull-up only
        } else {                                    // DRIVE LOW (assert the wired-OR veto)
            gpio_put(id, 0);
            gpio_set_dir(id, true);
        }
    } else {                                        // push-pull: drive the level directly
        gpio_put(id, value ? 1u : 0u);
        gpio_set_dir(id, true);
    }
}

void hal_pin_release_slot(uint8_t slot) {
    ensure_initialised();
    const il_slotmask_t slot_bit = (il_slotmask_t)(1u << slot);
    for (uint8_t id = 0; id < HAL_PIN_TABLE_SIZE; id++) {
        if ((g_claims[id].slot_mask & slot_bit) == 0u) continue;
        g_claims[id].slot_mask &= (il_slotmask_t)~slot_bit;
        if (g_claims[id].slot_mask == 0u) {
            // Only outputs were reconfigured by the HAL; return them to OK, then
            // hi-Z. Inputs/ADC were never touched (hwio owns them) — leave as-is.
            if (g_claims[id].mode == (uint8_t)HAL_PIN_MODE_GPIO_OUT) {
                gpio_set_dir(id, false);        // release to hi-Z (safe; pull-up/downstream decides)
                gpio_disable_pulls(id);
            }
            g_claims[id].mode        = HAL_PIN_MODE_UNCLAIMED;
            g_claims[id].ok_value    = 0;
            g_claims[id].err_value   = 0;
            g_claims[id].open_drain  = 0;
            g_claims[id].adc_channel = 0xFFu;
        }
    }
}

il_slotmask_t hal_pin_get_owners(uint8_t phys_id) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return 0;
    return g_claims[phys_id].slot_mask;
}

hal_pin_mode_t hal_pin_get_mode(uint8_t phys_id) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return HAL_PIN_MODE_UNCLAIMED;
    return (hal_pin_mode_t)g_claims[phys_id].mode;
}

bool hal_pin_check_consistency(void) {
    ensure_initialised();
    for (uint8_t id = 0; id < HAL_PIN_TABLE_SIZE; id++) {
        const hal_pin_claim_record_t* c = &g_claims[id];
        if (c->slot_mask == 0u) continue;
        // Outputs (OR-of-vetoes) and ADC (shared read) are legitimately multi-slot;
        // only the single-owner GPIO inputs must have exactly one owner.
        if (c->mode == (uint8_t)HAL_PIN_MODE_GPIO_OUT ||
            c->mode == (uint8_t)HAL_PIN_MODE_ADC_SCAN) continue;
        il_slotmask_t m = c->slot_mask; uint8_t bits = 0;
        while (m) { m &= (il_slotmask_t)(m - 1u); bits++; }       // popcount (no CLZ on M0+)
        if (bits > 1u) return false;
    }
    return true;
}

uint8_t hal_pin_read(uint8_t phys_id) {
    ensure_initialised();
    if (phys_id >= HAL_PIN_TABLE_SIZE) return 0;
    return gpio_get(phys_id) ? 1u : 0u;
}

void hal_pin_drive_outputs(il_slotmask_t veto_mask, il_slotmask_t managed_mask) {
    ensure_initialised();
    for (uint8_t id = 0; id < HAL_PIN_TABLE_SIZE; id++) {
        const hal_pin_claim_record_t* c = &g_claims[id];
        if (c->slot_mask == 0u) continue;
        if (c->mode != (uint8_t)HAL_PIN_MODE_GPIO_OUT) continue;
        if ((c->slot_mask & managed_mask) == 0u) continue;        // unmanaged slot drives itself
        uint8_t val = (c->slot_mask & veto_mask) ? c->err_value : c->ok_value;
        hal_pin_apply_output(id, val);   // push-pull level, or oc release(1)/drive-low(0)
    }
}
