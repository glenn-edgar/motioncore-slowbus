/*
 * cfl_function_loader.h - Register all ChainTree functions with binary image loader
 *
 * Registration name convention: C symbol minus "_fn", uppercased.
 * Names must match the function name strings in the .ctb binary image.
 */

 #ifndef CFL_FUNCTION_LOADER_H
 #define CFL_FUNCTION_LOADER_H
 
 #include "cfl_image_loader.h"
 
 /* Register all main functions (38 functions) */
 void cfl_register_all_main_functions(cfl_image_loader_t *img);
 
 /* Register all one-shot functions (82 functions) */
 void cfl_register_all_one_shot_functions(cfl_image_loader_t *img);
 
 /* Register all boolean functions (13 functions) */
 void cfl_register_all_boolean_functions(cfl_image_loader_t *img);
 
 /* Register everything + validate. Returns missing count (0 = all good). */
 void cfl_register_all_functions(cfl_image_loader_t *img);
 
 #endif /* CFL_FUNCTION_LOADER_H */