/* ============= REENTRANT cfl_heap.h ============= */
#ifndef CFL_HEAP_H
#define CFL_HEAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "cfl_perm.h"
/* cfl_perm.h provides cfl_global_definitions.h (BLOCK_ALIGNMENT, MIN_BLOCK_SIZE) */

#define INVALID_HEAP_IDX  0xFFFF
#define NODE_ID_NONE      0xFFFF

/* ========================================================================
 * HEAP STATISTICS
 * ======================================================================== */

typedef struct {
    uint16_t total_allocations;
    uint16_t total_frees;
    uint16_t current_blocks;
    uint16_t current_used_bytes;
    uint16_t peak_used_bytes;
    uint16_t largest_free_block;
    uint16_t free_blocks;
    uint16_t allocated_blocks;
} CflHeapStats;

/* ========================================================================
 * HEAP INSTANCE
 * ======================================================================== */

typedef struct CflHeap {
    uint8_t  *pool;
    uint16_t  pool_size;
    bool      initialized;
    bool      owns_pool;
    CflHeapStats stats;
} CflHeap, cfl_heap_t;

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

/**
 * @brief Initialize heap — allocates struct and pool from cfl_perm
 */
CflHeap *cfl_heap_init(cfl_perm_t *perm, uint16_t buffer_size);

/**
 * @brief Reset heap to initial state (all allocations lost)
 */
void cfl_heap_reset(CflHeap *heap);

/* ========================================================================
 * ALLOCATION / FREE — INDEX BASED
 * ======================================================================== */

/**
 * @brief Allocate memory block, returns index into pool
 */
uint16_t cfl_heap_malloc(CflHeap *heap, uint16_t size_bytes);

/**
 * @brief Free memory block by index
 */
void cfl_heap_free(CflHeap *heap, uint16_t idx);

/**
 * @brief Arena allocation with node tracking and custom alignment
 */
uint16_t cfl_heap_arena_alloc_aligned(CflHeap *heap, uint16_t requesting_node_id,
                                      uint16_t size_bytes, uint16_t alignment);

/* ========================================================================
 * ALLOCATION / FREE — POINTER BASED
 * ======================================================================== */

/**
 * @brief Allocate memory block, returns pointer
 */
void *cfl_heap_malloc_pointer(CflHeap *heap, uint16_t size_bytes);

/**
 * @brief Free memory block by pointer
 */
void cfl_heap_free_pointer(CflHeap *heap, void *ptr);

/* ========================================================================
 * INDEX / POINTER CONVERSION
 * ======================================================================== */

void *cfl_heap_ptr(CflHeap *heap, uint16_t idx);
uint16_t cfl_heap_ptr_to_idx(CflHeap *heap, void *ptr);

/* ========================================================================
 * DIAGNOSTICS
 * ======================================================================== */

uint16_t cfl_heap_used_bytes(CflHeap *heap);
uint16_t cfl_heap_free_bytes(CflHeap *heap);
void cfl_heap_get_stats(CflHeap *heap, CflHeapStats *stats);
void cfl_heap_dump_stats(CflHeap *heap);
bool cfl_heap_validate(CflHeap *heap);

/**
 * @brief Walk through all heap blocks, calling callback for each
 */
void cfl_heap_walk(CflHeap *heap,
    void (*callback)(void *block_ptr, uint16_t size, bool allocated, uint16_t node_id));

/**
 * @brief Get node ID that allocated a given block
 */
uint16_t cfl_heap_get_node_id(CflHeap *heap, uint16_t idx);

/* ========================================================================
 * STATIC ALLOCATION HELPERS
 *
 * CFL_HEAP_DEFINE_STATIC(name, size) declares aligned storage.
 * Call CFL_HEAP_INIT_STATIC(name) once at startup.
 * ======================================================================== */

#define CFL_HEAP_TOTAL_SIZE(buffer_size) \
    (sizeof(CflHeap) + (buffer_size))

#define CFL_HEAP_DEFINE_STATIC(name, size)                                  \
    static uint8_t name##_storage[CFL_HEAP_TOTAL_SIZE(size)]                \
        __attribute__((aligned(BLOCK_ALIGNMENT)));                          \
    static CflHeap *name = (CflHeap *)name##_storage

#define CFL_HEAP_INIT_STATIC(name)                                          \
    do {                                                                    \
        memset(name, 0, sizeof(CflHeap));                                   \
        name->pool = name##_storage + sizeof(CflHeap);                      \
        name->pool_size = (uint16_t)(sizeof(name##_storage) - sizeof(CflHeap)); \
        name->initialized = false;                                          \
        name->owns_pool = false;                                            \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* CFL_HEAP_H */