/* ============= cfl_heap_arena_allocate.h ============= */
#ifndef CFL_HEAP_ARENA_ALLOCATE_H
#define CFL_HEAP_ARENA_ALLOCATE_H

#include <stdint.h>
#include <stdbool.h>
#include "cfl_heap.h"
/* cfl_heap.h provides: cfl_perm.h, cfl_global_definitions.h (ARENA_ALIGNMENT) */

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * TYPES
 * ======================================================================== */

typedef uint8_t cfl_heap_allocator_id_t;

/* Forward declaration of internal control block (defined in .c) */
typedef struct CflHeapArenaControl CflHeapArenaControl;

typedef struct CflHeapArenaStats {
    uint32_t active_count;
    uint32_t total_data_allocated;
    uint32_t total_data_used;
} CflHeapArenaStats;

/* ========================================================================
 * ARENA SYSTEM INSTANCE
 * ======================================================================== */

typedef struct CflHeapArenaSystem {
    cfl_heap_t *heap;
    uint16_t max_allocator_count;
    CflHeapArenaControl *control_blocks;              /* Pre-allocated control blocks      */
    CflHeapArenaControl **arenas;                     /* Arena lookup table                */
    uint8_t *node_allocator_ids;                      /* Node-to-allocator mapping         */
    uint16_t *node_memory_index;                      /* Node memory offset within arena   */
    uint16_t total_node_count;
    cfl_heap_allocator_id_t next_allocator_id;
    cfl_heap_allocator_id_t active_allocator_context;
    void *allocator_0_buffer;                         /* Permanent buffer from perm        */
} CflHeapArenaSystem, cfl_heap_arena_system_t;

/* ========================================================================
 * LIFECYCLE
 *
 * cfl_heap_arena_system_create pre-allocates all control blocks and creates
 * allocator 0 automatically from perm.  Allocator 0 is permanent and cannot
 * be destroyed.
 * ======================================================================== */

CflHeapArenaSystem *cfl_heap_arena_system_create(cfl_perm_t *perm, cfl_heap_t *heap,
                                                  uint16_t max_allocator_count,
                                                  uint16_t total_node_count,
                                                  uint16_t allocator_0_size);

void cfl_heap_arena_system_reset(CflHeapArenaSystem *sys);

/* ========================================================================
 * ARENA CREATE / DESTROY
 * ======================================================================== */

cfl_heap_allocator_id_t cfl_heap_arena_create(CflHeapArenaSystem *sys,
                                               uint16_t owner_node_id,
                                               uint16_t size_bytes);

void cfl_heap_arena_destroy(CflHeapArenaSystem *sys,
                            cfl_heap_allocator_id_t id,
                            uint16_t owner_node_id);

/* ========================================================================
 * ALLOCATOR CONTEXT MANAGEMENT
 * ======================================================================== */

void cfl_heap_arena_set_active_allocator(CflHeapArenaSystem *sys, uint16_t owner_node_id);
void cfl_heap_arena_set_active_allocator_id(CflHeapArenaSystem *sys, cfl_heap_allocator_id_t allocator_id);
void cfl_heap_arena_set_node_allocator(CflHeapArenaSystem *sys, uint16_t requesting_node_id);

/* ========================================================================
 * ALLOCATION — NODE-BASED (updates node_memory_index)
 * ======================================================================== */

void *cfl_arena_system_alloc(CflHeapArenaSystem *sys, uint16_t requesting_node_id, uint16_t size_bytes);
void *cfl_arena_system_alloc_aligned(CflHeapArenaSystem *sys, uint16_t requesting_node_id,
                                     uint16_t size_bytes, uint8_t alignment);

/* ========================================================================
 * ALLOCATION — ADDITIONAL (does NOT update node_memory_index)
 * ======================================================================== */

void *cfl_arena_additional_alloc(CflHeapArenaSystem *sys, uint16_t node_index, uint16_t size_bytes);
void *cfl_arena_additional_alloc_aligned(CflHeapArenaSystem *sys, uint16_t node_index,
                                         uint16_t size_bytes, uint8_t alignment);

/* ========================================================================
 * ALLOCATION — FROM ACTIVE CONTEXT (updates node_memory_index + assigns allocator)
 * ======================================================================== */

void *cfl_arena_alloc_from_active(CflHeapArenaSystem *sys, uint16_t node_index, uint16_t size_bytes);
void *cfl_arena_alloc_from_active_aligned(CflHeapArenaSystem *sys, uint16_t node_index,
                                           uint16_t size_bytes, uint8_t alignment);

/* ========================================================================
 * NODE ACCESSORS
 * ======================================================================== */

void *cfl_heap_arena_get_node_ptr(CflHeapArenaSystem *sys, uint16_t node_id);

cfl_heap_allocator_id_t cfl_heap_arena_get_node_allocator_id(CflHeapArenaSystem *sys, uint16_t node_id);
void cfl_heap_arena_set_node_allocator_id(CflHeapArenaSystem *sys, uint16_t node_id,
                                           cfl_heap_allocator_id_t allocator_id);

uint16_t cfl_heap_arena_get_node_memory_index(CflHeapArenaSystem *sys, uint16_t node_id);
void cfl_heap_arena_set_node_memory_index(CflHeapArenaSystem *sys, uint16_t node_id, uint16_t memory_idx);

/* ========================================================================
 * DIAGNOSTICS
 * ======================================================================== */

uint16_t cfl_heap_arena_used_bytes(CflHeapArenaSystem *sys, cfl_heap_allocator_id_t id);
uint16_t cfl_heap_arena_free_bytes(CflHeapArenaSystem *sys, cfl_heap_allocator_id_t id);
CflHeapArenaStats cfl_heap_arena_dump_stats(CflHeapArenaSystem *sys);

#ifdef __cplusplus
}
#endif

#endif /* CFL_HEAP_ARENA_ALLOCATE_H */