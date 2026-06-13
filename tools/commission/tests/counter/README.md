# SAMD21 COUNTER-mode serial tests

COUNTER mode = software edge counters on the 9 GPIO pads, sampled by a timer ISR.
Commissioned via a **`cntr` config file** authored in the DSL:

```python
Unit(0x55, 'COUNTER').counter(rate=1000).pins(D1='count:up:rising', D2='count:down:both')
```

Each `count` pad gets a pull (`up`/`down`/`none`) and edge (`rising`/`falling`/
`both`); `counter(rate=Hz)` sets the bank-global sample rate. Undeclared pads stay
free for the **bench tools** — the DAC lives on **A0 (= D0/CH0)**, available as a
square-wave stimulus whenever CH0 isn't a counter.

## Register interface (read ALL counters in one go)

| reg | |
|-----|---|
| 0x10 `READ` (w) | snapshot all counters into the shadow |
| 0x11 `READ_CLR` (w) | snapshot all + atomically zero all |
| 0x12 `DATA` (r) | stream the 36-byte shadow (9 × u32 LE), auto-incrementing |
| 0x13/14 `ENABLE` (r) | u16 enable bitmap |
| 0x1A `CLEAR` (w) | 0xFF → zero all |
| 0x20–0x2C | DAC bench tool (the A0 stimulus) |

## Driving a counter input (one jumper)

`A1 = AIN4 = D1` (same pad), so the ADC-suite's **A0→A1 jumper is already A0→D1**.
Leave D0 free, declare D1 as a counter, and the on-board DAC drives D1 through that
jumper. The DC self-test (`dac_dc` → 0 edges) proves D1 is genuinely driven, not a
floating pin picking up noise.

## ⚠️ Known limitation — slow/jittery timer clock

On this board the timer/CPU clock domain runs **~9× slow and jittery** (a
GCLK0/DFLL clock-init issue — `board_millis`, every TC timer, and the DAC output
frequency are all affected; only USB has a correct 48 MHz). So the **DAC's absolute
output frequency is currently unreliable**, and the commissioned `rate` (and the
ADC windows) are ~9× off in real time.

These tests are therefore **frequency-independent** — they check edge counting
*logic* (edges present vs. absent, read/read-clear semantics, enabled-only), not
absolute rates. Precise rate validation waits on the clock fix. (See the project
notes / git history for the full clock investigation.)

## Staging

```
scp tools/commission/{libcomm,slave_dsl}.py robot:/tmp/commission/
scp tools/commission/tests/counter/*.py     robot:/tmp/commission/tests/counter/
```
