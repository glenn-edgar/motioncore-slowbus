# core1 — application engine (design)

Status: **design locked 2026-06-07** (chat-mode dialogue). No code yet. core0
(bus + uplink, FreeRTOS-SMP) is implemented and bench-green; this document is the
plan for the second core. The chain_tree reference (`reference/chain_tree_c`) was
studied first — every decision here maps to an existing engine primitive.

## Role of core1

core1 runs the **application engine** (chain_tree). core0 owns the southbound bus
and the northbound USB host link; core1 owns local application logic, local HIL,
and the safety interlocks.

core1 is a **bidirectional virtual slave**: core0 polls it like any RS-485 node
*and* core1 originates messages (to the USB host, or to other slaves). So core1 is
a command **source** alongside the Pi host, and **core0 stays the sole bus
arbiter**. The seam is an inter-core FreeRTOS queue with two lanes:

- **down** (core0 → core1): commands addressed to core1's virtual address — reuses
  the sweep's command-inject path.
- **up** (core1 → core0): tagged `{dest, opcode, payload}`. `dest == host` → core0
  stages an s2m frame to USB; `dest == a slave` → core0 injects it into the sweep
  exactly like a Pi-originated command.

Open: with the Pi *and* core1 both able to target a slave, core0's single in-flight
command slot must become a 2-deep, source-tagged queue (arbitration rule TBD —
round-robin or Pi-priority).

## The five KBs (chain_tree, state-machine based, 10 Hz tick)

Real-time is **not** in the KBs — the 1 kHz ISR owns it — so a 10 Hz (100 ms)
policy cadence is fine. The 10 Hz tick blocks via the platform timer hook
implemented as `vTaskDelay(100 ms)`, so the app_engine thread yields and bumps its
heartbeat (feeding the core1-starvation watchdog).

| KB | Role |
|----|------|
| **KB0** | Background **monitor**. Runs on USB command; reports system usage as **multiple packets, each a different msg id** (async report frames, not request/reply: the command gets its `OP_SHELL_REPLY` ack, then N telemetry frames flow as distinct opcodes). Also reports the 64-bit status word + edges, and queryable EEPROM logs. |
| **KB1** | **API** commands — the `OP_SHELL_EXEC` surface for core1's local HIL + BC-local commands. |
| **KB2, KB3** | The **two interlocks** — *identical*, two instances of one module (a Lua helper called inside two `start_test`/`end_test` blocks). Independent config / callback / veto-pin. |
| **KB4** | Interlock **manager** — admission control + allocator + cross-KB supervisor (below). |

KB4 is the scalability seam: more than two interlocks later = add KB5/KB6 and KB4
allocates among them.

## Interlocks — 1 kHz ISR fast path + 10 Hz KB policy

The safety property from the SAMD21 era (a veto that never depends on the engine
being healthy) is preserved by **splitting** the interlock:

### Central ADC service (shared spine)

One SAR ADC, **round-robin** over the 3 channels (GP26/27/28), **free-running** into
the FIFO, drained by **one FIFO ISR** (enabled on core1). The ISR demuxes the
round-robin sequence into per-channel accumulators and **boxcar-decimates** to
~1 kHz/channel (e.g. raw ~48 kHz total ÷16). Output is `latest[3]` (+ optional
min/max/mean for KB0 telemetry). **Single writer (ISR), lock-free readers**
(aligned reads are atomic on the M0+). The decimation cadence **is** the interlock
callback cadence — right after emitting a decimated sample the ISR walks the
registered interlock callbacks (fresh sample → immediate veto, one clock).

### Fast path (1 kHz ISR callbacks) — sole owner of fast state

Each armed interlock registers a callback. The callback does `watch → drive veto
output to safe → LATCH`. The **ISR is the sole writer** of latch + veto + re-arm
execution. The latch holds through input recovery — no auto-resume.

### Slow path (KB2/KB3 state machine)

Each interlock is a **state**: `DISARMED → ARMED → TRIPPED → ARMED` (on verified
re-arm). A config change **terminates** the old state (stop callback, release veto
pin) and inits the new — the SAMD21 init/tick/terminate contract realized as
chain_tree state enter/exit. **A config change is REFUSED while TRIPPED** (clear /
disarm first). The ISR owns the real latch; the KB's `TRIPPED` state is a **mirror**
it adopts when it observes the latch at the 10 Hz tick (to drive notify + re-arm).

### Re-arm (host-driven, verify-safe)

The latch clears only via an explicit **host re-arm message** → core0 → queue → KB
sets `rearm_request` → the **ISR executes it at the next tick**: verify the input is
on the safe side (+ dwell); if safe, clear latch + release veto; else leave latched
and set `rearm_refused`. Re-arm is a request with **ack/nack**, never
fire-and-forget. (New API command id.)

### Notification — two channels

1. **Summary** — the 64-bit status word (latched bits). core1's virtual-slave POLL
   response reads the ISR latch **directly**, so core0 escalates
   `OP_BUS_SLAVE_FLAGGED` at **bus-poll cadence**, not 10 Hz. The reliable index.
2. **Detail message** — a msg id carrying which interlock / channel / value / why,
   pushed by the KB at 10 Hz. A lost detail message is healed by the Pi
   controller's existing reconciliation/repush (the SAMD21 three-piece pattern).

### KB4 — admission control, allocation, supervision

All interlock requests route through KB4. It (a) validates against the EEPROM
**supported-interlock set** (the firmware carries *all* interlocks; the EEPROM says
which are supported on *this* board), (b) rejects **pin conflicts** and
**duplicates**, (c) **allocates** a free slot → KB2 or KB3, (d) applies the EEPROM
**power-on posture** at boot, and (e) owns the **pin-ownership/allocation table**
(shared C state: KB1 reads it to refuse HIL writes to a claimed pin; the ISR reads
it on arm).

**Cross-KB supervision via user functions:** chain_tree's native supervisor is
intra-KB only, so KB4 monitors KB2/KB3 through user functions that can call
`cfl_delete_test_by_index` / `cfl_add_test_by_index` to **restart a wedged interlock
KB**. This is **safe by construction**: the ISR owns the real latch+veto, so a KB
restart simply re-syncs the KB's mirror from ISR truth — **no veto gap**. (Verify
on-target that delete/add of another KB from inside KB4's tick is re-entrancy-safe.)

## Persistence — three stores

- **EEPROM** (I²C, *faked behind a provider seam until the part arrives*): all
  **hardware definitions** (supported interlocks, I²C device inventory, pin
  assignments) + **power-on interlock posture** + **optional logs KB0 can query**.
  Per-**board**, survives power loss. Provider surface: `hw_caps()`,
  `poweron_interlocks()`, `log_read(cursor)`, `log_append(entry)` — compiled-in
  defaults today, I²C driver later, nothing upstream changes.
- **Identity** — the **flash commission block** (`commission.c`, A/B + CRC): burn
  `class_id` + `instance_id` during commissioning; **slave addr = instance low
  byte**; store the **chip unique id** as a binding check. Persistent across power
  loss (flash). `.noinit` is only a warm-reset cache of it (RAM — wiped on power
  loss, so it cannot be identity's home). Identity travels with the **chip**;
  capability travels with the **board**.
  - **Dual-core flash rule:** commissioning writes must use `flash_safe_execute`
    (parks core1 + masks IRQ) or run pre-scheduler. A naive `flash_range_program`
    with core1 live will fault.
- **Firmware** — contains all interlock implementations; the EEPROM selects the
  supported subset.

**The latch is runtime-only:** on any reset KB4 re-applies the EEPROM power-on
posture and a still-live fault re-trips in ~1 ms (self-correcting). Only a
transient-already-cleared fault is forgotten across a reset — acceptable.

## Event discipline (core1 thread-safety)

Every input is a **node-addressed event**; producers only **deposit**, they never
walk or mutate the tree. **Only the timer tick progresses the tree**, so all tree
mutation is single-threaded and lock-free. Node-addressing localizes delivery — an
event to a node touches only its subtree (ADC trip → KB2/KB3, I²C completion → the
requesting node, host command → KB1/KB4), no cross-KB ripple.

The runtime's enqueue is single-producer, so the **tick thread is the sole
enqueuer**: each source (ADC ISR / I²C service / core0) writes its node-addressed
record into its **own SPSC mailbox**; the app_engine thread drains the mailboxes
immediately before the tick and injects them. Producers never touch the chain_tree
API. This decouples producers from tick phase (an ISR trip is processed at the next
tick ≤100 ms — fine, the veto already fired in the ISR).

## I²C service (no blocking in the engine)

chain_tree must never block on I²C, so a dedicated FreeRTOS task is the **single
owner** of the GP4/5 bus. KBs submit requests `{node_id, req_id, op, dev, reg, len,
data}`; the service runs the blocking transaction in its own thread and posts a
**completion event** (result or error) to the requesting node. This resolves I²C
bus ownership — one service serializes; no bus mutex.

- **Sync at boot:** read EEPROM capability / power-on posture / **the I²C device
  inventory itself** pre-tick (blocking is fine; the EEPROM is the first device at a
  known address, then it lists the rest).
- **Async at runtime:** `log_append`, config changes, live HIL reads → service +
  event.
- **Timeouts are mandatory:** a stuck device posts an **error** completion event
  (→ the node's `wait_for_event` error branch), never an indefinite block.

General principle: **all slow I/O = service task + completion event**, never inline
in a node.

## 64-bit status word — the fast condition lane

A condition register that **data-flow operations gate off** (the runtime's
bitmask-gating). This is the *condition* lane, distinct from events: an event
invokes one node's logic; a status bit **broadcasts a condition many data-flow
nodes key off in a single tick** — no per-node dispatch, every consumer sees it at
once. That lets an interrupt-level or lower-node condition drive upper-level
data-flow / configuration in one pass ("configure much faster").

**Writer rules (keep "only the tick touches the bitmask"):** the runtime swaps its
gating bitmask during the tick, so the ISR must not RMW it concurrently. Instead:

- **ISR / fast-condition bits** live in a **separate ISR-owned condition word**, set
  by a single atomic 32-bit store (the ISR computes the whole word and stores it).
- The **tick folds that word into the chain_tree gating bitmask at tick start**
  (`bitmask = (bitmask & ~ISR_MASK) | g_isr_cond`); the **tick stays the sole
  writer** of the runtime bitmask.
- **Data-flow nodes gate off the bitmask**; core0's virtual-slave POLL and KB0 read
  it atomically.

So **partition the 64 bits by writer** — ISR/fast-condition bits vs tick/policy bits
— each bit single-writer. The interlock *condition* bit is **ISR-sourced** (fresher
than a KB mirror), folded in by the tick; the **veto is still the ISR's direct pin
drive** — a status bit never gates a veto, only data-flow and reporting.

Uses: the node's **upward summary** core0 reads from the POLL (bits → mapped onto
`OP_BUS_SLAVE_FLAGGED`); **KB0 telemetry** (word + edges as a msg-id packet); an
**inter-KB signal bus** (KB4 "config rejected", KB1 "busy", KB2/KB3 "tripped"); and
the **trigger for data-flow operations**. Bitmap allocated — fixed compile-time bits
for health/config, **KB4 assigns each interlock its bit** at allocation. Open:
confirm runtime scope (one global word vs per-KB; design wants a single system word).

## events vs data-flow

Two complementary chain_tree mechanisms, kept distinct:

- **Events** — node-addressed **signals/triggers** that **invoke one node's logic**
  ("a trip happened", "command arrived", "I²C done"). Deposited; dispatched on tick.
- **Data-flow** (`data_flow.lua`) — structured **payload movement within a chain**,
  **gated off the 64-bit status bits** (the condition lane). KB0 assembles its
  multi-msg-id telemetry records; KB1 threads args → result; KB2/KB3 carry trip
  detail into the notify; KB4 flows a request through validate → allocate → assign.

Rule of thumb: **invoke a specific node → event; sense/gate/configure off a
broadcast condition → status bit driving data-flow.**

## Cadences

| Cadence | What |
|---------|------|
| **1 kHz ISR** | ADC decimation + interlock watch/veto/latch (real-time) |
| **bus-poll** | summary-bit escalation (core0 reads the ISR latch directly) |
| **10 Hz KB tick** | policy: notify, re-arm decision, sequencing, telemetry |

## core1 watchdog

The app_engine thread bumps a heartbeat each tick; it joins the existing
free-running-clock stale-check the watchdog already runs for the core0 threads.
core1 starvation → heartbeat goes stale → the watchdog withholds the pet → the HW
WDT bites. (chain_tree's own per-KB exception/heartbeat is intra-KB resilience;
cross-core liveness stays the RTOS watchdog. KB0 is monitor/telemetry, **not** a
cross-KB supervisor — that role, where needed, is KB4's via user functions.)

## Open items / build order

Open: core0 command arbitration (Pi vs core1); confirm the status-word scope;
verify cross-KB `delete/add_test` re-entrancy; allocate the s2m opcode catalog for
KB0's msg ids + the re-arm command id.

Suggested build order:
1. Fan-in mailboxes + tick-as-sole-injector.
2. ADC service (round-robin + decimation ISR) + the interlock fast path.
3. KB skeletons (KB0/KB1) ticking on core1, wired as core0's virtual slave.
4. KB2/KB3 interlock state machines + the summary word + notification.
5. KB4 manager + the fake-EEPROM provider seam.
6. Commissioning (flash-backed identity via `flash_safe_execute`).
7. Real I²C EEPROM behind the provider seam.
