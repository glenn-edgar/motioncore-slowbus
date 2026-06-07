// ============================================================================
// bus_addr.h — fast-bus address space + 9-bit MPCM framing constants.
//
// Chip-independent. Defines the wire-level constants every node and the bus
// controller share: the 128-node address space, the 9-bit address/data marker,
// and the BC-2 frame-type byte. See docs/protocol.md for the full wire spec.
// ============================================================================
#pragma once

#include <stdint.h>

// --- Address space (7-bit value, 128 addresses) -----------------------------
// The 9th UART bit (see below) marks the address byte; the address *value*
// itself is 7 bits, 0..127. The bus controller, every slave, and a peer
// destination all draw from this one space.
#define BUS_ADDR_BITS        7u
#define BUS_ADDR_MASK        0x7Fu
#define BUS_ADDR_COUNT       128u

#define BUS_ADDR_MASTER      0x00u   // the bus controller / arbiter
#define BUS_ADDR_BROADCAST   0x7Fu   // every node accepts (reserved, not a slave)
// Valid unicast slave addresses: 0x01 .. 0x7E.
#define BUS_ADDR_SLAVE_MIN   0x01u
#define BUS_ADDR_SLAVE_MAX   0x7Eu

// my_addr sentinel for a promiscuous bus monitor (accept every dest). Out of
// the 7-bit range so it can never collide with a real node.
#define BUS_ADDR_PROMISCUOUS 0xFFu

// --- 9-bit MPCM framing -----------------------------------------------------
// Each on-wire symbol is a 9-bit word. Bit 8 is the address/data marker:
//   bit8 = 1  -> this byte is a destination address: "new message starts here"
//   bit8 = 0  -> data byte (header field, payload, or CRC)
// The RX path keys off bit8 for frame-start resync and dest filtering — exactly
// the role the SAMD21/SERCOM 9-bit MPCM mode served, now portable to the
// RP2350 PIO and (natively) to the STM32 USART address-mark mode.
#define BUS_WORD_ADDR_BIT    0x100u
#define BUS_WORD_DATA_MASK   0x0FFu
#define BUS_WORD_ADDR(a)     (BUS_WORD_ADDR_BIT | ((a) & BUS_ADDR_MASK))
#define BUS_WORD_DATA(b)     ((uint16_t)((b) & 0xFFu))
#define BUS_WORD_IS_ADDR(w)  (((w) & BUS_WORD_ADDR_BIT) != 0u)
#define BUS_PREAMBLE_WORD    0x0FFu   // data byte 0xFF: RC/transceiver settle, not in CRC

// --- Frame type byte --------------------------------------------------------
// Low nibble = frame class; bit4 = reliability (TCP). Unchanged from BC-2 so
// the wire stays compatible; the addressing is what becomes peer-general.
#define BUS_FT_MASK          0x0Fu
#define BUS_FT_NO_MESSAGE    0x00u   // end-of-window terminator (hands the bus back)
#define BUS_FT_POLL          0x01u   // BC->node: "your window is open" (grant)
#define BUS_FT_DATA          0x02u   // payload = [opcode:u16-LE][body]
#define BUS_FT_ACK           0x03u   // reliable DATA seq received CRC-good
#define BUS_FT_NAK           0x04u   // reliable DATA seq received CRC-bad
#define BUS_TF_TCP           0x10u   // bit4: reliable (ack-tracked); else fire-and-forget
