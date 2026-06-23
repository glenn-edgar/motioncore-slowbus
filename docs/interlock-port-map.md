# SAMD21 interlock → RP2040 Thread 2 port map

Source of truth for porting the proven SAMD21 interlock framework into the
RP2040 firmware as **Thread 2** (see `three-thread-design.md` §"Thread 2"). The
framework logic ports almost verbatim; only the HAL (pin/ADC), the persistence
backing, and the host-coupling surface change.

Source files (`~/xiao_blocks/firmware/samd21/device/`):
`samd21_interlocks.{c,h}`, `samd21_interlock_dsl.c`, `samd21_hal_pin.{h,c}`,
`samd21_adc.h`. Reference-only RP-native re-do: `firmware/rp2350/dsp/il_engine.{c,h}`.

## Public API (samd21_interlocks.h)
Lifecycle:
- `void interlock_boot_decide(void)` — cold/warm decision; bumps boot_counter for ARMED slots; POISONs a slot after >3 warm boots (bootloop guard).
- `void interlock_warm_restore(void)` — after peripheral init: re-parse DSL + re-claim pins for ARMED slots; clears runtime-only state.
- `void interlock_tick_all(void)` — the periodic pump: eval slots → build veto mask → drive outputs → update status buffer + emit events.

Slot admin:
- `uint8_t interlock_set_slot_dsl(uint8_t slot, const char* text, uint16_t len, uint8_t err_payload[3])`
- `uint8_t interlock_disarm_slot(uint8_t slot)`
- `uint8_t interlock_arm_slot_compiled(uint8_t slot, uint8_t id)` / `_noop(slot)` / `interlock_set_slot_tf(slot, tf)`

Status:
- `const il_status_buffer_t* interlock_get_status_buffer(void)` — 64-B snapshot built each tick.
- `const interlock_slot_persist_t* interlock_get_slot(uint8_t)`, `interlock_get_crash(void)`, `interlock_armed_count(void)`, `interlock_summary_flags(void)` (bit0 = any ARMED slot tripped).

Helpers / parser:
- `bool il_dnf_result(const il_inst_t*, const bool* wpass)` — OR-across-groups of AND-within-group.
- `il_parse_status_t il_parse(text, len, il_inst_t* out, uint16_t* err_off)` (GPIO mode) and `il_parse_adc(...)` (ADC-stream mode).

## Core data structures
Constants: `INTERLOCK_MAX_SLOTS=2`, `INTERLOCK_MAX_BOOT_ATTEMPTS=3`, `IL_MAX_INPUTS=7`,
`IL_MAX_WATCHES=8`, `IL_MAX_OUTPUTS=2`, `IL_NAME_MAX=16`, `IL_DSL_MAX=128`.

- `il_input_t` (8 B): `phys_id, mode, oversample_exp, sh_cyc, debounce_depth, reserved, hyst(u16)`.
- `il_watch_t` (8 B): `input_idx, op, threshold(u16), group, reserved[3]`.
- `il_output_t` (4 B): `phys_id, ok_value, err_value, open_drain`.
- `il_inst_t`: `name[16], input_count, watch_count, output_count, tf_state, inputs[7], watches[8], outputs[2]`.
- `interlock_persist_t` (in `.noinit`): `magic(0xCD51AC73), version(8), reserved, self_size, slots[2], crash, inst[2], dsl_len[2], dsl_text[2][128]`.
- `interlock_slot_persist_t` (4 B): `state(EMPTY/ARMED/POISONED), id(0=none,1=NOOP,2=DSL,3+=compiled), boot_counter, reserved`.

## tick_all() internals
Phase 1 (eval each ARMED slot): read each input (GPIO `hal_pin_read`, ADC
`hal_pin_read_adc`, VIRTUAL `read_virtual_input`); apply GPIO debounce shift-register;
for each watch compute hysteresis-relaxed threshold then `compare(v, teff, op)` over
{eq,ne,lt,gt,le,ge}; store per-watch last_pass; aggregate via `il_dnf_result`.
Phase 2 (drive): `veto_mask` = OR of ARMED slots with `tf==IL_TF_FALSE`; call
`hal_pin_drive_outputs(veto_mask, managed_mask=0x3)`.
Phase 3 (status/events): diff old vs new per slot; on transition bump seq + emit event;
refresh the 64-B `g_il_status_buffer`.

## HAL surface to re-implement for RP2040 (samd21_hal_pin.h / samd21_adc.h)
- `hal_pin_claim_status_t hal_pin_claim(phys_id, slot, mode)` — modes GPIO_IN/_IN_PU/_IN_PD.
- `uint8_t hal_pin_read(phys_id)` — 0/1.
- `hal_pin_claim_status_t hal_pin_claim_adc(phys_id, slot, oversample_exp, sh_cyc)`.
- `uint16_t hal_pin_read_adc(phys_id)` — 0..4095.
- `hal_pin_claim_status_t hal_pin_claim_output(phys_id, slot, ok_value, err_value)` — shareable iff ok/err match.
- `void hal_pin_drive_outputs(veto_mask, managed_mask)` — per output drive err_value if its slot bit ∈ veto_mask else ok_value.
- `void hal_pin_release_slot(slot)`.
- `uint16_t samd21_adc_read_oneshot(channel, oversample_exp, sh_cyc)`.

**RP2040 substitution:** the SAMD21 HAL does its own `samd21_adc_read_oneshot`. On
RP2040 the interlock should NOT own the ADC — per the ADC §, it READS the shared
decimated outputs (`g_adc_latest[]` fast tier / window stats) that the existing ADC
ISR already produces. So `hal_pin_read_adc(phys_id)` → look up the channel for that
pin and return the shared `latest[ch]` (or window stat per the watch's stat mode).
GPIO read/drive → RP2040 SIO. Pin identity: replace the SAMD21 `phys_id =
(port<<5)|pin` + board pin table with the RP2040 GPIO number directly. The interlock
veto output is the board's `INTERLOCK_VETO_PIN` (GP0).

## DSL (flashed as `ilcf`)
`name;cfg[(pin,...):mode,...];watch[pin:op:threshold,...];out_ok[...];out_err[...]`.
cfg modes: `in[,up|,down|,debounce_N]`, `adc[,oversample_N|,sh_N|,hyst_N]`,
`out|oc|oc,up`, virtuals `_uptime|_t_since_m2s|_stack_hwm`. watch ops eq/ne/lt/gt/le/ge;
`,`=AND within group, `|`=OR across groups (DNF). Parse-status enum has 27 codes.

## Will NOT port directly — replace
1. **PORT/PINCFG registers** → RP2040 SIO + pad ctrl.
2. **ADC subsystem** → read the shared decimated ADC area (do not run a private SAR).
3. **Reset/panic** (`NVIC_SystemReset`, `PM->RCAUSE`, SP read) → `watchdog_reboot` + RP2040 reset-cause; slow_bus already keeps a crash slot in `.uninitialized_data`.
4. **`.noinit` persistence** → RP2040 `.uninitialized_data` (already used for the crash slot) for `interlock_persist_t`.
5. **OP_POLL / host register surface** → the **shared-status coupling**: Thread 2
   *writes* a status word (read by Thread 1 / transport for notify) and *reads* a
   `rearm_request` flag (set by Thread 1 on a host re-arm; interlock verifies
   input-safe + dwell on its tick, then clears or refuses). No event queue on the
   safety path.

## Thread-2 wiring
- Own core (Core A) with the 1 kHz ADC decimation ISR; nothing else competes.
- `interlock_boot_decide()` once at startup → peripheral init → `interlock_warm_restore()`
  → loop calling `interlock_tick_all()` on a fixed tick (SAMD21 used ~250 ms; the fast
  ADC-threshold veto path is the ISR, so the tick cadence is the supervisory rate).
- `ilcf` (DSL text) + the I²C inventory become config-FS files (I²C input source +
  freshness fail-safe are added when the I²C service lands — deferred).

## Port staging in slow_bus (`node/interlock/`)
Module is built in isolation by the `il_thread2_check` object lib (CMake,
`-Werror`) so the main image stays green until the wire-in stage.

- **Stage 1 — pin HAL ✅ (2026-06-23).** `il_hal.h` + `il_hal_rp2040.c`
  (ownership-table port on RP2040 SIO + the `il_plat_*` seam). Compiles clean.
- **Stage 2 — vendored framework ✅ (2026-06-23).** `interlock.{c,h}` from
  `samd21_interlocks.{c,h}`; logic unchanged, platform swapped: `.noinit` →
  `.uninitialized_data`; `panic()` → `il_panic()` via `watchdog_reboot`;
  `NVIC_SystemReset`/`PM->RCAUSE`/HardFault override dropped (chassis owns faults);
  `emit_op_event` → shared-status stub (no queue on the safety path).
- **Stage 3 — DSL parser ✅ (2026-06-23).** `interlock_dsl.c` (includes-only swap)
  + `il_pin_table.{c,h}` resolving ilcf names `gp0..gp29` / `adc0..2` (== `ain0..2`)
  to phys_id (== GPIO) + ADC channel. Whole module compiles clean in `il_thread2_check`.
- **Stage 4 — config + wire-in ◐ software-complete (2026-06-23); HW-verify pending.**
  `ilcf` reader arms slot 0 from the flashed DSL text; the `il_plat_*` seam in main.c
  binds the HAL to `g_hwio_role` + the shared `g_adc_latest[]`; `interlock_boot_decide()`
  + `interlock_thread2_start()` (warm-restore / arm) run in `app_engine_task`; the
  `il_tick_task` ticks on core1; veto on GP0; module LINKED into `bus_controller`
  (builds clean). `cfg_image.lua --ilcf` flashes the DSL. **Still TODO:** verify
  trip→latch→manual-reset on the bench Picos; the hard ADC-ISR fast-veto path; final
  core affinity; role-agnostic placement (master-path only today, slave is a stub);
  feed the VIRTUAL-input externs from real sources.

### Key adaptation decisions (locked)
- **phys_id == GPIO number** (flat); HAL table size 30.
- **Frozen hardware:** the interlock READS hwio-configured input/ADC pins (no
  reconfigure on claim); only its OUTPUT (veto GP0) is interlock-owned + driven.
- **ADC from the shared decimated area** (`il_plat_adc_latest`), never a private SAR.
- **Persistence:** `interlock_persist_t` in `.uninitialized_data` (slow_bus already
  uses it for the crash slot) instead of SAMD21 `.noinit`.
- **Reset/panic:** `watchdog_reboot` + RP2040 reset-cause instead of `NVIC_SystemReset`/
  `PM->RCAUSE`.
- **Host coupling = shared status only** (no event queue touches the safety path).
- **I²C-mirror input + freshness fail-safe** arrive with the deferred I²C service;
  ADC/GPIO/virtual inputs do not depend on it.
