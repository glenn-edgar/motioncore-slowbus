#ifndef CFL_NODE_CONTROL_SUPPORT_H
#define CFL_NODE_CONTROL_SUPPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cfl_runtime.h"
#include "cfl_common_function_headers.h"
#include "cfl_common_functions.h"
#include "cfl_engine.h"
#include "cfl_node_control_support.h"
#include "json_node_decoder.h"
#include "avro_common.h"



// Client controlled node data
typedef struct {
    cfl_port_t request_port;
    cfl_port_t response_port;
    unsigned server_node_index;
    void *aux_data;  // Application-specific data
    bool node_is_active;
} cfl_client_controlled_node_t;



// Client controlled node data
typedef struct {
    cfl_port_t request_port;
    cfl_port_t response_port;
    unsigned client_node_index;
    void *aux_data;  // Application-specific data
    
} cfl_server_controlled_node_t;



uint16_t cfl_server_controlled_node_get_server_node_index(cfl_runtime_handle_t *handle, unsigned node_index);


cfl_port_t *cfl_server_controlled_node_get_request_port(cfl_runtime_handle_t *handle, unsigned node_index);
 
cfl_port_t *cfl_server_controlled_node_get_response_port(cfl_runtime_handle_t *handle, unsigned node_index);
  


#ifdef __cplusplus
}
#endif

#endif

