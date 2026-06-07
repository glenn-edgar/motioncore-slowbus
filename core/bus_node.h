// ============================================================================
// bus_node.h — slave-side bus logic (ROLE=slave).
//
// Chip-independent. A node listens for frames addressed to it. When it receives
// its grant (a POLL, or a DATA from the BC), it owns the bus for one window and:
//   1. answers the BC if it has a reply/event/log queued  (DATA, dest=MASTER),
//   2. emits an ACK/NAK if the grant carried reliable DATA,
//   3. sends any queued PEER frames directly to other nodes (DATA, dest=peer),
//   4. yields the bus with NO_MESSAGE.
//
// Steps 1-4 happen back-to-back inside the window so the BC sees a clean burst
// terminated by NO_MESSAGE. A node only ever transmits inside its own granted
// window — that is what keeps the shared half-duplex pair collision-free while
// still allowing node->node messaging.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "bus_frame.h"

// my_addr = this node's bus address (1..126).
void bus_node_init(uint8_t my_addr);

// Queue a frame to send during this node's next window. dest = BUS_ADDR_MASTER
// for a reply/event up to the BC, or another node address for a peer message.
// Returns false if the outbound queue is full.
bool bus_node_queue(uint8_t dest, uint8_t type, const uint8_t *payload, uint8_t len);

// Service the bus: drain RX, and when a grant addressed to us arrives, transmit
// the window (queued frames + NO_MESSAGE terminator). Call from the node task.
void bus_node_task(void);

// --- Hook the application implements ----------------------------------------
// Called for every DATA frame delivered to this node — from the BC or a peer.
// The app dispatches by the inner opcode (payload = [opcode:u16-LE][body]) and
// may bus_node_queue() a reply. src tells you who sent it.
void bus_node_on_data(uint8_t src, const uint8_t *payload, uint8_t len);
