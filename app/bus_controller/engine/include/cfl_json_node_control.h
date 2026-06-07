/* ========================================================================
 * cfl_json_node_control.h — JSON-based controlled node types and functions
 *
 * Simplified version of the Avro controlled node pattern:
 *   - Ports carry only event_id (no schema_hash, handler_id, packet_pointer)
 *   - Client node_index set at runtime by client before server activation
 *   - Request/response data is JSON text (char*), not Avro packets
 *   - Container functions are format-agnostic (reuse existing)
 *
 * Naming convention (matches stage3 pipeline):
 *   DSL "CFL_JSON_CONTROLLED_NODE_MAIN" -> reg "cfl_json_controlled_node_main_main"
 *   DSL "CFL_JSON_CONTROLLED_NODE_INIT" -> reg "cfl_json_controlled_node_init_one_shot"
 *   C symbol = registration name + "_fn"
 * ======================================================================== */

#ifndef CFL_JSON_NODE_CONTROL_H
#define CFL_JSON_NODE_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "cfl_runtime.h"
#include "cfl_json_packets.h"

/* ── JSON port: only event_id needed ── */
typedef struct {
    unsigned event_id;
} cfl_json_port_t;

/* ── Server (controlled/dead node) ── */
typedef struct {
    cfl_json_port_t           request_port;
    cfl_json_port_t           response_port;
    unsigned                  client_node_index;  /* set by client at activation */
    cfl_json_heap_interface_t heap_iface;
    char                     *response_text;      /* heap-allocated by user finalizer */
    void                     *aux_data;
} cfl_json_server_controlled_node_t;

/* ── Client (initiator node) ── */
typedef struct {
    cfl_json_port_t           request_port;
    cfl_json_port_t           response_port;
    unsigned                  server_node_index;
    cfl_json_heap_interface_t heap_iface;
    void                     *aux_data;
    bool                      node_is_active;
    char                     *request_text;       /* heap-allocated JSON request */
    unsigned                  request_text_size;
} cfl_json_client_controlled_node_t;

/* ========================================================================
 * SERVER FUNCTIONS
 * ======================================================================== */

void     cfl_json_controlled_node_init_one_shot_fn(void *handle, unsigned node_index);
unsigned cfl_json_controlled_node_main_main_fn(void *handle, unsigned bool_function_index,
             unsigned node_index, unsigned event_type, unsigned event_id, void *event_data);
void     cfl_json_controlled_node_term_one_shot_fn(void *handle, unsigned node_index);

/* ========================================================================
 * CLIENT FUNCTIONS
 * ======================================================================== */

void     cfl_json_client_controlled_node_init_one_shot_fn(void *handle, unsigned node_index);
unsigned cfl_json_client_controlled_node_main_main_fn(void *handle, unsigned bool_function_index,
             unsigned node_index, unsigned event_type, unsigned event_id, void *event_data);
void     cfl_json_client_controlled_node_term_one_shot_fn(void *handle, unsigned node_index);

/* ========================================================================
 * HELPER FUNCTIONS — used by user one-shot finalizers
 * ======================================================================== */

/* Walk up tree to find enclosing JSON controlled node (server) */
uint16_t cfl_json_server_get_server_node_index(cfl_runtime_handle_t *handle, unsigned node_index);

/* Get server struct from any child node within the server column */
cfl_json_server_controlled_node_t *cfl_json_server_get_node(
    cfl_runtime_handle_t *handle, unsigned node_index);

/* Set response text on the enclosing server node (heap-allocated copy) */
void cfl_json_server_set_response_text(cfl_runtime_handle_t *handle,
    unsigned node_index, const char *text);

/* Inline retrieval helpers */
static inline cfl_json_server_controlled_node_t *cfl_get_json_server_node(
        cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_json_server_controlled_node_t *)cfl_heap_arena_get_node_ptr(
        rt->arena_system, node_index);
}

static inline cfl_json_client_controlled_node_t *cfl_get_json_client_node(
        cfl_runtime_handle_t *rt, unsigned node_index) {
    return (cfl_json_client_controlled_node_t *)cfl_heap_arena_get_node_ptr(
        rt->arena_system, node_index);
}

#ifdef __cplusplus
}
#endif

#endif /* CFL_JSON_NODE_CONTROL_H */
