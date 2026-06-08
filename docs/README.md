# slow_bus — 9-bit RS-485 multidrop bus (Pico W bus controller)

A **115200-baud, 9-bit RS-485 multidrop bus** with the bus controller on a
**Raspberry Pi Pico W (RP2040)**. The bus is single-master polled, but slaves
that are capable can message each other directly (peer / "slave-to-slave") inside
their granted window. Existing **SAMD21 slaves run unchanged** as master-only
nodes.

This repo is the **lead reference**. The faster sibling (`fast_bus`, Pico 2 W /
RP2350, 400 kbaud, motor + DSP) is built *after* this one and **copies the proven
parts** — PIO PHY, uplink seam, poll engine, FreeRTOS layout. Prove the wire and
the engine here on easy mode (115k, no motor ISR), then carry the lessons up to
the RP2350.

---

## Why this shape

- **115200, fixed forever.** The slow bus baud never changes — the SAMD21 slaves
  are 115200 and they stay the same, so the BC clocks to them.
- **9-bit (MPCM) framing.** The 9th UART bit marks the address byte, so every
  node's RX cheaply resyncs and address-filters. 128-node space.
- **PIO PHY, not the hardware UART.** The RP2040 PL011 is 8-bit only and has no
  9-bit/MPCM mode, so it cannot carry the address-marker bit. A PIO state machine
  clocks true 9-bit symbols. (115k is trivial for PIO — ~1085 cycles/bit at
  125 MHz.)
- **Auto-direction transceivers.** The on-hand MAX485-class boards self-direct on
  TX, so **no DE pin** is needed. The sacrificial `0xFF` preamble byte covers the
  RC turnaround/settle window; the PHY still discards self-echo with a skip
  counter (half-duplex — a node hears its own TX).
- **Single-master polled, no arbitration.** No token, no CAN, no multi-master
  problem. The BC grants windows; one transmitter per window.

---

## Topology

```
        Linux host (zenoh / proxy)
                 |
        USB-CDC  OR  WiFi-to-proxy        <- uplink, build-time pick ONE
                 |
        +------------------+
        |   Pico W  (BC)   |   RP2040, dual core
        |  core0: bus/msg  |   poll engine + uplink
        |  core1: app/API  |   supervisory interlocks, OP_EVENTs
        +------------------+
                 |  9-bit RS-485, 115200
   --------------+-----------------+----------------
        |                |                  |
   Pico slave        Pico slave        SAMD21 slave
   (peer-capable)    (peer-capable)    (master-only)
        \________peer________/         (filters peer traffic)
```

- **BC = Pico W**, dual-core. `core0` runs the message-control / poll engine and
  the uplink; `core1` runs the application + API + supervisory interlocks.
- **The BC polls its own app core like another slave.** `core1` is a *virtual
  slave on the inter-core FreeRTOS queue*; `core0`'s poll engine polls it with the
  same window mechanism it uses for wire slaves. One uniform interface: N real
  slaves on the PIO wire + 1 virtual slave on the queue. This is why the BC needs
  the full HIL peripheral set — its app core *is* a slave with real peripherals.
- **Pico slaves** are peer-capable. **SAMD21 slaves** are master-only and need
  **no firmware change** to coexist (see peer messaging below).

---

## Uplink — one of two, build-time selected (`UPLINK=usb|wifi`)

The chip speaks the **same libcomm frame stream** in both modes; only the
transport underneath differs. **zenoh always stays on Linux** — never on the MCU.

- **`UPLINK=usb`** — USB-CDC frame bridge to a Linux host. The proven SAMD21 BC
  model: Linux runs the zenoh side, the chip is a bridge + poll engine. Lower
  risk → **build this first.**
- **`UPLINK=wifi`** — WiFi (CYW43) to a **Linux proxy process** that does what the
  USB host did ("mangles the dongle"). Just a transport swap: same frames over a
  TCP socket instead of USB. No zenoh-pico on the chip.

Both terminate at one Linux codepath (serial port *or* socket → same handler).
USB-CDC stays available in *both* builds for flashing and the bring-up console;
it's just not the *uplink* in WiFi mode.

> Seam: `bus_uplink.h` with `uplink_usb_cdc.c` and `uplink_wifi_proxy.c`.

---

## Wire protocol (BC-2 frame)

After a sacrificial `0xFF` preamble:

```
[dest|bit8]  src  type  seq  len  payload[len]  crc8
```

- `dest` carries the **9th address-marker bit** (bit8) — marks "new message,
  here's the destination". `crc8` = CRC-8/AUTOSAR over `dest..payload`.
- `HEADER_LEN = 5`, payload ≤ 120 bytes.

| type | name | direction | notes |
|------|------|-----------|-------|
| 0x00 | NO_MESSAGE | slave→master | end-of-window terminator (UDP) |
| 0x01 | POLL | master→slave | "your window is open" (UDP) |
| 0x02 | DATA | any | payload = `[opcode:u16-LE][body]` |
| 0x03 | ACK | receiver | prior TCP DATA seq CRC-good |
| 0x04 | NAK | receiver | prior TCP DATA seq CRC-bad |
| +0x10 | TCP flag | — | reliable (ack-tracked); else UDP best-effort |

Addresses: `0x00` = master / broadcast, `0xFF` = sniffer (accept all).

---

## Slave-to-slave (peer) messaging — fully transparent

Peer messaging needs **zero roster capability tracking**. It's the same
tacked-on-tail mechanism as a normal slave→master reply, just with
`dest = peer_addr` instead of `dest = master(0x00)`, sent inside the sender's
**BC-granted poll window** (so single-transmitter-per-window still holds → no
collision).

Every node applies the **same address-match rule**:

- **BC** sees the frame (it hears everything), `dest != 0x00` → **throws it out**.
- **SAMD21 slave** sees it, `dest != my_addr` → throws it out (free; existing
  address filter, no firmware change).
- **Target Pico slave** sees it, `dest == my_addr` → **keeps it**.

No node tracks anyone else's capabilities. The BC grants windows uniformly; what
a slave does in its window (reply to master, send to a peer, or both) is the
slave's own business. The only BC concern is **cadence** — poll Pico slaves often
enough that peer latency is acceptable.

> Open: how a Pico slave learns its peer target address(es). Lean **BC-pushed**
> (the BC owns topology) rather than per-slave commissioning.

---

## Interlocks

Two distinct kinds — keep them separate:

- **Hard-safety interlocks live LOCAL on the slave.** Deterministic per-tick,
  local inputs → local outputs, no bus in the loop. This is the existing SAMD21
  interlock role-mode framework, unchanged.
- **BC interlocks are SUPERVISORY.** Generated by the **app core** on polled /
  own data, output is an **`OP_EVENT` pushed north to the host** (USB or
  wifi-proxy) — *not* a drive command back onto a remote slave. Because the output
  is an event, the fact that inputs are only as fresh as the last poll is fine.

> A BC interlock that reacts to a *remote* slave's input is bounded by poll
> round-trip time regardless of how fast the Pico is — two cores don't shrink bus
> latency. That's why hard-safety stays local; the BC does coordination /
> reporting. Reuse the SAMD21 `OP_EVENT` opcode + format so the host decoder is
> identical whether the event came from a slave or the BC.

---

## Roles (single tree, build-time `ROLE=bc|slave`)

| | BC | Slave |
|---|---|---|
| WiFi | on (only if `UPLINK=wifi`) | **off** |
| USB-CDC | uplink and/or console | console / flash only |
| Bus | master (poll engine) | node (window reply + peer send) |
| App core | core1 API + supervisory interlocks | HIL command surface |

Mirror the SAMD21 `ROLE` pattern (clean build required on role switch).

---

## Pin map — Pico W (RP2040), "pico1"

The "pico1" role is a **GPIO interface + I2C manager**. The reduced HIL set: **no
SPI**, **1 I2C**, **1 HIL UART**, **1 PWM**, **1 quadrature decoder**, plus an
8-pin contiguous GPIO block and the 3-channel ADC analog spine. Two boot straps
(GP0/GP1) select bus speed and BC master/slave. Header exposes **GP0–GP22 +
GP26/27/28** (26 usable); GP23/24/25/29 are CYW43-internal.

> The RP2040 pin mux ties **I2C0-SDA / UART1-TX to the same pins (GP4/GP8/GP20)**
> and I2C0-SCL / UART1-RX to GP5/GP9/GP21. A contiguous GP2–GP9 GPIO block is
> only possible because we settled on **1 I2C + 1 UART** — placed on i2c1
> (GP10/11) and uart0 (GP12/13), clear of the GPIO block.

### Function summary

| Function | Pins (GPIO) | Notes |
|----------|-------------|-------|
| Strap: bus speed | GP0 (pin 1) | read once at boot; worst-case jumper |
| Strap: BC master/slave | GP1 (pin 2) | read once at boot; worst-case jumper |
| GPIO (8) | GP2–GP9 (pins 4–12) | contiguous block |
| I2C | GP10 SDA, GP11 SCL (i2c1) | single bus — the "I2C manager" |
| UART | GP12 TX, GP13 RX (uart0) | single HIL serial; bus is separate (PIO) |
| PWM | GP14 (slice 7A) | 20 kHz, armed at boot @ 0 % |
| RS-485 bus | GP15 DI/TX, GP16 RO/RX (PIO) | adjacent pair; no DE (auto-direction) |
| QUAD | GP17 A, GP18 B (PIO) | RP2040 has no HW QEI |
| ADC | GP26 ADC0, GP27 ADC1, GP28 ADC2 | analog inputs, kept free |
| Spare GPIO | GP19, GP20, GP21, GP22 | expansion |

### Physical layout (USB at top, component side up)

**Left side (pins 1–20)**

| Pin | GPIO | Function | | Pin | GPIO | Function |
|----|------|----------|--|----|------|----------|
| 1 | GP0 | **STRAP: bus speed** | | 11 | GP8 | GPIO6 |
| 2 | GP1 | **STRAP: master/slave** | | 12 | GP9 | GPIO7 |
| 3 | — | GND | | 13 | — | GND |
| 4 | GP2 | GPIO0 | | 14 | GP10 | I2C SDA |
| 5 | GP3 | GPIO1 | | 15 | GP11 | I2C SCL |
| 6 | GP4 | GPIO2 | | 16 | GP12 | UART TX |
| 7 | GP5 | GPIO3 | | 17 | GP13 | UART RX |
| 8 | — | GND | | 18 | — | GND |
| 9 | GP6 | GPIO4 | | 19 | GP14 | PWM (slice 7A) |
| 10 | GP7 | GPIO5 | | 20 | GP15 | **RS-485 TX (DI)** |

**Right side (pins 21–40)**

| Pin | GPIO | Function | | Pin | GPIO | Function |
|----|------|----------|--|----|------|----------|
| 21 | GP16 | **RS-485 RX (RO)** | | 31 | GP26 | ADC0 |
| 22 | GP17 | QUAD A | | 32 | GP27 | ADC1 |
| 23 | — | GND | | 33 | — | AGND |
| 24 | GP18 | QUAD B | | 34 | GP28 | ADC2 |
| 25 | GP19 | spare GPIO | | 35 | — | ADC_VREF |
| 26 | GP20 | spare GPIO | | 36 | — | 3V3 (OUT) |
| 27 | GP21 | spare GPIO | | 37 | — | 3V3_EN |
| 28 | — | GND | | 38 | — | GND |
| 29 | GP22 | spare GPIO | | 39 | — | VSYS |
| 30 | — | RUN | | 40 | — | VBUS |

Why it lays out well: **straps at the top-left (pins 1–2)** are the two jumper
inputs; the **8-pin GPIO block is contiguous (GP2–GP9 = pins 4–12)**; I2C + UART
sit just below it (GP10–GP13); **RS-485 is an adjacent pair on GP15/GP16 (pins
20/21)** for the transceiver pigtail, with QUAD right after it; ADC sits beside
AGND + ADC_VREF in the bottom-right analog corner; spare GPIO fills the gap.

---

## PWM

- The PWM runs at **20 kHz** (GP14, slice 7A). Armed at boot, **compare = 0 → 0 %
  duty (pin held low)** until commanded — known-safe state.
- **11-bit resolution (2048 steps, TOP = 2047)** to match the SAMD21/RA4M1 HIL
  command surface (duty 0–2047), so the host-side command is portable across all
  chips. At 125 MHz sys_clk a 2048-count period needs clock ≈ 40.96 MHz → PWM
  divider ≈ 3.05 (8.4 format → 3 + 1/16 ≈ 3.0625, giving ~19.93 kHz). This is not
  a motor controller, so 11 bits is plenty.

---

## RP2040 vs SAMD21 — peripheral differences to remember

1. **No DAC.** RP2040 has no analog output (SAMD21 had one). Analog out = PWM + RC
   filter if ever needed. "Analog pins free" = ADC inputs only.
2. **ADC is one SAR, round-robin muxed** — 3 channels, **not simultaneous**
   (12-bit, ~500 ksps shared).
3. **Only 3 ADC channels** on the header — GP29/ADC3 is VSYS-sense via CYW43.
4. **Quadrature is PIO** (no HW encoder) — costs 2 of the 8 PIO state machines.

**PIO SM budget:** RS-485 TX+RX (2) + QUAD (1) = 3, plus CYW43 (1, only in
`UPLINK=wifi`) = 4 of 8. **4 spare.**

---

## App engine — open

The BC app core (`core1`) runs the API + supervisory interlocks. Engine choice is
**chain_tree-in-C** (KB / column / state-machine / watchdog / return-codes),
*not* s_engine, *not* LuaJIT. Study `fleet_design/vendor/lua/ct_*.lua` before
writing the C version. Whatever is chosen here becomes the fast_bus app engine too
(fast_bus copies this). Not urgent — downstream of getting the PHY + poll engine
proven.

---

## Build

Host = the **Pi dev host** (`ssh robot`), same toolchain as
`motioncore/firmware/pico`. **System cmake (3.18.4) is too old** for the Pico SDK
→ use `~/.local/bin/cmake` (4.3.2) and pass `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.

```sh
export PICO_SDK_PATH=/home/pi/pico/pico-sdk
export FREERTOS_KERNEL_PATH=/home/pi/pico/FreeRTOS-Kernel
~/.local/bin/cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DPICO_BOARD=pico_w .
~/.local/bin/cmake --build build -j4
```

- Board `pico_w`, platform `rp2040`, FreeRTOS **SMP RP2040 port** (not the
  RP2350 NTZ port).
- Flash = mount the BOOTSEL drive, copy the `.uf2`.

---

## Bring-up order (don't reorder — isolate variables)

1. **PIO 9-bit PHY + loopback self-test** (DI→RO, marker bit reassembles). Gating
   task — prove the wire before any protocol.
2. **2-node POLL / NO_MESSAGE** against a SAMD21 slave.
3. **USB-CDC uplink** — Linux host registers the roster and round-trips a command.
   This completes the USB dongle.
4. **App core as virtual slave** (inter-core queue) + supervisory `OP_EVENT` push.
5. **Peer (slave→slave)** between two Pico slaves.
6. **WiFi-proxy uplink** behind the same seam.
7. **Reliable DATA** ack tracking + bus-progress WDT.

---

## Relationship to fast_bus

| | slow_bus (this) | fast_bus |
|---|---|---|
| Chip | Pico W (RP2040) | Pico 2 W (RP2350) |
| Baud | 115200 (fixed) | 400000 |
| Built | **first (lead reference)** | second (copies this) |
| Extras | full-libcomm bridge, peer | peer fabric, motor control, DSP |

The buses differ in more than speed, so they're **two variants, not one
parameterized tree.** fast_bus copies the proven PHY + uplink seam + poll engine
once this is green, then adds 400k timing, peer fabric, and the motor/DSP load on
top.
