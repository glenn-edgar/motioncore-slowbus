# SAMD21 config-chip serial tests — methodology

How the per-mode test suites (`gpio/`, `adc/`, …) work, and the rule that shapes
them: **the hardware is location-fixed** — pin roles, directions, pulls, and
open-drain/DAC assignments are set once at commission and never change at
runtime. So tests don't poke configuration registers; they **re-commission**
(change the DSL → write files → reboot) and read back over the wire.

## Where tests run

On the Pi (`ssh robot`), where the register_dongle (XIAO, USB-CDC) enumerates as
`2886:802f`. Tests drive it through `libcomm.Dongle` — the USB→register bridge —
so they exercise the exact register interface the Pico uses over I2C. The Pi's
system `python3` has `pyserial`; commissioning writes raw bytes (no CBOR), so no
venv is needed. Stage with:

```
scp tools/commission/{libcomm,slave_dsl}.py robot:/tmp/commission/
scp tools/commission/tests/<mode>/*.py     robot:/tmp/commission/tests/<mode>/
```

Find the port via `libcomm.enumerate_dongles()` (VID 2886) — the `ttyACM` number
floats after a Pi reset, and a Pi reset wipes `/tmp` (re-stage).

## How we get deterministic inputs — without jumpers

Most checks need a known input level, and we get it **from the chip itself**, no
external wiring:

- **GPIO inputs**: the commissioned pull decides a floating input. `in:up` reads
  **1**, `in:down` reads **0**, `in:none` floats. To exercise an interlock across
  input combinations we **re-commission with different pulls** (e.g. `D0 && D2`
  with both `in:up` → condition true; re-commission `D0='in:down'` → false →
  trips). That's "change the DSL and re-run", using pulls instead of jumpers.
- **GPIO outputs**: drive via `OLAT`/`GPIO` (the one thing writable at runtime),
  read back on the same chip.
- **ADC inputs**: the on-board **DAC is a controllable source**. With a single
  `A0→A1` jumper, set the DAC level (`0x21`/`0x25`) and A1 reads it (~`level·4`
  counts). Sweep the DAC to move A1 across an interlock threshold and watch
  `cond-ok` flip — a closed-loop self-test.

## What genuinely needs a jumper

Only **physical pin-to-pin connectivity** — one pin's output actually reaching
another pin:

- **GPIO wired-OR** (`test_gpio_wired_or.py`): an `oc` open-drain output
  pulling a line read by an `in:up` input. Jumper `D8↔D0`.
- **ADC self-test** (`adc/`): the `A0→A1` DAC→ADC jumper (one jumper, used by the
  whole ADC suite).

Everything self-contained is automated; only these need your hand.

## Lessons baked into the suites

- **A wired output reads the bus, not its own drive.** On a populated board an
  output tied to a circuit reads the bus level — so `test_gpio_pinmap` asserts
  only pulled-*input* reads + config registers, and relaxes wired-output reads to
  info. (This once looked like a dead pin; it wasn't.)
- **Commissioning doesn't delete files yet**, so a no-interlock unit emits a null
  `ilcf` (`"off"`) to overwrite a stale interlock — otherwise the old one re-arms
  at boot and can hold an output at its safe value.
- **Windowed ADC stats settle on window-fill**, not instantly: fast ≈ 0.4 s, mid
  ≈ 3 s, slow ≈ 25 s. Reading too soon shows a mid-flush value.
- **Interlocks latch on trip**; tests `clear_trip()` (write `INT_FLAGS`) so the
  live `cond-ok` is what they assert, separate from the latched `tripped`.
