// ============================================================================
// bus_sched.h — bus controller arbiter (the "who talks when" scheduler).
//
// Chip-independent. Drives the round-robin grant cycle over the roster and
// owns the peer-fabric arbitration: each node, in its granted window, may reply
// to the BC and then send one or more DATA frames directly to *other* nodes
// before yielding with NO_MESSAGE. The BC never relays those peer frames — it
// only schedules the window in which they are allowed (collision-free by
// construction on the shared half-duplex pair).
//
// The BC is itself an active node: it injects its own DATA frames (from the
// uplink, or self-originated) into the rotation, not just POLL grants.
//
// Per round, for the next enabled node:
//   1. If a host->node (or self-originated) frame is queued: send it (DATA,
//      TCP-tracked if flagged) — this doubles as the node's grant.
//      Else send POLL (grant only).
//   2. Listen until the node yields (NO_MESSAGE) or the window budget elapses:
//        ACK/NAK(seq) -> resolve the reliable-DATA ack-table entry
//        DATA(dest=BC) -> forward body up the uplink (reply/event/log)
//        DATA(dest=peer) -> a node->node frame; the BC may mirror it up if the
//                           host asked to observe peer traffic, else ignore
//        (timeout)    -> consecutive_misses++; mark DEAD past max_misses
//   3. First good frame after DEAD -> ALIVE; notify the host.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "bus_frame.h"

// Initialise scheduler state (ack-table, cursor, queues). Roster is separate.
void bus_sched_init(void);

// Queue a frame for a node, sent on its next grant slot. Returns false if the
// per-node outbound queue is full. dest is the target node; the BC stamps src =
// BUS_ADDR_MASTER. tcp selects reliable (ack-tracked) delivery.
bool bus_sched_queue_tx(uint8_t dest, uint8_t type, const uint8_t *payload,
                        uint8_t len, bool tcp);

// Run exactly one grant slot (one node's window). Call from the scheduler task.
// Returns the node address serviced, or 0 if the roster has no enabled node.
// Blocks for at most the configured window budget. This is the unit the
// bus-progress watchdog pets around (pet only on real RX/TX byte progress).
uint8_t bus_sched_tick(void);

// --- Hooks the app/uplink layer implements (weak defaults provided) ---------
// Called for every frame the BC receives whose dest == BUS_ADDR_MASTER — i.e. a
// node's reply/event/log destined for the host. Default forwards via uplink.
void bus_sched_on_node_to_master(const bus_frame_t *f);

// Called for a node->peer frame the BC overhears, when peer-observe is enabled.
// Default is a no-op (the BC does not relay; the addressed peer already has it).
void bus_sched_on_peer_frame(const bus_frame_t *f);

// Called on a node liveness edge (BUS_STATE_ALIVE/DEAD). Default notifies host.
void bus_sched_on_node_state(uint8_t addr, uint8_t new_state, uint32_t class_id);
