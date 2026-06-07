/*
 * cfl_se_main_functions.h - ChainTree bridge main functions for S-Engine
 *
 * S-engine main functions that require ChainTree runtime access.
 * Registered as the "cfl" layer (layer 1) during module load.
 */

#ifndef CFL_SE_MAIN_FUNCTIONS_H
#define CFL_SE_MAIN_FUNCTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "s_engine_types.h"

const s_expr_fn_table_t *cfl_se_get_main_table(void);

#ifdef __cplusplus
}
#endif

#endif /* CFL_SE_MAIN_FUNCTIONS_H */
