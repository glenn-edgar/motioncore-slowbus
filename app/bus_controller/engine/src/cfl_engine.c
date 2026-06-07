/**
 * @file cfl_engine.c
 * @brief ChainTree Engine — tree walking, node execution, flag management
 */

 #include <stdio.h>
 #include <stdlib.h>
 #include <stdint.h>
 #include <stdbool.h>
 #include <limits.h>
 #include "cfl_engine.h"
 #include "cfl_common_functions.h"
 /* cfl_engine.h provides CT_Tree_Walker.h transitively */
 
 /*==============================================================================
  * FLAG DEFINITIONS
  *
  * CT_FLAG_USER1 (0x20) - Mark for termination flag (used during tree termination)
  * CT_FLAG_USER2 (0x40) - Node initialization flag (set after init/aux functions called)
  * CT_FLAG_USER3 (0x80) - Node enabled flag (must be set for node to execute)
  *============================================================================*/
 
 /* ========================================================================
  * FORWARD DECLARATIONS — STATIC HELPERS
  * ======================================================================== */
 
 static void cfl_disable_node(cfl_runtime_handle_t *handle, unsigned node_index);
 static void cfl_disable_all_node_flags(cfl_runtime_handle_t *handle);
 static void cfl_set_node_initialization_flag(cfl_runtime_handle_t *handle, unsigned node_index);
 static void cfl_reset_node_id(cfl_runtime_handle_t *handle, unsigned parent_id);
 
 static unsigned int cfl_get_forward_enabled_links(void *user_handle, unsigned int node_id,
     unsigned int *links_out, unsigned int max_links);
 static CT_ReturnCode cfl_execute_node(void *user_handle, unsigned int node_id,
     unsigned int level, uint8_t *flags);
 
 /* ========================================================================
  * PUBLIC API — ENGINE LIFECYCLE
  * ======================================================================== */
 
 void cfl_engine_create(cfl_runtime_handle_t *handle) {
     if (!handle) {
         EXCEPTION("cfl_engine_create: handle is NULL");
     }
     if (!handle->walker) {
         EXCEPTION("cfl_engine_create: walker is NULL");
     }
     if (!handle->flash_handle) {
         EXCEPTION("cfl_engine_create: flash_handle is NULL");
     }
     if (!handle->flags) {
         EXCEPTION("cfl_engine_create: flags is NULL");
     }
 
     ct_walker_init(handle->walker, handle->flash_handle->node_count, handle->flags,
                    cfl_get_forward_enabled_links, cfl_execute_node);
 }
 
 void cfl_engine_init(cfl_runtime_handle_t *handle) {
     if (!handle) {
         EXCEPTION("cfl_engine_init: handle is NULL");
     }
     cfl_disable_all_node_flags(handle);
 }
 
 void cfl_engine_init_test(cfl_runtime_handle_t *handle, unsigned start_node, unsigned node_count) {
     if (!handle) {
         EXCEPTION("cfl_engine_init_test: handle is NULL");
     }
     if (!handle->flash_handle) {
         EXCEPTION("cfl_engine_init_test: flash_handle is NULL");
     }
     if (start_node >= handle->flash_handle->node_count) {
         EXCEPTION("cfl_engine_init_test: start_node out of bounds");
     }
     if (node_count > 0 && start_node > handle->flash_handle->node_count - node_count) {
         EXCEPTION("cfl_engine_init_test: range exceeds node_count");
     }
 
     for (unsigned i = start_node; i < start_node + node_count; i++) {
         cfl_disable_node_flag(handle, i);
     }
     cfl_enable_node(handle, start_node);
 }
 
 /* ========================================================================
  * PUBLIC API — EVENT EXECUTION
  * ======================================================================== */
 
 bool cfl_execute_event(cfl_runtime_handle_t *handle) {
     if (!handle) {
         EXCEPTION("cfl_execute_event: handle is NULL");
     }
     if (!handle->event_data_ptr) {
         EXCEPTION("cfl_execute_event: event_data_ptr is NULL");
     }
     if (!handle->walker) {
         EXCEPTION("cfl_execute_event: walker is NULL");
     }
     if (!handle->flash_handle) {
         EXCEPTION("cfl_execute_event: flash_handle is NULL");
     }
     if (!handle->stack) {
         EXCEPTION("cfl_execute_event: stack is NULL");
     }
 
     unsigned node_index = handle->event_data_ptr->node_id;
 
     if (node_index < handle->kb_start_index) {
         EXCEPTION("cfl_execute_event: node_id out of bounds too low");
     }
     if (handle->kb_node_count > 0 &&
         handle->kb_start_index > UINT_MAX - handle->kb_node_count) {
         EXCEPTION("cfl_execute_event: kb range calculation overflow");
     }
     if (node_index >= handle->kb_start_index + handle->kb_node_count) {
         EXCEPTION("cfl_execute_event: node_id out of bounds too high");
     }
     if (node_index >= handle->flash_handle->node_count) {
         EXCEPTION("cfl_execute_event: node_id out of bounds");
     }
 
     if (!cfl_engine_node_is_enabled(handle, node_index)) {
         return false;
     }
 
     handle->cfl_engine_flag = true;
     handle->cfl_node_execution_count = 0;
     handle->node_start_index = node_index;
 
     ct_walker_walk(
         handle->walker,
         handle,
         node_index,
         handle->stack,
         handle->max_level,
         handle->walker->max_level,
         handle->flash_handle->node_count
     );
 
     if ((handle->cfl_node_execution_count == 0) || (handle->cfl_engine_flag == false)) {
         handle->cfl_engine_flag = false;
     }
 
     return handle->cfl_engine_flag;
 }
 
 /* ========================================================================
  * TREE WALKER CALLBACKS
  * ======================================================================== */
 
 static CT_ReturnCode cfl_execute_node(void *user_handle, unsigned int node_id,
     unsigned int level, uint8_t *flags) {
     (void)flags;
     (void)level;
 
     cfl_runtime_handle_t *handle = (cfl_runtime_handle_t *)user_handle;
 
     if (!handle || !handle->flash_handle) {
         EXCEPTION("cfl_execute_node: invalid handle");
         return CT_STOP_ALL;
     }
     if (node_id >= handle->flash_handle->node_count) {
         EXCEPTION("cfl_execute_node: node_id out of bounds");
         return CT_STOP_ALL;
     }
 
     const chaintree_node_t *node = &handle->flash_handle->nodes[node_id];
 
     if (node->main_function_index >= handle->flash_handle->main_function_count) {
         EXCEPTION("cfl_execute_node: main_function_index out of bounds");
         return CT_STOP_ALL;
     }
     if (node->init_function_index >= handle->flash_handle->one_shot_function_count) {
         EXCEPTION("cfl_execute_node: init_function_index out of bounds");
         return CT_STOP_ALL;
     }
     if (node->aux_function_index >= handle->flash_handle->boolean_function_count) {
         EXCEPTION("cfl_execute_node: aux_function_index out of bounds");
         return CT_STOP_ALL;
     }
 
     const main_function_t main_function = handle->flash_handle->main_functions[node->main_function_index];
     const one_shot_function_t one_shot_function = handle->flash_handle->one_shot_functions[node->init_function_index];
     const boolean_function_t boolean_function = handle->flash_handle->boolean_functions[node->aux_function_index];
 
     if (!cfl_engine_node_is_enabled(handle, node_id)) {
         return CT_SKIP_CHILDREN;
     }
 
     handle->cfl_node_execution_count++;
 
     /* Initialize node if not already initialized */
     if (!cfl_engine_node_is_initialized(handle, node_id)) {
         if (node->init_function_index != 0) {
             one_shot_function(handle, node_id);
         }
         if (node->aux_function_index != 0) {
             boolean_function(handle, node_id, CFL_EVENT_TYPE_NULL, CFL_INIT_EVENT, NULL);
         }
         cfl_set_node_initialization_flag(handle, node_id);
     }
 
     if (!handle->event_data_ptr) {
         EXCEPTION("cfl_execute_node: event_data_ptr is NULL");
         return CT_STOP_ALL;
     }
 
     /* Execute main function */
     unsigned return_code = main_function(handle, node->aux_function_index, node_id,
                                         handle->event_data_ptr->event_type,
                                         handle->event_data_ptr->event_id,
                                         (void *)handle->event_data_ptr->data.ptr);
 
     handle->cfl_engine_flag = true;
 
     switch (return_code) {
     case CFL_CONTINUE:
         return CT_CONTINUE;
 
     case CFL_HALT:
         return CT_STOP_SIBLINGS;
 
     case CFL_RESET:
         cfl_terminate_node_tree(handle, node->parent_index);
         cfl_reset_node_id(handle, node->parent_index);
         return CT_CONTINUE;
 
     case CFL_DISABLE:
         cfl_terminate_node_tree(handle, node_id);
         return CT_SKIP_CHILDREN;
 
     case CFL_SKIP_CONTINUE:
         return CT_SKIP_CHILDREN;
 
     case CFL_TERMINATE:
         if (node->parent_index != 0xffff) {
             cfl_terminate_node_tree(handle, node->parent_index);
             return CT_SKIP_CHILDREN;
         }
         cfl_disable_node(handle, node_id);
         return CT_STOP_ALL;
 
     case CFL_TERMINATE_SYSTEM:
         handle->cfl_engine_flag = false;
         cfl_terminate_node_tree(handle, handle->node_start_index);
         return CT_STOP_ALL;
 
     default:
         EXCEPTION("cfl_execute_node: invalid return code");
         return CT_STOP_ALL;
     }
 }
 
 static unsigned int cfl_get_forward_enabled_links(void *user_handle, unsigned int node_id,
     unsigned int *links_out, unsigned int max_links) {
 
     cfl_runtime_handle_t *handle = (cfl_runtime_handle_t *)user_handle;
 
     if (!handle || !handle->flash_handle) {
         EXCEPTION("cfl_get_forward_enabled_links: invalid handle");
         return 0;
     }
     if (node_id >= handle->flash_handle->node_count) {
         EXCEPTION("cfl_get_forward_enabled_links: node_id out of bounds");
         return 0;
     }
 
     const chaintree_node_t *node = &handle->flash_handle->nodes[node_id];
     uint16_t link_start = node->link_start;
     uint16_t link_count = (node->link_count & LINK_COUNT_MASK);
 
     if (link_count > 0) {
         if (link_start >= handle->flash_handle->link_table_size) {
             EXCEPTION("cfl_get_forward_enabled_links: link_start out of bounds");
             return 0;
         }
         if (link_start + link_count > handle->flash_handle->link_table_size) {
             EXCEPTION("cfl_get_forward_enabled_links: link range exceeds table size");
             return 0;
         }
     }
 
     const uint16_t *link_table = handle->flash_handle->link_table;
     unsigned int return_value = 0;
 
     for (unsigned i = 0; i < link_count; i++) {
         if (return_value >= max_links) {
             EXCEPTION("cfl_get_forward_enabled_links: max_links exceeded");
             return 0;
         }
 
         unsigned int link_id = link_table[link_start + i];
         if (link_id >= handle->flash_handle->node_count) {
             EXCEPTION("cfl_get_forward_enabled_links: link_id out of bounds");
             return 0;
         }
 
         links_out[return_value] = link_id;
         return_value++;
     }
 
     return return_value;
 }
 
 /* ========================================================================
  * NODE STATE QUERIES
  * ======================================================================== */
 
 bool cfl_engine_node_is_enabled(cfl_runtime_handle_t *handle, unsigned node_index) {
     if (!handle || !handle->flags || !handle->flash_handle) {
         EXCEPTION("cfl_engine_node_is_enabled: invalid handle");
         return false;
     }
     if (node_index >= handle->flash_handle->node_count) {
         EXCEPTION("cfl_engine_node_is_enabled: node_index out of bounds");
         return false;
     }
     return (handle->flags[node_index] & CT_FLAG_USER3) != 0;
 }
 
 bool cfl_engine_node_is_initialized(cfl_runtime_handle_t *handle, unsigned node_index) {
     if (!handle || !handle->flags || !handle->flash_handle) {
         EXCEPTION("cfl_engine_node_is_initialized: invalid handle");
         return false;
     }
     if (node_index >= handle->flash_handle->node_count) {
         EXCEPTION("cfl_engine_node_is_initialized: node_index out of bounds");
         return false;
     }
     uint8_t f = handle->flags[node_index];
     return (f & CT_FLAG_USER2) && (f & CT_FLAG_USER3);
 }
 
 /* ========================================================================
  * NODE DISABLE — single implementation
  *
  * Teardown order: aux(TERMINATE) first, then term function.
  * This lets the aux/boolean function clean up state that the
  * termination one-shot may depend on.
  *
  * NOTE: The previous codebase had two variants (cfl_disable_node and
  * cfl_disable_kb_nodes) with OPPOSITE call ordering. This unified
  * version uses aux-first ordering. If kb-level teardown genuinely
  * needs term-first, split back into two functions with a comment
  * explaining why.
  * ======================================================================== */
 
 static void cfl_disable_node(cfl_runtime_handle_t *handle, unsigned node_index) {
     if (!handle || !handle->flags || !handle->flash_handle) {
         EXCEPTION("cfl_disable_node: invalid handle");
         return;
     }
     if (node_index >= handle->flash_handle->node_count) {
         EXCEPTION("cfl_disable_node: node_index out of bounds");
         return;
     }
 
     /* Only run teardown if node is both enabled and initialized */
     if ((handle->flags[node_index] & (CT_FLAG_USER3 | CT_FLAG_USER2)) !=
         (CT_FLAG_USER3 | CT_FLAG_USER2)) {
         cfl_disable_node_flag(handle, node_index);
         return;
     }
 
     const chaintree_node_t *node = &handle->flash_handle->nodes[node_index];
 
     /* Aux/boolean terminate first */
     if (node->aux_function_index != 0) {
         if (node->aux_function_index >= handle->flash_handle->boolean_function_count) {
             EXCEPTION("cfl_disable_node: aux_function_index out of bounds");
             return;
         }
         handle->flash_handle->boolean_functions[node->aux_function_index](
             handle, node_index, CFL_EVENT_TYPE_NULL, CFL_TERMINATE_EVENT, NULL);
     }
 
     /* Term one-shot second */
     if (node->term_function_index != 0) {
         if (node->term_function_index >= handle->flash_handle->one_shot_function_count) {
             EXCEPTION("cfl_disable_node: term_function_index out of bounds");
             return;
         }
         handle->flash_handle->one_shot_functions[node->term_function_index](handle, node_index);
     }
 
     cfl_disable_node_flag(handle, node_index);
 }
 
 /* ========================================================================
  * NODE TERMINATION — tree and KB-level
  * ======================================================================== */
 
 static CT_ReturnCode cfl_mark_node_for_termination(void *handle, unsigned node_index,
     unsigned int level, uint8_t *flags) {
     (void)level;
 
     cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
 
     if (!runtime_handle || !runtime_handle->backup_flags || !runtime_handle->flash_handle) {
         EXCEPTION("cfl_mark_node_for_termination: invalid handle");
         return CT_STOP_ALL;
     }
     if (node_index >= runtime_handle->flash_handle->node_count) {
         EXCEPTION("cfl_mark_node_for_termination: node_index out of bounds");
         return CT_STOP_ALL;
     }
 
     if (flags[node_index] & CT_FLAG_USER3) {
         runtime_handle->backup_flags[node_index] |= CT_FLAG_USER1;
     }
 
     return CT_CONTINUE;
 }
 
 void cfl_terminate_node_tree(cfl_runtime_handle_t *handle, unsigned node_id) {
     /* FIX: null checks BEFORE any dereference */
     if (!handle || !handle->flash_handle) {
         EXCEPTION("cfl_terminate_node_tree: invalid handle");
         return;
     }
     if (node_id >= handle->flash_handle->node_count) {
         EXCEPTION("cfl_terminate_node_tree: node_id out of bounds");
         return;
     }
 
     /* Early out if node was never initialized */
     if ((handle->flags[node_id] & CT_FLAG_USER2) == 0) {
         return;
     }
 
     const chaintree_node_t *node = &handle->flash_handle->nodes[node_id];
 
     /* Leaf node — disable directly */
     if ((node->link_count & LINK_COUNT_MASK) == 0) {
         cfl_disable_node(handle, node_id);
         return;
     }
 
     if (!handle->walker || !handle->nested_stack || !handle->walker_context_ptr ||
         !handle->backup_flags) {
         EXCEPTION("cfl_terminate_node_tree: required handle members are NULL");
         return;
     }
 
     ct_walker_save_context(handle->walker, handle->walker_context_ptr, handle->backup_flags);
 
     ct_walker_update_functions(handle->walker, cfl_mark_node_for_termination,
         cfl_get_forward_enabled_links);
 
     ct_walker_walk(
         handle->walker,
         handle,
         node_id,
         handle->nested_stack,
         handle->max_level,
         handle->walker->max_level,
         handle->flash_handle->node_count
     );
 
     ct_walker_restore_context(handle->walker, handle->walker_context_ptr);
 
     if (handle->kb_node_count == 0) {
         return;
     }
     if (handle->kb_start_index > UINT_MAX - handle->kb_node_count) {
         EXCEPTION("cfl_terminate_node_tree: kb range calculation overflow");
         return;
     }
 
     unsigned int end_index = handle->kb_start_index + handle->kb_node_count - 1;
     if (end_index >= handle->flash_handle->node_count) {
         EXCEPTION("cfl_terminate_node_tree: end_index out of bounds");
         return;
     }
     if (node_id < handle->kb_start_index) {
         EXCEPTION("cfl_terminate_node_tree: node_id below kb_start_index");
         return;
     }
 
     /* Terminate marked nodes in reverse order */
     for (int i = (int)end_index; i >= (int)node_id; i--) {
         if (handle->flags[i] & CT_FLAG_USER1) {
             handle->flags[i] &= ~CT_FLAG_USER1;
             cfl_disable_node(handle, i);
         }
     }
 }
 
 void cfl_terminate_all_nodes_in_kb(cfl_runtime_handle_t *handle, unsigned start_node,
                                    unsigned node_count) {
     /* Uses same cfl_disable_node — unified teardown ordering */
     for (int i = (int)start_node + node_count - 1; i >= (int)start_node; i--) {
         cfl_disable_node(handle, i);
     }
 }
 
 static void cfl_reset_node_id(cfl_runtime_handle_t *handle, unsigned node_id) {
     if (!handle) {
         EXCEPTION("cfl_reset_node_id: handle is NULL");
         return;
     }
     cfl_enable_node(handle, node_id);
 }
 
 /* ========================================================================
  * SEQUENCE TRY-NODE DISCOVERY
  * ======================================================================== */
 
 static CT_ReturnCode cfl_find_try_mark_node(void *handle, unsigned node_index,
     unsigned int level, uint8_t *flags) {
     (void)level;
     (void)flags;
 
     cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
     const chaintree_node_t *node = &runtime_handle->flash_handle->nodes[node_index];
     uint16_t main_id = node->main_function_index;
 
     if (main_id == runtime_handle->main_function_data->main_function_ids[CFL_FUNCTION_ID_SEQUENCE_TRY_PASS] ||
         main_id == runtime_handle->main_function_data->main_function_ids[CFL_FUNCTION_ID_SEQUENCE_TRY_FAIL]) {
         runtime_handle->backup_flags[node_index] |= CT_FLAG_USER1;
         return CT_STOP_LEVEL;
     }
 
     return CT_CONTINUE;
 }
 
 void cfl_find_try_node_indexes(cfl_runtime_handle_t *handle, unsigned node_index,
                                sequence_aggregate_data_t *sequence_aggregate_data) {
     ct_walker_save_context(handle->walker, handle->walker_context_ptr, handle->backup_flags);
 
     ct_walker_update_functions(handle->walker, cfl_find_try_mark_node,
         cfl_get_forward_enabled_links);
 
     ct_walker_walk(
         handle->walker,
         handle,
         node_index,
         handle->nested_stack,
         handle->max_level,
         handle->walker->max_level,
         handle->flash_handle->node_count
     );
 
     ct_walker_restore_context(handle->walker, handle->walker_context_ptr);
 
     /* Count marked nodes */
     sequence_aggregate_data->try_node_count = 0;
     unsigned int end_index = handle->kb_start_index + handle->kb_node_count - 1;
     for (int i = (int)end_index; i >= (int)node_index; i--) {
         if (handle->flags[i] & CT_FLAG_USER1) {
             sequence_aggregate_data->try_node_count++;
         }
     }
 
     /* Allocate index array */
     sequence_aggregate_data->try_node_indexes =
         (uint16_t *)cfl_additional_arena_alloc(handle, node_index,
             sequence_aggregate_data->try_node_count * sizeof(uint16_t));
     if (!sequence_aggregate_data->try_node_indexes) {
         EXCEPTION("cfl_find_try_node_indexes: failed to allocate try_node_indexes");
         return;
     }
 
     /* Collect marked indexes */
     int j = 0;
     for (int i = (int)end_index; i >= (int)node_index; i--) {
         if (handle->flags[i] & CT_FLAG_USER1) {
             handle->flags[i] &= ~CT_FLAG_USER1;
             sequence_aggregate_data->try_node_indexes[j] = i;
             j++;
         }
     }
 }
 
 /* ========================================================================
  * FLAG MANIPULATION
  * ======================================================================== */
 
 static void cfl_disable_all_node_flags(cfl_runtime_handle_t *handle) {
     if (!handle || !handle->flags || !handle->flash_handle) {
         EXCEPTION("cfl_disable_all_node_flags: invalid handle");
         return;
     }
 
     uint8_t *flags = handle->flags;
     for (unsigned i = 0; i < handle->flash_handle->node_count; i++) {
         flags[i] &= ~CT_FLAG_USER_MASK;
     }
 }
 
 void cfl_enable_node(cfl_runtime_handle_t *handle, unsigned node_index) {
     if (!handle || !handle->flags || !handle->flash_handle) {
         EXCEPTION("cfl_enable_node: invalid handle");
         return;
     }
     if (node_index >= handle->flash_handle->node_count) {
         EXCEPTION("cfl_enable_node: node_index out of bounds");
         return;
     }
 
     uint8_t *flags = handle->flags;
     flags[node_index] &= ~CT_FLAG_USER_MASK;
     flags[node_index] |= CT_FLAG_USER3;
 }
 
 void cfl_disable_node_flag(cfl_runtime_handle_t *handle, unsigned node_index) {
     if (!handle || !handle->flags || !handle->flash_handle) {
         EXCEPTION("cfl_disable_node_flag: invalid handle");
         return;
     }
     if (node_index >= handle->flash_handle->node_count) {
         EXCEPTION("cfl_disable_node_flag: node_index out of bounds");
         return;
     }
 
     handle->flags[node_index] &= ~CT_FLAG_USER_MASK;
 }
 
 static void cfl_set_node_initialization_flag(cfl_runtime_handle_t *handle, unsigned node_index) {
     if (!handle || !handle->flags || !handle->flash_handle) {
         EXCEPTION("cfl_set_node_initialization_flag: invalid handle");
         return;
     }
     if (node_index >= handle->flash_handle->node_count) {
         EXCEPTION("cfl_set_node_initialization_flag: node_index out of bounds");
         return;
     }
 
     handle->flags[node_index] |= CT_FLAG_USER2;
 }
 
 /* ========================================================================
  * MEMORY ALLOCATOR ASSIGNMENT VIA TREE WALK
  * ======================================================================== */
 
 static CT_ReturnCode cfl_memory_allocator_node_assignment(void *handle, unsigned node_index,
     unsigned int level, uint8_t *flags) {
     (void)level;
     (void)flags;
 
     cfl_runtime_handle_t *runtime_handle = (cfl_runtime_handle_t *)handle;
     cfl_heap_arena_set_node_allocator_id(runtime_handle->arena_system, node_index,
                                          runtime_handle->allocator_id);
     cfl_heap_arena_set_node_memory_index(runtime_handle->arena_system, node_index, 0xFFFF);
     return CT_CONTINUE;
 }
 
 void cfl_memory_allocator_assignment(cfl_runtime_handle_t *handle, unsigned node_index,
                                      cfl_heap_allocator_id_t allocator_id) {
     handle->allocator_id = allocator_id;
     ct_walker_save_context(handle->walker, handle->walker_context_ptr, handle->backup_flags);
 
     ct_walker_update_functions(handle->walker, cfl_memory_allocator_node_assignment,
         cfl_get_forward_enabled_links);
 
     ct_walker_walk(
         handle->walker,
         handle,
         node_index,
         handle->nested_stack,
         handle->max_level,
         handle->walker->max_level,
         handle->flash_handle->node_count
     );
 
     ct_walker_restore_context(handle->walker, handle->walker_context_ptr);
 }