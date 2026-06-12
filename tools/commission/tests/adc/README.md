# ADC mode — serial test suite (needs the A0→A1 jumper)

ADC mode is a free-running 8-channel sampler (tumbling-window min/max/avg/rms) +
a DAC + an interlock that compares per-watch **ADC streams** and drives D6. These
tests use the DAC as a controllable self-test source, so they need **one jumper**:

```
A0/DAC  --jumper-->  A1 (AIN4)
```

## Run on the Pi

```bash
ssh robot
cd /tmp/commission/tests/adc          # harness + tests; libcomm.py + slave_dsl.py in ../../
python3 run_adc_tests.py
```

| File | Covers |
|---|---|
| `adc_harness.py` | port discovery, `commission()`, DAC set, ADC read, trip-clear, `ILSTATE`, settle times |
| `test_adc_selftest.py` | DAC→ADC loopback: A1's avg tracks the DAC (validates the source) |
| `test_adc_interlock.py` | the interlock: stream selectors (instantaneous / `avg` / `max`) + DNF (AND band, OR groups); cond-ok flips at the threshold across a DAC sweep |

## Stream selectors (DSL)

```
A1                 instantaneous 16x sample
A1.avg.fast        10 Hz window average    (stat avg/min/max/rms, window fast/mid/slow)
A1.rms.mid  ...
```

## Gotchas baked in

- **Windowed stats settle on window-fill**, not instantly: fast (10 Hz/100 samples)
  ≈ 0.4 s, mid (1 Hz/1000) ≈ 3 s, slow (0.1 Hz/10000) ≈ 25 s. The harness has the
  settle times; reading too soon shows a stale/mid-flush value.
- The interlock **latches** on trip; the tests `clear_trip()` (write `INT_FLAGS`
  bit3) each step so `cond-ok` (the live comparison, `ILSTATE` bit1) is what they
  assert. `tripped` (bit0) is the latched safety state.
- `rms` reads ~0 for a constant DAC — it needs an **AC** source (DAC sine/square),
  which is the DAC waveform work, not covered here.
- D6 is the interlock output (read back via its own ADC channel AIN2 if needed);
  A0=DAC and A6/D6 are not watchable channels.
