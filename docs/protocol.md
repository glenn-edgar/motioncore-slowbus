# fast_bus protocol — 9-bit RS-485 peer fabric

Status: **design, scaffold landed 2026-06-05.** Derived from the motioncore
SAMD21 bus-controller BC-2/BC-3 wire format (`reference/samd21/`), generalized
to a peer-messaging fabric and retargeted to the RP2350 (Pico 2 W).

This repo is **fully separate** from motioncore — its own repository, its own
build container. It shares only the wire-level CRC and frame shape so a bridge is
possible later; it carries none of the s_engine / commissioning / interlock stack
(the SAMD21 bus controller was stripped on purpose, and we keep it lean).

---

## 1. What's the same as BC-2, what's new

**Same (wire-compatible frame):**

```
  0xFF                 preamble (data byte, 9th bit = 0) — RC/transceiver settle
  [dest | 9th bit=1]   destination address  (MPCM address marker: "new message")
  [src]                source address
  [type]               frame class + TCP flag
  [seq]                sequence (reliable correlation; 0 on fire-and-forget)
  [len]                payload length, 0..240
  [payload ...]        len bytes
  [crc8]               CRC-8/AUTOSAR over dest..payload (9th bit not CRC'd)
```

**New:**

| Aspect | SAMD21 BC-2/BC-3 | fast_bus |
|---|---|---|
| Address space | 8-bit value, 1..254 | **7-bit value, 128 nodes (0..127)**; 9th bit = marker only |
| Topology | strict master→slave; slave→master only | **peer fabric**: any node → any node |
| Baud | 115200 | **400000** target |
| Controller PHY | SERCOM 9-bit MPCM (hardware) | **RP2350 PIO** 9-bit + DMA + side-set DE |
| BC role | near-dumb USB↔485 bridge | **active node**: originates + routes traffic |
| Nodes | trivial M0+/M4 slaves | **M33 with memory** (Pico 2 W now; STM32 later) |

The 9th bit's job is exactly what the requirement states: it tells the RX
interrupt handler "a new message starts here, and this byte is who it's for." The
address *value* is 7 bits; bit 8 is the address/data discriminator, never part of
the value.

---

## 2. Addressing (`core/bus_addr.h`)

- `0x00` = bus controller / master.
- `0x01..0x7E` = unicast nodes.
- `0x7F` = broadcast (every node accepts; reserved, not assignable).
- `0xFF` (out of 7-bit range) = a monitor's "promiscuous" sentinel, never on the wire.

Each node accepts a frame iff `dest == my_addr || dest == broadcast` (or it is a
promiscuous monitor). With M33 nodes and DMA the cost of hearing every frame and
software-filtering is negligible — which is *why* peer messaging is cheap here and
why the hardware address-match the SAMD21 leaned on is no longer load-bearing.

---

## 3. Arbitration — BC-granted windows with a peer tail

The controller owns "who talks when"; nodes never transmit unsolicited, so the
shared half-duplex pair stays collision-free. Per round, for the next enabled
node in the roster:

1. **Grant.** The BC sends either a queued `DATA` for that node (which *is* the
   grant) or a bare `POLL`.
2. **Window.** The granted node may, back-to-back:
   - reply to the BC (`DATA`, dest = master),
   - `ACK`/`NAK` a reliable DATA it was just given,
   - **send one or more `DATA` frames directly to other nodes** (dest = peer) —
     the peer-fabric step, delivered in one hop, *not* relayed by the BC,
   - yield with `NO_MESSAGE`.
3. The BC listens until `NO_MESSAGE` or the window budget elapses, forwarding
   node→master frames up the uplink and (optionally) observing peer frames.

The BC is itself a node: it injects its own `DATA` (from the uplink or
self-originated) into the rotation, satisfying "the controller can both receive
and transmit messages," not just poll.

```
  BC ──POLL/DATA──▶ node K          (grant)
  node K ──DATA(dest=BC)──▶          (reply to controller, optional)
  node K ──DATA(dest=J)──▶ node J    (peer message, 1 hop, BC does not relay)
  node K ──DATA(dest=M)──▶ node M    (one or more peer messages)
  node K ──NO_MESSAGE──▶             (yield the bus)
  BC advances to node K+1
```

---

## 4. Reliability

- **Reliable (TCP, `type` bit4):** BC→node DATA only. BC assigns `seq`, tracks it
  in an ack-table; node validates CRC → `ACK`/`NAK` in its window; BC retransmits
  on NAK/timeout up to `tcp_retries`, then marks the node DEAD.
- **Fire-and-forget (UDP):** everything else — POLL, NO_MESSAGE, node→master, and
  **peer node→node** frames. CRC-checked; a bad one is simply dropped and the
  sender's higher layer retries.

Open question for the peer step: do node→node messages need their own end-to-end
ack (peer-level reliable), or is fire-and-forget + app retry enough? Deferred
until a real peer use-case sets the requirement.

---

## 5. DATA payload = opcode + body

A `DATA` payload is `[opcode:u16-LE][body]`, the same opcode space as the
motioncore USB-CDC link, so the BC bridges bodies to/from the host uplink with no
per-opcode translation. Max body = 240 − 2 = 238 B; larger needs fragmentation
(a later slice).

---

## 6. Watchdog coupling (do not defer)

Autonomous granting breaks "the WDT proves the engine ticks" — a wedged grant
loop could still tick. So the bus-progress watchdog ships *with* the poll engine:
pet only on real RS-485 RX/TX byte progress within a bounded silent-wait, so a
stuck bus trips recovery while a healthy idle loop does not. (Same lesson as the
SAMD21 BC-3 spec, §7 of `reference/samd21/rs485-bus-protocol-bc2-bc3.md`.)

---

## 7. Status / next

Landed: chip-independent core (`core/`) — frame codec + CRC + roster, host-tested
(`tools/`); the two abstraction seams (`bus_phy.h`, `bus_uplink.h`); RP2350 app +
build skeletons. See `README.md` for the build/bring-up order. The PIO PHY, the
scheduler/node inner loops, and the USB-CDC host format are the first bench tasks.
