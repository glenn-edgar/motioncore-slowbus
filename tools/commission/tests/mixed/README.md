# SAMD21 MIXED-mode serial tests

MIXED mode runs **one interlock that mixes ADC thresholds and (debounced) GPIO
levels** in a single `&& || ~` expression, sampled at ~100 Hz. These tests are
also worked examples of authoring + commissioning a MIXED unit.

## What's distinctive about MIXED

- **One expression, both signal kinds.** `A1 > 2.0V && D8` ANDs an ADC comparison
  with a GPIO level; the shared `il_parse` / `il_dnf_result` engine evaluates it
  (same DNF as GPIO/ADC modes).
- **GPIO inputs are debounced** (a shift register at the 100 Hz tick) — unlike
  **GPIO mode, which reads the raw pin with no debounce** (a deliberate
  asymmetry). The debounce interval is authored in the DSL: `D8='in:up:debounce_50ms'`
  → the firmware rounds it to a shift depth at the 10 ms tick (~20–150 ms range).
- **The DAC bench tool is available in MIXED**, so the `A0→A1` jumper is a
  controllable source for the ADC half — same as the ADC suite.

## How inputs are driven (jumperless except the one DAC jumper)

- **ADC half (A1):** the on-board DAC via the `A0→A1` jumper. `dac_dc(dg, level)`
  sets a DC level; A1 reads `~level*4` counts. Sweep it across the threshold
  (2.0 V ≈ 2482, 2.5 V ≈ 3102).
- **GPIO half (D8):** the input's commissioned **pull** — `in:up` reads 1,
  `in:down` reads 0. Re-commission to toggle it (the "change the DSL and re-run"
  pattern), which also re-settles the debounce on the fresh, stable level.

## Tests

`run_mixed_tests.py` → `test_mixed_interlock.py`:
1. **AND + ADC sweep** — `A1 > 2.0V && D8` (D8 up) → cond-ok tracks A1.
2. **GPIO veto** — same interlock, D8 down → the AND fails regardless of A1.
3. **OR + debounce** — `A1 > 2.5V || D8` with `debounce_50ms` → D8=1 holds the OR
   even with A1 low, and the debounced bitmap matches the pull.

The debounce *timing* can't be exercised jumperless (no glitching source); the
suite confirms the depth parses/arms and the debounced level tracks a stable
pull. Timing is covered by construction (the firmware shift register mirrors the
RS-485 framework's `eval_slot`, already validated).

## Staging

```
scp tools/commission/{libcomm,slave_dsl}.py robot:/tmp/commission/
scp tools/commission/tests/mixed/*.py        robot:/tmp/commission/tests/mixed/
```

Find the port via `libcomm.enumerate_dongles()` (the `ttyACM` number floats after
a Pi reset). See `../README.md` for the shared testing methodology.
