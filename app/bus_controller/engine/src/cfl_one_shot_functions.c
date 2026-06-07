#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cfl_runtime.h"
#include "cfl_engine.h"
#include "json_node_decoder.h"
#include "cfl_common_functions.h"
#include "cfl_common_function_headers.h"
#include "cfl_supervisor_support.h"


static void cfl_enable_auto_start_nodes(cfl_runtime_handle_t *handle, uint16_t node_index){

    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    /* Validate node_index */
    if (node_index >= runtime_handle->flash_handle->node_count) {
        EXCEPTION("cfl_enable_auto_start_nodes: node_index out of bounds");
        return;
    }
    
    const chaintree_node_t *node = &runtime_handle->flash_handle->nodes[node_index];
    uint16_t link_start = node->link_start;
    uint16_t link_count = (node->link_count & LINK_COUNT_MASK);
    
    /* Validate link_start and link_count */
    if (link_count > 0) {
        if (link_start >= runtime_handle->flash_handle->link_table_size) {
            EXCEPTION("cfl_enable_auto_start_nodes: link_start out of bounds");
            return;
        }
        if (link_start + link_count > runtime_handle->flash_handle->link_table_size) {
            EXCEPTION("cfl_enable_auto_start_nodes: link range exceeds table size");
            return;
        }
    }
    
    const uint16_t *link_table = runtime_handle->flash_handle->link_table;
    for (unsigned i = 0; i < link_count; i++) {
        unsigned int link_id = link_table[link_start + i];
        
        /* Validate link_id */
        if (link_id >= runtime_handle->flash_handle->node_count) {
            EXCEPTION("cfl_enable_auto_start_nodes: link_id out of bounds");
            return;
        }
        
        const chaintree_node_t *link_node = &runtime_handle->flash_handle->nodes[link_id];
        if ((link_node->link_count & AUTO_START_BIT) != 0) {
            cfl_enable_node(runtime_handle, link_id);
        }
    }
   
}



void cfl_null_one_shot_fn(void *handle, uint16_t node_index){
    (void)handle;
    (void)node_index;
}

void cfl_column_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_enable_all_nodes(handle, node_index);
}

void cfl_column_term_one_shot_fn(void *handle, uint16_t node_index){
    (void)handle;
    (void)node_index;
}


void cfl_gate_node_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_enable_auto_start_nodes(handle, node_index);
}

void cfl_gate_node_term_one_shot_fn(void *handle, uint16_t node_index){
    (void)handle;
    (void)node_index;
}

void cfl_log_message_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime = (cfl_runtime_handle_t *)handle;
    const char *message;
    double timestamp;

    timestamp = cfl_timer_get_timestamp(runtime->timer_handle);
    
    json_decoder_init_from_runtime(runtime, node_index);
    json_extract_string_runtime(runtime, "node_dict.message", &message);
    
    printf("Timestamp: %f, Node Index: %d, Message: %s\n", timestamp, node_index, message);
}

void cfl_send_named_event_one_shot_fn(void *handle, uint16_t node_index){
    int32_t event_id;
    int32_t event_node_index;
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    json_decoder_init_from_runtime(runtime_handle, node_index);
    json_extract_int32_runtime(runtime_handle, "node_dict.event_id", &event_id);
    json_extract_int32_runtime(runtime_handle, "node_dict.node_id", &event_node_index);
    cfl_send_node_id_event(runtime_handle->event_queue, CFL_EVENT_PRIORITY_LOW, (unsigned)event_node_index, (unsigned)event_id, (unsigned)node_index);
}


void cfl_verify_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    bool already_allocated = cfl_allocate_state(runtime_handle, node_index);
    if (already_allocated) {
        return;
    }
    
    cfl_verify_fn_data_t *ptr = (cfl_verify_fn_data_t *)cfl_smart_arena_alloc(runtime_handle, node_index, sizeof(cfl_verify_fn_data_t));
    json_decoder_init_from_runtime(runtime_handle, node_index);
    json_extract_bool_runtime(runtime_handle, "node_dict.reset_flag", &ptr->reset_flag);
    json_extract_int32_runtime(runtime_handle, "node_dict.error_function_id", (int32_t*)&ptr->error_function);
    
    ptr->auxiliary_data = NULL;
}


void cfl_verify_term_one_shot_fn(void *handle, uint16_t node_index){
    (void)handle;
    (void)node_index;
}

void cfl_wait_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    bool already_allocated = cfl_allocate_state(runtime_handle, node_index);
    if (already_allocated) {
        return;
    }
    
    cfl_wait_fn_data_t *ptr = (cfl_wait_fn_data_t *)cfl_smart_arena_alloc(runtime_handle, node_index,
         sizeof(cfl_wait_fn_data_t));
    
    json_decoder_init_from_runtime(runtime_handle, node_index);
    
    json_extract_bool_runtime(runtime_handle, "node_dict.reset_flag", &ptr->reset_flag);
    json_extract_int32_runtime(runtime_handle, "node_dict.timeout", (int32_t*)&ptr->timeout);
    json_extract_int32_runtime(runtime_handle, "node_dict.time_out_event", (int32_t*)&ptr->time_out_event);
    json_extract_int32_runtime(runtime_handle, "node_dict.error_function_id", (int32_t*)&ptr->error_function);
    ptr->event_count = 0;
    ptr->auxiliary_data = NULL;
}


void cfl_wait_term_one_shot_fn(void *handle, uint16_t node_index){
    (void)handle;
    (void)node_index;
}

void cfl_wait_time_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    bool already_allocated = cfl_allocate_state(runtime_handle, node_index);
    
    cfl_wait_time_out_data_t *ptr;
    if (already_allocated) {
        ptr = (cfl_wait_time_out_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    } else {
        ptr = (cfl_wait_time_out_data_t *)cfl_smart_arena_alloc(runtime_handle, node_index, sizeof(cfl_wait_time_out_data_t));
    }
    
    float time_delay;
    json_decoder_init_from_runtime(runtime_handle, node_index);
    json_extract_float32_runtime(runtime_handle, "node_dict.time_delay", &time_delay);
    ptr->wait_time_out = (double)time_delay + cfl_timer_get_timestamp(runtime_handle->timer_handle);
}

void cfl_disable_nodes_one_shot_fn(void *handle, uint16_t node_index){
    uint32_t count;
    int32_t node_id;
    uint32_t array_nodes_record;

    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    json_decoder_init_from_runtime(runtime_handle, node_index);
    const record_control_t *region = &runtime_handle->json_decoder_ctx->controls[runtime_handle->json_decoder_ctx->current_control_idx];
    
    json_navigate_path(runtime_handle->json_decoder_ctx, region->start_position, "node_dict.nodes", &array_nodes_record);
    json_get_child_count(runtime_handle->json_decoder_ctx, array_nodes_record, &count);
    
    for (uint32_t i = 0; i < count; i++) {
        uint32_t element_record;
        json_get_array_child(runtime_handle->json_decoder_ctx, array_nodes_record, i, &element_record);
        
        json_get_int32(runtime_handle->json_decoder_ctx, element_record, &node_id);
        
        if (node_id < 0 || (unsigned)node_id >= runtime_handle->flash_handle->node_count) {
            EXCEPTION("cfl_disable_nodes_one_shot_fn: node_id out of bounds");
            continue;
        }
        
        cfl_terminate_node_tree(runtime_handle, (unsigned)node_id);
    }
}

void cfl_enable_nodes_one_shot_fn(void *handle, uint16_t node_index){
    uint32_t count;
    int32_t node_id;
    uint32_t array_nodes_record;

    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    json_decoder_init_from_runtime(runtime_handle, node_index);
    const record_control_t *region = &runtime_handle->json_decoder_ctx->controls[runtime_handle->json_decoder_ctx->current_control_idx];
    
    json_navigate_path(runtime_handle->json_decoder_ctx, region->start_position, "node_dict.nodes", &array_nodes_record);
    json_get_child_count(runtime_handle->json_decoder_ctx, array_nodes_record, &count);
    
    for (uint32_t i = 0; i < count; i++) {
        uint32_t element_record;
        json_get_array_child(runtime_handle->json_decoder_ctx, array_nodes_record, i, &element_record);
        
        json_get_int32(runtime_handle->json_decoder_ctx, element_record, &node_id);
        
        if (node_id < 0 || (unsigned)node_id >= runtime_handle->flash_handle->node_count) {
            EXCEPTION("cfl_enable_nodes_one_shot_fn: node_id out of bounds");
            continue;
        }
        
        cfl_enable_node(runtime_handle, (unsigned)node_id);
    }
}


void cfl_event_logger_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    bool already_allocated = cfl_allocate_state(runtime_handle, node_index);
    
    cfl_event_logger_fn_data_t *ptr;
    if (already_allocated) {
        ptr = (cfl_event_logger_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    } else {
        ptr = (cfl_event_logger_fn_data_t *)cfl_smart_arena_alloc(runtime_handle, node_index, sizeof(cfl_event_logger_fn_data_t));
    }
    
    json_decoder_init_from_runtime(runtime_handle, node_index);
    
    const json_decoder_ctx_t *ctx = runtime_handle->json_decoder_ctx;
    const record_control_t *region = &ctx->controls[ctx->current_control_idx];
    uint32_t root_record = region->start_position;
    
    json_extract_string(ctx, root_record, "node_dict.message", (const char**)&ptr->event_logger_message);
    
    uint32_t node_dict_record;
    json_find_object_child(ctx, root_record, "node_dict", &node_dict_record);
    
    uint32_t events_array_record;
    json_find_object_child(ctx, node_dict_record, "events", &events_array_record);
    
    json_get_child_count(ctx, events_array_record, &ptr->event_count);
    
    size_t alloc_size = ptr->event_count * sizeof(int32_t);
    if (alloc_size > 65535) {
        EXCEPTION("cfl_event_logger_init_one_shot_fn: event_ids allocation size exceeds uint16_t limit");
        ptr->event_count = 0;
        ptr->event_ids = NULL;
        return;
    }
    
    if (!already_allocated) {
        ptr->event_ids = (int32_t *)cfl_additional_arena_alloc(runtime_handle, node_index, (uint16_t)alloc_size);
        if (!ptr->event_ids) {
            EXCEPTION("cfl_event_logger_init_one_shot_fn: failed to allocate event_ids");
            return;
        }
    }
    
    for (uint32_t i = 0; i < ptr->event_count; i++) {
        uint32_t element_record;
        json_get_array_child(ctx, events_array_record, i, &element_record);
        json_get_int32(ctx, element_record, &ptr->event_ids[i]);
    }
}


void cfl_event_logger_term_one_shot_fn(void *handle, uint16_t node_index){
    (void)handle;
    (void)node_index;
}


void cfl_fork_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    cfl_enable_all_nodes(runtime_handle, node_index);
}

void cfl_fork_term_one_shot_fn(void *handle, uint16_t node_index){
    (void)handle;
    (void)node_index;
}


void cfl_sequence_pass_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    bool already_allocated = cfl_allocate_state(runtime_handle, node_index);
    
    sequence_start_fn_data_t *ptr;
    if (already_allocated) {
        ptr = (sequence_start_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    } else {
        ptr = (sequence_start_fn_data_t *)cfl_smart_arena_alloc(runtime_handle, node_index, sizeof(sequence_start_fn_data_t));
    }

    const chaintree_node_t *node = &runtime_handle->flash_handle->nodes[node_index];
    const uint16_t *link_table = runtime_handle->flash_handle->link_table;
    uint16_t link_count = node->link_count & LINK_COUNT_MASK;
    ptr->sequence_number = link_count;
    ptr->current_sequence_index = 0;
    ptr->recorded_sequence_index = -1;
    ptr->sequence_type = 0;
    
    if (!already_allocated) {
        ptr->sequence_result_data_array = (sequence_result_data_t *)cfl_additional_arena_alloc(runtime_handle, node_index,
            (uint16_t)(sizeof(sequence_result_data_t) * ptr->sequence_number));
        if (ptr->sequence_result_data_array == NULL) {
            EXCEPTION("cfl_sequence_pass_init_one_shot_fn: failed to allocate sequence_result_data_array");
            return;
        }
    }
    
    for (int32_t i = 0; i < link_count; i++) {
        uint16_t link_id = link_table[node->link_start + i];
        if (link_id >= runtime_handle->flash_handle->node_count) {
            EXCEPTION("cfl_sequence_pass_init_one_shot_fn: link_id out of bounds");
            continue;
        }
        
        ptr->sequence_result_data_array[i].sequence_result = false;
        ptr->sequence_result_data_array[i].node_index = link_id;
        
        if (i == 0) {
            cfl_enable_node(runtime_handle, link_id);
        } else {
            cfl_terminate_node_tree(runtime_handle, link_id);
        }
    }
    
    json_decoder_init_from_runtime(runtime_handle, node_index);
    json_extract_int32_runtime(runtime_handle, "node_dict.column_data.finalize_function_id", &ptr->finalize_function_id);
}

void cfl_sequence_pass_term_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    sequence_start_fn_data_t *ptr = (sequence_start_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    if (!ptr) {
        EXCEPTION("cfl_sequence_pass_term_one_shot_fn: failed to get node pointer");
        return;
    }
    
    one_shot_function_t finalize_function = runtime_handle->flash_handle->one_shot_functions[ptr->finalize_function_id];
    finalize_function(runtime_handle, node_index);
}

void cfl_sequence_fail_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    cfl_sequence_pass_init_one_shot_fn(handle, node_index);
    sequence_start_fn_data_t *ptr = (sequence_start_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    ptr->sequence_type = 1;
}

void cfl_sequence_fail_term_one_shot_fn(void *handle, uint16_t node_index){
    cfl_sequence_pass_term_one_shot_fn(handle, node_index);
}


void cfl_sequence_start_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    bool already_allocated = cfl_allocate_state(runtime_handle, node_index);
    
    sequence_aggregate_data_t *ptr;
    if (already_allocated) {
        ptr = (sequence_aggregate_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    } else {
        ptr = (sequence_aggregate_data_t *)cfl_smart_arena_alloc(runtime_handle, node_index, sizeof(sequence_aggregate_data_t));
        cfl_find_try_node_indexes(runtime_handle, node_index, ptr);
    }
    
    ptr->auxiliary_data = NULL;
    
    json_decoder_init_from_runtime(runtime_handle, node_index);
    json_extract_int32_runtime(runtime_handle, "node_dict.column_data.finalize_function_id", &ptr->finalize_function_id);
    
    int32_t initialize_function_id;
    json_extract_int32_runtime(runtime_handle, "node_dict.column_data.initialize_function_id", &initialize_function_id);
    one_shot_function_t initialize_function = runtime_handle->flash_handle->one_shot_functions[initialize_function_id];
    initialize_function(runtime_handle, node_index);
    
    cfl_enable_all_nodes(runtime_handle, node_index);
}

void cfl_sequence_start_term_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    int32_t finalize_function_id;
    json_decoder_init_from_runtime(runtime_handle, node_index);
    json_extract_int32_runtime(runtime_handle, "node_dict.column_data.finalize_function_id", &finalize_function_id);
    one_shot_function_t finalize_function = runtime_handle->flash_handle->one_shot_functions[finalize_function_id];
    finalize_function(runtime_handle, node_index);
}


void cfl_join_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    bool already_allocated = cfl_allocate_state(runtime_handle, node_index);
    if (already_allocated) {
        return;
    }
    
    int32_t *ptr = (int32_t *)cfl_smart_arena_alloc(runtime_handle, node_index, sizeof(int32_t));
   
    json_decoder_init_from_runtime(runtime_handle, node_index);
    json_extract_int32_runtime(runtime_handle, "node_dict.parent_node_name", ptr);
}

void cfl_join_sequence_element_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_join_init_one_shot_fn(handle, node_index);
}

void cfl_join_sequence_element_term_one_shot_fn(void *handle, uint16_t node_index){
    (void)handle;
    (void)node_index;
}

void cfl_join_term_one_shot_fn(void *handle, uint16_t node_index){
    (void)handle;
    (void)node_index;
}


void cfl_mark_sequence_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    int32_t result;
    int32_t parent_node_name;
    printf("cfl_mark_sequence_one_shot_fn node_index: %d\n", node_index);
    json_decoder_init_from_runtime(runtime_handle, node_index);
    json_extract_int32_runtime(runtime_handle, "node_dict.result", &result);
    json_extract_int32_runtime(runtime_handle, "node_dict.parent_node_name", &parent_node_name);
    sequence_start_fn_data_t *ptr = (sequence_start_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, parent_node_name);
    if (!ptr) {
        EXCEPTION("cfl_mark_sequence_one_shot_fn: failed to get node pointer");
        return;
    }
    ptr->sequence_result_data_array[ptr->current_sequence_index].node_index = node_index;
    if (result == 1) {
        ptr->sequence_result_data_array[ptr->current_sequence_index].sequence_result = true;
    } else {
        ptr->sequence_result_data_array[ptr->current_sequence_index].sequence_result = false;
    }
    ptr->recorded_sequence_index = ptr->current_sequence_index;
} 


void cfl_watch_dog_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    bool already_allocated = cfl_allocate_state(runtime_handle, node_index);
    
    cfl_watch_dog_fn_data_t *ptr;
    if (already_allocated) {
        ptr = (cfl_watch_dog_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
        ptr->current_count = 0;
        ptr->wd_enabled = false;
        return;
    }
    
    ptr = (cfl_watch_dog_fn_data_t *)cfl_smart_arena_alloc(runtime_handle, node_index, sizeof(cfl_watch_dog_fn_data_t));
    ptr->current_count = 0;
    ptr->wd_enabled = false;
    json_decoder_init_from_runtime(runtime_handle, node_index);
    json_extract_int32_runtime(runtime_handle, "node_dict.wd_time_count", &ptr->wd_time_count);
    json_extract_bool_runtime(runtime_handle, "node_dict.wd_reset", &ptr->wd_reset);
    json_extract_int32_runtime(runtime_handle, "node_dict.wd_fn_id", &ptr->wd_fn_id);
}

void cfl_watch_dog_term_one_shot_fn(void *handle, uint16_t node_index){
    (void)handle;
    (void)node_index;
}

void cfl_while_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    bool already_allocated = cfl_allocate_state(runtime_handle, node_index);
    
    cfl_while_fn_data_t *ptr;
    if (already_allocated) {
        ptr = (cfl_while_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    } else {
        ptr = (cfl_while_fn_data_t *)cfl_smart_arena_alloc(runtime_handle, node_index, sizeof(cfl_while_fn_data_t));
    }
    
    ptr->current_iteration = 0;
    const chaintree_node_t *node = &runtime_handle->flash_handle->nodes[node_index];
    const uint16_t *link_table = runtime_handle->flash_handle->link_table;
    uint16_t link_id = link_table[node->link_start];
    cfl_enable_node(runtime_handle, link_id);
}

void cfl_while_term_one_shot_fn(void *handle, uint16_t node_index){
    (void)handle;
    (void)node_index;
}

  
void cfl_for_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    bool already_allocated = cfl_allocate_state(runtime_handle, node_index);
    
    cfl_for_fn_data_t *ptr;
    if (already_allocated) {
        ptr = (cfl_for_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    } else {
        ptr = (cfl_for_fn_data_t *)cfl_smart_arena_alloc(runtime_handle, node_index, sizeof(cfl_for_fn_data_t));
        json_decoder_init_from_runtime(runtime_handle, node_index);
        json_extract_int32_runtime(runtime_handle, "node_dict.number_of_iterations", &ptr->number_of_iterations);
    }
    
    ptr->current_iteration = 0;
    const chaintree_node_t *node = &runtime_handle->flash_handle->nodes[node_index];
    const uint16_t *link_table = runtime_handle->flash_handle->link_table;
    uint16_t link_id = link_table[node->link_start];
    cfl_enable_node(runtime_handle, link_id);
}

void cfl_for_term_one_shot_fn(void *handle, uint16_t node_index){
    (void)handle;
    (void)node_index;
}

void cfl_pat_watch_dog_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    json_decoder_init_from_runtime(runtime_handle, node_index);
    
    int32_t watch_dog_node_id;
    json_extract_int32_runtime(runtime_handle, "node_dict.node_id", &watch_dog_node_id);
    cfl_watch_dog_fn_data_t *ptr = (cfl_watch_dog_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, watch_dog_node_id);
    if (!ptr) {
        EXCEPTION("cfl_pat_watch_dog_one_shot_fn: failed to get node pointer");
        return;
    }
    
    ptr->current_count = 0;
}


void cfl_enable_watch_dog_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    json_decoder_init_from_runtime(runtime_handle, node_index);
    
    int32_t watch_dog_node_id;
    json_extract_int32_runtime(runtime_handle, "node_dict.node_id", &watch_dog_node_id);
    cfl_watch_dog_fn_data_t *ptr = (cfl_watch_dog_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, watch_dog_node_id);
    if (!ptr) {
        EXCEPTION("cfl_enable_watch_dog_one_shot_fn: failed to get node pointer");
        return;
    }
    ptr->wd_enabled = true;
    ptr->current_count = 0;
}

void cfl_disable_watch_dog_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    json_decoder_init_from_runtime(runtime_handle, node_index);
    
    int32_t watch_dog_node_id;
    json_extract_int32_runtime(runtime_handle, "node_dict.node_id", &watch_dog_node_id);
    cfl_watch_dog_fn_data_t *ptr = (cfl_watch_dog_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, watch_dog_node_id);
    if (!ptr) {
        EXCEPTION("cfl_disable_watch_dog_one_shot_fn: failed to get node pointer");
        return;
    }
    ptr->wd_enabled = false;
    ptr->current_count = 0;
}

void cfl_clear_bitmask_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    json_decoder_init_from_runtime(runtime_handle, node_index);
     
    int32_t bitmask;
    json_extract_int32_runtime(runtime_handle, "node_dict.bit_mask", &bitmask);
    runtime_handle->shaddow_bitmask &= ~bitmask;
}

void cfl_df_mask_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    bool already_allocated = cfl_allocate_state(runtime_handle, node_index);
    
    cfl_df_mask_fn_data_t *ptr;
    if (already_allocated) {
        ptr = (cfl_df_mask_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    } else {
        ptr = (cfl_df_mask_fn_data_t *)cfl_smart_arena_alloc(runtime_handle, node_index, sizeof(cfl_df_mask_fn_data_t));
        json_decoder_init_from_runtime(runtime_handle, node_index);
        
        
        json_extract_int32_runtime(runtime_handle, "node_dict.column_data.required_bitmask", &ptr->required_bitmask);
        json_extract_int32_runtime(runtime_handle, "node_dict.column_data.excluded_bitmask", &ptr->excluded_bitmask);
        
    }
    
    ptr->node_state = false;
}

void cfl_df_mask_term_one_shot_fn(void *handle, uint16_t node_index){
    (void)handle;
    (void)node_index;
}

void cfl_set_bitmask_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    json_decoder_init_from_runtime(runtime_handle, node_index);
    
    int32_t bitmask;
    json_extract_int32_runtime(runtime_handle, "node_dict.bit_mask", &bitmask);
    runtime_handle->shaddow_bitmask |= (uint64_t)bitmask;
}


void cfl_start_stop_tests_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    json_decoder_init_from_runtime(runtime_handle, node_index);
    
    uint32_t temp_length;
    int32_t temp_value;
    json_extract_array_length_runtime(runtime_handle, "node_dict.stop_tests", &temp_length);
    
    if (temp_length > 0) {
        for (uint8_t i = 0; i < temp_length; i++) {
            json_extract_array_int32_runtime(runtime_handle, "node_dict.stop_tests", i, &temp_value);
            cfl_delete_test_by_index(runtime_handle, temp_value);
        }
    }
    json_extract_array_length_runtime(runtime_handle, "node_dict.start_tests", &temp_length);

    if (temp_length > 0) {
        for (uint8_t i = 0; i < temp_length; i++) {
            json_extract_array_int32_runtime(runtime_handle, "node_dict.start_tests", i, &temp_value);
            cfl_add_test_by_index(runtime_handle, temp_value);
        }
    }
}

typedef struct {
    cfl_heap_allocator_id_t allocator_id;
    uint16_t arena_size;
} local_arena_data_t;

void cfl_local_arena_init_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    
    bool already_allocated = cfl_allocate_state(runtime_handle, node_index);
    if (already_allocated) {
        cfl_enable_all_nodes(runtime_handle, node_index);
        return;
    }
    
    int32_t arena_size;
    json_decoder_init_from_runtime(runtime_handle, node_index);
    json_extract_int32_runtime(runtime_handle, "node_dict.column_data.arena_size", &arena_size);
    
    cfl_heap_allocator_id_t id = cfl_heap_arena_create(runtime_handle->arena_system, node_index, arena_size);
    if (id == 0xff) {
        EXCEPTION("cfl_local_arena_init_one_shot_fn: failed to create arena");
        return;
    }
    cfl_memory_allocator_assignment(runtime_handle, node_index, id);
    cfl_heap_arena_set_node_allocator_id(runtime_handle->arena_system, node_index, runtime_handle->allocator_id);
    cfl_heap_arena_set_node_memory_index(runtime_handle->arena_system, node_index, 0xFFFF); 
   
    local_arena_data_t *ptr = (local_arena_data_t *)cfl_smart_arena_alloc(runtime_handle, node_index, sizeof(local_arena_data_t));
    if (!ptr) {
        EXCEPTION("cfl_local_arena_init_one_shot_fn: failed to get node pointer");
        return;
    }
    ptr->allocator_id = id;
    ptr->arena_size = arena_size;
    cfl_enable_all_nodes(runtime_handle, node_index);
}

void cfl_local_arena_term_one_shot_fn(void *handle, uint16_t node_index){
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    local_arena_data_t *ptr = (local_arena_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    if (!ptr) {
        EXCEPTION("cfl_local_arena_term_one_shot_fn: failed to get node pointer");
        return;
    }
    cfl_heap_arena_destroy(runtime_handle->arena_system, ptr->allocator_id, node_index);
}
