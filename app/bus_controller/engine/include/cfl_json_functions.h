#ifndef CFL_JSON_FUNCTIONS_H
#define CFL_JSON_FUNCTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "cfl_runtime.h"
#include "cfl_json_packets.h"

/* ========================================================================
 * JSON PACKET NODE STRUCTURES
 *
 * All nodes use cfl_allocate_state / cfl_smart_arena_alloc so they
 * survive CFL_RESET loops without re-allocating.
 *
 * Naming convention (matches stage3 pipeline):
 *   DSL name "CFL_JSON_SINK" → registration "cfl_json_sink_main"
 *   DSL name "CFL_JSON_SINK_INIT" → registration "cfl_json_sink_init_one_shot"
 *   C symbol = registration name + "_fn"
 * ======================================================================== */

/* ── Emit node (one-shot): queues a JSON text string ── */
typedef struct {
    char                       *text_buffer;      /* heap-allocated, fixed size */
    unsigned                    text_buffer_size;
    unsigned                    event_column_id;
    unsigned                    event_id;
    cfl_heap_t                 *heap;             /* for freeing text_buffer */
} cfl_json_emit_node_t;

/* ── Sink node: consumes JSON packets ── */
typedef struct {
    unsigned                    event_id;
    cfl_json_heap_interface_t   heap_iface;
    void                       *user_data;
} cfl_json_sink_node_t;

/* ── Transform node: receives JSON, user modifies, emits new JSON ── */
typedef struct {
    unsigned                    input_event_id;
    unsigned                    output_event_id;
    unsigned                    output_event_column_id;
    cfl_json_packet_t          *out_packet;       /* created at init, reused */
    cfl_json_heap_interface_t   heap_iface;
    char                       *text_buffer;      /* heap-allocated, fixed size */
    unsigned                    text_buffer_size;
    void                       *user_data;
} cfl_json_transform_node_t;

/* ── Dispatch route entry ── */
typedef struct {
    uint32_t    type_hash;
    unsigned    column_id;
} cfl_json_dispatch_route_entry_t;

/* ── Dispatch node: routes by "type" field ── */
typedef struct {
    unsigned                         event_id;        /* input event to listen for */
    unsigned                         output_event_id; /* event ID for forwarded packets */
    cfl_json_dispatch_route_entry_t *routes;
    unsigned                         route_count;
    unsigned                         default_column_id;
    cfl_json_heap_interface_t        heap_iface;
    void                            *user_data;
} cfl_json_dispatch_node_t;

/* ========================================================================
 * NODE RETRIEVAL HELPERS
 * ======================================================================== */

static inline cfl_json_emit_node_t *cfl_get_json_emit_node(
        cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_json_emit_node_t *)cfl_heap_arena_get_node_ptr(
        rt->arena_system, node_index);
}

static inline cfl_json_sink_node_t *cfl_get_json_sink_node(
        cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_json_sink_node_t *)cfl_heap_arena_get_node_ptr(
        rt->arena_system, node_index);
}

static inline cfl_json_transform_node_t *cfl_get_json_transform_node(
        cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_json_transform_node_t *)cfl_heap_arena_get_node_ptr(
        rt->arena_system, node_index);
}

static inline cfl_json_dispatch_node_t *cfl_get_json_dispatch_node(
        cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_json_dispatch_node_t *)cfl_heap_arena_get_node_ptr(
        rt->arena_system, node_index);
}

/* ========================================================================
 * HEAP INTERFACE HELPER
 * ======================================================================== */

void cfl_json_bind_heap(cfl_json_heap_interface_t *iface, cfl_heap_t *heap);

/* ========================================================================
 * CFL ONE-SHOT FUNCTIONS
 * DSL name → stage3 name → C symbol
 * ======================================================================== */

/* CFL_JSON_EMIT → cfl_json_emit_one_shot → cfl_json_emit_one_shot_fn */
void cfl_json_emit_one_shot_fn(void *handle, unsigned node_index);
/* CFL_JSON_EMIT_TERM → cfl_json_emit_term_one_shot → cfl_json_emit_term_one_shot_fn */
void cfl_json_emit_term_one_shot_fn(void *handle, unsigned node_index);

/* CFL_JSON_SINK_INIT → cfl_json_sink_init_one_shot → cfl_json_sink_init_one_shot_fn */
void cfl_json_sink_init_one_shot_fn(void *handle, unsigned node_index);
/* CFL_JSON_SINK_TERM → cfl_json_sink_term_one_shot → cfl_json_sink_term_one_shot_fn */
void cfl_json_sink_term_one_shot_fn(void *handle, unsigned node_index);

/* CFL_JSON_TRANSFORM_INIT → cfl_json_transform_init_one_shot */
void cfl_json_transform_init_one_shot_fn(void *handle, unsigned node_index);
/* CFL_JSON_TRANSFORM_TERM → cfl_json_transform_term_one_shot */
void cfl_json_transform_term_one_shot_fn(void *handle, unsigned node_index);

/* CFL_JSON_DISPATCH_INIT → cfl_json_dispatch_init_one_shot */
void cfl_json_dispatch_init_one_shot_fn(void *handle, unsigned node_index);
/* CFL_JSON_DISPATCH_TERM → cfl_json_dispatch_term_one_shot */
void cfl_json_dispatch_term_one_shot_fn(void *handle, unsigned node_index);

/* ========================================================================
 * CFL MAIN FUNCTIONS
 * ======================================================================== */

/* CFL_JSON_SINK → cfl_json_sink_main → cfl_json_sink_main_fn */
unsigned cfl_json_sink_main_fn(void *handle, unsigned bool_function_index,
    unsigned node_index, unsigned event_type, unsigned event_id, void *event_data);

/* CFL_JSON_TRANSFORM → cfl_json_transform_main → cfl_json_transform_main_fn */
unsigned cfl_json_transform_main_fn(void *handle, unsigned bool_function_index,
    unsigned node_index, unsigned event_type, unsigned event_id, void *event_data);

/* CFL_JSON_DISPATCH → cfl_json_dispatch_main → cfl_json_dispatch_main_fn */
unsigned cfl_json_dispatch_main_fn(void *handle, unsigned bool_function_index,
    unsigned node_index, unsigned event_type, unsigned event_id, void *event_data);

#ifdef __cplusplus
}
#endif

#endif /* CFL_JSON_FUNCTIONS_H */
