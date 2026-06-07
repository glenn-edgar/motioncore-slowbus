#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "cfl_runtime.h"
#include "cfl_engine.h"
#include "json_node_decoder.h"
#include "cfl_common_function_headers.h"
#include "cfl_common_functions.h"
#include "cfl_supervisor_support.h"

bool cfl_null_boolean_fn(void *handle, unsigned node_index, unsigned event_type, unsigned event_id, void *event_data){
    (void)handle;
    (void)node_index;
    (void)event_type;
    (void)event_id;
    (void)event_data;
    return false;
}


bool cfl_bool_false_boolean_fn(void *handle, unsigned node_index, unsigned event_type, unsigned event_id, void *event_data){

    (void)handle;
    (void)node_index;
    (void)event_type;
    (void)event_id;
    (void)event_data;
    
    return false;
    
}

bool cfl_column_null_boolean_fn(void *handle, unsigned node_index, unsigned event_type, unsigned event_id, void *event_data){

    (void)handle;
    (void)node_index;
    (void)event_type;
    (void)event_id;
    (void)event_data;
    return false;
    
}

bool cfl_gate_node_null_boolean_fn(void *handle, unsigned node_index, unsigned event_type, unsigned event_id, void *event_data){

    (void)handle;
    (void)node_index;
    (void)event_id;
    (void)event_type;
    (void)event_data;
    
    return false;
    
}

typedef struct{
    double timestamp_timeout;
} cfl_verify_time_out_boolean_fn_data_t;

bool cfl_verify_time_out_boolean_fn(void *handle, unsigned node_index, unsigned event_type, unsigned event_id, void *event_data){
    (void)event_data;
    (void)event_type;

    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    cfl_verify_fn_data_t *ptr = (cfl_verify_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    if (!ptr) {
        EXCEPTION("cfl_verify_time_out_boolean_fn: failed to get node pointer");
        return false;
    }
    
    if(event_id == CFL_INIT_EVENT){
        float time_out;

        json_decoder_init_from_runtime(runtime_handle, node_index);
        cfl_verify_time_out_boolean_fn_data_t *auxiliary_data = (cfl_verify_time_out_boolean_fn_data_t *)cfl_heap_malloc_pointer(runtime_handle->heap, sizeof(cfl_verify_time_out_boolean_fn_data_t));
        if (!auxiliary_data) {
            EXCEPTION("cfl_verify_time_out_boolean_fn: failed to allocate auxiliary_data");
            return false;
        }
        json_extract_float32_runtime(runtime_handle, "node_dict.fn_data.time_out", &time_out);
        auxiliary_data->timestamp_timeout = cfl_timer_get_timestamp(runtime_handle->timer_handle) + (double)time_out;
        ptr->auxiliary_data = auxiliary_data;
        return false;
    }
    if(event_id == CFL_TERMINATE_EVENT){
        if (ptr->auxiliary_data) {
            cfl_heap_free_pointer(runtime_handle->heap, ptr->auxiliary_data);
        }
        return false;
    }
    if(event_id == CFL_TIMER_EVENT){
        if (!ptr->auxiliary_data) {
            EXCEPTION("cfl_verify_time_out_boolean_fn: auxiliary_data is NULL");
            return false;
        }
        
        cfl_verify_time_out_boolean_fn_data_t *auxiliary_data = (cfl_verify_time_out_boolean_fn_data_t *)ptr->auxiliary_data;
        double compare_timestamp = auxiliary_data->timestamp_timeout;
        
        if(cfl_timer_get_timestamp(runtime_handle->timer_handle) >= compare_timestamp){
            return false;
        }
    }
    
    
    return true;
}


typedef struct {
    int32_t event_id;
    int32_t event_count;
} cfl_wait_for_event_boolean_fn_data_t;
  


bool cfl_wait_for_event_boolean_fn(void *handle, unsigned node_index, unsigned event_type, unsigned event_id, void *event_data){
    (void)event_data;
    (void)event_type;
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    cfl_wait_fn_data_t *ptr = (cfl_wait_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    if (!ptr) {
        EXCEPTION("cfl_wait_for_event_boolean_fn: failed to get node pointer");
        return false;
    }

    if(event_id == CFL_INIT_EVENT){
    
       cfl_wait_for_event_boolean_fn_data_t *auxiliary_data =
        (cfl_wait_for_event_boolean_fn_data_t *) cfl_heap_malloc_pointer(runtime_handle->heap, sizeof(cfl_wait_for_event_boolean_fn_data_t));
        if (!auxiliary_data) {
            EXCEPTION("cfl_wait_for_event_boolean_fn: failed to allocate auxiliary_data");
            return false;
        }
        json_decoder_init_from_runtime(runtime_handle, node_index);
        json_extract_int32_runtime(runtime_handle, "node_dict.wait_fn_data.event_id", &auxiliary_data->event_id);
        json_extract_int32_runtime(runtime_handle, "node_dict.wait_fn_data.event_count", &auxiliary_data->event_count);
        ptr->auxiliary_data = auxiliary_data;
       return false;

    }
    if(event_id == CFL_TERMINATE_EVENT){
        if (ptr->auxiliary_data) {
            cfl_heap_free_pointer(runtime_handle->heap, ptr->auxiliary_data);
        }
        return false;

    }
    
    if (!ptr->auxiliary_data) {
        EXCEPTION("cfl_wait_for_event_boolean_fn: auxiliary_data is NULL");
        return false;
    }
    
    cfl_wait_for_event_boolean_fn_data_t *auxiliary_data = (cfl_wait_for_event_boolean_fn_data_t *)ptr->auxiliary_data;
    if((unsigned)event_id == (unsigned)auxiliary_data->event_id){
        auxiliary_data->event_count--;
        if(auxiliary_data->event_count == 0){
            return true;
        }
    }
    return false;
}




bool cfl_state_machine_null_boolean_fn(void *handle, unsigned node_index, unsigned event_type, unsigned event_id, void *event_data){
    (void)handle;
    (void)node_index;
    (void)event_type;
    (void)event_id;
    (void)event_data;
    return false;
}


bool cfl_sm_event_sync_boolean_fn(void *handle, unsigned node_index, unsigned event_type, unsigned event_id, void *event_data){
    (void)event_type;
    (void)event_data;
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    cfl_state_machine_column_data_t *ptr = (cfl_state_machine_column_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    if (!ptr) {
        EXCEPTION("cfl_sm_event_sync_boolean_fn: failed to get node pointer");
        return false;
    }

    if (event_id == CFL_INIT_EVENT){
        return false;
    }
    if (event_id == CFL_TERMINATE_EVENT){
        return false;
    }
    if ( ptr->sync_event_id_valid == false){
        return false;
    }
    if(event_id == (unsigned)ptr->sync_event_id){

        ptr->sync_event_id_valid = false;
    }

    return true;
}


bool cfl_verify_bitmask_boolean_fn(void *handle, unsigned node_index, unsigned event_type, unsigned event_id, void *event_data){
    (void)event_type;
    (void)event_data;
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    cfl_wait_fn_data_t *ptr = (cfl_wait_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    if (!ptr) {
        EXCEPTION("cfl_wait_for_bitmask_boolean_fn: failed to get node pointer");
        return false;
    }
    if(event_id == CFL_INIT_EVENT){
        if(ptr->auxiliary_data == NULL){
            int32_t required_bitmask;
            int32_t excluded_bitmask;
            json_decoder_init_from_runtime(runtime_handle, node_index);
           
            json_extract_int32_runtime(runtime_handle, "node_dict.fn_data.required_bitmask", &required_bitmask);
            json_extract_int32_runtime(runtime_handle, "node_dict.fn_data.excluded_bitmask", &excluded_bitmask);
            ptr->auxiliary_data = (void *)(cfl_df_mask_fn_data_t *)cfl_heap_malloc_pointer(runtime_handle->heap, sizeof(cfl_df_mask_fn_data_t));
            if (!ptr->auxiliary_data) {
                EXCEPTION("cfl_wait_for_bitmask_boolean_fn: failed to allocate auxiliary_data");
                return false;
            }
            ((cfl_df_mask_fn_data_t *)ptr->auxiliary_data)->required_bitmask = required_bitmask;
            ((cfl_df_mask_fn_data_t *)ptr->auxiliary_data)->excluded_bitmask = excluded_bitmask;
            ((cfl_df_mask_fn_data_t *)ptr->auxiliary_data)->node_state = false;
            return false;
        }
    }
    if(event_id == CFL_TERMINATE_EVENT){
        return false;
    }
    if(event_id != CFL_TIMER_EVENT){
        return true;
    }
    cfl_df_mask_fn_data_t *auxiliary_data = (cfl_df_mask_fn_data_t *)ptr->auxiliary_data;
    
    // Required bits must all be set, excluded bits must all be clear
    bool required_met = ((uint64_t)auxiliary_data->required_bitmask & runtime_handle->bitmask) == (uint64_t)auxiliary_data->required_bitmask;
    bool excluded_clear = ((uint64_t)auxiliary_data->excluded_bitmask & runtime_handle->bitmask) == 0;
    
    if (required_met && excluded_clear) {
        return true;
    }
    
    return false;
}





bool cfl_wait_for_bitmask_boolean_fn(void *handle, unsigned node_index, unsigned event_type, unsigned event_id, void *event_data){
    (void)event_type;
    (void)event_data;
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    cfl_wait_fn_data_t *ptr = (cfl_wait_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    if (!ptr) {
        EXCEPTION("cfl_wait_for_bitmask_boolean_fn: failed to get node pointer");
        return false;
    }
    if(event_id == CFL_INIT_EVENT){
        if(ptr->auxiliary_data == NULL){
            int32_t required_bitmask;
            int32_t excluded_bitmask;
            json_decoder_init_from_runtime(runtime_handle, node_index);
           
            json_extract_int32_runtime(runtime_handle, "node_dict.wait_fn_data.required_bitmask", &required_bitmask);
            json_extract_int32_runtime(runtime_handle, "node_dict.wait_fn_data.excluded_bitmask", &excluded_bitmask);
            ptr->auxiliary_data = (void *)(cfl_df_mask_fn_data_t *)cfl_heap_malloc_pointer(runtime_handle->heap, sizeof(cfl_df_mask_fn_data_t));
            if (!ptr->auxiliary_data) {
                EXCEPTION("cfl_wait_for_bitmask_boolean_fn: failed to allocate auxiliary_data");
                return false;
            }
            ((cfl_df_mask_fn_data_t *)ptr->auxiliary_data)->required_bitmask = required_bitmask;
            ((cfl_df_mask_fn_data_t *)ptr->auxiliary_data)->excluded_bitmask = excluded_bitmask;
            ((cfl_df_mask_fn_data_t *)ptr->auxiliary_data)->node_state = false;
            return false;
        }
    }
    if(event_id == CFL_TERMINATE_EVENT){
        return false;
    }
    if(event_id != CFL_TIMER_EVENT){
        return false;
    }
    cfl_df_mask_fn_data_t *auxiliary_data = (cfl_df_mask_fn_data_t *)ptr->auxiliary_data;
    
    // Required bits must all be set, excluded bits must all be clear
    bool required_met = ((uint64_t)auxiliary_data->required_bitmask & runtime_handle->bitmask) == (uint64_t)auxiliary_data->required_bitmask;
    bool excluded_clear = ((uint64_t)auxiliary_data->excluded_bitmask & runtime_handle->bitmask) == 0;
    
    if (required_met && excluded_clear) {
        return true;
    }
    
    return false;
}

typedef struct{

    uint8_t *test_ids;
    uint8_t test_ids_length;
}cfl_wait_for_tests_complete_boolean_fn_data_t;


bool cfl_verify_tests_active_boolean_fn(void *handle, unsigned node_index, unsigned event_type, unsigned event_id, void *event_data){
    (void)event_type;
    (void)event_data;
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    cfl_verify_fn_data_t *ptr = (cfl_verify_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    if (!ptr) {
        EXCEPTION("cfl_wait_for_tests_complete_boolean_fn: failed to get node pointer");
        return false;
    }
    if(event_id == CFL_INIT_EVENT){
        if(ptr->auxiliary_data == NULL){
            cfl_wait_for_tests_complete_boolean_fn_data_t *auxiliary_data = 
            (cfl_wait_for_tests_complete_boolean_fn_data_t *)cfl_additional_arena_alloc(runtime_handle, node_index, sizeof(cfl_wait_for_tests_complete_boolean_fn_data_t));
            if (!auxiliary_data) {
                EXCEPTION("cfl_wait_for_tests_complete_boolean_fn: failed to allocate auxiliary_data");
                return false;
            }
            int32_t temp_value;
            uint32_t temp_length;
            json_decoder_init_from_runtime(runtime_handle, node_index);
            json_extract_array_length_runtime(runtime_handle, "node_dict.fn_data.test_ids", &temp_length);
            auxiliary_data->test_ids_length = temp_length;
            if(temp_length > 0){
            auxiliary_data->test_ids = (uint8_t *)cfl_additional_arena_alloc(runtime_handle, node_index, sizeof(uint8_t) * temp_length);
                for(uint16_t i = 0; i < temp_length; i++){
                    json_extract_array_int32_runtime(runtime_handle, "node_dict.fn_data.test_ids", i, &temp_value);
                    auxiliary_data->test_ids[i] = temp_value;
                }
                ptr->auxiliary_data = auxiliary_data;
            }else{
                EXCEPTION("cfl_wait_for_tests_complete_boolean_fn: test_ids is empty");
                return false;
            }
        }
        return false;
    }
    if(event_id == CFL_TERMINATE_EVENT){
        return false;
    }
    if(event_id != CFL_TIMER_EVENT){
        return true;
    }
    cfl_wait_for_tests_complete_boolean_fn_data_t *auxiliary_data = (cfl_wait_for_tests_complete_boolean_fn_data_t *)ptr->auxiliary_data;
    for(uint16_t i = 0; i < auxiliary_data->test_ids_length; i++){
        if(!TEST_IS_ACTIVE(runtime_handle, auxiliary_data->test_ids[i])){
            return false;
        }
    }
    return true;
}


bool cfl_wait_for_tests_complete_boolean_fn(void *handle, unsigned node_index, unsigned event_type, unsigned event_id, void *event_data){
    (void)event_type;
    (void)event_data;
    cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
    cfl_wait_fn_data_t *ptr = (cfl_wait_fn_data_t *)cfl_heap_arena_get_node_ptr(runtime_handle->arena_system, node_index);
    if (!ptr) {
        EXCEPTION("cfl_wait_for_tests_complete_boolean_fn: failed to get node pointer");
        return false;
    }
    if(event_id == CFL_INIT_EVENT){
        if(ptr->auxiliary_data == NULL){
            cfl_wait_for_tests_complete_boolean_fn_data_t *auxiliary_data = 
            (cfl_wait_for_tests_complete_boolean_fn_data_t *)cfl_additional_arena_alloc(runtime_handle, node_index, sizeof(cfl_wait_for_tests_complete_boolean_fn_data_t));
            if (!auxiliary_data) {
                EXCEPTION("cfl_wait_for_tests_complete_boolean_fn: failed to allocate auxiliary_data");
                return false;
            }
            int32_t temp_value;
            uint32_t temp_length;
            json_decoder_init_from_runtime(runtime_handle, node_index);
            json_extract_array_length_runtime(runtime_handle, "node_dict.wait_fn_data.test_ids", &temp_length);
            auxiliary_data->test_ids_length = temp_length;
            if(temp_length > 0){
            auxiliary_data->test_ids = (uint8_t *)cfl_additional_arena_alloc(runtime_handle, node_index, sizeof(uint8_t) * temp_length);
                for(uint16_t i = 0; i < temp_length; i++){
                    json_extract_array_int32_runtime(runtime_handle, "node_dict.wait_fn_data.test_ids", i, &temp_value);
                    auxiliary_data->test_ids[i] = temp_value;
                }
                ptr->auxiliary_data = auxiliary_data;
            }else{
                EXCEPTION("cfl_wait_for_tests_complete_boolean_fn: test_ids is empty");
                return false;
            }
        }
        return false;
    }
    if(event_id == CFL_TERMINATE_EVENT){
        return false;
    }
    if(event_id != CFL_TIMER_EVENT){
        return false;
    }
    cfl_wait_for_tests_complete_boolean_fn_data_t *auxiliary_data = (cfl_wait_for_tests_complete_boolean_fn_data_t *)ptr->auxiliary_data;
    for(uint16_t i = 0; i < auxiliary_data->test_ids_length; i++){
        if(!TEST_IS_ACTIVE(runtime_handle, auxiliary_data->test_ids[i])){
            return false;
        }
    }
    return true;
}
