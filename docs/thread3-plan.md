# Thread 3 — chain-tree application: implementation plan

Scoped 2026-06-24 from an engine-model investigation. The goal (design `§Initial
chain-tree application`) is the **master↔slave chain-tree echo**: the master's
chain-tree *originates* a message → the slave's chain-tree → back. This proves the
two genuinely-new paths — Thread 3 **originating** RS-485, and Thread 1 **routing a
non-bench message up to Thread 3**.

## What already exists (reuse)
- **Chain-tree engine** (`cfl_runtime`) on core1 (`app_engine_task`). KBs come from a
  **baked IR** (`g_chaintree_handle`, generated from `connection.json`). The activate
  loop in `main()` walks `h->kb_table[]`, activates each KB, records its start node;
  `kb0`/`kb1` nodes are captured into `g_kb0_node`/`g_kb1_node`.
- **Event injection into a KB:** `cfl_send_integer_event(g_rt->event_queue,
  CFL_EVENT_PRIORITY_HIGH, <kb_node>, EVENT_CMD_*, <int data>)` — this is how
  `cfl_embed_pre_tick` routes `CMD_MON_PING`→kb0, `CMD_ADC_READ`→kb1 today.
- **KB handlers** are C "user functions" (`kb0_on_ping`, `kb1_on_adc`) that read the
  event, do work, and push a reply via `g_up_q` (`appcore_rep_t` → uplink → USB).
- **Bus relay (host→master→slave→host):** the master's `bus_control_task`
  (`BS_CMD_INJECT`) sends a host-queued command to a slave and relays the reply.
  `node_cmd_dispatch` (B1) handles `NODE_CMD_ECHO` on both roles.

## What's missing (the new work)
1. **An application KB in the IR.** Add an app KB (e.g. `kbapp`) to `connection.json`
   and regenerate `chaintree_handle.c` via the chain-tree compiler tooling
   (`tools/...` — confirm the generator + its invocation). Capture its node like
   kb0/kb1 (`g_kbapp_node`). One application entry per node (no sub-destination).
2. **Thread-1 routing of non-bench messages to Thread 3.** A non-bench/application
   opcode (e.g. `OP_APP_*` or a `cmd` range) that `cfl_embed_pre_tick` routes to
   `g_kbapp_node` via `cfl_send_integer_event` (instead of `node_cmd_dispatch`/
   engine-monitor). This is where the **tagged event** `{source, reply_addr, req_id}`
   finally earns its keep — the app handler is origin-agnostic; only the reply path
   reads the tag. (Do B2/B3 here, with this as the first real second consumer.)
3. **Engine ORIGINATES an RS-485 message.** New: a KB handler (or a trigger) that
   queues a bus command to a slave address through the **inject path**, with the
   master's *own* `req_id`, and **captures the slave's reply internally** (vs relaying
   it to the host). Today the inject path is host-driven; add a firmware-originated
   variant + reply correlation in `bus_control_task`.
4. **Correlated request/reply across the bus** — a `req_id` the master's app matches
   (like the shell-exec `req_id`), not fire-and-forget.

## Suggested increments (each build-green + HW-verify on the bench)
- **C1 — app routing on one node.** Add `kbapp` to the IR + a handler that echoes;
  route a new app opcode from Thread 1 → kbapp; reply over USB. Proves Thread1→Thread3
  routing + Thread3 reply on the master alone. (Pulls in B2/B3 tagged events.)
- **C2 — master originates to the slave.** Firmware-originated inject + internal reply
  correlation: a host "start app-echo to addr N" → master's app originates the bus
  message → slave's `kbapp` echoes → master correlates → returns to host. Proves the
  master↔slave application round-trip (master-initiated).
- **C3 — both ends in the engine.** The slave's `kbapp` (not just `node_cmd_dispatch`)
  handles the app message; per-node builds with distinct `instance_id` (design
  `§Build & identity model`); the cross-node "right app?" check via `OP_REGISTER`.

## Open questions to resolve at build time
- The chain-tree IR generator: which tool builds `chaintree_handle.c` from
  `connection.json`, and how to add a KB + its user-fn bindings.
- Whether the engine can cleanly originate a bus inject from a KB handler (core1)
  given the inject path lives in `bus_control_task` (core0) — likely a queue.
- Reply correlation storage for master-originated requests (a small pending table).

## Dependencies / ordering
- B2 (tagged events) + B3 (symmetric reply) fold into **C1** (their first real second
  consumer) — don't do them in isolation (no-op until then).
- The interlock's **chain-tree event clear source** (next-steps C7) drops in once
  `kbapp` exists: a KB handler calls `interlock_request_global_clear()`.
