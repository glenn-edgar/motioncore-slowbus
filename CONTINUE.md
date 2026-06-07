# CONTINUE — slow_bus pick-up doc

Read first on any session resume. Companion to `README.md` (orientation) and
`docs/README.md` (full spec: architecture, wire protocol, pin map).

## What this is

The **115200-baud 9-bit RS-485 multidrop bus**, bus controller on a **Pico W
(RP2040)**. Lead reference repo — `fast_bus` (Pico 2 W, 400k) copies it later.
Seeded from `fast_bus` on 2026-06-06 (shared host-tested `core/` + SAMD21 ref).

## State at last checkpoint (2026-06-06)

- Repo scaffolded: `core/` (host-tested, codec self-test green), `port/rp2040/`
  adapted from the RP2350 port (board.h pin map + 115200 + no-DE, PHY/uplink
  skeletons), `CMakeLists.txt` retargeted to `pico_w` / `rp2040` / RP2040
  FreeRTOS port with three images: `bus_controller` (UPLINK=usb),
  `bus_controller_wifi` (UPLINK=wifi), `slave`.
- Docs written: top-level `README.md`, full spec `docs/README.md`.
- **Not done yet:** the PIO PHY is still a skeleton (no real SM programs); the
  app/engine layer is untouched; firmware not yet built on the Pi.

## Next action

**Step 1 of the bring-up order (gating task): make `port/rp2040/phy_pio_rs485.c`
a real 9-bit half-duplex PIO PHY + a DI→RO loopback self-test.** Prove the wire
in isolation before any protocol. Then 2-node POLL vs a SAMD21 slave.

## Bring-up order (don't reorder — isolate variables, the RA4M1 lesson)

1. PIO 9-bit PHY + loopback self-test (DI→RO). **Gating.**
2. 2-node POLL / NO_MESSAGE against a SAMD21 slave.
3. USB-CDC uplink (port the motioncore libcomm host frame).
4. App core as virtual slave (inter-core queue) + supervisory `OP_EVENT` push.
5. Peer (slave→slave) between two Pico slaves.
6. WiFi-proxy uplink behind the same seam.
7. Reliable DATA ack tracking + bus-progress WDT.

## Build reminders

```sh
make -C tools test                                   # host codec self-test (WSL ok)
# firmware (Pi dev host, ssh robot):
export PICO_SDK_PATH=/home/pi/pico/pico-sdk
export FREERTOS_KERNEL_PATH=/home/pi/pico/FreeRTOS-Kernel
~/.local/bin/cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DPICO_BOARD=pico_w .
~/.local/bin/cmake --build build -j4
```

System cmake (3.18.4) is too old → `~/.local/bin/cmake` (4.3.2) + the policy
shim. Board `pico_w`, platform `rp2040`, FreeRTOS SMP **RP2040** port (not the
RP2350 NTZ port). Flash = BOOTSEL drive + copy `.uf2`.

## Open questions (tracked in docs/README.md)

- App engine = chain_tree-in-C; can be shrunk like the s_engine M-port. Study
  `fleet_design/vendor/lua/ct_*.lua` first. Downstream of the PHY — not urgent.
- Peer routing config: how a Pico slave learns its peer target(s). Lean
  BC-pushed.
