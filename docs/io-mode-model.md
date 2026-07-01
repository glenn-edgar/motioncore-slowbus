# Pico I/O-mode model — node = ADC + one io_mode + interlock + chain-tree

Status: **DESIGN** (2026-07-01, not yet implemented). Target: `app/bus_controller/main.c` (shared
by RP2040 / RP2350 via `port/rp2040/board.h`), the `node/interlock/` engine, and the chain-tree
runtime. Read with `docs/three-thread-design.md`, `docs/interlock-port-map.md`, and the SAMD21
reference in `~/xiao_blocks` (`contract/samd21/registers.h`, `records.h`,
`firmware/samd21/device/samd21_commands.c`).

Supersedes the current free-mix `hwio` per-pad role model (`UNUSED/INPUT/IN_PU/IN_PD/OUTPUT/SERVO/
PULSE_COUNT`, any pad any role). **This changes the commission format** (`hwio`).

---

## 1. Why

The SAMD21 blocks (`~/xiao_blocks`) are **one dedicated chip per mode**: `MODE_GPIO`, `MODE_MIXED`
(GPIO + debounce + ADC), `MODE_COUNTER`, `MODE_SERVO`, `MODE_ADC`, `MODE_HIPERF`. Each exposes a
mode-specific I2C register bank. We want:

- **One host driver spanning both platforms** — same role/config encodings, same behaviors
  (debounce, counter read-clear, servo e-stop), whether the target is a SAMD21 register device or a
  Pico shell-exec node.
- **A cleaner node structure than "any pad any role."** The free-mix role model made the interlock
  operand set, the bench commands, and commissioning all ad-hoc.
- **The chain-tree app to drive/read the same I/O the bench does** — including async **I2C**.

The Pico has one structural advantage over the SAMD21: its ADC pins (GP26–28) are physically
separate from the I/O pins (GP2–9), so **ADC is always available alongside any I/O mode** — no pad
multiplexing. On the SAMD21 you needed `MODE_MIXED` to get GPIO+ADC on shared pads; on the Pico,
"ADC + debounced GPIO" is that, for free.

---

## 2. The node model

```
node =  ADC          always on   — limited SAMD21 analog model: 3ch (GP26/27/28), WIN_SEL tiers,
                                    per-window {seq,min,max,avg,AC-rms}.  (shipped: commit 0045904)
      + io_mode      exactly ONE of { GPIO(+per-pin debounce) | COUNTER | SERVO }, on GP2..GP9,
                                    FROZEN at commission (like the SAMD21 REG_MODE, set at idnt).
      + interlock    always on   — GP0/GP1 wired-OR veto, 10 equations (slot0 built-in GP1 +
                                    ilc1..ilc9), DNF DSL.
      + chain-tree   always on   — the app engine; can READ and DRIVE the whole bench surface,
                                    incl. async I2C, via the operate layer + reply-sink dispatch.
```

"Limited SAMD21 ADC" = the `MODE_ADC`/`MODE_MIXED` **stats** model (min/max/avg/AC-rms over
selectable windows). It does **not** include the SAMD21 `MODE_HIPERF` multi-band analysis or the DAC
two-tone generator; the Pico's GP22 PWM test source stands in for a signal generator.

`io_mode` is frozen at commission and is the whole GP2–9 block — no per-node mixing (matches the
SAMD21: a counter chip is all counters, a servo chip is all servos). Spare pads within a mode are
handled by that mode's sub-config (e.g. a COUNTER pin with `enable=0`), not by a second mode.

---

## 3. io_mode: GPIO (with per-pin debounce)

SAMD21 parity: `MODE_GPIO` + the debounce/raw/deb half of `MODE_MIXED`. Debounce is **folded into
GPIO mode** as a per-pin depth (0 = plain GPIO = `MODE_GPIO`; ≥2 = debounced = `MODE_MIXED`).

**Per-pin sub-config byte (frozen):** `(debounce_depth << 4) | role` — mirrors the SAMD21 `mxmp`
pad byte.
- `role` (low nibble): `0 UNUSED/SAFE · 1 IN · 2 IN_PU · 3 IN_PD · 4 OUT · 5 OC · 6 OC_PU`
  (0–4 already match the Pico's roles; `OC`/`OC_PU` are new — reuse the interlock HAL's open-drain).
- `debounce_depth` (high nibble, 0–15): N-consecutive-sample integrator with hysteresis (debounced
  →1 only when the last N samples are all 1, →0 when all 0, else hold). `depth<2` = passthrough.
  Sampled in `pulse_sample_1khz` (the existing 1 kHz GP2–9 sampler), reusing the interlock's
  shift-register debounce.

**Runtime:** `IPOL` input-polarity-invert as a settable bitmap register (SAMD21 `IPOL` is rw).

**Commands:**
- `GPIO_WRITE` — drive `OUT`/`OC`/`OC_PU` pins (honors open-drain + IPOL).
- `GPIO_READ` — one pin, raw (accepts UNUSED read-only).
- `GPIO_READ_ALL` — SAMD21 register block: `[iodir][gppu][gppd][od][ipol][raw][deb][olat]` (bit per
  pin), i.e. `GPIO`+`GPIO_DEB` plus config, in one read.

---

## 4. io_mode: COUNTER

SAMD21 parity: `MODE_COUNTER`. The Pico is already ~80% there (`pulse_sample_1khz` software edge
counting, `g_pulse_edge` uses the same rising/falling/both encoding).

**Per-pin config byte (frozen):** mirrors the SAMD21 `cntr` byte —
`bit0 enable | bits1-2 pull (0 none/1 up/2 down) | bits3-4 edge (0 rising/1 falling/2 both)`.

**Commands:**
- `PULSE_READ` — all channel counts `[u32]×8` (coherent snapshot).
- `PULSE_READCLR` — **read-and-clear** (SAMD21 `READCLR`).
- `PULSE_CLEAR` — `[mask]` clear selected.
- enable-bitmap readback.

---

## 5. io_mode: SERVO

SAMD21 parity: `MODE_SERVO`, including the **e-stop**. On the SAMD21, CH8/D6 is the shared
active-low interlock line; a low limps all servos and **latches** until clear+restart. On the Pico
the e-stop **IS the existing GP0/GP1 wired-OR interlock** — no separate pad.

**Per-channel sub-config (frozen):** enable bit; channels = GP2–9 (contiguous run, like the current
servo bank).

**Behavior:** on an interlock veto (GP1 low / global trip) the servo bank goes **limp + latches**;
clear + the interlock grace window re-enables (same clear path the wired-OR already uses).

**Commands:**
- `SERVO_SET` — per-channel `[ch][width_us]` (SAMD21 `CH_SEL`+`WIDTH`).
- `SERVO_SET_ALL` — `[width_us]×n` (existing).
- `SERVO_STOP` / start (master run gate).
- `SERVO_STATE` — `run / latched / veto` (SAMD21 `STATE`).

---

## 6. ADC (always on) — shipped

Reference: commit `0045904`. 3 channels (GP26/27/28), `CMD_ADC_READ` (1 kHz decimated + liveness),
`CMD_ADC_WIN_SEL` (tier 0/1/2 = 1 kHz/100 Hz/10 Hz), `CMD_ADC_STATS` (per-ch `[seq][min][max][avg]
[rms]`, AC-rms from raw accumulation). 48 kHz total / 16 kHz-per-ch, ISR on core1. No change here.

---

## 7. Interlock (always on) — 10 equations + ADC-window operands

10 slots: slot 0 built-in GP1 safety, `ilc1..ilc9` config equations (DNF DSL). Always on,
independent of io_mode.

**Operands, by availability:**
- Always: `GP1`, `ADC ch0..2` — now `{raw(1kHz), min, max, avg, rms}` per channel (extend the ADC
  input source from just `g_adc_latest[ch]`), virtual `_nodesdead`.
- GPIO mode: GP2–9 raw/debounced as watch inputs; GP2–9 `OUT/OC` as interlock outputs.
- COUNTER mode: counter value/rate as watch inputs.
- SERVO mode: the veto = the servo e-stop.
- Always output: `GP0` open-drain veto (wired-OR).

**ADC-window operands (new, in now):**
```
ilc1: watch[adc0.rms > 1800] -> out_err[gp0:0]     // AC-rms overcurrent
ilc2: watch[adc1.max > 4000] -> ...                // peak / clip trip
```

---

## 8. Operate layer + reply-sink dispatch (3 issuers)

Refactor so one operate layer serves **host, interlock, and chain-tree**:

```
   operate layer (sync, mode-gated) ......... gpio_rw/deb · adc_read/stats · counter_rw · servo
   async services (queued) .................. i2c (request carries a ROUTE)
                    ▲
   node_cmd_dispatch(cmd, args, REPLY-SINK)   sink = host up-queue | bus window | engine event
            ▲                 ▲                       ▲
       host (USB/bus)    interlock (reads)      chain-tree (issues cmds, incl. async I2C)
```

- **Sync ops** (GPIO/ADC/counter/servo): return inline; host packs the reply, engine writes the
  blackboard.
- **Async I2C**: enqueue with a **route** field (host vs engine) + correlation id; the service task
  routes the reply to that sink. Mode-gating lives in the operate layer (one place).

---

## 9. Chain-tree ↔ bench bridge

The app engine interacts with the bench surface two ways (mirroring how ADC already feeds the
blackboard today):

- **Read (conditions):** publish every bench input into the chain-tree blackboard at the decimation
  tiers — ADC already does (`g_bb_*`); extend to GPIO raw/debounced bitmaps and counter values. KB
  `watch`/condition leaves read them like any stream.
- **Write / issue (actions):** KB action leaves call the operate layer.
  - Sync ops → direct call, result to the blackboard.
  - **Async I2C** → enqueue with the **engine route**; the KB parks on the correlated reply
    (injected event / blackboard slot) — the same pattern `kbapp` already uses for bus-echo replies
    (`app_req` + route/bus_src). This is the reason the bridge is generic, not fixed leaves.

**I2C implication:** unify `I2C_SCAN/WRITE/READ` into the shared dispatch and give the **slave its
own `i2c_service_task`** (as we did for the servo feeder), so a slave's chain-tree can do I2C. Folds
into the "polled I2C" work.

---

## 10. Commission format (`hwio` v2)

```
hwio v2 = [schema_ver=2][io_mode][ pin_subconfig[8] ]
  io_mode        : 1 GPIO | 5 SERVO | ... (reuse SAMD21 MODE_* values where sensible)
  pin_subconfig  : GPIO    -> (debounce_depth<<4) | role   (role 0..6)
                   COUNTER -> bit0 enable | bits1-2 pull | bits3-4 edge
                   SERVO   -> bit0 enable (channel), contiguous from GP2
```

Mode + sub-config are frozen at commission (like the SAMD21 `REG_MODE`, writable only via `idnt`).
ADC + interlock config stay in their existing records (`ilcf` for interlock; ADC has no per-unit
config beyond WIN_SEL default). Bumping `hwio` schema_ver → old maps rejected (safe: all-zero =
GPIO/all-UNUSED). Discovery: `CMD_GPIO_ROLES` → `CMD_IO_MODE` (reports `[io_mode]` + the 8
sub-config bytes).

---

## 11. SAMD21 parity map

| Pico (this model) | SAMD21 |
|---|---|
| ADC always on | `MODE_ADC` stats (min/max/avg/AC-rms, WIN_SEL) |
| GPIO mode, debounce 0 | `MODE_GPIO` (IODIR/GPPU/GPPD/OD/IPOL/GPIO/OLAT) |
| GPIO mode, debounce ≥2 + ADC-always | `MODE_MIXED` (raw+debounced GPIO, ADC) |
| COUNTER mode | `MODE_COUNTER` (READ/READCLR/snapshot/CLEAR, pull/edge byte) |
| SERVO mode + GP0/GP1 e-stop | `MODE_SERVO` (CH_SEL/WIDTH/ENABLE/CTRL/STATE, D6 e-stop) |
| pin byte `(deb<<4)\|role` | `mxmp` pad byte |
| — (not ported) | `MODE_HIPERF`, DAC two-tone |

---

## 12. Build order

1. **io_mode model + `hwio` v2** — commission format + `CMD_IO_MODE` discovery (replaces
   `GPIO_ROLES`).
2. **Operate layer + reply-sink dispatch** — refactor `node_cmd_dispatch(…, sink)`; sync ops
   callable inline.
3. **Modes to parity** — GPIO (debounce/OC/IPOL/raw+deb/read-all), COUNTER (config byte/read-clear/
   snapshot/enable), SERVO (per-ch/enable/**e-stop tied to interlock**/STATE).
4. **ADC-window interlock operands** — extend the interlock ADC source to `{min,max,avg,rms}`.
5. **I2C unified + slave `i2c_service_task`** + the **engine route** on the request/reply.
6. **Chain-tree bench bridge** — KB action leaves (sync) + blackboard publish of GPIO/counter +
   routed async-I2C wait.

Steps 1–4 = the I/O core; 5–6 = the engine/I2C bridge. Each breaks into small, HW-verified commits.

---

## 13. Decisions locked (2026-07-01)

- Debounce folded into GPIO mode (per-pin depth, 0 = plain). One io_mode per node, frozen at
  commission. ADC + interlock always on.
- 10 interlock equations; **ADC-window operands (`min/max/avg/rms`) included now**.
- Chain-tree ↔ bench via a **generic reply-sink bridge** (not fixed leaves), driven by the async
  **I2C** requirement; I2C unified to the shared dispatch + slave service task + engine route.

## 14. Resolved (2026-07-01)

- **io_mode numeric values: reuse the SAMD21 `MODE_*` verbatim** (`IDLE=0, GPIO=1, ADC=2, MIXED=3,
  SERVO=4, COUNTER=5, HIPERF=6`) — one enum across both platforms. The Pico uses GPIO/COUNTER/SERVO
  (+ ADC always on); MIXED/HIPERF are SAMD21-only.
- **`IPOL` is runtime-settable** (a rw bitmap register), not frozen — matches the SAMD21; the
  commission pin byte stays `(debounce_depth<<4) | role`.
- **`hwio` bumps to `schema_ver = 2` and rejects v1** — both bench nodes are uncommissioned, so there
  is no back-compat to preserve; dual-format parsing is needless complexity.

Deferred:
- Counter "rate" (edges/window) as an interlock operand — start with raw-count thresholds + host
  differencing; add rate later if a real trip needs it.
- Opcode numbers for the new commands — assigned during implementation.
