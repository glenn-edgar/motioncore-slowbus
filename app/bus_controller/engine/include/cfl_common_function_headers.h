#ifndef CFL_COMMON_FUNCTION_HEADERS_H
#define CFL_COMMON_FUNCTION_HEADERS_H

#ifdef __cplusplus
extern "C" {
#endif


typedef struct {
    double wait_time_out;
} cfl_wait_time_out_data_t;


typedef struct{
    char *error_message;
    uint32_t timeout;
    uint32_t time_out_event;
    uint32_t error_function;
    bool reset_flag;
    uint32_t event_count;
    void *auxiliary_data;
}cfl_wait_fn_data_t;



typedef struct{
  bool reset_flag;
  uint32_t error_function;
  void *auxiliary_data;
}cfl_verify_fn_data_t;


typedef struct{
  char *event_logger_message;
  uint32_t event_count;
  int32_t *event_ids;
}cfl_event_logger_fn_data_t;


typedef struct {

  const char **state_names;               // Array of pointers to strings                 // Array of state link node IDs
  int32_t current_state;
  int32_t new_state;
  int32_t sync_event_id;
  bool sync_event_id_valid;
  bool sync_occured;
} cfl_state_machine_column_data_t;





typedef struct {
  uint16_t sequence_result;
  uint16_t node_index;
}sequence_result_data_t;


typedef struct {
  int32_t finalize_function_id;
  int32_t sequence_number;
  int32_t recorded_sequence_index;
  int32_t current_sequence_index;
  sequence_result_data_t *sequence_result_data_array;
  void *auxiliary_data;
  bool  final_status;
  uint8_t sequence_type;
}sequence_start_fn_data_t;




typedef struct {
  uint32_t last_tick;
  int16_t  node_id;
  uint8_t  bucket;           // failure "credits"
  bool    active_node;
} cfl_supervisor_failure_t;


typedef struct cfl_supervisor_data_t{
  cfl_supervisor_failure_t  *supervisor_failure_array;
  int32_t max_reset_number;
  int32_t reset_window;
  int32_t finalize_function_id;
  int32_t termination_type;
  int32_t now_tick;
  uint8_t failed_link_index;
  bool reset_limited_enabled;
  bool restart_enabled;
} cfl_supervisor_data_t;


typedef struct {
  int32_t number_of_iterations;
  int32_t current_iteration;
} cfl_for_fn_data_t;

typedef struct {
  int32_t current_iteration;
  void *auxiliary_data;
} cfl_while_fn_data_t;


typedef struct {
  int32_t current_count;
  int32_t wd_time_count;
  int32_t wd_fn_id;
  bool wd_enabled;
  bool wd_reset;
} cfl_watch_dog_fn_data_t;
  
typedef struct {
  int32_t required_bitmask;
  int32_t excluded_bitmask;
  bool node_state;
} cfl_df_mask_fn_data_t;




#ifdef __cplusplus
}
#endif

#endif
