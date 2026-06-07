/**
 * @file cfl_runtime.c
 * @brief ChainTree Runtime — creation, reset, event loop, test management
 */

 #include <stdlib.h>
 #include <stdio.h>
 #include <string.h>
 #include <stdbool.h>
 #include "cfl_runtime.h"
 #include "cfl_blackboard.h"
 #include "json_node_decoder.h"
 /* cfl_runtime.h → cfl_engine.h transitively provides:
  *   chaintree_support.h, cfl_perm.h, cfl_heap.h, cfl_heap_arena_allocate.h,
  *   cfl_event_queue.h, cfl_timer_system.h, CT_Tree_Walker.h
  */
 
 /* ========================================================================
  * FORWARD DECLARATIONS — STATIC HELPERS
  * ======================================================================== */
 
 static void cfl_find_main_ids(cfl_runtime_handle_t *handle);
 static unsigned int cfl_calculate_max_level(cfl_runtime_handle_t *handle);
 static void cfl_set_timer_reference(cfl_runtime_handle_t *handle);
 static void cfl_send_system_event_to_test(cfl_runtime_handle_t *handle,
     uint16_t kb_idx, unsigned event_id, unsigned event_type,
     bool malloc_flag, void *data);
 static void cfl_generate_timer_events(cfl_runtime_handle_t *handle,
     uint16_t kb_idx, cfl_tick_result_t *result);
 static void cfl_init_test_system(cfl_runtime_handle_t *handle);
 static void process_stop_start_tests(cfl_runtime_handle_t *handle,
     cfl_start_stop_tests_fn_data_t *ptr);
 
 /* ========================================================================
  * PARAMETER BLOCK LIFECYCLE
  * ======================================================================== */
 
 cfl_runtime_create_params_t *cfl_runtime_create_params_create(void) {
     cfl_runtime_create_params_t *params =
         (cfl_runtime_create_params_t *)calloc(1, sizeof(cfl_runtime_create_params_t));
     if (!params) {
         EXCEPTION("cfl_runtime_create_params_create: Failed to allocate memory for params");
     }
     /* calloc already zeroes memory — no memset needed */
     return params;
 }
 
 void cfl_runtime_create_params_destroy(cfl_runtime_create_params_t *params) {
     if (!params) {
         EXCEPTION("cfl_runtime_create_params_destroy: NULL params pointer");
     }
     free(params);
 }
 
 /* ========================================================================
  * RUNTIME CREATION
  * ======================================================================== */
 
 cfl_runtime_handle_t *cfl_runtime_create(cfl_perm_t *perm,
                                           cfl_runtime_create_params_t *params,
                                           const chaintree_handle_t *flash_handle) {
;
     if (params->total_node_count != flash_handle->node_count) {
         EXCEPTION("cfl_runtime_create: params->total_node_count doesn't match flash_handle->node_count");
     }
 
     cfl_perm_set_instance(perm);
     cfl_perm_init(perm, params->perm_buffer, params->perm_buffer_size);
 
     cfl_runtime_handle_t *handle =
         (cfl_runtime_handle_t *)cfl_perm_alloc_pointer(perm, (uint16_t)sizeof(cfl_runtime_handle_t));
     if (!handle) {
         EXCEPTION("cfl_runtime_create: Failed to allocate memory for handle");
     }
    
     handle->flash_handle = flash_handle;
     handle->main_function_data =
         (main_function_data_t *)cfl_perm_alloc_pointer(perm, (uint16_t)sizeof(main_function_data_t));
     cfl_find_main_ids(handle);
     printf("made it here 2\n");
     /* Memory subsystems */
     handle->perm = perm;
     handle->heap = cfl_heap_init(perm, params->heap_size);
     printf("total_node_count: %d\n", params->total_node_count);
     printf("max_allocator_count: %d\n", params->max_allocator_count);
     handle->arena_system = cfl_heap_arena_system_create(
         perm, handle->heap, params->max_allocator_count,
         params->total_node_count, params->allocator_0_size);
     printf("arena_system created\n");
     /* Event queue */
     handle->event_queue = cfl_create_event_queue(
         params->event_queue_high_priority_size,
         params->event_queue_low_priority_size, perm);
 
     /* Flags arrays */
     size_t flags_size = sizeof(uint8_t) * params->total_node_count;
     if (flags_size > 65535) {
         EXCEPTION("cfl_runtime_create: Flags array size exceeds uint16_t limit");
     }
     handle->flags = (uint8_t *)cfl_perm_alloc_pointer(perm, (uint16_t)flags_size);
     handle->backup_flags = (uint8_t *)cfl_perm_alloc_pointer(perm, (uint16_t)flags_size);
 
     /* Timer */
     handle->timer_handle = cfl_timer_create(params->delta_time, perm);
     handle->delta_time = params->delta_time;
 
     /* Tree walker stacks */
     handle->max_level = cfl_calculate_max_level(handle);
     size_t stack_size = sizeof(CT_StackEntry) * handle->max_level;
     if (stack_size > 65535) {
         EXCEPTION("cfl_runtime_create: Stack size exceeds uint16_t limit");
     }
     handle->stack = (CT_StackEntry *)cfl_perm_alloc_pointer(perm, (uint16_t)stack_size);
     handle->nested_stack = (CT_StackEntry *)cfl_perm_alloc_pointer(perm, (uint16_t)stack_size);
     handle->walker = (CT_TreeWalker *)cfl_perm_alloc_pointer(perm, (uint16_t)sizeof(CT_TreeWalker));
     handle->walker_context_ptr = (CT_WalkerContext *)cfl_perm_alloc_pointer(perm, (uint16_t)sizeof(CT_WalkerContext));
 
     /* JSON decoder context */
     handle->json_decoder_ctx =
         (json_decoder_ctx_t *)cfl_perm_alloc_pointer(perm, (uint16_t)sizeof(json_decoder_ctx_t));

     /* Shared blackboard */
     if (!cfl_bb_init(handle)) {
         EXCEPTION("cfl_runtime_create: blackboard initialization failed");
     }

     memset((void *)handle->flags, 0, params->total_node_count);
 
     cfl_init_test_system(handle);
     cfl_engine_create(handle);
 
     printf("bytes used: %d\n", cfl_perm_used_bytes(perm));
     printf("bytes free: %d\n", cfl_perm_free_bytes(perm));
 
     return handle;
 }
 
 /* ========================================================================
  * RUNTIME RESET
  * ======================================================================== */
 
 void cfl_runtime_reset(cfl_runtime_handle_t *handle) {
     if (!handle) {
         EXCEPTION("cfl_runtime_reset: NULL handle pointer");
     }
 
     handle->max_level = cfl_calculate_max_level(handle);
     
     cfl_heap_arena_system_reset(handle->arena_system);
     cfl_clear_queue(handle->event_queue);
     handle->bitmask = 0;
     handle->shaddow_bitmask = 0;
     cfl_bb_reset(handle);
     cfl_engine_init(handle);
     memset((void *)handle->flags, 0, handle->flash_handle->node_count);
 
     /* Reinitialize all active tests */
     for (uint16_t kb_idx = 0; kb_idx < handle->flash_handle->kb_count; kb_idx++) {
         if (TEST_IS_ACTIVE(handle, kb_idx)) {
             const chaintree_kb_info_t *kb = &handle->flash_handle->kb_table[kb_idx];
             cfl_engine_init_test(handle, kb->start_index, kb->node_count);
         }
     }
 
     printf("heap used bytes: %d free bytes: %d\n",
            cfl_heap_used_bytes(handle->heap), cfl_heap_free_bytes(handle->heap));
 }
 
 /* ========================================================================
  * RUNTIME EVENT LOOP
  * ======================================================================== */

/* Find the active KB whose node range contains node_id. Used to route events
 * popped from the shared queue to their owning KB's window — so events injected
 * from outside the engine (firmware) or from another KB execute correctly
 * instead of tripping cfl_execute_event's out-of-bounds guard. 0xFFFF = none. */
 static uint16_t cfl_find_owning_active_kb(cfl_runtime_handle_t *handle, uint16_t node_id) {
     const chaintree_handle_t *fh = handle->flash_handle;
     for (uint16_t k = 0; k < fh->kb_count; k++) {
         if (!TEST_IS_ACTIVE(handle, k)) continue;
         uint16_t s = fh->kb_table[k].start_index;
         if (node_id >= s && node_id < s + fh->kb_table[k].node_count) return k;
     }
     return 0xFFFF;
 }

 bool cfl_runtime_run(cfl_runtime_handle_t *handle) {
     CFL_EVENT_DATA_T event_data;
     cfl_tick_result_t tick_result;
     bool result = true;
 
     if (!handle) {
         EXCEPTION("cfl_runtime_run: NULL handle pointer");
     }
     printf("made it here 3\n");
     printf("---------------------------------start of runtime run---------------------------------\n");
     printf("cfl_perm_used_bytes: %d\n", cfl_perm_used_bytes(handle->perm));
     printf("cfl_perm_free_bytes: %d\n", cfl_perm_free_bytes(handle->perm));
     printf("arena 0 : used bytes: %d free bytes: %d\n",
            cfl_heap_arena_used_bytes(handle->arena_system, 0),
            cfl_heap_arena_free_bytes(handle->arena_system, 0));
 
     cfl_set_timer_reference(handle);
     handle->shaddow_bitmask = 0;
     handle->bitmask = 0;
 
     bool loop_flag = true;
     while (loop_flag) {
         /* Wait for timer once per cycle — OUTSIDE the test loop */
         double delta_time = handle->future_time_stamp - cfl_timer_get_timestamp(handle->timer_handle);
         cfl_timer_wait(handle->timer_handle, delta_time, &tick_result);
 
         loop_flag = false;
         for (uint16_t kb_idx = 0; kb_idx < handle->flash_handle->kb_count; kb_idx++) {
             if (!TEST_IS_ACTIVE(handle, kb_idx)) continue;
 
             loop_flag = true;
             handle->current_kb_idx = kb_idx;
             handle->kb_start_index = handle->flash_handle->kb_table[kb_idx].start_index;
             handle->kb_node_count = handle->flash_handle->kb_table[kb_idx].node_count;
             handle->kb_max_level = handle->flash_handle->kb_table[kb_idx].max_depth + 1;
 
             cfl_generate_timer_events(handle, kb_idx, &tick_result);
             handle->bitmask = handle->shaddow_bitmask;
 
             while (cfl_total_event_count(handle->event_queue) > 0) {
                 cfl_pop_event(handle->event_queue, &event_data);
 
                 if (event_data.event_id == CFL_TERMINATE_SYSTEM_EVENT) {
                     printf("terminate system\n");
                     goto exit;
                 }
                 if (event_data.event_id == CFL_STOP_START_TESTS_EVENT) {
                     process_stop_start_tests(handle,
                         (cfl_start_stop_tests_fn_data_t *)event_data.data.ptr);
                     continue;
                 }
 
                 /* Route to the active KB that owns the target node. The shared
                  * queue is drained entirely during the first active KB's pass,
                  * so an event for another KB (e.g. a firmware-injected command)
                  * must run in ITS window, not this iteration's. */
                 uint16_t owner = cfl_find_owning_active_kb(handle, event_data.node_id);
                 if (owner == 0xFFFF) {
                     /* node_id past the whole node table is genuine corruption ->
                      * keep the loud guard. A valid node whose KB just isn't active
                      * (e.g. a command for a KB4-deleted interlock) is dropped. */
                     if (event_data.node_id >= handle->flash_handle->node_count) {
                         EXCEPTION("cfl_runtime_run: event node_id out of range");
                     }
                     continue;
                 }
                 handle->current_kb_idx = owner;
                 handle->kb_start_index = handle->flash_handle->kb_table[owner].start_index;
                 handle->kb_node_count  = handle->flash_handle->kb_table[owner].node_count;
                 handle->kb_max_level   = handle->flash_handle->kb_table[owner].max_depth + 1;

                 handle->event_data_ptr = &event_data;

                 if (cfl_execute_event(handle) == false) {
                     cfl_delete_test_by_index(handle, handle->current_kb_idx);
                 }
             }

             /* Check if start node is still enabled after processing all events.
              * Use kb_idx's own start node — the drain above may have re-pointed
              * kb_start_index to a routed event's owner KB. */
             if (!cfl_engine_node_is_enabled(handle,
                     handle->flash_handle->kb_table[kb_idx].start_index)) {
                 cfl_delete_test_by_index(handle, kb_idx);
             }
         }
 
         /* Update timestamp once per cycle — AFTER all tests processed */
         handle->future_time_stamp += handle->delta_time;
         if (!loop_flag) {
             goto exit;
         }
     }
 
 exit:
     printf("---------------------------------end of runtime run---------------------------------\n");
     printf("cfl_perm_used_bytes: %d\n", cfl_perm_used_bytes(handle->perm));
     printf("cfl_perm_free_bytes: %d\n", cfl_perm_free_bytes(handle->perm));
     printf("heap used bytes: %d free bytes: %d\n",
            cfl_heap_used_bytes(handle->heap), cfl_heap_free_bytes(handle->heap));
     printf("high priority count: %d low priority count: %d\n",
            cfl_high_priority_count(handle->event_queue),
            cfl_low_priority_count(handle->event_queue));
     printf("arena 0 : used bytes: %d free bytes: %d\n",
            cfl_heap_arena_used_bytes(handle->arena_system, 0),
            cfl_heap_arena_free_bytes(handle->arena_system, 0));
     printf("runtime run completed\n");
 
     return result;
 }
 
 /* ========================================================================
  * TIMER REFERENCE
  * ======================================================================== */
 
 static void cfl_set_timer_reference(cfl_runtime_handle_t *handle) {
     cfl_tick_result_t result;
     cfl_timer_wait(handle->timer_handle, 0.001, &result);
     handle->future_time_stamp = cfl_timer_get_timestamp(handle->timer_handle) + handle->delta_time;
 }
 
 /* ========================================================================
  * EVENT GENERATION
  * ======================================================================== */
 
 static void cfl_send_system_event_to_test(cfl_runtime_handle_t *handle,
     uint16_t kb_idx, unsigned event_id, unsigned event_type,
     bool malloc_flag, void *data) {
 
     if (!TEST_IS_ACTIVE(handle, kb_idx)) return;
 
     const chaintree_kb_info_t *kb = &handle->flash_handle->kb_table[kb_idx];
 
     if (cfl_engine_node_is_enabled(handle, kb->start_index)) {
         cfl_send_event(handle->event_queue, CFL_EVENT_PRIORITY_LOW,
             kb->start_index, event_type, malloc_flag, event_id, data);
     }
 }
 
 static void cfl_generate_timer_events(cfl_runtime_handle_t *handle,
     uint16_t kb_idx, cfl_tick_result_t *result) {
 
     cfl_send_system_event_to_test(handle, kb_idx, CFL_TIMER_EVENT,
         CFL_EVENT_TYPE_PTR, false, result);
 
     if (result->changed_mask & CFL_CHANGED_SECOND) {
         cfl_send_system_event_to_test(handle, kb_idx, CFL_SECOND_EVENT,
             CFL_EVENT_TYPE_PTR, false, result);
     }
     if (result->changed_mask & CFL_CHANGED_MINUTE) {
         cfl_send_system_event_to_test(handle, kb_idx, CFL_MINUTE_EVENT,
             CFL_EVENT_TYPE_PTR, false, result);
     }
     if (result->changed_mask & CFL_CHANGED_HOUR) {
         cfl_send_system_event_to_test(handle, kb_idx, CFL_HOUR_EVENT,
             CFL_EVENT_TYPE_PTR, false, result);
     }
     if (result->changed_mask & CFL_CHANGED_DAY) {
         cfl_send_system_event_to_test(handle, kb_idx, CFL_DAY_EVENT,
             CFL_EVENT_TYPE_PTR, false, result);
     }
     if (result->changed_mask & CFL_CHANGED_DOW) {
         cfl_send_system_event_to_test(handle, kb_idx, CFL_WEEK_EVENT,
             CFL_EVENT_TYPE_PTR, false, result);
     }
     if (result->changed_mask & CFL_CHANGED_DOY) {
         cfl_send_system_event_to_test(handle, kb_idx, CFL_YEAR_EVENT,
             CFL_EVENT_TYPE_PTR, false, result);
     }
 }
 
 /* ========================================================================
  * TEST SYSTEM
  * ======================================================================== */
 
 static void cfl_init_test_system(cfl_runtime_handle_t *handle) {
     unsigned bitmap_size = (handle->flash_handle->kb_count + 31) / 32;
 
     handle->active_test_bitmap =
         cfl_perm_alloc_pointer(handle->perm, bitmap_size * sizeof(uint32_t));
     memset((void *)handle->active_test_bitmap, 0, bitmap_size * sizeof(uint32_t));
 
     handle->kb_allocator_ids = (cfl_heap_allocator_id_t *)cfl_perm_alloc_pointer(
         handle->perm, handle->flash_handle->kb_count * sizeof(cfl_heap_allocator_id_t));
     handle->test_has_arena = (uint8_t *)cfl_perm_alloc_pointer(
         handle->perm, handle->flash_handle->kb_count * sizeof(uint8_t));
 
     for (uint16_t i = 0; i < handle->flash_handle->kb_count; i++) {
         handle->kb_allocator_ids[i] = 0xff;
         handle->test_has_arena[i] = 0;
     }
     handle->active_test_count = 0;
 }
 
 bool cfl_add_test_by_index(cfl_runtime_handle_t *handle, uint16_t kb_index) {
    printf("made it here 4\n");
     if (kb_index >= handle->flash_handle->kb_count) {
         EXCEPTION("cfl_add_test_by_index: kb_index out of bounds");
     }
     if (TEST_IS_ACTIVE(handle, kb_index)) return false;
 
     const chaintree_kb_info_t *kb = &handle->flash_handle->kb_table[kb_index];
     cfl_heap_allocator_id_t arena_id =
         cfl_heap_arena_create(handle->arena_system, kb->start_index,
                               kb->node_count * kb->memory_factor);
     if (arena_id == 0xff) {
         EXCEPTION("cfl_add_test_by_index: Arena allocation failed");
     }
 
     for (uint16_t i = kb->start_index; i < kb->start_index + kb->node_count; i++) {
         cfl_heap_arena_set_node_allocator_id(handle->arena_system, i, arena_id);
     }
     handle->kb_allocator_ids[kb_index] = arena_id;
     handle->test_has_arena[kb_index] = true;
     cfl_engine_init_test(handle, kb->start_index, kb->node_count);
 
     TEST_ACTIVE_SET(handle, kb_index);
     handle->active_test_count++;
 
     printf("made it here 5\n");
     return true;
 }
 
 bool cfl_delete_test_by_index(cfl_runtime_handle_t *handle, uint16_t kb_index) {
     if (kb_index >= handle->flash_handle->kb_count) return false;
     if (!TEST_IS_ACTIVE(handle, kb_index)) return false;
 
     if (handle->test_has_arena[kb_index]) {
         const chaintree_kb_info_t *kb = &handle->flash_handle->kb_table[kb_index];
         cfl_heap_allocator_id_t arena_id = handle->kb_allocator_ids[kb_index];
 
         cfl_terminate_all_nodes_in_kb(handle, kb->start_index, kb->node_count);
 
         printf("used bytes: %d\n", cfl_heap_arena_used_bytes(handle->arena_system, arena_id));
         printf("free bytes: %d\n", cfl_heap_arena_free_bytes(handle->arena_system, arena_id));
 
         cfl_heap_arena_destroy(handle->arena_system, arena_id, kb->start_index);
         for (uint16_t i = kb->start_index; i < kb->start_index + kb->node_count; i++) {
             cfl_heap_arena_set_node_allocator_id(handle->arena_system, i, 0xff);
         }
         handle->test_has_arena[kb_index] = false;
         handle->kb_allocator_ids[kb_index] = 0xff;
     }
 
     TEST_ACTIVE_CLR(handle, kb_index);
     handle->active_test_count--;
 
     return true;
 }
 
 /* ========================================================================
  * INTERNAL HELPERS
  * ======================================================================== */
 
 static unsigned int cfl_calculate_max_level(cfl_runtime_handle_t *handle) {
     unsigned int max_depth = 0;
     unsigned number_of_kbs = handle->flash_handle->kb_count;
 
     for (unsigned i = 0; i < number_of_kbs; i++) {
         const chaintree_kb_info_t *kb = &handle->flash_handle->kb_table[i];
         if (kb->max_depth > max_depth) {
             max_depth = kb->max_depth;
         }
     }
 
     return max_depth + 1;
 }
 
 uint16_t cfl_calculate_arrena_number(const chaintree_handle_t *flash_handle) {
    printf("calculating arena number\n");
     int index = ct_get_main_function_index(flash_handle, "CFL_LOCAL_ARENA_MAIN");
     if (index == -1) {
         return flash_handle->kb_count+1;
     }
     unsigned count = flash_handle->main_function_usage_count[index];
     return flash_handle->kb_count + count + 1;
 }
 
 static void cfl_find_main_ids(cfl_runtime_handle_t *handle) {
     const chaintree_handle_t *fh = handle->flash_handle;
     uint16_t *ids = handle->main_function_data->main_function_ids;
 
     ids[CFL_FUNCTION_ID_STATE_MACHINE]          = ct_get_main_function_index(fh, "CFL_STATE_MACHINE_MAIN");
     ids[CFL_FUNCTION_ID_SEQUENCE_TRY_PASS]      = ct_get_main_function_index(fh, "CFL_SEQUENCE_PASS_MAIN");
     ids[CFL_FUNCTION_ID_SEQUENCE_TRY_FAIL]      = ct_get_main_function_index(fh, "CFL_SEQUENCE_FAIL_MAIN");
     ids[CFL_FUNCTION_ID_SUPERVISOR_MAIN]         = ct_get_main_function_index(fh, "CFL_SUPERVISOR_MAIN");
     ids[CFL_FUNCTION_ID_EXCEPTION_CATCH_ALL_MAIN]= ct_get_main_function_index(fh, "CFL_EXCEPTION_CATCH_ALL_MAIN");
     ids[CFL_FUNCTION_ID_EXCEPTION_CATCH_MAIN]    = ct_get_main_function_index(fh, "CFL_EXCEPTION_CATCH_MAIN");
     ids[CFL_FUNCTION_ID_CONTROLLED_NODE_MAIN]    = ct_get_main_function_index(fh, "CFL_CONTROLLED_NODE_MAIN");
     ids[CFL_FUNCTION_ID_JSON_CONTROLLED_NODE_MAIN] = ct_get_main_function_index(fh, "CFL_JSON_CONTROLLED_NODE_MAIN");
     ids[CFL_FUNCTION_ID_CBOR_CONTROLLED_NODE_MAIN] = ct_get_main_function_index(fh, "CFL_CBOR_CONTROLLED_NODE_MAIN");
 }
 
 static void process_stop_start_tests(cfl_runtime_handle_t *handle,
     cfl_start_stop_tests_fn_data_t *ptr) {
 
     for (uint16_t i = 0; i < ptr->stop_tests_length; i++) {
         cfl_delete_test_by_index(handle, ptr->stop_tests[i]);
     }
     for (uint16_t i = 0; i < ptr->start_tests_length; i++) {
         cfl_add_test_by_index(handle, ptr->start_tests[i]);
     }
 }