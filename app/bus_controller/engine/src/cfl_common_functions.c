#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "cfl_engine.h"

#include "cfl_heap_arena_allocate.h"
#include "cfl_common_functions.h"


void cfl_uint16_to_str(uint16_t value, char* buffer) {
    char temp[6];  // Max 5 digits + null
    int i = 0;
    
    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    
    while (value > 0) {
        temp[i++] = '0' + (value % 10);
        value /= 10;
    }
    
    // Reverse into output buffer
    int j;
    for (j = 0; j < i; j++) {
        buffer[j] = temp[i - 1 - j];
    }
    buffer[j] = '\0';
}

bool cfl_allocate_state(cfl_runtime_handle_t *handle, uint16_t node_index){
    uint16_t memory_index = cfl_heap_arena_get_node_memory_index(( cfl_heap_arena_system_t *)handle->arena_system, node_index);
    if (memory_index == 0xFFFF){
        return false;
    }
    return true;
}

void *cfl_additional_arena_alloc(cfl_runtime_handle_t *handle, uint16_t node_index, uint16_t size){
    // Use cfl_arena_additional_alloc, NOT cfl_arena_alloc_from_active
    void *ptr = cfl_arena_additional_alloc((CflHeapArenaSystem*)handle->arena_system, node_index, size);
    if (!ptr){
        EXCEPTION("cfl_additional_arena_alloc: Failed to allocate memory");
        return NULL;
    }
    return ptr;
}

void *cfl_smart_arena_alloc(cfl_runtime_handle_t *handle, uint16_t node_index, uint16_t size){
    
    uint16_t memory_index = cfl_heap_arena_get_node_memory_index(( cfl_heap_arena_system_t *)handle->arena_system, node_index);
    if (memory_index == 0xFFFF){
        return  cfl_arena_system_alloc(handle->arena_system, node_index, size);
    }
    return cfl_heap_arena_get_node_ptr(handle->arena_system, node_index);
    
}




void cfl_enable_all_nodes(cfl_runtime_handle_t *handle, uint16_t node_index){

    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    /* Validate node_index */
    if (node_index >= runtime_handle->flash_handle->node_count) {
        EXCEPTION("cfl_enable_all_nodes: node_index out of bounds");
        
    }
    
    const chaintree_node_t *node = &runtime_handle->flash_handle->nodes[node_index];
    uint16_t link_start = node->link_start;
    uint16_t link_count = (node->link_count & LINK_COUNT_MASK);
    
    /* Validate link_start and link_count */
    if (link_count > 0) {
        if (link_start >= runtime_handle->flash_handle->link_table_size) {
            EXCEPTION("cfl_enable_all_nodes: link_start out of bounds");
            
        }
        if (link_start + link_count > runtime_handle->flash_handle->link_table_size) {
            EXCEPTION("cfl_enable_all_nodes: link range exceeds table size");
            
        }
    }
    
    const uint16_t *link_table = runtime_handle->flash_handle->link_table;
    for (unsigned i = 0; i < link_count; i++) {
        unsigned int link_id = link_table[link_start + i];
        
        /* Validate link_id */
        if (link_id >= runtime_handle->flash_handle->node_count) {
            EXCEPTION("cfl_enable_all_nodes: link_id out of bounds");
            
        }
        
        cfl_enable_node(runtime_handle, link_id);
    }
   
}

unsigned cfl_verify_active_children(cfl_runtime_handle_t *handle, uint16_t node_index)
{
    const chaintree_node_t *node = &handle->flash_handle->nodes[node_index];
    uint16_t link_start = node->link_start;
    uint16_t link_count = (node->link_count & LINK_COUNT_MASK);
    
    const uint16_t *link_table = handle->flash_handle->link_table;
    for (uint16_t i = 0; i < link_count; i++) {
        uint16_t link_id = link_table[link_start + i];
        if (cfl_engine_node_is_enabled(handle, link_id) == true) {
            return CFL_CONTINUE;
        }
    }
    return CFL_DISABLE;
}

unsigned cfl_verify_active_children_a(cfl_runtime_handle_t *handle, uint16_t node_index)
{
    const chaintree_node_t *node = &handle->flash_handle->nodes[node_index];
    uint16_t link_start = node->link_start;
    uint16_t link_count = (node->link_count & LINK_COUNT_MASK);
    
    const uint16_t *link_table = handle->flash_handle->link_table;
    for (uint16_t i = 0; i < link_count; i++) {
        printf("verifying active children %d\n", link_table[link_start + i]);
        uint16_t link_id = link_table[link_start + i];
        if (cfl_engine_node_is_enabled(handle, link_id) == true) {
            printf("active child found %d\n", link_id);
            return CFL_CONTINUE;
        }
    }
    return CFL_DISABLE;
}
void cfl_stop_start_tests(cfl_runtime_handle_t *handle, cfl_start_stop_tests_fn_data_t *ptr){
   
    cfl_send_data_event(
        handle->event_queue,
        CFL_EVENT_PRIORITY_LOW,
        CFL_EVENT_BROADCAST_NODE,
        false,
        CFL_STOP_START_TESTS_EVENT,
        ptr);
}

void cfl_enable_all_children(cfl_runtime_handle_t *handle, uint16_t node_index){
    cfl_enable_all_nodes(handle, node_index);
}

void cfl_disable_all_children(cfl_runtime_handle_t *handle, uint16_t node_index)
{
    const chaintree_node_t *node = &handle->flash_handle->nodes[node_index];
    uint16_t link_start = node->link_start;
    uint16_t link_count = (node->link_count & LINK_COUNT_MASK);
    
    const uint16_t *link_table = handle->flash_handle->link_table;
    for (uint16_t i = 0; i < link_count; i++) {
        uint16_t link_id = link_table[link_start + i];
    
        if (cfl_engine_node_is_enabled(handle, link_id) == true) {
            
            cfl_terminate_node_tree(handle, link_id);
        }
    }
    
}

void cfl_enable_child(cfl_runtime_handle_t *handle, uint16_t node_index, uint16_t child_node_index)
{
    const chaintree_node_t *node = &handle->flash_handle->nodes[node_index];
    uint16_t link_start = node->link_start;
    uint16_t link_count = (node->link_count & LINK_COUNT_MASK);
    
    const uint16_t *link_table = handle->flash_handle->link_table;

    if(child_node_index >= link_count){
        EXCEPTION("cfl_enable_child: node_index out of bounds");
        
    }
    
    uint16_t link_id = link_table[link_start + child_node_index];
    
    cfl_enable_node(handle, link_id);
    
}

void cfl_disable_child(cfl_runtime_handle_t *handle, uint16_t node_index, uint16_t child_node_index)
{
    const chaintree_node_t *node = &handle->flash_handle->nodes[node_index];
    uint16_t link_start = node->link_start;
    uint16_t link_count = (node->link_count & LINK_COUNT_MASK);
    
    const uint16_t *link_table = handle->flash_handle->link_table;

    if(child_node_index >= link_count){
        EXCEPTION("cfl_enable_child: node_index out of bounds");
        
    }
    
    uint16_t link_id = link_table[link_start + child_node_index];
    
    cfl_enable_node(handle, link_id);
    
}


bool cfl_child_is_enabled(cfl_runtime_handle_t *handle, uint16_t node_index, uint16_t child_node_index)
{
    const chaintree_node_t *node = &handle->flash_handle->nodes[node_index];
    uint16_t link_start = node->link_start;
    uint16_t link_count = (node->link_count & LINK_COUNT_MASK);
    
    const uint16_t *link_table = handle->flash_handle->link_table;

    if(child_node_index >= link_count){
        EXCEPTION("cfl_enable_child: node_index out of bounds");
        
    }
    
    uint16_t link_id = link_table[link_start + child_node_index];
    
    return cfl_engine_node_is_enabled(handle, link_id);
    
}
