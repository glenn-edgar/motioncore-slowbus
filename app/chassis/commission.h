// ============================================================================
// commission.h — protected device-identity block in RP2040 flash.
//
// Two-tier identity (class_id + instance_id) per [[dongle_class_identity]], stored
// power-fail-safe: A/B double-buffered in the top two flash sectors, each with a
// magic+version+CRC32 header. Newest valid (by seq) wins. instance_id low byte =
// the node's RS-485 bus address.
//
// READ (commission_load / commission_bus_addr) is XIP-mapped, safe anytime.
// WRITE (commission_write) erases+programs flash: call PRE-scheduler (single core)
// or wrap in flash_safe_execute() at runtime (SMP XIP lockout).
// ============================================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define COMMISSION_ADDR_NONE 0xFFu

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t len;
    uint32_t seq;          // monotonic; newest valid copy wins
    uint16_t class_id;
    uint16_t instance_id;
    uint8_t  reserved[12];
    uint32_t crc32;        // over magic..reserved
} commission_t;

bool    commission_load(commission_t *out);                       // newest valid A/B
bool    commission_write(uint16_t class_id, uint16_t instance_id);// ping-pong write
uint8_t commission_bus_addr(void);   // instance_id low byte, or COMMISSION_ADDR_NONE
