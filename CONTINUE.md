# CONTINUE — slow_bus pick-up doc

Read first on any session resume. Companion to `README.md` (orientation) and
`docs/README.md` (full spec). Last updated **2026-06-15 (end of day)**.

---

## TL;DR — where we are

The bus + 5 SAMD21 modes + commission toolchain are **done and HW-verified**
(see git history + memory). Today began a **major restructure** and the first
firmware in the new model. Branch: **`samd21-namespace-db`** (work committed at
EOD today).

**Tomorrow's first action:** start integration on the Pi. Step 1 firmware is
already built there (`~/slow_bus/build/bus_controller.uf2`, builds clean). Flash
it when a Pico is free (today the USB bus was occupied by xiao_blocks SAMD21s),
confirm the boot log reads `ident=-1` and the bench API is unchanged, then start
**Step 2** (config-FS region + real `cfg_load`).

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
      `rst=POWER` (guard passed). **NEXT: 2b** = Lua image builder (`cfg_image.lua`)
      → emit a 64 KB region with a real `idnt` entry, flash at the region base,
      confirm `cfg_load` finds it. Then 2c = wire real `idnt` values + self-test.
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
