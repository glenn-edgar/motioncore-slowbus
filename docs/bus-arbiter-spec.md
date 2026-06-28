# Bus Arbiter Spec — interrupt-driven, idle-gap delimited polling

Status: **DESIGN** (2026-06-28). Supersedes the period-gated cooperative poller in
`app/bus_controller/main.c` (`bus_control_task`, states `BS_SWEEP/BS_CMD_INJECT/BS_CMD_COLLECT`).
Read with `docs/three-thread-design.md` (Thread 1 = I/O router). RP2040 + RP2350 (shared PHY).

---

## 1. Why

The current master polls **one node per `g_poll_period_ms`** (200 ms on the bench, 500 ms
default), gated by a `vTaskDelay`-paced task. Measured consequences:

- **Bus ~99.8% idle** during liveness (one ~360 µs poll/reply per 200 ms).
- **Per-slave poll rate = 1/(N × period)** → 1 slave @ 5 Hz, 10 slaves @ 0.5 Hz (every 2 s);
  dead-node detection for 10 nodes ≈ `max_misses × N × period` ≈ 6 s.
- Round-trip latency is **scheduling-bound** (engine tick + 1–2 ms task quanta), not bus-bound;
  the 460800 wire is ~10–20× faster than we use it.

**Goal:** keep the bus *full* — back-to-back, interrupt-driven poll/grant rotation at the wire
limit (~2,700 transactions/s), so 10 nodes poll at ~270 Hz each and dead-node detection drops to
~10 ms. The rotation must support **variable-length grant windows** (a polled node may send its
own reply *plus* slave-to-slave frames), so the master delimits each node's turn by **bus idle**,
not by frame count.

---

## 2. Principles

1. **Interrupt-driven, not task-paced.** The rotation advances in ISR context (RX-word IRQ +
   one hardware alarm). No `vTaskDelay` in the hot path; the FreeRTOS task only does deferred,
   non-time-critical work (engine routing, host relay, roster edge reporting).
2. **Idle-gap delimits a node's turn.** A granted node may transmit a burst of frames; the
   master knows the node is finished when the line has been idle for `T_GAP` (Modbus-RTU style
   inter-frame gap), *not* after a fixed number of frames.
3. **Three timers on one alarm** bound every slot: `T_RESP` (dead-node), `T_GAP` (end-of-burst),
   `T_WINDOW_MAX` (anti-babble).
4. **Bus stays full.** The next POLL is issued the instant the current slot ends (idle, timeout,
   or cap) — no period gate. Throughput = wire-limited, not period-limited.
5. **Fail-safe roster.** Liveness lifecycle (ALIVE/DEAD/recovery) is preserved; dead nodes are
   demoted to slow-poll so they don't burn rotation time.
6. **ISR stays minimal.** ISRs push/pop PIO FIFOs, run the slot state machine, and hand completed
   frames to a queue. All parsing/routing/relay happens in a task.

---

## 3. Hardware substrate (RP2040 / RP2350, identical)

- **PHY:** PIO 9-bit MPCM UART (`rs485.pio`, `phy_pio_rs485.c`). Already has an **RX-FIFO-not-empty
  IRQ** (`phy_rx_isr`) draining 9-bit words into a ring. TX = push words to the PIO TX FIFO
  (`bus_phy_send_words`, non-blocking) — **ISR-safe**.
- **Alarm:** one channel of the 64-bit system timer (4 available) → a hardware ISR. Used for all
  three slot timers (armed with phase-dependent durations; only one is pending at a time).
- **No hardware idle detector exists** (the RX SM just `wait`s for the next start bit). The
  idle-gap is synthesized: **each RX word rearms the alarm to `now + T_GAP`**; expiry == idle.
  (A pure-PIO idle IRQ is a possible later optimization; not required.)

---

## 4. Slot state machine (per polled node)

One "slot" = the master grants node `N` a turn and runs it to completion. The master holds at most
one node granted at a time (half-duplex bus).

```
        ┌──────────────────────────── advance(next N) ───────────────────────────┐
        ▼                                                                          │
   ┌─────────┐   TX POLL(N)        ┌──────────────┐   first valid word    ┌──────────────┐
   │  ARM    │ ─ arm alarm=T_RESP ▶│  AWAIT_FIRST │ ────────────────────▶ │   IN_BURST   │
   └─────────┘                     └──────────────┘                       └──────────────┘
                                      │  alarm (T_RESP, 0 words)             │  each word: rearm alarm=T_GAP
                                      │                                      │             check elapsed vs T_WINDOW_MAX
                                      ▼                                      ▼
                                 NO_RESPONSE                          alarm (T_GAP)  → END_OK
                                 → mark_miss(N)                       cap exceeded   → END_BABBLE → flag(N)
                                 → advance                            (both) mark_alive(N) → advance
```

**States**
- `ARM` — choose the next node from the schedule (§6), TX its POLL frame, arm alarm = `T_RESP`,
  zero the word counter and slot-start timestamp. → `AWAIT_FIRST`.
- `AWAIT_FIRST` — waiting for the node's first word.
  - RX word (valid frame start) → cancel `T_RESP`, record `slot_start`, rearm alarm = `T_GAP`. → `IN_BURST`.
  - **alarm (T_RESP) with 0 words → `NO_RESPONSE`**: `mark_miss(N)`; advance. *(dead-node path)*
- `IN_BURST` — the node is transmitting; deliver/queue each completed frame (§5).
  - each completed word → rearm alarm = `T_GAP`; if `now - slot_start > T_WINDOW_MAX` → `END_BABBLE`.
  - **alarm (T_GAP) → `END_OK`**: burst complete; `mark_alive(N)`; advance.
  - `END_BABBLE`: force end; `mark_alive(N)` but `flag(N, BABBLE)`; advance. *(anti-starvation)*

**advance(next N):** pick the next scheduled node (§6) and re-enter `ARM` immediately — the next
POLL goes out with no gap, keeping the bus full.

All transitions run in ISR context (RX-word ISR or alarm ISR). The only shared state the task
touches is the roster (under a short critical section) and the completed-frame queue.

---

## 5. Frame handling inside a burst (incl. slave-to-slave)

The shared multidrop bus means **every node hears every frame**. A node, when granted, may emit:
- its **reply to the master** (`dest = MASTER`), and/or
- **slave-to-slave** frames (`dest = peer addr`), and/or
- a terminating **`NO_MESSAGE`** (optional fast-path "I'm done" marker — still emitted by
  `node_emit_window`; treated as a normal frame, not relied upon for delimiting).

**Delivery model (recommended): direct shared-bus.** Each completed frame's `dest` is examined by
the **receiving peer's** assembler (MPCM addr marker → accept if `dest == my_addr`). So a
slave-to-slave frame is delivered to the target peer directly, with no master relay. The master
still:
- hears the frame (for bookkeeping / optional routing audit),
- counts it toward the node's burst (rearms `T_GAP`),
- and, if the frame's `dest == MASTER`, hands it to the completed-frame queue for the task
  (engine routing / host relay).

*Alternative (master-relay):* peers don't RX peer-addressed frames; the master re-injects each
slave-to-slave frame on a later slot. Higher latency + doubles bus load; **deferred** unless peers
can't RX promiscuously. Decision: start with **direct delivery** (requires the slave assembler to
accept `dest == my_addr` from any `src`, which the current `bus_asm` already does via the addr
filter).

**ISR vs task split:** the ISR appends `{src, dest, type, len, payload}` to a completed-frame
queue and advances the slot. The arbiter task drains the queue → `node_engine_try_route` /
`node_cmd_dispatch` / host relay. Heavy work never blocks the rotation.

---

## 6. Schedule (which node next) + command interleaving

The rotation is a round-robin over **enabled** nodes (`bus_roster_next_enabled` / the `g_cursor`
model), with two refinements:

1. **Dead-node demotion (slow-poll).** ALIVE/UNKNOWN nodes are polled every rotation; **DEAD**
   nodes only every `DEAD_POLL_DIVISOR` rotations (enough to catch recovery, ~100 ms). Promote to
   fast-poll on the first valid response. This reclaims the `T_RESP` dead-air of absent nodes:
   per-rotation dead-air = `(N_dead / DEAD_POLL_DIVISOR) × T_RESP`.

2. **Command / data injection.** Host- or engine-originated transactions (today: `g_orig_q`
   promotion + `g_cmd_pending`, the `BS_CMD_INJECT/COLLECT` path) fold into the rotation as a
   **DATA grant** instead of a bare POLL: when node `N`'s slot comes up *and* a command is queued
   for `N`, the master TXes the DATA frame (expecting ACK + the reply burst) rather than a POLL.
   Same slot state machine (the DATA frame *is* the grant; `T_RESP` covers the ACK/first word).
   This unifies liveness polling and command delivery into one back-to-back rotation — no separate
   `BS_SWEEP` vs `BS_CMD_*` phases.
   - Priority: a queued command for the current node pre-empts its plain poll; cross-node commands
     wait for the target node's slot (bounded by the rotation period, now ~ms not ~100s of ms).

---

## 7. Timing parameters (baud-relative; 460800 example)

Word = 11 bits (start + 9 data + stop) = **23.9 µs @ 460800**.

| Param | Meaning | Range | @460800 example |
|---|---|---|---|
| `T_GAP` | inter-frame idle = end-of-burst | 1.5–3.5 words | ~36–84 µs (use ~60 µs) |
| `T_RESP` | first-word deadline = dead-node | node turnaround (IRQ + reply assembly) + margin | ~150–300 µs |
| `T_WINDOW_MAX` | per-node burst ceiling (anti-babble) | N × max reply + slack | ~2–5 ms |
| `DEAD_POLL_DIVISOR` | poll DEAD nodes 1-in-K rotations | — | ~recover within 100 ms |

All scale with baud (the PHY clkdiv already auto-adapts); store as **bit-times** internally and
convert at `bus_phy_init`. `T_RESP` is the one to tune on HW — too small false-misses a briefly
busy node, too large wastes dead-air. `T_GAP` must exceed worst-case inter-word jitter under
the RTOS/PIO so it can't false-trigger mid-burst.

---

## 8. Dead-node lifecycle (roster)

Reuses the existing roster (`mark_alive`/`mark_miss`/`max_misses`/`consecutive_misses`/`state`):

- `NO_RESPONSE` (T_RESP expiry, 0 words) → `mark_miss(N)`; `consecutive_misses++`.
- `consecutive_misses ≥ max_misses` → `state = DEAD` → demote to slow-poll (§6.1).
- Any valid response (in `AWAIT_FIRST`→`IN_BURST`) → `mark_alive(N)`: `consecutive_misses = 0`,
  `state = ALIVE`, promote to fast-poll. **Recovery is automatic.**
- `END_BABBLE` → `mark_alive` + `flag(N, BABBLE)` (a node that's up but misbehaving; surfaced to
  the host, not removed).
- Liveness edges (ALIVE↔DEAD transitions) are reported to the uplink by the **task**
  (`emit_liveness_edges`), not the ISR.

---

## 9. Concurrency / safety

- **ISR context** (RX-word, alarm): run the slot state machine, TX the next grant (push to PIO TX
  FIFO), update word/timestamp counters, enqueue completed frames, update roster miss/alive
  counters (single-writer from the arbiter ISR; the task reads under a short critical section).
- **Arbiter task** (low prio): drain the completed-frame queue → dispatch/route/relay; emit
  liveness edges; service `g_orig_q`/command queue (set up the next DATA grant). Never blocks the
  rotation.
- **No `vTaskDelay` in the rotation.** The alarm ISR is the clock.
- **TX-from-ISR** is just `pio_sm_put` to the TX FIFO (the PIO serializes) — bounded, no spin.
- **Watchdog:** the arbiter task still pets a heartbeat; a wedged rotation (alarm stuck) must be
  caught — add an arbiter liveness counter the watchdog task checks.
- **Self-echo:** half-duplex shared line — the master must not treat its own TX as an RX word
  (existing skip discipline in the PHY/assembler; preserve it).

---

## 10. Edge cases

- **Lost `NO_MESSAGE` terminator** — irrelevant: `T_GAP` delimits regardless. (The terminator is
  only a fast-path; idle is the truth.)
- **CRC error / partial frame** — assembler rejects it; it still rearms `T_GAP` (the line was
  active) but is not delivered/counted as a reply. Optionally a per-node CRC-error counter; a burst
  of CRC errors ending in idle → `mark_alive` but flag link quality.
- **Babbling node** — `T_WINDOW_MAX` force-ends the slot; node flagged, rotation protected.
- **Two nodes transmit (collision / mis-addressed grant)** — manifests as CRC garbage → handled as
  above; the polled-grant model makes this a fault, not normal.
- **Empty schedule** (no enabled nodes) — arbiter idles (one slow heartbeat poll of nothing);
  resumes on roster change.

---

## 11. Relationship to current code / migration

| Now | After |
|---|---|
| `bus_control_task` w/ `vTaskDelay(1–2ms)` + `g_poll_period_ms` gate | ISR-driven rotation; task does deferred work only |
| `BS_SWEEP` / `BS_CMD_INJECT` / `BS_CMD_COLLECT` phases | one slot state machine; commands = DATA grants in the rotation |
| `bus_recv(POLL_SLOT_TIMEOUT_MS)` blocking wait | RX-word IRQ + alarm (`T_RESP`/`T_GAP`/`T_WINDOW_MAX`) |
| reply "done" = 1 frame collected | "done" = bus idle (`T_GAP`) — supports variable bursts + slave-to-slave |
| dead detect ≈ `max_misses × N × period` (~6 s/10 nodes) | `max_misses × T_RESP` (~ms) |

PHY additions needed: an **alarm-timer hook** + a small **completed-frame queue**; the RX-FIFO IRQ
already exists. The roster API is reused as-is. `g_orig_q`/`g_cmd_pending` fold into the DATA-grant
slot. No wire-format change (same `bus_frame` codec, same 9-bit MPCM).

---

## 12. Open decisions

1. **Slave-to-slave delivery:** direct shared-bus (recommended; needs peers to RX peer-addressed
   frames) vs master-relay. → start direct.
2. **`T_RESP` value** — measure node turnaround on HW (IRQ pickup + reply assembly) and set with
   margin. The single most important tuning constant.
3. **DATA-grant vs poll fairness** — does a command pre-empt the target's *next* slot, or get its
   own immediate slot? (bounded either way now that the rotation is ~ms).
4. **Engine-tick coupling** — the chain-flow round-trip will still carry the per-node engine-tick
   latency; the arbiter removes the *bus* scheduling latency but not the engine tick. Per-node
   `delta_time` (already a TODO) is the complementary lever.
5. **Slave-side window scheduling** — `node_emit_window` must emit its full burst (reply + queued
   slave-to-slave) within `T_WINDOW_MAX`; confirm the slave can assemble back-to-back inside the
   grant.

---

## 13. Build order (incremental, HW-verify each)

1. **Alarm + idle-gap on the existing flow** — replace the `bus_recv` blocking COLLECT with the
   `T_RESP`/`T_GAP` alarm (still task-driven), prove idle delimiting + dead-node timeout on the
   bench (1 slave, then a deliberately-dead addr).
2. **Back-to-back rotation** — drop `g_poll_period_ms`; advance immediately on slot end. Measure
   per-slave rate + bus utilization (instrument TX/RX busy-µs).
3. **Move the slot machine into the ISRs** — RX-word + alarm drive it; task does deferred dispatch.
   Confirm watchdog-safe, no rotation stalls.
4. **DATA-grant interleave** — fold `g_orig_q`/commands into the rotation; re-run the chain-flow
   bench (expect bus latency gone, engine tick remaining).
5. **Slave-to-slave** — slave queues a peer-addressed frame in its window; confirm direct delivery
   + the master's idle-gap correctly spans the multi-frame burst.
6. **Dead-node slow-poll + anti-babble** — demotion/recovery + `T_WINDOW_MAX`; fault-inject a
   babbling node.
```

---

## 14. Measured slot budget (HW, 2026-06-28) — what's actually expensive

After step 3 (ISR-notify rotation), at 950 polls/s the ~1.05 ms slot breaks down as:

| Phase | ~µs | Nature |
|---|---|---|
| POLL frame wire (7 words @460800) | ~167 | bus active (floor) |
| **slave turnaround (RX-complete → first reply word)** | **~244** | **bus idle — slave *task* wakes to run `node_emit_window`** |
| reply burst wire (NO_MESSAGE, 7 words) | ~167 | bus active |
| `T_GAP` idle-confirm | ~300→ | bus idle (tunable) |
| 2× context switch (block/wake) | ~2 | master CPU |

**Findings that redirect the optimization:**
- The **master context switch is ~0.2%** of the slot — moving the master fully into the ISR does *not* help throughput (it helps *CPU*, see §16).
- **`T_GAP` is not the lever** either: HW-tested 300→100 µs gained only ~3% (950→981) and caused a rare false-miss (too tight for inter-word jitter). Keep `T_GAP` ≈ 300 µs.
- The dominant removable cost is the **~244 µs slave-task turnaround** → answer from the slave ISR (§15). That is the real throughput win.

---

## 15. Slave ISR responder + ping-pong staging (the throughput win)

Cut the slave's RX-complete → TX-first-word latency from ~244 µs (task wake) to ~ISR latency by
**pre-staging the outgoing window and firing it from the slave's RX ISR**.

**Mechanism — decouple PREPARE (task) from TRANSMIT (ISR):**
```
Slave task (responder/engine)             Slave RX ISR (frame complete = grant for my_addr)
  encode the outgoing window into the       push ACTIVE buffer words -> PIO TX FIFO   (instant TX)
  STAGING buffer (pre-encoded 9-bit         swap ACTIVE <-> STAGING (atomic index flip)
  words: reply + s2s frames + NO_MESSAGE)   notify task: "refill staging"
        ▲──────────────── refill ──────────────┘
```
- **Two pre-encoded word buffers** (ping-pong). The ISR always holds a ready buffer → no task wakeup in the response path.
- **Default = `NO_MESSAGE` staged** → an idle slave answers immediately from the ISR (stays ALIVE) with zero task involvement.
- **Atomic swap** in the ISR (index flip); ping-pong guarantees the task never writes the buffer the ISR is reading.

**Nuances:**
1. **TX-FIFO depth = 8 words.** A single reply / `NO_MESSAGE` (≤8 words) → the ISR pushes it whole.
   A **multi-frame** window (reply + slave-to-slave burst, >8 words) → needs a **TX-FIFO-not-full
   IRQ** to feed the remainder. So: simple = pure RX-ISR; burst = RX-ISR + TX-refill IRQ.
2. **Freshness / correlation.** A *data* reply is still *produced* by the task (engine-tick latency
   unchanged) and staged; TX is then instant. The master correlates by `req_id`, so the task stages
   the right reply. `NO_MESSAGE` is always safe to (re)fire.
3. **Same `bus_controller` image** — this is the slave-role path (`bus_node`/`node_emit_window`)
   moving to RX-ISR + ping-pong. The master arbiter is untouched.
4. **Risk:** TX-from-ISR (brick-needs-physical-recovery category) — justified here (buys ~244 µs
   vs the 2 µs the master context switch was worth).

**Expected:** turnaround ~411 → ~175 µs → slot ~0.8 ms → ~1250 polls/s (1 slave); with the master
full-ISR (§16) freeing CPU, both ends run lean. Next floor = the POLL wire time (~167 µs); a lean
2-word grant frame (preamble+addr) ≈ 48 µs is a later wire-format optimization.

---

## 16. Master full-ISR rotation (CPU + determinism, not throughput)

Step 3 left the slot state machine in the task (RX-ISR *wakes* it). Moving it fully into the ISRs:

- **RX-word IRQ + hardware alarm drive the slot SM.** A **liveness** slot (POLL → `NO_MESSAGE` →
  mark-alive → advance → TX next POLL) runs **100% in ISR — no task wake at all.** Only a **DATA**
  frame is enqueued to a deferred task (host relay / engine routing).
- **Removes the ~45% core0 busy-spin** (the burst/`T_GAP` wait becomes RX-word/alarm driven, core0
  idle between words) **and the per-slot context switch** for liveness.
- **It does NOT raise throughput** (the slot is turnaround-bound, §14) — its value is freeing core0
  for a contended production master (WiFi + engine + relay) and rotation determinism. Pair it with
  §15 (slave ISR), which is the actual throughput win.

**Components:** claim a hardware alarm; on POLL TX arm `T_RESP`; RX-word ISR feeds the assembler,
rearms the alarm to `T_GAP`, and on a complete frame either advances (liveness) or enqueues (data);
alarm ISR ends the slot (`T_RESP`/`T_GAP`/`T_WINDOW_MAX`) → mark roster → TX next POLL.

**Concurrency:** the PIO RX IRQ and the timer alarm IRQ both touch the slot state → give them the
**same NVIC priority** so they serialize (no nesting races). Roster counters: single-writer (the
arbiter ISRs); the task reads under a short critical section. **Deferred-data context switch
reappears only for data frames**, off the timing path — liveness never switches.

**Safety:** an **arbiter liveness counter** the watchdog task checks (a stuck rotation must reboot);
bounded ISR work (no blocking, no heavy parsing in ISR); keep the heartbeat fed. **Brick risk is
real** (TX/SM/assembler in ISR) → physical BOOTSEL recovery if it wedges; verify incrementally.

### Build order (continued)
7. **Slave ISR responder + ping-pong** (§15) — the throughput win. HW: turnaround drops, poll rate up.
8. **Master full-ISR rotation** (§16) — CPU/determinism. HW: core0 freed, liveness needs no task,
   watchdog-safe, no rotation stalls.
(§14's findings supersede the earlier guess that `T_GAP`/the master context switch were levers.)

---

## 17. Cyclic ping-pong message engine — the real-time data bus (SUPERSEDES §15–16 for the master)

Status: **DESIGN** (2026-06-28, Glenn). The per-slot rotation (§1–16) is fine for liveness but its
core0 cost is ∝ poll rate (HW: ~80% busy-spin at 1000/s; ~34% even after the alarm-block; rate-paced
gets ~3–6% but caps per-node rate). The fleet needs **feedback loops: ~200 msg/s per node × ~6 nodes
(~1200 grants/s, a design goal not hard)** at **low core0** and **bounded real-time**. This section is
the master arbiter redesign that meets that; it replaces the per-slot rotation on the master.

### 17.1 Principles
- **Fire-and-forget, UDP-style.** No bus-level reply/ack. A command is sent and the buffer recovered;
  **feedback is separate node-originated datagrams** delivered to the app (zenoh / chain-tree), which
  does correlation, the control loop, and loss handling — like UDP.
- **One producer path for both sources** (zenoh uplink + internal chain-tree): grab a TX buffer →
  fill → attach to the target node's list.
- **Ping-pong the per-node list HEADERS** (not individual buffers): the bus controller drains the
  active header set with **zero contention** while producers fill the other set.
- **Low core0**: the cycle is a task that **blocks** (idle-gap alarm, §16 mechanism) during each
  feedback wait; the only busy work is tiny per-buffer/per-node bookkeeping.
- **Real-time is measured**: full-cycle time (all nodes) is the watchdog metric.

### 17.2 Data structures
- **Node table, ordered by bus address** (fixed; one entry per node):
  `{ uint8_t addr; uint8_t flags; /*enabled/dead*/ uint8_t miss_count; /* + per-set list heads */ }`.
  This is the sweep index, the NO_MSG order, and the home of dead-flags.
- **Two header sets (ping-pong)**: `g_active`, `g_fill`, each = `array[node] of tx_buf_t*` (per-node
  singly-linked TX list head). Producers attach to `g_fill`; the BC drains `g_active`; the cycle swaps
  the two pointers.
- **Buffer pool** = N fixed `tx_buf_t` **carved from cfl_perm ONCE at init** (cfl_perm is a bump
  allocator — same place the engine event queue is carved; it has **no free**). Runtime grab/recover
  is a **free-list** (LIFO), NOT a cfl_perm free. cfl_perm is never touched after init.
  `tx_buf_t { tx_buf_t *next; uint8_t dest_node; uint8_t len; uint8_t payload[BUS_PAYLOAD_MAX];
             uint32_t origin_tag; /* zenoh req_id / chain-tree event id, for app correlation */ }`.

### 17.3 Concurrency — hw spinlock, never a FreeRTOS mutex
Producers run on **core0 (zenoh uplink)** and **core1 (chain-tree)**; the BC drains on core0; the free
path runs in the BC task. A FreeRTOS mutex can't serve ISRs/cross-core, so guard the three tiny
critical regions with a **pico hardware spinlock** (`spin_lock` / `critical_section_t`, ISR- and
SMP-safe, held ~tens of ns):
- **free-list** push/pop (producers grab; BC recovers),
- **header swap** (`g_active`↔`g_fill`),
- **producer→`g_fill[node]` attach** (producer-vs-producer).

The **drain of `g_active` is lock-free by construction** (after the swap, no producer touches it).
cfl_perm is init-only (no runtime lock). No mutex in the data path.

### 17.4 Producers (zenoh uplink, chain-tree engine)
```
buf = pool_grab();                 // free-list pop under spinlock
if (!buf) { exhaustion_exception(); zenoh_notify(POOL_EXHAUSTED); return; }
buf->dest_node = N; buf->len = ...; memcpy(buf->payload, ...); buf->origin_tag = req_id_or_event;
attach(g_fill[N], buf);            // push onto node N's fill list under spinlock
```
Pool exhaustion is an **exception + zenoh notification + error counter** (never a silent drop); a
**high-water-mark** stat tracks peak in-flight to warn before exhaustion.

### 17.5 The cycle (BC task)
```
cycle:
  spinlock { swap(g_active, g_fill); }              // active = last cycle's fills
  t0 = now
  // (1) MESSAGE pass — fire-and-forget, marks nodes that got a command
  for node in node_table (by address):
    for buf in g_active[node]:                      // lock-free (we own g_active)
      bus_tx(node, buf->payload, buf->len)          // no wait, no ack
      mark[node] = true
      pool_recover(buf)                             // free-list push under spinlock
  // (2) NO_MSG sweep — feedback + liveness for the rest
  for node in node_table (by address):
    if node.dead or mark[node]: continue
    bus_tx(node, NO_MSG)                            // grant the node its turn
    arm idle-gap alarm; BLOCK                       // core0 free during the wait (§16)
    fb = collect()                                  // node's feedback datagram, or NO_MESSAGE, or silence
    if fb: app_deliver(node, fb)                    // -> zenoh publish / chain-tree event
    else { node.miss_count++; if >= MAX_MISS: node.dead = true }
  cycle_stats(now - t0)                             // §17.7
  pace_to_period()                                  // optional fixed-rate (deterministic feedback)
```
Notes: command turns don't wait (fire-and-forget); only NO_MSG turns block for feedback. A node
commanded this cycle is polled next cycle. Dead nodes are skipped; the **task re-enables** them
(clears `dead`, an atomic byte — no lock vs the BC since both are core0, or spinlock if paranoid).

### 17.6 Fixed-rate option (control loops)
For jitter-free feedback, `pace_to_period()` blocks until `t0 + CYCLE_PERIOD` (e.g. 5 ms = 200 Hz).
Free-running (no pace) maximizes rate but jitters. Control loops want fixed-rate; make it config.

### 17.7 Real-time statistics (the watchdog)
Per cycle, measure **full-cycle time** and keep:
- `last / min / max / avg`,
- **overrun count** vs a configurable **deadline**,
- **slack** = deadline − cycle_time (early-warning as it trends to 0),
- **slowest node** this cycle (which node inflated it — finds a slow/babbling culprit).
Exposed via a stats command **and a zenoh notification pushed on each overrun** (lost-real-time is an
event, not just a polled value). Plus pool high-water + exhaustion count + per-node miss/dead.
**This is how we know if we've lost real time.**

### 17.8 Uplink (pub/sub, not RPC) — for multi-client + streaming feedback
- **Feedback**: BC batches a cycle's feedback → agent → **zenoh publish** to a key → any number of
  clients **subscribe** (fan-out is free). Batched-per-cycle publish, not per-message RPC, so the
  uplink isn't the bottleneck.
- **Commands**: clients **publish** to a command key → agent drains into the producer path (grab/fill/
  attach). Two clients → same node = two buffers on that node's list, sent in order (last-writer-wins
  on a setpoint is an app policy); each carries its `origin_tag`.

### 17.9 Build order
1. Buffer pool (cfl_perm carve + free-list + spinlock) + the node-table-by-address.
2. Ping-pong header sets + producer attach (one producer first: host/zenoh).
3. The cycle (message pass + NO_MSG sweep with alarm-blocked feedback wait), single node, then N.
4. Cycle-time stats + overrun/slack/slowest-node + zenoh notify.
5. Pool-exhaustion exception + zenoh notify + high-water.
6. Second producer (chain-tree engine) into the same path.
7. Dead-node skip/re-enable; fixed-rate pacing option.
8. Pub/sub uplink: batched feedback publish + command-key subscribe (agent side).
