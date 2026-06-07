#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "cfl_runtime.h"
#include "cfl_exception.h"
#include "cfl_engine.h"
#include "cfl_common_functions.h"
#include "cfl_common_function_headers.h"
#include "json_node_decoder.h"
#include "cfl_sm_functions.h"


/* Extract node_id, new_state, and sync_event_id (which can be null or integer) */
static void json_extract_new_state_data(
    cfl_runtime_handle_t *runtime,
    uint16_t node_index,
    int32_t *out_node_id,
    const char **out_new_state,
    bool *out_sync_flag,
    int32_t *out_sync_event_id)
{
    if (!runtime || !out_node_id || !out_new_state || !out_sync_flag || !out_sync_event_id) {
        EXCEPTION("json_extract_node_transition_data: NULL parameter");
    }
    
    json_decoder_init_from_runtime(runtime, node_index);
    
    const  json_decoder_ctx_t *ctx = runtime->json_decoder_ctx;
    const record_control_t *region = &ctx->controls[ctx->current_control_idx];
    uint32_t root_record = region->start_position;
    
    // Navigate to node_dict
    uint32_t node_dict_record;
    json_find_object_child(ctx, root_record, "node_dict", &node_dict_record);
    
    
    // Extract node_id
    json_extract_int32(ctx, node_dict_record, "node_id", out_node_id);
    
    // Extract new_state (pointer to string table)
    json_extract_string(ctx, node_dict_record, "new_state", out_new_state);
   
    // Extract sync_event_id - check if it's null or integer
    uint32_t sync_event_id_record;
    json_find_object_child(ctx, node_dict_record, "sync_event_id", &sync_event_id_record);
   
    if (json_is_null(ctx, sync_event_id_record)) {
        *out_sync_flag = false;
        *out_sync_event_id = 0;  // Default value when null
        
    } else {
        *out_sync_flag = true;
        json_get_int32(ctx, sync_event_id_record, out_sync_event_id);
        
    }
}

/*******************   public api functions ******************************************************************************** */


void cfl_terminate_state_machine(cfl_runtime_handle_t *handle, uint16_t node_index, int32_t sm_node_id){
    (void)node_index;
    
    /* Validate sm_node_id */
    if (sm_node_id < 0 || (unsigned)sm_node_id >= handle->flash_handle->node_count) {
        EXCEPTION("cfl_terminate_state_machine: sm_node_id out of bounds");
        return;
    }
    
    const chaintree_node_t *node = &handle->flash_handle->nodes[sm_node_id];
   //CFL_FUNCTION_ID_STATE_MACHINE
    if (node->main_function_index != handle->main_function_data->main_function_ids[CFL_FUNCTION_ID_STATE_MACHINE] ){
        EXCEPTION("cfl_change_state: Node is not a state machine");
        return;
    }
   
    
    cfl_terminate_node_tree(handle, sm_node_id);

}

void cfl_reset_state_machine(cfl_runtime_handle_t *handle, uint16_t node_index, int32_t sm_node_id){
    (void)node_index;
    
    /* Validate sm_node_id */
    if (sm_node_id < 0 || (unsigned)sm_node_id >= handle->flash_handle->node_count) {
        EXCEPTION("cfl_terminate_state_machine: sm_node_id out of bounds");
        return;
    }
    
    const chaintree_node_t *node = &handle->flash_handle->nodes[sm_node_id];
   //CFL_FUNCTION_ID_STATE_MACHINE
    if (node->main_function_index != handle->main_function_data->main_function_ids[CFL_FUNCTION_ID_STATE_MACHINE] ){
        EXCEPTION("cfl_change_state: Node is not a state machine");
        return;
    }
   
    
    cfl_terminate_node_tree(handle, sm_node_id);
    cfl_enable_node(handle, sm_node_id);
}

void cfl_reset_state_machine_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    json_decoder_init_from_runtime(runtime_handle, node_index);
    
    int32_t sm_node_id;
    json_extract_int32_runtime(runtime_handle, "node_dict.sm_node_id", &sm_node_id);

    cfl_reset_state_machine(runtime_handle, node_index, sm_node_id);
}

void cfl_change_state(cfl_runtime_handle_t *handle, uint16_t node_index, int32_t sm_node_id, const char *new_state, bool sync_flag, int32_t sync_event_id){
    (void)node_index;
    
    /* Validate sm_node_id */
    if (sm_node_id < 0 || (unsigned)sm_node_id >= handle->flash_handle->node_count) {
        EXCEPTION("cfl_change_state: sm_node_id out of bounds");
        return;
    }
    
    const chaintree_node_t *node = &handle->flash_handle->nodes[sm_node_id];
    
    if (node->main_function_index != handle->main_function_data->main_function_ids[CFL_FUNCTION_ID_STATE_MACHINE]){
        EXCEPTION("cfl_change_state: Node is not a state machine");
        return;
    }
    
    cfl_state_machine_column_data_t *ptr = (cfl_state_machine_column_data_t *)cfl_heap_arena_get_node_ptr(handle->arena_system, sm_node_id);
    if (!ptr) {
        EXCEPTION("cfl_change_state: failed to get node pointer");
        return;
    }
    
    if (!ptr->state_names) {
        EXCEPTION("cfl_change_state: state_names is NULL");
        return;
    }
    
    bool state_found = false;
    uint16_t state_count = node->link_count & LINK_COUNT_MASK;
    
    for(uint16_t i = 0; i < state_count; i++){
        if (strcmp(ptr->state_names[i], new_state) == 0){
            ptr->new_state = i;
            state_found = true;
            break;
        }
    }
    
    if (!state_found){
        EXCEPTION("cfl_change_state: State not found");
        return;
    }
    
    /* Validate new_state is within range */
    if (ptr->new_state < 0 || ptr->new_state >= (int32_t)state_count) {
        EXCEPTION("cfl_change_state: new_state out of range");
        return;
    }
    
    if (sync_flag){
        ptr->sync_event_id_valid = true;
        ptr->sync_event_id = sync_event_id;
        cfl_send_null_event(
            handle->event_queue,
            CFL_EVENT_PRIORITY_LOW,
            sm_node_id,
            sync_event_id);
       
    }
    else{
        ptr->sync_event_id_valid = false;
        ptr->sync_event_id = 0;
    }
}

/*******************   one shot functions ******************************************************************************** */
void cfl_change_state_one_shot_fn(void *handle, uint16_t node_index){
    
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    int32_t sm_node_id;
    const char *new_state;
    bool sync_flag;
    int32_t sync_event_id;
    
    json_decoder_init_from_runtime(runtime_handle, node_index);
    
    json_extract_new_state_data(runtime_handle, node_index, &sm_node_id, &new_state, &sync_flag, &sync_event_id);
    
    cfl_change_state(runtime_handle, node_index, sm_node_id, new_state, sync_flag, sync_event_id);
}







void cfl_terminate_state_machine_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    json_decoder_init_from_runtime(runtime_handle, node_index);
    
    int32_t sm_node_id;
    json_extract_int32_runtime(runtime_handle, "node_dict.sm_node_id", &sm_node_id);

    cfl_terminate_state_machine(runtime_handle, node_index, sm_node_id);


}


/****************************************************   state machine functions ******************************************************************************** */

unsigned cfl_state_machine_main_main_fn(void *handle, unsigned bool_function_index, unsigned node_index, unsigned event_type, unsigned event_id, void *event_data){
    
    (void)node_index;
    (void)event_type;
    (void)event_id;
    (void)event_data;
    unsigned return_value = CFL_CONTINUE;
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    /* Validate node_index */
    if (node_index >= runtime_handle->flash_handle->node_count) {
        EXCEPTION("cfl_state_machine_main_main_fn: node_index out of bounds");
        return CFL_TERMINATE_SYSTEM;
    }
    
    cfl_state_machine_column_data_t *ptr = (cfl_state_machine_column_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    if (!ptr) {
        EXCEPTION("cfl_state_machine_main_main_fn: failed to get node pointer");
        return CFL_TERMINATE_SYSTEM;
    }
    
    const chaintree_node_t *node = &runtime_handle->flash_handle->nodes[node_index];
    uint32_t node_count = node->link_count & LINK_COUNT_MASK;
    uint32_t link_start = node->link_start;
    
    /* Validate link_start and node_count */
    if (node_count > 0) {
        if (link_start >= runtime_handle->flash_handle->link_table_size) {
            EXCEPTION("cfl_state_machine_main_main_fn: link_start out of bounds");
            return CFL_TERMINATE_SYSTEM;
        }
        if (link_start + node_count > runtime_handle->flash_handle->link_table_size) {
            EXCEPTION("cfl_state_machine_main_main_fn: link range exceeds table size");
            return CFL_TERMINATE_SYSTEM;
        }
    }
    
    const uint16_t *link_table = runtime_handle->flash_handle->link_table;
    
    if (ptr->current_state != ptr->new_state) {
        /* Validate state indices */
        if (ptr->current_state < 0 || ptr->current_state >= (int32_t)node_count) {
            EXCEPTION("cfl_state_machine_main_main_fn: current_state out of range");
            return CFL_TERMINATE_SYSTEM;
        }
        if (ptr->new_state < 0 || ptr->new_state >= (int32_t)node_count) {
            EXCEPTION("cfl_state_machine_main_main_fn: new_state out of range");
            return CFL_TERMINATE_SYSTEM;
        }

        /* Validate link_id for current_state */
        uint16_t current_link_id = link_table[link_start + ptr->current_state];
        if (current_link_id >= runtime_handle->flash_handle->node_count) {
            EXCEPTION("cfl_state_machine_main_main_fn: current_state link_id out of bounds");
            return CFL_TERMINATE_SYSTEM;
        }

        /* Validate link_id for new_state */
        uint16_t new_link_id = link_table[link_start + ptr->new_state];
        if (new_link_id >= runtime_handle->flash_handle->node_count) {
            EXCEPTION("cfl_state_machine_main_main_fn: new_state link_id out of bounds");
            return CFL_TERMINATE_SYSTEM;
        }

        /* BUG FIX: Only terminate current_state ONCE, not in a loop */
        cfl_terminate_node_tree(runtime_handle, current_link_id);
        cfl_enable_node(runtime_handle, new_link_id);
        ptr->current_state = ptr->new_state;
    }

    boolean_function_t boolean_function = runtime_handle->flash_handle->boolean_functions[bool_function_index];
    bool result = boolean_function(runtime_handle, node_index, event_type, event_id, event_data);
    if (result == true) {
        return_value = CFL_SKIP_CONTINUE;
    }

    for (unsigned i = 0; i < node_count; i++) {
        unsigned int link_id = link_table[link_start + i];

        /* Validate link_id */
        if (link_id >= runtime_handle->flash_handle->node_count) {
            EXCEPTION("cfl_state_machine_main_main_fn: link_id out of bounds");
            return CFL_TERMINATE_SYSTEM;
        }

        if (cfl_engine_node_is_enabled(runtime_handle, link_id) == true) {

            return return_value;
        }
    }

    return CFL_DISABLE;
}

void cfl_state_machine_init_one_shot_fn(void *handle, uint16_t node_index){
    uint32_t node_count;
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    /* Validate node_index */
    if (node_index >= runtime_handle->flash_handle->node_count) {
        EXCEPTION("cfl_state_machine_init_one_shot_fn: node_index out of bounds");
        return;
    }
    
    const chaintree_node_t *node = &runtime_handle->flash_handle->nodes[node_index];
    node_count = node->link_count & LINK_COUNT_MASK;
    
    /* Validate link_start and node_count */
    if (node_count > 0) {
        if (node->link_start >= runtime_handle->flash_handle->link_table_size) {
            EXCEPTION("cfl_state_machine_init_one_shot_fn: link_start out of bounds");
            return;
        }
        if (node->link_start + node_count > runtime_handle->flash_handle->link_table_size) {
            EXCEPTION("cfl_state_machine_init_one_shot_fn: link range exceeds table size");
            return;
        }
    }
    bool allocator_state = cfl_allocate_state(runtime_handle, node_index);
    cfl_state_machine_column_data_t *ptr = cfl_smart_arena_alloc(runtime_handle, node_index, sizeof(cfl_state_machine_column_data_t));
    
    ptr->sync_event_id_valid = false;
    ptr->sync_event_id = 0;
    json_decoder_init_from_runtime(runtime_handle, node_index);
    
    
    const  json_decoder_ctx_t *ctx = runtime_handle->json_decoder_ctx;
    const record_control_t *region = &ctx->controls[ctx->current_control_idx];
    uint32_t root_record = region->start_position;
    
    // Extract initial_state_number
    json_extract_int32_runtime(runtime_handle, "node_dict.column_data.initial_state_number", &ptr->current_state);
    ptr->new_state = ptr->current_state;
    
    /* Validate initial_state_number */
    if (ptr->current_state < 0 || (unsigned)ptr->current_state >= node_count) {
        EXCEPTION("cfl_state_machine_init_one_shot_fn: initial_state_number out of range");
        return;
    }
    
    /* Check for overflow in state_names allocation */
    size_t alloc_size = node_count * sizeof(const char *);
    if (alloc_size > 65535) {
        EXCEPTION("cfl_state_machine_init_one_shot_fn: state_names allocation size exceeds uint16_t limit");
        return;
    }
    if(allocator_state == false){
        ptr->state_names = (const char **)cfl_additional_arena_alloc(runtime_handle, node_index, (uint16_t)alloc_size);
        if(ptr->state_names == NULL){
            EXCEPTION("cfl_state_machine_init_one_shot_fn: failed to allocate state_names");
            return;
        }
    }
    

    
    // Navigate to node_dict.column_data
    uint32_t node_dict_record;
    json_find_object_child(ctx, root_record, "node_dict", &node_dict_record);
    
    uint32_t column_data_record;
    json_find_object_child(ctx, node_dict_record, "column_data", &column_data_record);
    
    // Navigate to state_names array
    uint32_t state_names_array;
    json_find_object_child(ctx, column_data_record, "state_names", &state_names_array);
    
    // Extract each state name pointer from the string table
    for (uint32_t i = 0; i < node_count; i++) {
        uint32_t element_record;
        json_get_array_child(ctx, state_names_array, i, &element_record);
        json_get_string_value(ctx, element_record, &ptr->state_names[i]);
    }
    
    const uint16_t *link_table = runtime_handle->flash_handle->link_table;
    
    // Terminate all state nodes first
    for (uint32_t i = 0; i < node_count; i++) {
        uint16_t link_id = link_table[node->link_start + i];
        
        /* Validate link_id */
        if (link_id >= runtime_handle->flash_handle->node_count) {
            EXCEPTION("cfl_state_machine_init_one_shot_fn: link_id out of bounds");
            continue;
        }
        
        cfl_terminate_node_tree(runtime_handle, link_id);
    }
    
    /* Validate link_id for initial state before enabling */
    uint16_t initial_link_id = link_table[node->link_start + ptr->current_state];
    if (initial_link_id >= runtime_handle->flash_handle->node_count) {
        EXCEPTION("cfl_state_machine_init_one_shot_fn: initial state link_id out of bounds");
        return;
    }
    
    cfl_enable_node(runtime_handle, initial_link_id);
}

void cfl_state_machine_term_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    /* Validate node_index */
    if (node_index >= runtime_handle->flash_handle->node_count) {
        EXCEPTION("cfl_state_machine_term_one_shot_fn: node_index out of bounds");
        return;
    }
    
    
    
    const chaintree_node_t *node = &runtime_handle->flash_handle->nodes[node_index];
    uint16_t link_count = node->link_count & LINK_COUNT_MASK;
    uint16_t link_start = node->link_start;
    
    /* Validate link_start and link_count */
    if (link_count > 0) {
        if (link_start >= runtime_handle->flash_handle->link_table_size) {
            EXCEPTION("cfl_state_machine_term_one_shot_fn: link_start out of bounds");
            return;
        }
        if (link_start + link_count > runtime_handle->flash_handle->link_table_size) {
            EXCEPTION("cfl_state_machine_term_one_shot_fn: link range exceeds table size");
            return;
        }
    }
    
    const uint16_t *link_table = runtime_handle->flash_handle->link_table;
    
    /* BUG FIX: Terminate link_table[link_start + i], not link_start + i */
    for (uint32_t i = 0; i < link_count; i++) {
        uint16_t link_id = link_table[link_start + i];
        
        /* Validate link_id */
        if (link_id >= runtime_handle->flash_handle->node_count) {
            EXCEPTION("cfl_state_machine_term_one_shot_fn: link_id out of bounds");
            continue;
        }
        
        cfl_terminate_node_tree(runtime_handle, link_id);
    }    
}

