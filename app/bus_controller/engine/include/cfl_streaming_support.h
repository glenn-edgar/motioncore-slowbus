#ifndef CFL_STREAMING_SUPPORT_H
#define CFL_STREAMING_SUPPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "cfl_runtime.h"
#include "cfl_common_function_headers.h"
#include "avro_common.h"
// ============ CORE PORT ABSTRACTION ============


typedef struct {
    cfl_port_t port;
} cfl_inport_t;

typedef struct {
    cfl_port_t port;
    unsigned   event_column_id;
} cfl_outport_t;

// ============ NODE DATA STRUCTURES ============

typedef struct {
    cfl_outport_t outport;
    void         *user_data;
    void         *packet_buffer;
} cfl_emit_node_t;

typedef struct {
    cfl_inport_t  inport;
    void         *user_data;
} cfl_inport_node_t;

typedef struct {
    cfl_inport_t  inport;
    cfl_outport_t outport;
    void         *user_data;
    void         *packet_buffer;
} cfl_transform_node_t;

// Container for collected packet pointers
typedef struct {
    void    **packets;
    unsigned *port_indices;
    unsigned  capacity;
    unsigned  count;
    bool      pending;  // true if sent, awaiting consumption
} cfl_packet_container_t;

// Collect node - collects packet pointers into container
typedef struct {
    cfl_inport_t           *inports;
    unsigned                inport_count;
    unsigned                output_event_id;
    unsigned                output_event_column_id;
    cfl_packet_container_t  container;
    void                   *user_data;
} cfl_collect_node_t;

// Collected sink node - event-only matching, no port verification
typedef struct {
    unsigned event_id;
    void    *user_data;
} cfl_collected_sink_node_t;

// ============ NODE RETRIEVAL HELPERS ============

static inline cfl_emit_node_t* cfl_get_emit_node(cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_emit_node_t *)cfl_heap_arena_get_node_ptr(rt->arena_system, node_index);
}

static inline cfl_inport_node_t* cfl_get_inport_node(cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_inport_node_t *)cfl_heap_arena_get_node_ptr(rt->arena_system, node_index);
}

static inline cfl_transform_node_t* cfl_get_transform_node(cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_transform_node_t *)cfl_heap_arena_get_node_ptr(rt->arena_system, node_index);
}

static inline cfl_collect_node_t* cfl_get_collect_node(cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_collect_node_t *)cfl_heap_arena_get_node_ptr(rt->arena_system, node_index);
}

static inline cfl_collected_sink_node_t* cfl_get_collected_sink_node(cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_collected_sink_node_t *)cfl_heap_arena_get_node_ptr(rt->arena_system, node_index);
}

// ============ PACKET OPERATIONS ============

bool cfl_packet_matches_port(const void *packet, const cfl_port_t *port);

static inline bool cfl_event_matches_port(unsigned event_id, const cfl_port_t *port) {
    return event_id == port->event_id;
}

bool cfl_streaming_event_matches(unsigned event_type, unsigned event_id,
                                  void *event_data, const cfl_inport_t *inport);

void cfl_emit_packet(cfl_runtime_handle_t *rt, const cfl_outport_t *outport, void *packet);

// ============ PORT INITIALIZATION ============

void outport_init(cfl_outport_t *outport, cfl_runtime_handle_t *rt,
                  const char *port_path, const char *event_column_path);


// Verify packet node
typedef struct {
    cfl_inport_t  inport;
    unsigned      user_aux_fn_index;
    void         *user_data;
} cfl_verify_packet_node_t;

static inline cfl_verify_packet_node_t* cfl_get_verify_packet_node(cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_verify_packet_node_t *)cfl_heap_arena_get_node_ptr(rt->arena_system, node_index);
}
static inline cfl_verify_packet_node_t* cfl_get_streaming_verify_node(cfl_runtime_handle_t *rt, unsigned node_index) {
    cfl_verify_fn_data_t *verify_data = (cfl_verify_fn_data_t *)cfl_heap_arena_get_node_ptr(rt->arena_system, node_index);
    return (cfl_verify_packet_node_t *)verify_data->auxiliary_data;
}
#ifdef __cplusplus
}
#endif

#endif

