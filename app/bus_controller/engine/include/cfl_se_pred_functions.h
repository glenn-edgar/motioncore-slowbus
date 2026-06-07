/*
 * cfl_se_pred_functions.h - ChainTree bridge predicate functions for S-Engine
 *
 * S-engine predicate functions that read ChainTree runtime bitmask.
 * Registered as the "cfl" layer (layer 1) during module load.
 *
 * Functions that duplicate s-engine builtins have been removed:
 *   CFL_TRUE        → use se_true
 *   CFL_FALSE       → use se_false
 *   CFL_CHECK_EVENT → use se_check_event
 */

#ifndef CFL_SE_PRED_FUNCTIONS_H
#define CFL_SE_PRED_FUNCTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "s_engine_types.h"

const s_expr_fn_table_t *cfl_se_get_pred_table(void);

#ifdef __cplusplus
}
#endif

#endif /* CFL_SE_PRED_FUNCTIONS_H */
