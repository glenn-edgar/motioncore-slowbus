#ifndef CFL_SM_FUNCTIONS_H
#define CFL_SM_FUNCTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cfl_runtime.h"

void cfl_change_state(cfl_runtime_handle_t *handle, uint16_t node_index, int32_t sm_node_id, const char *new_state, bool sync_flag, int32_t sync_event_id);
void cfl_terminate_state_machine(cfl_runtime_handle_t *handle, uint16_t node_index, int32_t sm_node_id);
void cfl_reset_state_machine(cfl_runtime_handle_t *handle, uint16_t node_index, int32_t sm_node_id);




#ifdef __cplusplus
}
#endif

#endif

