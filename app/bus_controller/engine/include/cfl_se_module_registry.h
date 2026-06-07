/*
 * cfl_se_module_registry.h - S-Engine module registry as ChainTree app extension
 *
 * Stores s_expr_module_t handles indexed by name hash.
 * All storage on cfl_heap — modules can be loaded and unloaded dynamically.
 */

#ifndef CFL_SE_MODULE_REGISTRY_H
#define CFL_SE_MODULE_REGISTRY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "cfl_engine.h"
#include "s_engine_module.h"
#include "s_engine_node.h"
#include "s_engine_loader.h"

#define CFL_SE_MAX_MODULES 8

/* Function table set — one per registration layer */
typedef struct {
    const s_expr_fn_table_t *main_tbl;
    const s_expr_fn_table_t *oneshot_tbl;
    const s_expr_fn_table_t *pred_tbl;
} cfl_se_fn_table_set_t;

#define CFL_SE_MAX_FN_LAYERS 2  /* builtins, cfl — user registers via callback */

/*
 * User registration callback — a ChainTree boolean function.
 * Called after builtins and cfl layers are registered, before validate.
 * The user calls s_expr_module_register_main/oneshot/pred inside it.
 * Return value is ignored.
 */
typedef void (*cfl_se_user_register_fn_t)(s_expr_module_t *mod, void *user_ctx);

typedef struct {
    uint32_t          name_hash;   /* FNV-1a of module name, 0 = empty slot */
    s_expr_module_t   module;      /* inline — no extra alloc for the struct */
} cfl_se_module_slot_t;

/* Module binary entry — registered by app before engine starts */
typedef struct {
    uint32_t        name_hash;    /* FNV-1a of module name, 0 = empty */
    const uint8_t  *binary_data;  /* raw binary ROM (from _bin_32.h) */
    size_t          binary_size;  /* size in bytes */
    cfl_se_user_register_fn_t user_register;  /* optional user fn registration */
    void           *user_ctx;     /* context for user_register */
} cfl_se_module_def_entry_t;

typedef struct {
    cfl_se_module_slot_t       slots[CFL_SE_MAX_MODULES];
    cfl_se_module_def_entry_t  defs[CFL_SE_MAX_MODULES];  /* app-registered defs */
    uint8_t                    count;
    uint8_t                    def_count;
    cfl_runtime_handle_t      *rt;     /* back-reference for allocator bridge */
} cfl_se_module_registry_t;

/* --------------------------------------------------------------------
 * Registry lifecycle
 * -------------------------------------------------------------------- */

/* Create registry on cfl_heap. Caller stores via cfl_set_app_extensions(). */
cfl_se_module_registry_t *cfl_se_registry_create(cfl_runtime_handle_t *rt);

/* Unload all modules and free the registry itself. */
void cfl_se_registry_destroy(cfl_se_module_registry_t *reg);

/* Register a module binary (ROM) by name.
 * Called by app before engine starts. The init one-shot resolves
 * JSON module_name → name_hash → binary via this table.
 * user_register is optional (NULL = no user functions). */
void cfl_se_registry_register_def(
    cfl_se_module_registry_t *reg,
    const char *module_name,
    const uint8_t *binary_data,
    size_t binary_size
);

/* Register with user function callback. */
void cfl_se_registry_register_def_with_user(
    cfl_se_module_registry_t *reg,
    const char *module_name,
    const uint8_t *binary_data,
    size_t binary_size,
    cfl_se_user_register_fn_t user_register,
    void *user_ctx
);

/* Find a registered module binary by name hash. */
const cfl_se_module_def_entry_t *cfl_se_registry_find_def(
    cfl_se_module_registry_t *reg, uint32_t name_hash
);

/* --------------------------------------------------------------------
 * Module load / unload / lookup
 * -------------------------------------------------------------------- */

/* Load a module into the next free slot.
 * Registers function tables from layers[] in order (builtins, cfl),
 * then calls user_register (if non-NULL) for user function registration,
 * then validates. Returns module pointer or NULL. */
s_expr_module_t *cfl_se_registry_load(
    cfl_se_module_registry_t *reg,
    const s_expr_module_def_t *def,
    const cfl_se_fn_table_set_t *layers,
    uint8_t layer_count,
    cfl_se_user_register_fn_t user_register,
    void *user_ctx
);

/* Unload a module by name hash. Frees internal function tables. */
void cfl_se_registry_unload(cfl_se_module_registry_t *reg, uint32_t name_hash);

/* Find a loaded module by name hash. Returns NULL if not found. */
s_expr_module_t *cfl_se_registry_find(cfl_se_module_registry_t *reg, uint32_t name_hash);

/* --------------------------------------------------------------------
 * ChainTree main/oneshot functions for module load node
 * -------------------------------------------------------------------- */

/* Node data for the se_module_load node (stored in heap arena) */
typedef struct {
    s_expr_loaded_module_t     *loaded_module;  /* parsed binary (owns allocations) */
    uint32_t                    name_hash;
    bool                        loaded;
} cfl_se_load_node_data_t;

/* Init one-shot: loads the module into the registry */
void cfl_se_module_load_init_one_shot_fn(void *handle, uint16_t node_index);

/* Main: null main — does nothing after init */
unsigned cfl_se_module_load_main_fn(
    void *handle, unsigned bool_function_index, unsigned node_index,
    unsigned event_type, unsigned event_id, void *event_data);

/* Term one-shot: unloads the module from the registry */
void cfl_se_module_load_term_one_shot_fn(void *handle, uint16_t node_index);

/* --------------------------------------------------------------------
 * ChainTree main/oneshot functions for tree load node
 *
 * One leaf node per tree instance. Init creates the tree from a loaded
 * module and stores the instance pointer in a blackboard uint64 slot.
 * Term terminates the tree, frees it, and clears the slot.
 * -------------------------------------------------------------------- */

/* Node data for the se_tree_load node (stored in heap arena) */
typedef struct {
    uint32_t                   module_hash;   /* which module (registry lookup) */
    uint32_t                   tree_hash;     /* which tree within the module   */
    uint16_t                   bb_offset;     /* blackboard uint64 slot offset  */
    s_expr_tree_instance_t    *inst;          /* live instance (set by init)    */
} cfl_se_tree_load_node_data_t;

/* Init one-shot: creates tree instance, stores pointer in blackboard */
void cfl_se_tree_load_init_one_shot_fn(void *handle, uint16_t node_index);

/* Main: null main */
unsigned cfl_se_tree_load_main_fn(
    void *handle, unsigned bool_function_index, unsigned node_index,
    unsigned event_type, unsigned event_id, void *event_data);

/* Term one-shot: terminates tree, frees instance, clears blackboard slot */
void cfl_se_tree_load_term_one_shot_fn(void *handle, uint16_t node_index);

/* --------------------------------------------------------------------
 * ChainTree main/oneshot functions for se_tick node
 *
 * One leaf node that ticks an s-engine tree each ChainTree tick.
 * Init retrieves the tree instance from a blackboard uint64 slot
 * and resets it. Main ticks the tree and processes its event queue.
 * CFL_TIMER_EVENT is mapped to SE_EVENT_TICK; all others pass through.
 * -------------------------------------------------------------------- */

/* Node data for the se_tick node (stored in heap arena) */
typedef struct {
    s_expr_tree_instance_t    *inst;      /* tree instance (from bb) */
    uint16_t                   bb_offset; /* blackboard offset of tree ptr */
} cfl_se_tick_node_data_t;

/* Init one-shot: retrieves tree from blackboard, resets it */
void cfl_se_tick_init_one_shot_fn(void *handle, uint16_t node_index);

/* Main: ticks the s-engine tree, processes event queue, maps return codes */
unsigned cfl_se_tick_main_fn(
    void *handle, unsigned bool_function_index, unsigned node_index,
    unsigned event_type, unsigned event_id, void *event_data);

/* Term one-shot: no-op (tree lifecycle owned by the se_tree_load node) */
void cfl_se_tick_term_one_shot_fn(void *handle, uint16_t node_index);

/* --------------------------------------------------------------------
 * ChainTree composite node for se_engine
 *
 * A composite node whose children are se_module_load and se_tree_load.
 * Init enables all children (their inits load module + create tree).
 * Main ticks the s-engine tree each ChainTree tick.
 * Term terminates all children (their terms free tree + unload module).
 * -------------------------------------------------------------------- */

/* Node data for the se_engine composite node (stored in heap arena) */
typedef struct {
    s_expr_tree_instance_t    *inst;          /* live tree instance */
    s_expr_loaded_module_t    *loaded_module; /* parsed binary (owns allocations) */
    uint32_t                   module_hash;   /* for registry unload */
    uint16_t                   bb_offset;     /* blackboard offset of tree ptr */
    bool                       loaded;        /* module loaded in registry */
} cfl_se_engine_node_data_t;

/* Init one-shot: loads module, creates tree, stores ptr in BB.
 * Children are NOT enabled by init — the s-engine tree controls them
 * via cfl_enable_child / cfl_disable_children using ct_node_id. */
void cfl_se_engine_init_one_shot_fn(void *handle, uint16_t node_index);

/* Main: ticks the s-engine tree, processes event queue, maps return codes */
unsigned cfl_se_engine_main_fn(
    void *handle, unsigned bool_function_index, unsigned node_index,
    unsigned event_type, unsigned event_id, void *event_data);

/* Term one-shot: terminates all children */
void cfl_se_engine_term_one_shot_fn(void *handle, uint16_t node_index);

#ifdef __cplusplus
}
#endif

#endif /* CFL_SE_MODULE_REGISTRY_H */
