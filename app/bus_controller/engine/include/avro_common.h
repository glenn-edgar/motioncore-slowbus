#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "cfl_runtime.h"

typedef struct {
    uint32_t    schema_hash;       // Per-record hash
    unsigned    handler_id;        // Record index (for dispatch tables)
    unsigned    event_id;
    void        *packet_pointer;
    void        *data_pointer;
} cfl_port_t;

// Generic wire header (matches all generated _wire_header_t structs)
// 16 bytes, packed, no pointers — socket-safe
// schema_hash is per-record: FNV-1a of "<file>.h:<record>"
typedef struct __attribute__((packed)) {
    double      timestamp;     // 8 bytes (offset 0)
    uint32_t    schema_hash;   // 4 bytes (offset 8)  — per-record hash
    uint16_t    seq;           // 2 bytes (offset 12)
    uint16_t    source_node;   // 2 bytes (offset 14)
} avro_packet_header_t;

_Static_assert(sizeof(avro_packet_header_t) == 16, "avro_packet_header_t must be 16 bytes");

static inline uint16_t cfl_avro_get_source_node(const void* packet_buffer)
{
    const avro_packet_header_t* hdr = (const avro_packet_header_t*)packet_buffer;
    return hdr->source_node;
}

static inline uint32_t cfl_avro_get_schema_hash(const void* packet_buffer)
{
    const avro_packet_header_t* hdr = (const avro_packet_header_t*)packet_buffer;
    return hdr->schema_hash;
}

bool cfl_packet_matches_port(const void *packet, const cfl_port_t *port);

void cfl_avro_decode_port(const cfl_runtime_handle_t *runtime, const char *port_path, cfl_port_t *port);

void cfl_avro_update_packet_header(cfl_runtime_handle_t *runtime, void *packet);

void cfl_avro_update_packet_header_source_node(cfl_runtime_handle_t *runtime, void *packet, unsigned source_node);
#ifdef __cplusplus
}
#endif