#ifndef CFL_COMMON_FUNCTIONS_H
#define CFL_COMMON_FUNCTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#include "cfl_engine.h"

typedef struct{
    uint8_t *stop_tests;
    uint8_t *start_tests;
    uint8_t stop_tests_length;
    uint8_t start_tests_length;
  
  }cfl_start_stop_tests_fn_data_t;
  



void cfl_uint16_to_str(uint16_t value, char* buffer);

bool cfl_allocate_state(cfl_runtime_handle_t *handle, uint16_t node_index);
void *cfl_additional_arena_alloc(cfl_runtime_handle_t *handle, uint16_t node_index, uint16_t size);
void *cfl_smart_arena_alloc(cfl_runtime_handle_t *handle, uint16_t node_index, uint16_t size);

void cfl_enable_all_nodes(cfl_runtime_handle_t *handle, uint16_t node_index);
unsigned cfl_verify_active_children(cfl_runtime_handle_t *handle, uint16_t node_index);
unsigned cfl_verify_active_children_a(cfl_runtime_handle_t *handle, uint16_t node_index);
void cfl_stop_start_tests(cfl_runtime_handle_t *handle,  cfl_start_stop_tests_fn_data_t *ptr);
void cfl_enable_all_children(cfl_runtime_handle_t *handle, uint16_t node_index);
void cfl_disable_all_children(cfl_runtime_handle_t *handle, uint16_t node_index);
void cfl_enable_child(cfl_runtime_handle_t *handle, uint16_t node_index, uint16_t child_node_index);
void cfl_disable_child(cfl_runtime_handle_t *handle, uint16_t node_index, uint16_t child_node_index);
bool cfl_child_is_enabled(cfl_runtime_handle_t *handle, uint16_t node_index, uint16_t child_node_index);
static inline void cfl_change_bitmask(cfl_runtime_handle_t *handle, uint64_t bitmask){
    
    handle->shaddow_bitmask = bitmask;
}

static inline uint64_t cfl_get_bitmask(cfl_runtime_handle_t *handle){
    return handle->bitmask;
}
#ifdef __cplusplus
}
#endif
#endif

