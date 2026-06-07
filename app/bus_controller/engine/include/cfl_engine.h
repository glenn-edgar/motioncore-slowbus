#ifndef CFL_ENGINE_H
#define CFL_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cfl_perm.h"
#include "cfl_heap.h"
#include "cfl_heap_arena_allocate.h"
#include "cfl_event_queue.h"
#include "cfl_timer_system.h"
#include "chaintree_support.h"
#include "CT_Tree_Walker.h"


/* Forward declaration for blackboard descriptor */
typedef struct cfl_bb_record cfl_bb_record_t;

/* ========================================================================
 * ENGINE EVENT DEFINITIONS
 * ======================================================================== */

typedef enum {
    CFL_INIT_EVENT = 0,
    CFL_TERMINATE_EVENT = 1,
    CFL_START_TESTS = 2,
    CFL_TERMINATE_TESTS = 3,
    CFL_TIMER_EVENT = 4,
    CFL_SECOND_EVENT = 5,
    CFL_MINUTE_EVENT = 6,
    CFL_HOUR_EVENT = 7,
    CFL_DAY_EVENT = 8,
    CFL_WEEK_EVENT = 9,
    CFL_MONTH_EVENT = 10,
    CFL_YEAR_EVENT = 11,
    CFL_RAISE_EXCEPTION_EVENT = 12,
    CFL_TURN_HEARTBEAT_ON_EVENT = 13,
    CFL_TURN_HEARTBEAT_OFF_EVENT = 14,
    CFL_HEARTBEAT_EVENT = 15,
    CFL_SET_EXCEPTION_STEP_EVENT = 16,
    CFL_CHANGE_STATE_EVENT = 17,
    CFL_RESET_STATE_MACHINE_EVENT = 18,
    CFL_TERMINATE_STATE_MACHINE_EVENT = 19,
} cfl_engine_event_t;

#define CFL_TERMINATE_SYSTEM_EVENT 0xFFFF
#define CFL_STOP_START_TESTS_EVENT 0xFFF0

/* Chain Tree Main Function Return Codes */
#define CFL_CONTINUE 0
#define CFL_HALT 1
#define CFL_TERMINATE 2
#define CFL_RESET 3
#define CFL_DISABLE 4
#define CFL_SKIP_CONTINUE 5
#define CFL_TERMINATE_SYSTEM 6

/* ========================================================================
 * SUPPORT TYPES
 * ======================================================================== */

typedef struct {
    int32_t finalize_function_id;
    int32_t try_node_count;
    uint16_t *try_node_indexes;
    void *auxiliary_data;
} sequence_aggregate_data_t;

typedef struct {
    const json_record_t *records;
    uint32_t records_count;
    const char *strings;
    uint32_t strings_size;
    const record_control_t *controls;
    uint32_t controls_count;
    uint32_t current_control_idx;
    int error_code;
} json_decoder_ctx_t;

/* ========================================================================
 * FUNCTION ID CONSTANTS
 * ======================================================================== */

#define CFL_FUNCTION_ID_STATE_MACHINE 0
#define CFL_FUNCTION_ID_SEQUENCE_TRY_PASS 1
#define CFL_FUNCTION_ID_SEQUENCE_TRY_FAIL 2
#define CFL_FUNCTION_ID_SUPERVISOR_MAIN 3
#define CFL_FUNCTION_ID_EXCEPTION_CATCH_ALL_MAIN 4
#define CFL_FUNCTION_ID_EXCEPTION_CATCH_MAIN 5
#define CFL_FUNCTION_ID_CONTROLLED_NODE_MAIN 6
#define CFL_FUNCTION_ID_JSON_CONTROLLED_NODE_MAIN 7
#define CFL_FUNCTION_ID_CBOR_CONTROLLED_NODE_MAIN 8

typedef struct {
    uint16_t main_function_ids[9];
} main_function_data_t;

/* ========================================================================
 * RUNTIME HANDLE
 * ======================================================================== */

typedef struct CFL_RUNTIME_HANDLE cfl_runtime_handle_t;
struct CFL_RUNTIME_HANDLE {
    /* Memory management */
    cfl_perm_t *perm;
    cfl_heap_t *heap;
    cfl_heap_arena_system_t *arena_system;
    cfl_heap_allocator_id_t allocator_id;

    /* Event system */
    cfl_event_queue_t *event_queue;
    CFL_EVENT_DATA_T *event_data_ptr;

    /* Tree walker */
    CT_TreeWalker *walker;
    CT_StackEntry *stack;
    CT_StackEntry *nested_stack;
    uint8_t *flags;
    uint8_t *backup_flags;
    CT_WalkerContext *walker_context_ptr;

    /* Timing */
    cfl_timer_handle_t timer_handle;
    double delta_time;
    double future_time_stamp;

    /* Bitmask control */
    uint64_t shaddow_bitmask;
    uint64_t bitmask;

    /* Execution state */
    bool cfl_engine_flag;
    unsigned cfl_node_execution_count;
    unsigned node_start_index;
    unsigned kb_start_index;
    unsigned kb_node_count;
    unsigned kb_max_level;
    unsigned current_kb_idx;
    unsigned max_level;

    /* Test/KB management */
    unsigned test_count;
    uint32_t *active_test_bitmap;
    unsigned active_test_count;
    cfl_heap_allocator_id_t *kb_allocator_ids;
    uint8_t *test_has_arena;

    /* Function dispatch */
    main_function_data_t *main_function_data;

    /* Flash reference */
    const chaintree_handle_t *flash_handle;

    /* Shared blackboard (mutable, allocated from perm) */
    void *blackboard;
    uint16_t blackboard_size;
    const cfl_bb_record_t *bb_desc;

    /* Serialization (application layer) */
    json_decoder_ctx_t *json_decoder_ctx;

    /* User context */
    void *user_handle;
    unsigned app_extensions_count;
    void **app_extensions;
};

/* ========================================================================
 * ENGINE API
 * ======================================================================== */

void cfl_engine_create(cfl_runtime_handle_t *handle);
void cfl_engine_init(cfl_runtime_handle_t *handle);
void cfl_engine_init_test(cfl_runtime_handle_t *handle, unsigned start_node, unsigned node_count);
bool cfl_engine_node_is_enabled(cfl_runtime_handle_t *handle, unsigned node_index);
bool cfl_engine_node_is_initialized(cfl_runtime_handle_t *handle, unsigned node_index);
bool cfl_execute_event(cfl_runtime_handle_t *handle);
void cfl_enable_node(cfl_runtime_handle_t *handle, unsigned node_index);
void cfl_disable_node_flag(cfl_runtime_handle_t *handle, unsigned node_index);
void cfl_terminate_node_tree(cfl_runtime_handle_t *handle, unsigned node_id);
void cfl_find_try_node_indexes(cfl_runtime_handle_t *handle, unsigned node_index,
                               sequence_aggregate_data_t *sequence_aggregate_data);
void cfl_terminate_all_nodes_in_kb(cfl_runtime_handle_t *handle, unsigned start_node,
                                   unsigned node_count);
void cfl_memory_allocator_assignment(cfl_runtime_handle_t *handle, unsigned node_index,
                                     cfl_heap_allocator_id_t allocator_id);

/* Arena allocation — declared here so .c files don't need mid-file externs */
//void *cfl_additional_arena_alloc(cfl_runtime_handle_t *handle, unsigned node_index, uint16_t size);

/* ========================================================================
 * INLINE ACCESSORS
 * ======================================================================== */

static inline void cfl_set_user_handle(cfl_runtime_handle_t *handle, void *user_handle) {
    handle->user_handle = user_handle;
}

static inline void *cfl_get_user_handle(cfl_runtime_handle_t *handle) {
    return handle->user_handle;
}

static inline void cfl_set_app_extensions(cfl_runtime_handle_t *handle, void *extensions) {
    handle->app_extensions = extensions;
}

static inline void *cfl_get_app_extensions(cfl_runtime_handle_t *handle) {
    return handle->app_extensions;
}

#ifdef __cplusplus
}
#endif

#endif /* CFL_ENGINE_H */