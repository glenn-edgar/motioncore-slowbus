// ============================================================================
// host_link.h — the northbound dongle protocol, as a small transport-independent
// state machine.
//
// This is the hand-written "shrink" of the s_engine register_dongle chain: the
// Pi-side linux/bus_controller drives a four-layer sync ladder against every
// dongle it opens, and a bus controller must answer it to be recognised and
// driven. host_link implements exactly that conversation over the libcomm
// SLIP+CRC frame codec, with no dependency on USB, FreeRTOS, or the bus — so it
// builds and unit-tests on a host.
//
// The ladder (driven by the Pi, answered here):
//   boot         -> we announce OP_REGISTER (v2 identity) periodically
//   REGISTER_ACK -> Pi acknowledged; it will ask for the manifest next
//   GET_MANIFEST -> we reply OP_MANIFEST_REPLY (schema_hash, fw, m2s opcodes)
//   OPERATIONAL_BEGIN -> we go OPERATIONAL and start OP_HEARTBEAT
//   PING         -> OP_PONG (any state)
//
// Frame routing on RX:
//   addr == 0 (the dongle itself): ladder opcodes handled internally; an
//             OP_SHELL_EXEC is a BC-LOCAL command -> on_local_shell callback.
//   addr != 0: destined for that bus node -> on_bus_msg callback (the caller
//             bridges it over the southbound bus and relays the reply via
//             host_link_s2m).
//
// The caller owns the transport: feed RX bytes with host_link_feed(), pump
// host_link_tick() for the timed REGISTER/HEARTBEAT, and drain TX bytes with
// host_link_tx_drain() to write to the wire.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "frame.h"   // vendored libcomm: codec + frame_ring_t + COMM_PAYLOAD_MAX

// Northbound TX staging ring. Sized to hold the largest reply (128 B payload +
// header + SLIP worst case) plus backlog of heartbeats / relayed bus replies.
#define HOST_LINK_TX_RING_SIZE  1024u

typedef enum {
    HL_BOOT        = 0,   // announcing REGISTER, waiting for the Pi
    HL_L1_ACKED    = 1,   // REGISTER_ACK seen
    HL_MANIFEST    = 2,   // MANIFEST_REPLY sent
    HL_OPERATIONAL = 3,   // OPERATIONAL_BEGIN seen — heartbeats flowing
} host_link_state_t;

// Identity announced in OP_REGISTER + the manifest schema_hash.
typedef struct {
    uint32_t class_id;             // role class (CLASS_ID_BUS_CONTROLLER ...)
    uint32_t instance_id;          // 0 if uncommissioned
    uint8_t  commissioning_state;  // 0=UNCOMMISSIONED, 1=COMMISSIONED
    uint8_t  chip_uid[16];         // factory UID (RP2040: 8 B board id + zero pad)
    uint16_t vid;
    uint16_t pid;
    uint32_t fw_version;           // (major<<16)|(minor<<8)|patch
    uint32_t build_date;           // packed YYYYMMDD
    uint32_t schema_hash;          // manifest schema hash (opaque to the Pi)
} host_link_cfg_t;

struct host_link;
typedef struct host_link host_link_t;

// addr != 0 frame: a command for bus node `dest`. `opcode` is OP_SHELL_EXEC or
// OP_BUS_EXEC; `body` is the frame payload after the opcode (the [req_id][cmd]
// [args] or [exec_timeout][req_id][cmd][args] tail). The handler bridges it to
// the node and relays the reply with host_link_s2m().
typedef void (*host_link_bus_cb)(void *user, uint8_t dest, uint16_t opcode,
                                 const uint8_t *body, uint8_t len);

// addr == 0 OP_SHELL_EXEC: a BC-local command. `args` is the tail after the
// [req_id][cmd] header. The handler runs it and answers with
// host_link_shell_reply(req_id, ...).
typedef void (*host_link_shell_cb)(void *user, uint16_t req_id, uint16_t cmd,
                                   const uint8_t *args, uint8_t alen);

struct host_link {
    host_link_cfg_t   cfg;
    host_link_state_t state;

    frame_ring_t      tx;
    uint8_t           tx_buf[HOST_LINK_TX_RING_SIZE];
    frame_decoder_t   rx;

    uint8_t           seq;             // s2m frame seq
    uint32_t          next_register_ms;
    uint32_t          next_heartbeat_ms;

    host_link_bus_cb   on_bus_msg;
    host_link_shell_cb on_local_shell;
    void              *user;
};

// Initialise with identity config. Sets state = HL_BOOT and schedules the first
// REGISTER. Callbacks are NULL until set by the caller after init.
void host_link_init(host_link_t *h, const host_link_cfg_t *cfg);

void host_link_set_callbacks(host_link_t *h, host_link_bus_cb on_bus_msg,
                             host_link_shell_cb on_local_shell, void *user);

// Feed one inbound wire byte. May invoke a callback when a frame completes.
void host_link_feed(host_link_t *h, uint8_t byte);

// Timed work: re-announce REGISTER until OPERATIONAL, then emit HEARTBEAT.
// `connected` = host-link up (CDC DTR). When down, emit nothing — the host
// can't read it and the frames would just accrue stale in the TX ring; on the
// disconnect edge the caller also calls host_link_reset_boot() so the next host
// gets a fresh ladder. (The "is USB connected throughout the protocol" condition,
// the SAMD21 lesson, gating emission here.)
void host_link_tick(host_link_t *h, uint32_t now_ms, bool connected);

// Re-arm the ladder to BOOT and re-announce REGISTER. Call on a host (re)attach
// edge — the RP2040 has no DTR-triggered reset, so this is how a newly-attached
// controller is given a fresh REGISTER + sync ladder (the SAMD21 EV_HOST_REATTACH
// equivalent). Drops any stale staged TX and the partial RX frame.
void host_link_reset_boot(host_link_t *h);

// Drain up to `max` staged TX bytes into `dst`; returns the count written.
uint32_t host_link_tx_drain(host_link_t *h, uint8_t *dst, uint32_t max);

// Stage an s2m frame (relay a bus reply / push an event). Returns 0 on success,
// -1 if the TX ring is full (caller may retry after draining).
int host_link_s2m(host_link_t *h, uint8_t addr, uint16_t opcode,
                  const uint8_t *payload, uint8_t len);

// Convenience: stage an OP_SHELL_REPLY for a BC-local command (addr 0).
int host_link_shell_reply(host_link_t *h, uint16_t req_id, uint8_t status,
                          const uint8_t *result, uint8_t rlen);

host_link_state_t host_link_state(const host_link_t *h);
