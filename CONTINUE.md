# CONTINUE — slow_bus pick-up doc

Read first on any session resume. Companion to `README.md` (orientation),
`docs/three-thread-design.md` (architecture), and `docs/README.md` (full spec).
Last updated **2026-06-25**. Branch: **`samd21-namespace-db`** (clean + pushed).
**★RESUME HERE:** WiFi→zenoh uplink — W1/W2a/W2b/W2c all DONE+HW-verified; next = **W3**
(the real zenoh-agent). See the WiFi section.★

---

## ★ ACTIVE DIRECTION 2026-06-24 (f) — WiFi → zenoh container; W1 DONE, W2 NEXT ★

**Goal (Glenn):** the bus controller (Pico W) talks **WiFi to a zenoh container**. Chain-tree
(Thread 3) reached a good stopping point today (updates b–e below); switched to this.

**Architecture (CONFIRMED + matches `docs/README.md` §Uplink):** proxy model — the chip
streams the **SAME libcomm frame stream** it speaks over USB, but over a **TCP socket** to a
**Linux zenoh-agent** that bridges to zenoh. **zenoh-pico is NEVER on the MCU.** The big
enabler: the firmware↔agent wire IS libcomm (SLIP+CRC8, `OP_SHELL_EXEC`/`OP_SHELL_REPLY`),
so this is "the same frames over WiFi."

**Reuse blueprint = `~/xiao_blocks`** (proven Wio SAMD51→WiFi→zenoh, HW-verified there):
- `host/zenoh_agent/agent.lua` — the proxy. **TCP mode** (`DEVICE=tcp:<port>`): listens on a
  TCP port; the **chip dials in** and streams libcomm; agent is a zenoh RPC server
  (token = FNV1a hash of a key-expr) bridging to a key. Containerized (`Dockerfile`,
  debian+luajit+tini). `vendor/zenoh/*.lua` = LuaJIT zenoh client over a prebuilt `.so`.
- `eclipse/zenoh` **router** on `:46169`; agent is a client to it.
- Wio used RTL8720 eRPC; **Pico W is simpler** — CYW43 + lwIP native sockets.

**Glenn's answers (2026-06-24):** (1) proxy model — yes. (2) agent on the **Pi** (`robot`,
`/mnt/ssd`, container) — location flexible, Pi preferred. (3) agent not written — port from
xiao_blocks. (4) WiFi creds via the secondary-flash config file; SSID **`WIFI_SSID_REDACTED`** /
pass **`WIFI_PASS_REDACTED`** — **NEVER commit these** (cfg_image.lua CLI args only). Agent/zenoh TCP
port = a random configurable number unused by existing containers.

### W1 — 'neti' config file (secondary flash) — DONE + committed `785866a`
WiFi creds + agent endpoint in a new read-only config-FS file, flashed separately.
- `node/boot_netcfg.{c,h}` — reader (mirrors `boot_hwio`; cbor_min). `neti` CBOR:
  `{ v:1, ss:<ssid>, pw:<pass>, ip:h'..4..', pt:<port> }`. `netcfg_t {ssid,pass,ip[4],port,present}`.
  MISSING benign; validates schema + required ssid.
- `cfg_image.lua --ssid/--pass/--agent-ip/--agent-port` builds the `neti` entry.
- `main.c`: `boot_read_netcfg()` at boot; boot banner adds `neti=<rc>` + ssid + agent
  ip:port, **passphrase redacted to length** (`pwlen=N`). Source `node/boot_netcfg.c` added
  to CMake BC target.
- Verified locally (CBOR hand-decodes correct: `a5` map v/ss/pw/ip-bytes/pt; firmware builds
  + links). On-HW boot-banner check folds into W2 (needs a config reflash anyway).

### W2 — firmware WiFi+TCP uplink — DONE + HW-VERIFIED (2026-06-25)
host_link is transport-agnostic, so WiFi = feed/drain it over a TCP socket instead of USB.
- **ENV (one-time, Pi SDK):** wireless submodules were uninitialized → `cd ~/pico/pico-sdk &&
  git submodule update --init --recursive lib/cyw43-driver lib/lwip` + fresh cmake configure.
- **W2a** (`296f3c5`): `app/wifi_test/` smoke target + `port/rp2040/lwipopts.h`. Pico W joins
  `WIFI_SSID_REDACTED` from `neti`, DHCP `192.168.1.205`. (Also HW-confirms W1: `neti rc=0`.)
- **W2b** (`de5b9dd`): lwIP TCP client echo round-trip over WiFi.
- **W2c** (`90fc2b9`): **`bus_controller_wifi`** target (CMake `BC_SRCS` + `define_bus_controller()`
  builds USB + WiFi images; proven `bus_controller` unchanged). `main.c` `#ifdef UPLINK_WIFI`
  `wifi_uplink_task`: POLLED async join (HB_UPLINK kept fresh ⇒ 4s watchdog safe), blocking
  dial, host_link feed/drain over the socket. HW: stable TCP conn, BC streams correctly-framed
  libcomm `OP_REGISTER` (UID `e661..`, class `0x5E589000`) over WiFi; `-f` reflash works
  (no USB-reset wedge, unlike wifi_test).
- **HW gotchas (W2):** lwip non-blocking-connect+select didn't detect completion → blocking
  connect; this lwIP reports SO_RCVTIMEO timeout as **recv()==0** → don't break on n==0 (treat
  recv<=0 as no-data; a failed write detects a dead link); need `LWIP_SO_RCVTIMEO=1`; BC
  force-includes a **printf-stub** so USB printf debug is a no-op in that image; wifi_test (the
  smoke build) could wedge the USB-reset during a 20s blocking join (needs physical BOOTSEL) —
  bus_controller_wifi's polled join avoids that. Persistent Pi servers via **tmux**
  (`new-session -d ... </dev/null >/dev/null 2>&1`; plain `&` over ssh gets swallowed).

### W3 — NEXT: host zenoh-agent + container (the operational round-trip)
Port `~/xiao_blocks/host/zenoh_agent/` (agent.lua + vendor/zenoh + Dockerfile) →
`slow_bus/host/zenoh_agent/`, TCP mode (`DEVICE=tcp:47447`), map slow_bus appcore/bus commands
to a zenoh key. Run `eclipse/zenoh` router + the agent containers on the Pi (`/mnt/ssd`). The
agent ACKs the BC's REGISTER (host_link → OPERATIONAL) and does command round-trips — the
operational test a dumb echo/dump server can't do. End-to-end: zenoh client → router → agent
→ WiFi → Pico W → reply. Then: harden re-dial/backoff; agent maps the chain-tree app echo +
interlock commands.
- **WiFi hardening (from `docs/pico-wifi-research.md`, 2026-06-25 deep research — validates
  our W2c async-join + recv≤0 handling):** (1) **disable WiFi power-save** after join —
  `cyw43_wifi_pm(&cyw43_state, CYW43_DEFAULT_PM & ~0xf)` (0xa11140); default PM silently
  disassociates while link status still says "connected". (2) reconnect supervisor must
  rejoin on **link-down even if the IP lingers** (CYW43 never auto-reconnects). (3) **do NOT
  raise `TCPIP_THREAD_PRIO`** — keep it at lwIP default (1), BELOW the interlock (prio 4) +
  engine (prio 3), so WiFi can't starve safety (the ESP32 footgun). Protection is **core
  affinity**: interlock+engine on core1, WiFi (our uplink + the CYW43 worker, which
  auto-pins to the core that ran cyw43_arch_init = our core0 uplink task) on core0. W3
  core-affinity-review: confirm/pin the lwIP tcpip thread to core0; note CYW43 worker prio
  = interlock prio (4), safe only because different cores. (4) SMP `cyw43_arch_init()`-from
  -task can hang (#1718) — works for us, watch it. Pico2 W: FreeRTOS/RP2350 was
  submodule-only at sdk 2.1.1; same cyw43 driver/behaviors as Pico W — but RP2350 dual-core
  SMP FreeRTOS ALREADY WORKS in `~/xiao_blocks/firmware/rp2350` (branch pico2): reuse its
  FreeRTOSConfig (cores=2 + CORE_AFFINITY) + `RP2350_ARM_NTZ` kernel port; XIAO RP2350 has
  no WiFi, so Pico2 W = that base + our CYW43/lwIP + `PICO_BOARD=pico2_w`.
- **Recovery ladder — no physical reset (full detail + SAMD51 prior-art in
  `docs/pico-wifi-research.md`).** Unlike a bare ESP32 (radio on-die), the CYW43 is a separate
  chip → recover in software: (1) supervisor polls link status + async rejoin (rejoin on
  link-down even if IP lingers); (2) after N failed rejoins (grace window), `cyw43_arch_deinit()`
  +`cyw43_arch_init()` resets just the radio (MCU keeps running); (3) the 4s HW watchdog
  self-resets a true MCU wedge → no human power-switch. Gap: "WiFi dead but tasks still
  heartbeating" won't trip the watchdog → the supervisor MUST catch it. The SAMD51 RTL8720
  port (`xiao_blocks/firmware/samd51`, ~6mo field-tuned) proves the same ladder + gives tuned
  params to copy: grace window before chip-reset (their 8 fails×1s), idle backstop (40×3s),
  don't-retry-after-data-loss→re-dial, WDT pet from a medium-prio task, per-exit-path trace
  counters, multi-network fallback, and a **host-side keep-alive** (agent pings the BC ~12s →
  W3 agent TODO). W3 acceptance test: kill WiFi at the AP, verify auto re-join + re-dial, no
  power-cycle.
- **Bench state:** master is flashed **bus_controller_wifi** (on WiFi `192.168.1.205`, dialing
  `192.168.1.66:47447`); config has `neti`. Restore the chain-tree/USB bench by flashing
  `bus_controller` (USB) — full config incl `neti` saved at `/tmp/cfg_full.uf2` on the Pi.

### W3 — host zenoh-agent + container (later)
Port `~/xiao_blocks/host/zenoh_agent/` (agent.lua + vendor/zenoh + Dockerfile) into
`slow_bus/host/zenoh_agent/`, TCP mode, map slow_bus appcore/bus commands to a zenoh key.
Router + agent containers on the Pi (`/mnt/ssd`). End-to-end: zenoh client → router → agent
→ WiFi → Pico W → reply.

### Bench (unchanged): master Pico W `E6616408437D6628`, slave plain Pico `E6605481DB611135`
(addr 9). Build/flash on the Pi (`ssh robot`, `/mnt/ssd/slow_bus`); discover ttyACM by serial
(re-enumerates on reboot). Host tooling: `tools/commission/lua/` (luajit). Memory:
`pico-wifi-zenoh` (active), `chaintree-dsl-source` (chain-tree, paused).

---

## UPDATE 2026-06-24 (d) — Thread 3 C3: chain-tree ENGINE ON THE SLAVE (engine↔engine) — DONE & HW-VERIFIED

**Objective REACHED (Glenn):** the chain-tree engine runs on BOTH master and slave, and a
node-to-node app message is handled by the SLAVE's own engine (`kbapp`). Commit `22fd910`.

- **Tagged event** (the B2/B3 work, finally earning its keep): `appcore_cmd_t`/`app_req_t`
  gain `{route, bus_src}`. `kbapp_on_echo` is origin-agnostic — reply rides `g_up_q`, the
  role-specific drain delivers it: **ROUTE_USB** → master uplink (USB; keeps the C1 ver
  byte); **ROUTE_BUS** → slave bus window (`[req][status][echo]`, no ver — the on-wire echo
  contract the master correlates in COLLECT).
- **Engine factored to be role-agnostic:** `engine_runtime_bringup()` (runtime create +
  activate KBs + bind ADC blackboard) shared by `app_engine_task` (master) and the new
  `node_engine_task` (slave); queue creation → `appcore_queues_init()`.
- **Slave engine path** (`node_role_run` → `node_engine_start`): engine task on **core1**
  (prio 3, below interlock tick prio 4) + a **core0 reply pump** draining `g_up_q` into
  `bus_node_queue` (keeps the bus TX queue single-core; engine never touches `g_txq`).
  Peripherals already up via `node_thread2_start`. `bus_node`'s `node_emit_ack()` fires on
  the DATA grant independent of the reply, so the async engine reply ships on a later POLL.
- **Slave responder routing:** `bus_node_on_data` calls `node_engine_try_route()` first —
  `CMD_APP_ECHO` → engine (async); echo/GPIO/interlock keep their sync `node_cmd_dispatch`.
- **Master originate** now sends `CMD_APP_ECHO` (was `NODE_CMD_ECHO`) so the SLAVE'S ENGINE
  answers. `node_cmd_dispatch` does NOT handle 0x0300 → a correct echo proves the engine.
- **HW proof:** same image on master `E6616408437D6628` + slave @ addr 9; two engine↔engine
  echoes exact (status=0), C1 local echo OK, **regression 11/11 PASS**, slave ALIVE misses=0.
- Test: `pico_app_echo.lua <port> --to 9 <msg>` (now exercises master kbapp → slave kbapp).

### PERF (measured 2026-06-24, 50-iter round-trips, master ↔ slave @ 9)
Engine messaging was capped by the **engine tick**, not the bus. Bumped `delta_time`
0.1→**0.01** (10 Hz → **100 Hz**) in `engine_runtime_bringup()` (commit `71715d1`):
- local engine echo: ~100 ms → **~10 ms (~98 msg/s)**
- node-to-node engine↔engine: ~100 ms → **~20 ms (~49 msg/s)** — now transport-bound
- bus-sync (no engine) baseline: **~17 ms** (the RS-485+USB floor, unchanged)
Decision (Glenn): keep 0.01; **make `delta_time` a per-unit config field later so special
nodes can tune their tick** (like `baud` in `idnt`). Cost of 100 Hz: more idle core1 CPU
walking KBs each tick (negligible for echo; revisit under heavy KB load). Interlock
unaffected (separate ~2 ms `il_tick_task`).

### DONE 2026-06-24 (e) — interlock clear from a KB (Thread 3 → Thread 2) — HW-VERIFIED (`03bbb4f`)
A chain-tree app event now clears the interlock's latched trips — the engine↔safety wiring
the interlock design wanted. `kbapp` 3rd column **`CMD_APP_IL_CLEAR`** (0x0302, event 25) →
`kbapp_on_il_clear` calls `interlock_request_global_clear()`. Route-aware via a new shared
`kbapp_reply()` (refactored out of echo) so it works engine-local (USB) and over the bus
(slave) identically; `node_engine_try_route()` also accepts it. Fail-safe preserved (clear
is a request applied next il tick; still-violated slot re-latches). HW: induce→latch held→
engine clear `gveto 1→0`; clear-while-violated re-latches; resolve+clear clears. 6/6 in
`pico_app_il_clear.lua`; echo unchanged; regression 11/11.
**IL polarity gotcha:** `watch[gpN:eq:0]` is SATISFIED at input=0 (drive the paired OUTPUT
LOW), VIOLATED at 1. `tf_state`: `IL_TF_TRUE=1`=OK, `IL_TF_FALSE=2`=violated (interlock.h).

### NEXT (Thread 3 hardening / follow-ons)
- **Per-node `delta_time` config field** (tick rate from `idnt`/config, default 0.01).
- **instance_id / "right app?" check** (design's build-&-identity model) — deferred from C3;
  cross-node validation via `OP_REGISTER` before trusting a peer's app.
- Bidirectional / slave-originated app messages; richer app KBs beyond echo.
- Then the deferred infra: I²C two-bus service; core-affinity review;
  `g_roster`(16) vs `core/bus_roster.c`(32) reconcile; retire the SAMD21 tree.
  (interlock chain-tree-event clear source — DONE, see update (e) above.)

---

## UPDATE 2026-06-24 (c) — Thread 3 C2: master engine originates node-to-node echo — DONE & HW-VERIFIED

**Objective (Glenn):** chain-tree engine on BOTH master and slave + node-to-node
(engine↔engine) messaging. Arc: C2 = master engine originates to the slave (slave answers
via dispatch); C3 = slave runs the engine too (engine↔engine). Commit `e355cf9`.

C2 = the master's `kbapp` ORIGINATES a bus message to a slave node and the master
**correlates** the reply (first node-to-node, master-initiated transfer):
- kbapp gained a 2nd column: **`CMD_APP_ECHO_TO`** (event 24) → `APP_ECHO_TO` one-shot.
- Thread 1 routes `CMD_APP_ECHO_TO`=0x0301 (args `[addr][payload]`) to kbapp via the same
  low-prio pointer-event/slot path as C1.
- `kbapp_on_echo_to` (core1) **can't block** on a bus round-trip → hands `{addr,
  host_req_id, payload}` to the bus thread via a new **core1→core0 queue `g_orig_q`** and
  returns (no reply pushed there).
- `bus_control_task` (core0): when idle it **promotes** one `g_orig_q` request into the
  in-flight slot (mints a master wire `req_id`, builds `OP_SHELL_EXEC` for
  `NODE_CMD_ECHO`), runs the existing INJECT/COLLECT, and on the slave's `OP_SHELL_REPLY`
  **re-tags the wire req_id back to the host's** (correlation), surfacing it from the
  appcore. Promote holds the lock across check+set (no race vs a host command);
  `g_cmd_is_orig=false` on the host path. (Seed of the tagged event `{source,reply_addr,
  req_id}`.)
- **Slave unchanged** — answers via existing `NODE_CMD_ECHO`/`node_cmd_dispatch`.
- Host test: `pico_app_echo.lua <port> --to <addr> [msg]`.
- HW proof (master `E6616408437D6628` → slave @ addr 9): two payloads echoed exact
  node-to-node (status=0), C1 local echo still OK, **regression 11/11 PASS**. (Slave kept
  its existing fw — C2 is master-side only.)

### NEXT — C3: chain-tree engine on the SLAVE (the end objective)
The slave currently runs `node_role_run()` (a responder loop), **not** the engine
(`app_engine_task` is master-path only in `main()`). C3 = make the slave also run the
engine so its own `kbapp` handles the app message (engine↔engine), per-node `instance_id`,
cross-node "right app?" check via `OP_REGISTER`. Then: I²C two-bus; interlock chain-tree
clear source; core-affinity review.

---

## UPDATE 2026-06-24 (b) — Thread 3 C1: app echo through the chain-tree engine — DONE & HW-VERIFIED

First Thread-3 milestone (`docs/thread3-plan.md` C1). A non-bench application opcode is
routed from Thread 1 up into the chain-tree engine and answered by the engine itself,
on the master alone. Commit `f96b21f`.

- **DSL source reconstructed (the big enabler).** The `.lua` that built `kb0.json` was
  never committed — only the IR + the json→C codegen were. Reconstructed it as
  **`app/bus_controller/kb0/kb0.lua`** (now the **authoritative source** for `kb0/incr/*`);
  it reproduces the committed IR (kb0/kb1/blackboard byte-identical) and adds `kbapp`.
  Regenerate with **`tools/gen_kb.sh`** (DSL → `kb0.json` → `incr/` via luajit;
  `/usr/bin/luajit` on dev box + Pi). Uses `--no-support`: the authoritative
  `chaintree_support.h` (carries the blackboard `bb_table` field this vendored codegen
  copy predates) lives in `engine/include/`, which wins on include order — don't let the
  pipeline emit a stale `incr/` copy. Whole `incr/` regenerates together (random
  `unique_id` per run + per-run `node_data_id`/`link_start`). See memory `chaintree-dsl-source`.
- **kbapp KB** = one column looping `WAIT(CMD_APP_ECHO, event 23) → APP_ECHO one-shot →
  reset`, mirroring kb0/kb1's command-column shape. Event ids stay 20/21/22/23 (build
  order kb0→kb1→kbapp).
- **Event system (Glenn):** the event value is a **variant** (int OR pointer). Two
  queues, **high + low**; `cfl_pop_event` drains ALL high then low. **High = jump ahead
  of FIFO ordering** — for state-machine changes + exception handlers (others only if
  they need that bypass). App traffic → **low**. `malloc_flag`: ChainTree began on Linux
  where the engine freed malloc'd pointer events at end of the event cycle; **NOT wired
  in this embedded build** (free only in `ring_clear`) → pass payloads via a **static
  slot pool, malloc_flag=false** (no heap).
- **Routing:** `cfl_embed_pre_tick` handles `CMD_APP_ECHO=0x0300` — stashes the payload
  in a 4-slot static ring and injects a LOW-priority **pointer** event at `g_kbapp_node`.
  Handler `kbapp_on_echo` (main.c; weak hook in `kb0/user_functions.c`) reads
  `event_data_ptr->data.ptr` → `OP_SHELL_REPLY [req_id][status][ver][echo]`.
- **Host test:** `tools/commission/lua/pico_app_echo.lua <bc_port> [msg]`.
- **HW proof (master Pico W `E6616408437D6628`):** flashed firmware-only (config
  preserved), two payloads echoed exact (`status=0 ver=1`), **regression 11/11 PASS**
  (no kb0/kb1 regression). NOTE: master re-enumerated to a different ttyACM after the
  reboot — discover by serial, don't hardcode the port.

### NEXT (Thread 3, from docs/thread3-plan.md)
- **C2 — master originates to the slave.** Firmware-originated bus inject + internal
  reply correlation: host "start app-echo to addr N" → master's app originates the bus
  message → slave's `kbapp` echoes → master correlates → returns to host. (This is where
  the **tagged event** `{source, reply_addr, req_id}` earns its keep — B2/B3.)
- **C3 — both ends in the engine** (slave's `kbapp` handles it, per-node instance_id).
- Then: I²C two-bus service; interlock chain-tree-event clear source (a KB handler calls
  `interlock_request_global_clear()`); core-affinity review.

---

## UPDATE 2026-06-24 — hwio + Thread 2 (interlock) DONE & HW-VERIFIED; Thread 1 B1 done

**Architecture:** `docs/three-thread-design.md` (read first). Build-order status there.
Interlock port detail: `docs/interlock-port-map.md`. Thread 3 plan: `docs/thread3-plan.md`.

### What's DONE this stretch (all committed + HW-verified on the two bench Picos)
- **PWM + quadrature dropped** to the Pico2 (RP2040 GP14/17/18 freed).
- **Build-order step 1 — `hwio` frozen pin-role config (HW-VERIFIED).** `node/boot_hwio.{c,h}`
  (CBOR reader, mirrors `boot_identity.c`), `cfg_image.lua --io/--adc`, `hwio_apply()` at boot.
  `CMD_GPIO_CONFIG` retired; HIL is **operate-only**, validated per frozen role.
- **Build-order step 4 — Thread 2 interlock port (HW-VERIFIED, both roles).** Ported the
  proven SAMD21 framework into `node/interlock/` (`interlock.{c,h}`, `interlock_dsl.c`,
  `il_hal{.h,_rp2040.c}`, `il_pin_table.{c,h}`). Platform swaps: `.uninitialized_data`
  persistence, `il_panic`/`watchdog_reboot`, HardFault dropped (chassis owns faults),
  `emit_op_event`→shared-status. **Extended to Glenn's model:**
  - **10 boolean interlock slots** (`INTERLOCK_MAX_SLOTS=10`), usually mostly EMPTY;
    output = the **UNION (OR) of their LATCHED states** on the shared **GP0** veto.
  - **Latched** trips: a slot stays vetoing after its input recovers, until an
    **event-driven GLOBAL CLEAR** (`interlock_request_global_clear()` from a USB/bus
    command `CMD_INTERLOCK_CLEAR=0x0210`; chain-tree-event source lands with Thread 3).
    Clear is **fail-safe**: a still-violated slot re-latches the same tick.
  - **ADC = shared read resource** (any slot/bench/telemetry reads `g_adc_latest`).
  - Per-slot config `ilc0..ilc9` (`cfg_image.lua --il "N:<dsl>"`); absent = empty.
  - **Config-fingerprint re-arm:** reflashing `ilc` config takes effect WITHOUT a power
    cycle (persist v10 `cfg_fingerprint`); unchanged config + warm reset preserves latch.
  - **Role-agnostic:** the SLAVE runs the same interlock (`node_thread2_start`), driven
    over the bus. **Fast veto:** the `il` task ticks ~2 ms at priority 4 (above the engine).
  - Status: `CMD_INTERLOCK_STATUS=0x0211` → `[ver][gveto][n]+n*[slot][state][tf][latched]`;
    picolink `Link:il_status()/il_clear()`.
  - HW proof: single trip→latch→hold→clear→re-latch AND the multi-slot union, on BOTH nodes.
- **Build-order step 2 — Thread 1 B1 (done).** Unified `node_cmd_dispatch()` (echo / GPIO /
  interlock) shared by the master appcore drain and the slave bus responder — deletes the
  duplicated slave dispatch. **Remaining B (B2/B3 tagged events + symmetric reply) is a no-op
  until Thread 3 gives it a second consumer — fold it into Thread 3 C1 (see thread3-plan.md).**

### NEXT (recommended order)
1. **Thread 3 — chain-tree app echo PoC** (`docs/thread3-plan.md`, increments C1→C3). This is
   the next real milestone; B2/B3 fold into C1. Needs chain-tree IR tooling + an engine
   "originate RS-485" path — a fresh-context-sized build.
2. **I²C two-bus service** (deferred by Glenn) — unblocks the interlock's I²C-mirror input +
   freshness fail-safe (build-order step 3).
3. Core-affinity thread-review (isolate interlock + ADC ISR on their own core).

### BENCH STATE (the two Picos on the Pi)
- **Master** = Pico W, UID **`E6616408437D6628`**, bus addr **0** (variant 1). Talk to its
  appcore at picolink addr **`0xFB`**.
- **Slave** = plain Pico, UID **`E6605481DB611135`**, bus addr **9** (variant 3). Reach it via
  the master relay: `lk:exec(9, …)`.
- Both run the latest `bus_controller.uf2` with a **2-interlock TEST config**: `hwio` GP2=OUT,
  GP3=IN(pulldown), GP4=OUT, GP5=IN(pulldown); `ilc0`=`watch[gp3:eq:0]`, `ilc1`=`watch[gp5:eq:0]`,
  both → GP0 veto. **Jumpers on BOTH boards: GP2↔GP3 (pins 4↔5) and GP4↔GP5 (pins 6↔7).**
  (Reflash clean no-interlock configs if you don't want the test interlocks armed.)
- Bus healthy (master polls slave, 0 misses); both `idnt`s intact.

### BUILD / FLASH (post-Pi-SD-crash 2026-06-23 — paths moved to /mnt/ssd; see memory pi-ssd-rebuild)
- Pi `ssh robot` (192.168.1.66). All build software on the **SSD at `/mnt/ssd`**; `~/pico` →
  `/mnt/ssd/pico` symlinked. **slow_bus is at `/mnt/ssd/slow_bus`** (`~/slow_bus` symlinked).
- Build: `rsync -az --delete --exclude '.git' --exclude 'build' ./ robot:slow_bus/` then
  `ssh robot 'cd slow_bus && bash tools/pi-build.sh bus_controller'` (sources `tools/pi-env.sh`,
  which now adds the SSD arm-gnu-toolchain-14.2 to PATH — the apt arm-gcc is gone). cmake 4.x
  in `~/.local/bin`. `il_thread2_check` is an isolated `-Werror` compile target for the
  interlock module.
- **picotool** (v2.2.0, rebuilt to `~/.local/bin` — NOT on the default non-login ssh PATH, so
  prefix `. /mnt/ssd/pico/env.sh`). **Flashing syntax (verified 2026-06-24, supersedes the old
  gotchas below): device-selection flags go BEFORE the `<filename>`, and `reboot` accepts them.**
  - Reflash firmware only (preserves the config region = `idnt`/`hwio`/`ilc`):
    `picotool load --ser <UID> -f -x /home/pi/slow_bus/build/bus_controller.uf2` (`-f`→BOOTSEL,
    `-x`→run; ABSOLUTE path).
  - Reflash config (entries share one 4 KB sector → ALL in one UF2): `picotool reboot --ser <UID>
    -f -u` → `picotool load /tmp/cfg.uf2` → `picotool load -u -x <fw>.uf2`.
  - NOTE: a `load -x`/firmware reboot can clobber the crash slot (`rst=POWER`) but the interlock
    persist may survive → config changes are picked up via the **cfg_fingerprint**, not a cold boot.
- Host tooling (pure-Lua, on the Pi): `cd /mnt/ssd/slow_bus/tools/commission/lua && luajit …`.
  `picolink.lua` (`enumerate`/`open`/`exec`/`il_status`/`il_clear`), `cfg_image.lua` (build
  config UF2s). Bench test scripts were written to `/tmp/*.lua` (union2/echo/etc. — re-derive).

### KEY GOTCHAS / LEARNINGS
- **GCC 14** (post-crash toolchain) is stricter: `-Werror=address-of-packed-member` on the wire
  status buffer — evaluate into a local then copy element-wise (don't pass a `uint16_t*` into a
  packed struct). The SDK has its own `panic()` → the interlock's is `il_panic`.
- **Interlock config change needs the cfg_fingerprint** (already wired) — a plain reflash+soft-reboot
  warm-restores the OLD armed set otherwise.
- `INTERLOCK_MAX_SLOTS` bumps trip two guards: the slot bitmask `<=16` assert (now `il_slotmask_t`
  =uint16) and the `il_status_buffer` size (now scales with the slot count).

---

## UPDATE 2026-06-22 — SINGLE IMAGE (role from config) DONE + HW-VERIFIED

**ONE binary now serves BOTH roles** (Glenn: "same image; the flashed config picks
master/slave + bus speed"). Flash the same `bus_controller.uf2` to every board; the
per-unit `idnt` config's variant (`vr`) decides master vs slave at boot.

What changed:
- **Hoist**: the shared identity/config stack moved `app/bus_controller/` → `node/`
  (`boot_identity`, `boot_roster`, `cfg_file`, `cbor_min`, `variants`). The slave no
  longer reaches into the BC dir. Host CBOR test now builds with `-I../node`.
- **node/node_role.{c,h}**: the slave/node role (responder + `node_role_run()`),
  lifted from the old `app/slave/main.c` (DELETED), minus its main()/FreeRTOS hooks.
- **app/bus_controller/main.c**: after `boot_read_identity`, a ROLE BRANCH — a
  commissioned slave (`!variant_is_master(ident.variant)`) calls `node_role_run()`
  (never returns); master variants + uncommissioned/refused units fall through to the
  (UNCHANGED) master path, which self-quarantines its arbiter on a bad identity.
- **node/boot_identity.c + variants.h**: the variant check `vr == BUILD_VARIANT` →
  `variant_supported(vr)` (the SET of variants this image implements: USB-ctrl master
  + RS-485 slave). Chip + UUID guards UNCHANGED — those are the real mis-flash guards.
- **CMakeLists.txt**: the `bus_controller` target is now the unified image (added
  `node/node_role.c`, `core/bus_node.c`, `port/rp2040/board.c`); the separate `slave`
  target is REMOVED.

Size: unified text 167,932 B (+1.3 KB vs the old master), bss 101,672 B (+2.2 KB).
~160 KB `.bin` = ~8% of the RP2040's 2 MB flash; ~155 KB of 264 KB SRAM free.
(Pico / Pico W = RP2040 = 264 KB SRAM; Pico 2 = RP2350 = 520 KB. Bench builds rp2040.)

**HW-VERIFIED 2026-06-22** on the two bench Picos (built on the Pi):
- Flash (same .uf2 to both): `sudo picotool load -x -f --ser <serial> ~/slow_bus/build/bus_controller.uf2`.
  Master = Pico W `E6616408437D6628` (ttyACM2); slave = plain Pico `E6605481DB611135` (ttyACM3).
- Master banner: `boot#2 rst=POWER ident=0 slvr=1` (read vr=1 → master, roster loaded).
- Master roster: `addr=0x09 variant=3 state=ALIVE misses=0` (the SAME image on the
  plain Pico booted as the slave from vr=3, answering RS-485 polls).
- Round-trip: `CMD_ECHO -> addr 0x09: status=0 result="hello"` — DATA path OK.
- Configs SURVIVE the firmware flash (config region = top 64 KB, a separate flash region).

**ALSO DONE + HW-VERIFIED 2026-06-22 (commits `9cdda65`, `ae6b660`):**
- **Bus speed from config** (`9cdda65`): optional `sp` (baud) field in `idnt`; both roles
  `bus_phy_init(sp ? sp : BUS_DEFAULT_BAUD)`. `identity_t.baud` (0 = absent). `cfg_image.lua
  --speed`. Master boot banner now shows `baud=<n>`. Backward-compatible (no `sp` → 115200).
  VERIFIED end-to-end: re-commissioned BOTH idnt at `--speed 230400`, master banner read
  `baud=230400`, slave `ALIVE misses=0`, round-trip OK at 230400 → then reverted both to
  default 115200 (bench is back on baseline).
- **Slave USB reflash without BOOTSEL** (`ae6b660`): `node_task` used `taskYIELD()` (yields
  only to >= priority), which starved the `pico_async_context` USB worker → a board running
  the slave role couldn't be force-rebooted by picotool. Fix: `vTaskDelay(1)` so the USB
  worker runs (RX is IRQ-fed; 1 ms wake << the ~2 ms POLL window). VERIFIED: a slave on the
  fixed fw was force-rebooted + reflashed over USB, no BOOTSEL button.

**FLASHING GOTCHAS (picotool 2.2.0, two RP devices on the Pi — learned the hard way):**
- `--ser` does NOT disambiguate two RUNNING devices for `-f`; use `--bus/--address`
  (map serial→bus/dev from `/sys/bus/usb/devices/*/{serial,busnum,devnum}`; in BOOTSEL,
  `picotool info -a --bus 1 --address N` shows the `flash id` = board UID).
- `reboot` rejects `--ser/--bus/--address` → can't target a reboot among two devices.
- `load -x` on a CONFIG uf2 fails ("no valid executable") — config has no entry point.
- RELIABLE CONFIG FLASH: force ONE board to BOOTSEL (it becomes the single BOOTSEL device),
  then operate UNTARGETED: `picotool load -x -f --bus B --address A <cfg>.uf2` (rc 254, -x
  fails but it's now in BOOTSEL) → `picotool load <cfg>.uf2` (writes config) → `picotool
  load -x <bus_controller>.uf2` (reboots to run; firmware region is separate from config).

**BASELINE 2026-06-22** (git tag `baseline-2026-06-22`): single image + role-from-config +
speed-from-config + slave USB-reflash — all HW-verified; bench reverted to the 115200 default;
working tree clean + pushed. The working KBs are **KB0 (monitor)** and **KB1 (HIL/API surface)**.

## ARCHITECTURE REVISED 2026-06-23 — THREE-THREAD MODEL → see `docs/three-thread-design.md`

**Read `docs/three-thread-design.md` FIRST.** The KB0–KB4-as-chain-KBs plan (and the
KB2/KB3-chain interlock plan that was here) is **SUPERSEDED.** The firmware is now three
threads + a transport layer below them, with **hardware FROZEN at config time** (no runtime
reconfiguration). Headline decisions (all agreed in chat 2026-06-23):
- **Thread 1** = I/O router + the bench surface (monitor + HIL-**operate** + commission);
  routes only non-bench → events to the chain-tree. Absorbs old KB0+KB1.
- **Thread 2** = the **interlock**, a **port of the proven SAMD21 design** (`~/xiao_blocks/
  firmware/samd21/device/samd21_interlocks.*` + its DSL) — **NOT** a chain-tree KB. Tick-driven,
  local I/O only, **GPIO veto**, **shared-status-only** coupling (status word out, `rearm` flag in).
- **Thread 3** = the chain-tree engine, now **pure application** (non-bench events in, RS-485 out).
- **I²C** = one service: periodic-sample (INA219, …) → shared area + intermixed async; the
  interlock reads the mirror with a **freshness fail-safe**. Veto stays on GPIO.
- **KB4 dropped** (frozen config → static pin-ownership, no dynamic allocation).
- All config frozen in the config-FS: `hwio` (pin roles), `ilcf` (interlock DSL), I²C inventory.

SAFETY INVARIANT (preserve): **the veto never depends on any engine/thread being healthy** —
now structural (the interlock is a separate thread + the ADC ISR). The interlock is the
**safety-critical part — do it right.**

**Build order** (from the design note; verify between each):
1. `hwio` config (schema + host builder + boot reader applying pin roles).
2. Thread 1 — router + bench (operate-only HIL, validated against `hwio`).
3. I²C service (periodic-sample → shared area + async; inventory in config).
4. **Thread 2 — the SAMD21 interlock port** (HAL→RP2040, shared-status coupling, I²C-mirror
   input + freshness fail-safe).
5. Thread 3 — the chain-tree application.

**Deferred:** final core affinities + slave-responder timing (thread-review pass); Pico2/RP2350
build (the "pico AND pico2" half — only rp2040 verified); `g_roster`(16) vs
`core/bus_roster.c`(32) reconcile; retire the SAMD21 tree; later, the Pi wireless proxy.

---

## TL;DR — where we are (as of 2026-06-16)

The bus + 5 SAMD21 modes + commission toolchain are **done and HW-verified**
(see git history + memory). The **restructure** to the shared Pico/Pico2 model is
well underway. Branch: **`samd21-namespace-db`** — **pushed through `ef2a606`**
(working tree clean).

**A FULLY COMMISSIONED TWO-PICO RS-485 BUS IS WORKING + HW-verified (2026-06-16):**
- **Test #1 complete**: single USB bus controller, two-step flash, per-unit
  identity from a read-only config-FS (`ident=0`; mis-flash → `ident=-6 REFUSED`
  quarantine), 11/11 workbench regression.
- **Two-Pico bus**: BC (Pico W) + slave (plain Pico) over RS-485 (auto-direction
  transceivers, TX=GP15/RX=GP16). Liveness (ALIVE/DEAD), **slave identity
  commissioned from the config-FS** (slave moved to addr `0x09` via its `idnt`),
  **DATA round-trip** (BC↔slave `CMD_ECHO`), and the **`slvr` roster** (BC loads
  its roster from config and polls autonomously, no host register).
- **Found+fixed a latent CBOR decoder bug** (`cbor_min.h`, n≥24 values decoded
  wrong — surfaced as a 6.3 s poll period); added a host vector test
  (`make -C tools test`). Confirmed the bug is NOT in `~/xiao_blocks` (no on-device
  CBOR decoder there).

**Config-FS format = SAMD21 boot-store entries, READ-ONLY** (picotool-flashed, no
runtime writes → the RP2040/RP2350 XIP-vs-multicore-lockout hazard never arises).

**Tomorrow's options (pick one):** (1) **hoist** the identity/config stack
(`boot_identity`, `boot_roster`, `cfg_file`, `cbor_min`, `variants`) from
`app/bus_controller/` to a shared `node/` dir — it's now used by BOTH the BC and
the slave (the slave reaches into `app/bus_controller/` via a CMake include, which
is the one ugliness left); (2) the `g_roster` (cap 16) vs `core/bus_roster.c`
(cap 32) reconcile; (3) more node app logic (real commands beyond CMD_ECHO).

The LuaJIT Pico toolchain lives in `tools/commission/lua/`: `pico.lua`,
`picolink.lua`, `cfg_image.lua`, `pico_regress.lua`, `pico_bus.lua`,
`pico_slave.lua`, `pico_roster.lua`.

### Hardware bench state (the two Picos on the Pi)
- **BC** = Pico W, serial **`E6616408437D6628`**, runs `bus_controller.uf2`; config
  region has idnt (vr=1 USB-BC, addr 0x00) + slvr (polls slave 0x09).
- **Slave** = plain Pico (RP2040), serial **`E6605481DB611135`**, runs `slave.uf2`;
  config region has idnt (vr=3 SLAVE_RS485, addr `0x09`).
- Wired RS-485 (auto-direction): each `GP15`=TX→transceiver, `GP16`=RX←transceiver.
- **Multi-Pico flashing (picotool v2.2.0):** target by serial — `sudo picotool
  load -x -f --ser <serial> <uf2>`. The `reboot` subcmd rejects `--ser`/`--bus`,
  but `load -f --ser …` works. Find the BC's ttyACM by serial:
  `for t in /dev/ttyACM*; do … /sys/class/tty/$(basename $t)/device/../serial …`.
- Build on the Pi: `slow_bus/tools/pi-build.sh {bus_controller|slave}`.
- Quick checks (run on Pi in `tools/commission/lua/`): `luajit pico.lua listen 2
  <bc_port>` (banner), `luajit pico_roster.lua <bc_port> 3` (roster, watch mode),
  `luajit pico_slave.lua <bc_port> 9 hello` (round-trip).

---

## The restructure (decided today)

1. **SAMD21 leaves** → moving to `~/xiao_blocks` in a few days. *Do not delete
   the SAMD21 tree yet* — leave it ~a few days, then remove. When it goes, the
   README's "heterogeneous / SAMD21 master-only / filters peer traffic" model
   goes with it.
2. **This repo becomes** the shared **Pico/Pico2 bus software** (bus controller
   **and** slave) + a later **Pi wireless proxy**. Same software base serves
   slow_bus, fast_bus, and the BC role.
3. **Image vs config split (avoids the N×M artifact matrix):**
   - **Behavior is baked into the image** at build time: `{master|slave} ×
     {rs485|wifi} × {rp2040|rp2350} × variant`. A given image is identical across
     every unit of its class.
   - **Per-unit / per-deployment data lives in a read-only LittleFS** config FS
     in a reserved flash region, **flashed separately** (the "two-step flash").
     Built **incrementally** — one file at a time. Identity file first.
4. **Homogeneous bus**: every node has a crystal, runs high-speed, and is
   peer-capable. Peer capability is a bus-wide invariant, **not** a per-slave
   flag.

### Config-FS constraints (carried from the SAMD21 boot store)
Read-only at runtime, DSL-generated. **4-char filenames, ≤256 B/file, ≤32
files, ≤64 KB total.** CBOR-encoded. Per-unit config is bound to the board's
hardware UUID so it physically cannot boot on the wrong unit.

### The identity file (`idnt`) — first config file
One generic image reads it to validate + personalize. Fields (CBOR map):
- `v`  schema_ver (contract guard)
- `ch` chip: `pico`(0)/`pico2`(1) — vs `BUILD_CHIP`
- `vr` variant (product/hw-layout code, shared enum) — vs `BUILD_VARIANT`;
  **role is derived from variant** (`variant_is_master`)
- `ad` own RS-485 address (master=0x00; slave 0x01..0x7E) — the one
  load-bearing per-unit field (→ PHY RX address filter)
- `id` board hardware UUID (8 B) — vs `pico_get_unique_board_id()` (hard
  mis-flash guard)

Boot policy: **hard-refuse on mismatch** of chip/variant/uuid/addr (Step 4);
today it's log-only.

### The roster file (`slvr`) — designed, not yet wired
Master-only. CBOR `{ v, p(grant_period_ms), w(window_us), m(max_misses),
r(tcp_retries), s:[[addr,variant,flags], …] }`. **Positional arrays + a 1-byte
variant code (NOT the 32-bit class_id)** so 32 slaves fit 256 B. `flags` =
`ENABLED|TCP` (peer is universal, not encoded). Array order = poll order.

### Boot sequence (target)
```
common:  read+validate identity → crash slot → watchdog → heartbeats → RS-485 PHY @ speed
master:  load slvr (roster + sched) → uplink (usb now; wifi+net file later)
         → core1 chain_tree: KB0 monitor, KB1 api/HIL, KB2/KB3 interlocks = 0 (bench-armed)
         → init hardware (HIL per variant)
slave:   PHY RX filter = own addr → respond-only → variant hw init → housekeeping
```
Interlocks (KB2/KB3) **boot to 0**, configured by **bench commands only** for
now — no interlock file yet (they're DSL-heavy, can hit 256 B each, want their
own region/format later).

---

## Test #1 (the near-term goal)

**Single USB bus controller, no slave, no interlock, run workbench API, exercise
the two-step flash.** The thin vertical slice that proves the new pipeline with
almost no new firmware.

### 5-step plan (do in order, verify between)
1. **DONE today** — wire `idnt`/`slvr` boot readers into `main.c`, `cfg_load()`
   stubbed → reads ABSENT → graceful fallback to baked defaults. No behavior
   change but a boot-log field. Builds clean on the Pi.
2. **NEXT** — config-FS region + real `cfg_load()`: reserve a flash window in the
   linker memmap (`__cfg_start/_len`), read-only LittleFS mount, implement
   `cfg_load`. Region: **64 KB at top of flash (0x101F0000 on the 2 MB Pico W)**,
   block 4096 × 16. Verify with a self-test that reads a known file → `OP_DBG_LOG`.
3. Host config-image builder + **UUID read**: DSL → `idnt` → LittleFS image
   stamped with the board's UUID. Verify firmware-logged identity matches.
4. Flip missing/mismatch → **hard refuse** (`chassis_panic(RST_PANIC, code)`).
   Deliberately flash a bad `idnt` once to watch the refuse fire.
5. Workbench regression pass over USB.

### Decisions locked for test #1
- UUID check **real** now, with `-DIDENT_SOFT_UUID` escape hatch.
- Config region **64 KB @ 0x101F0000** (placeholder; revisit at real FS design).
- `slvr` **absent-tolerated** → the only config artifact flashed is `idnt`.

---

## Step 1 — what landed today (in this commit)

New files in `app/bus_controller/`:
- `variants.h` — variant enum + `variant_is_master()`; `BUILD_VARIANT` defaults
  to `VARIANT_BUS_CTRL_USB`. **Shared enum** the host config-gen must also import.
- `cfg_file.h` / `cfg_file.c` — FS seam; **`cfg_load()` is a stub returning -1**
  (real LittleFS body = Step 2). Only this function changes when the FS lands.
- `cbor_min.h` — bounds-checked minimal CBOR decoder (host-syntax-checked).
- `boot_identity.h` / `boot_identity.c` — `boot_read_identity()`.

Edits: `main.c` (include + read identity after `boot_count++` + `ident=%d` in the
boot banner); `CMakeLists.txt` (added the two `.c` to the `bus_controller` target).

### Deferred (flagged, NOT done) — read before Step 2
1. **Roster wiring blocked on a reconciliation:** `main.c` uses its OWN local
   `g_roster` (`slave_t`, cap 16); `core/bus_roster.c` (`bus_slave_t`, cap 32,
   what the drafted `boot_roster.c` targets) **isn't even compiled into the BC
   target**. Reconcile these before wiring `slvr`. Test #1 has no slaves, so
   `slvr`/`boot_roster.c` are intentionally unwired (boot_roster.c not created;
   its draft lives in today's chat transcript / re-derive from `boot_identity.c`).
2. **Hard-refuse** on mismatch — Step 4 (today log-only).
3. **`ident`→`addr`/instance rewiring** — still using baked `#define`s until the
   file carries real values (Step 3).

---

## Build & deploy mechanism (the "Pi loop")

**Roles:** dev box (WSL, this machine) = edit + host compile-check only (no Pico
SDK here). **Pi (`robot`, 192.168.1.66, `pi@raspberrypi`) = build + flash + run.**

**Pi facts:** SDK `/home/pi/pico/pico-sdk`, FreeRTOS
`/home/pi/pico/FreeRTOS-Kernel`, cmake 4.x in `~/.local/bin` (system cmake too
old), arm-gcc **8.3.1 (apt)** at `/usr/bin` (pinned xPack not installed on the
Pi; 8.3.1 builds it fine), `picotool` at `/usr/local/bin`. `~/slow_bus` is a
**synced snapshot, NOT a git checkout**; `PICO_SDK_PATH`/`FREERTOS_KERNEL_PATH`
are **not** in the login shell.

**Helper scripts (committed in this repo):**
- `tools/pi-env.sh` — exports SDK/FreeRTOS + `~/.local/bin` PATH (Pi-specific).
- `tools/pi-build.sh [target]` — run on the Pi: source env, configure-if-fresh,
  build → prints the `.uf2`.
- `tools/deploy.sh [target] [--flash]` — run on the dev box: rsync → remote build
  → optional `picotool load`.

**One-command loop (from the dev box):**
```sh
tools/deploy.sh bus_controller            # sync + build on the Pi
tools/deploy.sh bus_controller --flash    # + flash (Pico in BOOTSEL)
```
Manual equivalent (what was run today, verified):
```sh
rsync -az --exclude '.git' --exclude 'build' ~/slow_bus/ robot:slow_bus/
ssh robot 'bash -lc "export PICO_SDK_PATH=/home/pi/pico/pico-sdk; \
  export FREERTOS_KERNEL_PATH=/home/pi/pico/FreeRTOS-Kernel; \
  cmake --build ~/slow_bus/build --target bus_controller"'
```

**Flashing:** Pico in BOOTSEL (button, or `picotool reboot -f -u` if the running
fw exposes the USB reset iface), then `picotool load -x build/bus_controller.uf2`.
`picotool` is **RP-only** → safe to run while SAMD21 ACM ports are on USB.

**Observing:** USB-CDC is **binary libcomm, not text** (no `cat /dev/ttyACM*`).
Point `tools/commission/` (`bench.py` or Lua `libcomm`) at the Pico's CDC port to
decode `OP_DBG_LOG` (expect `[boot] bus_controller boot#N rst=POWER ident=-1`)
and drive the KB0/KB1 API (commands to appcore `0xFB`).

---

## Resume checklist (transparent across the WSL/Windows weekly reboot)
- [x] All Step-1 work committed on `samd21-namespace-db` (EOD 2026-06-15).
- [x] Pi has today's source (rsynced) + a clean `bus_controller.uf2` build.
- [x] Memory updated (`pico-restructure`, `pico-build-deploy`) → loads each session.
- [x] **Step 2a DONE + HW-verified (2026-06-16).** Config-FS format decided:
      **reuse the SAMD21 boot-store entry format, READ-ONLY** (not LittleFS, not a
      new blob) — `app/bus_controller/cfg_file.c` scans the top 64 KB of flash
      (256×256-B rows, magic `0x10C0FFEE` + seq + name[4] + len + CRC-8/AUTOSAR +
      ≤240 B), latest-seq-wins, pure XIP reads (no flash writes → no dual-core
      hazard). Boot-time `cfg_layout_ok(&__flash_binary_end)` guard → PANIC 0x10 if
      the image overlaps the region. Verified: erased region → `ident=-1`,
      `rst=POWER` (guard passed).
- [x] **Step 2b DONE + HW-verified (2026-06-16).** Host image builder
      `tools/commission/lua/cfg_image.lua` builds the `idnt` CBOR
      `{v,ch,vr,ad,id}` (cbor.lua gained `cbor.bytes()` for the byte-string UID),
      frames it into a 256-B store entry, emits a UF2 at the region base
      (0x101F0000), and **auto-detects the board UID over libcomm** (picolink
      OP_REGISTER) so the image is bound to the unit. Two-step flash verified on
      the Pico W: firmware UF2 + separate `cfg.uf2` → `ident=0` (IDENT_OK, UUID
      matched). Negative test: wrong-UID image → `ident=-6` (IDENT_ERR_UUID), so
      the mis-flash guard is real. Flash recipe: `picotool reboot -f -u` →
      `picotool load cfg.uf2` (no -x) → `picotool reboot`.
- [x] **Step 2c / Step 4 DONE + HW-verified (2026-06-16).** `main.c` now ACTS on
      the identity: OK → operate using `ident.addr` (0x00 for a master); MISSING →
      tolerate (baked defaults), with `-DIDENT_REQUIRE_PRESENT` to refuse instead
      (production lockdown); MISMATCH (FORMAT/SCHEMA/CHIP/VARIANT/UUID/ADDR) →
      **REFUSE**. Refuse is a *quarantine*, NOT `chassis_panic` (that
      watchdog_reboots → a persistent mismatch boot-loops, undiagnosable): the unit
      boots far enough to stay diagnosable (banner shows `ident=<code> REFUSED`,
      ping answers) but `bus_control_task` early-continues so the arbiter never
      drives the wire. Verified: good→`ident=0` operational; wrong-UID→`ident=-6
      REFUSED` with ping still OK; restored→`ident=0`.
- [x] **Step 5 workbench regression DONE + HW-verified (2026-06-16).**
      `tools/commission/lua/pico_regress.lua` — 11/11 PASS, exit 0. Covers both
      frame routes (appcore 0xFB: REGISTER identity + MON_PING; local shell 0x00:
      ECHO with SLIP-escape-heavy payload byte-exact round-trip, unknown-cmd error
      path, roster CRUD with class_id round-trip, SET_POLL/POLL_ENABLE). No slaves,
      no HIL pins -> bench-safe. **TEST #1 COMPLETE (all 5 steps done + HW-verified).**
- [x] **Two-Pico RS-485 bus bring-up DONE + HW-verified (2026-06-16).** 2nd Pico
      (plain Pico, RP2040, serial `E6605481DB611135` @ ttyACM1) flashed with the
      `slave` image (`NODE_ADDR=0x01`, echoes DATA; needed a one-line `chassis_assert`
      to link). Wiring: TX=GP15, RX=GP16, **auto-direction transceivers (no DE pin
      — firmware already DE-less), so no fw change.** BC (ttyACM0) driven over USB by
      `tools/commission/lua/pico_bus.lua`: register slave ENABLED + poll. Result:
      addr 0x01 → `state=ALIVE misses=0`; absent addr 0x05 → `state=DEAD` +
      `OP_BUS_SLAVE_DOWN`. (Note: a fresh UNKNOWN→ALIVE is announced SILENTLY; only
      DEAD→ALIVE emits `OP_BUS_SLAVE_UP` — check roster state, not the event.)
      Multi-device picotool: target by serial with `-f --ser <serial>` (the v2.2.0
      `reboot` subcmd rejects `--ser`/`--bus`, but `load -f --ser …` works).
- [x] **Slave identity from config-FS DONE + HW-verified (2026-06-16).** The slave
      now reads its RS-485 address from `idnt` (same config-FS as the BC — the "one
      base" goal). Wiring: `app/slave/main.c` calls `boot_read_identity` →
      OK=use `ident.addr`, MISSING=fall back to baked `NODE_ADDR`, MISMATCH=refuse
      (don't init the node → stays silent → BC ages it DEAD). CMake: the slave
      target compiles `boot_identity.c`+`cfg_file.c` (from `app/bus_controller/` —
      TODO hoist to a shared `node/` dir), `-DBUILD_VARIANT=VARIANT_SLAVE_RS485`,
      links `pico_unique_id`. `cfg_file.c` now uses `core`'s `bus_crc8_update`
      (byte-identical to the vendored libcomm CRC) so it links into the slave too —
      no BC regression (re-verified `ident=0` + 11/11). Two-stage HW proof on the
      2nd Pico: (A) no config → fallback `0x01` ALIVE; (B) flash `idnt{vr=3,ad=9,
      id=E660…1135}` → addr `0x09` ALIVE, old `0x01` DEAD (the slave *moved* to the
      commissioned addr). `cfg_image.lua --uid <hex> --variant 3 --addr N`.
- [x] **DATA round-trip DONE + HW-verified (2026-06-16).** Full command path:
      host → BC (USB) → RS-485 → slave → RS-485 → BC → host. `app/slave/main.c`
      `bus_node_on_data` is now a minimal responder (parses the BC-injected
      `[opcode][req_id][cmd][args]`, handles `CMD_ECHO` → `OP_SHELL_REPLY`).
      **Protocol fix in `core/bus_node.c`:** the skeleton emitted its reply
      in-window *immediately* on a DATA grant, but the BC's `BS_CMD_INJECT` is
      async two-phase — it wants a `BUS_FT_ACK` first (40 ms), then POLLs to
      collect. Node now ACKs a BC DATA grant and ships the reply on the next POLL.
      (`bus_node.c` is slave-only — the BC has its own arbiter — so the BC is
      untouched; regression still 11/11.) Driver `pico_slave.lua`; verified across
      payloads → `status=0`, exact echo.
- [x] **`slvr` roster from config-FS DONE + HW-verified (2026-06-16).** The BC loads
      its slave roster + poll schedule from the `slvr` config file at boot and polls
      autonomously — no host registration. `app/bus_controller/boot_roster.c` parses
      `slvr` CBOR `{v,p,w,m,r,s:[[addr,variant,flags],…]}`; `bc_load_cfg_roster()`
      populates `g_roster`/poll params and is re-run on every host-disconnect re-arm
      so the commissioned roster survives host churn (host registrations are
      transient overrides). `cfg_image.lua` now emits a MULTI-entry image (idnt+slvr
      in one UF2 — they share a 4 KB sector, so a separate load would wipe the
      other). Verified: BC boots `slvr=1`, polls slave `0x09` ALIVE with no host
      register, round-trip OK, regression 11/11.
      **Found+fixed a latent CBOR bug** (`cbor_min.h`): `cbor_hdr` initialized the
      value to the additional-info nibble for n≥24, so any value/length/count ≥24
      decoded wrong (200 → `0x18C8` = 6344). idnt dodged it (all values <24); `slvr`
      period 200 was the first to hit it. Fix: `v = (n<24)?n:0`. Tools:
      `pico_roster.lua`; `cfg_image.lua --slvr "addr:variant:flags" --poll p:m:r`.
      **NEXT** = (a) hoist the identity/config stack to a shared `node/` dir;
      (b) push the local commits; (c) the `g_roster`/`core/bus_roster.c` reconcile.
- [x] **Step 1 flashed + HW-verified on a Pico W (2026-06-16).** UID
      `E6616408437D6628`. Live libcomm round-trip works (appcore MON_PING reply;
      OP_REGISTER decoded: class 0x5E589000, fw 256, build 20260607). The UID over
      the link matches `pico_get_unique_board_id()` == the future `idnt` `id`
      field. Tooling: `tools/commission/lua/pico.lua` (+ `picolink.lua`).
- [ ] **NEXT: Step 2** — config-FS region + real `cfg_load`.

### Pico host tooling (LuaJIT — matches the stock-Pi, no-pip toolchain)
`tools/commission/lua/picolink.lua` is the Pico USB-CDC libcomm client (SEPARATE
from the departing SAMD21 `libcomm.lua`; frame addr is a parameter — 0x00 local
shell, 0xFB appcore, 0x01 s2m). CLI `pico.lua`: `info` (decode OP_REGISTER),
`ping` (appcore KB0 round-trip), `listen [secs]` (passive DBG_LOG/frame decode),
`port`. Run on the Pi: `cd ~/slow_bus/tools/commission/lua && luajit pico.lua`.

### `ident=-1` boot banner now observable over USB (fixed 2026-06-16)
The `[boot] … ident=-1` line is emitted once at startup into the host-link TX
ring *before any host can attach* (SDK drops disconnected CDC output). Fixed:
`uplink_task` now re-emits the banner on the `conn` false→true edge via
`bc_emit_boot_banner()` (shared with the boot path; `g_id_rc` holds the identity
result). HW-verified — `pico.lua listen` shows `[boot] bus_controller boot#2
rst=POWER ident=-1` on every fresh open. Remote reboot also works:
`sudo picotool reboot -f -u` drops it to BOOTSEL (the fw exposes the reset iface).

## Build reminders (legacy host self-test still valid)
```sh
make -C tools test    # host codec self-test (WSL ok)
```
Flash = BOOTSEL + `.uf2` (drag-drop or `picotool load`).
