# I/O Belongs on Microcontrollers, Reached as Services: A Position on Edge-Linux I/O Architecture

*Position paper — draft. Author: Glenn Edgar. Status: working draft for review.*
*Companion to "Why Zenoh, and Where" — same stack, one layer down.*

## Abstract / Thesis

The reflexive way to do machine I/O on a Linux single-board computer is to use its
expansion header. That is the wrong tool twice over: the header is physically hard and
fragile to interface, and a Linux application processor is the most power-hungry, least
deterministic device on the board for the job of toggling pins on a schedule. The industry
already knows the first half of the fix — **offload I/O to a microcontroller** — and ships
it (the Arduino Uno Q pairs a Qualcomm Linux SoC with an STM32 Cortex-M33). We agree.

Our position is the second half: doing it *one MCU bolted to one SBC* is not enough. The
real win is to make I/O a **network service**. We propose a systematic, container-based
architecture in which **USB and WiFi proxy containers** on the edge Linux box expose a bus
of remote I/O microcontrollers as services — consumable identically by local processes or by
an entirely separate Linux system. The I/O nodes hang off a **deterministic single-master
serial bus**, so real-time timing runs on processors built for it, at low energy, while the
Linux box does compute and exposes the I/O over the two ports every edge processor already
has. The differentiator is not the offload; it is that the I/O becomes a relocatable,
multi-consumer service rather than a pin captive to one board.

---

## 1. The expansion-header trap

Drive a machine from a Raspberry Pi and the first instinct is the 40-pin header — GPIO, SPI,
I²C, UART, a PWM line or two, right on the board. In practice it fails on two independent
axes.

**It is physically hard and fragile.** The header is bare 3.3 V, current-limited, and
unprotected. Anything real — a sensor down a cable, a solenoid, a motor, a 24 V field
device — needs level shifting, isolation, drivers, and over-voltage/ESD protection the header
does not provide. The result is a tower of HATs, ribbon cables, and hand-wired protection
boards: hard to service and a long way from a sealed industrial connector.

**It is expensive in cycles and energy, and still not deterministic.** A Cortex-A class
application processor is built to run an OS, a network stack, and compute — not to meet a
pin deadline. Software PWM, GPIO bit-banging, quadrature decode, and tight ADC sampling all
fight the scheduler for timing they will never reliably get, burn CPU, take interrupt-latency
hits, and jitter under load. You spend the highest-power processor on the board doing the one
job it is worst at. **These are the wrong processors to waste cycles and energy on low-level
I/O** — and even after spending them, the timing is soft.

## 2. The right instinct, half-applied

The fix is understood, and the market is converging on it. The clearest recent example is the
**Arduino Uno Q**: a Qualcomm **Dragonwing QRB2210** (quad-core Cortex-A53 @ 2.0 GHz) Linux
SoC paired *on one board* with an **STM32U585 Cortex-M33** (160 MHz, running Arduino Core on
the Zephyr RTOS), the two joined by Arduino's **"Bridge"** — an RPC layer (MessagePack RPC;
the physical interconnect is not publicly documented). Linux does compute and connectivity;
the **M33 owns the real-time I/O** — ADC, PWM, CAN, timers, the 3.3 V headers —
deterministically and at a fraction of the energy. This is exactly right: put I/O on a
processor designed for it. **We agree with the approach.**

But a single MCU soldered next to the SoC solves only the timing-and-energy problem. It
leaves the I/O **physically captive** to that board, in a fixed 1:1 pairing, reachable only
by code running on that one Linux instance. Real machines spread I/O across a chassis, add to
it incrementally, and have it consumed by more than one process — sometimes more than one
computer. That calls for a **system-match approach**: not a co-processor, but an I/O *fabric*
the Linux side reaches as a service.

## 3. The leverage already on every edge box

The systematic version needs no exotic hardware, because every edge Linux processor already
ships with the two ports that matter: a **USB 2.0 host port** and **WiFi**. Those are the two
transports to the I/O layer — one wired and local, one wireless and remote. The expansion
header is not used at all. The Linux board keeps doing what it is good at — compute,
networking, containers — and reaches its I/O over a standard port like everything else.

## 4. The proposal: container proxies, MCU nodes as services

We propose a container-based architecture with a clean seam between Linux and I/O:

```
   local processes        a DIFFERENT linux box
        │                        │
        └──────── services ──────┘
                     │
   ┌─────────────────┴───────────────────┐
   │  Edge Linux box (containers)         │
   │   ┌───────────┐     ┌────────────┐   │   one proxy container per transport;
   │   │ usb proxy │     │ wifi proxy │   │   exposes the bus as services
   │   └─────┬─────┘     └─────┬──────┘   │
   └─────────┼─────────────────┼──────────┘
        USB-CDC             WiFi/UDP
             │                  │
        ┌────┴──────────────────┴─────────┐
        │  bus-controller MCU  (uplink + poll engine, local app + interlocks)
        └───┬──────────┬──────────┬────────┘
       deterministic 9-bit RS-485 single-master serial bus
        ┌──┴───┐   ┌───┴──┐   ┌───┴──┐
        │ I/O  │   │ I/O  │   │ I/O  │     … heterogeneous MCU nodes
        │ node │   │ node │   │ node │
        └──────┘   └──────┘   └──────┘
```

- A **proxy container** terminates the USB or WiFi transport and exposes the I/O layer as
  **services**. Each remote node — and each capability on it — is an addressable service, not
  a memory-mapped pin on one board.
- **Local processes and external Linux systems consume those services identically.** The I/O
  is decoupled from the processor that happens to host the proxy: move the compute, swap the
  SBC, or add a second consumer, and the I/O nodes are unaffected.
- Because the proxy is a container, the whole I/O subsystem is **deployed, versioned, and
  replaced like any other service** — and the two proxy flavors (USB, WiFi) are
  interchangeable over one bus protocol.
- **This pattern is not exotic — it is what the reference product already does.** Even the
  Arduino Uno Q exposes its on-board M33 to Linux as **RPC services** (MessagePack RPC over
  its "Bridge"), not as memory-mapped pins. The soldered co-processor and our bus of nodes
  are the *same idea — I/O reached as a service —* at different scales; our contribution is
  to generalize Arduino's one captive MCU into a **relocatable, multi-consumer bus of nodes**
  reached over the USB/WiFi ports every edge box already has.

This is the load-bearing idea: **I/O as a relocatable, multi-consumer service**, not a
co-processor.

## 5. The MCUs and the deterministic serial bus

The proposal stands on this layer, so concretely:

**The bus.** A **9-bit RS-485 multidrop serial bus at a fixed 115 200 baud, single-master
polled**, with granted peer windows. Single-master polling is what makes the timing
*deterministic*: every node knows when it may speak, there is no contention, and the cycle
time is bounded. The 9th bit carries address/control framing (MPCM-style) so nodes filter
traffic in hardware. Frames are **libcomm** — SLIP framing + CRC-8, a small command/reply
opcode set (`OP_SHELL_EXEC` / `OP_SHELL_REPLY`) plus asynchronous event/feedback frames.
Auto-direction transceivers and a `0xFF` preamble cover line turnaround, so there is no
DE-pin timing to mishandle.

**The nodes.** Inexpensive, purpose-fit microcontrollers, each running the same libcomm
contract plus a hardware-I/O command set — GPIO config/read/write, ADC with oversampling,
DAC, native PWM, pulse/edge counters, quadrature encoders, and supervisory interlocks:
- a **bus-controller MCU** (e.g. an RP2040 Pico W): one core runs the bus poll engine and the
  Linux uplink, the other runs the local application and safety interlocks;
- **peer-capable slaves** (e.g. RP2350-class): can also message each other inside their
  granted window for fast local reflexes that never need a Linux round-trip;
- **master-only legacy slaves** (e.g. SAMD21): run unchanged and ignore peer traffic —
  heterogeneous nodes interoperating through the *bus contract*, not a shared framework.

**The seam.** The bus controller streams the **same** libcomm frame stream whether the
uplink is USB-CDC or WiFi/UDP; only the transport beneath changes (a single uplink seam in
the firmware). That is why one bus protocol serves two interchangeable proxy flavors — and
why a node added to the bus appears as a service without rewiring the Linux side.

The net effect: real-time I/O on processors built for it, at low energy; the Linux box doing
compute and publishing the I/O as network-reachable services; and the physical interface
reduced to one rugged RS-485 pair instead of a stack of HATs.

## 6. What it costs (and the mitigations)

A position paper should name its own downsides:

- **Added latency.** Pin → node MCU → RS-485 → proxy → service is slower than a direct GPIO
  read. *Mitigation:* keep tight control loops **local to the node or its bus peers** (the
  granted peer window lets two nodes interact without a Linux round-trip); only supervisory
  and cross-node coordination crosses the seam. Real-time deadlines are met on the MCU, not
  over the wire.
- **A new component to fail.** The bus-controller MCU and the proxy are now in the path.
  *Mitigation:* a bus-progress watchdog on the controller, node-side interlocks that hold a
  safe state without Linux, and a stateless proxy that re-accepts the link after a transport
  blip.
- **Throughput ceiling.** 115 200 baud, single-master polling, is *slow* by design — it
  buys determinism and cheap wiring, not bandwidth. *Mitigation:* it is sized for control and
  sensor I/O, not bulk data; a faster sibling bus (higher baud, peer fabric) reuses the same
  seam where bandwidth matters.

These are the right trades for **machine I/O**: bounded timing, low energy, rugged wiring,
and serviceable nodes, in exchange for bandwidth and a few milliseconds of supervisory
latency.

## 7. Recommendation — where the work runs

| Job | Runs on | Why |
|---|---|---|
| Real-time I/O (PWM, ADC, encoders, interlocks, tight loops) | **MCU node** (hardware peripherals) | deterministic, low-energy, the processor built for it |
| Local reflexes / node-to-node coordination | **Bus peers** in their granted window | no Linux round-trip; bounded latency |
| Compute, connectivity, service exposure | **Edge Linux box** (containers) | what an application processor is for |
| Reaching the I/O | **USB 2.0 host or WiFi proxy container** | ports every edge box already has; I/O as a service |
| Cross-machine / remote consumers | **The same services, over the network** | I/O decoupled from any one processor |

Concretely:

- **Never do low-level I/O on the edge application processor.** It is the wrong processor —
  expensive, energy-hungry, and non-deterministic. Offload to an MCU.
- **Go past the bolt-on.** Don't stop at one MCU per board (the Uno Q model); expose the I/O
  as **services** over USB/WiFi so it is relocatable and multi-consumer.
- **Put the nodes on a deterministic single-master serial bus.** Cheap rugged wiring,
  bounded timing, heterogeneous nodes through one contract.

This composes directly with the companion paper: the **services** here are the Zenoh services
there — the proxy container is the Zenoh agent, exposing the bus as RPC + pub/sub keys so any
process, local or remote, consumes a remote MCU's I/O the same way it consumes any other
fleet service. One stack, two layers: **deterministic bus at the embedded edge, Zenoh fabric
on Linux, the right processor doing each job.**
