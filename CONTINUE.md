# CONTINUE — slow_bus pick-up doc

Read first on any session resume. Companion to `README.md` (orientation) and
`docs/README.md` (full spec). Last updated **2026-06-22**.

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

**NEXT**: (a) **Pico2/RP2350 build of the same source** (the "pico AND pico2" half of the
goal — only rp2040 built/verified so far). (b) more node app logic beyond CMD_ECHO.
(c) `g_roster`(cap 16) vs `core/bus_roster.c`(cap 32) reconcile. (d) remove the SAMD21 tree
(→ `~/xiao_blocks`) once it's a few days settled. (e) later: the Pi wireless proxy.

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
