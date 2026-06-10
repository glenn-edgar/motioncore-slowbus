# SAMD21 config filesystem — design sketch

Status: **DRAFT for red-pen.** No code yet.

## Architecture
A **unit = Pico/Pico2 (RS-485 node + compute) + a SAMD21 (its identity/config
store + HIL)**. The SAMD21's flash is the unit's persistent config. **Same
firmware for every unit of a class** → `class_id` is **compile-time** (one image
per class); everything per-unit lives in the SAMD21 FS.

The SAMD21 flash holds (named CBOR files):
- **the Pico's RS-485 address** (the Pico reads this — it's the bus node),
- the **SAMD21's own I2C address** (stored, but defaults to the class well-known value),
- `instance_id`, **site config**, other hardware-specific data.

The **2 GPIO are input straps**, *not* stored config: the SAMD21 **samples** them at
boot (a hardware-set 2-bit site/slot id) and exposes the value **read-only**. Straps
+ the stored `site` file together define the deployment site. (One SAMD21 per Pico.)

**Boot order (load-bearing):**
```
SAMD21 powers up → reads its own FS → sets its I2C slave address
Pico powers up → reads SAMD21 FS over I2C → gets RS-485 addr + site/GPIO config
              → THEN does RS-485 bus negotiation
```

## Goal
Open-ended, **named** config blobs that:
- the **Pi (Python)** writes to the SAMD21 over **USB-CDC (ttyACM)** at commission time,
- the **Pico/Pico2** reads back **by name over I2C** at boot, *before* bus negotiation,
- the **SAMD21** itself reads (its own I2C address) at boot,
- survive power loss (commissioning must never brick the part).

Payload on flash is **CBOR**; the SAMD21 stores/serves **opaque bytes** — it never
parses CBOR/JSON.

## Decisions (locked)
| Parameter | Value | Consequence |
|---|---|---|
| Max name | **4 chars** | a name is exactly 4 bytes = a `u32` → fixed-size, no variable name buffer |
| Max file | **256 B** | one NVM row; with a row header, payload caps at **244 B** (or use a 512 B/2-row slot for a true 256 B) |
| FS budget | **64 KB** | ~256 rows; with 1-row slots, up to ~250 files after metadata |
| `instance_id` | **FS file** | field-set at commission (was a commission record) |
| `rs485_addr` | **FS file** | field-set, now **independent** of instance (was `instance_id` low byte) |
| `class_id` | **chip-bound, NOT a file** | identifies the hardware type; lives with the chip/firmware → see Identity |

## Backend — generalize the existing store (NOT LittleFS)
The numbers above are exactly what the existing **log-structured 256 B-row store**
in `samd21_commands.c` already does (round-robin wear-leveled rows, latest-seq
wins, CRC-8 per entry, power-safe, flash writes deferred to the main loop).
**Generalize it** from 6 fixed `rec_id`s to **N name-keyed slots**:
- key each entry by the **4-byte name** instead of a 1-byte `rec_id`,
- grow the partition **8 KB → 64 KB** (256 rows),
- entry = `{magic, seq, name[4], len, crc, data[≤244]}` in one 256 B row
  (or a 2-row slot if a full 256 B payload is ever needed).

Reuses proven, tested, power-safe code; **no new dependency, no bd-shim**.
*LittleFS is the fallback only if files ever exceed 256 B / need spanning / the
count blows past the slot model.*

## Identity model — three lifetimes
```
class_id    = WHAT the hardware is   → COMPILE-TIME (-DCLASS_ID); same image per class
instance_id = WHICH one of that type → FS file, field-set at commission
rs485_addr  = the PICO's bus address → FS file, field-set (independent of instance)
```
`class_id` is a **compile-time constant per class** — every unit of a class runs
the identical firmware, so the Makefile/CI bakes `CLASS_ID` per class build. The
per-unit identity (`instance`, the Pico's `addr`, the SAMD21's own `i2c` address,
GPIO, site) all live in the FS.

Change from today: `register_dongle_rs485_addr()` currently returns the low byte
of the commissioned `instance_id`. Under this model the address is **its own FS
file**, decoupled from instance, and it is the **Pico's** address (the SAMD21 just
stores it; the Pico consumes it to configure its RS-485 node).

## Bootstrapping & failure modes (new — boot order is load-bearing)
1. **I2C address — fixed well-known per class (resolved).** One SAMD21 per Pico, so
   the Pico talks to it at the **class well-known I2C address** (compile-time
   default) — **no scan**. The address is still stored in flash for completeness/
   override, but the Pico never has to discover it. (The SAMD21 reads its own FS at
   boot before serving I2C — it owns the flash, no race.)
2. **Blank / uncommissioned FS** (fresh unit, no `addr` file) → the Pico gets no
   RS-485 address → it must **stay off the bus** (no negotiation) and signal
   "uncommissioned", never default to an address that could collide.
3. **SAMD21 absent / FS corrupt** → the Pico read needs a **timeout + safe path**
   (don't hang at boot; degrade to off-bus). Note this makes the SAMD21 a **single
   point of identity** for the unit — acceptable, but design the Pico boot to fail
   safe, not hang.
4. **SAMD21 must be I2C-ready fast** so it isn't the long pole in unit boot.

## Proposed flash map (SAMD21G18A, 256 KB)
| Region | Range | Notes |
|---|---|---|
| Bootloader | `0x00000–0x02000` | UF2, untouched |
| App | `0x02000–0x14000` (72 KB) | currently ~48 KB; margin for growth |
| **Named-slot store** | `0x14000–0x24000` (64 KB) | the config FS (256 rows × 256 B) |
| (free) | `0x24000–0x3FE00` | spare / future |
| `class_id` chip record | top row(s) | only if option (b) above |
Needs a linker carve-out so `.text` can't land in the store partition. The old
8 KB 6-record store + commission A/B slots are **subsumed** by this store (instance
moves in; class goes chip-bound).

## Payload — JSON source, canonical CBOR on device
```
config/*.json --tool: cbor2.dumps(obj, canonical=True)--> <name> on flash --I2C--> Pico cfl_cbor_* decode
```
Canonical CBOR (RFC 8949 §4.2) → byte-stable for the per-entry CRC. String keys.
SAMD21 stores opaque bytes. Likely files (4-char names): `inst`, `addr` (or one
`node` file `{instance, addr}`), plus hw-specific `hw0`/`cal`/`pin`…

## I2C FILE register bank (Pico read-by-name)
Name is a fixed **4-byte** field — no variable buffer. Window `0x50–0x57`,
data-port pattern (auto-advancing, does not bump the register pointer):
| Reg | R/W | Meaning |
|---|---|---|
| `0x50 FILE_NAME` | W | the 4 name bytes (write all 4, then OPEN) |
| `0x51 FILE_CTRL` | W | `0x01 OPEN`, `0x02 CLOSE`, `0x03 LIST_BEGIN`, `0x04 LIST_NEXT` |
| `0x52 FILE_STAT` | R | `0 OK · 1 NOT_FOUND · 2 BUSY · 3 ERR` |
| `0x53 FILE_SIZE` | R | u16 — size of the open file (≤256) |
| `0x55 FILE_SEEK` | W | u16 read cursor |
| `0x56 FILE_DATA` | R | data port: streams ≤256 B from the cursor |
**Pico flow:** write 4 name bytes; `OPEN`; poll `STAT`; read `SIZE`; burst-read
`DATA`; `CLOSE`. OPEN copies the ≤256 B file into a RAM buffer → fast, minimal
clock-stretch. **Reads only on the I2C path — no flash writes.** `LIST_*` walks
the directory (read each name back via `FILE_NAME`).

## USB-CDC file-write commands (Pi commissioning)
Extends the existing framed protocol (the `OP_COMMISSION_*` path):
`OP_FILE_BEGIN [name(4)]` · `OP_FILE_DATA [chunk]` · `OP_FILE_COMMIT [crc32]` ·
`OP_FILE_DELETE [name(4)]` · `OP_FILE_LIST`. Writes commit atomically (latest-seq
wins) and stay on the **USB/main-loop path only** — never during an I2C/RS-485
transaction (SAMD21 stalls the CPU on NVM write/erase; no RWW).

## Commission tool (Python) — WSL needs a venv first
`pyserial` + `cbor2`. WSL/modern Debian system Python is externally-managed
(PEP 668) — `pip install` is refused. Use a venv; never `--break-system-packages`:
```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r tools/commission/requirements.txt
```
Live commissioning runs on the **Pi** (dongle enumerates there); from a WSL dev
box, pass the device through with `usbipd-win` (see docs/toolchain-wsl.md).

## Open decisions
1. ~~`class_id` home~~ → **compile-time `-DCLASS_ID` per class** (resolved).
2. ~~I2C address~~ → **fixed well-known per class, one SAMD21 per Pico, no scan** (resolved).
3. ~~2 GPIO~~ → **input straps, sampled at boot, exposed read-only** (resolved; not stored).
4. **File granularity:** one file per item or grouped CBOR maps? (4-char names; e.g.
   `net`={addr,inst}, `site`={…}.) — lean grouped.
5. **Payload cap 244 B (1-row slots) or true 256 B (2-row slots)?** (All known items
   are ≤ tens of B → 244 B is ample; lean 1-row.)
6. Retire the old 6-record store + commission A/B slots once these migrate in.

## Phasing
1. Generalize the store: name-keyed slots, 64 KB partition, linker carve-out.
2. USB `OP_FILE_*` (Pi can store files).
3. I2C FILE register bank (Pico reads by name).
4. Python commission tool (venv, JSON→canonical CBOR).
5. Firmware identity init: `class_id` from chip, `inst`/`addr` from FS.
6. Pico-side: read named CBOR config at boot.
