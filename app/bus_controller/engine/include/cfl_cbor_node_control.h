/* ========================================================================
 * cfl_cbor_node_control.h — CBOR-based controlled node types and functions
 *
 * Same pattern as JSON controlled nodes but request/response data
 * travels as CBOR bytes on the event queue. User boolean functions
 * receive JSON text (decoded from CBOR) — same interface as JSON tests.
 *
 * Memory: node structs on arena (via smart alloc). CBOR byte buffers
 * on heap (freed in term functions). Safe for CFL_RESET loops.
 *
 * Naming convention (matches stage3 pipeline):
 *   DSL "CFL_CBOR_CONTROLLED_NODE_MAIN" -> reg "cfl_cbor_controlled_node_main_main"
 *   DSL "CFL_CBOR_CONTROLLED_NODE_INIT" -> reg "cfl_cbor_controlled_node_init_one_shot"
 *   C symbol = registration name + "_fn"
 * ======================================================================== */

#ifndef CFL_CBOR_NODE_CONTROL_H
#define CFL_CBOR_NODE_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "cfl_runtime.h"
#include "cfl_cbor_packets.h"
#include "cfl_json_packets.h"
#include "cfl_cbor_functions.h"

/* ── CBOR port: same as JSON — only event_id needed ── */
typedef struct {
    unsigned event_id;
} cfl_cbor_port_t;

/* ── Server (controlled/dead node) ── */
typedef struct {
    cfl_cbor_port_t           request_port;
    cfl_cbor_port_t           response_port;
    unsigned                  client_node_index;
    cfl_json_heap_interface_t heap_iface;       /* for JSON parse/free */
    cfl_cbor_heap_interface_t cbor_heap_iface;  /* for CBOR transcode */
    char                     *response_text;    /* heap: JSON text from user finalizer */
    uint8_t                  *cbor_buffer;      /* heap: for encoding responses */
    cfl_cbor_buffer_t         cbor_ref;         /* for sending CBOR response */
    void                     *aux_data;
} cfl_cbor_server_controlled_node_t;

/* ── Client (initiator node) ── */
typedef struct {
    cfl_cbor_port_t           request_port;
    cfl_cbor_port_t           response_port;
    unsigned                  server_node_index;
    cfl_json_heap_interface_t heap_iface;
    cfl_cbor_heap_interface_t cbor_heap_iface;
    void                     *aux_data;
    bool                      node_is_active;
    uint8_t                  *cbor_request;     /* heap: CBOR-encoded request */
    size_t                    cbor_request_len;
    cfl_cbor_buffer_t         cbor_ref;         /* for sending CBOR request */
} cfl_cbor_client_controlled_node_t;

/* ========================================================================
 * SERVER FUNCTIONS
 * ======================================================================== */

void     cfl_cbor_controlled_node_init_one_shot_fn(void *handle, unsigned node_index);
unsigned cfl_cbor_controlled_node_main_main_fn(void *handle, unsigned bool_function_index,
             unsigned node_index, unsigned event_type, unsigned event_id, void *event_data);
void     cfl_cbor_controlled_node_term_one_shot_fn(void *handle, unsigned node_index);

/* ========================================================================
 * CLIENT FUNCTIONS
 * ======================================================================== */

void     cfl_cbor_client_controlled_node_init_one_shot_fn(void *handle, unsigned node_index);
unsigned cfl_cbor_client_controlled_node_main_main_fn(void *handle, unsigned bool_function_index,
             unsigned node_index, unsigned event_type, unsigned event_id, void *event_data);
void     cfl_cbor_client_controlled_node_term_one_shot_fn(void *handle, unsigned node_index);

/* ========================================================================
 * HELPER FUNCTIONS
 * ======================================================================== */

uint16_t cfl_cbor_server_get_server_node_index(cfl_runtime_handle_t *handle, unsigned node_index);
cfl_cbor_server_controlled_node_t *cfl_cbor_server_get_node(
    cfl_runtime_handle_t *handle, unsigned node_index);
void cfl_cbor_server_set_response_text(cfl_runtime_handle_t *handle,
    unsigned node_index, const char *text);

/* Inline retrieval helpers */
static inline cfl_cbor_server_controlled_node_t *cfl_get_cbor_server_node(
        cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_cbor_server_controlled_node_t *)cfl_heap_arena_get_node_ptr(
        rt->arena_system, node_index);
}

static inline cfl_cbor_client_controlled_node_t *cfl_get_cbor_client_node(
        cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_cbor_client_controlled_node_t *)cfl_heap_arena_get_node_ptr(
        rt->arena_system, node_index);
}

#ifdef __cplusplus
}
#endif

#endif /* CFL_CBOR_NODE_CONTROL_H */
