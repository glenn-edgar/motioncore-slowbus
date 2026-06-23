# Three-thread firmware architecture (Pico node)

**Status: design agreed 2026-06-23 (chat-mode with Glenn).** This is the current
firmware architecture. It **supersedes** the KB0–KB4-as-chain-KBs decomposition in
`core1-design.md` for the **interlock / bench / application split**. The lower-level
mechanisms in `core1-design.md` (the chain-tree engine, the monitor report set, the
single-owner I²C service with completion, the 64-bit status word) still inform
Threads 1 and 3 — read it as background.

---

## Governing principle — hardware is FROZEN at config time

All hardware *setup* — pin roles, ADC channels, the interlock veto-pin / input
bindings, and the I²C device inventory — is fixed by the **flashed config at boot**
and **cannot be changed at runtime**. Runtime commands **operate** hardware; they
never **reconfigure** it.

Config lives in the read-only config-FS, applied **once** at boot:
- `idnt` — identity (chip, variant→role, address, bus speed). *(exists)*
- `hwio` — pin-role map + ADC channel map. *(to build)*
- `ilcf` — the interlock DSL (the SAMD21 interlock text). *(port)*
- the **I²C device inventory** — which devices to periodically sample. *(to build)*

Consequence: the old runtime `GPIO_CONFIG` and servo-mode-switch commands are
**removed** — pin roles come from `hwio`.

---

## The three threads

### Thread 1 — I/O router + bench surface
- The **single** place that touches wire formats. Ingests messages from the **bus,
  USB, and bench**; converts each to an internal event.
- **Handles the bench surface itself** — monitor (system health/telemetry),
  HIL-**operate** (GPIO read/write, ADC read, pulse-count, I²C read/write/scan —
  operate-only, **validated against the `hwio` pin roles**), and commission.
  (PWM + quadrature were dropped to the Pico2/RP2350; GP14/17/18 are now spare.)
- **Routes only non-bench (application) messages** to the chain-tree as events.
- **Role-agnostic** (master/slave identical) — the role difference lives in the
  transport, not here.

### Thread 2 — interlock (the proven SAMD21 design, ported)
- **Tick-driven, autonomous; reads only local I/O; drives the hard veto on a LOCAL
  GPIO.** 2 slots, **DSL-configured** (flashed `ilcf`).
- Inputs: local **ADC/GPIO** (fast — the 1-clock veto path via the ADC ISR) **plus
  the I²C shared mirror** (slow/supervisory) with a **freshness fail-safe** (a stale
  or failed sample ⇒ trip; "can't confirm safe ⇒ assume unsafe").
- **Coupling to the rest of the system is SHARED STATUS ONLY.** It *writes* a
  trip/status word (read by Thread 1 / the transport POLL for notification) and
  *reads* a `rearm_request` flag (set by Thread 1 on a host re-arm; the interlock
  verifies input-safe + dwell on its tick → clear, or refuse). **No event queue
  touches the safety path.**
- Latch / trip / manual-reset semantics, OR-of-vetoes voting, `.noinit` persistence,
  and the 3-attempt boot-loop guard all come from the SAMD21 framework.

### Thread 3 — chain-tree application engine
- The flexible **application logic** — the chain-tree IR **baked into the build**
  (NOT flashed config; see *Build & identity model*). Consumes non-bench events from
  Thread 1; **emits RS-485 messages to the transport.**
- Engine-health-dependent logic is acceptable here — it is **not** the safety path.

### Transport — separate layer below Thread 1 (unchanged, not changing)
- Owns the PHY single-handed + the RX IRQ ring; handles all wire timing: master poll
  sweep, slave window-reply, USB framing. The **master/slave role difference is
  contained here.** Threads 1/2/3 sit above it and are role-agnostic.

---

## Event plumbing
- **One inbound queue per consumer thread.** Thread 1 routes by message
  type/destination → the engine queue (Thread 3). (No head-of-line blocking between
  consumers; each thread drains its own.)
- **Uniform event type, tagged** with origin + reply handle
  `{source: bus|usb|bench, reply_addr, req_id}`. Consumer logic is **origin-agnostic**;
  only the reply path reads the tag.
- **Symmetric up-path:** consumers emit tagged reply/notify events; Thread 1 routes
  them back out (USB `s2m` / bus inject / slave window-reply).
- The **interlock is OUTSIDE this** — shared status only (above).

---

## I²C — TWO buses: a POLLED bus and an ASYNC bus
Decision 2026-06-23: split I²C onto **two physical buses** so safety sampling is never
contended by ad-hoc traffic.
- **POLLED bus — `i2c0` on GP20/21.** The service **periodically samples** a configured
  device set (e.g. INA219 current/power) and **publishes to a shared area**. This bus is
  **dedicated** — nothing else touches it — so the interlock's inputs are deterministic.
- **ASYNC bus — `i2c1` on GP10/11.** Bench + chain-tree **one-off** read/write requests.
  This is the existing HIL I²C bus.
- **All consumers READ the shared area** for sampled values — the interlock gets its I²C
  inputs as **shared status**: tick-only, never blocks on the bus.
- Because the buses are physically separate, the earlier "async could delay a sample past
  its freshness deadline" worry is **eliminated** — the polled cadence stands alone.
- Rules:
  1. **Single writer** (the service) to the shared area; lock-free readers; **per-record
     seqlock / double-buffer** so a reader never sees a half-updated multi-field sample.
  2. Each value carries a **timestamp/generation + valid bit** → drives the interlock
     freshness fail-safe (stale/failed read = fault = trip).
  3. The **polled device set + cadence + the inventory for both buses = frozen config.**
  4. The service (both buses) runs on the **non-interlock core** (it blocks on I²C); the
     interlock only **reads** the shared area.
- **Veto stays on GPIO (GP0)** — I²C is strictly for **sensed inputs**, never the veto.

---

## ADC — three GENERAL channels, dual-tier
- **3 channels (GP26/27/28), one SAR round-robin, single ISR writer** (lock-free readers).
- **All three are general-purpose** — none firmware-reserved; both the interlock and
  telemetry read the same decimated outputs (read-only, no contention).
- **Two tiers:**
  - **Stage A — ~1 kHz `latest[3]`** (boxcar mean): the FAST tier, the interlock's
    hard-threshold input (the "1-clock veto" path).
  - **Stage B — 10 Hz windows: mean / max / RMS** per channel (100 ms windows; RMS = the
    AC component with DC removed — for CT/transformer AC current; ~440 Hz bandwidth).
    Feeds telemetry + `CMD_ADC_STATS`.
- **Per-channel meaning = `hwio`** (frozen config): a label + scaling/units per deployment.
- **Per-watch channel + stat mode = `ilcf`**: `now` (1 kHz latest — fast threshold) vs
  `avg/min/max/rms` (10 Hz window — e.g. overcurrent on RMS).

## Core mapping (RP2040 dual-core SMP)
- **Core A:** interlock thread **+ the 1 kHz ADC decimation ISR** — deterministic,
  isolated, nothing else competes.
- **Core B:** chain-tree engine + Thread 1 (router) + the I²C service.
- Final affinities and the slave-responder's window timing → a dedicated
  **thread-review pass** (deferred).

---

## The SAMD21 interlock source (port target) — in `~/xiao_blocks`
| file | what |
|---|---|
| `firmware/samd21/device/samd21_interlocks.{c,h}` (924+447) | the framework: 2 slots, `.noinit` persistence, DSL instance (inputs/watches/outputs), OR-of-vetoes voting, latch, crash-context, 3-attempt boot-loop guard |
| `firmware/samd21/device/samd21_interlock_dsl.c` (634) | on-device DSL parser |
| `host/commission/lua/{slave_dsl.lua, dsl_compile.lua, dsl_regress.lua}` | host DSL tooling + regression |
| `host/test/mixed_interlock*.lua` | example DSL + the trip→latch→manual-reset HW test |
| `firmware/rp2350/dsp/il_engine.{c,h}` | **reference only** — a leaner RP-native re-do of the DNF model, specialized for DSP pipeline taps, pure-evaluator (latch elsewhere). Style reference, NOT the port target. |

**The DSL** (text, flashed as `ilcf`):
`name;cfg[(pin):mode,…];watch[input:op:threshold];out_ok[pin:val];out_err[pin:val]`.
Input modes: GPIO (in/pull/out), ADC (oversample/hysteresis/debounce), VIRTUAL
(uptime, time-since-last-frame, stack-HWM), ADC_STREAM (avg/min/max/rms). Watch
clauses are grouped as **DNF** (AND within a group, OR across groups).

**Port =** lift `samd21_interlocks.c` + the DSL; swap the HAL
(`samd21_hal_pin.h` / `samd21_adc.h` → RP2040 GPIO/ADC, driven from `hwio`); use
`.uninitialized_data` for the `.noinit` persistence (slow_bus already does this for
the crash slot); replace the SAMD21 register/`OP_POLL` surface with the
**shared-status** coupling (status word out, `rearm` flag in); make `ilcf` + the I²C
inventory config-FS files; **add the I²C mirror as an input source** with the
freshness fail-safe.

---

## How it maps onto today's slow_bus code
- **Thread 1** ≈ today's `host_link` intake + bus command-collect + inter-core
  distribution, unified; **absorbs KB0 (monitor) + KB1 (HIL — made operate-only)** as
  its command handlers.
- **Thread 2** = NEW: the ported SAMD21 interlock — **replaces the planned KB2/KB3
  chain KBs.**
- **Thread 3** ≈ today's core1 `app_engine` (chain-tree), now **pure application.**
- **KB4 (manager) is dropped** — with frozen config there's no dynamic allocation;
  pin-ownership is **static** (Thread 1 validates against `hwio`).

---

## Build & identity model — the chain app is per BUILD, not config

The single-image bus node (the Thread-1/2 engine + transport + interlock framework)
is the **common foundation**. A real deployment is a **BUILD = foundation + one
chain-tree app**:
- The **chain-tree app (Thread 3's IR) is baked into the build** (like today's
  `connection.json`), **NOT** a flashed config artifact. **A different chain app = a
  different build.** The config-FS carries **role/hardware only** (`idnt`, `hwio`,
  `ilcf`, I²C inventory) — never the app.
- **Every unique build carries an `instance_id`**, baked in and reported at boot +
  over `OP_REGISTER`. It says *which app build* this unit runs. Config never sets it.
- **Orthogonal to `variant`:** `variant` (vr) = bus-node hardware + role
  (master/slave product); `instance_id` = which application build. A unit's identity
  = `{variant, instance_id}`. (If `class_id` is kept, treat it as the app *family* and
  `instance_id` as the specific build.)

**Two guards (both important):**
1. **Config↔build mis-flash guard.** The `idnt` config declares the **expected
   `instance_id`**; boot **REFUSES / quarantines** on mismatch — same pattern as the
   chip/variant/UUID guards. Rationale: `variant` can't tell two apps on the *same*
   hardware apart, so without this a per-unit config (address/`hwio`/`ilcf`)
   provisioned for app-X could silently run on a unit built as app-Y.
2. **Cross-node right-app check.** The master already collects each slave's identity
   via `OP_REGISTER`; carry the **`instance_id`** there so the master's app confirms a
   given address is running the **expected** counterpart app before exchanging.
   **Addressing stays the bus address** — `instance_id` is the "right app?" check, not
   the address.

## Initial chain-tree application (the PoC) — master↔slave chain round-trip

The first Thread-3 app is the **chain-tree-level echo** (the application-layer analog
of the HW-verified bus `CMD_ECHO`): the **master's chain-tree originates a message →
the slave's chain-tree → back to the master's chain-tree.** It proves the new paths on
both ends — Thread 3 *originating* RS-485, and Thread 1 *routing a non-bench message up
to Thread 3*.
- **Per-node builds, master initiates:** the master runs the *initiator* build (one
  `instance_id`), the slave the *responder* build (another). **Two builds.**
- **Addressing:** master targets `{slave bus addr, non-bench type}`; the slave's
  Thread 1 routes non-bench → Thread 3. One application entry per node (no
  sub-destination within the chain).
- **Correlated request/reply:** a `req_id` the master matches (like the shell-exec
  `req_id`), not fire-and-forget.
- **Transport reuse:** master originates via the command-inject path (the chain-tree
  is a message source like the host); the slave replies in its POLL window (two-phase
  ACK/POLL).

## Open / deferred
- Final core affinities + slave-responder timing (thread-review pass).
- The `hwio` schema + the I²C-inventory schema (CBOR, siblings to `idnt`).
- Whether the chain-tree also emits USB/host replies or is RS-485-only (settle when
  spec'ing Thread 3).

## Suggested build order
1. ✅ `hwio` config (schema + host builder + boot reader applying pin roles). **DONE
   2026-06-23** — `node/boot_hwio.{c,h}`, `cfg_image.lua --io/--adc`, `hwio_apply()`
   at boot. CMD_GPIO_CONFIG retired; HIL is operate-only, validated per frozen role.
2. ◐ Thread 1 — router + bench surface (operate-only HIL, validated against `hwio`).
   **Partial** — the HIL command surface is now operate-only/role-validated. The full
   router unification (one tagged event type, per-consumer queues, role-agnostic
   master/slave, slave-side hwio_apply) is still to do.
3. The I²C service (periodic-sample → shared area + intermixed async; inventory config).
   **DEFERRED** by request — build the I²C framework later.
4. ◐ **Thread 2 — the SAMD21 interlock port.** **Software-complete 2026-06-23**
   (Stages 1–4 in `interlock-port-map.md`): HAL + framework + DSL + pin table ported
   and LINKED into the image, arming from `ilcf`, ticking on core1, veto on GP0 —
   builds clean. **HW-verify pending** (trip→latch→reset on the bench). Still TODO:
   the hard ADC-ISR fast-veto path; I²C-mirror input (with the deferred I²C service);
   role-agnostic placement; final core affinity.
5. Thread 3 — the chain-tree application (non-bench events in, RS-485 out).
