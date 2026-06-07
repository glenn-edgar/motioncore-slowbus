#ifndef CFL_EXCEPTION_SUPPORT_H
#define CFL_EXCEPTION_SUPPORT_H

#ifdef __cplusplus
extern "C" {
#endif
// internal event
#define CFL_RECOVERY_CHECK_EVENT 0xFFFE

typedef enum {
    CFL_EXCEPTION_MAIN_LINK     = 0,
    CFL_EXCEPTION_RECOVERY_LINK = 1,
    CFL_EXCEPTION_FINALIZE_LINK = 2
} cfl_exception_stage_t;

typedef enum {
    CFL_RECOVERY_SEQ_EVAL,
    CFL_RECOVERY_SEQ_WAIT,
    CFL_RECOVERY_PARALLEL_ENABLE,
    CFL_RECOVERY_PARALLEL_WAIT
} cfl_recovery_state_t;

typedef enum {
    CFL_EXCEPTION_NONE = 0,
    CFL_EXCEPTION_RAISED,
    CFL_EXCEPTION_HEARTBEAT_TIMEOUT
} cfl_exception_type_t;

typedef struct{
    void    *logging_data;
    void    *auxiliary_data;
    uint16_t parent_node_id;
    uint16_t logging_function_id;
    uint16_t original_node_id;
    uint16_t heartbeat_time_out;
    uint16_t heartbeat_count;
    uint16_t exception_catch_links[CFL_EXCEPTION_FINALIZE_LINK + 1];
    cfl_exception_stage_t  exception_stage;
    cfl_exception_type_t   exception_type;
    uint8_t  max_steps;
    uint8_t  step_count;
    uint8_t  recovery_step_count;
    bool     heartbeat_enabled;
    
    uint8_t  current_step;
    cfl_recovery_state_t recovery_state;
    
} cfl_exception_support_data_t;

void cfl_raise_json_exception_event(cfl_runtime_handle_t *runtime_handle, uint16_t node_id, uint16_t parent_node_id);
void cfl_forward_exception_event(cfl_runtime_handle_t *runtime_handle, unsigned node_index , unsigned parent_node_id , 
    uint16_t record_index );
#ifdef __cplusplus
}
#endif

#endif