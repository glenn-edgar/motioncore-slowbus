// ============================================================================
// bus_frame.h — chip-independent BC-2 frame codec + reentrant RX assembler.
//
// A frame on the wire (after a 0xFF preamble) is:
//   [dest|bit8] [src] [type] [seq] [len] [payload..len] [crc8]
// crc8 = CRC-8/AUTOSAR over dest,src,type,seq,len,payload (the 9th bit is not
// CRC'd). See bus_addr.h for the symbol-level (9-bit) constants.
//
// This module is pure logic: it turns a bus_frame_t into an array of 9-bit
// words (which the PHY transmits) and reassembles received 9-bit words back
// into frames. It touches no hardware — the same .c builds on RP2350, STM32,
// or a host unit test. The PHY (port/<chip>/) owns the electrical layer; this
// owns the format.
//
// Unlike the SAMD21 starting point, the assembler state lives in a caller-owned
// bus_asm_t (not file statics), so a node can run independent assemblers and
// the codec is trivially unit-testable off-target.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "bus_addr.h"

// Max payload bytes. len is a u8 so the hard ceiling is 255; the M33 nodes have
// memory to spare, but a larger MTU means a node holds the half-duplex bus
// longer, hurting round-robin latency — so keep it moderate and tunable.
#define BUS_PAYLOAD_MAX   240u

#define BUS_HEADER_LEN    5u   // dest,src,type,seq,len (before payload)
// Worst-case encoded symbol count: preamble + header + payload + crc.
#define BUS_FRAME_WORDS_MAX  (1u + BUS_HEADER_LEN + BUS_PAYLOAD_MAX + 1u)

typedef struct {
    uint8_t dest;
    uint8_t src;
    uint8_t type;
    uint8_t seq;
    uint8_t len;
    uint8_t payload[BUS_PAYLOAD_MAX];
} bus_frame_t;

// Encode a frame into `words` (must hold >= BUS_FRAME_WORDS_MAX). Returns the
// number of 9-bit words written: preamble + header + payload + crc. `len` is
// clamped to BUS_PAYLOAD_MAX.
uint16_t bus_frame_encode(uint16_t *words, const bus_frame_t *f);

// Compute the frame CRC over the header + payload (the value placed in the crc
// byte and recomputed on RX).
uint8_t bus_frame_crc(const bus_frame_t *f);

// --- Reentrant RX assembler -------------------------------------------------
typedef enum {
    BUS_ASM_IDLE, BUS_ASM_SRC, BUS_ASM_TYPE, BUS_ASM_SEQ,
    BUS_ASM_LEN, BUS_ASM_PAYLOAD, BUS_ASM_CRC
} bus_asm_state_t;

typedef struct {
    bus_asm_state_t state;
    bus_frame_t     acc;        // accumulator
    uint8_t         idx;        // payload write cursor
    uint8_t         my_addr;    // accept dest == my_addr (or broadcast/promiscuous)
    bool            promiscuous;// accept every dest (bus monitor)
    uint32_t        crc_fail;   // frames dropped on CRC mismatch
    uint32_t        frames_ok;  // complete CRC-valid frames returned
} bus_asm_t;

// Initialise an assembler. my_addr = the node's address (BUS_ADDR_MASTER for the
// BC). Pass promiscuous = true (or my_addr = BUS_ADDR_PROMISCUOUS) for a monitor.
void bus_asm_init(bus_asm_t *a, uint8_t my_addr, bool promiscuous);

// Feed one received 9-bit word. Returns true and fills *out once a complete,
// CRC-valid frame addressed to us (or broadcast / any in promiscuous mode) is
// reassembled. An address word (bit8=1) always (re)starts a frame, giving cheap
// resync after line noise.
bool bus_asm_feed(bus_asm_t *a, uint16_t word9, bus_frame_t *out);
