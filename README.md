# slow_bus

A **9-bit RS-485 multidrop bus at a fixed 115200 baud**, with the bus controller
on a **Raspberry Pi Pico W (RP2040)**. Single-master polled; peer-capable Pico
slaves can also message each other directly (slave-to-slave) inside their granted
window. Existing **SAMD21 slaves run unchanged** as master-only nodes.

> **Full design + pin map + wire protocol live in [`docs/README.md`](docs/README.md).**
> This file is the short orientation; that one is the spec.

## What it is

- **BC = Pico W**, dual-core: `core0` = bus/message-control + uplink, `core1` =
  application + API + supervisory interlocks. The BC polls its own app core like
  another slave (a virtual slave on an inter-core queue).
- **Uplink, build-time, one of two** (same libcomm frame stream, zenoh stays on
  Linux): `UPLINK=usb` (USB-CDC bridge — built first) or `UPLINK=wifi` (WiFi to a
  Linux proxy process).
- **Heterogeneous slaves**: Pico slaves are peer-capable; SAMD21 slaves are
  master-only and filter peer traffic for free (no firmware change).
- **Auto-direction transceivers** → no DE pin; `0xFF` preamble covers turnaround.

## Relationship to fast_bus

`slow_bus` is the **lead reference**, built **first** on easy mode (115k, no
motor/DSP). The faster sibling `fast_bus` (Pico 2 W / RP2350, 400 kbaud, peer
fabric + motor control + DSP) is built afterward and **copies the proven parts** —
PIO PHY, uplink seam, poll engine, FreeRTOS layout — then adds the hard parts on
top. The two are **separate variants, not one parameterized tree**, because they
differ in more than speed. This repo was seeded from `fast_bus` (shared
host-tested `core/` + `reference/samd21/`).

## Layout

```
core/        chip-independent C — builds on RP2040, RP2350, or a host (host-tested)
  bus_addr.h    address space + 9-bit framing + frame-type constants
  bus_crc8.*    CRC-8/AUTOSAR
  bus_frame.*   BC-2 frame codec + reentrant RX assembler   (host-tested)
  bus_roster.*  RAM-only node roster held by the controller
  bus_sched.*   controller arbiter — grant cycle + peer windows   (skeleton)
  bus_node.*    node side — in-window reply + peer send            (skeleton)
  bus_phy.h     southbound 9-bit RS-485 PHY seam
  bus_uplink.h  northbound host-link seam
port/rp2040/ Pico W port: PIO PHY, board glue + pin map, uplinks   (skeleton)
  board.h        RS-485 + full HIL pin map (single tree = BC and slave)
  phy_pio_rs485.c  9-bit PIO PHY — GATING bring-up task (DI->RO loopback first)
  uplink_usb_cdc.c / uplink_wifi_proxy.c / uplink_none.c
app/
  bus_controller/  ROLE=bus_controller — core0 message-control / core1 app
  slave/           ROLE=slave           — node window logic
reference/samd21/  the SAMD21 bus controller — labeled starting point
tools/        host build of the codec self-test (no SDK)
docs/         README.md (full spec) + protocol.md (wire detail)
```

## Build

**Host self-test** (no SDK — proves the core is portable):

```sh
make -C tools test     # -> "frame_selftest: all checks passed"
```

**Firmware** (on the Pi dev host; system cmake is too old → use the pip one):

```sh
export PICO_SDK_PATH=/home/pi/pico/pico-sdk
export FREERTOS_KERNEL_PATH=/home/pi/pico/FreeRTOS-Kernel
~/.local/bin/cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DPICO_BOARD=pico_w .
~/.local/bin/cmake --build build -j4
# -> bus_controller.uf2 (UPLINK=usb), bus_controller_wifi.uf2, slave.uf2
```

Flash = mount the BOOTSEL drive, copy the `.uf2`.

## Bring-up order (don't reorder)

1. PIO 9-bit PHY + loopback self-test (DI→RO) — **gating task**.
2. 2-node POLL / NO_MESSAGE against a SAMD21 slave.
3. USB-CDC uplink — host registers the roster, round-trips a command.
4. App core as virtual slave + supervisory `OP_EVENT` push.
5. Peer (slave→slave) between two Pico slaves.
6. WiFi-proxy uplink behind the same seam.
7. Reliable DATA ack tracking + bus-progress WDT.
