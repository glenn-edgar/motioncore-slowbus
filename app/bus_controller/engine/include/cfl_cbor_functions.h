#ifndef CFL_CBOR_FUNCTIONS_H
#define CFL_CBOR_FUNCTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "cfl_runtime.h"
#include "cfl_cbor_packets.h"
#include "cfl_json_packets.h"

/* ========================================================================
 * CBOR PACKET NODE STRUCTURES
 *
 * Same pattern as JSON functions but event data on the wire is CBOR bytes.
 * Emit: DSL JSON text -> CBOR bytes -> event queue
 * Sink: CBOR bytes from event -> JSON text -> cfl_json_packet_t -> user bool
 *
 * User boolean functions see cfl_json_packet_t — identical to JSON tests.
 *
 * Memory: node structs live on arena (via smart alloc). CBOR byte buffers
 * live on the heap (pointers in the node struct). Term functions free heap.
 * ======================================================================== */

/* CBOR buffer reference — passed as event_data on the event queue */
typedef struct {
    uint8_t *data;
    size_t   len;
} cfl_cbor_buffer_t;

/* ── Emit node (one-shot): converts JSON text to CBOR, queues CBOR ── */
typedef struct {
    uint8_t                    *cbor_buffer;       /* heap-allocated */
    unsigned                    cbor_buffer_size;
    unsigned                    event_column_id;
    unsigned                    event_id;
    cfl_cbor_buffer_t           cbor_ref;          /* event_data payload */
    cfl_heap_t                 *heap;              /* for freeing cbor_buffer */
} cfl_cbor_emit_node_t;

/* ── Sink node: receives CBOR, decodes to JSON for user boolean ── */
typedef struct {
    unsigned                    event_id;
    cfl_json_heap_interface_t   heap_iface;
    cfl_cbor_heap_interface_t   cbor_heap_iface;
} cfl_cbor_sink_node_t;

/* ========================================================================
 * NODE RETRIEVAL HELPERS
 * ======================================================================== */

static inline cfl_cbor_emit_node_t *cfl_get_cbor_emit_node(
        cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_cbor_emit_node_t *)cfl_heap_arena_get_node_ptr(
        rt->arena_system, node_index);
}

static inline cfl_cbor_sink_node_t *cfl_get_cbor_sink_node(
        cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_cbor_sink_node_t *)cfl_heap_arena_get_node_ptr(
        rt->arena_system, node_index);
}

/* ========================================================================
 * HEAP INTERFACE HELPER
 * ======================================================================== */

void cfl_cbor_bind_heap(cfl_cbor_heap_interface_t *iface, cfl_heap_t *heap);

/* ========================================================================
 * CFL ONE-SHOT FUNCTIONS
 * ======================================================================== */

/* CFL_CBOR_EMIT -> cfl_cbor_emit_one_shot -> cfl_cbor_emit_one_shot_fn */
void cfl_cbor_emit_one_shot_fn(void *handle, unsigned node_index);
/* CFL_CBOR_EMIT_TERM -> cfl_cbor_emit_term_one_shot -> cfl_cbor_emit_term_one_shot_fn */
void cfl_cbor_emit_term_one_shot_fn(void *handle, unsigned node_index);

/* CFL_CBOR_SINK_INIT -> cfl_cbor_sink_init_one_shot -> cfl_cbor_sink_init_one_shot_fn */
void cfl_cbor_sink_init_one_shot_fn(void *handle, unsigned node_index);
/* CFL_CBOR_SINK_TERM -> cfl_cbor_sink_term_one_shot -> cfl_cbor_sink_term_one_shot_fn */
void cfl_cbor_sink_term_one_shot_fn(void *handle, unsigned node_index);

/* ========================================================================
 * CFL MAIN FUNCTIONS
 * ======================================================================== */

/* CFL_CBOR_SINK -> cfl_cbor_sink_main -> cfl_cbor_sink_main_fn */
unsigned cfl_cbor_sink_main_fn(void *handle, unsigned bool_function_index,
    unsigned node_index, unsigned event_type, unsigned event_id, void *event_data);

#ifdef __cplusplus
}
#endif

#endif /* CFL_CBOR_FUNCTIONS_H */
