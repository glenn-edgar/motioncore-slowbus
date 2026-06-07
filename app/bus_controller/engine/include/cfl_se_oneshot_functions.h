/*
 * cfl_se_oneshot_functions.h - ChainTree bridge oneshot functions for S-Engine
 *
 * These are s-engine oneshot functions that call back into ChainTree:
 * node control, event queue, bitmask, JSON decode, exception, logging.
 * Registered as the "cfl" layer (layer 1) during module load.
 */

#ifndef CFL_SE_ONESHOT_FUNCTIONS_H
#define CFL_SE_ONESHOT_FUNCTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "s_engine_types.h"

const s_expr_fn_table_t *cfl_se_get_oneshot_table(void);

#ifdef __cplusplus
}
#endif

#endif /* CFL_SE_ONESHOT_FUNCTIONS_H */
