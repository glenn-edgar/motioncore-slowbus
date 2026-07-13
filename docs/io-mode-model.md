# Pico I/O-mode model — node = ADC + one io_mode + chain-tree (KB-owned safety)

Status: **io_mode model (steps 1–6) SHIPPED + HW-verified (2026-07-08).** Target:
`app/bus_controller/main.c` (shared by RP2040 / RP2350 via `port/rp2040/board.h`) + the chain-tree
runtime. Read with `docs/three-thread-design.md` and the SAMD21 reference in `~/xiao_blocks`.

**★ Node redesign — DESIGN (2026-07-13). See §15 below. ★** Simplifies the RP2040/RP2350 node:
config moves to an **I2C EEPROM** (writable at runtime → commission over the RS-485 bus, no USB); the
**dedicated interlock engine is removed** — safety/interlock logic becomes **KB (chain-tree) code**,
using the Step-6 bench bridge; `idnt` gains a whole-set **`kb_id`** hard-checked at boot. §7 (interlock)
is **SUPERSEDED** by §15; §2's interlock line and §10's commission format are revised there.

Supersedes the original free-mix `hwio` per-pad role model. **This changes the commission format**.

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
      + chain-tree   always on   — the app engine (a compiled KB, selected/guarded by idnt.kb_id);
                                    READs bench inputs from the blackboard + DRIVEs outputs (incl. the
                                    GP0 veto) via the operate layer + reply-sink dispatch. OWNS safety
                                    (§15) — the interlock is now KB logic, not a separate engine.
```
**Redesign note (2026-07-13):** the old separate `interlock` layer is gone; safety is a KB rule with a
GP0 hardware fail-safe default + watchdog→veto backstop (§15). Config now lives in an I2C EEPROM (§15).

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

> **★ SUPERSEDED by §15 (Node redesign, 2026-07-13). ★** The dedicated interlock engine and the
> `ilc0..ilc9` config files are **removed**; safety/interlock logic is now KB (chain-tree) code. The
> operand set below (GP1, `adcN.{min,max,avg,rms}`, GP2–9, `_nodesdead`, GP0 veto) still describes what
> the KB reads from the blackboard / drives via the operate layer — only the *host* of the logic
> changed. Kept here as the operand reference; the "always-on separate engine" framing is obsolete.

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

---

## 15. Node redesign — EEPROM config + KB-owned safety + `kb_id` guard (DESIGN 2026-07-13)

Simplify the RP2040/RP2350 node and make it **field-commissionable over RS-485 without USB**. Three
coupled changes: config → I2C EEPROM; interlock engine → KB code; `idnt` gains a hard-checked `kb_id`.

### 15.1 Why
- **Reconfigure/commission over the bus, no USB at the node.** Internal-flash config can't be written
  at runtime safely (erasing a sector drops XIP for ~tens of ms → stalls core1, where the safety/ADC
  ISR live). An **external I2C EEPROM** is written by the async I2C service — core1 and the safety line
  are never touched — so config is writable live, and the whole `picotool` two-step BOOTSEL flash goes
  away for config (firmware still flashes normally).
- **One control layer.** The Step-6 bench bridge already lets a KB read every bench input (blackboard)
  and drive every output (operate layer, incl. the GP0 open-drain veto). So the dedicated interlock
  engine is redundant — fold its logic into the KB.

### 15.2 Config store → I2C EEPROM (system bus)
- **Dedicated system I2C bus = `i2c0` (GP20/21)** carries the config EEPROM (24C-series, 0x50–0x57;
  size per registry needs). Kept OFF the application bus `i2c1` (GP10/11) so identity bootstrap and
  user peripheral traffic don't contend. (This is the "two I2C models": **system** vs **application**.)
- **Seam unchanged, backing store swapped.** Keep `cfg_load(name,…)`; change `node/cfg_file.c` from an
  XIP pointer scan to an I2C read, and **add `cfg_save(name, cbor)`** (EEPROM write via the service task).
  The `idnt/hwio/neti/slvr` readers above the seam don't change.
- **Cache at boot.** EEPROM reads are ~ms I2C txns (vs instant XIP) → read the store into a small RAM
  cache once at boot; `cfg_load` scans RAM; `cfg_save` writes EEPROM **and** updates the cache.
- **Atomicity/wear.** Reuse the 256-B boot-store row format (magic/`seq`/name/len/CRC-8/CBOR) → last-
  writer-wins + torn-write detection for free; EEPROM endurance (~1M cyc) suits frequent writes.
- **Bootstrap ordering.** `i2c0` + the `idnt` read must run very early in `main()` (before role
  selection), with a safe fallback (quarantine, as today) if the EEPROM is absent/corrupt.

### 15.3 Over-bus commissioning
- `CFG_WRITE [name][cbor]` / `CFG_READ [name]` / `CFG_LIST` / `CFG_COMMIT`, dispatched through
  `node_cmd_dispatch` with a **`SINK_BUS`** reply. The EEPROM write is async (its own cfg/I2C service
  task) → returns `SHELL_ASYNC`, reply-when-done — never blocks the engine or safety. Reuses the
  reply-sink + async-service substrate from Steps 2/5/6.
- **Write-gate / commission lifecycle** (mirror SAMD21 `[[samd21-commission-lifecycle]]`): online-vs-
  offline gate so config can't be corrupted on a live machine; per-file **apply policy** — identity/
  io_mode/kb_id ⇒ reboot-to-apply; tunables ⇒ hot-apply.
- **Enrollment of a blank node** (zero-USB from blank) is a separate, larger piece: unconfigured node
  comes up at a default baud + commissioning address, master enumerates it **by UID** and writes `idnt`.
  **OPEN:** is first-commission USB-allowed (bench/manufacture) with the bus for field updates — or must
  a blank node be enrolled over the bus too? First-commission-USB-OK avoids the enrollment protocol.

### 15.4 Interlock removed → KB-owned safety
- The `node/interlock/` engine and `ilc0..ilc9` files are **deleted**. Safety/interlock is a **KB rule**:
  read `adc0_rms`/GP1/etc. from the blackboard → drive the GP0 veto via the operate layer. (The
  HW-verified ADC-rms overcurrent trip becomes exactly this rule.)
- **Fail-safe backstop stays in hardware/boot, NOT the KB** (this preserves the safety guarantee the
  separate task used to give):
  - GP0 defaults **veto-asserted** on reset/unpowered (open-drain wired-OR — already the case);
  - the **watchdog → veto**: an engine stall/crash drops the veto (KB alive ⇒ KB controls veto; KB dead
    ⇒ hardware safe).
- **Tradeoff (accepted):** safety now runs at the engine's priority / event-driven latency rather than
  the old dedicated prio-4 1 kHz task. Acceptable given the hardware backstop; note the loss of the
  separate-task determinism.

### 15.5 `idnt.kb_id` — whole-set, hard boot-check
- The KB is **compiled into the firmware** (not data-loaded) — deterministic, auditable, no interpreter
  surface. `kb_id` is a **cross-check, not a selector**, joining the existing mis-flash guards
  (`BUILD_VARIANT`, chip-id, UID).
- **Whole-set:** `kb_id` identifies the entire compiled KB set, as a **stable content hash of the
  canonical `kb0.json`** (semantics-based, INDEPENDENT of the codegen's per-run random symbol prefix —
  `gen_kb.sh` must emit a deterministic `KB_ID`/`KB_HASH`/`KB_VER` constant, not the random prefix).
- **Hard check:** firmware carries `{KB_ID,KB_HASH,KB_VER}`; `idnt` carries the expected `kb_id`
  (+ optional `kb_hash`); boot compares → **mismatch ⇒ `g_identity_refused` (quarantine) + GP0 veto
  asserted.** A mismatched node is *safe*, not just quiet.
- **Fleet audit:** extend the identity/`CMD_IO_MODE` report to return live `{kb_id,kb_hash,kb_ver}` so a
  master can verify every node runs the KB its `idnt` was commissioned for.

### 15.6 Config files after the redesign
| File | Holds | Role | Apply |
|---|---|---|---|
| `idnt` | UID + chip, variant, RS-485 addr/baud, **`kb_id` (+`kb_hash`)** | both | reboot |
| `hwio` | io_mode + 8 pin subconfig + ADC annotation | both | reboot |
| `neti` | WiFi APs + agent endpoint | master | hot |
| `slvr` | slave roster (addr/class/flags) | master | hot |

- **`ilc0..ilc9` REMOVED** (14 rows → 4). A pure **slave node record = `idnt` + `hwio`**; master adds
  `neti` + `slvr`. **OPEN:** keep the per-file 256-B rows, or store the node record as one atomic CBOR
  blob `{v, idnt, hwio}` (simplest for a slave, one seq/CRC)?

### 15.7 Open decisions
1. First-commission USB-OK (then bus updates) **vs** zero-USB enrollment from blank (§15.3).
2. EEPROM part/size (24C32 config-only … 24C256/512 if it also holds the app-bus device registry).
3. Per-file rows **vs** one atomic node-config CBOR blob (§15.6).
4. Whether `neti`/`slvr` live in the same EEPROM or stay in internal flash (master always has USB).

### 15.8 Build order (proposed)
1. `gen_kb.sh` emits deterministic `KB_ID/KB_HASH/KB_VER`; boot compares vs `idnt.kb_id` (hard,
   quarantine+veto). *(No EEPROM needed yet — hash lives in firmware, `kb_id` can start in flash `idnt`.)*
2. Port the safety interlock into a KB rule (GP0 veto from `adc0_rms`/GP1); add the GP0 boot fail-safe +
   watchdog→veto backstop; then **delete `node/interlock/` + `ilc*`**. HW-verify the ADC-rms trip parity.
3. `i2c0` system bus + EEPROM-backed `cfg_load`/`cfg_save` + boot RAM cache; keep flash as fallback.
4. `CFG_WRITE/READ/LIST/COMMIT` over the bus (SINK_BUS, async) + write-gate + per-file apply policy.
5. (If zero-USB) blank-node UID enrollment protocol.

### 15.9 I/O model revision — FULLY per-pin roles (decided 2026-07-13)

**Removes the block `io_mode` entirely.** There is no "one mode for the whole GP2–9 block" (§2/§3 obsolete)
and **no block modes at all** — **every pin 1–8 (= GP2..GP9) is individually assigned a role**, and *all*
functions, **including I2S and servo**, are expressed as per-pin roles. A node freely mixes them (e.g. pin
1 input, pin 2 counter, pin 3 servo, pins 4–6 the I2S mic, pins 7–8 output).

**Unified per-pin role set** (the `hwio` byte per pin; `io_mode` block selector is dropped — `hwio` becomes
just the 8 role bytes + ADC annotation):
`UNUSED · INPUT · IN_PU · IN_PD · OUTPUT · OC · OC_PU · COUNTER · SERVO · I2S_BCLK · I2S_WS · I2S_SD ·
NEOPIXEL · STEP · UART_TX · UART_RX · PWM_OUT · QUAD_A · QUAD_B` (input roles — incl. `QUAD_A`/`QUAD_B` —
keep the `(debounce<<4)|role` nibble). `hwio_apply` walks the 8 bytes, applies each pin's role, then
**assembles composite functions from the roles present**: pins tagged `I2S_*` → bring up one PIO I2S RX +
DMA on them; pins tagged `SERVO` → the servo feeder; `NEOPIXEL` pins → one WS2812 PIO SM each; `STEP` pins →
one step-pulse generator each; `UART_TX`/`UART_RX` pins → a PIO UART TX/RX SM each; `PWM_OUT` pins → a
hardware PWM slice/channel; a `QUAD_A`+`QUAD_B` pair → one debounced software quadrature channel; `COUNTER`
pins → the 1 kHz edge sampler.

**Implementation constraints to VALIDATE at commission (per-pin config is free; the silicon isn't):**
- **I2S** needs `I2S_BCLK` + `I2S_WS` + `I2S_SD` present as a set, and PIO side-set drives BCLK/WS so those
  two must be **adjacent** (SD is a separate `in` pin). The RP is I2S master (generates BCLK/WS, reads SD)
  for a mic. 1 PIO SM + 1 DMA; PCM → windowed stats/blackboard for the KB. (RP2350 preferred, PIO2 free.)
- **SERVO** per-pin: the current servo PIO frame drives a **contiguous** pin run from one base. Arbitrary
  non-contiguous servo pins ⇒ either keep servo pins contiguous, or spend one SM per servo pin (SM budget:
  RP2040 has ~6 free, RP2350 ~10 — see the PIO stock-take). Decide at implementation.
- **NeoPixel** (`NEOPIXEL`, WS2812/-B): single-pin **output**, ~800 kHz — 1 PIO SM per pin running the
  ~4-instr `ws2812` program, fed by FIFO (small strips) or DMA (larger). **No adjacency constraint** — the
  simplest composite role; only cost is 1 SM/pin (budget: RP2040 ~6 free, RP2350 ~10). Colors are pushed by
  an operate command (e.g. `NEOPIXEL_SET [pin][GRB…]`) → the KB or host drives status LEDs via the bench
  bridge. Output-only (a pin can't be `NEOPIXEL` + anything else).
- **Stepper (`STEP`), external driver = DRV8825 (step/dir, driver does the 1/32 microstepping):** the MCU
  provides only STEP + level lines, so it maps to per-pin roles — **`STEP`** = the pulse generator (1 PIO SM
  per pin, **leaning PIO** for accel ramps via FIFO/DMA inter-step delays; a PWM slice is the constant-
  velocity alternative that frees an SM). DIR / nENABLE / M0·M1·M2 (µstep select) / nSLEEP / nRESET →
  `OUTPUT`; nFAULT → `IN_PU`. Position tracked by counting generated steps; optional closed-loop via a
  `QUAD_A/QUAD_B` encoder input (`quadrature_encoder.pio` already in the tree). Motion is commanded by an
  operate command (`STEP_MOVE [pin][dir][steps][rate/accel]` or a velocity target) → the KB/host drives the
  motor via the bench bridge. **Pin budget:** minimum STEP+DIR = 2 pins/motor (M0–M2 jumpered for a fixed
  resolution); fully MCU-controlled (± EN/M0/M1/M2/FAULT) ≈ 7 pins → 1 motor. **OPEN:** MCU-direct coil
  microstepping (4-PWM sine, no driver chip) and multi-axis COORDINATED motion (interpolation) are separate,
  heavier layers — out of scope for the per-pin roles unless required.
- **UART (`UART_TX`, `UART_RX`):** a pin can be a UART output or a UART input. **PIO UART, any pin** — 1 SM
  per direction (reuses the RS-485 PHY's PIO-UART machinery; an 8-bit variant of the existing `uart_tx9`/
  `uart_rx9`). `UART_TX` = output, `UART_RX` = input; a TX pin + an RX pin form a full-duplex port (2 pins,
  2 SMs), or either alone is a one-way link. Needs a **baud** per port (companion config in `hwio`). Data
  flows through the operate layer + an async **uart service task** (RX ring buffer): `UART_WRITE [pin][bytes]`
  / `UART_READ [pin]` (or stream RX to the blackboard) — same async/reply-sink pattern as I2C, so the KB/host
  drives it via the bench bridge. SM cost 1/direction (budget: RP2040 ~6 free, RP2350 ~10). *Alt:* the 2
  hardware UART blocks (no SM cost) but pin-constrained (within GP2–9 only GP4/5/8/9 map to UART1) and only 2
  total — use for high-rate ports; PIO for arbitrary-pin flexibility.
- **PWM output (`PWM_OUT`):** general variable freq/duty output on any GP2–9 pin via the **hardware PWM
  peripheral — NO PIO SM cost** (the GP22 test-source machinery already exists; generalize `CMD_PWM_TEST`
  → per-pin `PWM_SET [pin][freq][duty]`, KB/host-driven). Costs **1 PWM slice-channel**. **Constraint:** each
  pin maps to a fixed slice/channel — GP2/3→slice1, GP4/5→slice2, GP6/7→slice3, GP8/9→slice4 (even pin=chan
  A, odd=chan B); **both channels of a slice SHARE frequency** (independent duty), so two `PWM_OUT` pins on
  the same pair must agree on frequency. Also note the **GP22 test source occupies slice3-A** (= GP6's
  slice/channel) — so `PWM_OUT` on GP6 collides and GP7 shares its frequency. Distinct from `STEP` (step
  pulse train) and `SERVO` (50 Hz RC pulse). For DC-motor speed etc., pair `PWM_OUT` (enable/PWM) with
  `OUTPUT` (direction) into an H-bridge driver.
- **Debounced quadrature counter (`QUAD_A`, `QUAD_B`):** for **mechanical flow meters / reed-relay
  quadrature contacts** that BOUNCE — a fast PIO decoder would miscount bounce, so this is a **software**
  decoder in the existing 1 kHz sampler: debounce A and B each (the same `(debounce<<4)|role` N-consecutive
  integrator as GPIO — set a heavy depth for relays, e.g. 4–15 = 4–15 ms), then run the quadrature Gray-code
  transition table on the *debounced* (A,B) → a **signed** count (direction = flow forward/reverse). A
  `QUAD_A` pin must be paired with a `QUAD_B` pin (both present); up to 4 channels (8 pins). **Zero PIO/DMA/
  PWM cost** (software sampler, like `COUNTER`). Read via `QUAD_READ`/`QUAD_READCLR [chan]` → signed count.
  **Rate:** debounce caps it (~`1000/depth` Hz/edge) — ample for flow meters (a few–tens of Hz); depth 0 =
  passthrough (~hundreds of Hz). *For a fast OPTICAL encoder* (closed-loop stepper) use the PIO decoder
  instead (`quadrature_encoder.pio`, 1 SM) — a separate high-speed variant, not this debounced one.
- **Counter** is already a per-pin software sampler → no constraint.

So: **config = one role enum, per pin, fully mixable**; the firmware derives I2S/servo/counter groups from
which pins carry which roles, and the commission tool rejects assignments the hardware can't honor
(missing/ non-adjacent I2S pins, over-budget servo SMs).

**Commission-time resource-budget check (per chip variant) — REQUIRED (2026-07-13).** Pins are cheap but
the PIO/DMA/hardware blocks aren't, and SM count is the binding constraint once several PIO roles are mixed.
Before accepting an `hwio` map the tool computes and validates the resource tally for the *target chip*:

- **PIO state machines** — sum SM cost of the mapped roles and require `≤ free SMs`:
  - fixed base: **RS-485 = 2** (always); **WiFi = 1** (master only).
  - per role: `I2S = 1`, `SERVO = 1` (contiguous bank) *or 1/pin non-contiguous*, `NEOPIXEL = 1/pin`,
    `STEP = 1/pin` (PIO) *or 0* (PWM-slice option), `UART_TX = 1`, `UART_RX = 1` (PIO) *or 0* (HW-UART option);
    `GPIO/IN/OUT/OC/COUNTER = 0` (direct GPIO / software sampler).
  - **totals:** RP2040 = 8 SM (2 blocks) → **~5 free master / ~6 slave**; RP2350 = 12 SM (3 blocks; PIO2
    always free) → **~9 free master / ~10 slave**.
- **PIO instruction memory** — ≤ 32 instr per block; sum the distinct programs used (RS-485 ~10, `ws2812`
  ~4, servo ~4, uart tx+rx, i2s, step) and check per-block fit, not just SM count.
- **DMA channels** — `I2S` (+ large NeoPixel/STEP) need a channel; require `≤ free` (RP2040 has 12).
- **Hardware blocks** — don't oversubscribe: **≤ 2 UART** (if any role picks HW-UART), **≤ 8 PWM slices**
  (`PWM_OUT`, STEP-via-PWM, and the GP22 test source all draw here — and honor the fixed pin→slice map +
  the shared-frequency-per-slice constraint above), and the two I2C buses (`i2c0` config/system, `i2c1`
  app) are system-reserved.

The tally depends on the PIO-vs-hardware choice per role (UART PIO/HW, STEP PIO/PWM), so those selections are
part of the `hwio` map. Over-budget ⇒ **reject at commission** (with the shortfall reported) — nudges
feature-dense nodes to the RP2350. This is a superset of the earlier "over-budget servo SMs" note.
