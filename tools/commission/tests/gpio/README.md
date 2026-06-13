# GPIO mode — serial test suite (and worked examples)

These `.py` files commission a SAMD21 GPIO config-chip and verify it over the
USB-CDC register bridge (`libcomm.Dongle`), hitting the exact register interface
the Pico drives over I2C. They double as **examples** of authoring a GPIO device
(`slave_dsl`) and exercising it.

## Run on the Pi

The register_dongle (XIAO, `2886:802f`) must be enumerated on `ssh robot`.

```bash
ssh robot
cd /tmp/commission/tests/gpio        # harness + tests; libcomm.py + slave_dsl.py in ../../
python3 run_gpio_tests.py            # non-interactive suite
python3 test_gpio_wired_or.py        # interactive: prompts for a D8<->D0 jumper
```

`run_gpio_tests.py` runs the non-interactive tests and prints a pass/fail summary.
The Pi's system `python3` has `pyserial` (no venv needed — these write raw bytes,
no CBOR). Staging from the repo: `scp tools/commission/{libcomm,slave_dsl}.py
robot:/tmp/commission/ && scp tools/commission/tests/gpio/*.py
robot:/tmp/commission/tests/gpio/`.

## What each test shows

| File | Covers |
|---|---|
| `gpio_harness.py` | shared: port discovery, `commission()` (offline→write→reboot), register map, `Checker` |
| `test_gpio_pinmap.py` | `gpmp` applied at boot: direction, pull up/down/none, output-init, open-drain mask; **config registers read-only** at runtime |
| `test_gpio_outputs.py` | output **values** (the only runtime-writable thing): push-pull drive + open-drain drive-low |
| `test_gpio_interlock.py` | interlock arm / cond-ok / **trip** / `INT_FLAGS` |
| `test_gpio_wired_or.py` | open-drain **wired-OR** with a manual jumper (D8 `oc` pulls a line read by D0 `in:up`) |

## Notes that bit us (so they don't bite you)

- **Commissioning does not delete files yet.** A no-interlock GPIO/MIXED unit
  therefore emits an **inert `ilcf` (`"off"`)** so it OVERWRITES any interlock left
  in flash by a previous commission — otherwise the old interlock re-arms at boot
  and can hold an output at its safe value (this exact bug made D8 read 0 even
  though it's unwired). The proper fix (file DELETE + commissioner reconciliation)
  is still a TODO for cross-mode stale files.
- **A wired output reads the bus, not its own drive** — a genuine gotcha on a
  populated board (an output tied to a circuit reads the bus level). If you wire
  an output in a test, relax its read assert to info.
- **The board is fixed**, so `IODIR`/`GPPU`/`GPPD`/`OD` are commission-static and
  **read-only** at runtime; only `OLAT`/`GPIO` (output values) and `IPOL` move.
  To change wiring config you change the DSL and re-commission (offline → write →
  reboot) — exactly what `commission()` does.
- Tests that need a specific input level use the pin's own pull (`in:up`/`in:down`)
  or a **manual jumper** (the `>>>` prompt) — matching how the hardware is tested.
