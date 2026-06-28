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
